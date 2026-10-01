cmake_minimum_required(VERSION 3.20)
project(OpenSCADAnnotationTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Test Network)
find_package(libzip CONFIG REQUIRED)
enable_testing()
add_executable(annotation-store-tests StoreTest.cc
  ../../src/gui/annotations/AnnotationStore.cc
  ../../src/gui/annotations/AnnotationStore.h)
target_sources(annotation-store-tests PRIVATE ../../src/gui/annotations/AnnotationDrawing.cc)
target_link_libraries(annotation-store-tests PRIVATE Qt6::Widgets Qt6::Test libzip::zip)
add_test(NAME annotation-store COMMAND annotation-store-tests)
add_executable(annotation-transport-tests TransportTest.cc
  ../../src/gui/annotations/AnnotationTransport.cc
  ../../src/gui/annotations/AnnotationTransport.h
  ../../src/gui/annotations/AnnotationStore.cc
  ../../src/gui/annotations/AnnotationStore.h)
target_link_libraries(annotation-transport-tests PRIVATE Qt6::Widgets Qt6::Test Qt6::Network libzip::zip)
add_test(NAME annotation-transport COMMAND annotation-transport-tests)
