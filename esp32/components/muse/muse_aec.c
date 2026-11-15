/* SPDX-License-Identifier: Apache-2.0 */

#include "muse_aec.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "muse_audio.h"

#define AEC_FRAME 512
#define AEC_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define REF_CHUNKS CONFIG_MUSE_VOICE_AEC_REF_DELAY_CHUNKS
#define REF_FRAMES (REF_CHUNKS * MUSE_AUDIO_CHUNK)

_Static_assert(MUSE_AUDIO_RATE == 16000, "AEC expects 16 kHz audio");
_Static_assert(MUSE_AUDIO_CHUNK * 1000 == MUSE_AUDIO_RATE * 20,
               "AEC reference delay is calibrated in 20 ms chunks");
_Static_assert(MUSE_AUDIO_CHUNK <= AEC_FRAME,
               "one call-loop pass must produce at most one AEC frame");

static const char *TAG = "muse_aec";

struct muse_aec {
    aec_handle_t *handle;
    int16_t *mic_frame;
    int16_t *ref_frame;
    int16_t *out_frame;
    int16_t *ref_delay;
    size_t used;
    size_t delay_slot;
};

void muse_aec_stop(muse_aec_t *a)
{
    if (!a) return;
    if (a->handle) aec_destroy(a->handle);
    free(a->mic_frame);
    free(a->ref_frame);
    free(a->out_frame);
    free(a->ref_delay);
    free(a);
}

muse_aec_t *muse_aec_start(void)
{
    muse_aec_t *a = heap_caps_calloc(1, sizeof(*a), AEC_CAPS);
    if (!a) return NULL;
    aec_config_t cfg = {
        .mic_num = 1, .ref_num = 1, .out_num = 1,
        .filter_length = 4, .sample_rate = MUSE_AUDIO_RATE,
        .caps = AEC_CAPS, .mode = AEC_MODE_FD_HIGH_PERF,
        .nlp_level = AEC_NLP_LEVEL_NORMAL,
    };
    a->handle = aec_create_from_config(&cfg);
    /* The 20 ms call loop produces at most one 512-sample AEC frame per pass.
     * If the library changes frame size, fail closed to the half-duplex path. */
    if (!a->handle || aec_get_chunksize(a->handle) != AEC_FRAME) goto fail;
    a->mic_frame = heap_caps_aligned_alloc(16, AEC_FRAME * sizeof(int16_t), AEC_CAPS);
    a->ref_frame = heap_caps_aligned_alloc(16, AEC_FRAME * sizeof(int16_t), AEC_CAPS);
    a->out_frame = heap_caps_aligned_alloc(16, AEC_FRAME * sizeof(int16_t), AEC_CAPS);
    a->ref_delay = heap_caps_calloc(REF_FRAMES, sizeof(int16_t), AEC_CAPS);
    if (!a->mic_frame || !a->ref_frame || !a->out_frame || !a->ref_delay) goto fail;
    ESP_LOGI(TAG, "software AEC ready: reference delay %u ms", (unsigned)(REF_CHUNKS * 20));
    return a;
fail:
    ESP_LOGW(TAG, "software AEC unavailable; using half-duplex call audio");
    muse_aec_stop(a);
    return NULL;
}

int16_t *muse_aec_output(muse_aec_t *a)
{
    return a ? a->out_frame : NULL;
}

size_t muse_aec_feed(muse_aec_t *a, const int16_t *mic,
                     const int16_t *played, size_t played_frames)
{
    if (!a || !mic || !played) return 0;
    if (played_frames > MUSE_AUDIO_CHUNK) played_frames = MUSE_AUDIO_CHUNK;
    int16_t *old_ref = a->ref_delay + a->delay_slot * MUSE_AUDIO_CHUNK;
    size_t offset = 0;
    size_t result = 0;
    while (offset < MUSE_AUDIO_CHUNK) {
        size_t take = AEC_FRAME - a->used;
        if (take > MUSE_AUDIO_CHUNK - offset) take = MUSE_AUDIO_CHUNK - offset;
        memcpy(a->mic_frame + a->used, mic + offset, take * sizeof(int16_t));
        memcpy(a->ref_frame + a->used, old_ref + offset, take * sizeof(int16_t));
        a->used += take;
        offset += take;
        if (a->used == AEC_FRAME) {
            aec_process(a->handle, a->mic_frame, a->ref_frame, a->out_frame);
            a->used = 0;
            result = AEC_FRAME;
        }
    }
    memset(old_ref, 0, MUSE_AUDIO_CHUNK * sizeof(int16_t));
    memcpy(old_ref, played, played_frames * sizeof(int16_t));
    a->delay_slot = (a->delay_slot + 1) % REF_CHUNKS;
    return result;
}
