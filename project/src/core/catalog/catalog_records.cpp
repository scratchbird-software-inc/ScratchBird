// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_records.hpp"

#include <utility>
#include <vector>

namespace scratchbird::core::catalog {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status CatalogRecordOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status CatalogRecordErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::catalog};
}

CatalogRecordDescriptor Descriptor(CatalogRecordKind kind,
                                   CatalogRecordScope scope,
                                   std::string stable_name,
                                   bool requires_object_uuid,
                                   bool requires_parent_uuid,
                                   bool may_reference_toast,
                                   bool mutable_after_create,
                                   bool parser_visible) {
  CatalogRecordDescriptor descriptor;
  descriptor.kind = kind;
  descriptor.scope = scope;
  descriptor.stable_name = std::move(stable_name);
  descriptor.requires_row_uuid = true;
  descriptor.requires_object_uuid = requires_object_uuid;
  descriptor.requires_parent_uuid = requires_parent_uuid;
  descriptor.may_reference_toast = may_reference_toast;
  descriptor.mutable_after_create = mutable_after_create;
  descriptor.parser_visible = parser_visible;
  descriptor.engine_authority = true;
  return descriptor;
}

CatalogRecordValidationResult CatalogRecordError(std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {}) {
  CatalogRecordValidationResult result;
  result.status = CatalogRecordErrorStatus();
  result.diagnostic = MakeCatalogRecordDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

}  // namespace

const char* CatalogRecordKindName(CatalogRecordKind kind) {
  switch (kind) {
    case CatalogRecordKind::database: return "database";
    case CatalogRecordKind::filespace: return "filespace";
    case CatalogRecordKind::schema: return "schema";
    case CatalogRecordKind::sql_object: return "sql_object";
    case CatalogRecordKind::localized_name: return "localized_name";
    case CatalogRecordKind::localized_comment: return "localized_comment";
    case CatalogRecordKind::synonym_descriptor: return "synonym_descriptor";
    case CatalogRecordKind::constraint_descriptor: return "constraint_descriptor";
    case CatalogRecordKind::key_descriptor: return "key_descriptor";
    case CatalogRecordKind::constraint_subject: return "constraint_subject";
    case CatalogRecordKind::constraint_dependency: return "constraint_dependency";
    case CatalogRecordKind::constraint_support_structure: return "constraint_support_structure";
    case CatalogRecordKind::column_descriptor: return "column_descriptor";
    case CatalogRecordKind::datatype_descriptor: return "datatype_descriptor";
    case CatalogRecordKind::domain: return "domain";
    case CatalogRecordKind::charset: return "charset";
    case CatalogRecordKind::charset_alias: return "charset_alias";
    case CatalogRecordKind::collation: return "collation";
    case CatalogRecordKind::collation_tailoring: return "collation_tailoring";
    case CatalogRecordKind::timezone: return "timezone";
    case CatalogRecordKind::timezone_transition: return "timezone_transition";
    case CatalogRecordKind::timezone_leap_second: return "timezone_leap_second";
    case CatalogRecordKind::resource_bundle: return "resource_bundle";
    case CatalogRecordKind::resource_artifact: return "resource_artifact";
    case CatalogRecordKind::policy: return "policy";
    case CatalogRecordKind::config_profile: return "config_profile";
    case CatalogRecordKind::user_account: return "user_account";
    case CatalogRecordKind::group_account: return "group_account";
    case CatalogRecordKind::role_account: return "role_account";
    case CatalogRecordKind::grant_record: return "grant_record";
    case CatalogRecordKind::masking_policy: return "masking_policy";
    case CatalogRecordKind::rls_policy: return "rls_policy";
    case CatalogRecordKind::udr_package: return "udr_package";
    case CatalogRecordKind::parser_package: return "parser_package";
    case CatalogRecordKind::sblr_module: return "sblr_module";
    case CatalogRecordKind::storage_descriptor: return "storage_descriptor";
    case CatalogRecordKind::table_descriptor: return "table_descriptor";
    case CatalogRecordKind::index_descriptor: return "index_descriptor";
    case CatalogRecordKind::toast_reference: return "toast_reference";
    case CatalogRecordKind::audit_evidence: return "audit_evidence";
    case CatalogRecordKind::management_operation: return "management_operation";
    case CatalogRecordKind::metric_descriptor: return "metric_descriptor";
    case CatalogRecordKind::metric_current_value: return "metric_current_value";
    case CatalogRecordKind::protected_material: return "protected_material";
    case CatalogRecordKind::protected_material_version: return "protected_material_version";
    case CatalogRecordKind::protected_material_policy_binding: return "protected_material_policy_binding";
    case CatalogRecordKind::protected_material_audit_event: return "protected_material_audit_event";
    case CatalogRecordKind::cluster_stub: return "cluster_stub";
    case CatalogRecordKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* CatalogRecordScopeName(CatalogRecordScope scope) {
  switch (scope) {
    case CatalogRecordScope::local_database: return "local_database";
    case CatalogRecordScope::cluster_shared: return "cluster_shared";
    case CatalogRecordScope::compatibility_projection: return "compatibility_projection";
    case CatalogRecordScope::private_cluster: return "private_cluster";
    case CatalogRecordScope::unknown: return "unknown";
  }
  return "unknown";
}

const std::vector<CatalogRecordDescriptor>& BuiltinCatalogRecordDescriptors() {
  static const std::vector<CatalogRecordDescriptor> descriptors = {
      Descriptor(CatalogRecordKind::database, CatalogRecordScope::local_database, "database", true, false, false, false, true),
      Descriptor(CatalogRecordKind::filespace, CatalogRecordScope::local_database, "filespace", true, true, false, false, false),
      Descriptor(CatalogRecordKind::schema, CatalogRecordScope::local_database, "schema", true, true, false, true, true),
      Descriptor(CatalogRecordKind::sql_object, CatalogRecordScope::local_database, "sql_object", true, true, true, true, true),
      Descriptor(CatalogRecordKind::localized_name, CatalogRecordScope::local_database, "localized_name", false, true, true, true, true),
      Descriptor(CatalogRecordKind::localized_comment, CatalogRecordScope::local_database, "localized_comment", false, true, true, true, true),
      Descriptor(CatalogRecordKind::synonym_descriptor, CatalogRecordScope::local_database, "synonym_descriptor", true, true, false, true, true),
      Descriptor(CatalogRecordKind::constraint_descriptor, CatalogRecordScope::local_database, "constraint_descriptor", true, true, true, false, true),
      Descriptor(CatalogRecordKind::key_descriptor, CatalogRecordScope::local_database, "key_descriptor", true, true, true, false, true),
      Descriptor(CatalogRecordKind::constraint_subject, CatalogRecordScope::local_database, "constraint_subject", true, true, true, false, true),
      Descriptor(CatalogRecordKind::constraint_dependency, CatalogRecordScope::local_database, "constraint_dependency", true, true, true, false, true),
      Descriptor(CatalogRecordKind::constraint_support_structure, CatalogRecordScope::local_database, "constraint_support_structure", true, true, true, false, true),
      Descriptor(CatalogRecordKind::column_descriptor, CatalogRecordScope::local_database, "column_descriptor", true, true, true, false, true),
      Descriptor(CatalogRecordKind::datatype_descriptor, CatalogRecordScope::local_database, "datatype_descriptor", true, true, false, false, true),
      Descriptor(CatalogRecordKind::domain, CatalogRecordScope::local_database, "domain", true, true, true, true, true),
      Descriptor(CatalogRecordKind::charset, CatalogRecordScope::local_database, "charset", true, true, true, false, true),
      Descriptor(CatalogRecordKind::charset_alias, CatalogRecordScope::local_database, "charset_alias", false, true, true, true, true),
      Descriptor(CatalogRecordKind::collation, CatalogRecordScope::local_database, "collation", true, true, true, true, true),
      Descriptor(CatalogRecordKind::collation_tailoring, CatalogRecordScope::local_database, "collation_tailoring", false, true, true, true, false),
      Descriptor(CatalogRecordKind::timezone, CatalogRecordScope::local_database, "timezone", true, true, true, true, true),
      Descriptor(CatalogRecordKind::timezone_transition, CatalogRecordScope::local_database, "timezone_transition", false, true, false, true, false),
      Descriptor(CatalogRecordKind::timezone_leap_second, CatalogRecordScope::local_database, "timezone_leap_second", false, true, false, true, false),
      Descriptor(CatalogRecordKind::resource_bundle, CatalogRecordScope::local_database, "resource_bundle", true, true, true, false, false),
      Descriptor(CatalogRecordKind::resource_artifact, CatalogRecordScope::local_database, "resource_artifact", true, true, true, false, false),
      Descriptor(CatalogRecordKind::policy, CatalogRecordScope::local_database, "policy", true, true, true, true, true),
      Descriptor(CatalogRecordKind::config_profile, CatalogRecordScope::local_database, "config_profile", true, true, true, true, false),
      Descriptor(CatalogRecordKind::user_account, CatalogRecordScope::local_database, "user_account", true, true, true, true, false),
      Descriptor(CatalogRecordKind::group_account, CatalogRecordScope::local_database, "group_account", true, true, true, true, false),
      Descriptor(CatalogRecordKind::role_account, CatalogRecordScope::local_database, "role_account", true, true, true, true, false),
      Descriptor(CatalogRecordKind::grant_record, CatalogRecordScope::local_database, "grant_record", false, true, true, true, false),
      Descriptor(CatalogRecordKind::masking_policy, CatalogRecordScope::local_database, "masking_policy", true, true, true, true, false),
      Descriptor(CatalogRecordKind::rls_policy, CatalogRecordScope::local_database, "rls_policy", true, true, true, true, false),
      Descriptor(CatalogRecordKind::udr_package, CatalogRecordScope::local_database, "udr_package", true, true, true, true, false),
      Descriptor(CatalogRecordKind::parser_package, CatalogRecordScope::local_database, "parser_package", true, true, true, true, false),
      Descriptor(CatalogRecordKind::sblr_module, CatalogRecordScope::local_database, "sblr_module", true, true, true, true, false),
      Descriptor(CatalogRecordKind::storage_descriptor, CatalogRecordScope::local_database, "storage_descriptor", true, true, true, true, false),
      Descriptor(CatalogRecordKind::table_descriptor, CatalogRecordScope::local_database, "table_descriptor", true, true, true, true, true),
      Descriptor(CatalogRecordKind::index_descriptor, CatalogRecordScope::local_database, "index_descriptor", true, true, true, true, true),
      Descriptor(CatalogRecordKind::toast_reference, CatalogRecordScope::local_database, "toast_reference", false, true, true, true, false),
      Descriptor(CatalogRecordKind::audit_evidence, CatalogRecordScope::local_database, "audit_evidence", false, true, true, false, false),
      Descriptor(CatalogRecordKind::management_operation, CatalogRecordScope::local_database, "management_operation", true, true, true, true, false),
      Descriptor(CatalogRecordKind::metric_descriptor, CatalogRecordScope::local_database, "metric_descriptor", true, true, true, true, true),
      Descriptor(CatalogRecordKind::metric_current_value, CatalogRecordScope::local_database, "metric_current_value", true, true, false, true, true),
      Descriptor(CatalogRecordKind::protected_material, CatalogRecordScope::local_database, "protected_material", true, true, true, true, false),
      Descriptor(CatalogRecordKind::protected_material_version, CatalogRecordScope::local_database, "protected_material_version", true, true, true, true, false),
      Descriptor(CatalogRecordKind::protected_material_policy_binding, CatalogRecordScope::local_database, "protected_material_policy_binding", true, true, true, true, false),
      Descriptor(CatalogRecordKind::protected_material_audit_event, CatalogRecordScope::local_database, "protected_material_audit_event", true, true, true, false, false),
      Descriptor(CatalogRecordKind::cluster_stub, CatalogRecordScope::private_cluster, "cluster_stub", true, true, false, false, false),
  };
  return descriptors;
}

CatalogRecordValidationResult LookupCatalogRecordDescriptor(CatalogRecordKind kind) {
  for (const CatalogRecordDescriptor& descriptor : BuiltinCatalogRecordDescriptors()) {
    if (descriptor.kind == kind) {
      return ValidateCatalogRecordDescriptor(descriptor);
    }
  }
  return CatalogRecordError("SB-CATALOG-RECORD-UNKNOWN-KIND",
                            "catalog.record.unknown_kind",
                            CatalogRecordKindName(kind));
}

CatalogRecordValidationResult ValidateCatalogRecordDescriptor(const CatalogRecordDescriptor& descriptor) {
  if (descriptor.kind == CatalogRecordKind::unknown || descriptor.scope == CatalogRecordScope::unknown ||
      descriptor.stable_name.empty()) {
    return CatalogRecordError("SB-CATALOG-RECORD-DESCRIPTOR-INCOMPLETE",
                              "catalog.record.descriptor_incomplete",
                              descriptor.stable_name);
  }
  if (!descriptor.engine_authority) {
    return CatalogRecordError("SB-CATALOG-RECORD-ENGINE-AUTHORITY-REQUIRED",
                              "catalog.record.engine_authority_required",
                              descriptor.stable_name);
  }

  CatalogRecordValidationResult result;
  result.status = CatalogRecordOkStatus();
  result.descriptor = descriptor;
  return result;
}

DiagnosticRecord MakeCatalogRecordDiagnostic(Status status,
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
                        "core.catalog.records");
}

}  // namespace scratchbird::core::catalog
