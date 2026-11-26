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

#ifndef HOST_SHIM_NET_BUF_H_
#define HOST_SHIM_NET_BUF_H_

#include <stdint.h>

/* Only what src/mg_smp_req_gen.c touches: a buffer's bytes and user data. */
struct net_buf {
	uint8_t *data;
	uint16_t len;
	uint8_t user_data[CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE];
};

static inline void *net_buf_user_data(const struct net_buf *buf)
{
	return (void *)buf->user_data;
}

#endif
