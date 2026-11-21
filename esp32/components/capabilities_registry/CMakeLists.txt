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

# The device registry (include/capabilities_registry.h): drivers describe their devices,
# apps and device.guide read them. Small and dependency-free, so board files,
# Link's drivers and the apps runtime can all use it.
# Built only with CONFIG_CAPABILITIES_REGISTRY (WebAssembly apps select it); the
# requirement on it stays unconditional, as requirements resolve before Kconfig.
set(srcs)
if(CONFIG_CAPABILITIES_REGISTRY)
    list(APPEND srcs "capabilities_registry.c")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer
)
