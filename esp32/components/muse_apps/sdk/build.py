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

"""Build a Muse gadget app (WebAssembly, apps API 1.x) from C, and check it as the gadget will.

    python3 build.py APP_FOLDER                       its .c files -> APP_FOLDER/app.wasm
    python3 build.py app.c [more.c ...] [-o app.wasm]

Uses wasi-sdk if it finds one ($WASI_SDK, ~/wasi-sdk, /opt/wasi-sdk), else Zig's
clang from the ziglang package of the Python that runs this script:

    python3 -m venv ~/gadget-sdk/venv && ~/gadget-sdk/venv/bin/pip install ziglang
    ~/gadget-sdk/venv/bin/python build.py ~/workspace/gadgets/apps/hello

gadget.h is next to this script. The app can use libc's pure functions (memcpy,
strlen, sinf, malloc) but nothing that reaches WASI, which gadgets don't offer:
stdio (printf, even snprintf) and the clock (time) do. It's compiled for what the
gadget's interpreter takes: the wasm MVP plus bulk memory, sign extension, mutable
globals and non-trapping float-to-int, as a WASI reactor (its _initialize, if
any, runs before on_start).

Then it checks the module as the gadget will, and deletes it if the gadget would
refuse it: an import from any module but "gadget", or one the API doesn't have or
with another signature; no on_start export; a manifest (GADGET_MANIFEST) that
isn't JSON or has a wrong api or name; more than 256 KB. It warns when there's no
manifest (the app then gets no permissions and no devices) and when the app calls
something whose permission the manifest doesn't ask for.

The API's functions come from api.capabilities.json: --capabilities FILE, else api.capabilities.json
beside this script or in api/ under it, else gadget.h's declarations. app.api
serves the gadget's own, in parts: joined, they're api.capabilities.json.
"""
import argparse
import glob
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FEATURES = ["-mcpu=mvp", "-mbulk-memory", "-msign-ext", "-mmutable-globals", "-mnontrapping-fptoint"]
APP_NAME = re.compile(r"^[a-z0-9][a-z0-9-]{0,30}$")
LIST_MAX, ENTRY_MAX = 16, 31   # a manifest's requires or uses: entries, characters each
VALTYPES = {0x7F: "i", 0x7E: "I", 0x7D: "f", 0x7C: "F"}


# ---- The module ----

def leb(b, i):
    result = shift = 0
    while True:
        byte = b[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, i


def name_at(b, i):
    n, i = leb(b, i)
    return b[i:i + n].decode("utf-8", "replace"), i + n


def limits_at(b, i):
    flags, i = leb(b, i)
    _, i = leb(b, i)
    if flags & 1:
        _, i = leb(b, i)
    return i


def read_module(data):
    """Imports as (module, name, kind, params, results), exported functions, custom sections."""
    if data[:8] != b"\0asm\x01\0\0\0":
        raise ValueError("not a WebAssembly module")
    types, imports, exports, customs = [], [], set(), {}
    i = 8
    while i < len(data):
        sid = data[i]
        size, j = leb(data, i + 1)
        end = j + size
        if sid == 0:
            name, k = name_at(data, j)
            customs[name] = data[k:end]
        elif sid == 1:
            count, k = leb(data, j)
            for _ in range(count):
                k += 1                                   # 0x60: a function type
                n, k = leb(data, k)
                params = "".join(VALTYPES.get(data[k + x], "?") for x in range(n))
                k += n
                n, k = leb(data, k)
                results = "".join(VALTYPES.get(data[k + x], "?") for x in range(n))
                types.append((params, results))
                k += n
        elif sid == 2:
            count, k = leb(data, j)
            for _ in range(count):
                mod, k = name_at(data, k)
                name, k = name_at(data, k)
                kind = data[k]
                k += 1
                params = results = ""
                if kind == 0:                            # function: its type
                    t, k = leb(data, k)
                    params, results = types[t]
                elif kind == 1:                          # table: element type, limits
                    k = limits_at(data, k + 1)
                elif kind == 2:                          # memory: limits
                    k = limits_at(data, k)
                else:                                    # global: type, mutability
                    k += 2
                imports.append((mod, name, kind, params, results))
        elif sid == 7:
            count, k = leb(data, j)
            for _ in range(count):
                name, k = name_at(data, k)
                kind = data[k]
                _, k = leb(data, k + 1)
                if kind == 0:
                    exports.add(name)
        i = end
    return imports, exports, customs


# ---- The API ----

def wamr_to_wasm(sig):
    """A WAMR signature, "(ii$*~)i", as wasm types: ("iiiii", "i")."""
    params, result = sig[1:].split(")")
    return params.replace("*~", "ii").replace("$", "i").replace("*", "i"), result


class Api:
    def __init__(self, capabilities_file=None):
        self.version, self.functions, self.perms, self.module_bytes = None, {}, {}, 256 * 1024
        self.source = None
        header = os.path.join(HERE, "gadget.h")
        text = open(header, encoding="utf-8").read() if os.path.exists(header) else ""
        # Which calls need which permission: gadget.h's comments say.
        for comment, name in re.findall(r"/\*((?:(?!\*/).)*)\*/\s*GADGET_IMPORT\((\w+)\)", text, re.S):
            m = re.search(r"Needs the (\S+) permission", " ".join(comment.replace("*", " ").split()))
            if m:
                self.perms[name] = m.group(1)
        found = [capabilities_file] if capabilities_file else [os.path.join(HERE, "api.capabilities.json"),
                                               os.path.join(HERE, "api", "api.capabilities.json")]
        for path in found:
            if path and os.path.exists(path):
                capabilities = json.load(open(path, encoding="utf-8"))
                self.version = capabilities["api"]
                self.functions = {n: wamr_to_wasm(s) for n, s in capabilities["functions"].items()}
                self.module_bytes = capabilities.get("limits", {}).get("module_bytes", self.module_bytes)
                rel = os.path.relpath(path)
                self.source = os.path.abspath(path) if rel.startswith("..") else rel
                return
            if capabilities_file:
                sys.exit("No %s" % capabilities_file)
        if not text:
            sys.exit("No gadget.h next to build.py, and no api.capabilities.json: save gadget.h there.")
        major = re.search(r"#define GADGET_API_MAJOR (\d+)", text)
        minor = re.search(r"#define GADGET_API_MINOR (\d+)", text)
        if not (major and minor):
            sys.exit("gadget.h next to build.py isn't an apps API 1.x header")
        self.version = "%s.%s" % (major.group(1), minor.group(1))
        for ret, name, params in re.findall(r"GADGET_IMPORT\(\w+\)\s+(\w+)\s+(\w+)\(([^)]*)\);", text):
            args = [] if params.strip() == "void" else params.split(",")
            wasm = "".join("I" if a.split()[0] in ("int64_t", "uint64_t") else
                           "f" if a.split()[0] == "float" else "i" for a in args)
            self.functions[name] = (wasm, "" if ret == "void" else "I" if ret == "int64_t" else "i")
        self.source = "gadget.h"


# ---- Building ----

def compiler():
    """The command that compiles for wasm32-wasi, and its name."""
    for d in (os.environ.get("WASI_SDK"), os.path.expanduser("~/wasi-sdk"), "/opt/wasi-sdk"):
        if d and os.path.exists(os.path.join(d, "bin", "clang")):
            return [os.path.join(d, "bin", "clang"), "--target=wasm32-wasip1",
                    "--sysroot=" + os.path.join(d, "share", "wasi-sysroot")], "wasi-sdk"
    if importlib.util.find_spec("ziglang"):
        return [sys.executable, "-m", "ziglang", "cc", "-target", "wasm32-wasi"], "zig"
    sys.exit("No compiler: pip install ziglang (into the Python that runs this script), or unpack wasi-sdk\n"
             "(https://github.com/WebAssembly/wasi-sdk/releases) and set WASI_SDK to it.")


def check(path, api):
    """What the gadget would refuse (errors) and what's likely a mistake (warnings)."""
    data = open(path, "rb").read()
    errors, warnings = [], []
    try:
        imports, exports, customs = read_module(data)
    except (ValueError, IndexError) as e:
        return ["%s isn't a module the gadget can read: %s" % (path, e)], []
    if len(data) > api.module_bytes:
        errors.append("%d bytes: the gadget takes up to %d" % (len(data), api.module_bytes))
    foreign = ["%s.%s" % (m, n) for m, n, _, _, _ in imports if m != "gadget"]
    if foreign:
        errors.append("imports %s, which gadgets don't offer: printf, snprintf, time and other libc calls that\n"
                      "  need WASI pull these in. Use only gadget.h and pure libc functions." % ", ".join(foreign))
    called = []
    for mod, name, kind, params, results in imports:
        if mod != "gadget":
            continue
        if kind != 0:
            errors.append("imports gadget.%s, which isn't a function" % name)
        elif name not in api.functions:
            errors.append("calls %s, which API %s doesn't have (%s)" % (name, api.version, api.source))
        elif (params, results) != api.functions[name]:
            want = api.functions[name]
            errors.append("calls %s as (%s)%s, but the API's is (%s)%s: declare it as gadget.h does"
                          % (name, params, results, want[0], want[1]))
        else:
            called.append(name)
    if "on_start" not in exports:
        errors.append("exports no on_start: GADGET_EXPORT(on_start) void on_start(void) { ... }")

    raw = customs.get("gadget.manifest")
    if raw is None:
        warnings.append('no manifest, so the app gets no permissions and no devices: add\n'
                        '  GADGET_MANIFEST("{\\"api\\":\\"%s\\",\\"name\\":\\"my-app\\"}");' % api.version)
        return errors, warnings
    try:
        manifest = json.loads(raw.rstrip(b"\0").decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        return errors + ["the manifest isn't JSON (%s): %r" % (e, raw[:80])], warnings
    if not isinstance(manifest, dict):
        return errors + ["the manifest isn't a JSON object"], warnings
    m = re.match(r"^(\d+)\.(\d+)$", str(manifest.get("api", "")))
    major, minor = (int(x) for x in api.version.split("."))
    if not isinstance(manifest.get("api"), str) or not m:
        errors.append('the manifest\'s api must be a version string: "%s"' % api.version)
    elif int(m.group(1)) != major or int(m.group(2)) > minor:
        errors.append("the manifest asks for API %s; this is %s" % (manifest["api"], api.version))
    name = manifest.get("name")
    if not isinstance(name, str) or not APP_NAME.match(name):
        errors.append("the manifest's name must be the app's folder name: a-z, 0-9 and '-', up to 31 characters")
    if "title" in manifest and not isinstance(manifest["title"], str):
        errors.append("the manifest's title must be a string")
    lists = {}
    for key in ("requires", "uses"):
        items = manifest.get(key, [])
        if not (isinstance(items, list) and len(items) <= LIST_MAX and
                all(isinstance(x, str) and 0 < len(x) <= ENTRY_MAX for x in items)):
            errors.append("the manifest's %s must be a list of up to %d names of up to %d characters"
                          % (key, LIST_MAX, ENTRY_MAX))
            items = []
        lists[key] = items
    for fn in called:
        perm = api.perms.get(fn)
        if perm and perm not in lists["uses"]:
            warnings.append('calls %s, which needs %s: add "%s" to the manifest\'s uses, or the gadget answers '
                            "ERR_DENIED" % (fn, perm, perm))
    folder = os.path.basename(os.path.dirname(os.path.abspath(path)))
    if os.path.basename(path) == "app.wasm" and isinstance(name, str) and folder != name:
        warnings.append("the manifest's name is %s, but the app's folder is %s: make them the same" % (name, folder))
    return errors, warnings


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="+", help="an app's folder, or its .c files")
    ap.add_argument("-o", "--output", help="default: app.wasm in the app's folder, else NAME.wasm here")
    ap.add_argument("--stack", type=int, default=8192, help="the app's own stack in its memory, bytes")
    ap.add_argument("--capabilities", help="the API to check against: api.capabilities.json, from app.api or the gadget SDK")
    args = ap.parse_args()
    sources, out = args.sources, args.output
    if len(sources) == 1 and os.path.isdir(sources[0]):
        folder = sources[0]
        sources = sorted(glob.glob(os.path.join(folder, "*.c")))
        if not sources:
            sys.exit("No .c files in %s" % folder)
        out = out or os.path.join(folder, "app.wasm")
    out = out or os.path.splitext(os.path.basename(sources[0]))[0] + ".wasm"
    api = Api(args.capabilities)
    cc, name = compiler()
    cmd = [*cc, "-mexec-model=reactor", "-O2", *FEATURES, "-I", HERE, "-Wall", "-Wextra",
           "-Wl,--no-entry", "-Wl,--gc-sections", "-Wl,--strip-debug", "-Wl,-z,stack-size=%d" % args.stack,
           *sources, "-o", out]
    if subprocess.run(cmd).returncode:
        return 1
    errors, warnings = check(out, api)
    for w in warnings:
        print("%s: warning: %s" % (out, w), file=sys.stderr)
    if errors:
        os.remove(out)
        for e in errors:
            print("%s: %s" % (out, e), file=sys.stderr)
        print("%s: removed: the gadget would refuse it" % out, file=sys.stderr)
        return 1
    with open(out, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()  # app.install's sha256, if you want the gadget to check it
    print("%s: %d bytes (%s; API %s from %s), sha256 %s" % (out, os.path.getsize(out), name, api.version, api.source,
                                                           digest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
