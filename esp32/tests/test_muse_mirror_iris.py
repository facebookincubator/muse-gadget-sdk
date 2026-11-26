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

"""The mirror over ESP-Iris: the board's link (C) and the viewer's client against a fake USB data port."""
import importlib.util
import json
import os
import re
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import zlib

try:
    import numpy as np
except ImportError:
    raise unittest.SkipTest("numpy is needed for the viewer")

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/muse'))
import mirror_iris as iris  # noqa: E402
sys.path.pop(0)
spec = importlib.util.spec_from_file_location('muse_iris_viewer', ROOT / 'tools/muse/mirror.py')
mirror = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mirror)

DEVICE = iris.DEVICE_PREFIX + '0123456789ab'
BOARD = dict(version=2, name='Espressif ESP-Mosaico', w=3, h=2, touch=True, keyboard=False, audio=False,
             route=False, buttons=[])
STRIDE = 256
PIPE = iris.MEDIA.pack(0, 0, 128, 65535, STRIDE, 1, iris.MARKER)


class Board:
    """The board's end of the pipe, as muse_mirror_iris.c cuts it: messages behind their length, pulls of
    whole rows behind epoch and valid bytes, each the body of a media DATA frame."""

    def __init__(self, capacity=3840, height=65535):
        self.capacity, self.height = capacity, height
        self.ring, self.frame, self.y = bytearray(), 1, 0

    def send(self, message):
        self.ring += struct.pack('<I', len(message)) + message

    def pull(self, epoch, dropped=0, stride=STRIDE):
        n = min(len(self.ring), self.capacity - iris.TILE.size)
        if not n:
            return None
        data = iris.TILE.pack(epoch, n) + bytes(self.ring[:n])
        del self.ring[:n]
        data += bytes(-len(data) % STRIDE)
        rows = len(data) // STRIDE
        body = (struct.pack('<QIIHH', 0, self.frame, dropped, 0, len(data))
                + iris.MEDIA.pack(0, self.y, 128, rows, stride, 1, iris.MARKER) + data)
        self.y += rows
        if self.y == self.height:
            self.frame, self.y = self.frame + 1, 0
        return body

    def pulls(self, epoch, **kw):
        out = []
        while True:
            tile = self.pull(epoch, **kw)
            if tile is None:
                return out
            out.append(tile)


def frame(pixels):
    raw = np.asarray(pixels, '<u2').tobytes()
    return b'F\0' + struct.pack('<HHH', 1, 3, 2) + struct.pack('<HHHHBBI', 0, 0, 3, 2, 0, 0, len(raw)) + raw


class BoardLink(unittest.TestCase):
    @unittest.skipUnless(os.name == 'posix', 'C harness requires a POSIX sanitizer toolchain')
    def test_firmware_iris_link(self):
        source = (ROOT / 'components_mosaico/muse_mosaico_iris/muse_mirror_iris.c').read_text()
        start = source.index('#include "muse_mirror_link.h"\n') + len('#include "muse_mirror_link.h"\n')
        impl = source[start:source.rindex('#endif')]
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'iris_impl.inc').write_text(impl)
            exe = folder / 'iris-harness'
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I' + str(folder), '-I' + str(ROOT / 'components/muse_mirror'),
                str(ROOT / 'tests/muse_mirror_iris_harness.c'), '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True, env={**os.environ, 'TMPDIR': tempfile.gettempdir()})
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([exe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, 'iris link ok\n')

    def test_option_and_hook(self):
        kconfig = (ROOT / 'components/muse_mirror/Kconfig').read_text()
        option = kconfig[kconfig.index('config MUSE_MIRROR_IRIS'):]
        self.assertIn('depends on MUSE_MIRROR && MUSE_BOARD_ESP_MOSAICO && ESP_IRIS_ENABLE', option[:300])
        start = (ROOT / 'components_mosaico/muse_mosaico_iris/muse_mosaico_iris.c').read_text()
        self.assertLess(start.index('muse_mirror_iris_register();'), start.index('iris_ota_support_start();'))
        # The bench's methods after the way back's, so a full RPC table never costs the way back; the table holds
        # the mirror's, the way back's and the bench's with room.
        body = start[start.index('void muse_mosaico_iris_start(void)'):]
        self.assertLess(body.index('iris_ota_support_start();'), body.index('bench_rpc_register();'))
        self.assertEqual(start.count('esp_iris_rpc_register('), 1)   # the bench table's loop, warning per method
        self.assertIn('not registered (%s)', start)
        overlay = (ROOT / 'devices/sdkconfig.muse-esp-mosaico-iris').read_text()
        handlers = int(re.search(r'^CONFIG_ESP_IRIS_MAX_RPC_HANDLERS=(\d+)$', overlay, re.M).group(1))
        bench = start[start.index('} s_bench_rpcs[] = {'):start.index('#define BENCH_RPCS')].count('{ BENCH_RPC_')
        self.assertGreaterEqual(handlers, 3 + 3 + bench + 4)   # mirror + way back + bench + headroom
        cmake = (ROOT / 'components_mosaico/muse_mosaico_iris/CMakeLists.txt').read_text()
        self.assertIn('"muse_mirror_iris.c"', cmake)
        link = (ROOT / 'components_mosaico/muse_mosaico_iris/muse_mirror_iris.c').read_text()
        self.assertIn('.audio = true', link)                # sound fits the direct link
        self.assertTrue(mirror.IrisLink('iris:' + DEVICE).native)   # so the microphone goes as PCM


class Pipe(unittest.TestCase):
    def test_messages_across_tiles(self):
        pipe = iris.Pipe(PIPE)
        pipe.reset(5)
        board = Board(capacity=512)
        big = bytes(range(256)) * 6
        for m in (b'B{}', big, b'O\x01'):
            board.send(m)
        got = []
        for tile in board.pulls(5):
            got += pipe.feed(tile)
        self.assertEqual(got, [b'B{}', big, b'O\x01'])
        # The last viewer's bytes still on their way are skipped.
        board.send(b'x')
        self.assertEqual(pipe.feed(board.pull(4)), [])
        board.send(b'y')
        self.assertEqual(pipe.feed(board.pull(5)), [b'y'])

    def test_frame_wraps(self):
        pipe = iris.Pipe(iris.MEDIA.pack(0, 0, 128, 2, STRIDE, 1, iris.MARKER))
        pipe.reset(5)
        board = Board(capacity=STRIDE, height=2)
        got = []
        for i in range(5):
            board.send(b'%d' % i)
            got += pipe.feed(board.pull(5))
        self.assertEqual(got, [b'0', b'1', b'2', b'3', b'4'])

    def test_gaps(self):
        def tiles(**kw):
            board = Board(capacity=STRIDE)
            board.send(b'm' * 600)
            return board.pulls(5, **kw)
        two = tiles()
        cases = {
            'missing tile': [two[0], two[2]],
            'dropped': tiles(dropped=1)[:1],
            'stride': tiles(stride=128)[:1],
            'bad length': [two[0][:-STRIDE]],
        }
        for name, frames in cases.items():
            pipe = iris.Pipe(PIPE)
            pipe.reset(5)
            with self.subTest(name), self.assertRaises(iris.Gap):
                for f in frames:
                    pipe.feed(f)
        with self.assertRaises(ValueError):
            iris.Pipe(iris.MEDIA.pack(0, 0, 128, 65535, STRIDE, 1, 0))

    def test_cobs(self):
        for data in (b'', b'\0', b'\0\0', bytes(range(1, 256)), bytes(254) + b'x', bytes(range(256)) * 3):
            encoded = iris.cobs_encode(data)
            self.assertNotIn(0, encoded)
            self.assertEqual(iris.cobs_decode(encoded), data)
        with self.assertRaises(ValueError):
            iris.cobs_decode(b'\x05ab')

    def test_target(self):
        self.assertIsInstance(mirror.open_link('iris:' + DEVICE, 115200), mirror.IrisLink)
        self.assertIs(type(mirror.open_link('/dev/ttyACM0', 115200)), mirror.Link)
        link = mirror.open_link('iris:' + DEVICE, 115200)
        with patch.object(iris, 'usb_boards', return_value=[]), patch.object(iris, 'gateway_running', return_value=False), \
                patch.object(mirror, 'iris_module', return_value=iris), self.assertRaisesRegex(ConnectionError, 'on USB'):
            link.connect()

    @unittest.skipUnless(os.name == 'posix', 'flock')
    def test_a_running_gateway_keeps_the_viewer_off_the_port(self):
        import fcntl
        with tempfile.TemporaryDirectory() as folder, patch.object(iris.tempfile, 'gettempdir', return_value=folder):
            self.assertFalse(iris.gateway_running())
            locks = Path(folder) / 'esp-iris-locks'
            locks.mkdir()
            (locks / 'stale.lock').write_bytes(b'pid=1\n')
            self.assertFalse(iris.gateway_running())      # a file nobody holds is no Gateway
            with (locks / 'held.lock').open('a+b') as held:
                fcntl.flock(held.fileno(), fcntl.LOCK_EX)
                self.assertTrue(iris.gateway_running())
                link = mirror.open_link('iris:' + DEVICE, 115200)
                with patch.object(iris, 'usb_boards', side_effect=AssertionError('no port is looked at')), \
                        patch.object(mirror, 'iris_module', return_value=iris), \
                        self.assertRaisesRegex(ConnectionError, 'Gateway is running'):
                    link.connect()
            self.assertFalse(iris.gateway_running())

    def test_writer_sends_a_drags_newest_position(self):
        """Queued faster than RPCs go: a press, the drag's last position, the release, then the key."""
        link = mirror.IrisLink('iris:' + DEVICE)
        sent = []

        class Client:
            def input(self, payload):
                sent.append(payload)
                if payload[0] == 6:
                    link.sock = None

        link.pointer(0, 1, True)
        for x in range(1, 20):
            link.pointer(x, 1, True)
        link.pointer(19, 1, False)
        link.pointer(5, 1, True)
        link.pointer(6, 1, True)
        link.key(2, True)
        link.sock = client = Client()
        link.writer(client)
        press = lambda x, down=1: struct.pack('<BHH', down, x, 1)
        self.assertEqual(sent, [press(0), press(19), press(19, 0), press(5), press(6), bytes((6, 2, 1))])


class FakeDataPort:
    """The board's ESP-Iris data port: HELLO until acknowledged, then RPCs, the SCREEN mirror and its tiles,
    answered as they are written."""

    def __init__(self, role=1, messages=(), lose_tile=None):
        self.rx, self.lock, self.closed = bytearray(), threading.Lock(), False
        self.decoder, self.session, self.seq, self.role = bytearray(), 0x1234, 0, role
        self.messages, self.board, self.lose_tile = list(messages), Board(), lose_tile
        self.inputs, self.starts, self.commands, self.credits, self.stopped = [], [], [], [], False
        self.emit(iris.CONTROL, iris.HELLO, bytes((iris.LINK_ROLE, 1, 0, role)))

    def emit(self, channel, type_, body=b'', rid=0, stream=0):
        self.seq += 1
        plain = iris.ENVELOPE.pack(b'IRIS', 2, 32, channel, type_, 0, 0, self.session, rid, stream, self.seq,
                                   len(body)) + body
        with self.lock:
            self.rx += iris.cobs_encode(plain + struct.pack('<I', zlib.crc32(plain))) + b'\0'

    @property
    def in_waiting(self):
        return len(self.rx)

    def read(self, n):
        if self.closed:
            raise OSError('closed')
        if not self.rx:
            time.sleep(.01)
        with self.lock:
            out = bytes(self.rx[:n])
            del self.rx[:n]
        return out

    def close(self):
        self.closed = True

    def write(self, data):
        self.decoder += data
        while b'\0' in self.decoder:
            raw, _, rest = self.decoder.partition(b'\0')
            self.decoder = bytearray(rest)
            plain = iris.cobs_decode(raw)
            _, _, _, channel, type_, _, _, session, rid, stream, _, size = iris.ENVELOPE.unpack_from(plain)
            assert zlib.crc32(plain[:-4]) == struct.unpack_from('<I', plain, len(plain) - 4)[0]
            self.answer(channel, type_, session, rid, stream, plain[32:32 + size])
        return len(data)

    def answer(self, channel, type_, session, rid, stream, body):
        if (channel, type_) == (iris.CONTROL, iris.HELLO_ACK):
            assert body[0] == 1 and len(body) == 17 and any(body[1:]) and session == self.session
            self.emit(iris.CONTROL, iris.AUTH_RESULT, b'\1')
        elif (channel, type_) == (iris.CONTROL, iris.CREDIT):
            self.credits.append((body[0], struct.unpack_from('<I', body, 4)[0], stream))
        elif (channel, type_) == (iris.CONTROL, iris.REQUEST):
            service, method, _, n, _ = struct.unpack_from('<HHIHH', body)
            payload = body[12:12 + n]
            answer = {iris.BOARD: json.dumps(dict(BOARD, iris_pipe=1)).encode(), iris.STREAM: payload,
                      iris.INPUT: payload[:4]}[method]
            if method == iris.STREAM:
                self.commands.append(payload[0])
            if method == iris.INPUT:
                self.inputs.append(payload[4:])
            self.emit(iris.CONTROL, iris.RESPONSE, struct.pack('<HHiHH', service, method, 0, len(answer), 0) + answer,
                      rid)
            if method == iris.STREAM and payload[0] == iris.START:
                epoch = struct.unpack_from('<I', payload, 1)[0]
                self.starts.append(epoch)
                for m in self.messages:
                    self.board.send(m)
                for i, tile in enumerate(self.board.pulls(epoch)):
                    if len(self.starts) == 1 and i == self.lose_tile:
                        continue
                    self.emit(iris.SCREEN, iris.MEDIA_DATA, tile, stream=7)
        elif (channel, type_) == (iris.SCREEN, iris.MIRROR_START):
            self.emit(iris.SCREEN, iris.MIRROR_STATE, PIPE, rid, 7)
        elif (channel, type_) == (iris.SCREEN, iris.MIRROR_STOP):
            self.stopped = stream == 7
            self.emit(iris.SCREEN, iris.MIRROR_STATE, b'', rid, 7)


class Viewer(unittest.TestCase):
    MESSAGES = [b'B' + json.dumps(BOARD).encode(), frame([[1, 2, 3], [4, 5, 12]]), b'O\x01']

    def receive(self, client, count):
        got, end = [], time.monotonic() + 8
        while len(got) < count and time.monotonic() < end:
            m = client.receive(timeout=1)
            if m is not None:
                got.append(m)
        return got

    def test_viewer_over_the_data_port(self):
        port = FakeDataPort(messages=self.MESSAGES)
        client = iris.IrisClient('/dev/fake', open_port=lambda name: port)
        try:
            self.assertEqual(client.open()['iris_pipe'], 1)
            self.assertEqual(port.credits, [(iris.SCREEN, iris.CREDIT_GRANT, 7)])
            self.assertEqual(self.receive(client, 3), self.MESSAGES)
            client.input(bytes((6, 2, 1)))
            self.assertEqual(port.inputs, [bytes((6, 2, 1))])
        finally:
            client.close()
        self.assertEqual(port.commands, [iris.START, iris.END])
        self.assertTrue(port.stopped and port.closed)

    def test_viewer_through_the_link(self):
        port = FakeDataPort(messages=self.MESSAGES)
        link = mirror.IrisLink('iris:' + DEVICE)
        boards = []
        link.on_board = boards.append
        with patch.object(iris, 'usb_boards', return_value=[(DEVICE, '/dev/fake')]), \
                patch.object(iris.IrisClient, '__init__', autospec=True,
                             side_effect=lambda self, name: IRIS_INIT(self, name, open_port=lambda n: port)), \
                patch.object(mirror, 'iris_module', return_value=iris), \
                patch.object(iris, 'gateway_running', return_value=False):
            try:
                link.start()
                end = time.monotonic() + 8
                while not (link.fb is not None and link.fb[1, 2] == 12 and link.text_open) and time.monotonic() < end:
                    time.sleep(.01)
                self.assertEqual(link.fb[1, 2], 12)
                self.assertEqual(boards[0]['name'], 'Espressif ESP-Mosaico')
                self.assertEqual(link.received, sum(len(m) for m in self.MESSAGES))
                end = time.monotonic() + 5
                while port.inputs[:2] != [b'O', b'Z'] and time.monotonic() < end:
                    time.sleep(.01)
                self.assertEqual(port.inputs[:2], [b'O', b'Z'])   # the cable's options go as input
            finally:
                link.close()
            self.assertEqual(port.commands[-1:], [iris.END])   # closing the window tells the board
            self.assertTrue(port.stopped)

    def test_lost_tile_starts_a_new_epoch(self):
        messages = [b'B' + json.dumps(dict(BOARD, name='x' * 300)).encode()] + self.MESSAGES[1:]
        port = FakeDataPort(messages=messages, lose_tile=0)
        port.board.capacity = STRIDE                # B spans several tiles
        client = iris.IrisClient('/dev/fake', open_port=lambda name: port)
        try:
            client.open()
            got = self.receive(client, 3)
        finally:
            client.close()
        self.assertEqual(len(port.starts), 2)
        self.assertNotEqual(port.starts[0], port.starts[1])
        self.assertEqual(got, messages)             # the board took the viewer afresh

    def test_board_refuses_input(self):
        port = FakeDataPort(messages=self.MESSAGES)
        client = iris.IrisClient('/dev/fake', open_port=lambda name: port)
        try:
            client.open()
            with patch.object(client, 'rpc', side_effect=iris.Refused('full')), patch.object(iris.time, 'sleep'):
                client.input(struct.pack('<BHH', 1, 1, 1))      # a position: a newer one follows
                with self.assertRaises(ConnectionError):
                    client.input(bytes((6, 1, 1)))
            with self.assertRaises(ConnectionError):
                client.receive()
        finally:
            client.close()

    def test_the_console_port_is_not_taken(self):
        port = FakeDataPort(role=0)
        client = iris.IrisClient('/dev/fake', open_port=lambda name: port)
        with patch.object(client.ready, 'wait', side_effect=lambda t: time.sleep(.2) or client.ready.is_set()):
            with self.assertRaisesRegex(ConnectionError, 'not the board'):
                client.open()
        client.close()


IRIS_INIT = iris.IrisClient.__init__


if __name__ == '__main__':
    unittest.main()
