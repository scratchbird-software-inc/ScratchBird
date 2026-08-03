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

struct RawPlanEvent {
  std::uint64_t event_sequence = 0;
  std::uint32_t event_schema_version = 0;
  std::string event_kind;
  std::map<std::string, std::string> fields;
  bool malformed = false;
  bool legacy = false;
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

bool HexDecode(const std::string& value, std::string* out) {
  out->clear();
  if ((value.size() % 2) != 0) {
    return false;
  }
  out->reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) {
      out->clear();
      return false;
    }
    out->push_back(static_cast<char>((hi << 4) | lo));
  }
  return true;
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
  return parsed;
}

std::optional<bool> ParseBool(const std::string& value) {
  if (value == "1") return true;
  if (value == "0") return false;
  return std::nullopt;
}

std::string BoolText(bool value) {
  return value ? "1" : "0";
}

const std::string* Field(const std::map<std::string, std::string>& fields,
                         const std::string& key) {
  const auto it = fields.find(key);
  return it == fields.end() ? nullptr : &it->second;
}

std::optional<std::uint64_t> FieldU64(
    const std::map<std::string, std::string>& fields,
    const std::string& key) {
  const auto* value = Field(fields, key);
  return value == nullptr ? std::nullopt : ParseU64(*value);
}

std::optional<bool> FieldBool(
    const std::map<std::string, std::string>& fields,
    const std::string& key) {
  const auto* value = Field(fields, key);
  return value == nullptr ? std::nullopt : ParseBool(*value);
}

bool ExactFields(const std::map<std::string, std::string>& fields,
                 std::initializer_list<const char*> required) {
  if (fields.size() != required.size()) return false;
  return std::all_of(required.begin(), required.end(), [&](const char* key) {
    return fields.contains(key);
  });
}

bool CanonicalUuid(const std::string& value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

std::optional<std::vector<std::string>> ParseUuidVector(
    const std::string& encoded) {
  if (encoded.empty() || encoded.front() == ',' || encoded.back() == ',' ||
      encoded.find(",,") != std::string::npos) {
    return std::nullopt;
  }
  auto values = Split(encoded, ',');
  if (values.empty() || !std::ranges::is_sorted(values) ||
      std::adjacent_find(values.begin(), values.end()) != values.end() ||
      !std::ranges::all_of(values, CanonicalUuid)) {
    return std::nullopt;
  }
  return values;
}

std::uint64_t StableHash(std::string_view value, std::uint64_t seed) {
  std::uint64_t hash = seed;
  for (const auto ch : value) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string DeterministicCanonicalUuid(std::string_view seed) {
  const std::uint64_t left = StableHash(seed, 1469598103934665603ull);
  const std::uint64_t right =
      StableHash(seed, 1099511628211ull ^ 0x9e3779b97f4a7c15ull);
  const auto part1 = static_cast<std::uint32_t>(left >> 32);
  const auto part2 = static_cast<std::uint16_t>(left >> 16);
  const auto part3 = static_cast<std::uint16_t>((left & 0x0fffull) | 0x7000ull);
  const auto part4 =
      static_cast<std::uint16_t>(((right >> 48) & 0x3fffull) | 0x8000ull);
  const auto part5 = right & 0x0000ffffffffffffull;
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::nouppercase
      << std::setw(8) << part1 << '-' << std::setw(4) << part2 << '-'
      << std::setw(4) << part3 << '-' << std::setw(4) << part4 << '-'
      << std::setw(12) << part5;
  return out.str();
}

std::optional<std::vector<std::string>> CanonicalObjectDependencies(
    std::vector<std::string> values) {
  if (values.empty() || !std::ranges::all_of(values, CanonicalUuid) ||
      !std::ranges::is_sorted(values)) {
    return std::nullopt;
  }
  if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
    return std::nullopt;
  }
  return values;
}

std::string JoinUuidVector(const std::vector<std::string>& values) {
  std::ostringstream out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) out << ',';
    out << values[index];
  }
  return out.str();
}

EngineOptimizerPlanDependencyIdentity DependencyIdentityFromDag(
    const plan_executor::TypedPhysicalNodeDag& dag,
    std::vector<std::string> object_dependencies) {
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
    const std::string& catalog_epoch_uuid,
    const plan_executor::PhysicalMgaStatementContext& context) {
  return CanonicalUuid(catalog_epoch_uuid) &&
         catalog_epoch_uuid != context.statement_uuid &&
         catalog_epoch_uuid != context.owning_transaction_uuid &&
         catalog_epoch_uuid != context.statement_snapshot_uuid &&
         catalog_epoch_uuid != context.statement_metadata_snapshot_uuid;
}

std::string StatementContextText(
    const plan_executor::PhysicalMgaStatementContext& context) {
  std::ostringstream out;
  out << context.statement_uuid << '|' << context.owning_transaction_uuid
      << '|' << context.statement_snapshot_uuid << '|'
      << context.statement_metadata_snapshot_uuid << '|'
      << context.owning_local_transaction_id << '|'
      << context.visible_committed_high_watermark << '|'
      << context.oldest_active_transaction_id << '|'
      << context.oldest_interesting_transaction_id << '|'
      << context.oldest_snapshot_transaction_id << '|'
      << context.retention_horizon_transaction_id << '|';
  for (const auto value : context.active_excluded_local_transaction_ids) {
    out << value << ',';
  }
  out << '|';
  for (const auto value : context.in_doubt_excluded_local_transaction_ids) {
    out << value << ',';
  }
  out << '|' << context.snapshot_kind << '|'
      << context.publication_inventory_next_local_transaction_id << '|'
      << context.inventory_authoritative << '|' << context.complete << '|'
      << context.current;
  return out.str();
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
    const std::string& selected_catalog_epoch_uuid) {
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
  if (context.transaction_uuid.canonical !=
          statement.owning_transaction_uuid ||
      context.local_transaction_id != statement.owning_local_transaction_id ||
      context.statement_uuid.canonical != statement.statement_uuid ||
      context.statement_snapshot_uuid.canonical !=
          statement.statement_snapshot_uuid ||
      context.statement_metadata_snapshot_uuid.canonical !=
          statement.statement_metadata_snapshot_uuid ||
      context.catalog_epoch_uuid.canonical != selected_catalog_epoch_uuid ||
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
      context.optimizer_capability_snapshot_uuid.canonical !=
          dag.capability_snapshot_uuid ||
      context.optimizer_resource_snapshot_uuid.canonical !=
          dag.resource_snapshot_uuid ||
      context.optimizer_route_snapshot_uuid.canonical !=
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

std::string MakeEvent(std::string event_kind,
                      std::vector<std::pair<std::string, std::string>> fields) {
  std::string out = std::string(kOptimizerPlanLifecycleEventMagic) + "\t" +
                    std::to_string(kOptimizerPlanLifecycleEventSchemaVersion) +
                    "\t" + std::move(event_kind);
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
                                std::vector<std::pair<std::string, std::string>> fields) {
  if (context.database_path.empty()) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticDatabasePathRequired, "database_path");
  }
  std::ofstream out(EventPath(context), std::ios::binary | std::ios::app);
  if (!out) {
    return PlanDiagnostic(kOptimizerPlanDiagnosticWriteFailed, "open");
  }
  out << MakeEvent(std::move(event_kind), std::move(fields)) << '\n';
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
    RawPlanEvent event;
    event.event_sequence = event_sequence;
    if (!parts.empty() &&
        parts[0] == kOptimizerPlanLifecycleLegacyEventMagic) {
      event.legacy = true;
      if (parts.size() > 1) {
        event.event_kind = parts[1];
      }
      events->push_back(std::move(event));
      continue;
    }
    if (parts.size() < 4 ||
        parts[0] != kOptimizerPlanLifecycleEventMagic ||
        parts[1] !=
            std::to_string(kOptimizerPlanLifecycleEventSchemaVersion) ||
        parts[2].empty()) {
      event.malformed = true;
      events->push_back(std::move(event));
      continue;
    }
    event.event_schema_version = kOptimizerPlanLifecycleEventSchemaVersion;
    event.event_kind = parts[2];
    for (std::size_t i = 3; i < parts.size(); ++i) {
      const auto pos = parts[i].find('=');
      if (pos == std::string::npos || pos == 0) {
        event.malformed = true;
        break;
      }
      const std::string key = parts[i].substr(0, pos);
      std::string value;
      if (event.fields.contains(key) ||
          !HexDecode(parts[i].substr(pos + 1), &value)) {
        event.malformed = true;
        break;
      }
      event.fields.emplace(key, std::move(value));
    }
    events->push_back(std::move(event));
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
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
      ParseUuidVector(*Field(event.fields, "object_dependency_uuids"));
  const auto& relation_uuid = *Field(event.fields, "relation_uuid");
  const auto& index_uuid = *Field(event.fields, "index_uuid");
  if (*Field(event.fields, "record_schema") !=
          "optimizer_plan_metadata_v2" ||
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
    if (!CanonicalUuid(*Field(event.fields, key))) return false;
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
          "optimizer_plan_invalidation_v2" ||
      !metadata_only.value_or(false) || !invalidate_all ||
      !plan_cache_epoch || *plan_cache_epoch == 0 ||
      !CanonicalUuid(*Field(event.fields, "event_uuid")) ||
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
  const auto& index_uuid = *Field(event.fields, "index_uuid");
  return *invalidate_all ? (index_uuid.empty() || CanonicalUuid(index_uuid))
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
             "optimizer_plan_recovery_v2" &&
         metadata_only.value_or(false) && plan_cache_epoch &&
         *plan_cache_epoch != 0 &&
         CanonicalUuid(*Field(event.fields, "event_uuid")) &&
         CanonicalUuid(*Field(event.fields, "recovery_snapshot_uuid"));
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
  entry.event_uuid = *Field(event.fields, "event_uuid");
  entry.event_sequence = event.event_sequence;
  entry.plan_cache_epoch = *FieldU64(event.fields, "plan_cache_epoch");
  entry.plan_uuid = *Field(event.fields, "plan_uuid");
  entry.query_fingerprint = *Field(event.fields, "query_fingerprint");
  entry.relation_uuid = *Field(event.fields, "relation_uuid");
  entry.index_uuid = *Field(event.fields, "index_uuid");
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
      *Field(event.fields, "bound_sblr_tree_uuid");
  entry.dependencies.catalog_epoch_uuid =
      *Field(event.fields, "catalog_epoch_uuid");
  entry.dependencies.security_context_uuid =
      *Field(event.fields, "security_context_uuid");
  entry.dependencies.capability_snapshot_uuid =
      *Field(event.fields, "capability_snapshot_uuid");
  entry.dependencies.resource_snapshot_uuid =
      *Field(event.fields, "resource_snapshot_uuid");
  entry.dependencies.statistics_snapshot_uuid =
      *Field(event.fields, "statistics_snapshot_uuid");
  entry.dependencies.route_snapshot_uuid =
      *Field(event.fields, "route_snapshot_uuid");
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
      *ParseUuidVector(*Field(event.fields, "object_dependency_uuids"));
  entry.metadata_only = true;
  entry.invalidated = false;
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

const EngineOptimizerPlanCacheEntry* FindEntryByEventUuid(
    const EngineOptimizerPlanLifecycleState& state,
    const std::string& event_uuid) {
  for (auto it = state.entries.rbegin(); it != state.entries.rend(); ++it) {
    if (it->event_uuid == event_uuid) return &*it;
  }
  return nullptr;
}

void ApplyInvalidation(EngineOptimizerPlanLifecycleState* state, const RawPlanEvent& event) {
  const bool invalidate_all = *FieldBool(event.fields, "invalidate_all");
  const std::string& index_uuid = *Field(event.fields, "index_uuid");
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
  if (touched || invalidate_all || !index_uuid.empty()) {
    ++state->invalidation_events;
  }
  state->plan_cache_epoch = std::max(state->plan_cache_epoch, plan_cache_epoch);
}

void ApplyRecoverySnapshot(EngineOptimizerPlanLifecycleState* state, const RawPlanEvent& event) {
  state->plan_cache_epoch =
      std::max(state->plan_cache_epoch,
               *FieldU64(event.fields, "plan_cache_epoch"));
  state->recovered_from_persisted_evidence = true;
  state->recovery_snapshot_uuid = *Field(event.fields, "recovery_snapshot_uuid");
  for (auto& entry : state->entries) {
    entry.recovered_from_persisted_evidence = true;
  }
}

void FillEntryResult(EngineApiResult* result, const EngineOptimizerPlanCacheEntry& entry) {
  result->primary_object.uuid.canonical = entry.plan_uuid;
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
      scratchbird::core::uuid::UuidToString(
          request.index_descriptor.index_uuid.value) != request.index_uuid ||
      scratchbird::core::uuid::UuidToString(
          request.index_descriptor.table_uuid.value) !=
          request.relation_uuid ||
      !request.statistics.index_uuid.valid() ||
      scratchbird::core::uuid::UuidToString(request.statistics.index_uuid.value) !=
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
    result.state.recovery_snapshot_uuid.clear();
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
  const std::string event_uuid = DeterministicCanonicalUuid(
      "optimizer-plan-cache-event-v2|" + request.context.database_path + "|" +
      request.plan_uuid + "|" + std::to_string(plan_cache_epoch) + "|" +
      dependencies.statistics_snapshot_uuid);
  const auto appended = AppendEvent(
      request.context,
      "CACHE_PLAN",
      {{"record_schema", "optimizer_plan_metadata_v2"},
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
       {"object_dependency_uuids", JoinUuidVector(object_dependencies)},
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
  publication_receipt->receipt_id_ = DeterministicCanonicalUuid(
      "optimizer-plan-publication-receipt-v2|" + event_uuid + "|" +
      StatementContextText(request.mga_authority.statement_context));
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
                                 request.index_uuid));
  }
  if (entry->invalidated) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticCacheInvalidated,
                                 entry->invalidation_reason),
                  true);
  }
  if (request.require_current_statistics && request.statistics_stale) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticStatisticsStale,
                                 request.index_uuid),
                  true);
  }
  if (!EntryEpochsMatch(*entry, request.current_resource_epochs)) {
    return refuse(PlanDiagnostic(kOptimizerPlanDiagnosticEpochMismatch,
                                 request.index_uuid),
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
  receipt->receipt_id_ = DeterministicCanonicalUuid(
      "optimizer-plan-statement-use-receipt-v2|" + entry->event_uuid + "|" +
      StatementContextText(request.mga_authority.statement_context));
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
  result.evidence = {
      "optimizer_plan_statement_use_receipt=" + receipt->receipt_id_,
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
  if ((!request.invalidate_all || !request.index_uuid.empty()) &&
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
  const std::string event_uuid = DeterministicCanonicalUuid(
      "optimizer-plan-invalidation-event-v2|" + request.context.database_path +
      "|" + std::to_string(plan_cache_epoch) + "|" + request.index_uuid +
      "|" + BoolText(request.invalidate_all) + "|" +
      std::to_string(request.new_index_generation) + "|" +
      std::to_string(request.new_statistics_generation) + "|" +
      std::to_string(request.new_catalog_generation_id) + "|" +
      std::to_string(request.new_resource_epochs.resource_epoch) + "|" +
      std::to_string(request.new_resource_epochs.charset_epoch) + "|" +
      std::to_string(request.new_resource_epochs.collation_epoch));
  const auto appended = AppendEvent(
      request.context,
      "INVALIDATE",
      {{"record_schema", "optimizer_plan_invalidation_v2"},
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
  const std::string event_uuid = DeterministicCanonicalUuid(
      "optimizer-plan-recovery-event-v2|" + request.context.database_path +
      "|" + std::to_string(plan_cache_epoch) + "|" +
      std::to_string(before.state.max_event_sequence + 1) + "|" +
      std::to_string(request.context.local_transaction_id));
  const std::string snapshot_uuid = DeterministicCanonicalUuid(
      "optimizer-plan-recovery-snapshot-v2|" + request.context.database_path +
      "|" + std::to_string(plan_cache_epoch) + "|" + event_uuid);
  const auto appended = AppendEvent(
      request.context,
      "RECOVERY_SNAPSHOT",
      {{"record_schema", "optimizer_plan_recovery_v2"},
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
