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

cmake_minimum_required(VERSION 3.24)

project(muse_ui_simulator VERSION 0.1.0 LANGUAGES C)

if(WIN32 OR NOT UNIX)
    message(FATAL_ERROR "The UI simulator currently supports Linux and macOS")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "The UI simulator requires GCC or Clang")
endif()

include(FetchContent)
find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MUSE_SIM_FETCH_DEPS
       "Fetch pinned LVGL and SDL2 sources when suitable system packages are unavailable"
       ON)
option(MUSE_SIM_SANITIZERS "Build the simulator with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MUSE_SIM_WARNINGS_AS_ERRORS "Treat simulator warnings as errors" OFF)
option(MUSE_SIM_EMOJI "Show emoji in captions, as CONFIG_MUSE_EMOJI does" OFF)

set(MUSE_SIM_LVGL_VERSION "9.5.0")
set(MUSE_SIM_SDL_MIN_VERSION "2.0.18")
set(MUSE_SIM_SDL_FETCH_VERSION "2.32.10")

find_package(PkgConfig QUIET)

# Prefer a system SDL2 so desktop packaging can control its display backends.
# SDL2's CMake package is not installed on every distribution, so pkg-config is
# also accepted before falling back to the pinned upstream archive.
find_package(SDL2 ${MUSE_SIM_SDL_MIN_VERSION} CONFIG QUIET)
if(TARGET SDL2::SDL2)
    set(MUSE_SIM_SDL_TARGET SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    set(MUSE_SIM_SDL_TARGET SDL2::SDL2-static)
elseif(PkgConfig_FOUND)
    pkg_check_modules(MUSE_SIM_SYSTEM_SDL QUIET IMPORTED_TARGET GLOBAL
                      "sdl2>=${MUSE_SIM_SDL_MIN_VERSION}")
    if(MUSE_SIM_SYSTEM_SDL_FOUND)
        set(MUSE_SIM_SDL_TARGET PkgConfig::MUSE_SIM_SYSTEM_SDL)
    endif()
endif()

if(NOT MUSE_SIM_SDL_TARGET)
    if(NOT MUSE_SIM_FETCH_DEPS)
        message(FATAL_ERROR
                "SDL2 >= ${MUSE_SIM_SDL_MIN_VERSION} was not found and "
                "MUSE_SIM_FETCH_DEPS is OFF")
    endif()

    # Build only the static SDL runtime. Tests and the SDL test helper library
    # add substantial compile time without being used by this executable.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_WERROR OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
    set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_POWER OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL2
        URL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-${MUSE_SIM_SDL_FETCH_VERSION}.tar.gz"
        URL_HASH "SHA256=03f9d7c191a837525c9cda6406af2f2e48be02b5e7eb03d949cc9f1e9ca41c8b"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(SDL2)
    set(MUSE_SIM_SDL_TARGET SDL2::SDL2-static)
endif()

# The UI depends on LVGL 9.5 private APIs and on the fonts, SDL driver, and
# renderer settings in this directory's lv_conf.h. Use the pinned source by
# default so an exact-version system package with a different configuration
# cannot be selected accidentally. Turning dependency fetching off explicitly
# opts into a compatible system LVGL build.
if(NOT MUSE_SIM_FETCH_DEPS)
    find_package(lvgl ${MUSE_SIM_LVGL_VERSION} EXACT CONFIG QUIET)
    if(TARGET lvgl::lvgl)
        set(MUSE_SIM_LVGL_TARGET lvgl::lvgl)
    elseif(TARGET lvgl)
        set(MUSE_SIM_LVGL_TARGET lvgl)
    elseif(PkgConfig_FOUND)
        pkg_check_modules(MUSE_SIM_SYSTEM_LVGL QUIET IMPORTED_TARGET GLOBAL
                          "lvgl=${MUSE_SIM_LVGL_VERSION}")
        if(MUSE_SIM_SYSTEM_LVGL_FOUND)
            foreach(_include_dir IN LISTS MUSE_SIM_SYSTEM_LVGL_INCLUDE_DIRS)
                if(EXISTS "${_include_dir}/src/draw/lv_image_decoder_private.h" AND
                   EXISTS "${_include_dir}/src/misc/lv_area_private.h")
                    set(MUSE_SIM_LVGL_TARGET PkgConfig::MUSE_SIM_SYSTEM_LVGL)
                    break()
                endif()
            endforeach()
        endif()
    endif()
    if(NOT MUSE_SIM_LVGL_TARGET)
        message(FATAL_ERROR
                "LVGL ${MUSE_SIM_LVGL_VERSION} with the simulator configuration "
                "was not found and MUSE_SIM_FETCH_DEPS is OFF")
    endif()
else()
    set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE FILEPATH "" FORCE)
    set(LV_BUILD_SET_CONFIG_OPTS OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_USE_PRIVATE_API ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        lvgl
        URL "https://github.com/lvgl/lvgl/archive/refs/tags/v${MUSE_SIM_LVGL_VERSION}.tar.gz"
        URL_HASH "SHA256=34a955cdf3a2d005507b704e87357af669a114523b6d3f77b5344fdc68717bc6"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(lvgl)
    set(MUSE_SIM_LVGL_TARGET lvgl::lvgl)

    # Both LVGL's SDL display driver and its SDL2 OS abstraction include SDL
    # headers while LVGL itself is compiled.
    target_link_libraries(lvgl PRIVATE ${MUSE_SIM_SDL_TARGET})
    # usleep() was removed from POSIX.1-2008, so _POSIX_C_SOURCE=200809L
    # alone hides its declaration on glibc; _DEFAULT_SOURCE re-exposes it.
    # LVGL's pthread OS layer calls usleep(), and GCC 14+ turns the
    # resulting implicit declaration into an error.
    target_compile_definitions(lvgl PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()

set(MUSE_COMPONENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/muse")

# Match the firmware's custom avatar override, including additions/removals
# after configure. Keep exactly one renderer in the executable.
file(GLOB MUSE_SIM_CUSTOM_AVATAR CONFIGURE_DEPENDS
     "${MUSE_COMPONENT_DIR}/avatar/muse_pixel.c")
if(MUSE_SIM_CUSTOM_AVATAR)
    set(MUSE_SIM_AVATAR_SOURCE "${MUSE_COMPONENT_DIR}/avatar/muse_pixel.c")
else()
    set(MUSE_SIM_AVATAR_SOURCE "${MUSE_COMPONENT_DIR}/../../avatar/muse_pixel.c")
endif()

# Every board's type set (muse_type.c), and the vendored haptics core takes this
# repo's protocol header (haptics/VENDORED.md): for the simulator and every test
# target below that reuses its sources.
add_compile_definitions(MUSE_TYPE_ALL=1 "MGH_MGCOMMANDS_H=\"mgcommands.h\"")
# The firmware's sources use POSIX/BSD calls (strdup, strnlen, M_PI) that
# ESP-IDF's newlib declares by default; with -std=c11 glibc hides them unless
# asked (macOS shows them anyway).
add_compile_definitions(_DEFAULT_SOURCE)

add_executable(muse_simulator
    src/main.c
    src/sim_board.c
    src/sim_platform.c
    src/sim_services.c
    "${MUSE_COMPONENT_DIR}/muse_ui.c"
    "${MUSE_COMPONENT_DIR}/muse_options.c"
    "${MUSE_COMPONENT_DIR}/muse_options_ui.c"
    "${MUSE_COMPONENT_DIR}/muse_state.c"
    "${MUSE_COMPONENT_DIR}/muse_text.c"
    "${MUSE_COMPONENT_DIR}/muse_lang.c"
    "${MUSE_SIM_AVATAR_SOURCE}"
)

target_include_directories(muse_simulator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MUSE_COMPONENT_DIR}"
    "${MUSE_COMPONENT_DIR}/../muse_gadget_ble/include"
    "${MUSE_COMPONENT_DIR}/../muse_gadget_ble/haptics"
    "${MUSE_COMPONENT_DIR}/../muse_vstream/include"
    "${MUSE_COMPONENT_DIR}/../network_proxy/include"
    "${MUSE_COMPONENT_DIR}/../../../protocols"
)

# The v1.0 screens (home, Status, Settings, the passcode, haptics tests), with
# the simulator's stand-ins for the services under them.
target_sources(muse_simulator PRIVATE
    src/sim_stubs.c
    src/sim_haptics.c
    src/sim_passcode.c
    "${MUSE_COMPONENT_DIR}/../muse_gadget_ble/haptics/mg_haptics.c"
    "${MUSE_COMPONENT_DIR}/../muse_gadget_ble/haptics/mg_haptic_test.c"
    "${MUSE_COMPONENT_DIR}/muse_mic_ring.c"
    "${MUSE_COMPONENT_DIR}/muse_turn_dot.c"
    "${MUSE_COMPONENT_DIR}/muse_home.c"
    "${MUSE_COMPONENT_DIR}/muse_status_conn.c"
    "${MUSE_COMPONENT_DIR}/muse_icons.c"
    "${MUSE_COMPONENT_DIR}/muse_type.c"
    "${MUSE_COMPONENT_DIR}/muse_settings_ui.c"
    "${MUSE_COMPONENT_DIR}/muse_keypad.c"
    "${MUSE_COMPONENT_DIR}/muse_passcode_core.c"
    "${MUSE_COMPONENT_DIR}/muse_passcode.c"
    "${MUSE_COMPONENT_DIR}/muse_passcode_ui.c"
)

# Every board's type set (muse_type.c picks by the panel's size).
file(GLOB MUSE_TYPE_FONTS "${MUSE_COMPONENT_DIR}/fonts/type/*.c")
target_sources(muse_simulator PRIVATE ${MUSE_TYPE_FONTS})

target_link_libraries(muse_simulator PRIVATE
    ${MUSE_SIM_LVGL_TARGET}
    ${MUSE_SIM_SDL_TARGET}
    Threads::Threads
)

target_link_libraries(muse_simulator PRIVATE m)

if(APPLE)
    # SDL's Cocoa display and screensaver backends use IOKit even when its
    # optional power subsystem is disabled. The static target does not carry
    # that framework in this configuration.
    find_library(MUSE_SIM_IOKIT_FRAMEWORK IOKit REQUIRED)
    target_link_libraries(muse_simulator PRIVATE ${MUSE_SIM_IOKIT_FRAMEWORK})
    # Optional custom-avatar cell accessor: ELF weak references need no flag;
    # Mach-O executables must allow the unresolved weak import explicitly.
    target_link_options(muse_simulator PRIVATE "-Wl,-U,_muse_pixel_cell_rgb565")
endif()

if(MUSE_SIM_EMOJI)
    target_sources(muse_simulator PRIVATE "${MUSE_COMPONENT_DIR}/fonts/muse_font_emoji_16.c")
    target_compile_definitions(muse_simulator PRIVATE CONFIG_MUSE_EMOJI=1)
endif()

target_compile_options(muse_simulator PRIVATE -Wall -Wextra -Wpedantic)
if(MUSE_SIM_WARNINGS_AS_ERRORS)
    target_compile_options(muse_simulator PRIVATE -Werror)
endif()

if(MUSE_SIM_SANITIZERS)
    target_compile_options(muse_simulator PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(muse_simulator PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
endif()

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_simulator.py")
    # Reuse the simulator adapters, but include the production UI directly so
    # the regression can check the rendered label's font and geometry.
    get_target_property(caption_sources muse_simulator SOURCES)
    list(REMOVE_ITEM caption_sources src/main.c "${MUSE_COMPONENT_DIR}/muse_ui.c")
    add_executable(muse_caption_layout_test tests/caption_layout_harness.c ${caption_sources})
    get_target_property(caption_includes muse_simulator INCLUDE_DIRECTORIES)
    get_target_property(caption_libraries muse_simulator LINK_LIBRARIES)
    target_include_directories(muse_caption_layout_test PRIVATE ${caption_includes})
    target_link_libraries(muse_caption_layout_test PRIVATE ${caption_libraries})
    get_target_property(caption_compile_options muse_simulator COMPILE_OPTIONS)
    target_compile_options(muse_caption_layout_test PRIVATE ${caption_compile_options})
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_caption_layout_test PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_caption_layout COMMAND muse_caption_layout_test)
    add_executable(muse_theme_ui_test tests/theme_ui_harness.c ${caption_sources}
        "${MUSE_COMPONENT_DIR}/muse_theme.c" "${MUSE_COMPONENT_DIR}/muse_theme_policy.c" "${MUSE_COMPONENT_DIR}/muse_theme_lvgl.c")
    target_include_directories(muse_theme_ui_test PRIVATE ${caption_includes})
    target_link_libraries(muse_theme_ui_test PRIVATE ${caption_libraries})
    target_compile_options(muse_theme_ui_test PRIVATE ${caption_compile_options})
    target_compile_definitions(muse_theme_ui_test PRIVATE CONFIG_MUSE_UI_THEME=1)
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_theme_ui_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_theme_ui COMMAND muse_theme_ui_test)
    set_tests_properties(muse_theme_ui PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1" TIMEOUT 30)
    add_executable(muse_avatar_ui_test tests/avatar_ui_harness.c ${caption_sources}
        "${MUSE_COMPONENT_DIR}/muse_avatar_render.c")
    target_include_directories(muse_avatar_ui_test PRIVATE ${caption_includes})
    target_link_libraries(muse_avatar_ui_test PRIVATE ${caption_libraries})
    target_compile_options(muse_avatar_ui_test PRIVATE ${caption_compile_options})
    target_compile_definitions(muse_avatar_ui_test PRIVATE CONFIG_MUSE_AVATAR_SYNC=1)
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_avatar_ui_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_avatar_ui COMMAND muse_avatar_ui_test)
    set_tests_properties(muse_avatar_ui PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1" TIMEOUT 30)
    add_executable(muse_avatar_motion_ui_test tests/avatar_motion_ui_harness.c ${caption_sources}
        "${MUSE_COMPONENT_DIR}/muse_avatar_render.c")
    target_include_directories(muse_avatar_motion_ui_test PRIVATE ${caption_includes})
    target_link_libraries(muse_avatar_motion_ui_test PRIVATE ${caption_libraries})
    target_compile_options(muse_avatar_motion_ui_test PRIVATE ${caption_compile_options})
    target_compile_definitions(muse_avatar_motion_ui_test PRIVATE CONFIG_MUSE_AVATAR_SYNC=1 CONFIG_MUSE_AVATAR_ANIMATIONS=1)
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_avatar_motion_ui_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_avatar_motion_ui COMMAND muse_avatar_motion_ui_test)
    set_tests_properties(muse_avatar_motion_ui PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1" TIMEOUT 30)

    add_executable(muse_volume_swipe_test tests/volume_swipe_harness.c ${caption_sources})
    target_include_directories(muse_volume_swipe_test PRIVATE ${caption_includes})
    target_link_libraries(muse_volume_swipe_test PRIVATE ${caption_libraries})
    target_compile_options(muse_volume_swipe_test PRIVATE ${caption_compile_options})
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_volume_swipe_test PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    # Swipe volume on the compact faces; none on the v1.0 home.
    foreach(face compact long home)
        add_test(NAME muse_volume_swipe_${face} COMMAND muse_volume_swipe_test ${face})
        set_tests_properties(muse_volume_swipe_${face} PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" TIMEOUT 30)
    endforeach()
    # The Confirm disc through LVGL's real indev, the BLE gadget's UI (its update state faked): a tap on it confirms
    # the setup prompt and the update card, a tap elsewhere doesn't.
    add_executable(muse_confirm_disc_test tests/confirm_disc_harness.c ${caption_sources})
    target_include_directories(muse_confirm_disc_test BEFORE PRIVATE tests/confirm_fakes)
    target_include_directories(muse_confirm_disc_test PRIVATE ${caption_includes})
    target_link_libraries(muse_confirm_disc_test PRIVATE ${caption_libraries})
    target_compile_options(muse_confirm_disc_test PRIVATE ${caption_compile_options})
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_confirm_disc_test PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_confirm_disc COMMAND muse_confirm_disc_test)
    add_test(NAME muse_confirm_disc_round_466 COMMAND muse_confirm_disc_test 466 466 1)
    add_test(NAME muse_confirm_disc_round_412 COMMAND muse_confirm_disc_test 412 412 1)
    set_tests_properties(muse_confirm_disc muse_confirm_disc_round_466 muse_confirm_disc_round_412 PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" TIMEOUT 30)

    add_executable(muse_options_ui_test tests/options_ui_harness.c ${caption_sources})
    target_include_directories(muse_options_ui_test PRIVATE ${caption_includes})
    target_link_libraries(muse_options_ui_test PRIVATE ${caption_libraries})
    target_compile_options(muse_options_ui_test PRIVATE ${caption_compile_options})
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_options_ui_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_options_ui COMMAND muse_options_ui_test)
    add_test(NAME muse_options_round_466 COMMAND muse_options_ui_test 466 466 1)
    add_test(NAME muse_options_rectangle COMMAND muse_options_ui_test 320 480 0)
    set_tests_properties(muse_options_ui muse_options_round_466 muse_options_rectangle PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1" TIMEOUT 30)
    set_tests_properties(muse_caption_layout PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" TIMEOUT 30)
    add_executable(muse_latest_reply_test tests/latest_reply_harness.c ${caption_sources}
        "${MUSE_COMPONENT_DIR}/muse_review.c"
        "${MUSE_COMPONENT_DIR}/muse_chat_text.c")
    target_include_directories(muse_latest_reply_test PRIVATE ${caption_includes})
    target_link_libraries(muse_latest_reply_test PRIVATE ${caption_libraries})
    target_compile_options(muse_latest_reply_test PRIVATE ${caption_compile_options})
    target_compile_definitions(muse_latest_reply_test PRIVATE CONFIG_MUSE_LATEST_REPLY=1)
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_latest_reply_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_latest_reply COMMAND muse_latest_reply_test)
    set_tests_properties(muse_latest_reply PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" TIMEOUT 30)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_executable(muse_ring_clip_test tests/muse_ring_clip_harness.c)
    target_include_directories(muse_ring_clip_test PRIVATE "${MUSE_COMPONENT_DIR}")
    target_compile_definitions(muse_ring_clip_test PRIVATE CONFIG_MUSE_BOARD_WAVESHARE_C6_18=1)
    target_link_libraries(muse_ring_clip_test PRIVATE ${MUSE_SIM_LVGL_TARGET} ${MUSE_SIM_SDL_TARGET} Threads::Threads m)
    target_compile_options(muse_ring_clip_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME muse_ring_clip_pixels COMMAND muse_ring_clip_test)
    set_tests_properties(muse_ring_clip_pixels PROPERTIES TIMEOUT 60)
    if(APPLE)
        target_link_libraries(muse_ring_clip_test PRIVATE ${MUSE_SIM_IOKIT_FRAMEWORK})
    endif()
    if(MUSE_SIM_SANITIZERS)
        target_compile_options(muse_ring_clip_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(muse_ring_clip_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(
        NAME muse_simulator_headless
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_simulator.py"
                --binary "$<TARGET_FILE:muse_simulator>"
    )
    set(MUSE_SIM_TEST_ENVIRONMENT
        "SDL_VIDEODRIVER=dummy"
        "SDL_AUDIODRIVER=dummy"
    )
    if(MUSE_SIM_SANITIZERS)
        # Apple Clang's ASan runtime rejects leak detection on macOS.
        if(APPLE)
            set(MUSE_SIM_ASAN_OPTIONS "detect_leaks=0:halt_on_error=1")
        else()
            set(MUSE_SIM_ASAN_OPTIONS "detect_leaks=1:halt_on_error=1")
        endif()
        list(APPEND MUSE_SIM_TEST_ENVIRONMENT
            "ASAN_OPTIONS=${MUSE_SIM_ASAN_OPTIONS}"
            "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
        )
    endif()
    set_tests_properties(muse_simulator_headless muse_ring_clip_pixels PROPERTIES
        ENVIRONMENT "${MUSE_SIM_TEST_ENVIRONMENT}"
        TIMEOUT 60
    )
    # Debug/Sanitizer LVGL draws hundreds of full frames; leave CPU contention
    # headroom when firmware builds are running alongside the test.
    set_tests_properties(muse_ring_clip_pixels PROPERTIES TIMEOUT 180)

    # The settings harness (tests/settings_text_harness.c) includes
    # muse_settings_ui.c with fakes for the services under its pages and checks
    # the card list's text entry, rows, Sound and Display, the bench's page
    # opening and the Appearance and Avatar pages on round, flat and narrow
    # panels. The Muse Gadgets pages (Talk, Passcode, Watch face, Developer,
    # Tests) are only shown or hidden: opening one fails it.
    option(MUSE_SIM_SETTINGS_TEXT_HARNESS "Build the settings text/layout harness" ON)
    if(MUSE_SIM_SETTINGS_TEXT_HARNESS)
    set(muse_settings_includes
        "${MUSE_COMPONENT_DIR}"
        "${MUSE_COMPONENT_DIR}/../muse_gadget_ble/include"
        "${MUSE_COMPONENT_DIR}/../muse_gadget_ble/haptics"
        "${MUSE_COMPONENT_DIR}/../../../protocols"
    )
    # The card list's real type sets, icons, home geometry and haptics test core (Developer > Tests > Vibration).
    set(muse_settings_sources
        "${MUSE_COMPONENT_DIR}/muse_keypad.c"
        "${MUSE_COMPONENT_DIR}/muse_lang.c"
        "${MUSE_COMPONENT_DIR}/muse_text.c"
        "${MUSE_COMPONENT_DIR}/muse_type.c"
        ${MUSE_TYPE_FONTS}
        "${MUSE_COMPONENT_DIR}/muse_icons.c"
        "${MUSE_COMPONENT_DIR}/muse_home.c"
        "${MUSE_COMPONENT_DIR}/../muse_gadget_ble/haptics/mg_haptics.c"
        "${MUSE_COMPONENT_DIR}/../muse_gadget_ble/haptics/mg_haptic_test.c"
    )
    add_executable(muse_settings_text_test tests/settings_text_harness.c ${muse_settings_sources})
    target_include_directories(muse_settings_text_test PRIVATE
        tests/settings_fakes compat ../tests/link_fakes ${muse_settings_includes}
    )
    target_compile_definitions(muse_settings_text_test PRIVATE _POSIX_C_SOURCE=200809L)
    add_executable(muse_settings_theme_test
        tests/settings_text_harness.c ${muse_settings_sources}
        "${MUSE_COMPONENT_DIR}/muse_theme.c" "${MUSE_COMPONENT_DIR}/muse_theme_policy.c" "${MUSE_COMPONENT_DIR}/muse_theme_lvgl.c")
    target_include_directories(muse_settings_theme_test PRIVATE tests/settings_fakes compat ../tests/link_fakes ${muse_settings_includes})
    target_compile_definitions(muse_settings_theme_test PRIVATE _POSIX_C_SOURCE=200809L CONFIG_MUSE_UI_THEME=1)
    target_compile_options(muse_settings_theme_test PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
    target_link_libraries(muse_settings_theme_test PRIVATE ${MUSE_SIM_LVGL_TARGET} ${MUSE_SIM_SDL_TARGET} Threads::Threads m)
    if(APPLE)
        target_compile_definitions(muse_settings_theme_test PRIVATE _DARWIN_C_SOURCE)
        target_link_options(muse_settings_theme_test PRIVATE -Wl,-dead_strip)
        target_link_libraries(muse_settings_theme_test PRIVATE ${MUSE_SIM_IOKIT_FRAMEWORK})
    else()
        target_link_options(muse_settings_theme_test PRIVATE -Wl,--gc-sections)
    endif()
    if(MUSE_SIM_SANITIZERS)
        target_compile_options(muse_settings_theme_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(muse_settings_theme_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_settings_theme_466 COMMAND muse_settings_theme_test 466 466 1 1.75 theme)
    add_test(NAME muse_settings_theme_240 COMMAND muse_settings_theme_test 240 240 1 1.28 theme)
    add_test(NAME muse_settings_theme_navigation COMMAND muse_settings_theme_test 466 466 1 1.75 theme-navigation)
    set_tests_properties(muse_settings_theme_navigation PROPERTIES ENVIRONMENT "${MUSE_SIM_TEST_ENVIRONMENT}" TIMEOUT 30)
    set_tests_properties(muse_settings_theme_466 muse_settings_theme_240 PROPERTIES ENVIRONMENT "${MUSE_SIM_TEST_ENVIRONMENT}" TIMEOUT 30)

    add_test(NAME muse_settings_volume_sync COMMAND muse_settings_text_test 466 466 1 1.75 volume)
    target_compile_options(muse_settings_text_test PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
    target_link_libraries(muse_settings_text_test PRIVATE ${MUSE_SIM_LVGL_TARGET} ${MUSE_SIM_SDL_TARGET} Threads::Threads m)
    if(APPLE)
        target_compile_definitions(muse_settings_text_test PRIVATE _DARWIN_C_SOURCE)
        target_link_options(muse_settings_text_test PRIVATE -Wl,-dead_strip)
        target_link_libraries(muse_settings_text_test PRIVATE ${MUSE_SIM_IOKIT_FRAMEWORK})
    else()
        target_link_options(muse_settings_text_test PRIVATE -Wl,--gc-sections)
    endif()
    if(MUSE_SIM_WARNINGS_AS_ERRORS)
        target_compile_options(muse_settings_text_test PRIVATE -Werror)
    endif()
    if(MUSE_SIM_SANITIZERS)
        target_compile_options(muse_settings_text_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(muse_settings_text_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    get_target_property(avatar_settings_sources muse_settings_text_test SOURCES)
    add_executable(muse_settings_avatar_test ${avatar_settings_sources})
    foreach(prop INCLUDE_DIRECTORIES LINK_LIBRARIES COMPILE_OPTIONS LINK_OPTIONS COMPILE_DEFINITIONS)
        get_target_property(value muse_settings_text_test ${prop})
        if(value)
            set_property(TARGET muse_settings_avatar_test PROPERTY ${prop} "${value}")
        endif()
    endforeach()
    target_compile_definitions(muse_settings_avatar_test PRIVATE CONFIG_MUSE_AVATAR_SYNC=1)
    add_test(NAME muse_settings_avatar_466 COMMAND muse_settings_avatar_test 466 466 1 1.75 avatar)
    add_test(NAME muse_settings_avatar_240 COMMAND muse_settings_avatar_test 240 240 1 1.28 avatar)
    set_tests_properties(muse_settings_avatar_466 muse_settings_avatar_240 PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")
    foreach(scenario other secure saved back)
        add_test(NAME muse_settings_xiao_${scenario}
                 COMMAND muse_settings_text_test 172 320 0 1.47 ${scenario})
    endforeach()
    add_test(NAME muse_settings_round_466 COMMAND muse_settings_text_test 466 466 1 1.75 geometry)
    add_test(NAME muse_settings_round_412 COMMAND muse_settings_text_test 412 412 1 1.45 geometry)
    add_test(NAME muse_settings_round_240 COMMAND muse_settings_text_test 240 240 1 1.28 geometry)
    add_test(NAME muse_settings_flat_keyboard COMMAND muse_settings_text_test 320 240 0 2.0 geometry)
    add_test(NAME muse_settings_flat_keyboard_password COMMAND muse_settings_text_test 320 240 0 2.0 password)
    add_test(NAME muse_settings_portrait_keyboard COMMAND muse_settings_text_test 240 320 0 2.8 geometry)
    add_test(NAME muse_settings_portrait_keyboard_password COMMAND muse_settings_text_test 240 320 0 2.8 password)
    add_test(NAME muse_settings_tall_keyboard COMMAND muse_settings_text_test 320 480 0 3.5 geometry)
    add_test(NAME muse_settings_epaper_keyboard COMMAND muse_settings_text_test 480 800 0 3.97 geometry)
    add_test(NAME muse_settings_wide_800 COMMAND muse_settings_text_test 800 480 0 4.3 geometry)
    add_test(NAME muse_settings_long_keypad COMMAND muse_settings_text_test 172 640 0 3.49 geometry)
    add_test(NAME muse_settings_xiao_switch COMMAND muse_settings_text_test 172 320 0 1.47 switch)
    add_test(NAME muse_settings_round_switch COMMAND muse_settings_text_test 466 466 1 1.75 switch)
    add_test(NAME muse_settings_round_240_switch COMMAND muse_settings_text_test 240 240 1 1.28 switch)
    add_test(NAME muse_settings_round_240_rows COMMAND muse_settings_text_test 240 240 1 1.28 rows)
    # Wi-Fi's MAC address row as each panel draws it, a screenshot of each in the build tree: stacked and whole
    # where a label and its value don't fit side by side (240 px wide and narrower), one line elsewhere.
    foreach(panel "round_240;240;240;1;1.28" "xiao;172;320;0;1.47" "portrait;240;320;0;2.8" "round_466;466;466;1;1.75")
        list(GET panel 0 name)
        list(GET panel 1 w)
        list(GET panel 2 h)
        list(GET panel 3 round)
        list(GET panel 4 diag)
        add_test(NAME muse_settings_${name}_info
                 COMMAND muse_settings_text_test ${w} ${h} ${round} ${diag} info
                         "${CMAKE_CURRENT_BINARY_DIR}/settings-info-${name}.ppm")
    endforeach()
    # 240 px wide and narrower, every page: no label cut short with "...", none outside its card; and
    # Wi-Fi's "Other network..." and Talk's longest choice drawn whole, a screenshot of each.
    foreach(panel "round_240;240;240;1;1.28" "xiao;172;320;0;1.47" "portrait;240;320;0;2.8")
        list(GET panel 0 name)
        list(GET panel 1 w)
        list(GET panel 2 h)
        list(GET panel 3 round)
        list(GET panel 4 diag)
        add_test(NAME muse_settings_${name}_sweep COMMAND muse_settings_text_test ${w} ${h} ${round} ${diag} sweep)
        if(NOT name STREQUAL "portrait")
            foreach(shot other talk)
                add_test(NAME muse_settings_${name}_shot_${shot}
                         COMMAND muse_settings_text_test ${w} ${h} ${round} ${diag} shot-${shot}
                                 "${CMAKE_CURRENT_BINARY_DIR}/settings-${shot}-${name}.ppm")
            endforeach()
        endif()
    endforeach()
    add_test(NAME muse_settings_xiao_rows COMMAND muse_settings_text_test 172 320 0 1.47 rows)
    add_test(NAME muse_settings_round_240_screen COMMAND muse_settings_text_test 240 240 1 1.28 screen)
    add_test(NAME muse_settings_round_240_mic COMMAND muse_settings_text_test 240 240 1 1.28 mic)
    add_test(NAME muse_settings_round_240_bench COMMAND muse_settings_text_test 240 240 1 1.28 bench)
    endif()
    # The button menu's layout, in English and in Vietnamese (accented letters
    # in the 16 px caption font), on compact and full-size screens.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/menu_fakes/esp_app_desc.h"
         "#include \"${CMAKE_CURRENT_SOURCE_DIR}/../tests/link_fakes/esp_app_desc.h\"\n")
    foreach(lang en vi)
        add_executable(muse_menu_layout_${lang}
            tests/menu_layout_harness.c
            "${MUSE_COMPONENT_DIR}/muse_lang.c"
            "${MUSE_COMPONENT_DIR}/muse_text.c"
        )
        # The harness defines esp_app_get_description() itself, which the
        # simulator's own compat/esp_app_desc.h has inline: link_fakes'
        # declaration comes first.
        target_include_directories(muse_menu_layout_${lang} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/menu_fakes" tests/settings_fakes compat ../tests/link_fakes
            "${MUSE_COMPONENT_DIR}"
        )
        target_compile_definitions(muse_menu_layout_${lang} PRIVATE _POSIX_C_SOURCE=200809L)
        if(lang STREQUAL "vi")
            target_sources(muse_menu_layout_${lang} PRIVATE
                "${MUSE_COMPONENT_DIR}/fonts/muse_font_latin_16.c"
                "${MUSE_COMPONENT_DIR}/fonts/muse_font_vi_14.c"
                "${MUSE_COMPONENT_DIR}/fonts/muse_font_vi_20.c"
                "${MUSE_COMPONENT_DIR}/fonts/muse_font_vi_28.c"
            )
            target_compile_definitions(muse_menu_layout_${lang} PRIVATE
                CONFIG_MUSE_LATIN_FONT=1 CONFIG_MUSE_UI_LANG_VI=1)
        endif()
        target_compile_options(muse_menu_layout_${lang} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(muse_menu_layout_${lang} PRIVATE ${MUSE_SIM_LVGL_TARGET} ${MUSE_SIM_SDL_TARGET} Threads::Threads m)
        if(APPLE)
            target_compile_definitions(muse_menu_layout_${lang} PRIVATE _DARWIN_C_SOURCE)
            target_link_libraries(muse_menu_layout_${lang} PRIVATE ${MUSE_SIM_IOKIT_FRAMEWORK})
        endif()
        if(MUSE_SIM_WARNINGS_AS_ERRORS)
            target_compile_options(muse_menu_layout_${lang} PRIVATE -Werror)
        endif()
        if(MUSE_SIM_SANITIZERS)
            target_compile_options(muse_menu_layout_${lang} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(muse_menu_layout_${lang} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        endif()
        foreach(size 128x128 240x135 284x240 320x240 240x240)
            string(REPLACE "x" ";" wh ${size})
            add_test(NAME muse_settings_menu_${lang}_${size} COMMAND muse_menu_layout_${lang} ${wh})
        endforeach()
    endforeach()
    get_property(settings_tests DIRECTORY PROPERTY TESTS)
    list(FILTER settings_tests INCLUDE REGEX "^muse_settings_")
    set_tests_properties(${settings_tests} PROPERTIES ENVIRONMENT "${MUSE_SIM_TEST_ENVIRONMENT}" TIMEOUT 30)
    include("${CMAKE_CURRENT_SOURCE_DIR}/avatar_theme_tests.cmake")

endif()

message(STATUS "Muse simulator LVGL target: ${MUSE_SIM_LVGL_TARGET}")
message(STATUS "Muse simulator SDL2 target: ${MUSE_SIM_SDL_TARGET}")
