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

# Lua 5.4.9 for on-device scripts (README.md). Standard 64-bit integers and
# doubles: scripts written for Lua expect them.
# Only the Watcher example's scripts use it (main/muse_script_sandbox.c).
if(NOT CONFIG_MUSE_SCRIPTS)
    idf_component_register()
    return()
endif()

set(core lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
    lparser lstate lstring ltable ltm lundump lvm lzio)
set(libs lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib)
set(srcs)
foreach(f ${core} ${libs})
    list(APPEND srcs "src/${f}.c")
endforeach()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS "src")

# C-stack depth for a 32 KB script stack; pattern recursion; and the sandbox's
# hook into the pattern matcher (main/muse_script_sandbox.c defines it).
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LUAI_MAXCCALLS=48
    MAXCCALLS=64
    LUAI_MATCHSTEP_FUNC=sb_matchstep)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-implicit-fallthrough -Wno-unused-parameter)
