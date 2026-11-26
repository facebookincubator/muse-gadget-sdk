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

"""Keeps secrets out of what the bench tools print and log.

Link setup records carry secrets (pairing_confirmed's sdk_token, provision_v2's
access_token, refresh_token and Wi-Fi password), and a gadget's console or
Nordic UART log could show a token from older firmware. Every tool that prints
or logs a protocol record or device output passes it through here first
(AGENTS.md, Muse Gadgets SDK token): a secret becomes its length and a
SHA-256 prefix, never any of its characters, the same text the firmware's
token_fp_text() logs.
"""

from __future__ import annotations

import hashlib
import json
import re

# JSON keys whose values are secrets, in Link setup records, device status and
# the bench tools' own records.
SECRET_KEYS = frozenset({
    "sdk_token", "access_token", "refresh_token", "auth_token", "device_token", "token",
    "hatch_token", "password", "passphrase", "psk", "wifi_password", "proof_key", "secret",
})

# An SDK token (mgst_ and base64url), however much of it is there.
_SDK_TOKEN = re.compile(r"mgst_[A-Za-z0-9_-]{4,}")
# Bearer credentials in a header or a log line.
_BEARER = re.compile(r"\b(Bearer\s+(?:hatch_refresh:)?|hatch_refresh:)([A-Za-z0-9._~+/=-]{8,})")
# "key": "value" for a secret key, in JSON text (also a record printed with repr()):
# the value runs to its own closing quote, past escaped ones and the other quote;
# one never closed is masked to the end of its line.
_JSON_FIELD = re.compile(
    r"""(["'](?:%s)["']\s*:\s*)(?:"((?:[^"\\\r\n]|\\.)*)("?)|'((?:[^'\\\r\n]|\\.)*)('?))"""
    % "|".join(sorted(SECRET_KEYS)), re.I)
# Console commands that carry one: >sdk_token=..., hatch.token=..., hatch.token+=...
_CONSOLE_SET = re.compile(r"((?:^|[>\s])(?:sdk_token|hatch\.token\+?|token)=)(\S+)", re.I)
# ...and a Wi-Fi password, which may hold spaces: the rest of the line after
# wifi.pass= or wifi=ssid, is masked.
_CONSOLE_PASS = re.compile(r"((?:^|[>\s])(?:wifi\.pass=|wifi=[^,\r\n]*,))([^\r\n]+)", re.I)


def fingerprint(secret: str | bytes | None) -> str:
    """What may be shown of a secret: "none", or "48 chars, sha256 1a2b3c4d"."""
    if not secret:
        return "none"
    data = secret.encode() if isinstance(secret, str) else bytes(secret)
    return f"{len(data)} chars, sha256 {hashlib.sha256(data).hexdigest()[:8]}"


def _mask(secret: str) -> str:
    return f"<redacted: {fingerprint(secret)}>"


def _json_field(m: re.Match) -> str:
    quote, value, end = ('"', m.group(2), m.group(3)) if m.group(2) is not None else ("'", m.group(4), m.group(5))
    if not value:
        return m.group(0)
    return m.group(1) + quote + _mask(value) + end


def redact_text(text: str) -> str:
    """Text (a log line, a printed record) with every secret in it masked."""
    if not text:
        return text
    text = _JSON_FIELD.sub(_json_field, text)
    text = _CONSOLE_PASS.sub(lambda m: m.group(1) + _mask(m.group(2)), text)
    text = _CONSOLE_SET.sub(lambda m: m.group(1) + _mask(m.group(2)), text)
    text = _BEARER.sub(lambda m: m.group(1) + _mask(m.group(2)), text)
    return _SDK_TOKEN.sub(lambda m: _mask(m.group(0)), text)


def redact_bytes(data: bytes) -> bytes:
    """redact_text for raw console bytes (decoded as UTF-8, invalid bytes replaced)."""
    return redact_text(data.decode("utf-8", "replace")).encode()


def redact_record(obj):
    """A copy of a record (dicts, lists, strings) with secret values masked."""
    if isinstance(obj, dict):
        return {k: (_mask(v) if isinstance(k, str) and k.lower() in SECRET_KEYS and isinstance(v, (str, bytes))
                    and v else redact_record(v)) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return type(obj)(redact_record(v) for v in obj)
    if isinstance(obj, str):
        return redact_text(obj)
    if isinstance(obj, (bytes, bytearray)):
        return redact_bytes(bytes(obj))
    return obj


def record_text(obj) -> str:
    """A record as one line of JSON, secrets masked: what to print or put in an error."""
    try:
        return json.dumps(redact_record(obj), separators=(",", ":"))
    except (TypeError, ValueError):
        return redact_text(repr(obj))


class LineRedactor:
    """Masks a stream that arrives in pieces (console reads, notifications)
    line by line, so a secret split across two pieces is still caught."""

    def __init__(self):
        self._partial = ""

    def feed(self, text: str) -> str:
        """Whole lines (with their newlines) seen so far, masked; keeps the rest."""
        data = self._partial + text
        cut = data.rfind("\n") + 1
        self._partial = data[cut:]
        return redact_text(data[:cut])

    def flush(self) -> str:
        """Whatever is left of the last line, masked."""
        rest, self._partial = self._partial, ""
        return redact_text(rest)
