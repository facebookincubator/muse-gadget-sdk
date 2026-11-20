/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "muse_motion_core.h"
#include "muse_motion.h"
#include "muse_board.h"
#include "qmi8658_motion.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool lift, quiet, asleep, enabled;
static int enable_calls, reads;
static esp_err_t enable_error, read_error;
static muse_motion_sample_t sample;
static bool speaker_on = true, expect_silence;
static int audio_writes;
bool muse_settings_lift_wake(void) { return lift; }
bool muse_settings_face_down_quiet(void) { return quiet; }
bool muse_settings_speaker_on(void) { return speaker_on; }
bool muse_state_asleep(void) { return asleep; }
static esp_err_t enable(bool on) { enabled = on; enable_calls++; return enable_error; }
static esp_err_t read_sample(muse_motion_sample_t *out)
{
    reads++;
    *out = sample;
    return read_error;
}
static const muse_board_t board = { .motion_enable = enable, .motion_read = read_sample };
const muse_board_t *muse_board = &board;

#define MUSE_AUDIO_RATE 16000
#define MUSE_AUDIO_CHUNK 8
#define CHANNELS 2
#define ESP_CODEC_DEV_OK 0
static void *s_spk;
static int16_t s_out_stereo[16];
static int esp_codec_dev_write(void *dev, const void *data, int bytes)
{
    (void)dev;
    const int16_t *pcm = data;
    for (int i = 0; i < bytes / (int)sizeof(*pcm); i++) assert(pcm[i] == (expect_silence ? 0 : 123));
    audio_writes++;
    return ESP_CODEC_DEV_OK;
}
static bool mirror_only_test;
static bool muse_mirror_audio_playback(const int16_t *pcm, size_t n)
{ (void)pcm; (void)n; return mirror_only_test; }
#include "motion_audio_write.inc"

static uint8_t regs[256];
static int writes, transactions, delay_ms;
static esp_err_t bus_error;
void vTaskDelay(int ms) { delay_ms += ms; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                   i2c_master_dev_handle_t *dev)
{
    assert(bus == (void *)1);
    assert(cfg->device_address == 0x6b && cfg->scl_speed_hz == 400000);
    *dev = (void *)2;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t n, int timeout)
{
    assert(dev == (void *)2 && n == 2 && timeout == 20);
    writes++;
    if (bus_error) return bus_error;
    regs[data[0]] = data[1];
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wn,
                                    uint8_t *r, size_t rn, int timeout)
{
    assert(dev == (void *)2 && wn == 1 && timeout == 20);
    transactions++;
    if (bus_error) return bus_error;
    memcpy(r, regs + *w, rn);
    return ESP_OK;
}

static const muse_motion_sample_t up = {0, 0, 1000}, down = {0, 0, -1000}, upright = {0, 1000, 0};

static void dwell(muse_motion_core_t *s, muse_motion_sample_t a, uint32_t start, int n)
{
    for (int i = 0; i < n; i++) {
        bool wake = muse_motion_update(s, a, start + i*100);
        if (wake) fprintf(stderr, "unexpected wake at %u ms (dwell starting %u)\n", start+i*100, start);
        assert(!wake);
    }
}

static void core_test(void)
{
    muse_motion_core_t s = {0};
    dwell(&s, up, 0, 20);                         /* no wake on boot or repeated pose */
    assert(s.pose == MUSE_POSE_UP);
    /* A normal pickup can start face up and settle at a viewing angle. */
    muse_motion_sample_t tilted = {600, 0, 800};
    assert(!muse_motion_update(&s, tilted, 2000));
    assert(!muse_motion_update(&s, tilted, 2039));
    assert(muse_motion_update(&s, tilted, 2040)); /* two fresh sensor frames */
    dwell(&s, tilted, 2400, 8);                   /* one event, not repeated */
    dwell(&s, down, 3200, 7);
    assert(s.pose == MUSE_POSE_DOWN);
    muse_motion_sample_t edge = {700, 0, -700};
    dwell(&s, edge, 3900, 8);                    /* hysteresis avoids chatter */
    assert(s.pose == MUSE_POSE_DOWN);
    /* Saturation/freefall cannot arm a lift. */
    assert(!muse_motion_update(&s, (muse_motion_sample_t){INT16_MIN,INT16_MIN,INT16_MIN}, 4700));
    dwell(&s, down, 4800, 5);
    muse_motion_reset(&s);
    dwell(&s, upright, 0, 7);
    dwell(&s, up, 2000, 5);                      /* stale history cannot trigger */
    muse_motion_reset(&s);
    dwell(&s, down, UINT32_MAX-800, 7);
    assert(!muse_motion_update(&s, up, UINT32_MAX-20));
    assert(!muse_motion_update(&s, up, UINT32_MAX));
    assert(muse_motion_update(&s, up, 19));       /* clock rollover */
    muse_motion_reset(&s);
    for (int i = 0; i < 30; i++)
        assert(!muse_motion_update(&s, i%2 ? down : up, i*100));
    assert(s.pose == MUSE_POSE_UNKNOWN);         /* rapid flipping never settles */

    /* Regression: intermediate angles must not discard the pickup. */
    muse_motion_reset(&s);
    dwell(&s, down, 0, 7);
    assert(!muse_motion_update(&s, (muse_motion_sample_t){600,500,-620}, 700));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){600,600,500}, 800));
    assert(muse_motion_update(&s, (muse_motion_sample_t){400,500,760}, 900));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){400,500,760}, 950));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){400,500,760}, 1000));
    /* Two upward-impulse samples wake a readable device without mistaking
     * its later braking phase for a new put-down. */
    muse_motion_reset(&s);
    dwell(&s, up, 0, 7);
    assert(!muse_motion_update(&s, (muse_motion_sample_t){0,0,1200}, 700));
    assert(muse_motion_update(&s, (muse_motion_sample_t){0,0,1200}, 750));
    assert(!muse_motion_update(&s, up, 800));
    assert(!muse_motion_update(&s, up, 850));
    assert(!muse_motion_update(&s, up, 900));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){0,0,800}, 950));
    assert(!s.lowering);                         /* upward braking is not a put-down */
    /* Low-rate replay also detects a gradual pickup. */
    muse_motion_reset(&s);
    for (int t = 0; t <= 750; t += 250) assert(!muse_motion_update(&s, down, t));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){500,500,-700}, 1000));
    assert(!muse_motion_update(&s, tilted, 1250));
    assert(muse_motion_update(&s, tilted, 1500));
    /* Stationary sensor noise must not look like a pickup. */
    muse_motion_reset(&s);
    for (int t = 0; t < 30000; t += 250) {
        int n = (t / 250) % 5;
        assert(!muse_motion_update(&s, (muse_motion_sample_t){-140+n*5,45-n*4,1000+n*3}, t));
    }
}

static void runtime_test(void)
{
    sample = up;
    assert(!muse_motion_tick(0));
    assert(!muse_motion_active() && enable_calls == 0 && reads == 0);
    quiet = true;
    sample = down;
    for (int t = 100; t <= 800; t += 100) assert(!muse_motion_tick(t));
    assert(enabled && muse_motion_active() && muse_motion_quiet());
    quiet = false;
    lift = true;                                 /* keep hardware active, unmute */
    assert(!muse_motion_tick(900) && !muse_motion_quiet());
    sample = up;
    assert(!muse_motion_tick(1000));
    assert(muse_motion_tick(1050));
    assert(!muse_motion_tick(1100));
    asleep = true;
    int previous = reads;
    assert(muse_motion_poll_ms() == 50);
    assert(!muse_motion_tick(1110) && reads == previous);
    assert(!muse_motion_tick(1150) && reads == previous + 1);
    lift = false;
    assert(!muse_motion_tick(1600));
    assert(!enabled && !muse_motion_active() && !muse_motion_quiet());
    quiet = true;
    assert(muse_motion_poll_ms() == 250);        /* quiet-only keeps slow polling */
    sample = down;
    for (int t = 2000; t <= 2750; t += 250) assert(!muse_motion_tick(t));
    assert(muse_motion_quiet());
    read_error = ESP_ERR_TIMEOUT;
    assert(!muse_motion_tick(3000));
    assert(!muse_motion_active() && !muse_motion_quiet() && !enabled);
    previous = reads;
    assert(!muse_motion_tick(4000) && reads == previous); /* no fault retry loop */
    quiet = false;
    muse_motion_tick(4100);
    quiet = true;
    read_error = ESP_ERR_NOT_FINISHED;
    muse_motion_tick(4200);
    assert(muse_motion_active());
    muse_motion_tick(5200);                       /* stalled sensor fails open */
    assert(!muse_motion_active() && !muse_motion_quiet());
    quiet = false;
    muse_motion_tick(5300);
    quiet = true;
    enable_error = ESP_FAIL;
    muse_motion_tick(5400);
    assert(!muse_motion_active() && !muse_motion_quiet());
}

static void driver_test(void)
{
    muse_motion_sample_t a = {123,123,123};
    assert(muse_qmi8658_read(&a) == ESP_ERR_INVALID_STATE);
    assert(muse_qmi8658_enable((void *)1, false) == ESP_OK && writes == 0);
    assert(muse_qmi8658_enable((void *)1, true) == ESP_ERR_NOT_FOUND && writes == 0);
    regs[0] = 5;
    assert(muse_qmi8658_enable((void *)1, true) == ESP_ERR_INVALID_RESPONSE);
    regs[0x4d] = 0x80;
    assert(muse_qmi8658_enable((void *)1, true) == ESP_OK);
    assert(regs[2] == 0x40 && regs[3] == 0x0d && regs[8] == 0x21 && delay_ms >= 20);
    int previous = transactions;
    assert(muse_qmi8658_read(&a) == ESP_ERR_NOT_FINISHED && transactions == previous+1);
    assert(a.x == 123);                          /* no stale output on no-data */
    regs[0x2e] = 1;
    regs[0x35] = 0; regs[0x36] = 0x40;           /* +1 g */
    regs[0x37] = 0; regs[0x38] = 0xc0;           /* -1 g */
    regs[0x39] = 0xff; regs[0x3a] = 0x7f;
    assert(muse_qmi8658_read(&a) == ESP_OK);
    assert(a.x == 1000 && a.y == -1000 && a.z == 1999);
    assert(muse_qmi8658_enable((void *)1, false) == ESP_OK && regs[8] == 0);
    bus_error = ESP_ERR_TIMEOUT;
    assert(muse_qmi8658_enable((void *)1, true) == ESP_ERR_TIMEOUT);
    assert(muse_qmi8658_read(&a) == ESP_ERR_TIMEOUT);
}

static void audio_test(void)
{
    int16_t pcm[24];
    for (int i = 0; i < 24; i++) pcm[i] = 123;
    quiet = true;
    sample = down;
    for (int t = 0; t <= 700; t += 100) muse_motion_tick(t);
    assert(muse_motion_quiet() && speaker_on);
    expect_silence = true;
    assert(muse_audio_write(pcm, 24) == ESP_OK && audio_writes == 3);
    sample = up;
    for (int t = 800; t <= 1200; t += 100) muse_motion_tick(t);
    assert(!muse_motion_quiet() && speaker_on);
    expect_silence = false;
    assert(muse_audio_write(pcm, 24) == ESP_OK);
    mirror_only_test = true;
    expect_silence = true;
    assert(muse_audio_write(pcm, 24) == ESP_OK);
    mirror_only_test = false;
    speaker_on = false;
    expect_silence = true;
    assert(muse_audio_write(pcm, 24) == ESP_OK);  /* saved mute still wins */
}

static void sleep_test(void)
{
    lift = true;
    sample = up;
    for (int t = 0; t <= 600; t += 100) assert(!muse_motion_tick(t));
    sample = (muse_motion_sample_t){600,0,800};
    assert(!muse_motion_tick(700));               /* pickup began while awake */
    asleep = true;                              /* explicit Sleep cancels it */
    assert(!muse_motion_tick(800));
    for (int t = 950; t <= 1450; t += 250) assert(!muse_motion_tick(t));
    sample = upright;                           /* a new pickup wakes once */
    assert(!muse_motion_tick(1700));
    assert(muse_motion_tick(1750));
    assert(!muse_motion_tick(1800));
    assert(!muse_motion_tick(2200));
    assert(!muse_motion_tick(2450));
    muse_motion_status();
}

static void lowering_test(void)
{
    muse_motion_core_t s = {0};
    /* Set it down from a viewing angle onto a face-up surface. */
    dwell(&s, upright, 0, 7);
    muse_motion_sample_t lowering[] = {
        {500,500,700},{500,500,700},{300,200,930},{300,200,930},up,up,up,up,
    };
    for (unsigned i = 0; i < sizeof(lowering)/sizeof(*lowering); i++)
        assert(!muse_motion_update(&s, lowering[i], 700+i*50));
    dwell(&s, up, 1100, 5);
    /* Lowering then braking/landing cannot be mistaken for an upward impulse. */
    assert(!muse_motion_update(&s, (muse_motion_sample_t){0,0,850}, 1600));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){0,0,1200}, 1650));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){0,0,1200}, 1700));
    dwell(&s, up, 1750, 5);
    /* A lone bump while resting is not a vertical pickup. */
    assert(!muse_motion_update(&s, (muse_motion_sample_t){0,0,1200}, 2300));
    dwell(&s, up, 2350, 5);
    /* Start lifting, then reverse before confirmation: do not wake on landing. */
    muse_motion_reset(&s);
    dwell(&s, up, 0, 7);
    assert(!muse_motion_update(&s, (muse_motion_sample_t){600,0,800}, 700));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){492,0,870}, 750));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){400,0,916}, 800));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){200,0,980}, 850));
    for (int t = 900; t <= 1600; t += 50) assert(!muse_motion_update(&s, up, t));
    /* The next real pickup still wakes promptly. */
    assert(!muse_motion_update(&s, (muse_motion_sample_t){600,0,800}, 1700));
    assert(muse_motion_update(&s, (muse_motion_sample_t){600,0,800}, 1750));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){600,0,800}, 1800));
    /* Downward acceleration followed by a tilted landing must rebase, not
     * produce a postponed wake once its suppression period ends. */
    muse_motion_reset(&s);
    dwell(&s, up, 0, 7);
    assert(!muse_motion_update(&s, (muse_motion_sample_t){240,90,705}, 700));
    for (int t = 750; t <= 1700; t += 50)
        assert(!muse_motion_update(&s, (muse_motion_sample_t){600,0,800}, t));
    for (int t = 1750; t <= 2300; t += 50) assert(!muse_motion_update(&s, up, t));
    assert(!muse_motion_update(&s, (muse_motion_sample_t){600,0,800}, 2350));
    assert(muse_motion_update(&s, (muse_motion_sample_t){600,0,800}, 2400));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "replay")) {
        muse_motion_core_t s = {0};
        unsigned ms;
        int x, y, z;
        while (scanf("%u %d %d %d", &ms, &x, &y, &z) == 4) {
            muse_motion_sample_t a = {x,y,z};
            if (muse_motion_update(&s, a, ms)) printf("%u\n", ms);
        }
    }
    else if (!strcmp(argv[1], "core")) core_test();
    else if (!strcmp(argv[1], "runtime")) runtime_test();
    else if (!strcmp(argv[1], "driver")) driver_test();
    else if (!strcmp(argv[1], "audio")) audio_test();
    else if (!strcmp(argv[1], "sleep")) sleep_test();
    else if (!strcmp(argv[1], "lowering")) lowering_test();
    else assert(0);
    return 0;
}
