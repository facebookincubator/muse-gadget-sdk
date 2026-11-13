# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""TLS client context: the system trust store minus distrusted root CAs.

Python verifies servers against the OS trust store, which a device keeps
until someone updates its ca-certificates package, and checks no revocation.
The Noise session doesn't authenticate the VM by itself, so a certificate
from a root that should no longer be trusted would let anyone stand in for
the Muse. Every HTTPS and WSS connection uses this context instead.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import logging
import os
import re
import ssl
import threading
import warnings

from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

log = logging.getLogger(__name__)

# (name, certificate SHA-256, SubjectPublicKeyInfo SHA-256). Mozilla removed
# these after the private keys of eMudhra intermediates under emSign ECC Root
# CA - G3 became public (Mozilla Bug 2083449). Comodo AAA Services root
# cross-signed those intermediates; Mozilla removed emSign C1 and C3 at
# eMudhra's request. The certificate hash needs no parsing; the key hash also
# catches any reissue of the same root.
DISTRUSTED_ROOTS = (
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

_context: ssl.SSLContext | None = None
_context_lock = threading.Lock()


def _spki_sha256(der: bytes) -> str | None:
    try:
        with warnings.catch_warnings():
            # Some system roots have a non-positive serial, which cryptography
            # warns about (and may later refuse); only the key matters here.
            warnings.simplefilter("ignore")
            key = x509.load_der_x509_certificate(der).public_key()
        spki = key.public_bytes(Encoding.DER, PublicFormat.SubjectPublicKeyInfo)
    except Exception:  # a root cryptography can't parse can't match a key hash
        return None
    return hashlib.sha256(spki).hexdigest()


def distrusted_name(der: bytes) -> str | None:
    """The name of the distrusted root that ``der`` is, or None."""
    cert_hash = hashlib.sha256(der).hexdigest()
    for name, cert_sha256, _ in DISTRUSTED_ROOTS:
        if cert_hash == cert_sha256:
            return name
    spki_hash = _spki_sha256(der)
    for name, _, spki_sha256 in DISTRUSTED_ROOTS:
        if spki_hash == spki_sha256:
            return name
    return None


# OpenSSL finds certificates in a CA directory by subject hash: "1a2b3c4d.0",
# then ".1" and so on, stopping at the first missing suffix.
_HASH_NAME = re.compile(r"([0-9a-f]{8})\.(0|[1-9][0-9]*)")
_PEM_CERT = re.compile(
    rb"-----BEGIN (TRUSTED CERTIFICATE|X509 CERTIFICATE|CERTIFICATE)-----(.+?)-----END \1-----",
    re.S)
_SERVER_AUTH = bytes.fromhex("06082b06010505070301")   # DER OID 1.3.6.1.5.5.7.3.1
_ANY_EKU = bytes.fromhex("0604551d2500")              # DER OID 2.5.29.37.0
# The fields of OpenSSL's X509_CERT_AUX, in order: trust, reject, alias, key id, other.
_AUX_FIELDS = (0x30, 0xA0, 0x0C, 0x04, 0xA1)


def _der_element(data: bytes, offset: int, limit: int) -> tuple[int, int, int]:
    """``(tag, content start, end)`` of the DER element at ``offset``, within ``limit``.

    Only DER's minimal length encoding is accepted, so equal values have
    equal bytes.
    """
    if offset + 2 > limit:
        raise ValueError("truncated DER element")
    tag, length = data[offset], data[offset + 1]
    offset += 2
    if length & 0x80:
        size = length & 0x7F
        if not 0 < size <= 4 or offset + size > limit or data[offset] == 0:
            raise ValueError("bad DER length")
        length = int.from_bytes(data[offset:offset + size], "big")
        if length < 0x80:
            raise ValueError("bad DER length")
        offset += size
    if offset + length > limit:
        raise ValueError("truncated DER element")
    return tag, offset, offset + length


def _oid(data: bytes, start: int, end: int) -> tuple[bytes, int]:
    """The DER OID at ``start`` and its end, checked as OpenSSL checks it."""
    tag, content, element_end = _der_element(data, start, end)
    value = data[content:element_end]
    # Each sub-identifier ends with a byte below 0x80 and has no leading 0x80.
    if tag != 0x06 or not value or value[-1] & 0x80 or any(
            byte == 0x80 and (i == 0 or not value[i - 1] & 0x80)
            for i, byte in enumerate(value)):
        raise ValueError("bad OID")
    return data[start:element_end], element_end


def _oids(data: bytes, start: int, end: int) -> set[bytes]:
    oids = set()
    while start < end:
        oid, start = _oid(data, start, end)
        oids.add(oid)
    return oids


def _algorithms(data: bytes, start: int, end: int) -> None:
    """Check a DER ``SEQUENCE OF AlgorithmIdentifier``."""
    while start < end:
        tag, content, element_end = _der_element(data, start, end)
        if tag != 0x30:
            raise ValueError("expected an AlgorithmIdentifier")
        _, content = _oid(data, content, element_end)
        if content < element_end:
            _, _, content = _der_element(data, content, element_end)    # parameters
        if content != element_end:
            raise ValueError("bad AlgorithmIdentifier")
        start = element_end


def _trusted_certificate(trusted: bytes) -> tuple[bytes, bool]:
    """An OpenSSL ``TRUSTED CERTIFICATE``'s certificate, and whether it may anchor TLS.

    Its trust settings follow the certificate. As OpenSSL does for a TLS
    server, a root rejected for server authentication (or any purpose), or
    with a trust list that includes neither, is not a TLS root. Settings
    that don't parse raise ``ValueError``; OpenSSL ignores bytes after them.
    """
    _, _, cert_end = _der_element(trusted, 0, len(trusted))
    cert, aux = trusted[:cert_end], trusted[cert_end:]
    if not aux:
        return cert, True
    tag, start, end = _der_element(aux, 0, len(aux))
    if tag != 0x30:
        raise ValueError("bad trust settings")
    trust = reject = None
    position = 0
    while start < end:
        tag, content, element_end = _der_element(aux, start, end)
        if tag not in _AUX_FIELDS[position:]:
            raise ValueError("bad trust settings")
        position = _AUX_FIELDS.index(tag) + 1
        if tag == 0x30:
            trust = _oids(aux, content, element_end)
        elif tag == 0xA0:
            reject = _oids(aux, content, element_end)
        elif tag == 0xA1:
            _algorithms(aux, content, element_end)
        start = element_end
    if reject and reject & {_SERVER_AUTH, _ANY_EKU}:
        return cert, False
    return cert, trust is None or bool(trust & {_SERVER_AUTH, _ANY_EKU})


def _pem_certificates(data: bytes) -> tuple[list[tuple[bytes, bool]], bool]:
    """The certificates of a PEM file, up to the first that doesn't parse.

    Returns each one's DER and whether it may anchor TLS, and whether every
    entry parsed. Like OpenSSL, a plain ``CERTIFICATE`` entry ignores
    anything after its certificate.
    """
    certs = []
    for label, body in _PEM_CERT.findall(data):
        try:
            der = base64.b64decode(b"".join(body.split()), validate=True)
            if label == b"TRUSTED CERTIFICATE":
                certs.append(_trusted_certificate(der))
            else:
                certs.append((der[:_der_element(der, 0, len(der))[2]], True))
        except (ValueError, binascii.Error):
            return certs, False
    return certs, True


def _file_certificates(path: str) -> tuple[list[tuple[bytes, bool]], bool]:
    """The CA certificates OpenSSL loads from a file, and whether each may anchor TLS.

    OpenSSL reads the file first, so a file it refuses gives nothing. The
    second value says whether the trust settings of every entry were read.
    """
    scratch = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    try:
        scratch.load_verify_locations(cafile=path)
        with open(path, "rb") as f:
            data = f.read()
    except OSError:     # ssl.SSLError included
        return [], False
    loaded = set(scratch.get_ca_certs(binary_form=True))
    certs, complete = _pem_certificates(data)
    return [(der, tls_root) for der, tls_root in certs if der in loaded], complete


def _hashed_files(capath: str) -> list[list[str]]:
    """The files of a CA directory that OpenSSL's subject-hash lookup reads.

    One list per subject hash, in the order OpenSSL reads them: ".0", ".1"
    and so on, up to the first missing suffix.
    """
    names = {}
    with os.scandir(capath) as entries:
        for entry in entries:
            if _HASH_NAME.fullmatch(entry.name) and entry.is_file():
                names[entry.name] = entry.path
    chains = []
    for stem in sorted({name[:8] for name in names}):
        chain = []
        while f"{stem}.{len(chain)}" in names:
            chain.append(names[f"{stem}.{len(chain)}"])
        chains.append(chain)
    return chains


def system_roots() -> list[bytes]:
    """DER of every TLS root in the OS trust store, as OpenSSL finds it.

    Reads the default CA file and the hash-named files in the default CA
    directory (``SSL_CERT_FILE`` and ``SSL_CERT_DIR`` override them), and
    keeps the trust settings of ``TRUSTED CERTIFICATE`` entries. A file is
    used only when OpenSSL loads it and every entry's trust settings parse;
    as in OpenSSL, a hashed file that fails ends its subject hash's lookup,
    and the first copy of a certificate decides its trust.
    """
    store = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    paths = ssl.get_default_verify_paths()
    chains = [[paths.cafile]] if paths.cafile else []
    if paths.capath:
        try:
            chains += _hashed_files(paths.capath)
        except OSError:
            pass
    seen = set()
    for chain in chains:
        for path in chain:
            certs, complete = _file_certificates(path)
            if not complete:
                break
            for der, tls_root in certs:
                if der not in seen:
                    seen.add(der)
                    if tls_root:
                        store.load_verify_locations(cadata=der)
    return store.get_ca_certs(binary_form=True)


def distrusted_system_roots() -> list[str]:
    """Names of the distrusted roots that the OS trust store still has."""
    return sorted({name for der in system_roots() if (name := distrusted_name(der))})


def _openssl_reads_a_trust_file() -> bool:
    """Whether OpenSSL loads any of the files ``system_roots()`` reads."""
    paths = ssl.get_default_verify_paths()
    files = [paths.cafile] if paths.cafile else []
    if paths.capath:
        try:
            files += [path for chain in _hashed_files(paths.capath) for path in chain]
        except OSError:
            pass
    for path in files:
        try:
            ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT).load_verify_locations(cafile=path)
            return True
        except OSError:     # ssl.SSLError included
            continue
    return False


def _build_context() -> ssl.SSLContext:
    roots = system_roots()
    if not roots and _openssl_reads_a_trust_file():
        # Python's default trust would load those files without the filter.
        log.error("found no usable TLS root CAs in the system store; not trusting any")
        return ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)    # trusts nothing, so connects nowhere
    if not roots:
        log.warning("found no system root CAs to filter; using Python's default TLS trust")
        return ssl.create_default_context()
    kept = [der for der in roots if distrusted_name(der) is None]
    if len(kept) < len(roots):
        log.info("not trusting %d distrusted root CAs from the system store",
                 len(roots) - len(kept))
    if not kept:
        return ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)    # trusts nothing, so connects nowhere
    return ssl.create_default_context(cadata=b"".join(kept))


def client_context() -> ssl.SSLContext:
    """The shared client context for every HTTPS and WSS connection."""
    global _context
    with _context_lock:
        if _context is None:
            _context = _build_context()
        return _context
