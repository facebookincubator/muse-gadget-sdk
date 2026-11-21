<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Optional UI appearance

`CONFIG_MUSE_UI_THEME` adds Settings → Appearance → Theme to the LVGL UI.
It is off by default and independent of avatar synchronization or video.
Existing builds keep their dark scheme. FreeInk and headless profiles exclude
this option.

The `devices/sdkconfig.muse-ui-theme` fragment enables it after a full-UI board
overlay. CI builds a Waveshare 1.75C profile with this fragment and no avatar
extension, in addition to the existing profiles where the option is off.

The choices are Automatic, Dark, and Light. Automatic is the upgrade default;
without a content hint it uses the original dark appearance. Explicit Dark or
Light overrides every hint. The preference persists in the `ui_theme_v1` NVS
key. An internal-stack worker saves it outside the LVGL task, coalesces rapid
changes, and rolls the requested setting back if the write fails.

Native home controls, Settings, text entry/keypad, option sheets, latest reply,
and system menus share semantic color styles. A palette change updates those
styles without recreating widgets or clearing input/scroll state. User apps'
explicit colors, media pixels, and the black sleep cover retain their owners.
The bundled character has an optional background hook; legacy custom C avatars
remain source-compatible and can implement that hook to adapt their backdrop.

Content providers can call `muse_theme_set_hint(valid, rgb)` from any task.
The UI consumes a stable hint on its next tick. Automatic chooses the light or
dark palette using relative luminance and can match the content's exact solid
background. Foreground styles adapt for contrast. This API does not fetch or
inspect content; provider integration belongs in a separate change.

The developer console accepts `theme`, `theme=auto`, `theme=dark`, and
`theme=light`. The status includes only the appearance preference, optional
background color, and save error. It does not alter screen sleep or brightness.

Standalone validation includes a Waveshare 1.75C firmware build without avatar
code, palette/contrast and NVS-failure host checks, and ASan/UBSan simulator
checks for live palette changes, bounded style lists, sleep, 466/240-pixel
settings, and preserving an unfinished text field during a theme change.
