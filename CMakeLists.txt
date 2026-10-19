cmake_minimum_required(VERSION 3.22)
set(SC_MODULE pdflite)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-pdflite" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_PDFLITE_BUILD_TESTS "Build simply-cpp-pdflite tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc::distance (distance.h) since 1.15.0.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.15.0)
# sc::standard_font: the built-in fonts' encoding (and, for callers, their metrics).
find_or_install_package(sc-graphics simply-cpp-graphics-dev simply-cpp-graphics 6.1.1)
# Streams are compressed with zlib, the one library the writer uses.
find_package(ZLIB REQUIRED)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(pdflite
        SOURCES src/object.cpp src/file.cpp src/content.cpp src/document.cpp
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

package_sc_module(
        DEV_DEPENDS "simply-cpp-dev (>= 1.15.0~)" "simply-cpp-graphics-dev (>= 6.1.1~)" zlib1g-dev)
