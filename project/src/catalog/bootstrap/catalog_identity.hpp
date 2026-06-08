// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "catalog_bootstrap.hpp"
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

enum class CatalogObjectKind : u16 {
  database,
  schema,
  table,
  view,
  domain,
  procedure,
  function,
  sequence,
  index,
  package,
  synonym,
  constraint,
  policy,
  role,
  user,
  metrics_surface,
  unknown
};

enum class LocalizedNameClass : u16 {
  default_name,
  alias,
  compatibility_name,
  system_path,
  unknown
};

struct CatalogObjectIdentity {
  TypedUuid catalog_row_uuid;
  TypedUuid object_uuid;
  CatalogObjectKind object_kind = CatalogObjectKind::unknown;
  TypedUuid parent_object_uuid;
  bool engine_owned = false;
  bool cluster_shared = false;
};

struct LocalizedObjectName {
  TypedUuid object_uuid;
  std::string language_tag;
  std::string localized_path;
  std::string localized_name;
  LocalizedNameClass name_class = LocalizedNameClass::unknown;
  bool default_for_language = false;
};

struct LocalizedObjectComment {
  TypedUuid object_uuid;
  std::string language_tag;
  std::string comment_text;
  bool default_for_language = false;
};

struct CatalogIdentityManifest {
  std::vector<CatalogObjectIdentity> objects;
  std::vector<LocalizedObjectName> names;
  std::vector<LocalizedObjectComment> comments;
};

struct CatalogObjectIdentityResult {
  Status status;
  CatalogObjectIdentity object;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct LocalizedObjectNameResult {
  Status status;
  LocalizedObjectName name;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct LocalizedObjectCommentResult {
  Status status;
  LocalizedObjectComment comment;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct CatalogIdentityManifestResult {
  Status status;
  CatalogIdentityManifest manifest;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct CatalogObjectLookupResult {
  Status status;
  CatalogObjectIdentity object;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct LocalizedNameLookupResult {
  Status status;
  LocalizedObjectName name;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* CatalogObjectKindName(CatalogObjectKind kind);
const char* LocalizedNameClassName(LocalizedNameClass name_class);
CatalogObjectIdentityResult MakeCatalogObjectIdentity(TypedUuid catalog_row_uuid,
                                                      TypedUuid object_uuid,
                                                      CatalogObjectKind object_kind,
                                                      TypedUuid parent_object_uuid = {},
                                                      bool engine_owned = false,
                                                      bool cluster_shared = false);
CatalogObjectIdentityResult ValidateCatalogObjectIdentity(const CatalogObjectIdentity& object);
LocalizedObjectNameResult MakeLocalizedObjectName(TypedUuid object_uuid,
                                                  std::string language_tag,
                                                  std::string localized_path,
                                                  std::string localized_name,
                                                  LocalizedNameClass name_class,
                                                  bool default_for_language);
LocalizedObjectNameResult ValidateLocalizedObjectName(const LocalizedObjectName& name);
LocalizedObjectCommentResult MakeLocalizedObjectComment(TypedUuid object_uuid,
                                                        std::string language_tag,
                                                        std::string comment_text,
                                                        bool default_for_language);
LocalizedObjectCommentResult ValidateLocalizedObjectComment(const LocalizedObjectComment& comment);
CatalogIdentityManifestResult ValidateCatalogIdentityManifest(const CatalogIdentityManifest& manifest);
CatalogObjectLookupResult LookupCatalogObjectByObjectUuid(const CatalogIdentityManifest& manifest,
                                                          TypedUuid object_uuid);
LocalizedNameLookupResult ResolveCatalogObjectByLocalizedPath(const CatalogIdentityManifest& manifest,
                                                              std::string language_tag,
                                                              std::string localized_path);
LocalizedObjectCommentResult LookupLocalizedObjectComment(const CatalogIdentityManifest& manifest,
                                                          TypedUuid object_uuid,
                                                          std::string language_tag);
DiagnosticRecord MakeCatalogIdentityDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

}  // namespace scratchbird::catalog::bootstrap
