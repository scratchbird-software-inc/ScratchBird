// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/agent_observability_api.hpp"

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agents/agent_management_api.hpp"
#include "agent_commercial_evidence.hpp"
#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "metric_contracts.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace agents = scratchbird::core::agents;

constexpr const char* kOperation = "observability.agent_runtime.collect";

bool ContainsPathLikeData(std::string_view value) {
  return value.find('/') != std::string_view::npos ||
         value.find('\\') != std::string_view::npos ||
         value.find("://") != std::string_view::npos;
}

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool ContainsProtectedMaterial(std::string_view value) {
  const std::string lowered = Lower(std::string(value));
  static constexpr const char* kForbidden[] = {
      "password",
      "secret",
      "token",
      "credential",
      "private_key",
      "encryption_key",
      "raw-principal",
      "principal-token"};
  return std::any_of(std::begin(kForbidden), std::end(kForbidden), [&](const char* key) {
    return lowered.find(key) != std::string::npos;
  });
}

bool OptionEnabled(const EngineCollectAgentRuntimeObservabilityRequest& request,
                   const std::string& option) {
  for (const auto& envelope : request.option_envelopes) {
    if (envelope == option || envelope == option + ":true" ||
        envelope == option + "=true") {
      return true;
    }
  }
  return false;
}

std::string OptionValue(const EngineCollectAgentRuntimeObservabilityRequest& request,
                        const std::string& prefix) {
  for (const auto& envelope : request.option_envelopes) {
    if (envelope.rfind(prefix, 0) == 0) {
      return envelope.substr(prefix.size());
    }
  }
  return {};
}

bool OptionBool(const EngineCollectAgentRuntimeObservabilityRequest& request,
                const std::string& prefix,
                bool fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) { return fallback; }
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return fallback;
}

std::string SafeOrRedacted(std::string value) {
  if (value.empty()) { return {}; }
  if (ContainsPathLikeData(value) || ContainsProtectedMaterial(value)) {
    return "<redacted>";
  }
  return value;
}

std::string ResultStateOrDefault(const EngineAgentRuntimeEvidenceRecord& record) {
  return record.result_state.empty() ? "success" : record.result_state;
}

std::string DiagnosticOrDefault(const EngineAgentRuntimeEvidenceRecord& record) {
  return record.diagnostic_code.empty() ? "AGENT.NONE" : record.diagnostic_code;
}

std::string EvidenceKindOrDefault(const EngineAgentRuntimeEvidenceRecord& record) {
  return record.evidence_kind.empty() ? "agent_runtime_evidence" : record.evidence_kind;
}

std::string ActionOrDefault(const EngineAgentRuntimeEvidenceRecord& record) {
  if (!record.action_id.empty()) { return record.action_id; }
  if (!record.evidence_kind.empty()) { return record.evidence_kind; }
  return "observe";
}

bool IsPageAllocationAgent(const std::string& agent_type_id) {
  return agent_type_id == "page_allocation_manager";
}

bool IsFilespaceCapacityAgent(const std::string& agent_type_id) {
  return agent_type_id == "filespace_capacity_manager";
}

EngineApiDiagnostic InvalidCatalogUuidDiagnostic(std::string field_name) {
  return MakeEngineApiDiagnostic("AGENT.OBSERVABILITY.INVALID_CATALOG_UUID",
                                 "agent.observability.invalid_catalog_uuid",
                                 std::move(field_name) + "_must_be_typed_durable_engine_uuid",
                                 true);
}

EngineApiDiagnostic ValidateOptionalEngineUuid(std::string_view value,
                                               platform::UuidKind kind,
                                               std::string field_name) {
  if (value.empty()) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  const auto parsed = uuid::ParseDurableEngineIdentityUuid(kind, std::string(value));
  if (!parsed.ok()) {
    return InvalidCatalogUuidDiagnostic(std::move(field_name));
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ValidateRecord(const EngineAgentRuntimeEvidenceRecord& record) {
  if (record.agent_type_id.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "agent_type_id_required");
  }
  if (record.agent_uuid.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "agent_uuid_required");
  }
  auto diagnostic = ValidateOptionalEngineUuid(record.agent_uuid,
                                               platform::UuidKind::object,
                                               "agent_uuid");
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ValidateOptionalEngineUuid(record.policy_uuid,
                                          platform::UuidKind::object,
                                          "policy_uuid");
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ValidateOptionalEngineUuid(record.evidence_uuid,
                                          platform::UuidKind::object,
                                          "evidence_uuid");
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ValidateOptionalEngineUuid(record.filespace_uuid,
                                          platform::UuidKind::filespace,
                                          "filespace_uuid");
  if (diagnostic.error) { return diagnostic; }
  const std::string result_state = ResultStateOrDefault(record);
  if (!EngineAgentZeroGreyResultStateAllowed(result_state) ||
      EngineAgentZeroGreyResultStateAmbiguous(result_state)) {
    return MakeEngineApiDiagnostic("AGENT.ZERO_GREY.RESULT_STATE_REFUSED",
                                   "agent.zero_grey.result_state_refused",
                                   result_state,
                                   true);
  }
  if (DiagnosticOrDefault(record).empty()) {
    return MakeEngineApiDiagnostic("AGENT.ZERO_GREY.DIAGNOSTIC_REQUIRED",
                                   "agent.zero_grey.diagnostic_required",
                                   record.agent_type_id,
                                   true);
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

metrics::MetricValidationResult RecordMetrics(const EngineAgentRuntimeEvidenceRecord& record) {
  const auto result = ResultStateOrDefault(record);
  const auto action = ActionOrDefault(record);
  auto status = metrics::RecordAgentAction(record.agent_type_id, action, result);
  if (!status.ok) { return status; }
  if (IsFilespaceCapacityAgent(record.agent_type_id) && !record.filespace_uuid.empty()) {
    status = metrics::RecordFilespaceAgentCapacityRequest(record.filespace_uuid, action, result);
    if (!status.ok) { return status; }
  }
  if (IsPageAllocationAgent(record.agent_type_id) && !record.filespace_uuid.empty()) {
    status = metrics::RecordPageAllocationAgentRequest(
        record.filespace_uuid,
        "data",
        action,
        result);
    if (!status.ok) { return status; }
  }
  return metrics::MetricOk();
}

void AddCollectedRow(EngineCollectAgentRuntimeObservabilityResult* result,
                     const EngineAgentRuntimeEvidenceRecord& record) {
  AddApiBehaviorRow(result,
                    {{"source_surface", record.source_surface.empty() ? "engine_api" : record.source_surface},
                     {"agent_type_id", record.agent_type_id},
                     {"agent_uuid", record.agent_uuid},
                     {"filespace_uuid", record.filespace_uuid},
                     {"policy_uuid", record.policy_uuid},
                     {"evidence_uuid", record.evidence_uuid},
                     {"evidence_kind", EvidenceKindOrDefault(record)},
                     {"action_id", ActionOrDefault(record)},
                     {"result_state", ResultStateOrDefault(record)},
                     {"diagnostic_code", DiagnosticOrDefault(record)},
                     {"payload_digest", SafeOrRedacted(record.payload_digest)},
                     {"payload_redacted", record.payload_redacted ? "true" : "false"},
                     {"redaction_class", record.redaction_class.empty() ? "summary" : record.redaction_class},
                     {"physical_path_present", record.physical_path.empty() ? "false" : "true"},
                     {"physical_path", record.physical_path.empty() ? "" : "<redacted>"},
                     {"principal_redacted", record.raw_principal.empty() ? "false" : "true"},
                     {"unsafe_payload", record.unsafe_payload.empty() ? "" : "<redacted>"},
                     {"parser_finality_authority", "false"},
                     {"client_catalog_uuid_authority", "false"},
                     {"support_bundle_safe", "true"}});
}

EngineAgentRuntimeEvidenceRecord RecordFromDurableEvidence(
    const agents::AgentEvidenceRecord& evidence) {
  EngineAgentRuntimeEvidenceRecord record;
  record.source_surface = "durable_agent_catalog_store";
  record.agent_type_id = evidence.agent_type_id;
  record.agent_uuid = evidence.instance_uuid;
  record.evidence_uuid = evidence.evidence_uuid;
  record.action_id = evidence.evidence_kind;
  record.evidence_kind = evidence.evidence_kind;
  record.result_state = evidence.result_state;
  record.diagnostic_code = evidence.diagnostic_code;
  record.payload_digest = evidence.decision_payload_digest;
  record.redaction_class = evidence.redaction_class;
  record.payload_redacted = true;
  return record;
}

EngineAgentRuntimeEvidenceRecord RecordFromDurableAction(
    const agents::DurableAgentActionRecord& action) {
  EngineAgentRuntimeEvidenceRecord record;
  record.source_surface = "durable_agent_catalog_store";
  record.agent_uuid = action.instance_uuid;
  record.evidence_uuid = action.evidence_uuid;
  record.action_id = action.operation_id;
  record.evidence_kind = "agent_durable_action";
  record.result_state =
      action.state == agents::DurableAgentActionState::completed
          ? "success"
          : agents::DurableAgentActionStateName(action.state);
  record.diagnostic_code =
      action.diagnostic_code.empty() ? "AGENT.NONE" : action.diagnostic_code;
  record.payload_digest = action.input_evidence_digest;
  record.payload_redacted = true;
  return record;
}

std::vector<EngineAgentRuntimeEvidenceRecord> DurableCatalogRecords(
    const agents::DurableAgentCatalogImage& image) {
  std::vector<EngineAgentRuntimeEvidenceRecord> records;
  records.reserve(image.evidence.size() + image.actions.size());
  for (const auto& evidence : image.evidence) {
    const auto validation = agents::ValidateCommercialAgentEvidence(evidence);
    if (!validation.status.ok || !validation.tamper_valid) {
      EngineAgentRuntimeEvidenceRecord refused = RecordFromDurableEvidence(evidence);
      refused.result_state = "refused";
      refused.diagnostic_code = "AGENT.OBSERVABILITY.TAMPER_INVALID";
      records.push_back(std::move(refused));
      continue;
    }
    records.push_back(RecordFromDurableEvidence(evidence));
  }
  for (const auto& action : image.actions) {
    auto record = RecordFromDurableAction(action);
    for (const auto& instance : image.instances) {
      if (instance.instance_uuid == action.instance_uuid) {
        record.agent_type_id = instance.agent_type_id;
        break;
      }
    }
    records.push_back(std::move(record));
  }
  return records;
}

}  // namespace

EngineCollectAgentRuntimeObservabilityResult EngineCollectAgentRuntimeObservability(
    const EngineCollectAgentRuntimeObservabilityRequest& request) {
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineCollectAgentRuntimeObservabilityResult>(
        request.context,
        kOperation,
        MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  const bool production_live_path =
      OptionEnabled(request, "agent_observability_production_live") ||
      OptionBool(request, "agent_observability_production_live:", false) ||
      OptionEnabled(request, "agent_durable_catalog_store_required");
  const bool allow_caller_records =
      OptionBool(request, "allow_caller_agent_runtime_evidence:",
                 !production_live_path);
  if (production_live_path && !allow_caller_records && !request.records.empty()) {
    return MakeApiBehaviorDiagnostic<EngineCollectAgentRuntimeObservabilityResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation,
                                     "agent_observability_caller_records_forbidden"));
  }

  std::vector<EngineAgentRuntimeEvidenceRecord> records = request.records;
  std::optional<AgentDurableCatalogStoreResult> loaded_catalog;
  if (production_live_path ||
      OptionBool(request, "agent_durable_catalog_store_load:", false)) {
    auto loaded = LoadAgentDurableCatalogImage(request.context, production_live_path);
    if (!loaded.ok) {
      if (production_live_path) {
        return MakeApiBehaviorDiagnostic<EngineCollectAgentRuntimeObservabilityResult>(
            request.context,
            kOperation,
            MakeEngineApiDiagnostic(
                "AGENT.OBSERVABILITY.DURABLE_CATALOG_REQUIRED",
                "agent.observability.durable_catalog_required",
                loaded.diagnostic.detail.empty() ? loaded.diagnostic.code
                                                  : loaded.diagnostic.detail,
                true));
      }
    } else {
      loaded_catalog = std::move(loaded);
      records = DurableCatalogRecords(loaded_catalog->image);
    }
  }

  if (records.empty()) {
    return MakeApiBehaviorDiagnostic<EngineCollectAgentRuntimeObservabilityResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "agent_runtime_evidence_required"));
  }

  auto result = MakeApiBehaviorSuccess<EngineCollectAgentRuntimeObservabilityResult>(
      request.context,
      kOperation);
  result.result_shape.result_kind = "agent.observability.v1";
  result.metrics_recorded = true;
  result.audit_recorded = true;
  result.diagnostics_rendered = true;
  result.support_bundle_ready = true;
  result.redaction_applied = true;

  for (const auto& record : records) {
    auto diagnostic = ValidateRecord(record);
    if (diagnostic.error) {
      return MakeApiBehaviorDiagnostic<EngineCollectAgentRuntimeObservabilityResult>(
          request.context,
          kOperation,
          std::move(diagnostic));
    }
    const auto metric_status = RecordMetrics(record);
    if (!metric_status.ok) {
      return MakeApiBehaviorDiagnostic<EngineCollectAgentRuntimeObservabilityResult>(
          request.context,
          kOperation,
          MakeEngineApiDiagnostic(metric_status.diagnostic_code,
                                  "agent.observability.metric_recording",
                                  metric_status.detail,
                                  true));
    }
    AddCollectedRow(&result, record);
    AddApiBehaviorEvidence(&result, "agent_observability_metric", record.agent_type_id);
    AddApiBehaviorEvidence(&result, "agent_observability_audit", EvidenceKindOrDefault(record));
    AddApiBehaviorEvidence(&result, "agent_observability_support_bundle", "redacted");
    if (!record.evidence_uuid.empty()) {
      AddApiBehaviorEvidence(&result, "agent_evidence_uuid", record.evidence_uuid);
    }
    result.diagnostics.push_back(MakeEngineApiDiagnostic(DiagnosticOrDefault(record),
                                                        "agent.observability.diagnostic",
                                                        ResultStateOrDefault(record),
                                                        false));
  }

  AddApiBehaviorEvidence(&result, "parser_client_surface", "diagnostic_rendering_required");
  AddApiBehaviorEvidence(&result, "listener_surface", "agent_runtime_evidence_counter");
  AddApiBehaviorEvidence(&result, "manager_surface", "support_bundle_agent_observability");
  if (loaded_catalog.has_value()) {
    AddApiBehaviorEvidence(&result,
                           "agent_observability_durable_catalog",
                           loaded_catalog->image.authority.catalog_root_digest);
    AddApiBehaviorEvidence(&result,
                           "agent_observability_tamper_chain",
                           loaded_catalog->image.evidence.empty()
                               ? "empty"
                               : "validated");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
