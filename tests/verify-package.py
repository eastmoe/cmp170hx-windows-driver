"""Verify local signature mathematically, PE digest, and catalog membership.
Does not add certificates to any trust store or claim Microsoft trust.
"""
import hashlib, struct, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'vendor/python'))
import pefile
from asn1crypto import cms,core
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding
cert_der=(ROOT/'dist/cmp170-local.cer').read_bytes()
certificate=x509.load_der_x509_certificate(cert_der)
class DigestInfo(core.Sequence):
    _fields=[('algorithm',core.Sequence),('digest',core.OctetString)]
class SpcIndirect(core.Sequence):
    _fields=[('data',core.Sequence),('message_digest',DigestInfo)]
def verify_cms(blob):
    signed=cms.ContentInfo.load(blob)['content']
    signer=signed['signer_infos'][0]
    assert signer['digest_algorithm']['algorithm'].native=='sha256'
    attrs=signer['signed_attrs']
    encoded=attrs.dump();encoded=b'\x31'+encoded[1:]
    certificate.public_key().verify(signer['signature'].native,encoded,padding.PKCS1v15(),hashes.SHA256())
    content=signed['encap_content_info']['content']
    # Authenticode uses PKCS#7 v1 ANY: hash the contents of the SEQUENCE.
    payload=content.parsed.contents if isinstance(content,core.Any) else content.native
    md=next(a['values'][0].native for a in attrs if a['type'].native=='message_digest')
    assert hashlib.sha256(payload).digest()==md,'CMS content digest mismatch'
    return content.parsed.untag().dump() if isinstance(content,core.Any) else payload
def verify_pe(name):
    path=ROOT/'dist'/name;data=path.read_bytes();pe=pefile.PE(data=data)
    assert pe.FILE_HEADER.Machine==0x8664
    if name.endswith('.sys'):
        assert pe.OPTIONAL_HEADER.Subsystem==1
        assert not any(s.Characteristics&0x20000000 and s.Characteristics&0x80000000 for s in pe.sections)
    sec=pe.OPTIONAL_HEADER.DATA_DIRECTORY[4];off=sec.VirtualAddress
    length,revision,kind=struct.unpack_from('<IHH',data,off)
    assert revision==0x200 and kind==2
    content=verify_cms(data[off+8:off+length])
    expected=SpcIndirect.load(content)['message_digest']['digest'].native
    checksum=pe.OPTIONAL_HEADER.get_field_absolute_offset('CheckSum')
    directory=sec.get_file_offset();h=hashlib.sha256()
    h.update(data[:checksum]);h.update(data[checksum+4:directory]);h.update(data[directory+8:pe.OPTIONAL_HEADER.SizeOfHeaders])
    total=pe.OPTIONAL_HEADER.SizeOfHeaders
    for s in sorted(pe.sections,key=lambda s:s.PointerToRawData):
        if s.SizeOfRawData:
            h.update(data[s.PointerToRawData:s.PointerToRawData+s.SizeOfRawData]);total+=s.SizeOfRawData
    if len(data)-sec.Size>total:h.update(data[total:len(data)-sec.Size])
    assert h.digest()==expected,f'{name}: PE digest mismatch'
    print(f'PASS {name}: SHA256 Authenticode digest and RSA signature match local certificate')
    return expected
digests=[verify_pe('cmp170.sys'),verify_pe('cmpctl.exe')]
catalog=verify_cms((ROOT/'dist/cmp170.cat').read_bytes())
# Inf2Cat stores the Authenticode digest of SYS and a whole-file digest of INF.
assert digests[0] in catalog,'SYS digest missing from catalog'
assert hashlib.sha256((ROOT/'dist/cmp170.inf').read_bytes()).digest() in catalog,'INF digest missing from catalog'
print('PASS catalog: RSA signature, content digest, SYS/INF membership')
print('Trust: locally self-signed; no Microsoft production trust or kernel load test implied.')

