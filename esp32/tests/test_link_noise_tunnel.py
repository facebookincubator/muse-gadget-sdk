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

"""Exercise production tunnel/control TX with deterministic DMA pressure and failures."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinkNoiseTunnelTest(unittest.TestCase):
    def run_harness(self, case):
        cache = ROOT / ".cache" / "tunnel-throughput"
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=cache) as tmpdir:
            out = Path(tmpdir)
            # Replace only SDK headers, retaining every production function.
            source = (ROOT / "main/noise_tunnel.cpp").read_text()
            source = "\n".join(
                line for line in source.splitlines()
                if not line.startswith(('#include "esp_', '#include "freertos/'))
            )
            (out / "noise_tunnel.inc").write_text(source + "\n")
            control = (ROOT / "main/noise_control.cpp").read_text()
            start = control.index("static char s_register_req_id[")
            end = control.index("static TaskHandle_t", start)
            registration_state = control[start:end]
            start = control.index("static void process_inbound_body_chunk(")
            end = control.index("// The session loop's pause", start)
            (out / "control_inbound.inc").write_text(
                registration_state + control[start:end])
            start = control.index("        s_register_acked = false;")
            end = control.index("        char *reg_json", start)
            (out / "control_registration_reset.inc").write_text(control[start:end])
            start = control.index("struct pending_result {")
            end = control.index("};", start) + 2
            (out / "pending_result.inc").write_text(control[start:end] + "\n")
            # Execute the actual queue drain, tunnel reopen/pump/tick and health
            # check together. Only unrelated RX and WebSocket I/O are omitted.
            start = control.index("        // Once registration is acked")
            end = control.index("        // RX:", start)
            initial_open = control[start:end]
            start = control.index("        // TX: keep exactly one control message")
            end = control.index("    s_connected = false;", start)
            end = control.rindex("    }", start, end)
            (out / "control_tx_turn.inc").write_text(
                initial_open + control[start:end])
            start = control.index("static bool flush_outbound(")
            end = control.index("// ---- Tunnel stream multiplexing", start)
            # The tunnel keeps the full-size control session buffers.
            constants = "#define SMALL_CONTROL_SESSION 0\n#define CARDPUTER_CONTROL_SESSION 0\n" + "\n".join(line for line in control.splitlines()
                                  if line.startswith(("#define CTRL_STREAM_ID",
                                                      "#define CTRL_BODY_CHUNK_MAX",
                                                      "#define OUT_SVC_SCRATCH",
                                                      "#define OUT_ENV_SCRATCH",
                                                      "#define WS_HEADROOM")))
            ws_start = control.index("#if CONFIG_SPIRAM\n#define WS_BUF_SIZE")
            ws_end = control.index("#endif", ws_start) + len("#endif")
            constants += "\n" + control[ws_start:ws_end]
            (out / "control_sender.inc").write_text(constants + "\n" + control[start:end])
            start = control.index("cleanup:\n") + len("cleanup:\n")
            end = control.index("    free(ws_buf);", start)
            (out / "control_cleanup.inc").write_text(control[start:end])
            start = control.index("    // Drain any stale results from previous sessions")
            end = control.index("    // 5. Queue link.register", start)
            (out / "control_start_drain.inc").write_text(control[start:end])
            env = {**os.environ, "TMPDIR": tmpdir,
                   "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                   "UBSAN_OPTIONS": "halt_on_error=1"}
            binary = out / "noise_tunnel"
            json_object = out / "cJSON.o"
            compiled_json = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                 "-Wall", "-Wextra", "-Werror", "-g",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-c", str(ROOT / "tests/link_fakes/cJSON.c"),
                 "-o", str(json_object)],
                capture_output=True, text=True, env=env,
            )
            self.assertEqual(compiled_json.returncode, 0,
                             compiled_json.stdout + compiled_json.stderr)
            core = ROOT / "components/noise_core"
            core_sources = [core / "src" / (name + ".cpp") for name in (
                "ClientSession", "InitiatorHandshake", "ServiceCodec", "Status",
                "Transport", "TransportFrameCodec")]
            compiled = subprocess.run(
                [*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                 "-Wall", "-Wextra", "-Werror", "-g",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-DCONFIG_SPIRAM=1", "-DCONFIG_HOMEHUB_TUNNEL=1",
                 "-I", str(ROOT / "main"), "-I", tmpdir,
                 "-I", str(core / "include"),
                 str(ROOT / "tests/link_noise_tunnel_harness.cpp"),
                 str(json_object),
                 *(str(src) for src in core_sources),
                 "-o", str(binary)],
                capture_output=True, text=True, env=env,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary), case], capture_output=True, text=True, env=env)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_backpressure_ownership_fairness_and_failure(self):
        self.run_harness("tunnel")

    def test_control_prefix_stream_and_no_whole_payload_copy(self):
        self.run_harness("stream")

    def test_control_contiguous_pressure_pause_resume_and_keepalives(self):
        for case in ("pressure", "heartbeat"):
            with self.subTest(case=case):
                self.run_harness(case)

    def test_sustained_fragmentation_makes_bounded_progress(self):
        self.run_harness("fragmentation")

    def test_each_control_chunk_requires_total_free_burst_margin(self):
        self.run_harness("burst")

    def test_tunnel_ping_defers_under_fragmentation_and_retries(self):
        self.run_harness("ping_fragmentation")

    def test_control_partial_teardown_and_stale_generation(self):
        self.run_harness("lifecycle")

    def test_control_attempted_send_failure_is_fatal(self):
        self.run_harness("failures")

    def test_config_guard_rejects_either_stale_tcp_limit(self):
        source = (ROOT / "cmake" / "validate_config.cmake").read_text()
        start = source.index("# TCP limits:")
        guard = source[start:source.index("endif()", start) + len("endif()")]
        cache = ROOT / ".cache" / "tunnel-throughput"
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=cache) as tmpdir:
            script = Path(tmpdir) / "guard.cmake"
            for snd, wnd, cardputer, tunnel, psram, accepted, lwip_psram in (
                (16384, 16384, False, True, True, True, False),
                (65535, 16384, False, True, True, False, False),
                (16384, 65535, False, True, True, False, False),
                # lwIP and Wi-Fi buffers in PSRAM: up to 32 KiB, no more.
                (32768, 32768, False, True, True, False, False),
                (32768, 32768, False, True, True, True, True),
                (16384, 32768, False, True, True, True, True),
                (65535, 32768, False, True, True, False, True),
                (32768, 65535, False, True, True, False, True),
                (5760, 5760, True, False, False, True, False),
                (5760, 5760, True, True, False, False, False),
                (5760, 5760, True, False, True, False, False),
                (5760, 5760, False, False, False, False, False),
            ):
                with self.subTest(snd=snd, wnd=wnd, lwip_psram=lwip_psram):
                    script.write_text(
                        f"set(CONFIG_LWIP_TCP_SND_BUF_DEFAULT {snd})\n"
                        f"set(CONFIG_LWIP_TCP_WND_DEFAULT {wnd})\n"
                        f"set(CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV {'ON' if cardputer else 'OFF'})\n"
                        f"set(CONFIG_HOMEHUB_TUNNEL {'ON' if tunnel else 'OFF'})\n"
                        f"set(CONFIG_SPIRAM {'ON' if psram else 'OFF'})\n"
                        f"set(CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP {'ON' if lwip_psram else 'OFF'})\n" + guard
                    )
                    ran = subprocess.run(
                        ["cmake", "-P", str(script)],
                        capture_output=True, text=True,
                        env={**os.environ, "TMPDIR": tmpdir},
                    )
                    self.assertEqual(ran.returncode == 0, accepted,
                                     ran.stdout + ran.stderr)
                    if not accepted:
                        self.assertIn("DMA headroom", " ".join(ran.stderr.split()))


if __name__ == "__main__":
    unittest.main()
