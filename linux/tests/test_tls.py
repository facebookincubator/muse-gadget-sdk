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


def test_system_roots_reads_the_ca_file_and_every_file_in_the_ca_directory(tmp_path, monkeypatch):
    in_file, in_directory = CA("In file"), CA("In directory")
    (tmp_path / "bundle.pem").write_text(ssl.DER_cert_to_PEM_cert(in_file.der))
    directory = tmp_path / "certs"
    (directory / "nested").mkdir(parents=True)
    (directory / "root.pem").write_text(ssl.DER_cert_to_PEM_cert(in_directory.der))
    (directory / "README").write_text("not a certificate\n")
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths", lambda: SimpleNamespace(
        cafile=str(tmp_path / "bundle.pem"), capath=str(directory)))
    assert sorted(tls.system_roots()) == sorted([in_file.der, in_directory.der])


def test_system_roots_without_a_ca_file_or_directory_is_empty(monkeypatch):
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=None, capath=None))
    assert tls.system_roots() == []


def test_the_client_context_refuses_a_distrusted_root_and_trusts_the_rest(tmp_path, monkeypatch):
    good, bad = CA("Good root"), CA("Bad root")
    monkeypatch.setattr(tls, "DISTRUSTED_ROOTS", (("Bad root", "0" * 64, spki_sha256(bad.key.public_key())),))
    monkeypatch.setattr(tls, "system_roots", lambda: [good.der, bad.der])
    context = tls._build_context()
    assert context.get_ca_certs(binary_form=True) == [good.der]
    assert context.verify_mode == ssl.CERT_REQUIRED and context.check_hostname

    handshake(context, server_context(good, tmp_path))
    with pytest.raises(ssl.SSLCertVerificationError):
        handshake(context, server_context(bad, tmp_path))


def test_no_system_roots_falls_back_to_default_trust_with_a_warning(monkeypatch, caplog):
    monkeypatch.setattr(tls, "system_roots", lambda: [])
    context = tls._build_context()
    assert context.verify_mode == ssl.CERT_REQUIRED and context.check_hostname
    assert "found no system root CAs" in caplog.text


def test_a_store_of_only_distrusted_roots_trusts_nothing(monkeypatch):
    der = published_roots()[0][1]
    monkeypatch.setattr(tls, "system_roots", lambda: [der])
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
