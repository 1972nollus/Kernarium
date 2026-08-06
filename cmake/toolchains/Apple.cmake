# SPDX-License-Identifier: GPL-2.0-or-later
# Configure with -DUML_APPLE_PLATFORM=macos|ios|ios-simulator.

set(UML_APPLE_PLATFORM "macos" CACHE STRING "Apple target platform")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES UML_APPLE_PLATFORM)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Apple architectures" FORCE)

function(uml_apple_sdk_path sdk result)
    execute_process(
        COMMAND xcrun --sdk ${sdk} --show-sdk-path
        OUTPUT_VARIABLE sdk_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    set(${result} "${sdk_path}" PARENT_SCOPE)
endfunction()

if(UML_APPLE_PLATFORM STREQUAL "macos")
    uml_apple_sdk_path(macosx UML_APPLE_SDK_PATH)
    set(CMAKE_SYSTEM_NAME Darwin)
    set(CMAKE_OSX_SYSROOT "${UML_APPLE_SDK_PATH}" CACHE PATH "Apple SDK" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Deployment target" FORCE)
elseif(UML_APPLE_PLATFORM STREQUAL "ios")
    uml_apple_sdk_path(iphoneos UML_APPLE_SDK_PATH)
    set(CMAKE_SYSTEM_NAME iOS)
    set(CMAKE_OSX_SYSROOT "${UML_APPLE_SDK_PATH}" CACHE PATH "Apple SDK" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "Deployment target" FORCE)
elseif(UML_APPLE_PLATFORM STREQUAL "ios-simulator")
    uml_apple_sdk_path(iphonesimulator UML_APPLE_SDK_PATH)
    set(CMAKE_SYSTEM_NAME iOS)
    set(CMAKE_OSX_SYSROOT "${UML_APPLE_SDK_PATH}" CACHE PATH "Apple SDK" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "Deployment target" FORCE)
else()
    message(FATAL_ERROR "Unknown UML_APPLE_PLATFORM=${UML_APPLE_PLATFORM}")
endif()

# The macOS arm64 default target enables the optional mnemonics used by the
# existing assembly bodies. iOS defaults to a more conservative baseline, so
# expose the required option to the CPU implementation and apply it globally
# only to standalone assembly sources. Ordinary C and C++ targets retain the
# platform baseline. Runtime feature selection remains the sole authority for
# executing optional host operations.
if(NOT UML_APPLE_PLATFORM STREQUAL "macos")
    set(UML_APPLE_CPU_INLINE_ASM_COMPILE_OPTION
        "-march=armv8.2-a+fp16+dotprod+crypto+fcma")
    add_compile_options(
        $<$<COMPILE_LANGUAGE:ASM>:${UML_APPLE_CPU_INLINE_ASM_COMPILE_OPTION}>)
endif()
