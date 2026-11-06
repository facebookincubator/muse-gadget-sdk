/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND.
 */
#include "ww_signal.h"

#include "ww_detector.h"
#include "ww_frontend.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

struct ww_signal {
  ww_frontend_t *frontend;
  ww_detector_t detector;
  char error[256];
  uint64_t frames_seen;
  uint8_t partial_byte;
  bool has_partial;
  bool poisoned;
};

typedef struct {
  ww_signal_t *handle;
  ww_signal_frame_fn callback;
  void *ctx;
  int status;
  int fired;
} ww_feed_context_t;

static int ww_signal_fail(ww_signal_t *handle, int status,
                          const char *message) {
  if (handle != NULL) {
    snprintf(handle->error, sizeof(handle->error), "%s", message);
  }
  return status;
}

static void ww_signal_on_frame(void *context, const float frame[40]) {
  ww_feed_context_t *feed = context;
  ww_signal_t *handle = feed->handle;
  if (handle->frames_seen != UINT64_MAX) {
    handle->frames_seen++;
  }
  if (feed->status != WW_SIGNAL_OK) {
    return;
  }

  float posterior = 0.0f;
  if (feed->callback(feed->ctx, frame, &posterior) != 0) {
    handle->poisoned = true;
    feed->fired = 0;
    feed->status = ww_signal_fail(handle, WW_SIGNAL_CALLBACK_ERROR,
                                  "model callback failed; reset required");
    return;
  }
  if (!isfinite(posterior)) {
    posterior = 0.0f;
  } else if (posterior < 0.0f) {
    posterior = 0.0f;
  } else if (posterior > 1.0f) {
    posterior = 1.0f;
  }
  /* Preserve warm-up completion when the unchanged ESP counter wraps. */
  if (handle->detector.frames_seen == UINT32_MAX) {
    handle->detector.frames_seen = WW_DETECTOR_WARMUP_FRAMES + 1;
  }
  if (ww_detector_push(&handle->detector, posterior)) {
    feed->fired = 1;
  }
}

ww_signal_t *ww_signal_create(int sensitivity, char *error_buf,
                              size_t error_size) {
  if (error_buf != NULL && error_size > 0) {
    error_buf[0] = '\0';
  }
  if (sensitivity < WW_SIGNAL_SENSITIVITY_LOW ||
      sensitivity > WW_SIGNAL_SENSITIVITY_HIGH ||
      (error_buf == NULL && error_size > 0)) {
    if (error_buf != NULL && error_size > 0) {
      snprintf(error_buf, error_size, "invalid sensitivity or error buffer");
    }
    return NULL;
  }
  ww_signal_t *handle = calloc(1, sizeof(*handle));
  if (handle != NULL) {
    handle->frontend = ww_frontend_create();
    if (handle->frontend != NULL) {
      ww_detector_init(&handle->detector, (ww_sensitivity_t)sensitivity);
      return handle;
    }
    free(handle);
  }
  if (error_buf != NULL && error_size > 0) {
    snprintf(error_buf, error_size, "allocating signal frontend failed");
  }
  return NULL;
}

int ww_signal_feed(ww_signal_t *handle, const void *bytes, size_t byte_count,
                   ww_signal_frame_fn callback, void *ctx, int *fired) {
  if (fired != NULL) {
    *fired = 0;
  }
  if (handle == NULL) {
    return WW_SIGNAL_INVALID_ARGUMENT;
  }
  handle->error[0] = '\0';
  if (callback == NULL || fired == NULL || (bytes == NULL && byte_count > 0)) {
    return ww_signal_fail(handle, WW_SIGNAL_INVALID_ARGUMENT,
                          "invalid feed arguments");
  }
  if (handle->poisoned) {
    return ww_signal_fail(handle, WW_SIGNAL_CALLBACK_ERROR,
                          "model callback failed; reset required");
  }

  ww_feed_context_t feed = {handle, callback, ctx, WW_SIGNAL_OK, 0};
  const uint8_t *input = bytes;
  size_t offset = 0;
  while (offset < byte_count && feed.status == WW_SIGNAL_OK) {
    int16_t samples[512];
    size_t count = 0;
    while (offset < byte_count && count < 512) {
      if (!handle->has_partial) {
        handle->partial_byte = input[offset++];
        handle->has_partial = true;
      }
      if (offset == byte_count) {
        break;
      }
      const uint16_t value =
          (uint16_t)handle->partial_byte | ((uint16_t)input[offset++] << 8);
      const int32_t signed_value =
          value >= 0x8000u ? (int32_t)value - 65536 : value;
      samples[count++] = (int16_t)signed_value;
      handle->has_partial = false;
    }
    if (count > 0) {
      ww_frontend_push(handle->frontend, samples, count, ww_signal_on_frame,
                       &feed);
    }
  }
  *fired = feed.status == WW_SIGNAL_OK ? feed.fired : 0;
  return feed.status;
}

int ww_signal_reset(ww_signal_t *handle) {
  if (handle == NULL) {
    return WW_SIGNAL_INVALID_ARGUMENT;
  }
  ww_frontend_reset(handle->frontend);
  ww_detector_reset(&handle->detector);
  handle->has_partial = false;
  handle->partial_byte = 0;
  handle->frames_seen = 0;
  handle->poisoned = false;
  handle->error[0] = '\0';
  return WW_SIGNAL_OK;
}

void ww_signal_destroy(ww_signal_t *handle) {
  if (handle != NULL) {
    ww_frontend_destroy(handle->frontend);
    free(handle);
  }
}

const char *ww_signal_last_error(ww_signal_t *handle) {
  return handle == NULL ? "invalid signal handle" : handle->error;
}

uint64_t ww_signal_frames_seen(ww_signal_t *handle) {
  return handle == NULL ? 0 : handle->frames_seen;
}
