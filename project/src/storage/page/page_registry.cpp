// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_registry.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::storage::disk::PageClassificationKindName;
using scratchbird::storage::disk::PageTypeName;
using scratchbird::storage::filespace::FilespaceRole;

bool StatusAllowsProductSupport(PageRegistryStatus status) {
  return status == PageRegistryStatus::selected_current ||
         status == PageRegistryStatus::compatibility_transition ||
         status == PageRegistryStatus::implemented;
}

Status PageRegistryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status PageRegistryWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::storage_page};
}

Status PageRegistryErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

PageFamilyDescriptor Descriptor(PageType page_type,
                                PageFamily family,
                                PageRegistryStatus registry_status,
                                std::string stable_name,
                                bool read,
                                bool write,
                                std::vector<FilespaceRole> roles,
                                std::string implementation_search_key,
                                bool cluster_only = false,
                                bool encrypted_or_opaque = false,
                                bool uses_index_special_header = false,
                                bool typed_payload_dependency_required = false,
                                bool resource_dependency_required = false) {
  PageFamilyDescriptor descriptor;
  descriptor.page_type = page_type;
  descriptor.family = family;
  descriptor.registry_status = registry_status;
  descriptor.stable_name = std::move(stable_name);
  descriptor.supported_local_read = read;
  descriptor.supported_local_write = write;
  descriptor.cluster_only = cluster_only;
  descriptor.encrypted_or_opaque = encrypted_or_opaque;
  descriptor.reserved = !StatusAllowsProductSupport(registry_status);
  descriptor.uses_index_special_header = uses_index_special_header;
  descriptor.typed_payload_dependency_required = typed_payload_dependency_required;
  descriptor.resource_dependency_required = resource_dependency_required;
  descriptor.legal_filespace_roles = std::move(roles);
  descriptor.implementation_search_key = std::move(implementation_search_key);
  return descriptor;
}

std::vector<FilespaceRole> Roles(std::initializer_list<FilespaceRole> roles) {
  return {roles};
}

std::vector<FilespaceRole> PrimaryRoles() {
  return Roles({FilespaceRole::active_primary,
                FilespaceRole::primary_shadow,
                FilespaceRole::primary_snapshot,
                FilespaceRole::primary_candidate});
}

std::vector<FilespaceRole> DataRoles() {
  return Roles({FilespaceRole::active_primary,
                FilespaceRole::primary_shadow,
                FilespaceRole::primary_snapshot,
                FilespaceRole::primary_candidate,
                FilespaceRole::secondary_data,
                FilespaceRole::secondary_overflow,
                FilespaceRole::secondary_history,
                FilespaceRole::secondary_shard});
}

std::vector<FilespaceRole> IndexRoles() {
  return Roles({FilespaceRole::active_primary,
                FilespaceRole::secondary_data,
                FilespaceRole::secondary_index,
                FilespaceRole::secondary_shard,
                FilespaceRole::temporary});
}

std::vector<FilespaceRole> ArchiveRoles() {
  return Roles({FilespaceRole::archive_history,
                FilespaceRole::archive_log,
                FilespaceRole::archive_detached});
}

std::vector<FilespaceRole> ImportRoles() {
  return Roles({FilespaceRole::import_candidate,
                FilespaceRole::archive_detached,
                FilespaceRole::forbidden});
}

std::vector<FilespaceRole> TemporaryRoles() {
  return Roles({FilespaceRole::temporary});
}

std::vector<FilespaceRole> AllDurableRoles() {
  return Roles({FilespaceRole::active_primary,
                FilespaceRole::primary_shadow,
                FilespaceRole::primary_snapshot,
                FilespaceRole::primary_candidate,
                FilespaceRole::secondary_data,
                FilespaceRole::secondary_index,
                FilespaceRole::secondary_overflow,
                FilespaceRole::secondary_history,
                FilespaceRole::secondary_shard,
                FilespaceRole::archive_history,
                FilespaceRole::archive_log,
                FilespaceRole::archive_detached,
                FilespaceRole::import_candidate});
}

}  // namespace

const char* PageRegistryStatusName(PageRegistryStatus status) {
  switch (status) {
    case PageRegistryStatus::selected_current: return "selected_current";
    case PageRegistryStatus::compatibility_transition: return "compatibility_transition";
    case PageRegistryStatus::implemented: return "implemented";
    case PageRegistryStatus::partially_implemented: return "partially_implemented";
    case PageRegistryStatus::reserved: return "reserved";
    case PageRegistryStatus::deferred: return "deferred";
    case PageRegistryStatus::superseded: return "superseded";
    case PageRegistryStatus::disabled: return "disabled";
    case PageRegistryStatus::quarantined: return "quarantined";
  }
  return "unknown";
}

bool PageRegistryStatusAllowsProductSupport(PageRegistryStatus status) {
  return StatusAllowsProductSupport(status);
}

const char* PageFamilyName(PageFamily family) {
  switch (family) {
    case PageFamily::startup: return "startup";
    case PageFamily::filespace_control: return "filespace_control";
    case PageFamily::allocation: return "allocation";
    case PageFamily::catalog: return "catalog";
    case PageFamily::transaction: return "transaction";
    case PageFamily::data: return "data";
    case PageFamily::index: return "index";
    case PageFamily::blob: return "blob";
    case PageFamily::metrics: return "metrics";
    case PageFamily::archive: return "archive";
    case PageFamily::columnar: return "columnar";
    case PageFamily::vector: return "vector";
    case PageFamily::graph: return "graph";
    case PageFamily::shard_placement: return "shard_placement";
    case PageFamily::cluster_private: return "cluster_private";
    case PageFamily::encrypted_or_opaque: return "encrypted_or_opaque";
    case PageFamily::import_export: return "import_export";
    case PageFamily::protected_material: return "protected_material";
    case PageFamily::audit: return "audit";
    case PageFamily::repair: return "repair";
    case PageFamily::typed_dependency: return "typed_dependency";
    case PageFamily::superseded: return "superseded";
    case PageFamily::reserved: return "reserved";
    case PageFamily::unknown: return "unknown";
  }
  return "unknown";
}

bool IsKnownPageFamilyName(const std::string& stable_name) {
  if (stable_name.empty()) {
    return false;
  }
  for (const PageFamilyDescriptor& descriptor : BuiltinPageFamilyRegistry()) {
    if (descriptor.stable_name == stable_name || PageFamilyName(descriptor.family) == stable_name) {
      return descriptor.supported_local_read || descriptor.supported_local_write || descriptor.reserved;
    }
  }
  return false;
}

const std::vector<PageFamilyDescriptor>& BuiltinPageFamilyRegistry() {
  static const std::vector<PageFamilyDescriptor> registry = {
      Descriptor(PageType::database_header, PageFamily::startup, PageRegistryStatus::implemented, "database_header", true, true, PrimaryRoles(), "PAGE-REG-IMPLEMENTED-DATABASE-HEADER"),
      Descriptor(PageType::allocation_map, PageFamily::allocation, PageRegistryStatus::implemented, "allocation_map", true, true, AllDurableRoles(), "PAGE-REG-IMPLEMENTED-ALLOCATION-MAP"),
      Descriptor(PageType::catalog, PageFamily::catalog, PageRegistryStatus::implemented, "catalog", true, true, PrimaryRoles(), "PAGE-REG-IMPLEMENTED-CATALOG"),
      Descriptor(PageType::transaction_inventory, PageFamily::transaction, PageRegistryStatus::implemented, "transaction_inventory", true, true, PrimaryRoles(), "PAGE-REG-IMPLEMENTED-TRANSACTION-INVENTORY"),
      Descriptor(PageType::row_data, PageFamily::data, PageRegistryStatus::implemented, "row_data", true, true, DataRoles(), "PAGE-REG-IMPLEMENTED-ROW-DATA"),
      Descriptor(PageType::index_btree, PageFamily::index, PageRegistryStatus::implemented, "index_btree", true, true, IndexRoles(), "PAGE-REG-IMPLEMENTED-BTREE-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_btree_root, PageFamily::index, PageRegistryStatus::implemented, "index_btree_root", true, true, IndexRoles(), "PAGE-REG-IMPLEMENTED-BTREE-ROOT", false, false, true, true, true),
      Descriptor(PageType::index_btree_branch, PageFamily::index, PageRegistryStatus::implemented, "index_btree_branch", true, true, IndexRoles(), "PAGE-REG-IMPLEMENTED-BTREE-BRANCH", false, false, true, true, true),
      Descriptor(PageType::index_btree_leaf, PageFamily::index, PageRegistryStatus::implemented, "index_btree_leaf", true, true, IndexRoles(), "PAGE-REG-IMPLEMENTED-BTREE-LEAF", false, false, true, true, true),
      Descriptor(PageType::index_btree_posting, PageFamily::index, PageRegistryStatus::implemented, "index_btree_posting", true, true, IndexRoles(), "PAGE-REG-IMPLEMENTED-BTREE-POSTING", false, false, true, true, true),
      Descriptor(PageType::index_hash, PageFamily::index, PageRegistryStatus::partially_implemented, "index_hash", true, false, IndexRoles(), "PAGE-REG-PARTIAL-HASH-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_bitmap, PageFamily::index, PageRegistryStatus::partially_implemented, "index_bitmap", true, false, IndexRoles(), "PAGE-REG-PARTIAL-BITMAP-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_summary, PageFamily::index, PageRegistryStatus::partially_implemented, "index_summary", true, false, IndexRoles(), "PAGE-REG-PARTIAL-SUMMARY-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_inverted, PageFamily::index, PageRegistryStatus::partially_implemented, "index_inverted", true, false, IndexRoles(), "PAGE-REG-PARTIAL-INVERTED-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_spatial, PageFamily::index, PageRegistryStatus::partially_implemented, "index_spatial", true, false, IndexRoles(), "PAGE-REG-PARTIAL-SPATIAL-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_vector, PageFamily::index, PageRegistryStatus::partially_implemented, "index_vector", true, false, IndexRoles(), "PAGE-REG-PARTIAL-VECTOR-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_graph, PageFamily::index, PageRegistryStatus::partially_implemented, "index_graph", true, false, IndexRoles(), "PAGE-REG-PARTIAL-GRAPH-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_temporary, PageFamily::index, PageRegistryStatus::partially_implemented, "index_temporary", true, false, TemporaryRoles(), "PAGE-REG-PARTIAL-TEMPORARY-INDEX", false, false, true, true, true),
      Descriptor(PageType::index_statistics, PageFamily::index, PageRegistryStatus::partially_implemented, "index_statistics", true, false, IndexRoles(), "PAGE-REG-PARTIAL-INDEX-STATISTICS", false, false, true, true, true),
      Descriptor(PageType::index_special_root, PageFamily::index, PageRegistryStatus::partially_implemented, "index_special_root", true, false, IndexRoles(), "PAGE-REG-PARTIAL-INDEX-SPECIAL-ROOT", false, false, true, true, true),
      Descriptor(PageType::blob, PageFamily::blob, PageRegistryStatus::implemented, "blob", true, true, DataRoles(), "PAGE-REG-IMPLEMENTED-BLOB"),
      Descriptor(PageType::metrics, PageFamily::metrics, PageRegistryStatus::implemented, "metrics", true, true, PrimaryRoles(), "PAGE-REG-IMPLEMENTED-METRICS"),
      Descriptor(PageType::archive, PageFamily::archive, PageRegistryStatus::partially_implemented, "archive", true, false, ArchiveRoles(), "PAGE-REG-PARTIAL-ARCHIVE"),
      Descriptor(PageType::columnar, PageFamily::columnar, PageRegistryStatus::partially_implemented, "columnar", true, false, DataRoles(), "PAGE-REG-PARTIAL-COLUMNAR", false, false, false, true, true),
      Descriptor(PageType::vector, PageFamily::vector, PageRegistryStatus::partially_implemented, "vector", true, false, DataRoles(), "PAGE-REG-PARTIAL-VECTOR", false, false, false, true, true),
      Descriptor(PageType::graph, PageFamily::graph, PageRegistryStatus::partially_implemented, "graph", true, false, DataRoles(), "PAGE-REG-PARTIAL-GRAPH", false, false, false, true, true),
      Descriptor(PageType::system_state, PageFamily::startup, PageRegistryStatus::implemented, "system_state", true, true, PrimaryRoles(), "PAGE-REG-IMPLEMENTED-SYSTEM-STATE"),
      Descriptor(PageType::bootstrap_reserved, PageFamily::startup, PageRegistryStatus::reserved, "bootstrap_reserved", true, false, PrimaryRoles(), "PAGE-REG-RESERVED-BOOTSTRAP"),
      Descriptor(PageType::filespace_directory, PageFamily::filespace_control, PageRegistryStatus::implemented, "filespace_directory", true, true, PrimaryRoles(), "PAGE-REG-IMPLEMENTED-FILESPACE-DIRECTORY"),
      Descriptor(PageType::config_root, PageFamily::catalog, PageRegistryStatus::implemented, "config_root", true, true, PrimaryRoles(), "PAGE-REG-IMPLEMENTED-CONFIG-ROOT"),
      Descriptor(PageType::security_root, PageFamily::catalog, PageRegistryStatus::implemented, "security_root", true, true, PrimaryRoles(), "PAGE-REG-IMPLEMENTED-SECURITY-ROOT"),
      Descriptor(PageType::filespace_lifecycle_state, PageFamily::filespace_control, PageRegistryStatus::reserved, "filespace_lifecycle_state", false, false, AllDurableRoles(), "PAGE-REG-RESERVED-FILESPACE-LIFECYCLE-STATE"),
      Descriptor(PageType::filespace_operation_record, PageFamily::filespace_control, PageRegistryStatus::reserved, "filespace_operation_record", false, false, AllDurableRoles(), "PAGE-REG-RESERVED-FILESPACE-OPERATION-RECORD"),
      Descriptor(PageType::filespace_prealloc_map, PageFamily::filespace_control, PageRegistryStatus::reserved, "filespace_prealloc_map", false, false, DataRoles(), "PAGE-REG-RESERVED-FILESPACE-PREALLOC-MAP"),
      Descriptor(PageType::filespace_quarantine_fence, PageFamily::filespace_control, PageRegistryStatus::reserved, "filespace_quarantine_fence", false, false, ImportRoles(), "PAGE-REG-RESERVED-FILESPACE-QUARANTINE-FENCE"),
      Descriptor(PageType::shard_placement_map, PageFamily::shard_placement, PageRegistryStatus::reserved, "shard_placement_map", false, false, Roles({FilespaceRole::secondary_shard}), "PAGE-REG-RESERVED-SHARD-PLACEMENT-MAP", true),
      Descriptor(PageType::shard_extent_map, PageFamily::shard_placement, PageRegistryStatus::reserved, "shard_extent_map", false, false, Roles({FilespaceRole::secondary_shard}), "PAGE-REG-RESERVED-SHARD-EXTENT-MAP", true),
      Descriptor(PageType::shard_operation_state, PageFamily::shard_placement, PageRegistryStatus::reserved, "shard_operation_state", false, false, Roles({FilespaceRole::secondary_shard}), "PAGE-REG-RESERVED-SHARD-OPERATION-STATE", true),
      Descriptor(PageType::cluster_member_placement, PageFamily::shard_placement, PageRegistryStatus::reserved, "cluster_member_placement", false, false, Roles({FilespaceRole::secondary_shard}), "PAGE-REG-RESERVED-CLUSTER-MEMBER-PLACEMENT", true),
      Descriptor(PageType::archive_manifest, PageFamily::import_export, PageRegistryStatus::reserved, "archive_manifest", false, false, ArchiveRoles(), "PAGE-REG-RESERVED-ARCHIVE-MANIFEST"),
      Descriptor(PageType::foreign_import_manifest, PageFamily::import_export, PageRegistryStatus::reserved, "foreign_import_manifest", false, false, ImportRoles(), "PAGE-REG-RESERVED-FOREIGN-IMPORT-MANIFEST"),
      Descriptor(PageType::export_package_manifest, PageFamily::import_export, PageRegistryStatus::reserved, "export_package_manifest", false, false, ArchiveRoles(), "PAGE-REG-RESERVED-EXPORT-PACKAGE-MANIFEST"),
      Descriptor(PageType::import_reconciliation_map, PageFamily::import_export, PageRegistryStatus::reserved, "import_reconciliation_map", false, false, ImportRoles(), "PAGE-REG-RESERVED-IMPORT-RECONCILIATION-MAP"),
      Descriptor(PageType::uuid_remap_conflict, PageFamily::import_export, PageRegistryStatus::reserved, "uuid_remap_conflict", false, false, ImportRoles(), "PAGE-REG-RESERVED-UUID-REMAP-CONFLICT"),
      Descriptor(PageType::snapshot_manifest, PageFamily::import_export, PageRegistryStatus::reserved, "snapshot_manifest", false, false, PrimaryRoles(), "PAGE-REG-RESERVED-SNAPSHOT-MANIFEST"),
      Descriptor(PageType::shadow_manifest, PageFamily::import_export, PageRegistryStatus::reserved, "shadow_manifest", false, false, PrimaryRoles(), "PAGE-REG-RESERVED-SHADOW-MANIFEST"),
      Descriptor(PageType::protected_material_root, PageFamily::protected_material, PageRegistryStatus::reserved, "protected_material_root", false, false, PrimaryRoles(), "PAGE-REG-RESERVED-PROTECTED-MATERIAL-ROOT", false, true),
      Descriptor(PageType::protected_material_version, PageFamily::protected_material, PageRegistryStatus::reserved, "protected_material_version", false, false, PrimaryRoles(), "PAGE-REG-RESERVED-PROTECTED-MATERIAL-VERSION", false, true),
      Descriptor(PageType::protected_material_chunk, PageFamily::protected_material, PageRegistryStatus::reserved, "protected_material_chunk", false, false, PrimaryRoles(), "PAGE-REG-RESERVED-PROTECTED-MATERIAL-CHUNK", false, true, false, true, true),
      Descriptor(PageType::protected_material_policy, PageFamily::protected_material, PageRegistryStatus::reserved, "protected_material_policy", false, false, PrimaryRoles(), "PAGE-REG-RESERVED-PROTECTED-MATERIAL-POLICY", false, true),
      Descriptor(PageType::audit_chain, PageFamily::audit, PageRegistryStatus::reserved, "audit_chain", false, false, AllDurableRoles(), "PAGE-REG-RESERVED-AUDIT-CHAIN"),
      Descriptor(PageType::system_journal, PageFamily::audit, PageRegistryStatus::reserved, "system_journal", false, false, PrimaryRoles(), "PAGE-REG-RESERVED-SYSTEM-JOURNAL"),
      Descriptor(PageType::repair_manifest, PageFamily::repair, PageRegistryStatus::reserved, "repair_manifest", false, false, AllDurableRoles(), "PAGE-REG-RESERVED-REPAIR-MANIFEST"),
      Descriptor(PageType::rebuild_manifest, PageFamily::repair, PageRegistryStatus::reserved, "rebuild_manifest", false, false, AllDurableRoles(), "PAGE-REG-RESERVED-REBUILD-MANIFEST"),
      Descriptor(PageType::salvage_manifest, PageFamily::repair, PageRegistryStatus::reserved, "salvage_manifest", false, false, AllDurableRoles(), "PAGE-REG-RESERVED-SALVAGE-MANIFEST"),
      Descriptor(PageType::damage_map, PageFamily::repair, PageRegistryStatus::reserved, "damage_map", false, false, AllDurableRoles(), "PAGE-REG-RESERVED-DAMAGE-MAP"),
      Descriptor(PageType::reachability_map, PageFamily::repair, PageRegistryStatus::reserved, "reachability_map", false, false, AllDurableRoles(), "PAGE-REG-RESERVED-REACHABILITY-MAP"),
      Descriptor(PageType::typed_payload_dependency, PageFamily::typed_dependency, PageRegistryStatus::reserved, "typed_payload_dependency", false, false, DataRoles(), "PAGE-REG-RESERVED-TYPED-PAYLOAD-DEPENDENCY", false, false, false, true, true),
      Descriptor(PageType::type_migration_state, PageFamily::typed_dependency, PageRegistryStatus::reserved, "type_migration_state", false, false, DataRoles(), "PAGE-REG-RESERVED-TYPE-MIGRATION-STATE", false, false, false, true, true),
      Descriptor(PageType::resource_dependency_map, PageFamily::typed_dependency, PageRegistryStatus::reserved, "resource_dependency_map", false, false, DataRoles(), "PAGE-REG-RESERVED-RESOURCE-DEPENDENCY-MAP", false, false, false, true, true),
      Descriptor(PageType::index_resource_state, PageFamily::typed_dependency, PageRegistryStatus::reserved, "index_resource_state", false, false, IndexRoles(), "PAGE-REG-RESERVED-INDEX-RESOURCE-STATE", false, false, true, true, true),
      Descriptor(PageType::type_statistics_state, PageFamily::typed_dependency, PageRegistryStatus::reserved, "type_statistics_state", false, false, DataRoles(), "PAGE-REG-RESERVED-TYPE-STATISTICS-STATE", false, false, false, true, true),
      Descriptor(PageType::derived_structure_manifest, PageFamily::typed_dependency, PageRegistryStatus::reserved, "derived_structure_manifest", false, false, DataRoles(), "PAGE-REG-RESERVED-DERIVED-STRUCTURE-MANIFEST", false, false, false, true, true),
      Descriptor(PageType::name_registry_superseded, PageFamily::superseded, PageRegistryStatus::superseded, "name_registry_superseded", false, false, PrimaryRoles(), "PAGE-REG-SUPERSEDED-NAME-REGISTRY"),
      Descriptor(PageType::reserved_local, PageFamily::reserved, PageRegistryStatus::reserved, "reserved_local", true, false, AllDurableRoles(), "PAGE-REG-RESERVED-LOCAL"),
      Descriptor(PageType::cluster_decision, PageFamily::cluster_private, PageRegistryStatus::deferred, "cluster_decision", true, false, Roles({FilespaceRole::secondary_shard}), "PAGE-REG-DEFERRED-CLUSTER-DECISION", true),
      Descriptor(PageType::cluster_route, PageFamily::cluster_private, PageRegistryStatus::deferred, "cluster_route", true, false, Roles({FilespaceRole::secondary_shard}), "PAGE-REG-DEFERRED-CLUSTER-ROUTE", true),
      Descriptor(PageType::cluster_catalog, PageFamily::cluster_private, PageRegistryStatus::deferred, "cluster_catalog", true, false, Roles({FilespaceRole::secondary_shard}), "PAGE-REG-DEFERRED-CLUSTER-CATALOG", true),
      Descriptor(PageType::cluster_transaction, PageFamily::cluster_private, PageRegistryStatus::deferred, "cluster_transaction", true, false, Roles({FilespaceRole::secondary_shard}), "PAGE-REG-DEFERRED-CLUSTER-TRANSACTION", true),
      Descriptor(PageType::encrypted_opaque, PageFamily::encrypted_or_opaque, PageRegistryStatus::disabled, "encrypted_opaque", true, false, AllDurableRoles(), "PAGE-REG-DISABLED-ENCRYPTED-OPAQUE", false, true),
  };
  return registry;
}

PageRegistryLookupResult LookupPageFamily(PageType page_type) {
  for (const PageFamilyDescriptor& descriptor : BuiltinPageFamilyRegistry()) {
    if (descriptor.page_type == page_type) {
      PageRegistryLookupResult result;
      result.status = PageRegistryOkStatus();
      result.descriptor = descriptor;
      return result;
    }
  }

  PageRegistryLookupResult result;
  result.status = PageRegistryErrorStatus();
  result.descriptor = Descriptor(page_type,
                                 PageFamily::unknown,
                                 PageRegistryStatus::disabled,
                                 PageTypeName(page_type),
                                 false,
                                 false,
                                 {},
                                 "PAGE-REG-UNKNOWN-PAGE-TYPE");
  result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                "SB_DIAG_PAGE_TYPE_UNKNOWN",
                                                "page.registry.unknown_page_type",
                                                PageTypeName(page_type));
  return result;
}

PageManagerClassification ClassifyForPageManager(const PageClassification& header_classification) {
  PageManagerClassification result;
  result.status = PageRegistryOkStatus();
  result.header_classification = header_classification;

  PageRegistryLookupResult lookup = LookupPageFamily(header_classification.page_type);
  result.descriptor = lookup.descriptor;

  const bool invalid_header_classification =
      header_classification.kind == PageClassificationKind::invalid_magic ||
      header_classification.kind == PageClassificationKind::invalid_header ||
      header_classification.kind == PageClassificationKind::checksum_mismatch;
  if (!header_classification.ok() && invalid_header_classification) {
    result.status = PageRegistryErrorStatus();
    result.may_read_body = false;
    result.may_write_body = false;
    result.requires_cluster_authority = false;
    result.requires_decryption = false;
    result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                   "SB-PAGE-REGISTRY-HEADER-CLASSIFICATION-FAILED",
                                                   "page.registry.header_classification_failed",
                                                   PageClassificationKindName(header_classification.kind));
    return result;
  }

  result.requires_cluster_authority = header_classification.cluster_authority_required || result.descriptor.cluster_only;
  result.requires_decryption = header_classification.decryption_required || result.descriptor.encrypted_or_opaque;
  result.may_read_body = header_classification.readable && result.descriptor.supported_local_read;
  result.may_write_body = header_classification.writable && result.descriptor.supported_local_write &&
                          !result.requires_cluster_authority && !result.requires_decryption &&
                          !result.descriptor.reserved;

  if (result.requires_cluster_authority) {
    result.status = PageRegistryWarningStatus();
    result.may_write_body = false;
    result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                   "SB-PAGE-REGISTRY-CLUSTER-AUTHORITY-REQUIRED",
                                                   "page.registry.cluster_authority_required",
                                                   result.descriptor.stable_name);
    return result;
  }

  if (result.requires_decryption) {
    result.status = PageRegistryWarningStatus();
    result.may_write_body = false;
    result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                   "SB-PAGE-REGISTRY-DECRYPTION-REQUIRED",
                                                   "page.registry.decryption_required",
                                                   result.descriptor.stable_name);
    return result;
  }

  if (!lookup.ok()) {
    result.status = PageRegistryErrorStatus();
    result.may_read_body = false;
    result.may_write_body = false;
    result.diagnostic = lookup.diagnostic;
    return result;
  }

  if (result.descriptor.reserved) {
    result.status = PageRegistryWarningStatus();
    result.may_write_body = false;
    result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                   "SB_DIAG_PAGE_TYPE_STATUS_UNSUPPORTED",
                                                   "page.registry.page_type_status_unsupported",
                                                   result.descriptor.stable_name + ":" +
                                                       PageRegistryStatusName(result.descriptor.registry_status));
  }

  return result;
}

PageRegistryValidationResult ValidatePageTypeProductSupportClaim(PageType page_type,
                                                                 std::string claimed_support) {
  PageRegistryValidationResult result;
  PageRegistryLookupResult lookup = LookupPageFamily(page_type);
  result.status = lookup.status;
  result.descriptor = lookup.descriptor;
  result.diagnostic = lookup.diagnostic;
  if (!lookup.ok()) {
    return result;
  }
  if (PageRegistryStatusAllowsProductSupport(lookup.descriptor.registry_status)) {
    result.status = PageRegistryOkStatus();
    result.diagnostic = {};
    return result;
  }

  result.status = PageRegistryErrorStatus();
  result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                "SB_DIAG_PAGE_RESERVED_SUPPORT_OVERCLAIM",
                                                "page.registry.reserved_support_overclaim",
                                                lookup.descriptor.stable_name + ":" +
                                                    PageRegistryStatusName(lookup.descriptor.registry_status) +
                                                    ":" + std::move(claimed_support));
  return result;
}

PageRegistryValidationResult ValidatePageTypeFilespaceRole(
    PageType page_type,
    FilespaceRole filespace_role,
    std::string operation) {
  PageRegistryValidationResult result;
  PageRegistryLookupResult lookup = LookupPageFamily(page_type);
  result.status = lookup.status;
  result.descriptor = lookup.descriptor;
  result.diagnostic = lookup.diagnostic;
  if (!lookup.ok()) {
    return result;
  }

  const auto& roles = lookup.descriptor.legal_filespace_roles;
  if (std::find(roles.begin(), roles.end(), filespace_role) != roles.end()) {
    result.status = PageRegistryOkStatus();
    result.diagnostic = {};
    return result;
  }

  result.status = PageRegistryErrorStatus();
  result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                "SB_DIAG_PAGE_FILESPACE_ROLE_FORBIDDEN",
                                                "page.registry.filespace_role_forbidden",
                                                lookup.descriptor.stable_name + ":" +
                                                    std::to_string(static_cast<u32>(filespace_role)) +
                                                    ":" + std::move(operation));
  return result;
}

DiagnosticRecord MakePageRegistryDiagnostic(Status status,
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
                        "storage.page.registry");
}

}  // namespace scratchbird::storage::page
