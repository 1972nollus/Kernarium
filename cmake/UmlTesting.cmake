# SPDX-License-Identifier: GPL-2.0-or-later

include_guard(GLOBAL)

function(uml_record_registered_test NAME SUITE)
    string(TOUPPER "${SUITE}" _suite_upper)
    set(_target_property "UML_${_suite_upper}_COMMAND_TARGETS")
    get_property(_command_targets GLOBAL PROPERTY "${_target_property}")
    foreach(_argument IN LISTS ARGN)
        set(_command_target "")
        if(_argument MATCHES "^\\$<TARGET_FILE:([^>]+)>$")
            set(_command_target "${CMAKE_MATCH_1}")
        elseif(_argument MATCHES
               "^\\$<JOIN:\\$<TARGET_OBJECTS:([^>]+)>,\\|>$")
            set(_command_target "${CMAKE_MATCH_1}")
        elseif(_argument MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
            set(_command_target "${CMAKE_MATCH_1}")
        elseif(TARGET "${_argument}")
            set(_command_target "${_argument}")
        endif()
        if(NOT _command_target OR NOT TARGET "${_command_target}")
            continue()
        endif()
        get_property(_aliased_target TARGET "${_command_target}"
            PROPERTY ALIASED_TARGET)
        if(_aliased_target)
            set(_command_target "${_aliased_target}")
        endif()
        get_target_property(_command_target_imported "${_command_target}"
            IMPORTED)
        get_target_property(_command_target_type "${_command_target}" TYPE)
        if(NOT _command_target_imported AND
           NOT _command_target_type STREQUAL "INTERFACE_LIBRARY")
            list(APPEND _command_targets "${_command_target}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _command_targets)
    set_property(GLOBAL PROPERTY "${_target_property}" "${_command_targets}")
endfunction()

function(uml_finalize_suite_binary_target SUITE AGGREGATE_TARGET)
    if(NOT TARGET "${AGGREGATE_TARGET}")
        add_custom_target("${AGGREGATE_TARGET}")
    endif()
    string(TOUPPER "${SUITE}" _suite_upper)
    get_property(_command_targets GLOBAL PROPERTY
        "UML_${_suite_upper}_COMMAND_TARGETS")
    list(REMOVE_DUPLICATES _command_targets)
    foreach(_command_target IN LISTS _command_targets)
        if(NOT TARGET "${_command_target}")
            message(FATAL_ERROR
                "${SUITE} test command target disappeared: ${_command_target}")
        endif()
        add_dependencies("${AGGREGATE_TARGET}" "${_command_target}")
    endforeach()

    get_property(_aggregate_dependencies TARGET "${AGGREGATE_TARGET}"
        PROPERTY MANUALLY_ADDED_DEPENDENCIES)
    foreach(_command_target IN LISTS _command_targets)
        list(FIND _aggregate_dependencies "${_command_target}"
            _dependency_index)
        if(_dependency_index EQUAL -1)
            message(FATAL_ERROR
                "${AGGREGATE_TARGET} does not build registered ${SUITE} "
                "command target ${_command_target}")
        endif()
    endforeach()
endfunction()

function(uml_apply_test_properties NAME)
    cmake_parse_arguments(TEST "" "WORKING_DIRECTORY;TIMEOUT;PROCESSORS;RESOURCE_LOCK"
        "LABELS;RESOURCE_GROUPS;FIXTURES_SETUP;FIXTURES_CLEANUP;FIXTURES_REQUIRED" ${ARGN})

    if(NOT TEST_LABELS)
        message(FATAL_ERROR "${NAME}: a suite label is required")
    endif()
    set_tests_properties("${NAME}" PROPERTIES LABELS "${TEST_LABELS}")
    if(TEST_WORKING_DIRECTORY)
        set_tests_properties("${NAME}" PROPERTIES
            WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}")
    endif()
    if(TEST_TIMEOUT)
        set_tests_properties("${NAME}" PROPERTIES TIMEOUT "${TEST_TIMEOUT}")
    endif()
    if(TEST_PROCESSORS)
        set_tests_properties("${NAME}" PROPERTIES PROCESSORS "${TEST_PROCESSORS}")
    endif()
    if(TEST_RESOURCE_LOCK)
        set_tests_properties("${NAME}" PROPERTIES RESOURCE_LOCK "${TEST_RESOURCE_LOCK}")
    endif()
    if(TEST_RESOURCE_GROUPS)
        set_tests_properties("${NAME}" PROPERTIES RESOURCE_GROUPS "${TEST_RESOURCE_GROUPS}")
    endif()
    if(TEST_FIXTURES_SETUP)
        set_tests_properties("${NAME}" PROPERTIES FIXTURES_SETUP "${TEST_FIXTURES_SETUP}")
    endif()
    if(TEST_FIXTURES_CLEANUP)
        set_tests_properties("${NAME}" PROPERTIES FIXTURES_CLEANUP "${TEST_FIXTURES_CLEANUP}")
    endif()
    if(TEST_FIXTURES_REQUIRED)
        set_tests_properties("${NAME}" PROPERTIES FIXTURES_REQUIRED "${TEST_FIXTURES_REQUIRED}")
    endif()
endfunction()

function(uml_add_unit_test)
    cmake_parse_arguments(TEST "" "NAME;WORKING_DIRECTORY;TIMEOUT;PROCESSORS;RESOURCE_LOCK"
        "COMMAND;LABELS;RESOURCE_GROUPS;FIXTURES_SETUP;FIXTURES_CLEANUP;FIXTURES_REQUIRED" ${ARGN})
    if(NOT TEST_NAME OR NOT TEST_COMMAND)
        message(FATAL_ERROR
            "uml_add_unit_test requires NAME and COMMAND")
    endif()

    # Unit tests are native host binaries (or CMake-only setup actions).  Keep
    # Python and shell runners in the tooling/integration suites even when a
    # caller attempts to hide one behind an absolute interpreter path.
    foreach(_command_argument IN LISTS TEST_COMMAND)
        if(_command_argument MATCHES "(^|/)(python[0-9.]*|sh|bash|zsh)$" OR
           _command_argument MATCHES "\\.(py|sh)$")
            message(FATAL_ERROR
                "${TEST_NAME}: unit tests may not invoke Python or shell: "
                "${_command_argument}")
        endif()
    endforeach()

    add_test(NAME "${TEST_NAME}" COMMAND ${TEST_COMMAND})
    set(_labels "unit")
    if(TEST_LABELS)
        list(APPEND _labels ${TEST_LABELS})
    endif()
    uml_apply_test_properties("${TEST_NAME}"
        LABELS "${_labels}"
        WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
        TIMEOUT "${TEST_TIMEOUT}"
        PROCESSORS "${TEST_PROCESSORS}"
        RESOURCE_LOCK "${TEST_RESOURCE_LOCK}"
        RESOURCE_GROUPS "${TEST_RESOURCE_GROUPS}"
        FIXTURES_SETUP "${TEST_FIXTURES_SETUP}"
        FIXTURES_CLEANUP "${TEST_FIXTURES_CLEANUP}"
        FIXTURES_REQUIRED "${TEST_FIXTURES_REQUIRED}")
    # Scratch files stay in a per-test build-tree directory; see
    # tests/unit/scratch/scratch_directory.hpp. UML_UNIT_TEST_NAME gives each
    # test its registered name.
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY ENVIRONMENT
        "UML_UNIT_TEST_NAME=${TEST_NAME}"
        "UML_TEST_SCRATCH_DIRECTORY=${CMAKE_BINARY_DIR}/test-scratch/${TEST_NAME}")
    uml_record_registered_test("${TEST_NAME}" unit ${TEST_COMMAND})
endfunction()

function(uml_add_tooling_test)
    cmake_parse_arguments(TEST "" "NAME;WORKING_DIRECTORY;TIMEOUT;PROCESSORS;RESOURCE_LOCK"
        "COMMAND;LABELS;RESOURCE_GROUPS;FIXTURES_SETUP;FIXTURES_CLEANUP;FIXTURES_REQUIRED" ${ARGN})
    if(NOT TEST_NAME OR NOT TEST_COMMAND)
        message(FATAL_ERROR
            "uml_add_tooling_test requires NAME and COMMAND")
    endif()

    add_test(NAME "${TEST_NAME}" COMMAND ${TEST_COMMAND})
    set(_labels "tooling")
    if(TEST_LABELS)
        list(APPEND _labels ${TEST_LABELS})
    endif()
    uml_apply_test_properties("${TEST_NAME}"
        LABELS "${_labels}"
        WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
        TIMEOUT "${TEST_TIMEOUT}"
        PROCESSORS "${TEST_PROCESSORS}"
        RESOURCE_LOCK "${TEST_RESOURCE_LOCK}"
        RESOURCE_GROUPS "${TEST_RESOURCE_GROUPS}"
        FIXTURES_SETUP "${TEST_FIXTURES_SETUP}"
        FIXTURES_CLEANUP "${TEST_FIXTURES_CLEANUP}"
        FIXTURES_REQUIRED "${TEST_FIXTURES_REQUIRED}")
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    uml_record_registered_test("${TEST_NAME}" tooling ${TEST_COMMAND})
endfunction()

function(uml_add_integration_test)
    cmake_parse_arguments(TEST "ROOTFS" "NAME;WORKING_DIRECTORY;TIMEOUT;PROCESSORS;RESOURCE_LOCK"
        "COMMAND;LABELS;RESOURCE_GROUPS;FIXTURES_SETUP;FIXTURES_CLEANUP;FIXTURES_REQUIRED" ${ARGN})
    if(NOT TEST_NAME OR NOT TEST_COMMAND)
        message(FATAL_ERROR
            "uml_add_integration_test requires NAME and COMMAND")
    endif()

    set(_command ${TEST_COMMAND})
    set(_labels "integration")
    if(TEST_ROOTFS)
        if(NOT UML_INTEGRATION_ROOTFS)
            message(FATAL_ERROR
                "${TEST_NAME}: ROOTFS integration tests require UML_INTEGRATION_ROOTFS")
        endif()
        list(APPEND _labels rootfs)
    endif()
    add_test(NAME "${TEST_NAME}" COMMAND ${_command})
    if(TEST_LABELS)
        list(APPEND _labels ${TEST_LABELS})
    endif()
    uml_apply_test_properties("${TEST_NAME}"
        LABELS "${_labels}"
        WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
        TIMEOUT "${TEST_TIMEOUT}"
        PROCESSORS "${TEST_PROCESSORS}"
        RESOURCE_LOCK "${TEST_RESOURCE_LOCK}"
        RESOURCE_GROUPS "${TEST_RESOURCE_GROUPS}"
        FIXTURES_SETUP "${TEST_FIXTURES_SETUP}"
        FIXTURES_CLEANUP "${TEST_FIXTURES_CLEANUP}"
        FIXTURES_REQUIRED "${TEST_FIXTURES_REQUIRED}")
    # Integration runners import helpers from neighboring source directories.
    # Keep validation from recreating source-adjacent __pycache__ debris.
    set_property(TEST "${TEST_NAME}" APPEND PROPERTY
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    uml_record_registered_test("${TEST_NAME}" integration ${_command})
endfunction()

function(uml_add_guest_integration_test)
    uml_add_integration_test(ROOTFS ${ARGN})
endfunction()
