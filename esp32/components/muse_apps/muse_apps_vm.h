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
#pragma once

/*
 * Inside the apps component: the app's own requests to Muse's VM, which the
 * chat and files modules share (muse_apps_vm.c). Natives never wait on the
 * VM: they queue a job for the VM worker, a task that runs jobs one at a time,
 * in order, and may block. A job ends an operation with muse_apps_op_finish().
 *
 * The app uses at most two of the session's three requests at once: the
 * chat's subscription, and the worker's one request at a time.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_link.h"

/* Statuses a request can end with besides HTTP's. */
#define MUSE_APPS_VM_NOT_SENT 0                       /* the session is down, or refused it */
#define MUSE_APPS_VM_DROPPED (-1)                     /* the stream or the session ended first */
#define MUSE_APPS_VM_TOO_LARGE MUSE_LINK_REQ_TOO_LARGE  /* a reply frame too large for this board */
#define MUSE_APPS_VM_TIMEOUT (-3)
#define MUSE_APPS_VM_CANCELLED (-4)                   /* the app stopped */

/* Jobs: run on the worker; a job that can't run any more (its app stopped
 * before the worker got to it) gets drop instead, which may be NULL. Either
 * owns arg from there. False if the job couldn't be queued. Any task. */
typedef void (*muse_apps_job_fn)(void *arg);
bool muse_apps_vm_job(muse_apps_job_fn run, muse_apps_job_fn drop, void *arg);

/* One request, waited for: worker only. Sends the gadget's own headers
 * (x-request-id, x-app-id) like Muse's chat requests, the body in pieces the
 * session takes, and gathers a 2xx reply's body in reply (cut at reply_cap,
 * reply_cut set). A refusal's status and the first bytes of its body go to
 * the log, and to error. */
typedef struct {
    const char *verb, *path;
    const char *range;            /* a Range header's value, or NULL */
    bool json;                    /* Accept: application/json (the routes that answer JSON) */
    bool ndjson;                  /* Accept: application/x-ndjson */
    const void *body;             /* JSON, or NULL */
    size_t body_len;
    uint8_t *reply;
    size_t reply_cap;
    int timeout_ms;
    /* out */
    size_t reply_len;
    bool reply_cut;
    char error[128];
} muse_apps_fetch_t;

/* The HTTP status, or one of MUSE_APPS_VM_*. */
int muse_apps_vm_fetch(muse_apps_fetch_t *f);

/* Opens a request with the same headers whose frames go to cb on the session
 * task, and sends body (NULL for none) in whole: worker only. The stream id,
 * or 0. As with muse_link_req_open(), cb may still run after a cancel. */
int64_t muse_apps_vm_open(const char *verb, const char *path, const char *accept, const char *body,
                          muse_link_req_cb cb, void *ctx);

/* A status that isn't 2xx as the GADGET_ERR_ an operation ends with. */
int32_t muse_apps_vm_error(int status);

/* "/fs/raw/" and a home-relative path, percent-encoded but for its slashes:
 * malloc'd, or NULL. */
char *muse_apps_vm_raw_path(const char *path);

/* Escapes n bytes of text into out for a JSON string, without quotes; out has
 * room for 6 * n bytes. Returns the length written. */
size_t muse_apps_vm_json_escape(char *out, const char *text, size_t n);
