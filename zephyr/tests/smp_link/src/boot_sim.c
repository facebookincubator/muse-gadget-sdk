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

/*
 * The reset path: MCUboot's own loader (bootutil, configured in
 * ../mcuboot/mcuboot_config) runs on the simulated flash before the app
 * starts, so a test, a confirm, a revert and a refused image go through
 * MCUboot's real swap-using-move code. What the app reports comes from the
 * flash, as on the board.
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <bootutil/bootutil.h>
#include <bootutil/fault_injection_hardening.h>
#include <bootutil/image.h>

#include "sim.h"

LOG_MODULE_REGISTER(mcuboot, LOG_LEVEL_INF);

int boot_sim_run(void)
{
	struct boot_rsp rsp;
	FIH_DECLARE(fih_rc, FIH_FAILURE);

	FIH_CALL(boot_go, fih_rc, &rsp);
	if (FIH_NOT_EQ(fih_rc, FIH_SUCCESS)) {
		printk("SIM: mcuboot: no bootable image\n");
		return -1;
	}
	printk("SIM: mcuboot: booting %u.%u.%u+%u\n", rsp.br_hdr->ih_ver.iv_major,
	       rsp.br_hdr->ih_ver.iv_minor, rsp.br_hdr->ih_ver.iv_revision,
	       rsp.br_hdr->ih_ver.iv_build_num);
	return 0;
}
