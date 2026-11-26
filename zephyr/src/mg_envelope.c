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

#include "mg_envelope.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

/* Field numbers, as ServiceCodec.cpp. */
#define REQ_ENV_SERVICE  1
#define REQ_ENV_PAYLOAD  2
#define RESP_ENV_PAYLOAD 1

#define FRAME_STREAM_ID  1
#define FRAME_REQUEST    2
#define FRAME_RESPONSE   3
#define FRAME_BODY_CHUNK 4
#define FRAME_RESET      5

#define HEADER_KEY   1
#define HEADER_VALUE 2

#define REQ_VERB     1
#define REQ_PATH     2
#define REQ_HEADERS  3
#define REQ_BODY     4
#define REQ_END_BODY 5

#define RESP_STATUS   1
#define RESP_HEADERS  2
#define RESP_BODY     3
#define RESP_END_BODY 4

#define CHUNK_DATA     1
#define CHUNK_END_BODY 2

#define RESET_CODE   1
#define RESET_REASON 2

#define WT_VARINT    0
#define WT_FIXED64   1
#define WT_DELIMITED 2
#define WT_FIXED32   5

struct mg_env_bytes mg_env_str(const char *s)
{
	return (struct mg_env_bytes){.p = (const uint8_t *)s, .n = s ? strlen(s) : 0};
}

/* ------------------------------------------------------------------------
 * Sizes
 * ---------------------------------------------------------------------- */

static size_t varint_size(uint64_t v)
{
	size_t n = 1;

	while (v >= 0x80) {
		v >>= 7;
		n++;
	}
	return n;
}

static uint64_t key_of(uint32_t field, uint8_t wt)
{
	return ((uint64_t)field << 3) | wt;
}

static size_t delimited_size(uint32_t field, size_t len)
{
	return varint_size(key_of(field, WT_DELIMITED)) + varint_size(len) + len;
}

static size_t varint_field_size(uint32_t field, uint64_t v)
{
	return varint_size(key_of(field, WT_VARINT)) + varint_size(v);
}

static size_t header_size(const struct mg_env_header *h)
{
	return (h->key.n ? delimited_size(HEADER_KEY, h->key.n) : 0) +
	       (h->value.n ? delimited_size(HEADER_VALUE, h->value.n) : 0);
}

static size_t headers_size(uint32_t field, const struct mg_env_frame *f)
{
	size_t n = 0;

	for (size_t i = 0; i < f->n_headers; i++) {
		n += delimited_size(field, header_size(&f->headers[i]));
	}
	return n;
}

/* The size of the frame's kind message (its payload). */
static size_t kind_size(const struct mg_env_frame *f)
{
	switch (f->kind) {
	case MG_ENV_REQUEST:
		return (f->verb.n ? delimited_size(REQ_VERB, f->verb.n) : 0) +
		       (f->path.n ? delimited_size(REQ_PATH, f->path.n) : 0) +
		       headers_size(REQ_HEADERS, f) +
		       (f->body.n ? delimited_size(REQ_BODY, f->body.n) : 0) +
		       (f->end_body ? varint_field_size(REQ_END_BODY, 1) : 0);
	case MG_ENV_RESPONSE:
		return (f->status ? varint_field_size(RESP_STATUS, (uint64_t)(int64_t)f->status)
				  : 0) +
		       headers_size(RESP_HEADERS, f) +
		       (f->body.n ? delimited_size(RESP_BODY, f->body.n) : 0) +
		       (f->end_body ? varint_field_size(RESP_END_BODY, 1) : 0);
	case MG_ENV_BODY_CHUNK:
		return (f->body.n ? delimited_size(CHUNK_DATA, f->body.n) : 0) +
		       (f->end_body ? varint_field_size(CHUNK_END_BODY, 1) : 0);
	case MG_ENV_RESET:
		return (f->reset_code ? varint_field_size(RESET_CODE,
							  (uint64_t)(int64_t)f->reset_code)
				      : 0) +
		       (f->reset_reason.n ? delimited_size(RESET_REASON, f->reset_reason.n) : 0);
	default:
		return 0;
	}
}

static uint32_t kind_field(enum mg_env_kind k)
{
	switch (k) {
	case MG_ENV_REQUEST:
		return FRAME_REQUEST;
	case MG_ENV_RESPONSE:
		return FRAME_RESPONSE;
	case MG_ENV_BODY_CHUNK:
		return FRAME_BODY_CHUNK;
	default:
		return FRAME_RESET;
	}
}

static size_t frame_size(const struct mg_env_frame *f)
{
	return varint_field_size(FRAME_STREAM_ID, (uint64_t)f->stream_id) +
	       delimited_size(kind_field(f->kind), kind_size(f));
}

/* ValidateFrameForEncode. */
static bool frame_valid(const struct mg_env_frame *f)
{
	if (f->stream_id == 0) {
		return false;
	}
	switch (f->kind) {
	case MG_ENV_REQUEST:
		return f->path.n > 0 && f->path.p[0] == '/';
	case MG_ENV_RESPONSE:
	case MG_ENV_BODY_CHUNK:
	case MG_ENV_RESET:
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------------------
 * Writing
 * ---------------------------------------------------------------------- */

struct wr {
	uint8_t *p;
	size_t cap;
	size_t len;
};

static void put_byte(struct wr *w, uint8_t b)
{
	if (w->len < w->cap) {
		w->p[w->len] = b;
	}
	w->len++;
}

static void put_varint(struct wr *w, uint64_t v)
{
	do {
		uint8_t b = v & 0x7f;

		v >>= 7;
		put_byte(w, b | (v ? 0x80 : 0));
	} while (v);
}

static void put_bytes(struct wr *w, const uint8_t *b, size_t n)
{
	if (n && w->len + n <= w->cap) {
		memcpy(&w->p[w->len], b, n);
	}
	w->len += n;
}

static void put_varint_field(struct wr *w, uint32_t field, uint64_t v)
{
	put_varint(w, key_of(field, WT_VARINT));
	put_varint(w, v);
}

static void put_prefix(struct wr *w, uint32_t field, size_t len)
{
	put_varint(w, key_of(field, WT_DELIMITED));
	put_varint(w, len);
}

/* A string or bytes field: left out when empty. */
static void put_field(struct wr *w, uint32_t field, struct mg_env_bytes b)
{
	if (b.n) {
		put_prefix(w, field, b.n);
		put_bytes(w, b.p, b.n);
	}
}

static void put_headers(struct wr *w, uint32_t field, const struct mg_env_frame *f)
{
	for (size_t i = 0; i < f->n_headers; i++) {
		const struct mg_env_header *h = &f->headers[i];

		/* Always written, even empty. */
		put_prefix(w, field, header_size(h));
		put_field(w, HEADER_KEY, h->key);
		put_field(w, HEADER_VALUE, h->value);
	}
}

static void put_frame(struct wr *w, const struct mg_env_frame *f)
{
	put_varint_field(w, FRAME_STREAM_ID, (uint64_t)f->stream_id);
	put_prefix(w, kind_field(f->kind), kind_size(f));
	switch (f->kind) {
	case MG_ENV_REQUEST:
		put_field(w, REQ_VERB, f->verb);
		put_field(w, REQ_PATH, f->path);
		put_headers(w, REQ_HEADERS, f);
		put_field(w, REQ_BODY, f->body);
		if (f->end_body) {
			put_varint_field(w, REQ_END_BODY, 1);
		}
		break;
	case MG_ENV_RESPONSE:
		if (f->status) {
			put_varint_field(w, RESP_STATUS, (uint64_t)(int64_t)f->status);
		}
		put_headers(w, RESP_HEADERS, f);
		put_field(w, RESP_BODY, f->body);
		if (f->end_body) {
			put_varint_field(w, RESP_END_BODY, 1);
		}
		break;
	case MG_ENV_BODY_CHUNK:
		put_field(w, CHUNK_DATA, f->body);
		if (f->end_body) {
			put_varint_field(w, CHUNK_END_BODY, 1);
		}
		break;
	case MG_ENV_RESET:
		if (f->reset_code) {
			put_varint_field(w, RESET_CODE, (uint64_t)(int64_t)f->reset_code);
		}
		put_field(w, RESET_REASON, f->reset_reason);
		break;
	default:
		break;
	}
}

static int finish(const struct wr *w)
{
	return w->len <= w->cap ? (int)w->len : -ENOSPC;
}

int mg_env_encode_frame(const struct mg_env_frame *f, uint8_t *out, size_t cap)
{
	struct wr w = {.p = out, .cap = cap};

	if (!frame_valid(f)) {
		return -EINVAL;
	}
	if (frame_size(f) > cap) {
		return -ENOSPC;
	}
	put_frame(&w, f);
	return finish(&w);
}

/* The services ServiceCodec takes: 0-3 and the Gadget API (6). */
static bool service_known(uint64_t service)
{
	return service <= MG_ENV_AUTHD || service == MG_ENV_GADGET;
}

static void put_request_envelope_prefix(struct wr *w, uint8_t service, size_t payload_len)
{
	if (service != MG_ENV_DAEMON) {
		put_varint_field(w, REQ_ENV_SERVICE, service);
	}
	if (payload_len) {
		put_prefix(w, REQ_ENV_PAYLOAD, payload_len);
	}
}

static size_t request_envelope_size(uint8_t service, size_t payload_len)
{
	return (service != MG_ENV_DAEMON ? varint_field_size(REQ_ENV_SERVICE, service) : 0) +
	       (payload_len ? delimited_size(REQ_ENV_PAYLOAD, payload_len) : 0);
}

int mg_env_encode_request_envelope(uint8_t service, const uint8_t *payload, size_t len,
				   uint8_t *out, size_t cap)
{
	struct wr w = {.p = out, .cap = cap};

	if (!service_known(service)) {
		return -EINVAL;
	}
	if (request_envelope_size(service, len) > cap) {
		return -ENOSPC;
	}
	put_request_envelope_prefix(&w, service, len);
	put_bytes(&w, payload, len);
	return finish(&w);
}

int mg_env_encode_response_envelope(const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
	struct wr w = {.p = out, .cap = cap};

	if ((len ? delimited_size(RESP_ENV_PAYLOAD, len) : 0) > cap) {
		return -ENOSPC;
	}
	if (len) {
		put_prefix(&w, RESP_ENV_PAYLOAD, len);
		put_bytes(&w, payload, len);
	}
	return finish(&w);
}

int mg_env_encode_request_message(uint8_t service, const struct mg_env_frame *f, uint8_t *out,
				  size_t cap)
{
	struct wr w = {.p = out, .cap = cap};
	size_t n;

	if (!service_known(service) || !frame_valid(f)) {
		return -EINVAL;
	}
	n = frame_size(f);
	if (request_envelope_size(service, n) > cap) {
		return -ENOSPC;
	}
	put_request_envelope_prefix(&w, service, n);
	put_frame(&w, f);
	return finish(&w);
}

int mg_env_encode_response_message(const struct mg_env_frame *f, uint8_t *out, size_t cap)
{
	struct wr w = {.p = out, .cap = cap};
	size_t n;

	if (!frame_valid(f)) {
		return -EINVAL;
	}
	n = frame_size(f);
	if (delimited_size(RESP_ENV_PAYLOAD, n) > cap) {
		return -ENOSPC;
	}
	put_prefix(&w, RESP_ENV_PAYLOAD, n);
	put_frame(&w, f);
	return finish(&w);
}

int mg_env_body_chunk_request_prefix(uint8_t service, int64_t stream_id, size_t data_len,
				     uint8_t *out, size_t cap)
{
	struct wr w = {.p = out, .cap = cap};
	size_t chunk, frame, prefix;

	if (!service_known(service) || stream_id == 0 || data_len == 0) {
		return -EINVAL;
	}
	chunk = delimited_size(CHUNK_DATA, data_len);
	frame = varint_field_size(FRAME_STREAM_ID, (uint64_t)stream_id) +
		delimited_size(FRAME_BODY_CHUNK, chunk);
	prefix = request_envelope_size(service, frame) - data_len;
	if (prefix > cap) {
		return -ENOSPC;
	}
	put_request_envelope_prefix(&w, service, frame);
	put_varint_field(&w, FRAME_STREAM_ID, (uint64_t)stream_id);
	put_prefix(&w, FRAME_BODY_CHUNK, chunk);
	put_prefix(&w, CHUNK_DATA, data_len);
	return finish(&w);
}

/* ------------------------------------------------------------------------
 * Reading
 * ---------------------------------------------------------------------- */

struct rd {
	const uint8_t *p;
	size_t len;
	size_t off;
};

static int get_varint(struct rd *r, uint64_t *out)
{
	uint64_t v = 0;

	for (int i = 0; i < 10; i++) {
		if (r->off >= r->len) {
			return -EBADMSG;
		}
		uint8_t b = r->p[r->off++];

		if (i == 9 && (b & 0xfe) != 0) {
			return -EBADMSG;
		}
		v |= (uint64_t)(b & 0x7f) << (i * 7);
		if (!(b & 0x80)) {
			*out = v;
			return 0;
		}
	}
	return -EBADMSG;
}

static int get_delimited(struct rd *r, struct mg_env_bytes *out)
{
	uint64_t n;

	if (get_varint(r, &n) || n > r->len - r->off) {
		return -EBADMSG;
	}
	out->p = &r->p[r->off];
	out->n = (size_t)n;
	r->off += (size_t)n;
	return 0;
}

static int get_key(struct rd *r, uint32_t *field, uint8_t *wt)
{
	uint64_t k, num;

	if (get_varint(r, &k)) {
		return -EBADMSG;
	}
	*wt = k & 7;
	num = k >> 3;
	if (num == 0 || num > (1u << 29) - 1 || (num >= 19000 && num <= 19999) ||
	    !(*wt == WT_VARINT || *wt == WT_FIXED64 || *wt == WT_DELIMITED || *wt == WT_FIXED32)) {
		return -EBADMSG;
	}
	*field = (uint32_t)num;
	return 0;
}

static int skip(struct rd *r, uint8_t wt)
{
	uint64_t v;
	struct mg_env_bytes b;

	switch (wt) {
	case WT_VARINT:
		return get_varint(r, &v);
	case WT_FIXED64:
	case WT_FIXED32: {
		size_t n = wt == WT_FIXED64 ? 8 : 4;

		if (r->len - r->off < n) {
			return -EBADMSG;
		}
		r->off += n;
		return 0;
	}
	default:
		return get_delimited(r, &b);
	}
}

/* Reads one known field of the wire type it must have. */
static int get_bytes_field(struct rd *r, uint8_t wt, struct mg_env_bytes *out)
{
	return wt == WT_DELIMITED ? get_delimited(r, out) : -EBADMSG;
}

static int get_varint_field(struct rd *r, uint8_t wt, uint64_t *out)
{
	return wt == WT_VARINT ? get_varint(r, out) : -EBADMSG;
}

static int decode_header(struct mg_env_bytes b, struct mg_env_header *h)
{
	struct rd r = {.p = b.p, .len = b.n};
	struct mg_env_header d = {0};

	while (r.off < r.len) {
		uint32_t field;
		uint8_t wt;
		int err = get_key(&r, &field, &wt);

		if (!err) {
			if (field == HEADER_KEY) {
				err = get_bytes_field(&r, wt, &d.key);
			} else if (field == HEADER_VALUE) {
				err = get_bytes_field(&r, wt, &d.value);
			} else {
				err = skip(&r, wt);
			}
		}
		if (err) {
			return err;
		}
	}
	*h = d;
	return 0;
}

/* Request or response: the fields after the kind's own. */
static int decode_kind(struct mg_env_bytes b, struct mg_env_frame *f,
		       struct mg_env_header *storage, size_t cap)
{
	struct rd r = {.p = b.p, .len = b.n};
	bool req = f->kind == MG_ENV_REQUEST;
	uint32_t f_headers = req ? REQ_HEADERS : RESP_HEADERS;
	uint32_t f_body = req ? REQ_BODY : RESP_BODY;
	uint32_t f_end = req ? REQ_END_BODY : RESP_END_BODY;
	size_t n = 0;
	bool too_many = false;

	while (r.off < r.len) {
		uint32_t field;
		uint8_t wt;
		uint64_t v = 0;
		struct mg_env_bytes hb;
		struct mg_env_header h;
		int err = get_key(&r, &field, &wt);

		if (err) {
			return err;
		}
		if (req && field == REQ_VERB) {
			err = get_bytes_field(&r, wt, &f->verb);
		} else if (req && field == REQ_PATH) {
			err = get_bytes_field(&r, wt, &f->path);
		} else if (!req && field == RESP_STATUS) {
			err = get_varint_field(&r, wt, &v);
			f->status = (int32_t)(uint32_t)v;
		} else if (field == f_headers) {
			err = get_bytes_field(&r, wt, &hb);
			err = err ?: decode_header(hb, &h);
			if (!err) {
				if (storage && n < cap) {
					storage[n] = h;
				} else if (storage) {
					too_many = true;
				}
				n++;
			}
		} else if (field == f_body) {
			err = get_bytes_field(&r, wt, &f->body);
		} else if (field == f_end) {
			err = get_varint_field(&r, wt, &v);
			f->end_body = v != 0;
		} else {
			err = skip(&r, wt);
		}
		if (err) {
			return err;
		}
	}
	if (too_many) {
		return -ENOSPC;
	}
	f->headers = storage;
	f->n_headers = n;
	return 0;
}

static int decode_chunk(struct mg_env_bytes b, struct mg_env_frame *f)
{
	struct rd r = {.p = b.p, .len = b.n};

	while (r.off < r.len) {
		uint32_t field;
		uint8_t wt;
		uint64_t v = 0;
		int err = get_key(&r, &field, &wt);

		if (!err) {
			if (field == CHUNK_DATA) {
				err = get_bytes_field(&r, wt, &f->body);
			} else if (field == CHUNK_END_BODY) {
				err = get_varint_field(&r, wt, &v);
				f->end_body = v != 0;
			} else {
				err = skip(&r, wt);
			}
		}
		if (err) {
			return err;
		}
	}
	return 0;
}

static int decode_reset(struct mg_env_bytes b, struct mg_env_frame *f)
{
	struct rd r = {.p = b.p, .len = b.n};

	while (r.off < r.len) {
		uint32_t field;
		uint8_t wt;
		uint64_t v = 0;
		int err = get_key(&r, &field, &wt);

		if (!err) {
			if (field == RESET_CODE) {
				err = get_varint_field(&r, wt, &v);
				f->reset_code = (int32_t)(uint32_t)v;
			} else if (field == RESET_REASON) {
				err = get_bytes_field(&r, wt, &f->reset_reason);
			} else {
				err = skip(&r, wt);
			}
		}
		if (err) {
			return err;
		}
	}
	return 0;
}

int mg_env_decode_frame(const uint8_t *buf, size_t len, struct mg_env_header *storage,
			size_t cap, struct mg_env_frame *out)
{
	struct rd r = {.p = buf, .len = len};
	struct mg_env_frame f = {0};
	struct mg_env_bytes payload = {0};
	int err = 0;

	while (r.off < r.len) {
		uint32_t field;
		uint8_t wt;
		uint64_t v = 0;

		err = get_key(&r, &field, &wt);
		if (err) {
			return err;
		}
		switch (field) {
		case FRAME_STREAM_ID:
			err = get_varint_field(&r, wt, &v);
			f.stream_id = (int64_t)v;
			break;
		case FRAME_REQUEST:
		case FRAME_RESPONSE:
		case FRAME_BODY_CHUNK:
		case FRAME_RESET:
			/* The last kind wins. */
			err = get_bytes_field(&r, wt, &payload);
			f.kind = field == FRAME_REQUEST    ? MG_ENV_REQUEST
				 : field == FRAME_RESPONSE ? MG_ENV_RESPONSE
				 : field == FRAME_BODY_CHUNK ? MG_ENV_BODY_CHUNK
							     : MG_ENV_RESET;
			break;
		default:
			err = skip(&r, wt);
			break;
		}
		if (err) {
			return err;
		}
	}
	switch (f.kind) {
	case MG_ENV_REQUEST:
	case MG_ENV_RESPONSE:
		err = decode_kind(payload, &f, storage, cap);
		break;
	case MG_ENV_BODY_CHUNK:
		err = decode_chunk(payload, &f);
		break;
	case MG_ENV_RESET:
		err = decode_reset(payload, &f);
		break;
	default:
		return -EINVAL;
	}
	if (err) {
		return err;
	}
	*out = f;
	return 0;
}

int mg_env_decode_request_envelope(const uint8_t *buf, size_t len, uint8_t *service,
				   struct mg_env_bytes *payload)
{
	struct rd r = {.p = buf, .len = len};
	struct mg_env_bytes p = {0};
	uint64_t svc = MG_ENV_DAEMON;

	while (r.off < r.len) {
		uint32_t field;
		uint8_t wt;
		int err = get_key(&r, &field, &wt);

		if (!err) {
			if (field == REQ_ENV_SERVICE) {
				err = get_varint_field(&r, wt, &svc);
			} else if (field == REQ_ENV_PAYLOAD) {
				err = get_bytes_field(&r, wt, &p);
			} else {
				err = skip(&r, wt);
			}
		}
		if (err) {
			return err;
		}
	}
	if (!service_known(svc)) {
		return -EINVAL;
	}
	*service = (uint8_t)svc;
	*payload = p;
	return 0;
}

int mg_env_decode_response_envelope(const uint8_t *buf, size_t len, struct mg_env_bytes *payload)
{
	struct rd r = {.p = buf, .len = len};
	struct mg_env_bytes p = {0};

	while (r.off < r.len) {
		uint32_t field;
		uint8_t wt;
		int err = get_key(&r, &field, &wt);

		if (!err) {
			err = field == RESP_ENV_PAYLOAD ? get_bytes_field(&r, wt, &p) : skip(&r, wt);
		}
		if (err) {
			return err;
		}
	}
	*payload = p;
	return 0;
}
