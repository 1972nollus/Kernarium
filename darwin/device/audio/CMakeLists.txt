# SPDX-License-Identifier: GPL-2.0-or-later

add_library(uml_audio_device STATIC
    apple_playback_sink.mm
    in_process_service.cpp
    virtio_sound_device.cpp)
add_library(UML::AudioDevice ALIAS uml_audio_device)
uml_declare_architecture_target(uml_audio_device device)
target_include_directories(uml_audio_device
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
target_compile_features(uml_audio_device PUBLIC cxx_std_20)
set_target_properties(uml_audio_device PROPERTIES OBJCXX_STANDARD 20)
target_compile_options(uml_audio_device PRIVATE
    -Wall -Wextra -Werror -Wno-deprecated-declarations
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(uml_audio_device PRIVATE
    uml_gpu_transport
    "-framework AVFAudio"
    "-framework Foundation")
