# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT UML_TARGET_MACOS)
    message(FATAL_ERROR
        "Source-built desktop dependencies are currently supported only on macOS")
endif()

set(_uml_dependency_verifier
    "${CMAKE_CURRENT_SOURCE_DIR}/verify-prefix.sh")
set(_uml_dependency_archive_tool
    "${CMAKE_CURRENT_SOURCE_DIR}/source_archive.py")
set(_uml_dependency_link_verifier
    "${CMAKE_CURRENT_SOURCE_DIR}/verify-production-link.sh")
set(_uml_dependency_receipt
    "${UML_DEPENDENCY_PREFIX}/share/uml-dependencies/receipt.tsv")
file(GLOB_RECURSE _uml_dependency_prefix_inputs CONFIGURE_DEPENDS
    LIST_DIRECTORIES false "${UML_DEPENDENCY_PREFIX}/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.tsv"
    "${CMAKE_CURRENT_SOURCE_DIR}/build.sh"
    "${_uml_dependency_verifier}"
    "${_uml_dependency_archive_tool}"
    "${_uml_dependency_link_verifier}"
    "${_uml_dependency_receipt}"
    ${_uml_dependency_prefix_inputs})

uml_verify_dependency_prefix(
    PREFIX "${UML_DEPENDENCY_PREFIX}"
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/manifest.tsv"
    BUILD_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/build.sh"
    DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")

add_custom_target(uml_dependency_prefix_verify
    COMMAND "${_uml_dependency_verifier}"
        --prefix "${UML_DEPENDENCY_PREFIX}"
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/manifest.tsv"
        --build-script "${CMAKE_CURRENT_SOURCE_DIR}/build.sh"
        --deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/manifest.tsv"
        "${CMAKE_CURRENT_SOURCE_DIR}/build.sh"
        "${_uml_dependency_verifier}"
        "${_uml_dependency_archive_tool}"
        "${_uml_dependency_receipt}"
    COMMENT "Revalidating the private dependency prefix before linking"
    VERBATIM)

find_package(Threads REQUIRED)
include(cmake/UmlThirdPartyTargets.cmake)
uml_define_third_party_targets("${UML_DEPENDENCY_PREFIX}")
