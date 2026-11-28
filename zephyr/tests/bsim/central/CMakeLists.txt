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

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mg_bsim_central)

set(MG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/mg_common.cmake)

add_subdirectory(${ZEPHYR_BASE}/tests/bsim/babblekit babblekit)
target_link_libraries(app PRIVATE babblekit)
target_include_directories(app PRIVATE ${MG_PROTOCOLS} ${MG_SRC})
zephyr_include_directories(
  $ENV{BSIM_COMPONENTS_PATH}/libUtilv1/src $ENV{BSIM_COMPONENTS_PATH}/libPhyComv1/src)

# The app's side of Link setup and the token proof, from the gadget's own
# portable sources.
target_sources(app PRIVATE src/main.c ${MG_SRC}/mg_json.c ${MG_SRC}/mg_psa.c
  ${MG_SRC}/mg_setup_crypto.c)
mg_add_transcript(app)

# The same codecs as the gadget, used here as decoders.
mg_add_sbc(app)
mg_add_lc3(app)
