"""Compare compiled C against the recovered original x64, with no GPU access."""
from pathlib import Path
import ctypes, importlib.util, json, random, struct, sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'research/emulation-python'))
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *
spec=importlib.util.spec_from_file_location('oracle',ROOT/'tools/extract-original-payload.py')
oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)
data=(ROOT/'analysis/emulated-original-fast/mapped-image.bin').read_bytes()
lib=ctypes.CDLL(str(ROOT/'build/memory_oracle.dll'))
lib.memory_payload.argtypes=[ctypes.c_void_p]+[ctypes.c_uint32]*4
lib.memory_meta.argtypes=[ctypes.c_void_p]+[ctypes.c_uint64]*4

def original_meta(fb,radix,bl,sig):
    base=oracle.BASE;buf=oracle.BUFFER;ctx=buf+0x1000;stack=oracle.STACK
    u=Uc(UC_ARCH_X86,UC_MODE_64);u.mem_map(base,(len(data)+4095)&~4095);u.mem_write(base,data)
    u.mem_map(buf,0x10000);u.mem_map(stack,0x10000)
    for off,value in [(0x38,buf),(0x158,fb),(0xa0,radix),(0x80,bl),(0x60,sig)]:
        u.mem_write(ctx+off,struct.pack('<Q',value))
    u.reg_write(UC_X86_REG_RBX,ctx);u.reg_write(UC_X86_REG_R14,0)
    u.reg_write(UC_X86_REG_RSP,stack+0xf008)
    def memset(uc,addr,size,unused):
        dest=uc.reg_read(UC_X86_REG_RCX);n=uc.reg_read(UC_X86_REG_R8)
        uc.mem_write(dest,bytes([uc.reg_read(UC_X86_REG_RDX)&255])*n)
        sp=uc.reg_read(UC_X86_REG_RSP);ret=struct.unpack('<Q',uc.mem_read(sp,8))[0]
        uc.reg_write(UC_X86_REG_RAX,dest);uc.reg_write(UC_X86_REG_RSP,sp+8);uc.reg_write(UC_X86_REG_RIP,ret)
    u.hook_add(UC_HOOK_CODE,memset,begin=base+0x6b80,end=base+0x6b80)
    u.emu_start(base+0x4182,base+0x4284,count=10000)
    assert u.reg_read(UC_X86_REG_RIP)==base+0x4284
    return bytes(u.mem_read(buf,256))

rng=random.Random(170)
cases=[(0x2779000,0x20b,0x88888888,8),(0x2669000,0x28a,0,0)]
cases += [tuple(rng.getrandbits(32) for _ in range(4)) for _ in range(30)]
for args in cases:
    expected,status=oracle.invoke(data,0x57f0,[oracle.BUFFER,0xf800,*args],0xf800)
    actual=ctypes.create_string_buffer(0xf800);lib.memory_payload(actual,*args)
    assert status==0 and actual.raw==expected,('payload mismatch',args)
meta_cases=[(8<<30,0x30000000,0x30001000,0x30002000),
            (10<<30,0x1234567800,0x2345678900,0x3456789000)]
for args in meta_cases:
    expected=original_meta(*args);actual=ctypes.create_string_buffer(256)
    lib.memory_meta(actual,*args)
    assert actual.raw==expected,('metadata mismatch',[(hex(i),x,y) for i,(x,y) in enumerate(zip(actual.raw,expected)) if x!=y])
report=dict(payload_cases=len(cases),metadata_cases=len(meta_cases),all_byte_equal=True,
            limitation='x64 data builders only, not Falcon or GPU hardware validation')
dma_path=ROOT/'build/memory-dma.bin'
if dma_path.exists():
    dma=dma_path.read_bytes()
    expected,status=oracle.invoke(data,0x57f0,[oracle.BUFFER,0xf800,0x2779000,0x20b,0x88888888,8],0xf800)
    assert status==0 and dma[4096:4096+0xf800]==expected,'production DMA payload differs from original x64'
    assert dma[:256]==original_meta(8<<30,0x10000000+18*4096,0x10000000+17*4096,0x10000000+4096),'production DMA metadata differs from original x64'
    report['production_dma_byte_equal']=True
(ROOT/'analysis/emulated-original-fast/c-oracle-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('PASS:',json.dumps(report))
