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
"""Checks api.json and makes what's generated from it.

    python3 components/muse_apps/sdk/api/gen_api.py           # write the generated files
    python3 components/muse_apps/sdk/api/gen_api.py --check   # fail if any is out of date

From api.json, beside this script:
  gadget.h          (in sdk/) the header apps include, and that MUSE.md carries
  REFERENCE.md      the reference for people and for Muse
  muse_apps_api.h   (in the component, beside sdk/) the firmware's side: imports with
                    their WAMR signatures, prototypes and weak stubs,
                    widgets, properties, well-known devices
  api.capabilities.json     api.json without the prose, as app.api serves it
It also rewrites api.json itself in its canonical layout.
"""

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = re.compile(r"^[a-z][a-z0-9_]*$")
CONST = re.compile(r"^[A-Z0-9_]+$")
COPYRIGHT = """Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License."""


class ApiError(Exception):
    pass


def fail(msg):
    raise ApiError(msg)


# ---------------------------------------------------------------------------
# Model


class Api:
    def __init__(self, raw):
        self.raw = raw
        self.version = raw["version"]
        self.types = raw["types"]
        self.enums = {e["name"]: e for e in raw["enums"] + raw["ui_enums"]}
        self.widgets = raw["widgets"]
        self.props = raw["props"]
        self.functions = raw["functions"]
        self.groups = raw["groups"]
        self.devices = raw["devices"]
        self.well_known = raw["devices"]["well_known"]
        self.events = raw["events"]
        self.check()

    # Types an argument or property may have: the base types, an enum, or
    # one of the generated id spaces.
    def type_known(self, t):
        return t in self.types or t in self.enums or t in ("widget", "prop")

    def c_params(self, args):
        out = []
        for name, t, _ in args:
            c = self.types[t]["c"] if t in self.types else "int32_t"
            if isinstance(c, list):
                tail = "size" if t == "out" else "len"
                out.append(f"{c[0]}{name}")
                out.append(f"{c[1]} {name}_{tail}")
            else:
                sep = "" if c.endswith("*") else " "
                out.append(f"{c}{sep}{name}")
        return ", ".join(out) or "void"

    def c_ret(self, fn):
        if not fn.get("ret"):
            return "void"
        t = fn["ret"][0]
        return self.types[t]["c"] if t in self.types else "int32_t"

    def signature(self, fn):
        s = "".join(self.types[t]["wasm"] if t in self.types else "i" for _, t, _ in fn["args"])
        r = ""
        if fn.get("ret"):
            t = fn["ret"][0]
            r = self.types[t]["wasm"] if t in self.types else "i"
        return f"({s}){r}"

    def check(self):
        seen = {}

        def claim(name, where):
            if name in seen:
                fail(f"{name} is defined twice: {seen[name]} and {where}")
            seen[name] = where

        if self.raw.get("schema") != 1:
            fail("schema must be 1")
        for name, cval, t, _ in self.raw["values"]:
            claim(name.split("(")[0], "values")
            if not self.type_known(t):
                fail(f"value {name}: unknown type {t}")
        for e in self.enums.values():
            nums = set()
            for vname, v, _ in e["values"]:
                claim(e["prefix"] + vname, f"enum {e['name']}")
                if not isinstance(v, int):
                    fail(f"{e['prefix']}{vname}: value must be an integer")
                if v in nums:
                    fail(f"{e['prefix']}{vname}: value {v} repeats in enum {e['name']}")
                nums.add(v)
                if e.get("bits") and v & (v - 1) and v not in _bit_masks(e):
                    fail(f"{e['prefix']}{vname}: {v} isn't a bit or a mask of the enum's bits")
        ev_nums = set()
        for name, v, a, b, _ in self.events:
            claim("EV_" + name, "events")
            if v in ev_nums:
                fail(f"EV_{name}: value {v} repeats")
            ev_nums.add(v)
        ids = set()
        wnames = set()
        for wid, name, lv, _, parts in self.widgets:
            claim("W_" + name.upper(), "widgets")
            if wid in ids:
                fail(f"widget {name}: id {wid} repeats")
            ids.add(wid)
            wnames.add(name)
            for p in parts:
                if p not in [v[0] for v in self.enums["part"]["values"]]:
                    fail(f"widget {name}: unknown part {p}")
        ids = set()
        for pid, name, t, applies, access, _ in self.props:
            claim("P_" + name.upper(), "props")
            if not NAME.match(name):
                fail(f"property {name}: names are lower_case")
            if pid in ids:
                fail(f"property {name}: id {pid} repeats")
            ids.add(pid)
            if not (0 < pid < 1000):
                fail(f"property {name}: id must be 1 to 999")
            if not self.type_known(t):
                fail(f"property {name}: unknown type {t}")
            if access not in ("rw", "r", "w"):
                fail(f"property {name}: access must be rw, r or w")
            if isinstance(applies, list):
                for w in applies:
                    if w not in wnames:
                        fail(f"property {name}: unknown widget {w}")
            elif applies not in ("obj", "style"):
                fail(f"property {name}: applies to obj, style or a list of widgets")
        gnames = [g[0] for g in self.groups]
        perms = [p[0] for p in self.raw["manifest"]["permissions"]]
        errs = [v[0] for v in self.enums["error"]["values"]]
        for fn in self.functions:
            n = fn["name"]
            claim(n, "functions")
            if not NAME.match(n):
                fail(f"function {n}: names are lower_case")
            if fn["group"] not in gnames:
                fail(f"function {n}: unknown group {fn['group']}")
            if not n.startswith(_prefix(fn["group"])):
                fail(f"function {n}: functions in {fn['group']} start with {_prefix(fn['group'])}")
            if fn.get("perm") and fn["perm"] not in perms:
                fail(f"function {n}: unknown permission {fn['perm']}")
            for a in fn["args"]:
                if len(a) != 3 or not self.type_known(a[1]):
                    fail(f"function {n}: bad argument {a}")
            if fn.get("ret") and not self.type_known(fn["ret"][0]):
                fail(f"function {n}: unknown return type {fn['ret'][0]}")
            for e in fn.get("errors", []):
                if e not in errs:
                    fail(f"function {n}: unknown error {e}")
            if len(self.signature(fn)) > 24:
                fail(f"function {n}: too many arguments")
        limits = {n: v for n, v, _ in self.raw["limits"]}
        for name, _, fields, actions, events in self.well_known:
            dev = "DEV_" + name.upper()
            claim(dev, "devices")
            if not NAME.match(name):
                fail(f"device {name}: names are lower_case")
            if len(fields) > limits["device_fields"]:
                fail(f"device {name}: more than {limits['device_fields']} fields")
            for f in fields:
                if len(f) != 2 or not NAME.match(f[0]):
                    fail(f"device {name}: bad field {f}")
                claim(f"{dev}_{f[0].upper()}", f"device {name}")
            for act in actions:
                if len(act) != 3 or not NAME.match(act[0]):
                    fail(f"device {name}: bad action {act}")
                claim(f"{dev}_{act[0].upper()}", f"device {name}")
                if len(act[2]) > limits["action_params"]:
                    fail(f"device {name}: action {act[0]} takes more than {limits['action_params']} numbers")
                for prm in act[2]:
                    if len(prm) != 4 or not NAME.match(prm[0]) or prm[2] > prm[3]:
                        fail(f"device {name}: action {act[0]}: bad parameter {prm}")
            for e in events:
                if not NAME.match(e):
                    fail(f"device {name}: bad event {e}")
                claim(f"{dev}_{e.upper()}", f"device {name}")
        for name, cp in self.raw["symbols"]:
            claim("SYM_" + name, "symbols")
            int(cp, 16)
        for e in self.raw["exports"]:
            claim(e["name"], "exports")


def _bit_masks(e):
    return {v for _, v, _ in e["values"]}


def _prefix(group):
    return {"canvas": "cv_", "style": "ui_", "anim": "ui_", "files": "file_",
            "devices": "dev_"}.get(group, group + "_")


# ---------------------------------------------------------------------------
# api.json's own layout: two levels indented, everything deeper on one line.


def canonical_json(raw):
    """Containers longer than a line are spread out, one item per line, except
    rows (lists of plain values, not all strings), which always stay on one
    line."""

    def dump(x, depth):
        flat = json.dumps(x, ensure_ascii=False)
        row = (isinstance(x, list) and not any(isinstance(i, (dict, list)) for i in x)
               and not all(isinstance(i, str) for i in x))
        if not isinstance(x, (dict, list)) or not x or row or len(flat) + 2 * depth <= 100:
            return flat
        pad = "  " * depth
        inner = "  " * (depth + 1)
        if isinstance(x, dict):
            items = [f"{inner}{json.dumps(k)}: {dump(v, depth + 1)}" for k, v in x.items()]
            return "{\n" + ",\n".join(items) + "\n" + pad + "}"
        items = [f"{inner}{dump(v, depth + 1)}" for v in x]
        return "[\n" + ",\n".join(items) + "\n" + pad + "]"

    return dump(raw, 0) + "\n"


# ---------------------------------------------------------------------------
# gadget.h


def wrap(text, width=76, prefix=" * "):
    words, lines, line = text.split(), [], ""
    for w in words:
        if line and len(line) + 1 + len(w) > width:
            lines.append(line)
            line = w
        else:
            line = f"{line} {w}" if line else w
    if line:
        lines.append(line)
    return [prefix + l for l in lines]


def c_comment(text, indent=""):
    if not text:
        return []
    return [indent + "/*"] + [indent + l for l in wrap(text)] + [indent + " */"]


def gen_header(api):
    v = api.version
    out = ["/*", " * gadget.h: the Muse gadget apps API, version %d.%d (%s)." % (v[0], v[1], api.raw["status"]),
           " *", " * Generated from components/muse_apps/sdk/api/api.json by components/muse_apps/sdk/api/gen_api.py. Don't edit it:", " * change api.json and run gen_api.py.",
           " *"]
    out += [" * " + l if l else " *" for l in COPYRIGHT.splitlines()]
    out += [" */", "", "#ifndef GADGET_H", "#define GADGET_H", "", "#include <stdint.h>", "",
            f"#define GADGET_API_MAJOR {v[0]}", f"#define GADGET_API_MINOR {v[1]}", "",
            "#ifdef __wasm__",
            '#define GADGET_IMPORT(name) __attribute__((import_module("gadget"), import_name(#name)))',
            "#define GADGET_EXPORT(name) __attribute__((export_name(#name)))",
            "/* The app's manifest, as one JSON string literal, written to the custom section",
            " * gadget.manifest: GADGET_MANIFEST(\"{\\\"api\\\":\\\"1.0\\\", ...}\"); */",
            '#define GADGET_MANIFEST(json) \\',
            '    __asm__(".section .custom_section.gadget.manifest,\\"\\",@\\n.ascii " #json "\\n.text\\n")',
            "#else",
            "#define GADGET_IMPORT(name)",
            "#define GADGET_EXPORT(name)",
            "#define GADGET_MANIFEST(json) extern int gadget_manifest_unused_",
            "#endif", ""]
    out += c_comment("Conventions. " + " ".join(api.raw["conventions"]))
    out += ["", "/* ---- Values ---- */", ""]
    for name, val, _, doc in api.raw["values"]:
        out.append(f"#define {name} {val}  /* {doc} */" if len(name) + len(val) + len(doc) < 66
                   else f"/* {doc} */\n#define {name} {val}")
    out += ["", "/* ---- Events: on_event(type, a, b) ---- */", ""]
    for name, val, a, b, doc in api.events:
        ab = f"a: {a}" + (f", b: {b}" if b else "")
        out.append(f"#define EV_{name} {val}  /* {ab}. {doc} */")
    out += ["", "/* ---- Constants ---- */"]
    for e in list(api.raw["enums"]) + list(api.raw["ui_enums"]):
        out += [""] + c_comment(e["doc"] or e["name"].replace("_", " ").capitalize() + ".")
        for vname, val, doc in e["values"]:
            line = f"#define {e['prefix']}{vname} ({val})" if val < 0 else f"#define {e['prefix']}{vname} {val}"
            out.append(line + (f"  /* {doc} */" if doc else ""))
    out += ["", "/* Widget types, for ui_create. */"]
    for wid, name, _, doc, _ in api.widgets:
        out.append(f"#define W_{name.upper()} {wid}  /* {doc} */")
    out += ["", "/* Properties, for ui_set, ui_get, ui_set_text, ui_style and ui_animate. */"]
    for pid, name, t, applies, access, doc in api.props:
        where = "any object" if applies == "obj" else "style" if applies == "style" else ", ".join(applies)
        note = f"{t}, {where}" + ("" if access == "rw" else ", read-only" if access == "r" else ", write-only")
        out.append(f"#define P_{name.upper()} {pid}  /* {note}" + (f". {doc}" if doc else "") + " */")
    out += [""] + c_comment("Well-known devices, for dev_find, and where their fields, actions and events are. "
                            "A gadget may have none of these, and devices with any other name: it lists its own.")
    for name, doc, fields, actions, events in api.well_known:
        dev = "DEV_" + name.upper()
        out.append(f'#define {dev} "{name}"  /* {doc} */')
        for i, (f, unit) in enumerate(fields):
            out.append(f"#define {dev}_{f.upper()} {i}  /* field" + (f", {unit}" if unit else "") + " */")
        for i, (act, adoc, params) in enumerate(actions):
            ps = ", ".join(f"{n} ({u}, {lo} to {hi})" for n, u, lo, hi in params)
            out.append(f"#define {dev}_{act.upper()} {i}  /* action: {adoc} Params: {ps}. */")
        for i, e in enumerate(events):
            out.append(f"#define {dev}_{e.upper()} {i}  /* event */")
    out += ["", "/* LVGL's symbols, as UTF-8 strings, for labels in any FONT_ but FONT_48. */"]
    for name, cp in api.raw["symbols"]:
        b = chr(int(cp, 16)).encode()
        out.append(f'#define SYM_{name} "' + "".join(f"\\x{c:02X}" for c in b) + '"')
    out += [""]
    for s in api.raw["structs"]:
        out += c_comment(s["doc"]) + ["typedef struct {"]
        for fname, ftype, fdoc in s["fields"]:
            out.append(f"    {ftype} {fname};  /* {fdoc} */")
        out.append(f"}} {s['name']};")
    out += ["", "/* ---- Functions ---- */"]
    for g, title, gdoc in api.groups:
        out += ["", f"/* == {title} == */"] + c_comment(gdoc)
        for fn in api.functions:
            if fn["group"] != g:
                continue
            doc = fn["doc"]
            args = [f"{a[0]}: {a[2]}" for a in fn["args"] if a[2]]
            if args:
                doc += " " + " ".join(args)
            if fn.get("ret") and fn["ret"][1]:
                rd = fn["ret"][1]
                doc += " Returns " + (rd[0].lower() + rd[1:] if rd[1:2].islower() else rd)
            if fn.get("perm"):
                doc += f" Needs the {fn['perm']} permission."
            out += [""] + c_comment(doc)
            out.append(f"GADGET_IMPORT({fn['name']}) {api.c_ret(fn)} {fn['name']}({api.c_params(fn['args'])});")
    out += ["", "/* == What the app exports == */"]
    for e in api.raw["exports"]:
        params = api.c_params([[a[0], a[1], ""] for a in e["args"]])
        req = "Required. " if e["required"] else ""
        out += [""] + c_comment(f"{req}{e['doc']}") + [f"/* GADGET_EXPORT({e['name']}) void {e['name']}({params}); */"]
    out += ["", "#endif /* GADGET_H */", ""]
    return "\n".join(out)


# ---------------------------------------------------------------------------
# REFERENCE.md


def md_cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def gen_reference(api):
    v = api.version
    r = api.raw
    o = [f"# Muse gadget apps API {v[0]}.{v[1]} ({r['status']})", "",
         "<!-- Generated from api.json by gen_api.py. Don't edit: change api.json. -->", "",
         r["doc"], "", "## Conventions", ""]
    o += [f"- {c}" for c in r["conventions"]]
    o += ["", "## Limits", "", "The same on every gadget that runs apps.", "", "| Limit | Value | |", "|---|---|---|"]
    o += [f"| `{n}` | {val:,} | {md_cell(d)} |" for n, val, d in r["limits"]]
    o += ["", "## What an app exports", "", "| Export | | |", "|---|---|---|"]
    for e in r["exports"]:
        args = ", ".join(a[0] for a in e["args"])
        o.append(f"| `{e['name']}({args})` | {'required' if e['required'] else 'optional'} | {md_cell(e['doc'])} |")
    o += ["", "## Events", "", "`on_event(type, a, b)`:", "", "| Event | a | b | |", "|---|---|---|---|"]
    o += [f"| `EV_{n}` | {md_cell(a)} | {md_cell(b)} | {md_cell(d)} |" for n, _, a, b, d in api.events]
    o += ["", "## Functions", ""]
    for g, title, gdoc in api.groups:
        o += [f"### {title}", "", gdoc, ""]
        for fn in api.functions:
            if fn["group"] != g:
                continue
            o += [f"#### `{fn['name']}`", "", "```c", f"{api.c_ret(fn)} {fn['name']}({api.c_params(fn['args'])});", "```", ""]
            o.append(fn["doc"] + (f" Needs `{fn['perm']}`." if fn.get("perm") else ""))
            rows = [a for a in fn["args"] if a[2]]
            if rows:
                o += [""] + [f"- `{a[0]}`: {a[2]}" for a in rows]
            if fn.get("ret") and fn["ret"][1]:
                o += [f"- Returns: {fn['ret'][1]}"]
            if fn.get("errors"):
                o += ["- Errors: " + ", ".join(f"`ERR_{e}`" for e in fn["errors"])]
            o += [f"- WAMR signature: `{api.signature(fn)}`", ""]
    o += ["## Widgets", "", "| Type | LVGL | Parts | |", "|---|---|---|---|"]
    o += [f"| `W_{n.upper()}` | `{lv}` | {', '.join(parts)} | {md_cell(d)} |" for _, n, lv, d, parts in api.widgets]
    o += ["", "## Properties", "",
          "Object properties (any object) and style properties work with `ui_set` and `ui_get`; style properties also with `ui_style` and `ui_style_set`. Widget properties apply to the widgets named.",
          "", "| Property | Type | Where | Access | |", "|---|---|---|---|---|"]
    for _, n, t, applies, access, d in api.props:
        where = "object" if applies == "obj" else "style" if applies == "style" else ", ".join(applies)
        o.append(f"| `P_{n.upper()}` | {t} | {where} | {access} | {md_cell(d)} |")
    d = api.devices
    o += ["", "## Devices", "", d["doc"], "", "| Key | Type | |", "|---|---|---|"]
    o += [f"| `{n}` | {t} | {md_cell(doc)} |" for n, t, doc in d["descriptor"]]
    o += ["", "For example:", "", "```json", json.dumps(d["example"], ensure_ascii=False), "```", "",
          "### Well-known names", "",
          "A device with one of these names has exactly these fields, actions and events, so an app for it runs on any gadget that has one. Fields are in this order.",
          "", "| Name | Fields | Actions | Events | |", "|---|---|---|---|---|"]
    for name, doc, fields, actions, events in api.well_known:
        fs = ", ".join(f"{f} ({u})" if u else f for f, u in fields) or "—"
        acts = "; ".join(f"{a}({', '.join(f'{n} {u}' for n, u, _, _ in ps)})" for a, _, ps in actions) or "—"
        o.append(f"| `{name}` | {fs} | {acts} | {', '.join(events) or '—'} | {md_cell(doc)} |")
    o += ["", "## Constants", ""]
    for e in list(r["enums"]) + list(r["ui_enums"]):
        vals = ", ".join(f"`{e['prefix']}{n}`" for n, _, _ in e["values"])
        o.append(f"- **{e['name']}**: {vals}." + (f" {e['doc']}" if e["doc"] else ""))
    o += ["", "## Manifest", "", r["manifest"]["doc"], "", "| Field | Type | Required | |", "|---|---|---|---|"]
    o += [f"| `{n}` | {t} | {'yes' if req else 'no'} | {md_cell(d)} |" for n, t, req, d in r["manifest"]["fields"]]
    o += ["", "Features: " + ", ".join(f"`{n}` ({d})" for n, d in r["manifest"]["features"]) + ".", "",
          "Permissions: " + ", ".join(f"`{n}` ({d})" for n, d in r["manifest"]["permissions"]) + ".", ""]
    return "\n".join(o)


# ---------------------------------------------------------------------------
# muse_apps_api.h, the firmware's tables


def native_params(api, args):
    """The firmware function's parameters, as WAMR passes them: a *~ buffer as
    a pointer it has checked and its length."""
    out = ["wasm_exec_env_t env"]
    for name, t, _ in args:
        c = api.types[t]["c"] if t in api.types else "int32_t"
        if isinstance(c, list):
            out += [f"{c[0]}{name}", f"uint32_t {name}_{'size' if t == 'out' else 'len'}"]
        else:
            out.append(f"{c}{'' if c.endswith('*') else ' '}{name}")
    return ", ".join(out)


def gen_firmware(api):
    v = api.version
    o = ["/*", " * muse_apps_api.h: the firmware's side of the apps API %d.%d." % (v[0], v[1]),
         " * Generated from components/muse_apps/sdk/api/api.json by gen_api.py. Don't edit it.", " *"]
    o += [" * " + l if l else " *" for l in COPYRIGHT.splitlines()]
    o += [" */", "", "#pragma once", "", f"#define MUSE_APPS_API_MAJOR {v[0]}", f"#define MUSE_APPS_API_MINOR {v[1]}", "",
          "/* Every import: GADGET_FN(name, WAMR signature, permission or NULL). */", "#define MUSE_APPS_FUNCTIONS(GADGET_FN) \\"]
    o += [f"    GADGET_FN({fn['name']}, \"{api.signature(fn)}\", " + (f"\"{fn['perm']}\"" if fn.get("perm") else "NULL") + ") \\"
          for fn in api.functions]
    o += ["", "", "/* The API's constants, as apps' gadget.h names them, with GADGET_ in front. */"]
    for name, val, _, _ in api.raw["values"]:
        o.append(f"#define GADGET_{name} {val}")
    for name, val, _, _, _ in api.events:
        o.append(f"#define GADGET_EV_{name} {val}")
    for e in list(api.raw["enums"]) + list(api.raw["ui_enums"]):
        for vname, val, _ in e["values"]:
            o.append(f"#define GADGET_{e['prefix']}{vname} ({val})")
    for wid, name, _, _, _ in api.widgets:
        o.append(f"#define GADGET_W_{name.upper()} {wid}")
    for pid, name, _, _, _, _ in api.props:
        o.append(f"#define GADGET_P_{name.upper()} {pid}")
    for name, _, fields, actions, events in api.well_known:
        dev = "GADGET_DEV_" + name.upper()
        o.append(f'#define {dev} "{name}"')
        for i, (f, _) in enumerate(fields):
            o.append(f"#define {dev}_{f.upper()} {i}")
        for i, (a, _, _) in enumerate(actions):
            o.append(f"#define {dev}_{a.upper()} {i}")
        for i, e in enumerate(events):
            o.append(f"#define {dev}_{e.upper()} {i}")
    o += ["", "#ifdef MUSE_APPS_NATIVES",
          "#include \"wasm_export.h\"", "",
          "/* Each import's implementation: gadget_<name>, in the module for its group. */"]
    for fn in api.functions:
        o.append(f"{api.c_ret(fn)} gadget_{fn['name']}({native_params(api, fn['args'])});")
    o += ["#endif", "", "#ifdef MUSE_APPS_DEFINE_STUBS",
          "/* Weak defaults: a function no module implements yet answers ERR_UNSUPPORTED",
          " * (or nothing). The component links whole, so a module's own definition wins. */"]
    unsupported = [v for v in api.enums["error"]["values"] if v[0] == "UNSUPPORTED"][0][1]
    for fn in api.functions:
        names = ["env"] + [x for a in fn["args"] for x in
                           ([a[0], a[0] + ("_size" if a[1] == "out" else "_len")] if a[1] in ("bytes", "out") else [a[0]])]
        voids = " ".join(f"(void){n};" for n in names)
        ret = api.c_ret(fn)
        body = f"{voids}" + ("" if ret == "void" else f" return {unsupported};")
        o.append(f"__attribute__((weak)) {ret} gadget_{fn['name']}({native_params(api, fn['args'])}) {{ {body} }}")
    o += ["#endif"]
    o += ["", "", "/* GADGET_WIDGET(id, name, LVGL class) */", "#define MUSE_APPS_WIDGETS(GADGET_WIDGET) \\"]
    o += [f"    GADGET_WIDGET({wid}, {n}, {lv}_class) \\" for wid, n, lv, _, _ in api.widgets]
    o += ["", "", "/* GADGET_PROP(id, name, type, applies to: obj, style or widget, access) */", "#define MUSE_APPS_PROPS(GADGET_PROP) \\"]
    for pid, n, t, applies, access, _ in api.props:
        kind = applies if isinstance(applies, str) else "widget"
        o.append(f"    GADGET_PROP({pid}, {n}, {t}, {kind}, {access}) \\")
    o += ["", "", "/* Well-known devices: a driver registering one of these names must match it.",
          " * GADGET_DEV(name, \"fields\", \"actions\", \"events\") */", "#define MUSE_APPS_WELL_KNOWN(GADGET_DEV) \\"]
    for name, _, fields, actions, events in api.well_known:
        o.append(f'    GADGET_DEV({name}, "{",".join(f for f, _ in fields)}", "{",".join(a for a, _, _ in actions)}", '
                 f'"{",".join(events)}") \\')
    o += ["", ""]
    return "\n".join(o)


def gen_capabilities(api):
    """What app.api serves: the machine-readable part of api.json, without prose."""
    r = api.raw
    capabilities = {
        "api": f"{api.version[0]}.{api.version[1]}",
        "limits": {n: val for n, val, _ in r["limits"]},
        "functions": {fn["name"]: api.signature(fn) for fn in api.functions},
        "events": {n: val for n, val, _, _, _ in api.events},
        "widgets": {n: wid for wid, n, _, _, _ in api.widgets},
        "props": {n: [pid, t, a if isinstance(a, str) else a, acc] for pid, n, t, a, acc, _ in api.props},
        "well_known": {n: {"fields": [f for f, _ in fs], "actions": [a for a, _, _ in acts], "events": evs}
                       for n, _, fs, acts, evs in api.well_known},
        "constants": {e["prefix"] + n: val for e in list(r["enums"]) + list(r["ui_enums"]) for n, val, _ in e["values"]},
    }
    return json.dumps(capabilities, separators=(",", ":"), ensure_ascii=False) + "\n"


# ---------------------------------------------------------------------------


def outputs(api, raw_text):
    return {
        "api.json": canonical_json(api.raw),
        "../gadget.h": gen_header(api),
        "REFERENCE.md": gen_reference(api),
        "../../muse_apps_api.h": gen_firmware(api),
        "api.capabilities.json": gen_capabilities(api),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if a generated file is out of date")
    ap.add_argument("--dir", type=Path, default=HERE, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    src = args.dir / "api.json"
    text = src.read_text()
    try:
        api = Api(json.loads(text))
    except (ApiError, KeyError, ValueError, TypeError) as e:
        print(f"api.json: {e}", file=sys.stderr)
        return 1
    stale = []
    for name, content in outputs(api, text).items():
        path = args.dir / name
        if path.exists() and path.read_text() == content:
            continue
        if args.check:
            stale.append(name)
        else:
            path.write_text(content)
            print(f"wrote {path.resolve()}")
    if stale:
        print("out of date: " + ", ".join(stale) + " (run components/muse_apps/sdk/api/gen_api.py)", file=sys.stderr)
        return 1
    if not args.check:
        n = len(api.functions)
        print(f"{n} functions, {len(api.widgets)} widgets, {len(api.props)} properties, {len(api.well_known)} well-known devices; "
              f"api.capabilities.json {len(gen_capabilities(api))} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
