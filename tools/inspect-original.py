"""Static analysis only: never loads or executes the input binaries."""
import hashlib, json, math, re, struct
from collections import Counter
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT.parent/'B站170HX Windows方案'/'170_boot_v3'
def inspect(path):
    data=path.read_bytes();pe=struct.unpack_from('<I',data,60)[0]
    machine,n,ts=struct.unpack_from('<HHI',data,pe+4)
    opt=pe+24;size=struct.unpack_from('<H',data,pe+20)[0]
    entry=struct.unpack_from('<I',data,opt+16)[0];sections=[]
    for i in range(n):
        off=opt+size+i*40
        name,vs,va,length,raw=struct.unpack_from('<8sIIII',data,off)
        chunk=data[raw:raw+length];counts=Counter(chunk)
        entropy=-sum((v/len(chunk))*math.log2(v/len(chunk)) for v in counts.values()) if chunk else 0
        sections.append(dict(name=name.rstrip(b'\0').decode('ascii','replace'),virtual_size=vs,rva=hex(va),raw_size=length,entropy=round(entropy,3)))
    strings=set(x.decode('ascii','replace') for x in re.findall(rb'[ -~]{7,}',data))
    strings.update(x.decode('utf-16le','replace') for x in re.findall(rb'(?:[ -~]\x00){7,}',data))
    selected=sorted(s for s in strings if re.search(r'cert|170_boot|ga100|setupdi|updateDriver|wdf|password|timestamp|https?://|\.pdb|\.cer|\.crt',s,re.I))
    return dict(file=path.name,size=len(data),sha256=hashlib.sha256(data).hexdigest(),machine=hex(machine),entry_rva=hex(entry),sections=sections,selected_strings=selected)
result=[inspect(SOURCE/n) for n in ('170_boot.sys','ga100ctl.exe','TestCert.exe')]
(ROOT/'analysis'/'static.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps([{k:r[k] for k in ('file','size','sha256','entry_rva')} for r in result],indent=2))
