// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "partial_index_implication.hpp"
#include "predicate_normalization.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "partial_index_implication_gate: " << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values, std::string_view prefix) {
  return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
  });
}

opt::OptimizerStatsIdentity FreshIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 31;
  identity.catalog_epoch = 31;
  identity.transaction_visibility_epoch = 31;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::IndexStats BasePartialIndex(const std::string& relation_uuid,
                                 const std::string& index_uuid,
                                 const std::string& partial_predicate) {
  opt::IndexStats stats;
  stats.identity = FreshIdentity(index_uuid, index_uuid + ":index");
  stats.index_uuid = index_uuid;
  stats.relation_uuid = relation_uuid;
  stats.index_family = "btree";
  stats.descriptor_digest = "desc:partial:v1";
  stats.collation_identity = "unicode.casefold.det";
  stats.key_column_uuids = {"col.amount"};
  stats.partial = true;
  stats.partial_predicate_text = partial_predicate;
  stats.height = 3;
  stats.leaf_pages = 48;
  stats.distinct_keys = 12000;
  stats.clustering_factor = 0.75;
  stats.fragmentation_ratio = 0.02;
  stats.visibility_coverage = 1.0;
  stats.predicate_coverage = 1.0;
  return stats;
}

opt::TableCardinalityStats TableStats(const std::string& relation_uuid) {
  opt::TableCardinalityStats stats;
  stats.identity = FreshIdentity(relation_uuid, relation_uuid + ":table");
  stats.row_count = 50000;
  stats.visible_row_count = 45000;
  stats.page_count = 900;
  stats.average_row_bytes = 96;
  return stats;
}

idx::PartialPredicateImplicationRequest SafeRequest(std::string query_predicate,
                                                    std::string index_predicate) {
  idx::PartialPredicateImplicationRequest request;
  request.query_predicate_text = std::move(query_predicate);
  request.index_predicate_text = std::move(index_predicate);
  request.predicate_immutable = true;
  request.predicate_security_safe = true;
  request.descriptor_epoch_valid = true;
  request.resource_epoch_valid = true;
  request.collation_epoch_valid = true;
  request.function_epoch_valid = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  return request;
}

bool CoreProofAcceptsSafeImplicationShapes() {
  auto request = SafeRequest(
      "active and status in ('open','paid') and amount >= 100 and "
      "deleted_at is null and customer_name like 'Ada Lovelace%' and "
      "tenant_id = 42 and region = 'ca'",
      "active = true and status in ('open','paid','trial') and amount > 50 "
      "and deleted_at is null and customer_name like 'Ada%' and "
      "tenant_id is not null");

  const auto proof = idx::ProvePartialIndexPredicateImplication(request);
  return Require(proof.predicate_implied && proof.safe_to_consider_index,
                 "safe conjunction/range/IN/LIKE/null implication refused") &&
         Require(Has(proof.acceptance_reasons, "partial_predicate_proven"),
                 "core proof omitted canonical acceptance reason") &&
         Require(Has(proof.acceptance_reasons,
                     "partial_predicate_mga_security_recheck_preserved"),
                 "core proof did not preserve MGA/security recheck evidence") &&
         Require(Has(proof.evidence, "partial_index_visibility_authority=false"),
                 "visibility non-authority evidence missing") &&
         Require(Has(proof.evidence, "partial_index_authorization_authority=false"),
                 "authorization non-authority evidence missing") &&
         Require(Has(proof.evidence,
                     "partial_index_transaction_finality_authority=false"),
                 "transaction finality non-authority evidence missing") &&
         Require(Has(proof.evidence, "partial_index_cleanup_authority=false"),
                 "cleanup non-authority evidence missing") &&
         Require(Has(proof.evidence, "partial_index_recovery_authority=false"),
                 "recovery non-authority evidence missing") &&
         Require(Has(proof.evidence, "base_row_mga_recheck_required=true"),
                 "base row MGA recheck evidence missing") &&
         Require(Has(proof.evidence, "base_row_security_recheck_required=true"),
                 "base row security recheck evidence missing") &&
         Require(HasPrefix(proof.evidence,
                           "partial_predicate_remaining_query_conjuncts="),
                 "partial-shape remaining predicate composition evidence missing") &&
         Require(!proof.remaining_query_conjuncts.empty(),
                 "remaining query predicates were not retained for composition");
}

bool CoreProofHandlesRangeAndNullTightening() {
  auto request = SafeRequest(
      "age = 42 and active",
      "age >= 18 and age < 65 and age is not null and active = true");
  const auto proof = idx::ProvePartialIndexPredicateImplication(request);
  return Require(proof.predicate_implied,
                 "equality did not imply safe range and IS NOT NULL predicates");
}

bool CoreProofRefusesUnsupportedOrUnsafeShapes() {
  {
    auto request = SafeRequest("status in ('open','closed')",
                               "status in ('open','paid')");
    const auto proof = idx::ProvePartialIndexPredicateImplication(request);
    if (!Require(!proof.predicate_implied,
                 "IN-list superset query incorrectly implied smaller partial set") ||
        !Require(Has(proof.refusal_reasons, "partial_predicate_not_proven"),
                 "IN-list refusal did not report exact non-proof")) {
      return false;
    }
  }
  {
    auto request = SafeRequest("customer_name like 'Ada%'",
                               "customer_name like '%Ada'");
    const auto proof = idx::ProvePartialIndexPredicateImplication(request);
    if (!Require(!proof.predicate_implied,
                 "leading-wildcard partial LIKE was accepted") ||
        !Require(HasPrefix(proof.refusal_reasons,
                           "partial_predicate_like_prefix_refused:"),
                 "LIKE refusal did not name exact prefix diagnostic")) {
      return false;
    }
  }
  {
    auto request = SafeRequest("active and random() = 1", "active = true");
    const auto proof = idx::ProvePartialIndexPredicateImplication(request);
    if (!Require(!proof.predicate_implied,
                 "volatile query function was accepted") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_volatile_function_refused:random"),
                 "volatile function refusal missing")) {
      return false;
    }
  }
  {
    idx::PartialPredicateImplicationRequest request;
    request.query_predicate_text = "active and amount > 10";
    request.index_predicate_text = "active = true";
    const auto proof = idx::ProvePartialIndexPredicateImplication(request);
    if (!Require(!proof.predicate_implied,
                 "default proof bits accepted a partial predicate proof") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_non_immutable_refused"),
                 "default non-immutable refusal missing") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_security_unsafe_refused"),
                 "default security-unsafe refusal missing") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_descriptor_epoch_stale"),
                 "default descriptor epoch refusal missing") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_resource_epoch_stale"),
                 "default resource epoch refusal missing") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_collation_epoch_stale"),
                 "default collation epoch refusal missing") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_function_epoch_stale"),
                 "default function epoch refusal missing") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_mga_recheck_missing"),
                 "default MGA recheck refusal missing") ||
        !Require(Has(proof.refusal_reasons,
                     "partial_predicate_security_recheck_missing"),
                 "default security recheck refusal missing")) {
      return false;
    }
  }
  {
    auto request = SafeRequest("sqrt(amount) = 2", "sqrt(amount) = 2");
    const auto proof = idx::ProvePartialIndexPredicateImplication(request);
    return Require(!proof.predicate_implied,
                   "unknown function was accepted as a proven immutable predicate") &&
           Require(Has(proof.refusal_reasons,
                       "partial_predicate_function_not_proven:sqrt"),
                   "unknown function proof refusal missing");
  }
}

bool CoreProofRefusesParseErrors() {
  auto request = SafeRequest("active and amount > ", "active = true");
  const auto proof = idx::ProvePartialIndexPredicateImplication(request);
  return Require(!proof.predicate_implied,
                 "parse error was accepted") &&
         Require(Has(proof.refusal_reasons, "query_predicate_parse_refused"),
                 "query parse refusal missing");
}

bool OptimizerRouteConsumesCoreProof() {
  const std::string relation_uuid = "rel.partial.implication";
  auto index = BasePartialIndex(
      relation_uuid,
      "idx.partial.amount.active",
      "active = true and amount > 50 and tenant_id is not null");

  const auto match = opt::MatchPredicateToIndex(
      {"amount >= 100 and active and tenant_id = 42 and region = 'ca'",
       "desc:partial:v1",
       "unicode.casefold.det",
       true,
       true,
       index});
  if (!Require(match.matches, "optimizer predicate match did not consume core proof") ||
      !Require(Has(match.acceptance_reasons,
                   "partial_predicate_core_implication_proven"),
               "optimizer match omitted core proof acceptance reason") ||
      !Require(Has(match.acceptance_reasons,
                   "partial_index_transaction_finality_authority=false"),
               "optimizer match omitted non-finality evidence")) {
    return false;
  }

  opt::AccessPathPlanningRequest request;
  request.relation_uuid = relation_uuid;
  request.predicate_kind = "scalar_range";
  request.predicate_text =
      "amount >= 100 and active and tenant_id = 42 and region = 'ca'";
  request.descriptor_digest = "desc:partial:v1";
  request.collation_identity = "unicode.casefold.det";
  request.visibility_proven = true;
  request.grants_proven = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  request.index_visibility_native = true;
  request.table_stats = TableStats(relation_uuid);
  request.candidate_indexes = {index};
  const auto candidates = opt::GenerateFullAccessPathCandidates(request);
  const auto found = std::find_if(
      candidates.begin(),
      candidates.end(),
      [&](const opt::PlanCandidate& candidate) {
        return candidate.candidate_id == "CAND-OPT-INDEX:" + index.index_uuid;
      });
  return Require(found != candidates.end(),
                 "access path candidate using partial index was missing") &&
         Require(found->cost.selectable,
                 "partial index access path was refused after proof") &&
         Require(Has(found->acceptance_reasons,
                     "partial_predicate_core_implication_proven"),
                 "access path did not carry core proof reason") &&
         Require(Has(found->acceptance_reasons,
                     "base_row_mga_recheck_required=true"),
                 "access path did not carry MGA recheck evidence");
}

bool OptimizerRouteRefusesUnsafeCoreProof() {
  auto index = BasePartialIndex("rel.partial.refused",
                                "idx.partial.unsafe",
                                "active = true");
  index.partial_predicate_security_safe = false;
  const auto match = opt::MatchPredicateToIndex({"active",
                                                 "desc:partial:v1",
                                                 "unicode.casefold.det",
                                                 true,
                                                 true,
                                                 index});
  return Require(!match.matches,
                 "optimizer accepted security-unsafe partial predicate") &&
         Require(Has(match.refusal_reasons,
                     "partial_predicate_security_unsafe_refused"),
                 "optimizer did not expose exact core refusal reason");
}

}  // namespace

int main() {
  if (!CoreProofAcceptsSafeImplicationShapes()) return 1;
  if (!CoreProofHandlesRangeAndNullTightening()) return 1;
  if (!CoreProofRefusesUnsupportedOrUnsafeShapes()) return 1;
  if (!CoreProofRefusesParseErrors()) return 1;
  if (!OptimizerRouteConsumesCoreProof()) return 1;
  if (!OptimizerRouteRefusesUnsafeCoreProof()) return 1;
  std::cout << "partial_index_implication_gate=passed\n";
  return 0;
}
