# SPDX-License-Identifier: GPL-2.0-or-later

# These freestanding payloads are manual performance controls, not correctness
# tests. Keep them buildable from the canonical graph without adding them to a
# CTest suite or an ordinary suite-binary aggregate.
set(_uml_performance_probe_manifest "${CMAKE_CURRENT_SOURCE_DIR}/probes.tsv")
set(_uml_performance_probe_common
    "${CMAKE_CURRENT_SOURCE_DIR}/vector_store32_probe.inc")
file(STRINGS "${_uml_performance_probe_manifest}" _uml_performance_probe_rows)
list(POP_FRONT _uml_performance_probe_rows _uml_performance_probe_header)
if(NOT _uml_performance_probe_header STREQUAL
        "source\tcategory\trequired_feature\trunner\tretention")
    message(FATAL_ERROR
        "Invalid performance probe manifest header: ${_uml_performance_probe_header}")
endif()

set(_uml_performance_probe_outputs)
foreach(_uml_performance_probe_row IN LISTS _uml_performance_probe_rows)
    if(_uml_performance_probe_row STREQUAL "" OR
       _uml_performance_probe_row MATCHES "^#")
        continue()
    endif()
    string(REPLACE "\t" ";" _uml_performance_probe_fields
        "${_uml_performance_probe_row}")
    list(LENGTH _uml_performance_probe_fields _uml_performance_probe_field_count)
    if(NOT _uml_performance_probe_field_count EQUAL 5)
        message(FATAL_ERROR
            "Malformed performance probe row: ${_uml_performance_probe_row}")
    endif()
    list(GET _uml_performance_probe_fields 0 _uml_performance_probe_source)
    list(GET _uml_performance_probe_fields 2 _uml_performance_probe_feature)
    get_filename_component(_uml_performance_probe_name
        "${_uml_performance_probe_source}" NAME_WE)
    set(_uml_performance_probe_output
        "${CMAKE_CURRENT_BINARY_DIR}/probes/${_uml_performance_probe_name}")
    if(_uml_performance_probe_feature STREQUAL "base")
        set(_uml_performance_probe_architecture armv8-a)
    elseif(_uml_performance_probe_feature STREQUAL "lse")
        set(_uml_performance_probe_architecture armv8.1-a+lse)
    else()
        message(FATAL_ERROR
            "Unknown performance probe feature: ${_uml_performance_probe_feature}")
    endif()
    set(_uml_performance_probe_dependencies
        "${CMAKE_CURRENT_SOURCE_DIR}/${_uml_performance_probe_source}"
        "${_uml_performance_probe_manifest}")
    if(_uml_performance_probe_source MATCHES "^guest_vector_store32_")
        list(APPEND _uml_performance_probe_dependencies
            "${_uml_performance_probe_common}")
    endif()
    add_custom_command(
        OUTPUT "${_uml_performance_probe_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/probes"
        COMMAND "${CMAKE_CXX_COMPILER}"
            --target=aarch64-unknown-linux-gnu
            -march=${_uml_performance_probe_architecture}
            -nostdlib
            -static
            -fuse-ld=lld
            -Wl,-e,_start
            "${CMAKE_CURRENT_SOURCE_DIR}/${_uml_performance_probe_source}"
            -o "${_uml_performance_probe_output}"
        COMMAND "${CMAKE_COMMAND}"
            -DOUTPUT=${_uml_performance_probe_output}
            -P ${PROJECT_SOURCE_DIR}/cmake/VerifyGeneratedFile.cmake
        DEPENDS ${_uml_performance_probe_dependencies}
        COMMENT "Building performance probe ${_uml_performance_probe_source}"
        VERBATIM)
    list(APPEND _uml_performance_probe_outputs
        "${_uml_performance_probe_output}")
endforeach()

add_custom_target(performance-probe-binaries
    DEPENDS ${_uml_performance_probe_outputs})

# The scalar ceiling is a manual benchmark, not a correctness test. Its native
# and ideal modes share the exact labeled AArch64 body; the ideal mode feeds
# those words to the production gadget Builder and never enters the executor.
add_executable(uml-gadget-ceiling-bench EXCLUDE_FROM_ALL
    gadget_ceiling_bench.cpp
    gadget_ceiling_ideal.cpp
    gadget_ceiling_workload.s
    $<TARGET_OBJECTS:uml_arm64_dispatch>
    $<TARGET_OBJECTS:uml_static_mmu>
    $<TARGET_OBJECTS:uml_arm64_gadgets>
    $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
target_include_directories(uml-gadget-ceiling-bench PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin
    ${PROJECT_SOURCE_DIR}/darwin/include)
target_link_libraries(uml-gadget-ceiling-bench PRIVATE
    UML::CpuContract)
target_compile_definitions(uml-gadget-ceiling-bench PRIVATE
    UML_GADGET_CEILING_WITH_IDEAL=1)
target_compile_features(uml-gadget-ceiling-bench PRIVATE cxx_std_20)
target_compile_options(uml-gadget-ceiling-bench PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
set_target_properties(uml-gadget-ceiling-bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

add_executable(uml-gadget-ceiling-native EXCLUDE_FROM_ALL
    gadget_ceiling_bench.cpp
    gadget_ceiling_workload.s)
target_compile_features(uml-gadget-ceiling-native PRIVATE cxx_std_20)
target_compile_options(uml-gadget-ceiling-native PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
set_target_properties(uml-gadget-ceiling-native PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

add_custom_target(gadget-ceiling-binaries
    DEPENDS uml-gadget-ceiling-bench uml-gadget-ceiling-native)
