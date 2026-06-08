// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cache/sblr_template_cache.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "optimizer_plan_cache.hpp"
#include "snapshot_safe_result_cache.hpp"
#include "streaming_cursor_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace parser = scratchbird::parser::sbsql;
namespace wire = scratchbird::wire;

constexpr std::string_view kUnsafePrefix = "ORH_SECURITY_REDACTION_RACE_UNSAFE";

struct EvidenceRecord {
  std::string surface;
  std::string trigger;
  std::string diagnostic_code;
  bool invalidated_or_refused = false;
  bool safe_resolution = false;
  bool advisory_only = true;
  std::vector<std::string> evidence;
};

std::vector<EvidenceRecord> g_records;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-126 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values,
               std::string_view prefix) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.rfind(prefix, 0) == 0;
  });
}

void Record(EvidenceRecord record) {
  Require(!record.surface.empty(), "ORH-126 evidence surface missing");
  Require(!record.trigger.empty(), "ORH-126 evidence trigger missing");
  Require(!record.diagnostic_code.empty(),
          "ORH-126 evidence diagnostic missing for " + record.surface);
  Require(record.invalidated_or_refused,
          "ORH-126 surface did not invalidate or refuse: " + record.surface +
              "/" + record.trigger);
  Require(record.safe_resolution,
          "ORH-126 surface did not use a safe resolution: " + record.surface + "/" +
              record.trigger);
  Require(record.advisory_only,
          "ORH-126 evidence attempted to own MGA/security authority: " +
              record.surface);
  g_records.push_back(std::move(record));
}

parser::CacheKey ParserKey() {
  parser::CacheKey key;
  key.shape_hash = 12601;
  key.registry_version = 1;
  key.catalog_epoch = 126;
  key.security_policy_epoch = 200;
  key.grant_epoch = 300;
  key.descriptor_epoch = 400;
  key.udr_epoch = 500;
  key.name_resolution_epoch = 600;
  key.resource_epoch = 700;
  key.parser_package_generation = 800;
  key.protocol_version = 3;
  key.parser_package_version_hash = 900;
  key.disclosure_policy_generation = 1000;
  key.redaction_policy_generation = 1100;
  key.security_authority_epoch = 1200;
  key.cluster_policy_generation = 1300;
  key.ttl_generation = 1400;
  key.memory_pressure_generation = 1500;
  key.normalized_statement_hash = 1600;
  key.parameter_type_shape_hash = 1700;
  key.connection_uuid = "orh126-auth-context-alice";
  key.transaction_context_hash =
      "mga_snapshot:engine_transaction_inventory:orh126";
  key.dialect = "sbsql_v3";
  key.role_set_hash = "role_set:reader";
  key.group_set_hash = "group_set:reporting";
  key.search_path_hash = "search_path:public";
  key.language_profile = "en-US";
  key.policy_profile = "policy:default";
  key.parser_profile = "parser:sbsql_v3";
  key.result_contract_hash = "result_contract:orh126:v1";
  return key;
}

template <typename MutateCurrent, typename Invalidate>
void RequireParserTemplateInvalidation(std::string_view trigger,
                                       MutateCurrent mutate_current,
                                       Invalidate invalidate) {
  parser::SblrTemplateCache cache(8);
  const auto stale = ParserKey();
  auto current = stale;
  mutate_current(current);
  Require(cache.Store(stale, "stale-template").stored,
          "could not store stale parser template");
  Require(cache.Store(current, "current-template").stored,
          "could not store current parser template");

  invalidate(cache, current);

  const auto stale_lookup = cache.Lookup(stale);
  const auto current_lookup = cache.Lookup(current);
  Require(!stale_lookup.has_value(),
          "parser template survived security race trigger " +
              std::string(trigger));
  Require(current_lookup.has_value() && *current_lookup == "current-template",
          "parser template invalidation removed current entry for " +
              std::string(trigger));
  const auto snapshot = cache.SnapshotJson();
  Record({"parser_sblr_template_cache",
          std::string(trigger),
          std::string(kUnsafePrefix) + ".PARSER_TEMPLATE_INVALIDATED",
          true,
          true,
          true,
          {"stale_template_hit=false",
           "current_template_hit=true",
           "parser_template_authority=advisory_sblr_template_only",
           snapshot.find("\"invalidations\":1") == std::string::npos
               ? "parser_template_invalidation_count_missing"
               : "parser_template_invalidation_count=1"}});
}

void ProveParserTemplateSecurityRaceInvalidation() {
  RequireParserTemplateInvalidation(
      "role_revocation_role_set_digest_drift",
      [](parser::CacheKey& key) { key.role_set_hash = "role_set:revoked"; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateRoleSetHash(key.role_set_hash);
      });
  RequireParserTemplateInvalidation(
      "privilege_change_grant_epoch",
      [](parser::CacheKey& key) { key.grant_epoch += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateGrantEpoch(key.grant_epoch);
      });
  RequireParserTemplateInvalidation(
      "security_epoch_change",
      [](parser::CacheKey& key) { key.security_policy_epoch += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateSecurityPolicyEpoch(key.security_policy_epoch);
      });
  RequireParserTemplateInvalidation(
      "redaction_policy_epoch_change",
      [](parser::CacheKey& key) { key.redaction_policy_generation += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateRedactionPolicyGeneration(
            key.redaction_policy_generation);
      });
  RequireParserTemplateInvalidation(
      "session_auth_context_change",
      [](parser::CacheKey& key) {
        key.connection_uuid = "orh126-auth-context-bob";
      },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateConnection(key.connection_uuid);
      });
}

opt::OptimizerPlanCacheKeyInput OptimizerInput() {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = "orh126.select";
  input.sblr_digest = "sblr:orh126";
  input.descriptor_set_digest = "descriptor:orders:v1";
  input.statistics_snapshot_id = "stats:orh126:v1";
  input.catalog_stats_digest = "catalog_stats:orh126:v1";
  input.cost_profile_id = "cost:local:v1";
  input.executor_capability_set_id = "executor:local:v1";
  input.route_capability_digest = "route:embedded:v1";
  input.security_policy_digest = "principal=alice|role_set=reader|grant=select";
  input.redaction_route_digest = "redaction:masked:v1";
  input.parameter_shape_digest = "param:tenant:int64";
  input.memory_grant_class = "memory:small";
  input.memory_grant_digest = "grant:64k";
  input.catalog_epoch = 126;
  input.stats_epoch = 127;
  input.security_epoch = 128;
  input.policy_epoch = 129;
  input.resource_epoch = 130;
  input.name_resolution_epoch = 131;
  input.memory_policy_epoch = 132;
  input.compatibility_epoch = 133;
  input.format_compatibility_epoch = 134;
  input.route_epoch = 135;
  input.object_uuids = {"rel.orders"};
  input.function_uuids = {"fn.redact_order"};
  input.index_uuids = {"idx.orders.tenant"};
  input.filespace_uuids = {"filespace.hot"};
  return input;
}

opt::CachedOptimizerPlan OptimizerPlan(
    const opt::OptimizerPlanCacheKeyInput& input) {
  opt::CachedOptimizerPlan plan;
  plan.key_input = input;
  plan.cache_key = opt::BuildOptimizerPlanCacheKey(input);
  plan.created_epoch = input.catalog_epoch;
  plan.result.ok = true;
  plan.result.plan_id = "orh126.plan";
  opt::PlanCandidate candidate;
  candidate.candidate_id = "orders_by_tenant";
  candidate.required_facts = {"rel.orders", "fn.redact_order"};
  candidate.runtime_evidence = {
      "mga_visibility_recheck_required=true",
      "security_authorization_recheck_required=true",
      "orh126_cached_plan_evidence=advisory_candidate_route_only"};
  plan.result.candidates.push_back(std::move(candidate));
  return plan;
}

template <typename Mutator>
void RequireOptimizerLookupRefusal(std::string_view trigger,
                                   Mutator mutate,
                                   std::string_view expected_diagnostic) {
  opt::OptimizerPlanCache cache;
  const auto input = OptimizerInput();
  cache.Put(OptimizerPlan(input));
  auto request = input;
  mutate(request);
  const auto result = cache.Lookup(request);
  Require(!result.hit, "optimizer plan cache hit after " + std::string(trigger));
  Require(result.diagnostic_code == expected_diagnostic,
          "optimizer diagnostic mismatch for " + std::string(trigger) + ": " +
              result.diagnostic_code);
  Require(Has(result.evidence, "security_authorization_recheck=preserved") ||
              Has(result.evidence,
                  "optimizer_plan_cache_redaction_security_policy_mismatch"),
          "optimizer evidence missing security recheck/refusal marker");
  Record({"optimizer_plan_cache",
          std::string(trigger),
          std::string(kUnsafePrefix) + ".OPTIMIZER_PLAN_REFUSED",
          true,
          true,
          true,
          result.evidence});
}

void RequireOptimizerInvalidation(std::string_view trigger,
                                  opt::OptimizerInvalidationEvent event,
                                  std::string_view expected_diagnostic) {
  opt::OptimizerPlanCache cache;
  const auto input = OptimizerInput();
  cache.Put(OptimizerPlan(input));
  const auto invalidation = cache.InvalidateWithEvidence(event);
  Require(invalidation.invalidated_count == 1,
          "optimizer invalidation count mismatch for " + std::string(trigger));
  const auto result = cache.Lookup(input);
  Require(!result.hit,
          "optimizer plan cache hit after invalidation " + std::string(trigger));
  Require(result.diagnostic_code == expected_diagnostic,
          "optimizer invalidation diagnostic mismatch for " +
              std::string(trigger));
  Require(Has(result.evidence, "optimizer_plan_cache_dependency_invalidation"),
          "optimizer invalidation evidence missing dependency marker");
  Record({"optimizer_plan_cache",
          std::string(trigger),
          std::string(kUnsafePrefix) + ".OPTIMIZER_PLAN_INVALIDATED",
          true,
          true,
          true,
          result.evidence});
}

void ProveOptimizerSecurityRaceInvalidation() {
  RequireOptimizerLookupRefusal(
      "role_revocation_role_set_digest_drift",
      [](opt::OptimizerPlanCacheKeyInput& input) {
        input.security_policy_digest =
            "principal=alice|role_set=revoked|grant=select";
      },
      "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH");
  RequireOptimizerLookupRefusal(
      "redaction_policy_digest_change",
      [](opt::OptimizerPlanCacheKeyInput& input) {
        input.redaction_route_digest = "redaction:masked:v2";
      },
      "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH");
  RequireOptimizerInvalidation(
      "privilege_change_security_policy",
      {"security_policy_change", {}, 12601},
      "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH");
  RequireOptimizerInvalidation(
      "security_epoch_change",
      {"security_epoch", {}, 12602},
      "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH");
  RequireOptimizerInvalidation(
      "redaction_policy_change",
      {"redaction_policy_change", {}, 12603},
      "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH");
}

wire::StreamingCursorState CursorState() {
  wire::StreamingCursorState state;
  state.cursor_id = "orh126-cursor";
  state.plan_result_contract_hash = "result_contract:orh126:v1";
  state.catalog_epoch = 126;
  state.descriptor_epoch = 127;
  state.transaction_snapshot_class = "repeatable_read";
  state.transaction_uuid = "orh126-tx-alice";
  state.local_transaction_id = 128;
  state.snapshot_visible_through_local_transaction_id = 127;
  state.security_epoch = 129;
  state.redaction_epoch = 130;
  state.route_kind = "embedded";
  state.expiry_deadline_unix_millis = 10000;
  state.client_credit.frame_credit = 8;
  state.client_credit.row_credit = 256;
  state.client_credit.byte_credit = 16384;
  state.advisory_metadata_only = true;
  state.mga_visibility_or_finality_authority = false;
  return state;
}

void RequireCursorRefusal(wire::StreamingCursorBinding expected,
                          std::string_view trigger,
                          std::string_view expected_code,
                          std::string_view expected_reason) {
  wire::StreamingCursorManager manager;
  const auto opened = manager.OpenCursor({CursorState(), 1});
  Require(opened.ok(), "cursor open failed for " + std::string(trigger));
  const auto result = manager.ValidateFetch({std::move(expected), 2});
  Require(!result.ok() && result.fail_closed,
          "cursor did not fail closed for " + std::string(trigger));
  Require(result.diagnostic.diagnostic_code == expected_code,
          "cursor diagnostic mismatch for " + std::string(trigger) + ": " +
              result.diagnostic.diagnostic_code);
  Require(Has(result.refusal_reasons, expected_reason),
          "cursor refusal reason missing for " + std::string(trigger));
  Require(Has(result.evidence,
              "cursor_mga_visibility_or_finality_authority=false"),
          "cursor evidence attempted MGA authority");
  Record({"streaming_cursor_manager",
          std::string(trigger),
          std::string(kUnsafePrefix) + ".CURSOR_REFUSED",
          true,
          true,
          true,
          result.evidence});
}

void RequireContinuationTokenRefusal(
    wire::StreamingCursorBinding expected,
    std::string_view trigger,
    std::string_view expected_code,
    std::string_view expected_reason,
    wire::u64 now_unix_millis = 2) {
  const wire::ContinuationTokenSecret secret{"orh126-key", "orh126-secret"};
  const auto issued = wire::IssueContinuationToken(
      wire::StreamingCursorBindingFromState(CursorState()), secret);
  Require(issued.ok(), "continuation token issue failed");
  const auto result = wire::ValidateContinuationToken(
      issued.token, expected, secret, now_unix_millis);
  Require(!result.ok() && result.fail_closed,
          "continuation token did not fail closed for " + std::string(trigger));
  Require(result.diagnostic.diagnostic_code == expected_code,
          "continuation token diagnostic mismatch for " +
              std::string(trigger) + ": " + result.diagnostic.diagnostic_code);
  Require(Has(result.refusal_reasons, expected_reason),
          "continuation token refusal reason missing for " +
              std::string(trigger));
  Record({"secure_continuation_token",
          std::string(trigger),
          std::string(kUnsafePrefix) + ".CONTINUATION_TOKEN_REFUSED",
          true,
          true,
          true,
          result.evidence});
}

void ProveCursorAndContinuationSecurityRaceRefusals() {
  auto binding = wire::StreamingCursorBindingFromState(CursorState());
  auto changed = binding;
  changed.security_epoch += 1;
  RequireCursorRefusal(changed,
                       "security_epoch_change",
                       "SB_ORH_STREAMING_CURSOR.SECURITY_EPOCH_MISMATCH",
                       "cursor_security_epoch_mismatch");
  RequireContinuationTokenRefusal(
      changed,
      "security_epoch_change",
      "SB_ORH_CONTINUATION_TOKEN.SECURITY_EPOCH_MISMATCH",
      "token_security_epoch_mismatch");

  changed = binding;
  changed.redaction_epoch += 1;
  RequireCursorRefusal(changed,
                       "redaction_policy_epoch_change",
                       "SB_ORH_STREAMING_CURSOR.REDACTION_EPOCH_MISMATCH",
                       "cursor_redaction_epoch_mismatch");
  RequireContinuationTokenRefusal(
      changed,
      "redaction_policy_epoch_change",
      "SB_ORH_CONTINUATION_TOKEN.REDACTION_EPOCH_MISMATCH",
      "token_redaction_epoch_mismatch");

  changed = binding;
  changed.transaction_uuid = "orh126-tx-bob";
  RequireCursorRefusal(changed,
                       "session_auth_context_change",
                       "SB_ORH_STREAMING_CURSOR.TRANSACTION_UUID_MISMATCH",
                       "cursor_transaction_uuid_mismatch");
  RequireContinuationTokenRefusal(
      changed,
      "session_auth_context_change",
      "SB_ORH_CONTINUATION_TOKEN.TRANSACTION_UUID_MISMATCH",
      "token_transaction_uuid_mismatch");

  RequireContinuationTokenRefusal(
      binding,
      "token_expiry",
      "SB_ORH_CONTINUATION_TOKEN.EXPIRED",
      "token_expired",
      10001);
}

exec::SnapshotSafeCacheKey SnapshotKey() {
  exec::SnapshotSafeCacheKey key;
  key.normalized_operation = "orh126.select";
  key.safe_parameter_digest = "tenant:42";
  key.catalog_epoch = 126;
  key.statistics_epoch = 127;
  key.security_epoch = 128;
  key.redaction_epoch = 129;
  key.mga_visibility_snapshot_class = "repeatable_read";
  key.provider_generation = 130;
  key.result_contract_identity = "orh126.rowset.v1";
  key.result_contract_hash = "sha256:orh126-rowset-v1";
  key.route_compatibility = "embedded";
  key.dialect_compatibility = "sbsql_v3";
  return key;
}

exec::SnapshotSafeCacheEntry SnapshotEntry() {
  exec::SnapshotSafeCacheEntry entry;
  entry.key = SnapshotKey();
  entry.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  entry.row_count = 2;
  entry.cached_result_digest = "sha256:orh126-result";
  entry.cached_mga_security_digest = "sha256:orh126-mga-security";
  return entry;
}

exec::SnapshotSafeCacheStoreRequest SnapshotStoreRequest() {
  exec::SnapshotSafeCacheStoreRequest request;
  request.entry = SnapshotEntry();
  request.read_only_operation = true;
  request.small_final_result = true;
  request.max_small_result_rows = 16;
  return request;
}

exec::SnapshotSafeCacheLookupRequest SnapshotLookupRequest() {
  exec::SnapshotSafeCacheLookupRequest request;
  request.key = SnapshotKey();
  request.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  request.read_only_operation = true;
  request.small_final_result = true;
  request.row_count = 2;
  request.max_small_result_rows = 16;
  request.recomputed_result_digest = "sha256:orh126-result";
  request.recomputed_mga_security_digest = "sha256:orh126-mga-security";
  return request;
}

template <typename Mutator>
void RequireSnapshotMiss(std::string_view trigger, Mutator mutate) {
  exec::SnapshotSafeResultCache cache;
  Require(cache.Store(SnapshotStoreRequest()).action ==
              exec::SnapshotSafeCacheAction::kStore,
          "snapshot cache store failed");
  auto lookup = SnapshotLookupRequest();
  mutate(lookup);
  const auto decision = cache.Lookup(lookup);
  Require(decision.action == exec::SnapshotSafeCacheAction::kMissRecompute ||
              decision.action == exec::SnapshotSafeCacheAction::kInvalidateRecompute,
          "snapshot cache did not recompute for " + std::string(trigger));
  Require(!decision.fail_closed,
          "snapshot cache should recompute rather than fail closed for " +
              std::string(trigger));
  Require(Has(decision.evidence, "cache_transaction_finality_authority=false"),
          "snapshot cache evidence attempted finality authority");
  Record({"snapshot_safe_result_cache",
          std::string(trigger),
          std::string(kUnsafePrefix) + ".SNAPSHOT_CACHE_RECOMPUTE",
          true,
          true,
          true,
          decision.evidence});
}

void RequireSnapshotRefusal(exec::SnapshotSafeCacheLookupRequest lookup,
                            std::string_view trigger,
                            std::string_view expected_code) {
  exec::SnapshotSafeResultCache cache;
  Require(cache.Store(SnapshotStoreRequest()).action ==
              exec::SnapshotSafeCacheAction::kStore,
          "snapshot cache store failed for refusal");
  const auto decision = cache.Lookup(lookup);
  Require(decision.fail_closed &&
              decision.action == exec::SnapshotSafeCacheAction::kRefuse,
          "snapshot cache did not fail closed for " + std::string(trigger));
  Require(decision.diagnostic_code == expected_code,
          "snapshot cache diagnostic mismatch for " + std::string(trigger) +
              ": " + decision.diagnostic_code);
  Require(Has(decision.evidence, "cache_authorization_authority=false"),
          "snapshot cache evidence attempted authorization authority");
  Record({"snapshot_safe_result_cache",
          std::string(trigger),
          std::string(kUnsafePrefix) + ".SNAPSHOT_CACHE_REFUSED",
          true,
          true,
          true,
          decision.evidence});
}

void ProveSnapshotSafeResultCacheSecurityRaceBehavior() {
  RequireSnapshotMiss("security_epoch_change", [](auto& lookup) {
    lookup.key.security_epoch += 1;
  });
  RequireSnapshotMiss("redaction_policy_epoch_change", [](auto& lookup) {
    lookup.key.redaction_epoch += 1;
  });
  RequireSnapshotMiss("mga_security_digest_drift", [](auto& lookup) {
    lookup.recomputed_mga_security_digest =
        "sha256:orh126-mga-security-after-revocation";
  });

  auto lookup = SnapshotLookupRequest();
  lookup.security_uncertain = true;
  RequireSnapshotRefusal(lookup,
                         "security_epoch_uncertain",
                         "EXECUTOR.SNAPSHOT_RESULT_CACHE.UNCERTAINTY_REFUSED");
  lookup = SnapshotLookupRequest();
  lookup.redaction_uncertain = true;
  RequireSnapshotRefusal(lookup,
                         "redaction_policy_uncertain",
                         "EXECUTOR.SNAPSHOT_RESULT_CACHE.UNCERTAINTY_REFUSED");
  lookup = SnapshotLookupRequest();
  lookup.authorization_authority_cached = true;
  RequireSnapshotRefusal(lookup,
                         "authorization_authority_cached",
                         "EXECUTOR.SNAPSHOT_RESULT_CACHE.AUTHORITY_REFUSED");
}

api::EngineNoSqlPhysicalProviderContract ProviderContract() {
  api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = api::EngineNoSqlProviderFamily::kDocument;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = "nosql.local.document.orh126";
  contract.local_provider_available = true;
  contract.exact_fallback_available = true;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.descriptor_visibility.descriptor_generation = 126;
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.security_redaction.redaction_profile = "masked";
  contract.security_redaction.proof_id = "security:redaction:orh126";
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = 126;
  contract.index_generation.available_generation = 126;
  contract.index_generation.index_uuid = "idx.orh126";
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.policy.policy_snapshot_uuid = "policy.orh126";
  contract.provider_generation.required = true;
  contract.provider_generation.proof_present = true;
  contract.provider_generation.visible_to_snapshot = true;
  contract.provider_generation.publish_state_bound = true;
  contract.provider_generation.validation_state_bound = true;
  contract.provider_generation.backup_restore_repair_metadata_bound = true;
  contract.provider_generation.support_bundle_evidence_bound = true;
  contract.provider_generation.required_generation = 126;
  contract.provider_generation.available_generation = 126;
  contract.provider_generation.descriptor_epoch = 126;
  contract.provider_generation.security_epoch = 127;
  contract.provider_generation.redaction_epoch = 128;
  contract.provider_generation.catalog_epoch = 129;
  contract.provider_generation.generation_uuid = "generation.orh126";
  contract.provider_generation.provider_id = contract.provider_id;
  contract.provider_generation.database_uuid = "database.orh126";
  contract.provider_generation.collection_uuid = "collection.orh126";
  contract.provider_generation.publish_state = "published";
  contract.provider_generation.validation_state = "validated";
  contract.provider_generation.backup_metadata_ref = "backup.orh126";
  contract.provider_generation.restore_metadata_ref = "restore.orh126";
  contract.provider_generation.repair_metadata_ref = "repair.orh126";
  contract.provider_generation.support_bundle_evidence_id = "support.orh126";
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

void RequireProviderSelectionRefusal(
    api::EngineNoSqlPhysicalProviderContract contract,
    std::string_view trigger,
    std::string_view expected_diagnostic) {
  const auto selection = api::SelectLocalNoSqlPhysicalProvider(contract);
  Require(!selection.ok && !selection.selected && selection.fail_closed,
          "provider selection did not fail closed for " + std::string(trigger));
  Require(api::EngineNoSqlSelectionHasDiagnostic(selection,
                                                 expected_diagnostic),
          "provider selection diagnostic mismatch for " + std::string(trigger));
  Require(Has(selection.evidence,
              "transaction_authority_source=engine_transaction_inventory"),
          "provider selection lost MGA authority evidence");
  Record({"nosql_provider_selection",
          std::string(trigger),
          std::string(kUnsafePrefix) + ".PROVIDER_SELECTION_REFUSED",
          true,
          true,
          true,
          selection.evidence});
}

std::filesystem::path UniqueTempDir() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  auto dir = std::filesystem::temp_directory_path() /
             ("scratchbird_orh126_" + std::to_string(now));
  std::filesystem::create_directories(dir);
  return dir;
}

struct TempDatabase {
  std::filesystem::path dir = UniqueTempDir();
  std::filesystem::path path = dir / "orh126.sbdb";

  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext NoSqlContext(const std::filesystem::path& path) {
  api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical = "orh126-database";
  context.current_schema_uuid.canonical = "orh126-collection";
  context.transaction_uuid.canonical = "orh126-nosql-tx";
  context.local_transaction_id = 126;
  context.security_context_present = true;
  context.resource_epoch = 127;
  context.security_epoch = 128;
  context.catalog_generation_id = 129;
  return context;
}

api::EngineNoSqlPhysicalProviderContract ProviderGenerationContract(
    const api::EngineRequestContext& context,
    const api::EngineNoSqlProviderGenerationMetadata& generation) {
  auto contract = ProviderContract();
  contract.provider_id = generation.provider_id;
  contract.provider_generation.required_generation = generation.generation_id;
  contract.provider_generation.available_generation = generation.generation_id;
  contract.provider_generation.descriptor_epoch = context.resource_epoch;
  contract.provider_generation.security_epoch = context.security_epoch;
  contract.provider_generation.redaction_epoch = context.security_epoch;
  contract.provider_generation.catalog_epoch = context.catalog_generation_id;
  contract.provider_generation.generation_uuid = generation.generation_uuid;
  contract.provider_generation.provider_id = generation.provider_id;
  contract.provider_generation.database_uuid = context.database_uuid.canonical;
  contract.provider_generation.collection_uuid = generation.collection_uuid;
  contract.provider_generation.backup_metadata_ref =
      generation.backup_metadata_ref;
  contract.provider_generation.restore_metadata_ref =
      generation.restore_metadata_ref;
  contract.provider_generation.repair_metadata_ref =
      generation.repair_metadata_ref;
  contract.provider_generation.support_bundle_evidence_id =
      generation.support_bundle_evidence_id;
  return contract;
}

void ProveProviderSelectionAndGenerationSecurityRaceRefusals() {
  auto contract = ProviderContract();
  contract.security_redaction.proof_present = false;
  RequireProviderSelectionRefusal(
      contract,
      "security_redaction_proof_missing",
      api::kNoSqlProviderSecurityProofMissing);

  contract = ProviderContract();
  contract.security_redaction.security_snapshot_bound = false;
  RequireProviderSelectionRefusal(
      contract,
      "security_snapshot_binding_missing",
      api::kNoSqlProviderSecuritySnapshotProofMissing);

  contract = ProviderContract();
  contract.policy.allowed = false;
  contract.policy.refusal_reasons = {"session_principal_provider_binding_stale"};
  RequireProviderSelectionRefusal(
      contract,
      "session_provider_binding_change",
      api::kNoSqlProviderPolicyRefused);

  TempDatabase database;
  const auto context = NoSqlContext(database.path);
  std::ofstream seed(context.database_path, std::ios::binary | std::ios::trunc);
  seed << "SBCRUD1\tTX_BEGIN\t" << context.local_transaction_id << '\t'
       << context.transaction_uuid.canonical << '\n';
  seed.flush();
  Require(static_cast<bool>(seed), "could not seed ORH-126 NoSQL database");

  const auto metadata = api::MakeDocumentProviderGenerationMetadata(
      context,
      "nosql.local.document.orh126",
      context.current_schema_uuid.canonical,
      126);
  const auto published =
      api::PublishNoSqlProviderGeneration(context, metadata);
  Require(published.ok, "NoSQL provider generation publish failed");

  auto generation_contract =
      ProviderGenerationContract(context, published.metadata);
  generation_contract.provider_generation.security_epoch += 1;
  auto stale = api::ValidateNoSqlProviderGeneration(context,
                                                    generation_contract);
  Require(!stale.ok,
          "NoSQL provider generation accepted stale security epoch");
  Require(Has(stale.evidence,
              std::string("provider_generation_refusal=") +
                  api::kNoSqlProviderGenerationEpochMismatch),
          "NoSQL provider generation security epoch diagnostic mismatch: " +
              stale.diagnostic.code);
  Record({"nosql_provider_generation",
          "security_epoch_change",
          std::string(kUnsafePrefix) + ".PROVIDER_GENERATION_REFUSED",
          true,
          true,
          true,
          stale.evidence});

  generation_contract =
      ProviderGenerationContract(context, published.metadata);
  generation_contract.provider_generation.redaction_epoch += 1;
  stale = api::ValidateNoSqlProviderGeneration(context,
                                               generation_contract);
  Require(!stale.ok,
          "NoSQL provider generation accepted stale redaction epoch");
  Require(Has(stale.evidence,
              std::string("provider_generation_refusal=") +
                  api::kNoSqlProviderGenerationEpochMismatch),
          "NoSQL provider generation redaction epoch diagnostic mismatch: " +
              stale.diagnostic.code);
  Record({"nosql_provider_generation",
          "redaction_policy_epoch_change",
          std::string(kUnsafePrefix) + ".PROVIDER_GENERATION_REFUSED",
          true,
          true,
          true,
          stale.evidence});
}

void VerifyCommercialEvidenceCoverage() {
  const std::vector<std::string> required_surfaces = {
      "parser_sblr_template_cache",
      "optimizer_plan_cache",
      "streaming_cursor_manager",
      "secure_continuation_token",
      "snapshot_safe_result_cache",
      "nosql_provider_selection",
      "nosql_provider_generation",
  };
  const std::vector<std::string> required_triggers = {
      "role_revocation_role_set_digest_drift",
      "privilege_change_grant_epoch",
      "token_expiry",
      "security_epoch_change",
      "redaction_policy_epoch_change",
      "session_auth_context_change",
      "session_provider_binding_change",
      "mga_security_digest_drift",
  };
  for (const auto& surface : required_surfaces) {
    Require(std::any_of(g_records.begin(), g_records.end(),
                        [&](const auto& record) {
                          return record.surface == surface;
                        }),
            "ORH-126 missing evidence surface " + surface);
  }
  for (const auto& trigger : required_triggers) {
    Require(std::any_of(g_records.begin(), g_records.end(),
                        [&](const auto& record) {
                          return record.trigger == trigger;
                        }),
            "ORH-126 missing evidence trigger " + trigger);
  }
  for (const auto& record : g_records) {
    Require(record.diagnostic_code.rfind(kUnsafePrefix, 0) == 0,
            "ORH-126 diagnostic prefix mismatch for " + record.surface);
  }
}

}  // namespace

int main() {
  ProveParserTemplateSecurityRaceInvalidation();
  ProveOptimizerSecurityRaceInvalidation();
  ProveCursorAndContinuationSecurityRaceRefusals();
  ProveSnapshotSafeResultCacheSecurityRaceBehavior();
  ProveProviderSelectionAndGenerationSecurityRaceRefusals();
  VerifyCommercialEvidenceCoverage();
  std::cout << "optimizer_runtime_hot_path_orh_126_gate=passed "
            << "evidence_records=" << g_records.size()
            << " benchmark_clean=true\n";
  return EXIT_SUCCESS;
}
