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

"""Hatch Noise protocol: Noise XX handshake, framing and service envelopes."""

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
from .framing import (
    MAX_ASSEMBLY_BYTES,
    MAX_CHUNK_PAYLOAD,
    MAX_TOTAL_CHUNKS,
    NoiseFrameDecoder,
    NoiseTransportFrame,
    decode_noise_frame,
    encode_noise_frames,
)
from .noise_xx import CipherState, NoiseProtocolError, NoiseXXInitiator, NoiseXXResponder
from .transport import DecryptedFrame, EncryptedFrames, NoiseTransport

__all__ = [
    "ApplicationRequest",
    "ApplicationResponse",
    "BodyChunk",
    "CipherState",
    "DecryptedFrame",
    "EncryptedFrames",
    "Header",
    "MAX_ASSEMBLY_BYTES",
    "MAX_CHUNK_PAYLOAD",
    "MAX_TOTAL_CHUNKS",
    "NoiseFrameDecoder",
    "NoiseProtocolError",
    "NoiseTransport",
    "NoiseTransportFrame",
    "NoiseXXInitiator",
    "NoiseXXResponder",
    "Reset",
    "ResetCode",
    "ServiceFrame",
    "ServiceRequest",
    "ServiceResponse",
    "ServiceType",
    "decode_noise_frame",
    "decode_service_frame",
    "decode_service_request",
    "decode_service_response",
    "encode_noise_frames",
    "encode_service_frame",
    "encode_service_request",
    "encode_service_response",
]
