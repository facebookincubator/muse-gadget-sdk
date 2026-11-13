// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One retained text question. Oversized widgets are rejected, never truncated. */
#define MUSE_OPTIONS_MAX 8
#define MUSE_OPTION_BYTES 192
#define MUSE_OPTION_ID_BYTES 80
#define MUSE_OPTION_QUESTION_BYTES 384
typedef enum {
    MUSE_OPTIONS_EMPTY, MUSE_OPTIONS_READY, MUSE_OPTIONS_QUEUED,
    MUSE_OPTIONS_SENDING, MUSE_OPTIONS_SELECTED, MUSE_OPTIONS_UNCERTAIN,
    MUSE_OPTIONS_STALE,
} muse_options_status_t;

typedef struct {
    uint32_t generation, revision;
    char widget_id[MUSE_OPTION_ID_BYTES], message_id[MUSE_OPTION_ID_BYTES];
    char question[MUSE_OPTION_QUESTION_BYTES];
    char labels[MUSE_OPTIONS_MAX][MUSE_OPTION_BYTES];
    int count, selected;
    double state_version;
    muse_options_status_t status;
    bool saved;
    char detail[96];
} muse_options_t;

/* Snapshots and commands can cross tasks. No caller retains internal pointers. */
bool muse_options_snapshot(muse_options_t *out);
void muse_options_clear(void);
void muse_options_disconnected(void);
bool muse_options_publish(const muse_options_t *question);
bool muse_options_request(uint32_t generation, int index);
bool muse_options_pending(void);
bool muse_options_waiting(void);
bool muse_options_take(muse_options_t *out);
/* USB console: options, or options.select=GENERATION,INDEX (zero based). */
bool muse_options_console(const char *line);
void muse_options_result(uint32_t generation, muse_options_status_t status,
                         bool saved, const char *detail);

/* Session-task JSON adapter; not linked into Link-only or simulator builds. */
struct cJSON;
bool muse_options_receive(const struct cJSON *payload);
struct cJSON *muse_options_chat_body(const muse_options_t *q, const char *interaction_id);
struct cJSON *muse_options_state_body(const muse_options_t *q, const char *mutation_id);
bool muse_options_state_response(const muse_options_t *q, const struct cJSON *response);

#ifdef __cplusplus
}
#endif
