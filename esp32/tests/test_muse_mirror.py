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

"""Cable framing, codec and host serial regression tests."""
import importlib.util
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)

try:
    import numpy as np
except ImportError:
    raise unittest.SkipTest("numpy is needed for mirror codec fixtures")

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('muse_cable_viewer', ROOT / 'tools/muse/mirror.py')
mirror = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mirror)

@unittest.skipUnless(os.name == 'posix', 'C harnesses require a POSIX sanitizer toolchain')
class MirrorCore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe = Path(cls.tmp.name) / 'mirror-core'
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I' + str(ROOT / 'components/muse'), '-I' + str(ROOT / 'components/muse_mirror'),
            str(ROOT / 'components/muse_mirror/muse_mirror_core.c'),
            str(ROOT / 'tests/muse_mirror_harness.c'), '-o', str(cls.exe)]
        result = subprocess.run(command, capture_output=True, text=True,
                                env={**os.environ, 'TMPDIR': cls.tmp.name})
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @staticmethod
    def update(x, y, pixels):
        h, w = pixels.shape
        return b'U' + struct.pack('<4H', x, y, w, h) + pixels.astype('<u2').tobytes()

    def code(self, source, updates=None, cap=8192):
        h, w = source.shape
        commands = self.update(0, 0, source) if updates is None else updates
        data = struct.pack('<HHI', w, h, cap) + commands + b'SD\x02B\x00'
        result = run_child([self.exe], input=data, capture_output=True, timeout=20,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        fb = np.zeros_like(source)
        frames, codings, off = [], set(), 0
        while off < len(result.stdout):
            n, = struct.unpack_from('<I', result.stdout, off)
            msg = result.stdout[off + 4:off + 4 + n]
            self.assertEqual(len(msg), n)
            self.assertLessEqual(n, cap)
            state = mirror.apply_frame(fb, msg)
            self.assertIn(state, (0, 2))
            end = 8
            count, = struct.unpack_from('<H', msg, 2)
            for _ in range(count):
                codings.add(msg[end + 8])
                payload, = struct.unpack_from('<I', msg, end + 10)
                end += 14 + payload
            self.assertEqual(end, n)
            frames.append(msg)
            off += 4 + n
        self.assertEqual(frames[-1], struct.pack('<BBHHH', ord('F'), 0, 0, w, h))
        np.testing.assert_array_equal(fb, source)
        return frames[:-1], codings

    def test_import_needs_numpy_only(self):
        subprocess.run([sys.executable, '-c',
                        'import sys; sys.path.insert(0, sys.argv[1]); import mirror; '
                        'assert not any(n.startswith(("PySide6", "zeroconf")) for n in sys.modules)',
                        str(ROOT / 'tools/muse')], check=True)

    def test_solid_and_horizontal_bars(self):
        for source in (np.full((128, 200), 0xf81f, dtype=np.uint16),
                       np.repeat(np.arange(128, dtype=np.uint16)[:, None], 200, axis=1)):
            frames, codings = self.code(source)
            self.assertEqual(len(frames), 1)
            self.assertEqual(codings, {1})

    def test_vertical_bars(self):
        source = np.tile(np.arange(200, dtype=np.uint16), (128, 1))
        frames, codings = self.code(source)
        self.assertEqual(len(frames), 1)
        self.assertEqual(codings, {2})

    def test_noise_larger_than_message(self):
        rng = np.random.default_rng(37)
        for w, h in ((466, 466), (172, 640), (412, 412), (135, 240), (320, 480), (800, 480)):
            for cap in (8192, 32768, 65536):
                with self.subTest(w=w, h=h, cap=cap):
                    source = rng.integers(0, 65536, (h, w), dtype=np.uint16)
                    frames, codings = self.code(source, cap=cap)
                    self.assertEqual(len(frames) > 1, source.nbytes + 22 > cap)
                    self.assertEqual(codings, {0})

    def test_random_screens(self):
        rng = np.random.default_rng(9)
        for _ in range(30):
            w, h = rng.integers(1, 300, 2)
            source = rng.integers(0, 65536, (h, w), dtype=np.uint16)
            self.code(source)

    def test_dirty_overflow_overlap_and_clipping(self):
        rng = np.random.default_rng(83)
        source = np.zeros((250, 250), dtype=np.uint16)
        commands = bytearray()
        for y in range(0, 250, 50):
            for x in range(0, 250, 50):
                pixels = rng.integers(0, 65536, (12, 13), dtype=np.uint16)
                source[y:y + 12, x:x + 13] = pixels
                commands += self.update(x, y, pixels)
        pixels = np.full((100, 110), 0x07e0, dtype=np.uint16)
        source[7:107, 9:119] = pixels
        commands += self.update(9, 7, pixels)
        self.code(source, commands)
        self.code(source, commands + b'A')

    def test_tail_with_continuous_updates(self):
        rng = np.random.default_rng(42)
        source = rng.integers(0, 65536, (466, 466), dtype=np.uint16)
        commands = self.update(0, 0, source)
        for _ in range(70):
            commands += b'B\x00'
            pixels = rng.integers(0, 65536, (10, 466), dtype=np.uint16)
            source[:10] = pixels
            commands += self.update(0, 0, pixels)
        frames, _ = self.code(source, commands)
        # The initial bottom must arrive despite new dirt at the top every frame.
        bottom_frames = []
        for i, msg in enumerate(frames):
            off = 8
            for _ in range(struct.unpack_from('<H', msg, 2)[0]):
                _x, y, _w, h, _enc, _pad, n = struct.unpack_from('<HHHHBBI', msg, off)
                if y + h == 466:
                    bottom_frames.append(i)
                off += 14 + n
        self.assertLess(bottom_frames[0], 60)

    def test_single_pixel_and_one_row_capacity(self):
        self.code(np.array([[0xffff]], dtype=np.uint16), cap=64)
        self.code(np.arange(300, dtype=np.uint16).reshape(3, 100), cap=222)


    def test_extended_random_rectangles_and_tails(self):
        rng = np.random.default_rng(512)
        source = np.zeros((81, 101), np.uint16)
        commands = bytearray()
        for i in range(45):
            x, y = int(rng.integers(0, 90)), int(rng.integers(0, 70))
            w, h = int(rng.integers(1, 102 - x)), int(rng.integers(1, 82 - y))
            pixels = rng.integers(0, 65536, (h, w), dtype=np.uint16)
            if i % 3 == 0:
                pixels &= 0x0821
            source[y:y + h, x:x + w] = pixels
            commands += self.update(x, y, pixels)
            commands += b'B\x00'
        self.code(source, commands, cap=256)

    def test_run_counters_past_65535(self):
        self.code(np.arange(65536, dtype=np.uint16).reshape(256, 256), cap=200000)
        for colour in (0, 0xffff):
            _, encs = self.code(np.full((256, 256), colour, np.uint16), cap=200000)
            self.assertEqual(encs, {1})

    def test_last_candidate_small_message_remainders(self):
        rng = np.random.default_rng(740)
        source = np.zeros((24, 24), np.uint16)
        commands = bytearray()
        for x in (0, 9, 18):
            pixels = rng.choice(np.array([0, 0xf800, 0x07e0, 0x001f], np.uint16), (24, 6))
            source[:, x:x + 6] = pixels
            commands += self.update(x, 0, pixels)
        for capacity in range(70, 129):
            with self.subTest(capacity=capacity):
                self.code(source, commands, cap=capacity)

class DecoderTests(unittest.TestCase):
    @staticmethod
    def frame(enc, payload, w=3, h=2):
        return (struct.pack('<BBHHH', ord('F'), 0, 1, w, h)
                + struct.pack('<4HBBI', 0, 0, w, h, enc, 0, len(payload)) + payload)

    def test_malformed_frames(self):
        good = self.frame(0, bytes(12))
        bad = [good[:n] for n in range(len(good))]
        bad += [good + b'x', b'X' + good[1:], self.frame(5, bytes(12)),
                self.frame(0, bytes(10)), self.frame(1, b'\x00'),
                self.frame(2, struct.pack('<HH', 0, 0)),
                self.frame(1, struct.pack('<HH', 65535, 0)),
                self.frame(2, struct.pack('<HH', 5, 0)),
                self.frame(3, struct.pack('<HH', 6, 0)), self.frame(4, b'\xc5')]
        for offset, value in ((8, 3), (12, 0), (14, 0), (17, 1)):
            corrupt = bytearray(good)
            corrupt[offset] = value
            bad.append(corrupt)
        for msg in bad:
            with self.subTest(msg=msg), self.assertRaises(ValueError):
                mirror.apply_frame(np.zeros((2, 3), np.uint16), msg)

class CableFramingTests(unittest.TestCase):
    def test_crc_chunks_logs_and_embedded_sync(self):
        logs, errors, got = bytearray(), [], []
        demux = mirror.CableDemux(logs.extend, lambda: errors.append(True))
        messages = [b'B{}', b'F' + mirror.cable_frame(b'E') + bytes(range(256)), b'E']
        wire = b'boot\r\n' + b''.join(mirror.cable_frame(m) + b'log line\n' for m in messages)
        for byte in wire:
            got.extend(demux.feed(bytes([byte])))
        self.assertEqual(got, messages)
        self.assertEqual(logs, b'boot\r\n' + b'log line\n' * 3)
        self.assertFalse(errors)
        self.assertEqual(mirror.cable_frame(b'R').hex(), 'a55a010000005275ee277e')

    def test_corrupt_frame_and_bad_length_recover(self):
        logs, errors = bytearray(), []
        demux = mirror.CableDemux(logs.extend, lambda: errors.append(True))
        broken = mirror.cable_frame(b'F' + b'\x80' * 40)
        broken = broken[:20] + b'\nROM interrupted\n' + broken[20:]
        got = []
        for start in range(0, len(broken), 3):
            got.extend(demux.feed(broken[start:start + 3]))
        got.extend(demux.feed(mirror.cable_frame(b'E')))
        self.assertEqual(got, [b'E'])
        self.assertIn(b'ROM interrupted\n', logs)
        self.assertEqual(len(errors), 1)
        self.assertEqual(demux.feed(mirror.SYNC + b'\xff' * 4 + mirror.cable_frame(b'T')), [b'T'])
        self.assertEqual(len(errors), 2)

    def test_incomplete_header_and_frame_expire(self):
        for prefix in (mirror.SYNC, mirror.SYNC + struct.pack('<I', 60000) + b'F'):
            errors = []
            demux = mirror.CableDemux(lambda data: None, lambda: errors.append(True))
            self.assertEqual(demux.feed(prefix, now=10), [])
            self.assertEqual(demux.feed(mirror.cable_frame(b'E'), now=14), [b'E'])
            self.assertEqual(len(errors), 1)
            self.assertLess(len(demux.buffer), 2)

    @unittest.skipUnless(os.name == 'posix', 'C harness uses pthreads and sanitizers')
    def test_firmware_serial_send_and_log_lock(self):
        source = (ROOT / 'components/muse_mirror/muse_mirror_session.c').read_text()
        send = source[source.index('#define SERIAL_CHUNK'):source.index('static bool viewer_send', source.index('#define SERIAL_CHUNK'))]
        log = ('static char s_log_buf[LOG_LINE], *s_log_line = s_log_buf;\n'
               + source[source.index('static atomic_uint s_log_dropped'):source.index('static bool serial_enqueue')])
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'serial_impl.inc').write_text(send + log)
            exe = folder / 'serial-harness'
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-I' + str(folder),
                str(ROOT / 'tests/muse_mirror_serial_harness.c'), '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True, env={**os.environ, 'TMPDIR': tempfile.gettempdir()})
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([exe], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            logs, errors = bytearray(), []
            demux = mirror.CableDemux(logs.extend, lambda: errors.append(True))
            messages = demux.feed(result.stdout)
            self.assertEqual(messages, [bytes(i % 256 for i in range(3000))] * 100)
            self.assertFalse(errors)
            self.assertEqual(len(logs.splitlines()), 100)
            for i, line in enumerate(logs.splitlines()):
                self.assertEqual(line.decode(), 'log %d %0600d' % (i, i))

    @unittest.skipUnless(os.name == 'posix', 'C harness requires a POSIX sanitizer toolchain')
    def test_firmware_session_pump(self):
        source = (ROOT / 'components/muse_mirror/muse_mirror_session.c').read_text()
        impl = source[source.index('static void viewer_release(void)\n'):source.index('static atomic_uint s_log_dropped')]
        start = source.index('static bool serial_enqueue')
        impl += source[start:source.index('#define ZIP_MIN', start)]
        declarations = source[source.index('typedef struct { uint16_t len; uint8_t b[IN_MAX]; } serial_msg_t;'):source.index('static QueueHandle_t s_serial_in;')]
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            (folder / 'session_impl.inc').write_text(impl)
            (folder / 'session_types.inc').write_text(declarations)
            exe = folder / 'session-harness'
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-I' + str(folder), '-I' + str(ROOT / 'components/muse_mirror'),
                str(ROOT / 'tests/muse_mirror_session_harness.c'), '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True, env={**os.environ, 'TMPDIR': tempfile.gettempdir()})
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([exe], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_deflated_message_bounds(self):
        def packed(body, level=6):
            z = zlib.compressobj(level, zlib.DEFLATED, -15)
            return b'Z' + z.compress(body) + z.flush()
        self.assertEqual(mirror.inflate_message(packed(b'F' + bytes(65535))), b'F' + bytes(65535))
        for bad in (packed(b'F' + bytes(65536)), packed(b''), packed(b'F123')[:-2], packed(b'F123') + b'x',
                    b'Z', b'Z\xff\xff'):
            with self.assertRaises(ConnectionError):
                mirror.inflate_message(bad)

    def test_windows_port_names(self):
        for port in ('COM1', 'com12', r'\\.\COM32'):
            self.assertTrue(mirror.is_port(port), port)
        self.assertFalse(mirror.is_port('example:7331'))

    def test_log_lines_stdout_and_file(self):
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'console.log'
            output = io.StringIO()
            logger = mirror.ConsoleLog('COM12', path)
            with contextlib.redirect_stdout(output):
                logger.feed(b'hel')
                logger.feed(b'lo\r\nnext')
                logger.flush()
            self.assertEqual(output.getvalue(), path.read_text())
            lines = output.getvalue().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertRegex(lines[0], r'^\[\d{4}-\d\d-\d\d .* COM12\] hello$')
            self.assertTrue(lines[1].endswith('COM12] next'))

class SerialTests(unittest.TestCase):
    @unittest.skipUnless(os.name == 'posix' and importlib.util.find_spec('serial'), 'PTY test needs POSIX and pyserial')
    def test_viewer_over_a_console_pty(self):
        import json
        import pty
        import select
        import serial
        import threading
        import time
        from unittest.mock import patch

        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        incoming = bytearray()

        def read(n):
            until = time.monotonic() + 4
            while len(incoming) < n and time.monotonic() < until:
                if select.select([master], [], [], .1)[0]:
                    incoming.extend(os.read(master, 4096))
            self.assertGreaterEqual(len(incoming), n, bytes(incoming))
            result = bytes(incoming[:n])
            del incoming[:n]
            return result

        def message():
            n, = struct.unpack('<I', read(4))
            return read(n)

        def send(data):
            for offset in range(0, len(data), 3):
                os.write(master, data[offset:offset + 3])

        info = dict(name='PTY Muse', w=3, h=2, touch=True, audio=False,
                    buttons=[dict(role=1, name='Talk', x=-1, y=-1)])
        board = mirror.cable_frame(b'B' + json.dumps(info).encode())

        def frame_body(pixel):
            return struct.pack('<BBHHH4HBBIHH', ord('F'), 0, 1, 3, 2, 0, 0, 3, 2, 1, 0, 4, 6, pixel)

        def frame(pixel):
            return mirror.cable_frame(frame_body(pixel))

        def deflated(body):
            packed = zlib.compressobj(6, zlib.DEFLATED, -15)
            return mirror.cable_frame(b'Z' + packed.compress(body) + packed.flush())

        # PTYs have no DTR/RTS lines; the separate serial tests check those operations.
        opener = lambda port, baud: serial.Serial(port, baudrate=baud, timeout=.1, write_timeout=1)
        with patch.object(mirror, 'open_usb_serial', opener):
            link = mirror.Link(os.ttyname(slave))
            drawn = threading.Event()
            link.on_frame = drawn.set
            link.start()
            try:
                self.assertEqual(read(13), mirror.BREAK + b'\n>mirror\n')
                send(mirror.MARK + board + frame(0x07e0))
                self.assertTrue(drawn.wait(4))
                with link.lock:
                    self.assertTrue(link.seen.all())
                    np.testing.assert_array_equal(link.fb, np.full((2, 3), 0x07e0))
                self.assertEqual([message(), message()], [b"O", b"Z"])
                drawn.clear()
                # A deflated message is the same message; a frame the board deflated draws as any other.
                send(mirror.cable_frame(b'O\x01') + deflated(frame_body(0xf81f)))
                self.assertTrue(drawn.wait(4))
                self.assertTrue(link.text_open)
                with link.lock:
                    np.testing.assert_array_equal(link.fb, np.full((2, 3), 0xf81f))
                self.assertTrue(mirror.route_text_key(link, None, paste='é' * 40))
                self.assertEqual([message(), message()], [b'I' + ('é' * 31).encode(), b'I' + ('é' * 9).encode()])
                self.assertTrue(mirror.route_text_key(link, 'Enter'))
                self.assertEqual(message(), b'K\x05')
                drawn.clear()
                send(mirror.cable_frame(b'O\x00') + frame(0x07e0))
                self.assertTrue(drawn.wait(4))
                self.assertFalse(link.text_open)
                self.assertFalse(mirror.route_text_key(link, None, '1'))
                link.key(1, True)
                link.pointer(2, 1, True)
                link.pointer(2, 1, False)
                link.key(1, False)
                self.assertEqual([message() for _ in range(4)],
                                 [b'\x06\x01\x01', b'\x01\x02\x00\x01\x00',
                                  b'\x00\x02\x00\x01\x00', b'\x06\x01\x00'])
                bad = bytearray(frame(0xffff))
                bad[-1] ^= 1
                drawn.clear()
                send(bad)
                self.assertEqual(message(), b'R')
                send(board + frame(0x001f))
                self.assertEqual([message(), message()], [b"O", b"Z"])
                self.assertTrue(drawn.wait(4))
                with link.lock:
                    np.testing.assert_array_equal(link.fb, np.full((2, 3), 0x001f))
                    self.assertTrue(link.seen.all())
            finally:
                link.close()
            self.assertFalse(link.thread.is_alive())
            self.assertFalse(link.writer_thread.is_alive())
            self.assertEqual(read(4), mirror.BREAK)

    class Serial:
        def __init__(self, **kwargs):
            self.options = kwargs
            self.is_open = False
            self.events = []
            self._dtr = self._rts = True
            self.port = kwargs['port']
            self.timeout = kwargs['timeout']
            self.reads = bytearray(b'boot log\n')
            self.error = None
            self.platform = 'linux'
            self.driver_states = []

        @property
        def dtr(self):
            return self._dtr

        @dtr.setter
        def dtr(self, value):
            self._dtr = value
            self.events.append(('dtr', value, self.is_open))
            if self.is_open:
                self.driver_states.append((value, self._rts))

        @property
        def rts(self):
            return self._rts

        @rts.setter
        def rts(self, value):
            self._rts = value
            self.events.append(('rts', value, self.is_open))
            if self.is_open:
                self.driver_states.append((self._dtr, value))

        def open(self):
            self.events.append(('open', self.dtr, self.rts))
            if self.error:
                raise self.error
            self.is_open = True
            # POSIX DTR first, with an inherited asserted RTS; Windows DCB together.
            if self.platform != 'win32':
                self.driver_states.append((self.dtr, True))
            self.driver_states.append((self.dtr, self.rts))

        def close(self):
            self.events.append(('close',))
            self.is_open = False

        def write(self, data):
            self.events.append(('write', data))
            if data == b'\n>mirror\n':
                self.reads += mirror.MARK
            return len(data)

        @property
        def in_waiting(self):
            return len(self.reads)

        def read(self, size):
            result = bytes(self.reads[:size])
            del self.reads[:size]
            return result

        def reset_input_buffer(self):
            self.events.append(('reset_input_buffer',))

    def install(self, platform='linux', error=None):
        from types import SimpleNamespace
        from unittest.mock import patch
        instances = []

        def create(**options):
            ser = self.Serial(**options)
            ser.platform, ser.error = platform, error
            instances.append(ser)
            return ser

        self.enterContext(patch.dict(sys.modules, {'serial': SimpleNamespace(Serial=create)}))
        self.enterContext(patch.object(mirror.sys, 'platform', platform))
        self.enterContext(patch.object(mirror.time, 'sleep'))
        return instances

    def test_open_line_order_and_existing_handshake(self):
        for platform, port in (('linux', '/dev/ttyACM0'), ('darwin', '/dev/cu.usbmodem101'), ('win32', 'COM12')):
            with self.subTest(platform=platform):
                instances = self.install(platform)
                ser = mirror.open_serial(port)
                self.assertIs(ser, instances[0])
                self.assertIsNone(ser.options['port'])
                self.assertTrue(ser.options['exclusive'])
                for name in ('rtscts', 'dsrdtr', 'xonxoff'):
                    self.assertFalse(ser.options[name])
                self.assertEqual(ser.events[:3], [('dtr', True, False), ('rts', False, False), ('open', True, False)])
                self.assertNotIn((False, True), ser.driver_states)
                self.assertEqual(ser.driver_states[-1], (False, False))
                self.assertEqual([e for e in ser.events if e[0] == 'write'],
                                 [('write', mirror.BREAK), ('write', b'\n>mirror\n')])
                self.assertEqual(ser.timeout, .1)
                ser.close()

    def test_actionable_open_errors(self):
        import errno
        for platform, error, expected in (
                ('linux', PermissionError(errno.EACCES, 'denied'), 'dialout'),
                ('linux', PermissionError(errno.EPERM, 'denied'), 'uucp'),
                ('linux', OSError(errno.EBUSY, 'busy'), 'idf.py monitor'),
                ('linux', OSError(errno.EAGAIN, 'Could not exclusively lock port'), 'another viewer'),
                ('linux', FileNotFoundError(errno.ENOENT, 'gone'), 'Reconnect'),
                ('win32', OSError("could not open port 'COM12': PermissionError(13, 'Access is denied.')"), 'idf.py monitor')):
            with self.subTest(platform=platform, error=error):
                instances = self.install(platform, error)
                with self.assertRaisesRegex(ConnectionError, expected):
                    mirror.open_usb_serial('COM12' if platform == 'win32' else '/dev/ttyACM0')
                self.assertEqual(instances[0].events[-1], ('close',))

    def test_missing_marker_closes_port(self):
        from unittest.mock import patch
        instances = self.install()
        with patch.object(mirror.time, 'monotonic', side_effect=[0, 6]):
            with self.assertRaisesRegex(ConnectionError, 'CONFIG_MUSE_MIRROR=y'):
                mirror.open_serial('/dev/ttyACM0', wait=5)
        self.assertFalse(instances[0].is_open)

    def test_missing_pyserial(self):
        from unittest.mock import patch
        with patch.dict(sys.modules, {'serial': None}):
            with self.assertRaisesRegex(ConnectionError, 'pip install pyserial'):
                mirror.open_usb_serial('COM12')

    def test_mac_normalization(self):
        for serial in ('aabbccddeeff', 'AA:BB:CC:DD:EE:FF', 'AA_BB_CC_DD_EE_FF', 'AA-BB-CC-DD-EE-FF'):
            self.assertEqual(mirror.usb_serial_mac(serial), 'aa:bb:cc:dd:ee:ff')
        for serial in (None, '', '6&ABCDEF&0&0000', 'AABBCCDDEEFF&0', 'USB\\VID_303A&PID_1001', 'AA:BB_CC:DD:EE:FF'):
            self.assertEqual(mirror.usb_serial_mac(serial), '')



    def test_hupcl_cleared_before_handshake_and_close(self):
        from types import SimpleNamespace
        from unittest.mock import Mock, patch
        instances = self.install()
        attrs = [11, 22, 0x471, 44, 55, 66, []]
        termios = SimpleNamespace(HUPCL=0x400, TCSANOW=0, tcgetattr=Mock(return_value=attrs), tcsetattr=Mock())
        with patch.object(mirror.os, 'name', 'posix'), patch.dict(sys.modules, {'termios': termios}), \
                patch.object(self.Serial, 'fileno', lambda self: 17, create=True):
            with patch.object(mirror.time, 'monotonic', side_effect=[0, 6]):
                with self.assertRaises(ConnectionError):
                    mirror.open_serial('/dev/ttyACM0')
        termios.tcgetattr.assert_called_once_with(17)
        termios.tcsetattr.assert_called_once_with(17, 0, [11, 22, 0x71, 44, 55, 66, []])
        self.assertFalse(instances[0].is_open)

    def test_hupcl_unavailable_or_unsupported(self):
        from types import SimpleNamespace
        from unittest.mock import Mock, patch
        for termios in (None, SimpleNamespace(tcgetattr=Mock(side_effect=OSError('unsupported')))):
            with patch.object(mirror.os, 'name', 'posix'), patch.dict(sys.modules, {'termios': termios}):
                mirror.keep_serial_lines_on_close(SimpleNamespace(fileno=lambda: 17))

    def test_hupcl_not_used_on_windows(self):
        from unittest.mock import Mock, patch
        ser = Mock()
        with patch.object(mirror.os, 'name', 'nt'):
            mirror.keep_serial_lines_on_close(ser)
        ser.fileno.assert_not_called()
