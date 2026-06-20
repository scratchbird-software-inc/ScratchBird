// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// IPAR-P1-10/IPAR-P1-16/IPAR-P6-08/IPAR-P6-15/IPAR-P6-16 support.
// These services prepare and validate DML/DDL security, policy, diagnostics,
// observability, and policy-pack support state. They are cache/preflight
// support only; engine SBLR/UUID admission and MGA transaction inventory remain
// the only mutation, visibility, and finality authorities.

#include "session_registry.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::server {

enum class IparDmlDdlOperationClass {
  kDml,
  kDdl,
};

enum class IparPredicateKind {
  kRowLevelSecurity,
  kMask,
  kGrant,
  kColumnVisibility,
  kPolicyExpression,
};

enum class IparPreflightDecision {
  kAdmit,
  kRefuse,
  kExecutorGovernanceRequired,
};

struct IparSecurityPolicyAuthorityBoundary {
  bool engine_mga_authoritative = true;
  bool sblr_uuid_only = true;
  bool server_revalidates_before_execution = true;
  bool cache_is_authorization_authority = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool provider_authority = false;
  bool finality_authority = false;
  bool visibility_authority = false;
};

struct IparSecurityPolicyEpochVector {
  std::uint64_t catalog_generation = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t descriptor_epoch = 1;
  std::uint64_t grant_epoch = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t resource_epoch = 1;
  std::uint64_t cache_invalidation_epoch = 1;
  std::string role_set_hash = "roles/default";
  std::string group_set_hash = "groups/default";
};

struct IparColumnRightsMask {
  std::string column_uuid;
  bool read = false;
  bool insert = false;
  bool update = false;
  bool delete_right = false;
  bool ddl_alter = false;
};

struct IparSecurityPolicySnapshotPut {
  std::string snapshot_id;
  std::string database_uuid;
  std::string object_uuid;
  std::string principal_uuid;
  std::string auth_context_uuid;
  IparDmlDdlOperationClass operation_class = IparDmlDdlOperationClass::kDml;
  std::string operation_id;
  IparSecurityPolicyEpochVector epoch;
  IparSecurityPolicyAuthorityBoundary authority;
  bool object_rights_mask = false;
  bool rls_required = false;
  bool mask_required = false;
  bool ddl_policy_required = false;
  std::vector<IparColumnRightsMask> column_rights;
};

struct IparSecurityPolicySnapshotRecord {
  std::string cache_key;
  IparSecurityPolicySnapshotPut snapshot;
  std::string rights_digest;
  std::uint64_t generation = 0;
  std::uint64_t hit_count = 0;
};

struct IparSecurityPolicyLookup {
  std::string cache_key;
  std::string database_uuid;
  std::string object_uuid;
  std::string principal_uuid;
  std::string auth_context_uuid;
  IparDmlDdlOperationClass operation_class = IparDmlDdlOperationClass::kDml;
  std::string operation_id;
  IparSecurityPolicyEpochVector epoch;
  IparSecurityPolicyAuthorityBoundary authority;
};

struct IparSecurityPolicyLookupResult {
  bool accepted = false;
  bool cache_hit = false;
  bool stale = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::string detail;
  IparSecurityPolicySnapshotRecord record;
  std::vector<std::string> evidence;
};

struct IparCompiledPredicatePut {
  std::string predicate_id;
  IparPredicateKind predicate_kind = IparPredicateKind::kRowLevelSecurity;
  std::string database_uuid;
  std::string object_uuid;
  std::string column_uuid;
  std::string canonical_sblr_predicate;
  std::string expression_digest;
  IparSecurityPolicyEpochVector epoch;
  IparSecurityPolicyAuthorityBoundary authority;
  bool deterministic = true;
  bool row_value_required = true;
};

struct IparCompiledPredicatePlan {
  std::string cache_key;
  IparCompiledPredicatePut source;
  std::string plan_digest;
  std::uint64_t generation = 0;
  std::uint64_t hit_count = 0;
};

struct IparCompiledPredicateEval {
  std::string cache_key;
  std::string database_uuid;
  std::string object_uuid;
  std::string principal_uuid;
  std::string auth_context_uuid;
  std::string row_fact_digest;
  IparSecurityPolicyEpochVector epoch;
  IparSecurityPolicyAuthorityBoundary authority;
};

struct IparCompiledPredicateResult {
  bool accepted = false;
  bool cache_hit = false;
  bool stale = false;
  bool predicate_allowed = false;
  bool mask_applied = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::string detail;
  IparCompiledPredicatePlan plan;
  std::vector<std::string> evidence;
};

struct IparSlowPathDiagnostic {
  std::string statement_id;
  IparDmlDdlOperationClass operation_class = IparDmlDdlOperationClass::kDml;
  std::string chosen_path;
  std::string reason_code;
  std::string validation_stage;
  std::string required_action;
  std::string diagnostic_code;
  std::uint64_t fallback_count = 1;
  std::uint64_t sample_count = 1;
  bool driver_visible = true;
  bool sampled_away = false;
  IparSecurityPolicyAuthorityBoundary authority;
};

struct IparSlowPathDiagnosticResult {
  bool accepted = false;
  bool fail_closed = false;
  std::string driver_visible_message;
  std::map<std::string, std::string> labels;
  std::vector<std::string> evidence;
};

struct IparDmlDdlStageObservation {
  std::string stage_id;
  std::string operation_id;
  IparDmlDdlOperationClass operation_class = IparDmlDdlOperationClass::kDml;
  std::uint64_t cpu_microseconds = 0;
  std::uint64_t queue_wait_microseconds = 0;
  std::uint64_t lock_wait_microseconds = 0;
  std::uint64_t rows_observed = 0;
  std::uint64_t refusal_count = 0;
  std::string diagnostic_code;
  bool authorized_projection = true;
  bool proof_required = false;
};

struct IparDmlDdlObservabilityRow {
  std::string row_id;
  std::string stage_id;
  std::string operation_id;
  std::string operation_class;
  std::uint64_t cpu_microseconds = 0;
  std::uint64_t queue_wait_microseconds = 0;
  std::uint64_t lock_wait_microseconds = 0;
  std::uint64_t rows_observed = 0;
  std::uint64_t refusal_count = 0;
  std::string diagnostic_code;
  std::string source_state;
  std::vector<std::string> evidence;
};

struct IparDmlDdlObservabilityResult {
  bool accepted = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::vector<IparDmlDdlObservabilityRow> rows;
  std::vector<std::string> evidence;
};

struct IparPolicyResourcePackRecord {
  std::string pack_id;
  std::string pack_family;
  std::string version;
  std::string content_hash;
  std::uint64_t compatibility_generation = 1;
  bool required_for_create_database = true;
  bool loaded = false;
};

struct IparPolicyResourcePackLoadRequest {
  std::string database_uuid;
  std::uint64_t current_policy_epoch = 1;
  std::uint64_t current_resource_epoch = 1;
  std::uint64_t minimum_compatibility_generation = 1;
  bool create_database_seed = false;
  bool reload = false;
  IparSecurityPolicyAuthorityBoundary authority;
  std::vector<IparPolicyResourcePackRecord> packs;
};

struct IparPolicyResourcePackLoadResult {
  bool accepted = false;
  bool fail_closed = false;
  std::uint64_t policy_epoch_after = 0;
  std::uint64_t resource_epoch_after = 0;
  std::uint64_t loaded_count = 0;
  std::uint64_t refused_count = 0;
  std::vector<std::string> invalidated_cache_families;
  std::vector<std::string> evidence;
  std::string diagnostic_code;
  std::string detail;
};

struct IparStatementPreflightRequest {
  std::string statement_shape_hash;
  std::string operation_id;
  IparDmlDdlOperationClass operation_class = IparDmlDdlOperationClass::kDml;
  std::uint64_t resource_budget_bytes = 0;
  std::uint64_t estimated_bytes = 0;
  bool transaction_active = true;
  bool route_available = true;
  bool dependencies_ready = true;
  bool profile_supported = true;
  IparSecurityPolicyEpochVector epoch;
  IparSecurityPolicyAuthorityBoundary authority;
};

struct IparStatementPreflightResult {
  IparPreflightDecision decision = IparPreflightDecision::kRefuse;
  bool cacheable = false;
  bool fail_closed = false;
  std::string cache_key;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
};

class IparSecurityPolicySupportService {
 public:
  IparSecurityPolicyLookupResult PutSecurityPolicySnapshot(
      IparSecurityPolicySnapshotPut snapshot);
  IparSecurityPolicyLookupResult LookupSecurityPolicySnapshot(
      const IparSecurityPolicyLookup& lookup);
  std::uint64_t InvalidateSecurityPolicySnapshots(
      const IparSecurityPolicyEpochVector& current_epoch);

  IparCompiledPredicateResult PutCompiledPredicate(
      IparCompiledPredicatePut predicate);
  IparCompiledPredicateResult EvaluateCompiledPredicate(
      const IparCompiledPredicateEval& eval);
  std::uint64_t InvalidateCompiledPredicates(
      const IparSecurityPolicyEpochVector& current_epoch);

 private:
  std::map<std::string, IparSecurityPolicySnapshotRecord> snapshots_;
  std::map<std::string, IparCompiledPredicatePlan> predicates_;
  std::uint64_t next_generation_ = 1;
};

IparSecurityPolicyEpochVector IparSecurityPolicyEpochForSession(
    const ServerSessionRecord& session);
IparSlowPathDiagnosticResult BuildIparSlowPathDiagnostic(
    IparSlowPathDiagnostic diagnostic);
IparDmlDdlObservabilityResult BuildIparDmlDdlObservabilityRows(
    const std::vector<IparDmlDdlStageObservation>& observations,
    IparSecurityPolicyAuthorityBoundary authority = {});
IparPolicyResourcePackLoadResult PlanIparPolicyResourcePackLoad(
    IparPolicyResourcePackLoadRequest request);
IparStatementPreflightResult PlanIparStatementAdmissionPreflight(
    IparStatementPreflightRequest request);

const char* IparDmlDdlOperationClassName(IparDmlDdlOperationClass operation_class);
const char* IparPredicateKindName(IparPredicateKind predicate_kind);
const char* IparPreflightDecisionName(IparPreflightDecision decision);
bool IparSecurityPolicyAuthorityBoundarySafe(
    const IparSecurityPolicyAuthorityBoundary& authority);

}  // namespace scratchbird::server
