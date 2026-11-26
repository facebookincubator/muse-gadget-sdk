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

set(srcs
    "src/ClientSession.cpp"
    "src/InitiatorHandshake.cpp"
    "src/PsaCryptoBackend.cpp"
    "src/ServiceCodec.cpp"
    "src/Status.cpp"
    "src/Transport.cpp"
    "src/TransportFrameCodec.cpp"
    "src/X25519Selftest.cpp"
)
set(priv_includes "")
# X25519 from HACL* (Kconfig NOISE_CORE_X25519_HACL): ESP-IDF's own copy of
# mbedTLS's Everest driver. Its paths are private to ESP-IDF, so on an
# ESP-IDF it wasn't checked on, or without one of its files, Noise keeps
# PSA's X25519 (same results, slower) and the build goes on.
set(use_hacl FALSE)
if(CONFIG_NOISE_CORE_X25519_HACL)
    set(everest "${IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/drivers/everest")
    set(everest_inc "${everest}/include/tf-psa-crypto/private/everest")
    set(idf_ver "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
    set(use_hacl TRUE)
    if(idf_ver VERSION_LESS 6.0 OR NOT idf_ver VERSION_LESS 6.1)
        set(use_hacl FALSE)
        set(why "checked on ESP-IDF v6.0.x only, this is v${idf_ver}")
    else()
        foreach(file
                "${everest}/library/legacy/Hacl_Curve25519.c"
                "${everest}/library/kremlib/FStar_UInt128_extracted.c"
                "${everest}/library/kremlib/FStar_UInt64_FStar_UInt32_FStar_UInt16_FStar_UInt8.c"
                "${everest_inc}/Hacl_Curve25519.h"
                "${everest_inc}/kremlib.h"
                "${everest_inc}/kremlib/FStar_UInt128.h"
                "${everest_inc}/kremlin/internal/types.h")
            if(NOT EXISTS "${file}")
                set(use_hacl FALSE)
                set(why "no ${file}")
                break()
            endif()
        endforeach()
    endif()
    if(use_hacl)
        list(APPEND srcs "src/X25519Hacl.c")
        list(APPEND priv_includes "${everest}/library" "${everest_inc}" "${everest_inc}/kremlib")
    elseif(NOT CMAKE_BUILD_EARLY_EXPANSION)
        message(WARNING "NOISE_CORE_X25519_HACL: ${why}; Noise uses PSA's X25519 instead")
    endif()
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${priv_includes}
    PRIV_REQUIRES mbedtls secure_random esp_timer
)

# The console's `>noise.x25519` (components/muse, Muse builds only) reaches
# the self-test through a weak reference, which alone wouldn't pull it out of
# the archive. Only builds that hold a Noise session of their own get it:
# Link-only builds have no such console, and BLE-standalone builds
# (MUSE_GADGET_BLE_STANDALONE) link no Noise crypto at all, so forcing the
# self-test in would add PSA's X25519 and AEAD code (~15 KB on the C6) for it.
if(CONFIG_MUSE_ENABLED AND NOT CONFIG_MUSE_GADGET_BLE_STANDALONE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u noise_core_x25519_selftest")
endif()

if(use_hacl)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOISE_CORE_X25519_HACL=1)
    # Generated (KreMLin) code: its unused helpers aren't ours to trim.
    set_source_files_properties("src/X25519Hacl.c" PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-function;-Wno-unused-variable;-Wno-unused-parameter")
endif()

# Every random draw (Noise's ephemeral keys, Random()) only with a radio on
# (components/secure_random). Host builds of the core leave it off.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NOISE_CORE_SECURE_RANDOM=1)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
