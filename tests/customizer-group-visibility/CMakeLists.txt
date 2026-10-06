cmake_minimum_required(VERSION 3.21)
project(CustomizerGroupVisibilityTest LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(customizer-group-visibility-test
  group_visibility_test.cc
  ../../src/gui/parameter/GroupWidget.cc
  ../../src/gui/parameter/GroupWidget.h)
target_include_directories(customizer-group-visibility-test PRIVATE ../../src)
target_link_libraries(customizer-group-visibility-test PRIVATE Qt6::Widgets)
enable_testing()
add_test(NAME customizer-group-visibility COMMAND customizer-group-visibility-test)
set_tests_properties(customizer-group-visibility PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
