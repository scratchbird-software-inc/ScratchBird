// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/temporal_bitemporal_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/binary_catalog_metadata.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::string_view kTemporalPeriodKind = "temporal_period";
constexpr std::string_view kTemporalDmlEventKind = "temporal_dml_event";

std::string Lower(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string OptionValue(const EngineApiRequest& request, std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) {
      return option.substr(prefix.size());
    }
  }
  return {};
}

std::vector<std::string> OptionValues(const EngineApiRequest& request,
                                      std::string_view prefix) {
  std::vector<std::string> values;
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) {
      values.push_back(option.substr(prefix.size()));
    }
  }
  return values;
}

bool OptionBool(const EngineApiRequest& request, std::string_view key) {
  const std::string value = Lower(OptionValue(request, key));
  return value == "true" || value == "1" || value == "yes";
}

bool DecodeTemporalPayload(std::string_view payload, BinaryCatalogMetadata* fields) {
  BinaryCatalogMetadata decoded;
  if (!DecodeBinaryCatalogMetadata(payload, "temporal.v2", &decoded)) return false;
  for (const auto& [key, identity] : decoded.identities) {
    if ((key != "table_uuid" && key != "period_uuid") ||
        !core::uuid::IsEngineIdentityUuid(identity)) return false;
  }
  *fields = std::move(decoded);
  return true;
}
std::string PayloadField(const BinaryCatalogMetadata& fields, std::string_view key) {
  const auto found = fields.text.find(std::string(key));
  return found == fields.text.end() ? std::string{} : found->second;
}
EngineUuid PayloadUuid(const BinaryCatalogMetadata& fields, const std::string& key) {
  return BinaryCatalogUuid(fields, key);
}

std::optional<std::string> CanonicalAxis(std::string axis) {
  axis = Lower(std::move(axis));
  if (axis == "system_time" || axis == "system-time" || axis == "system") {
    return "system_time";
  }
  if (axis == "application_time" || axis == "application-time" ||
      axis == "application" || axis == "valid_time" ||
      axis == "valid-time" || axis == "valid") {
    return "application_time";
  }
  return std::nullopt;
}

EngineUuid TableUuid(const EngineApiRequest& request) {
  for (const auto& object : request.related_objects) {
    if (object.object_kind == "table" && !object.uuid.is_nil()) return object.uuid;
  }
  return request.target_object.object_kind == "table" ? request.target_object.uuid : EngineUuid{};
}
EngineUuid PeriodUuid(const EngineApiRequest& request) {
  for (const auto& object : request.related_objects) {
    if (object.object_kind == kTemporalPeriodKind && !object.uuid.is_nil()) return object.uuid;
  }
  return request.target_object.object_kind == kTemporalPeriodKind ? request.target_object.uuid : EngineUuid{};
}

bool MatchesTarget(const EngineUuid& grant_target,
                   const EngineUuid& target_uuid,
                   const EngineApiRequest& request) {
  return grant_target.is_nil() ||
         grant_target == target_uuid ||
         grant_target == request.target_database.uuid ||
         grant_target == request.target_schema.uuid ||
         grant_target == request.target_object.uuid;
}

bool SubjectMatches(const EngineMaterializedAuthorizationContext& auth,
                    const EngineUuid& subject_uuid,
                    const EngineRequestContext& context) {
  if (!subject_uuid.is_nil() &&
      subject_uuid == context.principal_uuid) {
    return true;
  }
  for (const auto& subject : auth.effective_subjects) {
    if (!subject.subject_uuid.is_nil() &&
        subject.subject_uuid == subject_uuid) {
      return true;
    }
  }
  return false;
}

bool EvidenceTagGrants(const EngineRequestContext& context,
                       std::string_view right) {
  for (const auto& tag : context.authorization_context.evidence_tags) {
    const std::string lower = Lower(tag);
    if (lower == "sysarch" || lower == "right.all" ||
        lower == Lower(std::string(right)) ||
        lower == "right." + Lower(std::string(right))) {
      return true;
    }
  }
  return false;
}

bool HasRight(const EngineApiRequest& request,
              std::string_view right,
              const EngineUuid& target_uuid) {
  const auto& context = request.context;
  const auto& auth = context.authorization_context;
  if (!context.security_context_present || !auth.present) {
    return false;
  }
  if (EvidenceTagGrants(context, right)) {
    return true;
  }
  bool allowed = false;
  for (const auto& grant : auth.grants) {
    if (!SubjectMatches(auth, grant.subject_uuid, context)) {
      continue;
    }
    const bool right_matches = grant.right == right ||
                               grant.right == "ALL" ||
                               grant.right == "SYSARCH";
    if (!right_matches ||
        !MatchesTarget(grant.target_uuid, target_uuid, request)) {
      continue;
    }
    if (grant.deny) {
      return false;
    }
    allowed = true;
  }
  return allowed;
}

bool ContainsRawSqlText(const EngineApiRequest& request) {
  for (const auto& option : request.option_envelopes) {
    if (Lower(option).find("sql_text") != std::string::npos ||
        Lower(option).find("source_sql") != std::string::npos) {
      return true;
    }
  }
  for (const auto& descriptor : request.descriptors) {
    const std::string encoded = Lower(descriptor.encoded_descriptor);
    if (encoded.find("sql_text") != std::string::npos ||
        encoded.find("source_sql") != std::string::npos) {
      return true;
    }
  }
  return false;
}

EngineApiDiagnostic ValidateBase(const EngineApiRequest& request,
                                 std::string_view operation_id,
                                 bool require_transaction,
                                 std::string_view right,
                                 const EngineUuid& target_uuid) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "table_uuid:") || StartsWith(option, "period_uuid:")) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                           "temporal_UUID_requires_binary_object_binding");
    }
  }
  for (const auto identity : {TableUuid(request), PeriodUuid(request)}) {
    if (!identity.is_nil() && !core::uuid::IsEngineIdentityUuid(identity)) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                           "temporal_bound_identity_invalid");
    }
  }
  if (ContainsRawSqlText(request)) {
    return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                        "temporal_engine_request_must_not_contain_sql_text");
  }
  const auto context_status = ValidateApiBehaviorContext(request.context,
                                                        std::string(operation_id),
                                                        require_transaction,
                                                        true);
  if (context_status.error) {
    return context_status;
  }
  if (!request.context.security_context_present) {
    return MakeSecurityContextRequiredDiagnostic(std::string(operation_id));
  }
  if (!HasRight(request, right, target_uuid)) {
    return MakeEngineApiDiagnostic("SECURITY.AUTHORIZATION.DENIED",
                                   "security.authorization.denied",
                                   std::string(right) + "_required");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool AllowedBoundType(const std::string& type) {
  const std::string lower = Lower(type);
  return lower == "date" || lower == "timestamp" ||
         lower == "timestamp_tz" || lower == "timestamp with time zone";
}

EngineApiDiagnostic ValidateBounds(const EngineApiRequest& request,
                                   std::string_view operation_id,
                                   std::string_view from_key,
                                   std::string_view to_key) {
  const std::string from = OptionValue(request, from_key);
  const std::string to = OptionValue(request, to_key);
  if (!from.empty() && !to.empty() && from > to) {
    return MakeEngineApiDiagnostic("SBSQL.TEMPORAL_RANGE_REVERSED",
                                   "sbsql.temporal.range_reversed",
                                   std::string(from_key) + ">" + std::string(to_key));
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ValidateRepeatedAxes(const EngineApiRequest& request,
                                         std::string_view operation_id) {
  std::set<std::string> seen;
  for (const auto& axis_text : OptionValues(request, "axis:")) {
    const auto axis = CanonicalAxis(axis_text);
    if (!axis.has_value()) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "temporal_axis_invalid");
    }
    if (!seen.insert(*axis).second) {
      return MakeEngineApiDiagnostic("SBSQL.TEMPORAL_AXIS_REPEATED",
                                     "sbsql.temporal_axis_repeated",
                                     *axis);
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::optional<std::string> TemporalPayload(const EngineApiRequest& request,
                            std::string_view action,
                            std::string_view axis) {
  BinaryCatalogMetadata fields;
  const auto append = [&](std::string key, std::string value) {
    if (!value.empty()) fields.text.emplace(std::move(key), std::move(value));
  };
  const auto append_identity = [&](std::string key, const EngineUuid& identity) {
    if (!identity.is_nil()) fields.identities.emplace(std::move(key), identity);
  };
  append("temporal_action", std::string(action));
  append("axis", std::string(axis));
  append_identity("period_uuid", PeriodUuid(request));
  append("period_name", ApiBehaviorPrimaryName(request, "unnamed_temporal_period"));
  append_identity("table_uuid", TableUuid(request));
  append("system_versioning", OptionValue(request, "system_versioning:"));
  append("history_table", OptionValue(request, "history_table:"));
  append("history_retention", OptionValue(request, "history_retention:"));
  append("history_table_filespace", OptionValue(request, "history_table_filespace:"));
  append("history_table_visible", OptionValue(request, "history_table_visible:"));
  append("history_table_storage_engine", OptionValue(request, "history_table_storage_engine:"));
  append("start_bound_type", OptionValue(request, "start_bound_type:"));
  append("end_bound_type", OptionValue(request, "end_bound_type:"));
  append("system_time_from", OptionValue(request, "system_time_from:"));
  append("system_time_to", OptionValue(request, "system_time_to:"));
  append("application_time_from", OptionValue(request, "application_time_from:"));
  append("application_time_to", OptionValue(request, "application_time_to:"));
  append("history_disposition", OptionValue(request, "history_disposition:"));
  append("mga_snapshot_visible_through",
         std::to_string(request.context.snapshot_visible_through_local_transaction_id));
  append("local_transaction_id", std::to_string(request.context.local_transaction_id));
  append("temporal_filter_order", "temporal_before_rls");
  append("rls_evaluation_timestamp", "live_time");
  append("mask_evaluation_timestamp", "live_time_default");
  append("period_descriptor_storage", "table_descriptor_field");
  append("period_uuid_kind", "uuidv7");
  append("cluster_scope_preserved", "true");
  append("parser_sql_authority", "false");
  append("mga_finality_authority", "engine");
  std::string payload;
  if (!EncodeBinaryCatalogMetadata(fields, "temporal.v2", &payload)) return std::nullopt;
  return payload;
}

void AddTemporalEvidence(EngineApiResult* result,
                         std::string_view action,
                         std::string_view operation_id) {
  AddApiBehaviorEvidence(result, "temporal_operation", std::string(action));
  AddApiBehaviorEvidence(result, "engine_api_function", std::string(operation_id));
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "parser_finality_authority", "false");
  AddApiBehaviorEvidence(result, "mga_authority_boundary", "engine_owned");
  AddApiBehaviorEvidence(result, "temporal_filter_order", "temporal_before_rls");
  AddApiBehaviorEvidence(result, "rls_evaluation_timestamp", "live_time");
  AddApiBehaviorEvidence(result, "mask_evaluation_timestamp", "live_time_default");
  AddApiBehaviorEvidence(result, "period_catalog_identity", "table_descriptor_field_uuidv7");
}

template <typename TResult>
TResult DiagnosticResult(const EngineApiRequest& request,
                         std::string operation_id,
                         EngineApiDiagnostic diagnostic) {
  return MakeApiBehaviorDiagnostic<TResult>(request.context,
                                            std::move(operation_id),
                                            std::move(diagnostic));
}

EngineApiDiagnostic AddTemporalPeriodRows(EngineApiResult* result,
                           const EngineApiRequest& request,
                           const EngineUuid& table_filter,
                           const EngineUuid& period_filter) {
  EngineApiDiagnostic diagnostic;
  const auto records = VisibleApiBehaviorRecords(request.context,
                                                std::string(kTemporalPeriodKind),
                                                request.context.local_transaction_id, diagnostic);
  if (diagnostic.error) return diagnostic;
  for (const auto& record : records) {
    BinaryCatalogMetadata fields;
    if (!DecodeTemporalPayload(record.payload, &fields)) {
      return MakeInvalidRequestDiagnostic(request.operation_id,
                                           "temporal_catalog_payload_invalid");
    }
    const auto table_uuid = PayloadUuid(fields, "table_uuid");
    if (!table_filter.is_nil() && table_uuid != table_filter) {
      continue;
    }
    if (!period_filter.is_nil() && record.object_uuid != period_filter &&
        PayloadUuid(fields, "period_uuid") != period_filter) {
      continue;
    }
    AddApiBehaviorRow(result,
                      {{"period_uuid", record.object_uuid},
                       {"period_name", record.default_name},
                       {"table_uuid", table_uuid},
                       {"axis", PayloadField(fields, "axis")},
                       {"system_versioning", PayloadField(fields, "system_versioning")},
                       {"history_table_visible", PayloadField(fields, "history_table_visible")},
                       {"period_descriptor_storage", "table_descriptor_field"},
                       {"period_uuid_kind", "uuidv7"},
                       {"mga_snapshot_visible_through",
                        std::to_string(request.context.snapshot_visible_through_local_transaction_id)}});
  }
  return diagnostic;
}

}  // namespace

EngineCreateTemporalPeriodResult EngineCreateTemporalPeriod(
    const EngineCreateTemporalPeriodRequest& request) {
  constexpr std::string_view kOperation = "versioned.bitemporal.create_period";
  const auto table_uuid = TableUuid(request);
  const auto base = ValidateBase(request,
                                 kOperation,
                                 true,
                                 "TEMPORAL_HISTORY_ADMIN",
                                 table_uuid);
  if (base.error) {
    return DiagnosticResult<EngineCreateTemporalPeriodResult>(request,
                                                              std::string(kOperation),
                                                              base);
  }
  const auto axis = CanonicalAxis(OptionValue(request, "axis:"));
  if (!axis.has_value()) {
    return DiagnosticResult<EngineCreateTemporalPeriodResult>(
        request, std::string(kOperation),
        MakeInvalidRequestDiagnostic(std::string(kOperation), "temporal_axis_required"));
  }
  if (*axis == "system_time" &&
      (!OptionBool(request, "start_generated_always:") ||
       !OptionBool(request, "end_generated_always:") ||
       OptionBool(request, "system_time_user_writable:"))) {
    return DiagnosticResult<EngineCreateTemporalPeriodResult>(
        request, std::string(kOperation),
        MakeEngineApiDiagnostic("SBSQL.TEMPORAL_SYSTEM_TIME_GENERATED_ALWAYS_REQUIRED",
                                "sbsql.temporal.system_time_generated_always_required",
                                "system_time_bounds_must_be_generated_always"));
  }
  const std::string start_type = OptionValue(request, "start_bound_type:");
  const std::string end_type = OptionValue(request, "end_bound_type:");
  if (*axis == "application_time") {
    if (!AllowedBoundType(start_type) || !AllowedBoundType(end_type)) {
      return DiagnosticResult<EngineCreateTemporalPeriodResult>(
          request, std::string(kOperation),
          MakeInvalidRequestDiagnostic(std::string(kOperation),
                                       "application_time_bound_type_invalid"));
    }
    if (Lower(start_type) != Lower(end_type)) {
      return DiagnosticResult<EngineCreateTemporalPeriodResult>(
          request, std::string(kOperation),
          MakeEngineApiDiagnostic("SBSQL.TEMPORAL_BOUND_TYPE_MISMATCH",
                                  "sbsql.temporal.bound_type_mismatch",
                                  "mixed_application_time_bound_types_refused"));
    }
  }

  const auto payload = TemporalPayload(request, "create_period", *axis);
  if (!payload) {
    return DiagnosticResult<EngineCreateTemporalPeriodResult>(request, std::string(kOperation),
        MakeInvalidRequestDiagnostic(std::string(kOperation), "temporal_payload_encoding_failed"));
  }
  EngineCreateTemporalPeriodRequest normalized = request;
  normalized.target_object.object_kind = std::string(kTemporalPeriodKind);
  auto result = PersistedRecordResultWithPayload<EngineCreateTemporalPeriodResult>(
      normalized,
      std::string(kOperation),
      std::string(kTemporalPeriodKind),
      true,
      "active",
      false,
      *payload);
  if (result.ok) {
    result.result_shape.result_kind = "rs.bitemporal.periods.v1";
    AddTemporalEvidence(&result, "create_period", "EngineCreateTemporalPeriod");
    AddApiBehaviorEvidence(&result, "audit_event", "catalog.bitemporal_period.added");
  }
  return result;
}

EngineDropTemporalPeriodResult EngineDropTemporalPeriod(
    const EngineDropTemporalPeriodRequest& request) {
  constexpr std::string_view kOperation = "versioned.bitemporal.drop_period";
  const auto base = ValidateBase(request,
                                 kOperation,
                                 true,
                                 "TEMPORAL_HISTORY_ADMIN",
                                 TableUuid(request));
  if (base.error) {
    return DiagnosticResult<EngineDropTemporalPeriodResult>(request,
                                                            std::string(kOperation),
                                                            base);
  }
  const std::string disposition = Lower(OptionValue(request, "history_disposition:"));
  if (disposition != "preserve_history" && disposition != "with_history" &&
      disposition != "destroy_history") {
    return DiagnosticResult<EngineDropTemporalPeriodResult>(
        request, std::string(kOperation),
        MakeEngineApiDiagnostic("SBSQL.TEMPORAL_HISTORY_DISPOSITION_REQUIRED",
                                "sbsql.temporal.history_disposition_required",
                                "drop_period_requires_explicit_history_disposition"));
  }
  const auto payload = TemporalPayload(request, "drop_period", "period_drop");
  if (!payload) {
    return DiagnosticResult<EngineDropTemporalPeriodResult>(request, std::string(kOperation),
        MakeInvalidRequestDiagnostic(std::string(kOperation), "temporal_payload_encoding_failed"));
  }
  EngineDropTemporalPeriodRequest normalized = request;
  normalized.target_object.object_kind = std::string(kTemporalPeriodKind);
  auto result = PersistedRecordResultWithPayload<EngineDropTemporalPeriodResult>(
      normalized,
      std::string(kOperation),
      std::string(kTemporalPeriodKind),
      true,
      "dropped",
      true,
      *payload);
  if (result.ok) {
    result.result_shape.result_kind = "rs.ddl.commit.v1";
    AddTemporalEvidence(&result, "drop_period", "EngineDropTemporalPeriod");
    AddApiBehaviorEvidence(&result, "audit_event", "catalog.bitemporal_period.dropped");
    AddApiBehaviorEvidence(&result, "audit_event", "catalog.bitemporal_period.history_disposition_set");
  }
  return result;
}

EngineShowBitemporalPeriodsResult EngineShowBitemporalPeriods(
    const EngineShowBitemporalPeriodsRequest& request) {
  constexpr std::string_view kOperation = "engine.op.catalog_introspect";
  const auto base = ValidateBase(request,
                                 kOperation,
                                 false,
                                 "TEMPORAL_HISTORY_READ",
                                 TableUuid(request));
  if (base.error) {
    return DiagnosticResult<EngineShowBitemporalPeriodsResult>(request,
                                                               std::string(kOperation),
                                                               base);
  }
  auto result = MakeApiBehaviorSuccess<EngineShowBitemporalPeriodsResult>(
      request.context, std::string(kOperation));
  result.result_shape.result_kind = "rs.bitemporal.periods.v1";
  AddTemporalEvidence(&result, "show_periods", "EngineShowBitemporalPeriods");
  AddApiBehaviorEvidence(&result, "sblr_opcode", "SBLR_CATALOG_INTROSPECT");
  auto behavior_diagnostic = AddTemporalPeriodRows(&result, request, TableUuid(request), {});
  if (behavior_diagnostic.error) return DiagnosticResult<EngineShowBitemporalPeriodsResult>(
      request, std::string(kOperation), behavior_diagnostic);
  result.result_shape.result_kind = "rs.bitemporal.periods.v1";
  return result;
}

EngineShowBitemporalHistoryResult EngineShowBitemporalHistory(
    const EngineShowBitemporalHistoryRequest& request) {
  constexpr std::string_view kOperation =
      "engine.op.bitemporal_for_versions_between";
  const auto base = ValidateBase(request,
                                 kOperation,
                                 false,
                                 "TEMPORAL_HISTORY_READ",
                                 TableUuid(request));
  if (base.error) {
    return DiagnosticResult<EngineShowBitemporalHistoryResult>(request,
                                                               std::string(kOperation),
                                                               base);
  }
  auto result = MakeApiBehaviorSuccess<EngineShowBitemporalHistoryResult>(
      request.context, std::string(kOperation));
  result.result_shape.result_kind = "rs.bitemporal.history.v1";
  AddTemporalEvidence(&result, "show_history", "EngineShowBitemporalHistory");
  AddApiBehaviorEvidence(&result,
                         "sblr_opcode",
                         "SBLR_BITEMPORAL_FOR_VERSIONS_BETWEEN");
  auto behavior_diagnostic = AddTemporalPeriodRows(&result, request, TableUuid(request), PeriodUuid(request));
  if (behavior_diagnostic.error) return DiagnosticResult<EngineShowBitemporalHistoryResult>(
      request, std::string(kOperation), behavior_diagnostic);
  const auto dml_records = VisibleApiBehaviorRecords(request.context,
                                                     std::string(kTemporalDmlEventKind),
                                                     request.context.local_transaction_id, behavior_diagnostic);
  if (behavior_diagnostic.error) return DiagnosticResult<EngineShowBitemporalHistoryResult>(
      request, std::string(kOperation), behavior_diagnostic);
  for (const auto& record : dml_records) {
    BinaryCatalogMetadata fields;
    if (!DecodeTemporalPayload(record.payload, &fields)) {
      return DiagnosticResult<EngineShowBitemporalHistoryResult>(request,
          std::string(kOperation), MakeInvalidRequestDiagnostic(std::string(kOperation),
              "temporal_catalog_payload_invalid"));
    }
    if (!TableUuid(request).is_nil() &&
        PayloadUuid(fields, "table_uuid") != TableUuid(request)) {
      continue;
    }
    AddApiBehaviorRow(&result,
                      {{"temporal_dml_event_uuid", record.object_uuid},
                       {"table_uuid", PayloadUuid(fields, "table_uuid")},
                       {"period_uuid", PayloadUuid(fields, "period_uuid")},
                       {"axis", PayloadField(fields, "axis")},
                       {"system_time_from", PayloadField(fields, "system_time_from")},
                       {"system_time_to", PayloadField(fields, "system_time_to")},
                       {"application_time_from", PayloadField(fields, "application_time_from")},
                       {"application_time_to", PayloadField(fields, "application_time_to")}});
  }
  result.result_shape.result_kind = "rs.bitemporal.history.v1";
  return result;
}

EngineReadBitemporalHistoryResult EngineReadBitemporalHistory(
    const EngineReadBitemporalHistoryRequest& request) {
  constexpr std::string_view kOperation = "versioned.bitemporal.as_of";
  const auto base = ValidateBase(request,
                                 kOperation,
                                 false,
                                 "TEMPORAL_HISTORY_READ",
                                 TableUuid(request));
  if (base.error) {
    return DiagnosticResult<EngineReadBitemporalHistoryResult>(request,
                                                               std::string(kOperation),
                                                               base);
  }
  if (auto repeated = ValidateRepeatedAxes(request, kOperation); repeated.error) {
    return DiagnosticResult<EngineReadBitemporalHistoryResult>(request,
                                                               std::string(kOperation),
                                                               repeated);
  }
  if (auto bounds = ValidateBounds(request, kOperation,
                                   "system_time_from:",
                                   "system_time_to:"); bounds.error) {
    return DiagnosticResult<EngineReadBitemporalHistoryResult>(request,
                                                               std::string(kOperation),
                                                               bounds);
  }
  if (auto bounds = ValidateBounds(request, kOperation,
                                   "application_time_from:",
                                   "application_time_to:"); bounds.error) {
    return DiagnosticResult<EngineReadBitemporalHistoryResult>(request,
                                                               std::string(kOperation),
                                                               bounds);
  }
  auto result = MakeApiBehaviorSuccess<EngineReadBitemporalHistoryResult>(
      request.context, std::string(kOperation));
  result.result_shape.result_kind = "rs.bitemporal.read.v1";
  AddTemporalEvidence(&result, "read_history", "EngineReadBitemporalHistory");
  const auto axes = OptionValues(request, "axis:");
  AddApiBehaviorEvidence(&result,
                         "sblr_opcode",
                         axes.size() >= 2 ? "SBLR_BITEMPORAL_FOR_VERSIONS_BETWEEN"
                                          : "SBLR_BITEMPORAL_AS_OF");
  AddApiBehaviorRow(&result,
                    {{"table_uuid", TableUuid(request)},
                     {"axis_count", std::to_string(axes.size())},
                     {"system_time_from", OptionValue(request, "system_time_from:")},
                     {"system_time_to", OptionValue(request, "system_time_to:")},
                     {"application_time_from", OptionValue(request, "application_time_from:")},
                     {"application_time_to", OptionValue(request, "application_time_to:")},
                     {"system_time_all", OptionBool(request, "system_time_all:") ? "true" : "false"},
                     {"mga_snapshot_visible_through",
                      std::to_string(request.context.snapshot_visible_through_local_transaction_id)},
                     {"temporal_filter_order", "temporal_before_rls"}});
  return result;
}

EngineApplyForPortionOfPeriodResult EngineApplyForPortionOfPeriod(
    const EngineApplyForPortionOfPeriodRequest& request) {
  const std::string dml_action = Lower(OptionValue(request, "dml_action:"));
  if (dml_action != "update" && dml_action != "delete") {
    return DiagnosticResult<EngineApplyForPortionOfPeriodResult>(
        request,
        "engine.op.update",
        MakeEngineApiDiagnostic("SB_ENGINE_API_INVALID_REQUEST",
                                "sbsql.temporal.dml_action_required",
                                "dml_action_update_or_delete_required"));
  }
  const std::string_view kOperation =
      dml_action == "update" ? "engine.op.update" : "engine.op.delete";
  const auto base = ValidateBase(request,
                                 kOperation,
                                 true,
                                 "TEMPORAL_HISTORY_READ",
                                 TableUuid(request));
  if (base.error) {
    return DiagnosticResult<EngineApplyForPortionOfPeriodResult>(request,
                                                                 std::string(kOperation),
                                                                 base);
  }
  if (OptionBool(request, "backdate:") &&
      !HasRight(request, "TEMPORAL_BACKDATE", TableUuid(request))) {
    return DiagnosticResult<EngineApplyForPortionOfPeriodResult>(
        request, std::string(kOperation),
        MakeEngineApiDiagnostic("TEMPORAL.BACKDATE_RIGHT_REQUIRED",
                                "temporal.backdate_right_required",
                                "TEMPORAL_BACKDATE_required"));
  }
  if (auto bounds = ValidateBounds(request, kOperation,
                                   "application_time_from:",
                                   "application_time_to:"); bounds.error) {
    return DiagnosticResult<EngineApplyForPortionOfPeriodResult>(request,
                                                                 std::string(kOperation),
                                                                 bounds);
  }
  const auto payload = TemporalPayload(request, "for_portion_of_period", "application_time");
  if (!payload) {
    return DiagnosticResult<EngineApplyForPortionOfPeriodResult>(request, std::string(kOperation),
        MakeInvalidRequestDiagnostic(std::string(kOperation), "temporal_payload_encoding_failed"));
  }
  EngineApplyForPortionOfPeriodRequest normalized = request;
  normalized.target_object.object_kind = std::string(kTemporalDmlEventKind);
  auto result = PersistedRecordResultWithPayload<EngineApplyForPortionOfPeriodResult>(
      normalized,
      std::string(kOperation),
      std::string(kTemporalDmlEventKind),
      true,
      "applied",
      false,
      *payload);
  if (result.ok) {
    result.result_shape.result_kind = "rs.dml.change.v1";
    result.dml_summary.rows_changed = request.rows.empty() ? 1 : request.rows.size();
    AddTemporalEvidence(&result, "for_portion_of_period", "EngineApplyForPortionOfPeriod");
    AddApiBehaviorEvidence(&result,
                           "sblr_opcode",
                           dml_action == "update" ? "SBLR_UPDATE" : "SBLR_DELETE");
    if (OptionBool(request, "backdate:") ||
        HasRight(request, "TEMPORAL_HISTORY_ADMIN", TableUuid(request))) {
      AddApiBehaviorEvidence(&result, "audit_event", "dml.for_portion_of_period.applied");
    }
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
