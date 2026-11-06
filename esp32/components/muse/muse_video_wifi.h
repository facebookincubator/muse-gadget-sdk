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
 * Video over Wi-Fi (CONFIG_MUSE_VIDEO_WIFI): a TCP server on the local
 * network, announced over mDNS as _musevideo._tcp, that feeds muse_video.
 * One client at a time; a new connection replaces the old one. The wire
 * format is in ../../../protocols/VIDEO.md (Wi-Fi) and muse_video_wire.h.
 */

/* Starts the server task; it waits for Wi-Fi by itself. Once, from Muse's start. */
void muse_video_wifi_start(void);
