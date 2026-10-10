# Copyright (c) 2026 ComBba
# SPDX-License-Identifier: Apache-2.0
"""Incomplete bench transfers must not create or overwrite a screenshot."""
import base64
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/muse/snap.py'
spec = importlib.util.spec_from_file_location('muse_snap', SCRIPT)
snap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snap)


def packet(raw, w=466, h=466, per_line=144, legacy=False):
    header = f'SNAP BEGIN {w} {h}' + ('' if legacy else f' {per_line}')
    lines = [header]
    for y in range(h):
        row = raw[y * w * 2:(y + 1) * w * 2]
        lines.extend(base64.b64encode(row[x:x + per_line]).decode()
                     for x in range(0, len(row), per_line))
    return '\n'.join(lines) + '\nSNAP END\n'


class SnapshotDecoder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw = bytes(range(256)) * (466 * 466 * 2 // 256) + bytes(range(466 * 466 * 2 % 256))
        cls.transfer = packet(cls.raw)

    def test_complete_round_frame(self):
        self.assertEqual(snap.decode_snapshot(self.transfer), (466, 466, self.raw))

    def test_legacy_header(self):
        self.assertEqual(snap.decode_snapshot(packet(b'\x00\xf8', 1, 1, legacy=True)),
                         (1, 1, b'\x00\xf8'))

    def test_uart_log_prefix_and_crlf(self):
        lines = self.transfer.splitlines()
        lines[1] = 'I (100) codec: initialized ' + lines[1]
        lines.insert(2, 'I (101) codec: started')
        self.assertEqual(snap.decode_snapshot('\r\n'.join(lines) + '\r\n')[2], self.raw)

    def test_missing_pixel_line(self):
        for index in (1, 100, len(self.transfer.splitlines()) - 2):
            with self.subTest(index=index), self.assertRaises(ValueError):
                lines = self.transfer.splitlines()
                del lines[index]
                snap.decode_snapshot('\n'.join(lines) + '\n')

    def test_missing_end(self):
        with self.assertRaises(ValueError):
            snap.decode_snapshot(self.transfer.replace('SNAP END\n', ''))

    def test_wrong_decoded_chunk_size(self):
        with self.assertRaises(ValueError):
            snap.decode_snapshot('SNAP BEGIN 1 1 144\nAAAA\nSNAP END\n')

    def test_extra_pixels(self):
        with self.assertRaises(ValueError):
            snap.decode_snapshot(packet(b'\x00\xf8', 1, 1).replace('SNAP END', 'APg=\nSNAP END'))

    def test_zero_chunk_size(self):
        with self.assertRaises(ValueError):
            snap.decode_snapshot('SNAP BEGIN 1 1 0\nAPg=\nSNAP END\n')


class SnapshotCLI(unittest.TestCase):
    def run_capture(self, transfer, existing=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'serial.py').write_text('''class Serial:
    def __init__(self, *args, **kwargs):
        self.ready = False
    def write(self, data):
        if data == b'p':
            self.ready = True
    def read(self, size):
        if self.ready:
            self.ready = False
            return TRANSFER
        return b''
''' + f'TRANSFER = {transfer.encode()!r}\n')
            output = root / 'screen.png'
            if existing is not None:
                output.write_bytes(existing)
            env = dict(os.environ, PYTHONPATH=str(root))
            result = subprocess.run([sys.executable, str(SCRIPT), 'fake-port', '', str(output)],
                                    env=env, capture_output=True, timeout=5)
            return result, output.read_bytes() if output.exists() else None

    def test_missing_pixels_creates_no_output(self):
        result, output = self.run_capture('SNAP BEGIN 2 1 144\nSNAP END\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'missing pixel bytes', result.stderr)
        self.assertIsNone(output)

    def test_failed_capture_preserves_existing_file(self):
        result, output = self.run_capture('SNAP BEGIN 2 1 144\nSNAP END\n', b'previous capture')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output, b'previous capture')

    def test_complete_capture_writes_png(self):
        result, output = self.run_capture(packet(b'\x00\xf8', 1, 1))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.startswith(b'\x89PNG\r\n\x1a\n'))


if __name__ == '__main__':
    unittest.main()
