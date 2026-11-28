/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  Modified by Meta Platforms, Inc.: speed and size optimizations (fast
 *  symmetric DCT, small CRC table, mono-only and 4-subband trimming,
 *  optional concealment, mergeable constant tables); bit-exact with upstream. Dual-channel encoding
 *  doesn't write past its scale factors, concealment uses the last good
 *  frame's channel count, a frame description with an out-of-range
 *  frequency, mode or allocation method is rejected, and no signed value is
 *  shifted left. See README.md in this directory.
 ******************************************************************************/

#include <sbc.h>
#include <string.h>

/* OPT: SBC_MERGE_TABLES=1 (GCC, ELF targets, 4-byte int): the small constant tables
 * that are identical (the header's decode and encode maps, the two
 * frequency tables, the two cos(i*pi/8) tables) keep their static
 * definitions, each in its own mergeable section ("aM") whose entity is the
 * whole table, so the linker keeps one copy of each. One table per section:
 * GCC may address tables that share a section from one anchor plus an
 * offset, which merging would break. The same definitions and code; off (the
 * default), nothing changes. */
#if defined(SBC_MERGE_TABLES) && SBC_MERGE_TABLES && \
    defined(__GNUC__) && !defined(__clang__) && defined(__ELF__) && \
    defined(__SIZEOF_INT__) && __SIZEOF_INT__ == 4
#if defined(__arm__)
#define SBC_MERGE_ARGS_(bytes) ",\"aM\",%progbits," #bytes " @"
#elif defined(__aarch64__)
#define SBC_MERGE_ARGS_(bytes) ",\"aM\",@progbits," #bytes " //"
#else
#define SBC_MERGE_ARGS_(bytes) ",\"aM\",@progbits," #bytes " #"
#endif
/* The flags the compiler appends to the section name land in the comment */
#define SBC_MERGE(name, bytes) \
    __attribute__((aligned(4), section(".rodata.sbc_" name SBC_MERGE_ARGS_(bytes))))
/* The header's decode maps hold ints here, as the encode maps do: their
 * entity sizes then hold whatever size an enum has (-fshort-enums) */
#define SBC_HDR_MAP(type) int
#else
#define SBC_MERGE(name, bytes)
#define SBC_HDR_MAP(type) type
#endif

#undef  SBC_BITS_TRACE
#include "bits.h"
#if SBC_UNITY
#define SBC_BITS_IMPL
#include "bits.c"
#endif


/**
 * SBC Frame header size
 */

#define SBC_HEADER_SIZE   ( 4)


/**
 * Assembly declaration
 */

void sbc_synthesize_4(struct sbc_dstate *state,
    const int16_t *in, int scale, int16_t *out, int pitch);

void sbc_synthesize_8(struct sbc_dstate *state,
    const int16_t *in, int scale, int16_t *out, int pitch);

#ifndef SBC_ASM
#define ASM(fn) (fn##_c)
#else
#define ASM(fn) (fn)
#endif


/**
 * Build-time trimming helpers
 */

#ifndef SBC_WITH_4SB
#define SBC_WITH_4SB 1                  /* 4-subband support */
#endif
#ifndef SBC_FAST_DCT
#define SBC_FAST_DCT 0                  /* symmetric analysis matrixing */
#endif
#ifndef SBC_SMALL_CRC
#define SBC_SMALL_CRC 0                 /* 16-entry CRC table instead of 256 */
#endif
#ifndef SBC_DSP
#define SBC_DSP 0                       /* Armv7E-M/Armv8-M DSP dual-MAC windowing */
#endif

#define NCH(frame) \
    (SBC_MAX_CHANNELS == 1 ? 1 : 1 + ((frame)->mode != SBC_MODE_MONO))
#define IS_MODE(frame, m) \
    (SBC_MAX_CHANNELS > 1 && (frame)->mode == (m))

#if SBC_DSP && defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
static inline int32_t sbc_ld32(const int16_t *p)
    { int32_t v; __builtin_memcpy(&v, p, 4); return v; }
/* inline asm keeps GCC from splitting the dual MAC back into two smlabb */
static inline int sbc_smlad(int32_t a, int32_t b, int acc)
    { int r; __asm__ ("smlad %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(acc)); return r; }
#define SBC_DOT2(acc, x, w) sbc_smlad(sbc_ld32(x), sbc_ld32(w), acc)
#else
#define SBC_DOT2(acc, x, w) ((acc) + (x)[0] * (w)[0] + (x)[1] * (w)[1])
#endif

static inline int sbc_dot5(const int16_t *x, const int16_t *w, int acc)
{
    acc = SBC_DOT2(acc, x + 0, w + 0);
    acc = SBC_DOT2(acc, x + 2, w + 2);
    return acc + x[4] * w[4];
}

/**
 * Macros
 * MIN/MAX  Minimum and maximum between 2 values
 * SAT16    Signed saturation on 16 bits
 * Saturate on 16 bits
 */

#define SBC_MIN(a, b)  ( (a) < (b) ?  (a) : (b) )
#define SBC_MAX(a, b)  ( (a) > (b) ?  (a) : (b) )

#define SBC_SAT16(v) (int16_t)\
    ( (v) > INT16_MAX ? INT16_MAX : \
      (v) < INT16_MIN ? INT16_MIN : (v) )


/**
 * Count number of leading zero bits
 */

#define SBC_CLZ(n) __builtin_clz(n)


/**
 * mSBC constant frame description
 */
static const struct sbc_frame msbc_frame = {
    .msbc = true,
    .mode = SBC_MODE_MONO,
    .freq = SBC_FREQ_16K,
    .bam = SBC_BAM_LOUDNESS,
    .nsubbands = 8, .nblocks = 15,
    .bitpool = 26
};


/* ----------------------------------------------------------------------------
 *  Common
 * ------------------------------------------------------------------------- */

/**
 * Compute CRC of frame
 * frame           La description de frame
 * data, size      Frame data, and maximum readable size
 * return          The CRC-8 value, -1: size too small
 */
static int compute_crc(
    const struct sbc_frame *frame, const uint8_t *data, unsigned size)
{
    /* Look-up table par 8 bits
     * Generator polynomial G(X) = X^8 + X^4 + X^3 + X^2 + 1 (0x1d) */

#if SBC_SMALL_CRC
    static const uint8_t t[16] = { 0x00, 0x1d, 0x3a, 0x27, 0x74, 0x69, 0x4e, 0x53, 0xe8, 0xf5, 0xd2, 0xcf, 0x9c, 0x81, 0xa6, 0xbb };
#else
    static const uint8_t t[] = {
        0x00, 0x1d, 0x3a, 0x27, 0x74, 0x69, 0x4e, 0x53,
        0xe8, 0xf5, 0xd2, 0xcf, 0x9c, 0x81, 0xa6, 0xbb,
        0xcd, 0xd0, 0xf7, 0xea, 0xb9, 0xa4, 0x83, 0x9e,
        0x25, 0x38, 0x1f, 0x02, 0x51, 0x4c, 0x6b, 0x76,
        0x87, 0x9a, 0xbd, 0xa0, 0xf3, 0xee, 0xc9, 0xd4,
        0x6f, 0x72, 0x55, 0x48, 0x1b, 0x06, 0x21, 0x3c,
        0x4a, 0x57, 0x70, 0x6d, 0x3e, 0x23, 0x04, 0x19,
        0xa2, 0xbf, 0x98, 0x85, 0xd6, 0xcb, 0xec, 0xf1,
        0x13, 0x0e, 0x29, 0x34, 0x67, 0x7a, 0x5d, 0x40,
        0xfb, 0xe6, 0xc1, 0xdc, 0x8f, 0x92, 0xb5, 0xa8,
        0xde, 0xc3, 0xe4, 0xf9, 0xaa, 0xb7, 0x90, 0x8d,
        0x36, 0x2b, 0x0c, 0x11, 0x42, 0x5f, 0x78, 0x65,
        0x94, 0x89, 0xae, 0xb3, 0xe0, 0xfd, 0xda, 0xc7,
        0x7c, 0x61, 0x46, 0x5b, 0x08, 0x15, 0x32, 0x2f,
        0x59, 0x44, 0x63, 0x7e, 0x2d, 0x30, 0x17, 0x0a,
        0xb1, 0xac, 0x8b, 0x96, 0xc5, 0xd8, 0xff, 0xe2,
        0x26, 0x3b, 0x1c, 0x01, 0x52, 0x4f, 0x68, 0x75,
        0xce, 0xd3, 0xf4, 0xe9, 0xba, 0xa7, 0x80, 0x9d,
        0xeb, 0xf6, 0xd1, 0xcc, 0x9f, 0x82, 0xa5, 0xb8,
        0x03, 0x1e, 0x39, 0x24, 0x77, 0x6a, 0x4d, 0x50,
        0xa1, 0xbc, 0x9b, 0x86, 0xd5, 0xc8, 0xef, 0xf2,
        0x49, 0x54, 0x73, 0x6e, 0x3d, 0x20, 0x07, 0x1a,
        0x6c, 0x71, 0x56, 0x4b, 0x18, 0x05, 0x22, 0x3f,
        0x84, 0x99, 0xbe, 0xa3, 0xf0, 0xed, 0xca, 0xd7,
        0x35, 0x28, 0x0f, 0x12, 0x41, 0x5c, 0x7b, 0x66,
        0xdd, 0xc0, 0xe7, 0xfa, 0xa9, 0xb4, 0x93, 0x8e,
        0xf8, 0xe5, 0xc2, 0xdf, 0x8c, 0x91, 0xb6, 0xab,
        0x10, 0x0d, 0x2a, 0x37, 0x64, 0x79, 0x5e, 0x43,
        0xb2, 0xaf, 0x88, 0x95, 0xc6, 0xdb, 0xfc, 0xe1,
        0x5a, 0x47, 0x60, 0x7d, 0x2e, 0x33, 0x14, 0x09,
        0x7f, 0x62, 0x45, 0x58, 0x0b, 0x16, 0x31, 0x2c,
        0x97, 0x8a, 0xad, 0xb0, 0xe3, 0xfe, 0xd9, 0xc4,
    };
#endif

    /* The CRC cover the syntax indicated with "[]" :
     *
     * Header()
     * | syncword                       8
     * | [ desc ]                      16
     * | crc                            8
     * Data()
     * | SI mode `Join-Stereo`
     * | | [ joint ]              nsb x 1
     * | [ scale-factors ]  nch x nsb x 4
     * | ...                                */

    int nch  = NCH(frame);
    int nsb  = frame->nsubbands;
    unsigned i, nbit = nch*nsb*4 +
        (IS_MODE(frame, SBC_MODE_JOINT_STEREO) ? nsb : 0);

    if (size < ((SBC_HEADER_SIZE*8 + nbit + 7) >> 3))
        return -1;

    uint8_t crc = 0x0f;
#if SBC_SMALL_CRC
#define SBC_CRC_BYTE(c, b) \
    ( c = (c << 4) ^ t[(c >> 4) ^ ((b) >> 4)], \
      c = (c << 4) ^ t[(c >> 4) ^ ((b) & 15)] )
    SBC_CRC_BYTE(crc, data[1]);
    SBC_CRC_BYTE(crc, data[2]);
    for (i = 4; i < 4 + nbit/8; i++)
        SBC_CRC_BYTE(crc, data[i]);
#else
    crc = t[crc ^ data[1]];
    crc = t[crc ^ data[2]];

    for (i = 4; i < 4 + nbit/8; i++)
        crc = t[crc ^ data[i]];
#endif

    if (nbit % 8)
        crc = (crc << 4) ^ t[(crc >> 4) ^ (data[i] >> 4)];

    return crc;
}

/**
 * Check a frame description
 * frame           The frame description to check
 * return          True when valid, False otherwise
 */
static bool check_frame(const struct sbc_frame *frame)
{
    if ((unsigned)frame->freq >= SBC_NUM_FREQ ||
            (unsigned)frame->mode >= SBC_NUM_MODE ||
            (unsigned)frame->bam >= SBC_NUM_BAM)
        return false;
    if (SBC_MAX_CHANNELS == 1 && frame->mode != SBC_MODE_MONO)
        return false;
    if (!SBC_WITH_4SB && frame->nsubbands != 8)
        return false;

    /* --- Check number of sub-blocks and sub-bands --- */

    if ((unsigned)(frame->nblocks - 4) > 12 ||
            (!frame->msbc && frame->nblocks % 4 != 0))
        return false;

    if ((unsigned)(frame->nsubbands - 4) > 4 || frame->nsubbands % 4 != 0)
        return false;

    /* --- Validate the bitpool value --- */

    bool two_channels = (NCH(frame) > 1);
    bool dual_mode = (IS_MODE(frame, SBC_MODE_DUAL_CHANNEL));
    bool joint_mode = (IS_MODE(frame, SBC_MODE_JOINT_STEREO));
    bool stereo_mode = joint_mode || (IS_MODE(frame, SBC_MODE_STEREO));

    int max_bits =
        ((16 * frame->nsubbands * frame->nblocks) << two_channels) -
        (SBC_HEADER_SIZE * 8) -
        ((4 * frame->nsubbands) << two_channels) -
        (joint_mode ? frame->nsubbands : 0);

    int max_bitpool = SBC_MIN( max_bits / (frame->nblocks << dual_mode),
                               (16 << stereo_mode) * frame->nsubbands );

    return frame->bitpool <= max_bitpool;
}

/**
 * Compute the bit distribution for Independent or Stereo Channel
 * frame           Frame description
 * scale_factors   Scale-factor values
 * nbits           Return of allocated bits for each channels / subbands
 */
static void compute_nbits(const struct sbc_frame *frame,
    const int (*scale_factors)[SBC_MAX_SUBBANDS],
    int (*nbits)[SBC_MAX_SUBBANDS])
{
    /* --- Offsets of "Loudness" bit allocation --- */

    static const int loudness_offset_4[SBC_NUM_FREQ][4] = {
        [SBC_FREQ_16K ] = { -1,  0,  0,  0 },
        [SBC_FREQ_32K ] = { -2,  0,  0,  1 },
        [SBC_FREQ_44K1] = { -2,  0,  0,  1 },
        [SBC_FREQ_48K ] = { -2,  0,  0,  1 },
    };

    static const int loudness_offset_8[SBC_NUM_FREQ][8] = {
        [SBC_FREQ_16K ] = { -2,  0,  0,  0,  0,  0,  0,  1 },
        [SBC_FREQ_32K ] = { -3,  0,  0,  0,  0,  0,  1,  2 },
        [SBC_FREQ_44K1] = { -4,  0,  0,  0,  0,  0,  1,  2 },
        [SBC_FREQ_48K ] = { -4,  0,  0,  0,  0,  0,  1,  2 },
    };

    /* --- Compute the number of bits needed --- */

    const int *loudness_offset = SBC_WITH_4SB && frame->nsubbands == 4 ?
        loudness_offset_4[frame->freq] : loudness_offset_8[frame->freq];

    bool stereo_mode = IS_MODE(frame, SBC_MODE_STEREO) ||
                       IS_MODE(frame, SBC_MODE_JOINT_STEREO);

    int nsubbands = frame->nsubbands;
    int nchannels = 1 + stereo_mode;

    int bitneeds[2][SBC_MAX_SUBBANDS];
    int max_bitneed = 0;

    for (int ich = 0; ich < nchannels; ich++)
        for (int isb = 0; isb < nsubbands; isb++) {
            int bitneed, scf = scale_factors[ich][isb];

            if (frame->bam == SBC_BAM_LOUDNESS) {
                bitneed = scf ? scf - loudness_offset[isb] : -5;
                bitneed >>= (bitneed > 0);
            } else {
                bitneed = scf;
            }

            if (bitneed > max_bitneed)
                max_bitneed = bitneed;

            bitneeds[ich][isb] = bitneed;
        }

    /* --- Loop over the bit distribution, until reaching the bitpool --- */

    int bitpool = frame->bitpool;

    int bitcount = 0;
    int bitslice = max_bitneed + 1;

    for (int bc = 0; bc < bitpool; ) {

        int bs = bitslice--;
        bitcount = bc;
        if (bitcount == bitpool)
            break;

        for (int ich = 0; ich < nchannels; ich++)
            for (int isb = 0; isb < nsubbands; isb++) {
                int bn = bitneeds[ich][isb];
                bc += (bn >= bs && bn < bs + 15) + (bn == bs);
            }
    }

    /* --- Bits distribution --- */

    for (int ich = 0; ich < nchannels; ich++)
        for (int isb = 0; isb < nsubbands; isb++) {
            int nbit = bitneeds[ich][isb] - bitslice;
            nbits[ich][isb] = nbit < 2 ? 0 : nbit > 16 ? 16 : nbit;
        }

    /* --- Allocate remaining bits --- */

    for (int isb = 0; isb < nsubbands && bitcount < bitpool; isb++)
        for (int ich = 0; ich < nchannels && bitcount < bitpool; ich++) {

            int n = nbits[ich][isb] && nbits[ich][isb] < 16 ? 1 :
                    bitneeds[ich][isb] == bitslice + 1 &&
                    bitpool > bitcount + 1 ? 2 : 0;

            nbits[ich][isb] += n;
            bitcount += n;
        }

    for (int isb = 0; isb < nsubbands && bitcount < bitpool; isb++)
        for (int ich = 0; ich < nchannels && bitcount < bitpool; ich++) {

            int n = (nbits[ich][isb] < 16);
            nbits[ich][isb] += n;
            bitcount += n;
        }
}

/**
 * Return the sampling frequency in Hz
 */
int sbc_get_freq_hz(enum sbc_freq freq)
{
    static const int freq_hz[SBC_NUM_FREQ] SBC_MERGE("freq_hz_get", 16) = {
        [SBC_FREQ_16K ] = 16000, [SBC_FREQ_32K ] = 32000,
        [SBC_FREQ_44K1] = 44100, [SBC_FREQ_48K ] = 48000,
    };

    return freq_hz[freq];
}

/**
 * Return the frame size from a frame description
 */
unsigned sbc_get_frame_size(const struct sbc_frame *frame)
{
    if (!check_frame(frame))
        return 0;

    bool two_channels = (NCH(frame) > 1);
    bool dual_mode = (IS_MODE(frame, SBC_MODE_DUAL_CHANNEL));
    bool joint_mode = (IS_MODE(frame, SBC_MODE_JOINT_STEREO));

    unsigned nbits =
        ((4 * frame->nsubbands) << two_channels) +
        ((frame->nblocks * frame->bitpool) << dual_mode) +
        ((joint_mode ? frame->nsubbands : 0));

    return SBC_HEADER_SIZE + ((nbits + 7) >> 3);
}

/**
 * Return the bitrate from a frame description
 */
unsigned sbc_get_frame_bitrate(const struct sbc_frame *frame)
{
    if (!check_frame(frame))
        return 0;

    unsigned nsamples = frame->nblocks * frame->nsubbands;
    unsigned nbits = 8 * sbc_get_frame_size(frame);

    return (nbits * sbc_get_freq_hz(frame->freq)) / nsamples;
}

/**
 * Return the bitrate from a frame description
 */
int sbc_get_frame_bps(enum sbc_freq freq)
{
    static const int freq_hz[SBC_NUM_FREQ] SBC_MERGE("freq_hz_bps", 16) = {
        [SBC_FREQ_16K ] = 16000, [SBC_FREQ_32K ] = 32000,
        [SBC_FREQ_44K1] = 44100, [SBC_FREQ_48K ] = 48000,
    };

    return freq_hz[freq];
}

/**
 * Reset of the context
 */
void sbc_reset(struct sbc *sbc)
{
    *sbc = (struct sbc){ };
}


/* ----------------------------------------------------------------------------
 *  Decoding
 * ------------------------------------------------------------------------- */

/**
 * Decode the 4 bytes frame header
 * bits            Bitstream reader
 * frame           Return the frame description
 * crc             Return the CRC validating the stream, or NULL
 * return          True on success, False otherwise
 */
static bool decode_header(sbc_bits_t *bits, struct sbc_frame *frame, int *crc)
{
    static const SBC_HDR_MAP(enum sbc_freq) dec_freq[] SBC_MERGE("hdr16_dec_freq", 16) =
        { SBC_FREQ_16K, SBC_FREQ_32K, SBC_FREQ_44K1, SBC_FREQ_48K };

    static const SBC_HDR_MAP(enum sbc_mode) dec_mode[] SBC_MERGE("hdr16_dec_mode", 16) =
        { SBC_MODE_MONO, SBC_MODE_DUAL_CHANNEL,
          SBC_MODE_STEREO, SBC_MODE_JOINT_STEREO };

    static const SBC_HDR_MAP(enum sbc_bam) dec_bam[] SBC_MERGE("hdr8_dec_bam", 8) =
        { SBC_BAM_LOUDNESS, SBC_BAM_SNR };

    /* --- Decode header ---
     *
     * Two possible headers :
     * - Header, with syncword 0x9c (A2DP)
     * - mSBC header, with syncword 0xad (HFP) */

    SBC_WITH_BITS(bits);

    int syncword = SBC_GET_BITS("syncword", 8);
    frame->msbc = (syncword == 0xad);
    if (frame->msbc) {
        SBC_GET_BITS("reserved", 16);
        *frame = msbc_frame;
    }

    else if (syncword == 0x9c) {
        frame->freq = dec_freq[SBC_GET_BITS("sampling_frequency", 2)];
        frame->nblocks = (1 + SBC_GET_BITS("blocks", 2)) << 2;
        frame->mode = dec_mode[SBC_GET_BITS("channel_mode", 2)];
        frame->bam = dec_bam[SBC_GET_BITS("allocation_method", 1)];
        frame->nsubbands = (1 + SBC_GET_BITS("subbands", 1)) << 2;
        frame->bitpool = SBC_GET_BITS("bitpool", 8);

    } else
        return false;

    if (crc)
        *crc = SBC_GET_BITS("crc_check", 8);

    SBC_END_WITH_BITS();

    /* --- Check bitpool value and return --- */

    return check_frame(frame);
}

/**
 * Decode frame data
 * bits            Bitstream reader
 * frame           Frame description
 * sb_samples      Return the sub-band samples, by channels
 * sb_scale        Return the sample scaler, by track (indep. channels)
 */
static void decode_frame(sbc_bits_t *bits, const struct sbc_frame *frame,
    int16_t (*sb_samples)[SBC_MAX_SAMPLES], int *sb_scale)
{
    static const int range_scale[] = {
        0xFFFFFFF, 0x5555556, 0x2492492, 0x1111111,
        0x0842108, 0x0410410, 0x0204081, 0x0101010,
        0x0080402, 0x0040100, 0x0020040, 0x0010010,
        0x0008004, 0x0004001, 0x0002000, 0x0001000
    };

    /* --- Decode joint bands indications --- */

    SBC_WITH_BITS(bits);

    unsigned mjoint = 0;

    if (IS_MODE(frame, SBC_MODE_JOINT_STEREO) && frame->nsubbands == 4)
    {
        unsigned v = SBC_GET_BITS("join[]", 4);
        mjoint = ((    0x00) << 3) | ((v & 0x02) << 1) |
                 ((v & 0x04) >> 1) | ((v & 0x08) >> 3)  ;

    } else if (IS_MODE(frame, SBC_MODE_JOINT_STEREO)) {
        unsigned v = SBC_GET_BITS("join[]", 8);

        mjoint = ((    0x00) << 7) | ((v & 0x02) << 5) |
                 ((v & 0x04) << 3) | ((v & 0x08) << 1) |
                 ((v & 0x10) >> 1) | ((v & 0x20) >> 3) |
                 ((v & 0x40) >> 5) | ((v & 0x80) >> 7)  ;
    }

    /* --- Decode scale factors --- */

    int nchannels = NCH(frame);
    int nsubbands = frame->nsubbands;

    int scale_factors[2][SBC_MAX_SUBBANDS];
    int nbits[2][SBC_MAX_SUBBANDS];

    for (int ich = 0; ich < nchannels; ich++)
        for (int isb = 0; isb < nsubbands; isb++)
            scale_factors[ich][isb] = SBC_GET_BITS("scale_factor", 4);

    compute_nbits(frame, scale_factors, nbits);
    if (IS_MODE(frame, SBC_MODE_DUAL_CHANNEL))
        compute_nbits(frame, scale_factors + 1, nbits + 1);

    /* --- Decode samples ---
     *
     * They are unquantized according :
     *
     *                  2 sample + 1
     *   sb_sample = ( -------------- - 1 ) 2^(scf + 1)
     *                   2^nbit - 1
     *
     * A sample is coded on maximum 16 bits, and the scale factor is limited
     * to 15 bits. Thus the dynamic of sub-bands samples are 17 bits.
     * Regarding "Joint-Stereo" sub-bands, uncoupling increase the dynamic
     * to 18 bits.
     *
     * The `1 / (2^nbit - 1)` values are precalculated on 1.28 :
     *
     *   sb_sample = ((2 sample + 1) * range_scale - 2^28) / 2^shr
     *
     *   with  shr = 28 - ((scf + 1) + sb_scale)
     *         sb_scale = (15 - max(scale_factor[])) - (18 - 16)
     *
     * We introduce `sb_scale`, to limit the range on 16 bits, or increase
     * precision when the scale-factor of the frame is below 13. */

    for (int ich = 0; ich < nchannels; ich++) {
        int max_scf = 0;

        for (int isb = 0; isb < nsubbands; isb++) {
            int scf = scale_factors[ich][isb] + ((mjoint >> isb) & 1);
            if (scf > max_scf) max_scf = scf;
        }

        sb_scale[ich] = (15 - max_scf) - (17 - 16);
    }

    if (IS_MODE(frame, SBC_MODE_JOINT_STEREO))
        sb_scale[0] = sb_scale[1] =
            sb_scale[0] < sb_scale[1] ? sb_scale[0] : sb_scale[1];

    for (int iblk = 0; iblk < frame->nblocks; iblk++)
        for (int ich = 0; ich < nchannels; ich++) {
            int16_t *p_sb_samples = sb_samples[ich] + iblk*nsubbands;

            for (int isb = 0; isb < nsubbands; isb++) {
                int nbit = nbits[ich][isb];
                int scf = scale_factors[ich][isb];

                if (!nbit) { *(p_sb_samples++) = 0; continue; }

                int s = SBC_GET_BITS("audio_sample", nbit);
                s = ((s << 1) | 1) * range_scale[nbit-1];

                *(p_sb_samples++) =
                    (s - (1 << 28)) >> (28 - ((scf + 1) + sb_scale[ich]));
            }
        }

    /* --- Uncoupling "Joint-Stereo" ---
     *
     * The `Left/Right` samples are coded as :
     *   `sb_sample(left ) = sb_sample(ch 0) + sb_sample(ch 1)`
     *   `sb_sample(right) = sb_sample(ch 0) - sb_sample(ch 1)` */

    for (int isb = 0; isb < nsubbands; isb++) {

        if (((mjoint >> isb) & 1) == 0)
            continue;

        for (int iblk = 0; iblk < frame->nblocks; iblk++) {
            int16_t s0 = sb_samples[0][iblk*nsubbands + isb];
            int16_t s1 = sb_samples[1][iblk*nsubbands + isb];

            sb_samples[0][iblk*nsubbands + isb] = s0 + s1;
            sb_samples[1][iblk*nsubbands + isb] = s0 - s1;
        }
    }

    /* --- Remove padding --- */

    int padding_nbits = 8 - (sbc_tell_bits(bits) % 8);
    if (padding_nbits < 8)
        SBC_GET_FIXED("padding_bits", padding_nbits, 0);

    SBC_END_WITH_BITS();
}

#if SBC_WITH_4SB
/**
 * Perform a DCT on 4 samples
 * in              Subbands input samples
 * scale           Scale factor of input samples (-2 to 14)
 * out0            Output of 1st half samples
 * out1            Output of 2nd half samples
 * idx             Index of transformed samples
 */
static inline void dct4(const int16_t *in, int scale,
    int16_t (*out0)[10], int16_t (*out1)[10], int idx)
{
     /* cos(i*pi/8)  for i = [0;3], in fixed 0.13 */

    static const int16_t cos8[] SBC_MERGE("cos8_dct4", 8) = { 8192, 7568, 5793, 3135 };

    /* --- DCT of subbands samples ---
     *          ___
     *          \
     *   u[k] = /__  h(k,i) * s(i) , i = [0;n-1]  k = [0;2n-1]
     *           i
     *
     *   With  n the number of subbands (4)
     *         h(k,i) = cos( (i + 1/2) (k + n/2) pi/n )
     *
     * Note :
     *
     *     h( 2, i) =  0 et h( n-k,i) = -h(k,i)   , k = [0;n/2-1]
     *     h(12, i) = -1 et h(2n-k,i) =  h(n+k,i) , k = [1;n/2-1]
     *
     * To assist the windowing step, the 2 halves are stored in 2 buffers.
     * After scaling of coefficients, the result is saturated on 16 bits. */

    int16_t s03 = (in[0] + in[3]) >> 1, d03 = (in[0] - in[3]) >> 1;
    int16_t s12 = (in[1] + in[2]) >> 1, d12 = (in[1] - in[2]) >> 1;

    int a0 = ( (s03 - s12 ) * cos8[2] );
    int b1 = (-(s03 + s12 ) ) * 8192;  /* was << 13 (UB on negatives) */
    int a1 = ( d03*cos8[3] - d12 * cos8[1] );
    int b0 = (-d03*cos8[1] - d12 * cos8[3] );

    int shr = 12 + scale;

    a0 = (a0 + (1 << (shr-1))) >> shr;  b0 = (b0 + (1 << (shr-1))) >> shr;
    a1 = (a1 + (1 << (shr-1))) >> shr;  b1 = (b1 + (1 << (shr-1))) >> shr;

    out0[0][idx] = SBC_SAT16( a0);  out0[3][idx] = SBC_SAT16(-a1);
    out0[1][idx] = SBC_SAT16( a1);  out0[2][idx] = SBC_SAT16(  0);

    out1[0][idx] = SBC_SAT16(-a0);  out1[3][idx] = SBC_SAT16( b0);
    out1[1][idx] = SBC_SAT16( b0);  out1[2][idx] = SBC_SAT16( b1);
}
#endif

/**
 * Perform a DCT on 8 samples
 * in              Subbands input samples
 * scale           Scale factor of input samples (-2 to 14)
 * out0            Output of 1st half samples
 * out1            Output of 2nd half samples
 * idx             Index of transformed samples
 */
static inline void dct8(const int16_t *in, int scale,
    int16_t (*out0)[10], int16_t (*out1)[10], int idx)
{
     /* cos(i*pi/16)  for i = [0;7], in fixed 0.13 */

    static const int16_t cos16[] =
        { 8192, 8035, 7568, 6811, 5793, 4551, 3135, 1598 };

    /* --- DCT of subbands samples ---
     *          ___
     *          \
     *   u[k] = /__  h(k,i) * s(i) , i = [0;n-1]  k = [0;2n-1]
     *           i
     *
     *   With  n the number of subbands (8)
     *         h(k,i) = cos( (i + 1/2) (k + n/2) pi/n )
     *
     *
     *
     * Note :
     *
     *     h( 4, i) =  0 et h( n-k,i) = -h(k,i)   , k = [0;n/2-1]
     *     h(12, i) = -1 et h(2n-k,i) =  h(n+k,i) , k = [1;n/2-1]
     *
     * To assist the windowing step, the 2 halves are stored in 2 buffers.
     * After scaling of coefficients, the result is saturated on 16 bits. */

    int16_t s07 = (in[0] + in[7]) >> 1, d07 = (in[0] - in[7]) >> 1;
    int16_t s16 = (in[1] + in[6]) >> 1, d16 = (in[1] - in[6]) >> 1;
    int16_t s25 = (in[2] + in[5]) >> 1, d25 = (in[2] - in[5]) >> 1;
    int16_t s34 = (in[3] + in[4]) >> 1, d34 = (in[3] - in[4]) >> 1;

    int a0 = ( (s07 + s34) - (s25 + s16) ) * cos16[4];
    int b3 = (-(s07 + s34) - (s25 + s16) ) * 8192;  /* was << 13 (UB on negatives) */
    int a2 = ( (s07 - s34) * cos16[6] + (s25 - s16) * cos16[2] );
    int b1 = ( (s34 - s07) * cos16[2] + (s25 - s16) * cos16[6] );
    int a1 = ( d07*cos16[5] - d16*cos16[1] + d25*cos16[7] + d34*cos16[3] );
    int b2 = (-d07*cos16[1] - d16*cos16[3] - d25*cos16[5] - d34*cos16[7] );
    int a3 = ( d07*cos16[7] - d16*cos16[5] + d25*cos16[3] - d34*cos16[1] );
    int b0 = (-d07*cos16[3] + d16*cos16[7] + d25*cos16[1] + d34*cos16[5] );

    int shr = 12 + scale;

    a0 = (a0 + (1 << (shr-1))) >> shr;  b0 = (b0 + (1 << (shr-1))) >> shr;
    a1 = (a1 + (1 << (shr-1))) >> shr;  b1 = (b1 + (1 << (shr-1))) >> shr;
    a2 = (a2 + (1 << (shr-1))) >> shr;  b2 = (b2 + (1 << (shr-1))) >> shr;
    a3 = (a3 + (1 << (shr-1))) >> shr;  b3 = (b3 + (1 << (shr-1))) >> shr;

    out0[0][idx] = SBC_SAT16( a0);  out0[7][idx] = SBC_SAT16(-a1);
    out0[1][idx] = SBC_SAT16( a1);  out0[6][idx] = SBC_SAT16(-a2);
    out0[2][idx] = SBC_SAT16( a2);  out0[5][idx] = SBC_SAT16(-a3);
    out0[3][idx] = SBC_SAT16( a3);  out0[4][idx] = SBC_SAT16(  0);

    out1[0][idx] = SBC_SAT16(-a0);  out1[7][idx] = SBC_SAT16( b0);
    out1[1][idx] = SBC_SAT16( b0);  out1[6][idx] = SBC_SAT16( b1);
    out1[2][idx] = SBC_SAT16( b1);  out1[5][idx] = SBC_SAT16( b2);
    out1[3][idx] = SBC_SAT16( b2);  out1[4][idx] = SBC_SAT16( b3);
}

/**
 * Apply window on reconstructed samples
 * in, n           Reconstructed samples and number of subbands
 * window          Window coefficients
 * offset          Offset of coefficients for each samples
 * out             Output adress of PCM samples
 * pitch           Number of PCM samples between two consecutive
 */
static inline void apply_window(const int16_t (*in)[10], int n,
    const int16_t (*window)[2*10], int offset, int16_t *out, int pitch)
{
    const int16_t *u = (const int16_t *)in;

    for (int i = 0; i < n; i++) {
        const int16_t *w = window[i] + offset;
        int s;

#if SBC_DSP
        s = SBC_DOT2(0, u + 0, w + 0);  s = SBC_DOT2(s, u + 2, w + 2);
        s = SBC_DOT2(s, u + 4, w + 4);  s = SBC_DOT2(s, u + 6, w + 6);
        s = SBC_DOT2(s, u + 8, w + 8);  u += 10;
#else
        s  = *(u++) * *(w++);  s += *(u++) * *(w++);
        s += *(u++) * *(w++);  s += *(u++) * *(w++);
        s += *(u++) * *(w++);  s += *(u++) * *(w++);
        s += *(u++) * *(w++);  s += *(u++) * *(w++);
        s += *(u++) * *(w++);  s += *(u++) * *(w++);
#endif

        *out = SBC_SAT16((s + (1 << 12)) >> 13);  out += pitch;
    }
}

#if SBC_WITH_4SB
/**
 * Synthesize samples of a 4 subbands block
 * state           Previous transformed samples of the channel
 * in              Sub-band samples
 * scale           Scale factor of samples
 * out             Output adress of PCM samples
 * pitch           Number of samples between two consecutive
 */
void sbc_synthesize_4_c(struct sbc_dstate *state,
    const int16_t *in, int scale, int16_t *out, int pitch)
{
    /* --- Windowing coefficients (fixed 2.13) ---
     *
     * The table is duplicated and transposed to fit the circular
     * buffer of reconstructed samples */

    static const int16_t window[4][2*10] =  {
        {   0, -126,  -358, -848, -4443, -9644, 4443,  -848,  358, -126,
            0, -126,  -358, -848, -4443, -9644, 4443,  -848,  358, -126 },

        { -18, -128,  -670, -201, -6389, -9235, 2544, -1055,  100,  -90,
          -18, -128,  -670, -201, -6389, -9235, 2544, -1055,  100,  -90 },

        { -49,  -61,  -946,  944, -8082, -8082,  944,  -946,  -61,  -49,
          -49,  -61,  -946,  944, -8082, -8082,  944,  -946,  -61,  -49 },

        { -90,  100, -1055, 2544, -9235, -6389, -201,  -670, -128,  -18,
          -90,  100, -1055, 2544, -9235, -6389, -201,  -670, -128,  -18 }
    };

    /* --- IDCT and windowing --- */

    int dct_idx = state->idx ? 10 - state->idx : 0, odd = dct_idx & 1;

    dct4(in, scale, state->v[odd], state->v[!odd], dct_idx);
    apply_window(state->v[odd], 4, window, state->idx, out, pitch);

    state->idx = state->idx < 9 ? state->idx + 1 : 0;
}
#endif

/**
 * Synthesize samples of a 8 subbands block
 * state           Previous transformed samples of the channel
 * sb_samples      Sub-band samples
 * sb_scale        Scale factor of samples (-2 to 14)
 * out             Output adress of PCM samples
 * pitch           Number of PCM samples between two consecutive
 */
void sbc_synthesize_8_c(struct sbc_dstate *state,
    const int16_t *in, int scale, int16_t *out, int pitch)
{
    /* --- Windowing coefficients (fixed 2.13) ---
     *
     * The table is duplicated and transposed to fit the circular
     * buffer of reconstructed samples */

    static const int16_t window[8][2*10] = {
        {    0, -132,  -371, -848, -4456, -9631, 4456,  -848,  371, -132,
             0, -132,  -371, -848, -4456, -9631, 4456,  -848,  371, -132 },

        {  -10, -138,  -526, -580, -5438, -9528, 3486, -1004,  229, -117,
           -10, -138,  -526, -580, -5438, -9528, 3486, -1004,  229, -117 },

        {  -22, -131,  -685, -192, -6395, -9224, 2561, -1063,  108,  -97,
           -22, -131,  -685, -192, -6395, -9224, 2561, -1063,  108,  -97 },

        {  -36, -106,  -835,  322, -7287, -8734, 1711, -1042,   12,  -75,
           -36, -106,  -835,  322, -7287, -8734, 1711, -1042,   12,  -75 },

        {  -54,  -59,  -960,  959, -8078, -8078,  959,  -960,  -59,  -54,
           -54,  -59,  -960,  959, -8078, -8078,  959,  -960,  -59,  -54 },

        {  -75,   12, -1042, 1711, -8734, -7287,  322,  -835, -106,  -36,
           -75,   12, -1042, 1711, -8734, -7287,  322,  -835, -106,  -36 },

        {  -97,  108, -1063, 2561, -9224, -6395, -192,  -685, -131,  -22,
           -97,  108, -1063, 2561, -9224, -6395, -192,  -685, -131,  -22 },

        { -117,  229, -1004, 3486, -9528, -5438, -580,  -526, -138,  -10,
          -117,  229, -1004, 3486, -9528, -5438, -580,  -526, -138,  -10 }
    };

    /* --- IDCT and windowing --- */

    int dct_idx = state->idx ? 10 - state->idx : 0, odd = dct_idx & 1;

    dct8(in, scale, state->v[odd], state->v[!odd], dct_idx);
    apply_window(state->v[odd], 8, window, state->idx, out, pitch);

    state->idx = state->idx < 9 ? state->idx + 1 : 0;
}

/**
 * Synthesize samples of a channel
 * state           Previous transformed samples of the channel
 * nblocks         Number of blocks (4, 8, 12 or 16)
 * nsubbands       Number of subbands (4 or 8)
 * in              Sub-band input samples
 * scale           Scale factor of samples
 * out             Output adress of PCM samples
 * pitch           Number of PCM samples between two consecutive
 */
static inline void synthesize(
    struct sbc_dstate *state, int nblocks, int nsubbands,
    const int16_t *in, int scale, int16_t *out, int pitch)
{
    for (int iblk = 0; iblk < nblocks; iblk++) {

#if SBC_WITH_4SB
        if (nsubbands == 4)
            ASM(sbc_synthesize_4)(state, in, scale, out, pitch);
        else
#endif
            ASM(sbc_synthesize_8)(state, in, scale, out, pitch);

        in += nsubbands;
        out += nsubbands * pitch;
    }
}

/**
 * Probe data and return frame description
 */
int sbc_probe(const void *data, struct sbc_frame *frame)
{
    sbc_bits_t bits;

    sbc_setup_bits(&bits, SBC_BITS_READ, (void *)data, SBC_HEADER_SIZE);
    return !decode_header(&bits, frame, NULL) ||
           sbc_bits_error(&bits) ? -1 : 0;
}

/**
 * Decode a frame
 */
int sbc_decode(struct sbc *sbc,
    const void *data, unsigned size, struct sbc_frame *frame,
    int16_t *pcml, int pitchl, int16_t *pcmr, int pitchr)
{
    sbc_bits_t bits;
    int crc;

    /* --- Decode the frame header --- */

    if (data) {

        if (size < SBC_HEADER_SIZE)
            return -1;

        sbc_setup_bits(&bits, SBC_BITS_READ, (void *)data, SBC_HEADER_SIZE);
        if (!decode_header(&bits, frame, &crc) || sbc_bits_error(&bits))
            return -1;

        /* robustness: a (corrupted or unexpected) 2-channel frame must not be
         * synthesized into a missing right-channel buffer */
        if (NCH(frame) > 1 && !pcmr)
            return -1;

        if (size < sbc_get_frame_size(frame) || compute_crc(frame, data, size) != crc)
            return -1;

    } else if (!sbc->nchannels || (sbc->nchannels > 1 && !pcmr)) {

        /* concealment replays the decoder's last good frame: none yet, or a
         * 2-channel one with no right-channel buffer to write */
        return -1;
    }

    /* --- Decode the frame data --- */

    int16_t alignas(sizeof(int)) sb_samples[SBC_MAX_CHANNELS][SBC_MAX_SAMPLES];
    int sb_scale[2];

    if (data) {

        sbc_setup_bits(&bits, SBC_BITS_READ,
            (void *)((uintptr_t)data + SBC_HEADER_SIZE),
            sbc_get_frame_size(frame) - SBC_HEADER_SIZE);

        decode_frame(&bits, frame, sb_samples, sb_scale);

        sbc->nchannels = NCH(frame);
        sbc->nblocks = frame->nblocks;
        sbc->nsubbands = frame->nsubbands;

    } else {

        int nsamples = sbc->nblocks * sbc->nsubbands;

#if SBC_PLC
        /* Concealment: replay the last good frame's sub-band samples,
         * attenuated by 6 dB per consecutive lost frame (muted after 4) */
        if (sbc->plc.nlost <= 4)
            sbc->plc.nlost++;
        int shr = sbc->plc.nlost;
        for (int ich = 0; ich < sbc->nchannels; ich++) {
            for (int i = 0; i < nsamples; i++)
                sb_samples[ich][i] = shr > 4 ? 0 : sbc->plc.sb[ich][i] >> shr;
            sb_scale[ich] = sbc->plc.scale[ich];
        }
#else
        for (int ich = 0; ich < sbc->nchannels; ich++) {
            memset(sb_samples[ich], 0, nsamples * sizeof(int16_t));
            sb_scale[ich] = 0;
        }
#endif
    }

#if SBC_PLC
    if (data) {
        int nsamples = sbc->nblocks * sbc->nsubbands;
        sbc->plc.nlost = 0;
        for (int ich = 0; ich < sbc->nchannels; ich++) {
            memcpy(sbc->plc.sb[ich], sb_samples[ich], nsamples * sizeof(int16_t));
            sbc->plc.scale[ich] = sb_scale[ich];
        }
    }
#endif

    synthesize(&sbc->dstates[0],
        sbc->nblocks, sbc->nsubbands,
        sb_samples[0], sb_scale[0], pcml, pitchl);

#if SBC_MAX_CHANNELS > 1
    if (sbc->nchannels > 1)
        synthesize(&sbc->dstates[1], sbc->nblocks, sbc->nsubbands,
            sb_samples[1], sb_scale[1], pcmr, pitchr);
#else
    (void)pcmr; (void)pitchr;
#endif

    return 0;
}


/* ----------------------------------------------------------------------------
 *  Encoding
 * ------------------------------------------------------------------------- */

/**
 * Compute the scale factors and joint-stereo mask from sub-band samples
 * frame           Frame description
 * sb_samples      Sub-band samples (fixed 17.14)
 * scale_factors   Output of sub-bands scale-factors
 * mjoint          Masque of joint sub-bands
 */
static void compute_scale_factors_js(const struct sbc_frame *frame,
    const int16_t (*sb_samples)[SBC_MAX_SAMPLES],
    int (*scale_factors)[SBC_MAX_SUBBANDS], unsigned *mjoint)
{
    /* As long as coding L+R / 2, L-R / 2 result at smaller
     * scale-factors, the bands are joint. */

    *mjoint = 0;

    for (int isb = 0; isb < frame->nsubbands; isb++) {
        unsigned m[2] = { }, mj[2] = { };

        for (int iblk = 0; iblk < frame->nblocks; iblk++) {
            int s0 = sb_samples[0][iblk * frame->nsubbands + isb];
            int s1 = sb_samples[1][iblk * frame->nsubbands + isb];

            m[0]  |= s0 < 0 ? ~s0 : s0;
            m[1]  |= s1 < 0 ? ~s1 : s1;

            mj[0] |= s0 + s1 < 0 ? ~(s0 + s1) : s0 + s1;
            mj[1] |= s0 - s1 < 0 ? ~(s0 - s1) : s0 - s1;
        }

        int scf0 = m[0] ? (8*sizeof(unsigned) - 1) - SBC_CLZ(m[0]) : 0;
        int scf1 = m[1] ? (8*sizeof(unsigned) - 1) - SBC_CLZ(m[1]) : 0;

        int js0 = mj[0] ? (8*sizeof(unsigned) - 1) - SBC_CLZ(mj[0]) : 0;
        int js1 = mj[1] ? (8*sizeof(unsigned) - 1) - SBC_CLZ(mj[1]) : 0;

        if (isb < frame->nsubbands-1 && js0 + js1 < scf0 + scf1) {
            *mjoint |= 1 << isb;
            scf0 = js0, scf1 = js1;
        }

        scale_factors[0][isb] = scf0;
        scale_factors[1][isb] = scf1;
    }
}

/**
 * Compute the scale factors from sub-band samples
 * frame           Frame description
 * sb_samples      Sub-band samples
 * scale_factors   Output of sub-bands scale-factors
 * mjoint          Masque of joint sub-bands (Joint-Stereo mode)
 */
static void compute_scale_factors(const struct sbc_frame *frame,
    const int16_t (*sb_samples)[SBC_MAX_SAMPLES],
    int (*scale_factors)[SBC_MAX_SUBBANDS])
{
    for (int ich = 0; ich < NCH(frame); ich++)
        for (int isb = 0; isb < frame->nsubbands; isb++) {
            unsigned m = 0;

            for (int iblk = 0; iblk < frame->nblocks; iblk++) {
                int s = sb_samples[ich][iblk * frame->nsubbands + isb];

                m |= s < 0 ? ~s : s;
            }

            int scf = m ? (8*sizeof(unsigned) - 1) - SBC_CLZ(m) : 0;
            scale_factors[ich][isb] = scf;
        }
}

/**
 * Encode the 4 bytes frame header
 * bits            Bitstream writer
 * frame           Frame description to encode
 */
static void encode_header(sbc_bits_t *bits, const struct sbc_frame *frame)
{
    static const int enc_freq[SBC_NUM_FREQ] SBC_MERGE("hdr16_enc_freq", 16) = {
        /* SBC_FREQ_16K  */ 0, /* SBC_FREQ_32K  */ 1,
        /* SBC_FREQ_44K1 */ 2, /* SBC_FREQ_48K  */ 3
    };

    static const int enc_mode[SBC_NUM_MODE] SBC_MERGE("hdr16_enc_mode", 16) = {
        /* SBC_MODE_MONO   */ 0, /* SBC_MODE_DUAL_CHANNEL */ 1,
        /* SBC_MODE_STEREO */ 2, /* SBC_MODE_JOINT_STEREO */ 3 };

    static const int enc_bam[SBC_NUM_BAM] SBC_MERGE("hdr8_enc_bam", 8) = {
        /* SBC_BAM_LOUDNESS */ 0, /* SBC_BAM_SNR */ 1 };

    /* Two possible headers :
     * - Header, with syncword 0x9c (A2DP)
     * - mSBC header, with syncword 0xad (HFP) */

    SBC_WITH_BITS(bits);

    SBC_PUT_BITS("syncword", frame->msbc ? 0xad : 0x9c, 8);

    if (!frame->msbc) {
        SBC_PUT_BITS("sampling_frequency", enc_freq[frame->freq], 2);
        SBC_PUT_BITS("blocks", (frame->nblocks >> 2) - 1, 2);
        SBC_PUT_BITS("channel_mode", enc_mode[frame->mode], 2);
        SBC_PUT_BITS("allocation_method", enc_bam[frame->bam], 1);
        SBC_PUT_BITS("subbands", (frame->nsubbands >> 2) - 1, 1);
        SBC_PUT_BITS("bitpool", frame->bitpool, 8);
    } else
        SBC_PUT_BITS("reserved", 0, 16);

    SBC_PUT_BITS("crc_check", 0, 8);

    SBC_END_WITH_BITS();
}

/**
 * Put the CRC of the frame
 * frame           Frame description
 * data, size      Frame data, and maximum writable size
 * return          0: On success  -1: size too small
 */
static int put_crc(const struct sbc_frame *frame, void *data, unsigned size)
{
    int crc = compute_crc(frame, data, size);
    return crc < 0 ? -1 : (((uint8_t *)data)[3] = crc), 0;
}

/**
 * Encode frame data
 * bits            Bitstream writer
 * frame           Frame description
 * sb_samples      Sub-band samples, by channels
 */
static void encode_frame(sbc_bits_t *bits,
    const struct sbc_frame *frame, int16_t (*sb_samples)[SBC_MAX_SAMPLES] )
{
    SBC_WITH_BITS(bits);

    /* --- Compute Scale Factors --- */

    int scale_factors[2][SBC_MAX_SUBBANDS];
    unsigned mjoint = 0;

    if (IS_MODE(frame, SBC_MODE_JOINT_STEREO))
        compute_scale_factors_js(frame, sb_samples, scale_factors, &mjoint);
    else
        compute_scale_factors(frame, sb_samples, scale_factors);

    /* --- Joint-Stereo mask --- */

    if (IS_MODE(frame, SBC_MODE_JOINT_STEREO) && frame->nsubbands == 4)
        SBC_PUT_BITS("join[]",
            ((mjoint & 0x01) << 3) | ((mjoint & 0x02) << 1) |
            ((mjoint & 0x04) >> 1) | ((         0x00) >> 3)  , 4);

    else if (IS_MODE(frame, SBC_MODE_JOINT_STEREO))
        SBC_PUT_BITS("join[]",
            ((mjoint & 0x01) << 7) | ((mjoint & 0x02) << 5) |
            ((mjoint & 0x04) << 3) | ((mjoint & 0x08) << 1) |
            ((mjoint & 0x10) >> 1) | ((mjoint & 0x20) >> 3) |
            ((mjoint & 0x40) >> 5) | ((         0x00) >> 7)  , 8);

    /* --- Encode Scale Factors --- */

    int nchannels = NCH(frame);
    int nsubbands = frame->nsubbands;
    int nbits[2][SBC_MAX_SUBBANDS];

    for (int ich = 0; ich < nchannels; ich++)
        for (int isb = 0; isb < nsubbands; isb++)
            SBC_PUT_BITS("scale_factor", scale_factors[ich][isb], 4);

    compute_nbits(frame, scale_factors, nbits);
    if (IS_MODE(frame, SBC_MODE_DUAL_CHANNEL))
        compute_nbits(frame, scale_factors + 1, nbits + 1);

    /* --- Coupling Joint-Stereo --- */

    for (int isb = 0; isb < nsubbands; isb++) {

        if (((mjoint >> isb) & 1) == 0)
            continue;

        for (int iblk = 0; iblk < frame->nblocks; iblk++) {
            int16_t s0 = sb_samples[0][iblk*nsubbands + isb];
            int16_t s1 = sb_samples[1][iblk*nsubbands + isb];

            sb_samples[0][iblk*nsubbands + isb] = (s0 + s1) >> 1;
            sb_samples[1][iblk*nsubbands + isb] = (s0 - s1) >> 1;
        }
    }

    /* --- Quantization ---
     *
     * The quantization is given by :
     *
     *                     sb_sample
     *   sb_sample = ( ( ------------- + 1 ) (2^nbit - 1) ) >> 1
     *                    2^(scf + 1)
     *
     * The number of bits coding a sample is limited to 16 bits,
     * thus the unquantized sub-bands samples cannot overflow */

    for (int iblk = 0; iblk < frame->nblocks; iblk++)
        for (int ich = 0; ich < nchannels; ich++)
            for (int isb = 0; isb < nsubbands; isb++) {
                int nbit = nbits[ich][isb];
                int scf = scale_factors[ich][isb];

                if (!nbit) continue;

                int s = sb_samples[ich][iblk*nsubbands + isb] ;
                int range = ~(UINT_MAX << nbit);

                SBC_PUT_BITS("audio_sample",
                    (((s * range) >> (scf + 1)) + range) >> 1, nbit);
            }


    /* --- Padding --- */

    int padding_nbits = 8 - (sbc_tell_bits(bits) % 8);
    SBC_PUT_BITS("padding_bits", 0, padding_nbits < 8 ? padding_nbits : 0);

    SBC_END_WITH_BITS();
}

#if SBC_WITH_4SB
/**
 * Tranform 4 PCM samples into 4 sub-bands samples of a channel
 * state           Previous PCM samples of the channel
 * in              PCM input samples
 * pitch           Number of PCM samples betwwen two consecutive
 * out             Output address of sub-band samples
 */
static void analyze_4(
    struct sbc_estate *state, const int16_t *in, int pitch, int16_t *out)
{
    /* --- Windowing coefficients (fixed 2.13) ---
     *
     * The table is duplicated and transposed to fit the circular
     * buffer of PCM samples, and DCT symmetry. */

    static const int16_t window[2][4][5*2] = {

    {       /*   0     1     2     3     4     0     1     2     3     4 */
    /* 0 */ {    0,  358, 4443,-4443, -358,    0,  358, 4443,-4443, -358 },
    /* 2 */ {   49,  946, 8082, -944,   61,   49,  946, 8082, -944,   61 },
    /* 1 */ {   18,  670, 6389,-2544, -100,   18,  670, 6389,-2544, -100 },
    /* 3 */ {   90, 1055, 9235,  201,  128,   90, 1055, 9235,  201,  128 },
    },      /* ---------------------------------------------------------- */

    {       /*   0     1     2     3     4     0     1     2     3     4 */
    /* 0 */ {  126,  848, 9644,  848,  126,  126,  848, 9644,  848,  126 },
    /* 2 */ {   61, -944, 8082,  946,   49,   61, -944, 8082,  946,   49 },
    /* 1 */ {  128,  201, 9235, 1055,   90,  128,  201, 9235, 1055,   90 },
    /* 3 */ { -100,-2544, 6389,  670,   18, -100,-2544, 6389,  670,   18 },
    },      /* ---------------------------------------------------------- */

    };

    /* --- Load PCM samples ---
     *
     * - To assist the windowing step, which is done by halves,
     *   the odd and even blocks of samples are separated
     *
     * - Rely on DCT symmetry :
     *
     *   h(k,i) = cos( (i + 1/2) (k - n/2) pi/n ) ,  n = 4
     *
     *   h( n-k,i) =  h(  k,i) , k = [0;n/2-1]
     *   h(2n-k,i) = -h(n+k,i) , k = [1;n/2-1]
     *
     *   The "symmetric" samples are stored closer, thus we arite samples
     *   in the order : [ 0, 2, (1, 3) ]  */

    int idx = state->idx >> 1, odd = state->idx & 1;

    int16_t (*x)[5] = state->x[odd];
    int in_idx = idx ? 5 - idx : 0;

    x[0][in_idx] = in[(3-0) * pitch];  x[1][in_idx] = in[(3-2) * pitch];
    x[2][in_idx] = in[(3-1) * pitch];  x[3][in_idx] = in[(3-3) * pitch];

    /* --- Process and window ---
     *          ___
     *          \
     *   y(i) = /__ x0[i + k*4] * w0[i + k*4]   , i = [0;3] , k = [0;4]
     *           k
     *          ___
     *          \
     *   y(i) = /__ x1[i + k*4] * w1[i + k*4]   , i = [4;7] , k = [0;4]
     *           k
     *
     *   x0()/w0()  Blcoks, windowing coefficients
     *   x1()/w1()  Blocks, windowing coefficients with inverse
     *              parity, starting the previous block.
     *
     * Symmetry of cosinus function :
     *
     *   h(0,i) =  h(4,i) = +/- pi/4 , h(2,i) =  1 , h(6,i) = 0
     *   h(1,i) =  h(3,i) , h( 5,i) = -h(7,i)
     *
     * Thus, we do :
     *   y'(0) = y(0) + y(4)   y'(2) = y(5) - y(7)
     *   y'(1) = y(1) + y(3)   y'(3) = y(2)        */

    const int16_t (*w0)[10] = (const int16_t (*)[10])(window[0][0] + idx);
    const int16_t (*w1)[10] = (const int16_t (*)[10])(window[1][0] + idx);
    int y0, y1, y2, y3; int16_t y[4];

    y0 = x[0][0] * w0[0][0] + x[0][1] * w0[0][1] +
         x[0][2] * w0[0][2] + x[0][3] * w0[0][3] +
         x[0][4] * w0[0][4] + state->y[0];

    state->y[0] =
         x[0][0] * w1[0][0] + x[0][1] * w1[0][1] +
         x[0][2] * w1[0][2] + x[0][3] * w1[0][3] +
         x[0][4] * w1[0][4]                       ;

    y1 = x[2][0] * w0[2][0] + x[2][1] * w0[2][1] +
         x[2][2] * w0[2][2] + x[2][3] * w0[2][3] +
         x[2][4] * w0[2][4] + x[3][0] * w0[3][0] +
         x[3][1] * w0[3][1] + x[3][2] * w0[3][2] +
         x[3][3] * w0[3][3] + x[3][4] * w0[3][4]  ;

    y2 = state->y[1];
    state->y[1] =
        x[2][0] * w1[2][0] + x[2][1] * w1[2][1] +
        x[2][2] * w1[2][2] + x[2][3] * w1[2][3] +
        x[2][4] * w1[2][4] - x[3][0] * w1[3][0] -
        x[3][1] * w1[3][1] - x[3][2] * w1[3][2] -
        x[3][3] * w1[3][3] - x[3][4] * w1[3][4]  ;

    y3 = x[1][0] * w0[1][0] + x[1][1] * w0[1][1] +
         x[1][2] * w0[1][2] + x[1][3] * w0[1][3] +
         x[1][4] * w0[1][4]                       ;


    y[0] = SBC_SAT16((y0 + (1 << 14)) >> 15);
    y[1] = SBC_SAT16((y1 + (1 << 14)) >> 15);
    y[2] = SBC_SAT16((y2 + (1 << 14)) >> 15);
    y[3] = SBC_SAT16((y3 + (1 << 14)) >> 15);

    state->idx = state->idx < 9 ? state->idx + 1 : 0;

    /* --- Output subbands coefficients ---
     *
     * With H(k,i) = sign(x(k,i)) cos(abs(x(k,i)) * pi/8),
     *      x(i,k) =
     *
     *   k\i |  0   1   2   3
     *  -----|----------------
     *     0 |  2  -2  -2   2  = h(0,i) =  h(4,i)
     *     1 |  1   3  -3  -1  = h(1,i) =  h(3,i)
     *     2 |  3  -1   1  -3  = h(5,i) = -h(7,i)
     *     3 |  0   0   0   0  = h(2,i)
     *
     *  h(k,i) = cos( (i + 1/2) (k - 4/2) pi/8 ) */

    static const int16_t cos8[4] SBC_MERGE("cos8_analyze4", 8) = { 8192, 7568, 5793, 3135 };
    int s0, s1, s2, s3;

    s0 =  y[0] * cos8[2] + y[1] * cos8[1] + y[2] * cos8[3] + y[3] * 8192;
    s1 = -y[0] * cos8[2] + y[1] * cos8[3] - y[2] * cos8[1] + y[3] * 8192;
    s2 = -y[0] * cos8[2] - y[1] * cos8[3] + y[2] * cos8[1] + y[3] * 8192;
    s3 =  y[0] * cos8[2] - y[1] * cos8[1] - y[2] * cos8[3] + y[3] * 8192;

    *(out++) = SBC_SAT16((s0 + (1 << 12)) >> 13);
    *(out++) = SBC_SAT16((s1 + (1 << 12)) >> 13);
    *(out++) = SBC_SAT16((s2 + (1 << 12)) >> 13);
    *(out++) = SBC_SAT16((s3 + (1 << 12)) >> 13);
}
#endif

/**
 * Tranform 8 PCM samples into 8 sub-bands samples of a channel
 * state           Previous PCM samples of the channel
 * in              PCM input samples
 * pitch           Number of PCM samples betwwen two consecutive
 * out             Output address of sub-band samples
 */
static void analyze_8(
    struct sbc_estate *state, const int16_t *in, int pitch, int16_t *out)
{
    /* --- Windowing coefficients (fixed 2.13) ---
     *
     * The table is transposed and "scrambled" to fit the circular
     * buffer of PCM samples, and DCT symmetry. */

    static const int16_t window[2][8][5*2] = {

    {       /*   0     1     2     3     4     0     1     2     3     4 */
    /* 0 */ {    0,  185, 2228,-2228, -185,    0,  185, 2228,-2228, -185 },
    /* 4 */ {   27,  480, 4039, -480,   30,   27,  480, 4039, -480,   30 },
    /* 1 */ {    5,  263, 2719,-1743, -115,    5,  263, 2719,-1743, -115 },
    /* 7 */ {   58,  502, 4764,  290,   69,   58,  502, 4764,  290,   69 },
    /* 2 */ {   11,  343, 3197,-1280,  -54,   11,  343, 3197,-1280,  -54 },
    /* 6 */ {   48,  532, 4612,   96,   65,   48,  532, 4612,   96,   65 },
    /* 3 */ {   18,  418, 3644, -856,   -6,   18,  418, 3644, -856,   -6 },
    /* 5 */ {   37,  521, 4367, -161,   53,   37,  521, 4367, -161,   53 },
    },      /* ---------------------------------------------------------- */

    {       /*   0     1     2     3     4     0     1     2     3     4 */
    /* 0 */ {   66,  424, 4815,  424,   66,   66,  424, 4815,  424,   66 },
    /* 4 */ {   30, -480, 4039,  480,   27,   30, -480, 4039,  480,   27 },
    /* 1 */ {   69,  290, 4764,  502,   58,   69,  290, 4764,  502,   58 },
    /* 7 */ { -115,-1743, 2719,  263,    5, -115,-1743, 2719,  263,    5 },
    /* 2 */ {   65,   96, 4612,  532,   48,   65,   96, 4612,  532,   48 },
    /* 6 */ {  -54,-1280, 3197,  343,   11,  -54,-1280, 3197,  343,   11 },
    /* 3 */ {   53, -161, 4367,  521,   37,   53, -161, 4367,  521,   37 },
    /* 5 */ {   -6, -856, 3644,  418,   18,   -6, -856, 3644,  418,   18 },
    },      /* ---------------------------------------------------------- */

    };

    /* --- Load PCM samples ---
     *
     * - To assist the windowing step, which is done by halves,
     *   the odd and even blocks of samples are separated
     *
     * - Rely on DCT symmetry :
     *
     *   h(k,i) = cos( (i + 1/2) (k - n/2) pi/n ) ,  n = 8
     *
     *   h( n-k,i) =  h(  k,i) , k = [0;n/2-1]
     *   h(2n-k,i) = -h(n+k,i) , k = [1;n/2-1]
     *
     *   The "symmetric" samples are stored closer, thus we arite samples
     *   in the order : [ 0, 4, (1, 7), (2, 6), (3, 5) ]  */

    int idx = state->idx >> 1, odd = state->idx & 1;

    int16_t (*x)[5] = state->x[odd];
    int in_idx = idx ? 5 - idx : 0;

    x[0][in_idx] = in[(7-0) * pitch];  x[1][in_idx] = in[(7-4) * pitch];
    x[2][in_idx] = in[(7-1) * pitch];  x[3][in_idx] = in[(7-7) * pitch];
    x[4][in_idx] = in[(7-2) * pitch];  x[5][in_idx] = in[(7-6) * pitch];
    x[6][in_idx] = in[(7-3) * pitch];  x[7][in_idx] = in[(7-5) * pitch];

    /* --- Process and window ---
     *          ___
     *          \
     *   y(i) = /__ x0[i + k*8] * w0[i + k*8]   , i = [0; 7] , k = [0;4]
     *           k
     *          ___
     *          \
     *   y(i) = /__ x1[i + k*8] * w1[i + k*8]   , i = [8;15] , k = [0;4]
     *           k
     *
     *   x0()/w0()  Blocks, windowing coefficients
     *   x1()/w1()  Blocks, windowing coefficients with inverse
     *              parity, starting the previous block.
     *
     * Symmetry of cosinus function :
     *
     *   h( 0,i) =  h( 8,i) = +/- pi/4 , h( 4,i) =  1 , h(12,i) = 0
     *   h( 1,i) =  h( 7,i) , h( 2,i) =  h( 6,i) , h( 3,i) =  h( 5,i)
     *   h( 9,i) = -h(15,i) , h(10,i) = -h(14,i) , h(12,i) = -h(13,i)
     *
     * Thus, we do :
     *   y'(0) = y(0) + y(8)   y'(4) = y( 9) - y(15)
     *   y'(1) = y(1) + y(7)   y'(5) = y(10) - y(14)
     *   y'(2) = y(2) + y(6)   y'(6) = y(11) - y(13)
     *   y'(3) = y(3) + y(5)   y'(7) = y( 4)          */

    const int16_t (*w0)[10] = (const int16_t (*)[10])(window[0][0] + idx);
    const int16_t (*w1)[10] = (const int16_t (*)[10])(window[1][0] + idx);
    int y0, y1, y2, y3, y4, y5, y6, y7; int16_t y[8];

#if SBC_DSP
    y0 = sbc_dot5(x[0], w0[0], state->y[0]);
    state->y[0] = sbc_dot5(x[0], w1[0], 0);
    y1 = sbc_dot5(x[3], w0[3], sbc_dot5(x[2], w0[2], 0));
    y4 = state->y[1];
    state->y[1] = sbc_dot5(x[2], w1[2], 0) - sbc_dot5(x[3], w1[3], 0);
    y2 = sbc_dot5(x[5], w0[5], sbc_dot5(x[4], w0[4], 0));
    y5 = state->y[2];
    state->y[2] = sbc_dot5(x[4], w1[4], 0) - sbc_dot5(x[5], w1[5], 0);
    y3 = sbc_dot5(x[7], w0[7], sbc_dot5(x[6], w0[6], 0));
    y6 = state->y[3];
    state->y[3] = sbc_dot5(x[6], w1[6], 0) - sbc_dot5(x[7], w1[7], 0);
    y7 = sbc_dot5(x[1], w0[1], 0);
#else
    y0 = x[0][0] * w0[0][0] + x[0][1] * w0[0][1] +
         x[0][2] * w0[0][2] + x[0][3] * w0[0][3] +
         x[0][4] * w0[0][4] + state->y[0];

    state->y[0] =
         x[0][0] * w1[0][0] + x[0][1] * w1[0][1] +
         x[0][2] * w1[0][2] + x[0][3] * w1[0][3] +
         x[0][4] * w1[0][4]                       ;

    y1 = x[2][0] * w0[2][0] + x[2][1] * w0[2][1] +
         x[2][2] * w0[2][2] + x[2][3] * w0[2][3] +
         x[2][4] * w0[2][4] + x[3][0] * w0[3][0] +
         x[3][1] * w0[3][1] + x[3][2] * w0[3][2] +
         x[3][3] * w0[3][3] + x[3][4] * w0[3][4]  ;

    y4 = state->y[1];
    state->y[1] =
         x[2][0] * w1[2][0] + x[2][1] * w1[2][1] +
         x[2][2] * w1[2][2] + x[2][3] * w1[2][3] +
         x[2][4] * w1[2][4] - x[3][0] * w1[3][0] -
         x[3][1] * w1[3][1] - x[3][2] * w1[3][2] -
         x[3][3] * w1[3][3] - x[3][4] * w1[3][4]  ;

    y2 = x[4][0] * w0[4][0] + x[4][1] * w0[4][1] +
         x[4][2] * w0[4][2] + x[4][3] * w0[4][3] +
         x[4][4] * w0[4][4] + x[5][0] * w0[5][0] +
         x[5][1] * w0[5][1] + x[5][2] * w0[5][2] +
         x[5][3] * w0[5][3] + x[5][4] * w0[5][4]  ;

    y5 = state->y[2];
    state->y[2] =
         x[4][0] * w1[4][0] + x[4][1] * w1[4][1] +
         x[4][2] * w1[4][2] + x[4][3] * w1[4][3] +
         x[4][4] * w1[4][4] - x[5][0] * w1[5][0] -
         x[5][1] * w1[5][1] - x[5][2] * w1[5][2] -
         x[5][3] * w1[5][3] - x[5][4] * w1[5][4]  ;

    y3 = x[6][0] * w0[6][0] + x[6][1] * w0[6][1] +
         x[6][2] * w0[6][2] + x[6][3] * w0[6][3] +
         x[6][4] * w0[6][4] + x[7][0] * w0[7][0] +
         x[7][1] * w0[7][1] + x[7][2] * w0[7][2] +
         x[7][3] * w0[7][3] + x[7][4] * w0[7][4]  ;

    y6 = state->y[3];
    state->y[3] =
        x[6][0] * w1[6][0] + x[6][1] * w1[6][1] +
        x[6][2] * w1[6][2] + x[6][3] * w1[6][3] +
        x[6][4] * w1[6][4] - x[7][0] * w1[7][0] -
        x[7][1] * w1[7][1] - x[7][2] * w1[7][2] -
        x[7][3] * w1[7][3] - x[7][4] * w1[7][4]  ;

    y7 = x[1][0] * w0[1][0] + x[1][1] * w0[1][1] +
         x[1][2] * w0[1][2] + x[1][3] * w0[1][3] +
         x[1][4] * w0[1][4]                       ;

#endif

    y[0] = SBC_SAT16((y0 + (1 << 14)) >> 15);
    y[1] = SBC_SAT16((y1 + (1 << 14)) >> 15);
    y[2] = SBC_SAT16((y2 + (1 << 14)) >> 15);
    y[3] = SBC_SAT16((y3 + (1 << 14)) >> 15);
    y[4] = SBC_SAT16((y4 + (1 << 14)) >> 15);
    y[5] = SBC_SAT16((y5 + (1 << 14)) >> 15);
    y[6] = SBC_SAT16((y6 + (1 << 14)) >> 15);
    y[7] = SBC_SAT16((y7 + (1 << 14)) >> 15);

    state->idx = state->idx < 9 ? state->idx + 1 : 0;

    /* --- Output subbands coefficients ---
     *
     * With H(k,i) = sign(x(k,i)) cos(abs(x(k,i)) * pi/16),
     *      x(i,k) =
     *
     *   k\i |  0   1   2   3   4   5   6   7
     *  -----|--------------------------------
     *     0 |  4  -4  -4   4   4  -4  -4   4  = h( 0,i) =  h( 8,i)
     *     1 |  3  -7  -1  -5   5   1   7  -3  = h( 1,i) =  h( 7,i)
     *     2 |  2   6  -6  -2  -2  -6   6   2  = h( 2,i) =  h( 6,i)
     *     3 |  1   3   5   7  -7  -5  -3  -1  = h( 3,i) =  h( 5,i)
     *       |
     *     4 |  5  -1   7   3  -3  -7   1  -5  = h( 9,i) = -h(15,i)
     *     5 |  6  -2   2  -6  -6   2  -2   6  = h(10,i) = -h(14,i)
     *     6 |  7  -5   3  -1   1  -3   5  -7  = h(11,i) = -h(13,i)
     *     7 |  0   0   0   0   0   0   0   0  = h( 4,i)
     *
     *  h(k,i) = cos( (i + 1/2) (k - 8/2) pi/8 ) */

    static const int16_t cosmat[8][8] = {
        /* 0 */ {  5793,  6811,  7568,  8035,   4551,  3135,  1598, 8192 },
        /* 1 */ { -5793, -1598,  3135,  6811,  -8035, -7568, -4551, 8192 },
        /* 2 */ { -5793, -8035, -3135,  4551,   1598,  7568,  6811, 8192 },
        /* 3 */ {  5793, -4551, -7568,  1598,   6811, -3135, -8035, 8192 },
        /* 4 */ {  5793,  4551, -7568, -1598,  -6811, -3135,  8035, 8192 },
        /* 5 */ { -5793,  8035, -3135, -4551,  -1598,  7568, -6811, 8192 },
        /* 6 */ { -5793,  1598,  3135, -6811,   8035, -7568,  4551, 8192 },
        /* 7 */ {  5793, -6811,  7568, -8035,  -4551,  3135, -1598, 8192 },
    };

#if SBC_FAST_DCT
    /* Rows i and 7-i share the even-column products and negate the odd ones;
     * the even part is a rotation. 21 multiplies instead of 64, exact. */
    {
        int t0 = y[0] * 5793, t7 = y[7] * 8192;
        int p = y[2] * 7568 + y[5] * 3135;
        int q = y[2] * 3135 - y[5] * 7568;
        int e[4] = { t7 + t0 + p, t7 - t0 + q, t7 - t0 - q, t7 + t0 - p };
        int o[4] = {
             y[1] * 6811 + y[3] * 8035 + y[4] * 4551 + y[6] * 1598,
            -y[1] * 1598 + y[3] * 6811 - y[4] * 8035 - y[6] * 4551,
            -y[1] * 8035 + y[3] * 4551 + y[4] * 1598 + y[6] * 6811,
            -y[1] * 4551 + y[3] * 1598 + y[4] * 6811 - y[6] * 8035 };
        for (int i = 0; i < 4; i++) {
            out[i]     = SBC_SAT16((e[i] + o[i] + (1 << 12)) >> 13);
            out[7 - i] = SBC_SAT16((e[i] - o[i] + (1 << 12)) >> 13);
        }
        (void)cosmat;
    }
#else
    for (int i = 0; i < 8; i++) {
        int s = y[0] * cosmat[i][0] + y[1] * cosmat[i][1] +
                y[2] * cosmat[i][2] + y[3] * cosmat[i][3] +
                y[4] * cosmat[i][4] + y[5] * cosmat[i][5] +
                y[6] * cosmat[i][6] + y[7] * cosmat[i][7]  ;
        *(out++) = SBC_SAT16((s + (1 << 12)) >> 13);
    }
#endif
}

/**
 * Tranform PCM samples into sub-bands samples of a channel
 * state           Previous PCM samples of the channel
 * frame           Frame description
 * in              PCM input samples
 * pitch           Number of samples between two consecutive
 * out             Sub-band samples
 */
static inline void analyze(struct sbc_estate *state,
    const struct sbc_frame *frame, const int16_t *in, int pitch, int16_t *out)
{
    for (int iblk = 0; iblk < frame->nblocks; iblk++) {

#if SBC_WITH_4SB
        if (frame->nsubbands == 4)
            analyze_4(state, in, pitch, out);
        else
#endif
            analyze_8(state, in, pitch, out);

        in += frame->nsubbands * pitch;
        out += frame->nsubbands;
    }
}

/**
 * Encode a frame
 */
int sbc_encode(struct sbc *sbc,
    const int16_t *pcml, int pitchl, const int16_t *pcmr, int pitchr,
    const struct sbc_frame *frame, void *data, unsigned size)
{
    /* --- Override configuration on mSBC signaling --- */

    if (frame->msbc)
        frame = &msbc_frame;

    /* --- Check the given frame description --- */

    if (!check_frame(frame) || size < sbc_get_frame_size(frame))
        return -1;

    /* -- Analyse PCM samples --- */

    int16_t alignas(sizeof(int)) sb_samples[SBC_MAX_CHANNELS][SBC_MAX_SAMPLES];

    analyze(&sbc->estates[0], frame, pcml, pitchl, sb_samples[0]);
#if SBC_MAX_CHANNELS > 1
    if (NCH(frame) > 1)
        analyze(&sbc->estates[1], frame, pcmr, pitchr, sb_samples[1]);
#else
    (void)pcmr; (void)pitchr;
#endif

    /* --- Encode the frame --- */

    sbc_bits_t bits;

    sbc_setup_bits(&bits, SBC_BITS_WRITE,
        (void *)((uintptr_t)data + SBC_HEADER_SIZE),
        sbc_get_frame_size(frame) - SBC_HEADER_SIZE);
    encode_frame(&bits, frame, sb_samples);
    sbc_flush_bits(&bits);

    sbc_setup_bits(&bits, SBC_BITS_WRITE, data, SBC_HEADER_SIZE);
    encode_header(&bits, frame);
    sbc_flush_bits(&bits);

    put_crc(frame, data, size);

    return 0;
}
