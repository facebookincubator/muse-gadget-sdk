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

#pragma once

/*
 * Where the musegadgets client is, for the screen and the log
 * (mg_ble_link()). Ready, and only ready, means a client is connected,
 * subscribed to Control and Data and has push-to-talk on.
 */
typedef enum {
    MG_LINK_NEVER,           /* no client since boot, and no proof key: nobody has set it up */
    MG_LINK_DISCONNECTED,    /* no connection */
    MG_LINK_CONNECTING,      /* connected, not yet subscribed */
    MG_LINK_SESSION,         /* a client's session is up, push-to-talk off */
    MG_LINK_READY,           /* ...and push-to-talk on */
} mg_link_t;

const char *mg_link_name(mg_link_t link);
