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

# LC3 for voice over musegadgets BLE (the BLE gadget component,
# components/muse_gadget_ble, sets the CONFIG_MUSE_GADGET_BLE_* options), 10 ms
# frames, no LTPF, at 16 and 24 kHz. Two implementations of the same API and bitstream, each
# decoding the other's frames:
# - xplat/liblc3: Google's Apache-2.0 liblc3 (floating point), shared with
#   the Zephyr SDK; for chips with a fast FPU (ESP32, S3).
# - xplat/lc3fx: its fixed-point port (integer only), a copy of the
#   fixed-point codec (VENDORED.md), for chips without an FPU (C3, C5, C6) and
#   wherever CONFIG_MUSE_GADGET_BLE_LC3_FIXED picks it (the ESP-Mosaico).
# See the README.md of each. linker.lf places the codec
# (CONFIG_MUSE_GADGET_BLE_LC3_PLACE_*).
if(CONFIG_MUSE_GADGET_BLE_LC3_FIXED)
    set(src_dir "${CMAKE_CURRENT_LIST_DIR}/../../../xplat/lc3fx")
else()
    set(src_dir "${CMAKE_CURRENT_LIST_DIR}/../../../xplat/liblc3")
endif()
if(CONFIG_MUSE_GADGET_BLE_LC3)
    set(srcs)
    foreach(f attdet bits bwdet energy lc3 ltpf mdct plc sns spec tables tns)
        list(APPEND srcs "${src_dir}/src/${f}.c")
    endforeach()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "${src_dir}/include"
        PRIV_INCLUDE_DIRS "${src_dir}/src"
        LDFRAGMENTS "linker.lf")
    if(CONFIG_MUSE_GADGET_BLE_LC3_FIXED)
        # LC3_DEC_LTPF sizes the decoder state, so it is public.
        target_compile_definitions(${COMPONENT_LIB}
            PUBLIC LC3_DEC_LTPF=0
            PRIVATE LC3_DT_MASK=8 LC3_SR_MASK=6)   # 10 ms at 16 and 24 kHz
        # Integer only: no -ffast-math, no floating point at all.
        target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function -Wno-unused-const-variable)
    else()
        # The LTPF options size the encoder and decoder state, so they are public.
        target_compile_definitions(${COMPONENT_LIB}
            PUBLIC LC3_ENC_LTPF=0 LC3_DEC_LTPF=0
            PRIVATE LC3_PLUS=0 LC3_PLUS_HR=0 LC3_DT_MASK=8 LC3_SR_MASK=6)   # 10 ms at 16 and 24 kHz
        # Upstream code otherwise as it ships: the masks and the NULL tables of the trimmed configurations
        # leave some of its functions and constants unused.
        target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math -Wno-unused-function -Wno-unused-const-variable)
    endif()
else()
    idf_component_register()
endif()
