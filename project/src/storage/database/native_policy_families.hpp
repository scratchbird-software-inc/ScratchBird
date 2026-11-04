// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_identifier_profile.hpp"
#include "catalog_isolation_profile.hpp"
#include "catalog_management_descriptor.hpp"
#include "catalog_naming_collision_policy.hpp"
#include "catalog_policy_descriptor.hpp"
#include "catalog_verification_policy.hpp"
#include "runtime_platform.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::storage::database {
// SEARCH_KEY: SB_NATIVE_POLICY_FAMILY_SEEDS (owner direction 2026-11-04). The
// five native policy families native create lays down -- verification,
// transaction, management, naming and collision -- take their values from the
// policy pack (policies/native_*.json), each value marked there for owner
// review. Cross references are pack keys; identities are issued at create.
// Typed values only: nothing here is encoded, named or placed in a catalog.
using u64 = core::platform::u64;
struct NativePolicyDescriptorSeed {
  std::string key, body, evidence_profile;
  core::catalog::CatalogPolicyClass policy_class{};
  core::catalog::CatalogPolicyScope scope{};
  std::optional<core::catalog::CatalogPolicyDefault> default_decision;
  u64 precedence_rank=0, enforcement_points=0, cache_invalidation_mask=0;
};
struct NativeVerificationBodySeed {
  std::string key, evidence_policy, access_policy;
  core::catalog::VerificationPolicyRole role{};
  u64 verification_class_mask=0, authority_class_mask=0, verification_level_mask=0;
  u64 maximum_scan_pages=0, maximum_work_bytes=0, maximum_findings=0;
  bool mark_salvaged_output=false;
};
struct NativeEvidenceBodySeed {
  std::string key, storage_filespace;
  std::vector<std::string> required_evidence_classes, required_search_keys;
  u64 maximum_record_bytes=0, maximum_batch_bytes=0;
};
struct NativeAuthorizationBodySeed {
  std::string key, schema;
  u64 maximum_records=0, maximum_membership_depth=0, maximum_work=0;
};
struct NativeTransactionBodySeed {
  std::string key, default_isolation_profile, startup_isolation_profile, lock_policy, evidence_policy;
  std::vector<std::string> allowed_isolation_profiles;
  u64 read_mode_mask=0, maximum_transaction_microseconds=0, maximum_snapshot_microseconds=0;
  u64 maximum_active_transactions=0;
};
struct NativeLockBodySeed {
  std::string key, evidence_policy;
  u64 fairness_threshold_microseconds=0, maximum_locks=0, maximum_lock_memory_bytes=0;
  u64 maximum_graph_nodes=0, maximum_graph_edges=0, maximum_deadlock_work=0;
  u64 deadlock_detection_interval_microseconds=0;
};
struct NativeIsolationProfileSeed {
  std::string key, wait_policy, deadlock_policy;
  core::catalog::IsolationProfileKind profile_kind{};
  core::catalog::IsolationSnapshotPoint snapshot_construction_point{};
  core::catalog::IsolationMetadataVisibility metadata_visibility{};
  u64 snapshot_refresh_mask=0, write_conflict_detection_mask=0;
};
struct NativeProfileBindingSeed {
  std::string policy_profile_area, resource_definition;
};
struct NativeManagementDescriptorSeed {
  std::string key;
  core::catalog::CatalogManagementOperationCode code{};
  u64 flags=0;
  std::vector<core::catalog::CatalogManagementProfileCode> profiles;
};
struct NativeIdentifierProfileSeed {
  std::string key, profile_name;
  core::catalog::IdentifierEscapeRule escape_rule{};
  core::catalog::IdentifierComparisonMode comparison_mode{};
  core::catalog::IdentifierDisplayMode storage_display_mode{};
  core::catalog::IdentifierReservedWordMode reserved_word_behavior{};
  core::catalog::IdentifierLengthUnit length_unit{};
  u64 dialect_mode_flags=0;
};
struct NativeDialectProfileSeed {
  std::string key, identifier_profile;
};
struct NativeCollisionGroupSeed {
  std::string key;
  core::catalog::CatalogCollisionLanguageLane language_lane{};
  core::catalog::CatalogCollisionKeyLane key_lane{};
};
struct NativeCollisionPolicySeed {
  std::string key, record_kind_group;
  core::catalog::CatalogPolicyScope scope{};
  u64 precedence_rank=0, enforcement_points=0;
  bool all_built_in_record_kinds=false;
};
struct NativePolicyFamilySeeds {
  std::vector<NativePolicyDescriptorSeed> verification_descriptors, transaction_descriptors;
  std::vector<NativeVerificationBodySeed> verification_bodies;
  std::vector<NativeEvidenceBodySeed> evidence_bodies;
  std::vector<NativeAuthorizationBodySeed> authorization_bodies;
  std::vector<NativeTransactionBodySeed> transaction_bodies;
  std::vector<NativeLockBodySeed> lock_bodies;
  std::vector<NativeIsolationProfileSeed> isolation_profiles;
  std::vector<NativeProfileBindingSeed> profile_bindings;
  std::vector<core::catalog::CatalogManagementProfileCode> management_profiles;
  std::vector<NativeManagementDescriptorSeed> management_descriptors;
  std::vector<NativeIdentifierProfileSeed> identifier_profiles;
  std::vector<NativeDialectProfileSeed> dialect_profiles;
  std::vector<NativeCollisionGroupSeed> collision_groups;
  std::vector<NativeCollisionPolicySeed> collision_policies;
};
struct NativePolicyFamiliesLoadResult {
  core::platform::Status status;
  core::platform::DiagnosticRecord diagnostic;
  // Absent when the pack declares none of the four family files; native
  // create refuses such a pack. A partial set is a load refusal instead.
  std::optional<NativePolicyFamilySeeds> seeds;
  bool ok() const noexcept { return status.ok(); }
};
// Loads the admitted pack (manifest hashes, schema) and returns its native
// policy family seeds. Every cross reference is checked to name a declared
// key of the right family.
NativePolicyFamiliesLoadResult LoadNativePolicyFamilies(std::string_view policy_seed_pack_root) noexcept;
}  // namespace scratchbird::storage::database
