// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cache/sblr_template_cache.hpp"
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
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace parser = scratchbird::parser::sbsql;
namespace wire = scratchbird::wire;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-122 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
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

bool DiagnosticContains(const api::EngineNoSqlProviderGenerationResult& result,
                        std::string_view token) {
  return result.diagnostic.code.find(token) != std::string::npos ||
         result.diagnostic.detail.find(token) != std::string::npos ||
         HasPrefix(result.evidence, std::string("provider_generation_refusal=") +
                                    std::string(token));
}

parser::CacheKey ParserCacheKey() {
  parser::CacheKey key;
  key.shape_hash = 12201;
  key.registry_version = 3;
  key.catalog_epoch = 122;
  key.security_policy_epoch = 123;
  key.grant_epoch = 124;
  key.descriptor_epoch = 125;
  key.udr_epoch = 126;
  key.name_resolution_epoch = 127;
  key.resource_epoch = 128;
  key.parser_package_generation = 129;
  key.protocol_version = 3;
  key.parser_package_version_hash = 130;
  key.disclosure_policy_generation = 131;
  key.redaction_policy_generation = 132;
  key.security_authority_epoch = 133;
  key.cluster_policy_generation = 134;
  key.ttl_generation = 135;
  key.memory_pressure_generation = 136;
  key.normalized_statement_hash = 137;
  key.parameter_type_shape_hash = 138;
  key.connection_uuid = "orh122-connection";
  key.transaction_context_hash =
      "mga_snapshot:engine_transaction_inventory:orh122";
  key.dialect = "sbsql_v3";
  key.role_set_hash = "roles/report_reader";
  key.group_set_hash = "groups/reporting";
  key.search_path_hash = "search_path/public";
  key.language_profile = "en-US";
  key.policy_profile = "policy/default";
  key.parser_profile = "parser/sbsql_v3";
  key.result_contract_hash = "result_contract/orh122/v1";
  return key;
}

template <typename Mutator, typename Invalidator>
void ProveParserChurnCause(std::string_view cause,
                           Mutator mutate,
                           Invalidator invalidate) {
  parser::SblrTemplateCache cache(8);
  const auto stale = ParserCacheKey();
  auto current = stale;
  mutate(current);
  Require(cache.Store(stale, "stale-sblr-template").stored,
          std::string(cause) + " stale parser template was not stored");
  Require(cache.Store(current, "current-sblr-template").stored,
          std::string(cause) + " current parser template was not stored");

  std::thread invalidator([&]() { invalidate(cache, current); });
  std::thread concurrent_reader([&]() {
    (void)cache.Lookup(current);
    (void)cache.Lookup(stale);
  });
  invalidator.join();
  concurrent_reader.join();

  Require(!cache.Lookup(stale).has_value(),
          std::string(cause) + " churn retained stale parser SBLR template");
  const auto retained = cache.Lookup(current);
  Require(retained.has_value() && *retained == "current-sblr-template",
          std::string(cause) + " churn removed current parser template");
  Require(cache.SnapshotJson().find("\"invalidations\":1") !=
              std::string::npos,
          std::string(cause) + " churn did not record parser invalidation");
}

void ProveParserFrontDoorConcurrentChurnInvalidation() {
  ProveParserChurnCause(
      "catalog epoch",
      [](parser::CacheKey& key) { key.catalog_epoch += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateCatalogEpoch(key.catalog_epoch);
      });
  ProveParserChurnCause(
      "security policy epoch",
      [](parser::CacheKey& key) { key.security_policy_epoch += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateSecurityPolicyEpoch(key.security_policy_epoch);
      });
  ProveParserChurnCause(
      "grant epoch",
      [](parser::CacheKey& key) { key.grant_epoch += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateGrantEpoch(key.grant_epoch);
      });
  ProveParserChurnCause(
      "descriptor epoch",
      [](parser::CacheKey& key) { key.descriptor_epoch += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateDescriptorEpoch(key.descriptor_epoch);
      });
  ProveParserChurnCause(
      "name resolution epoch",
      [](parser::CacheKey& key) { key.name_resolution_epoch += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateNameResolutionEpoch(key.name_resolution_epoch);
      });
  ProveParserChurnCause(
      "resource epoch",
      [](parser::CacheKey& key) { key.resource_epoch += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateResourceEpoch(key.resource_epoch);
      });
  ProveParserChurnCause(
      "parser package generation",
      [](parser::CacheKey& key) { key.parser_package_generation += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateParserPackageGeneration(
            key.parser_package_generation);
      });
  ProveParserChurnCause(
      "protocol version",
      [](parser::CacheKey& key) { key.protocol_version += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateProtocolVersion(key.protocol_version);
      });
  ProveParserChurnCause(
      "parser package version hash",
      [](parser::CacheKey& key) { key.parser_package_version_hash += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateParserPackageVersionHash(
            key.parser_package_version_hash);
      });
  ProveParserChurnCause(
      "disclosure policy generation",
      [](parser::CacheKey& key) { key.disclosure_policy_generation += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateDisclosurePolicyGeneration(
            key.disclosure_policy_generation);
      });
  ProveParserChurnCause(
      "redaction policy generation",
      [](parser::CacheKey& key) { key.redaction_policy_generation += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateRedactionPolicyGeneration(
            key.redaction_policy_generation);
      });
  ProveParserChurnCause(
      "role set",
      [](parser::CacheKey& key) { key.role_set_hash = "roles/report_admin"; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateRoleSetHash(key.role_set_hash);
      });
  ProveParserChurnCause(
      "group set",
      [](parser::CacheKey& key) { key.group_set_hash = "groups/audit"; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateGroupSetHash(key.group_set_hash);
      });
  ProveParserChurnCause(
      "search path",
      [](parser::CacheKey& key) { key.search_path_hash = "search_path/audit"; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateSearchPathHash(key.search_path_hash);
      });
  ProveParserChurnCause(
      "policy profile",
      [](parser::CacheKey& key) { key.policy_profile = "policy/restricted"; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidatePolicyProfile(key.policy_profile);
      });
  ProveParserChurnCause(
      "parser profile",
      [](parser::CacheKey& key) { key.parser_profile = "parser/audit"; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateParserProfile(key.parser_profile);
      });
  ProveParserChurnCause(
      "parameter shape",
      [](parser::CacheKey& key) { key.parameter_type_shape_hash += 1; },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateParameterTypeShapeHash(
            key.parameter_type_shape_hash);
      });
  ProveParserChurnCause(
      "result contract",
      [](parser::CacheKey& key) {
        key.result_contract_hash = "result_contract/orh122/v2";
      },
      [](parser::SblrTemplateCache& cache, const parser::CacheKey& key) {
        cache.InvalidateResultContractHash(key.result_contract_hash);
      });

  parser::CacheEntry unsafe;
  unsafe.key = ParserCacheKey();
  unsafe.sblr_payload = "unsafe-template";
  unsafe.finality_authority_cached = true;
  const auto refusal = parser::SblrTemplateCache(2).StoreEntry(unsafe);
  Require(!refusal.stored,
          "parser front-door cache accepted finality authority");
  Require(refusal.diagnostic_code ==
              "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.FINALITY_AUTHORITY_CACHED",
          "parser cache authority refusal diagnostic mismatch");
}

opt::OptimizerPlanCacheKeyInput OptimizerInput() {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = "orh122.select";
  input.sblr_digest = "sblr:orh122";
  input.descriptor_set_digest = "descriptor:customer:v1";
  input.statistics_snapshot_id = "stats:orh122:v1";
  input.catalog_stats_digest = "catalog_stats:orh122:v1";
  input.cost_profile_id = "cost:local:v1";
  input.executor_capability_set_id = "executor:embedded:v1";
  input.route_capability_digest = "route:embedded:v1";
  input.security_policy_digest = "security:reader:v1";
  input.redaction_route_digest = "redaction:masked:v1";
  input.parameter_shape_digest = "param:tenant:int64:v1";
  input.memory_grant_class = "memory:small";
  input.memory_grant_digest = "grant:64k";
  input.catalog_epoch = 122;
  input.stats_epoch = 123;
  input.security_epoch = 124;
  input.policy_epoch = 125;
  input.resource_epoch = 126;
  input.name_resolution_epoch = 127;
  input.memory_policy_epoch = 128;
  input.compatibility_epoch = 129;
  input.format_compatibility_epoch = 130;
  input.route_epoch = 131;
  input.object_uuids = {"rel.customer"};
  input.function_uuids = {"fn.mask"};
  input.index_uuids = {"idx.customer_tenant"};
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
  plan.result.plan_id = "orh122.plan";
  opt::PlanCandidate candidate;
  candidate.candidate_id = "scan:rel.customer:idx.customer_tenant";
  candidate.required_facts = {"rel.customer", "fn.mask", "filespace.hot"};
  candidate.runtime_evidence = {
      "mga_visibility_recheck_required=true",
      "security_authorization_recheck_required=true"};
  plan.result.candidates.push_back(std::move(candidate));
  return plan;
}

void RequireOptimizerInvalidation(
    const opt::OptimizerInvalidationEvent& event,
    std::string_view expected_diagnostic,
    std::string_view expected_cause) {
  opt::OptimizerPlanCache cache;
  const auto input = OptimizerInput();
  cache.Put(OptimizerPlan(input));

  std::thread churn([&]() { (void)cache.InvalidateWithEvidence(event); });
  churn.join();

  const auto result = cache.Lookup(input);
  Require(!result.hit,
          std::string("optimizer plan cache hit after churn cause ") +
              std::string(expected_cause));
  Require(result.diagnostic_code == expected_diagnostic,
          std::string("optimizer diagnostic mismatch for ") +
              std::string(expected_cause) + ": " + result.diagnostic_code);
  Require(Has(result.evidence, std::string("invalidation_kind=") +
                               std::string(expected_cause)),
          std::string("optimizer evidence missing churn cause ") +
              std::string(expected_cause));
  Require(Has(result.evidence,
              "optimizer_plan_cache_dependency_invalidation"),
          "optimizer evidence missing dependency invalidation marker");
}

void ProveOptimizerPlanCacheChurnInvalidation() {
  RequireOptimizerInvalidation({"catalog_epoch", {}, 1221},
                               "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
                               "catalog_epoch");
  RequireOptimizerInvalidation({"statistics_refresh", {}, 1222},
                               "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
                               "statistics_refresh");
  RequireOptimizerInvalidation(
      {"security_policy_change", {}, 1223},
      "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH",
      "security_policy_change");
  RequireOptimizerInvalidation(
      {"redaction_policy_change", {}, 1224},
      "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH",
      "redaction_policy_change");
  RequireOptimizerInvalidation({"nosql_generation_publish", {}, 1225},
                               "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
                               "nosql_generation_publish");
  RequireOptimizerInvalidation({"route_capability_change", {}, 1226},
                               "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH",
                               "route_capability_change");
  RequireOptimizerInvalidation({"format_change", {}, 1227},
                               "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
                               "format_change");

  opt::OptimizerPlanCache cache;
  const auto input = OptimizerInput();
  cache.Put(OptimizerPlan(input));
  auto changed = input;
  changed.catalog_epoch += 1;
  auto miss = cache.Lookup(changed);
  Require(!miss.hit &&
              miss.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
          "optimizer epoch-key miss did not fail closed on catalog churn");
  changed = input;
  changed.parameter_shape_digest = "param:tenant:text:v2";
  miss = cache.Lookup(changed);
  Require(!miss.hit &&
              miss.diagnostic_code ==
                  "SB_OPTIMIZER_PLAN_CACHE_INCOMPATIBLE_PARAMETER_SHAPE",
          "optimizer parameter-shape churn diagnostic mismatch");
}

wire::StreamingCursorState CursorState() {
  wire::StreamingCursorState state;
  state.cursor_id = "orh122-cursor";
  state.plan_result_contract_hash = "result_contract/orh122/v1";
  state.catalog_epoch = 122;
  state.descriptor_epoch = 123;
  state.transaction_snapshot_class = "repeatable_read";
  state.transaction_uuid = "orh122-tx";
  state.local_transaction_id = 124;
  state.snapshot_visible_through_local_transaction_id = 125;
  state.security_epoch = 126;
  state.redaction_epoch = 127;
  state.route_kind = "embedded";
  state.expiry_deadline_unix_millis = 10000;
  state.client_credit.frame_credit = 4;
  state.client_credit.row_credit = 128;
  state.client_credit.byte_credit = 8192;
  return state;
}

void RequireCursorRefusal(wire::StreamingCursorBinding expected,
                          std::string_view expected_code,
                          std::string_view expected_reason) {
  wire::StreamingCursorManager manager;
  const auto open = manager.OpenCursor({CursorState(), 1});
  Require(open.ok(), "cursor did not open for churn validation");
  const auto result = manager.ValidateFetch({std::move(expected), 2});
  Require(!result.ok() && result.fail_closed,
          "cursor fetch mismatch did not fail closed");
  Require(result.diagnostic.diagnostic_code == expected_code,
          "cursor churn diagnostic mismatch: " +
              result.diagnostic.diagnostic_code);
  Require(Has(result.refusal_reasons, expected_reason),
          "cursor churn refusal reason missing");
  Require(Has(result.evidence, std::string("fallback_refusal_reason=") +
                               std::string(expected_reason)),
          "cursor evidence missing exact churn cause");
  Require(Has(result.evidence,
              "cursor_mga_visibility_or_finality_authority=false"),
          "cursor evidence must preserve MGA authority boundary");
}

void ProveStreamingCursorFetchRefusals() {
  auto binding = wire::StreamingCursorBindingFromState(CursorState());
  auto changed = binding;
  changed.catalog_epoch += 1;
  RequireCursorRefusal(changed,
                       "SB_ORH_STREAMING_CURSOR.CATALOG_EPOCH_MISMATCH",
                       "cursor_catalog_epoch_mismatch");
  changed = binding;
  changed.descriptor_epoch += 1;
  RequireCursorRefusal(changed,
                       "SB_ORH_STREAMING_CURSOR.DESCRIPTOR_EPOCH_MISMATCH",
                       "cursor_descriptor_epoch_mismatch");
  changed = binding;
  changed.security_epoch += 1;
  RequireCursorRefusal(changed,
                       "SB_ORH_STREAMING_CURSOR.SECURITY_EPOCH_MISMATCH",
                       "cursor_security_epoch_mismatch");
  changed = binding;
  changed.redaction_epoch += 1;
  RequireCursorRefusal(changed,
                       "SB_ORH_STREAMING_CURSOR.REDACTION_EPOCH_MISMATCH",
                       "cursor_redaction_epoch_mismatch");
  changed = binding;
  changed.plan_result_contract_hash = "result_contract/orh122/v2";
  RequireCursorRefusal(changed,
                       "SB_ORH_STREAMING_CURSOR.CONTRACT_MISMATCH",
                       "cursor_result_contract_hash_mismatch");
  changed = binding;
  changed.transaction_snapshot_class = "read_committed";
  RequireCursorRefusal(
      changed,
      "SB_ORH_STREAMING_CURSOR.SNAPSHOT_CLASS_MISMATCH",
      "cursor_transaction_snapshot_class_mismatch");
  changed = binding;
  changed.transaction_uuid = "orh122-tx-new";
  RequireCursorRefusal(changed,
                       "SB_ORH_STREAMING_CURSOR.TRANSACTION_UUID_MISMATCH",
                       "cursor_transaction_uuid_mismatch");
  changed = binding;
  changed.snapshot_visible_through_local_transaction_id += 1;
  RequireCursorRefusal(
      changed,
      "SB_ORH_STREAMING_CURSOR.SNAPSHOT_VISIBLE_THROUGH_MISMATCH",
      "cursor_snapshot_visible_through_local_transaction_id_mismatch");
}

exec::SnapshotSafeCacheKey SnapshotKey() {
  exec::SnapshotSafeCacheKey key;
  key.normalized_operation = "orh122.select";
  key.safe_parameter_digest = "tenant:42";
  key.catalog_epoch = 122;
  key.statistics_epoch = 123;
  key.security_epoch = 124;
  key.redaction_epoch = 125;
  key.mga_visibility_snapshot_class = "repeatable_read";
  key.provider_generation = 126;
  key.result_contract_identity = "orh122.rowset.v1";
  key.result_contract_hash = "sha256:orh122-rowset-v1";
  key.route_compatibility = "embedded";
  key.dialect_compatibility = "sbsql_v3";
  return key;
}

exec::SnapshotSafeCacheEntry SnapshotEntry() {
  exec::SnapshotSafeCacheEntry entry;
  entry.key = SnapshotKey();
  entry.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  entry.row_count = 2;
  entry.cached_result_digest = "sha256:orh122-result";
  entry.cached_mga_security_digest = "sha256:orh122-mga-security";
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
  request.recomputed_result_digest = "sha256:orh122-result";
  request.recomputed_mga_security_digest = "sha256:orh122-mga-security";
  return request;
}

template <typename Mutator>
void RequireSnapshotMissForKeyChurn(std::string_view cause, Mutator mutate) {
  exec::SnapshotSafeResultCache cache;
  Require(cache.Store(SnapshotStoreRequest()).action ==
              exec::SnapshotSafeCacheAction::kStore,
          "snapshot cache store failed");
  auto request = SnapshotLookupRequest();
  mutate(request.key);
  const auto decision = cache.Lookup(request);
  Require(decision.action == exec::SnapshotSafeCacheAction::kMissRecompute,
          std::string("snapshot cache did not miss on ") +
              std::string(cause));
  Require(!decision.cache_hit && !decision.fail_closed,
          std::string("snapshot cache churn did not choose recompute miss for ") +
              std::string(cause));
  Require(Has(decision.evidence, "snapshot_cache_miss_recompute=true"),
          "snapshot cache evidence missing miss-recompute cause");
  Require(HasPrefix(decision.evidence, std::string(cause) + "="),
          std::string("snapshot cache evidence missing churn field ") +
              std::string(cause));
  Require(Has(decision.evidence, "cache_transaction_finality_authority=false"),
          "snapshot cache evidence must preserve finality authority boundary");
}

void ProveSnapshotSafeResultCacheChurnMissesAndRefusals() {
  RequireSnapshotMissForKeyChurn(
      "catalog_epoch", [](exec::SnapshotSafeCacheKey& key) {
        key.catalog_epoch += 1;
      });
  RequireSnapshotMissForKeyChurn(
      "statistics_epoch", [](exec::SnapshotSafeCacheKey& key) {
        key.statistics_epoch += 1;
      });
  RequireSnapshotMissForKeyChurn(
      "security_epoch", [](exec::SnapshotSafeCacheKey& key) {
        key.security_epoch += 1;
      });
  RequireSnapshotMissForKeyChurn(
      "redaction_epoch", [](exec::SnapshotSafeCacheKey& key) {
        key.redaction_epoch += 1;
      });
  RequireSnapshotMissForKeyChurn(
      "provider_generation", [](exec::SnapshotSafeCacheKey& key) {
        key.provider_generation += 1;
      });
  RequireSnapshotMissForKeyChurn(
      "result_contract_hash", [](exec::SnapshotSafeCacheKey& key) {
        key.result_contract_hash = "sha256:orh122-rowset-v2";
      });
  RequireSnapshotMissForKeyChurn(
      "route_compatibility", [](exec::SnapshotSafeCacheKey& key) {
        key.route_compatibility = "inet";
      });
  RequireSnapshotMissForKeyChurn(
      "dialect_compatibility", [](exec::SnapshotSafeCacheKey& key) {
        key.dialect_compatibility = "firebirdsql";
      });

  exec::SnapshotSafeResultCache cache;
  Require(cache.Store(SnapshotStoreRequest()).action ==
              exec::SnapshotSafeCacheAction::kStore,
          "snapshot cache store failed for refusal checks");
  auto lookup = SnapshotLookupRequest();
  lookup.security_uncertain = true;
  auto decision = cache.Lookup(lookup);
  Require(decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.UNCERTAINTY_REFUSED",
          "snapshot security churn uncertainty did not refuse");
  Require(decision.fail_closed, "snapshot uncertainty did not fail closed");
  lookup = SnapshotLookupRequest();
  lookup.result_contract_uncertain = true;
  decision = cache.Lookup(lookup);
  Require(decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.RESULT_CONTRACT_UNCERTAIN",
          "snapshot result-contract uncertainty diagnostic mismatch");
  lookup = SnapshotLookupRequest();
  lookup.route_mismatch = true;
  decision = cache.Lookup(lookup);
  Require(decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.ROUTE_MISMATCH",
          "snapshot route churn diagnostic mismatch");
  lookup = SnapshotLookupRequest();
  lookup.visibility_authority_cached = true;
  lookup.transaction_finality_authority_cached = true;
  decision = cache.Lookup(lookup);
  Require(decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.AUTHORITY_REFUSED",
          "snapshot authority-bearing cache diagnostic mismatch");
}

std::filesystem::path UniqueTempDir() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  auto dir = std::filesystem::temp_directory_path() /
             ("scratchbird_orh122_" + std::to_string(now));
  std::filesystem::create_directories(dir);
  return dir;
}

struct TempDatabase {
  std::filesystem::path dir = UniqueTempDir();
  std::filesystem::path path = dir / "orh122.sbdb";

  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext NoSqlContext(const std::filesystem::path& path) {
  api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical = "orh122-database";
  context.current_schema_uuid.canonical = "orh122-collection";
  context.transaction_uuid.canonical = "orh122-nosql-tx";
  context.local_transaction_id = 122;
  context.security_context_present = true;
  context.resource_epoch = 123;
  context.security_epoch = 124;
  context.catalog_generation_id = 125;
  return context;
}

api::EngineNoSqlPhysicalProviderContract NoSqlContract(
    const api::EngineRequestContext& context,
    const api::EngineNoSqlProviderGenerationMetadata& generation) {
  api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = api::EngineNoSqlProviderFamily::kDocument;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = generation.provider_id;
  contract.local_provider_available = true;
  contract.exact_fallback_available = true;
  contract.provider_generation.required = true;
  contract.provider_generation.proof_present = true;
  contract.provider_generation.visible_to_snapshot = true;
  contract.provider_generation.publish_state_bound = true;
  contract.provider_generation.validation_state_bound = true;
  contract.provider_generation.backup_restore_repair_metadata_bound = true;
  contract.provider_generation.support_bundle_evidence_bound = true;
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
  contract.provider_generation.publish_state = "published";
  contract.provider_generation.validation_state = "validated";
  contract.provider_generation.backup_metadata_ref =
      generation.backup_metadata_ref;
  contract.provider_generation.restore_metadata_ref =
      generation.restore_metadata_ref;
  contract.provider_generation.repair_metadata_ref =
      generation.repair_metadata_ref;
  contract.provider_generation.support_bundle_evidence_id =
      generation.support_bundle_evidence_id;
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

void ProveNoSqlProviderGenerationChurnRefusals() {
  TempDatabase database;
  const auto context = NoSqlContext(database.path);
  std::ofstream seed(context.database_path, std::ios::binary | std::ios::trunc);
  seed << "SBCRUD1\tTX_BEGIN\t" << context.local_transaction_id << '\t'
       << context.transaction_uuid.canonical << '\n';
  seed.flush();
  Require(static_cast<bool>(seed), "could not seed NoSQL temp database");

  const auto metadata = api::MakeDocumentProviderGenerationMetadata(
      context,
      "nosql.local.document.path_provider",
      context.current_schema_uuid.canonical,
      122);
  const auto published =
      api::PublishNoSqlProviderGeneration(context, metadata);
  Require(published.ok, "NoSQL provider generation publish failed");
  auto contract = NoSqlContract(context, published.metadata);
  auto valid = api::ValidateNoSqlProviderGeneration(context, contract);
  Require(valid.ok, "fresh NoSQL provider generation was refused");
  Require(Has(valid.evidence, "provider_generation_validated=true"),
          "NoSQL generation validation evidence missing");
  Require(Has(valid.evidence,
              "provider_generation_mga_authority=engine_transaction_inventory"),
          "NoSQL provider validation must preserve MGA authority");

  contract = NoSqlContract(context, published.metadata);
  contract.provider_generation.required_generation =
      published.metadata.generation_id + 1;
  contract.provider_generation.available_generation =
      published.metadata.generation_id + 1;
  auto stale = api::ValidateNoSqlProviderGeneration(context, contract);
  Require(!stale.ok, "stale NoSQL generation was accepted");
  Require(DiagnosticContains(stale, api::kNoSqlProviderGenerationStale),
          "stale NoSQL generation diagnostic mismatch");
  Require(Has(stale.evidence, "provider_generation_fail_closed=true"),
          "stale NoSQL generation did not fail closed");

  contract = NoSqlContract(context, published.metadata);
  contract.provider_generation.catalog_epoch += 1;
  auto epoch = api::ValidateNoSqlProviderGeneration(context, contract);
  Require(!epoch.ok, "NoSQL generation epoch mismatch was accepted");
  Require(DiagnosticContains(epoch,
                             api::kNoSqlProviderGenerationEpochMismatch),
          "NoSQL epoch mismatch diagnostic mismatch");
  Require(Has(epoch.evidence,
              "provider_generation_mga_authority=engine_transaction_inventory"),
          "NoSQL epoch mismatch must preserve MGA authority evidence");

  contract = NoSqlContract(context, published.metadata);
  contract.provider_generation.provider_claims_transaction_finality_authority =
      true;
  auto authority = api::ValidateNoSqlProviderGeneration(context, contract);
  Require(!authority.ok, "NoSQL provider finality authority claim accepted");
  Require(DiagnosticContains(authority,
                             api::kNoSqlProviderGenerationAuthorityRefused),
          "NoSQL authority refusal diagnostic mismatch");
}

}  // namespace

int main() {
  ProveParserFrontDoorConcurrentChurnInvalidation();
  ProveOptimizerPlanCacheChurnInvalidation();
  ProveStreamingCursorFetchRefusals();
  ProveSnapshotSafeResultCacheChurnMissesAndRefusals();
  ProveNoSqlProviderGenerationChurnRefusals();
  std::cout << "optimizer_runtime_hot_path_orh_122_gate=passed\n";
  return EXIT_SUCCESS;
}
