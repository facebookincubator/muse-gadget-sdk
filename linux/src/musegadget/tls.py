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


# OpenSSL finds certificates in a CA directory by subject hash ("1a2b3c4d.0").
_HASH_NAME = re.compile(r"[0-9a-f]{8}\.[0-9]+")
_PEM_CERT = re.compile(
    rb"-----BEGIN (TRUSTED CERTIFICATE|X509 CERTIFICATE|CERTIFICATE)-----(.+?)-----END \1-----",
    re.S)
_SERVER_AUTH = bytes.fromhex("06082b06010505070301")   # DER OID 1.3.6.1.5.5.7.3.1
_ANY_EKU = bytes.fromhex("0604551d2500")              # DER OID 2.5.29.37.0


def _der_element(data: bytes, offset: int) -> tuple[int, int, int]:
    """``(tag, content start, end)`` of the DER element at ``offset``."""
    tag, length = data[offset], data[offset + 1]
    offset += 2
    if length & 0x80:
        size = length & 0x7F
        if not 0 < size <= 4 or offset + size > len(data):
            raise ValueError("bad DER length")
        length = int.from_bytes(data[offset:offset + size], "big")
        offset += size
    if offset + length > len(data):
        raise ValueError("truncated DER element")
    return tag, offset, offset + length


def _oids(data: bytes, start: int, end: int) -> set[bytes]:
    oids = set()
    while start < end:
        tag, _, element_end = _der_element(data, start)
        if tag != 0x06:
            raise ValueError("expected an OID")
        oids.add(data[start:element_end])
        start = element_end
    return oids


def _tls_certificate(trusted: bytes) -> bytes | None:
    """The certificate of an OpenSSL ``TRUSTED CERTIFICATE``, if it may anchor TLS.

    Its trust settings follow the certificate. As OpenSSL does for a TLS
    server, a root rejected for server authentication (or any purpose), or
    with a trust list that includes neither, is not a TLS root.
    """
    _, _, cert_end = _der_element(trusted, 0)
    cert, aux = trusted[:cert_end], trusted[cert_end:]
    if not aux:
        return cert
    _, start, end = _der_element(aux, 0)
    trust = reject = None
    while start < end:
        tag, content, element_end = _der_element(aux, start)
        if tag == 0x30:
            trust = _oids(aux, content, element_end)
        elif tag == 0xA0:
            reject = _oids(aux, content, element_end)
        start = element_end
    if reject and reject & {_SERVER_AUTH, _ANY_EKU}:
        return None
    if trust is not None and not trust & {_SERVER_AUTH, _ANY_EKU}:
        return None
    return cert


def _pem_certificates(data: bytes) -> list[bytes]:
    """DER of the certificates in a PEM file that may anchor TLS."""
    certs = []
    for label, body in _PEM_CERT.findall(data):
        try:
            der = base64.b64decode(b"".join(body.split()), validate=True)
            if label == b"TRUSTED CERTIFICATE":
                der = _tls_certificate(der)
        except (ValueError, IndexError, binascii.Error):
            continue
        if der:
            certs.append(der)
    return certs


def system_roots() -> list[bytes]:
    """DER of every TLS root in the OS trust store, as OpenSSL finds it.

    Reads the default CA file and the hash-named files in the default CA
    directory (``SSL_CERT_FILE`` and ``SSL_CERT_DIR`` override them), and
    keeps the trust settings of ``TRUSTED CERTIFICATE`` entries.
    """
    store = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    paths = ssl.get_default_verify_paths()
    files = [paths.cafile] if paths.cafile else []
    if paths.capath:
        try:
            with os.scandir(paths.capath) as entries:
                files += sorted(entry.path for entry in entries
                                if _HASH_NAME.fullmatch(entry.name) and entry.is_file())
        except OSError:
            pass
    for path in files:
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            continue
        for der in _pem_certificates(data):    # CA directories also hold CRLs
            try:
                store.load_verify_locations(cadata=der)
            except ssl.SSLError:
                continue
    return store.get_ca_certs(binary_form=True)


def distrusted_system_roots() -> list[str]:
    """Names of the distrusted roots that the OS trust store still has."""
    return sorted({name for der in system_roots() if (name := distrusted_name(der))})


def _build_context() -> ssl.SSLContext:
    roots = system_roots()
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
