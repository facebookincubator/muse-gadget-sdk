# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Byte-level Python/C matte conformance on synthetic inputs only."""
import ctypes
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT.parent))
try:
    from PIL import Image,ImageDraw
    from avatar_pixel.matte import matte_uniform_white
except ImportError:
    matte_uniform_white=None


class Stats(ctypes.Structure):
    _fields_=[('applied',ctypes.c_bool),('removed',ctypes.c_uint),('softened',ctypes.c_uint)]


@unittest.skipIf(matte_uniform_white is None,'Pillow and avatar_pixel are required')
class MatteConformance(unittest.TestCase):
    def test_python_and_c_share_mask_rounding_and_foreground_bytes(self):
        fixtures=[]
        white=Image.new('RGBA',(16,16),(255,255,255,255));fixtures.append(white)
        outlined=white.copy();draw=ImageDraw.Draw(outlined);draw.rectangle((4,4,11,11),outline=(0,0,0,255));fixtures.append(outlined)
        colored=outlined.copy();colored.putpixel((0,0),(0,0,255,255));fixtures.append(colored)
        soft=white.copy();soft.putpixel((6,6),(0,0,0,255));soft.putpixel((5,6),(231,231,231,255));fixtures.append(soft)
        thin=Image.new('RGBA',(1,200),(255,255,255,255));thin.putpixel((0,100),(0,0,0,255));fixtures.append(thin)
        try:
            with tempfile.TemporaryDirectory() as tmp:
                library=Path(tmp)/'matte.so'
                subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-O2','-Wall','-Wextra','-Werror',
                                '-shared','-fPIC',str(ROOT/'components/muse/muse_avatar_matte.c'),'-o',str(library)],check=True)
                lib=ctypes.CDLL(str(library))
                lib.muse_avatar_white_mask.argtypes=[ctypes.POINTER(ctypes.c_uint16),ctypes.c_int,ctypes.c_int,
                    ctypes.POINTER(ctypes.c_uint8),ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t,ctypes.POINTER(Stats)]
                lib.muse_avatar_white_mask.restype=ctypes.c_bool
                lib.muse_avatar_matte_decontaminate.argtypes=[ctypes.POINTER(ctypes.c_uint16),ctypes.POINTER(ctypes.c_uint8),ctypes.c_size_t]
                for i,image in enumerate(fixtures):
                    with self.subTest(fixture=i):
                        w,h=image.size;n=w*h
                        original=list(image.get_flattened_data())
                        words=[((r>>3)<<11)|((g>>2)<<5)|(b>>3) for r,g,b,a in original]
                        pixels=(ctypes.c_uint16*n)(*words);alpha=(ctypes.c_uint8*n)();queue=(ctypes.c_uint32*n)();stats=Stats()
                        self.assertTrue(lib.muse_avatar_white_mask(pixels,w,h,alpha,queue,n,ctypes.byref(stats)))
                        lib.muse_avatar_matte_decontaminate(pixels,alpha,n)
                        result,expected=matte_uniform_white(image)
                        try:
                            rgba=list(result.get_flattened_data())
                            self.assertEqual(list(alpha),[a for r,g,b,a in rgba])
                            self.assertEqual(list(pixels),[((r>>3)<<11)|((g>>2)<<5)|(b>>3) for r,g,b,a in rgba])
                            self.assertEqual(stats.applied,expected['applied'])
                            self.assertEqual(stats.removed,expected['removed'])
                            self.assertEqual(stats.softened,expected['softened'])
                        finally:result.close()
        finally:
            for image in fixtures:image.close()


if __name__=='__main__':
    unittest.main()
