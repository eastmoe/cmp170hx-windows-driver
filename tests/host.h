/* Test-only HAL/MMIO substitutions. Never included in production builds. */
#include <stdlib.h>
#include <stdio.h>
static Context *active;
static unsigned writes,reads,allocations,frees,releases,resetCalls;
static NTSTATUS resetResult;
static NTSTATUS fakeFinalReset(PVOID context,DEVICE_RESET_TYPE type,ULONG flags,PVOID parameters) {
    (void)context;resetCalls++;
    if(type!=FunctionLevelDeviceReset||flags||parameters||!active->mmioClosed)return STATUS_INVALID_PARAMETER;
    /* Model the geometry-vs-compute reset-domain distinction. */
    if(resetResult==STATUS_SUCCESS) {
        *(PULONG)(active->bar+REG_CFG1)=0x02449000;
        *(PULONG)(active->bar+REG_LMR)=active->profile->device==0x20c2?0x208:0x288;
        *(PULONG)(active->bar+0x9a0148)=0xffffff8f;
    }
    return resetResult;
}
static BOOLEAN allocationFail,fireFail,resetFail,cleanupIgnore,cleanupReadFail,dmaBusy,finalSnapshotFail;
/* Model assumptions: WPR_CFG opens host access; a payload can keep running
 * after its target is visible. These are simulated, not firmware emulation. */
static BOOLEAN holdCpuRunning,wprRestoreIgnore,tlsNeedsXve;
static ULONG fireFailAddress;
static ULONG geometryIgnoreAddress;
static BOOLEAN secureFail,graphicsFail,sec2Busy,queueBusy,pmcIgnore;
static unsigned gspResets,secureCommands,hostGeometry,hostWpr;
static ULONG writeRegs[40000],writeValues[40000],writeCount;
static unsigned fireCount,wprWhileRunning,geometryWrites,cases;
static ULONG fired[4];
static USHORT pciCommand=6;
static ULONG fakeRead(PULONG p) {
    reads++;
    ULONG offset=(ULONG)((PUCHAR)p-active->bar);
    if(finalSnapshotFail&&active->report.cleanup_attempted&&offset==0)return ~0u;
    if(cleanupReadFail&&active->report.stage==21&&offset==REG_WPR_LO)return 0xbadf0001;
    return *p;
}
static VOID fakeWrite(PULONG p,ULONG value) {
    ULONG offset=(ULONG)((PUCHAR)p-active->bar);writes++;
    if(writeCount<40000){writeRegs[writeCount]=offset;writeValues[writeCount++]=value;}
    if(offset==0x1103c0)gspResets++;
    if(offset==REG_CFG1||offset==REG_LMR)hostGeometry++;
    if(offset==REG_WPR_LO||offset==REG_WPR_HI)hostWpr++;
    if(offset==0x600&&pmcIgnore)return;
    if(offset==0x840700&&value==0x800000f1) {
        secureCommands++;
        if(!secureFail){*(PULONG)(active->bar+0x840244)=0x40000000;*p=value;return;}
        *p=value|0x1000;return;
    }
    if(graphicsFail&&offset==0x41a614)return;
    if(offset==REG_CFG1||offset==REG_LMR){geometryWrites++;if(offset==geometryIgnoreAddress)return;}
    if(offset==REG_WPR_LO||offset==REG_WPR_HI) {
        if(!(*(PULONG)(active->bar+0x840100)&16))wprWhileRunning++;
        if(*(PULONG)(active->bar+0x1fa7cc)!=0xfffff0ff||wprRestoreIgnore)return;
    }
    if(cleanupIgnore&&active->report.stage==21&&(offset==REG_WPR_LO||offset==REG_WPR_HI||offset==0x1180f8))return;
    if(offset==0x880a8&&tlsNeedsXve&&*(PULONG)(active->bar+0x8872c)!=6)return;
    *p=value;
    if(offset==0x8403c0||offset==0x1103c0) {
        *(PULONG)(active->bar+offset-0x3c0+0x10c)=resetFail?6:0;
        if(offset==0x8403c0&&active->report.mode==CMP_MEMORY) {
            *(PULONG)(active->bar+0x84010c)=resetFail?6:1;
            *(PULONG)(active->bar+0x840040)=0;*(PULONG)(active->bar+0x840044)=0;
            *(PULONG)(active->bar+0x840240)=0x3000;*(PULONG)(active->bar+0x840250)=0xf;
            *(PULONG)(active->bar+0x84027c)=0xff;*(PULONG)(active->bar+0x840280)=0xff;
            *(PULONG)(active->bar+0x840284)=0xff;*(PULONG)(active->bar+0x84028c)=0xff;
            *(PULONG)(active->bar+0x840290)=0xff;*(PULONG)(active->bar+0x8403c4)=0xff;
        }
        *(PULONG)(active->bar+offset-0x3c0+0x100)=16;
        *(PULONG)(active->bar+offset-0x3c0+0x118)=((dmaBusy&&offset==0x1103c0)||(sec2Busy&&offset==0x8403c0))?0:2;
    }
    if((offset==0x840100||offset==0x840130)&&value==2) {
        PUCHAR sig=(PUCHAR)active->buffer+4096;
        ULONG addr=*(PULONG)(sig+0xf76c),val=*(PULONG)(sig+0xf754);
        if(fireCount<4)fired[fireCount]=addr;
        fireCount++;
        if(active->report.mode==CMP_MEMORY) {
            unsigned i;ULONG nextValue=*(PULONG)(sig+0xe44c);
            for(i=0;i<21&&!fireFail;i++) {
                ULONG o=0xe468+i*0x30,reg=*(PULONG)(sig+o-8);
                if(reg==fireFailAddress)break;
                if(reg==REG_CFG1||reg==REG_LMR)geometryWrites++;
                if(reg!=geometryIgnoreAddress&&reg+4<=active->length)*(PULONG)(active->bar+reg)=nextValue;
                nextValue=*(PULONG)(sig+o+12);
            }
            *(PULONG)(active->bar+0x840100)=holdCpuRunning?0:16;
            *(PULONG)(active->bar+0x840118)=sec2Busy?0:2;
            *(PULONG)(active->bar+0x84004c)=queueBusy?1:0;
            return;
        }
        if(!fireFail&&addr!=fireFailAddress&&addr+4<=active->length)*(PULONG)(active->bar+addr)=val;
        *(PULONG)(active->bar+0x840040)=0x31;
        *(PULONG)(active->bar+0x840100)=holdCpuRunning?0:16;
        /* The payload establishes WPR even when later host writes are ignored. */
        *(PULONG)(active->bar+REG_WPR_LO)=0x01fa1000;
        *(PULONG)(active->bar+REG_WPR_HI)=0x01ffee00;
    }
}
static NTSTATUS fakeDelay(KPROCESSOR_MODE mode,BOOLEAN alert,PLARGE_INTEGER time) {
    (void)mode;(void)alert;(void)time;return STATUS_SUCCESS;
}
static VOID fakePut(PDMA_ADAPTER a) {(void)a;releases++;}
static PVOID fakeAlloc(PDMA_ADAPTER a,ULONG n,PPHYSICAL_ADDRESS logical,BOOLEAN cache) {
    (void)a;(void)cache;allocations++;logical->QuadPart=0x10000000;
    return allocationFail?NULL:calloc(1,n);
}
static VOID fakeFree(PDMA_ADAPTER a,ULONG n,PHYSICAL_ADDRESS logical,PVOID p,BOOLEAN cache) {
    if(active->report.mode==CMP_MEMORY&&!active->report.pcie_requested&&active->report.checks&CMP_CLEANUP_VERIFIED) {
        FILE *f=NULL;
        if(fopen_s(&f,"build/memory-dma.bin","wb")==0){fwrite(p,1,n,f);fclose(f);}
    }
    (void)a;(void)n;(void)logical;(void)cache;frees++;free(p);
}
static PDMA_ADAPTER fakeAdapter(PDEVICE_OBJECT p,PDEVICE_DESCRIPTION d,PULONG maps) {
    static DMA_OPERATIONS ops;static DMA_ADAPTER adapter;
    (void)p;(void)d;*maps=8192;ops.PutDmaAdapter=fakePut;
    ops.AllocateCommonBuffer=fakeAlloc;ops.FreeCommonBuffer=fakeFree;adapter.DmaOperations=&ops;return &adapter;
}
static ULONG fakeConfig(PVOID context,ULONG space,PVOID buffer,ULONG offset,ULONG length) {
    (void)context;(void)space;(void)offset;
    if(length!=2)return 0;*(USHORT*)buffer=pciCommand;return 2;
}
#undef READ_REGISTER_ULONG
#undef WRITE_REGISTER_ULONG
#define READ_REGISTER_ULONG fakeRead
#define WRITE_REGISTER_ULONG fakeWrite
#define KeDelayExecutionThread fakeDelay
#define KeStallExecutionProcessor(x) ((void)(x))
#define IoGetDmaAdapter fakeAdapter
#define WdfDeviceWdmGetPhysicalDevice(x) ((PDEVICE_OBJECT)(x))
