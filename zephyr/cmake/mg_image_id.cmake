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

# MCUboot image ids that tell build kinds apart (an updater that tells images
# apart by the id would otherwise take one for another, and skip marking it
# for test). The id separates the release, automation, bench and token
# variants and the commit count; it is not a content hash: two dirty builds of one commit, or
# builds that differ in another option (CONFIG_MG_LC3=n, say), can share one,
# and every build without git shares base+V. An updater compares the image
# hash too (MCUboot's SHA-256 TLV, what img_mgmt's image list reports). The
# build number is
#
#   bits  0-3   the build variant: 1 the SDK token compiled in, 2 a bench
#               build, 4 a tree with uncommitted changes, 8 the automation
#               tools on (CONFIG_MG_AUTOMATION; 0: a clean release build)
#   bits  4-6   the test shape: the one test-only switch that shapes the
#               image (MG_TEST_SHAPES, 1-7; 0 none), so a bench build and a
#               never-healthy bench build of one commit never share an id;
#               two shapes at once fail the build
#   bits  7-23  N, the commit count (below 2^17)
#   bits 24-31  0 from the build: reserved for personalization (an updater
#               that writes the owner's token into a published image)
#
# so it rises with N within a variant (any downgrade compare still orders a
# variant's builds), and across variants a newer commit is still the bigger
# number. tests/host/image_id_check.cmake checks both (host run.py).

set(MG_VARIANT_TOKEN 1)
set(MG_VARIANT_BENCH 2)
set(MG_VARIANT_DIRTY 4)
set(MG_VARIANT_AUTO 8)
# The bits a build's configuration decides (the dirty bit is the tree's),
# the test shape included.
set(MG_VARIANT_CONFIG_MASK 123)
# The test shapes, by index (1 the first): each a bench-only switch that
# changes what the image does; the firmware string carries its suffix.
set(MG_TEST_SHAPES CONFIG_MG_BENCH_BOARD_NOT_READY CONFIG_MG_BENCH_EARLY_HANG
    CONFIG_MG_BENCH_NO_CONFIRM)
set(MG_TEST_SHAPE_SUFFIXES .notready .earlyhang .noconfirm)
set(MG_IMAGE_ID_KCONFIG ${CMAKE_CURRENT_LIST_DIR}/../Kconfig.mg)

# variant: bits 0-3 as above, bits 4-6 the test shape (0-127 in all).
function(mg_image_build out n variant)
  if(NOT n MATCHES "^[0-9]+$" OR n GREATER_EQUAL 131072)
    message(FATAL_ERROR "the commit count ${n} doesn't fit the image build number (below 2^17)")
  endif()
  if(NOT variant MATCHES "^[0-9]+$" OR variant GREATER 127)
    message(FATAL_ERROR "build variant ${variant} isn't 0-127")
  endif()
  math(EXPR b "${n} * 128 + ${variant}")
  set(${out} ${b} PARENT_SCOPE)
endfunction()

# The test shape's variant bits for a list of the shape symbols that are on;
# two at once is an error (their images would share an id).
function(mg_test_shape_bits out on_syms)
  set(bits 0)
  set(found "")
  set(i 0)
  foreach(sym ${MG_TEST_SHAPES})
    math(EXPR i "${i} + 1")
    list(FIND on_syms ${sym} at)
    if(at GREATER -1)
      list(APPEND found ${sym})
      math(EXPR bits "${i} * 16")
    endif()
  endforeach()
  list(LENGTH found nf)
  if(nf GREATER 1)
    message(FATAL_ERROR "two test-only image shapes at once (${found}): their images would share "
                        "an id; build one at a time")
  endif()
  set(${out} ${bits} PARENT_SCOPE)
endfunction()

# CONFIG_MG_AUTOMATION's default in this tree's Kconfig.mg (n here: release
# builds are the default), the guess before any conf file says.
function(mg_automation_default out)
  set(d n)
  file(READ ${MG_IMAGE_ID_KCONFIG} _k)
  if(_k MATCHES "\nconfig MG_AUTOMATION\n([^\n]*\n)?([^\n]*\n)?[ \t]*default (y|n)")
    set(d ${CMAKE_MATCH_3})
  endif()
  set(${out} ${d} PARENT_SCOPE)
endfunction()

# What the effective configuration says the variant is, checked after Kconfig
# against the guess mg_version.cmake made before it (the guess sets the image
# version, which Kconfig needs; the check makes a wrong guess fail the build
# instead of giving a lab build the release's id): the token bit for a
# non-empty CONFIG_MG_SDK_TOKEN, the automation bit for CONFIG_MG_AUTOMATION,
# the bench bit for any of MG_VARIANT_BENCH_SYMBOLS: the bench marker
# overlay-bench.conf sets and the bench-only test options (the test shapes).
set(MG_VARIANT_BENCH_SYMBOLS CONFIG_MG_BENCH_MODE CONFIG_MG_BENCH_BUILD ${MG_TEST_SHAPES})

function(mg_image_variant_check guessed)
  set(want 0)
  if("${CONFIG_MG_AUTOMATION}" STREQUAL "y")
    math(EXPR want "${want} | ${MG_VARIANT_AUTO}")
  endif()
  if(DEFINED CONFIG_MG_SDK_TOKEN AND NOT "${CONFIG_MG_SDK_TOKEN}" STREQUAL "")
    math(EXPR want "${want} | ${MG_VARIANT_TOKEN}")
  endif()
  foreach(sym ${MG_VARIANT_BENCH_SYMBOLS})
    if("${${sym}}" STREQUAL "y")
      math(EXPR want "${want} | ${MG_VARIANT_BENCH}")
    endif()
  endforeach()
  set(shapes_on "")
  foreach(sym ${MG_TEST_SHAPES})
    if("${${sym}}" STREQUAL "y")
      list(APPEND shapes_on ${sym})
    endif()
  endforeach()
  mg_test_shape_bits(sb "${shapes_on}")
  math(EXPR want "${want} | ${sb}")
  math(EXPR got "${guessed} & ${MG_VARIANT_CONFIG_MASK}")
  if(NOT got EQUAL want)
    message(FATAL_ERROR
      "the image id's build variant (${got}: 1 token, 2 bench, 8 automation, 16-112 the test "
      "shape) doesn't match the configuration (${want}); the token, bench, automation or "
      "test-shape options came from somewhere "
      "mg_version.cmake doesn't read. Pass -DMG_BUILD_VARIANT=${want} so this build doesn't "
      "share another variant's image id.")
  endif()
endfunction()

# The firmware string's variant marks (for people and tools reading it):
# .tok, then .bench (a bench build has the automation tools too) or .auto
# (automation without the bench marker); a release has neither; a dirty tree
# keeps its .dYYMMDDHHMM.
function(mg_fw_variant_suffix out variant)
  set(s "")
  math(EXPR t "${variant} & ${MG_VARIANT_TOKEN}")
  math(EXPR be "${variant} & ${MG_VARIANT_BENCH}")
  math(EXPR au "${variant} & ${MG_VARIANT_AUTO}")
  if(t)
    string(APPEND s ".tok")
  endif()
  if(be)
    string(APPEND s ".bench")
  elseif(au)
    string(APPEND s ".auto")
  endif()
  math(EXPR sh "(${variant} >> 4) & 7")
  if(sh GREATER 0)
    math(EXPR si "${sh} - 1")
    list(GET MG_TEST_SHAPE_SUFFIXES ${si} ss)
    string(APPEND s "${ss}")
  endif()
  set(${out} "${s}" PARENT_SCOPE)
endfunction()
