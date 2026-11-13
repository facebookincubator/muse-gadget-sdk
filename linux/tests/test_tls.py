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

from __future__ import annotations

import base64
import datetime
import hashlib
import re
import socket
import ssl
import threading
from pathlib import Path
from types import SimpleNamespace

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import (
    Encoding, NoEncryption, PrivateFormat, PublicFormat,
)
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from musegadget import tls

VECTORS = Path(__file__).parent / "vectors" / "distrusted_roots.pem"


def published_roots() -> list[tuple[str, bytes]]:
    text = VECTORS.read_text()
    blocks = re.findall(r"# ([^\n]+)\n(-----BEGIN CERTIFICATE-----.+?-----END CERTIFICATE-----)",
                        text, re.S)
    return [(name, ssl.PEM_cert_to_DER_cert(pem)) for name, pem in blocks]


def spki_sha256(public_key) -> str:
    return hashlib.sha256(public_key.public_bytes(
        Encoding.DER, PublicFormat.SubjectPublicKeyInfo)).hexdigest()


def make_cert(subject: str, key, issuer: tuple[str, object] | None = None, *, ca: bool):
    """A certificate OpenSSL's strict mode accepts; self-signed without ``issuer``."""
    issuer_name, issuer_key = issuer or (subject, key)
    now = datetime.datetime.now(datetime.timezone.utc)
    builder = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject)]))
        .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, issuer_name)]))
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
        .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(
            issuer_key.public_key()), critical=False)
    )
    if ca:
        builder = builder.add_extension(x509.KeyUsage(
            digital_signature=False, content_commitment=False, key_encipherment=False,
            data_encipherment=False, key_agreement=False, key_cert_sign=True, crl_sign=True,
            encipher_only=False, decipher_only=False), critical=True)
    else:
        builder = (
            builder
            .add_extension(x509.KeyUsage(
                digital_signature=True, content_commitment=False, key_encipherment=False,
                data_encipherment=False, key_agreement=False, key_cert_sign=False, crl_sign=False,
                encipher_only=False, decipher_only=False), critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), critical=False)
        )
    return builder.sign(issuer_key, hashes.SHA256())


class CA:
    def __init__(self, name: str) -> None:
        self.name = name
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.cert = make_cert(name, self.key, ca=True)
        self.der = self.cert.public_bytes(Encoding.DER)


def server_context(ca: CA, directory: Path) -> ssl.SSLContext:
    key = ec.generate_private_key(ec.SECP256R1())
    leaf = make_cert("localhost", key, (ca.name, ca.key), ca=False)
    certfile, keyfile = directory / f"{ca.name}.crt", directory / f"{ca.name}.key"
    certfile.write_bytes(leaf.public_bytes(Encoding.PEM))
    keyfile.write_bytes(key.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption()))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile, keyfile)
    return context


def handshake(client: ssl.SSLContext, server: ssl.SSLContext) -> None:
    """One TLS handshake with ``server`` on localhost; raises what the client raises."""
    with socket.create_server(("127.0.0.1", 0)) as listener:
        def serve() -> None:
            conn, _ = listener.accept()
            try:
                with server.wrap_socket(conn, server_side=True) as tls_conn:
                    tls_conn.recv(1)
            except (ssl.SSLError, OSError):
                pass

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            with socket.create_connection(listener.getsockname(), timeout=5) as raw:
                with client.wrap_socket(raw, server_hostname="localhost"):
                    pass
        finally:
            thread.join(timeout=5)


def test_the_table_matches_the_published_roots():
    roots = published_roots()
    assert [name for name, _ in roots] == [name for name, _, _ in tls.DISTRUSTED_ROOTS]
    for (name, der), (_, cert_sha256, spki_hash) in zip(roots, tls.DISTRUSTED_ROOTS):
        assert hashlib.sha256(der).hexdigest() == cert_sha256
        assert spki_sha256(x509.load_der_x509_certificate(der).public_key()) == spki_hash
        assert tls.distrusted_name(der) == name


def test_a_reissued_certificate_for_a_distrusted_key_is_refused(monkeypatch):
    key = ec.generate_private_key(ec.SECP256R1())
    original = make_cert("Original", key, ca=True).public_bytes(Encoding.DER)
    reissue = make_cert("Reissue", key, ca=True).public_bytes(Encoding.DER)
    monkeypatch.setattr(tls, "DISTRUSTED_ROOTS", (
        ("Test root", hashlib.sha256(original).hexdigest(), spki_sha256(key.public_key())),))
    assert tls.distrusted_name(original) == "Test root"
    assert tls.distrusted_name(reissue) == "Test root"
    assert tls.distrusted_name(CA("Unrelated").der) is None


def test_system_roots_reads_the_ca_file_and_the_hashed_files_in_the_ca_directory(tmp_path, monkeypatch):
    # OpenSSL looks certificates up in the CA directory by subject hash, from
    # ".0" up to the first missing suffix. Other files are not part of the
    # system's trust store.
    in_file, first, second = CA("In file"), CA("First"), CA("Second")
    unhashed, after_gap, padded = CA("Unhashed"), CA("After a gap"), CA("Padded suffix")
    (tmp_path / "bundle.pem").write_text(ssl.DER_cert_to_PEM_cert(in_file.der))
    directory = tmp_path / "certs"
    (directory / "nested").mkdir(parents=True)
    for name, ca in (("0a1b2c3d.0", first), ("0a1b2c3d.1", second), ("root.pem", unhashed),
                     ("4e5f6a7b.1", after_gap), ("0a1b2c3d.02", padded)):
        (directory / name).write_text(ssl.DER_cert_to_PEM_cert(ca.der))
    (directory / "README").write_text("not a certificate\n")
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths", lambda: SimpleNamespace(
        cafile=str(tmp_path / "bundle.pem"), capath=str(directory)))
    assert sorted(tls.system_roots()) == sorted([in_file.der, first.der, second.der])


SERVER_AUTH = bytes.fromhex("06082b06010505070301")      # 1.3.6.1.5.5.7.3.1
EMAIL_PROTECTION = bytes.fromhex("06082b06010505070304")  # 1.3.6.1.5.5.7.3.4
ANY_PURPOSE = bytes.fromhex("0604551d2500")             # 2.5.29.37.0


def der(tag: int, body: bytes) -> bytes:
    assert len(body) < 128
    return bytes([tag, len(body)]) + body


def trusted_pem(cert_der: bytes, trust=None, reject=None) -> str:
    """An OpenSSL ``TRUSTED CERTIFICATE``: the certificate and its trust settings."""
    aux = b""
    if trust is not None:
        aux += der(0x30, b"".join(trust))
    if reject is not None:
        aux += der(0xA0, b"".join(reject))
    body = base64.encodebytes(cert_der + der(0x30, aux)).decode()
    return f"-----BEGIN TRUSTED CERTIFICATE-----\n{body}-----END TRUSTED CERTIFICATE-----\n"


def test_system_roots_keeps_the_trust_settings_of_trusted_certificates(tmp_path, monkeypatch):
    # Some distributions ship an OpenSSL bundle with per-root trust settings
    # (e.g. ca-bundle.trust.crt): roots trusted only for email, and roots
    # rejected for TLS. They must not become TLS trust anchors here. As in
    # OpenSSL, "any purpose" counts for TLS in both lists.
    tls_root, any_purpose, email_only, rejected, rejected_any, plain = (
        CA("TLS root"), CA("Any-purpose root"), CA("Email-only root"),
        CA("Rejected root"), CA("Rejected for any purpose"), CA("No settings"))
    bundle = tmp_path / "bundle.trust.crt"
    bundle.write_text(
        trusted_pem(tls_root.der, trust=[EMAIL_PROTECTION, SERVER_AUTH])
        + trusted_pem(any_purpose.der, trust=[ANY_PURPOSE])
        + trusted_pem(email_only.der, trust=[EMAIL_PROTECTION])
        + trusted_pem(rejected.der, reject=[SERVER_AUTH])
        + trusted_pem(rejected_any.der, trust=[SERVER_AUTH], reject=[ANY_PURPOSE])
        + trusted_pem(plain.der))
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=str(bundle), capath=None))
    assert sorted(tls.system_roots()) == sorted([tls_root.der, any_purpose.der, plain.der])

    monkeypatch.setattr(tls, "DISTRUSTED_ROOTS", ())
    context = tls._build_context()
    for root in (tls_root, any_purpose, plain):
        handshake(context, server_context(root, tmp_path))
    for root in (email_only, rejected, rejected_any):
        with pytest.raises(ssl.SSLCertVerificationError):
            handshake(context, server_context(root, tmp_path))


ROOT = CA("Root")
NONMINIMAL_SERVER_AUTH = bytes.fromhex("0681082b06010505070301")   # long-form length


@pytest.mark.parametrize("aux, kept", [
    (None, True),                                                  # no settings at all
    (der(0x30, b""), True),                                         # empty settings
    (der(0x30, der(0x30, b"")), False),                             # empty trust list
    (der(0x30, der(0xA0, EMAIL_PROTECTION)), True),                 # rejected for email only
    (der(0x30, der(0x30, SERVER_AUTH) + der(0x0C, b"alias")), True),
    (der(0x30, der(0x0C, b"alias") + der(0x30, SERVER_AUTH)), False),   # out of order
    (der(0x30, der(0x30, EMAIL_PROTECTION) + der(0x30, SERVER_AUTH)), False),  # repeated
    (der(0x30, der(0x05, b"")), False),                             # unknown field
    (der(0x30, der(0xA0, NONMINIMAL_SERVER_AUTH)), False),          # not DER
    (der(0x30, der(0x30, SERVER_AUTH)) + b"\x00", True),            # OpenSSL ignores what follows
    (der(0x30, der(0x30, SERVER_AUTH))[:-1], False),                # truncated
    (der(0x30, bytes([0x30, 0x0B]) + SERVER_AUTH), False),          # list overruns settings
    (der(0x30, der(0x30, b"\x04\x00")), False),                     # not an OID
    (der(0x30, der(0xA0, b"\x06\x00")), False),                     # empty OID
    (der(0x30, der(0x30, SERVER_AUTH + bytes.fromhex("0603802a03"))), False),  # padded OID
    (der(0x30, der(0x30, SERVER_AUTH + bytes.fromhex("06022a83"))), False),    # unfinished OID
    (der(0x30, der(0x30, SERVER_AUTH) + der(0x04, b"\x01")
               + der(0xA1, der(0x30, bytes.fromhex("06032a0304") + b"\x05\x00"))), True),
    (der(0x30, der(0x30, SERVER_AUTH) + der(0xA1, der(0x30, b"\x05\x00"))), False),
    (der(0x30, der(0x30, SERVER_AUTH) + der(0xA1, b"\x04\x00")), False),
])
def test_trust_settings_that_do_not_parse_are_not_trusted(tmp_path, monkeypatch, aux, kept):
    body = ROOT.der if aux is None else ROOT.der + aux
    bundle = tmp_path / "bundle.trust.crt"
    bundle.write_text("-----BEGIN TRUSTED CERTIFICATE-----\n"
                      + base64.encodebytes(body).decode()
                      + "-----END TRUSTED CERTIFICATE-----\n")
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=str(bundle), capath=None))
    assert tls.system_roots() == ([ROOT.der] if kept else [])
    if kept:    # never more trust than OpenSSL gives the same file
        handshake(ssl.create_default_context(cafile=str(bundle)), server_context(ROOT, tmp_path))


def test_the_first_copy_of_a_root_decides_its_trust(tmp_path, monkeypatch):
    # OpenSSL keeps the first copy of a certificate it loads and ignores the rest.
    rejected_first, plain_first = CA("Rejected first"), CA("Plain first")
    bundle = tmp_path / "bundle.pem"
    bundle.write_text(trusted_pem(rejected_first.der, reject=[SERVER_AUTH])
                      + ssl.DER_cert_to_PEM_cert(rejected_first.der)
                      + ssl.DER_cert_to_PEM_cert(plain_first.der)
                      + trusted_pem(plain_first.der, reject=[SERVER_AUTH]))
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=str(bundle), capath=None))
    assert tls.system_roots() == [plain_first.der]


def test_a_file_that_does_not_parse_is_not_used(tmp_path, monkeypatch):
    # OpenSSL loads none of a CA file with an entry it can't read, and a
    # hashed file that fails ends the lookup of its subject hash.
    good, other = CA("Good"), CA("Other")
    pem = ssl.DER_cert_to_PEM_cert
    bad_settings = trusted_pem(CA("Bad").der, reject=[b"\x06\x00"])    # an empty OID
    bad_framing = trusted_pem(CA("Bad").der).replace("END TRUSTED CERTIFICATE", "END CERTIFICATE")
    directory = tmp_path / "certs"
    directory.mkdir()
    (directory / "0a1b2c3d.0").write_text(pem(good.der) + bad_settings)
    (directory / "0a1b2c3d.1").write_text(pem(good.der))
    (directory / "4e5f6a7b.0").write_text(pem(other.der))
    for bad in (bad_settings, bad_framing):
        (tmp_path / "bundle.pem").write_text(pem(good.der) + bad + pem(good.der))
        monkeypatch.setattr(tls.ssl, "get_default_verify_paths", lambda: SimpleNamespace(
            cafile=str(tmp_path / "bundle.pem"), capath=str(directory)))
        assert tls.system_roots() == [other.der]


def test_hashed_files_are_read_in_suffix_order(tmp_path, monkeypatch):
    # ".2" comes before ".10", so its copy of a root decides the root's trust.
    root = CA("Root")
    directory = tmp_path / "certs"
    directory.mkdir()
    fillers = [CA(f"Filler {i}") for i in range(11)]
    for i, filler in enumerate(fillers):
        (directory / f"0a1b2c3d.{i}").write_text(ssl.DER_cert_to_PEM_cert(filler.der))
    (directory / "0a1b2c3d.2").write_text(ssl.DER_cert_to_PEM_cert(root.der))
    (directory / "0a1b2c3d.10").write_text(trusted_pem(root.der, reject=[SERVER_AUTH]))
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=None, capath=str(directory)))
    assert root.der in tls.system_roots()


def test_pem_markers_must_start_a_line(tmp_path, monkeypatch):
    # OpenSSL ignores a commented-out entry; so must the trust filter.
    commented, root = CA("Commented"), CA("Root")
    text = "".join("# " + line + "\n" for line in ssl.DER_cert_to_PEM_cert(commented.der).splitlines())
    bundle = tmp_path / "bundle.pem"
    bundle.write_text(text + ssl.DER_cert_to_PEM_cert(root.der))
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=str(bundle), capath=None))
    assert tls.system_roots() == [root.der]
    handshake(ssl.create_default_context(cafile=str(bundle)), server_context(root, tmp_path))
    with pytest.raises(ssl.SSLCertVerificationError):
        handshake(ssl.create_default_context(cafile=str(bundle)), server_context(commented, tmp_path))


def subject_hash(name: str) -> str:
    """OpenSSL's subject hash (``X509_NAME_hash``) of a ``CN=name`` name, for simple names."""
    value = name.lower().encode()
    canonical = der(0x31, der(0x30, bytes.fromhex("0603550403") + der(0x0C, value)))
    return hashlib.sha1(canonical).digest()[3::-1].hex()


def test_the_ca_directory_trusts_what_openssl_trusts(tmp_path, monkeypatch):
    # Real subject-hash names, so OpenSSL's own directory lookup finds them.
    plain, tls_only, email_only, rejected, unhashed = (
        CA("plain root"), CA("tls root"), CA("email root"), CA("rejected root"), CA("unhashed root"))
    directory = tmp_path / "certs"
    directory.mkdir()
    for ca, text in ((plain, ssl.DER_cert_to_PEM_cert(plain.der)),
                     (tls_only, trusted_pem(tls_only.der, trust=[SERVER_AUTH])),
                     (email_only, trusted_pem(email_only.der, trust=[EMAIL_PROTECTION])),
                     (rejected, trusted_pem(rejected.der, reject=[SERVER_AUTH]))):
        (directory / f"{subject_hash(ca.name)}.0").write_text(text)
    (directory / "bundle.trust.crt").write_text(ssl.DER_cert_to_PEM_cert(unhashed.der))
    native = ssl.create_default_context(cafile=None, capath=str(directory))
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=None, capath=str(directory)))
    monkeypatch.setattr(tls, "DISTRUSTED_ROOTS", ())
    ours = tls._build_context()
    for ca, trusted in ((plain, True), (tls_only, True), (email_only, False),
                        (rejected, False), (unhashed, False)):
        for context in (native, ours):
            if trusted:
                handshake(context, server_context(ca, tmp_path))
            else:
                with pytest.raises(ssl.SSLCertVerificationError):
                    handshake(context, server_context(ca, tmp_path))


def test_system_roots_without_a_ca_file_or_directory_is_empty(monkeypatch):
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=None, capath=None))
    assert tls.system_roots() == []


def test_the_client_context_refuses_a_distrusted_root_and_trusts_the_rest(tmp_path, monkeypatch):
    good, bad = CA("Good root"), CA("Bad root")
    monkeypatch.setattr(tls, "DISTRUSTED_ROOTS", (("Bad root", "0" * 64, spki_sha256(bad.key.public_key())),))
    monkeypatch.setattr(tls, "_system_roots", lambda: ([good.der, bad.der], True))
    context = tls._build_context()
    assert context.get_ca_certs(binary_form=True) == [good.der]
    assert context.verify_mode == ssl.CERT_REQUIRED and context.check_hostname

    handshake(context, server_context(good, tmp_path))
    with pytest.raises(ssl.SSLCertVerificationError):
        handshake(context, server_context(bad, tmp_path))


def test_no_system_roots_falls_back_to_default_trust_with_a_warning(monkeypatch, caplog):
    monkeypatch.setattr(tls, "_system_roots", lambda: ([], False))
    context = tls._build_context()
    assert context.verify_mode == ssl.CERT_REQUIRED and context.check_hostname
    assert "found no system root CAs" in caplog.text


def test_unusable_trust_files_do_not_fall_back_to_default_trust(tmp_path, monkeypatch, caplog):
    # Default trust would load these files without the distrusted-root filter.
    # OpenSSL loads this entry, but its settings' length isn't minimally encoded.
    nonminimal = CA("Root").der + bytes.fromhex("30810c300a06082b06010505070301")
    bundle = tmp_path / "bundle.pem"
    bundle.write_text("-----BEGIN TRUSTED CERTIFICATE-----\n"
                      + base64.encodebytes(nonminimal).decode()
                      + "-----END TRUSTED CERTIFICATE-----\n")
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=str(bundle), capath=None))
    assert tls._system_roots() == ([], True)
    context = tls._build_context()
    assert context.get_ca_certs() == []
    assert "no usable TLS root CAs" in caplog.text


def test_a_store_of_only_distrusted_roots_trusts_nothing(monkeypatch):
    der = published_roots()[0][1]
    monkeypatch.setattr(tls, "_system_roots", lambda: ([der], True))
    context = tls._build_context()
    assert context.get_ca_certs() == []
    assert context.verify_mode == ssl.CERT_REQUIRED and context.check_hostname


def test_the_client_context_is_built_once(monkeypatch):
    built = []
    monkeypatch.setattr(tls, "_context", None)
    monkeypatch.setattr(tls, "_build_context", lambda: built.append(object()) or built[-1])
    assert tls.client_context() is tls.client_context()
    assert len(built) == 1


def test_distrusted_system_roots_names_each_root_once(monkeypatch):
    g3 = published_roots()[0][1]
    monkeypatch.setattr(tls, "system_roots", lambda: [g3, CA("Fine").der, g3])
    assert tls.distrusted_system_roots() == ["emSign ECC Root CA - G3"]
