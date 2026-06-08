// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_index_profile.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::catalog {
namespace {

CatalogTableColumnProfile Column(std::string column_name,
                                 CatalogColumnRole role,
                                 bool nullable = false) {
  CatalogTableColumnProfile column;
  column.column_name = std::move(column_name);
  column.role = role;
  column.nullable = nullable;
  return column;
}

CatalogIndexKeyColumnProfile Key(std::string column_name,
                                 CatalogColumnRole role,
                                 bool equality = true,
                                 bool ordered = false,
                                 bool prefix = false) {
  CatalogIndexKeyColumnProfile key;
  key.column_name = std::move(column_name);
  key.role = role;
  key.equality_component = equality;
  key.ordered_component = ordered;
  key.prefix_component = prefix;
  return key;
}

CatalogTableProfile Table(std::string table_path,
                          CatalogRecordKind record_kind,
                          CatalogRecordScope scope,
                          CatalogTableSurface surface,
                          std::vector<CatalogTableColumnProfile> columns,
                          bool parser_visible = false) {
  CatalogTableProfile table;
  table.table_path = std::move(table_path);
  table.record_kind = record_kind;
  table.scope = scope;
  table.surface = surface;
  table.columns = std::move(columns);
  table.local_only = true;
  table.parser_visible = parser_visible;
  table.cluster_path_fail_closed = true;
  return table;
}

CatalogPhysicalIndexProfile Index(std::string index_name,
                                  std::string table_path,
                                  CatalogIndexMethod method,
                                  CatalogIndexPurpose purpose,
                                  std::vector<CatalogIndexKeyColumnProfile> key_columns) {
  CatalogPhysicalIndexProfile profile;
  profile.index_name = std::move(index_name);
  profile.table_path = std::move(table_path);
  profile.method = method;
  profile.purpose = purpose;
  profile.key_columns = std::move(key_columns);
  profile.authoritative = true;
  profile.supports_mga_snapshot_visibility = true;
  profile.cluster_path_fail_closed = true;
  profile.authority_boundary = "sys.catalog.uuid_identity";
  return profile;
}

void AddIssue(CatalogIndexValidationResult* result,
              std::string code,
              std::string detail,
              bool error = true) {
  result->issues.push_back({std::move(code), std::move(detail), error});
  if (error) { result->ok = false; }
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool KeyUsesUuidExactRole(CatalogColumnRole role) {
  return role == CatalogColumnRole::row_uuid || role == CatalogColumnRole::object_uuid ||
         role == CatalogColumnRole::descriptor_uuid ||
         role == CatalogColumnRole::column_uuid ||
         role == CatalogColumnRole::constraint_uuid ||
         role == CatalogColumnRole::key_descriptor_uuid ||
         role == CatalogColumnRole::support_binding_uuid;
}

}  // namespace

const char* CatalogTableSurfaceName(CatalogTableSurface surface) {
  switch (surface) {
    case CatalogTableSurface::base_catalog: return "base_catalog";
    case CatalogTableSurface::identity_resolver: return "identity_resolver";
    case CatalogTableSurface::comment_resolver: return "comment_resolver";
    case CatalogTableSurface::information_projection: return "information_projection";
  }
  return "unknown";
}

const char* CatalogColumnRoleName(CatalogColumnRole role) {
  switch (role) {
    case CatalogColumnRole::row_uuid: return "row_uuid";
    case CatalogColumnRole::object_uuid: return "object_uuid";
    case CatalogColumnRole::parent_object_uuid: return "parent_object_uuid";
    case CatalogColumnRole::schema_uuid: return "schema_uuid";
    case CatalogColumnRole::descriptor_uuid: return "descriptor_uuid";
    case CatalogColumnRole::transaction_uuid: return "transaction_uuid";
    case CatalogColumnRole::transaction_id: return "transaction_id";
    case CatalogColumnRole::catalog_generation_id: return "catalog_generation_id";
    case CatalogColumnRole::history_sequence: return "history_sequence";
    case CatalogColumnRole::lifecycle_state: return "lifecycle_state";
    case CatalogColumnRole::machine_key: return "machine_key";
    case CatalogColumnRole::payload_hash: return "payload_hash";
    case CatalogColumnRole::security_policy_uuid: return "security_policy_uuid";
    case CatalogColumnRole::security_epoch: return "security_epoch";
    case CatalogColumnRole::resource_epoch: return "resource_epoch";
    case CatalogColumnRole::ordinal: return "ordinal";
    case CatalogColumnRole::resolver_language: return "resolver_language";
    case CatalogColumnRole::resolver_profile: return "resolver_profile";
    case CatalogColumnRole::resolver_name_class: return "resolver_name_class";
    case CatalogColumnRole::resolver_raw_name: return "resolver_raw_name";
    case CatalogColumnRole::resolver_display_name: return "resolver_display_name";
    case CatalogColumnRole::resolver_normalized_lookup_key: return "resolver_normalized_lookup_key";
    case CatalogColumnRole::resolver_exact_lookup_key: return "resolver_exact_lookup_key";
    case CatalogColumnRole::resolver_full_path_lookup_key: return "resolver_full_path_lookup_key";
    case CatalogColumnRole::resolver_comment_text: return "resolver_comment_text";
    case CatalogColumnRole::object_class: return "object_class";
    case CatalogColumnRole::target_object_uuid: return "target_object_uuid";
    case CatalogColumnRole::target_object_class: return "target_object_class";
    case CatalogColumnRole::owner_uuid: return "owner_uuid";
    case CatalogColumnRole::dependency_strength: return "dependency_strength";
    case CatalogColumnRole::constraint_uuid: return "constraint_uuid";
    case CatalogColumnRole::constraint_class: return "constraint_class";
    case CatalogColumnRole::owner_object_uuid: return "owner_object_uuid";
    case CatalogColumnRole::constraint_policy_version_uuid: return "constraint_policy_version_uuid";
    case CatalogColumnRole::enforcement_timing: return "enforcement_timing";
    case CatalogColumnRole::validation_state: return "validation_state";
    case CatalogColumnRole::trust_state: return "trust_state";
    case CatalogColumnRole::support_requirement: return "support_requirement";
    case CatalogColumnRole::predicate_sblr_uuid: return "predicate_sblr_uuid";
    case CatalogColumnRole::diagnostic_profile_uuid: return "diagnostic_profile_uuid";
    case CatalogColumnRole::metrics_profile_uuid: return "metrics_profile_uuid";
    case CatalogColumnRole::conformance_profile_uuid: return "conformance_profile_uuid";
    case CatalogColumnRole::descriptor_hash: return "descriptor_hash";
    case CatalogColumnRole::key_descriptor_uuid: return "key_descriptor_uuid";
    case CatalogColumnRole::key_class: return "key_class";
    case CatalogColumnRole::component_order_hash: return "component_order_hash";
    case CatalogColumnRole::comparison_profile_hash: return "comparison_profile_hash";
    case CatalogColumnRole::null_policy: return "null_policy";
    case CatalogColumnRole::canonical_encoding_uuid: return "canonical_encoding_uuid";
    case CatalogColumnRole::constraint_subject_uuid: return "constraint_subject_uuid";
    case CatalogColumnRole::subject_object_uuid: return "subject_object_uuid";
    case CatalogColumnRole::subject_kind: return "subject_kind";
    case CatalogColumnRole::subject_ordinal: return "subject_ordinal";
    case CatalogColumnRole::expression_sblr_uuid: return "expression_sblr_uuid";
    case CatalogColumnRole::dependency_uuid: return "dependency_uuid";
    case CatalogColumnRole::dependency_kind: return "dependency_kind";
    case CatalogColumnRole::dependency_object_uuid: return "dependency_object_uuid";
    case CatalogColumnRole::dependency_version_uuid: return "dependency_version_uuid";
    case CatalogColumnRole::invalidation_action: return "invalidation_action";
    case CatalogColumnRole::support_binding_uuid: return "support_binding_uuid";
    case CatalogColumnRole::support_uuid: return "support_uuid";
    case CatalogColumnRole::support_class: return "support_class";
    case CatalogColumnRole::support_family: return "support_family";
    case CatalogColumnRole::coverage_scope_hash: return "coverage_scope_hash";
    case CatalogColumnRole::durability_class: return "durability_class";
    case CatalogColumnRole::residency_class: return "residency_class";
    case CatalogColumnRole::validity_state: return "validity_state";
    case CatalogColumnRole::enforcement_role: return "enforcement_role";
    case CatalogColumnRole::column_uuid: return "column_uuid";
    case CatalogColumnRole::default_expression_envelope: return "default_expression_envelope";
  }
  return "unknown";
}

const char* CatalogIndexMethodName(CatalogIndexMethod method) {
  switch (method) {
    case CatalogIndexMethod::hash_equality: return "hash_equality";
    case CatalogIndexMethod::btree_ordered: return "btree_ordered";
  }
  return "unknown";
}

const char* CatalogIndexPurposeName(CatalogIndexPurpose purpose) {
  switch (purpose) {
    case CatalogIndexPurpose::uuid_exact_lookup: return "uuid_exact_lookup";
    case CatalogIndexPurpose::row_uuid_exact_lookup: return "row_uuid_exact_lookup";
    case CatalogIndexPurpose::parent_child_group: return "parent_child_group";
    case CatalogIndexPurpose::catalog_generation_visibility: return "catalog_generation_visibility";
    case CatalogIndexPurpose::transaction_history: return "transaction_history";
    case CatalogIndexPurpose::object_history: return "object_history";
    case CatalogIndexPurpose::dependency_source_group: return "dependency_source_group";
    case CatalogIndexPurpose::dependency_target_group: return "dependency_target_group";
    case CatalogIndexPurpose::constraint_owner_group: return "constraint_owner_group";
    case CatalogIndexPurpose::constraint_subject_group: return "constraint_subject_group";
    case CatalogIndexPurpose::constraint_dependency_group: return "constraint_dependency_group";
    case CatalogIndexPurpose::constraint_support_group: return "constraint_support_group";
    case CatalogIndexPurpose::name_resolution_lookup: return "name_resolution_lookup";
    case CatalogIndexPurpose::full_path_name_accelerator: return "full_path_name_accelerator";
    case CatalogIndexPurpose::uuid_to_name_language_fallback: return "uuid_to_name_language_fallback";
    case CatalogIndexPurpose::comment_language_fallback: return "comment_language_fallback";
    case CatalogIndexPurpose::index_definition_lookup: return "index_definition_lookup";
  }
  return "unknown";
}

const std::vector<CatalogTableProfile>& BuiltinCatalogTableProfiles() {
  static const std::vector<CatalogTableProfile> tables = {
      Table("sys.catalog.object_identity",
            CatalogRecordKind::sql_object,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("object_uuid", CatalogColumnRole::object_uuid),
             Column("object_class", CatalogColumnRole::machine_key),
             Column("parent_object_uuid", CatalogColumnRole::parent_object_uuid, true),
             Column("schema_uuid", CatalogColumnRole::schema_uuid, true),
             Column("descriptor_uuid", CatalogColumnRole::descriptor_uuid, true),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_transaction_uuid", CatalogColumnRole::transaction_uuid),
             Column("created_local_transaction_id", CatalogColumnRole::transaction_id),
             Column("dropped_transaction_uuid", CatalogColumnRole::transaction_uuid, true),
             Column("dropped_local_transaction_id", CatalogColumnRole::transaction_id, true),
             Column("history_sequence", CatalogColumnRole::history_sequence),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state),
             Column("security_policy_uuid", CatalogColumnRole::security_policy_uuid),
             Column("payload_descriptor_hash", CatalogColumnRole::payload_hash, true)}),
      Table("sys.catalog.object_versions",
            CatalogRecordKind::audit_evidence,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("object_uuid", CatalogColumnRole::object_uuid),
             Column("object_class", CatalogColumnRole::machine_key),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_transaction_uuid", CatalogColumnRole::transaction_uuid),
             Column("created_local_transaction_id", CatalogColumnRole::transaction_id),
             Column("ended_transaction_uuid", CatalogColumnRole::transaction_uuid, true),
             Column("ended_local_transaction_id", CatalogColumnRole::transaction_id, true),
             Column("history_sequence", CatalogColumnRole::history_sequence),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state),
             Column("payload_descriptor_hash", CatalogColumnRole::payload_hash, true)}),
      Table("sys.catalog.object_dependencies",
            CatalogRecordKind::audit_evidence,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("source_object_uuid", CatalogColumnRole::object_uuid),
             Column("source_object_class", CatalogColumnRole::machine_key),
             Column("dependency_object_uuid", CatalogColumnRole::object_uuid),
             Column("dependency_object_class", CatalogColumnRole::machine_key),
             Column("dependency_kind", CatalogColumnRole::machine_key),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_transaction_uuid", CatalogColumnRole::transaction_uuid),
             Column("created_local_transaction_id", CatalogColumnRole::transaction_id),
             Column("history_sequence", CatalogColumnRole::history_sequence),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)}),
      Table("sys.catalog.synonym",
            CatalogRecordKind::synonym_descriptor,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("synonym_uuid", CatalogColumnRole::object_uuid),
             Column("parent_object_uuid", CatalogColumnRole::parent_object_uuid),
             Column("target_object_uuid", CatalogColumnRole::target_object_uuid),
             Column("target_object_class", CatalogColumnRole::target_object_class),
             Column("owner_uuid", CatalogColumnRole::owner_uuid),
             Column("policy_uuid", CatalogColumnRole::security_policy_uuid, true),
             Column("created_txn_uuid", CatalogColumnRole::transaction_uuid),
             Column("retired_txn_uuid", CatalogColumnRole::transaction_uuid, true),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("security_epoch", CatalogColumnRole::security_epoch),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state),
             Column("dependency_strength", CatalogColumnRole::dependency_strength)}),
      Table("sys.catalog.column_descriptor",
            CatalogRecordKind::column_descriptor,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("column_uuid", CatalogColumnRole::column_uuid),
             Column("owner_object_uuid", CatalogColumnRole::owner_object_uuid),
             Column("descriptor_uuid", CatalogColumnRole::descriptor_uuid, true),
             Column("ordinal", CatalogColumnRole::ordinal),
             Column("null_policy", CatalogColumnRole::null_policy),
             Column("default_expression_envelope", CatalogColumnRole::default_expression_envelope, true),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_txn_uuid", CatalogColumnRole::transaction_uuid),
             Column("security_epoch", CatalogColumnRole::security_epoch),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)}),
      Table("sys.constraint_descriptor",
            CatalogRecordKind::constraint_descriptor,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("constraint_uuid", CatalogColumnRole::constraint_uuid),
             Column("constraint_class", CatalogColumnRole::constraint_class),
             Column("owner_object_uuid", CatalogColumnRole::owner_object_uuid),
             Column("name_ref_uuid", CatalogColumnRole::row_uuid, true),
             Column("constraint_policy_version_uuid", CatalogColumnRole::constraint_policy_version_uuid, true),
             Column("enforcement_timing", CatalogColumnRole::enforcement_timing),
             Column("validation_state", CatalogColumnRole::validation_state),
             Column("trust_state", CatalogColumnRole::trust_state),
             Column("support_requirement", CatalogColumnRole::support_requirement),
             Column("predicate_sblr_uuid", CatalogColumnRole::predicate_sblr_uuid, true),
             Column("diagnostic_profile_uuid", CatalogColumnRole::diagnostic_profile_uuid, true),
             Column("metrics_profile_uuid", CatalogColumnRole::metrics_profile_uuid, true),
             Column("conformance_profile_uuid", CatalogColumnRole::conformance_profile_uuid, true),
             Column("constraint_hash", CatalogColumnRole::descriptor_hash),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_txn_uuid", CatalogColumnRole::transaction_uuid),
             Column("retired_txn_uuid", CatalogColumnRole::transaction_uuid, true),
             Column("security_epoch", CatalogColumnRole::security_epoch),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)}),
      Table("sys.key_descriptor",
            CatalogRecordKind::key_descriptor,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("key_descriptor_uuid", CatalogColumnRole::key_descriptor_uuid),
             Column("constraint_uuid", CatalogColumnRole::constraint_uuid, true),
             Column("key_class", CatalogColumnRole::key_class),
             Column("owner_object_uuid", CatalogColumnRole::owner_object_uuid),
             Column("component_order_hash", CatalogColumnRole::component_order_hash),
             Column("comparison_profile_hash", CatalogColumnRole::comparison_profile_hash),
             Column("null_policy", CatalogColumnRole::null_policy),
             Column("canonical_encoding_uuid", CatalogColumnRole::canonical_encoding_uuid, true),
             Column("candidate_reference_allowed", CatalogColumnRole::machine_key),
             Column("key_state", CatalogColumnRole::lifecycle_state),
             Column("key_hash", CatalogColumnRole::descriptor_hash),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_txn_uuid", CatalogColumnRole::transaction_uuid),
             Column("retired_txn_uuid", CatalogColumnRole::transaction_uuid, true),
             Column("security_epoch", CatalogColumnRole::security_epoch)}),
      Table("sys.constraint_subject",
            CatalogRecordKind::constraint_subject,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("constraint_subject_uuid", CatalogColumnRole::constraint_subject_uuid),
             Column("constraint_uuid", CatalogColumnRole::constraint_uuid),
             Column("subject_kind", CatalogColumnRole::subject_kind),
             Column("subject_object_uuid", CatalogColumnRole::subject_object_uuid),
             Column("subject_descriptor_hash", CatalogColumnRole::descriptor_hash, true),
             Column("expression_sblr_uuid", CatalogColumnRole::expression_sblr_uuid, true),
             Column("ordinal", CatalogColumnRole::subject_ordinal),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_txn_uuid", CatalogColumnRole::transaction_uuid),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)}),
      Table("sys.constraint_dependency",
            CatalogRecordKind::constraint_dependency,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("dependency_uuid", CatalogColumnRole::dependency_uuid),
             Column("constraint_uuid", CatalogColumnRole::constraint_uuid),
             Column("dependency_kind", CatalogColumnRole::dependency_kind),
             Column("dependency_object_uuid", CatalogColumnRole::dependency_object_uuid),
             Column("dependency_version_uuid", CatalogColumnRole::dependency_version_uuid, true),
             Column("invalidation_action", CatalogColumnRole::invalidation_action),
             Column("dependency_hash", CatalogColumnRole::descriptor_hash),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_txn_uuid", CatalogColumnRole::transaction_uuid),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)}),
      Table("sys.constraint_support_structure",
            CatalogRecordKind::constraint_support_structure,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("support_binding_uuid", CatalogColumnRole::support_binding_uuid),
             Column("constraint_uuid", CatalogColumnRole::constraint_uuid),
             Column("support_uuid", CatalogColumnRole::support_uuid),
             Column("support_class", CatalogColumnRole::support_class),
             Column("support_family", CatalogColumnRole::support_family),
             Column("coverage_scope_hash", CatalogColumnRole::coverage_scope_hash),
             Column("durability_class", CatalogColumnRole::durability_class),
             Column("residency_class", CatalogColumnRole::residency_class),
             Column("validity_state", CatalogColumnRole::validity_state),
             Column("enforcement_role", CatalogColumnRole::enforcement_role),
             Column("binding_hash", CatalogColumnRole::descriptor_hash),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_txn_uuid", CatalogColumnRole::transaction_uuid),
             Column("retired_txn_uuid", CatalogColumnRole::transaction_uuid, true)}),
      Table("sys.catalog.index_definitions",
            CatalogRecordKind::index_descriptor,
            CatalogRecordScope::local_database,
            CatalogTableSurface::base_catalog,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("index_uuid", CatalogColumnRole::object_uuid),
             Column("table_object_uuid", CatalogColumnRole::object_uuid),
             Column("index_family", CatalogColumnRole::machine_key),
             Column("access_method", CatalogColumnRole::machine_key),
             Column("physical_profile_key", CatalogColumnRole::machine_key),
             Column("key_descriptor_uuid", CatalogColumnRole::descriptor_uuid),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_transaction_uuid", CatalogColumnRole::transaction_uuid),
             Column("created_local_transaction_id", CatalogColumnRole::transaction_id),
             Column("history_sequence", CatalogColumnRole::history_sequence),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)}),
      Table("sys.catalog.object_name_vectors",
            CatalogRecordKind::localized_name,
            CatalogRecordScope::local_database,
            CatalogTableSurface::identity_resolver,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("name_vector_uuid", CatalogColumnRole::object_uuid),
             Column("object_uuid", CatalogColumnRole::object_uuid),
             Column("object_class", CatalogColumnRole::machine_key),
             Column("owning_schema_uuid", CatalogColumnRole::schema_uuid, true),
             Column("default_name_entry_uuid", CatalogColumnRole::row_uuid, true),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("security_policy_uuid", CatalogColumnRole::security_policy_uuid),
             Column("name_resolution_epoch", CatalogColumnRole::resource_epoch),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)}),
      Table("sys.catalog.object_name_entries",
            CatalogRecordKind::localized_name,
            CatalogRecordScope::local_database,
            CatalogTableSurface::identity_resolver,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("name_entry_uuid", CatalogColumnRole::object_uuid),
             Column("name_vector_uuid", CatalogColumnRole::object_uuid),
             Column("object_uuid", CatalogColumnRole::object_uuid),
             Column("object_class", CatalogColumnRole::machine_key),
             Column("scope_uuid", CatalogColumnRole::parent_object_uuid),
             Column("parent_schema_uuid", CatalogColumnRole::schema_uuid, true),
             Column("language_tag", CatalogColumnRole::resolver_language),
             Column("name_class", CatalogColumnRole::resolver_name_class),
             Column("identifier_profile_uuid", CatalogColumnRole::resolver_profile),
             Column("raw_name_text", CatalogColumnRole::resolver_raw_name),
             Column("display_name", CatalogColumnRole::resolver_display_name),
             Column("normalized_lookup_key", CatalogColumnRole::resolver_normalized_lookup_key),
             Column("exact_lookup_key", CatalogColumnRole::resolver_exact_lookup_key),
             Column("full_path_lookup_key", CatalogColumnRole::resolver_full_path_lookup_key),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_transaction_uuid", CatalogColumnRole::transaction_uuid),
             Column("created_local_transaction_id", CatalogColumnRole::transaction_id),
             Column("resource_epoch", CatalogColumnRole::resource_epoch),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)},
            true),
      Table("sys.catalog.object_comments",
            CatalogRecordKind::localized_comment,
            CatalogRecordScope::local_database,
            CatalogTableSurface::comment_resolver,
            {Column("row_uuid", CatalogColumnRole::row_uuid),
             Column("comment_uuid", CatalogColumnRole::object_uuid),
             Column("object_uuid", CatalogColumnRole::object_uuid),
             Column("object_class", CatalogColumnRole::machine_key),
             Column("language_tag", CatalogColumnRole::resolver_language),
             Column("comment_text", CatalogColumnRole::resolver_comment_text),
             Column("catalog_generation_id", CatalogColumnRole::catalog_generation_id),
             Column("created_transaction_uuid", CatalogColumnRole::transaction_uuid),
             Column("created_local_transaction_id", CatalogColumnRole::transaction_id),
             Column("lifecycle_state", CatalogColumnRole::lifecycle_state)},
            true),
  };
  return tables;
}

const std::vector<CatalogPhysicalIndexProfile>& BuiltinCatalogIndexProfiles() {
  static const std::vector<CatalogPhysicalIndexProfile> profiles = [] {
    std::vector<CatalogPhysicalIndexProfile> out;

    auto push = [&out](CatalogPhysicalIndexProfile profile) {
      out.push_back(std::move(profile));
    };

    auto object_uuid = Index("sys_catalog_object_identity_uuid_hash",
                             "sys.catalog.object_identity",
                             CatalogIndexMethod::hash_equality,
                             CatalogIndexPurpose::uuid_exact_lookup,
                             {Key("object_uuid", CatalogColumnRole::object_uuid)});
    object_uuid.unique = true;
    object_uuid.supports_uuid_exact_lookup = true;
    push(std::move(object_uuid));

    auto object_row = Index("sys_catalog_object_identity_row_hash",
                            "sys.catalog.object_identity",
                            CatalogIndexMethod::hash_equality,
                            CatalogIndexPurpose::row_uuid_exact_lookup,
                            {Key("row_uuid", CatalogColumnRole::row_uuid)});
    object_row.unique = true;
    object_row.supports_uuid_exact_lookup = true;
    push(std::move(object_row));

    auto parent_generation = Index("sys_catalog_object_identity_parent_generation_btree",
                                   "sys.catalog.object_identity",
                                   CatalogIndexMethod::btree_ordered,
                                   CatalogIndexPurpose::parent_child_group,
                                   {Key("schema_uuid", CatalogColumnRole::schema_uuid, true, false, true),
                                    Key("parent_object_uuid", CatalogColumnRole::parent_object_uuid, true, false, true),
                                    Key("object_class", CatalogColumnRole::machine_key, true, false, true),
                                    Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, false),
                                    Key("object_uuid", CatalogColumnRole::object_uuid, true, true, false)});
    parent_generation.supports_ordered_scan = true;
    parent_generation.supports_group_scan = true;
    parent_generation.supports_prefix_scan = true;
    parent_generation.supports_catalog_generation_visibility = true;
    push(std::move(parent_generation));

    auto generation = Index("sys_catalog_object_identity_generation_btree",
                            "sys.catalog.object_identity",
                            CatalogIndexMethod::btree_ordered,
                            CatalogIndexPurpose::catalog_generation_visibility,
                            {Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                             Key("history_sequence", CatalogColumnRole::history_sequence, true, true, false),
                             Key("object_uuid", CatalogColumnRole::object_uuid, true, true, false)});
    generation.supports_ordered_scan = true;
    generation.supports_prefix_scan = true;
    generation.supports_catalog_generation_visibility = true;
    push(std::move(generation));

    auto object_history = Index("sys_catalog_object_versions_history_btree",
                                "sys.catalog.object_versions",
                                CatalogIndexMethod::btree_ordered,
                                CatalogIndexPurpose::object_history,
                                {Key("object_uuid", CatalogColumnRole::object_uuid, true, false, true),
                                 Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                 Key("history_sequence", CatalogColumnRole::history_sequence, true, true, false)});
    object_history.supports_ordered_scan = true;
    object_history.supports_group_scan = true;
    object_history.supports_prefix_scan = true;
    object_history.supports_catalog_generation_visibility = true;
    object_history.supports_transaction_history = true;
    push(std::move(object_history));

    auto tx_history = Index("sys_catalog_object_versions_tx_history_btree",
                            "sys.catalog.object_versions",
                            CatalogIndexMethod::btree_ordered,
                            CatalogIndexPurpose::transaction_history,
                            {Key("created_local_transaction_id", CatalogColumnRole::transaction_id, true, true, true),
                             Key("history_sequence", CatalogColumnRole::history_sequence, true, true, false),
                             Key("object_uuid", CatalogColumnRole::object_uuid, true, true, false)});
    tx_history.supports_ordered_scan = true;
    tx_history.supports_prefix_scan = true;
    tx_history.supports_transaction_history = true;
    push(std::move(tx_history));

    auto dependencies_source = Index("sys_catalog_object_dependencies_source_btree",
                                     "sys.catalog.object_dependencies",
                                     CatalogIndexMethod::btree_ordered,
                                     CatalogIndexPurpose::dependency_source_group,
                                     {Key("source_object_uuid", CatalogColumnRole::object_uuid, true, false, true),
                                      Key("dependency_object_class", CatalogColumnRole::machine_key, true, false, true),
                                      Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                      Key("dependency_object_uuid", CatalogColumnRole::object_uuid, true, true, false)});
    dependencies_source.supports_ordered_scan = true;
    dependencies_source.supports_group_scan = true;
    dependencies_source.supports_prefix_scan = true;
    dependencies_source.supports_catalog_generation_visibility = true;
    push(std::move(dependencies_source));

    auto dependencies_target = Index("sys_catalog_object_dependencies_target_btree",
                                     "sys.catalog.object_dependencies",
                                     CatalogIndexMethod::btree_ordered,
                                     CatalogIndexPurpose::dependency_target_group,
                                     {Key("dependency_object_uuid", CatalogColumnRole::object_uuid, true, false, true),
                                      Key("source_object_class", CatalogColumnRole::machine_key, true, false, true),
                                      Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                      Key("source_object_uuid", CatalogColumnRole::object_uuid, true, true, false)});
    dependencies_target.supports_ordered_scan = true;
    dependencies_target.supports_group_scan = true;
    dependencies_target.supports_prefix_scan = true;
    dependencies_target.supports_catalog_generation_visibility = true;
    push(std::move(dependencies_target));

    auto synonym_uuid = Index("sys_catalog_synonym_uuid_hash",
                              "sys.catalog.synonym",
                              CatalogIndexMethod::hash_equality,
                              CatalogIndexPurpose::uuid_exact_lookup,
                              {Key("synonym_uuid", CatalogColumnRole::object_uuid)});
    synonym_uuid.unique = true;
    synonym_uuid.supports_uuid_exact_lookup = true;
    push(std::move(synonym_uuid));

    auto synonym_parent = Index("sys_catalog_synonym_parent_generation_btree",
                                "sys.catalog.synonym",
                                CatalogIndexMethod::btree_ordered,
                                CatalogIndexPurpose::parent_child_group,
                                {Key("parent_object_uuid", CatalogColumnRole::parent_object_uuid, true, false, true),
                                 Key("target_object_class", CatalogColumnRole::target_object_class, true, false, true),
                                 Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                 Key("synonym_uuid", CatalogColumnRole::object_uuid, true, true, false)});
    synonym_parent.supports_ordered_scan = true;
    synonym_parent.supports_group_scan = true;
    synonym_parent.supports_prefix_scan = true;
    synonym_parent.supports_catalog_generation_visibility = true;
    push(std::move(synonym_parent));

    auto synonym_target = Index("sys_catalog_synonym_target_btree",
                                "sys.catalog.synonym",
                                CatalogIndexMethod::btree_ordered,
                                CatalogIndexPurpose::dependency_target_group,
                                {Key("target_object_uuid", CatalogColumnRole::target_object_uuid, true, false, true),
                                 Key("target_object_class", CatalogColumnRole::target_object_class, true, false, true),
                                 Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                 Key("synonym_uuid", CatalogColumnRole::object_uuid, true, true, false)});
    synonym_target.supports_ordered_scan = true;
    synonym_target.supports_group_scan = true;
    synonym_target.supports_prefix_scan = true;
    synonym_target.supports_catalog_generation_visibility = true;
    push(std::move(synonym_target));

    auto column_uuid = Index("sys_catalog_column_descriptor_uuid_hash",
                             "sys.catalog.column_descriptor",
                             CatalogIndexMethod::hash_equality,
                             CatalogIndexPurpose::uuid_exact_lookup,
                             {Key("column_uuid", CatalogColumnRole::column_uuid)});
    column_uuid.unique = true;
    column_uuid.supports_uuid_exact_lookup = true;
    push(std::move(column_uuid));

    auto column_owner = Index("sys_catalog_column_descriptor_owner_btree",
                              "sys.catalog.column_descriptor",
                              CatalogIndexMethod::btree_ordered,
                              CatalogIndexPurpose::parent_child_group,
                              {Key("owner_object_uuid", CatalogColumnRole::owner_object_uuid, true, false, true),
                               Key("ordinal", CatalogColumnRole::ordinal, true, true, false),
                               Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                               Key("column_uuid", CatalogColumnRole::column_uuid, true, true, false)});
    column_owner.supports_ordered_scan = true;
    column_owner.supports_group_scan = true;
    column_owner.supports_prefix_scan = true;
    column_owner.supports_catalog_generation_visibility = true;
    push(std::move(column_owner));

    auto constraint_uuid = Index("sys_constraint_descriptor_uuid_hash",
                                 "sys.constraint_descriptor",
                                 CatalogIndexMethod::hash_equality,
                                 CatalogIndexPurpose::uuid_exact_lookup,
                                 {Key("constraint_uuid", CatalogColumnRole::constraint_uuid)});
    constraint_uuid.unique = true;
    constraint_uuid.supports_uuid_exact_lookup = true;
    push(std::move(constraint_uuid));

    auto constraint_owner = Index("sys_constraint_descriptor_owner_btree",
                                  "sys.constraint_descriptor",
                                  CatalogIndexMethod::btree_ordered,
                                  CatalogIndexPurpose::constraint_owner_group,
                                  {Key("owner_object_uuid", CatalogColumnRole::owner_object_uuid, true, false, true),
                                   Key("constraint_class", CatalogColumnRole::constraint_class, true, false, true),
                                   Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                   Key("constraint_uuid", CatalogColumnRole::constraint_uuid, true, true, false)});
    constraint_owner.supports_ordered_scan = true;
    constraint_owner.supports_group_scan = true;
    constraint_owner.supports_prefix_scan = true;
    constraint_owner.supports_catalog_generation_visibility = true;
    push(std::move(constraint_owner));

    auto key_uuid = Index("sys_key_descriptor_uuid_hash",
                          "sys.key_descriptor",
                          CatalogIndexMethod::hash_equality,
                          CatalogIndexPurpose::uuid_exact_lookup,
                          {Key("key_descriptor_uuid", CatalogColumnRole::key_descriptor_uuid)});
    key_uuid.unique = true;
    key_uuid.supports_uuid_exact_lookup = true;
    push(std::move(key_uuid));

    auto key_constraint = Index("sys_key_descriptor_constraint_btree",
                                "sys.key_descriptor",
                                CatalogIndexMethod::btree_ordered,
                                CatalogIndexPurpose::constraint_dependency_group,
                                {Key("constraint_uuid", CatalogColumnRole::constraint_uuid, true, false, true),
                                 Key("key_class", CatalogColumnRole::key_class, true, false, true),
                                 Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                 Key("key_descriptor_uuid", CatalogColumnRole::key_descriptor_uuid, true, true, false)});
    key_constraint.supports_ordered_scan = true;
    key_constraint.supports_group_scan = true;
    key_constraint.supports_prefix_scan = true;
    key_constraint.supports_catalog_generation_visibility = true;
    push(std::move(key_constraint));

    auto subject_constraint = Index("sys_constraint_subject_constraint_btree",
                                    "sys.constraint_subject",
                                    CatalogIndexMethod::btree_ordered,
                                    CatalogIndexPurpose::constraint_subject_group,
                                    {Key("constraint_uuid", CatalogColumnRole::constraint_uuid, true, false, true),
                                     Key("ordinal", CatalogColumnRole::subject_ordinal, true, true, false),
                                     Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                     Key("constraint_subject_uuid", CatalogColumnRole::constraint_subject_uuid, true, true, false)});
    subject_constraint.supports_ordered_scan = true;
    subject_constraint.supports_group_scan = true;
    subject_constraint.supports_prefix_scan = true;
    subject_constraint.supports_catalog_generation_visibility = true;
    push(std::move(subject_constraint));

    auto dependency_constraint = Index("sys_constraint_dependency_constraint_btree",
                                       "sys.constraint_dependency",
                                       CatalogIndexMethod::btree_ordered,
                                       CatalogIndexPurpose::constraint_dependency_group,
                                       {Key("constraint_uuid", CatalogColumnRole::constraint_uuid, true, false, true),
                                        Key("dependency_kind", CatalogColumnRole::dependency_kind, true, false, true),
                                        Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                        Key("dependency_uuid", CatalogColumnRole::dependency_uuid, true, true, false)});
    dependency_constraint.supports_ordered_scan = true;
    dependency_constraint.supports_group_scan = true;
    dependency_constraint.supports_prefix_scan = true;
    dependency_constraint.supports_catalog_generation_visibility = true;
    push(std::move(dependency_constraint));

    auto dependency_target = Index("sys_constraint_dependency_target_btree",
                                   "sys.constraint_dependency",
                                   CatalogIndexMethod::btree_ordered,
                                   CatalogIndexPurpose::dependency_target_group,
                                   {Key("dependency_object_uuid", CatalogColumnRole::dependency_object_uuid, true, false, true),
                                    Key("dependency_kind", CatalogColumnRole::dependency_kind, true, false, true),
                                    Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                    Key("constraint_uuid", CatalogColumnRole::constraint_uuid, true, true, false)});
    dependency_target.supports_ordered_scan = true;
    dependency_target.supports_group_scan = true;
    dependency_target.supports_prefix_scan = true;
    dependency_target.supports_catalog_generation_visibility = true;
    push(std::move(dependency_target));

    auto support_binding = Index("sys_constraint_support_structure_binding_hash",
                                 "sys.constraint_support_structure",
                                 CatalogIndexMethod::hash_equality,
                                 CatalogIndexPurpose::uuid_exact_lookup,
                                 {Key("support_binding_uuid", CatalogColumnRole::support_binding_uuid)});
    support_binding.unique = true;
    support_binding.supports_uuid_exact_lookup = true;
    push(std::move(support_binding));

    auto support_constraint = Index("sys_constraint_support_structure_constraint_btree",
                                    "sys.constraint_support_structure",
                                    CatalogIndexMethod::btree_ordered,
                                    CatalogIndexPurpose::constraint_support_group,
                                    {Key("constraint_uuid", CatalogColumnRole::constraint_uuid, true, false, true),
                                     Key("support_family", CatalogColumnRole::support_family, true, false, true),
                                     Key("validity_state", CatalogColumnRole::validity_state, true, false, true),
                                     Key("support_binding_uuid", CatalogColumnRole::support_binding_uuid, true, true, false)});
    support_constraint.supports_ordered_scan = true;
    support_constraint.supports_group_scan = true;
    support_constraint.supports_prefix_scan = true;
    push(std::move(support_constraint));

    auto index_uuid = Index("sys_catalog_index_definitions_uuid_hash",
                            "sys.catalog.index_definitions",
                            CatalogIndexMethod::hash_equality,
                            CatalogIndexPurpose::index_definition_lookup,
                            {Key("index_uuid", CatalogColumnRole::object_uuid)});
    index_uuid.unique = true;
    index_uuid.supports_uuid_exact_lookup = true;
    push(std::move(index_uuid));

    auto table_indexes = Index("sys_catalog_index_definitions_table_generation_btree",
                               "sys.catalog.index_definitions",
                               CatalogIndexMethod::btree_ordered,
                               CatalogIndexPurpose::parent_child_group,
                               {Key("table_object_uuid", CatalogColumnRole::object_uuid, true, false, true),
                                Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, true),
                                Key("history_sequence", CatalogColumnRole::history_sequence, true, true, false),
                                Key("index_uuid", CatalogColumnRole::object_uuid, true, true, false)});
    table_indexes.supports_ordered_scan = true;
    table_indexes.supports_group_scan = true;
    table_indexes.supports_prefix_scan = true;
    table_indexes.supports_catalog_generation_visibility = true;
    push(std::move(table_indexes));

    auto name_lookup = Index("sys_catalog_name_entries_authoritative_lookup_btree",
                             "sys.catalog.object_name_entries",
                             CatalogIndexMethod::btree_ordered,
                             CatalogIndexPurpose::name_resolution_lookup,
                             {Key("scope_uuid", CatalogColumnRole::parent_object_uuid, true, false, true),
                              Key("object_class", CatalogColumnRole::machine_key, true, false, true),
                              Key("identifier_profile_uuid", CatalogColumnRole::resolver_profile, true, false, true),
                              Key("language_tag", CatalogColumnRole::resolver_language, true, false, true),
                              Key("normalized_lookup_key", CatalogColumnRole::resolver_normalized_lookup_key, true, true, false)});
    name_lookup.supports_name_resolution = true;
    name_lookup.supports_ordered_scan = true;
    name_lookup.supports_group_scan = true;
    name_lookup.supports_prefix_scan = true;
    name_lookup.authority_boundary = "identity_resolver";
    push(std::move(name_lookup));

    auto exact_name_lookup = Index("sys_catalog_name_entries_exact_lookup_btree",
                                   "sys.catalog.object_name_entries",
                                   CatalogIndexMethod::btree_ordered,
                                   CatalogIndexPurpose::name_resolution_lookup,
                                   {Key("scope_uuid", CatalogColumnRole::parent_object_uuid, true, false, true),
                                    Key("object_class", CatalogColumnRole::machine_key, true, false, true),
                                    Key("identifier_profile_uuid", CatalogColumnRole::resolver_profile, true, false, true),
                                    Key("language_tag", CatalogColumnRole::resolver_language, true, false, true),
                                    Key("exact_lookup_key", CatalogColumnRole::resolver_exact_lookup_key, true, true, false)});
    exact_name_lookup.supports_name_resolution = true;
    exact_name_lookup.supports_ordered_scan = true;
    exact_name_lookup.supports_group_scan = true;
    exact_name_lookup.supports_prefix_scan = true;
    exact_name_lookup.authority_boundary = "identity_resolver";
    push(std::move(exact_name_lookup));

    auto full_path = Index("sys_catalog_name_entries_full_path_accelerator_btree",
                           "sys.catalog.object_name_entries",
                           CatalogIndexMethod::btree_ordered,
                           CatalogIndexPurpose::full_path_name_accelerator,
                           {Key("scope_uuid", CatalogColumnRole::parent_object_uuid, true, false, true),
                            Key("identifier_profile_uuid", CatalogColumnRole::resolver_profile, true, false, true),
                            Key("language_tag", CatalogColumnRole::resolver_language, true, false, true),
                            Key("full_path_lookup_key", CatalogColumnRole::resolver_full_path_lookup_key, true, true, false),
                            Key("object_class", CatalogColumnRole::machine_key, true, true, false)});
    full_path.authoritative = false;
    full_path.supports_name_resolution = true;
    full_path.supports_ordered_scan = true;
    full_path.supports_group_scan = true;
    full_path.supports_prefix_scan = true;
    full_path.authority_boundary = "identity_resolver_accelerator";
    push(std::move(full_path));

    auto uuid_to_name = Index("sys_catalog_name_entries_uuid_language_btree",
                              "sys.catalog.object_name_entries",
                              CatalogIndexMethod::btree_ordered,
                              CatalogIndexPurpose::uuid_to_name_language_fallback,
                              {Key("object_uuid", CatalogColumnRole::object_uuid, true, false, true),
                               Key("object_class", CatalogColumnRole::machine_key, true, false, true),
                               Key("language_tag", CatalogColumnRole::resolver_language, true, false, true),
                               Key("name_class", CatalogColumnRole::resolver_name_class, true, true, false),
                               Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, false)});
    uuid_to_name.supports_name_resolution = true;
    uuid_to_name.supports_ordered_scan = true;
    uuid_to_name.supports_group_scan = true;
    uuid_to_name.supports_prefix_scan = true;
    uuid_to_name.supports_catalog_generation_visibility = true;
    uuid_to_name.authority_boundary = "identity_resolver";
    push(std::move(uuid_to_name));

    auto comments = Index("sys_catalog_object_comments_language_btree",
                          "sys.catalog.object_comments",
                          CatalogIndexMethod::btree_ordered,
                          CatalogIndexPurpose::comment_language_fallback,
                          {Key("object_uuid", CatalogColumnRole::object_uuid, true, false, true),
                           Key("object_class", CatalogColumnRole::machine_key, true, false, true),
                           Key("language_tag", CatalogColumnRole::resolver_language, true, false, true),
                           Key("catalog_generation_id", CatalogColumnRole::catalog_generation_id, true, true, false)});
    comments.supports_ordered_scan = true;
    comments.supports_group_scan = true;
    comments.supports_prefix_scan = true;
    comments.supports_catalog_generation_visibility = true;
    comments.authority_boundary = "comment_resolver";
    push(std::move(comments));

    return out;
  }();
  return profiles;
}

const CatalogTableProfile* FindCatalogTableProfile(std::string_view table_path) {
  for (const auto& table : BuiltinCatalogTableProfiles()) {
    if (std::string_view(table.table_path) == table_path) { return &table; }
  }
  return nullptr;
}

const CatalogPhysicalIndexProfile* FindCatalogIndexProfile(std::string_view index_name) {
  for (const auto& profile : BuiltinCatalogIndexProfiles()) {
    if (std::string_view(profile.index_name) == index_name) { return &profile; }
  }
  return nullptr;
}

std::vector<CatalogPhysicalIndexProfile> CatalogIndexProfilesForTable(std::string_view table_path) {
  std::vector<CatalogPhysicalIndexProfile> result;
  for (const auto& profile : BuiltinCatalogIndexProfiles()) {
    if (std::string_view(profile.table_path) == table_path) { result.push_back(profile); }
  }
  return result;
}

bool CatalogPathIsClusterScoped(std::string_view path) {
  return StartsWith(path, "cluster.") || path == "cluster";
}

CatalogIndexValidationResult ValidateCatalogPathForLocalCatalog(std::string_view path) {
  CatalogIndexValidationResult result;
  if (path.empty()) {
    AddIssue(&result, "CATALOG.INDEX.PATH_REQUIRED", "path");
    return result;
  }
  if (CatalogPathIsClusterScoped(path)) {
    AddIssue(&result, "CATALOG.INDEX.CLUSTER_SCOPE_FORBIDDEN", std::string(path));
  }
  return result;
}

bool CatalogColumnRoleContainsHumanNameText(CatalogColumnRole role) {
  return role == CatalogColumnRole::resolver_raw_name ||
         role == CatalogColumnRole::resolver_display_name ||
         role == CatalogColumnRole::resolver_normalized_lookup_key ||
         role == CatalogColumnRole::resolver_exact_lookup_key ||
         role == CatalogColumnRole::resolver_full_path_lookup_key;
}

bool CatalogTableSurfaceAllowsHumanNameText(CatalogTableSurface surface) {
  return surface == CatalogTableSurface::identity_resolver;
}

bool CatalogTableProfileContainsHumanNameText(const CatalogTableProfile& table) {
  for (const auto& column : table.columns) {
    if (CatalogColumnRoleContainsHumanNameText(column.role)) { return true; }
  }
  return false;
}

bool CatalogIndexProfileHasOrderedNeed(const CatalogPhysicalIndexProfile& profile) {
  return profile.supports_ordered_scan || profile.supports_group_scan || profile.supports_prefix_scan ||
         profile.supports_catalog_generation_visibility || profile.supports_transaction_history;
}

CatalogIndexValidationResult ValidateCatalogTableProfile(const CatalogTableProfile& table) {
  CatalogIndexValidationResult result;
  if (table.table_path.empty()) {
    AddIssue(&result, "CATALOG.TABLE.PATH_REQUIRED", "table_path");
  }
  if (CatalogPathIsClusterScoped(table.table_path)) {
    AddIssue(&result, "CATALOG.TABLE.CLUSTER_SCOPE_FORBIDDEN", table.table_path);
  }
  if (!table.cluster_path_fail_closed) {
    AddIssue(&result, "CATALOG.TABLE.CLUSTER_FAIL_CLOSED_REQUIRED", table.table_path);
  }
  if (table.record_kind == CatalogRecordKind::unknown || table.scope == CatalogRecordScope::unknown) {
    AddIssue(&result, "CATALOG.TABLE.AUTHORITY_INCOMPLETE", table.table_path);
  }
  if (table.surface == CatalogTableSurface::base_catalog && table.parser_visible) {
    AddIssue(&result, "CATALOG.TABLE.BASE_PARSER_VISIBLE", table.table_path);
  }
  if (!CatalogTableSurfaceAllowsHumanNameText(table.surface) &&
      CatalogTableProfileContainsHumanNameText(table)) {
    AddIssue(&result, "CATALOG.TABLE.HUMAN_NAME_DUPLICATION", table.table_path);
  }
  for (const auto& column : table.columns) {
    if (column.column_name.empty()) {
      AddIssue(&result, "CATALOG.TABLE.COLUMN_NAME_REQUIRED", table.table_path);
    }
  }
  return result;
}

CatalogIndexValidationResult ValidateCatalogPhysicalIndexProfile(const CatalogPhysicalIndexProfile& profile) {
  CatalogIndexValidationResult result;
  if (profile.index_name.empty()) {
    AddIssue(&result, "CATALOG.INDEX.NAME_REQUIRED", "index_name");
  }
  if (profile.table_path.empty()) {
    AddIssue(&result, "CATALOG.INDEX.TABLE_REQUIRED", profile.index_name);
  }
  if (profile.key_columns.empty()) {
    AddIssue(&result, "CATALOG.INDEX.KEY_REQUIRED", profile.index_name);
  }
  if (CatalogPathIsClusterScoped(profile.table_path)) {
    AddIssue(&result, "CATALOG.INDEX.CLUSTER_SCOPE_FORBIDDEN", profile.table_path);
  }
  if (!profile.cluster_path_fail_closed) {
    AddIssue(&result, "CATALOG.INDEX.CLUSTER_FAIL_CLOSED_REQUIRED", profile.index_name);
  }
  const auto* table = FindCatalogTableProfile(profile.table_path);
  if (table == nullptr) {
    AddIssue(&result, "CATALOG.INDEX.TABLE_UNKNOWN", profile.table_path);
  }

  if (profile.method == CatalogIndexMethod::hash_equality) {
    if (profile.supports_ordered_scan || profile.supports_group_scan || profile.supports_prefix_scan ||
        profile.supports_catalog_generation_visibility || profile.supports_transaction_history ||
        profile.supports_name_resolution) {
      AddIssue(&result, "CATALOG.INDEX.HASH_ORDERED_CAPABILITY_FORBIDDEN", profile.index_name);
    }
    for (const auto& key : profile.key_columns) {
      if (!key.equality_component || key.ordered_component || key.prefix_component) {
        AddIssue(&result, "CATALOG.INDEX.HASH_KEY_NOT_EQUALITY_ONLY",
                 profile.index_name + ":" + key.column_name);
      }
    }
  }

  if (profile.method == CatalogIndexMethod::btree_ordered &&
      !CatalogIndexProfileHasOrderedNeed(profile)) {
    AddIssue(&result, "CATALOG.INDEX.BTREE_WITHOUT_ORDERED_NEED", profile.index_name);
  }

  if (profile.supports_uuid_exact_lookup) {
    if (profile.method != CatalogIndexMethod::hash_equality) {
      AddIssue(&result, "CATALOG.INDEX.UUID_EXACT_REQUIRES_HASH", profile.index_name);
    }
    if (profile.key_columns.size() != 1 || !KeyUsesUuidExactRole(profile.key_columns.front().role)) {
      AddIssue(&result, "CATALOG.INDEX.UUID_EXACT_KEY_INVALID", profile.index_name);
    }
  }

  if (profile.supports_name_resolution) {
    if (table == nullptr || table->surface != CatalogTableSurface::identity_resolver) {
      AddIssue(&result, "CATALOG.INDEX.NAME_RESOLUTION_TABLE_INVALID", profile.index_name);
    }
    if (profile.authority_boundary != "identity_resolver" &&
        profile.authority_boundary != "identity_resolver_accelerator") {
      AddIssue(&result, "CATALOG.INDEX.NAME_RESOLUTION_BOUNDARY_REQUIRED", profile.index_name);
    }
  }

  for (const auto& key : profile.key_columns) {
    if (key.column_name.empty()) {
      AddIssue(&result, "CATALOG.INDEX.KEY_COLUMN_REQUIRED", profile.index_name);
    }
    if (profile.method == CatalogIndexMethod::btree_ordered &&
        key.ordered_component &&
        !(profile.supports_ordered_scan || profile.supports_catalog_generation_visibility ||
          profile.supports_transaction_history)) {
      AddIssue(&result, "CATALOG.INDEX.ORDERED_KEY_WITHOUT_ORDERED_CAPABILITY",
               profile.index_name + ":" + key.column_name);
    }
  }

  return result;
}

CatalogIndexValidationResult ValidateBuiltinCatalogIndexProfiles() {
  CatalogIndexValidationResult result;
  for (const auto& table : BuiltinCatalogTableProfiles()) {
    const auto table_result = ValidateCatalogTableProfile(table);
    for (const auto& issue : table_result.issues) {
      AddIssue(&result, issue.code, issue.detail, issue.error);
    }
  }
  for (const auto& profile : BuiltinCatalogIndexProfiles()) {
    const auto index_result = ValidateCatalogPhysicalIndexProfile(profile);
    for (const auto& issue : index_result.issues) {
      AddIssue(&result, issue.code, issue.detail, issue.error);
    }
  }

  const auto has_object_uuid_hash =
      FindCatalogIndexProfile("sys_catalog_object_identity_uuid_hash") != nullptr;
  if (!has_object_uuid_hash) {
    AddIssue(&result, "CATALOG.INDEX.UUID_HASH_PROFILE_MISSING", "sys.catalog.object_identity");
  }

  const auto has_resolver =
      FindCatalogIndexProfile("sys_catalog_name_entries_authoritative_lookup_btree") != nullptr;
  if (!has_resolver) {
    AddIssue(&result, "CATALOG.INDEX.RESOLVER_PROFILE_MISSING", "sys.catalog.object_name_entries");
  }

  const auto has_synonym_uuid =
      FindCatalogIndexProfile("sys_catalog_synonym_uuid_hash") != nullptr;
  if (!has_synonym_uuid) {
    AddIssue(&result, "CATALOG.INDEX.SYNONYM_UUID_PROFILE_MISSING", "sys.catalog.synonym");
  }

  if (FindCatalogTableProfile("sys.constraint_descriptor") == nullptr ||
      FindCatalogTableProfile("sys.key_descriptor") == nullptr ||
      FindCatalogTableProfile("sys.constraint_subject") == nullptr ||
      FindCatalogTableProfile("sys.constraint_dependency") == nullptr ||
      FindCatalogTableProfile("sys.constraint_support_structure") == nullptr) {
    AddIssue(&result, "CATALOG.INDEX.CONSTRAINT_TABLE_PROFILE_MISSING", "sys.constraint_*");
  }
  if (FindCatalogIndexProfile("sys_constraint_descriptor_uuid_hash") == nullptr ||
      FindCatalogIndexProfile("sys_key_descriptor_uuid_hash") == nullptr ||
      FindCatalogIndexProfile("sys_constraint_support_structure_binding_hash") == nullptr) {
    AddIssue(&result, "CATALOG.INDEX.CONSTRAINT_UUID_PROFILE_MISSING", "sys.constraint_*");
  }

  return result;
}

}  // namespace scratchbird::core::catalog
