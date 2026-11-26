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

#ifndef MG_VM_LINK_H_
#define MG_VM_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_json.h"

/* The link's buffers, part of the device session's memory block
 * (mg_vm_session.c), which exists only while a session is open: what the VM
 * sent on /link-control that isn't a whole message yet, the control message
 * being written, and one request's parsed members. */
struct mg_vm_link_mem {
	uint8_t ctrl_in[CONFIG_MG_VM_LINK_CONTROL_MAX];
	char out_json[CONFIG_MG_VM_LINK_CONTROL_MAX];
	struct mg_json_obj msg_obj, sub_obj;
};

/*
 * The gadget's end of its VM session (mgcommands.h, Device session), what a
 * Wi-Fi gadget does over Noise (esp32/main/noise_control.cpp): when the
 * session opens it opens stream 1, POST /link-control, and sends
 * link.register with its identity and its device commands; it answers each
 * link.invoke with link.result, {"method":"link.result","id":...,"ok":true,
 * "payload":{...}} or "ok":false with an "error" string; after a
 * "registered" reply it sends link.heartbeat every
 * CONFIG_MG_VM_LINK_HEARTBEAT_S. A reset or refused /link-control ends a
 * Wi-Fi gadget's session; this one asks the client for a new session, with
 * the ESP32's reconnect backoff. Messages on /link-control are a u32-LE
 * length and compact JSON, in body chunks. Runs on the app work queue,
 * behind mg_vm_session.
 *
 * Before a handler runs, the request is checked against the command's
 * parameters: params an object (or absent), every required parameter
 * present and every declared one of its type; and while the send queue
 * might not hold its result it is answered "busy" instead. Handlers check
 * ranges. A handler runs on the app work queue and must return quickly.
 */

enum mg_vm_param_type {
	MG_VM_STRING,
	MG_VM_INTEGER,
	MG_VM_NUMBER,
	MG_VM_BOOLEAN,
	MG_VM_OBJECT,
	MG_VM_ARRAY,
};

struct mg_vm_param {
	const char *name;
	enum mg_vm_param_type type;
	const char *description; /* may be NULL */
	bool required;
};

/*
 * Runs a device command. params holds the request's params (checked; empty
 * when absent); the handler writes its payload's members into payload, an
 * open object. Returns 0, or nonzero with err filled in (mg_vm_fail): the
 * result's error string.
 */
typedef int (*mg_vm_command_handler_t)(const struct mg_json_obj *params, struct mg_jw *payload,
				       char *err, size_t err_len);

/* A link.register command entry, plus its name and handler. */
struct mg_vm_command {
	const char *name; /* <domain>.<verb>: lowercase ASCII, digits, '_' */
	const char *description;
	const struct mg_vm_param *params;
	size_t n_params;
	uint32_t timeout_ms; /* 0: left out */
	mg_vm_command_handler_t handler;
};

/* What link.register says about the gadget besides its commands. */
struct mg_vm_identity {
	const char *node_id; /* the device key's node id (also in credentials) */
	const char *display_name;
	const char *platform;
	const char *version;
	const char *device_family;
	const char *model_id;
	/* The product name the agent names the gadget by (link.register
	 * metadata.model_name, e.g. "XN54"); NULL or
	 * empty: no metadata. */
	const char *model_name;
	/* link.register carries "is_wakeup_supported": true: the board
	 * handles mg_session_wake (none on this app; the vectors' reference
	 * device does). */
	bool is_wakeup_supported;
};

/* Fills err and returns -1, for a handler's return statement. */
int mg_vm_fail(char *err, size_t err_len, const char *fmt, ...);

/* Adds a device command (kept by pointer). -EINVAL for a bad name or a
 * missing handler, -EEXIST for a duplicate, -ENOMEM past
 * CONFIG_MG_VM_LINK_COMMANDS_MAX. */
int mg_vm_link_add_command(const struct mg_vm_command *cmd);
/* Kept by pointer. */
void mg_vm_link_set_identity(const struct mg_vm_identity *id);
const char *mg_vm_link_node_id(void);

/* From mg_vm_session: the session opened (a new epoch) or ended. */
void mg_vm_link_open(void);
void mg_vm_link_close(void);
/* The connection ended: nothing waits for a next session any more. */
void mg_vm_link_disconnected(void);
/* One whole message from the VM: a ServiceResponseEnvelope. */
void mg_vm_link_rx(const uint8_t *msg, size_t len);

/* link.register's reply said "registered". */
bool mg_vm_link_registered(void);
/* VM traffic owed while the session is down (an active task). */
bool mg_vm_link_owes(void);
/* Host tests: the keepalive's interval (0: off). */
void mg_vm_link_test_keepalive(uint32_t ms);

void mg_vm_link_init(void);
/* A test trigger for the phone's relay: one control message
 * ({"method":"link.heartbeat"}) on /link-control now (0), or, with no
 * session open, at the next open within 10 s (1). */
int mg_vm_link_send_test(void);
/* The board's device commands and identity (mg_vm_commands.c). */
int mg_vm_commands_init(void);
/* Tests: forget every command and the identity. */
void mg_vm_link_test_clear(void);

#endif
