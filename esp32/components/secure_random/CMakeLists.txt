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

# The security draws' one source (README.md): the hardware RNG, only while
# a radio is on. Every board. esp_wifi is only named here (main requires it on
# every build anyway); secure_random.c calls it only where Wi-Fi is linked, so
# BLE-only images stay without the Wi-Fi stack.
idf_component_register(
    SRCS "secure_random.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt esp_wifi esp_hw_support log
)
