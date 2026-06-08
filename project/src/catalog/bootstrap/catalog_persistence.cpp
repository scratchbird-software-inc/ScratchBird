// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_persistence.hpp"

#include <utility>
#include <vector>

namespace scratchbird::catalog::bootstrap {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CatalogPersistenceOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status CatalogPersistenceErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::catalog};
}

CatalogPersistenceImageResult CatalogPersistenceError(std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail = {}) {
  CatalogPersistenceImageResult result;
  result.status = CatalogPersistenceErrorStatus();
  result.diagnostic = MakeCatalogPersistenceDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

}  // namespace

CatalogPersistenceImageResult BuildCatalogPersistenceImage(const CatalogPersistenceSeedConfig& config) {
  const auto bootstrap = ValidateLocalCatalogBootstrapManifest(config.bootstrap_manifest);
  if (!bootstrap.ok()) {
    CatalogPersistenceImageResult result;
    result.status = bootstrap.status;
    result.diagnostic = bootstrap.diagnostic;
    return result;
  }
  if (config.objects.empty()) {
    return CatalogPersistenceError("SB-CATALOG-PERSISTENCE-NO-OBJECT-SEEDS",
                                   "catalog.persistence.no_object_seeds");
  }

  CatalogIdentityManifest identity_manifest;
  for (const CatalogPersistenceObjectSeed& seed : config.objects) {
    const auto object = MakeCatalogObjectIdentity(seed.catalog_row_uuid,
                                                  seed.object_uuid,
                                                  seed.object_kind,
                                                  seed.parent_object_uuid,
                                                  seed.engine_owned,
                                                  seed.cluster_shared);
    if (!object.ok()) {
      CatalogPersistenceImageResult result;
      result.status = object.status;
      result.diagnostic = object.diagnostic;
      return result;
    }
    identity_manifest.objects.push_back(object.object);

    const auto name = MakeLocalizedObjectName(seed.object_uuid,
                                              seed.language_tag,
                                              seed.localized_path,
                                              seed.localized_name,
                                              LocalizedNameClass::default_name,
                                              true);
    if (!name.ok()) {
      CatalogPersistenceImageResult result;
      result.status = name.status;
      result.diagnostic = name.diagnostic;
      return result;
    }
    identity_manifest.names.push_back(name.name);

    const auto comment = MakeLocalizedObjectComment(seed.object_uuid,
                                                    seed.language_tag,
                                                    std::string("ScratchBird bootstrap object: ") + seed.localized_path,
                                                    true);
    if (!comment.ok()) {
      CatalogPersistenceImageResult result;
      result.status = comment.status;
      result.diagnostic = comment.diagnostic;
      return result;
    }
    identity_manifest.comments.push_back(comment.comment);
  }

  const auto identity = ValidateCatalogIdentityManifest(identity_manifest);
  if (!identity.ok()) {
    CatalogPersistenceImageResult result;
    result.status = identity.status;
    result.diagnostic = identity.diagnostic;
    return result;
  }

  CatalogPersistenceImageResult result;
  result.status = CatalogPersistenceOkStatus();
  result.image.bootstrap_manifest = bootstrap.manifest;
  result.image.identity_manifest = identity.manifest;
  result.image.recursive_schema_tree_enabled = bootstrap.manifest.recursive_schema_tree_enabled;
  result.image.final_catalog_page_body_layout_available = false;
  return result;
}

CatalogPersistenceImageResult ValidateCatalogPersistenceImage(const CatalogPersistenceImage& image) {
  const auto bootstrap = ValidateLocalCatalogBootstrapManifest(image.bootstrap_manifest);
  if (!bootstrap.ok()) {
    CatalogPersistenceImageResult result;
    result.status = bootstrap.status;
    result.diagnostic = bootstrap.diagnostic;
    return result;
  }
  if (!image.recursive_schema_tree_enabled) {
    return CatalogPersistenceError("SB-CATALOG-PERSISTENCE-RECURSIVE-TREE-REQUIRED",
                                   "catalog.persistence.recursive_tree_required");
  }

  const auto identity = ValidateCatalogIdentityManifest(image.identity_manifest);
  if (!identity.ok()) {
    CatalogPersistenceImageResult result;
    result.status = identity.status;
    result.diagnostic = identity.diagnostic;
    return result;
  }

  CatalogPersistenceImageResult result;
  result.status = CatalogPersistenceOkStatus();
  result.image = image;
  return result;
}

DiagnosticRecord MakeCatalogPersistenceDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "catalog.bootstrap.persistence");
}

}  // namespace scratchbird::catalog::bootstrap
