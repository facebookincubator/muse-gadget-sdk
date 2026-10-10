#!/usr/bin/env python3
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

"""Put your avatar on your board: ask Muse for it, build it and flash it.

    python3 tools/muse/avatar.py                        the whole thing
    python3 tools/muse/avatar.py --edit "bigger ears"   change the avatar you have
    python3 tools/muse/avatar.py --no-flash             stop after the build
    python3 tools/muse/avatar.py --reply FILE           use a reply you got from Muse yourself

1. Finds the board on USB and checks it's connected to your Muse.
2. Sends Muse the prompt (tools/muse/avatar_prompt.md) and the current renderer
   through the board (tools/muse/chat.py), so this machine needs no token.
3. Checks the C file Muse sends back before it touches the avatar you have: it
   builds and runs it here and renders one GIF per animation, then builds the
   firmware for the board from a copy of this tree. Errors go back to Muse to
   fix, twice at most. A reply cut off on the way is never used.
4. Only once it all passes, saves it as components/muse/avatar/muse_pixel.c (the
   one it replaces becomes muse_pixel.c.prev), with its GIFs in
   components/muse/avatar/gifs/. That directory is gitignored; the build uses the
   file in place of the default avatar.
5. Flashes the firmware it built and checked. With --no-flash, that build stays
   in components/muse/avatar/firmware-BOARD/ (the hashes of its avatar and app
   image in VERIFIED there) and it says how to flash it.

--status says which avatar the next build uses and what this tool last flashed;
--restore previous|default goes back, through the same checks.

Exit status: 0 done, 1 failed, 2 no usable board, 3 the board isn't set up.
"""
import argparse
import collections
import contextlib
import fcntl
import fnmatch
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import chat  # noqa: E402
import make_gifs  # noqa: E402

ROOT = make_gifs.ROOT
AVATAR_DIR = os.path.join(ROOT, "components", "muse", "avatar")
AVATAR_SRC = make_gifs.CUSTOM_SRC
DEFAULT_SRC = make_gifs.DEFAULT_SRC
LAST_REPLY = os.path.join(AVATAR_DIR, "last_reply.md")
FLASHED = "flashed"   # in AVATAR_DIR: what this tool has flashed, one line each
BUILD_LOG = "/tmp/muse_build_{}.log"   # where board.sh build logs
LOCK = ".lock"
TMP_SUFFIX = ".tmp"
# What a copy of the tree for a firmware build leaves out: builds, downloads, and
# your avatar (the candidate goes there instead).
COPY_IGNORE = ("build", "build-*", "managed_components", "dependencies.lock", "sdkconfig", "sdkconfig.old",
               "__pycache__", ".git")
PROMPT = os.path.join(HERE, "avatar_prompt.md")
BOARD_SH = os.path.join(HERE, "board.sh")
API = ("muse_pixel_accent", "muse_pixel_render", "muse_pixel_set_size", "muse_pixel_scale")
FIX_ROUNDS = 2
ERROR_LINES = 60

# muse_board->name, as "@status" reports it -> tools/muse/board.sh's name
BOARDS = {
    "Espressif ESP32-S3-BOX-3": "box3",
    "Waveshare ESP32-S3-Touch-AMOLED-1.75C": "s3",
    "Waveshare ESP32-S3-Touch-AMOLED-1.75": "s3n",
    "Waveshare ESP32-S3-Touch-AMOLED-2.16": "s3-216",
    "AIPI Lite": "aipi",
    "Waveshare ESP32-C6-Touch-AMOLED-1.8": "c6",
    "Waveshare ESP32-C6-Touch-AMOLED-2.06": "c6-206",
    "Seeed SenseCAP Watcher": "watcher",
    "M5Stack StickS3": "sticks3",
    "M5Stack Cardputer ADV": "cardputer-adv",
    "M5Stack StickC Plus2": "plus2",
    "M5Stack StopWatch": "stopwatch",
    "M5Stack CoreS3": "cores3",
    "M5Stack Core2": "core2",
    "Freenove FNK0104B": "fnk0104b",
    "Guition JC3248W535": "jc3248w535",
    "Waveshare ESP32-S3-Touch-LCD-7": "lcd7",
    "VN ESP32-S3 1.83-inch NV3023": "vn183",
    "FoloToy AI Passport": "ai-passport",
}
CHAT_BOARDS = ("s3", "s3n", "s3-216", "aipi", "box3", "sticks3", "watcher", "stopwatch", "cores3", "core2", "fnk0104b", "jc3248w535", "lcd7", "vn183")


class Stop(Exception):
    def __init__(self, msg, code=1):
        super().__init__(msg)
        self.code = code


def say(msg):
    print(msg, file=sys.stderr, flush=True)


def rel(path):
    return os.path.relpath(path, os.getcwd())


# ---- The board ----

def open_board(port):
    """The open board and its "@status", or Stop if it doesn't answer the console."""
    board = chat.Board(port)
    st = board.status()
    if st is None:
        board.close()
        raise Stop(f"The board on {port} doesn't answer. Its firmware is probably older than serial chat: "
                   "run this again with --board s3, aipi, sticks3, stopwatch, cores3, core2 or watcher to flash it first.", 2)
    return board, st


def check_board(st):
    """Stops unless the board can chat with your Muse."""
    name = st.get("board", "?")
    dev = st.get("device", {})
    hatch, wifi = dev.get("hatch", {}), dev.get("wifi", {})
    if not st.get("chat"):
        raise Stop(f"The {name} can't chat over USB: it has no PSRAM, so it talks to Muse through Home "
                   "Link, and only in short replies. Ask Muse for the file yourself "
                   "(tools/muse/AVATAR_RECIPE.md), then run this with --reply FILE.", 2)
    # Either reaches the Muse's VM, as muse_hatch_configured() says.
    if not hatch.get("token") and not dev.get("link", {}).get("paired"):
        raise Stop(f"Your {name} isn't connected to your Muse: it isn't paired in the Muse app and has "
                   "no device token. Pair it in the Muse app, or set a device token from your phone "
                   "(tools/muse/ble_setup.html), then run this again.", 3)
    if wifi.get("state") != "connected":
        raise Stop(f"Your {name} isn't on Wi-Fi ({wifi.get('state', 'unknown')}). Set it up from your phone "
                   "(tools/muse/ble_setup.html) or over USB with '>wifi.ssid=NAME', '>wifi.pass=PASSWORD' "
                   "and '>wifi.connect', then run this again.", 3)


def summary(st):
    dev = st.get("device", {})
    return (f"{st.get('board')}: Wi-Fi {dev.get('wifi', {}).get('state')}, "
            f"Muse {dev.get('hatch', {}).get('state')}")


# ---- Muse ----

def request(edit):
    """The message for Muse: the prompt, then the renderer to start from."""
    with open(PROMPT, encoding="utf-8") as f:
        prompt = f.read().strip()
    if edit:
        with open(AVATAR_SRC, encoding="utf-8") as f:
            current = f.read()
        return (f"{prompt}\n\nYou already drew yourself: that's the muse_pixel.c below. Change only this, "
                f"keep everything else as it is, and send the whole file back:\n{edit}\n\n```c\n{current}```\n")
    with open(make_gifs.DEFAULT_SRC, encoding="utf-8") as f:
        default = f.read()
    return f"{prompt}\n\nThe current muse_pixel.c (the default avatar):\n\n```c\n{default}```\n"


def ask(board, text, what):
    """Sends `text` through the board and returns Muse's reply, showing progress."""
    got = [0]
    start = time.monotonic()

    def progress(f):
        t = f.get("type")
        if t in ("text", "final"):
            got[0] += len(f.get("text", ""))
            sys.stderr.write(f"\r  {what}: {got[0]} characters, {time.monotonic() - start:.0f} s ")
            sys.stderr.flush()
        elif t == "sent":
            say(f"  sent {f.get('bytes')} bytes; waiting for Muse")
        elif t == "busy" and f.get("on"):
            say("  Muse is working on it")

    try:
        reply = board.chat(text, progress)
    except KeyboardInterrupt:
        board.cancel()
        raise
    except chat.BoardError as e:
        if "NOT SET UP" in str(e) or "CAN'T REACH" in str(e):   # older firmware too
            raise Stop(f"{e}. The board can't reach your Muse: check it's paired in the Muse app, or check "
                       "its device token (and VM, if set) in tools/muse/ble_setup.html, then run this again.", 3)
        raise
    finally:
        if got[0]:
            sys.stderr.write("\n")
    if not reply.intact() or not reply.complete:
        save_reply(reply.text)
        why = (f"{reply.lost} console line(s) went missing on the way" if not reply.intact()
               else "it was cut off before Muse finished")
        raise Stop(f"Muse's reply didn't arrive whole ({why}), so it isn't used and your avatar is as it was. "
                   f"It's saved in {rel(LAST_REPLY)}. Run this again; if you're sure the file in it is "
                   "complete, pass it with --reply.")
    return reply.text


def extract_c(reply):
    """The C file in a reply: the largest fenced block with the whole API, or the reply itself."""
    blocks = re.findall(r"^```[ \t]*[\w+]*[ \t]*\n(.*?)^```", reply, re.S | re.M)
    if reply.count("```") % 2:
        last = reply.rfind("```")
        blocks.append(reply[reply.find("\n", last) + 1:] if "\n" in reply[last:] else "")   # never closed
    if "```" not in reply:
        blocks.append(reply)
    files = [b.strip() + "\n" for b in blocks if all(f in b for f in API) and "muse_pixel.h" in b]
    return max(files, key=len) if files else None


def description(code):
    """The comment the prompt asks for, describing the avatar, right below the copyright header."""
    m = re.match(r"\s*(?:/\*.*?(?:Copyright|License).*?\*/|//[^\n]*Copyright[^\n]*\n)"
                 r"\s*(/\*.*?\*/|(?://[^\n]*\n\s*)+)", code, re.S)
    if not m:
        return None
    lines = (re.sub(r"^\s*(/\*+|\*+/|\*|//)|\*+/\s*$", "", l) for l in m.group(1).splitlines())
    return " ".join(" ".join(lines).split()) or None


# ---- Build ----

def clip(out, n=ERROR_LINES):
    """The first `n` lines of compiler output, with paths relative to esp32/ (they go to Muse)."""
    return "\n".join([l for l in out.replace(ROOT + os.sep, "").splitlines() if l.strip()][:n])


def host_check(src, gifs_dir=None):
    """Builds `src` against tools/muse/anim.c and runs it through every animation.

    Returns (errors, gifs): the compiler's or sanitizer's complaints (None if it's
    fine) and the GIF previews made from it, in `gifs_dir` (components/muse/avatar/gifs/
    if not given).
    """
    cc = ["cc", "-O1", "-g", "-Wall", "-Werror", "-I", "components/muse", "tools/muse/anim.c", src, "-lm"]
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "anim")
        p = subprocess.run(cc + ["-o", exe], cwd=ROOT, capture_output=True, text=True)
        if p.returncode:
            return clip(p.stdout + p.stderr), None
        # Out-of-bounds writes corrupt memory on the board rather than crash, so look for them here.
        san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        if subprocess.run(cc + san + ["-o", exe], cwd=ROOT, capture_output=True).returncode == 0:
            os.makedirs(os.path.join(tmp, "frames"))
            p = subprocess.run([exe, os.path.join(tmp, "frames")], capture_output=True, text=True)
            if p.returncode:
                return "It crashed while drawing the animations:\n" + clip(p.stderr, 30), None
    try:
        return None, make_gifs.render(src, gifs_dir or os.path.join(AVATAR_DIR, "gifs"))
    except ImportError:
        say("  (no GIF previews without Pillow: python3 -m pip install pillow)")
    except subprocess.CalledProcessError as e:
        return clip((e.stdout or "") + (e.stderr or "")) or f"It exits with status {e.returncode}.", None
    return None, []


def build_errors(key):
    """What in the firmware build log is about the avatar, or None if the failure is elsewhere."""
    try:
        with open(BUILD_LOG.format(key), encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    keep = []
    for i, l in enumerate(lines):
        if "avatar/muse_pixel.c:" in l and re.search(r"(error|warning):", l):
            keep += lines[i:i + 4]   # the message, then the source line and the caret
    return clip("\n".join(keep)) or None


def board_sh(*args, root=ROOT):
    """Runs tools/muse/board.sh of the tree at `root`; returns (ok, output)."""
    p = subprocess.run([os.path.join(root, "tools", "muse", "board.sh"), *args], cwd=root, capture_output=True,
                       text=True)
    return p.returncode == 0, (p.stdout + p.stderr).strip()


def build(key, root=ROOT):
    say(f"Building the {key} firmware (a few minutes; log in {BUILD_LOG.format(key)})")
    return board_sh("build", key, root=root)


def copy_tree(dest):
    """This tree, without builds or your avatar, at `dest`: a candidate builds there, leaving yours alone."""
    shutil.copytree(ROOT, dest, symlinks=True, ignore=lambda d, names: [
        n for n in names if any(fnmatch.fnmatch(n, pat) for pat in COPY_IGNORE)
        or os.path.join(d, n) in (AVATAR_DIR, os.path.join(ROOT, "components", "muse", "avatar"))])
    return dest


def build_dir(key, root=ROOT):
    """board.sh's build directory for `key` (build-muse-PROFILE, with -bench under MUSE_BENCH)."""
    with open(os.path.join(root, "tools", "muse", "board.sh"), encoding="utf-8") as f:
        m = re.search(rf"^\s*{re.escape(key)}\)\s+profile=([\w.-]+);", f.read(), re.M)
    if not m:
        raise Stop(f"tools/muse/board.sh has no board {key}.", 2)
    return f"build-muse-{m.group(1)}" + ("-bench" if os.environ.get("MUSE_BENCH") else "")


def build_candidate(src, key, work):
    """Builds the firmware for `key` with `src` as the avatar (the default one if None), in a copy
    of this tree under `work`.

    It reuses the configuration of your own build for that board (build-muse-*/sdkconfig: your
    SDK token, Wi-Fi and options); without one, it's configured from the defaults, as a first
    build of yours would be.
    Returns (ok, output, root): root is the copy, whose build is the one to flash.
    """
    root = copy_tree(os.path.join(work, "esp32"))
    b = build_dir(key)
    mine = os.path.join(ROOT, b, "sdkconfig")
    if os.path.exists(mine):
        os.makedirs(os.path.join(root, b), mode=0o700)
        shutil.copy2(mine, os.path.join(root, b, "sdkconfig"))   # never printed: it can hold your token
        say(f"  with the configuration of your {b} build")
    else:
        say(f"  no {b} build here yet: configured from the defaults")
    if src:
        os.makedirs(os.path.join(root, "components", "muse", "avatar"))
        shutil.copyfile(src, os.path.join(root, "components", "muse", "avatar", "muse_pixel.c"))
    ok, out = build(key, root)
    return ok, out, root


def flash_command(key, port, root=ROOT):
    return f"{rel(os.path.join(root, 'tools', 'muse', 'board.sh'))} flash {key} {port or 'PORT'}"


def flash(key, port, root=ROOT):
    say(f"Flashing {port}")
    ok, out = board_sh("flash", key, port, root=root)
    if not ok:
        raise Stop(out + "\n\nFlashing failed. Close anything else using the port, or hold BOOT, tap RESET, "
                   f"release BOOT, and run: {flash_command(key, port, root)}")


# ---- Your avatar's files ----
#
# A file changes by writing a new one beside it and renaming it into place, so a
# crash leaves the old file or the new one, never part of either. The rename gives
# it a fresh time, so a build never mistakes it for the one it replaced.

def sync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_atomic(path, text):
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix="." + os.path.basename(path) + ".", suffix=TMP_SUFFIX)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        raise
    sync_dir(d)


def recover():
    """Clears what an interrupted run left half written. Your avatar files are whole either way."""
    if not os.path.isdir(AVATAR_DIR):
        return
    for name in os.listdir(AVATAR_DIR):
        if name.startswith(".") and name.endswith(TMP_SUFFIX):
            path = os.path.join(AVATAR_DIR, name)
            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)   # a candidate's GIFs
            else:
                os.unlink(path)


@contextlib.contextmanager
def lock():
    """One avatar.py at a time works on components/muse/avatar/ (a lock the system drops if it dies)."""
    os.makedirs(AVATAR_DIR, exist_ok=True)
    fd = os.open(os.path.join(AVATAR_DIR, LOCK), os.O_CREAT | os.O_RDWR, 0o644)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise Stop(f"Another avatar.py is working on {rel(AVATAR_DIR)}. Let it finish, then run this again.",
                       2) from None
        recover()
        yield
    finally:
        os.close(fd)


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def image_path(key, root):
    """The app image board.sh flashes from the build of `key` in the tree at `root`, if built."""
    b = os.path.join(root, build_dir(key, root))
    try:
        with open(os.path.join(b, "project_description.json"), encoding="utf-8") as f:
            path = os.path.join(b, json.load(f)["app_bin"])
    except (OSError, ValueError, KeyError):
        return None
    return path if os.path.isfile(path) else None


def renderer(custom, default):
    """Which avatar a build with yours at `custom` and the default at `default` draws, and its sha256:
    {"renderer": "custom" or "default", "renderer_sha256": ...}."""
    if os.path.exists(custom):
        return {"renderer": "custom", "renderer_sha256": sha256(custom)}
    return {"renderer": "default", "renderer_sha256": sha256(default) if os.path.exists(default) else None}


def current():
    """The avatar the next build here draws."""
    return renderer(AVATAR_SRC, DEFAULT_SRC)


def built_with(root):
    """The avatar the tree at `root` builds."""
    return renderer(os.path.join(root, "components", "muse", "avatar", "muse_pixel.c"),
                    os.path.join(root, "avatar", "muse_pixel.c"))


def kept_dir(key):
    """Where the firmware checked with your avatar stays until it's flashed (gitignored)."""
    return os.path.join(AVATAR_DIR, f"firmware-{key}")


def checked_build(root, key, meant):
    """The hashes of the avatar `meant` ({"renderer", "renderer_sha256"}, as checked) and of the app
    image built with it in `root`, or None if that build has another avatar or made no image."""
    built = built_with(root)
    image = image_path(key, root)
    if image is None or built != meant:
        return None
    return {"board": key, **built, "image_sha256": sha256(image)}


def kept_record(dest):
    try:
        with open(os.path.join(dest, "VERIFIED"), encoding="utf-8") as f:
            return dict(line.split(" ", 1) for line in f.read().splitlines() if " " in line)
    except OSError:
        return None


def drop_kept():
    """Removes kept builds of an avatar other than the one you have now: they'd flash that one."""
    now = current()
    for d in sorted(glob.glob(os.path.join(AVATAR_DIR, "firmware-*"))):
        rec = kept_record(d) or {}
        if {k: rec.get(k) for k in now} != now:
            shutil.rmtree(d, ignore_errors=True)
            say(f"  removed {rel(d)}: it was built with the avatar before")


def keep_build(root, record):
    """Keeps the checked build `root` of your avatar, now promoted, as kept_dir(board), with its
    hashes in VERIFIED there, in place of an older one. Other boards' builds of an older avatar go."""
    dest = kept_dir(record["board"])
    shutil.rmtree(dest, ignore_errors=True)
    drop_kept()
    os.replace(root, dest)   # same filesystem: the work was under components/muse/avatar/ too
    write_atomic(os.path.join(dest, "VERIFIED"), "".join(f"{k} {v}\n" for k, v in record.items()))
    return dest


def verify_kept(key):
    """Checks, just before flashing, that the kept build of `key` is still the one checked with
    the avatar you have: same renderer, same app image. Returns its directory."""
    dest = kept_dir(key)
    record = kept_record(dest)
    if record is None:
        raise Stop(f"There's no checked build in {rel(dest)}. Run this again with --board {key}.")
    image = image_path(key, dest)
    now = {"board": key, **current(), "image_sha256": sha256(image) if image else None}
    if record != now or {**built_with(dest), "board": key, "image_sha256": now["image_sha256"]} != now:
        raise Stop(f"The build in {rel(dest)} isn't the one checked with your avatar any more, so it "
                   f"isn't flashed. Run this again with --board {key}.")
    return dest


def new_work():
    """A scratch directory beside your avatar, cleared by recover() if a run dies."""
    os.makedirs(AVATAR_DIR, exist_ok=True)
    return tempfile.mkdtemp(dir=AVATAR_DIR, prefix=".work-", suffix=TMP_SUFFIX)


def save_reply(reply):
    write_atomic(LAST_REPLY, reply)


def promote(code, gifs_dir):
    """Makes the checked `code` your avatar (None: the default one), keeping the one it replaces
    as muse_pixel.c.prev."""
    if os.path.exists(AVATAR_SRC):
        with open(AVATAR_SRC, encoding="utf-8") as f:
            write_atomic(AVATAR_SRC + ".prev", f.read())
    if code is None:
        if os.path.exists(AVATAR_SRC):
            os.remove(AVATAR_SRC)
            sync_dir(AVATAR_DIR)
    else:
        write_atomic(AVATAR_SRC, code)
    if gifs_dir and os.path.isdir(gifs_dir):
        gifs = os.path.join(AVATAR_DIR, "gifs")
        shutil.rmtree(gifs, ignore_errors=True)
        os.replace(gifs_dir, gifs)


Made = collections.namedtuple("Made", "gifs root")   # root: the tree whose firmware build has it, or None


def install(code, key, work):
    """Checks `code` (None: the default avatar) here, and with `key` builds the firmware with it in a
    copy of this tree under `work`; only if it all passes does it become your avatar.
    Returns (Made, None), or (None, the errors in the avatar). A failure elsewhere stops."""
    round_dir = tempfile.mkdtemp(prefix="round-", dir=work)
    src = None
    if code is not None:
        src = os.path.join(round_dir, "muse_pixel.c")
        with open(src, "w", encoding="utf-8") as f:
            f.write(code)
        meant = {"renderer": "custom", "renderer_sha256": sha256(src)}   # a copy only this run writes
    else:
        # The default is a file in the tree anyone can edit, and this doesn't lock it: what's checked is
        # its hash now, which the build and the switch to it must still have (else it stops).
        meant = {"renderer": "default", "renderer_sha256": sha256(DEFAULT_SRC)}
    gifs_dir = os.path.join(AVATAR_DIR, f".gifs-{os.path.basename(round_dir)}{TMP_SUFFIX}")
    errors, gifs = host_check(src or DEFAULT_SRC, gifs_dir)
    root = record = None
    if not errors and key:
        ok, out, root = build_candidate(src, key, round_dir)
        if not ok:
            errors = build_errors(key) if src else None
            if not errors:
                raise Stop(out + f"\n\nThe firmware build failed, but not in the avatar. See "
                           f"{BUILD_LOG.format(key)}. Your avatar is as it was.")
        else:
            record = checked_build(root, key, meant)
            if record is None:
                raise Stop("The firmware built, but there's no app image of it with this avatar "
                           "to flash, so it isn't used. Your avatar is as it was.")
    if errors:
        shutil.rmtree(gifs_dir, ignore_errors=True)
        return None, errors
    if code is None and sha256(DEFAULT_SRC) != meant["renderer_sha256"]:
        shutil.rmtree(gifs_dir, ignore_errors=True)
        raise Stop(f"{rel(DEFAULT_SRC)} changed while it was being checked, so your avatar is as it was. "
                   "Run this again.")
    had = os.path.exists(AVATAR_SRC)
    promote(code, gifs_dir)
    if code is None and not had:
        say("You have no avatar of your own: builds use the default one.")
    elif code is None:
        say(f"Removed {rel(AVATAR_SRC)}: builds use the default avatar (yours is "
            f"{rel(AVATAR_SRC + '.prev')})")
    else:
        say(f"Saved {rel(AVATAR_SRC)}" + (f" (the one before is {rel(AVATAR_SRC + '.prev')})"
                                          if os.path.exists(AVATAR_SRC + ".prev") else ""))
    gifs = [(os.path.join(AVATAR_DIR, "gifs", os.path.basename(p)), n) for p, n in gifs or []]
    if record is None:
        drop_kept()
        return Made(gifs, None), None
    return Made(gifs, keep_build(root, record)), None


def make_avatar(board, key, reply, work=None):
    """Checks the avatar in `reply` (and builds the firmware for `key`'s board with it, in a copy
    of this tree under `work`, if given), sending errors back to Muse through `board`, if any.
    Only once it passes does it become your avatar. Returns Made(GIF previews, built tree)."""
    own = work is None
    work = work or new_work()
    try:
        for attempt in range(FIX_ROUNDS + 1):
            save_reply(reply)
            if reply.strip().startswith("NO AVATAR"):
                raise Stop("Muse couldn't find your avatar: " + reply.strip()[len("NO AVATAR"):].lstrip(": ") +
                           "\nSet one in Muse, then run this again.")
            code = extract_c(reply)
            if code is None:
                raise Stop(f"Muse's reply has no muse_pixel.c in it. It's saved in {rel(LAST_REPLY)}.")
            say(f"Checking the muse_pixel.c Muse sent ({len(code.encode())} bytes)")
            if attempt == 0 and description(code):
                say(f"  Muse drew: {description(code)[:300]}")
            made, errors = install(code, key, work)
            if made:
                return made
            if board is None or attempt == FIX_ROUNDS:
                raise Stop(errors + "\n\nThe muse_pixel.c Muse sent doesn't build or run, so your avatar is as it "
                           f"was. Muse's reply is in {rel(LAST_REPLY)}.")
            say("It doesn't work yet; sending the errors back to Muse")
            reply = ask(board, "That muse_pixel.c doesn't work:\n\n```\n" + errors + "\n```\n\nFix it and send "
                        "the whole file again, the same way: one ```c block and nothing after it.", "fix")
    finally:
        if own:
            shutil.rmtree(work, ignore_errors=True)



def restore(which, key, work):
    """Goes back to your previous avatar (muse_pixel.c.prev, checked like a new one; the one you
    have becomes the previous) or to the default one. Returns Made."""
    if which == "previous":
        if not os.path.exists(AVATAR_SRC + ".prev"):
            raise Stop(f"There's no previous avatar ({rel(AVATAR_SRC + '.prev')}) to go back to.")
        with open(AVATAR_SRC + ".prev", encoding="utf-8") as f:
            code = f.read()
        say(f"Checking your previous avatar ({len(code.encode())} bytes)")
    else:
        code = None
        say("Going back to the default avatar")
    made, errors = install(code, key, work)
    if errors:
        raise Stop(errors + f"\n\nThe {which} avatar doesn't build or run here, so your avatar is as it was.")
    return made


def record_flash(key, port, record):
    """Adds what was just flashed to FLASHED: a record of what this tool wrote, not of the board."""
    path, old = os.path.join(AVATAR_DIR, FLASHED), ""
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            old = f.read()
    when = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    write_atomic(path, old + f"{when} board={key} port={port} renderer={record['renderer']} "
                               f"renderer_sha256={record['renderer_sha256']} "
                               f"image_sha256={record['image_sha256']}\n")


def last_flashes():
    """The last FLASHED record of each board, as dicts."""
    last = {}
    try:
        with open(os.path.join(AVATAR_DIR, FLASHED), encoding="utf-8") as f:
            for line in f:
                when, *fields = line.split()
                rec = dict(x.split("=", 1) for x in fields if "=" in x)
                rec["when"] = when
                last[rec.get("board")] = rec
    except OSError:
        pass
    return last


def short(sha):
    return (sha or "?")[:12]


def described(rec):
    return f"{'your avatar' if rec.get('renderer') == 'custom' else 'the default avatar'} " \
           f"(sha256 {short(rec.get('renderer_sha256'))})"


def status():
    """Says, from this computer alone, which avatar the next build uses, and what this tool flashed."""
    now = current()
    say("On this computer (what the next firmware build uses):")
    src = AVATAR_SRC if now["renderer"] == "custom" else DEFAULT_SRC
    with open(src, encoding="utf-8") as f:
        what = description(f.read())
    say(f"  {described(now)}, {rel(src)}" + (f": {what[:120]}" if what else ""))
    prev = AVATAR_SRC + ".prev"
    say(f"  previous: {rel(prev)} (sha256 {short(sha256(prev))})" if os.path.exists(prev) else "  no previous avatar")
    for d in sorted(glob.glob(os.path.join(AVATAR_DIR, "firmware-*"))):
        key = os.path.basename(d)[len("firmware-"):]
        try:
            verify_kept(key)
            say(f"  retained checked build for {key}: {rel(d)} (has the avatar here)")
        except Stop:
            say(f"  build for {key} in {rel(d)} doesn't match the avatar here any more; it won't be flashed")
    last = last_flashes()
    say("Last flashed by this tool (what it wrote, not what the board runs now):")
    if not last:
        say("  nothing yet")
    for key, rec in sorted(last.items(), key=lambda kv: str(kv[0])):
        same = {k: rec.get(k) for k in now} == now
        say(f"  {key} on {rec.get('port')} at {rec['when']}: {described(rec)}, image {short(rec.get('image_sha256'))}"
            + (" (the avatar here now)" if same else " (not the avatar here now)"))
    say("The board doesn't report which image it runs, so this can't tell what the board plugged in shows: "
        "it may have been flashed since, by another tool or computer.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="the board's serial port (found by itself when there's one board)")
    ap.add_argument("--board", choices=sorted(set(BOARDS.values())),
                    help="the board, if it doesn't answer yet: flashes s3, aipi, box3, sticks3, stopwatch, cores3, core2 or watcher firmware with serial "
                         "chat first, or with --reply or --restore, the firmware to build")
    ap.add_argument("--edit", metavar="CHANGE", help="ask Muse to change the avatar you have, not redraw it")
    ap.add_argument("--reply", metavar="FILE", help="use this reply from Muse instead of asking through the board")
    ap.add_argument("--no-flash", action="store_true", help="stop after building the firmware, and keep the build to flash")
    ap.add_argument("--status", action="store_true",
                    help="say which avatar the next build uses and what this tool last flashed (needs no board)")
    ap.add_argument("--restore", choices=("previous", "default"),
                    help="go back to your previous avatar or to the default one, checked, built and flashed like a new one")
    args = ap.parse_args()
    if sum(map(bool, (args.status, args.restore, args.reply, args.edit))) > 1:
        raise Stop("Use one of --status, --restore, --reply and --edit at a time.", 2)
    with lock():   # status reads, and --edit checks, what a run beside it could be changing
        if args.status:
            status()
            return
        if args.edit and not os.path.exists(AVATAR_SRC):
            raise Stop(f"You have no avatar to change yet ({rel(AVATAR_SRC)}). Run this without --edit first.", 2)
        run(args)


def run(args):
    board, key, port = None, args.board, args.port
    work = new_work()
    try:
        local = args.reply or args.restore   # the avatar comes from this computer, not from Muse
        if not (local and args.no_flash):
            port = port or chat.pick_port()
            say(f"Board on {port}")
            try:
                board, st = open_board(port)
            except Stop:
                if local and args.board:
                    st = None   # it gets flashed anyway; it needn't answer
                elif args.board in CHAT_BOARDS:
                    ok, out = build(args.board)
                    if not ok:
                        raise Stop(out + "\n\nThe firmware doesn't build.")
                    flash(args.board, port)
                    time.sleep(6)
                    board, st = open_board(port)
                else:
                    raise
            if st:
                say("  " + summary(st))
                key = BOARDS.get(st.get("board"), key)
                if not local:
                    check_board(st)

        if args.restore:
            made = restore(args.restore, key, work)
        elif args.reply:
            with open(args.reply, encoding="utf-8") as f:
                made = make_avatar(None, key, f.read(), work)
        else:
            say("Asking your Muse for your avatar (this takes a few minutes)")
            made = make_avatar(board, key, ask(board, request(args.edit), "reply"), work)
        for path, n in made.gifs:
            say(f"  preview: {rel(path)} ({n} frames)")
        if not key:
            say("Built and checked here. Pass --board to build the firmware too.")
            return
        if args.no_flash:
            say(f"Firmware built and checked, kept in {rel(made.root)}. Flash it with: "
                f"{flash_command(key, port, made.root)}\n(Running this again replaces it; "
                f"rm -r {rel(made.root)} removes it.)")
            return
        if board:
            board.close()
            board = None
        kept = verify_kept(key)
        record = kept_record(kept)
        flash(key, port, kept)   # a failure keeps it, and says how to flash it
        record_flash(key, port, record)
        shutil.rmtree(kept, ignore_errors=True)
        say(f"Flashed {port} with the build checked with your avatar (image sha256 "
            f"{short(record['image_sha256'])}).")
        time.sleep(6)
        try:
            with chat.Board(port) as b:
                st = b.status(timeout=5)
        except chat.BoardError:
            st = None
        # Its answer says it booted, not which image it runs: look at the screen for the avatar.
        say(f"The board answered after booting ({summary(st)})." if st else
            "The board hasn't answered since; it may still be booting.")
    finally:
        if board:
            board.close()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except chat.BoardError as e:
        say(str(e))
        sys.exit(2)
    except Stop as e:
        say(str(e))
        sys.exit(e.code)
    except KeyboardInterrupt:
        sys.exit(130)
