# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(_uml_resolve_kernel_program candidate output)
    if(candidate STREQUAL "")
        set(${output} "" PARENT_SCOPE)
        return()
    endif()

    unset(_resolved CACHE)
    unset(_resolved)
    if(IS_ABSOLUTE "${candidate}")
        get_filename_component(_candidate_dir "${candidate}" DIRECTORY)
        get_filename_component(_candidate_name "${candidate}" NAME)
        find_program(_resolved
            NAMES "${_candidate_name}"
            PATHS "${_candidate_dir}"
            NO_DEFAULT_PATH
            NO_CACHE)
    else()
        find_program(_resolved NAMES "${candidate}" NO_CACHE)
    endif()
    if(_resolved MATCHES "-NOTFOUND$")
        set(_resolved "")
    endif()
    set(${output} "${_resolved}" PARENT_SCOPE)
endfunction()

function(uml_kernel_compiler_supports_required_features)
    cmake_parse_arguments(ARG "" "COMPILER;WORK_DIR;OUT_SUPPORTED;OUT_DIAGNOSTIC" "" ${ARGN})
    if(NOT ARG_COMPILER OR NOT ARG_WORK_DIR OR NOT ARG_OUT_SUPPORTED OR
       NOT ARG_OUT_DIAGNOSTIC)
        message(FATAL_ERROR
            "uml_kernel_compiler_supports_required_features requires "
            "COMPILER, WORK_DIR, OUT_SUPPORTED, and OUT_DIAGNOSTIC")
    endif()

    file(MAKE_DIRECTORY "${ARG_WORK_DIR}")
    string(SHA256 _compiler_key "${ARG_COMPILER}")
    set(_source "${ARG_WORK_DIR}/counted-by-${_compiler_key}.c")
    set(_object "${ARG_WORK_DIR}/counted-by-${_compiler_key}.o")
    file(WRITE "${_source}"
        "#define __counted_by(member) __attribute__((__counted_by__(member)))\n"
        "struct uml_counted_by_probe { int count; int values[] __counted_by(count); };\n"
        "int uml_counted_by_probe(struct uml_counted_by_probe *probe) {\n"
        "  return __builtin_types_compatible_p(__typeof__(probe->values), __typeof__(&(probe->values)[0]));\n"
        "}\n")
    file(REMOVE "${_object}")
    execute_process(
        COMMAND "${ARG_COMPILER}" -x c -Werror -c
            -o "${_object}" "${_source}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(_result EQUAL 0 AND EXISTS "${_object}")
        file(SIZE "${_object}" _object_size)
        if(_object_size GREATER 0)
            set(_supported TRUE)
        else()
            set(_supported FALSE)
            string(APPEND _error
                "compiler produced an empty object at ${_object}")
        endif()
    else()
        set(_supported FALSE)
        if(_result EQUAL 0)
            string(APPEND _error
                "compiler reported success without producing ${_object}")
        endif()
    endif()
    file(REMOVE "${_object}")
    string(STRIP "${_output}${_error}" _diagnostic)
    set(${ARG_OUT_SUPPORTED} ${_supported} PARENT_SCOPE)
    set(${ARG_OUT_DIAGNOSTIC} "${_diagnostic}" PARENT_SCOPE)
endfunction()

function(uml_select_kernel_clang)
    cmake_parse_arguments(ARG "" "OUT_COMPILER;WORK_DIR;REQUESTED" "CANDIDATES" ${ARGN})
    if(NOT ARG_OUT_COMPILER OR NOT ARG_WORK_DIR)
        message(FATAL_ERROR
            "uml_select_kernel_clang requires OUT_COMPILER and WORK_DIR")
    endif()

    if(ARG_REQUESTED)
        set(_candidates "${ARG_REQUESTED}")
        set(_explicit TRUE)
    else()
        set(_candidates ${ARG_CANDIDATES})
        set(_explicit FALSE)
    endif()
    list(REMOVE_DUPLICATES _candidates)

    set(_attempts)
    foreach(_candidate IN LISTS _candidates)
        if(_candidate STREQUAL "")
            continue()
        endif()
        _uml_resolve_kernel_program("${_candidate}" _compiler)
        if(NOT _compiler)
            list(APPEND _attempts "${_candidate}: unavailable")
            if(_explicit)
                message(FATAL_ERROR
                    "Requested kernel compiler is unavailable or not "
                    "executable: ${_candidate}")
            endif()
            continue()
        endif()
        uml_kernel_compiler_supports_required_features(
            COMPILER "${_compiler}"
            WORK_DIR "${ARG_WORK_DIR}"
            OUT_SUPPORTED _supported
            OUT_DIAGNOSTIC _diagnostic)
        if(_supported)
            set(${ARG_OUT_COMPILER} "${_compiler}" PARENT_SCOPE)
            return()
        endif()
        list(APPEND _attempts "${_compiler}: missing required language support")
        if(_explicit)
            string(JOIN "; " _attempt_text ${_attempts})
            message(FATAL_ERROR
                "Requested kernel compiler does not support Linux 6.18 "
                "__counted_by type expressions: ${_attempt_text}\n"
                "${_diagnostic}")
        endif()
    endforeach()

    if(_attempts)
        string(JOIN "; " _attempt_text ${_attempts})
    else()
        set(_attempt_text "no candidates")
    endif()
    message(FATAL_ERROR
        "No compatible kernel compiler was found. Linux 6.18 requires "
        "__counted_by type-expression support. Tried: ${_attempt_text}. "
        "Set UML_KERNEL_TOOLCHAIN_ROOT or UML_KERNEL_CC to a recent LLVM "
        "installation.")
endfunction()

function(uml_select_kernel_program)
    cmake_parse_arguments(ARG "ALLOW_PATH_SEARCH"
        "OUT_PROGRAM;DESCRIPTION;REQUESTED;SIBLING_DIR"
        "NAMES;CANDIDATES" ${ARGN})
    if(NOT ARG_OUT_PROGRAM OR NOT ARG_DESCRIPTION OR NOT ARG_NAMES)
        message(FATAL_ERROR
            "uml_select_kernel_program requires OUT_PROGRAM, DESCRIPTION, "
            "and NAMES")
    endif()

    if(ARG_REQUESTED)
        set(_candidates "${ARG_REQUESTED}")
        set(_explicit TRUE)
    else()
        set(_candidates)
        if(ARG_SIBLING_DIR)
            foreach(_name IN LISTS ARG_NAMES)
                list(APPEND _candidates "${ARG_SIBLING_DIR}/${_name}")
            endforeach()
        endif()
        list(APPEND _candidates ${ARG_CANDIDATES})
        if(ARG_ALLOW_PATH_SEARCH)
            list(APPEND _candidates ${ARG_NAMES})
        endif()
        set(_explicit FALSE)
    endif()
    list(REMOVE_DUPLICATES _candidates)

    set(_attempts)
    foreach(_candidate IN LISTS _candidates)
        if(_candidate STREQUAL "")
            continue()
        endif()
        _uml_resolve_kernel_program("${_candidate}" _program)
        if(_program)
            set(${ARG_OUT_PROGRAM} "${_program}" PARENT_SCOPE)
            return()
        endif()
        list(APPEND _attempts "${_candidate}")
    endforeach()

    if(_attempts)
        string(JOIN ", " _attempt_text ${_attempts})
    else()
        set(_attempt_text "no candidates")
    endif()
    if(_explicit)
        message(FATAL_ERROR
            "Requested ${ARG_DESCRIPTION} is unavailable or not executable: "
            "${_attempt_text}")
    endif()
    message(FATAL_ERROR
        "No ${ARG_DESCRIPTION} was found. Tried: ${_attempt_text}. "
        "Install the tools beside the selected clang or set the matching "
        "UML_KERNEL_* override explicitly.")
endfunction()

function(uml_select_kernel_toolchain)
    set(_one_value_args
        WORK_DIR
        TOOLCHAIN_ROOT
        CONFIGURED_CC
        REQUESTED_CC
        REQUESTED_HOST_CC
        REQUESTED_LD
        REQUESTED_AR
        REQUESTED_NM
        REQUESTED_OBJCOPY
        REQUESTED_OBJDUMP
        REQUESTED_READELF
        REQUESTED_STRIP
        OUT_CC
        OUT_HOST_CC
        OUT_LD
        OUT_AR
        OUT_NM
        OUT_OBJCOPY
        OUT_OBJDUMP
        OUT_READELF
        OUT_STRIP)
    cmake_parse_arguments(ARG "" "${_one_value_args}" "COMPILER_CANDIDATES" ${ARGN})
    foreach(_required IN ITEMS WORK_DIR OUT_CC OUT_HOST_CC OUT_LD OUT_AR
            OUT_NM OUT_OBJCOPY OUT_OBJDUMP OUT_READELF OUT_STRIP)
        if(NOT ARG_${_required})
            message(FATAL_ERROR
                "uml_select_kernel_toolchain requires ${_required}")
        endif()
    endforeach()

    if(ARG_REQUESTED_CC)
        set(_requested_cc "${ARG_REQUESTED_CC}")
        set(_compiler_candidates)
    elseif(ARG_TOOLCHAIN_ROOT)
        set(_requested_cc "${ARG_TOOLCHAIN_ROOT}/bin/clang")
        set(_compiler_candidates)
    else()
        set(_requested_cc "")
        set(_compiler_candidates
            "${ARG_CONFIGURED_CC}"
            ${ARG_COMPILER_CANDIDATES})
    endif()
    uml_select_kernel_clang(
        OUT_COMPILER _cc
        WORK_DIR "${ARG_WORK_DIR}/target"
        REQUESTED "${_requested_cc}"
        CANDIDATES ${_compiler_candidates})

    if(ARG_REQUESTED_HOST_CC)
        # HOSTCC compiles build utilities, not Linux target translation units.
        # Keep an explicit host compiler authoritative after checking only
        # that it can be executed; it need not support target-only language
        # constructs used by Linux 6.18.
        uml_select_kernel_program(
            OUT_PROGRAM _host_cc
            DESCRIPTION "kernel host compiler"
            REQUESTED "${ARG_REQUESTED_HOST_CC}"
            NAMES clang)
    else()
        set(_host_cc "${_cc}")
    endif()

    get_filename_component(_toolchain_dir "${_cc}" DIRECTORY)
    uml_select_kernel_program(
        OUT_PROGRAM _ld
        DESCRIPTION "kernel linker"
        REQUESTED "${ARG_REQUESTED_LD}"
        SIBLING_DIR "${_toolchain_dir}"
        NAMES ld.lld
        ALLOW_PATH_SEARCH)

    foreach(_tool IN ITEMS AR NM OBJCOPY OBJDUMP READELF STRIP)
        string(TOLOWER "${_tool}" _tool_name)
        uml_select_kernel_program(
            OUT_PROGRAM _${_tool_name}
            DESCRIPTION "kernel llvm-${_tool_name} tool"
            REQUESTED "${ARG_REQUESTED_${_tool}}"
            SIBLING_DIR "${_toolchain_dir}"
            NAMES "llvm-${_tool_name}")
    endforeach()

    set(${ARG_OUT_CC} "${_cc}" PARENT_SCOPE)
    set(${ARG_OUT_HOST_CC} "${_host_cc}" PARENT_SCOPE)
    set(${ARG_OUT_LD} "${_ld}" PARENT_SCOPE)
    set(${ARG_OUT_AR} "${_ar}" PARENT_SCOPE)
    set(${ARG_OUT_NM} "${_nm}" PARENT_SCOPE)
    set(${ARG_OUT_OBJCOPY} "${_objcopy}" PARENT_SCOPE)
    set(${ARG_OUT_OBJDUMP} "${_objdump}" PARENT_SCOPE)
    set(${ARG_OUT_READELF} "${_readelf}" PARENT_SCOPE)
    set(${ARG_OUT_STRIP} "${_strip}" PARENT_SCOPE)
endfunction()
