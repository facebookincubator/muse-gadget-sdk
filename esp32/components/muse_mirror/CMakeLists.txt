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

# The cable mirror (CONFIG_MUSE_MIRROR, tools/muse/mirror.py) and the virtual
# display board it makes possible. Without them this component builds nothing.
set(srcs)
if(CONFIG_MUSE_MIRROR)
    list(APPEND srcs "muse_mirror.c" "muse_mirror_session.c" "muse_mirror_core.c" "muse_mirror_audio.c")
endif()
if(CONFIG_MUSE_BOARD_VIRTUAL_DISPLAY)
    list(APPEND srcs "board_virtual_display.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES muse esp_timer
)
