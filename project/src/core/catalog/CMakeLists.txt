# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sb_core_catalog
  cluster_catalog_access_profile.cpp
  cluster_catalog_crypto_evidence.cpp
  cluster_catalog_manifest.cpp
  cluster_catalog_record_codec.cpp
  cluster_catalog_schema_versioning.cpp
  cluster_descriptor_manifest.cpp
  cluster_schema_gating.cpp
  catalog_index_profile.cpp
  ipar_catalog_support_services.cpp
  catalog_record_codec.cpp
  catalog_records.cpp
)

target_compile_features(sb_core_catalog PUBLIC cxx_std_17)
target_include_directories(sb_core_catalog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_catalog PUBLIC sb_core_platform sb_core_uuid sb_core_datatypes sb_storage_page)
