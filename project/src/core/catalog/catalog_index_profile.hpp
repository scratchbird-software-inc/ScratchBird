// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CATALOG-INDEX-PROFILE-ANCHOR
#include "catalog_records.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::catalog {

enum class CatalogTableSurface : std::uint16_t {
  base_catalog,
  identity_resolver,
  comment_resolver,
  information_projection,
};

enum class CatalogColumnRole : std::uint16_t {
  row_uuid,
  object_uuid,
  parent_object_uuid,
  schema_uuid,
  descriptor_uuid,
  transaction_uuid,
  transaction_id,
  catalog_generation_id,
  history_sequence,
  lifecycle_state,
  machine_key,
  payload_hash,
  security_policy_uuid,
  security_epoch,
  resource_epoch,
  ordinal,
  resolver_language,
  resolver_profile,
  resolver_name_class,
  resolver_raw_name,
  resolver_display_name,
  resolver_normalized_lookup_key,
  resolver_exact_lookup_key,
  resolver_full_path_lookup_key,
  resolver_comment_text,
  object_class,
  target_object_uuid,
  target_object_class,
  owner_uuid,
  dependency_strength,
  constraint_uuid,
  constraint_class,
  owner_object_uuid,
  constraint_policy_version_uuid,
  enforcement_timing,
  validation_state,
  trust_state,
  support_requirement,
  predicate_sblr_uuid,
  diagnostic_profile_uuid,
  metrics_profile_uuid,
  conformance_profile_uuid,
  descriptor_hash,
  key_descriptor_uuid,
  key_class,
  component_order_hash,
  comparison_profile_hash,
  null_policy,
  canonical_encoding_uuid,
  constraint_subject_uuid,
  subject_object_uuid,
  subject_kind,
  subject_ordinal,
  expression_sblr_uuid,
  dependency_uuid,
  dependency_kind,
  dependency_object_uuid,
  dependency_version_uuid,
  invalidation_action,
  support_binding_uuid,
  support_uuid,
  support_class,
  support_family,
  coverage_scope_hash,
  durability_class,
  residency_class,
  validity_state,
  enforcement_role,
  column_uuid,
  default_expression_envelope,
};

struct CatalogTableColumnProfile {
  std::string column_name;
  CatalogColumnRole role = CatalogColumnRole::machine_key;
  bool persisted = true;
  bool nullable = false;
};

struct CatalogTableProfile {
  std::string table_path;
  CatalogRecordKind record_kind = CatalogRecordKind::unknown;
  CatalogRecordScope scope = CatalogRecordScope::unknown;
  CatalogTableSurface surface = CatalogTableSurface::base_catalog;
  std::vector<CatalogTableColumnProfile> columns;
  bool local_only = true;
  bool parser_visible = false;
  bool cluster_path_fail_closed = true;
};

enum class CatalogIndexMethod : std::uint16_t {
  hash_equality,
  btree_ordered,
};

enum class CatalogIndexPurpose : std::uint16_t {
  uuid_exact_lookup,
  row_uuid_exact_lookup,
  parent_child_group,
  catalog_generation_visibility,
  transaction_history,
  object_history,
  dependency_source_group,
  dependency_target_group,
  constraint_owner_group,
  constraint_subject_group,
  constraint_dependency_group,
  constraint_support_group,
  name_resolution_lookup,
  full_path_name_accelerator,
  uuid_to_name_language_fallback,
  comment_language_fallback,
  index_definition_lookup,
};

struct CatalogIndexKeyColumnProfile {
  std::string column_name;
  CatalogColumnRole role = CatalogColumnRole::machine_key;
  bool equality_component = true;
  bool ordered_component = false;
  bool prefix_component = false;
};

struct CatalogPhysicalIndexProfile {
  std::string index_name;
  std::string table_path;
  CatalogIndexMethod method = CatalogIndexMethod::hash_equality;
  CatalogIndexPurpose purpose = CatalogIndexPurpose::uuid_exact_lookup;
  std::vector<CatalogIndexKeyColumnProfile> key_columns;
  bool unique = false;
  bool authoritative = true;
  bool supports_uuid_exact_lookup = false;
  bool supports_name_resolution = false;
  bool supports_ordered_scan = false;
  bool supports_group_scan = false;
  bool supports_prefix_scan = false;
  bool supports_catalog_generation_visibility = false;
  bool supports_transaction_history = false;
  bool supports_mga_snapshot_visibility = true;
  bool cluster_path_fail_closed = true;
  std::string authority_boundary;
};

struct CatalogIndexValidationIssue {
  std::string code;
  std::string detail;
  bool error = true;
};

struct CatalogIndexValidationResult {
  bool ok = true;
  std::vector<CatalogIndexValidationIssue> issues;
};

const char* CatalogTableSurfaceName(CatalogTableSurface surface);
const char* CatalogColumnRoleName(CatalogColumnRole role);
const char* CatalogIndexMethodName(CatalogIndexMethod method);
const char* CatalogIndexPurposeName(CatalogIndexPurpose purpose);

const std::vector<CatalogTableProfile>& BuiltinCatalogTableProfiles();
const std::vector<CatalogPhysicalIndexProfile>& BuiltinCatalogIndexProfiles();

const CatalogTableProfile* FindCatalogTableProfile(std::string_view table_path);
const CatalogPhysicalIndexProfile* FindCatalogIndexProfile(std::string_view index_name);
std::vector<CatalogPhysicalIndexProfile> CatalogIndexProfilesForTable(std::string_view table_path);

bool CatalogPathIsClusterScoped(std::string_view path);
CatalogIndexValidationResult ValidateCatalogPathForLocalCatalog(std::string_view path);
bool CatalogColumnRoleContainsHumanNameText(CatalogColumnRole role);
bool CatalogTableSurfaceAllowsHumanNameText(CatalogTableSurface surface);
bool CatalogTableProfileContainsHumanNameText(const CatalogTableProfile& table);
bool CatalogIndexProfileHasOrderedNeed(const CatalogPhysicalIndexProfile& profile);

CatalogIndexValidationResult ValidateCatalogTableProfile(const CatalogTableProfile& table);
CatalogIndexValidationResult ValidateCatalogPhysicalIndexProfile(const CatalogPhysicalIndexProfile& profile);
CatalogIndexValidationResult ValidateBuiltinCatalogIndexProfiles();

}  // namespace scratchbird::core::catalog
