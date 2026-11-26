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

/* Host shim: MCUmgr's command callback, as Zephyr declares it; the test
 * calls what mg_smp_gate.c registered. */

#ifndef HOST_SHIM_MGMT_CALLBACKS_H_
#define HOST_SHIM_MGMT_CALLBACKS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mgmt_cb_return {
	MGMT_CB_OK,
	MGMT_CB_ERROR_RC,
	MGMT_CB_ERROR_ERR,
};

#define MGMT_EVT_OP_CMD_RECV 0x00010001u

struct mgmt_evt_op_cmd_arg {
	uint16_t group;
	uint8_t id;
	union {
		uint8_t op;
		int err;
	};
};

typedef enum mgmt_cb_return (*mgmt_cb)(uint32_t event, enum mgmt_cb_return prev_status,
				       int32_t *rc, uint16_t *group, bool *abort_more, void *data,
				       size_t data_size);

struct mgmt_callback {
	mgmt_cb callback;
	uint32_t event_id;
};

void mgmt_callback_register(struct mgmt_callback *callback);

#endif
