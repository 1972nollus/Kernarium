# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED PREFIX)
    message(FATAL_ERROR "PREFIX is required for dependency verification")
endif()
if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required for dependency verification")
endif()
if(NOT DEFINED PLATFORM)
    set(PLATFORM macos)
endif()

if(PLATFORM STREQUAL "macos")
    set(_platform_contract macos-gpu-arm64.tsv)
    set(_mach_platform 1)
    set(_deployment_target 14.0)
    set(_minimum_os_key LSMinimumSystemVersion)
elseif(PLATFORM STREQUAL "ios-simulator")
    set(_platform_contract ios-simulator-gpu-arm64.tsv)
    set(_mach_platform 7)
    set(_deployment_target 16.0)
    set(_minimum_os_key MinimumOSVersion)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_contract ios-device-gpu-arm64.tsv)
    set(_mach_platform 2)
    set(_deployment_target 16.0)
    set(_minimum_os_key MinimumOSVersion)
else()
    message(FATAL_ERROR "Unsupported Apple GPU platform: ${PLATFORM}")
endif()

if(NOT IS_DIRECTORY "${PREFIX}" OR IS_SYMLINK "${PREFIX}")
    message(FATAL_ERROR "Invalid Apple GPU dependency prefix: ${PREFIX}")
endif()

foreach(_tool IN ITEMS ar lipo nm otool)
    execute_process(
        COMMAND xcrun --find "${_tool}"
        RESULT_VARIABLE _tool_status
        OUTPUT_VARIABLE _tool_path
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _tool_status EQUAL 0 OR NOT EXISTS "${_tool_path}")
        message(FATAL_ERROR "Xcode tool is unavailable: ${_tool}")
    endif()
    set(_tool_${_tool} "${_tool_path}")
endforeach()

set(_contract_source
    "${SOURCE_ROOT}/dependencies/platforms/${_platform_contract}")
set(_contract_installed
    "${PREFIX}/share/uml-dependencies/${_platform_contract}")
if(NOT EXISTS "${_contract_source}" OR IS_SYMLINK "${_contract_source}")
    message(FATAL_ERROR "Apple GPU platform contract is missing: ${_contract_source}")
endif()

function(_contract_expect _field _expected)
    set(_matches)
    foreach(_row IN LISTS _contract_rows)
        string(REPLACE "\t" ";" _values "${_row}")
        list(GET _values 0 _candidate)
        if(_candidate STREQUAL "${_field}")
            list(APPEND _matches "${_row}")
        endif()
    endforeach()
    list(LENGTH _matches _match_count)
    if(NOT _match_count EQUAL 1)
        message(FATAL_ERROR
            "Apple GPU platform contract has ${_match_count} ${_field} fields")
    endif()
    list(GET _matches 0 _row)
    string(REPLACE "\t" ";" _values "${_row}")
    list(LENGTH _values _value_count)
    if(NOT _value_count EQUAL 2)
        message(FATAL_ERROR "Malformed Apple GPU contract field: ${_field}")
    endif()
    list(GET _values 1 _actual)
    if(NOT _actual STREQUAL "${_expected}")
        message(FATAL_ERROR
            "Apple GPU contract ${_field} is ${_actual}, expected ${_expected}")
    endif()
endfunction()

set(_contract_fields
    format platform architecture deployment_target linkage payload
    gl_provider graphics_api presentation_api renderer_features components
    source_manifest)
file(STRINGS "${_contract_installed}" _contract_rows)
set(_seen_contract_fields)
foreach(_row IN LISTS _contract_rows)
    if(_row MATCHES "^#" OR _row STREQUAL "" OR _row STREQUAL "field\tvalue")
        continue()
    endif()
    string(REPLACE "\t" ";" _values "${_row}")
    list(LENGTH _values _value_count)
    if(NOT _value_count EQUAL 2)
        message(FATAL_ERROR "Malformed Apple GPU platform contract row: ${_row}")
    endif()
    list(GET _values 0 _field)
    list(FIND _contract_fields "${_field}" _field_index)
    list(FIND _seen_contract_fields "${_field}" _seen_index)
    if(_field_index EQUAL -1 OR NOT _seen_index EQUAL -1)
        message(FATAL_ERROR
            "Unexpected or duplicate Apple GPU contract field: ${_field}")
    endif()
    list(APPEND _seen_contract_fields "${_field}")
endforeach()
list(LENGTH _contract_fields _expected_contract_count)
list(LENGTH _seen_contract_fields _actual_contract_count)
if(NOT _actual_contract_count EQUAL _expected_contract_count)
    message(FATAL_ERROR "Apple GPU platform contract is incomplete")
endif()

_contract_expect(format 3)
_contract_expect(platform "${PLATFORM}")
_contract_expect(architecture arm64)
_contract_expect(deployment_target "${_deployment_target}")
_contract_expect(linkage static-and-framework)
_contract_expect(payload
    "static-libepoxy,static-libvirglrenderer,dynamic-EGL-framework,dynamic-GLESv2-framework")
_contract_expect(gl_provider angle-metal)
_contract_expect(graphics_api ANGLE-EGL-GLES)
_contract_expect(presentation_api Metal)
_contract_expect(renderer_features
    "gles,native-share-texture,render-server-thread,video-h264-decode,video-hevc-decode,video-jpeg-decode")
_contract_expect(components utm-webkit,utm-libepoxy,utm-virglrenderer)
_contract_expect(source_manifest dependencies/manifest.tsv)

set(_provenance_files manifest.tsv "${_platform_contract}")
foreach(_series IN ITEMS
    "patches/utm-webkit/series.tsv"
    "patches/utm-libepoxy/series.tsv"
    "patches/utm-virglrenderer/series.tsv")
    list(APPEND _provenance_files "${_series}")
    get_filename_component(_series_directory "${_series}" DIRECTORY)
    file(STRINGS "${SOURCE_ROOT}/dependencies/${_series}" _series_rows)
    foreach(_row IN LISTS _series_rows)
        if(_row MATCHES "^#" OR _row STREQUAL "")
            continue()
        endif()
        string(REPLACE "\t" ";" _fields "${_row}")
        list(GET _fields 0 _order)
        if(NOT _order STREQUAL "order")
            list(GET _fields 1 _patch)
            list(APPEND _provenance_files "${_series_directory}/${_patch}")
        endif()
    endforeach()
endforeach()

foreach(_relative IN LISTS _provenance_files)
    if(_relative STREQUAL "${_platform_contract}")
        set(_source "${SOURCE_ROOT}/dependencies/platforms/${_relative}")
    else()
        set(_source "${SOURCE_ROOT}/dependencies/${_relative}")
    endif()
    set(_installed "${PREFIX}/share/uml-dependencies/${_relative}")
    if(NOT EXISTS "${_installed}" OR IS_SYMLINK "${_installed}")
        message(FATAL_ERROR "Missing Apple GPU provenance: ${_installed}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${_source}" "${_installed}"
        RESULT_VARIABLE _provenance_status)
    if(NOT _provenance_status EQUAL 0)
        message(FATAL_ERROR
            "Apple GPU dependency provenance does not match: ${_relative}")
    endif()
endforeach()

file(GLOB_RECURSE _prefix_entries LIST_DIRECTORIES true "${PREFIX}/*")
foreach(_entry IN LISTS _prefix_entries)
    if(IS_SYMLINK "${_entry}")
        message(FATAL_ERROR "Symlink in Apple GPU dependency prefix: ${_entry}")
    endif()
endforeach()

file(GLOB _top_level RELATIVE "${PREFIX}" "${PREFIX}/*")
list(SORT _top_level)
set(_expected_top_level Frameworks include lib share)
if(NOT _top_level STREQUAL _expected_top_level)
    message(FATAL_ERROR "Unexpected Apple GPU top-level payload: ${_top_level}")
endif()

file(GLOB _frameworks RELATIVE "${PREFIX}/Frameworks"
    "${PREFIX}/Frameworks/*")
list(SORT _frameworks)
set(_expected_frameworks EGL.framework GLESv2.framework)
if(NOT _frameworks STREQUAL _expected_frameworks)
    message(FATAL_ERROR "Unexpected Apple GPU frameworks: ${_frameworks}")
endif()

file(GLOB_RECURSE _library_files LIST_DIRECTORIES false
    RELATIVE "${PREFIX}/lib" "${PREFIX}/lib/*")
list(SORT _library_files)
set(_expected_library_files
    libepoxy.a libvirglrenderer.a)
list(SORT _expected_library_files)
if(NOT _library_files STREQUAL _expected_library_files)
    message(FATAL_ERROR "Unexpected Apple GPU library payload: ${_library_files}")
endif()

file(GLOB _include_roots RELATIVE "${PREFIX}/include" "${PREFIX}/include/*")
list(SORT _include_roots)
set(_expected_include_roots EGL GLES GLES2 GLES3 KHR epoxy virgl)
if(NOT _include_roots STREQUAL _expected_include_roots)
    message(FATAL_ERROR "Unexpected Apple GPU include payload: ${_include_roots}")
endif()

file(GLOB_RECURSE _installed_provenance LIST_DIRECTORIES false
    RELATIVE "${PREFIX}/share/uml-dependencies"
    "${PREFIX}/share/uml-dependencies/*")
list(SORT _installed_provenance)
set(_expected_provenance ${_provenance_files})
list(SORT _expected_provenance)
if(NOT _installed_provenance STREQUAL _expected_provenance)
    message(FATAL_ERROR
        "Unexpected Apple GPU provenance payload: ${_installed_provenance}")
endif()

set(_static_archives
    "${PREFIX}/lib/libepoxy.a"
    "${PREFIX}/lib/libvirglrenderer.a")
set(_framework_binaries
    "${PREFIX}/Frameworks/EGL.framework/EGL"
    "${PREFIX}/Frameworks/GLESv2.framework/GLESv2")
set(_required
    ${_static_archives}
    ${_framework_binaries}
    "${PREFIX}/include/epoxy/egl.h"
    "${PREFIX}/include/virgl/virglrenderer.h"
    "${PREFIX}/include/virgl/virgl_hw.h"
    "${PREFIX}/Frameworks/EGL.framework/Info.plist"
    "${PREFIX}/Frameworks/GLESv2.framework/Info.plist")
foreach(_file IN LISTS _required)
    if(NOT EXISTS "${_file}" OR IS_SYMLINK "${_file}")
        message(FATAL_ERROR "Invalid Apple GPU dependency: ${_file}")
    endif()
endforeach()

foreach(_archive IN LISTS _static_archives)
    execute_process(COMMAND "${_tool_ar}" -t "${_archive}"
        RESULT_VARIABLE _archive_status OUTPUT_QUIET ERROR_QUIET)
    if(NOT _archive_status EQUAL 0)
        message(FATAL_ERROR "Invalid Apple GPU static archive: ${_archive}")
    endif()
    execute_process(COMMAND "${_tool_nm}" -u "${_archive}"
        RESULT_VARIABLE _archive_nm_status
        OUTPUT_VARIABLE _archive_undefined_symbols)
    if(NOT _archive_nm_status EQUAL 0 OR
       _archive_undefined_symbols MATCHES "_(CGL|EAGL|vk[A-Z])")
        message(FATAL_ERROR
            "${_archive} contains a forbidden provider symbol")
    endif()
endforeach()

foreach(_binary IN LISTS _static_archives _framework_binaries)
    execute_process(
        COMMAND "${_tool_lipo}" -archs "${_binary}"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _architectures
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _status EQUAL 0 OR NOT _architectures STREQUAL "arm64")
        message(FATAL_ERROR
            "${_binary} is not an arm64-only Apple binary: ${_architectures}")
    endif()
    execute_process(
        COMMAND "${_tool_otool}" -l "${_binary}"
        RESULT_VARIABLE _otool_status
        OUTPUT_VARIABLE _load_commands)
    if(NOT _otool_status EQUAL 0)
        message(FATAL_ERROR "Could not inspect Apple GPU binary: ${_binary}")
    endif()
    string(REGEX MATCHALL "platform [0-9]+" _platform_rows "${_load_commands}")
    string(REGEX MATCHALL "minos [0-9]+\\.[0-9]+" _minimum_rows "${_load_commands}")
    if(NOT _platform_rows OR NOT _minimum_rows)
        message(FATAL_ERROR "Apple GPU binary has no build-version command: ${_binary}")
    endif()
    foreach(_row IN LISTS _platform_rows)
        if(NOT _row STREQUAL "platform ${_mach_platform}")
            message(FATAL_ERROR "Wrong Mach-O platform in ${_binary}: ${_row}")
        endif()
    endforeach()
    foreach(_row IN LISTS _minimum_rows)
        if(NOT _row STREQUAL "minos ${_deployment_target}")
            message(FATAL_ERROR "Wrong deployment target in ${_binary}: ${_row}")
        endif()
    endforeach()
endforeach()

foreach(_framework IN ITEMS EGL GLESv2)
    set(_binary "${PREFIX}/Frameworks/${_framework}.framework/${_framework}")
    file(GLOB_RECURSE _framework_payload LIST_DIRECTORIES false
        RELATIVE "${PREFIX}/Frameworks/${_framework}.framework"
        "${PREFIX}/Frameworks/${_framework}.framework/*")
    list(SORT _framework_payload)
    set(_expected_framework_payload Info.plist "${_framework}")
    list(SORT _expected_framework_payload)
    if(NOT _framework_payload STREQUAL _expected_framework_payload)
        message(FATAL_ERROR
            "Unexpected ${_framework}.framework payload: ${_framework_payload}")
    endif()

    execute_process(COMMAND "${_tool_otool}" -D "${_binary}"
        RESULT_VARIABLE _id_status OUTPUT_VARIABLE _install_name_output)
    if(NOT _id_status EQUAL 0 OR NOT _install_name_output MATCHES
       "@rpath/${_framework}\\.framework/${_framework}")
        message(FATAL_ERROR "${_framework}.framework has an invalid install name")
    endif()
    execute_process(COMMAND "${_tool_otool}" -l "${_binary}"
        RESULT_VARIABLE _load_status OUTPUT_VARIABLE _load_commands)
    if(NOT _load_status EQUAL 0 OR _load_commands MATCHES "LC_SUB_CLIENT" OR
       _load_commands MATCHES "LC_RPATH")
        message(FATAL_ERROR
            "${_framework}.framework has an invalid client or rpath command")
    endif()
    execute_process(COMMAND plutil -extract "${_minimum_os_key}" raw -o -
        "${PREFIX}/Frameworks/${_framework}.framework/Info.plist"
        RESULT_VARIABLE _plist_status OUTPUT_VARIABLE _minimum_os
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _plist_status EQUAL 0 OR
       NOT _minimum_os STREQUAL "${_deployment_target}")
        message(FATAL_ERROR
            "${_framework}.framework has an invalid deployment target")
    endif()

    execute_process(COMMAND "${_tool_otool}" -L "${_binary}"
        RESULT_VARIABLE _closure_status OUTPUT_VARIABLE _closure)
    if(NOT _closure_status EQUAL 0)
        message(FATAL_ERROR "Could not inspect ${_framework}.framework closure")
    endif()
    string(REPLACE "\n" ";" _closure_lines "${_closure}")
    foreach(_line IN LISTS _closure_lines)
        if(NOT _line MATCHES "^[ \t]+([^ \t]+)")
            continue()
        endif()
        set(_dependency "${CMAKE_MATCH_1}")
        if(_dependency STREQUAL "@rpath/${_framework}.framework/${_framework}" OR
           _dependency MATCHES "^/usr/lib/(libc\\+\\+\\.1|libSystem\\.B|libobjc\\.A|libz\\.1)\\.dylib$" OR
           _dependency MATCHES "^/System/Library/Frameworks/(CoreFoundation|CoreGraphics|Foundation|IOKit|IOSurface|Metal|QuartzCore)\\.framework/")
            continue()
        endif()
        message(FATAL_ERROR
            "Unexpected ${_framework}.framework dependency: ${_dependency}")
    endforeach()
    if(_closure MATCHES "OpenGL|OpenGLES|MoltenVK|Vulkan")
        message(FATAL_ERROR
            "${_framework}.framework contains a forbidden GL/Vulkan dependency")
    endif()
    execute_process(COMMAND "${_tool_nm}" -u "${_binary}"
        RESULT_VARIABLE _nm_status OUTPUT_VARIABLE _undefined_symbols)
    if(NOT _nm_status EQUAL 0 OR
       _undefined_symbols MATCHES "_(CGL|EAGL|vk[A-Z])")
        message(FATAL_ERROR
            "${_framework}.framework contains a forbidden provider symbol")
    endif()
    if(_framework STREQUAL "EGL")
        execute_process(COMMAND "${_tool_nm}" -gU "${_binary}"
            RESULT_VARIABLE _export_status OUTPUT_VARIABLE _exports)
        if(NOT _export_status EQUAL 0 OR
           NOT _exports MATCHES "[ 	]_eglGetPlatformDisplayEXT([ 	\n]|$)")
            message(FATAL_ERROR
                "EGL.framework does not export the ANGLE platform bootstrap")
        endif()
    endif()
endforeach()

message(STATUS
    "Verified ${PLATFORM} Apple GPU dependency prefix: ${PREFIX}")
