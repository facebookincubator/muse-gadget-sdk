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

/*
 * The e-ink UI's fonts: the LVGL UI's, drawn by FreeInkFont from the files
 * LVGL builds its own from (eink/fonts/, tools/muse/gen_eink_fonts.sh), read
 * in place from the `fonts` partition (tools/muse/pack_eink_fonts.py).
 *
 * Each slot chains faces as LVGL's fonts do: Montserrat, then the FontAwesome
 * symbols LVGL builds into it (LV_SYMBOL_*), then GNU Unifont, the LVGL UI's
 * CJK fallback, for every script the others lack; unscii, then Unifont. Sizes
 * are em sizes, as lv_font_conv's are. Text is thresholded to 1 bit, which a
 * fast refresh shows crisply.
 */

#include <FreeInkUIRasterFont.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "muse_eink_priv.h"

namespace muse_eink {

using freeink::font::Arena;
using freeink::font::FontChain;
using freeink::font::GlyphBitmap;
using freeink::font::RasterFont;
using freeink::font::TtfFont;

static const char *TAG = "muse_eink";

#define ARENA_BYTES (96 * 1024)     /* each face's glyph cache, in PSRAM */
#define INK_THRESHOLD 128

/* Unifont is a 16 px pixel font: drawn at 16 or 32 px, whichever is nearer
 * the slot's size, so its pixels stay whole. */
class PixelFont final : public RasterFont {
 public:
    explicit PixelFont(RasterFont &face, uint16_t cell) : face_(face), cell_(cell) {}
    bool hasGlyph(uint32_t cp) const override { return face_.hasGlyph(cp); }
    int16_t advance(uint32_t cp, uint16_t size, uint8_t style) override { return face_.advance(cp, at(size), style); }
    int16_t lineHeight(uint16_t size) override { return face_.lineHeight(at(size)); }
    int16_t ascent(uint16_t size) override { return face_.ascent(at(size)); }
    const GlyphBitmap *rasterize(uint32_t cp, uint16_t size) override { return face_.rasterize(cp, at(size)); }

 private:
    uint16_t at(uint16_t size) const { return size >= cell_ * 3 / 2 ? cell_ * 2 : cell_; }
    RasterFont &face_;
    uint16_t cell_;
};

static TtfFont s_montserrat, s_symbols, s_unscii, s_unifont;
static Arena s_arenas[4];
static PixelFont s_unifont_px(s_unifont, 16);
static FontChain s_text, s_mono;
static freeink::ui::RasterGlyphSource s_text_src(s_text, INK_THRESHOLD), s_mono_src(s_mono, INK_THRESHOLD);

/* A font in the packed partition, by name, or NULL. */
static const uint8_t *find(const uint8_t *pack, size_t pack_len, const char *name, uint32_t *len)
{
    if (pack_len < 8 || memcmp(pack, "MFP1", 4)) return nullptr;
    uint32_t n;
    memcpy(&n, pack + 4, 4);
    for (uint32_t i = 0; i < n && 8 + 24 * (i + 1) <= pack_len; i++) {
        const uint8_t *e = pack + 8 + 24 * i;
        uint32_t off, size;
        memcpy(&off, e + 16, 4);
        memcpy(&size, e + 20, 4);
        if (!strncmp((const char *)e, name, 16) && off + size <= pack_len) {
            *len = size;
            return pack + off;
        }
    }
    return nullptr;
}

static bool load(TtfFont &font, Arena &arena, const uint8_t *pack, size_t pack_len, const char *name)
{
    uint32_t len;
    const uint8_t *data = find(pack, pack_len, name, &len);
    void *mem = heap_caps_malloc(ARENA_BYTES, MALLOC_CAP_SPIRAM);
    if (!data || !mem) {
        ESP_LOGE(TAG, "font %s: %s", name, data ? "no memory" : "not in the fonts partition");
        heap_caps_free(mem);
        return false;
    }
    arena.init(mem, ARENA_BYTES);
    font.setEmSizing(true);
    if (!font.init(data, len, arena)) {
        ESP_LOGE(TAG, "font %s: not a font", name);
        return false;
    }
    return true;
}

bool fonts_init(freeink::ui::DisplayTarget &target)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x42, "fonts");
    const void *pack = nullptr;
    esp_partition_mmap_handle_t handle;
    if (!part || esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &pack, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "no fonts partition: flash the whole build (partitions_muse_eink.csv)");
        return false;
    }
    const uint8_t *p = (const uint8_t *)pack;
    bool ok = load(s_montserrat, s_arenas[0], p, part->size, "montserrat") &
              load(s_symbols, s_arenas[1], p, part->size, "symbols") &
              load(s_unscii, s_arenas[2], p, part->size, "unscii") &
              load(s_unifont, s_arenas[3], p, part->size, "unifont");
    if (!ok) return false;
    s_text.add(&s_montserrat);
    s_text.add(&s_symbols);
    s_text.add(&s_unifont_px);
    s_mono.add(&s_unscii);
    s_mono.add(&s_unifont_px);

    struct Slot {
        FontId id;
        freeink::ui::RasterGlyphSource &src;
        FontChain &chain;
        uint16_t px;
    };
    const Slot slots[] = {
        {FONT_UNSCII8, s_mono_src, s_mono, 8},   {FONT_UNSCII16, s_mono_src, s_mono, 16},
        {FONT_M14, s_text_src, s_text, 14},      {FONT_M16, s_text_src, s_text, 16},
        {FONT_M20, s_text_src, s_text, 20},      {FONT_M28, s_text_src, s_text, 28},
    };
    for (const Slot &s : slots) {
        target.setFont(s.id, s.src, s.px, s.chain.lineHeight(s.px), s.chain.ascent(s.px));
    }
    return true;
}

}  // namespace muse_eink
