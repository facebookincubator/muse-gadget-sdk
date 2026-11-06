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
"""Host side of the C5 pipeline bench (CONFIG_HOMEHUB_PIPELINE_BENCH).

Announces itself by UDP broadcast, then serves plain TCP and TLS
connections, and plain HTTP /blob and /sink for timing the real Home Link
tunnel from the VM. The device's first line picks the direction: "TX" (the
device sends, the server counts) or "RX" (the server sends for --secs).
"""
import argparse
import http.server
import os
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.parse


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def serve_conn(conn, peer, kind, secs):
    try:
        conn.settimeout(30)
        line = b""
        while not line.endswith(b"\n"):
            c = conn.recv(1)
            if not c:
                return
            line += c
        cmd = line.strip().decode()
        t0 = time.monotonic()
        total = 0
        if cmd == "TX":
            while True:
                b = conn.recv(65536)
                if not b:
                    break
                total += len(b)
        elif cmd == "RX":
            blob = os.urandom(16384)
            end = t0 + secs + 1
            while time.monotonic() < end:
                conn.sendall(blob)
                total += len(blob)
        dt = time.monotonic() - t0
        log(f"{kind} {cmd} from {peer[0]}: {total} B in {dt:.2f} s = {total * 8 / dt / 1e6:.2f} Mbit/s")
    except Exception as e:  # noqa: BLE001 - keep serving
        log(f"{kind} error from {peer[0]}: {e}")
    finally:
        try:
            conn.close()
        except OSError:
            pass


def listener(port, kind, secs, ctx=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    s.bind(("0.0.0.0", port))
    s.listen(4)
    while True:
        conn, peer = s.accept()
        if ctx:
            try:
                conn = ctx.wrap_socket(conn, server_side=True)
            except Exception as e:  # noqa: BLE001
                log(f"tls handshake error from {peer[0]}: {e}")
                continue
        threading.Thread(target=serve_conn, args=(conn, peer, kind, secs), daemon=True).start()


class TunnelHTTP(http.server.BaseHTTPRequestHandler):
    """Plain-HTTP endpoints for timing the real Home Link tunnel from the VM:
    GET /blob?bytes=N streams N random bytes (LAN -> device -> VM),
    POST /sink reads and discards the body (VM -> device -> LAN).
    Both report bytes and seconds as measured here."""

    protocol_version = "HTTP/1.1"
    BLOB = os.urandom(65536)

    def log_message(self, fmt, *args):
        log(f"http {self.client_address[0]} " + fmt % args)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path != "/blob":
            self.send_error(404)
            return
        q = urllib.parse.parse_qs(url.query)
        n = max(0, min(int(q.get("bytes", ["10000000"])[0]), 1 << 30))
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(n))
        self.end_headers()
        t0 = time.monotonic()
        left = n
        while left > 0:
            chunk = self.BLOB[: min(left, len(self.BLOB))]
            self.wfile.write(chunk)
            left -= len(chunk)
        dt = time.monotonic() - t0
        log(f"http GET /blob {n} B in {dt:.2f} s = {n * 8 / dt / 1e6:.2f} Mbit/s (server side)")

    def do_POST(self):
        if urllib.parse.urlparse(self.path).path != "/sink":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", "0"))
        t0 = time.monotonic()
        left = n
        while left > 0:
            b = self.rfile.read(min(left, 65536))
            if not b:
                break
            left -= len(b)
        dt = max(time.monotonic() - t0, 1e-6)
        got = n - left
        body = f'{{"bytes":{got},"secs":{dt:.3f},"mbps":{got * 8 / dt / 1e6:.2f}}}\n'.encode()
        log(f"http POST /sink {got} B in {dt:.2f} s = {got * 8 / dt / 1e6:.2f} Mbit/s (server side)")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def make_ctx():
    d = tempfile.mkdtemp()
    key, crt = os.path.join(d, "k.pem"), os.path.join(d, "c.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                    "ec_paramgen_curve:prime256v1", "-nodes", "-days", "2",
                    "-subj", "/CN=c5bench", "-keyout", key, "-out", crt],
                   check=True, capture_output=True)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(crt, key)
    return ctx


def broadcast_addrs():
    """Each interface's subnet broadcast address. macOS won't route a
    limited broadcast (255.255.255.255) without a bound interface."""
    out = subprocess.run(["ifconfig"], capture_output=True, text=True).stdout
    addrs = sorted({tok[i + 1] for tok in (l.split() for l in out.splitlines())
                    for i, t in enumerate(tok[:-1]) if t == "broadcast"})
    return addrs or ["255.255.255.255"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp", type=int, default=5201)
    ap.add_argument("--tls", type=int, default=5202)
    ap.add_argument("--beacon", type=int, default=5200)
    ap.add_argument("--secs", type=int, default=8)
    ap.add_argument("--http", type=int, default=5280,
                    help="plain-HTTP /blob and /sink for tunnel tests (0 to disable)")
    a = ap.parse_args()
    if a.http:
        httpd = http.server.ThreadingHTTPServer(("0.0.0.0", a.http), TunnelHTTP)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        log(f"http /blob and /sink on :{a.http}")
    threading.Thread(target=listener, args=(a.tcp, "tcp", a.secs), daemon=True).start()
    threading.Thread(target=listener, args=(a.tls, "tls", a.secs, make_ctx()), daemon=True).start()
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    b.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    msg = f"C5BENCH {a.tcp} {a.tls}".encode()
    log(f"serving tcp:{a.tcp} tls:{a.tls}, beacon udp:{a.beacon}")
    dsts = broadcast_addrs()
    log(f"beacon to {dsts}")
    while True:
        for dst in dsts:
            try:
                b.sendto(msg, (dst, a.beacon))
            except OSError as e:
                log(f"beacon error: {e}")
        time.sleep(1)


if __name__ == "__main__":
    main()
