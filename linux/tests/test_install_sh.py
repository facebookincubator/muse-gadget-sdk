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

import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

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
        'out=""; url=""\n'
        'while [ $# -gt 0 ]; do\n'
        '    case "$1" in\n'
        '        -o|--proto) [ "$1" = -o ] && out="$2"; shift 2 ;;\n'
        '        -*) shift ;;\n'
        '        *) url="$1"; shift ;;\n'
        '    esac\n'
        'done\n'
        f'echo "$url" >> {work}/curl-calls\n'
        f'echo "$out" >> {work}/curl-outs\n'
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
ARCH=amd64 UV_TMP=""
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
    result = run(env, "prepare_prefix\ninstall_uv\nshow /opt/musegadget /opt/musegadget/bin\n" + AFTER)
    assert_installed_uv(env, result)
    for path in ("/opt/musegadget", "/opt/musegadget/bin"):
        made = stat_of(result.stdout, path)
        assert made["uid"] == "0" and made["mode"] == "755" and made["type"] == "directory"
    assert curl_calls(env) == [
        f"https://github.com/astral-sh/uv/releases/download/0.9.9/uv-{TARGET}.tar.gz"
    ]
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
