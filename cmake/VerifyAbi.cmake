# SPDX-License-Identifier: GPL-2.0-or-later
include_guard(GLOBAL)

function(uml_verify_generated_abi)
    execute_process(
        COMMAND "${PROJECT_SOURCE_DIR}/kernel/abi/sync.sh" --verify
        RESULT_VARIABLE _uml_abi_result
        OUTPUT_VARIABLE _uml_abi_output
        ERROR_VARIABLE _uml_abi_error)
    if(NOT _uml_abi_result EQUAL 0)
        message(FATAL_ERROR
            "Generated Darwin UML host ABI files are stale. Run "
            "kernel/abi/sync.sh.\n${_uml_abi_output}${_uml_abi_error}")
    endif()

    file(STRINGS "${PROJECT_SOURCE_DIR}/kernel/abi/identity.env"
        _uml_identity_rows)
    foreach(_uml_identity_row IN LISTS _uml_identity_rows)
        if(_uml_identity_row MATCHES
           "^DARWIN_UML_ABI_VERSION=([0-9]+)$")
            set(_uml_abi_version "${CMAKE_MATCH_1}")
        elseif(_uml_identity_row MATCHES
               "^DARWIN_UML_ABI_FINGERPRINT='(0x[0-9A-Fa-f]+)'$")
            set(_uml_abi_fingerprint "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT DEFINED _uml_abi_version OR
       NOT DEFINED _uml_abi_fingerprint)
        message(FATAL_ERROR "Unable to parse kernel/abi/identity.env")
    endif()
    set(UML_KERNEL_ABI_VERSION "${_uml_abi_version}" CACHE INTERNAL
        "Canonical Darwin UML host/kernel ABI version" FORCE)
    set(UML_KERNEL_ABI_FINGERPRINT "${_uml_abi_fingerprint}" CACHE INTERNAL
        "Canonical Darwin UML host/kernel ABI fingerprint" FORCE)
    file(WRITE "${CMAKE_BINARY_DIR}/uml-abi-identity.txt"
        "version=${_uml_abi_version}\n"
        "fingerprint=${_uml_abi_fingerprint}\n")

    include(CheckCXXSourceCompiles)
    set(_uml_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
    set(CMAKE_REQUIRED_INCLUDES
        "${PROJECT_SOURCE_DIR}/darwin/api/include")
    set(_uml_abi_consumer_source
        "#include <uml/emulator.h>\n"
        "static_assert(DARWIN_UML_ABI_VERSION == ${_uml_abi_version}U);\n"
        "static_assert(DARWIN_UML_ABI_FINGERPRINT == ${_uml_abi_fingerprint}ULL);\n"
        "int main() { return 0; }\n")
    check_cxx_source_compiles(
        "${_uml_abi_consumer_source}"
        UML_EXTERNAL_ABI_IDENTITY_COMPILES)
    set(CMAKE_REQUIRED_INCLUDES "${_uml_saved_required_includes}")
    if(NOT UML_EXTERNAL_ABI_IDENTITY_COMPILES)
        message(FATAL_ERROR
            "Public external-consumer ABI identity does not match identity.env")
    endif()
endfunction()
