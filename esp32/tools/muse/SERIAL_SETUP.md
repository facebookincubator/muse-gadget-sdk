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

# USB settings with Web Serial

Read a Muse device's complete status and edit its display sleep delay,
brightness, speaker volume, speaker enable and microphone gain in a browser.
The page uses the existing `>status` and settings commands; no firmware change
is required. This USB page is separate from `ble_setup.html` and the web flasher.

It works with boards that run the full Muse UI (`CONFIG_MUSE_ENABLED`), whose
console takes these commands. Link-only builds, such as the default DevKitC-1,
don't answer `>status`, so the page times out on them.

English is the default on every load. The Language picker switches to Japanese
without reconnecting or discarding unsaved edits. The language is not persisted.

## Start

From the repository's `esp32` directory:

```sh
python3 -m http.server 8765 --bind 127.0.0.1 --directory tools/muse
```

Open [the USB settings page](http://localhost:8765/serial_setup.html) in desktop
Chrome and choose **Connect via USB**, then select the device's console port.
Web Serial requires localhost or HTTPS; see the
[Chrome Web Serial guide](https://developer.chrome.com/docs/capabilities/serial).

1. Connecting reads the current settings and status without changing settings.
2. Edit the controls and choose **Save changes**. The page checks both the
   command acknowledgement and a readback before reporting success.
3. Choose **Disconnect** to release the port before using another serial monitor.

| Setting | Allowed values |
| --- | --- |
| Auto-sleep delay | `0` keeps the display on; `1`–`3600` seconds |
| Display brightness | `10`–`100` percent |
| Speaker volume | `0`–`100` percent |
| Speaker enable | On or off |
| Microphone gain | `0`–`36` dB |

The device persists settings across restarts. **Wake display** sends only the
wake command and preserves unsaved edits. The page does not record audio,
start calls, add TTS, change Wi-Fi/pairing, accept SDK tokens or flash firmware.

## Complete device status

Every field returned by `>status` is displayed read-only, including board,
firmware, chat state, Wi-Fi, Muse, Link, battery, settings and the last command
result. Nested objects are flattened into key paths. Unknown future keys,
`false`, `0`, `null`, empty strings and empty arrays/objects are preserved.
The original JSON is available in an expandable section.

Labels, controls, notices and activity messages are localized. Status keys and
device-reported values remain unchanged. Status can include SSIDs and IP
addresses; consider that when sharing screenshots. The page sends no data to
external services and uses no browser storage, CDN or external JavaScript.
The activity list excludes raw console output and conversation text.

## Connection help

Use a USB data cable. Connecting may restart the device. If the page gets no
response, close other serial monitors and reconnect USB. After unplugging a
connected device, reconnect from the page. An unacknowledged write or mismatched
readback is reported as an error rather than a successful save.

## Tests

With Node.js 22 or newer, from `esp32`:

```sh
node --test tools/muse/serial_settings.test.mjs tools/muse/serial_status.test.mjs
```

The tests cover split UTF-8/JSON, response ordering, readback mismatch, invalid
settings, disconnects, stalled writes, timeouts and lock release. Status tests
check all current fields, unknown keys, translation parity and English defaults.
