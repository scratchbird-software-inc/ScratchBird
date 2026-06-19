// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/plan_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/descriptor_api.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/sys_information_projection.hpp"
#include "crud_support/crud_store.hpp"
#include "executor_foundation.hpp"
#include "logical_plan.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "metric_registry.hpp"
#include "optimizer_contract.hpp"
#include "optimizer_plan_cache.hpp"
#include "physical_plan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace metrics = scratchbird::core::metrics;

plan::PhysicalAccessKind AccessKindForQueryOperation(const std::string& operation);
bool StatisticsForcedStale(const EnginePlanOperationRequest& request);
bool ProjectionRowMatchesPredicate(const EngineRowValue& row,
                                   const EnginePredicateEnvelope& predicate);

bool IsExecutableUpperAccessKind(plan::PhysicalAccessKind access_kind) {
  return access_kind == plan::PhysicalAccessKind::kAggregateGeneric ||
         access_kind == plan::PhysicalAccessKind::kAggregateHash ||
         access_kind == plan::PhysicalAccessKind::kSort ||
         access_kind == plan::PhysicalAccessKind::kTopN ||
         access_kind == plan::PhysicalAccessKind::kSortThenWindow ||
         access_kind == plan::PhysicalAccessKind::kCteInline ||
         access_kind == plan::PhysicalAccessKind::kCteMaterialize ||
         access_kind == plan::PhysicalAccessKind::kSetOperation;
}

template <typename TResult>
TResult QueryFailure(const EngineRequestContext& context, const std::string& detail) {
  TResult result;
  result.ok = false;
  result.operation_id = "query.plan_operation";
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(MakeInvalidRequestDiagnostic("query.plan_operation", detail));
  return result;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string UpperAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
  return value;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) { return option.substr(prefix.size()); }
  }
  return {};
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) {
    if (!current.empty()) { parts.push_back(current); }
  }
  return parts;
}

std::string TrimAscii(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::vector<std::string> SplitOptionList(const EngineApiRequest& request,
                                         const std::string& prefix) {
  std::vector<std::string> parts;
  for (auto part : Split(OptionValue(request, prefix), ',')) {
    part = TrimAscii(std::move(part));
    if (!part.empty()) parts.push_back(std::move(part));
  }
  return parts;
}

std::size_t ParseSizeValue(const std::string& value, std::size_t fallback) {
  if (value.empty()) { return fallback; }
  std::uint64_t parsed = 0;
  for (const char c : value) {
    if (!std::isdigit(static_cast<unsigned char>(c))) { return fallback; }
    parsed = (parsed * 10) + static_cast<unsigned>(c - '0');
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) { return fallback; }
  }
  return static_cast<std::size_t>(parsed);
}

std::uint64_t ParseU64Value(const std::string& value, std::uint64_t fallback) {
  if (value.empty()) { return fallback; }
  std::uint64_t parsed = 0;
  for (const char c : value) {
    if (!std::isdigit(static_cast<unsigned char>(c))) { return fallback; }
    const std::uint64_t digit = static_cast<unsigned>(c - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) { return fallback; }
    parsed = (parsed * 10) + digit;
  }
  return parsed;
}

bool ParseBoolValue(const std::string& value, bool fallback) {
  const std::string lowered = LowerAscii(value);
  if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "asc" || lowered == "ascending") {
    return true;
  }
  if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "desc" || lowered == "descending") {
    return false;
  }
  return fallback;
}

bool RequestOptionEnabled(const EngineApiRequest& request,
                          const std::string& primary_prefix,
                          const std::string& alias_prefix = {}) {
  const std::string primary = LowerAscii(OptionValue(request, primary_prefix));
  const std::string alias = alias_prefix.empty() ? std::string{} : LowerAscii(OptionValue(request, alias_prefix));
  return primary == "enabled" || primary == "true" || primary == "1" ||
         alias == "enabled" || alias == "true" || alias == "1";
}

bool RequestOptionDisabled(const EngineApiRequest& request,
                           const std::string& primary_prefix,
                           const std::string& alias_prefix = {}) {
  const std::string primary = LowerAscii(OptionValue(request, primary_prefix));
  const std::string alias = alias_prefix.empty() ? std::string{} : LowerAscii(OptionValue(request, alias_prefix));
  return primary == "disabled" || primary == "false" || primary == "0" ||
         alias == "disabled" || alias == "false" || alias == "0";
}

void AddSbsfc081Evidence(EnginePlanOperationResult* result,
                         const EnginePlanOperationRequest& request) {
  if (result == nullptr) return;
  const std::string surface_id = OptionValue(request, "sbsfc081_surface_id:");
  if (surface_id.empty()) return;
  const std::string evidence_kind = OptionValue(request, "sbsfc081_runtime_evidence_kind:");
  const std::string evidence_id = OptionValue(request, "sbsfc081_runtime_evidence_id:");
  const std::string descriptor_role = OptionValue(request, "sbsfc081_descriptor_role:");
  AddApiBehaviorEvidence(result,
                         evidence_kind.empty() ? "sbsfc081_descriptor_expression_route"
                                               : evidence_kind,
                         evidence_id.empty() ? surface_id : evidence_id);
  AddApiBehaviorEvidence(result, "sbsfc081_surface", surface_id);
  AddApiBehaviorEvidence(result,
                         "sbsfc081_descriptor_role",
                         descriptor_role.empty() ? "general_descriptor" : descriptor_role);
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

void AddSbsfc082Evidence(EnginePlanOperationResult* result,
                         const EnginePlanOperationRequest& request) {
  if (result == nullptr) return;
  const std::string surface_id = OptionValue(request, "sbsfc082_surface_id:");
  if (surface_id.empty()) return;
  const std::string evidence_kind = OptionValue(request, "sbsfc082_runtime_evidence_kind:");
  const std::string evidence_id = OptionValue(request, "sbsfc082_runtime_evidence_id:");
  const std::string descriptor_role = OptionValue(request, "sbsfc082_descriptor_role:");
  const std::string descriptor_ref = OptionValue(request, "sbsfc082_descriptor_ref:");
  AddApiBehaviorEvidence(result,
                         evidence_kind.empty() ? "sbsfc082_surface_descriptor_route"
                                               : evidence_kind,
                         evidence_id.empty() ? surface_id : evidence_id);
  AddApiBehaviorEvidence(result, "sbsfc082_surface", surface_id);
  AddApiBehaviorEvidence(result,
                         "sbsfc082_descriptor_role",
                         descriptor_role.empty() ? "general_descriptor" : descriptor_role);
  if (!descriptor_ref.empty()) {
    AddApiBehaviorEvidence(result, "sbsfc082_descriptor_ref", descriptor_ref);
  }
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

void AddSbsfc083Evidence(EnginePlanOperationResult* result,
                         const EnginePlanOperationRequest& request) {
  if (result == nullptr) return;
  const std::string surface_id = OptionValue(request, "sbsfc083_surface_id:");
  if (surface_id.empty()) return;
  const std::string evidence_kind = OptionValue(request, "sbsfc083_runtime_evidence_kind:");
  const std::string evidence_id = OptionValue(request, "sbsfc083_runtime_evidence_id:");
  const std::string descriptor_role = OptionValue(request, "sbsfc083_descriptor_role:");
  const std::string descriptor_ref = OptionValue(request, "sbsfc083_descriptor_ref:");
  AddApiBehaviorEvidence(result,
                         evidence_kind.empty() ? "sbsfc083_grammar_surface_route"
                                               : evidence_kind,
                         evidence_id.empty() ? surface_id : evidence_id);
  AddApiBehaviorEvidence(result, "sbsfc083_surface", surface_id);
  AddApiBehaviorEvidence(result,
                         "sbsfc083_descriptor_role",
                         descriptor_role.empty() ? "general_descriptor" : descriptor_role);
  if (!descriptor_ref.empty()) {
    AddApiBehaviorEvidence(result, "sbsfc083_descriptor_ref", descriptor_ref);
  }
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

void AddSbsfc084Evidence(EnginePlanOperationResult* result,
                         const EnginePlanOperationRequest& request) {
  if (result == nullptr) return;
  const std::string surface_id = OptionValue(request, "sbsfc084_surface_id:");
  if (surface_id.empty()) return;
  const std::string evidence_kind = OptionValue(request, "sbsfc084_runtime_evidence_kind:");
  const std::string evidence_id = OptionValue(request, "sbsfc084_runtime_evidence_id:");
  const std::string descriptor_role = OptionValue(request, "sbsfc084_descriptor_role:");
  const std::string descriptor_ref = OptionValue(request, "sbsfc084_descriptor_ref:");
  AddApiBehaviorEvidence(result,
                         evidence_kind.empty() ? "sbsfc084_grammar_surface_route"
                                               : evidence_kind,
                         evidence_id.empty() ? surface_id : evidence_id);
  AddApiBehaviorEvidence(result, "sbsfc084_surface", surface_id);
  AddApiBehaviorEvidence(result,
                         "sbsfc084_descriptor_role",
                         descriptor_role.empty() ? "general_descriptor" : descriptor_role);
  if (!descriptor_ref.empty()) {
    AddApiBehaviorEvidence(result, "sbsfc084_descriptor_ref", descriptor_ref);
  }
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

void AddSbsfc085Evidence(EnginePlanOperationResult* result,
                         const EnginePlanOperationRequest& request) {
  if (result == nullptr) return;
  const std::string surface_id = OptionValue(request, "sbsfc085_surface_id:");
  if (surface_id.empty()) return;
  const std::string evidence_kind = OptionValue(request, "sbsfc085_runtime_evidence_kind:");
  const std::string evidence_id = OptionValue(request, "sbsfc085_runtime_evidence_id:");
  const std::string descriptor_role = OptionValue(request, "sbsfc085_descriptor_role:");
  const std::string descriptor_ref = OptionValue(request, "sbsfc085_descriptor_ref:");
  AddApiBehaviorEvidence(result,
                         evidence_kind.empty() ? "sbsfc085_grammar_surface_route"
                                               : evidence_kind,
                         evidence_id.empty() ? surface_id : evidence_id);
  AddApiBehaviorEvidence(result, "sbsfc085_surface", surface_id);
  AddApiBehaviorEvidence(result,
                         "sbsfc085_descriptor_role",
                         descriptor_role.empty() ? "general_descriptor" : descriptor_role);
  if (!descriptor_ref.empty()) {
    AddApiBehaviorEvidence(result, "sbsfc085_descriptor_ref", descriptor_ref);
  }
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "parser_claims_transaction_finality", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

std::vector<std::size_t> ParseProjectedColumns(const EnginePlanOperationRequest& request) {
  if (!request.projected_columns.empty()) { return request.projected_columns; }
  const std::string encoded = OptionValue(request, "project_columns:");
  std::vector<std::size_t> columns;
  for (const auto& part : Split(encoded, ',')) { columns.push_back(ParseSizeValue(part, 0)); }
  return columns;
}

bool TryParseI64Value(const std::string& value, std::int64_t* out) {
  if (value.empty()) { return false; }
  char* end = nullptr;
  const long long parsed = std::strtoll(value.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') { return false; }
  if (out != nullptr) { *out = static_cast<std::int64_t>(parsed); }
  return true;
}

bool TryParseReal64Value(const std::string& value, double* out) {
  if (value.empty()) { return false; }
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) { return false; }
  if (out != nullptr) { *out = parsed; }
  return true;
}

std::int64_t ParseI64Value(const std::string& value) {
  std::int64_t parsed = 0;
  (void)TryParseI64Value(value, &parsed);
  return parsed;
}

double ParseReal64Value(const std::string& value, double fallback) {
  double parsed = fallback;
  return TryParseReal64Value(value, &parsed) ? parsed : fallback;
}

std::uint64_t EncodedRowBytes(const EngineRowValue& row) {
  std::uint64_t bytes = 0;
  for (const auto& [field, typed] : row.fields) {
    bytes += field.size();
    bytes += typed.encoded_value.size();
    bytes += typed.descriptor.encoded_descriptor.size();
  }
  return bytes;
}

double MetricValueAsDouble(const metrics::MetricValue& value) {
  if (value.type == metrics::MetricType::histogram && value.count != 0) {
    return value.sum / static_cast<double>(value.count);
  }
  return value.value;
}

std::optional<double> CurrentMetricValue(const std::string& family) {
  const auto snapshot = metrics::DefaultMetricRegistry().SnapshotCurrent(true);
  auto it = std::find_if(snapshot.begin(), snapshot.end(), [&](const metrics::MetricValue& value) {
    return value.family == family;
  });
  if (it == snapshot.end()) { return std::nullopt; }
  return MetricValueAsDouble(*it);
}

void AddRuntimeStatistic(opt::OptimizerStatisticsCatalog* catalog,
                         std::string name,
                         std::string scope,
                         std::string object_uuid,
                         double value,
                         opt::CostConfidence confidence = opt::CostConfidence::kMedium) {
  catalog->Add(opt::MakeStatistic(std::move(name),
                                  std::move(scope),
                                  std::move(object_uuid),
                                  value,
                                  opt::StatisticSource::kRuntimeMetric,
                                  1,
                                  0,
                                  confidence,
                                  true));
}

std::uint64_t CountRetainedVersions(const CrudState& state, const std::string& table_uuid) {
  return static_cast<std::uint64_t>(std::count_if(state.row_versions.begin(), state.row_versions.end(), [&](const CrudRowVersionRecord& row) {
    return row.table_uuid == table_uuid;
  }));
}

std::uint64_t CountIndexEntries(const CrudState& state, const std::string& index_uuid) {
  return static_cast<std::uint64_t>(std::count_if(state.index_entries.begin(), state.index_entries.end(), [&](const CrudIndexEntryRecord& entry) {
    return entry.index_uuid == index_uuid;
  }));
}

std::uint64_t CountDistinctIndexKeys(const CrudState& state, const std::string& index_uuid) {
  std::vector<std::string> keys;
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid == index_uuid) { keys.push_back(entry.key_value); }
  }
  std::sort(keys.begin(), keys.end());
  return static_cast<std::uint64_t>(std::unique(keys.begin(), keys.end()) - keys.begin());
}

EngineLocalizedName NameForPlanCacheResolution(const EnginePlanOperationRequest& request,
                                               const std::string& value) {
  EngineLocalizedName name;
  name.language_tag = request.context.language_context.language_tag.empty()
                          ? "en"
                          : request.context.language_context.language_tag;
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

bool RelationNameLooksResolvable(const std::string& relation_name) {
  return !relation_name.empty() &&
         relation_name != "request_rows" &&
         relation_name.rfind("crud:", 0) != 0 &&
         relation_name.find(':') == std::string::npos;
}

struct PlanCacheBinding {
  std::vector<std::string> object_uuids;
  std::vector<std::string> descriptor_digests;
  std::vector<std::string> evidence;
};

std::string DescriptorDigest(const EngineDescriptor& descriptor) {
  return descriptor.descriptor_uuid.canonical + ":" + descriptor.descriptor_kind + ":" +
         descriptor.canonical_type_name + ":" + descriptor.encoded_descriptor;
}

void AddDescriptorBinding(PlanCacheBinding* binding,
                          const EnginePlanOperationRequest& request,
                          const std::string& object_uuid,
                          const std::string& object_kind) {
  if (binding == nullptr || object_uuid.empty()) return;
  EngineGetDescriptorRequest descriptor_request;
  descriptor_request.context = request.context;
  descriptor_request.target_object.uuid.canonical = object_uuid;
  descriptor_request.target_object.object_kind = object_kind.empty() ? "table" : object_kind;
  descriptor_request.option_envelopes.push_back("descriptor_cache:enabled");
  const auto descriptor = EngineGetDescriptor(descriptor_request);
  binding->object_uuids.push_back(object_uuid);
  if (descriptor.ok) {
    binding->descriptor_digests.push_back(DescriptorDigest(descriptor.descriptor));
    binding->evidence.push_back("descriptor:" + object_uuid);
  } else {
    binding->descriptor_digests.push_back("descriptor_unavailable:" + object_uuid);
    binding->evidence.push_back("descriptor_unavailable:" + object_uuid);
  }
}

PlanCacheBinding BindPlanCacheRelations(const EnginePlanOperationRequest& request,
                                        const std::vector<EngineQueryRelation>& relations) {
  PlanCacheBinding binding;
  for (const auto& relation : relations) {
    if (!relation.source_object.uuid.canonical.empty()) {
      AddDescriptorBinding(&binding,
                           request,
                           relation.source_object.uuid.canonical,
                           relation.source_object.object_kind);
      continue;
    }
    if (RelationNameLooksResolvable(relation.relation_name) && !request.context.database_path.empty()) {
      EngineResolveNameRequest resolve;
      resolve.context = request.context;
      resolve.target_schema = request.target_schema;
      resolve.target_object.object_kind = relation.source_object.object_kind.empty()
                                              ? "table"
                                              : relation.source_object.object_kind;
      resolve.localized_names.push_back(NameForPlanCacheResolution(request, relation.relation_name));
      const auto resolved = EngineResolveName(resolve);
      if (resolved.ok && !resolved.bound_object_identity.object_uuid.canonical.empty()) {
        binding.evidence.push_back("resolver:" + resolved.bound_object_identity.object_uuid.canonical);
        AddDescriptorBinding(&binding,
                             request,
                             resolved.bound_object_identity.object_uuid.canonical,
                             resolved.bound_object_identity.resolved_object_type);
        continue;
      }
    }
    const std::string fallback = relation.descriptor_digest.empty()
                                     ? relation.relation_name
                                     : relation.descriptor_digest;
    if (!fallback.empty()) {
      binding.descriptor_digests.push_back("inline:" + fallback);
    }
  }
  return binding;
}

std::string JoinSortedValues(std::vector<std::string> values, char delimiter) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  std::ostringstream out;
  for (const auto& value : values) {
    if (out.tellp() > 0) out << delimiter;
    out << value;
  }
  return out.str();
}

std::string PlanCacheStatisticsSnapshotId(const EnginePlanOperationRequest& request,
                                          const std::vector<EngineQueryRelation>& relations) {
  std::string explicit_snapshot = OptionValue(request, "statistics_snapshot_id:");
  if (!explicit_snapshot.empty()) return explicit_snapshot;
  explicit_snapshot = OptionValue(request, "statistics_epoch:");
  if (!explicit_snapshot.empty()) return "statistics_epoch:" + explicit_snapshot;
  std::ostringstream out;
  out << "catalog=" << request.context.catalog_generation_id
      << ";resource=" << request.context.resource_epoch
      << ";name=" << request.context.name_resolution_epoch
      << ";relations=";
  for (const auto& relation : relations) {
    out << relation.rows.size() << ',';
  }
  return out.str();
}

std::string JoinProfileDigest(const EngineProfileSet& profile, const std::string& fallback) {
  std::vector<std::string> values = profile.names;
  values.insert(values.end(), profile.encoded_profiles.begin(), profile.encoded_profiles.end());
  const std::string digest = JoinSortedValues(std::move(values), ',');
  return digest.empty() ? fallback : digest;
}

std::string PlanCacheParameterShapeDigest(const EnginePlanOperationRequest& request,
                                          const std::vector<EngineQueryRelation>& relations) {
  const std::string explicit_digest = OptionValue(request, "parameter_shape_digest:");
  if (!explicit_digest.empty()) return explicit_digest;
  std::ostringstream out;
  out << "predicate=" << request.predicate.predicate_kind
      << ";range=" << OptionValue(request, "parameter_range_shape:")
      << ";cardinality=" << OptionValue(request, "parameter_cardinality_shape:")
      << ";values=";
  std::uint64_t ordinal = 0;
  for (const auto& value : request.predicate.bound_values) {
    out << ordinal++ << ':'
        << value.descriptor.descriptor_uuid.canonical << ':'
        << value.descriptor.canonical_type_name << ':'
        << value.descriptor.descriptor_kind << ':'
        << (value.is_null ? "null" : "not_null") << ':'
        << (value.encoded_value.empty() ? "unbound_or_empty" : "bound") << ';';
  }
  out << "relations=";
  for (const auto& relation : relations) {
    out << relation.relation_name << ':' << relation.rows.size() << ';';
  }
  return out.str();
}

std::string PlanCacheMemoryGrantClass(const EnginePlanOperationRequest& request) {
  const std::string explicit_class = OptionValue(request, "memory_grant_class:");
  if (!explicit_class.empty()) return explicit_class;
  const std::size_t row_goal = request.limit == 0 ? request.context.last_row_count : request.limit;
  if (row_goal > 100000) return "memory_grant:large";
  if (row_goal > 1000) return "memory_grant:medium";
  return "memory_grant:small";
}

std::string PlanCacheMemoryGrantDigest(const EnginePlanOperationRequest& request,
                                       const std::string& memory_grant_class) {
  const std::string explicit_digest = OptionValue(request, "memory_grant_digest:");
  if (!explicit_digest.empty()) return explicit_digest;
  std::ostringstream out;
  out << memory_grant_class
      << ";resource_epoch=" << request.context.resource_epoch
      << ";limit=" << request.limit
      << ";last_row_count=" << (request.context.last_row_count_present
                                    ? std::to_string(request.context.last_row_count)
                                    : "unknown");
  return out.str();
}

opt::OptimizerPlanCache& LiveOptimizerPlanCache() {
  static opt::OptimizerPlanCache cache;
  return cache;
}

opt::OptimizerPlanCacheKeyInput BuildLiveOptimizerPlanCacheKeyInput(
    const EnginePlanOperationRequest& request,
    const std::string& operation,
    const std::vector<EngineQueryRelation>& relations,
    const PlanCacheBinding& binding) {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = operation;
  input.sblr_digest = OptionValue(request, "sblr_digest:");
  if (input.sblr_digest.empty()) {
    input.sblr_digest = "engine.query.plan_operation:" + operation;
  }
  input.descriptor_set_digest = JoinSortedValues(binding.descriptor_digests, ',');
  input.statistics_snapshot_id = PlanCacheStatisticsSnapshotId(request, relations);
  input.catalog_stats_digest = OptionValue(request, "catalog_stats_digest:");
  if (input.catalog_stats_digest.empty()) input.catalog_stats_digest = input.statistics_snapshot_id;
  input.cost_profile_id = "runtime_local_cost_profile";
  input.executor_capability_set_id = OptionValue(request, "executor_capability_set_id:");
  if (input.executor_capability_set_id.empty()) input.executor_capability_set_id = "local_noncluster_executor";
  input.route_capability_digest = OptionValue(request, "route_capability_digest:");
  if (input.route_capability_digest.empty()) input.route_capability_digest = input.executor_capability_set_id;
  input.security_policy_digest = OptionValue(request, "security_policy_digest:");
  if (input.security_policy_digest.empty()) {
    input.security_policy_digest = JoinProfileDigest(request.policy_profile,
                                                     "security_epoch:" + std::to_string(request.context.security_epoch));
  }
  input.redaction_route_digest = OptionValue(request, "redaction_route_digest:");
  if (input.redaction_route_digest.empty()) input.redaction_route_digest = "redaction_route:none";
  input.parameter_shape_digest = PlanCacheParameterShapeDigest(request, relations);
  input.memory_grant_class = PlanCacheMemoryGrantClass(request);
  input.memory_grant_digest = PlanCacheMemoryGrantDigest(request, input.memory_grant_class);
  input.catalog_epoch = request.context.catalog_generation_id;
  input.stats_epoch = ParseU64Value(OptionValue(request, "stats_epoch:"), request.context.catalog_generation_id);
  input.security_epoch = request.context.security_epoch;
  input.policy_epoch = request.context.resource_epoch;
  input.resource_epoch = request.context.resource_epoch;
  input.name_resolution_epoch = request.context.name_resolution_epoch;
  input.memory_policy_epoch = ParseU64Value(OptionValue(request, "memory_policy_epoch:"),
                                            request.context.resource_epoch);
  input.compatibility_epoch = ParseU64Value(OptionValue(request, "compatibility_epoch:"),
                                            request.context.catalog_generation_id);
  input.format_compatibility_epoch = ParseU64Value(OptionValue(request, "format_compatibility_epoch:"),
                                                   request.context.catalog_generation_id);
  input.route_epoch = ParseU64Value(OptionValue(request, "route_epoch:"),
                                    request.context.resource_epoch);
  input.object_uuids = binding.object_uuids;
  for (const auto& object : request.related_objects) {
    if (object.uuid.canonical.empty()) continue;
    const std::string kind = LowerAscii(object.object_kind);
    if (kind.find("function") != std::string::npos || kind.find("udr") != std::string::npos) {
      input.function_uuids.push_back(object.uuid.canonical);
    } else if (kind.find("index") != std::string::npos) {
      input.index_uuids.push_back(object.uuid.canonical);
    } else if (kind.find("filespace") != std::string::npos) {
      input.filespace_uuids.push_back(object.uuid.canonical);
    } else {
      input.object_uuids.push_back(object.uuid.canonical);
    }
  }
  if (!request.target_object.uuid.canonical.empty()) {
    input.object_uuids.push_back(request.target_object.uuid.canonical);
  }
  for (const auto& index : request.indexes) {
    if (!index.requested_index_uuid.canonical.empty()) {
      input.index_uuids.push_back(index.requested_index_uuid.canonical);
    }
  }
  const std::string function_dependency = OptionValue(request, "function_dependency_uuid:");
  if (!function_dependency.empty()) input.function_uuids.push_back(function_dependency);
  const std::string filespace_dependency = OptionValue(request, "filespace_dependency_uuid:");
  if (!filespace_dependency.empty()) input.filespace_uuids.push_back(filespace_dependency);
  return input;
}

std::string BuildLiveOptimizerPlanCacheKey(const EnginePlanOperationRequest& request,
                                           const std::string& operation,
                                           const std::vector<EngineQueryRelation>& relations,
                                           const PlanCacheBinding& binding) {
  return opt::BuildOptimizerPlanCacheKey(
      BuildLiveOptimizerPlanCacheKeyInput(request, operation, relations, binding));
}

void AttachLivePlanCacheEvidence(EnginePlanOperationResult* result,
                                 const std::string& cache_state,
                                 const std::string& cache_key,
                                 const PlanCacheBinding& binding,
                                 const std::string& diagnostic_code = {},
                                 const std::vector<std::string>& lookup_evidence = {}) {
  if (result == nullptr) return;
  AddApiBehaviorEvidence(result, "optimizer_live_plan_cache", cache_state);
  if (!cache_key.empty()) {
    AddApiBehaviorEvidence(result, "optimizer_live_plan_cache_key", cache_key);
  }
  if (!diagnostic_code.empty()) {
    AddApiBehaviorEvidence(result, "optimizer_live_plan_cache_diagnostic", diagnostic_code);
  }
  for (const auto& evidence : lookup_evidence) {
    AddApiBehaviorEvidence(result, "optimizer_live_plan_cache_evidence", evidence);
  }
  for (const auto& item : binding.evidence) {
    AddApiBehaviorEvidence(result, "optimizer_live_plan_cache_binding", item);
  }
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "parser_claims_transaction_finality", "false");
}

CrudStoreResult LoadQueryCrudCompatibilityState(const EngineRequestContext& context) {
  CrudStoreResult result;
  const auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.ok = false;
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  result.ok = true;
  result.state = BuildCrudCompatibilityStateFromMga(loaded.state);
  return result;
}

opt::OptimizerStatisticsCatalog BuildRuntimeOptimizerStatistics(const EnginePlanOperationRequest& request,
                                                                const std::vector<EngineQueryRelation>& relations) {
  opt::OptimizerStatisticsCatalog catalog = opt::DefaultLocalStatisticsCatalog();
  opt::AddClusterUnavailableStatistics(&catalog);
  if (StatisticsForcedStale(request)) {
    catalog.Add(opt::MakeStatistic("runtime_relation_statistics_forced_stale",
                                   "relation",
                                   "runtime.request",
                                   0.0,
                                   opt::StatisticSource::kUnavailable,
                                   0,
                                   0,
                                   opt::CostConfidence::kRejected,
                                   false));
    return catalog;
  }

  CrudStoreResult loaded;
  const bool can_load_crud = request.context.local_transaction_id != 0 && !request.context.database_path.empty();
  if (can_load_crud) { loaded = LoadQueryCrudCompatibilityState(request.context); }

  for (const auto& relation : relations) {
    const std::string object_uuid = !relation.source_object.uuid.canonical.empty()
        ? relation.source_object.uuid.canonical
        : (relation.descriptor_digest.empty() ? relation.relation_name : relation.descriptor_digest);
    std::uint64_t visible_rows = static_cast<std::uint64_t>(relation.rows.size());
    std::uint64_t retained_versions = visible_rows;
    if (loaded.ok && !relation.source_object.uuid.canonical.empty()) {
      const auto persistent_visible_rows =
          static_cast<std::uint64_t>(VisibleCrudRowsForContext(loaded.state,
                                                               relation.source_object.uuid.canonical,
                                                               request.context)
                                         .size());
      if (visible_rows == 0) {
        visible_rows = persistent_visible_rows;
      }
      retained_versions = std::max<std::uint64_t>(CountRetainedVersions(loaded.state, relation.source_object.uuid.canonical), visible_rows);
    }
    std::uint64_t total_bytes = 0;
    for (const auto& row : relation.rows) { total_bytes += EncodedRowBytes(row); }
    const std::uint64_t average_row_bytes = visible_rows == 0 ? 32 : std::max<std::uint64_t>(total_bytes / visible_rows, 1);
    const std::uint64_t page_count = std::max<std::uint64_t>(1, ((std::max<std::uint64_t>(retained_versions, 1) * average_row_bytes) + 8191) / 8192);

    AddRuntimeStatistic(&catalog, "row_count", "relation", object_uuid, static_cast<double>(visible_rows), opt::CostConfidence::kHigh);
    AddRuntimeStatistic(&catalog, "visible_row_count", "relation", object_uuid, static_cast<double>(visible_rows), opt::CostConfidence::kHigh);
    AddRuntimeStatistic(&catalog, "relation_visible_version_count", "relation", object_uuid, static_cast<double>(visible_rows), opt::CostConfidence::kHigh);
    AddRuntimeStatistic(&catalog, "relation_retained_version_count", "relation", object_uuid, static_cast<double>(retained_versions), opt::CostConfidence::kHigh);
    AddRuntimeStatistic(&catalog, "page_count", "relation", object_uuid, static_cast<double>(page_count), opt::CostConfidence::kMedium);
    AddRuntimeStatistic(&catalog, "average_row_bytes", "relation", object_uuid, static_cast<double>(average_row_bytes), opt::CostConfidence::kMedium);

    if (loaded.ok && !relation.source_object.uuid.canonical.empty()) {
      const auto indexes = VisibleCrudIndexesForTable(loaded.state,
                                                      relation.source_object.uuid.canonical,
                                                      request.context.local_transaction_id);
      for (const auto& index : indexes) {
        const auto entry_count = CountIndexEntries(loaded.state, index.index_uuid);
        const auto distinct_keys = CountDistinctIndexKeys(loaded.state, index.index_uuid);
        const auto height = std::max<std::uint64_t>(1, 1 + (entry_count / 1024));
        const auto leaf_pages = std::max<std::uint64_t>(1, (entry_count + 63) / 64);
        const double selectivity = visible_rows == 0 || distinct_keys == 0
            ? 1.0
            : std::clamp(1.0 / static_cast<double>(distinct_keys), 1.0 / static_cast<double>(visible_rows), 1.0);
        AddRuntimeStatistic(&catalog, "index_depth", "index", object_uuid, static_cast<double>(height), opt::CostConfidence::kMedium);
        AddRuntimeStatistic(&catalog, "index_leaf_pages", "index", object_uuid, static_cast<double>(leaf_pages), opt::CostConfidence::kMedium);
        AddRuntimeStatistic(&catalog, "index_selectivity", "index", object_uuid, selectivity, opt::CostConfidence::kMedium);
        AddRuntimeStatistic(&catalog, "index_fragmentation_ratio", "index", object_uuid, 0.0, opt::CostConfidence::kLow);
        AddRuntimeStatistic(&catalog, "index_visibility_coverage", "index", object_uuid, 1.0, opt::CostConfidence::kHigh);
        AddRuntimeStatistic(&catalog, "index_depth", "index", index.index_uuid, static_cast<double>(height), opt::CostConfidence::kMedium);
        AddRuntimeStatistic(&catalog, "index_leaf_pages", "index", index.index_uuid, static_cast<double>(leaf_pages), opt::CostConfidence::kMedium);
        AddRuntimeStatistic(&catalog, "index_selectivity", "index", index.index_uuid, selectivity, opt::CostConfidence::kMedium);
        AddRuntimeStatistic(&catalog, "index_fragmentation_ratio", "index", index.index_uuid, 0.0, opt::CostConfidence::kLow);
        AddRuntimeStatistic(&catalog, "index_visibility_coverage", "index", index.index_uuid, 1.0, opt::CostConfidence::kHigh);
      }
    }
  }

  if (const auto memory_available = CurrentMetricValue("sb_memory_emergency_reserve_bytes")) {
    AddRuntimeStatistic(&catalog, "memory_grant_available_bytes", "session", "local.default", *memory_available, opt::CostConfidence::kMedium);
  }
  if (const auto filespace_free = CurrentMetricValue("sb_filespace_free_bytes")) {
    AddRuntimeStatistic(&catalog, "filespace_available_pages", "filespace", "local.default", std::max(1.0, *filespace_free / 8192.0), opt::CostConfidence::kMedium);
  }
  if (const auto read_latency = CurrentMetricValue("sb_filespace_device_read_latency_microseconds")) {
    AddRuntimeStatistic(&catalog, "page_family_read_latency_microseconds", "page_family", "local.default", *read_latency, opt::CostConfidence::kMedium);
    AddRuntimeStatistic(&catalog, "io_latency_multiplier", "page_family", "local.default", std::clamp(*read_latency / 1000.0, 0.25, 10.0), opt::CostConfidence::kMedium);
  }
  if (const auto lookup_latency = CurrentMetricValue("sb_index_lookup_latency_microseconds")) {
    AddRuntimeStatistic(&catalog, "operator_latency_multiplier", "index", "local.default", std::clamp(*lookup_latency / 1000.0, 0.25, 10.0), opt::CostConfidence::kMedium);
  }
  if (const auto resident_pages = CurrentMetricValue("sb_page_cache_resident_pages")) {
    AddRuntimeStatistic(&catalog, "page_cache_hit_ratio", "page_cache", "local.default", *resident_pages > 0.0 ? 0.75 : 0.0, opt::CostConfidence::kLow);
  }
  if (const auto dirty_pages = CurrentMetricValue("sb_page_cache_dirty_pages")) {
    AddRuntimeStatistic(&catalog, "page_cache_pressure_level", "page_cache", "local.default", std::clamp(*dirty_pages / 1024.0, 0.0, 10.0), opt::CostConfidence::kLow);
    AddRuntimeStatistic(&catalog, "estimate_uncertainty", "page_cache", "local.default", std::clamp(*dirty_pages, 0.0, 1000000.0), opt::CostConfidence::kLow);
  }
  return catalog;
}

bool StatisticsForcedStale(const EnginePlanOperationRequest& request) {
  return RequestOptionEnabled(request, "optimizer_force_stale_stats:", "statistics_stale:") ||
         RequestOptionDisabled(request, "optimizer_statistics:", "statistics_enabled:");
}

bool StatisticUsable(const opt::OptimizerStatisticsCatalog& statistics,
                     const std::string& name,
                     const std::string& object_uuid,
                     const EnginePlanOperationRequest& request) {
  if (StatisticsForcedStale(request)) return false;
  const auto statistic = statistics.Find(name, object_uuid);
  if (!statistic || !statistic->available) return false;
  if (statistic->confidence == opt::CostConfidence::kUnknown ||
      statistic->confidence == opt::CostConfidence::kRejected) {
    return false;
  }
  const auto max_freshness =
      ParseSizeValue(OptionValue(request, "optimizer_max_stats_freshness_us:"), 300000000);
  return statistic->freshness_microseconds <= max_freshness;
}

std::string RelationObjectUuid(const EngineQueryRelation& relation) {
  if (!relation.source_object.uuid.canonical.empty()) {
    return relation.source_object.uuid.canonical;
  }
  if (!relation.descriptor_digest.empty()) return relation.descriptor_digest;
  if (!relation.relation_name.empty()) return relation.relation_name;
  return "local.default";
}

bool PredicateCanUseScalarBtree(const EnginePredicateEnvelope& predicate) {
  return predicate.predicate_kind == "column_equals" ||
         predicate.predicate_kind == "columns_all_equal" ||
         predicate.predicate_kind == "expression_equals";
}

bool PredicateCanUseScalarBtreeRange(const EnginePredicateEnvelope& predicate) {
  return predicate.predicate_kind == "column_range" ||
         predicate.predicate_kind == "column_in_list";
}

bool RequestProjectionCovered(const EnginePlanOperationRequest& request,
                              const CrudIndexRecord& index) {
  if (RequestOptionEnabled(request, "optimizer_projection_covered:", "projection_covered:")) {
    return true;
  }
  const auto projected = SplitOptionList(request, "project_fields:");
  if (projected.empty()) return false;
  std::vector<std::string> key_columns = index.key_envelopes;
  if (key_columns.empty() && !index.column_name.empty()) {
    key_columns.push_back(index.column_name);
  }
  std::set<std::string> covered(key_columns.begin(), key_columns.end());
  covered.insert(index.include_columns.begin(), index.include_columns.end());
  return std::all_of(projected.begin(), projected.end(), [&](const std::string& field) {
    return covered.find(field) != covered.end();
  });
}

std::optional<CrudIndexRecord> UsableCrudIndexForPredicate(const EnginePlanOperationRequest& request,
                                                           const std::string& relation_uuid,
                                                           const EnginePredicateEnvelope& predicate,
                                                           std::vector<EngineEvidenceReference>* evidence) {
  if (relation_uuid.empty() || relation_uuid == "local.default" ||
      request.context.local_transaction_id == 0 || request.context.database_path.empty()) {
    return std::nullopt;
  }
  const auto loaded = LoadQueryCrudCompatibilityState(request.context);
  if (!loaded.ok) return std::nullopt;
  for (const auto& index : VisibleCrudIndexesForTable(loaded.state,
                                                      relation_uuid,
                                                      request.context.local_transaction_id)) {
    const std::string family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
    if (family != kCrudIndexFamilyBtree && family != kCrudIndexFamilyCovering &&
        family != kCrudIndexFamilyInMemory && family != kCrudIndexFamilyReferenceEmulated) {
      continue;
    }
    if (!CrudIndexSupportsPredicate(index, predicate)) continue;
    if (index.unique) {
      if (evidence != nullptr) {
        evidence->push_back({"optimizer_secondary_index_delta_overlay",
                             "unique_synchronous_bypass"});
      }
      return index;
    }
    // DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP
    const auto lookup = IndexedMgaRowsForPredicateForContext(loaded.state,
                                                             relation_uuid,
                                                             predicate,
                                                             request.context,
                                                             1);
    if (lookup.index_refused) {
      if (evidence != nullptr) {
        evidence->push_back({"optimizer_access_path_unselectable",
                             "secondary_index_delta_overlay_refused"});
        evidence->insert(evidence->end(),
                         lookup.evidence.begin(),
                         lookup.evidence.end());
      }
      continue;
    }
    if (evidence != nullptr) {
      evidence->insert(evidence->end(),
                       lookup.evidence.begin(),
                       lookup.evidence.end());
    }
    return index;
  }
  return std::nullopt;
}

plan::PhysicalAccessKind PlannedAccessKindForRequest(const EnginePlanOperationRequest& request,
                                                     const std::string& operation,
                                                     const std::vector<EngineQueryRelation>& relations,
                                                     const opt::OptimizerStatisticsCatalog& statistics,
                                                     std::vector<EngineEvidenceReference>* evidence) {
  if (operation == "join" || operation == "inner_join" || operation == "equi_join" ||
      operation == "left_join" || operation == "left_outer_join" ||
      operation == "semi_join" || operation == "join_group_sum_assertion" ||
      operation == "join_window_max_assertion") {
    if (RequestOptionDisabled(request, "optimizer_join_costing:", "join_costing:")) {
      if (evidence != nullptr) {
        evidence->push_back({"optimizer_join_costing", "disabled"});
      }
      return plan::PhysicalAccessKind::kJoinNestedLoop;
    }
    return RequestOptionEnabled(request, "join_inputs_ordered:", "optimizer_join_ordered_inputs:")
               ? plan::PhysicalAccessKind::kJoinMerge
               : plan::PhysicalAccessKind::kJoinHash;
  }
  if (!(operation == "filter_gt" || operation == "scan" || operation == "index_lookup" ||
        operation == "count" || operation == "count_all")) {
    return AccessKindForQueryOperation(operation);
  }
  if (relations.empty()) return plan::PhysicalAccessKind::kTableScan;
  const auto& predicate = request.predicate;
  if (predicate.predicate_kind.empty()) return plan::PhysicalAccessKind::kTableScan;

  const std::string relation_uuid = RelationObjectUuid(relations.front());
  const bool row_stats_usable = StatisticUsable(statistics, "row_count", relation_uuid, request) &&
                                StatisticUsable(statistics, "page_count", relation_uuid, request);
  if (!row_stats_usable) {
    if (evidence != nullptr) {
      evidence->push_back({"optimizer_access_path_fallback", "stale_or_missing_relation_statistics_scan"});
    }
    return plan::PhysicalAccessKind::kTableScan;
  }

  const auto index = UsableCrudIndexForPredicate(request, relation_uuid, predicate, evidence);
  if (!index) {
    if (evidence != nullptr) {
      evidence->push_back({"optimizer_access_path_unselectable", "usable_scalar_btree_index_missing"});
    }
    return plan::PhysicalAccessKind::kTableScan;
  }
  if (!StatisticUsable(statistics, "index_depth", index->index_uuid, request) ||
      !StatisticUsable(statistics, "index_selectivity", index->index_uuid, request)) {
    if (evidence != nullptr) {
      evidence->push_back({"optimizer_access_path_fallback", "stale_or_missing_index_statistics_scan"});
    }
    return plan::PhysicalAccessKind::kTableScan;
  }
  if (RequestProjectionCovered(request, *index)) {
    if (evidence != nullptr) {
      evidence->push_back({"optimizer_access_path_index", index->index_uuid});
    }
    return plan::PhysicalAccessKind::kCoveringIndexScan;
  }
  if (PredicateCanUseScalarBtree(predicate)) {
    if (evidence != nullptr) {
      evidence->push_back({"optimizer_access_path_index", index->index_uuid});
    }
    return plan::PhysicalAccessKind::kScalarBtreeLookup;
  }
  if (PredicateCanUseScalarBtreeRange(predicate)) {
    if (evidence != nullptr) {
      evidence->push_back({"optimizer_access_path_index", index->index_uuid});
    }
    return plan::PhysicalAccessKind::kScalarBtreeRange;
  }
  if (evidence != nullptr) {
    evidence->push_back({"optimizer_access_path_unselectable", "predicate_not_supported_by_scalar_btree"});
  }
  return plan::PhysicalAccessKind::kTableScan;
}

EngineDescriptor Int64Descriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor = "canonical=int64";
  return descriptor;
}

EngineDescriptor Real64Descriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "real64";
  descriptor.encoded_descriptor = "canonical=real64";
  return descriptor;
}

EngineDescriptor BoolDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "boolean";
  descriptor.encoded_descriptor = "canonical=boolean";
  return descriptor;
}

EngineDescriptor TextDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "canonical=text";
  return descriptor;
}

EngineDescriptor JsonDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "json";
  descriptor.encoded_descriptor = "canonical=json";
  return descriptor;
}

EngineDescriptor ListDescriptor(const EngineDescriptor& element_descriptor) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "list";
  descriptor.canonical_type_name = "list";
  const std::string element_type = element_descriptor.canonical_type_name.empty()
                                       ? "any"
                                       : element_descriptor.canonical_type_name;
  descriptor.encoded_descriptor = "canonical=list;element=" + element_type +
                                  ";nulls=included;order=parser_provided";
  return descriptor;
}

std::string DescriptorFieldValue(const std::string& encoded, std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  for (const auto& part : Split(encoded, ';')) {
    if (part.rfind(prefix, 0) == 0) { return part.substr(prefix.size()); }
  }
  return {};
}

EngineDescriptor DescriptorFromCrudColumnDescriptor(const std::string& encoded) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.encoded_descriptor = encoded;
  descriptor.canonical_type_name = DescriptorFieldValue(encoded, "canonical");
  if (descriptor.canonical_type_name.empty()) {
    descriptor.canonical_type_name = DescriptorFieldValue(encoded, "type");
  }
  if (descriptor.canonical_type_name.empty()) {
    descriptor.canonical_type_name = DescriptorFieldValue(encoded, "base_type");
  }
  if (descriptor.canonical_type_name.empty() &&
      encoded.find('=') == std::string::npos &&
      encoded.find(';') == std::string::npos) {
    descriptor.canonical_type_name = encoded;
  }
  descriptor.canonical_type_name = LowerAscii(std::move(descriptor.canonical_type_name));
  return descriptor;
}

std::unordered_map<std::string, EngineDescriptor> CrudColumnDescriptorsByName(
    const CrudTableRecord& table) {
  std::unordered_map<std::string, EngineDescriptor> descriptors;
  descriptors.reserve(table.columns.size());
  for (const auto& [name, encoded] : table.columns) {
    if (!encoded.empty()) {
      descriptors.emplace(name, DescriptorFromCrudColumnDescriptor(encoded));
    }
  }
  return descriptors;
}

std::vector<EngineDescriptor> CrudRelationColumnDescriptors(const CrudTableRecord& table) {
  std::vector<EngineDescriptor> descriptors;
  descriptors.reserve(table.columns.size());
  for (const auto& [name, encoded] : table.columns) {
    (void)name;
    descriptors.push_back(DescriptorFromCrudColumnDescriptor(encoded));
  }
  return descriptors;
}

EngineTypedValue CrudRelationTypedValue(const std::string& value,
                                        const EngineDescriptor* descriptor) {
  EngineTypedValue typed;
  if (descriptor != nullptr) { typed.descriptor = *descriptor; }
  typed.is_null = value == "<NULL>";
  if (!typed.is_null) { typed.encoded_value = value; }
  return typed;
}

EngineTypedValue Int64Value(std::int64_t value) {
  EngineTypedValue typed;
  typed.descriptor = Int64Descriptor();
  typed.encoded_value = std::to_string(value);
  return typed;
}

std::string FormatReal64(double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

EngineTypedValue Real64Value(double value) {
  EngineTypedValue typed;
  typed.descriptor = Real64Descriptor();
  typed.encoded_value = FormatReal64(value);
  return typed;
}

EngineTypedValue BoolValue(bool value) {
  EngineTypedValue typed;
  typed.descriptor = BoolDescriptor();
  typed.encoded_value = value ? "true" : "false";
  return typed;
}

EngineTypedValue TextValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor = TextDescriptor();
  typed.encoded_value = std::move(value);
  return typed;
}

EngineTypedValue JsonValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor = JsonDescriptor();
  typed.encoded_value = std::move(value);
  return typed;
}

EngineTypedValue NullValue(EngineDescriptor descriptor) {
  EngineTypedValue typed;
  typed.descriptor = std::move(descriptor);
  typed.is_null = true;
  return typed;
}

EngineDescriptor InferredDescriptorForValue(const EngineTypedValue& typed) {
  if (!typed.descriptor.canonical_type_name.empty()) { return typed.descriptor; }
  if (!typed.is_null && TryParseI64Value(typed.encoded_value, nullptr)) { return Int64Descriptor(); }
  double ignored = 0.0;
  if (!typed.is_null && TryParseReal64Value(typed.encoded_value, &ignored)) { return Real64Descriptor(); }
  return TextDescriptor();
}

EngineTypedValue NormalizeTypedValue(const EngineTypedValue& typed) {
  EngineTypedValue normalized = typed;
  normalized.descriptor = InferredDescriptorForValue(typed);
  if (normalized.is_null) { normalized.encoded_value.clear(); }
  return normalized;
}

exec::Batch RelationToBatch(const EngineQueryRelation& relation) {
  std::vector<exec::Tuple> rows;
  rows.reserve(relation.rows.size());
  for (const auto& row : relation.rows) {
    exec::Tuple tuple;
    tuple.values.reserve(row.fields.size());
    for (const auto& field : row.fields) { tuple.values.push_back(ParseI64Value(field.second.encoded_value)); }
    rows.push_back(std::move(tuple));
  }
  return exec::MakeBatch(relation.descriptor_digest.empty() ? relation.relation_name : relation.descriptor_digest,
                         std::move(rows));
}

bool WindowFunctionRequiresTypedResult(const std::string& function) {
  return function == "percent_rank" || function == "cume_dist" || function == "nth_value";
}

std::string AggregateFunctionLeaf(std::string function) {
  function = LowerAscii(std::move(function));
  constexpr std::string_view kSbPrefix = "sb.aggregate.";
  constexpr std::string_view kDataPrefix = "data.aggregate.";
  constexpr std::string_view kGenericPrefix = "aggregate.";
  if (function.starts_with(kSbPrefix)) function = function.substr(kSbPrefix.size());
  if (function.starts_with(kDataPrefix)) function = function.substr(kDataPrefix.size());
  if (function.starts_with(kGenericPrefix)) function = function.substr(kGenericPrefix.size());
  std::replace(function.begin(), function.end(), '-', '_');
  if (function == "count(distinct)" || function == "count_distinct_exact") return "count_distinct";
  return function;
}

bool StatisticalAggregateRequiresTypedResult(const std::string& function) {
  const std::string leaf = AggregateFunctionLeaf(function);
  return leaf == "stddev" || leaf == "stddev_samp" || leaf == "stddev_pop" ||
         leaf == "variance" || leaf == "variance_samp" || leaf == "variance_pop";
}

bool CoreAggregateRequiresTypedResult(const std::string& function) {
  const std::string leaf = AggregateFunctionLeaf(function);
  return leaf == "count" || leaf == "count_distinct" ||
         leaf == "sum" || leaf == "avg" || leaf == "min" || leaf == "max";
}

bool BooleanAggregateRequiresTypedResult(const std::string& function) {
  const std::string leaf = AggregateFunctionLeaf(function);
  return leaf == "every" || leaf == "bool_and" || leaf == "bool_or";
}

bool ApproxAggregateRequiresTypedResult(const std::string& function) {
  const std::string leaf = AggregateFunctionLeaf(function);
  return leaf == "approx_count_distinct" || leaf == "approx_median";
}

bool PairAggregateRequiresTypedResult(const std::string& function) {
  const std::string leaf = AggregateFunctionLeaf(function);
  return leaf == "corr" || leaf == "covar_pop" ||
         leaf == "covar_samp" || leaf == "regr_avgx" ||
         leaf == "regr_avgy" || leaf == "regr_count" ||
         leaf == "regr_intercept" || leaf == "regr_r2" ||
         leaf == "regr_slope" || leaf == "regr_sxx" ||
         leaf == "regr_sxy" || leaf == "regr_syy";
}

bool DistributionAggregateRequiresTypedResult(const std::string& function) {
  const std::string leaf = AggregateFunctionLeaf(function);
  return leaf == "mode" || leaf == "approx_top_k" ||
         leaf == "percentile_cont" || leaf == "percentile_disc" ||
         leaf == "approx_percentile_cont" || leaf == "approx_percentile_disc";
}

bool ListAggAggregateRequiresTypedResult(const std::string& function) {
  const std::string leaf = AggregateFunctionLeaf(function);
  return leaf == "listagg" || leaf == "string_agg";
}

bool JsonAggregateRequiresTypedResult(const std::string& function) {
  const std::string leaf = AggregateFunctionLeaf(function);
  return leaf == "json_agg" || leaf == "json_object_agg";
}

bool ArrayAggregateRequiresTypedResult(const std::string& function) {
  return AggregateFunctionLeaf(function) == "array_agg";
}

std::optional<std::int64_t> RowInt64ValueAt(const EngineRowValue& row,
                                            std::size_t column,
                                            std::string* error_detail) {
  if (column >= row.fields.size()) {
    if (error_detail != nullptr) *error_detail = "query_plan_window_column_out_of_range";
    return std::nullopt;
  }
  const auto typed = NormalizeTypedValue(row.fields[column].second);
  if (typed.is_null) {
    if (error_detail != nullptr) *error_detail = "query_plan_window_order_requires_non_null_int64";
    return std::nullopt;
  }
  std::int64_t parsed = 0;
  if (!TryParseI64Value(typed.encoded_value, &parsed)) {
    if (error_detail != nullptr) *error_detail = "query_plan_window_order_requires_int64";
    return std::nullopt;
  }
  return parsed;
}

std::optional<std::int64_t> RowAggregateGroupKeyInt64ValueAt(const EngineRowValue& row,
                                                            std::size_t column,
                                                            std::string* error_detail) {
  if (column >= row.fields.size()) {
    if (error_detail != nullptr) *error_detail = "query_plan_aggregate_group_key_column_out_of_range";
    return std::nullopt;
  }
  const auto typed = NormalizeTypedValue(row.fields[column].second);
  if (typed.is_null) {
    if (error_detail != nullptr) *error_detail = "query_plan_aggregate_group_key_requires_non_null_int64";
    return std::nullopt;
  }
  std::int64_t parsed = 0;
  if (!TryParseI64Value(typed.encoded_value, &parsed)) {
    if (error_detail != nullptr) *error_detail = "query_plan_aggregate_group_key_requires_int64";
    return std::nullopt;
  }
  return parsed;
}

EngineResultShape TypedWindowResultShape(const EngineQueryRelation& relation,
                                         const std::string& function,
                                         std::size_t order_column,
                                         std::size_t value_column,
                                         std::uint64_t window_n,
                                         std::string* error_detail) {
  struct IndexedRow {
    EngineRowValue row;
    std::int64_t order_value = 0;
  };

  std::vector<IndexedRow> rows;
  rows.reserve(relation.rows.size());
  for (const auto& row : relation.rows) {
    const auto order_value = RowInt64ValueAt(row, order_column, error_detail);
    if (!order_value) { return {}; }
    rows.push_back({row, *order_value});
  }
  std::stable_sort(rows.begin(), rows.end(), [](const IndexedRow& lhs, const IndexedRow& rhs) {
    return lhs.order_value < rhs.order_value;
  });

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  std::size_t input_width = 0;
  for (const auto& row : rows) { input_width = std::max(input_width, row.row.fields.size()); }
  for (std::size_t column = 0; column < input_width; ++column) {
    EngineDescriptor descriptor = TextDescriptor();
    for (const auto& row : rows) {
      if (column < row.row.fields.size()) {
        descriptor = InferredDescriptorForValue(row.row.fields[column].second);
        break;
      }
    }
    shape.columns.push_back(descriptor);
  }
  EngineDescriptor window_descriptor = Real64Descriptor();
  if (function == "nth_value") {
    if (window_n == 0) {
      if (error_detail != nullptr) *error_detail = "query_plan_window_nth_value_requires_positive_n";
      return {};
    }
    if (value_column >= input_width) {
      if (error_detail != nullptr) *error_detail = "query_plan_window_value_column_out_of_range";
      return {};
    }
    window_descriptor = shape.columns[value_column];
  }
  shape.columns.push_back(window_descriptor);

  const std::size_t row_count = rows.size();
  for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
    EngineRowValue out;
    out.requested_row_uuid = rows[row_index].row.requested_row_uuid;
    for (std::size_t column = 0; column < input_width; ++column) {
      EngineTypedValue value = column < rows[row_index].row.fields.size()
                                   ? NormalizeTypedValue(rows[row_index].row.fields[column].second)
                                   : NullValue(shape.columns[column]);
      out.fields.push_back({"c" + std::to_string(column), std::move(value)});
    }

    if (function == "percent_rank") {
      std::size_t rank_index = row_index;
      while (rank_index > 0 && rows[rank_index - 1].order_value == rows[row_index].order_value) {
        --rank_index;
      }
      const double value = row_count <= 1
          ? 0.0
          : static_cast<double>(rank_index) / static_cast<double>(row_count - 1);
      out.fields.push_back({"c" + std::to_string(input_width), Real64Value(value)});
    } else if (function == "cume_dist") {
      std::size_t peer_end = row_index + 1;
      while (peer_end < row_count && rows[peer_end].order_value == rows[row_index].order_value) {
        ++peer_end;
      }
      const double value = row_count == 0
          ? 0.0
          : static_cast<double>(peer_end) / static_cast<double>(row_count);
      out.fields.push_back({"c" + std::to_string(input_width), Real64Value(value)});
    } else {
      const std::size_t requested_index = static_cast<std::size_t>(window_n - 1);
      if (requested_index > row_index || requested_index >= row_count ||
          value_column >= rows[requested_index].row.fields.size()) {
        out.fields.push_back({"c" + std::to_string(input_width), NullValue(window_descriptor)});
      } else {
        EngineTypedValue selected =
            NormalizeTypedValue(rows[requested_index].row.fields[value_column].second);
        if (selected.descriptor.canonical_type_name.empty()) { selected.descriptor = window_descriptor; }
        out.fields.push_back({"c" + std::to_string(input_width), std::move(selected)});
      }
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape StatisticalAggregateResultShape(const EngineQueryRelation& relation,
                                                  const std::string& function,
                                                  std::size_t group_key_column,
                                                  std::size_t value_column,
                                                  std::string* error_detail) {
  struct Stats {
    std::uint64_t count = 0;
    long double mean = 0.0L;
    long double m2 = 0.0L;
  };

  std::map<std::int64_t, Stats> groups;
  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    auto& stats = groups[*key];
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    double parsed = 0.0;
    if (!TryParseReal64Value(typed.encoded_value, &parsed)) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_numeric_input_required";
      return {};
    }
    ++stats.count;
    const long double value = static_cast<long double>(parsed);
    const long double delta = value - stats.mean;
    stats.mean += delta / static_cast<long double>(stats.count);
    const long double delta2 = value - stats.mean;
    stats.m2 += delta * delta2;
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(Real64Descriptor());
  const std::string leaf = AggregateFunctionLeaf(function);
  const bool stddev = leaf == "stddev" || leaf == "stddev_samp" || leaf == "stddev_pop";
  const bool population = leaf == "stddev_pop" || leaf == "variance_pop";
  for (const auto& [key, stats] : groups) {
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (stats.count == 0 || (!population && stats.count < 2)) {
      out.fields.push_back({"c1", NullValue(Real64Descriptor())});
    } else {
      const long double denominator = population
          ? static_cast<long double>(stats.count)
          : static_cast<long double>(stats.count - 1);
      const long double variance = stats.m2 / denominator;
      const double value = stddev
          ? std::sqrt(static_cast<double>(variance))
          : static_cast<double>(variance);
      out.fields.push_back({"c1", Real64Value(value)});
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape CoreAggregateResultShape(const EngineQueryRelation& relation,
                                           const std::string& function,
                                           std::size_t group_key_column,
                                           std::size_t value_column,
                                           std::string* error_detail) {
  struct CoreStats {
    std::uint64_t input_count = 0;
    std::uint64_t non_null_count = 0;
    long double sum = 0.0L;
    EngineTypedValue min_value;
    EngineTypedValue max_value;
    std::set<std::string> distinct_values;
  };

  std::map<std::int64_t, CoreStats> groups;
  const std::string leaf = AggregateFunctionLeaf(function);
  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    auto& stats = groups[*key];
    ++stats.input_count;
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    ++stats.non_null_count;
    if (leaf == "count_distinct") {
      stats.distinct_values.insert(typed.descriptor.canonical_type_name + ":" + typed.encoded_value);
    } else if (leaf == "sum" || leaf == "avg") {
      double parsed = 0.0;
      if (!TryParseReal64Value(typed.encoded_value, &parsed)) {
        if (error_detail != nullptr) *error_detail = "query_plan_aggregate_numeric_input_required";
        return {};
      }
      stats.sum += static_cast<long double>(parsed);
    } else if (leaf == "min" || leaf == "max") {
      if (stats.non_null_count == 1) {
        stats.min_value = typed;
        stats.max_value = typed;
      } else {
        const bool numeric_candidate =
            TryParseReal64Value(typed.encoded_value, nullptr) &&
            TryParseReal64Value(stats.min_value.encoded_value, nullptr) &&
            TryParseReal64Value(stats.max_value.encoded_value, nullptr);
        if (numeric_candidate) {
          const double candidate = ParseReal64Value(typed.encoded_value, 0.0);
          const double current_min = ParseReal64Value(stats.min_value.encoded_value, 0.0);
          const double current_max = ParseReal64Value(stats.max_value.encoded_value, 0.0);
          if (candidate < current_min) stats.min_value = typed;
          if (candidate > current_max) stats.max_value = typed;
        } else {
          if (typed.encoded_value < stats.min_value.encoded_value) stats.min_value = typed;
          if (typed.encoded_value > stats.max_value.encoded_value) stats.max_value = typed;
        }
      }
    }
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back((leaf == "count" || leaf == "count_distinct") ? Int64Descriptor() : Real64Descriptor());
  for (const auto& [key, stats] : groups) {
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (leaf == "count") {
      out.fields.push_back({"c1", Int64Value(static_cast<std::int64_t>(stats.input_count))});
    } else if (leaf == "count_distinct") {
      out.fields.push_back({"c1", Int64Value(static_cast<std::int64_t>(stats.distinct_values.size()))});
    } else if (leaf == "sum") {
      if (stats.non_null_count == 0) {
        out.fields.push_back({"c1", NullValue(Real64Descriptor())});
      } else {
        out.fields.push_back({"c1", Real64Value(static_cast<double>(stats.sum))});
      }
    } else if (leaf == "avg") {
      if (stats.non_null_count == 0) {
        out.fields.push_back({"c1", NullValue(Real64Descriptor())});
      } else {
        out.fields.push_back({"c1", Real64Value(static_cast<double>(
                                      stats.sum / static_cast<long double>(stats.non_null_count)))});
      }
    } else if (stats.non_null_count == 0) {
      out.fields.push_back({"c1", NullValue(Real64Descriptor())});
    } else {
      const auto selected = leaf == "min" ? stats.min_value : stats.max_value;
      double parsed = 0.0;
      if (TryParseReal64Value(selected.encoded_value, &parsed)) {
        out.fields.push_back({"c1", Real64Value(parsed)});
      } else {
        out.fields.push_back({"c1", selected});
      }
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape EveryAggregateResultShape(const EngineQueryRelation& relation,
                                            const std::string& function,
                                            std::size_t group_key_column,
                                            std::size_t value_column,
                                            std::string* error_detail) {
  struct BoolStats {
    std::uint64_t count = 0;
    bool saw_false = false;
    bool saw_true = false;
  };

  std::map<std::int64_t, BoolStats> groups;
  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    auto& stats = groups[*key];
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    const std::string value = LowerAscii(typed.encoded_value);
    bool truth = false;
    if (value == "true" || value == "1") {
      truth = true;
    } else if (value == "false" || value == "0") {
      truth = false;
    } else {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_boolean_input_required";
      return {};
    }
    ++stats.count;
    if (truth) stats.saw_true = true;
    if (!truth) stats.saw_false = true;
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(BoolDescriptor());
  for (const auto& [key, stats] : groups) {
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (stats.count == 0) {
      out.fields.push_back({"c1", NullValue(BoolDescriptor())});
    } else if (AggregateFunctionLeaf(function) == "bool_or") {
      out.fields.push_back({"c1", BoolValue(stats.saw_true)});
    } else {
      out.fields.push_back({"c1", BoolValue(!stats.saw_false)});
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape ApproxCountDistinctAggregateResultShape(const EngineQueryRelation& relation,
                                                          std::size_t group_key_column,
                                                          std::size_t value_column,
                                                          std::string* error_detail) {
  std::map<std::int64_t, std::set<std::string>> groups;
  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    auto& values = groups[*key];
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    values.insert(typed.descriptor.canonical_type_name + ":" + typed.encoded_value);
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(Int64Descriptor());
  for (const auto& [key, values] : groups) {
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    out.fields.push_back({"c1", Int64Value(static_cast<std::int64_t>(values.size()))});
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape ApproxMedianAggregateResultShape(const EngineQueryRelation& relation,
                                                   std::size_t group_key_column,
                                                   std::size_t value_column,
                                                   std::string* error_detail) {
  std::map<std::int64_t, std::vector<double>> groups;
  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    auto& values = groups[*key];
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    double parsed = 0.0;
    if (!TryParseReal64Value(typed.encoded_value, &parsed)) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_numeric_input_required";
      return {};
    }
    values.push_back(parsed);
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(Real64Descriptor());
  for (auto& [key, values] : groups) {
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (values.empty()) {
      out.fields.push_back({"c1", NullValue(Real64Descriptor())});
    } else {
      std::sort(values.begin(), values.end());
      const std::size_t mid = values.size() / 2;
      const double median = values.size() % 2 == 1
          ? values[mid]
          : (values[mid - 1] + values[mid]) / 2.0;
      out.fields.push_back({"c1", Real64Value(median)});
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape PairAggregateResultShape(const EngineQueryRelation& relation,
                                           const std::string& function,
                                           std::size_t group_key_column,
                                           std::size_t y_column,
                                           std::size_t x_column,
                                           std::string* error_detail) {
  struct PairStats {
    std::uint64_t count = 0;
    long double sum_x = 0.0L;
    long double sum_y = 0.0L;
    long double sum_xx = 0.0L;
    long double sum_yy = 0.0L;
    long double sum_xy = 0.0L;
  };

  std::map<std::int64_t, PairStats> groups;
  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    if (y_column >= row.fields.size() || x_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_pair_value_column_out_of_range";
      return {};
    }
    auto& stats = groups[*key];
    const auto y_typed = NormalizeTypedValue(row.fields[y_column].second);
    const auto x_typed = NormalizeTypedValue(row.fields[x_column].second);
    if (y_typed.is_null || x_typed.is_null) continue;
    double y = 0.0;
    double x = 0.0;
    if (!TryParseReal64Value(y_typed.encoded_value, &y) ||
        !TryParseReal64Value(x_typed.encoded_value, &x)) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_numeric_input_required";
      return {};
    }
    ++stats.count;
    stats.sum_x += static_cast<long double>(x);
    stats.sum_y += static_cast<long double>(y);
    stats.sum_xx += static_cast<long double>(x) * static_cast<long double>(x);
    stats.sum_yy += static_cast<long double>(y) * static_cast<long double>(y);
    stats.sum_xy += static_cast<long double>(x) * static_cast<long double>(y);
  }

  const std::string leaf = AggregateFunctionLeaf(function);
  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(leaf == "regr_count" ? Int64Descriptor() : Real64Descriptor());
  for (const auto& [key, stats] : groups) {
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (leaf == "regr_count") {
      out.fields.push_back({"c1", Int64Value(static_cast<std::int64_t>(stats.count))});
      shape.rows.push_back(std::move(out));
      continue;
    }
    if (stats.count == 0 || (leaf == "covar_samp" && stats.count < 2)) {
      out.fields.push_back({"c1", NullValue(Real64Descriptor())});
      shape.rows.push_back(std::move(out));
      continue;
    }
    const long double n = static_cast<long double>(stats.count);
    const long double sxx = stats.sum_xx - (stats.sum_x * stats.sum_x / n);
    const long double syy = stats.sum_yy - (stats.sum_y * stats.sum_y / n);
    const long double sxy = stats.sum_xy - (stats.sum_x * stats.sum_y / n);
    if (leaf == "corr") {
      if (sxx == 0.0L || syy == 0.0L) {
        out.fields.push_back({"c1", NullValue(Real64Descriptor())});
      } else {
        out.fields.push_back({"c1", Real64Value(static_cast<double>(sxy / std::sqrt(sxx * syy)))});
      }
    } else if (leaf == "covar_pop") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>(sxy / n))});
    } else if (leaf == "covar_samp") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>(sxy / (n - 1.0L)))});
    } else if (leaf == "regr_avgx") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>(stats.sum_x / n))});
    } else if (leaf == "regr_avgy") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>(stats.sum_y / n))});
    } else if (leaf == "regr_sxx") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>(sxx))});
    } else if (leaf == "regr_sxy") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>(sxy))});
    } else if (leaf == "regr_syy") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>(syy))});
    } else if (sxx == 0.0L) {
      out.fields.push_back({"c1", NullValue(Real64Descriptor())});
    } else if (leaf == "regr_r2") {
      if (syy == 0.0L) {
        out.fields.push_back({"c1", Real64Value(1.0)});
      } else {
        out.fields.push_back({"c1", Real64Value(static_cast<double>((sxy * sxy) / (sxx * syy)))});
      }
    } else if (leaf == "regr_slope") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>(sxy / sxx))});
    } else if (leaf == "regr_intercept") {
      out.fields.push_back({"c1", Real64Value(static_cast<double>((stats.sum_y - stats.sum_x * sxy / sxx) / n))});
    } else {
      out.fields.push_back({"c1", NullValue(Real64Descriptor())});
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

std::string JsonEscape(std::string_view value) {
  std::string out;
  for (const char ch : value) {
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string JsonArrayElementFromValue(const EngineTypedValue& typed) {
  if (typed.is_null) return "null";
  const std::string descriptor = LowerAscii(typed.descriptor.canonical_type_name);
  if (descriptor == "boolean" || descriptor == "bool") {
    const std::string value = LowerAscii(typed.encoded_value);
    if (value == "true" || value == "1") return "true";
    if (value == "false" || value == "0") return "false";
    return "\"" + JsonEscape(typed.encoded_value) + "\"";
  }
  if (descriptor == "int64" || descriptor == "uint64" ||
      descriptor == "integer" || descriptor == "bigint") {
    std::int64_t ignored = 0;
    if (TryParseI64Value(typed.encoded_value, &ignored)) return typed.encoded_value;
  }
  if (descriptor == "real64" || descriptor == "double" ||
      descriptor == "numeric" || descriptor == "decimal") {
    double ignored = 0.0;
    if (TryParseReal64Value(typed.encoded_value, &ignored) && std::isfinite(ignored)) {
      return typed.encoded_value;
    }
  }
  if (descriptor == "json" || descriptor == "json_document") {
    return typed.encoded_value.empty() ? "null" : typed.encoded_value;
  }
  return "\"" + JsonEscape(typed.encoded_value) + "\"";
}

EngineResultShape DistributionAggregateResultShape(const EngineQueryRelation& relation,
                                                   const std::string& function,
                                                   std::size_t group_key_column,
                                                   std::size_t value_column,
                                                   double fraction,
                                                   std::size_t limit,
                                                   std::string* error_detail) {
  const std::string leaf = AggregateFunctionLeaf(function);
  if (fraction < 0.0 || fraction > 1.0 || !std::isfinite(fraction)) {
    if (error_detail != nullptr) *error_detail = "query_plan_aggregate_fraction_out_of_range";
    return {};
  }
  struct GroupValues {
    std::vector<double> numeric;
    std::map<std::string, std::uint64_t> frequencies;
  };

  std::map<std::int64_t, GroupValues> groups;
  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    auto& values = groups[*key];
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    if (leaf == "approx_top_k") {
      const std::string key_value = typed.descriptor.canonical_type_name + ":" + typed.encoded_value;
      ++values.frequencies[key_value];
      continue;
    }
    double parsed = 0.0;
    if (!TryParseReal64Value(typed.encoded_value, &parsed)) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_numeric_input_required";
      return {};
    }
    values.numeric.push_back(parsed);
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(leaf == "approx_top_k" ? JsonDescriptor() : Real64Descriptor());
  for (auto& [key, values] : groups) {
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (leaf == "approx_top_k") {
      if (values.frequencies.empty()) {
        out.fields.push_back({"c1", NullValue(JsonDescriptor())});
      } else {
        std::vector<std::pair<std::string, std::uint64_t>> ranked(values.frequencies.begin(),
                                                                  values.frequencies.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
          if (lhs.second != rhs.second) return lhs.second > rhs.second;
          return lhs.first < rhs.first;
        });
        const std::size_t capped = std::min<std::size_t>(limit == 0 ? 10 : limit, ranked.size());
        std::ostringstream json;
        json << '[';
        for (std::size_t index = 0; index < capped; ++index) {
          if (index != 0) json << ',';
          const auto separator = ranked[index].first.find(':');
          const std::string encoded_value =
              separator == std::string::npos ? ranked[index].first : ranked[index].first.substr(separator + 1);
          json << "{\"value\":\"" << JsonEscape(encoded_value) << "\",\"count\":"
               << ranked[index].second << '}';
        }
        json << ']';
        out.fields.push_back({"c1", JsonValue(json.str())});
      }
      shape.rows.push_back(std::move(out));
      continue;
    }
    if (values.numeric.empty()) {
      out.fields.push_back({"c1", NullValue(Real64Descriptor())});
      shape.rows.push_back(std::move(out));
      continue;
    }
    std::sort(values.numeric.begin(), values.numeric.end());
    if (leaf == "mode") {
      double best_value = values.numeric.front();
      std::uint64_t best_count = 0;
      for (std::size_t index = 0; index < values.numeric.size();) {
        std::size_t next = index + 1;
        while (next < values.numeric.size() && values.numeric[next] == values.numeric[index]) ++next;
        const auto count = static_cast<std::uint64_t>(next - index);
        if (count > best_count) {
          best_count = count;
          best_value = values.numeric[index];
        }
        index = next;
      }
      out.fields.push_back({"c1", Real64Value(best_value)});
    } else if (leaf == "percentile_disc" || leaf == "approx_percentile_disc") {
      const double scaled = fraction * static_cast<double>(values.numeric.size());
      const std::size_t index = scaled <= 0.0
          ? 0
          : std::min<std::size_t>(values.numeric.size() - 1,
                                  static_cast<std::size_t>(std::ceil(scaled)) - 1);
      out.fields.push_back({"c1", Real64Value(values.numeric[index])});
    } else {
      const double scaled = fraction * static_cast<double>(values.numeric.size() - 1);
      const auto lower = static_cast<std::size_t>(std::floor(scaled));
      const auto upper = static_cast<std::size_t>(std::ceil(scaled));
      const double weight = scaled - static_cast<double>(lower);
      const double value = values.numeric[lower] +
                           ((values.numeric[upper] - values.numeric[lower]) * weight);
      out.fields.push_back({"c1", Real64Value(value)});
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape JsonAggAggregateResultShape(const EngineQueryRelation& relation,
                                              std::size_t group_key_column,
                                              std::size_t value_column,
                                              std::size_t order_column,
                                              std::string* error_detail) {
  struct OrderedValue {
    std::int64_t order_value = 0;
    EngineTypedValue value;
  };
  std::map<std::int64_t, std::vector<OrderedValue>> groups;

  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    auto& values = groups[*key];
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    if (order_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_json_agg_order_column_out_of_range";
      return {};
    }
    const auto order = RowInt64ValueAt(row, order_column, error_detail);
    if (!order) {
      if (error_detail != nullptr && error_detail->find("query_plan_window_") == 0) {
        *error_detail = "query_plan_json_agg_order_requires_int64";
      }
      return {};
    }
    values.push_back(OrderedValue{*order, NormalizeTypedValue(row.fields[value_column].second)});
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(JsonDescriptor());
  for (auto& [key, values] : groups) {
    std::stable_sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.order_value < rhs.order_value;
    });
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (values.empty()) {
      out.fields.push_back({"c1", NullValue(JsonDescriptor())});
      shape.rows.push_back(std::move(out));
      continue;
    }
    std::ostringstream json;
    json << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index != 0) json << ',';
      json << JsonArrayElementFromValue(values[index].value);
    }
    json << ']';
    out.fields.push_back({"c1", JsonValue(json.str())});
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape JsonObjectAggAggregateResultShape(const EngineQueryRelation& relation,
                                                    std::size_t group_key_column,
                                                    std::size_t key_column,
                                                    std::size_t value_column,
                                                    std::size_t order_column,
                                                    std::string* error_detail) {
  struct OrderedPair {
    std::int64_t order_value = 0;
    EngineTypedValue key;
    EngineTypedValue value;
  };
  std::map<std::int64_t, std::vector<OrderedPair>> groups;

  for (const auto& row : relation.rows) {
    const auto group_key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!group_key) return {};
    auto& values = groups[*group_key];
    if (key_column >= row.fields.size() || value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    if (order_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_json_object_agg_order_column_out_of_range";
      return {};
    }
    EngineTypedValue key = NormalizeTypedValue(row.fields[key_column].second);
    if (key.is_null) {
      if (error_detail != nullptr) *error_detail = "query_plan_json_object_agg_key_required";
      return {};
    }
    const auto order = RowInt64ValueAt(row, order_column, error_detail);
    if (!order) {
      if (error_detail != nullptr && error_detail->find("query_plan_window_") == 0) {
        *error_detail = "query_plan_json_object_agg_order_requires_int64";
      }
      return {};
    }
    values.push_back(OrderedPair{*order,
                                 std::move(key),
                                 NormalizeTypedValue(row.fields[value_column].second)});
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(JsonDescriptor());
  for (auto& [key, values] : groups) {
    std::stable_sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.order_value < rhs.order_value;
    });
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (values.empty()) {
      out.fields.push_back({"c1", NullValue(JsonDescriptor())});
      shape.rows.push_back(std::move(out));
      continue;
    }

    std::vector<std::pair<std::string, EngineTypedValue>> object_items;
    for (auto& value : values) {
      const std::string object_key = value.key.encoded_value;
      const auto duplicate = std::find_if(object_items.begin(),
                                          object_items.end(),
                                          [&](const auto& item) {
                                            return item.first == object_key;
                                          });
      if (duplicate != object_items.end()) {
        object_items.erase(duplicate);
      }
      object_items.push_back({object_key, std::move(value.value)});
    }

    std::ostringstream json;
    json << '{';
    for (std::size_t index = 0; index < object_items.size(); ++index) {
      if (index != 0) json << ',';
      json << '"' << JsonEscape(object_items[index].first) << "\":"
           << JsonArrayElementFromValue(object_items[index].second);
    }
    json << '}';
    out.fields.push_back({"c1", JsonValue(json.str())});
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

std::string ListElementFromTypedValue(const EngineTypedValue& typed) {
  if (typed.is_null) return "NULL";
  const std::string descriptor = typed.descriptor.canonical_type_name.empty()
                                     ? "any"
                                     : typed.descriptor.canonical_type_name;
  return descriptor + ":" + typed.encoded_value;
}

EngineResultShape ArrayAggAggregateResultShape(const EngineQueryRelation& relation,
                                               std::size_t group_key_column,
                                               std::size_t value_column,
                                               std::size_t order_column,
                                               std::string* error_detail) {
  struct OrderedValue {
    std::int64_t order_value = 0;
    EngineTypedValue value;
  };
  std::map<std::int64_t, std::vector<OrderedValue>> groups;
  EngineDescriptor element_descriptor = TextDescriptor();
  bool element_descriptor_set = false;

  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    auto& values = groups[*key];
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    if (order_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_array_agg_order_column_out_of_range";
      return {};
    }
    const auto order = RowInt64ValueAt(row, order_column, error_detail);
    if (!order) {
      if (error_detail != nullptr && error_detail->find("query_plan_window_") == 0) {
        *error_detail = "query_plan_array_agg_order_requires_int64";
      }
      return {};
    }
    EngineTypedValue typed = NormalizeTypedValue(row.fields[value_column].second);
    if (!element_descriptor_set && !typed.descriptor.canonical_type_name.empty()) {
      element_descriptor = typed.descriptor;
      element_descriptor_set = true;
    }
    values.push_back(OrderedValue{*order, std::move(typed)});
  }

  const EngineDescriptor list_descriptor = ListDescriptor(element_descriptor);
  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(list_descriptor);
  for (auto& [key, values] : groups) {
    std::stable_sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.order_value < rhs.order_value;
    });
    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (values.empty()) {
      out.fields.push_back({"c1", NullValue(list_descriptor)});
      shape.rows.push_back(std::move(out));
      continue;
    }
    std::ostringstream encoded;
    encoded << "list[";
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index != 0) encoded << ';';
      encoded << ListElementFromTypedValue(values[index].value);
    }
    encoded << "]";
    EngineTypedValue list_value;
    list_value.descriptor = list_descriptor;
    list_value.encoded_value = encoded.str();
    out.fields.push_back({"c1", std::move(list_value)});
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

std::string JoinTextItems(const std::vector<std::string>& items,
                          std::size_t count,
                          const std::string& separator) {
  std::string out;
  const std::size_t bounded_count = std::min(count, items.size());
  for (std::size_t index = 0; index < bounded_count; ++index) {
    if (index != 0) out += separator;
    out += items[index];
  }
  return out;
}

std::string ListAggTruncationSuffix(const std::string& indicator,
                                    bool with_count,
                                    std::uint64_t truncated_count) {
  std::string suffix = indicator.empty() ? "..." : indicator;
  if (with_count) {
    suffix += "(";
    suffix += std::to_string(truncated_count);
    suffix += ")";
  }
  return suffix;
}

std::optional<std::string> ApplyListAggOverflow(const std::vector<std::string>& values,
                                                const std::string& separator,
                                                const std::string& full_text,
                                                const std::string& overflow_mode,
                                                std::size_t max_output_bytes,
                                                const std::string& indicator,
                                                bool with_count,
                                                std::string* error_detail) {
  if (overflow_mode.empty() || overflow_mode == "none" || max_output_bytes == 0 ||
      full_text.size() <= max_output_bytes) {
    return full_text;
  }
  if (overflow_mode == "error") {
    if (error_detail != nullptr) *error_detail = "query_plan_listagg_overflow";
    return std::nullopt;
  }
  if (overflow_mode != "truncate") return full_text;

  for (std::size_t retained = values.size(); retained > 0; --retained) {
    const auto truncated_count = static_cast<std::uint64_t>(values.size() - retained);
    if (truncated_count == 0) continue;
    std::string candidate = JoinTextItems(values, retained, separator);
    candidate += separator;
    candidate += ListAggTruncationSuffix(indicator, with_count, truncated_count);
    if (candidate.size() <= max_output_bytes) return candidate;
  }

  const auto truncated_count = static_cast<std::uint64_t>(values.size());
  std::string suffix_only = ListAggTruncationSuffix(indicator, with_count, truncated_count);
  if (suffix_only.size() <= max_output_bytes) return suffix_only;
  if (error_detail != nullptr) *error_detail = "query_plan_listagg_truncation_indicator_too_large";
  return std::nullopt;
}

EngineResultShape ListAggAggregateResultShape(const EngineQueryRelation& relation,
                                              std::size_t group_key_column,
                                              std::size_t value_column,
                                              std::size_t order_column,
                                              const std::string& separator,
                                              const std::string& overflow_mode,
                                              std::size_t max_output_bytes,
                                              const std::string& indicator,
                                              bool with_count,
                                              std::string* error_detail) {
  struct OrderedValue {
    std::int64_t order_value = 0;
    std::string text;
  };
  std::map<std::int64_t, std::vector<OrderedValue>> groups;

  for (const auto& row : relation.rows) {
    const auto key = RowAggregateGroupKeyInt64ValueAt(row, group_key_column, error_detail);
    if (!key) return {};
    auto& values = groups[*key];
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_value_column_out_of_range";
      return {};
    }
    if (order_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_listagg_order_column_out_of_range";
      return {};
    }
    const auto order = RowInt64ValueAt(row, order_column, error_detail);
    if (!order) {
      if (error_detail != nullptr && error_detail->find("query_plan_window_") == 0) {
        *error_detail = "query_plan_listagg_order_requires_int64";
      }
      return {};
    }
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    values.push_back(OrderedValue{*order, typed.encoded_value});
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(TextDescriptor());
  for (auto& [key, values] : groups) {
    std::stable_sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.order_value < rhs.order_value;
    });
    std::vector<std::string> text_values;
    text_values.reserve(values.size());
    for (const auto& value : values) text_values.push_back(value.text);

    EngineRowValue out;
    out.fields.push_back({"c0", Int64Value(key)});
    if (text_values.empty()) {
      out.fields.push_back({"c1", NullValue(TextDescriptor())});
      shape.rows.push_back(std::move(out));
      continue;
    }
    const std::string full_text = JoinTextItems(text_values, text_values.size(), separator);
    const auto final_text = ApplyListAggOverflow(text_values,
                                                 separator,
                                                 full_text,
                                                 overflow_mode,
                                                 max_output_bytes,
                                                 indicator,
                                                 with_count,
                                                 error_detail);
    if (!final_text) return {};
    out.fields.push_back({"c1", TextValue(*final_text)});
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

std::size_t ColumnIndexForRelation(const EngineQueryRelation& relation,
                                   const std::string& field_name,
                                   std::size_t fallback) {
  if (field_name.empty() || relation.rows.empty()) return fallback;
  const std::string wanted = LowerAscii(field_name);
  const auto& fields = relation.rows.front().fields;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (LowerAscii(fields[index].first) == wanted) return index;
  }
  return fallback;
}

std::vector<std::string> FieldOrderForRelation(const EngineQueryRelation& relation) {
  std::vector<std::string> fields;
  if (relation.rows.empty()) return fields;
  fields.reserve(relation.rows.front().fields.size());
  for (const auto& field : relation.rows.front().fields) {
    fields.push_back(LowerAscii(field.first));
  }
  return fields;
}

std::optional<std::int64_t> RowFieldValueByName(const EngineRowValue& row,
                                                const std::string& field_name) {
  const std::string wanted = LowerAscii(field_name);
  for (const auto& [name, typed] : row.fields) {
    if (LowerAscii(name) != wanted) continue;
    if (typed.is_null) return std::nullopt;
    std::int64_t parsed = 0;
    if (!TryParseI64Value(typed.encoded_value, &parsed)) return std::nullopt;
    return parsed;
  }
  return std::nullopt;
}

bool RelationRowsHaveFieldOrder(const EngineQueryRelation& relation,
                                const std::vector<std::string>& field_order) {
  if (field_order.empty() || relation.rows.empty()) return false;
  for (const auto& row : relation.rows) {
    for (const auto& field : field_order) {
      if (!RowFieldValueByName(row, field).has_value()) return false;
    }
    if (row.fields.size() != field_order.size()) return false;
  }
  return true;
}

bool RelationsAreNameAligned(const std::vector<EngineQueryRelation>& relations,
                             std::string* error_detail) {
  if (relations.size() < 2) {
    if (error_detail != nullptr) *error_detail = "set_operation_by_name_requires_two_relations";
    return false;
  }
  const auto field_order = FieldOrderForRelation(relations[0]);
  if (!RelationRowsHaveFieldOrder(relations[0], field_order) ||
      !RelationRowsHaveFieldOrder(relations[1], field_order)) {
    if (error_detail != nullptr) {
      *error_detail = "set_operation_by_name_requires_matching_int64_descriptor_fields";
    }
    return false;
  }
  return true;
}

exec::Batch RelationToBatchByName(const EngineQueryRelation& relation,
                                  const std::vector<std::string>& field_order) {
  std::vector<exec::Tuple> rows;
  rows.reserve(relation.rows.size());
  for (const auto& row : relation.rows) {
    exec::Tuple tuple;
    tuple.values.reserve(field_order.size());
    for (const auto& field : field_order) {
      tuple.values.push_back(RowFieldValueByName(row, field).value_or(0));
    }
    rows.push_back(std::move(tuple));
  }
  return exec::MakeBatch(relation.descriptor_digest.empty() ? relation.relation_name : relation.descriptor_digest,
                         std::move(rows));
}

EngineResultShape BatchToResultShape(const exec::Batch& batch) {
  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  std::size_t width = 0;
  for (const auto& row : batch.rows) { width = std::max(width, row.values.size()); }
  for (std::size_t i = 0; i < width; ++i) { shape.columns.push_back(Int64Descriptor()); }
  for (const auto& row : batch.rows) {
    EngineRowValue out;
    for (std::size_t i = 0; i < row.values.size(); ++i) {
      out.fields.push_back({"c" + std::to_string(i), Int64Value(row.values[i])});
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape ValuesRowsToResultShape(const std::vector<EngineRowValue>& rows) {
  EngineResultShape shape;
  shape.result_kind = "values_rowset";
  std::size_t width = 0;
  for (const auto& row : rows) { width = std::max(width, row.fields.size()); }
  for (std::size_t column = 0; column < width; ++column) {
    EngineDescriptor descriptor;
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = "text";
    descriptor.encoded_descriptor = "type=text";
    for (const auto& row : rows) {
      if (column < row.fields.size()) {
        descriptor = row.fields[column].second.descriptor;
        break;
      }
    }
    shape.columns.push_back(std::move(descriptor));
  }
  shape.rows = rows;
  return shape;
}

std::string EqualityKeyDescriptorFamily(std::string type_name);
std::optional<std::string> EqualityKeyForTypedValue(const EngineTypedValue& value);

std::optional<std::uint64_t> CountRelationRows(const EnginePlanOperationRequest& request,
                                               const EngineQueryRelation& relation,
                                               std::string* error_detail) {
  const bool count_all =
      ParseBoolValue(OptionValue(request, "count_all:"), request.aggregate_value_field.empty());
  const bool count_distinct =
      ParseBoolValue(OptionValue(request, "count_distinct:"), false);
  if (count_all) {
    if (request.predicate.predicate_kind.empty()) {
      return static_cast<std::uint64_t>(relation.rows.size());
    }
    std::uint64_t count = 0;
    for (const auto& row : relation.rows) {
      if (ProjectionRowMatchesPredicate(row, request.predicate)) ++count;
    }
    return count;
  }

  const std::string value_field = !request.aggregate_value_field.empty()
                                      ? request.aggregate_value_field
                                      : OptionValue(request, "aggregate_value_field:");
  if (value_field.empty()) {
    if (error_detail != nullptr) *error_detail = "query_plan_count_value_field_required";
    return std::nullopt;
  }
  const std::size_t value_column =
      ColumnIndexForRelation(relation, value_field, request.aggregate_value_column);
  std::set<std::string> distinct_values;
  std::uint64_t count = 0;
  for (const auto& row : relation.rows) {
    if (!ProjectionRowMatchesPredicate(row, request.predicate)) continue;
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_count_value_column_out_of_range";
      return std::nullopt;
    }
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    if (count_distinct) {
      const auto key = EqualityKeyForTypedValue(typed);
      if (key) distinct_values.insert(*key);
      continue;
    }
    ++count;
  }
  if (count_distinct) return static_cast<std::uint64_t>(distinct_values.size());
  return count;
}

EngineResultShape CountResultShape(const EnginePlanOperationRequest& request,
                                   const EngineQueryRelation& relation,
                                   std::string* error_detail) {
  const auto count = CountRelationRows(request, relation, error_detail);
  if (!count) return {};

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  EngineRowValue out;
  out.fields.push_back({"c0", Int64Value(static_cast<std::int64_t>(*count))});
  shape.rows.push_back(std::move(out));
  return shape;
}

EngineResultShape CountAssertionResultShape(const EnginePlanOperationRequest& request,
                                            std::uint64_t actual_count,
                                            std::string* error_detail) {
  const std::string assertion_id = OptionValue(request, "assertion_id:");
  std::string actual_column = OptionValue(request, "actual_column_name:");
  if (actual_column.empty()) actual_column = "actual_count";
  std::string expected_column = OptionValue(request, "expected_column_name:");
  if (expected_column.empty()) expected_column = "expected_count";

  std::int64_t expected_count = 0;
  if (!TryParseI64Value(OptionValue(request, "expected_count:"), &expected_count)) {
    if (error_detail != nullptr) *error_detail = "query_plan_count_assertion_expected_count_invalid";
    return {};
  }
  if (actual_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    if (error_detail != nullptr) *error_detail = "query_plan_count_assertion_actual_count_overflow";
    return {};
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(Int64Descriptor());
  EngineRowValue out;
  out.fields.push_back({"assertion_id", TextValue(assertion_id)});
  out.fields.push_back({actual_column, Int64Value(static_cast<std::int64_t>(actual_count))});
  out.fields.push_back({expected_column, Int64Value(expected_count)});
  shape.rows.push_back(std::move(out));
  return shape;
}

EngineResultShape NumericAssertionResultShape(const EnginePlanOperationRequest& request,
                                              double actual_value,
                                              std::string* error_detail) {
  const std::string assertion_id = OptionValue(request, "assertion_id:");
  std::string actual_column = OptionValue(request, "actual_column_name:");
  if (actual_column.empty()) actual_column = "actual_value";
  std::string expected_column = OptionValue(request, "expected_column_name:");
  if (expected_column.empty()) expected_column = "expected_value";
  const std::string expected_value = OptionValue(request, "expected_value:");
  if (expected_value.empty()) {
    if (error_detail != nullptr) *error_detail = "query_plan_numeric_assertion_expected_value_missing";
    return {};
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(TextDescriptor());
  EngineRowValue out;
  out.fields.push_back({"assertion_id", TextValue(assertion_id)});
  out.fields.push_back({actual_column, TextValue(FormatReal64(actual_value))});
  out.fields.push_back({expected_column, TextValue(expected_value)});
  shape.rows.push_back(std::move(out));
  return shape;
}

std::optional<double> EvaluateRecursiveCteAggregateAssertion(const exec::Batch& batch,
                                                            const std::string& aggregate_leaf,
                                                            std::string* error_detail) {
  if (aggregate_leaf == "count") {
    return static_cast<double>(batch.rows.size());
  }
  if (batch.rows.empty()) {
    if (error_detail != nullptr) {
      *error_detail = "query_plan_recursive_cte_aggregate_empty_input";
    }
    return std::nullopt;
  }
  bool saw_value = false;
  double value = 0.0;
  for (const auto& row : batch.rows) {
    if (row.values.empty()) {
      if (error_detail != nullptr) {
        *error_detail = "query_plan_recursive_cte_aggregate_column_missing";
      }
      return std::nullopt;
    }
    const double current = static_cast<double>(row.values.front());
    if (aggregate_leaf == "sum") {
      value += current;
      saw_value = true;
    } else if (aggregate_leaf == "min") {
      value = saw_value ? std::min(value, current) : current;
      saw_value = true;
    } else if (aggregate_leaf == "max") {
      value = saw_value ? std::max(value, current) : current;
      saw_value = true;
    } else {
      if (error_detail != nullptr) {
        *error_detail = "query_plan_recursive_cte_aggregate_current_route_unsupported";
      }
      return std::nullopt;
    }
  }
  return value;
}

std::optional<double> EvaluateRelationScalarAggregateAssertion(
    const EngineQueryRelation& relation,
    const std::string& aggregate_leaf,
    std::string value_field,
    std::size_t fallback_column,
    std::string* error_detail) {
  if (aggregate_leaf == "count") {
    return static_cast<double>(relation.rows.size());
  }
  if (value_field.empty()) {
    if (error_detail != nullptr) {
      *error_detail = "query_plan_scalar_aggregate_value_field_required";
    }
    return std::nullopt;
  }
  const std::size_t value_column =
      ColumnIndexForRelation(relation, value_field, fallback_column);
  bool saw_value = false;
  double value = 0.0;
  for (const auto& row : relation.rows) {
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) {
        *error_detail = "query_plan_scalar_aggregate_value_column_out_of_range";
      }
      return std::nullopt;
    }
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    double current = 0.0;
    if (!TryParseReal64Value(typed.encoded_value, &current)) {
      if (error_detail != nullptr) {
        *error_detail = "query_plan_scalar_aggregate_numeric_value_required";
      }
      return std::nullopt;
    }
    if (aggregate_leaf == "sum") {
      value += current;
      saw_value = true;
    } else if (aggregate_leaf == "min") {
      value = saw_value ? std::min(value, current) : current;
      saw_value = true;
    } else if (aggregate_leaf == "max") {
      value = saw_value ? std::max(value, current) : current;
      saw_value = true;
    } else {
      if (error_detail != nullptr) {
        *error_detail = "query_plan_scalar_aggregate_current_route_unsupported";
      }
      return std::nullopt;
    }
  }
  if (!saw_value) {
    if (error_detail != nullptr) {
      *error_detail = "query_plan_scalar_aggregate_empty_input";
    }
    return std::nullopt;
  }
  return value;
}

std::size_t RelationWidth(const EngineQueryRelation& relation) {
  std::size_t width = relation.columns.size();
  for (const auto& row : relation.rows) {
    width = std::max(width, row.fields.size());
  }
  return width;
}

EngineDescriptor DescriptorForRelationColumn(const EngineQueryRelation& relation,
                                             std::size_t column) {
  if (column < relation.columns.size()) return relation.columns[column];
  for (const auto& row : relation.rows) {
    if (column < row.fields.size()) {
      return InferredDescriptorForValue(row.fields[column].second);
    }
  }
  return TextDescriptor();
}

std::string EqualityKeyDescriptorFamily(std::string type_name) {
  type_name = LowerAscii(std::move(type_name));
  if (type_name.empty() ||
      type_name == "text" ||
      type_name == "character" ||
      type_name == "char" ||
      type_name == "string" ||
      type_name == "varchar" ||
      type_name == "character varying" ||
      type_name.rfind("char(", 0) == 0 ||
      type_name.rfind("varchar(", 0) == 0 ||
      type_name.rfind("character(", 0) == 0 ||
      type_name.rfind("character varying(", 0) == 0) {
    return "text";
  }
  return type_name;
}

std::optional<std::string> EqualityKeyForTypedValue(const EngineTypedValue& value) {
  const auto typed = NormalizeTypedValue(value);
  if (typed.is_null) return std::nullopt;
  return EqualityKeyDescriptorFamily(typed.descriptor.canonical_type_name) + ":" +
         typed.encoded_value;
}

std::optional<std::string> JoinKeyForRow(const EngineRowValue& row,
                                         std::size_t column,
                                         std::string* error_detail) {
  if (column >= row.fields.size()) {
    if (error_detail != nullptr) *error_detail = "query_plan_join_key_column_out_of_range";
    return std::nullopt;
  }
  return EqualityKeyForTypedValue(row.fields[column].second);
}

EngineTypedValue JoinOutputValue(const EngineRowValue& row,
                                 std::size_t column,
                                 const EngineDescriptor& fallback_descriptor) {
  if (column >= row.fields.size()) { return NullValue(fallback_descriptor); }
  const EngineTypedValue& typed = row.fields[column].second;
  if (!typed.descriptor.canonical_type_name.empty() || typed.is_null) {
    EngineTypedValue out = typed;
    if (out.is_null) { out.encoded_value.clear(); }
    return out;
  }
  return NormalizeTypedValue(typed);
}

EngineResultShape TypedInnerJoinResultShape(const EnginePlanOperationRequest& request,
                                            const EngineQueryRelation& left,
                                            const EngineQueryRelation& right,
                                            std::size_t left_key_column,
                                            std::size_t right_key_column,
                                            std::string_view algorithm,
                                            std::string* error_detail) {
  const std::size_t left_width = RelationWidth(left);
  const std::size_t right_width = RelationWidth(right);
  const std::size_t offset =
      ParseSizeValue(OptionValue(request, "offset:"), static_cast<std::size_t>(request.offset));
  const std::size_t limit =
      ParseSizeValue(OptionValue(request, "limit:"), static_cast<std::size_t>(request.limit));

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  for (std::size_t column = 0; column < left_width; ++column) {
    shape.columns.push_back(DescriptorForRelationColumn(left, column));
  }
  for (std::size_t column = 0; column < right_width; ++column) {
    shape.columns.push_back(DescriptorForRelationColumn(right, column));
  }

  std::size_t matched_index = 0;
  const auto emit_joined_row = [&](const EngineRowValue& left_row,
                                   const EngineRowValue& right_row) -> bool {
    if (matched_index++ < offset) return true;
    if (limit != 0 && shape.rows.size() >= limit) return false;

    EngineRowValue out;
    out.requested_row_uuid = left_row.requested_row_uuid;
    std::size_t output_column = 0;
    for (std::size_t column = 0; column < left_width; ++column, ++output_column) {
      out.fields.push_back({"c" + std::to_string(output_column),
                            JoinOutputValue(left_row, column, shape.columns[output_column])});
    }
    for (std::size_t column = 0; column < right_width; ++column, ++output_column) {
      out.fields.push_back({"c" + std::to_string(output_column),
                            JoinOutputValue(right_row, column, shape.columns[output_column])});
    }
    shape.rows.push_back(std::move(out));
    return limit == 0 || shape.rows.size() < limit;
  };

  const std::string normalized_algorithm = LowerAscii(std::string(algorithm));
  if (normalized_algorithm == "nested" || normalized_algorithm == "nested_loop") {
    for (const auto& left_row : left.rows) {
      const auto left_key = JoinKeyForRow(left_row, left_key_column, error_detail);
      if (error_detail != nullptr && !error_detail->empty()) return {};
      if (!left_key) continue;
      for (const auto& right_row : right.rows) {
        const auto right_key = JoinKeyForRow(right_row, right_key_column, error_detail);
        if (error_detail != nullptr && !error_detail->empty()) return {};
        if (right_key && *left_key == *right_key && !emit_joined_row(left_row, right_row)) {
          return shape;
        }
      }
    }
    return shape;
  }

  if (normalized_algorithm == "merge") {
    std::vector<std::pair<std::string, const EngineRowValue*>> left_rows;
    std::vector<std::pair<std::string, const EngineRowValue*>> right_rows;
    left_rows.reserve(left.rows.size());
    right_rows.reserve(right.rows.size());
    for (const auto& row : left.rows) {
      const auto key = JoinKeyForRow(row, left_key_column, error_detail);
      if (error_detail != nullptr && !error_detail->empty()) return {};
      if (key) left_rows.push_back({*key, &row});
    }
    for (const auto& row : right.rows) {
      const auto key = JoinKeyForRow(row, right_key_column, error_detail);
      if (error_detail != nullptr && !error_detail->empty()) return {};
      if (key) right_rows.push_back({*key, &row});
    }
    const auto by_key = [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) return lhs.first < rhs.first;
      return lhs.second < rhs.second;
    };
    std::sort(left_rows.begin(), left_rows.end(), by_key);
    std::sort(right_rows.begin(), right_rows.end(), by_key);
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left_rows.size() && right_index < right_rows.size()) {
      if (left_rows[left_index].first < right_rows[right_index].first) {
        ++left_index;
        continue;
      }
      if (right_rows[right_index].first < left_rows[left_index].first) {
        ++right_index;
        continue;
      }
      const std::string key = left_rows[left_index].first;
      const std::size_t left_begin = left_index;
      const std::size_t right_begin = right_index;
      while (left_index < left_rows.size() && left_rows[left_index].first == key) ++left_index;
      while (right_index < right_rows.size() && right_rows[right_index].first == key) ++right_index;
      for (std::size_t l = left_begin; l < left_index; ++l) {
        for (std::size_t r = right_begin; r < right_index; ++r) {
          if (!emit_joined_row(*left_rows[l].second, *right_rows[r].second)) return shape;
        }
      }
    }
    return shape;
  }

  std::unordered_map<std::string, std::vector<const EngineRowValue*>> right_rows_by_key;
  right_rows_by_key.reserve(right.rows.size());
  for (const auto& row : right.rows) {
    const auto key = JoinKeyForRow(row, right_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return {};
    if (key) right_rows_by_key[*key].push_back(&row);
  }

  for (const auto& left_row : left.rows) {
    const auto key = JoinKeyForRow(left_row, left_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return {};
    if (!key) continue;
    const auto found = right_rows_by_key.find(*key);
    if (found == right_rows_by_key.end()) continue;
    for (const auto* right_row : found->second) {
      if (!emit_joined_row(left_row, *right_row)) return shape;
    }
  }
  return shape;
}

std::uint64_t TypedLeftJoinRightNullFilterRowCount(const EngineQueryRelation& left,
                                                   const EngineQueryRelation& right,
                                                   std::size_t left_key_column,
                                                   std::size_t right_key_column,
                                                   std::size_t right_filter_column,
                                                   std::string* error_detail) {
  if (right_filter_column == std::numeric_limits<std::size_t>::max()) {
    if (error_detail != nullptr) {
      *error_detail = "query_plan_left_join_null_filter_field_not_found";
    }
    return 0;
  }

  std::unordered_map<std::string, std::vector<const EngineRowValue*>> right_rows_by_key;
  right_rows_by_key.reserve(right.rows.size());
  for (const auto& row : right.rows) {
    const auto key = JoinKeyForRow(row, right_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0;
    if (key) right_rows_by_key[*key].push_back(&row);
  }

  std::uint64_t count = 0;
  for (const auto& left_row : left.rows) {
    const auto key = JoinKeyForRow(left_row, left_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0;
    if (!key) {
      ++count;
      continue;
    }

    const auto found = right_rows_by_key.find(*key);
    if (found == right_rows_by_key.end()) {
      ++count;
      continue;
    }
    for (const auto* right_row : found->second) {
      if (right_filter_column >= right_row->fields.size()) {
        if (error_detail != nullptr) {
          *error_detail = "query_plan_left_join_null_filter_column_out_of_range";
        }
        return 0;
      }
      if (NormalizeTypedValue(right_row->fields[right_filter_column].second).is_null) {
        ++count;
      }
    }
  }
  return count;
}

std::uint64_t TypedSemiJoinMatchedLeftRowCount(const EngineQueryRelation& left,
                                               const EngineQueryRelation& right,
                                               std::size_t left_key_column,
                                               std::size_t right_key_column,
                                               std::string* error_detail) {
  std::unordered_map<std::string, bool> right_keys;
  right_keys.reserve(right.rows.size());
  for (const auto& row : right.rows) {
    const auto key = JoinKeyForRow(row, right_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0;
    if (key) right_keys[*key] = true;
  }

  std::uint64_t count = 0;
  for (const auto& left_row : left.rows) {
    const auto key = JoinKeyForRow(left_row, left_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0;
    if (key && right_keys.find(*key) != right_keys.end()) {
      ++count;
    }
  }
  return count;
}

struct DescriptorRowFilter {
  std::string kind;
  std::size_t column = std::numeric_limits<std::size_t>::max();
  std::vector<std::string> values;
};

std::optional<std::vector<DescriptorRowFilter>> DescriptorRowFiltersForRequest(
    const EnginePlanOperationRequest& request,
    const EngineQueryRelation& relation,
    std::string_view prefix,
    std::string* error_detail) {
  const std::string count_text = OptionValue(request, std::string(prefix) + "count:");
  if (count_text.empty()) return std::vector<DescriptorRowFilter>{};
  std::uint64_t count = 0;
  try {
    count = static_cast<std::uint64_t>(std::stoull(count_text));
  } catch (...) {
    if (error_detail != nullptr) *error_detail = "query_plan_join_filter_count_invalid";
    return std::nullopt;
  }
  if (count > 8) {
    if (error_detail != nullptr) *error_detail = "query_plan_join_filter_count_unsupported";
    return std::nullopt;
  }

  std::vector<DescriptorRowFilter> filters;
  filters.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    const std::string filter_prefix =
        std::string(prefix) + std::to_string(index) + "_";
    DescriptorRowFilter filter;
    filter.kind = OptionValue(request, filter_prefix + "kind:");
    const std::string column = OptionValue(request, filter_prefix + "column:");
    const std::string values = OptionValue(request, filter_prefix + "value:");
    if (filter.kind.empty() || column.empty()) {
      if (error_detail != nullptr) *error_detail = "query_plan_join_filter_descriptor_required";
      return std::nullopt;
    }
    if (filter.kind != "column_equals" && filter.kind != "column_in_list") {
      if (error_detail != nullptr) *error_detail = "query_plan_join_filter_kind_unsupported";
      return std::nullopt;
    }
    filter.column =
        ColumnIndexForRelation(relation, column, std::numeric_limits<std::size_t>::max());
    if (filter.column == std::numeric_limits<std::size_t>::max()) {
      if (error_detail != nullptr) *error_detail = "query_plan_join_filter_column_not_found";
      return std::nullopt;
    }
    filter.values = Split(values, ',');
    if (filter.values.empty()) {
      if (error_detail != nullptr) *error_detail = "query_plan_join_filter_value_required";
      return std::nullopt;
    }
    filters.push_back(std::move(filter));
  }
  return filters;
}

bool RowMatchesDescriptorFilter(const EngineRowValue& row,
                                const DescriptorRowFilter& filter,
                                std::string* error_detail) {
  if (filter.column >= row.fields.size()) {
    if (error_detail != nullptr) *error_detail = "query_plan_join_filter_column_out_of_range";
    return false;
  }
  const auto typed = NormalizeTypedValue(row.fields[filter.column].second);
  if (typed.is_null) return false;
  if (filter.kind == "column_equals") {
    return !filter.values.empty() && typed.encoded_value == filter.values.front();
  }
  if (filter.kind == "column_in_list") {
    for (const auto& value : filter.values) {
      if (typed.encoded_value == value) return true;
    }
    return false;
  }
  if (error_detail != nullptr) *error_detail = "query_plan_join_filter_kind_unsupported";
  return false;
}

bool RowMatchesDescriptorFilters(const EngineRowValue& row,
                                 const std::vector<DescriptorRowFilter>& filters,
                                 std::string* error_detail) {
  for (const auto& filter : filters) {
    if (!RowMatchesDescriptorFilter(row, filter, error_detail)) return false;
    if (error_detail != nullptr && !error_detail->empty()) return false;
  }
  return true;
}

std::uint64_t TypedInnerJoinDistinctLeftCount(
    const EngineQueryRelation& left,
    const EngineQueryRelation& right,
    std::size_t left_key_column,
    std::size_t right_key_column,
    std::string distinct_left_field,
    const std::vector<DescriptorRowFilter>& left_filters,
    std::string* error_detail) {
  const std::size_t distinct_column =
      ColumnIndexForRelation(left, distinct_left_field, std::numeric_limits<std::size_t>::max());
  if (distinct_column == std::numeric_limits<std::size_t>::max()) {
    if (error_detail != nullptr) *error_detail = "query_plan_join_distinct_field_not_found";
    return 0;
  }

  std::set<std::string> right_keys;
  for (const auto& row : right.rows) {
    const auto key = JoinKeyForRow(row, right_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0;
    if (key) right_keys.insert(*key);
  }

  std::set<std::string> distinct_values;
  for (const auto& left_row : left.rows) {
    if (!RowMatchesDescriptorFilters(left_row, left_filters, error_detail)) {
      if (error_detail != nullptr && !error_detail->empty()) return 0;
      continue;
    }
    const auto key = JoinKeyForRow(left_row, left_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0;
    if (!key || right_keys.find(*key) == right_keys.end()) continue;
    if (distinct_column >= left_row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_join_distinct_column_out_of_range";
      return 0;
    }
    const auto distinct_value = NormalizeTypedValue(left_row.fields[distinct_column].second);
    if (distinct_value.is_null) continue;
    const auto distinct_key = EqualityKeyForTypedValue(distinct_value);
    if (distinct_key) distinct_values.insert(*distinct_key);
  }
  return static_cast<std::uint64_t>(distinct_values.size());
}

double TypedJoinGroupSumHavingTotal(const EngineQueryRelation& left,
                                    const EngineQueryRelation& right,
                                    std::size_t left_key_column,
                                    std::size_t right_key_column,
                                    std::size_t left_group_column,
                                    std::size_t right_value_column,
                                    double having_threshold,
                                    std::string* error_detail) {
  if (left_group_column == std::numeric_limits<std::size_t>::max()) {
    if (error_detail != nullptr) *error_detail = "query_plan_join_group_key_field_not_found";
    return 0.0;
  }
  if (right_value_column == std::numeric_limits<std::size_t>::max()) {
    if (error_detail != nullptr) *error_detail = "query_plan_join_group_value_field_not_found";
    return 0.0;
  }

  std::unordered_map<std::string, std::vector<const EngineRowValue*>> right_rows_by_key;
  right_rows_by_key.reserve(right.rows.size());
  for (const auto& row : right.rows) {
    const auto key = JoinKeyForRow(row, right_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0.0;
    if (key) right_rows_by_key[*key].push_back(&row);
  }

  std::map<std::string, double> sums_by_group;
  for (const auto& left_row : left.rows) {
    const auto key = JoinKeyForRow(left_row, left_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0.0;
    if (!key) continue;
    const auto found = right_rows_by_key.find(*key);
    if (found == right_rows_by_key.end()) continue;
    if (left_group_column >= left_row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_join_group_key_column_out_of_range";
      return 0.0;
    }
    const auto group_value = NormalizeTypedValue(left_row.fields[left_group_column].second);
    if (group_value.is_null) continue;
    const std::string group_key = group_value.descriptor.canonical_type_name + ":" + group_value.encoded_value;
    for (const auto* right_row : found->second) {
      if (right_value_column >= right_row->fields.size()) {
        if (error_detail != nullptr) *error_detail = "query_plan_join_group_value_column_out_of_range";
        return 0.0;
      }
      const auto value = NormalizeTypedValue(right_row->fields[right_value_column].second);
      if (value.is_null) continue;
      double parsed = 0.0;
      if (!TryParseReal64Value(value.encoded_value, &parsed)) {
        if (error_detail != nullptr) *error_detail = "query_plan_join_group_value_not_numeric";
        return 0.0;
      }
      sums_by_group[group_key] += parsed;
    }
  }

  double total = 0.0;
  for (const auto& [group_key, sum] : sums_by_group) {
    (void)group_key;
    if (sum >= having_threshold) total += sum;
  }
  return total;
}

std::uint64_t TypedJoinWindowMaxRowNumber(const EngineQueryRelation& left,
                                          const EngineQueryRelation& right,
                                          std::size_t left_key_column,
                                          std::size_t right_key_column,
                                          std::size_t left_partition_column,
                                          std::string* error_detail) {
  if (left_partition_column == std::numeric_limits<std::size_t>::max()) {
    if (error_detail != nullptr) *error_detail = "query_plan_join_window_partition_field_not_found";
    return 0;
  }

  std::unordered_map<std::string, std::vector<const EngineRowValue*>> right_rows_by_key;
  right_rows_by_key.reserve(right.rows.size());
  for (const auto& row : right.rows) {
    const auto key = JoinKeyForRow(row, right_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0;
    if (key) right_rows_by_key[*key].push_back(&row);
  }

  std::map<std::string, std::uint64_t> counts_by_partition;
  for (const auto& left_row : left.rows) {
    const auto key = JoinKeyForRow(left_row, left_key_column, error_detail);
    if (error_detail != nullptr && !error_detail->empty()) return 0;
    if (!key) continue;
    const auto found = right_rows_by_key.find(*key);
    if (found == right_rows_by_key.end()) continue;
    if (left_partition_column >= left_row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_join_window_partition_column_out_of_range";
      return 0;
    }
    const auto partition_value = NormalizeTypedValue(left_row.fields[left_partition_column].second);
    if (partition_value.is_null) continue;
    const std::string partition_key =
        partition_value.descriptor.canonical_type_name + ":" + partition_value.encoded_value;
    counts_by_partition[partition_key] += static_cast<std::uint64_t>(found->second.size());
  }

  std::uint64_t max_row_number = 0;
  for (const auto& [partition_key, count] : counts_by_partition) {
    (void)partition_key;
    max_row_number = std::max(max_row_number, count);
  }
  return max_row_number;
}

std::optional<EngineTypedValue> NormalizedRowValueAt(const EngineRowValue& row,
                                                     std::size_t column,
                                                     std::string_view error_code,
                                                     std::string* error_detail) {
  if (column >= row.fields.size()) {
    if (error_detail != nullptr) *error_detail = std::string(error_code);
    return std::nullopt;
  }
  return NormalizeTypedValue(row.fields[column].second);
}

std::optional<std::int64_t> RowInt64ValueForRoute(const EngineRowValue& row,
                                                  std::size_t column,
                                                  std::string_view error_code,
                                                  std::string* error_detail) {
  const auto typed = NormalizedRowValueAt(row, column, error_code, error_detail);
  if (!typed) return std::nullopt;
  if (typed->is_null) {
    if (error_detail != nullptr) *error_detail = std::string(error_code);
    return std::nullopt;
  }
  std::int64_t parsed = 0;
  if (!TryParseI64Value(typed->encoded_value, &parsed)) {
    if (error_detail != nullptr) *error_detail = std::string(error_code);
    return std::nullopt;
  }
  return parsed;
}

EngineResultShape PivotResultShape(const EnginePlanOperationRequest& request,
                                   const EngineQueryRelation& relation,
                                   std::string* error_detail) {
  const std::string group_field = OptionValue(request, "pivot_group_field:");
  const std::string for_field = OptionValue(request, "pivot_for_field:");
  const std::string value_field = OptionValue(request, "pivot_value_field:");
  auto in_values = SplitOptionList(request, "pivot_in_values:");
  std::string aggregate_function = LowerAscii(OptionValue(request, "pivot_aggregate_function:"));
  if (aggregate_function.empty()) {
    aggregate_function = LowerAscii(OptionValue(request, "aggregate_function:"));
  }
  if (aggregate_function.empty()) aggregate_function = "sum";
  const std::string aggregate_leaf = AggregateFunctionLeaf(aggregate_function);
  if (group_field.empty() || for_field.empty() || value_field.empty()) {
    if (error_detail != nullptr) *error_detail = "query_plan_pivot_requires_descriptor_fields";
    return {};
  }
  if (in_values.empty()) {
    if (error_detail != nullptr) *error_detail = "query_plan_pivot_requires_in_values";
    return {};
  }
  if (aggregate_leaf != "sum" && aggregate_leaf != "count" &&
      aggregate_leaf != "min" && aggregate_leaf != "max" &&
      aggregate_leaf != "avg" && aggregate_leaf != "count_distinct") {
    if (error_detail != nullptr) *error_detail = "query_plan_pivot_aggregate_unsupported";
    return {};
  }

  const std::size_t group_column = ColumnIndexForRelation(relation, group_field, 0);
  const std::size_t for_column = ColumnIndexForRelation(relation, for_field, 1);
  const std::size_t value_column = ColumnIndexForRelation(relation, value_field, 2);
  struct PivotStats {
    std::int64_t sum = 0;
    std::uint64_t count = 0;
    std::int64_t min = 0;
    std::int64_t max = 0;
    std::set<std::int64_t> distinct_values;
  };
  std::map<std::string, std::map<std::string, PivotStats>> grouped;
  for (const auto& row : relation.rows) {
    const auto group_value =
        NormalizedRowValueAt(row, group_column, "query_plan_pivot_group_column_out_of_range", error_detail);
    const auto for_value =
        NormalizedRowValueAt(row, for_column, "query_plan_pivot_for_column_out_of_range", error_detail);
    if (!group_value || !for_value) return {};
    if (group_value->is_null || for_value->is_null) {
      if (error_detail != nullptr) *error_detail = "query_plan_pivot_requires_non_null_group_and_for_values";
      return {};
    }
    std::optional<std::int64_t> parsed_value;
    if (aggregate_leaf != "count") {
      const auto value =
          RowInt64ValueForRoute(row, value_column, "query_plan_pivot_aggregate_requires_int64_value", error_detail);
      if (!value) return {};
      parsed_value = *value;
    }
    for (const auto& in_value : in_values) {
      if (for_value->encoded_value == in_value) {
        auto& stats = grouped[group_value->encoded_value][in_value];
        ++stats.count;
        if (parsed_value) {
          stats.sum += *parsed_value;
          if (stats.count == 1) {
            stats.min = *parsed_value;
            stats.max = *parsed_value;
          } else {
            stats.min = std::min(stats.min, *parsed_value);
            stats.max = std::max(stats.max, *parsed_value);
          }
          stats.distinct_values.insert(*parsed_value);
        }
        break;
      }
    }
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(TextDescriptor());
  for (std::size_t index = 0; index < in_values.size(); ++index) {
    shape.columns.push_back(Int64Descriptor());
  }
  for (const auto& [group_key, pivot_values] : grouped) {
    EngineRowValue out;
    out.fields.push_back({"c0", TextValue(group_key)});
    for (std::size_t index = 0; index < in_values.size(); ++index) {
      const auto found = pivot_values.find(in_values[index]);
      std::int64_t rendered = 0;
      if (found != pivot_values.end()) {
        const auto& stats = found->second;
        if (aggregate_leaf == "sum") {
          rendered = stats.sum;
        } else if (aggregate_leaf == "count") {
          rendered = static_cast<std::int64_t>(stats.count);
        } else if (aggregate_leaf == "min") {
          rendered = stats.count == 0 ? 0 : stats.min;
        } else if (aggregate_leaf == "max") {
          rendered = stats.count == 0 ? 0 : stats.max;
        } else if (aggregate_leaf == "avg") {
          rendered = stats.count == 0 ? 0 : stats.sum / static_cast<std::int64_t>(stats.count);
        } else if (aggregate_leaf == "count_distinct") {
          rendered = static_cast<std::int64_t>(stats.distinct_values.size());
        }
      }
      out.fields.push_back({"c" + std::to_string(index + 1),
                            Int64Value(rendered)});
    }
    shape.rows.push_back(std::move(out));
  }
  return shape;
}

EngineResultShape UnpivotResultShape(const EnginePlanOperationRequest& request,
                                     const EngineQueryRelation& relation,
                                     std::string* error_detail) {
  const std::string group_field = OptionValue(request, "unpivot_group_field:");
  auto value_fields = SplitOptionList(request, "unpivot_value_fields:");
  if (value_fields.empty()) {
    const std::string single_value_field = OptionValue(request, "unpivot_value_field:");
    if (!single_value_field.empty()) value_fields.push_back(single_value_field);
  }
  auto name_values = SplitOptionList(request, "unpivot_name_values:");
  if (name_values.empty()) name_values = value_fields;
  if (group_field.empty() || value_fields.empty()) {
    if (error_detail != nullptr) *error_detail = "query_plan_unpivot_requires_descriptor_fields";
    return {};
  }
  if (name_values.size() != value_fields.size()) {
    if (error_detail != nullptr) *error_detail = "query_plan_unpivot_name_value_arity_mismatch";
    return {};
  }

  const std::size_t group_column = ColumnIndexForRelation(relation, group_field, 0);
  std::vector<std::size_t> value_columns;
  value_columns.reserve(value_fields.size());
  for (const auto& field : value_fields) {
    value_columns.push_back(ColumnIndexForRelation(relation, field, value_columns.size() + 1));
  }

  EngineDescriptor value_descriptor = Int64Descriptor();
  if (!relation.rows.empty() && !value_columns.empty() &&
      value_columns.front() < relation.rows.front().fields.size()) {
    value_descriptor =
        InferredDescriptorForValue(relation.rows.front().fields[value_columns.front()].second);
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(value_descriptor);
  for (const auto& row : relation.rows) {
    const auto group_value =
        NormalizedRowValueAt(row, group_column, "query_plan_unpivot_group_column_out_of_range", error_detail);
    if (!group_value) return {};
    for (std::size_t index = 0; index < value_columns.size(); ++index) {
      const auto value =
          NormalizedRowValueAt(row, value_columns[index], "query_plan_unpivot_value_column_out_of_range", error_detail);
      if (!value) return {};
      EngineRowValue out;
      out.fields.push_back({"c0", group_value->is_null ? NullValue(TextDescriptor())
                                                       : TextValue(group_value->encoded_value)});
      out.fields.push_back({"c1", TextValue(name_values[index])});
      out.fields.push_back({"c2", *value});
      shape.rows.push_back(std::move(out));
    }
  }
  return shape;
}

EngineQueryRelation CrudRelation(const CrudState& state,
                                 const std::string& table_uuid,
                                 const EngineRequestContext& context,
                                 const EnginePredicateEnvelope& predicate) {
  EngineQueryRelation relation;
  relation.relation_name = "crud:" + table_uuid;
  relation.descriptor_digest = relation.relation_name;
  relation.source_object.uuid.canonical = table_uuid;
  relation.source_object.object_kind = "table";
  auto rows = VisibleCrudRowsForContext(state, table_uuid, context);
  if (!predicate.predicate_kind.empty()) {
    std::vector<CrudRowVersionRecord> filtered;
    for (const auto& row : rows) {
      if (CrudRowMatchesPredicate(row, predicate)) { filtered.push_back(row); }
    }
    rows = std::move(filtered);
  }
  const auto table = FindVisibleCrudTable(state, table_uuid, context.local_transaction_id);
  if (table) {
    relation.columns = CrudRelationColumnDescriptors(*table);
    const auto descriptors_by_name = CrudColumnDescriptorsByName(*table);
    std::set<std::string> table_column_names;
    for (const auto& [name, encoded] : table->columns) {
      (void)encoded;
      table_column_names.insert(name);
    }
    relation.rows.reserve(rows.size());
    for (const auto& row : rows) {
      std::unordered_map<std::string, const std::string*> values_by_name;
      values_by_name.reserve(row.values.size());
      for (const auto& [field, value] : row.values) {
        values_by_name.emplace(field, &value);
      }
      EngineRowValue out_row;
      out_row.requested_row_uuid.canonical = row.row_uuid;
      out_row.fields.reserve(row.values.size());
      for (const auto& [field, encoded] : table->columns) {
        (void)encoded;
        const auto value = values_by_name.find(field);
        if (value == values_by_name.end()) { continue; }
        const auto descriptor = descriptors_by_name.find(field);
        out_row.fields.push_back(
            {field,
             CrudRelationTypedValue(*value->second,
                                    descriptor == descriptors_by_name.end()
                                        ? nullptr
                                        : &descriptor->second)});
      }
      for (const auto& [field, value] : row.values) {
        if (table_column_names.count(field) != 0) { continue; }
        const auto descriptor = descriptors_by_name.find(field);
        out_row.fields.push_back(
            {field,
             CrudRelationTypedValue(value,
                                    descriptor == descriptors_by_name.end()
                                        ? nullptr
                                        : &descriptor->second)});
      }
      relation.rows.push_back(std::move(out_row));
    }
  } else {
    relation.rows.reserve(rows.size());
    for (const auto& row : rows) {
      EngineRowValue out_row;
      out_row.requested_row_uuid.canonical = row.row_uuid;
      out_row.fields.reserve(row.values.size());
      for (const auto& [field, value] : row.values) {
        out_row.fields.push_back({field, CrudRelationTypedValue(value, nullptr)});
      }
      relation.rows.push_back(std::move(out_row));
    }
  }
  return relation;
}

std::optional<std::string> ProjectionFieldValue(const EngineRowValue& row,
                                                const std::string& field_name) {
  for (const auto& [field, value] : row.fields) {
    if (field == field_name) {
      const auto typed = NormalizeTypedValue(value);
      if (typed.is_null) return std::string{};
      return typed.encoded_value;
    }
  }
  return std::nullopt;
}

bool SqlLikePatternMatches(std::string_view value, std::string_view pattern) {
  std::vector<bool> previous(pattern.size() + 1, false);
  std::vector<bool> current(pattern.size() + 1, false);
  previous[0] = true;
  for (std::size_t pattern_index = 0; pattern_index < pattern.size();) {
    const char pattern_char = pattern[pattern_index];
    if (pattern_char == '%') {
      previous[pattern_index + 1] = previous[pattern_index];
    }
    if (pattern_char == '\\' && pattern_index + 1 < pattern.size()) {
      ++pattern_index;
    }
    ++pattern_index;
  }

  for (std::size_t value_index = 0; value_index < value.size(); ++value_index) {
    std::fill(current.begin(), current.end(), false);
    for (std::size_t pattern_index = 0; pattern_index < pattern.size(); ++pattern_index) {
      const char pattern_char = pattern[pattern_index];
      if (pattern_char == '%') {
        current[pattern_index + 1] = current[pattern_index] || previous[pattern_index + 1];
        continue;
      }
      char expected = pattern_char;
      bool escaped = false;
      if (pattern_char == '\\' && pattern_index + 1 < pattern.size()) {
        escaped = true;
        expected = pattern[++pattern_index];
      }
      if ((expected == '_' && !escaped) || expected == value[value_index]) {
        current[pattern_index + 1] = previous[pattern_index];
      }
    }
    previous.swap(current);
  }
  return previous[pattern.size()];
}

int CompareProjectionScalar(const std::string& left, const std::string& right) {
  std::int64_t left_i64 = 0;
  std::int64_t right_i64 = 0;
  if (TryParseI64Value(left, &left_i64) && TryParseI64Value(right, &right_i64)) {
    if (left_i64 < right_i64) return -1;
    if (left_i64 > right_i64) return 1;
    return 0;
  }
  if (left < right) return -1;
  if (left > right) return 1;
  return 0;
}

bool ProjectionRowMatchesPredicate(const EngineRowValue& row,
                                   const EnginePredicateEnvelope& predicate) {
  if (predicate.predicate_kind.empty()) return true;
  if (predicate.predicate_kind == "column_equals") {
    if (predicate.bound_values.empty()) return false;
    const auto value = ProjectionFieldValue(row, predicate.canonical_predicate_envelope);
    return value && *value == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "columns_all_equal") {
    const auto columns = Split(predicate.canonical_predicate_envelope, ',');
    if (columns.empty() || predicate.bound_values.size() < columns.size()) return false;
    for (std::size_t index = 0; index < columns.size(); ++index) {
      const auto value = ProjectionFieldValue(row, columns[index]);
      if (!value || *value != predicate.bound_values[index].encoded_value) return false;
    }
    return true;
  }
  if (predicate.predicate_kind == "columns_all_null") {
    const auto columns = Split(predicate.canonical_predicate_envelope, ',');
    if (columns.empty()) return false;
    for (const auto& column : columns) {
      const auto value = ProjectionFieldValue(row, column);
      if (value && !value->empty()) return false;
    }
    return true;
  }
  if (predicate.predicate_kind == "columns_all_not_null") {
    const auto columns = Split(predicate.canonical_predicate_envelope, ',');
    if (columns.empty()) return false;
    for (const auto& column : columns) {
      const auto value = ProjectionFieldValue(row, column);
      if (!value || value->empty()) return false;
    }
    return true;
  }
  if (predicate.predicate_kind == "column_equals_column_or_left_null") {
    const auto columns = Split(predicate.canonical_predicate_envelope, ',');
    if (columns.size() != 2) return false;
    const auto left = ProjectionFieldValue(row, columns[0]);
    if (left && left->empty()) return true;
    const auto right = ProjectionFieldValue(row, columns[1]);
    return left && right && *left == *right;
  }
  if (predicate.predicate_kind == "column_mod_equals") {
    if (predicate.bound_values.size() < 2) return false;
    const auto value_text = ProjectionFieldValue(row, predicate.canonical_predicate_envelope);
    if (!value_text) return false;
    try {
      const auto value = std::stoll(*value_text);
      const auto divisor = std::stoll(predicate.bound_values[0].encoded_value);
      const auto expected = std::stoll(predicate.bound_values[1].encoded_value);
      return divisor != 0 && value % divisor == expected;
    } catch (...) {
      return false;
    }
  }
  if (predicate.predicate_kind == "column_in_list") {
    const auto value = ProjectionFieldValue(row, predicate.canonical_predicate_envelope);
    if (!value) return false;
    for (const auto& bound : predicate.bound_values) {
      if (*value == bound.encoded_value) return true;
    }
    return false;
  }
  if (predicate.predicate_kind == "column_not_in_list") {
    const auto value = ProjectionFieldValue(row, predicate.canonical_predicate_envelope);
    if (!value) return false;
    for (const auto& bound : predicate.bound_values) {
      if (*value == bound.encoded_value) return false;
    }
    return true;
  }
  if (predicate.predicate_kind == "column_range") {
    const auto value = ProjectionFieldValue(row, predicate.canonical_predicate_envelope);
    if (!value) return false;
    const bool lower_ok = predicate.bound_values.empty() ||
                          CompareProjectionScalar(*value, predicate.bound_values[0].encoded_value) >= 0;
    const bool upper_ok = predicate.bound_values.size() < 2 ||
                          CompareProjectionScalar(*value, predicate.bound_values[1].encoded_value) <= 0;
    return lower_ok && upper_ok;
  }
  if (predicate.predicate_kind == "column_like" ||
      predicate.predicate_kind == "column_not_like") {
    if (predicate.bound_values.empty()) return false;
    const auto value = ProjectionFieldValue(row, predicate.canonical_predicate_envelope);
    if (!value) return false;
    const bool matches = SqlLikePatternMatches(*value, predicate.bound_values.front().encoded_value);
    return predicate.predicate_kind == "column_like" ? matches : !matches;
  }
  if (predicate.predicate_kind == "expression_equals") {
    if (predicate.bound_values.empty()) return false;
    const std::string expression = predicate.canonical_predicate_envelope;
    const auto separator = expression.find(':');
    if (separator == std::string::npos) return false;
    const std::string function_name = LowerAscii(expression.substr(0, separator));
    auto value = ProjectionFieldValue(row, expression.substr(separator + 1));
    if (!value) return false;
    if (function_name == "lower") {
      *value = LowerAscii(std::move(*value));
    } else if (function_name == "upper") {
      *value = UpperAscii(std::move(*value));
    } else {
      return false;
    }
    return *value == predicate.bound_values.front().encoded_value;
  }
  return false;
}

SysInformationProjectionContext ProjectionContextFromRequest(
    const EnginePlanOperationRequest& request) {
  SysInformationProjectionContext context;
  context.catalog_display_name = "default";
  context.session_language = request.context.language_context.language_tag.empty()
                                 ? "en"
                                 : request.context.language_context.language_tag;
  context.default_language = request.context.language_context.default_language_tag.empty()
                                 ? "en"
                                 : request.context.language_context.default_language_tag;
  context.session_uuid = request.context.session_uuid.canonical;
  context.principal_uuid = request.context.principal_uuid.canonical;
  context.principal_name = OptionValue(request, "principal_name:");
  context.requested_role_name = OptionValue(request, "requested_role_name:");
  context.active_role_name = OptionValue(request, "active_role_name:");
  if (context.active_role_name.empty()) context.active_role_name = context.requested_role_name;
  context.active_role_uuid = request.context.current_role_uuid.canonical;
  if (context.active_role_uuid.empty()) {
    context.active_role_uuid = OptionValue(request, "current_role_uuid:");
  }
  if (!context.active_role_name.empty()) {
    context.effective_role_names.push_back(context.active_role_name);
  }
  if (!context.active_role_uuid.empty()) {
    context.effective_role_uuids.push_back(context.active_role_uuid);
  }
  for (const auto& role_uuid : Split(OptionValue(request, "effective_role_uuid_set:"), ',')) {
    if (!role_uuid.empty() &&
        std::find(context.effective_role_uuids.begin(),
                  context.effective_role_uuids.end(),
                  role_uuid) == context.effective_role_uuids.end()) {
      context.effective_role_uuids.push_back(role_uuid);
    }
  }
  for (const auto& group_uuid : Split(OptionValue(request, "effective_group_uuid_set:"), ',')) {
    if (!group_uuid.empty()) context.effective_group_uuids.push_back(group_uuid);
  }
  context.visible_catalog_generation_id = request.context.catalog_generation_id;
  context.cluster_authority_available = request.context.cluster_authority_available;
  return context;
}

std::optional<EngineQueryRelation> SysInformationProjectionRelation(
    const EnginePlanOperationRequest& request,
    std::string* error_detail) {
  const std::string projection = OptionValue(request, "catalog_projection:");
  if (projection.empty()) return std::nullopt;
  const auto projection_result = BuildSysInformationProjection(
      projection,
      ProjectionContextFromRequest(request),
      std::vector<SysInformationCatalogObjectSource>{},
      std::vector<SysInformationResolverNameSource>{},
      std::vector<SysInformationCommentSource>{},
      std::vector<SysInformationDatatypeDescriptorSource>{},
      std::vector<SysInformationColumnSource>{},
      std::vector<SysInformationSettingSource>{},
      std::vector<SysInformationFrontendAgentSource>{},
      std::vector<SysInformationProtectedMaterialSource>{},
      std::vector<SysInformationProtectedMaterialVersionSource>{},
      std::vector<SysInformationAgentSource>{},
      std::vector<SysInformationAgentMetricDependencySource>{},
      std::vector<SysInformationAgentPolicySource>{},
      std::vector<SysInformationAgentActionSource>{},
      std::vector<SysInformationAgentOverrideSource>{},
      std::vector<SysInformationAgentEvidenceSource>{},
      std::vector<SysInformationAgentAuditSource>{},
      std::vector<SysInformationFilespaceCapacityAgentStateSource>{},
      std::vector<SysInformationPageAllocationAgentStateSource>{},
      std::vector<SysInformationFilespaceShrinkReadinessSource>{},
      std::vector<SysInformationDomainSource>{},
      request.ipar_agent_lifecycle,
      request.ipar_metric_counters,
      request.ipar_telemetry_controls,
      request.ipar_slow_path_reasons);
  if (!projection_result.ok) {
    if (error_detail != nullptr) {
      *error_detail = projection_result.diagnostic_detail.empty()
                          ? projection_result.diagnostic_code
                          : projection_result.diagnostic_detail;
    }
    return std::nullopt;
  }

  EngineQueryRelation relation;
  relation.relation_name = "sys_projection:" + projection;
  relation.descriptor_digest = relation.relation_name;
  relation.source_object.object_kind = "view";
  relation.source_object.uuid = request.target_object.uuid;
  if (!projection_result.rows.empty()) {
    for (const auto& [field, ignored] : projection_result.rows.front().fields) {
      (void)ignored;
      relation.columns.push_back(TextDescriptor());
    }
  }
  for (const auto& source_row : projection_result.rows) {
    EngineRowValue row;
    row.fields.reserve(source_row.fields.size());
    for (const auto& [field, value] : source_row.fields) {
      row.fields.emplace_back(field, TextValue(value));
    }
    if (ProjectionRowMatchesPredicate(row, request.predicate)) {
      relation.rows.push_back(std::move(row));
    }
  }
  return relation;
}

std::optional<std::vector<EngineQueryRelation>> BuildRelations(const EnginePlanOperationRequest& request,
                                                               std::string* error_detail) {
  std::vector<EngineQueryRelation> relations = request.relations;
  if (relations.empty() && !request.rows.empty()) {
    EngineQueryRelation relation;
    relation.relation_name = "request_rows";
    relation.descriptor_digest = "request_rows";
    relation.rows = request.rows;
    relations.push_back(std::move(relation));
  }

  const bool has_projection_source = !OptionValue(request, "catalog_projection:").empty();
  if (has_projection_source) {
    auto projection_relation = SysInformationProjectionRelation(request, error_detail);
    if (!projection_relation) return std::nullopt;
    relations.push_back(std::move(*projection_relation));
  }

  const bool has_crud_sources =
      (!has_projection_source && !request.target_object.uuid.canonical.empty()) ||
      !request.related_objects.empty();
  if (has_crud_sources) {
    if (request.context.local_transaction_id == 0) {
      *error_detail = "local_transaction_id_required_for_crud_query_sources";
      return std::nullopt;
    }
    const auto loaded = LoadQueryCrudCompatibilityState(request.context);
    if (!loaded.ok) {
      *error_detail = loaded.diagnostic.detail.empty() ? loaded.diagnostic.code : loaded.diagnostic.detail;
      return std::nullopt;
    }
    if (!has_projection_source && !request.target_object.uuid.canonical.empty()) {
      relations.push_back(CrudRelation(loaded.state,
                                       request.target_object.uuid.canonical,
                                       request.context,
                                       request.predicate));
    }
    for (const auto& object : request.related_objects) {
      if (!object.uuid.canonical.empty()) {
        relations.push_back(CrudRelation(loaded.state, object.uuid.canonical, request.context, {}));
      }
    }
  }
  return relations;
}

bool RelationsAreIntegerCompatible(const std::vector<EngineQueryRelation>& relations,
                                   std::string* error_detail) {
  for (const auto& relation : relations) {
    for (const auto& row : relation.rows) {
      for (const auto& [field, typed] : row.fields) {
        if (typed.is_null || !TryParseI64Value(typed.encoded_value, nullptr)) {
          if (error_detail != nullptr) {
            *error_detail = "query_plan_integer_executor_requires_integer_row_values";
          }
          return false;
        }
      }
    }
  }
  return true;
}

void ApplyCommonPipeline(const EnginePlanOperationRequest& request, exec::Batch* batch) {
  if (request.order_column != 0 || !request.ordering.canonical_ordering_envelopes.empty() ||
      !OptionValue(request, "order_column:").empty()) {
    const auto order_column = ParseSizeValue(OptionValue(request, "order_column:"), request.order_column);
    const bool ascending = ParseBoolValue(OptionValue(request, "order:"), request.ascending);
    *batch = exec::SortByColumn(*batch, order_column, ascending);
  }
  const auto projected = ParseProjectedColumns(request);
  if (!projected.empty()) { *batch = exec::ProjectColumns(*batch, projected); }
  const auto offset = ParseSizeValue(OptionValue(request, "offset:"), static_cast<std::size_t>(request.offset));
  const auto limit = ParseSizeValue(OptionValue(request, "limit:"), static_cast<std::size_t>(request.limit));
  if (limit != 0 || offset != 0) {
    *batch = exec::LimitOffset(*batch, limit == 0 ? batch->rows.size() : limit, offset);
  }
}

std::string QueryOperation(const EnginePlanOperationRequest& request) {
  if (!request.query_operation.empty()) { return LowerAscii(request.query_operation); }
  std::string value = OptionValue(request, "query_operation:");
  if (value.empty()) { value = OptionValue(request, "query_shape:"); }
  if (!value.empty()) { return LowerAscii(value); }
  return "scan";
}

bool ApplyAggregateHavingFilter(const EnginePlanOperationRequest& request,
                                exec::Batch* batch,
                                std::vector<EngineEvidenceReference>* evidence,
                                std::string* error_detail) {
  const std::string predicate = LowerAscii(OptionValue(request, "having_predicate:"));
  if (predicate.empty()) { return true; }
  exec::Int64ComparisonOperator comparison = exec::Int64ComparisonOperator::kGreaterThan;
  std::string normalized_predicate = predicate;
  if (predicate == "aggregate_gt" || predicate == "gt" || predicate == ">") {
    comparison = exec::Int64ComparisonOperator::kGreaterThan;
    normalized_predicate = "aggregate_gt";
  } else if (predicate == "aggregate_gte" || predicate == "aggregate_ge" ||
             predicate == "gte" || predicate == "ge" || predicate == ">=") {
    comparison = exec::Int64ComparisonOperator::kGreaterThanOrEqual;
    normalized_predicate = "aggregate_gte";
  } else if (predicate == "aggregate_lt" || predicate == "lt" || predicate == "<") {
    comparison = exec::Int64ComparisonOperator::kLessThan;
    normalized_predicate = "aggregate_lt";
  } else if (predicate == "aggregate_lte" || predicate == "aggregate_le" ||
             predicate == "lte" || predicate == "le" || predicate == "<=") {
    comparison = exec::Int64ComparisonOperator::kLessThanOrEqual;
    normalized_predicate = "aggregate_lte";
  } else if (predicate == "aggregate_eq" || predicate == "eq" || predicate == "=" || predicate == "==") {
    comparison = exec::Int64ComparisonOperator::kEqual;
    normalized_predicate = "aggregate_eq";
  } else if (predicate == "aggregate_ne" || predicate == "aggregate_neq" ||
             predicate == "ne" || predicate == "neq" || predicate == "!=" || predicate == "<>") {
    comparison = exec::Int64ComparisonOperator::kNotEqual;
    normalized_predicate = "aggregate_ne";
  } else {
    if (error_detail != nullptr) { *error_detail = "query_plan_aggregate_having_predicate_unsupported"; }
    return false;
  }
  std::int64_t threshold = 0;
  if (!TryParseI64Value(OptionValue(request, "having_threshold:"), &threshold)) {
    if (error_detail != nullptr) { *error_detail = "query_plan_aggregate_having_threshold_invalid"; }
    return false;
  }
  const std::size_t value_column = ParseSizeValue(OptionValue(request, "having_value_column:"), 1);
  *batch = exec::FilterByInt64Comparison(*batch, value_column, comparison, threshold);
  if (evidence != nullptr) {
    evidence->push_back({"query_aggregate_having_predicate", normalized_predicate});
    evidence->push_back({"query_aggregate_having_threshold", std::to_string(threshold)});
    evidence->push_back({"query_aggregate_having_value_column", std::to_string(value_column)});
    evidence->push_back({"query_aggregate_having_filter_after_grouping", "true"});
  }
  return true;
}

bool NormalizeHavingPredicate(const EnginePlanOperationRequest& request,
                              std::string* normalized_predicate,
                              double* threshold,
                              std::size_t* value_column,
                              std::string* error_detail) {
  const std::string predicate = LowerAscii(OptionValue(request, "having_predicate:"));
  if (predicate.empty()) return false;
  if (predicate == "aggregate_gt" || predicate == "gt" || predicate == ">") {
    *normalized_predicate = "aggregate_gt";
  } else if (predicate == "aggregate_gte" || predicate == "aggregate_ge" ||
             predicate == "gte" || predicate == "ge" || predicate == ">=") {
    *normalized_predicate = "aggregate_gte";
  } else if (predicate == "aggregate_lt" || predicate == "lt" || predicate == "<") {
    *normalized_predicate = "aggregate_lt";
  } else if (predicate == "aggregate_lte" || predicate == "aggregate_le" ||
             predicate == "lte" || predicate == "le" || predicate == "<=") {
    *normalized_predicate = "aggregate_lte";
  } else if (predicate == "aggregate_eq" || predicate == "eq" || predicate == "=" || predicate == "==") {
    *normalized_predicate = "aggregate_eq";
  } else if (predicate == "aggregate_ne" || predicate == "aggregate_neq" ||
             predicate == "ne" || predicate == "neq" || predicate == "!=" || predicate == "<>") {
    *normalized_predicate = "aggregate_ne";
  } else {
    if (error_detail != nullptr) *error_detail = "query_plan_aggregate_having_predicate_unsupported";
    return true;
  }
  if (!TryParseReal64Value(OptionValue(request, "having_threshold:"), threshold)) {
    if (error_detail != nullptr) *error_detail = "query_plan_aggregate_having_threshold_invalid";
    return true;
  }
  *value_column = ParseSizeValue(OptionValue(request, "having_value_column:"), 1);
  return true;
}

bool HavingComparisonPasses(double value,
                            std::string_view normalized_predicate,
                            double threshold) {
  if (normalized_predicate == "aggregate_gt") return value > threshold;
  if (normalized_predicate == "aggregate_gte") return value >= threshold;
  if (normalized_predicate == "aggregate_lt") return value < threshold;
  if (normalized_predicate == "aggregate_lte") return value <= threshold;
  if (normalized_predicate == "aggregate_eq") return value == threshold;
  if (normalized_predicate == "aggregate_ne") return value != threshold;
  return false;
}

bool ApplyTypedAggregateHavingFilter(const EnginePlanOperationRequest& request,
                                     EngineResultShape* shape,
                                     std::vector<EngineEvidenceReference>* evidence,
                                     std::string* error_detail) {
  if (shape == nullptr) return true;
  std::string normalized_predicate;
  double threshold = 0.0;
  std::size_t value_column = 1;
  const bool predicate_present = NormalizeHavingPredicate(request,
                                                         &normalized_predicate,
                                                         &threshold,
                                                         &value_column,
                                                         error_detail);
  if (!predicate_present) return true;
  if (error_detail != nullptr && !error_detail->empty()) return false;

  std::vector<EngineRowValue> filtered;
  filtered.reserve(shape->rows.size());
  for (const auto& row : shape->rows) {
    if (value_column >= row.fields.size()) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_having_value_column_out_of_range";
      return false;
    }
    const auto typed = NormalizeTypedValue(row.fields[value_column].second);
    if (typed.is_null) continue;
    double parsed = 0.0;
    if (!TryParseReal64Value(typed.encoded_value, &parsed)) {
      if (error_detail != nullptr) *error_detail = "query_plan_aggregate_having_numeric_input_required";
      return false;
    }
    if (HavingComparisonPasses(parsed, normalized_predicate, threshold)) {
      filtered.push_back(row);
    }
  }
  shape->rows = std::move(filtered);
  if (evidence != nullptr) {
    evidence->push_back({"query_aggregate_having_predicate", normalized_predicate});
    evidence->push_back({"query_aggregate_having_threshold", FormatReal64(threshold)});
    evidence->push_back({"query_aggregate_having_value_column", std::to_string(value_column)});
    evidence->push_back({"query_aggregate_having_filter_after_grouping", "true"});
  }
  return true;
}

bool IsSetOperationName(const std::string& operation) {
  return operation == "set_operation" || operation == "union" || operation == "union_distinct" ||
         operation == "union_all" || operation == "intersect" ||
         operation == "intersect_distinct" || operation == "intersect_all" ||
         operation == "except" || operation == "except_distinct" || operation == "except_all";
}

plan::PhysicalAccessKind AccessKindForQueryOperation(const std::string& operation) {
  if (operation == "join" || operation == "inner_join" || operation == "equi_join" ||
      operation == "left_join" || operation == "left_outer_join" ||
      operation == "semi_join" || operation == "join_group_sum_assertion" ||
      operation == "join_window_max_assertion") {
    return plan::PhysicalAccessKind::kJoinHash;
  }
  if (operation == "aggregate" || operation == "aggregate_scalar" ||
      operation == "scalar_aggregate" || operation == "group" || operation == "group_by" ||
      operation == "count" || operation == "count_all") {
    return plan::PhysicalAccessKind::kAggregateHash;
  }
  if (operation == "window" || operation == "row_number_window" ||
      operation == "partition_count_window" ||
      operation == "lag_window" || operation == "lead_window" ||
      operation == "first_value_window" || operation == "last_value_window" ||
      operation == "ntile_window" || operation == "percent_rank_window" ||
      operation == "cume_dist_window" || operation == "nth_value_window") {
    return plan::PhysicalAccessKind::kSortThenWindow;
  }
  if (operation == "cte" || operation == "materialized_cte" || operation == "recursive_cte") {
    return plan::PhysicalAccessKind::kCteMaterialize;
  }
  if (IsSetOperationName(operation)) {
    return plan::PhysicalAccessKind::kSetOperation;
  }
  if (operation == "filter_gt" || operation == "scan") {
    return plan::PhysicalAccessKind::kTableScan;
  }
  if (operation == "subquery" || operation == "scalar_subquery" || operation == "correlated_subquery") {
    return plan::PhysicalAccessKind::kCteMaterialize;
  }
  return plan::PhysicalAccessKind::kTableScan;
}

plan::LogicalPlan BuildExecutableLogicalPlan(const EnginePlanOperationRequest& request,
                                             const std::string& operation,
                                             const std::vector<EngineQueryRelation>& relations,
                                             const opt::OptimizerStatisticsCatalog* statistics = nullptr,
                                             std::vector<EngineEvidenceReference>* evidence = nullptr) {
  std::optional<opt::OptimizerStatisticsCatalog> owned_statistics;
  if (statistics == nullptr) {
    owned_statistics = BuildRuntimeOptimizerStatistics(request, relations);
    statistics = &*owned_statistics;
  }
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "engine.query.plan_operation:" + operation;
  const auto access_kind = AccessKindForQueryOperation(operation);
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        PlannedAccessKindForRequest(request,
                                                                    operation,
                                                                    relations,
                                                                    *statistics,
                                                                    evidence),
                                        "query.plan_operation",
                                        operation);
  const bool join_operation = operation == "join" || operation == "inner_join" ||
                              operation == "equi_join" || operation == "left_join" ||
                              operation == "left_outer_join" || operation == "semi_join" ||
                              operation == "join_group_sum_assertion" ||
                              operation == "join_window_max_assertion";
  if (!join_operation && !request.target_object.uuid.canonical.empty()) {
    node.required_object_uuids.push_back(request.target_object.uuid.canonical);
  }
  for (const auto& object : request.related_objects) {
    if (join_operation) break;
    if (!object.uuid.canonical.empty()) { node.required_object_uuids.push_back(object.uuid.canonical); }
  }
  for (const auto& relation : relations) {
    if (!relation.descriptor_digest.empty()) { node.required_descriptors.push_back(relation.descriptor_digest); }
    if (!relation.source_object.uuid.canonical.empty()) {
      node.required_object_uuids.push_back(relation.source_object.uuid.canonical);
    }
  }
  if (IsExecutableUpperAccessKind(access_kind)) {
    std::size_t relation_index = 0;
    for (const auto& relation : relations) {
      std::string stable_name = relation.relation_name.empty()
          ? "query_input_" + std::to_string(relation_index)
          : relation.relation_name;
      auto input = plan::MakeLogicalPlanNode(
          plan::LogicalPlanNodeKind::kDmlRead,
          plan::PhysicalAccessKind::kTableScan,
          "query.plan_operation.input." + std::to_string(relation_index),
          stable_name);
      input.required_descriptors.push_back(
          relation.descriptor_digest.empty() ? stable_name : relation.descriptor_digest);
      if (!relation.source_object.uuid.canonical.empty()) {
        input.required_object_uuids.push_back(relation.source_object.uuid.canonical);
      }
      logical.nodes.push_back(std::move(input));
      ++relation_index;
    }
  }
  logical.nodes.push_back(std::move(node));
  return logical;
}

bool AttachOptimizerSelectionEvidence(const plan::LogicalPlan& logical,
                                      const opt::OptimizerStatisticsCatalog& statistics,
                                      std::vector<EngineEvidenceReference>* evidence,
                                      std::string* error_detail,
                                      plan::PhysicalAccessKind* selected_access = nullptr) {
  const auto optimized = opt::OptimizeLogicalPlanWithStatistics(logical, statistics);
  evidence->push_back({"optimizer_profile", optimized.optimizer_profile});
  evidence->push_back({"logical_plan_id", logical.plan_id});
  std::size_t emitted_stats = 0;
  for (const auto& statistic : statistics.Statistics()) {
    if (!statistic.available) {
      if (statistic.cluster_only) {
        evidence->push_back({"optimizer_metric_unavailable", statistic.statistic_name + ":" + statistic.object_uuid + ":" + opt::StatisticSourceName(statistic.source)});
      }
      continue;
    }
    if (emitted_stats >= 12) { continue; }
    evidence->push_back({"optimizer_metric_input", statistic.statistic_name + ":" + statistic.object_uuid + ":" + opt::StatisticSourceName(statistic.source)});
    ++emitted_stats;
  }
  if (!optimized.ok) {
    *error_detail = optimized.diagnostics.empty() ? "optimizer_no_selectable_plan" : optimized.diagnostics.front();
    return false;
  }
  std::size_t emitted_candidates = 0;
  for (const auto& candidate : optimized.candidates) {
    if (emitted_candidates >= 8) break;
    evidence->push_back({"optimizer_candidate",
                         candidate.plan_candidate.candidate_id + ":" +
                             plan::PhysicalAccessKindName(candidate.plan_candidate.access_kind) +
                             ":rows=" + std::to_string(candidate.plan_candidate.estimated_rows) +
                             ":cost=" + std::to_string(candidate.cost.total_cost) +
                             ":selectable=" + (candidate.cost.selectable ? "true" : "false")});
    if (!candidate.rejection_reason.empty()) {
      evidence->push_back({"optimizer_candidate_rejected",
                           candidate.plan_candidate.candidate_id + ":" + candidate.rejection_reason});
    }
    ++emitted_candidates;
  }
  if (optimized.has_physical_plan) {
    const auto validation = opt::ValidatePhysicalPlanNode(optimized.physical_root);
    if (!validation.ok) {
      *error_detail = validation.diagnostics.empty() ? "optimizer_physical_plan_invalid" : validation.diagnostics.front();
      return false;
    }
    evidence->push_back({"optimizer_selected_candidate",
                         optimized.selected_primary_candidate_id.empty()
                             ? optimized.physical_root.node_id
                             : optimized.selected_primary_candidate_id});
    evidence->push_back({"optimizer_selected_access",
                         plan::PhysicalAccessKindName(optimized.physical_root.access_kind)});
    evidence->push_back({"optimizer_executor_capability", optimized.physical_root.executor_capability_id});
    evidence->push_back({"optimizer_physical_root", optimized.physical_root.node_id});
    auto selected_candidate = std::find_if(optimized.candidates.begin(),
                                           optimized.candidates.end(),
                                           [&](const opt::OptimizerCandidate& candidate) {
                                             return candidate.plan_candidate.candidate_id ==
                                                    optimized.selected_primary_candidate_id;
                                           });
    if (selected_candidate == optimized.candidates.end()) {
      selected_candidate = std::find_if(optimized.candidates.begin(),
                                        optimized.candidates.end(),
                                        [](const opt::OptimizerCandidate& candidate) {
                                          return candidate.selected;
                                        });
    }
    if (selected_candidate == optimized.candidates.end() && !optimized.candidates.empty()) {
      selected_candidate = optimized.candidates.begin();
    }
    if (selected_candidate != optimized.candidates.end()) {
      evidence->push_back({"optimizer_statistics_version", selected_candidate->statistics_version});
    }
    if (selected_access != nullptr) {
      *selected_access = optimized.physical_root.access_kind;
    }
    return true;
  }
  for (const auto& candidate : optimized.candidates) {
    if (!candidate.selected) { continue; }
    const auto capability = opt::RequiredExecutorCapabilityForAccessKind(candidate.plan_candidate.access_kind);
    const auto physical = opt::PhysicalPlanNodeFromCandidate(candidate.plan_candidate,
                                                             capability,
                                                             candidate.node.required_descriptors.empty()
                                                                 ? candidate.node.stable_name
                                                                 : candidate.node.required_descriptors.front());
    const auto validation = opt::ValidatePhysicalPlanNode(physical);
    if (!validation.ok) {
      *error_detail = validation.diagnostics.empty() ? "optimizer_physical_plan_invalid" : validation.diagnostics.front();
      return false;
    }
    evidence->push_back({"optimizer_selected_candidate", candidate.plan_candidate.candidate_id});
    evidence->push_back({"optimizer_selected_access", plan::PhysicalAccessKindName(candidate.plan_candidate.access_kind)});
    evidence->push_back({"optimizer_executor_capability", capability});
    evidence->push_back({"optimizer_statistics_version", candidate.statistics_version});
    if (selected_access != nullptr) {
      *selected_access = candidate.plan_candidate.access_kind;
    }
    return true;
  }
  *error_detail = "optimizer_selected_candidate_missing";
  return false;
}

exec::Batch ExecuteQueryBatch(const EnginePlanOperationRequest& request,
                              const std::vector<EngineQueryRelation>& relations,
                              const std::string& operation,
                              std::vector<EngineEvidenceReference>* evidence,
                              std::string* error_detail,
                              std::string planned_join_algorithm = {}) {
  const auto require = [&](std::size_t index) -> exec::Batch {
    if (index >= relations.size()) { return exec::MakeBatch("missing_relation", {}); }
    return RelationToBatch(relations[index]);
  };

  exec::Batch batch = require(0);
  if (operation == "join" || operation == "inner_join" || operation == "equi_join" ||
      operation == "left_join" || operation == "left_outer_join" ||
      operation == "semi_join" || operation == "join_group_sum_assertion" ||
      operation == "join_window_max_assertion") {
    const exec::Batch right = require(1);
    const std::size_t left_key_column =
        relations.empty() ? request.left_key_column
                          : ColumnIndexForRelation(relations[0], request.left_key_field, request.left_key_column);
    const std::size_t right_key_column =
        relations.size() < 2 ? request.right_key_column
                             : ColumnIndexForRelation(relations[1], request.right_key_field, request.right_key_column);
    evidence->push_back({"query_join_left_key_column", std::to_string(left_key_column)});
    evidence->push_back({"query_join_right_key_column", std::to_string(right_key_column)});
    if (!batch.rows.empty() && left_key_column < batch.rows.front().values.size()) {
      evidence->push_back({"query_join_left_key_sample", std::to_string(batch.rows.front().values[left_key_column])});
    }
    if (!right.rows.empty() && right_key_column < right.rows.front().values.size()) {
      evidence->push_back({"query_join_right_key_sample", std::to_string(right.rows.front().values[right_key_column])});
    }
    std::string algorithm = LowerAscii(!request.join_algorithm.empty()
                                           ? request.join_algorithm
                                           : OptionValue(request, "join_algorithm:"));
    if (algorithm.empty()) algorithm = std::move(planned_join_algorithm);
    if (algorithm == "nested" || algorithm == "nested_loop") {
      batch = exec::NestedLoopJoinEqual(batch, right, left_key_column, right_key_column);
      evidence->push_back({"query_join_algorithm", "nested_loop"});
    } else if (algorithm == "merge") {
      batch = exec::MergeJoinEqual(batch, right, left_key_column, right_key_column);
      evidence->push_back({"query_join_algorithm", "merge"});
    } else {
      batch = exec::HashJoinEqual(batch, right, left_key_column, right_key_column);
      evidence->push_back({"query_join_algorithm", "hash"});
    }
    evidence->push_back({"query_join_key_binding", request.left_key_field.empty() ? "ordinal" : "descriptor_field"});
  } else if (operation == "aggregate" || operation == "group" || operation == "group_by") {
    std::string aggregate_function = request.aggregate_function;
    if (aggregate_function.empty()) aggregate_function = OptionValue(request, "aggregate_function:");
    if (aggregate_function.empty()) aggregate_function = OptionValue(request, "having_aggregate_function:");
    if (aggregate_function.empty()) aggregate_function = "sum";
    const std::string aggregate_leaf = AggregateFunctionLeaf(aggregate_function);
    if (aggregate_leaf != "sum") {
      if (error_detail != nullptr) { *error_detail = "query_plan_aggregate_function_unsupported"; }
      return exec::MakeBatch("query_plan_aggregate_invalid", {});
    }
    const std::size_t group_key_column =
        relations.empty() ? request.group_key_column
                          : ColumnIndexForRelation(relations[0], request.group_key_field, request.group_key_column);
    const std::size_t aggregate_value_column =
        relations.empty()
            ? request.aggregate_value_column
            : ColumnIndexForRelation(relations[0], request.aggregate_value_field, request.aggregate_value_column);
    evidence->push_back({"query_aggregate_group_key_column", std::to_string(group_key_column)});
    evidence->push_back({"query_aggregate_value_column", std::to_string(aggregate_value_column)});
    evidence->push_back({"query_aggregate_key_binding",
                         request.group_key_field.empty() ? "ordinal" : "descriptor_field"});
    evidence->push_back({"query_aggregate_value_binding",
                         request.aggregate_value_field.empty() ? "ordinal" : "descriptor_field"});
    batch = exec::AggregateSumByKey(batch, group_key_column, aggregate_value_column);
    evidence->push_back({"query_aggregate", "sum_by_key"});
    evidence->push_back({"query_aggregate_function_requested", aggregate_leaf});
    if (!ApplyAggregateHavingFilter(request, &batch, evidence, error_detail)) {
      return exec::MakeBatch("query_plan_aggregate_having_invalid", {});
    }
  } else if (operation == "window" || operation == "row_number_window" ||
             operation == "partition_count_window" ||
             operation == "lag_window" || operation == "lead_window" ||
             operation == "first_value_window" || operation == "last_value_window" ||
             operation == "ntile_window" || operation == "percent_rank_window" ||
             operation == "cume_dist_window" || operation == "nth_value_window") {
    const std::size_t order_column =
        relations.empty() ? request.order_column
                          : ColumnIndexForRelation(relations[0], request.order_field, request.order_column);
    const std::size_t partition_column =
        relations.empty()
            ? request.partition_key_column
            : ColumnIndexForRelation(relations[0], request.partition_key_field, request.partition_key_column);
    const std::size_t value_column =
        relations.empty()
            ? request.window_value_column
            : ColumnIndexForRelation(relations[0], request.window_value_field, request.window_value_column);
    std::string function = LowerAscii(request.window_function);
    if (function.empty() || function == "row_number_window") function = "row_number";
    if (operation == "partition_count_window") function = "count_star_partition";
    if (operation == "lag_window") function = "lag";
    if (operation == "lead_window") function = "lead";
    if (operation == "first_value_window") function = "first_value";
    if (operation == "last_value_window") function = "last_value";
    if (operation == "ntile_window") function = "ntile";
    if (operation == "percent_rank_window") function = "percent_rank";
    if (operation == "cume_dist_window") function = "cume_dist";
    if (operation == "nth_value_window") function = "nth_value";
    evidence->push_back({"query_window_order_column", std::to_string(order_column)});
    evidence->push_back({"query_window_value_column", std::to_string(value_column)});
    evidence->push_back({"query_window_binding",
                         request.order_field.empty() ? "ordinal" : "descriptor_field"});
    if (function == "count_star_partition") {
      evidence->push_back({"query_window_partition_column", std::to_string(partition_column)});
      evidence->push_back({"query_window_partition_binding",
                           request.partition_key_field.empty() ? "ordinal" : "descriptor_field"});
      batch = exec::AddPartitionCountWindow(batch, partition_column);
    } else if (function == "rank") {
      batch = exec::AddRankWindow(batch, order_column);
    } else if (function == "dense_rank") {
      batch = exec::AddDenseRankWindow(batch, order_column);
    } else if (function == "ntile") {
      batch = exec::AddNtileWindow(batch, order_column, static_cast<std::int64_t>(request.window_n));
    } else if (function == "lag") {
      batch = exec::AddLagWindow(batch, order_column, value_column);
    } else if (function == "lead") {
      batch = exec::AddLeadWindow(batch, order_column, value_column);
    } else if (function == "first_value") {
      batch = exec::AddFirstValueWindow(batch, order_column, value_column);
    } else if (function == "last_value") {
      batch = exec::AddLastValueWindow(batch, order_column, value_column);
    } else {
      function = "row_number";
      batch = exec::AddRowNumberWindow(batch, order_column);
    }
    evidence->push_back({"query_window", function});
  } else if (operation == "cte" || operation == "materialized_cte") {
    batch = exec::MaterializeCte(batch);
    evidence->push_back({"query_cte", "materialized"});
  } else if (operation == "recursive_cte") {
    const auto max_iterations = ParseSizeValue(OptionValue(request, "recursive_iterations:"), 32);
    const std::string step_mode = LowerAscii(OptionValue(request, "recursive_step_mode:"));
    if (step_mode == "counter_add_until_lt") {
      std::int64_t step = 0;
      std::int64_t limit = 0;
      if (!TryParseI64Value(OptionValue(request, "recursive_counter_step:"), &step) ||
          !TryParseI64Value(OptionValue(request, "recursive_counter_limit:"), &limit) ||
          step <= 0) {
        if (error_detail != nullptr) {
          *error_detail = "query_plan_recursive_counter_descriptor_invalid";
        }
        return exec::MakeBatch("query_plan_recursive_counter_invalid", {});
      }
      std::vector<exec::Tuple> rows = batch.rows;
      for (const auto& anchor : batch.rows) {
        if (anchor.values.empty()) {
          if (error_detail != nullptr) {
            *error_detail = "query_plan_recursive_counter_anchor_column_missing";
          }
          return exec::MakeBatch("query_plan_recursive_counter_invalid", {});
        }
        std::int64_t current = anchor.values.front();
        std::size_t iteration = 0;
        while (iteration++ < max_iterations && current < limit) {
          current += step;
          rows.push_back(exec::Tuple{{current}});
        }
      }
      batch = exec::MakeBatch(batch.descriptor_digest, std::move(rows));
      evidence->push_back({"query_cte_recursive_step", "counter_add_until_lt"});
      evidence->push_back({"query_cte_recursive_counter_step", std::to_string(step)});
      evidence->push_back({"query_cte_recursive_counter_limit", std::to_string(limit)});
    } else if (relations.size() >= 2) {
      const exec::Batch recursive_step = RelationToBatch(relations[1]);
      std::size_t previous_count = 0;
      std::size_t iteration = 0;
      while (iteration++ < max_iterations && batch.rows.size() != previous_count) {
        previous_count = batch.rows.size();
        batch = exec::SetUnionDistinct(batch, recursive_step);
      }
      evidence->push_back({"query_cte_recursive_step", "values_union_distinct"});
    }
    batch = exec::MaterializeCte(batch);
    evidence->push_back({"query_cte", "recursive_fixed_point_materialized"});
  } else if (operation == "subquery" || operation == "scalar_subquery" || operation == "correlated_subquery") {
    const std::int64_t value = exec::ScalarSubqueryFirstValue(batch, request.projected_columns.empty() ? 0 : request.projected_columns.front());
    batch = exec::MakeBatch("scalar_subquery", {{{value}}});
    evidence->push_back({"query_subquery", operation});
  } else if (operation == "filter_gt") {
    const auto threshold = ParseI64Value(OptionValue(request, "threshold:"));
    batch = exec::FilterGreaterThan(batch, request.left_key_column, threshold);
    evidence->push_back({"query_filter", "greater_than"});
  } else if (operation == "sample" || operation == "table_sample") {
    const std::string method = LowerAscii(OptionValue(request, "sample_method:").empty()
                                              ? "bernoulli"
                                              : OptionValue(request, "sample_method:"));
    const double percent = ParseReal64Value(OptionValue(request, "sample_percent:"), 100.0);
    if ((method != "bernoulli" && method != "system") ||
        percent < 0.0 || percent > 100.0 || !std::isfinite(percent)) {
      if (error_detail != nullptr) *error_detail = "query_plan_sample_descriptor_invalid";
      return exec::MakeBatch("query_plan_sample_invalid", {});
    }
    const std::size_t before_count = batch.rows.size();
    std::size_t keep_count = before_count;
    if (percent <= 0.0) {
      keep_count = 0;
    } else if (percent < 100.0) {
      keep_count = static_cast<std::size_t>(
          std::floor((static_cast<double>(before_count) * percent) / 100.0));
      if (keep_count == 0 && before_count != 0) keep_count = 1;
    }
    if (keep_count < batch.rows.size()) batch.rows.resize(keep_count);
    evidence->push_back({"query_sample", method});
    evidence->push_back({"query_sample_percent", FormatReal64(percent)});
    evidence->push_back({"query_sample_descriptor", "engine_row_descriptor_sample_route"});
    evidence->push_back({"query_sample_rows_before", std::to_string(before_count)});
    evidence->push_back({"query_sample_rows_after", std::to_string(batch.rows.size())});
  } else if (IsSetOperationName(operation)) {
    const bool set_by_name = request.set_by_name ||
                             ParseBoolValue(OptionValue(request, "set_by_name:"), false);
    const std::string set_operation = operation == "set_operation"
        ? LowerAscii(!request.set_operation.empty() ? request.set_operation : OptionValue(request, "set_operation:"))
        : operation;
    const std::string left_project_field = OptionValue(request, "left_project_field:");
    const std::string right_project_field = OptionValue(request, "right_project_field:");
    const auto projected_relation = [&](std::size_t relation_index) -> exec::Batch {
      std::string project_field =
          OptionValue(request, "relation_" + std::to_string(relation_index) + "_project_field:");
      if (project_field.empty() && relation_index == 0) project_field = left_project_field;
      if (project_field.empty() && relation_index == 1) project_field = right_project_field;
      if (project_field.empty() && !left_project_field.empty()) project_field = left_project_field;
      if (!project_field.empty() && relation_index < relations.size()) {
        evidence->push_back({"query_set_relation_" + std::to_string(relation_index) + "_project_field",
                             project_field});
        return RelationToBatchByName(relations[relation_index], {project_field});
      }
      if (set_by_name && relation_index < relations.size()) {
        return RelationToBatchByName(relations[relation_index], FieldOrderForRelation(relations[0]));
      }
      return require(relation_index);
    };
    batch = projected_relation(0);
    evidence->push_back({"query_set_binding", set_by_name ? "descriptor_name" : "ordinal"});
    const auto apply_set_operation = [&](const exec::Batch& right) {
      if (set_operation == "intersect_all") {
        batch = exec::SetIntersectAll(batch, right);
      } else if (set_operation == "except_all") {
        batch = exec::SetExceptAll(batch, right);
      } else if (set_operation == "union_all") {
        batch = exec::SetUnionAll(batch, right);
      } else if (set_operation == "intersect" || set_operation == "intersect_distinct") {
        batch = exec::SetIntersectDistinct(batch, right);
      } else if (set_operation == "except" || set_operation == "except_distinct") {
        batch = exec::SetExceptDistinct(batch, right);
      } else {
        batch = exec::SetUnionDistinct(batch, right);
      }
    };
    for (std::size_t relation_index = 1; relation_index < relations.size(); ++relation_index) {
      apply_set_operation(projected_relation(relation_index));
    }
    if (set_operation == "intersect_all") {
      evidence->push_back({"query_set_operation", "intersect_all"});
    } else if (set_operation == "except_all") {
      evidence->push_back({"query_set_operation", "except_all"});
    } else if (set_operation == "union_all") {
      evidence->push_back({"query_set_operation", "union_all"});
    } else if (set_operation == "intersect" || set_operation == "intersect_distinct") {
      evidence->push_back({"query_set_operation", "intersect_distinct"});
    } else if (set_operation == "except" || set_operation == "except_distinct") {
      evidence->push_back({"query_set_operation", "except_distinct"});
    } else {
      evidence->push_back({"query_set_operation", "union_distinct"});
    }
  } else {
    evidence->push_back({"query_scan", "local_rowset"});
  }
  ApplyCommonPipeline(request, &batch);
  return batch;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_PLAN_API_BEHAVIOR
EnginePlanOperationResult EnginePlanOperationUncachedImpl(const EnginePlanOperationRequest& request) {
  auto result = MakeApiBehaviorSuccess<EnginePlanOperationResult>(request.context, "query.plan_operation");
  AddSbsfc081Evidence(&result, request);
  AddSbsfc082Evidence(&result, request);
  AddSbsfc083Evidence(&result, request);
  AddSbsfc084Evidence(&result, request);
  AddSbsfc085Evidence(&result, request);
  if (request.execute || OptionValue(request, "execute:") == "true") {
    std::string error_detail;
    const auto relations = BuildRelations(request, &error_detail);
    if (!relations) { return QueryFailure<EnginePlanOperationResult>(request.context, error_detail); }
    if (relations->empty()) { return QueryFailure<EnginePlanOperationResult>(request.context, "query_relation_required"); }
    const std::string operation = QueryOperation(request);
    const bool set_by_name = request.set_by_name ||
                             ParseBoolValue(OptionValue(request, "set_by_name:"), false);
    if (IsSetOperationName(operation) && set_by_name &&
        !RelationsAreNameAligned(*relations, &error_detail)) {
      return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
    }
    if (operation == "values" || operation == "values_rows" || operation == "values_rowset") {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      result.evidence.push_back({"query_values_rowset", std::to_string(relations->front().rows.size())});
      result.plan_kind = "values";
      result.output_row_count = relations->front().rows.size();
      result.result_shape = ValuesRowsToResultShape(relations->front().rows);
      AddApiBehaviorEvidence(&result, "query_execution", "values");
      return result;
    }
    if (operation == "recursive_cte") {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      auto batch = ExecuteQueryBatch(request,
                                     *relations,
                                     operation,
                                     &result.evidence,
                                     &error_detail);
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      const auto validation = exec::ValidateBatch(batch);
      if (!validation.ok) {
        return QueryFailure<EnginePlanOperationResult>(request.context,
                                                       validation.diagnostic_code);
      }
      const std::string result_projection = LowerAscii(OptionValue(request, "result_projection:"));
      if (result_projection == "aggregate_assertion") {
        const std::string aggregate_function =
            AggregateFunctionLeaf(OptionValue(request, "aggregate_function:"));
        const auto actual_value =
            EvaluateRecursiveCteAggregateAssertion(batch, aggregate_function, &error_detail);
        if (!actual_value) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.result_shape = NumericAssertionResultShape(request, *actual_value, &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.plan_kind = operation;
        result.output_row_count = result.result_shape.rows.size();
        AddApiBehaviorEvidence(&result, "query_execution", operation);
        result.evidence.push_back({"query_cte_result_projection", "aggregate_assertion"});
        result.evidence.push_back({"query_cte_aggregate_function", aggregate_function});
        result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
        result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
        return result;
      }
      result.plan_kind = operation;
      result.output_row_count = batch.rows.size();
      result.result_shape = BatchToResultShape(batch);
      AddApiBehaviorEvidence(&result, "query_execution", operation);
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(batch.rows.size())});
      return result;
    }
    if (operation == "pivot" || operation == "table_pivot") {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      const auto statistics = BuildRuntimeOptimizerStatistics(request, *relations);
      const auto logical = BuildExecutableLogicalPlan(request, operation, *relations, &statistics, &result.evidence);
      if (!AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail)) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.result_shape = PivotResultShape(request, relations->front(), &error_detail);
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.plan_kind = "pivot";
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", "pivot");
      std::string pivot_aggregate = OptionValue(request, "pivot_aggregate_function:");
      if (pivot_aggregate.empty()) pivot_aggregate = OptionValue(request, "aggregate_function:");
      if (pivot_aggregate.empty()) pivot_aggregate = "sum";
      const std::string pivot_aggregate_leaf = AggregateFunctionLeaf(pivot_aggregate);
      result.evidence.push_back({"query_pivot", pivot_aggregate_leaf + "_by_for_values"});
      result.evidence.push_back({"query_pivot_descriptor", "engine_row_descriptor_pivot_route"});
      result.evidence.push_back({"query_pivot_aggregate", pivot_aggregate_leaf});
      result.evidence.push_back({"query_pivot_for_field", OptionValue(request, "pivot_for_field:")});
      result.evidence.push_back({"query_pivot_in_items",
                                 std::to_string(SplitOptionList(request, "pivot_in_values:").size())});
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    if (operation == "unpivot" || operation == "table_unpivot") {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      const auto statistics = BuildRuntimeOptimizerStatistics(request, *relations);
      const auto logical = BuildExecutableLogicalPlan(request, operation, *relations, &statistics, &result.evidence);
      if (!AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail)) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.result_shape = UnpivotResultShape(request, relations->front(), &error_detail);
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.plan_kind = "unpivot";
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", "unpivot");
      result.evidence.push_back({"query_unpivot", "columns_to_rows"});
      result.evidence.push_back({"query_unpivot_descriptor", "engine_row_descriptor_unpivot_route"});
      result.evidence.push_back({"query_unpivot_value_columns",
                                 OptionValue(request, "unpivot_value_column_count:").empty()
                                     ? "1"
                                     : OptionValue(request, "unpivot_value_column_count:")});
      result.evidence.push_back({"query_unpivot_in_items",
                                 std::to_string(SplitOptionList(request, "unpivot_value_fields:").size())});
      result.evidence.push_back({"query_unpivot_pivot_field",
                                 OptionValue(request, "unpivot_pivot_column_field:")});
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    if (operation == "count" || operation == "count_all") {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      const auto statistics = BuildRuntimeOptimizerStatistics(request, *relations);
      const auto logical = BuildExecutableLogicalPlan(request, operation, *relations, &statistics, &result.evidence);
      if (!AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail)) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      const bool count_all =
          ParseBoolValue(OptionValue(request, "count_all:"), request.aggregate_value_field.empty());
      const std::string result_projection = LowerAscii(OptionValue(request, "result_projection:"));
      if (result_projection == "count_assertion") {
        const auto count = CountRelationRows(request, relations->front(), &error_detail);
        if (!count) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.result_shape = CountAssertionResultShape(request, *count, &error_detail);
        result.evidence.push_back({"query_count_result_projection", "count_assertion"});
      } else {
        result.result_shape = CountResultShape(request, relations->front(), &error_detail);
      }
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.plan_kind = operation;
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", operation);
      result.evidence.push_back({"query_aggregate", count_all ? "count_all" : "count_non_null"});
      result.evidence.push_back({"query_aggregate_function_requested", "count"});
      result.evidence.push_back({"query_aggregate_typed_result", "int64_nonnull"});
      result.evidence.push_back({"query_count_input_row_count",
                                 std::to_string(relations->front().rows.size())});
      if (!count_all) {
        result.evidence.push_back({"query_aggregate_value_binding",
                                   request.aggregate_value_field.empty() ? "ordinal" : "descriptor_field"});
      }
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    if (operation == "aggregate_scalar" || operation == "scalar_aggregate") {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      const auto statistics = BuildRuntimeOptimizerStatistics(request, *relations);
      const auto logical = BuildExecutableLogicalPlan(request, operation, *relations, &statistics, &result.evidence);
      if (!AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail)) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      std::string aggregate_function = request.aggregate_function;
      if (aggregate_function.empty()) aggregate_function = OptionValue(request, "aggregate_function:");
      if (aggregate_function.empty()) aggregate_function = "sum";
      const std::string aggregate_leaf = AggregateFunctionLeaf(aggregate_function);
      const std::string aggregate_value_field =
          request.aggregate_value_field.empty()
              ? OptionValue(request, "aggregate_value_field:")
              : request.aggregate_value_field;
      const std::size_t aggregate_value_column =
          ColumnIndexForRelation(relations->front(),
                                 aggregate_value_field,
                                 request.aggregate_value_column);
      const auto actual_value = EvaluateRelationScalarAggregateAssertion(
          relations->front(),
          aggregate_leaf,
          aggregate_value_field,
          aggregate_value_column,
          &error_detail);
      if (!actual_value) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      const std::string result_projection = LowerAscii(OptionValue(request, "result_projection:"));
      if (result_projection != "aggregate_assertion") {
        return QueryFailure<EnginePlanOperationResult>(
            request.context,
            "query_plan_scalar_aggregate_current_route_requires_assertion_projection");
      }
      result.result_shape = NumericAssertionResultShape(request, *actual_value, &error_detail);
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.plan_kind = operation;
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", operation);
      result.evidence.push_back({"query_scalar_aggregate", aggregate_leaf});
      result.evidence.push_back({"query_aggregate_function_requested", aggregate_leaf});
      result.evidence.push_back({"query_aggregate_value_column",
                                 std::to_string(aggregate_value_column)});
      result.evidence.push_back({"query_aggregate_value_binding",
                                 aggregate_value_field.empty() ? "ordinal" : "descriptor_field"});
      result.evidence.push_back({"query_scalar_aggregate_result_projection", "aggregate_assertion"});
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    if (operation == "join" || operation == "inner_join" || operation == "equi_join" ||
        operation == "left_join" || operation == "left_outer_join" ||
        operation == "semi_join" || operation == "join_group_sum_assertion" ||
        operation == "join_window_max_assertion") {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      const auto statistics = BuildRuntimeOptimizerStatistics(request, *relations);
      const auto logical = BuildExecutableLogicalPlan(request, operation, *relations, &statistics, &result.evidence);
      plan::PhysicalAccessKind selected_join_access = plan::PhysicalAccessKind::kJoinHash;
      if (RequestOptionDisabled(request, "optimizer_join_costing:", "join_costing:")) {
        selected_join_access = plan::PhysicalAccessKind::kJoinNestedLoop;
        result.evidence.push_back({"optimizer_profile", "disabled_join_costing_baseline_v1"});
        result.evidence.push_back({"optimizer_selected_access", "join_nested_loop"});
        result.evidence.push_back({"optimizer_executor_capability", "nested_loop_join"});
      } else {
        if (!AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail, &selected_join_access)) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
      }
      if (relations->size() < 2) {
        return QueryFailure<EnginePlanOperationResult>(request.context,
                                                       "query_plan_join_requires_two_relations");
      }
      const std::size_t left_key_column =
          ColumnIndexForRelation((*relations)[0], request.left_key_field, request.left_key_column);
      const std::size_t right_key_column =
          ColumnIndexForRelation((*relations)[1], request.right_key_field, request.right_key_column);
      result.evidence.push_back({"query_join_left_key_column", std::to_string(left_key_column)});
      result.evidence.push_back({"query_join_right_key_column", std::to_string(right_key_column)});
      result.evidence.push_back({"optimizer_join_left_cardinality", std::to_string((*relations)[0].rows.size())});
      result.evidence.push_back({"optimizer_join_right_cardinality", std::to_string((*relations)[1].rows.size())});
      result.evidence.push_back({"optimizer_join_relation_order",
                                 RelationObjectUuid((*relations)[0]) + "," + RelationObjectUuid((*relations)[1])});
      result.evidence.push_back({"query_join_key_binding",
                                 request.left_key_field.empty() ? "ordinal" : "descriptor_field"});
      std::string join_algorithm = LowerAscii(!request.join_algorithm.empty()
                                                  ? request.join_algorithm
                                                  : OptionValue(request, "join_algorithm:"));
      if (join_algorithm.empty()) {
        if (selected_join_access == plan::PhysicalAccessKind::kJoinNestedLoop) {
          join_algorithm = "nested_loop";
        } else if (selected_join_access == plan::PhysicalAccessKind::kJoinMerge) {
          join_algorithm = "merge";
        } else {
          join_algorithm = "hash";
        }
      }
      result.evidence.push_back({"query_join_algorithm", join_algorithm});
      const std::string result_projection = LowerAscii(OptionValue(request, "result_projection:"));
      const bool join_group_sum_assertion = operation == "join_group_sum_assertion";
      if (join_group_sum_assertion) {
        if (result_projection != "aggregate_assertion") {
          return QueryFailure<EnginePlanOperationResult>(
              request.context,
              "query_plan_join_group_current_route_requires_aggregate_assertion");
        }
        double having_threshold = 0.0;
        if (!TryParseReal64Value(OptionValue(request, "having_threshold:"), &having_threshold)) {
          return QueryFailure<EnginePlanOperationResult>(
              request.context,
              "query_plan_join_group_having_threshold_invalid");
        }
        const std::string group_field = OptionValue(request, "group_key_field:");
        const std::string aggregate_field = OptionValue(request, "aggregate_value_field:");
        const std::size_t group_column =
            ColumnIndexForRelation((*relations)[0],
                                   group_field,
                                   std::numeric_limits<std::size_t>::max());
        const std::size_t aggregate_column =
            ColumnIndexForRelation((*relations)[1],
                                   aggregate_field,
                                   std::numeric_limits<std::size_t>::max());
        const double actual_total =
            TypedJoinGroupSumHavingTotal((*relations)[0],
                                         (*relations)[1],
                                         left_key_column,
                                         right_key_column,
                                         group_column,
                                         aggregate_column,
                                         having_threshold,
                                         &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.result_shape = NumericAssertionResultShape(request, actual_total, &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.evidence.push_back({"query_join_kind", "inner_group_sum"});
        result.evidence.push_back({"query_join_result_projection", "aggregate_assertion"});
        result.evidence.push_back({"query_join_group_key_field", group_field});
        result.evidence.push_back({"query_join_group_aggregate_field", aggregate_field});
        result.evidence.push_back({"query_join_group_having_threshold",
                                   OptionValue(request, "having_threshold:")});
        result.plan_kind = operation;
        result.output_row_count = result.result_shape.rows.size();
        AddApiBehaviorEvidence(&result, "query_execution", operation);
        result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
        result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
        return result;
      }
      const bool join_window_max_assertion = operation == "join_window_max_assertion";
      if (join_window_max_assertion) {
        if (result_projection != "aggregate_assertion") {
          return QueryFailure<EnginePlanOperationResult>(
              request.context,
              "query_plan_join_window_current_route_requires_aggregate_assertion");
        }
        const std::string partition_field = OptionValue(request, "partition_key_field:");
        const std::size_t partition_column =
            ColumnIndexForRelation((*relations)[0],
                                   partition_field,
                                   std::numeric_limits<std::size_t>::max());
        const std::uint64_t max_row_number =
            TypedJoinWindowMaxRowNumber((*relations)[0],
                                        (*relations)[1],
                                        left_key_column,
                                        right_key_column,
                                        partition_column,
                                        &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.result_shape =
            NumericAssertionResultShape(request, static_cast<double>(max_row_number), &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.evidence.push_back({"query_join_kind", "inner_window_row_number"});
        result.evidence.push_back({"query_join_result_projection", "aggregate_assertion"});
        result.evidence.push_back({"query_join_window_partition_field", partition_field});
        result.evidence.push_back({"query_join_window_max_row_number",
                                   std::to_string(max_row_number)});
        result.plan_kind = operation;
        result.output_row_count = result.result_shape.rows.size();
        AddApiBehaviorEvidence(&result, "query_execution", operation);
        result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
        result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
        return result;
      }
      const bool semi_join_operation = operation == "semi_join";
      if (semi_join_operation) {
        if (result_projection != "count_assertion") {
          return QueryFailure<EnginePlanOperationResult>(
              request.context,
              "query_plan_semi_join_current_route_requires_count_assertion");
        }
        const std::uint64_t matched_left_row_count =
            TypedSemiJoinMatchedLeftRowCount((*relations)[0],
                                             (*relations)[1],
                                             left_key_column,
                                             right_key_column,
                                             &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.result_shape =
            CountAssertionResultShape(request, matched_left_row_count, &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.evidence.push_back({"query_join_kind", "semi"});
        result.evidence.push_back({"query_join_result_projection", "count_assertion"});
        result.evidence.push_back({"query_join_matched_left_row_count",
                                   std::to_string(matched_left_row_count)});
        result.plan_kind = operation;
        result.output_row_count = result.result_shape.rows.size();
        AddApiBehaviorEvidence(&result, "query_execution", operation);
        result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
        result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
        return result;
      }
      const bool left_join_operation = operation == "left_join" || operation == "left_outer_join";
      if (left_join_operation) {
        if (result_projection != "count_assertion") {
          return QueryFailure<EnginePlanOperationResult>(
              request.context,
              "query_plan_left_join_current_route_requires_count_assertion");
        }
        const std::string right_null_filter_field = OptionValue(request, "right_null_filter_field:");
        if (right_null_filter_field.empty()) {
          return QueryFailure<EnginePlanOperationResult>(
              request.context,
              "query_plan_left_join_right_null_filter_field_required");
        }
        const std::size_t right_null_filter_column =
            ColumnIndexForRelation((*relations)[1],
                                   right_null_filter_field,
                                   std::numeric_limits<std::size_t>::max());
        const std::uint64_t joined_row_count =
            TypedLeftJoinRightNullFilterRowCount((*relations)[0],
                                                 (*relations)[1],
                                                 left_key_column,
                                                 right_key_column,
                                                 right_null_filter_column,
                                                 &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.result_shape = CountAssertionResultShape(request, joined_row_count, &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.evidence.push_back({"query_join_kind", "left_outer"});
        result.evidence.push_back({"query_join_result_projection", "count_assertion"});
        result.evidence.push_back({"query_join_right_null_filter_field", right_null_filter_field});
        result.evidence.push_back({"query_join_right_null_filter_count",
                                   std::to_string(joined_row_count)});
        result.plan_kind = operation;
        result.output_row_count = result.result_shape.rows.size();
        AddApiBehaviorEvidence(&result, "query_execution", operation);
        result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
        result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
        return result;
      }
      const std::string distinct_count_field = OptionValue(request, "distinct_count_field:");
      if (result_projection == "count_assertion" && !distinct_count_field.empty()) {
        const auto left_filters =
            DescriptorRowFiltersForRequest(request, (*relations)[0], "left_filter_", &error_detail);
        if (!left_filters) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        const std::uint64_t joined_distinct_count =
            TypedInnerJoinDistinctLeftCount((*relations)[0],
                                            (*relations)[1],
                                            left_key_column,
                                            right_key_column,
                                            distinct_count_field,
                                            *left_filters,
                                            &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.result_shape = CountAssertionResultShape(request, joined_distinct_count, &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.evidence.push_back({"query_join_result_projection", "count_distinct_assertion"});
        result.evidence.push_back({"query_join_distinct_left_field", distinct_count_field});
        result.evidence.push_back({"query_join_left_filter_count",
                                   std::to_string(left_filters->size())});
        result.evidence.push_back({"query_join_matched_distinct_left_count",
                                   std::to_string(joined_distinct_count)});
        result.plan_kind = operation;
        result.output_row_count = result.result_shape.rows.size();
        AddApiBehaviorEvidence(&result, "query_execution", operation);
        result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
        result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
        return result;
      }
      result.result_shape = TypedInnerJoinResultShape(request,
                                                      (*relations)[0],
                                                      (*relations)[1],
                                                      left_key_column,
                                                      right_key_column,
                                                      join_algorithm,
                                                      &error_detail);
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      if (result_projection == "count_assertion") {
        const std::uint64_t joined_row_count =
            static_cast<std::uint64_t>(result.result_shape.rows.size());
        result.result_shape = CountAssertionResultShape(request, joined_row_count, &error_detail);
        if (!error_detail.empty()) {
          return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
        }
        result.evidence.push_back({"query_join_result_projection", "count_assertion"});
        result.evidence.push_back({"query_join_matched_row_count", std::to_string(joined_row_count)});
      }
      result.plan_kind = operation;
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", operation);
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    std::string aggregate_function = request.aggregate_function;
    if (aggregate_function.empty()) aggregate_function = OptionValue(request, "aggregate_function:");
    if (aggregate_function.empty()) aggregate_function = "sum";
    aggregate_function = LowerAscii(std::move(aggregate_function));
    const bool typed_aggregate_route =
        (operation == "aggregate" || operation == "group" || operation == "group_by") &&
        (CoreAggregateRequiresTypedResult(aggregate_function) ||
         StatisticalAggregateRequiresTypedResult(aggregate_function) ||
         BooleanAggregateRequiresTypedResult(aggregate_function) ||
         ApproxAggregateRequiresTypedResult(aggregate_function) ||
         PairAggregateRequiresTypedResult(aggregate_function) ||
         DistributionAggregateRequiresTypedResult(aggregate_function) ||
         ListAggAggregateRequiresTypedResult(aggregate_function) ||
         JsonAggregateRequiresTypedResult(aggregate_function) ||
         ArrayAggregateRequiresTypedResult(aggregate_function));
    if (typed_aggregate_route) {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      const auto statistics = BuildRuntimeOptimizerStatistics(request, *relations);
      const auto logical = BuildExecutableLogicalPlan(request, operation, *relations, &statistics, &result.evidence);
      if (!AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail)) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      const std::size_t group_key_column =
          relations->empty()
              ? request.group_key_column
              : ColumnIndexForRelation(relations->front(), request.group_key_field, request.group_key_column);
      const std::size_t aggregate_value_column =
          relations->empty()
              ? request.aggregate_value_column
              : ColumnIndexForRelation(relations->front(), request.aggregate_value_field, request.aggregate_value_column);
      const std::size_t aggregate_pair_value_column =
          relations->empty()
              ? request.aggregate_pair_value_column
              : ColumnIndexForRelation(relations->front(),
                                       request.aggregate_pair_value_field,
                                       request.aggregate_pair_value_column);
      const std::size_t aggregate_order_column =
          relations->empty()
              ? request.order_column
              : ColumnIndexForRelation(relations->front(), request.order_field, request.order_column);
      result.evidence.push_back({"query_aggregate_group_key_column", std::to_string(group_key_column)});
      result.evidence.push_back({"query_aggregate_value_column", std::to_string(aggregate_value_column)});
      result.evidence.push_back({"query_aggregate_key_binding",
                                 request.group_key_field.empty() ? "ordinal" : "descriptor_field"});
      result.evidence.push_back({"query_aggregate_value_binding",
                                 request.aggregate_value_field.empty() ? "ordinal" : "descriptor_field"});
      const std::string aggregate_leaf = AggregateFunctionLeaf(aggregate_function);
      if (CoreAggregateRequiresTypedResult(aggregate_function)) {
        result.result_shape = CoreAggregateResultShape(relations->front(),
                                                       aggregate_function,
                                                       group_key_column,
                                                       aggregate_value_column,
                                                       &error_detail);
      } else if (StatisticalAggregateRequiresTypedResult(aggregate_function)) {
        result.result_shape = StatisticalAggregateResultShape(relations->front(),
                                                              aggregate_function,
                                                              group_key_column,
                                                              aggregate_value_column,
                                                              &error_detail);
      } else if (aggregate_leaf == "approx_count_distinct") {
        result.result_shape = ApproxCountDistinctAggregateResultShape(relations->front(),
                                                                      group_key_column,
                                                                      aggregate_value_column,
                                                                      &error_detail);
      } else if (aggregate_leaf == "approx_median") {
        result.result_shape = ApproxMedianAggregateResultShape(relations->front(),
                                                               group_key_column,
                                                               aggregate_value_column,
                                                               &error_detail);
      } else if (PairAggregateRequiresTypedResult(aggregate_function)) {
        result.evidence.push_back({"query_aggregate_pair_value_column",
                                   std::to_string(aggregate_pair_value_column)});
        result.evidence.push_back({"query_aggregate_pair_value_binding",
                                   request.aggregate_pair_value_field.empty()
                                       ? "ordinal"
                                       : "descriptor_field"});
        result.result_shape = PairAggregateResultShape(relations->front(),
                                                       aggregate_function,
                                                       group_key_column,
                                                       aggregate_value_column,
                                                       aggregate_pair_value_column,
                                                       &error_detail);
      } else if (DistributionAggregateRequiresTypedResult(aggregate_function)) {
        const double aggregate_fraction = ParseReal64Value(OptionValue(request, "aggregate_fraction:"), 0.5);
        const std::size_t aggregate_limit = ParseSizeValue(OptionValue(request, "aggregate_limit:"), 10);
        result.evidence.push_back({"query_aggregate_fraction", FormatReal64(aggregate_fraction)});
        if (aggregate_leaf == "approx_top_k") {
          result.evidence.push_back({"query_aggregate_limit", std::to_string(aggregate_limit)});
        }
        result.result_shape = DistributionAggregateResultShape(relations->front(),
                                                               aggregate_function,
                                                               group_key_column,
                                                               aggregate_value_column,
                                                               aggregate_fraction,
                                                               aggregate_limit,
                                                               &error_detail);
      } else if (ListAggAggregateRequiresTypedResult(aggregate_function)) {
        const std::string separator = OptionValue(request, "listagg_separator:").empty()
            ? ","
            : OptionValue(request, "listagg_separator:");
        const std::string overflow_mode = LowerAscii(OptionValue(request, "listagg_overflow_mode:"));
        const std::size_t max_output_bytes =
            ParseSizeValue(OptionValue(request, "listagg_max_output_bytes:"), 0);
        const std::string indicator = OptionValue(request, "listagg_truncation_indicator:").empty()
            ? "..."
            : OptionValue(request, "listagg_truncation_indicator:");
        const bool with_count = ParseBoolValue(OptionValue(request, "listagg_with_count:"), true);
        result.evidence.push_back({"query_aggregate_order_column", std::to_string(aggregate_order_column)});
        result.evidence.push_back({"query_aggregate_order_binding",
                                   request.order_field.empty() ? "ordinal" : "descriptor_field"});
        result.evidence.push_back({"query_aggregate_listagg_separator", separator});
        if (!overflow_mode.empty()) {
          result.evidence.push_back({"query_aggregate_listagg_overflow", overflow_mode});
        }
        if (max_output_bytes != 0) {
          result.evidence.push_back({"query_aggregate_listagg_max_output_bytes",
                                     std::to_string(max_output_bytes)});
        }
        result.result_shape = ListAggAggregateResultShape(relations->front(),
                                                          group_key_column,
                                                          aggregate_value_column,
                                                          aggregate_order_column,
                                                          separator,
                                                          overflow_mode,
                                                          max_output_bytes,
                                                          indicator,
                                                          with_count,
                                                          &error_detail);
      } else if (JsonAggregateRequiresTypedResult(aggregate_function)) {
        result.evidence.push_back({"query_aggregate_order_column", std::to_string(aggregate_order_column)});
        result.evidence.push_back({"query_aggregate_order_binding",
                                   request.order_field.empty() ? "ordinal" : "descriptor_field"});
        if (aggregate_leaf == "json_object_agg") {
          result.evidence.push_back({"query_aggregate_pair_value_column",
                                     std::to_string(aggregate_pair_value_column)});
          result.evidence.push_back({"query_aggregate_pair_value_binding",
                                     request.aggregate_pair_value_field.empty()
                                         ? "ordinal"
                                         : "descriptor_field"});
          result.evidence.push_back({"query_aggregate_duplicate_key_policy",
                                     "last_key_wins_by_order"});
          result.result_shape = JsonObjectAggAggregateResultShape(relations->front(),
                                                                  group_key_column,
                                                                  aggregate_value_column,
                                                                  aggregate_pair_value_column,
                                                                  aggregate_order_column,
                                                                  &error_detail);
        } else {
          result.result_shape = JsonAggAggregateResultShape(relations->front(),
                                                            group_key_column,
                                                            aggregate_value_column,
                                                            aggregate_order_column,
                                                            &error_detail);
        }
      } else if (ArrayAggregateRequiresTypedResult(aggregate_function)) {
        result.evidence.push_back({"query_aggregate_order_column", std::to_string(aggregate_order_column)});
        result.evidence.push_back({"query_aggregate_order_binding",
                                   request.order_field.empty() ? "ordinal" : "descriptor_field"});
        result.evidence.push_back({"query_aggregate_array_descriptor", "list"});
        result.result_shape = ArrayAggAggregateResultShape(relations->front(),
                                                           group_key_column,
                                                           aggregate_value_column,
                                                           aggregate_order_column,
                                                           &error_detail);
      } else {
        result.result_shape = EveryAggregateResultShape(relations->front(),
                                                        aggregate_function,
                                                        group_key_column,
                                                        aggregate_value_column,
                                                        &error_detail);
      }
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      if (!ApplyTypedAggregateHavingFilter(request,
                                           &result.result_shape,
                                           &result.evidence,
                                           &error_detail)) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.plan_kind = operation;
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", operation);
      result.evidence.push_back({"query_aggregate", aggregate_leaf + "_by_key"});
      std::string result_profile = "boolean_nullable";
      if (aggregate_leaf == "count" || aggregate_leaf == "count_distinct") {
        result_profile = "int64_nonnull";
      } else if (aggregate_leaf == "sum" || aggregate_leaf == "avg" ||
                 aggregate_leaf == "min" || aggregate_leaf == "max" ||
                 StatisticalAggregateRequiresTypedResult(aggregate_function) || aggregate_leaf == "approx_median") {
        result_profile = "real64_nullable";
      } else if (aggregate_leaf == "approx_count_distinct") {
        result_profile = "int64_nonnull";
      } else if (PairAggregateRequiresTypedResult(aggregate_function)) {
        result_profile = aggregate_leaf == "regr_count" ? "int64_nonnull" : "real64_nullable";
      } else if (aggregate_leaf == "approx_top_k") {
        result_profile = "json_nullable";
      } else if (DistributionAggregateRequiresTypedResult(aggregate_function)) {
        result_profile = "real64_nullable";
      } else if (ListAggAggregateRequiresTypedResult(aggregate_function)) {
        result_profile = "text_nullable";
      } else if (JsonAggregateRequiresTypedResult(aggregate_function)) {
        result_profile = "json_nullable";
      } else if (ArrayAggregateRequiresTypedResult(aggregate_function)) {
        result_profile = "list_nullable";
      }
      result.evidence.push_back({"query_aggregate_typed_result", result_profile});
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    std::string window_function = LowerAscii(request.window_function);
    if (operation == "percent_rank_window") window_function = "percent_rank";
    if (operation == "cume_dist_window") window_function = "cume_dist";
    if (operation == "nth_value_window") window_function = "nth_value";
    const bool typed_window_route =
        (operation == "window" || operation == "percent_rank_window" ||
         operation == "cume_dist_window" || operation == "nth_value_window") &&
        WindowFunctionRequiresTypedResult(window_function);
    if (typed_window_route) {
      result.evidence.push_back({"query_executor", "local_noncluster"});
      const auto statistics = BuildRuntimeOptimizerStatistics(request, *relations);
      const auto logical = BuildExecutableLogicalPlan(request, operation, *relations, &statistics, &result.evidence);
      if (!AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail)) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      const std::size_t order_column =
          relations->empty() ? request.order_column
                             : ColumnIndexForRelation(relations->front(), request.order_field, request.order_column);
      const std::size_t value_column =
          relations->empty()
              ? request.window_value_column
              : ColumnIndexForRelation(relations->front(), request.window_value_field, request.window_value_column);
      result.evidence.push_back({"query_window_order_column", std::to_string(order_column)});
      result.evidence.push_back({"query_window_value_column", std::to_string(value_column)});
      result.evidence.push_back({"query_window_binding",
                                 request.order_field.empty() ? "ordinal" : "descriptor_field"});
      result.result_shape = TypedWindowResultShape(relations->front(),
                                                   window_function,
                                                   order_column,
                                                   value_column,
                                                   request.window_n,
                                                   &error_detail);
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.plan_kind = operation;
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", operation);
      result.evidence.push_back({"query_window", window_function});
      result.evidence.push_back({"query_window_typed_result", "descriptor_nullable"});
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    const bool projected_set_operation =
        IsSetOperationName(operation) && !OptionValue(request, "left_project_field:").empty();
    if (!projected_set_operation && !RelationsAreIntegerCompatible(*relations, &error_detail)) {
      return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
    }
    result.evidence.push_back({"query_executor", "local_noncluster"});
    const auto statistics = BuildRuntimeOptimizerStatistics(request, *relations);
    const auto logical = BuildExecutableLogicalPlan(request, operation, *relations, &statistics, &result.evidence);
    plan::PhysicalAccessKind selected_access = plan::PhysicalAccessKind::kTableScan;
    if (!AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail, &selected_access)) {
      return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
    }
    std::string planned_join_algorithm;
    if (selected_access == plan::PhysicalAccessKind::kJoinNestedLoop) {
      planned_join_algorithm = "nested_loop";
    } else if (selected_access == plan::PhysicalAccessKind::kJoinMerge) {
      planned_join_algorithm = "merge";
    } else if (selected_access == plan::PhysicalAccessKind::kJoinHash) {
      planned_join_algorithm = "hash";
    }
    auto batch = ExecuteQueryBatch(request,
                                   *relations,
                                   operation,
                                   &result.evidence,
                                   &error_detail,
                                   planned_join_algorithm);
    if (!error_detail.empty()) { return QueryFailure<EnginePlanOperationResult>(request.context, error_detail); }
    const auto validation = exec::ValidateBatch(batch);
    if (!validation.ok) { return QueryFailure<EnginePlanOperationResult>(request.context, validation.diagnostic_code); }
    const std::string result_projection = LowerAscii(OptionValue(request, "result_projection:"));
    if (operation == "recursive_cte" && result_projection == "aggregate_assertion") {
      const std::string aggregate_function =
          AggregateFunctionLeaf(OptionValue(request, "aggregate_function:"));
      const auto actual_value =
          EvaluateRecursiveCteAggregateAssertion(batch, aggregate_function, &error_detail);
      if (!actual_value) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.result_shape = NumericAssertionResultShape(request, *actual_value, &error_detail);
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.plan_kind = operation;
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", operation);
      result.evidence.push_back({"query_cte_result_projection", "aggregate_assertion"});
      result.evidence.push_back({"query_cte_aggregate_function", aggregate_function});
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    if (IsSetOperationName(operation) && result_projection == "count_assertion") {
      result.result_shape =
          CountAssertionResultShape(request, static_cast<std::uint64_t>(batch.rows.size()), &error_detail);
      if (!error_detail.empty()) {
        return QueryFailure<EnginePlanOperationResult>(request.context, error_detail);
      }
      result.plan_kind = operation;
      result.output_row_count = result.result_shape.rows.size();
      AddApiBehaviorEvidence(&result, "query_execution", operation);
      result.evidence.push_back({"query_set_result_projection", "count_assertion"});
      result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
      result.evidence.push_back({"query_output_row_count", std::to_string(result.result_shape.rows.size())});
      return result;
    }
    result.plan_kind = operation;
    result.output_row_count = batch.rows.size();
    result.result_shape = BatchToResultShape(batch);
    AddApiBehaviorEvidence(&result, "query_execution", operation);
    result.evidence.push_back({"query_relation_count", std::to_string(relations->size())});
    result.evidence.push_back({"query_output_row_count", std::to_string(batch.rows.size())});
    return result;
  }
  std::string plan_name = "scan";
  if (!request.target_object.uuid.canonical.empty() && request.context.local_transaction_id != 0) {
    const auto loaded = LoadQueryCrudCompatibilityState(request.context);
    if (loaded.ok) {
      // DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP
      const auto lookup = IndexedMgaRowsForPredicateForContext(
          loaded.state,
          request.target_object.uuid.canonical,
          request.predicate,
          request.context,
          1);
      if (lookup.index_used) {
        result.evidence.insert(result.evidence.end(),
                               lookup.evidence.begin(),
                               lookup.evidence.end());
        plan_name = "index_lookup";
      } else if (lookup.index_refused) {
        result.evidence.insert(result.evidence.end(),
                               lookup.evidence.begin(),
                               lookup.evidence.end());
      }
    }
  }
  std::vector<EngineQueryRelation> planned_relations;
  EngineQueryRelation planned_relation;
  planned_relation.relation_name = request.target_object.uuid.canonical.empty() ? "request_rows" : "crud:" + request.target_object.uuid.canonical;
  planned_relation.descriptor_digest = planned_relation.relation_name;
  planned_relation.source_object = request.target_object;
  planned_relations.push_back(std::move(planned_relation));
  std::string error_detail;
  const auto statistics = BuildRuntimeOptimizerStatistics(request, planned_relations);
  const auto logical = BuildExecutableLogicalPlan(request, plan_name, planned_relations, &statistics, &result.evidence);
  plan::PhysicalAccessKind selected_access = plan::PhysicalAccessKind::kTableScan;
  (void)AttachOptimizerSelectionEvidence(logical, statistics, &result.evidence, &error_detail, &selected_access);
  const std::string selected_plan = plan::PhysicalAccessKindName(selected_access);
  result.plan_kind = selected_plan;
  AddApiBehaviorEvidence(&result, "query_plan", selected_plan);
  AddApiBehaviorRow(&result, {{"plan_kind", selected_plan}, {"predicate_kind", request.predicate.predicate_kind}, {"payload", ApiBehaviorPayloadFromRequest(request)}});
  return result;
}

EnginePlanOperationResult EnginePlanOperation(const EnginePlanOperationRequest& request) {
  const bool cache_enabled =
      RequestOptionEnabled(request, "optimizer_plan_cache:", "plan_cache:");
  const bool cache_disabled =
      RequestOptionDisabled(request, "optimizer_plan_cache:", "plan_cache:");
  if (!cache_enabled || cache_disabled) {
    auto result = EnginePlanOperationUncachedImpl(request);
    if (cache_disabled) {
      PlanCacheBinding binding;
      AttachLivePlanCacheEvidence(&result, "disabled", {}, binding);
    }
    return result;
  }

  std::string error_detail;
  const auto relations = BuildRelations(request, &error_detail);
  if (!relations || relations->empty()) {
    auto result = EnginePlanOperationUncachedImpl(request);
    PlanCacheBinding binding;
    AttachLivePlanCacheEvidence(&result, "miss_unbound", {}, binding);
    return result;
  }

  const std::string operation = QueryOperation(request);
  const PlanCacheBinding binding = BindPlanCacheRelations(request, *relations);
  const auto cache_key_input =
      BuildLiveOptimizerPlanCacheKeyInput(request, operation, *relations, binding);
  const auto lookup = LiveOptimizerPlanCache().Lookup(cache_key_input);
  const std::string& cache_key = lookup.cache_key;
  const bool cache_hit = lookup.hit;
  auto result = EnginePlanOperationUncachedImpl(request);
  if (result.ok && !cache_hit) {
    opt::CachedOptimizerPlan cached;
    cached.cache_key = cache_key;
    cached.key_input = cache_key_input;
    cached.created_epoch = std::max({request.context.catalog_generation_id,
                                     request.context.security_epoch,
                                     request.context.resource_epoch,
                                     request.context.name_resolution_epoch});
    cached.result.ok = true;
    cached.result.plan_id = result.plan_kind.empty() ? operation : result.plan_kind;
    cached.result.diagnostic_code = "ok";
    LiveOptimizerPlanCache().Put(std::move(cached));
  }
  AttachLivePlanCacheEvidence(&result,
                              cache_hit ? "hit" : "miss",
                              cache_key,
                              binding,
                              lookup.diagnostic_code,
                              lookup.evidence);
  return result;
}

}  // namespace scratchbird::engine::internal_api
