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

/*
 * mg_envelope.c's decoder: frames and envelopes worked out by hand from
 * ServiceCodec.cpp's rules, and its refusals of bad input. (The device
 * session vectors check the codec in place, and test_envelope_xcheck against
 * ServiceCodec.cpp itself.)
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/sys/util.h>

#include "check.h"
#include "mg_envelope.h"

static void test_decode(void)
{
	struct mg_env_header hs[2];
	struct mg_env_frame f;
	struct mg_env_bytes p;
	uint8_t svc;

	printf("  decoding\n");
	/* A request frame. */
	static const uint8_t req[] = {0x08, 0x01, 0x12, 0x13, 0x12, 0x01, '/',  0x1a, 0x06, 0x0a,
				      0x01, 'k',  0x12, 0x01, 'v',  0x1a, 0x00, 0x22, 0x02, 'x',
				      'y',  0x28, 0x01};

	CHECK(mg_env_decode_frame(req, sizeof(req), hs, 2, &f) == 0);
	CHECK(f.stream_id == 1 && f.kind == MG_ENV_REQUEST && f.verb.n == 0 && f.path.n == 1 &&
	      f.n_headers == 2 && f.headers == hs && hs[0].key.n == 1 && hs[0].key.p[0] == 'k' &&
	      hs[1].key.n == 0 && f.body.n == 2 && f.end_body);
	/* Header storage: too small is -ENOSPC; NULL counts them. */
	CHECK(mg_env_decode_frame(req, sizeof(req), hs, 1, &f) == -ENOSPC);
	CHECK(mg_env_decode_frame(req, sizeof(req), NULL, 0, &f) == 0 && f.n_headers == 2);
	/* Unknown fields of every wire type are skipped; the last kind wins. */
	static const uint8_t odd[] = {
		0x30, 0x05,                                     /* field 6 varint */
		0x39, 1,    2,    3,    4,    5, 6, 7, 8,       /* field 7 fixed64 */
		0x45, 1,    2,    3,    4,                      /* field 8 fixed32 */
		0x4a, 0x01, 0xff,                               /* field 9 bytes */
		0x12, 0x00,                                     /* a request... */
		0x22, 0x05, 0x0a, 0x01, 'z',  0x10, 0x01,       /* ...then a body chunk */
		0x08, 0x07,                                     /* stream 7, last */
	};

	CHECK(mg_env_decode_frame(odd, sizeof(odd), NULL, 0, &f) == 0);
	CHECK(f.stream_id == 7 && f.kind == MG_ENV_BODY_CHUNK && f.body.n == 1 &&
	      f.body.p[0] == 'z' && f.end_body);
	/* Refusals. */
	static const struct {
		uint8_t b[12];
		uint8_t n;
		int err;
	} bad[] = {
		{{0x08}, 1, -EBADMSG},                         /* truncated varint */
		{{0x12, 0x05, 0x01}, 3, -EBADMSG},             /* length past the end */
		{{0x00, 0x01}, 2, -EBADMSG},                   /* field 0 */
		{{0x0b}, 1, -EBADMSG},                         /* wire type 3 */
		{{0x0e}, 1, -EBADMSG},                         /* wire type 6 */
		{{0xc0, 0xa3, 0x09, 0x00}, 4, -EBADMSG},       /* field 19000 (reserved) */
		{{0x0a, 0x00}, 2, -EBADMSG},                   /* stream id as bytes */
		{{0x10, 0x01}, 2, -EBADMSG},                   /* request as a varint */
		{{0x22, 0x02, 0x08, 0x01}, 4, -EBADMSG},       /* chunk data as a varint */
		{{0x08, 0x01}, 2, -EINVAL},                    /* no kind */
		{{0}, 0, -EINVAL},                             /* empty */
		{{0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02},
		 11,
		 -EBADMSG}, /* varint over 64 bits */
		{{0x39, 1, 2, 3}, 4, -EBADMSG},                /* short fixed64 */
	};
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		int err = mg_env_decode_frame(bad[i].b, bad[i].n, NULL, 0, &f);

		CHECK(err == bad[i].err);
		if (err != bad[i].err) {
			fprintf(stderr, "    case %u: %d\n", (unsigned int)i, err);
		}
	}
	/* Ten-byte varints: the largest is fine. */
	static const uint8_t big[] = {0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
				      0xff, 0xff, 0x01, 0x22, 0x00};

	CHECK(mg_env_decode_frame(big, sizeof(big), NULL, 0, &f) == 0 && f.stream_id == -1);
	/* A response's status is the low 32 bits (200 + 2^32 here). */
	static const uint8_t st[] = {0x08, 0x01, 0x1a, 0x06, 0x08, 0xc8, 0x81, 0x80, 0x80, 0x10};

	CHECK(mg_env_decode_frame(st, sizeof(st), NULL, 0, &f) == 0 && f.status == 200);

	/* Envelopes. */
	static const uint8_t renv[] = {0x08, 0x02, 0x12, 0x02, 0x22, 0x00, 0x18, 0x01};

	CHECK(mg_env_decode_request_envelope(renv, sizeof(renv), &svc, &p) == 0 && svc == 2 &&
	      p.n == 2 && p.p == &renv[4]);
	static const uint8_t renv_bad[] = {0x08, 0x04};

	CHECK(mg_env_decode_request_envelope(renv_bad, 2, &svc, &p) == -EINVAL);
	CHECK(mg_env_decode_request_envelope(NULL, 0, &svc, &p) == 0 && svc == 0 && p.n == 0);
	static const uint8_t resp[] = {0x10, 0x05, 0x0a, 0x01, 0x07};

	CHECK(mg_env_decode_response_envelope(resp, sizeof(resp), &p) == 0 && p.n == 1 &&
	      p.p[0] == 7);
	CHECK(mg_env_decode_response_envelope(resp, 4, &p) == -EBADMSG);
	static const uint8_t resp_wt[] = {0x08, 0x01};

	CHECK(mg_env_decode_response_envelope(resp_wt, 2, &p) == -EBADMSG);
}

HOST_MAIN(test_decode)
