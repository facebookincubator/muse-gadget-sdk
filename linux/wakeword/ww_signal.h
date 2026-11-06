/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define WW_SIGNAL_API __attribute__((visibility("default")))
#else
#define WW_SIGNAL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ww_signal ww_signal_t;

#define WW_SIGNAL_SENSITIVITY_LOW 0
#define WW_SIGNAL_SENSITIVITY_DEFAULT 1
#define WW_SIGNAL_SENSITIVITY_HIGH 2

enum {
  WW_SIGNAL_OK = 0,
  WW_SIGNAL_INVALID_ARGUMENT = 1,
  WW_SIGNAL_CALLBACK_ERROR = 2,
  WW_SIGNAL_OUT_OF_MEMORY = 3
};

/* Receives one normalized LFBE frame. Write the wake-class probability to
 * posterior and return 0 on success. Any nonzero return poisons the stream.
 * The frame is borrowed for this callback only. Catch exceptions in the caller;
 * no exception may cross this C function. Do not call back into this handle. */
typedef int (*ww_signal_frame_fn)(void *ctx, const float frame[40],
                                  float *posterior);

/* No model/runtime is loaded. Sensitivity is the exact ESP enum:
 * 0=low (.99), 1=default (.95), 2=high (.90).
 * Returns NULL on error. A nonempty error_buf is always NUL-terminated.
 * Handles are not thread-safe; the caller must serialize all access. */
WW_SIGNAL_API ww_signal_t *ww_signal_create(int sensitivity, char *error_buf,
                                            size_t error_size);

/* Signed 16-bit LE mono PCM at 16 kHz, in arbitrary byte chunks. An odd byte is
 * retained. callback/fired must be non-NULL, even for an empty feed; bytes may
 * be NULL only with byte_count=0. Successful feeds process ALL bytes after a
 * fire; *fired is 0/1 for any fire in this call.
 * Callback failure clears *fired and suppresses further callbacks until reset.
 * Failed feeds may consume a prefix; do not replay as if no input was consumed.
 * Input decoding uses a fixed-size buffer; feed never allocates. */
WW_SIGNAL_API int ww_signal_feed(ww_signal_t *handle, const void *bytes,
                                 size_t byte_count, ww_signal_frame_fn callback,
                                 void *ctx, int *fired);

/* Clear frontend, detector, odd byte, frame count and callback-error poison.
 * Python/model state is NOT reset here: the caller resets its model separately.
 * Idempotent; reset of a valid handle cannot allocate or fail. */
WW_SIGNAL_API int ww_signal_reset(ww_signal_t *handle);
WW_SIGNAL_API void ww_signal_destroy(ww_signal_t *handle);

/* Borrowed error text until next operation. Success clears the previous error.
 * NULL returns a static diagnostic. Do not use a handle after destroy. */
WW_SIGNAL_API const char *ww_signal_last_error(ww_signal_t *handle);

/* Cumulative LFBE frames emitted since create/reset, saturating at UINT64_MAX.
 * An already-buffered frontend batch can emit frames after callback failure;
 * those count here but do not invoke the model callback or detector. */
WW_SIGNAL_API uint64_t ww_signal_frames_seen(ww_signal_t *handle);

#ifdef __cplusplus
}
#endif
