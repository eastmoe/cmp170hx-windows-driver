"""Build the GA100 production Booter Unload image from pinned NVIDIA bindata."""
import hashlib
import re
import struct
import zlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / 'research/610.57.04/g_bindata_kgspGetBinArchiveBooterUnloadUcode_GA100.c'
assert hashlib.sha256(source.read_bytes()).hexdigest() == '14a5ec1455c5c4b29b001d9608e522a60c66a8310df667f76cc4c80edad9ade1'
text = source.read_text(encoding='utf-8')

def data(label, compressed=False):
    m = re.search(r'BINDATA_LABEL_' + label + r'_data\[\]\s*=\s*\{(.*?)\}', text, re.S)
    if not m:
        raise ValueError(label)
    raw = bytes.fromhex(''.join(re.findall(r'0x([0-9a-fA-F]{2})', m[1])))
    return zlib.decompress(raw, -15) if compressed else raw

header = struct.unpack('<9I', data('HEADER_PROD', True))
assert header == (0, 0x100, 0x4d00, 0x4f00, 1, 0x100, 0x4c00, 0x100, 0)
assert struct.unpack('<I', data('NUM_SIGS')) == (1,)
assert struct.unpack('<I', data('PATCH_SIG')) == (0,)
assert struct.unpack('<I', data('PATCH_LOC')) == (0x4f00,)
image = bytearray(data('IMAGE_PROD', True))
sig = data('SIG_PROD')
assert len(sig) == 384 and len(image) == 0x9c00
# NVIDIA s_patchBooterUcodeSignature selects signature zero for NUM_SIGS=1.
image[0x4f00:0x4f00 + len(sig)] = sig
digest = hashlib.sha256(image).hexdigest()
assert digest == '46c1397b90e3f781f54ba12e17bf5158d50b8df18722730131aa957e58a20ebb'
(root / 'vendor/ga100_unload.bin').write_bytes(image)
print(f'GA100 Booter Unload: {len(image)} bytes, sha256={digest}')
