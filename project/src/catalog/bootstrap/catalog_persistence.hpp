// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CATALOG-PERSISTENCE-ANCHOR
#include "catalog_bootstrap.hpp"
#include "catalog_identity.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::catalog::bootstrap {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;

struct CatalogPersistenceObjectSeed {
  TypedUuid catalog_row_uuid;
  TypedUuid object_uuid;
  CatalogObjectKind object_kind = CatalogObjectKind::unknown;
  TypedUuid parent_object_uuid;
  std::string language_tag = "en";
  std::string localized_path;
  std::string localized_name;
  bool engine_owned = false;
  bool cluster_shared = false;
};

struct CatalogPersistenceSeedConfig {
  LocalCatalogBootstrapManifest bootstrap_manifest;
  std::vector<CatalogPersistenceObjectSeed> objects;
};

struct CatalogPersistenceImage {
  LocalCatalogBootstrapManifest bootstrap_manifest;
  CatalogIdentityManifest identity_manifest;
  bool recursive_schema_tree_enabled = true;
  bool final_catalog_page_body_layout_available = false;
};

struct CatalogPersistenceImageResult {
  Status status;
  CatalogPersistenceImage image;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

CatalogPersistenceImageResult BuildCatalogPersistenceImage(const CatalogPersistenceSeedConfig& config);
CatalogPersistenceImageResult ValidateCatalogPersistenceImage(const CatalogPersistenceImage& image);
DiagnosticRecord MakeCatalogPersistenceDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::catalog::bootstrap
