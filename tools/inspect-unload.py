"""Offline inspection only; the separate 0.4 extractor/build links this firmware."""
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path

folder = Path(__file__).resolve().parents[1] / 'research' / '610.57.04'
source = folder / 'g_bindata_kgspGetBinArchiveBooterUnloadUcode_GA100.c'
text = source.read_text(encoding='utf-8')


def data(label, compressed=False):
    match = re.search(r'BINDATA_LABEL_' + label + r'_data\[\]\s*=\s*\{(.*?)\}', text, re.S)
    if not match:
        raise ValueError('Missing bindata label: ' + label)
    raw = bytes.fromhex(''.join(re.findall(r'0x([0-9a-fA-F]{2})', match[1])))
    return zlib.decompress(raw, -15) if compressed else raw


image = bytearray(data('IMAGE_PROD', True))
header = struct.unpack('<9I', data('HEADER_PROD', True))
loc, = struct.unpack('<I', data('PATCH_LOC'))
sigoff, = struct.unpack('<I', data('PATCH_SIG'))
signature = data('SIG_PROD')
assert len(signature) == 384 and sigoff == 0
assert header[4] == 1 and header[0] + header[1] <= len(image)
assert header[2] + header[3] <= len(image)
assert header[5] + header[6] <= len(image)
assert header[2] <= loc and loc + len(signature) <= header[2] + header[3]
image[loc:loc + len(signature)] = signature
result = {
    'purpose': 'offline structure inspection; 0.4 extract-unload.py builds the same pinned production image',
    'source_url': 'https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/610.57.04/src/nvidia/generated/' + source.name,
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'header_words': [hex(x) for x in header],
    'image_bytes': len(image),
    'signature_bytes': len(signature),
    'signature_patch_offset': hex(loc),
    'signature_source_offset': hex(sigoff),
    'patched_image_sha256': hashlib.sha256(image).hexdigest(),
    'local_sources_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.glob('*.c')) + sorted(folder.glob('*.h'))},
}
(folder / 'unload-inspection.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, indent=2))
