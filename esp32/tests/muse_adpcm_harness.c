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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_adpcm.h"

/*
 * Independent oracle: CPython 3.12.5 audioop, sample width 2, initial (0, 0)
 * for ordinary fixtures; seeded cases specify their initial state below.
 * https://github.com/python/cpython/blob/v3.12.5/Modules/audioop.c
 * audioop_lin2adpcm_impl, audioop_adpcm2lin_impl and their ADPCM tables
 * were compared with v3.12.0 and are identical. PCM used native int16 packing.
 * lin2adpcm bytes have their nibbles exchanged for Muse's low-first layout;
 * decoded samples come from adpcm2lin on the original oracle bytes, not Muse.
 * Both oracle directions give the final state recorded with each fixture.
 * These raw streams have no WAV header or predictor sample prefix.
 *
 * Short anchors and silence were also checked by hand against IMA Recommended
 * Practices, revision 3.00 (1992), sections 6.1 and 6.2, printed pages 28-32:
 * https://www.cs.columbia.edu/~hgs/audio/dvi/IMA_ADPCM.pdf
 * Packing order is the contract in muse_adpcm.h, not the IMA container format.
 */
static const int16_t silence_pcm[32] = {0};
static const uint8_t silence_bytes[16] = {0};
static const int16_t silence_decoded[32] = {0};

/* One full-scale positive impulse, followed by 31 zeros. */
static const int16_t impulse_pcm[32] = {32767};
static const uint8_t impulse_bytes[16] = {
    0xa7, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const int16_t impulse_decoded[32] = {11, 1, 0, 1, 0, 1, 0, 1};

/* The integers define this periodic fixture; no floating-point sine oracle. */
static const int16_t tone_pcm[32] = {
    0, 5793, 8192, 5793, 0, -5793, -8192, -5793,
    0, 5793, 8192, 5793, 0, -5793, -8192, -5793,
    0, 5793, 8192, 5793, 0, -5793, -8192, -5793,
    0, 5793, 8192, 5793, 0, -5793, -8192, -5793,
};
static const uint8_t tone_bytes[16] = {
    0x70, 0x77, 0xfd, 0xff, 0x75, 0x96, 0xbc, 0x19,
    0x44, 0x91, 0xbc, 0x29, 0x53, 0x91, 0xbc, 0x29,
};
static const int16_t tone_decoded[32] = {
    0, 11, 41, 104, 4, -195, -625, -1550,
    -93, 2817, 8222, 6013, -14, -5687, -7896, -5888,
    -409, 6221, 8895, 6464, -166, -6406, -8837, -5154,
    -467, 6229, 8903, 6472, -158, -6398, -8829, -5146,
};

static const int16_t alternating_pcm[32] = {
    32767, -32768, 32767, -32768, 32767, -32768, 32767, -32768,
    32767, -32768, 32767, -32768, 32767, -32768, 32767, -32768,
    32767, -32768, 32767, -32768, 32767, -32768, 32767, -32768,
    32767, -32768, 32767, -32768, 32767, -32768, 32767, -32768,
};
static const uint8_t alternating_bytes[16] = {
    0xf7, 0xf7, 0xf7, 0xf7, 0xf7, 0xe7, 0xf7, 0xf7,
    0xf7, 0xf7, 0xf7, 0xf7, 0xf7, 0xf7, 0xf7, 0xf7,
};
static const int16_t alternating_decoded[32] = {
    11, -19, 44, -92, 201, -430, 927, -1983,
    4253, -9119, 19547, -32768, 28668, -32768, 28668, -32768,
    28668, -32768, 28668, -32768, 28668, -32768, 28668, -32768,
    28668, -32768, 28668, -32768, 28668, -32768, 28668, -32768,
};

typedef struct {
    const char *name;
    const int16_t *pcm;
    const uint8_t *bytes;
    const int16_t *decoded;
    size_t n;
    muse_adpcm_t end;
} fixture_t;

static const fixture_t fixtures[] = {
    {"silence32", silence_pcm, silence_bytes, silence_decoded, 32, {0, 0}},
    {"impulse32", impulse_pcm, impulse_bytes, impulse_decoded, 32, {0, 0}},
    {"tone32", tone_pcm, tone_bytes, tone_decoded, 32, {-5146, 69}},
    {"alternating32", alternating_pcm, alternating_bytes, alternating_decoded, 32, {-32768, 88}},
};

static void equal(const char *fixture, const char *operation, size_t at, int actual, int expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s %s[%zu]: expected %d, got %d\n",
                fixture, operation, at, expected, actual);
        exit(EXIT_FAILURE);
    }
}

static void state_equal(const char *fixture, const char *operation,
                        muse_adpcm_t actual, muse_adpcm_t expected)
{
    equal(fixture, operation, 0, actual.pred, expected.pred);
    equal(fixture, operation, 1, actual.index, expected.index);
}

static void bytes_equal(const char *fixture, const uint8_t *actual,
                        const uint8_t *expected, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        equal(fixture, "encode", i, actual[i], expected[i]);
    }
}

static void pcm_equal(const char *fixture, const int16_t *actual,
                      const int16_t *expected, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        equal(fixture, "decode", i, actual[i], expected[i]);
    }
}

/* Output guards are immediately adjacent to the documented payload extent. */
static void check_vector(const fixture_t *f, muse_adpcm_t initial)
{
    uint8_t encoded[f->n / 2 + 2];
    int16_t decoded[f->n + 2];
    memset(encoded, 0xa5, sizeof(encoded));
    for (size_t i = 0; i < f->n + 2; ++i) {
        decoded[i] = 12345;
    }
    muse_adpcm_t enc = initial, dec = initial;
    muse_adpcm_encode_block(&enc, f->pcm, f->n, encoded + 1);
    bytes_equal(f->name, encoded + 1, f->bytes, f->n / 2);
    state_equal(f->name, "encode state (pred,index)", enc, f->end);
    equal(f->name, "encode guard", 0, encoded[0], 0xa5);
    equal(f->name, "encode guard", f->n / 2 + 1, encoded[f->n / 2 + 1], 0xa5);

    /* Always decode the independent literal, not the encoder's output. */
    muse_adpcm_decode_block(&dec, f->bytes, f->n, decoded + 1);
    pcm_equal(f->name, decoded + 1, f->decoded, f->n);
    state_equal(f->name, "decode state (pred,index)", dec, f->end);
    equal(f->name, "decode guard", 0, decoded[0], 12345);
    equal(f->name, "decode guard", f->n + 1, decoded[f->n + 1], 12345);
}

static const int16_t packing_pcm[2] = {1, 12};
static const uint8_t packing_bytes[1] = {0x71};
/* IMA codes 1 then 7: predictions 1 then 12, indices 0 then 8. */
static const fixture_t packing_pair = {
    "packing_pair", packing_pcm, packing_bytes, packing_pcm, 2, {12, 8},
};

static void check_low_nibble_first(void)
{
    check_vector(&packing_pair, (muse_adpcm_t){0, 0});
}

static void check_fixture_vectors(void)
{
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        check_vector(&fixtures[i], (muse_adpcm_t){0, 0});
    }
}

static void check_quantizer_boundaries(void)
{
    /* audioop 3.12.5 from (0,0): raw 0x42, predictions [7,12], end (12,1).
     * 7 hits the whole-step comparison; 11 hits the next half-step boundary. */
    const int16_t pcm[2] = {7, 11}, decoded[2] = {7, 12};
    const uint8_t bytes[1] = {0x24};
    const fixture_t boundary = {"quantizer_boundary", pcm, bytes, decoded, 2, {12, 1}};
    check_vector(&boundary, (muse_adpcm_t){0, 0});
}

static void check_index_sweep(void)
{
    /* audioop.lin2adpcm: two samples of 29794, starting (0,index) for 0..88.
     * Each row freezes the nibble-swapped byte and returned (pred,index).
     * Seeding every valid index exercises every step-table entry directly. */
    static const struct {
        uint8_t byte;
        muse_adpcm_t end;
    } golden[89] = {
        {0x77, {41, 16}}, {0x77, {46, 17}}, {0x77, {50, 18}},
        {0x77, {56, 19}}, {0x77, {60, 20}}, {0x77, {68, 21}},
        {0x77, {75, 22}}, {0x77, {81, 23}}, {0x77, {93, 24}},
        {0x77, {99, 25}}, {0x77, {110, 26}}, {0x77, {121, 27}},
        {0x77, {134, 28}}, {0x77, {147, 29}}, {0x77, {164, 30}},
        {0x77, {179, 31}}, {0x77, {199, 32}}, {0x77, {218, 33}},
        {0x77, {241, 34}}, {0x77, {264, 35}}, {0x77, {292, 36}},
        {0x77, {321, 37}}, {0x77, {355, 38}}, {0x77, {389, 39}},
        {0x77, {429, 40}}, {0x77, {473, 41}}, {0x77, {520, 42}},
        {0x77, {572, 43}}, {0x77, {629, 44}}, {0x77, {693, 45}},
        {0x77, {764, 46}}, {0x77, {840, 47}}, {0x77, {924, 48}},
        {0x77, {1017, 49}}, {0x77, {1120, 50}}, {0x77, {1232, 51}},
        {0x77, {1355, 52}}, {0x77, {1493, 53}}, {0x77, {1641, 54}},
        {0x77, {1807, 55}}, {0x77, {1988, 56}}, {0x77, {2186, 57}},
        {0x77, {2407, 58}}, {0x77, {2645, 59}}, {0x77, {2912, 60}},
        {0x77, {3205, 61}}, {0x77, {3523, 62}}, {0x77, {3877, 63}},
        {0x77, {4267, 64}}, {0x77, {4691, 65}}, {0x77, {5162, 66}},
        {0x77, {5677, 67}}, {0x77, {6247, 68}}, {0x77, {6869, 69}},
        {0x77, {7556, 70}}, {0x77, {8314, 71}}, {0x77, {9146, 72}},
        {0x77, {10061, 73}}, {0x77, {11068, 74}}, {0x77, {12175, 75}},
        {0x77, {13391, 76}}, {0x77, {14729, 77}}, {0x77, {16203, 78}},
        {0x77, {17827, 79}}, {0x77, {19608, 80}}, {0x77, {21570, 81}},
        {0x77, {23728, 82}}, {0x77, {26100, 83}}, {0x77, {28709, 84}},
        {0x67, {28710, 83}}, {0x57, {28422, 82}}, {0x57, {31267, 83}},
        {0x47, {30572, 82}}, {0x37, {29423, 80}}, {0x27, {27742, 81}},
        {0x27, {30516, 82}}, {0x17, {27972, 83}}, {0x17, {30768, 84}},
        {0x07, {27074, 85}}, {0x07, {29783, 86}}, {0x07, {32761, 87}},
        {0x87, {27438, 87}}, {0x86, {25967, 87}}, {0x05, {31704, 86}},
        {0x85, {26684, 87}}, {0x04, {31424, 86}}, {0x84, {26376, 87}},
        {0x84, {28672, 87}}, {0x03, {32393, 86}},
    };
    const int16_t pcm[2] = {29794, 29794};
    for (size_t i = 0; i < 89; ++i) {
        muse_adpcm_t enc = {0, (int8_t)i};
        uint8_t encoded[1];
        muse_adpcm_encode_block(&enc, pcm, 2, encoded);
        equal("index_sweep", "encode at seeded index", i, encoded[0], golden[i].byte);
        equal("index_sweep", "predictor at seeded index", i, enc.pred, golden[i].end.pred);
        equal("index_sweep", "index at seeded index", i, enc.index, golden[i].end.index);
    }
}

static void check_saturation(void)
{
    /* Spec bounds, cross-checked with seeded audioop.adpcm2lin calls. */
    const struct {
        const char *name;
        muse_adpcm_t initial;
        uint8_t byte;
        int16_t decoded[2];
        muse_adpcm_t end;
    } cases[] = {
        {"positive_limit", {32760, 88}, 0x77, {32767, 32767}, {32767, 88}},
        {"negative_limit", {-32760, 88}, 0xff, {-32768, -32768}, {-32768, 88}},
        /* Step 8 contributes 1, then step 7 contributes 0; index stays at 0. */
        {"index_floor", {0, 1}, 0x00, {1, 1}, {1, 0}},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int16_t out[2];
        muse_adpcm_t state = cases[i].initial;
        muse_adpcm_decode_block(&state, &cases[i].byte, 2, out);
        pcm_equal(cases[i].name, out, cases[i].decoded, 2);
        state_equal(cases[i].name, "decode state (pred,index)", state, cases[i].end);
    }
    /* Independent lin2adpcm from (32760,88): raw 0x00, end (32767,86).
     * Even code 0's positive increment takes this encoder past the limit. */
    const int16_t pcm[2] = {32767, 32767};
    uint8_t encoded[1];
    muse_adpcm_t enc = {32760, 88};
    muse_adpcm_encode_block(&enc, pcm, 2, encoded);
    equal("encoder_positive_limit", "encode", 0, encoded[0], 0x00);
    state_equal("encoder_positive_limit", "encode state (pred,index)", enc,
                (muse_adpcm_t){32767, 86});
}

static void check_even_lengths(void)
{
    /* n=2 and n=32 payloads and guards are covered by the fixtures above. */
    /* Valid non-null buffers at n=0: no samples, writes or state advancement. */
    const int16_t unused_pcm[1] = {2345};
    const uint8_t unused_bytes[1] = {0x71};
    const fixture_t empty = {
        "empty_even", unused_pcm, unused_bytes, unused_pcm, 0, {1234, 7},
    };
    check_vector(&empty, empty.end);
    /* Odd counts are outside the API contract and are deliberately not tested. */
}

static void check_continuity_and_saved_start(void)
{
    /* Oracle checkpoints after 2, 8, 16 and 32 samples, in both directions. */
    const size_t counts[4] = {2, 6, 8, 16};
    const muse_adpcm_t checkpoints[4] = {{11, 8}, {-1550, 52}, {-5888, 68}, {-5146, 69}};
    uint8_t split[16];
    int16_t split_pcm[32];
    muse_adpcm_t enc = {0, 0}, dec = {0, 0};
    size_t at = 0;
    for (size_t i = 0; i < 4; ++i) {
        muse_adpcm_encode_block(&enc, tone_pcm + at, counts[i], split + at / 2);
        muse_adpcm_decode_block(&dec, tone_bytes + at / 2, counts[i], split_pcm + at);
        bytes_equal("tone32 split", split + at / 2, tone_bytes + at / 2, counts[i] / 2);
        pcm_equal("tone32 split", split_pcm + at, tone_decoded + at, counts[i]);
        state_equal("tone32 split", "encode state (pred,index)", enc, checkpoints[i]);
        state_equal("tone32 split", "decode state (pred,index)", dec, checkpoints[i]);
        at += counts[i];
    }

    /* Like pre-roll: save the state before encoding each chunk, then decode
     * a chunk alone from a copy. No muse_voice.c extraction or ring emulation. */
    muse_adpcm_t starts[2];
    enc = (muse_adpcm_t){0, 0};
    for (size_t i = 0; i < 2; ++i) {
        starts[i] = enc;
        muse_adpcm_encode_block(&enc, tone_pcm + i * 16, 16, split + i * 8);
    }
    const size_t order[2] = {1, 0};
    for (size_t i = 0; i < 2; ++i) {
        size_t chunk = order[i];
        int16_t out[16];
        dec = starts[chunk];
        muse_adpcm_decode_block(&dec, tone_bytes + chunk * 8, 16, out);
        pcm_equal("tone32 isolated chunk", out, tone_decoded + chunk * 16, 16);
        state_equal("tone32 isolated chunk", "decode state (pred,index)", dec, checkpoints[chunk + 2]);
    }
}

int main(void)
{
    check_low_nibble_first();
    check_fixture_vectors();
    check_quantizer_boundaries();
    check_index_sweep();
    check_saturation();
    check_even_lengths();
    check_continuity_and_saved_start();
    puts("PASS muse_adpcm");
    return 0;
}
