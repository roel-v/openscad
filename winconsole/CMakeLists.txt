cmake_minimum_required(VERSION 3.5...3.20)

project(winconsole VERSION 1.0 LANGUAGES C)

set(CMAKE_EXECUTABLE_SUFFIX_C .com)

add_executable(winconsole winconsole.c)

get_target_property(application_name OpenSCADExe OUTPUT_NAME)
set_target_properties(winconsole PROPERTIES
  OUTPUT_NAME "${application_name}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_definitions(winconsole PRIVATE EXE_NAME="$<TARGET_FILE_NAME:OpenSCADExe>")

install(TARGETS winconsole RUNTIME DESTINATION ".")
