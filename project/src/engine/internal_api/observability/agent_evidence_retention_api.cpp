// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/agent_evidence_retention_api.hpp"

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agents/agent_management_api.hpp"
#include "agent_commercial_evidence.hpp"
#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace agents = scratchbird::core::agents;

constexpr const char* kOperation = "observability.agent_evidence.retention";

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool ContainsPathLikeData(std::string_view value) {
  return value.find('/') != std::string_view::npos ||
         value.find('\\') != std::string_view::npos ||
         value.find("://") != std::string_view::npos;
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
      "principal-token",
      "protected_payload"};
  return std::any_of(std::begin(kForbidden), std::end(kForbidden), [&](const char* key) {
    return lowered.find(key) != std::string::npos;
  });
}

bool OptionEnabled(const EngineEvaluateAgentEvidenceRetentionRequest& request,
                   const std::string& option) {
  for (const auto& envelope : request.option_envelopes) {
    if (envelope == option || envelope == option + ":true" ||
        envelope == option + "=true") {
      return true;
    }
  }
  return false;
}

std::string OptionValue(const EngineEvaluateAgentEvidenceRetentionRequest& request,
                        const std::string& prefix) {
  for (const auto& envelope : request.option_envelopes) {
    if (envelope.rfind(prefix, 0) == 0) {
      return envelope.substr(prefix.size());
    }
  }
  return {};
}

bool OptionBool(const EngineEvaluateAgentEvidenceRetentionRequest& request,
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

bool ProductionRetentionPath(const EngineEvaluateAgentEvidenceRetentionRequest& request) {
  return OptionEnabled(request, "agent_evidence_retention_production_live") ||
         OptionBool(request, "agent_evidence_retention_production_live:", false) ||
         OptionEnabled(request, "agent_durable_catalog_store_required");
}

bool HasTraceRight(const EngineRequestContext& context, std::string_view right) {
  const std::string token = "right:" + std::string(right);
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), token) !=
         context.trace_tags.end();
}

std::string SafeOrRedacted(std::string value) {
  if (value.empty()) { return {}; }
  if (ContainsPathLikeData(value) || ContainsProtectedMaterial(value)) {
    return "<redacted>";
  }
  return value;
}

EngineApiDiagnostic EvidenceDiagnostic(std::string code,
                                       std::string message_key,
                                       std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(message_key), std::move(detail), true);
}

EngineApiDiagnostic InvalidCatalogUuidDiagnostic(std::string field_name) {
  return EvidenceDiagnostic("AGENT.EVIDENCE.INVALID_CATALOG_UUID",
                            "agent.evidence.invalid_catalog_uuid",
                            std::move(field_name) + "_must_be_typed_durable_engine_uuid");
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

EngineApiDiagnostic ValidateRecord(const EngineAgentEvidenceAuditRetentionRecord& record) {
  if (record.agent_type_id.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "agent_type_id_required");
  }
  if (record.agent_uuid.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "agent_uuid_required");
  }
  if (record.evidence_uuid.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "evidence_uuid_required");
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
  diagnostic = ValidateOptionalEngineUuid(record.action_uuid,
                                          platform::UuidKind::object,
                                          "action_uuid");
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ValidateOptionalEngineUuid(record.actor_uuid,
                                          platform::UuidKind::principal,
                                          "actor_uuid");
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ValidateOptionalEngineUuid(record.filespace_uuid,
                                          platform::UuidKind::filespace,
                                          "filespace_uuid");
  if (diagnostic.error) { return diagnostic; }
  if (record.retention_class.empty()) {
    return EvidenceDiagnostic("AGENT.EVIDENCE.RETENTION_POLICY_REQUIRED",
                              "agent.evidence.retention_policy_required",
                              "retention_class_required");
  }
  if (record.retention_policy_ref.empty() || !record.retention_policy_installed) {
    return EvidenceDiagnostic("AGENT.EVIDENCE.RETENTION_POLICY_REQUIRED",
                              "agent.evidence.retention_policy_required",
                              "retention_policy_ref_required");
  }
  if (!record.retention_policy_valid) {
    return EvidenceDiagnostic("AGENT.EVIDENCE.RETENTION_POLICY_INVALID",
                              "agent.evidence.retention_policy_invalid",
                              "retention_policy_invalid");
  }
  if (record.retention_deadline.empty()) {
    return EvidenceDiagnostic("AGENT.EVIDENCE.RETENTION_POLICY_REQUIRED",
                              "agent.evidence.retention_policy_required",
                              "retention_deadline_required");
  }
  const std::string result_state = record.result_state.empty() ? "success" : record.result_state;
  if (!EngineAgentZeroGreyResultStateAllowed(result_state) ||
      EngineAgentZeroGreyResultStateAmbiguous(result_state)) {
    return EvidenceDiagnostic("AGENT.ZERO_GREY.RESULT_STATE_REFUSED",
                              "agent.zero_grey.result_state_refused",
                              result_state);
  }
  if ((record.diagnostic_code.empty() && result_state != "success") ||
      (!record.diagnostic_code.empty() &&
       record.diagnostic_code.find("implementation-defined") != std::string::npos)) {
    return EvidenceDiagnostic("AGENT.ZERO_GREY.DIAGNOSTIC_REQUIRED",
                              "agent.zero_grey.diagnostic_required",
                              record.agent_type_id);
  }
  if (result_state == "success" && !record.evidence_write_available && !record.evidence_recoverable) {
    return EvidenceDiagnostic("AGENT.EVIDENCE.BEFORE_SUCCESS_REQUIRED",
                              "agent.evidence.before_success_required",
                              "success_requires_written_retained_evidence");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool HasClusterScopedRecord(const EngineEvaluateAgentEvidenceRetentionRequest& request) {
  return std::any_of(request.records.begin(), request.records.end(), [](const auto& record) {
    return record.cluster_scoped;
  });
}

EngineEvaluateAgentEvidenceRetentionResult ClusterProviderRoute(
    const EngineEvaluateAgentEvidenceRetentionRequest& request) {
  cluster_provider::ClusterProviderRequest cluster_request;
  cluster_request.context = request.context;
  cluster_request.envelope.operation_id = kOperation;
  cluster_request.envelope.opcode = "SBLR_AGENT_EVIDENCE_RETENTION";
  cluster_request.envelope.trace_key = "agent-evidence-retention-cluster-provider-boundary";
  cluster_request.envelope.requires_security_context = true;
  cluster_request.envelope.requires_cluster_authority = true;
  cluster_request.envelope.contains_sql_text = false;
  cluster_request.api_request = request;
  cluster_request.api_request.operation_id = kOperation;

  EngineEvaluateAgentEvidenceRetentionResult result;
  static_cast<EngineApiResult&>(result) =
      cluster_provider::ExecuteClusterOperation(cluster_request);
  AddApiBehaviorEvidence(&result, "agent_evidence_cluster_route", "provider_boundary");
  return result;
}

bool PrivilegedView(const EngineEvaluateAgentEvidenceRetentionRequest& request) {
  if (ProductionRetentionPath(request)) {
    return HasTraceRight(request.context, "OBS_AGENT_EVIDENCE_READ") ||
           HasTraceRight(request.context, "OBS_AGENT_AUDIT_READ");
  }
  return request.admin_view || request.sysarch_view;
}

std::string ResultStateForRow(const EngineAgentEvidenceAuditRetentionRecord& record) {
  const std::string result_state = record.result_state.empty() ? "success" : record.result_state;
  if (result_state == "success" && !record.evidence_write_available && record.evidence_recoverable) {
    return "pending_evidence";
  }
  return result_state;
}

std::string DiagnosticForRow(const EngineAgentEvidenceAuditRetentionRecord& record) {
  if (ResultStateForRow(record) == "pending_evidence") {
    return "AGENT.EVIDENCE.PENDING_EVIDENCE";
  }
  return record.diagnostic_code.empty() ? "AGENT.NONE" : record.diagnostic_code;
}

std::string EvidenceKindForRow(const EngineAgentEvidenceAuditRetentionRecord& record) {
  return record.evidence_kind.empty() ? "agent_action_evidence" : record.evidence_kind;
}

std::string HoldState(const EngineAgentEvidenceAuditRetentionRecord& record) {
  if (record.legal_hold) { return "legal_hold"; }
  if (record.maintenance_hold) { return "maintenance_hold"; }
  return "none";
}

std::string PurgeEligibility(const EngineAgentEvidenceAuditRetentionRecord& record) {
  if (record.legal_hold || record.maintenance_hold) { return "held"; }
  if (record.retention_deadline_expired) { return "eligible"; }
  return "retained";
}

std::string VisibleActor(const EngineEvaluateAgentEvidenceRetentionRequest& request,
                         const EngineAgentEvidenceAuditRetentionRecord& record) {
  if (!PrivilegedView(request) || !record.actor_visible || record.actor_uuid.empty()) {
    return record.actor_uuid.empty() ? "" : "<redacted:actor_uuid>";
  }
  return record.actor_uuid;
}

std::string VisibleRestrictedText(bool visible, std::string value, std::string redacted_marker) {
  if (value.empty()) { return {}; }
  if (!visible) { return redacted_marker; }
  return SafeOrRedacted(std::move(value));
}

void AddRetentionRow(EngineEvaluateAgentEvidenceRetentionResult* result,
                     const EngineEvaluateAgentEvidenceRetentionRequest& request,
                     const EngineAgentEvidenceAuditRetentionRecord& record) {
  const std::string purge = PurgeEligibility(record);
  AddApiBehaviorRow(result,
                    {{"source_surface", record.source_surface.empty() ? "engine_api" : record.source_surface},
                     {"agent_type_id", record.agent_type_id},
                     {"agent_uuid", record.agent_uuid},
                     {"filespace_uuid", record.filespace_uuid},
                     {"policy_uuid", record.policy_uuid},
                     {"evidence_uuid", record.evidence_uuid},
                     {"action_uuid", record.action_uuid},
                     {"actor_uuid", VisibleActor(request, record)},
                     {"evidence_kind", EvidenceKindForRow(record)},
                     {"result_state", ResultStateForRow(record)},
                     {"diagnostic_code", DiagnosticForRow(record)},
                     {"retention_class", record.retention_class},
                     {"retention_policy_ref", record.retention_policy_ref},
                     {"retention_deadline", record.retention_deadline},
                     {"policy_generation", record.policy_generation},
                     {"hold_state", HoldState(record)},
                     {"legal_hold", record.legal_hold ? "true" : "false"},
                     {"maintenance_hold", record.maintenance_hold ? "true" : "false"},
                     {"purge_eligibility", purge},
                     {"reason_text", VisibleRestrictedText(PrivilegedView(request),
                                                           record.reason_text,
                                                           "<redacted:reason_text>")},
                     {"policy_body", VisibleRestrictedText(PrivilegedView(request) && record.policy_body_visible,
                                                           record.policy_body,
                                                           "<redacted:policy_body>")},
                     {"physical_path_present", record.physical_path.empty() ? "false" : "true"},
                     {"physical_path", record.physical_path.empty() ? "" : "<redacted>"},
                     {"raw_principal", record.raw_principal.empty() ? "" : "<redacted:principal>"},
                     {"raw_evidence_body", record.raw_evidence_body.empty() ? "" : "<redacted:evidence_body>"},
                     {"support_bundle_payload", record.support_bundle_payload.empty() ? "" : "<redacted:support_bundle>"},
                     {"evidence_write_state", record.evidence_write_available ? "written" : "not_written"},
                     {"evidence_before_success_enforced", "true"},
                     {"parser_finality_authority", "false"},
                     {"client_catalog_uuid_authority", "false"}});
  if (purge == "eligible") { result->purge_eligible = true; }
  if (ResultStateForRow(record) == "pending_evidence") { result->pending_evidence = true; }
}

EngineAgentEvidenceAuditRetentionRecord RecordFromDurableEvidence(
    const agents::AgentEvidenceRecord& evidence) {
  EngineAgentEvidenceAuditRetentionRecord record;
  record.source_surface = "durable_agent_catalog_store";
  record.agent_type_id = evidence.agent_type_id;
  record.agent_uuid = evidence.instance_uuid;
  record.evidence_uuid = evidence.evidence_uuid;
  record.actor_uuid = evidence.principal_uuid;
  record.evidence_kind = evidence.evidence_kind.empty()
                             ? "agent_action_evidence"
                             : evidence.evidence_kind;
  record.result_state = evidence.result_state.empty() ? "success"
                                                      : evidence.result_state;
  record.diagnostic_code = evidence.diagnostic_code.empty()
                               ? "AGENT.NONE"
                               : evidence.diagnostic_code;
  record.retention_class = evidence.retention_class.empty()
                               ? "agent_evidence_400_day"
                               : evidence.retention_class;
  record.retention_policy_ref = "agent.evidence.default_retention.v1";
  record.retention_deadline = std::to_string(evidence.expires_at_microseconds);
  record.policy_generation = std::to_string(evidence.policy_generation);
  record.evidence_write_available = true;
  record.retention_policy_installed = true;
  record.retention_policy_valid = true;
  record.legal_hold = evidence.retention_class == "legal_hold";
  record.maintenance_hold = false;
  record.retention_deadline_expired = false;
  record.actor_visible = true;
  record.policy_body_visible = false;
  return record;
}

std::vector<EngineAgentEvidenceAuditRetentionRecord> DurableRetentionRecords(
    const agents::DurableAgentCatalogImage& image,
    std::optional<EngineApiDiagnostic>* diagnostic) {
  std::vector<EngineAgentEvidenceAuditRetentionRecord> records;
  records.reserve(image.evidence.size());
  for (const auto& evidence : image.evidence) {
    const auto validation = agents::ValidateCommercialAgentEvidence(evidence);
    if (!validation.status.ok || !validation.tamper_valid ||
        !validation.authority_clean) {
      if (diagnostic != nullptr) {
        *diagnostic = EvidenceDiagnostic(
            "AGENT.EVIDENCE.TAMPER_INVALID",
            "agent.evidence.tamper_invalid",
            evidence.evidence_uuid.empty() ? evidence.agent_type_id
                                           : evidence.evidence_uuid);
      }
      return {};
    }
    records.push_back(RecordFromDurableEvidence(evidence));
  }
  return records;
}

}  // namespace

EngineEvaluateAgentEvidenceRetentionResult EngineEvaluateAgentEvidenceRetention(
    const EngineEvaluateAgentEvidenceRetentionRequest& request) {
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineEvaluateAgentEvidenceRetentionResult>(
        request.context,
        kOperation,
        MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  const bool production_live_path = ProductionRetentionPath(request);
  const bool allow_caller_records =
      OptionBool(request, "allow_caller_agent_evidence_retention_records:",
                 !production_live_path);
  if (production_live_path && !allow_caller_records && !request.records.empty()) {
    return MakeApiBehaviorDiagnostic<EngineEvaluateAgentEvidenceRetentionResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(
            kOperation,
            "agent_evidence_retention_caller_records_forbidden"));
  }

  std::vector<EngineAgentEvidenceAuditRetentionRecord> records = request.records;
  std::optional<AgentDurableCatalogStoreResult> loaded_catalog;
  if (production_live_path ||
      OptionBool(request, "agent_durable_catalog_store_load:", false)) {
    auto loaded = LoadAgentDurableCatalogImage(request.context, production_live_path);
    if (!loaded.ok) {
      if (production_live_path) {
        return MakeApiBehaviorDiagnostic<EngineEvaluateAgentEvidenceRetentionResult>(
            request.context,
            kOperation,
            MakeEngineApiDiagnostic(
                "AGENT.EVIDENCE.DURABLE_CATALOG_REQUIRED",
                "agent.evidence.durable_catalog_required",
                loaded.diagnostic.detail.empty() ? loaded.diagnostic.code
                                                  : loaded.diagnostic.detail,
                true));
      }
    } else {
      loaded_catalog = std::move(loaded);
      std::optional<EngineApiDiagnostic> tamper_diagnostic;
      records = DurableRetentionRecords(loaded_catalog->image, &tamper_diagnostic);
      if (tamper_diagnostic.has_value()) {
        return MakeApiBehaviorDiagnostic<EngineEvaluateAgentEvidenceRetentionResult>(
            request.context,
            kOperation,
            std::move(*tamper_diagnostic));
      }
    }
  }

  if (records.empty()) {
    return MakeApiBehaviorDiagnostic<EngineEvaluateAgentEvidenceRetentionResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "agent_evidence_required"));
  }
  EngineEvaluateAgentEvidenceRetentionRequest effective_request = request;
  effective_request.records = records;

  if (HasClusterScopedRecord(effective_request)) {
    return ClusterProviderRoute(request);
  }

  for (const auto& record : effective_request.records) {
    auto diagnostic = ValidateRecord(record);
    if (diagnostic.error) {
      return MakeApiBehaviorDiagnostic<EngineEvaluateAgentEvidenceRetentionResult>(
          request.context,
          kOperation,
          std::move(diagnostic));
    }
  }

  auto result = MakeApiBehaviorSuccess<EngineEvaluateAgentEvidenceRetentionResult>(
      request.context,
      kOperation);
  result.result_shape.result_kind = "agent.evidence.retention.v1";
  result.evidence_before_success_enforced = true;
  result.retention_decision_recorded = true;
  result.redaction_applied = true;
  for (const auto& record : effective_request.records) {
    AddRetentionRow(&result, effective_request, record);
    AddApiBehaviorEvidence(&result, "agent_evidence_retention_decision", record.evidence_uuid);
    AddApiBehaviorEvidence(&result, "agent_audit_redaction", EvidenceKindForRow(record));
    AddApiBehaviorEvidence(&result, "support_bundle_redaction", "agent_evidence_safe");
    AddApiBehaviorEvidence(&result, "retention_policy_ref", record.retention_policy_ref);
    result.diagnostics.push_back(MakeEngineApiDiagnostic(DiagnosticForRow(record),
                                                        "agent.evidence.retention.diagnostic",
                                                        ResultStateForRow(record),
                                                        false));
  }
  if (loaded_catalog.has_value()) {
    AddApiBehaviorEvidence(&result,
                           "agent_evidence_retention_durable_catalog",
                           loaded_catalog->image.authority.catalog_root_digest);
    AddApiBehaviorEvidence(&result,
                           "agent_evidence_retention_tamper_chain",
                           loaded_catalog->image.evidence.empty()
                               ? "empty"
                               : "validated");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
