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

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mg_unit)

set(MG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/mg_common.cmake)

target_include_directories(app PRIVATE ${MG_PROTOCOLS} ${MG_SRC} src ${CMAKE_CURRENT_BINARY_DIR})
# After src: the app's own mg_token_proof.h comes first.
target_include_directories(app PRIVATE ${MG_SRC}/../../portable/mg_device_session/include
  ${MG_SRC}/../../portable/mg_link_setup/include)
target_compile_definitions(app PRIVATE MGLS_CRYPTO_PSA)

# The portable modules under test; the rest (BLE, audio thread, LED) are
# replaced by src/fakes.c.
target_sources(app PRIVATE
  ${MG_SRC}/mg_core.c
  ${MG_SRC}/mg_clock.c
  ${MG_SRC}/mg_power.c
  ${MG_SRC}/mg_boot.c
  ${MG_SRC}/mg_settings.c
  ${MG_SRC}/mg_session.c
  ${MG_SRC}/mg_identity.c
  ${MG_SRC}/mg_json.c
  ${MG_SRC}/mg_psa.c
  ${MG_SRC}/mg_setup.c
  ${MG_SRC}/mg_setup_crypto.c
  ${MG_SRC}/mg_setup_store.c
  ${MG_SRC}/mg_token_proof.c
  ${MG_SRC}/mg_codec.c
  ${MG_SRC}/mg_queue.c
  ${MG_SRC}/mg_hpf.c
  ${MG_SRC}/mg_connparam.c
  ${MG_SRC}/mg_throughput.c
  ${MG_SRC}/mg_envelope.c
  ${MG_SRC}/mg_vm_session.c
  ${MG_SRC}/mg_vm_link.c
  ${MG_SRC}/../../portable/mg_device_session/src/mg_session_seal.c
  ${MG_SRC}/mg_seal_crypto.c
  src/fakes.c
  src/test_settings.c
  src/test_core.c
  src/test_codec.c
  src/test_queue.c
  src/test_hpf.c
  src/test_throughput.c
  src/test_proof.c
  src/test_flash_writes.c
  ${MG_SRC}/mg_bt_txfix.c
  src/test_bt_txfix.c
)

mg_add_sbc(app)
mg_add_lc3(app)

mg_add_transcript(app)

# Link setup and token proof vectors, straight from the JSON files.
set(LINK_JSON ${MG_ROOT}/../esp32/tests/vectors/link_pairing_v5.json)
set(PROOF_JSON ${MG_PROTOCOLS}/test-vectors/mg-token-proof-v1.json)
set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/../host/gen_vectors.py)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vectors.h
  COMMAND ${PYTHON_EXECUTABLE} ${GEN} ${LINK_JSON} ${PROOF_JSON} ${CMAKE_CURRENT_BINARY_DIR}/vectors.h
  DEPENDS ${LINK_JSON} ${PROOF_JSON} ${GEN}
)
add_custom_target(mg_vectors DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/vectors.h)
add_dependencies(app mg_vectors)

# Every flash write through settings is counted (test_flash_writes.c): none
# per connection or link drop.
zephyr_link_libraries(-Wl,--wrap=settings_save_one -Wl,--wrap=settings_delete)
