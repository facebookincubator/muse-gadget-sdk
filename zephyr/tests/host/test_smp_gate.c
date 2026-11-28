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
 * CONFIG_MG_SMP_GATE_BOARD's gate (src/mg_smp_gate.c) through the callback it
 * registers with MCUmgr: on a set-up gadget every command needs this
 * connection's token proof but the OS group's harmless status, SAFE takes
 * only SAFE asked with the button, and each write that passes meets the
 * board's own gate.
 */

#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#include "check.h"
#include "host_fakes.h"
#include "mg_app.h"
#include "mg_boot.h"
#include "mg_ota_policy.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"

static struct mgmt_callback *registered;
static bool set_up, proven, safe, button_safe, board_allows = true;
static int board_asked, rx_requests;

void mgmt_callback_register(struct mgmt_callback *callback)
{
	registered = callback;
}

bool mg_setup_store_complete(void)
{
	return set_up;
}

/* A row's link change while the gate decides: the old link drops and the
 * next connection proves just as the proof (or the board's gate) is read. */
static bool relink_at_proof, relink_at_board;

bool mg_token_proof_matched(void)
{
	if (relink_at_proof) {
		host_conn_gen++;
		return true;
	}
	return proven;
}

bool mg_health_safe(void)
{
	return safe;
}

bool mg_health_safe_by_button(void)
{
	return button_safe;
}

void mg_transport_rx_request(void)
{
	rx_requests++;
}

/* The request's source connection (src/mg_smp_req_gen.c on a board): the
 * link's own unless a row says otherwise. */
static bool gen_follows = true;
static uint32_t req_gen;

uint32_t mg_smp_request_gen(void)
{
	return gen_follows ? host_conn_gen : req_gen;
}

/* The board's gate (no default: this build defines it, as a board must). */
bool mg_board_smp_write_allowed(uint16_t group, uint8_t id)
{
	(void)group;
	(void)id;
	board_asked++;
	if (relink_at_board) {
		host_conn_gen++;
	}
	return board_allows;
}

/* One command through the gate: whether it may run. */
static bool cmd(uint16_t group, uint8_t id, uint8_t op)
{
	struct mgmt_evt_op_cmd_arg arg = {.group = group, .id = id, .op = op};
	int32_t rc = 0;
	uint16_t grp = 0;
	bool abort_more = false;
	enum mgmt_cb_return r = registered->callback(MGMT_EVT_OP_CMD_RECV, MGMT_CB_OK, &rc, &grp,
						     &abort_more, &arg, sizeof(arg));

	if (r == MGMT_CB_OK) {
		return true;
	}
	CHECK(r == MGMT_CB_ERROR_RC && rc == MGMT_ERR_EACCESSDENIED);
	return false;
}

static void state(bool s, bool p, bool sf, bool b)
{
	set_up = s;
	proven = p;
	safe = sf;
	button_safe = b;
}

static const struct {
	uint16_t group;
	uint8_t id;
	uint8_t op;
} harmless[] = {
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_ECHO, MGMT_OP_READ},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_ECHO, MGMT_OP_WRITE},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_TASKSTAT, MGMT_OP_READ},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_MPSTAT, MGMT_OP_READ},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_MCUMGR_PARAMS, MGMT_OP_READ},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_INFO, MGMT_OP_READ},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_BOOTLOADER_INFO, MGMT_OP_READ},
}, guarded[] = {
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_DATETIME_STR, MGMT_OP_READ},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_DATETIME_STR, MGMT_OP_WRITE},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_CONS_ECHO_CTRL, MGMT_OP_WRITE},
	{MGMT_GROUP_ID_OS, OS_MGMT_ID_TASKSTAT, MGMT_OP_WRITE},
	{MGMT_GROUP_ID_STAT, 0, MGMT_OP_READ},
	{MGMT_GROUP_ID_SETTINGS, 0, MGMT_OP_READ},
	{MGMT_GROUP_ID_SETTINGS, 0, MGMT_OP_WRITE},
	{MGMT_GROUP_ID_FS, 0, MGMT_OP_READ},
	{MGMT_GROUP_ID_SHELL, 0, MGMT_OP_WRITE},
	{MGMT_GROUP_ID_ENUM, 0, MGMT_OP_READ},
	{64, 0, MGMT_OP_READ}, /* a board's own group */
	{64, 0, MGMT_OP_WRITE},
};

#define N(a) (sizeof(a) / sizeof((a)[0]))

static void test_gate(void)
{
	printf("  the gate on every group\n");
	CHECK(registered != NULL && registered->event_id == MGMT_EVT_OP_CMD_RECV);
	if (!registered) {
		return;
	}
	/* Set up, no proof: only the harmless status. */
	state(true, false, false, false);
	for (size_t i = 0; i < N(harmless); i++) {
		CHECK(cmd(harmless[i].group, harmless[i].id, harmless[i].op));
	}
	board_asked = 0;
	for (size_t i = 0; i < N(guarded); i++) {
		CHECK(!cmd(guarded[i].group, guarded[i].id, guarded[i].op));
	}
	CHECK(board_asked == 0); /* refused before the board is asked */
	CHECK(strstr(host_log_buf, "no token proof on this connection") != NULL);
	/* Proven: everything, each write then asked of the board. */
	state(true, true, false, false);
	board_asked = 0;
	for (size_t i = 0; i < N(guarded); i++) {
		CHECK(cmd(guarded[i].group, guarded[i].id, guarded[i].op));
	}
	CHECK(board_asked == 7); /* the writes among them */
	board_allows = false;
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	CHECK(cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_DATETIME_STR, MGMT_OP_READ)); /* reads aren't the board's */
	CHECK(strstr(host_log_buf, "the board's gate") != NULL);
	board_allows = true;
	/* Not set up: open, as with MG_DFU (no proof to ask). */
	state(false, false, false, false);
	for (size_t i = 0; i < N(guarded); i++) {
		CHECK(cmd(guarded[i].group, guarded[i].id, guarded[i].op));
	}
	/* SAFE the gadget entered itself, set up: only the harmless status,
	 * a proof flag notwithstanding (SAFE answers no token proof). */
	state(true, true, true, false);
	CHECK(cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_INFO, MGMT_OP_READ));
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	CHECK(!cmd(MGMT_GROUP_ID_STAT, 0, MGMT_OP_READ));
	CHECK(strstr(host_log_buf, "SAFE entered by the gadget itself") != NULL);
	/* SAFE asked with the button: open. */
	state(true, false, true, true);
	CHECK(cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	CHECK(cmd(MGMT_GROUP_ID_STAT, 0, MGMT_OP_READ));
	/* Every request counts as the peer's progress, refused or not. */
	CHECK(rx_requests > 0);
}

/* A request that came on a connection that isn't the link now is refused,
 * whatever the link now proved, before the board is asked. */
static void test_request_gen(void)
{
	printf("  a request whose connection is gone\n");
	host_conn_gen = 5;
	state(true, true, false, false); /* proven on the link now */
	CHECK(cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	/* Queued on connection 4, handled once 5 is the proven link: refused,
	 * the harmless status too. */
	gen_follows = false;
	req_gen = 4;
	board_asked = 0;
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_ECHO, MGMT_OP_READ));
	CHECK(board_asked == 0);
	CHECK(strstr(host_log_buf, "its connection is gone") != NULL);
	/* The same on a gadget that isn't set up, and in the button's SAFE. */
	state(false, false, false, false);
	CHECK(!cmd(MGMT_GROUP_ID_STAT, 0, MGMT_OP_READ));
	state(true, false, true, true);
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	/* One from before any connection, handled on one: refused. */
	host_conn_gen = 6;
	req_gen = 0;
	state(false, false, false, false);
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_ECHO, MGMT_OP_READ));
	/* No connection then or now: the gate decides as before. */
	host_conn_gen = 0;
	CHECK(cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	state(true, false, false, false);
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	CHECK(cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_ECHO, MGMT_OP_READ));
	/* Its link drops while it's decided and the next connection proves
	 * meanwhile: the proof read is the new link's, so it's refused. */
	host_conn_gen = 7;
	req_gen = 7;
	state(true, false, false, false);
	relink_at_proof = true;
	host_log_clear();
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	CHECK(strstr(host_log_buf, "its connection is gone") != NULL);
	relink_at_proof = false;
	/* The same while the board's gate decides. */
	req_gen = host_conn_gen;
	state(true, true, false, false);
	relink_at_board = true;
	CHECK(!cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	relink_at_board = false;
	/* Nothing changed meanwhile: it runs. */
	req_gen = host_conn_gen;
	CHECK(cmd(MGMT_GROUP_ID_OS, OS_MGMT_ID_RESET, MGMT_OP_WRITE));
	gen_follows = true;
}

/* The policy covers the update gate: nothing harmless is an update write.
 * (A release build's update gate also holds a gadget that isn't set up to
 * the local window: the board gate, which has no image group, doesn't.) */
static void test_covers_update_gate(void)
{
	printf("  covers the update gate\n");
	for (int s = 0; s < 64; s++) {
		bool su = s & 1, p = s & 2, sf = s & 4, b = s & 8, rel = s & 16, win = s & 32;

		for (uint16_t g = 0; g < 4; g++) {
			for (uint8_t id = 0; id < 10; id++) {
				for (uint8_t op = 0; op <= MGMT_OP_WRITE; op += 2) {
					if (mg_smp_gate_allowed(g, id, op, su, p, sf, b) &&
					    (su || !rel || win)) {
						CHECK(mg_ota_smp_allowed(g, id, op, p, sf, b, rel, win));
					}
				}
			}
		}
	}
}

HOST_MAIN(test_gate, test_request_gen, test_covers_update_gate)
