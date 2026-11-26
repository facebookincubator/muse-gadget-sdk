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


#include "mg_hpf.h"

#include <math.h>

void mg_hpf_init(struct mg_hpf *f, unsigned int fc_hz, unsigned int rate_hz)
{
	f->a = fc_hz && rate_hz ? 1.0f - 2.0f * 3.14159265f * (float)fc_hz / (float)rate_hz : -1.0f;
	mg_hpf_reset(f);
}

void mg_hpf_reset(struct mg_hpf *f)
{
	f->x1 = 0.0f;
	f->y1 = 0.0f;
	f->primed = false;
}

void mg_hpf_run(struct mg_hpf *f, int16_t *pcm, size_t n)
{
	if (f->a < 0.0f || n == 0) {
		return;
	}
	if (!f->primed) {
		f->x1 = pcm[0];
		f->y1 = 0.0f;
		f->primed = true;
	}
	float a = f->a, x1 = f->x1, y1 = f->y1;

	for (size_t i = 0; i < n; i++) {
		float x = pcm[i];
		float y = x - x1 + a * y1;

		x1 = x;
		y1 = y;
		y = y < -32768.0f ? -32768.0f : y > 32767.0f ? 32767.0f : y;
		pcm[i] = (int16_t)lrintf(y);
	}
	f->x1 = x1;
	f->y1 = y1;
}
