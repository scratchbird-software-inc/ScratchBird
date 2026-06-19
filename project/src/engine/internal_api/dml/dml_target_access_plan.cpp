// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_target_access_plan.hpp"

#include "hot_point_lookup_cache.hpp"
#include "logical_plan.hpp"
#include "memory.hpp"
#include "optimizer_hot_point_lookup.hpp"
#include "physical_plan.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
namespace idx = scratchbird::core::index;
namespace mem = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr const char* kMissingRelation = "missing relation";
constexpr const char* kMissingPredicateAccessDescriptor = "missing predicate/access descriptor";
constexpr const char* kMissingMgaRecheck = "missing MGA recheck";
constexpr const char* kMissingSecurityRecheck = "missing security recheck";
constexpr const char* kMissingGrantsSecurityContext = "missing grants/security context";
constexpr const char* kStaleCatalogEpoch = "stale catalog epoch";
constexpr const char* kStaleSecurityEpoch = "stale security epoch";
constexpr const char* kStalePolicyEpoch = "stale policy epoch";
constexpr const char* kStaleStatsEpoch = "stale stats epoch";
constexpr const char* kUnsafeSummaryPruning = "unsafe summary pruning";
constexpr const char* kUnsafeParserReferenceAuthority = "unsafe parser/reference authority";

bool EpochIsStale(std::uint64_t observed, std::uint64_t current) {
  return current != 0 && observed != 0 && observed < current;
}

bool HasPredicateOrAccessDescriptor(const DmlTargetAccessPlanRequest& request) {
  return request.access_descriptor_present ||
         request.explicit_table_scan_fallback ||
         !request.predicate_kind.empty() ||
         !request.predicate_descriptor_digest.empty() ||
         !request.row_uuid.empty() ||
         !request.row_uuids.empty() ||
         !request.index_uuid.empty() ||
         request.summary_prune.requested;
}

bool SummaryPruningSafe(const DmlTargetSummaryPruneDescriptor& summary) {
  if (!summary.requested) {
    return true;
  }
  if (!summary.summary_present || !summary.predicate_supported) {
    return false;
  }
  if (summary.summary_generation == 0 ||
      summary.relation_generation == 0 ||
      summary.summary_generation != summary.relation_generation) {
    return false;
  }
  if (summary.ranges_pruned > summary.candidate_ranges) {
    return false;
  }
  if (summary.pages_pruned > summary.pages_considered) {
    return false;
  }
  return true;
}

bool IsEqualityPredicate(std::string_view predicate_kind) {
  return predicate_kind == "unique_eq" ||
         predicate_kind == "scalar_eq" ||
         predicate_kind == "nonunique_eq";
}

bool IsRangePredicate(std::string_view predicate_kind) {
  return predicate_kind == "scalar_range" ||
         predicate_kind == "range" ||
         predicate_kind == "index_range";
}

const char* BoolText(bool value) {
  return value ? "true" : "false";
}

std::uint64_t CurrentOrObservedEpoch(std::uint64_t current,
                                     std::uint64_t observed) {
  return current != 0 ? current : observed;
}

platform::TypedUuid ParseTypedUuidOrEmpty(platform::UuidKind kind,
                                          const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const auto durable = uuid::ParseDurableEngineIdentityUuid(kind, text);
  if (durable.ok()) {
    return durable.value;
  }
  const auto parsed = uuid::ParseTypedUuid(kind, text);
  return parsed.ok() ? parsed.value : platform::TypedUuid{};
}

std::optional<idx::HotPointProbeClass> HotPointProbeClassForAccessKind(
    DmlTargetAccessKind access_kind) {
  switch (access_kind) {
    case DmlTargetAccessKind::row_uuid_singleton:
      return idx::HotPointProbeClass::row_uuid_lookup;
    case DmlTargetAccessKind::row_uuid_list:
      return std::nullopt;
    case DmlTargetAccessKind::unique_index_lookup:
      return idx::HotPointProbeClass::unique_index_lookup;
    case DmlTargetAccessKind::refused:
    case DmlTargetAccessKind::nonunique_index_lookup:
    case DmlTargetAccessKind::range_index_lookup:
    case DmlTargetAccessKind::summary_pruned:
    case DmlTargetAccessKind::table_scan:
      return std::nullopt;
  }
  return std::nullopt;
}

idx::AdaptiveHotPointLookupCache& DmlHotPointLookupCache() {
  static idx::AdaptiveHotPointLookupCache cache([] {
    idx::HotPointLookupCacheConfig config;
    const auto memory_state = mem::DefaultMemoryManagerState();
    const auto page_pool_bytes =
        memory_state.active_policy.page_buffer_pool_limit_bytes == 0
            ? 512ull * 1024ull * 1024ull
            : memory_state.active_policy.page_buffer_pool_limit_bytes;
    const auto scale = std::max<std::uint64_t>(
        1,
        page_pool_bytes / (128ull * 1024ull * 1024ull));
    config.partition_count = static_cast<std::size_t>(
        std::min<std::uint64_t>(128, 16ull * scale));
    config.contention_disable_threshold = 256ull * scale;
    config.authority_refusal_disable_threshold = 256ull * scale;
    config.max_entries_per_partition = 4096ull * scale;
    return config;
  }());
  return cache;
}

idx::HotPointLookupCacheKey BuildDmlHotPointLookupCacheKey(
    idx::HotPointProbeClass probe_class,
    const DmlTargetAccessPlanRequest& request) {
  idx::HotPointLookupCacheKey key;
  key.probe_class = probe_class;
  key.database_uuid = ParseTypedUuidOrEmpty(platform::UuidKind::database,
                                            request.database_uuid);
  key.object_uuid = ParseTypedUuidOrEmpty(platform::UuidKind::object,
                                          request.relation_uuid);
  key.index_uuid = ParseTypedUuidOrEmpty(platform::UuidKind::object,
                                         request.index_uuid);
  key.encoded_probe_key =
      "mutation=" + request.mutation_kind +
      "|predicate=" + request.predicate_kind +
      "|descriptor=" + request.predicate_descriptor_digest +
      "|row_uuid=" + request.row_uuid +
      "|index_uuid=" + request.index_uuid;
  key.statistics_snapshot_id =
      "stats_epoch:" +
      std::to_string(CurrentOrObservedEpoch(request.current_stats_epoch,
                                           request.observed_stats_epoch));
  key.descriptor_set_digest =
      request.relation_uuid + "|" + request.predicate_descriptor_digest;
  key.index_definition_digest =
      request.index_uuid + "|" + request.index_family + "|" +
      (request.index_unique ? "unique" : "nonunique");
  key.security_policy_digest =
      request.security_policy_digest.empty()
          ? "security_epoch:" +
                std::to_string(CurrentOrObservedEpoch(request.current_security_epoch,
                                                     request.observed_security_epoch))
          : request.security_policy_digest;
  key.redaction_policy_digest =
      request.redaction_policy_digest.empty()
          ? "redaction_epoch:" +
                std::to_string(CurrentOrObservedEpoch(request.current_policy_epoch,
                                                     request.observed_policy_epoch))
          : request.redaction_policy_digest;
  key.access_policy_digest =
      request.access_policy_digest.empty()
          ? "access_epoch:" +
                std::to_string(CurrentOrObservedEpoch(request.current_policy_epoch,
                                                     request.observed_policy_epoch))
          : request.access_policy_digest;
  key.collation_profile_digest =
      request.collation_profile_digest.empty() ? "collation:default"
                                               : request.collation_profile_digest;
  key.catalog_epoch = CurrentOrObservedEpoch(request.current_catalog_epoch,
                                            request.observed_catalog_epoch);
  key.index_epoch = request.index_epoch != 0 ? request.index_epoch
                                             : key.catalog_epoch;
  key.statistics_epoch = CurrentOrObservedEpoch(request.current_stats_epoch,
                                               request.observed_stats_epoch);
  key.security_epoch = CurrentOrObservedEpoch(request.current_security_epoch,
                                             request.observed_security_epoch);
  key.policy_epoch = CurrentOrObservedEpoch(request.current_policy_epoch,
                                           request.observed_policy_epoch);
  key.object_epoch = request.object_epoch != 0 ? request.object_epoch
                                               : key.catalog_epoch;
  key.compatibility_epoch =
      request.compatibility_epoch != 0 ? request.compatibility_epoch
                                       : request.local_transaction_id;
  return key;
}

idx::HotPointLookupCacheEntry BuildDmlHotPointLookupCacheEntry(
    const idx::HotPointLookupCacheKey& key,
    const DmlTargetAccessPlanRequest& request,
    const std::string& actual_row_uuid) {
  idx::HotPointLookupCacheEntry entry;
  entry.key = key;
  idx::HotPointLookupCandidate candidate;
  candidate.locator.table_uuid = key.object_uuid;
  candidate.locator.row_uuid = ParseTypedUuidOrEmpty(platform::UuidKind::row,
                                                     actual_row_uuid);
  candidate.locator.local_transaction_id = request.local_transaction_id;
  candidate.proof_kind = "dml_target_access_successful_row_locator";
  candidate.posting_list_digest =
      request.predicate_descriptor_digest.empty() ? key.encoded_probe_key
                                                  : request.predicate_descriptor_digest;
  candidate.candidate_locator_only = true;
  candidate.equality_proof_metadata_only = true;
  candidate.requires_mga_visibility_recheck = true;
  candidate.requires_security_authorization_recheck = true;
  entry.candidates.push_back(std::move(candidate));
  if (key.object_uuid.valid()) {
    entry.dependency_uuids.push_back(key.object_uuid);
  }
  if (key.index_uuid.valid()) {
    entry.dependency_uuids.push_back(key.index_uuid);
  }
  entry.created_epoch = key.catalog_epoch;
  return entry;
}

void AddHotPointLookupCacheEvidence(const DmlTargetAccessPlanRequest& request,
                                    DmlTargetAccessPlan* plan) {
  const auto probe_class = HotPointProbeClassForAccessKind(plan->access_kind);
  if (!probe_class.has_value()) {
    return;
  }
  const auto decision =
      opt::PlanOptimizerHotPointLookup(*probe_class,
                                       true,
                                       request.mga_visibility_recheck_planned,
                                       request.security_recheck_planned);
  plan->evidence.push_back(
      "hot_point_lookup_cache_probe_class=" +
      std::string(idx::HotPointProbeClassName(*probe_class)));
  plan->evidence.push_back("hot_point_lookup_cache_decision=" +
                           decision.diagnostic_code);
  plan->evidence.push_back("hot_point_lookup_cache_lookup_allowed=" +
                           std::string(BoolText(decision.lookup_allowed)));
  plan->evidence.push_back("hot_point_lookup_cache_admission_allowed=" +
                           std::string(BoolText(decision.admission_allowed)));
  for (const auto& evidence : decision.evidence) {
    plan->evidence.push_back("hot_point_lookup_cache_decision_evidence=" +
                             evidence);
  }
  if (!decision.lookup_allowed) {
    plan->evidence.push_back("hot_point_lookup_cache_lookup=refused");
    return;
  }

  const auto key = BuildDmlHotPointLookupCacheKey(*probe_class, request);
  auto& cache = DmlHotPointLookupCache();
  const auto lookup = cache.Lookup(key);
  plan->evidence.push_back(std::string("hot_point_lookup_cache_lookup=") +
                           (lookup.cache_hit ? "hit" : "miss"));
  plan->evidence.push_back("hot_point_lookup_cache_diagnostic=" +
                           lookup.diagnostic_code);
  plan->evidence.push_back("hot_point_lookup_cache_key=" + lookup.cache_key);
  for (const auto& evidence : lookup.evidence) {
    plan->evidence.push_back("hot_point_lookup_cache_evidence=" + evidence);
  }
  if (!lookup.cache_hit &&
      plan->access_kind == DmlTargetAccessKind::row_uuid_singleton &&
      !request.row_uuid.empty() &&
      decision.admission_allowed &&
      request.mutation_kind != "dml.merge_rows") {
    const auto put =
        cache.Put(BuildDmlHotPointLookupCacheEntry(key, request, request.row_uuid));
    plan->evidence.push_back("hot_point_lookup_cache_admission=" +
                             put.diagnostic_code);
    plan->evidence.push_back("hot_point_lookup_cache_admitted=" +
                             std::string(BoolText(put.admitted)));
    plan->evidence.push_back("hot_point_lookup_cache_actual_row_locator=" +
                             request.row_uuid);
    for (const auto& evidence : put.evidence) {
      plan->evidence.push_back("hot_point_lookup_cache_admission_evidence=" +
                               evidence);
    }
    return;
  }
  plan->evidence.push_back(
      "hot_point_lookup_cache_admission=deferred_until_successful_row_locator");
}

planner::PhysicalAccessKind PhysicalKindForIndex(const DmlTargetAccessPlanRequest& request,
                                                 bool equality) {
  if (equality && request.index_family == "hash") {
    return planner::PhysicalAccessKind::kScalarHashLookup;
  }
  return equality ? planner::PhysicalAccessKind::kScalarBtreeLookup
                  : planner::PhysicalAccessKind::kScalarBtreeRange;
}

void AddDiagnostic(std::vector<std::string>* diagnostics, const char* diagnostic) {
  if (std::find(diagnostics->begin(), diagnostics->end(), diagnostic) == diagnostics->end()) {
    diagnostics->push_back(diagnostic);
  }
}

void AddCommonEvidence(const DmlTargetAccessPlanRequest& request,
                       DmlTargetAccessPlan* plan) {
  plan->evidence.push_back("relation_uuid=" + request.relation_uuid);
  plan->evidence.push_back("predicate_kind=" + request.predicate_kind);
  plan->evidence.push_back("mga_visibility_recheck=required");
  plan->evidence.push_back("security_recheck=required");
  plan->evidence.push_back("grants_security_context=present");
  plan->evidence.push_back("parser_or_reference_authority=false");
  plan->evidence.push_back("mga_finality_authority=engine_transaction_inventory");
}

void FinishAccepted(DmlTargetAccessKind access_kind,
                    planner::PhysicalAccessKind physical_kind,
                    const DmlTargetAccessPlanRequest& request,
                    DmlTargetAccessPlan* plan) {
  plan->ok = true;
  plan->access_kind = access_kind;
  plan->physical_access_kind = planner::PhysicalAccessKindName(physical_kind);
  plan->executor_capability = opt::RequiredExecutorCapabilityForAccessKind(physical_kind);
  plan->relation_uuid = request.relation_uuid;
  plan->predicate_kind = request.predicate_kind;
  plan->predicate_descriptor_digest = request.predicate_descriptor_digest;
  plan->row_uuid = request.row_uuid;
  plan->row_uuids = request.row_uuids;
  plan->index_uuid = request.index_uuid;
  plan->estimated_rows = request.estimated_rows == 0 ? 1 : request.estimated_rows;
  AddCommonEvidence(request, plan);
  plan->evidence.push_back(std::string("dml_target_access_kind=") +
                           DmlTargetAccessKindName(access_kind));
  plan->evidence.push_back("physical_access_kind=" + plan->physical_access_kind);
  plan->evidence.push_back("executor_capability=" + plan->executor_capability);
  if (!request.index_uuid.empty()) {
    plan->evidence.push_back("index_uuid=" + request.index_uuid);
    plan->evidence.push_back(std::string("index_unique=") +
                             (request.index_unique ? "true" : "false"));
  }
  AddHotPointLookupCacheEvidence(request, plan);
  if (request.summary_prune.requested) {
    plan->evidence.push_back("summary_prune=selected");
    plan->evidence.push_back("summary_prune_authority=metadata_only");
    plan->evidence.push_back("summary_prune_base_row_mga_recheck=required");
    plan->evidence.push_back("summary_prune_base_row_security_recheck=required");
  }
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

}  // namespace

const char* DmlTargetAccessKindName(DmlTargetAccessKind kind) {
  switch (kind) {
    case DmlTargetAccessKind::refused: return "refused";
    case DmlTargetAccessKind::row_uuid_singleton: return "row_uuid_singleton";
    case DmlTargetAccessKind::row_uuid_list: return "row_uuid_list";
    case DmlTargetAccessKind::unique_index_lookup: return "unique_index_lookup";
    case DmlTargetAccessKind::nonunique_index_lookup: return "nonunique_index_lookup";
    case DmlTargetAccessKind::range_index_lookup: return "range_index_lookup";
    case DmlTargetAccessKind::summary_pruned: return "summary_pruned";
    case DmlTargetAccessKind::table_scan: return "table_scan";
  }
  return "refused";
}

DmlTargetAccessPlan BuildDmlTargetAccessPlan(const DmlTargetAccessPlanRequest& request) {
  DmlTargetAccessPlan plan;
  plan.relation_uuid = request.relation_uuid;
  plan.predicate_kind = request.predicate_kind;
  plan.predicate_descriptor_digest = request.predicate_descriptor_digest;
  plan.row_uuid = request.row_uuid;
  plan.row_uuids = request.row_uuids;
  plan.index_uuid = request.index_uuid;

  if (!request.relation_present || request.relation_uuid.empty()) {
    AddDiagnostic(&plan.diagnostics, kMissingRelation);
  }
  if (!HasPredicateOrAccessDescriptor(request)) {
    AddDiagnostic(&plan.diagnostics, kMissingPredicateAccessDescriptor);
  }
  if (!request.mga_visibility_recheck_planned) {
    AddDiagnostic(&plan.diagnostics, kMissingMgaRecheck);
  }
  if (!request.security_recheck_planned) {
    AddDiagnostic(&plan.diagnostics, kMissingSecurityRecheck);
  }
  if (!request.grants_proven || !request.security_context_present) {
    AddDiagnostic(&plan.diagnostics, kMissingGrantsSecurityContext);
  }
  if (EpochIsStale(request.observed_catalog_epoch, request.current_catalog_epoch)) {
    AddDiagnostic(&plan.diagnostics, kStaleCatalogEpoch);
  }
  if (EpochIsStale(request.observed_security_epoch, request.current_security_epoch)) {
    AddDiagnostic(&plan.diagnostics, kStaleSecurityEpoch);
  }
  if (EpochIsStale(request.observed_policy_epoch, request.current_policy_epoch)) {
    AddDiagnostic(&plan.diagnostics, kStalePolicyEpoch);
  }
  if (EpochIsStale(request.observed_stats_epoch, request.current_stats_epoch)) {
    AddDiagnostic(&plan.diagnostics, kStaleStatsEpoch);
  }
  if (!SummaryPruningSafe(request.summary_prune)) {
    AddDiagnostic(&plan.diagnostics, kUnsafeSummaryPruning);
  }
  if (request.parser_or_reference_authority) {
    AddDiagnostic(&plan.diagnostics, kUnsafeParserReferenceAuthority);
  }
  if (!plan.diagnostics.empty()) {
    plan.evidence.push_back("dml_target_access_kind=refused");
    for (const auto& diagnostic : plan.diagnostics) {
      plan.evidence.push_back("refusal=" + diagnostic);
    }
    return plan;
  }

  if (request.summary_prune.requested) {
    FinishAccepted(DmlTargetAccessKind::summary_pruned,
                   planner::PhysicalAccessKind::kBitmapSummaryScan,
                   request,
                   &plan);
    return plan;
  }
  if (request.predicate_kind == "row_uuid_eq" ||
      request.predicate_kind == "row_uuid_match" ||
      !request.row_uuid.empty()) {
    FinishAccepted(DmlTargetAccessKind::row_uuid_singleton,
                   planner::PhysicalAccessKind::kRowUuidLookup,
                   request,
                   &plan);
    plan.estimated_rows = 1;
    return plan;
  }
  if (request.predicate_kind == "row_uuid_in_list" &&
      !request.row_uuids.empty()) {
    FinishAccepted(DmlTargetAccessKind::row_uuid_list,
                   planner::PhysicalAccessKind::kRowUuidLookup,
                   request,
                   &plan);
    plan.estimated_rows = static_cast<std::uint64_t>(request.row_uuids.size());
    return plan;
  }
  if (IsEqualityPredicate(request.predicate_kind) && !request.index_uuid.empty()) {
    const bool unique = request.index_unique || request.predicate_kind == "unique_eq";
    FinishAccepted(unique ? DmlTargetAccessKind::unique_index_lookup
                          : DmlTargetAccessKind::nonunique_index_lookup,
                   PhysicalKindForIndex(request, true),
                   request,
                   &plan);
    if (unique) {
      plan.estimated_rows = 1;
    }
    return plan;
  }
  if (IsRangePredicate(request.predicate_kind) && !request.index_uuid.empty()) {
    FinishAccepted(DmlTargetAccessKind::range_index_lookup,
                   PhysicalKindForIndex(request, false),
                   request,
                   &plan);
    return plan;
  }
  if (request.explicit_table_scan_fallback) {
    FinishAccepted(DmlTargetAccessKind::table_scan,
                   planner::PhysicalAccessKind::kTableScan,
                   request,
                   &plan);
    return plan;
  }

  AddDiagnostic(&plan.diagnostics, kMissingPredicateAccessDescriptor);
  plan.evidence.push_back("dml_target_access_kind=refused");
  plan.evidence.push_back(std::string("refusal=") + kMissingPredicateAccessDescriptor);
  return plan;
}

void AdmitDmlHotPointLookupCacheSuccessfulRowLocator(
    const DmlTargetAccessPlanRequest& request,
    const std::string& actual_row_uuid,
    std::vector<std::string>* evidence) {
  auto add = [evidence](std::string item) {
    if (evidence != nullptr) {
      evidence->push_back(std::move(item));
    }
  };
  if (!request.index_uuid.empty() && !request.index_unique) {
    add("hot_point_lookup_cache_admission=refused_nonunique_locator_stream");
    return;
  }
  const auto probe_class =
      HotPointProbeClassForAccessKind(request.index_uuid.empty()
                                          ? DmlTargetAccessKind::row_uuid_singleton
                                          : DmlTargetAccessKind::unique_index_lookup);
  if (!probe_class.has_value()) {
    return;
  }
  if (actual_row_uuid.empty()) {
    add("hot_point_lookup_cache_admission=refused_empty_actual_row_uuid");
    return;
  }
  auto row_uuid = ParseTypedUuidOrEmpty(platform::UuidKind::row,
                                        actual_row_uuid);
  if (!row_uuid.valid()) {
    add("hot_point_lookup_cache_admission=refused_invalid_actual_row_uuid");
    return;
  }
  const auto decision =
      opt::PlanOptimizerHotPointLookup(*probe_class,
                                       true,
                                       request.mga_visibility_recheck_planned,
                                       request.security_recheck_planned);
  if (!decision.admission_allowed) {
    add("hot_point_lookup_cache_admission=refused_" + decision.diagnostic_code);
    return;
  }
  const auto key = BuildDmlHotPointLookupCacheKey(*probe_class, request);
  auto& cache = DmlHotPointLookupCache();
  const auto put =
      cache.Put(BuildDmlHotPointLookupCacheEntry(key, request, actual_row_uuid));
  add("hot_point_lookup_cache_admission=" + put.diagnostic_code);
  add("hot_point_lookup_cache_admitted=" + std::string(BoolText(put.admitted)));
  add("hot_point_lookup_cache_actual_row_locator=" + actual_row_uuid);
  for (const auto& item : put.evidence) {
    add("hot_point_lookup_cache_admission_evidence=" + item);
  }
}

std::string SerializeDmlTargetAccessPlanEvidence(const DmlTargetAccessPlan& plan) {
  std::ostringstream out;
  out << "{";
  out << "\"ok\":" << (plan.ok ? "true" : "false") << ",";
  out << "\"access_kind\":\"" << DmlTargetAccessKindName(plan.access_kind) << "\",";
  out << "\"physical_access_kind\":\"" << JsonEscape(plan.physical_access_kind) << "\",";
  out << "\"executor_capability\":\"" << JsonEscape(plan.executor_capability) << "\",";
  out << "\"relation_uuid\":\"" << JsonEscape(plan.relation_uuid) << "\",";
  out << "\"predicate_kind\":\"" << JsonEscape(plan.predicate_kind) << "\",";
  out << "\"row_uuid\":\"" << JsonEscape(plan.row_uuid) << "\",";
  out << "\"row_uuid_count\":" << plan.row_uuids.size() << ",";
  out << "\"index_uuid\":\"" << JsonEscape(plan.index_uuid) << "\",";
  out << "\"estimated_rows\":" << plan.estimated_rows << ",";
  out << "\"diagnostics\":[";
  for (std::size_t index = 0; index < plan.diagnostics.size(); ++index) {
    out << "\"" << JsonEscape(plan.diagnostics[index]) << "\"";
    if (index + 1 != plan.diagnostics.size()) out << ",";
  }
  out << "],\"evidence\":[";
  for (std::size_t index = 0; index < plan.evidence.size(); ++index) {
    out << "\"" << JsonEscape(plan.evidence[index]) << "\"";
    if (index + 1 != plan.evidence.size()) out << ",";
  }
  out << "]}";
  return out.str();
}

}  // namespace scratchbird::engine::internal_api
