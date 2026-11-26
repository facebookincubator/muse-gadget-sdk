# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Strict JSON request/response helpers. No URL or trusted-source overrides."""

import base64
import json

from .compiler import AvatarError, MAX_IMAGE_BYTES, compile_image, validate_options

MAX_BASE64_CHARS = 4 * ((MAX_IMAGE_BYTES + 2) // 3)
MAX_BODY_BYTES = MAX_BASE64_CHARS + 4096
MAX_RESPONSE_BYTES = 800 * 1024
FIELDS = frozenset(("image_base64", "profile", "colors", "dither", "source_key"))


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise AvatarError("duplicate_field")
        result[key] = value
    return result


def _constant(_value):
    raise AvatarError("invalid_json")


def parse_request(body: bytes) -> dict:
    if type(body) is not bytes or not body:
        raise AvatarError("invalid_json")
    if len(body) > MAX_BODY_BYTES:
        raise AvatarError("body_bytes_exceeded")
    try:
        request = json.loads(
            body.decode("utf-8"), object_pairs_hook=_object, parse_constant=_constant
        )
    except AvatarError:
        raise
    except (ValueError, RecursionError):
        raise AvatarError("invalid_json") from None
    if type(request) is not dict:
        raise AvatarError("invalid_request")
    if request.keys() - FIELDS:
        raise AvatarError("unexpected_field")
    if "image_base64" not in request or type(request["image_base64"]) is not str:
        raise AvatarError("invalid_image_base64")
    if "source_key" in request and type(request["source_key"]) is not str:
        raise AvatarError("invalid_source_key")
    validate_options(
        request.get("profile", 64),
        request.get("colors", 24),
        request.get("dither", False),
        request.get("source_key"),
    )
    text = request["image_base64"]
    if not text or len(text) > MAX_BASE64_CHARS:
        raise AvatarError("image_bytes_exceeded")
    try:
        encoded = base64.b64decode(text, validate=True)
    except ValueError:
        raise AvatarError("invalid_image_base64") from None
    if not encoded or len(encoded) > MAX_IMAGE_BYTES:
        raise AvatarError("image_bytes_exceeded")
    # Reject excessive/noncanonical padding as well as URL/data URI inputs.
    if base64.b64encode(encoded).decode("ascii") != text:
        raise AvatarError("invalid_image_base64")
    return {
        "encoded": encoded,
        "profile": request.get("profile", 64),
        "colors": request.get("colors", 24),
        "dither": request.get("dither", False),
        "source_key": request.get("source_key"),
    }


def json_bytes(value) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")


def compile_request(body: bytes) -> bytes:
    options = parse_request(body)
    result = compile_image(**options)
    response = json_bytes(
        {
            "pack_base64": base64.b64encode(result.pack).decode("ascii"),
            "preview_png_base64": base64.b64encode(result.preview_png).decode("ascii"),
            "metadata": result.manifest,
        }
    )
    if len(response) > MAX_RESPONSE_BYTES:
        raise AvatarError("response_bytes_exceeded")
    return response
