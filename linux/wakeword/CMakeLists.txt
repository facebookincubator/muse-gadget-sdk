# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0.
cmake_minimum_required(VERSION 3.16)
project(hey_muse_signal LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# The signal sources live in the ESP32 tree in this repo; there are no
# copies under linux/wakeword. Compile them directly from there.
set(ESP_WAKEWORD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../esp32/components/muse_wakeword)
add_library(ww_signal SHARED ww_signal.c
  ${ESP_WAKEWORD_DIR}/ww_frontend.c ${ESP_WAKEWORD_DIR}/ww_detector.c)
set_target_properties(ww_signal PROPERTIES
  OUTPUT_NAME muse_wakeword_signal
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(ww_signal PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
  PRIVATE ${ESP_WAKEWORD_DIR} ${ESP_WAKEWORD_DIR}/include)
target_link_libraries(ww_signal PRIVATE m)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(ww_signal PRIVATE -Wl,-z,defs)
endif()
install(TARGETS ww_signal LIBRARY DESTINATION lib)
install(FILES ww_signal.h DESTINATION include)

include(CTest)
if(BUILD_TESTING)
  add_executable(ww_signal_test tests/signal_test.c
    ${ESP_WAKEWORD_DIR}/ww_frontend.c ${ESP_WAKEWORD_DIR}/ww_detector.c)
  target_include_directories(ww_signal_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${ESP_WAKEWORD_DIR} ${ESP_WAKEWORD_DIR}/include)
  target_link_libraries(ww_signal_test PRIVATE ww_signal m)
  add_test(NAME ww_signal_test COMMAND ww_signal_test)
endif()
