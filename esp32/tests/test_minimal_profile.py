# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Compile the production minimal-profile gates and real service-frame bounds.

No board, IDF build, credential, network, or persisted device state is needed.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "components/noise_core"



def between(text, start, end):
    at = text.index(start)
    return text[at:text.index(end, at)]


class MinimalProfile(unittest.TestCase):
    def run_ok(self, command, **kwargs):
        result = subprocess.run(command, capture_output=True, text=True, timeout=30, **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_console_defaults_on_and_cmake_omits_only_its_source_when_off(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        entry = between(kconfig, "    config GADGET_SDK_TOKEN_CONSOLE\n",
                        "    config HOMEHUB_WIFI_MIN_MODEM\n")
        self.assertIn("default y", entry)
        self.assertNotIn("depends on", entry)
        # Evaluate the actual CMake source selection, stopping before IDF registration.
        cmake = (ROOT / "main/CMakeLists.txt").read_text().split("idf_component_register(", 1)[0]
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "sources.cmake"
            result = Path(tmp) / "sources.txt"
            # The default SDK has no product control extension. IDF supplies
            # these build properties before the source-selection code runs.
            build_properties = ('function(idf_build_get_property output property)\n'
                                '    set(${output} "" PARENT_SCOPE)\n'
                                'endfunction()\n')
            script.write_text(build_properties + cmake
                              + f'\nfile(WRITE "{result}" "${{GADGET_SRCS}}")\n')
            lists = {}
            for on in (0, 1):
                self.run_ok(["cmake", f"-DCONFIG_GADGET_SDK_TOKEN_CONSOLE={on}", "-P", str(script)])
                lists[on] = set(result.read_text().split(";"))
            self.assertEqual(lists[1] - lists[0], {"sdk_token_console.c"})
            self.assertFalse(lists[0] - lists[1])
            self.assertIn("wifi_known.c", lists[0])

    def test_console_startup_and_muse_dispatch_on_off_with_apps(self):
        app = (ROOT / "main/app.c").read_text()
        startup = app.split("vm_api_set_sdk_token(identity_sdk_token());", 1)[1].split(
            "    if (!led_status_init())", 1)[0]
        glue = (ROOT / "main/muse_glue.c").read_text()
        guard = "#if CONFIG_MUSE_WASM_APPS || CONFIG_MUSE_AVATAR_SYNC\n"
        console = between(glue, guard + "// The console's setup lines:",
                          "void muse_glue_start(void)")
        hooks = between(glue, guard + "    muse_input_set_console_hook",
                        "    muse_ble_set_name")
        prelude = r"""
#include <assert.h>
#include <stdbool.h>
#include <string.h>
static int starts, sdk_calls, app_calls, avatar_calls, apps_started;
static bool (*hook)(const char *);
#if CONFIG_GADGET_SDK_TOKEN_CONSOLE
static void sdk_token_console_start(void) { ++starts; }
static bool sdk_token_console_command(const char *s) { ++sdk_calls; return !strcmp(s, "sdk_token=test"); }
#endif
#if CONFIG_MUSE_AVATAR_SYNC
static bool avatar_sync_console(const char *s) { ++avatar_calls; return !strcmp(s, "avatar=test"); }
#endif
#if CONFIG_MUSE_WASM_APPS
static bool muse_apps_console(const char *s) { ++app_calls; return !strcmp(s, "app=test"); }
static void noise_ctrl_send_command_result(void) {}
static void muse_apps_start(void (*send)(void)) { (void)send; ++apps_started; }
#endif
#if CONFIG_MUSE_WASM_APPS || CONFIG_MUSE_AVATAR_SYNC || CONFIG_GADGET_SDK_TOKEN_CONSOLE
static void muse_input_set_console_hook(bool (*fn)(const char *)) { hook = fn; }
#endif
"""
        # Each line reaches the handlers in order (SDK token, avatar, apps)
        # until one takes it; a disabled handler is neither compiled nor called.
        checks = r"""
#define CONSOLE CONFIG_GADGET_SDK_TOKEN_CONSOLE
#define AVATAR CONFIG_MUSE_AVATAR_SYNC
#define APPS CONFIG_MUSE_WASM_APPS
int main(void) {
    boot();
    assert(starts == (CONSOLE && !CONFIG_MUSE_ENABLED));
    install_hook();
    assert(!!hook == (CONSOLE || AVATAR || APPS));
    assert(apps_started == APPS);
    if (hook) {
        assert(hook("sdk_token=test") == CONSOLE);
        assert(hook("avatar=test") == AVATAR);
        assert(hook("app=test") == APPS);
    }
    assert(sdk_calls == (CONSOLE ? 3 : 0));
    assert(avatar_calls == (AVATAR ? (CONSOLE ? 2 : 3) : 0));
    assert(app_calls == (APPS ? 1 + !CONSOLE + !AVATAR : 0));
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "gates.c", Path(tmp) / "gates"
            src.write_text(prelude + console + "\nstatic void boot(void) {\n" + startup
                           + "}\nstatic void install_hook(void) {\n" + hooks + "}\n" + checks)
            for on in (0, 1):
                for muse in (0, 1):
                    for apps in (0, 1):
                        for avatar in (0, 1):
                            with self.subTest(console=on, muse=muse, apps=apps, avatar=avatar):
                                self.run_ok([*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                                             # Stubs can be unused in some feature combinations.
                                             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                             f"-DCONFIG_GADGET_SDK_TOKEN_CONSOLE={on}",
                                             f"-DCONFIG_MUSE_ENABLED={muse}",
                                             f"-DCONFIG_MUSE_WASM_APPS={apps}",
                                             f"-DCONFIG_MUSE_AVATAR_SYNC={avatar}",
                                             str(src), "-o", str(exe)])
                                self.run_ok([str(exe)])

    def test_console_off_setup_lines_are_not_logged(self):
        # With the console off, a typed sdk_token= or wifi_proxy= line falls
        # through to the Muse command handler, whose log must not echo it.
        ble = (ROOT / "components/muse/muse_ble.c").read_text()
        start = ble.index("    bool secret = ")
        predicate = ble[start:ble.index(";", start) + 1]
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "secret.c", Path(tmp) / "secret"
            src.write_text("#include <assert.h>\n#include <stdbool.h>\n#include <string.h>\n"
                           "static bool is_secret(const char *cmd) {\n" + predicate
                           + "\n    return secret;\n}\nint main(void) {\n"
                           '    assert(is_secret("sdk_token") && is_secret("wifi_proxy"));\n'
                           '    assert(is_secret("wifi.pass") && !is_secret("volume"));\n}\n')
            self.run_ok([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra",
                         "-Werror", str(src), "-o", str(exe)])
            self.run_ok([str(exe)])

    def test_service_scratch_policy_and_frame_round_trips(self):
        control = (ROOT / "main/noise_control.cpp").read_text()
        sizing = between(control, "#if (CONFIG_MUSE_ENABLED && !CONFIG_SPIRAM && !CONFIG_HOMEHUB_TUNNEL)",
                         "static char s_node_id[64];")
        tunnel = (ROOT / "main/noise_tunnel.cpp").read_text()
        batch = re.search(r"^#define TUN_MAX_MSG_BYTES (\d+)$", tunnel, re.M).group(1)
        # Production codec round-trips a maximum tunnel batch and a maximum
        # control chunk with its four-byte JSON length prefix, not just constants.
        exercise = r'''
static void round_trip(int64_t stream, size_t count) {
    std::vector<uint8_t> body(count, 0x5a), svc(SVC_FRAME_SCRATCH), env(SVC_FRAME_SCRATCH), wire(SVC_FRAME_SCRATCH);
    BodyChunkView chunk{ConstByteSpan(body.data(), body.size()), false};
    auto s = EncodeBodyChunkFrame(stream, chunk, ByteSpan(svc.data(), svc.size()));
    assert(s.ok());
    auto e = EncodeServiceResponseEnvelope(ConstByteSpan(svc.data(), s.size()), ByteSpan(env.data(), env.size()));
    assert(e.ok());
    TransportFrameView frame{UINT64_MAX, 0, 1, ConstByteSpan(env.data(), e.size())};
    auto w = EncodeTransportFrame(frame, ByteSpan(wire.data(), wire.size()));
    assert(w.ok());
    assert(w.size() + CryptoBackend::kAes256GcmTagSize <= WS_RX_BUF_SIZE);
    DecodedTransportFrame transport;
    assert(DecodeTransportFrame(ConstByteSpan(wire.data(), w.size()), transport).ok());
    DecodedServiceResponseEnvelope envelope;
    assert(DecodeServiceResponseEnvelopeDetailed(transport.payload, envelope).ok());
    DecodedServiceFrame decoded;
    assert(DecodeServerServiceFrameDetailed(envelope.payload, {}, decoded).ok());
    assert(decoded.stream_id == stream && decoded.body_chunk.data.size() == count);
    assert(!memcmp(decoded.body_chunk.data.data(), body.data(), count));
}
int main() {
    round_trip(2, TUN_MAX_MSG_BYTES);
    round_trip(1, CTRL_BODY_CHUNK_MAX + 4);
}
'''
        # (PSRAM, UI, tunnel, special board, scratch with daemon requests on)
        shapes = [(1, 0, 1, "", 65536), (1, 1, 0, "", 65536),
                  (0, 0, 0, "", 12288), (0, 1, 0, "", 17408),
                  (0, 1, 0, "MUSE_BOARD_AI_PASSPORT", 12288),
                  (0, 1, 0, "MUSE_BOARD_M5STACK_ATOMS3", 12288),
                  (0, 0, 0, "HOMEHUB_LED_BACKEND_CYD_ILI9341", 17408)]
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "bounds.cpp", Path(tmp) / "bounds"
            for spiram, muse, tunnel_on, board, daemon_size in shapes:
                for daemon in (0, 1):
                    with self.subTest(spiram=spiram, muse=muse, board=board, daemon=daemon):
                        config = (f"#define CONFIG_SPIRAM {spiram}\n#define CONFIG_MUSE_ENABLED {muse}\n"
                                  f"#define CONFIG_HOMEHUB_TUNNEL {tunnel_on}\n"
                                  f"#define CONFIG_HOMEHUB_DAEMON_REQUESTS {daemon}\n")
                        if board:
                            config += f"#define CONFIG_{board} 1\n"
                        expected = daemon_size if daemon else 12288
                        src.write_text(config + '''
#include <cassert>
#include <cstring>
#include <vector>
#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/InitiatorHandshake.h>
#include <xplat/noise/core/ServiceCodec.h>
using namespace musegadgets::noise::core;
''' + sizing + f'''
static_assert(SVC_FRAME_SCRATCH == {expected}, "service scratch policy");
#define TUN_MAX_MSG_BYTES {batch}
''' + (exercise if spiram and not muse else "int main() {}\n"))
                        self.run_ok([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                                     "-Wall", "-Wextra", "-Werror", "-I", str(CORE / "include"),
                                     str(src),
                                     str(CORE / "src/ServiceCodec.cpp"), str(CORE / "src/TransportFrameCodec.cpp"),
                                     str(CORE / "src/Status.cpp"), "-o", str(exe)])
                        self.run_ok([str(exe)])


if __name__ == "__main__":
    unittest.main()
