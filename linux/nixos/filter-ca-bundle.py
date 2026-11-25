#!/usr/bin/env python3
"""Filter a CA bundle for musegadget on NixOS.

Removes the roots musegadget's tls.py refuses (COMPROMISED_ROOTS) or
warns about (RETIRED_ROOTS): the eMudhra intermediates whose keys
became public (Mozilla Bug 2083449), the Comodo AAA root that
cross-signed them, and the two retired eMudhra roots. nixpkgs'
cacert still ships the AAA key (as a reissued cert — same key,
different cert bytes), so matching is by cert SHA-256, SPKI
SHA-256, or subject CN, mirroring tls.py's own matching.

Usage: filter-ca-bundle.py <in-bundle.crt> <out-bundle.crt>
"""
import base64
import hashlib
import re
import sys

from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

# (name, cert SHA-256, SPKI SHA-256) — copied from musegadget/tls.py.
DISTRUSTED = (
    ("emSign ECC Root CA - G3",
     "86a1ecba089c4a8d3bbe2734c612ba341d813e043cf9e8a862cd5c57a36bbe6b",
     "8d417db2dd8bf5e3084d1e3f196d583849d81bdd4c00c70b9d39369e96b8c782"),
    ("Comodo AAA Services root",
     "d7a7a0fb5d7e2731d771e9484ebcdef71d5f0c3e0a2948782bc83ee0ea699ef4",
     "bd153ed7b0434f6886b17bce8bbe84ed340c7132d702a8f4fa318f756ecbd6f3"),
    ("emSign Root CA - C1",
     "125609aa301da0a249b97a8239cb6a34216f44dcac9f3954b14292f2e8c8608f",
     "b7408b4d2be0238ba37004dd34e276c6019bd2f24c9db7d4980f5f6c359a4bcc"),
    ("emSign ECC Root CA - C3",
     "bc4d809b15189d78db3e1d8cf4f9726a795da1643ca5f1358e1ddb0edc0d7eb3",
     "eabc185c4e82d942b1a5978ba3c0181487d6b3b9974e5c49f72f6d0bd9637150"),
)
CERT_HASHES = {h for _, h, _ in DISTRUSTED}
SPKI_HASHES = {h for _, _, h in DISTRUSTED}
NAMES = {n for n, _, _ in DISTRUSTED} | {"AAA Certificate Services"}


def main() -> None:
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src).read()
    # The Mozilla bundle mixes plain CERTIFICATE blocks with TRUSTED
    # CERTIFICATE blocks (certdata entries carrying trust flags — the
    # AAA root is one of them). A CERTIFICATE-only pattern sees 118
    # of the bundle's 169 certs; OpenSSL reads all of them.
    blocks = re.findall(
        r"-----BEGIN (?:TRUSTED )?CERTIFICATE-----.*?"
        r"-----END (?:TRUSTED )?CERTIFICATE-----",
        data, re.S)
    kept, dropped = [], []
    for block in blocks:
        # A TRUSTED block's body is cert DER + auxiliary trust data;
        # slice off the first DER object (the certificate itself).
        raw = base64.b64decode(re.sub(r"-----.*-----|\s", "", block))
        first = raw[1]
        if first & 0x80:
            nbytes = first & 0x7F
            header = 2 + nbytes
            length = int.from_bytes(raw[2:header], "big")
        else:
            header, length = 2, first
        der = raw[:header + length]
        cert = x509.load_der_x509_certificate(der)
        cn = cert.subject.get_attributes_for_oid(x509.NameOID.COMMON_NAME)
        spki = cert.public_key().public_bytes(
            Encoding.DER, PublicFormat.SubjectPublicKeyInfo)
        if (hashlib.sha256(der).hexdigest() in CERT_HASHES
                or hashlib.sha256(spki).hexdigest() in SPKI_HASHES
                or (cn and cn[0].value in NAMES)):
            dropped.append(cn[0].value if cn else "<no CN>")
        else:
            kept.append(block)
    with open(dst, "w") as fh:
        fh.write("\n".join(kept) + "\n")
    print(f"kept {len(kept)}, dropped {len(dropped)}: {dropped}")


if __name__ == "__main__":
    main()
