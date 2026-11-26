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

# Muse: the voice companion UI that runs beside Home Link on boards with a
# display (CONFIG_MUSE_BOARD_*). Link-only builds register an empty component,
# except the Voice PE, which borrows the Muse voice session.
#
# Requirements are resolved before Kconfig is known, so everything any board
# needs is listed unconditionally. Managed components (LVGL, codecs, BSPs) come
# from idf_component.yml, where rules keep them out of Link-only builds.
set(srcs)
set(embed)
if(CONFIG_MUSE_ENABLED)
    list(APPEND srcs
        "muse_app.c"
        "muse_f2.c"
        "muse_board.c"
        "muse_console.c"
        "muse_state.c"
        "muse_options.c"
        "muse_battery.c"
        "muse_battery_policy.c"
        "muse_charge_dark.c"
        "muse_charge_dark_core.c"
        "muse_charged.c"        # Sleep when charged: what counts as charged, pure C
        "muse_status_conn.c"
        "muse_mic_ring.c"
        "muse_turn_dot.c"
        "muse_clock.c"
        "muse_audio.c"
        "muse_spk_dsp.c"        # the speech chain (muse_board_t.speaker_speech_chain)
        "muse_audio_dma.c"
        "muse_audio_tx.c"       # the speaker's DMA, cleared at a barge-in's touch-down
        "muse_audio_tx_esp.c"
        "muse_pmu.c"
        "muse_input.c"
        "muse_voice.c"
        "muse_adpcm.c"
        "muse_settings.c"
        "muse_text.c"
        "muse_lang.c")
    if(CONFIG_MUSE_GADGET_BLE_AUDIO)
        # The passcode lock, with the v1.0 home (muse_home_ui.h): without them, the headers' stubs.
        list(APPEND srcs
            "muse_passcode_core.c"  # its rules, pure C
            "muse_passcode.c"
            "muse_passcode_esp.c")
    endif()
    list(APPEND srcs
        "muse_link.c"
        "muse_ble.c"
        "muse_ble_gate.c")
    if(CONFIG_MUSE_UI_THEME)
        list(APPEND srcs "muse_theme.c" "muse_theme_policy.c" "muse_theme_lvgl.c")
    endif()
    if(CONFIG_MUSE_WAKEWORD_ENABLE)
        # Wake word tap and task; without it, muse_wakeword_glue.h's inline stubs.
        list(APPEND srcs "muse_wakeword_glue.c")
    endif()
    if(CONFIG_MUSE_AVATAR_SYNC)
        list(APPEND srcs "muse_avatar_render.c")
        if(CONFIG_MUSE_UI_THEME)
            list(APPEND srcs "muse_avatar_background.c" "muse_avatar_theme.c")
        endif()
    endif()
    if(CONFIG_MUSE_MOTION_GESTURES)
        list(APPEND srcs "muse_motion.c" "muse_motion_core.c" "boards/qmi8658_motion.c")
    endif()
    if(CONFIG_MUSE_BOARD_SENSECAP_WATCHER OR CONFIG_MUSE_LATEST_REPLY)
        list(APPEND srcs "muse_review.c")
    endif()
    if(CONFIG_MUSE_LATEST_REPLY)
        list(APPEND srcs "muse_latest_reply.c")
    endif()
    if(CONFIG_MUSE_UI_FREEINK)
        # E-paper: FreeInk draws the face, Settings and the keyboard.
        list(APPEND srcs "eink/muse_eink_ui.cpp" "eink/muse_eink_settings.cpp" "eink/muse_eink_fonts.cpp")
        # The LVGL UI's fonts, as the TTFs LVGL builds them from (eink/fonts/).
        list(APPEND embed "eink/fonts/montserrat.ttf" "eink/fonts/symbols.ttf" "eink/fonts/unscii.ttf")
    else()
        # The LVGL UI: the face, home, menu, Settings, keypad and passcode screens. The v1.0 home's icon set and
        # passcode screens only with BLE gadget audio (muse_home_ui.h).
        list(APPEND srcs "muse_ui.c")
        if(CONFIG_MUSE_GADGET_BLE_AUDIO)
            list(APPEND srcs "muse_icons.c")
        endif()
        list(APPEND srcs "muse_home.c" "muse_type.c" "muse_touch.c"
                         "muse_options_ui.c" "muse_menu.c" "muse_settings_ui.c" "muse_keypad.c")
        if(CONFIG_MUSE_GADGET_BLE_AUDIO)
            list(APPEND srcs "muse_passcode_ui.c")
        endif()
        list(APPEND srcs "muse_lv_mem.c")
    endif()
    if(CONFIG_MUSE_HATCH)
        list(APPEND srcs "muse_chat.c" "muse_account_api.c" "muse_chat_session.cpp" "muse_options_json.c")
        list(APPEND embed "test_reply.mp3")   # muse_hatch_mp3_selftest
        if(CONFIG_MUSE_RESEND_NOTES)
            list(APPEND srcs "muse_resend.c")   # when a note that didn't send goes again
        endif()
        if(CONFIG_MUSE_PHOTO_NOTES)
            list(APPEND srcs "muse_photo_note.c" "muse_chat_image.c")   # a photo with each voice note
        endif()
        if(CONFIG_MUSE_REALTIME_VOICE)
            list(APPEND srcs "muse_voice_call.cpp" "muse_voice_call_core.c")
            if(CONFIG_MUSE_VOICE_SOFTWARE_AEC)
                list(APPEND srcs "muse_aec.c")
            endif()
        endif()
    else()
        list(APPEND srcs "muse_chat_link.c")   # text replies over Link's session
    endif()
    list(APPEND srcs "muse_chat_text.c")
    if(CONFIG_MUSE_CJK_FONT_NOTO)
        list(APPEND srcs "fonts/muse_font_cjk_noto_16.c")   # captions' CJK fallback
    elseif(CONFIG_MUSE_CJK_FONT)
        list(APPEND srcs "fonts/muse_font_cjk_16.c")
    endif()
    if(CONFIG_MUSE_LATIN_FONT)
        list(APPEND srcs "fonts/muse_font_latin_16.c")   # captions with accented letters
    endif()
    if(CONFIG_MUSE_EMOJI)
        list(APPEND srcs "fonts/muse_font_emoji_16.c")   # captions' emoji
    endif()
    if(CONFIG_MUSE_UI_LANG_VI)
        # Montserrat's Vietnamese letters, for the menu and the pairing card
        list(APPEND srcs "fonts/muse_font_vi_12.c" "fonts/muse_font_vi_14.c"
                         "fonts/muse_font_vi_20.c" "fonts/muse_font_vi_28.c")
    endif()
    if(CONFIG_MUSE_UI_LANG_PT_BR)
        # Montserrat's Portuguese letters: menu, pairing card, settings pages
        list(APPEND srcs "fonts/muse_font_pt_12.c" "fonts/muse_font_pt_14.c" "fonts/muse_font_pt_16.c"
                         "fonts/muse_font_pt_20.c" "fonts/muse_font_pt_28.c")
    endif()
    # The v1.0 UI's type at the panel's size (muse_type.c, tools/muse/gen_ui_assets.py):
    # only the board's own set, with CONFIG_MUSE_UI_PANEL_TYPE.
    set(type_px "")
    if(CONFIG_MUSE_BOARD_M5STACK_STOPWATCH)
        set(type_px 466)
    elseif(CONFIG_MUSE_BOARD_ESP_MOSAICO)
        set(type_px 480)
    elseif(CONFIG_MUSE_BOARD_SENSECAP_WATCHER)
        set(type_px 412)
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_C6_18)
        set(type_px 368)
    endif()
    if(type_px AND CONFIG_MUSE_UI_PANEL_TYPE)
        foreach(role time date title conn row label value small digit)
            list(APPEND srcs "fonts/type/muse_font_${role}_${type_px}.c")
        endforeach()
    endif()
    if(CONFIG_MUSE_PERF)
        list(APPEND srcs "muse_perf.c")
    endif()
    if(CONFIG_LV_USE_PROFILER)
        list(APPEND srcs "lvprof/muse_lv_profiler.c")   # LVGL's profiler hooks, for mg.perf
    endif()
    # Your own character (tools/muse/AVATAR_RECIPE.md) stands in for the default
    # avatar when avatar/muse_pixel.c exists here; this avatar/ is gitignored. The
    # default is in avatar/ at the top of the project.
    file(GLOB custom_avatar CONFIGURE_DEPENDS "${COMPONENT_DIR}/avatar/muse_pixel.c")
    if(custom_avatar)
        message(STATUS "Custom avatar: components/muse/avatar/muse_pixel.c")
        list(APPEND srcs "avatar/muse_pixel.c")
    else()
        list(APPEND srcs "../../avatar/muse_pixel.c")
        if(NOT CONFIG_MUSE_GADGET_BLE_AUDIO)
            # Its rings' angle slots (1 KB of internal RAM) only with BLE gadget audio: the same pixels without.
            set_source_files_properties("../../avatar/muse_pixel.c" PROPERTIES COMPILE_DEFINITIONS
                                        "MUSE_PIXEL_RING_CACHE=0")
        endif()
    endif()
    if(CONFIG_MUSE_BOARD_WAVESHARE_S3_175C OR CONFIG_MUSE_BOARD_WAVESHARE_S3_175)
        # One driver: the 1.75 differs only in pins (its BSP's) and its PWR key.
        list(APPEND srcs "boards/board_waveshare_s3_175c.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_OSTB_ECHOEAR_2ST)
        list(APPEND srcs "boards/board_ostb_echoear_2st.c")
    elseif(CONFIG_MUSE_BOARD_RETERMINAL_STICKY)
        list(APPEND srcs "boards/board_reterminal_sticky.c")
        list(APPEND srcs "boards/board_reterminal_sticky_eink.cpp")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_216)
        list(APPEND srcs "boards/board_waveshare_s3_216.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_206)
        list(APPEND srcs "boards/board_waveshare_s3_206.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_185B)
        list(APPEND srcs "boards/board_waveshare_s3_185b.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_185C)
        list(APPEND srcs "boards/board_waveshare_s3_185c.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_143C)
        list(APPEND srcs "boards/board_waveshare_s3_143c.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_143)
        list(APPEND srcs "boards/board_waveshare_s3_143.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_146)
        list(APPEND srcs "boards/board_waveshare_s3_146.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_LILYGO_S3_AMOLED_175)
        list(APPEND srcs "boards/board_lilygo_s3_amoled_175.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_ESP_MOSAICO)
        list(APPEND srcs "boards/board_esp_mosaico.c" "boards/muse_lcd_bands.c")
        if(CONFIG_MUSE_MOSAICO_CAMERA)
            list(APPEND srcs "boards/mosaico_camera.c")
        endif()
    elseif(CONFIG_MUSE_BOARD_DFROBOT_UNIHIKER_K10)
        list(APPEND srcs "boards/board_dfrobot_unihiker_k10.c")
    elseif(CONFIG_MUSE_BOARD_AIPI)
        list(APPEND srcs "boards/board_aipi.c")
    elseif(CONFIG_MUSE_BOARD_ATK_DNESP32S3_BOX)
        list(APPEND srcs "boards/board_atk_dnesp32s3_box.c")
    elseif(CONFIG_MUSE_BOARD_ATK_DNESP32S3_BOX2)
        list(APPEND srcs "boards/board_atk_dnesp32s3_box2.c")
    elseif(CONFIG_MUSE_BOARD_ESPRESSIF_BOX_3)
        list(APPEND srcs "boards/board_espressif_box_3.c" "boards/box_3_microphone.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_C6_18)
        list(APPEND srcs "boards/board_waveshare_c6_18.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_18)
        list(APPEND srcs "boards/board_waveshare_s3_18.c" "boards/muse_lcd_bands.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_C6_206)
        list(APPEND srcs "boards/board_waveshare_c6_206.c")
    elseif(CONFIG_MUSE_BOARD_SENSECAP_WATCHER)
        list(APPEND srcs "boards/board_sensecap_watcher.c" "boards/muse_lcd_bands.c")
        if(CONFIG_MUSE_WATCHER_CAMERA)
            list(APPEND srcs "boards/watcher_camera.c")
        endif()
    elseif(CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV)
        list(APPEND srcs "boards/board_m5stack_cardputer_adv.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_ATOMS3)
        list(APPEND srcs "boards/board_m5stack_atoms3.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_STICKS3)
        list(APPEND srcs "boards/board_m5stack_sticks3.c")
    elseif(CONFIG_MUSE_BOARD_CROWPANEL_ADVANCE_43)
        list(APPEND srcs "boards/board_crowpanel_advance_43.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_STICKC_PLUS2)
        list(APPEND srcs "boards/board_m5stack_stickc_plus2.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY)
        list(APPEND srcs "boards/board_m5stack_stopwatch.c" "boards/muse_lcd_bands.c")
        if(CONFIG_CAPABILITIES_REGISTRY)
            # BMI270 registry/lifecycle adapter; vendor driver is a gated managed dependency.
            list(APPEND srcs "boards/bmi270_device.c" "boards/imu_events.c")
        endif()
    elseif(CONFIG_MUSE_BOARD_SEEED_XIAO_S3_147)
        list(APPEND srcs "boards/board_seeed_xiao_s3_147.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_CORES3 OR CONFIG_MUSE_BOARD_M5STACK_CORES3LITE)
        list(APPEND srcs "boards/board_m5stack_cores3.c")
    elseif(CONFIG_MUSE_BOARD_M5STACK_CORE2)
        list(APPEND srcs "boards/board_m5stack_core2.c")
    elseif(CONFIG_MUSE_BOARD_FNK0104B)
        list(APPEND srcs "boards/board_fnk0104b.c")
    elseif(CONFIG_MUSE_BOARD_GUITION_JC3248W535)
        list(APPEND srcs "boards/board_guition_jc3248w535.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_LCD7)
        list(APPEND srcs "boards/board_waveshare_s3_lcd7.c")
    elseif(CONFIG_MUSE_BOARD_SPOTPEAR_ESP32_AI_128)
        list(APPEND srcs "boards/board_spotpear_esp32_ai_128.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_LCD128)
        list(APPEND srcs "boards/board_waveshare_s3_lcd128.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_LCD349)
        list(APPEND srcs "boards/board_waveshare_s3_lcd349.c")
    elseif(CONFIG_MUSE_BOARD_VN_S3_183)
        list(APPEND srcs "boards/board_vn_s3_183.c")
    elseif(CONFIG_MUSE_BOARD_AI_PASSPORT)
        list(APPEND srcs "boards/board_ai_passport.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_P4_LCD35)
        list(APPEND srcs "boards/board_waveshare_p4_lcd35.c")
        if(CONFIG_MUSE_P4_CSI_CAMERA)
            list(APPEND srcs "boards/p4_csi_camera.c")
        endif()
    elseif(CONFIG_MUSE_BOARD_DEOTALAND_ROROLEE)
        list(APPEND srcs "boards/board_deotaland_rorolee.c")
    elseif(CONFIG_MUSE_BOARD_VIEWE_SMARTRING_PLUS)
        list(APPEND srcs "boards/board_viewe_smartring_plus.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_LCD19)
        list(APPEND srcs "boards/board_waveshare_s3_lcd19.c")
    elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_LCD_35)
        list(APPEND srcs "boards/board_waveshare_s3_lcd_35.c")
    endif()
    if(CONFIG_MUSE_VIDEO OR CONFIG_MUSE_AVATAR_ANIMATIONS)
        list(APPEND srcs "muse_vdec.c")
        list(APPEND srcs "muse_vdec_same.c")   # the decoder's map of unchanged macroblocks
    endif()
    if(CONFIG_MUSE_AVATAR_ANIMATIONS)
        list(APPEND srcs "muse_avatar_motion.c" "muse_avatar_pack.c")
    endif()
    if(CONFIG_MUSE_VIDEO)
        list(APPEND srcs "muse_yuv.c")
        # Phone camera: decode and draw (muse_video), and the transports that feed it.
        list(APPEND srcs "muse_video.c")
        list(APPEND srcs "muse_ui_fb.c" "muse_video_tick.c" "muse_video_fit.c" "muse_vdirty.c")
        if(CONFIG_IDF_TARGET_ESP32S3)
            list(APPEND srcs "muse_yuv_pie.S")   # the S3's SIMD colour conversion
        elseif(CONFIG_IDF_TARGET_ESP32S31)
            list(APPEND srcs "muse_yuv_pie_s31.S")   # the same on the S31's SIMD (checked at first use)
        endif()
        if(CONFIG_MUSE_VIDEO_WIFI)
            list(APPEND srcs "muse_video_wifi.c" "muse_video_wire.c" "muse_video_wifi_gate.c")
        endif()
    endif()
    if(CONFIG_CAPABILITIES_REGISTRY)
        list(APPEND srcs "board_capabilities.c")    # the battery as a device for apps (components/capabilities_registry)
    endif()
    if(CONFIG_MUSE_APP_VIEW)
        list(APPEND srcs "muse_app_view.c")   # the screen an app draws on (components/muse_apps)
    endif()
    # Shared core, independent of board selection. Keep it after the BSP so
    # Watcher's independently retained recovery markers keep their link order.
    list(APPEND srcs "muse_lvgl_hold.c")   # who stops LVGL (the screen off, video)
    if(CONFIG_MUSE_POWER_TEST)
        list(APPEND srcs "muse_power_test.c")
        if(CONFIG_MUSE_BOARD_RETERMINAL_STICKY)
            list(APPEND srcs "boards/board_reterminal_sticky_power_test.c" "boards/sticky_power_test_display.cpp")
        endif()
        if(CONFIG_MUSE_BOARD_WAVESHARE_S3_175C OR CONFIG_MUSE_BOARD_WAVESHARE_S3_216 OR CONFIG_MUSE_BOARD_AIPI OR CONFIG_MUSE_BOARD_ESP_MOSAICO)
            list(APPEND srcs "boards/board_power_test_generic.c")
        endif()
    endif()
elseif(CONFIG_HOMEHUB_VOICE)
    # The Voice PE board uses just the Muse voice session; the project's main/
    # voice_muse_chat.c stands in for the Muse settings, Wi-Fi and Link hooks.
    list(APPEND srcs "muse_chat.c" "muse_account_api.c" "muse_chat_session.cpp" "muse_chat_text.c" "muse_text.c")
    # The shared session handles option events even without a touchscreen.
    list(APPEND srcs "muse_options.c" "muse_options_json.c")
    list(APPEND embed "test_reply.mp3")
    if(CONFIG_MUSE_REALTIME_VOICE)
        list(APPEND srcs "muse_voice_call.cpp" "muse_voice_call_core.c")
    endif()
endif()

set(muse_priv_requires
    muse_restart secure_random
    esp_timer esp_driver_usb_serial_jtag esp_psram nvs_flash esp_pm esp_event esp_netif
    bt esp-tls mbedtls esp_app_format esp_http_client network_proxy
    noise_core minimp3 muse_vstream tinyh264 libvpx_vp8 muse_mirror muse_gadget_ble capabilities_registry
    esp_lcd esp_adc esp_driver_i2s esp_driver_ledc esp_driver_spi esp_driver_uart esp_wifi
    esp_driver_pcnt efuse bootloader_support app_update esp_partition esp_mm
    # The Sticky's microSD (CONFIG_MUSE_STICKY_SD). Requirements are resolved
    # before Kconfig values exist, so these can't depend on it; only the code does.
    fatfs esp_driver_sdspi sdmmc)
list(APPEND muse_priv_requires camera)
# The P4 camera's JPEG encoder; Kconfig is not loaded yet when requirements expand.
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND muse_priv_requires esp_driver_jpeg)
endif()
# The Mosaico camera's JPEG encoder and PPA; Kconfig is not loaded yet when
# requirements expand.
if(IDF_TARGET STREQUAL "esp32s31")
    list(APPEND muse_priv_requires esp_driver_jpeg esp_driver_ppa)
endif()
list(APPEND muse_priv_requires muse_wakeword)   # registers empty unless MUSE_WAKEWORD_ENABLE

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_FILES ${embed}
    LDFRAGMENTS "linker.lf"   # CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM
    WHOLE_ARCHIVE   # muse_lv_mem.c provides symbols only LVGL references
    REQUIRES esp_driver_gpio esp_driver_i2c
    PRIV_REQUIRES ${muse_priv_requires}
)

if(CONFIG_MUSE_UI_FREEINK)
    # FreeInk's board profile, for the panel and its pins. PUBLIC: FreeInk's
    # headers read it too, in every file that includes them.
    idf_component_get_property(freeink_lib freeink-sdk COMPONENT_LIB)
    # One framebuffer, the e-ink UI's DisplayTarget's: the panel's RAM keeps
    # the previous frame for fast refreshes (as FreeInk's samples build it).
    target_compile_definitions(${freeink_lib} PUBLIC "FREEINK_DEVICE_${CONFIG_MUSE_FREEINK_DEVICE}=1"
                               "EINK_DISPLAY_SINGLE_BUFFER_MODE=1")
    # Muse's CJK font is LVGL's format: the e-ink UI reads it with LVGL's font
    # types only (eink/lvgl_font/lvgl.h), not LVGL.
    set_source_files_properties("fonts/muse_font_cjk_16.c" PROPERTIES
        COMPILE_OPTIONS "-I${COMPONENT_DIR}/eink/lvgl_font")
elseif(CONFIG_MUSE_ENABLED)
    # LVGL spins forever on a failed assert unless LV_ASSERT_HANDLER is set,
    # which its Kconfig can't do. Abort, so the panic handler prints a backtrace
    # and reboots. PUBLIC reaches LVGL and every file that includes lvgl.h.
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PUBLIC
        "LV_ASSERT_HANDLER_INCLUDE=<stdlib.h>"
        "LV_ASSERT_HANDLER=abort()\;")
    if(CONFIG_LV_USE_PROFILER)
        # LV_PROFILER_INCLUDE names muse_lv_profiler.h: LVGL and everything
        # that includes lvgl.h needs to find it.
        target_include_directories(${lvgl_lib} PUBLIC "${COMPONENT_DIR}/lvprof")
    endif()
endif()
