#!/usr/bin/env python3
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

"""Forward Pebble ring transcriptions to a Muse side chat.

A small webhook listener: the ring's companion app POSTs each transcription
to ``/ingest`` (multipart form or JSON, authenticated with a shared secret),
and this hands it to ``musegadget send-user-msg``, which delivers it over the device's
existing connection to the Muse. It holds no Muse credentials itself.

Configuration (environment):
  PEBBLE_SESSION_ID   side chat to post into (required)
  PEBBLE_SECRET_FILE  file holding the shared secret (default /etc/pebble-bridge/secret)
  PEBBLE_PORT         port to listen on (default 8787)
  MUSEGADGET          path to the musegadget command

Standard library only; run it with the system Python as an account in the
musegadget socket's group.
"""

from __future__ import annotations

import email.parser
import email.policy
import hmac
import json
import logging
import os
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

log = logging.getLogger("pebble-ring-bridge")

MAX_BODY_BYTES = 1024 * 1024
TEXT_FIELDS = ("transcription", "text", "transcript")
SEND_TIMEOUT_S = 100


def parse_body(content_type: str, raw: bytes) -> dict:
    """Return the request's fields from a multipart form, JSON, or plain text."""
    if content_type.startswith("multipart/form-data"):
        message = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
            b"Content-Type: " + content_type.encode("latin-1") + b"\r\n\r\n" + raw
        )
        fields = {}
        for part in message.iter_parts():
            name = part.get_param("name", header="content-disposition")
            if name:
                payload = part.get_payload(decode=True) or b""
                fields[name] = payload.decode("utf-8", errors="replace").strip()
        return fields
    if content_type.startswith("application/json"):
        data = json.loads(raw or b"{}")
        return data if isinstance(data, dict) else {}
    return {"text": raw.decode("utf-8", errors="replace").strip()}


def transcription(fields: dict) -> str:
    for key in TEXT_FIELDS:
        value = fields.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def presented_token(headers, fields: dict) -> str:
    token = headers.get("X-Pebble-Token") or headers.get("Authorization") or ""
    if token.startswith("Bearer "):
        token = token[len("Bearer "):]
    return token or str(fields.get("token") or "")


def send_user_msg(text: str) -> tuple[bool, str]:
    command = [os.environ.get("MUSEGADGET", "/opt/musegadget/venv/bin/musegadget"),
               "send-user-msg", "--session-id", os.environ["PEBBLE_SESSION_ID"], "-"]
    try:
        result = subprocess.run(command, input=text, text=True, capture_output=True,
                                timeout=SEND_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)
    return result.returncode == 0, (result.stdout or result.stderr).strip()


class Handler(BaseHTTPRequestHandler):
    secret = ""

    def do_GET(self):
        if self.path.split("?", 1)[0] == "/health":
            self._reply(200, {"ok": True})
        else:
            self._reply(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if self.path.split("?", 1)[0] != "/ingest":
            return self._reply(404, {"ok": False, "error": "not found"})
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return self._reply(400, {"ok": False, "error": "bad content length"})
        if length > MAX_BODY_BYTES:
            return self._reply(413, {"ok": False, "error": "body too large"})
        try:
            fields = parse_body(self.headers.get("Content-Type", ""), self.rfile.read(length))
        except (ValueError, UnicodeDecodeError):
            return self._reply(400, {"ok": False, "error": "unreadable body"})
        # Compared as bytes: compare_digest() raises on non-ASCII str input.
        presented = presented_token(self.headers, fields).encode()
        if not hmac.compare_digest(presented, self.secret.encode()):
            return self._reply(401, {"ok": False, "error": "invalid token"})
        text = transcription(fields)
        if not text:
            return self._reply(400, {"ok": False, "error": "no text"})
        delivered, detail = send_user_msg(f"{text} [via Pebble Ring]")
        log.info("transcription (%d chars) %s", len(text), "delivered" if delivered else f"failed: {detail}")
        self._reply(200 if delivered else 502, {"ok": delivered, "detail": detail})

    def _reply(self, status: int, body: dict) -> None:
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        log.debug("%s %s", self.address_string(), fmt % args)


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    os.environ["PEBBLE_SESSION_ID"]  # fail fast if unset
    with open(os.environ.get("PEBBLE_SECRET_FILE", "/etc/pebble-bridge/secret")) as f:
        Handler.secret = f.read().strip()
    if not Handler.secret:
        raise SystemExit("empty secret")
    port = int(os.environ.get("PEBBLE_PORT", "8787"))
    log.info("listening on :%d, posting to side chat %s", port, os.environ["PEBBLE_SESSION_ID"])
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
