# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.10)
project(scratchbird_odbc VERSION 0.1.0 LANGUAGES C CXX)

# Platform detection
if(WIN32)
    set(SB_PLATFORM_WINDOWS TRUE)
elseif(APPLE)
    set(SB_PLATFORM_MACOS TRUE)
elseif(UNIX)
    set(SB_PLATFORM_LINUX TRUE)
endif()

# Source files
set(ODBC_SOURCES
    src/odbc_client_bridge.cpp
    src/odbc_driver.cpp
    src/odbc_handles.cpp
    src/keepalive.cpp
    src/leak_detector.cpp
    src/circuit_breaker.cpp
    src/statement_cache.cpp
)

# Platform-specific sources
if(WIN32)
    list(APPEND ODBC_SOURCES src/platform_windows.c)
else()
    list(APPEND ODBC_SOURCES src/platform_unix.c)
endif()

add_library(scratchbird_odbc SHARED ${ODBC_SOURCES})

add_executable(sb_isql_odbc
    tools/sb_isql_odbc.cpp
)

find_package(OpenSSL REQUIRED)

# C++ standard
set_target_properties(scratchbird_odbc PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

if(WIN32)
    set_target_properties(scratchbird_odbc PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

include(CTest)
option(ODBC_FETCH_GTEST "Fetch GoogleTest from source when not found locally." OFF)

# Include directories
target_include_directories(scratchbird_odbc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../cpp/include>
        $<INSTALL_INTERFACE:include>
)

target_include_directories(sb_isql_odbc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/../cpp/include
        ${CMAKE_CURRENT_LIST_DIR}/../cpp/tools
)
target_compile_features(sb_isql_odbc PRIVATE cxx_std_17)
target_link_libraries(sb_isql_odbc PRIVATE scratchbird_odbc OpenSSL::Crypto)

# Platform-specific includes and libraries
if(WIN32)
    # Windows - ODBC is part of the SDK
    target_compile_definitions(scratchbird_odbc PRIVATE 
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        SB_PLATFORM_WINDOWS=1
    )
    target_link_libraries(scratchbird_odbc PRIVATE 
        odbc32
        odbccp32
        ws2_32
    )
    target_link_libraries(sb_isql_odbc PRIVATE odbc32 odbccp32 ws2_32)
else()
    # Unix - Find ODBC driver manager
    set(ODBC_INCLUDE_DIR "")
    set(ODBC_LIBRARY "")
    
    # Try to find unixODBC or iODBC
    find_path(ODBC_INCLUDE_DIR 
        NAMES sql.h sqlext.h
        PATHS
            /usr/include
            /usr/local/include
            /opt/homebrew/include
            /usr/include/odbc
            /usr/local/include/odbc
            /opt/odbc/include
            /usr/include/iodbc
            /usr/local/include/iodbc
        PATH_SUFFIXES odbc iodbc
    )
    
    find_library(ODBC_LIBRARY
        NAMES odbc iodbc unixodbc
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/homebrew/lib
            /usr/lib/x86_64-linux-gnu
            /usr/lib64
            /opt/odbc/lib
        PATH_SUFFIXES odbc
    )
    
    if(ODBC_INCLUDE_DIR AND ODBC_LIBRARY)
        message(STATUS "Found ODBC: ${ODBC_LIBRARY}")
        message(STATUS "ODBC include: ${ODBC_INCLUDE_DIR}")
        target_include_directories(scratchbird_odbc PRIVATE ${ODBC_INCLUDE_DIR})
        target_link_libraries(scratchbird_odbc PRIVATE ${ODBC_LIBRARY})
        target_include_directories(sb_isql_odbc PRIVATE ${ODBC_INCLUDE_DIR})
        target_link_libraries(sb_isql_odbc PRIVATE ${ODBC_LIBRARY})
    else()
        message(WARNING "ODBC not found. Building with generic ODBC headers.")
        # Assume headers are in standard location
    endif()
    
    # Platform-specific definitions
    if(APPLE)
        target_compile_definitions(scratchbird_odbc PRIVATE 
            SB_PLATFORM_MACOS=1
            SB_PLATFORM_UNIX=1
        )
        # macOS frameworks
        find_library(COREFoundation_LIBRARY CoreFoundation)
        if(COREFoundation_LIBRARY)
            target_link_libraries(scratchbird_odbc PRIVATE ${COREFoundation_LIBRARY})
        endif()
    else()
        target_compile_definitions(scratchbird_odbc PRIVATE 
            SB_PLATFORM_LINUX=1
            SB_PLATFORM_UNIX=1
        )
    endif()
    
    # pthread for threading
    find_package(Threads REQUIRED)
    target_link_libraries(scratchbird_odbc PRIVATE Threads::Threads)
    target_link_libraries(sb_isql_odbc PRIVATE Threads::Threads)
    
    # dl for dynamic loading
    target_link_libraries(scratchbird_odbc PRIVATE ${CMAKE_DL_LIBS})
    target_link_libraries(sb_isql_odbc PRIVATE ${CMAKE_DL_LIBS})
endif()

# Link with scratchbird C++ client if available
if(TARGET scratchbird_client)
    target_link_libraries(scratchbird_odbc PRIVATE scratchbird_client)
else()
    # Try to find or build it
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../cpp)
        add_subdirectory(
            ${CMAKE_CURRENT_LIST_DIR}/../cpp
            ${CMAKE_CURRENT_BINARY_DIR}/scratchbird_cpp
        )
        target_link_libraries(scratchbird_odbc PRIVATE scratchbird_client)
    endif()
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS scratchbird_odbc
    EXPORT scratchbird_odbc-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Export targets
install(EXPORT scratchbird_odbc-targets
    FILE scratchbird_odbc-targets.cmake
    NAMESPACE scratchbird::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scratchbird_odbc
)

# Create package config file
include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scratchbird_odbc-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/scratchbird_odbc-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scratchbird_odbc
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/scratchbird_odbc-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/scratchbird_odbc-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/scratchbird_odbc-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scratchbird_odbc
)

if (BUILD_TESTING)
    find_package(GTest QUIET)

    if (NOT GTest_FOUND AND ODBC_FETCH_GTEST)
        message(STATUS "GoogleTest not found. Fetching GoogleTest from source.")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_GetProperties(googletest)
        if (NOT googletest_POPULATED)
            FetchContent_Populate(googletest)
            add_subdirectory(${googletest_SOURCE_DIR} ${googletest_BINARY_DIR})
        endif()
        find_package(GTest CONFIG QUIET)
    endif()

    set(_sb_odbc_gtest FALSE)
    set(_sb_odbc_gtest_target "")
    set(_sb_odbc_gtest_main_target "")
    if (GTest_FOUND)
        if (TARGET GTest::gtest AND TARGET GTest::gtest_main)
            set(_sb_odbc_gtest TRUE)
            set(_sb_odbc_gtest_target GTest::gtest)
            set(_sb_odbc_gtest_main_target GTest::gtest_main)
        endif()
    endif()
    if (NOT _sb_odbc_gtest AND TARGET gtest AND TARGET gtest_main)
        set(_sb_odbc_gtest TRUE)
        set(_sb_odbc_gtest_target gtest)
        set(_sb_odbc_gtest_main_target gtest_main)
    endif()

    if (_sb_odbc_gtest)
        add_executable(scratchbird_odbc_tests
            tests/test_odbc_cursor_operations.cpp
            tests/test_odbc_bulk_operations.cpp
            tests/test_odbc_capabilities_browse.cpp
            tests/test_odbc_catalog_and_types.cpp
            tests/test_odbc_capabilities_descriptors.cpp
            tests/test_odbc_lob_streaming.cpp
            tests/test_odbc_unicode_compat.cpp
            tests/test_odbc_statement_cache.cpp
            tests/test_odbc_external_runtime.cpp
        )

        if(WIN32)
            target_sources(scratchbird_odbc_tests PRIVATE ${ODBC_SOURCES})
            target_compile_definitions(scratchbird_odbc_tests PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN
                SB_PLATFORM_WINDOWS=1
                SCRATCHBIRD_ODBC_EXPORTS
            )
            target_include_directories(scratchbird_odbc_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_LIST_DIR}/../cpp/include
            )
            target_link_libraries(scratchbird_odbc_tests
                PRIVATE
                    scratchbird_client
                    ${_sb_odbc_gtest_target}
                    ${_sb_odbc_gtest_main_target}
                    ws2_32
            )
        else()
            target_link_libraries(scratchbird_odbc_tests
                PRIVATE
                    scratchbird_odbc
                    scratchbird_client
                    ${_sb_odbc_gtest_target}
                    ${_sb_odbc_gtest_main_target}
            )
        endif()

        add_test(NAME scratchbird_odbc_tests COMMAND scratchbird_odbc_tests)
    else()
        message(STATUS "Skipping ODBC tests: GoogleTest not found")
    endif()
endif()

# Print summary
message(STATUS "")
message(STATUS "ScratchBird ODBC Driver Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Windows: ${SB_PLATFORM_WINDOWS}")
message(STATUS "  Linux: ${SB_PLATFORM_LINUX}")
message(STATUS "  macOS: ${SB_PLATFORM_MACOS}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
