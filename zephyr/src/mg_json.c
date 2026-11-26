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

#include "mg_json.h"

#include <errno.h>
#include <string.h>

#define BIT32(n) ((uint32_t)1 << (n))

struct cur {
	char *p;
	char *end;
	/* Inside a nested value: check strings without decoding them, so the
	 * value's raw text stays as it was (mg_json_field.raw). */
	bool scan;
};

static void skip_ws(struct cur *c)
{
	while (c->p < c->end &&
	       (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
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
			return -EINVAL;
		}
	}
	*out = v;
	return 0;
}

/* At the opening quote. Decodes in place from just after it; *out points at
 * the decoded, NUL-terminated text. */
static int parse_string(struct cur *c, char **out, size_t *out_len)
{
	char *w;

#define EMIT(ch)                                                                                   \
	do {                                                                                       \
		if (!c->scan) {                                                                    \
			*w = (char)(ch);                                                           \
		}                                                                                  \
		w++;                                                                               \
	} while (0)

	if (c->p >= c->end || *c->p != '"') {
		return -EINVAL;
	}
	c->p++;
	*out = w = c->p;
	while (c->p < c->end) {
		unsigned char ch = (unsigned char)*c->p;

		if (ch == '"') {
			if (!c->scan) {
				*w = '\0'; /* w <= c->p: the closing quote's cell at the latest */
			}
			*out_len = (size_t)(w - *out);
			c->p++;
			return 0;
		}
		if (ch < 0x20) {
			return -EINVAL;
		}
		if (ch != '\\') {
			EMIT(ch);
			c->p++;
			continue;
		}
		if (c->end - c->p < 2) {
			return -EINVAL;
		}
		char e = c->p[1];

		c->p += 2;
		switch (e) {
		case '"':
		case '\\':
		case '/':
			EMIT(e);
			break;
		case 'b':
			EMIT('\b');
			break;
		case 'f':
			EMIT('\f');
			break;
		case 'n':
			EMIT('\n');
			break;
		case 'r':
			EMIT('\r');
			break;
		case 't':
			EMIT('\t');
			break;
		case 'u': {
			unsigned int u, lo;

			if (c->end - c->p < 4 || hex4(c->p, &u)) {
				return -EINVAL;
			}
			c->p += 4;
			if (u >= 0xDC00 && u <= 0xDFFF) {
				return -EINVAL; /* lone low surrogate */
			}
			if (u >= 0xD800 && u <= 0xDBFF) {
				if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u' ||
				    hex4(&c->p[2], &lo) || lo < 0xDC00 || lo > 0xDFFF) {
					return -EINVAL;
				}
				c->p += 6;
				u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
			}
			if (u == 0) {
				return -EINVAL;
			}
			/* At most 4 bytes out for 6 or 12 read: stays behind c->p. */
			if (u < 0x80) {
				EMIT(u);
			} else if (u < 0x800) {
				EMIT(0xC0 | (u >> 6));
				EMIT(0x80 | (u & 0x3F));
			} else if (u < 0x10000) {
				EMIT(0xE0 | (u >> 12));
				EMIT(0x80 | ((u >> 6) & 0x3F));
				EMIT(0x80 | (u & 0x3F));
			} else {
				EMIT(0xF0 | (u >> 18));
				EMIT(0x80 | ((u >> 12) & 0x3F));
				EMIT(0x80 | ((u >> 6) & 0x3F));
				EMIT(0x80 | (u & 0x3F));
			}
			break;
		}
		default:
			return -EINVAL;
		}
	}
	return -EINVAL;
#undef EMIT
}

static bool is_digit(char ch)
{
	return ch >= '0' && ch <= '9';
}

static int parse_number(struct cur *c, int64_t *num, bool *is_int)
{
	bool neg = false, frac = false, big = false;
	uint64_t v = 0;

	if (c->p < c->end && *c->p == '-') {
		neg = true;
		c->p++;
	}
	if (c->p >= c->end || !is_digit(*c->p)) {
		return -EINVAL;
	}
	if (*c->p == '0') {
		c->p++;
	} else {
		while (c->p < c->end && is_digit(*c->p)) {
			if (v > (UINT64_C(1) << 62) / 10) {
				big = true;
			} else {
				v = v * 10 + (uint64_t)(*c->p - '0');
			}
			c->p++;
		}
	}
	if (c->p < c->end && *c->p == '.') {
		frac = true;
		c->p++;
		if (c->p >= c->end || !is_digit(*c->p)) {
			return -EINVAL;
		}
		while (c->p < c->end && is_digit(*c->p)) {
			c->p++;
		}
	}
	if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
		frac = true;
		c->p++;
		if (c->p < c->end && (*c->p == '+' || *c->p == '-')) {
			c->p++;
		}
		if (c->p >= c->end || !is_digit(*c->p)) {
			return -EINVAL;
		}
		while (c->p < c->end && is_digit(*c->p)) {
			c->p++;
		}
	}
	*is_int = !frac && !big;
	*num = *is_int ? (neg ? -(int64_t)v : (int64_t)v) : 0;
	return 0;
}

static int literal(struct cur *c, const char *word)
{
	size_t n = strlen(word);

	if ((size_t)(c->end - c->p) < n || memcmp(c->p, word, n) != 0) {
		return -EINVAL;
	}
	c->p += n;
	return 0;
}

static int parse_value(struct cur *c, struct mg_json_field *f, int depth);

/* An object or array below the top level: checked, not kept. */
static int skip_container(struct cur *c, int depth)
{
	struct mg_json_field tmp;
	char close = *c->p == '{' ? '}' : ']';
	bool obj = close == '}';

	if (depth > MG_JSON_MAX_DEPTH) {
		return -E2BIG;
	}
	c->p++;
	skip_ws(c);
	if (c->p < c->end && *c->p == close) {
		c->p++;
		return 0;
	}
	for (;;) {
		int err;

		if (obj) {
			char *k;
			size_t kl;

			skip_ws(c);
			err = parse_string(c, &k, &kl);
			if (err) {
				return err;
			}
			skip_ws(c);
			if (c->p >= c->end || *c->p != ':') {
				return -EINVAL;
			}
			c->p++;
		}
		err = parse_value(c, &tmp, depth);
		if (err) {
			return err;
		}
		skip_ws(c);
		if (c->p < c->end && *c->p == ',') {
			c->p++;
			continue;
		}
		if (c->p < c->end && *c->p == close) {
			c->p++;
			return 0;
		}
		return -EINVAL;
	}
}

static int parse_value(struct cur *c, struct mg_json_field *f, int depth)
{
	char *s;

	skip_ws(c);
	if (c->p >= c->end) {
		return -EINVAL;
	}
	memset(&f->type, 0, sizeof(*f) - offsetof(struct mg_json_field, type));
	switch (*c->p) {
	case '"':
		f->type = MG_JSON_STRING;
		if (parse_string(c, &s, &f->str_len)) {
			return -EINVAL;
		}
		f->str = s;
		return 0;
	case '{':
	case '[': {
		bool was = c->scan;
		int err;

		f->type = MG_JSON_NESTED;
		f->is_array = *c->p == '[';
		f->raw = c->p;
		c->scan = true;
		err = skip_container(c, depth + 1);
		c->scan = was;
		f->raw_len = (size_t)(c->p - f->raw);
		return err;
	}
	case 't':
		f->type = MG_JSON_TRUE;
		return literal(c, "true");
	case 'f':
		f->type = MG_JSON_FALSE;
		return literal(c, "false");
	case 'n':
		f->type = MG_JSON_NULL;
		return literal(c, "null");
	default:
		f->type = MG_JSON_NUMBER;
		return parse_number(c, &f->num, &f->num_is_int);
	}
}

int mg_json_parse(char *buf, size_t len, struct mg_json_obj *out)
{
	struct cur c = {.p = buf, .end = buf + len};

	out->n = 0;
	skip_ws(&c);
	if (c.p >= c.end || *c.p != '{') {
		return -EINVAL;
	}
	c.p++;
	skip_ws(&c);
	if (c.p < c.end && *c.p == '}') {
		c.p++;
	} else {
		for (;;) {
			struct mg_json_field f;
			char *k;
			size_t kl;
			int err;

			skip_ws(&c);
			if (parse_string(&c, &k, &kl)) {
				return -EINVAL;
			}
			skip_ws(&c);
			if (c.p >= c.end || *c.p != ':') {
				return -EINVAL;
			}
			c.p++;
			err = parse_value(&c, &f, 1);
			if (err) {
				return err;
			}
			f.key = k;
			if (mg_json_get(out, k) != NULL) {
				return -EINVAL; /* duplicate member */
			}
			if (out->n >= MG_JSON_MAX_FIELDS) {
				return -E2BIG;
			}
			out->f[out->n++] = f;
			skip_ws(&c);
			if (c.p < c.end && *c.p == ',') {
				c.p++;
				continue;
			}
			if (c.p < c.end && *c.p == '}') {
				c.p++;
				break;
			}
			return -EINVAL;
		}
	}
	skip_ws(&c);
	return c.p == c.end ? 0 : -EINVAL;
}

const struct mg_json_field *mg_json_get(const struct mg_json_obj *o, const char *key)
{
	for (size_t i = 0; i < o->n; i++) {
		if (strcmp(o->f[i].key, key) == 0) {
			return &o->f[i];
		}
	}
	return NULL;
}

const char *mg_json_str(const struct mg_json_obj *o, const char *key)
{
	const struct mg_json_field *f = mg_json_get(o, key);

	return f && f->type == MG_JSON_STRING ? f->str : NULL;
}

/* ---- writer ---- */

static void put(struct mg_jw *w, const char *s, size_t n)
{
	if (w->err || w->len + n + 1 > w->cap) {
		w->err = true;
		return;
	}
	if (w->buf) {
		memcpy(&w->buf[w->len], s, n);
	}
	w->len += n;
}

static void put_escaped(struct mg_jw *w, const char *s)
{
	put(w, "\"", 1);
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		char esc[8];

		if (ch == '"' || ch == '\\') {
			esc[0] = '\\';
			esc[1] = (char)ch;
			put(w, esc, 2);
		} else if (ch < 0x20) {
			static const char hex[] = "0123456789abcdef";

			memcpy(esc, "\\u00", 4);
			esc[4] = hex[ch >> 4];
			esc[5] = hex[ch & 0xF];
			put(w, esc, 6);
		} else {
			put(w, (const char *)&ch, 1);
		}
	}
	put(w, "\"", 1);
}

/* A member's key (NULL inside an array), after a comma if the container
 * already has a member. */
static void key(struct mg_jw *w, const char *k)
{
	uint32_t bit;

	if (w->depth == 0) {
		w->err = true;
		return;
	}
	bit = BIT32(w->depth - 1);
	if (w->nonempty & bit) {
		put(w, ",", 1);
	}
	w->nonempty |= bit;
	if (k != NULL) {
		put_escaped(w, k);
		put(w, ":", 1);
	}
}

static void jw_open(struct mg_jw *w, bool array)
{
	if (w->depth >= 32) {
		w->err = true;
		return;
	}
	put(w, array ? "[" : "{", 1);
	w->nonempty &= ~BIT32(w->depth);
	if (array) {
		w->arrays |= BIT32(w->depth);
	} else {
		w->arrays &= ~BIT32(w->depth);
	}
	w->depth++;
}

void mg_jw_begin(struct mg_jw *w, char *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->err = cap == 0;
	w->depth = 0;
	w->nonempty = 0;
	w->arrays = 0;
	jw_open(w, false);
}

void mg_jw_str(struct mg_jw *w, const char *k, const char *val)
{
	key(w, k);
	put_escaped(w, val ? val : "");
}

void mg_jw_int(struct mg_jw *w, const char *k, long long val)
{
	char num[24];
	size_t i = sizeof(num);
	unsigned long long u = val < 0 ? 0ULL - (unsigned long long)val : (unsigned long long)val;

	do {
		num[--i] = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	if (val < 0) {
		num[--i] = '-';
	}
	key(w, k);
	put(w, &num[i], sizeof(num) - i);
}

void mg_jw_bool(struct mg_jw *w, const char *k, bool val)
{
	key(w, k);
	put(w, val ? "true" : "false", val ? 4 : 5);
}

void mg_jw_raw(struct mg_jw *w, const char *k, const char *json, size_t len)
{
	key(w, k);
	put(w, json, len);
}

void mg_jw_obj(struct mg_jw *w, const char *k)
{
	key(w, k);
	jw_open(w, false);
}

void mg_jw_arr(struct mg_jw *w, const char *k)
{
	key(w, k);
	jw_open(w, true);
}

void mg_jw_close(struct mg_jw *w)
{
	if (w->depth <= 1) {
		w->err = true; /* the outermost object closes in mg_jw_end */
		return;
	}
	w->depth--;
	put(w, (w->arrays & BIT32(w->depth)) ? "]" : "}", 1);
}

int mg_jw_end(struct mg_jw *w)
{
	while (w->depth > 1) {
		mg_jw_close(w);
	}
	if (w->depth == 1) {
		w->depth = 0;
		put(w, "}", 1);
	} else {
		w->err = true;
	}
	if (w->err) {
		if (w->buf && w->cap) {
			w->buf[0] = '\0';
		}
		return -ENOSPC;
	}
	if (w->buf) {
		w->buf[w->len] = '\0';
	}
	return (int)w->len;
}
