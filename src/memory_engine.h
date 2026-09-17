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
/* Readback is a configuration result, not proof of negotiated Gen2. */
static BOOLEAN pcieVerify(Context *c) {
    ULONG i,v;BOOLEAN ok=TRUE;
    for(i=0;i<PCIE_COUNT;i++) {
        v=readReg(c,pcie_regs[i]);c->report.pcie_after[i]=v;
        if(v==~0u&&i>=7)ok=FALSE;
        if((v^c->report.pcie_wanted[i])&pcie_mask(i)) {
            ok=FALSE;trace("pcie.mismatch.register",c->report.stage,STATUS_DEVICE_HARDWARE_ERROR,pcie_regs[i]);
            trace("pcie.mismatch.value",c->report.stage,STATUS_DEVICE_HARDWARE_ERROR,v);
        }
        if(i==12&&v!=c->report.pcie_wanted[i])trace("pcie.VSEC_DEVICE.optional-bit0",c->report.stage,STATUS_SUCCESS,v);
    }
    c->report.pcie_link_status=readReg(c,0x88088);
    if(c->report.pcie_link_status==~0u||c->report.pcie_after[17]==~0u)ok=FALSE;
    trace("pcie.link-status",c->report.stage,STATUS_SUCCESS,c->report.pcie_link_status);
    return ok&&!c->ioFailed;
}
static BOOLEAN pciePrepare(Context *c) {
    ULONG i,v;
    for(i=0;i<PCIE_COUNT;i++) {
        v=readReg(c,pcie_regs[i]);c->report.pcie_before[i]=v;
        if(c->ioFailed||(i>=7&&v==~0u))return FALSE;
        c->report.pcie_wanted[i]=pcie_value(i,v);
    }
    return TRUE;
}
static BOOLEAN pcieHost(Context *c) {
    ULONG i;
    for(i=13;i<17;i++) {
        memoryWrite(c,pcie_regs[i],c->report.pcie_wanted[i]);
        trace("pcie.host.register",c->report.stage,STATUS_SUCCESS,pcie_regs[i]);
        trace("pcie.host.readback",c->report.stage,STATUS_SUCCESS,readReg(c,pcie_regs[i]));
    }
    /* GA100 patch 0008's XVE override is required on some cards before TLS
     * accepts Gen2. One bounded fallback only, no upstream bridge access. */
    if(!c->ioFailed&&(readReg(c,0x880a8)&15)==1) {
        ULONG xve=readReg(c,0x8872c);
        trace("pcie.XVE.before",c->report.stage,STATUS_SUCCESS,xve);
        if(c->ioFailed||xve==~0u)return FALSE;
        for(i=0;i<16;i++)if(((readReg(c,pcie_regs[i])^c->report.pcie_wanted[i])&pcie_mask(i))||c->ioFailed)return FALSE;
        memoryWrite(c,0x8872c,6);
        for(i=0;i<50;i++)pauseMs();
        trace("pcie.XVE.after",c->report.stage,STATUS_SUCCESS,readReg(c,0x8872c));
        if(c->ioFailed||readReg(c,0)==~0u)return FALSE;
        memoryWrite(c,0x880a8,c->report.pcie_wanted[16]);
    }
    c->report.pcie_configured=pcieVerify(c)?1u:0u;
    return c->report.pcie_configured!=0;
}
static BOOLEAN pcieIdle(Context *c) {
    c->report.stop_final[0]=readReg(c,0x84010c);c->report.stop_final[1]=readReg(c,0x11010c);
    c->report.stop_cpu[0]=readReg(c,0x840100);c->report.stop_cpu[1]=readReg(c,0x110100);
    c->report.stop_dma[0]=readReg(c,0x840118);c->report.stop_dma[1]=readReg(c,0x110118);
    c->report.stop_gsp_riscv=readReg(c,0x111240);
    return !c->ioFailed&&c->report.stop_final[0]!=~0u&&c->report.stop_final[1]!=~0u&&
        !(c->report.stop_final[0]&6)&&!(c->report.stop_final[1]&6)&&
        c->report.stop_cpu[0]==16&&c->report.stop_cpu[1]==16&&
        c->report.stop_dma[0]==2&&c->report.stop_dma[1]==2&&
        c->report.stop_gsp_riscv==0&&readReg(c,0x84004c)==0;
}
/* Resume only already-complete Memory + PCIe privileged state. No allocation,
 * payload launch, engine reset or memory geometry write on this path. */
static NTSTATUS runPcieResume(Context *c) {
    ULONG i;BOOLEAN ok;
    if(c->profile->device!=0x20c2||c->buffer||!memoryTargets(c)||!memoryState(c)||
       !pcieIdle(c)||!pciePrepare(c))return STATUS_INVALID_DEVICE_STATE;
    for(i=0;i<12;i++)if(c->report.pcie_before[i]!=c->report.pcie_wanted[i])return STATUS_INVALID_DEVICE_STATE;
    c->attempted=TRUE;c->report.stage=23;
    for(i=0;i<3;i++)c->report.cleanup_before[i]=readReg(c,cleanupRegs[i]);
    ok=pcieHost(c);
    c->quiescent=pcieIdle(c);
    if(c->quiescent)c->report.checks|=CMP_SEC2_STOPPED|CMP_GSP_STOPPED;
    for(i=0;i<3;i++)c->report.cleanup_after[i]=readReg(c,cleanupRegs[i]);
    c->report.cleanup_mismatch=(c->report.cleanup_after[0]!=0x1ffffe00?1u:0u)|
        (c->report.cleanup_after[1]!=0?2u:0u)|(c->report.cleanup_after[2]&0xfff00000?4u:0u);
    ok=ok&&c->quiescent&&!c->report.cleanup_mismatch&&memoryTargets(c)&&memoryState(c);
    /* No DMA was allocated; ownership is empty, independently observed idle. */
    c->report.checks|=CMP_DMA_RELEASED;
    if(ok)c->report.checks|=CMP_TARGET_MATCHED|CMP_CLEANUP_VERIFIED;
    snapshot(c,c->report.after);ok=ok&&!c->ioFailed;
    c->report.state=ok?1:2;if(ok)c->report.stage=30;
    trace("pcie.resume.no-payload",c->report.stage,ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.checks);
    return ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR;
}
static BOOLEAN pcieApply(Context *c) {
    ULONG i;
    /* Stop/drain the first launch before touching its shared DMA signature. */
    if(!memoryStop(c))return FALSE;
    fill_pcie_signature((u8*)c->buffer+4096,(const u32*)c->report.pcie_wanted,c->profile->cfg1,c->profile->lmr);
    if(!memorySec2(c))return FALSE;
    c->report.stage=22;memoryWrite(c,0x14fc,0);
    if(!poll(c,0x14fc,~0u,0)||!fire(c,0x14fc,0x4f4e0013)||
       !poll(c,0x840118,3,2)||!poll(c,0x84004c,0xffff,0)||!memoryStop(c))return FALSE;
    for(i=0;i<13;i++)if(((readReg(c,pcie_regs[i])^c->report.pcie_wanted[i])&pcie_mask(i))||c->ioFailed)return FALSE;
    /* No PL_LINK_RATE constant, LTSSM kick, endpoint RL, or upstream writes.
     * NVIDIA/PnP may train at handover; post-bind acceptance checks real speed. */
    return pcieHost(c);
}
static NTSTATUS runMemory(WDFDEVICE dev,Context *c) {
    ULONG i,bit,npages=(MEMORY_FW_SIZE+4095)/4096,leaves=(npages+511)/512;
    ULONGLONG base,*root,*middle,*leaf;PUCHAR p;NTSTATUS status;
    BOOLEAN ok=FALSE,stopped,graphics=FALSE;
    if(!memoryResetBit(c,&bit))return STATUS_NOT_SUPPORTED;
    if(c->report.pcie_requested&&!pciePrepare(c))return STATUS_DEVICE_HARDWARE_ERROR;
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
    if(ok&&c->report.pcie_requested)ok=pcieApply(c);
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
    if(graphics&&c->quiescent&&!c->report.cleanup_mismatch&&memoryTargets(c)&&memoryState(c)&&(!c->report.pcie_requested||pcieVerify(c)))c->report.checks|=CMP_CLEANUP_VERIFIED;
    ok=ok&&(c->report.checks&CMP_CLEANUP_VERIFIED)!=0;
    snapshot(c,c->report.after);freeDma(c);
    ok=ok&&!c->ioFailed&&(c->report.checks&CMP_DMA_RELEASED)!=0;
    c->report.state=ok?1:2;if(ok)c->report.stage=30;
    trace("memory.single-chain.end",c->report.stage,ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR,c->report.checks);
    return ok?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR;
}
