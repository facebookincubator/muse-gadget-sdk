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
 *  Modified by Meta Platforms, Inc.: build-time trimming options
 *  (SBC_MAX_CHANNELS, SBC_PLC). See README.md in this
 *  directory.
 ******************************************************************************/

/**
 * Subband Codec (SBC)
 *
 * This implementation conforms to :
 *   Advanced Audio Distribution v1.3.2 - Appendix B
 *   Hands-Free Profile v1.8 - Appendix A
 *   Bluetooth Profile Specification
 *
 * ---
 *
 * Antoine SOULIER, Tempow / Google LLC
 *
 */

#ifndef __SBC_H
#define __SBC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdalign.h>


/**
 * Normative SBC Constants / Limits
 */

#define SBC_HEADER_SIZE   ( 4)

/* Build-time trimming (all default to the full codec)
 * SBC_MAX_CHANNELS  1: mono only (stereo / dual / joint-stereo compiled out)
 * SBC_PLC           1: packet loss concealment (below)
 *
 * These two change the size and layout of `sbc_t`: they are layout
 * definitions. Every file that includes this header, the application's as
 * well as sbc.c, must be built with the same values (pass them as public
 * compile definitions): a caller built with other values allocates a context
 * of the wrong size, which sbc.c then reads and writes past its end. */
#ifndef SBC_MAX_CHANNELS
#define SBC_MAX_CHANNELS 2
#endif
#define SBC_PROBE_SIZE SBC_HEADER_SIZE

#define SBC_MAX_SUBBANDS  ( 8)
#define SBC_MAX_BLOCKS    (16)
#define SBC_MAX_SAMPLES   (SBC_MAX_BLOCKS * SBC_MAX_SUBBANDS)

#define SBC_MSBC_SAMPLES  (120)
#define SBC_MSBC_SIZE     ( 57)


/**
 * SBC Frame description, include :
 * - The sampling frequency
 * - The channel mode
 * - The number of blocks, subbands encoded
 * - The bit allocation method and bitpool selected
 */

enum sbc_freq
{
    SBC_FREQ_16K,
    SBC_FREQ_32K,
    SBC_FREQ_44K1,
    SBC_FREQ_48K,

    SBC_NUM_FREQ
};

enum sbc_mode
{
    SBC_MODE_MONO,
    SBC_MODE_DUAL_CHANNEL,
    SBC_MODE_STEREO,
    SBC_MODE_JOINT_STEREO,

    SBC_NUM_MODE
};

enum sbc_bam
{
    SBC_BAM_LOUDNESS,
    SBC_BAM_SNR,

    SBC_NUM_BAM
};

struct sbc_frame
{
    bool msbc;
    enum sbc_freq freq;
    enum sbc_mode mode;
    enum sbc_bam bam;
    int nblocks, nsubbands;
    int bitpool;
};


/**
 * Algorithmic codec delay (Encoding + Decoding)
 * From number of subbands or frame configuration
 */

#define SBC_DELAY_SUBBANDS(nsubbands) \
    (10 * (nsubbands))

#define SBC_DELAY(frame) \
    SBC_DELAY_SUBBANDS((frame)->nsubbands)


/**
 * Private context
 */

struct sbc_dstate
{
    int idx;
    int16_t alignas(sizeof(int)) v[2][SBC_MAX_SUBBANDS][10];
};

struct sbc_estate
{
    int idx;
    int16_t alignas(sizeof(int)) x[2][SBC_MAX_SUBBANDS][5];
    int32_t y[4];
};

#ifndef SBC_PLC
#define SBC_PLC 0     /* 1: sub-band repeat + fade concealment (+ nch x 256 B) */
#endif

typedef struct sbc
{
    int nchannels;
    int nblocks, nsubbands;
#if SBC_PLC
    struct { int nlost; int scale[SBC_MAX_CHANNELS];
             int16_t sb[SBC_MAX_CHANNELS][SBC_MAX_SAMPLES]; } plc;
#endif

    union {
        struct sbc_dstate dstates[SBC_MAX_CHANNELS];
        struct sbc_estate estates[SBC_MAX_CHANNELS];
    };

} sbc_t;


/**
 * Return the sampling frequency in Hz
 * freq            The frequency enum value
 * return          The Hz frequency value
 */
int sbc_get_freq_hz(enum sbc_freq freq);

/**
 * Return the frame size from a frame description
 * frame           The frame description
 * return          The size of the frame, 0 on error
 */
unsigned sbc_get_frame_size(const struct sbc_frame *frame);

/**
 * Return the bitrate from a frame description
 * frame           The frame description
 * return          The bitrate in bps, 0 on error
 */
unsigned sbc_get_frame_bitrate(const struct sbc_frame *frame);

/**
 * Reset of the context
 * sbc             Context to reset
 */
void sbc_reset(sbc_t *sbc);

/**
 * Probe the data and return frame description
 * data            Data pointer with at least ̀`SBC_PROBE_SIZE` bytes
 * frame           Frame description
 * return          0 on success, -1 otherwise
 */
int sbc_probe(const void *data, struct sbc_frame *frame);

/**
 * Decode a frame
 * sbc             Decoding context
 * data, size      Frame data, and maximum readable size
 * frame           Return of frame description
 * pcmx            Output PCM buffer for channel L/R
 * pitchx          Number of samples between two consecutives
 * return          0 on success, -1 otherwise
 *
 * Each PCM buffer must hold SBC_MAX_SAMPLES (128) samples per channel at its
 * pitch, whatever the stream: a frame's own count (nblocks x nsubbands) is
 * known only once its header is read, and an mSBC frame's 120 samples
 * (SBC_MSBC_SAMPLES) is not the largest a stream can carry.
 *
 * `data` can be NULL to enable PLC emulation: it conceals with the last good
 * frame's layout, and returns -1 before the first good frame, or when that
 * frame had 2 channels and `pcmr` is NULL. As upstream, concealment doesn't
 * fill `frame`: a caller that needs the layout keeps the last good frame's
 */
int sbc_decode(sbc_t *sbc,
    const void *data, unsigned size, struct sbc_frame *frame,
    int16_t *pcml, int pitchl, int16_t *pcmr, int pitchr);

/**
 * Encode a frame
 * sbc             Encoding context
 * pcmx            Input PCM buffer for channel L/R
 * pitchx          Number of samples between two consecutives
 * frame           Frame description as encoding parameters
 * data, size      Output frame data, and maximum writable size
 * return          0 on success, -1 otherwise
 */
int sbc_encode(sbc_t *sbc,
    const int16_t *pcml, int pitchl, const int16_t *pcmr, int pitchr,
    const struct sbc_frame *frame, void *data, unsigned size);


#ifdef __cplusplus
}
#endif

#endif /* !__SBC_H */
