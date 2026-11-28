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

#ifndef MG_SMP_DEPTH_H_
#define MG_SMP_DEPTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The SMP requests' CBOR nesting limit, checked before any handler decodes a
 * request (src/zephyr/smp.c with CONFIG_MG_DFU, src/mg_smp_req_gen.c with
 * CONFIG_MG_SMP_GATE_BOARD). zcbor skips an unknown map value by recursing
 * once per nested array or map, with no limit of its own, so a ~512 B echo
 * of ~500 nested arrays overflowed the SMP work queue's stack and rebooted
 * the gadget, before any proof. No SMP request the apps or the web updater
 * send nests deeper than 3.
 *
 * It fails closed: a request passes only as whole, well-formed CBOR inside
 * its own length. zcbor's length checks don't hold on a 32-bit target (its
 * str_overflow_check() casts a length to size_t, so 0x1_0000_0000 skips 0
 * bytes and the decode goes on into what follows), so anything the walk
 * can't account for byte by byte is refused rather than left to the
 * handler.
 */
#define MG_SMP_MAX_DEPTH 8

/* One CBOR body of n bytes: true only if it is whole, well-formed CBOR
 * items (any number, back to back) and no array or map nests deeper than
 * max. Refused: a length, count or tag number of 2^32 or more, or one that
 * runs past the body; a reserved additional-information value; an
 * indefinite length on anything but an array or map (no SMP client sends an
 * indefinite string, and zcbor decodes none); a break outside an indefinite
 * array or map; and an array, map or tag still open at the end. The walk is
 * iterative (a fixed stack of counts), and indefinite arrays and maps
 * (0x9f / 0xbf ... 0xff) count like definite ones. */
static inline bool mg_cbor_depth_ok(const uint8_t *p, size_t n, int max)
{
	/* Per open array or map: items still to come (-1: indefinite). */
	int64_t left[MG_SMP_MAX_DEPTH + 1];
	int depth = 0;
	bool tagged = false; /* a tag waits for the item it tags */
	size_t i = 0;

	if (max > MG_SMP_MAX_DEPTH) {
		max = MG_SMP_MAX_DEPTH;
	}
	while (i < n) {
		uint8_t b = p[i++];
		uint8_t major = b >> 5, ai = b & 0x1f;
		uint64_t v = ai;

		if (b == 0xff) { /* break: ends an indefinite array or map */
			if (tagged || depth == 0 || left[depth - 1] != -1) {
				return false;
			}
			depth--;
			goto item_done;
		}
		if (ai >= 24 && ai <= 27) {
			size_t len = (size_t)1 << (ai - 24);

			if (n - i < len) {
				return false;
			}
			v = 0;
			for (size_t k = 0; k < len; k++) {
				v = (v << 8) | p[i++];
			}
		} else if (ai >= 28 && (ai != 31 || (major != 4 && major != 5))) {
			return false;
		}
		/* A length, count or tag number: 32 bits at most (an integer or
		 * a float may be 64). */
		if (major >= 2 && major <= 6 && v > UINT32_MAX) {
			return false;
		}
		tagged = false;
		switch (major) {
		case 2:
		case 3:
			if (v > n - i) {
				return false;
			}
			i += (size_t)v;
			break;
		case 4:
		case 5:
			if (depth >= max) {
				return false;
			}
			if (ai == 31) {
				left[depth++] = -1;
				continue;
			}
			if (major == 5) {
				v *= 2; /* at most 2^33: no overflow */
			}
			if (v > n - i) {
				return false; /* each item takes a byte at least */
			}
			if (v == 0) {
				break; /* empty: done at once */
			}
			left[depth++] = (int64_t)v;
			continue;
		case 6:
			tagged = true;
			continue; /* the item it tags follows */
		default:
			break; /* integers, simple values, floats */
		}
item_done:
		/* One item done: close every definite container it completes. */
		while (depth > 0 && left[depth - 1] > 0) {
			if (--left[depth - 1] > 0) {
				break;
			}
			depth--;
		}
	}
	return depth == 0 && !tagged;
}

/* An SMP packet (8-byte headers, each followed by its body): true only if
 * every request in it is whole, with a body that passes mg_cbor_depth_ok()
 * inside its own length. Zephyr's reader decodes a body from its start to
 * the packet's end, not to its length, but stops after one whole item: a
 * whole body keeps it inside. An empty body is the packet's last request
 * only (its decode would start on the next request's header). */
static inline bool mg_smp_depth_ok(const uint8_t *p, size_t n)
{
	while (n > 0) {
		size_t len;

		if (n < 8) {
			return false;
		}
		len = ((size_t)p[2] << 8) | p[3];
		if (len > n - 8 || (len == 0 && n > 8) ||
		    !mg_cbor_depth_ok(p + 8, len, MG_SMP_MAX_DEPTH)) {
			return false;
		}
		p += 8 + len;
		n -= 8 + len;
	}
	return true;
}

#endif
