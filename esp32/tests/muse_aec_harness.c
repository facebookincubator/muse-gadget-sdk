/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "muse_aec.h"

static bool fail_create;
static int fake_frame_size = 512;
static int process_count;
static int16_t last_mic[512], last_ref[512];

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}

void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps)
{
    (void)caps;
    return aligned_alloc(alignment, size);
}

aec_handle_t *aec_create_from_config(aec_config_t *config)
{
    assert(config->sample_rate == 16000);
    assert(config->mode == AEC_MODE_FD_HIGH_PERF);
    if (fail_create) return NULL;
    aec_handle_t *h = malloc(sizeof(*h));
    assert(h);
    h->frame_size = fake_frame_size;
    return h;
}

int aec_get_chunksize(const aec_handle_t *h) { return h->frame_size; }

void aec_process(const aec_handle_t *h, int16_t *mic, int16_t *ref, int16_t *out)
{
    assert(h->frame_size == 512);
    memcpy(last_mic, mic, sizeof(last_mic));
    memcpy(last_ref, ref, sizeof(last_ref));
    memcpy(out, mic, sizeof(last_mic));
    ++process_count;
}

void aec_destroy(aec_handle_t *h) { free(h); }

static void fill(int16_t *samples, int16_t value)
{
    for (int i = 0; i < 320; i++) samples[i] = value;
}

int main(void)
{
    int16_t mic[320], played[320];
    muse_aec_t *a = muse_aec_start();
    assert(a);
    fill(mic, 1); fill(played, 100);
    assert(muse_aec_feed(a, mic, played, 320) == 0);
    fill(mic, 2); fill(played, 200);
    assert(muse_aec_feed(a, mic, played, 320) == 512);
    assert(process_count == 1);
    for (int i = 0; i < 512; i++) {
        assert(last_ref[i] == 0);
        assert(last_mic[i] == (i < 320 ? 1 : 2));
        assert(muse_aec_output(a)[i] == last_mic[i]);
    }
    fill(mic, 3); fill(played, 300);
    assert(muse_aec_feed(a, mic, played, 320) == 0);
    fill(mic, 4); fill(played, 400);
    assert(muse_aec_feed(a, mic, played, 320) == 512);
    assert(process_count == 2);
    for (int i = 0; i < 512; i++) {
        int expected = i < 128 ? 0 : i < 448 ? 100 : 200;
        assert(last_ref[i] == expected);
    }
    muse_aec_stop(a);

    fail_create = true;
    assert(muse_aec_start() == NULL);
    fail_create = false;
    fake_frame_size = 256;
    assert(muse_aec_start() == NULL);
    muse_aec_stop(NULL);
    return 0;
}
