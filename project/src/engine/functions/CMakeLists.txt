# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_engine_functions STATIC
  common/function_runtime.cpp
  common/function_gate.cpp
  common/function_result_helpers.cpp
  dispatch/function_dispatch.cpp
  families/bit/bit_function_landing_zone.cpp
  families/collection/collection_function_landing_zone.cpp
  families/conditional/conditional_function_landing_zone.cpp
  families/conversion/conversion_function_landing_zone.cpp
  families/cursor_stream_functions.cpp
  families/crypto_hash/crypto_hash_function_landing_zone.cpp
  families/data_aggregate_functions.cpp
  families/data_scalar_functions.cpp
  families/diagnostics/diagnostics_function_landing_zone.cpp
  families/extension_functions.cpp
  families/inactive_family_dispatch.cpp
  families/json_document/json_document_function_landing_zone.cpp
  families/lob_locator_functions.cpp
  families/management_functions.cpp
  families/metrics_functions.cpp
  families/native_surface_functions.cpp
  families/nosql_document_functions.cpp
  families/nosql_graph_functions.cpp
  families/nosql_kv_functions.cpp
  families/numeric/numeric_function_landing_zone.cpp
  families/range/range_function_landing_zone.cpp
  families/rowset_table_functions.cpp
  families/regex/regex_function_landing_zone.cpp
  families/schema_ddl_functions.cpp
  families/search_functions.cpp
  families/security_functions.cpp
  families/spatial_functions.cpp
  families/string/string_function_landing_zone.cpp
  families/system/system_function_landing_zone.cpp
  families/temporal/temporal_function_landing_zone.cpp
  families/timeseries_functions.cpp
  families/uuid/uuid_function_landing_zone.cpp
  families/vector_functions.cpp
  generated/function_completion_manifest.cpp
  metadata/compatibility_catalog_projection_policy.cpp
  metadata/compatibility_function_surface_policy.cpp
  metadata/function_hardening.cpp
  metadata/function_optimizer_metadata.cpp
  metadata/function_parser_projection.cpp
  registry/function_registry.cpp
  registry/function_seed_registry.cpp
)

find_package(ICU REQUIRED COMPONENTS uc i18n)
find_package(OpenSSL REQUIRED)

if(NOT TARGET sb_core_uuid)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core/uuid ${CMAKE_CURRENT_BINARY_DIR}/core_uuid)
endif()

target_compile_features(sb_engine_functions PUBLIC cxx_std_20)

target_include_directories(sb_engine_functions PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${ICU_INCLUDE_DIRS}
)

target_link_libraries(sb_engine_functions PUBLIC
  sb_engine_sblr
  sb_core_uuid
  OpenSSL::Crypto
)

if(TARGET ICU::uc)
  target_link_libraries(sb_engine_functions PUBLIC ICU::uc ICU::i18n)
else()
  target_link_libraries(sb_engine_functions PUBLIC ${ICU_LIBRARIES})
endif()
