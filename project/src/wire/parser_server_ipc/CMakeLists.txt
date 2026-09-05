# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_wire_bulk_import_stream_codec STATIC
  sbps_bulk_import_stream_codec.cpp
)
target_compile_features(sb_wire_bulk_import_stream_codec PUBLIC cxx_std_23)
target_include_directories(sb_wire_bulk_import_stream_codec PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(sb_wire_bulk_import_stream_codec PUBLIC
  sb_core_hash
)

add_library(sb_wire_statement_management_bind_codec STATIC
  sbps_statement_management_bind_codec.cpp
)
target_compile_features(sb_wire_statement_management_bind_codec PUBLIC cxx_std_23)
target_include_directories(sb_wire_statement_management_bind_codec PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(sb_wire_statement_management_bind_codec PUBLIC
  sb_core_hash
)

add_library(sbl_parser_server_ipc_schema STATIC
  parser_ipc_common.cpp
  parser_server_ipc.cpp
  sbps_execution_request_payload_codec.cpp
  sbps_narrow_statement_context_alias_codec.cpp
  typed_result_payload_codec.cpp
)

add_library(sbl_parser_server_ipc_client STATIC
  parser_server_client.cpp
)

if(NOT TARGET sb_core_datatypes)
  add_subdirectory(
    ${PROJECT_SOURCE_DIR}/src/core/datatypes
    ${CMAKE_CURRENT_BINARY_DIR}/core_datatypes)
endif()

if(NOT TARGET sb_wire_typed_result_transport_codec)
  add_library(sb_wire_typed_result_transport_codec STATIC
    ${PROJECT_SOURCE_DIR}/src/wire/typed_result_transport_carrier.cpp
    ${PROJECT_SOURCE_DIR}/src/wire/typed_result_transport_codec.cpp
  )
  target_compile_features(sb_wire_typed_result_transport_codec PUBLIC cxx_std_20)
  target_include_directories(sb_wire_typed_result_transport_codec PUBLIC
    ${PROJECT_SOURCE_DIR}/src/wire
  )
  target_link_libraries(sb_wire_typed_result_transport_codec PUBLIC
    sb_core_platform
    sb_core_datatypes
    sb_core_hash
  )
endif()

target_compile_features(sbl_parser_server_ipc_schema PUBLIC cxx_std_23)
target_include_directories(sbl_parser_server_ipc_schema PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sbl_parser_server_ipc_schema PUBLIC
  sb_wire_bulk_import_stream_codec
  sb_wire_statement_management_bind_codec
  sb_wire_typed_result_transport_codec
)

target_compile_features(sbl_parser_server_ipc_client PUBLIC cxx_std_23)
target_include_directories(sbl_parser_server_ipc_client PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(sbl_parser_server_ipc_client PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(sbl_parser_server_ipc_client
  PUBLIC
    sbl_parser_server_ipc_schema
  PRIVATE
    sb_core_datatypes
)
if(WIN32)
  target_link_libraries(sbl_parser_server_ipc_client PUBLIC ws2_32)
endif()
