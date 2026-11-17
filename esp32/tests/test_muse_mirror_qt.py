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

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(importlib.util.find_spec('PySide6') and importlib.util.find_spec('numpy'),
                     'PySide6 and numpy are needed for the offscreen viewer test')
class ViewerTests(unittest.TestCase):
    def test_profiles_states_keys_and_capture(self):
        script = r'''
import os
import tempfile
from pathlib import Path
from unittest.mock import patch
import numpy as np
from PySide6.QtCore import QTimer, QEvent, Qt
from PySide6.QtGui import QInputMethodEvent, QKeyEvent
from PySide6.QtWidgets import QApplication
import mirror

app = QApplication([])
errors = []
completed = []

def check():
    win = next(w for w in app.topLevelWidgets() if hasattr(w, 'render_devices'))
    try:
        scale = float(os.environ['QT_SCALE_FACTOR'])
        assert win.devicePixelRatioF() == scale
        names = ['waveshare-s3-216', 'waveshare-s3-154g', 'waveshare-s3-349',
                 'waveshare-p4-35', 'waveshare-s3-175c', 'aipi-lite']
        for i, name in enumerate(names):
            sizes = [(480, 480), (200, 200), (172, 640), (480, 320), (466, 466), (128, 128)]
            names = ['Waveshare 2.16', 'Waveshare 1.54G', 'Waveshare 3.49', 'Waveshare P4 3.5', 'Waveshare 1.75C', 'AIPI Lite']
            w, h = sizes[i]
            info = dict(name=names[i], w=w, h=h, round=i == 4, touch=True, buttons=[dict(role=1, name='Talk', x=-1, y=-1)])
            d = win.devs[i]
            d.link.live = True
            d.link.fb = np.full((info['h'], info['w']), 0xf800, np.uint16)
            win.on_board(i, info)
            win.on_frame(i)
            win.arrange()
            assert (d.L["active_r"] > 0) == info["round"]
            # A known device shows what is printed by the key, another the board's own word for it.
            # The 3.49's and the P4's RST is drawn but no board button reaches it.
            printed = {0: ['KEY3', 'PWR'], 2: ['BOOT', 'RST'], 3: ['BOOT', 'RST'], 4: ['PWR']}.get(i, ['Talk'])
            assert sorted(k["label"] for k in d.L["keys"]) == printed, (names[i], d.L["keys"])
            assert {k["role"] for k in d.L["keys"]} - {None} == {1}   # the 2.16's PWR talks, as its KEY3 does
            role = info['buttons'][0]['role']
            win.press(d, role, True)
            win.press(d, role, False)
            first = d.link.out.get_nowait()
            if d.look['profile'].get('hold', True):
                assert first == bytes([6, role, 1])
                assert d.link.out.get_nowait() == bytes([6, role, 0])
            else:
                assert first == bytes([3, role])
        # One physical key that the board names in two roles is one key on the drawing, where its hint is.
        d = win.devs[5]
        win.on_board(5, dict(name='Seeed SenseCAP Watcher', w=412, h=412, round=True, touch=True, buttons=[
            dict(role=1, name='wheel', x=306, y=63), dict(role=2, name='wheel', x=-1, y=-1)]))
        win.arrange()
        assert [(k["label"], k["role"]) for k in d.L["keys"]] == [('wheel', 1)], d.L["keys"]
        # Its picture: the screen in the picture's hole, the wheel where the picture has it.
        assert d.L["photo"] is not None and d.L["keys"][0].get("pictured")
        bx, by, bw, bh = d.L["body"]
        ax, ay, aw, ah = d.L["active"]
        assert abs((ax + aw / 2 - bx) / bw - 364.5 / 727) < .01 and abs((ay + ah / 2 - by) / bh - 421.5 / 784) < .01
        assert abs(aw / bw - 409 / 727) < .01 and d.L["keys"][0]["cx"] > ax + aw * .8 and d.L["keys"][0]["cy"] < ay
        cx, cy = d.L["corners"][(1, 1)]                  # the corner that resizes is where the picture rounds off
        assert abs((cx - bx) / bw - 616.5 / 727) < .01 and abs((cy - by) / bh - 673.5 / 784) < .01
        d = win.devs[0]
        win.device_under_mouse = lambda: d
        def key(kind, code, text='', mods=Qt.KeyboardModifier.NoModifier):
            event = QKeyEvent(kind, code, mods, text)
            (win.keyPressEvent if kind == QEvent.Type.KeyPress else win.keyReleaseEvent)(event)
        key(QEvent.Type.KeyPress, Qt.Key.Key_1, '1')
        assert d.link.out.get_nowait() == bytes([6, 1, 1])
        d.link.text_open = True
        key(QEvent.Type.KeyRelease, Qt.Key.Key_1, '1')
        assert d.link.out.get_nowait() == bytes([6, 1, 0])
        key(QEvent.Type.KeyPress, Qt.Key.Key_1, '1')
        key(QEvent.Type.KeyRelease, Qt.Key.Key_1, '1')
        assert d.link.out.get_nowait() == b'I1'
        for code, edit in ((Qt.Key.Key_Left, 3), (Qt.Key.Key_Backspace, 1), (Qt.Key.Key_Return, 5)):
            key(QEvent.Type.KeyPress, code)
            key(QEvent.Type.KeyRelease, code)
            assert d.link.out.get_nowait() == bytes([ord('K'), edit])
        ime = QInputMethodEvent()
        ime.setCommitString('日本語')
        win.inputMethodEvent(ime)
        assert d.link.out.get_nowait() == 'I日本語'.encode()   # what an input method composed
        key(QEvent.Type.KeyPress, Qt.Key.Key_Escape)
        assert d.link.out.empty() and not win.held
        d.link.text_open = False
        key(QEvent.Type.KeyPress, Qt.Key.Key_1, '1')
        win.let_go()
        assert d.link.out.get_nowait() == bytes([6, 1, 1])
        assert d.link.out.get_nowait() == bytes([6, 1, 0])
        with tempfile.TemporaryDirectory() as folder:
            renders = []
            for state in range(3):
                for d in win.devs:
                    d.link.state = state
                shot = win.render_devices()
                assert not shot.isNull()
                assert shot.devicePixelRatio() == scale
                assert shot.save(str(Path(folder) / ('state%d.png' % state)))
                renders.append(bytes(shot.constBits()))
            assert renders[0] != renders[1] != renders[2]
            win.grab()
        completed.append(True)
    except BaseException as error:
        errors.append(error)
    finally:
        win.close()
        app.quit()

QTimer.singleShot(100, check)
QTimer.singleShot(10000, app.quit)
with patch.object(mirror.Link, 'start', lambda self: self):
    mirror.run_window([('test%d' % i, 115200) for i in range(6)], 1, False, True)
if errors:
    raise errors[0]
assert completed, "viewer callback did not run"
'''
        for scale in ('1', '1.5', '2'):
            with self.subTest(scale=scale):
                result = subprocess.run([sys.executable, '-c', script], cwd=ROOT / 'tools/muse',
                                        env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen', 'QT_SCALE_FACTOR': scale},
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
