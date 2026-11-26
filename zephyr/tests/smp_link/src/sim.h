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

#ifndef MG_SIM_H_
#define MG_SIM_H_

#include <stdbool.h>
#include <stdint.h>

/* boot_sim.c: MCUboot's boot_go() on the simulated flash, as at a reset.
 * 0, or <0 when no image can boot (the process then exits). */
int boot_sim_run(void);

/* link.c: the UDP "fake GATT" link (README.md, The link). */
int link_start(uint16_t port);
/* GATT is up: answer connects with ready (now, or the next one). */
void link_set_ready(bool ready);
/* The SMP reset is coming: drop the client (0x02) and take no connect. */
void link_hold_for_reset(void);

/* stubs.c, main.c */
void sim_app_wq_start(void);
extern bool sim_safe_mode;
extern bool sim_safe_button; /* SAFE mode asked with the button */
extern bool sim_set_up; /* a proof key stored */
extern bool sim_proven; /* this connection passed the token proof */
extern bool sim_release; /* --release: a release build's update rule */
extern bool sim_proven_at_connect; /* --proven: each connection starts proven */
extern bool sim_healthy;
extern uint32_t sim_boot; /* this boot's number since run.py started (--boot) */

/* ../../src/mg_smp_mem.c: memory blocks SMP holds right now (0 when idle). */
int mg_smp_mem_blocks(void);

#endif
