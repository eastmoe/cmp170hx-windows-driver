"""Check compiled C against original upstream payload, and bound WPR layouts."""
import ctypes as c
import re
import struct
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
lib=c.CDLL(str(ROOT/'build/core_tests.dll'))
lib.test_payload.argtypes=[c.c_void_p,c.c_uint32,c.c_uint32]
lib.test_meta.argtypes=[c.c_void_p]+[c.c_uint64]*6
lib.test_profile.argtypes=[c.c_uint16,c.c_uint16]


class CoreTests(unittest.TestCase):
    def test_identity_allowlist(self):
        for ven,dev,want in [(0x10de,0x20c2,8),(0x10de,0x2082,10),(0x10de,0x220d,0),
                             (0x10de,0x20b0,0),(0x1234,0x20c2,0),(0xffff,0xffff,0)]:
            self.assertEqual(lib.test_profile(ven,dev),want)

    def test_payload_matches_upstream_all_bytes_and_bounds(self):
        patch=(ROOT/'tests/fixtures/sec2-postbl-plm-ss-cfg.patch').read_text()
        body=patch.split('+_kgspSec2PostblTimingFillPayload(',1)[1].split('+NV_STATUS',1)[0]
        for addr,val in [(0x823804,0xffffffff),(0x9a0148,0xffffffff),(0x1fa7cc,0xfffff0ff)]:
            expected=bytearray(struct.pack('<I',0x4a7)*(0xf800//4))
            for offset,value in re.findall(r'PutU32\(pSignatureVa, (0x\w+), (\w+)\);',body):
                value={'writeAddr':addr,'writeValue':val}.get(value,value)
                if isinstance(value,str):value=int(value.rstrip('U'),16)
                struct.pack_into('<I',expected,int(offset,16),value)
            buf=(c.c_ubyte*(0xf800+32))(*([0xa5]*(0xf800+32)))
            lib.test_payload(c.byref(buf,16),addr,val)
            self.assertEqual(bytes(buf[16:-16]),expected)
            self.assertEqual(bytes(buf[:16]+buf[-16:]),b'\xa5'*32)

    def test_layout_and_guard_bytes(self):
        for gb in [8,10]:
            buf=(c.c_ubyte*288)(*([0xa5]*288))
            fb=gb<<30
            self.assertEqual(lib.test_meta(c.byref(buf,16),fb,fb-(1<<20),0x10000,29367752,0x20000,0x30000),1)
            meta=bytes(buf[16:272]);q=struct.unpack('<32Q',meta)
            self.assertEqual(q[:6],(0xdc3aae21371a60b3,1,0x10000,29367752,0x20000,4096))
            self.assertEqual(q[9:11],(0x30000,0xf800))
            self.assertEqual(q[20],0) # no GA102 FRTS
            self.assertEqual(q[22],fb)
            self.assertEqual(q[31],0) # never pre-mark signature verified
            self.assertEqual(meta[241],1)
            self.assertLessEqual(fb-q[11],256<<20)
            self.assertEqual(q[14]%0x20000,0)
            self.assertEqual(q[18]%4096,0)
            self.assertEqual(q[17]%65536,0)
            self.assertEqual(bytes(buf[:16]+buf[-16:]),b'\xa5'*32)

    def test_invalid_layouts_rejected_without_writes(self):
        fb=8<<30
        for top,fwsize in [(0,29367752),(fb+1,29367752),(fb-(129<<20),29367752),(fb-(1<<20),0),
                           (fb-(1<<20),65<<20)]:
            buf=(c.c_ubyte*256)(*([0xa5]*256))
            self.assertEqual(lib.test_meta(buf,fb,top,0x10000,fwsize,0x20000,0x30000),0)
            self.assertEqual(bytes(buf),b'\xa5'*256)


if __name__=='__main__':unittest.main(verbosity=2)

