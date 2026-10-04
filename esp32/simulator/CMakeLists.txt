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
    target_compile_definitions(lvgl PRIVATE _POSIX_C_SOURCE=200809L)
endif()

set(MUSE_COMPONENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/muse")

add_executable(muse_simulator
    src/main.c
    src/sim_board.c
    src/sim_platform.c
    src/sim_services.c
    "${MUSE_COMPONENT_DIR}/muse_ui.c"
    "${MUSE_COMPONENT_DIR}/muse_state.c"
    "${MUSE_COMPONENT_DIR}/muse_text.c"
    "${MUSE_COMPONENT_DIR}/../../avatar/muse_pixel.c"
)

target_include_directories(muse_simulator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${MUSE_COMPONENT_DIR}"
)

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
option(MUSE_BUILD_DESKTOP_APP "Build the native LVGL conversation app" ON)
if(MUSE_BUILD_DESKTOP_APP)
    add_executable(muse_linux_app
        ../../linux/lvgl/main.c
        ../../linux/lvgl/chat_client.c
        ../../linux/lvgl/media_client.c
        src/sim_platform.c
        "${MUSE_COMPONENT_DIR}/muse_state.c"
        "${MUSE_COMPONENT_DIR}/muse_text.c"
        "${MUSE_COMPONENT_DIR}/../../avatar/muse_pixel.c"
    )
    target_include_directories(muse_linux_app PRIVATE compat include "${MUSE_COMPONENT_DIR}")
    target_link_libraries(muse_linux_app PRIVATE ${MUSE_SIM_LVGL_TARGET} ${MUSE_SIM_SDL_TARGET} Threads::Threads m)
    target_compile_options(muse_linux_app PRIVATE -Wall -Wextra -Wpedantic)
    if(APPLE)
        target_link_libraries(muse_linux_app PRIVATE ${MUSE_SIM_IOKIT_FRAMEWORK})
    endif()
    if(MUSE_SIM_SANITIZERS)
        target_compile_options(muse_linux_app PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(muse_linux_app PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    if(BUILD_TESTING)
        add_test(NAME muse_linux_app_smoke COMMAND muse_linux_app --headless --run-ms 800
                 --screenshot "${CMAKE_CURRENT_BINARY_DIR}/muse-linux-app.ppm")
    endif()
endif()
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_simulator.py")
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
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
    set_tests_properties(muse_simulator_headless PROPERTIES
        ENVIRONMENT "${MUSE_SIM_TEST_ENVIRONMENT}"
        TIMEOUT 60
    )
endif()

message(STATUS "Muse simulator LVGL target: ${MUSE_SIM_LVGL_TARGET}")
message(STATUS "Muse simulator SDL2 target: ${MUSE_SIM_SDL_TARGET}")
