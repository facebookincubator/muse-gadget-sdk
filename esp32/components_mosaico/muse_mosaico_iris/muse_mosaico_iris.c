/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "muse_bootguard.h"
#include "../../components/muse/muse_line_origin.h"   /* the one definition; no dependency on muse */
#include "muse_restart.h"

/* The bench console over ESP-Iris (tools/muse/iris_bench.py): the board's
 * console is on a UART header, not its USB-C port. Method 1 runs one console
 * line (">ui.dump=buf", ">ui.menu", ...) and answers 1 if a command took it;
 * method 2 reads the dump ui.dump=buf left: u32 offset in, u32 total (0: not
 * ready) and the bytes from there out; offset 0xFFFFFFFF frees it. */
#define BENCH_RPC_SERVICE 0x4D42   /* "MB" */
#define BENCH_RPC_LINE 1
#define BENCH_RPC_DUMP 2
#define BENCH_RPC_BOOT 3   /* how this boot began (muse_restart_boot_describe) */

bool muse_input_console_line(char *line) __attribute__((weak));
void muse_ble_command(char *cmd, muse_line_origin_t origin) __attribute__((weak));
size_t muse_video_dump_read(size_t off, void *out, size_t cap, size_t *total) __attribute__((weak));
void muse_video_dump_release(void) __attribute__((weak));

static esp_err_t bench_line(const esp_iris_rpc_request_t *rq, uint8_t *out, size_t cap, size_t *len, void *ctx)
{
    (void)ctx;
    char line[160];
    size_t n = rq->payload_size < sizeof(line) - 1 ? rq->payload_size : sizeof(line) - 1;
    memcpy(line, rq->payload, n);
    line[n] = 0;
    if (n && line[0] == '>') {
        memmove(line, line + 1, n);
    }
    if (cap < 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    out[0] = muse_input_console_line && muse_input_console_line(line);
    /* The device settings the S3 boards' serial console takes the same way
     * (muse_ble_command): the speaker level for a test, say. Never in SAFE: SAFE's console refused the line
     * (Muse is off), and that refusal stands. */
    static const char *const settings[] = { "volume=", "speaker=", "mic_gain=", "brightness=", "sleep=" };
    bool safe = muse_bootguard_mode() == MUSE_BOOT_SAFE;
    for (size_t i = 0; !out[0] && !safe && muse_ble_command && i < sizeof(settings) / sizeof(settings[0]); i++) {
        if (!strncmp(line, settings[i], strlen(settings[i]))) {
            muse_ble_command(line, MUSE_LINE_WIRELESS);   /* an RPC, not the console port (settings only) */
            out[0] = 1;
        }
    }
    *len = 1;
    return ESP_OK;
}

static esp_err_t bench_dump(const esp_iris_rpc_request_t *rq, uint8_t *out, size_t cap, size_t *len, void *ctx)
{
    (void)ctx;
    if (rq->payload_size < 4 || cap < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t off;
    memcpy(&off, rq->payload, 4);
    if (off == UINT32_MAX) {
        if (muse_video_dump_release) {
            muse_video_dump_release();
        }
        memset(out, 0, 4);
        *len = 4;
        return ESP_OK;
    }
    size_t total = 0;
    size_t n = muse_video_dump_read ? muse_video_dump_read(off, out + 4, cap - 4, &total) : 0;   /* a copy, never a pointer */
    uint32_t t = (uint32_t)total;
    memcpy(out, &t, 4);
    *len = 4 + n;
    return ESP_OK;
}

static esp_err_t bench_boot(const esp_iris_rpc_request_t *rq, uint8_t *out, size_t cap, size_t *len, void *ctx)
{
    (void)rq;
    (void)ctx;
    char why[192];
    muse_restart_boot_describe(why, sizeof(why));
    size_t n = strlen(why) < cap ? strlen(why) : cap;
    memcpy(out, why, n);
    *len = n;
    return ESP_OK;
}

/*
 * ESP-Iris's RPC table holds CONFIG_ESP_IRIS_MAX_RPC_HANDLERS methods. The way back's come first after the
 * mirror's (iris_ota_support_start: OTA state, OTA accept, enter recovery, each under ESP_ERROR_CHECK, so one that
 * didn't fit would abort the boot before Iris starts), then these bench methods, each only a warning if it doesn't
 * fit. The build fails unless all of them fit with room to spare.
 */
#define IRIS_WAY_BACK_RPCS 3   /* iris_ota_support_start's (esp_mosaico_app_recovery) */
#if CONFIG_MUSE_MIRROR_IRIS
#define IRIS_MIRROR_RPCS 3     /* muse_mirror_iris_register's: board, stream, input */
#else
#define IRIS_MIRROR_RPCS 0
#endif
#define IRIS_RPC_HEADROOM 4

static const struct {
    uint16_t method;
    esp_err_t (*fn)(const esp_iris_rpc_request_t *rq, uint8_t *out, size_t cap, size_t *len, void *ctx);
    const char *name;
} s_bench_rpcs[] = {
    { BENCH_RPC_LINE, bench_line, "line" },
    { BENCH_RPC_DUMP, bench_dump, "dump" },
    { BENCH_RPC_BOOT, bench_boot, "boot" },
};
#define BENCH_RPCS (sizeof(s_bench_rpcs) / sizeof(s_bench_rpcs[0]))
_Static_assert(IRIS_MIRROR_RPCS + IRIS_WAY_BACK_RPCS + BENCH_RPCS + IRIS_RPC_HEADROOM <= CONFIG_ESP_IRIS_MAX_RPC_HANDLERS,
               "ESP-Iris's RPC table (CONFIG_ESP_IRIS_MAX_RPC_HANDLERS) must hold the mirror's and the way back's "
               "methods, the bench methods and IRIS_RPC_HEADROOM more");

/* After iris_ota_support_start: a table that's full anyway costs a bench word, never the way back. */
static void bench_rpc_register(void)
{
    for (size_t i = 0; i < BENCH_RPCS; i++) {
        esp_err_t err = esp_iris_rpc_register(BENCH_RPC_SERVICE, s_bench_rpcs[i].method, s_bench_rpcs[i].fn, NULL);
        if (err != ESP_OK) {
            ESP_LOGW("muse_iris", "bench console method %s not registered (%s)", s_bench_rpcs[i].name,
                     esp_err_to_name(err));
        }
    }
}

void muse_mirror_iris_register(void);

/*
 * ESP-Iris on the ESP-Mosaico's USB-C port, as Espressif's application
 * template starts it: the board's Vibe Mode firmware switches a running app
 * back to Vibe Mode over it to install the next build, a crash loop falls back
 * to Vibe Mode, and it marks a newly installed app healthy a few seconds in.
 * The mirror's RPCs register before Iris starts, as its other services do.
 */
void muse_mosaico_iris_start(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
#if CONFIG_MUSE_MIRROR_IRIS
    muse_mirror_iris_register();
#endif
    iris_ota_support_start();   /* the way back's methods, then Iris starts */
    bench_rpc_register();       /* then the bench methods (ESP-Iris takes methods registered after its start) */
}
