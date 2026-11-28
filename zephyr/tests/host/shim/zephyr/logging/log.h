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

#ifndef HOST_SHIM_LOG_H_
#define HOST_SHIM_LOG_H_

#include <stdio.h>

extern int host_log_verbose;
#define LOG_MODULE_REGISTER(...) extern int host_log_verbose
#define LOG_MODULE_DECLARE(...) extern int host_log_verbose
/* Every log line is kept in host_log_buf (host_zephyr.c), so a test can check
 * what the firmware logged (never a token, ...); printed with HOST_VERBOSE. */
void host_log_line(const char *lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG(lvl, ...) host_log_line(lvl, __VA_ARGS__)
#define LOG_ERR(...) HOST_LOG("err", __VA_ARGS__)
#define LOG_WRN(...) HOST_LOG("wrn", __VA_ARGS__)
#define LOG_INF(...) HOST_LOG("inf", __VA_ARGS__)
#define LOG_DBG(...) HOST_LOG("dbg", __VA_ARGS__)

#endif
