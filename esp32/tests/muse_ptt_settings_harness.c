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
#include <assert.h>
#include "muse_settings.c"
#include "muse_mic_gain_word.h"
#include "muse_vol_word.h"
#include "muse_vol_show.h"

static bool saved, value;
static int commits, notices;
/* Developer mode's key: stored only while it's on. */
static bool dev_saved;
static int closes;
/* The mic gain's key. */
static bool gain_saved;
static uint8_t gain_value;
static int gain_writes;
/* The speaker's keys (the bench volume: vol= never writes them). */
static bool vol_saved, spk_saved;
static uint8_t vol_value, spk_value;
static int vol_writes;
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { assert(!"must preserve existing NVS"); return ESP_FAIL; }
esp_err_t nvs_open(const char *ns,nvs_open_mode_t mode,nvs_handle_t *h) {
    assert(!strcmp(ns,"muse") && mode==NVS_READWRITE); *h=1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v) {
    assert(h==1);
    if(!strcmp(key,"voice_notes") && saved) { *v=value; return ESP_OK; }
    if(!strcmp(key,"developer") && dev_saved) { *v=1; return ESP_OK; }
    if(!strcmp(key,"mic_gain") && gain_saved) { *v=gain_value; return ESP_OK; }
    if(!strcmp(key,"volume") && vol_saved) { *v=vol_value; return ESP_OK; }
    if(!strcmp(key,"speaker") && spk_saved) { *v=spk_value; return ESP_OK; }
    return ESP_FAIL;
}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {
    assert(h==1 && !strcmp(key,"developer"));
    if(!dev_saved) return ESP_FAIL;   /* not found */
    dev_saved=false; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h==1); ++closes; }
esp_err_t nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *v) { (void)h;(void)key;(void)v;return ESP_FAIL; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *v,size_t *n) { (void)h;(void)key;(void)v;(void)n;return ESP_FAIL; }
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v) {
    assert(h==1);
    if(!strcmp(key,"developer")) { assert(v==1); dev_saved=true; return ESP_OK; }
    if(!strcmp(key,"mic_gain")) { gain_saved=true; gain_value=v; ++gain_writes; return ESP_OK; }
    if(!strcmp(key,"volume")) { vol_saved=true; vol_value=v; ++vol_writes; return ESP_OK; }
    if(!strcmp(key,"speaker")) { spk_saved=true; spk_value=v; ++vol_writes; return ESP_OK; }
    assert(!strcmp(key,"voice_notes")); saved=true; value=v; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h==1);
    ++commits; return ESP_OK;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static void changed(muse_setting_t what) { assert(what==MUSE_SETTING_VOICE_NOTES || what==MUSE_SETTING_DEVELOPER); ++notices; }
/* A boot's view: the RAM defaults again (a fresh process), the NVS as it was left. */
static void boot(void) { s.mic_gain=MUSE_MIC_GAIN_TUNED; assert(muse_settings_init()==ESP_OK); }
/* The stored mic gain is the user's: every value boots as stored, and a fresh NVS boots at the default. */
static int migration(void) {
    gain_saved=true; gain_value=36;
    boot();
    assert(muse_settings_mic_gain()==36 && gain_writes==0);
    muse_settings_set_mic_gain(12);   /* the user's own choice, in Settings */
    assert(gain_value==12 && gain_writes==1);
    boot();
    assert(muse_settings_mic_gain()==12 && gain_writes==1);
    gain_saved=false; gain_writes=0;
    boot();
    assert(muse_settings_mic_gain()==MUSE_MIC_GAIN_TUNED && !gain_saved && gain_writes==0);
    char t[96];
    /* The console words: mic.gain? and mic.gain=N, clamped to 0..36 on the slider's 3 dB steps. */
    static const struct { const char *line; muse_mic_word_t w; int db; } words[] = {
        { "mic.gain?", MUSE_MIC_WORD_QUERY, -1 }, { "mic.gain=30", MUSE_MIC_WORD_SET, 30 },
        { "mic.gain=0", MUSE_MIC_WORD_SET, 0 }, { "mic.gain=36", MUSE_MIC_WORD_SET, 36 },
        { "mic.gain=99", MUSE_MIC_WORD_SET, 36 }, { "mic.gain=-5", MUSE_MIC_WORD_SET, 0 },
        { "mic.gain=31", MUSE_MIC_WORD_SET, 30 }, { "mic.gain=32", MUSE_MIC_WORD_SET, 33 },
        { "mic.gain=35", MUSE_MIC_WORD_SET, 36 }, { "mic.gain=", MUSE_MIC_WORD_BAD, -1 },
        { "mic.gain=3x", MUSE_MIC_WORD_BAD, -1 }, { "mic.gain=99999999", MUSE_MIC_WORD_BAD, -1 },
        { "mic.gain", MUSE_MIC_WORD_NONE, -1 }, { "mic.gains=3", MUSE_MIC_WORD_NONE, -1 },
        { "vib?", MUSE_MIC_WORD_NONE, -1 },
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        int v = -1;
        muse_mic_word_t w = muse_mic_gain_word(words[i].line, &v);
        if (w != words[i].w || (w == MUSE_MIC_WORD_SET && v != words[i].db)) {
            fprintf(stderr, "%s: %d %d\n", words[i].line, w, v);
            return 1;
        }
    }
    muse_mic_gain_status(t, sizeof(t), 30, -1, 30);
    assert(!strcmp(t, "mic gain: settings 30 dB, connection none, effective 30 dB"));
    muse_mic_gain_status(t, sizeof(t), 30, 21, 21);
    assert(!strcmp(t, "mic gain: settings 30 dB, connection 21 dB, effective 21 dB"));
    return 0;
}
/* The bench volume (bench and test mutes are RAM-only):
 * vol=N never writes NVS, a reboot comes back at the saved volume, vol=saved restores it, and a user's
 * or the app's change ends it, saved over the saved values (never over the bench's). */
static int vol_notices;
static void vol_changed(muse_setting_t what) { assert(what==MUSE_SETTING_VOLUME || what==MUSE_SETTING_SPEAKER); ++vol_notices; }
static void vboot(void) {   /* a fresh process: the RAM defaults, the NVS as it was left */
    s.volume=CONFIG_MUSE_DEFAULT_VOLUME; s.speaker_on=true; s_vol_ram=false;
    assert(muse_settings_init()==ESP_OK);
}
static int now_vol(void) { return muse_settings_speaker_on() ? muse_settings_volume() : 0; }
static int bench_volume(void) {
    vol_saved=spk_saved=true; vol_value=40; spk_value=1;
    vboot();
    muse_settings_set_listener(vol_changed);
    assert(now_vol()==40 && !muse_settings_volume_is_ram() && muse_settings_saved_speaker_volume()==40);
    assert(vol_notices==0);                       /* the boot's restore from NVS isn't a change: no notice */
    int c0=commits;
    /* vol=0: off in RAM, the level kept; nothing written; one notice. */
    muse_settings_set_volume_ram(0);
    assert(now_vol()==0 && muse_settings_volume()==40 && !muse_settings_speaker_on());
    assert(muse_settings_volume_is_ram() && muse_settings_saved_speaker_volume()==40);
    assert(vol_writes==0 && commits==c0 && vol_notices==1);
    /* vol=25 on top: still RAM, the saved values unchanged. */
    muse_settings_set_volume_ram(25);
    assert(now_vol()==25 && muse_settings_saved_speaker_volume()==40 && vol_writes==0 && commits==c0 && vol_notices==2);
    /* A reboot comes back at the saved volume. */
    vboot();
    assert(now_vol()==40 && !muse_settings_volume_is_ram() && vol_value==40 && spk_value==1);
    /* vol=saved: back, one notice; again: nothing to do, no notice. */
    muse_settings_set_volume_ram(0);
    int n=vol_notices;
    muse_settings_volume_restore();
    assert(now_vol()==40 && !muse_settings_volume_is_ram() && vol_notices==n+1 && vol_writes==0 && commits==c0);
    muse_settings_volume_restore();
    assert(vol_notices==n+1);
    /* A user's mute during a bench volume (the speaker key): the saved values come back first, then the
     * mute is saved: the saved level stays 40, never the bench's 10. */
    muse_settings_set_volume_ram(10);
    muse_settings_set_speaker_on(false);
    assert(!muse_settings_volume_is_ram() && muse_settings_volume()==40 && now_vol()==0);
    assert(vol_writes==1 && spk_value==0 && vol_value==40 && muse_settings_saved_speaker_volume()==0);
    /* ...and back on from a bench 0: the saved level. */
    muse_settings_set_volume_ram(0);
    muse_settings_set_speaker_on(true);
    assert(now_vol()==40 && spk_value==1 && vol_writes==2 && !muse_settings_volume_is_ram());
    /* The app's write (set_settings speaker_volume 70, mg_glue.c: the level, then the switch) during a
     * bench 0: saved, as the user's phone setting. */
    muse_settings_set_volume_ram(0);
    muse_settings_set_volume(70);
    muse_settings_set_speaker_on(true);
    assert(now_vol()==70 && vol_value==70 && spk_value==1 && !muse_settings_volume_is_ram());
    assert(muse_settings_saved_speaker_volume()==70);
    vboot();
    assert(now_vol()==70);
    /* Who changed it (the gadget shows every change it didn't make itself). The UI's watch
     * (muse_vol_show.h) sees the count move and the latest source. A boot's restore counts nothing. */
    muse_vol_by_t by = MUSE_VOL_BY_DEVICE;
    uint32_t c = muse_settings_volume_changes(&by);
    muse_vol_watch_t w = { .seen = c };
    vboot();
    assert(muse_settings_volume_changes(NULL)==c && muse_vol_watch(&w, c, by)==NULL);   /* boot restore: nothing shown */
    muse_settings_set_volume_by(30, MUSE_VOL_BY_PHONE);
    muse_settings_set_speaker_on_by(true, MUSE_VOL_BY_PHONE);   /* the app's set: two writes... */
    c = muse_settings_volume_changes(&by);
    assert(by==MUSE_VOL_BY_PHONE && vol_value==30);             /* ...saved, as the user's phone setting */
    const char *line = muse_vol_watch(&w, c, by);
    assert(line && !strcmp(line, "Set by phone"));              /* ...shown once */
    assert(muse_vol_watch(&w, c, by)==NULL);
    muse_settings_set_volume_ram(0);
    c = muse_settings_volume_changes(&by);
    line = muse_vol_watch(&w, c, by);
    assert(by==MUSE_VOL_BY_BENCH && line && !strcmp(line, "Set by bench"));
    muse_settings_volume_restore();                             /* a bench restore shows too */
    c = muse_settings_volume_changes(&by);
    line = muse_vol_watch(&w, c, by);
    assert(by==MUSE_VOL_BY_BENCH && line && !strcmp(line, "Set by bench") && now_vol()==30);
    muse_settings_set_speaker_on(false);                        /* the gadget's own key: its own UI, no source line */
    c = muse_settings_volume_changes(&by);
    assert(by==MUSE_VOL_BY_DEVICE && muse_vol_watch(&w, c, by)==NULL);
    assert(muse_vol_muted(muse_settings_speaker_on(), muse_settings_volume()));     /* muted: the icon shows */
    muse_settings_set_speaker_on(true);
    assert(!muse_vol_muted(muse_settings_speaker_on(), muse_settings_volume()));    /* raised: it goes */
    assert(muse_vol_muted(true, 0) && !muse_vol_muted(true, 1));
    /* The words. */
    static const struct { const char *line; muse_vol_word_t w; int pct; } words[] = {
        { "vol?", MUSE_VOL_WORD_SHOW, -1 }, { "vol=0", MUSE_VOL_WORD_SET, 0 }, { "vol=100", MUSE_VOL_WORD_SET, 100 },
        { "vol=40", MUSE_VOL_WORD_SET, 40 }, { "vol=007", MUSE_VOL_WORD_SET, 7 }, { "vol=saved", MUSE_VOL_WORD_SAVED, -1 },
        { "vol=101", MUSE_VOL_WORD_BAD, -1 }, { "vol=-1", MUSE_VOL_WORD_BAD, -1 }, { "vol=", MUSE_VOL_WORD_BAD, -1 },
        { "vol=1000", MUSE_VOL_WORD_BAD, -1 }, { "vol=5 ", MUSE_VOL_WORD_BAD, -1 }, { "vol= 5", MUSE_VOL_WORD_BAD, -1 },
        { "vol=Saved", MUSE_VOL_WORD_BAD, -1 }, { "vol=saved2", MUSE_VOL_WORD_BAD, -1 }, { "vol", MUSE_VOL_WORD_BAD, -1 },
        { "vol??", MUSE_VOL_WORD_BAD, -1 }, { "vol!", MUSE_VOL_WORD_BAD, -1 },
        { "volume=5", MUSE_VOL_WORD_NONE, -1 }, { "vols=5", MUSE_VOL_WORD_NONE, -1 }, { "vol.x=1", MUSE_VOL_WORD_NONE, -1 },
        { "vib?", MUSE_VOL_WORD_NONE, -1 }, { "", MUSE_VOL_WORD_NONE, -1 },
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        int v = -1;
        muse_vol_word_t w = muse_vol_word(words[i].line, &v);
        if (w != words[i].w || (w == MUSE_VOL_WORD_SET && v != words[i].pct)) {
            fprintf(stderr, "%s: %d %d\n", words[i].line, w, v);
            return 1;
        }
    }
    char t[48];
    muse_vol_text(t, sizeof(t), true, 0, 40);
    assert(!strcmp(t, "vol 0 (ram; saved 40)"));
    muse_vol_text(t, sizeof(t), false, 40, 40);
    assert(!strcmp(t, "vol 40 saved 40"));
    assert(!strcmp(MUSE_VOL_USAGE, "vol=N (0-100, RAM only), vol=saved or vol?"));
    return 0;
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"2")) return migration();
    if(!strcmp(argv[1],"3")) return bench_volume();
    assert(muse_settings_init()==ESP_OK);
    assert(!muse_settings_voice_notes() && !saved && !commits);
    muse_settings_set_listener(changed);
    muse_settings_set_voice_notes(true);
    assert(muse_settings_voice_notes() && saved && value && commits==1 && notices==1);
    atomic_store(&s_voice_notes,false); /* fresh process's zero-initialized default */
    assert(muse_settings_init()==ESP_OK);
    assert(muse_settings_voice_notes()==(CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE));
    muse_settings_set_voice_notes(false);
    assert(!muse_settings_voice_notes() && !value && commits==2 && notices==2);
    /* Developer mode: off by default, kept across a restart, off again by
     * its switch (the key goes) or by a factory reset. */
    assert(!muse_settings_developer() && !dev_saved);
    muse_settings_set_developer(true);
    assert(muse_settings_developer() && dev_saved && commits==3 && notices==3);
    s.developer=false; /* a fresh boot */
    assert(muse_settings_init()==ESP_OK && muse_settings_developer());
    muse_settings_set_developer(false);
    assert(!muse_settings_developer() && !dev_saved && commits==4 && notices==4);
    s.developer=false;
    assert(muse_settings_init()==ESP_OK && !muse_settings_developer());
    muse_settings_set_developer(true);
    muse_settings_factory_reset();
    assert(!muse_settings_developer() && !dev_saved && closes==1);
    s.developer=false;
    assert(muse_settings_init()==ESP_OK && !muse_settings_developer());
    return 0;
}
