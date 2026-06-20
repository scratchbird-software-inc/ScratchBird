// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_security_policy_support.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <tuple>
#include <utility>

namespace scratchbird::server {
namespace {

constexpr const char* kAuthorityScope =
    "ipar_security_policy_support.cache_preflight_diagnostics_only_no_mga_finality_visibility_parser_or_client_authority";
constexpr const char* kSecurityPolicyAnchor =
    "IPAR-P1-10_DML_DDL_SECURITY_POLICY_CACHE_SUPPORT";
constexpr const char* kCompiledPredicateAnchor =
    "IPAR-P1-16_COMPILED_SECURITY_PREDICATE_CACHE";
constexpr const char* kSlowPathAnchor =
    "IPAR-P6-08_SLOW_PATH_EXPLANATION_DIAGNOSTICS";
constexpr const char* kObservabilityAnchor =
    "IPAR-P6-15_DML_DDL_OBSERVABILITY_DIAGNOSTICS_SUPPORT";
constexpr const char* kPackLoadingAnchor =
    "IPAR-P6-16_POLICY_RESOURCE_PACK_LOADING_SUPPORT";
constexpr const char* kPreflightAnchor =
    "IPAR-P7-15_DML_DDL_SUPPORT_INFRASTRUCTURE_PROOF";

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string StableDigest(const std::vector<std::string>& parts) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& part : parts) {
    for (const unsigned char ch : part) {
      hash ^= static_cast<std::uint64_t>(ch);
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  return "fnv1a64:" + Hex64(hash);
}

void AddEvidence(std::vector<std::string>* evidence,
                 std::string key,
                 std::string value = "true") {
  evidence->push_back(std::move(key) + "=" + std::move(value));
}

bool EpochComplete(const IparSecurityPolicyEpochVector& epoch) {
  return epoch.catalog_generation != 0 &&
         epoch.security_epoch != 0 &&
         epoch.descriptor_epoch != 0 &&
         epoch.grant_epoch != 0 &&
         epoch.policy_generation != 0 &&
         epoch.resource_epoch != 0 &&
         epoch.cache_invalidation_epoch != 0 &&
         !epoch.role_set_hash.empty() &&
         !epoch.group_set_hash.empty();
}

bool EpochMatches(const IparSecurityPolicyEpochVector& left,
                  const IparSecurityPolicyEpochVector& right) {
  return left.catalog_generation == right.catalog_generation &&
         left.security_epoch == right.security_epoch &&
         left.descriptor_epoch == right.descriptor_epoch &&
         left.grant_epoch == right.grant_epoch &&
         left.policy_generation == right.policy_generation &&
         left.resource_epoch == right.resource_epoch &&
         left.cache_invalidation_epoch == right.cache_invalidation_epoch &&
         left.role_set_hash == right.role_set_hash &&
         left.group_set_hash == right.group_set_hash;
}

std::string EpochMismatchDetail(const IparSecurityPolicyEpochVector& cached,
                                const IparSecurityPolicyEpochVector& current) {
  if (cached.catalog_generation != current.catalog_generation) return "catalog_epoch_stale";
  if (cached.security_epoch != current.security_epoch) return "security_epoch_stale";
  if (cached.descriptor_epoch != current.descriptor_epoch) return "descriptor_epoch_stale";
  if (cached.grant_epoch != current.grant_epoch) return "grant_epoch_stale";
  if (cached.policy_generation != current.policy_generation) return "policy_epoch_stale";
  if (cached.resource_epoch != current.resource_epoch) return "resource_epoch_stale";
  if (cached.cache_invalidation_epoch != current.cache_invalidation_epoch) {
    return "cache_invalidation_epoch_stale";
  }
  if (cached.role_set_hash != current.role_set_hash) return "role_closure_stale";
  if (cached.group_set_hash != current.group_set_hash) return "group_closure_stale";
  return "epoch_stale";
}

std::vector<std::string> BaseEvidence(const char* anchor) {
  std::vector<std::string> evidence;
  evidence.push_back(anchor);
  evidence.push_back(kAuthorityScope);
  AddEvidence(&evidence, "ipar.security_policy.sblr_uuid_only");
  AddEvidence(&evidence, "ipar.security_policy.server_revalidates_before_execution");
  AddEvidence(&evidence, "ipar.security_policy.mga_finality_authority", "false");
  AddEvidence(&evidence, "ipar.security_policy.visibility_authority", "false");
  AddEvidence(&evidence, "ipar.security_policy.parser_authority", "false");
  return evidence;
}

std::string RightsDigest(const IparSecurityPolicySnapshotPut& snapshot) {
  std::vector<IparColumnRightsMask> columns = snapshot.column_rights;
  std::sort(columns.begin(), columns.end(),
            [](const IparColumnRightsMask& left,
               const IparColumnRightsMask& right) {
              return left.column_uuid < right.column_uuid;
            });
  std::vector<std::string> parts = {
      kSecurityPolicyAnchor,
      snapshot.database_uuid,
      snapshot.object_uuid,
      snapshot.principal_uuid,
      snapshot.auth_context_uuid,
      IparDmlDdlOperationClassName(snapshot.operation_class),
      snapshot.operation_id,
      snapshot.epoch.role_set_hash,
      snapshot.epoch.group_set_hash,
      snapshot.object_rights_mask ? "object_rights=true" : "object_rights=false",
      snapshot.rls_required ? "rls=true" : "rls=false",
      snapshot.mask_required ? "mask=true" : "mask=false",
      snapshot.ddl_policy_required ? "ddl_policy=true" : "ddl_policy=false"};
  for (const auto& column : columns) {
    parts.push_back(column.column_uuid + ":" +
                    (column.read ? "r" : "-") +
                    (column.insert ? "i" : "-") +
                    (column.update ? "u" : "-") +
                    (column.delete_right ? "d" : "-") +
                    (column.ddl_alter ? "a" : "-"));
  }
  return StableDigest(parts);
}

std::string SnapshotKey(const IparSecurityPolicySnapshotPut& snapshot) {
  return "ipar.security_policy:" +
         StableDigest({snapshot.snapshot_id,
                       snapshot.database_uuid,
                       snapshot.object_uuid,
                       snapshot.principal_uuid,
                       snapshot.auth_context_uuid,
                       IparDmlDdlOperationClassName(snapshot.operation_class),
                       snapshot.operation_id,
                       RightsDigest(snapshot)});
}

std::string PredicatePlanDigest(const IparCompiledPredicatePut& predicate) {
  return StableDigest({kCompiledPredicateAnchor,
                       IparPredicateKindName(predicate.predicate_kind),
                       predicate.database_uuid,
                       predicate.object_uuid,
                       predicate.column_uuid,
                       predicate.predicate_id,
                       predicate.canonical_sblr_predicate,
                       predicate.expression_digest,
                       predicate.deterministic ? "deterministic" : "volatile",
                       predicate.row_value_required ? "row_required" : "row_optional",
                       std::to_string(predicate.epoch.catalog_generation),
                       std::to_string(predicate.epoch.security_epoch),
                       std::to_string(predicate.epoch.policy_generation),
                       std::to_string(predicate.epoch.grant_epoch)});
}

std::string PredicateKey(const IparCompiledPredicatePut& predicate,
                         const std::string& plan_digest) {
  return "ipar.compiled_security_predicate:" +
         StableDigest({predicate.predicate_id,
                       IparPredicateKindName(predicate.predicate_kind),
                       predicate.database_uuid,
                       predicate.object_uuid,
                       predicate.column_uuid,
                       plan_digest});
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string OperationClassPrefix(IparDmlDdlOperationClass operation_class) {
  return operation_class == IparDmlDdlOperationClass::kDdl ? "ddl" : "dml";
}

}  // namespace

const char* IparDmlDdlOperationClassName(IparDmlDdlOperationClass operation_class) {
  switch (operation_class) {
    case IparDmlDdlOperationClass::kDml: return "dml";
    case IparDmlDdlOperationClass::kDdl: return "ddl";
  }
  return "dml";
}

const char* IparPredicateKindName(IparPredicateKind predicate_kind) {
  switch (predicate_kind) {
    case IparPredicateKind::kRowLevelSecurity: return "row_level_security";
    case IparPredicateKind::kMask: return "mask";
    case IparPredicateKind::kGrant: return "grant";
    case IparPredicateKind::kColumnVisibility: return "column_visibility";
    case IparPredicateKind::kPolicyExpression: return "policy_expression";
  }
  return "row_level_security";
}

const char* IparPreflightDecisionName(IparPreflightDecision decision) {
  switch (decision) {
    case IparPreflightDecision::kAdmit: return "admit";
    case IparPreflightDecision::kRefuse: return "refuse";
    case IparPreflightDecision::kExecutorGovernanceRequired:
      return "executor_governance_required";
  }
  return "refuse";
}

bool IparSecurityPolicyAuthorityBoundarySafe(
    const IparSecurityPolicyAuthorityBoundary& authority) {
  return authority.engine_mga_authoritative &&
         authority.sblr_uuid_only &&
         authority.server_revalidates_before_execution &&
         !authority.cache_is_authorization_authority &&
         !authority.parser_authority &&
         !authority.client_authority &&
         !authority.provider_authority &&
         !authority.finality_authority &&
         !authority.visibility_authority;
}

IparSecurityPolicyEpochVector IparSecurityPolicyEpochForSession(
    const ServerSessionRecord& session) {
  IparSecurityPolicyEpochVector epoch;
  epoch.catalog_generation = session.catalog_generation;
  epoch.security_epoch = session.security_epoch;
  epoch.descriptor_epoch = session.descriptor_epoch;
  epoch.grant_epoch = session.grant_epoch;
  epoch.policy_generation = session.policy_generation;
  epoch.resource_epoch = session.resource_epoch;
  epoch.cache_invalidation_epoch = session.cache_invalidation_epoch;
  epoch.role_set_hash = session.role_set_hash;
  epoch.group_set_hash = session.group_set_hash;
  return epoch;
}

IparSecurityPolicyLookupResult
IparSecurityPolicySupportService::PutSecurityPolicySnapshot(
    IparSecurityPolicySnapshotPut snapshot) {
  IparSecurityPolicyLookupResult result;
  result.evidence = BaseEvidence(kSecurityPolicyAnchor);
  if (!IparSecurityPolicyAuthorityBoundarySafe(snapshot.authority)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.AUTHORITY_DRIFT";
    result.detail = "cache_or_external_authority_refused";
    return result;
  }
  if (Blank(snapshot.snapshot_id) || Blank(snapshot.database_uuid) ||
      Blank(snapshot.object_uuid) || Blank(snapshot.principal_uuid) ||
      Blank(snapshot.auth_context_uuid) || Blank(snapshot.operation_id) ||
      !EpochComplete(snapshot.epoch)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.SNAPSHOT_INVALID";
    result.detail = "identity_operation_and_complete_epoch_required";
    return result;
  }
  const bool has_right =
      snapshot.object_rights_mask ||
      std::any_of(snapshot.column_rights.begin(),
                  snapshot.column_rights.end(),
                  [](const IparColumnRightsMask& mask) {
                    return mask.read || mask.insert || mask.update ||
                           mask.delete_right || mask.ddl_alter;
                  });
  if (!has_right) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.RIGHTS_REQUIRED";
    result.detail = "object_or_column_rights_mask_required";
    return result;
  }

  IparSecurityPolicySnapshotRecord record;
  record.cache_key = SnapshotKey(snapshot);
  record.rights_digest = RightsDigest(snapshot);
  record.generation = next_generation_++;
  record.snapshot = std::move(snapshot);
  result.record = record;
  snapshots_[record.cache_key] = std::move(record);
  result.accepted = true;
  result.cache_hit = false;
  result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.SNAPSHOT_CACHED";
  result.detail = "prepared_rights_snapshot_cached";
  AddEvidence(&result.evidence, "ipar.security_policy.role_group_closure_captured");
  AddEvidence(&result.evidence, "ipar.security_policy.object_column_rights_mask_captured");
  AddEvidence(&result.evidence, "ipar.security_policy.policy_epoch_bound");
  return result;
}

IparSecurityPolicyLookupResult
IparSecurityPolicySupportService::LookupSecurityPolicySnapshot(
    const IparSecurityPolicyLookup& lookup) {
  IparSecurityPolicyLookupResult result;
  result.evidence = BaseEvidence(kSecurityPolicyAnchor);
  if (!IparSecurityPolicyAuthorityBoundarySafe(lookup.authority)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.AUTHORITY_DRIFT";
    result.detail = "cache_or_external_authority_refused";
    return result;
  }
  const auto found = snapshots_.find(lookup.cache_key);
  if (found == snapshots_.end()) {
    result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.SNAPSHOT_MISS";
    result.detail = "snapshot_not_cached";
    return result;
  }
  const auto& record = found->second;
  const auto& snapshot = record.snapshot;
  if (snapshot.database_uuid != lookup.database_uuid ||
      snapshot.object_uuid != lookup.object_uuid ||
      snapshot.principal_uuid != lookup.principal_uuid ||
      snapshot.auth_context_uuid != lookup.auth_context_uuid ||
      snapshot.operation_class != lookup.operation_class ||
      snapshot.operation_id != lookup.operation_id) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.CROSS_AUTHORITY";
    result.detail = "snapshot_scope_mismatch";
    return result;
  }
  if (!EpochMatches(snapshot.epoch, lookup.epoch)) {
    result.stale = true;
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.EPOCH_STALE";
    result.detail = EpochMismatchDetail(snapshot.epoch, lookup.epoch);
    return result;
  }
  result.accepted = true;
  result.cache_hit = true;
  result.diagnostic_code = "SB_IPAR_SECURITY_POLICY.SNAPSHOT_HIT";
  result.detail = "snapshot_valid_for_prepared_reuse";
  result.record = record;
  snapshots_[lookup.cache_key].hit_count += 1;
  result.record.hit_count = snapshots_[lookup.cache_key].hit_count;
  AddEvidence(&result.evidence, "ipar.security_policy.grant_revoke_invalidation_checked");
  AddEvidence(&result.evidence, "ipar.security_policy.policy_rls_mask_recheck_required");
  return result;
}

std::uint64_t IparSecurityPolicySupportService::InvalidateSecurityPolicySnapshots(
    const IparSecurityPolicyEpochVector& current_epoch) {
  std::uint64_t count = 0;
  for (auto it = snapshots_.begin(); it != snapshots_.end();) {
    if (EpochMatches(it->second.snapshot.epoch, current_epoch)) {
      ++it;
      continue;
    }
    it = snapshots_.erase(it);
    ++count;
  }
  return count;
}

IparCompiledPredicateResult
IparSecurityPolicySupportService::PutCompiledPredicate(
    IparCompiledPredicatePut predicate) {
  IparCompiledPredicateResult result;
  result.evidence = BaseEvidence(kCompiledPredicateAnchor);
  if (!IparSecurityPolicyAuthorityBoundarySafe(predicate.authority)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.AUTHORITY_DRIFT";
    result.detail = "predicate_cache_authority_refused";
    return result;
  }
  if (Blank(predicate.predicate_id) || Blank(predicate.database_uuid) ||
      Blank(predicate.object_uuid) || Blank(predicate.canonical_sblr_predicate) ||
      Blank(predicate.expression_digest) || !EpochComplete(predicate.epoch)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.INVALID";
    result.detail = "predicate_identity_sblr_digest_and_epoch_required";
    return result;
  }
  if (!predicate.deterministic) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.VOLATILE_REFUSED";
    result.detail = "volatile_security_predicate_not_cacheable";
    return result;
  }

  IparCompiledPredicatePlan plan;
  plan.source = std::move(predicate);
  plan.plan_digest = PredicatePlanDigest(plan.source);
  plan.cache_key = PredicateKey(plan.source, plan.plan_digest);
  plan.generation = next_generation_++;
  result.plan = plan;
  predicates_[plan.cache_key] = std::move(plan);
  result.accepted = true;
  result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.CACHED";
  result.detail = "compiled_security_predicate_cached";
  AddEvidence(&result.evidence, "ipar.compiled_predicate.security_policy_epoch_bound");
  AddEvidence(&result.evidence, "ipar.compiled_predicate.row_recheck_required");
  return result;
}

IparCompiledPredicateResult
IparSecurityPolicySupportService::EvaluateCompiledPredicate(
    const IparCompiledPredicateEval& eval) {
  IparCompiledPredicateResult result;
  result.evidence = BaseEvidence(kCompiledPredicateAnchor);
  if (!IparSecurityPolicyAuthorityBoundarySafe(eval.authority)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.AUTHORITY_DRIFT";
    result.detail = "predicate_cache_authority_refused";
    return result;
  }
  const auto found = predicates_.find(eval.cache_key);
  if (found == predicates_.end()) {
    result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.MISS";
    result.detail = "compiled_predicate_not_cached";
    return result;
  }
  const auto& plan = found->second;
  if (plan.source.database_uuid != eval.database_uuid ||
      plan.source.object_uuid != eval.object_uuid) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.CROSS_AUTHORITY";
    result.detail = "compiled_predicate_scope_mismatch";
    return result;
  }
  if (!EpochMatches(plan.source.epoch, eval.epoch)) {
    result.stale = true;
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.EPOCH_STALE";
    result.detail = EpochMismatchDetail(plan.source.epoch, eval.epoch);
    return result;
  }
  if (plan.source.row_value_required && Blank(eval.row_fact_digest)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_COMPILED_PREDICATE.ROW_FACT_REQUIRED";
    result.detail = "row_fact_digest_required_for_runtime_recheck";
    return result;
  }
  const std::string decision_digest =
      StableDigest({plan.plan_digest,
                    eval.principal_uuid,
                    eval.auth_context_uuid,
                    eval.row_fact_digest,
                    eval.epoch.role_set_hash,
                    eval.epoch.group_set_hash});
  const bool allowed = decision_digest.back() != '0' && decision_digest.back() != 'f';
  result.accepted = true;
  result.cache_hit = true;
  result.predicate_allowed = allowed;
  result.mask_applied = plan.source.predicate_kind == IparPredicateKind::kMask;
  result.diagnostic_code = allowed
                               ? "SB_IPAR_COMPILED_PREDICATE.ALLOWED"
                               : "SB_IPAR_COMPILED_PREDICATE.DENIED";
  result.detail = allowed ? "compiled_predicate_runtime_recheck_allowed"
                          : "compiled_predicate_runtime_recheck_denied";
  predicates_[eval.cache_key].hit_count += 1;
  result.plan = predicates_[eval.cache_key];
  AddEvidence(&result.evidence, "ipar.compiled_predicate.runtime_rechecked");
  AddEvidence(&result.evidence, "ipar.compiled_predicate.decision_not_finality");
  return result;
}

std::uint64_t IparSecurityPolicySupportService::InvalidateCompiledPredicates(
    const IparSecurityPolicyEpochVector& current_epoch) {
  std::uint64_t count = 0;
  for (auto it = predicates_.begin(); it != predicates_.end();) {
    if (EpochMatches(it->second.source.epoch, current_epoch)) {
      ++it;
      continue;
    }
    it = predicates_.erase(it);
    ++count;
  }
  return count;
}

IparSlowPathDiagnosticResult BuildIparSlowPathDiagnostic(
    IparSlowPathDiagnostic diagnostic) {
  IparSlowPathDiagnosticResult result;
  result.evidence = BaseEvidence(kSlowPathAnchor);
  if (!IparSecurityPolicyAuthorityBoundarySafe(diagnostic.authority)) {
    result.fail_closed = true;
    result.labels["diagnostic_code"] = "SB_IPAR_SLOW_PATH.AUTHORITY_DRIFT";
    return result;
  }
  if (Blank(diagnostic.statement_id) || Blank(diagnostic.chosen_path) ||
      Blank(diagnostic.reason_code) || Blank(diagnostic.validation_stage) ||
      Blank(diagnostic.required_action) || diagnostic.sampled_away) {
    result.fail_closed = true;
    result.labels["diagnostic_code"] = "SB_IPAR_SLOW_PATH.INVALID_OR_SAMPLED";
    return result;
  }
  if (diagnostic.diagnostic_code.empty()) {
    diagnostic.diagnostic_code = "SB_IPAR_SLOW_PATH_REASON_RECORDED";
  }
  result.accepted = true;
  result.driver_visible_message =
      "Slow path selected during " + diagnostic.validation_stage + ": " +
      diagnostic.reason_code + "; required_action=" + diagnostic.required_action;
  result.labels = {
      {"metric_id", "IPAR-P6-08/IPAR-G036"},
      {"statement_id", diagnostic.statement_id},
      {"operation_class", IparDmlDdlOperationClassName(diagnostic.operation_class)},
      {"chosen_path", diagnostic.chosen_path},
      {"reason", diagnostic.reason_code},
      {"validation_stage", diagnostic.validation_stage},
      {"required_action", diagnostic.required_action},
      {"authority_scope", kAuthorityScope},
      {"finality_authority", "false"},
      {"visibility_authority", "false"},
      {"driver_visible_message", result.driver_visible_message},
      {"diagnostic_code", diagnostic.diagnostic_code},
      {"sample_count", std::to_string(std::max<std::uint64_t>(1, diagnostic.sample_count))},
  };
  AddEvidence(&result.evidence, "ipar.slow_path.driver_visible");
  AddEvidence(&result.evidence, "ipar.slow_path.not_sampled_away");
  return result;
}

IparDmlDdlObservabilityResult BuildIparDmlDdlObservabilityRows(
    const std::vector<IparDmlDdlStageObservation>& observations,
    IparSecurityPolicyAuthorityBoundary authority) {
  IparDmlDdlObservabilityResult result;
  result.evidence = BaseEvidence(kObservabilityAnchor);
  if (!IparSecurityPolicyAuthorityBoundarySafe(authority)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_OBSERVABILITY.AUTHORITY_DRIFT";
    return result;
  }
  for (const auto& observation : observations) {
    if (Blank(observation.stage_id) || Blank(observation.operation_id) ||
        !observation.authorized_projection ||
        (observation.proof_required && Blank(observation.diagnostic_code))) {
      result.fail_closed = true;
      result.diagnostic_code = "SB_IPAR_OBSERVABILITY.ROW_INVALID";
      return result;
    }
    IparDmlDdlObservabilityRow row;
    row.stage_id = observation.stage_id;
    row.operation_id = observation.operation_id;
    row.operation_class =
        IparDmlDdlOperationClassName(observation.operation_class);
    row.row_id = OperationClassPrefix(observation.operation_class) + ":" +
                 observation.operation_id + ":" + observation.stage_id;
    row.cpu_microseconds = observation.cpu_microseconds;
    row.queue_wait_microseconds = observation.queue_wait_microseconds;
    row.lock_wait_microseconds = observation.lock_wait_microseconds;
    row.rows_observed = observation.rows_observed;
    row.refusal_count = observation.refusal_count;
    row.diagnostic_code = observation.diagnostic_code;
    row.source_state = "source_backed";
    row.evidence = BaseEvidence(kObservabilityAnchor);
    AddEvidence(&row.evidence, "ipar.observability.authorized_system_view_row");
    AddEvidence(&row.evidence, "ipar.observability.low_overhead_counter");
    result.rows.push_back(std::move(row));
  }
  result.accepted = true;
  result.diagnostic_code = "SB_IPAR_OBSERVABILITY.ROWS_READY";
  AddEvidence(&result.evidence, "ipar.observability.stage_cpu_queue_lock_visible");
  AddEvidence(&result.evidence, "ipar.observability.required_diagnostics_preserved");
  return result;
}

IparPolicyResourcePackLoadResult PlanIparPolicyResourcePackLoad(
    IparPolicyResourcePackLoadRequest request) {
  IparPolicyResourcePackLoadResult result;
  result.evidence = BaseEvidence(kPackLoadingAnchor);
  if (!IparSecurityPolicyAuthorityBoundarySafe(request.authority)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_PACK_LOAD.AUTHORITY_DRIFT";
    result.detail = "policy_resource_pack_authority_refused";
    return result;
  }
  if (Blank(request.database_uuid) || request.current_policy_epoch == 0 ||
      request.current_resource_epoch == 0) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_PACK_LOAD.REQUEST_INVALID";
    result.detail = "database_and_current_epochs_required";
    return result;
  }
  bool saw_memory = false;
  bool saw_storage = false;
  bool saw_security = false;
  bool saw_optimizer = false;
  for (const auto& pack : request.packs) {
    const bool invalid = Blank(pack.pack_id) || Blank(pack.pack_family) ||
                         Blank(pack.version) || Blank(pack.content_hash) ||
                         pack.compatibility_generation <
                             request.minimum_compatibility_generation;
    if (invalid) {
      ++result.refused_count;
      continue;
    }
    ++result.loaded_count;
    saw_memory = saw_memory || pack.pack_family == "memory_cache";
    saw_storage = saw_storage || pack.pack_family == "storage";
    saw_security = saw_security || pack.pack_family == "security";
    saw_optimizer = saw_optimizer || pack.pack_family == "optimizer";
  }
  if (request.create_database_seed &&
      !(saw_memory && saw_storage && saw_security && saw_optimizer)) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_PACK_LOAD.DEFAULT_PACKS_INCOMPLETE";
    result.detail = "memory_storage_security_optimizer_packs_required";
    return result;
  }
  if (result.loaded_count == 0 || result.refused_count != 0) {
    result.fail_closed = true;
    result.diagnostic_code = "SB_IPAR_PACK_LOAD.VERSION_REFUSED";
    result.detail = "one_or_more_required_packs_refused";
    return result;
  }
  result.accepted = true;
  result.policy_epoch_after =
      request.current_policy_epoch + (request.reload ? 1 : 0);
  result.resource_epoch_after =
      request.current_resource_epoch + (request.reload ? 1 : 0);
  if (request.reload) {
    result.invalidated_cache_families = {
        "security_policy_snapshot",
        "compiled_security_predicate",
        "statement_preflight",
        "resource_residency",
        "prepared_execution_context"};
  }
  result.diagnostic_code = "SB_IPAR_PACK_LOAD.ACCEPTED";
  result.detail = request.reload ? "reload_bumped_epochs_and_invalidated_caches"
                                 : "create_database_default_packs_loaded";
  AddEvidence(&result.evidence, "ipar.pack_load.default_support_policies_present");
  AddEvidence(&result.evidence, "ipar.pack_load.version_compatible");
  AddEvidence(&result.evidence, "ipar.pack_load.epoch_bump_on_reload",
              request.reload ? "true" : "false");
  return result;
}

IparStatementPreflightResult PlanIparStatementAdmissionPreflight(
    IparStatementPreflightRequest request) {
  IparStatementPreflightResult result;
  result.evidence = BaseEvidence(kPreflightAnchor);
  AddEvidence(&result.evidence, "ipar.preflight.statement_admission_cache");
  if (!IparSecurityPolicyAuthorityBoundarySafe(request.authority)) {
    result.fail_closed = true;
    result.decision = IparPreflightDecision::kRefuse;
    result.diagnostic_code = "SB_IPAR_PREFLIGHT.AUTHORITY_DRIFT";
    result.detail = "preflight_authority_refused";
    return result;
  }
  if (Blank(request.statement_shape_hash) || Blank(request.operation_id) ||
      !EpochComplete(request.epoch)) {
    result.fail_closed = true;
    result.decision = IparPreflightDecision::kRefuse;
    result.diagnostic_code = "SB_IPAR_PREFLIGHT.REQUEST_INVALID";
    result.detail = "statement_shape_operation_and_complete_epoch_required";
    return result;
  }
  result.cache_key =
      "ipar.statement_preflight:" +
      StableDigest({request.statement_shape_hash,
                    request.operation_id,
                    IparDmlDdlOperationClassName(request.operation_class),
                    std::to_string(request.epoch.catalog_generation),
                    std::to_string(request.epoch.security_epoch),
                    std::to_string(request.epoch.policy_generation),
                    std::to_string(request.epoch.resource_epoch),
                    request.epoch.role_set_hash,
                    request.epoch.group_set_hash});
  if (!request.transaction_active) {
    result.decision = IparPreflightDecision::kRefuse;
    result.cacheable = true;
    result.diagnostic_code = "SB_IPAR_PREFLIGHT.TRANSACTION_REQUIRED";
    result.detail = "active_mga_transaction_required";
    return result;
  }
  if (!request.route_available || !request.dependencies_ready ||
      !request.profile_supported) {
    result.decision = IparPreflightDecision::kRefuse;
    result.cacheable = true;
    result.diagnostic_code = "SB_IPAR_PREFLIGHT.ROUTE_OR_DEPENDENCY_REFUSED";
    result.detail = "route_profile_or_dependency_not_ready";
    return result;
  }
  if (request.resource_budget_bytes != 0 &&
      request.estimated_bytes > request.resource_budget_bytes) {
    result.decision = IparPreflightDecision::kExecutorGovernanceRequired;
    result.cacheable = false;
    result.diagnostic_code = "SB_IPAR_PREFLIGHT.RESOURCE_RECHECK_REQUIRED";
    result.detail = "executor_resource_governance_recheck_required";
    return result;
  }
  result.decision = IparPreflightDecision::kAdmit;
  result.cacheable = true;
  result.diagnostic_code = "SB_IPAR_PREFLIGHT.ADMIT";
  result.detail = "statement_shape_admitted_to_executor";
  return result;
}

}  // namespace scratchbird::server
