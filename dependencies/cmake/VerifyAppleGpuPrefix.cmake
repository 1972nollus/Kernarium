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
include("${CMAKE_CURRENT_LIST_DIR}/VerifyManifestProduct.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyPrefix.cmake")

if(PLATFORM STREQUAL "macos")
    set(_platform_contract macos-gpu-vulkan-arm64.tsv)
    set(_mach_platform 1)
    set(_minimum_os_key LSMinimumSystemVersion)
elseif(PLATFORM STREQUAL "ios")
    set(_platform_contract ios-device-gpu-vulkan-arm64.tsv)
    set(_mach_platform 2)
    set(_minimum_os_key MinimumOSVersion)
elseif(PLATFORM STREQUAL "ios-simulator")
    # KosmicKrisp is a Metal 4 driver and the iOS Simulator exposes no Metal 4
    # device, so the Simulator has no build of this stack.
    message(FATAL_ERROR
        "the iOS Simulator has no Metal 4 device; build for the iOS device")
else()
    message(FATAL_ERROR "Unsupported Apple GPU platform: ${PLATFORM}")
endif()
set(_deployment_target 26.0)

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

uml_verify_provenance_copy(
    LABEL "Apple GPU"
    SOURCE "${SOURCE_ROOT}/dependencies/platforms/${_platform_contract}"
    INSTALLED "${PREFIX}/share/uml-dependencies/${_platform_contract}")

uml_verify_dependency_manifest_product(
    SOURCE "${SOURCE_ROOT}/dependencies/manifest.tsv"
    INSTALLED "${PREFIX}/share/uml-dependencies/manifest.tsv"
    PRODUCT apple-gpu-vulkan-experiment
    LABEL "Apple GPU")
uml_verify_source_trees(
    INSTALLED "${PREFIX}/share/uml-dependencies/source-trees.tsv"
    LABEL "Apple GPU"
    SOURCES
        libepoxy "${SOURCE_ROOT}/dependencies/patches/libepoxy/series.tsv"
        virglrenderer "${SOURCE_ROOT}/dependencies/patches/virglrenderer/series.tsv"
        mesa "${SOURCE_ROOT}/dependencies/patches/mesa/series.tsv"
        venus-protocol
            "${SOURCE_ROOT}/tools/rootfs/vulkan/patches/venus-protocol/series.tsv")

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
set(_expected_frameworks
    MesaEGL.framework MesaGallium.framework KosmicKrisp.framework)
list(SORT _expected_frameworks)
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
set(_expected_include_roots EGL GL KHR epoxy virgl vk_video vulkan)
list(SORT _expected_include_roots)
if(NOT _include_roots STREQUAL _expected_include_roots)
    message(FATAL_ERROR "Unexpected Apple GPU include payload: ${_include_roots}")
endif()

file(GLOB_RECURSE _installed_provenance LIST_DIRECTORIES false
    RELATIVE "${PREFIX}/share/uml-dependencies"
    "${PREFIX}/share/uml-dependencies/*")
list(SORT _installed_provenance)
set(_expected_provenance
    manifest.tsv "${_platform_contract}" source-trees.tsv build-receipt.tsv)
list(SORT _expected_provenance)
if(NOT _installed_provenance STREQUAL _expected_provenance)
    message(FATAL_ERROR
        "Unexpected Apple GPU provenance payload: ${_installed_provenance}")
endif()

set(_receipt
    "${PREFIX}/share/uml-dependencies/build-receipt.tsv")
file(STRINGS "${_receipt}" _receipt_rows)
set(_receipt_fields
    format product platform sdk_name sdk_version sdk_path compiler
    built_at_utc source_manifest_sha256 platform_contract_sha256
    mesa_base_commit vulkan_headers_base_commit
    venus_protocol_generated_sha256)
set(_seen_receipt_fields)
foreach(_row IN LISTS _receipt_rows)
    if(_row STREQUAL "" OR _row STREQUAL "field\tvalue")
        continue()
    endif()
    string(REPLACE "\t" ";" _values "${_row}")
    list(LENGTH _values _value_count)
    if(NOT _value_count EQUAL 2)
        message(FATAL_ERROR
            "Malformed Apple GPU build receipt row: ${_row}")
    endif()
    list(GET _values 0 _field)
    list(FIND _receipt_fields "${_field}" _field_index)
    list(FIND _seen_receipt_fields "${_field}" _seen_index)
    if(_field_index EQUAL -1 OR NOT _seen_index EQUAL -1)
        message(FATAL_ERROR
            "Unexpected or duplicate Apple GPU receipt field: ${_field}")
    endif()
    list(APPEND _seen_receipt_fields "${_field}")
endforeach()
list(LENGTH _receipt_fields _expected_receipt_count)
list(LENGTH _seen_receipt_fields _actual_receipt_count)
if(NOT _actual_receipt_count EQUAL _expected_receipt_count)
    message(FATAL_ERROR "Apple GPU build receipt is incomplete")
endif()
# The loop above admits each receipt field exactly once.
function(_receipt_expect _field _expected)
    foreach(_row IN LISTS _receipt_rows)
        if(_row MATCHES "^${_field}\t(.*)$")
            if(NOT CMAKE_MATCH_1 STREQUAL "${_expected}")
                message(FATAL_ERROR
                    "Apple GPU build receipt ${_field} is ${CMAKE_MATCH_1}, expected ${_expected}")
            endif()
            return()
        endif()
    endforeach()
endfunction()
# The manifest rows and source trees above identify what the prefix was built
# from, and the platform contract is compared byte for byte. The receipt's
# manifest, contract, and base-commit fields remain for older checkouts only.
_receipt_expect(format 2)
_receipt_expect(product apple-gpu-vulkan-experiment)
_receipt_expect(platform "${PLATFORM}")
foreach(_row IN LISTS _receipt_rows)
    if(_row MATCHES "^venus_protocol_generated_sha256\t(.*)$")
        string(LENGTH "${CMAKE_MATCH_1}" _length)
        if(NOT CMAKE_MATCH_1 MATCHES "^[0-9a-f]+$" OR NOT _length EQUAL 64)
            message(FATAL_ERROR
                "Invalid protocol build identity: venus_protocol_generated_sha256")
        endif()
    endif()
endforeach()
foreach(_row IN LISTS _receipt_rows)
    if(_row MATCHES "^(sdk_name|sdk_version|sdk_path|compiler|built_at_utc)\t$")
        message(FATAL_ERROR
            "Apple GPU build receipt has invalid ${CMAKE_MATCH_1}")
    endif()
endforeach()

set(_static_archives
    "${PREFIX}/lib/libepoxy.a"
    "${PREFIX}/lib/libvirglrenderer.a")
set(_gl_frameworks MesaEGL MesaGallium)
set(_framework_binaries)
foreach(_framework IN LISTS _gl_frameworks)
    list(APPEND _framework_binaries
        "${PREFIX}/Frameworks/${_framework}.framework/${_framework}")
endforeach()
list(APPEND _framework_binaries
    "${PREFIX}/Frameworks/KosmicKrisp.framework/KosmicKrisp")
set(_required
    ${_static_archives}
    ${_framework_binaries}
    "${PREFIX}/include/epoxy/egl.h"
    "${PREFIX}/include/virgl/virglrenderer.h"
    "${PREFIX}/include/virgl/virgl_hw.h"
    )
foreach(_framework IN LISTS _gl_frameworks)
    list(APPEND _required
        "${PREFIX}/Frameworks/${_framework}.framework/Info.plist")
endforeach()
list(APPEND _required
    "${PREFIX}/include/vulkan/vulkan.h"
    "${PREFIX}/Frameworks/KosmicKrisp.framework/Info.plist"
    "${PREFIX}/Frameworks/KosmicKrisp.framework/Headers/KosmicKrisp.h"
    "${PREFIX}/Frameworks/KosmicKrisp.framework/Modules/module.modulemap")
uml_require_prefix_files(LABEL "Apple GPU" FILES ${_required})

foreach(_archive IN LISTS _static_archives)
    execute_process(COMMAND "${_tool_ar}" -t "${_archive}"
        RESULT_VARIABLE _archive_status OUTPUT_QUIET ERROR_QUIET)
    if(NOT _archive_status EQUAL 0)
        message(FATAL_ERROR "Invalid Apple GPU static archive: ${_archive}")
    endif()
    execute_process(COMMAND "${_tool_nm}" -u "${_archive}"
        RESULT_VARIABLE _archive_nm_status
        OUTPUT_VARIABLE _archive_undefined_symbols)
    get_filename_component(_archive_name "${_archive}" NAME)
    if(_archive_name STREQUAL "libvirglrenderer.a")
        if(NOT _archive_undefined_symbols MATCHES "_vkGetInstanceProcAddr")
            message(FATAL_ERROR
                "${_archive} does not directly reference vkGetInstanceProcAddr")
        endif()
        string(REGEX REPLACE
            "[^\n]*_vkGetInstanceProcAddr[^\n]*(\n|$)" ""
            _unexpected_provider_symbols "${_archive_undefined_symbols}")
    else()
        set(_unexpected_provider_symbols "${_archive_undefined_symbols}")
    endif()
    if(NOT _archive_nm_status EQUAL 0 OR
       _unexpected_provider_symbols MATCHES "_(CGL|EAGL|vk[A-Z])" OR
       _archive_undefined_symbols MATCHES
           "[ \t]_(dlopen|dlsym)([ \t\n]|$)")
        message(FATAL_ERROR
            "${_archive} contains a forbidden provider symbol")
    endif()
endforeach()

foreach(_binary IN LISTS _static_archives _framework_binaries)
    uml_verify_apple_binary(PATH "${_binary}"
        PLATFORM ${_mach_platform} MINOS ${_deployment_target}
        LIPO "${_tool_lipo}" OTOOL "${_tool_otool}")
endforeach()

foreach(_framework IN LISTS _gl_frameworks)
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
        # The OpenGL provider is a two-library Mesa stack rendering on the
        # packaged Vulkan driver, so those exact edges are expected.
        if(_framework STREQUAL "MesaEGL" AND _dependency STREQUAL
               "@rpath/MesaGallium.framework/MesaGallium")
            continue()
        endif()
        if(_framework STREQUAL "MesaGallium" AND _dependency STREQUAL
               "@rpath/KosmicKrisp.framework/KosmicKrisp")
            continue()
        endif()
        if(_dependency STREQUAL "/usr/lib/libexpat.1.dylib")
            continue()
        endif()
        message(FATAL_ERROR
            "Unexpected ${_framework}.framework dependency: ${_dependency}")
    endforeach()
    set(_forbidden_closure "OpenGL\\.framework|OpenGLES\\.framework|libvulkan")
    if(_closure MATCHES "${_forbidden_closure}")
        message(FATAL_ERROR
            "${_framework}.framework contains a forbidden GL/Vulkan dependency")
    endif()
    execute_process(COMMAND "${_tool_nm}" -u "${_binary}"
        RESULT_VARIABLE _nm_status OUTPUT_VARIABLE _undefined_symbols)
    if(_framework STREQUAL "MesaGallium")
        if(NOT _undefined_symbols MATCHES "_vkGetInstanceProcAddr")
            message(FATAL_ERROR
                "MesaGallium.framework does not reference vkGetInstanceProcAddr")
        endif()
        string(REGEX REPLACE
            "[^\n]*_vkGetInstanceProcAddr[^\n]*(\n|$)" ""
            _provider_symbols "${_undefined_symbols}")
    else()
        set(_provider_symbols "${_undefined_symbols}")
    endif()
    if(NOT _nm_status EQUAL 0 OR
       _provider_symbols MATCHES "_(CGL|EAGL|vk[A-Z])")
        message(FATAL_ERROR
            "${_framework}.framework contains a forbidden provider symbol")
    endif()
    if(_framework STREQUAL "MesaEGL")
        execute_process(COMMAND "${_tool_nm}" -gU "${_binary}"
            RESULT_VARIABLE _export_status OUTPUT_VARIABLE _exports)
        if(NOT _export_status EQUAL 0 OR
           NOT _exports MATCHES "[ 	]_eglGetPlatformDisplay([ 	
]|$)")
            message(FATAL_ERROR
                "MesaEGL.framework does not export the EGL platform bootstrap")
        endif()
    endif()
endforeach()

set(_framework KosmicKrisp)
set(_binary
    "${PREFIX}/Frameworks/KosmicKrisp.framework/KosmicKrisp")
execute_process(COMMAND "${_tool_otool}" -D "${_binary}"
    RESULT_VARIABLE _id_status OUTPUT_VARIABLE _install_name_output)
if(NOT _id_status EQUAL 0 OR NOT _install_name_output MATCHES
   "@rpath/KosmicKrisp\\.framework/KosmicKrisp")
    message(FATAL_ERROR
        "KosmicKrisp.framework has an invalid install name")
endif()
execute_process(COMMAND "${_tool_otool}" -l "${_binary}"
    RESULT_VARIABLE _load_status OUTPUT_VARIABLE _load_commands)
if(NOT _load_status EQUAL 0 OR _load_commands MATCHES "LC_SUB_CLIENT" OR
   _load_commands MATCHES "LC_RPATH")
    message(FATAL_ERROR
        "KosmicKrisp.framework has an invalid load command")
endif()
execute_process(COMMAND plutil -extract "${_minimum_os_key}" raw -o -
    "${PREFIX}/Frameworks/KosmicKrisp.framework/Info.plist"
    RESULT_VARIABLE _plist_status OUTPUT_VARIABLE _minimum_os
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _plist_status EQUAL 0 OR
   NOT _minimum_os STREQUAL "${_deployment_target}")
    message(FATAL_ERROR
        "KosmicKrisp.framework has an invalid deployment target")
endif()
execute_process(COMMAND "${_tool_otool}" -L "${_binary}"
    RESULT_VARIABLE _closure_status OUTPUT_VARIABLE _closure)
if(NOT _closure_status EQUAL 0)
    message(FATAL_ERROR
        "Could not inspect KosmicKrisp.framework closure")
endif()
string(REPLACE "\n" ";" _closure_lines "${_closure}")
foreach(_line IN LISTS _closure_lines)
    if(NOT _line MATCHES "^[ \t]+([^ \t]+)")
        continue()
    endif()
    set(_dependency "${CMAKE_MATCH_1}")
    if(_dependency STREQUAL
           "@rpath/KosmicKrisp.framework/KosmicKrisp" OR
       _dependency MATCHES
           "^/usr/lib/(libc\\+\\+\\.1|libSystem\\.B|libobjc\\.A|libz\\.1)\\.dylib$" OR
       _dependency MATCHES
           "^/System/Library/Frameworks/(Metal|QuartzCore)\\.framework/")
        continue()
    endif()
    message(FATAL_ERROR
        "Unexpected KosmicKrisp.framework dependency: ${_dependency}")
endforeach()
if(_closure MATCHES "libvulkan|Vulkan\\.framework")
    message(FATAL_ERROR
        "KosmicKrisp.framework depends on an ambient Vulkan loader")
endif()
execute_process(COMMAND "${_tool_nm}" -gU "${_binary}"
    RESULT_VARIABLE _export_status OUTPUT_VARIABLE _exports)
if(NOT _export_status EQUAL 0 OR
   NOT _exports MATCHES "[ \t]_vkGetInstanceProcAddr([ \t\n]|$)")
    message(FATAL_ERROR
        "KosmicKrisp.framework does not export vkGetInstanceProcAddr")
endif()
execute_process(COMMAND "${_tool_nm}" -u "${_binary}"
    RESULT_VARIABLE _undefined_status OUTPUT_VARIABLE _undefined_symbols)
if(NOT _undefined_status EQUAL 0 OR
   _undefined_symbols MATCHES "[ \t]_(dlopen|dlsym)([ \t\n]|$)")
    message(FATAL_ERROR
        "KosmicKrisp.framework contains a dynamic-loader fallback")
endif()
file(READ
    "${PREFIX}/Frameworks/KosmicKrisp.framework/Modules/module.modulemap"
    _module_map)
if(NOT _module_map MATCHES "framework module KosmicKrisp" OR
   NOT _module_map MATCHES "umbrella header \"KosmicKrisp.h\"")
    message(FATAL_ERROR
        "KosmicKrisp.framework has an invalid module map")
endif()

message(STATUS
    "Verified apple-gpu-vulkan-experiment/${PLATFORM} Apple GPU dependency "
    "prefix: ${PREFIX}")
