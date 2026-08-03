cmake_minimum_required(VERSION 3.25)

project(uml_emulator_external_consumer LANGUAGES CXX)

if(NOT IS_ABSOLUTE "${UML_EMULATOR_LIBRARY}" OR
   NOT EXISTS "${UML_EMULATOR_LIBRARY}")
    message(FATAL_ERROR
        "UML_EMULATOR_LIBRARY must name an existing absolute archive")
endif()
if(NOT IS_ABSOLUTE "${UML_EMULATOR_HEADERS}" OR
   NOT EXISTS "${UML_EMULATOR_HEADERS}/uml/emulator.h")
    message(FATAL_ERROR
        "UML_EMULATOR_HEADERS must contain uml/emulator.h")
endif()

add_library(uml_emulator_external STATIC IMPORTED GLOBAL)
set_target_properties(uml_emulator_external PROPERTIES
    IMPORTED_LOCATION "${UML_EMULATOR_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${UML_EMULATOR_HEADERS}")

add_executable(uml-emulator-external-consumer main.cpp)
target_compile_features(uml-emulator-external-consumer PRIVATE cxx_std_20)
target_compile_options(uml-emulator-external-consumer PRIVATE
    -Wall -Wextra -Werror)
target_link_libraries(uml-emulator-external-consumer PRIVATE
    uml_emulator_external)
