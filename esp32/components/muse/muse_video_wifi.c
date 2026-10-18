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
#include "muse_video_wifi.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"

#include "muse_ble.h"
#include "muse_mem.h"
#include "muse_video.h"
#include "muse_video_wire.h"
#include "muse_link.h"
#include "muse_wifi.h"

static const char *TAG = "muse_video_wifi";

/*
 * What the gadget suggests over Wi-Fi, which carries several times what the
 * decoder can use: the bitrate is set by the decoder and the picture, not the
 * link (protocols/VIDEO.md, Sizes and bitrates). On the S3 at 232 px, 1000
 * kb/s decodes in about 25 ms a frame: 1 dB over 600, still 30 fps; at 1500
 * the decoder falls behind. The S31's 480 px bands decoded 750 kb/s in 24 ms.
 */
#define WIFI_SUG_KBPS 1000
#define AU_MAX (64 * 1024)
#define LISTEN_BACKLOG 2
#define SCRATCH 1460                   /* bytes of an unwanted access unit taken at a time */
/* The port is unauthenticated (protocols/VIDEO.md, Trust): a connection must
 * send a whole message within FIRST_MSG_MS, and while the client has sent
 * something within ACTIVE_MS a new connection is turned away rather than
 * taking its place. */
#define FIRST_MSG_MS 5000
#define ACTIVE_MS 10000

typedef enum {
    RX_TYPE,       /* the next message's first byte */
    RX_JSON_LEN,   /* a control message's two length bytes */
    RX_JSON,       /* its text */
    RX_VHDR,       /* a frame's header */
    RX_VHDR_PUT,   /* waiting for room for the header in the ring */
    RX_VBODY,      /* its access unit, into the ring */
    RX_SKIP,       /* an access unit nobody wants (no stream, or too big) */
} rx_state_t;

static muse_vstream_t s_ring;
static muse_video_caps_t s_caps;
static bool s_have_caps;
static SemaphoreHandle_t s_tx_lock;
static volatile int s_sock = -1;        /* the client's connection */
static int64_t s_conn_us, s_last_rx_us; /* when it connected, and last sent a whole message */
static volatile bool s_tx_failed;       /* a send gave up: the client isn't reading */
static volatile bool s_want;            /* the view asks for a stream */
static volatile bool s_streaming;   /* ended with take_stream() */
static char s_name[40];

/* While a stream is on: Wi-Fi out of modem sleep, and no BLE advertising
 * (muse_link_ble_quiet says why). */
static void radio_for_video(bool on)
{
    muse_wifi_keep_awake(on);
    muse_link_ble_quiet(on);
}

/* ---- sending (any task) ------------------------------------------------- */

static void tx(const uint8_t *m, size_t n)
{
    if (!n) {
        return;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int sock = s_sock;
    size_t off = 0;
    while (sock >= 0 && off < n) {
        int k = send(sock, m + off, n - off, 0);
        if (k <= 0) {
            /* Gone, or not reading (SO_SNDTIMEO): the server task drops it,
             * so a stalled client can't hold up the decoder's credits. */
            s_tx_failed = true;
            break;
        }
        off += (size_t)k;
    }
    xSemaphoreGive(s_tx_lock);
}

static void tx_buffer(const muse_video_status_t *st)
{
    uint8_t m[200];
    muse_video_status_t none = { .last_age_ms = 0xffff };
    if (!st) {
        st = &none;
    }
    tx(m, muse_video_wire_buffer(m, sizeof(m), muse_vstream_available(&s_ring), muse_vstream_received(&s_ring),
                                 st->shown, st->dropped, st->decode_ms, st->last_number, st->last_age_ms));
}

/* ---- the transport (muse_video's side) ----------------------------------- */

static void on_freed(const muse_video_status_t *st)
{
    if (s_streaming) {
        tx_buffer(st);
    }
}

static void on_keyframe(void)
{
    uint8_t m[32];
    tx(m, muse_video_wire_simple(m, sizeof(m), "keyframe"));
}

/* The stream ends once, whichever side ends it first. */
static bool take_stream(void)
{
    return __atomic_exchange_n(&s_streaming, false, __ATOMIC_SEQ_CST);
}

static void on_stop(void)
{
    if (!take_stream()) {
        return;
    }
    muse_vstream_stop(&s_ring);
    radio_for_video(false);
    uint8_t m[32];
    tx(m, muse_video_wire_simple(m, sizeof(m), "stop"));
}

static void on_request(bool want)
{
    s_want = want;
    uint8_t m[40];
    tx(m, muse_video_wire_want(m, sizeof(m), want));
}

static bool has_client(void)
{
    return s_sock >= 0;
}

static const muse_video_transport_t s_transport = {
    .name = "wifi",
    .ring = &s_ring,
    .freed = on_freed,
    .keyframe = on_keyframe,
    .stop = on_stop,
    .request = on_request,
    .has_client = has_client,
};

/* ---- the connection (server task) ---------------------------------------- */

static void end_stream(void)
{
    if (take_stream()) {
        muse_vstream_stop(&s_ring);
        muse_video_end(&s_transport);
        radio_for_video(false);
    }
}

static void close_client(const char *why)
{
    end_stream();
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int sock = s_sock;
    s_sock = -1;
    xSemaphoreGive(s_tx_lock);
    if (sock >= 0) {
        ESP_LOGI(TAG, "client gone (%s)", why);
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
}

static void on_start(const muse_video_params_t *p)
{
    uint8_t m[96];
    end_stream();   /* a new start replaces the stream that's on */
#if !CONFIG_MUSE_VIDEO_WIFI_START_ANYTIME
    /* Only while someone has the Phone camera view open on the board: the
     * client waits for "want" (VIDEO.md, Trust). */
    if (!s_want && !muse_video_is_open()) {
        tx(m, muse_video_wire_error(m, sizeof(m), "start", "closed"));
        return;
    }
#endif
    if (!s_have_caps) {
        tx(m, muse_video_wire_error(m, sizeof(m), "start", "unsupported"));
        return;
    }
    muse_video_start_check_t c = muse_video_wire_check(p, &s_caps);
    if (c != MUSE_VIDEO_START_OK) {
        ESP_LOGW(TAG, "start refused: %ux%u @%u, %u bands, playout %u ms (%s)", p->width, p->height, p->fps,
                 p->tiles, p->playout_ms, c == MUSE_VIDEO_START_UNSUPPORTED ? "codec" : "out of range");
        tx(m, muse_video_wire_error(m, sizeof(m), "start", c == MUSE_VIDEO_START_UNSUPPORTED ? "unsupported" : "invalid"));
        return;
    }
    muse_vstream_start(&s_ring);
    if (!muse_video_begin(&s_transport, p)) {
        muse_vstream_stop(&s_ring);
        tx(m, muse_video_wire_error(m, sizeof(m), "start", "busy"));
        return;
    }
    ESP_LOGI(TAG, "video stream %ux%u %u fps %u kb/s%s", p->width, p->height, p->fps, p->kbps,
             p->tiles == 2 ? ", two bands" : "");
    s_streaming = true;
    radio_for_video(true);
    tx_buffer(NULL);   /* the credits to start with */
}

static void on_json(const char *json, size_t n)
{
    muse_video_params_t p;
    switch (muse_video_wire_parse(json, n, &p)) {
    case MUSE_VIDEO_MSG_START:
        on_start(&p);
        break;
    case MUSE_VIDEO_MSG_STOP:
        if (s_streaming) {
            ESP_LOGI(TAG, "video stream ended by the client: %u bytes, %u resyncs", (unsigned)s_ring.received,
                     (unsigned)s_ring.resyncs);
        }
        end_stream();
        break;
    case MUSE_VIDEO_MSG_STATUS:
        tx_buffer(NULL);
        break;
    case MUSE_VIDEO_MSG_BAD:
        ESP_LOGW(TAG, "bad control message (%u bytes)", (unsigned)n);
        break;
    default:
        break;   /* a newer client's: ignored */
    }
}

static void greet(void)
{
    uint8_t m[400];
    tx(m, muse_video_wire_hello(m, sizeof(m), s_name, &s_caps, s_ring.size));
    if (s_want) {
        tx(m, muse_video_wire_want(m, sizeof(m), true));   /* the view was already open */
    }
}

static void accept_client(int lsock)
{
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    int sock = accept(lsock, (struct sockaddr *)&from, &len);
    if (sock < 0) {
        return;
    }
    char ip[16];
    inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
    if (s_sock >= 0) {
        /* A client that's talking keeps its place (one that reconnected
         * after its link dropped has gone quiet on the old connection). */
        if ((esp_timer_get_time() - s_last_rx_us) / 1000 < ACTIVE_MS) {
            ESP_LOGW(TAG, "connection from %s turned away: a client is connected", ip);
            close(sock);
            return;
        }
        close_client("replaced by a new connection");
    }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 5, intvl = 2, cnt = 3;   /* a phone that vanished: gone in about 11 s */
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_sock = sock;
    s_tx_failed = false;
    s_conn_us = esp_timer_get_time();
    s_last_rx_us = 0;
    xSemaphoreGive(s_tx_lock);
    ESP_LOGI(TAG, "client connected from %s", ip);
    greet();
}

/* Waits until the station has an address. */
static void wait_for_ip(void)
{
    for (;;) {
        esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void start_mdns(void)
{
    char host[40];
    size_t k = 0;
    for (const char *c = s_name; *c && k < sizeof(host) - 1; c++) {
        host[k++] = (char)tolower((unsigned char)*c);
    }
    host[k] = 0;
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(host);
        mdns_instance_name_set(s_name);
        char w[8], h[8];
        snprintf(w, sizeof(w), "%u", s_caps.display_w);
        snprintf(h, sizeof(h), "%u", s_caps.display_h);
        mdns_txt_item_t txt[] = {
            { "version", "1" }, { "width", w }, { "height", h }, { "round", s_caps.round ? "1" : "0" },
        };
        err = mdns_service_add(s_name, MUSE_VIDEO_WIRE_SERVICE, MUSE_VIDEO_WIRE_PROTO, CONFIG_MUSE_VIDEO_WIFI_PORT, txt,
                               sizeof(txt) / sizeof(txt[0]));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: %s (the phone can still connect by address)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "announced as %s.local, %s.%s.local port %d", host, MUSE_VIDEO_WIRE_SERVICE, MUSE_VIDEO_WIRE_PROTO,
                 CONFIG_MUSE_VIDEO_WIFI_PORT);
    }
}

static int listen_socket(void)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_MUSE_VIDEO_WIFI_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "can't listen on port %d: errno %d", CONFIG_MUSE_VIDEO_WIFI_PORT, errno);
        close(s);
        return -1;
    }
    return s;
}

static void name_gadget(void)
{
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (b.name[0]) {
        strlcpy(s_name, b.name, sizeof(s_name));
        return;
    }
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_name, sizeof(s_name), "MuseGadget-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void server_task(void *arg)
{
    (void)arg;
    wait_for_ip();
    name_gadget();
    start_mdns();
    int lsock = -1;
    while ((lsock = listen_socket()) < 0) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI(TAG, "video over Wi-Fi on port %d", CONFIG_MUSE_VIDEO_WIFI_PORT);

    rx_state_t st = RX_TYPE;
    /* In PSRAM, like the stack: internal RAM is what the radios need. */
    char *json = heap_caps_malloc(MUSE_VIDEO_WIRE_MAX_JSON + 1, MUSE_BIG_CAPS);
    uint8_t *scratch = heap_caps_malloc(SCRATCH, MUSE_BIG_CAPS);
    if (!json || !scratch) {
        ESP_LOGE(TAG, "no memory for the receive buffers");
        free(json);
        free(scratch);
        vTaskDelete(NULL);
        return;
    }
    uint8_t hdr[MUSE_VIDEO_FRAME_HEADER_LEN];
    size_t have = 0, need = 0;
    uint32_t left = 0;
    int64_t told_full = 0;
    for (;;) {
        int sock = s_sock;
        if (sock >= 0 && s_tx_failed) {
            close_client("not reading");
            continue;
        }
        if (sock >= 0 && !s_last_rx_us && (esp_timer_get_time() - s_conn_us) / 1000 > FIRST_MSG_MS) {
            close_client("said nothing");
            continue;
        }
        if (sock < 0) {
            st = RX_TYPE;
            have = 0;
        }
        /* With the ring full mid-frame, leave the socket alone (TCP holds
         * the client back) and look again shortly. */
        bool waiting = sock >= 0 && s_streaming &&
                       ((st == RX_VBODY && muse_vstream_available(&s_ring) == 0) ||
                        (st == RX_VHDR_PUT && muse_vstream_available(&s_ring) < MUSE_VIDEO_FRAME_HEADER_LEN));
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lsock, &rd);
        int maxfd = lsock;
        if (sock >= 0 && !waiting) {
            FD_SET(sock, &rd);
            maxfd = sock > maxfd ? sock : maxfd;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = waiting ? 2000 : 500000 };
        int r = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (FD_ISSET(lsock, &rd)) {
            accept_client(lsock);
            st = RX_TYPE;
            continue;
        }
        if (st == RX_VHDR_PUT && s_streaming && muse_vstream_available(&s_ring) >= MUSE_VIDEO_FRAME_HEADER_LEN) {
            muse_vstream_write(&s_ring, hdr, sizeof(hdr));
            st = left ? RX_VBODY : RX_TYPE;
            continue;
        }
        if (sock < 0 || !FD_ISSET(sock, &rd)) {
            int64_t now = esp_timer_get_time();
            if (waiting && now - told_full > 5000000) {
                told_full = now;
                ESP_LOGD(TAG, "ring full: holding the client back");
            }
            continue;
        }
        uint8_t *dst = scratch;
        size_t want = 0;
        switch (st) {
        case RX_TYPE:
            dst = hdr;
            want = 1;
            break;
        case RX_JSON_LEN:
            dst = hdr + have;
            want = 2 - have;
            break;
        case RX_JSON:
            dst = (uint8_t *)json + have;
            want = need - have;
            break;
        case RX_VHDR:
            dst = hdr + have;
            want = sizeof(hdr) - have;
            break;
        case RX_VBODY: {
            uint32_t room = s_streaming ? muse_vstream_space(&s_ring, &dst) : 0;
            if (!s_streaming) {
                st = RX_SKIP;   /* the stream ended mid-frame */
                continue;
            }
            want = room < left ? room : left;
            if (!want) {
                continue;   /* the ring filled meanwhile: wait for room */
            }
            break;
        }
        case RX_SKIP:
        default:
            dst = scratch;
            want = left < SCRATCH ? left : SCRATCH;
            break;
        }
        int n = recv(sock, dst, want, 0);
        if (n <= 0) {
            close_client(n == 0 ? "closed" : "error");
            continue;
        }
        switch (st) {
        case RX_TYPE:
            if (hdr[0] == MUSE_VIDEO_WIRE_JSON) {
                st = RX_JSON_LEN;
                have = 0;
            } else if (hdr[0] == MUSE_VIDEO_FRAME_MAGIC) {
                st = RX_VHDR;
                have = 1;
            } else {
                close_client("not the video protocol");
            }
            break;
        case RX_JSON_LEN:
            have += (size_t)n;
            if (have == 2) {
                need = hdr[0] | hdr[1] << 8;
                if (need > MUSE_VIDEO_WIRE_MAX_JSON) {
                    close_client("control message too long");
                    break;
                }
                have = 0;
                st = need ? RX_JSON : RX_TYPE;
            }
            break;
        case RX_JSON:
            have += (size_t)n;
            if (have == need) {
                s_last_rx_us = esp_timer_get_time();
                on_json(json, need);
                st = RX_TYPE;
            }
            break;
        case RX_VHDR:
            have += (size_t)n;
            if (have == sizeof(hdr)) {
                left = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
                s_last_rx_us = esp_timer_get_time();
                if (left > AU_MAX) {
                    close_client("frame too long");   /* no client sends these */
                } else if (!s_streaming) {
                    st = left ? RX_SKIP : RX_TYPE;
                } else if (left > s_ring.max_frame) {
                    /* It would never fit: drop it and start again from a keyframe. */
                    ESP_LOGW(TAG, "frame of %u bytes is over the %u limit: dropped", (unsigned)left,
                             (unsigned)s_ring.max_frame);
                    on_keyframe();
                    st = RX_SKIP;
                } else {
                    st = RX_VHDR_PUT;
                }
            }
            break;
        case RX_VBODY:
            muse_vstream_commit(&s_ring, (uint32_t)n);
            left -= (uint32_t)n;
            if (!left) {
                st = RX_TYPE;
                muse_video_wake();
            }
            break;
        case RX_SKIP:
        default:
            left -= (uint32_t)n;
            if (!left) {
                st = RX_TYPE;
            }
            break;
        }
    }
}

void muse_video_wifi_start(void)
{
    const muse_video_caps_t *c = muse_video_caps();
    if (!c) {
        ESP_LOGI(TAG, "this panel can't show video: no Wi-Fi video");
        return;
    }
    s_caps = *c;
    s_caps.sug_kbps = WIFI_SUG_KBPS;
    s_have_caps = true;
    uint32_t size = muse_video_buffer_size();
    uint8_t *buf = heap_caps_malloc(size, MUSE_BIG_CAPS);
    s_tx_lock = xSemaphoreCreateMutex();
    if (!buf || !s_tx_lock) {
        ESP_LOGE(TAG, "no memory for the video ring");
        free(buf);
        return;
    }
    muse_vstream_init(&s_ring, buf, size, size / 2 < AU_MAX ? size / 2 : AU_MAX);
    muse_video_add_transport(&s_transport);
    /* Sockets and mDNS only: the stack can live in PSRAM. */
    if (xTaskCreatePinnedToCoreWithCaps(server_task, "video_wifi", 4096, NULL, 5, NULL, 0, MUSE_BIG_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the server task");
    }
}
