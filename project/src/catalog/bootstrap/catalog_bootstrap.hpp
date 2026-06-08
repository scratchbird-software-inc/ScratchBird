// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::catalog::bootstrap {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

enum class BootstrapSchemaRootKind : u16 {
  sys_catalog,
  sys_metrics,
  local_user,
  unknown
};

struct DatabaseBootstrapIdentity {
  TypedUuid database_uuid;
  u64 creation_unix_epoch_millis = 0;

  constexpr bool valid() const {
    return database_uuid.kind == UuidKind::database && database_uuid.valid();
  }
};

struct SchemaRootBootstrap {
  BootstrapSchemaRootKind kind = BootstrapSchemaRootKind::unknown;
  TypedUuid schema_uuid;
  std::string canonical_path;
  std::string default_language;
  std::string default_name;
  bool engine_owned = false;
  bool user_mutable = false;
};

struct LocalCatalogBootstrapManifest {
  DatabaseBootstrapIdentity database;
  std::vector<SchemaRootBootstrap> schema_roots;
  bool recursive_schema_tree_enabled = true;
};

struct DatabaseBootstrapIdentityResult {
  Status status;
  DatabaseBootstrapIdentity identity;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct SchemaRootBootstrapResult {
  Status status;
  SchemaRootBootstrap schema_root;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct LocalCatalogBootstrapManifestResult {
  Status status;
  LocalCatalogBootstrapManifest manifest;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* BootstrapSchemaRootKindName(BootstrapSchemaRootKind kind);
DatabaseBootstrapIdentityResult MakeDatabaseBootstrapIdentity(TypedUuid database_uuid,
                                                              u64 creation_unix_epoch_millis);
DatabaseBootstrapIdentityResult ValidateDatabaseBootstrapIdentity(const DatabaseBootstrapIdentity& identity);
SchemaRootBootstrapResult MakeSchemaRootBootstrap(BootstrapSchemaRootKind kind,
                                                  TypedUuid schema_uuid,
                                                  std::string canonical_path,
                                                  std::string default_language,
                                                  std::string default_name);
SchemaRootBootstrapResult ValidateSchemaRootBootstrap(const SchemaRootBootstrap& schema_root);
LocalCatalogBootstrapManifestResult MakeLocalCatalogBootstrapManifest(DatabaseBootstrapIdentity database,
                                                                      TypedUuid sys_catalog_schema_uuid,
                                                                      TypedUuid sys_metrics_schema_uuid,
                                                                      TypedUuid local_user_schema_uuid);
LocalCatalogBootstrapManifestResult ValidateLocalCatalogBootstrapManifest(
    const LocalCatalogBootstrapManifest& manifest);
DiagnosticRecord MakeCatalogBootstrapDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

}  // namespace scratchbird::catalog::bootstrap
