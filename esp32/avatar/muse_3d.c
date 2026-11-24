/* Jollybot artwork has separate character rights. See avatar/3d/README.md. */
#include "muse_3d.h"
#include "muse_pixel.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define ART_SIZE 320
#define BLOCK_ROWS 16
#define BLOCKS (ART_SIZE / BLOCK_ROWS)

typedef struct {
    muse_art_rect_t rect;
    uint32_t offset;
} patch_t;

typedef struct {
    uint32_t blocks[BLOCKS + 1];
    patch_t patches[MUSE_EXPRESSION_COUNT];
    uint32_t background_mask;
} pose_art_t;

#include "3d/muse3d_assets.inc"
extern const uint8_t art[] asm("_binary_muse3d_bin_start");
extern const uint8_t art_end[] asm("_binary_muse3d_bin_end");

typedef struct {
    uint16_t *pixels, *patch_pixels;
    patch_t patches[MUSE_EXPRESSION_COUNT];   /* offsets in uint16_t pixels */
    muse_base_t pose;
    int size;
} frame_t;

static frame_t frames[2], *front = &frames[0], *back = &frames[1];
static int requested_size, shown_variant, capacity;
static int next_block;
static bool preparing, ready, failed;
static uint16_t *scratch;
static uint16_t background;

void muse_pixel_set_background(uint32_t rgb)
{
    background = ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
}
static const char *TAG = "muse_art";

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    static const uint32_t colors[MUSE_MODE_COUNT] = {
        [MUSE_MODE_BOOT] = 0xa9c0ff, [MUSE_MODE_IDLE] = 0xa77dff,
        [MUSE_MODE_LISTENING] = 0x5cb8ff, [MUSE_MODE_THINKING] = 0xe07bff,
        [MUSE_MODE_SPEAKING] = 0x6ff0bf, [MUSE_MODE_ERROR] = 0xff5c5c,
        [MUSE_MODE_OFF] = 0x7c72d0,
    };
    return colors[(unsigned)mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE];
}

uint32_t muse_pixel_working_accent(void) { return 0xffbc65; }

void muse_pixel_set_size(int px)
{
    requested_size = px;
    if (capacity || failed || px < 1) return;
    capacity = ART_SIZE;  /* the largest native pose, even when starting in a small layout */
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    scratch = heap_caps_malloc(ART_SIZE * BLOCK_ROWS * sizeof(*scratch), caps);
    for (int i = 0; i < 2; i++) {
        frames[i].pixels = heap_caps_malloc(capacity * capacity * sizeof(uint16_t), caps);
        frames[i].patch_pixels = heap_caps_malloc(PATCH_BYTES, caps);
    }
    if (!scratch || !frames[0].pixels || !frames[1].pixels ||
        !frames[0].patch_pixels || !frames[1].patch_pixels) {
        for (int i = 0; i < 2; i++) {
            heap_caps_free(frames[i].pixels);
            heap_caps_free(frames[i].patch_pixels);
            frames[i] = (frame_t){0};
        }
        heap_caps_free(scratch);
        failed = true;
        ESP_LOGE(TAG, "insufficient PSRAM for avatar cache");
        return;
    }
    ESP_LOGI(TAG, "pose caches %u bytes, preparation %u bytes",
             (unsigned)(2 * (capacity * capacity * sizeof(uint16_t) + PATCH_BYTES)),
             (unsigned)(ART_SIZE * BLOCK_ROWS * sizeof(*scratch)));
}

/* First output coordinate whose source sample is at or beyond src. All
 * patches use exactly the base's sampling grid, also in smaller reply layouts. */
static int project(int src, int size) { return (src * size + ART_SIZE - 1) / ART_SIZE; }

static bool decode_block(int block)
{
    const uint32_t *offset = assets[back->pose].blocks;
    if (offset[block+1] > (size_t)(art_end-art)) return false;
    const uint8_t *src = art + offset[block], *end = art + offset[block+1];
    int first = block * BLOCK_ROWS, size = back->size;
    uint16_t *dst = size == ART_SIZE ? back->pixels + first*size : scratch;
    unsigned remaining = ART_SIZE * BLOCK_ROWS;
    while (end-src >= 2) {
        uint16_t header;
        memcpy(&header, src, 2);
        src += 2;
        unsigned count = header & 0x7fff;
        if (!count || count > remaining) return false;
        if (header & 0x8000) {
            if (end-src < 2) return false;
            uint16_t color;
            memcpy(&color, src, 2);
            src += 2;
            if (!color) memset(dst, 0, count*sizeof(*dst));
            else for (unsigned i = 0; i < count; i++) dst[i] = color;
        } else {
            if ((size_t)(end-src) < count*2) return false;
            memcpy(dst, src, count*2);
            src += count*2;
        }
        dst += count;
        remaining -= count;
    }
    if (remaining || src != end) return false;
    if (size != ART_SIZE) {
        for (int y = project(first, size); y < project(first + BLOCK_ROWS, size); y++) {
            const uint16_t *row = scratch + (y * ART_SIZE / size - first) * ART_SIZE;
            for (int x = 0; x < size; x++) back->pixels[y*size+x] = row[x * ART_SIZE / size];
        }
    }
    return true;
}

static bool prepare_patches(void)
{
    const int size = back->size;
    size_t used = 0;
    for (int i = 0; i < MUSE_EXPRESSION_COUNT; i++) {
        patch_t source = assets[back->pose].patches[i];
        muse_art_rect_t r = source.rect;
        int x = project(r.x, size), y = project(r.y, size);
        int w = project(r.x + r.w, size) - x, h = project(r.y + r.h, size) - y;
        if (used + w*h > PATCH_BYTES / 2 || source.offset + 2*r.w*r.h > (size_t)(art_end-art))
            return false;
        back->patches[i] = (patch_t){{x,y,w,h}, used};
        uint16_t *out = back->patch_pixels + used;
        used += w*h;
        const uint8_t *src = art + source.offset;
        if (size == ART_SIZE) {
            memcpy(out, src, 2*w*h);
        } else {
            for (int j = y; j < y+h; j++) {
                int row = (j * ART_SIZE / size - r.y) * r.w;
                for (int k = x; k < x+w; k++) {
                    int index = row + k * ART_SIZE / size - r.x;
                    memcpy(out++, src + index*2, 2);
                }
            }
        }
    }
    return true;
}

bool muse_3d_preparing(void) { return preparing; }

void muse_3d_prepare_slice(void)
{
    if (!preparing) return;
    int64_t until = esp_timer_get_time() + 2000;
    do {
        if (next_block == BLOCKS) {
            ready = prepare_patches();
            failed = !ready;
            preparing = false;
        } else if (!decode_block(next_block++)) {
            failed = true;
            preparing = false;
        }
    } while (preparing && esp_timer_get_time() < until);
    if (failed) ESP_LOGE(TAG, "asset preparation failed for pose %d", back->pose);
}

muse_art_damage_t muse_3d_update(muse_expression_t wanted)
{
    muse_art_damage_t damage = {0};
    int size = requested_size < capacity ? requested_size : capacity;
    if (failed || !size || (unsigned)wanted.base >= POSE_COUNT || wanted.variant >= MUSE_EXPRESSION_COUNT)
        return damage;
    if (front->pose != wanted.base || front->size != size) {
        if ((!ready && !preparing) || back->pose != wanted.base || back->size != size) {
            back->pose = wanted.base;
            back->size = size;
            next_block = 0;
            preparing = true;
            ready = false;
            muse_3d_prepare_slice();  /* use this tick's first bounded slice */
        }
        if (!ready) return damage;
        /* LVGL timers run serially. draw_buf_flush joins all draw tasks before
         * its refresh returns. DMA owns LVGL's separate band, never this image. */
        frame_t *previous = front;
        front = back;
        back = previous;
        damage.rect[damage.count++] = (muse_art_rect_t){0, 0, requested_size, requested_size};
    } else if ((unsigned)shown_variant != wanted.variant) {
        muse_art_rect_t old = front->patches[shown_variant].rect;
        muse_art_rect_t next = front->patches[wanted.variant].rect;
        if (old.w && old.h) damage.rect[damage.count++] = old;
        if (next.w && next.h && (!damage.count || memcmp(&old, &next, sizeof(old))))
            damage.rect[damage.count++] = next;
    }
    if (size != requested_size && damage.count && damage.rect[0].w != requested_size) {
        for (unsigned i = 0; i < damage.count; i++) {
            muse_art_rect_t r = damage.rect[i];
            int x = (r.x * requested_size + size - 1) / size;
            int y = (r.y * requested_size + size - 1) / size;
            damage.rect[i] = (muse_art_rect_t){x, y,
                ((r.x+r.w)*requested_size + size-1)/size-x,
                ((r.y+r.h)*requested_size + size-1)/size-y};
        }
    }
    preparing = ready = false;
    shown_variant = wanted.variant;
    return damage;
}

/* Base is immutable. A patch is a second immutable layer; switching it
 * restores all old pixels automatically, including when the ring exposes them. */
void muse_pixel_scale(uint16_t *dst, int stride, int x0, int x1, int y0, int y1)
{
    const int size = front->size;
    const patch_t *p = &front->patches[shown_variant];
    const muse_art_rect_t r = p->rect;
    int width = x1-x0+1;
    for (int y = y0; y <= y1; y++, dst += stride) {
        if (!size) {
            memset(dst, 0, width*sizeof(*dst));
        } else if (size == requested_size) {
            memcpy(dst, front->pixels+y*size+x0, width*sizeof(*dst));
            int left = x0 > r.x ? x0 : r.x, end = x1+1 < r.x+r.w ? x1+1 : r.x+r.w;
            if (y >= r.y && y < r.y+r.h && end > left)
                memcpy(dst+left-x0, front->patch_pixels+p->offset+(y-r.y)*r.w+left-r.x,
                       (end-left)*sizeof(*dst));
        } else {
            /* The existing caption layout can resize the widget every tick.
             * Keep showing the previous complete pose until the new size is ready. */
            int sy = y * size / requested_size;
            for (int x = x0; x <= x1; x++) {
                int sx = x * size / requested_size;
                bool overlay = sx >= r.x && sx < r.x+r.w && sy >= r.y && sy < r.y+r.h;
                dst[x-x0] = overlay ? front->patch_pixels[p->offset+(sy-r.y)*r.w+sx-r.x]
                                    : front->pixels[sy*size+sx];
            }
        }
        if (background) {
            for (int x = x0; x <= x1; x++) {
                if (!size) {
                    dst[x-x0] = background;
                    continue;
                }
                int sx = (x * size / requested_size) * ART_SIZE / size;
                int sy = (y * size / requested_size) * ART_SIZE / size;
                unsigned at = sy * ART_SIZE + sx;
                uint32_t mask = assets[front->pose].background_mask + at / 8;
                if (mask < (size_t)(art_end-art) && (art[mask] & (1u << (at % 8))))
                    dst[x-x0] = background;
            }
        }
    }
}
