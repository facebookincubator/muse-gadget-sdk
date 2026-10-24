# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def production_function(source, name):
    """Extract an existing production function, never a test reimplementation."""
    match = re.search(r"(?:static )?(?:esp_err_t|bool|void) " + name + r"\([^)]*\)\s*\{", source)
    if not match:
        raise AssertionError(name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class LinkEpaperStatusTest(unittest.TestCase):
    def compile_production_check(self, name, code):
        path = self.out / (name + ".c")
        path.write_text(code)
        cc = shlex.split(os.environ.get("CC", "cc"))
        exe = self.out / (name + ".exe")
        r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(exe)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout+r.stderr)

    def test_it8951_packing_windows_and_bus_release(self):
        source = (ROOT / "main/epd_it8951.c").read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define ESP_ERR_INVALID_ARG -2
#define PANEL_W 1872
#define PANEL_H 1404
#define GRAY_ROW_BYTES (PANEL_W/2)
#define TRANSFER_ROWS 14
#define CMD_LOAD_AREA 0x21
#define CMD_LOAD_END 0x22
#define CMD_DISPLAY_AREA 0x34
#define REG_LISAR 0x208
#define REG_ONE_BIT_MODE 0x113a
#define REG_ONE_BIT_COLOURS 0x1250
#define SPI_TRANS_CS_KEEP_ACTIVE 1
#define portMAX_DELAY 10000
#define ESP_LOGI(tag,...) ((void)snprintf((char[256]){0},256,__VA_ARGS__))
#define ESP_LOGE(...) ((void)0)
typedef struct { int flags; size_t length; const void *tx_buffer; } spi_transaction_t;
static void *s_spi = (void *)1;
static uint32_t s_image_address;
static uint8_t tx[TRANSFER_ROWS*GRAY_ROW_BYTES], *s_tx=tx;
static uint8_t luma[PANEL_W*PANEL_H], wire[PANEL_W*PANEL_H/2];
static size_t used;
static int acquired, released, outer, takes, gives, fail_load, displays;
static uint16_t args[8];
static size_t nargs;
static esp_err_t write_reg(uint16_t r,uint16_t v) { (void)r;(void)v;return 0; }
static esp_err_t read_reg(uint16_t r,uint16_t *v) { (void)r;*v=0x18;return 0; }
static esp_err_t command(uint16_t c) { nargs=0;if(c==CMD_DISPLAY_AREA) displays++;return 0; }
static esp_err_t data_word(uint16_t v) { assert(nargs<8);args[nargs++]=v;return 0; }
static esp_err_t wait_ready(int t) { assert(t>0);return 0; }
static esp_err_t word(uint16_t v,void *r,bool keep) { (void)v;(void)r;assert(keep);return 0; }
static esp_err_t wait_refresh(bool start) { (void)start;return 0; }
static esp_err_t set_refresh_temperature(void) { return 0; }
static bool reterminal_e100x_spi_lock(void) { assert(!outer);outer=1;takes++;return true; }
static void reterminal_e100x_spi_unlock(void) { assert(outer);outer=0;gives++; }
static esp_err_t spi_device_acquire_bus(void *s,int t) { assert(s && t);acquired++;return 0; }
static void spi_device_release_bus(void *s) { assert(s);released++; }
static esp_err_t spi_device_polling_transmit(void *s,const spi_transaction_t *t) {
    assert(s && used+t->length/8<=sizeof(wire));
    memcpy(wire+used,t->tx_buffer,t->length/8);used+=t->length/8;return 0;
}
static esp_err_t load_frame_area(const uint8_t *f,int x,int y,int w,int h) {
    assert(f && x>=0 && y>=0 && w>0 && h>0);return fail_load ? -3 : 0;
}
'''
        for name in ("load_gray16", "display_area", "it8951_update_gray16", "it8951_update_partial"):
            code += production_function(source, name) + "\n"
        code += r'''
int main(void) {
    for (int y=0;y<PANEL_H;y++) for(int x=0;x<PANEL_W;x++) luma[y*PANEL_W+x]=(x%16)*17;
    assert(it8951_update_gray16(luma)==0 && used==sizeof(wire));
    assert(acquired==released && takes==gives && !outer);
    assert(args[0]==0 && args[2]==1872 && args[3]==1404 && args[4]==2);
    for(size_t i=0;i<sizeof(wire);i+=8) {
        static const uint8_t expected[8]={0xcd,0xef,0x89,0xab,0x45,0x67,0x01,0x23};
        assert(!memcmp(wire+i,expected,8));
    }
    memset(luma,255,sizeof(luma));used=0;assert(load_gray16(luma)==0);
    for(size_t i=0;i<sizeof(wire);i++) assert(wire[i]==255);
    memset(luma,0,sizeof(luma));used=0;assert(load_gray16(luma)==0);
    for(size_t i=0;i<sizeof(wire);i++) assert(wire[i]==0);
    assert(it8951_update_partial(luma,288,312,288,312)==0);
    assert(args[0]==1280 && args[1]==312 && args[2]==320 && args[3]==312 && args[4]==1);
    for(int x=0;x<PANEL_W;x+=16) for(int w=16;w<=PANEL_W-x;w+=16) {
        assert(it8951_update_partial(luma,x,0,w,1)==0);
        int left=PANEL_W-args[0]-args[2];
        assert(left<=x && left+args[2]>=x+w && left>=0 && left+args[2]<=PANEL_W);
        assert(args[0]%32==0 && x-left<32 && left+args[2]-x-w<32);
    }
    used=0;memset(luma,0,sizeof(luma));
    luma[PANEL_W-4]=8;luma[PANEL_W-3]=9;luma[PANEL_W-2]=246;luma[PANEL_W-1]=247;
    assert(load_gray16(luma)==0 && wire[0]==0x01 && wire[1]==0xef);
    int before=displays;fail_load=1;
    assert(it8951_update_partial(luma,288,312,288,312)!=0 && displays==before);
    assert(takes==gives && !outer);
    assert(it8951_update_partial(luma,1,0,16,1)==ESP_ERR_INVALID_ARG);
    return 0;
}
'''
        self.compile_production_check("it8951", code)

    def test_partial_refresh_state_and_cancel(self):
        source = (ROOT / "main/epaper_status.c").read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951 1
#define FULL_REFRESH_EVERY 10
#define EPD_W 64
#define EPD_H 16
#define EPD_ROW_BYTES (EPD_W/8)
#define EPD_FRAME_BYTES (EPD_ROW_BYTES*EPD_H)
#define portMAX_DELAY 10000
#define ESP_LOGI(tag,...) ((void)snprintf((char[256]){0},256,__VA_ARGS__))
static bool s_ready=true,s_image_mode,s_image_dirty,s_gray4_mode,s_gray16_mode;
static bool s_partial_mode,s_partial_ready,s_status_drawn;
static unsigned s_partial_updates;
static int s_partial_x,s_partial_y,s_partial_w,s_partial_h,s_fast_refreshes;
static int s_panel_lock,s_lock,fulls,partials,grays,failing;
static uint8_t canvas[EPD_W*EPD_H],frame[EPD_FRAME_BYTES],shown[EPD_FRAME_BYTES];
static uint8_t *s_canvas=canvas,*s_frame=frame,*s_shown=shown;
static uint8_t *s_gray_old=frame,*s_gray4_frame=frame;
static void xSemaphoreTake(int m,int t) { (void)m;(void)t; }
static void xSemaphoreGive(int m) { (void)m; }
static void epd_dither(void) {}
static esp_err_t update(const uint8_t *a,const uint8_t *b,bool full) { (void)a;(void)b;assert(full);fulls++;return failing; }
static esp_err_t gray(const uint8_t *a) { (void)a;grays++;return failing; }
static esp_err_t partial(const uint8_t *a,int x,int y,int w,int h) { (void)a;assert(x==16 && y==2 && w==16 && h==4);partials++;return failing; }
static struct {
    esp_err_t (*update)(const uint8_t *,const uint8_t *,bool);
    esp_err_t (*update_gray16)(const uint8_t *);
    esp_err_t (*update_gray4_planes)(const uint8_t *,const uint8_t *);
    esp_err_t (*update_gray4)(const uint8_t *);
    esp_err_t (*update_partial)(const uint8_t *,int,int,int,int);
} panel={.update=update,.update_gray16=gray,.update_partial=partial},*s_panel=&panel;
'''
        for name in ("led_status_draw_begin_partial", "led_status_draw_cancel_partial", "led_status_draw_done"):
            code += production_function(source, name) + "\n"
        code += r'''
int main(void) {
    assert(!led_status_draw_begin_partial(16,2,16,4));
    s_image_mode=s_image_dirty=true;assert(led_status_draw_done() && s_partial_ready);
    for(int i=1;i<=10;i++) {
        assert(led_status_draw_begin_partial(16,2,16,4));s_image_dirty=true;
        assert(led_status_draw_done());assert(s_partial_updates==(unsigned)i%10);
    }
    assert(partials==9 && fulls==2);
    assert(led_status_draw_begin_partial(16,2,16,4));s_image_dirty=true;failing=-1;
    assert(!led_status_draw_done() && s_partial_updates==0);
    failing=0;assert(led_status_draw_begin_partial(16,2,16,4));s_image_dirty=true;
    assert(led_status_draw_done() && s_partial_updates==1);
    for(int i=0;i<8;i++) {
        assert(led_status_draw_begin_partial(16,2,16,4));s_image_dirty=true;
        assert(led_status_draw_done());
    }
    assert(s_partial_updates==9);
    failing=-1;assert(led_status_draw_begin_partial(16,2,16,4));s_image_dirty=true;
    assert(!led_status_draw_done() && s_partial_updates==9);
    failing=0;assert(led_status_draw_begin_partial(16,2,16,4));s_image_dirty=true;
    assert(led_status_draw_done() && s_partial_updates==0);
    memset(shown,255,sizeof(shown));memset(canvas,7,sizeof(canvas));
    assert(led_status_draw_begin_partial(16,2,16,4));led_status_draw_cancel_partial();
    for(int y=0;y<EPD_H;y++) for(int x=0;x<EPD_W;x++)
        assert(canvas[y*EPD_W+x]==((x>=16 && x<32 && y>=2 && y<6)?255:7));
    s_image_dirty=true;s_gray16_mode=true;assert(led_status_draw_done());
    assert(grays==1 && !s_partial_ready && !led_status_draw_begin_partial(16,2,16,4));
    return 0;
}
'''
        self.compile_production_check("partial_state", code)

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = (ROOT / "main/epaper_status.c").read_text()
        start = source.index("// ---- Pixels (host-tested)")
        (out / "epaper_pixels.inc").write_text(source[start:source.index("// ---- Panel", start)])
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmd = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(out),
               str(ROOT / "tests/link_epaper_status_harness.c"), "-o", str(out / "epaper")]
        compiled = subprocess.run(cmd, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_pixels(self):
        result = subprocess.run([str(self.out / "epaper")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sticky_board_wiring(self):
        overlay = (ROOT / "devices/sdkconfig.reterminal-sticky").read_text()
        for setting in ('CONFIG_IDF_TARGET="esp32s3"',
                        'CONFIG_MUSE_BOARD_RETERMINAL_STICKY=y',
                        'CONFIG_HOMEHUB_LED_BACKEND_MUSE=y',
                        'CONFIG_HOMEHUB_BUTTON_GPIO=4',
                        'CONFIG_MUSE_CJK_FONT=y',
                        'CONFIG_SPIRAM_MODE_OCT=y'):
            self.assertIn(setting, overlay)
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        self.assertIn("config HOMEHUB_LED_BACKEND_STICKY_SSD1677", kconfig)
        self.assertIn("default y if HOMEHUB_LED_BACKEND_STICKY_SSD1677", kconfig)
        self.assertIn('"epaper_status.c" "epd_ssd1677.c"', (ROOT / "main/CMakeLists.txt").read_text())
        helper = (ROOT / "tools/board.sh").read_text()
        self.assertIn("reterminal-sticky)", helper)
        self.assertIn('DEFAULTS="$DEFAULTS;devices/sdkconfig.muse;devices/sdkconfig.$BOARD"', helper)
        self.assertNotIn('build-$BOARD-full', helper)   # one build directory per board, as for the rest
        # Its USB-C reaches the console UART through a bridge: the Muse tools'
        # serial commands (>status, chat.py) read UART, as on the LCD7.
        for setting in ("CONFIG_ESP_CONSOLE_UART_DEFAULT=y", "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n"):
            self.assertIn(setting, overlay)
        console = (ROOT / "components/muse/Kconfig").read_text()
        console = console[console.index("config MUSE_CONSOLE_UART"):]
        self.assertIn("MUSE_BOARD_RETERMINAL_STICKY", console[:console.index("\n\n")])
        # The microSD mount is opt-in until something reads the card.
        kc = (ROOT / "components/muse/Kconfig").read_text()
        sd = kc[kc.index("config MUSE_STICKY_SD"):]
        self.assertIn("default n", sd[:sd.index("help")])
        sticky = (ROOT / "components/muse/boards/board_reterminal_sticky.c").read_text()
        task = sticky[:sticky.index("static void sd_task(")]
        self.assertTrue(task.rstrip().endswith("#if CONFIG_MUSE_STICKY_SD"))
        workflow = (ROOT.parent / ".github/workflows/esp32.yml").read_text()
        self.assertIn('board: reterminal-sticky\n            target: esp32s3\n'
                      '            overlays: ";devices/sdkconfig.muse;devices/sdkconfig.reterminal-sticky"',
                      workflow)
        self.assertIn('"boards/board_reterminal_sticky.c" "../../main/epd_ssd1677.c"',
                      (ROOT / "components/muse/CMakeLists.txt").read_text())
        board = (ROOT / "components/muse/boards/board_reterminal_sticky.c").read_text()
        for part in ('esp_lcd_touch_new_i2c_gt911', 'i2s_channel_init_pdm_rx_mode',
                     'esp_vfs_fat_sdspi_mount', 'gauge_read(0x2c', 'epd_panel_get',
                     'gpio_set_level(GPIO_NUM_45, 0)', '.display_pause = display_pause',
                     '.x_max = SCREEN_W', 'data->point.x = point.x',
                     'data->point.y = sticky_touch_y(point.y);'):
            self.assertIn(part, board)
        main = (ROOT / "main/main.c").read_text()
        self.assertLess(main.index("sticky_power_hold();"), main.index("diagnostic_log_init()"))
        self.assertIn("gpio_set_level(GPIO_NUM_45, 1)", main)
        self.assertIn("gpio_set_level(GPIO_NUM_46, 1)", main)
        sensor = (ROOT / "main/reterminal_sht4x.c").read_text()
        self.assertIn("SHT4X_SDA_GPIO 1", sensor)
        self.assertIn("SHT4X_SCL_GPIO 0", sensor)
        self.assertIn("SHT4X_I2C_PORT I2C_NUM_1", sensor)
        self.assertIn("CONFIG_HOMEHUB_LED_BACKEND_STICKY_SSD1677 || CONFIG_MUSE_BOARD_RETERMINAL_STICKY", sensor)
        self.assertIn("reTerminal Sticky*", (ROOT / "devices/README.md").read_text())
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertIn("font.fallback = &muse_font_cjk_16;", ui)
        self.assertIn("s_reply_lbl = make_label(face, font, COLOR_CAPTION);", ui)

    def test_sticky_link_profile_is_independent(self):
        overlay = (ROOT / "devices/sdkconfig.reterminal-sticky-link").read_text()
        self.assertIn("CONFIG_MUSE_BOARD_NONE=y", overlay)
        self.assertIn("CONFIG_HOMEHUB_LED_BACKEND_STICKY_SSD1677=y", overlay)
        self.assertIn("CONFIG_HOMEHUB_RETERMINAL_SHT4X=y", overlay)
        self.assertNotIn("CONFIG_MUSE_BOARD_RETERMINAL_STICKY=y", overlay)
        helper = (ROOT / "tools/board.sh").read_text()
        self.assertIn("reterminal-sticky-link)", helper)
        self.assertIn('DEFAULTS="$DEFAULTS;devices/sdkconfig.$BOARD"', helper)
        workflow = (ROOT.parent / ".github/workflows/esp32.yml").read_text()
        self.assertIn('board: reterminal-sticky-link\n            target: esp32s3\n'
                      '            overlays: ";devices/sdkconfig.reterminal-sticky-link"', workflow)

    def test_sticky_features_table_preserves_other_boards(self):
        docs = (ROOT / "devices/README.md").read_text()
        table = docs.split("## Features\n", 1)[1].split("*Sticky entries", 1)[0]
        rows = [line for line in table.splitlines() if line.startswith("|")]
        self.assertIn("reTerminal Sticky*", rows[0])
        self.assertIn("Waveshare LCD 1.28", rows[0])
        for row in rows[1:]:
            self.assertEqual(row.count("|"), rows[0].count("|"), row.split("|", 2)[1])
        columns = [[cell.strip() for cell in row.split("|")[1:-1]] for row in rows]
        features = {row[0]: row for row in columns[2:]}
        self.assertEqual(features["Buttons"][columns[0].index("Waveshare LCD 1.28")], "Screen only")
        self.assertEqual(features["Battery status"][columns[0].index("Waveshare LCD 1.28")],
                         "Voltage only (unverified)")
        self.assertEqual(features["UI and settings"][columns[0].index("reTerminal Sticky*")],
                         "Basic navigation checked")

    def test_sticky_settings_flash_uses_internal_stack(self):
        board = (ROOT / "components/muse/boards/board_reterminal_sticky.c").read_text()
        settings = (ROOT / "components/muse/muse_settings.c").read_text()
        self.assertIn('nvs_set_u16(s_nvs, "sleep_s", s.sleep_s)', settings)
        self.assertRegex(board, r'xTaskCreatePinnedToCoreWithCaps\(lvgl_task, "sticky_ui", 8192, NULL, '
                                r'MUSE_UI_PRIORITY,\s*&s_ui_task,\s*MUSE_UI_CORE,\s*'
                                r'MALLOC_CAP_INTERNAL\s*\|\s*MALLOC_CAP_8BIT\)')

    def test_sticky_settings_refreshes_are_epaper_safe(self):
        ui = (ROOT / "components/muse/muse_settings_ui.c").read_text()
        wifi = ui[ui.index("static void tick_wifi(void)"):ui.index("/* ---------- Hatch ---------- */")]
        sleep = ui[ui.index("static void on_sleep_choice("):ui.index("/* ---------- Battery ---------- */")]
        self.assertIn('if (muse_board->epaper)', wifi)
        self.assertIn('"Connected to %s\\n%s"', wifi)
        self.assertIn('tick_sleep();', sleep)
        self.assertIn('s_sleep_rows', sleep)
        self.assertIn('muse_board->epaper ? 30000000 : 1000000', ui)
        self.assertNotIn('ESP_LOGI(', sleep)   # no leftover debug log
        # No mic meter on e-paper, whether the Sound page opens or is ticked.
        self.assertIn('if (p == s_sound && !muse_board->epaper) {', ui)
        self.assertIn('visible && s_current == s_sound && !muse_board->epaper', ui)
        # A blinking cursor would keep the panel from ever settling to refresh.
        text = ui[ui.index("s_text_ta = lv_textarea_create(s_text);"):]
        self.assertIn("lv_obj_set_style_anim_duration(s_text_ta, 0, LV_PART_CURSOR);",
                      text[:text.index("lv_obj_align(s_text_ta")])

    def test_sticky_mic_only_board_shows_the_mic(self):
        # Sticky has a PDM mic (mic_init) and no codec (audio_init): once
        # paired, the on-screen mic carries its touch-to-talk.
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertIn("bool mic = paired && (muse_board->audio_init || muse_board->mic_init);", ui)
        self.assertIn("lv_obj_set_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN, !mic);", ui)
        # The speaker button stays a codec board's.
        self.assertIn("lv_obj_set_flag(s_speaker, LV_OBJ_FLAG_HIDDEN, !paired || !muse_board->audio_init);", ui)

    def test_sticky_reports_four_grays_to_muse(self):
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertIn("return muse_board->epaper ? 2 : 16;", ui)
        led = (ROOT / "main/led_status.c").read_text()
        self.assertIn("return muse_ui_image_size(&w, &h) ? muse_ui_image_bits() : 0;", led)

    def test_sticky_panel(self):
        panel = (ROOT / "main/epd_ssd1677.c").read_text()
        for name, pin in (("SCLK", 13), ("MOSI", 14), ("CS", 15), ("DC", 16),
                          ("RST", 17), ("BUSY", 18), ("EN", 47)):
            self.assertRegex(panel, rf"#define EPD_PIN_{name}\s+{pin}\b")
        self.assertIn("while (gpio_get_level(EPD_PIN_BUSY) == 1)", panel)
        self.assertIn(".bits_per_pixel  = 1,", panel)
        self.assertIn(".width           = EPD_LOGICAL_W,", panel)
        self.assertIn(".height          = EPD_LOGICAL_H,", panel)
        self.assertIn("#define EPD_SPI_HZ      (20 * 1000 * 1000)", panel)
        self.assertIn("epd_cmd(0x21, update_control, sizeof(update_control))", panel)
        self.assertIn("s_partial_prepared", panel)
        self.assertIn("if (err == ESP_OK && !full) err = epd_frame(0x26, frame)", panel)
        self.assertIn("if (err == ESP_OK && !full) err = epd_frame(0x24, frame)", panel)
        self.assertIn("full ? 0xF7 : 0xFF", panel)
        for command in ("0x26", "0x24", "0x22", "0x20"):
            self.assertIn(command, panel)

    def test_sticky_portrait_mapping(self):
        source = (ROOT / "main/epd_ssd1677.c").read_text()
        start = source.index("static void ssd1677_transpose_rows(")
        end = source.index("static void ssd1677_gray4_plane_rows(", start)
        harness = (self.out / "sticky_mapping.c")
        harness.write_text("#include <assert.h>\n#include <stdint.h>\n#include <stddef.h>\n"
                           "#include <string.h>\n" + source[start:end] + """
int main(void) {
    uint8_t src[16], dst[16];
    memset(src, 0xFF, sizeof(src));
    src[0] &= 0x7F;
    src[9] &= 0xEF;
    src[15] &= 0xFE;
    ssd1677_transpose_rows(dst, src, 0, 4, 8, 16);
    ssd1677_transpose_rows(dst + 8, src, 4, 4, 8, 16);
    assert(dst[0] == 0x7F && dst[1] == 0xFF);
    assert(dst[6] == 0xFF && dst[7] == 0xBF);
    assert(dst[14] == 0xFF && dst[15] == 0xFE);
    static uint8_t full_src[480 * 800 / 8], full_dst[480 * 800 / 8];
    memset(full_src, 0xFF, sizeof(full_src));
    full_src[0] &= 0x7F;
    full_src[799 * 60 + 59] &= 0xFE;
    full_src[200 * 60 + 2] &= 0xEF;
    for (int row = 0; row < 480; row += 40) {
        ssd1677_transpose_rows(full_dst + row * 100, full_src, row, 40, 480, 800);
    }
    assert(full_dst[0] == 0x7F);
    assert(full_dst[479 * 100 + 99] == 0xFE);
    assert(full_dst[19 * 100 + 25] == 0x7F);
    return 0;
}
""")
        cc = shlex.split(os.environ.get("CC", "cc"))
        binary = self.out / "sticky_mapping"
        compiled = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                   str(harness), "-o", str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sticky_gray4_planes(self):
        source = (ROOT / "main/epd_ssd1677.c").read_text()
        start = source.index("static void ssd1677_gray4_plane_rows(")
        end = source.index("static esp_err_t epd_frame(", start)
        harness = self.out / "sticky_gray4.c"
        harness.write_text("#include <assert.h>\n#include <stdint.h>\n#include <stddef.h>\n"
                           "#include <string.h>\n" + source[start:end] + """
int main(void) {
    uint8_t src[16] = {0xD8};
    uint8_t high[8], low[8];
    ssd1677_gray4_plane_rows(high, src, 0, 8, 8, 8, 1);
    ssd1677_gray4_plane_rows(low, src, 0, 8, 8, 8, 0);
    assert(high[0] == 0x7F && low[0] == 0x7F);
    assert(high[1] == 0xFF && low[1] == 0x7F);
    assert(high[2] == 0x7F && low[2] == 0xFF);
    assert(high[3] == 0xFF && low[3] == 0xFF);
    memset(src, 0, sizeof(src));
    ssd1677_gray4_plane_rows(high, src, 0, 8, 8, 8, 1);
    ssd1677_gray4_plane_rows(low, src, 0, 8, 8, 8, 0);
    for (int i = 0; i < 8; i++) assert(high[i] == 0xFF && low[i] == 0xFF);
    memset(src, 0xFF, sizeof(src));
    ssd1677_gray4_plane_rows(high, src, 0, 8, 8, 8, 1);
    ssd1677_gray4_plane_rows(low, src, 0, 8, 8, 8, 0);
    for (int i = 0; i < 8; i++) assert(high[i] == 0 && low[i] == 0);
    return 0;
}
""")
        cc = shlex.split(os.environ.get("CC", "cc"))
        binary = self.out / "sticky_gray4"
        compiled = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                   str(harness), "-o", str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("epd_gray4_frame(0x24, frame, 1)", source)
        self.assertIn("epd_gray4_frame(0x26, frame, 0)", source)
        self.assertIn("const uint8_t refresh = 0xD7", source)
        status = (ROOT / "main/epaper_status.c").read_text()
        self.assertIn("s_panel->update_gray4(s_gray4_frame)", status)
        self.assertIn("return s_panel->update_gray4 ? 2 : s_panel->bits_per_pixel", status)

    def test_sticky_touch_maps_screen_edges(self):
        board = (ROOT / "components/muse/boards/board_reterminal_sticky.c").read_text()
        self.assertIn("static uint16_t sticky_touch_y(", board)
        start = board.index("static uint16_t sticky_touch_y(")
        end = board.index("static void touch_read(", start)
        harness = self.out / "sticky_touch_y.c"
        harness.write_text("#include <assert.h>\n#include <stdint.h>\n#define SCREEN_H 800\n"
                           + board[start:end] + """
int main(void) {
    assert(sticky_touch_y(0) == 799);
    assert(sticky_touch_y(12) == 787);
    assert(sticky_touch_y(399) == 400);
    assert(sticky_touch_y(766) == 33);
    assert(sticky_touch_y(799) == 0);
    assert(sticky_touch_y(800) == 0);
    return 0;
}
""")
        cc = shlex.split(os.environ.get("CC", "cc"))
        binary = self.out / "sticky_touch_y"
        compiled = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                   str(harness), "-o", str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("data->point.x = point.x < SCREEN_W ? point.x : SCREEN_W - 1;", board)
        self.assertIn("data->point.y = sticky_touch_y(point.y);", board)

    def test_sticky_ui_gray4_pixels(self):
        source = (ROOT / "components/muse/boards/board_reterminal_sticky.c").read_text()
        start = source.index("static void rgb565_to_gray4(")
        end = source.index("static void epd_task(", start)
        harness = self.out / "sticky_ui_gray4.c"
        harness.write_text("#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n"
                           "#include <stddef.h>\n#include <string.h>\n" + source[start:end] + """
int main(void) {
    uint16_t src[] = {0x0000, 0x4208, 0x8410, 0xffff,
                      0xffff, 0xffff, 0xffff, 0xffff};
    uint8_t dst[2], shown[2];
    rgb565_to_gray4(dst, src, 4, 2);
    assert(dst[0] == 0x1b && dst[1] == 0xff);
    assert(gray4_needs_refresh(dst, shown, sizeof(dst), false));
    memcpy(shown, dst, sizeof(dst));
    assert(!gray4_needs_refresh(dst, shown, sizeof(dst), true));
    shown[1] = 0;
    assert(gray4_needs_refresh(dst, shown, sizeof(dst), true));
    /* Still for 700 ms, or changing for 3 s without a pause. */
    assert(!gray4_refresh_due(1000000, 900000, 900000));
    assert(gray4_refresh_due(1600000, 900000, 900000));
    assert(!gray4_refresh_due(3000000, 2800000, 400000));
    assert(gray4_refresh_due(3400000, 3200000, 400000));
    return 0;
}
""")
        cc = shlex.split(os.environ.get("CC", "cc"))
        binary = self.out / "sticky_ui_gray4"
        compiled = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                   str(harness), "-o", str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sticky_epd_task_sleeps_unless_settling(self):
        # Paused, or with nothing new, the panel task waits for the next flush
        # instead of waking every 250 ms; it starts by looking, in case LVGL
        # flushed before it existed.
        board = (ROOT / "components/muse/boards/board_reterminal_sticky.c").read_text()
        task = board[board.index("static void epd_task("):board.index("static void lvgl_task(")]
        self.assertIn("bool settling = true;", task)
        self.assertIn("ulTaskNotifyTake(pdTRUE, settling ? pdMS_TO_TICKS(250) : portMAX_DELAY);", task)
        self.assertIn("settling = s_dirty && !s_paused;", task)
        self.assertNotIn(".keyboard = true", board)   # touch boards have no button menu to label

    def test_muse_images_dither_to_four_grays(self):
        led = (ROOT / "main/led_status.c").read_text()
        done = led[led.index("bool led_status_draw_done(void) {"):]
        self.assertIn("muse_ui_image_done();", done[:done.index("\n}\n")])
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        start = ui.index("static void dither_gray4_565(")
        end = ui.index("void muse_ui_image_done(", start)
        harness = self.out / "muse_dither_gray4.c"
        harness.write_text("#include <assert.h>\n#include <stdint.h>\n#include <stddef.h>\n"
                           "#include <string.h>\n" + ui[start:end] + """
#define W 64
#define H 32
static int level(uint16_t p) {
    for (int i = 0; i < 4; i++) {
        static const uint16_t g[4] = {0x0000, 0x52AA, 0xAD55, 0xFFFF};
        if (p == g[i]) return i;
    }
    return -1;
}
int main(void) {
    static uint16_t px[W * H];
    int16_t err[2 * (W + 2)];
    /* The four grays come through exactly. */
    const uint16_t exact[4] = {0x0000, 0x52AA, 0xAD55, 0xFFFF};
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < W * H; k++) px[k] = exact[i];
        dither_gray4_565(px, W, H, err);
        for (int k = 0; k < W * H; k++) assert(px[k] == exact[i]);
    }
    /* Mid gray (128) between two of them: a mix of 85 and 170 averaging 128. */
    for (int k = 0; k < W * H; k++) px[k] = 0x8410;
    dither_gray4_565(px, W, H, err);
    long sum = 0;
    int seen[4] = {0};
    for (int k = 0; k < W * H; k++) {
        int l = level(px[k]);
        assert(l >= 0);
        seen[l]++;
        sum += l * 85;
    }
    assert(seen[1] > 0 && seen[2] > 0 && seen[0] == 0 && seen[3] == 0);
    long avg = sum / (W * H);
    assert(avg >= 120 && avg <= 136);
    return 0;
}
""")
        cc = shlex.split(os.environ.get("CC", "cc"))
        binary = self.out / "muse_dither_gray4"
        compiled = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                   str(harness), "-o", str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_color_rect_decodes_big_endian_pixels(self):
        source = (ROOT / "main/epaper_status.c").read_text()
        color = source[source.index("if (EPD_COLOR) {", source.index("bool led_status_draw_rect(")):]
        self.assertIn("rgb565_from_be(src)", color.split("} else {", 1)[0])

    def test_draw_url_states_the_bit_depth(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_DISPLAY_COMMANDS")
        block = noise[start:noise.index('"display.show_animation"', start)]
        self.assertIn("int bits = led_status_display_bits();", block)
        self.assertIn("1 bit per pixel", block)
        self.assertIn("16 bits per pixel (RGB565)", block)

    def test_draw_url_states_the_six_colour_inks(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_DISPLAY_COMMANDS")
        block = noise[start:noise.index('"display.show_animation"', start)]
        self.assertIn("bits == 4", block)
        self.assertIn("six-colour e-paper screen (E Ink Spectra 6)", block)
        self.assertIn("4 bits per pixel", block)
        # Every ink the driver dithers to is named with its exact colour.
        driver = (ROOT / "main/epaper_status.c").read_text()
        inks = re.findall(r"\{\d, (\d+), (\d+), (\d+)\},\s+// (\w+)", driver)
        self.assertEqual(len(inks), 6)
        text = re.sub(r'"\s*"', "", block)
        for r, g, b, name in inks:
            self.assertIn(f"{name} #{int(r):02x}{int(g):02x}{int(b):02x}", text)
        self.assertIn("led_status_display_bits", driver)
        self.assertIn("return s_panel->update_gray4 ? 2 : s_panel->bits_per_pixel;", driver)
        for panel, bits in (("epd_spectra6.c", 4), ("epd_uc8179.c", 1),
                            ("epd_ssd1681.c", 1), ("epd_ssd1677.c", 1)):
            self.assertRegex((ROOT / "main" / panel).read_text(),
                             rf"\.bits_per_pixel\s*=\s*{bits},", panel)

    def test_epaper_replaces_led_status_and_refreshes_after_each_image(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        # Each e-paper backend builds the status screen with its panel driver.
        self.assertRegex(cmake, r'if\(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c" "epd_uc8179.c"\)\s*'
                                r'elseif\(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c" "epd_spectra6.c"\)\s*'
                                r'elseif\(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c" "epd_it8951.c"\)\s*'
                                r'elseif\(CONFIG_HOMEHUB_LED_BACKEND_STICKY_SSD1677\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c" "epd_ssd1677.c"\)\s*'
                                r'elseif\(CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_SSD1681\)\s*'
                                r'list\(APPEND GADGET_SRCS "epaper_status.c" "epd_ssd1681.c"\)\s*'
                                r'else\(\)\s*'
                                r'list\(APPEND GADGET_SRCS "led_status.c"\)')
        # Both implementations provide the whole display interface.
        header = (ROOT / "main/led_status.h").read_text()
        for name in re.findall(r"^\w[\w ]*?\b(led_status_\w+)\(", header, re.M):
            if name in ("led_status_set_voice", "led_status_set_level", "led_status_show_volume"):
                continue
            for impl in ("led_status.c", "epaper_status.c"):
                self.assertRegex((ROOT / "main" / impl).read_text(), rf"\n\w[\w ]*\b{name}\(",
                                 f"{impl} lacks {name}")
        fetch = (ROOT / "main/image_fetch.c").read_text()
        task = fetch[fetch.index("static void fetch_task("):]
        self.assertLess(task.index("draw_raw(f, &rows)"), task.index("led_status_draw_done()"))


if __name__ == "__main__":
    unittest.main()
