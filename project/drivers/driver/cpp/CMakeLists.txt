# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.20)
project(scratchbird_client VERSION 0.1.0 LANGUAGES C CXX)

add_library(scratchbird_client
    src/connection.cpp
    src/metadata.cpp
    src/circuit_breaker.cpp
    src/keepalive.cpp
    src/leak_detector.cpp
    src/telemetry.cpp
    src/statement_cache.cpp
    src/pool.cpp
    src/driver_config.cpp
    src/network_client.cpp
    src/protocol/sbwp_protocol.cpp
    src/scratchbird_client_c.cpp
    src/core/sqlstate.cpp
    src/core/type_extractor.cpp
    src/network/network.cpp
    src/network/socket.cpp
    src/network/socket_types.cpp
    src/security/tls_context.cpp
)

set_target_properties(scratchbird_client PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(scratchbird_client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(scratchbird_client PUBLIC cxx_std_17)

find_package(OpenSSL REQUIRED)
target_link_libraries(scratchbird_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)

add_executable(sb_isql_cpp
    tools/sb_isql_cpp.cpp
)
target_compile_features(sb_isql_cpp PRIVATE cxx_std_17)
target_link_libraries(sb_isql_cpp PRIVATE scratchbird_client OpenSSL::Crypto)

add_executable(sb_regress_cpp
    tools/sb_regress_cpp.cpp
)
target_compile_features(sb_regress_cpp PRIVATE cxx_std_17)
target_link_libraries(sb_regress_cpp PRIVATE scratchbird_client OpenSSL::Crypto)

if (TARGET sbl_sbsql_parser_worker_core)
    target_compile_definitions(scratchbird_client PRIVATE SCRATCHBIRD_CLIENT_ENABLE_LOCAL_SBSQL_BRIDGE)
    if (TARGET sb_server_core)
        target_compile_definitions(scratchbird_client PRIVATE SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
        if (TARGET sb_core_memory)
            target_link_libraries(scratchbird_client PRIVATE sb_core_memory)
        endif()
    endif()
    target_link_libraries(scratchbird_client PRIVATE sbl_sbsql_parser_worker_core)
endif()

if (WIN32)
    target_link_libraries(scratchbird_client PRIVATE ws2_32)
endif()

include(CTest)
if (BUILD_TESTING)
    target_compile_definitions(scratchbird_client PRIVATE SCRATCHBIRD_TEST_HOOKS)

    add_test(NAME sb_regress_cpp_help COMMAND sb_regress_cpp --help)
    set_tests_properties(sb_regress_cpp_help PROPERTIES
        LABELS "driver;cpp;full_surface;regression_runner"
    )

    add_executable(scratchbird_client_c_memory_ownership_test
        tests/test_c_api_memory_ownership.cpp
    )
    target_compile_features(scratchbird_client_c_memory_ownership_test PRIVATE cxx_std_17)
    target_compile_definitions(scratchbird_client_c_memory_ownership_test PRIVATE SCRATCHBIRD_TEST_HOOKS)
    target_link_libraries(scratchbird_client_c_memory_ownership_test
        PRIVATE
            scratchbird_client
    )
    add_test(NAME scratchbird_client_c_memory_ownership_test COMMAND scratchbird_client_c_memory_ownership_test)
    set_tests_properties(scratchbird_client_c_memory_ownership_test PROPERTIES
        LABELS "MMCH-016;driver;c_api;memory_ownership;mga_transaction_regression"
    )

    find_package(GTest)
    if (GTest_FOUND)
        add_executable(scratchbird_client_checkout_leak_detector_gate
            tests/test_checkout_leak_detector.cpp
        )
        target_link_libraries(scratchbird_client_checkout_leak_detector_gate
            PRIVATE
                scratchbird_client
                GTest::gtest
                GTest::gtest_main
        )
        add_test(
            NAME scratchbird_client_checkout_leak_detector_gate
            COMMAND scratchbird_client_checkout_leak_detector_gate
        )
        set_tests_properties(scratchbird_client_checkout_leak_detector_gate PROPERTIES
            LABELS "MMCH-090;driver;checkout_leak_detector;MMCH_DRIVER_CHECKOUT_LEAK_DETECTOR"
        )

        add_executable(scratchbird_client_tests
            tests/test_driver_connectivity.cpp
            tests/test_driver_defaults.cpp
            tests/test_metadata_schema_tree.cpp
            tests/test_type_mapping.cpp
            tests/test_paging_payload.cpp
        )
        target_link_libraries(scratchbird_client_tests
            PRIVATE
                scratchbird_client
                GTest::gtest
                GTest::gtest_main
        )
        add_test(NAME scratchbird_client_tests COMMAND scratchbird_client_tests)
    endif()
endif()
