# SPDX-License-Identifier: GPL-2.0-or-later

if(UML_TARGET_MACOS AND NOT UML_GADGET_DEPENDENCY_COVERAGE)
    add_executable(uml_scalar_ceiling_bench EXCLUDE_FROM_ALL
        bench.cpp workloads.S
        $<TARGET_OBJECTS:uml_static_mmu>
        $<TARGET_OBJECTS:uml_arm64_gadgets>
        $<TARGET_OBJECTS:uml_arm64_execution_helpers>)
    target_link_libraries(uml_scalar_ceiling_bench PRIVATE
        UML::CpuContract UML::MemorySharedBacking)
    target_include_directories(uml_scalar_ceiling_bench PRIVATE
        ${PROJECT_SOURCE_DIR}/darwin ${PROJECT_SOURCE_DIR}/darwin/include)
    target_compile_definitions(uml_scalar_ceiling_bench PRIVATE
        UML_SCALAR_CEILING_GADGETS=1
        UML_GADGET_SCALAR_FORWARDING=$<BOOL:${UML_GADGET_SCALAR_FORWARDING}>)
    target_compile_options(uml_scalar_ceiling_bench PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
endif()
