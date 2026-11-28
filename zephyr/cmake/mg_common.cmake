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

# Shared by the app and the tests: the protocol headers, the vendored codecs
# and the portable sources that run the same on the board, native_sim and
# BabbleSim.

set(MG_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(MG_PROTOCOLS ${MG_ROOT}/../protocols)
# The codecs: one copy, shared with the ESP32 SDK.
set(MG_XPLAT ${MG_ROOT}/../xplat)
# The Muse Link setup transcript (pairing v5), compiled from the ESP32 SDK.
set(MG_TRANSCRIPT ${MG_ROOT}/../esp32/main/pairing_transcript)

if(NOT EXISTS ${MG_PROTOCOLS}/mgcommands.h OR NOT EXISTS ${MG_XPLAT}/libsbc/src/sbc.c
   OR NOT EXISTS ${MG_TRANSCRIPT}.c
   OR NOT EXISTS ${MG_ROOT}/../portable/mg_device_session/src/mg_session_seal.c)
  message(FATAL_ERROR "protocols/, xplat/, esp32/main/ or portable/ not found next to "
    "zephyr/; build from a full checkout")
endif()

# esp32/main/pairing_transcript.c as is (portable C), so both SDKs build the
# same setup transcript. Only its header is put on the include path, not the
# rest of esp32/main.
function(mg_add_transcript target)
  set(inc ${CMAKE_CURRENT_BINARY_DIR}/mg_transcript)
  configure_file(${MG_TRANSCRIPT}.h ${inc}/pairing_transcript.h COPYONLY)
  target_include_directories(${target} PRIVATE ${inc})
  target_sources(${target} PRIVATE ${MG_TRANSCRIPT}.c)
endfunction()

# Optimized Google libsbc (Apache-2.0), mono 8-subband build. See
# xplat/libsbc/README.md for the options.
function(mg_add_sbc target)
  set(dir ${MG_XPLAT}/libsbc)
  target_include_directories(${target} PRIVATE ${dir}/include)
  # SBC_MAX_CHANNELS sizes sbc_t, so it is public: the library and every file
  # that includes sbc.h see one value (sbc.h). The rest tune sbc.c only.
  target_compile_definitions(${target} PUBLIC SBC_MAX_CHANNELS=1)
  target_compile_definitions(${target} PRIVATE
    SBC_WITH_4SB=0 SBC_SMALL_CRC=1 SBC_FAST_DCT=1)
  if(CONFIG_CPU_CORTEX_M_HAS_DSP OR CONFIG_ARMV8_M_DSP)
    # DSP dual-MAC windowing, and bits.c compiled into sbc.c (SBC_UNITY).
    target_compile_definitions(${target} PRIVATE SBC_DSP=1 SBC_UNITY=1)
    set(sbc_srcs ${dir}/src/sbc.c)
  else()
    set(sbc_srcs ${dir}/src/sbc.c ${dir}/src/bits.c)
  endif()
  target_sources(${target} PRIVATE ${sbc_srcs})
  set_source_files_properties(${sbc_srcs} PROPERTIES
    INCLUDE_DIRECTORIES ${dir}/src COMPILE_OPTIONS "-O2;-Wno-maybe-uninitialized")
endfunction()

# Trimmed liblc3 (Apache-2.0), 16 and 24 kHz, 10 ms only, no LTPF. See
# xplat/liblc3/README.md. A firmware that only encodes links no decoder.
function(mg_add_lc3 target)
  set(dir ${MG_XPLAT}/liblc3)
  file(GLOB lc3_srcs ${dir}/src/*.c)
  target_sources(${target} PRIVATE ${lc3_srcs})
  target_include_directories(${target} PRIVATE ${dir}/include)
  # LC3_ENC_LTPF and LC3_DEC_LTPF size the encoder and decoder state, so they
  # are public: the library and every file that includes lc3.h see one value
  # (lc3_private.h). LC3_DT_MASK bit 3 = 10 ms, LC3_SR_MASK bits 1-2 = 16 and
  # 24 kHz.
  target_compile_definitions(${target} PUBLIC LC3_ENC_LTPF=0 LC3_DEC_LTPF=0)
  target_compile_definitions(${target} PRIVATE
    LC3_PLUS=0 LC3_PLUS_HR=0 LC3_DT_MASK=8 LC3_SR_MASK=6)
  set_source_files_properties(${lc3_srcs} PROPERTIES
    INCLUDE_DIRECTORIES ${dir}/src
    COMPILE_OPTIONS "-O2;-Wno-unused-function;-Wno-unused-variable")
endfunction()
