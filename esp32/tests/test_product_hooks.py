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

"""Explicit product control component selection, registration and dispatch."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]


class ProductHooksTest(unittest.TestCase):
    def test_one_shared_updater_and_complete_control_selection(self):
        import itertools
        cases = [(component, header, impl, not any((component, header, impl)) or all((component, header, impl)))
                 for component, header, impl in itertools.product(("", "my_product_control"), ("", "x.h"), ("", "x.inc"))]
        cases += [(component, "x.h", "x.inc", False)
                  for component in ("main", "ota;other", "../ota", "ota/name")]
        for component, header, impl, accepted in cases:
            with self.subTest(component=component, header=header, impl=impl), tempfile.TemporaryDirectory() as name:
                script = Path(name) / "check.cmake"
                script.write_text('''
cmake_policy(SET CMP0057 NEW)
function(idf_build_get_property out property)
    set(${out} "${${property}}" PARENT_SCOPE)
endfunction()
macro(idf_component_register)
    cmake_parse_arguments(R "" "" "SRCS;REQUIRES;INCLUDE_DIRS;PRIV_INCLUDE_DIRS" ${ARGN})
    list(FILTER R_SRCS INCLUDE REGEX "(^|/)ota[.]c$")
    list(LENGTH R_SRCS count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "every product must compile exactly one shared ota.c")
    endif()
    if(PRODUCT_CONTROL_COMPONENT AND NOT "${PRODUCT_CONTROL_COMPONENT}" IN_LIST R_REQUIRES)
        message(FATAL_ERROR "selected control component must be a dependency")
    endif()
    return()
endmacro()
''' + f'set(PRODUCT_CONTROL_COMPONENT "{component}")\n'
                     + f'set(PRODUCT_CONTROL_EXTENSION_HEADER "{header}")\n'
                     + f'set(PRODUCT_CONTROL_EXTENSION_IMPL "{impl}")\n'
                     + f'include("{ROOT}/main/CMakeLists.txt")\n')
                ran = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
                self.assertEqual(ran.returncode == 0, accepted, ran.stdout + ran.stderr)
        project = (ROOT / "cmake/project.cmake").read_text()
        self.assertIn('idf_build_set_property(${property} "${${property}}")', project)
        self.assertIn('foreach(property PRODUCT_CONTROL_COMPONENT ', project)

    def test_control_hook_compile_definitions_are_main_private(self):
        source = (ROOT / "main/CMakeLists.txt").read_text()
        start = source.index("# Apply only to main")
        end = source.index("# device.guide", start)
        for component in ("", "my_product_control"):
            with self.subTest(component=component), tempfile.TemporaryDirectory() as name:
                script = Path(name) / "check.cmake"
                script.write_text('''
set(COMPONENT_LIB fake_main)
set(control_header product.h)
set(control_impl product.inc)
set(definitions_seen FALSE)
macro(target_compile_definitions target visibility)
    if(NOT "${target}" STREQUAL "fake_main" OR NOT "${visibility}" STREQUAL "PRIVATE")
        message(FATAL_ERROR "extension defines must be private to main")
    endif()
    set(definitions_seen TRUE)
endmacro()
''' + f'set(control_component "{component}")\n' + source[start:end] + '''
if(control_component AND NOT definitions_seen)
    message(FATAL_ERROR "control extension not enabled")
elseif(NOT control_component AND definitions_seen)
    message(FATAL_ERROR "public main must not enable product extensions")
endif()
''')
                ran = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_public_and_product_registration_and_invoke(self):
        source = (ROOT / "main/noise_control.cpp").read_text()
        functions = "\n".join(function_source(source, signature) for signature in (
            "static void add_command(", "static void add_register_metadata_string(",
            "static char *build_register_json(", "static void queue_result(",
        ))
        invoke = function_source(source, "static void handle_invoke_request(")
        def include_block(macro):
            start = source.index(f"#ifdef {macro}\n")
            return source[start:source.index("#endif", start) + len("#endif")]
        harness = (ROOT / "tests/product_hooks_harness.cpp").read_text()
        harness = harness.replace("@HEADER@", include_block("PRODUCT_CONTROL_EXTENSION_HEADER"))
        harness = harness.replace("@FUNCTIONS@", functions)
        harness = harness.replace("@IMPL@", include_block("PRODUCT_CONTROL_EXTENSION_IMPL"))
        harness = harness.replace("@INVOKE@", invoke)
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "harness.cpp").write_text(harness)
            (tmp / "product.h").write_text('''
static void register_product(cJSON *commands);
#define PRODUCT_CONTROL_REGISTER_COMMANDS register_product
''')
            (tmp / "product.inc").write_text('''
static void register_product(cJSON *commands) {
    add_command(commands, "product.ping", "Test-only product command", nullptr, nullptr);
}
static bool invoke_product(const char *command, cJSON *, noise_ctrl_session_generation_t g, const char *id) {
    if (strcmp(command, "product.ping")) return false;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    queue_result(g, id, result);
    return true;
}
#define PRODUCT_CONTROL_INVOKE invoke_product
''')
            flags = ["-Wall", "-Wextra", "-Werror", "-g", "-I", str(ROOT / "tests/link_fakes"),
                     "-I", str(tmp), "-I", str(ROOT / "main")]
            sanitizer = os.environ.get("HOST_SANITIZERS", "address,undefined")
            if sanitizer:
                flags += [f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer"]
            compiled = subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", *flags,
                                       "-c", str(ROOT / "tests/link_fakes/cJSON.c"), "-o", str(tmp / "json.o")],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            for product in (False, True):
                with self.subTest(product=product):
                    defines = ['-DPRODUCT_CONTROL_EXTENSION_HEADER="product.h"',
                               '-DPRODUCT_CONTROL_EXTENSION_IMPL="product.inc"'] if product else []
                    binary = tmp / "hooks"
                    compiled = subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                                               *flags, *defines, str(tmp / "harness.cpp"), str(tmp / "json.o"),
                                               "-o", str(binary)], capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    ran = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
