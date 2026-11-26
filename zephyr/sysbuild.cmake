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

# Never build a unit that trusts a public signing key by
# accident. The configure step fails when the MCUboot signing key is one of
# MCUboot's public development keys (by path or by content) and the build
# doesn't say it's for development (SB_CONFIG_MG_DEV_SIGNING_KEY,
# sysbuild-dev.conf), or when there is no key.
if(SB_CONFIG_BOOTLOADER_MCUBOOT)
  set(mg_dev_ok)
  if(SB_CONFIG_MG_DEV_SIGNING_KEY)
    set(mg_dev_ok --dev-ok)
    # The app says so too: ".devkey" in its firmware string and a boot
    # banner line, so a sample-key build is never taken for a release
    # (cmake/mg_version.cmake reads this).
    set(${DEFAULT_IMAGE}_MG_DEVKEY y CACHE INTERNAL "signed with a public development key")
  endif()
  # An optional blocklist of public, sample or committed keys
  # (-DMG_KEY_BLOCKLIST=<file>: public-key sha256 fingerprints, one per line;
  # tools/check_signing_key.py --blocklist). A key listed there counts as
  # public, like MCUboot's own sample keys.
  set(mg_blocklist)
  if(DEFINED MG_KEY_BLOCKLIST)
    set(mg_blocklist --blocklist "${MG_KEY_BLOCKLIST}")
  endif()
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/check_signing_key.py
            --key "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}" --mcuboot "${ZEPHYR_MCUBOOT_MODULE_DIR}"
            ${mg_dev_ok} ${mg_blocklist}
    RESULT_VARIABLE mg_key_rc
    OUTPUT_VARIABLE mg_key_out
    ERROR_VARIABLE mg_key_out)
  if(mg_key_rc)
    message(FATAL_ERROR "${mg_key_out}")
  elseif(mg_key_out)
    message(WARNING "${mg_key_out}")
  endif()
endif()

# The boot record's retained block sits above CONFIG_MG_BOOT_RAM_END, a
# number the board sets for its MCUboot (mg_health.c checks the block against
# it): after both images are built, the MCUboot this build made must not use
# RAM past it (tools/check_boot_ram.py; nothing to check when it's 0).
if(SB_CONFIG_BOOTLOADER_MCUBOOT)
  ExternalProject_Get_property(mcuboot BINARY_DIR)
  set(mg_mcuboot_dir ${BINARY_DIR})
  ExternalProject_Get_property(${DEFAULT_IMAGE} BINARY_DIR)
  add_custom_target(mg_boot_ram_check ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/check_boot_ram.py
            --app-config ${BINARY_DIR}/zephyr/.config
            --mcuboot-map ${mg_mcuboot_dir}/zephyr/zephyr.map
    VERBATIM)
  add_dependencies(mg_boot_ram_check mcuboot ${DEFAULT_IMAGE})
endif()

# The Bluetooth controller on a separate network core (NET_CORE_BOARD).
if(SB_CONFIG_NET_CORE_IMAGE_HCI_IPC)
  ExternalZephyrProject_Add(
    APPLICATION hci_ipc
    SOURCE_DIR  ${ZEPHYR_BASE}/samples/bluetooth/hci_ipc
    BOARD       ${SB_CONFIG_NET_CORE_BOARD}
  )
endif()
