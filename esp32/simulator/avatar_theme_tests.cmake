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

# Optional appearance integration is tested when the independent theme is present.
if(EXISTS "${MUSE_COMPONENT_DIR}/muse_theme.c")
    add_executable(muse_avatar_theme_ui_test tests/avatar_theme_ui_harness.c ${caption_sources}
        "${MUSE_COMPONENT_DIR}/muse_avatar_render.c" "${MUSE_COMPONENT_DIR}/muse_avatar_background.c" "${MUSE_COMPONENT_DIR}/muse_avatar_theme.c"
        "${MUSE_COMPONENT_DIR}/muse_theme.c" "${MUSE_COMPONENT_DIR}/muse_theme_policy.c" "${MUSE_COMPONENT_DIR}/muse_theme_lvgl.c")
    target_include_directories(muse_avatar_theme_ui_test PRIVATE ${caption_includes})
    target_link_libraries(muse_avatar_theme_ui_test PRIVATE ${caption_libraries})
    target_compile_options(muse_avatar_theme_ui_test PRIVATE ${caption_compile_options})
    target_compile_definitions(muse_avatar_theme_ui_test PRIVATE CONFIG_MUSE_AVATAR_SYNC=1 CONFIG_MUSE_AVATAR_ANIMATIONS=1 CONFIG_MUSE_UI_THEME=1)
    if(MUSE_SIM_SANITIZERS)
        target_link_options(muse_avatar_theme_ui_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_test(NAME muse_avatar_theme_ui COMMAND muse_avatar_theme_ui_test)
    set_tests_properties(muse_avatar_theme_ui PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;ASAN_OPTIONS=detect_leaks=0:halt_on_error=1" TIMEOUT 30)
    if(TARGET muse_settings_theme_test)
        get_target_property(avatar_theme_settings_sources muse_settings_theme_test SOURCES)
        add_executable(muse_settings_avatar_theme_test ${avatar_theme_settings_sources})
        foreach(prop INCLUDE_DIRECTORIES LINK_LIBRARIES COMPILE_OPTIONS LINK_OPTIONS COMPILE_DEFINITIONS)
            get_target_property(value muse_settings_theme_test ${prop})
            if(value)
                set_property(TARGET muse_settings_avatar_theme_test PROPERTY ${prop} "${value}")
            endif()
        endforeach()
        target_compile_definitions(muse_settings_avatar_theme_test PRIVATE CONFIG_MUSE_AVATAR_SYNC=1)
        add_test(NAME muse_settings_avatar_theme_navigation_466 COMMAND muse_settings_avatar_theme_test 466 466 1 1.75 theme-navigation)
        add_test(NAME muse_settings_avatar_theme_navigation_240 COMMAND muse_settings_avatar_theme_test 240 240 1 1.28 theme-navigation)
        add_test(NAME muse_settings_avatar_theme_navigation_flat COMMAND muse_settings_avatar_theme_test 320 240 0 2.0 theme-navigation)
        set_tests_properties(muse_settings_avatar_theme_navigation_466 muse_settings_avatar_theme_navigation_240 muse_settings_avatar_theme_navigation_flat
            PROPERTIES ENVIRONMENT "${MUSE_SIM_TEST_ENVIRONMENT}" TIMEOUT 30)
    endif()
endif()
