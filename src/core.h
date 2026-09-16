/* SPDX-License-Identifier: GPL-2.0-only
 * GA100 payload offsets from cmpunlocker/sec2-postbl-plm-ss-cfg.patch.
 * Metadata layout and boot sequencing from NVIDIA open-gpu-kernel-modules.
 */
#ifndef CMP_CORE_H
#define CMP_CORE_H
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
#define SIG_SIZE 0xf800U
#define MB 0x100000ULL
#define REG_SS0 0x82381cU
#define REG_SS1 0x823820U
#define REG_FEAT 0x823804U
#define REG_CFG1 0x9a0204U
#define REG_LMR 0x100ce0U
#define REG_WPR_LO 0x1fa824U
#define REG_WPR_HI 0x1fa828U
typedef struct { u32 reg, value; } RegisterWrite;
/* WPR permissions are required for per-fire and final WPR restoration even
 * when memory geometry is unchanged. Preserve upstream relative order. */
static const RegisterWrite compute_plms[] = {
    {0x1fa7cc, 0xfffff0ff}, {0x1fa7c4, 0xffffffff}, {0x823804, 0xffffffff}
};
static const RegisterWrite memory_plms[] = {
    {0x1fa7cc, 0xfffff0ff}, {0x9a0148, 0xffffffff},
    {0x1fa7c4, 0xffffffff}, {0x823804, 0xffffffff}
};
typedef struct { u16 device; u64 stock_bytes; u32 cfg1, lmr; } Profile;
static const Profile profiles[] = {
    {0x20c2, 8ULL << 30, 0x02779000, 0x20b},
    {0x2082, 10ULL << 30, 0x02669000, 0x28a}
};
static const Profile *profile_for(u16 vendor, u16 device) {
    unsigned i;
    if (vendor != 0x10de) return 0;
    for (i=0;i<2;i++) if (profiles[i].device == device) return profiles+i;
    return 0;
}
static void put32(u8 *b, u32 o, u32 v) {
    b[o]=(u8)v; b[o+1]=(u8)(v>>8); b[o+2]=(u8)(v>>16); b[o+3]=(u8)(v>>24);
}
static void fill_signature(u8 *b, u32 addr, u32 value) {
    static const RegisterWrite chain[] = {
        {0x1100,7}, {0x5b40,0xc0deca7e}, {0xf758,0xc0deca7e},
        {0xf75c,0xcbd}, {0xf774,0x1fbd}, {0xf780,0}, {0xf788,0x10aa},
        {0xf78c,0x815a}, {0xf790,0x8e18}, {0xf794,0xc0deca7e},
        {0xf798,0x815a}, {0xf79c,0}, {0xf7a0,0xc0deca7e},
        {0xf7a4,0x1fbd}, {0xf7b0,0xffbc}, {0xf7b8,0x582d},
        {0xf7c4,0xc0deca7e}, {0xf7c8,0xcbd}, {0xf7d8,3},
        {0xf7e0,0x1fbd}, {0xf7f4,0xccb}, {0xf7f8,0x7f2f}
    };
    unsigned i;
    for(i=0;i<SIG_SIZE;i+=4) put32(b,i,0x4a7);
    for(i=0;i<sizeof(chain)/sizeof(chain[0]);i++) put32(b,chain[i].reg,chain[i].value);
    put32(b,0xf754,value); put32(b,0xf76c,addr);
}
/* Same ABI as GspFwWprMeta. Explicit byte offsets avoid host packing ambiguity. */
static void put64(u8 *b, unsigned o, u64 v) { put32(b,o,(u32)v); put32(b,o+4,(u32)(v>>32)); }
static int build_meta(u8 *b, u64 fb, u64 reserved_top, u64 radix, u64 fwsize,
                      u64 bl, u64 sig) {
    u64 top, boot, fw, heap, start;
    unsigned i;
    if (fb < 8ULL<<30 || !fwsize || fwsize > 64*MB || reserved_top > fb ||
        reserved_top < fb-128*MB) return 0;
    top = reserved_top & ~0x1ffffULL;
    boot = (top-4096) & ~0xfffULL;
    fw = (boot-fwsize) & ~0xffffULL;
    /* Explicit 64 MiB minimum heap (GA100 supported override); no GSP-RM is
     * intentionally started here. Keep the entire layout in pre-scrubbed FB. */
    heap = (fw-64*MB) & ~(MB-1);
    start = heap-MB;
    if (fb-(start-MB) > 256*MB) return 0;
    for(i=0;i<256;i++) b[i]=0;
    put64(b,0,0xdc3aae21371a60b3ULL); put64(b,8,1);
    put64(b,16,radix); put64(b,24,fwsize); put64(b,32,bl); put64(b,40,4096);
    /* GA100 descriptor monitorCode/Data/Manifest offsets are all zero. */
    put64(b,72,sig); put64(b,80,SIG_SIZE);
    put64(b,88,start-MB); put64(b,96,start-MB); put64(b,104,MB);
    put64(b,112,start); put64(b,120,heap); put64(b,128,(fw-heap)&~(MB-1));
    put64(b,136,fw); put64(b,144,boot); put64(b,152,top);
    put64(b,160,0); put64(b,168,top); put64(b,176,fb);
    put64(b,184,fb-MB); put64(b,192,MB);
    b[241]=1;
    return 1;
}
#endif
