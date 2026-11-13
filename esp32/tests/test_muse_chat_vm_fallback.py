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

"""Host test for standalone token -> Link VM fallback in muse_chat_session.cpp."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ChatSessionVmFallbackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        if not cxx or shutil.which(cxx[0]) is None:
            raise unittest.SkipTest("C++ compiler not available")

        source = (ROOT / "components/muse/muse_chat_session.cpp").read_text(encoding="utf-8")
        forget_fn = source[
            source.index("static void forget_vm(void)") : source.index("static bool open_subscription(void)")
        ]
        ensure_fn = source[
            source.index("static bool ensure_connected(void)") : source.index("/* ---- Resampling ---- */")
        ]
        forget_case = source[
            source.index("    case CMD_FORGET:") : source.index("    case CMD_BEGIN:")
        ]

        harness = (
            r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "host_compat.h"

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define MUSE_VM_MAX 64
#define MUSE_TOKEN_MAX 256
#define MUSE_HOST_MAX 64
#define MUSE_HATCH_API_AUTH (-2)
#define AUTO_RETRY_MIN_US 5000000LL

enum muse_hatch_state_t {
    MUSE_HATCH_TESTING,
    MUSE_HATCH_REACHABLE,
    MUSE_HATCH_UNREACHABLE,
};

struct muse_hatch_vm_t {
    char vm_id[MUSE_VM_MAX + 1];
    char vm_name[64];
    char *vm_token;
};

enum phase_t { P_IDLE, P_LISTEN };
struct turn_t { phase_t phase; };
enum cmd_type_t : uint8_t { CMD_FORGET };
struct cmd_t { cmd_type_t type; };

static const char *TAG = "test";
static bool s_connected;
static muse_hatch_vm_t s_vm;
static bool s_vm_direct;
static bool s_ignore_local_token;
static char s_host[MUSE_HOST_MAX + 1];
static int64_t s_auto_next_us;
static int64_t s_auto_backoff_us;
static turn_t s_turn = { P_IDLE };

static char fake_local_token[MUSE_TOKEN_MAX + 1];
static char fake_want_vm[MUSE_VM_MAX + 1];
static bool fake_linked;
static bool fake_link_ok;
static int fake_api_rc;
static int fake_http_statuses[4];
static int fake_connect_calls;
static int fake_api_calls;
static int fake_link_calls;
static char last_err[64];
static char connected_vm_id[64];
static char connected_vm_token[128];

static size_t muse_settings_hatch_token_len(void) { return strlen(fake_local_token); }
static void muse_settings_hatch_token(char *out) { strlcpy(out, fake_local_token, MUSE_TOKEN_MAX + 1); }
static void muse_settings_hatch_vm(char *out) { strlcpy(out, fake_want_vm, MUSE_VM_MAX + 1); }
static void muse_settings_hatch_host(char *out) { out[0] = '\0'; }
static bool muse_link_hatch_linked(void) { return fake_linked; }
static bool muse_hatch_configured(void) { return fake_linked || fake_local_token[0] != '\0'; }
static bool muse_wifi_connected(void) { return true; }
static void muse_hatch_report(muse_hatch_state_t, const char *err) {
    strlcpy(last_err, err ? err : "", sizeof(last_err));
}
static void muse_hatch_report_vm(const char *) {}

static bool muse_link_hatch_vm(const char *want, char *id, size_t id_cap,
                               char *name, size_t name_cap, char **token_out) {
    fake_link_calls++;
    if (!fake_linked || !fake_link_ok) return false;
    strlcpy(id, want && want[0] ? want : "vm-link-1", id_cap);
    strlcpy(name, "Link VM", name_cap);
    *token_out = strdup("link-vm-secret");
    return true;
}

static int muse_hatch_api_find_vm(const char *token, const char *want, muse_hatch_vm_t *out) {
    fake_api_calls++;
    if (fake_api_rc != 0) return fake_api_rc;
    strlcpy(out->vm_id, want && want[0] ? want : "vm-local-1", sizeof(out->vm_id));
    strlcpy(out->vm_name, "Local VM", sizeof(out->vm_name));
    out->vm_token = strdup(token);
    return 0;
}

static void disconnect(const char *) { s_connected = false; }
static void pending_clear(void) {}
static void turn_fail(const char *) { s_turn.phase = P_IDLE; }

static bool connect_once(char *err, size_t err_cap, int *http_status) {
    int idx = fake_connect_calls < 4 ? fake_connect_calls : 3;
    fake_connect_calls++;
    int status = fake_http_statuses[idx];
    *http_status = status;
    if (status == 101) {
        s_connected = true;
        strlcpy(connected_vm_id, s_vm.vm_id, sizeof(connected_vm_id));
        strlcpy(connected_vm_token, s_vm.vm_token ? s_vm.vm_token : "", sizeof(connected_vm_token));
        return true;
    }
    strlcpy(err, (status == 401 || status == 403) ? "VM refused the token" : "HTTP error", err_cap);
    return false;
}
'''
            + forget_fn
            + ensure_fn
            + r'''
static void handle_forget(void) {
    cmd_t cmd{ CMD_FORGET };
    switch (cmd.type) {
'''
            + forget_case
            + r'''
    }
}

static void reset_env(void) {
    disconnect("reset");
    forget_vm();
    s_ignore_local_token = false;
    fake_local_token[0] = '\0';
    fake_want_vm[0] = '\0';
    fake_linked = false;
    fake_link_ok = true;
    fake_api_rc = 0;
    for (int i = 0; i < 4; i++) fake_http_statuses[i] = 101;
    fake_connect_calls = fake_api_calls = fake_link_calls = 0;
    last_err[0] = connected_vm_id[0] = connected_vm_token[0] = '\0';
}

int main(void) {
    // 1. Valid standalone token is preserved and used even when Link is paired.
    reset_env();
    strlcpy(fake_local_token, "valid-standalone", sizeof(fake_local_token));
    fake_linked = true;
    fake_api_rc = 0;
    assert(ensure_connected());
    assert(fake_api_calls == 1 && fake_link_calls == 0);
    assert(strcmp(connected_vm_id, "vm-local-1") == 0);
    assert(!s_ignore_local_token);

    // 2. Rejected standalone token (account API 401) falls back to Link and remembers across reconnects.
    reset_env();
    strlcpy(fake_local_token, "expired-standalone", sizeof(fake_local_token));
    fake_linked = true;
    fake_api_rc = MUSE_HATCH_API_AUTH;
    assert(ensure_connected());
    assert(fake_api_calls == 1 && fake_link_calls == 1);
    assert(strcmp(connected_vm_id, "vm-link-1") == 0);
    assert(strcmp(connected_vm_token, "link-vm-secret") == 0);
    assert(s_ignore_local_token);

    disconnect("drop");
    forget_vm();
    assert(ensure_connected());
    assert(fake_api_calls == 1 && fake_link_calls == 2);

    // 3. CMD_FORGET (settings updated) clears s_ignore_local_token so a newly entered token is tried.
    handle_forget();
    assert(!s_ignore_local_token);
    strlcpy(fake_local_token, "fresh-standalone", sizeof(fake_local_token));
    fake_api_rc = 0;
    assert(ensure_connected());
    assert(fake_api_calls == 2 && strcmp(connected_vm_id, "vm-local-1") == 0);

    // 4. Explicit VM ID + rejected direct VM token (HTTP 401) retries via Link on attempt 2.
    reset_env();
    strlcpy(fake_local_token, "bad-direct-vm-token", sizeof(fake_local_token));
    strlcpy(fake_want_vm, "vm-explicit", sizeof(fake_want_vm));
    fake_linked = true;
    fake_api_rc = MUSE_HATCH_API_AUTH;
    fake_http_statuses[0] = 401;
    fake_http_statuses[1] = 101;
    assert(ensure_connected());
    assert(fake_connect_calls == 2 && fake_link_calls == 1);
    assert(s_ignore_local_token);
    assert(strcmp(connected_vm_id, "vm-explicit") == 0);
    assert(strcmp(connected_vm_token, "link-vm-secret") == 0);

    // 5. Rejected standalone token without Link pairing still reports Token rejected cleanly.
    reset_env();
    strlcpy(fake_local_token, "expired-standalone", sizeof(fake_local_token));
    fake_linked = false;
    fake_api_rc = MUSE_HATCH_API_AUTH;
    assert(!ensure_connected());
    assert(fake_link_calls == 0 && !s_ignore_local_token);
    assert(strcmp(last_err, "Token rejected") == 0);

    forget_vm();
    return 0;
}
'''
        )

        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        src_path = Path(cls.tmp.name) / "vm_fallback_harness.cpp"
        bin_path = Path(cls.tmp.name) / "vm_fallback_harness"
        src_path.write_text(harness, encoding="utf-8")
        subprocess.run(
            [
                *cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "tests"),
                str(src_path),
                "-o",
                str(bin_path),
            ],
            check=True,
        )
        cls.binary = bin_path

    def test_standalone_token_fallback_and_reset(self) -> None:
        subprocess.run([str(self.binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
