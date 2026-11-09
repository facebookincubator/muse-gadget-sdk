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

import hashlib
import logging
import os
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


def system_roots() -> list[bytes]:
    """DER of every root in the OS trust store, as OpenSSL finds it.

    Reads the default CA file and every file in the default CA directory
    (``SSL_CERT_FILE`` and ``SSL_CERT_DIR`` override them), parsed by OpenSSL.
    """
    store = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    paths = ssl.get_default_verify_paths()
    files = [paths.cafile] if paths.cafile else []
    if paths.capath:
        try:
            with os.scandir(paths.capath) as entries:
                files += sorted(entry.path for entry in entries if entry.is_file())
        except OSError:
            pass
    for path in files:
        try:
            store.load_verify_locations(cafile=path)
        except (OSError, ssl.SSLError):
            continue    # CA directories also hold CRLs and other files
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
