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

#ifndef MG_ZEPHYR_VERSION_H_
#define MG_ZEPHYR_VERSION_H_

/*
 * The app's link-time wraps (-Wl,--wrap=..., CMakeLists.txt and
 * tests/smp_link/CMakeLists.txt) and the marked copy of Zephyr's smp.c stand
 * in for Zephyr internals as they are in the version below: their signatures,
 * their callers and what each caller expects of them. The linker checks none
 * of that, so another Zephyr version fails the build here. To move to one,
 * check every __wrap_ function (and src/zephyr/smp.c) against its source,
 * then change the version.
 */
#define MG_ZEPHYR_WRAPS_MAJOR 4
#define MG_ZEPHYR_WRAPS_MINOR 4
#define MG_ZEPHYR_WRAPS_PATCH 2

#if !defined(MG_HOST_TEST)
#include <zephyr/toolchain.h>
#include <zephyr/version.h>

#define MG_ZEPHYR_WRAPS_CHECK()                                                                    \
	BUILD_ASSERT(KERNEL_VERSION_MAJOR == MG_ZEPHYR_WRAPS_MAJOR &&                              \
			     KERNEL_VERSION_MINOR == MG_ZEPHYR_WRAPS_MINOR &&                      \
			     KERNEL_PATCHLEVEL == MG_ZEPHYR_WRAPS_PATCH,                           \
		     "the link-time wraps were written for Zephyr v4.4.2: check each against "    \
		     "this version's source (src/mg_zephyr_version.h)")
#else
/* The host tests build the wrapping files against their own shims. */
#define MG_ZEPHYR_WRAPS_CHECK() struct mg_zephyr_wraps_unchecked
#endif

#endif
