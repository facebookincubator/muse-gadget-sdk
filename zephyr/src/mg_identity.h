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

#ifndef MG_IDENTITY_H_
#define MG_IDENTITY_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The names the gadget goes by, from its BLE identity address (the one it
 * advertises from), like an ESP32 gadget's from its MAC:
 *   name       MuseGadget-1A2B3C   (CONFIG_MG_DEVICE_NAME_PREFIX, last 3 bytes),
 *              MuseGadget-XN54-1A2B3C for a catalog product (its model code,
 *              CONFIG_MG_DEVICE_PRODUCT_CODE)
 *   node_id    gadget-1a2b3c       (the name the Muse server knows it by;
 *              offered at every new setup
 *              and kept with K; a gadget set up before keeps its stored
 *              homelink-1a2b3c until it is set up again)
 *   mac        aa:bb:cc:1a:2b:3c
 *   device_id  hatch-link:aa:bb:cc:1a:2b:3c  (the Link setup device id)
 */

/* addr: most significant byte first; product: the catalog model code, or
 * NULL or "" for none. */
void mg_identity_init(const char *name_prefix, const char *product, const uint8_t addr[6]);
const char *mg_identity_name(void);
/* The name SAFE mode advertises, SAFE-[<PRODUCT>-]1A2B3C: a recovery mode
 * keeps a distinct identity, never the Muse one, so an app that doesn't read
 * its declaration never takes it for the gadget (mgcommands.h, Recovery mode;
 * as the ESP32's SAFE). */
const char *mg_identity_safe_name(void);
/* The node id the gadget is now: the one stored with its setup (the offered
 * form, or homelink- for a setup from before the gadget- form), the offered one when
 * not set up. The pointer stays the same; the store refreshes the text when
 * a setup commits, loads or is erased. */
const char *mg_identity_node_id(void);
/* The node id a new setup offers and derives K with: gadget-1a2b3c with
 * CONFIG_MG_NODE_ID_GADGET, else homelink-1a2b3c. */
const char *mg_identity_offer_node_id(void);
bool mg_identity_offer_gadget(void);
/* The older id from the same address: homelink-1a2b3c. */
const char *mg_identity_legacy_node_id(void);
void mg_identity_node_refresh(void);
/* Tests: which form a new setup offers (CONFIG_MG_NODE_ID_GADGET at boot). */
void mg_identity_set_offer_gadget(bool on);
const char *mg_identity_mac(void);
const char *mg_identity_device_id(void);

#endif
