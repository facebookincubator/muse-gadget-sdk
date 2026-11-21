# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Called only when avatar support is enabled. Decoder only; no encoder,
# SIMD, threads, tools, or unbounded libc allocation. The original BSD license
# and patent grant remain in the pinned source archive.
function(muse_avatar_add_webp target)
    include(FetchContent)
    FetchContent_Declare(muse_avatar_webp
        URL "https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz"
        URL_HASH "SHA256=93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR muse-no-subproject)
    FetchContent_MakeAvailable(muse_avatar_webp)
    set(root "${muse_avatar_webp_SOURCE_DIR}")
    file(GLOB dec "${root}/src/dec/*.c")
    set(dsp alpha_processing cpu dec dec_clip_tables filters lossless rescaler upsampling yuv)
    set(utils bit_reader_utils color_cache_utils filters_utils huffman_utils
              palette quant_levels_dec_utils rescaler_utils random_utils thread_utils utils)
    set(srcs ${dec})
    foreach(name IN LISTS dsp)
        list(APPEND srcs "${root}/src/dsp/${name}.c")
    endforeach()
    foreach(name IN LISTS utils)
        list(APPEND srcs "${root}/src/utils/${name}.c")
    endforeach()
    set(conf "${CMAKE_CURRENT_BINARY_DIR}/avatar-webp-config")
    file(MAKE_DIRECTORY "${conf}/src/webp")
    file(WRITE "${conf}/src/webp/config.h" "/* Scalar decoder; no optional CPU features or threads. */\n")
    set_source_files_properties(${srcs} PROPERTIES
        COMPILE_DEFINITIONS "HAVE_CONFIG_H=1;WEBP_SWAP_16BIT_CSP=1")
    set_property(SOURCE "${root}/src/utils/utils.c" APPEND PROPERTY COMPILE_DEFINITIONS
        "malloc=muse_webp_malloc;calloc=muse_webp_calloc;free=muse_webp_free")
    target_sources(${target} PRIVATE ${srcs})
    target_include_directories(${target} PRIVATE "${conf}" "${root}" "${root}/src")
endfunction()
