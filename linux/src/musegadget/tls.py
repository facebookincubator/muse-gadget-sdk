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
# PEM markers start a line, as OpenSSL reads them: a BEGIN line may start with
# a UTF-8 byte order mark, and spaces and control characters may end a line.
# Other text is ignored.
_PEM_CERT = re.compile(
    rb"^(?:\xef\xbb\xbf)?-----BEGIN (TRUSTED CERTIFICATE|X509 CERTIFICATE|CERTIFICATE)-----"
    rb"[\x00-\x09\x0b-\x20]*$(.*?)^-----END \1-----[\x00-\x09\x0b-\x20]*$",
    re.S | re.M)
_SERVER_AUTH = bytes.fromhex("06082b06010505070301")   # DER OID 1.3.6.1.5.5.7.3.1
_ANY_EKU = bytes.fromhex("0604551d2500")              # DER OID 2.5.29.37.0
# The fields of OpenSSL's X509_CERT_AUX, in order: trust, reject, alias, key id, other.
_AUX_FIELDS = (0x30, 0xA0, 0x0C, 0x04, 0xA1)
# Name strings that OpenSSL canonicalises for its subject hash, and their encodings.
_CANON_STRINGS = {0x0C: "utf-8", 0x13: "latin-1", 0x14: "latin-1", 0x16: "latin-1",
                  0x1A: "latin-1", 0x1C: "utf-32-be", 0x1E: "utf-16-be"}
_ASCII_SPACE = re.compile("[ \t\n\v\f\r]+")
_ASCII_LOWER = {c: c + 32 for c in range(ord("A"), ord("Z") + 1)}


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


def _der(tag: int, content: bytes) -> bytes:
    if len(content) < 0x80:
        return bytes([tag, len(content)]) + content
    size = (len(content).bit_length() + 7) // 8
    return bytes([tag, 0x80 | size]) + len(content).to_bytes(size, "big") + content


def _subject(cert: bytes) -> bytes:
    """OpenSSL's canonical form of a certificate's subject (``x509_name_canon``).

    Strings become lower-case UTF-8 with runs of ASCII white space collapsed
    and trimmed. OpenSSL compares and hashes subjects in this form.
    """
    _, start, end = _der_element(cert, 0, len(cert))
    _, start, end = _der_element(cert, start, end)               # tbsCertificate
    if start < end and cert[start] == 0xA0:
        start = _der_element(cert, start, end)[2]                # version
    for _ in range(4):                                           # serial to validity
        start = _der_element(cert, start, end)[2]
    tag, start, end = _der_element(cert, start, end)
    if tag != 0x30:
        raise ValueError("expected a subject")
    canon = b""
    while start < end:
        tag, entry, rdn_end = _der_element(cert, start, end)
        if tag != 0x31:
            raise ValueError("expected a relative distinguished name")
        entries = []
        while entry < rdn_end:
            tag, content, entry_end = _der_element(cert, entry, rdn_end)
            if tag != 0x30:
                raise ValueError("expected an attribute")
            oid, value = _oid(cert, content, entry_end)
            tag, content, value_end = _der_element(cert, value, entry_end)
            if value_end != entry_end:
                raise ValueError("bad attribute")
            encoded = cert[value:value_end]
            if tag in _CANON_STRINGS:
                text = cert[content:value_end].decode(_CANON_STRINGS[tag])
                text = _ASCII_SPACE.sub(" ", text).strip(" ").translate(_ASCII_LOWER)
                encoded = _der(0x0C, text.encode())
            entries.append(_der(0x30, oid + encoded))
            entry = entry_end
        canon += _der(0x31, b"".join(sorted(entries)))           # DER sorts a SET OF
        start = rdn_end
    return canon


def _subject_hash(subject: bytes) -> str:
    """OpenSSL's subject hash (``X509_NAME_hash``) of a canonical subject."""
    return hashlib.sha1(subject).digest()[3::-1].hex()


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


def _file_certificates(path: str) -> tuple[list[tuple[bytes, bool]], set[bytes], bool] | None:
    """Each certificate of a file OpenSSL loads, and whether it may anchor TLS.

    ``None`` if OpenSSL refuses the file. Also returns the CA certificates
    OpenSSL loaded, and whether the trust settings of every entry were read,
    including every certificate OpenSSL loaded.
    """
    scratch = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    try:
        scratch.load_verify_locations(cafile=path)
        with open(path, "rb") as f:
            data = f.read()
    except OSError:     # ssl.SSLError included
        return None
    certs, complete = _pem_certificates(data)
    loaded = set(scratch.get_ca_certs(binary_form=True))
    complete = complete and loaded <= {der for der, _ in certs}
    if not complete:
        log.warning("couldn't read the TLS trust settings in %s; not using it", path)
    return certs, loaded, complete


def _hashed_files(capath: str) -> list[tuple[str, list[str]]]:
    """The files of a CA directory that OpenSSL's subject-hash lookup reads.

    Each subject hash with its files, in the order OpenSSL reads them: ".0",
    ".1" and so on, up to the first missing suffix.
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
        if chain:
            chains.append((stem, chain))
    return chains


def system_roots() -> list[bytes]:
    """DER of every TLS root in the OS trust store, as OpenSSL finds it.

    See ``_system_roots()``.
    """
    return _system_roots()[0]


def _system_roots() -> tuple[list[bytes], bool]:
    """DER of every TLS root in the OS trust store, and whether OpenSSL loads any of its files.

    Reads the default CA file and the hash-named files in the default CA
    directory (``SSL_CERT_FILE`` and ``SSL_CERT_DIR`` override them), and
    keeps the trust settings of ``TRUSTED CERTIFICATE`` entries. It follows
    OpenSSL's store (``X509_STORE``, ``by_dir``):

    - a file is used only when OpenSSL loads it and every entry's trust
      settings parse; if that fails for the CA file, nothing is trusted;
    - a hashed file that fails ends its subject hash's lookup;
    - the directory is searched only for subjects the CA file lacks, and a
      certificate counts only under its own subject hash;
    - the first copy of a certificate decides its trust.
    """
    store = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    paths = ssl.get_default_verify_paths()
    seen, readable, file_subjects = set(), False, set()

    def add(certs: list[tuple[bytes, bool]], loaded: set[bytes]) -> None:
        for der, tls_root in certs:
            if der not in seen:
                seen.add(der)
                if tls_root and der in loaded:
                    store.load_verify_locations(cadata=der)

    if paths.cafile:
        result = _file_certificates(paths.cafile)
        if result is not None:
            readable = True
            certs, loaded, complete = result
            if not complete:
                return [], True
            try:
                file_subjects = {_subject(der) for der, _ in certs}
            except (ValueError, UnicodeError):
                return [], True
            add(certs, loaded)
    chains = []
    if paths.capath:
        try:
            chains = _hashed_files(paths.capath)
        except OSError:
            pass
    for stem, chain in chains:
        for path in chain:
            result = _file_certificates(path)
            readable = readable or result is not None
            if result is None or not result[2]:
                break
            certs, loaded, _ = result
            usable = []
            for der, tls_root in certs:
                try:
                    subject = _subject(der)
                except (ValueError, UnicodeError):
                    continue
                if _subject_hash(subject) == stem and subject not in file_subjects:
                    usable.append((der, tls_root))
            add(usable, loaded)
    return store.get_ca_certs(binary_form=True), readable


def distrusted_system_roots() -> list[str]:
    """Names of the distrusted roots that the OS trust store still has."""
    return sorted({name for der in system_roots() if (name := distrusted_name(der))})


def _build_context() -> ssl.SSLContext:
    roots, readable = _system_roots()
    if not roots and readable:
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
