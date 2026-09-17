/* SPDX-License-Identifier: GPL-2.0-only */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "protocol.h"
static const GUID interfaceId=CMP_GUID_VALUE;
/* Generic device IDs, not SUBSYS/REV-qualified ones: identical boards enumerate
 * with the same hardware ID, so one update call covers every installed card. */
#define CMP_HWID_LENGTH 21u
#define CMP_HWID_VARIANTS 2u
static int bindDevice(const wchar_t *inf) {
    HDEVINFO list;SP_DEVINFO_DATA dev;DWORD index,type;unsigned count=0,baseCount=0,u;
    wchar_t ids[4096],bases[CMP_HWID_VARIANTS][24],full[MAX_PATH];BOOL reboot=FALSE,required=FALSE;
    DWORD length=GetFullPathNameW(inf,MAX_PATH,full,NULL);
    if(!length||length>=MAX_PATH||GetFileAttributesW(full)==INVALID_FILE_ATTRIBUTES)return 2;
    list=SetupDiGetClassDevsW(NULL,L"PCI",NULL,DIGCF_ALLCLASSES|DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE)return 2;
    for(index=0;;index++) {
        wchar_t *id;BOOLEAN seen=FALSE;dev.cbSize=sizeof(dev);
        if(!SetupDiEnumDeviceInfo(list,index,&dev))break;
        ZeroMemory(ids,sizeof(ids));
        if(!SetupDiGetDeviceRegistryPropertyW(list,&dev,SPDRP_HARDWAREID,&type,(PBYTE)ids,sizeof(ids)-sizeof(wchar_t),NULL))continue;
        if(type!=REG_MULTI_SZ)continue;
        for(id=ids;*id;id+=wcslen(id)+1) {
            if((!_wcsnicmp(id,L"PCI\\VEN_10DE&DEV_20C2",CMP_HWID_LENGTH)||!_wcsnicmp(id,L"PCI\\VEN_10DE&DEV_2082",CMP_HWID_LENGTH))&&
               (id[CMP_HWID_LENGTH]==0||id[CMP_HWID_LENGTH]==L'&')) {
                if(!seen){count++;seen=TRUE;}
                for(u=0;u<baseCount;u++)if(!_wcsnicmp(bases[u],id,CMP_HWID_LENGTH))break;
                if(u==baseCount&&baseCount<CMP_HWID_VARIANTS) {
                    wcsncpy_s(bases[baseCount],24,id,CMP_HWID_LENGTH);
                    bases[baseCount][CMP_HWID_LENGTH]=0;baseCount++;
                }
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(list);
    if(!count){fprintf(stderr,"Refusing binding: no present CMP target found.\n");return 3;}
    printf("Binding %u present CMP target(s), %u distinct device ID(s), to %ls\n",count,baseCount,full);
    for(u=0;u<baseCount;u++) {
        printf("  Device ID %ls\n",bases[u]);
        reboot=FALSE;
        if(!UpdateDriverForPlugAndPlayDevicesW(NULL,bases[u],full,INSTALLFLAG_FORCE,&reboot)) {
            fprintf(stderr,"Binding failed for %ls: Win32 %lu\n",bases[u],GetLastError());return 4;
        }
        if(reboot)required=TRUE;
    }
    printf("Binding completed. RebootRequired=%u\n",required?1:0);return required?3010:0;
}
/* One IOCTL, one device. Every present interface gets its own block so a caller
 * can attribute each result to a specific card. */
static int runDevice(const wchar_t *path,ULONG code,const CMP_INPUT *input) {
    HANDLE handle;CMP_OUTPUT output;DWORD bytes=0;BOOL ok;unsigned i;
    handle=CreateFileW(path,GENERIC_READ|(code!=CMP_DIAG?GENERIC_WRITE:0),0,NULL,OPEN_EXISTING,0,NULL);
    if(handle==INVALID_HANDLE_VALUE){fprintf(stderr,"Open failed: Win32 %lu (run elevated).\n",GetLastError());return 4;}
    ZeroMemory(&output,sizeof(output));
    ok=DeviceIoControl(handle,code,code!=CMP_DIAG?(PVOID)input:NULL,code!=CMP_DIAG?(DWORD)sizeof(*input):0,&output,sizeof(output),&bytes,NULL);
    CloseHandle(handle);
    if(!ok){fprintf(stderr,"IOCTL failed: Win32 %lu\n",GetLastError());return 5;}
    if(bytes!=sizeof(output)||output.version!=CMP_VERSION){fprintf(stderr,"Protocol mismatch.\n");return 6;}
    printf("protocol=%lu build=%08lX device=%04lX state=%lu status=0x%08lX stage=%lu mode=%lu\n",output.version,output.build,output.device,output.state,output.status,output.stage,output.mode);
    printf("PCIE requested=%lu configured=%lu current_gen=%lu width=%lu link_status=%08lX (configuration is not negotiated-speed acceptance; VSEC_DEVICE bit0 best-effort)\n",
        output.pcie_requested,output.pcie_configured,(output.pcie_link_status>>16)&15,
        (output.pcie_link_status>>20)&63,output.pcie_link_status);
    if(output.pcie_requested)for(i=0;i<18;i++)printf("PCIE[%02u] before=%08lX wanted=%08lX after=%08lX\n",i,
        output.pcie_before[i],output.pcie_wanted[i],output.pcie_after[i]);
    printf("RESET query=0x%08lX supported=0x%08lX attempted=%lu status=0x%08lX complete=%lu cached=%lu\n",
        output.reset_query_status,output.reset_supported,output.reset_attempted,output.reset_status,output.reset_complete,output.snapshot_cached);
    puts(output.snapshot_cached==2?"Register   Before     Cached NO-FLR handover":output.snapshot_cached?"Register   Before     Cached PRE-RESET (not current)":"Register   Before     After/current");
    for(i=0;i<CMP_REG_COUNT;i++)printf("%08lX   %08lX   %08lX\n",output.regs[i],output.before[i],output.after[i]);
    printf("RUN checks=0x%02lX target=%u SEC2_stopped=%u GSP_stopped=%u cleanup_verified=%u DMA_released=%u DMA_retained=%u\n",
        output.checks,!!(output.checks&CMP_TARGET_MATCHED),!!(output.checks&CMP_SEC2_STOPPED),
        !!(output.checks&CMP_GSP_STOPPED),!!(output.checks&CMP_CLEANUP_VERIFIED),
        !!(output.checks&CMP_DMA_RELEASED),!!(output.checks&CMP_DMA_RETAINED));
      printf("RUN cleanup_attempted=%lu mismatch_mask=0x%lX (%s)\n",output.cleanup_attempted,output.cleanup_mismatch,
          output.mode==CMP_MEMORY?"bit0=WPR_LO bit1=WPR_HI bit2=1180F8 high bits":"bit0=reserved bit1=WPR_HI.VAL bit2=1180F8");
      printf("RUN cleanup before: %08lX %08lX %08lX; immediate cleanup readback: %08lX %08lX %08lX\n",
        output.cleanup_before[0],output.cleanup_before[1],output.cleanup_before[2],
        output.cleanup_after[0],output.cleanup_after[1],output.cleanup_after[2]);
    printf("RUN stop final DMACTL: SEC2=%08lX GSP=%08lX; latest DIAG status=0x%08lX\n",output.stop_final[0],output.stop_final[1],output.diagnostic_status);
    printf("RUN stop CPUCTL: SEC2=%08lX GSP=%08lX; DMATRFCMD: SEC2=%08lX GSP=%08lX\n",output.stop_cpu[0],output.stop_cpu[1],output.stop_dma[0],output.stop_dma[1]);
      printf("RUN GSP RISCV active status=%08lX (bit0 must be zero)\n",output.stop_gsp_riscv);
      puts(output.mode==CMP_MEMORY?"CLEANUP strategy=original-single-chain SEC2-PMC/security graphics-reset; GSP idle observed without reset":"CLEANUP strategy=WPR-PLM-host-restore (no Booter Unload)");
      if(output.mode==CMP_MEMORY) {
          puts("MEMORY candidate=64GiB (20C2 only); expected CFG1=02779000 LMR=0000020B.");
          puts("MEMORY handover must preserve geometry: no final FLR. Verify NVIDIA capacity and CUDA separately.");
          printf("MEMORY handover=%lu no_flr=1 cached=%lu FBPA_readable=0x%06lX\n",output.memory_handover,output.snapshot_cached,output.fbpa_readable_mask);
          if(output.memory_handover)for(i=0;i<CMP_FBPA_COUNT;i++)
              printf("FBPA[%02u] CFG1=%08lX CSTATUS=%08lX readable=%u\n",i,output.fbpa_cfg1[i],output.fbpa_amount[i],!!(output.fbpa_readable_mask&(1u<<i)));
      }
      for(i=0;i<output.plm_count&&i<4;i++) {
          const CMP_PLM_OUTPUT *p=&output.plm[i];
          printf("PLM[%lu] reg=%08lX wanted=%08lX before=%08lX after=%08lX outcome=%lu (1=skip 2=written 3=failed)\n",
              i,p->reg,p->wanted,p->before,p->after,p->outcome);
          printf("PLM[%lu] pre-fire WPR=%08lX/%08lX; exit CPUCTL=%08lX mailbox0=%08lX mailbox1=%08lX\n",
              i,p->wpr_lo,p->wpr_hi,p->cpu,p->mailbox0,p->mailbox1);
      }
    if(output.state==1&&!output.status&&!output.diagnostic_status)
        puts("Targets and cleanup verified by this driver. NVIDIA startup and CUDA correctness remain unverified.");
    if(!output.state)puts("No completed RUN. Reset/diagnostics alone do not unlock compute.");
    if(output.state==2)puts("Hardware attempt failed. Save this log and cold power cycle before another attempt.");
    return output.status||output.diagnostic_status||output.state==2||(code==CMP_RUN&&output.state!=1)||(code==CMP_FINAL_RESET&&!output.reset_complete)||(code==CMP_MEMORY_HANDOVER&&!output.memory_handover)?7:0;
}
int wmain(int argc,wchar_t **argv) {
    HDEVINFO list;SP_DEVICE_INTERFACE_DATA dev;PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
    DWORD required;wchar_t **paths;CMP_INPUT input={CMP_VERSION,0,CMP_ACK,0};unsigned i,count=0;
    ULONG code=CMP_DIAG;int failed=0,result;
    if(argc==4&&!wcscmp(argv[1],L"--bind")&&!wcscmp(argv[3],L"--ack-device-rebind"))return bindDevice(argv[2]);
    if(argc==2&&!wcscmp(argv[1],L"--diag")) {}
    else if(argc==3&&!wcscmp(argv[1],L"--final-reset")&&!wcscmp(argv[2],L"--ack-experimental"))code=CMP_FINAL_RESET;
    else if(argc==3&&!wcscmp(argv[1],L"--memory-handover")&&!wcscmp(argv[2],L"--ack-experimental"))code=CMP_MEMORY_HANDOVER;
    else if(argc==3&&!wcscmp(argv[2],L"--ack-experimental")&&
        (!wcscmp(argv[1],L"--compute")||!wcscmp(argv[1],L"--memory")||!wcscmp(argv[1],L"--memory-gen2")||!wcscmp(argv[1],L"--pcie-resume"))) {
        input.mode=!wcscmp(argv[1],L"--compute")?CMP_COMPUTE:CMP_MEMORY;
        input.reserved=!wcscmp(argv[1],L"--pcie-resume")?CMP_GEN2_RESUME:!wcscmp(argv[1],L"--memory-gen2")?CMP_GEN2_REQUEST:0;code=CMP_RUN;
    } else {
        puts("cmpctl --diag\ncmpctl --compute --ack-experimental\ncmpctl --memory --ack-experimental\ncmpctl --memory-gen2 --ack-experimental\ncmpctl --pcie-resume --ack-experimental\ncmpctl --final-reset --ack-experimental\n"
             "cmpctl --memory-handover --ack-experimental (Memory uses no FLR)\n"
             "cmpctl --bind ABSOLUTE_INF_PATH --ack-device-rebind\n"
             "Memory mode also applies compute registers. Use one operation per cold boot.\n"
             "A register match does NOT prove usable expanded HBM.");return 2;
    }
    list=SetupDiGetClassDevsW(&interfaceId,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(list==INVALID_HANDLE_VALUE)return 3;
    dev.cbSize=sizeof(dev);
    while(SetupDiEnumDeviceInterfaces(list,NULL,&interfaceId,count,&dev))count++;
    if(!count){fprintf(stderr,"No active CMP device interface found. INF binding alone does not mean the driver started. Run manage.ps1 -Action Diagnostic for PnP ProblemCode/ProblemStatus, kernel and CodeIntegrity logs.\n");SetupDiDestroyDeviceInfoList(list);return 3;}
    paths=malloc(count*sizeof(*paths));
    if(!paths){SetupDiDestroyDeviceInfoList(list);return 3;}
    for(i=0;i<count;i++) {
        paths[i]=NULL;
        SetupDiEnumDeviceInterfaces(list,NULL,&interfaceId,i,&dev);
        required=0;SetupDiGetDeviceInterfaceDetailW(list,&dev,NULL,0,&required,NULL);
        if(required<sizeof(*detail)||required>65536){SetupDiDestroyDeviceInfoList(list);free(paths);return 3;}
        detail=malloc(required);
        if(!detail){SetupDiDestroyDeviceInfoList(list);free(paths);return 3;}
        detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(list,&dev,detail,required,NULL,NULL)){free(detail);SetupDiDestroyDeviceInfoList(list);free(paths);return 3;}
        paths[i]=_wcsdup(detail->DevicePath);
        free(detail);
        if(!paths[i]){while(i)free(paths[--i]);SetupDiDestroyDeviceInfoList(list);free(paths);return 3;}
    }
    SetupDiDestroyDeviceInfoList(list);
    for(i=0;i<count;i++) {
        /* Callers attribute each block to a card by the interface path. */
        printf("DEVICE index=%u count=%u path=%ls\n",i,count,paths[i]);
        result=runDevice(paths[i],code,&input);
        if(result&&!failed)failed=result;
        free(paths[i]);
    }
    free(paths);
    return failed;
}

