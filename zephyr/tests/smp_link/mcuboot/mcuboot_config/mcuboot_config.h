/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MG_SIM_MCUBOOT_CONFIG_H_
#define MG_SIM_MCUBOOT_CONFIG_H_

/*
 * MCUboot's loader for the harness, configured like the XIAO's sysbuild
 * MCUboot (build-xiao/mcuboot/zephyr/.config): swap using move, slot 0
 * validated at every boot, SHA-256 through TinyCrypt, 16-byte writes on
 * storage that needs no erase.
 *
 * One difference on purpose: no signature check (no MCUBOOT_SIGN_*), so the
 * loader validates the SHA-256 TLV only. The board's ECDSA TLV is skipped;
 * signatures are MCUboot's own, unchanged code, and the update path under
 * test never touches them.
 */

#define MCUBOOT_USE_TINYCRYPT
#define MCUBOOT_SHA256
#define MCUBOOT_VALIDATE_PRIMARY_SLOT
#define MCUBOOT_IMAGE_NUMBER 1
#define MCUBOOT_MAX_IMG_SECTORS 256
#define MCUBOOT_USE_FLASH_AREA_GET_SECTORS
#define MCUBOOT_HAVE_LOGGING 1
#define MCUBOOT_FIH_PROFILE_OFF

#define MCUBOOT_BOOT_MAX_ALIGN 16
#define MCUBOOT_SWAP_USING_MOVE 1
#define MCUBOOT_SUPPORT_DEV_WITHOUT_ERASE 1 /* MCUboot's own name for CONFIG_MCUBOOT_STORAGE_WITHOUT_ERASE */

#define MCUBOOT_WATCHDOG_FEED() do { } while (0)
#define MCUBOOT_CPU_IDLE() do { } while (0)

#endif
