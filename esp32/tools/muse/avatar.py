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
3. Saves the C file Muse sends back as components/muse/avatar/muse_pixel.c.
   That directory is gitignored; the build uses the file in place of the default avatar.
4. Builds and runs it here, and renders one GIF per animation to
   components/muse/avatar/gifs/. Errors go back to Muse to fix, twice at most.
5. Builds the firmware for the board and flashes it.

Exit status: 0 done, 1 failed, 2 no usable board, 3 the board isn't set up.
"""
import argparse
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
LAST_REPLY = os.path.join(AVATAR_DIR, "last_reply.md")
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
    "OSTB EchoEar-2ST": "echoear",
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
CHAT_BOARDS = ("s3", "s3n", "s3-216", "aipi", "box3", "sticks3", "watcher", "stopwatch", "cores3", "core2", "fnk0104b", "jc3248w535", "lcd7", "vn183", "echoear")


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
    if not reply.intact():
        say(f"  warning: {reply.lost} console line(s) went missing on the way")
    if not reply.complete:
        say("  warning: the reply was cut off before Muse finished")
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


def host_check(src):
    """Builds `src` against tools/muse/anim.c and runs it through every animation.

    Returns (errors, gifs): the compiler's or sanitizer's complaints (None if it's
    fine) and the GIF previews made from it.
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
        return None, make_gifs.render(src, os.path.join(AVATAR_DIR, "gifs"))
    except ImportError:
        say("  (no GIF previews without Pillow: python3 -m pip install pillow)")
    except subprocess.CalledProcessError as e:
        return clip((e.stdout or "") + (e.stderr or "")) or f"It exits with status {e.returncode}.", None
    return None, []


def build_errors(key):
    """What in the firmware build log is about the avatar, or None if the failure is elsewhere."""
    try:
        with open(f"/tmp/muse_build_{key}.log", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    keep = []
    for i, l in enumerate(lines):
        if "avatar/muse_pixel.c:" in l and re.search(r"(error|warning):", l):
            keep += lines[i:i + 4]   # the message, then the source line and the caret
    return clip("\n".join(keep)) or None


def board_sh(*args):
    """Runs tools/muse/board.sh; returns (ok, output)."""
    p = subprocess.run([BOARD_SH, *args], cwd=ROOT, capture_output=True, text=True)
    return p.returncode == 0, (p.stdout + p.stderr).strip()


def build(key):
    say(f"Building the {key} firmware (a few minutes; log in /tmp/muse_build_{key}.log)")
    return board_sh("build", key)


def flash(key, port):
    say(f"Flashing {port}")
    ok, out = board_sh("flash", key, port)
    if not ok:
        raise Stop(out + "\n\nFlashing failed. Close anything else using the port, or hold BOOT, tap RESET, "
                   f"release BOOT, and run: tools/muse/board.sh flash {key} {port}")


def save(code, reply):
    os.makedirs(AVATAR_DIR, exist_ok=True)
    with open(LAST_REPLY, "w", encoding="utf-8") as f:
        f.write(reply)
    if code is None:
        return
    if os.path.exists(AVATAR_SRC):
        shutil.copyfile(AVATAR_SRC, AVATAR_SRC + ".prev")
    with open(AVATAR_SRC, "w", encoding="utf-8") as f:
        f.write(code)


def make_avatar(board, key, reply):
    """Saves the avatar in `reply` and checks that it builds (for `key`'s board too, if
    given), sending errors back to Muse through `board`, if any. Returns the GIF previews."""
    for attempt in range(FIX_ROUNDS + 1):
        if reply.strip().startswith("NO AVATAR"):
            save(None, reply)
            raise Stop("Muse couldn't find your avatar: " + reply.strip()[len("NO AVATAR"):].lstrip(": ") +
                       "\nSet one in Muse, then run this again.")
        code = extract_c(reply)
        save(code, reply)
        if code is None:
            raise Stop(f"Muse's reply has no muse_pixel.c in it. It's saved in {rel(LAST_REPLY)}.")
        say(f"Saved {rel(AVATAR_SRC)} ({len(code.encode())} bytes)")
        if attempt == 0 and description(code):
            say(f"  Muse drew: {description(code)[:300]}")
        errors, gifs = host_check(AVATAR_SRC)
        if not errors and key:
            ok, out = build(key)
            if not ok:
                errors = build_errors(key)
                if not errors:
                    raise Stop(out + f"\n\nThe firmware build failed, but not in the avatar. See "
                               f"/tmp/muse_build_{key}.log.")
        if not errors:
            return gifs
        if board is None or attempt == FIX_ROUNDS:
            raise Stop(errors + f"\n\n{rel(AVATAR_SRC)} doesn't build or run. Ask Muse to fix it, or go back "
                       "to the last one: it's muse_pixel.c.prev, in the same directory.")
        say("It doesn't work yet; sending the errors back to Muse")
        reply = ask(board, "That muse_pixel.c doesn't work:\n\n```\n" + errors + "\n```\n\nFix it and send "
                    "the whole file again, the same way: one ```c block and nothing after it.", "fix")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", help="the board's serial port (found by itself when there's one board)")
    ap.add_argument("--board", choices=sorted(set(BOARDS.values())),
                    help="the board, if it doesn't answer yet: flashes s3, aipi, box3, sticks3, stopwatch, cores3, core2 or watcher firmware with serial "
                         "chat first, or with --reply, the firmware to build")
    ap.add_argument("--edit", metavar="CHANGE", help="ask Muse to change the avatar you have, not redraw it")
    ap.add_argument("--reply", metavar="FILE", help="use this reply from Muse instead of asking through the board")
    ap.add_argument("--no-flash", action="store_true", help="stop after building the firmware")
    args = ap.parse_args()
    if args.edit and not os.path.exists(AVATAR_SRC):
        raise Stop(f"You have no avatar to change yet ({rel(AVATAR_SRC)}). Run this without --edit first.", 2)

    board, key, port = None, args.board, args.port
    try:
        if not (args.reply and args.no_flash):
            port = port or chat.pick_port()
            say(f"Board on {port}")
            try:
                board, st = open_board(port)
            except Stop:
                if args.reply and args.board:
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
                if not args.reply:
                    check_board(st)

        if args.reply:
            with open(args.reply, encoding="utf-8") as f:
                gifs = make_avatar(None, key, f.read())
        else:
            say("Asking your Muse for your avatar (this takes a few minutes)")
            gifs = make_avatar(board, key, ask(board, request(args.edit), "reply"))
        for path, n in gifs:
            say(f"  preview: {rel(path)} ({n} frames)")
        if not key:
            say("Built and checked here. Pass --board to build the firmware too.")
            return
        if args.no_flash:
            say(f"Firmware built. Flash it with: tools/muse/board.sh flash {key} {port or 'PORT'}")
            return
        if board:
            board.close()
            board = None
        flash(key, port)
        time.sleep(6)
        try:
            with chat.Board(port) as b:
                st = b.status(timeout=5)
        except chat.BoardError:
            st = None
        say("Done: your avatar is on the board." if st else
            "Flashed. The board hasn't answered yet; your avatar shows once it has booted.")
    finally:
        if board:
            board.close()


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
