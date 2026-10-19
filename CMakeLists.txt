cmake_minimum_required(VERSION 3.22)
set(SC_MODULE pdflite)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-pdflite" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_PDFLITE_BUILD_TESTS "Build simply-cpp-pdflite tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc::distance (distance.h), and points and sizes of distances with in<Unit>() since 1.17.0.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.17.0)
# sc::standard_font (the built-in fonts by face since 6.2.0) and paper sizes in millimetres.
find_or_install_package(sc-graphics simply-cpp-graphics-dev simply-cpp-graphics 6.2.0)
# Streams are compressed with zlib; embedded fonts are read by FreeType (as sc-graphics reads them).
find_package(ZLIB REQUIRED)
config_or_install_package(freetype2 libfreetype-dev freetype)
# Encryption (AES-256, SHA-2, random keys) by OpenSSL's libcrypto: cryptography isn't ours to write.
find_or_install_package(OpenSSL libssl-dev openssl)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(pdflite
        SOURCES src/object.cpp src/file.cpp src/content.cpp src/document.cpp src/jpeg.cpp src/png.cpp src/svg.cpp src/font.cpp src/encryption.cpp
        LINK_LIBRARIES sc::sc-core sc::sc-graphics ZLIB::ZLIB PkgConfig::freetype2 OpenSSL::Crypto
        PUBLIC_LINK_LIBRARIES sc::sc-core sc::sc-graphics ZLIB::ZLIB OpenSSL::Crypto)

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
add_sc_example(sc-pdflite-png)
add_sc_example(sc-pdflite-svg)
add_sc_example(sc-pdflite-fonts)
add_sc_example(sc-pdflite-passwords)
# The PNG and SVG demos' pictures are installed with them, and found in the source when they run
# from a build.
install(FILES examples/resource/badge.png examples/resource/badge-interlaced.png examples/resource/stamp.png
        examples/resource/gradient.png examples/resource/drawing.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/simply-cpp-pdflite COMPONENT runtime)
foreach (demo IN ITEMS sc-pdflite-png sc-pdflite-svg)
    if (TARGET ${demo})
        target_compile_definitions(${demo} PRIVATE
                SC_PDFLITE_EXAMPLE_INSTALLED="${CMAKE_INSTALL_FULL_DATADIR}/simply-cpp-pdflite"
                SC_PDFLITE_EXAMPLE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/examples/resource")
    endif ()
endforeach ()

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
        DEV_DEPENDS "simply-cpp-dev (>= 1.17.0~)" "simply-cpp-graphics-dev (>= 6.2.0~)" zlib1g-dev libfreetype-dev libssl-dev)
