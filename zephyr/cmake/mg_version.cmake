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

# Quoted arguments are strings, never variable names, also when this runs
# under cmake -P with no cmake_minimum_required (CMake 3.x keeps the old
# behavior there).
cmake_policy(SET CMP0054 NEW)

# The firmware version, worked out once, before Kconfig
# runs, from VERSION (MAJOR.MINOR.PATCH, the one place it is set) and git:
#
#   release  HEAD is the tag <MG_TAG_PREFIX><MAJOR.MINOR.PATCH> (xn54-v0.2.0)
#            and the tree is clean:          0.2.0          image 0.2.0+V
#   dev      anything else:                  0.2.0-dev.N+SHA  image 0.2.0+(N*128+V)
#            N = commits in HEAD's history, SHA = its short hash; a tree with
#            changes adds .dYYMMDDHHMM (when it was configured), so two
#            builds never share a version string.
#   no git   (a source tarball):             0.2.0-dev+nogit  image 0.2.0+V
#
# V, the build variant (cmake/mg_image_id.cmake), tells the release,
# automation, bench and token builds of a commit apart in the MCUboot image id
# (it isn't a content hash: compare the image hash too): 1 the SDK token
# compiled in (any extra conf file setting CONFIG_MG_SDK_TOKEN, or a
# -DCONFIG_MG_SDK_TOKEN; only whether one is set is read), 2 a bench build (an
# overlay-bench* conf file, or one setting a MG_VARIANT_BENCH_SYMBOLS option),
# 4 a tree with changes, 8 the automation tools on (Kconfig.mg's default, off,
# unless a conf file or -D sets CONFIG_MG_AUTOMATION); -DMG_BUILD_VARIANT=<0-3,
# + 8 for automation> sets the others instead. The firmware string marks them:
# .tok, .bench or .auto (0.2.0-dev.N+SHA.tok.bench, or 0.2.0+tok for a
# release; a release build has neither .bench nor .auto). After Kconfig, CMakeLists.txt checks the guess
# against the effective configuration and fails the build on a mismatch
# (mg_image_variant_check()).
#
# MG_FW_VERSION goes to the Device Information Firmware Revision, Link
# device_info and the logs (mg_app_version()); MG_IMAGE_VERSION is the
# MCUboot image version imgtool signs (what SMP's image list reports).
# Both reach Kconfig through the environment (Kconfig: "$(MG_FW_VERSION)").
# -DMG_BUILD_NUMBER=<n> overrides N (CI).

set(MG_TAG_PREFIX "xn54-v" CACHE STRING "Release tag prefix (<code>-vX.Y.Z)")
include(${CMAKE_CURRENT_LIST_DIR}/mg_image_id.cmake)

# The token, bench and automation bits, from the build's own inputs.
set(_mg_variant 0)
mg_automation_default(_mg_auto)
# Under sysbuild a -D for the image (-Dzephyr_MG_BUILD_VARIANT=) lands in its
# sysbuild cache, loaded only later.
if(NOT DEFINED MG_BUILD_VARIANT AND DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}")
  file(STRINGS "${SYSBUILD_CACHE}" _mg_bv REGEX "^([A-Za-z0-9]+_)?MG_BUILD_VARIANT:")
  if(_mg_bv)
    string(REGEX REPLACE "^.*=" "" MG_BUILD_VARIANT "${_mg_bv}")
  endif()
endif()
if(DEFINED MG_BUILD_VARIANT)
  math(EXPR _mg_variant "${MG_BUILD_VARIANT} & ${MG_VARIANT_CONFIG_MASK}")
  set(_mg_auto "")
else()
  string(JOIN "|" _mg_bsyms ${MG_VARIANT_BENCH_SYMBOLS})
  set(_mg_confs ${EXTRA_CONF_FILE} ${OVERLAY_CONFIG} ${CONF_FILE} $ENV{EXTRA_CONF_FILE})
  # Under sysbuild the image's options arrive in its sysbuild cache, which
  # Zephyr loads only later (find_package): read EXTRA_CONF_FILE from it.
  if(DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}")
    file(STRINGS "${SYSBUILD_CACHE}" _mg_sb REGEX "^(EXTRA_CONF_FILE|OVERLAY_CONFIG|CONF_FILE):")
    foreach(_l ${_mg_sb})
      string(REGEX REPLACE "^[A-Z_]+:[A-Z]+=" "" _l "${_l}")
      list(APPEND _mg_confs ${_l})
    endforeach()
    # A -D option for the image arrives there too, as <image>_CONFIG_...
    file(STRINGS "${SYSBUILD_CACHE}" _mg_sbd
         REGEX "^([A-Za-z0-9]+_)*(CONFIG_MG_SDK_TOKEN|CONFIG_MG_AUTOMATION|${_mg_bsyms}):")
    foreach(_l ${_mg_sbd})
      if(_l MATCHES "CONFIG_MG_SDK_TOKEN:[A-Z]+=.+")
        math(EXPR _mg_variant "${_mg_variant} | ${MG_VARIANT_TOKEN}")
      elseif(_l MATCHES "(${_mg_bsyms}):[A-Z]+=(y|1|ON)$")
        math(EXPR _mg_variant "${_mg_variant} | ${MG_VARIANT_BENCH}")
      elseif(_l MATCHES "(^|_)CONFIG_MG_AUTOMATION:[A-Z]+=(y|1|ON)$")
        set(_mg_auto y)
      elseif(_l MATCHES "(^|_)CONFIG_MG_AUTOMATION:[A-Z]+=(n|0|OFF)$")
        set(_mg_auto n)
      endif()
    endforeach()
    unset(_l) # only whether a token is set
    unset(_mg_sbd)
  endif()
  string(REPLACE " " ";" _mg_confs "${_mg_confs}")
  foreach(_c ${_mg_confs})
    if(NOT IS_ABSOLUTE "${_c}")
      set(_c ${CMAKE_CURRENT_SOURCE_DIR}/${_c})
    endif()
    if(NOT EXISTS "${_c}")
      continue()
    endif()
    get_filename_component(_cn "${_c}" NAME)
    # Only whether a token is set: the line itself is dropped at once.
    file(STRINGS "${_c}" _tok REGEX "^CONFIG_MG_SDK_TOKEN=\".+\"")
    if(_tok)
      math(EXPR _mg_variant "${_mg_variant} | ${MG_VARIANT_TOKEN}")
    endif()
    unset(_tok)
    set(_bench "")
    foreach(_sym ${MG_VARIANT_BENCH_SYMBOLS})
      file(STRINGS "${_c}" _b REGEX "^${_sym}=y")
      list(APPEND _bench ${_b})
    endforeach()
    if(_cn MATCHES "^overlay-bench" OR _bench)
      math(EXPR _mg_variant "${_mg_variant} | ${MG_VARIANT_BENCH}")
    endif()
    foreach(_sym ${MG_TEST_SHAPES})
      file(STRINGS "${_c}" _sh REGEX "^${_sym}=y")
      if(_sh)
        list(APPEND _mg_shapes ${_sym})
      endif()
    endforeach()
    file(STRINGS "${_c}" _au REGEX "^(CONFIG_MG_AUTOMATION=[yn]|# CONFIG_MG_AUTOMATION is not set)")
    foreach(_a ${_au})
      if(_a STREQUAL "CONFIG_MG_AUTOMATION=y")
        set(_mg_auto y)
      else()
        set(_mg_auto n)
      endif()
    endforeach()
  endforeach()
  get_cmake_property(_mg_cache CACHE_VARIABLES)
  foreach(_v ${_mg_cache})
    if(_v MATCHES "^(${_mg_bsyms})$" AND "${${_v}}" MATCHES "^(y|1|ON)$")
      math(EXPR _mg_variant "${_mg_variant} | ${MG_VARIANT_BENCH}")
    endif()
    list(FIND MG_TEST_SHAPES "${_v}" _mg_si)
    if(_mg_si GREATER -1 AND "${${_v}}" MATCHES "^(y|1|ON)$")
      list(APPEND _mg_shapes ${_v})
    endif()
    if(_v STREQUAL "CONFIG_MG_SDK_TOKEN" AND NOT "${${_v}}" STREQUAL "")
      math(EXPR _mg_variant "${_mg_variant} | ${MG_VARIANT_TOKEN}") # a -D token
    endif()
    if(_v STREQUAL "CONFIG_MG_AUTOMATION")
      if("${${_v}}" MATCHES "^(y|1|ON)$")
        set(_mg_auto y)
      else()
        set(_mg_auto n)
      endif()
    endif()
  endforeach()
endif()
if(_mg_auto STREQUAL "y")
  math(EXPR _mg_variant "${_mg_variant} | ${MG_VARIANT_AUTO}")
endif()
if(DEFINED _mg_shapes)
  list(REMOVE_DUPLICATES _mg_shapes)
  mg_test_shape_bits(_mg_sb "${_mg_shapes}")
  math(EXPR _mg_variant "${_mg_variant} | ${_mg_sb}")
endif()
mg_fw_variant_suffix(_mg_vsuffix ${_mg_variant})

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../VERSION _mg_ver REGEX "^(VERSION_MAJOR|VERSION_MINOR|PATCHLEVEL) *=")
foreach(_l ${_mg_ver})
  string(REGEX MATCH "^([A-Z_]+) *= *([0-9]+)" _m "${_l}")
  set(_mg_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
endforeach()
if(NOT DEFINED _mg_VERSION_MAJOR OR NOT DEFINED _mg_VERSION_MINOR OR NOT DEFINED _mg_PATCHLEVEL)
  message(FATAL_ERROR "zephyr/VERSION needs VERSION_MAJOR, VERSION_MINOR and PATCHLEVEL")
endif()
set(_mg_base "${_mg_VERSION_MAJOR}.${_mg_VERSION_MINOR}.${_mg_PATCHLEVEL}")

set(_mg_src ${CMAKE_CURRENT_LIST_DIR}/../..)
find_package(Git QUIET)
set(_mg_sha "")
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${_mg_src} rev-parse --short=8 HEAD
                  OUTPUT_VARIABLE _mg_sha OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _mg_rc)
  if(NOT _mg_rc EQUAL 0)
    set(_mg_sha "")
  endif()
endif()

if(_mg_sha STREQUAL "")
  message(WARNING "no git: the image id is ${_mg_base}+V, the same for every build without git "
                  "(and as a tagged release's); updaters must compare the image hash too")
  set(MG_FW_VERSION "${_mg_base}-dev+nogit${_mg_vsuffix}")
  set(MG_IMAGE_VERSION "${_mg_base}+${_mg_variant}")
else()
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${_mg_src} rev-list --count HEAD
                  OUTPUT_VARIABLE _mg_count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${_mg_src} tag --points-at HEAD
                  OUTPUT_VARIABLE _mg_tags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${_mg_src} status --porcelain --
                          zephyr protocols xplat esp32/main portable
                  OUTPUT_VARIABLE _mg_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(DEFINED MG_BUILD_NUMBER)
    set(_mg_count ${MG_BUILD_NUMBER})
  endif()
  string(REPLACE "\n" ";" _mg_tags "${_mg_tags}")
  list(FIND _mg_tags "${MG_TAG_PREFIX}${_mg_base}" _mg_tagged)
  if(NOT _mg_dirty STREQUAL "")
    math(EXPR _mg_variant "${_mg_variant} | ${MG_VARIANT_DIRTY}")
  endif()
  if(_mg_tagged GREATER -1 AND _mg_dirty STREQUAL "")
    set(MG_FW_VERSION "${_mg_base}")
    if(NOT _mg_vsuffix STREQUAL "")
      string(SUBSTRING "${_mg_vsuffix}" 1 -1 _mg_vs)
      string(APPEND MG_FW_VERSION "+${_mg_vs}")
    endif()
    set(MG_IMAGE_VERSION "${_mg_base}+${_mg_variant}")
  else()
    set(MG_FW_VERSION "${_mg_base}-dev.${_mg_count}+${_mg_sha}${_mg_vsuffix}")
    if(NOT _mg_dirty STREQUAL "")
      string(TIMESTAMP _mg_now "%y%m%d%H%M")
      string(APPEND MG_FW_VERSION ".d${_mg_now}")
    endif()
    mg_image_build(_mg_build ${_mg_count} ${_mg_variant})
    set(MG_IMAGE_VERSION "${_mg_base}+${_mg_build}")
  endif()
  # A new commit or tag reconfigures, so the version follows HEAD.
  foreach(_p HEAD packed-refs)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${_mg_src} rev-parse --git-path ${_p}
                    OUTPUT_VARIABLE _mg_gp OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT IS_ABSOLUTE "${_mg_gp}")
      set(_mg_gp ${_mg_src}/${_mg_gp})
    endif()
    if(EXISTS ${_mg_gp})
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_mg_gp})
    endif()
  endforeach()
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${_mg_src} symbolic-ref -q HEAD
                  OUTPUT_VARIABLE _mg_ref OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _mg_ref STREQUAL "")
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${_mg_src} rev-parse --git-path ${_mg_ref}
                    OUTPUT_VARIABLE _mg_gp OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT IS_ABSOLUTE "${_mg_gp}")
      set(_mg_gp ${_mg_src}/${_mg_gp})
    endif()
    if(EXISTS ${_mg_gp})
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_mg_gp})
    endif()
  endif()
endif()

# A build signed with a public development key (sysbuild-dev.conf; sysbuild.cmake
# hands it over) says so in its firmware string, never in the image id.
set(MG_DEVKEY n)
if(DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}")
  file(STRINGS "${SYSBUILD_CACHE}" _mg_dk REGEX "^([A-Za-z0-9]+_)?MG_DEVKEY:[A-Z]+=y$")
  if(_mg_dk)
    set(MG_DEVKEY y)
  endif()
elseif("${MG_DEVKEY_BUILD}" STREQUAL "y")
  set(MG_DEVKEY y)
endif()
if(MG_DEVKEY STREQUAL "y")
  string(APPEND MG_FW_VERSION ".devkey")
endif()

set(MG_IMAGE_VARIANT ${_mg_variant})
set(ENV{MG_FW_VERSION} "${MG_FW_VERSION}")
set(ENV{MG_IMAGE_VERSION} "${MG_IMAGE_VERSION}")
message(STATUS "Muse Gadget firmware ${MG_FW_VERSION} (image ${MG_IMAGE_VERSION})")
