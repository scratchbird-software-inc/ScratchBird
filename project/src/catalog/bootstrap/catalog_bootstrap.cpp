// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_bootstrap.hpp"

#include <utility>
#include <vector>

namespace scratchbird::catalog::bootstrap {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CatalogBootstrapOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status CatalogBootstrapErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::catalog};
}

bool IsEngineIdentityTypedUuid(const TypedUuid& uuid, UuidKind expected_kind) {
  return uuid.kind == expected_kind && uuid.valid() && scratchbird::core::uuid::IsEngineIdentityUuid(uuid.value);
}

bool SameTypedUuidValue(const TypedUuid& left, const TypedUuid& right) {
  return left.value == right.value;
}

SchemaRootBootstrapResult MakeValidatedSchema(BootstrapSchemaRootKind kind,
                                              TypedUuid schema_uuid,
                                              std::string canonical_path,
                                              std::string default_name,
                                              bool engine_owned,
                                              bool user_mutable) {
  SchemaRootBootstrap schema_root;
  schema_root.kind = kind;
  schema_root.schema_uuid = schema_uuid;
  schema_root.canonical_path = std::move(canonical_path);
  schema_root.default_language = "en";
  schema_root.default_name = std::move(default_name);
  schema_root.engine_owned = engine_owned;
  schema_root.user_mutable = user_mutable;
  return ValidateSchemaRootBootstrap(schema_root);
}

}  // namespace

const char* BootstrapSchemaRootKindName(BootstrapSchemaRootKind kind) {
  switch (kind) {
    case BootstrapSchemaRootKind::sys_catalog: return "sys_catalog";
    case BootstrapSchemaRootKind::sys_metrics: return "sys_metrics";
    case BootstrapSchemaRootKind::local_user: return "local_user";
    case BootstrapSchemaRootKind::unknown: return "unknown";
  }
  return "unknown";
}

DatabaseBootstrapIdentityResult MakeDatabaseBootstrapIdentity(TypedUuid database_uuid,
                                                              u64 creation_unix_epoch_millis) {
  DatabaseBootstrapIdentity identity;
  identity.database_uuid = database_uuid;
  identity.creation_unix_epoch_millis = creation_unix_epoch_millis;
  return ValidateDatabaseBootstrapIdentity(identity);
}

DatabaseBootstrapIdentityResult ValidateDatabaseBootstrapIdentity(const DatabaseBootstrapIdentity& identity) {
  DatabaseBootstrapIdentityResult result;
  result.status = CatalogBootstrapOkStatus();
  result.identity = identity;

  if (!IsEngineIdentityTypedUuid(identity.database_uuid, UuidKind::database)) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-DATABASE-UUID-MUST-BE-V7",
                                                       "catalog.bootstrap.database_uuid_must_be_v7");
    return result;
  }

  if (identity.creation_unix_epoch_millis == 0) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-MISSING-CREATION-TIME",
                                                       "catalog.bootstrap.missing_creation_time");
    return result;
  }

  return result;
}

SchemaRootBootstrapResult MakeSchemaRootBootstrap(BootstrapSchemaRootKind kind,
                                                  TypedUuid schema_uuid,
                                                  std::string canonical_path,
                                                  std::string default_language,
                                                  std::string default_name) {
  SchemaRootBootstrap schema_root;
  schema_root.kind = kind;
  schema_root.schema_uuid = schema_uuid;
  schema_root.canonical_path = std::move(canonical_path);
  schema_root.default_language = std::move(default_language);
  schema_root.default_name = std::move(default_name);
  schema_root.engine_owned = kind == BootstrapSchemaRootKind::sys_catalog ||
                             kind == BootstrapSchemaRootKind::sys_metrics;
  schema_root.user_mutable = kind == BootstrapSchemaRootKind::local_user;
  return ValidateSchemaRootBootstrap(schema_root);
}

SchemaRootBootstrapResult ValidateSchemaRootBootstrap(const SchemaRootBootstrap& schema_root) {
  SchemaRootBootstrapResult result;
  result.status = CatalogBootstrapOkStatus();
  result.schema_root = schema_root;

  if (schema_root.kind == BootstrapSchemaRootKind::unknown) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-UNKNOWN-SCHEMA-ROOT",
                                                       "catalog.bootstrap.unknown_schema_root");
    return result;
  }

  if (!IsEngineIdentityTypedUuid(schema_root.schema_uuid, UuidKind::schema)) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-SCHEMA-UUID-MUST-BE-V7",
                                                       "catalog.bootstrap.schema_uuid_must_be_v7",
                                                       BootstrapSchemaRootKindName(schema_root.kind));
    return result;
  }

  if (schema_root.canonical_path.empty() || schema_root.default_language.empty() ||
      schema_root.default_name.empty()) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-SCHEMA-NAME-INCOMPLETE",
                                                       "catalog.bootstrap.schema_name_incomplete",
                                                       BootstrapSchemaRootKindName(schema_root.kind));
    return result;
  }

  if ((schema_root.kind == BootstrapSchemaRootKind::sys_catalog ||
       schema_root.kind == BootstrapSchemaRootKind::sys_metrics) &&
      (!schema_root.engine_owned || schema_root.user_mutable)) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-SYSTEM-SCHEMA-AUTHORITY-VIOLATION",
                                                       "catalog.bootstrap.system_schema_authority_violation",
                                                       BootstrapSchemaRootKindName(schema_root.kind));
    return result;
  }

  if (schema_root.kind == BootstrapSchemaRootKind::local_user &&
      (schema_root.engine_owned || !schema_root.user_mutable)) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-LOCAL-USER-SCHEMA-AUTHORITY-VIOLATION",
                                                       "catalog.bootstrap.local_user_schema_authority_violation");
    return result;
  }

  return result;
}

LocalCatalogBootstrapManifestResult MakeLocalCatalogBootstrapManifest(DatabaseBootstrapIdentity database,
                                                                      TypedUuid sys_catalog_schema_uuid,
                                                                      TypedUuid sys_metrics_schema_uuid,
                                                                      TypedUuid local_user_schema_uuid) {
  LocalCatalogBootstrapManifest manifest;
  manifest.database = database;
  manifest.recursive_schema_tree_enabled = true;

  manifest.schema_roots.push_back(MakeValidatedSchema(BootstrapSchemaRootKind::sys_catalog,
                                                      sys_catalog_schema_uuid,
                                                      "sys.catalog",
                                                      "sys.catalog",
                                                      true,
                                                      false)
                                      .schema_root);
  manifest.schema_roots.push_back(MakeValidatedSchema(BootstrapSchemaRootKind::sys_metrics,
                                                      sys_metrics_schema_uuid,
                                                      "sys.metrics",
                                                      "sys.metrics",
                                                      true,
                                                      false)
                                      .schema_root);
  manifest.schema_roots.push_back(MakeValidatedSchema(BootstrapSchemaRootKind::local_user,
                                                      local_user_schema_uuid,
                                                      "local.user",
                                                      "local.user",
                                                      false,
                                                      true)
                                      .schema_root);

  return ValidateLocalCatalogBootstrapManifest(manifest);
}

LocalCatalogBootstrapManifestResult ValidateLocalCatalogBootstrapManifest(
    const LocalCatalogBootstrapManifest& manifest) {
  LocalCatalogBootstrapManifestResult result;
  result.status = CatalogBootstrapOkStatus();
  result.manifest = manifest;

  DatabaseBootstrapIdentityResult database_result = ValidateDatabaseBootstrapIdentity(manifest.database);
  if (!database_result.ok()) {
    result.status = database_result.status;
    result.diagnostic = database_result.diagnostic;
    return result;
  }

  if (!manifest.recursive_schema_tree_enabled) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-RECURSIVE-SCHEMA-TREE-REQUIRED",
                                                       "catalog.bootstrap.recursive_schema_tree_required");
    return result;
  }

  bool has_sys_catalog = false;
  bool has_sys_metrics = false;
  bool has_local_user = false;

  for (const SchemaRootBootstrap& schema_root : manifest.schema_roots) {
    SchemaRootBootstrapResult schema_result = ValidateSchemaRootBootstrap(schema_root);
    if (!schema_result.ok()) {
      result.status = schema_result.status;
      result.diagnostic = schema_result.diagnostic;
      return result;
    }

    has_sys_catalog = has_sys_catalog || schema_root.kind == BootstrapSchemaRootKind::sys_catalog;
    has_sys_metrics = has_sys_metrics || schema_root.kind == BootstrapSchemaRootKind::sys_metrics;
    has_local_user = has_local_user || schema_root.kind == BootstrapSchemaRootKind::local_user;

    for (const SchemaRootBootstrap& other : manifest.schema_roots) {
      if (&schema_root != &other && SameTypedUuidValue(schema_root.schema_uuid, other.schema_uuid)) {
        result.status = CatalogBootstrapErrorStatus();
        result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                           "SB-CATALOG-BOOTSTRAP-DUPLICATE-SCHEMA-UUID",
                                                           "catalog.bootstrap.duplicate_schema_uuid",
                                                           schema_root.canonical_path);
        return result;
      }
    }
  }

  if (!has_sys_catalog || !has_sys_metrics || !has_local_user) {
    result.status = CatalogBootstrapErrorStatus();
    result.diagnostic = MakeCatalogBootstrapDiagnostic(result.status,
                                                       "SB-CATALOG-BOOTSTRAP-MISSING-REQUIRED-SCHEMA-ROOT",
                                                       "catalog.bootstrap.missing_required_schema_root");
    return result;
  }

  return result;
}

DiagnosticRecord MakeCatalogBootstrapDiagnostic(Status status,
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
                        "catalog.bootstrap");
}

}  // namespace scratchbird::catalog::bootstrap
