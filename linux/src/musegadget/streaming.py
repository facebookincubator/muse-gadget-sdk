# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Bounded, duplex HTTP response streams over a LinkSession.

A slow consumer fails its own stream rather than blocking control or other
requests on the multiplexed connection. Callers must close streams (prefer
``async with``), including when they abandon a response before EOF.
"""

from __future__ import annotations

import asyncio
import json
from collections import deque

MAX_BUFFER_BYTES = 2 * 1024 * 1024
MAX_BUFFER_CHUNKS = 256
MAX_LINE_BYTES = 1024 * 1024
MAX_ERROR_BYTES = 64 * 1024
MAX_SEND_BYTES = 64 * 1024


class HTTPStatusError(ConnectionError):
    def __init__(self, status: int, body: bytes) -> None:
        # Do not include an untrusted response body in logs or CLI errors.
        super().__init__(f"request refused: HTTP {status}")
        self.status, self.body = status, body


class StreamingRequest:
    """One request with independent input and output lifetimes; one reader.

    ``send_body`` sends immediately; ``half_close`` ends only the upload.
    ``wait_response`` validates the HTTP status. Iteration yields bytes as they
    arrive, not a buffered response. All receive queues and send chunks have
    hard bounds. An overflow is an error, never silent audio loss.
    """

    def __init__(self, session, stream_id: int, *, max_buffer_bytes: int,
                 max_buffer_chunks: int) -> None:
        self.session, self.stream_id = session, stream_id
        self.max_buffer_bytes = max_buffer_bytes
        self.max_buffer_chunks = max_buffer_chunks
        self.status = None
        self.headers = []
        self._chunks = deque()
        self._bytes = 0
        self._changed = asyncio.Event()
        self._error = None
        self._ended = False
        self._closed = False
        self._input_closed = False  # EOF intent blocks new body producers immediately.
        self._input_eof_sent = False
        self._close_complete = False
        self._end_lock = asyncio.Lock()

    @property
    def ended(self) -> bool:
        return self._ended

    def fail(self, error: Exception) -> None:
        if self._error is None:
            self._error = error
        self._chunks.clear()
        self._bytes = 0
        self._changed.set()

    def on_frame(self, frame) -> None:
        if self._closed or self._error is not None:
            return
        if self._ended:
            self.fail(ValueError("response data after EOF"))
            return
        if frame.kind == "reset":
            self.fail(ConnectionError("request stream reset"))
            return
        if frame.kind == "response":
            if self.status is not None or not 100 <= frame.value.status <= 599:
                self.fail(ValueError("invalid or duplicate response head"))
                return
            self.status, self.headers = frame.value.status, frame.value.headers
            data = frame.value.body
        elif frame.kind == "body_chunk" and self.status is not None:
            data = frame.value.data
        else:
            self.fail(ValueError("response body before head"))
            return
        if data:
            if (self._bytes + len(data) > self.max_buffer_bytes
                    or len(self._chunks) >= self.max_buffer_chunks):
                self.fail(ValueError("response stream buffer full"))
                return
            self._chunks.append(data)
            self._bytes += len(data)
        self._ended = frame.value.end_body
        self._changed.set()

    async def wait_response(self) -> int:
        while self.status is None:
            self._check()
            self._changed.clear()
            await self._changed.wait()
        self._check()
        if not 200 <= self.status < 300:
            body = bytearray()
            while len(body) < MAX_ERROR_BYTES:
                chunk = await self.read()
                if chunk is None:
                    break
                body.extend(chunk[:MAX_ERROR_BYTES - len(body)])
            raise HTTPStatusError(self.status, bytes(body))
        return self.status

    def _check(self) -> None:
        if self._error is not None:
            raise self._error
        if self._closed:
            raise ConnectionError("request stream closed")

    async def read(self) -> bytes | None:
        """Next nonempty response chunk, or None at clean EOF."""
        while True:
            self._check()
            if self._chunks:
                chunk = self._chunks.popleft()
                self._bytes -= len(chunk)
                return chunk
            if self._ended:
                return None
            self._changed.clear()
            await self._changed.wait()

    def __aiter__(self):
        return self

    async def __anext__(self) -> bytes:
        chunk = await self.read()
        if chunk is None:
            raise StopAsyncIteration
        return chunk

    async def send_body(self, data: bytes, *, end_body: bool = False, check=None) -> None:
        """Send a chunk; optional check runs under the send lock before encryption."""
        self._check()
        if self._input_closed:
            raise ValueError("request body already closed")
        if len(data) > MAX_SEND_BYTES:
            raise ValueError("request chunk exceeds 64 KiB")
        if end_body:
            self._input_closed = True
            async with self._end_lock:
                await self._send_body(data, end_body=True, check=check)
        else:
            await self._send_body(data, check=check)

    def _check_send(self) -> None:
        self._check()
        if self._input_eof_sent:
            raise ValueError("request body already closed")

    async def _send_body(self, data: bytes, *, end_body: bool = False, check=None) -> None:
        def admissible():
            self._check_send()
            if check is not None:
                check()

        await self.session._send_encrypted(lambda: self.session._transport.encrypt_body_chunk(
            self.stream_id, data, end_body=end_body,
        ), check=admissible)
        if end_body:
            self._input_eof_sent = True

    async def half_close(self) -> None:
        self._input_closed = True
        async with self._end_lock:
            self._check()
            if not self._input_eof_sent:
                await self._send_body(b"", end_body=True)

    async def aclose(self) -> None:
        if self._close_complete:
            return
        self._closed = True
        self._chunks.clear()
        self._bytes = 0
        self._changed.set()
        # Cancellation before publication leaves close retryable. Keep the
        # request registered until EOS/reset completes or the session fails.
        async with self._end_lock:
            if self._close_complete:
                return
            if not (self._ended and self._input_eof_sent):
                await self.session._send_encrypted(lambda: self.session._transport.encrypt_reset(
                    self.stream_id, reason="client closed stream",
                ))
            self._close_complete = True
            self.session._requests.pop(self.stream_id, None)

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, traceback):
        try:
            await asyncio.wait_for(self.aclose(), 2)
        except Exception:
            if exc is None:
                raise


async def read_ndjson(stream, *, max_line_bytes: int = MAX_LINE_BYTES):
    """Yield objects from arbitrarily fragmented UTF-8 NDJSON, bounded per line.

    LF is required even on the last record; CRLF and blank lines are accepted.
    Malformed JSON/UTF-8, non-object records and a truncated final line fail
    explicitly. Extra fields and unknown event types are the caller's concern.
    """
    if max_line_bytes < 1:
        raise ValueError("line limit must be positive")
    pending = bytearray()
    async for chunk in stream:
        offset = 0
        while offset < len(chunk):
            newline = chunk.find(b"\n", offset)
            end = len(chunk) if newline < 0 else newline + 1
            if len(pending) + end - offset > max_line_bytes:
                raise ValueError("NDJSON line too large")
            pending.extend(chunk[offset:end])
            offset = end
            if newline >= 0:
                if pending.strip():
                    try:
                        record = json.loads(pending)
                    except (ValueError, UnicodeError, RecursionError) as exc:
                        raise ValueError("invalid NDJSON record") from exc
                    if not isinstance(record, dict):
                        raise ValueError("NDJSON record must be an object")
                    yield record
                pending.clear()
    if pending:
        raise ValueError("response ended within an NDJSON record")
