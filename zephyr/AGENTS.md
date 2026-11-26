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

# AGENTS.md

How to build this firmware, test it, and flash it onto a Seeed XIAO nRF54L15.
See `README.md` for a shorter human overview.

## What this is

The Zephyr Device SDK: a freestanding Zephyr application for a push-to-talk
BLE gadget speaking the musegadgets protocol (`../protocols/mgcommands.h`),
set up by the Muse app over Muse Link setup (`../protocols/README.md`, Gadget
setup over Muse Link). Include the protocol header; never copy its
constants. The build adds `../protocols` to the include path and compiles
`../esp32/main/pairing_transcript.c`, so build from a full checkout. The
Bluetooth link isn't encrypted (`CONFIG_BT_SMP=n`): the device session is
sealed under keys from the token proof (Device session, below), and the rest
is plaintext musegadgets.

| File | Role |
|---|---|
| `src/main.c` | Boot order, the app work queue, battery polling |
| `src/mg_ota.c`, `src/mg_ota_policy.h` | Firmware updates: MCUmgr hooks, when a test image is kept, the orderly SMP reset, handing the clip slot to OTA and back |
| `src/mg_smp_gate.c`, `src/mg_smp_req_gen.c` | SMP over Bluetooth without MCUboot updates (`CONFIG_MG_SMP_GATE_BOARD`): the token-proof gate, then the board's own; each request's source connection |
| `src/mg_smp_mem.c`, `src/zephyr/smp.c` | Firmware updates take no base memory: SMP's buffers, work queue and image writer from the malloc arena only while SMP is in use; `smp.c` is a marked copy of Zephyr's, built in its place |
| `src/mg_bench.c` | Bench console (`CONFIG_MG_BENCH`, every build): single-key commands and lines on the console UART; `>pair.confirm` presses only in bench builds (`CONFIG_MG_AUTOMATION`, `overlay-bench.conf`) |
| `src/mg_core.c` | Protocol engine: commands, push-to-talk, settings and queue commands |
| `src/mg_session.c` | Between GATT and the core: Control to the core, readiness |
| `src/mg_setup.c` | Muse Link setup: the setup service's framing, dispatch, statuses, community pairing v5 phases and timeouts, button confirmation, get_device_info, token-only provision_v2 |
| `src/mg_setup_crypto.c`, `src/mg_psa.c` | Link setup crypto (P-256 ECDH, session keys, AES-256-GCM records, base64url) and SHA-256 / HMAC / HKDF, on PSA Crypto |
| `src/mg_setup_store.c` | What setup stores: tokens, proof key K and the commit record in settings (ZMS), boot recovery |
| `src/mg_token_proof.c` | mg_command_token_proof (34): challenge, response, confirm, result, clear |
| `src/mg_vm_session.c` | mg_command_session (0x89), the device side: hello gating, credentials (sealed only), state, data fragments, credit, credentials_update |
| `src/mg_seal_crypto.c` | The shared seal's two crypto calls (`../portable/mg_device_session`) on PSA |
| `src/mg_vm_link.c` | The gadget's end of its VM session: POST /link-control, link.register, link.invoke -> link.result, link.heartbeat |
| `src/mg_vm_commands.c` | The board's device commands (device.health, device.led) and the identity it registers with |
| `src/mg_envelope.c` | ServiceRequest/ResponseEnvelopes and service frames, byte for byte as `esp32/components/noise_core` (ServiceCodec.cpp) |
| `src/mg_json.c` | The strict JSON reader and writer (flat objects, nested members as raw text; nested writing) |
| `src/mg_identity.c` | Name, node id, MAC and device id from the identity address |
| `src/mg_ble.c` | GATT service, advertising, connection, PHY / data length, the ordered notification queue |
| `src/mg_connparam.c` | Link modes (settings link_mode 16 / link_params 17): ready 24/36/5/400 at connect, active 12/12/0/400 while busy, sleep 100/160/4/600 |
| `src/mg_throughput.c` | The throughput test (device_action throughput_test): send and receive |
| `src/mg_audio.c` | Audio thread: capture, encode, packetize, offline recording, clip download |
| `src/mg_audio_dmic.c`, `src/mg_audio_synth.c` | PDM microphone, or a 1 kHz test tone |
| `src/mg_codec.c` | SBC and LC3 encoders behind one interface |
| `src/mg_hpf.c` | The microphone's DC-blocking high-pass |
| `src/mg_queue.c` | Offline clip storage: MCUboot's `slot1_partition` on the XIAO (shared with OTA), `audio_queue_partition` elsewhere |
| `src/mg_settings.c` | Settings validation, wire encoding, persistence (Zephyr settings, ZMS) |
| `src/mg_nus.c` | Nordic UART: log mirror and console: reads (`status`, `version`, `settings`, `queue`, `battery`, `reset`) and the state-changing `bench`, `listen` and `mode` (Mic output only to a proven central, below); never blocks: a 2 KB ring under a spinlock that drops and counts when full, one notification in flight (its sent callback counts it), nothing while the data path has notifications queued; `status` and the bench `s` give the drops; a new subscriber gets only what's logged from its subscription on (the ring is emptied, never handed to the next central) |
| `src/mg_clock.c`, `src/mg_power.c`, `src/mg_poweroff.c` | Clock device actions; battery state, report and low-battery rules; power off |
| `src/mg_led_pattern.c` | The LED language (table and priorities) `mg_led.c` plays |
| `src/mg_boot.c`, `src/mg_health.c` | Boot record, SAFE mode, crash breadcrumbs; watchdog supervisor, reset reason, healthy point |
| `sysbuild.conf`, `boards/xiao_nrf54l15_nrf54l15_cpuapp.conf` | MCUboot (swap using move, dev signing key) and MCUmgr SMP for the XIAO |
| `src/mg_led.c`, `src/mg_button.c`, `src/mg_battery.c` | LED patterns and "buzz", button events, VBAT |
| `../xplat/libsbc`, `../xplat/liblc3` | Vendored codecs (Apache-2.0), one copy shared with the ESP32 SDK, each with a provenance README |
| `tools/flash_hex.py` | One padded hex (MCUboot + signed app) for flashing the XIAO |
| `tests/host` | Host C tests (any OS) for Link setup, the token proof and the device session |
| `tests/unit` | ztest suites on native_sim |
| `tests/bsim` | BabbleSim: gadget hooks, a test central, `run.sh` |
| `tests/Kconfig.ext`, `tests/app_ext.cmake`, `tests/board_hooks.c` | The board hooks' stand-in for the simulations (Board hooks) |
| `tests/smp_link` | The Muse apps' update code against Zephyr's MCUmgr, `src/mg_ota.c` and MCUboot's loader on native_sim, over a fake GATT link (its `README.md`) |

Threads: all protocol state lives on the app work queue (`mg_app_wq()`);
BLE callbacks post events to it. The audio thread produces everything on
Data and sends `change_data_type` / `stop_mic` itself so Control and Data stay
in order. One notification FIFO feeds the stack from a TX thread. Never block
the system work queue: the Bluetooth host needs it.

## Toolchain

Tested with **Zephyr v4.4.2** and **Zephyr SDK 1.0.1** (GCC 14.3). Older SDKs
(0.16) lack the nRF54L15. Set up a west workspace outside this repository:

```sh
python3 -m venv ~/zephyrproject/.venv && source ~/zephyrproject/.venv/bin/activate
pip install west
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.2 ~/zephyrproject
cd ~/zephyrproject
# Only the modules this app needs (much faster than a full update):
west config manifest.group-filter -- +babblesim
west config manifest.project-filter -- '-.*,+cmsis,+cmsis_6,+hal_nordic,+mbedtls,+tf-psa-crypto,+liblc3,+littlefs,+segger,+tinycrypt,+picolibc,+nrf_hw_models,+trusted-firmware-m,+psa-arch-tests,+babblesim_base,+babblesim_ext_2G4_libPhyComv1,+babblesim_ext_2G4_phy_v1,+babblesim_ext_2G4_channel_NtNcable,+babblesim_ext_2G4_channel_multiatt,+babblesim_ext_2G4_modem_magic,+babblesim_ext_2G4_modem_BLE_simple,+babblesim_ext_2G4_device_burst_interferer,+babblesim_ext_2G4_device_WLAN_actmod,+babblesim_ext_2G4_device_playback,+babblesim_ext_libCryptov1'
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt
```

MCUboot and MCUmgr need two more projects and imgtool's Python packages:

```sh
west config manifest.project-filter -- "$(west config manifest.project-filter),+mcuboot,+zcbor"
west update --narrow -o=--depth=1 mcuboot zcbor
pip install -r bootloader/mcuboot/scripts/requirements.txt
```

The SDK: the minimal bundle plus the Arm toolchain is enough (macOS arm64
shown; pick the matching `linux-x86_64` files on Linux):

```sh
B=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1
curl -LO $B/zephyr-sdk-1.0.1_macos-aarch64_minimal.tar.xz
curl -LO $B/toolchain_gnu_macos-aarch64_arm-zephyr-eabi.tar.xz
tar xf zephyr-sdk-1.0.1_macos-aarch64_minimal.tar.xz -C ~
mkdir -p ~/zephyr-sdk-1.0.1/gnu
tar xf toolchain_gnu_macos-aarch64_arm-zephyr-eabi.tar.xz -C ~/zephyr-sdk-1.0.1/gnu
~/zephyr-sdk-1.0.1/setup.sh -c
```

SDK 1.0 expects toolchains under `gnu/`. An error that
`modules/crypto/mbedtls/tf-psa-crypto` is not an existing directory means the
`tf-psa-crypto` project wasn't fetched.

## Muse Gadgets SDK token (secret)

A Muse Gadget build that pairs with the Muse app needs the owner's Muse
Gadgets SDK token (`mgst_...`, from gadgets.muse.ai > Account > SDK tokens).
The firmware hands it to the app in Link setup's `pairing_confirmed`.

- **If no token was provided to you in context, ask the person for it.**
  Never invent one, never reuse one from another project, and never build an
  image meant for a person's gadget without it unless they tell you to.
- **Keep it only in protected local storage outside the repository**: the
  owner keeps it in `~/.config/muse-gadgets/` (directory 0700, files 0600:
  `sdk_token`, plus ready-made build fragments beside it). Builds read it from
  there (or from a path the person gives you) at build time, into a build
  directory outside the source tree.
- **Never store or check it in anywhere unprotected**: not in a tracked file,
  a committed or generated config/header inside the repo (gitignored is not
  protected), a test fixture, a commit message, a diff, a log, a paste, an
  artifact, chat, or a world-readable path such as `/tmp` or a shared folder.
- **Never print any of it**: not even a prefix. Its length and a SHA-256
  prefix (`48 chars, sha256 1a2b3c4d`) are enough to tell tokens apart, in a
  log, a tool's output or a report alike.
- **An image with the token compiled in is a secret too**: don't upload it to
  artifacts, GitHub/GHE releases, pastes or shared storage. Release images
  that are published either carry no token (the token is written on the
  device, e.g. over a console or the web flasher) or are built without one.
- If a token is ever committed, uploaded or printed, stop and tell the owner
  at once so it can be revoked; don't try to hide it with a new commit.

For Zephyr builds: `~/.config/muse-gadgets/zephyr-sdk-token.conf` holds one
line, `CONFIG_MG_SDK_TOKEN="mgst_..."`. Pass it as an `EXTRA_CONF_FILE` and
build in the west workspace, outside the source tree:

```sh
west build --sysbuild -p -b xiao_nrf54l15/nrf54l15/cpuapp /path/to/muse-gadget-sdk/zephyr -d build-xiao \
  -- -DEXTRA_CONF_FILE=$HOME/.config/muse-gadgets/zephyr-sdk-token.conf \
     -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"/path/to/your-private-key.pem\"
# a release build (automation off) for a development unit (MCUboot's public
# dev key; Signing, below):
#   -- -DEXTRA_CONF_FILE=$HOME/.config/muse-gadgets/zephyr-sdk-token.conf -DSB_EXTRA_CONF_FILE=sysbuild-dev.conf
# bench build (lab and CI: the automation tools, Bench console below):
#   -- -DEXTRA_CONF_FILE="overlay-bench.conf;$HOME/.config/muse-gadgets/zephyr-sdk-token.conf" -DSB_EXTRA_CONF_FILE=sysbuild-dev.conf
# release build, said explicitly (anything that leaves the bench; automation
# off over a board or conf before it: pass it last, the last value wins):
#   -- -DEXTRA_CONF_FILE="$HOME/.config/muse-gadgets/zephyr-sdk-token.conf;overlay-release.conf" ...
```

Never put it in `prj.conf`, a board `.conf`, an overlay or anything else in
the tree. The build directory's `.config`, `autoconf.h` and images hold it:
keep them private and to yourself. The firmware logs only whether it has one.

### SDK token slot (`CONFIG_MG_SDK_TOKEN_SLOT`)

For images that are published and then personalized by an updater (a web
updater that writes the owner's token in the browser): instead of
`CONFIG_MG_SDK_TOKEN`, the token comes from a slot in the image, so the image
that is built, stored and served never holds one. Off by default; with it off
nothing changes.

The contract an updater relies on (`src/mg_setup.h`, `src/mg_token_slot.c`):

- `const char mg_sdk_token_slot[128]`, alone in its own file (so `mg_setup.c`
  reads it only at run time), initialized to the ASCII marker
  `MG-SDK-TOKEN-SLOT-v1:` followed by NULs. It lands once in the signed image's
  payload (`.rodata`, byte-aligned, offset not fixed): the updater finds the
  marker, which must occur exactly once with 107 NULs after it, and refuses
  otherwise.
- The updater writes the token (`mgst_...`, at most 127 characters) and NULs
  over all 128 bytes, then recomputes the image's SHA-256 TLV and its signature
  (MCUboot header, payload and protected TLVs, as imgtool does) and keeps the
  other TLVs. The uploaded image's hash is the new SHA-256.
- The firmware takes no token when the slot still starts with the whole
  marker, starts with NUL, or holds no NUL at all (malformed). It logs the
  token's length only (`tests/host`, `test_setup_slot`). It compares the
  marker in two pieces, so the marker's text appears nowhere else in the
  image; a build with the slot checks that on `zephyr.bin` after every link
  (`tools/check_token_slot.py`, the `mg_token_slot_check` target: exactly once,
  else the build fails).

**Signing**: an updater can only re-sign in the browser with a key it holds.
MCUboot's ECDSA P-256 development key (`bootloader/mcuboot/root-ec-p256.pem`)
is public and is what the units' bootloader trusts today, so the page carries
it. A production key must never ship in a page: with one, personalizing moves
to a server (or the token goes to the device another way), and this slot
contract stays the same.

## Build

From the workspace (`source ~/zephyrproject/.venv/bin/activate`, and
`export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1` if CMake can't find it):

```sh
cd ~/zephyrproject
# a release build (automation off) for a development unit (MCUboot's public
# dev key: sysbuild-dev.conf)
west build --sysbuild -p -b xiao_nrf54l15/nrf54l15/cpuapp /path/to/muse-gadget-sdk/zephyr -d build-xiao \
  -- -DEXTRA_CONF_FILE=$HOME/.config/muse-gadgets/zephyr-sdk-token.conf -DSB_EXTRA_CONF_FILE=sysbuild-dev.conf
# bench build (lab and CI: automation on): add overlay-bench.conf
west build --sysbuild -p -b xiao_nrf54l15/nrf54l15/cpuapp /path/to/muse-gadget-sdk/zephyr -d build-bench \
  -- -DEXTRA_CONF_FILE="overlay-bench.conf;$HOME/.config/muse-gadgets/zephyr-sdk-token.conf" \
     -DSB_EXTRA_CONF_FILE=sysbuild-dev.conf
# your own product under your own key (an option): your key instead of sysbuild-dev.conf
#   -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"/path/to/your-key.pem\"
```

A unit a person wears or carries runs a release build (automation and bench
mode off, `overlay-release.conf` last), with the microphone's gates on;
test, bench, automation and bench-mode builds (`overlay-bench.conf`,
`overlay-bench-mode.conf`, the bench-only test options) go on bench units
only.

The XIAO boots through MCUboot, so always build it with `--sysbuild` (or
`west config build.sysbuild True`); without it CMake stops with a message
saying so. A sysbuild directory holds two images:

| File | What |
|---|---|
| `build-xiao/mcuboot/zephyr/zephyr.hex` | MCUboot, flashed once |
| `build-xiao/zephyr/zephyr/zephyr.signed.hex` | the app, signed: flash it, or `zephyr.signed.bin` for an over-the-air update |

Flash them as one padded hex from `tools/flash_hex.py` (see Flash: the raw
hexes through `west flash` can leave the last bytes unwritten).

The version is set in one place, `VERSION`
(MAJOR.MINOR.PATCH), and `cmake/mg_version.cmake` turns it into what the
gadget reports, before Kconfig runs:

| Build | Firmware version (DIS, Link `device_info`, logs, NUS `version`) | MCUboot / SMP image version |
|---|---|---|
| the release tag `xn54-v0.2.0`, clean tree | `0.2.0` (`0.2.0+tok` with the SDK token) | `0.2.0+V` |
| any other commit | `0.2.0-dev.N+SHA` (N = commits in HEAD's history), `.tok` and `.bench` or `.auto` added for those variants | `0.2.0+(N*128+V)` |
| ... with uncommitted changes | `0.2.0-dev.N+SHA[.tok][.bench\|.auto].dYYMMDDHHMM` (configure time) | `0.2.0+(N*128+V)`, V + 4 (the dirty value) |
| no git (a tarball) | `0.2.0-dev+nogit` | `0.2.0+V` |

V is the build variant (`cmake/mg_image_id.cmake`), a sum of values: 1 the
SDK token compiled in (an extra conf file setting `CONFIG_MG_SDK_TOKEN`, or a
`-DCONFIG_MG_SDK_TOKEN`; only whether one is set is read, nothing of the token
goes into the version), 2 a bench build (an `overlay-bench*` conf, or one
setting one of the bench options below), value 4 uncommitted changes, 8 the
automation tools on (`CONFIG_MG_AUTOMATION`: off by default here, on in
`overlay-bench.conf`), and 16 x shape for a test shape (below);
`-DMG_BUILD_VARIANT=<token 1 + bench 2 + automation 8 + 16 x shape>` sets
the others. The firmware string marks them `.tok`, then `.bench` or
`.auto` (automation without the bench marker). A release built
from a clean tree without the token is `0.2.0` / `0.2.0+0`, as before, and
a release build (`overlay-release.conf`, or no automation) carries no
automation bit. So the release, automation, bench and token builds of a
commit never share an MCUboot image id (an updater that tells images apart
by it would otherwise take one for another and skip its test mark); after
Kconfig the build fails if the token, bench or automation options in effect
don't match the variant (`mg_image_variant_check()`, the bench options
being `CONFIG_MG_BENCH_MODE`, the marker `overlay-bench.conf` and
`overlay-bench-mode.conf` set (`CONFIG_MG_BENCH_BUILD`, its old name, implies it),
and the test shapes; pass `-DMG_BUILD_VARIANT` if they came from somewhere
else). The test shapes, each a bench-only switch that changes what the image
does (`MG_TEST_SHAPES`: 1 `CONFIG_MG_BENCH_BOARD_NOT_READY`, 2
`CONFIG_MG_BENCH_EARLY_HANG`, 3 `CONFIG_MG_BENCH_NO_CONFIRM`), take V's bits
4-6 (16 x shape) and add `.notready`, `.earlyhang` or `.noconfirm` to the
firmware string, so a bench build and a never-healthy bench build of one
commit never share an id; two shapes at once fail the build. A build signed
with MCUboot's public development key (`sysbuild-dev.conf`) adds `.devkey`
to its firmware string (never to the image id; `sysbuild.cmake` hands
`MG_DEVKEY` to the image) and logs a boot warning, so a sample-key build is
never taken for a release. Give the SDK token in a conf file, never with `-D`: a failed configure
prints the whole command line. The id isn't a content hash: two dirty builds
of one commit, or builds differing in another option, can share one, and
every build without git shares `base+V` (with a warning), so an updater
compares the image hash too. Each variant's build numbers rise with N (any
downgrade compare still orders
them), a newer commit is the bigger number whatever the variants, and the top
byte (bits 24-31) stays free for an updater's personalization; N must stay
below 2^17 (host `image_id`, `tests/host/image_id_check.cmake`).
So no two images share a version string. A release bumps `VERSION`, commits,
and tags `xn54-vX.Y.Z` (`<code>-vX.Y.Z`, as multi-board repos do); a test
update for a revert or upgrade check needs nothing: the next commit has a
higher N. `-DMG_BUILD_NUMBER=<n>` overrides N. A new commit or tag
reconfigures by itself; uncommitted edits don't, so build releases with
`-p`. The boot log names it: `Muse Gadget 0.2.0-dev.331+1a2b3c4d starting`.

Last measured (v4.4.2, SDK 1.0.1), app / MCUboot:

| Build | App flash | App RAM |
|---|---|---|
| plain, the Muse apps' updates (SMP memory only while in use) | 299,504 B | 104,820 B |
| plain, with the gadget behavior, SMP memory held for good | 296,720 B | 123,120 B |
| plain, without the gadget behavior | 287,684 B | 114,816 B |
| MCUboot | 30,612 B of 62 KB | 17,376 B |

The gadget behavior (clock, battery and power off, LED language, low
battery, watchdog and boot record, hold contract, link policy) costs 9.0 KB of flash and
8.3 KB of RAM, 7.5 KB of it the pre-roll backlog (1 s of SBC).

Muse Link setup and the token proof cost about 46 KB of flash and 10 KB of
RAM over the build with device tokens (239,904 B / 104,504 B): their own
code about 15 KB and 8 KB of RAM (mostly the 4.3 KB setup receive buffer and
1 KB transmit buffer), mbedTLS's P-256, AES-GCM, SHA-256 and HMAC about
29 KB, a 2 KB larger app work queue stack (P-256 runs on it), and GATT
caching, which PSA Crypto turns on (its database hash, 1.5 KB of RAM for
the host's long work queue); dropping device tokens saved about 2 KB of each.
MCUmgr SMP adds about 18 KB of flash and 7.5 KB of RAM; the SBC encoder is about
3.4 KB of flash and LC3 about 37 KB flash / 1.8 KB RAM (`-DCONFIG_MG_LC3=n`
drops it). The app can't outgrow its slot: it links into `slot0_partition`
(712 KB, ending where slot1 starts), and imgtool refuses an image too big for
the slot.

Board notes (`boards/xiao_nrf54l15_nrf54l15_cpuapp.*`):

- The RRAM (1524 KB, no external flash) keeps the board's standard MCUboot
  map (`nrf54l15_cpuapp_partition.dtsi`):

  | Partition | Offset | Size | Holds |
  |---|---|---|---|
  | `boot_partition` | 0x0 | 62 KB | MCUboot |
  | `slot0_partition` | 0x10000 | 712 KB | the running app |
  | `slot1_partition` | 0xC2000 | 712 KB | an uploaded update, or the offline clips |
  | `storage_partition` | 0x174000 | 36 KB | settings (ZMS): app settings, what Link setup stores |

- MCUboot runs swap using move (`sysbuild.conf`): an update runs on test
  after the next reset and keeps itself (`boot_write_img_confirmed`) once the
  boot is healthy (BLE up and advertising for 30 s with its threads alive;
  Gadget behavior below) and a link has proved ATT works or any central's
  image-list read was answered (the Keep signal; Firmware updates below; a
  client's earlier confirm takes effect then too), and an image that never
  gets there is swapped back at the
  reset after. The watchdog resets an image
  that hangs. Swapping 712 KB of RRAM takes a few seconds at boot (MCUboot
  feeds the watchdog meanwhile). Known limit: no downgrade prevention
  (MCUboot's `MCUBOOT_DOWNGRADE_PREVENTION` is off), so an older image signed
  with the unit's key installs; the update gate (the token proof, or one
  confirm press on the gadget; Firmware updates, below) decides who may
  upload.
- **Signing**: images are signed with ECDSA P-256 under
  `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`. MCUboot's development key
  (`bootloader/mcuboot/root-ec-p256.pem`, the default) is public: anyone can
  sign images a unit built with it accepts. So the configure step fails
  (`sysbuild.cmake`, `tools/check_signing_key.py`, host `signing_key`) when
  the key is one of MCUboot's public keys (in its tree, or a copy elsewhere
  with the same public key, in any encoding), or its public key is on an
  optional key blocklist (`-DMG_KEY_BLOCKLIST=<file>`: public-key sha256
  fingerprints of DER SubjectPublicKeyInfo, one per line, `#` comments;
  `check_signing_key.py --blocklist`; a committed key is a sample key),
  missing, or can't be checked (no `cryptography` package or `openssl` to
  parse it, or a blocklist that can't be read), unless the build says it's
  for development:
  `-DSB_EXTRA_CONF_FILE=sysbuild-dev.conf` (`SB_CONFIG_MG_DEV_SIGNING_KEY`,
  `Kconfig.sysbuild`; it builds with a warning). No private key of your
  own is needed for that; someone shipping their own product can sign with
  their own key instead, kept out of the repository. Boards flashed with a
  dev-key MCUboot only take dev-key updates (a bench XIAO, for example):
  their bootloader has to be replaced through SWD to trust another key.
- The FPU is on (`CONFIG_FPU=y`); LC3 is float code.
- Battery: `/zephyr,user io-channels = <&adc 7>` (AIN7, P1.14), the board's
  ADC channel 7 (gain 1/4, internal reference, 12 bits, 8x oversampling). VBAT
  reaches it through a 2:1 divider (`CONFIG_MG_BATTERY_DIVIDER_RATIO`) that
  the board switches with P1.15 (`vbat_pwr`, a fixed regulator the board
  turns on at boot and leaves on). Seeed's sample uses the same channel,
  switch and ratio; it hasn't been measured on this board yet. Below 2.5 V
  (no cell, or one past use) Battery Level reads 100, as for a gadget
  without a battery; on USB with no cell the pin may also read the charger's
  output (about 4.2 V, so 100 again). The bench `s` key prints the raw mV.
- The PDM mic is `dmic_dev` (pdm20, CLK P1.12, DIN P1.13), powered by the
  board's `pdm_imu_pwr` regulator. The channel is taken as LEFT. On the
  hardware it starts with a DC offset near -1100 that drifts for seconds:
  a one-pole high-pass (`CONFIG_MG_MIC_HPF_HZ`, 30 Hz, `src/mg_hpf.c`) runs
  right after the PDM, before the gain, primed with the first sample of each
  capture so there's no settling step. Speech from 30 cm peaked near -30 dBFS
  with the tuned software gain (4x, +12 dB), so the PDM's own gain is raised
  to +10 dB (`CONFIG_MG_DMIC_HW_GAIN_DB`) for about -20 dBFS. set_mic_gain's
  level (1-100, mgcommands.h; 0 or over 100 is `invalid_value`) maps onto the
  software gain monotonically in dB through that tuned gain
  (`src/mg_mic_gain.h`): level 50
  (`CONFIG_MG_MIC_DEFAULT_GAIN`) is the tuned gain, 1 is -30 dB under it and
  100 is `CONFIG_MG_MIC_GAIN_TOP_DB` (+12 dB) over it, speech at 30 cm then
  peaking near -8 dBFS (100 = the highest gain at which it doesn't clip); the PDM clock is left to
  the driver (1.0 to 3.5 MHz requested). Not re-measured on hardware since.
  Every turn records its capture level
  (`src/mg_cap_level.h`, integer only, a fixed-point log2 within 0.1 dB):
  peak and rms of what the
  encoder gets (after the high-pass and the gain), the floor (rms of the
  first 100 ms), the samples that clipped (the ADC's rail or the gain's
  clamp) and the gain level in effect, numbers only. Logged at the
  capture's end (`capture level: peak ... dBFS, N clipped, gain level 50
  (12.0 dB)`, mirrored on the Nordic UART), the last four on the bench `s`;
  the first real turn on a unit is the close-talk anchor.
- No vibration motor (`CONFIG_MG_HAPTICS` off): haptics_enabled and the
  ptt_buzz_* settings aren't listed and answer `unsupported`, and the
  activation "buzz" never runs. The typed settings list (`[supported_features,
  sub_feature, set_settings, …]`, sent with request_status) names exactly the
  settings get/set_settings accept.
- The LED speaks the Muse Gadgets single-color LED language
  (`src/mg_led_pattern.c` has the table and the priorities, `src/mg_led.c`
  plays it; the README lists it): setup confirm 50/50, update confirm two
  120 ms blinks every 0.8 s, capture solid,
  recording offline solid (as live capture: the light is solid for any
  capture that leaves the gadget), setup done / turn done 3 quick blinks, clip
  kept 2 / not kept 4 blinks, error 3 fast blinks (100/100), thinking
  150/850, responding 500/500, low battery one 80 ms flash every 30 s,
  idle dark (connected or not, on USB too: the board can't tell;
  `CONFIG_MG_LED_IDLE_HEARTBEAT` adds a 20 ms / 4 s blink for bench boards).
  Callers on any thread only set atomics and kick the LED's tick; the tick
  is the only writer of the playing pattern and the until-times (the
  audio thread and the work queue once raced on them).
  The link (ready only with a client connected, subscribed to Control and
  Data, its request_status received, in either order (mgcommands.h),
  so a turn never goes to a client that hasn't read the
  features; and push-to-talk on) is logged, not shown. The assistant's turn
  (`CONFIG_MG_ASSISTANT_STATE`, `mg_command_assistant_state`): capture while
  listening, thinking after stop_mic, then responding, done or error as the
  client reports, idle after 120 s without an update or on disconnect. The
  LED is a GPIO, not dimmable: no `led_brightness` (brightness is for dimmable
  lights; nothing could dim the capture light anyway). Thinking at stop_mic comes from the
  device itself, whatever the client has sent (the iOS app never sends
  thinking); any assistant_state takes over. A client that has sent none on
  the connection gets done `CONFIG_MG_STATE_COMPAT_TIMEOUT_MS` (15 s) after
  stop_mic if nothing arrives. Changes are logged as
  `mg.ble: link <old> -> <new>`, `mg.face: <state>` and `mg.led: <pattern>`
  (the README lists the patterns).

### Muse Link setup

The gadget gets its device token pair the way Wi-Fi Muse gadgets do, over
the Muse Link BLE setup service (`src/mg_setup.c`), and follows the ESP32
reference message for message (`esp32/main/ble_server.c` dispatch and
statuses, `link_pairing.c` session, `app.c` phases):

- **Service** `7fdd3d1c-…` with RX (write, write without response) and TX
  (read, notify), `0xFE` chunk framing both ways (chunks of at most 160 bytes
  and MTU - 3; messages up to 4.3 KB in). TX reads return the last plaintext
  status, or `encrypted_status`.
- **Community pairing v5** only (`pairing_auth` `none`, epoch 0, policy
  `confirm_press`, no manufacturer attestation): `pairing_client_hello` ->
  `pairing_ready` (P-256 ECDH, the transcript from
  `esp32/main/pairing_transcript.c`, HKDF session secret, record keys and
  session id) -> encrypted `pairing_client_finished` -> `confirm_required`
  -> one press of the talk button (or the bench console's `>pair.confirm`)
  while the prompt is up -> `pairing_confirmed`, carrying `sdk_token`
  when `CONFIG_MG_SDK_TOKEN` is set (default empty; from the protected conf
  file, never one in the tree: Muse Gadgets SDK token, above). Without one, boot and `pairing_confirmed` log
  a warning: the Muse app refuses to finish setup without it. Records are AES-256-GCM with the ESP32's
  nonce, AAD and counter rules. Phases time out as on the ESP32: 60 s for
  client_finished, none for the press (the prompt stays until the press,
  the link's end or the phone giving up), 120 s for provision_v2. While a
  confirmation is pending the press is never push-to-talk; the LED blinks
  very fast (50/50), and 3 quick blinks follow `auth_ok`. A first setup and
  a re-setup (**setting a set-up gadget up again**) are confirmed alike, by
  ONE press of the button: no hold, no press time (host
  `test_resetup_one_press`, BabbleSim `setup`), but only a press that began
  after the prompt armed: arming it marks the button's queue
  (`mg_button_flush_injected()`), and a press queued or down then (a
  debounced press that began before it, `mg_input_after_arm()`) is taken by
  the prompt with its release and confirms nothing, set up or not, in setup
  mode or not; the next press confirms (`mg_setup_button_at()`, host
  `test_press_before_prompt_never_confirms`). On release
  builds only the button's own press confirms, set up or not, in setup mode
  or not: an injected one (the USB console's `d`) is taken by the prompt,
  never push-to-talk, and confirms nothing (`mg_setup_button(real)`, host
  `test_injected_press_never_confirms`); the Nordic UART's state-changing
  words stay refused, so nobody confirms from radio range. In automation
  and bench-mode builds an injected press confirms as the button does, and
  so does the bench console's `>pair.confirm`.
  A re-setup never takes an `ota_url` (ignored and logged; the gadget has
  no Wi-Fi updates anyway). Host `test_reprovision`.
- **Plaintext**: before a session only `error_encryption_required`,
  `error_pairing_invalid_hello`, `error_pairing_unavailable` and
  `error_pairing_decrypt` go out; after `pairing_ready` plaintext commands
  are ignored.
- **get_device_info** (plaintext, before a session): node id, version,
  device id (`hatch-link:<mac>`), mac (the identity address), model
  `hatch_link`, protocol 5, auth `none`, epoch 0, policy `confirm_press`,
  `wifi: "none"`, `device_key_auth: 0` (a
  BLE-only setup offers no device-key auth, so the app's setup leaves the
  gadget keyless and the phone relays its session; a VM that mints keys mints one when
  it's offered and the phone then answers the sealed hello `not_enrolled`;
  `CONFIG_MG_SETUP_DEVICE_KEY_AUTH`, the automation console's `>keyauth=`),
  `cvm_supported: true` (the phone
  runs the device session, the attestation handshake included), `mgcommands: 1`. The
  node id is `gadget-` and the last six hex digits of the advertised name in
  lowercase (`MuseGadget-1A2B3C` is `gadget-1a2b3c`; `homelink-`
  confused community gadgets with the Home Link),
  with `CONFIG_MG_NODE_ID_GADGET` (off by default; turn it on when the
  apps you pair with classify gadget nodes by family; `overlay-bench.conf`
  and BabbleSim turn it on); without it `homelink-1a2b3c` as before. A setup offers its form (device_info, the
  transcript, pairing_ready), derives K with it and stores it with K (the
  commit record's flag 0x10 for gadget-, kept by token updates); a gadget
  keeps the id it was set up with, K, session, sealed credentials and
  link.register included, whatever a later image offers, until it is set up
  again: no forced re-pair (`mg_identity_node_id()` is the id in use,
  `mg_identity_offer_node_id()` what a setup offers; host `test_node_ids`,
  `test_handover_node_id`).
  A product in the Muse
  Gadget catalog sets its model code (`CONFIG_MG_DEVICE_PRODUCT_CODE`, e.g.
  `XN54`): `MuseGadget-XN54-1A2B3C`, with the same node id. The XIAO's is
  `XN54` (its board `.conf`): it advertises `MuseGadget-XN54-1A2B3C`
  (node id `gadget-1a2b3c` with `CONFIG_MG_NODE_ID_GADGET`). The host and unit Link setup
  replays offer `homelink-`, the form `community_v5` was made with; host
  `test_setup_crypto` checks `community_gadget_v5` (node `gadget-000001`)
  and `test_node_ids` sets up with the `gadget-` form.
- **provision_v2** is token-only: `access_token`, `refresh_token` and
  `token_type: "device"` are required (`error_missing_credentials`); a
  nonempty `ssid` or `password` gets `error_wifi_unsupported` and stores
  nothing; an optional `device_key` and `device_key_id`, with `vm_id` and
  `vm_kind`, are stored with the tokens (Device session, below); other
  fields are ignored. Each token is
  at most 2048 bytes.
  The gadget derives the token proof key K (mgcommands.h, Token proof) from
  the access token and its node id, stores tokens, K and the commit record
  (`src/mg_setup_store.h`: the tokens, K, the device key and its `vm_id`
  in one of two slots, `mg/setup/at`, `rt`, `k`, `dk`, `kid`, `vm` or
  `at1`, `rt1`, `k1`, `dk1`, `kid1`, `vm1`, then `done` with "setup
  complete", "Wi-Fi skipped", "device key held", the slot, the lengths and
  `vm_kind`; ZMS writes one entry
  atomically, so `done` is the commit point, written last and erased first),
  and only then answers `auth_ok`. A failed write sends `error_storage`,
  erases what this provisioning wrote (a set-up gadget keeps its old set)
  and drops the session. Boot erases
  anything without a valid `done`, and the slot `done` doesn't name, but
  only from a whole listing: Zephyr's settings listing returns 0 whatever
  its backend hit (ZMS ends it early at a list link it can't read), so
  before it erases anything or takes the gadget for not set up, the load
  looks every entry up again by name; a lookup that fails or disagrees
  with the listing, like a record that can't be read, erases nothing,
  chooses nothing and refuses commits until a load reads the store (host
  `test_boot_partial_listing`, `test_boot_unreadable`). Meanwhile it fails
  closed: `mg_setup_store_complete()` is true (no Link setup advertising,
  no first-setup rules, the set-up gates), the set's tokens, K and device
  key are unavailable (`-ENOENT`: the token proof answers `busy`), and an
  erase (the reset hold) that empties the store opens setup again (host
  `test_boot_unreadable_fails_closed`). ZMS's own listing would delete an
  entry it couldn't read, a transient error included:
  `CONFIG_SETTINGS_ZMS_NO_LL_DELETE` (selected by Kconfig.mg's
  `MG_SETTINGS_KEEP_ON_READ_ERROR`) makes it skip one instead (unit
  `test_listing_never_deletes`). Logs
  show token lengths, never tokens or K.
- **Afterwards** Link setup advertising stops (mg only); the setup service
  stays, and a set-up gadget can be set up again without Forget, but
  outside setup mode a re-setup is never confirmed by the talk key: the
  XIAO's only key is its talk key, so a set-up XIAO takes a re-setup only
  in setup mode, where one press confirms it (above): the button held at
  boot and released before the SAFE cue (3 s) boots the normal firmware
  with Link setup open for 5 min or until a re-setup is confirmed
  (`mg_setup_mode()`, `mg_setup_mode_begins()`; neither SAFE is setup mode,
  and SAFE runs no Link setup; test and bench builds always are); outside
  it the hello gets `error_pairing_unavailable` in plaintext and nothing
  shows (host `test_resetup_setup_mode`), and a talk press at a re-setup
  prompt only dismisses it. On the prompt's own connection a new hello
  takes the prompt over, and one that fails validation (its fields, its
  key, or after the wipe) ends the prompt with nothing confirmed (host
  `test_resetup_bad_takeover`); another connection's gets
  `error_operation_in_progress`; a hello on a connection within 10 s of its
  dismiss is refused on release builds (`mg_resetup_hello()`). Known limit:
  the re-setup prompt is limited per connection, so a nearby device can
  raise it again by reconnecting; outside setup mode the talk key never
  confirms it, so the owner can only dismiss it. (Gadget setup v2,
  re-provisioning, `../protocols/README.md`): `pairing_client_hello` is
  answered as before setup, on a new connection or the one the app already
  has, with the same handshake, confirmed by one press (above). Its
  `provision_v2` writes the new
  set into the other slot and moves `done` to it: until that write the old
  tokens, K and device key stay (a failed or abandoned setup, a power loss,
  a refused `provision_v2`), after it only the new ones (a setup without a
  key leaves none). A token proof matched with the old K stops counting at
  the commit, and the device session ends. The earlier owner's offline clips
  go too (Offline clips and updates share slot1, below). A session provisions once: another
  `provision_v2` on it gets `error_pairing_confirm_required`. A confirmed
  session's `unpair` erases the setup.
- **Reset**: on a cold boot (power-on, the reset pin; never a wake from
  System OFF, so a talk from off can't do it),
  hold the button `CONFIG_MG_RESET_HOLD_MS` (5 s) until the LED flickers
  ("release now", 10 Hz), then release within `CONFIG_MG_RESET_RELEASE_MS`
  (5 s); a shorter or longer hold does nothing (`src/mg_reset_hold.h`,
  host `test_reset_hold`; a board can add conditions in
  `mg_board_reset_hold_allowed()`). With `CONFIG_MG_SAFE_HOLD` (default y:
  `mg_board_safe_hold_allowed()`, which a board can narrow, e.g. only with
  the charger present; recovery tier 3 by button) the same hold has a SAFE
  point: at `CONFIG_MG_SAFE_HOLD_MS` (3 s) the SAFE cue (a double blink,
  100/100/100/700; cyan on RGB), and a release before the
  reset's cue boots once in SAFE mode (release < 3 s nothing, [3, 5) s
  SAFE, [5, 10) s reset). A SAFE the gadget entered by itself reads the
  boot hold too: either release point there makes it the button's SAFE
  (Bluetooth updates open; the setup reset waits for a normal boot), so a
  set-up gadget always has a physical way out. Or the token proof's `clear`
  after a
  match: tokens, K and the markers are erased and Link setup is advertised
  again.

Advertising (`src/mg_ble.c`): one legacy advertiser on the identity address
(the board's static random address; no privacy), so Link setup and the mg
service are found at one address, which the apps key their record by.
Until setup is complete the payload takes turns every 1.5 s between the Link
setup UUID (with the manufacturer data `FF FF 02`: the test company id, then
the flags byte with bit 1, a Muse Gadget over Bluetooth, set and bit 0, set
up, clear; `../protocols/README.md`, Gadget setup v2) and the mg UUID
(`bt_le_adv_update_data`, the same advertiser); afterwards it is mg only.
The interval: fast, 20-30 ms, for `CONFIG_MG_ADV_FAST_SECS` (30 s)
after boot, after a disconnect (from the link's loss) and after a press with
no app connected, before setup too (never fast
without end; `src/mg_adv_policy.h`); then `CONFIG_MG_ADV_SLOW_MS`
(Apple's 1022.5 ms step). Logged: `mg.ble: advertising slowly (every 1022
ms)`, `mg.ble: advertising fast for 30 s`.

### Token proof

`mg_command_token_proof` (34, mgcommands.h): the client sends a challenge
nonce, the gadget answers a fresh nonce from PSA and HMAC-SHA256(K, device
label || nonces), the client confirms with its own MAC, and the gadget
answers result 1 or 0 (constant-time compare). Every answer (and the
proof's errors) goes at once, ahead of anything queued (clip data, a
capture; `mg_transport_notify_urgent()`: the queue's head, a buffer held
back for it), and `busy` rather than silence when it can't go within
about 1 s. Without K a challenge is
`not_found`; confirm with nothing pending is `invalid_value`; a confirm
the gadget can't check (K's read or the HMAC failed) is `busy`, never
result 0: no match, a held capture neither sent nor discarded, the client
challenges again (host `test_token_proof_key_error`); a new
challenge restarts the exchange (and drops an earlier match), and clears
the client's ready state (an iOS relaunch can keep the link
up): push-to-talk is off, the codec kept, until the client enables it
again, so presses meanwhile send gestures only. The result
lasts for the connection (reset on connect and disconnect; bound to the
connection it was made on, `mg_transport_conn_gen()`, so SMP's own thread
never sees it on another one before those events are handled) and gates
the device session's hello (below), mic output and, on a set-up
gadget, the commands that destroy or store something:
the clip queue's `clear`, device_action `power_off`, and a set_settings that
names a stored setting (the buzz, codec, capture rate and bitrate,
audio_queue_enabled) answer `proof_required` without a match (a set_settings
skips each stored pair, still applies the per-connection ones in the same
write, then answers `proof_required` once: `mg_settings_skip_persisted()`,
`mg_settings_apply_ex()`; unit `test_privileged_need_proof`); per-connection
settings (push-to-talk, link mode, client app), reads and status stay open,
the bench switch doesn't count, and a gadget that isn't set up (no K) is open
as before (unit `test_privileged_need_proof`). device_action set_unix_time
always applies and is never refused, but stores its UTC offset and flags
only where a stored setting would be taken (unit
`test_clock_stores_only_proven`; the Nordic UART `status`
counts nothing new, `mg_core_privileged_refused()` does). `clear` after a match answers
result 1, then erases the setup; before a match it is `proof_required`.
Forget's clear inside the sealed session is `mg_session_clear` (12),
taken in every build; with
`CONFIG_MG_SESSION_SEALED_CLEAR` (off by default; turn it on when the apps you pair with send it) the caps carry `mg_session_cap_sealed_clear` and this
unsealed `clear` erases nothing, refused in the hello's order (no token
`not_found`, before a match `proof_required`, then `insecure`). In the
device session, `mg_session_clear` with plaintext [0] on the next client
counter erases the setup the moment its tag checks, then answers its own
sealed `mg_session_clear` on the next device counter, [1] once erased or
[0] if the erase failed; only the answer waits for a device message already
part way out (its counter was spent), queued data not yet started never
goes, nor do sealed credentials still waiting for room (the answer takes
their counter 0), and the session ends once the answer has gone; unsealed is `insecure`, a bad tag or another plaintext
`invalid_value`, one whose first fragment arrives while a
credentials_update is still arriving `busy` with its counter used, none
erasing anything (host `test_sealed_clear`, its own build).
The supported_features list includes 34.

### Device session

`mg_command_session` (0x89, mgcommands.h, Device session;
`src/mg_vm_session.c`): the gadget is a gadget node on its owner's VM, as a
Wi-Fi gadget is (`esp32/main/noise_control.cpp`), with the phone as its
network. supported_features lists 0x89 and not skills (0x88), which it
supersedes.

- **Sealed credentials** (mgcommands.h, Sealed credentials):
  request_status lists `[supported_features, sub_feature,
  mg_command_session, caps]` after the command list, caps
  `mg_session_cap_sealed | mg_session_cap_client_refreshes` (no Wi-Fi: the
  app refreshes the pair and writes it back). A hello with
  `mg_session_hello_flag_sealed` (its optional last byte) needs no
  encrypted link: the gadget echoes the flag in its hello and sends the
  credentials sealed (`mg_session_flag_sealed`, total_len the sealed
  length) under keys from K, the proof's nonces and the node id, with the
  shared seal `../portable/mg_device_session` (vendored unchanged; its
  `mglsc_hmac_sha256` / `mglsc_ct_equal` come from `src/mg_seal_crypto.c` on
  PSA). **One sealed session per proof**: the nonces are taken once
  (`mg_token_proof_take_nonces()`), so a second sealed hello before a new
  token proof gets `proof_required` and the open session stays. **Sealed
  data**: in a sealed session every message
  with a payload is sealed both ways, `data` included, under one counter
  per direction shared with the credentials and `credentials_update`
  (from 0 at each sealed hello, never at a `state` open). The device seals
  each data message whole when it's queued (`mg_vm_session_sendv`) and
  fragments the sealed bytes (flag `mg_session_flag_sealed`; total_len,
  window and credit count sealed bytes, so a message carries at most
  max_message - 20 bytes of plaintext). A sealed first fragment carries the
  whole 4-byte counter both ways (mgcommands.h): the device starts
  a sealed message only with 4 bytes of credit, and a shorter sealed first
  fragment from the client is `invalid_length`, its credit given back and
  no counter used (host `test_sealed_first_fragment`; the vectors'
  "credit below the counter" exchange). A client message takes its counter
  at its first fragment: the next one, or the one open hole (below), else
  `invalid_value`; from then on it is used (a message aborted or dropped
  after its first fragment, a state change), except by a refused tag: the
  tag is checked at the last fragment (a bad one: `invalid_value`, the
  message dropped, its credit given back) and the counter is given back, or,
  when a later counter was taken meanwhile (a `credentials_update` and a
  `data` message in flight together, each reassembled on its own), it
  becomes the hole, accepted once; a newer hole replaces it, as in `mg_session_seal_ref.py`'s
  receiver. Three counter refusals in a row (a first fragment, data or
  credentials_update, whose counter is neither the next one nor the hole)
  end the sealed session completely (keys wiped, counters 0, the hello gone:
  a new proof and hello start over from 0); tag refusals never count, and an
  accepted counter or a hello resets the run (as the portable core's MGDS_CTR_END: a relay one counter
  ahead for good after a cut first fragment; logged, counted by
  `mg_vm_session_ctr_ends()`, host `test_ctr_end`). Counters never wrap
  (mgcommands.h, Sealed credentials): once the client's 0xffffffff is taken no other
  counter is (`rx_spent`), and once the device has sealed its 0xffffffff it
  seals nothing more and refuses each sealed message from the client with
  `invalid_value` (`tx_spent`): a new proof and sealed hello start over. A
  sealed hello on a link below `MG_MIN_ATT_MTU` (100) is `invalid_value` (its
  one-write messages, such as credentials_saved, need it); the board's
  `CONFIG_BT_L2CAP_TX_MTU` is asserted at or above it, so the cap stays
  declared. Unsealed `data` or
  `credentials_update` in a sealed session gets `insecure`; sealed ones in
  a plain session `invalid_value`. Messages without a payload (state,
  open_request, credit, abort) stay unsealed. Every unsealed hello is
  `insecure`, on any link (hello, below). Host: the `sealed_data` and `sealed_interleaved`
  vectors frame by frame (`test_sealed_data`, `test_sealed_interleaved`).
- **hello**: answered `not_found` without a setup, `proof_required` before
  a matching token proof on this connection, `insecure` without
  `mg_session_hello_flag_sealed` on any link, encrypted or bonded included
  (a token proof can be
  relayed, and a proxy that bonds on its own leg has an encrypted link, so
  credentials leave only sealed; nothing is sent, nothing taken, the proof's
  nonces stay for the sealed hello), `invalid_value` for another version or
  bad limits (a sealed hello's window under 4 too: it must hold a whole
  counter). Otherwise the
  device's hello (max_message `CONFIG_MG_VM_SESSION_MAX_MESSAGE`, window
  `CONFIG_MG_VM_SESSION_WINDOW`, 16 KB each) and the credentials as
  fragments outside credit: node id, the stored token pair and, when setup
  provisioned one, the device key and key id in base64, then `vm_id` and
  `vm_kind` when setup sent them. Credentials longer
  than 2048 bytes get `not_found` with a log string instead.
- **Setup's device key**: `provision_v2` may carry `device_key` (32 bytes)
  and `device_key_id` (16 bytes) in standard base64 with padding, both or
  neither, with an optional `vm_id` (1-64 printable ASCII bytes) and
  `vm_kind` (`"svm"` or `"cvm"`). Anything else is
  `error_invalid_device_key`, with nothing stored and the session still
  usable. They go in the same commit as the tokens (`mg/setup/dk`, `kid`,
  `vm` in the set's slot, `vm_kind` in `done`). A setup without a key
  leaves none, its binding included.
- **credentials_update** stores the refreshed pair in one commit
  (`mg_setup_store_update_tokens`): the pair, with copies of K, the
  device key and its binding, goes to the other slot, then a new `done` record names it,
  then the old slot is erased; boot erases whichever slot the record
  doesn't name. K stays as setup derived it (the app keeps that K). A
  record from an earlier device session build that kept the pair in
  `mg/setup/atb`/`rtb` isn't read: boot erases it and the gadget is set up
  again.
  An `access_expires_at` in the update (a positive integer, unix seconds;
  any other form is ignored, as other unknown members are) is kept in the
  same commit, in the `done` record, and the next credentials carry it last
  (an app treats credentials without it as expired, so a gadget that drops
  it is refreshed, and commits, on every connection); Link setup stores
  none, and an update without it clears it.
  In a sealed session whose hello carried
  `mg_session_hello_flag_sealed_saved` (echoed: the device's hello flags
  0x03) the answer is sealed too:
  plaintext result || SHA-256(refresh_token)[0:8] (zeros when
  not saved), under the next device counter, framed as credentials
  (first|last|sealed, total_len 29, one write), only after the commit; it
  waits until no data is queued (their counters come first), and an app
  that hears nothing in 35 s assumes saved. Without the flag (an older
  app: its relay didn't count the sealed answer,
  fell one counter behind and parked the session) every answer is the
  legacy one-byte `credentials_saved`, which takes no counter, in a sealed
  session too (host `test_sealed_saved_opt_in`). Host: the
  `sealed_credentials_saved` vectors byte for byte (`test_sealed_saved`)
  and `sealed_data_large` (the shared counter: `test_sealed_data_large`).
  Answered credentials_saved 1, or 0 if a write failed. A pair whose
  stored document (node id, the pair, the device key and binding,
  access_expires_at: what the next credentials send) would be longer than
  2048 bytes is `invalid_value`, checked before anything is stored, so the
  old pair stays and the next hello can still send it.
- **state** open starts a session (a new epoch): `src/mg_vm_link.c` opens
  stream 1, POST /link-control, and sends link.register (node id,
  display_name `MuseGadget-XXXXXX`, platform `zephyr`, the version,
  device_family `gadget`, model_id `muse-gadget-<board>` in DEVICE.md's
  `[a-z0-9-]`, the board its Zephyr board name (`CONFIG_BOARD`, `_` as `-`:
  `muse-gadget-xiao-nrf54l15` on the XIAO), never the uppercase catalog
  code, `metadata.model_name` the product
  name the agent names the gadget by
  (`CONFIG_MG_DEVICE_MODEL_NAME`, else the catalog code, so `XN54`; none on a
  board with neither), the commands); any other
  state drops the streams, the partial message and the queue, and, unless
  it answers an open_request still out, a closed or no network state is
  asked again (mgcommands.h, Session state: a
  gadget with commands must stay reachable): open_request [1] at once,
  then with backoff while it stays connected, whether or not the client
  answers (gadgets, the ESP32 among them, once stopped after the
  first timed ask with a silent client: no_network then silence, asks at
  0, 1, 2, 4, 8, 15, 15 ... s apart; any
  state from the client takes over the timing). But only while the gadget has an active task
  (iOS suspends an
  idle backgrounded app about 30 s after the last traffic, so re-asking
  regardless made a reopen loop all day): a user action in the last 2 min
  (`mg_vm_session_user_action()`, at a talk press), VM traffic it owes
  (`mg_vm_link_owes()`: a request waiting for the next open; an
  unregistered link alone owes nothing), a board's wait
  (`mg_vm_wait_active()`; none on this app), or its own open request (the
  close-then-open after a /link-control reset). With none, nothing is
  asked after closed or no_network and the session stays closed (an agent's
  invoke gets node_offline); a task starting asks at once as its series'
  first ask, or, while that series runs (it has asked since its last
  restart), at the series' next step, and one ending stops the series. While a task is active and the session is open,
  `mg_vm_link.c` sends an empty body chunk on /link-control every
  `CONFIG_MG_VM_KEEPALIVE_MS` (15 s, at most 20; never a new POST); none
  with no task, and `CONFIG_MG_VM_IDLE_CLOSE_MS` (20 s) after the last task
  ends it closes its own session with open_request [0]
  (Android holds the VM socket with pings while a session is open, keeping the
  VM awake); that closed is asked for, so nothing re-asks, and a new task asks
  again; a credentials_update saved while the session has ended asks again
  only with an active task (a token
  refresh on an idle gadget opens nothing). The vectors
  mark a gadget with an active task (the device flag `task`, the `task`
  event; vectors without either describe one that always has a task): the
  replay sets a board's wait for it, and the keepalive is off in it (a suspended iOS app ends its
  sessions at each wake: gaps under a second are normal), in two series
  that never restart each other: after closed 1, 2,
  4 ... s capped at 60 s, after no_network capped at 15 s. An open
  restarts a series only after a session that
  stayed open 60 s; after a shorter one the backoff continues, so a server
  that keeps ending at once is never asked faster (mgcommands.h); a hello or a saved credentials_update restarts both, and a credentials_update saved while the
  session has ended asks again at once, right behind its credentials_saved
  (new credentials may fix what ended it; a new save each time, so no loop). A request the device makes while the session isn't open (a
  heartbeat that comes due, the bench's `l`) waits for the next open, at
  most 10 s, and goes right behind link.register; older, it's dropped
  (logged). Nothing is sent again after a session the client closed,
  even one the VM said nothing after (mgcommands.h, Session state: only GET
  and HEAD would be, and the gadget makes neither); a heartbeat lost with a
  session is followed by the next one on its schedule, and every open sends
  link.register. After not_enrolled,
  auth_failed or too_large nothing is re-asked, not even after a later
  closed, until a new hello, a credentials_update or an open; connecting
  asks nothing, and a 60 s session restarts the series (logged `mg.session: asking for the session again in N
  ms`). link.invoke
  is checked against the command's parameters (required ones present,
  declared ones of their type) and answered link.result `ok` with the
  handler's payload, or `ok: false` with an `error` string; `busy`, and the
  handler doesn't run, while the send queue might not hold its result (the
  client isn't giving credit). If the VM resets or refuses
  /link-control, which ends a Wi-Fi gadget's session, the gadget asks the
  client for a new one (open_request 0, then 1) after the ESP32's reconnect
  backoff (1 s, doubling to 15 s), starting over at 1 s only after a
  /link-control that stayed up 60 s past `registered`, so a server that registers and then resets is asked at 1, 2, 4,
  8, then every 15 s, never faster and never stopping. A clean end (a 200 head, then the body's
  end) asks nothing itself: the client reports closed and the session's
  re-ask reopens, one path (`test_control_clean_end`). After a
  `registered` reply it sends link.heartbeat every
  `CONFIG_MG_VM_LINK_HEARTBEAT_S` (a day, as the ESP32), each logged
  `mg.link: heartbeat`.
- **Messages**: one at a time each way, fragmented to ATT MTU - 3, within
  the client's window; a VM message's bytes are given back once all of it
  has arrived and been handled, and those of one larger than the window
  (with `CONFIG_MG_VM_SESSION_WINDOW` below the maximum message) as its
  fragments land, or it could never finish.
  Every reply fits what it goes through: request_status's
  replies are built in one buffer sized by `BUILD_ASSERT`s (the capture-rate
  list's worst case; a settings list that wouldn't fit is cut short only
  with a count and an error), get_settings' longest answer is asserted to
  fit one notification (on a link whose ATT MTU holds less, an answer that
  doesn't fit is `invalid_length`), and a notification the stack refuses (longer than
  the link's ATT MTU allows, or any other error) is counted and logged,
  never dropped quietly; the NUS `status` line ends `dropped N` and the
  bench `s` says it too. Unit test_status_replies_fit.
  A turn's gestures and stop_mic are never dropped: they wait
  for a queue entry until they get one or the link is gone; other Control
  replies wait 500 ms, and one dropped then is counted (bench `s`) and
  logged. In BabbleSim three request_status during a hold (twelve in a
  stress run) all got their replies and the turn ended normally.
  A central's traffic never holds the app work queue
  (`src/mg_evt_policy.h`, host `test_evt_policy`): the event handler takes
  at most 4 events or 50 ms a run, then queues itself behind the rest (the
  watchdog's ping, the healthy mark); the replies to one event wait for
  buffers at most 1 s together; request_status, which needs no proof, is
  answered at most 4 times a second per connection (more are counted and
  logged); and an app work queue stale while peer events have been backed
  up 4 s with a link up, the event handler still taking them (within those
  4 s), is a peer loss (`peer: event queue saturated`: a soft restart that
  never counts toward SAFE; past the bound the link goes and the queue gets
  a fresh window), not a crash; a queue stuck in anything else leaves the
  events untaken and stays a crash.
  Nothing here waits for the notification queue (`mg_session_try_control`):
  the app work queue that sends is the one that takes the client's writes
  off the event queue, so a wait there could drop them. The hello reply,
  the credentials (never before the hello), credit, credentials_saved (the
  commit isn't redone) and data that didn't fit go out 20 ms later. A
  client write on Control dropped because the event queue (16 deep) was
  full would lose session bytes and their credit unseen, so with a hello on
  the connection the gadget disconnects.
  Data, credit and credentials_update before a hello, or data while not
  open, get `invalid_value`. Client data whose (opened) plaintext isn't a
  ServiceResponseEnvelope (noise_core ServiceCodec's rules) is
  `invalid_value`, its credit back and its counter used, so a clear or
  credentials_update plaintext relabeled as data fails as data; from a
  sealed clear's erase on, data and credentials_update are refused (credit
  back, no counter, nothing stored), so the clear's [1] still means erased.
  A credentials_update's plaintext starts with `{` (no whitespace before the
  object). A token proof `clear` (any setup erase) or a
  new challenge ends the session at once: nothing it queued goes out.
- **Relay placeholders** (the Muse apps relay session data both ways between
  the gadget and its VM, the phone a transparent pipe): anything inbound
  without a handler is taken and logged with its kind and size (`mg.link:
  in: ...`: a frame on a stream that isn't open, an unknown frame kind, a
  control message other than link.register's reply, link.invoke or an
  event), and its bytes go back as credit like any other; nothing unknown
  is an error. Out: link.register at every session open (logged), and the
  bench key `l` sends one control message, `{"method":"link.heartbeat"}`
  (`mg_vm_link_send_test()`), for testing a relay.
- **Device commands** (`src/mg_vm_commands.c`): `device.health` (overall,
  metrics with uptime and battery, version, as a Wi-Fi gadget) and, with an
  LED, `device.led` (r, g, b 0-255, optional duration_ms; any colour lights
  the XIAO's one LED, all 0 turns it off, then it shows the link again);
  never over the mic's light: while the mic captures or records offline the
  capture light shows, and the command only after (host
  `test_led_command_vs_capture`).
- **Codec**: `src/mg_envelope.c` is a small C codec for the envelopes and
  service frames, not `esp32/components/noise_core` itself: that is C++17
  (Zephyr's C++ support and library for 1,700 lines of ServiceCodec.cpp)
  and mostly Noise, which the phone runs, not the gadget. The host tests
  check the C codec against ServiceCodec.cpp directly.
- **Encryption**: off (`CONFIG_BT_SMP=n`): sealed credentials need no bond,
  and a bond opens nothing, so there is no Security Request and
  no bonding overlay. The gadget stores no OS bonds: `src/mg_ble.c`'s build
  checks need `CONFIG_BT_SETTINGS=n`, and `CONFIG_BT_BONDABLE=n` for a board
  that turns SMP on. A future feature that needs a bond (BLE encryption,
  ANCS, AMS, HID) brings its own option with it: LE Secure Connections only,
  bondable off at boot and only inside setup or an owner action; none
  exists yet.

RAM: the session takes no base RAM. Its buffers (the 16 KB receive buffer,
the 4 KB send queue, the credentials both ways, /link-control's both ways
and three JSON member tables, about 30 KB with the defaults) are one block
from the malloc arena (`CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE`, all the RAM
no static data uses), taken at a hello that's answered and given back,
wiped, when the connection ends (a disconnect, a new token proof challenge,
a setup erase); a hello that finds no memory waits unanswered,
retried every 100 ms, and is dropped with no answer after
30 s or at the connection's end; nothing is taken meanwhile, so a sealed
hello's nonces stay. Firmware updates take their memory from the same arena only while
they run, and it holds both. The board build is 728 B of RAM over the build
before the device session (106,596 B; 105,868 B before that), and
about 18 KB of flash (323,364 B).

### The BLE link

These are in `prj.conf` and `Kconfig.mg`, so every board this app builds for
(the XIAO and any other nRF54L15) gets them, not just the XIAO's files:

- **2M PHY and 251-byte PDUs.** Right after connecting the gadget asks for
  the 2M PHY (`bt_conn_le_phy_update`; `CONFIG_BT_USER_PHY_UPDATE`,
  `CONFIG_BT_CTLR_PHY_2M`), the largest data length (`CONFIG_BT_CTLR_DATA_LENGTH_MAX=251`,
  `CONFIG_BT_USER_DATA_LEN_UPDATE`) and the largest ATT MTU, and logs what it
  gets: `mg.ble: phy tx 2M rx 2M`, `mg.ble: data length tx 251 B / 2120 us …`.
  A central that starts its own PHY or data length update at the same time
  (phones often do; so does the BabbleSim test central) makes the controller
  refuse ours (logged as "already under way"); the outcome is logged all
  the same. If no PHY update has come 3 s after connecting, 2M is asked
  once more (on a Galaxy S22 link the request collided and the link stayed
  on 1M, ~24 kB/s against the iPhone's ~94 kB/s on 2M).
- **A peer that vanishes mid-send** (`src/mg_bt_txfix.c`): Zephyr
  v4.4.2's host, when an ACL send fails because the controller already let
  the link go ("Invalid Tx Enqueue"), disconnects from its own TX processor,
  whose `bt_hci_cmd_send_sync()` then waits for a command only that thread
  sends: 10 s later "Controller unresponsive, command opcode 0x0406" asserts
  and the gadget restarts (the Muse app killed mid-link; 1 in 7 kill -9
  drops on the bench). `--wrap=bt_send` learns that thread from conn.c's
  ACL sends and `--wrap=bt_hci_disconnect` sends a disconnect asked for on it
  from the system work queue instead (logged `disconnect asked on the
  Bluetooth TX thread`), one per link (a second link's is never dropped);
  every other caller is unchanged. Any board on
  Zephyr's host with `CONFIG_BT_TX_PROCESSOR_THREAD` (the SoftDevice
  Controller builds too). Unit `bt_txfix`.
- **Link modes (mgcommands.h, Link modes).** Several
  gadgets share one phone's radio and the central serves every link at its
  interval whatever the peripheral latency, so only the gadget in use holds
  15 ms (`src/mg_connparam.c`). Three modes, each a real connection
  parameter update, with supervision timeout 400 (4 s,
  `CONFIG_MG_CONN_TIMEOUT`), sleep's 600 (6 s, `CONFIG_MG_CONN_SLEEP_TIMEOUT`):

  | Mode | Set | Kconfig |
  |---|---|---|
  | sleep | 100-160 x 1.25 ms (125-200 ms), latency 4, timeout 6 s (200 left too little slack at a 240 ms grant; a gadget dropped overnight in sleep) | `CONFIG_MG_CONN_SLEEP_*` |
  | ready | 24-36 (30-45 ms), latency 5; never 15 ms | `CONFIG_MG_CONN_IDLE_*` |
  | active | 12-12 (15 ms, exactly: iOS grants 15 ms only for 12/12), latency 0 | `CONFIG_MG_CONN_INTERVAL_*`, `CONFIG_MG_CONN_LATENCY` |

  - **Ready at connect**: once PHY and data length are done (or after 2 s),
    and never at connect: after the central's own first update, or 1 s
    after connecting if none came (not at all when the central connected
    with it, or moved to it first).
  - **Active on the gadget's own activity**, asked at its start
    (`mg_connparam_busy()`, called again while it lasts): a talk press at
    touch-down (so the pre-roll never waits an interval), the mic (a
    client's `start_mic` too), a clip download, any SMP request (an
    update), a throughput test, set_tokens; and Link setup from
    `pairing_client_hello` until it ends or fails (`mg_connparam_hold()`,
    the press wait included). Not activity: a GATT write alone, console
    input on the Nordic UART, assistant_state.
  - **Back** `CONFIG_MG_CONN_IDLE_AFTER_MS` (5 s) after the last activity
    ends, to the client's mode (ready unless it wrote sleep); after
    `CONFIG_MG_CONN_SLEEP_AFTER_MS` (10 min) in ready without activity it
    sleeps by itself, until activity or a client write.
  - **Who holds the link (settings 18 `client_name`, 19
    `client_app`):** per connection, empty at every connect, never stored;
    read back with get_settings; an over-long value (name over 32 bytes,
    app or version over 24) is `invalid_value`, a cut-short one
    `invalid_length`; 19 is in set_settings' typed list, 18 isn't (reserved:
    unlisted on every board). The gadget
    has no status page: the bench `s` (USB) says `connected: <name> (<app>
    <version>)`, the name the central's own GAP Device Name (0x2A00, read
    once per connection after the MTU exchange; logged by length only),
    else `phone` (`client_name` is reserved: the apps don't send it;
    it's still accepted and read back) (`app unknown`; `connected: none`
    without a link), and the Nordic UART's `status` says `connected:
    <app> <version>` without the name (the Android app relays that
    console into its logs, and the phone's name is the user's).
  - **The client's setting 16** (`link_mode`, uint8, per connection, ready
    at every connect, never stored): a write of active makes it active
    ahead of a stream, lapsing to ready after
    `CONFIG_MG_CONN_CLIENT_ACTIVE_MS` (30 s) without activity; ready or
    sleep take effect once the gadget's own activity ends. A read returns
    the effective mode, and every effective change is notified unsolicited
    as `[get_settings, link_mode, mode]`. Setting 17 (`link_params`,
    read-only, `invalid_value` on a write) is the last interval, latency
    and timeout the controller reported (uint16 each, little-endian; 0s
    before any). Both are in the typed settings list.
  - **During a switch the gadget hears every connection event**
    (`CONFIG_MG_CONN_SWITCH_LISTEN`, default y with Zephyr's open
    controller): from the request until the grant it
    cancels the peripheral latency skip twice an interval, through the
    controller's own `ull_periph_latency_cancel()` (what its TX path
    uses), then latency resumes. On the XIAO with a Mac central (10
    switches each): sleep -> active median 6.8 s without, 3.4 s
    with (the Mac's own ~3.44 s schedule is the floor); sleep -> ready 4.2
    s -> 3.5 s; ready -> sleep 0.87 -> 0.64 s; about 11 extra events heard
    per switch. Every switch logs `mg.ble: switch A -> B applied in N ms
    (... listen on, M cancels)`; the bench console's `>listen=0|1` and
    `>mode=0|1|2` (the client's link mode, as if written) drive the A/B
    while a central holds a quiet link (no subscriptions); the Nordic UART
    console takes the same as `listen 0|1` and `mode 0|1|2` (boards
    without a UART console). With the nRF Connect SDK's SoftDevice
    Controller (a board built with NCS) the same can be done by its per-connection
    peripheral latency mode (`hci_vs_sdc_peripheral_latency_mode_set`,
    0xfd09): disabled at the first cancel, held three intervals (at least
    100 ms) past the last, then enabled again; a disconnect ends it. It's
    off by default there: on an nRF54L15 board with a
    Mac central (10 rounds each) both ways sat at the central's 3.2 s floor,
    so it gained nothing for 27-51 extra wakes a switch.
  - The set the link is already in is never asked again (a Mac
    rejects it, and Zephyr's controller completes it without telling
    either host, so neither re-plans the link). At most one request per
    2 s (`CONFIG_MG_CONN_REQ_GAP_MS`), so a mode change never follows the
    previous grant back to back (at least 500 ms); a request not
    granted within 5 s is sent once more, then the central's choice stands
    until the next change. All three sets meet Apple's accessory rules
    (15 ms when min = max, else max at least 15 ms above min; latency ≤ 30;
    2 s ≤ timeout ≤ 6 s; max interval × (latency + 1) ≤ 2 s and × 3 <
    timeout).
  - GAP's Peripheral Preferred Connection Parameters characteristic, which
    iOS reads, carries the active set (`CONFIG_BT_PERIPHERAL_PREF_*`), and
    the host's own automatic update is off
    (`CONFIG_BT_GAP_AUTO_UPDATE_CONN_PARAMS=n`) so nothing else asks for
    other values. The host holds a peripheral's request until 1 s after
    connecting (`CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT=1000`, the Core spec's
    minimum pause; Zephyr's default is 5 s). Zephyr's open controller
    (`BT_LL_SW_SPLIT`, what this SDK links on upstream Zephyr v4.4.2) has
    no local latency switch, which wouldn't count as idle anyway.
- **Checking it.** All of these lines go to the console and the Nordic UART
  mirror (`python3 esp32/tools/mg_ble_client.py log`): after a connect,
  `mg.ble: phy tx 2M rx 2M`, `mg.ble: data length …`, `mg.ble: link ready
  (connected)`, `mg.ble: asking for 24-36 x 1.25 ms, latency 5, timeout
  4000 ms (ready)`, then each grant, `mg.ble: conn 30.00 ms latency 5
  timeout 4000 ms`; at a press `mg.ble: link active (talk press)` and its
  request; 5 s after `mg.ble: link ready (activity over)`; 10 min later
  `mg.ble: link sleep (activity over)`.

### Throughput

| Direction | What limits it | Settings |
|---|---|---|
| Phone -> gadget (SMP upload) | SMP round trips, latency, then the central's packets per event | Active (15 ms, latency 0) while an upload runs (above). SMP (`boards/xiao_*.conf`): `CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY` with four 512-byte buffers, so one request spans two or three ATT writes, and `CONFIG_MCUMGR_GRP_OS_MCUMGR_PARAMS` tells clients that size (smpclient fills it; nRF Connect Device Manager and the Muse apps too). 512 at most: the Android app sizes requests to it and cuts them into MTU - 3 writes, and Android refuses a write over 512 bytes. `CONFIG_IMG_BLOCK_BUF_SIZE=4096`: RRAM is written through the radio-synchronised flash driver (`CONFIG_SOC_FLASH_NRF_RADIO_SYNC_TICKER`, which waits for a gap between connection events), so bigger blocks mean fewer waits. `CONFIG_IMG_ERASE_PROGRESSIVELY=y`: without it img_mgmt blanks the whole image's size before answering the first chunk, about 38 s for 320 KB through that driver, which the Android app gives up on (4 x 5 s) and iOS nearly (30 s); RRAM needs no erase, and progressively only MCUboot's trailer is blanked before the first write. The controller's and host's receive buffers stay at Zephyr's defaults (`prj.conf`): eight and seven more (3,776 B) made no difference on the XIAO (upload and Link setup times) or in BabbleSim. |
| Gadget -> phone (clip download, live audio) | the central's packets per event; then 2M airtime | Up to ten notifications handed to the stack at once (`TX_INFLIGHT` in `mg_ble.c`) within `CONFIG_BT_CONN_TX_MAX` and `CONFIG_BT_BUF_ACL_TX_COUNT` (12, also the controller's TX buffers). One 251-byte PDU and its acknowledgement take 1.4 ms at 2M, so a 15 ms event carries at most about ten: 160 KB/s. A Mac took about four per event (65 KB/s) with six in flight. Live SBC is 7.5 KB/s. |

RAM for this: the four SMP buffers
(2 KB) and the image block buffer (4 KB) only during an update (from the
malloc arena). In BabbleSim (2M PHY, 15 ms, 251-byte
PDUs both ways, latency 0) the throughput test moves 171 KB/s gadget ->
central and 163 KB/s central -> gadget: the airtime limit, about ten PDUs
an event.

Measure it with the bench client: `dfu` prints the upload rate and each
phase, `queue --download` each clip's download rate, and the gadget logs
`mg.ble: link active (dfu)` at an upload's start and `mg.ble: link ready
(activity over)` 5 s after its end.

Raw link throughput: `device_action throughput_test` (mgcommands.h,
Throughput test; `src/mg_throughput.c`). It runs for any central on every
build, proof or not (mgcommands.h, Token proof: the display and test
commands stay open; unit `test_privileged_need_proof`). Send streams numbered MTU-3 byte
notifications from a thread of its own, the notification queue kept full;
receive counts the client's Data writes with latency off for as long as
they come. Each logs `mg.ble: throughput send|receive: … B in … packets, …
ms (… B/s), … gaps` and reports it to the client. `python3
esp32/tools/mg_ble_client.py bench --send 10 --recv 10` runs both and
prints the rate seen on each side, losses, and packets per connection event
(from how notifications bunch up on arrival).

Kconfig options live in `Kconfig.mg` (`CONFIG_MG_*`): SBC bitpool, LC3 frame
size, audio source, mic gain, queue size, the setup reset hold, firmware
updates, the bench console.

### Firmware updates (MCUmgr SMP)

The SMP GATT service sits beside the mg service, DIS, BAS and NUS, with the
image group (list, upload, test, confirm, erase) and the OS group (echo,
reset, info), so nRF Connect Device Manager, `mcumgr` / `smpmgr` and
`esp32/tools/mg_ble_client.py images|dfu` can update it. Upload
`build-*/zephyr/zephyr/zephyr.signed.bin`. The OS group's MCUmgr parameters
report 512-byte buffers (no more: Android refuses a write over 512 bytes,
and the app's writes are as long as a request), four of them; the bench client fills each request
and keeps two in flight (Zephyr's SMP transport handles queued requests in
order, one buffer holding the response and one the request being
reassembled). The Muse apps size
requests the same way: Android keeps up to three in flight (three requests
and one response: the four buffers), iOS two.

How updates behave (`src/mg_ota_policy.h`; `tests/smp_link` runs them
against Zephyr's MCUmgr):

- **An update from an unproven central takes one press, never a token**
  (`protocols/test-vectors/mg-update-v2.json`, replayed by host
  `test_update_vectors`): on a release build, set up or not (a gadget that
  holds no device token, not set up or after Forget, asks the same), an
  update write (image upload, state write, erase, OS reset) from a central
  without this connection's token proof,
  in normal mode, is answered `MGMT_ERR_EBUSY` (10) and starts the prompt:
  the LED's "update confirm" (two short blinks every 0.8 s) for 30 s, on
  that connection (`mg.ota: firmware update asked without the token proof:
  press the button once`), at most once per 10 s per connection
  (`MG_UPDATE_REARM_MS`, as the re-setup prompt; a write that would show
  it sooner gets 10 with nothing shown). ONE physical press of the talk button (or a
  board's update key, `mg_core_update_key()`) while the LED shows that cue
  (`mg_led_update_cue_showing()`: no setup prompt, reset or SAFE cue or
  mic light above it) and no setup confirm is pending (a pending setup
  confirm takes the press first, so a setup-prompt press never approves a
  hidden update) confirms the upload whose write started the prompt and binds the press to its image
  (length and whole-image SHA-256) for 600 s (`MG_UPDATE_CONFIRM_MS`)
  across link drops: that upload's chunks, its state write, an erase and
  the reset on its link go on, and the same upload started again on a new
  link (its first chunk, img_mgmt's resume query, the same length and
  SHA-256) goes on there with no new prompt; another upload, or the same
  one started again after the 600 s, on its link or another, asks again (a
  transfer under way still finishes). Each unproven update write is read
  from its own request before img_mgmt or os_mgmt acts on it
  (`src/zephyr/smp.c` change 4, `mg_upd_parse()`): an upload's first chunk
  that names no SHA-256 gets 11 and no prompt, and so does a chunk with no
  upload started on its link; a write past the bound length gets 11, and
  the last chunk (slot1's bytes, the image writer's buffer and the chunk,
  hashed before it's written) and an image state write (slot1's first
  length bytes) are taken only if they hash to the bound SHA-256; refused,
  each ends the binding (the claim alone is only the client's word). A
  press on a prompt an erase, the reset or a state write started covers
  only that request (its group, id and body) on its link, for 600 s. A SAFE the gadget entered by itself
  (failed boots, a crash guard) has no way to prompt: on a release build
  its update writes get 11, never asked, set up or not
  (`mg_ota_smp_closed()`); reads stay open, and the way back is the
  button's SAFE or SWD. Only a press queued after the prompt started
  counts; the prompt (and its light) ends with its link; a setup's commit
  and Forget (`mg_setup_store_erased()`) drop the prompt, the binding, the
  covered change and the transfer the link's proof started (its proof ends
  with the token; the wired console's arm stays); the prompt's blink never covers the
  capture light (it shows after the capture).
  Unanswered, the prompt ends at 30 s and the next update write gets
  `MGMT_ERR_EACCESSDENIED` (11) once; the one after starts a new prompt.
  Only a real press answers it, never injected input (bench and automation
  builds never prompt).
  Known limit: on a release build, set up or not, a nearby device can start
  a firmware update without the token proof, and the owner's next physical
  button press (the talk key included) installs it. Where images are
  signed, they are signed with the repository's development key, which is
  public, so a valid signature doesn't show who built the image.
  `struct mg_upd` (`src/mg_ota_policy.h`) is the state;
  the Nordic UART's `status` says `update: idle|press|admitted`. A web
  updater needs no phone.
- **What needs no press**: a connection that passed its token proof (the
  owner's app; never a gadget's lack of a device token: one not set up, or
  after Forget, asks as a set-up one does); SAFE asked with the button at
  power-up (the button recovery tier,
  `mg_health_safe_by_button()`, a RAM flag the boot hold sets), so SMP is
  still the BLE recovery tier; the wired USB console's `>update.window`
  and the Nordic UART's `update window` (automation and bench-mode builds;
  a release build refuses the word, proven or not), which arm the
  connection for 60 s (`MG_UPDATE_WINDOW_MS`, logged `update armed`); and
  every bench or automation build (`overlay-bench.conf`), set up or not set up,
  a bench-mode image without automation included (`mg_ota_release_build()`).
  The press is for a person with a release image; a bench or automation
  run never waits for one. Reads (the image list, params, info) and echo
  stay open to anyone. Refused writes are logged (`no token proof: waiting
  for one button press`, or `the press prompt ended unanswered`) and
  counted by `mg_ota_smp_refused()` (`mg_ota_smp_allowed()`, host
  `test_update_window`, `test_update_vectors`; smp_link's `--release`
  plays the release rule in `Gate`, `CarryOn`, `Owner`, `SourceGen`). A
  board with a dedicated update key (`CONFIG_MG_UPDATE_KEY`, off by
  default) answers the prompt with one press of it; the XIAO's one key is
  its talk key. The XIAO has no streams to it (no speaker or display), so
  the rule that they need the proof has nothing to gate here. A malformed
  MCUmgr hook argument fails closed (11).
- **The capture light is on before the mic starts**: the audio thread sets
  the light, then waits up to 200 ms for the LED tick to have driven the
  capture pattern on (`mg_led_capture_wait()`) before it starts the mic;
  no light, no capture (counted, logged). Live capture and offline
  recording show the same solid light.
- **No log line formats a secret**: every log line can reach the Nordic
  UART mirror, so `tools/check_log_secrets.py` (a build target, and host
  run.py) fails a hex-buffer log macro or a `%s`/`%p` argument named like a
  token, key, nonce, MAC or credential.
- **The talk button's pin is only read once gpio-keys owns it**: gpio-keys
  configures sw0 and arms its trigger at boot, and reconfiguring the pin
  removes that trigger in Zephyr's nRF GPIO driver (every press after it is
  lost), so the boot reads (`mg_button_held_at_boot()`, the boot hold,
  `mg_power_boot_gate()`) configure it only on a board where gpio-keys
  doesn't own it (`MG_SW0_GPIO_KEYS`); System OFF's wake arm, right before
  power-off, is the one exception. `tools/check_button_pin.py` (host
  run.py) fails any other reconfigure. gpio-keys reports edges from boot,
  so button events count only from `mg_button_init()`, once the core, the
  session and Link setup are up (main.c); an edge before it is dropped,
  never queued (its work item is defined statically, never left zeroed:
  host `test_button`).
- **An allowed transfer finishes on its connection**: once an upload passed
  that check, the rest of it isn't asked again on the same connection, even
  if the proof lapses (the client challenges again): its chunks and a retry
  from offset 0 with the same len and sha, then, once its last chunk is in
  (img_mgmt's DFU pending), the image state write and the OS reset. MCUmgr's
  upload check hook records it at its first chunk (the connection's
  generation, `mg_transport_conn_gen()`, its size and sha) and holds each
  later chunk to it: another len or sha gets 11. An erase never carries
  over, and a link drop or Forget ends it: a resume on the next connection
  is asked again, unless a press bound that image (above)
  (`mg_ota_smp_carries()`, `mg_ota_smp_same_transfer()`; host
  `test_smp_carry`, smp_link's `CarryOn`). The command hook decides each
  unproven upload on its own request, before img_mgmt answers a resume's
  offset query, so a continuation chunk on an unproven connection is
  refused unless the bound upload started again there (above). Each SMP
  request carries the connection its first fragment came on
  (`src/zephyr/smp.c` change 3, `mg_smp_request_gen()`; the board gate below
  the same way), a partly received request whose connection isn't the link
  any more is dropped before the next write is collected
  (`mg_smp_rx_orphan()`: a central that never subscribed leaves its partial
  request behind, and GATT's cleanup skips it), and a request whose
  connection isn't the link any more is refused, so one still queued or half
  received when its link dropped never borrows a later connection's proof
  (host `test_smp_source`, smp_link's `SourceGen`). A write that packs two requests (a read and a
  reset) never runs either: Zephyr's SMP transport reassembles one request
  per write and drops the overflow.
- **A client's confirm before the boot is healthy** (30 s up and
  advertising, threads alive) is answered OK at once and takes effect at the
  healthy mark. In SAFE mode of either kind a client's confirm of the running
  image is refused (a SAFE boot isn't the image working, and the button's SAFE
  can run a test image just swapped in; host `test_set_next`); the other
  slot's test or confirm still passes. img_mgmt changes the next boot only through
  `boot_set_next()`, which the build wraps (`-Wl,--wrap=boot_set_next`);
  every other state write passes straight through, and so does the gadget's
  own confirm (from the app work queue).
- **A test image keeps itself at the healthy mark once a link has proved
  ATT works, or a central read its image list** (`mg_ota_confirm_ready()`,
  the Keep signal): healthy (board-ready included) AND either a completed
  ATT exchange through the gadget's own path this boot (the reachability
  rule under Recovery, below; on a set-up gadget on a link past its token
  proof) or an SMP image list / state read this image answered after the
  boot, from any central (the web updater's post-install check; taken when
  the read is answered, logged `a central's image list read was
  answered`). Never on advertising or a timer alone, so
  an image no central can talk to stays on test and the next reset reverts
  it. The Muse apps
  confirm right after the reset when the image isn't confirmed yet; that
  takes effect at the same point. Not in SAFE mode (a test image never
  gets there: MCUboot swaps it back at the first unhealthy reset). An image
  still on test after 30 min of counted time restarts on its own, so
  MCUboot reverts it (peer losses never restart an image on test, so a
  broken one would otherwise only drop links for good). Time counts only
  with no link or a token-proven one, so a stranger holding the single link
  can't run the clock down on a good update; a connection or disconnection
  at any point in a 10 s tick pauses that tick even if the link is gone by
  the tick (each one latches it), and so does a link up unproven at the tick
  before (it held the interval's start), so one link spanning a tick and
  dropping just before the next is never charged (`mg_ota_trial_unproven()`).
  4 h of uptime reverts an unconfirmed trial regardless, an
  upload moving or not. Local failure (a crash, the watchdog, never
  healthy) reverts at once through MCUboot. An upload moving (a chunk
  accepted in the last 60 s; a refused one doesn't count) defers the 30 min
  restart, a newer image staged in slot1 (MCUboot's TEST or PERM) stands
  it down, and a confirm ends it; the trial image itself reads MCUboot's
  REVERT (copy done, not yet kept) and keeps counting
  (`mg_ota_trial_tick()`, the whole tick; host `test_trial_tick`).
- **A lost write doesn't wedge SMP.** A request spans about ten ATT writes,
  which Zephyr reassembles by the length in the SMP header, with no timeout:
  one lost write (the iOS app writes without response and doesn't wait for
  `canSendWriteWithoutResponse`) would leave the partial request waiting and
  every retry appended to it. A write that comes more than
  `CONFIG_MG_SMP_RX_GAP_MS` (2 s) after the previous one drops a partly
  received request first (wrapping `smp_reassembly_expected()`, which
  `smp_bt` asks before it collects each write, so the next request gets its
  connection; and `smp_reassembly_collect()`, for the time); a request's
  writes come milliseconds apart and clients retry after seconds (Android
  5 s, iOS 30 s).
- **No base memory.** SMP takes RAM only while
  it is in use: each request and response buffer from the C library's
  malloc arena (all the RAM no static data uses,
  `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1`) until it is sent; MCUmgr's work
  queue from the first request until nothing has been about for
  `CONFIG_MG_SMP_SESSION_IDLE_MS` (2 s), on a thread of its own (the app
  work queue is never held by flash writes); the image writer (with its
  4 KB block buffer) from an upload's first chunk to its last, or to the
  session's end for an upload that stopped (a dropped link): its buffered
  bytes are dropped, img_mgmt's offset moves back to what is on flash, and
  the client resumes from there with a fresh writer. Zephyr keeps all three static, so
  `src/zephyr/smp.c` is a copy of Zephyr v4.4.2's with five marked changes
  (the two for memory, each request's source connection, the request's
  bytes for the update gate, and the nesting limit below; built in place
  of upstream's, `CMakeLists.txt`), and
  `src/mg_smp_mem.c` wraps `img_mgmt_write_image_data()`. Static RAM for
  SMP: 616 bytes (was 11,176; free RAM with no update running is the SMP-off
  build's minus that): Zephyr's own statics (img_mgmt's upload state,
  os_mgmt's reset work, smp_bt, the pool's registry entry), and the
  session's pointer, lock and idle timer, which can't live in the block they
  free (Zephyr's work queue touches a work item after its handler returns). The `smp_link` suite checks that nothing is held
  when idle.
- **No request nests deeper than 8, and none that isn't whole CBOR.** zcbor
  skips a map value a handler doesn't know by recursing once per nested
  array or map, with no limit of its own, so an echo (open to any central)
  of ~500 nested arrays in one 512-byte request overflowed the SMP work
  queue's 4 KB stack. The marked `src/zephyr/smp.c` walks every request's
  CBOR first, iteratively and with indefinite arrays and maps
  (`src/mg_smp_depth.h`, `MG_SMP_MAX_DEPTH`), and drops the packet before
  any handler decodes it (logged `an SMP request not whole CBOR or nesting
  deeper than 8: dropped`; no answer) unless every request in it is whole,
  well-formed CBOR inside its own length and nests 8 deep at most. The walk
  fails closed: a length, count or tag of 2^32 or more (zcbor's length
  check casts it to a 32-bit `size_t` on the nRF54L15, so 2^32 skips
  nothing and the decode goes on into what follows), one past the request's
  end, a reserved head, an indefinite string, a stray break or anything
  left open is refused, never left to the handler; an empty body only as a
  packet's last request (Zephyr's reader runs to the packet's end). No
  request the apps or the web updater send nests deeper than 3. Host
  `test_smp_depth`, smp_link's `Gate.test_deep_request_dropped` and
  `Gate.test_long_length_deep_echo_dropped`.
- **The first chunk is answered at once** (within
  5 s). With `CONFIG_IMG_ERASE_PROGRESSIVELY` img_mgmt erases nothing up
  front; flash_img only blanks MCUboot's trailer before the first write.
  Without it, img_mgmt blanked the whole image's size first: on the XIAO
  (from a Mac) the first chunk waited about 38 s, a 320 KB
  upload took 63 s, and the Android app's flow gave up ("Upload stalled",
  4 x 5 s). With it: first chunk 0.1 s; the Android flow 18-20 s (16-18
  KB/s), iOS 23 s, reset to reconnected 7-8 s; resume from half (both
  flows), a corrupted file kept out by MCUboot, a NO_CONFIRM image swapped
  back, and clips dropped at the upload and recorded again after it, all
  on that board. The default BT RX buffers (1/1) upload as fast as 8/7 did there
  (16.4-16.9 against 16.3-17.8 KB/s).
- **No fixed table a way back shares**: MCUmgr's groups (OS, image) register into a linked list at
  init and the GATT services (mg, Link setup, DIS, BAS, NUS, SMP) are all
  static (no `CONFIG_BT_GATT_DYNAMIC_DB`), so nothing optional can crowd the
  recovery tier out of a table; mg's command, Nordic UART and bench words
  are switches and const lists, nothing registered. The fixed pools SMP
  shares are asserted at build time: mg's notifications leave two of the
  host's ATT, L2CAP and TX-context buffers (`mg_ble.c`), SMP keeps at least a
  request and a response buffer of at most 512 bytes (`mg_smp_mem.c`), and
  `tools/check_arena.py` fails a link whose malloc arena (the session's and
  SMP's memory) is under 48 KB (the XIAO's: 150 KB).
- **The SMP reset** answers, then at 100 ms
  (`CONFIG_MG_DFU_RESET_DISCONNECT_MS`) stops media and advertising and drops
  the link itself (the phone sees the disconnect at once instead of
  after the 4 s supervision timeout, and can't reconnect to the old image),
  and restarts at 500 ms (`CONFIG_MCUMGR_GRP_OS_RESET_MS`).

`CONFIG_MG_BENCH_NO_CONFIRM` (bench only) makes an image never keep itself;
only a client's confirm keeps it, so without one MCUboot swaps the previous
one back at the next reset: build a test image with it to show a revert,
never ship it.

On a release build, set up or not, update writes from a central without the
token proof need one physical press on the gadget (the update prompt,
above); the button's SAFE, the wired console's arm and every bench or
automation build need none.

A board whose images go through its own loader can keep MCUmgr's other
groups over Bluetooth without `CONFIG_MG_DFU` with `CONFIG_MG_SMP_GATE_BOARD`
(no image group with it, nor the settings, file system or shell groups):
on a set-up gadget `src/mg_smp_gate.c` refuses every SMP command, read or
write, without this connection's token proof (SAFE: only SAFE asked with
the button), except the OS group's harmless status (echo, task and memory
pool statistics, MCUmgr's parameters, OS and bootloader info;
`mg_smp_gate_allowed()`) and a request the board takes on itself
(`mg_board_smp_gate_defers()`, weak, false by default), then asks the board's
`mg_board_smp_write_allowed()` about each write, which has no default, so a
board that forgets it doesn't link (host `test_smp_gate`). A request whose
connection isn't the link any more is refused before any of that, and again
after the proof and the board's gate are read (a link that changed while it
was decided), so one still queued when its link dropped never borrows a
later connection's proof: `src/mg_smp_req_gen.c` stamps each request with
the connection its first fragment came on where the transport hands it
over and reads it back while its handlers run (Zephyr's `smp_rx_req()` and
`smp_process_request_packet()`, wrapped at link time; `src/zephyr/smp.c`
isn't built here), and with SMP reassembly drops a partly received request
whose connection is gone before the next write is collected, as the
updates' build does (`smp_reassembly_expected()` and
`smp_reassembly_collect()` wrapped). The nesting limit holds here too:
the `smp_process_request_packet()` wrap drops a packet `mg_smp_depth_ok()`
refuses before Zephyr's request handling sees it (the gate lets the echo
through unproven; host `test_smp_req_gen`). This build has
no receive-gap check (`CONFIG_MG_SMP_RX_GAP_MS` comes with `CONFIG_MG_DFU`):
a partial request on its own connection waits for its bytes, as upstream's
does. With neither option the build refuses SMP over Bluetooth
(`CMakeLists.txt`).

### Offline clips and updates share slot1

The clip queue (`src/mg_queue.h`) lives in `slot1_partition`, which updates
need too:

- **Clips own it** while MCUboot needs nothing there. The queue never writes
  the last 12 KB (`CONFIG_MG_QUEUE_SLOT1_TRAILER`; MCUboot's trailer for 178
  sectors is 8.6 KB), and offset 0, where MCUboot looks for an image header,
  only ever holds a clip header (`MGQ1`) or blank cells. That leaves 700 KB,
  about 90 s of SBC.
- **An upload's first chunk hands it to OTA** (img_mgmt's DFU-started hook,
  before anything is written): recording stops, the clips are dropped and
  logged (`firmware update takes the clip store: N clip(s), B bytes
  dropped`). The app should download clips before updating. An image erase
  hands it over only once img_mgmt has accepted the erase
  (`-Wl,--wrap=img_mgmt_erase_slot`), so an erase it refuses (the active
  slot) keeps the clips. At a setup's commit (an ownership change, never a
  token refresh) an image staged for the next boot (test or permanent;
  staged before this owner, when anyone could upload; a completed upload
  included) is dropped on every board, the clip store reclaimed on the XIAO
  or the secondary slot erased elsewhere, so it never installs, and a
  transfer or press admission from before it carries nothing over (its next
  chunk or state write needs the new owner's proof, or on a release build a
  new press; `mg_ota_setup_committed()`, smp_link's `Owner`, host
  `test_commit_hook`). A
  clip a reset interrupted is recovered as whole frames only.
- **OTA keeps it** while an upload runs, an upgrade is pending (test,
  permanent or revert), or the running image is on test (MCUboot may still
  swap back to the old image now in slot1). Meanwhile the queue reports
  itself off with capacity 0, its other commands answer `busy`, and presses
  without a phone are dropped and logged.
- **The queue takes it back** at boot or once the running image is kept, once
  none of that holds: an old image, an abandoned upload or an image uploaded
  but never marked are discarded by blanking the image header and the
  trailer (RRAM needs no erase; the rest is overwritten by clips).
- **The clips go with their owner** (as on the ESP32): a re-setup's commit
  and every Forget (the token proof's `clear`, the sealed `mg_session_clear`,
  Link setup's `unpair`, the reset hold) erase them, a clip being recorded
  included (`mg_queue_forget()`, `mg_setup.c`). A mark makes it survive a
  power loss: a re-setup's commit record carries it (flag 0x20, so it is
  durable exactly when the new set is: a failed or abandoned re-setup keeps
  the clips), Forget stores `mg/setup/clips` before it erases the setup (a
  mark that can't be stored keeps the setup: `error_storage`). The index
  (the first header) is blanked at once, before `auth_ok` or the Forget's
  answer, and then a thread of its own (lowest priority, 1 KB stack) erases
  the whole store below the slot's trailer, 1 KB (or a flash page) at a
  time under the queue's lock, skipping what is already blank, so the app
  work queue and the watchdog never wait on it; the mark is cleared only
  when every byte is blank. Until then the queue is closed (status off,
  zero clips and bytes; its other commands `busy`, "clips being erased";
  presses without a phone not recorded). A power loss or a failed write
  keeps the mark and boot starts the erase over (`mg_setup_clips_resume()`,
  after the update's slot check; not in SAFE mode); a clip store that
  didn't mount keeps it too, never taken for a build without one
  (`mg_clips_owed_step()`, host `test_clips_owed`). If a firmware update
  holds the slot at the owner change, or takes it during the erase, the
  mark stays and the erase runs when the queue takes the slot back (at
  boot or once the new image is kept). A new clip's header is written
  second half (partial) first, so a reset mid-header never rebuilds a
  length from older bytes (unit `test_owner_change_erases_clips`: every
  byte blank after the erase, including clips a same-owner `clear` left
  behind, and an erase interrupted by a reset). A first setup keeps clips recorded
  before it.

### Gadget behavior: clock, battery, LED, audio, recovery

What the gadget does about its clock, battery, LED, push-to-talk audio and
recovery:

- **Clock** (`src/mg_clock.c`): device_action `set_unix_time` 18 and
  `get_unix_time` 19 -> `report_unix_time` 20 (mgcommands.h, Clock), listed
  in the device_action sub-feature. The time is kept against the uptime
  counter: it lasts across disconnects, not a reset (the XIAO keeps no RTC
  through one), so after a reset the gadget reports INT64_MIN until the next
  set. The UTC offset (param 2, int16 minutes) and the flags (param 3, bit0
  24-hour) are kept when a set omits them; before any set, UTC and 12-hour.
  They are persisted (settings key `mg/clk`) only on a token-proven
  connection or a gadget that isn't set up, and only when they differ from
  what is stored; otherwise they apply in RAM until the next reset, and the
  set still succeeds, except that what a connection set before its proof
  is saved when that proof matches on the same connection (a phone may set
  the clock first; `mg_clock_proof_matched()` from the proof's match hook),
  and another connection's, or a disconnected one's, drops back to
  `mg/clk`'s values; the time itself is taken from any central (host
  `test_clock`, which carries the vectors' clock bytes). On a gadget that
  isn't set up they wait in RAM for the settings' 10-min window, setup's
  commit or a planned power off (`mg_settings_clock_changed()`,
  `mg_clock_flush()`; Settings a stranger can change, below). The gadget has no display, so
  nothing shows the time yet; it is there for the app and for clip
  timestamps later.
  A set before 2024-01-01T00:00:00Z (`MG_CLOCK_FLOOR_MS`) is refused with
  invalid_value and the clock stays as it was (host `test_clock`).
- **Only the owner's phone reads the owner's content:** on a release build
  (no automation, no bench mode) of a set-up gadget, reads that return the
  owner's content answer only a token-proven connection
  (`protocols/test-vectors/mg-session-v1.json` settings, "the owner's
  content without the proof, release build"; unit
  `test_owner_reads_release`). This app's list is `src/mg_owner_reads.h`
  (MG_OWNER_READS: get_unix_time `proof_required`; get_settings'
  client_name / client_app skipped from the answer, then `proof_required`
  once; the log mirror silent), each site gated by `MG_OWNER_READ("name")`;
  `tools/check_owner_reads.py` (a build target, and host run.py) fails a
  listed read without a gated site or a gated site off the list. Identity
  and status reads stay open (request_status, the other get_settings
  fields, battery, the Nordic UART's `status`; its `queue` keeps the set-up
  gadget's rule); test, bench and automation builds keep their own rules.
- **A board's own update prompt (`mg_board_press_take()`, weak, false):** a
  talk-button press goes there first (in SAFE too); true means the board's
  prompt took it (a board whose update goes through its own loader), never
  push-to-talk.
- **Feature lists are the build's:** no supported_features entry depends on
  a boot's allocation or init: audio_queue is listed whenever the build has
  the clip store and answers busy while it isn't mounted (counted, logged);
  the session is listed and answers busy at a hello without memory (unit
  `test_features_stable`: the first read after a connect and a later one
  are identical).
- **Proof gates on a set-up gadget from an unproven central:** the gates on
  ui_input hid_key, a legacy skill invoke and haptic_play have nothing to
  gate here: this app implements none of them, so each answers unsupported
  before any proof check (unit `test_privileged_need_proof`). On a set-up
  gadget's release build the re-setup prompt shows at most once per 10 s per
  connection; a pairing_client_hello sooner gets a plaintext
  error_operation_in_progress and leaves the setup under way alone; a new
  connection may show it at once; test and bench builds are exempt
  (`src/mg_resetup_policy.h`, host `test_resetup_rate`,
  `test_resetup_rate_flow`). In every build a client fragment past the
  credit granted (the window less what isn't given back) drops its message
  with invalid_value and its bytes given back, counted, never buffered; a
  message larger than the window gets its bytes back each time they fill it
  (the vectors' "credit overrun at MTU 517"). Credit [0] from the client is
  taken; `mg_vm_session_nudge()` is the device's keepalive (the credit owed
  for messages fully taken, or credit [0]), for a board whose screen waits on the session (no caller on
  the XIAO). Notifications go up to an ATT value (512 B) where the link's
  MTU allows; the XIAO's MTU keeps them at 244.
- **Silent talk turns (`CONFIG_MG_SILENT_TURN_DROP`, test and automation
  builds only):** a talk turn's output, its gestures included, is held
  (`MG_AUDIO_GATE_VOICE`) until the capture rises above room noise
  (`src/mg_voice.h`: three 20 ms windows in a row 12 dB over the quietest
  window so far and over -50 dBFS), then goes in order; a turn that never
  does is dropped unsent and shows the no-result cue (4 blinks), no thinking
  face, counted in the Nordic UART status `silent=` and the bench `s`, so a
  scripted hold never sends a silent or room-only turn. Release builds send
  every hold. The simulations' synthetic source starts with 40 ms of room so
  it is heard (host `test_voice`, unit `test_silent_turn_no_thinking`,
  BabbleSim ptt).
- **Settings a stranger can change**: on a gadget that isn't set up any
  central may change the stored settings (codec, capture format, the clip
  queue), so they stay in RAM and reach flash at most once per 10 min, the
  window starting at boot (a restart, open to anyone before setup, buys no
  immediate write; `src/mg_settings_policy.h`, host `test_gadget`), at
  setup's commit (`mg_setup_store_committed()`), or before a planned power
  off (`mg_settings_flush()`); the clock's offset and flags likewise (above).
  On a set-up gadget they're saved at once (their change needs the proof).
  Nothing is written per connection or link drop (unit `flash_writes`: 50
  connect, subscribe, request_status and per-connection settings cycles, 0
  writes; 40 stranger changes in about 7 min, 1 write).
- **Battery and power off** (`src/mg_power.c`, `src/mg_poweroff.c`):
  device_action `request_battery_status` 8 -> `report_battery_status` 9
  (mgcommands.h layout, 18 bytes): the last VBAT reading (polled every 60 s)
  in mV and the charge from the LiPo curve, with the flag `present`, when a
  cell is there (2.5 V or more); current, temperature and capacities are
  not measured on this board and read 0, and so does charging (the board's
  charger status isn't on a pin the app reads); without a cell everything
  but the timestamp (device ms of the reading) is 0. Listed on every
  build (`CONFIG_MG_BATTERY_STATUS`; the apps
  prefer it over BAS); a board that doesn't read VBAT answers present 0.
  It also goes unsolicited when a cell appears or goes, after two
  readings in the new state (the charger's status can't be read; USB
  without a cell can flip between 4.2 V and nothing), every 5 % of charge
  since the last report (counted from it, so a reading wobbling on a
  boundary sends nothing), and when 15 % or 10 % is crossed or cleared. `power_off` 10
  (`CONFIG_MG_POWER_OFF`, nRF54L): no reply; the gadget stops capture,
  darkens the LED, drops the link, waits for the button's release, arms
  it as a level wake (GPIO SENSE) and enters System OFF (`sys_poweroff`);
  a press wakes it and it boots as after a reset (MCUboot first). On USB
  the board keeps charging a cell while off. Unknown device actions still
  answer `[error, device_action, unsupported]`.
- **LED language**: the single-color column, under The LED in
  the board notes above and in the README. Setup done (after `auth_ok`),
  clip kept / not kept (after an offline press) are one-shot cues
  (`mg_led_event()`); a press with no app and no clip queue opens no mic and
  shows 4 blinks. No `led_brightness`: the LED isn't dimmable.
- **Low battery** (`src/mg_power.c`, applied in `main.c`), only with
  a cell present (2.5 V or more): at 15 % a warning is logged
  (`battery low`); at 10 % the LED's low-battery flash starts (one 80 ms
  flash every 30 s) and VBAT is read every 10 s instead of 60 s; both clear
  at 20 % (charging). Two readings in a row at or under
  `CONFIG_MG_BATTERY_SHUTDOWN_MV` (3.3 V: a LiPo at 0 % under this light
  load, above its protection cut-off and the 3.3 V regulator's dropout)
  power the gadget off as `power_off` does (`battery empty`), before a
  brownout; the button wakes it (and, still empty, it powers off again
  after two readings). Never on USB without a cell: that reads under
  2.5 V, or the charger's 4.2 V (100 %).
- **Hold contract** (`mg_core.c` `hold_fn`, `mg_audio.c`
  `mg_audio_start_held` / `commit` / `discard`): the press picks its route
  (live to a ready client, the clip queue, or none: 4 blinks, mic
  off). Capture starts at key-down (pre-roll: encoded frames held back in a
  ring of at most 1 s, `BACKLOG_BYTES`; the LED's capture light on at once);
  after `CONFIG_MG_HOLD_MS` (250 ms) held: `[gesture, button_down]`, then
  `change_data_type`, the pre-roll as a burst, then live audio (offline: the
  clip opens with the pre-roll, its start time moved back to the key-down).
  A tap (released sooner) sends nothing at all, no gesture, no audio, no
  stop_mic, keeps no clip and shows no extra cue (the light was on only
  while held: the single-color LED has no "Hold to talk" text, and an error
  blink would read as a failed turn). With push-to-talk off the gestures
  follow the same threshold. The buzz (boards with a motor) comes at the
  threshold. A release just after the threshold still sends what was
  captured (the audio thread commits before it stops). After a live turn's
  button_up the capture runs on `CONFIG_MG_PTT_POSTROLL_MS` (200 ms:
  people let go on the last syllable), then the last frame and
  stop_mic, the capture light on until it stops; a turn the device ends
  (the client's stop_mic, a disconnect, not ready) stops at once, and a new
  press ends a post-roll at once. A client's start_mic during a held turn
  takes it over only once its capture starts: a refused or failed one leaves
  the press's turn, so the release still stops the mic; push-to-talk turned
  off mid-hold stops the capture at once. The button is not
  the reset button, so no guard is needed. A board whose talk button can't
  be held (a reset supervisor on the same line) sets `CONFIG_MG_TAP_TO_TALK`
  (off by default): a press starts the turn as a held press does, its
  release changes nothing, the next press ends it (and does nothing else),
  and a turn ends by itself after `CONFIG_MG_TAP_TO_TALK_MAX_MS` (30 s) or
  with the client; Link setup's presses and holds stay as they are (unit
  suite `tap`, its own `run_all.sh` configuration).
- **Codec (LC3 preferred)**: LC3 is in the `start_mic` codec list
  and in setting 7's (`audio_codec`) values next to SBC. A push-to-talk turn
  captures in setting 7's codec, never in the codec of the client's last
  `start_mic` (a start_mic's format is for that capture only); setting 7 is
  remembered across connections and restarts (SBC on a fresh device or
  after a factory reset);
  a stored codec this build doesn't list is skipped. The XIAO has
  no speaker, so no `stream_audio` codecs and no buffer_update, and no
  inbound stream for a lapsed proof to end (mgcommands.h: a board that adds
  playback or video ends an open stream with its stop_streaming when the
  connection's proof ends with the link up) (a board that adds playback counts "total received" as every byte received since
  start_streaming, dropped writes included: mgcommands.h).
- **Capture formats**: 16 kHz and
  32 kb/s stay the default; a client gets anything else only when it asks.
  `request_status` adds a typed list under change_data_type, per codec its
  rates (16000 first) with a kb/s range each: SBC 16000 at 60 fixed, LC3
  16000 at 16 to `CONFIG_MG_LC3_MAX_KBPS` (64), and 24000 the same with
  `CONFIG_MG_LC3_24K`. `start_mic` params 2 and 3 (rate uint16 Hz, bitrate
  uint16 kb/s) capture at that rate where the codec lists it, else 16000,
  at the bitrate clamped into the range; never refused. Settings 13
  `capture_sample_rate` and 14 `capture_bitrate_kbps` (uint16, remembered
  like setting 7; 16000 and 32 on a fresh device; a stored value this build
  doesn't list, e.g. 24000 with 24 kHz off, is skipped) do the same for
  push-to-talk turns (an SBC turn stays 16 kHz at its own bitrate); a value
  outside every listed rate or range is `invalid_value`; 13 is listed in the
  typed settings list only with `CONFIG_MG_LC3_24K` (a 16 kHz-only build
  still accepts 16000), 14 with LC3. LC3 frame bytes = floor(kb/s * 10000 /
  8000) (16 kb/s 20 B, 32 kb/s 40 B, 64 kb/s 80 B), LTPF off. Every
  capture's change_data_type carries its rate and frame bytes. With
  `CONFIG_MG_LC3_24K` the PDM runs at 32 kHz (exact; 24 kHz has none on
  the nRF54L15) through `src/mg_resample.c` (4:3 polyphase,
  `tools/gen_resampler.py`; host test `test_resample`), the same DMIC slab
  (two 5 ms blocks per 10 ms). `CONFIG_MG_LC3_24K` is on since the board
  check (XIAO): encode 1.12-1.27 ms per 10 ms frame at 16 to
  64 kb/s (at most 1.6 ms; 0.95 ms at 16 kHz), a played 1 kHz tone heard at
  24 kHz at the level it has at 16 kHz (`mg_ble_client.py test`),
  and tones of 13 to 16 kHz played into the mic leaving nothing at their
  aliases (11 to 8 kHz) above the room's noise, while in-band tones of 6 to
  11 kHz from the same speaker come through 20-30 dB above it (the host
  test measures the resampler itself at 74 dB or better).
- **Offline clips**: the gadget picks the clip
  format, never settings 7, 13 or 14: SBC at 16 kHz, or LC3 at 16 kHz and
  16 kb/s with `CONFIG_MG_CLIP_LC3_16KBPS` (off by default; turn it on when
  the apps you pair with decode clips by their clip_info). The clip header keeps the format (codec, rate,
  channels, frame bytes, duration) and `clip_info` carries it as trailing
  fields; a clip stored before reads as its codec's default. Capacity in
  the XIAO's 700 KB store (712 KB slot1 less the 12 KB trailer; 32 B header
  per clip, 16 B alignment): about 95 s in SBC (7.5 KB/s), 179 s in LC3 at
  32 kb/s and 358 s (6 min) at 16 kb/s, less 48 B per clip. The BabbleSim
  `offline` scenario runs on both clip formats (`gadget-lc3clips`).
- **Recovery** (`src/mg_boot.c`, portable; `src/mg_health.c`):
  - The tiers stay protected and up before app code: MCUboot (revert of an
    unconfirmed image), SMP over BLE (registered at build time, served as
    soon as BLE is up, in every mode; on a release build an update write
    without the token proof waits for one confirm press on the gadget, set
    up or not, a SAFE the gadget entered itself refuses it, and SAFE asked
    with the button takes them at once) and SWD
    through the on-board probe
    (the XIAO's "USB" tier; it has no 1200-baud bootloader). Nothing here
    touches MCUboot or gates SMP; the only change is when an image is
    kept: at healthy once a link proved ATT works (a client's confirm
    included), not at the first advertisement.
  - A boot record in retained RAM (survives resets, not a power-on or a wake
    from System OFF; magic + CRC) counts boots in a row that never got
    healthy. It must sit where no bootloader clears it on the way (a
    compressed-image MCUboot, for one, zeroes its large LZMA dictionary in RAM
    at every boot, where the app's .noinit can land): a board reserves the
    devicetree block `mg_retained` (`zephyr,memory-region`, NOLOAD) at the top
    of SRAM outside `zephyr,sram` and sets `CONFIG_MG_BOOT_RAM_END` at or
    above its shipped MCUboot's `_image_ram_end`; the build fails if the
    block is below it, inside the app's RAM, or too small, or if a board
    sets the limit without the block, and a sysbuild fails if the MCUboot
    it built uses RAM past the limit (`tools/check_boot_ram.py`, from
    MCUboot's map). The XIAO's is the top 256 B (0x2003ff00, its overlay;
    its MCUboot ends near 0x200043e0, the limit 0x20008000). With the nRF
    Connect SDK's partition manager the app takes its RAM from pm_static's
    `sram_primary`, so shrink that too (checked when `USE_PARTITION_MANAGER`).
    `mg_health_init()` runs first in `main()`. Healthy = BLE up and
    advertising for `CONFIG_MG_HEALTHY_AFTER_MS` (30 s) with the watchdog's
    threads alive AND the board ready (advertising and uptime alone would
    confirm an image whose board init had blocked): every part the boot
    needs is up (`MG_READY_*`: settings and the Muse layer, the mic source's
    probe (`mg_audio_src_probe()`, the PDM's device ready), the clip store
    mounted where the build has one, and `mg_board_ready()`, a weak hook a
    board overrides for its own mic or codec; SAFE needs the core only).
    Until then nothing clears the count, so a test image stays on test and
    MCUboot reverts it, and three such boots reach SAFE
    (`CONFIG_MG_BENCH_BOARD_NOT_READY` stands in for a blocked board init on
    the bench; host `test_board_ready`). It clears the count and keeps a test
    image (Firmware updates, above). The watchdog runs while the CPU sleeps
    (no `WDT_OPT_PAUSE_IN_SLEEP`: the nRF's CONFIG.SLEEP = Run), and a
    debugger's halt pauses it only in test and bench builds; it's fed only
    from the supervisor's progress check (each guarded thread's beat).
  - **Reachable or failed (`src/mg_reach_policy.h`,
    `CONFIG_MG_REACH_RESET`, off in simulations):** from the power gate on,
    a boot with no connectable advertising and no link within 30 s restarts
    as a failed boot, and once reachable, 30 s with neither restarts too (an
    update needs a link, so one in progress is never a loss; power off on
    its way to System OFF is exempt; a link before any advertising counts).
    **Losses no peer could cause escalate; losses a peer could cause only
    restart** (a central that connects and never answers ATT must not park
    a set-up gadget in SAFE). A stretch with no link at all (not
    advertising, nothing connectable) is the gadget's own fault: it counts
    across boots (the record's `reach_resets`, carried into the unhealthy
    count, so three in a row reach SAFE), cleared after 10 min reachable,
    and a SAFE that reach losses alone led to (`MG_BOOT_F_SAFE_REACH`;
    never reachable at boot counts too) restarts itself into normal mode
    after 10 min reachable, the check still watching past SAFE's own
    restart bound (SAFE asked with the button doesn't). A stretch with a link up in it, three ATT-dead links, or a
    stuck TX path is something a peer could cause (`mg_health_peer_loss()`):
    a clean soft restart that sheds the peer, breadcrumb `peer: ...` (reason
    0x102), with the board told first (`mg_board_peer_restart()`: a loader
    that counts boot attempts skips it); the breadcrumb is kept as a
    crash's is (Breadcrumbs, below). A peer restart counts toward nothing:
    not the unhealthy count (that boot's own count is taken back), and never
    MCUboot's revert: while the running image is on test, and in SAFE mode
    of either kind (a restart buys nothing there, and out of the button's
    SAFE it would put the broken image back), a peer loss never restarts,
    the link goes instead.
    After three such restarts since the last token-proven session (the
    record's flags), a peer loss drops the link instead (the gadget keeps
    advertising, the owner's proven update path stays open), except once
    per boot after 10 min up; only a token proof, the user's button at
    power-up or a new image resets that bound, never a plain peer exchange,
    a forced restart or a healthy boot (host `test_peer_losses`). Inside
    SAFE, after three unreachable restarts in a row (`safe_reach`, cleared
    only by 10 min reachable: the healthy timer can fire during an
    unreachable stretch) it stays up and logs, the button route and SWD
    still working. It reads plain flags the BLE callbacks set
    (`mg_transport_reachable()`, `mg_transport_link_up()`), sampled each
    second on the system work queue, plus the connection edge `connected()`
    latches (`mg_transport_link_seen_take()`), so a link that comes and goes
    between two samples still marks the stretch as a peer's; host
    `test_reach`, `test_reach_safe_trial`. Every change to the
    record takes one spinlock (the watchdog's callback included; the fatal
    handler alone doesn't, never returning, and starts from the last
    committed copy when it finds the record half-sealed), every writer
    refuses a record
    whose CRC fails (`mg_boot_valid()`), and each boot is counted at
    PRE_KERNEL_2, before the early watchdog can fire, so a hang before
    `main()` counts too (SAFE itself starts in `main()`: MCUboot's revert
    covers an unconfirmed image; a confirmed one that hangs there needs
    SWD). Its bench check needs no unreachable image: in a bench build
    (`CONFIG_MG_AUTOMATION`) the Nordic UART word `reach test` (the USB
    console's `>reach.test`) turns connectable advertising off until the
    next boot and drops the link (RAM only), so the check restarts the
    gadget 30 s later; three in a row reach SAFE. Release builds refuse it
    (`disabled`), and a build without the restart answers `unavailable`.
    **A link counts only once ATT works** (a link can form with ATT dead both
    ways): `mg_transport_reachable()` is connectable advertising, or a link on
    which an ATT exchange completed through the gadget's own path with on-air
    evidence (`mg_reach_att_counts()`): one of our own requests answered (the
    MTU exchange, or the central's name read when the central exchanged MTU
    first; the peer's ATT error is an answer, the host's UNLIKELY for a
    timeout or a link's end isn't), or a peer's second Write Request on the
    link (ATT allows one at a time, so it proves our answer to the first
    arrived; the first proves nothing). A completed notification doesn't
    count: the host's completion is the controller's Number of Completed
    Packets, which follows the peer's link-layer acknowledgement on both the
    open controller and the SoftDevice Controller in normal running, but the
    Core spec lets a controller report flushed packets the same way. The host
    answering a peer's MTU request by itself never counts (a link's MTU can
    reach 247 while its next write never completes), and an idle link that
    got there is no loss. A link without one is unreachable time, and three
    links in a row that end without one while the gadget's own requests
    failed (or went unanswered 5 s) are a peer restart
    (`mg_reach_link_ended()`, breadcrumb `peer: ATT dead on 3 links`); a
    link that ends after an exchange starts the run over, and a short one
    with nothing failed never counts (host `test_reach_att`). A link that
    stays up while the gadget's own sends fail is ended by the TX check
    (below; at most 30 s, 10 s when the peer speaks) as a peer restart. The
    run lives in the boot record (`att_dead`); a completed exchange or a
    peer restart clears it. The host's UNLIKELY on our request counts as a
    failure only while the link is still usable (ATT's 30 s timeout); at a
    link's end ATT cancels what's outstanding the same way, which leaves it
    pending (the 5 s rule decides). Idle links (nothing owed) never count.
    Bench check (bench builds only; release builds answer `disabled`): the
    USB console's `>attdead.test` and the Nordic UART's `att dead test` make
    every link until the next boot count as ATT-dead and drops it 6 s after
    it forms (RAM only; it stands in at the signals the check reads, the
    host stack's ATT still runs), so the third link restarts the gadget.
    **Only a link past the gate moves this state**: on a set-up gadget the
    ATT evidence above counts (clears the run, counts as reachable, marks an
    updated image's link proven) only once this connection's token proof
    matched (`mg_reach_exchange_counts()`, `mg_transport_gate_passed()` from
    the match hook); a stranger's link, its requests refused, moves none of
    it and, its ATT working, doesn't count as dead either. A gadget that
    isn't set up has no gate. An unproven link still counts as reachable
    while the gadget's own update path is live on it (an update chunk it
    took within 10 s, or a press admitting one, within the press's 600 s:
    `mg_upd_admitted_live()`), never for the update prompt alone: any central in range raises that, and raising it again
    each time it ends would hold the single link and lock the owner's phone
    out (`mg_reach_update_live()`, host `test_bench_switch`). The peer
    restart bound is cleared only by a proof (`mg_token_proof_on_match()`);
    the healthy mark needs no peer (advertising with the board ready,
    below). Host `test_reach_att`.
  - After `CONFIG_MG_SAFE_MODE_BOOTS` (3) unhealthy boots the next runs in
    SAFE mode: BLE, DIS/BAS/NUS, SMP, and the mg service answering
    request_status with the recovery declaration (mgcommands.h, Recovery
    mode: `[supported_features, sub_feature, request_status, recovery,
    reason, 0]`, reason `user` for the boot hold's SAFE and `boot_loop` for
    one it entered by itself; no reboot action, so no restart flag), then
    `[supported_features, request_status]`, and every other command
    `recovery` ("safe mode"; unit `test_safe_mode`). It advertises as
    `SAFE-XN54-1A2B3C` with no mg or Link setup UUID, a distinct identity
    so an app that doesn't read the declaration never takes it for the
    gadget (as the ESP32's SAFE; the
    bench client finds it by name for SMP recovery); no audio thread, clip
    queue, push to talk, Link setup or bench console. A healthy SAFE boot clears the count:
    the next reset (an SMP reset after an update, or the button) runs the
    normal image again. An unconfirmed image that crashes never gets this
    far: MCUboot swaps the old one back at the first reset.
  - **SAFE frees the BLE tier from a silent holder** (`src/mg_safe_policy.h`, host
    `test_safe_idle`): the XIAO has
    one connection slot (`CONFIG_BT_MAX_CONN=1`), so in SAFE (either kind) a
    central that holds it and writes nothing for 60 s (no mg, Nordic UART or
    SMP write; the link's start counts) is dropped and the gadget advertises
    again (`safe: idle link dropped`), never while SMP is handling a request
    or holds an upload's writer. A central that keeps writing keeps the slot:
    the button's SAFE and SWD stay the ways back from one.
  - The watchdog (`CONFIG_MG_WATCHDOG`, WDT31, 10 s, paused only when the
    debugger halts the CPU) starts at the first moments of a boot
    (`SYS_INIT` PRE_KERNEL_2, right after the drivers), so a hang anywhere
    before `main()` resets the gadget too (breadcrumb `watchdog: hung before
    the supervisor`; host `test_boot_record`'s early-watchdog row; bench
    check, bench builds only: `CONFIG_MG_BENCH_EARLY_HANG`, a test image
    that hangs there, reset in 10 s and swapped back by MCUboot). It is
    then fed once a second by a supervisor on the system work queue, only while the app work queue answers a ping within
    8 s and the audio and Bluetooth TX threads, while working, beat within
    8 s (`mg_health_busy()` / `mg_health_beat()`). A stuck one leaves the
    breadcrumb `watchdog: <thread> stuck` and the supervisor stops feeding;
    a stuck system work queue is caught by the WDT's own callback. Power
    off stops the watchdog first. Nothing a central does starves it: the
    Bluetooth TX thread never hands the stack more notifications than it
    has slots (10, two fewer than the ATT pool, so a notify call never
    waits on a buffer) and never waits more than 500 ms at a time; on a
    link whose notifications stop completing for 2 s it drops what's queued
    (counted) until one completes, keeping the link; the throughput test's
    send ends with its report when the link takes nothing for 2 s; and a TX
    path a central holds dead is a peer restart (below), not a watchdog
    reset.
  - **The TX path is progress too (`src/mg_tx_progress.h`, host
    `test_tx_progress`):** with a link up, something owed and nothing
    completed, the supervisor restarts the gadget cleanly as a peer loss
    (`peer: TX path stuck`, a soft reset, never counted toward SAFE; past
    the restart bound it drops the link instead; any other stuck thread
    still goes through the watchdog). Two host-side legs, the same on the
    open controller and the SoftDevice Controller: our notifications in
    flight or waiting for an ATT buffer against their sent callbacks, and
    the ACL packets the controller holds (`bt_conn_get_pkts()`, sampled
    every 100 ms) against the packets it reports completed, counted at the
    host's Number of Completed Packets (`bt_conn_tx_notify(conn, false)`,
    wrapped at link time), so traffic outside mg's queue (the Nordic UART,
    ATT responses, SMP) counts too; that sees a write's response stuck with
    no notification after it (a CCC subscription). Both legs start over at
    every connect and disconnect (a walk-away's notifications in flight
    never get their sent callbacks). A sent callback, an ATT Write Request
    or an SMP request is progress (a peer sends more only once answered);
    completions are counted, not queue liveness, so a slow system work
    queue can't hide a dead path and a slow-but-moving one never trips. The
    clock is 10 s when the peer spoke during the stall, else the link's
    supervision timeout + 2 s (at most 30 s), so a phone walking away ends
    in the controller's clean disconnect, not a restart. Bench check (bench
    builds only; release builds answer `disabled`): the USB console's
    `>txdead.test` and the Nordic UART's `tx dead test` take and hold the
    link's free controller credits (RAM only), so the gadget restarts as a
    peer loss about 10 s later (refused without the watchdog, on test, in
    SAFE or past the rate bound, and while an earlier one still holds them:
    that restart gives them back, or the link's end if it comes first; the
    take and the give-back hold the connection lock).
- **Mic output only to a proven central:** a capture's output (its change_data_type, Data, the
  press's gestures and stop_mic) goes only to a central that passed this
  connection's token proof, or with the bench switch on (`mg_core_output_ok()`).
  Otherwise it's held whole, in order, in a 24 KB hold taken from the heap at
  the capture's start (`src/mg_out_hold.h`, host `test_out_hold`;
  `mg_audio.c`'s output gate), and goes right after the proof's result once it
  matches. Output that is going when that stops being true (the match reset by
  a new challenge, a setup commit or erase; the bench switch off) stops: a
  live capture or clip download that isn't held ends at once, and nothing
  encoded meanwhile goes (`mg_core_output_revoked()`, unit
  `test_privileged_need_proof`). A failed proof discards it unsent; held output not released within
  3 s of the capture's end is discarded (logged, counted); a hold that fills
  stops the capture and keeps what it holds (64 bytes stay free for the turn's
  button_up and stop_mic). Nothing about a held capture reaches an unproven
  central: request_status reports the data type from before it (the capture's
  change_data_type counts only once released), the Nordic UART's `status`
  shows `audio 0`, and the capture doesn't move the link (no "mic" activity).
  - **A talk press in the connect burst** (the client's link is up but it
    isn't proven, or not ready: subscribed, request_status, push-to-talk on)
    is a held live turn: SBC 16 kHz (the clip format,
    announced by its own change_data_type at go-live whatever audio_codec
    says), its button_down, audio and button_up held, live with everything
    held once the client is proven and ready; not within 3 s of the press
    (`MG_AUDIO_HELD_TURN_MS`), or the link drops, the held audio opens an
    offline clip (back-dated to the press) and the turn goes on into it (or
    is saved whole, for a turn already over); with no queue it stays held
    under the 3 s rule. A failed proof discards it. Nothing new starts while
    a hold waits.
  - **A client's start_mic before the proof** opens the mic only while a
    token-proof challenge is out on this connection (`mg_token_proof_pending()`;
    its output held); otherwise `proof_required` (counted).
  - **The press gate** (`src/mg_mic_policy.h`): on a release build
    (`CONFIG_MG_AUTOMATION=n`) of a set-up gadget a client's start_mic also
    needs a real press of the button (or a wake word a board's detector
    heard, `mg_core_wake_word()`) within the last 5 s; console words,
    protocol commands (mg_session_wake) and a charger never count;
    otherwise `press_required` (9): no capture, no mic ring. The proof is
    checked first, then the press, as the vectors order them. Within the
    window a start_mic after a stop_mic is taken again. Automation builds
    need the proof alone. No console word captures on this app. Only the
    button's own GPIO press stamps the press, outside bench mode (unit
    `test_privileged_need_proof`: an injected press, then start_mic, is
    `press_required`).
  - `read_clip`
    before the proof (or the bench switch): `proof_required`, before the
    clip is looked up. On a set-up gadget the clips' metadata is the
    owner's too: until this connection's proof matched (the bench switch
    doesn't count) `clip_info` answers `proof_required` and the queue's
    status (and the Nordic UART's `queue`) report zero clips and zero bytes
    used (unit `test_privileged_need_proof`), and the Nordic UART's log
    mirror leaves out the clip queue's lines (`mg_queue`: clip counts and
    sizes); the USB console keeps them. A gadget not set up has no proven
    central: a press with a client connected goes nowhere (4 blinks), with no
    link it goes to the clip queue as before.
  - **The bench switch** (automation): the USB console's `>bench=1|0` and
    the Nordic UART's `bench 1|0` (over Bluetooth too) let
    this connection get mic output without a proof; per connection, off at
    every connect, disconnect and boot, logged (`mg.ble: bench switch on`),
    and the LED's bench mark while on (two 30 ms flashes 100 ms apart every
    4 s, lowest priority). A held capture goes at once.
    `mg_ble_client.py --bench-word "bench 1"` sends it right after connect.
    Over Bluetooth (the Nordic UART's `bench 1`) it acts only with this
    connection's token proof, in every build but a bench-mode image, never on
    a press (`mg_bench_ble_allowed()`; answered `bench refused (needs the
    token proof)`). The wired USB console is the owner's, never locked, and
    simulations are exempt. A release build (`CONFIG_MG_AUTOMATION=n`)
    refuses it over Bluetooth on any set-up gadget, proven or not, and an
    automation build takes it there only on a token-proven connection
    (`bench refused (a set-up gadget in a release build)`,
    `mg_bench_switch_allowed()`, `mg_core_ble_bench_locked()`; host
    `test_bench_switch`).
    The Nordic UART's log mirror (every INF line: talk presses, captures,
    setup and link events, with their times) goes to an unproven central
    only on a gadget that isn't set up or in a bench-mode image; a set-up
    gadget mirrors only to its token-proven central, automation builds
    included (`mg_nus_log_allowed()`, host `test_bench_switch`). Word replies
    keep their own rules.
    In a release build (`CONFIG_MG_AUTOMATION=n`) the Nordic UART's state-changing words (`bench`, `listen`,
    `mode`) run only on a token-proven connection and answer `locked`
    otherwise; reads stay open, and automation builds (`overlay-bench.conf`)
    and the USB console keep everything open (`src/mg_nus_policy.h`, host
    `test_nus_policy`). The lock is default-deny: only the read words
    (`status`, `version`, `settings`, `queue`, `battery`, `pins`, `reset`) run
    unproven in a release build, an unknown line is `locked` too, and the gate
    runs before any word is compared; every word `run_cmd` dispatches is in
    `mg_nus_words[]` with its class, and `tools/check_nus_words.py` (every
    build and the host run) fails on one that isn't, on a prefix match, or on
    a comparison before the gate.
  - **Checked in the image:** after every device link
    `tools/check_mic_gate.py` (the `mg_mic_gate_check` target) fails the
    build unless `mg_core_output_ok()` and start_mic's `proof_required`
    refusal are linked; in a release build (`CONFIG_MG_AUTOMATION=n`) also
    the wireless bench switch's release lock (`mg_core_ble_bench_locked()`,
    found by its symbol), and no memory reader over
    a wireless link (MCUmgr's shell, file system, settings, statistics or
    log groups, a shell on the Nordic UART, a Bluetooth log backend) and no
    bench switch press path (`mg_core_press_window_open()`). The USB bench
    console is wired and stays.
  - **Counters:** the Nordic UART's `status` adds `output: unproven|proven|bench
    held= releases= discards= stale= full= turns= queued= start_refused=`.
  - Host `test_out_hold`; unit `test_unproven_central_output_held`; BabbleSim's
    offline scenario reads its clips on the bench switch.
- **Breadcrumbs**: the last reset's reason (hwinfo: power-on, reset
  pin, software, watchdog, CPU lockup, wake from power off, ...) and the
  crash breadcrumb (a fatal error's reason, PC, thread and uptime from the
  replacement fatal error handler, which restarts at once,
  `CONFIG_MG_FATAL_REBOOT`; the watchdog's; or a peer restart's) are logged
  at boot (`boot N since power-on; reset: ...`, `last boot crashed: ...`), kept in
  settings (`mg/crash`, so later boots still say `earlier crash: ...`), and
  answered by the Nordic UART console's `reset` command and the bench `s`
  key: `reset: watchdog; unhealthy boots before: 1; mode: normal, healthy;
  crash: watchdog: audio thread stuck, up 73 s`.

### Bench console (automation tools)

Every build has the bench console (`CONFIG_MG_BENCH`): single-key commands
on the console UART (uart20, the XIAO's USB serial port, 115200 baud): `d` /
`u` press and release the talk button through the real button path, `l`
sends a test control message on an open device session (Device session,
relay placeholders), `s` prints link state, push-to-talk, battery mV and %,
image version and whether it's confirmed, and the queue (owner, clips,
bytes), and the lines below. The automation tools are one switch,
`CONFIG_MG_AUTOMATION`, off by default, so release builds (what is
published) have none of them, and `overlay-bench.conf` turns it on for lab
and CI builds: `@pair.pending`, `>pair.confirm`'s press and `d` confirming
a waiting setup, NUS `pins`, and
the Nordic UART's state-changing words without the token proof. A setup
confirm takes any one press, the console's `d` included; the boot's holds
read the GPIO directly. The USB
console's other words are in every build; the recovery tiers (SMP over BLE,
SWD, the button) too. BabbleSim's gadget turns automation on in its own
`tests/bsim/gadget.conf`.

**Bench mode** (`CONFIG_MG_BENCH_MODE`, never shipped, bench units only:
a unit a person wears or carries runs a release build): every human step
scriptable, orthogonal to the automation tools, so a release-configured
image runs its release gates end to end with no hand on the gadget.
`overlay-bench.conf` sets it; `overlay-bench-mode.conf` adds it to any
build (`-DEXTRA_CONF_FILE="overlay-release.conf;overlay-bench-mode.conf"`).
With it on: an injected press (`d` / `u`, the Nordic UART's `talk down` /
`talk up`) counts as the button's for every human gate (the mic's press
window; a setup's confirm takes any press anyway), and an injected press
still held when its link drops is released then, so the turn and the
microphone don't stay open with nothing left to send `talk up` (unit
`test_injected_press_ends_with_link`); the bench switch over
Bluetooth needs neither a proof nor a press (per connection, off at
disconnect; refused in SAFE in every build); Bluetooth update writes need
no press, automation or not (`mg_ota_release_build()`); every Nordic UART word runs
unproven, and the human steps are on it too for boards without a console
UART: `talk down`, `talk up`, `pair confirm`, `safe next`,
`reset next` (compiled only with bench mode; `tools/check_nus_words.py`
refuses one outside `#if defined(CONFIG_MG_BENCH_MODE)`); `safe next` /
`reset next` and the USB console's `>bench.safe` / `>bench.reset` leave a
request in the boot record (`MG_BOOT_F_BENCH_SAFE` / `_RESET`) and restart
warm, and the next boot takes it as the button's boot hold would: SAFE as
the button's, declared with recovery reason bench and open to updates, or
the setup reset (an image without bench mode drops a stale request); the
re-setup prompt's rate doesn't apply; the reach, ATT-dead and TX-dead rows
run. The image id gets its bench bit (`.bench`), the image carries
`mg_bench_mode_image` (logged at boot), and `tools/check_mic_gate.py
--release` refuses it (the build runs it with `--bench-mode`, which keeps
the release checks and allows the bench paths). Host
`test_bench_mode_flows`.

`>update.window` (with `CONFIG_MG_DFU`) arms the connection that is up for
60 s: its update writes need no press (Firmware updates; `@update.window no
link` without one).

`>expire` sets the stored pair's `access_expires_at` in the past (the
tokens kept, one store commit; answered `@expire done`), so the app
refreshes at its next hello: a refused-refresh test after a server-side
revoke. `>keyauth=0|1` changes get_device_info's device_key_auth.

For unattended setup tests the console takes the ESP32 console's line
`>pair.confirm`: in a bench build the button's setup press
(`mg_setup_button()`, on the app work queue), never push-to-talk, answered
`@pair.confirm confirmed` or `@pair.confirm none` when nothing waits, and
`@pair.pending` goes out when a confirmation starts waiting; a release
build answers `@pair.confirm disabled`, as the ESP32 does. `../esp32/tools/mg_confirm.py PORT` answers it
while the Muse app or `mg_ble_client.py setup` drives the other side, or
`mg_ble_client.py setup --confirm-serial PORT` does both (`esp32/AGENTS.md`,
Automated setup tests). `../esp32/tests/test_mg_confirm.py` checks the tool
and this side of it. In a bench build `d` then `u` also confirms (it is
the real button; setting a set-up gadget up again needs `u` 3 s after
`d`), but pushes to talk when nothing waits, so rigs use the line. In a
release build no console path confirms a setup: `d` while one waits is
refused with `@pair.confirm disabled` (and its `u` with it), as the line
is; otherwise `d` / `u` are the talk button as in every build.

### Board hooks

For a board this tree doesn't carry: its own code lives in a directory
beside the app (`zephyr/<dir>/`) with an `app_ext.cmake`, which
`CMakeLists.txt` includes (each one adds its sources only when its board
builds), and a `Kconfig.ext` for its options (`zephyr/Kconfig`'s
`orsource "*/Kconfig.ext"`). Its code then defines the hooks it needs
(`src/mg_app.h`); the weak defaults change nothing, so the XIAO's builds
use none of them:

| Hook | For | Default |
|---|---|---|
| `mg_board_reset_hold_allowed()` | extra conditions for the setup reset hold (only with the charger present, say) | true |
| `mg_board_safe_hold_allowed()` | whether the boot hold has its SAFE point (`CONFIG_MG_SAFE_HOLD`) | `CONFIG_MG_SAFE_HOLD` |
| `mg_board_reset_cause(hw)` | the reset cause when the board knows better than hwinfo (a loader that resets into the app) | hwinfo's |
| `mg_board_healthy()` | told at the healthy mark | nothing |
| `mg_board_wdt_fed()` | each time the app feeds its watchdog: a board whose loader left more watchdog channels or instances running feeds them here, so they starve exactly when the app's own does | nothing |
| `mg_board_peer_restart()` | right before a restart a peer could cause (Recovery: a loader that counts boot attempts skips it) | nothing |
| `mg_board_setup_state(complete)` | told the setup state at boot and at every change | nothing |
| `mg_board_nus_word(cmd, out, len)` | the board's own Nordic UART console words, for words the console doesn't know; all state-changing (`mg_nus_board_word_locked()`: in a release build only on a token-proven connection) | none |
| `mg_board_sdk_token()` with `CONFIG_MG_SDK_TOKEN_BOARD` | the owner's SDK token from the board's loader, under the token slot's rules (at most 127 characters, only its length logged) | none: a board that picks the option defines it |
| `mg_audio_src_start/read/stop()` with `CONFIG_MG_AUDIO_SRC_BOARD` | the board's own microphone front end (`src/mg_audio.h`) | none: likewise |
| `mg_board_smp_write_allowed(group, id)` with `CONFIG_MG_SMP_GATE_BOARD` | the board's say on each SMP write, after the token-proof gate (Firmware updates, above) | none: likewise |
| `mg_board_smp_gate_defers(group, id, op)` with `CONFIG_MG_SMP_GATE_BOARD` | true: the token-proof gate leaves that request to the board without the proof (writes still meet `mg_board_smp_write_allowed()`) | false |

With `CONFIG_MG_TAP_TO_TALK` a talk button that can't be held starts and
ends a turn with presses (Gadget behavior, Hold contract). A board with a
separate network core (an nRF5340, say) names that core's board in
`NET_CORE_BOARD` (its own `Kconfig.sysbuild`) and gets Zephyr's `hci_ipc`
image built for it (`NET_CORE_IMAGE_HCI_IPC`, `sysbuild.cmake`); the
controller settings in `zephyr/Kconfig` are defaults that apply only where
the controller is built into the app, so such a board sets them for its
`hci_ipc` image itself. No test configuration builds this one (it needs
such a board).

How each is exercised: host `test_nus_policy` (the board words' lock),
`test_setup_board` (the board's SDK token and `mg_board_setup_state()`),
`test_smp_gate` (`src/mg_smp_gate.c` with a board gate), `test_smp_req_gen`
(`src/mg_smp_req_gen.c`'s reassembly wraps in `smp_bt`'s call order: a
partial request from a connection that's gone dropped, each request stamped
with its first fragment's connection); the unit suite
`tap`; and the BabbleSim gadget-homelink build (`tests/run_all.sh`), which
turns on `tests/Kconfig.ext`'s `CONFIG_MG_TEST_BOARD_HOOKS` (simulation
only): `tests/app_ext.cmake` adds `tests/board_hooks.c`, which defines the
hooks as a board would and logs each call (the run checks the reset cause
and both setup states), the synthetic source as the board's audio source,
and SMP over Bluetooth behind `CONFIG_MG_SMP_GATE_BOARD`
(`tests/bsim/hooks.conf`). The reset and SAFE holds have the XIAO's own
host rows (`test_reset_hold`). `board_hooks.c` also defines
`mg_board_wdt_fed()` and `mg_board_peer_restart()`, but the simulation
calls neither: it has no watchdog and no reachability restart (both off in
simulations), so only the XIAO's builds link their callers.

## Flash

The XIAO flashes through its on-board SAMD11, a CMSIS-DAP probe that also
carries the console UART; not nrfutil, which wants a J-Link.

The RRAM is written in 16-byte words. OpenOCD's `nrf54l-load` (the board's
own config, used by `west flash`) writes through the memory bus, and on this
board it left a signed image's final partial word (an odd-length signature
tail) unwritten, and an earlier firmware's bytes in the primary slot's
trailer, which MCUboot then misreads. So don't flash the raw hexes; make one
padded hex:

```sh
python3 tools/flash_hex.py ~/zephyrproject/build-xiao -o mg-xiao.hex           # reflash
python3 tools/flash_hex.py ~/zephyrproject/build-xiao -o mg-xiao-first.hex --fresh  # first flash
```

It merges MCUboot and the signed app, pads every segment to whole 16-byte
words with 0xff, and blanks MCUboot's swap state in both slots: the last
16 KB of slot0, and slot1's trailer, so a test image staged in slot1 earlier
can't swap over the app just flashed (only the trailer: with the offline
clips in slot1, the `CONFIG_MG_QUEUE_SLOT1_TRAILER` bytes the queue never
writes, so the clips stay; host run.py `flash_hex`);
`--fresh` also blanks slot1's first and last 16 KB and the settings partition
(drops settings and what Link setup stored), which replaces a chip erase on
a board fresh from the factory.

Then flash it with either tool:

- **pyOCD** (`pip install pyocd`; the built-in `nrf54l` target needs no
  pack, and it writes through a proper RRAM flash algorithm):

  ```sh
  pyocd list                                       # the probe's unique id
  pyocd flash -t nrf54l -u <probe id> mg-xiao-first.hex
  ```

  `pyocd flash` doesn't read the image back: check its exit status (never
  pipe it through `tail` without `set -o pipefail`), and expect the gadget
  to boot and advertise within 60 s; if it doesn't, stop and report. The OpenOCD recipe below verifies the image before it
  resets the chip.

- **OpenOCD** with the board's config (no `target/nordic/nrf54l.cfg`
  needed; the Zephyr SDK's OpenOCD or Homebrew's 0.12 both work):

  ```sh
  S=~/zephyr-sdk-1.0.1/hosttools/opt/openocd/share/openocd/scripts
  ~/zephyr-sdk-1.0.1/hosttools/usr/bin/openocd -s $S \
    -f ~/zephyrproject/zephyr/boards/seeed/xiao_nrf54l15/support/openocd.cfg \
    -c "adapter serial <probe serial>" -c init -c "targets nrf54l.cpu" -c "reset init" \
    -c "nrf54l-load mg-xiao-first.hex" -c "reset init" -c "verify_image mg-xiao-first.hex" \
    -c "reset run" -c shutdown
  ```

A chip with access port protection on needs a mass erase first, which wipes
everything: `pyocd erase -t nrf54l -u <probe id> --mass`, or with OpenOCD
`-c init -c nrf54l_mass_erase -c shutdown` (the board config also recovers a
locked chip by itself when it can't examine it). Updates over BLE (SMP) don't
have the alignment problem: img_mgmt writes through the flash driver.

The console is on the USB serial port at 115200 baud (uart20). A healthy boot
logs `Muse Gadget <version> starting` and `advertising as MuseGadget-XN54-XXXXXX`.
Holding the button while powering up, for 5 s, resets setup (`setup
complete`/`half-written setup` and `not set up` lines tell what boot found).

## Tests

`tests/host` runs anywhere with a C and a C++ compiler and mbedTLS (Homebrew
`mbedtls`, Debian `libmbedtls-dev`; found with `pkg-config mbedcrypto`, or
set `PSA_CFLAGS` / `PSA_LIBS`), with AddressSanitizer and UBSan:

```sh
python3 tests/host/run.py       # -v for the firmware's log lines
```

CI runs this suite on every pull request (`.github/workflows/zephyr.yml`,
Ubuntu's `libmbedtls-dev`); the native_sim and BabbleSim suites run locally.

The other two suites need Linux: native_sim and BabbleSim don't run on macOS, and
BabbleSim images are 32-bit x86 host programs (the Zephyr BLE controller is
not 64-bit clean). `tests/run_all.sh` runs everything inside Linux with
`ZEPHYR_BASE`, `BSIM_OUT_PATH` and `BSIM_COMPONENTS_PATH` set:

```sh
tests/run_all.sh          # unit (4 configurations) + BabbleSim (4 scenarios)
tests/run_all.sh unit
tests/run_all.sh bsim
tests/run_all.sh build    # the XIAO build checks: release, CONFIG_MG_AUDIO_QUEUE=n,
                          # CONFIG_MG_BATTERY_ADC=n (any host with the Zephyr SDK)
```

BabbleSim itself is built once in the workspace:

```sh
cd ~/zephyrproject/tools/bsim && ln -sf components/common/Makefile Makefile
BSIM_OUT_PATH=$PWD BSIM_COMPONENTS_PATH=$PWD/components make everything -j
```

(`libCryptov1` may fail to build; it is only needed for link-layer
encryption, which these tests don't use.)

### From a Mac

The unit and BabbleSim tests build native_sim and BabbleSim with the host's
32-bit x86 compiler, so they need an x86-64 Linux host: run them on one (a
Linux machine, a cloud VM or CI) with the workspace above. Don't run them
under x86 emulation on Apple silicon: it is slow, and the emulated toolchain
fails in places the native one doesn't. `tests/docker` builds the same
toolchain as an image for an x86-64 Linux host that has no Zephyr setup:

```sh
docker build -t mgzephyr86 tests/docker
tar -C /path/to/muse-gadget-sdk -cf - zephyr protocols xplat | \
  docker run -i --rm -v "$HOME/zephyrproject:/zp" \
  -e ZEPHYR_BASE=/zp/zephyr -e BSIM_OUT_PATH=/zp/tools/bsim \
  -e BSIM_COMPONENTS_PATH=/zp/tools/bsim/components -w /zp mgzephyr86 \
  bash -c 'mkdir -p /work && tar -xf - -C /work && /work/zephyr/tests/run_all.sh'
```

The XIAO image itself builds on any host with the Zephyr SDK, macOS on
Apple silicon included (above).

### What the tests cover

- `tests/host` (any OS), `test_gadget`: the gadget behavior
  pieces: the clock device actions frame by frame (persisted
  offset and flags, kept in RAM only by a set that may not store, refusals), the LED command never over the capture light, the low-battery
  thresholds, hysteresis and shutdown debounce (never without a cell), the
  boot record (unhealthy count, SAFE mode, healthy, breadcrumbs, CRC), the
  setup reset hold, the held capture output and the Nordic UART's word
  policy.
- `tests/host` (any OS): the shared seal's vectors
  (`../protocols/test-vectors/mg-session-seal-v1.json`) on this app's PSA
  glue; sealed credentials: a sealed hello on a plain link (no insecure,
  the flag echoed, nothing in the clear), a second sealed
  hello under one proof refused, sealed updates saved, a replayed counter,
  a flipped tag, the reflected direction, an unsealed update and a sealed
  non-object refused with the pair unchanged, a new proof sealing again,
  no session taking a sealed update after an unsealed hello's refusal; the
  device session against `../protocols/test-vectors/mg-session-v1.json`,
  every write of its exchanges (all but wake: no `mg_session_cap_wake` here) (the unsealed ones refusals, the sealed
  ones under its session_seal K and nonces) answered with exactly the
  vector's notifications, with the real token proof and store behind it
  (the register params are the reference device's when no exchange carries
  them); every other session test runs sealed (the harness seals its data
  and credentials_update and opens the device's); the token update (both
  slots, a reboot, a failed write at each step, a power loss before the
  record, K kept), credentials without a device key or too long, unsealed
  hellos refused on both links, the send queue and credit, a full notification
  queue at every point of a result and of the hello and credentials, and at
  credentials_saved, a lost client write, link.invoke's
  checks and failures, busy while the send queue is full, /link-control
  messages split, joined and oversized, /link-control reset or refused,
  and the board's commands; the envelope decoder's refusals by hand and the
  codec against `../esp32/components/noise_core`'s ServiceCodec.cpp (random
  frames encoded, random and damaged bytes decoded, by both); the Link setup crypto against
  `../esp32/tests/vectors/link_pairing_v5.json` (keys, ECDH, transcript,
  hash, session secret, record keys, session id, AAD, client_finished both
  ways, tampering) and RFC 5869; the JSON reader and writer (escapes,
  surrogates, depth, duplicates, malformed input); Link setup driven as the
  app drives it, with the gadget's identity and ephemeral key set to the
  vector's so pairing_ready must match it: device_info, the plaintext rules,
  every phase, the press, sdk_token, provision_v2's refusals, token-only
  provisioning and what it stores, K against the proof vectors, a failed
  write at each of the four steps rolled back, boot recovery, the timeouts
  (a silent client included), replay, small MTUs, reset; re-provisioning a set-up gadget (on the same
  connection and a new one, the new K replacing the old one in the proof,
  a matched proof dropped at the commit, kept across a reboot, an abandoned
  or refused setup and a failed write at each step keeping the old set, a
  power loss at each point of the commit, the commit ending an open device
  session); the device key (every malformed form refused with nothing
  stored, the VM binding stored and sent in the credentials, a keyless
  re-setup dropping both, a failed write at the key, its id, the vm_id or
  the record keeping the old set, a token update keeping them, records
  from before the binding loading); and the token proof frame by frame against
  `../protocols/test-vectors/mg-token-proof-v1.json` and its rules (a
  match counting only on its own connection).
- `tests/unit` (ztest, native_sim): settings (validation, persistence,
  per-connection reset), the protocol engine through `mg_session` with the
  audio thread faked (mic output only to a proven central, request_status
  and settings replies and their bounds, capture formats, the queue
  commands, SAFE mode), SBC and LC3 round trips (SNR), the clip
  queue on NOR-style flash and on RRAM-style rewritable flash
  (`CONFIG_FLASH_SIMULATOR_EXPLICIT_ERASE=n`) including power-loss recovery,
  the queue in MCUboot's slot1 (the `slot1` configuration): it never writes
  the image magic at offset 0 or the trailer even when full, and the slot
  changes hands (clips, upload, an image across a reboot, reclaimed, clips
  again) with the ownership rules; the clips erased at an owner change (a
  re-setup's commit, Forget, a clip being recorded) and at the next boot
  after a power loss before the erase; the typed settings list with and without a
  vibration motor (the `slot1` configuration has none, like the XIAO), the
  mic high-pass's primed start, the throughput test (packet format, gaps,
  refusals), and the token proof through the engine with Zephyr's PSA Crypto
  and ZMS (every vector frame by frame, clear, the commands that need it,
  set_unix_time storing only when proven, output ending when the match is
  reset) (`test_proof.c`).
- `tests/bsim`: the real firmware image (synthetic audio source, built
  without MCUboot: the queue is on its own `audio_queue_partition`) against a
  test central on simulated radios. `offline`:
  enable the queue, disconnect, two presses, reconnect, clip_info, download
  and decode both clips, clear. `setup`: both advertising payloads from one
  address, then the central as the Muse app: get_device_info, hello, the
  transcript hash and session id it computes itself, client_finished,
  confirm_required, the gadget's hook presses the button, pairing_confirmed,
  Wi-Fi refused, token-only provision_v2, auth_ok; the token proof on the
  same connection; reconnect (mg only advertised, proof matches), set up
  again on that connection (the hook holds the button 3 s; the old K fails the
  proof, the new one matches), clear, reconnect (Link setup advertised, no
  token). The Link setup payload's flags byte is checked (0x02). `setup`
  runs twice: on the bench build's `gadget-` node id and on a gadget built
  with the default (`homelink-`, `gadget-homelink`).
- `tests/smp_link` (native_sim, clients on the Mac): the Muse apps' update
  flows as `smplink.py` models them: update at several MTUs, resume (from the
  first block too), a stale partial request (a lost write), a corrupted
  payload refused by MCUboot, revert of a never-healthy image, keeping one
  after a link that confirms nothing (none without a link); the update gate
  and its carry-on (the link's auth message, 0x09, proves or un-proves the
  connection: a proof lapsing mid-upload, a new upload or an erase after it, a
  retry from 0, a link drop, one write packing a read and a reset).
- The unit tests print the encoded streams (`MGSTREAM` lines) for decoding
  with other decoders.

## Say Muse, never Hatch

Anything a person reads (logs, docs, Kconfig prompts, the advertised name)
says Muse. Don't use `hatch` in file names or identifiers.

## Before you hand back work

1. The board build (`--sysbuild`) passes.
2. `tests/host/run.py` passes, and `tests/run_all.sh` on Linux.
3. New files carry the Apache-2.0 header; vendored code keeps its own.
