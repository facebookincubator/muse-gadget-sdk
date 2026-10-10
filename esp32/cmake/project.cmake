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

# Shared setup for the standalone application and an enclosing internal project.
# Include before project(); source lists and dependencies stay in the components.
get_filename_component(GADGET_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Keep each build's generated configuration beside its build artifacts.
# Explicit -DSDKCONFIG paths, including the flash helper's, still take priority.
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "Project configuration")
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL GADGET_SOURCE_DIR)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${GADGET_SOURCE_DIR}/main"
        "${GADGET_SOURCE_DIR}/components"
    )
endif()

# When the component manager is off, CMake still needs managed dependencies on the
# component search path (they are normally injected by idf_component_manager).
if(DEFINED ENV{IDF_COMPONENT_MANAGER} AND "$ENV{IDF_COMPONENT_MANAGER}" STREQUAL "0")
    list(APPEND EXTRA_COMPONENT_DIRS "${GADGET_SOURCE_DIR}/managed_components")
endif()

# ESP-IDF reads version.txt from the active project root unless PROJECT_VER is set.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
