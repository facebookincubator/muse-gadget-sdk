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

"""Customize a Muse avatar locally, preview every state, and export a preset.

    python3 tools/muse/customize.py                         open the browser editor
    python3 tools/muse/customize.py --preset preset.json --out avatar.zip
    python3 tools/muse/customize.py --preset preset.json --install

Needs a C compiler and Pillow. No board, Muse connection, or token is needed.
Install only writes the ignored local renderer; use the existing board tools
to build and flash it. Character artwork retains its own licensing terms.
"""
import argparse
import base64
import io
import json
import math
import re
import secrets
import shutil
import subprocess
import tempfile
import webbrowser
import zipfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import make_gifs

ROOT = Path(make_gifs.ROOT)
HERE = Path(__file__).resolve().parent
DEFAULT = {
    "version": 1, "pack": "jollybot", "name": "My Muse",
    "fur": "#cfbc9f", "face": "#f6dfbd", "eyes": "#120d0b", "blush": "#f4aaa0",
    "width": 1.0, "height": 1.0, "face_x": 0.0, "face_y": 0.0,
    "accessory": "none", "accessory_color": "#5cb8ff",
}
LIMITS = {"width": (0.85, 1.15), "height": (0.90, 1.08),
          "face_x": (-2.0, 2.0), "face_y": (-2.0, 2.0)}
COLORS = ("fur", "face", "eyes", "blush", "accessory_color")
ACCESSORIES = {"none": 0, "scarf": 1, "bow": 2}
ANIMATIONS = ("boot", "idle", "listening", "thinking", "speaking", "happy", "off", "error")
MAX_BODY = 8192


def packs():
    """Only registered local templates are compiled; requests cannot supply C."""
    found = {"jollybot": {"name": "Jollybot", "source": Path(make_gifs.DEFAULT_SRC),
                         "notice": "Meta artwork; excluded from the SDK's Apache license."}}
    for manifest in sorted((ROOT / "avatar" / "packs").glob("*/pack.json")):
        data = json.loads(manifest.read_text(encoding="utf-8"))
        key = manifest.parent.name
        source = manifest.parent / "muse_pixel.c"
        if source.is_file() and re.fullmatch(r"[a-z0-9-]+", key):
            found[key] = {"name": data["name"], "source": source, "notice": data["notice"]}
    return found


def validate(value):
    if not isinstance(value, dict):
        raise ValueError("A preset must be a JSON object.")
    unknown = set(value) - set(DEFAULT)
    if unknown:
        raise ValueError("Unknown preset fields: " + ", ".join(sorted(unknown)))
    p = {**DEFAULT, **value}
    if type(p["version"]) is not int or p["version"] != 1:
        raise ValueError("Unsupported preset version; expected 1.")
    if not isinstance(p["pack"], str) or p["pack"] not in packs():
        raise ValueError("Choose a registered character pack.")
    if not isinstance(p["name"], str) or not 1 <= len(p["name"].strip()) <= 64 or not p["name"].isprintable():
        raise ValueError("The name must contain 1–64 printable characters.")
    p["name"] = p["name"].strip()
    for key in COLORS:
        if not isinstance(p[key], str) or not re.fullmatch(r"#[0-9a-fA-F]{6}", p[key]):
            raise ValueError(f"{key} must be a six-digit hex colour.")
        p[key] = p[key].lower()
    for key, (lo, hi) in LIMITS.items():
        v = p[key]
        if type(v) not in (int, float) or not math.isfinite(v) or not lo <= v <= hi:
            raise ValueError(f"{key} must be between {lo} and {hi}.")
    if not isinstance(p["accessory"], str) or p["accessory"] not in ACCESSORIES:
        raise ValueError("Choose none, scarf, or bow.")
    return p


def tint(colour, reference, shade):
    """Retain the original shading ratios; default colours are bit-for-bit equal."""
    channels = lambda c: tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))
    result = [min(255, (v * s + r // 2) // r)
              for v, r, s in zip(channels(colour.lstrip("#")), channels(reference), channels(shade))]
    return "0x" + "".join(f"{v:02x}" for v in result)


def source(preset):
    p = validate(preset)
    template = packs()[p["pack"]]["source"].read_text(encoding="utf-8")
    marker = '#include "muse_avatar_style.h"'
    if marker not in template:
        raise ValueError("This character does not support appearance presets.")
    values = {}
    for macro, shade in {"OUT": "3a2b22", "OUT2": "8c7560", "FUR_D": "ae987e",
                         "FUR": "cfbc9f", "FUR_L": "e6d7bd", "FUR_H": "f8eedc"}.items():
        values[macro] = tint(p["fur"], "cfbc9f", shade)
    for macro, shade in {"FACE_D": "e9cba4", "FACE": "f6dfbd", "FACE_L": "fdeed6"}.items():
        values[macro] = tint(p["face"], "f6dfbd", shade)
    values.update(EYES="0x" + p["eyes"][1:], BLUSH="0x" + p["blush"][1:],
                  BLUSH_D=tint(p["blush"], "f4aaa0", "ea8f8e"),
                  CLOTH="0x" + p["accessory_color"][1:],
                  CLOTH_D=tint(p["accessory_color"], "5cb8ff", "2a5bd7"),
                  ACCESSORY=str(ACCESSORIES[p["accessory"]]))
    values.update({k.upper(): f"{float(p[k]):.6f}f" for k in LIMITS})
    definitions = "/* Appearance preset generated by tools/muse/customize.py. */\n"
    definitions += "".join(f"#define MUSE_STYLE_{k} {v}\n" for k, v in values.items())
    return template.replace(marker, definitions + marker, 1)


def frames(preset):
    with tempfile.TemporaryDirectory(prefix="muse-customizer-") as tmp:
        path = Path(tmp) / "muse_pixel.c"
        path.write_text(source(preset), encoding="utf-8")
        return make_gifs.render_frames(path, scale=1)


def sprites(animations):
    """Opaque RGB atlases include the renderer's background, glow, and shadows."""
    from PIL import Image

    files, states = {}, {}
    for name in ANIMATIONS:
        sequence = animations[name]
        width, height = sequence[0].size
        columns = 10
        rows = (len(sequence) + columns - 1) // columns
        atlas = Image.new("RGB", (width * columns, height * rows))
        for i, frame in enumerate(sequence):
            atlas.paste(frame, ((i % columns) * width, (i // columns) * height))
        out = io.BytesIO()
        atlas.save(out, format="PNG")
        filename = f"sprites/{name}.png"
        files[filename] = out.getvalue()
        states[name] = {"file": filename, "frames": len(sequence), "columns": columns,
                        "rows": rows, "loop": name not in ("boot", "happy", "off")}
    manifest = {"version": 1, "width": width, "height": height, "frame_ms": make_gifs.FRAME_MS,
                "background": "opaque", "states": states}
    files["sprites/manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    return files, manifest


def preview(preset):
    files, manifest = sprites(frames(preset))
    return {"manifest": manifest, "images": {
        name: "data:image/png;base64," + base64.b64encode(files[entry["file"]]).decode()
        for name, entry in manifest["states"].items()}}


def bundle(preset):
    p = validate(preset)
    animations = frames(p)
    files, _ = sprites(animations)
    files["preset.json"] = (json.dumps(p, indent=2) + "\n").encode()
    files["muse_pixel.c"] = source(p).encode()
    files["ARTWORK_NOTICE.txt"] = (packs()[p["pack"]]["notice"] + "\n").encode()
    for name, sequence in animations.items():
        out = io.BytesIO()
        make_gifs.save_gif(sequence, out)
        files[f"gifs/{name}.gif"] = out.getvalue()
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)
    return out.getvalue()


def install(preset):
    """Validate/run all states first, then replace the private local avatar."""
    code = source(preset)
    frames(preset)
    target = Path(make_gifs.CUSTOM_SRC)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        shutil.copyfile(target, str(target) + ".prev")
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=target.parent,
                                     suffix=".c", delete=False) as f:
        tmp = Path(f.name)
        f.write(code)
    try:
        tmp.replace(target)
    finally:
        tmp.unlink(missing_ok=True)
    (target.parent / "preset.json").write_text(json.dumps(validate(preset), indent=2) + "\n", encoding="utf-8")
    return target


def handler(token):
    downloads = {}

    class Handler(BaseHTTPRequestHandler):
        # HTTPServer serializes compilation; only recent downloads are cached.
        def log_message(self, *args):
            pass

        def reply(self, status, data, content_type="application/json", filename=None):
            if isinstance(data, dict):
                data = json.dumps(data).encode()
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            if filename:
                self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(data)

        def local(self):
            return self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}"

        def do_GET(self):
            if not self.local():
                self.reply(403, {"error": "Use the local URL printed by the tool."})
            elif self.path == "/":
                html = (HERE / "customize.html").read_text(encoding="utf-8").replace("__SESSION_TOKEN__", token)
                self.reply(200, html.encode(), "text/html; charset=utf-8")
            elif self.path in downloads:
                data, content_type, filename = downloads[self.path]
                self.reply(200, data, content_type, filename)
            elif self.path == "/api/catalog":
                self.reply(200, {"defaults": DEFAULT, "limits": LIMITS, "packs": {
                    k: {"name": v["name"], "notice": v["notice"]} for k, v in packs().items()}})
            else:
                self.reply(404, {"error": "Unknown route."})

        def do_POST(self):
            origin = self.headers.get("Origin")
            expected = f"http://127.0.0.1:{self.server.server_port}"
            if not self.local() or (origin and origin != expected) or self.headers.get("X-Muse-Session") != token:
                self.reply(403, {"error": "Use the local customizer page."})
                return
            if self.path not in ("/api/preview", "/api/export", "/api/preset"):
                self.reply(404, {"error": "Unknown route."})
                return
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= MAX_BODY:
                    raise ValueError("Preset is empty or too large.")
                if self.headers.get("Content-Type") != "application/json":
                    raise ValueError("Expected application/json.")
                p = validate(json.loads(self.rfile.read(size)))
                if self.path == "/api/preview":
                    self.reply(200, preview(p))
                else:
                    if self.path == "/api/export":
                        data, mime, filename = bundle(p), "application/zip", "muse-character.zip"
                    else:
                        data = (json.dumps(p, indent=2) + "\n").encode()
                        mime, filename = "application/json", "preset.json"
                    url = f"/download/{secrets.token_hex(24)}/{filename}"
                    downloads[url] = data, mime, filename
                    # Bound memory while leaving recent links available for retries.
                    while len(downloads) > 4:
                        del downloads[next(iter(downloads))]
                    self.reply(200, {"download": url})
            except (ValueError, UnicodeError) as e:
                self.reply(400, {"error": str(e)})
            except (OSError, subprocess.SubprocessError, ImportError) as e:
                # Avoid returning machine paths or compiler diagnostics to the browser.
                print(f"Preview failed: {e}")
                self.reply(500, {"error": "Preview failed. Check the terminal for compiler or Pillow errors."})
    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preset", type=Path, help="load a saved JSON preset")
    output = ap.add_mutually_exclusive_group()
    output.add_argument("--out", type=Path, help="export preset, C, GIFs, and sprite atlases as a ZIP")
    output.add_argument("--install", action="store_true", help="save the ignored local renderer; does not flash")
    ap.add_argument("--port", type=int, default=0, help="local HTTP port (default: find a free port)")
    ap.add_argument("--no-browser", action="store_true", help="print the URL without opening a browser")
    args = ap.parse_args()
    if args.preset and not (args.out or args.install):
        ap.error("Use --preset with --out or --install; the browser can import JSON presets.")
    try:
        p = validate(json.loads(args.preset.read_text(encoding="utf-8"))) if args.preset else DEFAULT
        if args.install:
            print(f"Installed {install(p).relative_to(ROOT)}; previous renderer saved as muse_pixel.c.prev.")
            print("Build and flash with tools/muse/board.sh, or avatar.py --reply with this renderer.")
        elif args.out:
            args.out.write_bytes(bundle(p))
            print(f"Exported {args.out}")
        else:
            with HTTPServer(("127.0.0.1", args.port), handler(secrets.token_hex(24))) as server:
                server.timeout = 1
                url = f"http://127.0.0.1:{server.server_port}/"
                print(f"Muse avatar customizer: {url}", flush=True)
                print("Press Ctrl-C to stop. All previews stay on this computer.", flush=True)
                if not args.no_browser:
                    webbrowser.open(url)
                server.serve_forever()
    except (ValueError, OSError, ImportError, subprocess.SubprocessError) as e:
        ap.exit(1, f"{e}\n")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
