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

from dataclasses import dataclass, field
from enum import IntEnum
from typing import List, Optional, Sequence, Union

from ._proto import (
    ProtoError,
    WIRE_DELIMITED,
    WIRE_VARINT,
    bool_field,
    bytes_field,
    decode_int32,
    decode_int64,
    delimited_field,
    int32_field,
    int64_field,
    read_delimited,
    read_key,
    read_varint,
    skip_field,
    string_field,
    varint_field,
)


class ServiceType(IntEnum):
    SERVICE_DAEMON = 0
    SERVICE_SENTINEL = 1
    SERVICE_VAULT = 2
    SERVICE_AUTHD = 3


class ResetCode(IntEnum):
    CODE_UNSPECIFIED = 0
    CANCELLED = 1
    TIMEOUT = 2
    PROTOCOL_ERROR = 3
    REFUSED_STREAM = 4
    INTERNAL_ERROR = 5
    SERVICE_UNAVAILABLE = 6


@dataclass(frozen=True)
class Header:
    key: str = ""
    value: str = ""


@dataclass(frozen=True)
class ApplicationRequest:
    verb: str = ""
    path: str = ""
    headers: Sequence[Header] = field(default_factory=list)
    body: bytes = b""
    end_body: bool = False


@dataclass(frozen=True)
class ApplicationResponse:
    status: int = 0
    headers: Sequence[Header] = field(default_factory=list)
    body: bytes = b""
    end_body: bool = False


@dataclass(frozen=True)
class BodyChunk:
    data: bytes = b""
    end_body: bool = False


@dataclass(frozen=True)
class Reset:
    code: ResetCode = ResetCode.CODE_UNSPECIFIED
    reason: str = ""


ServiceFrameValue = Union[ApplicationRequest, ApplicationResponse, BodyChunk, Reset]


@dataclass(frozen=True)
class ServiceFrame:
    stream_id: int = 0
    kind: Optional[str] = None
    value: Optional[ServiceFrameValue] = None

    @classmethod
    def request(cls, stream_id: int, request: ApplicationRequest) -> "ServiceFrame":
        return cls(stream_id=stream_id, kind="request", value=request)

    @classmethod
    def response(cls, stream_id: int, response: ApplicationResponse) -> "ServiceFrame":
        return cls(stream_id=stream_id, kind="response", value=response)

    @classmethod
    def body_chunk(cls, stream_id: int, body_chunk: BodyChunk) -> "ServiceFrame":
        return cls(stream_id=stream_id, kind="body_chunk", value=body_chunk)

    @classmethod
    def reset(cls, stream_id: int, reset: Reset) -> "ServiceFrame":
        return cls(stream_id=stream_id, kind="reset", value=reset)


@dataclass(frozen=True)
class ServiceRequest:
    service: ServiceType = ServiceType.SERVICE_DAEMON
    payload: bytes = b""


@dataclass(frozen=True)
class ServiceResponse:
    payload: bytes = b""


def _ensure_bytes(value: bytes) -> bytes:
    return bytes(value)


def _decode_string(value: bytes) -> str:
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProtoError("invalid utf-8 string") from exc


def _coerce_service_type(value: Union[ServiceType, int]) -> ServiceType:
    try:
        return ServiceType(int(value))
    except ValueError as exc:
        raise ProtoError("unknown service type") from exc


def _coerce_reset_code(value: Union[ResetCode, int]) -> ResetCode:
    try:
        return ResetCode(int(value))
    except ValueError as exc:
        raise ProtoError("unknown reset code") from exc


def encode_header(header: Header) -> bytes:
    out = bytearray()
    if header.key:
        out += string_field(1, header.key)
    if header.value:
        out += string_field(2, header.value)
    return bytes(out)


def decode_header(data: bytes) -> Header:
    key = ""
    value = ""
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("Header.key wrong wire type")
            raw, offset = read_delimited(data, offset)
            key = _decode_string(raw)
        elif field_number == 2:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("Header.value wrong wire type")
            raw, offset = read_delimited(data, offset)
            value = _decode_string(raw)
        else:
            offset = skip_field(data, offset, wire_type)
    return Header(key=key, value=value)


def encode_application_request(request: ApplicationRequest) -> bytes:
    out = bytearray()
    if request.verb:
        out += string_field(1, request.verb)
    if request.path:
        out += string_field(2, request.path)
    for header in request.headers:
        out += delimited_field(3, encode_header(header))
    body = _ensure_bytes(request.body)
    if body:
        out += bytes_field(4, body)
    if request.end_body:
        out += bool_field(5, True)
    return bytes(out)


def decode_application_request(data: bytes) -> ApplicationRequest:
    verb = ""
    path = ""
    headers: List[Header] = []
    body = b""
    end_body = False
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ApplicationRequest.verb wrong wire type")
            raw, offset = read_delimited(data, offset)
            verb = _decode_string(raw)
        elif field_number == 2:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ApplicationRequest.path wrong wire type")
            raw, offset = read_delimited(data, offset)
            path = _decode_string(raw)
        elif field_number == 3:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ApplicationRequest.headers wrong wire type")
            raw, offset = read_delimited(data, offset)
            headers.append(decode_header(raw))
        elif field_number == 4:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ApplicationRequest.body wrong wire type")
            body, offset = read_delimited(data, offset)
        elif field_number == 5:
            if wire_type != WIRE_VARINT:
                raise ProtoError("ApplicationRequest.end_body wrong wire type")
            raw, offset = read_varint(data, offset)
            end_body = raw != 0
        else:
            offset = skip_field(data, offset, wire_type)
    return ApplicationRequest(
        verb=verb,
        path=path,
        headers=headers,
        body=body,
        end_body=end_body,
    )


def encode_application_response(response: ApplicationResponse) -> bytes:
    out = bytearray()
    if response.status != 0:
        out += int32_field(1, response.status)
    for header in response.headers:
        out += delimited_field(2, encode_header(header))
    body = _ensure_bytes(response.body)
    if body:
        out += bytes_field(3, body)
    if response.end_body:
        out += bool_field(4, True)
    return bytes(out)


def decode_application_response(data: bytes) -> ApplicationResponse:
    status = 0
    headers: List[Header] = []
    body = b""
    end_body = False
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_VARINT:
                raise ProtoError("ApplicationResponse.status wrong wire type")
            raw, offset = read_varint(data, offset)
            status = decode_int32(raw)
        elif field_number == 2:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ApplicationResponse.headers wrong wire type")
            raw, offset = read_delimited(data, offset)
            headers.append(decode_header(raw))
        elif field_number == 3:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ApplicationResponse.body wrong wire type")
            body, offset = read_delimited(data, offset)
        elif field_number == 4:
            if wire_type != WIRE_VARINT:
                raise ProtoError("ApplicationResponse.end_body wrong wire type")
            raw, offset = read_varint(data, offset)
            end_body = raw != 0
        else:
            offset = skip_field(data, offset, wire_type)
    return ApplicationResponse(
        status=status,
        headers=headers,
        body=body,
        end_body=end_body,
    )


def encode_body_chunk(chunk: BodyChunk) -> bytes:
    out = bytearray()
    data = _ensure_bytes(chunk.data)
    if data:
        out += bytes_field(1, data)
    if chunk.end_body:
        out += bool_field(2, True)
    return bytes(out)


def decode_body_chunk(data: bytes) -> BodyChunk:
    chunk_data = b""
    end_body = False
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("BodyChunk.data wrong wire type")
            chunk_data, offset = read_delimited(data, offset)
        elif field_number == 2:
            if wire_type != WIRE_VARINT:
                raise ProtoError("BodyChunk.end_body wrong wire type")
            raw, offset = read_varint(data, offset)
            end_body = raw != 0
        else:
            offset = skip_field(data, offset, wire_type)
    return BodyChunk(data=chunk_data, end_body=end_body)


def encode_reset(reset: Reset) -> bytes:
    out = bytearray()
    code = _coerce_reset_code(reset.code)
    if code != ResetCode.CODE_UNSPECIFIED:
        out += int32_field(1, int(code))
    if reset.reason:
        out += string_field(2, reset.reason)
    return bytes(out)


def decode_reset(data: bytes) -> Reset:
    code = ResetCode.CODE_UNSPECIFIED
    reason = ""
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_VARINT:
                raise ProtoError("Reset.code wrong wire type")
            raw, offset = read_varint(data, offset)
            code = _coerce_reset_code(decode_int32(raw))
        elif field_number == 2:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("Reset.reason wrong wire type")
            raw, offset = read_delimited(data, offset)
            reason = _decode_string(raw)
        else:
            offset = skip_field(data, offset, wire_type)
    return Reset(code=code, reason=reason)


def encode_service_frame(frame: ServiceFrame) -> bytes:
    out = bytearray()
    if frame.stream_id != 0:
        out += int64_field(1, frame.stream_id)

    if frame.kind is None:
        if frame.value is not None:
            raise ProtoError("ServiceFrame value provided without kind")
        return bytes(out)

    if frame.kind == "request":
        if not isinstance(frame.value, ApplicationRequest):
            raise ProtoError("ServiceFrame request value has wrong type")
        out += delimited_field(2, encode_application_request(frame.value))
    elif frame.kind == "response":
        if not isinstance(frame.value, ApplicationResponse):
            raise ProtoError("ServiceFrame response value has wrong type")
        out += delimited_field(3, encode_application_response(frame.value))
    elif frame.kind == "body_chunk":
        if not isinstance(frame.value, BodyChunk):
            raise ProtoError("ServiceFrame body_chunk value has wrong type")
        out += delimited_field(4, encode_body_chunk(frame.value))
    elif frame.kind == "reset":
        if not isinstance(frame.value, Reset):
            raise ProtoError("ServiceFrame reset value has wrong type")
        out += delimited_field(5, encode_reset(frame.value))
    else:
        raise ProtoError("unknown ServiceFrame kind")
    return bytes(out)


def decode_service_frame(data: bytes) -> ServiceFrame:
    stream_id = 0
    kind: Optional[str] = None
    value: Optional[ServiceFrameValue] = None
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_VARINT:
                raise ProtoError("ServiceFrame.stream_id wrong wire type")
            raw, offset = read_varint(data, offset)
            stream_id = decode_int64(raw)
        elif field_number == 2:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ServiceFrame.request wrong wire type")
            raw, offset = read_delimited(data, offset)
            kind = "request"
            value = decode_application_request(raw)
        elif field_number == 3:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ServiceFrame.response wrong wire type")
            raw, offset = read_delimited(data, offset)
            kind = "response"
            value = decode_application_response(raw)
        elif field_number == 4:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ServiceFrame.body_chunk wrong wire type")
            raw, offset = read_delimited(data, offset)
            kind = "body_chunk"
            value = decode_body_chunk(raw)
        elif field_number == 5:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ServiceFrame.reset wrong wire type")
            raw, offset = read_delimited(data, offset)
            kind = "reset"
            value = decode_reset(raw)
        else:
            offset = skip_field(data, offset, wire_type)
    return ServiceFrame(stream_id=stream_id, kind=kind, value=value)


def encode_service_request(request: ServiceRequest) -> bytes:
    service = _coerce_service_type(request.service)
    out = bytearray()
    if service != ServiceType.SERVICE_DAEMON:
        out += varint_field(1, int(service))
    payload = _ensure_bytes(request.payload)
    if payload:
        out += bytes_field(2, payload)
    return bytes(out)


def decode_service_request(data: bytes) -> ServiceRequest:
    service = ServiceType.SERVICE_DAEMON
    payload = b""
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_VARINT:
                raise ProtoError("ServiceRequest.service wrong wire type")
            raw, offset = read_varint(data, offset)
            service = _coerce_service_type(raw)
        elif field_number == 2:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ServiceRequest.payload wrong wire type")
            payload, offset = read_delimited(data, offset)
        else:
            offset = skip_field(data, offset, wire_type)
    return ServiceRequest(service=service, payload=payload)


def encode_service_response(response: ServiceResponse) -> bytes:
    payload = _ensure_bytes(response.payload)
    if not payload:
        return b""
    return bytes_field(1, payload)


def decode_service_response(data: bytes) -> ServiceResponse:
    payload = b""
    offset = 0
    while offset < len(data):
        field_number, wire_type, offset = read_key(data, offset)
        if field_number == 1:
            if wire_type != WIRE_DELIMITED:
                raise ProtoError("ServiceResponse.payload wrong wire type")
            payload, offset = read_delimited(data, offset)
        else:
            offset = skip_field(data, offset, wire_type)
    return ServiceResponse(payload=payload)
