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

/*
 * Confirm a phone's ATT indications on a NimBLE host built without the GATT
 * client (CONFIG_BT_NIMBLE_ROLE_CENTRAL off, as the Muse boards build it
 * unless CONFIG_MUSE_GADGET_BLE_PEER_NAME selects the client: then NimBLE
 * confirms them itself and this file isn't built).
 *
 * Without the client, NimBLE (ESP-IDF v6.0.1) has no handler for an incoming
 * Handle Value Indication (0x1D) and answers it with an Error Response
 * "request not supported" for opcode 0x1D. The NimBLE of ESP-IDF v6.0.3, the
 * one this repository pins, drops it instead (ble_att.c,
 * ble_att_rx_handle_unknown_request: an opcode with bit 0 set gets no
 * answer), so this wrap no longer fires there; it stays for the older
 * NimBLE. A phone's own GATT server
 * indicates (Service Changed) right after connecting; Android's GATT client,
 * meanwhile waiting for the answer to its first service discovery, takes
 * that Error Response for the discovery's answer ("Incorrect discovery
 * opcode 001d"), finds no services and gives up on the gadget. iOS and macOS
 * don't indicate then, which is why only Android saw it.
 *
 * The error goes out through ble_att_svr_tx_error_rsp(), which ble_att.c
 * calls across files, so a linker wrap (CMakeLists.txt) catches it: for an
 * indication we send the Handle Value Confirmation (0x1E) the spec asks for
 * instead, and drop the indication itself (the gadget has no use for it).
 */

#include <stdint.h>

#include "host/ble_hs.h"
#include "os/os_mbuf.h"

#define ATT_OP_INDICATE 0x1D
#define ATT_OP_CONFIRM 0x1E
#define ATT_ERR_REQ_NOT_SUPPORTED 0x06

/* NimBLE's (private) ATT layer, as in host/src/ble_att_priv.h and ble_att_cmd_priv.h. */
int __real_ble_att_svr_tx_error_rsp(uint16_t conn_handle, uint16_t cid, struct os_mbuf *txom, uint8_t req_op,
                                    uint16_t handle, uint8_t error_code);
void *ble_att_cmd_prepare(uint8_t opcode, size_t len, struct os_mbuf *txom);
int ble_att_tx(uint16_t conn_handle, uint16_t cid, struct os_mbuf *txom);

int __wrap_ble_att_svr_tx_error_rsp(uint16_t conn_handle, uint16_t cid, struct os_mbuf *txom, uint8_t req_op,
                                    uint16_t handle, uint8_t error_code)
{
    if (req_op == ATT_OP_INDICATE && error_code == ATT_ERR_REQ_NOT_SUPPORTED) {
        /* txom is the emptied request mbuf, reused for the reply (as the error would). */
        if (ble_att_cmd_prepare(ATT_OP_CONFIRM, 0, txom) == NULL) {
            return BLE_HS_ENOMEM;   /* freed inside, as the error's own path does */
        }
        return ble_att_tx(conn_handle, cid, txom);
    }
    return __real_ble_att_svr_tx_error_rsp(conn_handle, cid, txom, req_op, handle, error_code);
}
