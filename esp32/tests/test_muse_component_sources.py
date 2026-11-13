# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Execute component source selection for graphical, headless and Link builds."""
from pathlib import Path
import subprocess
import tempfile
import unittest

MUSE = Path(__file__).resolve().parents[1] / 'components/muse'


class MuseComponentSources(unittest.TestCase):
    def sources(self, **config):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            output = directory / 'sources.txt'
            script = directory / 'CMakeLists.txt'
            # Only replace the ESP-IDF registration/target hooks. The entire
            # production component CMakeLists selects its sources unchanged.
            hooks = '''
function(idf_component_register)
    cmake_parse_arguments(component "WHOLE_ARCHIVE" ""
        "SRCS;INCLUDE_DIRS;EMBED_FILES;LDFRAGMENTS;REQUIRES;PRIV_REQUIRES" ${ARGN})
    string(REPLACE ";" "\\n" lines "${component_SRCS}")
    file(WRITE "${SOURCES_OUT}" "${lines}")
endfunction()
function(idf_component_get_property name)
    set(${name} test_target PARENT_SCOPE)
endfunction()
function(target_compile_definitions)
endfunction()
function(set_source_files_properties)
endfunction()
'''
            settings = [f'set({key} {"ON" if value else "OFF"})' for key, value in config.items()]
            script.write_text(
                'cmake_minimum_required(VERSION 3.16)\nproject(muse_sources NONE)\n' + hooks + '\n'.join(settings)
                + f'\nset(COMPONENT_DIR "{MUSE.as_posix()}")\n'
                + f'set(SOURCES_OUT "{output.as_posix()}")\n'
                + f'include("{(MUSE / "CMakeLists.txt").as_posix()}")\n')
            result = subprocess.run(['cmake', '-S', str(directory), '-B', str(directory / 'build')],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return output.read_text().splitlines()

    def assert_option_support(self, sources):
        self.assertIn('muse_chat_session.cpp', sources)
        self.assertIn('muse_options.c', sources)
        self.assertIn('muse_options_json.c', sources)

    def test_each_headless_voice_profile_links_chat_option_support_without_ui(self):
        for board in ('VOICE_PE', 'RESPEAKER_LITE', 'RESPEAKER_XVF3800', 'WAVESHARE_154G'):
            for realtime in (False, True):
                with self.subTest(board=board, realtime=realtime):
                    sources = self.sources(CONFIG_HOMEHUB_VOICE=True,
                                           CONFIG_MUSE_REALTIME_VOICE=realtime,
                                           **{f'CONFIG_HOMEHUB_VOICE_BOARD_{board}': True})
                    self.assert_option_support(sources)
                    self.assertNotIn('muse_ui.c', sources)
                    self.assertNotIn('muse_options_ui.c', sources)

    def test_touchscreen_chat_build_includes_picker(self):
        sources = self.sources(CONFIG_MUSE_ENABLED=True, CONFIG_MUSE_HATCH=True)
        self.assert_option_support(sources)
        self.assertIn('muse_options_ui.c', sources)

    def test_freeink_chat_build_uses_shared_support_without_lvgl_picker(self):
        sources = self.sources(CONFIG_MUSE_ENABLED=True, CONFIG_MUSE_HATCH=True,
                               CONFIG_MUSE_UI_FREEINK=True)
        self.assert_option_support(sources)
        self.assertNotIn('muse_options_ui.c', sources)

    def test_link_only_ui_does_not_link_the_session_json_adapter(self):
        sources = self.sources(CONFIG_MUSE_ENABLED=True)
        self.assertIn('muse_chat_link.c', sources)
        self.assertNotIn('muse_chat_session.cpp', sources)
        self.assertNotIn('muse_options_json.c', sources)

    def test_plain_link_does_not_link_option_support(self):
        self.assertEqual(self.sources(), [])
