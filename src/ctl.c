/* SPDX-License-Identifier: GPL-2.0-only */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "protocol.h"
static const GUID interfaceId=CMP_GUID_VALUE;
static int bindDevice(const wchar_t *inf) {
    HDEVINFO list;SP_DEVINFO_DATA dev;DWORD index,type;unsigned count=0;
    wchar_t ids[4096],chosen[64]={0},full[MAX_PATH];BOOL reboot=FALSE;
    DWORD length=GetFullPathNameW(inf,MAX_PATH,full,NULL);
    if(!length||length>=MAX_PATH||GetFileAttributesW(full)==INVALID_FILE_ATTRIBUTES)return 2;
    list=SetupDiGetClassDevsW(NULL,L"PCI",NULL,DIGCF_ALLCLASSES|DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE)return 2;
    for(index=0;;index++) {
        wchar_t *id;dev.cbSize=sizeof(dev);
        if(!SetupDiEnumDeviceInfo(list,index,&dev))break;
        ZeroMemory(ids,sizeof(ids));
        if(!SetupDiGetDeviceRegistryPropertyW(list,&dev,SPDRP_HARDWAREID,&type,(PBYTE)ids,sizeof(ids)-sizeof(wchar_t),NULL))continue;
        if(type!=REG_MULTI_SZ)continue;
        for(id=ids;*id;id+=wcslen(id)+1) {
            if((!_wcsnicmp(id,L"PCI\\VEN_10DE&DEV_20C2",21)||!_wcsnicmp(id,L"PCI\\VEN_10DE&DEV_2082",21))&&
               (id[21]==0||id[21]==L'&')&&wcslen(id)<64) {
                wcscpy_s(chosen,64,id);count++;break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(list);
    if(count!=1){fprintf(stderr,"Refusing binding: expected exactly one CMP target, found %u.\n",count);return 3;}
    wprintf(L"Binding %ls to %ls\n",chosen,full);
    if(!UpdateDriverForPlugAndPlayDevicesW(NULL,chosen,full,INSTALLFLAG_FORCE,&reboot)) {
        fprintf(stderr,"Binding failed: Win32 %lu\n",GetLastError());return 4;
    }
    printf("Binding completed. RebootRequired=%u\n",reboot?1:0);return reboot?3010:0;
}
int wmain(int argc,wchar_t **argv) {
    HDEVINFO list;SP_DEVICE_INTERFACE_DATA dev;PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
    DWORD required,bytes;HANDLE handle;CMP_OUTPUT output;CMP_INPUT input={CMP_VERSION,0,CMP_ACK,0};unsigned i,count=0;
    BOOL ok;ULONG code=CMP_DIAG;
    if(argc==4&&!wcscmp(argv[1],L"--bind")&&!wcscmp(argv[3],L"--ack-device-rebind"))return bindDevice(argv[2]);
    if(argc==2&&!wcscmp(argv[1],L"--diag")) {}
    else if(argc==3&&!wcscmp(argv[1],L"--final-reset")&&!wcscmp(argv[2],L"--ack-experimental"))code=CMP_FINAL_RESET;
    else if(argc==3&&!wcscmp(argv[1],L"--memory-handover")&&!wcscmp(argv[2],L"--ack-experimental"))code=CMP_MEMORY_HANDOVER;
    else if(argc==3&&!wcscmp(argv[2],L"--ack-experimental")&&
        (!wcscmp(argv[1],L"--compute")||!wcscmp(argv[1],L"--memory"))) {
        input.mode=!wcscmp(argv[1],L"--memory")?CMP_MEMORY:CMP_COMPUTE;code=CMP_RUN;
    } else {
        puts("cmpctl --diag\ncmpctl --compute --ack-experimental\ncmpctl --memory --ack-experimental\ncmpctl --final-reset --ack-experimental\n"
             "cmpctl --memory-handover --ack-experimental (Memory uses no FLR)\n"
             "cmpctl --bind ABSOLUTE_INF_PATH --ack-device-rebind\n"
             "Memory mode also applies compute registers. Use one operation per cold boot.\n"
             "A register match does NOT prove usable expanded HBM.");return 2;
    }
    list=SetupDiGetClassDevsW(&interfaceId,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(list==INVALID_HANDLE_VALUE)return 3;
    dev.cbSize=sizeof(dev);
    while(SetupDiEnumDeviceInterfaces(list,NULL,&interfaceId,count,&dev))count++;
    if(count!=1){fprintf(stderr,"Expected one active CMP device interface; found %u. INF binding alone does not mean the driver started. Run manage.ps1 -Action Diagnostic for PnP ProblemCode/ProblemStatus, kernel and CodeIntegrity logs.\n",count);SetupDiDestroyDeviceInfoList(list);return 3;}
    SetupDiEnumDeviceInterfaces(list,NULL,&interfaceId,0,&dev);
    required=0;SetupDiGetDeviceInterfaceDetailW(list,&dev,NULL,0,&required,NULL);
    if(required<sizeof(*detail)||required>65536){SetupDiDestroyDeviceInfoList(list);return 3;}
    detail=malloc(required);if(!detail){SetupDiDestroyDeviceInfoList(list);return 3;}detail->cbSize=sizeof(*detail);
    if(!SetupDiGetDeviceInterfaceDetailW(list,&dev,detail,required,NULL,NULL)){free(detail);SetupDiDestroyDeviceInfoList(list);return 3;}
    handle=CreateFileW(detail->DevicePath,GENERIC_READ|(code!=CMP_DIAG?GENERIC_WRITE:0),0,NULL,OPEN_EXISTING,0,NULL);
    free(detail);SetupDiDestroyDeviceInfoList(list);
    if(handle==INVALID_HANDLE_VALUE){fprintf(stderr,"Open failed: Win32 %lu (run elevated).\n",GetLastError());return 4;}
    ZeroMemory(&output,sizeof(output));
    ok=DeviceIoControl(handle,code,code!=CMP_DIAG?&input:NULL,code!=CMP_DIAG?sizeof(input):0,&output,sizeof(output),&bytes,NULL);
    if(!ok){fprintf(stderr,"IOCTL failed: Win32 %lu\n",GetLastError());CloseHandle(handle);return 5;}
    CloseHandle(handle);
    if(bytes!=sizeof(output)||output.version!=CMP_VERSION){fprintf(stderr,"Protocol mismatch.\n");return 6;}
    printf("protocol=%lu build=%08lX device=%04lX state=%lu status=0x%08lX stage=%lu mode=%lu\n",output.version,output.build,output.device,output.state,output.status,output.stage,output.mode);
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

