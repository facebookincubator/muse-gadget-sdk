/*
 * Muse Gadget TTS proxy transport (Apache License, Version 2.0).
 * No ElevenLabs API keys are stored on the gadget.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_TTS_PACKET_BYTES 1024

typedef enum {
    MUSE_TTS_DATA = 1,
    MUSE_TTS_DONE = 2,
    MUSE_TTS_FAILED = 3
} muse_tts_packet_kind_t;

typedef struct {
    uint32_t id;
    uint16_t len;
    uint8_t kind;
    uint8_t bytes[MUSE_TTS_PACKET_BYTES];
} muse_tts_packet_t;

/* One worker owns HTTP. The Muse/Noise task owns all MP3 decode state. */
bool muse_tts_proxy_init(void);
bool muse_tts_proxy_ready(void);
bool muse_tts_proxy_submit(uint32_t request_id, const char *text);
bool muse_tts_proxy_poll(muse_tts_packet_t *packet);
void muse_tts_proxy_cancel(void);

#ifdef __cplusplus
}
#endif
