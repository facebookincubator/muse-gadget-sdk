# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
cmake_minimum_required(VERSION 3.24)
project(avatar_webp_test C)
set(CMAKE_C_STANDARD 11)
add_executable(avatar_webp_test harness.c ../../main/avatar_webp.c)
target_include_directories(avatar_webp_test PRIVATE ../../main)
include(../../cmake/avatar_webp.cmake)
muse_avatar_add_webp(avatar_webp_test)
target_compile_options(avatar_webp_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(avatar_webp_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_libraries(avatar_webp_test PRIVATE m)
enable_testing()
add_test(NAME avatar_webp COMMAND avatar_webp_test "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set_tests_properties(avatar_webp PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")
