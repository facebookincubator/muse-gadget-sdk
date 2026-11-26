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
package com.musegadgets.test;

import java.util.UUID;

/** Constants from protocols/mgcommands.h (keep in step with it). */
final class Mg {
    private Mg() {}

    static final UUID SERVICE = UUID.fromString("65E6635D-174A-4AF0-805E-D1EDED7C2A63");
    static final UUID CONTROL = UUID.fromString("732CE4AB-9F36-4F11-9147-0BAC4FCE97C4");
    static final UUID DATA = UUID.fromString("5B84AFDF-3B31-4FEF-89E6-514A4A97ABB3");
    static final UUID CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    static final UUID NUS_TX = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
    static final UUID NUS_SERVICE = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");

    static final int CMD_START_MIC = 1;
    static final int CMD_STOP_MIC = 2;
    static final int CMD_CHANGE_DATA_TYPE = 8;
    static final int CMD_REQUEST_STATUS = 10;
    static final int CMD_SUPPORTED_FEATURES = 16;
    static final int CMD_ERROR = 25;
    static final int ERROR_PROOF_REQUIRED = 6;
    static final int CMD_STREAM_AUDIO = 29;
    static final int CMD_STREAM_VIDEO = 0x84;
    static final int CMD_UI_INPUT = 0x86;
    static final int SUB_FEATURE = 0xFE;

    /* mg_error_code_t names, by code. */
    static final String[] ERROR_NAMES = {"", "unsupported", "invalid length", "invalid value", "busy", "not found",
                                         "proof required", "insecure", "recovery", "press required"};

    /** An mg_error_code_t as a word ("busy"; "code N" for one this app doesn't know). */
    static String errorName(int code) {
        return code > 0 && code < ERROR_NAMES.length ? ERROR_NAMES[code] : "code " + code;
    }

    /* UI input (protocols/UI.md) */
    static final int UI_TOUCH_ENABLE = 0;
    static final int UI_TOUCH = 1;
    static final int UI_HID_MAP = 2;
    static final int UI_HID_STATUS = 3;
    static final int UI_FEATURE_TOUCH = 1;
    static final int UI_FEATURE_HID_DIGITIZER = 2;
    static final int UI_FEATURE_HID_MOUSE_ABS = 3;
    static final int UI_FEATURE_HID_MOUSE_REL = 4;
    static final int TOUCH_DOWN = 0;
    static final int TOUCH_MOVE = 1;
    static final int TOUCH_UP = 2;
    static final int HID_OFF = 0;
    static final int HID_DIGITIZER = 1;
    static final int HID_MOUSE_ABS = 2;
    static final int HID_MOUSE_REL = 3;
    static final int HID_MAP_FISHEYE = 1;
    static final int HID_MAP_OFFSET = 2;   /* x, y as (fraction + 0.5) / 2: the square may reach past the screen */
    static final int HID_STATUS_BONDED = 1;
    static final int HID_STATUS_HOST = 2;
    static final int UI_CONTROL = 4;
    static final int UI_FEATURE_CONTROLS = 5;
    /* mg_ui_control_t */
    static final int CTL_FISHEYE = 1;
    static final int CTL_PAN = 2;
    static final int CTL_QUALITY = 3;
    static final int CTL_STOP = 4;
    static final int CTL_MENU = 5;
    static final int CTL_EDGE_PAN = 6;
    static final int CTL_OVERSCROLL = 7;
    static final int UI_HID_KEY = 5;
    static final int TOUCH_ID_PAN = 0x80;   /* a touch event's id: the gesture pans the mirror (MG_TOUCH_ID_PAN) */

    static final int DATA_TYPE_VIDEO_H264 = 24;
    static final int DATA_TYPE_VIDEO_VP8 = 25;

    static final int V_REQUEST_CAPABILITIES = 0;
    static final int V_REPORT_CAPABILITIES = 1;
    static final int V_START_STREAMING = 2;
    static final int V_STOP_STREAMING = 3;
    static final int V_REQUEST_BUFFER_UPDATE = 4;
    static final int V_BUFFER_UPDATE = 5;
    static final int V_REQUEST_KEYFRAME = 6;
    static final int V_REQUEST_STREAMING = 7;

    static final int VCAP_CODECS = 1;
    static final int VCAP_DISPLAY = 2;
    static final int VCAP_MAX_SIZE = 3;
    static final int VCAP_MAX_FPS = 4;
    static final int VCAP_BUFFER_SIZE = 5;
    static final int VCAP_SUGGESTED = 6;
    static final int VCAP_L2CAP_PSM = 7;
    static final int VCAP_PLAYOUT = 8;
    static final int VCAP_TILES = 9;
    static final int VCAP_TILE_SPLIT = 10;
    static final int VCAP_UI = 11;
    static final int VCAP_UI_STILL = 12;   /* still patch formats: 1 << mg_video_still_format_t */

    static final int VP_CODEC = 1;
    static final int VP_WIDTH = 2;
    static final int VP_HEIGHT = 3;
    static final int VP_FPS = 4;
    static final int VP_BITRATE = 5;
    static final int VP_PLAYOUT = 6;
    static final int VP_TILES = 7;
    static final int VP_CONTENT = 8;
    static final int CONTENT_UI = 1;

    static final int BUFFER_DROP = 1;

    static final int FRAME_MAGIC = 0x56;
    static final int FRAME_HEADER_LEN = 8;
    static final int FRAME_KEYFRAME = 1;

    static int u16(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8;
    }

    static long u32(byte[] b, int o) {
        return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24;
    }
}
