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

#ifndef MG_ENVELOPE_H_
#define MG_ENVELOPE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The messages of a VM session (mgcommands.h, Device session): device ->
 * client ServiceRequestEnvelopes and client -> device
 * ServiceResponseEnvelopes, and the service frames they carry (request,
 * response, body chunk, reset). Byte for byte what
 * esp32/components/noise_core (ServiceCodec.cpp) writes and accepts: fields
 * in number order, zero values left out, a frame's kind always written (even
 * empty), unknown fields skipped, the last of a repeated field kept.
 *
 * Portable C, no allocation: decoded byte strings point into the input. Only
 * the codec: the phone runs TLS, the WebSocket and Noise.
 */

enum mg_env_service {
	MG_ENV_DAEMON = 0,
	MG_ENV_SENTINEL = 1,
	MG_ENV_VAULT = 2,
	MG_ENV_AUTHD = 3,
	MG_ENV_GADGET = 6,   /* the VM's Gadget API; 4 and 5 are refused, as ServiceCodec does */
};

enum mg_env_kind {
	MG_ENV_NONE,
	MG_ENV_REQUEST,
	MG_ENV_RESPONSE,
	MG_ENV_BODY_CHUNK,
	MG_ENV_RESET,
};

/* ResetCode. */
enum mg_env_reset_code {
	MG_ENV_RESET_UNSPECIFIED = 0,
	MG_ENV_RESET_CANCELLED = 1,
	MG_ENV_RESET_TIMEOUT = 2,
	MG_ENV_RESET_PROTOCOL_ERROR = 3,
	MG_ENV_RESET_REFUSED_STREAM = 4,
	MG_ENV_RESET_INTERNAL_ERROR = 5,
	MG_ENV_RESET_SERVICE_UNAVAILABLE = 6,
};

struct mg_env_bytes {
	const uint8_t *p;
	size_t n;
};

struct mg_env_header {
	struct mg_env_bytes key;
	struct mg_env_bytes value;
};

/*
 * One service frame. kind picks the members that count:
 *   request     verb, path, headers, body, end_body
 *   response    status, headers, body, end_body
 *   body chunk  body (the chunk's data), end_body
 *   reset       reset_code, reset_reason
 */
struct mg_env_frame {
	int64_t stream_id;
	enum mg_env_kind kind;
	struct mg_env_bytes verb;
	struct mg_env_bytes path;
	const struct mg_env_header *headers;
	size_t n_headers;
	struct mg_env_bytes body;
	bool end_body;
	int32_t status;
	int32_t reset_code;
	struct mg_env_bytes reset_reason;
};

/* A NUL-terminated string as bytes. */
struct mg_env_bytes mg_env_str(const char *s);

/*
 * Encoders return the length written, -EINVAL for a frame noise_core
 * refuses to write (stream id 0, a request without a path starting with
 * '/', kind none, an unknown service) or -ENOSPC if cap is too small.
 */
int mg_env_encode_frame(const struct mg_env_frame *f, uint8_t *out, size_t cap);
int mg_env_encode_request_envelope(uint8_t service, const uint8_t *payload, size_t len,
				   uint8_t *out, size_t cap);
int mg_env_encode_response_envelope(const uint8_t *payload, size_t len, uint8_t *out,
				    size_t cap);
/* A frame inside a ServiceRequestEnvelope, in one go. */
int mg_env_encode_request_message(uint8_t service, const struct mg_env_frame *f, uint8_t *out,
				  size_t cap);
/* A frame inside a ServiceResponseEnvelope, in one go (the VM's side: tests). */
int mg_env_encode_response_message(const struct mg_env_frame *f, uint8_t *out, size_t cap);
/*
 * The bytes before the data of a ServiceRequestEnvelope carrying one body
 * chunk frame with end_body false (EncodeBodyChunkRequestPrefix): that
 * message is exactly this prefix followed by the data. data_len > 0.
 */
int mg_env_body_chunk_request_prefix(uint8_t service, int64_t stream_id, size_t data_len,
				     uint8_t *out, size_t cap);

/*
 * Decoders return 0, -EBADMSG for malformed protobuf (a bad varint or key, a
 * known field of the wrong wire type, a length past the end), -EINVAL for an
 * unknown service or a frame with no kind.
 */
int mg_env_decode_request_envelope(const uint8_t *buf, size_t len, uint8_t *service,
				   struct mg_env_bytes *payload);
int mg_env_decode_response_envelope(const uint8_t *buf, size_t len,
				    struct mg_env_bytes *payload);
/*
 * Decodes a frame. Headers go into storage (cap entries; f->headers points
 * there and n_headers counts them); more than cap is -ENOSPC, as
 * DecodeServiceFrameDetailed. storage NULL takes any number without keeping
 * them (n_headers still counts them).
 */
int mg_env_decode_frame(const uint8_t *buf, size_t len, struct mg_env_header *storage,
			size_t cap, struct mg_env_frame *f);

#endif
