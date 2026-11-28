# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""USB A8 parser/installer tests with a fake board; no device or network."""
import base64
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[1]


class FakeBoard:
    def __init__(self,alpha=False,alpha_value=None,alpha_size=None,crc_bad=False,
                 extra=False,changed=False,invalidated=False,alpha_export=None,snapshot_alpha=None):
        self.alpha=alpha;self.alpha_value=alpha_value;self.alpha_size=alpha_size
        self.alpha_export=alpha_export;self.snapshot_alpha=snapshot_alpha
        self.crc_bad=crc_bad;self.extra=extra;self.changed=changed;self.invalidated=invalidated
        self.lines=[];self.commands=[]
        self.key='23'*32
        self.rgb=struct.pack('<4H',0xffff,0xf800,0,0x07e0)
        self.mask=bytes((0,128,255,255))

    def write_line(self,line):
        self.commands.append(line)
        if line=='avatar':
            value=self.alpha if self.alpha_value is None else self.alpha_value
            metadata={'alpha':value}
            if self.alpha_export is not None:metadata['alpha_export']=self.alpha_export
            self.lines.append('@avatar '+json.dumps(metadata))
        elif line in ('pixel.image','pixel.image_alpha'):
            if self.invalidated:
                self.lines.append('@pixel_export_error context_changed');return
            available=self.alpha if self.snapshot_alpha is None else self.snapshot_alpha
            a=self.mask if available and line=='pixel.image_alpha' else b''
            body=self.rgb+a
            meta={'width':2,'height':2,'bytes':len(body),'crc':zlib.crc32(body)+(1 if self.crc_bad else 0),'source_key':self.key}
            if available or self.alpha_size is not None:meta['alpha_bytes']=len(a) if self.alpha_size is None else self.alpha_size
            if self.snapshot_alpha is not None or self.alpha_export is not None:meta['alpha_available']=available
            self.lines.append('@pixel_image '+json.dumps(meta))
            self.lines.append('@pixel_bytes '+base64.b64encode(body+(b'x' if self.extra else b'')).decode())
            self.lines.append('@pixel_end')
        elif line=='pixel.assets':
            self.lines.append('@pixel_assets '+json.dumps({'source_key':'24'*32 if self.changed else self.key}))
        elif line.startswith(('pixel.begin=','pixel.chunk=')) or line in ('pixel.commit','pixel.abort'):
            self.lines.append('@pixel_install ok')
        elif line in ('pixel.on','pixel.off'):
            self.lines.append('@pixel_setting ok')
        elif line=='pixel':
            self.lines.append('@pixel '+json.dumps({'enabled':False,'matching':False}))
        else:raise AssertionError('Unexpected fake command: '+line)

    def read_line(self,deadline):
        del deadline
        if not self.lines:raise AssertionError('Unexpected empty fake response')
        return self.lines.pop(0)

    def status(self,timeout=5):
        del timeout
        return {'device':{'link':{'paired':True}}}

    def __enter__(self):return self
    def __exit__(self,*args):return False


class PixelAlphaExport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        chat=types.ModuleType('chat');chat.Board=lambda port:None;chat.BoardError=RuntimeError
        old=sys.modules.get('chat');sys.modules['chat']=chat
        try:
            spec=importlib.util.spec_from_file_location('tested_pixel_avatar',ROOT/'tools/muse/pixel_avatar.py')
            cls.helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.helper)
        finally:
            if old is None:sys.modules.pop('chat',None)
            else:sys.modules['chat']=old

    def test_legacy_rgb_export_still_works(self):
        from PIL import Image
        b=FakeBoard();encoded,key=self.helper.read_image(b)
        with Image.open(io.BytesIO(encoded)) as image:
            self.assertEqual(image.mode,'RGB');self.assertEqual(image.size,(2,2))
            self.assertEqual(image.getpixel((0,0)),(255,255,255))
            self.assertEqual(image.getpixel((0,1)),(0,0,0))
        self.assertEqual(key,b.key)
        self.assertEqual(b.commands,['avatar','pixel.image','pixel.assets'])

    def test_alpha_export_is_preserved_in_png(self):
        from PIL import Image
        b=FakeBoard(alpha=True);encoded,key=self.helper.read_image(b)
        with Image.open(io.BytesIO(encoded)) as image:
            self.assertEqual(image.mode,'RGBA')
            self.assertEqual(list(image.getchannel('A').get_flattened_data()),[0,128,255,255])
            self.assertEqual(image.getpixel((0,1)),(0,0,0,255))
        self.assertEqual(key,b.key)
        self.assertEqual(b.commands,['avatar','pixel.image_alpha','pixel.assets'])

    def test_capability_handles_false_to_true_matte_recovery(self):
        from PIL import Image
        b=FakeBoard(alpha=False,alpha_export=True,snapshot_alpha=True)
        encoded,key=self.helper.read_image(b)
        with Image.open(io.BytesIO(encoded)) as image:
            self.assertEqual(image.mode,'RGBA')
            self.assertEqual(image.getpixel((0,0))[3],0)
        self.assertEqual(key,b.key)
        self.assertIn('pixel.image_alpha',b.commands)

    def test_outdated_legacy_opaque_choice_rejects_pinned_alpha(self):
        from PIL import Image
        b=FakeBoard(alpha=False,snapshot_alpha=True)
        with self.assertRaisesRegex(ValueError,'pinned export has alpha'):
            self.helper.read_image(b)
        self.assertFalse(b.lines)  # Drain the bounded legacy response before retry.
        b.alpha=True
        encoded,key=self.helper.read_image(b)
        with Image.open(io.BytesIO(encoded)) as image:self.assertEqual(image.mode,'RGBA')
        self.assertEqual(key,b.key)

    def test_actual_snapshot_can_have_no_alpha_after_observed_alpha(self):
        from PIL import Image
        b=FakeBoard(alpha=True,alpha_export=True,snapshot_alpha=False)
        encoded,_=self.helper.read_image(b)
        with Image.open(io.BytesIO(encoded)) as image:self.assertEqual(image.mode,'RGB')

    def test_malformed_stable_export_capability_is_rejected(self):
        with self.assertRaises(ValueError):self.helper.read_image(FakeBoard(alpha_export='yes'))

    def test_disabled_export_capability_is_rejected(self):
        with self.assertRaises(RuntimeError):self.helper.read_image(FakeBoard(alpha_export=False))

    def test_malformed_snapshot_availability_is_rejected(self):
        with self.assertRaises(ValueError):
            self.helper.read_image(FakeBoard(alpha_export=True,snapshot_alpha='yes'))

    def test_malformed_alpha_capability_is_rejected(self):
        with self.assertRaises(ValueError):self.helper.read_image(FakeBoard(alpha_value='true'))

    def test_malformed_alpha_size_is_rejected(self):
        for size in (True,3,5,-1):
            with self.subTest(size=size),self.assertRaises(ValueError):
                self.helper.read_image(FakeBoard(alpha=True,alpha_size=size))

    def test_alpha_disappearance_is_not_silently_flattened(self):
        with self.assertRaises(ValueError):self.helper.read_image(FakeBoard(alpha=True,alpha_size=0))

    def test_crc_mismatch_is_rejected(self):
        with self.assertRaises(ValueError):self.helper.read_image(FakeBoard(alpha=True,crc_bad=True))

    def test_extra_bytes_are_rejected(self):
        with self.assertRaises(ValueError):self.helper.read_image(FakeBoard(alpha=True,extra=True))

    def test_generation_change_is_rejected(self):
        with self.assertRaises(ValueError):self.helper.read_image(FakeBoard(alpha=True,changed=True))

    def test_mid_export_invalidation_is_rejected(self):
        with self.assertRaises(RuntimeError):self.helper.read_image(FakeBoard(alpha=True,invalidated=True))

    def test_pack_install_can_preserve_normal_style(self):
        from PIL import Image
        from avatar_pixel import compile_image
        image=Image.new('RGBA',(16,16),(80,100,120,255));output=io.BytesIO();image.save(output,format='PNG');image.close()
        b=FakeBoard();pack=compile_image(output.getvalue(),source_key=b.key).pack
        old=self.helper.chat.Board;self.helper.chat.Board=lambda port:b
        try:
            with tempfile.TemporaryDirectory() as tmp,contextlib.redirect_stdout(io.StringIO()):
                path=Path(tmp)/'asset.mspix';path.write_bytes(pack)
                self.helper.install('fake',path,enable=False)
        finally:self.helper.chat.Board=old
        self.assertIn('pixel.commit',b.commands)
        self.assertNotIn('pixel.on',b.commands)
        self.assertNotIn('pixel.off',b.commands)


if __name__=='__main__':
    unittest.main()
