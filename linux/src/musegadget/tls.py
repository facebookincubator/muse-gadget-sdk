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

"""TLS client context: the system trust store, checked before use.

Every HTTPS and WSS connection uses this one context. It is OpenSSL's
default trust, so the distro's own trust and reject settings apply, and so
do its ca-certificates updates. While the store still holds any of
``COMPROMISED_ROOTS``, musegadget refuses to connect at all; for
``RETIRED_ROOTS`` it only warns.
"""

from __future__ import annotations

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

# (name, certificate SHA-256, SubjectPublicKeyInfo SHA-256). The key hash
# also catches any reissue of the same root.
#
# Compromised: the private keys of eMudhra intermediates under emSign ECC
# Root CA - G3 became public (Mozilla Bug 2083449), and Comodo AAA Services
# root cross-signed those intermediates. musegadget won't connect while the
# system trust store still trusts either.
COMPROMISED_ROOTS = (
    ("emSign ECC Root CA - G3",
     "86a1ecba089c4a8d3bbe2734c612ba341d813e043cf9e8a862cd5c57a36bbe6b",
     "8d417db2dd8bf5e3084d1e3f196d583849d81bdd4c00c70b9d39369e96b8c782"),
    ("Comodo AAA Services root",
     "d7a7a0fb5d7e2731d771e9484ebcdef71d5f0c3e0a2948782bc83ee0ea699ef4",
     "bd153ed7b0434f6886b17bce8bbe84ed340c7132d702a8f4fa318f756ecbd6f3"),
)
# Retired: Mozilla removed these at eMudhra's request, as part of eMudhra's
# plan to retire its dual-purpose roots (Mozilla Bugs 2018979 and 2083709),
# not because of a key compromise. Some distros' ca-certificates packages
# still ship them, so musegadget only warns while the store trusts them.
RETIRED_ROOTS = (
    ("emSign Root CA - C1",
     "125609aa301da0a249b97a8239cb6a34216f44dcac9f3954b14292f2e8c8608f",
     "b7408b4d2be0238ba37004dd34e276c6019bd2f24c9db7d4980f5f6c359a4bcc"),
    ("emSign ECC Root CA - C3",
     "bc4d809b15189d78db3e1d8cf4f9726a795da1643ca5f1358e1ddb0edc0d7eb3",
     "eabc185c4e82d942b1a5978ba3c0181487d6b3b9974e5c49f72f6d0bd9637150"),
)
DISTRUSTED_ROOTS = COMPROMISED_ROOTS + RETIRED_ROOTS
_RETIRED_NAMES = frozenset(name for name, _, _ in RETIRED_ROOTS)

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


# The CA-directory files OpenSSL may read: named by subject hash ("1a2b3c4d.0",
# or ".r0" for CRLs); its store loader matches the hash in either case.
_HASH_NAME = re.compile(r"[0-9a-f]{8}\.r?[0-9]+", re.I)
_URI = re.compile(r"(?:^|:)[A-Za-z][A-Za-z0-9+.-]*:/")    # a scheme, as in file:///…


FIX = "update your ca-certificates package, then restart musegadget"
RETIRED_FIX = ("to stop trusting a retired root, deselect it in the system trust store "
               "(see \"Root CAs\" in the musegadget README)")


class TrustStoreError(Exception):
    """The system trust store can't be read or still holds a compromised root."""


def check_trust_store() -> list[str] | None:
    """Names of the distrusted roots in the system trust store, or None if it can't be read.

    Reads what OpenSSL uses by default: the CA file and the hash-named files
    of the CA directory (``SSL_CERT_FILE`` and ``SSL_CERT_DIR`` override
    them; ``SSL_CERT_DIR`` may list several, separated as in ``PATH``).
    Other files there, such as a distro's bundle with its own trust
    settings, are never looked up by OpenSSL, so they aren't read. A root
    counts if it's there at all, whatever trust settings it carries.
    A CA file, CA directory or hash-named file that exists but can't be read
    makes the whole store unreadable, since OpenSSL might still read what
    this can't, and so does a store URI in ``SSL_CERT_DIR`` or
    ``SSL_CERT_URI``.
    """
    paths = ssl.get_default_verify_paths()
    store = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    readable = False
    if paths.cafile:
        try:
            store.load_verify_locations(cafile=paths.cafile)
        except (OSError, ssl.SSLError):
            return None
        readable = True
    variable = getattr(paths, "openssl_capath_env", None)
    listed = os.environ.get(variable) if variable else None
    if os.environ.get("SSL_CERT_URI") or (listed and _URI.search(listed)):
        return None         # an OpenSSL store URI, which this doesn't read
    directories = listed.split(os.pathsep) if listed else ([paths.capath] if paths.capath else [])
    for directory in directories:
        if not os.path.isdir(directory):
            if directory and os.path.exists(directory):
                return None     # OpenSSL may read it as a store
            continue        # OpenSSL skips a listed directory that isn't there
        try:
            with os.scandir(directory) as entries:
                files = sorted(entry.path for entry in entries
                               if _HASH_NAME.fullmatch(entry.name) and entry.is_file())
        except OSError:
            return None
        for path in files:
            try:
                store.load_verify_locations(cafile=path)
            except (OSError, ssl.SSLError):
                # OpenSSL's lookup reads files differently, and may still
                # take a certificate from one that fails here.
                return None
            readable = True
    if not readable:
        return None
    return sorted({name for der in store.get_ca_certs(binary_form=True)
                   if (name := distrusted_name(der))})


def trust_store_problem() -> str | None:
    """Why the system trust store isn't safe to use, or None if it is.

    Only an unreadable store or a compromised root is a problem; see
    ``trust_store_warning()`` for retired roots.
    """
    found = check_trust_store()
    if found is None:
        return "can't read the system trust store"
    compromised = [name for name in found if name not in _RETIRED_NAMES]
    if compromised:
        return f"the system trust store still trusts {', '.join(compromised)}"
    return None


def trust_store_warning() -> str | None:
    """The retired roots the system trust store still trusts, as a sentence, or None."""
    retired = [name for name in check_trust_store() or () if name in _RETIRED_NAMES]
    if not retired:
        return None
    return (f"the system trust store still trusts {', '.join(retired)}, "
            "which Mozilla has retired")


def client_context() -> ssl.SSLContext:
    """The shared client context for every HTTPS and WSS connection: the system trust store.

    Raises ``TrustStoreError`` instead while ``trust_store_problem()`` finds one.
    Logs a warning, once, while ``trust_store_warning()`` finds a retired root.
    """
    global _context
    with _context_lock:
        if _context is None:
            problem = trust_store_problem()
            if problem is not None:
                raise TrustStoreError(f"musegadget won't connect: {problem}; {FIX}.")
            warning = trust_store_warning()
            if warning is not None:
                log.warning("%s; %s.", warning, RETIRED_FIX)
            _context = ssl.create_default_context()
        return _context
