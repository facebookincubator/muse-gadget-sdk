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

"""A musegadgets BLE client for bench tests (protocols/mgcommands.h).

    python3 tools/mg_ble_client.py scan                   gadgets advertising the service
    python3 tools/mg_ble_client.py test [--secs 2]        the protocol checks below, and a WAV
    python3 tools/mg_ble_client.py ptt [--count N]        record each button push-to-talk to a WAV,
                                                         then drive the face: responding, done
    python3 tools/mg_ble_client.py state thinking         send one assistant_state (idle, thinking,
                                                         responding, done, error)
    python3 tools/mg_ble_client.py call [--answer live|failed|none] [--start] [--state S ...]
                                                         the phone's side of a live call: prints the
                                                         gadget's start/end and answers with states
    python3 tools/mg_ble_client.py setup [--tokens FILE --address ADDR] [--confirm-serial PORT]
                                                         set a gadget up over Muse Link setup:
                                                         pairing (one press or one tap on
                                                         Confirm; a set-up gadget's never by
                                                         the talk key outside its setup mode;
                                                         or have its console do it), token-only
                                                         provision_v2; keeps the proof key
    python3 tools/mg_ble_client.py reprovision [--tokens FILE --address ADDR] [--confirm-serial PORT]
                                                         set a set-up gadget up again (Gadget setup
                                                         v2): the old key's proof must fail, the
                                                         new one match
    python3 tools/mg_ble_client.py proof [--clear]        the token proof with the kept key; --clear
                                                         then resets the gadget to setup
    python3 tools/mg_ble_client.py queue [--download] [--clear]
                                                         list (and fetch) offline clips; --clear
                                                         erases them all, fetched or not
    python3 tools/mg_ble_client.py log [--secs 30]        print the Nordic UART log mirror
    python3 tools/mg_ble_client.py play [--tone 1000 --secs 3 | --sweep | --wav FILE] [--loopback]
                                                         stream audio to the gadget's speaker
    python3 tools/mg_ble_client.py bench [--send SECS] [--recv SECS]
                                                         raw link throughput (default 10 s each way)
    python3 tools/mg_ble_client.py images                MCUboot image slots (Zephyr gadgets, SMP)
    python3 tools/mg_ble_client.py dfu --image zephyr.signed.bin [--test | --confirm] [--reset]
                                [--window N] [--chunk B] upload a firmware image over SMP

Pick a gadget with --address (from scan) or --name (a name prefix, default
MuseGadget); otherwise the first one found is used. `test` connects, reads
Device Information and Battery, subscribes, sends request_status, reads and
sets settings, captures --secs of audio with start_mic/stop_mic, checks the
codec frames decode, saves the WAV, and asks for the offline queue's status.
With a proof key from `setup` for this gadget it runs the token proof too.
It prints PASS/FAIL per step and exits 1 if any failed; on a gadget with a
speaker it also plays a 2 s tone (as `play` does).

`setup` does what the Muse app does for a gadget (protocols/README.md, Gadget
setup over Muse Link), against a gadget advertising the Link setup service
(not yet set up), or a set-up one found by its musegadgets advertisement
(set up again, Gadget setup v2): device_info (it must say wifi optional or none), community
pairing v5 (P-256 ECDH, HKDF-SHA256, AES-GCM records, as
esp32/main/link_pairing.c), one press of the gadget's button or one tap on its
on-screen Confirm (a set-up gadget's re-setup is never confirmed by the talk
key: a second key or Confirm does it, or, on a board whose only key is its
talk key, the talk key in setup mode, the key pressed at power-on and let go
early; with --confirm-serial PORT, ">pair.confirm" on its serial console
instead, so nobody has to be there: tools/mg_confirm.py), then token-only
provision_v2 with generated bench-... test tokens, or real ones read from
--tokens FILE (two lines, access then refresh; mode 0600; `-` reads stdin),
never from the command line, and then only to the gadget --address names
(never to whichever advertises strongest; `scan` lists the addresses). It
prints the node_id and keeps K, the token proof key, in
~/.mg_ble_client/proof-<address>.json (mode 0600; MG_BLE_CLIENT_HOME moves
it), never the tokens. `proof` runs the token proof against that key on the
musegadgets service: challenge, the device's MAC checked, confirm, result;
--clear then has the gadget erase its tokens and key and go back to setup.
`reprovision` tests setting a set-up gadget up again on one connection: the
proof with the kept key (a match), setup again with new bench tokens, then
the kept key's proof must fail (the gadget dropped it) and the new key's
match; it keeps the new key. `scan` shows a Link setup payload's flags byte
(bit 1: a Muse Gadget over Bluetooth; bit 0: set up).
Bench tokens aren't minted by Muse: they only exercise the gadget.

`play` streams SBC to the speaker (mg_command_stream_audio): capabilities,
start_streaming, Data writes under the buffer_update flow control, stop
keep, and the device's own stop_streaming; then the device's per-stream
stats from the Nordic UART mirror (decoded level, and the level that reached
the speaker's codec, which is silence while the speaker is off). --volume N
sets the gadget's speaker_volume for the run only: RAM only through its
`!IN vol=N` word (never saved: bench volumes are RAM-only), put back at the end
(set_settings and the original written back on an older gadget); a set-up
gadget takes either only on a proven connection, so the run first proves with
the key `setup` saved and stops if the proof fails. --loopback also records the gadget's
mic while it plays and checks the tone comes back (frequency and level
against a silent baseline), saving the capture WAV; it fails at once, saying
why, if the speaker is off.

`bench` runs device_action throughput_test: --send has the gadget stream
numbered MTU-3 byte notifications for SECS seconds, --recv writes them to the
gadget as fast as the stack takes Write Without Response (CoreBluetooth's
back-pressure honoured), and each prints the rate on both sides, missing or
corrupt packets, and for --send how many packets arrive together (about one
connection event's worth). Without either it runs both, 10 s each.

`images` and `dfu` speak MCUmgr SMP to gadgets with MCUboot (the Zephyr
gadget): `images` lists both slots (version, hash, active, confirmed,
pending); `dfu` uploads a signed image to the second slot, then --test marks
it to run once at the next reset (the gadget keeps it when its boot comes up
healthy, or MCUboot swaps back at the reset after), --confirm marks it
permanent, and --reset restarts the gadget to install it (with --test or
--confirm it then waits for the gadget to come back, confirms the new image
as the Muse apps do, which the gadget holds until its boot is healthy, and
checks it runs confirmed). A token-proven connection's update writes go
through at once; on any other connection the gadget answers the first one
with rc 10 and asks for a press of its own button (30 s), and `dfu` sends it
again about every second until the press admits it (rc 11: no press in time,
or cancelled). An image can hold the SDK token, so `dfu` first runs the proof
with the key `setup` saved and stops unless it matches; --unproven (with
--address) skips it, waits for that press instead, and refuses an image that
holds a token. The proof encrypts
nothing: the image goes over BLE in the clear (the link isn't encrypted, and
SMP's writes are plain), so anyone in radio range can record it, and an SDK
token compiled into it with it. For over-the-air bench updates build images
without the token: `dfu` refuses an image that holds one unless
--allow-token-image says to send it anyway (never with --unproven). The
check is best effort: it finds a token stored as is (mgst_...) and a
Zephyr token slot that isn't blank; one stored any other way (compressed,
encrypted) passes it, so don't count on it. It sends requests as large as the gadget's SMP buffer (from
its MCUmgr parameters; --chunk caps them) and keeps --window of them in
flight (default 2 when the gadget has four buffers or more), and prints the
throughput and how long the upload, the reset and swap, and the confirmation
took. Uploading drops the gadget's offline clips (they share the slot):
download them first. `queue --download` prints each clip's download speed.
They need smpclient, imported only for these two.
`queue` proves the connection first: a set-up gadget lists and sends its
clips only on a proven one.

At every connect, after request_status, the client names its app as the
Muse apps do (setting client_app 19), where the
gadget lists it in its set_settings typed list: "mg_ble_client" with the
tool's commit as its version, in one set_settings write. It sends no device
name: client_name (18) is reserved (mgcommands.h); the gadget's Status
page names this computer by the GAP name it reads itself ("phone" when it
can't), and a tap shows "mg_ble_client: <commit>". --no-identity leaves it
unset.

A gadget's advertised name and Nordic UART lines are printed with every
control character but newline and tab shown as \\xNN, so a peripheral can't
drive the terminal.

Needs a C compiler and the Python packages at the versions it's tested with
(`python3 -m pip install -r esp32/tools/requirements-bench.txt`: bleak, and
cryptography for `setup`, smpclient for `images` and `dfu`, each imported only
where it's used; `dfu` reaches into smpclient's BLE transport, so keep that
pin). Audio is decoded with the
firmware's own codecs (xplat/libsbc, xplat/liblc3), built on first
use into ~/.cache/mg_ble_client. The UUIDs and command numbers are read from
protocols/mgcommands.h, never copied. On macOS, the app running this (e.g.
Terminal) needs Bluetooth permission in System Settings > Privacy & Security.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import ctypes
import hashlib
import hmac
import json
import math
import os
import re
import secrets
import shlex
import struct
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import redact  # noqa: E402  (tools/redact.py: what may be printed of a secret)

ESP32 = Path(__file__).resolve().parents[1]
PROTOCOLS = ESP32.parent / "protocols"


# ---- the protocol, from its header ------------------------------------------

def load_protocol(headers: tuple[Path, ...] = (PROTOCOLS / "mgcommands.h",)) -> dict:
    """UUID strings and enum values from mgcommands.h, by name."""
    text = "\n".join(h.read_text() for h in headers)
    names: dict[str, object] = {}
    for m in re.finditer(r"#define\s+(MG_\w+?)\s+\"([0-9A-Fa-f-]{36})\"", text):
        names[m.group(1)] = m.group(2).lower()
    for m in re.finditer(r"#define\s+(MG_\w+)\s+(0x[0-9A-Fa-f]+|\d+)[uU]?\b", text):
        names[m.group(1)] = int(m.group(2), 0)
    for m in re.finditer(r"#define\s+(MG_DEVICE_\w*NAME_PREFIX)\s+\"([^\"]+)\"", text):
        names[m.group(1)] = m.group(2)
    for m in re.finditer(r"^\s*(mg_\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,", text, re.M):
        names[m.group(1)] = int(m.group(2), 0)
    for m in re.finditer(r"^\s*(mg_\w+)\s*=\s*(\d+)\s*<<\s*(\d+)\s*,", text, re.M):   # flags: 1 << n
        names[m.group(1)] = int(m.group(2)) << int(m.group(3))
    return names


P = load_protocol()

# The names gadgets advertise: MuseGadget-XXXXXX on the SDK's boards,
# MuseGadget-<PRODUCT>-XXXXXX on catalog products (protocols/README.md, Gadget
# setup v2). MG- was proposed for products and deferred: nothing advertises it,
# but a gadget named so still counts.
NAME_PREFIXES = (P["MG_DEVICE_NAME_PREFIX"], "MG-")


def gadget_name(name: str) -> bool:
    return bool(name) and name.startswith(NAME_PREFIXES)


# Link setup's manufacturer data: company 0xFFFF, then a flags byte.
LINK_COMPANY_ID = 0xFFFF
LINK_ADV_SET_UP = 0x01
LINK_ADV_BLE_GADGET = 0x02


def link_flags_text(flags: int | None) -> str:
    """What a Link setup advertisement's flags byte says (None: no Link setup payload seen)."""
    if flags is None:
        return ""
    kind = "Muse Gadget over Bluetooth" if flags & LINK_ADV_BLE_GADGET else "Wi-Fi device"
    return f"Link setup 0x{flags:02x} ({kind}, {'set up' if flags & LINK_ADV_SET_UP else 'not set up'})"


def uuid16(value: int) -> str:
    return f"0000{value:04x}-0000-1000-8000-00805f9b34fb"


def command_name(value: int) -> str:
    for k, v in P.items():
        if k.startswith("mg_command_") and v == value:
            return k[len("mg_command_"):]
    return f"0x{value:02x}"


def data_type_name(value: int) -> str:
    for k, v in P.items():
        if k.startswith("mg_data_type_") and v == value:
            return k[len("mg_data_type_"):]
    return f"type {value}"


def parse_change_data_type(payload: bytes) -> dict:
    """[change_data_type, type, rate, channels, frame bytes, frame us], the format optional."""
    fmt = {"type": payload[1], "rate": P["MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ"],
           "channels": P["MG_AUDIO_DEFAULT_CHANNELS"], "frame_bytes": 0, "frame_us": 0}
    if len(payload) >= 4:
        fmt["rate"] = int.from_bytes(payload[2:4], "little")
    if len(payload) >= 5:
        fmt["channels"] = payload[4]
    if len(payload) >= 7:
        fmt["frame_bytes"] = int.from_bytes(payload[5:7], "little")
    if len(payload) >= 9:
        fmt["frame_us"] = int.from_bytes(payload[7:9], "little")
    if fmt["type"] == P["mg_data_type_audio_lc3"]:
        fmt["frame_bytes"] = fmt["frame_bytes"] or P["MG_LC3_DEFAULT_FRAME_BYTES"]
        fmt["frame_us"] = fmt["frame_us"] or P["MG_LC3_DEFAULT_FRAME_US"]
    return fmt


def parse_clip_info(payload: bytes) -> dict:
    """[audio_queue, clip_info, index, codec, start ms, bytes, then the clip's format as change_data_type's
    params 2-5, then optionally start_unix_ms]. Without the format (older firmware): 16000, mono and the
    codec's defaults (mgcommands.h, clip_info)."""
    out = {"index": int.from_bytes(payload[2:4], "little"), "codec": payload[4],
           "start_ms": int.from_bytes(payload[5:9], "little"), "size": int.from_bytes(payload[9:13], "little"),
           "format": parse_change_data_type(bytes([P["mg_command_change_data_type"], payload[4]]) + payload[13:20])}
    if len(payload) >= 28:
        out["start_unix_ms"] = int.from_bytes(payload[20:28], "little")
    return out


# ---- who is connected: setting client_app 19 (client_name 18 is reserved) ----

CLIENT_APP_NAME = "mg_ble_client"
IDENTIFY = True   # --no-identity: the client doesn't send it

# ---- bench switches: each board's mic-gate word, sent on the console right after connect ----
# Mic output goes only to a central that passed this connection's token proof; on a bench build
# (sdkconfig.muse-bench, overlay-bench.conf) a Mac bench client without a proof key turns its board's bench
# switch on first (--mic-bench), or sends any word (--bench-word, repeatable). Every one is a console word, off
# at each boot. Release builds refuse state-changing Nordic UART words on a connection that isn't proven, and
# these go out before any proof.
MIC_BENCH_WORDS = {
    "esp32": "bench.out=1",
    "xn54": "bench 1",
}
# The ESP32 takes bench.out=1 over the Nordic UART too: for that connection only (it
# ends at the disconnect, off at every boot), logged on and off, "Bench" on the gadget's screen while it's on.
# The Zephyr app (xn54, the XIAO nRF54L15) takes "bench 1" on the Nordic UART, per connection too.
BENCH_WORDS: list[str] = []   # sent by Gadget.start(), in order, before anything else


def bench_words(mic_bench: str | None, extra: list[str] | None) -> list[str]:
    """The words --mic-bench and --bench-word send after connect: the board's switch first, then each
    --bench-word in order."""
    words = [MIC_BENCH_WORDS[mic_bench]] if mic_bench else []
    return words + [w for w in (extra or []) if w]


def utf8_cut(text: str, limit: int) -> bytes:
    """text as UTF-8, control characters dropped, cut to limit bytes at a whole character."""
    clean = "".join(ch for ch in text if ch.isprintable())
    out = clean.encode("utf-8")
    if len(out) <= limit:
        return out
    return out[:limit].decode("utf-8", "ignore").encode("utf-8")


def client_version() -> str:
    """The tool's version: its checkout's commit (short), "dev" outside git."""
    try:
        r = subprocess.run(["git", "-C", str(Path(__file__).resolve().parent), "rev-parse", "--short=8", "HEAD"],
                           capture_output=True, text=True, timeout=2)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "dev"


def identity_payload(app: str = CLIENT_APP_NAME, version: str | None = None) -> bytes:
    """[set_settings, client_app, len, app, len, version], each within the header's limits. No client_name:
    18 is reserved (mgcommands.h), the gadget reads this computer's GAP name itself."""
    a = utf8_cut(app, P["MG_CLIENT_APP_FIELD_MAX"])
    v = utf8_cut(client_version() if version is None else version, P["MG_CLIENT_APP_FIELD_MAX"])
    return bytes([P["mg_command_set_settings"], P["mg_setting_parameter_client_app"], len(a)]) + a + bytes([len(v)]) + v


def parse_client_settings(payload: bytes) -> dict:
    """client_app (and an older firmware's client_name) in a get_settings answer:
    {"client_app": (name, version), "client_name": str}."""
    out, i = {}, 1
    while i < len(payload):
        sid = payload[i]
        if sid == P["mg_setting_parameter_client_name"] and i + 1 < len(payload):
            n = payload[i + 1]
            out["client_name"] = payload[i + 2:i + 2 + n].decode("utf-8", "replace")
            i += 2 + n
        elif sid == P["mg_setting_parameter_client_app"] and i + 1 < len(payload):
            an = payload[i + 1]
            app = payload[i + 2:i + 2 + an].decode("utf-8", "replace")
            j = i + 2 + an
            vn = payload[j] if j < len(payload) else 0
            out["client_app"] = (app, payload[j + 1:j + 1 + vn].decode("utf-8", "replace"))
            i = j + 1 + vn
        else:
            break
    return out


# Byte width of each setting's value (mg_setting_parameter_t).
SETTING_WIDTH = {
    "spec_version": 1, "push_to_talk_enabled": 1, "haptics_enabled": 1, "ptt_buzz_freq_hz": 2,
    "ptt_buzz_duration_ms": 2, "ptt_buzz_volume_percent": 1, "audio_codec": 1, "audio_queue_enabled": 1,
    "speaker_volume": 1,
}


FACE_RE = re.compile(r"mg\.face: (?:face |ring )?(.+?)\s*$")

ASSISTANT_STATES = ("idle", "thinking", "responding", "done", "error")


def assistant_state(name: str) -> int:
    return P["mg_assistant_state_" + name]


# The real client is the Muse app; `call` plays the phone's side over BLE only, to
# test a gadget.
LIVE_CALL_STATES = ("idle", "connecting", "live", "ended", "failed")


def live_call_state(name: str) -> int:
    return P["mg_live_call_state_" + name]


def live_call_name(value: int, outgoing: bool) -> str:
    """[live_call, x]: x is a request (start, end) from the gadget, a state from the client."""
    prefix = "mg_live_call_state_" if outgoing else "mg_live_call_request_"
    for k, v in P.items():
        if k.startswith(prefix) and v == value:
            return k[len(prefix):]
    return str(value)


def live_call_answer(request: int, answer: str = "live", connect_s: float = 1.0) -> list[tuple[float, int]]:
    """What the phone sends back for the gadget's [live_call, request]: (delay s, state) pairs."""
    if request == P["mg_live_call_request_start"]:
        if answer == "none":
            return []
        return [(0.0, live_call_state("connecting")), (connect_s, live_call_state(answer))]
    if request == P["mg_live_call_request_end"]:
        return [(0.0, live_call_state("ended"))]
    return []


def setting_name(sid: int) -> str:
    for n in SETTING_WIDTH:
        if setting_id(n) == sid:
            return n
    return f"setting {sid}"


def setting_id(name: str) -> int:
    return P["mg_setting_parameter_" + name]


def parse_settings(payload: bytes) -> dict:
    """[get_settings, (param, value)...] -> {name: value}."""
    by_id = {setting_id(n): n for n in SETTING_WIDTH}
    out, i = {}, 1
    while i < len(payload):
        name = by_id.get(payload[i])
        if not name:
            break
        w = SETTING_WIDTH[name]
        out[name] = int.from_bytes(payload[i + 1:i + 1 + w], "little")
        i += 1 + w
    return out


def parse_error(payload: bytes) -> str:
    cmd = command_name(payload[1]) if len(payload) > 1 else "?"
    code = int.from_bytes(payload[3:5], "little") if len(payload) >= 5 else 0
    names = {v: k[len("mg_error_code_"):] for k, v in P.items() if k.startswith("mg_error_code_")}
    return f"error on {cmd}: {names.get(code, code)}"


# ---- decoding with the firmware's codecs ------------------------------------

SHIM = r"""
#include <string.h>
#include "sbc.h"
#include "lc3.h"
static sbc_t s_sbc;
static lc3_decoder_mem_48k_t s_mem;
static lc3_decoder_t s_lc3;
static int s_lc3_rate = 16000;
/* rate: the LC3 stream's (16000 or 24000); 0 keeps the last. */
void mg_reset(int rate) {
    sbc_reset(&s_sbc);
    if (rate) s_lc3_rate = rate;
    s_lc3 = lc3_setup_decoder(10000, s_lc3_rate, 0, &s_mem);
}
/* Whole SBC frames in buf: returns samples, *used the bytes taken (-1 samples: not SBC). */
int mg_sbc(const unsigned char *buf, int len, short *out, int cap, int *used) {
    int o = 0, u = 0;
    while (len - u >= SBC_PROBE_SIZE) {
        struct sbc_frame f;
        if (sbc_probe(buf + u, &f)) { if (!o) { *used = u; return -1; } break; }
        int fs = (int)sbc_get_frame_size(&f), n = f.nblocks * f.nsubbands;
        if (fs <= 0 || len - u < fs || o + n > cap) break;
        if (sbc_decode(&s_sbc, buf + u, fs, &f, out + o, 1, NULL, 0)) break;
        o += n; u += fs;
    }
    *used = u; return o;
}
/* SBC encoder: mono, 16 blocks, 8 subbands, loudness. Returns the frame size. */
static sbc_t s_enc;
static struct sbc_frame s_ef;
int mg_sbc_enc_setup(int rate, int bitpool) {
    memset(&s_ef, 0, sizeof(s_ef));
    s_ef.freq = rate == 16000 ? SBC_FREQ_16K : rate == 32000 ? SBC_FREQ_32K : rate == 44100 ? SBC_FREQ_44K1
              : rate == 48000 ? SBC_FREQ_48K : -1;
    if ((int)s_ef.freq < 0) return -1;
    s_ef.mode = SBC_MODE_MONO; s_ef.bam = SBC_BAM_LOUDNESS; s_ef.nblocks = 16; s_ef.nsubbands = 8;
    s_ef.bitpool = bitpool;
    sbc_reset(&s_enc);
    return (int)sbc_get_frame_size(&s_ef);
}
/* 128 samples -> one frame. */
int mg_sbc_enc(const short *pcm, unsigned char *out, int cap) {
    if (sbc_encode(&s_enc, pcm, 1, NULL, 0, &s_ef, out, cap)) return -1;
    return (int)sbc_get_frame_size(&s_ef);
}
/* One 10 ms LC3 frame of `len` bytes at the rate mg_reset() set (NULL: conceal a lost one). */
int mg_lc3(const unsigned char *buf, int len, short *out) {
    if (!s_lc3) return -1;
    return lc3_decode(s_lc3, buf, len, LC3_PCM_FORMAT_S16, out, 1) < 0 ? -1 : s_lc3_rate / 100;
}
/* LC3 encoder (playback to a gadget): 10 ms frames at rate (16000 or 24000), LTPF off (LC3_ENC_LTPF=0).
 * Returns the samples a frame takes, -1 if the rate isn't built in. */
static lc3_encoder_mem_48k_t s_emem;
static lc3_encoder_t s_lc3e;
int mg_lc3_enc_setup(int rate) {
    s_lc3e = lc3_setup_encoder(10000, rate, 0, &s_emem);
    return s_lc3e ? rate / 100 : -1;
}
/* One frame of samples -> nbytes of LC3. */
int mg_lc3_enc(const short *pcm, unsigned char *out, int nbytes) {
    if (!s_lc3e) return -1;
    return lc3_encode(s_lc3e, LC3_PCM_FORMAT_S16, pcm, 1, nbytes, out) == 0 ? nbytes : -1;
}
"""

LC3_SRCS = ("attdet.c", "bits.c", "bwdet.c", "energy.c", "lc3.c", "ltpf.c", "mdct.c", "plc.c", "sns.c",
            "spec.c", "tables.c", "tns.c")
LC3_DEFS = ("LC3_ENC_LTPF=0", "LC3_DEC_LTPF=0", "LC3_PLUS=0", "LC3_PLUS_HR=0", "LC3_DT_MASK=8",
            "LC3_SR_MASK=6")


def decoder_sources() -> list[Path]:
    sbc = ESP32.parent / "xplat/libsbc/src"
    lc3 = ESP32.parent / "xplat/liblc3/src"
    return [sbc / "sbc.c", sbc / "bits.c", *(lc3 / f for f in LC3_SRCS)]


class Decoder:
    """SBC, and 10 ms LC3 at 16 or 24 kHz, through ctypes."""

    def __init__(self, cache: Path | None = None):
        cache = cache or Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "mg_ble_client"
        cache.mkdir(mode=0o700, parents=True, exist_ok=True)
        srcs = decoder_sources()
        digest = hashlib.sha256(SHIM.encode())
        for s in srcs:
            digest.update(s.read_bytes())
        lib = cache / f"mgdec-{digest.hexdigest()[:12]}{'.dylib' if sys.platform == 'darwin' else '.so'}"
        if not lib.exists():
            # Built in a private directory beside it, then renamed into place: a run that stops part-way, or two
            # runs at once, never leave a partial library under the name that's loaded.
            with tempfile.TemporaryDirectory(dir=cache, prefix=".build-") as tmp:
                shim, out = Path(tmp) / "shim.c", Path(tmp) / lib.name
                shim.write_text(SHIM)
                sbc_dir, lc3_dir = ESP32.parent / "xplat/libsbc", ESP32.parent / "xplat/liblc3"
                cc = shlex.split(os.environ.get("CC", "cc"))
                cmd = [*cc, "-O2", "-shared", "-fPIC", "-w", "-o", str(out),
                       "-I", str(sbc_dir / "include"), "-I", str(sbc_dir / "src"),
                       "-I", str(lc3_dir / "include"), "-I", str(lc3_dir / "src"),
                       *[f"-D{d}" for d in LC3_DEFS], str(shim), *map(str, srcs), "-lm"]
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode:
                    raise RuntimeError("building the decoder failed:\n" + r.stderr[-4000:])
                os.replace(out, lib)
        self.lib = ctypes.CDLL(str(lib))
        self.lib.mg_sbc.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_short), ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_int)]
        self.lib.mg_lc3.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_short)]
        self.lib.mg_reset.argtypes = [ctypes.c_int]
        self.lib.mg_sbc_enc.argtypes = [ctypes.POINTER(ctypes.c_short), ctypes.c_char_p, ctypes.c_int]
        self.lib.mg_lc3_enc_setup.argtypes = [ctypes.c_int]
        self.lib.mg_lc3_enc.argtypes = [ctypes.POINTER(ctypes.c_short), ctypes.c_char_p, ctypes.c_int]
        self.reset()

    def lc3_encode(self, pcm: list[int], rate: int = 24000, frame_bytes: int = 40) -> list[bytes]:
        """Mono PCM at rate -> 10 ms LC3 frames of frame_bytes each (LTPF off), padded to whole frames."""
        n = self.lib.mg_lc3_enc_setup(rate)
        if n <= 0:
            raise ValueError(f"LC3 can't encode {rate} Hz")
        frames, buf = [], ctypes.create_string_buffer(frame_bytes)
        pcm = list(pcm) + [0] * (-len(pcm) % n)
        for i in range(0, len(pcm), n):
            block = (ctypes.c_short * n)(*pcm[i:i + n])
            if self.lib.mg_lc3_enc(block, buf, frame_bytes) != frame_bytes:
                raise ValueError("LC3 encode failed")
            frames.append(buf.raw)
        return frames

    def sbc_encode(self, pcm: list[int], rate: int = 16000, bitpool: int = 26) -> tuple[list[bytes], int]:
        """Mono PCM -> SBC frames (16 blocks, 8 subbands, loudness), padded to whole frames."""
        fb = self.lib.mg_sbc_enc_setup(rate, bitpool)
        if fb <= 0:
            raise ValueError(f"SBC can't encode {rate} Hz")
        frames, buf = [], ctypes.create_string_buffer(fb)
        pcm = list(pcm) + [0] * (-len(pcm) % 128)
        for i in range(0, len(pcm), 128):
            block = (ctypes.c_short * 128)(*pcm[i:i + 128])
            if self.lib.mg_sbc_enc(block, buf, fb) != fb:
                raise ValueError("SBC encode failed")
            frames.append(buf.raw)
        return frames, fb

    def reset(self, lc3_rate: int = 0) -> None:
        """Fresh decoders; lc3_rate (16000 or 24000) picks the LC3 rate, 0 keeps the last."""
        self.lib.mg_reset(lc3_rate)

    def sbc(self, data: bytes) -> tuple[list[int], int]:
        """Decodes whole SBC frames from data; returns (samples, bytes used)."""
        out = (ctypes.c_short * (len(data) * 4 + 256))()
        used = ctypes.c_int(0)
        n = self.lib.mg_sbc(data, len(data), out, len(out), ctypes.byref(used))
        if n < 0:
            raise ValueError("not SBC")
        return list(out[:n]), used.value

    def lc3(self, frame: bytes | None, frame_bytes: int) -> list[int]:
        out = (ctypes.c_short * 480)()
        n = self.lib.mg_lc3(frame, frame_bytes, out)
        if n < 0:
            raise ValueError("LC3 decode failed")
        return list(out[:n])

    def decode(self, fmt: dict, chunks: list[bytes]) -> tuple[list[int], int]:
        """PCM of the Data notifications of one utterance, and how many frames they held (LC3 at the
        format's rate: a 24 kHz stream through a 16 kHz decoder decodes to silence). A format it doesn't build
        (more than one channel, LC3 at another frame duration or rate) is refused, never misdecoded."""
        if fmt.get("channels", 1) != 1:
            raise ValueError(f"{fmt['channels']} channels: this decoder is mono")
        if fmt["type"] == P["mg_data_type_audio_lc3"] and (fmt["frame_us"] != P["MG_LC3_DEFAULT_FRAME_US"]
                                                          or fmt["rate"] not in (16000, 24000)):
            raise ValueError(f"LC3 at {fmt['rate']} Hz, {fmt['frame_us']} us frames: this decoder takes 10 ms "
                             "frames at 16 or 24 kHz")
        self.reset(fmt["rate"] if fmt["type"] == P["mg_data_type_audio_lc3"] else 0)
        pcm, frames = [], 0
        if fmt["type"] == P["mg_data_type_audio_sbc"]:
            for c in chunks:   # whole frames per notification
                samples, used = self.sbc(c)
                if used != len(c):
                    raise ValueError(f"a notification held {len(c) - used} bytes that aren't whole SBC frames")
                pcm += samples
                frames += len(samples) // 128 if samples else 0
        elif fmt["type"] == P["mg_data_type_audio_lc3"]:
            fb = fmt["frame_bytes"]
            for c in chunks:
                if len(c) % fb:
                    raise ValueError(f"a notification of {len(c)} bytes isn't whole {fb}-byte LC3 frames")
                for i in range(0, len(c), fb):
                    pcm += self.lc3(c[i:i + fb], fb)
                    frames += 1
        else:
            raise ValueError(f"can't decode {data_type_name(fmt['type'])}")
        return pcm, frames


def write_wav(path: Path, pcm: list[int], rate: int = 16000) -> None:
    import array
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(array.array("h", pcm).tobytes())


# ---- BLE ----------------------------------------------------------------------

async def wait_write_room(client, timeout: float = 1.0) -> None:
    """Waits until the host stack can take another Write Without Response.

    CoreBluetooth drops a write-without-response it has no room for, and
    bleak doesn't check, so without this a burst loses whole writes. Other
    stacks queue or block, so there this is only a short pause.
    """
    peripheral = getattr(getattr(client, "_backend", None), "_peripheral", None)
    if peripheral is None or not hasattr(peripheral, "canSendWriteWithoutResponse"):
        await asyncio.sleep(0.002)
        return
    deadline = time.monotonic() + timeout
    while not peripheral.canSendWriteWithoutResponse() and time.monotonic() < deadline:
        await asyncio.sleep(0.002)


class Gadget:
    """One connection, collecting Control and Data notifications."""

    def __init__(self, client, verbose: bool = True):
        self.client = client
        self.verbose = verbose
        self.control: asyncio.Queue = asyncio.Queue()
        self.data: list[bytes] = []
        self.nus: list[str] = []
        self.battery: list[int] = []
        # Playback: the latest buffer_update, stop_streaming notifications and stream_audio errors.
        self.buf_avail = 0
        self.buf_received = 0
        self.buf_queued_ms: int | None = None
        self.buf_event = asyncio.Event()
        self.stops: list[tuple[float, int]] = []
        self.stream_errors: list[str] = []
        # Face changes the device logs on the Nordic UART mirror (face_log: print them as they come).
        self.face_log = False
        self.faces: list[str] = []
        self._nus_line = ""
        # Takes Data notifications instead of the list when set (the throughput test).
        self.data_hook = None
        # Prints the Nordic UART mirror's lines that match this pattern as they come.
        self.nus_watch: re.Pattern | None = None
        # Video and UI streams: the latest stream_video buffer_update (parse_video_update),
        # the time it came, and errors on stream_video.
        self.vbuf: dict = {}
        self.vbuf_t = 0.0
        self.vbuf_event = asyncio.Event()
        self.video_errors: list[str] = []
        # UI input: touch events (parse_touch, with the time they came) and hid_status.
        self.touches: list[dict] = []
        self.touch_print = False
        self.hid_status: list[dict] = []
        # Named itself on this connection (settings 18 and 19).
        self.identified = False
        self.proof_nonces: tuple[bytes, bytes] | None = None   # the last matching token proof's (run_proof())

    async def start(self, nus: bool = True) -> None:
        await self.client.start_notify(P["MG_CONTROL_UUID"], self._on_control)
        await self.client.start_notify(P["MG_DATA_UUID"], self._on_data)
        if nus:
            await self.client.start_notify(P["MG_NUS_TX_UUID"], self._on_nus)
        try:
            await self.client.start_notify(uuid16(P["MG_BAS_BATTERY_LEVEL_UUID16"]), self._on_battery)
        except Exception:
            pass
        # --mic-bench / --bench-word: on the console, before the first command.
        for word in BENCH_WORDS:
            await self.client.write_gatt_char(P["MG_NUS_RX_UUID"], word.encode() + b"\n", response=True)

    async def identify(self, version: str | None = None) -> bytes:
        """Names this client's app: "mg_ble_client <version>" in one set_settings write (no device
        name: client_name is reserved). A gadget that doesn't know it answers unsupported, which nothing
        waits for."""
        payload = identity_payload(version=version)
        await self.send(*payload)
        self.identified = True
        return payload

    async def maybe_identify(self, settings: list[int] | None) -> bool:
        """At connect, after request_status (mgcommands.h): once, and only where the gadget lists
        client_app in its set_settings typed list (clients send only listed settings); --no-identity: never."""
        if not IDENTIFY or self.identified or not settings or P["mg_setting_parameter_client_app"] not in settings:
            return False
        await self.identify()
        return True

    def _on_control(self, _char, payload: bytearray) -> None:
        payload = bytes(payload)
        if not payload:
            return   # nothing to read: skipped
        sv, ui = P["mg_command_stream_video"], P["mg_command_ui_input"]
        if payload[:2] == bytes([sv, P["mg_stream_video_buffer_update"]]):
            self.vbuf = parse_video_update(payload)
            self.vbuf_t = time.monotonic()
            self.vbuf_event.set()
            return   # frequent: not printed or queued
        if payload[:2] == bytes([P["mg_command_error"], sv]):
            self.video_errors.append(parse_error(payload))
        if payload[:2] == bytes([ui, P["mg_ui_input_touch"]]):
            if len(payload) < 10:
                return   # a short touch event: skipped
            t = parse_touch(payload)
            t["t"] = time.monotonic()
            self.touches.append(t)
            if self.touch_print:
                print(f"  touch {t['action']:<4} {t['x']:3d},{t['y']:3d}  t={t['time']} ms  id {t['id']}")
            return
        if payload[:2] == bytes([ui, P["mg_ui_input_hid_status"]]) and len(payload) >= 4:
            self.hid_status.append(parse_hid_status(payload))
        sa = P["mg_command_stream_audio"]
        if payload[:2] == bytes([sa, P["mg_stream_audio_buffer_update"]]) and len(payload) >= 10:
            self.buf_avail = int.from_bytes(payload[2:6], "little")
            self.buf_received = int.from_bytes(payload[6:10], "little")
            # A/V sync: the audio the gadget holds ahead of its speaker (0xffff unknown; absent on older firmware).
            self.buf_queued_ms = int.from_bytes(payload[10:12], "little") if len(payload) >= 12 else None
            self.buf_event.set()
            return   # frequent: not printed or queued
        if payload[:2] == bytes([sa, P["mg_stream_audio_stop_streaming"]]) and len(payload) >= 3:
            self.stops.append((time.monotonic(), payload[2]))
        if payload[:2] == bytes([P["mg_command_error"], sa]):
            self.stream_errors.append(parse_error(payload))
        if self.verbose:
            print(f"  <- {describe(payload)}")
        self.control.put_nowait(payload)

    def _on_data(self, _char, payload: bytearray) -> None:
        if self.data_hook:
            self.data_hook(bytes(payload))
            return
        self.data.append(bytes(payload))

    def _on_nus(self, _char, payload: bytearray) -> None:
        text = printable(bytes(payload).decode("utf-8", "replace"))
        self.nus.append(text)
        if self.face_log or self.nus_watch:
            # The device's face changes ("mg.face: face thinking"), as whole lines.
            self._nus_line += text
            *lines, self._nus_line = self._nus_line.split("\n")
            for line in lines:
                m = FACE_RE.search(line) if self.face_log else None
                if m:
                    self.faces.append(m.group(1))
                    print(f"      face: {m.group(1)}")
                elif self.nus_watch and self.nus_watch.search(line):
                    print(f"      log: {redact.redact_text(line.strip())}")

    def _on_battery(self, _char, payload: bytearray) -> None:
        if payload:
            self.battery.append(payload[0])

    async def send(self, *payload: int) -> None:
        data = bytes(payload)
        if self.verbose:
            print(f"  -> {describe(data, outgoing=True)}")
        await self.client.write_gatt_char(P["MG_CONTROL_UUID"], data, response=False)

    async def expect(self, command: int, timeout: float = 3.0, sub: int | None = None) -> bytes:
        """The next Control notification of `command` (and first parameter `sub`); others are skipped."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"no {command_name(command)} notification")
            payload = await asyncio.wait_for(self.control.get(), left)
            if payload[0] == command and (sub is None or (len(payload) > 1 and payload[1] == sub)):
                return payload
            if payload[0] == P["mg_command_error"] and len(payload) > 1 and payload[1] == command:
                raise RuntimeError(parse_error(payload))

    async def status(self) -> dict:
        """request_status: the data type, the commands and every typed list, by the command it's about."""
        g = self
        g.drain()
        await g.send(P["mg_command_request_status"])
        cdt = await g.expect(P["mg_command_change_data_type"])
        typed, others = {}, []
        feats = await g.command_list(typed)
        recovery = typed.get(P["mg_command_request_status"])
        if recovery is not None:
            print(f"NOTE  the gadget declares a recovery mode (mode, reason, flags {recovery}): Muse isn't served")
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            try:
                p = await asyncio.wait_for(g.control.get(), deadline - time.monotonic())
            except asyncio.TimeoutError:
                break
            if p[0] == P["mg_command_supported_features"] and len(p) > 2 and p[1] == P["mg_command_sub_feature"]:
                typed[p[2]] = list(p[3:])
            else:
                others.append(p)   # pending requests (request_streaming, live_call) come here
        settings = typed.get(P["mg_command_set_settings"])
        await g.maybe_identify(settings)   # who is connected: after request_status, where listed
        return {"data_type": cdt, "commands": list(feats[1:]), "typed": typed, "settings": settings,
                "others": others, "recovery": recovery}

    async def command_list(self, typed: dict | None = None) -> bytes:
        """The supported_features command list after request_status. A typed list that comes before it (a
        recovery mode's declaration, [supported_features, sub_feature, request_status, mode, reason, flags]) is
        kept in typed, never taken for the list."""
        while True:
            feats = await self.expect(P["mg_command_supported_features"])
            if len(feats) > 2 and feats[1] == P["mg_command_sub_feature"]:
                if typed is not None:
                    typed[feats[2]] = list(feats[3:])
                continue
            return feats

    def drain(self) -> list[bytes]:
        out = []
        while not self.control.empty():
            out.append(self.control.get_nowait())
        return out


# ---- video and UI streams (mgcommands.h, Video streaming, UI streaming) ----

# mgcommands.h, stream_video capabilities: from this id on every value starts with its length byte, so a client
# skips one it doesn't take. The ids this SDK doesn't define there (17 among them) are reserved: skipped the
# same way.
VIDEO_CAP_LENGTH_PREFIXED = 17


def parse_video_caps(payload: bytes) -> dict:
    """stream_video report_capabilities, by capability. An unknown id under 17 ends the parse (its width isn't
    known); from 17 on each value starts with its length, so an unknown or reserved one is skipped (mgcommands.h)."""
    out: dict = {}
    if len(payload) < 3:
        return out
    count, i = payload[2], 3
    try:
        for _ in range(count):
            if i >= len(payload):
                break
            cap = payload[i]
            i += 1
            if cap == P["mg_stream_video_capability_codecs"]:
                n = payload[i]
                out["codecs"] = list(_need(payload, i + 1, n))
                i += 1 + n
            elif cap == P["mg_stream_video_capability_display"]:
                w, h, rnd = struct.unpack_from("<HHB", payload, i)
                out["display"] = (w, h, bool(rnd))
                i += 5
            elif cap == P["mg_stream_video_capability_max_size"]:
                out["max_size"] = struct.unpack_from("<HH", payload, i)
                i += 4
            elif cap == P["mg_stream_video_capability_max_fps"]:
                out["max_fps"] = payload[i]
                i += 1
            elif cap == P["mg_stream_video_capability_buffer_size"]:
                out["buffer_size"] = struct.unpack_from("<I", payload, i)[0]
                i += 4
            elif cap == P["mg_stream_video_capability_suggested"]:
                w, h, fps, kbps = struct.unpack_from("<HHBH", payload, i)
                out["suggested"] = {"w": w, "h": h, "fps": fps, "kbps": kbps}
                i += 7
            elif cap in (P["mg_stream_video_capability_l2cap_psm"], P["mg_stream_video_capability_playout_delay"]):
                key = "l2cap_psm" if cap == P["mg_stream_video_capability_l2cap_psm"] else "playout_delay"
                out[key] = struct.unpack_from("<H", payload, i)[0]
                i += 2
            elif cap == P["mg_stream_video_capability_tiles"]:
                out["tiles"] = payload[i]
                i += 1
            elif cap == P["mg_stream_video_capability_tile_split"]:
                out["tile_split"] = payload[i]
                i += 1
            elif cap == P["mg_stream_video_capability_ui"]:
                w, h, fps, frame, tiles = struct.unpack_from("<HHBIB", payload, i)
                out["ui"] = {"w": w, "h": h, "fps": fps, "max_frame": frame, "tiles": tiles}
                i += 10
            elif cap == P["mg_stream_video_capability_ui_still"]:
                out["ui_still"] = payload[i]
                i += 1
            elif cap == P.get("mg_stream_video_capability_flags", -1):
                out["flags"] = payload[i]
                out["early_keyframe"] = bool(payload[i] & P["mg_stream_video_capability_flag_early_keyframe"])
                i += 1
            elif cap == P["mg_stream_video_capability_still"]:
                w, h, max_bytes, _, _, n = struct.unpack_from("<HHIHHB", payload, i)
                out["still"] = {"w": w, "h": h, "max_bytes": max_bytes, "codecs": list(_need(payload, i + 13, n))}
                i += 13 + n
            elif cap == P["mg_stream_video_capability_rotated"]:
                w, h, fps, kbps, turn = struct.unpack_from("<HHBHB", payload, i)
                out["rotated"] = {"w": w, "h": h, "fps": fps, "kbps": kbps, "turn": turn}
                i += 8
            elif cap >= VIDEO_CAP_LENGTH_PREFIXED:
                # From 17 on every value starts with its length: an unknown or reserved one is skipped.
                if i >= len(payload) or i + 1 + payload[i] > len(payload):
                    out["unknown"] = cap
                    break
                out.setdefault("skipped", []).append(cap)
                i += 1 + payload[i]
            else:
                out["unknown"] = cap
                break
    except (IndexError, struct.error):
        out["truncated"] = True   # a value cut short: what came before it stands
    return out


def parse_video_update(payload: bytes) -> dict:
    """stream_video buffer_update: credits, then the optional counters and the shown-frame echo."""
    out = {"avail": int.from_bytes(payload[2:6], "little"), "received": int.from_bytes(payload[6:10], "little")}
    if len(payload) >= 15:
        out["shown"], out["dropped"], out["decode_ms"] = struct.unpack_from("<HHB", payload, 10)
    if len(payload) >= 19:
        out["last_number"], out["last_age_ms"] = struct.unpack_from("<HH", payload, 15)
    return out


def video_start_payload(width: int, height: int, fps: int = 30, kbps: int = 300, tiles: int = 1,
                        ui: bool = False, codec: int | None = None) -> bytes:
    """stream_video start_streaming: H.264 at width x height, with tiles and content=ui when asked."""
    codec = P["mg_data_type_video_h264"] if codec is None else codec
    params = [bytes([P["mg_stream_video_parameter_codec"], codec]),
              bytes([P["mg_stream_video_parameter_width"]]) + struct.pack("<H", width),
              bytes([P["mg_stream_video_parameter_height"]]) + struct.pack("<H", height),
              bytes([P["mg_stream_video_parameter_fps"], fps]),
              bytes([P["mg_stream_video_parameter_bitrate"]]) + struct.pack("<H", kbps)]
    if tiles > 1:
        params.append(bytes([P["mg_stream_video_parameter_tiles"], tiles]))
    if ui:
        params.append(bytes([P["mg_stream_video_parameter_content"], P["mg_video_content_ui"]]))
    return bytes([P["mg_command_stream_video"], P["mg_stream_video_start_streaming"], len(params)]) + b"".join(params)


def video_frame(number: int, key: bool, au: bytes, still: bool = False) -> bytes:
    """One frame on Data: the 8-byte header, then the access unit (or a two-band payload, or a still patch)."""
    flags = P["mg_video_frame_still"] if still else P["mg_video_frame_keyframe"] if key else 0
    return struct.pack("<BBHI", P["MG_VIDEO_FRAME_MAGIC"], flags, number & 0xFFFF, len(au)) + au


def still_patch(raw: bytes, x: int, y: int, w: int, h: int) -> bytes:
    """A lossless still patch (mg_video_frame_still): RGB565 big-endian rows, zlib."""
    import zlib
    return struct.pack("<HHHHB", x, y, w, h, P["mg_video_still_rgb565_zlib"]) + zlib.compress(raw, 6)


def read_records(path: Path) -> list[bytes]:
    """A file of uint32-LE-length records (the codec study's .au and .mgframes)."""
    data, out, i = path.read_bytes(), [], 0
    while i + 4 <= len(data):
        n = int.from_bytes(data[i:i + 4], "little")
        out.append(data[i + 4:i + 4 + n])
        i += 4 + n
    return out


def split_access_units(stream: bytes) -> list[bytes]:
    """An Annex-B H.264 stream as access units: each starts at SPS/PPS or a picture's first slice."""
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", stream)]
    nals = []
    for k, st in enumerate(starts):
        begin = st - 1 if st > 0 and stream[st - 1] == 0 else st
        end = starts[k + 1] if k + 1 < len(starts) else len(stream)
        if k + 1 < len(starts) and stream[end - 1] == 0:
            end -= 1
        nals.append(stream[begin:end])
    aus, cur, have_slice = [], b"", False
    for nal in nals:
        hdr = nal[3] if nal[2] == 1 else nal[4]
        t = hdr & 0x1F
        body = nal[4:] if nal[2] == 1 else nal[5:]
        first_slice = t in (1, 5) and body and body[0] & 0x80   # first_mb_in_slice == 0 (ue(v) "1")
        if have_slice and (t in (7, 8, 9) or first_slice):
            aus.append(cur)
            cur, have_slice = b"", False
        cur += nal
        have_slice = have_slice or t in (1, 5)
    if cur:
        aus.append(cur)
    return aus


def is_keyframe(payload: bytes, bands: int) -> bool:
    """An IDR in the (first band's) access unit."""
    au = payload[4:4 + int.from_bytes(payload[:4], "little")] if bands == 2 else payload
    return any(au[m.end()] & 0x1F == 5 for m in re.finditer(b"\x00\x00\x01", au) if m.end() < len(au))


def load_ui_frames(path: str) -> tuple[list[bytes], int]:
    """Frames and bands from the codec study's files: NAME.mgframes (two bands), NAME.au or NAME.264."""
    p = Path(path)
    if p.suffix == ".mgframes":
        return read_records(p), 2
    if p.suffix == ".au":
        return read_records(p), 1
    return split_access_units(p.read_bytes()), 1


def parse_touch(payload: bytes) -> dict:
    action, x, y, t, pid = struct.unpack_from("<BHHHB", payload, 2)
    return {"action": {0: "down", 1: "move", 2: "up"}.get(action, str(action)), "x": x, "y": y, "time": t, "id": pid}


def parse_hid_status(payload: bytes) -> dict:
    mode, flags = payload[2], payload[3]
    names = {0: "off", 1: "digitizer", 2: "mouse_abs", 3: "mouse_rel"}
    return {"mode": names.get(mode, str(mode)), "bonded": bool(flags & P["mg_hid_status_bonded"]),
            "host": bool(flags & P["mg_hid_status_host"])}


HID_MODES = {"off": 0, "digitizer": 1, "abs": 2, "rel": 3}


def hid_map_payload(mode: str, square: tuple[float, float, float, float] = (0, 0, 1, 1), fisheye: bool = False,
                    rel: tuple[int, int] = (0, 0)) -> bytes:
    """ui_input hid_map: the mode, the square on the screen (fractions of its width and height), the warp flag
    and, for the relative pointer, the counts across the screen."""
    m = HID_MODES[mode]
    out = bytes([P["mg_command_ui_input"], P["mg_ui_input_hid_map"], m])
    if m == 0:
        return out
    x, y, w, h = (max(0, min(65535, round(v * 65535))) for v in square)
    out += struct.pack("<HHHHB", x, y, w, h, P["mg_hid_map_fisheye"] if fisheye else 0)
    if m == HID_MODES["rel"]:
        out += struct.pack("<HH", *rel)
    return out


def describe(payload: bytes, outgoing: bool = False) -> str:
    """One Control message for the log; a malformed one is shown as hex, never raises (it's called from the
    notification callback, which then still queues the message)."""
    try:
        return _describe(payload, outgoing)
    except (IndexError, KeyError, ValueError, struct.error):
        return f"{bytes(payload).hex(' ')} (malformed)"


def _describe(payload: bytes, outgoing: bool = False) -> str:
    """One Control message, readably; outgoing: from the client (some read differently then)."""
    if not payload:
        return "(empty)"
    name = command_name(payload[0])
    if payload[0] == P["mg_command_live_call"] and len(payload) > 1:
        return "live_call " + ("state " if outgoing else "") + live_call_name(payload[1], outgoing)
    if payload[0] == P["mg_command_change_data_type"]:
        f = parse_change_data_type(payload)
        return (f"change_data_type {data_type_name(f['type'])} {f['rate']} Hz x{f['channels']} "
                f"{f['frame_bytes']} B/{f['frame_us']} us")
    if payload[0] == P["mg_command_supported_features"]:
        if len(payload) > 1 and payload[1] == P["mg_command_sub_feature"]:
            return f"supported_features[{command_name(payload[2])}] {list(payload[3:])}"
        return "supported_features " + " ".join(command_name(b) for b in payload[1:])
    if payload[0] == P["mg_command_error"]:
        return parse_error(payload)
    if (payload[0] in (P["mg_command_set_settings"], P["mg_command_get_settings"]) and len(payload) > 1 and
            payload[1] in (P["mg_setting_parameter_client_name"], P["mg_setting_parameter_client_app"])):
        who = parse_client_settings(payload)
        return f"{name} " + " ".join(f"{k}={v!r}" for k, v in who.items())
    if payload[0] == P["mg_command_stream_audio"] and len(payload) > 1:
        sub = {v: k[len("mg_stream_audio_"):] for k, v in P.items() if k.startswith("mg_stream_audio_")
               and not k.startswith(("mg_stream_audio_capability_", "mg_stream_audio_parameter_",
                                     "mg_stream_audio_buffer_keep", "mg_stream_audio_buffer_drop"))}
        name = sub.get(payload[1], str(payload[1]))
        if payload[1] == P["mg_stream_audio_report_capabilities"]:
            return f"stream_audio report_capabilities {parse_capabilities(payload)}"
        if payload[1] == P["mg_stream_audio_stop_streaming"] and len(payload) > 2:
            return f"stream_audio stop_streaming {'keep' if payload[2] == P['mg_stream_audio_buffer_keep'] else 'drop'}"
        return f"stream_audio {name} {payload[2:].hex(' ')}".strip()
    if payload[0] == P["mg_command_gesture"]:
        return "gesture " + ("button_down" if payload[1] == P["mg_gesture_button_down"] else "button_up")
    if payload[:2] == bytes([P["mg_command_stream_video"], P["mg_stream_video_report_capabilities"]]):
        return f"stream_video report_capabilities {parse_video_caps(payload)}"
    if payload[:2] == bytes([P["mg_command_ui_input"], P["mg_ui_input_hid_status"]]) and len(payload) >= 4:
        return f"ui_input hid_status {parse_hid_status(payload)}"
    return f"{name} {payload[1:].hex(' ')}".strip()


_CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f-\x9f\u061c\u200b-\u200f\u2028-\u202e\u2060-\u2069\ufeff]")


def printable(text: str) -> str:
    """Text a gadget sent (its advertised name, its Nordic UART lines, its strings and errors), safe to print:
    every control character but \\n and \\t, and every bidi, zero-width or line-separator character, shown as
    \\xNN or \\uNNNN, so a peripheral can't move the cursor, recolour or clear a terminal, or reorder or hide text
    in it. A carriage return is dropped (CRLF line ends, possibly split across notifications)."""
    def escape(m: re.Match) -> str:
        c = ord(m.group())
        return f"\\x{c:02x}" if c < 0x100 else f"\\u{c:04x}"
    return _CONTROL.sub(escape, text.replace("\r", ""))


def note_advertisement(seen: dict, address: str, name: str, rssi: int, uuids, mfg: dict | None = None) -> None:
    """Adds one advertisement to seen[address] = {"name", "rssi", "uuids", "link_flags"}.

    Gadgets take turns between the Link setup and musegadgets payloads from one
    address, so the UUIDs of every packet in the scan count, not just the
    last one's (BleakScanner.discover keeps only the latest per device).
    link_flags is the last Link setup payload's flags byte (None without one)."""
    entry = seen.setdefault(address, {"name": "", "rssi": rssi, "uuids": set(), "link_flags": None})
    entry["uuids"].update(u.lower() for u in uuids)
    entry["name"] = printable(name) if name else entry["name"]
    entry["rssi"] = max(entry["rssi"], rssi)
    data = (mfg or {}).get(LINK_COMPANY_ID)
    if data is not None and len(data) == 1:
        entry["link_flags"] = data[0]


async def scan_adverts(secs: float) -> dict:
    from bleak import BleakScanner

    seen: dict = {}

    def detected(dev, adv):
        note_advertisement(seen, dev.address, adv.local_name or dev.name or "", adv.rssi, adv.service_uuids,
                           adv.manufacturer_data)

    async with BleakScanner(detection_callback=detected):
        await asyncio.sleep(secs)
    return seen


async def find(args):
    want = P["MG_SERVICE_UUID"]
    print(f"scanning {args.scan_secs:.0f} s for the musegadgets service ...")
    hits = []
    for addr, e in (await scan_adverts(args.scan_secs)).items():
        has = want in e["uuids"]
        # A gadget in SAFE mode advertises as "SAFE-<product>-<tail>" with no
        # UUIDs: found by name, for safe.off and SMP recovery.
        safe = e["name"].startswith("SAFE-")
        if has or ((gadget_name(e["name"]) or safe) and args.address is None):
            hits.append((addr, e["name"], e["rssi"], has, e.get("link_flags")))
    return hits


async def pick(args):
    hits = await find(args)
    if args.address:
        hits = [h for h in hits if h[0].lower() == args.address.lower()] or [(args.address, "?", 0, True)]
    elif args.name:
        hits = [h for h in hits if h[1].startswith(args.name)]
    if not args.address and not args.name:
        # Wi-Fi Muse gadgets share the name prefix; only a name the user asked for may stand in for the service.
        hits = [h for h in hits if h[3]]
    if not hits:
        raise SystemExit("no musegadgets gadget advertising (connected to a phone? try `scan`, or pass --name/--address)")
    addr, name, rssi = sorted(hits, key=lambda h: -h[2])[0][:3]
    print(f"using {name} {addr} (RSSI {rssi})")
    return addr, name


async def cmd_scan(args) -> int:
    hits = await find(args)
    for addr, name, rssi, has, flags in sorted(hits, key=lambda h: -h[2]):
        link = link_flags_text(flags)
        print(f"{addr}  {name or '(no name)'}  RSSI {rssi}  {'mg service' if has else 'name only'}"
              + (f"  {link}" if link else ""))
    return 0 if hits else 1


class Results:
    def __init__(self):
        self.failed = []

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        print(f"{'PASS' if ok else 'FAIL'}  {name}{': ' + printable(detail) if detail else ''}")
        if not ok:
            self.failed.append(name)
        return ok


async def att_mtu(client) -> int | None:
    """The connection's negotiated ATT MTU, or None when it can't be read. BlueZ reports the default 23
    until the MTU is acquired from a characteristic (bleak's _acquire_mtu, as smpclient does); CoreBluetooth
    and WinRT report the negotiated value."""
    backend = getattr(client, "_backend", None)
    if type(backend).__name__ == "BleakClientBlueZDBus" and getattr(backend, "_mtu_size", None) is None:
        try:
            await backend._acquire_mtu()
        except Exception:
            return None
    return client.mtu_size


async def read_str(client, uuid: int) -> str:
    return bytes(await client.read_gatt_char(uuid16(uuid))).decode("utf-8", "replace")


HEAR_PROMPTS = (
    "Testing the microphone, one two three.",
    "The quick brown fox jumps over the lazy dog.",
    "Can you hear me? This is a microphone check.",
    "Seven sunny Sundays, six silver spoons.",
    "Checking the gadget's mic at the bench.",
)


def capture_rates(typed: list[int] | None) -> dict[int, list[tuple[int, int, int]]]:
    """change_data_type's typed list: codec -> [(rate, min kb/s, max kb/s)]."""
    out: dict[int, list[tuple[int, int, int]]] = {}
    b = bytes(typed or [])
    i = 0
    while i + 2 <= len(b):
        codec, n = b[i], b[i + 1]
        i += 2
        if i + 6 * n > len(b):
            break
        out[codec] = [struct.unpack_from("<HHH", b, i + 6 * k) for k in range(n)]
        i += 6 * n
    return out


def hear_prompt(args) -> tuple[list[str] | None, str]:
    """The command that plays a prompt from this computer, and what it plays: --prompt-cmd, or a
    generated tone burst through afplay, paplay or aplay."""
    import shutil
    import tempfile
    if args.prompt_cmd:
        return shlex.split(args.prompt_cmd), args.prompt_cmd
    player = next((p for p in ("afplay", "paplay", "aplay") if shutil.which(p)), None)
    if not player:
        return None, "no player (afplay, paplay or aplay)"
    pcm: list[int] = []
    for f in (500, 1000, 2000, 1000):
        pcm += tone_pcm(f, 0.5, 16000, -6.0) + [0] * 1600
    path = Path(tempfile.mkdtemp(prefix="mg-hear-")) / "burst.wav"
    write_wav(path, pcm, 16000)
    return mac_awake([player, *afplay_volume(), str(path)]) if player == "afplay" else [player, str(path)], \
        "a tone burst (500/1000/2000 Hz)"


def afplay_volume() -> list[str]:
    """afplay's own scale, never the system volume (desk speakers can be loud):
    MG_PLAY_VOLUME, 0.3 by default, raised only as far as a check needs."""
    return ["-v", os.environ.get("MG_PLAY_VOLUME", "0.3")]


def mac_awake(cmd: list[str]) -> list[str]:
    """On macOS, the player under `caffeinate -u`: a Mac in DarkWake (an idle desktop) silences
    every audio output (coreaudiod's DarkWakeSilenceBuffers; afplay plays nothing, or hangs) until
    user activity is declared."""
    import shutil
    return ["caffeinate", "-u", "-d", "-i", *cmd] if sys.platform == "darwin" and shutil.which("caffeinate") else cmd


def tone_prompt(freq: float = 1000.0, secs: float = 1.5) -> list[str] | None:
    """A pre-rendered tone for the frequency check, played like the phrases (afplay under caffeinate)."""
    import shutil
    import tempfile
    player = next((p for p in ("afplay", "paplay", "aplay") if shutil.which(p)), None)
    if not player:
        return None
    path = Path(tempfile.mkdtemp(prefix="mg-hear-")) / f"tone{int(freq)}.wav"
    write_wav(path, tone_pcm(freq, secs, 48000, -6.0), 48000)
    return mac_awake([player, *afplay_volume(), str(path)]) if player == "afplay" else [player, str(path)]


def peak_hz(pcm: list[int], rate: int, lo: float = 400.0, hi: float = 2400.0, step: float = 5.0) -> float:
    """The strongest frequency in the loudest 0.5 s of pcm (Goertzel over lo..hi)."""
    n = rate // 2
    if len(pcm) < n:
        return 0.0
    best_i, best_e = 0, -1.0
    for i in range(0, len(pcm) - n + 1, n // 4):
        e = sum(v * v for v in pcm[i:i + n])
        if e > best_e:
            best_i, best_e = i, e
    seg = pcm[best_i:best_i + n]
    peak_f, peak_p = 0.0, -1.0
    f = lo
    while f <= hi:
        w = 2 * math.pi * f / rate
        c = 2 * math.cos(w)
        s1 = s2 = 0.0
        for v in seg:
            s0 = v + c * s1 - s2
            s2, s1 = s1, s0
        p = s1 * s1 + s2 * s2 - c * s1 * s2
        if p > peak_p:
            peak_f, peak_p = f, p
        f += step
    return peak_f


async def capture_once(g, dec, codec: int, rate: int, rate_param: bool, secs: float):
    """One client capture: start_mic (with the rate when the board lists several), secs, stop_mic.
    Returns (format, decoded PCM)."""
    g.data.clear()
    g.drain()
    payload = [codec] + (list(struct.pack("<H", rate)) if rate_param else [])
    await g.send(P["mg_command_start_mic"], *payload)
    cdt = await g.expect(P["mg_command_change_data_type"], timeout=5)
    fmt = parse_change_data_type(cdt)
    await asyncio.sleep(secs)
    await g.send(P["mg_command_stop_mic"])
    try:
        await g.expect(P["mg_command_stop_mic"], timeout=3)
    except Exception:
        pass
    await asyncio.sleep(0.3)
    chunks = list(g.data)
    pcm, _ = dec.decode(fmt, chunks) if chunks else ([], 0)
    return fmt, pcm


def settled(pcm: list[int], rate: int, skip_s: float = 0.3) -> list[int]:
    """pcm without its first skip_s (an ADC's start-up step) and with its DC removed."""
    x = pcm[int(rate * skip_s):]
    if not x:
        return []
    mean = sum(x) / len(x)
    return [int(round(v - mean)) for v in x]


async def play_and_capture(g, dec, codec, rate, rate_param, cmd, secs):
    """Plays cmd while capturing secs; returns (format, pcm, None) or (None, None, why it didn't play)."""
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.monotonic()
    played = None
    try:
        fmt, pcm = await capture_once(g, dec, codec, rate, rate_param, secs)
    finally:
        try:
            played = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    if played != 0 or time.monotonic() - t0 > secs + 8:
        # The prompt didn't play (the player failed or hung): that says nothing about the mic.
        return None, None, f"the prompt didn't play here ({shlex.join(cmd)}: " \
                           f"{'hung' if played is None else f'exit {played}'})"
    return fmt, pcm, None


async def hear_check(g, r, dec, codec: int, rates: list[int], args) -> None:
    """The mic hears. At every capture rate the board lists for this codec, with the gadget's
    own playback not running (this runs before the playback check): a quiet baseline, a spoken
    phrase played from this computer (reported, and never exact digital silence: a valid stream
    from an unpowered mic), then a pre-rendered 1 kHz tone, which carries the verdict: its level
    above --hear-db and the baseline by 6 dB, its peak at 1000 +-25 Hz at the labeled rate. The
    first 0.3 s and DC are removed first; a capture cut short is its own verdict."""
    cmd, what = hear_prompt(args)
    tcmd = tone_prompt(1000.0, 1.5)
    if not cmd or not tcmd:
        r.check("mic hears", False, f"can't play a prompt here: {what} (--prompt-cmd, or --no-hear to skip)")
        return
    rp = len(rates) > 1
    tries = max(1, int(getattr(args, "hear_tries", 3)))
    wait = getattr(args, "hear_retry_wait", (4.0, 12.0))
    for rate in rates:
        name = f"mic hears at {rate} Hz"
        for attempt in range(1, tries + 1):
            try:
                if await hear_once(g, r, dec, codec, rate, rp, cmd, tcmd, what, args, name,
                                   attempt, tries):
                    break
            except Exception as e:
                r.check(name, False, repr(e))
                break
            # The baseline held another sound as loud as the tone (another prompt on a
            # shared speaker): not a mic verdict; wait a random while and measure again.
            import random
            await asyncio.sleep(random.uniform(*wait))


class speaker_lock:
    """A shared-speaker lock around one baseline + prompt + tone, only when MG_SPEAKER_LOCK names a
    lock file (runs sharing one speaker take flock(2) on it; `lockf -k` interoperates); waits at
    most 120 s, then measures anyway (the retry above still catches a polluted baseline)."""

    def __init__(self):
        p = os.environ.get("MG_SPEAKER_LOCK")
        self.path = Path(p) if p else None
        self.fd = None

    async def __aenter__(self):
        if not self.path:
            return self
        import fcntl
        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
        deadline = time.monotonic() + 120
        while True:
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except BlockingIOError:
                if time.monotonic() > deadline:
                    print(f"  speaker lock {self.path} still held after 120 s: measuring anyway")
                    os.close(self.fd)
                    self.fd = None
                    return self
                await asyncio.sleep(0.5)

    async def __aexit__(self, *exc):
        if self.fd is not None:
            os.close(self.fd)   # closing releases the flock
            self.fd = None
        return False


async def hear_once(g, r, dec, codec, rate, rp, cmd, tcmd, what, args, name, attempt, tries) -> bool:
    """One measurement at one rate, under the shared-speaker lock. Returns False only to ask for
    another try: the tone was loud enough but the quiet baseline wasn't quiet (contention)."""
    async with speaker_lock():
        return await hear_measure(g, r, dec, codec, rate, rp, cmd, tcmd, what, args, name, attempt, tries)


async def hear_measure(g, r, dec, codec, rate, rp, cmd, tcmd, what, args, name, attempt, tries) -> bool:
    fmt, base = await capture_once(g, dec, codec, rate, rp, 1.5)
    if len(base) < 0.8 * 1.5 * fmt["rate"]:
        r.check(name, False, f"capture cut short: {len(base) / fmt['rate']:.2f} s of 1.5 s (not a mic verdict)")
        return True
    floor = rms_db(settled(base, fmt["rate"]))
    fmt, pcm, why = await play_and_capture(g, dec, codec, rate, rp, cmd, args.hear_secs)
    if why:
        r.check(name, False, why)
        return True
    if not pcm or len(pcm) < 0.8 * args.hear_secs * fmt["rate"]:
        r.check(name, False, f"capture cut short: {len(pcm or []) / fmt['rate']:.2f} s of "
                             f"{args.hear_secs:.1f} s (not a mic verdict)")
        return True
    if len(set(pcm)) <= 1:
        r.check(name, False, f"exact digital silence ({len(pcm)} samples all {pcm[0]}): the mic "
                             f"isn't powered or wired, though the frames are valid")
        return True
    speech = rms_db(settled(pcm, fmt["rate"]))
    if fmt["rate"] != rate:
        r.check(f"capture rate {rate} Hz honored", False, f"captured at {fmt['rate']} Hz")
    write_wav(Path(args.out).with_name(f"{Path(args.out).stem}_hear_{rate}.wav"), pcm, fmt["rate"])
    fmt, tpcm, why = await play_and_capture(g, dec, codec, rate, rp, tcmd, 2.5)
    if why:
        r.check(name, False, why)
        return True
    if not tpcm or len(tpcm) < 0.8 * 2.5 * fmt["rate"]:
        r.check(name, False, f"tone capture cut short: {len(tpcm or []) / fmt['rate']:.2f} s of 2.5 s "
                             f"(not a mic verdict)")
        return True
    t = settled(tpcm, fmt["rate"])
    lvl, f = rms_db(t), peak_hz(t, fmt["rate"])
    ok = lvl >= args.hear_db and lvl >= floor + 6
    if not ok and lvl >= args.hear_db and attempt < tries:
        print(f"  {name}: the quiet baseline ({floor:.1f} dBFS) is within 6 dB of the tone ({lvl:.1f}): "
              f"another sound in the room; measuring again ({attempt + 1} of {tries})")
        return False
    retried = f"; {attempt} tries (the quiet baseline had another sound)" if attempt > 1 else ""
    r.check(name, ok, f"1 kHz tone at {lvl:.1f} dBFS vs {floor:.1f} dBFS quiet (needs >= {args.hear_db:.0f} "
                      f"and +6 dB); speech ({what}) {speech:.1f} dBFS; {fmt['rate']} Hz, {data_type_name(codec)}"
                      f"{retried}")
    r.check(f"1 kHz tone at its frequency, {rate} Hz", abs(f - 1000.0) <= 25,
            f"peak {f:.0f} Hz (labeled {fmt['rate']} Hz; needs 1000 +-25)")
    return True


async def cmd_test(args) -> int:
    from bleak import BleakClient

    r = Results()
    addr, name = await pick(args)
    r.check("advertised name", gadget_name(name), name)
    dec = Decoder()
    async with BleakClient(addr, timeout=20.0) as client:
        r.check("connect", client.is_connected)
        mtu = await att_mtu(client)
        if mtu is None:
            mtu = client.mtu_size
            print(f"NOTE  ATT MTU: skipped, the negotiated MTU can't be read here (BlueZ reports {mtu} until "
                  "it's acquired)")
        else:
            r.check("ATT MTU", mtu >= P["MG_MIN_ATT_MTU"], f"{mtu} (protocol minimum {P['MG_MIN_ATT_MTU']})")
        try:
            man = await read_str(client, P["MG_DIS_MANUFACTURER_NAME_UUID16"])
            model = await read_str(client, P["MG_DIS_MODEL_NUMBER_UUID16"])
            fw = await read_str(client, P["MG_DIS_FIRMWARE_REVISION_UUID16"])
            sw = await read_str(client, P["MG_DIS_SOFTWARE_REVISION_UUID16"])
            r.check("Device Information", bool(man and model and fw),
                    f"manufacturer={man!r} model={model!r} firmware={fw!r} software={sw!r}")
            r.check("software revision", sw == f"mg{P['MG_SPEC_VERSION']}", sw)
        except Exception as e:
            r.check("Device Information", False, repr(e))
        try:
            level = bytes(await client.read_gatt_char(uuid16(P["MG_BAS_BATTERY_LEVEL_UUID16"])))[0]
            r.check("Battery Level", 0 <= level <= 100, f"{level}%")
        except Exception as e:
            r.check("Battery Level", False, repr(e))

        g = Gadget(client)
        await g.start()
        await asyncio.sleep(0.3)

        listed = None
        st = None
        try:
            st = await g.status()
            cdt = st["data_type"]
            fmt = parse_change_data_type(cdt)
            r.check("request_status: change_data_type", True, describe(cdt))
            cmds = st["commands"]
            codecs = st["typed"].get(P["mg_command_start_mic"], [])
            r.check("request_status: supported_features", P["mg_command_start_mic"] in cmds,
                    " ".join(command_name(c) for c in cmds))
            r.check("start_mic codecs", P["mg_data_type_audio_sbc"] in codecs,
                    ", ".join(data_type_name(c) for c in codecs))
            listed = st["settings"]
            r.check("supported settings list", listed is not None and setting_id("spec_version") in listed,
                    ", ".join(setting_name(s) for s in listed or []) or "not sent")
        except Exception as e:
            r.check("request_status", False, repr(e))
            cmds, fmt = [], None
        if listed is None:
            # Firmware from before the typed list: guess from the commands.
            optional = {"audio_queue_enabled": P["mg_command_audio_queue"],
                        "speaker_volume": P["mg_command_stream_audio"]}
            listed = [setting_id(n) for n in SETTING_WIDTH if n not in optional or optional[n] in cmds]
        # The token proof first, as the apps run it: push-to-talk and the mic go only to a proven connection.
        try:
            result = await prove_connection(g, addr, st or {"commands": cmds})
            if result is not None:
                r.check("token proof", result == "match", result)
        except Exception as e:
            r.check("token proof", False, repr(e))
        ids = [s for s in listed if s in {setting_id(n) for n in SETTING_WIDTH}]
        await g.send(P["mg_command_get_settings"], *ids)
        try:
            settings = parse_settings(await g.expect(P["mg_command_get_settings"]))
            r.check("get_settings", settings.get("spec_version") == P["MG_SPEC_VERSION"], str(settings))
            r.check("push_to_talk_enabled starts at 0", settings.get("push_to_talk_enabled") == 0)
        except Exception as e:
            r.check("get_settings", False, repr(e))
        await g.send(P["mg_command_set_settings"], setting_id("push_to_talk_enabled"), 1)
        await asyncio.sleep(0.3)
        errors = [p for p in g.drain() if p[0] == P["mg_command_error"]]
        r.check("set_settings push_to_talk_enabled=1 (no reply on success)", not errors,
                "; ".join(parse_error(e) for e in errors))
        await g.send(P["mg_command_get_settings"], setting_id("push_to_talk_enabled"))
        try:
            settings = parse_settings(await g.expect(P["mg_command_get_settings"]))
            r.check("push_to_talk_enabled reads back 1", settings.get("push_to_talk_enabled") == 1)
        except Exception as e:
            r.check("push_to_talk_enabled reads back", False, repr(e))
        # Every listed setting round-trips (set to what it is, so nothing changes)...
        for sid in ids:
            name = setting_name(sid)
            if name in ("spec_version", "push_to_talk_enabled"):
                continue   # read-only; checked above
            val = await get_setting(g, name)
            errors = await set_setting(g, name, val) if val is not None else ["no value"]
            back = await get_setting(g, name)
            r.check(f"{name} round trip", not errors and back == val,
                    f"{val}{' (speaker off)' if name == 'speaker_volume' and val == 0 else ''} set and read "
                    f"back {back}" + (f"; {'; '.join(errors)}" if errors else ""))
        # The app sent at connect reads back, where the gadget lists it; client_name (18) is
        # reserved (mgcommands.h): a gadget doesn't list it.
        r.check("client_name not listed (reserved)", P["mg_setting_parameter_client_name"] not in listed, str(listed))
        if IDENTIFY and P["mg_setting_parameter_client_app"] in listed:
            await g.send(P["mg_command_get_settings"], P["mg_setting_parameter_client_app"])
            try:
                who = parse_client_settings(await g.expect(P["mg_command_get_settings"]))
                want = parse_client_settings(bytes([P["mg_command_get_settings"]]) + identity_payload()[1:])
                r.check("client_app read back", who == want, str(who))
            except Exception as e:
                r.check("client_app read back", False, repr(e))
        # ...and every setting it doesn't list is refused.
        for name in SETTING_WIDTH:
            if setting_id(name) in listed:
                continue
            g.drain()
            await g.send(P["mg_command_get_settings"], setting_id(name))
            await asyncio.sleep(0.3)
            got = g.drain()
            refused_get = any(p[0] == P["mg_command_error"] for p in got)
            errors = await set_setting(g, name, 0)
            r.check(f"unlisted {name} refused", refused_get and bool(errors),
                    "; ".join(errors) if errors else "set_settings accepted it")

        # Client-driven capture.
        codec = P["mg_data_type_audio_" + args.codec]
        g.data.clear()
        await g.send(P["mg_command_start_mic"], codec)
        try:
            cdt = await g.expect(P["mg_command_change_data_type"], timeout=5)
            fmt = parse_change_data_type(cdt)
            r.check("start_mic: change_data_type", fmt["type"] == codec, describe(cdt))
        except Exception as e:
            r.check("start_mic: change_data_type", False, repr(e))
        await asyncio.sleep(args.secs)
        await g.send(P["mg_command_stop_mic"])
        try:
            await g.expect(P["mg_command_stop_mic"], timeout=3)
            r.check("stop_mic notification", True)
        except Exception as e:
            r.check("stop_mic notification", False, repr(e))
        await asyncio.sleep(0.3)
        chunks = list(g.data)
        payload = max((len(c) for c in chunks), default=0)
        r.check("audio on Data", bool(chunks),
                f"{len(chunks)} notifications, {sum(map(len, chunks))} bytes, largest {payload} "
                f"(fits {mtu - 3})")
        if chunks and fmt:
            try:
                pcm, frames = dec.decode(fmt, chunks)
                secs = len(pcm) / fmt["rate"]
                ok = secs > args.secs * 0.6
                r.check("frames decode", ok, f"{frames} frames, {secs:.2f} s of audio for {args.secs:.1f} s asked")
                peak = max((abs(s) for s in pcm), default=0)
                print(f"      peak {20 * __import__('math').log10(peak / 32768 + 1e-9):.1f} dBFS")
                out = Path(args.out)
                write_wav(out, pcm, fmt["rate"])
                print(f"      saved {out}")
            except Exception as e:
                r.check("frames decode", False, repr(e))

        if not args.no_hear:
            by_codec = capture_rates(st["typed"].get(P["mg_command_change_data_type"]) if st else None)
            rates = [x[0] for x in by_codec.get(codec, [])] or [16000]
            await hear_check(g, r, dec, codec, rates, args)

        if P["mg_command_audio_queue"] in cmds:
            await g.send(P["mg_command_audio_queue"], P["mg_audio_queue_command_status"])
            try:
                st = await g.expect(P["mg_command_audio_queue"], sub=P["mg_audio_queue_command_status"])
                enabled, used, cap, count = st[2], int.from_bytes(st[3:7], "little"), \
                    int.from_bytes(st[7:11], "little"), int.from_bytes(st[11:13], "little")
                r.check("audio_queue status", cap > 0,
                        f"enabled={enabled} {count} clips, {used} of {cap} bytes used")
            except Exception as e:
                r.check("audio_queue status", False, repr(e))
        else:
            print("NOTE  no offline queue on this gadget")
        if P["mg_command_stream_audio"] in cmds:
            await play_stream(g, dec, tone_pcm(1000, 2.0), 16000, r, mtu, label="playback: ")
        else:
            print("NOTE  no playback (stream_audio) on this gadget")
        if g.nus:
            print("NUS log mirror:\n  " + redact.redact_text("".join(g.nus)).strip().replace("\n", "\n  ")[-1500:])
        r.check("Nordic UART log mirror", bool(g.nus), f"{len(g.nus)} notifications")
    print(f"\n{len(r.failed)} failed" + (": " + ", ".join(r.failed) if r.failed else ""))
    return 1 if r.failed else 0


# ---- playback (stream_audio) ------------------------------------------------

def _need(payload: bytes, i: int, n: int) -> bytes:
    """payload[i:i + n], or IndexError when the message ends first."""
    if i + n > len(payload):
        raise IndexError(f"{n} bytes at {i}, message of {len(payload)}")
    return payload[i:i + n]


def parse_capabilities(payload: bytes) -> dict:
    """[stream_audio, report_capabilities, count, (capability, value)...] -> dict."""
    caps, i = {}, 3
    try:
        for _ in range(payload[2] if len(payload) > 2 else 0):
            cap = payload[i]
            i += 1
            if cap == P["mg_stream_audio_capability_codecs"]:
                n = payload[i]
                _need(payload, i + 1, n)
                caps["codecs"] = list(payload[i + 1:i + 1 + n])
                i += 1 + n
            elif cap == P["mg_stream_audio_capability_buffer_size"]:
                caps["buffer_size"] = int.from_bytes(_need(payload, i, 4), "little")
                i += 4
            elif cap == P["mg_stream_audio_capability_sample_rates"]:
                n = payload[i]
                _need(payload, i + 1, 4 * n)
                caps["sample_rates"] = [int.from_bytes(payload[i + 1 + 4 * k:i + 5 + 4 * k], "little") for k in range(n)]
                i += 1 + 4 * n
            elif cap == P["mg_stream_audio_capability_max_bitrate"]:
                caps["max_bitrate"] = int.from_bytes(_need(payload, i, 2), "little")
                i += 2
            elif cap == P["mg_stream_audio_capability_max_channels"]:
                caps["max_channels"] = payload[i]
                i += 1
            elif cap == P.get("mg_stream_audio_capability_flush"):
                caps["flush"] = payload[i]   # takes mg_stream_audio_flush (barge-in)
                i += 1
            elif cap == P.get("mg_stream_audio_capability_live_depth"):
                caps["live_depth"] = int.from_bytes(_need(payload, i, 2), "little")   # the floor in ms; takes parameter 6
                i += 2
            elif cap == P.get("mg_stream_audio_capability_video_sync"):
                caps["video_sync"] = payload[i]   # takes mg_stream_audio_video_anchor (lip sync)
                i += 1
            else:
                break   # unknown: can't know its width
    except IndexError:
        caps["truncated"] = True   # a value cut short: what came before it stands
    return caps


def start_streaming(codec: int, rate: int, channels: int = 1) -> bytes:
    sa = P["mg_command_stream_audio"]
    return bytes([sa, P["mg_stream_audio_start_streaming"], 3,
                  P["mg_stream_audio_parameter_codec"], codec,
                  P["mg_stream_audio_parameter_sample_rate"], *rate.to_bytes(4, "little"),
                  P["mg_stream_audio_parameter_channels"], channels])


def tone_pcm(freq: float, secs: float, rate: int = 16000, dbfs: float = -12.0) -> list[int]:
    amp = 32767 * 10 ** (dbfs / 20)
    n = int(secs * rate)
    fade = int(0.01 * rate)   # 10 ms ramps: no clicks
    out = []
    for i in range(n):
        env = min(1.0, i / fade, (n - 1 - i) / fade) if fade else 1.0
        out.append(int(round(amp * env * math.sin(2 * math.pi * freq * i / rate))))
    return out


def sweep_pcm(f0: float, f1: float, secs: float, rate: int = 16000, dbfs: float = -12.0) -> list[int]:
    """A logarithmic sweep from f0 to f1."""
    amp = 32767 * 10 ** (dbfs / 20)
    n = int(secs * rate)
    k = math.log(f1 / f0)
    fade = int(0.01 * rate)
    out = []
    for i in range(n):
        t = i / rate
        phase = 2 * math.pi * f0 * secs / k * (math.exp(t / secs * k) - 1)
        env = min(1.0, i / fade, (n - 1 - i) / fade)
        out.append(int(round(amp * env * math.sin(phase))))
    return out


def read_wav(path: Path) -> tuple[list[int], int]:
    """16-bit WAV -> mono samples (channels averaged), rate."""
    import array
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise SystemExit(f"{path}: only 16-bit WAV files")
        ch, rate = w.getnchannels(), w.getframerate()
        a = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    if ch > 1:
        a = [sum(a[i:i + ch]) // ch for i in range(0, len(a), ch)]
    return list(a), rate


def resample_linear(pcm: list[int], rate: int, to: int) -> list[int]:
    """Plain linear interpolation: fine for a test signal, not for music."""
    if rate == to:
        return pcm
    n = int(len(pcm) * to / rate)
    out = []
    for i in range(n):
        x = i * rate / to
        j = int(x)
        f = x - j
        a = pcm[j] if j < len(pcm) else 0
        b = pcm[j + 1] if j + 1 < len(pcm) else a
        out.append(int(a + (b - a) * f))
    return out


def goertzel_db(x: list[int], freq: float, rate: int = 16000) -> float:
    """Level of `freq` in x, in dBFS of a sine."""
    if not x:
        return -120.0
    k = 2 * math.cos(2 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + k * s1 - s2
        s2, s1 = s1, s0
    p = s1 * s1 + s2 * s2 - k * s1 * s2
    amp = 2 * math.sqrt(max(p, 0.0)) / len(x)
    return 20 * math.log10(amp / 32768 + 1e-9)


def rms_db(x: list[int]) -> float:
    if not x:
        return -120.0
    return 10 * math.log10(sum(v * v for v in x) / len(x) / 32768 ** 2 + 1e-12)


def zero_cross_hz(x: list[int], rate: int = 16000) -> float:
    """Dominant frequency by rising zero crossings, after removing DC."""
    if len(x) < 2:
        return 0.0
    mean = sum(x) / len(x)
    crossings = sum(1 for a, b in zip(x, x[1:]) if a - mean < 0 <= b - mean)
    return crossings * rate / len(x)


STATS_RE = re.compile(
    r"stream end \((?P<how>\w+)\): (?P<codec>\w+) (?P<rate>\d+) Hz, (?P<frames_in>\d+) frames in, "
    r"(?P<decoded>\d+) decoded \((?P<bad>\d+) bad\), (?P<underruns>\d+) underruns, (?P<silence_ms>\d+) ms silence"
    r".*?stream bytes: (?P<received>\d+) received \((?P<dropped>\d+) dropped\), (?P<played_ms>\d+) ms played"
    r".*?stream level: decoded rms (?P<decoded_rms>-?[\d.]+) peak (?P<decoded_peak>-?[\d.]+) dBFS, "
    r"speaker rms (?P<rms>-?[\d.]+) peak (?P<peak>-?[\d.]+) dBFS(?P<off>, speaker off(?: (?P<off_ms>\d+) ms)?)?",
    re.S)


def parse_stream_stats(text: str) -> dict | None:
    """The last per-stream stats the gadget logged on the Nordic UART mirror.

    rms/peak are what reached the speaker's codec (silence while it's off);
    decoded_rms/decoded_peak the decoded audio. speaker_off: the whole stream
    played muted; off_ms how long it was muted if only part.
    """
    found = list(STATS_RE.finditer(text))
    if not found:
        return None
    d = found[-1].groupdict()
    out = {}
    for k, v in d.items():
        if k == "off":
            continue
        if k in ("how", "codec"):
            out[k] = v
        elif k in ("rms", "peak", "decoded_rms", "decoded_peak"):
            out[k] = float(v)
        else:
            out[k] = int(v) if v is not None else 0
    out["speaker_off"] = bool(d["off"]) and d["off_ms"] is None
    return out


async def prove_if_kept(g: "Gadget", addr: str) -> str | None:
    """The token proof with this Mac's kept key for the gadget, if there is one (a set-up gadget takes streams and
    kept settings only from a proven connection): "match", "mismatch" or "error", or None with no kept key."""
    rec = load_proof_record(addr)
    if not rec:
        return None
    try:
        return await run_proof(g, rec["proof_key"])
    except (RuntimeError, asyncio.TimeoutError):
        return "error"


async def get_setting(g: "Gadget", name: str) -> int | None:
    await g.send(P["mg_command_get_settings"], setting_id(name))
    try:
        return parse_settings(await g.expect(P["mg_command_get_settings"])).get(name)
    except Exception:
        return None


async def set_setting(g: "Gadget", name: str, value: int) -> list[str]:
    """set_settings one value; returns the errors it drew (none on success)."""
    g.drain()
    w = SETTING_WIDTH[name]
    await g.send(P["mg_command_set_settings"], setting_id(name), *value.to_bytes(w, "little"))
    await asyncio.sleep(0.3)
    return [parse_error(p) for p in g.drain() if p[0] == P["mg_command_error"]]


async def run_volume(g: "Gadget", vol: int, wait_s: float = 2.0) -> dict:
    """speaker_volume vol for a bench run, never saved (bench and test volumes are RAM-only).
    Through the gadget's console word over the Nordic UART, `!IN vol=N` (bench builds, or a proven connection on
    release builds), which sets
    the speaker in RAM only: the saved volume is untouched, and end_run_volume (`!IN vol=saved`) or a reboot
    puts it back. A gadget without the word (older firmware: speaker_volume doesn't read vol after wait_s) gets
    set_settings instead, which saves, and end_run_volume writes the original back. Returns what
    end_run_volume needs, with "how": "ram", "saved" or "none" (already at vol)."""
    before = await get_setting(g, "speaker_volume")
    if before == vol:
        return {"how": "none", "before": before, "errors": []}
    await g.client.write_gatt_char(P["MG_NUS_RX_UUID"], f"!IN vol={vol}\n".encode(), response=True)
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        await asyncio.sleep(0.1)
        if await get_setting(g, "speaker_volume") == vol:
            return {"how": "ram", "before": before, "errors": []}
    errors = await set_setting(g, "speaker_volume", vol)
    return {"how": "saved", "before": before, "errors": errors}


async def end_run_volume(g: "Gadget", state: dict | None) -> bool:
    """Undoes run_volume: `!IN vol=saved` after a RAM volume, the original written back after a saved one.
    True when nothing is left to undo."""
    if not state or state.get("how") == "none":
        return True
    if state["how"] == "ram":
        await g.client.write_gatt_char(P["MG_NUS_RX_UUID"], b"!IN vol=saved\n", response=True)
        return True
    if state.get("before") is None:
        return False
    return not await set_setting(g, "speaker_volume", state["before"])


async def play_stream(g: "Gadget", dec: "Decoder", pcm: list[int], rate: int, r: "Results", mtu: int,
                      bitpool: int = 26, label: str = "") -> dict:
    """Streams pcm as SBC with the protocol's flow control; PASS/FAIL lines into r. Returns what it saw."""
    sa = P["mg_command_stream_audio"]
    out = {"ok": False}
    g.stream_errors.clear()
    g.stops.clear()
    nus_from = len(g.nus)
    await g.send(sa, P["mg_stream_audio_request_capabilities"])
    try:
        caps = parse_capabilities(await g.expect(sa, sub=P["mg_stream_audio_report_capabilities"]))
    except Exception as e:
        r.check(f"{label}capabilities", False, repr(e))
        return out
    sbc = P["mg_data_type_audio_sbc"]
    r.check(f"{label}capabilities", sbc in caps.get("codecs", []) and 16000 in caps.get("sample_rates", []),
            f"codecs {', '.join(data_type_name(c) for c in caps.get('codecs', []))}; "
            f"rates {caps.get('sample_rates')}; buffer {caps.get('buffer_size')} B; "
            f"max {caps.get('max_bitrate')} kb/s, {caps.get('max_channels')} ch")
    if rate not in caps.get("sample_rates", []):
        print(f"NOTE  {rate} Hz isn't offered: resampling to 16000 (linear)")
        pcm, rate = resample_linear(pcm, rate, 16000), 16000
    frames, fb = dec.sbc_encode(pcm, rate, bitpool)
    secs = len(frames) * 128 / rate
    per_write = max(1, att_payload(mtu) // fb)
    writes = [b"".join(frames[i:i + per_write]) for i in range(0, len(frames), per_write)]
    total = sum(map(len, writes))
    g.buf_event.clear()
    await g.client.write_gatt_char(P["MG_CONTROL_UUID"], start_streaming(sbc, rate), response=False)
    try:
        await asyncio.wait_for(g.buf_event.wait(), 3)
        r.check(f"{label}start_streaming accepted", not g.stream_errors,
                f"SBC {rate} Hz, {len(frames)} frames of {fb} B ({secs:.2f} s, "
                f"{fb * 8 * rate / 128 / 1000:.0f} kb/s), {per_write} per write; buffer {g.buf_avail} B free")
    except asyncio.TimeoutError:
        r.check(f"{label}start_streaming accepted", False, "; ".join(g.stream_errors) or "no buffer_update")
        return out
    sent, waits, t_first = 0, 0, None
    for w in writes:
        while sent - g.buf_received + len(w) > g.buf_avail:
            # Not room for it: ask where the device is, and wait for its answer
            # (at most every 20 ms: the device frees a chunk per 20 ms it plays).
            if waits:
                await asyncio.sleep(0.02)
            waits += 1
            g.buf_event.clear()
            await g.client.write_gatt_char(P["MG_CONTROL_UUID"],
                                           bytes([sa, P["mg_stream_audio_request_buffer_update"]]), response=False)
            try:
                await asyncio.wait_for(g.buf_event.wait(), 2)
            except asyncio.TimeoutError:
                r.check(f"{label}buffer_update while streaming", False, "none for 2 s")
                return out
            if g.stops:
                break
        if g.stops:
            break
        await wait_write_room(g.client)
        await g.client.write_gatt_char(P["MG_DATA_UUID"], w, response=False)
        t_first = t_first or time.monotonic()
        sent += len(w)
    # Everything the device received, by its own count.
    deadline = time.monotonic() + 3
    while g.buf_received < sent and time.monotonic() < deadline and not g.stops:
        await asyncio.sleep(0.02)
        g.buf_event.clear()
        await g.client.write_gatt_char(P["MG_CONTROL_UUID"],
                                       bytes([sa, P["mg_stream_audio_request_buffer_update"]]), response=False)
        try:
            await asyncio.wait_for(g.buf_event.wait(), 1)
        except asyncio.TimeoutError:
            pass
    r.check(f"{label}bytes sent = device total received", g.buf_received == sent == total,
            f"sent {sent} of {total}, device received {g.buf_received}; {waits} flow-control waits")
    await g.send(sa, P["mg_stream_audio_stop_streaming"], P["mg_stream_audio_buffer_keep"])
    deadline = time.monotonic() + secs + 5
    while not g.stops and time.monotonic() < deadline:
        await asyncio.sleep(0.05)
    t_stop = g.stops[0][0] if g.stops else None
    kept = bool(g.stops) and g.stops[0][1] == P["mg_stream_audio_buffer_keep"]
    r.check(f"{label}stop_streaming keep acknowledged", kept,
            "keep" if kept else ("drop" if g.stops else "no stop_streaming from the device"))
    r.check(f"{label}no errors", not g.stream_errors, "; ".join(g.stream_errors))
    if t_stop and t_first:
        took = t_stop - t_first
        # Playback starts after ~100 ms of prebuffer and ends with 100 ms of silence.
        r.check(f"{label}duration", secs - 0.3 <= took <= secs + 1.5,
                f"{took:.2f} s from the first write to the device's stop, for {secs:.2f} s of audio")
    # The device's own account of the stream, on the Nordic UART mirror.
    deadline = time.monotonic() + 2
    stats = None
    while time.monotonic() < deadline and not (stats := parse_stream_stats("".join(g.nus[nus_from:]))):
        await asyncio.sleep(0.1)
    if stats:
        good = (stats["decoded"] == len(frames) and stats["bad"] == 0 and stats["dropped"] == 0
                and abs(stats["played_ms"] - secs * 1000) <= 60)
        r.check(f"{label}device stats", good,
                f"{stats['decoded']} of {len(frames)} frames decoded, {stats['bad']} bad, {stats['underruns']} "
                f"underruns ({stats['silence_ms']} ms silence), {stats['played_ms']} ms played; decoded rms "
                f"{stats['decoded_rms']} dBFS, speaker rms {stats['rms']} peak {stats['peak']} dBFS"
                + (", SPEAKER OFF" if stats["speaker_off"] else f", speaker off {stats['off_ms']} ms"
                   if stats["off_ms"] else ""))
    else:
        print(f"NOTE  no stats line on the Nordic UART mirror (subscribed: {bool(g.nus)})")
    out.update(ok=kept and not g.stream_errors, secs=secs, frames=len(frames), sent=sent, stats=stats)
    return out


async def capture(g: "Gadget", dec: "Decoder", secs: float | None = None, until=None) -> list[int]:
    """start_mic (SBC), then stop_mic after `secs` or once `until` (a coroutine) finishes; decoded PCM."""
    g.data.clear()
    await g.send(P["mg_command_start_mic"], P["mg_data_type_audio_sbc"])
    fmt = parse_change_data_type(await g.expect(P["mg_command_change_data_type"], timeout=5))
    result = None
    if until is not None:
        result = await until
    else:
        await asyncio.sleep(secs)
    await g.send(P["mg_command_stop_mic"])
    await g.expect(P["mg_command_stop_mic"], timeout=3)
    await asyncio.sleep(0.2)
    pcm, _ = dec.decode(fmt, list(g.data))
    return pcm if until is None else (pcm, result)


async def cmd_play(args) -> int:
    from bleak import BleakClient

    r = Results()
    dec = Decoder()
    if args.wav:
        pcm, rate = read_wav(Path(args.wav))
        what = f"{args.wav} ({len(pcm) / rate:.2f} s at {rate} Hz)"
    elif args.sweep:
        rate = args.rate
        pcm = sweep_pcm(200, min(7000, rate / 2 * 0.9), args.secs, rate, args.level)
        what = f"a 200 Hz-7 kHz sweep, {args.secs:.1f} s at {rate} Hz, {args.level:.0f} dBFS"
    else:
        rate = args.rate
        pcm = tone_pcm(args.tone, args.secs, rate, args.level)
        what = f"a {args.tone:.0f} Hz tone, {args.secs:.1f} s at {rate} Hz, {args.level:.0f} dBFS"
    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        mtu = await att_mtu(client) or client.mtu_size
        g = Gadget(client, verbose=args.verbose)
        await g.start()
        await asyncio.sleep(0.3)
        await g.send(P["mg_command_request_status"])
        typed: dict = {}
        feats = await g.command_list(typed)
        if not r.check("stream_audio offered", P["mg_command_stream_audio"] in feats[1:]):
            return 1
        # A set-up gadget takes audio, sends mic audio and takes the volume only on a proven connection: this
        # one proves first whenever a key is saved, a plain play too.
        result = await prove_connection(g, addr, {"commands": list(feats[1:]),
                                                  "recovery": typed.get(P["mg_command_request_status"])})
        if args.volume is not None and result not in (None, "match", "not_found"):
            r.check("token proof", False, f"{result}: a set-up gadget refuses --volume on this connection")
            return 1
        if result not in (None, "match"):
            print(f"NOTE  token proof: {result} (a set-up gadget takes no audio from this connection and sends it "
                  "no mic audio)")
        run_vol = None
        if args.volume is not None:
            # For this run only (bench volumes are RAM-only): the gadget's vol= word, or on an older
            # gadget set_settings with the original written back at the end.
            run_vol = await run_volume(g, args.volume)
            errors = run_vol["errors"]
            vol = await get_setting(g, "speaker_volume")
            r.check(f"speaker_volume set to {args.volume} for the run ({run_vol['how']})",
                    not errors and vol == args.volume,
                    f"reads back {vol}" + (f"; {'; '.join(errors)}" if errors else ""))
        else:
            vol = await get_setting(g, "speaker_volume")
        try:
            if vol == 0:
                print("NOTE  the gadget's speaker is off (speaker_volume 0): it plays silence; turn it on with --volume N")
            elif vol is not None:
                print(f"      speaker volume {vol}")
            print(f"playing {what}")
            if not args.loopback:
                await play_stream(g, dec, pcm, rate, r, mtu, args.bitpool)
            else:
                # A silent baseline from the mic first, then the mic again while the speaker plays.
                base = await capture(g, dec, secs=1.5)
                cap, res = await capture(g, dec, until=play_stream(g, dec, pcm, rate, r, mtu, args.bitpool))
                out = Path(args.out)
                write_wav(out, cap)
                print(f"      saved the mic capture to {out} ({len(cap) / 16000:.2f} s)")
                stats = res.get("stats") or {}
                if vol == 0 or stats.get("speaker_off"):
                    r.check("loopback", False, "the speaker is off (speaker_volume 0), so the mic can't hear it: "
                                               "rerun with --volume 60")
                    print(f"\n{len(r.failed)} failed: " + ", ".join(r.failed))
                    return 1
                if stats and stats["rms"] < -60:
                    r.check("loopback", False, f"the speaker got silence (rms {stats['rms']} dBFS at the codec)")
                skip = int(0.4 * 16000)   # start_mic, prebuffer and the first frames
                seg = cap[skip:skip + int((res.get("secs", args.secs) - 0.3) * 16000)]
                b = base[int(0.3 * 16000):]
                spk = (f"; speaker at volume {vol}, {stats['rms']} dBFS rms at the codec" if stats else
                       f"; speaker at volume {vol}")
                if args.sweep or args.wav:
                    lvl, floor = rms_db(seg), rms_db(b)
                    r.check("loopback level", lvl >= floor + 6,
                            f"mic {lvl:.1f} dBFS while playing vs {floor:.1f} dBFS silent{spk}")
                else:
                    f = args.tone
                    lvl, floor = goertzel_db(seg, f), goertzel_db(b, f)
                    others = max(goertzel_db(seg, f * m) for m in (0.8, 1.25))
                    zc = zero_cross_hz(seg)
                    r.check("loopback tone level", lvl >= floor + 10,
                            f"{f:.0f} Hz at {lvl:.1f} dBFS while playing vs {floor:.1f} dBFS silent{spk}")
                    r.check("loopback tone frequency", lvl >= others + 6 and abs(zc - f) <= 0.1 * f,
                            f"{f:.0f} Hz bin {lvl - others:.1f} dB above its neighbours; zero crossings say {zc:.0f} Hz")
            await g.send(P["mg_command_request_status"])
            if args.verbose and g.nus:
                print("NUS log mirror:\n  " + redact.redact_text("".join(g.nus)).strip().replace("\n", "\n  ")[-2000:])
        finally:
            if not await end_run_volume(g, run_vol):   # back to the saved volume
                print(f"WARNING: the gadget's speaker_volume wasn't put back to {run_vol['before']}")
    print(f"\n{len(r.failed)} failed" + (": " + ", ".join(r.failed) if r.failed else ""))
    return 1 if r.failed else 0


async def cmd_ptt(args) -> int:
    """Waits for the talk button: records each utterance until stop_mic, then saves it."""
    from bleak import BleakClient

    addr, _ = await pick(args)
    dec = Decoder()
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client)
        g.face_log = True
        await g.start()
        st = await g.status()
        states = not args.no_states and P["mg_command_assistant_state"] in st["commands"]
        if not args.no_states and not states:
            print("NOTE  this gadget doesn't take assistant_state: no face states sent")
        result = await prove_connection(g, addr, st)
        if result not in (None, "match"):
            print(f"NOTE  token proof: {result} (the gadget sends no audio to this connection)")
        on = [setting_id("push_to_talk_enabled"), 1]
        if args.haptics:
            if setting_id("haptics_enabled") in (st["settings"] or []):
                on += [setting_id("haptics_enabled"), 1]
            else:
                print("NOTE  no vibration motor (haptics_enabled isn't listed): no activation buzz")
        await g.send(P["mg_command_set_settings"], *on)
        print(f"push-to-talk on: hold the gadget's talk button and speak (Ctrl-C to stop) ...")
        n = 0
        while args.count == 0 or n < args.count:
            fmt = None
            while True:
                p = await g.control.get()
                if p[0] == P["mg_command_change_data_type"]:
                    fmt = parse_change_data_type(p)
                    g.data.clear()
                    t0 = time.monotonic()
                elif p[0] == P["mg_command_stop_mic"] and fmt:
                    break
            await asyncio.sleep(0.2)
            n += 1
            chunks = list(g.data)
            try:
                pcm, frames = dec.decode(fmt, chunks)
                out = Path(args.out_dir) / f"ptt_{time.strftime('%H%M%S')}_{n}.wav"
                out.parent.mkdir(parents=True, exist_ok=True)
                write_wav(out, pcm, fmt["rate"])
                print(f"utterance {n}: {data_type_name(fmt['type'])}, {frames} frames, "
                      f"{len(pcm) / fmt['rate']:.2f} s ({time.monotonic() - t0:.2f} s wall) -> {out}")
            except Exception as e:
                print(f"utterance {n}: {len(chunks)} notifications, decode failed: {e}")
            if states:
                # A turn as the Muse app runs it: the gadget shows thinking by itself after stop_mic.
                await asyncio.sleep(args.think)
                await g.send(P["mg_command_assistant_state"], assistant_state("responding"))
                await asyncio.sleep(args.respond)
                await g.send(P["mg_command_assistant_state"], assistant_state("done"))
                await asyncio.sleep(0.5)
    return 0


# ---- tokens, for printing ------------------------------------------------------

def mask_token(t: str) -> str:
    """Enough of a token to tell two apart and none of its characters: its
    length and a SHA-256 prefix, as the firmware logs it (redact.fingerprint)."""
    return redact.fingerprint(t)


def token_fingerprint(t: bytes) -> str:
    """The first 4 bytes of SHA-256, as the device logs it."""
    return hashlib.sha256(t).hexdigest()[:8]


# ---- Muse Link setup (protocols/README.md, Gadget setup over Muse Link) --------
# The setup service of esp32/main/ble_server.c, and community pairing v5 as
# esp32/main/link_pairing.c and pairing_transcript.c run it. Vectors:
# esp32/tests/vectors/link_pairing_v5.json.

SETUP_SERVICE_UUID = "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c"
SETUP_RX_UUID = "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01"
SETUP_TX_UUID = "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c"
SETUP_CHUNK_MAGIC = 0xFE
PAIRING_VERSION = 5
PAIRING_MODEL = "hatch_link"
PAIRING_SUITE = "p256-hkdf-sha256-aes-gcm-v1"
PAIRING_POLICY = "confirm_press"
PAIRING_AUTH = "none"            # community pairing: no manufacturer attestation
PAIRING_CONFIRM_TIMEOUT_S = 60
PAIRING_RECORD_LABEL = "hatch-link ble setup v1"
PAIRING_SESSION_ID_LABEL = b"hatch-link session id v1"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def unb64url(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def hkdf_sha256(salt: bytes, ikm: bytes, info: bytes, length: int = 32) -> bytes:
    """RFC 5869 extract and expand."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, i = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([i]), hashlib.sha256).digest()
        out += block
        i += 1
    return out[:length]


def pairing_transcript(device: dict, mobile_pub: str, mobile_nonce: str) -> str:
    """The v5 community transcript both sides hash (pairing_transcript.c); device is pairing_ready."""
    return "\n".join([
        f"hatch-link-pairing-v{PAIRING_VERSION}",
        f"version={PAIRING_VERSION}",
        "initiator_role=mobile",
        "responder_role=link",
        f"device_id={device['device_id']}",
        f"node_id={device['node_id']}",
        f"mac={device['mac']}",
        f"model={PAIRING_MODEL}",
        f"firmware_version={device['firmware_version']}",
        f"selected_cipher_suite={PAIRING_SUITE}",
        f"pairing_auth={PAIRING_AUTH}",
        "pairing_auth_epoch=0",
        f"pairing_policy={PAIRING_POLICY}",
        f"confirm_timeout_seconds={PAIRING_CONFIRM_TIMEOUT_S}",
        f"mobile_pub={mobile_pub}",
        f"device_pub={device['device_pub']}",
        f"mobile_nonce={mobile_nonce}",
        f"device_nonce={device['device_nonce']}",
    ])


def pairing_keys(ecdh: bytes, mobile_nonce: bytes, device_nonce: bytes, transcript_hash: bytes) -> dict:
    """The session's keys (derive_session_keys in link_pairing.c), from the mobile side."""
    salt = hashlib.sha256(mobile_nonce + device_nonce + transcript_hash).digest()
    secret = hkdf_sha256(salt, ecdh, PAIRING_RECORD_LABEL.encode())

    def expand(label: bytes) -> bytes:
        return hmac.new(secret, label + b"\x01", hashlib.sha256).digest()

    sid = hashlib.sha256(PAIRING_SESSION_ID_LABEL + transcript_hash + ecdh).digest()[:16]
    return {"session_secret": secret, "tx": expand(b"mobile->device"), "rx": expand(b"device->mobile"),
            "session_id": b64url(sid)}


def record_nonce(direction: int, counter: int) -> bytes:
    """direction 0 is mobile to device, 1 device to mobile; the counter big-endian in the last 8 bytes."""
    return bytes([direction, 0, 0, 0]) + counter.to_bytes(8, "big")


def record_aad(session_id: str, direction: int, counter: int) -> bytes:
    return f"{PAIRING_RECORD_LABEL}|{session_id}|{'m2d' if direction == 0 else 'd2m'}|{counter}".encode()


def seal_record(key: bytes, session_id: str, counter: int, plaintext: bytes, direction: int = 0) -> dict:
    """A pairing_encrypted envelope (the client's carry "action"; the device's "type")."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    sealed = AESGCM(key).encrypt(record_nonce(direction, counter), plaintext, record_aad(session_id, direction, counter))
    return {"action" if direction == 0 else "type": "pairing_encrypted", "session_id": session_id,
            "counter": str(counter), "ciphertext": b64url(sealed[:-16]), "tag": b64url(sealed[-16:])}


def open_record(key: bytes, session_id: str, counter: int, env: dict, direction: int = 1) -> bytes:
    """The plaintext of a pairing_encrypted envelope; raises on a wrong session, counter or tag."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    if env.get("session_id") != session_id or env.get("counter") != str(counter):
        raise ValueError(f"unexpected record: session {env.get('session_id')} counter {env.get('counter')}")
    data = unb64url(env["ciphertext"]) + unb64url(env["tag"])
    return AESGCM(key).decrypt(record_nonce(direction, counter), data, record_aad(session_id, direction, counter))


def setup_chunks(data: bytes, mtu: int) -> list[bytes]:
    """RX writes of one message: [0xFE, index, total] then up to MTU - 6 bytes each."""
    room = max(1, min(mtu - 3, 180) - 3)
    parts = [data[i:i + room] for i in range(0, len(data), room)] or [b""]
    if len(parts) > 255:
        raise ValueError("message too long for the setup framing")
    return [bytes([SETUP_CHUNK_MAGIC, i, len(parts)]) + p for i, p in enumerate(parts)]


class SetupReassembler:
    """TX notifications into messages: chunked JSON, or a bare status string (plaintext errors)."""

    def __init__(self):
        self.parts: dict[int, bytes] = {}
        self.total = 0

    def feed(self, payload: bytes) -> bytes | None:
        if len(payload) < 3 or payload[0] != SETUP_CHUNK_MAGIC:
            return payload
        idx, total = payload[1], payload[2]
        if idx >= total:
            return None   # a malformed chunk: skipped
        if idx == 0 or total != self.total:
            self.parts, self.total = {}, total
        self.parts[idx] = payload[3:]
        if len(self.parts) < self.total:
            return None
        out = b"".join(self.parts[i] for i in range(self.total))
        self.parts, self.total = {}, 0
        return out


class SetupLink:
    """A connection to the setup service: write messages, read them back in order."""

    def __init__(self, client):
        self.client = client
        self.rx = SetupReassembler()
        self.messages: asyncio.Queue = asyncio.Queue()

    async def start(self) -> None:
        await self.client.start_notify(SETUP_TX_UUID, self._on_tx)

    def _on_tx(self, _char, payload: bytearray) -> None:
        msg = self.rx.feed(bytes(payload))
        if msg is not None:
            self.messages.put_nowait(msg)

    async def send(self, obj: dict) -> None:
        data = json.dumps(obj, separators=(",", ":")).encode()
        for chunk in setup_chunks(data, getattr(self.client, "mtu_size", 23)):
            await self.client.write_gatt_char(SETUP_RX_UUID, chunk, response=True)

    async def receive(self, timeout: float) -> dict | str:
        """The next message: a JSON object, or a bare status string."""
        msg = await asyncio.wait_for(self.messages.get(), timeout)
        try:
            return json.loads(msg)
        except ValueError:
            return msg.decode("utf-8", "replace")


class PairingSession:
    """The mobile side of community pairing v5, over a SetupLink."""

    def __init__(self, link: SetupLink, private_value: int | None = None, mobile_nonce: bytes | None = None):
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

        self.link = link
        self.priv = (ec.derive_private_key(private_value, ec.SECP256R1()) if private_value
                     else ec.generate_private_key(ec.SECP256R1()))
        self.mobile_pub = b64url(self.priv.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint))
        self.mobile_nonce = mobile_nonce or os.urandom(16)
        self.keys: dict = {}
        self.device: dict = {}
        self.tx_counter = 0
        self.rx_counter = 0

    def hello(self) -> dict:
        return {"action": "pairing_client_hello", "version": PAIRING_VERSION, "pairing_auth": PAIRING_AUTH,
                "pairing_policy": PAIRING_POLICY, "mobile_pub": self.mobile_pub,
                "mobile_nonce": b64url(self.mobile_nonce)}

    def accept_ready(self, ready: dict) -> None:
        """Checks pairing_ready (transcript hash, session id) and keeps the session keys."""
        from cryptography.hazmat.primitives.asymmetric import ec

        if ready.get("type") != "pairing_ready" or ready.get("version") != PAIRING_VERSION:
            raise RuntimeError(f"not a v5 pairing_ready: {redact.record_text(ready)}")
        if ready.get("pairing_auth") != PAIRING_AUTH or ready.get("pairing_policy") != PAIRING_POLICY:
            raise RuntimeError("the device offered another pairing mode")
        transcript = pairing_transcript(ready, self.mobile_pub, b64url(self.mobile_nonce))
        th = hashlib.sha256(transcript.encode()).digest()
        if b64url(th) != ready.get("transcript_hash"):
            raise RuntimeError("transcript hash mismatch")
        peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), unb64url(ready["device_pub"]))
        ecdh = self.priv.exchange(ec.ECDH(), peer)
        keys = pairing_keys(ecdh, self.mobile_nonce, unb64url(ready["device_nonce"]), th)
        if keys["session_id"] != ready.get("session_id"):
            raise RuntimeError("session id mismatch")
        self.keys, self.device = keys, ready

    def seal(self, obj: dict) -> dict:
        env = seal_record(self.keys["tx"], self.keys["session_id"], self.tx_counter,
                          json.dumps(obj, separators=(",", ":")).encode())
        self.tx_counter += 1
        return env

    def open(self, env: dict) -> dict:
        plain = open_record(self.keys["rx"], self.keys["session_id"], self.rx_counter, env)
        self.rx_counter += 1
        return json.loads(plain)

    async def status(self, timeout: float) -> dict:
        """The next encrypted status; a bare plaintext status is an error from the device."""
        msg = await self.link.receive(timeout)
        if isinstance(msg, str):
            raise RuntimeError(f"device: {redact.record_text(msg)}")
        if msg.get("type") != "pairing_encrypted":
            raise RuntimeError(f"unexpected message: {redact.record_text(msg)}")
        return self.open(msg)


async def setup_flow(link: SetupLink, access: str, refresh: str, prompt=print, session: PairingSession | None = None,
                     confirm_timeout: float = PAIRING_CONFIRM_TIMEOUT_S + 5, confirm=None, set_up: bool = False) -> dict:
    """device_info, pairing, the button, then token-only provision_v2. Returns what setup stored.
    `confirm`, if given, is awaited instead of prompting for the press (it presses the
    button some other way, e.g. over the serial console) and returns False on failure.
    set_up: the gadget is set up already, so the talk key doesn't confirm (it dismisses the prompt): a second key or
    Confirm does, or the talk key in the gadget's setup mode."""
    await link.send({"action": "get_device_info"})
    info = await link.receive(10)
    if not isinstance(info, dict) or info.get("type") != "device_info":
        raise RuntimeError(f"no device_info: {redact.record_text(info)}")
    wifi = info.get("wifi", "required")
    if wifi == "required":
        raise RuntimeError("this device needs Wi-Fi in provision_v2 (no gadget setup); use the Muse app")
    if info.get("pairing_protocol") != PAIRING_VERSION or info.get("pairing_auth") != PAIRING_AUTH:
        raise RuntimeError(f"needs community pairing v5, the device offers {info.get('pairing_protocol')} "
                           f"{info.get('pairing_auth')}")
    s = session or PairingSession(link)
    await link.send(s.hello())
    ready = await link.receive(10)
    if isinstance(ready, str):
        raise RuntimeError(f"device: {redact.record_text(ready)}")
    s.accept_ready(ready)
    if ready["node_id"] != info["node_id"]:
        raise RuntimeError("pairing_ready names another node")
    await link.send(s.seal({"action": "pairing_client_finished"}))
    st = await s.status(10)
    if st.get("status") != "confirm_required":
        raise RuntimeError(f"expected confirm_required, got {redact.record_text(st)}")
    if confirm is None and set_up:
        prompt("confirm setting the gadget up again with one press of its second key, or one tap on Confirm on a "
               "touch screen (never the talk key, which dismisses it; a board whose only key is its talk key: power "
               "it on with the key pressed, let go within 10 s, then press it once)")
    elif confirm is None:
        prompt("press the gadget's button to confirm pairing (talk button on Muse boards)")
    elif not await confirm():
        raise RuntimeError("the serial console didn't confirm the pairing")
    st = await s.status(confirm_timeout)
    if st.get("status") != "pairing_confirmed":
        raise RuntimeError(f"pairing not confirmed: {st.get('status')}")
    sdk_token = st.get("sdk_token", "")
    await link.send(s.seal({"action": "provision_v2", "access_token": access, "refresh_token": refresh,
                            "token_type": "device"}))
    st = await s.status(30)
    if st.get("status") != "auth_ok":
        raise RuntimeError(f"provisioning failed: {st.get('status')}")
    return {"node_id": info["node_id"], "wifi": wifi, "mgcommands": info.get("mgcommands") == 1,
            "sdk_token": sdk_token, "proof_key": proof_key(access, info["node_id"])}


# ---- the token proof (mgcommands.h, Token proof) ------------------------------

PROOF_SALT = b"mg token proof v1"


def proof_key(access_token: str, node_id: str) -> bytes:
    """K = HKDF-SHA256(salt "mg token proof v1", IKM access token, info node_id, 32 bytes)."""
    return hkdf_sha256(PROOF_SALT, access_token.encode("utf-8"), node_id.encode("utf-8"))


def proof_mac(k: bytes, device: bool, client_nonce: bytes, device_nonce: bytes) -> bytes:
    label = b"mg token proof v1 device" if device else b"mg token proof v1 client"
    return hmac.new(k, label + client_nonce + device_nonce, hashlib.sha256).digest()


def proof_dir() -> Path:
    return Path(os.environ.get("MG_BLE_CLIENT_HOME", Path.home() / ".mg_ble_client"))


def proof_path(address: str) -> Path:
    return proof_dir() / ("proof-" + re.sub(r"[^0-9A-Za-z]", "_", address.lower()) + ".json")


def save_proof_record(address: str, node_id: str, k: bytes, access_token: str) -> Path:
    """What the app keeps for a gadget: node_id and K (and a fingerprint), never the tokens. Mode 0600."""
    d = proof_dir()
    d.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(d, 0o700)
    path = proof_path(address)
    body = json.dumps({"address": address, "node_id": node_id, "proof_key": k.hex(),
                       "access_fingerprint": token_fingerprint(access_token.encode())}, indent=1)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(body + "\n")
    os.chmod(path, 0o600)
    return path


def load_proof_record(address: str) -> dict | None:
    try:
        rec = json.loads(proof_path(address).read_text())
    except (OSError, ValueError):
        return None
    rec["proof_key"] = bytes.fromhex(rec["proof_key"])
    return rec


async def run_proof(g: "Gadget", k: bytes, clear: bool = False, busy_s: float = 20.0) -> str:
    """challenge, check the device's MAC, confirm. "match", "mismatch", "device_mac_mismatch" or "not_found".
    A busy answer asks again with a new challenge 200 ms later, for up to busy_s from the first. clear: the
    unsealed token_proof clear, for a gadget that doesn't declare mg_session_cap_sealed_clear (sealed_clear())."""
    tp = P["mg_command_token_proof"]
    n = P["MG_TOKEN_PROOF_NONCE_LEN"]
    deadline = time.monotonic() + busy_s
    while True:
        client_nonce = os.urandom(n)
        await g.send(tp, P["mg_token_proof_challenge"], *client_nonce)
        try:
            rsp = await g.expect(tp, sub=P["mg_token_proof_response"])
            device_nonce, mac = rsp[2:2 + n], rsp[2 + n:2 + n + P["MG_TOKEN_PROOF_MAC_LEN"]]
            if not hmac.compare_digest(mac, proof_mac(k, True, client_nonce, device_nonce)):
                return "device_mac_mismatch"   # the gadget doesn't hold this key: don't confirm
            await g.send(tp, P["mg_token_proof_confirm"], *proof_mac(k, False, client_nonce, device_nonce))
            res = await g.expect(tp, sub=P["mg_token_proof_result"])
        except RuntimeError as e:
            if "not_found" in str(e):
                return "not_found"
            if str(e).endswith(": busy") and time.monotonic() < deadline:
                await asyncio.sleep(0.2)
                continue
            raise
        break
    if len(res) < 3:
        raise RuntimeError("short token_proof result")
    if res[2] != 1:
        return "mismatch"
    g.proof_nonces = (client_nonce, device_nonce)   # a sealed hello's keys (sealed_hello())
    if clear:
        await g.send(tp, P["mg_token_proof_clear"])
        res = await g.expect(tp, sub=P["mg_token_proof_result"])
        if len(res) < 3 or res[2] != 1:
            raise RuntimeError("clear refused")
    return "match"


async def prove_connection(g: "Gadget", addr: str, st: dict) -> str | None:
    """This connection's token proof with the key `setup` saved, before push-to-talk, the mic or clips: a set-up
    gadget sends those only to a proven connection, and a proof on another connection doesn't count. None when
    there's nothing to prove with (no token_proof listed, no saved key, or a recovery mode, which is never
    proved)."""
    if st.get("recovery") is not None:
        return None
    if P["mg_command_token_proof"] not in st["commands"]:
        print("NOTE  no token proof on this gadget")
        return None
    rec = load_proof_record(addr)
    if not rec:
        print("NOTE  no proof key for this gadget (run `setup`): token proof skipped")
        return None
    return await run_proof(g, rec["proof_key"])


# ---- the sealed device session (mgcommands.h, Device session, Sealed credentials) ----
#
# What the Muse app does for a bench row, and nothing more: a sealed hello after a matching token proof (its
# credentials read and dropped: never printed or kept), then messages sealed on the client-to-device counter (the
# sealed clear). The seal is mgcommands.h's (HMAC-SHA256 stream and tag), checked against the vectors
# (tests/test_mg_ble_client.py).

SEAL_SALT = b"mg session seal v1"
SEAL_STREAM_LABEL = b"mg session seal v1 stream"
SEAL_TAG_LABEL = b"mg session seal v1 tag"
SEAL_TAG_LEN = 16
SEAL_DEVICE_TO_CLIENT, SEAL_CLIENT_TO_DEVICE = 1, 2


def seal_keys(k: bytes, client_nonce: bytes, device_nonce: bytes, node_id: str) -> tuple[bytes, bytes]:
    """enc_key, mac_key = HKDF-SHA256(salt, IKM K, info cn || dn || node_id, 64)."""
    keys = hkdf_sha256(SEAL_SALT, k, client_nonce + device_nonce + node_id.encode("utf-8"), 64)
    return keys[:32], keys[32:]


def _seal_stream(enc: bytes, direction: int, counter: int, n: int) -> bytes:
    out, i = b"", 0
    head = SEAL_STREAM_LABEL + bytes([direction]) + counter.to_bytes(4, "big")
    while len(out) < n:
        out += hmac.new(enc, head + i.to_bytes(4, "big"), hashlib.sha256).digest()
        i += 1
    return out[:n]


def _seal_tag(mac: bytes, direction: int, counter: int, ct: bytes) -> bytes:
    data = SEAL_TAG_LABEL + bytes([direction]) + counter.to_bytes(4, "big") + len(ct).to_bytes(4, "big") + ct
    return hmac.new(mac, data, hashlib.sha256).digest()[:SEAL_TAG_LEN]


def seal(enc: bytes, mac: bytes, direction: int, counter: int, plaintext: bytes) -> bytes:
    """counter (uint32 BE) || ciphertext || tag (16)."""
    ct = bytes(a ^ b for a, b in zip(plaintext, _seal_stream(enc, direction, counter, len(plaintext))))
    return counter.to_bytes(4, "big") + ct + _seal_tag(mac, direction, counter, ct)


def unseal(enc: bytes, mac: bytes, direction: int, counter: int, sealed: bytes) -> bytes | None:
    """The plaintext, or None for a wrong counter or tag."""
    if len(sealed) < 4 + SEAL_TAG_LEN or int.from_bytes(sealed[:4], "big") != counter:
        return None
    ct, t = sealed[4:-SEAL_TAG_LEN], sealed[-SEAL_TAG_LEN:]
    if not hmac.compare_digest(t, _seal_tag(mac, direction, counter, ct)):
        return None
    return bytes(a ^ b for a, b in zip(ct, _seal_stream(enc, direction, counter, len(ct))))


class SealedSession:
    """A sealed session on this connection: its keys, the client's next counter, the flags the gadget echoed and
    the credentials' field names (their values are dropped: a bench needs none of them)."""

    def __init__(self, enc: bytes, mac: bytes, echoed: int, fields: list[str]) -> None:
        self.enc, self.mac, self.echoed, self.fields = enc, mac, echoed, fields
        self.ctr_out = 0   # client -> device, shared with data and credentials_update

    def sealed_frame(self, sub: int, pt: bytes) -> bytes:
        """[session, sub, first|last|sealed, total_len u32 LE, counter || ciphertext || tag], one write; takes the
        next client counter."""
        sealed = seal(self.enc, self.mac, SEAL_CLIENT_TO_DEVICE, self.ctr_out, pt)
        self.ctr_out += 1
        f = P["mg_session_flag_first"] | P["mg_session_flag_last"] | P["mg_session_flag_sealed"]
        return bytes([P["mg_command_session"], sub, f]) + len(sealed).to_bytes(4, "little") + sealed


async def read_sealed_message(g: "Gadget", sub: int, max_total: int, first: bytes | None = None,
                              secs: float = 5.0) -> bytes:
    """One sealed session message from the gadget, reassembled before any tag check: a whole FIRST header first,
    its total at most max_total, no bytes past it, no second FIRST, and one deadline for every fragment."""
    ses = P["mg_command_session"]
    deadline = time.monotonic() + secs
    blob, total, frag = None, 0, first
    while True:
        if frag is None:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("the gadget's sealed message didn't finish in time")
            frag = await g.expect(ses, timeout=left, sub=sub)
        if len(frag) < P["MG_SESSION_DATA_HEADER"]:
            raise RuntimeError("a short sealed fragment")
        fl = frag[2]
        if blob is None:
            if not fl & P["mg_session_flag_first"] or len(frag) < P["MG_SESSION_FIRST_HEADER"]:
                raise RuntimeError("a sealed message that doesn't start with a whole first fragment")
            total = int.from_bytes(frag[3:7], "little")
            if total > max_total:
                raise RuntimeError(f"a sealed message of {total} bytes (at most {max_total})")
            blob = frag[P["MG_SESSION_FIRST_HEADER"]:]
        elif fl & P["mg_session_flag_first"]:
            raise RuntimeError("a second first fragment inside a sealed message")
        else:
            blob += frag[P["MG_SESSION_DATA_HEADER"]:]
        if len(blob) > total:
            raise RuntimeError("a sealed message longer than its total")
        if fl & P["mg_session_flag_last"]:
            if len(blob) != total:
                raise RuntimeError("a sealed message shorter than its total")
            return bytes(blob)
        frag = None


async def sealed_hello(g: "Gadget", k: bytes, node_id: str,
                       max_message: int = 65536, window: int = 65536) -> SealedSession:
    """After a matching run_proof() on this connection: hello with mg_session_hello_flag_sealed, its answer, and
    the credentials unsealed on device counter 0 (checked, then dropped)."""
    nonces = getattr(g, "proof_nonces", None)
    if not nonces:
        raise RuntimeError("no matching token proof on this connection")
    cn, dn = nonces
    ses = P["mg_command_session"]
    flags = P["mg_session_hello_flag_sealed"]
    await g.send(ses, P["mg_session_hello"], P["MG_SESSION_VERSION"], *max_message.to_bytes(4, "little"),
                 *window.to_bytes(4, "little"), flags)
    ans = await g.expect(ses, sub=P["mg_session_hello"])
    if len(ans) < 12 or not ans[11] & P["mg_session_hello_flag_sealed"]:
        raise RuntimeError("the gadget answered the sealed hello unsealed")
    blob = await read_sealed_message(g, P["mg_session_credentials"], P["MG_SESSION_MAX_CREDENTIALS"] + 4 + SEAL_TAG_LEN)
    enc, mac = seal_keys(k, cn, dn, node_id)
    pt = unseal(enc, mac, SEAL_DEVICE_TO_CLIENT, 0, blob)
    if pt is None:
        raise RuntimeError("the sealed credentials didn't open (counter 0, this proof's keys)")
    fields = sorted(json.loads(pt))
    pt = None   # the tokens go no further
    return SealedSession(enc, mac, ans[11], fields)


async def sealed_clear(g: "Gadget", k: bytes, node_id: str, busy_s: float = 5.0) -> bool:
    """After a matching run_proof() on this connection, for a gadget that declares mg_session_cap_sealed_clear: a
    sealed hello, then mg_session_clear [0] sealed on the next client counter. True once the gadget's own sealed
    clear answers [1] (erased); a busy answer is retried on the next counter."""
    s = await sealed_hello(g, k, node_id)
    ses, clr = P["mg_command_session"], P["mg_session_clear"]
    deadline = time.monotonic() + busy_s
    while True:
        await g.client.write_gatt_char(P["MG_CONTROL_UUID"], s.sealed_frame(clr, b"\x00"), response=False)
        try:
            frag = await g.expect(ses, sub=clr)
        except RuntimeError as e:
            if str(e).endswith(": busy") and time.monotonic() < deadline:
                await asyncio.sleep(0.2)
                continue
            raise
        break
    blob = await read_sealed_message(g, clr, 1 + 4 + SEAL_TAG_LEN, first=frag)
    # The device's counter: the credentials took 0, so its answer's is later.
    ctr = int.from_bytes(blob[:4], "big") if len(blob) >= 4 else 0
    pt = unseal(s.enc, s.mac, SEAL_DEVICE_TO_CLIENT, ctr, blob) if ctr >= 1 else None
    if pt is None:
        raise RuntimeError("the gadget's sealed clear answer didn't open")
    return pt == b"\x01"


async def find_setup(args):
    """Gadgets to set up: (address, name, rssi, set_up). One not set up advertises
    the Link setup service, taking turns with the musegadgets UUID; a set-up one
    is found by its musegadgets advertisement and set up again on the same
    service (protocols/README.md, Gadget setup v2)."""
    print(f"scanning {args.scan_secs:.0f} s for gadgets (Muse Link setup or musegadgets) ...")
    hits = []
    for addr, e in (await scan_adverts(args.scan_secs)).items():
        link, mg = SETUP_SERVICE_UUID in e["uuids"], P["MG_SERVICE_UUID"] in e["uuids"]
        if link or mg:
            flags = e.get("link_flags")
            set_up = not link or (flags is not None and bool(flags & LINK_ADV_SET_UP))
            hits.append((addr, e["name"], e["rssi"], set_up))
    if args.address:
        hits = [h for h in hits if h[0].lower() == args.address.lower()]
    elif args.name:
        hits = [h for h in hits if h[1].startswith(args.name)]
    return sorted(hits, key=lambda h: -h[2])


def serial_confirm(port: str | None):
    """Presses the button over the gadget's serial console (tools/mg_confirm.py), or None."""
    if not port:
        return None

    async def confirm() -> bool:
        from mg_confirm import confirm_now

        print(f">>> confirming over {port}")
        return await asyncio.to_thread(confirm_now, port)

    return confirm


TOKENS_HELP = ("the access and refresh tokens to provision, one per line, from a file only you can read (mode 0600) "
               "or `-` for stdin (default: generated bench-access-... and bench-refresh-... tokens); needs "
               "--address, the gadget they go to")


def setup_tokens(args) -> tuple[str, str]:
    """The tokens to provision: generated bench tokens, or --tokens (a 0600 file or stdin), never a command-line
    value (shell history, `ps`)."""
    if not getattr(args, "tokens", None):
        return "bench-access-" + secrets.token_hex(16), "bench-refresh-" + secrets.token_hex(16)
    if args.tokens == "-":
        text = sys.stdin.read()
    else:
        path = Path(args.tokens)
        if path.stat().st_mode & 0o077:
            raise SystemExit(f"FAIL  {path} can be read by others: chmod 600 it first")
        text = path.read_text()
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if len(lines) != 2:
        raise SystemExit("FAIL  --tokens needs two lines: the access token, then the refresh token")
    return lines[0], lines[1]


def tokens_need_address(args) -> None:
    """--tokens are real tokens: they go only to the gadget the user named by --address, never to whichever
    gadget advertises strongest (community pairing has no attestation)."""
    if getattr(args, "tokens", None) and not getattr(args, "address", None):
        raise SystemExit("FAIL  --tokens needs --address: real tokens go only to the gadget you name (`scan` "
                         "lists the addresses), never to the strongest advertiser")


async def cmd_setup(args) -> int:
    from bleak import BleakClient

    tokens_need_address(args)
    try:
        import cryptography  # noqa: F401
    except ImportError:
        print("FAIL  setup needs the cryptography package: pip install -r esp32/tools/requirements-bench.txt")
        return 2
    hits = await find_setup(args)
    if not hits:
        print("FAIL  no gadget advertising Link setup or musegadgets (connected to a phone? or pass --address)")
        return 1
    addr, name, rssi, set_up = hits[0]
    print(f"using {name} {addr} (RSSI {rssi}){', set up already: setting it up again' if set_up else ''}")
    access, refresh = setup_tokens(args)
    async with BleakClient(addr, timeout=20.0) as client:
        link = SetupLink(client)
        await link.start()
        try:
            got = await setup_flow(link, access, refresh, prompt=lambda m: print(f">>> {m}"),
                                   confirm=serial_confirm(args.confirm_serial), set_up=set_up)
        except (RuntimeError, ValueError, asyncio.TimeoutError) as e:
            print(f"FAIL  {printable(str(e) or type(e).__name__)}")
            return 1
    print(f"PASS  set up {printable(got['node_id'])} (wifi {printable(str(got['wifi']))}, mgcommands {'yes' if got['mgcommands'] else 'no'}), "
          f"access {len(access)} B ({token_fingerprint(access.encode())}), refresh {len(refresh)} B")
    print(f"SDK token   {mask_token(got['sdk_token'])}")
    path = save_proof_record(addr, got["node_id"], got["proof_key"], access)
    print(f"proof key saved to {path} (0600)")
    return 0


async def reprovision_flow(link: SetupLink, g: "Gadget", old_k: bytes | None, access: str, refresh: str,
                           prompt=print, confirm=None) -> dict:
    """Sets a set-up gadget up again on one connection (protocols/README.md, Gadget
    setup v2) and checks what that changes: the token proof with the old key
    before (a match), Link setup again (the same handshake, confirmed with a 3 s hold), then the
    proof with the old key (the gadget must no longer hold it: device_mac_mismatch)
    and with the new one (a match). Returns setup_flow's result plus proof_before,
    proof_old, proof_new and ok."""
    before = await run_proof(g, old_k) if old_k else "no_old_key"
    got = await setup_flow(link, access, refresh, prompt=prompt, confirm=confirm, set_up=True)
    got["proof_before"] = before
    got["proof_old"] = await run_proof(g, old_k) if old_k else "no_old_key"
    got["proof_new"] = await run_proof(g, got["proof_key"])
    got["ok"] = got["proof_new"] == "match" and (
        old_k is None or old_k == got["proof_key"] or got["proof_old"] == "device_mac_mismatch")
    return got


async def cmd_reprovision(args) -> int:
    """The re-provisioning test on a set-up gadget: see reprovision_flow."""
    from bleak import BleakClient

    tokens_need_address(args)
    try:
        import cryptography  # noqa: F401
    except ImportError:
        print("FAIL  reprovision needs the cryptography package: pip install -r esp32/tools/requirements-bench.txt")
        return 2
    hits = [h for h in await find_setup(args) if h[3]]
    if not hits:
        print("FAIL  no set-up gadget found (set one up first with `setup`; or pass --address)")
        return 1
    addr, name, rssi, _ = hits[0]
    rec = load_proof_record(addr)
    print(f"using {name} {addr} (RSSI {rssi}); "
          + (f"old proof key for {printable(rec['node_id'])}" if rec else "no old proof key here (only the new one is checked)"))
    access, refresh = setup_tokens(args)
    async with BleakClient(addr, timeout=20.0) as client:
        link = SetupLink(client)
        await link.start()
        g = Gadget(client, verbose=args.verbose)
        await g.start(nus=False)
        try:
            got = await reprovision_flow(link, g, rec["proof_key"] if rec else None, access, refresh,
                                         prompt=lambda m: print(f">>> {m}"),
                                         confirm=serial_confirm(args.confirm_serial))
        except (RuntimeError, ValueError, asyncio.TimeoutError) as e:
            print(f"FAIL  {printable(str(e) or type(e).__name__)}")
            return 1
    print(f"{'PASS' if got['proof_before'] in ('match', 'no_old_key') else 'FAIL'}  "
          f"proof with the old key before: {got['proof_before']}")
    print(f"PASS  set up again: {printable(got['node_id'])}, access {len(access)} B ({token_fingerprint(access.encode())})")
    print(f"{'PASS' if got['ok'] else 'FAIL'}  proof afterwards: old key {got['proof_old']}, "
          f"new key {got['proof_new']}")
    print(f"SDK token   {mask_token(got['sdk_token'])}")
    if got["proof_new"] == "match":
        path = save_proof_record(addr, got["node_id"], got["proof_key"], access)
        print(f"new proof key saved to {path} (0600)")
    return 0 if got["ok"] and got["proof_before"] in ("match", "no_old_key") else 1


async def cmd_proof(args) -> int:
    from bleak import BleakClient

    addr, _ = await pick(args)
    rec = load_proof_record(addr)
    if not rec:
        print(f"FAIL  no proof key for {addr} in {proof_dir()}: run `setup` first")
        return 1
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=args.verbose)
        await g.start(nus=False)
        st = await g.status()
        if P["mg_command_token_proof"] not in st["commands"]:
            print("FAIL  this gadget doesn't list token_proof")
            return 1
        caps = (st["typed"].get(P["mg_command_session"]) or [0])[0]
        sealed = bool(caps & P["mg_session_cap_sealed_clear"])
        try:
            result = await run_proof(g, rec["proof_key"], clear=args.clear and not sealed)
            if result == "match" and args.clear and sealed and not await sealed_clear(g, rec["proof_key"],
                                                                                    rec["node_id"]):
                raise RuntimeError("the gadget's erase failed")
        except (RuntimeError, asyncio.TimeoutError) as e:
            print(f"FAIL  {printable(str(e) or type(e).__name__)}")
            return 1
    ok = result == "match"
    print(f"{'PASS' if ok else 'FAIL'}  token proof for {printable(rec['node_id'])}: {result}")
    if ok and args.clear:
        proof_path(addr).unlink(missing_ok=True)
        print("cleared: the gadget erased its tokens and returns to setup; local key deleted")
    return 0 if ok else 1


async def cmd_state(args) -> int:
    """Sends one assistant_state, then prints the face changes the gadget logs for a few seconds."""
    from bleak import BleakClient

    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client)
        g.face_log = True
        await g.start()
        st = await g.status()
        if P["mg_command_assistant_state"] not in st["commands"]:
            print("FAIL  this gadget doesn't list assistant_state")
            return 1
        await g.send(P["mg_command_assistant_state"], assistant_state(args.state))
        await asyncio.sleep(args.wait)
        errors = [parse_error(p) for p in g.drain() if p[0] == P["mg_command_error"]]
        if errors:
            print("FAIL  " + "; ".join(errors))
            return 1
        if not g.faces:
            print("NOTE  no face change logged (already in that state, or the log mirror is off)")
    return 0


async def cmd_call(args) -> int:
    """The phone's side of a live call: answers the gadget's start and end, and sends states."""
    from bleak import BleakClient

    lc = P["mg_command_live_call"]
    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client)
        g.nus_watch = re.compile(r"mg\.(call|face)|mg\.ble: (live call|video)|muse_video: (view|stream)")
        await g.start()
        st = await g.status()   # a start the gadget has pending comes right after it
        if lc not in st["commands"]:
            print("FAIL  this gadget doesn't list live_call")
            return 1
        print("OK    live_call listed in supported_features")
        # A set-up gadget takes the call's states (its own start's answer too) only on a proven connection.
        result = await prove_connection(g, addr, st)
        if result not in (None, "match"):
            print(f"NOTE  token proof: {result} (a set-up gadget refuses the call on this connection)")
        # Push-to-talk on, as the app has it in a call: a talk press reaches us.
        await g.send(P["mg_command_set_settings"], setting_id("push_to_talk_enabled"), 1)
        t0 = time.monotonic()
        plan: list[tuple[float, int]] = []
        for i, name in enumerate(args.state or []):
            plan.append((t0 + i * args.connect, live_call_state(name)))
        if args.start:   # the phone starts the call itself
            plan += [(t0 + d, s) for d, s in live_call_answer(P["mg_live_call_request_start"], "live", args.connect)]
        requests = [p for p in st["others"] if p[0] == lc and len(p) > 1]
        seen = 0
        deadline = t0 + args.secs if args.secs else None
        while deadline is None or time.monotonic() < deadline:
            for p in requests:
                seen += 1
                print(f"      gadget asks: {live_call_name(p[1], False)}")
                now = time.monotonic()
                plan += [(now + d, s) for d, s in live_call_answer(p[1], args.answer, args.connect)]
            requests = []
            now = time.monotonic()
            plan.sort()
            while plan and plan[0][0] <= now:
                _, s = plan.pop(0)
                await g.send(lc, s)
                if s == live_call_state("live") and args.end_after:
                    plan.append((now + args.end_after, live_call_state("ended")))
            try:
                p = await asyncio.wait_for(g.control.get(), 0.1)
            except asyncio.TimeoutError:
                continue
            if p[0] == lc and len(p) > 1:
                requests.append(p)
            elif p[0] == P["mg_command_error"]:
                g.control.put_nowait(p)   # reported below
                break
        errors = [parse_error(p) for p in g.drain() if p[0] == P["mg_command_error"]]
        if errors:
            print("FAIL  " + "; ".join(errors))
            return 1
        print(f"done: {seen} request(s) from the gadget")
    return 0


def clip_path(out_dir: str, run: str, i: int) -> Path:
    """Where queue saves clip i of this run: clip_<run>_<i>.wav (beside its .sbc or .lc3), never over a file
    already there. The gadget numbers each batch's clips from 0 again, so a second batch keeps the first."""
    n = 1
    while True:
        out = Path(out_dir) / (f"clip_{run}_{i}" + (f"_{n}" if n > 1 else "") + ".wav")
        if not any(out.with_suffix(x).exists() for x in (".wav", ".sbc", ".lc3")):
            return out
        n += 1


def queue_unchanged(before: bytes, now: bytes) -> bool:
    """Two audio_queue status answers name the same clips: the same count and the same bytes used."""
    return len(before) >= 13 and len(now) >= 13 and before[3:7] == now[3:7] and before[11:13] == now[11:13]


async def cmd_queue(args) -> int:
    from bleak import BleakClient

    addr, _ = await pick(args)
    dec = Decoder()
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=False)
        await g.start(nus=False)
        # A set-up gadget answers the queue (its status and clip_info too) only on a proven connection.
        result = await prove_connection(g, addr, await g.status())
        if result not in (None, "match"):
            print(f"NOTE  token proof: {result} (a set-up gadget refuses the queue on this connection)")
        q = P["mg_command_audio_queue"]
        await g.send(q, P["mg_audio_queue_command_status"])
        try:
            st = await g.expect(q, sub=P["mg_audio_queue_command_status"])
        except RuntimeError as e:
            print(f"FAIL  queue status: {e}")
            return 1
        count = int.from_bytes(st[11:13], "little")
        failed: list[int] = []
        run = time.strftime("%Y%m%d-%H%M%S")
        print(f"queue: enabled={st[2]} {count} clips, {int.from_bytes(st[3:7], 'little')} of "
              f"{int.from_bytes(st[7:11], 'little')} bytes used")
        for i in range(count):
            await g.send(q, P["mg_audio_queue_command_clip_info"], i & 0xFF, i >> 8)
            info = parse_clip_info(await g.expect(q, sub=P["mg_audio_queue_command_clip_info"]))
            codec, start, size, fmt = info["codec"], info["start_ms"], info["size"], info["format"]
            print(f"  clip {i}: {data_type_name(codec)} {fmt['rate']} Hz"
                  + (f" {fmt['frame_bytes']} B frames" if fmt["frame_bytes"] else "")
                  + f", {size} bytes, started at uptime {start / 1000:.1f} s")
            if not args.download:
                continue
            g.data.clear()
            t0 = time.monotonic()
            await g.send(q, P["mg_audio_queue_command_read_clip"], i & 0xFF, i >> 8)
            await g.expect(P["mg_command_change_data_type"])
            t1 = time.monotonic()
            deadline = t1 + 30
            while sum(map(len, g.data)) < size and time.monotonic() < deadline:
                await asyncio.sleep(0.01)
            t2 = time.monotonic()
            blob = b"".join(g.data)[:size]
            print(f"    downloaded in {t2 - t0:.2f} s ({t1 - t0:.2f} s to start): "
                  f"{len(blob) / max(t2 - t1, 1e-3) / 1024:.1f} KB/s, {len(g.data)} notifications")
            out = clip_path(args.out_dir, run, i)
            try:
                out.parent.mkdir(parents=True, exist_ok=True)
                # The clip as the gadget keeps it, beside its decoded WAV.
                out.with_suffix(".sbc" if codec == P["mg_data_type_audio_sbc"] else ".lc3").write_bytes(blob)
                if codec == P["mg_data_type_audio_sbc"]:
                    pcm, _ = dec.decode(fmt, [blob])
                else:
                    pcm, _ = dec.decode(fmt, [blob[:len(blob) // fmt["frame_bytes"] * fmt["frame_bytes"]]])
                write_wav(out, pcm, fmt["rate"])
                print(f"    {len(blob)} of {size} bytes, {len(pcm) / fmt['rate']:.2f} s -> {out}")
                if len(blob) < size:
                    failed.append(i)
                    print(f"FAIL  clip {i}: only {len(blob)} of {size} bytes arrived")
            except Exception as e:
                failed.append(i)
                print(f"FAIL  clip {i}: {len(blob)} of {size} bytes, not saved: {e}")
        if args.clear and failed:
            # Clearing would lose the clips that didn't come down whole.
            print(f"FAIL  not clearing: clip{'s' if len(failed) > 1 else ''} {', '.join(map(str, failed))} "
                  "didn't download whole; the queue is kept")
            return 1
        if args.clear:
            # clear erases every clip: only the ones listed above, so not if the gadget recorded more since.
            await g.send(q, P["mg_audio_queue_command_status"])
            now = await g.expect(q, sub=P["mg_audio_queue_command_status"])
            if not queue_unchanged(st, now):
                print(f"FAIL  not clearing: the queue changed since it was listed ({count} clips then, "
                      f"{int.from_bytes(now[11:13], 'little')} now); list it again")
                return 1
            await g.send(q, P["mg_audio_queue_command_clear"])
            st = await g.expect(q, sub=P["mg_audio_queue_command_status"])
            print(f"cleared: {int.from_bytes(st[11:13], 'little')} clips left")
    return 1 if failed else 0


# ---- throughput test (device_action throughput_test) ----

def att_payload(mtu: int) -> int:
    """The most one write or notification carries: MTU - 3, but never over an ATT value's 512 bytes."""
    return min(mtu - 3, P["MG_MAX_ATT_VALUE"])


def tp_fill(n: int, seq: int) -> bytes:
    """A throughput test packet, as the firmware makes them: the sequence, then (seq + i) & 0xFF."""
    return seq.to_bytes(4, "little") + bytes((seq + i) & 0xFF for i in range(4, n))


class TpCounter:
    """Counts a throughput test stream: bytes, packets, sequence gaps, corrupt packets, and
    arrival bursts (notifications that land together, roughly one connection event each)."""

    def __init__(self, burst_gap_s: float = 0.004):
        self.bytes = self.packets = self.gaps = self.bad = 0
        self.next_seq = 0
        self.first = self.last = 0.0
        self.burst_gap_s = burst_gap_s
        self.bursts: list[int] = []

    def add(self, payload: bytes, t: float) -> None:
        if len(payload) < 4:
            return
        seq = int.from_bytes(payload[:4], "little")
        if self.packets == 0:
            self.first = t
            self.next_seq = seq
        if seq > self.next_seq:
            self.gaps += seq - self.next_seq
        if seq >= self.next_seq:
            self.next_seq = seq + 1
        if payload != tp_fill(len(payload), seq):
            self.bad += 1
        if not self.bursts or t - self.last > self.burst_gap_s:
            self.bursts.append(0)
        self.bursts[-1] += 1
        self.bytes += len(payload)
        self.packets += 1
        self.last = t

    def rate(self) -> float:
        return self.bytes / max(self.last - self.first, 1e-3)

    def per_burst(self) -> str:
        if not self.bursts:
            return "-"
        return f"{self.packets / len(self.bursts):.1f} on average, at most {max(self.bursts)}"


def parse_tp_report(payload: bytes) -> dict:
    """[device_action, throughput_test, 0, bytes, packets, ms, gaps]."""
    v = [int.from_bytes(payload[i:i + 4], "little") for i in (3, 7, 11, 15)]
    return {"bytes": v[0], "packets": v[1], "ms": v[2], "gaps": v[3]}


async def cmd_bench(args) -> int:
    """Raw link throughput both ways with device_action throughput_test."""
    from bleak import BleakClient

    r = Results()
    da, tt = P["mg_command_device_action"], P["mg_device_action_throughput_test"]
    send_s, recv_s = args.send, args.recv
    if send_s is None and recv_s is None:
        send_s = recv_s = 10
    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=False)
        await g.start(nus=False)
        st = await g.status()
        if da not in st["commands"]:
            print("FAIL  this gadget doesn't list device_action (older firmware)")
            return 1
        mtu = await att_mtu(client) or client.mtu_size
        payload = att_payload(mtu)
        print(f"ATT MTU {mtu}: {payload} B packets")

        if send_s:
            c = TpCounter()
            g.data_hook = lambda d: c.add(d, time.monotonic())
            await g.send(da, tt, P["mg_throughput_test_send"], send_s & 0xFF, send_s >> 8)
            cdt = await g.expect(P["mg_command_change_data_type"])
            r.check("send: change_data_type throughput_test", cdt[1] == P["mg_data_type_throughput_test"])
            rep = parse_tp_report(await g.expect(da, timeout=send_s + 10, sub=tt))
            await asyncio.sleep(0.3)   # the last notifications
            g.data_hook = None
            dev_rate = rep["bytes"] / max(rep["ms"], 1) * 1000
            print(f"send (device -> {sys.platform}): device sent {rep['packets']} packets, {rep['bytes']} B in "
                  f"{rep['ms']} ms = {dev_rate / 1024:.1f} KB/s")
            print(f"  received {c.packets} packets, {c.bytes} B in {c.last - c.first:.2f} s = "
                  f"{c.rate() / 1024:.1f} KB/s; packets per burst (about one connection event): "
                  f"{c.per_burst()}")
            r.check("send: everything arrived", c.packets == rep["packets"] and c.gaps == 0 and c.bad == 0,
                    f"{c.gaps} missing, {c.bad} corrupt")

        if recv_s:
            await g.send(da, tt, P["mg_throughput_test_receive"], 0, 0)   # until stop
            await g.send(P["mg_command_change_data_type"], P["mg_data_type_throughput_test"])
            # The gadget turns its peripheral latency off for the test; at latency 10 the
            # update takes about a second to apply.
            await asyncio.sleep(1.5)
            t0 = time.monotonic()
            seq = 0
            while time.monotonic() - t0 < recv_s:
                await wait_write_room(client)
                await client.write_gatt_char(P["MG_DATA_UUID"], tp_fill(payload, seq), response=False)
                seq += 1
            wrote_s = time.monotonic() - t0
            await asyncio.sleep(0.5)
            await g.send(da, tt, P["mg_throughput_test_stop"])
            rep = parse_tp_report(await g.expect(da, timeout=5, sub=tt))
            dev_rate = rep["bytes"] / max(rep["ms"], 1) * 1000
            print(f"receive ({sys.platform} -> device): wrote {seq} packets, {seq * payload} B in {wrote_s:.2f} s = "
                  f"{seq * payload / wrote_s / 1024:.1f} KB/s")
            print(f"  device got {rep['packets']} packets, {rep['bytes']} B in {rep['ms']} ms = "
                  f"{dev_rate / 1024:.1f} KB/s, {rep['gaps']} missing")
            r.check("receive: everything arrived", rep["packets"] == seq and rep["gaps"] == 0,
                    f"{seq - rep['packets']} lost")
    return 1 if r.failed else 0


# ---- firmware update over MCUmgr SMP (images, dfu) ----

MCUBOOT_IMAGE_MAGIC = 0x96F3B83D
MCUBOOT_TLV_INFO_MAGIC = 0x6907
MCUBOOT_TLV_PROT_INFO_MAGIC = 0x6908
MCUBOOT_TLV_SHA256 = 0x10


def mcuboot_image_info(data: bytes) -> dict:
    """Version and SHA-256 (the hash SMP names images by) of an MCUboot-signed image."""
    if len(data) < 32:
        raise ValueError("too short for an MCUboot image")
    magic, _load, hdr_size, prot_size, img_size, _flags = struct.unpack_from("<IIHHII", data, 0)
    if magic != MCUBOOT_IMAGE_MAGIC:
        raise ValueError("not an MCUboot image (sign it: use zephyr.signed.bin)")
    major, minor, rev, build = struct.unpack_from("<BBHI", data, 20)
    off = hdr_size + img_size
    if prot_size:
        tlv_magic, tot = struct.unpack_from("<HH", data, off)
        if tlv_magic != MCUBOOT_TLV_PROT_INFO_MAGIC:
            raise ValueError("bad protected TLV area")
        off += tot
    tlv_magic, tot = struct.unpack_from("<HH", data, off)
    if tlv_magic != MCUBOOT_TLV_INFO_MAGIC:
        raise ValueError("no TLV area after the image")
    end, p, sha = off + tot, off + 4, None
    while p + 4 <= end:
        t, n = struct.unpack_from("<HH", data, p)
        if t == MCUBOOT_TLV_SHA256:
            sha = bytes(data[p + 4:p + 4 + n])
        p += 4 + n
    if sha is None:
        raise ValueError("no SHA-256 TLV")
    return {"version": f"{major}.{minor}.{rev}" + (f"+{build}" if build else ""), "sha256": sha,
            "size": len(data)}


def dfu_state_requests(sha: bytes, mode: str) -> list:
    """The SMP requests that follow an upload: mark the image to test or confirm it."""
    from smpclient.requests.image_management import ImageStatesWrite

    if mode == "test":
        return [ImageStatesWrite(hash=sha, confirm=False)]
    if mode == "confirm":
        return [ImageStatesWrite(hash=sha, confirm=True)]
    return []


def format_image_states(images) -> list[str]:
    lines = []
    for im in images:
        flags = [n for n in ("active", "confirmed", "pending", "permanent", "bootable") if getattr(im, n, False)]
        h = bytes(im.hash).hex() if getattr(im, "hash", None) else "-"
        lines.append(f"slot {im.slot}: {printable(str(im.version)):<12} {h[:16]}…  {' '.join(flags) or '-'}")
    return lines


def smp_rc(rsp):
    """An SMP response's rc (an SMP v1 error's, or a v2 error's group rc), or None."""
    rc = getattr(rsp, "rc", None)
    err = getattr(rsp, "err", None)
    if rc is None and err is not None:
        rc = getattr(err, "rc", None)
    return rc


SMP_RC_PRESS = 10    # the update waits for a press of the gadget's own button
SMP_RC_DENIED = 11   # not confirmed: no press in time, cancelled, or refused


def smp_refusal_hint(rsp) -> str:
    """What a refused update write means: rc 11 (access denied) is the gadget's update gate."""
    if smp_rc(rsp) != SMP_RC_DENIED:
        return ""
    return ("  (access denied: the update wasn't confirmed on the gadget: no press in time, cancelled, or a SAFE "
            "mode the gadget entered itself on a board without a display. An update works on any gadget, set up or "
            "not: on a connection without the token proof, after one press of the gadget's button. Otherwise "
            "update over USB or SWD)")


async def smp_request_pressed(client, req, what: str, timeout_s: float = 10.0, deadline_s: float = 35.0,
                              interval_s: float = 1.0):
    """client.request(req), held through the gadget's update prompt: on rc 10 the connection stays up, the user is
    asked to press the gadget's button, and the same request goes again about every interval_s until it's
    admitted. rc 11 (no press in time, or cancelled) or the deadline stops dfu (SystemExit). Any other answer is
    returned as is."""
    rsp = await client.request(req, timeout_s=timeout_s)
    if smp_rc(rsp) != SMP_RC_PRESS:
        return rsp
    print("press the gadget's button to confirm the update (30 s)")
    t0 = time.monotonic()
    while smp_rc(rsp) == SMP_RC_PRESS:
        if time.monotonic() - t0 >= deadline_s:
            raise SystemExit(f"FAIL  {what}: not confirmed: no press in time; run dfu again")
        await asyncio.sleep(interval_s)
        rsp = await client.request(req, timeout_s=timeout_s)
    if smp_rc(rsp) == SMP_RC_DENIED:
        raise SystemExit(f"FAIL  {what}: not confirmed: no press in time or cancelled; run dfu again")
    return rsp


def smp_ok(rsp) -> bool:
    from smpclient.generics import success

    return success(rsp)


async def ble_open(address: str, timeout_s: float, disconnected_callback=None, **kwargs):
    """A connected BleakClient to the gadget at address, with every GATT service discovered.

    No services filter: `dfu` runs the token proof on MG Control and the update on SMP, two services, over this
    one connection (a set-up gadget binds SMP writes to the connection the proof matched on)."""
    from bleak import BleakClient, BleakScanner

    device = await BleakScanner.find_device_by_address(address, timeout=timeout_s)
    if device is None:
        raise RuntimeError(f"gadget {address} not found")
    client = BleakClient(device, timeout=timeout_s, disconnected_callback=disconnected_callback, **kwargs)
    await client.connect()
    return client


def smp_transport_class():
    """smpclient's BLE transport, on a connection that also reaches MG Control, made safe for bursts and pipelining.

    - _connect() opens the connection with every service (ble_open): smpclient's own opens it filtered to the
      SMP service, where MG Control, and so the token proof, can't be reached. The rest is smpclient 7.4.0's
      (requirements-bench.txt pins it): the SMP characteristic, the write size, the notifications;
    - send() waits for room before each Write Without Response
      (wait_write_room: CoreBluetooth silently drops writes it has no room
      for, and one lost write ruins a reassembled SMP request);
    - receive() takes one response off the front of the buffer, so two
      responses that arrive back to back (a window of requests) don't break it;
    - max_unencoded_size can be capped (--chunk).
    """
    from smp import header as smphdr
    from smpclient.transport import SMP_CHARACTERISTIC_UUID
    from smpclient.transport.ble import SMPBLETransport, SMPBLETransportNotSMPServer

    class MgSMPBLETransport(SMPBLETransport):
        chunk_cap: int | None = None

        async def _connect(self, address: str, timeout_s: float) -> None:
            self._client = await ble_open(address, timeout_s, self._set_disconnected_event, winrt=self._winrt)
            self._disconnected_event.clear()
            smp_characteristic = self._client.services.get_characteristic(SMP_CHARACTERISTIC_UUID)
            if smp_characteristic is None:
                raise SMPBLETransportNotSMPServer("Missing the SMP characteristic UUID.")
            self._max_write_without_response_size = smp_characteristic.max_write_without_response_size
            if self._winrt_backend(self._client._backend) and self._max_write_without_response_size == 20:
                # As smpclient: Windows can report 20 until the session settles.
                await asyncio.sleep(2)
                self._max_write_without_response_size = self._client._backend._session.max_pdu_size - 3
            elif self._bluez_backend(self._client._backend):
                # BlueZ reports the default MTU (23) until it's acquired.
                await self._client._backend._acquire_mtu()
                self._max_write_without_response_size = self._client.mtu_size - 3
            self._smp_characteristic = smp_characteristic
            await self._await_or_disconnect(self._client.start_notify(SMP_CHARACTERISTIC_UUID, self._notify_callback))

        async def send(self, data: bytes) -> None:
            for off in range(0, len(data), self.mtu):
                await wait_write_room(self._client)
                await self._client.write_gatt_char(self._smp_characteristic, data[off:off + self.mtu],
                                                   response=False)

        async def receive(self) -> bytes:
            async with self._notify_condition:
                while True:
                    if len(self._buffer) >= smphdr.Header.SIZE:
                        h = smphdr.Header.loads(bytes(self._buffer[:smphdr.Header.SIZE]))
                        n = h.length + smphdr.Header.SIZE
                        if len(self._buffer) >= n:
                            out = bytes(self._buffer[:n])
                            del self._buffer[:n]
                            return out
                    await self._notify_or_disconnect()

        @property
        def max_unencoded_size(self) -> int:
            n = super().max_unencoded_size
            return min(n, self.chunk_cap) if self.chunk_cap else n

    return MgSMPBLETransport


async def smp_client(args, addr: str | None = None):
    from smpclient import SMPClient

    if addr is None:
        addr, _ = await pick(args)
    t = smp_transport_class()()
    t.chunk_cap = getattr(args, "chunk", None)
    return SMPClient(t, addr, timeout_s=10.0)


async def smp_prove(client, addr: str, unproven: bool = False) -> str:
    """The token proof on the SMP client's own BLE connection, before its first write: a set-up gadget takes
    update writes (upload, image state, erase, reset) only on a proven connection, and an image can carry the
    SDK token, so it goes only to the gadget that holds the key `setup` saved for this address. Anything but a
    match stops the update (SystemExit) before any write. With --unproven (cmd_dfu has checked the address was
    given and the image holds no token) no proof is run: the gadget asks for a press of its button instead."""
    if unproven:
        print("NOTE  --unproven: no token proof; the gadget asks for a press of its button to confirm the update")
        return "unproven"
    rec = load_proof_record(addr)
    if rec is None:
        raise SystemExit("FAIL  no saved proof record for this gadget: run `setup` first, or pass --address and "
                         "--unproven to confirm the update with a press of the gadget's button")
    g = Gadget(client._transport._client, verbose=False)
    await g.client.start_notify(P["MG_CONTROL_UUID"], g._on_control)
    result = await run_proof(g, rec["proof_key"])
    print(f"token proof: {result}")
    if result == "not_found":
        raise SystemExit("FAIL  token proof: not_found: the gadget was reset (or isn't the one set up): "
                         "run setup, or pass --unproven")
    if result != "match":
        raise SystemExit(f"FAIL  token proof: {result}: nothing is sent to this gadget")
    return result


async def smp_list(client) -> list:
    from smpclient.requests.image_management import ImageStatesRead

    rsp = await client.request(ImageStatesRead())
    if not smp_ok(rsp):
        raise SystemExit(f"FAIL  image list: {rsp}")
    return list(rsp.images)


async def smp_params(client) -> tuple[int, int] | None:
    """The server's SMP buffer size and count (OS group MCUmgr parameters), or None."""
    from smpclient.requests.os_management import MCUMgrParametersRead

    try:
        rsp = await client.request(MCUMgrParametersRead())
    except Exception:
        return None
    return (rsp.buf_size, rsp.buf_count) if smp_ok(rsp) else None


def smp_decode(req, frame: bytes):
    """A response frame for req, as SMPClient.request decodes it."""
    for kind in (req._Response, req._ErrorV1, req._ErrorV2):
        try:
            return kind.loads(frame)
        except Exception:
            continue
    raise ValueError(f"undecodable SMP response: {frame[:16].hex()}")


async def smp_upload(client, image: bytes, window: int = 1, progress=None, timeout_s: float = 10.0) -> int:
    """Uploads image to the second slot with up to window requests in flight.

    The first request (offset 0, length and SHA-256) goes alone: the server
    prepares the slot before answering. After it, requests are pipelined:
    Zephyr's SMP transport queues them and handles them in order, so each
    response's offset says where the next one should have started; on a
    mismatch (a request lost or refused) the outstanding ones are drained and
    the upload resumes from the server's offset. Returns the number of
    requests sent.
    """
    from collections import deque
    from smpclient.requests.image_management import ImageUploadWrite

    tr = client._transport
    first = client._maximize_upload_packet(
        ImageUploadWrite(off=0, data=b"", image=0, len=len(image), sha=hashlib.sha256(image).digest(),
                         upgrade=False), image)
    rsp = await smp_request_pressed(client, first, "upload", timeout_s=40.0)
    if not smp_ok(rsp) or rsp.off is None:
        raise RuntimeError(f"upload refused: {rsp}{smp_refusal_hint(rsp)}")
    acked = next_off = rsp.off
    sent = 1
    if progress:
        progress(acked)
    pending: deque = deque()
    while acked < len(image):
        while len(pending) < max(1, window) and next_off < len(image):
            req = client._maximize_upload_packet(ImageUploadWrite(off=next_off, data=b""), image)
            await tr.send(req.BYTES)
            pending.append(req)
            next_off += len(req.data)
            sent += 1
        req = pending.popleft()
        frame = await asyncio.wait_for(tr.receive(), timeout_s)
        rsp = smp_decode(req, frame)
        if not smp_ok(rsp) or rsp.off is None:
            raise RuntimeError(f"upload failed at offset {req.off}: {rsp}")
        acked = rsp.off
        if acked != req.off + len(req.data):
            # The server is somewhere else: forget what's in flight, resume there.
            for _ in range(len(pending)):
                try:
                    await asyncio.wait_for(tr.receive(), 2.0)
                except Exception:
                    break
            pending.clear()
            next_off = acked
        if progress:
            progress(acked)
    return sent


async def cmd_images(args) -> int:
    async with await smp_client(args) as client:
        for line in format_image_states(await smp_list(client)):
            print(line)
    return 0


async def smp_reconnect(args, addr: str, deadline_s: float = 90.0):
    """Connects again once the gadget is back (after a reset and MCUboot's swap)."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline_s:
        client = await smp_client(args, addr)
        try:
            await client.connect(10.0)
            return client
        except Exception:
            await asyncio.sleep(1.0)
    raise SystemExit("FAIL  the gadget didn't come back after the reset")


# The Zephyr image's SDK token slot (CONFIG_MG_SDK_TOKEN_SLOT, zephyr/AGENTS.md): 128 bytes, blank as this
# marker and NULs; an updater writes the owner's token and NULs over all of it.
SDK_TOKEN_SLOT_MARKER = b"MG-SDK-TOKEN-SLOT-v1:"
SDK_TOKEN_SLOT_LEN = 128
# An SDK token as identity_sdk_token_valid() (esp32 main/identity.c) takes one: "mgst_", then base64url of 32
# bytes, whose last character carries only four bits. The firmware's own "mgst_" strings don't match.
SDK_TOKEN_RE = re.compile(rb"mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]")


def image_token_findings(data: bytes) -> list[str]:
    """Why the image looks like it holds an SDK token; [] when nothing was found. Best effort: it finds a token
    stored as is (mgst_...) anywhere and, in an MCUboot image's payload, a token slot that isn't blank (the
    marker with anything but NULs after it). A token stored compressed, encrypted or any other way isn't found."""
    found = []
    if SDK_TOKEN_RE.search(data):
        found.append("it holds an SDK token (mgst_...)")
    if len(data) >= 32 and struct.unpack_from("<I", data, 0)[0] == MCUBOOT_IMAGE_MAGIC:
        hdr_size, img_size = struct.unpack_from("<H", data, 8)[0], struct.unpack_from("<I", data, 12)[0]
        payload = data[hdr_size:hdr_size + img_size]
        blank = SDK_TOKEN_SLOT_MARKER + b"\0" * (SDK_TOKEN_SLOT_LEN - len(SDK_TOKEN_SLOT_MARKER))
        at = payload.find(SDK_TOKEN_SLOT_MARKER)
        while at >= 0:
            if payload[at:at + SDK_TOKEN_SLOT_LEN] != blank:
                found.append(f"its SDK token slot (at payload offset 0x{at:x}) isn't blank")
                break
            at = payload.find(SDK_TOKEN_SLOT_MARKER, at + 1)
    return found


def unproven_dfu_check(args, data: bytes) -> None:
    """--unproven: no proof says which gadget this is, so only one named by address, and never an image with an
    SDK token in it (--allow-token-image doesn't change that)."""
    if not args.address:
        raise SystemExit("FAIL  --unproven needs --address (the gadget to update)")
    if image_token_findings(data):
        raise SystemExit("FAIL  the image holds an SDK token: it goes only to a proven gadget (drop --unproven)")


def dfu_image_check(args, data: bytes) -> None:
    """Before dfu connects: the image goes over BLE unencrypted, so one that holds an SDK token is refused in
    every mode unless --allow-token-image says to send it anyway (never with --unproven)."""
    if args.unproven:
        unproven_dfu_check(args, data)
        return
    found = image_token_findings(data)
    if found and not getattr(args, "allow_token_image", False):
        raise SystemExit(f"FAIL  {args.image}: {'; '.join(found)}. dfu sends the image unencrypted over BLE, so "
                         "anyone in range can record the token: build the image without it, or pass "
                         "--allow-token-image to send it anyway")
    if found:
        print(f"NOTE  --allow-token-image: {'; '.join(found)}; it goes over BLE unencrypted")


async def cmd_dfu(args) -> int:
    from smpclient.requests.os_management import ResetWrite

    r = Results()
    data = Path(args.image).read_bytes()
    info = mcuboot_image_info(data)
    print(f"image {args.image}: version {info['version']}, {info['size']} bytes, sha256 {info['sha256'].hex()[:16]}…")
    mode = "test" if args.test else "confirm" if args.confirm else None
    dfu_image_check(args, data)
    addr, _ = await pick(args)
    t_connect = time.monotonic()
    client = await smp_client(args, addr)
    await client.connect(20.0)
    try:
        print(f"connected in {time.monotonic() - t_connect:.1f} s, ATT write {client._transport.mtu} B")
        await smp_prove(client, addr, args.unproven)
        params = await smp_params(client)
        window = args.window
        if params:
            buf_size, buf_count = params
            # One buffer holds the response, one the request being reassembled.
            auto = max(1, min(2, buf_count - 2))
            window = window or auto
            print(f"SMP server: {buf_size} B x {buf_count} buffers; requests of up to "
                  f"{client._transport.max_unencoded_size} B, window {window}")
        else:
            window = window or 1
            print(f"SMP server: no MCUmgr parameters (older firmware): one ATT write per request, window {window}")
        before = await smp_list(client)
        for line in format_image_states(before):
            print("before  " + line)
        print("NOTE  uploading drops the gadget's offline clips (download them first with `queue --download`)")
        t0, last = time.monotonic(), [-1]

        def progress(off: int) -> None:
            pct = off * 100 // len(data)
            if pct // 10 != last[0]:
                last[0] = pct // 10
                el = time.monotonic() - t0
                print(f"  {pct:3d}%  {off} of {len(data)} bytes, {off / max(el, 1e-3) / 1024:.1f} KB/s")

        sent = await smp_upload(client, data, window, progress)
        secs = time.monotonic() - t0
        r.check("upload", True, f"{len(data)} bytes in {secs:.1f} s = {len(data) / secs / 1024:.1f} KB/s "
                f"({sent} requests, {len(data) / max(sent, 1):.0f} B each, window {window})")
        after = await smp_list(client)
        r.check("image in the second slot",
                any(im.slot == 1 and bytes(im.hash) == info["sha256"] for im in after),
                f"version {info['version']}")
        for req in dfu_state_requests(info["sha256"], mode) if not r.failed else []:   # never mark another image
            rsp = await smp_request_pressed(client, req, f"mark for {mode}")
            r.check(f"mark for {mode}", smp_ok(rsp), "" if smp_ok(rsp) else str(rsp))
        for line in format_image_states(await smp_list(client)):
            print("after   " + line)
        if r.failed and args.reset:
            print("FAIL  not resetting: the image isn't in the second slot as sent, or wasn't marked")
            return 1
        if not args.reset:
            if mode:
                print("NOTE  installs at the next reset (`dfu ... --reset`, or reset the board)")
            return 1 if r.failed else 0
        rsp = await client.request(ResetWrite())
        r.check("reset", smp_ok(rsp), "the gadget restarts" + (" into the new image" if mode else ""))
        t_reset = time.monotonic()
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
    if not mode:
        return 1 if r.failed else 0
    # Reset and MCUboot's swap. Then, as the Muse apps do, the running image is
    # checked and confirmed if it isn't: the gadget holds that confirm until its
    # boot is healthy (30 s up and advertising), when it keeps the image anyway.
    await asyncio.sleep(2.0)
    client = await smp_reconnect(args, addr)
    t_back = time.monotonic()
    r.check("back after reset and swap", True, f"{t_back - t_reset:.1f} s")
    try:
        await smp_prove(client, addr, args.unproven)  # a new connection: the confirm is a write
        from smpclient.requests.image_management import ImageStatesWrite

        confirmed = asked = False
        while time.monotonic() - t_back < 60.0:
            imgs = await smp_list(client)
            run = next((im for im in imgs if im.slot == 0), None)
            if run and bytes(run.hash) == info["sha256"] and (run.confirmed or mode == "confirm"):
                confirmed = True
                break
            if run and bytes(run.hash) == info["sha256"] and not asked:
                rsp = await smp_request_pressed(client, ImageStatesWrite(confirm=True), "confirm")
                r.check("confirm sent", smp_ok(rsp), "held by the gadget until its boot is healthy"
                        if smp_ok(rsp) else str(rsp))
                asked = True
            await asyncio.sleep(1.0)
        r.check("running and confirmed", confirmed,
                f"{time.monotonic() - t_back:.1f} s after reconnecting" if confirmed
                else "the new image isn't running confirmed (reverted, or not confirmed yet)")
        for line in format_image_states(await smp_list(client)):
            print("now     " + line)
    finally:
        await client.disconnect()
    print(f"phases: upload {secs:.1f} s, reset and swap {t_back - t_reset:.1f} s, total "
          f"{time.monotonic() - t0:.1f} s")
    return 1 if r.failed else 0


def ui_ladder(n: int, interval: float) -> list[float]:
    """When each frame of a test stream goes, from the first: the change, its refinements at 0.3 s and 1 s
    (protocols/UI.md), then the rest `interval` apart."""
    times = [0.0, 0.3, 1.0][:n]
    while len(times) < n:
        times.append(times[-1] + interval)
    return times


async def cmd_ui(args) -> int:
    """Streams a UI stream (content=ui) from a test stream file and reports each frame's numbers."""
    from bleak import BleakClient

    frames, bands = load_ui_frames(args.file)
    if args.bands:
        bands = args.bands
    expect_md5 = {}
    if args.index:
        idx = json.loads(Path(args.index).read_text())
        key = Path(args.file).name.split(".")[0]
        for target, streams in idx.items() if isinstance(idx, dict) else []:
            if isinstance(streams, dict) and key in streams and (bands == 2) == target.endswith("2band"):
                entry = streams[key]
                for k, f in enumerate(entry if isinstance(entry, list) else entry.get("frames", [])):
                    if "md5" in f:
                        expect_md5[k] = f["md5"]
    sv = P["mg_command_stream_video"]
    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=args.verbose)
        await g.start()
        st = await g.status()
        if sv not in st["commands"]:
            print("FAIL  this gadget doesn't list stream_video")
            return 1
        # A set-up gadget takes a stream only on a proven connection: this one proves first when a key is saved.
        result = await prove_connection(g, addr, st)
        if result not in (None, "match"):
            print(f"NOTE  token proof: {result} (a set-up gadget refuses the stream on this connection)")
        await g.send(sv, P["mg_stream_video_request_capabilities"])
        caps = parse_video_caps(await g.expect(sv, sub=P["mg_stream_video_report_capabilities"]))
        ui = caps.get("ui")
        print(f"capabilities: {caps}")
        if not ui:
            print("FAIL  no mg_stream_video_capability_ui: this gadget takes no UI streams")
            return 1
        width, height = args.width or ui["w"], args.height or ui["h"]
        mtu = await att_mtu(client) or client.mtu_size
        chunk = min(512, mtu - 3)
        if args.md5:
            await client.write_gatt_char(P["MG_NUS_RX_UUID"], b"ui.md5=1\n", response=True)
        if args.latency is not None:
            await client.write_gatt_char(P["MG_NUS_RX_UUID"], f"ui.lat={args.latency}\n".encode(), response=True)
        nus_from = len(g.nus)
        # busy: the gadget's screen was asleep (it wakes now) or its panel isn't up yet: ask again.
        for attempt in range(8):
            g.vbuf_event.clear()
            g.video_errors.clear()
            await client.write_gatt_char(P["MG_CONTROL_UUID"],
                                         video_start_payload(width, height, args.fps, args.kbps, bands, ui=True),
                                         response=False)
            try:
                await asyncio.wait_for(g.vbuf_event.wait(), 1.0)
                break
            except asyncio.TimeoutError:
                if not any("busy" in e for e in g.video_errors):
                    break
        if not g.vbuf_event.is_set():
            print("FAIL  start_streaming: " + ("; ".join(g.video_errors) or "no buffer_update"))
            return 1
        print(f"UI stream {width}x{height}, {bands} band(s), {len(frames)} frames, MTU {mtu} "
              f"(writes of {chunk} B), device buffer {g.vbuf['avail']} B")
        n_codec = len(frames)
        if args.still:
            # After the ladder: the whole picture exactly, as a still patch (the gadget must list ui_still).
            raw = Path(args.still).read_bytes()
            if len(raw) != width * height * 2:
                print(f"FAIL  --still: {len(raw)} bytes, not {width}x{height} RGB565")
                return 1
            if not caps.get("ui_still"):
                print("FAIL  this gadget doesn't list mg_stream_video_capability_ui_still")
                return 1
            frames = frames + [still_patch(raw, 0, 0, width, height)]
        times = ui_ladder(len(frames), args.interval)
        sent, t0, rows = 0, time.monotonic(), []
        for k, payload in enumerate(frames):
            wait = t0 + times[k] - time.monotonic()
            if wait > 0:
                await asyncio.sleep(wait)
            is_still = k >= n_codec
            data = video_frame(k, not is_still and is_keyframe(payload, bands), payload, still=is_still)
            t_first = time.monotonic()
            for i in range(0, len(data), chunk):
                w = data[i:i + chunk]
                while sent - g.vbuf.get("received", 0) + len(w) > g.vbuf.get("avail", 0):
                    g.vbuf_event.clear()
                    try:
                        await asyncio.wait_for(g.vbuf_event.wait(), 0.2)
                    except asyncio.TimeoutError:
                        await client.write_gatt_char(P["MG_CONTROL_UUID"],
                                                     bytes([sv, P["mg_stream_video_request_buffer_update"]]),
                                                     response=False)
                await wait_write_room(client)
                await client.write_gatt_char(P["MG_DATA_UUID"], w, response=False)
                sent += len(w)
            t_last = time.monotonic()
            # In: the device's received count covers the frame (the client's stack may still have been
            # sending after its last write returned). Shown: it echoes the frame number it finished
            # drawing, and how long ago.
            shown_at = in_at = None
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                v = g.vbuf
                if in_at is None and v.get("received", 0) >= sent:
                    in_at = g.vbuf_t
                if v.get("last_number") == k and v.get("last_age_ms", 0xffff) != 0xffff:
                    shown_at = g.vbuf_t - v["last_age_ms"] / 1000
                    in_at = in_at or shown_at
                    break
                g.vbuf_event.clear()
                try:
                    await asyncio.wait_for(g.vbuf_event.wait(), 0.1)
                except asyncio.TimeoutError:
                    pass
            row = {"frame": k, "bytes": len(payload),
                   "link_ms": (in_at - t_first) * 1000 if in_at else None,
                   "in_to_shown_ms": (shown_at - in_at) * 1000 if shown_at else None,
                   "first_to_shown_ms": (shown_at - t_first) * 1000 if shown_at else None}
            rows.append(row)
            kbps = len(data) * 8 / (in_at - t_first) / 1000 if in_at and in_at > t_first else 0
            print(f"frame {k:2d}: {len(payload):6d} B over the link in {row['link_ms'] or 0:6.0f} ms ({kbps:4.0f} kb/s); "
                  + (f"all in -> shown {row['in_to_shown_ms']:4.0f} ms; first byte -> shown "
                     f"{row['first_to_shown_ms']:5.0f} ms" if shown_at else "NOT SEEN"))
        if args.hold:
            print(f"holding the last picture for {args.hold:.0f} s (nothing sent: the link relaxes after 1 s) ...")
            await asyncio.sleep(args.hold)
        v = g.vbuf
        log = "".join(g.nus[nus_from:])
        dev = re.findall(r"ui frame (\d+): (\w+) (\d+) bytes, decode (\d+) ms \(top (\d+), bottom (\d+)\), "
                         r"draw (\d+) ms, in->shown (\d+) ms", log)
        for m in dev:
            print(f"  device: frame {m[0]} {m[1]} {m[2]} B, decode {m[3]} ms (top {m[4]}, bottom {m[5]}), "
                  f"draw {m[6]} ms, received->shown {m[7]} ms")
        md5s = dict(re.findall(r"ui frame (\d+) md5 ([0-9a-f]{32})", log))
        bad = missing = 0
        for k, want in expect_md5.items():
            got = md5s.get(str(k))
            if got and got != want:
                bad += 1
                print(f"FAIL  frame {k} md5 {got}, expected {want}")
            elif not got and args.md5:
                missing += 1
        if missing:
            # No checksum is no proof (a gadget that refused ui.md5=1 logs none).
            print(f"FAIL  {missing} of {len(expect_md5)} expected checksums never arrived")
            bad += missing
        if expect_md5 and args.md5:
            print(f"{'PASS' if not bad else 'FAIL'}  decoded pictures: {len(md5s)} checksummed, "
                  f"{bad - missing} differ, {missing} missing")
        for line in re.findall(r"[^\n]*connection: [^\n]*", log):
            print("  device: " + line.split("mg.ble: ")[-1])
        if not args.keep:
            await g.send(sv, P["mg_stream_video_stop_streaming"], P["mg_stream_audio_buffer_drop"])
            await asyncio.sleep(0.3)
        if args.md5:
            await client.write_gatt_char(P["MG_NUS_RX_UUID"], b"ui.md5=0\n", response=True)
        print(f"shown {v.get('shown')}, dropped {v.get('dropped')}; errors: {'; '.join(g.video_errors) or 'none'}")
        if args.json:
            Path(args.json).write_text(json.dumps({"frames": rows, "device": dev, "md5": md5s}, indent=1))
        return 0 if all(r["first_to_shown_ms"] is not None for r in rows) and not g.video_errors and not bad else 1


async def ui_input_check(g: "Gadget", want: int) -> bool:
    st = await g.status()
    feats = st["typed"].get(P["mg_command_ui_input"], [])
    if P["mg_command_ui_input"] not in st["commands"] or want not in feats:
        print(f"FAIL  this gadget doesn't offer that (ui_input features {feats})")
        return False
    print(f"ui_input features: {feats}")
    return True


async def cmd_touch(args) -> int:
    """Turns touch events on and prints them."""
    from bleak import BleakClient

    ui = P["mg_command_ui_input"]
    addr, _ = await pick(args)
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=args.verbose)
        await g.start()
        if not await ui_input_check(g, P["mg_ui_input_feature_touch"]):
            return 1
        g.touch_print = True
        await g.send(ui, P["mg_ui_input_touch_enable"], 1)
        print(f"touch events for {args.secs:.0f} s (touch the gadget's screen) ...")
        await asyncio.sleep(args.secs)
        await g.send(ui, P["mg_ui_input_touch_enable"], 0)
        downs = sum(t["action"] == "down" for t in g.touches)
        print(f"{len(g.touches)} events, {downs} touches")
    return 0


async def cmd_hid(args) -> int:
    """Sends hid_map and shows hid_status (bonded, host attached), then watches it."""
    from bleak import BleakClient

    addr, _ = await pick(args)
    square = tuple(float(v) for v in args.square.split(","))
    rel = tuple(int(v) for v in args.rel.split(",")) if args.rel else (0, 0)
    async with BleakClient(addr, timeout=20.0) as client:
        g = Gadget(client, verbose=True)
        await g.start()
        feature = {"off": "hid_digitizer", "digitizer": "hid_digitizer", "abs": "hid_mouse_abs",
                   "rel": "hid_mouse_rel"}[args.mode]
        if not await ui_input_check(g, P[f"mg_ui_input_feature_{feature}"]):
            return 1
        # A set-up gadget takes hid_map only on a proven connection.
        result = await prove_connection(g, addr, await g.status())
        if result not in (None, "match", "not_found"):
            print(f"FAIL  token proof: {result}: a set-up gadget refuses hid_map on this connection")
            return 1
        await client.write_gatt_char(P["MG_CONTROL_UUID"], hid_map_payload(args.mode, square, args.fisheye, rel),
                                     response=False)
        await asyncio.sleep(0.5)
        if args.pair:
            # Reading the Report Map needs encryption: the OS pairs (Just Works) and bonds.
            try:
                rm = await client.read_gatt_char(uuid16(0x2A4B))
                print(f"report map: {len(rm)} bytes (link encrypted)")
            except Exception as e:
                print(f"report map read failed: {e!r}")
        await asyncio.sleep(args.secs)
        for s_ in g.hid_status:
            print(f"hid_status: {s_}")
        if not g.hid_status:
            print("FAIL  no hid_status")
            return 1
    return 0


async def cmd_log(args) -> int:
    from bleak import BleakClient

    addr, _ = await pick(args)
    lines = redact.LineRedactor()   # whole lines, so a secret split across notifications is caught too
    async with BleakClient(addr, timeout=20.0) as client:
        await client.start_notify(P["MG_NUS_TX_UUID"], lambda _c, d: print(
            lines.feed(printable(bytes(d).decode("utf-8", "replace"))), end="", flush=True))
        if args.command:
            await client.write_gatt_char(P["MG_NUS_RX_UUID"], args.command.encode() + b"\n", response=True)
        await asyncio.sleep(args.secs)
    print(lines.flush(), end="", flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--address", help="BLE address (a UUID on macOS) from `scan`")
    ap.add_argument("--name", default=None, help=f"name prefix (default: any {P['MG_DEVICE_NAME_PREFIX']}*)")
    ap.add_argument("--scan-secs", type=float, default=6.0)
    ap.add_argument("--mic-bench", choices=tuple(MIC_BENCH_WORDS),
                    help="send that board's bench word on the console right after connect, so this client "
                         "gets mic output without a token proof (esp32 and xn54, for this connection; bench "
                         "builds only: release builds refuse it before a proof)")
    ap.add_argument("--bench-word", action="append", default=[], metavar="CMD",
                    help="a console word to send right after connect (repeatable, in order, after --mic-bench's)")
    ap.add_argument("--no-identity", action="store_true",
                    help="don't send the client's app and version at connect (setting 19)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan")
    t = sub.add_parser("test")
    t.add_argument("--secs", type=float, default=2.0, help="how long to capture")
    t.add_argument("--codec", choices=("sbc", "lc3"), default="sbc")
    t.add_argument("--out", default="mg_capture.wav")
    t.add_argument("--no-hear", action="store_true",
                   help="skip the mic-hears check (a prompt played from this computer must come back "
                        "above --hear-db at every listed capture rate)")
    t.add_argument("--hear-db", type=float, default=-50.0, help="the prompt capture's minimum RMS (dBFS)")
    t.add_argument("--hear-secs", type=float, default=4.0, help="how long the prompt capture runs")
    t.add_argument("--prompt-cmd", help="the command that plays the prompt (default: a tone burst "
                                        "through afplay, paplay or aplay)")
    p = sub.add_parser("ptt")
    p.add_argument("--count", type=int, default=0, help="utterances to record (0: until Ctrl-C)")
    p.add_argument("--haptics", action="store_true", help="also turn on the activation buzz")
    p.add_argument("--out-dir", default="mg_ptt")
    p.add_argument("--no-states", action="store_true",
                   help="don't send assistant_state (responding, then done) after each utterance")
    p.add_argument("--think", type=float, default=1.0, help="seconds of thinking before responding")
    p.add_argument("--respond", type=float, default=1.5, help="seconds of responding before done")
    su = sub.add_parser("setup")
    su.add_argument("--tokens", metavar="FILE", help=TOKENS_HELP)
    su.add_argument("--confirm-serial", metavar="PORT",
                    help="press the gadget's button over its serial console (>pair.confirm; tools/mg_confirm.py) "
                         "instead of asking you to")
    rp = sub.add_parser("reprovision", help="set a set-up gadget up again and check the old key is gone")
    rp.add_argument("--tokens", metavar="FILE", help=TOKENS_HELP)
    rp.add_argument("--confirm-serial", metavar="PORT",
                    help="press the gadget's button over its serial console (>pair.confirm, bench builds)")
    rp.add_argument("--verbose", action="store_true")
    pr = sub.add_parser("proof")
    pr.add_argument("--clear", action="store_true", help="after a match, have the gadget reset to setup")
    pr.add_argument("--verbose", action="store_true")
    s = sub.add_parser("state")
    s.add_argument("state", choices=ASSISTANT_STATES)
    s.add_argument("--wait", type=float, default=4.0, help="seconds to watch the face log")
    c = sub.add_parser("call")
    c.add_argument("--answer", choices=("live", "failed", "none"), default="live",
                   help="how to answer the gadget's start: connecting then live (default), connecting then "
                        "failed, or not at all")
    c.add_argument("--connect", type=float, default=1.0, help="seconds between connecting and the answer")
    c.add_argument("--start", action="store_true", help="start a call from the phone's side (connecting, live)")
    c.add_argument("--state", nargs="+", choices=LIVE_CALL_STATES, help="send these states, --connect s apart")
    c.add_argument("--end-after", type=float, default=0.0, metavar="SECS",
                   help="send ended this long after the call goes live (default: never)")
    c.add_argument("--secs", type=float, default=60.0, help="how long to stay connected (0: until Ctrl-C)")
    q = sub.add_parser("queue")
    q.add_argument("--download", action="store_true")
    q.add_argument("--clear", action="store_true",
                   help="erase every clip on the gadget afterwards. Without --download that includes clips never "
                        "fetched, which are lost; with --download, only once every clip came down whole and saved")
    q.add_argument("--out-dir", default="mg_queue",
                   help="where --download saves each clip, as clip_<date-time>_<index>.wav beside its .sbc or .lc3")
    pl = sub.add_parser("play")
    pl.add_argument("--tone", type=float, default=1000.0, help="tone frequency in Hz (default 1000)")
    pl.add_argument("--secs", type=float, default=3.0)
    pl.add_argument("--sweep", action="store_true", help="a 200 Hz to 7 kHz sweep instead of a tone")
    pl.add_argument("--wav", help="play a 16-bit WAV file instead")
    pl.add_argument("--rate", type=int, default=16000, choices=(16000, 32000, 44100, 48000),
                    help="SBC sample rate for --tone/--sweep")
    pl.add_argument("--level", type=float, default=-12.0, help="tone level in dBFS")
    pl.add_argument("--bitpool", type=int, default=26)
    pl.add_argument("--volume", type=int, choices=range(0, 101), metavar="0-100",
                    help="the gadget's speaker_volume for this run, RAM only (its vol= word) and put "
                         "back at the end (0 turns the speaker off); default: leave it")
    pl.add_argument("--loopback", action="store_true",
                    help="record the gadget's mic while it plays and check the tone comes back")
    pl.add_argument("--out", default="mg_loopback.wav", help="where --loopback saves the mic capture")
    pl.add_argument("--verbose", action="store_true")
    b = sub.add_parser("bench")
    b.add_argument("--send", type=int, metavar="SECS", help="device -> client for SECS seconds")
    b.add_argument("--recv", type=int, metavar="SECS", help="client -> device for SECS seconds")
    sub.add_parser("images")
    d = sub.add_parser("dfu")
    d.add_argument("--image", required=True,
                   help="an MCUboot-signed image (zephyr.signed.bin); it goes over BLE unencrypted, so an SDK token "
                        "built into it can be recorded by anyone in range: build bench images without one (one "
                        "with a token is refused unless --allow-token-image)")
    m = d.add_mutually_exclusive_group()
    m.add_argument("--test", action="store_true", help="run it once at the next reset (reverts unless confirmed)")
    m.add_argument("--confirm", action="store_true", help="make it permanent")
    d.add_argument("--reset", action="store_true",
                   help="restart the gadget afterwards; with --test/--confirm, wait for it to come back confirmed")
    d.add_argument("--chunk", type=int, help="cap each SMP request at this many bytes (default: the server's buffer)")
    d.add_argument("--window", type=int, default=0,
                   help="requests in flight (default: 2 when the server has 4+ buffers, else 1)")
    d.add_argument("--allow-token-image", action="store_true",
                   help="send an image that holds an SDK token anyway (refused otherwise: it goes over BLE "
                        "unencrypted); never with --unproven. The check is best effort: it finds a plain mgst_ "
                        "token or a filled token slot, not a token stored any other way")
    d.add_argument("--unproven", action="store_true",
                   help="update without the token proof: the gadget asks for a press of its button to confirm (30 s); "
                        "needs --address and an image without an SDK token")
    u = sub.add_parser("ui", help="stream a UI stream (content=ui) from a test stream file")
    u.add_argument("file", help="NAME.au / NAME.264 (one band) or NAME.mgframes (two bands), as the codec study writes")
    u.add_argument("--bands", type=int, choices=(1, 2), help="override the bands the file name implies")
    u.add_argument("--width", type=int, help="picture width (default: the ui capability's)")
    u.add_argument("--height", type=int)
    u.add_argument("--fps", type=int, default=15, help="nominal fps parameter")
    u.add_argument("--kbps", type=int, default=300, help="nominal bitrate parameter")
    u.add_argument("--interval", type=float, default=1.0, help="seconds between frames after the refinement ladder")
    u.add_argument("--hold", type=float, default=0.0, help="seconds to keep the last picture (link relaxes) before stopping")
    u.add_argument("--still", help="after the stream, this raw RGB565 big-endian picture (W x H x 2 bytes) as a lossless still patch")
    u.add_argument("--keep", action="store_true", help="leave the stream on at the end")
    u.add_argument("--md5", action="store_true", help="have the gadget log each picture's MD5 (and compare with --index)")
    u.add_argument("--index", help="the codec study's index.json, for the expected MD5s")
    u.add_argument("--json", help="write the per-frame numbers here")
    u.add_argument("--latency", type=int, help="the peripheral latency a still stream's link takes (0: never relax)")
    u.add_argument("--verbose", action="store_true")
    to = sub.add_parser("touch", help="turn touch events on and print them")
    to.add_argument("--secs", type=float, default=20.0)
    to.add_argument("--verbose", action="store_true")
    hd = sub.add_parser("hid", help="send hid_map and show hid_status")
    hd.add_argument("--mode", choices=tuple(HID_MODES), default="digitizer")
    hd.add_argument("--square", default="0,0,1,1",
                    help="the streamed square on the phone's screen: x,y,w,h as fractions of its width and height")
    hd.add_argument("--fisheye", action="store_true", help="the square is warped onto the round panel")
    hd.add_argument("--rel", help="mode rel: pointer counts across the screen's width,height")
    hd.add_argument("--pair", action="store_true", help="read the Report Map (encrypted) so this host pairs")
    hd.add_argument("--secs", type=float, default=5.0, help="seconds to watch hid_status")
    lg = sub.add_parser("log")
    lg.add_argument("--secs", type=float, default=30.0)
    lg.add_argument("--command", help="a console command to send first (status, version)")
    args = ap.parse_args(argv)
    global IDENTIFY, BENCH_WORDS
    IDENTIFY = not args.no_identity
    BENCH_WORDS = bench_words(args.mic_bench, args.bench_word)
    fn = {"scan": cmd_scan, "test": cmd_test, "ptt": cmd_ptt, "queue": cmd_queue, "log": cmd_log,
          "play": cmd_play, "state": cmd_state, "call": cmd_call, "setup": cmd_setup, "proof": cmd_proof,
          "images": cmd_images,
          "reprovision": cmd_reprovision,
          "dfu": cmd_dfu, "bench": cmd_bench, "ui": cmd_ui, "touch": cmd_touch, "hid": cmd_hid}[args.cmd]
    try:
        return asyncio.run(fn(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
