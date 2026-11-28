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

/* A tiny test framework for the host harnesses. */

#ifndef HOST_CHECK_H_
#define HOST_CHECK_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int host_checks, host_failures;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		host_checks++;                                                                     \
		if (!(cond)) {                                                                     \
			host_failures++;                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
		}                                                                                  \
	} while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)
static inline int host_str_eq(const char *a, const char *b)
{
	if (a == NULL || strcmp(a, b) != 0) {
		fprintf(stderr, "    got \"%s\", want \"%s\"\n", a ? a : "(null)", b);
		return 0;
	}
	return 1;
}
#define CHECK_STR(a, b) CHECK(host_str_eq((a), (b)))

#define HOST_MAIN(...)                                                                             \
	int host_checks, host_failures;                                                            \
	int main(void)                                                                             \
	{                                                                                          \
		void (*tests[])(void) = {__VA_ARGS__};                                             \
		for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {                    \
			tests[i]();                                                                \
		}                                                                                  \
		printf("%d checks, %d failures\n", host_checks, host_failures);                  \
		return host_failures ? 1 : 0;                                                      \
	}

#endif
