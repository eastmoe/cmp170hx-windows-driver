/* SPDX-License-Identifier: GPL-2.0-only
 * Experimental GA100 PCI function driver. Requires exclusive PnP ownership.
 * Firmware algorithm adapted from ../cmp170hx-windows-efi/src/main.c.
 */
#include <ntddk.h>
#pragma warning(push)
#pragma warning(disable:4324) /* WDK aligned request union */
#include <wdf.h>
#pragma warning(pop)
#include <initguid.h>
#include <wdmguid.h>
#include "protocol.h"
#include "core.h"
#include "firmware.h"
#include "memory_payload.h"
#ifndef CMP_HOST_TEST
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#pragma comment(lib,"ntstrsafe.lib")
/* Best-effort append at PASSIVE_LEVEL, independent of the user-mode interface.
 * A failed log write never changes the hardware operation's result. */
static VOID trace(const char *event,ULONG stage,NTSTATUS status,ULONG value) {
    UNICODE_STRING path;OBJECT_ATTRIBUTES a;IO_STATUS_BLOCK io;HANDLE file;
    LARGE_INTEGER now;char line[256];size_t bytes;
    KeQuerySystemTime(&now);
    if(!NT_SUCCESS(RtlStringCbPrintfA(line,sizeof(line),
        "%I64d event=%s stage=%lu status=0x%08lX value=0x%08lX\r\n",
        now.QuadPart,event,stage,(ULONG)status,value)))return;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,DPFLTR_INFO_LEVEL,"cmp170: %s",line);
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return;
    RtlInitUnicodeString(&path,L"\\SystemRoot\\Temp\\cmp170-kernel.log");
    InitializeObjectAttributes(&a,&path,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    if(!NT_SUCCESS(ZwCreateFile(&file,FILE_APPEND_DATA|SYNCHRONIZE,&a,&io,NULL,
        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT|FILE_WRITE_THROUGH|FILE_NON_DIRECTORY_FILE|FILE_OPEN_REPARSE_POINT,NULL,0)))return;
    if(NT_SUCCESS(RtlStringCbLengthA(line,sizeof(line),&bytes))) {
        ZwWriteFile(file,NULL,NULL,NULL,&io,line,(ULONG)bytes,NULL,NULL);
    }
    ZwClose(file);
}
#else
#define trace(event,stage,status,value) ((void)(event),(void)(stage),(void)(status),(void)(value))
#endif

typedef struct {
    PUCHAR bar; SIZE_T length;
    BUS_INTERFACE_STANDARD bus;
    DEVICE_RESET_INTERFACE_STANDARD resetInterface;
    BOOLEAN mmioClosed;
    const Profile *profile;
    PDMA_ADAPTER adapter; PVOID buffer; PHYSICAL_ADDRESS logical; ULONG bytes;
    BOOLEAN ioFailed,mutated,quiescent,attempted,removed;
    CMP_OUTPUT report;
} Context;
static BOOLEAN memoryStop(Context *c);
static BOOLEAN memoryTargets(Context *c);
static BOOLEAN memoryState(Context *c);
#ifdef CMP_HOST_TEST
#include "../tests/host.h"
#else
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Context,Ctx)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
EVT_WDF_DEVICE_D0_ENTRY Enter;
EVT_WDF_DEVICE_D0_EXIT Leave;
EVT_WDF_DEVICE_SURPRISE_REMOVAL Surprise;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;
static const GUID interfaceId=CMP_GUID_VALUE;
#endif
static const ULONG registers[CMP_REG_COUNT]={0,0x100000,REG_FEAT,REG_SS0,REG_SS1,
    REG_CFG1,REG_LMR,REG_WPR_LO,REG_WPR_HI,0x840100,0x840094,0x110100,
    0x1180f8,0x1fa7cc,0x9a0148,0x1fa7c4,0x111240,0x8403c4,0x1180f0};
static const ULONG cleanupRegs[3]={REG_WPR_LO,REG_WPR_HI,0x1180f8};
/* NVIDIA kgspIsWpr2Up uses HI.VAL[31:4], not an exact disabled LO value. */
static const ULONG cleanupMask[3]={0,0xfffffff0u,0xffffffffu};

static ULONG readReg(Context *c,ULONG r) {
    ULONG v;
    if(c->mmioClosed || c->removed || c->ioFailed || !c->bar || r>c->length-4 || (r&3)) {c->ioFailed=TRUE;return ~0u;}
    v=READ_REGISTER_ULONG((PULONG)(c->bar+r));
    /* FEAT PLM legitimately reads all ones when access is enabled. */
    if((v&0xffff0000)==0xbadf0000) c->ioFailed=TRUE;
    return v;
}
static VOID writeReg(Context *c,ULONG r,ULONG v) {
    if(c->mmioClosed || c->removed || c->ioFailed || !c->bar || r>c->length-4 || (r&3)) {c->ioFailed=TRUE;return;}
    c->mutated=TRUE;c->quiescent=FALSE;
    WRITE_REGISTER_ULONG((PULONG)(c->bar+r),v);
}
static VOID pauseMs(VOID) { LARGE_INTEGER t;t.QuadPart=-10000;KeDelayExecutionThread(KernelMode,FALSE,&t); }
static BOOLEAN poll(Context *c,ULONG r,ULONG mask,ULONG want) {
    ULONG i,v;
    for(i=0;i<3000;i++) {v=readReg(c,r);if(c->ioFailed || v==~0u)return FALSE;
        if((v&mask)==want)return TRUE;pauseMs();}
    trace("poll.timeout.register",c->report.stage,STATUS_IO_TIMEOUT,r);
    trace("poll.timeout.value",c->report.stage,STATUS_IO_TIMEOUT,v);
    return FALSE;
}
static BOOLEAN reset(Context *c,ULONG base) {
    ULONG i;
    writeReg(c,base+0x3c0,1);for(i=0;i<16;i++)readReg(c,base+0x3c0);
    writeReg(c,base+0x3c0,0);for(i=0;i<16;i++)readReg(c,base+0x3c0);
    if(!poll(c,base+0x10c,6,0))return FALSE;
    writeReg(c,base+0x84,readReg(c,0));return !c->ioFailed;
}
static BOOLEAN stopEngines(Context *c) {
    if(c->report.mode==CMP_MEMORY)return memoryStop(c);
    BOOLEAN a=reset(c,0x840000),b=reset(c,0x110000);
    /* DMACTL bits 1/2 only describe memory scrubbing. Require stopped CPU
     * and DMATRFCMD.IDLE as well before recycling the common buffer. */
    if(a)a=poll(c,0x840100,16,16)&&poll(c,0x840118,2,2);
    if(b)b=poll(c,0x110100,16,16)&&poll(c,0x110118,2,2)&&poll(c,0x111240,1,0);
    c->report.stop_final[0]=readReg(c,0x84010c);
    c->report.stop_final[1]=readReg(c,0x11010c);
    c->report.stop_cpu[0]=readReg(c,0x840100);c->report.stop_cpu[1]=readReg(c,0x110100);
    c->report.stop_dma[0]=readReg(c,0x840118);c->report.stop_dma[1]=readReg(c,0x110118);
    c->report.stop_gsp_riscv=readReg(c,0x111240);
    a=a&&c->report.stop_final[0]!=~0u&&!(c->report.stop_final[0]&6);
    b=b&&c->report.stop_final[1]!=~0u&&!(c->report.stop_final[1]&6);
    a=a&&c->report.stop_cpu[0]!=~0u&&(c->report.stop_cpu[0]&16)&&c->report.stop_dma[0]!=~0u&&(c->report.stop_dma[0]&2);
    b=b&&c->report.stop_cpu[1]!=~0u&&(c->report.stop_cpu[1]&16)&&c->report.stop_dma[1]!=~0u&&(c->report.stop_dma[1]&2);
    b=b&&c->report.stop_gsp_riscv!=~0u&&!(c->report.stop_gsp_riscv&1);
    c->report.checks&=~(CMP_SEC2_STOPPED|CMP_GSP_STOPPED);
    if(a&&!c->ioFailed)c->report.checks|=CMP_SEC2_STOPPED;
    if(b&&!c->ioFailed)c->report.checks|=CMP_GSP_STOPPED;
    trace("stop.SEC2",c->report.stage,a&&!c->ioFailed?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.stop_final[0]);
    trace("stop.GSP",c->report.stage,b&&!c->ioFailed?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.stop_final[1]);
    trace("stop.SEC2.CPUCTL",c->report.stage,a?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.stop_cpu[0]);
    trace("stop.SEC2.DMATRFCMD",c->report.stage,a?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.stop_dma[0]);
    trace("stop.GSP.CPUCTL",c->report.stage,b?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.stop_cpu[1]);
    trace("stop.GSP.DMATRFCMD",c->report.stage,b?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.stop_dma[1]);
    trace("stop.GSP.RISCV",c->report.stage,b?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.stop_gsp_riscv);
    c->quiescent=a&&b&&!c->ioFailed;return c->quiescent;
}
static VOID snapshot(Context *c,ULONG *out) {
    ULONG i;NTSTATUS s;
    /* Capture before synchronous file I/O can spread reads over milliseconds. */
    for(i=0;i<CMP_REG_COUNT;i++) {
        out[i]=readReg(c,registers[i]);
        /* All ones is legitimate for privilege masks, not the other sampled registers. */
        if(i!=2&&(i<13||i==16)&&out[i]==~0u)c->ioFailed=TRUE;
        if(i==0&&!out[i])c->ioFailed=TRUE;
    }
    s=c->ioFailed?STATUS_DEVICE_HARDWARE_ERROR:STATUS_SUCCESS;
    for(i=0;i<CMP_REG_COUNT;i++) {
        trace("snapshot.register",c->report.stage,s,registers[i]);
        trace("snapshot.value",c->report.stage,s,out[i]);
    }
}
static VOID freeDma(Context *c) {
    if(c->buffer) {
        if(c->mutated&&(!c->quiescent||c->ioFailed||c->removed)) {
            /* Intentionally retain allocation AND adapter until shutdown. Never
             * recycle DMA memory when engine stop could not be confirmed.
             * These are HAL-owned allocations, not WDF-child allocations. */
            c->report.checks|=CMP_DMA_RETAINED;
            trace("dma.retained-until-shutdown",c->report.stage,STATUS_DEVICE_HARDWARE_ERROR,c->bytes);
            c->buffer=NULL;c->adapter=NULL;return;
        }
        c->adapter->DmaOperations->FreeCommonBuffer(c->adapter,c->bytes,c->logical,c->buffer,FALSE);
        c->report.checks|=CMP_DMA_RELEASED;
        trace("dma.released",c->report.stage,STATUS_SUCCESS,c->bytes);
        c->buffer=NULL;
    }
    if(c->adapter){c->adapter->DmaOperations->PutDmaAdapter(c->adapter);c->adapter=NULL;}
}
static BOOLEAN cleanup(Context *c) {
    ULONG i;BOOLEAN stopped=stopEngines(c);
    for(i=0;i<3;i++)c->report.cleanup_before[i]=readReg(c,cleanupRegs[i]);
    c->report.cleanup_attempted=stopped&&!c->ioFailed;
    if(c->report.cleanup_attempted) {
        /* PLMs opened before FEAT now permit the upstream host WPR restore.
         * No Booter Unload: it returned 0x29 in the 0.4 hardware run. */
        writeReg(c,REG_WPR_LO,c->report.before[7]);
        writeReg(c,REG_WPR_HI,c->report.before[8]);
        writeReg(c,0x1180f8,0);
    }
    /* Capture immediate readback before file I/O or releasing DMA. */
    for(i=0;i<3;i++)c->report.cleanup_after[i]=readReg(c,cleanupRegs[i]);
    c->report.cleanup_mismatch=0;
    for(i=0;i<3;i++) {
        if(c->report.cleanup_after[i]&cleanupMask[i])c->report.cleanup_mismatch|=1u<<i;
    }
    /* Configuration mismatch and DMA quiescence are different predicates. */
    c->quiescent=stopped&&!c->ioFailed;
    if(c->quiescent&&c->report.cleanup_attempted&&!c->report.cleanup_mismatch)
        c->report.checks|=CMP_CLEANUP_VERIFIED;
    for(i=0;i<3;i++) {
        NTSTATUS s=c->ioFailed||(c->report.cleanup_mismatch&(1u<<i))?STATUS_DEVICE_HARDWARE_ERROR:STATUS_SUCCESS;
        trace("cleanup.register",c->report.stage,s,cleanupRegs[i]);
        trace("cleanup.before",c->report.stage,s,c->report.cleanup_before[i]);
        trace("cleanup.zero-mask",c->report.stage,s,cleanupMask[i]);
        trace("cleanup.actual",c->report.stage,s,c->report.cleanup_after[i]);
    }
    trace("cleanup.result",c->report.stage,(c->report.checks&CMP_CLEANUP_VERIFIED)?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.cleanup_mismatch);
    return (c->report.checks&CMP_CLEANUP_VERIFIED)!=0;
}
static VOID diagnose(Context *c) {
    BOOLEAN valid;
    if(c->mmioClosed)return;
    trace("Diagnostic.begin",c->report.stage,STATUS_SUCCESS,c->report.state);
    snapshot(c,c->report.after);
    valid=!c->ioFailed&&c->report.after[0]&&c->report.after[0]!=~0u;
    if(c->attempted) {
        valid=valid&&!(c->report.after[8]>>4)&&!(c->report.after[12]&(c->report.mode==CMP_MEMORY?0xfff00000u:~0u));
        valid=valid&&c->report.after[2]==~0u&&c->report.after[3]==0x88888888&&c->report.after[4]==8;
        if(c->report.mode==CMP_MEMORY)
            valid=valid&&c->report.after[5]==c->profile->cfg1&&c->report.after[6]==c->profile->lmr&&memoryTargets(c)&&memoryState(c);
    }
    c->report.diagnostic_status=valid?STATUS_SUCCESS:(ULONG)STATUS_DEVICE_HARDWARE_ERROR;
    if(!valid) {
        c->report.status=(ULONG)STATUS_DEVICE_HARDWARE_ERROR;
        if(c->attempted)c->report.state=2;
    }
    trace("Diagnostic.result",c->report.stage,(NTSTATUS)c->report.diagnostic_status,c->report.state);
}
static NTSTATUS allocateDma(WDFDEVICE dev,Context *c) {
    DEVICE_DESCRIPTION d;ULONG maps;
    RtlZeroMemory(&d,sizeof(d));d.Version=DEVICE_DESCRIPTION_VERSION;
    d.Master=TRUE;d.ScatterGather=TRUE;d.Dma64BitAddresses=TRUE;
    d.InterfaceType=PCIBus;d.MaximumLength=c->bytes;
    c->adapter=IoGetDmaAdapter(WdfDeviceWdmGetPhysicalDevice(dev),&d,&maps);
    if(!c->adapter)return STATUS_NOT_SUPPORTED;
    c->buffer=c->adapter->DmaOperations->AllocateCommonBuffer(c->adapter,c->bytes,&c->logical,FALSE);
    if(!c->buffer){freeDma(c);return STATUS_INSUFFICIENT_RESOURCES;}
    if((c->logical.QuadPart&4095) || c->logical.QuadPart<0 ||
       (ULONGLONG)c->logical.QuadPart+c->bytes>(1ULL<<40)) {freeDma(c);return STATUS_NOT_SUPPORTED;}
    RtlZeroMemory(c->buffer,c->bytes);return STATUS_SUCCESS;
}
static BOOLEAN fire(Context *c,ULONG addr,ULONG value) {
    ULONG i,cpu;PUCHAR p=c->buffer;
    if(c->report.mode!=CMP_MEMORY)fill_signature(p+4096,addr,value);
    KeMemoryBarrier();
    cpu=readReg(c,0x840100);if(cpu==~0u || c->ioFailed)return FALSE;
    writeReg(c,0x840624,readReg(c,0x840624)|(1u<<7));writeReg(c,0x84010c,0);
    writeReg(c,0x840600,(readReg(c,0x840600)&~7u)|5u);
    writeReg(c,0x840180,1u<<24);
    for(i=0;i<0x100;i+=4){if(!(i&255))writeReg(c,0x840188,i>>8);writeReg(c,0x840184,*(const ULONG*)(ga100_booter+i));}
    writeReg(c,0x840180,0x100|(1u<<24)|(1u<<28));
    for(i=0x100;i<0x8700;i+=4){if(!(i&255))writeReg(c,0x840188,i>>8);writeReg(c,0x840184,*(const ULONG*)(ga100_booter+i));}
    writeReg(c,0x8401c0,1u<<24);
    for(i=0;i<0x6400;i+=4)writeReg(c,0x8401c4,*(const ULONG*)(ga100_booter+0x8700+i));
    writeReg(c,0x840104,0);writeReg(c,0x840040,c->logical.LowPart);writeReg(c,0x840044,(ULONG)c->logical.HighPart);
    writeReg(c,(cpu&(1u<<6))?0x840130:0x840100,2);
    for(i=0;i<3000;i++) {
        if(c->ioFailed)return FALSE;
        if(readReg(c,addr)==value) {
            trace("fire.target-matched",c->report.stage,STATUS_SUCCESS,i);return !c->ioFailed;
        }
        cpu=readReg(c,0x840100);if(cpu==~0u || (cpu&16))break;pauseMs();
    }
    trace("fire.exit-cpu",c->report.stage,STATUS_DEVICE_HARDWARE_ERROR,cpu);
    return !c->ioFailed&&readReg(c,addr)==value;
}
static BOOLEAN openPlm(Context *c,ULONG addr,ULONG value,ULONG lo,ULONG hi) {
    CMP_PLM_OUTPUT *p=&c->report.plm[c->report.plm_count++];BOOLEAN ok=FALSE;
    p->reg=addr;p->wanted=value;p->before=readReg(c,addr);
    trace("plm.register",c->report.stage,STATUS_SUCCESS,addr);
    if(p->before==value&&!c->ioFailed) {p->outcome=1;ok=TRUE;goto done;}
    /* fire() can observe the target before the previous payload halts.
     * Stop SEC2 BEFORE restoring WPR or rewriting the shared signature. */
    if(!reset(c,0x840000)||!poll(c,0x840100,16,16)||!poll(c,0x840118,2,2))goto done;
    writeReg(c,REG_WPR_LO,lo);writeReg(c,REG_WPR_HI,hi);
    p->wpr_lo=readReg(c,REG_WPR_LO);p->wpr_hi=readReg(c,REG_WPR_HI);
    trace("plm.refire.WPR_LO",c->report.stage,STATUS_SUCCESS,p->wpr_lo);
    trace("plm.refire.WPR_HI",c->report.stage,STATUS_SUCCESS,p->wpr_hi);
    if(c->ioFailed||p->wpr_lo!=lo||p->wpr_hi!=hi)goto done;
    ok=fire(c,addr,value);p->outcome=ok?2u:3u;
done:
    p->after=readReg(c,addr);p->cpu=readReg(c,0x840100);
    p->mailbox0=readReg(c,0x840040);p->mailbox1=readReg(c,0x840044);
    ok=ok&&!c->ioFailed;
    if(!ok)p->outcome=3;
    trace("plm.after",c->report.stage,ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,p->after);
    trace("plm.mailbox0",c->report.stage,ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,p->mailbox0);
    return ok;
}
#include "memory_engine.h"
static NTSTATUS run(WDFDEVICE dev,Context *c,ULONG mode) {
    ULONG i,npages=(GA100_FW_SIZE+4095)/4096,leaves=(npages+511)/512;
    ULONG lo,hi;USHORT command;ULONGLONG top,base,*root,*middle,*leaf;PUCHAR p;
    BOOLEAN ok=FALSE;NTSTATUS s;
    if(c->attempted||c->mmioClosed)return STATUS_INVALID_DEVICE_STATE;
    c->report.mode=mode;c->report.stage=1;
    trace("run.begin",1,STATUS_SUCCESS,mode);
    snapshot(c,c->report.before);
    for(i=0;i<CMP_REG_COUNT;i++)if(i!=2&&(i<13||i==16)&&c->report.before[i]==~0u)c->ioFailed=TRUE;
    if(c->ioFailed)return STATUS_DEVICE_HARDWARE_ERROR;
    /* Memory bring-up is validated against the user's cold 8 GiB board.
     * Reject unknown/previously expanded geometry BEFORE allocating DMA or
     * launching SEC2. 10 GiB cold geometry has not been captured on Windows. */
    if(mode==CMP_MEMORY && (c->profile->device!=0x20c2 ||
       c->report.before[5]!=0x02449000 || c->report.before[6]!=0x208 ||
       c->report.before[2]!=0xffffff8f || c->report.before[7]!=0x1ffffe00 ||
       c->report.before[8]!=0)) {
        trace("memory.baseline-rejected",1,STATUS_NOT_SUPPORTED,c->report.before[5]);
        return STATUS_NOT_SUPPORTED;
    }
    if(c->bus.GetBusData(c->bus.Context,PCI_WHICHSPACE_CONFIG,&command,4,2)!=2 || (command&6)!=6)
        return STATUS_DEVICE_NOT_READY;
    if(mode==CMP_MEMORY)return runMemory(dev,c);
    lo=c->report.before[7];hi=c->report.before[8];
    c->bytes=(20+leaves+npages)*4096;s=allocateDma(dev,c);
    trace("allocateDma",c->report.stage,s,c->bytes);if(!NT_SUCCESS(s))return s;
    trace("dma.address-low",c->report.stage,s,c->logical.LowPart);
    trace("dma.address-high",c->report.stage,s,(ULONG)c->logical.HighPart);
    p=c->buffer;base=(ULONGLONG)c->logical.QuadPart;
    root=(void*)(p+18*4096);middle=(void*)(p+19*4096);leaf=(void*)(p+20*4096);
    root[0]=base+19*4096;
    for(i=0;i<leaves;i++)middle[i]=base+(20+i)*4096;
    for(i=0;i<npages;i++)leaf[i]=base+(20+leaves+i)*4096;
    RtlCopyMemory(p+17*4096,ga100_bl,GA100_BL_SIZE);
    RtlCopyMemory(p+(20+leaves)*4096,ga100_fw,GA100_FW_SIZE);
    top=c->profile->stock_bytes-MB;
    if(lo<=hi&&((ULONGLONG)lo<<8)<top)top=(ULONGLONG)lo<<8;
    if(!build_meta(p,c->profile->stock_bytes,top,base+18*4096,GA100_FW_SIZE,base+17*4096,base+4096)) {
        freeDma(c);return STATUS_NOT_SUPPORTED;
    }
    c->attempted=TRUE;c->report.stage=2;
    trace("run.dma-ready",2,STATUS_SUCCESS,c->bytes);
    if(!reset(c,0x110000))goto done;
    {
        const RegisterWrite *plms=mode==CMP_MEMORY?memory_plms:compute_plms;
        ULONG count=mode==CMP_MEMORY?RTL_NUMBER_OF(memory_plms):RTL_NUMBER_OF(compute_plms);
        for(i=0;i<count;i++) {
            c->report.stage=10+i;
            trace("run.plm",c->report.stage,STATUS_SUCCESS,plms[i].reg);
            if(!openPlm(c,plms[i].reg,plms[i].value,lo,hi))goto done;
        }
    }
    c->report.stage=20;
    trace("run.write-settings",20,STATUS_SUCCESS,mode);
    if(!reset(c,0x840000))goto done;
    writeReg(c,REG_SS0,0x88888888);writeReg(c,REG_SS1,8);
    if(mode==CMP_MEMORY){
        /* Confirm FBPA access survived the last SEC2 stop, then verify CFG1
         * before changing LMR. Never advance past a partial geometry write. */
        if(readReg(c,0x9a0148)!=~0u || c->ioFailed)goto done;
        writeReg(c,REG_CFG1,c->profile->cfg1);
        if(readReg(c,REG_CFG1)!=c->profile->cfg1 || c->ioFailed)goto done;
        writeReg(c,REG_LMR,c->profile->lmr);
        if(readReg(c,REG_LMR)!=c->profile->lmr || c->ioFailed)goto done;
        trace("memory.geometry-written",20,STATUS_SUCCESS,c->profile->cfg1);
    }
    snapshot(c,c->report.after);
    ok=!c->ioFailed&&c->report.after[2]==~0u&&c->report.after[3]==0x88888888&&c->report.after[4]==8;
    if(mode==CMP_MEMORY)ok=ok&&c->report.after[5]==c->profile->cfg1&&c->report.after[6]==c->profile->lmr;
done:
    if(!ok)snapshot(c,c->report.after);
    if(ok)c->report.checks|=CMP_TARGET_MATCHED;
    c->report.stage=21;
    if(!cleanup(c))ok=FALSE;
    snapshot(c,c->report.after);
    if((c->report.after[8]>>4)||c->report.after[12]!=0)ok=FALSE;
    if(c->report.after[2]!=~0u||c->report.after[3]!=0x88888888||c->report.after[4]!=8)ok=FALSE;
    if(mode==CMP_MEMORY&&(c->report.after[5]!=c->profile->cfg1||c->report.after[6]!=c->profile->lmr))ok=FALSE;
    freeDma(c);
    if(c->ioFailed)ok=FALSE;
    c->report.state=ok?1u:2u;
    if(ok)c->report.stage=30;
    trace("run.end",c->report.stage,ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.state);
    /* Final reset is a separate IOCTL after independent cleanup diagnosis. */
    return ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR;
}

static NTSTATUS finalReset(Context *c) {
    NTSTATUS s;
    /* FLR can erase memory geometry while leaving compute straps intact. */
    if(c->report.mode==CMP_MEMORY)return STATUS_INVALID_DEVICE_STATE;
    if(c->mmioClosed||c->removed||c->ioFailed)return STATUS_INVALID_DEVICE_STATE;
    if(!c->resetInterface.DeviceReset||!(c->report.reset_supported&1u))return STATUS_NOT_SUPPORTED;
    /* Also allow a no-payload ResetOnly control experiment. */
    if(c->attempted&&(c->report.state!=1||c->report.status||c->report.diagnostic_status||
       c->report.checks!=31u||c->buffer||!c->quiescent))return STATUS_INVALID_DEVICE_STATE;
    diagnose(c);
    if(c->report.diagnostic_status)return STATUS_DEVICE_HARDWARE_ERROR;
    /* End all BAR access BEFORE calling the bus. Failure is also terminal.
     * NULL reset parameters: no asynchronous completion callback requested.
     * Only exact STATUS_SUCCESS counts; STATUS_PENDING is not completion. */
    c->mmioClosed=TRUE;c->report.snapshot_cached=1;c->report.reset_attempted=1;
    trace("final-reset.begin",31,STATUS_SUCCESS,c->report.reset_supported);
    s=c->resetInterface.DeviceReset(c->resetInterface.Context,FunctionLevelDeviceReset,0,NULL);
    c->report.reset_status=(ULONG)s;c->report.reset_complete=(s==STATUS_SUCCESS);
    c->report.stage=31;
    trace("final-reset.result",31,s,c->report.reset_complete);
    if(s!=STATUS_SUCCESS){c->report.state=2;return NT_SUCCESS(s)?STATUS_DEVICE_HARDWARE_ERROR:s;}
    trace("final-reset.complete",31,s,1);return STATUS_SUCCESS;
}

static NTSTATUS memoryHandover(Context *c) {
    ULONG i,cfg,amount;
    if(c->mmioClosed||c->removed||c->ioFailed||!c->attempted||
       c->report.mode!=CMP_MEMORY||c->report.state!=1||c->report.status||
       c->report.checks!=31u||c->buffer||!c->quiescent)return STATUS_INVALID_DEVICE_STATE;
    diagnose(c);
    if(c->report.diagnostic_status)return STATUS_DEVICE_HARDWARE_ERROR;
    /* Read individual FBPA mirrors, not just the broadcast register. Disabled
     * partitions return BADF and are reported as unavailable, not MMIO failure.
     * These fixed read-only addresses are within the existing mapped BAR0. */
    c->report.fbpa_readable_mask=0;
    for(i=0;i<CMP_FBPA_COUNT;i++) {
        cfg=READ_REGISTER_ULONG((PULONG)(c->bar+0x900204+i*0x4000));
        amount=READ_REGISTER_ULONG((PULONG)(c->bar+0x90020c+i*0x4000));
        c->report.fbpa_cfg1[i]=cfg;c->report.fbpa_amount[i]=amount;
        if(cfg!=~0u&&amount!=~0u&&(cfg&0xffff0000)!=0xbadf0000&&
           (amount&0xffff0000)!=0xbadf0000)c->report.fbpa_readable_mask|=1u<<i;
    }
    /* No payload, engine reset or geometry write after this point. PnP may now
     * release this helper and bind NVIDIA. NVIDIA acceptance is a separate test. */
    c->report.memory_handover=1;c->report.stage=32;
    c->report.snapshot_cached=2;c->mmioClosed=TRUE;
    trace("memory.handover-no-FLR",32,STATUS_SUCCESS,c->report.fbpa_readable_mask);
    return STATUS_SUCCESS;
}

#ifndef CMP_HOST_TEST
static NTSTATUS prepareHardware(WDFDEVICE dev,WDFCMRESLIST raw,WDFCMRESLIST translated) {
    Context *c=Ctx(dev);NTSTATUS s;ULONG i;PCI_COMMON_CONFIG cfg;ULONGLONG bar0;
    UNREFERENCED_PARAMETER(raw);
    trace("Prepare.begin",0,STATUS_SUCCESS,0);
    s=WdfFdoQueryForInterface(dev,&GUID_BUS_INTERFACE_STANDARD,(PINTERFACE)&c->bus,sizeof(c->bus),1,NULL);
    if(!NT_SUCCESS(s))return s;
    RtlZeroMemory(&cfg,sizeof(cfg));
    if(c->bus.GetBusData(c->bus.Context,PCI_WHICHSPACE_CONFIG,&cfg,0,64)!=64)return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->profile=profile_for(cfg.VendorID,cfg.DeviceID);
    if(!c->profile||cfg.BaseClass!=3||cfg.HeaderType!=0)return STATUS_NOT_SUPPORTED;
    bar0=cfg.u.type0.BaseAddresses[0];
    if((bar0&1)||((bar0&6)!=0&&(bar0&6)!=4))return STATUS_NOT_SUPPORTED;
    if((bar0&6)==4)bar0|=(ULONGLONG)cfg.u.type0.BaseAddresses[1]<<32;bar0&=~15ULL;
    /* Match BAR0 in RAW bus addresses, map corresponding TRANSLATED resource. */
    for(i=0;i<WdfCmResourceListGetCount(raw);i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(raw,i),t;
        if(r->Type!=CmResourceTypeMemory||(ULONGLONG)r->u.Memory.Start.QuadPart!=bar0)continue;
        t=WdfCmResourceListGetDescriptor(translated,i);
        if(!t||t->Type!=CmResourceTypeMemory||t->u.Memory.Length<0x9a4000)return STATUS_DEVICE_CONFIGURATION_ERROR;
        c->length=0x9a4000;c->bar=MmMapIoSpaceEx(t->u.Memory.Start,c->length,PAGE_READWRITE|PAGE_NOCACHE);break;
    }
    if(!c->bar)return STATUS_INSUFFICIENT_RESOURCES;
    c->report.version=CMP_VERSION;c->report.build=CMP_BUILD;c->report.device=cfg.DeviceID;
    s=WdfFdoQueryForInterface(dev,&GUID_DEVICE_RESET_INTERFACE_STANDARD,
        (PINTERFACE)&c->resetInterface,sizeof(c->resetInterface),DEVICE_RESET_INTERFACE_VERSION_1,NULL);
    c->report.reset_query_status=(ULONG)s;
    if(NT_SUCCESS(s))c->report.reset_supported=c->resetInterface.DeviceReset?c->resetInterface.SupportedResetTypes:0;
    else RtlZeroMemory(&c->resetInterface,sizeof(c->resetInterface));
    trace("reset-interface.query",0,s,c->report.reset_supported);
    RtlCopyMemory(c->report.regs,registers,sizeof(registers));
    trace("Prepare.ready",0,STATUS_SUCCESS,cfg.DeviceID);return STATUS_SUCCESS;
}
NTSTATUS Prepare(WDFDEVICE dev,WDFCMRESLIST raw,WDFCMRESLIST translated) {
    NTSTATUS s=prepareHardware(dev,raw,translated);trace("Prepare.return",0,s,0);return s;
}
NTSTATUS Enter(WDFDEVICE dev,WDF_POWER_DEVICE_STATE old) {
    Context *c=Ctx(dev);ULONG boot;UNREFERENCED_PARAMETER(old);
    if(c->report.state==2)return STATUS_DEVICE_HARDWARE_ERROR;
    if(c->mmioClosed)return STATUS_SUCCESS;
    c->ioFailed=FALSE;boot=readReg(c,0);
    trace("D0Entry.boot-register",0,STATUS_SUCCESS,boot);
    if(!boot||boot==~0u||c->ioFailed)return STATUS_DEVICE_HARDWARE_ERROR;
    return STATUS_SUCCESS;
}
NTSTATUS Leave(WDFDEVICE dev,WDF_POWER_DEVICE_STATE next) {
    Context *c=Ctx(dev);
    trace("D0Exit.begin",c->report.stage,STATUS_SUCCESS,(ULONG)next);
    if(!c->removed&&!c->mmioClosed) {
        if(c->mutated&&!c->quiescent)stopEngines(c);
        diagnose(c);
    } else trace("D0Exit.MMIO-skipped",c->report.stage,STATUS_DEVICE_REMOVED,0);
    trace("D0Exit.hardware-status",c->report.stage,(NTSTATUS)c->report.status,c->report.checks);
    /* Do not fail PnP solely for a configuration mismatch and prevent recovery.
     * Hardware outcome is reported separately from callback completion. */
    trace("D0Exit.callback-return",c->report.stage,STATUS_SUCCESS,(ULONG)next);
    return STATUS_SUCCESS;
}
VOID Surprise(WDFDEVICE dev) {
    Context *c=Ctx(dev);c->removed=TRUE;c->ioFailed=TRUE;c->quiescent=FALSE;
    trace("SurpriseRemoval",c->report.stage,STATUS_DEVICE_REMOVED,c->report.checks);
}
NTSTATUS Release(WDFDEVICE dev,WDFCMRESLIST resources) {
    Context *c=Ctx(dev);UNREFERENCED_PARAMETER(resources);
    /* Cached evidence only: BAR may no longer be accessible here. */
    trace("Release.begin",c->report.stage,(NTSTATUS)c->report.status,c->report.checks);
    freeDma(c);
    if(c->resetInterface.InterfaceDereference){c->resetInterface.InterfaceDereference(c->resetInterface.Context);RtlZeroMemory(&c->resetInterface,sizeof(c->resetInterface));}
    if(c->bar){MmUnmapIoSpace(c->bar,c->length);c->bar=NULL;}
    if(c->bus.InterfaceDereference){c->bus.InterfaceDereference(c->bus.Context);RtlZeroMemory(&c->bus,sizeof(c->bus));}
    trace("Release.callback-return",c->report.stage,STATUS_SUCCESS,c->report.checks);
    return STATUS_SUCCESS;
}
VOID Control(WDFQUEUE q,WDFREQUEST request,size_t outSize,size_t inSize,ULONG code) {
    WDFDEVICE dev=WdfIoQueueGetDevice(q);Context *c=Ctx(dev);CMP_OUTPUT *out;CMP_INPUT *in;CMP_INPUT input;
    NTSTATUS s;UNREFERENCED_PARAMETER(outSize);UNREFERENCED_PARAMETER(inSize);
    if(code!=CMP_DIAG&&code!=CMP_RUN&&code!=CMP_FINAL_RESET&&code!=CMP_MEMORY_HANDOVER){WdfRequestComplete(request,STATUS_INVALID_DEVICE_REQUEST);return;}
    s=WdfRequestRetrieveOutputBuffer(request,sizeof(*out),(PVOID*)&out,NULL);if(!NT_SUCCESS(s))goto fail;
    if(code==CMP_RUN||code==CMP_FINAL_RESET||code==CMP_MEMORY_HANDOVER) {
        s=WdfRequestRetrieveInputBuffer(request,sizeof(*in),(PVOID*)&in,NULL);if(!NT_SUCCESS(s))goto fail;
        input=*in;
        if(input.version!=CMP_VERSION||input.ack!=CMP_ACK||input.reserved||
           (code==CMP_RUN?(input.mode!=CMP_COMPUTE&&input.mode!=CMP_MEMORY):input.mode!=0)){s=STATUS_INVALID_PARAMETER;goto fail;}
        s=code==CMP_FINAL_RESET?finalReset(c):code==CMP_MEMORY_HANDOVER?memoryHandover(c):run(dev,c,input.mode);c->report.status=(ULONG)s;
        trace("Control.run-return",c->report.stage,s,c->report.state);
    } else {
        diagnose(c);
    }
    *out=c->report;
    /* Transport success preserves diagnostic output even after hardware failure. */
    WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(*out));return;
fail: WdfRequestComplete(request,s);
}
NTSTATUS AddDevice(WDFDRIVER driver,PWDFDEVICE_INIT init) {
    WDFDEVICE dev;WDF_OBJECT_ATTRIBUTES a;WDF_PNPPOWER_EVENT_CALLBACKS p;WDF_IO_QUEUE_CONFIG q;NTSTATUS s;
    UNREFERENCED_PARAMETER(driver);
    trace("AddDevice.begin",0,STATUS_SUCCESS,0);
    WdfDeviceInitSetIoType(init,WdfDeviceIoBuffered);WdfDeviceInitSetExclusive(init,TRUE);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);p.EvtDevicePrepareHardware=Prepare;p.EvtDeviceReleaseHardware=Release;
    p.EvtDeviceD0Entry=Enter;p.EvtDeviceD0Exit=Leave;p.EvtDeviceSurpriseRemoval=Surprise;
    WdfDeviceInitSetPnpPowerEventCallbacks(init,&p);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,Context);a.ExecutionLevel=WdfExecutionLevelPassive;
    a.SynchronizationScope=WdfSynchronizationScopeDevice;
    s=WdfDeviceCreate(&init,&a,&dev);trace("WdfDeviceCreate",0,s,0);if(!NT_SUCCESS(s))return s;
    s=WdfDeviceCreateDeviceInterface(dev,&interfaceId,NULL);trace("CreateInterface",0,s,0);if(!NT_SUCCESS(s))return s;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchSequential);q.EvtIoDeviceControl=Control;
    s=WdfIoQueueCreate(dev,&q,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);trace("CreateQueue",0,s,0);return s;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING path) {
    NTSTATUS s;WDF_DRIVER_CONFIG config;WDF_DRIVER_CONFIG_INIT(&config,AddDevice);
    trace("DriverEntry",0,STATUS_SUCCESS,WdfMinimumVersionRequired);
    trace("Build.0.9.0.0",0,STATUS_SUCCESS,CMP_BUILD);
    s=WdfDriverCreate(driver,path,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
    trace("WdfDriverCreate",0,s,config.Size);return s;
}
#endif
