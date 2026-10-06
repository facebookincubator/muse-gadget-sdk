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

from __future__ import annotations

import secrets
import time
from dataclasses import dataclass
from typing import Dict, Optional

from ._proto import (
    ProtoError,
    WIRE_DELIMITED,
    WIRE_VARINT,
    bytes_field,
    decode_int64,
    decode_uint32,
    int64_field,
    read_delimited,
    read_key,
    read_varint,
    skip_field,
    uint32_field,
)


MAX_CHUNK_PAYLOAD = 65489
MAX_PENDING_ASSEMBLIES = 16
MAX_TOTAL_CHUNKS = 256
MAX_ASSEMBLY_BYTES = 16 * 1024 * 1024
ASSEMBLY_TTL_SECONDS = 60


@dataclass(frozen=True)
class NoiseTransportFrame:
    chunk_id: int = 0
    chunk_index: int = 0
    total_chunks: int = 1
    payload: bytes = b""


@dataclass
class _Assembly:
    chunks: Dict[int, bytes]
    total: int
    total_bytes: int
    created_at: float
    last_updated: float


def _random_int64() -> int:
    value = secrets.randbits(64)
    if value >= 1 << 63:
        value -= 1 << 64
    return value


def encode_noise_frame(frame: NoiseTransportFrame) -> bytes:
    out = bytearray()
    if frame.chunk_id != 0:
        out += int64_field(1, frame.chunk_id)
    if frame.chunk_index != 0:
        out += uint32_field(2, frame.chunk_index)
    if frame.total_chunks != 0:
        out += uint32_field(3, frame.total_chunks)
    payload = bytes(frame.payload)
    if payload:
        out += bytes_field(4, payload)
    return bytes(out)


def decode_noise_frame(data: bytes) -> NoiseTransportFrame:
    chunk_id = 0
    chunk_index = 0
    total_chunks = 1
    payload = b""
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_VARINT:
                raise ProtoError("NoiseTransportFrame.chunk_id wrong wire type")
            raw, offset = read_varint(data, offset)
            chunk_id = decode_int64(raw)
        elif field_number == 2:
            if wire_type != WIRE_VARINT:
                raise ProtoError("NoiseTransportFrame.chunk_index wrong wire type")
            raw, offset = read_varint(data, offset)
            chunk_index = decode_uint32(raw)
        elif field_number == 3:
            if wire_type != WIRE_VARINT:
                raise ProtoError("NoiseTransportFrame.total_chunks wrong wire type")
            raw, offset = read_varint(data, offset)
            total_chunks = decode_uint32(raw)
        elif field_number == 4:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("NoiseTransportFrame.payload wrong wire type")
            payload, offset = read_delimited(data, offset)
        else:
            offset = skip_field(data, offset, wire_type)
    return NoiseTransportFrame(
        chunk_id=chunk_id,
        chunk_index=chunk_index,
        total_chunks=total_chunks,
        payload=payload,
    )


def encode_noise_frames(data: bytes, chunk_id: Optional[int] = None) -> list:
    payload = bytes(data)
    selected_chunk_id = _random_int64() if chunk_id is None else chunk_id
    total_chunks = max(1, (len(payload) + MAX_CHUNK_PAYLOAD - 1) // MAX_CHUNK_PAYLOAD)
    if total_chunks > MAX_TOTAL_CHUNKS:
        raise ValueError(
            "payload too large for noise framing "
            f"({len(payload)} bytes, {total_chunks} chunks > {MAX_TOTAL_CHUNKS})"
        )

    if not payload:
        return [
            encode_noise_frame(
                NoiseTransportFrame(
                    chunk_id=selected_chunk_id,
                    chunk_index=0,
                    total_chunks=1,
                    payload=b"",
                )
            )
        ]

    frames = []
    for index in range(total_chunks):
        start = index * MAX_CHUNK_PAYLOAD
        chunk = payload[start : start + MAX_CHUNK_PAYLOAD]
        frames.append(
            encode_noise_frame(
                NoiseTransportFrame(
                    chunk_id=selected_chunk_id,
                    chunk_index=index,
                    total_chunks=total_chunks,
                    payload=chunk,
                )
            )
        )
    return frames


class NoiseFrameDecoder:
    def __init__(self) -> None:
        self._pending: Dict[int, _Assembly] = {}
        self._poisoned = False

    def decode(self, frame_bytes: bytes) -> Optional[bytes]:
        if self._poisoned:
            raise RuntimeError("NoiseFrameDecoder: poisoned after prior failure")

        try:
            frame = decode_noise_frame(bytes(frame_bytes))
            if frame.chunk_id < -(1 << 63) or frame.chunk_id > (1 << 63) - 1:
                raise ValueError(f"chunkId outside int64 range: {frame.chunk_id}")
            if frame.total_chunks < 1 or frame.total_chunks > MAX_TOTAL_CHUNKS:
                raise ValueError(f"invalid totalChunks: {frame.total_chunks}")
            if frame.chunk_index < 0 or frame.chunk_index >= frame.total_chunks:
                raise ValueError(
                    f"chunkIndex {frame.chunk_index} out of range [0, {frame.total_chunks})"
                )
            if len(frame.payload) > MAX_CHUNK_PAYLOAD:
                raise ValueError(
                    f"payload too large for noise frame ({len(frame.payload)} bytes)"
                )

            self._evict_expired()

            assembly = self._pending.get(frame.chunk_id)
            if assembly is None:
                if len(self._pending) >= MAX_PENDING_ASSEMBLIES:
                    raise ValueError("too many pending noise frame assemblies")
                now = time.time()
                assembly = _Assembly(
                    chunks={},
                    total=frame.total_chunks,
                    total_bytes=0,
                    created_at=now,
                    last_updated=now,
                )
                self._pending[frame.chunk_id] = assembly

            if assembly.total != frame.total_chunks:
                self._pending.pop(frame.chunk_id, None)
                raise ValueError(
                    "inconsistent totalChunks for chunkId: "
                    f"expected {assembly.total}, got {frame.total_chunks}"
                )

            if frame.chunk_index in assembly.chunks:
                self._pending.pop(frame.chunk_id, None)
                raise ValueError(f"duplicate chunkIndex {frame.chunk_index}")

            assembly.last_updated = time.time()
            assembly.total_bytes += len(frame.payload)
            if assembly.total_bytes > MAX_ASSEMBLY_BYTES:
                self._pending.pop(frame.chunk_id, None)
                raise ValueError("assembly exceeded byte budget")

            assembly.chunks[frame.chunk_index] = frame.payload
            if len(assembly.chunks) < assembly.total:
                return None

            self._pending.pop(frame.chunk_id, None)
            if assembly.total == 1:
                return assembly.chunks[0]
            return b"".join(assembly.chunks[i] for i in range(assembly.total))
        except Exception:
            self._poisoned = True
            raise

    def _evict_expired(self) -> None:
        now = time.time()
        expired = [
            chunk_id
            for chunk_id, assembly in self._pending.items()
            if now - assembly.created_at > ASSEMBLY_TTL_SECONDS
        ]
        for chunk_id in expired:
            self._pending.pop(chunk_id, None)
