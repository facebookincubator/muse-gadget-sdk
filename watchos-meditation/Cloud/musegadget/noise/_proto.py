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

from typing import Tuple


WIRE_VARINT = 0
WIRE_FIXED64 = 1
WIRE_DELIMITED = 2
WIRE_FIXED32 = 5

MAX_FIELD_NUMBER = (1 << 29) - 1
FIRST_RESERVED_FIELD_NUMBER = 19000
LAST_RESERVED_FIELD_NUMBER = 19999


class ProtoError(ValueError):
    """Raised when a Hatch Noise protobuf message is malformed."""


def _valid_field_number(field_number: int) -> bool:
    return (
        field_number != 0
        and field_number <= MAX_FIELD_NUMBER
        and not (FIRST_RESERVED_FIELD_NUMBER <= field_number <= LAST_RESERVED_FIELD_NUMBER)
    )


def encode_varint(value: int) -> bytes:
    if value < 0:
        raise ProtoError("varint value must be non-negative")
    if value >= 1 << 64:
        raise ProtoError("varint value exceeds uint64 range")

    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def signed_int64_value(value: int) -> int:
    if value < -(1 << 63) or value > (1 << 63) - 1:
        raise ProtoError("int64 value out of range")
    return value & ((1 << 64) - 1)


def signed_int32_value(value: int) -> int:
    if value < -(1 << 31) or value > (1 << 31) - 1:
        raise ProtoError("int32 value out of range")
    return value & ((1 << 64) - 1)


def decode_int64(value: int) -> int:
    if value >= 1 << 64:
        raise ProtoError("int64 wire value exceeds uint64 range")
    if value >= 1 << 63:
        return value - (1 << 64)
    return value


def decode_int32(value: int) -> int:
    if value >= 1 << 64:
        raise ProtoError("int32 wire value exceeds uint64 range")
    signed = value - (1 << 64) if value >= 1 << 63 else value
    if signed < -(1 << 31) or signed > (1 << 31) - 1:
        raise ProtoError("int32 value out of range")
    return signed


def decode_uint32(value: int) -> int:
    if value > 0xFFFFFFFF:
        raise ProtoError("uint32 value out of range")
    return value


def encode_key(field_number: int, wire_type: int) -> bytes:
    if not _valid_field_number(field_number):
        raise ProtoError("invalid field number")
    if wire_type not in (WIRE_VARINT, WIRE_FIXED64, WIRE_DELIMITED, WIRE_FIXED32):
        raise ProtoError("invalid wire type")
    return encode_varint((field_number << 3) | wire_type)


def varint_field(field_number: int, value: int) -> bytes:
    return encode_key(field_number, WIRE_VARINT) + encode_varint(value)


def int64_field(field_number: int, value: int) -> bytes:
    return varint_field(field_number, signed_int64_value(value))


def int32_field(field_number: int, value: int) -> bytes:
    return varint_field(field_number, signed_int32_value(value))


def uint32_field(field_number: int, value: int) -> bytes:
    if value < 0 or value > 0xFFFFFFFF:
        raise ProtoError("uint32 value out of range")
    return varint_field(field_number, value)


def bool_field(field_number: int, value: bool) -> bytes:
    return varint_field(field_number, 1 if value else 0)


def delimited_field(field_number: int, payload: bytes) -> bytes:
    return encode_key(field_number, WIRE_DELIMITED) + encode_varint(len(payload)) + payload


def string_field(field_number: int, value: str) -> bytes:
    return delimited_field(field_number, value.encode("utf-8"))


def bytes_field(field_number: int, value: bytes) -> bytes:
    return delimited_field(field_number, bytes(value))


def read_varint(data: bytes, offset: int) -> Tuple[int, int]:
    value = 0
    shift = 0
    for i in range(10):
        if offset >= len(data):
            raise ProtoError("truncated varint")
        byte = data[offset]
        offset += 1
        if i == 9 and (byte & 0xFE) != 0:
            raise ProtoError("malformed varint")
        value |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return value, offset
        shift += 7
    raise ProtoError("malformed varint")


def read_key(data: bytes, offset: int) -> Tuple[int, int, int]:
    key, offset = read_varint(data, offset)
    field_number = key >> 3
    wire_type = key & 0x07
    if not _valid_field_number(field_number):
        raise ProtoError("invalid field number")
    if wire_type not in (WIRE_VARINT, WIRE_FIXED64, WIRE_DELIMITED, WIRE_FIXED32):
        raise ProtoError("invalid wire type")
    return field_number, wire_type, offset


def read_delimited(data: bytes, offset: int) -> Tuple[bytes, int]:
    length, offset = read_varint(data, offset)
    end = offset + length
    if end > len(data):
        raise ProtoError("truncated delimited field")
    return data[offset:end], end


def skip_field(data: bytes, offset: int, wire_type: int) -> int:
    if wire_type == WIRE_VARINT:
        _, offset = read_varint(data, offset)
        return offset
    if wire_type == WIRE_FIXED64:
        end = offset + 8
        if end > len(data):
            raise ProtoError("truncated fixed64 field")
        return end
    if wire_type == WIRE_DELIMITED:
        _, offset = read_delimited(data, offset)
        return offset
    if wire_type == WIRE_FIXED32:
        end = offset + 4
        if end > len(data):
            raise ProtoError("truncated fixed32 field")
        return end
    raise ProtoError("invalid wire type")
