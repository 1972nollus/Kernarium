cmake_minimum_required(VERSION 3.25)

project(uml_emulator_external_consumer LANGUAGES C CXX OBJC)

set(UML_EMULATOR_PACKAGE_PREFIX "" CACHE PATH
    "Installed UMLEmulator package prefix")
set(UML_EMULATOR_LIBRARY "" CACHE FILEPATH
    "Raw UMLEmulator archive used only for final-slice verification")
set(UML_EMULATOR_HEADERS "" CACHE PATH
    "Raw UMLEmulator include root used only for final-slice verification")
set(UML_EXPECTED_GUEST_PAGE_SIZE "16384" CACHE STRING
    "Expected installed package guest page size")

if(UML_EMULATOR_PACKAGE_PREFIX)
    if(UML_EMULATOR_LIBRARY OR UML_EMULATOR_HEADERS)
        message(FATAL_ERROR
            "Select either the installed package or a raw final slice")
    endif()
    if(NOT IS_ABSOLUTE "${UML_EMULATOR_PACKAGE_PREFIX}")
        message(FATAL_ERROR
            "UML_EMULATOR_PACKAGE_PREFIX must be absolute")
    endif()
    find_package(UMLEmulator CONFIG REQUIRED
        PATHS "${UML_EMULATOR_PACKAGE_PREFIX}/lib/cmake/UMLEmulator"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)
    if(NOT "${UMLEmulator_APPLE_PLATFORM}" STREQUAL
       "${UML_APPLE_PLATFORM}")
        message(FATAL_ERROR
            "Installed package platform ${UMLEmulator_APPLE_PLATFORM} does not match ${UML_APPLE_PLATFORM}")
    endif()
    if(NOT "${UMLEmulator_GUEST_PAGE_SIZE}" STREQUAL
       "${UML_EXPECTED_GUEST_PAGE_SIZE}")
        message(FATAL_ERROR
            "Installed package page size ${UMLEmulator_GUEST_PAGE_SIZE} does not match ${UML_EXPECTED_GUEST_PAGE_SIZE}")
    endif()
    if(UMLEmulator_PROFILING_ENABLED)
        message(FATAL_ERROR "Installed release package contains profiling")
    endif()
    set(_uml_emulator_consumer_target UML::Emulator)
else()
    if(NOT IS_ABSOLUTE "${UML_EMULATOR_LIBRARY}" OR
       NOT EXISTS "${UML_EMULATOR_LIBRARY}")
        message(FATAL_ERROR
            "UML_EMULATOR_LIBRARY must name an existing absolute archive")
    endif()
    if(NOT IS_ABSOLUTE "${UML_EMULATOR_HEADERS}" OR
       NOT EXISTS "${UML_EMULATOR_HEADERS}/uml/emulator.h" OR
       NOT EXISTS "${UML_EMULATOR_HEADERS}/uml/abi_identity.h")
        message(FATAL_ERROR
            "UML_EMULATOR_HEADERS must contain both public headers")
    endif()
    add_library(uml_emulator_external STATIC IMPORTED GLOBAL)
    set_target_properties(uml_emulator_external PROPERTIES
        IMPORTED_LOCATION "${UML_EMULATOR_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${UML_EMULATOR_HEADERS}"
        INTERFACE_LINK_LIBRARIES "$<$<LINK_LANGUAGE:C,OBJC>:c++>")
    set(_uml_emulator_consumer_target uml_emulator_external)
endif()

add_executable(uml-emulator-external-consumer main.cpp)
target_compile_features(uml-emulator-external-consumer PRIVATE cxx_std_20)
target_compile_options(uml-emulator-external-consumer PRIVATE
    -Wall -Wextra -Werror)
target_compile_definitions(uml-emulator-external-consumer PRIVATE
    UML_EXPECTED_PLATFORM="${UML_APPLE_PLATFORM}")
target_link_libraries(uml-emulator-external-consumer PRIVATE
    ${_uml_emulator_consumer_target})

add_executable(uml-emulator-external-c-consumer main.c)
target_compile_features(uml-emulator-external-c-consumer PRIVATE c_std_11)
target_compile_options(uml-emulator-external-c-consumer PRIVATE
    -Wall -Wextra -Werror)
target_compile_definitions(uml-emulator-external-c-consumer PRIVATE
    UML_EXPECTED_PLATFORM="${UML_APPLE_PLATFORM}")
target_link_libraries(uml-emulator-external-c-consumer PRIVATE
    ${_uml_emulator_consumer_target})

add_executable(uml-emulator-external-objc-consumer main.m)
set_target_properties(uml-emulator-external-objc-consumer PROPERTIES
    OBJC_STANDARD 11
    OBJC_STANDARD_REQUIRED YES
    OBJC_EXTENSIONS NO)
target_compile_options(uml-emulator-external-objc-consumer PRIVATE
    -Wall -Wextra -Werror)
target_compile_definitions(uml-emulator-external-objc-consumer PRIVATE
    UML_EXPECTED_PLATFORM="${UML_APPLE_PLATFORM}")
target_link_libraries(uml-emulator-external-objc-consumer PRIVATE
    ${_uml_emulator_consumer_target})
