# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.15)
project(libCCI VERSION 0.1.0 LANGUAGES C
        DESCRIPTION "Reference reader/writer for the CCI compressed Xbox disc format")

option(CCI_SYSTEM_LZ4    "Link the system liblz4 instead of the bundled submodule" OFF)
option(CCI_BUILD_EXAMPLES "Build the example CLI tools" ON)
option(CCI_BUILD_TESTS    "Build the test suite" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(cci
    src/cci_reader.c
    src/cci_volume.c
    src/cci_file.c
    src/cci_writer.c)

target_include_directories(cci PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# --- LZ4 (decoder + HC encoder) ---
if(CCI_SYSTEM_LZ4)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LZ4 IMPORTED_TARGET liblz4)
    endif()
    if(TARGET PkgConfig::LZ4)
        target_link_libraries(cci PUBLIC PkgConfig::LZ4)
    else()
        find_library(LZ4_LIBRARY NAMES lz4 liblz4 REQUIRED)
        find_path(LZ4_INCLUDE_DIR NAMES lz4.h REQUIRED)
        target_include_directories(cci PRIVATE ${LZ4_INCLUDE_DIR})
        target_link_libraries(cci PUBLIC ${LZ4_LIBRARY})
    endif()
else()
    set(LZ4_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4/lib)
    if(NOT EXISTS ${LZ4_DIR}/lz4.c)
        message(FATAL_ERROR
            "Bundled LZ4 not found. Initialize the submodule:\n"
            "  git submodule update --init --recursive\n"
            "or configure with -DCCI_SYSTEM_LZ4=ON to use the system liblz4.")
    endif()
    target_sources(cci PRIVATE ${LZ4_DIR}/lz4.c ${LZ4_DIR}/lz4hc.c)
    target_include_directories(cci PRIVATE ${LZ4_DIR})
endif()

set_target_properties(cci PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(CCI_BUILD_EXAMPLES)
    foreach(ex cci_info cci_extract cci_encode)
        add_executable(${ex} examples/${ex}.c)
        target_link_libraries(${ex} PRIVATE cci)
    endforeach()
endif()

if(CCI_BUILD_TESTS)
    enable_testing()
    add_executable(cci_test_roundtrip tests/test_roundtrip.c)
    target_link_libraries(cci_test_roundtrip PRIVATE cci)
    add_test(NAME roundtrip COMMAND cci_test_roundtrip)
endif()

include(GNUInstallDirs)
install(TARGETS cci
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
