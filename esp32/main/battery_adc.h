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

// Battery voltage monitoring via ADC. The reTerminal E1002 monitors its
// Li-ion battery through ADC1 channel 0 (GPIO1) fed by a 1:2 voltage
// divider, enabled by GPIO21 (active high).

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read the battery voltage. The first call configures the ADC and the
// divider enable GPIO. Each read enables the divider, waits for it to
// settle, samples, then disables the divider again so it draws no
// current between reads. On success, *mv_out is the battery voltage in
// millivolts and *pct_out is the estimated charge (0-100). Returns
// false when the ADC cannot be set up or a read fails.
bool battery_adc_read(int *mv_out, int *pct_out);

#ifdef __cplusplus
}
#endif
