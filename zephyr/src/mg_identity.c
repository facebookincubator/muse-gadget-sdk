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

#include "mg_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "mg_setup_store.h"

static char name[32];
static char safe_name[32]; /* SAFE-[<PRODUCT>-]1A2B3C: the recovery identity */
static char node_id[24];     /* the current one (mg_identity_node_id) */
static char node_gadget[24]; /* gadget-1a2b3c */
static char node_legacy[24]; /* homelink-1a2b3c: setups from before */
/* What a new setup offers: gadget- with CONFIG_MG_NODE_ID_GADGET (off by
 * default), else homelink-. */
static bool offer_gadget = IS_ENABLED(CONFIG_MG_NODE_ID_GADGET);
static uint8_t suffix[3];

static void build_node_ids(void)
{
	snprintf(node_gadget, sizeof(node_gadget), "gadget-%02x%02x%02x", suffix[0], suffix[1],
		 suffix[2]);
	snprintf(node_legacy, sizeof(node_legacy), "homelink-%02x%02x%02x", suffix[0], suffix[1],
		 suffix[2]);
	mg_identity_node_refresh();
}

void mg_identity_node_refresh(void)
{
	/* Set up: the form stored with K; not set up: what setup offers. */
	bool gadget = mg_setup_store_complete() ? mg_setup_store_gadget_node() : offer_gadget;
	const char *now = gadget ? node_gadget : node_legacy;

	if (strcmp(node_id, now) != 0) {
		strcpy(node_id, now);
	}
}

void mg_identity_set_offer_gadget(bool on)
{
	offer_gadget = on;
	mg_identity_node_refresh();
}

bool mg_identity_offer_gadget(void)
{
	return offer_gadget;
}
static char mac[18];
static char device_id[32];

void mg_identity_init(const char *name_prefix, const char *product, const uint8_t a[6])
{
	bool has_product = product && product[0];

	snprintf(name, sizeof(name), "%s%s%s%02X%02X%02X", name_prefix, has_product ? product : "",
		 has_product ? "-" : "", a[3], a[4], a[5]);
	snprintf(safe_name, sizeof(safe_name), "SAFE-%s%s%02X%02X%02X", has_product ? product : "",
		 has_product ? "-" : "", a[3], a[4], a[5]);
	suffix[0] = a[3];
	suffix[1] = a[4];
	suffix[2] = a[5];
	build_node_ids();
	snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4],
		 a[5]);
	snprintf(device_id, sizeof(device_id), "hatch-link:%s", mac);
}

const char *mg_identity_name(void)
{
	return name;
}

const char *mg_identity_safe_name(void)
{
	return safe_name;
}

const char *mg_identity_node_id(void)
{
	mg_identity_node_refresh();
	return node_id;
}

const char *mg_identity_offer_node_id(void)
{
	return offer_gadget ? node_gadget : node_legacy;
}

const char *mg_identity_legacy_node_id(void)
{
	return node_legacy;
}

const char *mg_identity_mac(void)
{
	return mac;
}

const char *mg_identity_device_id(void)
{
	return device_id;
}
