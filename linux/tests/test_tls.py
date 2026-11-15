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
import os
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


def test_a_reissued_certificate_for_a_distrusted_key_is_recognised(monkeypatch):
    key = ec.generate_private_key(ec.SECP256R1())
    original = make_cert("Original", key, ca=True).public_bytes(Encoding.DER)
    reissue = make_cert("Reissue", key, ca=True).public_bytes(Encoding.DER)
    monkeypatch.setattr(tls, "DISTRUSTED_ROOTS", (
        ("Test root", hashlib.sha256(original).hexdigest(), spki_sha256(key.public_key())),))
    assert tls.distrusted_name(original) == "Test root"
    assert tls.distrusted_name(reissue) == "Test root"
    assert tls.distrusted_name(CA("Unrelated").der) is None


SERVER_AUTH = bytes.fromhex("06082b06010505070301")      # 1.3.6.1.5.5.7.3.1
EMAIL_PROTECTION = bytes.fromhex("06082b06010505070304")  # 1.3.6.1.5.5.7.3.4


def trusted_pem(cert_der: bytes, *, trust: bytes = b"", reject: bytes = b"") -> str:
    """An OpenSSL ``TRUSTED CERTIFICATE`` trusted for ``trust`` and rejected for ``reject``."""
    aux = b""
    if trust:
        aux += bytes([0x30, len(trust)]) + trust
    if reject:
        aux += bytes([0xA0, len(reject)]) + reject
    body = base64.encodebytes(cert_der + bytes([0x30, len(aux)]) + aux).decode()
    return f"-----BEGIN TRUSTED CERTIFICATE-----\n{body}-----END TRUSTED CERTIFICATE-----\n"


def use_store(monkeypatch, cafile, capath) -> None:
    monkeypatch.setattr(tls.ssl, "get_default_verify_paths",
                        lambda: SimpleNamespace(cafile=cafile and str(cafile), capath=capath and str(capath)))


def test_check_trust_store_reads_the_ca_file_and_the_hashed_files(tmp_path, monkeypatch):
    (g3, g3_der), (comodo, comodo_der), (c1, c1_der), (_, c3_der) = published_roots()
    (tmp_path / "bundle.pem").write_text(ssl.DER_cert_to_PEM_cert(CA("Fine").der)
                                         + ssl.DER_cert_to_PEM_cert(g3_der))
    directory = tmp_path / "certs"
    (directory / "nested").mkdir(parents=True)
    (directory / "0a1b2c3d.0").write_text(ssl.DER_cert_to_PEM_cert(g3_der))
    (directory / "4e5f6a7b.0").write_text(trusted_pem(comodo_der, reject=SERVER_AUTH))
    (directory / "5c8f9a0b.R1").write_text(ssl.DER_cert_to_PEM_cert(c1_der))
    # OpenSSL never looks up other files, such as a distro's own trust bundle.
    (directory / "ca-bundle.trust.crt").write_text(ssl.DER_cert_to_PEM_cert(c3_der))
    (directory / "README").write_text("not a certificate\n")
    use_store(monkeypatch, tmp_path / "bundle.pem", directory)
    assert tls.check_trust_store() == sorted([g3, comodo, c1])


def test_check_trust_store_reads_every_directory_ssl_cert_dir_lists(tmp_path, monkeypatch):
    (g3, g3_der), (comodo, comodo_der), _, _ = published_roots()
    one, two = tmp_path / "one", tmp_path / "two"
    one.mkdir()
    two.mkdir()
    (one / "0a1b2c3d.0").write_text(ssl.DER_cert_to_PEM_cert(g3_der))
    (two / "4e5f6a7b.0").write_text(ssl.DER_cert_to_PEM_cert(comodo_der))
    monkeypatch.setenv("SSL_CERT_FILE", str(tmp_path / "absent.pem"))
    monkeypatch.setenv("SSL_CERT_DIR", f"{one}{os.pathsep}{two}")
    assert tls.check_trust_store() == sorted([g3, comodo])


def test_check_trust_store_tells_a_clean_store_from_an_unreadable_one(tmp_path, monkeypatch):
    (tmp_path / "bundle.pem").write_text(ssl.DER_cert_to_PEM_cert(CA("Fine").der))
    use_store(monkeypatch, tmp_path / "bundle.pem", None)
    assert tls.check_trust_store() == []
    assert tls.trust_store_problem() is None
    (tmp_path / "empty").mkdir()
    for cafile, capath in ((None, None), (None, tmp_path / "empty"),
                           (tmp_path / "absent.pem", tmp_path / "absent")):
        use_store(monkeypatch, cafile, capath)
        assert tls.check_trust_store() is None
        assert tls.trust_store_problem() == "can't read the system trust store"


def test_a_directory_that_cant_be_listed_makes_the_store_unreadable(tmp_path, monkeypatch):
    # OpenSSL opens hashed files by name, so it might read a root from a
    # directory this check can't list.
    (tmp_path / "bundle.pem").write_text(ssl.DER_cert_to_PEM_cert(CA("Fine").der))
    (tmp_path / "certs").mkdir()
    real_scandir = tls.os.scandir

    def scandir(path):
        if str(path) == str(tmp_path / "certs"):
            raise PermissionError(path)
        return real_scandir(path)

    monkeypatch.setattr(tls.os, "scandir", scandir)
    use_store(monkeypatch, tmp_path / "bundle.pem", tmp_path / "certs")
    assert tls.check_trust_store() is None


@pytest.mark.parametrize("variable,value", [
    ("SSL_CERT_DIR", "file:///etc/ssl/legacy.pem"),
    ("SSL_CERT_DIR", "{tmp}/bundle.pem"),
    ("SSL_CERT_URI", "file:///etc/ssl/certs"),
])
def test_an_openssl_store_the_check_cant_read_makes_the_store_unreadable(tmp_path, monkeypatch,
                                                                         variable, value):
    (tmp_path / "bundle.pem").write_text(ssl.DER_cert_to_PEM_cert(CA("Fine").der))
    (tmp_path / "certs").mkdir()
    monkeypatch.setenv("SSL_CERT_FILE", str(tmp_path / "bundle.pem"))
    monkeypatch.setenv("SSL_CERT_DIR", str(tmp_path / "certs"))
    monkeypatch.delenv("SSL_CERT_URI", raising=False)
    assert tls.check_trust_store() == []
    monkeypatch.setenv(variable, value.format(tmp=tmp_path))
    assert tls.check_trust_store() is None


def test_a_hashed_file_that_doesnt_load_makes_the_store_unreadable(tmp_path, monkeypatch):
    # OpenSSL's directory lookup reads certificates one at a time and skips
    # other entries, so it may take a root from a file that fails to load here.
    (tmp_path / "bundle.pem").write_text(ssl.DER_cert_to_PEM_cert(CA("Fine").der))
    directory = tmp_path / "certs"
    directory.mkdir()
    damaged = (ssl.DER_cert_to_PEM_cert(published_roots()[0][1])
               + "-----BEGIN X509 CRL-----\nMAA=\n-----END X509 CRL-----\n")
    use_store(monkeypatch, tmp_path / "bundle.pem", directory)
    for name in ("14bc7599.0", "14BC7599.1"):     # OpenSSL's store loader ignores case
        (directory / name).write_text(damaged)
        assert tls.check_trust_store() is None
        (directory / name).unlink()
    (directory / "README").write_text(damaged)    # never looked up
    assert tls.check_trust_store() == []


def test_the_client_context_refuses_an_unsafe_trust_store(tmp_path, monkeypatch):
    monkeypatch.setattr(tls, "_context", None)
    for problem in ("can't read the system trust store",
                    "the system trust store still trusts emSign ECC Root CA - G3"):
        monkeypatch.setattr(tls, "trust_store_problem", lambda: problem)
        with pytest.raises(tls.TrustStoreError, match=f"{problem}; {tls.FIX}"):
            tls.client_context()
        assert tls._context is None


def test_the_client_context_is_the_system_trust_store(tmp_path, monkeypatch):
    # The distro's trust settings apply as OpenSSL reads them: a root trusted
    # only for email, or rejected for TLS, doesn't anchor a server certificate.
    plain, tls_root, email_only, rejected = CA("Plain"), CA("TLS"), CA("Email only"), CA("Rejected")
    bundle = tmp_path / "bundle.pem"
    bundle.write_text(ssl.DER_cert_to_PEM_cert(plain.der)
                      + trusted_pem(tls_root.der, trust=SERVER_AUTH)
                      + trusted_pem(email_only.der, trust=EMAIL_PROTECTION)
                      + trusted_pem(rejected.der, reject=SERVER_AUTH))
    (tmp_path / "certs").mkdir()
    monkeypatch.setenv("SSL_CERT_FILE", str(bundle))
    monkeypatch.setenv("SSL_CERT_DIR", str(tmp_path / "certs"))
    monkeypatch.setattr(tls, "_context", None)
    assert tls.trust_store_problem() is None
    context = tls.client_context()
    assert context.verify_mode == ssl.CERT_REQUIRED and context.check_hostname
    for ca in (plain, tls_root):
        handshake(context, server_context(ca, tmp_path))
    for ca in (email_only, rejected):
        with pytest.raises(ssl.SSLCertVerificationError):
            handshake(context, server_context(ca, tmp_path))


def test_the_client_context_is_built_once(monkeypatch):
    built = []
    monkeypatch.setattr(tls, "_context", None)
    monkeypatch.setattr(tls, "trust_store_problem", lambda: None)
    monkeypatch.setattr(tls.ssl, "create_default_context", lambda: built.append(object()) or built[-1])
    assert tls.client_context() is tls.client_context()
    assert len(built) == 1


UNSAFE = ("can't read the system trust store",
          "the system trust store still trusts emSign ECC Root CA - G3")


@pytest.mark.parametrize("problem", UNSAFE)
@pytest.mark.parametrize("argv", [["run"], ["pair"], ["voice-call", "--vm-id", "vm-1"]])
def test_commands_that_connect_refuse_to_start_on_an_unsafe_trust_store(monkeypatch, capsys, argv, problem):
    from unittest.mock import Mock

    from musegadget import cli, muse_api, service

    connect = Mock(side_effect=AssertionError("must not start"))
    for module, name in ((muse_api, "fetch_vms_with_status"), (service, "run_service")):
        monkeypatch.setattr(module, name, connect)
    monkeypatch.setattr(tls, "_context", None)
    monkeypatch.setattr(tls, "trust_store_problem", lambda: problem)
    assert cli.main(argv) == cli.EXIT_TRUST_STORE == 78
    output = capsys.readouterr()
    assert problem in output.err and tls.FIX in output.err
    assert "Setup open" not in output.out
    connect.assert_not_called()


def test_the_service_checks_the_trust_store_before_connecting(monkeypatch):
    import asyncio
    from unittest.mock import Mock

    from musegadget import config, muse_api
    from musegadget.executor import Account, Executor
    from musegadget.identity import Identity
    from musegadget.service import Service

    connect = Mock(side_effect=AssertionError("must not connect"))
    monkeypatch.setattr(muse_api, "fetch_vms_with_status", connect)
    monkeypatch.setattr(config, "load_json", connect)    # the first step of each connection
    monkeypatch.setattr(tls, "_context", None)
    monkeypatch.setattr(tls, "trust_store_problem", lambda: UNSAFE[1])

    async def run() -> None:
        await Service(identity=Identity("02:00:00:00:00:01"), executor=Executor(Account.current())).run()

    with pytest.raises(tls.TrustStoreError):
        asyncio.run(run())
    connect.assert_not_called()


def test_pairing_checks_the_trust_store_before_opening_setup(monkeypatch):
    from unittest.mock import Mock

    from musegadget import pair

    on_open = Mock()
    monkeypatch.setattr(tls, "_context", None)
    monkeypatch.setattr(tls, "trust_store_problem", lambda: UNSAFE[0])
    with pytest.raises(tls.TrustStoreError):
        pair.pair(on_open=on_open)
    on_open.assert_not_called()
