"""Rebuild read-only COFF blobs from pinned firmware inputs."""
import hashlib,json,struct
from pathlib import Path
root=Path(__file__).resolve().parents[1]
manifest=json.loads((root/'vendor/firmware-manifest.json').read_text())
for name,info in manifest['blobs'].items():
    data=(root/'vendor'/f'{name}.bin').read_bytes()
    if len(data)!=info['bytes'] or hashlib.sha256(data).hexdigest()!=info['sha256']:
        raise ValueError(f'{name}: firmware hash or length mismatch')
    symbol=name.encode()+b'\0';offset=60+len(data)
    header=struct.pack('<HHIIIHH',0x8664,1,0,offset,1,0,0)
    section=struct.pack('<8sIIIIIIHHI',b'.rdata\0\0',0,0,len(data),60,0,0,0,0,0x40500040)
    sym=struct.pack('<IIIhHBB',0,4,0,1,0,2,0)
    (root/'vendor'/f'{name}.obj').write_bytes(header+section+data+sym+struct.pack('<I',4+len(symbol))+symbol)
    print(f'{name}: pinned input hash verified, COFF generated')
