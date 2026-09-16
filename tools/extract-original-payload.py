"""Run only the recovered pure payload builder in isolated Unicorn memory.

No kernel API, firmware execution or GPU access. Outputs are analysis artifacts.
"""
from pathlib import Path
import hashlib, json, struct, sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'research/emulation-python'))
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64
from unicorn.x86_const import *

BASE, STACK, BUFFER, STOP = 0x140000000, 0x10000000, 0x20000000, 0x140006fff
OUT = ROOT / 'analysis/emulated-original'

def invoke(data, entry, args, size):
    u = Uc(UC_ARCH_X86, UC_MODE_64)
    u.mem_map(BASE, (len(data)+4095)&~4095)
    u.mem_write(BASE, data)
    u.mem_map(STACK, 0x10000)
    u.mem_map(BUFFER, 0x10000)
    sp = STACK + 0xf008
    u.reg_write(UC_X86_REG_RSP, sp)
    u.mem_write(sp, struct.pack('<Q', STOP))
    for reg, value in zip((UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9), args):
        u.reg_write(reg, value)
    for i, value in enumerate(args[4:]):
        u.mem_write(sp+0x28+i*8, struct.pack('<Q', value))
    u.emu_start(BASE+entry, STOP, count=500000)
    if u.reg_read(UC_X86_REG_RIP) != STOP:
        raise RuntimeError('Builder did not return')
    return bytes(u.mem_read(BUFFER, size)), u.reg_read(UC_X86_REG_EAX)

def reconstruct(writes):
    b = bytearray(struct.pack('<I', 0x4a7)* (0xf800//4))
    def put(o, v): struct.pack_into('<I', b, o, v)
    for o,v in [(0x1100,7),(0x5b40,0xc0deca7e),(0xf744,0),
                (0xf754,0x5331c0de),(0xf758,0xc0deca7e),(0xf75c,0xcbd),
                (0xf76c,0x14fc),(0xf774,0x1fbd),(0xf780,0xffffffff),
                (0xf788,0x10aa),(0xf78c,0x7934),(0xf790,0xffffffff),
                (0xe44c,0xffffffff),(0xe450,0xcbd)]: put(o,v)
    for i,(reg,value) in enumerate(writes):
        o=0xe468+i*0x30
        put(o-8,reg);put(o,0x1fbd)
        put(o+12,writes[i+1][1] if i+1<len(writes) else 0)
        put(o+20,0x10aa);put(o+24,0xcbd if i+1<len(writes) else 0x815a)
    for o,v in [(0xe844,0x8e18),(0xe848,0xc0deca7e),(0xe84c,0x815a),
                (0xe850,0),(0xe854,0xc0deca7e),(0xe858,0x1fbd),
                (0xe864,0xf070),(0xe86c,0x582d),(0xe878,0xc0deca7e),
                (0xe87c,0xcbd),(0xe88c,3),(0xe894,0x1fbd),
                (0xe8a8,0xccb),(0xe8ac,0x7f2f)]: put(o,v)
    return bytes(b)

def main():
    data=(OUT/'mapped-image.bin').read_bytes()
    results=[]
    for name,cfg,lmr,ss0,ss1 in [('20c2',0x2779000,0x20b,0,0),
                                ('variable-check',0x12345678,0x87654321,0xaabbccdd,0x11223344)]:
        table,_=invoke(data,0x5da0,[BUFFER,cfg,lmr,ss0,ss1],21*8)
        writes=list(struct.iter_unpack('<II',table))
        payload,status=invoke(data,0x57f0,[BUFFER,0xf800,cfg,lmr,ss0,ss1],0xf800)
        expected=reconstruct(writes)
        diff=[hex(i) for i,(x,y) in enumerate(zip(payload,expected)) if x!=y]
        result=dict(profile=name,status=hex(status),sha256=hashlib.sha256(payload).hexdigest(),
                    reconstructed_equal=not diff,differing_offsets=diff[:32],
                    writes=[dict(reg=f'{r:08x}',value=f'{v:08x}') for r,v in writes])
        results.append(result)
        (OUT/f'payload-{name}.bin').write_bytes(payload)
    report=dict(image_sha256=hashlib.sha256(data).hexdigest(),builder_rva='0x57f0',
                table_rva='0x5da0',results=results,
                limitation='Pure x64 builders only; no Falcon execution or hardware validation.')
    (OUT/'payload-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2))
    if not all(r['status']=='0x0' and r['reconstructed_equal'] for r in results):
        raise SystemExit(1)

if __name__=='__main__': main()
