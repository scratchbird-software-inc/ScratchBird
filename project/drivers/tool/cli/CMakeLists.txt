# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

cmake_minimum_required(VERSION 3.20)

project(scratchbird_cli LANGUAGES CXX)

# Section 32 invariant: the CLI target set is a bounded public tooling surface.
# It does not imply universal runtime parity or a stable plugin ABI across
# engine, listener, parser-agent, and client layers.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(OpenSSL REQUIRED)

option(SB_BUILD_CLI_FDW "Build FDW-backed CLI tools (pg/mysql/firebird)" OFF)
get_filename_component(SB_DRIVERS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(SB_PROJECT_ROOT "${SB_DRIVERS_ROOT}/.." ABSOLUTE)
set(SB_PROJECT_SOURCE_DIR "${SB_PROJECT_ROOT}")
set(SB_ENGINE_SOURCE_DIR "${SB_PROJECT_ROOT}" CACHE PATH
    "Path to ScratchBird engine source tree (for FDW CLI linkage)")
set(SB_ENGINE_BUILD_DIR "${CMAKE_BINARY_DIR}" CACHE PATH
    "Path to ScratchBird engine build tree (for FDW CLI linkage)")

include("${SB_PROJECT_ROOT}/cmake/PublicBranding.cmake")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SB_NONCLUSTER_ENGINE_PROFILE "bootstrap")
    set(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE OFF)
    set(SB_NONCLUSTER_ENGINE_DEGRADED_PROFILE ON)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/scratchbird/core/platform")
    configure_file(
        "${SB_PROJECT_ROOT}/src/core/platform/noncluster_engine_profile.hpp.in"
        "${CMAKE_BINARY_DIR}/generated/scratchbird/core/platform/noncluster_engine_profile.hpp"
        @ONLY
    )
endif()

set(SB_CPP_DRIVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../driver/cpp" CACHE PATH
    "Path to the current-repo C++ driver source tree")
if (NOT TARGET scratchbird_client)
    add_subdirectory("${SB_CPP_DRIVER_DIR}" "${CMAKE_BINARY_DIR}/drivers/driver/cpp")
endif()
if (NOT TARGET sb_core_platform)
    add_subdirectory("${SB_PROJECT_ROOT}/src/core/platform"
                     "${CMAKE_BINARY_DIR}/core_platform_for_cli")
endif()
if (NOT TARGET sb_core_uuid)
    add_subdirectory("${SB_PROJECT_ROOT}/src/core/uuid"
                     "${CMAKE_BINARY_DIR}/core_uuid_for_cli")
endif()
if (NOT TARGET sb_core_datatypes)
    add_subdirectory("${SB_PROJECT_ROOT}/src/core/datatypes"
                     "${CMAKE_BINARY_DIR}/core_datatypes_for_cli")
endif()
if (NOT TARGET sb_core_resources)
    add_subdirectory("${SB_PROJECT_ROOT}/src/core/resources"
                     "${CMAKE_BINARY_DIR}/core_resources_for_cli")
endif()
if (NOT TARGET sb_core_index)
    add_subdirectory("${SB_PROJECT_ROOT}/src/core/index"
                     "${CMAKE_BINARY_DIR}/core_index_for_cli")
endif()
if (NOT TARGET sb_storage_database)
    add_subdirectory("${SB_PROJECT_ROOT}/src/storage/database"
                     "${CMAKE_BINARY_DIR}/storage_database_for_cli")
endif()

add_executable(sb_isql
    sb_isql.cpp
    cli_auth_bootstrap.cpp
    metadata_shaping.cpp
)
add_executable(sb_admin
    sb_admin.cpp
    cli_auth_bootstrap.cpp
)
add_executable(sb_backup sb_backup.cpp)
add_executable(sb_security
    sb_security.cpp
    cli_auth_bootstrap.cpp
)
add_executable(sb_verify sb_verify.cpp)
add_executable(sbdriver_conformance
    sbdriver_conformance.cpp
    conformance_assertions.cpp
    txn_exec_parity.cpp
    res_lifecycle_parity.cpp
)

set(cli_targets
    sb_isql
    sb_admin
    sb_backup
    sb_security
    sb_verify
    sbdriver_conformance
)

# The first-principal database bootstrap is a bounded embedded-engine lifecycle
# operation shared by SBsql and SBsec.  It intentionally has no dependency on a
# parser worker, listener, manager, or client connection path.
add_library(sb_cli_first_principal_bootstrap STATIC
    first_principal_bootstrap.cpp
    bootstrap_os_authority.cpp
    bootstrap_password_verifier.cpp
)
target_include_directories(sb_cli_first_principal_bootstrap
    PRIVATE
        "${SB_CPP_DRIVER_DIR}/include"
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(sb_cli_first_principal_bootstrap
    PUBLIC
        sb_storage_database
        OpenSSL::Crypto
)
if(WIN32)
    target_link_libraries(sb_cli_first_principal_bootstrap PUBLIC advapi32 netapi32)
endif()

if (SB_BUILD_CLI_FDW)
    add_executable(sb_pg_isql sb_pg_isql.cpp)
    add_executable(sb_my_isql sb_my_isql.cpp)
    add_executable(sb_fb_isql sb_fb_isql.cpp)
    list(APPEND cli_targets sb_pg_isql sb_my_isql sb_fb_isql)

    set(fdw_cli_targets sb_pg_isql sb_my_isql sb_fb_isql)
    set(fdw_basic_cli_targets sb_pg_isql sb_my_isql)
    set(SB_ENGINE_INCLUDE_DIR "${SB_ENGINE_SOURCE_DIR}/include")
    set(SB_ENGINE_LIB_DIR "${SB_ENGINE_BUILD_DIR}/src")
    set(SB_ENGINE_FDW_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_fdw.a")
    set(SB_ENGINE_SBLR_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_sblr.a")
    set(SB_ENGINE_PARSER_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_parser.a")
    set(SB_ENGINE_COMPILER_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_compiler.a")
    set(SB_ENGINE_CORE_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_core.a")
    set(SB_ENGINE_OPTIMIZER_RUNTIME_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_optimizer_runtime.a")
    set(SB_ENGINE_OPTIMIZER_COMPILER_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_optimizer_compiler.a")
    set(SB_ENGINE_SECURITY_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_security.a")
    set(SB_ENGINE_IPC_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_ipc.a")
    set(SB_ENGINE_UDR_LIB "${SB_ENGINE_LIB_DIR}/libscratchbird_udr.a")

    set(sb_engine_libs_ready TRUE)
    foreach(required_path IN LISTS
            SB_ENGINE_FDW_LIB
            SB_ENGINE_SBLR_LIB
            SB_ENGINE_PARSER_LIB
            SB_ENGINE_COMPILER_LIB
            SB_ENGINE_CORE_LIB
            SB_ENGINE_OPTIMIZER_RUNTIME_LIB
            SB_ENGINE_SECURITY_LIB
            SB_ENGINE_IPC_LIB
            SB_ENGINE_UDR_LIB)
        if (NOT EXISTS "${required_path}")
            message(WARNING "Missing required ScratchBird engine artifact: ${required_path}")
            set(sb_engine_libs_ready FALSE)
        endif()
    endforeach()

    if (NOT EXISTS "${SB_ENGINE_INCLUDE_DIR}")
        message(WARNING "Missing ScratchBird engine include directory: ${SB_ENGINE_INCLUDE_DIR}")
        set(sb_engine_libs_ready FALSE)
    endif()

    if (NOT sb_engine_libs_ready)
        message(FATAL_ERROR
            "SB_BUILD_CLI_FDW=ON requires ScratchBird engine headers and libs. "
            "Build ScratchBird first or override SB_ENGINE_SOURCE_DIR/SB_ENGINE_BUILD_DIR.")
    endif()

    set(SB_FB_LINK_ARCHIVES
        "${SB_ENGINE_COMPILER_LIB}"
        "${SB_ENGINE_PARSER_LIB}"
        "${SB_ENGINE_SBLR_LIB}"
        "${SB_ENGINE_CORE_LIB}"
        "${SB_ENGINE_OPTIMIZER_RUNTIME_LIB}"
        "${SB_ENGINE_SECURITY_LIB}"
        "${SB_ENGINE_IPC_LIB}"
        "${SB_ENGINE_UDR_LIB}"
    )
    if (EXISTS "${SB_ENGINE_OPTIMIZER_COMPILER_LIB}")
        list(APPEND SB_FB_LINK_ARCHIVES "${SB_ENGINE_OPTIMIZER_COMPILER_LIB}")
    endif()

    find_package(Threads REQUIRED)
    find_package(LibXml2 REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(GEOS QUIET)
    find_package(PROJ QUIET)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(LZ4 QUIET liblz4)
        pkg_check_modules(ZSTD QUIET libzstd)
    endif()
    find_library(CRYPT_LIBRARY crypt)
    find_library(GEOS_C_LIBRARY NAMES geos_c)

    foreach(target IN LISTS fdw_basic_cli_targets)
        target_include_directories(${target} BEFORE PRIVATE "${SB_ENGINE_INCLUDE_DIR}")
        target_link_libraries(${target}
            PRIVATE
                "${SB_ENGINE_FDW_LIB}"
                "${SB_ENGINE_SBLR_LIB}"
                "${SB_ENGINE_PARSER_LIB}"
                "${SB_ENGINE_CORE_LIB}"
                "${SB_ENGINE_UDR_LIB}"
                OpenSSL::SSL
                OpenSSL::Crypto
                Threads::Threads
        )
    endforeach()

    target_include_directories(sb_fb_isql BEFORE PRIVATE "${SB_ENGINE_INCLUDE_DIR}")
    if (CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED)
        string(JOIN "," SB_FB_LINK_ARCHIVES_CSV ${SB_FB_LINK_ARCHIVES})
        target_link_libraries(sb_fb_isql PRIVATE "$<LINK_GROUP:RESCAN,${SB_FB_LINK_ARCHIVES_CSV}>")
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(sb_fb_isql
            PRIVATE
                -Wl,--start-group
                ${SB_FB_LINK_ARCHIVES}
                -Wl,--end-group
        )
    else()
        target_link_libraries(sb_fb_isql PRIVATE ${SB_FB_LINK_ARCHIVES})
    endif()

    set(SB_FB_EXTERNAL_LIBS
        "${SB_ENGINE_FDW_LIB}"
        LibXml2::LibXml2
        ZLIB::ZLIB
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    if (GEOS_C_LIBRARY)
        list(APPEND SB_FB_EXTERNAL_LIBS "${GEOS_C_LIBRARY}")
    elseif (GEOS_FOUND)
        list(APPEND SB_FB_EXTERNAL_LIBS ${GEOS_LIBRARIES})
    else()
        list(APPEND SB_FB_EXTERNAL_LIBS geos_c)
    endif()
    if (PROJ_FOUND)
        list(APPEND SB_FB_EXTERNAL_LIBS ${PROJ_LIBRARIES})
    else()
        list(APPEND SB_FB_EXTERNAL_LIBS proj)
    endif()
    if (LZ4_FOUND)
        target_link_directories(sb_fb_isql PRIVATE ${LZ4_LIBRARY_DIRS})
        target_compile_options(sb_fb_isql PRIVATE ${LZ4_CFLAGS_OTHER})
        list(APPEND SB_FB_EXTERNAL_LIBS ${LZ4_LIBRARIES})
    else()
        list(APPEND SB_FB_EXTERNAL_LIBS lz4)
    endif()
    if (ZSTD_FOUND)
        target_link_directories(sb_fb_isql PRIVATE ${ZSTD_LIBRARY_DIRS})
        target_compile_options(sb_fb_isql PRIVATE ${ZSTD_CFLAGS_OTHER})
        list(APPEND SB_FB_EXTERNAL_LIBS ${ZSTD_LIBRARIES})
    else()
        list(APPEND SB_FB_EXTERNAL_LIBS zstd)
    endif()
    if (CRYPT_LIBRARY)
        list(APPEND SB_FB_EXTERNAL_LIBS "${CRYPT_LIBRARY}")
    endif()
    if (UNIX AND NOT APPLE)
        list(APPEND SB_FB_EXTERNAL_LIBS dl m)
    endif()
    target_link_libraries(sb_fb_isql PRIVATE ${SB_FB_EXTERNAL_LIBS})
endif()

foreach(target IN LISTS cli_targets)
    if (target STREQUAL "sbdriver_conformance")
        target_include_directories(${target}
            PRIVATE
                "${SB_CPP_DRIVER_DIR}/include"
        )
    else()
        target_include_directories(${target}
            PRIVATE
                "${SB_CPP_DRIVER_DIR}/include"
                ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
    endif()
    target_link_libraries(${target} PRIVATE scratchbird_client)
    if (target STREQUAL "sbdriver_conformance")
        target_link_libraries(${target} PRIVATE OpenSSL::Crypto)
    endif()
endforeach()

target_link_libraries(sb_isql PRIVATE sb_cli_first_principal_bootstrap)
target_link_libraries(sb_security PRIVATE OpenSSL::Crypto sb_cli_first_principal_bootstrap)

add_executable(sb_bootstrap_password_verifier_tests
    bootstrap_password_verifier_test.cpp
    bootstrap_password_verifier.cpp
)
target_link_libraries(sb_bootstrap_password_verifier_tests PRIVATE OpenSSL::Crypto)

sb_public_brand_target(sb_isql "SBsql" "ScratchBird SQL" "cli")
sb_public_brand_target(sb_admin "SBadm" "ScratchBird Administrator" "cli")
sb_public_brand_target(sb_backup "SBbak" "ScratchBird Backup Manager" "cli")
sb_public_brand_target(sb_security "SBsec" "ScratchBird Security" "cli")
sb_public_brand_target(sb_verify "SBdoc" "ScratchBird Doctor" "cli")
sb_public_brand_target(sbdriver_conformance "SBcop" "ScratchBird Conformance Officer" "cli")

if(SB_INSTALL_NON_ENGINE_COMPONENTS)
    install(TARGETS
        sb_isql
        sb_admin
        sb_backup
        sb_security
        sb_verify
        sbdriver_conformance
        RUNTIME DESTINATION bin
        COMPONENT non_engine_runtime
    )
    install(FILES package_contract.json
        DESTINATION share/scratchbird/cli
        COMPONENT non_engine_runtime
    )
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    sb_public_write_branding_manifest("${CMAKE_BINARY_DIR}/public_branding_manifest.tsv")
    add_test(
        NAME cli_public_branding_gate
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/cli_public_branding_smoke.py"
                "--manifest=${CMAKE_BINARY_DIR}/public_branding_manifest.tsv"
    )
endif()

if (TARGET sb_verify)
    target_sources(sb_verify PRIVATE crc32c.cpp)
endif()

add_executable(sbdriver_txn_exec_tests
    txn_exec_parity_test.cpp
    txn_exec_parity.cpp
)
target_include_directories(sbdriver_txn_exec_tests
    PRIVATE
        "${SB_CPP_DRIVER_DIR}/include"
)
target_link_libraries(sbdriver_txn_exec_tests PRIVATE scratchbird_client)

add_executable(sbdriver_metadata_shaping_tests
    metadata_shaping_test.cpp
    metadata_shaping.cpp
)

add_executable(sbdriver_res_lifecycle_tests
    res_lifecycle_parity_test.cpp
    res_lifecycle_parity.cpp
)
target_include_directories(sbdriver_res_lifecycle_tests
    PRIVATE
        "${SB_CPP_DRIVER_DIR}/include"
)
target_link_libraries(sbdriver_res_lifecycle_tests PRIVATE scratchbird_client)

add_executable(sbdriver_auth_bootstrap_tests
    cli_auth_bootstrap_test.cpp
    cli_auth_bootstrap.cpp
)
target_include_directories(sbdriver_auth_bootstrap_tests
    PRIVATE
        "${SB_CPP_DRIVER_DIR}/include"
)
target_link_libraries(sbdriver_auth_bootstrap_tests PRIVATE scratchbird_client)

add_executable(sb_bootstrap_os_authority_tests
    bootstrap_os_authority_test.cpp
    bootstrap_os_authority.cpp
)
target_include_directories(sb_bootstrap_os_authority_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
if(WIN32)
    target_link_libraries(sb_bootstrap_os_authority_tests PRIVATE advapi32 netapi32)
endif()

add_executable(sb_first_principal_bootstrap_args_tests
    first_principal_bootstrap_args_test.cpp
)
target_include_directories(sb_first_principal_bootstrap_args_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sb_first_principal_bootstrap_args_tests
    PRIVATE
        sb_cli_first_principal_bootstrap
)

add_executable(sbdriver_conformance_assertion_tests
    conformance_assertions_test.cpp
    conformance_assertions.cpp
)
target_include_directories(sbdriver_conformance_assertion_tests
    PRIVATE
        "${SB_CPP_DRIVER_DIR}/include"
)
target_link_libraries(sbdriver_conformance_assertion_tests PRIVATE scratchbird_client)

if (BUILD_TESTING)
    add_test(NAME cli_auth_bootstrap_tests COMMAND sbdriver_auth_bootstrap_tests)
    add_test(NAME cli_bootstrap_os_authority_tests COMMAND sb_bootstrap_os_authority_tests)
    add_test(NAME cli_bootstrap_password_verifier_tests COMMAND sb_bootstrap_password_verifier_tests)
    add_test(NAME cli_first_principal_bootstrap_args_tests COMMAND sb_first_principal_bootstrap_args_tests)
    add_test(
        NAME cli_first_principal_bootstrap_cli_smoke
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/first_principal_bootstrap_cli_smoke.py"
                --sb-isql $<TARGET_FILE:sb_isql>
                --sb-security $<TARGET_FILE:sb_security>
                --work-dir "${CMAKE_BINARY_DIR}/first_principal_bootstrap_cli_smoke"
    )
    set_tests_properties(
        cli_first_principal_bootstrap_args_tests
        cli_first_principal_bootstrap_cli_smoke
        PROPERTIES
            LABELS "public_release_correctness;cli_tool_gate;security_auth_gate;embedded_bootstrap_gate"
    )
    add_test(NAME cli_txn_exec_parity_tests COMMAND sbdriver_txn_exec_tests)
    add_test(NAME cli_metadata_shaping_tests COMMAND sbdriver_metadata_shaping_tests)
    add_test(NAME cli_res_lifecycle_tests COMMAND sbdriver_res_lifecycle_tests)
    add_test(NAME cli_conformance_assertion_tests COMMAND sbdriver_conformance_assertion_tests)
    add_test(
        NAME cli_package_contract_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/package_contract_smoke.py"
    )
endif()
