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
 * The e-ink UI's fonts: the LVGL UI's, drawn by FreeInkFont from the TTFs
 * LVGL builds its own from (eink/fonts/, tools/muse/gen_eink_fonts.sh),
 * embedded in the app.
 *
 * Each slot chains faces as LVGL's fonts do: Montserrat, then the FontAwesome
 * symbols LVGL builds into it (LV_SYMBOL_*); unscii. With CONFIG_MUSE_CJK_FONT
 * both end in the LVGL UI's CJK fallback, GNU Unifont's 16 px bitmaps
 * (fonts/muse_font_cjk_16.c), as is. Sizes are em sizes, as lv_font_conv's
 * are. Text is thresholded to 1 bit, which a fast refresh shows crisply.
 */

#include <FreeInkUIRasterFont.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "muse_eink_priv.h"
#if CONFIG_MUSE_CJK_FONT
#include "lvgl_font/lvgl.h"
extern "C" const lv_font_t muse_font_cjk_16;
#endif

extern const uint8_t montserrat_ttf_start[] asm("_binary_montserrat_ttf_start");
extern const uint8_t montserrat_ttf_end[] asm("_binary_montserrat_ttf_end");
extern const uint8_t symbols_ttf_start[] asm("_binary_symbols_ttf_start");
extern const uint8_t symbols_ttf_end[] asm("_binary_symbols_ttf_end");
extern const uint8_t unscii_ttf_start[] asm("_binary_unscii_ttf_start");
extern const uint8_t unscii_ttf_end[] asm("_binary_unscii_ttf_end");

namespace muse_eink {

using freeink::font::Arena;
using freeink::font::FontChain;
using freeink::font::GlyphBitmap;
using freeink::font::RasterFont;
using freeink::font::TtfFont;

static const char *TAG = "muse_eink";

#define ARENA_BYTES (96 * 1024)     /* each face's glyph cache, in PSRAM */
#define INK_THRESHOLD 64     /* coverage that counts as ink: lower is bolder; 128 drew Montserrat too thin */

#if CONFIG_MUSE_CJK_FONT
/* The LVGL UI's CJK font, a 1 bpp lv_font_conv font whose ranges are all
 * FORMAT0_TINY, at its own 16 px whatever the slot's size. */
class CjkFont final : public RasterFont {
 public:
    bool hasGlyph(uint32_t cp) const override { return id(cp) != 0; }
    int16_t advance(uint32_t cp, uint16_t, uint8_t) override
    {
        int g = id(cp);
        return g ? (int16_t)((dsc()->glyph_dsc[g].adv_w + 8) / 16) : 0;
    }
    int16_t lineHeight(uint16_t) override { return (int16_t)muse_font_cjk_16.line_height; }
    int16_t ascent(uint16_t) override { return (int16_t)(muse_font_cjk_16.line_height - muse_font_cjk_16.base_line); }
    const GlyphBitmap *rasterize(uint32_t cp, uint16_t) override
    {
        int g = id(cp);
        if (!g) return nullptr;
        const lv_font_fmt_txt_glyph_dsc_t &d = dsc()->glyph_dsc[g];
        int n = d.box_w * d.box_h;
        if (n > (int)sizeof(px_)) return nullptr;
        const uint8_t *bits = dsc()->glyph_bitmap + d.bitmap_index;
        for (int i = 0; i < n; i++) px_[i] = (bits[i >> 3] >> (7 - (i & 7))) & 1 ? 255 : 0;
        out_ = GlyphBitmap{px_, d.box_w, d.box_h, d.ofs_x, (int16_t)-(d.ofs_y + d.box_h),
                           (int16_t)((d.adv_w + 8) / 16)};
        return &out_;
    }

 private:
    static const lv_font_fmt_txt_dsc_t *dsc() { return (const lv_font_fmt_txt_dsc_t *)muse_font_cjk_16.dsc; }
    static int id(uint32_t cp)
    {
        const lv_font_fmt_txt_dsc_t *d = dsc();
        int lo = 0, hi = (int)d->cmap_num - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            const lv_font_fmt_txt_cmap_t &c = d->cmaps[mid];
            if (cp < c.range_start) hi = mid - 1;
            else if (cp >= c.range_start + c.range_length) lo = mid + 1;
            else return c.glyph_id_start + (int)(cp - c.range_start);
        }
        return 0;
    }
    uint8_t px_[16 * 17];
    GlyphBitmap out_{};
};
static CjkFont s_cjk;
#endif

static TtfFont s_montserrat, s_symbols, s_unscii;
static Arena s_arenas[3];
static FontChain s_text, s_mono;
static freeink::ui::RasterGlyphSource s_text_src(s_text, INK_THRESHOLD), s_mono_src(s_mono, INK_THRESHOLD);

static bool load(TtfFont &font, Arena &arena, const uint8_t *start, const uint8_t *end, const char *name)
{
    void *mem = heap_caps_malloc(ARENA_BYTES, MALLOC_CAP_SPIRAM);
    if (!mem) {
        ESP_LOGE(TAG, "font %s: no memory", name);
        return false;
    }
    arena.init(mem, ARENA_BYTES);
    font.setEmSizing(true);
    if (!font.init(start, (uint32_t)(end - start), arena)) {
        ESP_LOGE(TAG, "font %s: not a font", name);
        return false;
    }
    return true;
}

bool fonts_init(freeink::ui::DisplayTarget &target)
{
    bool ok = load(s_montserrat, s_arenas[0], montserrat_ttf_start, montserrat_ttf_end, "montserrat") &
              load(s_symbols, s_arenas[1], symbols_ttf_start, symbols_ttf_end, "symbols") &
              load(s_unscii, s_arenas[2], unscii_ttf_start, unscii_ttf_end, "unscii");
    if (!ok) return false;
    s_text.add(&s_montserrat);
    s_text.add(&s_symbols);
    s_mono.add(&s_unscii);
#if CONFIG_MUSE_CJK_FONT
    s_text.add(&s_cjk);
    s_mono.add(&s_cjk);
#endif

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
