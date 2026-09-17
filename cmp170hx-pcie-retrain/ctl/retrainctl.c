/* SPDX-License-Identifier: GPL-2.0-only
 *
 * retrainctl — CMP 170HX Gen2 重训练用户态控制器。
 *
 * 用法(管理员)：
 *   retrainctl --query                 只读巡检全部 20C2/2082(默认，任何时刻安全)
 *   retrainctl --apply                 对每张卡执行策略补写 + 根端口重训练
 *   retrainctl --install [-Sys path]   注册 SCM 内核服务 cmpretrain 并立即加载
 *   retrainctl --uninstall             停止并删除 cmpretrain 服务
 *   retrainctl --apply --gpu 179,0,0 --root 178,0,0   手动指定(十进制)单卡
 *
 * 前提：cmp170hx-windows-driver 0.11 MemoryGen2 已在本次启动内完成且 NVIDIA
 * 已健康接管(64 GiB)。本工具只补 GSP 初始化后缺失的策略与真实链路重训练。
 */
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/retrainabi.h"

#define MAX_CARDS 8

typedef struct {
    DWORD bus, dev, fn, seg;
    DWORD rbus, rdev, rfn;
    WCHAR instanceId[256];
    WCHAR service[128];
    WCHAR parentId[256];
    int   manual;
} CARD;

static FILE *gLog;

static void logline(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (gLog) {
        va_start(ap, fmt);
        vfprintf(gLog, fmt, ap);
        va_end(ap);
        fprintf(gLog, "\n");
        fflush(gLog);
    }
}

static void open_log(void) {
    WCHAR mod[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], stamp[64];
    SYSTEMTIME st;
    GetModuleFileNameW(NULL, mod, MAX_PATH);
    wcscpy_s(dir, MAX_PATH, mod);
    {
        WCHAR *s = wcsrchr(dir, L'\\');
        if (s) *s = 0;
    }
    GetLocalTime(&st);
    swprintf(stamp, 64, L"%04d%02d%02d-%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    swprintf(path, MAX_PATH, L"%s\\logs", dir);
    CreateDirectoryW(path, NULL);
    wcscat_s(path, MAX_PATH, L"\\retrain-");
    wcscat_s(path, MAX_PATH, stamp);
    wcscat_s(path, MAX_PATH, L".log");
    gLog = NULL;
    _wfopen_s(&gLog, path, L"w");
    if (gLog) wprintf(L"log: %s\n", path);
}

static int is_cmp_hwid(const char *s) {
    return _strnicmp(s, "PCI\\VEN_10DE&DEV_20C2", 20) == 0 ||
           _strnicmp(s, "PCI\\VEN_10DE&DEV_2082", 20) == 0;
}

static BOOL get_prop_u32(HDEVINFO set, PSP_DEVINFO_DATA d, const DEVPROPKEY *k, DWORD *out) {
    DEVPROPTYPE t = 0;
    BYTE buf[8];
    DWORD sz = sizeof(buf);
    if (!SetupDiGetDevicePropertyW(set, d, k, &t, buf, sz, NULL, 0)) return FALSE;
    if (t != DEVPROP_TYPE_UINT32) return FALSE;
    memcpy(out, buf, 4);
    return TRUE;
}

static BOOL get_prop_string(HDEVINFO set, PSP_DEVINFO_DATA d, const DEVPROPKEY *k,
                            WCHAR *out, DWORD outChars) {
    DEVPROPTYPE t = 0;
    if (!SetupDiGetDevicePropertyW(set, d, k, &t, (PBYTE)out, outChars * 2, NULL, 0))
        return FALSE;
    return t == DEVPROP_TYPE_STRING;
}

static BOOL hwid_is_cmp(HDEVINFO set, PSP_DEVINFO_DATA d) {
    DEVPROPTYPE t = 0;
    WCHAR buf[1024];
    WCHAR *p;
    if (!SetupDiGetDevicePropertyW(set, d, &DEVPKEY_Device_HardwareIds, &t,
                                   (PBYTE)buf, sizeof(buf), NULL, 0))
        return FALSE;
    for (p = buf; *p; p += wcslen(p) + 1) {
        char narrow[256];
        size_t conv = 0;
        wcstombs_s(&conv, narrow, sizeof(narrow), p, _TRUNCATE);
        if (is_cmp_hwid(narrow)) return TRUE;
    }
    return FALSE;
}

static BOOL first_hwid(HDEVINFO set, PSP_DEVINFO_DATA d, WCHAR *out, DWORD outChars) {
    DEVPROPTYPE t = 0;
    if (!SetupDiGetDevicePropertyW(set, d, &DEVPKEY_Device_HardwareIds, &t,
                                   (PBYTE)out, outChars * 2, NULL, 0))
        return FALSE;
    return out[0] != 0;
}

/* 枚举全部在场 CMP 卡并取直接父桥 BDF */
static int enum_cards(CARD *cards, int maxc) {
    HDEVINFO set;
    SP_DEVINFO_DATA d;
    DWORD i;
    int n = 0;

    set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return 0;

    for (i = 0; ; i++) {
        DWORD bus = 0, addr = 0, seg = 0;
        DEVINST parent = 0;
        DWORD pbus = 0, paddr = 0;

        memset(&d, 0, sizeof(d));
        d.cbSize = sizeof(d);
        if (!SetupDiEnumDeviceInfo(set, i, &d)) break;
        if (!hwid_is_cmp(set, &d)) continue;
        if (!get_prop_u32(set, &d, &DEVPKEY_Device_BusNumber, &bus)) continue;
        if (!get_prop_u32(set, &d, &DEVPKEY_Device_Address, &addr)) continue;
        seg = 0; /* 本机 segment 0；驱动侧也只接受 0 */

        if (CM_Get_Parent(&parent, d.DevInst, 0) != CR_SUCCESS) continue;

        /* 父节点 BDF：用 CM_Get_Device_ID + 新 devinfo 读取属性 */
        {
            WCHAR pid[256];
            CONFIGRET cr;
            HDEVINFO pset;
            SP_DEVINFO_DATA pd2;
            cr = CM_Get_Device_IDW(parent, pid, 256, 0);
            if (cr != CR_SUCCESS) continue;
            pset = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
            if (pset == INVALID_HANDLE_VALUE) continue;
            memset(&pd2, 0, sizeof(pd2));
            pd2.cbSize = sizeof(pd2);
            if (!SetupDiOpenDeviceInfoW(pset, pid, NULL, 0, &pd2)) {
                SetupDiDestroyDeviceInfoList(pset);
                continue;
            }
            if (!get_prop_u32(pset, &pd2, &DEVPKEY_Device_BusNumber, &pbus) ||
                !get_prop_u32(pset, &pd2, &DEVPKEY_Device_Address, &paddr)) {
                SetupDiDestroyDeviceInfoList(pset);
                continue;
            }
            if (n < maxc) {
                CARD *c = &cards[n];
                memset(c, 0, sizeof(*c));
                c->bus = bus; c->dev = addr & 0xFFFF; c->fn = (addr >> 16) & 0xFFFF;
                c->seg = seg;
                c->rbus = pbus; c->rdev = paddr & 0xFFFF; c->rfn = (paddr >> 16) & 0xFFFF;
                SetupDiGetDeviceInstanceIdW(set, &d, c->instanceId, 256, NULL);
                if (!get_prop_string(set, &d, &DEVPKEY_Device_Service, c->service, 128))
                    c->service[0] = 0;
                wcscpy_s(c->parentId, 256, pid);
                n++;
            }
            SetupDiDestroyDeviceInfoList(pset);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

static HANDLE open_interface(void) {
    return CreateFileW(CMPRT_WIN32_PATH, GENERIC_READ | GENERIC_WRITE,
                       0, NULL, OPEN_EXISTING, 0, NULL);
}

static const char *code_name(unsigned code) {
    switch (code) {
    case CMPRT_OK_ALREADY_GEN2:   return "OK_ALREADY_GEN2";
    case CMPRT_OK_RETRAINED:      return "OK_RETRAINED";
    case CMPRT_ERR_BAD_INPUT:     return "ERR_BAD_INPUT";
    case CMPRT_ERR_GPU_ID:        return "ERR_GPU_ID";
    case CMPRT_ERR_ROOT_ID:       return "ERR_ROOT_ID";
    case CMPRT_ERR_TOPOLOGY:      return "ERR_TOPOLOGY";
    case CMPRT_ERR_BAR0:          return "ERR_BAR0";
    case CMPRT_ERR_BAR0_DEAD:     return "ERR_BAR0_DEAD";
    case CMPRT_ERR_REG_STUCK:     return "ERR_REG_STUCK";
    case CMPRT_ERR_LINK_DOWN:     return "ERR_LINK_DOWN";
    case CMPRT_ERR_POLL_TIMEOUT:  return "ERR_POLL_TIMEOUT";
    case CMPRT_ERR_WIDTH_CHANGED: return "ERR_WIDTH_CHANGED";
    case CMPRT_ERR_NO_PCIE_CAP:   return "ERR_NO_PCIE_CAP";
    case CMPRT_ERR_MCFG:          return "ERR_MCFG";
    case CMPRT_ERR_MAP:           return "ERR_MAP";
    case CMPRT_OK_QUERY:          return "OK_QUERY";
    default:                      return "UNKNOWN";
    }
}

static void print_link(const char *tag, unsigned short st) {
    logline("%s Gen%u x%u LNKSTA=0x%04X%s", tag, st & 0xF, (st >> 4) & 0x3F, st,
            (st & 0x0800) ? " TRAINING" : "");
}

static void print_result(const CMPRT_RESULT *r) {
    static const struct { unsigned off; const char *name; } snap[CMPRT_BAR0_SNAP_COUNT] = {
        { CMPRT_R_BOOT0, "BOOT0" }, { CMPRT_R_XVE_OVR, "XVE_OVR" },
        { CMPRT_R_PRIV_MISC_1, "PRIV_MISC_1" }, { CMPRT_R_VSEC_DEVICE, "VSEC_DEVICE" },
        { CMPRT_R_VSEC_HIER, "VSEC_HIER" }, { CMPRT_R_LNKCAP_MIR, "LNKCAP_MIR" },
        { CMPRT_R_LC2_MIR, "LC2_MIR" }, { CMPRT_R_CYA_0, "CYA_0" },
        { CMPRT_R_LINK_CONFIG_0, "LINK_CONFIG_0" }, { CMPRT_R_XP3G_OVR0, "XP3G_OVR0" },
        { CMPRT_R_XP3G_VAL0, "XP3G_VAL0" }, { CMPRT_R_XP3G_OVR3, "XP3G_OVR3" },
        { CMPRT_R_XP3G_VAL3, "XP3G_VAL3" }
    };
    unsigned i;
    logline("  code=%u(%s) stage=%u polls=%u msg=%s",
            r->code, code_name(r->code), r->stage, r->poll_count, r->message);
    logline("  GPU id=0x%08X cap=0x%X BAR0=0x%08X%08X",
            r->gpu_id, r->gpu_pcie_cap, r->gpu_bar0_hi, r->gpu_bar0_lo);
    logline("  ROOT id=0x%08X cap=0x%X", r->root_id, r->root_pcie_cap);
    print_link("  GPU  before:", r->gpu_lnksta_before);
    print_link("  ROOT before:", r->root_lnksta_before);
    logline("  LNKCTL  gpu 0x%04X->0x%04X root 0x%04X->0x%04X",
            r->gpu_lnkctl_before, r->gpu_lnkctl_after,
            r->root_lnkctl_before, r->root_lnkctl_after);
    logline("  LNKCTL2 gpu 0x%04X->0x%04X root 0x%04X->0x%04X",
            r->gpu_lnkctl2_before, r->gpu_lnkctl2_after,
            r->root_lnkctl2_before, r->root_lnkctl2_after);
    print_link("  GPU  after :", r->gpu_lnksta_after);
    print_link("  ROOT after :", r->root_lnksta_after);
    for (i = 0; i < CMPRT_BAR0_SNAP_COUNT; i++) {
        if (r->bar0_before[i] != r->bar0_after[i])
            logline("  BAR0 %-14s 0x%08X -> 0x%08X", snap[i].name,
                    r->bar0_before[i], r->bar0_after[i]);
        else
            logline("  BAR0 %-14s 0x%08X", snap[i].name, r->bar0_before[i]);
    }
    if (r->code == CMPRT_ERR_REG_STUCK)
        logline("  STUCK at BAR0 offset 0x%X", r->stuck_reg);
    if (r->poll_count) {
        unsigned n = r->poll_count < 12 ? r->poll_count : 12;
        logline("  poll head(%u of %u): [root,gpu] LNKSTA", n, r->poll_count);
        for (i = 0; i < n; i++)
            logline("    #%02u root=0x%04X gpu=0x%04X", i,
                    r->poll_trace[i] & 0xFFFF, (r->poll_trace[i] >> 16) & 0xFFFF);
    }
}

static int run_one(HANDLE h, const CARD *c, int apply, CMPRT_RESULT *r) {
    CMPRT_REQUEST req;
    DWORD ret = 0;
    memset(&req, 0, sizeof(req));
    req.Segment = c->seg;
    req.GpuBus = c->bus; req.GpuDevice = c->dev; req.GpuFunction = c->fn;
    req.RootBus = c->rbus; req.RootDevice = c->rdev; req.RootFunction = c->rfn;
    req.Flags = apply ? CMPRT_FLAG_APPLY : 0;
    if (!DeviceIoControl(h, IOCTL_CMPRT_RUN, &req, sizeof(req), r, sizeof(*r), &ret, NULL) ||
        ret != sizeof(*r))
        return -1;
    return 0;
}

/* SCM 服务模式安装：CreateService + StartService，立即生效、无需 PnP/INF */
static int do_install(const WCHAR *sysPath) {
    WCHAR full[MAX_PATH], binPath[MAX_PATH + 8];
    SC_HANDLE scm = NULL, svc = NULL;
    DWORD e;
    int rc = 0;

    if (!sysPath) {
        /* 默认：与 retrainctl.exe 同目录的 cmpretrain.sys */
        GetModuleFileNameW(NULL, full, MAX_PATH);
        {
            WCHAR *s = wcsrchr(full, L'\\');
            if (s) s[1] = 0;
        }
        wcscat_s(full, MAX_PATH, L"cmpretrain.sys");
    } else if (!_wfullpath(full, sysPath, MAX_PATH)) {
        logline("bad sys path");
        return 40;
    }
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        logline("cmpretrain.sys not found: %ls", full);
        return 41;
    }

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        logline("OpenSCManager err=%lu (需要管理员)", GetLastError());
        return 42;
    }
    swprintf(binPath, MAX_PATH + 8, L"\\??\\%s", full);
    svc = CreateServiceW(scm, CMPRT_SERVICE_NAME, L"CMP 170HX Gen2 Retrain Helper",
                         SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
                         SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                         binPath, NULL, NULL, NULL, NULL, NULL);
    if (!svc) {
        e = GetLastError();
        if (e != ERROR_SERVICE_EXISTS) {
            logline("CreateService err=%lu (0x%08lX)", e, e);
            CloseServiceHandle(scm);
            return 43;
        }
        logline("service exists; updating binary path");
        svc = OpenServiceW(scm, CMPRT_SERVICE_NAME, SERVICE_ALL_ACCESS);
        if (!svc) {
            logline("open existing service err=%lu", GetLastError());
            CloseServiceHandle(scm);
            return 43;
        }
        if (!ChangeServiceConfigW(svc, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                                  SERVICE_ERROR_NORMAL, binPath, NULL, NULL,
                                  NULL, NULL, NULL, NULL))
            logline("WARN: ChangeServiceConfig err=%lu (非致命)", GetLastError());
        /* 关键：先停旧驱动再启动，否则运行中的仍是旧镜像(0.2 曾因此白跑一轮) */
        {
            SERVICE_STATUS_PROCESS ssp;
            SERVICE_STATUS ss2;
            DWORD bytes = 0, wait;
            int stopped = 0;
            memset(&ssp, 0, sizeof(ssp));
            if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                     (LPBYTE)&ssp, sizeof(ssp), &bytes) &&
                ssp.dwCurrentState == SERVICE_RUNNING) {
                memset(&ss2, 0, sizeof(ss2));
                if (ControlService(svc, SERVICE_CONTROL_STOP, &ss2)) {
                    for (wait = 0; wait < 30; wait++) {
                        memset(&ssp, 0, sizeof(ssp));
                        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                                  (LPBYTE)&ssp, sizeof(ssp), &bytes))
                            break;
                        if (ssp.dwCurrentState == SERVICE_STOPPED) { stopped = 1; break; }
                        Sleep(100);
                    }
                    logline(stopped ? "old driver stopped" :
                            "WARN: stop timed out; 启动的可能仍是旧驱动");
                } else {
                    logline("WARN: stop old driver err=%lu (非致命)", GetLastError());
                }
            } else {
                stopped = 1;
            }
        }
    }

    if (!StartServiceW(svc, 0, NULL)) {
        e = GetLastError();
        if (e == ERROR_SERVICE_ALREADY_RUNNING) {
            logline("service already running");
        } else {
            logline("StartService err=%lu (0x%08lX)", e, e);
            logline("提示: 需测试签名模式且 .sys 已用受信证书签名(与 cmp170 相同); "
                    "err=577=签名不受信, err=1275=驱动加载策略阻止");
            rc = 44;
        }
    } else {
        logline("driver installed and started: %ls", binPath);
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return rc;
}

static int do_uninstall(void) {
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;
    DWORD e;
    HDEVINFO set;
    SP_DEVINFO_DATA d;
    DWORD i;
    int nodes = 0;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        logline("OpenSCManager err=%lu (需要管理员)", GetLastError());
        return 42;
    }
    svc = OpenServiceW(scm, CMPRT_SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        e = GetLastError();
        logline("service %ls not found (err=%lu)", CMPRT_SERVICE_NAME, e);
        CloseServiceHandle(scm);
    } else {
        memset(&ss, 0, sizeof(ss));
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
            e = GetLastError();
            if (e != ERROR_SERVICE_NOT_ACTIVE)
                logline("WARN: stop service err=%lu (非致命)", e);
        }
        if (!DeleteService(svc)) {
            e = GetLastError();
            if (e != ERROR_SERVICE_MARKED_FOR_DELETE) {
                logline("DeleteService err=%lu", e);
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return 45;
            }
        }
        logline("service %ls removed", CMPRT_SERVICE_NAME);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
    }

    /* 顺手清理旧根枚举方案可能残留的 ROOT\CMP170RETRAIN 节点(若有) */
    set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set != INVALID_HANDLE_VALUE) {
        for (i = 0; ; i++) {
            WCHAR id[256];
            memset(&d, 0, sizeof(d));
            d.cbSize = sizeof(d);
            if (!SetupDiEnumDeviceInfo(set, i, &d)) break;
            if (!SetupDiGetDeviceInstanceIdW(set, &d, id, 256, NULL)) continue;
            if (_wcsnicmp(id, L"ROOT\\CMP170RETRAIN", 18) != 0) continue;
            if (SetupDiSetSelectedDevice(set, &d) &&
                SetupDiCallClassInstaller(DIF_REMOVE, set, &d)) nodes++;
        }
        SetupDiDestroyDeviceInfoList(set);
        if (nodes) logline("legacy root device nodes removed: %d", nodes);
    }
    return 0;
}

static void usage(void) {
    logline("retrainctl [--query|--apply|--install|--uninstall] "
            "[--gpu bus,dev,fn --root bus,dev,fn] [-Sys cmpretrain.sys]");
}

int wmain(int argc, WCHAR **argv) {
    int apply = 0, install = 0, uninstall = 0;
    int i, n, rc = 0;
    CARD cards[MAX_CARDS];
    HANDLE h;
    const WCHAR *sysPath = NULL;
    const WCHAR *gpuArg = NULL, *rootArg = NULL;

    open_log();
    for (i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--apply")) apply = 1;
        else if (!wcscmp(argv[i], L"--query")) apply = 0;
        else if (!wcscmp(argv[i], L"--install")) install = 1;
        else if (!wcscmp(argv[i], L"--uninstall")) uninstall = 1;
        else if (!wcscmp(argv[i], L"-Sys") && i + 1 < argc) sysPath = argv[++i];
        else if (!wcscmp(argv[i], L"-Inf") && i + 1 < argc) i++; /* 旧参数兼容：忽略 */
        else if (!wcscmp(argv[i], L"--gpu") && i + 1 < argc) gpuArg = argv[++i];
        else if (!wcscmp(argv[i], L"--root") && i + 1 < argc) rootArg = argv[++i];
        else { usage(); return 2; }
    }

    if (install) return do_install(sysPath);
    if (uninstall) return do_uninstall();

    logline("=== retrainctl %s ===", apply ? "APPLY" : "QUERY");

    if (gpuArg || rootArg) {
        unsigned b, d2, f;
        if (!gpuArg || !rootArg) {
            logline("both --gpu and --root are required in manual mode");
            return 2;
        }
        memset(cards, 0, sizeof(cards));
        if (swscanf_s(gpuArg, L"%u,%u,%u", &b, &d2, &f) != 3) { usage(); return 2; }
        cards[0].bus = b; cards[0].dev = d2; cards[0].fn = f;
        if (swscanf_s(rootArg, L"%u,%u,%u", &b, &d2, &f) != 3) { usage(); return 2; }
        cards[0].rbus = b; cards[0].rdev = d2; cards[0].rfn = f;
        cards[0].manual = 1;
        n = 1;
    } else {
        n = enum_cards(cards, MAX_CARDS);
    }

    if (n == 0) {
        logline("no present CMP 170HX (10DE:20C2/2082) found");
        return 3;
    }
    for (i = 0; i < n; i++) {
        CARD *c = &cards[i];
        logline("found GPU %02X:%02X.%X -> parent bridge %02X:%02X.%X service=%ls",
                c->bus, c->dev, c->fn, c->rbus, c->rdev, c->rfn,
                c->service[0] ? c->service : L"(manual)");
    }

    h = open_interface();
    if (h == INVALID_HANDLE_VALUE) {
        logline("cannot open %ls (err=%lu)", CMPRT_WIN32_PATH, GetLastError());
        logline("先运行 tools\\install.ps1 (或 retrainctl --install) 安装并启动服务");
        return 4;
    }

    for (i = 0; i < n; i++) {
        CARD *c = &cards[i];
        CMPRT_RESULT r;
        int one;
        logline("---- card %d/%d GPU %02X:%02X.%X ROOT %02X:%02X.%X %s",
                i + 1, n, c->bus, c->dev, c->fn, c->rbus, c->rdev, c->rfn,
                apply ? "[APPLY]" : "[query]");
        if (!c->manual)
            logline("     instance=%ls service=%ls parent=%ls",
                    c->instanceId, c->service, c->parentId);
        if (c->service[0] && _wcsicmp(c->service, L"nvlddmkm") != 0)
            logline("     WARN: GPU 当前服务不是 nvlddmkm；应先完成 0.11 MemoryGen2 交接");
        memset(&r, 0, sizeof(r));
        one = run_one(h, c, apply, &r);
        if (one != 0) {
            logline("     DeviceIoControl failed err=%lu", GetLastError());
            if (!rc) rc = 5;
            continue;
        }
        print_result(&r);
        if (r.code != CMPRT_OK_ALREADY_GEN2 && r.code != CMPRT_OK_RETRAINED &&
            r.code != CMPRT_OK_QUERY) {
            if (!rc) rc = (int)(10 + r.code);
        }
    }
    CloseHandle(h);

    if (apply && rc == 0) {
        logline("全部通过。下一步：nvidia-smi 确认 pcie.link.gen.current=2，"
                "并跑 256MiB pinned H2D/D2H 带宽验证(预期约 1.6 GB/s 量级)");
    }
    logline("exit=%d", rc);
    return rc;
}
