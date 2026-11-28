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

#include "mg_battery.h"

#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mg_battery, CONFIG_MG_LOG_LEVEL);

/* VBAT on /zephyr,user io-channels, behind a divider of
 * CONFIG_MG_BATTERY_DIVIDER_RATIO. */
static const struct adc_dt_spec vbat = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

int mg_battery_read_mv(void)
{
	int16_t raw;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int err;

	if (!adc_is_ready_dt(&vbat)) {
		return -ENODEV;
	}
	err = adc_channel_setup_dt(&vbat);
	err = err ?: adc_sequence_init_dt(&vbat, &seq);
	err = err ?: adc_read_dt(&vbat, &seq);
	if (err) {
		return err;
	}
	int32_t mv = raw;

	err = adc_raw_to_millivolts_dt(&vbat, &mv);
	if (err) {
		return err;
	}
	return mv * CONFIG_MG_BATTERY_DIVIDER_RATIO;
}
