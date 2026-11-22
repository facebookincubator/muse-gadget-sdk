/*
 * 3D-style Muse artwork for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 * The seven RGB565 stills are pre-rendered concept art. Small source-image
 * shifts, a short state crossfade, and voice-driven motion animate them while
 * keeping all pixel work in the existing LVGL strip decoder.
 *
 * The artwork depicts the Jollybot character, whose rights are separate from
 * the Apache-licensed SDK (see the repository README).
 */

#include "muse_pixel.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define ART_SIZE 160
#define MAP_MAX 512

#define ART_SYMBOL(name) \
    extern const uint8_t name##_start[] asm("_binary_" #name "_rgb565_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_rgb565_end")

ART_SYMBOL(muse3d_idle);
ART_SYMBOL(muse3d_boot);
ART_SYMBOL(muse3d_listening);
ART_SYMBOL(muse3d_thinking);
ART_SYMBOL(muse3d_speaking);
ART_SYMBOL(muse3d_error);
ART_SYMBOL(muse3d_off);

static const uint8_t *const ART[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = muse3d_boot_start,
    [MUSE_MODE_IDLE] = muse3d_idle_start,
    [MUSE_MODE_LISTENING] = muse3d_listening_start,
    [MUSE_MODE_THINKING] = muse3d_thinking_start,
    [MUSE_MODE_SPEAKING] = muse3d_speaking_start,
    [MUSE_MODE_ERROR] = muse3d_error_start,
    [MUSE_MODE_OFF] = muse3d_off_start,
};

static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = 0xa9c0ff,
    [MUSE_MODE_IDLE] = 0xa77dff,
    [MUSE_MODE_LISTENING] = 0x5cb8ff,
    [MUSE_MODE_THINKING] = 0xe07bff,
    [MUSE_MODE_SPEAKING] = 0x6ff0bf,
    [MUSE_MODE_ERROR] = 0xff5c5c,
    [MUSE_MODE_OFF] = 0x7c72d0,
};

static uint16_t s_coord[MAP_MAX]; /* Q8 source coordinate for each output pixel */
static int s_size;
static const uint8_t *s_art = muse3d_idle_start;
static const uint8_t *s_previous;
static float s_changed_at;
static float s_t;
static float s_mode_t;
static muse_mode_t s_mode = MUSE_MODE_IDLE;
static int s_shift;
static int s_shift_x;

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return ACCENT[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE];
}

uint32_t muse_pixel_working_accent(void)
{
    /* This art uses the thinking image for working, so keep its violet UI. */
    return 0;
}

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    if (s_size < 1) s_size = 1;
    for (int i = 0; i < s_size; i++) {
        s_coord[i] = (uint16_t)((uint32_t)i * (ART_SIZE - 1) * 256 / (s_size - 1 ? s_size - 1 : 1));
    }
}

void muse_pixel_render(const muse_pose_t *pose)
{
    muse_mode_t mode = pose->mode < MUSE_MODE_COUNT ? pose->mode : MUSE_MODE_IDLE;
    const uint8_t *next = ART[mode];
    if (next != s_art) {
        s_previous = s_art;
        s_art = next;
        s_changed_at = pose->t;
    }
    s_mode = mode;
    s_t = pose->t;
    s_mode_t = pose->mode_t;
    /* A few source pixels of motion animate the stills without moving the UI. */
    float bob = sinf(pose->t * 3.0f) * 0.65f;
    if (mode == MUSE_MODE_SPEAKING) bob -= pose->level * 1.5f;
    if (mode == MUSE_MODE_BOOT) bob -= 2.0f * fmaxf(0.0f, 1.0f - pose->mode_t / 0.6f);
    s_shift = (int)lroundf(bob - pose->happy * 2.0f);
    s_shift_x = 0;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING)
        s_shift_x = (int)lroundf(sinf(pose->t * 1.9f) * 0.9f);
    if (mode == MUSE_MODE_ERROR)
        s_shift_x = (int)lroundf(sinf(pose->t * 17.0f) * 1.2f);
}

static inline uint16_t pixel(const uint8_t *art, int x, int y)
{
    if (y < 0 || y >= ART_SIZE) return 0;
    const uint8_t *p = art + ((size_t)y * ART_SIZE + x) * 2;
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static inline uint16_t mix565(uint16_t a, uint16_t b, unsigned t)
{
    unsigned u = 256 - t;
    unsigned r = (((a >> 11) * u + (b >> 11) * t) >> 8);
    unsigned g = ((((a >> 5) & 63) * u + ((b >> 5) & 63) * t) >> 8);
    unsigned bl = (((a & 31) * u + (b & 31) * t) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static inline uint16_t sample(const uint8_t *art, unsigned fx, int fy)
{
    int x = fx >> 8;
    int y = fy >> 8;
    unsigned tx = fx & 255;
    unsigned ty = fy & 255;
    int x1 = x + (x < ART_SIZE - 1);
    uint16_t top = mix565(pixel(art, x, y), pixel(art, x1, y), tx);
    uint16_t bottom = mix565(pixel(art, x, y + 1), pixel(art, x1, y + 1), tx);
    return mix565(top, bottom, ty);
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    if (!s_size) muse_pixel_set_size(MUSE_PX_W * 2);
    float elapsed = s_t - s_changed_at;
    unsigned blend = s_previous && elapsed >= 0 && elapsed < 0.20f
                         ? (unsigned)(elapsed * 1280.0f) : 256;
    /* Decoder strips may be sampled concurrently: keep frame state read-only. */

    unsigned brightness = 256;
    if (s_mode == MUSE_MODE_BOOT && s_mode_t < 0.35f)
        brightness = (unsigned)(s_mode_t > 0 ? s_mode_t * (256.0f / 0.35f) : 0);
    if (s_mode == MUSE_MODE_OFF && s_mode_t > 0.4f)
        brightness = s_mode_t >= 1.3f ? 0 : (unsigned)((1.3f - s_mode_t) * (256.0f / 0.9f));
    if (brightness > 256) brightness = 256;

    for (int y = y0; y <= y1; y++, dst += stride_px) {
        int fy = (int)s_coord[y] - s_shift * 256;
        for (int x = x0; x <= x1; x++) {
            int fx = (int)s_coord[x] - s_shift_x * 256;
            uint16_t color = fx >= 0 && fx <= (ART_SIZE - 1) * 256
                                 ? sample(s_art, (unsigned)fx, fy) : 0;
            if (blend < 256) {
                uint16_t before = fx >= 0 && fx <= (ART_SIZE - 1) * 256
                                      ? sample(s_previous, (unsigned)fx, fy) : 0;
                color = mix565(before, color, blend);
            }
            if (brightness < 256) color = mix565(0, color, brightness);
            dst[x - x0] = color;
        }
    }
}
