/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

#if CONFIG_MUSE_MOTION_GESTURES
/* Input-task only. Returns a lift event; caller owns screen waking. */
bool muse_motion_tick(uint32_t now_ms);
bool muse_motion_active(void);
/* Shared with the input task's sleep wait so its cadence cannot delay reads. */
uint32_t muse_motion_poll_ms(void);
/* Transient playback suppression; never writes the saved speaker setting. */
bool muse_motion_quiet(void);
void muse_motion_status(void);
#else
static inline bool muse_motion_quiet(void) { return false; }
#endif
