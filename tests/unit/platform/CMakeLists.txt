# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_guest_file_sync_tests guest_file_sync_tests.cpp)
target_link_libraries(uml_guest_file_sync_tests PRIVATE UML::GuestFiles)
uml_add_unit_test(NAME platform-guest-file-sync
    COMMAND uml_guest_file_sync_tests)

add_executable(uml_video_decoder_tests video_decoder_tests.cpp)
target_link_libraries(uml_video_decoder_tests PRIVATE UML::VideoDecoder)
uml_add_unit_test(NAME platform-video-decoder
    COMMAND uml_video_decoder_tests ${PROJECT_SOURCE_DIR}/tests/support/video
    LABELS video TIMEOUT 120)

# The platform's run routing; the transaction tests use its test variant.
add_executable(uml_run_routing_tests
    run_routing_tests.cpp
    $<TARGET_OBJECTS:uml_run_routing_test_variant>)
target_link_libraries(uml_run_routing_tests PRIVATE
    uml_test_allocation_failure)
target_include_directories(uml_run_routing_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
target_compile_definitions(uml_run_routing_tests PRIVATE
    UML_RUN_ROUTING_TESTING=1)
uml_add_unit_test(
    NAME runtime-route-transaction
    COMMAND uml_run_routing_tests
    LABELS tsan)

add_executable(uml_run_routing_stress_tests
    run_routing_stress_tests.cpp
    $<TARGET_OBJECTS:uml_run_routing>)
target_include_directories(uml_run_routing_stress_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform)
uml_add_unit_test(
    NAME runtime-route-unbound-reads
    COMMAND uml_run_routing_stress_tests)
