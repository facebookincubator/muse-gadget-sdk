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
/* set_settings / get_settings / start_mic with every request in a buffer of
 * exactly its length (heap, so ASan catches a one-byte over-read). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mg_proto.h"
static bool f_notify(void *c, mg_ch_t ch, const uint8_t *d, size_t n) { (void)c; (void)ch; (void)d; (void)n; return true; }
static void f_cap(void *c, bool s, const mg_audio_format_t *f) { (void)c; (void)s; (void)f; }
static uint8_t vol = 50;
static uint8_t f_vol(void *c) { (void)c; return vol; }
static void f_setvol(void *c, uint8_t v) { (void)c; vol = v; }
static void w(mg_proto_t *p, const uint8_t *src, size_t n)
{
    uint8_t *b = malloc(n);   /* exact size: no slack after the last byte */
    memcpy(b, src, n);
    mg_proto_write(p, MG_CH_CONTROL, b, n);
    free(b);
}
int main(void)
{
    mg_proto_ops_t ops = { .notify = f_notify, .capture = f_cap, .speaker_volume = f_vol, .set_speaker_volume = f_setvol };
    mg_proto_config_t cfg = { .haptics = true, .capture_24k = true };
    static mg_proto_t p;
    mg_proto_init(&p, &cfg, &ops, NULL);
    mg_proto_connect(&p);
    mg_proto_subscribe(&p, MG_CH_CONTROL, true);
    mg_proto_subscribe(&p, MG_CH_DATA, true);
    const uint8_t reqs[][8] = {
        { 0x41, 0x07, 0x13 },             /* 1-byte setting last */
        { 0x41, 0x02, 0x01 },
        { 0x41, 0x0a, 0x32 },             /* speaker_volume */
        { 0x41, 0x04, 0xd2 },             /* 2-byte setting cut short */
        { 0x41, 0x0d, 0xc0 },             /* capture_sample_rate cut short */
        { 0x41, 0x0e },                   /* parameter with no value */
        { 0x41, 0x07, 0x13, 0x0e, 0x20 }, /* 1-byte, then a 2-byte one cut short */
        { 0x3f, 0x07 },                   /* get one */
        { 0x01, 0x13, 0xc0 },             /* start_mic: rate cut short */
        { 0x01, 0x13, 0xc0, 0x5d, 0x40 }, /* bitrate cut short */
    };
    const size_t lens[] = { 3, 3, 3, 3, 3, 2, 5, 2, 3, 5 };
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        w(&p, reqs[i], lens[i]);
        if (reqs[i][0] == 0x01) { const uint8_t stop = 0x02; w(&p, &stop, 1); }
    }
    puts("PASS no over-read");
    return 0;
}
