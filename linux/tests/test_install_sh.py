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

"""install.sh never runs a uv, or anything else under /opt/musegadget, that
another account could have changed.

Each test runs install.sh's functions as root in a private mount namespace,
with tmpfs over /opt and /mnt (where any uid may leave a mark) and uid 1001
as the other account: as real root when
the tests run as root, otherwise in a `podman unshare` user namespace. The
uv archive is a fake served by a fake curl, so no network is needed.
"""

from __future__ import annotations

import datetime
import hashlib
import io
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import Encoding
from cryptography.x509.oid import NameOID

from musegadget import tls

INSTALL_SH = Path(__file__).resolve().parents[1] / "install.sh"
TARGET = "x86_64-unknown-linux-gnu"
OTHER_UID = 1001


def _namespace_command():
    if not sys.platform.startswith("linux") or not all(map(os.path.isdir, ("/opt", "/mnt"))):
        return None
    if shutil.which("unshare") is None or shutil.which("setpriv") is None:
        return None
    if os.geteuid() == 0:
        command = ["unshare", "-m", "--propagation", "private"]
    elif shutil.which("podman"):
        command = ["podman", "unshare", "unshare", "-m", "--propagation", "private"]
    else:
        return None
    probe = (
        "mount -t tmpfs -o mode=0755 tmpfs /opt && "
        f"setpriv --reuid={OTHER_UID} --regid={OTHER_UID} --clear-groups true"
    )
    try:
        result = subprocess.run(command + ["bash", "-c", probe], capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return command if result.returncode == 0 else None


NAMESPACE = _namespace_command()
needs_namespace = pytest.mark.skipif(
    NAMESPACE is None,
    reason="needs Linux, and root or podman for a mount namespace with another uid",
)


def _uv_archive(binary: bytes) -> bytes:
    """A uv release archive as the real ones are packed: owned by uid 1001."""
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w:gz") as tar:
        for name, data in (("uv", binary), ("uvx", b"#!/bin/sh\nexit 0\n")):
            info = tarfile.TarInfo(f"uv-{TARGET}/{name}")
            info.size, info.mode, info.uid, info.gid = len(data), 0o755, OTHER_UID, OTHER_UID
            tar.addfile(info, io.BytesIO(data))
    return out.getvalue()


@pytest.fixture
def env(tmp_path):
    work = tmp_path
    (work / "bin").mkdir()
    (work / "tmp").mkdir()
    marks = Path("/mnt/marks")   # inside the namespace; any uid may write

    good_uv = (
        "#!/bin/sh\n"
        f'echo "$(id -u) $*" >> {marks}/good-uv\n'
        'echo "uv 0.9.9"\n'
    ).encode()
    good = work / "good.tar.gz"
    good.write_bytes(_uv_archive(good_uv))
    (work / "good-uv").write_bytes(good_uv)
    bad = work / "bad.tar.gz"
    bad.write_bytes(_uv_archive(good_uv + b"# changed\n"))

    curl = work / "bin" / "curl"
    curl.write_text(
        "#!/bin/sh\n"
        'out=""; url=""; cacert=""; capath=""\n'
        'while [ $# -gt 0 ]; do\n'
        '    case "$1" in\n'
        '        -o|--proto|--cacert|--capath)\n'
        '            [ "$1" = -o ] && out="$2"; [ "$1" = --cacert ] && cacert="$2"\n'
        '            [ "$1" = --capath ] && capath="$2"; shift 2 ;;\n'
        '        -*) shift ;;\n'
        '        *) url="$1"; shift ;;\n'
        '    esac\n'
        'done\n'
        f'echo "$url" >> {work}/curl-calls\n'
        f'echo "$out" >> {work}/curl-outs\n'
        f'echo "$cacert $capath" >> {work}/curl-cas\n'
        'cp "$FAKE_ARCHIVE" "$out"\n'
    )
    curl.chmod(0o755)
    return {
        "work": work,
        "marks": marks,
        "good": good,
        "bad": bad,
        "good_uv": work / "good-uv",
        "archive_sha": hashlib.sha256(good.read_bytes()).hexdigest(),
        "binary_sha": hashlib.sha256(good_uv).hexdigest(),
    }


def run(env, scenario, *, archive="good"):
    """Runs `scenario` after sourcing install.sh, as root with an empty /opt."""
    work = env["work"]
    script = f"""
set -u
mount -t tmpfs -o mode=0755 tmpfs /opt
mount -t tmpfs -o mode=1777 tmpfs /mnt
install -d -m 1777 /mnt/marks
(
export PATH="{work}/bin:$PATH" TMPDIR="{work}/tmp" FAKE_ARCHIVE="{env[archive]}"
source "{INSTALL_SH}"
ARCH=amd64 UV_TMP="" CA_FILE=/checked/ca-bundle.crt
uv_release() {{ UV_ARCHIVE_SHA256={env["archive_sha"]}; UV_BINARY_SHA256={env["binary_sha"]}; }}
as_other() {{ setpriv --reuid={OTHER_UID} --regid={OTHER_UID} --clear-groups "$@"; }}
show() {{ stat -c '%n uid=%u mode=%a ino=%i type=%F' "$@"; }}
# A helper planted by the other account: it answers --version like uv, and
# records any other run.
probe() {{
    printf '#!/bin/sh\\necho "$(id -u) $*" >> {env["marks"]}/probe\\n'
    printf '[ "$1" = --version ] && echo "uv 0.9.9" && exit 0\\nexit 1\\n'
}}
{scenario}
)
status=$?
for f in /mnt/marks/*; do [ -e "$f" ] && sed "s|^|ran:${{f##*/}}: |" "$f"; done
exit $status
"""
    result = subprocess.run(
        NAMESPACE + ["bash", "-c", script], capture_output=True, text=True, timeout=120
    )
    return result


def stat_of(output, path):
    for line in output.splitlines():
        if line.startswith(path + " uid="):
            head, kind = line.split(" type=", 1)
            fields = dict(field.split("=", 1) for field in head.split()[1:])
            fields["type"] = kind
            return fields
    raise AssertionError(f"no stat for {path} in:\n{output}")


def ran(result, name):
    prefix = f"ran:{name}: "
    return [line[len(prefix):] for line in result.stdout.splitlines() if line.startswith(prefix)]


def curl_calls(env):
    path = env["work"] / "curl-calls"
    return path.read_text().splitlines() if path.exists() else []


def assert_installed_uv(env, result):
    assert result.returncode == 0, result.stdout + result.stderr
    uv = stat_of(result.stdout, "/opt/musegadget/bin/uv")
    assert uv["uid"] == "0" and uv["mode"] == "755" and uv["type"] == "regular file"
    assert "sha=" + env["binary_sha"] in result.stdout
    assert "uvx gone" in result.stdout
    return uv


AFTER = """
show /opt/musegadget/bin/uv
echo "sha=$(sha256sum /opt/musegadget/bin/uv | cut -d' ' -f1)"
[ -e /opt/musegadget/bin/uvx ] || echo "uvx gone"
"""


@needs_namespace
def test_fresh_install_is_root_owned_and_never_run(env):
    result = run(env, "prepare_prefix\ninstall_uv\nshow /opt/musegadget /opt/musegadget/bin /opt/musegadget/no-ca-dir\n"
                 "echo \"no-ca-dir entries: $(ls -A /opt/musegadget/no-ca-dir | wc -l)\"\n" + AFTER)
    assert_installed_uv(env, result)
    for path in ("/opt/musegadget", "/opt/musegadget/bin", "/opt/musegadget/no-ca-dir"):
        made = stat_of(result.stdout, path)
        assert made["uid"] == "0" and made["mode"] == "755" and made["type"] == "directory"
    assert curl_calls(env) == [
        f"https://github.com/astral-sh/uv/releases/download/0.9.9/uv-{TARGET}.tar.gz"
    ]
    # It uses only the bundle check_trust_store_before_downloads checked: no CA directory.
    assert (env["work"] / "curl-cas").read_text().split() == ["/checked/ca-bundle.crt", "/opt/musegadget/no-ca-dir"]
    assert re.search(r"no-ca-dir entries: *0\n", result.stdout)
    assert ran(result, "good-uv") == []


@needs_namespace
def test_planted_helper_is_never_run_and_is_replaced(env):
    """uid 1001 owns uv and swaps in a helper that answers --version."""
    result = run(env, f"""
prepare_prefix
install -o {OTHER_UID} -g {OTHER_UID} -m 0755 /dev/null /opt/musegadget/bin/uv
install -o {OTHER_UID} -g {OTHER_UID} -m 0755 /dev/null /opt/musegadget/bin/uvx
probe | as_other sh -c 'cat > /opt/musegadget/bin/uv'
prepare_prefix
install_uv
{AFTER}""")
    assert_installed_uv(env, result)
    assert ran(result, "probe") == []
    assert len(curl_calls(env)) == 1


@needs_namespace
@pytest.mark.parametrize("mode", ["0755", "0644"])
def test_wrong_bytes_are_never_run(env, mode):
    """A root-owned uv with other bytes, executable or not, is replaced unrun."""
    result = run(env, f"""
prepare_prefix
probe > /opt/musegadget/bin/uv
chmod {mode} /opt/musegadget/bin/uv
install_uv
{AFTER}""")
    assert_installed_uv(env, result)
    assert ran(result, "probe") == []
    assert len(curl_calls(env)) == 1


@needs_namespace
def test_pinned_uv_is_reused_as_a_new_file(env):
    """The right bytes, even owned by uid 1001, are copied to a new root-owned
    inode without a download, so the old inode stops mattering."""
    result = run(env, f"""
prepare_prefix
install -o {OTHER_UID} -g {OTHER_UID} -m 0755 {env["good_uv"]} /opt/musegadget/bin/uv
show /opt/musegadget/bin/uv | sed 's/^/before /'
install_uv
{AFTER}""")
    uv = assert_installed_uv(env, result)
    before = stat_of(result.stdout.replace("before ", ""), "/opt/musegadget/bin/uv")
    assert before["uid"] == str(OTHER_UID) and before["ino"] != uv["ino"]
    assert curl_calls(env) == []
    assert ran(result, "good-uv") == []


@needs_namespace
def test_open_descriptor_on_the_old_file_cannot_change_the_new_one(env):
    result = run(env, f"""
prepare_prefix
install -o {OTHER_UID} -g {OTHER_UID} -m 0755 {env["good_uv"]} /opt/musegadget/bin/uv
mkfifo -m 0666 /mnt/go
as_other sh -c 'exec 3>>/opt/musegadget/bin/uv; read x < /mnt/go; echo changed >&3' &
holder=$!
sleep 0.5
install_uv
echo go > /mnt/go
wait $holder
{AFTER}""")
    assert_installed_uv(env, result)


@needs_namespace
def test_symlinked_uv_is_not_followed(env):
    result = run(env, f"""
prepare_prefix
probe > {env["work"]}/tmp/target
chmod 0755 {env["work"]}/tmp/target
ln -s {env["work"]}/tmp/target /opt/musegadget/bin/uv
install_uv
{AFTER}""")
    assert_installed_uv(env, result)
    assert ran(result, "probe") == []
    assert (env["work"] / "tmp" / "target").read_text().startswith("#!/bin/sh")


@needs_namespace
def test_bad_download_stops_before_running_anything(env):
    result = run(env, f"""
prepare_prefix
install -o {OTHER_UID} -g {OTHER_UID} -m 0755 /dev/null /opt/musegadget/bin/uv
probe | as_other sh -c 'cat > /opt/musegadget/bin/uv'
install_uv
echo "not reached"
""", archive="bad")
    assert result.returncode != 0
    assert "doesn't match its checksum" in result.stderr
    assert "not reached" not in result.stdout
    assert ran(result, "probe") == []


@needs_namespace
def test_untrusted_uv_is_removed_even_when_the_download_fails(env):
    result = run(env, f"""
prepare_prefix
install -o {OTHER_UID} -g {OTHER_UID} -m 0755 /dev/null /opt/musegadget/bin/uv
( install_uv ) || true
[ -e /opt/musegadget/bin/uv ] && echo "uv still there" || echo "uv removed"
""", archive="bad")
    assert "uv removed" in result.stdout, result.stdout + result.stderr


@needs_namespace
@pytest.mark.parametrize(
    "setup",
    [
        f"install -d -o {OTHER_UID} -m 0755 /opt/musegadget",
        "install -d -m 0755 /opt/musegadget && install -d -m 0777 /opt/musegadget/bin",
        "install -d -m 0775 /opt/musegadget",
        f"install -d -o {OTHER_UID} -m 0755 /opt/elsewhere && ln -s /opt/elsewhere /opt/musegadget",
        "chmod 0777 /opt",
    ],
    ids=["prefix-owner", "bin-writable", "prefix-group-writable", "prefix-symlink", "parent-writable"],
)
def test_unsafe_directories_are_refused(env, setup):
    result = run(env, f"""
{setup}
prepare_prefix
echo "not reached"
""")
    assert result.returncode != 0
    assert "must be a directory that only root can change" in result.stderr
    assert "not reached" not in result.stdout
    assert curl_calls(env) == []


@needs_namespace
def test_temp_dir_ignores_tmpdir(env):
    """Another account owning $TMPDIR can't swap uv after it is checked."""
    result = run(env, f"""
install -d -o {OTHER_UID} -m 0755 /mnt/other-tmp
export TMPDIR=/mnt/other-tmp
prepare_prefix
install_uv
{AFTER}""")
    assert_installed_uv(env, result)
    outs = (env["work"] / "curl-outs").read_text().split()
    assert outs and all(out.startswith("/opt/musegadget/.uv.") for out in outs)


def fake_uv(marks, fail_pip=False):
    """A uv that records its calls and makes just enough of a venv."""
    return f"""cat > /opt/musegadget/bin/uv <<'UV'
#!/bin/sh
echo "UV_NO_CONFIG=$UV_NO_CONFIG UV_LINK_MODE=$UV_LINK_MODE $*" >> {marks}/uv
for arg; do last="$arg"; done
case "$1" in
    venv)
        mkdir -p "$last/bin"
        printf '#!/bin/sh\\necho "$*" >> {marks}/python\\necho "$last/data"\\n' > "$last/bin/python"
        chmod 0755 "$last/bin/python" ;;
    pip) {"exit 1" if fail_pip else ":"} ;;
esac
UV
chmod 0755 /opt/musegadget/bin/uv
install -d -m 0755 /opt/musegadget/venv
install -o {OTHER_UID} -m 0644 /dev/null /opt/musegadget/venv/planted.pth
"""


@needs_namespace
def test_venv_is_rebuilt_and_uv_and_python_ignore_the_current_directory(env):
    """install_musegadget swaps in a new venv, and root's uv and python ignore
    settings and modules in the current directory (UV_NO_CONFIG, python -I)."""
    marks = env["marks"]
    result = run(env, f"""
mount -t tmpfs -o mode=0755 tmpfs /usr/local/bin
prepare_prefix
{fake_uv(marks)}
install -d -o {OTHER_UID} -m 0755 /mnt/cwd
cd /mnt/cwd
SOURCE=/src.whl
install_musegadget
[ -e /opt/musegadget/venv/planted.pth ] && echo "planted kept" || echo "planted gone"
[ -x /opt/musegadget/venv/bin/python ] && echo "new venv in place"
echo "leftovers: $(ls -A /opt/musegadget | grep -c '^\\.')"
""")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "planted gone" in result.stdout and "new venv in place" in result.stdout
    assert "leftovers: 0" in result.stdout
    calls = ran(result, "uv")
    assert calls and all(call.startswith("UV_NO_CONFIG=1 UV_LINK_MODE=copy ") for call in calls)
    assert calls[0].split()[2] == "venv" and "--relocatable" in calls[0]
    assert ran(result, "python") and all(call.startswith("-I ") for call in ran(result, "python"))


@needs_namespace
def test_failed_install_keeps_the_old_venv(env):
    result = run(env, f"""
prepare_prefix
{fake_uv(env["marks"], fail_pip=True)}
SOURCE=/src.whl
set +e
(set -e; install_musegadget)
[ $? -ne 0 ] && echo "install failed"
set -e
[ -e /opt/musegadget/venv/planted.pth ] && echo "old venv kept"
prepare_prefix
echo "leftovers: $(ls -A /opt/musegadget | grep -c '^\\.')"
""")
    assert "install failed" in result.stdout and "old venv kept" in result.stdout
    assert "leftovers: 0" in result.stdout, result.stdout + result.stderr


@needs_namespace
def test_root_python_ignores_modules_in_the_current_directory(env):
    marks = env["marks"]
    result = run(env, f"""
install -d -o {OTHER_UID} -m 0755 /mnt/cwd
for module in re shutil sys_hook; do
    printf 'open("{marks}/cwd-import", "a").write("%s\\n")\\n' "$module" > /mnt/cwd/$module.py
done
systemd_running() {{ false; }}
BLUEZ_CONF=/mnt/main.conf
cd /mnt/cwd
configure_bluez
cat /mnt/main.conf
""")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ExchangeMTU = 256" in result.stdout
    assert ran(result, "cwd-import") == []


def test_main_never_runs_uv_first():
    """main sets up the prefix and uv before running uv."""
    text = INSTALL_SH.read_text()
    assert "--version" not in text.split("install_uv() {", 1)[1].split("\n}\n", 1)[0]
    main = text.split("\nmain() {", 1)[1]
    steps = ("check_system", "prepare_prefix", "install_uv", "install_musegadget", "check_prefix")
    order = [main.index(step) for step in steps]
    assert order == sorted(order)


@needs_namespace
def test_check_prefix_refuses_group_writable_files(env):
    result = run(env, """
prepare_prefix
install -m 0664 /dev/null /opt/musegadget/bin/extra
check_prefix
echo "not reached"
""")
    assert result.returncode != 0
    assert "/opt/musegadget/bin/extra can be changed" in result.stderr


@pytest.mark.parametrize(
    "arch, machine, target",
    [
        ("amd64", "x86_64", "x86_64-unknown-linux-gnu"),
        ("x86_64", "x86_64", "x86_64-unknown-linux-gnu"),
        ("arm64", "aarch64", "aarch64-unknown-linux-gnu"),
        ("aarch64", "aarch64", "aarch64-unknown-linux-gnu"),
        ("armhf", "armv7l", "armv7-unknown-linux-gnueabihf"),
        ("armhf", "aarch64", "armv7-unknown-linux-gnueabihf"),
        ("armhf", "armv6l", "arm-unknown-linux-musleabihf"),
    ],
)
def test_uv_target(arch, machine, target):
    script = f'source "{INSTALL_SH}"; uname() {{ echo {machine}; }}; ARCH={arch}; uv_target'
    result = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
    assert result.stdout.strip() == target


def test_every_target_has_both_checksums():
    targets = [
        "x86_64-unknown-linux-gnu",
        "aarch64-unknown-linux-gnu",
        "armv7-unknown-linux-gnueabihf",
        "arm-unknown-linux-musleabihf",
    ]
    for target in targets:
        script = (
            f'source "{INSTALL_SH}"; uv_release {target}; '
            'echo "$UV_ARCHIVE_SHA256 $UV_BINARY_SHA256"'
        )
        out = subprocess.run(["bash", "-c", script], capture_output=True, text=True).stdout.split()
        assert len(out) == 2 and all(re.fullmatch("[0-9a-f]{64}", h) for h in out), target


def check_trust_store_in_installer(tmp_path, problem, warning, family="debian", status=0):
    """Run install.sh's check_trust_store against a fake venv Python that reports these."""
    python = tmp_path / "venv" / "bin" / "python"
    python.parent.mkdir(parents=True)
    python.write_text(f"#!/bin/sh\nprintf '%s\\n%s\\n' {shlex.quote(problem)} {shlex.quote(warning)}\n"
                      f"exit {status}\n")
    python.chmod(0o755)
    script = (
        f'source "{INSTALL_SH}"; as_root() {{ "$@"; }}; systemd_running() {{ false; }}; '
        f'VENV="{tmp_path / "venv"}"; DISTRO_FAMILY={family}; check_trust_store; echo "carried on"'
    )
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=60)


RETIRED_WARNING = ("the system trust store still trusts emSign ECC Root CA - C3, "
                   "emSign Root CA - C1, which Mozilla has retired")


def test_installer_carries_on_with_a_clean_trust_store(tmp_path):
    result = check_trust_store_in_installer(tmp_path, "", "")
    assert result.returncode == 0, result.stderr
    assert "carried on" in result.stdout
    assert result.stderr == ""


@pytest.mark.parametrize("family,hint", [
    ("debian", "put '!' in front of its line in /etc/ca-certificates.conf"),
    ("fedora", '"Root CAs" in the musegadget README'),
])
def test_installer_only_warns_about_retired_roots(tmp_path, family, hint):
    result = check_trust_store_in_installer(tmp_path, "", RETIRED_WARNING, family)
    assert result.returncode == 0, result.stderr
    assert "carried on" in result.stdout
    assert "warning:" in result.stderr and RETIRED_WARNING in result.stderr
    assert "musegadget still connects" in result.stderr and hint in result.stderr


def test_installer_points_suse_at_the_blocklist(tmp_path):
    problem = "the system trust store still trusts Comodo AAA Services root"
    result = check_trust_store_in_installer(tmp_path, problem, "", "suse")
    assert result.returncode != 0
    assert "carried on" not in result.stdout
    assert "/etc/pki/trust/blocklist/" in result.stderr and '"Root CAs"' in result.stderr


@pytest.mark.parametrize("problem,warning", [
    ("the system trust store still trusts emSign ECC Root CA - G3", RETIRED_WARNING),
    ("the system trust store still trusts emSign ECC Root CA - G3", ""),
    ("can't read the system trust store", ""),
])
def test_installer_stops_on_a_compromised_root_or_an_unreadable_store(tmp_path, problem, warning):
    result = check_trust_store_in_installer(tmp_path, problem, warning)
    assert result.returncode == 78
    assert "carried on" not in result.stdout
    assert f"musegadget won't connect: {problem}. Update your ca-certificates package" in result.stderr
    assert "still connects" not in result.stderr


def test_installer_stops_when_the_trust_store_check_fails(tmp_path):
    result = check_trust_store_in_installer(tmp_path, "", "", status=1)
    assert result.returncode != 0
    assert "carried on" not in result.stdout
    assert "couldn't check the system trust store" in result.stderr


# --- Trust store before downloading ------------------------------------------

VECTORS = Path(__file__).parent / "vectors" / "distrusted_roots.pem"


def published_roots() -> dict[str, str]:
    """Each root in tests/vectors/distrusted_roots.pem, by name, as PEM."""
    text = VECTORS.read_text()
    return dict(re.findall(r"# ([^\n]+)\n(-----BEGIN CERTIFICATE-----.+?-----END CERTIFICATE-----\n)",
                           text, re.S))


def self_signed_ca(name, public_key=None, *, ca=True):
    """A CA certificate (or not) as PEM: for its own new key, or for ``public_key`` (signed by a new key)."""
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject).issuer_name(subject)
        .public_key(public_key or key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )
    return cert.public_bytes(Encoding.PEM).decode()


def check_before_downloads(tmp_path, bundles=None, *, cert_file=True, cert_dir=None, family="debian",
                           python=None, unit=False):
    """Run check_trust_store_before_downloads with only these stores, and no network.

    ``bundles`` stand in for CA_BUNDLES. ``cert_file`` and ``cert_dir`` stand
    in for OpenSSL's default CA file and directory, the store the service
    uses; ``cert_file`` is a clean CA file by default, and False for none.
    This test's Python is given them, since the check clears the caller's
    SSL_CERT_* settings (set to elsewhere here). With ``unit``, an earlier
    install's service exists. The installer's prefix is ``prefix``. Prints
    CA_FILE after the check; ``systemctl_calls`` returns the systemctl runs.
    """
    bundles = bundles or [tmp_path / "no-such-bundle.crt"]
    if cert_file is True:
        cert_file = bundle(tmp_path, "cert.pem", self_signed_ca("Service"))
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir(exist_ok=True)
    if python is None:
        python = bin_dir / "python-with-store"
        python.write_text(
            "#!/bin/sh\n"
            f"export SSL_CERT_FILE={shlex.quote(str(cert_file or tmp_path / 'no-such-file.pem'))}\n"
            f"export SSL_CERT_DIR={shlex.quote(str(cert_dir or tmp_path / 'no-such-dir'))}\n"
            f'exec {shlex.quote(sys.executable)} "$@"\n')
        python.chmod(0o755)
    systemctl = bin_dir / "systemctl"
    systemctl.write_text(f'#!/bin/sh\necho "systemctl $*" >> {shlex.quote(str(tmp_path / "systemctl-calls"))}\n')
    systemctl.chmod(0o755)
    unit_path = tmp_path / "musegadget.service"
    if unit:
        unit_path.write_text("[Service]\n")
    (tmp_path / "prefix").mkdir(exist_ok=True)
    script = (
        f'source "{INSTALL_SH}"; as_root() {{ "$@"; }}; systemd_running() {{ true; }}; '
        f'DISTRO_FAMILY={family}; SYSTEM_PYTHON={shlex.quote(str(python))}; '
        f'UNIT={shlex.quote(str(unit_path))}; PREFIX={shlex.quote(str(tmp_path / "prefix"))}; '
        f'CA_BUNDLES=({" ".join(shlex.quote(str(path)) for path in bundles)}); '
        'check_trust_store_before_downloads; echo "carried on with CA_FILE=$CA_FILE"'
    )
    env = {
        **os.environ,
        "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
        "SSL_CERT_FILE": "/elsewhere/cert.pem",
        "SSL_CERT_DIR": "/elsewhere/certs",
        "SSL_CERT_URI": "file:///elsewhere",
    }
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=env, timeout=60)


def systemctl_calls(tmp_path):
    path = tmp_path / "systemctl-calls"
    return path.read_text().splitlines() if path.exists() else []


def bundle(directory, name, *pems):
    path = directory / name
    path.write_text("".join(pems))
    return path


def test_the_installer_refuses_the_roots_musegadget_refuses():
    script = f'source "{INSTALL_SH}"; printf "%s\\n" "${{COMPROMISED_ROOTS[@]}}"'
    lines = subprocess.run(["bash", "-c", script], capture_output=True, text=True).stdout.splitlines()
    pinned = [tuple(line.split(" ", 2)) for line in lines]
    assert pinned == [(cert, spki, name) for name, cert, spki in tls.COMPROMISED_ROOTS]


@pytest.mark.parametrize("name", ["emSign ECC Root CA - G3", "Comodo AAA Services root"])
@pytest.mark.parametrize("where", ["default file", "default directory", "CRL name"])
def test_a_compromised_root_stops_the_installer_and_the_service(tmp_path, name, where):
    pem = published_roots()[name]
    stores = {"bundles": [bundle(tmp_path, "ca-certificates.crt", self_signed_ca("Fine"))]}
    if where == "default file":
        stores["cert_file"] = bundle(tmp_path, "cert.pem", self_signed_ca("Service"), pem)
    else:
        (tmp_path / "certs").mkdir()
        bundle(tmp_path / "certs", "0a1b2c3d.0" if where == "default directory" else "0a1b2c3d.r0", pem)
        stores["cert_dir"] = tmp_path / "certs"
    result = check_before_downloads(tmp_path, unit=True, **stores)
    assert result.returncode == 78, result.stderr
    assert "carried on" not in result.stdout
    assert (f"not installing: the system trust store still trusts {name}. "
            "Update your ca-certificates package, then run this installer again.") in result.stderr
    assert systemctl_calls(tmp_path) == ["systemctl stop musegadget.service"]


def test_a_compromised_root_in_a_ca_bundle_stops_only_the_installer(tmp_path):
    # Downloads could use that bundle; the service's own store is clean, so it keeps running.
    pem = published_roots()["emSign ECC Root CA - G3"]
    result = check_before_downloads(
        tmp_path, [bundle(tmp_path, "ca-certificates.crt", self_signed_ca("Fine"), pem)], unit=True)
    assert result.returncode == 78, result.stderr
    assert "not installing: the system CA bundle still trusts emSign ECC Root CA - G3." in result.stderr
    assert systemctl_calls(tmp_path) == []


def test_the_service_store_is_judged_before_the_ca_bundle(tmp_path):
    # Its compromised root still stops the service when there's no bundle to download with.
    (tmp_path / "certs").mkdir()
    bundle(tmp_path / "certs", "0a1b2c3d.0", published_roots()["Comodo AAA Services root"])
    result = check_before_downloads(tmp_path, cert_file=False, cert_dir=tmp_path / "certs", unit=True)
    assert result.returncode == 78, result.stderr
    assert "still trusts Comodo AAA Services root." in result.stderr
    assert systemctl_calls(tmp_path) == ["systemctl stop musegadget.service"]


def test_the_installer_points_suse_at_the_blocklist_before_downloading(tmp_path):
    pem = published_roots()["Comodo AAA Services root"]
    result = check_before_downloads(tmp_path, [bundle(tmp_path, "ca-bundle.pem", pem)], family="suse")
    assert result.returncode == 78, result.stderr
    assert "still trusts Comodo AAA Services root." in result.stderr
    assert "/etc/pki/trust/blocklist/" in result.stderr and '"Root CAs"' in result.stderr


@pytest.mark.parametrize("name", ["emSign ECC Root CA - G3", "Comodo AAA Services root"])
def test_a_reissued_certificate_for_a_compromised_key_stops_the_installer(tmp_path, name):
    key = x509.load_pem_x509_certificate(published_roots()[name].encode()).public_key()
    result = check_before_downloads(tmp_path, cert_file=bundle(tmp_path, "ca.pem", self_signed_ca("Reissue", key)))
    assert result.returncode == 78, result.stderr
    assert f"still trusts {name}." in result.stderr


def test_a_compromised_key_in_a_certificate_that_is_not_a_ca_stops_the_installer(tmp_path):
    # OpenSSL leaves such a certificate out of its CAs, but uv would take it as a trust anchor.
    key = x509.load_pem_x509_certificate(published_roots()["emSign ECC Root CA - G3"].encode()).public_key()
    distro = bundle(tmp_path, "ca-certificates.crt", self_signed_ca("Fine"), self_signed_ca("Leaf", key, ca=False))
    result = check_before_downloads(tmp_path, [distro])
    assert result.returncode == 78, result.stderr
    assert "the system CA bundle still trusts emSign ECC Root CA - G3." in result.stderr


def test_a_commented_out_root_in_the_ca_bundle_is_not_a_certificate(tmp_path):
    commented = "".join("# " + line + "\n" for line in published_roots()["emSign ECC Root CA - G3"].splitlines())
    result = check_before_downloads(tmp_path, [bundle(tmp_path, "ca.pem", self_signed_ca("Fine"), commented)])
    assert result.returncode == 0, result.stderr


def test_a_rejected_ca_bundle_leaves_the_checked_copy_alone(tmp_path):
    distro = bundle(tmp_path, "ca-certificates.crt", self_signed_ca("Fine"))
    assert check_before_downloads(tmp_path, [distro]).returncode == 0
    checked = (tmp_path / "prefix" / "ca-bundle.crt").read_bytes()
    distro.write_text(self_signed_ca("Fine") + published_roots()["emSign ECC Root CA - G3"])
    assert check_before_downloads(tmp_path, [distro]).returncode == 78
    assert (tmp_path / "prefix" / "ca-bundle.crt").read_bytes() == checked
    assert [path.name for path in (tmp_path / "prefix").iterdir()] == ["ca-bundle.crt"]


def test_a_clean_store_or_retired_roots_let_the_installer_carry_on(tmp_path):
    # Retired roots are only warned about, by check_trust_store once musegadget is installed.
    roots = published_roots()
    (tmp_path / "certs").mkdir()
    bundle(tmp_path / "certs", "0a1b2c3d.0", roots["emSign Root CA - C1"])
    bundle(tmp_path / "certs", "not-a-hash.pem", roots["emSign ECC Root CA - G3"])   # never read
    distro = bundle(tmp_path, "ca.pem", self_signed_ca("Fine"), roots["emSign ECC Root CA - C3"])
    result = check_before_downloads(tmp_path, [tmp_path / "missing.crt", distro],
                                    cert_dir=tmp_path / "certs", unit=True)
    assert result.returncode == 0, result.stderr
    # Downloads use a copy of the distro's bundle, which was checked, where only root can change it.
    copy = tmp_path / "prefix" / "ca-bundle.crt"
    assert f"carried on with CA_FILE={copy}\n" in result.stdout
    assert copy.read_bytes() == distro.read_bytes() and copy.stat().st_mode & 0o777 == 0o644
    assert [path.name for path in (tmp_path / "prefix").iterdir()] == ["ca-bundle.crt"]
    assert result.stderr == ""
    assert systemctl_calls(tmp_path) == []


def test_downloads_use_openssls_default_file_without_a_distro_bundle(tmp_path):
    cert_file = bundle(tmp_path, "cert.pem", self_signed_ca("Fine"))
    result = check_before_downloads(tmp_path, cert_file=cert_file)
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "prefix" / "ca-bundle.crt").read_bytes() == cert_file.read_bytes()


@pytest.mark.parametrize("case", ["no CA file or directory", "unreadable CA file", "unreadable hashed file"])
def test_an_unreadable_service_store_stops_the_installer_and_the_service(tmp_path, case):
    stores = {"bundles": [bundle(tmp_path, "ca-certificates.crt", self_signed_ca("Fine"))]}
    if case == "no CA file or directory":
        stores["cert_file"] = False
        reason = "found no CA file or CA directory"
    elif case == "unreadable CA file":
        stores["cert_file"] = bundle(tmp_path, "cert.pem", "not a certificate\n")
        reason = "can't read"
    else:
        (tmp_path / "certs").mkdir()
        bundle(tmp_path / "certs", "0a1b2c3d.0", "not a certificate\n")
        stores["cert_dir"] = tmp_path / "certs"
        reason = "can't read"
    result = check_before_downloads(tmp_path, unit=True, **stores)
    assert result.returncode == 78
    assert "carried on" not in result.stdout
    assert f"not installing: can't read the system trust store: {reason}" in result.stderr
    assert "Update your ca-certificates package" in result.stderr
    assert systemctl_calls(tmp_path) == ["systemctl stop musegadget.service"]


@pytest.mark.parametrize("case", ["no bundle", "unreadable bundle"])
def test_a_missing_or_unreadable_ca_bundle_stops_only_the_installer(tmp_path, case):
    if case == "no bundle":   # the service reads its CA directory alone
        (tmp_path / "certs").mkdir()
        bundle(tmp_path / "certs", "0a1b2c3d.0", self_signed_ca("Fine"))
        result = check_before_downloads(tmp_path, cert_file=False, cert_dir=tmp_path / "certs", unit=True)
        reason = "found no CA bundle"
    else:
        bad = bundle(tmp_path, "ca.pem", "not a certificate\n")
        result = check_before_downloads(tmp_path, [bad], unit=True)
        reason = f"can't read {bad}"
    assert result.returncode == 78
    assert f"not installing: can't read the system CA bundle: {reason}." in result.stderr
    assert systemctl_calls(tmp_path) == []


def test_a_check_that_cannot_run_blames_neither_the_store_nor_the_service(tmp_path):
    python = tmp_path / "broken-python"
    python.write_text("#!/bin/sh\necho 'Traceback (most recent call last):' >&2\nexit 1\n")
    python.chmod(0o755)
    result = check_before_downloads(tmp_path, [bundle(tmp_path, "ca.pem", self_signed_ca("Fine"))],
                                    python=python, unit=True)
    assert result.returncode == 1
    assert "couldn't check the system trust store." in result.stderr
    assert "ca-certificates" not in result.stderr
    assert systemctl_calls(tmp_path) == []


def parity_store(tmp_path, case):
    """A store for the parity test: (default CA file or None, default CA directory)."""
    roots = published_roots()
    cert_file = bundle(tmp_path, "cert.pem", self_signed_ca("Fine"))
    certs = tmp_path / "certs"
    certs.mkdir()
    if case == "compromised in the file":
        cert_file.write_text(self_signed_ca("Fine") + roots["emSign ECC Root CA - G3"])
    elif case == "compromised in a hash-named file":
        bundle(certs, "0a1b2c3d.0", roots["Comodo AAA Services root"])
    elif case == "compromised in a CRL-named file":
        bundle(certs, "0a1b2c3d.r1", roots["emSign ECC Root CA - G3"])
    elif case == "compromised in another file":
        bundle(certs, "notes.pem", roots["emSign ECC Root CA - G3"])
    elif case == "compromised key in a certificate that is not a CA":
        key = x509.load_pem_x509_certificate(roots["emSign ECC Root CA - G3"].encode()).public_key()
        bundle(certs, "0a1b2c3d.0", self_signed_ca("Leaf", key, ca=False))
    elif case == "retired only":
        bundle(certs, "0a1b2c3d.0", roots["emSign Root CA - C1"])
    elif case == "unreadable hash-named file":
        bundle(certs, "0a1b2c3d.0", "not a certificate\n")
    elif case == "directory is a file":
        certs.rmdir()
        bundle(tmp_path, "certs", self_signed_ca("Fine"))
    elif case == "directory only":
        cert_file.unlink()
        bundle(certs, "0a1b2c3d.0", self_signed_ca("Fine"))
        cert_file = None
    elif case == "nothing":
        cert_file.unlink()
        cert_file = None
    return cert_file, certs


@pytest.mark.parametrize("case", [
    "clean", "compromised in the file", "compromised in a hash-named file",
    "compromised in a CRL-named file", "compromised in another file",
    "compromised key in a certificate that is not a CA", "retired only",
    "unreadable hash-named file", "directory is a file", "directory only", "nothing",
])
def test_the_installer_refuses_the_stores_musegadget_refuses(tmp_path, monkeypatch, case):
    """With a clean distro bundle, the check before downloading refuses exactly the stores musegadget refuses."""
    cert_file, cert_dir = parity_store(tmp_path, case)
    monkeypatch.setenv("SSL_CERT_FILE", str(cert_file or tmp_path / "no-such-file.pem"))
    monkeypatch.setenv("SSL_CERT_DIR", str(cert_dir))
    monkeypatch.delenv("SSL_CERT_URI", raising=False)
    # As tls.trust_store_problem(), which conftest.py stubs out here.
    found = tls.check_trust_store()
    compromised = {name for name, _, _ in tls.COMPROMISED_ROOTS}
    musegadget_refuses = found is None or bool(compromised & set(found))
    distro = bundle(tmp_path, "ca-certificates.crt", self_signed_ca("Distro"))
    result = check_before_downloads(tmp_path, [distro], cert_file=cert_file or False, cert_dir=cert_dir)
    assert result.returncode in (0, 78), result.stderr
    assert (result.returncode == 78) == musegadget_refuses, result.stderr


def test_uv_and_git_use_only_the_checked_bundle(tmp_path):
    """uv's root CAs and insecure hosts, git's CA settings and generic sslVerify, and SSL_CERT_DIR are overridden."""
    uv = tmp_path / "bin" / "uv"
    uv.parent.mkdir()
    uv.write_text('#!/bin/sh\necho "file=$SSL_CERT_FILE git=$GIT_SSL_CAINFO gitdir=$GIT_SSL_CAPATH '
                  'dir=${SSL_CERT_DIR-unset} noverify=${GIT_SSL_NO_VERIFY-unset} '
                  'insecure=${UV_INSECURE_HOST-unset} '
                  '$GIT_CONFIG_COUNT $GIT_CONFIG_KEY_0=$GIT_CONFIG_VALUE_0"\n')
    uv.chmod(0o755)
    script = (f'source "{INSTALL_SH}"; as_root() {{ "$@"; }}; PREFIX="{tmp_path}"; NO_CA_DIR=/empty; '
              'export SSL_CERT_DIR=/elsewhere GIT_SSL_NO_VERIFY=1 UV_INSECURE_HOST=github.com; '
              'CA_FILE=/checked/ca.crt; run_uv pip install x')
    result = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=60)
    assert result.stdout.strip() == ("file=/checked/ca.crt git=/checked/ca.crt gitdir=/empty dir=unset "
                                     "noverify=unset insecure=unset 1 http.sslVerify=true"), result.stderr
    unchecked = subprocess.run(["bash", "-c", f'source "{INSTALL_SH}"; PREFIX="{tmp_path}"; run_uv pip'],
                               capture_output=True, text=True, timeout=60)
    assert unchecked.returncode != 0 and "the trust store is not checked" in unchecked.stderr
    assert "file=" not in unchecked.stdout


def test_main_checks_the_trust_store_before_downloading():
    """Before the check, only the package manager, which checks package signatures, downloads."""
    main = INSTALL_SH.read_text().split("\nmain() {", 1)[1]
    steps = ("install_packages", "update_ca_certificates", "prepare_prefix",
             "check_trust_store_before_downloads", "install_uv", "install_musegadget", "check_trust_store")
    order = [main.index(f"\n    {step}\n") for step in steps]
    assert order == sorted(order)


# --- Proxy -------------------------------------------------------------------

PROXY = "http://user:p%40ss@proxy.example:3128"


def run_proxy(tmp_path, body):
    """Run install.sh's proxy functions with the drop-in directory under tmp_path.

    as_root runs commands directly, and install drops -o/-g (recording them) so
    this needs no root.
    """
    folder = tmp_path / "musegadget.service.d"
    script = f"""
source "{INSTALL_SH}"
as_root() {{ "$@"; }}
install() {{
    local args=()
    while [ $# -gt 0 ]; do
        case "$1" in -o|-g) echo "install $1 $2" >>"{tmp_path}/owners"; shift 2 ;; *) args+=("$1"); shift ;; esac
    done
    command install "${{args[@]}}"
}}
PROXY_DIR="{folder}" PROXY_DROPIN="{folder}/proxy.conf" PROXY_ENV="{folder}/proxy.env"
PROXY="" NO_PROXY_HOSTS="" REMOVE_PROXY=0
{body}
"""
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=60)


def test_proxy_is_saved_for_the_service_in_a_root_only_file(tmp_path):
    result = run_proxy(tmp_path, f'PROXY="{PROXY}" NO_PROXY_HOSTS="localhost,.lan"; save_proxy')
    assert result.returncode == 0, result.stderr
    folder = tmp_path / "musegadget.service.d"
    env_file, dropin = folder / "proxy.env", folder / "proxy.conf"
    assert env_file.read_text() == f"https_proxy={PROXY}\nno_proxy=localhost,.lan\n"
    assert dropin.read_text() == ("# Written by the musegadget installer (--proxy).\n"
                                  f"[Service]\nEnvironmentFile={env_file}\n")
    assert env_file.stat().st_mode & 0o777 == 0o600
    assert dropin.stat().st_mode & 0o777 == 0o644
    assert (tmp_path / "owners").read_text().count("install -o 0") == 3
    assert "http://proxy.example:3128" in result.stdout
    assert "p%40ss" not in result.stdout + result.stderr


def test_no_proxy_line_is_left_out_without_hosts(tmp_path):
    result = run_proxy(tmp_path, f'PROXY="{PROXY}"; save_proxy')
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "musegadget.service.d" / "proxy.env").read_text() == f"https_proxy={PROXY}\n"


def test_a_later_run_reuses_the_saved_proxy(tmp_path):
    result = run_proxy(tmp_path, f"""
PROXY="{PROXY}" NO_PROXY_HOSTS="localhost"; save_proxy
PROXY="" NO_PROXY_HOSTS=""; load_saved_proxy
echo "loaded $PROXY $NO_PROXY_HOSTS"
""")
    assert result.returncode == 0, result.stderr
    assert f"loaded {PROXY} localhost" in result.stdout


def test_a_given_proxy_wins_over_the_saved_one(tmp_path):
    result = run_proxy(tmp_path, f"""
PROXY="{PROXY}"; save_proxy
PROXY="http://other.example:8080"; load_saved_proxy
echo "loaded $PROXY"
""")
    assert result.returncode == 0, result.stderr
    assert "loaded http://other.example:8080" in result.stdout


def test_a_run_without_proxy_keeps_the_saved_one(tmp_path):
    result = run_proxy(tmp_path, f'PROXY="{PROXY}"; save_proxy; PROXY=""; save_proxy')
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "musegadget.service.d" / "proxy.env").read_text() == f"https_proxy={PROXY}\n"


def test_proxy_none_removes_the_saved_proxy(tmp_path):
    result = run_proxy(tmp_path, f'PROXY="{PROXY}"; save_proxy; PROXY="" REMOVE_PROXY=1; save_proxy')
    assert result.returncode == 0, result.stderr
    assert not (tmp_path / "musegadget.service.d").exists()
    assert "connects directly" in result.stdout


def test_a_tampered_saved_proxy_stops_the_installer(tmp_path):
    folder = tmp_path / "musegadget.service.d"
    folder.mkdir()
    (folder / "proxy.env").write_text("https_proxy=socks5://proxy.example:1080\n")
    result = run_proxy(tmp_path, 'load_saved_proxy; echo "carried on"')
    assert result.returncode != 0
    assert "carried on" not in result.stdout
    assert "saved proxy" in result.stderr and "is not valid" in result.stderr


@pytest.mark.parametrize("url,valid", [
    ("http://proxy.example:3128", True),
    ("http://user:p%40ss@10.0.0.1:8080", True),
    ("http://[2001:db8::1]:3128", True),
    ("https://proxy.example:3128", False),
    ("socks5://proxy.example:1080", False),
    ("proxy.example:3128", False),
    ("http://", False),
    ("http://proxy.example:3128 extra", False),
    ('http://proxy.example:3128"', False),
    ("http://user:pa$s@proxy.example", False),
    (r"http://user:pa\ss@proxy.example", False),
    ("http://proxy.example:3128\nEvil=1", False),
    ("http://proxy.example:3128/", True),
    ("http://user:pw@proxy.example:3128/path", False),
    ("http://proxy.example:3128?x=1", False),
    ("http://proxy.example:3128#x", False),
    ("http:///", False),
    ("http://proxy.example", False),
    ("http://user:pw@proxy.example", False),
    ("http://[2001:db8::1]", False),
])
def test_valid_proxy(url, valid):
    script = f'source "{INSTALL_SH}"; valid_proxy {shlex.quote(url)} && echo valid || echo invalid'
    result = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
    assert result.stdout.strip() == ("valid" if valid else "invalid")


@pytest.mark.parametrize("proxy,no_proxy,expected", [
    (PROXY, "localhost", f"{PROXY}|{PROXY}|localhost"),
    (PROXY, "", f"{PROXY}|{PROXY}|unset"),
    ("", "", "unset|unset|unset"),
])
def test_root_commands_get_the_proxy_through_sudo(proxy, no_proxy, expected):
    # The sudo stand-in clears the environment, as sudo's env_reset does, keeps
    # what --preserve-env names, and shows its command line on stderr.
    script = f"""
source "{INSTALL_SH}"
id() {{ echo 1000; }}
sudo() {{
    echo "sudo $*" >&2
    local keep=() names=() name
    case "$1" in --preserve-env=*) IFS=, read -ra names <<<"${{1#--preserve-env=}}"; shift ;; esac
    for name in ${{names[@]+"${{names[@]}}"}}; do keep+=("$name=${{!name}}"); done
    env -i PATH="$PATH" ${{keep[@]+"${{keep[@]}}"}} "$@"
}}
export https_proxy=http://leaked.example:1 http_proxy=http://leaked.example:1
PROXY="{proxy}" NO_PROXY_HOSTS="{no_proxy}"
as_root sh -c 'echo "${{http_proxy-unset}}|${{https_proxy-unset}}|${{no_proxy-unset}}"'
"""
    result = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == expected
    assert "p%40ss" not in result.stderr


@pytest.mark.parametrize("args,message", [
    (["--proxy", "socks5://proxy.example:1080"], "--proxy needs an http:// URL"),
    (["--proxy", "http://proxy.example:3128", "--no-proxy", "a b"], "--no-proxy needs a comma-separated"),
    (["--no-proxy", "localhost"], "--no-proxy goes with --proxy."),
    (["--proxy", "none", "--no-proxy", "localhost"], "not --proxy none"),
])
def test_bad_proxy_options_stop_before_anything_runs(args, message):
    # Checked before the installer asks for sudo or reads the system.
    result = subprocess.run(["bash", str(INSTALL_SH), *args], capture_output=True, text=True, timeout=60)
    assert result.returncode != 0
    assert message in result.stderr


def test_main_saves_the_proxy_before_the_service_starts():
    text = INSTALL_SH.read_text()
    main = text.split("\nmain() {", 1)[1]
    steps = ("load_saved_proxy", "check_system", "install_packages", "install_uv",
             "save_proxy", "install_service", "pair")
    order = [main.index(f"\n    {step}\n") for step in steps]
    assert order == sorted(order)
    uninstall = text.split("\nuninstall() {", 1)[1].split("\n}\n", 1)[0]
    assert "\n    remove_proxy\n" in uninstall
