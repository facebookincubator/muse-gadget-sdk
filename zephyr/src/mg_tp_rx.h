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

#ifndef MG_TP_RX_H_
#define MG_TP_RX_H_

#include <stdbool.h>
#include <stdint.h>

#include "mgcommands.h"

/* The receive throughput test's Data selector (mgcommands.h, Throughput test).
 *
 * change_data_type selects what the client's Data writes carry, for the
 * connection, whether or not a test runs: the selection stands until another
 * one or a disconnect. A receive test counts Data whenever throughput_test is
 * selected, so a client that selected it before (an earlier test, then Stop)
 * and starts receive again without re-sending change_data_type is counted
 * (it got 0 B before). */
struct mg_tp_rx {
	uint8_t selected;  /* the client's last change_data_type; 0: none this connection */
	bool receiving;
};

static inline void mg_tp_rx_select(struct mg_tp_rx *r, uint8_t type)
{
	r->selected = type;
}

static inline void mg_tp_rx_start(struct mg_tp_rx *r)
{
	r->receiving = true;
}

static inline void mg_tp_rx_stop(struct mg_tp_rx *r)
{
	r->receiving = false;
}

/* A disconnect: the next connection selects again. */
static inline void mg_tp_rx_disconnected(struct mg_tp_rx *r)
{
	r->selected = 0;
	r->receiving = false;
}

static inline bool mg_tp_rx_counts(const struct mg_tp_rx *r)
{
	return r->receiving && r->selected == mg_data_type_throughput_test;
}

#endif /* MG_TP_RX_H_ */
