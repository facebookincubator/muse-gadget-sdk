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



#include <zephyr/ztest.h>

#include "mg_hpf.h"

#define RATE 16000

/* Primed at the start of a capture: a mic sitting at -1100 starts at zero,
 * with no settling transient in the first frames. */
ZTEST(hpf, test_primed_start)
{
	static int16_t x[480];
	struct mg_hpf f;

	mg_hpf_init(&f, 30, RATE);
	for (size_t i = 0; i < ARRAY_SIZE(x); i++) {
		x[i] = -1100;
	}
	mg_hpf_run(&f, x, ARRAY_SIZE(x));
	for (size_t i = 0; i < ARRAY_SIZE(x); i++) {
		zassert_equal(x[i], 0, "sample %u is %d", (unsigned int)i, x[i]);
	}
	/* And again after a reset (the next capture), from a new level. */
	mg_hpf_reset(&f);
	for (size_t i = 0; i < ARRAY_SIZE(x); i++) {
		x[i] = 500;
	}
	mg_hpf_run(&f, x, 160);
	zassert_equal(x[0], 0);
	zassert_equal(x[159], 0);
}

ZTEST_SUITE(hpf, NULL, NULL, NULL, NULL, NULL);
