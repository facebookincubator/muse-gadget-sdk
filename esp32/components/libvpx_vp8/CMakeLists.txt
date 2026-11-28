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

# libvpx's VP8 decoder for video streaming (CONFIG_MUSE_VIDEO_VP8). The
# sources are the one copy in xplat/libvpx_vp8 (see its README.md), which keep
# their BSD license (LICENSE, PATENTS); this component only builds them for
# ESP-IDF.
set(src_dir "${CMAKE_CURRENT_LIST_DIR}/../../../xplat/libvpx_vp8/src")
if(CONFIG_MUSE_VIDEO_VP8)
    file(GLOB_RECURSE srcs "${src_dir}/*.c")
    if(CONFIG_IDF_TARGET_ESP32S3)
        # Bilinear prediction on the S3's SIMD (bit-exact; esp32/tests/test_libvpx_vp8.py checks the C).
        list(APPEND srcs "${src_dir}/vp8/common/xtensa/bilinear_pie.S")
    endif()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "${src_dir}"
        LDFRAGMENTS "linker.lf")
    # Upstream code, warnings and all; -O2 for the same IRAM reason as tinyh264.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
    if(CONFIG_COMPILER_HIDE_PATHS_MACROS)
        # __FILE__ (libvpx's asserts and allocation errors) as ../xplat/...,
        # relative to the project as ESP-IDF writes its own ./components/...,
        # not the build machine's path.
        get_filename_component(xplat_dir "${CMAKE_CURRENT_LIST_DIR}/../../../xplat" ABSOLUTE)
        target_compile_options(${COMPONENT_LIB} PRIVATE "-fmacro-prefix-map=${xplat_dir}=../xplat")
    endif()
    if(CONFIG_IDF_TARGET_ESP32S3)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MGX_PIE_S3=1)
    endif()
    # Picture border (xplat/libvpx_vp8/src/vpx_scale/yv12config.h): 16 pixels
    # keeps rows 16-byte aligned for the SIMD code (the S3's bilinear
    # prediction, the S31's color conversion); 8 elsewhere.
    if(CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32S31)
        target_compile_definitions(${COMPONENT_LIB} PUBLIC VP8_MGX_BORDER=16)
    endif()
    if(CONFIG_MUSE_VP8_IRAM_COMPACT)
        # Smaller code for IRAM (xplat/libvpx_vp8/src/vp8/common/filter.c; same
        # arithmetic).
        target_compile_definitions(${COMPONENT_LIB} PRIVATE VP8_MGX_COMPACT=1)
    endif()
    if(CONFIG_MUSE_VIDEO_VP8_PROF)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MGX_PROF=1)
    endif()
else()
    idf_component_register()
endif()
