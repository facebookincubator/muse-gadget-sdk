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

idf_component_register(
    SRCS "mosaico_entry.c"
    REQUIRES esp_iris esp_mosaico_app_recovery muse
    WHOLE_ARCHIVE)
# The board support package joins the build once the configuration names the
# board (components/muse/idf_component.yml), too late to be listed above.
idf_component_optional_requires(PRIVATE esp-mosaico-bsp)
# mosaico_entry.c runs ahead of the gadget's app_main().
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=app_main")
