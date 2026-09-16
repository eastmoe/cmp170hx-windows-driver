/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/core.h"
__declspec(dllexport) int test_profile(u16 vendor,u16 device) {
    const Profile *p=profile_for(vendor,device); return p?(int)(p->stock_bytes>>30):0;
}
__declspec(dllexport) void test_payload(u8 *buffer,u32 addr,u32 value) {fill_signature(buffer,addr,value);}
__declspec(dllexport) int test_meta(u8 *buffer,u64 fb,u64 top,u64 radix,u64 fwsize,u64 bl,u64 sig) {
    return build_meta(buffer,fb,top,radix,fwsize,bl,sig);
}
