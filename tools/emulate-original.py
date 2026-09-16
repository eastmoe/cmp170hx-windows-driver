"""Offline x64 unpacking experiment. Guest code has NO host API forwarding.

The input is mapped only into Unicorn RAM. Unknown imports stop emulation.
No Windows driver load, certificate install, real MMIO, network or guest syscalls.
Recovered bytes and a machine-readable stop report are written for static review.
This is NOT a Windows/GPU hardware emulator and cannot validate an unlock.
"""
from pathlib import Path
import argparse, collections, hashlib, json, struct, sys, time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'research/emulation-python'))
import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_HOOK_MEM_WRITE
from unicorn.x86_const import *

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input',type=Path,default=ROOT.parent/'B站170HX Windows方案/170_boot_v3/170_boot.sys')
    ap.add_argument('--out',type=Path,default=ROOT/'analysis/emulated-original')
    ap.add_argument('--instructions',type=int,default=3000000)
    ap.add_argument('--slices',type=int,default=1,help='bounded 30-second emulation slices')
    ap.add_argument('--fast',action='store_true',help='Hook API/CRC boundaries only; no instruction trace/count')
    ap.add_argument('--probe-handover',action='store_true',help='After unpacking, run handover routines against modeled RAM registers')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    data=args.input.read_bytes();pe=pefile.PE(data=data)
    base=pe.OPTIONAL_HEADER.ImageBase;size=(pe.OPTIONAL_HEADER.SizeOfImage+4095)&~4095
    u=Uc(UC_ARCH_X86,UC_MODE_64);u.mem_map(base,size);u.mem_write(base,pe.get_memory_mapped_image())
    stack=0x10000000;u.mem_map(stack,0x200000)
    objects=0x20000000;u.mem_map(objects,0x10000)
    stubs=0x180000000;u.mem_map(stubs,0x10000)
    heap=0x30000000;u.mem_map(heap,0x2000000)
    stop=stubs+0xff00;u.mem_write(stop,b'\xc3')
    rsp=stack+0x1fff08;u.mem_write(rsp,struct.pack('<Q',stop));u.reg_write(UC_X86_REG_RSP,rsp)
    u.reg_write(UC_X86_REG_RCX,objects);u.reg_write(UC_X86_REG_RDX,objects+0x1000)
    # DRIVER_OBJECT fields used by common entry wrappers, entirely guest memory.
    u.mem_write(objects+0x18,struct.pack('<Q',base));u.mem_write(objects+0x20,struct.pack('<I',size))
    names={};calls=[];recent=collections.deque(maxlen=16);count=0;reason='instruction budget';cursor=heap
    accelerations=[];mdls={}
    modules=[];exports={}
    for mi,(dll,path) in enumerate([('ntoskrnl.exe',Path('C:/Windows/System32/ntoskrnl.exe')),
                                    ('HAL.dll',Path('C:/Windows/System32/hal.dll')),
                                    ('WDFLDR.SYS',Path('C:/Windows/System32/drivers/WdfLdr.sys'))]):
        mp=pefile.PE(str(path));mb=0x190000000+mi*0x4000000;ms=(mp.OPTIONAL_HEADER.SizeOfImage+4095)&~4095
        u.mem_map(mb,ms);u.mem_write(mb,mp.get_memory_mapped_image())
        modules.append((dll,mb,ms))
        for sym in mp.DIRECTORY_ENTRY_EXPORT.symbols:
            if not sym.name:continue
            name=sym.name.decode();ea=mb+sym.address;exports[dll.lower(),name]=ea
            sec=mp.get_section_by_rva(sym.address)
            if sec and sec.Characteristics&0x20000000:names[ea]=name
    module_info=bytearray(8+len(modules)*296);struct.pack_into('<I',module_info,0,len(modules))
    for mi,(dll,mb,ms) in enumerate(modules):
        off=8+mi*296;path=('\\SystemRoot\\system32\\'+dll).encode()
        struct.pack_into('<QQQIIHHHH',module_info,off,0,0,mb,ms,0,mi,mi,1,len(path)-len(dll))
        module_info[off+40:off+40+len(path)]=path
    for desc in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in desc.imports:
            name=(imp.name or b'ordinal').decode()
            addr=exports.get((desc.dll.decode().lower(),name))
            if addr is None:raise RuntimeError('Missing guest export '+name)
            u.mem_write(imp.address,struct.pack('<Q',addr))
    def ret(value=0):
        sp=u.reg_read(UC_X86_REG_RSP);target=struct.unpack('<Q',u.mem_read(sp,8))[0]
        u.reg_write(UC_X86_REG_RAX,value&0xffffffffffffffff)
        u.reg_write(UC_X86_REG_RSP,sp+8);u.reg_write(UC_X86_REG_RIP,target)
    def code(uc,addr,length,_):
        nonlocal count,reason,cursor
        count+=1;recent.append(hex(addr))
        # Byte-equivalent acceleration of the observed table-CRC loop. This
        # preserves the guest result; it does not bypass an integrity check.
        if addr==base+0x324bb9 and bytes(uc.mem_read(addr,4))==bytes.fromhex('410fb632'):
            n=uc.reg_read(UC_X86_REG_RBX);ptr=uc.reg_read(UC_X86_REG_R10)
            if 0<n<=0x4000000:
                tab=struct.unpack('<256I',uc.mem_read(uc.reg_read(UC_X86_REG_RCX),1024))
                d=uc.reg_read(UC_X86_REG_RDX)&0xffffffff;s=0
                for byte in uc.mem_read(ptr,n):s=tab[(byte^d)&255];d=(d>>8)^s^0x6d255bb9
                uc.reg_write(UC_X86_REG_RDX,d);uc.reg_write(UC_X86_REG_RSI,s)
                uc.reg_write(UC_X86_REG_R10,ptr+n);uc.reg_write(UC_X86_REG_RBX,0)
                f=uc.reg_read(UC_X86_REG_EFLAGS);uc.reg_write(UC_X86_REG_EFLAGS,(f&~0x8d5)|0x44)
                uc.reg_write(UC_X86_REG_RIP,base+0x324fff)
                accelerations.append(dict(kind='table CRC',bytes=n,address=hex(ptr)));return
        if addr==stop:reason='entry returned';uc.emu_stop();return
        if addr not in names:return
        name=names[addr];rcx=uc.reg_read(UC_X86_REG_RCX);rdx=uc.reg_read(UC_X86_REG_RDX)
        r8=uc.reg_read(UC_X86_REG_R8);r9=uc.reg_read(UC_X86_REG_R9)
        calls.append(dict(name=name,rcx=hex(rcx),rdx=hex(rdx),r8=hex(r8),r9=hex(r9)))
        if name=='ExAllocatePool':
            n=(rdx+4095)&~4095
            if n>0x1000000 or cursor+n>heap+0x2000000:reason='guest heap limit';uc.emu_stop();return
            p=cursor;cursor+=max(n,4096);ret(p)
        elif name=='NtQuerySystemInformation' and rcx==11:
            if r9:uc.mem_write(r9,struct.pack('<I',len(module_info)))
            if r8<len(module_info):ret(0xc0000004)
            else:uc.mem_write(rdx,bytes(module_info));ret()
        elif name=='IoAllocateMdl':
            p=cursor;cursor+=4096;mdls[p]=(rcx,rdx)
            uc.mem_write(p,struct.pack('<QHHIQQQII',0,48,0,0,0,rcx,rcx&~4095,rdx,rcx&4095));ret(p)
        elif name=='MmMapLockedPagesSpecifyCache' and rcx in mdls:ret(mdls[rcx][0])
        elif name in ('MmProbeAndLockPages','MmUnlockPages','IoFreeMdl','MmUnmapLockedPages','ExFreePoolWithTag','KeStallExecutionProcessor','KeSetSystemAffinityThread','KeRevertToUserAffinityThread','DbgPrint'):ret()
        elif name=='KeQueryActiveProcessors':ret(1)
        elif name=='KeQueryPerformanceCounter':
            if rcx:uc.mem_write(rcx,struct.pack('<Q',10000000))
            ret(count*100)
        else:reason='unmodeled import: '+name;uc.emu_stop()
    def invalid(uc,access,addr,n,value,_):
        nonlocal reason
        reason=f'unmapped guest access type={access} address={addr:#x} size={n}'
        return False
    if args.fast:
        u.hook_add(UC_HOOK_CODE,code,begin=base+0x324bb9,end=base+0x324bb9)
        u.hook_add(UC_HOOK_CODE,code,begin=stop,end=stop)
        for _,mb,ms in modules:u.hook_add(UC_HOOK_CODE,code,begin=mb,end=mb+ms-1)
    else:u.hook_add(UC_HOOK_CODE,code)
    u.hook_add(UC_HOOK_MEM_INVALID,invalid)
    started=time.monotonic()
    try:
        pc=base+pe.OPTIONAL_HEADER.AddressOfEntryPoint
        for part in range(args.slices):
            u.emu_start(pc,stop+1,timeout=30000000,count=args.instructions)
            pc=u.reg_read(UC_X86_REG_RIP)
            print(f'slice={part+1} instructions={count} rip={pc:#x} reason={reason}',flush=True)
            if reason!='instruction budget':break
    except UcError as e:reason+='; '+str(e)
    unpack_reason=reason;unpack_rip=hex(u.reg_read(UC_X86_REG_RIP))
    if args.probe_handover:
        # A separate synthetic experiment: actual recovered CPU routines,
        # invented MMIO responses. This does NOT exercise GPU reset hardware.
        bar=0x40000000;u.mem_map(bar,0x1000000)
        shared=0xfffff78000000000;u.mem_map(shared,0x1000)
        u.mem_write(shared+8,struct.pack('<Q',10000000))
        # Unicorn's no-paging address translation masks to 52 physical bits.
        shared_physical=shared&((1<<52)-1);u.mem_map(shared_physical,0x1000)
        u.mem_write(shared_physical+8,struct.pack('<Q',10000000))
        u.mem_write(objects+8,struct.pack('<Q',bar))
        u.mem_write(objects+0x18,struct.pack('<I',0x1000000))
        initial={0:0x170000a1,0x224fc:2<<20,0x22800:0x8d000000,0x22804:0x840002,
                 0x600:0x1004,0x84010c:1,0x840100:16,0x840118:2,
                 0x840244:0x40000000,0x1180f8:0x1234,
                 0x409614:0x110,0x41a614:0xa20,0x502614:0xa20,
                 0x41a610:1,0x502610:1,0x409240:0x3000,0x41a240:0x3000,0x502240:0x3000}
        for reg,value in initial.items():u.mem_write(bar+reg,struct.pack('<I',value))
        writes=[]
        def mmio_write(uc,access,addr,n,value,unused):
            writes.append(dict(reg=hex(addr-bar),value=hex(value),size=n,rip=hex(uc.reg_read(UC_X86_REG_RIP))))
            if addr==bar+0x840700 and value==0x800000f1:
                uc.mem_write(bar+0x840244,struct.pack('<I',0x40000000))
        u.hook_add(UC_HOOK_MEM_WRITE,mmio_write,begin=bar,end=bar+0xffffff)
        outcomes=[]
        for entry in (0x54a0,0x4ff0,0x52a0):
            if entry==0x52a0:
                u.mem_write(bar+0x840244,struct.pack('<I',0x80000000))
                u.mem_write(bar+0x840700,struct.pack('<I',0x1000))
            sp=stack+0x1fff08;u.mem_write(sp,struct.pack('<Q',stop))
            u.reg_write(UC_X86_REG_RSP,sp);u.reg_write(UC_X86_REG_RCX,objects)
            u.reg_write(UC_X86_REG_RDX,objects+0x1000)
            reason='handover probe budget';start_writes=len(writes)
            try:u.emu_start(base+entry,stop+1,timeout=5000000,count=1000000)
            except UcError as e:reason+='; '+str(e)
            outcomes.append(dict(entry=hex(entry),reason=reason,status=hex(u.reg_read(UC_X86_REG_EAX)),
                                 rip=hex(u.reg_read(UC_X86_REG_RIP)),writes=writes[start_writes:]))
            if reason!='entry returned':break
        (args.out/'handover-probe.json').write_text(json.dumps(dict(
            initial_registers={hex(k):hex(v) for k,v in initial.items()},outcomes=outcomes,
            limitation='Synthetic register responses, including modeled secure-command completion; code order only, not hardware behavior.'),indent=2),encoding='utf-8')
    image=bytes(u.mem_read(base,size));(args.out/'mapped-image.bin').write_bytes(image)
    sections=[]
    for s in pe.sections:
        chunk=image[s.VirtualAddress:s.VirtualAddress+s.Misc_VirtualSize]
        sections.append(dict(name=s.Name.rstrip(b'\0').decode('ascii','replace'),rva=hex(s.VirtualAddress),nonzero=sum(b!=0 for b in chunk),size=len(chunk)))
    report=dict(input=str(args.input),sha256=hashlib.sha256(data).hexdigest(),base=hex(base),
                instructions=None if args.fast else count,hook_events=count,elapsed=time.monotonic()-started,reason=reason,rip=hex(u.reg_read(UC_X86_REG_RIP)),
                unpack_reason=unpack_reason,unpack_rip=unpack_rip,
                recent=list(recent),imports=calls,sections=sections,accelerations=accelerations,
                limitation='Offline CPU emulation only. API results are modeled, not observed Windows behavior.')
    (args.out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
