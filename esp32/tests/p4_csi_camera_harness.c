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

/* boards/p4_csi_camera.c for test_p4_csi_camera.py, against a scripted
 * esp_video device and JPEG encoder: every capture, failed or not, leaves
 * esp_video torn down, and the quality steps down until the JPEG fits. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "p4_csi_camera_fakes.h"
#include "p4_csi_camera.h"

#define W 640
#define H 480
#define FD 7

static bool sensor, frames = true, video_live;
static int video_inits, video_deinits, opens, closes, encoders_live, jpeg_bufs;
static uint32_t jpeg_len_at_quality_ten;   /* scripted size: this x quality/10 */
static uint32_t jpeg_len_fixed;            /* or this, whatever the quality */
static uint32_t last_quality;
static int64_t now_us;
static uint8_t buffers[2][W * H * 2];

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "err";
}

int64_t esp_timer_get_time(void)
{
    return now_us;
}

esp_err_t esp_video_init(const esp_video_init_config_t *config)
{
    assert(config && config->csi);
    assert(!video_live);   /* never set up over a live one */
    video_live = true;
    video_inits++;
    return ESP_OK;
}

esp_err_t esp_video_deinit(void)
{
    assert(video_live);
    video_live = false;
    video_deinits++;
    return ESP_OK;
}

int fake_open(const char *path, int flags)
{
    assert(video_live && strcmp(path, "/dev/video0") == 0 && flags == O_RDONLY);
    if (!sensor) return -1;   /* esp_video_init found no sensor */
    opens++;
    return FD;
}

int fake_close(int fd)
{
    assert(fd == FD);
    closes++;
    return 0;
}

int fake_ioctl(int fd, unsigned long request, void *arg)
{
    assert(fd == FD && video_live);
    struct v4l2_buffer *b = arg;
    switch (request) {
    case VIDIOC_G_FMT: {
        struct v4l2_format *f = arg;
        f->fmt.pix.width = W;
        f->fmt.pix.height = H;
        return 0;
    }
    case VIDIOC_QUERYBUF:
        assert(b->index < 2);
        b->length = sizeof(buffers[0]);
        b->m.offset = b->index;
        return 0;
    case VIDIOC_DQBUF:
        if (!frames) return -1;   /* the DQBUF timeout */
        now_us += 25000;
        b->index = (uint32_t)(now_us / 25000) % 2;
        b->bytesused = sizeof(buffers[0]);
        b->flags = 0;
        return 0;
    default:
        return 0;
    }
}

void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, long offset)
{
    (void)addr;
    (void)prot;
    (void)flags;
    assert(fd == FD && len == sizeof(buffers[0]) && offset >= 0 && offset < 2);
    return buffers[offset];
}

esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *cfg, jpeg_encoder_handle_t *out)
{
    (void)cfg;
    encoders_live++;
    *out = (jpeg_encoder_handle_t)&encoders_live;
    return ESP_OK;
}

esp_err_t jpeg_del_encoder_engine(jpeg_encoder_handle_t encoder)
{
    assert(encoder);
    encoders_live--;
    return ESP_OK;
}

void *jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t *cfg, size_t *allocated)
{
    (void)cfg;
    *allocated = size;
    jpeg_bufs++;
    return calloc(1, size);
}

esp_err_t jpeg_encoder_process(jpeg_encoder_handle_t encoder, const jpeg_encode_cfg_t *cfg,
                               const uint8_t *in, uint32_t in_len, uint8_t *out,
                               uint32_t out_cap, uint32_t *out_len)
{
    assert(encoder && in && in_len == W * H * 2 && cfg->width == W && cfg->height == H);
    last_quality = cfg->image_quality;
    uint32_t len = jpeg_len_fixed ? jpeg_len_fixed : jpeg_len_at_quality_ten * cfg->image_quality / 10;
    if (len > out_cap) return ESP_FAIL;
    *out_len = len;
    out[0] = 0xFF;
    out[1] = 0xD8;
    return ESP_OK;
}

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (slen + 2) / 3 * 4;
    *olen = need;
    if (dlen < need + 1) return -1;
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16 | (i + 1 < slen ? src[i + 1] << 8 : 0)
                     | (i + 2 < slen ? src[i + 2] : 0);
        dst[o++] = abc[v >> 18 & 63];
        dst[o++] = abc[v >> 12 & 63];
        dst[o++] = i + 1 < slen ? abc[v >> 6 & 63] : '=';
        dst[o++] = i + 2 < slen ? abc[v & 63] : '=';
    }
    dst[o] = 0;
    return 0;
}

/* One capture; checks that it left nothing of esp_video behind. */
static bool capture(char **b64, const char **error)
{
    *error = NULL;
    bool ok = p4_csi_camera_capture(b64, error);
    assert(!video_live && video_inits == video_deinits);
    assert(opens == closes && encoders_live == 0);
    assert(ok == (*b64 != NULL) && ok == (*error == NULL));
    return ok;
}

int main(void)
{
    char *b64 = NULL;
    const char *error = NULL;
    p4_csi_camera_register(NULL);

    /* No sensor on the connector: esp_video still comes up, and has to go. */
    sensor = false;
    assert(!capture(&b64, &error) && strcmp(error, "no camera") == 0);
    assert(!capture(&b64, &error) && strcmp(error, "no camera") == 0);
    assert(video_inits == 2);

    /* A frame that fits at quality 70: 90 and 80 are over 150 KB. */
    sensor = true;
    jpeg_len_at_quality_ten = 20000;
    int64_t start = now_us;
    assert(capture(&b64, &error));
    assert(last_quality == 70 && now_us - start >= 1500000);
    assert(strncmp(b64, "/9", 2) == 0 && strlen(b64) == (140000 + 2) / 3 * 4);
    free(b64);

    /* Over 150 KB even at quality 20. */
    jpeg_len_fixed = 200000;
    assert(!capture(&b64, &error) && strcmp(error, "frame over 150 KB even at low quality") == 0);
    assert(last_quality == 20);

    /* A sensor that sends nothing. */
    frames = false;
    assert(!capture(&b64, &error) && strcmp(error, "camera sent no frame") == 0);

    assert(video_inits == 5 && jpeg_bufs == 2);
    printf("ok\n");
    return 0;
}
