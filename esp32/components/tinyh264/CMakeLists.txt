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

# H.264 constrained baseline decoder for video streaming
# (CONFIG_MUSE_VIDEO). See README.md.
if(CONFIG_MUSE_VIDEO)
    file(GLOB srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
    if(CONFIG_MUSE_VIDEO_SIMD_MC)
        # The luma 6-tap filters on the S31's SIMD (h264bsd_reconstruct.c checks them).
        list(APPEND srcs "${CMAKE_CURRENT_LIST_DIR}/src/h264bsd_interp_pie_s31.S")
    endif()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "src"
        LDFRAGMENTS "linker.lf")
    # Upstream code, warnings and all. -O2, not the -O3 the host bench liked:
    # -O3's inlining makes the hot code 2.3 times bigger (110 KB), more than
    # IRAM can spare, and IRAM matters more.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
    if(CONFIG_MUSE_VIDEO_SIMD_MC)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE TINYH264_PIE_S31=1)
    endif()
else()
    idf_component_register()
endif()
