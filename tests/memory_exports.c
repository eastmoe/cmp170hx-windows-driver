#include "../src/memory_payload.h"
__declspec(dllexport) void memory_payload(u8 *b,u32 cfg,u32 lmr,u32 ss0,u32 ss1) {
    fill_memory_signature(b,cfg,lmr,ss0,ss1);
}
__declspec(dllexport) void memory_meta(u8 *b,u64 fb,u64 radix,u64 bl,u64 sig) {
    build_memory_meta(b,fb,radix,bl,sig);
}
