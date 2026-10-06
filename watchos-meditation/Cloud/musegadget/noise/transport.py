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

from dataclasses import dataclass
from typing import Optional, Sequence, Union

from .envelope import (
    ApplicationRequest,
    ApplicationResponse,
    BodyChunk,
    Header,
    Reset,
    ResetCode,
    ServiceFrame,
    ServiceRequest,
    ServiceResponse,
    ServiceType,
    decode_service_frame,
    decode_service_request,
    decode_service_response,
    encode_service_frame,
    encode_service_request,
    encode_service_response,
)
from .framing import NoiseFrameDecoder, encode_noise_frames
from .noise_xx import CipherState, NoiseProtocolError


EMPTY_AD = b""

SERVICE_MAP = {
    "daemon": ServiceType.SERVICE_DAEMON,
    "sentinel": ServiceType.SERVICE_SENTINEL,
    "vault": ServiceType.SERVICE_VAULT,
    "authd": ServiceType.SERVICE_AUTHD,
}


@dataclass(frozen=True)
class EncryptedFrames:
    stream_id: int
    frames: Sequence[bytes]


@dataclass(frozen=True)
class DecryptedFrame:
    kind: str
    stream_id: int
    value: Union[ApplicationResponse, BodyChunk, Reset]

    @property
    def response(self) -> Optional[ApplicationResponse]:
        return self.value if self.kind == "response" else None

    @property
    def body_chunk(self) -> Optional[BodyChunk]:
        return self.value if self.kind == "body_chunk" else None

    @property
    def reset(self) -> Optional[Reset]:
        return self.value if self.kind == "reset" else None


def _coerce_service(service: Union[str, ServiceType]) -> ServiceType:
    if isinstance(service, ServiceType):
        return service
    try:
        return SERVICE_MAP[service]
    except KeyError as exc:
        raise NoiseProtocolError(f"unknown noise service: {service}") from exc


class NoiseTransport:
    def __init__(self, send: CipherState, recv: CipherState) -> None:
        self._send = send
        self._recv = recv
        self._decoder = NoiseFrameDecoder()
        self._next_stream_id = 1
        self._dead = False

    def _assert_alive(self) -> None:
        if self._dead:
            raise NoiseProtocolError("NoiseTransport: dead after prior failure")

    def encrypt_http_request(
        self,
        http_method: str,
        path: str,
        body: bytes = b"",
        service: Union[str, ServiceType] = "daemon",
        headers: Optional[Sequence[Header]] = None,
    ) -> EncryptedFrames:
        return self._send_application_request(
            http_method=http_method,
            path=path,
            body=body,
            service=service,
            headers=headers,
            end_body=True,
        )

    def start_stream_request(
        self,
        http_method: str,
        path: str,
        service: Union[str, ServiceType] = "daemon",
        headers: Optional[Sequence[Header]] = None,
    ) -> EncryptedFrames:
        return self._send_application_request(
            http_method=http_method,
            path=path,
            body=b"",
            service=service,
            headers=headers,
            end_body=False,
        )

    def _send_application_request(
        self,
        http_method: str,
        path: str,
        body: bytes,
        service: Union[str, ServiceType],
        headers: Optional[Sequence[Header]],
        end_body: bool,
    ) -> EncryptedFrames:
        self._assert_alive()
        try:
            stream_id = self._next_stream_id
            self._next_stream_id += 1
            request = ApplicationRequest(
                verb=http_method,
                path=path,
                headers=list(headers or []),
                body=bytes(body),
                end_body=end_body,
            )
            frame = ServiceFrame.request(stream_id, request)
            frames = self._encrypt_request(_coerce_service(service), frame)
            return EncryptedFrames(stream_id=stream_id, frames=frames)
        except Exception:
            self._dead = True
            raise

    def encrypt_body_chunk(
        self,
        stream_id: int,
        data: bytes,
        service: Union[str, ServiceType] = "daemon",
        end_body: bool = False,
    ) -> Sequence[bytes]:
        self._assert_alive()
        try:
            frame = ServiceFrame.body_chunk(
                stream_id,
                BodyChunk(data=bytes(data), end_body=end_body),
            )
            return self._encrypt_request(_coerce_service(service), frame)
        except Exception:
            self._dead = True
            raise

    def encrypt_reset(
        self,
        stream_id: int,
        service: Union[str, ServiceType] = "daemon",
        reason: str = "",
        code: ResetCode = ResetCode.CANCELLED,
    ) -> Sequence[bytes]:
        self._assert_alive()
        try:
            frame = ServiceFrame.reset(
                stream_id,
                Reset(code=code, reason=reason),
            )
            return self._encrypt_request(_coerce_service(service), frame)
        except Exception:
            self._dead = True
            raise

    def decrypt_frame(self, ciphertext: bytes) -> Optional[DecryptedFrame]:
        self._assert_alive()
        try:
            plain_frame = self._recv.decrypt_with_ad(EMPTY_AD, bytes(ciphertext))
            reassembled = self._decoder.decode(plain_frame)
            if reassembled is None:
                return None

            response = decode_service_response(reassembled)
            if len(response.payload) == 0:
                raise NoiseProtocolError("empty ServiceResponse payload")

            frame = decode_service_frame(response.payload)
            if frame.kind == "response":
                if not isinstance(frame.value, ApplicationResponse):
                    raise NoiseProtocolError("invalid response frame")
                return DecryptedFrame(
                    kind="response",
                    stream_id=frame.stream_id,
                    value=frame.value,
                )
            if frame.kind == "body_chunk":
                if not isinstance(frame.value, BodyChunk):
                    raise NoiseProtocolError("invalid body_chunk frame")
                return DecryptedFrame(
                    kind="body_chunk",
                    stream_id=frame.stream_id,
                    value=frame.value,
                )
            if frame.kind == "reset":
                if not isinstance(frame.value, Reset):
                    raise NoiseProtocolError("invalid reset frame")
                return DecryptedFrame(
                    kind="reset",
                    stream_id=frame.stream_id,
                    value=frame.value,
                )
            if frame.kind == "request":
                raise NoiseProtocolError("NoiseTransport: unexpected request frame from server")
            return None
        except Exception:
            self._dead = True
            raise

    def _encrypt_request(self, service: ServiceType, frame: ServiceFrame) -> Sequence[bytes]:
        frame_bytes = encode_service_frame(frame)
        request_bytes = encode_service_request(
            ServiceRequest(service=service, payload=frame_bytes)
        )
        proto_frames = encode_noise_frames(request_bytes)
        return [self._send.encrypt_with_ad(EMPTY_AD, frame) for frame in proto_frames]


def decode_request_envelope(data: bytes) -> ServiceFrame:
    request = decode_service_request(data)
    return decode_service_frame(request.payload)


def encode_response_envelope(frame: ServiceFrame) -> bytes:
    return encode_service_response(ServiceResponse(payload=encode_service_frame(frame)))
