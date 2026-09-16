/* Reconstructed from the user's 170_boot_v3 sample, pure data builders.
 * See analysis/emulated-original/payload-report.json for the x64 oracle.
 * Connected to the v0.9 Memory path; hardware acceptance remains unverified.
 */
#ifndef CMP_MEMORY_PAYLOAD_H
#define CMP_MEMORY_PAYLOAD_H
#include "core.h"
#define MEMORY_WRITE_COUNT 21u
#define MEMORY_FW_SIZE 0x1d09ea0u
static void memory_writes(RegisterWrite *w,u32 cfg,u32 lmr,u32 ss0,u32 ss1) {
    static const RegisterWrite fixed[MEMORY_WRITE_COUNT]={
        {0x9a0148,0xffffffff},{0x1fa7c4,0xffffffff},{0x823804,0xffffffff},
        {0x9a3c84,0xffffffff},{0x9a014c,0xffffffff},{0x9a3c7c,0xffffffff},
        {0x9a0168,0xffffffff},{0x9a3c80,0xffffffff},{0x1fa7f4,0xffffffff},
        {0x9a0204,0},{0x100ce0,0},{0x82381c,0},{0x823820,0},
        {0x1fa824,0},{0x1fa828,0},{0x1fa828,0},{0x1fa824,0x1ffffe00},
        {0x1fa814,0x08000000},{0x1fa818,0x08000000},
        {0x1fa7cc,0x0004cb8f},{0x14fc,0x4f4e0013}
    };
    unsigned i;for(i=0;i<MEMORY_WRITE_COUNT;i++)w[i]=fixed[i];
    w[9].value=cfg;w[10].value=lmr;w[11].value=ss0;w[12].value=ss1;
}
static void fill_memory_signature(u8 *b,u32 cfg,u32 lmr,u32 ss0,u32 ss1) {
    static const RegisterWrite edges[]={
        {0x1100,7},{0x5b40,0xc0deca7e},{0xf744,0},
        {0xf754,0x5331c0de},{0xf758,0xc0deca7e},{0xf75c,0xcbd},
        {0xf76c,0x14fc},{0xf774,0x1fbd},{0xf780,0xffffffff},
        {0xf788,0x10aa},{0xf78c,0x7934},{0xf790,0xffffffff},
        {0xe44c,0xffffffff},{0xe450,0xcbd},
        {0xe844,0x8e18},{0xe848,0xc0deca7e},{0xe84c,0x815a},
        {0xe850,0},{0xe854,0xc0deca7e},{0xe858,0x1fbd},
        {0xe864,0xf070},{0xe86c,0x582d},{0xe878,0xc0deca7e},
        {0xe87c,0xcbd},{0xe88c,3},{0xe894,0x1fbd},
        {0xe8a8,0xccb},{0xe8ac,0x7f2f}
    };
    RegisterWrite w[MEMORY_WRITE_COUNT];unsigned i,o;
    memory_writes(w,cfg,lmr,ss0,ss1);
    for(i=0;i<SIG_SIZE;i+=4)put32(b,i,0x4a7);
    for(i=0;i<MEMORY_WRITE_COUNT;i++) {
        o=0xe468+i*0x30;
        put32(b,o-8,w[i].reg);put32(b,o,0x1fbd);
        put32(b,o+12,i+1<MEMORY_WRITE_COUNT?w[i+1].value:0);
        put32(b,o+20,0x10aa);
        put32(b,o+24,i+1<MEMORY_WRITE_COUNT?0xcbd:0x815a);
    }
    for(i=0;i<sizeof(edges)/sizeof(edges[0]);i++)put32(b,edges[i].reg,edges[i].value);
}
/* Original builder RVA 0x4182..0x4280: stock FB, 105 MiB heap,
 * copied signature length 0xf7f0, and zero boot flags. */
static void build_memory_meta(u8 *b,u64 fb,u64 radix,u64 bl,u64 sig) {
    u64 top=(fb-MB)&~0x1ffffULL,boot=top-4096;
    u64 fw=(top-0x1d0aea0ULL)&~0xffffULL;
    u64 heap=(fw-0x6900000ULL)&~(MB-1);
    unsigned i;for(i=0;i<256;i++)b[i]=0;
    put64(b,0,0xdc3aae21371a60b3ULL);put64(b,8,1);
    put64(b,16,radix);put64(b,24,MEMORY_FW_SIZE);
    put64(b,32,bl);put64(b,40,4096);put64(b,72,sig);put64(b,80,0xf7f0);
    put64(b,88,heap-2*MB);put64(b,96,heap-2*MB);put64(b,104,MB);
    put64(b,112,heap-MB);put64(b,120,heap);put64(b,128,(fw-heap)&~(MB-1));
    put64(b,136,fw);put64(b,144,boot);put64(b,152,top);put64(b,168,top);
    put64(b,176,fb);put64(b,184,fb-MB);put64(b,192,MB);
}
#endif
