/* SPDX-License-Identifier: GPL-2.0-only
 *
 * cmpretrain — CMP 170HX PCIe Gen2 重训练辅助驱动(经典 WDM 服务模式)。
 *
 * 职责单一：接收用户态给出的 GPU/上游桥 BDF，经 CF8/CFC 做两端 PCI 配置读写、
 * 经 BAR0 做 GPU 私有寄存器读写，执行 retrain_core.h 中的状态机。
 * 不绑定任何 GPU、不参与 PnP 电源、不做 FLR、不写其他设备。
 *
 * 配置空间访问机制变迁：
 *  0.3 MCFG/ECAM —— 本机 ZwQuerySystemInformation(SystemFirmwareTableInformation)
 *      直接返回 STATUS_NOT_IMPLEMENTED(0xC0000002)，用户态 GetSystemFirmwareTable
 *      同样 ERROR_INVALID_FUNCTION(gle=1)；固件表接口被系统级禁用(Win11 26200)，
 *      ECAM 基址无从获得。
 *  0.4 改 CF8/CFC —— PCI 配置机制 #1，I/O 端口 0xCF8/0xCFC，不依赖任何固件表；
 *      覆盖 segment 0 全部 256 条总线，本包用到的寄存器都在 256B 窗口内
 *      (能力链表指针是 8 位，PCIe cap 必在 0x00-0xFF)。16 位写经 32 位 RMW，
 *      相邻半字均为 RO(LNKSTA/LNKSTA2)，回写无副作用。
 *      与 OS 的配置访问不做全局仲裁(RWEverything/CPU-Z 同款取舍)，
 *      本驱动内部用 FAST_MUTEX 串行化。
 *
 * 安装：SCM 服务 cmpretrain(retrainctl --install)，设备 \Device\CmpRetrain，
 * Win32 路径 \\.\CmpRetrain。
 */
#include <ntddk.h>
#include <wdmsec.h>
#include "retrainabi.h"

#define CMPRT_POOL_TAG 'TRpC'
#define CMPRT_BAR0_MAP_SIZE 0x100000u   /* 只用到 0x8C2C0，映射 1 MiB 足够 */
#define CMPRT_CFG_WINDOW    0x100u      /* CF8/CFC 机制 #1 窗口：256 B */

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD CmpUnload;
static DRIVER_DISPATCH CmpDispatchCreateClose;
static DRIVER_DISPATCH CmpDispatchDeviceControl;

/* ---------- CF8/CFC 配置访问 ---------- */

#define CMPRT_CFG_ADDR_PORT ((PULONG)(ULONG_PTR)0xCF8)
#define CMPRT_CFG_DATA_PORT ((PULONG)(ULONG_PTR)0xCFC)

static FAST_MUTEX g_CfgLock;

static ULONG CmpCfgReadDword(ULONG bus, ULONG dev, ULONG fn, ULONG off)
{
    ULONG addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFCu);
    ULONG v;
    ExAcquireFastMutex(&g_CfgLock);
    WRITE_PORT_ULONG(CMPRT_CFG_ADDR_PORT, addr);
    v = READ_PORT_ULONG(CMPRT_CFG_DATA_PORT);
    /* 地址端口复位，避免遗留指向后 OS 自己的访问串位 */
    WRITE_PORT_ULONG(CMPRT_CFG_ADDR_PORT, 0);
    ExReleaseFastMutex(&g_CfgLock);
    return v;
}

static void CmpCfgWriteDword(ULONG bus, ULONG dev, ULONG fn, ULONG off, ULONG val)
{
    ULONG addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFCu);
    ExAcquireFastMutex(&g_CfgLock);
    WRITE_PORT_ULONG(CMPRT_CFG_ADDR_PORT, addr);
    WRITE_PORT_ULONG(CMPRT_CFG_DATA_PORT, val);
    WRITE_PORT_ULONG(CMPRT_CFG_ADDR_PORT, 0);
    ExReleaseFastMutex(&g_CfgLock);
}

/* ---------- 内核 IO 上下文：实现 cmprt_io ---------- */

typedef struct {
    volatile UCHAR *bar0;
    cmprt_u32 gpuBdf;
    cmprt_u32 rootBdf;
} CMPRT_KCTX;

static void CmpBdfSplit(cmprt_u32 bdf, ULONG *bus, ULONG *dev, ULONG *fn)
{
    *bus = (bdf >> 8) & 0xFF;
    *dev = (bdf >> 3) & 0x1F;
    *fn  = bdf & 0x7;
}

static cmprt_u32 CmpCfgRead(void *vctx, cmprt_u32 bdf, cmprt_u32 off, cmprt_u8 size)
{
    ULONG bus, dev, fn, v;
    UNREFERENCED_PARAMETER(vctx);
    if (off + size > CMPRT_CFG_WINDOW) return 0xFFFFFFFFu;
    CmpBdfSplit(bdf, &bus, &dev, &fn);
    v = CmpCfgReadDword(bus, dev, fn, off);
    v >>= (off & 3) * 8;
    if (size == 1) return v & 0xFFu;
    if (size == 2) return v & 0xFFFFu;
    return v;
}

static void CmpCfgWrite(void *vctx, cmprt_u32 bdf, cmprt_u32 off, cmprt_u32 val, cmprt_u8 size)
{
    ULONG bus, dev, fn, v, shift, mask;
    UNREFERENCED_PARAMETER(vctx);
    if (off + size > CMPRT_CFG_WINDOW) return;
    CmpBdfSplit(bdf, &bus, &dev, &fn);
    if (size == 4 && (off & 3) == 0) {
        CmpCfgWriteDword(bus, dev, fn, off, val);
        return;
    }
    /* 16/8 位写：读-改-写所在双字。本包目标寄存器的相邻半字均为
     * 只读状态位(LNKSTA/LNKSTA2/VEN-ID 等)，回写原值无副作用。 */
    v = CmpCfgReadDword(bus, dev, fn, off);
    shift = (off & 3) * 8;
    mask = (size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu)) << shift;
    v = (v & ~mask) | ((val << shift) & mask);
    CmpCfgWriteDword(bus, dev, fn, off, v);
}

static cmprt_u32 CmpMmioRead(void *vctx, cmprt_u32 off)
{
    CMPRT_KCTX *k = (CMPRT_KCTX*)vctx;
    if (off + 4 > CMPRT_BAR0_MAP_SIZE) return 0xFFFFFFFFu;
    return READ_REGISTER_ULONG((PULONG)(k->bar0 + off));
}

static void CmpMmioWrite(void *vctx, cmprt_u32 off, cmprt_u32 val)
{
    CMPRT_KCTX *k = (CMPRT_KCTX*)vctx;
    if (off + 4 > CMPRT_BAR0_MAP_SIZE) return;
    WRITE_REGISTER_ULONG((PULONG)(k->bar0 + off), (ULONG)val);
}

static void CmpSleepMs(void *vctx, cmprt_u32 ms)
{
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(vctx);
    interval.QuadPart = -(LONGLONG)ms * 10 * 1000;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

/* ---------- 设备对象与派遣 ---------- */

static const GUID CmpRtGuid = CMPRT_INTERFACE_GUID_INIT;  /* 仅作设备类 GUID */

static PDEVICE_OBJECT g_DeviceObject;
static BOOLEAN        g_SymLinkCreated;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS st;
    UNICODE_STRING devName, symName;

    UNREFERENCED_PARAMETER(RegistryPath);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "cmpretrain: DriverEntry (CF8/CFC backend)\n");

    ExInitializeFastMutex(&g_CfgLock);

    RtlInitUnicodeString(&devName, CMPRT_NT_DEVICE_NAME);
    st = IoCreateDeviceSecure(DriverObject, 0, &devName, CMPRT_DEVICE_TYPE,
                              0, FALSE,
                              &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,  /* 仅 SYSTEM/管理员 */
                              &CmpRtGuid, &g_DeviceObject);
    if (!NT_SUCCESS(st)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "cmpretrain: IoCreateDeviceSecure failed st=0x%x\n", st);
        return st;
    }
    RtlInitUnicodeString(&symName, CMPRT_SYMLINK_NAME);
    st = IoCreateSymbolicLink(&symName, &devName);
    if (!NT_SUCCESS(st)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "cmpretrain: IoCreateSymbolicLink failed st=0x%x\n", st);
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return st;
    }
    g_SymLinkCreated = TRUE;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = CmpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = CmpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CmpDispatchDeviceControl;
    DriverObject->DriverUnload = CmpUnload;

    g_DeviceObject->Flags |= DO_BUFFERED_IO;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID CmpUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symName;
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "cmpretrain: Unload\n");
    if (g_SymLinkCreated) {
        RtlInitUnicodeString(&symName, CMPRT_SYMLINK_NAME);
        IoDeleteSymbolicLink(&symName);
        g_SymLinkCreated = FALSE;
    }
    if (g_DeviceObject) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }
}

static NTSTATUS CmpDispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS CmpDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;
    CMPRT_REQUEST *in;
    CMPRT_RESULT out;
    CMPRT_KCTX k;
    cmprt_io io;
    PHYSICAL_ADDRESS pa;
    ULONGLONG bar0;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (sp->Parameters.DeviceIoControl.IoControlCode != IOCTL_CMPRT_RUN) {
        st = STATUS_INVALID_DEVICE_REQUEST;
        goto done;
    }
    if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(CMPRT_REQUEST) ||
        sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(CMPRT_RESULT)) {
        st = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }
    /* METHOD_BUFFERED：输入/输出共用 SystemBuffer */
    in = (CMPRT_REQUEST*)Irp->AssociatedIrp.SystemBuffer;

    RtlZeroMemory(&out, sizeof(out));
    RtlZeroMemory(&k, sizeof(k));

    if (in->GpuBus > 255 || in->GpuDevice > 31 || in->GpuFunction > 7 ||
        in->RootBus > 255 || in->RootDevice > 31 || in->RootFunction > 7 ||
        in->Segment != 0) {
        out.code = CMPRT_ERR_BAD_INPUT;
        cmprt_strcpy(out.message, "BDF out of range or segment unsupported",
                     sizeof(out.message));
        goto reply;
    }

    k.gpuBdf = cmprt_bdf(in->GpuBus, in->GpuDevice, in->GpuFunction);
    k.rootBdf = cmprt_bdf(in->RootBus, in->RootDevice, in->RootFunction);

    /* GPU 身份预检 + BAR0 映射(CF8/CFC 配置读) */
    if (CmpCfgRead(&k, k.gpuBdf, CMPRT_CFG_ID, 4) == 0xFFFFFFFFu ||
        (CmpCfgRead(&k, k.gpuBdf, CMPRT_CFG_BAR0_LO, 4) & 1u)) {
        out.code = CMPRT_ERR_GPU_ID;
        cmprt_strcpy(out.message, "GPU config space unreadable / BAR0 is I/O",
                     sizeof(out.message));
        st = STATUS_SUCCESS;
        goto reply;
    }
    bar0 = (ULONGLONG)(CmpCfgRead(&k, k.gpuBdf, CMPRT_CFG_BAR0_LO, 4) & 0xFFFFFFF0u);
    if ((CmpCfgRead(&k, k.gpuBdf, CMPRT_CFG_BAR0_LO, 4) & 0x6u) == 0x4u) {
        bar0 |= ((ULONGLONG)CmpCfgRead(&k, k.gpuBdf, CMPRT_CFG_BAR0_HI, 4)) << 32;
    }
    if (bar0 == 0) {
        out.code = CMPRT_ERR_BAR0;
        cmprt_strcpy(out.message, "BAR0 is zero", sizeof(out.message));
        st = STATUS_SUCCESS;
        goto reply;
    }
    pa.QuadPart = (LONGLONG)bar0;
    k.bar0 = (volatile UCHAR*)MmMapIoSpace(pa, CMPRT_BAR0_MAP_SIZE, MmNonCached);
    if (!k.bar0) {
        out.code = CMPRT_ERR_MAP;
        cmprt_strcpy(out.message, "BAR0 mapping failed", sizeof(out.message));
        st = STATUS_SUCCESS;
        goto reply;
    }

    io.ctx = &k;
    io.cfg_read = CmpCfgRead;
    io.cfg_write = CmpCfgWrite;
    io.mmio_read = CmpMmioRead;
    io.mmio_write = CmpMmioWrite;
    io.sleep_ms = CmpSleepMs;

    cmprt_run(&io, k.gpuBdf, k.rootBdf,
              (in->Flags & CMPRT_FLAG_APPLY) ? 1 : 0, &out);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "cmpretrain: run code=%u stage=%u gpu=%02x:%02x.%u root=%02x:%02x.%u "
               "lnksta %04x/%04x -> %04x/%04x polls=%u\n",
               out.code, out.stage,
               in->GpuBus, in->GpuDevice, in->GpuFunction,
               in->RootBus, in->RootDevice, in->RootFunction,
               out.gpu_lnksta_before, out.root_lnksta_before,
               out.gpu_lnksta_after, out.root_lnksta_after, out.poll_count);

    if (k.bar0) MmUnmapIoSpace((PVOID)k.bar0, CMPRT_BAR0_MAP_SIZE);
    st = STATUS_SUCCESS;

reply:
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &out, sizeof(out));
    info = sizeof(out);

done:
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}
