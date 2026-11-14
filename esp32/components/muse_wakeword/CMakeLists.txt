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

# Muse wake word: the SDK-owned session core, audio rings, LFBE frontend,
# detector, and ExecuTorch runner. models/hey_muse.pte is embedded here;
# its export lives in tools/export_hey_muse.py.
#
# With MUSE_WAKEWORD_ENABLE=n this registers empty: no sources and no include
# directories. The ExecuTorch registry component is declared
# in this component's idf_component.yml with a rule on
# MUSE_WAKEWORD_EXECUTORCH, so builds without the feature don't download it,
# and the component manager adds it to this component's requirements only
# when it is part of the build.
set(srcs)
set(includes)
if(CONFIG_MUSE_WAKEWORD_ENABLE)
    list(APPEND srcs "muse_wakeword_core.c" "ww_detector.c" "ww_frontend.c" "ww_runner_et.cpp")
    # Not EMBED_FILES: its generated asm has no alignment directive,
    # and ExecuTorch Program::load requires 8-aligned program data.
    # hey_muse_pte.S.in embeds the model 16-aligned in flash instead.
    set(HEY_MUSE_PTE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_muse.pte")
    file(SIZE "${HEY_MUSE_PTE_PATH}" HEY_MUSE_PTE_LEN)
    configure_file(hey_muse_pte.S.in "${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S" @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S"
                                PROPERTIES OBJECT_DEPENDS "${HEY_MUSE_PTE_PATH}")
    list(APPEND srcs "${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S")
    list(APPEND includes "include")
endif()

# Requirements are collected in an early pass before Kconfig values exist,
# so they are unconditional.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${includes}
    PRIV_REQUIRES esp_timer
)
if(NOT CONFIG_MUSE_WAKEWORD_ENABLE)
    return()
endif()

# The registry component links nothing until a model and a kernel bundle
# are declared. The bundle is generated at configure time into the build
# directory by the component's own tools/gen_kernels.py, unmodified, from
# the model declared below. That generator refuses an executorch whose
# version and commit don't match the component's, so the bundle comes only
# from the generator the component was built with, and the build refuses a
# bundle that does not match the model or component. Wakeword builds need
# that executorch installed; see esp32/docs/executorch.md for
# EXECUTORCH_PYTHON.
executorch_add_model(PTE "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_muse.pte")
if(NOT EXECUTORCH_PYTHON)
    set(EXECUTORCH_PYTHON "$ENV{EXECUTORCH_PYTHON}")
endif()
if(NOT EXECUTORCH_PYTHON)
    idf_build_get_property(EXECUTORCH_PYTHON PYTHON)
endif()
execute_process(
    COMMAND "${EXECUTORCH_PYTHON}" -c "import executorch.codegen.tools.selective_build"
    RESULT_VARIABLE _et_no_executorch
    OUTPUT_QUIET ERROR_QUIET
)
if(_et_no_executorch)
    message(FATAL_ERROR "ExecuTorch: wakeword builds need a Python with executorch installed to generate the kernel bundle; set EXECUTORCH_PYTHON to one (see esp32/docs/executorch.md). ${EXECUTORCH_PYTHON} cannot import executorch.codegen.tools.selective_build.")
endif()
get_property(_et_dir GLOBAL PROPERTY EXECUTORCH_COMPONENT_DIR)
if(NOT _et_dir)
    if(EXISTS "${CMAKE_SOURCE_DIR}/components/executorch/tools/gen_kernels.py")
        set(_et_dir "${CMAKE_SOURCE_DIR}/components/executorch")
    else()
        file(GLOB _et_dirs "${CMAKE_SOURCE_DIR}/managed_components/*__executorch")
        list(LENGTH _et_dirs _et_n)
        if(NOT _et_n EQUAL 1)
            message(FATAL_ERROR "ExecuTorch: cannot locate the executorch component (expected exactly one managed *__executorch directory).")
        endif()
        list(GET _et_dirs 0 _et_dir)
    endif()
endif()
set(_et_bundle_dir "${CMAKE_BINARY_DIR}/executorch_kernels")
execute_process(
    COMMAND "${EXECUTORCH_PYTHON}" "${_et_dir}/tools/gen_kernels.py"
        --component-dir "${_et_dir}"
        --pte "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_muse.pte"
        --out "${_et_bundle_dir}"
    RESULT_VARIABLE _et_bundle_failed
    OUTPUT_VARIABLE _et_bundle_output
    ERROR_VARIABLE _et_bundle_output
)
if(_et_bundle_failed)
    message(FATAL_ERROR "ExecuTorch: kernel bundle generation failed:\n${_et_bundle_output}")
endif()
executorch_add_kernels(DIR "${_et_bundle_dir}")
