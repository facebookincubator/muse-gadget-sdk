/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *  Copyright 2026 Meta Platforms, Inc. and affiliates (fixed-point port)
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
 ******************************************************************************/

#include <lc3.h>

#include "common.h"
#include "bits.h"

#include "attdet.h"
#include "bwdet.h"
#include "ltpf.h"
#include "mdct.h"
#include "energy.h"
#include "sns.h"
#include "tns.h"
#include "spec.h"
#include "plc.h"


/**
 * Frame side data
 */

struct side_data {
    enum lc3_bandwidth bw;
    bool pitch_present;
    lc3_ltpf_data_t ltpf;
    lc3_sns_data_t sns;
    lc3_tns_data_t tns;
    lc3_spec_side_t spec;
};


/* ----------------------------------------------------------------------------
 *  General
 * -------------------------------------------------------------------------- */

/**
 * Resolve frame duration in us
 * us              Frame duration in us
 * hrmode          High-resolution mode indication
 * return          Frame duration identifier, or LC3_NUM_DT
 */
/* Stages of LC3FX_MARK() (common.h):
 *  encoder  0 load, attack detection   1 MDCT   2 energy, bandwidth
 *           3 SNS scale factors   4 SNS codebooks, PVQ   5 SNS shaping
 *           6 TNS LPC analysis    7 TNS filtering
 *           8 gain estimation     9 quantization, bit count   10 gain adjust, noise level
 *          11 side information   12 spectrum coding
 *  decoder 16 side information   17 spectrum decoding   18 normalization
 *          19 TNS   20 SNS unquantization   21 SNS shaping   22 IMDCT   23 output */

#ifdef LC3FX_STAGES
uint32_t lc3fx_stage_t;
#endif

static enum lc3_dt resolve_dt_all(int us, bool hrmode)
{
    return LC3_PLUS && us ==  2500 ? LC3_DT_2M5 :
           LC3_PLUS && us ==  5000 ? LC3_DT_5M  :
           !hrmode  && us ==  7500 ? LC3_DT_7M5 :
                       us == 10000 ? LC3_DT_10M : LC3_NUM_DT;
}

/**
 * Resolve samplerate in Hz
 * hz              Samplerate in Hz
 * hrmode          High-resolution mode indication
 * return          Sample rate identifier, or LC3_NUM_SRATE
 */
static enum lc3_srate resolve_srate_all(int hz, bool hrmode)
{
    if (hrmode && !LC3_PLUS_HR)
        return LC3_NUM_SRATE;   /* no high-resolution mode: setup, sizes and queries refuse it */

    return !hrmode && hz ==  8000 ? LC3_SRATE_8K     :
           !hrmode && hz == 16000 ? LC3_SRATE_16K    :
           !hrmode && hz == 24000 ? LC3_SRATE_24K    :
           !hrmode && hz == 32000 ? LC3_SRATE_32K    :
           !hrmode && hz == 48000 ? LC3_SRATE_48K    :
            hrmode && hz == 48000 ? LC3_SRATE_48K_HR :
            hrmode && hz == 96000 ? LC3_SRATE_96K_HR : LC3_NUM_SRATE;
}
#define resolve_dt resolve_dt_all
#define resolve_srate resolve_srate_all

/* Build-time configuration trimming: configurations outside LC3_DT_MASK / LC3_SR_MASK are rejected, so their
 * code drops out. The tables here carry every configuration; the defaults let them all through. */
#ifndef LC3_DT_MASK
#define LC3_DT_MASK 0xf
#endif
#ifndef LC3_SR_MASK
#define LC3_SR_MASK 0x1f
#endif
static enum lc3_dt resolve_dt_m(int us, bool hrmode)
{
    enum lc3_dt dt = resolve_dt_all(us, hrmode);
    return dt < LC3_NUM_DT && ((LC3_DT_MASK >> dt) & 1) ? dt : LC3_NUM_DT;
}

static enum lc3_srate resolve_srate_m(int hz, bool hrmode)
{
    enum lc3_srate sr = resolve_srate_all(hz, hrmode);
    return sr < LC3_NUM_SRATE && ((LC3_SR_MASK >> sr) & 1) ? sr : LC3_NUM_SRATE;
}

/**
 * Return the number of PCM samples in a frame
 */
LC3_EXPORT int lc3_hr_frame_samples(bool hrmode, int dt_us, int sr_hz)
{
    enum lc3_dt dt = resolve_dt(dt_us, hrmode);
    enum lc3_srate sr = resolve_srate(sr_hz, hrmode);

    if (dt >= LC3_NUM_DT || sr >= LC3_NUM_SRATE)
        return -1;

    return lc3_ns(dt, sr);
}

LC3_EXPORT int lc3_frame_samples(int dt_us, int sr_hz)
{
    return lc3_hr_frame_samples(false, dt_us, sr_hz);
}

/**
 * Return the size of frames or frame blocks, from bitrate
 */
LC3_EXPORT int lc3_hr_frame_block_bytes(
    bool hrmode, int dt_us, int sr_hz, int nchannels, int bitrate)
{
    enum lc3_dt dt = resolve_dt(dt_us, hrmode);
    enum lc3_srate sr = resolve_srate(sr_hz, hrmode);

    if (dt >= LC3_NUM_DT || sr >= LC3_NUM_SRATE
            || nchannels < 1 || nchannels > 8 || bitrate < 0)
        return -1;

    bitrate = LC3_CLIP(bitrate, 0, 8*LC3_HR_MAX_BITRATE);

    return LC3_CLIP((bitrate * (int)(1 + dt)) / 3200,
        nchannels * lc3_min_frame_bytes(dt, sr),
        nchannels * lc3_max_frame_bytes(dt, sr) );
}

LC3_EXPORT int lc3_frame_block_bytes(int dt_us, int nchannels, int bitrate)
{
    return lc3_hr_frame_block_bytes(false, dt_us, 8000, nchannels, bitrate);
}

LC3_EXPORT int lc3_hr_frame_bytes(
    bool hrmode, int dt_us, int sr_hz, int bitrate)
{
    return lc3_hr_frame_block_bytes(hrmode, dt_us, sr_hz, 1, bitrate);
}

LC3_EXPORT int lc3_frame_bytes(int dt_us, int bitrate)
{
    return lc3_hr_frame_bytes(false, dt_us, 8000, bitrate);
}

/**
 * Resolve the bitrate, from the size of frames or frame blocks
 */
LC3_EXPORT int lc3_hr_resolve_bitrate(
    bool hrmode, int dt_us, int sr_hz, int nbytes)
{
    enum lc3_dt dt = resolve_dt(dt_us, hrmode);
    enum lc3_srate sr = resolve_srate(sr_hz, hrmode);

    if (dt >= LC3_NUM_DT || sr >= LC3_NUM_SRATE || nbytes < 0)
        return -1;

    return LC3_MIN(((int64_t)nbytes * 3200 + dt) / (1 + dt), INT_MAX);
}

LC3_EXPORT int lc3_resolve_bitrate(int dt_us, int nbytes)
{
    return lc3_hr_resolve_bitrate(false, dt_us, 8000, nbytes);
}

/**
 * Return algorithmic delay, as a number of samples
 */
LC3_EXPORT int lc3_hr_delay_samples(bool hrmode, int dt_us, int sr_hz)
{
    enum lc3_dt dt = resolve_dt(dt_us, hrmode);
    enum lc3_srate sr = resolve_srate(sr_hz, hrmode);

    if (dt >= LC3_NUM_DT || sr >= LC3_NUM_SRATE)
        return -1;

    return 2 * lc3_nd(dt, sr) - lc3_ns(dt, sr);
}

LC3_EXPORT int lc3_delay_samples(int dt_us, int sr_hz)
{
    return lc3_hr_delay_samples(false, dt_us, sr_hz);
}


/* ----------------------------------------------------------------------------
 *  Encoder
 * -------------------------------------------------------------------------- */

/**
 * Input PCM Samples from signed 16 bits
 * encoder         Encoder state
 * pcm, stride     Input PCM samples, and count between two consecutives
 */
static void load_s16(
    struct lc3_encoder *encoder, const void *_pcm, int stride)
{
    const int16_t *pcm = _pcm;

    enum lc3_dt dt = encoder->dt;
    enum lc3_srate sr = encoder->sr_pcm;

    int16_t *xt = encoder->x + encoder->xt_off;
    int ns = lc3_ns(dt, sr);

    if (stride == 1) {
        memcpy(xt, pcm, ns * sizeof(*xt));
        return;
    }

    for (int i = 0; i < ns; i++, pcm += stride)
        xt[i] = *pcm;
}

/**
 * Input PCM Samples from signed 24 bits (rounded to 16 bits)
 * encoder         Encoder state
 * pcm, stride     Input PCM samples, and count between two consecutives
 */
static void load_s24(
    struct lc3_encoder *encoder, const void *_pcm, int stride)
{
    const int32_t *pcm = _pcm;

    enum lc3_dt dt = encoder->dt;
    enum lc3_srate sr = encoder->sr_pcm;

    int16_t *xt = encoder->x + encoder->xt_off;
    int ns = lc3_ns(dt, sr);

    for (int i = 0; i < ns; i++, pcm += stride)
        xt[i] = LC3FX_SAT16((*pcm + (1 << 7)) >> 8);
}

/**
 * Input PCM Samples from signed 24 bits packed (rounded to 16 bits)
 * encoder         Encoder state
 * pcm, stride     Input PCM samples, and count between two consecutives
 */
static void load_s24_3le(
    struct lc3_encoder *encoder, const void *_pcm, int stride)
{
    const uint8_t *pcm = _pcm;

    enum lc3_dt dt = encoder->dt;
    enum lc3_srate sr = encoder->sr_pcm;

    int16_t *xt = encoder->x + encoder->xt_off;
    int ns = lc3_ns(dt, sr);

    for (int i = 0; i < ns; i++, pcm += 3*stride) {
        int32_t in = ((uint32_t)pcm[0] <<  8) |
                     ((uint32_t)pcm[1] << 16) |
                     ((uint32_t)pcm[2] << 24)  ;

        /* Round on the 24-bit value: `in + (1 << 15)` overflows near +full scale */
        xt[i] = LC3FX_SAT16(((in >> 8) + (1 << 7)) >> 8);
    }
}

/**
 * Frame Analysis
 * encoder         Encoder state
 * nbytes          Size in bytes of the frame
 * side            Return frame data
 * xf              Return the quantized spectrum (Q15)
 * buf             Scratch buffer
 */
static void analyze(struct lc3_encoder *encoder,
    int nbytes, struct side_data *side, int32_t *xf, int32_t *buf)
{
    enum lc3_dt dt = encoder->dt;
    enum lc3_srate sr = encoder->sr;
    enum lc3_srate sr_pcm = encoder->sr_pcm;

    int16_t *xt = encoder->x + encoder->xt_off;
    int16_t *xd = encoder->x + encoder->xd_off;
    int ns = lc3_ns(dt, sr_pcm);
    int nt = lc3_nt(sr_pcm);

    /* --- Temporal --- */

    bool att = lc3_attdet_run(dt, sr_pcm, nbytes, &encoder->attdet, xt);
    LC3FX_MARK(0);

    side->pitch_present = false;

    /* --- Spectral --- */

    uint32_t e[LC3_MAX_BANDS];
    int e_exp;

    int x_exp = lc3_mdct_forward(dt, sr, xt, xd, xf, buf);

    memmove(xt - nt, xt + (ns-nt), nt * sizeof(*xt));
    LC3FX_MARK(1);

    bool nn_flag = lc3_energy_compute(dt, sr, xf, x_exp, e, &e_exp, buf);
    lc3_ltpf_disable(&side->ltpf);

    side->bw = lc3_bwdet_run(dt, sr, e, e_exp);
    LC3FX_MARK(2);

    x_exp = lc3_sns_analyze(dt, sr, nbytes,
        e, e_exp, att, &side->sns, xf, x_exp, xf, 23, buf);
    LC3FX_MARK(5);

#ifndef LC3_ENC_TNS
#define LC3_ENC_TNS 1   /* 1: always, 0: never, 2: only on frames flagged by the attack detector */
#endif
    if (LC3_ENC_TNS == 1 || (LC3_ENC_TNS == 2 && att))
        lc3_tns_analyze(dt, side->bw, nn_flag, nbytes, &side->tns, xf);
    else
        lc3_tns_bypass(dt, side->bw, nbytes, &side->tns);
    LC3FX_MARK(7);

    lc3_spec_analyze(dt, sr,
        nbytes, side->pitch_present, &side->tns,
        &encoder->spec, xf, x_exp, &side->spec);
    LC3FX_MARK(10);
}

/**
 * Encode bitstream
 * encoder         Encoder state
 * side            The frame data
 * nbytes          Target size of the frame (20 to 400)
 * buffer          Output bitstream buffer of `nbytes` size
 * xf              Quantized spectrum (Q15)
 */
static void encode(struct lc3_encoder *encoder,
    const struct side_data *side, int nbytes, void *buffer, int32_t *xf)
{
    enum lc3_dt dt = encoder->dt;
    enum lc3_srate sr = encoder->sr;

    enum lc3_bandwidth bw = side->bw;

    lc3_bits_t bits;

    lc3_setup_bits(&bits, LC3_BITS_MODE_WRITE, buffer, nbytes);

    lc3_bwdet_put_bw(&bits, sr, bw);

    lc3_spec_put_side(&bits, dt, sr, &side->spec);

    lc3_tns_put_data(&bits, &side->tns);

    lc3_put_bit(&bits, side->pitch_present);

    lc3_sns_put_data(&bits, &side->sns);

    if (side->pitch_present)
        lc3_ltpf_put_data(&bits, &side->ltpf);

    LC3FX_MARK(11);
    lc3_spec_encode(&bits, dt, sr, bw, nbytes, &side->spec, xf);

    lc3_flush_bits(&bits);
    LC3FX_MARK(12);
}

/**
 * Return size needed for an encoder
 */
LC3_EXPORT unsigned lc3_hr_encoder_size(bool hrmode, int dt_us, int sr_hz)
{
    if (resolve_dt_m(dt_us, hrmode) >= LC3_NUM_DT ||
        resolve_srate_m(sr_hz, hrmode) >= LC3_NUM_SRATE)
        return 0;

    return sizeof(struct lc3_encoder) +
        (LC3_ENCODER_BUFFER_COUNT(dt_us, sr_hz)-1) * sizeof(int16_t);
}

LC3_EXPORT unsigned lc3_encoder_size(int dt_us, int sr_hz)
{
    return lc3_hr_encoder_size(false, dt_us, sr_hz);
}

/**
 * Setup encoder
 */
LC3_EXPORT struct lc3_encoder *lc3_hr_setup_encoder(
    bool hrmode, int dt_us, int sr_hz, int sr_pcm_hz, void *mem)
{
    if (sr_pcm_hz <= 0)
        sr_pcm_hz = sr_hz;

    enum lc3_dt dt = resolve_dt_m(dt_us, hrmode);
    enum lc3_srate sr = resolve_srate_m(sr_hz, hrmode);
    enum lc3_srate sr_pcm = resolve_srate_m(sr_pcm_hz, hrmode);

    /* lc3fx: the PCM samplerate must match the codec samplerate */

    if (dt >= LC3_NUM_DT || sr_pcm >= LC3_NUM_SRATE || sr != sr_pcm || !mem)
        return NULL;

    struct lc3_encoder *encoder = mem;
    int ns = lc3_ns(dt, sr_pcm);
    int nt = lc3_nt(sr_pcm);

    *encoder = (struct lc3_encoder){
        .dt = dt, .sr = sr,
        .sr_pcm = sr_pcm,

        .xt_off = nt,
        .xd_off = nt + ns,
    };

    memset(encoder->x, 0,
        LC3_ENCODER_BUFFER_COUNT(dt_us, sr_pcm_hz) * sizeof(int16_t));

    return encoder;
}

LC3_EXPORT struct lc3_encoder *lc3_setup_encoder(
    int dt_us, int sr_hz, int sr_pcm_hz, void *mem)
{
    return lc3_hr_setup_encoder(false, dt_us, sr_hz, sr_pcm_hz, mem);
}

LC3_EXPORT void lc3_encoder_disable_ltpf(
    struct lc3_encoder *encoder)
{
    encoder->ltpf_bypass = true;
}

/**
 * Encode a frame
 */
LC3_EXPORT int lc3_encode(struct lc3_encoder *encoder,
    enum lc3_pcm_format fmt, const void *pcm, int stride, int nbytes, void *out)
{
    static void (* const load[])(struct lc3_encoder *, const void *, int) = {
        [LC3_PCM_FORMAT_S16    ] = load_s16,
        [LC3_PCM_FORMAT_S24    ] = load_s24,
        [LC3_PCM_FORMAT_S24_3LE] = load_s24_3le,
        [LC3_PCM_FORMAT_FLOAT  ] = NULL,
    };

    /* --- Check parameters --- */

    if (!encoder || nbytes < lc3_min_frame_bytes(encoder->dt, encoder->sr)
                 || nbytes > lc3_max_frame_bytes(encoder->dt, encoder->sr)
                 || (unsigned)fmt > LC3_PCM_FORMAT_FLOAT || !load[fmt])
        return -1;

    /* --- Processing --- */

    struct side_data side;
    int32_t xf[LC3_MAX_NS];
    /* 8-byte aligned by hand, with a word of slack: compilers that realign
     * over-aligned stack arrays without slack on a 4-byte-aligned stack
     * overflowed an alignas(8) array here */
    int32_t buf_raw[LC3_MAX(LC3_MAX_NS, 144) + 1];
    int32_t *buf = (int32_t *)(((uintptr_t)buf_raw + 7) & ~(uintptr_t)7);

    LC3FX_MARK0();
    load[fmt](encoder, pcm, stride);

    analyze(encoder, nbytes, &side, xf, buf);

    encode(encoder, &side, nbytes, out, xf);

    return 0;
}


/* ----------------------------------------------------------------------------
 *  Decoder
 * -------------------------------------------------------------------------- */

/**
 * Output PCM Samples to signed 16 bits
 * decoder         Decoder state
 * y               Samples (Q12)
 * pcm, stride     Output PCM samples, and count between two consecutives
 */
static void store_s16(
    struct lc3_decoder *decoder, const int32_t *y, void *_pcm, int stride)
{
    int16_t *pcm = _pcm;

    enum lc3_dt dt = decoder->dt;
    enum lc3_srate sr = decoder->sr_pcm;
    int ns = lc3_ns(dt, sr);

    _Pragma("GCC unroll 4")
    for (int i = 0; i < ns; i++)
        pcm[i * stride] = LC3FX_SAT16((y[i] + (1 << 11)) >> 12);
}

/**
 * Output PCM Samples to signed 24 bits
 * decoder         Decoder state
 * y               Samples (Q12)
 * pcm, stride     Output PCM samples, and count between two consecutives
 */
static void store_s24(
    struct lc3_decoder *decoder, const int32_t *y, void *_pcm, int stride)
{
    int32_t *pcm = _pcm;

    enum lc3_dt dt = decoder->dt;
    enum lc3_srate sr = decoder->sr_pcm;
    int ns = lc3_ns(dt, sr);

    for ( ; ns > 0; ns--, y++, pcm += stride) {
        int32_t s = (*y + (1 << 3)) >> 4;
        *pcm = LC3_SAT24(s);
    }
}

/**
 * Output PCM Samples to signed 24 bits packed
 * decoder         Decoder state
 * y               Samples (Q12)
 * pcm, stride     Output PCM samples, and count between two consecutives
 */
static void store_s24_3le(
    struct lc3_decoder *decoder, const int32_t *y, void *_pcm, int stride)
{
    uint8_t *pcm = _pcm;

    enum lc3_dt dt = decoder->dt;
    enum lc3_srate sr = decoder->sr_pcm;
    int ns = lc3_ns(dt, sr);

    for ( ; ns > 0; ns--, y++, pcm += 3*stride) {
        int32_t s = (*y + (1 << 3)) >> 4;

        s = LC3_SAT24(s);
        pcm[0] = (s >>  0) & 0xff;
        pcm[1] = (s >>  8) & 0xff;
        pcm[2] = (s >> 16) & 0xff;
    }
}

/**
 * Decode bitstream
 * decoder         Decoder state
 * data, nbytes    Input bitstream buffer
 * side            Return the side data
 * x, g_int        Return the decoded spectrum (Q4), and global gain index
 * return          0: Ok  < 0: Bitsream error detected
 */
static int decode(struct lc3_decoder *decoder,
    const void *data, int nbytes, struct side_data *side,
    int32_t *x, int *g_int)
{
    enum lc3_dt dt = decoder->dt;
    enum lc3_srate sr = decoder->sr;

    int ns = lc3_ns(dt, sr);
    int ne = lc3_ne(dt, sr);

    lc3_bits_t bits;
    int ret = 0;

    lc3_setup_bits(&bits, LC3_BITS_MODE_READ, (void *)data, nbytes);

    if ((ret = lc3_bwdet_get_bw(&bits, sr, &side->bw)) < 0)
        return ret;

    if ((ret = lc3_spec_get_side(&bits, dt, sr, &side->spec)) < 0)
        return ret;

    if ((ret = lc3_tns_get_data(&bits, dt, side->bw, nbytes, &side->tns)) < 0)
        return ret;

    side->pitch_present = lc3_get_bit(&bits);

    if ((ret = lc3_sns_get_data(&bits, &side->sns)) < 0)
        return ret;

    if (side->pitch_present)
      lc3_ltpf_get_data(&bits, &side->ltpf);

    LC3FX_MARK(16);
    if ((ret = lc3_spec_decode(&bits, dt, sr,
                    side->bw, nbytes, &side->spec, x, g_int)) < 0)
        return ret;

    memset(x + ne, 0, (ns - ne) * sizeof(*x));

    return lc3_check_bits(&bits);
}

/**
 * Frame synthesis
 * decoder         Decoder state
 * side            Frame data, NULL performs PLC
 * nbytes          Size in bytes of the frame
 * g_int           Global gain index
 * buf0, buf1      Scratch buffers, `buf1` holds the decoded spectrum (Q4)
 * return          The synthesized samples (Q12)
 */
static int32_t *synthesize(struct lc3_decoder *decoder,
    const struct side_data *side, int nbytes, int g_int,
    int32_t *buf0, int32_t *buf1)
{
    enum lc3_dt dt = decoder->dt;
    enum lc3_srate sr = decoder->sr;
    enum lc3_srate sr_pcm = decoder->sr_pcm;

    int ns = lc3_ns(dt, sr_pcm);
    int ne = lc3_ne(dt, sr);

    int32_t *xf = buf1;
    int32_t *xg = decoder->x + decoder->xg_off;
    int32_t *xd = decoder->x + decoder->xd_off;
    int32_t *y;

    if (side) {
        enum lc3_bandwidth bw = side->bw;

        lc3_plc_suspend(&decoder->plc);

        /* --- Normalization (magnitude 2^22) and TNS ---
         * The spectrum is integer (Q4), the headroom of the TNS
         * synthesis filter is 2^9 */

        uint32_t m = 0;
        _Pragma("GCC unroll 4")
        for (int i = 0; i < ne; i++)
            m |= (uint32_t)(xf[i] ^ (xf[i] >> 31));

        int sh = m ? 22 - lc3fx_nbits(m) : 0;
        if (sh > 0) {
            _Pragma("GCC unroll 2")
            for (int i = 0; i < ne; i++) xf[i] = LC3FX_SHL(xf[i], sh);
        } else if (sh < 0) {
            _Pragma("GCC unroll 1")
            for (int i = 0; i < ne; i++) xf[i] >>= -sh;
        }

        LC3FX_MARK(18);
        lc3_tns_synthesize(dt, bw, &side->tns, xf);
        LC3FX_MARK(19);

        /* --- SNS, with the global gain 10^(g_int/28) = 2^(gi + gf) --- */

        /* In 64 bits: the log2 reaches +/-30, past the +/-16 of a Q27 int32 */
        int64_t lg = ((int64_t)g_int * 254778081) >> 4;
        int32_t gf = (int32_t)(lg & ((1 << 27) - 1));
        int gi = (int)(lg >> 27);

        decoder->xg_exp = lc3_sns_synthesize(dt, sr,
            &side->sns, gf, xf, -4 - sh + gi, xg, 28, buf0);
        LC3FX_MARK(21);

        y = lc3_mdct_inverse(dt, sr_pcm, xg, decoder->xg_exp, xd, buf0, buf1);
        LC3FX_MARK(22);

    } else {
        lc3_plc_synthesize(dt, sr, &decoder->plc, xg, xf);

        memset(xf + ne, 0, (ns - ne) * sizeof(*xf));

        y = lc3_mdct_inverse(dt, sr_pcm, xf, decoder->xg_exp, xd, buf0, buf1);
    }

#if !defined(LC3_DEC_LTPF) || LC3_DEC_LTPF
    int32_t *xs = decoder->x + decoder->xs_off;
    int32_t *xh = decoder->x + decoder->xh_off;

    memcpy(xs, y, ns * sizeof(*xs));

    lc3_ltpf_synthesize(dt, sr_pcm, nbytes, &decoder->ltpf,
        side && side->pitch_present ? &side->ltpf : NULL, xh, xs);

    y = xs;
#else
    (void)nbytes;
#endif

    return y;
}

/**
 * Update decoder state on decoding completion
 * decoder         Decoder state
 */
static void complete(struct lc3_decoder *decoder)
{
#if !defined(LC3_DEC_LTPF) || LC3_DEC_LTPF
    enum lc3_dt dt = decoder->dt;
    enum lc3_srate sr_pcm = decoder->sr_pcm;
    int nh = lc3_nh(dt, sr_pcm);
    int ns = lc3_ns(dt, sr_pcm);

    decoder->xs_off = decoder->xs_off - decoder->xh_off < nh ?
        decoder->xs_off + ns : decoder->xh_off;
#else
    (void)decoder;
#endif
}

/**
 * Return size needed for a decoder
 */
LC3_EXPORT unsigned lc3_hr_decoder_size(bool hrmode, int dt_us, int sr_hz)
{
    if (resolve_dt_m(dt_us, hrmode) >= LC3_NUM_DT ||
        resolve_srate_m(sr_hz, hrmode) >= LC3_NUM_SRATE)
        return 0;

    return sizeof(struct lc3_decoder) +
        (LC3_DECODER_BUFFER_COUNT(dt_us, sr_hz)-1) * sizeof(int32_t);
}

LC3_EXPORT unsigned lc3_decoder_size(int dt_us, int sr_hz)
{
    return lc3_hr_decoder_size(false, dt_us, sr_hz);
}

/**
 * Setup decoder
 */
LC3_EXPORT struct lc3_decoder *lc3_hr_setup_decoder(
    bool hrmode, int dt_us, int sr_hz, int sr_pcm_hz, void *mem)
{
    if (sr_pcm_hz <= 0)
        sr_pcm_hz = sr_hz;

    enum lc3_dt dt = resolve_dt_m(dt_us, hrmode);
    enum lc3_srate sr = resolve_srate_m(sr_hz, hrmode);
    enum lc3_srate sr_pcm = resolve_srate_m(sr_pcm_hz, hrmode);

    /* lc3fx: the PCM samplerate must match the codec samplerate */

    if (dt >= LC3_NUM_DT || sr_pcm >= LC3_NUM_SRATE || sr != sr_pcm || !mem)
        return NULL;

    struct lc3_decoder *decoder = mem;
    int ns = lc3_ns(dt, sr_pcm);
    int nd = lc3_nd(dt, sr_pcm);

#if !defined(LC3_DEC_LTPF) || LC3_DEC_LTPF
    int nh = lc3_nh(dt, sr_pcm);

    *decoder = (struct lc3_decoder){
        .dt = dt, .sr = sr,
        .sr_pcm = sr_pcm,

        .xh_off = 0,
        .xs_off = nh,
        .xd_off = nh + ns,
        .xg_off = nh + ns + nd,
    };
#else
    *decoder = (struct lc3_decoder){
        .dt = dt, .sr = sr,
        .sr_pcm = sr_pcm,

        .xd_off = 0,
        .xg_off = nd,
    };
    (void)ns;
#endif

    lc3_plc_reset(&decoder->plc);

    memset(decoder->x, 0,
        LC3_DECODER_BUFFER_COUNT(dt_us, sr_pcm_hz) * sizeof(int32_t));

    return decoder;
}

LC3_EXPORT struct lc3_decoder *lc3_setup_decoder(
    int dt_us, int sr_hz, int sr_pcm_hz, void *mem)
{
    return lc3_hr_setup_decoder(false, dt_us, sr_hz, sr_pcm_hz, mem);
}

/**
 * Decode a frame
 */
LC3_EXPORT int lc3_decode(struct lc3_decoder *decoder,
    const void *in, int nbytes, enum lc3_pcm_format fmt, void *pcm, int stride)
{
    static void (* const store[])(
        struct lc3_decoder *, const int32_t *, void *, int) =
    {
        [LC3_PCM_FORMAT_S16    ] = store_s16,
        [LC3_PCM_FORMAT_S24    ] = store_s24,
        [LC3_PCM_FORMAT_S24_3LE] = store_s24_3le,
        [LC3_PCM_FORMAT_FLOAT  ] = NULL,
    };

    /* --- Check parameters --- */

    if (!decoder || (unsigned)fmt > LC3_PCM_FORMAT_FLOAT || !store[fmt])
        return -1;

    if (in && (nbytes < LC3_MIN_FRAME_BYTES ||
               nbytes > lc3_max_frame_bytes(decoder->dt, decoder->sr) ))
        return -1;

    /* --- Processing --- */

    struct side_data side;
    int32_t buf0[LC3_MAX(LC3_MAX_NS, 144)], buf1[LC3_MAX_NS];
    int g_int = 0;

    LC3FX_MARK0();
    int ret = !in || (decode(decoder, in, nbytes, &side, buf1, &g_int) < 0);
    LC3FX_MARK(17);

    int32_t *y = synthesize(decoder, ret ? NULL : &side, nbytes, g_int, buf0, buf1);

    store[fmt](decoder, y, pcm, stride);
    LC3FX_MARK(23);

    complete(decoder);

    return ret;
}
