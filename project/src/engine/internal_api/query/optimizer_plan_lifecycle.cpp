// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/optimizer_plan_lifecycle.hpp"

#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include "catalog/binary_catalog_metadata.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"
#include <filesystem>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using PlanField = std::variant<std::string, EngineUuid, std::vector<EngineUuid>>;
using PlanFields = std::map<std::string, PlanField>;
struct RawPlanEvent {
  std::uint64_t event_sequence = 0;
  std::uint32_t event_schema_version = 0;
  std::string event_kind;
  PlanFields fields;
  bool malformed = false;
  bool legacy = false;
};

std::string EventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.optimizer_plan_events";
}

std::optional<std::uint64_t> ParseU64(const std::string& value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') return std::nullopt;
    const std::uint64_t digit = ch - '0';
    if (parsed >
        (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + digit;
  }
  return std::to_string(parsed) == value ? std::optional(parsed) : std::nullopt;
}

std::optional<bool> ParseBool(const std::string& value) {
  if (value == "1") return true;
  if (value == "0") return false;
  return std::nullopt;
}

std::string BoolText(bool value) {
  return value ? "1" : "0";
}

const std::string* Field(const PlanFields& fields,
                         const std::string& key) {
  const auto it = fields.find(key);
  return it == fields.end() ? nullptr : std::get_if<std::string>(&it->second);
}

std::optional<std::uint64_t> FieldU64(
    const PlanFields& fields,
    const std::string& key) {
  const auto* value = Field(fields, key);
  return value == nullptr ? std::nullopt : ParseU64(*value);
}

std::optional<bool> FieldBool(
    const PlanFields& fields,
    const std::string& key) {
  const auto* value = Field(fields, key);
  return value == nullptr ? std::nullopt : ParseBool(*value);
}

bool ExactFields(const PlanFields& fields,
                 std::initializer_list<const char*> required) {
  if (fields.size() != required.size()) return false;
  return std::all_of(required.begin(), required.end(), [&](const char* key) {
    const auto it = fields.find(key);
    if (it == fields.end()) return false;
    const std::string_view name(key);
    if (name == "object_dependency_uuids") return std::holds_alternative<std::vector<EngineUuid>>(it->second);
    if (name.ends_with("_uuid")) return std::holds_alternative<EngineUuid>(it->second);
    return std::holds_alternative<std::string>(it->second);
  });
}

bool CanonicalUuid(const EngineUuid& value) {
  return core::uuid::IsEngineIdentityUuid(value);
}
const EngineUuid* FieldUuid(const PlanFields& fields, const std::string& key) {
  const auto it = fields.find(key);
  return it == fields.end() ? nullptr : std::get_if<EngineUuid>(&it->second);
}
std::optional<std::vector<EngineUuid>> CanonicalObjectDependencies(std::vector<EngineUuid> values) {
  if (values.empty() || !std::ranges::all_of(values, CanonicalUuid) ||
      !std::ranges::is_sorted(values) || std::adjacent_find(values.begin(), values.end()) != values.end()) return std::nullopt;
  return values;
}
std::optional<std::vector<EngineUuid>> FieldDependencies(const PlanFields& fields) {
  const auto it = fields.find("object_dependency_uuids");
  if (it == fields.end()) return std::nullopt;
  const auto* values = std::get_if<std::vector<EngineUuid>>(&it->second);
  return values ? CanonicalObjectDependencies(*values) : std::nullopt;
}

EngineOptimizerPlanDependencyIdentity DependencyIdentityFromDag(
    const plan_executor::TypedPhysicalNodeDag& dag,
    std::vector<EngineUuid> object_dependencies) {
  EngineOptimizerPlanDependencyIdentity identity;
  identity.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  identity.catalog_epoch_uuid = dag.catalog_epoch_uuid;
  identity.security_context_uuid = dag.security_context_uuid;
  identity.capability_snapshot_uuid = dag.capability_snapshot_uuid;
  identity.resource_snapshot_uuid = dag.resource_snapshot_uuid;
  identity.statistics_snapshot_uuid = dag.statistics_snapshot_uuid;
  identity.route_snapshot_uuid = dag.route_snapshot_uuid;
  identity.catalog_generation_id = dag.catalog_generation;
  identity.security_epoch = dag.security_epoch;
  identity.policy_epoch = dag.policy_epoch;
  identity.resource_epoch = dag.resource_epoch;
  identity.statistics_generation = dag.statistics_generation;
  identity.route_epoch = dag.route_epoch;
  identity.route_generation = dag.route_generation;
  identity.object_dependency_uuids = std::move(object_dependencies);
  return identity;
}

bool DependencyIdentityEqual(
    const EngineOptimizerPlanDependencyIdentity& left,
    const EngineOptimizerPlanDependencyIdentity& right) {
  return left.bound_sblr_tree_uuid == right.bound_sblr_tree_uuid &&
         left.catalog_epoch_uuid == right.catalog_epoch_uuid &&
         left.security_context_uuid == right.security_context_uuid &&
         left.capability_snapshot_uuid == right.capability_snapshot_uuid &&
         left.resource_snapshot_uuid == right.resource_snapshot_uuid &&
         left.statistics_snapshot_uuid == right.statistics_snapshot_uuid &&
         left.route_snapshot_uuid == right.route_snapshot_uuid &&
         left.catalog_generation_id == right.catalog_generation_id &&
         left.security_epoch == right.security_epoch &&
         left.policy_epoch == right.policy_epoch &&
         left.resource_epoch == right.resource_epoch &&
         left.statistics_generation == right.statistics_generation &&
         left.route_epoch == right.route_epoch &&
         left.route_generation == right.route_generation &&
         left.object_dependency_uuids == right.object_dependency_uuids;
}

bool DependencyIdentityComplete(
    const EngineOptimizerPlanDependencyIdentity& identity) {
  for (const auto* uuid :
       {&identity.bound_sblr_tree_uuid, &identity.catalog_epoch_uuid,
        &identity.security_context_uuid, &identity.capability_snapshot_uuid,
        &identity.resource_snapshot_uuid, &identity.statistics_snapshot_uuid,
        &identity.route_snapshot_uuid}) {
    if (!CanonicalUuid(*uuid)) return false;
  }
  return identity.catalog_generation_id != 0 && identity.security_epoch != 0 &&
         identity.policy_epoch != 0 && identity.resource_epoch != 0 &&
         identity.statistics_generation != 0 && identity.route_epoch != 0 &&
         identity.route_generation != 0 &&
         CanonicalObjectDependencies(identity.object_dependency_uuids)
             .has_value();
}

bool CatalogIdentityIndependent(
    const EngineUuid& catalog_epoch_uuid,
    const plan_executor::PhysicalMgaStatementContext& context) {
  return CanonicalUuid(catalog_epoch_uuid) &&
         catalog_epoch_uuid != context.statement_uuid &&
         catalog_epoch_uuid != context.owning_transaction_uuid &&
         catalog_epoch_uuid != context.statement_snapshot_uuid &&
         catalog_epoch_uuid != context.statement_metadata_snapshot_uuid;
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
  } else if (diagnostic_code == kOptimizerPlanDiagnosticMgaAuthorityRequired) {
    message_key = "optimizer.plan.mga_authority_required";
  } else if (diagnostic_code ==
             kOptimizerPlanDiagnosticCatalogIdentityRequired) {
    message_key = "optimizer.plan.catalog_identity_required";
  } else if (diagnostic_code == kOptimizerPlanDiagnosticDependencyMismatch) {
    message_key = "optimizer.plan.dependency_mismatch";
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

std::optional<EngineApiDiagnostic> ValidateStatementBinding(
    const EngineRequestContext& context,
    const plan_executor::CanonicalExecutionMgaAuthority& authority,
    const plan_executor::TypedPhysicalNodeDag& dag,
    const EngineUuid& selected_catalog_epoch_uuid) {
  if (authority.origin !=
          plan_executor::CanonicalMgaAuthorityOrigin::
              kEngineTransactionInventory ||
      dag.abi_version != 2) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticMgaAuthorityRequired,
                          "ABI-v2 engine transaction inventory authority is required");
  }
  const auto revalidated =
      plan_executor::RevalidateCanonicalExecutionMgaAuthority(authority, dag);
  if (!revalidated.ok) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticMgaAuthorityRequired,
                          revalidated.diagnostic_code + ":" +
                              revalidated.detail);
  }
  const auto& statement = authority.statement_context;
  if (selected_catalog_epoch_uuid != dag.catalog_epoch_uuid ||
      !CatalogIdentityIndependent(selected_catalog_epoch_uuid, statement)) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticCatalogIdentityRequired,
                          "independent selected catalog epoch UUID is required");
  }
  if (context.transaction_uuid !=
          statement.owning_transaction_uuid ||
      context.local_transaction_id != statement.owning_local_transaction_id ||
      context.statement_uuid != statement.statement_uuid ||
      context.statement_snapshot_uuid !=
          statement.statement_snapshot_uuid ||
      context.statement_metadata_snapshot_uuid !=
          statement.statement_metadata_snapshot_uuid ||
      context.catalog_epoch_uuid != selected_catalog_epoch_uuid ||
      context.snapshot_visible_through_local_transaction_id !=
          statement.visible_committed_high_watermark ||
      !context.statement_metadata_snapshot_engine_owned ||
      context
              .statement_metadata_snapshot_visible_through_local_transaction_id !=
          statement.visible_committed_high_watermark ||
      context.statement_metadata_snapshot_active_excluded_local_transaction_ids !=
          statement.active_excluded_local_transaction_ids ||
      context
              .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids !=
          statement.in_doubt_excluded_local_transaction_ids) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticMgaAuthorityRequired,
                          "engine request context differs from the exact statement carrier");
  }
  if (!context.security_context_present ||
      context.catalog_generation_id != dag.catalog_generation ||
      context.security_epoch != dag.security_epoch ||
      context.resource_epoch != dag.resource_epoch ||
      context.optimizer_capability_snapshot_uuid !=
          dag.capability_snapshot_uuid ||
      context.optimizer_resource_snapshot_uuid !=
          dag.resource_snapshot_uuid ||
      context.optimizer_route_snapshot_uuid !=
          dag.route_snapshot_uuid ||
      context.optimizer_route_epoch != dag.route_epoch ||
      context.optimizer_route_generation != dag.route_generation) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticDependencyMismatch,
                          "engine request dependency snapshots differ from the selected DAG");
  }
  return std::nullopt;
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

void AddRow(EngineApiResult* result, ApiBehaviorFields fields) {
  AddApiBehaviorRow(result, std::move(fields));
  result->result_shape.result_kind = "optimizer_plan_lifecycle_rows";
}
void AddEvidence(EngineApiResult* result, std::string kind, EngineEvidenceValue id) {
  AddApiBehaviorEvidence(result, std::move(kind), std::move(id));
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

// Every record is framed. UUID fields use the identity section; dependency
// arrays retain native elements. No parser or persisted metadata grants finality.
std::string MakeEvent(const EngineRequestContext& context, std::string event_kind,
                      const PlanFields& fields) {
  if (!CanonicalUuid(context.database_uuid)) return {};
  BinaryCatalogMetadata metadata;
  metadata.identities.emplace("database_uuid", context.database_uuid);
  metadata.text.emplace("event_kind", std::move(event_kind));
  for (const auto& [key, value] : fields) {
    if (key == "database_uuid" || key == "event_kind" || key.starts_with("dependency_element.")) return {};
    if (const auto* text = std::get_if<std::string>(&value)) metadata.text.emplace(key, *text);
    else if (const auto* uuid = std::get_if<EngineUuid>(&value)) metadata.identities.emplace(key, *uuid);
    else {
      if (key != "object_dependency_uuids") return {};
      const auto& values = std::get<std::vector<EngineUuid>>(value);
      metadata.text.emplace(key, std::to_string(values.size()));
      for (std::size_t n=0; n<values.size(); ++n) metadata.identities.emplace("dependency_element."+std::to_string(n), values[n]);
    }
  }
  std::string bytes;
  if (!EncodeBinaryCatalogMetadata(metadata, "optimizer.plan.event.v3", &bytes)) return {};
  return bytes;
}

bool DecodeEvent(std::string_view bytes, const EngineRequestContext& context, RawPlanEvent* event) {
  BinaryCatalogMetadata metadata;
  if (!DecodeBinaryCatalogMetadata(bytes, "optimizer.plan.event.v3", &metadata) ||
      !CanonicalUuid(context.database_uuid) || BinaryCatalogUuid(metadata, "database_uuid") != context.database_uuid ||
      !metadata.text.contains("event_kind")) return false;
  event->event_kind = metadata.text.at("event_kind");
  metadata.text.erase("event_kind"); metadata.identities.erase("database_uuid");
  if (metadata.text.contains("object_dependency_uuids")) {
    const auto count = ParseU64(metadata.text.at("object_dependency_uuids"));
    if (!count || *count > metadata.identities.size()) return false;
    std::vector<EngineUuid> values;
    for (std::uint64_t n=0; n<*count; ++n) {
      const auto key = "dependency_element."+std::to_string(n);
      const auto it = metadata.identities.find(key);
      if (it == metadata.identities.end()) return false;
      values.push_back(it->second); metadata.identities.erase(it);
    }
    event->fields.emplace("object_dependency_uuids", std::move(values));
    metadata.text.erase("object_dependency_uuids");
  }
  for (const auto& [key, value] : metadata.text) if (!event->fields.emplace(key, value).second) return false;
  for (const auto& [key, value] : metadata.identities) if (!event->fields.emplace(key, value).second) return false;
  if (MakeEvent(context, event->event_kind, event->fields) != bytes) return false;
  event->event_schema_version = kOptimizerPlanLifecycleEventSchemaVersion;
  return true;
}

EngineLoadOptimizerPlanLifecycleStateResult ReadRawEvents(
    const EngineRequestContext& context, std::vector<RawPlanEvent>* events) {
  EngineLoadOptimizerPlanLifecycleStateResult result;
  const auto refuse = [&](const char* detail) {
    result.diagnostic = PlanDiagnostic(kOptimizerPlanDiagnosticCacheInvalidated, detail);
    return result;
  };
  if (context.database_path.empty()) return refuse("database_path");
  std::error_code ec;
  const bool exists = std::filesystem::exists(EventPath(context), ec);
  if (ec) return refuse("event_file_stat_failed");
  if (!exists) { result.ok = true; result.diagnostic = OkDiagnostic(); return result; }
  std::ifstream in(EventPath(context), std::ios::binary);
  if (!in) return refuse("event_file_open_failed");
  const std::string bytes((std::istreambuf_iterator<char>(in)), {});
  if (in.bad()) return refuse("event_file_read_failed");
  const std::string_view magic(kOptimizerPlanLifecycleEventMagic);
  if (!bytes.starts_with(magic)) {
    RawPlanEvent event; event.legacy = bytes.starts_with("SBPLANL1") || bytes.starts_with("SBPLANL2");
    event.malformed = !event.legacy; events->push_back(std::move(event));
    result.ok = true; result.diagnostic = OkDiagnostic(); return result;
  }
  std::span<const std::uint8_t> input(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  std::size_t cursor = magic.size();
  std::uint64_t sequence = 0;
  while (cursor < input.size()) {
    RawPlanEvent event; event.event_sequence = ++sequence;
    std::uint32_t size = 0;
    if (!ReadBinaryU32(input, &cursor, &size) || size > input.size()-cursor) {
      event.malformed = true; events->push_back(std::move(event)); break;
    }
    event.malformed = !DecodeEvent(std::string_view(bytes).substr(cursor,size), context, &event);
    cursor += size; events->push_back(std::move(event));
  }
  result.ok = true; result.diagnostic = OkDiagnostic(); return result;
}

EngineApiDiagnostic AppendEvent(const EngineRequestContext& context,
                                std::string event_kind, PlanFields fields) {
  const auto before = LoadOptimizerPlanLifecycleState(context);
  if (!before.ok) return before.diagnostic;
  const auto bytes = MakeEvent(context, std::move(event_kind), fields);
  if (bytes.empty() || bytes.size() > std::numeric_limits<std::uint32_t>::max())
    return PlanDiagnostic(kOptimizerPlanDiagnosticWriteFailed, "event_encoding_failed");
  std::error_code ec;
  const bool exists = std::filesystem::exists(EventPath(context), ec);
  if (ec) return PlanDiagnostic(kOptimizerPlanDiagnosticWriteFailed, "event_file_stat_failed");
  std::string frame;
  if (!exists) frame = kOptimizerPlanLifecycleEventMagic;
  AppendBinaryU32(&frame, static_cast<std::uint32_t>(bytes.size())); frame += bytes;
  std::ofstream out(EventPath(context), std::ios::binary | std::ios::app);
  if (!out) return PlanDiagnostic(kOptimizerPlanDiagnosticWriteFailed, "open");
  out.write(frame.data(), frame.size()); out.flush();
  if (!out) return PlanDiagnostic(kOptimizerPlanDiagnosticWriteFailed, "flush");
  return OkDiagnostic();
}

bool CachePlanEventSchemaValid(const RawPlanEvent& event) {
  if (event.malformed || event.legacy ||
      event.event_schema_version != kOptimizerPlanLifecycleEventSchemaVersion ||
      event.event_kind != "CACHE_PLAN" ||
      !ExactFields(
          event.fields,
          {"record_schema", "event_uuid", "metadata_only",
           "plan_cache_epoch", "plan_uuid", "query_fingerprint",
           "relation_uuid", "index_uuid", "catalog_physical_profile_key",
           "plan_shape_digest", "index_generation",
           "statistics_generation", "catalog_generation_id",
           "resource_epoch", "charset_epoch", "collation_epoch",
           "bound_sblr_tree_uuid", "catalog_epoch_uuid",
           "security_context_uuid", "capability_snapshot_uuid",
           "resource_snapshot_uuid", "statistics_snapshot_uuid",
           "route_snapshot_uuid", "dependency_catalog_generation_id",
           "dependency_security_epoch", "dependency_policy_epoch",
           "dependency_resource_epoch", "dependency_statistics_generation",
           "dependency_route_epoch", "dependency_route_generation",
           "object_dependency_uuids", "invalidated"})) {
    return false;
  }
  const auto metadata_only = FieldBool(event.fields, "metadata_only");
  const auto invalidated = FieldBool(event.fields, "invalidated");
  const auto plan_cache_epoch = FieldU64(event.fields, "plan_cache_epoch");
  const auto index_generation = FieldU64(event.fields, "index_generation");
  const auto statistics_generation =
      FieldU64(event.fields, "statistics_generation");
  const auto catalog_generation_id =
      FieldU64(event.fields, "catalog_generation_id");
  const auto resource_epoch = FieldU64(event.fields, "resource_epoch");
  const auto charset_epoch = FieldU64(event.fields, "charset_epoch");
  const auto collation_epoch = FieldU64(event.fields, "collation_epoch");
  const auto dependency_catalog =
      FieldU64(event.fields, "dependency_catalog_generation_id");
  const auto dependency_security =
      FieldU64(event.fields, "dependency_security_epoch");
  const auto dependency_policy =
      FieldU64(event.fields, "dependency_policy_epoch");
  const auto dependency_resource =
      FieldU64(event.fields, "dependency_resource_epoch");
  const auto dependency_statistics =
      FieldU64(event.fields, "dependency_statistics_generation");
  const auto dependency_route_epoch =
      FieldU64(event.fields, "dependency_route_epoch");
  const auto dependency_route_generation =
      FieldU64(event.fields, "dependency_route_generation");
  const auto dependencies =
      FieldDependencies(event.fields);
  const auto& relation_uuid = *FieldUuid(event.fields, "relation_uuid");
  const auto& index_uuid = *FieldUuid(event.fields, "index_uuid");
  if (*Field(event.fields, "record_schema") !=
          "optimizer_plan_metadata_v3" ||
      !metadata_only.value_or(false) || invalidated.value_or(true) ||
      !plan_cache_epoch || *plan_cache_epoch == 0 ||
      !index_generation || *index_generation == 0 ||
      !statistics_generation || *statistics_generation == 0 ||
      !catalog_generation_id || *catalog_generation_id == 0 ||
      !resource_epoch || *resource_epoch == 0 || !charset_epoch ||
      *charset_epoch == 0 || !collation_epoch || *collation_epoch == 0 ||
      !dependency_catalog || *dependency_catalog == 0 ||
      !dependency_security || *dependency_security == 0 ||
      !dependency_policy || *dependency_policy == 0 ||
      !dependency_resource || *dependency_resource == 0 ||
      !dependency_statistics || *dependency_statistics == 0 ||
      !dependency_route_epoch || *dependency_route_epoch == 0 ||
      !dependency_route_generation || *dependency_route_generation == 0 ||
      *catalog_generation_id != *dependency_catalog ||
      *resource_epoch != *dependency_resource ||
      *statistics_generation != *dependency_statistics || !dependencies ||
      !std::ranges::binary_search(*dependencies, relation_uuid) ||
      !std::ranges::binary_search(*dependencies, index_uuid)) {
    return false;
  }
  for (const auto* key :
       {"event_uuid", "plan_uuid", "relation_uuid", "index_uuid",
        "bound_sblr_tree_uuid", "catalog_epoch_uuid",
        "security_context_uuid", "capability_snapshot_uuid",
        "resource_snapshot_uuid", "statistics_snapshot_uuid",
        "route_snapshot_uuid"}) {
    if (!CanonicalUuid(*FieldUuid(event.fields, key))) return false;
  }
  return !Field(event.fields, "query_fingerprint")->empty() &&
         !Field(event.fields, "catalog_physical_profile_key")->empty() &&
         !Field(event.fields, "plan_shape_digest")->empty();
}

bool InvalidationEventSchemaValid(const RawPlanEvent& event) {
  if (event.malformed || event.legacy ||
      event.event_schema_version != kOptimizerPlanLifecycleEventSchemaVersion ||
      event.event_kind != "INVALIDATE" ||
      !ExactFields(event.fields,
                   {"record_schema", "event_uuid", "metadata_only",
                    "plan_cache_epoch", "index_uuid", "reason",
                    "new_index_generation", "new_statistics_generation",
                    "new_catalog_generation_id", "new_resource_epoch",
                    "new_charset_epoch", "new_collation_epoch",
                    "invalidate_all"})) {
    return false;
  }
  const auto metadata_only = FieldBool(event.fields, "metadata_only");
  const auto invalidate_all = FieldBool(event.fields, "invalidate_all");
  const auto plan_cache_epoch = FieldU64(event.fields, "plan_cache_epoch");
  if (*Field(event.fields, "record_schema") !=
          "optimizer_plan_invalidation_v3" ||
      !metadata_only.value_or(false) || !invalidate_all ||
      !plan_cache_epoch || *plan_cache_epoch == 0 ||
      !CanonicalUuid(*FieldUuid(event.fields, "event_uuid")) ||
      Field(event.fields, "reason")->empty()) {
    return false;
  }
  for (const auto* key :
       {"new_index_generation", "new_statistics_generation",
        "new_catalog_generation_id", "new_resource_epoch",
        "new_charset_epoch", "new_collation_epoch"}) {
    const auto value = FieldU64(event.fields, key);
    if (!value || *value == 0) return false;
  }
  const auto& index_uuid = *FieldUuid(event.fields, "index_uuid");
  return *invalidate_all ? (index_uuid.is_nil() || CanonicalUuid(index_uuid))
                         : CanonicalUuid(index_uuid);
}

bool RecoveryEventSchemaValid(const RawPlanEvent& event) {
  if (event.malformed || event.legacy ||
      event.event_schema_version != kOptimizerPlanLifecycleEventSchemaVersion ||
      event.event_kind != "RECOVERY_SNAPSHOT" ||
      !ExactFields(event.fields,
                   {"record_schema", "event_uuid", "metadata_only",
                    "plan_cache_epoch", "recovery_snapshot_uuid"})) {
    return false;
  }
  const auto metadata_only = FieldBool(event.fields, "metadata_only");
  const auto plan_cache_epoch = FieldU64(event.fields, "plan_cache_epoch");
  return *Field(event.fields, "record_schema") ==
             "optimizer_plan_recovery_v3" &&
         metadata_only.value_or(false) && plan_cache_epoch &&
         *plan_cache_epoch != 0 &&
         CanonicalUuid(*FieldUuid(event.fields, "event_uuid")) &&
         CanonicalUuid(*FieldUuid(event.fields, "recovery_snapshot_uuid"));
}

bool CorrectedEventSchemaValid(const RawPlanEvent& event) {
  if (event.event_kind == "CACHE_PLAN") return CachePlanEventSchemaValid(event);
  if (event.event_kind == "INVALIDATE") {
    return InvalidationEventSchemaValid(event);
  }
  if (event.event_kind == "RECOVERY_SNAPSHOT") {
    return RecoveryEventSchemaValid(event);
  }
  return false;
}

std::optional<EngineOptimizerPlanCacheEntry> EntryFromFields(
    const RawPlanEvent& event) {
  if (!CachePlanEventSchemaValid(event)) return std::nullopt;
  EngineOptimizerPlanCacheEntry entry;
  entry.event_schema_version = event.event_schema_version;
  entry.event_uuid = *FieldUuid(event.fields, "event_uuid");
  entry.event_sequence = event.event_sequence;
  entry.plan_cache_epoch = *FieldU64(event.fields, "plan_cache_epoch");
  entry.plan_uuid = *FieldUuid(event.fields, "plan_uuid");
  entry.query_fingerprint = *Field(event.fields, "query_fingerprint");
  entry.relation_uuid = *FieldUuid(event.fields, "relation_uuid");
  entry.index_uuid = *FieldUuid(event.fields, "index_uuid");
  entry.catalog_physical_profile_key =
      *Field(event.fields, "catalog_physical_profile_key");
  entry.plan_shape_digest = *Field(event.fields, "plan_shape_digest");
  entry.index_generation = *FieldU64(event.fields, "index_generation");
  entry.statistics_generation =
      *FieldU64(event.fields, "statistics_generation");
  entry.catalog_generation_id =
      *FieldU64(event.fields, "catalog_generation_id");
  entry.resource_epoch = *FieldU64(event.fields, "resource_epoch");
  entry.charset_epoch = *FieldU64(event.fields, "charset_epoch");
  entry.collation_epoch = *FieldU64(event.fields, "collation_epoch");
  entry.dependencies.bound_sblr_tree_uuid =
      *FieldUuid(event.fields, "bound_sblr_tree_uuid");
  entry.dependencies.catalog_epoch_uuid =
      *FieldUuid(event.fields, "catalog_epoch_uuid");
  entry.dependencies.security_context_uuid =
      *FieldUuid(event.fields, "security_context_uuid");
  entry.dependencies.capability_snapshot_uuid =
      *FieldUuid(event.fields, "capability_snapshot_uuid");
  entry.dependencies.resource_snapshot_uuid =
      *FieldUuid(event.fields, "resource_snapshot_uuid");
  entry.dependencies.statistics_snapshot_uuid =
      *FieldUuid(event.fields, "statistics_snapshot_uuid");
  entry.dependencies.route_snapshot_uuid =
      *FieldUuid(event.fields, "route_snapshot_uuid");
  entry.dependencies.catalog_generation_id =
      *FieldU64(event.fields, "dependency_catalog_generation_id");
  entry.dependencies.security_epoch =
      *FieldU64(event.fields, "dependency_security_epoch");
  entry.dependencies.policy_epoch =
      *FieldU64(event.fields, "dependency_policy_epoch");
  entry.dependencies.resource_epoch =
      *FieldU64(event.fields, "dependency_resource_epoch");
  entry.dependencies.statistics_generation =
      *FieldU64(event.fields, "dependency_statistics_generation");
  entry.dependencies.route_epoch =
      *FieldU64(event.fields, "dependency_route_epoch");
  entry.dependencies.route_generation =
      *FieldU64(event.fields, "dependency_route_generation");
  entry.dependencies.object_dependency_uuids =
      *FieldDependencies(event.fields);
  entry.metadata_only = true;
  entry.invalidated = false;
  return entry;
}

bool EntryMatches(const EngineOptimizerPlanCacheEntry& entry,
                  const EngineUuid& plan_uuid,
                  const std::string& query_fingerprint,
                  const EngineUuid& index_uuid) {
  if (!plan_uuid.is_nil() && entry.plan_uuid != plan_uuid) {
    return false;
  }
  if (!query_fingerprint.empty() && entry.query_fingerprint != query_fingerprint) {
    return false;
  }
  if (!index_uuid.is_nil() && entry.index_uuid != index_uuid) {
    return false;
  }
  return !plan_uuid.is_nil() || !query_fingerprint.empty() || !index_uuid.is_nil();
}

const EngineOptimizerPlanCacheEntry* FindEntry(const EngineOptimizerPlanLifecycleState& state,
                                               const EngineUuid& plan_uuid,
                                               const std::string& query_fingerprint,
                                               const EngineUuid& index_uuid) {
  for (auto it = state.entries.rbegin(); it != state.entries.rend(); ++it) {
    if (EntryMatches(*it, plan_uuid, query_fingerprint, index_uuid)) {
      return &*it;
    }
  }
  return nullptr;
}

const EngineOptimizerPlanCacheEntry* FindEntryByEventUuid(
    const EngineOptimizerPlanLifecycleState& state,
    const EngineUuid& event_uuid) {
  for (auto it = state.entries.rbegin(); it != state.entries.rend(); ++it) {
    if (it->event_uuid == event_uuid) return &*it;
  }
  return nullptr;
}

void ApplyInvalidation(EngineOptimizerPlanLifecycleState* state, const RawPlanEvent& event) {
  const bool invalidate_all = *FieldBool(event.fields, "invalidate_all");
  const EngineUuid& index_uuid = *FieldUuid(event.fields, "index_uuid");
  const std::string& reason = *Field(event.fields, "reason");
  const std::uint64_t plan_cache_epoch =
      *FieldU64(event.fields, "plan_cache_epoch");
  const std::uint64_t new_index_generation =
      *FieldU64(event.fields, "new_index_generation");
  const std::uint64_t new_statistics_generation =
      *FieldU64(event.fields, "new_statistics_generation");
  const std::uint64_t new_catalog_generation_id =
      *FieldU64(event.fields, "new_catalog_generation_id");
  const std::uint64_t new_resource_epoch =
      *FieldU64(event.fields, "new_resource_epoch");
  const std::uint64_t new_charset_epoch =
      *FieldU64(event.fields, "new_charset_epoch");
  const std::uint64_t new_collation_epoch =
      *FieldU64(event.fields, "new_collation_epoch");

  bool touched = false;
  for (auto& entry : state->entries) {
    if (!invalidate_all && entry.index_uuid != index_uuid) {
      continue;
    }
    if (entry.index_generation == new_index_generation &&
        entry.statistics_generation == new_statistics_generation &&
        entry.catalog_generation_id == new_catalog_generation_id &&
        entry.resource_epoch == new_resource_epoch &&
        entry.charset_epoch == new_charset_epoch &&
        entry.collation_epoch == new_collation_epoch) {
      continue;
    }
    entry.invalidated = true;
    entry.invalidation_reason = reason;
    entry.plan_cache_epoch = std::max(entry.plan_cache_epoch, plan_cache_epoch);
    touched = true;
  }
  if (touched || invalidate_all || !index_uuid.is_nil()) {
    ++state->invalidation_events;
  }
  state->plan_cache_epoch = std::max(state->plan_cache_epoch, plan_cache_epoch);
}

void ApplyRecoverySnapshot(EngineOptimizerPlanLifecycleState* state, const RawPlanEvent& event) {
  state->plan_cache_epoch =
      std::max(state->plan_cache_epoch,
               *FieldU64(event.fields, "plan_cache_epoch"));
  state->recovered_from_persisted_evidence = true;
  state->recovery_snapshot_uuid = *FieldUuid(event.fields, "recovery_snapshot_uuid");
  for (auto& entry : state->entries) {
    entry.recovered_from_persisted_evidence = true;
  }
}

void FillEntryResult(EngineApiResult* result, const EngineOptimizerPlanCacheEntry& entry) {
  result->primary_object.uuid = entry.plan_uuid;
  result->primary_object.object_kind = "optimizer_plan_cache_entry";
  AddEvidence(result, "optimizer_plan_cache_entry", entry.plan_uuid);
  AddEvidence(result, "optimizer_plan_metadata_only", "true");
  AddEvidence(result, "optimizer_plan_metadata_authority", "none");
  AddEvidence(result, "catalog_physical_index_profile", entry.catalog_physical_profile_key);
  AddRow(result,
         {{"plan_uuid", entry.plan_uuid},
          {"query_fingerprint", entry.query_fingerprint},
          {"index_uuid", entry.index_uuid},
          {"catalog_epoch_uuid", entry.dependencies.catalog_epoch_uuid},
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
  if (!CanonicalUuid(request.plan_uuid) ||
      !CanonicalUuid(request.relation_uuid) ||
      !CanonicalUuid(request.index_uuid) || request.query_fingerprint.empty() ||
      request.plan_shape_digest.empty() ||
      request.plan_uuid != request.selected_physical_dag.selected_plan_uuid) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest, "plan_identity");
  }
  const auto object_dependencies =
      CanonicalObjectDependencies(request.object_dependency_uuids);
  if (!object_dependencies ||
      !std::ranges::binary_search(*object_dependencies,
                                  request.relation_uuid) ||
      !std::ranges::binary_search(*object_dependencies, request.index_uuid)) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticDependencyMismatch,
                          "canonical relation and index dependencies are required");
  }
  const auto dependencies = DependencyIdentityFromDag(
      request.selected_physical_dag, *object_dependencies);
  if (!DependencyIdentityComplete(dependencies) ||
      dependencies.catalog_epoch_uuid !=
          request.selected_catalog_epoch_uuid) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticDependencyMismatch,
                          "selected DAG dependency identity is incomplete");
  }
  if (!request.index_descriptor.index_uuid.valid() ||
      !request.index_descriptor.table_uuid.valid() ||
      request.index_descriptor.index_uuid.value != request.index_uuid ||
      request.index_descriptor.table_uuid.value !=
          request.relation_uuid ||
      !request.statistics.index_uuid.valid() ||
      request.statistics.index_uuid.value !=
          request.index_uuid) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticDependencyMismatch,
                          "index and relation UUID dependencies differ");
  }
  if (request.statistics.statistics_generation == 0 ||
      request.statistics.index_generation == 0 ||
      request.statistics.catalog_generation_id == 0 ||
      request.statistics.refreshed_by_transaction_id == 0 ||
      !request.statistics.catalog_profile_coupled ||
      !request.statistics.current || request.statistics.stale ||
      request.index_descriptor.lifecycle_state !=
          index_lifecycle::IndexStatisticsLifecycleState::ready ||
      request.statistics.index_generation !=
          request.index_descriptor.index_generation ||
      request.statistics.catalog_generation_id !=
          request.index_descriptor.catalog_generation_id ||
      request.statistics.physical_profile_key !=
          request.index_descriptor.catalog_profile.physical_profile_key ||
      !index_lifecycle::IndexResourceEpochVectorEqual(
          request.statistics.resource_epochs,
          request.index_descriptor.resource_epochs) ||
      request.selected_physical_dag.catalog_generation !=
          request.statistics.catalog_generation_id ||
      request.selected_physical_dag.statistics_generation !=
          request.statistics.statistics_generation ||
      request.selected_physical_dag.resource_epoch !=
          request.statistics.resource_epochs.resource_epoch) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest, "statistics_generation");
  }
  if (!plan_executor::CanonicalMgaCreatorVisibleToStatement(
          request.mga_authority.statement_context,
          request.statistics.refreshed_by_transaction_id)) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticStatisticsStale,
                          "statistics provenance is not visible to the exact statement carrier");
  }
  return OkDiagnostic();
}

std::uint64_t NextPlanCacheEpoch(
    const EngineOptimizerPlanLifecycleState& state,
    const EngineOptimizerCachePlanRequest& request) {
  return std::max(
      {state.plan_cache_epoch + 1, request.statistics.index_generation,
       request.statistics.statistics_generation,
       request.statistics.catalog_generation_id,
       request.statistics.resource_epochs.resource_epoch,
       request.statistics.resource_epochs.charset_epoch,
       request.statistics.resource_epochs.collation_epoch,
       request.selected_physical_dag.security_epoch,
       request.selected_physical_dag.policy_epoch,
       request.selected_physical_dag.route_epoch,
       request.selected_physical_dag.route_generation});
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

  bool rejected = false;
  for (const auto& event : events) {
    result.state.max_event_sequence =
        std::max(result.state.max_event_sequence, event.event_sequence);
    if (event.legacy) {
      ++result.state.legacy_event_count;
      ++result.state.rejected_event_count;
      rejected = true;
      continue;
    }
    if (event.malformed || !CorrectedEventSchemaValid(event)) {
      ++result.state.malformed_event_count;
      ++result.state.rejected_event_count;
      rejected = true;
    }
  }
  if (rejected) {
    result.state.entries.clear();
    result.state.plan_cache_epoch = 0;
    result.state.invalidation_events = 0;
    result.state.recovered_from_persisted_evidence = false;
    result.state.recovery_snapshot_uuid = {};
    result.ok = false;
    result.diagnostic = PlanDiagnostic(
        kOptimizerPlanDiagnosticCacheInvalidated,
        "legacy, malformed, truncated, unknown, or incomplete optimizer plan metadata event");
    return result;
  }

  for (const auto& event : events) {
    if (event.event_kind == "CACHE_PLAN") {
      auto parsed = EntryFromFields(event);
      if (!parsed) {
        result.state = {};
        result.ok = false;
        result.diagnostic = PlanDiagnostic(
            kOptimizerPlanDiagnosticCacheInvalidated,
            "optimizer plan metadata event failed strict replay");
        return result;
      }
      result.state.plan_cache_epoch = std::max(result.state.plan_cache_epoch,
                                               parsed->plan_cache_epoch);
      result.state.entries.push_back(std::move(*parsed));
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
  if (const auto binding = ValidateStatementBinding(
          request.context, request.mga_authority,
          request.selected_physical_dag,
          request.selected_catalog_epoch_uuid)) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context, kOperation, *binding);
  }
  const auto validation = ValidateCacheRequest(request);
  if (validation.error) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context, kOperation, validation);
  }

  const auto before = LoadOptimizerPlanLifecycleState(request.context);
  if (!before.ok) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context, kOperation, before.diagnostic);
  }
  const auto object_dependencies =
      *CanonicalObjectDependencies(request.object_dependency_uuids);
  const auto dependencies = DependencyIdentityFromDag(
      request.selected_physical_dag, object_dependencies);
  const std::uint64_t plan_cache_epoch = NextPlanCacheEpoch(before.state, request);
  const EngineUuid event_uuid = GenerateCrudEngineUuid("object");
  const auto appended = AppendEvent(
      request.context,
      "CACHE_PLAN",
      {{"record_schema", "optimizer_plan_metadata_v3"},
       {"event_uuid", event_uuid},
       {"metadata_only", "1"},
       {"plan_cache_epoch", std::to_string(plan_cache_epoch)},
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
       {"bound_sblr_tree_uuid", dependencies.bound_sblr_tree_uuid},
       {"catalog_epoch_uuid", dependencies.catalog_epoch_uuid},
       {"security_context_uuid", dependencies.security_context_uuid},
       {"capability_snapshot_uuid", dependencies.capability_snapshot_uuid},
       {"resource_snapshot_uuid", dependencies.resource_snapshot_uuid},
       {"statistics_snapshot_uuid", dependencies.statistics_snapshot_uuid},
       {"route_snapshot_uuid", dependencies.route_snapshot_uuid},
       {"dependency_catalog_generation_id",
        std::to_string(dependencies.catalog_generation_id)},
       {"dependency_security_epoch",
        std::to_string(dependencies.security_epoch)},
       {"dependency_policy_epoch", std::to_string(dependencies.policy_epoch)},
       {"dependency_resource_epoch",
        std::to_string(dependencies.resource_epoch)},
       {"dependency_statistics_generation",
        std::to_string(dependencies.statistics_generation)},
       {"dependency_route_epoch", std::to_string(dependencies.route_epoch)},
       {"dependency_route_generation",
        std::to_string(dependencies.route_generation)},
       {"object_dependency_uuids", object_dependencies},
       {"invalidated", "0"}});
  if (appended.error) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(request.context, kOperation, appended);
  }

  const auto loaded = LoadOptimizerPlanLifecycleState(request.context);
  if (!loaded.ok) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context, kOperation, loaded.diagnostic);
  }
  const auto* entry = FindEntryByEventUuid(loaded.state, event_uuid);
  if (entry == nullptr || !entry->metadata_only ||
      !DependencyIdentityEqual(entry->dependencies, dependencies)) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticCacheMiss,
                       "corrected metadata event did not replay exactly"));
  }
  if (const auto binding = ValidateStatementBinding(
          request.context, request.mga_authority,
          request.selected_physical_dag,
          request.selected_catalog_epoch_uuid)) {
    return DiagnosticResult<EngineOptimizerCachePlanResult>(
        request.context, kOperation, *binding);
  }

  auto publication_receipt =
      std::shared_ptr<EngineOptimizerPlanStatementUseReceipt>(
          new EngineOptimizerPlanStatementUseReceipt());
  publication_receipt->receipt_id_ = GenerateCrudEngineUuid("object");
  publication_receipt->plan_uuid_ = entry->plan_uuid;
  publication_receipt->dependencies_ = entry->dependencies;
  publication_receipt->statement_context_ =
      request.mga_authority.statement_context;
  publication_receipt->selected_physical_dag_ =
      request.selected_physical_dag;
  publication_receipt->resolve_current_ = request.mga_authority.resolve_current;
  publication_receipt->authority_origin_ = request.mga_authority.origin;
  publication_receipt->purpose_ =
      EngineOptimizerPlanReceiptPurpose::kPublication;
  publication_receipt->metadata_dependencies_revalidated_ = true;
  publication_receipt->exact_current_revalidated_before_issue_ = true;

  auto result = SuccessResult<EngineOptimizerCachePlanResult>(request.context, kOperation);
  result.entry = *entry;
  result.plan_cache_epoch = loaded.state.plan_cache_epoch;
  result.publication_receipt = std::move(publication_receipt);
  FillEntryResult(&result, result.entry);
  AddEvidence(&result, "optimizer_plan_publication_receipt",
              result.publication_receipt->receipt_id());
  AddEvidence(&result, "optimizer_plan_publication_receipt_immutable", "true");
  AddEvidence(&result, "optimizer_plan_publication_receipt_executable", "false");
  AddEvidence(&result, "optimizer_plan_mga_authority",
              "engine_transaction_inventory");
  return result;
}

EngineOptimizerValidateCachedPlanResult EngineOptimizerValidateCachedPlan(
    const EngineOptimizerValidateCachedPlanRequest& request) {
  constexpr const char* kOperation = "query.optimizer.validate_cached_plan";
  const auto refuse = [&](EngineApiDiagnostic diagnostic,
                          const bool invalidation_required = false) {
    auto result = DiagnosticResult<EngineOptimizerValidateCachedPlanResult>(
        request.context, kOperation, std::move(diagnostic));
    result.invalidation_required = invalidation_required;
    return result;
  };
  if (request.context.database_path.empty()) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticDatabasePathRequired,
                                 "database_path"));
  }
  if (!CanonicalUuid(request.plan_uuid) ||
      request.query_fingerprint.empty() ||
      !CanonicalUuid(request.index_uuid)) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest,
                                 "cache_lookup_key"));
  }
  if (const auto binding = ValidateStatementBinding(
          request.context, request.mga_authority,
          request.selected_physical_dag,
          request.selected_catalog_epoch_uuid)) {
    return refuse(*binding);
  }
  const auto object_dependencies =
      CanonicalObjectDependencies(request.object_dependency_uuids);
  if (!object_dependencies) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticDependencyMismatch,
                                 "canonical object dependency UUIDs are required"),
                  true);
  }
  const auto dependencies = DependencyIdentityFromDag(
      request.selected_physical_dag, *object_dependencies);
  if (!DependencyIdentityComplete(dependencies) ||
      dependencies.catalog_epoch_uuid !=
          request.selected_catalog_epoch_uuid) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticDependencyMismatch,
                                 "selected DAG dependency identity is incomplete"),
                  true);
  }

  const auto loaded = LoadOptimizerPlanLifecycleState(request.context);
  if (!loaded.ok) {
    return refuse(loaded.diagnostic, true);
  }
  const auto* entry =
      FindEntry(loaded.state, request.plan_uuid, request.query_fingerprint, request.index_uuid);
  if (entry == nullptr) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticCacheMiss,
                                 "requested_index"));
  }
  if (entry->invalidated) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticCacheInvalidated,
                                 entry->invalidation_reason),
                  true);
  }
  if (request.require_current_statistics && request.statistics_stale) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticStatisticsStale,
                                 "requested_index"),
                  true);
  }
  if (!EntryEpochsMatch(*entry, request.current_resource_epochs)) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticEpochMismatch,
                                 "requested_index"),
                  true);
  }
  if (!entry->metadata_only ||
      entry->event_schema_version !=
          kOptimizerPlanLifecycleEventSchemaVersion ||
      entry->plan_uuid != request.plan_uuid ||
      entry->query_fingerprint != request.query_fingerprint ||
      entry->index_uuid != request.index_uuid ||
      entry->plan_uuid != request.selected_physical_dag.selected_plan_uuid ||
      entry->index_generation != request.current_index_generation ||
      entry->statistics_generation != request.current_statistics_generation ||
      entry->catalog_generation_id != request.current_catalog_generation_id ||
      request.current_catalog_generation_id !=
          request.selected_physical_dag.catalog_generation ||
      request.current_statistics_generation !=
          request.selected_physical_dag.statistics_generation ||
      request.current_resource_epochs.resource_epoch !=
          request.selected_physical_dag.resource_epoch ||
      !DependencyIdentityEqual(entry->dependencies, dependencies)) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticCacheInvalidated,
                                 "cached metadata dependency mismatch"),
                  true);
  }
  if (const auto binding = ValidateStatementBinding(
          request.context, request.mga_authority,
          request.selected_physical_dag,
          request.selected_catalog_epoch_uuid)) {
    return refuse(*binding);
  }

  auto receipt = std::shared_ptr<EngineOptimizerPlanStatementUseReceipt>(
      new EngineOptimizerPlanStatementUseReceipt());
  receipt->receipt_id_ = GenerateCrudEngineUuid("object");
  receipt->plan_uuid_ = entry->plan_uuid;
  receipt->dependencies_ = entry->dependencies;
  receipt->statement_context_ = request.mga_authority.statement_context;
  receipt->selected_physical_dag_ = request.selected_physical_dag;
  receipt->resolve_current_ = request.mga_authority.resolve_current;
  receipt->authority_origin_ = request.mga_authority.origin;
  receipt->purpose_ = EngineOptimizerPlanReceiptPurpose::kStatementUse;
  receipt->metadata_dependencies_revalidated_ = true;
  receipt->exact_current_revalidated_before_issue_ = true;

  auto result = SuccessResult<EngineOptimizerValidateCachedPlanResult>(
      request.context, kOperation);
  result.entry = *entry;
  result.cache_hit = true;
  result.metadata_cache_hit = true;
  result.statement_use_admitted = false;
  result.plan_cache_epoch = loaded.state.plan_cache_epoch;
  result.statement_use_receipt = std::move(receipt);
  FillEntryResult(&result, result.entry);
  AddEvidence(&result, "optimizer_plan_metadata_cache_hit", "true");
  AddEvidence(&result, "optimizer_plan_metadata_executable", "false");
  AddEvidence(&result, "optimizer_plan_statement_use_admitted", "false");
  AddEvidence(&result, "optimizer_plan_statement_use_receipt",
              result.statement_use_receipt->receipt_id());
  AddEvidence(&result, "optimizer_plan_statement_use_receipt_immutable", "true");
  return result;
}

EngineOptimizerPlanUseValidationResult RevalidateOptimizerPlanStatementUse(
    const EngineOptimizerPlanCacheEntry& entry,
    const std::shared_ptr<const EngineOptimizerPlanStatementUseReceipt>&
        receipt) {
  EngineOptimizerPlanUseValidationResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic_code = std::move(code);
    result.detail = std::move(detail);
    return result;
  };
  if (!receipt ||
      receipt->purpose_ != EngineOptimizerPlanReceiptPurpose::kStatementUse) {
    return refuse(kOptimizerPlanDiagnosticMgaAuthorityRequired,
                  "a statement-use receipt, not a publication receipt, is required");
  }
  if (!entry.metadata_only || entry.invalidated ||
      entry.event_schema_version != kOptimizerPlanLifecycleEventSchemaVersion ||
      entry.plan_uuid != receipt->plan_uuid_ ||
      !DependencyIdentityComplete(entry.dependencies) ||
      !DependencyIdentityEqual(entry.dependencies, receipt->dependencies_) ||
      !receipt->metadata_dependencies_revalidated_ ||
      !receipt->exact_current_revalidated_before_issue_ ||
      receipt->authority_origin_ !=
          plan_executor::CanonicalMgaAuthorityOrigin::
              kEngineTransactionInventory ||
      receipt->selected_physical_dag_.abi_version != 2) {
    return refuse(kOptimizerPlanDiagnosticDependencyMismatch,
                  "statement-use receipt does not match complete corrected metadata");
  }
  const auto receipt_dependencies = DependencyIdentityFromDag(
      receipt->selected_physical_dag_,
      receipt->dependencies_.object_dependency_uuids);
  if (!DependencyIdentityComplete(receipt_dependencies) ||
      !DependencyIdentityEqual(receipt_dependencies, entry.dependencies) ||
      !CatalogIdentityIndependent(entry.dependencies.catalog_epoch_uuid,
                                  receipt->statement_context_)) {
    return refuse(kOptimizerPlanDiagnosticDependencyMismatch,
                  "receipt DAG or catalog identity differs from cached metadata");
  }
  plan_executor::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = receipt->statement_context_;
  authority.resolve_current = receipt->resolve_current_;
  authority.origin = receipt->authority_origin_;
  const auto revalidated =
      plan_executor::RevalidateCanonicalExecutionMgaAuthority(
          authority, receipt->selected_physical_dag_);
  if (!revalidated.ok) {
    return refuse(kOptimizerPlanDiagnosticMgaAuthorityRequired,
                  revalidated.diagnostic_code + ":" + revalidated.detail);
  }
  result.ok = true;
  result.diagnostic_code = kOptimizerPlanDiagnosticOk;
  result.executable_receipt = receipt;
  result.identity_evidence.emplace_back("optimizer_plan_statement_use_receipt", receipt->receipt_id_);
  result.evidence = {
      "optimizer_plan_statement_use_receipt_present=true",
      "optimizer_plan_statement_use_receipt_executable=true",
      "optimizer_plan_metadata_authority=none",
      "optimizer_plan_mga_authority=engine_transaction_inventory",
      "optimizer_plan_exact_current_revalidated_before_execution=true",
  };
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
  if ((!request.invalidate_all || !request.index_uuid.is_nil()) &&
      !CanonicalUuid(request.index_uuid)) {
    return DiagnosticResult<EngineOptimizerInvalidatePlanCacheResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest, "index_uuid"));
  }
  if (request.new_index_generation == 0 ||
      request.new_statistics_generation == 0 ||
      request.new_catalog_generation_id == 0 ||
      request.new_resource_epochs.resource_epoch == 0 ||
      request.new_resource_epochs.charset_epoch == 0 ||
      request.new_resource_epochs.collation_epoch == 0) {
    return DiagnosticResult<EngineOptimizerInvalidatePlanCacheResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest,
                       "complete nonzero invalidation dependency generations are required"));
  }

  const auto before = LoadOptimizerPlanLifecycleState(request.context);
  if (!before.ok) {
    return DiagnosticResult<EngineOptimizerInvalidatePlanCacheResult>(
        request.context, kOperation, before.diagnostic);
  }
  if (before.state.plan_cache_epoch ==
      std::numeric_limits<std::uint64_t>::max()) {
    return DiagnosticResult<EngineOptimizerInvalidatePlanCacheResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest,
                       "plan_cache_epoch_exhausted"));
  }

  const std::uint64_t plan_cache_epoch =
      std::max({before.state.plan_cache_epoch + 1,
                request.new_index_generation,
                request.new_statistics_generation,
                request.new_catalog_generation_id,
                request.new_resource_epochs.resource_epoch,
                request.new_resource_epochs.charset_epoch,
                request.new_resource_epochs.collation_epoch});
  const std::string reason =
      request.reason.empty() ? "explicit_invalidation" : request.reason;
  const EngineUuid event_uuid = GenerateCrudEngineUuid("object");
  const auto appended = AppendEvent(
      request.context,
      "INVALIDATE",
      {{"record_schema", "optimizer_plan_invalidation_v3"},
       {"event_uuid", event_uuid},
       {"metadata_only", "1"},
       {"plan_cache_epoch", std::to_string(plan_cache_epoch)},
       {"index_uuid", request.index_uuid},
       {"reason", reason},
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
  AddEvidence(&result, "optimizer_plan_metadata_only", "true");
  AddEvidence(&result, "optimizer_plan_metadata_authority", "none");
  AddRow(&result,
         {{"plan_cache_epoch", std::to_string(result.plan_cache_epoch)},
          {"invalidation_events", std::to_string(result.state.invalidation_events)},
          {"reason", reason}});
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
  const auto before = LoadOptimizerPlanLifecycleState(request.context);
  if (!before.ok) {
    return DiagnosticResult<EngineOptimizerRecoverPlanCacheResult>(
        request.context, kOperation, before.diagnostic);
  }
  if (before.state.plan_cache_epoch ==
      std::numeric_limits<std::uint64_t>::max()) {
    return DiagnosticResult<EngineOptimizerRecoverPlanCacheResult>(
        request.context,
        kOperation,
        PlanDiagnostic(kOptimizerPlanDiagnosticInvalidRequest,
                       "plan_cache_epoch_exhausted"));
  }
  const std::uint64_t plan_cache_epoch = before.state.plan_cache_epoch + 1;
  const EngineUuid event_uuid = GenerateCrudEngineUuid("object");
  const EngineUuid snapshot_uuid = GenerateCrudEngineUuid("object");
  const auto appended = AppendEvent(
      request.context,
      "RECOVERY_SNAPSHOT",
      {{"record_schema", "optimizer_plan_recovery_v3"},
       {"event_uuid", event_uuid},
       {"metadata_only", "1"},
       {"plan_cache_epoch", std::to_string(plan_cache_epoch)},
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
  AddEvidence(&result, "optimizer_plan_recovery_metadata_only", "true");
  AddEvidence(&result, "optimizer_plan_recovery_authority", "none");
  AddRow(&result,
         {{"recovery_snapshot_uuid", snapshot_uuid},
          {"plan_cache_epoch", std::to_string(result.state.plan_cache_epoch)},
          {"recovered_entries", std::to_string(result.state.entries.size())},
          {"statement_snapshot_reconstructed", "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
