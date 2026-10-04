# Native Linux LVGL conversation app

`muse_linux_app` is a native C executable. LVGL draws the complete desktop UI;
SDL2 supplies its Linux window, keyboard and mouse. It reuses the SDK's live
procedural mascot renderer and includes a spin button. Muse AI runs remotely
through an enrolled Linux SDK connector, with streamed text replies displayed
in the app. The app starts a fresh side conversation each time it opens.

## Open on this Mac

Double-click `Launch Muse Linux.command`, or run:

```sh
python3 linux/lvgl/launch.py
```

The launcher uses Docker Desktop and XQuartz, the `muse-linux-lvgl:local` image,
and the already enrolled `muse-ma35d1-a1-sim` connector. The executable runs in
Linux and its X11 window appears on the Mac. Type a message and press Enter or
Send. Close the window or press Esc to exit. Set `MUSE_LVGL_CONNECTOR` to use a
different enrolled, chat-enabled container.

The main chat's **Mic (5s)** button records and sends a WAV voice note directly
to Muse. Speak during the five-second recording; the reply is spoken
automatically when it finishes. No separate transcript or Send step is needed.
The main chat's **Camera** button captures a still photo, uploads the JPEG to
Muse and displays its analysis. Each capture is explicitly requested; the
camera does not stream continuously or upload in the background.

The executable and UI run in Linux. Docker Desktop does not expose this Mac's
camera and microphone as Linux devices, so `host_media.py` captures them on
macOS and exchanges files with the Linux app through a private mounted folder.
The launcher starts that helper automatically. Capture happens when requested;
the camera demonstration is a still frame, not a continuous video feed. Audio
playback, speech output and optional transcription also run on the Mac.
Media files remain under ignored `.local/media/`.

The helper needs FFmpeg on the Mac. Its default camera is AVFoundation device
0 and microphone is audio device 1; `host_media.py --camera N --microphone N`
can select different devices. The launcher uses the existing
`mimo-music-lab/tools/listen-env/bin/python` environment for Faster Whisper when
available; transcription reports an explicit error otherwise.

XQuartz must accept network clients on display :0. The launcher copies its
private authorization cookie; it does not disable X11 access controls. Muse
credentials stay inside the connector. A random bridge token under ignored
`.local/` authorizes the app over the private Docker network; no bridge port
is published to the host.

The XQuartz launch disables SDL framebuffer acceleration. This keeps SDL from
trying to create a GLX context on the remote display while LVGL draws in software.

## Build directly on Linux

Install a C compiler, CMake 3.24+, Ninja, Python 3 and SDL2 development headers.
From the repository root:

```sh
cmake -S esp32/simulator -B esp32/simulator/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build esp32/simulator/build --target muse_linux_app --parallel
./esp32/simulator/build/muse_linux_app
```

The standalone UI opens without an account. To enable conversation, run
`bridge.py --token-file FILE` alongside a chat-enabled `musegadget` service.
Set `MUSE_CHAT_HOST`, `MUSE_CHAT_PORT` (default 8765), and `MUSE_CHAT_TOKEN_FILE`
for the GUI. The token file contains 64 random hexadecimal characters and
should have mode 0600. The bridge defaults to the SDK's local Unix socket
`/run/musegadget/musegadget.sock`; use `--socket` to select another. Bind it to
`--host 127.0.0.1` when the GUI and bridge run directly on the same Linux host.
Install the SDK from this repository's `linux/` directory to enable streamed
replies and attachment forwarding. Voice notes use the same `items` file format
as the ESP32 SDK, with `mime_type: audio/wav` and `data_base64`. Replies remain
text; automatic speech is a local playback integration.

The Dockerfile reuses the local `muse-mascot-x11:local` runtime and the pinned
LVGL source already fetched in `esp32/simulator/build-spin/_deps/lvgl-src`.
It is intended for this workspace's existing simulator installation. The
direct CMake build above fetches its own pinned dependencies on a fresh system.

## Checks

```sh
ctest --test-dir esp32/simulator/build --output-on-failure
python3 -m unittest discover -s linux/lvgl -p 'test_*.py'
```

The bridge tests use a fake local SDK socket to check coalesced replies,
authorization, UUID validation, size limits and cancellation. The CMake app
smoke test renders the native UI headlessly. For an actual end-to-end check:

```sh
./esp32/simulator/build/muse_linux_app --headless --run-ms 35000 \
  --prompt 'Hello, Muse. Say a short hello.' --screenshot live.ppm
```

A successful live turn prints `CHAT_DONE` and the assistant's reply. An error
prints `CHAT_ERROR` with the diagnostic. The same prompt can be entered in
the visible app. Set `MUSE_MEDIA_DIR` to the media folder when running outside
Docker. Use `--voice-file FILE.wav` to check direct audio input with a saved
16 kHz mono WAV shorter than five seconds. `--camera-file FILE.jpg` checks
photo upload and analysis with a saved JPEG.
