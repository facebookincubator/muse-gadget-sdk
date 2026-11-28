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
 * The board hooks (mg_app.h) defined as a board's own code would define them
 * (CONFIG_MG_TEST_BOARD_HOOKS, simulation only): each logs when it's called,
 * so the BabbleSim run can check the app calls it. A real board keeps its
 * code in a directory beside the app with an app_ext.cmake and a Kconfig.ext
 * (zephyr/AGENTS.md, Board hooks).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "mg_app.h"

LOG_MODULE_REGISTER(mg_board_hooks, CONFIG_MG_LOG_LEVEL);

void mg_board_healthy(void)
{
	LOG_INF("board hooks: healthy");
}

void mg_board_setup_state(bool complete)
{
	LOG_INF("board hooks: setup state: %s", complete ? "set up" : "not set up");
}

uint32_t mg_board_reset_cause(uint32_t hw)
{
	LOG_INF("board hooks: reset cause 0x%x", (unsigned int)hw);
	return hw;
}

/* Not called in the simulation (no watchdog, no reachability restart):
 * defined so the build links them as a board's would. */
void mg_board_wdt_fed(void)
{
}

void mg_board_peer_restart(void)
{
	LOG_INF("board hooks: peer restart");
}

bool mg_board_nus_word(const char *cmd, char *out, size_t len)
{
	if (strcmp(cmd, "board hooks") != 0) {
		return false;
	}
	snprintk(out, len, "board hooks: a board word");
	return true;
}

#if defined(CONFIG_MG_SMP_GATE_BOARD)
/* A board whose images go through its own loader: the OS group's writes
 * only (its reset into that loader). */
bool mg_board_smp_write_allowed(uint16_t group, uint8_t id)
{
	ARG_UNUSED(id);
	return group == 0; /* MGMT_GROUP_ID_OS */
}
#endif
