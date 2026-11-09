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

# Only in ESP-Mosaico builds with esp-mosaico-utils (cmake/project.cmake adds
# this directory then): starts ESP-Iris for the board file, which calls
# muse_mosaico_iris_start() if it is linked in. WHOLE_ARCHIVE, since nothing
# else references it and the board file only declares it weak.
set(srcs "muse_mosaico_iris.c")
if(CONFIG_MUSE_POWER_TEST)
    list(APPEND srcs "muse_ptest_iris.c")
endif()
idf_component_register(
    SRCS ${srcs}
    PRIV_REQUIRES esp_iris esp_mosaico_app_recovery esp_pm
    WHOLE_ARCHIVE
)
