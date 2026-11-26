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
 *  Modified by Meta Platforms, Inc.: optional single-unit inlining
 *  (SBC_UNITY). See README.md in this directory.
 ******************************************************************************/

#include "bits.h"
#if !(defined(SBC_UNITY) && SBC_UNITY) || defined(SBC_BITS_IMPL)



/**
 * Accumulator bit size
 */

#define ACCU_NBITS (8 * sizeof(bits_accu_t))


/**
 * Bits extraction :
 * MASK  Mask of less-significant `n` bits
 * EXT   Extract the value `v` from bits `b(s+n-1)..b(s)`
 */

#define MASK(n)       ( (1U << (n)) - 1 )
#define EXT(v, s, n)  ( ((v) >> (s)) & MASK(n) )


/**
 * Sub-function of  "load_accu()", general way loading
 * bits            Bitsream context
 */
static void load_accu_slow(struct sbc_bits *bits)
{
    while (bits->accu.nleft < ACCU_NBITS - 7 && bits->data.nleft) {
        bits->accu.v = (bits->accu.v << 8) | *(bits->data.p++);
        bits->accu.nleft += 8;
        bits->data.nleft--;
    }

    if (bits->accu.nleft < ACCU_NBITS - 7)
    {
        unsigned nover = ((ACCU_NBITS - bits->accu.nleft) >> 3) << 3;

        if (bits->accu.nleft >= bits->accu.nover)
            bits->accu.nover += nover;
        else
            bits->accu.nover = UINT_MAX;

        if (nover < ACCU_NBITS)
            bits->accu.v <<= nover;
        else
            bits->accu.v = 0;

        bits->accu.nleft += nover;
    }
}

/**
 * Load the accumulator
 * bits            Bitsream context
 *
 * On return `8 * sizeof(bits_accu_t) - (accu.nleft % 8)` are available
 */
static inline void load_accu(struct sbc_bits *bits)
{
    unsigned nbytes = sizeof(bits_accu_t) - ((bits->accu.nleft + 7) >> 3);

    if (nbytes > bits->data.nleft) {
        load_accu_slow(bits);
        return;
    }

    bits->accu.nleft += nbytes << 3;
    bits->data.nleft -= nbytes;
    while (nbytes--)
        bits->accu.v = (bits->accu.v << 8) | *(bits->data.p++);
}

/**
 * Flush the accumulator
 * bits            Bitsream context
 *
 * On return `8 * sizeof(bits_accu_t) - (accu.nleft % 8)` are available
 */
static inline void flush_accu(struct sbc_bits *bits)
{
    unsigned nbytes = sizeof(bits_accu_t) - ((bits->accu.nleft + 7) >> 3);

    unsigned nflush = nbytes < bits->data.nleft ? nbytes : bits->data.nleft;
    bits->data.nleft -= nflush;

    for (int shr = (ACCU_NBITS - 8) - bits->accu.nleft; nflush--; shr -= 8)
        *(bits->data.p++) = bits->accu.v >> shr;

    bits->accu.v &= (1 << bits->accu.nleft) - 1;
    bits->accu.nleft += nbytes << 3;
}

/**
 * Setup bitstream reader context
 */
SBC_BITS_FN void sbc_setup_bits(struct sbc_bits *bits,
    enum sbc_bits_mode mode, void *data, unsigned size)
{
    /* field-wise init: a compound literal becomes a memset call on small cores */
    bits->mode = mode;
    bits->data.p = data; bits->data.nbytes = size; bits->data.nleft = size;
    bits->accu.v = 0; bits->accu.nover = 0;
    bits->accu.nleft = mode == SBC_BITS_READ ? 0 : 8*sizeof(bits_accu_t);
    bits->error = false;
}

/**
 * Return the bit position in the stream
 * bits            Bitstream context
 */
SBC_BITS_FN unsigned sbc_tell_bits(sbc_bits_t *bits)
{
    unsigned nbytes = bits->data.nbytes - bits->data.nleft;
    if (bits->mode == SBC_BITS_WRITE)
        nbytes += sizeof(bits_accu_t);

    return 8 * nbytes - ( bits->accu.nleft < bits->accu.nover ? 0 :
                          bits->accu.nleft - bits->accu.nover      );
}

/**
 * Get from 1 to 32 bits
 */
SBC_BITS_FN unsigned __sbc_get_bits(struct sbc_bits *bits, unsigned n)
{
    if (n > 32) n = 32;

    if (!bits->accu.nleft)
        load_accu(bits);

    if (bits->accu.nleft >= n)
        return (bits->accu.nleft -= n),
            EXT(bits->accu.v, bits->accu.nleft, n);

    n -= bits->accu.nleft;
    unsigned v = bits->accu.v & MASK(bits->accu.nleft);
    bits->accu.nleft = 0;

    load_accu(bits);

    return (bits->accu.nleft -= n),
        ((v << n) | EXT(bits->accu.v, bits->accu.nleft, n));
}

/**
 * Put from 1 to 32 bits
 */
SBC_BITS_FN void __sbc_put_bits(struct sbc_bits *bits, unsigned v, unsigned n)
{
    if (n > 32) n = 32;

    if (!bits->accu.nleft)
        flush_accu(bits);

    int m = bits->accu.nleft < n ? bits->accu.nleft : n;
    n -= m;

    bits->accu.nleft -= m;
    bits->accu.v = (bits->accu.v << m) | ((v >> n) & ((1U << m) - 1));

    if (n <= 0)
        return;

    flush_accu(bits);

    bits->accu.nleft -= n;
    bits->accu.v = (bits->accu.v << n) | (v & ((1U << n) - 1));
}

/**
 * Flush pending bytes
 */
SBC_BITS_FN void sbc_flush_bits(struct sbc_bits *bits)
{
    flush_accu(bits);
}

#endif
