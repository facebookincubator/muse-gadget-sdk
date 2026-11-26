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

/* Host shim: the input event a gpio-keys driver reports, and the one
 * callback a test calls in its place (host_input_cb). */

#ifndef HOST_SHIM_INPUT_H_
#define HOST_SHIM_INPUT_H_

#include <stdint.h>

#define INPUT_EV_KEY    0x01
#define INPUT_KEY_ENTER 28

struct input_event {
	uint8_t sync;
	uint8_t type;
	uint16_t code;
	int32_t value;
};

#define INPUT_CALLBACK_DEFINE(dev, cb, user_data)                                                  \
	void (*const host_input_cb)(struct input_event *, void *) = (cb)

#endif
