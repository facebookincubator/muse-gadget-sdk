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

/* Host shim: MCUmgr's operation codes, groups and the error the gate answers
 * (Zephyr's own values). */

#ifndef HOST_SHIM_MGMT_DEFINES_H_
#define HOST_SHIM_MGMT_DEFINES_H_

enum mcumgr_op_t {
	MGMT_OP_READ = 0,
	MGMT_OP_READ_RSP,
	MGMT_OP_WRITE,
	MGMT_OP_WRITE_RSP,
};

enum mcumgr_group_t {
	MGMT_GROUP_ID_OS = 0,
	MGMT_GROUP_ID_IMAGE,
	MGMT_GROUP_ID_STAT,
	MGMT_GROUP_ID_SETTINGS,
	MGMT_GROUP_ID_LOG,
	MGMT_GROUP_ID_CRASH,
	MGMT_GROUP_ID_SPLIT,
	MGMT_GROUP_ID_RUN,
	MGMT_GROUP_ID_FS,
	MGMT_GROUP_ID_SHELL,
	MGMT_GROUP_ID_ENUM,
};

#define MGMT_ERR_EINVAL        3
#define MGMT_ERR_EACCESSDENIED 11

#endif
