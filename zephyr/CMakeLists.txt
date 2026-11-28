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

# Before Zephyr's Kconfig runs: the version strings.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mg_version.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(muse_gadget_zephyr)

# The image id's build variant, guessed before Kconfig, against the effective
# configuration (cmake/mg_image_id.cmake): a mismatch fails the build.
mg_image_variant_check(${MG_IMAGE_VARIANT})

# The XIAO's flash map (mcuboot, slot0, slot1, storage) boots through MCUboot:
# without it the image would sit at slot0 with nothing at 0 to start it.
if(CONFIG_BOARD_XIAO_NRF54L15 AND NOT CONFIG_BOOTLOADER_MCUBOOT)
  message(FATAL_ERROR "Build the XIAO nRF54L15 with --sysbuild: it boots through MCUboot "
    "(sysbuild.conf). See AGENTS.md, Build.")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mg_common.cmake)

target_include_directories(app PRIVATE ${MG_PROTOCOLS} src)

target_sources(app PRIVATE
  src/main.c
  src/mg_core.c
  src/mg_clock.c
  src/mg_power.c
  src/mg_boot.c
  src/mg_health.c
  src/mg_settings.c
  src/mg_session.c
  src/mg_identity.c
  src/mg_json.c
  src/mg_psa.c
  src/mg_setup.c
  src/mg_setup_crypto.c
  src/mg_setup_store.c
  src/mg_token_proof.c
  src/mg_codec.c
  src/mg_audio.c
  src/mg_hpf.c
  src/mg_ble.c
  src/mg_connparam.c
  src/mg_throughput.c
  src/mg_led.c
  src/mg_led_pattern.c
  src/mg_button.c
)
# The TX path's ACL leg counts the controller's completed packets (mg_ble.c): the
# host's bt_conn_tx_notify(conn, false), once per Number of Completed Packets.
zephyr_link_libraries(-Wl,--wrap=bt_conn_tx_notify)
# A disconnect the host asks for on its own TX processor goes out from the
# system work queue (src/mg_bt_txfix.c: Zephyr v4.4.2's host would wait there
# for a command only that thread sends, then assert after 10 s).
if(CONFIG_BT_TX_PROCESSOR_THREAD AND CONFIG_BT_CONN)
  target_sources(app PRIVATE src/mg_bt_txfix.c)
  zephyr_link_libraries(-Wl,--wrap=bt_send -Wl,--wrap=bt_hci_disconnect)
endif()
target_sources_ifdef(CONFIG_MG_VM_SESSION app PRIVATE
  src/mg_envelope.c
  src/mg_vm_session.c
  src/mg_vm_link.c
  src/mg_vm_commands.c
  # Sealed credentials: the shared seal, vendored
  # unchanged, on this app's PSA Crypto.
  ../portable/mg_device_session/src/mg_session_seal.c
  src/mg_seal_crypto.c
)
if(CONFIG_MG_VM_SESSION)
  # After src: this app's own mg_token_proof.h comes first.
  target_include_directories(app PRIVATE
    ../portable/mg_device_session/include ../portable/mg_link_setup/include)
  # mgls_crypto.h's PSA types. MGSEAL_EXT_SHA stays 0 here: the seal's own
  # SHA-256 is smaller on this app than PSA's multipart hash, which nothing
  # else links (MGSEAL_EXT_SHA=1 measured +1,252 B of flash).
  target_compile_definitions(app PRIVATE MGLS_CRYPTO_PSA)
endif()
target_sources_ifdef(CONFIG_MG_AUDIO_QUEUE app PRIVATE src/mg_queue.c)
# A board's own app code (its board hooks, mg_app.h) lives beside the app in a
# directory with an app_ext.cmake; each one includes itself when its board
# builds.
file(GLOB mg_app_exts ${CMAKE_CURRENT_SOURCE_DIR}/*/app_ext.cmake)
foreach(ext ${mg_app_exts})
  include(${ext})
endforeach()
target_sources_ifdef(CONFIG_MG_BATTERY_ADC app PRIVATE src/mg_battery.c)
target_sources_ifdef(CONFIG_MG_SDK_TOKEN_SLOT app PRIVATE src/mg_token_slot.c)
if(CONFIG_MG_SDK_TOKEN_SLOT)
  # The updater finds the slot by its marker: exactly once in the image,
  # checked on zephyr.bin after every link.
  add_custom_target(mg_token_slot_check ALL
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_token_slot.py
            ${ZEPHYR_BINARY_DIR}/${KERNEL_BIN_NAME}
    VERBATIM)
  add_dependencies(mg_token_slot_check zephyr_final)
endif()
if(NOT CONFIG_ARCH_POSIX)
  # Every Nordic UART word is classified and gated (tools/check_nus_words.py):
  # before the app compiles, so an unclassified word never builds.
  add_custom_target(mg_nus_words_check ALL
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_nus_words.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src/mg_nus.c ${CMAKE_CURRENT_SOURCE_DIR}/src/mg_nus_policy.h
    VERBATIM)
  add_dependencies(app mg_nus_words_check)
  # Every owner-content wireless read is listed and gated
  # (tools/check_owner_reads.py, src/mg_owner_reads.h).
  add_custom_target(mg_owner_reads_check ALL
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_owner_reads.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    VERBATIM)
  add_dependencies(app mg_owner_reads_check)
  # No log line formats a secret or a hex buffer (every line can reach the
  # Nordic UART mirror; tools/check_log_secrets.py), board code included:
  # every directory beside src/ with an app_ext.cmake.
  set(mg_app_ext_dirs)
  file(GLOB mg_app_ext_files ${CMAKE_CURRENT_SOURCE_DIR}/*/app_ext.cmake)
  foreach(mg_ext ${mg_app_ext_files})
    get_filename_component(mg_ext_dir ${mg_ext} DIRECTORY)
    list(APPEND mg_app_ext_dirs ${mg_ext_dir})
  endforeach()
  add_custom_target(mg_log_secrets_check ALL
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_log_secrets.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src ${mg_app_ext_dirs}
    VERBATIM)
  add_dependencies(app mg_log_secrets_check)
  # The malloc arena (all RAM no static data uses) holds both the device
  # session's block and SMP's, the BLE recovery tier: a build whose statics
  # leave too little fails here instead of an update failing in the field
  # (tools/check_arena.py).
  if(CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE EQUAL -1 AND CONFIG_MCUMGR)
    math(EXPR mg_ram_end "${CONFIG_SRAM_BASE_ADDRESS} + ${CONFIG_SRAM_SIZE} * 1024" OUTPUT_FORMAT HEXADECIMAL)
    add_custom_target(mg_arena_check ALL
      COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_arena.py
              --nm ${CMAKE_NM} --elf ${ZEPHYR_BINARY_DIR}/${KERNEL_ELF_NAME}
              --ram-end ${mg_ram_end} --min-kb 48
      VERBATIM)
    add_dependencies(mg_arena_check zephyr_final)
  endif()
  # The mic's token-proof gate is linked, and a release image has no bench
  # path around it (tools/check_mic_gate.py): checked after every link. A
  # bench-mode image passes its own build's check (--bench-mode); what
  # refuses it as a release is its .bench marker and a separate
  # check_mic_gate.py --release run without --bench-mode (publish tooling).
  if(CONFIG_MG_AUTOMATION)
    set(mg_mic_release "")
  elseif(CONFIG_MG_BENCH_MODE)
    set(mg_mic_release --release --bench-mode)
  else()
    set(mg_mic_release "--release")
  endif()
  add_custom_target(mg_mic_gate_check ALL
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mic_gate.py
            --nm ${CMAKE_NM} --elf ${ZEPHYR_BINARY_DIR}/${KERNEL_ELF_NAME}
            --bin ${ZEPHYR_BINARY_DIR}/${KERNEL_BIN_NAME} ${mg_mic_release}
    VERBATIM)
  add_dependencies(mg_mic_gate_check zephyr_final)
endif()
target_sources_ifdef(CONFIG_MG_NUS app PRIVATE src/mg_nus.c)
if(CONFIG_MCUMGR_TRANSPORT_BT AND NOT CONFIG_MG_DFU AND NOT CONFIG_MG_SMP_GATE_BOARD)
  # mg_ota.c (or mg_smp_gate.c) holds the update writes' token-proof gate:
  # without one SMP over Bluetooth would take updates from anyone.
  message(FATAL_ERROR "CONFIG_MCUMGR_TRANSPORT_BT needs CONFIG_MG_DFU (the update gate) or "
                      "CONFIG_MG_SMP_GATE_BOARD; turn it off for a build without Bluetooth updates")
endif()
if(CONFIG_MG_SMP_GATE_BOARD)
  # Each request's source connection (src/mg_smp_req_gen.c): its first
  # fragment's, stamped where the transport hands the request over, read
  # while its handlers run; with reassembly a partly received request whose
  # connection is gone is dropped before the next write is collected.
  target_sources(app PRIVATE src/mg_smp_gate.c src/mg_smp_req_gen.c)
  zephyr_link_libraries(-Wl,--wrap=smp_rx_req -Wl,--wrap=smp_process_request_packet)
  if(CONFIG_MCUMGR_TRANSPORT_REASSEMBLY)
    target_include_directories(app PRIVATE ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include)
    zephyr_link_libraries(-Wl,--wrap=smp_reassembly_expected -Wl,--wrap=smp_reassembly_collect)
  endif()
endif()
target_sources_ifdef(CONFIG_MG_DFU app PRIVATE src/mg_ota.c)
if(CONFIG_MG_DFU)
  # img_mgmt's image state write changes the next boot only through
  # boot_set_next(): mg_ota.c applies a client's confirm that comes before
  # the boot is healthy at the healthy mark (src/mg_ota_policy.h).
  target_link_libraries(app PRIVATE MCUBOOT_BOOTUTIL)
  zephyr_link_libraries(-Wl,--wrap=boot_set_next)
  # Update support takes no base memory (src/mg_smp_mem.c): MCUmgr's
  # buffers and work queue from src/zephyr/smp.c, the copy of Zephyr's built
  # in its place, and the image writer's context from the wrap of
  # img_mgmt_write_image_data(), all allocated only while SMP is in use.
  target_sources(app PRIVATE src/mg_smp_mem.c)
  zephyr_link_libraries(-Wl,--wrap=img_mgmt_write_image_data)
  if(CONFIG_MG_QUEUE_IN_SLOT1)
    # An image erase takes slot1 from the clips only once img_mgmt accepted it.
    zephyr_link_libraries(-Wl,--wrap=img_mgmt_erase_slot)
  endif()
  get_target_property(mg_mcumgr_srcs mcumgr_transport SOURCES)
  list(FILTER mg_mcumgr_srcs EXCLUDE REGEX "(^|/subsys/mgmt/mcumgr/transport/)src/smp\\.c$")
  set_property(TARGET mcumgr_transport PROPERTY SOURCES ${mg_mcumgr_srcs})
  target_sources(mcumgr_transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/zephyr/smp.c)
  if(CONFIG_MCUMGR_TRANSPORT_REASSEMBLY)
    # smp_bt asks smp_reassembly_expected() before it collects each SMP
    # write: mg_ota.c drops a partly received request there after a gap (a
    # lost write) or when the link it began on is gone, and records the
    # connection of a request's first fragment.
    target_include_directories(app PRIVATE ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include)
    zephyr_link_libraries(-Wl,--wrap=smp_reassembly_expected -Wl,--wrap=smp_reassembly_collect)
  endif()
endif()
target_sources_ifdef(CONFIG_MG_POWER_OFF app PRIVATE src/mg_poweroff.c src/mg_usb.c)
target_sources_ifdef(CONFIG_MG_BENCH app PRIVATE src/mg_bench.c)
target_sources_ifdef(CONFIG_MG_AUDIO_SRC_DMIC app PRIVATE src/mg_audio_dmic.c)
target_sources_ifdef(CONFIG_MG_LC3_24K app PRIVATE src/mg_resample.c)
target_sources_ifdef(CONFIG_MG_AUDIO_SRC_SYNTH app PRIVATE src/mg_audio_synth.c)
if(CONFIG_MG_BSIM_TESTS)
  add_subdirectory(${ZEPHYR_BASE}/tests/bsim/babblekit babblekit)
  target_link_libraries(app PRIVATE babblekit)
  target_sources(app PRIVATE tests/bsim/gadget_hooks.c)
  zephyr_include_directories(
    $ENV{BSIM_COMPONENTS_PATH}/libUtilv1/src $ENV{BSIM_COMPONENTS_PATH}/libPhyComv1/src)
endif()

mg_add_transcript(app)
mg_add_sbc(app)
if(CONFIG_MG_LC3)
  mg_add_lc3(app)
endif()
