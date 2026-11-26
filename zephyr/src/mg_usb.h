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

#ifndef MG_USB_H_
#define MG_USB_H_

#include <stdbool.h>

/* USB power plugged in: 1 or 0, or -ENOTSUP on a board that can't
 * tell (no /zephyr,user mg-usb-sense-gpios). */
int mg_usb_present(void);

/* Before System OFF: the sense pin wakes the chip when USB is plugged in
 * (on_plug) or unplugged. Nothing on a board without one. */
void mg_usb_arm_wake(bool on_plug);

#endif
