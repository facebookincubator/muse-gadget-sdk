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

#include "mgls_util.h"

#include <string.h>

/* ---- JSON reader --------------------------------------------------------- */

struct cur {
	char *p;
	char *end;
};

static void skip_ws(struct cur *c)
{
	while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
		c->p++;
	}
}

static int hex4(const char *s, unsigned int *out)
{
	unsigned int v = 0;

	for (int i = 0; i < 4; i++) {
		char ch = s[i];

		v <<= 4;
		if (ch >= '0' && ch <= '9') {
			v |= (unsigned int)(ch - '0');
		} else if (ch >= 'a' && ch <= 'f') {
			v |= (unsigned int)(ch - 'a' + 10);
		} else if (ch >= 'A' && ch <= 'F') {
			v |= (unsigned int)(ch - 'A' + 10);
		} else {
			return -1;
		}
	}
	*out = v;
	return 0;
}

/* At the opening quote. Decodes in place from just after it (the output
 * never overtakes the input) and NUL-terminates where the closing quote's
 * cell is at the latest. */
static int parse_string(struct cur *c, char **out, size_t *out_len)
{
	char *w;

	if (c->p >= c->end || *c->p != '"') {
		return -1;
	}
	c->p++;
	*out = w = c->p;
	while (c->p < c->end) {
		unsigned char ch = (unsigned char)*c->p;

		if (ch == '"') {
			*w = '\0';
			*out_len = (size_t)(w - *out);
			c->p++;
			return 0;
		}
		if (ch < 0x20) {
			return -1;
		}
		if (ch != '\\') {
			*w++ = (char)ch;
			c->p++;
			continue;
		}
		if (c->end - c->p < 2) {
			return -1;
		}
		char e = c->p[1];

		c->p += 2;
		switch (e) {
		case '"':
		case '\\':
		case '/':
			*w++ = e;
			break;
		case 'b':
			*w++ = '\b';
			break;
		case 'f':
			*w++ = '\f';
			break;
		case 'n':
			*w++ = '\n';
			break;
		case 'r':
			*w++ = '\r';
			break;
		case 't':
			*w++ = '\t';
			break;
		case 'u': {
			unsigned int u, lo;

			if (c->end - c->p < 4 || hex4(c->p, &u)) {
				return -1;
			}
			c->p += 4;
			if (u >= 0xDC00 && u <= 0xDFFF) {
				return -1; /* lone low surrogate */
			}
			if (u >= 0xD800 && u <= 0xDBFF) {
				if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u' ||
				    hex4(&c->p[2], &lo) || lo < 0xDC00 || lo > 0xDFFF) {
					return -1;
				}
				c->p += 6;
				u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
			}
			if (u == 0) {
				return -1;
			}
			/* At most 4 bytes out for 6 or 12 read. */
			if (u < 0x80) {
				*w++ = (char)u;
			} else if (u < 0x800) {
				*w++ = (char)(0xC0 | (u >> 6));
				*w++ = (char)(0x80 | (u & 0x3F));
			} else if (u < 0x10000) {
				*w++ = (char)(0xE0 | (u >> 12));
				*w++ = (char)(0x80 | ((u >> 6) & 0x3F));
				*w++ = (char)(0x80 | (u & 0x3F));
			} else {
				*w++ = (char)(0xF0 | (u >> 18));
				*w++ = (char)(0x80 | ((u >> 12) & 0x3F));
				*w++ = (char)(0x80 | ((u >> 6) & 0x3F));
				*w++ = (char)(0x80 | (u & 0x3F));
			}
			break;
		}
		default:
			return -1;
		}
	}
	return -1;
}

static bool is_digit(char ch)
{
	return ch >= '0' && ch <= '9';
}

static int parse_number(struct cur *c)
{
	if (c->p < c->end && *c->p == '-') {
		c->p++;
	}
	if (c->p >= c->end || !is_digit(*c->p)) {
		return -1;
	}
	if (*c->p == '0') {
		c->p++;
	} else {
		while (c->p < c->end && is_digit(*c->p)) {
			c->p++;
		}
	}
	if (c->p < c->end && *c->p == '.') {
		c->p++;
		if (c->p >= c->end || !is_digit(*c->p)) {
			return -1;
		}
		while (c->p < c->end && is_digit(*c->p)) {
			c->p++;
		}
	}
	if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
		c->p++;
		if (c->p < c->end && (*c->p == '+' || *c->p == '-')) {
			c->p++;
		}
		if (c->p >= c->end || !is_digit(*c->p)) {
			return -1;
		}
		while (c->p < c->end && is_digit(*c->p)) {
			c->p++;
		}
	}
	return 0;
}

static int literal(struct cur *c, const char *word, size_t n)
{
	if ((size_t)(c->end - c->p) < n || memcmp(c->p, word, n) != 0) {
		return -1;
	}
	c->p += n;
	return 0;
}

/* A scalar at c->p (not a container). */
static int parse_scalar(struct cur *c, uint8_t *type)
{
	char *s;
	size_t n;

	switch (*c->p) {
	case '"':
		*type = MGJ_STRING;
		return parse_string(c, &s, &n);
	case 't':
		*type = MGJ_TRUE;
		return literal(c, "true", 4);
	case 'f':
		*type = MGJ_FALSE;
		return literal(c, "false", 5);
	case 'n':
		*type = MGJ_NULL;
		return literal(c, "null", 4);
	default:
		*type = MGJ_NUMBER;
		return parse_number(c);
	}
}

static int parse_key(struct cur *c, char **k, size_t *kl)
{
	skip_ws(c);
	if (parse_string(c, k, kl)) {
		return -1;
	}
	skip_ws(c);
	if (c->p >= c->end || *c->p != ':') {
		return -1;
	}
	c->p++;
	return 0;
}

/* A nested object or array at c->p, validated without recursion: one bit
 * per open level (1 object, 0 array). depth0 levels are already open. */
static int skip_nested(struct cur *c, int depth0)
{
	uint32_t objs = 0;
	int depth = 0;
	uint8_t t;
	char *k;
	size_t kl;

	for (;;) {
		/* A value starts here. */
		skip_ws(c);
		if (c->p >= c->end) {
			return -1;
		}
		if (*c->p == '{' || *c->p == '[') {
			bool obj = *c->p == '{';

			if (depth0 + depth + 1 > MGJ_MAX_DEPTH) {
				return -1;
			}
			objs = (objs << 1) | (obj ? 1u : 0u);
			depth++;
			c->p++;
			skip_ws(c);
			if (c->p < c->end && *c->p == (obj ? '}' : ']')) {
				c->p++;
				objs >>= 1;
				depth--;
			} else {
				if (obj && parse_key(c, &k, &kl)) {
					return -1;
				}
				continue;
			}
		} else if (parse_scalar(c, &t)) {
			return -1;
		}
		/* After a value: close levels or go on to the next member. */
		for (;;) {
			if (depth == 0) {
				return 0;
			}
			skip_ws(c);
			if (c->p >= c->end) {
				return -1;
			}
			bool obj = (objs & 1u) != 0;

			if (*c->p == ',') {
				c->p++;
				if (obj && parse_key(c, &k, &kl)) {
					return -1;
				}
				break;
			}
			if (*c->p != (obj ? '}' : ']')) {
				return -1;
			}
			c->p++;
			objs >>= 1;
			depth--;
		}
	}
}

int mgj_parse(char *buf, size_t len, mgls_jfield_t *f, size_t maxf, uint8_t *nout)
{
	struct cur c = {buf, buf + len};
	size_t n = 0;

	*nout = 0;
	if (len >= 65536) {
		return -1;
	}
	skip_ws(&c);
	if (c.p >= c.end || *c.p != '{') {
		return -1;
	}
	c.p++;
	skip_ws(&c);
	if (c.p < c.end && *c.p == '}') {
		c.p++;
	} else {
		for (;;) {
			char *k;
			size_t kl;
			mgls_jfield_t fl;

			if (parse_key(&c, &k, &kl)) {
				return -1;
			}
			skip_ws(&c);
			if (c.p >= c.end) {
				return -1;
			}
			fl.key = (uint16_t)(k - buf);
			fl.pad = 0;
			char *v = c.p;

			if (*c.p == '{' || *c.p == '[') {
				fl.type = MGJ_NESTED;
				if (skip_nested(&c, 1)) {
					return -1;
				}
				fl.val = (uint16_t)(v - buf);
				fl.len = 0;
			} else if (*c.p == '"') {
				char *s;
				size_t sl;

				fl.type = MGJ_STRING;
				if (parse_string(&c, &s, &sl)) {
					return -1;
				}
				fl.val = (uint16_t)(s - buf);
				fl.len = (uint16_t)sl;
			} else {
				if (parse_scalar(&c, &fl.type)) {
					return -1;
				}
				fl.val = (uint16_t)(v - buf);
				fl.len = (uint16_t)(c.p - v);
			}
			if (mgj_get(buf, f, n, k) != NULL) {
				return -1; /* duplicate member */
			}
			if (n >= maxf) {
				return -1;
			}
			f[n++] = fl;
			skip_ws(&c);
			if (c.p < c.end && *c.p == ',') {
				c.p++;
				continue;
			}
			if (c.p < c.end && *c.p == '}') {
				c.p++;
				break;
			}
			return -1;
		}
	}
	skip_ws(&c);
	if (c.p != c.end) {
		return -1;
	}
	*nout = (uint8_t)n;
	return 0;
}

const mgls_jfield_t *mgj_get(const char *base, const mgls_jfield_t *f, size_t n, const char *key)
{
	for (size_t i = 0; i < n; i++) {
		if (strcmp(base + f[i].key, key) == 0) {
			return &f[i];
		}
	}
	return NULL;
}

const char *mgj_str(const char *base, const mgls_jfield_t *f, size_t n, const char *key)
{
	const mgls_jfield_t *fl = mgj_get(base, f, n, key);

	return fl != NULL && fl->type == MGJ_STRING ? base + fl->val : NULL;
}

bool mgj_int(const char *base, const mgls_jfield_t *fld, int32_t *out)
{
	const char *p, *e;
	bool neg = false;
	uint32_t v = 0;

	if (fld == NULL || fld->type != MGJ_NUMBER) {
		return false;
	}
	p = base + fld->val;
	e = p + fld->len;
	if (p < e && *p == '-') {
		neg = true;
		p++;
	}
	if (p >= e) {
		return false;
	}
	for (; p < e; p++) {
		if (!is_digit(*p) || v > 214748364u) {
			return false; /* a fraction, an exponent or too big */
		}
		v = v * 10 + (uint32_t)(*p - '0');
	}
	if (v > 2147483647u) {
		return false;
	}
	*out = neg ? -(int32_t)v : (int32_t)v;
	return true;
}

/* ---- JSON writer --------------------------------------------------------- */

void mgj_begin(mgj_w *w, char *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->err = cap == 0;
}

void mgj_raw(mgj_w *w, const char *s, size_t n)
{
	if (w->err || w->len + n + 1 > w->cap) {
		w->err = true;
		return;
	}
	memcpy(&w->buf[w->len], s, n);
	w->len += n;
}

void mgj_cstr(mgj_w *w, const char *s)
{
	mgj_raw(w, s, strlen(s));
}

void mgj_u32(mgj_w *w, uint32_t v)
{
	char num[10];
	size_t i = sizeof(num);

	do {
		num[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	mgj_raw(w, &num[i], sizeof(num) - i);
}

static void put_escaped(mgj_w *w, const char *s, size_t n)
{
	static const char hex[] = "0123456789abcdef";

	mgj_raw(w, "\"", 1);
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		char esc[6];

		if (ch == '"' || ch == '\\') {
			esc[0] = '\\';
			esc[1] = (char)ch;
			mgj_raw(w, esc, 2);
		} else if (ch < 0x20) {
			memcpy(esc, "\\u00", 4);
			esc[4] = hex[ch >> 4];
			esc[5] = hex[ch & 0xF];
			mgj_raw(w, esc, 6);
		} else {
			mgj_raw(w, (const char *)&ch, 1);
		}
	}
	mgj_raw(w, "\"", 1);
}

static void key(mgj_w *w, const char *k)
{
	mgj_raw(w, w->len == 0 ? "{" : ",", 1);
	put_escaped(w, k, strlen(k));
	mgj_raw(w, ":", 1);
}

void mgj_kstr(mgj_w *w, const char *k, const char *val)
{
	key(w, k);
	put_escaped(w, val != NULL ? val : "", val != NULL ? strlen(val) : 0);
}

void mgj_kstrn(mgj_w *w, const char *k, const char *val, size_t n)
{
	key(w, k);
	put_escaped(w, val, n);
}

void mgj_kraw(mgj_w *w, const char *k, const char *val)
{
	key(w, k);
	mgj_cstr(w, val);
}

void mgj_kint(mgj_w *w, const char *k, uint32_t val)
{
	key(w, k);
	mgj_u32(w, val);
}

int mgj_end(mgj_w *w)
{
	mgj_raw(w, w->len == 0 ? "{}" : "}", w->len == 0 ? 2 : 1);
	return mgj_finish(w);
}

int mgj_finish(mgj_w *w)
{
	if (w->err) {
		if (w->cap) {
			w->buf[0] = '\0';
		}
		return -1;
	}
	w->buf[w->len] = '\0';
	return (int)w->len;
}

/* ---- base64url ----------------------------------------------------------- */

static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int mgls_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
	size_t need = len / 3 * 4 + (len % 3 ? len % 3 + 1 : 0);
	size_t o = 0;

	if (need + 1 > cap) {
		return -1;
	}
	for (size_t i = 0; i < len; i += 3) {
		size_t rem = len - i;
		uint32_t v = (uint32_t)in[i] << 16;

		if (rem > 1) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (rem > 2) {
			v |= in[i + 2];
		}
		out[o++] = B64URL[(v >> 18) & 63];
		out[o++] = B64URL[(v >> 12) & 63];
		if (rem > 1) {
			out[o++] = B64URL[(v >> 6) & 63];
		}
		if (rem > 2) {
			out[o++] = B64URL[v & 63];
		}
	}
	out[o] = '\0';
	return (int)o;
}

static int b64val(char c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '-') {
		return 62;
	}
	if (c == '_') {
		return 63;
	}
	return -1;
}

int mgls_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
	size_t need = n / 4 * 3 + (n % 4 ? n % 4 - 1 : 0);
	size_t o = 0;
	uint32_t acc = 0;
	int bits = 0;

	if (n == 0 || n % 4 == 1 || need > cap) {
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		int v = b64val(in[i]);

		if (v < 0) {
			return -1;
		}
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (uint8_t)(acc >> bits);
		}
	}
	*out_len = o;
	return 0;
}

bool mgls_parse_u32(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	if (s == NULL || *s == '\0') {
		return false;
	}
	for (; *s; s++) {
		if (!is_digit(*s)) {
			return false;
		}
		uint32_t d = (uint32_t)(*s - '0');

		if (v > (UINT32_MAX - d) / 10) {
			return false;
		}
		v = v * 10 + d;
	}
	*out = v;
	return true;
}
