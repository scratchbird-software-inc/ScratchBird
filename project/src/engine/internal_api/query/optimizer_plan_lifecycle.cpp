// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/optimizer_plan_lifecycle.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

struct RawPlanEvent {
  std::uint64_t event_sequence = 0;
  std::string event_kind;
  std::uint64_t creator_tx = 0;
  std::map<std::string, std::string> fields;
};

std::string EventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.optimizer_plan_events";
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) {
    parts.push_back(current);
  }
  return parts;
}

std::string HexEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecode(const std::string& value) {
  std::string out;
  if ((value.size() % 2) != 0) {
    return out;
  }
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) {
      return {};
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::uint64_t ParseU64(const std::string& value, std::uint64_t fallback = 0) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

bool ParseBool(const std::string& value, bool fallback = false) {
  if (value == "1" || value == "true" || value == "TRUE" || value == "yes") {
    return true;
  }
  if (value == "0" || value == "false" || value == "FALSE" || value == "no") {
    return false;
  }
  return fallback;
}

std::string BoolText(bool value) {
  return value ? "1" : "0";
}

std::string Field(const std::map<std::string, std::string>& fields,
                  const std::string& key,
                  std::string fallback = {}) {
  const auto it = fields.find(key);
  return it == fields.end() ? std::move(fallback) : it->second;
}

std::uint64_t FieldU64(const std::map<std::string, std::string>& fields,
                       const std::string& key,
                       std::uint64_t fallback = 0) {
  return ParseU64(Field(fields, key), fallback);
}

bool FieldBool(const std::map<std::string, std::string>& fields,
               const std::string& key,
               bool fallback = false) {
  return ParseBool(Field(fields, key), fallback);
}

EngineApiDiagnostic PlanDiagnostic(const char* code, std::string detail = {}, bool error = true) {
  std::string message_key = "optimizer.plan.lifecycle";
  const std::string diagnostic_code = code;
  if (diagnostic_code == kOptimizerPlanDiagnosticOk) {
    message_key = "optimizer.plan.ok";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticDatabasePathRequired) {
    message_key = "optimizer.plan.database_path_required";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticMgaTransactionRequired) {
    message_key = "optimizer.plan.mga_transaction_required";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticInvalidRequest) {
    message_key = "optimizer.plan.invalid_request";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticStatisticsStale) {
    message_key = "optimizer.plan.statistics_stale";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticCacheMiss) {
    message_key = "optimizer.plan.cache_miss";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticCacheInvalidated) {
    message_key = "optimizer.plan.cache_invalidated";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticEpochMismatch) {
    message_key = "optimizer.plan.epoch_mismatch";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticWriteFailed) {
    message_key = "optimizer.plan.write_failed";
  }
  return MakeEngineApiDiagnostic(diagnostic_code, std::move(message_key), std::move(detail), error);
}

EngineApiDiagnostic OkDiagnostic() {
  return PlanDiagnostic(kOptimizerPlanDiagnosticOk, {}, false);
}

template <typename TResult>
TResult SuccessResult(const EngineRequestContext& context, std::string operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.embedded_trust_mode_observed =
      context.trust_mode == EngineTrustMode::embedded_in_process;
  return result;
}

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         std::string operation_id,
                         EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.embedded_trust_mode_observed =
      context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

EngineTypedValue TextValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

void AddRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields) {
  EngineRowValue row;
  row.requested_row_uuid.canonical =
      "optimizer-plan-row-" + std::to_string(result->result_shape.rows.size() + 1);
  for (auto& field : fields) {
    row.fields.push_back({std::move(field.first), TextValue(std::move(field.second))});
  }
  result->result_shape.result_kind = "optimizer_plan_lifecycle_rows";
  result->result_shape.rows.push_back(std::move(row));
}

void AddEvidence(EngineApiResult* result, std::string kind, std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

bool ValidateMutatingContext(const EngineRequestContext& context, EngineApiDiagnostic* diagnostic) {
  if (context.database_path.empty()) {
    *diagnostic = PlanDiagnostic(kOptimizerPlanDiagnosticDatabasePathRequired, "database_path");
    return false;
  }
  if (context.local_transaction_id == 0) {
    *diagnostic =
        PlanDiagnostic(kOptimizerPlanDiagnosticMgaTransactionRequired, "local_transaction_id");
    return false;
  }
  return true;
}

bool EventVisible(const EngineRequestContext& context, std::uint64_t creator_tx) {
  if (creator_tx == 0) {
    return true;
  }
  if (context.local_transaction_id != 0 && creator_tx == context.local_transaction_id) {
    return true;
  }
  if (context.snapshot_visible_through_local_transaction_id != 0) {
    return creator_tx <= context.snapshot_visible_through_local_transaction_id;
  }
  if (context.local_transaction_id != 0) {
    return creator_tx <= context.local_transaction_id;
  }
  return false;
}

std::string MakeEvent(std::string event_kind,
                      std::uint64_t creator_tx,
                      std::vector<std::pair<std::string, std::string>> fields) {
  std::string out = std::string(kOptimizerPlanLifecycleEventMagic) + "\t" +
                    std::move(event_kind) + "\t" + std::to_string(creator_tx);
  for (auto& field : fields) {
    out.push_back('\t');
    out += field.first;
    out.push_back('=');
    out += HexEncode(field.second);
  }
  return out;
}

EngineApiDiagnostic AppendEvent(const EngineRequestContext& context,
                                std::string event_kind,
                                std::uint64_t creator_tx,
                                std::vector<std::pair<std::string, std::string>> fields) {
  if (context.database_path.empty()) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticDatabasePathRequired, "database_path");
  }
  std::ofstream out(EventPath(context), std::ios::binary | std::ios::app);
  if (!out) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticWriteFailed, "open");
  }
  out << MakeEvent(std::move(event_kind), creator_tx, std::move(fields)) << '\n';
  out.flush();
  if (!out) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticWriteFailed, "flush");
  }
  return OkDiagnostic();
}

EngineLoadOptimizerPlanLifecycleStateResult ReadRawEvents(
    const EngineRequestContext& context,
    std::vector<RawPlanEvent>* events) {
  EngineLoadOptimizerPlanLifecycleStateResult result;
  if (context.database_path.empty()) {
    result.diagnostic =
        PlanDiagnostic(kOptimizerPlanDiagnosticDatabasePathRequired, "database_path");
    return result;
  }
  std::ifstream in(EventPath(context), std::ios::binary);
  if (!in) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }

  std::string line;
  std::uint64_t event_sequence = 0;
  while (std::getline(in, line)) {
    ++event_sequence;
    const auto parts = Split(line, '\t');
    if (parts.size() < 3 || parts[0] != kOptimizerPlanLifecycleEventMagic) {
      continue;
    }
    RawPlanEvent event;
    event.event_sequence = event_sequence;
    event.event_kind = parts[1];
    event.creator_tx = ParseU64(parts[2]);
    for (std::size_t i = 3; i < parts.size(); ++i) {
      const auto pos = parts[i].find('=');
      if (pos == std::string::npos) {
        continue;
      }
      event.fields[parts[i].substr(0, pos)] = HexDecode(parts[i].substr(pos + 1));
    }
    events->push_back(std::move(event));
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineOptimizerPlanCacheEntry EntryFromFields(const RawPlanEvent& event) {
  EngineOptimizerPlanCacheEntry entry;
  entry.creator_tx = event.creator_tx;
  entry.event_sequence = event.event_sequence;
  entry.plan_cache_epoch = FieldU64(event.fields, "plan_cache_epoch", event.creator_tx);
  entry.plan_uuid = Field(event.fields, "plan_uuid");
  entry.query_fingerprint = Field(event.fields, "query_fingerprint");
  entry.relation_uuid = Field(event.fields, "relation_uuid");
  entry.index_uuid = Field(event.fields, "index_uuid");
  entry.catalog_physical_profile_key = Field(event.fields, "catalog_physical_profile_key");
  entry.plan_shape_digest = Field(event.fields, "plan_shape_digest");
  entry.index_generation = FieldU64(event.fields, "index_generation");
  entry.statistics_generation = FieldU64(event.fields, "statistics_generation");
  entry.catalog_generation_id = FieldU64(event.fields, "catalog_generation_id");
  entry.resource_epoch = FieldU64(event.fields, "resource_epoch");
  entry.charset_epoch = FieldU64(event.fields, "charset_epoch");
  entry.collation_epoch = FieldU64(event.fields, "collation_epoch");
  entry.invalidated = FieldBool(event.fields, "invalidated");
  entry.invalidation_reason = Field(event.fields, "invalidation_reason");
  return entry;
}

bool EntryMatches(const EngineOptimizerPlanCacheEntry& entry,
                  const std::string& plan_uuid,
                  const std::string& query_fingerprint,
                  const std::string& index_uuid) {
  if (!plan_uuid.empty() && entry.plan_uuid != plan_uuid) {
    return false;
  }
  if (!query_fingerprint.empty() && entry.query_fingerprint != query_fingerprint) {
    return false;
  }
  if (!index_uuid.empty() && entry.index_uuid != index_uuid) {
    return false;
  }
  return !plan_uuid.empty() || !query_fingerprint.empty() || !index_uuid.empty();
}

const EngineOptimizerPlanCacheEntry* FindEntry(const EngineOptimizerPlanLifecycleState& state,
                                               const std::string& plan_uuid,
                                               const std::string& query_fingerprint,
                                               const std::string& index_uuid) {
  for (auto it = state.entries.rbegin(); it != state.entries.rend(); ++it) {
    if (EntryMatches(*it, plan_uuid, query_fingerprint, index_uuid)) {
      return &*it;
    }
  }
  return nullptr;
}

void ApplyInvalidation(EngineOptimizerPlanLifecycleState* state, const RawPlanEvent& event) {
  const bool invalidate_all = FieldBool(event.fields, "invalidate_all");
  const std::string index_uuid = Field(event.fields, "index_uuid");
  const std::string reason = Field(event.fields, "reason", "explicit_invalidation");
  const std::uint64_t plan_cache_epoch = FieldU64(event.fields, "plan_cache_epoch", event.creator_tx);
  const std::uint64_t new_index_generation = FieldU64(event.fields, "new_index_generation");
  const std::uint64_t new_statistics_generation = FieldU64(event.fields, "new_statistics_generation");
  const std::uint64_t new_catalog_generation_id = FieldU64(event.fields, "new_catalog_generation_id");
  const std::uint64_t new_resource_epoch = FieldU64(event.fields, "new_resource_epoch");
  const std::uint64_t new_charset_epoch = FieldU64(event.fields, "new_charset_epoch");
  const std::uint64_t new_collation_epoch = FieldU64(event.fields, "new_collation_epoch");

  bool touched = false;
  for (auto& entry : state->entries) {
    if (!invalidate_all && entry.index_uuid != index_uuid) {
      continue;
    }
    if (new_index_generation != 0 && entry.index_generation == new_index_generation &&
        new_statistics_generation != 0 && entry.statistics_generation == new_statistics_generation &&
        new_catalog_generation_id != 0 && entry.catalog_generation_id == new_catalog_generation_id &&
        new_resource_epoch != 0 && entry.resource_epoch == new_resource_epoch &&
        new_charset_epoch != 0 && entry.charset_epoch == new_charset_epoch &&
        new_collation_epoch != 0 && entry.collation_epoch == new_collation_epoch) {
      continue;
    }
    entry.invalidated = true;
    entry.invalidation_reason = reason;
    entry.plan_cache_epoch = std::max(entry.plan_cache_epoch, plan_cache_epoch);
    touched = true;
  }
  if (touched || invalidate_all || !index_uuid.empty()) {
    ++state->invalidation_events;
  }
  state->plan_cache_epoch = std::max(state->plan_cache_epoch, plan_cache_epoch);
}

void ApplyRecoverySnapshot(EngineOptimizerPlanLifecycleState* state, const RawPlanEvent& event) {
  state->recovered_from_persisted_evidence = true;
  state->recovery_snapshot_uuid = Field(event.fields, "recovery_snapshot_uuid");
  for (auto& entry : state->entries) {
    entry.recovered_from_persisted_evidence = true;
  }
}

void FillEntryResult(EngineApiResult* result, const EngineOptimizerPlanCacheEntry& entry) {
  result->primary_object.uuid.canonical = entry.plan_uuid;
  result->primary_object.object_kind = "optimizer_plan_cache_entry";
  AddEvidence(result, "optimizer_plan_cache_entry", entry.plan_uuid);
  AddEvidence(result, "mga_visible_plan_generation", std::to_string(entry.creator_tx));
  AddEvidence(result, "catalog_physical_index_profile", entry.catalog_physical_profile_key);
  AddRow(result,
         {{"plan_uuid", entry.plan_uuid},
          {"query_fingerprint", entry.query_fingerprint},
          {"index_uuid", entry.index_uuid},
          {"plan_cache_epoch", std::to_string(entry.plan_cache_epoch)},
          {"index_generation", std::to_string(entry.index_generation)},
          {"statistics_generation", std::to_string(entry.statistics_generation)},
          {"catalog_generation_id", std::to_string(entry.catalog_generation_id)},
          {"resource_epoch", std::to_string(entry.resource_epoch)},
          {"charset_epoch", std::to_string(entry.charset_epoch)},
          {"collation_epoch", std::to_string(entry.collation_epoch)},
          {"invalidated", BoolText(entry.invalidated)}});
}

EngineApiDiagnostic ValidateCacheRequest(const EngineOptimizerCachePlanRequest& request) {
  if (request.plan_uuid.empty() || request.query_fingerprint.empty() ||
      request.index_uuid.empty() || request.plan_shape_digest.empty()) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest, "plan_identity");
  }
  if (request.statistics.statistics_generation == 0 ||
      request.statistics.index_generation == 0 ||
      request.statistics.catalog_generation_id == 0 ||
      !request.statistics.catalog_profile_coupled) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest, "statistics_generation");
  }
  return OkDiagnostic();
}

bool EntryEpochsMatch(const EngineOptimizerPlanCacheEntry& entry,
                      const index_lifecycle::IndexResourceEpochVector& current) {
  return entry.resource_epoch == current.resource_epoch &&
         entry.charset_epoch == current.charset_epoch &&
         entry.collation_epoch == current.collation_epoch;
}

}  // namespace

EngineLoadOptimizerPlanLifecycleStateResult LoadOptimizerPlanLifecycleState(
    const EngineRequestContext& context) {
  std::vector<RawPlanEvent> events;
  auto result = ReadRawEvents(context, &events);
  if (!result.ok) {
    return result;
  }

  for (const auto& event : events) {
    result.state.max_event_sequence = std::max(result.state.max_event_sequence, event.event_sequence);
    if (!EventVisible(context, event.creator_tx)) {
      continue;
    }
    if (event.event_kind == "CACHE_PLAN") {
      auto entry = EntryFromFields(event);
      result.state.plan_cache_epoch = std::max(result.state.plan_cache_epoch,
                                               entry.plan_cache_epoch);
      result.state.entries.push_back(std::move(entry));
    } else if (event.event_kind == "INVALIDATE") {
      ApplyInvalidation(&result.state, event);
    } else if (event.event_kind == "RECOVERY_SNAPSHOT") {
      ApplyRecoverySnapshot(&result.state, event);
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineOptimizerCachePlanResult EngineOptimizerCachePlan(
    const EngineOptimizerCachePlanRequest& request) {
  constexpr const char* kOperation = "query.optimizer.cache_plan";
  EngineApiDiagnostic context_diagnostic;
  if (!ValidateMutatingContext(request.context, &context_diagnostic)) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context, kOperation, context_diagnostic);
  }
  const auto validation = ValidateCacheRequest(request);
  if (validation.error) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context, kOperation, validation);
  }

  const auto statistics = index_lifecycle::EvaluateIndexStatisticsForUse(
      request.index_descriptor,
      request.statistics,
      request.index_descriptor.resource_epochs,
      request.freshness_policy,
      request.context.snapshot_visible_through_local_transaction_id);
  if (!statistics.ok()) {
    const bool stale = statistics.diagnostic.diagnostic_code ==
                       index_lifecycle::kIndexStatisticsDiagnosticStaleRefused;
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(stale ? kOptimizerPlanDiagnosticStatisticsStale
                             : kOptimizerPlanDiagnosticInvalidRequest,
                       statistics.diagnostic.diagnostic_code));
  }

  const std::uint64_t plan_cache_epoch =
      std::max(request.context.local_transaction_id, request.statistics.statistics_generation);
  const auto appended = AppendEvent(
      request.context,
      "CACHE_PLAN",
      request.context.local_transaction_id,
      {{"plan_cache_epoch", std::to_string(plan_cache_epoch)},
       {"plan_uuid", request.plan_uuid},
       {"query_fingerprint", request.query_fingerprint},
       {"relation_uuid", request.relation_uuid},
       {"index_uuid", request.index_uuid},
       {"catalog_physical_profile_key", request.statistics.physical_profile_key},
       {"plan_shape_digest", request.plan_shape_digest},
       {"index_generation", std::to_string(request.statistics.index_generation)},
       {"statistics_generation", std::to_string(request.statistics.statistics_generation)},
       {"catalog_generation_id", std::to_string(request.statistics.catalog_generation_id)},
       {"resource_epoch", std::to_string(request.statistics.resource_epochs.resource_epoch)},
       {"charset_epoch", std::to_string(request.statistics.resource_epochs.charset_epoch)},
       {"collation_epoch", std::to_string(request.statistics.resource_epochs.collation_epoch)},
       {"invalidated", "0"}});
  if (appended.error) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(request.context, kOperation, appended);
  }

  const auto loaded = LoadOptimizerPlanLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context, kOperation, loaded.diagnostic);
  }
  const auto* entry =
      FindEntry(loaded.state, request.plan_uuid, request.query_fingerprint, request.index_uuid);
  if (entry == nullptr) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticCacheMiss, request.plan_uuid));
  }

  auto result = SuccessResult<EngineOptimizerCachePlanResult>(request.context, kOperation);
  result.entry = *entry;
  result.plan_cache_epoch = loaded.state.plan_cache_epoch;
  FillEntryResult(&result, result.entry);
  return result;
}

EngineOptimizerValidateCachedPlanResult EngineOptimizerValidateCachedPlan(
    const EngineOptimizerValidateCachedPlanRequest& request) {
  constexpr const char* kOperation = "query.optimizer.validate_cached_plan";
  if (request.context.database_path.empty()) {
    return DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticDatabasePathRequired, "database_path"));
  }
  if ((request.plan_uuid.empty() && request.query_fingerprint.empty()) ||
      request.index_uuid.empty()) {
    return DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest, "cache_lookup_key"));
  }

  const auto loaded = LoadOptimizerPlanLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context, kOperation, loaded.diagnostic);
  }
  const auto* entry =
      FindEntry(loaded.state, request.plan_uuid, request.query_fingerprint, request.index_uuid);
  if (entry == nullptr) {
    return DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticCacheMiss, request.index_uuid));
  }
  if (entry->invalidated) {
    auto result = DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticCacheInvalidated, entry->invalidation_reason));
    result.entry = *entry;
    result.invalidation_required = true;
    result.plan_cache_epoch = loaded.state.plan_cache_epoch;
    return result;
  }
  if (request.require_current_statistics && request.statistics_stale) {
    auto result = DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticStatisticsStale, request.index_uuid));
    result.entry = *entry;
    result.invalidation_required = true;
    result.plan_cache_epoch = loaded.state.plan_cache_epoch;
    return result;
  }
  if (!EntryEpochsMatch(*entry, request.current_resource_epochs)) {
    auto result = DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticEpochMismatch, request.index_uuid));
    result.entry = *entry;
    result.invalidation_required = true;
    result.plan_cache_epoch = loaded.state.plan_cache_epoch;
    return result;
  }
  if (entry->index_generation != request.current_index_generation ||
      entry->statistics_generation != request.current_statistics_generation ||
      entry->catalog_generation_id != request.current_catalog_generation_id) {
    auto result = DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticCacheInvalidated, request.index_uuid));
    result.entry = *entry;
    result.invalidation_required = true;
    result.plan_cache_epoch = loaded.state.plan_cache_epoch;
    return result;
  }

  auto result = SuccessResult<EngineOptimizerValidateCachedPlanResult>(request.context, kOperation);
  result.entry = *entry;
  result.cache_hit = true;
  result.plan_cache_epoch = loaded.state.plan_cache_epoch;
  FillEntryResult(&result, result.entry);
  AddEvidence(&result, "optimizer_plan_cache_hit", result.entry.plan_uuid);
  return result;
}

EngineOptimizerInvalidatePlanCacheResult EngineOptimizerInvalidatePlanCache(
    const EngineOptimizerInvalidatePlanCacheRequest& request) {
  constexpr const char* kOperation = "query.optimizer.invalidate_plan_cache";
  EngineApiDiagnostic context_diagnostic;
  if (!ValidateMutatingContext(request.context, &context_diagnostic)) {
    return DiagnosticResult<EngineOptimizerInvalidatePlanCacheResult>(
        request.context, kOperation, context_diagnostic);
  }
  if (!request.invalidate_all && request.index_uuid.empty()) {
    return DiagnosticResult<EngineOptimizerInvalidatePlanCacheResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest, "index_uuid"));
  }

  const std::uint64_t plan_cache_epoch =
      std::max({request.context.local_transaction_id,
                request.new_index_generation,
                request.new_statistics_generation,
                request.new_catalog_generation_id});
  const auto appended = AppendEvent(
      request.context,
      "INVALIDATE",
      request.context.local_transaction_id,
      {{"plan_cache_epoch", std::to_string(plan_cache_epoch)},
       {"index_uuid", request.index_uuid},
       {"reason", request.reason.empty() ? std::string("explicit_invalidation") : request.reason},
       {"new_index_generation", std::to_string(request.new_index_generation)},
       {"new_statistics_generation", std::to_string(request.new_statistics_generation)},
       {"new_catalog_generation_id", std::to_string(request.new_catalog_generation_id)},
       {"new_resource_epoch", std::to_string(request.new_resource_epochs.resource_epoch)},
       {"new_charset_epoch", std::to_string(request.new_resource_epochs.charset_epoch)},
       {"new_collation_epoch", std::to_string(request.new_resource_epochs.collation_epoch)},
       {"invalidate_all", BoolText(request.invalidate_all)}});
  if (appended.error) {
    return DiagnosticResult<EngineOptimizerInvalidatePlanCacheResult>(
        request.context, kOperation, appended);
  }

  const auto loaded = LoadOptimizerPlanLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineOptimizerInvalidatePlanCacheResult>(
        request.context, kOperation, loaded.diagnostic);
  }
  auto result = SuccessResult<EngineOptimizerInvalidatePlanCacheResult>(
      request.context, kOperation);
  result.state = loaded.state;
  result.plan_cache_epoch = loaded.state.plan_cache_epoch;
  AddEvidence(&result, "optimizer_plan_cache_invalidation", request.index_uuid);
  AddRow(&result,
         {{"plan_cache_epoch", std::to_string(result.plan_cache_epoch)},
          {"invalidation_events", std::to_string(result.state.invalidation_events)},
          {"reason", request.reason}});
  return result;
}

EngineOptimizerRecoverPlanCacheResult EngineOptimizerRecoverPlanCache(
    const EngineOptimizerRecoverPlanCacheRequest& request) {
  constexpr const char* kOperation = "query.optimizer.recover_plan_cache";
  EngineApiDiagnostic context_diagnostic;
  if (!ValidateMutatingContext(request.context, &context_diagnostic)) {
    return DiagnosticResult<EngineOptimizerRecoverPlanCacheResult>(
        request.context, kOperation, context_diagnostic);
  }
  const std::string snapshot_uuid =
      "optimizer-plan-recovery-" + std::to_string(request.context.local_transaction_id);
  const auto appended = AppendEvent(
      request.context,
      "RECOVERY_SNAPSHOT",
      request.context.local_transaction_id,
      {{"plan_cache_epoch", std::to_string(request.context.local_transaction_id)},
       {"recovery_snapshot_uuid", snapshot_uuid}});
  if (appended.error) {
    return DiagnosticResult<EngineOptimizerRecoverPlanCacheResult>(
        request.context, kOperation, appended);
  }

  const auto loaded = LoadOptimizerPlanLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineOptimizerRecoverPlanCacheResult>(
        request.context, kOperation, loaded.diagnostic);
  }
  auto result = SuccessResult<EngineOptimizerRecoverPlanCacheResult>(request.context, kOperation);
  result.state = loaded.state;
  result.recovery_snapshot_uuid = snapshot_uuid;
  AddEvidence(&result, "optimizer_plan_cache_recovery_snapshot", snapshot_uuid);
  AddRow(&result,
         {{"recovery_snapshot_uuid", snapshot_uuid},
          {"recovered_entries", std::to_string(result.state.entries.size())}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
