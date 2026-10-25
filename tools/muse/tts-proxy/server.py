#!/usr/bin/env python3
"""Muse Gadget → ElevenLabs streaming MP3 proxy. Standard library only.

Bind behind an HTTPS reverse proxy. Never expose this HTTP process directly to
the Internet; it accepts a distinct device bearer token (not the vendor key).
"""
from __future__ import annotations

import hmac
import json
import logging
import os
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import HTTPRedirectHandler, Request, build_opener

LOG = logging.getLogger("muse_tts")
MAX_REQUEST_BYTES = 4096
MAX_TEXT_CHARS = 1023
MAX_MP3_BYTES = 512 * 1024


@dataclass(frozen=True)
class Settings:
    elevenlabs_key: str
    voice_id: str
    proxy_token: str
    model_id: str = "eleven_flash_v2_5"

    @classmethod
    def from_env(cls) -> "Settings":
        s = cls(
            elevenlabs_key=os.environ.get("ELEVENLABS_API_KEY", ""),
            voice_id=os.environ.get("ELEVENLABS_VOICE_ID", ""),
            proxy_token=os.environ.get("TTS_PROXY_TOKEN", ""),
            model_id=os.environ.get("ELEVENLABS_MODEL_ID", "eleven_flash_v2_5"),
        )
        if not s.elevenlabs_key or not s.voice_id or len(s.proxy_token) < 24:
            raise ValueError("Set ELEVENLABS_API_KEY, ELEVENLABS_VOICE_ID and a random TTS_PROXY_TOKEN (>=24 characters)")
        return s


class _NoRedirect(HTTPRedirectHandler):
    """Never send the ElevenLabs API key to a redirect destination."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def open_upstream(request: Request):
    return build_opener(_NoRedirect()).open(request, timeout=35)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MuseGadgetTTS/1.0"
    settings: Settings

    def _json(self, code: int, message: str) -> None:
        body = json.dumps({"error": message}).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_GET(self) -> None:
        if self.path == "/health":
            self._json(200, "ok")
        else:
            self._json(404, "not found")

    def do_POST(self) -> None:
        if self.path != "/v1/tts":
            self._json(404, "not found")
            return
        expected = "Bearer " + self.settings.proxy_token
        if not hmac.compare_digest(self.headers.get("Authorization", ""), expected):
            self._json(401, "unauthorized")
            return
        length_value = self.headers.get("Content-Length", "")
        try:
            length = int(length_value)
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_REQUEST_BYTES:
            self._json(413, "invalid request size")
            return
        if self.headers.get("Content-Type", "").split(";")[0].lower() != "application/json":
            self._json(415, "expected application/json")
            return
        try:
            payload = json.loads(self.rfile.read(length))
        except (ValueError, UnicodeDecodeError):
            self._json(400, "invalid JSON")
            return
        text = payload.get("text") if isinstance(payload, dict) else None
        if not isinstance(text, str) or not 1 <= len(text.strip()) <= MAX_TEXT_CHARS:
            self._json(422, "invalid text")
            return

        url = ("https://api.elevenlabs.io/v1/text-to-speech/"
               + quote(self.settings.voice_id, safe="")
               + "/stream?output_format=mp3_22050_32")
        body = json.dumps({"text": text, "model_id": self.settings.model_id},
                          ensure_ascii=False).encode("utf-8")
        request = Request(url, data=body, method="POST", headers={
            "xi-api-key": self.settings.elevenlabs_key,
            "Content-Type": "application/json",
            "Accept": "audio/mpeg",
        })
        try:
            upstream = open_upstream(request)
        except HTTPError as exc:
            LOG.warning("ElevenLabs returned HTTP %s", exc.code)
            self._json(502, "speech provider unavailable")
            return
        except (URLError, TimeoutError, OSError) as exc:
            LOG.warning("ElevenLabs connection failed: %s", type(exc).__name__)
            self._json(502, "speech provider unavailable")
            return

        with upstream:
            media_type = upstream.headers.get("Content-Type", "").lower()
            if not media_type.startswith(("audio/mpeg", "audio/mp3")):
                LOG.warning("Unexpected upstream content type")
                self._json(502, "unexpected speech response")
                return
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Connection", "close")
            self.end_headers()
            used = 0
            try:
                while True:
                    chunk = upstream.read(2048)
                    if not chunk:
                        break
                    used += len(chunk)
                    if used > MAX_MP3_BYTES:
                        LOG.warning("Speech length exceeded proxy limit")
                        break   # Client detects incomplete chunked response.
                    self.wfile.write(("%X\r\n" % len(chunk)).encode("ascii") + chunk + b"\r\n")
                    self.wfile.flush()
                if used <= MAX_MP3_BYTES:
                    self.wfile.write(b"0\r\n\r\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
                LOG.info("Client disconnected during audio streaming")
            self.close_connection = True

    def log_message(self, fmt: str, *args: object) -> None:
        LOG.info("%s - %s", self.client_address[0], fmt % args)


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    Handler.settings = Settings.from_env()
    host = os.environ.get("LISTEN_HOST", "0.0.0.0")
    port = int(os.environ.get("LISTEN_PORT", "8765"))
    server = ThreadingHTTPServer((host, port), Handler)
    server.daemon_threads = True
    LOG.info("Muse TTS ready on %s:%s (HTTP behind TLS reverse proxy)", host, port)
    server.serve_forever()


if __name__ == "__main__":
    main()
