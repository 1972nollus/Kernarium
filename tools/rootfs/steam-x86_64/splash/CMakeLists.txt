# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.27)
project(KernariumSplash LANGUAGES CXX)
# Built only inside a producer-prepared verified toolchain, with networking
# disabled. The renderer itself is architecture neutral; each Steam appliance
# builds it natively for its own guest.
if(NOT UML_STEAM_VERIFIED_SDK OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|x86_64|AMD64)$")
    message(FATAL_ERROR "Use a Steam rootfs producer and its verified toolchain")
endif()
add_executable(kernarium-screen screen.cpp)
target_compile_features(kernarium-screen PRIVATE cxx_std_20)
target_compile_options(kernarium-screen PRIVATE -Wall -Wextra -Werror)
target_include_directories(kernarium-screen PRIVATE /usr/include/cairo /usr/include/libdrm)
target_link_libraries(kernarium-screen PRIVATE cairo X11 drm gbm EGL GLESv2)
install(TARGETS kernarium-screen RUNTIME DESTINATION libexec)
