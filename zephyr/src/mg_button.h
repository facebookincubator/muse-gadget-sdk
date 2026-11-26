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

#ifndef MG_BUTTON_H_
#define MG_BUTTON_H_

#include <stdbool.h>

/* Button events count from here (after mg_core_init()); an edge before it,
 * from boot on, is dropped. */
void mg_button_init(void);
/* The button is down (read directly; at power-up, before input events run). */
bool mg_button_held_at_boot(void);

/* sw0 belongs to gpio-keys (its parent is a gpio-keys node and the driver is
 * built): gpio-keys configured the pin and armed its trigger at POST_KERNEL,
 * and reconfiguring it removes that trigger in Zephyr's nRF GPIO driver, so
 * every press after it would be lost. Such a pin is only read; one that
 * isn't (a board without gpio-keys for it) is configured as an input first
 * (tools/check_button_pin.py). */
#define MG_SW0_GPIO_KEYS                                                                           \
	(DT_NODE_EXISTS(DT_ALIAS(sw0)) && IS_ENABLED(CONFIG_INPUT_GPIO_KEYS) &&                    \
	 DT_NODE_HAS_COMPAT(DT_PARENT(DT_ALIAS(sw0)), gpio_keys))
/* Feeds a press or release as if it came from the button (tests). */
void mg_button_inject(bool pressed);
/* A confirm prompt was armed: events queued before now count as before it
 * (injected ones are dropped as they come out of the queue). */
void mg_button_flush_injected(void);

#endif
