# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# musegadgets BLE audio (CONFIG_MUSE_GADGET_BLE_AUDIO): the protocol in
# protocols/ at the repository root, served on Home Link's NimBLE host. See
# README.md. Without the option it builds nothing; its headers stay available
# so callers compile unchanged.
set(protocols "../../../protocols")
set(portable "../../../portable")
if(CONFIG_MUSE_GADGET_BLE_AUDIO)
    set(srcs "haptics/mg_haptics.c" "haptics/mg_haptic_test.c" "mg_ble.c" "mg_linkpol.c" "mg_voice.c" "mg_proto.c" "mg_codec.c" "mg_route.c" "mg_queue.c" "mg_play.c" "mg_chain.c"
        "mg_resample.c" "mg_token_proof.c" "mg_unlock.c" "mg_ui.c" "mg_lct.c" "mg_lct_fmt.c" "mg_session.c"
        # Sealed device-session credentials (mgcommands.h, Sealed credentials): the
        # seal, vendored unchanged, on this component's token proof crypto;
        # and the credentials-only device session (layer 2) a Wi-Fi
        # gadget runs (mg_proto.c, cfg.creds_session).
        "${portable}/mg_device_session/src/mg_session_seal.c" "mg_seal_crypto.c"
        "${portable}/mg_device_session/src/mg_device_session.c")
    # Which link closes our code asked for: a 0x216 disconnect without one was NimBLE's own (mg_link_close.h).
    list(APPEND srcs "mg_link_close.c")
    set(mg_link_close 1)
    if(CONFIG_MUSE_GADGET_BLE_HID)
        list(APPEND srcs "mg_hogp.c")
    endif()
    if(CONFIG_MUSE_GADGET_BLE_SMP)
        # Firmware updates over BLE (MCUmgr SMP): the SMP
        # core (smp/, vendored) under the ESP-IDF backend, behind the request
        # front that asks for the confirm press (mg_smp_front.h).
        list(APPEND srcs "smp/mg_smp.c" "smp/mg_smp_sha256.c" "mg_smp_esp.c" "mg_smp_esp_util.c" "mg_smp_front.c")
    endif()
    if(NOT CONFIG_BT_NIMBLE_GATT_CLIENT)
        # A phone's indications confirmed without NimBLE's GATT client (mg_att_confirm.c).
        list(APPEND srcs "mg_att_confirm.c")
        set(mg_att_confirm 1)
    endif()
else()
    set(srcs)
endif()

# Requirements resolve before Kconfig, so they're unconditional.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" ${protocols}
    PRIV_INCLUDE_DIRS "." "smp" "haptics" "seal" "${portable}/mg_device_session/include"
    LDFRAGMENTS "linker.lf"   # CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM
    REQUIRES muse_vstream   # video: the receive ring every transport shares (mg_video.h)
    PRIV_REQUIRES bt nvs_flash mbedtls esp_partition esp_timer esp_app_format esp_bootloader_format bootloader_support app_update libsbc liblc3 muse_restart muse_image_id
                 secure_random   # every security draw (op_random, unlock_rng)
)
if(srcs)
    # mg_session_cap_sealed_clear in layer 2 too, as mg_ble.c's MG_SEALED_CLEAR.
    if(CONFIG_MUSE_GADGET_SEALED_CLEAR)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MGDS_CAP_SEALED_CLEAR=1)
    endif()
    # The vendored haptics core takes this repo's protocol header (haptics/VENDORED.md).
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "MGH_MGCOMMANDS_H=\"mgcommands.h\"")
endif()
if(mg_link_close)
    # Every ble_gap_terminate() in the image is noted (NimBLE's own timeouts use ble_gap_terminate_with_conn).
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_gap_terminate"
                          "-Wl,-u,__wrap_ble_gap_terminate")
endif()
if(mg_att_confirm)
    # -u: nothing else pulls mg_att_confirm.o out of the archive before libbt asks for it.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_att_svr_tx_error_rsp"
                          "-Wl,-u,__wrap_ble_att_svr_tx_error_rsp")
endif()
