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

#ifndef __SBC_BITS_H
#define __SBC_BITS_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>


/**
 * Bitstream mode
 */
enum sbc_bits_mode
{
    SBC_BITS_READ = 0,
    SBC_BITS_WRITE,
};


/**
 * Private context
 */

typedef unsigned bits_accu_t;

/* SBC_UNITY: bits.c is included into sbc.c and its functions are inlined */
#if defined(SBC_UNITY) && SBC_UNITY
#define SBC_BITS_FN static inline __attribute__((always_inline))
#else
#define SBC_BITS_FN
#endif

typedef struct sbc_bits
{
    enum sbc_bits_mode mode;

    struct {
        uint8_t *p;
        unsigned nbytes;
        unsigned nleft;
    } data;

    struct {
        bits_accu_t v;
        unsigned nleft;
        unsigned nover;
    } accu;

    bool error;

} sbc_bits_t;


/**
 * Debug trace procedure, enable with `SBC_BITS_TRACE`
 */

#ifdef SBC_BITS_TRACE

#include <stdio.h>

#define sbc_trace(pos, name, val) \
  fprintf(stderr, "|%5u| %-32s : %4d\n", pos, name, val)

#endif /* SBC_BITS_TRACE */


/**
 * Helper macros on bitstream reading
 */

#define SBC_WITH_BITS(bits) \
    do { \
        sbc_bits_t *__bits = bits

#ifndef SBC_BITS_TRACE

#define SBC_GET_BITS(name, n)      sbc_get_bits(__bits, n)
#define SBC_GET_FIXED(name, n, v)  sbc_get_fixed(__bits, n, v)

#define SBC_PUT_BITS(name, v, n)   sbc_put_bits(__bits, v, n)

#else

#define SBC_GET_BITS(name, n)      sbc_get_named_bits(__bits, name, n)
#define SBC_GET_FIXED(name, n, v)  sbc_get_named_fixed(__bits, name, n, v)

#define SBC_PUT_BITS(name, v, n)   sbc_put_named_bits(__bits, name, v, n)

#endif

#define SBC_END_WITH_BITS() \
    } while(0)

/**
 * Setup bitstream context
 * bits            Context to setup
 * mode            Bitstream mode "read or write"
 * data            Data buffer to read or write
 * size            Number of bytes available
 */
SBC_BITS_FN void sbc_setup_bits(sbc_bits_t *bits,
    enum sbc_bits_mode mode, void *data, unsigned size);

/**
 * Return the bit position in the stream
 * bits            Bitstream context
 */
SBC_BITS_FN unsigned sbc_tell_bits(sbc_bits_t *bits);


/* ----------------------------------------------------------------------------
 * Reading
 * ------------------------------------------------------------------------- */

/**
 * Return true when a reading error is detected
 * return          True whenever an error has been detected
 */
static inline bool sbc_bits_error(sbc_bits_t *bits);

/**
 * Get from 0 to 32 bits
 * bits            Bitsream context
 * n               Number of bits to read (0 to 32)
 *
 * For 0 bit reading, the value 0 is returned
 * If there is no more data to read from the bistsream,
 * the missing bits are emulated with bits of value 0.
 */
static inline unsigned sbc_get_bits(sbc_bits_t *bits, unsigned n);

/**
 * Get from 1 to 32 bits, whose value is a constant
 * bits            Bitsream context
 * n               Number of bits to read (1 to 32)
 * val             Fixed value to check
 */
static inline void sbc_get_fixed(struct sbc_bits *bits, unsigned n, unsigned v);


/* ----------------------------------------------------------------------------
 * Writing
 * ------------------------------------------------------------------------- */

/**
 * Put from 0 to 32 bits
 * bits            Bitsream context
 * n               Number of bits to write (0 to 32)
 */
static inline void sbc_put_bits(sbc_bits_t *bits, unsigned v, unsigned n);

/**
 * Flush pending bytes
 * bits            Bitsream context
 */
SBC_BITS_FN void sbc_flush_bits(sbc_bits_t *bits);


/* ----------------------------------------------------------------------------
 * Inline implementations
 * ------------------------------------------------------------------------- */

static inline bool sbc_bits_error(struct sbc_bits *bits)
{
    return bits->error;
}

SBC_BITS_FN unsigned __sbc_get_bits(sbc_bits_t *, unsigned);
SBC_BITS_FN void __sbc_put_bits(sbc_bits_t *, unsigned, unsigned);

static inline unsigned sbc_get_bits(struct sbc_bits *bits, unsigned n)
{
    if (bits->accu.nleft < n)
      return __sbc_get_bits(bits, n);

    bits->accu.nleft -= n;
    return (bits->accu.v >> bits->accu.nleft) & ((1U << n) - 1);
}

static inline void sbc_put_bits(struct sbc_bits *bits, unsigned v, unsigned n)
{
    if (bits->accu.nleft < n)
        __sbc_put_bits(bits, v, n);
    else {
        bits->accu.nleft -= n;
        bits->accu.v = (bits->accu.v << n) | (v & ((1U << n) - 1));
    }
}

static inline void sbc_get_fixed(struct sbc_bits *bits, unsigned n, unsigned v)
{
    if (sbc_get_bits(bits, n) != v)
        bits->error = true;
}

#ifdef SBC_BITS_TRACE

static inline unsigned sbc_get_named_bits(
    struct sbc_bits *bits, const char *name, unsigned n)
{
    unsigned off = sbc_tell_bits(bits);
    unsigned val = sbc_get_bits(bits, n);
    return sbc_trace(off, name, val), val;
}

static inline void sbc_put_named_bits(
    struct sbc_bits *bits, const char *name, unsigned v, unsigned n)
{
    sbc_trace(sbc_tell_bits(bits), name, v);
    sbc_put_bits(bits, v, n);
}

static inline void sbc_get_named_fixed(struct sbc_bits *bits,
    const char *name, unsigned n, unsigned fixed_value)
{
    unsigned off = sbc_tell_bits(bits);
    unsigned val = sbc_get_bits(bits, n);

    if (val != fixed_value)
        bits->error = true;

    sbc_trace(off, name, val);
}

#endif /* SBC_BITS_TRACE */

#endif /* __SBC_BITS_H */
