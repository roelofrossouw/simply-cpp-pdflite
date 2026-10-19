cmake_minimum_required(VERSION 3.22)
set(SC_MODULE pdflite)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-pdflite" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_PDFLITE_BUILD_TESTS "Build simply-cpp-pdflite tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc::distance (distance.h), written with its unit and cast between units since 1.16.0.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.16.0)
# sc::standard_font (the built-in fonts by face since 6.2.0) and paper sizes in millimetres.
find_or_install_package(sc-graphics simply-cpp-graphics-dev simply-cpp-graphics 6.2.0)
# Streams are compressed with zlib, the one library the writer uses.
find_package(ZLIB REQUIRED)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(pdflite
        SOURCES src/object.cpp src/file.cpp src/content.cpp src/document.cpp src/jpeg.cpp src/png.cpp
        LINK_LIBRARIES sc::sc-core sc::sc-graphics ZLIB::ZLIB
        PUBLIC_LINK_LIBRARIES sc::sc-core sc::sc-graphics ZLIB::ZLIB)

add_sc_libraries(DESCRIPTION "simply-cpp-pdflite")

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-pdflite
        COMPONENT development)

if (SC_PDFLITE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-pdflite-test)
endif ()

add_sc_example(sc-pdflite-demo)
add_sc_example(sc-pdflite-links)
add_sc_example(sc-pdflite-pages)

# The images demo also draws a picture with sc-image (OpenCV), so it's built when sc-image is
# installed and isn't installed itself: the runtime package stays free of OpenCV.
find_package(sc-image QUIET)
if (sc-image_FOUND AND SC_BUILD_EXAMPLES)
    add_executable(sc-pdflite-images examples/sc-pdflite-images.cpp)
    target_link_libraries(sc-pdflite-images PRIVATE sc::sc-pdflite-shared sc::sc-image-shared)
    target_compile_definitions(sc-pdflite-images PRIVATE SC_PDFLITE_EXAMPLE_PHOTO="${CMAKE_CURRENT_SOURCE_DIR}/examples/resource/cat.jpg")
    sc_program_info(sc-pdflite-images)
endif ()

package_sc_module(
        DEV_DEPENDS "simply-cpp-dev (>= 1.16.0~)" "simply-cpp-graphics-dev (>= 6.2.0~)" zlib1g-dev)
