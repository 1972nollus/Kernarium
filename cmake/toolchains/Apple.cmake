# SPDX-License-Identifier: GPL-2.0-or-later
# Configure with -DUML_APPLE_PLATFORM=macos|ios|ios-simulator.

set(UML_APPLE_PLATFORM "macos" CACHE STRING "Apple target platform")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES UML_APPLE_PLATFORM)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Apple architectures" FORCE)

# Each platform's SDK and CMake system name. Every Apple product builds at one
# deployment target, which its dependency prefixes record as well.
set(_uml_apple_deployment_target 26.0)
if(UML_APPLE_PLATFORM STREQUAL "macos")
    set(_uml_apple_platform macosx Darwin)
elseif(UML_APPLE_PLATFORM STREQUAL "ios")
    set(_uml_apple_platform iphoneos iOS)
elseif(UML_APPLE_PLATFORM STREQUAL "ios-simulator")
    set(_uml_apple_platform iphonesimulator iOS)
else()
    message(FATAL_ERROR "Unknown UML_APPLE_PLATFORM=${UML_APPLE_PLATFORM}")
endif()
list(POP_FRONT _uml_apple_platform _uml_apple_sdk CMAKE_SYSTEM_NAME)
execute_process(
    COMMAND xcrun --sdk ${_uml_apple_sdk} --show-sdk-path
    OUTPUT_VARIABLE _uml_apple_sdk_path
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
set(CMAKE_OSX_SYSROOT "${_uml_apple_sdk_path}" CACHE PATH "Apple SDK" FORCE)
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR
        CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET ${_uml_apple_deployment_target}
        CACHE STRING "Deployment target")
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
