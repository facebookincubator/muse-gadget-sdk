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

// Runs noise_control.cpp's control_stream_ended(), which test_link_noise_session.py
// copies into control_stream_end.inc: only the end of Link's own control stream
// ends the session, not the end of the tunnel's, a request's or the identity
// stream's, and not a frame that leaves the control stream open.

#include <xplat/noise/core/ServiceCodec.h>

#include <cstdio>

using namespace musegadgets::noise::core;

#include "control_stream_end.inc"

static int failures = 0;

static void expect(bool got, bool want, const char *what) {
    if (got != want) {
        std::printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static DecodedServiceFrame body_chunk(int64_t stream, bool end) {
    DecodedServiceFrame f;
    f.stream_id = stream;
    f.kind = ServiceFrameKind::BodyChunk;
    f.body_chunk.end_body = end;
    return f;
}

static DecodedServiceFrame response(int64_t stream, int32_t status, bool end) {
    DecodedServiceFrame f;
    f.stream_id = stream;
    f.kind = ServiceFrameKind::Response;
    f.response.status = status;
    f.response.end_body = end;
    return f;
}

int main() {
    expect(control_stream_ended(body_chunk(CTRL_STREAM_ID, true)), true,
           "control BodyChunk with end_body");
    expect(control_stream_ended(body_chunk(CTRL_STREAM_ID, false)), false,
           "control BodyChunk without end_body");
    expect(control_stream_ended(response(CTRL_STREAM_ID, 200, true)), true,
           "control Response with end_body");
    expect(control_stream_ended(response(CTRL_STREAM_ID, 200, false)), false,
           "control Response without end_body");
    expect(control_stream_ended(body_chunk(2, true)), false, "tunnel stream end");
    expect(control_stream_ended(body_chunk(3, true)), false, "identity stream end");
    expect(control_stream_ended(body_chunk(16, true)), false, "request stream end");
    expect(control_stream_ended(response(16, 200, true)), false, "request Response end");
    DecodedServiceFrame reset;
    reset.stream_id = CTRL_STREAM_ID;
    reset.kind = ServiceFrameKind::Reset;
    expect(control_stream_ended(reset), false, "control Reset (handled on its own)");
    if (failures) return 1;
    std::printf("ok\n");
    return 0;
}
