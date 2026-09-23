// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lifecycle/sequence_generator_lifecycle.hpp"
#include "dml/mutation_savepoint_capability.hpp"
#include "catalog/binary_catalog_metadata.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "core/uuid/uuid.hpp"
#include "local_transaction_store.hpp"
#include "transaction/transaction_api.hpp"
#include <filesystem>
#include <mutex>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

struct RawSequenceEvent {
  std::uint64_t event_sequence = 0;
  std::string event_kind;
  std::uint64_t creator_tx = 0;
  BinaryCatalogMetadata fields;
};

struct TransactionOutcome {
  std::string outcome = "unknown";
  EngineUuid transaction_uuid;
  std::uint64_t commit_sequence = 0;
  std::uint64_t begin_visible_through_commit_sequence = 0;
  bool committed_row_effects = true;
  bool folded_to_no_effect = false;
  bool external_exposure_observed = true;
};

struct WindowPlan {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::int64_t first_value = 0;
  std::int64_t last_value = 0;
  std::int64_t durable_next_value = 0;
  bool durable_exhausted = false;
  std::uint64_t value_count = 0;
};

std::string EventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.sequence_generator_events";
}

std::uint64_t ParseU64(const std::string& value, std::uint64_t fallback = 0) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

std::int64_t ParseI64(const std::string& value, std::int64_t fallback = 0) {
  try {
    return static_cast<std::int64_t>(std::stoll(value));
  } catch (...) {
    return fallback;
  }
}

bool ParseBool(const std::string& value, bool fallback = false) {
  if (value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on") { return true; }
  if (value == "0" || value == "false" || value == "FALSE" || value == "no" || value == "off") { return false; }
  return fallback;
}

std::string BoolText(bool value) { return value ? "1" : "0"; }

std::string Field(const BinaryCatalogMetadata& fields,
                  const std::string& key,
                  std::string fallback = {}) {
  const auto it = fields.text.find(key);
  return it == fields.text.end() ? std::move(fallback) : it->second;
}

std::uint64_t FieldU64(const BinaryCatalogMetadata& fields,
                       const std::string& key,
                       std::uint64_t fallback = 0) {
  return ParseU64(Field(fields, key), fallback);
}

std::int64_t FieldI64(const BinaryCatalogMetadata& fields,
                      const std::string& key,
                      std::int64_t fallback = 0) {
  return ParseI64(Field(fields, key), fallback);
}

bool FieldBool(const BinaryCatalogMetadata& fields,
               const std::string& key,
               bool fallback = false) {
  return ParseBool(Field(fields, key), fallback);
}

EngineApiDiagnostic SequenceDiagnostic(const char* code, std::string detail = {}, bool error = true) {
  std::string message_key = "generator.lifecycle";
  const std::string diagnostic_code = code;
  if (diagnostic_code == kSequenceDiagnosticOk) { message_key = "engine.api.ok"; }
  else if (diagnostic_code == kSequenceDiagnosticDatabasePathRequired) { message_key = "generator.database_path_required"; }
  else if (diagnostic_code == kSequenceDiagnosticMgaTransactionRequired) { message_key = "generator.mga_transaction_required"; }
  else if (diagnostic_code == kSequenceDiagnosticUuidInvalid) { message_key = "generator.uuid_invalid"; }
  else if (diagnostic_code == kSequenceDiagnosticDuplicateUuid) { message_key = "generator.duplicate_uuid"; }
  else if (diagnostic_code == kSequenceDiagnosticNotFound) { message_key = "generator.not_found"; }
  else if (diagnostic_code == kSequenceDiagnosticDropped) { message_key = "generator.dropped"; }
  else if (diagnostic_code == kSequenceDiagnosticIncrementInvalid) { message_key = "generator.increment_invalid"; }
  else if (diagnostic_code == kSequenceDiagnosticRangeInvalid) { message_key = "generator.range_invalid"; }
  else if (diagnostic_code == kSequenceDiagnosticCacheInvalid) { message_key = "generator.cache_invalid"; }
  else if (diagnostic_code == kSequenceDiagnosticExhausted) { message_key = "generator.exhausted"; }
  else if (diagnostic_code == kSequenceDiagnosticUnavailable) { message_key = "generator.unavailable"; }
  else if (diagnostic_code == kSequenceDiagnosticClusterPathAbsent) { message_key = "generator.cluster_path_absent"; }
  else if (diagnostic_code == kSequenceDiagnosticReferenceMappingIncomplete) { message_key = "generator.reference_mapping_incomplete"; }
  else if (diagnostic_code == kSequenceDiagnosticIdentityDoubleUuidForbidden) {
    message_key = "generator.identity_double_uuid_forbidden";
  } else if (diagnostic_code == kSequenceDiagnosticIdentityAssigned) {
    message_key = "generator.row_uuid_identity_assigned";
  } else if (diagnostic_code == kSequenceDiagnosticMgaRetentionBlocked) {
    message_key = "generator.mga_retention_blocked";
  } else if (diagnostic_code == kSequenceDiagnosticPolicyResolutionFailed) {
    message_key = "generator.policy_resolution_failed";
  } else if (diagnostic_code == kSequenceDiagnosticDatabaseWriteFailed) {
    message_key = "generator.database_write_failed";
  }
  return {diagnostic_code, std::move(message_key), std::move(detail), error};
}

EngineApiDiagnostic SequenceDiagnostic(const char* code, const EngineUuid& identity, bool error = true) {
  auto diagnostic = SequenceDiagnostic(code, std::string{}, error);
  diagnostic.identity_fields.emplace_back("generator_uuid", identity);
  return diagnostic;
}

EngineApiDiagnostic OkDiagnostic() { return SequenceDiagnostic(kSequenceDiagnosticOk, std::string{}, false); }

template <typename TResult>
TResult SuccessResult(const EngineRequestContext& context, std::string operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
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
  result.cluster_authority_required = diagnostic.code == kSequenceDiagnosticClusterPathAbsent ||
                                      diagnostic.code == kSequenceDiagnosticUnavailable;
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

using SequenceFields = std::vector<std::pair<std::string, EngineEvidenceValue>>;

void AddRow(EngineApiResult* result, SequenceFields fields) {
  ApiBehaviorFields values;
  for (auto& [key, value] : fields)
    std::visit([&](auto&& v) { values.emplace_back(key, v); }, value);
  result->result_shape.result_kind = "sequence_generator_lifecycle_rows";
  result->result_shape.rows.push_back(ApiBehaviorRow(std::move(values)));
}

void AddEvidence(EngineApiResult* result, std::string kind, EngineEvidenceValue id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

EngineUuid IssueSequenceIdentity() {
  return scratchbird::core::uuid::IssueRuntimeIdentityV7().value_or(EngineUuid{});
}

EngineApiDiagnostic AppendEvent(const EngineRequestContext& context,
                                std::string event_kind,
                                std::uint64_t creator_tx,
                                SequenceFields fields) {
  if (context.database_path.empty())
    return SequenceDiagnostic(kSequenceDiagnosticDatabasePathRequired, "database_path");
  BinaryCatalogMetadata metadata;
  metadata.text.emplace("event_kind", std::move(event_kind));
  metadata.text.emplace("creator_tx", std::to_string(creator_tx));
  for (const auto& [key, value] : fields) {
    if (metadata.text.contains(key) || metadata.identities.contains(key))
      return SequenceDiagnostic(kSequenceDiagnosticDatabaseWriteFailed, "duplicate_event_field");
    if (const auto* uuid = std::get_if<EngineUuid>(&value)) metadata.identities.emplace(key, *uuid);
    else metadata.text.emplace(key, std::get<std::string>(value));
  }
  std::string bytes;
  if (!EncodeBinaryCatalogMetadata(metadata, kSequenceGeneratorLifecycleEventMagic, &bytes))
    return SequenceDiagnostic(kSequenceDiagnosticDatabaseWriteFailed, "invalid_event_fields");
  std::string framed;
  AppendBinaryU32(&framed, static_cast<std::uint32_t>(bytes.size()));
  framed += bytes;
  static std::mutex append_mutex;
  const std::lock_guard lock(append_mutex);
  const auto path = EventPath(context);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return SequenceDiagnostic(kSequenceDiagnosticDatabaseWriteFailed, "open");
  out.write(framed.data(), static_cast<std::streamsize>(framed.size()));
  out.flush();
  out.close();
  if (!out || !scratchbird::storage::disk::SyncFilesystemPath(path, true).ok() ||
      !scratchbird::storage::disk::SyncParentDirectoryPath(path).ok())
    return SequenceDiagnostic(kSequenceDiagnosticDatabaseWriteFailed, "sync");
  return OkDiagnostic();
}

EngineApiDiagnostic AppendDiagnosticEvent(const EngineRequestContext& context,
                                          const std::string& code,
                                          const EngineUuid& generator_uuid,
                                          const std::string& detail) {
  if (context.database_path.empty()) { return OkDiagnostic(); }
  return AppendEvent(context,
                     "DIAGNOSTIC",
                     context.local_transaction_id,
                     {{"diagnostic_code", code},
                      {"generator_uuid", generator_uuid},
                      {"transaction_uuid", context.transaction_uuid},
                      {"detail", detail}});
}

EngineLoadSequenceGeneratorLifecycleStateResult ReadRawEvents(
    const EngineRequestContext& context,
    std::vector<RawSequenceEvent>* events) {
  EngineLoadSequenceGeneratorLifecycleStateResult result;
  if (context.database_path.empty()) {
    result.diagnostic = SequenceDiagnostic(kSequenceDiagnosticDatabasePathRequired, "database_path");
    return result;
  }
  std::ifstream in(EventPath(context), std::ios::binary);
  if (!in) {
    std::error_code error;
    if (std::filesystem::exists(EventPath(context), error) || error) {
      result.diagnostic = SequenceDiagnostic(kSequenceDiagnosticDatabaseWriteFailed, "open_read");
      return result;
    }
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  std::vector<RawSequenceEvent> decoded;
  while (in.peek() != std::char_traits<char>::eof()) {
    std::array<std::uint8_t, 4> prefix{};
    in.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
    std::uint32_t size = 0;
    std::size_t cursor = 0;
    if (!in || !ReadBinaryU32(prefix, &cursor, &size) || size > kApiBehaviorRecordMaximumBytes || size < 20) {
      result.diagnostic = SequenceDiagnostic(kSequenceDiagnosticDatabaseWriteFailed, "invalid_event_frame");
      return result;
    }
    std::string bytes(size, '\0');
    in.read(bytes.data(), size);
    RawSequenceEvent event;
    if (!in || !DecodeBinaryCatalogMetadata(bytes, kSequenceGeneratorLifecycleEventMagic, &event.fields) ||
        !event.fields.text.contains("event_kind") || !event.fields.text.contains("creator_tx")) {
      result.diagnostic = SequenceDiagnostic(kSequenceDiagnosticDatabaseWriteFailed, "invalid_event_record");
      return result;
    }
    event.event_sequence = decoded.size() + 1;
    event.event_kind = Field(event.fields, "event_kind");
    event.creator_tx = FieldU64(event.fields, "creator_tx");
    decoded.push_back(std::move(event));
  }
  if (in.bad()) {
    result.diagnostic = SequenceDiagnostic(kSequenceDiagnosticDatabaseWriteFailed, "read");
    return result;
  }
  *events = std::move(decoded);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

bool ValidateMutatingContext(const EngineRequestContext& context, EngineApiDiagnostic* diagnostic) {
  const auto savepoint = AdmitMgaSavepointProducer(context, MgaMutationProducer::sequence_default);
  if (savepoint.error) { *diagnostic = savepoint; return false; }
  if (context.database_path.empty()) {
    *diagnostic = SequenceDiagnostic(kSequenceDiagnosticDatabasePathRequired, "database_path");
    return false;
  }
  if (context.local_transaction_id == 0) {
    *diagnostic = SequenceDiagnostic(kSequenceDiagnosticMgaTransactionRequired, "local_transaction_id");
    return false;
  }
  const auto inventory = scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (inventory.ok()) {
    for (const auto& entry : inventory.inventory.entries) {
      if (entry.identity.local_id.value == context.local_transaction_id &&
          entry.identity.transaction_uuid.value == context.transaction_uuid &&
          entry.state == scratchbird::transaction::mga::TransactionState::active && !entry.rollback_only)
        return true;
    }
  }
  *diagnostic = SequenceDiagnostic(kSequenceDiagnosticMgaTransactionRequired, "active_inventory_owner_required");
  return false;
}

EngineUuid GeneratorUuidFromRequest(const EngineApiRequest& request, const EngineUuid& explicit_uuid) {
  if (!explicit_uuid.is_nil()) { return explicit_uuid; }
  if (!request.target_object.uuid.is_nil()) { return request.target_object.uuid; }
  if (!request.bound_object_identity.object_uuid.is_nil()) {
    return request.bound_object_identity.object_uuid;
  }
  return {};
}

EngineSequenceGeneratorDefinition NormalizeDefinition(const EngineSequenceCreateGeneratorRequest& request) {
  auto definition = request.definition;
  if (definition.generator_uuid.is_nil()) {
    definition.generator_uuid = GeneratorUuidFromRequest(request, {});
  }
  if (definition.database_uuid.is_nil()) { definition.database_uuid = request.context.database_uuid; }
  if (definition.schema_uuid.is_nil()) { definition.schema_uuid = request.target_schema.uuid; }
  if (definition.allocation_mode.empty()) { definition.allocation_mode = "local_node_generator"; }
  if (definition.value_type_uuid.is_nil()) { definition.value_type_uuid = kSequenceInt64TypeUuid; }
  if (definition.cache_size == 0) { definition.cache_size = 1; }
  if (definition.policy_generation == 0) { definition.policy_generation = 1; }
  return definition;
}

EngineSequenceGeneratorDefinition NormalizeDefinition(const EngineSequenceAlterGeneratorRequest& request) {
  EngineSequenceCreateGeneratorRequest create_request;
  create_request.context = request.context;
  create_request.target_object = request.target_object;
  create_request.target_schema = request.target_schema;
  create_request.bound_object_identity = request.bound_object_identity;
  create_request.definition = request.definition;
  return NormalizeDefinition(create_request);
}

bool ReferenceMappingIncomplete(const EngineSequenceGeneratorDefinition& definition) {
  if (definition.reference_profile_uuid.is_nil()) { return false; }
  return !definition.reference_mapping_complete ||
         definition.reference_family.empty() ||
         definition.reference_mapping_label.empty() ||
         definition.reference_allocation_timing.empty() ||
         definition.reference_rollback_behavior.empty() ||
         definition.reference_finality_behavior.empty() ||
         definition.reference_cache_behavior.empty();
}

EngineApiDiagnostic ValidateDefinition(const EngineRequestContext& context,
                                       const EngineSequenceGeneratorDefinition& definition,
                                       bool validate_cluster_metric_request) {
  if (definition.generator_uuid.is_nil()) {
    return SequenceDiagnostic(kSequenceDiagnosticUuidInvalid, "generator_uuid");
  }
  if (definition.increment_by == 0) {
    return SequenceDiagnostic(kSequenceDiagnosticIncrementInvalid, definition.generator_uuid);
  }
  if (definition.min_value > definition.max_value ||
      definition.start_value < definition.min_value ||
      definition.start_value > definition.max_value) {
    return SequenceDiagnostic(kSequenceDiagnosticRangeInvalid, definition.generator_uuid);
  }
  if (definition.cache_size > 1000000ull) {
    return SequenceDiagnostic(kSequenceDiagnosticCacheInvalid, definition.generator_uuid);
  }
  if (definition.allocation_mode.empty()) {
    return SequenceDiagnostic(kSequenceDiagnosticPolicyResolutionFailed, definition.generator_uuid);
  }
  if (ReferenceMappingIncomplete(definition)) {
    return SequenceDiagnostic(kSequenceDiagnosticReferenceMappingIncomplete, definition.generator_uuid);
  }
  if (validate_cluster_metric_request &&
      definition.cluster_metric_path_requested &&
      !context.cluster_authority_available) {
    return SequenceDiagnostic(kSequenceDiagnosticClusterPathAbsent, definition.generator_uuid);
  }
  return OkDiagnostic();
}

std::vector<std::pair<std::string, EngineEvidenceValue>> DefinitionFields(
    const EngineSequenceGeneratorDefinition& definition) {
  return {
      {"generator_uuid", definition.generator_uuid},
      {"database_uuid", definition.database_uuid},
      {"schema_uuid", definition.schema_uuid},
      {"table_uuid", definition.table_uuid},
      {"column_uuid", definition.column_uuid},
      {"constraint_uuid", definition.constraint_uuid},
      {"domain_uuid", definition.domain_uuid},
      {"value_type_uuid", definition.value_type_uuid},
      {"allocation_mode", definition.allocation_mode},
      {"start_value", std::to_string(definition.start_value)},
      {"increment_by", std::to_string(definition.increment_by)},
      {"min_value", std::to_string(definition.min_value)},
      {"max_value", std::to_string(definition.max_value)},
      {"cache_size", std::to_string(definition.cache_size)},
      {"cycle_allowed", BoolText(definition.cycle_allowed)},
      {"gapless_required", BoolText(definition.gapless_required)},
      {"transactional_allocation", BoolText(definition.transactional_allocation)},
      {"reusable_if_no_effect", BoolText(definition.reusable_if_no_effect)},
      {"consumed_on_rollback", BoolText(definition.consumed_on_rollback)},
      {"policy_uuid", definition.policy_uuid},
      {"policy_version_uuid", definition.policy_version_uuid},
      {"policy_generation", std::to_string(definition.policy_generation)},
      {"reference_profile_uuid", definition.reference_profile_uuid},
      {"reference_family", definition.reference_family},
      {"reference_mapping_label", definition.reference_mapping_label},
      {"reference_allocation_timing", definition.reference_allocation_timing},
      {"reference_rollback_behavior", definition.reference_rollback_behavior},
      {"reference_finality_behavior", definition.reference_finality_behavior},
      {"reference_cache_behavior", definition.reference_cache_behavior},
      {"reference_mapping_complete", BoolText(definition.reference_mapping_complete)},
      {"requires_cluster_authority", BoolText(definition.requires_cluster_authority)},
      {"cluster_metric_path_requested", BoolText(definition.cluster_metric_path_requested)}};
}

EngineSequenceGeneratorDefinition DefinitionFromFields(const BinaryCatalogMetadata& fields) {
  EngineSequenceGeneratorDefinition definition;
  definition.generator_uuid = BinaryCatalogUuid(fields, "generator_uuid");
  definition.database_uuid = BinaryCatalogUuid(fields, "database_uuid");
  definition.schema_uuid = BinaryCatalogUuid(fields, "schema_uuid");
  definition.table_uuid = BinaryCatalogUuid(fields, "table_uuid");
  definition.column_uuid = BinaryCatalogUuid(fields, "column_uuid");
  definition.constraint_uuid = BinaryCatalogUuid(fields, "constraint_uuid");
  definition.domain_uuid = BinaryCatalogUuid(fields, "domain_uuid");
  definition.value_type_uuid = BinaryCatalogUuid(fields, "value_type_uuid", kSequenceInt64TypeUuid);
  definition.allocation_mode = Field(fields, "allocation_mode", "local_node_generator");
  definition.start_value = FieldI64(fields, "start_value", 1);
  definition.increment_by = FieldI64(fields, "increment_by", 1);
  definition.min_value = FieldI64(fields, "min_value", std::numeric_limits<std::int64_t>::min());
  definition.max_value = FieldI64(fields, "max_value", std::numeric_limits<std::int64_t>::max());
  definition.cache_size = FieldU64(fields, "cache_size", 32);
  definition.cycle_allowed = FieldBool(fields, "cycle_allowed");
  definition.gapless_required = FieldBool(fields, "gapless_required");
  definition.transactional_allocation = FieldBool(fields, "transactional_allocation");
  definition.reusable_if_no_effect = FieldBool(fields, "reusable_if_no_effect");
  definition.consumed_on_rollback = FieldBool(fields, "consumed_on_rollback", true);
  definition.policy_uuid = BinaryCatalogUuid(fields, "policy_uuid");
  definition.policy_version_uuid = BinaryCatalogUuid(fields, "policy_version_uuid");
  definition.policy_generation = FieldU64(fields, "policy_generation", 1);
  definition.reference_profile_uuid = BinaryCatalogUuid(fields, "reference_profile_uuid");
  definition.reference_family = Field(fields, "reference_family");
  definition.reference_mapping_label = Field(fields, "reference_mapping_label");
  definition.reference_allocation_timing = Field(fields, "reference_allocation_timing");
  definition.reference_rollback_behavior = Field(fields, "reference_rollback_behavior");
  definition.reference_finality_behavior = Field(fields, "reference_finality_behavior");
  definition.reference_cache_behavior = Field(fields, "reference_cache_behavior");
  definition.reference_mapping_complete = FieldBool(fields, "reference_mapping_complete");
  definition.requires_cluster_authority = FieldBool(fields, "requires_cluster_authority");
  definition.cluster_metric_path_requested = FieldBool(fields, "cluster_metric_path_requested");
  return definition;
}

EngineSequenceGeneratorRecord* FindGenerator(EngineSequenceGeneratorLifecycleState* state,
                                             const EngineUuid& generator_uuid) {
  for (auto& generator : state->generators) {
    if (generator.definition.generator_uuid == generator_uuid) { return &generator; }
  }
  return nullptr;
}

const EngineSequenceGeneratorRecord* FindGenerator(const EngineSequenceGeneratorLifecycleState& state,
                                                   const EngineUuid& generator_uuid) {
  for (const auto& generator : state.generators) {
    if (generator.definition.generator_uuid == generator_uuid) { return &generator; }
  }
  return nullptr;
}

bool DdlEventVisible(const EngineRequestContext& context,
                     std::uint64_t creator_tx,
                     const std::map<std::uint64_t, TransactionOutcome>& outcomes) {
  const auto outcome = outcomes.find(creator_tx);
  if (creator_tx == 0 || outcome == outcomes.end()) return false;
  const auto reader = outcomes.find(context.local_transaction_id);
  if (context.local_transaction_id != 0) {
    if (reader == outcomes.end() || reader->second.transaction_uuid != context.transaction_uuid ||
        (reader->second.outcome != "active" && reader->second.outcome != "read_only_active")) return false;
    if (creator_tx == context.local_transaction_id) return true;
  }
  if (outcome->second.outcome != "committed") return false;
  if (reader != outcomes.end() && outcome->second.commit_sequence > reader->second.begin_visible_through_commit_sequence)
    return false;
  if (context.snapshot_visible_through_local_transaction_id != 0 &&
      creator_tx > context.snapshot_visible_through_local_transaction_id) return false;
  return true;
}

bool ValueInRange(const EngineSequenceGeneratorDefinition& definition, std::int64_t value) {
  return value >= definition.min_value && value <= definition.max_value;
}

std::optional<std::int64_t> SteppedValue(std::int64_t value, std::int64_t increment) {
  if (increment > 0 && value > std::numeric_limits<std::int64_t>::max() - increment) {
    return std::nullopt;
  }
  if (increment < 0 && value < std::numeric_limits<std::int64_t>::min() - increment) {
    return std::nullopt;
  }
  return value + increment;
}

std::optional<std::int64_t> NextValueForDefinition(const EngineSequenceGeneratorDefinition& definition,
                                                   std::int64_t value,
                                                   bool* exhausted) {
  *exhausted = false;
  const auto stepped = SteppedValue(value, definition.increment_by);
  if (!stepped.has_value()) {
    if (definition.cycle_allowed) {
      return definition.increment_by > 0 ? definition.min_value : definition.max_value;
    }
    *exhausted = true;
    return std::nullopt;
  }
  if (ValueInRange(definition, *stepped)) { return *stepped; }
  if (definition.cycle_allowed) {
    return definition.increment_by > 0 ? definition.min_value : definition.max_value;
  }
  *exhausted = true;
  return std::nullopt;
}

bool CacheValueWithinWindow(const EngineSequenceGeneratorRecord& generator, std::int64_t value) {
  if (!generator.cache_window_active) { return false; }
  if (generator.definition.increment_by > 0) {
    return value >= generator.cache_window_first_value && value <= generator.cache_window_last_value;
  }
  return value <= generator.cache_window_first_value && value >= generator.cache_window_last_value;
}

bool CacheWindowConsumedAfter(const EngineSequenceGeneratorRecord& generator, std::int64_t value) {
  return value == generator.cache_window_last_value;
}

WindowPlan PlanWindow(const EngineSequenceGeneratorRecord& generator) {
  WindowPlan plan;
  if (generator.durable_exhausted && !generator.cache_window_active) {
    plan.diagnostic = SequenceDiagnostic(kSequenceDiagnosticExhausted, generator.definition.generator_uuid);
    return plan;
  }
  std::int64_t first = generator.durable_next_value_present
                           ? generator.durable_next_value
                           : generator.definition.start_value;
  if (!ValueInRange(generator.definition, first)) {
    if (generator.definition.cycle_allowed) {
      first = generator.definition.increment_by > 0
                  ? generator.definition.min_value
                  : generator.definition.max_value;
    } else {
      plan.diagnostic = SequenceDiagnostic(kSequenceDiagnosticExhausted, generator.definition.generator_uuid);
      return plan;
    }
  }

  const std::uint64_t cache_size = generator.definition.cache_size == 0 ? 1 : generator.definition.cache_size;
  std::int64_t last = first;
  std::uint64_t count = 1;
  for (; count < cache_size; ++count) {
    bool exhausted = false;
    const auto next = NextValueForDefinition(generator.definition, last, &exhausted);
    if (!next.has_value() || exhausted) { break; }
    last = *next;
  }

  bool durable_exhausted = false;
  const auto next_after = NextValueForDefinition(generator.definition, last, &durable_exhausted);
  plan.ok = true;
  plan.first_value = first;
  plan.last_value = last;
  plan.durable_next_value = next_after.value_or(last);
  plan.durable_exhausted = durable_exhausted || !next_after.has_value();
  plan.value_count = count;
  return plan;
}

bool BuildOutcomeMap(const EngineRequestContext& context,
                     const std::vector<RawSequenceEvent>& events,
                     std::map<std::uint64_t, TransactionOutcome>* outcomes) {
  const auto loaded = scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded.ok()) return false;
  using scratchbird::transaction::mga::TransactionState;
  for (const auto& entry : loaded.inventory.entries) {
    TransactionOutcome outcome;
    outcome.transaction_uuid = entry.identity.transaction_uuid.value;
    outcome.commit_sequence = entry.commit_sequence;
    outcome.begin_visible_through_commit_sequence = entry.begin_visible_through_commit_sequence;
    switch (scratchbird::transaction::mga::InventoryVisibilityState(entry)) {
      case TransactionState::committed: outcome.outcome = "committed"; break;
      case TransactionState::rolled_back: outcome.outcome = "rolled_back"; break;
      case TransactionState::active: outcome.outcome = "active"; break;
      case TransactionState::read_only_active: outcome.outcome = "read_only_active"; break;
      default: break;
    }
    outcomes->emplace(entry.identity.local_id.value, outcome);
  }
  // Events annotate row effects only after the durable inventory proves finality.
  for (const auto& event : events) {
    if (event.event_kind != "MGA_OUTCOME") continue;
    const auto it = outcomes->find(FieldU64(event.fields, "outcome_local_transaction_id"));
    const auto declared = Field(event.fields, "mga_outcome");
    if (it == outcomes->end() || it->second.transaction_uuid != BinaryCatalogUuid(event.fields, "outcome_transaction_uuid") ||
        (declared != it->second.outcome && !(declared == "archived" && it->second.outcome == "committed"))) return false;
    it->second.committed_row_effects = FieldBool(event.fields, "committed_row_effects", true);
    it->second.folded_to_no_effect = FieldBool(event.fields, "folded_to_no_effect", false);
    it->second.external_exposure_observed = FieldBool(event.fields, "external_exposure_observed", true);
  }
  return true;
}

void ApplyOutcomeToAllocation(const TransactionOutcome& outcome,
                              EngineSequenceAllocationRecord* allocation,
                              EngineSequenceGeneratorMetrics* metrics) {
  allocation->transaction_outcome = outcome.outcome;
  if (outcome.outcome == "rolled_back") {
    if (allocation->consumed_on_rollback) {
      allocation->allocation_finality = "allocated_consumed_no_row_effect";
      allocation->lifecycle_state = "rolled_back_consumed";
      ++metrics->rolled_back_consumed_total;
      ++metrics->consumed_no_row_effect_total;
    } else if (allocation->reusable_if_no_effect && !outcome.external_exposure_observed) {
      allocation->allocation_finality = "released";
      allocation->lifecycle_state = "released";
      allocation->released = true;
      ++metrics->reservations_released_total;
    }
    return;
  }
  if (outcome.outcome == "committed" || outcome.outcome == "archived") {
    allocation->row_effect_committed = outcome.committed_row_effects && !outcome.folded_to_no_effect;
    if (!allocation->row_effect_committed) {
      if (allocation->consumed_on_rollback) {
        allocation->allocation_finality = "allocated_consumed_no_row_effect";
        allocation->lifecycle_state = "consumed_no_row_effect";
        ++metrics->consumed_no_row_effect_total;
      } else if (allocation->reusable_if_no_effect && !outcome.external_exposure_observed) {
        allocation->allocation_finality = "released";
        allocation->lifecycle_state = "released";
        allocation->released = true;
        ++metrics->reservations_released_total;
      }
    } else if (allocation->allocation_mode == "preallocated_partition_range") {
      allocation->allocation_finality = "local_partition_allocated";
      allocation->lifecycle_state = "committed_allocated";
    } else {
      allocation->allocation_finality = "allocated_committed";
      allocation->lifecycle_state = "committed_allocated";
    }
  }
}

void ApplyOutcomeState(EngineSequenceGeneratorLifecycleState* state,
                       const std::map<std::uint64_t, TransactionOutcome>& outcomes) {
  for (auto& allocation : state->allocations) {
    const auto outcome = outcomes.find(allocation.local_transaction_id);
    if (outcome != outcomes.end() && outcome->second.transaction_uuid == allocation.transaction_uuid) {
      ApplyOutcomeToAllocation(outcome->second, &allocation, &state->metrics);
    }
  }
}

void EnsureMetricPaths(EngineSequenceGeneratorLifecycleState* state) {
  state->metrics.local_metric_paths = {
      "sys.metrics.generator.policy_resolution_total",
      "sys.metrics.generator.cache_windows_reserved_total",
      "sys.metrics.generator.allocations_total",
      "sys.metrics.generator.consumed_no_row_effect_total",
      "sys.metrics.generator.recovery_snapshots_total",
      "sys.metrics.generator.exhaustion_refusals_total",
      "sys.metrics.generator.reference_mapping_reject_total",
      "sys.metrics.generator.cluster_path_reject_total",
      "sys.metrics.generator.mga_retention_blocked_total"};
}

EngineSequenceAllocationRecord AllocationFromFields(const RawSequenceEvent& event) {
  EngineSequenceAllocationRecord allocation;
  allocation.creator_tx = event.creator_tx;
  allocation.event_sequence = event.event_sequence;
  allocation.local_transaction_id = FieldU64(event.fields, "local_transaction_id", event.creator_tx);
  allocation.sequence_epoch = FieldU64(event.fields, "sequence_epoch", event.event_sequence);
  allocation.cache_window_generation = FieldU64(event.fields, "cache_window_generation");
  allocation.allocation_uuid = BinaryCatalogUuid(event.fields, "allocation_uuid");
  allocation.reservation_uuid = BinaryCatalogUuid(event.fields, "reservation_uuid");
  allocation.generator_uuid = BinaryCatalogUuid(event.fields, "generator_uuid");
  allocation.table_uuid = BinaryCatalogUuid(event.fields, "table_uuid");
  allocation.column_uuid = BinaryCatalogUuid(event.fields, "column_uuid");
  allocation.statement_uuid = BinaryCatalogUuid(event.fields, "statement_uuid");
  allocation.record_uuid = BinaryCatalogUuid(event.fields, "record_uuid");
  allocation.transaction_uuid = BinaryCatalogUuid(event.fields, "transaction_uuid");
  allocation.allocated_value = FieldI64(event.fields, "allocated_value");
  allocation.allocation_mode = Field(event.fields, "allocation_mode", "local_node_generator");
  allocation.allocation_finality = Field(event.fields, "allocation_finality", "allocated_uncommitted");
  allocation.lifecycle_state = Field(event.fields, "lifecycle_state", "allocated");
  allocation.consumed_on_rollback = FieldBool(event.fields, "consumed_on_rollback", true);
  allocation.reusable_if_no_effect = FieldBool(event.fields, "reusable_if_no_effect", false);
  allocation.external_exposure_allowed = FieldBool(event.fields, "external_exposure_allowed", true);
  allocation.row_effect_expected = FieldBool(event.fields, "row_effect_expected", true);
  allocation.unique_validation_required = FieldBool(event.fields, "unique_validation_required", true);
  allocation.reference_mapping_label = Field(event.fields, "reference_mapping_label");
  allocation.cache_window_first_value = FieldI64(event.fields, "cache_window_first_value");
  allocation.cache_window_last_value = FieldI64(event.fields, "cache_window_last_value");
  allocation.durable_high_water_after = FieldI64(event.fields, "durable_high_water_after");
  return allocation;
}

EngineIdentityValueBindingRecord BindingFromFields(const RawSequenceEvent& event) {
  EngineIdentityValueBindingRecord binding;
  binding.creator_tx = event.creator_tx;
  binding.event_sequence = event.event_sequence;
  binding.identity_binding_uuid = BinaryCatalogUuid(event.fields, "identity_binding_uuid");
  binding.generator_uuid = BinaryCatalogUuid(event.fields, "generator_uuid");
  binding.allocation_uuid = BinaryCatalogUuid(event.fields, "allocation_uuid");
  binding.table_uuid = BinaryCatalogUuid(event.fields, "table_uuid");
  binding.record_uuid = BinaryCatalogUuid(event.fields, "record_uuid");
  binding.identity_column_uuid = BinaryCatalogUuid(event.fields, "identity_column_uuid");
  binding.identity_value_kind = Field(event.fields, "identity_value_kind");
  binding.identity_value = event.fields.identities.contains("identity_value")
      ? EngineEvidenceValue{BinaryCatalogUuid(event.fields, "identity_value")}
      : EngineEvidenceValue{Field(event.fields, "identity_value")};
  binding.binding_finality = Field(event.fields, "binding_finality", "allocated_uncommitted");
  binding.transaction_uuid = BinaryCatalogUuid(event.fields, "transaction_uuid");
  binding.local_transaction_id = FieldU64(event.fields, "local_transaction_id", event.creator_tx);
  return binding;
}

void FillGeneratorResult(EngineApiResult* result, const EngineSequenceGeneratorRecord& generator) {
  result->primary_object.uuid = generator.definition.generator_uuid;
  result->primary_object.object_kind = "sequence_generator";
  // This allocation record does not claim a catalog-row identity.
  AddEvidence(result, "sequence_generator_lifecycle", generator.definition.generator_uuid);
  AddEvidence(result, "sequence_policy_cache", "bounded_nonfinality_cache_v1");
  AddEvidence(result, "mga_transaction_authority", "allocation_not_transaction_finality");
  AddRow(result,
         {{"generator_uuid", generator.definition.generator_uuid},
          {"lifecycle_state", generator.lifecycle_state},
          {"allocation_mode", generator.definition.allocation_mode},
          {"durable_next_value", std::to_string(generator.durable_next_value)},
          {"cache_window_generation", std::to_string(generator.cache_window_generation)},
          {"cache_window_active", BoolText(generator.cache_window_active)},
          {"metadata_epoch", std::to_string(generator.metadata_epoch)}});
}

void FillAllocationResult(EngineApiResult* result, const EngineSequenceAllocationRecord& allocation) {
  AddEvidence(result, "sequence_allocation", allocation.allocation_uuid);
  AddEvidence(result, "sequence_reservation", allocation.reservation_uuid);
  AddEvidence(result, "cache_window_generation", std::to_string(allocation.cache_window_generation));
  AddEvidence(result, "unique_validation_required", allocation.unique_validation_required ? "true" : "false");
  if (!allocation.reference_mapping_label.empty()) {
    AddEvidence(result, "reference_mapping_label", allocation.reference_mapping_label);
  }
  AddRow(result,
         {{"allocation_uuid", allocation.allocation_uuid},
          {"generator_uuid", allocation.generator_uuid},
          {"allocated_value", std::to_string(allocation.allocated_value)},
          {"allocation_finality", allocation.allocation_finality},
          {"durable_high_water_after", std::to_string(allocation.durable_high_water_after)}});
}

}  // namespace

EngineLoadSequenceGeneratorLifecycleStateResult LoadSequenceGeneratorLifecycleState(
    const EngineRequestContext& context) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(context.database_path);
  std::vector<RawSequenceEvent> events;
  auto read = ReadRawEvents(context, &events);
  if (!read.ok) { return read; }

  EngineLoadSequenceGeneratorLifecycleStateResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.state.max_event_sequence = events.empty() ? 0 : events.back().event_sequence;
  EnsureMetricPaths(&result.state);

  std::map<std::uint64_t, TransactionOutcome> outcomes;
  if (!BuildOutcomeMap(context, events, &outcomes)) {
    result.ok = false;
    result.diagnostic = SequenceDiagnostic(kSequenceDiagnosticMgaTransactionRequired, "durable_inventory_or_annotation_invalid");
    return result;
  }
  std::vector<EngineUuid> released_allocations;
  for (const auto& event : events) {
    result.state.max_event_sequence = std::max(result.state.max_event_sequence, event.event_sequence);
    if (event.event_kind == "CREATE") {
      ++result.state.metrics.policy_resolution_total;
      if (!DdlEventVisible(context, event.creator_tx, outcomes)) { continue; }
      EngineSequenceGeneratorRecord record;
      record.creator_tx = event.creator_tx;
      record.event_sequence = event.event_sequence;
      record.metadata_epoch = event.event_sequence;
      record.definition = DefinitionFromFields(event.fields);
      record.lifecycle_state = "active";
      record.durable_next_value = record.definition.start_value;
      record.durable_next_value_present = true;
      record.durable_exhausted = false;
      record.cache_window_active = false;
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (FindGenerator(&result.state, record.definition.generator_uuid) == nullptr) {
        result.state.generators.push_back(std::move(record));
      }
    } else if (event.event_kind == "ALTER" || event.event_kind == "RESTART") {
      if (!DdlEventVisible(context, event.creator_tx, outcomes)) { continue; }
      const EngineUuid generator_uuid = BinaryCatalogUuid(event.fields, "generator_uuid");
      auto* generator = FindGenerator(&result.state, generator_uuid);
      if (generator == nullptr) { continue; }
      EngineSequenceGeneratorDefinition replacement = DefinitionFromFields(event.fields);
      if (replacement.generator_uuid.is_nil()) { replacement.generator_uuid = generator_uuid; }
      generator->definition = replacement;
      generator->metadata_epoch = event.event_sequence;
      generator->event_sequence = event.event_sequence;
      generator->cache_window_active = false;
      generator->cache_window_generation = FieldU64(event.fields, "cache_window_generation", generator->cache_window_generation + 1);
      if (event.event_kind == "RESTART" || FieldBool(event.fields, "restart_with_value")) {
        generator->durable_next_value = FieldI64(event.fields, "restart_value", generator->definition.start_value);
        generator->durable_next_value_present = true;
        generator->durable_exhausted = false;
      }
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, generator->metadata_epoch);
    } else if (event.event_kind == "DROP") {
      if (!DdlEventVisible(context, event.creator_tx, outcomes)) { continue; }
      const EngineUuid generator_uuid = BinaryCatalogUuid(event.fields, "generator_uuid");
      auto* generator = FindGenerator(&result.state, generator_uuid);
      if (generator == nullptr) { continue; }
      generator->metadata_epoch = event.event_sequence;
      generator->event_sequence = event.event_sequence;
      generator->lifecycle_state = "dropped";
      generator->dropped = true;
      generator->cache_window_active = false;
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, generator->metadata_epoch);
    } else if (event.event_kind == "CACHE_WINDOW") {
      auto* generator = FindGenerator(&result.state, BinaryCatalogUuid(event.fields, "generator_uuid"));
      if (generator == nullptr) { continue; }
      ++result.state.metrics.cache_windows_reserved_total;
      generator->cache_window_generation = FieldU64(event.fields, "cache_window_generation");
      generator->cache_window_first_value = FieldI64(event.fields, "cache_window_first_value");
      generator->cache_window_last_value = FieldI64(event.fields, "cache_window_last_value");
      generator->cache_next_value = FieldI64(event.fields, "cache_next_value");
      generator->cache_window_active = FieldBool(event.fields, "cache_window_active", true);
      generator->durable_next_value = FieldI64(event.fields, "durable_high_water_after");
      generator->durable_next_value_present = true;
      generator->durable_exhausted = FieldBool(event.fields, "durable_exhausted");
    } else if (event.event_kind == "ALLOCATE") {
      auto allocation = AllocationFromFields(event);
      ++result.state.metrics.allocations_total;
      if (allocation.unique_validation_required) { ++result.state.metrics.unique_validation_required_total; }
      auto* generator = FindGenerator(&result.state, allocation.generator_uuid);
      if (generator != nullptr && !FieldBool(event.fields, "reused_released_value", false)) {
        generator->last_allocated_value = allocation.allocated_value;
        generator->last_allocated_value_present = true;
        generator->cache_window_generation = allocation.cache_window_generation;
        generator->cache_window_first_value = allocation.cache_window_first_value;
        generator->cache_window_last_value = allocation.cache_window_last_value;
        generator->cache_next_value = FieldI64(event.fields, "cache_next_after");
        generator->cache_window_active = FieldBool(event.fields, "cache_window_active_after", false);
        generator->durable_next_value = allocation.durable_high_water_after;
        generator->durable_next_value_present = true;
        generator->durable_exhausted = FieldBool(event.fields, "durable_exhausted_after", generator->durable_exhausted);
      }
      result.state.allocations.push_back(std::move(allocation));
    } else if (event.event_kind == "RELEASE_VALUE") {
      auto* generator = FindGenerator(&result.state, BinaryCatalogUuid(event.fields, "generator_uuid"));
      if (generator == nullptr) { continue; }
      const auto allocation_uuid = BinaryCatalogUuid(event.fields, "allocation_uuid");
      if (std::find(released_allocations.begin(), released_allocations.end(), allocation_uuid) != released_allocations.end()) continue;
      const auto allocation = std::find_if(result.state.allocations.begin(), result.state.allocations.end(),
          [&](const auto& entry) { return entry.allocation_uuid == allocation_uuid; });
      if (allocation == result.state.allocations.end()) continue;
      const auto outcome = outcomes.find(allocation->local_transaction_id);
      if (outcome == outcomes.end() || outcome->second.transaction_uuid != allocation->transaction_uuid) continue;
      auto evaluated = *allocation;
      EngineSequenceGeneratorMetrics ignored_metrics;
      ApplyOutcomeToAllocation(outcome->second, &evaluated, &ignored_metrics);
      if (!evaluated.released || evaluated.generator_uuid != generator->definition.generator_uuid ||
          evaluated.allocated_value != FieldI64(event.fields, "released_value")) continue;
      released_allocations.push_back(allocation_uuid);
      generator->reusable_released_values.push_back(evaluated.allocated_value);
    } else if (event.event_kind == "REUSE_VALUE") {
      auto* generator = FindGenerator(&result.state, BinaryCatalogUuid(event.fields, "generator_uuid"));
      if (generator == nullptr) { continue; }
      const std::int64_t reused = FieldI64(event.fields, "reused_value");
      const auto it = std::find(generator->reusable_released_values.begin(),
                                generator->reusable_released_values.end(),
                                reused);
      if (it != generator->reusable_released_values.end()) {
        generator->reusable_released_values.erase(it);
      }
    } else if (event.event_kind == "IDENTITY_BIND") {
      result.state.identity_bindings.push_back(BindingFromFields(event));
    } else if (event.event_kind == "RECOVERY_SNAPSHOT") {
      ++result.state.metrics.recovery_snapshots_total;
      result.state.recovered_from_persisted_state = true;
      result.state.recovery_snapshot_uuid = BinaryCatalogUuid(event.fields, "recovery_snapshot_uuid");
      auto* generator = FindGenerator(&result.state, BinaryCatalogUuid(event.fields, "generator_uuid"));
      if (generator == nullptr) { continue; }
      generator->recovered_from_persisted_state = true;
      generator->recovery_snapshot_uuid = result.state.recovery_snapshot_uuid;
      generator->cache_unused_on_recovery = FieldU64(event.fields, "cache_unused_on_recovery");
      result.state.metrics.recovered_cache_gap_values_total += generator->cache_unused_on_recovery;
      generator->cache_window_active = false;
      generator->cache_next_value = generator->durable_next_value;
    } else if (event.event_kind == "RETENTION_BLOCKED") {
      ++result.state.metrics.mga_retention_blocked_total;
      auto* generator = FindGenerator(&result.state, BinaryCatalogUuid(event.fields, "generator_uuid"));
      if (generator != nullptr) { generator->retained_by_mga_horizon = true; }
    } else if (event.event_kind == "DIAGNOSTIC") {
      const std::string code = Field(event.fields, "diagnostic_code");
      if (code == kSequenceDiagnosticExhausted) { ++result.state.metrics.exhaustion_refusals_total; }
      else if (code == kSequenceDiagnosticReferenceMappingIncomplete) { ++result.state.metrics.reference_mapping_reject_total; }
      else if (code == kSequenceDiagnosticClusterPathAbsent || code == kSequenceDiagnosticUnavailable) {
        ++result.state.metrics.cluster_path_reject_total;
      } else if (code == kSequenceDiagnosticPolicyResolutionFailed) {
        ++result.state.metrics.policy_resolution_reject_total;
      }
    }
  }
  ApplyOutcomeState(&result.state, outcomes);
  return result;
}

EngineSequenceCreateGeneratorResult EngineSequenceCreateGenerator(
    const EngineSequenceCreateGeneratorRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.generator.create";
  EngineApiDiagnostic diagnostic;
  if (!ValidateMutatingContext(request.context, &diagnostic)) {
    return DiagnosticResult<EngineSequenceCreateGeneratorResult>(request.context, kOperation, diagnostic);
  }
  const auto definition = NormalizeDefinition(request);
  diagnostic = ValidateDefinition(request.context, definition, true);
  if (diagnostic.error) {
    AppendDiagnosticEvent(request.context, diagnostic.code, definition.generator_uuid, diagnostic.detail);
    return DiagnosticResult<EngineSequenceCreateGeneratorResult>(request.context, kOperation, diagnostic);
  }
  const auto loaded = LoadSequenceGeneratorLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineSequenceCreateGeneratorResult>(request.context, kOperation, loaded.diagnostic);
  }
  if (FindGenerator(loaded.state, definition.generator_uuid) != nullptr) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticDuplicateUuid, definition.generator_uuid);
    AppendDiagnosticEvent(request.context, diagnostic.code, definition.generator_uuid, diagnostic.detail);
    return DiagnosticResult<EngineSequenceCreateGeneratorResult>(request.context, kOperation, diagnostic);
  }
  auto fields = DefinitionFields(definition);
  fields.push_back({"transaction_uuid", request.context.transaction_uuid});
  const auto appended = AppendEvent(request.context, "CREATE", request.context.local_transaction_id, std::move(fields));
  if (appended.error) {
    return DiagnosticResult<EngineSequenceCreateGeneratorResult>(request.context, kOperation, appended);
  }
  const auto after = LoadSequenceGeneratorLifecycleState(request.context);
  const auto* generator = FindGenerator(after.state, definition.generator_uuid);
  auto result = SuccessResult<EngineSequenceCreateGeneratorResult>(request.context, kOperation);
  if (generator != nullptr) {
    result.generator = *generator;
    result.metadata_cache_epoch = generator->metadata_epoch;
    FillGeneratorResult(&result, *generator);
  }
  return result;
}

EngineSequenceAlterGeneratorResult EngineSequenceAlterGenerator(
    const EngineSequenceAlterGeneratorRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.generator.alter";
  EngineApiDiagnostic diagnostic;
  if (!ValidateMutatingContext(request.context, &diagnostic)) {
    return DiagnosticResult<EngineSequenceAlterGeneratorResult>(request.context, kOperation, diagnostic);
  }
  auto definition = NormalizeDefinition(request);
  diagnostic = ValidateDefinition(request.context, definition, true);
  if (diagnostic.error) {
    AppendDiagnosticEvent(request.context, diagnostic.code, definition.generator_uuid, diagnostic.detail);
    return DiagnosticResult<EngineSequenceAlterGeneratorResult>(request.context, kOperation, diagnostic);
  }
  const auto loaded = LoadSequenceGeneratorLifecycleState(request.context);
  const auto* existing = FindGenerator(loaded.state, definition.generator_uuid);
  if (existing == nullptr) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticNotFound, definition.generator_uuid);
    return DiagnosticResult<EngineSequenceAlterGeneratorResult>(request.context, kOperation, diagnostic);
  }
  if (existing->dropped) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticDropped, definition.generator_uuid);
    return DiagnosticResult<EngineSequenceAlterGeneratorResult>(request.context, kOperation, diagnostic);
  }
  auto fields = DefinitionFields(definition);
  fields.push_back({"restart_with_value", BoolText(request.restart_with_value)});
  fields.push_back({"restart_value", std::to_string(request.restart_value)});
  fields.push_back({"cache_window_generation", std::to_string(existing->cache_window_generation + 1)});
  const auto appended = AppendEvent(request.context, "ALTER", request.context.local_transaction_id, std::move(fields));
  if (appended.error) {
    return DiagnosticResult<EngineSequenceAlterGeneratorResult>(request.context, kOperation, appended);
  }
  const auto after = LoadSequenceGeneratorLifecycleState(request.context);
  const auto* generator = FindGenerator(after.state, definition.generator_uuid);
  auto result = SuccessResult<EngineSequenceAlterGeneratorResult>(request.context, kOperation);
  if (generator != nullptr) {
    result.generator = *generator;
    result.metadata_cache_epoch = generator->metadata_epoch;
    FillGeneratorResult(&result, *generator);
  }
  return result;
}

EngineSequenceRestartGeneratorResult EngineSequenceRestartGenerator(
    const EngineSequenceRestartGeneratorRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.generator.restart";
  EngineApiDiagnostic diagnostic;
  if (!ValidateMutatingContext(request.context, &diagnostic)) {
    return DiagnosticResult<EngineSequenceRestartGeneratorResult>(request.context, kOperation, diagnostic);
  }
  const EngineUuid generator_uuid = GeneratorUuidFromRequest(request, request.generator_uuid);
  const auto loaded = LoadSequenceGeneratorLifecycleState(request.context);
  const auto* existing = FindGenerator(loaded.state, generator_uuid);
  if (existing == nullptr) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticNotFound, generator_uuid);
    return DiagnosticResult<EngineSequenceRestartGeneratorResult>(request.context, kOperation, diagnostic);
  }
  if (!ValueInRange(existing->definition, request.restart_value)) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticRangeInvalid, generator_uuid);
    return DiagnosticResult<EngineSequenceRestartGeneratorResult>(request.context, kOperation, diagnostic);
  }
  auto fields = DefinitionFields(existing->definition);
  fields.push_back({"restart_with_value", "1"});
  fields.push_back({"restart_value", std::to_string(request.restart_value)});
  fields.push_back({"cache_window_generation", std::to_string(existing->cache_window_generation + 1)});
  const auto appended = AppendEvent(request.context, "RESTART", request.context.local_transaction_id, std::move(fields));
  if (appended.error) {
    return DiagnosticResult<EngineSequenceRestartGeneratorResult>(request.context, kOperation, appended);
  }
  const auto after = LoadSequenceGeneratorLifecycleState(request.context);
  const auto* generator = FindGenerator(after.state, generator_uuid);
  auto result = SuccessResult<EngineSequenceRestartGeneratorResult>(request.context, kOperation);
  if (generator != nullptr) {
    result.generator = *generator;
    result.metadata_cache_epoch = generator->metadata_epoch;
    FillGeneratorResult(&result, *generator);
  }
  return result;
}

EngineSequenceDropGeneratorResult EngineSequenceDropGenerator(
    const EngineSequenceDropGeneratorRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.generator.drop";
  EngineApiDiagnostic diagnostic;
  if (!ValidateMutatingContext(request.context, &diagnostic)) {
    return DiagnosticResult<EngineSequenceDropGeneratorResult>(request.context, kOperation, diagnostic);
  }
  const EngineUuid generator_uuid = GeneratorUuidFromRequest(request, request.generator_uuid);
  const auto loaded = LoadSequenceGeneratorLifecycleState(request.context);
  const auto* existing = FindGenerator(loaded.state, generator_uuid);
  if (existing == nullptr) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticNotFound, generator_uuid);
    return DiagnosticResult<EngineSequenceDropGeneratorResult>(request.context, kOperation, diagnostic);
  }
  const auto appended = AppendEvent(request.context,
                                    "DROP",
                                    request.context.local_transaction_id,
                                    {{"generator_uuid", generator_uuid},
                                     {"transaction_uuid", request.context.transaction_uuid}});
  if (appended.error) {
    return DiagnosticResult<EngineSequenceDropGeneratorResult>(request.context, kOperation, appended);
  }
  const auto after = LoadSequenceGeneratorLifecycleState(request.context);
  const auto* generator = FindGenerator(after.state, generator_uuid);
  auto result = SuccessResult<EngineSequenceDropGeneratorResult>(request.context, kOperation);
  if (generator != nullptr) {
    result.generator = *generator;
    result.metadata_cache_epoch = generator->metadata_epoch;
    FillGeneratorResult(&result, *generator);
  }
  return result;
}

EngineSequenceAllocateValueResult EngineSequenceAllocateValue(
    const EngineSequenceAllocateValueRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.generator.allocate";
  EngineApiDiagnostic diagnostic;
  if (!ValidateMutatingContext(request.context, &diagnostic)) {
    return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, diagnostic);
  }
  const EngineUuid generator_uuid = GeneratorUuidFromRequest(request, request.generator_uuid);
  const auto loaded = LoadSequenceGeneratorLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, loaded.diagnostic);
  }
  const auto* existing = FindGenerator(loaded.state, generator_uuid);
  if (existing == nullptr) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticNotFound, generator_uuid);
    return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, diagnostic);
  }
  if (existing->dropped) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticDropped, generator_uuid);
    return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, diagnostic);
  }
  if (existing->definition.requires_cluster_authority && !request.context.cluster_authority_available) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticClusterPathAbsent, generator_uuid);
    AppendDiagnosticEvent(request.context, diagnostic.code, generator_uuid, "standalone_cluster_generator_path");
    return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, diagnostic);
  }
  if (ReferenceMappingIncomplete(existing->definition)) {
    diagnostic = SequenceDiagnostic(kSequenceDiagnosticReferenceMappingIncomplete, generator_uuid);
    AppendDiagnosticEvent(request.context, diagnostic.code, generator_uuid, "reference_mapping_required");
    return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, diagnostic);
  }

  EngineSequenceGeneratorRecord generator = *existing;
  if (!generator.reusable_released_values.empty()) {
    const std::int64_t value = generator.reusable_released_values.front();

    const EngineUuid allocation_uuid = IssueSequenceIdentity();
    const EngineUuid reservation_uuid = IssueSequenceIdentity();
    if (allocation_uuid.is_nil() || reservation_uuid.is_nil()) return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, SequenceDiagnostic(kSequenceDiagnosticUuidInvalid, "identity_issuance_failed"));
    const auto reuse = AppendEvent(
        request.context,
        "REUSE_VALUE",
        request.context.local_transaction_id,
        {{"generator_uuid", generator_uuid},
         {"reused_value", std::to_string(value)},
         {"transaction_uuid", request.context.transaction_uuid}});
    if (reuse.error) {
      return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, reuse);
    }
    const auto appended = AppendEvent(
        request.context,
        "ALLOCATE",
        request.context.local_transaction_id,
        {{"allocation_uuid", allocation_uuid},
         {"reservation_uuid", reservation_uuid},
         {"generator_uuid", generator_uuid},
         {"table_uuid", generator.definition.table_uuid},
         {"column_uuid", generator.definition.column_uuid},
         {"statement_uuid", request.statement_uuid},
         {"record_uuid", request.record_uuid},
         {"transaction_uuid", request.context.transaction_uuid},
         {"local_transaction_id", std::to_string(request.context.local_transaction_id)},
         {"allocated_value", std::to_string(value)},
         {"allocation_mode", generator.definition.allocation_mode},
         {"allocation_finality", "allocated_uncommitted"},
         {"lifecycle_state", "allocated"},
         {"consumed_on_rollback", BoolText(generator.definition.consumed_on_rollback)},
         {"reusable_if_no_effect", BoolText(generator.definition.reusable_if_no_effect)},
         {"external_exposure_allowed", BoolText(request.external_exposure_allowed)},
         {"row_effect_expected", BoolText(request.row_effect_expected)},
         {"unique_validation_required", "1"},
         {"reference_mapping_label", generator.definition.reference_mapping_label},
         {"sequence_epoch", std::to_string(generator.metadata_epoch)},
         {"cache_window_generation", std::to_string(generator.cache_window_generation)},
         {"cache_window_first_value", std::to_string(generator.cache_window_first_value)},
         {"cache_window_last_value", std::to_string(generator.cache_window_last_value)},
         {"cache_next_after", std::to_string(generator.cache_next_value)},
         {"cache_window_active_after", BoolText(generator.cache_window_active)},
         {"durable_high_water_after", std::to_string(generator.durable_next_value)},
         {"durable_exhausted_after", BoolText(generator.durable_exhausted)},
         {"reused_released_value", "1"}});
    if (appended.error) {
      return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, appended);
    }
    const auto after = LoadSequenceGeneratorLifecycleState(request.context);
    auto result = SuccessResult<EngineSequenceAllocateValueResult>(request.context, kOperation);
    if (!after.state.allocations.empty()) {
      result.allocation = after.state.allocations.back();
      result.allocated_value = result.allocation.allocated_value;
      FillAllocationResult(&result, result.allocation);
    }
    const auto* after_generator = FindGenerator(after.state, generator_uuid);
    if (after_generator != nullptr) {
      result.generator = *after_generator;
      FillGeneratorResult(&result, *after_generator);
    }
    AddEvidence(&result, "transactional_released_value_reused", std::to_string(value));
    return result;
  }

  if (!generator.cache_window_active || !CacheValueWithinWindow(generator, generator.cache_next_value)) {
    const auto plan = PlanWindow(generator);
    if (!plan.ok) {
      AppendDiagnosticEvent(request.context, plan.diagnostic.code, generator_uuid, "range_exhausted");
      return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, plan.diagnostic);
    }
    const auto cache_appended = AppendEvent(
        request.context,
        "CACHE_WINDOW",
        request.context.local_transaction_id,
        {{"generator_uuid", generator_uuid},
         {"cache_window_generation", std::to_string(generator.cache_window_generation + 1)},
         {"cache_window_first_value", std::to_string(plan.first_value)},
         {"cache_window_last_value", std::to_string(plan.last_value)},
         {"cache_next_value", std::to_string(plan.first_value)},
         {"cache_window_active", "1"},
         {"durable_high_water_after", std::to_string(plan.durable_next_value)},
         {"durable_exhausted", BoolText(plan.durable_exhausted)},
         {"transaction_uuid", request.context.transaction_uuid}});
    if (cache_appended.error) {
      return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, cache_appended);
    }
    const auto refreshed = LoadSequenceGeneratorLifecycleState(request.context);
    const auto* refreshed_generator = FindGenerator(refreshed.state, generator_uuid);
    if (refreshed_generator != nullptr) { generator = *refreshed_generator; }
  }

  const std::int64_t value = generator.cache_next_value;
  bool exhausted_after_step = false;
  const auto next_after_value = NextValueForDefinition(generator.definition, value, &exhausted_after_step);
  const bool active_after = !CacheWindowConsumedAfter(generator, value) &&
                            next_after_value.has_value() &&
                            CacheValueWithinWindow(generator, *next_after_value);

  const EngineUuid allocation_uuid = IssueSequenceIdentity();
  const EngineUuid reservation_uuid = IssueSequenceIdentity();
    if (allocation_uuid.is_nil() || reservation_uuid.is_nil()) return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, SequenceDiagnostic(kSequenceDiagnosticUuidInvalid, "identity_issuance_failed"));
  const auto appended = AppendEvent(
      request.context,
      "ALLOCATE",
      request.context.local_transaction_id,
      {{"allocation_uuid", allocation_uuid},
       {"reservation_uuid", reservation_uuid},
       {"generator_uuid", generator_uuid},
       {"table_uuid", generator.definition.table_uuid},
       {"column_uuid", generator.definition.column_uuid},
       {"statement_uuid", request.statement_uuid},
       {"record_uuid", request.record_uuid},
       {"transaction_uuid", request.context.transaction_uuid},
       {"local_transaction_id", std::to_string(request.context.local_transaction_id)},
       {"allocated_value", std::to_string(value)},
       {"allocation_mode", generator.definition.allocation_mode},
       {"allocation_finality", "allocated_uncommitted"},
       {"lifecycle_state", "allocated"},
       {"consumed_on_rollback", BoolText(generator.definition.consumed_on_rollback)},
       {"reusable_if_no_effect", BoolText(generator.definition.reusable_if_no_effect)},
       {"external_exposure_allowed", BoolText(request.external_exposure_allowed)},
       {"row_effect_expected", BoolText(request.row_effect_expected)},
       {"unique_validation_required", "1"},
       {"reference_mapping_label", generator.definition.reference_mapping_label},
       {"sequence_epoch", std::to_string(generator.metadata_epoch)},
       {"cache_window_generation", std::to_string(generator.cache_window_generation)},
       {"cache_window_first_value", std::to_string(generator.cache_window_first_value)},
       {"cache_window_last_value", std::to_string(generator.cache_window_last_value)},
       {"cache_next_after", std::to_string(next_after_value.value_or(value))},
       {"cache_window_active_after", BoolText(active_after)},
       {"durable_high_water_after", std::to_string(generator.durable_next_value)},
       {"durable_exhausted_after", BoolText(generator.durable_exhausted)}});
  if (appended.error) {
    return DiagnosticResult<EngineSequenceAllocateValueResult>(request.context, kOperation, appended);
  }
  const auto after = LoadSequenceGeneratorLifecycleState(request.context);
  auto result = SuccessResult<EngineSequenceAllocateValueResult>(request.context, kOperation);
  if (!after.state.allocations.empty()) {
    result.allocation = after.state.allocations.back();
    result.allocated_value = result.allocation.allocated_value;
    FillAllocationResult(&result, result.allocation);
  }
  const auto* after_generator = FindGenerator(after.state, generator_uuid);
  if (after_generator != nullptr) {
    result.generator = *after_generator;
    FillGeneratorResult(&result, *after_generator);
  }
  return result;
}

EngineSequenceApplyMgaTransactionOutcomeResult EngineSequenceApplyMgaTransactionOutcome(
    const EngineSequenceApplyMgaTransactionOutcomeRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.generator.apply_mga_outcome";
  if (request.context.database_path.empty()) {
    return DiagnosticResult<EngineSequenceApplyMgaTransactionOutcomeResult>(
        request.context,
        kOperation,
        SequenceDiagnostic(kSequenceDiagnosticDatabasePathRequired, "database_path"));
  }
  if (request.outcome_local_transaction_id == 0 ||
      (request.mga_outcome != "committed" && request.mga_outcome != "rolled_back" &&
       request.mga_outcome != "archived")) {
    return DiagnosticResult<EngineSequenceApplyMgaTransactionOutcomeResult>(
        request.context,
        kOperation,
        SequenceDiagnostic(kSequenceDiagnosticMgaTransactionRequired, "mga_outcome"));
  }
  std::map<std::uint64_t, TransactionOutcome> outcomes;
  const bool inventory_ok = BuildOutcomeMap(request.context, {}, &outcomes);
  const auto outcome = outcomes.find(request.outcome_local_transaction_id);
  if (!inventory_ok || outcome == outcomes.end() ||
      outcome->second.transaction_uuid != request.outcome_transaction_uuid ||
      (request.mga_outcome != outcome->second.outcome &&
       !(request.mga_outcome == "archived" && outcome->second.outcome == "committed")))
    return DiagnosticResult<EngineSequenceApplyMgaTransactionOutcomeResult>(request.context, kOperation,
        SequenceDiagnostic(kSequenceDiagnosticMgaTransactionRequired, "durable_outcome_mismatch"));
  const auto appended = AppendEvent(
      request.context,
      "MGA_OUTCOME",
      request.context.local_transaction_id,
      {{"outcome_local_transaction_id", std::to_string(request.outcome_local_transaction_id)},
       {"outcome_transaction_uuid", request.outcome_transaction_uuid},
       {"mga_outcome", request.mga_outcome},
       {"committed_row_effects", BoolText(request.committed_row_effects)},
       {"folded_to_no_effect", BoolText(request.folded_to_no_effect)},
       {"external_exposure_observed", BoolText(request.external_exposure_observed)}});
  if (appended.error) {
    return DiagnosticResult<EngineSequenceApplyMgaTransactionOutcomeResult>(request.context, kOperation, appended);
  }
  std::vector<RawSequenceEvent> existing_events;
  const auto existing = ReadRawEvents(request.context, &existing_events);
  if (!existing.ok) return DiagnosticResult<EngineSequenceApplyMgaTransactionOutcomeResult>(request.context, kOperation, existing.diagnostic);
  const auto after = LoadSequenceGeneratorLifecycleState(request.context);
  if (!after.ok) return DiagnosticResult<EngineSequenceApplyMgaTransactionOutcomeResult>(request.context, kOperation, after.diagnostic);
  for (const auto& allocation : after.state.allocations) {
    if (allocation.local_transaction_id != request.outcome_local_transaction_id) { continue; }
    if (!allocation.released) { continue; }
    if (std::any_of(existing_events.begin(), existing_events.end(), [&](const auto& event) {
          return event.event_kind == "RELEASE_VALUE" && BinaryCatalogUuid(event.fields, "allocation_uuid") == allocation.allocation_uuid;
        })) continue;
    const auto release = AppendEvent(
        request.context,
        "RELEASE_VALUE",
        request.context.local_transaction_id,
        {{"generator_uuid", allocation.generator_uuid},
         {"allocation_uuid", allocation.allocation_uuid},
         {"released_value", std::to_string(allocation.allocated_value)},
         {"release_reason", request.mga_outcome},
         {"transaction_uuid", allocation.transaction_uuid}});
    if (release.error) {
      return DiagnosticResult<EngineSequenceApplyMgaTransactionOutcomeResult>(request.context, kOperation, release);
    }
  }
  const auto reloaded = LoadSequenceGeneratorLifecycleState(request.context);
  if (!reloaded.ok) return DiagnosticResult<EngineSequenceApplyMgaTransactionOutcomeResult>(request.context, kOperation, reloaded.diagnostic);
  auto result = SuccessResult<EngineSequenceApplyMgaTransactionOutcomeResult>(request.context, kOperation);
  for (const auto& allocation : reloaded.state.allocations) {
    if (allocation.local_transaction_id == request.outcome_local_transaction_id) {
      result.updated_allocations.push_back(allocation);
    }
  }
  AddEvidence(&result, "mga_transaction_inventory_outcome_observed", request.mga_outcome);
  AddEvidence(&result, "sequence_allocation_not_finality_authority", "true");
  return result;
}

EngineSequenceBindIdentityValueResult EngineSequenceBindIdentityValue(
    const EngineSequenceBindIdentityValueRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.identity.bind";
  EngineApiDiagnostic diagnostic;
  if (!ValidateMutatingContext(request.context, &diagnostic)) {
    return DiagnosticResult<EngineSequenceBindIdentityValueResult>(request.context, kOperation, diagnostic);
  }
  if (request.table_uuid.is_nil() || request.record_uuid.is_nil() || request.identity_column_uuid.is_nil()) {
    return DiagnosticResult<EngineSequenceBindIdentityValueResult>(
        request.context,
        kOperation,
        SequenceDiagnostic(kSequenceDiagnosticRangeInvalid, "identity_binding_metadata"));
  }
  EngineEvidenceValue identity_value = request.identity_value;
  const auto* identity_text = std::get_if<std::string>(&identity_value);
  const bool identity_empty = identity_text && identity_text->empty();
  if (request.identity_value_kind == "row_uuid_identity") {
    if (request.attempted_second_uuid_for_row_identity ||
        (!identity_empty && identity_value != EngineEvidenceValue{request.record_uuid})) {
      diagnostic = SequenceDiagnostic(kSequenceDiagnosticIdentityDoubleUuidForbidden, request.record_uuid);
      AppendDiagnosticEvent(request.context, diagnostic.code, request.generator_uuid, diagnostic.detail);
      return DiagnosticResult<EngineSequenceBindIdentityValueResult>(request.context, kOperation, diagnostic);
    }
    identity_value = request.record_uuid;
  } else if (identity_empty) {
    identity_value = request.allocation_uuid;
  }

  const EngineUuid binding_uuid = IssueSequenceIdentity();
  if (binding_uuid.is_nil()) return DiagnosticResult<EngineSequenceBindIdentityValueResult>(request.context, kOperation, SequenceDiagnostic(kSequenceDiagnosticUuidInvalid, "identity_issuance_failed"));
  const auto appended = AppendEvent(
      request.context,
      "IDENTITY_BIND",
      request.context.local_transaction_id,
      {{"identity_binding_uuid", binding_uuid},
       {"generator_uuid", request.generator_uuid},
       {"allocation_uuid", request.allocation_uuid},
       {"table_uuid", request.table_uuid},
       {"record_uuid", request.record_uuid},
       {"identity_column_uuid", request.identity_column_uuid},
       {"identity_value_kind", request.identity_value_kind},
       {"identity_value", identity_value},
       {"binding_finality", "allocated_uncommitted"},
       {"transaction_uuid", request.context.transaction_uuid},
       {"local_transaction_id", std::to_string(request.context.local_transaction_id)}});
  if (appended.error) {
    return DiagnosticResult<EngineSequenceBindIdentityValueResult>(request.context, kOperation, appended);
  }
  const auto after = LoadSequenceGeneratorLifecycleState(request.context);
  if (!after.ok) return DiagnosticResult<EngineSequenceBindIdentityValueResult>(request.context, kOperation, after.diagnostic);
  auto result = SuccessResult<EngineSequenceBindIdentityValueResult>(request.context, kOperation);
  if (!after.state.identity_bindings.empty()) { result.binding = after.state.identity_bindings.back(); }
  if (request.identity_value_kind == "row_uuid_identity") {
    result.diagnostics.push_back(SequenceDiagnostic(kSequenceDiagnosticIdentityAssigned, request.record_uuid, false));
    AddEvidence(&result, "row_uuid_identity", request.record_uuid);
  }
  AddEvidence(&result, "identity_binding", binding_uuid);
  AddRow(&result,
         {{"identity_binding_uuid", binding_uuid},
          {"table_uuid", request.table_uuid},
          {"record_uuid", request.record_uuid},
          {"identity_column_uuid", request.identity_column_uuid},
          {"identity_value_kind", request.identity_value_kind},
          {"identity_value", identity_value}});
  return result;
}

EngineSequenceRecoverGeneratorStateResult EngineSequenceRecoverGeneratorState(
    const EngineSequenceRecoverGeneratorStateRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.generator.recover";
  const auto savepoint = AdmitMgaSavepointProducer(request.context, MgaMutationProducer::sequence_default);
  if (savepoint.error)
    return DiagnosticResult<EngineSequenceRecoverGeneratorStateResult>(request.context, kOperation, savepoint);
  if (request.context.database_path.empty()) {
    return DiagnosticResult<EngineSequenceRecoverGeneratorStateResult>(
        request.context,
        kOperation,
        SequenceDiagnostic(kSequenceDiagnosticDatabasePathRequired, "database_path"));
  }
  const auto loaded = LoadSequenceGeneratorLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineSequenceRecoverGeneratorStateResult>(request.context, kOperation, loaded.diagnostic);
  }
  const EngineUuid snapshot_uuid =
      IssueSequenceIdentity();
  if (snapshot_uuid.is_nil()) return DiagnosticResult<EngineSequenceRecoverGeneratorStateResult>(request.context, kOperation, SequenceDiagnostic(kSequenceDiagnosticUuidInvalid, "identity_issuance_failed"));
  for (const auto& generator : loaded.state.generators) {
    if (generator.dropped || !generator.cache_window_active) { continue; }
    std::uint64_t unused = 0;
    std::int64_t cursor = generator.cache_next_value;
    while (CacheValueWithinWindow(generator, cursor)) {
      ++unused;
      if (cursor == generator.cache_window_last_value) { break; }
      bool exhausted = false;
      const auto next = NextValueForDefinition(generator.definition, cursor, &exhausted);
      if (!next.has_value() || exhausted) { break; }
      cursor = *next;
    }
    const auto appended = AppendEvent(
        request.context,
        "RECOVERY_SNAPSHOT",
        request.context.local_transaction_id,
        {{"recovery_snapshot_uuid", snapshot_uuid},
         {"generator_uuid", generator.definition.generator_uuid},
         {"cache_unused_on_recovery", std::to_string(unused)},
         {"durable_high_water_after", std::to_string(generator.durable_next_value)},
         {"cache_window_generation", std::to_string(generator.cache_window_generation)}});
    if (appended.error) {
      return DiagnosticResult<EngineSequenceRecoverGeneratorStateResult>(request.context, kOperation, appended);
    }
  }
  const auto after = LoadSequenceGeneratorLifecycleState(request.context);
  if (!after.ok) return DiagnosticResult<EngineSequenceRecoverGeneratorStateResult>(request.context, kOperation, after.diagnostic);
  auto result = SuccessResult<EngineSequenceRecoverGeneratorStateResult>(request.context, kOperation);
  result.state = after.state;
  result.recovery_snapshot_uuid = snapshot_uuid;
  AddEvidence(&result, "crash_recovery_uses_persisted_generator_high_water", snapshot_uuid);
  AddEvidence(&result, "cache_window_gaps_allowed_by_policy", "bounded_nonfinality_cache_v1");
  return result;
}

EngineSequenceEvaluateMgaRetentionResult EngineSequenceEvaluateMgaRetention(
    const EngineSequenceEvaluateMgaRetentionRequest& request) {
  const auto inventory_guard = AcquireTransactionInventoryGuard(request.context.database_path);
  constexpr const char* kOperation = "sequence.generator.evaluate_mga_retention";
  const auto savepoint = AdmitMgaSavepointProducer(request.context, MgaMutationProducer::sequence_default);
  if (savepoint.error)
    return DiagnosticResult<EngineSequenceEvaluateMgaRetentionResult>(request.context, kOperation, savepoint);
  if (request.context.database_path.empty()) {
    return DiagnosticResult<EngineSequenceEvaluateMgaRetentionResult>(
        request.context,
        kOperation,
        SequenceDiagnostic(kSequenceDiagnosticDatabasePathRequired, "database_path"));
  }
  const auto loaded = LoadSequenceGeneratorLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineSequenceEvaluateMgaRetentionResult>(request.context, kOperation, loaded.diagnostic);
  }
  std::uint64_t blocked = 0;
  EngineUuid blocked_generator;
  for (const auto& generator : loaded.state.generators) {
    if (!generator.dropped) { continue; }
    for (const auto& allocation : loaded.state.allocations) {
      if (allocation.generator_uuid != generator.definition.generator_uuid) { continue; }
      if (!allocation.released &&
          allocation.local_transaction_id > request.retention_visible_through_local_transaction_id) {
        ++blocked;
        blocked_generator = generator.definition.generator_uuid;
      }
    }
  }
  if (blocked != 0) {
    AppendEvent(request.context,
                "RETENTION_BLOCKED",
                request.context.local_transaction_id,
                {{"generator_uuid", blocked_generator},
                 {"blocked_allocation_count", std::to_string(blocked)},
                 {"retention_visible_through_local_transaction_id",
                  std::to_string(request.retention_visible_through_local_transaction_id)}});
    auto result = DiagnosticResult<EngineSequenceEvaluateMgaRetentionResult>(
        request.context,
        kOperation,
        SequenceDiagnostic(kSequenceDiagnosticMgaRetentionBlocked, blocked_generator));
    result.blocked_allocation_count = blocked;
    return result;
  }
  auto result = SuccessResult<EngineSequenceEvaluateMgaRetentionResult>(request.context, kOperation);
  result.blocked_allocation_count = 0;
  AddEvidence(&result, "mga_retention_horizon_allows_sequence_cleanup",
              std::to_string(request.retention_visible_through_local_transaction_id));
  return result;
}

}  // namespace scratchbird::engine::internal_api
