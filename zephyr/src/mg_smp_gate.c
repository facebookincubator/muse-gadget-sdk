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
 * SMP over Bluetooth without CONFIG_MG_DFU (CONFIG_MG_SMP_GATE_BOARD): a board
 * whose images go through its own loader keeps MCUmgr's other groups. On a
 * set-up gadget every SMP command, read or write, needs this connection's
 * token proof (or SAFE asked with the button), but for the OS group's
 * harmless status (mg_smp_gate_allowed(), mg_ota_policy.h), and for a
 * request the board takes on itself (mg_board_smp_gate_defers(), weak, false
 * by default); then each write meets the board's own
 * mg_board_smp_write_allowed(), which has no default: a build without it
 * fails to link. The proof counts only for a request that
 * came on the connection that's the link now (mg_smp_request_gen(),
 * src/mg_smp_req_gen.c: the connection of the request's first fragment),
 * checked again after the proof and the board's gate are read: one still
 * queued when its link dropped, or whose link dropped while it was decided,
 * is refused, whatever a later connection proved.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h>

#include "mg_app.h"
#include "mg_boot.h"
#include "mg_ota_policy.h"
#include "mg_setup_store.h"
#include "mg_token_proof.h"
#include "mg_transport.h"

LOG_MODULE_REGISTER(mg_smp_gate, CONFIG_MG_LOG_LEVEL);

BUILD_ASSERT(MG_SMP_OP_WRITE == MGMT_OP_WRITE && MG_SMP_OP_READ == MGMT_OP_READ &&
		     MG_SMP_GROUP_OS == MGMT_GROUP_ID_OS &&
		     MG_SMP_GROUP_IMAGE == MGMT_GROUP_ID_IMAGE,
	     "mg_ota_policy.h names MCUmgr's ids");
BUILD_ASSERT(MG_SMP_OS_ECHO == OS_MGMT_ID_ECHO && MG_SMP_OS_TASKSTAT == OS_MGMT_ID_TASKSTAT &&
		     MG_SMP_OS_MPSTAT == OS_MGMT_ID_MPSTAT &&
		     MG_SMP_OS_MCUMGR_PARAMS == OS_MGMT_ID_MCUMGR_PARAMS &&
		     MG_SMP_OS_INFO == OS_MGMT_ID_INFO &&
		     MG_SMP_OS_BOOTLOADER_INFO == OS_MGMT_ID_BOOTLOADER_INFO &&
		     MG_SMP_OS_RESET == OS_MGMT_ID_RESET,
	     "mg_ota_policy.h names the OS group's ids");

__attribute__((weak)) bool mg_board_smp_gate_defers(uint16_t group, uint8_t id, uint8_t op)
{
	ARG_UNUSED(group);
	ARG_UNUSED(id);
	ARG_UNUSED(op);
	return false;
}

static enum mgmt_cb_return on_cmd(uint32_t event, enum mgmt_cb_return prev, int32_t *rc,
				  uint16_t *group, bool *abort_more, void *data, size_t size)
{
	const struct mgmt_evt_op_cmd_arg *cmd = data;
	/* The request's own connection, and the link now. */
	uint32_t gen = mg_smp_request_gen();
	const char *why;

	ARG_UNUSED(event);
	ARG_UNUSED(prev);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	if (size < sizeof(*cmd)) {
		return MGMT_CB_OK;
	}
	mg_transport_rx_request(); /* a client sends more only once answered: TX progress */
	if (gen != mg_transport_conn_gen()) {
		/* It came on a connection that's gone (or before any): no proof of
		 * another connection counts for it. */
		why = "its connection is gone";
	} else if (!mg_smp_gate_allowed(cmd->group, cmd->id, cmd->op, mg_setup_store_complete(),
				 mg_token_proof_matched(), mg_health_safe(),
				 mg_health_safe_by_button()) &&
		   !mg_board_smp_gate_defers(cmd->group, cmd->id, cmd->op)) {
		why = mg_health_safe() ? "SAFE entered by the gadget itself" :
					 "no token proof on this connection";
	} else if (cmd->op == MGMT_OP_WRITE &&
		   !mg_board_smp_write_allowed(cmd->group, cmd->id)) {
		why = "the board's gate";
	} else if (gen != mg_transport_conn_gen()) {
		/* Its link changed while it was decided: the proof read may be the
		 * next connection's (still its own link here, the proof was its own). */
		why = "its connection is gone";
	} else {
		return MGMT_CB_OK;
	}
	LOG_WRN("mg.smp: SMP %s (group %u id %u) refused: %s",
		cmd->op == MGMT_OP_WRITE ? "write" : "read", cmd->group, cmd->id, why);
	*rc = MGMT_ERR_EACCESSDENIED;
	return MGMT_CB_ERROR_RC;
}

static struct mgmt_callback cmd_cb = {
	.callback = on_cmd,
	.event_id = MGMT_EVT_OP_CMD_RECV,
};

static int mg_smp_gate_init(void)
{
	mgmt_callback_register(&cmd_cb);
	return 0;
}
SYS_INIT(mg_smp_gate_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
