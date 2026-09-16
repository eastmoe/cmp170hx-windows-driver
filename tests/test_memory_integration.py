"""Check production run() DMA and MMIO against recovered SYS evidence (no GPU)."""
from pathlib import Path
import hashlib, json, struct

ROOT = Path(__file__).resolve().parents[1]
dma = (ROOT / 'build/memory-dma.bin').read_bytes()
base = 0x10000000  # host HAL test allocation
q = lambda offset: struct.unpack_from('<Q', dma, offset)[0]
npages = (0x1d09ea0 + 4095) // 4096
leaves = (npages + 511) // 512
assert len(dma) == (23 + leaves) * 4096
assert q(18*4096) == base + 19*4096
for i in range(leaves):
    assert q(19*4096+i*8) == base+(20+i)*4096
for i in range(npages):
    physical = 21+leaves if i == 0 else 22+leaves if i == 18 else 20+leaves
    assert q(20*4096+i*8) == base+physical*4096
assert dma[(20+leaves)*4096:(21+leaves)*4096] == bytes(4096)
assert dma[20*4096+npages*8:(20+leaves)*4096] == bytes(leaves*4096-npages*8)
resource = (ROOT/'analysis/emulated-original-fast/sparse-fw.bin').read_bytes()
assert hashlib.sha256(resource).hexdigest() == 'bbd0ab26d8851d0e637ed8361e46692a0ce65675731554f4d1936af9450a0ad2'
assert dma[(21+leaves)*4096:] == resource[256:256+8192]
assert dma[17*4096:18*4096] == (ROOT/'vendor/ga100_bl.bin').read_bytes()
assert q(24) == 0x1d09ea0 and q(80) == 0xf7f0 and dma[241] == 0
assert q(16) == base+18*4096 and q(32) == base+17*4096 and q(72) == base+4096

trace = [tuple(int(x,16) for x in line.split(','))
         for line in (ROOT/'build/memory-mmio.csv').read_text().splitlines()]
reference = json.loads((ROOT/'analysis/emulated-handover/handover-probe.json').read_text())
def expected(index):
    return [(int(w['reg'],16), int(w['value'],16)) for w in reference['outcomes'][index]['writes']]
sec2 = expected(0)
sec2[-1] = (0x1180f8, 0x12345)  # randomized preserved low bits in engine.c
graphics = expected(1)
watched = {r for r,v in sec2+graphics}
assert [w for w in trace if w[0] in watched] == sec2+sec2+graphics
assert not any(r in (0x1103c0,0x9a0204,0x100ce0,0x1fa824,0x1fa828) for r,v in trace)
assert sum(r in (0x840100,0x840130) and v == 2 for r,v in trace) == 1
print('PASS: production sparse DMA layout, pinned original pages, original SEC2/graphics write order, single launch, no host geometry/WPR or GSP reset')
