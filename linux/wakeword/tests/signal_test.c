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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define TEST_SAMPLES (512 + 320 * 139)
#define TEST_FRAMES 140

typedef struct {
  size_t count;
  float frames[TEST_FRAMES][40];
  float posterior;
  size_t fail_at;
} callback_state_t;

static int model_callback(void *context, const float frame[40],
                          float *posterior) {
  callback_state_t *state = context;
  CHECK(state->count < TEST_FRAMES);
  memcpy(state->frames[state->count], frame, sizeof(state->frames[0]));
  state->count++;
  *posterior = state->posterior;
  return state->fail_at != 0 && state->count == state->fail_at ? 1 : 0;
}

static void frontend_callback(void *context, const float frame[40]) {
  float posterior;
  CHECK(model_callback(context, frame, &posterior) == 0);
}

static ww_signal_t *create(int sensitivity) {
  char error[256];
  ww_signal_t *handle = ww_signal_create(sensitivity, error, sizeof(error));
  CHECK(handle != NULL && error[0] == '\0');
  return handle;
}

static void make_pcm(int16_t *pcm, uint8_t *bytes) {
  for (size_t i = 0; i < TEST_SAMPLES; i++) {
    const uint16_t value = (uint16_t)(i * 739u);
    pcm[i] = (int16_t)(value >= 0x8000u ? (int32_t)value - 65536 : value);
    bytes[i * 2] = (uint8_t)value;
    bytes[i * 2 + 1] = (uint8_t)(value >> 8);
  }
  pcm[0] = INT16_MIN;
  bytes[0] = 0;
  bytes[1] = 128;
  pcm[1] = INT16_MAX;
  bytes[2] = 255;
  bytes[3] = 127;
}

static void test_frontend_and_chunking(const int16_t *pcm,
                                       const uint8_t *bytes) {
  callback_state_t reference = {0};
  callback_state_t whole = {.posterior = 1.0f};
  callback_state_t chunks = {.posterior = 1.0f};
  ww_frontend_t *frontend = ww_frontend_create();
  CHECK(frontend != NULL);
  CHECK(ww_frontend_push(frontend, pcm, TEST_SAMPLES, frontend_callback,
                         &reference) == TEST_FRAMES);
  ww_frontend_destroy(frontend);

  ww_signal_t *a = create(WW_SIGNAL_SENSITIVITY_DEFAULT);
  ww_signal_t *b = create(WW_SIGNAL_SENSITIVITY_DEFAULT);
  int fired = -1;
  CHECK(ww_signal_feed(a, bytes, TEST_SAMPLES * 2, model_callback, &whole,
                       &fired) == 0);
  CHECK(fired == 1 && whole.count == TEST_FRAMES &&
        ww_signal_frames_seen(a) == TEST_FRAMES);
  int any = 0;
  for (size_t offset = 0; offset < TEST_SAMPLES * 2;) {
    size_t take = 1 + offset % 997;
    if (take > TEST_SAMPLES * 2 - offset) {
      take = TEST_SAMPLES * 2 - offset;
    }
    CHECK(ww_signal_feed(b, bytes + offset, take, model_callback, &chunks,
                         &fired) == 0);
    any |= fired;
    offset += take;
  }
  CHECK(any && chunks.count == TEST_FRAMES &&
        ww_signal_frames_seen(b) == TEST_FRAMES);
  CHECK(memcmp(whole.frames, reference.frames, sizeof(reference.frames)) == 0);
  CHECK(memcmp(chunks.frames, reference.frames, sizeof(reference.frames)) == 0);
  ww_signal_destroy(a);
  ww_signal_destroy(b);
}

static void test_odd_bytes_and_reset(const uint8_t *bytes) {
  callback_state_t state = {.posterior = 1.0f};
  ww_signal_t *handle = create(WW_SIGNAL_SENSITIVITY_DEFAULT);
  int fired;
  CHECK(ww_signal_feed(handle, bytes, 1023, model_callback, &state, &fired) ==
        0);
  CHECK(state.count == 0 && ww_signal_frames_seen(handle) == 0 && fired == 0);
  CHECK(ww_signal_feed(handle, bytes + 1023, 1, model_callback, &state,
                       &fired) == 0);
  CHECK(state.count == 1 && ww_signal_frames_seen(handle) == 1);
  CHECK(ww_signal_feed(handle, bytes, 1, model_callback, &state, &fired) == 0);
  CHECK(ww_signal_reset(handle) == 0 && ww_signal_reset(handle) == 0);
  CHECK(ww_signal_frames_seen(handle) == 0);
  memset(&state, 0, sizeof(state));
  state.posterior = 1.0f;
  CHECK(ww_signal_feed(handle, bytes, 1023, model_callback, &state, &fired) ==
        0);
  CHECK(state.count == 0 && ww_signal_frames_seen(handle) == 0);
  CHECK(ww_signal_feed(handle, bytes + 1023, 1, model_callback, &state,
                       &fired) == 0);
  CHECK(state.count == 1);
  ww_signal_destroy(handle);
}

static void test_detector(const uint8_t *bytes) {
  const float thresholds[] = {0.99f, 0.95f, 0.90f};
  for (int sensitivity = 0; sensitivity < 3; sensitivity++) {
    ww_detector_t detector;
    ww_detector_init(&detector, (ww_sensitivity_t)sensitivity);
    CHECK(detector.threshold == thresholds[sensitivity]);
    for (int i = 0; i < 29; i++) {
      CHECK(!ww_detector_push(&detector, 1.0f));
    }
    CHECK(ww_detector_push(&detector, 1.0f));
    for (int i = 0; i < 50; i++) {
      CHECK(!ww_detector_push(&detector, 1.0f));
    }
    CHECK(ww_detector_push(&detector, 1.0f));
    ww_detector_reset(&detector);
    CHECK(detector.frames_seen == 0 &&
          detector.threshold == thresholds[sensitivity]);

    callback_state_t state = {.posterior = 0.97f};
    ww_signal_t *handle = create(sensitivity);
    int fired;
    CHECK(ww_signal_feed(handle, bytes, TEST_SAMPLES * 2, model_callback,
                         &state, &fired) == 0);
    CHECK(fired == (sensitivity != WW_SIGNAL_SENSITIVITY_LOW));
    CHECK(state.count == TEST_FRAMES);
    CHECK(ww_signal_reset(handle) == 0);
    memset(&state, 0, sizeof(state));
    state.posterior = 1.0f;
    for (size_t frame = 1; frame <= TEST_FRAMES; frame++) {
      const size_t offset = frame == 1 ? 0 : (512 + (frame - 2) * 320) * 2;
      const size_t count = frame == 1 ? 1024 : 640;
      CHECK(ww_signal_feed(handle, bytes + offset, count, model_callback,
                           &state, &fired) == 0);
      CHECK(fired == (frame == 30 || frame == 81 || frame == 132));
    }
    CHECK(ww_signal_frames_seen(handle) == TEST_FRAMES);
    ww_signal_destroy(handle);
  }
  const float values[] = {NAN, INFINITY, -INFINITY, -1.0f, 2.0f};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    callback_state_t state = {.posterior = values[i]};
    ww_signal_t *handle = create(WW_SIGNAL_SENSITIVITY_DEFAULT);
    int fired;
    CHECK(ww_signal_feed(handle, bytes, TEST_SAMPLES * 2, model_callback,
                         &state, &fired) == 0);
    CHECK(fired == (values[i] == 2.0f));
    ww_signal_destroy(handle);
  }
}

static void test_callback_failure(const uint8_t *bytes) {
  ww_signal_t *handle = create(WW_SIGNAL_SENSITIVITY_DEFAULT);
  callback_state_t state = {.posterior = 1.0f, .fail_at = 3};
  int fired = -1;
  CHECK(ww_signal_feed(handle, bytes, TEST_SAMPLES * 2, model_callback, &state,
                       &fired) == WW_SIGNAL_CALLBACK_ERROR);
  CHECK(fired == 0 && state.count == 3);
  // The third decoded batch emits two frontend frames; only its first calls the
  // model.
  CHECK(ww_signal_frames_seen(handle) == 4);
  CHECK(strstr(ww_signal_last_error(handle), "reset required") != NULL);
  CHECK(ww_signal_feed(handle, bytes, TEST_SAMPLES * 2, model_callback, &state,
                       &fired) == WW_SIGNAL_CALLBACK_ERROR);
  CHECK(ww_signal_feed(handle, NULL, 0, model_callback, &state, &fired) ==
        WW_SIGNAL_CALLBACK_ERROR);
  CHECK(state.count == 3 && ww_signal_frames_seen(handle) == 4 && fired == 0);

  CHECK(ww_signal_reset(handle) == 0 &&
        ww_signal_last_error(handle)[0] == '\0');
  memset(&state, 0, sizeof(state));
  state.posterior = 1.0f;
  state.fail_at = 31;
  CHECK(ww_signal_feed(handle, bytes, TEST_SAMPLES * 2, model_callback, &state,
                       &fired) == WW_SIGNAL_CALLBACK_ERROR);
  CHECK(state.count == 31 &&
        fired == 0); // A fire at frame 30 must not escape a failed feed.
  CHECK(ww_signal_reset(handle) == 0);
  memset(&state, 0, sizeof(state));
  state.posterior = 1.0f;
  CHECK(ww_signal_feed(handle, bytes, (512 + 28 * 320) * 2, model_callback,
                       &state, &fired) == 0);
  CHECK(state.count == 29 && fired == 0);
  CHECK(ww_signal_feed(handle, bytes, 640, model_callback, &state, &fired) ==
            0 &&
        fired == 1);
  ww_signal_destroy(handle);
}

static void test_arguments_and_silence(void) {
  char error[8];
  CHECK(ww_signal_create(-1, error, sizeof(error)) == NULL && error[7] == '\0');
  CHECK(ww_signal_create(3, error, sizeof(error)) == NULL);
  char one = 'x';
  CHECK(ww_signal_create(-1, &one, 1) == NULL && one == '\0');
  CHECK(ww_signal_create(1, NULL, 1) == NULL);
  ww_signal_t *handle = ww_signal_create(1, NULL, 0);
  CHECK(handle != NULL);
  ww_signal_destroy(handle);
  CHECK(ww_signal_reset(NULL) == WW_SIGNAL_INVALID_ARGUMENT);
  CHECK(ww_signal_feed(NULL, NULL, 0, NULL, NULL, NULL) ==
        WW_SIGNAL_INVALID_ARGUMENT);
  CHECK(ww_signal_frames_seen(NULL) == 0 &&
        strlen(ww_signal_last_error(NULL)) > 0);
  ww_signal_destroy(NULL);

  handle = create(WW_SIGNAL_SENSITIVITY_DEFAULT);
  callback_state_t state = {0};
  int fired;
  CHECK(ww_signal_feed(handle, NULL, 1, model_callback, &state, &fired) ==
            WW_SIGNAL_INVALID_ARGUMENT &&
        fired == 0);
  CHECK(strlen(ww_signal_last_error(handle)) > 0);
  CHECK(ww_signal_feed(handle, NULL, 0, NULL, &state, &fired) ==
        WW_SIGNAL_INVALID_ARGUMENT);
  CHECK(ww_signal_feed(handle, NULL, 0, model_callback, &state, NULL) ==
        WW_SIGNAL_INVALID_ARGUMENT);
  CHECK(ww_signal_feed(handle, NULL, 0, model_callback, &state, &fired) == 0);
  CHECK(fired == 0 && state.count == 0 &&
        ww_signal_last_error(handle)[0] == '\0');
  uint8_t zeros[1024] = {0};
  CHECK(ww_signal_feed(handle, zeros, sizeof(zeros), model_callback, &state,
                       &fired) == 0);
  CHECK(state.count == 1 && fired == 0);
  for (size_t i = 0; i < 40; i++) {
    const float expected =
        (logf(WW_FRONTEND_LOG_EPS) - kWwFrontendNormMean[i]) /
        kWwFrontendNormStd[i];
    CHECK(fabsf(state.frames[0][i] - expected) < 1e-6f);
  }
  ww_signal_destroy(handle);
}

int main(void) {
  int16_t *pcm = malloc(TEST_SAMPLES * sizeof(*pcm));
  uint8_t *bytes = malloc(TEST_SAMPLES * 2);
  CHECK(pcm != NULL && bytes != NULL);
  make_pcm(pcm, bytes);
  test_frontend_and_chunking(pcm, bytes);
  test_odd_bytes_and_reset(bytes);
  test_detector(bytes);
  test_callback_failure(bytes);
  test_arguments_and_silence();
  free(bytes);
  free(pcm);
  puts("C-only shared signal shim: frontend, LE/odd chunks, detector, "
       "full-chunk fires, reset, callback errors passed");
  return 0;
}
