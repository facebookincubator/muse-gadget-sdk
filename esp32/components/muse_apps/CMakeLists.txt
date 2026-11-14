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

# WebAssembly apps (CONFIG_MUSE_WASM_APPS). Optional: without the
# option this component builds nothing and its manifest pulls in no WAMR.
set(srcs)
set(embed)
if(CONFIG_MUSE_WASM_APPS)
    list(APPEND srcs
        "muse_apps.c"          # the runtime: app task, lifecycle, budget, manifest, app.* commands
        "muse_apps_stubs.c"    # a weak default for every import (muse_apps_api.h)
        "muse_apps_sys.c"      # sys_*, muse_*
        "muse_apps_vm.c")      # whole files on the VM: app.run's download
    # The installed apps (store, catalog), the gadget's own files (flash) and
    # the launcher (launcher, icons) beside the API's modules.
    foreach(module ui canvas chat files dev store catalog launcher icons flash capabilities)
        if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/muse_apps_${module}.c")
            list(APPEND srcs "muse_apps_${module}.c")
        endif()
    endforeach()
    # What app.api serves: generated from components/muse_apps/sdk/api/api.json by gen_api.py.
    list(APPEND embed "${CMAKE_CURRENT_LIST_DIR}/sdk/api/api.capabilities.json")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    EMBED_TXTFILES ${embed}
    PRIV_REQUIRES muse capabilities_registry esp_timer esp_hw_support esp_netif esp_partition pthread mbedtls nvs_flash
    # Whole, so the modules' imports override the weak stubs.
    WHOLE_ARCHIVE
)

if(CONFIG_MUSE_WASM_APPS)
    # WAMR 2.4's ESP-IDF port on IDF v6, interpreter only. Its file layer is
    # for WASI (off) and wants newlib's *at() calls, which picolibc lacks; its
    # platform file wants <sys/stat.h>. Instruction metering gives each call
    # into an app its budget.
    idf_component_get_property(wamr_lib wasm-micro-runtime COMPONENT_LIB)
    idf_component_get_property(wamr_dir wasm-micro-runtime COMPONENT_DIR)
    set_source_files_properties("${wamr_dir}/core/shared/platform/esp-idf/espidf_file.c"
        TARGET_DIRECTORY ${wamr_lib} PROPERTIES HEADER_FILE_ONLY TRUE)
    # Its memory-map layer takes linear memory outside the app's budget (and
    # off the S3 with heap_caps_malloc(MALLOC_CAP_8BIT), internal RAM first):
    # ours keeps it in PSRAM and counts it (muse_apps_wamr_mem.c, muse_apps.c).
    set_source_files_properties("${wamr_dir}/core/shared/platform/esp-idf/espidf_memmap.c"
        TARGET_DIRECTORY ${wamr_lib} PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(${wamr_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/muse_apps_wamr_mem.c")
    target_compile_options(${wamr_lib} PRIVATE -include sys/stat.h)
    target_compile_definitions(${wamr_lib} PRIVATE WASM_ENABLE_INSTRUCTION_METERING=1)
endif()
