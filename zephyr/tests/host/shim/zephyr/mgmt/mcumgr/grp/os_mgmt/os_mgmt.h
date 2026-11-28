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

/* Host shim: the OS group's command ids (Zephyr's own values). */

#ifndef HOST_SHIM_OS_MGMT_H_
#define HOST_SHIM_OS_MGMT_H_

#define OS_MGMT_ID_ECHO            0
#define OS_MGMT_ID_CONS_ECHO_CTRL  1
#define OS_MGMT_ID_TASKSTAT        2
#define OS_MGMT_ID_MPSTAT          3
#define OS_MGMT_ID_DATETIME_STR    4
#define OS_MGMT_ID_RESET           5
#define OS_MGMT_ID_MCUMGR_PARAMS   6
#define OS_MGMT_ID_INFO            7
#define OS_MGMT_ID_BOOTLOADER_INFO 8

#endif
