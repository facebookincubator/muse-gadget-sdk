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
"""The apps API's UI modules (components/muse_apps/muse_apps_ui.c and
muse_apps_canvas.c) against components/muse_apps/sdk/api/api.json: every ui_, style, animation and
canvas function defined with the generated prototype, every property, widget,
event and easing handled, and their memory kept out of internal RAM."""

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
API = json.loads((ROOT / "components/muse_apps/sdk" / "api" / "api.json").read_text())
APPS = ROOT / "components" / "muse_apps"
UI = (APPS / "muse_apps_ui.c").read_text()
CANVAS = (APPS / "muse_apps_canvas.c").read_text()
GENERATED = (APPS / "muse_apps_api.h").read_text()
VIEW_C = (ROOT / "components" / "muse" / "muse_app_view.c").read_text()
VIEW_H = (ROOT / "components" / "muse" / "muse_app_view.h").read_text()

UI_GROUPS = {"ui", "style", "anim"}


def function(source, signature):
    """The body of the C function whose definition starts with signature."""
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0
    for i in range(open_brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace:i + 1]
    raise AssertionError("unterminated " + signature)


def ui_enum(name):
    return next(e for e in API["ui_enums"] if e["name"] == name)


def c_table(source, name):
    """The entries of `static const uint8_t name[] = {...};`."""
    m = re.search(r"static const \w+ %s\[\] = \{(.*?)\};" % re.escape(name), source, re.S)
    assert m, name
    return [x.strip() for x in m.group(1).split(",") if x.strip()]


class ApiCoverage(unittest.TestCase):
    def test_every_function_is_defined_with_its_prototype(self):
        prototypes = {name: (ret, args) for ret, name, args in
                      re.findall(r"^(int32_t|int64_t|void) gadget_(\w+)\((.*?)\);$", GENERATED, re.M)}
        for f in API["functions"]:
            if f["group"] not in UI_GROUPS | {"canvas"}:
                continue
            name = f["name"]
            self.assertIn(name, prototypes, name)
            ret, args = prototypes[name]
            definition = (re.escape(ret) + r"\s+gadget_" + name + r"\(" +
                          r",\s*".join(re.escape(a.strip()) for a in args.split(",")) + r"\)\s*\{")
            source = CANVAS if f["group"] == "canvas" else UI
            self.assertRegex(source, definition, "%s isn't defined as muse_apps_api.h declares it" % name)

    def test_the_other_modules_functions_stay_out(self):
        for f in API["functions"]:
            if f["group"] in UI_GROUPS | {"canvas"}:
                continue
            self.assertNotIn("gadget_%s(" % f["name"], UI + CANVAS, f["name"])

    def test_every_property_is_dispatched(self):
        for pid, name, ptype, where, access, _ in API["props"]:
            const = "GADGET_P_" + name.upper()
            if where == "style":
                # The style table maps it to an LVGL style property.
                self.assertRegex(UI, r"\[%s - 100\] = \{LV_STYLE_\w+, K_\w+\}" % const, const)
            elif ptype == "text":
                self.assertIn("case %s:" % const, function(UI, "static const char *text_get("), const)
            else:
                getter = "obj_get" if where == "obj" else "widget_get"
                setter = "obj_set" if where == "obj" else "widget_set"
                if "r" in access:
                    self.assertIn("case %s:" % const, function(UI, "static int32_t %s(" % getter), const)
                if "w" in access:
                    self.assertIn("case %s:" % const, function(UI, "static int32_t %s(" % setter), const)
        self.assertEqual(132, len(API["props"]))

    def test_property_metadata_comes_from_the_generated_table(self):
        self.assertIn("MUSE_APPS_PROPS(GADGET_PROP)", function(UI, "static int prop_info("))
        types = {p[2] for p in API["props"]}
        for t in types:
            self.assertRegex(UI, r"\bPT_%s\b" % t, t)

    def test_every_widget_is_created(self):
        body = function(UI, "static lv_obj_t *create(")
        for wid, name, lvgl, _, _ in API["widgets"]:
            self.assertIn("case GADGET_W_%s: return %s_create(parent);" % (name.upper(), lvgl), body, name)
        self.assertEqual(19, len(API["widgets"]))

    def test_every_ui_event_is_sent(self):
        events = function(UI, "static void on_ui_event(")
        self.assertIn("muse_apps_post(GADGET_EV_UI", events)
        table = re.search(r"s_events\[\] = \{(.*?)\};", UI, re.S).group(1)
        for name, _, _ in ui_enum("ui_event")["values"]:
            const = "GADGET_UI_" + name
            if name in ("ANIM_DONE", "LOADED", "LOAD_FAILED"):
                self.assertRegex(UI, r"muse_apps_post\(GADGET_EV_UI, [^;]*%s" % const, const)
            else:
                self.assertRegex(table, r"\{%s, LV_EVENT_%s\}" % (const, name), const)

    def test_easings_and_flags(self):
        paths = re.search(r"s_paths\[\] = \{(.*?)\};", UI, re.S).group(1)
        lvgl = {"LINEAR": "linear", "IN": "ease_in", "OUT": "ease_out", "IN_OUT": "ease_in_out"}
        for name, _, _ in ui_enum("easing")["values"]:
            self.assertRegex(paths, r"\[GADGET_EASE_%s\] = lv_anim_path_%s\b" % (name, lvgl.get(name, name.lower())),
                             name)
        animate = function(UI, "int32_t gadget_ui_animate(")
        self.assertIn("GADGET_ANIM_REPEAT", animate)
        self.assertIn("GADGET_ANIM_REVERSE", animate)

    def test_enum_tables_follow_the_api_order(self):
        tables = {"s_layouts": "layout", "s_flows": "flex_flow", "s_grads": "grad_dir", "s_long_modes": "long_mode",
                  "s_bar_modes": "bar_mode", "s_orients": "orientation", "s_arc_modes": "arc_mode",
                  "s_kb_modes": "keyboard_mode", "s_scale_modes": "scale_mode", "s_chart_types": "chart_type",
                  "s_chart_updates": "chart_update", "s_image_aligns": "image_align"}
        for table, enum in tables.items():
            values = ui_enum(enum)["values"]
            entries = c_table(UI, table)
            self.assertEqual(len(values), len(entries), table)
            self.assertEqual(list(range(len(values))), [v[1] for v in values], enum)
            for (name, _, _), entry in zip(values, entries):
                last = name.split("_")[-1]
                self.assertIn(last, entry.replace("TEXT_LOWER", "LOWER").replace("TEXT_UPPER", "UPPER"),
                              "%s: %s maps to %s" % (table, name, entry))

    def test_hooks(self):
        for hook in ("void muse_apps_ui_start(void)", "void muse_apps_ui_stop(void)"):
            self.assertIn(hook + "\n{", UI)
        self.assertIn("void muse_apps_canvas_stop(void)", CANVAS)
        stop = function(UI, "void muse_apps_ui_stop(void)")
        # Objects go first (each frees what it held), then the shared styles.
        self.assertLess(stop.index("lv_obj_clean(root)"), stop.index("lv_style_reset"))
        self.assertIn("s_session++", stop)

    def test_natives_take_lvgls_lock(self):
        for f in API["functions"]:
            if f["group"] not in UI_GROUPS | {"canvas"} or f["name"] == "ui_span":   # ui_span: the board's geometry
                continue
            source = CANVAS if f["group"] == "canvas" else UI
            body = function(source, "gadget_%s(wasm_exec_env_t env" % f["name"])
            self.assertRegex(body, r"muse_apps_ui_lock\(\)|obj_arg\(|style_arg\(|chart_arg\(|canvas_arg\(|"
                             r"return rect\(", f["name"])
        self.assertIn("canvas_arg(", function(CANVAS, "static int32_t rect("))


class Memory(unittest.TestCase):
    def test_no_large_static_buffers(self):
        for source in (UI, CANVAS):
            for m in re.finditer(r"^static\s+(?!const)([\w ]+?)\s+\**(\w+)\[(\w+)\]\s*[;=]", source, re.M):
                size = m.group(3)
                count = int(size) if size.isdigit() else int(re.search(r"#define %s (\d+)" % size, source).group(1))
                self.assertLess(count, 512, "static %s[%s] belongs in PSRAM" % (m.group(2), size))

    def test_app_memory_is_counted(self):
        self.assertIn("muse_apps_calloc(1, sizeof(ui_t))", UI)   # the table
        self.assertIn("muse_apps_charge(s_cost[type])", UI)       # each object
        self.assertIn("muse_apps_charge(img->bytes)", UI)         # images
        self.assertIn("muse_apps_alloc((size_t)stride", CANVAS)  # canvases
        self.assertNotRegex(UI + CANVAS, r"\bmalloc\(|MALLOC_CAP_INTERNAL")

    def test_images_and_canvases_share_the_screens_pixels(self):
        self.assertIn("muse_apps_ui_pixels_take(img->pixels)", UI)
        self.assertIn("muse_apps_ui_pixels_take(now)", CANVAS)
        self.assertIn("ui->pixel_limit = (uint32_t)(w * h)", UI)


class CanvasStyles(unittest.TestCase):
    def test_styled_rectangles_use_bounded_style_properties(self):
        styled = function(CANVAS, "int32_t gadget_cv_rect_style(")
        self.assertIn("muse_apps_ui_style(style)", styled)
        self.assertIn("rect_style(st, &dsc)", styled)
        self.assertIn("lv_draw_rect(&layer, &dsc, &a)", styled)
        for field in ("border_width", "shadow_width"):
            self.assertIn("dsc.%s = clamp(dsc.%s, 0, 64)" % (field, field), styled)
        self.assertIn("dsc.shadow_spread = clamp(dsc.shadow_spread, 0, 64)", styled)
        self.assertIn("w > c->w || h > c->h", styled)
        mapping = function(CANVAS, "static void rect_style(")
        for prop in ("BG_GRAD_COLOR", "BG_GRAD_DIR", "SHADOW_COLOR", "SHADOW_WIDTH",
                     "SHADOW_OPA", "SHADOW_OFFSET_X", "SHADOW_OFFSET_Y", "SHADOW_SPREAD"):
            self.assertIn(prop, mapping)
        self.assertNotIn("TEXT_FONT", mapping)
        self.assertIn("muse_apps_ui_style(int32_t handle)", (APPS / "muse_apps_ui_priv.h").read_text())


class Images(unittest.TestCase):
    def test_decoders_are_on_with_apps(self):
        overlay = (ROOT / "devices" / "sdkconfig.muse-apps").read_text()
        self.assertIn("CONFIG_LV_USE_LODEPNG=y", overlay)
        self.assertIn("CONFIG_LV_USE_TJPGD=y", overlay)

    def test_loads_run_off_the_app_task(self):
        load = function(UI, "int32_t gadget_ui_image_load(")
        self.assertIn("muse_apps_vm_path(", load)
        self.assertIn("pthread_create(", load)
        self.assertIn("MALLOC_CAP_SPIRAM", load)
        loader = function(UI, "static void *loader(")
        self.assertIn("muse_apps_vm_file_read(", loader)
        # The app and its object are checked again under the lock before the image is shown.
        self.assertRegex(loader, r"muse_app_view_lock\(\);\s*if \(s_session == job->session\)")
        self.assertIn("GADGET_UI_LOAD_FAILED : GADGET_UI_LOADED", loader)


class Navigation(unittest.TestCase):
    def test_the_view_hands_navigation_keys_over(self):
        self.assertIn("void muse_app_view_set_nav(muse_app_view_nav_fn fn);", VIEW_H)
        self.assertIn("atomic_store(&s_nav, NULL);", function(VIEW_C, "void muse_app_view_close(void)"))
        buttons = function(VIEW_C, "unsigned muse_app_view_buttons(unsigned ev)")
        self.assertLess(buttons.index("nav(keys[i].button)"), buttons.index("button_edge(keys[i].button, true, true)"))
        nav = function(UI, "int32_t gadget_ui_navigation(")
        self.assertIn("muse_app_view_set_nav(nav_button)", nav)
        self.assertIn("muse_app_view_set_nav(NULL)", function(UI, "static void nav_off("))


if __name__ == "__main__":
    unittest.main()
