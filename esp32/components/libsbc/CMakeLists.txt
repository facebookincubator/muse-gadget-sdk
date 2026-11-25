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

# libsbc: Google's Apache-2.0 SBC codec, trimmed for 16 kHz mono voice over
# musegadgets BLE (the BLE gadget component, which comes in a later change,
# sets CONFIG_MUSE_GADGET_BLE_AUDIO). The sources are the one copy
# in xplat/libsbc, shared with the Zephyr SDK; see its README.md.
set(src_dir "${CMAKE_CURRENT_LIST_DIR}/../../../xplat/libsbc")
if(CONFIG_MUSE_GADGET_BLE_AUDIO)
    idf_component_register(
        SRCS "${src_dir}/src/sbc.c" "${src_dir}/src/bits.c"
        INCLUDE_DIRS "${src_dir}/include"
        PRIV_INCLUDE_DIRS "${src_dir}/src")
    # Mono, 8 subbands only; the options that change struct sbc are public.
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SBC_MAX_CHANNELS=1
        PRIVATE SBC_WITH_4SB=0 SBC_SMALL_CRC=1 SBC_FAST_DCT=1)
    # Upstream code as it ships; GCC 15 flags a false positive in sbc_encode.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-maybe-uninitialized)
else()
    idf_component_register()
endif()
