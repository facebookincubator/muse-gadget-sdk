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

# The XIAO's firmware-update path on native_sim, for the Muse apps' mcumgr
# clients (README.md): Zephyr's MCUmgr (img_mgmt, os_mgmt, the BLE
# transport's reassembly) with the XIAO's settings, src/mg_ota.c as built
# for the board, and MCUboot's own loader (swap using move) run on the
# simulated RRAM at every start, behind a UDP "fake GATT" link.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mg_smp_link)

set(MG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(MG_PROTOCOLS ${CMAKE_CURRENT_SOURCE_DIR}/../../../protocols)
set(MCUBOOT ${ZEPHYR_MCUBOOT_MODULE_DIR})

target_include_directories(app PRIVATE ${MG_SRC} src ${MG_PROTOCOLS}
  ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include)
target_sources(app PRIVATE src/main.c src/link.c src/stubs.c src/boot_sim.c ${MG_SRC}/mg_ota.c)

# As on the XIAO: Zephyr's smp.c replaced by the board's copy
# (buffers and work queue allocated while SMP is in use), the image writer
# from the wrap in mg_smp_mem.c.
set(MG_SDK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_sources(app PRIVATE ${MG_SDK_SRC}/mg_smp_mem.c)
zephyr_link_libraries(-Wl,--wrap=img_mgmt_write_image_data)
get_target_property(mg_mcumgr_srcs mcumgr_transport SOURCES)
list(FILTER mg_mcumgr_srcs EXCLUDE REGEX "(^|/subsys/mgmt/mcumgr/transport/)src/smp\\.c$")
set_property(TARGET mcumgr_transport PROPERTY SOURCES ${mg_mcumgr_srcs})
target_sources(mcumgr_transport PRIVATE ${MG_SDK_SRC}/zephyr/smp.c)
target_link_libraries(app PRIVATE MCUBOOT_BOOTUTIL)
# As on the board (CMakeLists.txt): img_mgmt's state write goes through
# mg_ota.c's boot_set_next(). And an SMP reset ends the process with status
# 77 instead of re-executing it, so run.py can restart it (src/main.c).
zephyr_link_libraries(-Wl,--wrap=boot_set_next -Wl,--wrap=sys_arch_reboot)
# src/link.c feeds writes to the reassembly as smp_bt does (expected, then
# collect): through mg_ota.c's gap check, as on the board.
zephyr_link_libraries(-Wl,--wrap=smp_reassembly_expected -Wl,--wrap=smp_reassembly_collect)

# MCUboot's loader, configured like the XIAO's (mcuboot/mcuboot_config,
# found before MCUboot's own Zephyr config).
target_include_directories(app BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mcuboot)
target_include_directories(app PRIVATE
  ${MCUBOOT}/boot/bootutil/include ${MCUBOOT}/boot/bootutil/src
  ${MCUBOOT}/boot/zephyr/include ${MCUBOOT}/ext/tinycrypt/lib/include)
target_sources(app PRIVATE
  ${MCUBOOT}/boot/bootutil/src/loader.c
  ${MCUBOOT}/boot/bootutil/src/bootutil_loader.c
  ${MCUBOOT}/boot/bootutil/src/bootutil_misc.c
  ${MCUBOOT}/boot/bootutil/src/bootutil_area.c
  ${MCUBOOT}/boot/bootutil/src/swap_misc.c
  ${MCUBOOT}/boot/bootutil/src/swap_move.c
  ${MCUBOOT}/boot/bootutil/src/swap_scratch.c
  ${MCUBOOT}/boot/bootutil/src/image_validate.c
  ${MCUBOOT}/boot/bootutil/src/bootutil_img_hash.c
  ${MCUBOOT}/boot/bootutil/src/tlv.c
  ${MCUBOOT}/boot/bootutil/src/fault_injection_hardening.c
  ${MCUBOOT}/boot/zephyr/flash_map_extended.c
  ${MCUBOOT}/ext/tinycrypt/lib/source/sha256.c
  ${MCUBOOT}/ext/tinycrypt/lib/source/utils.c
)
# The release rule at run time (--release), for the update prompt rows.
target_compile_definitions(app PRIVATE MG_SMP_LINK_SIM)
