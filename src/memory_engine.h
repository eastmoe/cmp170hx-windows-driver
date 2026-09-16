/* Original 170_boot_v3 Memory path. Included after MMIO/DMA helpers.
 * Evidence: analysis/ORIGINAL-MEMORY-FINDINGS.md and handover-disasm.txt.
 * No GSP reset and no host geometry/WPR writes on this path. */
static BOOLEAN memoryRead(Context *c,ULONG reg,ULONG *value) {
    *value=readReg(c,reg);
    return !c->ioFailed&&*value!=~0u;
}
static VOID memoryWrite(Context *c,ULONG reg,ULONG value) {
    writeReg(c,reg,value);readReg(c,reg); /* flush posted writes */
}
static BOOLEAN memoryResetBit(Context *c,ULONG *bit) {
    ULONG count,i,n=0,type=0,instance=0,resetBit=0,v;
    if(!memoryRead(c,0x224fc,&count))return FALSE;
    count>>=20;
    if(!count||count>(c->length-0x22800)/4)return FALSE;
    for(i=0;i<count;i++) {
        if(!memoryRead(c,0x22800+4*i,&v))return FALSE;
        if(!v&&!n)continue;
        if(n==0){type=(v>>24)&63;instance=(v>>16)&15;}
        if(n==1)resetBit=v&31;
        n++;
        if(v&0x80000000u)continue;
        if(type==13&&!instance&&n>=2){*bit=1u<<resetBit;return TRUE;}
        n=0;
    }
    /* Unlike the original's bit-2 fallback, reject an unknown table. */
    return FALSE;
}
static BOOLEAN memorySec2(Context *c) {
    ULONG bit,pmc,v,cmd,secure,i;
    if(!memoryResetBit(c,&bit)||!memoryRead(c,0x600,&pmc))return FALSE;
    memoryWrite(c,0x600,pmc&~bit);KeStallExecutionProcessor(20);
    if(!memoryRead(c,0x600,&v)||(v&bit))return FALSE;
    memoryWrite(c,0x600,pmc|bit);KeStallExecutionProcessor(20);
    if(!memoryRead(c,0x600,&v)||!(v&bit))return FALSE;
    memoryWrite(c,0x8403c0,1);for(i=0;i<11;i++)readReg(c,0x8403c0);
    memoryWrite(c,0x8403c0,0);for(i=0;i<11;i++)readReg(c,0x8403c0);
    if(!poll(c,0x84010c,6,0)||!memoryRead(c,0,&v))return FALSE;
    memoryWrite(c,0x840084,v);
    if(!memoryRead(c,0x840700,&cmd)||!memoryRead(c,0x840244,&secure))return FALSE;
    if((cmd&0x7000)||(secure&0x80000000u)) {
        memoryWrite(c,0x840704,0x200);memoryWrite(c,0x840700,0x800000f1);
        if(!poll(c,0x840700,0x7000,0))return FALSE;
    }
    if(!memoryRead(c,0x840244,&secure)||(secure&0x80000000u))return FALSE;
    memoryWrite(c,0x840618,0);
    if(!memoryRead(c,0x1180f8,&v))return FALSE;
    v&=0xfffff;memoryWrite(c,0x1180f8,v);
    return poll(c,0x840618,~0u,0)&&poll(c,0x1180f8,~0u,v);
}
static BOOLEAN memoryGraphics(Context *c) {
    ULONG pmc,fecs,gpccs;
    if(!memoryRead(c,0x600,&pmc)||!memoryRead(c,0x409614,&fecs)||
       !memoryRead(c,0x41a614,&gpccs))return FALSE;
    memoryWrite(c,0x600,pmc&~0x1000u);memoryWrite(c,0x41a610,0);KeStallExecutionProcessor(1);
    memoryWrite(c,0x41a610,1);memoryWrite(c,0x600,pmc|0x1000);KeStallExecutionProcessor(1);
    memoryWrite(c,0x409614,(fecs&0xfffffefe)|0x10);
    memoryWrite(c,0x41a614,(gpccs&0xfffff5fd)|0x20);KeStallExecutionProcessor(3);
    memoryWrite(c,0x409614,(fecs&0xfffffffe)|0x110);
    memoryWrite(c,0x41a614,(gpccs&0xfffffffd)|0xa20);KeStallExecutionProcessor(3);
    return poll(c,0x600,0x1000,0x1000)&&poll(c,0x409614,0x111,0x110)&&
        poll(c,0x41a610,1,1)&&poll(c,0x41a614,0xa22,0xa20)&&
        poll(c,0x502610,1,1)&&poll(c,0x502614,0xa22,0xa20)&&
        poll(c,0x409240,~0u,0x3000)&&poll(c,0x41a240,~0u,0x3000)&&poll(c,0x502240,~0u,0x3000);
}
static BOOLEAN memoryStop(Context *c) {
    BOOLEAN a=memorySec2(c),b;
    if(a)a=poll(c,0x840100,16,16)&&poll(c,0x840118,3,2)&&poll(c,0x84004c,0xffff,0);
    /* Conservative read-only GSP DMA guard. Do not reuse the Compute reset. */
    b=poll(c,0x11010c,6,0)&&poll(c,0x110100,16,16)&&
      poll(c,0x110118,3,2)&&poll(c,0x111240,1,0);
    c->report.stop_final[0]=readReg(c,0x84010c);c->report.stop_final[1]=readReg(c,0x11010c);
    c->report.stop_cpu[0]=readReg(c,0x840100);c->report.stop_cpu[1]=readReg(c,0x110100);
    c->report.stop_dma[0]=readReg(c,0x840118);c->report.stop_dma[1]=readReg(c,0x110118);
    c->report.stop_gsp_riscv=readReg(c,0x111240);
    c->report.checks&=~(CMP_SEC2_STOPPED|CMP_GSP_STOPPED);
    if(a&&!c->ioFailed)c->report.checks|=CMP_SEC2_STOPPED;
    if(b&&!c->ioFailed)c->report.checks|=CMP_GSP_STOPPED;
    c->quiescent=a&&b&&!c->ioFailed;
    trace("memory.dma-quiescent",c->report.stage,c->quiescent?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.checks);
    return c->quiescent;
}
static BOOLEAN memoryTargets(Context *c) {
    RegisterWrite w[MEMORY_WRITE_COUNT];ULONG i,j,want,got;BOOLEAN ok=TRUE;
    memory_writes(w,c->profile->cfg1,c->profile->lmr,0x88888888,8);
    for(i=0;i<MEMORY_WRITE_COUNT;i++) {
        want=w[i].value;
        for(j=i+1;j<MEMORY_WRITE_COUNT;j++)if(w[j].reg==w[i].reg)want=w[j].value;
        got=readReg(c,w[i].reg);
        if(got!=want){ok=FALSE;trace("memory.target-mismatch.register",c->report.stage,STATUS_DEVICE_HARDWARE_ERROR,w[i].reg);
            trace("memory.target-mismatch.value",c->report.stage,STATUS_DEVICE_HARDWARE_ERROR,got);}
    }
    return ok&&!c->ioFailed;
}
static BOOLEAN memoryState(Context *c) {
    /* Original final state at RVA 0x4541..0x468e plus graphics checks. */
    static const RegisterWrite expected[]={
        {0x840100,0x10},{0x840040,0},{0x840044,0},{0x840240,0x3000},
        {0x840244,0x40000000},{0x840250,0xf},{0x8403c0,0},{0x84010c,1},{0x840118,2},
        {0x409240,0x3000},{0x41a240,0x3000},{0x502240,0x3000},{0x840618,0},
        {0x84027c,0xff},{0x840280,0xff},{0x840284,0xff},{0x84028c,0xff},{0x840290,0xff},
        {0x840804,0},{0x840808,0},{0x84080c,0},{0x840810,0},{0x50227c,0}
    };
    ULONG i,v;BOOLEAN ok=TRUE;
    for(i=0;i<RTL_NUMBER_OF(expected);i++) {
        if(!memoryRead(c,expected[i].reg,&v)||v!=expected[i].value) {
            ok=FALSE;trace("memory.final-state.register",c->report.stage,STATUS_DEVICE_HARDWARE_ERROR,expected[i].reg);
            trace("memory.final-state.value",c->report.stage,STATUS_DEVICE_HARDWARE_ERROR,v);
        }
    }
    if(!memoryRead(c,0x8403c4,&v)||(v!=0xff&&v!=0x9f))ok=FALSE;
    return ok&&poll(c,0x840700,0x7000,0)&&poll(c,0x84004c,0xffff,0)&&
        poll(c,0x600,0x1000,0x1000)&&poll(c,0x409614,0x111,0x110)&&
        poll(c,0x41a610,1,1)&&poll(c,0x41a614,0xa22,0xa20)&&
        poll(c,0x502610,1,1)&&poll(c,0x502614,0xa22,0xa20);
}
static NTSTATUS runMemory(WDFDEVICE dev,Context *c) {
    ULONG i,bit,npages=(MEMORY_FW_SIZE+4095)/4096,leaves=(npages+511)/512;
    ULONGLONG base,*root,*middle,*leaf;PUCHAR p;NTSTATUS status;
    BOOLEAN ok=FALSE,stopped,graphics=FALSE;
    if(!memoryResetBit(c,&bit))return STATUS_NOT_SUPPORTED;
    c->bytes=(23+leaves)*4096;status=allocateDma(dev,c);
    if(!NT_SUCCESS(status))return status;
    p=c->buffer;base=(ULONGLONG)c->logical.QuadPart;
    root=(void*)(p+18*4096);middle=(void*)(p+19*4096);leaf=(void*)(p+20*4096);
    root[0]=base+19*4096;
    for(i=0;i<leaves;i++)middle[i]=base+(20+i)*4096;
    /* Three physical pages: zero, logical page 0, logical page 18. */
    for(i=0;i<npages;i++)leaf[i]=base+(20+leaves)*4096;
    leaf[0]=base+(21+leaves)*4096;leaf[18]=base+(22+leaves)*4096;
    RtlCopyMemory(p+(21+leaves)*4096,ga100_memory_pages,8192);
    RtlCopyMemory(p+17*4096,ga100_bl,4096);
    build_memory_meta(p,c->profile->stock_bytes,base+18*4096,base+17*4096,base+4096);
    fill_memory_signature(p+4096,c->profile->cfg1,c->profile->lmr,0x88888888,8);
    c->attempted=TRUE;c->report.stage=2;
    trace("memory.single-chain.begin",2,STATUS_SUCCESS,c->bytes);
    if(!memorySec2(c))goto done;
    c->report.stage=20;memoryWrite(c,0x14fc,0);
    if(!poll(c,0x14fc,~0u,0)||!fire(c,0x14fc,0x4f4e0013))goto done;
    /* Marker precedes the exit tail. Drain before post-payload reset. */
    if(!poll(c,0x840118,3,2)||!poll(c,0x84004c,0xffff,0))goto done;
    ok=memoryTargets(c);
    if(ok)c->report.checks|=CMP_TARGET_MATCHED;
done:
    c->report.stage=21;
    for(i=0;i<3;i++)c->report.cleanup_before[i]=readReg(c,cleanupRegs[i]);
    stopped=memoryStop(c);c->report.cleanup_attempted=1;
    if(stopped)graphics=memoryGraphics(c);
    /* Graphics writes clear quiescent; re-observe DMA, never infer from marker. */
    c->quiescent=stopped&&poll(c,0x840100,16,16)&&poll(c,0x840118,3,2)&&
        poll(c,0x84004c,0xffff,0)&&poll(c,0x110100,16,16)&&poll(c,0x110118,3,2)&&poll(c,0x111240,1,0)&&!c->ioFailed;
    for(i=0;i<3;i++)c->report.cleanup_after[i]=readReg(c,cleanupRegs[i]);
    c->report.cleanup_mismatch=(c->report.cleanup_after[0]!=0x1ffffe00?1u:0u)|
        (c->report.cleanup_after[1]!=0?2u:0u)|(c->report.cleanup_after[2]&0xfff00000?4u:0u);
    if(graphics&&c->quiescent&&!c->report.cleanup_mismatch&&memoryTargets(c)&&memoryState(c))c->report.checks|=CMP_CLEANUP_VERIFIED;
    ok=ok&&(c->report.checks&CMP_CLEANUP_VERIFIED)!=0;
    snapshot(c,c->report.after);freeDma(c);
    ok=ok&&!c->ioFailed&&(c->report.checks&CMP_DMA_RELEASED)!=0;
    c->report.state=ok?1:2;if(ok)c->report.stage=30;
    trace("memory.single-chain.end",c->report.stage,ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.checks);
    return ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR;
}
