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

#include "mg_vm_link.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "mg_envelope.h"
#include "mg_transport.h"
#include "mg_vm_session.h"

LOG_MODULE_REGISTER(mg_vm_link, CONFIG_MG_LOG_LEVEL);

/* esp32/main/noise_control.cpp's control stream and register request id. */
#define CTRL_STREAM 1
#define REGISTER_ID "reg-1"
#define CONTROL_MAX CONFIG_MG_VM_LINK_CONTROL_MAX
#define NAME_MAX_LEN 48
/* A body chunk's envelope and frame before its bytes. */
#define PREFIX_MAX 24
/* The most one control message takes in the session's send queue. */
#define CONTROL_ROOM (PREFIX_MAX + 4 + CONTROL_MAX)

/* After a failed /link-control the gadget asks for a new session this long
 * after, doubling up to the most, as esp32/main/noise_control.cpp
 * reconnects (RECONNECT_MIN_MS, RECONNECT_MAX_MS). */
#define REOPEN_MIN_MS 1000
#define REOPEN_MAX_MS 15000
/* The backoff starts over only after a /link-control that stayed up this
 * long past `registered` (a server that registers and
 * then ends at once must not be asked again every second forever). */
#define REOPEN_STABLE_MS 60000

BUILD_ASSERT(CONTROL_ROOM + 4 <= CONFIG_MG_VM_SESSION_TX_QUEUE,
	     "a control message must fit the session's send queue");

static const struct mg_vm_command *commands[CONFIG_MG_VM_LINK_COMMANDS_MAX];
static size_t n_commands;
static const struct mg_vm_identity *ident;

static struct {
	bool open;       /* the session is open and /link-control was opened */
	bool registered; /* link.register's reply said registered */
	bool failed;     /* the VM refused or reset /link-control */
	size_t ctrl_len; /* bytes in ctrl_in */
	uint32_t skip;   /* bytes left of a control message too long to keep */
} l;
static int64_t registered_at; /* when link.register was answered registered (0: not) */

/* The buffers (struct mg_vm_link_mem) are the session's: the link runs only
 * while a session is open, and its memory is allocated then. */
#define ctrl_in  (mg_vm_session_link_mem()->ctrl_in)
#define out_json (mg_vm_session_link_mem()->out_json)
#define msg_obj  (mg_vm_session_link_mem()->msg_obj)
#define sub_obj  (mg_vm_session_link_mem()->sub_obj)

static struct k_work_delayable heartbeat_work;
static struct k_work_delayable reopen_work;
/* The keepalive: an empty frame on /link-control while a task is active
 * (CONFIG_MG_VM_KEEPALIVE_MS; host tests set it per test). */
static struct k_work_delayable keepalive_work;
static uint32_t keepalive_ms = CONFIG_MG_VM_KEEPALIVE_MS;
/* The tick looks every TICK_MS while the session is open: the
 * keepalive while a task is active, the idle close after none for
 * CONFIG_MG_VM_IDLE_CLOSE_MS. Both off with the keepalive (host tests). */
#define TICK_MS 5000
static int64_t last_keepalive, last_task;
static uint32_t reopen_ms = REOPEN_MIN_MS;

static const char *const TYPE_NAMES[] = {"string",  "integer", "number",
					 "boolean", "object",  "array"};
static const char *const TYPE_ARTICLES[] = {"a string",  "an integer", "a number",
					    "a boolean", "an object",  "an array"};

int mg_vm_fail(char *err, size_t err_len, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err, err_len, fmt, ap);
	va_end(ap);
	return -1;
}

static bool valid_name(const char *name)
{
	size_t n = name ? strlen(name) : 0;
	bool dot = false;

	if (n == 0 || n > NAME_MAX_LEN || name[0] == '.' || name[n - 1] == '.') {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		char ch = name[i];

		if (ch == '.') {
			if (name[i + 1] == '.') {
				return false;
			}
			dot = true;
		} else if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
			return false;
		}
	}
	return dot;
}

int mg_vm_link_add_command(const struct mg_vm_command *c)
{
	if (c == NULL || !valid_name(c->name) || c->handler == NULL || c->description == NULL) {
		return -EINVAL;
	}
	for (size_t i = 0; i < c->n_params; i++) {
		if (c->params[i].name == NULL || c->params[i].type > MG_VM_ARRAY) {
			return -EINVAL;
		}
	}
	for (size_t i = 0; i < n_commands; i++) {
		if (strcmp(commands[i]->name, c->name) == 0) {
			return -EEXIST;
		}
	}
	if (n_commands >= ARRAY_SIZE(commands)) {
		return -ENOMEM;
	}
	commands[n_commands++] = c;
	return 0;
}

void mg_vm_link_set_identity(const struct mg_vm_identity *id)
{
	ident = id;
}

const char *mg_vm_link_node_id(void)
{
	return ident && ident->node_id ? ident->node_id : "";
}

static const char *ident_str(const char *s)
{
	return s ? s : "";
}

bool mg_vm_link_registered(void)
{
	return l.registered;
}

/* VM-supplied text for the log, which the Nordic UART mirrors: printable
 * ASCII only, others as '?', at most out_len - 1 bytes. */
static const char *log_safe(const char *in, size_t n, char *out, size_t out_len)
{
	size_t k = 0;

	for (size_t i = 0; in != NULL && i < n && in[i] != '\0' && k + 1 < out_len; i++) {
		out[k++] = in[i] >= 0x20 && in[i] < 0x7f ? in[i] : '?';
	}
	out[k] = '\0';
	return out;
}

/* ------------------------------------------------------------------------
 * Sending
 * ---------------------------------------------------------------------- */

/* One control message: a body chunk on /link-control carrying the length
 * and the JSON. */
static int send_control(const char *json, size_t len)
{
	uint8_t prefix[PREFIX_MAX];
	uint8_t le[4] = {(uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16),
			 (uint8_t)(len >> 24)};
	int n = mg_env_body_chunk_request_prefix(MG_ENV_DAEMON, CTRL_STREAM, sizeof(le) + len,
						 prefix, sizeof(prefix));
	const struct mg_vm_iov iov[] = {{prefix, n > 0 ? (size_t)n : 0}, {le, 4}, {json, len}};

	return n < 0 ? n : mg_vm_session_sendv(iov, ARRAY_SIZE(iov));
}

static void write_params(struct mg_jw *w, const struct mg_vm_command *c, bool required)
{
	mg_jw_obj(w, required ? "required" : "optional");
	for (size_t i = 0; i < c->n_params; i++) {
		const struct mg_vm_param *p = &c->params[i];

		if (p->required != required) {
			continue;
		}
		mg_jw_obj(w, p->name);
		mg_jw_str(w, "type", TYPE_NAMES[p->type]);
		if (p->description) {
			mg_jw_str(w, "description", p->description);
		}
		mg_jw_close(w);
	}
	mg_jw_close(w);
}

static int send_register(void)
{
	struct mg_jw w;
	int n;

	mg_jw_begin(&w, out_json, sizeof(out_json));
	mg_jw_str(&w, "type", "req");
	mg_jw_str(&w, "id", REGISTER_ID);
	mg_jw_str(&w, "method", "link.register");
	mg_jw_obj(&w, "params");
	mg_jw_str(&w, "node_id", mg_vm_link_node_id());
	mg_jw_str(&w, "display_name", ident_str(ident ? ident->display_name : NULL));
	mg_jw_str(&w, "platform", ident_str(ident ? ident->platform : NULL));
	mg_jw_str(&w, "version", ident_str(ident ? ident->version : NULL));
	mg_jw_str(&w, "device_family", ident_str(ident ? ident->device_family : NULL));
	mg_jw_str(&w, "model_id", ident_str(ident ? ident->model_id : NULL));
	if (ident && ident->model_name && ident->model_name[0]) {
		/* The agent names the gadget in hand from the registry. */
		mg_jw_obj(&w, "metadata");
		mg_jw_str(&w, "model_name", ident->model_name);
		mg_jw_close(&w);
	}
	if (ident && ident->is_wakeup_supported) {
		mg_jw_bool(&w, "is_wakeup_supported", true);
	}
	mg_jw_obj(&w, "commands_v2"); /* as the ESP32's noise_control.cpp and the reference */
	for (size_t i = 0; i < n_commands; i++) {
		const struct mg_vm_command *c = commands[i];

		mg_jw_obj(&w, c->name);
		mg_jw_str(&w, "description", c->description);
		write_params(&w, c, true);
		write_params(&w, c, false);
		if (c->timeout_ms) {
			mg_jw_int(&w, "timeout_ms", c->timeout_ms);
		}
		mg_jw_close(&w);
	}
	n = mg_jw_end(&w);
	if (n < 0) {
		LOG_ERR("mg.link: link.register doesn't fit %d bytes", CONTROL_MAX);
		return n;
	}
	LOG_INF("mg.link: link.register as %s, %u command(s)", mg_vm_link_node_id(),
		(unsigned int)n_commands);
	return send_control(out_json, (size_t)n);
}

/* A request the device makes while the session isn't open waits for the
 * next open, at most WAIT_OPEN_MS, rather than being dropped
 * (mgcommands.h, Session state: a suspended iOS app ends its sessions
 * at each wake). */
#define WAIT_OPEN_MS 10000
static int64_t hb_waiting;   /* a heartbeat came due while closed (uptime ms, 0 none) */
static int64_t test_waiting; /* the bench's test message, the same */
/* Nothing is re-sent after a session the client closed: only GET and HEAD
 * would be, and the gadget makes neither (mgcommands.h, Session state); a
 * heartbeat lost with a session is followed by the next one, and every open
 * sends link.register. */

static int send_heartbeat(const char *why)
{
	static const char hb[] = "{\"method\":\"link.heartbeat\"}";
	int err = send_control(hb, sizeof(hb) - 1);

	if (err) {
		LOG_WRN("mg.link: heartbeat not sent");
	} else {
		LOG_INF("mg.link: heartbeat%s", why);
	}
	return err;
}

static void heartbeat_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!l.open || !l.registered) {
		/* Due while closed: sent at the next open if it comes within
		 * WAIT_OPEN_MS. */
		if (hb_waiting == 0) {
			hb_waiting = k_uptime_get();
			mg_vm_session_reason(); /* owed traffic: a reason to ask */
		}
		return;
	}
	(void)send_heartbeat("");
	k_work_reschedule_for_queue(mg_app_wq(), &heartbeat_work,
				    K_SECONDS(CONFIG_MG_VM_LINK_HEARTBEAT_S));
}

/* At a session's open, behind link.register: what waited for it. */
static void send_waiting(void)
{
	static const char hb[] = "{\"method\":\"link.heartbeat\"}";
	int64_t now = k_uptime_get();

	if (hb_waiting) {
		if (now - hb_waiting <= WAIT_OPEN_MS) {
			(void)send_heartbeat(" (waited for the session)");
		} else {
			LOG_INF("mg.link: a heartbeat due while closed dropped (%lld ms)",
				(long long)(now - hb_waiting));
		}
		hb_waiting = 0;
	}
	if (test_waiting) {
		if (now - test_waiting <= WAIT_OPEN_MS) {
			int err = send_control(hb, sizeof(hb) - 1);

			LOG_INF("mg.link: out: test link.heartbeat on /link-control (%u B, waited "
				"for the session)%s",
				(unsigned int)(sizeof(hb) - 1), err ? ": not sent" : "");
		}
		test_waiting = 0;
	}
}

/* VM traffic owed while the session is down (an active task):
 * link.register not yet answered on this connection, or a
 * request waiting for the next open within WAIT_OPEN_MS. */
bool mg_vm_link_owes(void)
{
	int64_t now = k_uptime_get();

	/* An unregistered link alone owes nothing (mgcommands.h,
	 * "wake": registered commands alone make the device ask nothing). */
	return (hb_waiting && now - hb_waiting <= WAIT_OPEN_MS) ||
	       (test_waiting && now - test_waiting <= WAIT_OPEN_MS);
}

/* An empty body chunk on the open /link-control stream: the VM reads no
 * message from it, and on iOS it keeps a backgrounded app awake (never a new
 * POST, which would be a second link stream for the node). */
static int send_keepalive(void)
{
	const struct mg_env_frame f = {
		.stream_id = CTRL_STREAM,
		.kind = MG_ENV_BODY_CHUNK,
	};
	uint8_t msg[16];
	int n = mg_env_encode_request_message(MG_ENV_DAEMON, &f, msg, sizeof(msg));

	if (n > 0) {
		const struct mg_vm_iov iov = {msg, (size_t)n};

		n = mg_vm_session_sendv(&iov, 1);
	}
	return n < 0 ? n : 0;
}

static void keepalive_fn(struct k_work *work)
{
	int64_t now = k_uptime_get();

	ARG_UNUSED(work);
	if (!l.open || keepalive_ms == 0) {
		return;
	}
	if (mg_vm_session_task_active()) {
		last_task = now;
		if (now - last_keepalive >= keepalive_ms) {
			last_keepalive = now;
			if (send_keepalive() == 0) {
				LOG_DBG("mg.link: keepalive");
			}
		}
	} else if (now - last_task >= CONFIG_MG_VM_IDLE_CLOSE_MS) {
		/* No silent session left open. */
		mg_vm_session_close_idle();
		return;
	}
	k_work_reschedule_for_queue(mg_app_wq(), &keepalive_work,
				    K_MSEC(MIN(TICK_MS, keepalive_ms)));
}

void mg_vm_link_test_keepalive(uint32_t ms)
{
	keepalive_ms = ms;
	if (ms == 0) {
		(void)k_work_cancel_delayable(&keepalive_work);
	} else if (l.open) {
		last_keepalive = last_task = k_uptime_get();
		k_work_reschedule_for_queue(mg_app_wq(), &keepalive_work,
					    K_MSEC(MIN(TICK_MS, ms)));
	}
}

void mg_vm_link_open(void)
{
	static const char verb[] = "POST", path[] = "/link-control";
	const struct mg_env_frame f = {
		.stream_id = CTRL_STREAM,
		.kind = MG_ENV_REQUEST,
		.verb = {(const uint8_t *)verb, sizeof(verb) - 1},
		.path = {(const uint8_t *)path, sizeof(path) - 1},
	};
	uint8_t msg[32];
	int n;

	mg_vm_link_close();
	l.open = true;
	/* What a Wi-Fi gadget does once its Noise session is up. */
	n = mg_env_encode_request_message(MG_ENV_DAEMON, &f, msg, sizeof(msg));
	if (n > 0) {
		const struct mg_vm_iov iov = {msg, (size_t)n};

		n = mg_vm_session_sendv(&iov, 1);
	}
	if (n < 0 || send_register()) {
		LOG_ERR("mg.link: couldn't open /link-control (%d)", n);
		return;
	}
	send_waiting(); /* behind link.register, what waited for the session */
	if (keepalive_ms) {
		last_keepalive = last_task = k_uptime_get();
		k_work_reschedule_for_queue(mg_app_wq(), &keepalive_work,
					    K_MSEC(MIN(TICK_MS, keepalive_ms)));
	}
}

void mg_vm_link_close(void)
{
	/* The heartbeat timer runs on: one due while closed waits for the next
	 * open (send_waiting). */
	(void)k_work_cancel_delayable(&reopen_work);
	(void)k_work_cancel_delayable(&keepalive_work);
	memset(&l, 0, sizeof(l));
}

void mg_vm_link_disconnected(void)
{
	(void)k_work_cancel_delayable(&heartbeat_work);
	hb_waiting = test_waiting = 0;
}

static void reopen_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (l.open && l.failed) {
		mg_vm_session_request_reopen();
	}
}

/* A Wi-Fi gadget treats a refused or reset /link-control as the end of its
 * session and reconnects (esp32/main/noise_control.cpp): without it the
 * gadget's commands are unreachable. This one asks the client for a new
 * session, with the same backoff. */
static void control_failed(void)
{
	if (l.failed) {
		return;
	}
	if (l.registered && registered_at &&
	    k_uptime_get() - registered_at >= REOPEN_STABLE_MS) {
		reopen_ms = REOPEN_MIN_MS; /* it was a working link: start over */
	}
	registered_at = 0;
	l.failed = true;
	l.registered = false;
	(void)k_work_cancel_delayable(&heartbeat_work);
	LOG_WRN("mg.link: asking for a new session in %u ms", (unsigned int)reopen_ms);
	k_work_reschedule_for_queue(mg_app_wq(), &reopen_work, K_MSEC(reopen_ms));
	reopen_ms = MIN(reopen_ms * 2, REOPEN_MAX_MS);
}

/* ------------------------------------------------------------------------
 * link.invoke
 * ---------------------------------------------------------------------- */

static const struct mg_vm_command *find(const char *name)
{
	for (size_t i = 0; i < n_commands; i++) {
		if (strcmp(commands[i]->name, name) == 0) {
			return commands[i];
		}
	}
	return NULL;
}

static bool type_ok(enum mg_vm_param_type t, const struct mg_json_field *f)
{
	switch (t) {
	case MG_VM_STRING:
		return f->type == MG_JSON_STRING;
	case MG_VM_INTEGER:
		return f->type == MG_JSON_NUMBER && f->num_is_int;
	case MG_VM_NUMBER:
		return f->type == MG_JSON_NUMBER;
	case MG_VM_BOOLEAN:
		return f->type == MG_JSON_TRUE || f->type == MG_JSON_FALSE;
	case MG_VM_OBJECT:
		return f->type == MG_JSON_NESTED && !f->is_array;
	case MG_VM_ARRAY:
		return f->type == MG_JSON_NESTED && f->is_array;
	}
	return true;
}

/* Checks params (sub_obj) against the command: 0, or -1 with err set. */
static int check_params(const struct mg_vm_command *c, char *err, size_t err_len)
{
	for (int pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < c->n_params; i++) {
			const struct mg_vm_param *p = &c->params[i];
			const struct mg_json_field *f;

			if (p->required != (pass == 0)) {
				continue;
			}
			f = mg_json_get(&sub_obj, p->name);
			if (f == NULL) {
				if (p->required) {
					return mg_vm_fail(err, err_len, "missing parameter %s",
							  p->name);
				}
				continue;
			}
			if (!type_ok(p->type, f)) {
				return mg_vm_fail(err, err_len, "parameter %s must be %s", p->name,
						  TYPE_ARTICLES[p->type]);
			}
		}
	}
	return 0;
}

static void on_invoke(void)
{
	const char *id = mg_json_str(&msg_obj, "id");
	const char *name = mg_json_str(&msg_obj, "command");
	const struct mg_json_field *pf = mg_json_get(&msg_obj, "params");
	const struct mg_vm_command *c;
	char err[96] = "";
	struct mg_jw w = {0};
	int ok = 0, n;

	if (id == NULL) {
		return; /* nothing to answer to */
	}
	if (name == NULL) {
		name = "";
	}
	sub_obj.n = 0;
	c = find(name);
	if (c == NULL) {
		snprintf(err, sizeof(err), "no command named %.*s", NAME_MAX_LEN + 8, name);
	} else if (pf != NULL && pf->type != MG_JSON_NULL &&
		   (pf->type != MG_JSON_NESTED || pf->is_array ||
		    mg_json_parse(pf->raw, pf->raw_len, &sub_obj))) {
		snprintf(err, sizeof(err), "params must be an object");
	} else if (check_params(c, err, sizeof(err)) != 0) {
		/* err says which. */
	} else if (mg_vm_session_tx_room() < CONTROL_ROOM) {
		/* Not run: its result might not fit the send queue (the client
		 * isn't giving credit), and the VM would see a timeout for a
		 * command that ran. */
		snprintf(err, sizeof(err), "busy");
	} else {
		mg_jw_begin(&w, out_json, sizeof(out_json));
		mg_jw_str(&w, "method", "link.result");
		mg_jw_str(&w, "id", id);
		mg_jw_bool(&w, "ok", true);
		mg_jw_obj(&w, "payload");
		ok = c->handler(&sub_obj, &w, err, sizeof(err)) == 0;
		if (!ok && err[0] == '\0') {
			snprintf(err, sizeof(err), "%s failed", c->name);
		}
	}
	n = ok ? mg_jw_end(&w) : -1;
	if (ok && n < 0) {
		ok = 0;
		snprintf(err, sizeof(err), "result too large");
	}
	if (!ok) {
		mg_jw_begin(&w, out_json, sizeof(out_json));
		mg_jw_str(&w, "method", "link.result");
		mg_jw_str(&w, "id", id);
		mg_jw_bool(&w, "ok", false);
		mg_jw_str(&w, "error", err);
		n = mg_jw_end(&w);
	}
	/* The command's name and outcome only: params may carry user text, and
	 * the log is mirrored on the Nordic UART service. */
	char safe[NAME_MAX_LEN + 24];

	if (ok) {
		LOG_INF("mg.link: %s ok", c->name);
	} else {
		LOG_INF("mg.link: %s failed: %s", c ? c->name : "unknown command",
			log_safe(err, sizeof(err), safe, sizeof(safe)));
	}
	if (n < 0 || send_control(out_json, (size_t)n)) {
		LOG_WRN("mg.link: result for %s dropped",
			log_safe(name, NAME_MAX_LEN, safe, sizeof(safe)));
	}
}

/* ------------------------------------------------------------------------
 * Receiving
 * ---------------------------------------------------------------------- */

/* The link.register reply (esp32/main/noise_control.cpp): an error is
 * logged; "registered" starts the heartbeat. */
static void on_register_reply(void)
{
	const struct mg_json_field *e = mg_json_get(&msg_obj, "error");
	const struct mg_json_field *r = mg_json_get(&msg_obj, "result");
	const char *type = mg_json_str(&msg_obj, "type");
	const char *status = NULL;

	if (e != NULL && e->type != MG_JSON_NULL && e->type != MG_JSON_FALSE) {
		char safe[64];

		LOG_ERR("mg.link: link.register refused: %s",
			e->type == MG_JSON_STRING ? log_safe(e->str, strlen(e->str), safe, sizeof(safe))
						  : "(see the VM's log)");
		return;
	}
	if (r != NULL && r->type == MG_JSON_NESTED && !r->is_array &&
	    mg_json_parse(r->raw, r->raw_len, &sub_obj) == 0) {
		status = mg_json_str(&sub_obj, "status");
	}
	if (type == NULL || strcmp(type, "res") != 0 || status == NULL ||
	    strcmp(status, "registered") != 0) {
		LOG_WRN("mg.link: link.register answered without \"registered\"");
		return;
	}
	l.registered = true;
	registered_at = k_uptime_get(); /* the backoff starts over once this lasts */
	LOG_INF("mg.link: registered");
	k_work_reschedule_for_queue(mg_app_wq(), &heartbeat_work,
				    K_SECONDS(CONFIG_MG_VM_LINK_HEARTBEAT_S));
}

static void on_control(char *json, size_t len)
{
	const char *type, *method, *id, *event;

	if (mg_json_parse(json, len, &msg_obj)) {
		LOG_WRN("mg.link: a control message that isn't a JSON object (%u B)",
			(unsigned int)len);
		return;
	}
	type = mg_json_str(&msg_obj, "type");
	method = mg_json_str(&msg_obj, "method");
	id = mg_json_str(&msg_obj, "id");
	if (type != NULL && strcmp(type, "event") == 0) {
		event = mg_json_str(&msg_obj, "event");
		if (event != NULL &&
		    (strcmp(event, "link.unpaired") == 0 || strcmp(event, "node.unpaired") == 0)) {
			LOG_WRN("mg.link: the VM unpaired this gadget");
		}
		return;
	}
	if (!l.registered && id != NULL && strcmp(id, REGISTER_ID) == 0) {
		on_register_reply();
		return;
	}
	if (method != NULL && strcmp(method, "link.invoke") == 0) {
		on_invoke();
		return;
	}
	/* Anything else is taken and logged (the relay's placeholder). */
	LOG_INF("mg.link: in: control message type %s, method %s (%u B): no handler",
		type ? type : "-", method ? method : "-", (unsigned int)len);
}

/* Body bytes of /link-control: u32-LE length, JSON, ... */
static void ctrl_feed(const uint8_t *data, size_t len)
{
	while (len > 0) {
		size_t k;

		if (l.skip) {
			k = MIN((size_t)l.skip, len);
			l.skip -= k;
			data += k;
			len -= k;
			continue;
		}
		k = MIN(len, sizeof(ctrl_in) - l.ctrl_len);
		memcpy(&ctrl_in[l.ctrl_len], data, k);
		l.ctrl_len += k;
		data += k;
		len -= k;
		while (l.ctrl_len >= 4) {
			uint32_t n = (uint32_t)ctrl_in[0] | (uint32_t)ctrl_in[1] << 8 |
				     (uint32_t)ctrl_in[2] << 16 | (uint32_t)ctrl_in[3] << 24;

			if (n > sizeof(ctrl_in) - 4) {
				LOG_WRN("mg.link: a %u B control message: too long, skipped", n);
				l.skip = n - (uint32_t)(l.ctrl_len - 4);
				l.ctrl_len = 0;
				break;
			}
			if (l.ctrl_len < 4 + n) {
				break;
			}
			if (n > 0) {
				on_control((char *)&ctrl_in[4], n);
				if (!l.open) {
					return; /* the session ended meanwhile */
				}
			}
			memmove(ctrl_in, &ctrl_in[4 + n], l.ctrl_len - 4 - n);
			l.ctrl_len -= 4 + n;
		}
	}
}

void mg_vm_link_rx(const uint8_t *msg, size_t len)
{
	struct mg_env_bytes payload;
	struct mg_env_frame f;

	if (mg_env_decode_response_envelope(msg, len, &payload) ||
	    mg_env_decode_frame(payload.p, payload.n, NULL, 0, &f)) {
		LOG_WRN("mg.link: a %u B message that isn't a service frame", (unsigned int)len);
		return;
	}
	if (!l.open) {
		return;
	}
	if (f.stream_id != CTRL_STREAM) {
		/* No other streams yet: a Wi-Fi gadget's GET /fs/raw and the
		 * like would be routed here. Taken and logged (the relay's
		 * placeholder): its bytes are given back like any other. */
		LOG_INF("mg.link: in: frame kind %d on stream %lld, %u B body: no handler yet",
			(int)f.kind, (long long)f.stream_id, (unsigned int)f.body.n);
		return;
	}
	switch (f.kind) {
	case MG_ENV_RESPONSE:
		if (f.status == 200 && f.end_body) {
			/* As the ESP32 (noise_control.cpp): a 200 that ends
			 * the body ends the control stream; the client reports
			 * closed and the session's re-ask reopens. */
			LOG_INF("mg.link: /link-control open");
			LOG_WRN("mg.link: the VM ended /link-control in its response");
		} else if (f.status == 200) {
			LOG_INF("mg.link: /link-control open");
			/* The response's inline body carries control messages
			 * too (the ESP32 reads them the same way). */
			if (f.body.n) {
				ctrl_feed(f.body.p, f.body.n);
			}
		} else {
			LOG_ERR("mg.link: the VM refused /link-control (%d)", (int)f.status);
			control_failed();
		}
		break;
	case MG_ENV_BODY_CHUNK:
		ctrl_feed(f.body.p, f.body.n);
		if (f.end_body && l.open) {
			/* A clean end: the client reports closed and the
			 * session's re-ask reopens (one
			 * reopen path, not two racing). */
			LOG_WRN("mg.link: the VM ended /link-control");
		}
		break;
	case MG_ENV_RESET:
		LOG_WRN("mg.link: the VM reset /link-control (code %d)", (int)f.reset_code);
		control_failed();
		break;
	default:
		LOG_INF("mg.link: in: frame kind %d on /link-control, %u B: no handler yet",
			(int)f.kind, (unsigned int)f.body.n);
		break;
	}
}

int mg_vm_link_send_test(void)
{
	static const char hb[] = "{\"method\":\"link.heartbeat\"}";
	int err;

	if (!l.open) {
		/* Waits for the next session (WAIT_OPEN_MS at most). */
		test_waiting = k_uptime_get();
		LOG_INF("mg.link: test link.heartbeat waits for the session");
		mg_vm_session_reason(); /* owed traffic: a reason to ask */
		return 1;
	}
	err = send_control(hb, sizeof(hb) - 1);
	LOG_INF("mg.link: out: test link.heartbeat on /link-control (%u B)%s",
		(unsigned int)(sizeof(hb) - 1), err ? ": not sent" : "");
	return err;
}

void mg_vm_link_init(void)
{
	k_work_init_delayable(&heartbeat_work, heartbeat_fn);
	k_work_init_delayable(&reopen_work, reopen_fn);
	k_work_init_delayable(&keepalive_work, keepalive_fn);
}

void mg_vm_link_test_clear(void)
{
	mg_vm_link_close();
	n_commands = 0;
	ident = NULL;
	reopen_ms = REOPEN_MIN_MS;
	registered_at = 0;
}
