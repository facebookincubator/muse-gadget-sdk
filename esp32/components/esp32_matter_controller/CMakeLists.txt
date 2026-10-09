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


# Matter devices through Link commands (esp32_matter_controller.h). esp-matter
# comes from the component registry only when HOMEHUB_MATTER_CONTROLLER is on
# (idf_component.yml), and may be named as a requirement only when it is there.
# Requirements are collected in an early pass (CMAKE_BUILD_EARLY_EXPANSION)
# that sees only the known components; the build pass sees BUILD_COMPONENTS.
set(have_esp_matter FALSE)
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(known_components __COMPONENT_TARGETS)
    list(FILTER known_components INCLUDE REGEX "espressif__esp_matter$")
    if(known_components)
        set(have_esp_matter TRUE)
    endif()
else()
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if("espressif__esp_matter" IN_LIST build_components)
        set(have_esp_matter TRUE)
    endif()
endif()
set(srcs)
set(priv_reqs)
set(ldfragments)
if(have_esp_matter)
    set(priv_reqs espressif__esp_matter bt esp_event esp_netif esp_timer esp_wifi lwip nvs_flash spiffs)
    if(CONFIG_HOMEHUB_MATTER_CONTROLLER)
        list(APPEND srcs "esp32_matter_controller.cpp" "esp32_matter_controller_ipv6.c"
                         "esp32_matter_controller_proto.c")
        set(ldfragments "linker.lf")
    endif()
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
    PRIV_REQUIRES ${priv_reqs}
    LDFRAGMENTS ${ldfragments}
)

if(CONFIG_HOMEHUB_MATTER_CONTROLLER AND have_esp_matter)
    # esp-matter's own projects build CHIP with these (their project
    # CMakeLists); here they apply to esp-matter's sources only.
    idf_component_get_property(esp_matter_lib espressif__esp_matter COMPONENT_LIB)
    # -Os as esp-matter builds CHIP: at the firmware's -O2, GCC raises
    # stringop warnings inside CHIP, which builds with -Werror.
    target_compile_options(${esp_matter_lib} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error=unused-result;-Wno-overloaded-virtual>"
        "-Wno-format-nonliteral;-Wno-format-security" "-Os")
    idf_component_get_property(esp_matter_dir espressif__esp_matter COMPONENT_DIR)
    set_source_files_properties(
        "${esp_matter_dir}/components/esp_matter_controller/commands/esp_matter_controller_pairing_command.cpp"
        TARGET_DIRECTORY ${esp_matter_lib}
        PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/esp_matter_compat.h")

    # The PAA certificates the commissioner checks devices against: a prebuilt
    # SPIFFS image (paa/README.md) for the flash layout's paa_cert partition
    # (esp-matter's SPIFFS trust store), which idf.py flash writes with the app.
    if(CONFIG_SPIFFS_ATTESTATION_TRUST_STORE)
        set(paa_image "${CMAKE_CURRENT_LIST_DIR}/paa/paa_cert.bin")
        partition_table_get_partition_info(paa_size "--partition-name paa_cert" "size")
        file(SIZE "${paa_image}" paa_image_size)
        if(NOT paa_size)
            message(FATAL_ERROR "The flash layout has no paa_cert partition "
                                "(see devices/sdkconfig.matter-controller-*)")
        elseif(NOT paa_image_size EQUAL paa_size)
            message(FATAL_ERROR "paa/paa_cert.bin is ${paa_image_size} bytes, the paa_cert partition ${paa_size}: "
                                "rebuild it with tools/matter_paa_image.py")
        endif()
        esptool_py_flash_to_partition(flash paa_cert "${paa_image}")
    endif()
endif()
