/* SPDX-License-Identifier: GPL-2.0-only
 * GA100 Gen2 experiment: cmpunlocker pcie-gen2.patch and CMP90 v3 recipe.
 * Reuse the recovered 21-write ABI and its WPR/exit tail, not GA102 firmware.
 */
#define PCIE_COUNT 18u
static const u32 pcie_regs[PCIE_COUNT]={
    0x8e1b0,0x8e1b4,0x8e1b8,0x8e1bc,0x88fe8,0x88fec,0x88ff0,
    0x8e120,0x8e110,0x8e12c,0x8e11c,0x8841c,0x8860c,
    0x88610,0x8c2c0,0x8c040,0x880a8,0x88084
};
/* VSEC_DEVICE is best effort in upstream and remained 0x800 on this board.
 * Ignore only bit 0; an inaccessible value or other corruption still fails. */
static u32 pcie_mask(unsigned i) {
    return i==12?0xfffffffeu:i==16?0x0000ffffu:i==17?0u:0xffffffffu;
}
static u32 pcie_value(unsigned i,u32 old) {
    if(i<7)return 0xffffffff;
    switch(i) {
    case 7:return 0;case 8:return 1;case 9:return 0x200000;case 10:return 4;
    case 11:return (old|0x2800u)&~0x5000u;
    case 12:return old|1u;
    case 13:return (old&~0x1000u)|1u;
    case 14:return old&~4u;
    case 15:return (old&~0xc0000u)|0x80000u;
    case 16:return (old&~15u)|2u|0x000f0000u;
    default:return old; /* LINK_CAP is observed, never fabricated. */
    }
}
static void fill_pcie_signature(u8 *b,const u32 *wanted,u32 cfg,u32 lmr) {
    RegisterWrite w[MEMORY_WRITE_COUNT];unsigned i,o;
    fill_memory_signature(b,cfg,lmr,0x88888888,8);
    memory_writes(w,cfg,lmr,0x88888888,8);
    for(i=0;i<13;i++){w[i].reg=pcie_regs[i];w[i].value=wanted[i];}
    /* First value remains FFFFFFFF; retain original marker and exit gadgets. */
    for(i=0;i<MEMORY_WRITE_COUNT;i++) {
        o=0xe468+i*0x30;put32(b,o-8,w[i].reg);
        put32(b,o+12,i+1<MEMORY_WRITE_COUNT?w[i+1].value:0);
    }
}
