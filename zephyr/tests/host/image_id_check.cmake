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

# cmake -DTMP=<empty dir> -P image_id_check.cmake: every variant pair at a
# commit differs, each variant rises with N, the top byte stays free, and a
# newer commit's build number is bigger whatever the variants; the variant
# cmake/mg_version.cmake reads from each way an option reaches the image, and
# the check after Kconfig (mg_image_variant_check()) passing a match and
# failing a mismatch, never printing the (fake) token.

# Quoted arguments are strings, never variable names (CMake 3.x under -P keeps
# the old behavior, and "n" would read the loop variable below).
cmake_policy(SET CMP0054 NEW)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/mg_image_id.cmake)

# The check after Kconfig in a cmake of its own (a FATAL_ERROR ends it).
if(ROW STREQUAL "check")
  mg_image_variant_check(${V})
  message("check: ok")
  return()
endif()
# Every variant (bits 0-3) with every test shape (bits 4-6): 0-127.
set(all "")
foreach(v RANGE 0 127)
  list(APPEND all ${v})
endforeach()
set(seen "")
foreach(n 0 1 1218 1219 131071)
  foreach(v ${all})
    mg_image_build(b ${n} ${v})
    list(FIND seen ${b} i)
    if(i GREATER -1)
      message(FATAL_ERROR "collision: N ${n} variant ${v} -> ${b}")
    endif()
    list(APPEND seen ${b})
    if(b GREATER_EQUAL 16777216)
      message(FATAL_ERROR "N ${n} variant ${v} -> ${b} uses the personalization byte")
    endif()
  endforeach()
endforeach()
foreach(v 0 3 11 27 123 127)
  foreach(w 0 2 10 26 106 127)
    mg_image_build(a 1218 ${v})
    mg_image_build(c 1219 ${w})
    if(NOT c GREATER a)
      message(FATAL_ERROR "N 1219 variant ${w} (${c}) isn't above N 1218 variant ${v} (${a})")
    endif()
  endforeach()
endforeach()
# A bench build and its never-healthy and early-hang shapes, of one commit:
# different ids and firmware strings.
mg_test_shape_bits(nr "CONFIG_MG_BENCH_BOARD_NOT_READY")
mg_test_shape_bits(nh "CONFIG_MG_BENCH_EARLY_HANG")
math(EXPR vb "2 | 8")
math(EXPR vnr "${vb} | ${nr}")
math(EXPR vnh "${vb} | ${nh}")
mg_image_build(ib 531 ${vb})
mg_image_build(inr 531 ${vnr})
mg_image_build(inh 531 ${vnh})
if(ib EQUAL inr OR ib EQUAL inh OR inr EQUAL inh)
  message(FATAL_ERROR "a bench and a test-shaped bench build share an id (${ib} ${inr} ${inh})")
endif()
foreach(pair "${vnr};.bench.notready" "${vnh};.bench.earlyhang")
  list(GET pair 0 v)
  list(GET pair 1 want)
  mg_fw_variant_suffix(s ${v})
  if(NOT s STREQUAL want)
    message(FATAL_ERROR "suffix for ${v}: '${s}', want '${want}'")
  endif()
endforeach()
# An automation and a release build of one commit never share an id or a
# firmware string.
mg_image_build(rel 1245 1)
mg_image_build(aut 1245 9)
if(rel EQUAL aut)
  message(FATAL_ERROR "release and automation token builds share ${rel}")
endif()
foreach(pair "3;.tok.bench" "11;.tok.bench" "10;.bench" "9;.tok.auto" "8;.auto" "1;.tok" "0;")
  list(GET pair 0 v)
  list(LENGTH pair len)
  set(want "")
  if(len GREATER 1)
    list(GET pair 1 want)
  endif()
  mg_fw_variant_suffix(s ${v})
  if(NOT s STREQUAL want)
    message(FATAL_ERROR "suffix for ${v}: '${s}', want '${want}'")
  endif()
endforeach()
mg_automation_default(d)
if(NOT d STREQUAL "n")
  message(FATAL_ERROR "Kconfig.mg's MG_AUTOMATION default read as '${d}', not n")
endif()

if(NOT DEFINED TMP OR NOT IS_DIRECTORY "${TMP}")
  message(FATAL_ERROR "pass -DTMP=<an empty directory>")
endif()
set(VERSION_CMAKE ${CMAKE_CURRENT_LIST_DIR}/../../cmake/mg_version.cmake)
# Not a token: the rows check that it never shows in any output.
set(FAKE "mgst_fake_image_id_row")
set(APP ${TMP}/app)
file(MAKE_DIRECTORY ${APP})
file(WRITE ${APP}/prj.conf "CONFIG_MG_LC3=y\nCONFIG_MG_BENCH=y\n")
file(COPY ${CMAKE_CURRENT_LIST_DIR}/../../overlay-bench.conf DESTINATION ${APP})
file(WRITE ${APP}/tok.conf "CONFIG_MG_SDK_TOKEN=\"${FAKE}\"\n")
file(WRITE ${APP}/lab.conf "CONFIG_MG_AUTOMATION=y\n")
file(WRITE ${APP}/noconfirm.conf "CONFIG_MG_BENCH_NO_CONFIRM=y\n")
file(WRITE ${APP}/extra.conf "CONFIG_MG_LC3=y\n")
file(COPY ${CMAKE_CURRENT_LIST_DIR}/../../overlay-release.conf DESTINATION ${APP})

# variant(<want> <args...>): the token, bench, automation and test-shape bits of the image version
# mg_version.cmake works out, run as cmake -P from the app's directory
# without git (quicker: the bits are the same; its warning is checked too)
# with each -D argument, ENV:NAME=value in its environment, and SB:<line> in the
# image's sysbuild cache (SYSBUILD_NAME zephyr, the main app; "," is the
# cache's escaped list separator).
function(variant want)
  set(defs "")
  set(env "")
  set(sb "")
  foreach(a ${ARGN})
    if(a MATCHES "^ENV:(.*)$")
      list(APPEND env "${CMAKE_MATCH_1}")
    elseif(a MATCHES "^SB:(.*)$")
      string(REPLACE "," "\\;" l "${CMAKE_MATCH_1}")
      string(APPEND sb "${l}\n")
    else()
      string(REPLACE "," "\;" a "${a}")
      list(APPEND defs "${a}")
    endif()
  endforeach()
  if(NOT sb STREQUAL "")
    file(WRITE ${TMP}/sysbuild_cache.txt
         "SYSBUILD_NAME:STRING=zephyr\nSYSBUILD_MAIN_APP:BOOL=True\n${sb}")
    list(APPEND defs -DSYSBUILD=ON -DSYSBUILD_CACHE=${TMP}/sysbuild_cache.txt)
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E env ${env}
                          ${CMAKE_COMMAND} -DGIT_EXECUTABLE=${TMP}/no-git ${defs}
                          -P ${VERSION_CMAKE}
                  WORKING_DIRECTORY ${APP}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  string(FIND "${out}" "${FAKE}" i)
  if(NOT i EQUAL -1)
    message(FATAL_ERROR "variant '${ARGN}' printed the token")
  endif()
  if(NOT rc EQUAL 0 OR NOT out MATCHES "\\(image [0-9]+\\.[0-9]+\\.[0-9]+\\+([0-9]+)\\)")
    message(FATAL_ERROR "variant '${ARGN}': rc ${rc}: ${out}")
  endif()
  math(EXPR got "${CMAKE_MATCH_1} % 128 & 123")
  if(NOT out MATCHES "no git: the image id is")
    message(FATAL_ERROR "variant '${ARGN}': no warning for a build without git: ${out}")
  endif()
  if(NOT got EQUAL want)
    message(FATAL_ERROR "variant '${ARGN}' read as ${got}, not ${want}: ${out}")
  endif()
endfunction()

variant(0)                                                # prj.conf alone
variant(1 -DEXTRA_CONF_FILE=tok.conf)
variant(1 -DOVERLAY_CONFIG=${APP}/tok.conf)
variant(1 -DCONFIG_MG_SDK_TOKEN=${FAKE})                  # -D without sysbuild
variant(1 SB:EXTRA_CONF_FILE:UNINITIALIZED=tok.conf)      # west build --sysbuild -- -DEXTRA_CONF_FILE=
variant(1 "SB:zephyr_CONFIG_MG_SDK_TOKEN:STRING=\"${FAKE}\"") # -Dzephyr_CONFIG_MG_SDK_TOKEN=
variant(1 "SB:CONFIG_MG_SDK_TOKEN:UNINITIALIZED=\"${FAKE}\"")
variant(1 ENV:EXTRA_CONF_FILE=tok.conf)
variant(10 -DEXTRA_CONF_FILE=overlay-bench.conf)          # the marker and automation
variant(8 -DEXTRA_CONF_FILE=lab.conf)                     # automation alone: .auto
variant(8 -DCONFIG_MG_AUTOMATION=y)
variant(8 SB:zephyr_CONFIG_MG_AUTOMATION:BOOL=y)
variant(0 -DCONFIG_MG_AUTOMATION=n)
variant(0 -DEXTRA_CONF_FILE=overlay-release.conf)
variant(1 "-DEXTRA_CONF_FILE=overlay-release.conf,tok.conf") # a release with the token
variant(10 -DCONF_FILE=prj.conf,overlay-bench.conf)
variant(10 SB:CONF_FILE:STRING=prj.conf,overlay-bench.conf)
variant(50 -DEXTRA_CONF_FILE=noconfirm.conf)
variant(50 -DCONFIG_MG_BENCH_NO_CONFIRM=y)   # bench + test shape 3
variant(2 -DCONFIG_MG_BENCH_BUILD=y)
variant(0 -DCONFIG_MG_BENCH=y)                            # the console, in every build
variant(11 "-DEXTRA_CONF_FILE=overlay-bench.conf,tok.conf")
# The documented bench + token build under sysbuild: the list reaches the
# sysbuild cache with its separator escaped.
variant(11 SB:EXTRA_CONF_FILE:UNINITIALIZED=overlay-bench.conf,${APP}/tok.conf)
variant(10 SB:EXTRA_CONF_FILE:UNINITIALIZED=overlay-bench.conf,${APP}/extra.conf)
variant(3 -DMG_BUILD_VARIANT=3)
variant(9 -DMG_BUILD_VARIANT=9)
variant(1 SB:zephyr_MG_BUILD_VARIANT:STRING=1)

# check(<fails> <variant> <text> [-D...]): mg_image_variant_check() as after
# Kconfig with those CONFIG_ values.
function(check fails v text)
  execute_process(COMMAND ${CMAKE_COMMAND} -DROW=check -DV=${v} ${ARGN}
                          -P ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  string(REPLACE "\n" " " out "${out}")
  if((fails AND rc EQUAL 0) OR (NOT fails AND NOT rc EQUAL 0))
    message(FATAL_ERROR "check ${v} '${ARGN}': rc ${rc}: ${out}")
  endif()
  string(FIND "${out}" "${text}" i)
  if(i EQUAL -1)
    message(FATAL_ERROR "check ${v} '${ARGN}': no '${text}' in: ${out}")
  endif()
  string(FIND "${out}" "${FAKE}" i)
  if(NOT i EQUAL -1)
    message(FATAL_ERROR "check ${v} '${ARGN}' printed the token")
  endif()
endfunction()
check(0 0 "check: ok")
check(0 0 "check: ok" -DCONFIG_MG_SDK_TOKEN= -DCONFIG_MG_BENCH=y -DCONFIG_MG_LC3=y)
check(0 1 "check: ok" -DCONFIG_MG_SDK_TOKEN=${FAKE})
check(0 9 "check: ok" -DCONFIG_MG_SDK_TOKEN=${FAKE} -DCONFIG_MG_AUTOMATION=y)
check(0 12 "check: ok" -DCONFIG_MG_AUTOMATION=y)              # the dirty bit isn't checked
check(0 10 "check: ok" -DCONFIG_MG_AUTOMATION=y -DCONFIG_MG_BENCH_BUILD=y)
check(0 50 "check: ok" -DCONFIG_MG_BENCH_NO_CONFIRM=y)          # bench + test shape 3
check(1 2 "-DMG_BUILD_VARIANT=50" -DCONFIG_MG_BENCH_NO_CONFIRM=y) # the shape not guessed
check(1 18 "two test-only image shapes" -DCONFIG_MG_BENCH_BOARD_NOT_READY=y
      -DCONFIG_MG_BENCH_EARLY_HANG=y)
check(1 0 "-DMG_BUILD_VARIANT=1" -DCONFIG_MG_SDK_TOKEN=${FAKE})
check(1 0 "-DMG_BUILD_VARIANT=8" -DCONFIG_MG_AUTOMATION=y)
check(1 8 "-DMG_BUILD_VARIANT=0")                             # a release guessed as automation
check(1 0 "-DMG_BUILD_VARIANT=34" -DCONFIG_MG_BENCH_EARLY_HANG=y)
check(1 2 "-DMG_BUILD_VARIANT=0")
check(1 1 "-DMG_BUILD_VARIANT=9" -DCONFIG_MG_SDK_TOKEN=${FAKE} -DCONFIG_MG_AUTOMATION=y)
check(1 8 "-DMG_BUILD_VARIANT=10" -DCONFIG_MG_AUTOMATION=y -DCONFIG_MG_BENCH_BUILD=y)
message("image_id: ok")
