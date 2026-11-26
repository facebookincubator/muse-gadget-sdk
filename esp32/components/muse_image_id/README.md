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

# muse_image_id: which board an image is for

Every image this firmware builds carries a 64-byte `muse_board_desc_t`
(`include/muse_image_id.h`): the chip, the product code
(`CONFIG_HOMEHUB_BLE_PRODUCT_CODE`, e.g. W175, SCW1, M5SW, WC6, EMO1), the
board profile (`CONFIG_MUSE_BOARD_ID`, e.g. `waveshare_s3_175c`; `none` on
Link-only builds) and flags (musegadgets BLE audio, BLE updates).

It lives in ESP-IDF's custom app description section, the documented way
to add a structure to the app image (ESP-IDF v6.0.1, App Image Format,
"Adding a Custom Structure to an Application"):
`__attribute__((section(".rodata_custom_desc")))`. The linker scripts place
that section right after `esp_app_desc_t` at the start of the first
segment, so in the `.bin` it sits at a fixed offset: 24 bytes of image
header, 8 of segment header, 256 of `esp_app_desc_t`, then this, at 288.
`-u muse_board_desc` keeps it in images that never reference it.
`esp_app_desc_t` itself has no field for a board (`project_name` is
`muse-gadget` everywhere and `version` is the release string), and
`CONFIG_APP_PROJECT_VER_FROM_CONFIG` only changes where the version comes
from.

`muse_image_check()` reads the first `MUSE_IMAGE_HEAD_BYTES` (352) of a
download and says whether the image is for this chip and, when asked, this
board (same product code and board profile, and BLE updates if this build
takes them), before any flash write. Its callers:

- `main/ota.c` (Wi-Fi OTA, from Link setup): only with
  `CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD` on a Muse Gadget build; a gadget
  build otherwise refuses Wi-Fi OTA outright.
- `components/muse_gadget_ble/mg_smp_esp.c` (BLE updates, MCUmgr SMP):
  every upload.

An image built before the description existed reads as "no board
description" and is refused wherever the board is checked: install such a
build over USB.

`tests/test_muse_image_id.py` runs the check on the host.
