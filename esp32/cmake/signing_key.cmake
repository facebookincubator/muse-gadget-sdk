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

# A product build (CONFIG_HOMEHUB_DEV_BUILD=n, an option for someone shipping under their
# own key) never signs with the repository's dev_signing_key.pem, and fails without a
# readable key of its own. Dev builds need no private key of their own.
# Dev builds (the default) are unchanged. The dev key is matched by its file and by its
# public key, so a copy under another name or encoding is refused too: a production build
# needs openssl to read that public key, and fails without it.
set(MUSE_DEV_KEY_FILE_SHA256 "e57311281ea0478a80be57036768ef71481804eab73182b2b70255311a2ded6a")
set(MUSE_DEV_KEY_PUB_SHA256 "ad81bb60d2b0428e64da51fbe748c33053bc16ff7191c5078acc75ed95adc66b")

# dev_build: CONFIG_HOMEHUB_DEV_BUILD; key: CONFIG_SECURE_BOOT_SIGNING_KEY (relative to project_dir).
function(muse_check_signing_key dev_build signs key project_dir)
    if(dev_build OR NOT signs)
        return()
    endif()
    get_filename_component(path "${key}" ABSOLUTE BASE_DIR "${project_dir}")
    if(key STREQUAL "" OR NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
        message(FATAL_ERROR "Production build (CONFIG_HOMEHUB_DEV_BUILD=n): no signing key at '${key}'. "
                            "Point CONFIG_SECURE_BOOT_SIGNING_KEY at your own signing key, or build with CONFIG_HOMEHUB_DEV_BUILD=y.")
    endif()
    file(SHA256 "${path}" sha)
    set(pub "")
    find_program(MUSE_OPENSSL openssl)
    if(NOT MUSE_OPENSSL)
        message(FATAL_ERROR "Production build (CONFIG_HOMEHUB_DEV_BUILD=n): openssl is needed to check that "
                            "'${key}' isn't the repository's shared dev signing key.")
    endif()
    execute_process(COMMAND "${MUSE_OPENSSL}" pkey -in "${path}" -pubout -outform DER
                    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/muse_signing_pub.der" RESULT_VARIABLE rc
                    ERROR_QUIET)
    if(rc EQUAL 0)
        file(SHA256 "${CMAKE_CURRENT_BINARY_DIR}/muse_signing_pub.der" pub)
    endif()
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/muse_signing_pub.der")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Production build (CONFIG_HOMEHUB_DEV_BUILD=n): openssl can't read the private key "
                            "'${key}', so it can't be checked against the repository's dev signing key.")
    endif()
    if(sha STREQUAL MUSE_DEV_KEY_FILE_SHA256 OR pub STREQUAL MUSE_DEV_KEY_PUB_SHA256)
        message(FATAL_ERROR "Production build (CONFIG_HOMEHUB_DEV_BUILD=n) with the repository's shared "
                            "dev signing key ('${key}'): use your own signing key, or build with CONFIG_HOMEHUB_DEV_BUILD=y.")
    endif()
endfunction()
