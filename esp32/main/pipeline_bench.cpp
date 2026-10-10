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

// On-device benchmark of the tunnel data path and BLE coexistence
// (CONFIG_HOMEHUB_PIPELINE_BENCH). Runs instead of the gadget app and prints
// one "BENCH <name> key=value ..." line per measurement. See
// tools/coexist/bench/README.md for the host side.

#include "sdkconfig.h"

#if CONFIG_HOMEHUB_PIPELINE_BENCH

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "esp_netif_net_stack.h"

extern "C" {
#include "ble_basic.h"
#include "ble_central.h"
#include "ble_server.h"
#include "config_store.h"
#include "identity.h"
#include "wifi_mgr.h"
#include "fast_gcm.h"
#include "ghash32.h"
#include "aes/esp_aes.h"
bool noise_bench_ws_send_inplace(esp_tls_t *tls, uint8_t *payload, size_t len);
void pipeline_bench_main(void);
}

#include "psa/crypto.h"
#include "mbedtls/ssl_ciphersuites.h"
#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/Transport.h>

using namespace musegadgets::noise::core;


#define BATCH_BYTES   8192
// ServiceFrame + envelope + transport frame header around an 8 KB batch.
#define FRAME_OVERHEAD 40
#define FRAME_BYTES   (BATCH_BYTES + FRAME_OVERHEAD)
#define WS_HEADROOM   32
#define WS_CAP        (16 * 1024)
#define PHASE_S       CONFIG_HOMEHUB_BENCH_SECONDS

static char s_host[48];
static bool s_tls_chacha;   // offer ChaCha20-Poly1305 first in TLS

static const char *gcm_name(fast_gcm_mode_t m) {
    return m == FAST_GCM_OFF ? "idf" : m == FAST_GCM_G4 ? "g4" : "g8";
}
static int s_tcp_port, s_tls_port;

// ---- helpers -----------------------------------------------------------------

static int64_t now_us() { return esp_timer_get_time(); }

static void heap_line(const char *label) {
    printf("BENCH heap label=%s int=%u int_big=%u dma=%u dma_big=%u dma_min=%u psram=%u\n",
           label,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// CPU busy share from the idle task's run-time counter.
struct cpu_meter {
    uint64_t idle0 = 0;
    int64_t t0 = 0;
    void start() {
        idle0 = ulTaskGetIdleRunTimeCounter();
        t0 = now_us();
    }
    int busy_pct() const {
        uint64_t idle = ulTaskGetIdleRunTimeCounter() - idle0;
        int64_t dt = now_us() - t0;
        if (dt <= 0) return -1;
        int pct = 100 - (int)(idle * 100 / (uint64_t)dt);
        return pct < 0 ? 0 : pct;
    }
};

static uint8_t *ps_alloc(size_t n, size_t align) {
    return static_cast<uint8_t *>(
        heap_caps_aligned_alloc(align, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
static uint8_t *int_alloc(size_t n, size_t align) {
    return static_cast<uint8_t *>(
        heap_caps_aligned_alloc(align, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
}

// ---- 1. memory copies ----------------------------------------------------------

static void bench_memcpy() {
    uint8_t *ia = int_alloc(BATCH_BYTES, 32), *ib = int_alloc(BATCH_BYTES, 32);
    uint8_t *pa = ps_alloc(BATCH_BYTES, 32), *pb = ps_alloc(BATCH_BYTES, 32);
    if (!ia || !ib || !pa || !pb) {
        printf("BENCH memcpy error=alloc\n");
        goto out;
    }
    {
        struct { const char *name; uint8_t *dst; const uint8_t *src; } cases[] = {
            {"int_to_int", ib, ia}, {"int_to_psram", pa, ia},
            {"psram_to_int", ia, pa}, {"psram_to_psram", pb, pa},
        };
        for (auto &c : cases) {
            const int iters = 400;
            int64_t t0 = now_us();
            for (int i = 0; i < iters; i++) memcpy(c.dst, c.src, BATCH_BYTES);
            int64_t dt = now_us() - t0;
            printf("BENCH memcpy case=%s bytes=%d MBps=%.1f us_per_8k=%.1f\n", c.name,
                   BATCH_BYTES, (double)iters * BATCH_BYTES / dt, (double)dt / iters);
        }
    }
out:
    free(ia); free(ib); free(pa); free(pb);
}

// ---- 2. AES-256-GCM seal/open through noise_core --------------------------------

static void bench_aead() {
    PsaCryptoBackend crypto;
    uint8_t k1[32], k2[32];
    esp_fill_random(k1, sizeof(k1));
    esp_fill_random(k2, sizeof(k2));
    uint8_t *ps = ps_alloc(FRAME_BYTES + 128, 32);
    uint8_t *in = int_alloc(FRAME_BYTES + 128, 32);
    uint8_t *ps_out = ps_alloc(FRAME_BYTES + 128, 32);
    if (!ps || !in || !ps_out) {
        printf("BENCH aead error=alloc\n");
        free(ps); free(in); free(ps_out);
        return;
    }
    struct { const char *name; uint8_t *buf; size_t len; } cases[] = {
        {"psram_off8_len8232", ps + 8, FRAME_BYTES},       // old ws_buf: not line-aligned
        {"psram_al32_len8232", ps, FRAME_BYTES},           // aligned, len%32 = 8
        {"psram_al32_len8224", ps, FRAME_BYTES - 8},       // aligned, len%32 = 0
        {"psram_al32_len1400", ps, 1400},                  // one MTU-sized packet
        {"internal_len8232", in, FRAME_BYTES},
        {"internal_len1400", in, 1400},
    };
    for (int variant = 0; variant < 4; variant++)
    for (auto &c : cases) {
        // idf, g4, g8 (tables internal), g8 with its tables in PSRAM
        fast_gcm_mode_t mode = variant == 0 ? FAST_GCM_OFF : variant == 1 ? FAST_GCM_G4 : FAST_GCM_G8;
        if (variant == 1 && strncmp(c.name, "psram_al32", 10) == 0) continue;
        fast_gcm_set_table_psram(variant == 3);
        fast_gcm_set_mode(mode);
        const int iters = 120;
        esp_fill_random(c.buf, c.len);
        bool ok = true;
        int64_t seal_dt = 0, open_dt = 0;
        {
            Transport tx(crypto, ConstByteSpan(k1, 32), ConstByteSpan(k2, 32));
            Transport rx(crypto, ConstByteSpan(k2, 32), ConstByteSpan(k1, 32));
            for (int i = 0; i < iters && ok; i++) {
                int64_t t0 = now_us();
                ok = tx.SealInPlace(ByteSpan(c.buf, c.len + Transport::kTagSize), c.len).ok();
                seal_dt += now_us() - t0;
                // Open into a separate PSRAM buffer, as the session's
                // tf_scratch. Every seal is opened so the nonces stay in step.
                t0 = now_us();
                ok = ok && rx.Open(ConstByteSpan(c.buf, c.len + Transport::kTagSize),
                                   ByteSpan(ps_out, c.len)).ok();
                open_dt += now_us() - t0;
            }
        }
        printf("BENCH aead gcm=%s%s case=%s ok=%d seal_MBps=%.2f seal_us=%.0f open_MBps=%.2f open_us=%.0f\n",
               gcm_name(mode), variant == 3 ? "_pstables" : "", c.name, ok ? 1 : 0, (double)iters * c.len / seal_dt, (double)seal_dt / iters,
               (double)iters * c.len / open_dt, (double)open_dt / iters);
    }
    free(ps); free(in); free(ps_out);
    fast_gcm_set_table_psram(false);
    fast_gcm_set_mode((fast_gcm_mode_t)CONFIG_HOMEHUB_FAST_GCM_DEFAULT_MODE);
}

// Fast GCM against IDF's: same key, nonce and AAD must give the same
// ciphertext and tag, and each must open the other's output.
static void bench_gcm_verify() {
    PsaCryptoBackend crypto;
    uint8_t *pt = ps_alloc(9000, 4), *a = ps_alloc(9100, 4), *b = ps_alloc(9100, 4);
    int checked = 0, bad = 0;
    for (size_t len : {0u, 1u, 15u, 16u, 17u, 31u, 100u, 1400u, 4095u, 8224u, 8232u, 8233u}) {
        for (int aadn : {0, 13, 40}) {
            for (fast_gcm_mode_t mode : {FAST_GCM_G4, FAST_GCM_G8}) {
                uint8_t key[32], nonce[12], aad[40];
                esp_fill_random(key, 32);
                esp_fill_random(nonce, 12);
                esp_fill_random(aad, sizeof(aad));
                esp_fill_random(pt, len);
                ConstByteSpan k(key, 32), n(nonce, 12), ad(aad, aadn), p(pt, len);
                fast_gcm_set_mode(FAST_GCM_OFF);
                bool ok = crypto.Aes256GcmSeal(k, n, ad, p, ByteSpan(a, len + 16)).ok();
                fast_gcm_set_mode(mode);
                ok = ok && crypto.Aes256GcmSeal(k, n, ad, p, ByteSpan(b, len + 16)).ok();
                ok = ok && memcmp(a, b, len + 16) == 0;
                // fast opens IDF's; IDF opens fast's; a flipped tag bit fails.
                ok = ok && crypto.Aes256GcmOpen(k, n, ad, ConstByteSpan(a, len + 16),
                                                ByteSpan(b, len)).ok()
                        && memcmp(b, pt, len) == 0;
                a[len] ^= 1;
                ok = ok && !crypto.Aes256GcmOpen(k, n, ad, ConstByteSpan(a, len + 16),
                                                 ByteSpan(b, len)).ok();
                a[len] ^= 1;
                fast_gcm_set_mode(FAST_GCM_OFF);
                ok = ok && crypto.Aes256GcmOpen(k, n, ad, ConstByteSpan(a, len + 16),
                                                ByteSpan(b, len)).ok();
                checked++;
                if (!ok) {
                    bad++;
                    printf("BENCH gcm_verify MISMATCH len=%u aad=%d gcm=%s\n",
                           (unsigned)len, aadn, gcm_name(mode));
                }
            }
        }
    }
    unsigned fast = 0, fb = 0;
    fast_gcm_counts(&fast, &fb);
    printf("BENCH gcm_verify checked=%d bad=%d fast_ops=%u idf_ops=%u\n", checked, bad, fast, fb);
    free(pt); free(a); free(b);
    fast_gcm_set_mode((fast_gcm_mode_t)CONFIG_HOMEHUB_FAST_GCM_DEFAULT_MODE);
}

// Where an AES-256-GCM seal's time goes: the CTR pass on the AES engine,
// GHASH on the CPU, and ChaCha20-Poly1305 in software for comparison.
static void bench_crypto_parts() {
    uint8_t *ps = ps_alloc(9000, 32), *in = int_alloc(9000, 32);
    uint8_t key[32];
    esp_fill_random(key, 32);
    const size_t len = FRAME_BYTES;
    const int iters = 100;
    for (int where = 0; where < 3; where++) {
        uint8_t *buf = where == 1 ? in : where == 2 ? ps + 8 : ps;
        esp_aes_context aes;
        esp_aes_init(&aes);
        esp_aes_setkey(&aes, key, 256);
        uint8_t ctr[16] = {0}, stream[16];
        size_t off = 0;
        int64_t t0 = now_us();
        for (int i = 0; i < iters; i++) esp_aes_crypt_ctr(&aes, len, &off, ctr, stream, buf, buf);
        int64_t dt = now_us() - t0;
        esp_aes_free(&aes);
        printf("BENCH crypto part=aes_ctr_hw buf=%s MBps=%.2f us_per_8k=%.0f\n",
               where == 1 ? "internal" : where == 2 ? "psram_off8_bounced" : "psram_al32",
               (double)iters * len / dt, (double)dt / iters);
    }
    {
        uint8_t h[16];
        esp_fill_random(h, 16);
        static ghash4_key_t g4;
        ghash8_key_t *g8 = static_cast<ghash8_key_t *>(
            heap_caps_malloc(sizeof(ghash8_key_t), MALLOC_CAP_INTERNAL));
        ghash4_init(&g4, h);
        if (g8) ghash8_init(g8, h);
        for (int bits : {4, 8}) {
            if (bits == 8 && !g8) continue;
            uint8_t y[16] = {0};
            int64_t t0 = now_us();
            for (int i = 0; i < iters; i++) {
                if (bits == 4) ghash4_update(&g4, y, ps, len);
                else ghash8_update(g8, y, ps, len);
            }
            int64_t dt = now_us() - t0;
            printf("BENCH crypto part=ghash%d MBps=%.2f us_per_8k=%.0f cycles_per_byte=%.0f\n",
                   bits, (double)iters * len / dt, (double)dt / iters,
                   (double)dt * 240.0 / ((double)iters * len));
        }
        free(g8);
    }
    {
        psa_key_attributes_t at = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&at, PSA_KEY_TYPE_CHACHA20);
        psa_set_key_bits(&at, 256);
        psa_set_key_usage_flags(&at, PSA_KEY_USAGE_ENCRYPT);
        psa_set_key_algorithm(&at, PSA_ALG_CHACHA20_POLY1305);
        psa_key_id_t kid = 0;
        uint8_t nonce[12] = {0};
        psa_status_t st = psa_crypto_init();
        if (st == PSA_SUCCESS) st = psa_import_key(&at, key, 32, &kid);
        uint8_t *out = ps_alloc(9100, 4);
        if (st == PSA_SUCCESS && out) {
            int64_t t0 = now_us();
            size_t olen = 0;
            for (int i = 0; i < iters && st == PSA_SUCCESS; i++) {
                st = psa_aead_encrypt(kid, PSA_ALG_CHACHA20_POLY1305, nonce, 12, nullptr, 0,
                                      ps, len, out, len + 16, &olen);
            }
            int64_t dt = now_us() - t0;
            printf("BENCH crypto part=chacha20_poly1305_sw ok=%d MBps=%.2f us_per_8k=%.0f\n",
                   st == PSA_SUCCESS, (double)iters * len / dt, (double)dt / iters);
        } else {
            printf("BENCH crypto part=chacha20_poly1305_sw error=%d\n", (int)st);
        }
        if (kid) psa_destroy_key(kid);
        free(out);
    }
    free(ps); free(in);
}

// ---- 3. network ------------------------------------------------------------------

static bool find_server(int timeout_s) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(CONFIG_HOMEHUB_BENCH_BEACON_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    bool ok = false;
    if (bind(s, (sockaddr *)&a, sizeof(a)) == 0) {
        for (int i = 0; i < timeout_s && !ok; i++) {
            char buf[64] = {0};
            sockaddr_in from = {};
            socklen_t fl = sizeof(from);
            int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
            if (n > 0 && sscanf(buf, "C5BENCH %d %d", &s_tcp_port, &s_tls_port) == 2) {
                inet_ntoa_r(from.sin_addr, s_host, sizeof(s_host));
                ok = true;
            }
        }
    }
    close(s);
    return ok;
}

static int tcp_open() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(s_tcp_port);
    inet_aton(s_host, &a.sin_addr);
    timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (sockaddr *)&a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static void bench_tcp(const char *tag) {
    static uint8_t buf[4096];
    esp_fill_random(buf, sizeof(buf));
    for (int dir = 0; dir < 2; dir++) {
        int s = tcp_open();
        if (s < 0) {
            printf("BENCH tcp tag=%s error=connect\n", tag);
            return;
        }
        const char *cmd = dir == 0 ? "TX\n" : "RX\n";
        send(s, cmd, 3, 0);
        cpu_meter cpu;
        cpu.start();
        int64_t t0 = now_us(), end = t0 + PHASE_S * 1000000LL;
        uint64_t bytes = 0;
        while (now_us() < end) {
            int n = dir == 0 ? send(s, buf, sizeof(buf), 0) : recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            bytes += n;
        }
        int64_t dt = now_us() - t0;
        printf("BENCH tcp tag=%s dir=%s Mbps=%.2f cpu=%d\n", tag, dir == 0 ? "tx" : "rx",
               bytes * 8.0 / dt, cpu.busy_pct());
        close(s);
    }
}

static esp_tls_t *tls_open() {
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return nullptr;
    esp_tls_cfg_t cfg = {};
    cfg.timeout_ms = 10000;
    cfg.skip_common_name = true;
    static const int chacha_first[] = {
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, 0};
    static const int aes_only[] = {
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, 0};
    cfg.ciphersuites_list = s_tls_chacha ? chacha_first : aes_only;
    if (esp_tls_conn_new_sync(s_host, strlen(s_host), s_tls_port, &cfg, tls) != 1) {
        esp_tls_conn_destroy(tls);
        return nullptr;
    }
    return tls;
}

static bool tls_write_all(esp_tls_t *tls, const uint8_t *p, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = esp_tls_conn_write(tls, p + off, len - off);
        if (n > 0) off += n;
        else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
        else return false;
    }
    return true;
}

// The pre-change WebSocket sender: header, then the payload masked into a
// 512-byte stack chunk per TLS write.
static bool ws_send_legacy(esp_tls_t *tls, const uint8_t *payload, size_t len) {
    uint8_t hdr[8] = {0x82, 0x80 | 126, (uint8_t)(len >> 8), (uint8_t)len};
    uint32_t mask = esp_random();
    memcpy(hdr + 4, &mask, 4);
    if (!tls_write_all(tls, hdr, 8)) return false;
    const uint8_t *mk = reinterpret_cast<const uint8_t *>(&mask);
    uint8_t chunk[512];
    for (size_t off = 0; off < len;) {
        size_t n = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        for (size_t i = 0; i < n; i++) chunk[i] = payload[off + i] ^ mk[(off + i) & 3];
        if (!tls_write_all(tls, chunk, n)) return false;
        off += n;
    }
    return true;
}

enum class path { ws_legacy, ws_inplace, full_legacy, full_new, full_new_int, tls16k };
static const char *pipe_name(path p) {
    switch (p) {
    case path::ws_legacy: return "ws_legacy";
    case path::ws_inplace: return "ws_inplace";
    case path::full_legacy: return "full_legacy";
    case path::full_new: return "full_new";
    case path::full_new_int: return "full_new_intbuf";
    case path::tls16k: return "tls_16k_writes";
    }
    return "?";
}

// Buffers shaped like the session's: tunnel packet slots, noise pool slot,
// ServiceFrame/envelope scratch, ws_buf.
struct pipe_bufs {
    uint8_t *pkts = nullptr;        // 6 x 1400 B tunnel_netif slots (PSRAM)
    uint8_t *staging = nullptr;     // old s_batch_buf (internal)
    uint8_t *slot = nullptr;        // noise_tunnel pool slot (PSRAM)
    uint8_t *svc = nullptr, *env = nullptr;   // OUT_SVC/OUT_ENV scratch (PSRAM)
    uint8_t *ws_old = nullptr;      // old ws_buf: heap_caps_malloc (PSRAM)
    uint8_t *ws_new = nullptr;      // new ws_buf: 32-aligned + headroom (PSRAM)
    uint8_t *ws_int = nullptr;      // ws_buf in internal RAM
    bool need_staging = false, need_int = false;
    bool ok() const {
        return pkts && slot && svc && env && ws_old && ws_new
            && (staging || !need_staging) && (ws_int || !need_int);
    }
    // Internal-RAM buffers only for the paths that use them, so the heap
    // figures reflect each path's real cost.
    void alloc(path p) {
        need_staging = p == path::full_legacy;
        need_int = p == path::full_new_int;
        pkts = ps_alloc(6 * 1400, 4);
        if (need_staging) staging = int_alloc(BATCH_BYTES, 4);
        slot = ps_alloc(BATCH_BYTES, 4);
        svc = ps_alloc(12288, 4);
        env = ps_alloc(12288, 4);
        ws_old = static_cast<uint8_t *>(heap_caps_malloc(WS_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ws_new = ps_alloc(WS_HEADROOM + WS_CAP, 32);
        if (need_int) ws_int = int_alloc(WS_HEADROOM + FRAME_BYTES + 64, 32);   // >= 7050 + tag
        if (pkts) esp_fill_random(pkts, 6 * 1400);
    }
    void release() {
        free(pkts); free(staging); free(slot); free(svc); free(env);
        free(ws_old); free(ws_new); free(ws_int);
    }
};

// One 8 KB batch through the chosen path; returns false on a send failure.
static bool pipe_once(path p, esp_tls_t *tls, Transport &t, pipe_bufs &b) {
    if (p == path::tls16k) {
        return tls_write_all(tls, b.svc, 12288);
    }
    if (p == path::ws_legacy || p == path::ws_inplace) {
        uint8_t *payload = b.ws_new + WS_HEADROOM;
        return p == path::ws_legacy ? ws_send_legacy(tls, payload, FRAME_BYTES)
                                    : noise_bench_ws_send_inplace(tls, payload, FRAME_BYTES);
    }
    // tunnel_netif: packets into a batch (old: via the internal staging buffer).
    uint8_t *batch = p == path::full_legacy ? b.staging : b.slot;
    size_t off = 0;
    for (int i = 0; i < 6 && off + 1402 <= BATCH_BYTES; i++) {
        batch[off++] = 0x78; batch[off++] = 0x05;
        memcpy(batch + off, b.pkts + i * 1400, 1400);
        off += 1400;
    }
    if (p == path::full_legacy) memcpy(b.slot, b.staging, off);   // noise_tunnel_send_packet
    // ServiceFrame encode, envelope encode, framer encode into ws_buf.
    memcpy(b.svc + 24, b.slot, off);
    memcpy(b.env + 16, b.svc, off + 24);
    uint8_t *payload = p == path::full_legacy ? b.ws_old
                     : p == path::full_new_int ? b.ws_int + WS_HEADROOM
                     : b.ws_new + WS_HEADROOM;
    size_t len = off + FRAME_OVERHEAD;
    memcpy(payload, b.env, len);
    if (!t.SealInPlace(ByteSpan(payload, len + Transport::kTagSize), len).ok()) return false;
    len += Transport::kTagSize;
    return p == path::full_legacy ? ws_send_legacy(tls, payload, len)
                                  : noise_bench_ws_send_inplace(tls, payload, len);
}

static std::atomic<bool> s_pipe_running{false};
static bool s_last_pipe_ok;

static double bench_pipe(path p, const char *tag, bool quiet = false) {
    s_last_pipe_ok = false;
    esp_tls_t *tls = tls_open();
    if (!tls) {
        printf("BENCH pipe tag=%s case=%s error=tls_connect\n", tag, pipe_name(p));
        return -1;
    }
    tls_write_all(tls, (const uint8_t *)"TX\n", 3);
    const char *suite = "?";
    {
        mbedtls_ssl_context *ssl = static_cast<mbedtls_ssl_context *>(esp_tls_get_ssl_context(tls));
        if (ssl) suite = mbedtls_ssl_get_ciphersuite(ssl);
    }
    PsaCryptoBackend crypto;
    uint8_t k1[32], k2[32];
    esp_fill_random(k1, 32);
    esp_fill_random(k2, 32);
    Transport t(crypto, ConstByteSpan(k1, 32), ConstByteSpan(k2, 32));
    pipe_bufs b;
    b.alloc(p);
    double mbps = -1;
    if (b.ok()) {
        heap_caps_monitor_local_minimum_free_size_start();
        cpu_meter cpu;
        cpu.start();
        int64_t t0 = now_us(), end = t0 + PHASE_S * 1000000LL;
        uint64_t batches = 0;
        bool ok = true;
        s_pipe_running = true;
        while (now_us() < end && (ok = pipe_once(p, tls, t, b))) batches++;
        s_pipe_running = false;
        s_last_pipe_ok = ok;
        int64_t dt = now_us() - t0;
        // tunnel_netif batches five 1400 B packets (it stops once a sixth
        // could overflow 8 KB): 7010 B of IP data per batch.
        size_t per = p == path::tls16k ? 12288 : 5 * 1402;
        mbps = batches * per * 8.0 / dt;
        size_t dma_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
        heap_caps_monitor_local_minimum_free_size_stop();
        if (!quiet) {
            printf("BENCH pipe tag=%s case=%s gcm=%s tls=%s ok=%d goodput_Mbps=%.2f cpu=%d us_per_batch=%.0f "
                   "ws_old_mod32=%u dma_min=%u\n",
                   tag, pipe_name(p), gcm_name(fast_gcm_get_mode()), suite, ok ? 1 : 0, mbps, cpu.busy_pct(),
                   batches ? (double)dt / batches : 0.0,
                   (unsigned)((uintptr_t)b.ws_old & 31), (unsigned)dma_min);
        }
    } else {
        printf("BENCH pipe tag=%s case=%s error=alloc\n", tag, pipe_name(p));
    }
    b.release();
    esp_tls_conn_destroy(tls);
    return mbps;
}

static double bench_tls_rx(const char *tag) {
    esp_tls_t *tls = tls_open();
    if (!tls) {
        printf("BENCH tls_rx tag=%s error=tls_connect\n", tag);
        return -1;
    }
    tls_write_all(tls, (const uint8_t *)"RX\n", 3);
    mbedtls_ssl_context *ssl = static_cast<mbedtls_ssl_context *>(esp_tls_get_ssl_context(tls));
    const char *suite = ssl ? mbedtls_ssl_get_ciphersuite(ssl) : "?";
    uint8_t *rx = ps_alloc(16 * 1024, 32);
    cpu_meter cpu;
    cpu.start();
    int64_t t0 = now_us(), end = t0 + PHASE_S * 1000000LL;
    uint64_t bytes = 0;
    while (rx && now_us() < end) {
        ssize_t n = esp_tls_conn_read(tls, rx, 16 * 1024);
        if (n > 0) bytes += n;
        else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        else break;
    }
    int64_t dt = now_us() - t0;
    printf("BENCH tls_rx tag=%s gcm=%s tls=%s Mbps=%.2f cpu=%d\n", tag,
           gcm_name(fast_gcm_get_mode()), suite, bytes * 8.0 / dt, cpu.busy_pct());
    free(rx);
    esp_tls_conn_destroy(tls);
    return bytes * 8.0 / dt;
}

// Emulate a WAN round trip on a LAN: hold every inbound packet on the STA
// netif for CONFIG_HOMEHUB_BENCH_RX_DELAY_MS before lwIP sees it, so ACKs
// (for TX) and data (for RX) arrive that much later. Packets are copied out of
// the Wi-Fi RX buffers (PSRAM when lwIP prefers it) so the delay line doesn't
// hold the driver's internal buffers.
#if CONFIG_HOMEHUB_BENCH_RX_DELAY_MS > 0
struct delayed_pkt {
    struct pbuf *p;
    struct netif *inp;
    int64_t due_us;
};
static QueueHandle_t s_delay_q;
static netif_input_fn s_orig_input;
static std::atomic<uint32_t> s_delay_drops{0};

static err_t delay_input(struct pbuf *p, struct netif *inp) {
    struct pbuf *q = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
    pbuf_free(p);
    if (!q) {
        s_delay_drops++;
        return ERR_OK;
    }
    delayed_pkt d = {q, inp, now_us() + CONFIG_HOMEHUB_BENCH_RX_DELAY_MS * 1000LL};
    if (xQueueSend(s_delay_q, &d, 0) != pdTRUE) {
        pbuf_free(q);
        s_delay_drops++;
    }
    return ERR_OK;
}

static void delay_task(void *) {
    delayed_pkt d;
    for (;;) {
        if (xQueueReceive(s_delay_q, &d, portMAX_DELAY) != pdTRUE) continue;
        int64_t wait = d.due_us - now_us();
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS((wait + 999) / 1000));
        if (s_orig_input(d.p, d.inp) != ERR_OK) pbuf_free(d.p);
    }
}

static void install_rx_delay() {
    struct netif *n = static_cast<struct netif *>(
        esp_netif_get_netif_impl(wifi_mgr_get_netif()));
    if (!n) return;
    s_delay_q = xQueueCreate(512, sizeof(delayed_pkt));
    xTaskCreate(delay_task, "rx_delay", 3072, nullptr, 19, nullptr);
    LOCK_TCPIP_CORE();
    s_orig_input = n->input;
    n->input = delay_input;
    UNLOCK_TCPIP_CORE();
    printf("BENCH rx_delay ms=%d (emulated RTT added)\n", CONFIG_HOMEHUB_BENCH_RX_DELAY_MS);
}
#endif

// ---- 4. BLE --------------------------------------------------------------------

static void ble_start() {
    heap_line("before_ble");
    ble_basic_register();
    ble_server_start(identity_ble_name(), nullptr);
    for (int i = 0; i < 50 && !ble_basic_host_synced(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    ble_server_set_companion_advertising(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    heap_line("after_ble");
    printf("BENCH ble_name name=%s\n", identity_ble_name());
}

struct scan_job {
    uint32_t ms;
    int found;
    uint32_t reports;
    TaskHandle_t waiter;
};
static void scan_task(void *arg) {
    auto *j = static_cast<scan_job *>(arg);
    static ble_central_dev_t devs[48];
    j->found = ble_central_scan(j->ms, false, 100, 50, devs, 48, &j->reports);
    xTaskNotifyGive(j->waiter);
    vTaskDelete(nullptr);
}

static double bench_ble_scan(const char *tag, bool with_wifi) {
    scan_job j = {(uint32_t)PHASE_S * 1000, 0, 0, xTaskGetCurrentTaskHandle()};
    xTaskCreate(scan_task, "bscan", 4096, &j, 5, nullptr);
    double mbps = with_wifi ? bench_pipe(path::full_new, tag, true) : 0;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PHASE_S * 1000 + 5000));
    printf("BENCH ble_scan tag=%s duty=50pct devices=%d reports=%lu reports_per_s=%.1f wifi_Mbps=%.2f\n",
           tag, j.found, (unsigned long)j.reports, (double)j.reports / PHASE_S, mbps);
    return (j.found < 0 || (with_wifi && !s_last_pipe_ok)) ? -1 : mbps;
}

struct probe_job {
    ble_central_dev_t dev;
    ble_central_session_t s;
    bool ok;
    TaskHandle_t waiter;
};
static void probe_task(void *arg) {
    auto *j = static_cast<probe_job *>(arg);
    // Start once the Wi-Fi stream is running, then probe repeatedly.
    for (int i = 0; i < 200 && !s_pipe_running; i++) vTaskDelay(pdMS_TO_TICKS(50));
    j->ok = ble_central_probe(&j->dev, 8000, false, &j->s);
    xTaskNotifyGive(j->waiter);
    vTaskDelete(nullptr);
}

static bool bench_ble_central(const char *tag, bool with_wifi) {
    probe_job j = {};
    j.waiter = xTaskGetCurrentTaskHandle();
    int64_t f0 = now_us();
    if (!ble_central_find(CONFIG_HOMEHUB_BENCH_BLE_PEER, 15000, &j.dev)) {
        printf("BENCH ble_central tag=%s peer=%s error=not_found\n", tag, CONFIG_HOMEHUB_BENCH_BLE_PEER);
        return false;
    }
    uint32_t find_ms = (uint32_t)((now_us() - f0) / 1000);
    double mbps = 0;
    if (with_wifi) {
        xTaskCreate(probe_task, "bprobe", 4096, &j, 5, nullptr);
        mbps = bench_pipe(path::full_new, tag, true);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    } else {
        j.ok = ble_central_probe(&j.dev, 8000, false, &j.s);
    }
    printf("BENCH ble_central tag=%s peer=%s ok=%d rssi=%d find_ms=%lu connect_ms=%lu mtu=%u "
           "mtu_ms=%lu services=%d discover_ms=%lu gap_name=%s wifi_Mbps=%.2f\n",
           tag, CONFIG_HOMEHUB_BENCH_BLE_PEER, j.ok ? 1 : 0, j.dev.rssi, (unsigned long)find_ms,
           (unsigned long)j.s.connect_ms, j.s.mtu, (unsigned long)j.s.mtu_ms, j.s.services,
           (unsigned long)j.s.discover_ms, j.s.gap_name[0] ? j.s.gap_name : "-", mbps);
    return j.ok;
}

static void bench_ble_peripheral_with_wifi() {
    printf("BENCH wait_ble_client name=%s timeout_s=120\n", identity_ble_name());
    for (int i = 0; i < 1200 && !ble_basic_client_connected(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!ble_basic_client_connected()) {
        printf("BENCH ble_periph error=no_client\n");
        return;
    }
    // The host script streams for a fixed time once connected; overlap it.
    vTaskDelay(pdMS_TO_TICKS(2000));
    double mbps = bench_pipe(path::full_new, "ble_client_streaming", true);
    printf("BENCH ble_periph wifi_Mbps_during_ble_stream=%.2f\n", mbps);
}

// ---- entry -----------------------------------------------------------------------

void pipeline_bench_main(void) {
    esp_log_level_set("wifi", ESP_LOG_WARN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("BENCH start phase_s=%d\n", PHASE_S);
    config_store_init();
    identity_init();
    heap_line("boot");
#if !CONFIG_HOMEHUB_BENCH_QUICK
    bench_memcpy();
    bench_crypto_parts();
    bench_aead();
#endif
    bench_gcm_verify();

    ble_start();
    bench_ble_scan("idle", false);
    bench_ble_central("idle", false);

    const char *ssid = CONFIG_HOMEHUB_WIFI_SSID;
    if (!ssid[0]) {
        printf("BENCH wifi skipped=no_ssid\n");
        printf("BENCH done\n");
        return;
    }
    wifi_mgr_init();
#if CONFIG_HOMEHUB_BENCH_BAND == 1
    esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
#elif CONFIG_HOMEHUB_BENCH_BAND == 2
    esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
#endif
    heap_line("after_wifi_init");
    if (!wifi_mgr_connect(ssid, CONFIG_HOMEHUB_WIFI_PASSWORD, 30000)) {
        printf("BENCH wifi error=connect\n");
        printf("BENCH done\n");
        return;
    }
    uint8_t ch = 0;
    wifi_mgr_get_connected_channel(&ch);
    wifi_ap_record_t ap = {};
    esp_wifi_sta_get_ap_info(&ap);
    printf("BENCH wifi connected=1 channel=%u rssi=%d\n", ch, ap.rssi);
    if (!find_server(60)) {
        printf("BENCH server error=no_beacon\n");
        printf("BENCH done\n");
        return;
    }
    printf("BENCH server host=%s tcp=%d tls=%d\n", s_host, s_tcp_port, s_tls_port);
#if CONFIG_HOMEHUB_BENCH_RX_DELAY_MS > 0
    install_rx_delay();
#endif
    heap_line("wifi_up");

#if CONFIG_HOMEHUB_BENCH_SOAK_MINUTES > 0
    // Soak: tunnel TX with BLE scanning, TLS RX, and a BLE central connect
    // every fourth cycle, until the time is up. Reports failures and the
    // lowest free internal and DMA-capable RAM seen.
    {
        int64_t soak_end = now_us() + CONFIG_HOMEHUB_BENCH_SOAK_MINUTES * 60LL * 1000000LL;
        int cycles = 0, tx_fail = 0, rx_fail = 0, central_fail = 0, central_runs = 0;
        double tx_sum = 0, rx_sum = 0, tx_min = 1e9, rx_min = 1e9;
        printf("BENCH soak start minutes=%d\n", CONFIG_HOMEHUB_BENCH_SOAK_MINUTES);
        while (now_us() < soak_end) {
            double tx = bench_ble_scan("soak", true);
            double rx = bench_tls_rx("soak");
            if (tx < 0) tx_fail++; else { tx_sum += tx; if (tx < tx_min) tx_min = tx; }
            if (rx < 0) rx_fail++; else { rx_sum += rx; if (rx < rx_min) rx_min = rx; }
            if (cycles % 4 == 3) {
                central_runs++;
                if (!bench_ble_central("soak", true)) central_fail++;
            }
            cycles++;
            char label[24];
            snprintf(label, sizeof(label), "soak_%d", cycles);
            heap_line(label);
        }
        int tx_ok = cycles - tx_fail, rx_ok = cycles - rx_fail;
        printf("BENCH soak_summary cycles=%d tx_fail=%d rx_fail=%d central=%d/%d "
               "tx_avg=%.2f tx_min=%.2f rx_avg=%.2f rx_min=%.2f int_min=%u dma_min=%u\n",
               cycles, tx_fail, rx_fail, central_runs - central_fail, central_runs,
               tx_ok ? tx_sum / tx_ok : 0.0, tx_ok ? tx_min : 0.0,
               rx_ok ? rx_sum / rx_ok : 0.0, rx_ok ? rx_min : 0.0,
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
        printf("BENCH done\n");
        return;
    }
#endif
    bench_tcp("ble_adv");
#if CONFIG_HOMEHUB_BENCH_QUICK
    // Variant builds (TCP buffers, buffer placement): before vs after only.
    fast_gcm_set_mode(FAST_GCM_OFF);
    bench_pipe(path::full_legacy, "ble_adv");
    bench_tls_rx("ble_adv");
    fast_gcm_set_mode(FAST_GCM_G8);
    for (path p : {path::tls16k, path::full_new, path::full_new_int}) bench_pipe(p, "ble_adv");
    bench_tls_rx("ble_adv");
#else
    // Before: IDF GCM, legacy framing. Then each change on its own and together.
    fast_gcm_set_mode(FAST_GCM_OFF);
    s_tls_chacha = false;
    for (path p : {path::tls16k, path::ws_legacy, path::ws_inplace,
                   path::full_legacy, path::full_new, path::full_new_int}) {
        bench_pipe(p, "ble_adv");
    }
    bench_tls_rx("ble_adv");
    fast_gcm_set_mode(FAST_GCM_G8);
    for (path p : {path::tls16k, path::ws_inplace, path::full_legacy, path::full_new,
                   path::full_new_int}) {
        bench_pipe(p, "ble_adv");
    }
    bench_tls_rx("ble_adv");
    s_tls_chacha = true;
    for (path p : {path::tls16k, path::full_new}) bench_pipe(p, "ble_adv");
    bench_tls_rx("ble_adv");
    s_tls_chacha = false;
#endif
    heap_line("after_pipes");

    bench_ble_scan("wifi", true);
    bench_ble_central("wifi", true);
    bench_ble_peripheral_with_wifi();
    heap_line("end");
#if CONFIG_HOMEHUB_BENCH_RX_DELAY_MS > 0
    printf("BENCH rx_delay drops=%lu\n", (unsigned long)s_delay_drops.load());
#endif
    printf("BENCH done\n");
}

#endif  // CONFIG_HOMEHUB_PIPELINE_BENCH
