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

# The optional M5Stack Core2 multi-app demo UI (CONFIG_MUSE_APPS_DEMO): a touch
# launcher and three sample apps drawn as the face inside the normal muse_ui
# flow. muse_ui.c calls muse_apps_demo_build()/muse_apps_demo_tick() behind the
# same option; without it this registers an empty component.
set(srcs)
if(CONFIG_MUSE_APPS_DEMO)
    list(APPEND srcs
        "muse_apps_demo.c"
        "app_manager.c"
        "apps/app_launcher.c"
        "apps/app_debugger.c"
        "apps/app_demo.c"
        "apps/pet_world.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    # muse publishes LVGL and the muse_*.h state/session APIs the apps read.
    PRIV_REQUIRES muse esp_timer esp_psram
)
