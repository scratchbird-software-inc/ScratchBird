// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_identity.hpp"

#include <utility>
#include <vector>

namespace scratchbird::catalog::bootstrap {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CatalogIdentityOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status CatalogIdentityWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::catalog};
}

Status CatalogIdentityErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::catalog};
}

bool IsEngineIdentityTypedUuid(const TypedUuid& uuid, UuidKind expected_kind) {
  return uuid.kind == expected_kind && uuid.valid() && scratchbird::core::uuid::IsEngineIdentityUuid(uuid.value);
}

bool SameUuidValue(const TypedUuid& left, const TypedUuid& right) {
  return left.value == right.value;
}

bool IsLanguageTagValid(const std::string& language_tag) {
  if (language_tag.empty()) {
    return false;
  }

  for (char value : language_tag) {
    const bool alpha = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    const bool digit = value >= '0' && value <= '9';
    if (!alpha && !digit && value != '-') {
      return false;
    }
  }
  return true;
}

bool ObjectExists(const CatalogIdentityManifest& manifest, const TypedUuid& object_uuid) {
  for (const CatalogObjectIdentity& object : manifest.objects) {
    if (SameUuidValue(object.object_uuid, object_uuid)) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* CatalogObjectKindName(CatalogObjectKind kind) {
  switch (kind) {
    case CatalogObjectKind::database: return "database";
    case CatalogObjectKind::schema: return "schema";
    case CatalogObjectKind::table: return "table";
    case CatalogObjectKind::view: return "view";
    case CatalogObjectKind::domain: return "domain";
    case CatalogObjectKind::procedure: return "procedure";
    case CatalogObjectKind::function: return "function";
    case CatalogObjectKind::sequence: return "sequence";
    case CatalogObjectKind::index: return "index";
    case CatalogObjectKind::package: return "package";
    case CatalogObjectKind::synonym: return "synonym";
    case CatalogObjectKind::constraint: return "constraint";
    case CatalogObjectKind::policy: return "policy";
    case CatalogObjectKind::role: return "role";
    case CatalogObjectKind::user: return "user";
    case CatalogObjectKind::metrics_surface: return "metrics_surface";
    case CatalogObjectKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* LocalizedNameClassName(LocalizedNameClass name_class) {
  switch (name_class) {
    case LocalizedNameClass::default_name: return "default_name";
    case LocalizedNameClass::alias: return "alias";
    case LocalizedNameClass::compatibility_name: return "compatibility_name";
    case LocalizedNameClass::system_path: return "system_path";
    case LocalizedNameClass::unknown: return "unknown";
  }
  return "unknown";
}

CatalogObjectIdentityResult MakeCatalogObjectIdentity(TypedUuid catalog_row_uuid,
                                                      TypedUuid object_uuid,
                                                      CatalogObjectKind object_kind,
                                                      TypedUuid parent_object_uuid,
                                                      bool engine_owned,
                                                      bool cluster_shared) {
  CatalogObjectIdentity object;
  object.catalog_row_uuid = catalog_row_uuid;
  object.object_uuid = object_uuid;
  object.object_kind = object_kind;
  object.parent_object_uuid = parent_object_uuid;
  object.engine_owned = engine_owned;
  object.cluster_shared = cluster_shared;
  return ValidateCatalogObjectIdentity(object);
}

CatalogObjectIdentityResult ValidateCatalogObjectIdentity(const CatalogObjectIdentity& object) {
  CatalogObjectIdentityResult result;
  result.status = CatalogIdentityOkStatus();
  result.object = object;

  if (!IsEngineIdentityTypedUuid(object.catalog_row_uuid, UuidKind::row)) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-ROW-UUID-MUST-BE-V7",
                                                      "catalog.identity.row_uuid_must_be_v7");
    return result;
  }

  if (!IsEngineIdentityTypedUuid(object.object_uuid, UuidKind::object)) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-OBJECT-UUID-MUST-BE-V7",
                                                      "catalog.identity.object_uuid_must_be_v7");
    return result;
  }

  if (SameUuidValue(object.catalog_row_uuid, object.object_uuid)) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-ROW-OBJECT-UUID-COLLISION",
                                                      "catalog.identity.row_object_uuid_collision");
    return result;
  }

  if (object.object_kind == CatalogObjectKind::unknown) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-UNKNOWN-OBJECT-KIND",
                                                      "catalog.identity.unknown_object_kind");
    return result;
  }

  if (!object.parent_object_uuid.value.is_nil() &&
      !IsEngineIdentityTypedUuid(object.parent_object_uuid, UuidKind::object)) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-PARENT-OBJECT-UUID-MUST-BE-V7",
                                                      "catalog.identity.parent_object_uuid_must_be_v7");
    return result;
  }

  return result;
}

LocalizedObjectNameResult MakeLocalizedObjectName(TypedUuid object_uuid,
                                                  std::string language_tag,
                                                  std::string localized_path,
                                                  std::string localized_name,
                                                  LocalizedNameClass name_class,
                                                  bool default_for_language) {
  LocalizedObjectName name;
  name.object_uuid = object_uuid;
  name.language_tag = std::move(language_tag);
  name.localized_path = std::move(localized_path);
  name.localized_name = std::move(localized_name);
  name.name_class = name_class;
  name.default_for_language = default_for_language;
  return ValidateLocalizedObjectName(name);
}

LocalizedObjectNameResult ValidateLocalizedObjectName(const LocalizedObjectName& name) {
  LocalizedObjectNameResult result;
  result.status = CatalogIdentityOkStatus();
  result.name = name;

  if (!IsEngineIdentityTypedUuid(name.object_uuid, UuidKind::object)) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-NAME-OBJECT-UUID-MUST-BE-V7",
                                                      "catalog.identity.name_object_uuid_must_be_v7");
    return result;
  }

  if (!IsLanguageTagValid(name.language_tag) || name.localized_path.empty() || name.localized_name.empty()) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-LOCALIZED-NAME-INCOMPLETE",
                                                      "catalog.identity.localized_name_incomplete",
                                                      name.localized_path);
    return result;
  }

  if (name.name_class == LocalizedNameClass::unknown) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-UNKNOWN-NAME-CLASS",
                                                      "catalog.identity.unknown_name_class");
    return result;
  }

  return result;
}

LocalizedObjectCommentResult MakeLocalizedObjectComment(TypedUuid object_uuid,
                                                        std::string language_tag,
                                                        std::string comment_text,
                                                        bool default_for_language) {
  LocalizedObjectComment comment;
  comment.object_uuid = object_uuid;
  comment.language_tag = std::move(language_tag);
  comment.comment_text = std::move(comment_text);
  comment.default_for_language = default_for_language;
  return ValidateLocalizedObjectComment(comment);
}

LocalizedObjectCommentResult ValidateLocalizedObjectComment(const LocalizedObjectComment& comment) {
  LocalizedObjectCommentResult result;
  result.status = CatalogIdentityOkStatus();
  result.comment = comment;

  if (!IsEngineIdentityTypedUuid(comment.object_uuid, UuidKind::object)) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-COMMENT-OBJECT-UUID-MUST-BE-V7",
                                                      "catalog.identity.comment_object_uuid_must_be_v7");
    return result;
  }

  if (!IsLanguageTagValid(comment.language_tag) || comment.comment_text.empty()) {
    result.status = CatalogIdentityErrorStatus();
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-COMMENT-INCOMPLETE",
                                                      "catalog.identity.comment_incomplete",
                                                      comment.language_tag);
    return result;
  }

  return result;
}

CatalogIdentityManifestResult ValidateCatalogIdentityManifest(const CatalogIdentityManifest& manifest) {
  CatalogIdentityManifestResult result;
  result.status = CatalogIdentityOkStatus();
  result.manifest = manifest;

  for (const CatalogObjectIdentity& object : manifest.objects) {
    CatalogObjectIdentityResult object_result = ValidateCatalogObjectIdentity(object);
    if (!object_result.ok()) {
      result.status = object_result.status;
      result.diagnostic = object_result.diagnostic;
      return result;
    }

    for (const CatalogObjectIdentity& other : manifest.objects) {
      if (&object == &other) {
        continue;
      }

      if (SameUuidValue(object.catalog_row_uuid, other.catalog_row_uuid)) {
        result.status = CatalogIdentityErrorStatus();
        result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                          "SB-CATALOG-IDENTITY-DUPLICATE-ROW-UUID",
                                                          "catalog.identity.duplicate_row_uuid");
        return result;
      }

      if (SameUuidValue(object.object_uuid, other.object_uuid)) {
        result.status = CatalogIdentityErrorStatus();
        result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                          "SB-CATALOG-IDENTITY-DUPLICATE-OBJECT-UUID",
                                                          "catalog.identity.duplicate_object_uuid");
        return result;
      }
    }
  }

  for (const LocalizedObjectName& name : manifest.names) {
    LocalizedObjectNameResult name_result = ValidateLocalizedObjectName(name);
    if (!name_result.ok()) {
      result.status = name_result.status;
      result.diagnostic = name_result.diagnostic;
      return result;
    }

    if (!ObjectExists(manifest, name.object_uuid)) {
      result.status = CatalogIdentityErrorStatus();
      result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                        "SB-CATALOG-IDENTITY-NAME-REFERENCES-MISSING-OBJECT",
                                                        "catalog.identity.name_references_missing_object",
                                                        name.localized_path);
      return result;
    }

    for (const LocalizedObjectName& other : manifest.names) {
      if (&name == &other) {
        continue;
      }

      if (name.language_tag == other.language_tag && name.localized_path == other.localized_path &&
          !SameUuidValue(name.object_uuid, other.object_uuid)) {
        result.status = CatalogIdentityErrorStatus();
        result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                          "SB-CATALOG-IDENTITY-LOCALIZED-PATH-AMBIGUOUS",
                                                          "catalog.identity.localized_path_ambiguous",
                                                          name.localized_path);
        return result;
      }
    }
  }

  for (const LocalizedObjectComment& comment : manifest.comments) {
    LocalizedObjectCommentResult comment_result = ValidateLocalizedObjectComment(comment);
    if (!comment_result.ok()) {
      result.status = comment_result.status;
      result.diagnostic = comment_result.diagnostic;
      return result;
    }

    if (!ObjectExists(manifest, comment.object_uuid)) {
      result.status = CatalogIdentityErrorStatus();
      result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                        "SB-CATALOG-IDENTITY-COMMENT-REFERENCES-MISSING-OBJECT",
                                                        "catalog.identity.comment_references_missing_object",
                                                        comment.language_tag);
      return result;
    }
  }

  return result;
}

CatalogObjectLookupResult LookupCatalogObjectByObjectUuid(const CatalogIdentityManifest& manifest,
                                                          TypedUuid object_uuid) {
  CatalogObjectLookupResult result;
  result.status = CatalogIdentityErrorStatus();

  if (!IsEngineIdentityTypedUuid(object_uuid, UuidKind::object)) {
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-LOOKUP-OBJECT-UUID-MUST-BE-V7",
                                                      "catalog.identity.lookup_object_uuid_must_be_v7");
    return result;
  }

  for (const CatalogObjectIdentity& object : manifest.objects) {
    if (SameUuidValue(object.object_uuid, object_uuid)) {
      result.status = CatalogIdentityOkStatus();
      result.object = object;
      return result;
    }
  }

  result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                    "SB-CATALOG-IDENTITY-OBJECT-NOT-FOUND",
                                                    "catalog.identity.object_not_found");
  return result;
}

LocalizedNameLookupResult ResolveCatalogObjectByLocalizedPath(const CatalogIdentityManifest& manifest,
                                                              std::string language_tag,
                                                              std::string localized_path) {
  LocalizedNameLookupResult result;
  result.status = CatalogIdentityErrorStatus();
  bool matched = false;

  for (const LocalizedObjectName& name : manifest.names) {
    if (name.language_tag != language_tag || name.localized_path != localized_path) {
      continue;
    }

    if (matched && !SameUuidValue(result.name.object_uuid, name.object_uuid)) {
      result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                        "SB-CATALOG-IDENTITY-LOOKUP-LOCALIZED-PATH-AMBIGUOUS",
                                                        "catalog.identity.lookup_localized_path_ambiguous",
                                                        localized_path);
      return result;
    }

    result.name = name;
    matched = true;
  }

  if (!matched) {
    result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                      "SB-CATALOG-IDENTITY-LOCALIZED-PATH-NOT-FOUND",
                                                      "catalog.identity.localized_path_not_found",
                                                      localized_path);
    return result;
  }

  result.status = CatalogIdentityOkStatus();
  return result;
}

LocalizedObjectCommentResult LookupLocalizedObjectComment(const CatalogIdentityManifest& manifest,
                                                          TypedUuid object_uuid,
                                                          std::string language_tag) {
  LocalizedObjectCommentResult result;
  result.status = CatalogIdentityErrorStatus();

  for (const LocalizedObjectComment& comment : manifest.comments) {
    if (SameUuidValue(comment.object_uuid, object_uuid) && comment.language_tag == language_tag) {
      result.status = CatalogIdentityOkStatus();
      result.comment = comment;
      return result;
    }
  }

  result.diagnostic = MakeCatalogIdentityDiagnostic(result.status,
                                                    "SB-CATALOG-IDENTITY-COMMENT-NOT-FOUND",
                                                    "catalog.identity.comment_not_found",
                                                    language_tag);
  return result;
}

DiagnosticRecord MakeCatalogIdentityDiagnostic(Status status,
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
                        "catalog.identity");
}

}  // namespace scratchbird::catalog::bootstrap
