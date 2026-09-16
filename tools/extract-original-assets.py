"""Extract and compare already-unpacked data; does not execute a driver."""
from pathlib import Path
import hashlib,json,struct
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'analysis/emulated-original-fast'
b=(OUT/'mapped-image.bin').read_bytes()
sha=lambda x:hashlib.sha256(x).hexdigest()
s=b[0x402058:0x402058+0x2100]
assert s[:8]==b'GA100SP1'
version,page_size,fw_size,count,encoding=struct.unpack_from('<IIQII',s,8)
assert (version,page_size,fw_size,count,encoding)==(1,4096,0x1d09ea0,2,4)
pages=struct.unpack_from('<2I',s,0x40)
assert pages==(0,18)
fw=bytearray(fw_size)
for j,index in enumerate(pages):fw[index*4096:(index+1)*4096]=s[0x100+j*4096:0x1100+j*4096]
(OUT/'sparse-fw.bin').write_bytes(s)
(OUT/'reconstructed-sparse-fw.bin').write_bytes(fw)
assets=[]
for name,rva,size in [('ga100_booter',0x72e0,60160),('ga100_bl',0x16de0,4096)]:
    recovered=b[rva:rva+size];ours=(ROOT/f'vendor/{name}.bin').read_bytes()
    assets.append(dict(name=name,rva=hex(rva),bytes=size,sha256=sha(recovered),equal_to_vendor=recovered==ours))
    assert recovered==ours,name
report=dict(input_sys_sha256=json.loads((OUT/'report.json').read_text())['sha256'],
            mapped_image_sha256=sha(b),assets=assets,
            sparse_resource_sha256=sha(s),firmware_size=fw_size,pages=list(pages),
            reconstructed_sparse_sha256=sha(fw),
            limitation='The sparse image is an analysis reconstruction, not a complete runnable GSP firmware.')
(OUT/'assets-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
