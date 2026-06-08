// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/support_bundle_api.hpp"

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agents/agent_management_api.hpp"
#include "agent_commercial_evidence.hpp"
#include "agent_durable_catalog.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include "transaction_recovery.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <sstream>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace agents = scratchbird::core::agents;

bool OptionEnabled(const EnginePrepareSupportBundleRequest& request, const std::string& option) {
  for (const auto& envelope : request.option_envelopes) {
    if (envelope == option || envelope == option + ":true" || envelope == option + "=true") {
      return true;
    }
  }
  return false;
}

std::string OptionValue(const EnginePrepareSupportBundleRequest& request, const std::string& prefix) {
  for (const auto& envelope : request.option_envelopes) {
    if (envelope.rfind(prefix, 0) == 0) {
      return envelope.substr(prefix.size());
    }
  }
  return {};
}

bool OptionBool(const EnginePrepareSupportBundleRequest& request,
                const std::string& prefix,
                bool fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) return fallback;
  if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
  if (value == "0" || value == "false" || value == "no" || value == "off") return false;
  return fallback;
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
      "raw-principal"};
  return std::any_of(std::begin(kForbidden), std::end(kForbidden), [&](const char* key) {
    return lowered.find(key) != std::string::npos;
  });
}

std::string RedactedPayloadValue(std::string value) {
  if (value.empty()) { return {}; }
  if (value.find('/') != std::string::npos ||
      value.find('\\') != std::string::npos ||
      ContainsProtectedMaterial(value)) {
    return "<redacted>";
  }
  return value;
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

EngineApiDiagnostic ValidateEvidenceUuids(
    const EngineSupportBundleAgentEvidenceSource& evidence) {
  auto diagnostic = ValidateOptionalEngineUuid(evidence.agent_uuid,
                                               platform::UuidKind::object,
                                               "agent_uuid");
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ValidateOptionalEngineUuid(evidence.policy_uuid,
                                          platform::UuidKind::object,
                                          "policy_uuid");
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ValidateOptionalEngineUuid(evidence.evidence_uuid,
                                          platform::UuidKind::object,
                                          "evidence_uuid");
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ValidateOptionalEngineUuid(evidence.filespace_uuid,
                                          platform::UuidKind::filespace,
                                          "filespace_uuid");
  if (diagnostic.error) { return diagnostic; }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string SupportBundleResultStateOrDefault(
    const EngineSupportBundleAgentEvidenceSource& evidence) {
  return evidence.result_state.empty() ? "success" : evidence.result_state;
}

std::string SupportBundleDiagnosticOrDefault(
    const EngineSupportBundleAgentEvidenceSource& evidence) {
  return evidence.diagnostic_code.empty() ? "AGENT.NONE" : evidence.diagnostic_code;
}

bool ContainsVagueDiagnosticText(std::string_view value) {
  const std::string lowered = Lower(std::string(value));
  return lowered.find("implementation-defined") != std::string::npos ||
         lowered.find("implementation defined") != std::string::npos ||
         lowered.find("best_effort") != std::string::npos ||
         lowered.find("best effort") != std::string::npos ||
         lowered.find("partial") != std::string::npos ||
         lowered.find("tbd") != std::string::npos;
}

EngineApiDiagnostic ValidateSupportBundleZeroGrey(
    const EngineSupportBundleAgentEvidenceSource& evidence) {
  const auto result_state = SupportBundleResultStateOrDefault(evidence);
  if (!EngineAgentZeroGreyResultStateAllowed(result_state) ||
      EngineAgentZeroGreyResultStateAmbiguous(result_state)) {
    return MakeEngineApiDiagnostic("AGENT.ZERO_GREY.RESULT_STATE_REFUSED",
                                   "agent.zero_grey.result_state_refused",
                                   result_state,
                                   true);
  }
  if ((evidence.diagnostic_code.empty() && result_state != "success") ||
      ContainsVagueDiagnosticText(evidence.diagnostic_code)) {
    return MakeEngineApiDiagnostic("AGENT.ZERO_GREY.DIAGNOSTIC_REQUIRED",
                                   "agent.zero_grey.diagnostic_required",
                                   "exact_diagnostic_code_required",
                                   true);
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool OptionEnabledAny(const EnginePrepareSupportBundleRequest& request,
                      std::initializer_list<std::string_view> options) {
  for (const auto option : options) {
    if (OptionEnabled(request, std::string(option))) {
      return true;
    }
  }
  return false;
}

struct TransactionBundleInventoryStats {
  platform::u64 entry_count = 0;
  platform::u64 active_count = 0;
  platform::u64 prepared_count = 0;
  platform::u64 limbo_count = 0;
  platform::u64 recovery_required_count = 0;
  platform::u64 recovery_fail_closed_count = 0;
  platform::u64 committed_count = 0;
  platform::u64 rolled_back_count = 0;
  bool evidence_complete = true;
};

std::string RedactedUuidIfPresent(const std::string& value) {
  return value.empty() ? "" : "<redacted>";
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream out;
  for (const char ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00"
              << kHex[(static_cast<unsigned char>(ch) >> 4) & 0x0f]
              << kHex[static_cast<unsigned char>(ch) & 0x0f];
        } else {
          out << ch;
        }
        break;
    }
  }
  return out.str();
}

const char* TransactionSupportBundleSchemaId() {
  return "scratchbird.transaction_support_bundle.v1";
}

bool TransactionActionRequiresRecovery(
    scratchbird::transaction::mga::TransactionRecoveryAction action) {
  using scratchbird::transaction::mga::TransactionRecoveryAction;
  return action == TransactionRecoveryAction::complete_commit ||
         action == TransactionRecoveryAction::complete_rollback ||
         action == TransactionRecoveryAction::fail_closed_ambiguous;
}

TransactionBundleInventoryStats SummarizeTransactionInventory(
    const scratchbird::transaction::mga::LocalTransactionInventory& inventory) {
  namespace mga = scratchbird::transaction::mga;
  TransactionBundleInventoryStats stats;
  stats.entry_count = inventory.entries.size();
  for (const auto& entry : inventory.entries) {
    switch (entry.state) {
      case mga::TransactionState::active:
      case mga::TransactionState::read_only_active:
        ++stats.active_count;
        break;
      case mga::TransactionState::prepared:
        ++stats.prepared_count;
        break;
      case mga::TransactionState::limbo:
        ++stats.limbo_count;
        break;
      case mga::TransactionState::committed:
        ++stats.committed_count;
        break;
      case mga::TransactionState::rolled_back:
        ++stats.rolled_back_count;
        break;
      default:
        break;
    }
    if (entry.evidence_record_required && !entry.evidence_record_written) {
      stats.evidence_complete = false;
    }
    const auto classification = mga::ClassifyLocalTransactionForRecovery(entry);
    if (TransactionActionRequiresRecovery(classification.action)) {
      ++stats.recovery_required_count;
    }
    if (classification.fail_closed) {
      ++stats.recovery_fail_closed_count;
    }
  }
  return stats;
}

EngineApiDiagnostic ValidateTransactionEvidenceSnapshot(
    const EngineSupportBundleTransactionEvidenceSnapshot& snapshot) {
  constexpr const char* kOperation = "management.prepare_support_bundle";
  if (!snapshot.inventory_present || !snapshot.inventory_authoritative) {
    return MakeInvalidRequestDiagnostic(
        kOperation,
        "OPS.SUPPORT_BUNDLE.TRANSACTION_INVENTORY_AUTHORITY_REQUIRED");
  }
  if (!snapshot.horizons_present || !snapshot.horizons_authoritative ||
      !snapshot.horizons.valid) {
    return MakeInvalidRequestDiagnostic(
        kOperation,
        "OPS.SUPPORT_BUNDLE.TRANSACTION_HORIZON_AUTHORITY_REQUIRED");
  }
  if (snapshot.support_bundle_is_authority || snapshot.parser_finality_authority ||
      snapshot.client_finality_authority || snapshot.donor_finality_authority ||
      snapshot.timestamp_finality_authority ||
      snapshot.uuid_ordering_finality_authority ||
      snapshot.event_stream_finality_authority ||
      snapshot.wal_recovery_authority) {
    return MakeInvalidRequestDiagnostic(
        kOperation,
        "OPS.SUPPORT_BUNDLE.TRANSACTION_AUTHORITY_CLAIM_REFUSED");
  }
  if (snapshot.current_row_decision_present &&
      (snapshot.current_row_decision.map_is_transaction_finality_authority ||
       !snapshot.current_row_decision.durable_mga_inventory_remains_authority)) {
    return MakeInvalidRequestDiagnostic(
        kOperation,
        "OPS.SUPPORT_BUNDLE.CURRENT_ROW_AUTHORITY_CLAIM_REFUSED");
  }
  if (snapshot.page_finality_decision_present &&
      (snapshot.page_finality_decision.map_is_transaction_finality_authority ||
       !snapshot.page_finality_decision.durable_mga_inventory_remains_authority)) {
    return MakeInvalidRequestDiagnostic(
        kOperation,
        "OPS.SUPPORT_BUNDLE.PAGE_FINALITY_AUTHORITY_CLAIM_REFUSED");
  }
  if (snapshot.cleanup_result_present &&
      snapshot.cleanup_result.physical_storage_mutated) {
    return MakeInvalidRequestDiagnostic(
        kOperation,
        "OPS.SUPPORT_BUNDLE.TRANSACTION_CLEANUP_MUTATION_REFUSED");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string RenderTransactionSupportBundleJson(
    const EngineSupportBundleTransactionEvidenceSnapshot& snapshot,
    const TransactionBundleInventoryStats& stats) {
  namespace mga = scratchbird::transaction::mga;
  std::ostringstream out;
  out << "{\"support_bundle\":{\"section\":\"transaction_evidence\","
      << "\"schema_id\":\"" << TransactionSupportBundleSchemaId() << "\","
      << "\"redaction_state\":\"public_safe_summary\","
      << "\"forbidden_fields_absent\":true,"
      << "\"support_bundle_is_authority\":false,"
      << "\"authority_source\":\"durable_mga_transaction_inventory\","
      << "\"inventory_entry_count\":" << stats.entry_count << ','
      << "\"active_transaction_count\":" << stats.active_count << ','
      << "\"prepared_transaction_count\":" << stats.prepared_count << ','
      << "\"limbo_transaction_count\":" << stats.limbo_count << ','
      << "\"recovery_required_transaction_count\":"
      << stats.recovery_required_count << ','
      << "\"oldest_interesting_transaction_id\":"
      << snapshot.horizons.oldest_interesting_transaction.value << ','
      << "\"oldest_active_transaction_id\":"
      << snapshot.horizons.oldest_active_transaction.value << ','
      << "\"oldest_snapshot_transaction_id\":"
      << snapshot.horizons.oldest_snapshot_transaction.value << ','
      << "\"next_transaction_id\":"
      << snapshot.horizons.next_transaction_id.value << ','
      << "\"current_row_refusal_reason\":\""
      << JsonEscape(snapshot.current_row_decision_present
                        ? snapshot.current_row_decision.refusal_reason
                        : "not_reported")
      << "\","
      << "\"page_finality_refusal_reason\":\""
      << JsonEscape(snapshot.page_finality_decision_present
                        ? snapshot.page_finality_decision.refusal_reason
                        : "not_reported")
      << "\","
      << "\"cleanup_reclaim_evidence_count\":"
      << (snapshot.cleanup_result_present
              ? snapshot.cleanup_result.reclaim_evidence_records.size()
              : 0)
      << ','
      << "\"cleanup_physical_storage_mutated\":false,"
      << "\"lock_decision\":\""
      << JsonEscape(snapshot.lock_result_present
                        ? mga::TransactionLockDecisionName(
                              snapshot.lock_result.decision)
                        : "not_reported")
      << "\","
      << "\"savepoint_decision\":\""
      << JsonEscape(snapshot.savepoint_plan_present
                        ? mga::SavepointRollbackDecisionName(
                              snapshot.savepoint_plan.decision)
                        : "not_reported")
      << "\","
      << "\"parser_finality_authority\":false,"
      << "\"client_finality_authority\":false,"
      << "\"donor_finality_authority\":false,"
      << "\"timestamp_finality_authority\":false,"
      << "\"uuid_ordering_finality_authority\":false,"
      << "\"event_stream_finality_authority\":false,"
      << "\"wal_recovery_authority\":false}}";
  return out.str();
}

void AddTransactionSupportBundleRows(
    EnginePrepareSupportBundleResult* result,
    const EngineSupportBundleTransactionEvidenceSnapshot& snapshot) {
  namespace mga = scratchbird::transaction::mga;
  const auto stats = SummarizeTransactionInventory(snapshot.inventory);
  AddApiBehaviorRow(
      result,
      {{"bundle_record_kind", "transaction_support_bundle_summary"},
       {"schema_id", TransactionSupportBundleSchemaId()},
       {"schema_version", "1"},
       {"authority_source", "durable_mga_transaction_inventory"},
       {"support_bundle_is_authority", "false"},
       {"inventory_authoritative", snapshot.inventory_authoritative ? "true" : "false"},
       {"inventory_entry_count", std::to_string(stats.entry_count)},
       {"inventory_next_local_transaction_id",
        std::to_string(snapshot.inventory.next_local_transaction_id)},
       {"active_transaction_count", std::to_string(stats.active_count)},
       {"prepared_transaction_count", std::to_string(stats.prepared_count)},
       {"limbo_transaction_count", std::to_string(stats.limbo_count)},
       {"recovery_required_transaction_count",
        std::to_string(stats.recovery_required_count)},
       {"recovery_fail_closed_count",
        std::to_string(stats.recovery_fail_closed_count)},
       {"committed_transaction_count", std::to_string(stats.committed_count)},
       {"rolled_back_transaction_count", std::to_string(stats.rolled_back_count)},
       {"transaction_evidence_complete", stats.evidence_complete ? "true" : "false"},
       {"oldest_interesting_transaction_id",
        std::to_string(snapshot.horizons.oldest_interesting_transaction.value)},
       {"oldest_active_transaction_id",
        std::to_string(snapshot.horizons.oldest_active_transaction.value)},
       {"oldest_snapshot_transaction_id",
        std::to_string(snapshot.horizons.oldest_snapshot_transaction.value)},
       {"next_transaction_id",
        std::to_string(snapshot.horizons.next_transaction_id.value)},
       {"horizons_authoritative", snapshot.horizons_authoritative ? "true" : "false"},
       {"parser_finality_authority", "false"},
       {"client_finality_authority", "false"},
       {"donor_finality_authority", "false"},
       {"timestamp_finality_authority", "false"},
       {"uuid_ordering_finality_authority", "false"},
       {"event_stream_finality_authority", "false"},
       {"wal_recovery_authority", "false"}});

  for (const auto& entry : snapshot.inventory.entries) {
    const auto classification = mga::ClassifyLocalTransactionForRecovery(entry);
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "transaction_inventory_entry"},
         {"local_transaction_id", std::to_string(entry.identity.local_id.value)},
         {"transaction_uuid", entry.identity.transaction_uuid.valid()
                                  ? uuid::UuidToString(
                                        entry.identity.transaction_uuid.value)
                                  : ""},
         {"transaction_scope", mga::TransactionScopeName(entry.identity.scope)},
         {"transaction_state", mga::TransactionStateName(entry.state)},
         {"recovery_action",
          mga::TransactionRecoveryActionName(classification.action)},
         {"recovery_required",
          TransactionActionRequiresRecovery(classification.action) ? "true" : "false"},
         {"fail_closed", classification.fail_closed ? "true" : "false"},
         {"rollback_only", entry.rollback_only ? "true" : "false"},
         {"evidence_record_required",
          entry.evidence_record_required ? "true" : "false"},
         {"evidence_record_written",
          entry.evidence_record_written ? "true" : "false"},
         {"begin_visible_through_local_transaction_id",
          std::to_string(entry.begin_visible_through_local_transaction_id)}});
  }

  if (snapshot.current_row_decision_present) {
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "transaction_current_row_refusal"},
         {"accepted", snapshot.current_row_decision.accepted ? "true" : "false"},
         {"refusal_reason", snapshot.current_row_decision.refusal_reason},
         {"normal_mga_recheck_required",
          snapshot.current_row_decision.normal_mga_recheck_required ? "true"
                                                                    : "false"},
         {"security_recheck_required",
          snapshot.current_row_decision.security_recheck_required ? "true" : "false"},
         {"durable_mga_inventory_remains_authority",
          snapshot.current_row_decision.durable_mga_inventory_remains_authority
              ? "true"
              : "false"},
         {"map_is_transaction_finality_authority", "false"},
         {"probe_count",
          std::to_string(snapshot.current_row_decision.counters.probes)},
         {"accepted_count",
          std::to_string(snapshot.current_row_decision.counters.accepted)},
         {"refused_count",
          std::to_string(snapshot.current_row_decision.counters.refused)},
         {"stale_refusal_count",
          std::to_string(snapshot.current_row_decision.counters.stale_refusals)},
         {"epoch_refusal_count",
          std::to_string(snapshot.current_row_decision.counters.epoch_refusals)},
         {"horizon_refusal_count",
          std::to_string(snapshot.current_row_decision.counters.horizon_refusals)},
         {"authority_refusal_count",
          std::to_string(snapshot.current_row_decision.counters.authority_refusals)}});
  }

  if (snapshot.page_finality_decision_present) {
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "transaction_page_finality_refusal"},
         {"accepted", snapshot.page_finality_decision.accepted ? "true" : "false"},
         {"all_visible", snapshot.page_finality_decision.all_visible ? "true" : "false"},
         {"all_final", snapshot.page_finality_decision.all_final ? "true" : "false"},
         {"refusal_reason", snapshot.page_finality_decision.refusal_reason},
         {"normal_mga_recheck_required",
          snapshot.page_finality_decision.normal_mga_recheck_required ? "true"
                                                                      : "false"},
         {"durable_mga_inventory_remains_authority",
          snapshot.page_finality_decision.durable_mga_inventory_remains_authority
              ? "true"
              : "false"},
         {"map_is_transaction_finality_authority", "false"},
         {"evidence_examined",
          std::to_string(
              snapshot.page_finality_decision.counters.evidence_examined)},
         {"accepted_count",
          std::to_string(snapshot.page_finality_decision.counters.accepted)},
         {"refused_count",
          std::to_string(snapshot.page_finality_decision.counters.refused)},
         {"stale_refusal_count",
          std::to_string(snapshot.page_finality_decision.counters.stale_refusals)},
         {"epoch_refusal_count",
          std::to_string(snapshot.page_finality_decision.counters.epoch_refusals)},
         {"horizon_refusal_count",
          std::to_string(snapshot.page_finality_decision.counters.horizon_refusals)},
         {"provenance_refusal_count",
          std::to_string(
              snapshot.page_finality_decision.counters.provenance_refusals)}});
  }

  if (snapshot.cleanup_result_present) {
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "transaction_cleanup_evidence"},
         {"cleanup_horizon_authoritative",
          snapshot.cleanup_result.cleanup_horizon_authoritative ? "true"
                                                                : "false"},
         {"authoritative_cleanup_horizon_local_transaction_id",
          std::to_string(
              snapshot.cleanup_result
                  .authoritative_cleanup_horizon_local_transaction_id)},
         {"reclaimed_row_version_count",
          std::to_string(snapshot.cleanup_result.reclaimed_row_version_count)},
         {"retained_row_version_count",
          std::to_string(snapshot.cleanup_result.retained_row_version_count)},
         {"horizon_blocked_row_version_count",
          std::to_string(
              snapshot.cleanup_result.horizon_blocked_row_version_count)},
         {"limbo_or_unknown_outcome_blocked_row_version_count",
          std::to_string(snapshot.cleanup_result
                             .limbo_or_unknown_outcome_blocked_row_version_count)},
         {"reclaim_evidence_record_count",
          std::to_string(snapshot.cleanup_result.reclaim_evidence_records.size())},
         {"physical_storage_mutated", "false"}});
  }

  if (snapshot.lock_result_present) {
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "transaction_lock_evidence"},
         {"lock_decision", mga::TransactionLockDecisionName(
                               snapshot.lock_result.decision)},
         {"blocking_transaction_id",
          std::to_string(snapshot.lock_result.blocking_transaction.value)},
         {"retry_after_millis",
          std::to_string(snapshot.lock_result.retry_after_millis)},
         {"wait_elapsed_millis",
          std::to_string(snapshot.lock_result.wait_elapsed_millis)},
         {"diagnostic_code", snapshot.lock_result.diagnostic.diagnostic_code}});
  }

  if (snapshot.savepoint_plan_present) {
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "transaction_savepoint_evidence"},
         {"savepoint_decision", mga::SavepointRollbackDecisionName(
                                    snapshot.savepoint_plan.decision)},
         {"savepoint_name", snapshot.savepoint_plan.savepoint.name},
         {"affected_mutation_count",
          std::to_string(snapshot.savepoint_plan.affected_mutation_count)},
         {"rollback_action_count",
          std::to_string(snapshot.savepoint_plan.rollback_actions.size())},
         {"diagnostic_code", snapshot.savepoint_plan.diagnostic.diagnostic_code}});
  }
}

void AddDurableCatalogSummaryRows(EnginePrepareSupportBundleResult* result,
                                  const agents::DurableAgentCatalogImage& image,
                                  const std::string& storage_linkage_digest) {
  AddApiBehaviorRow(
      result,
      {{"bundle_record_kind", "agent_durable_catalog_summary"},
       {"catalog_source", agents::AgentCatalogStateSourceName(image.source)},
       {"catalog_generation",
        std::to_string(image.authority.catalog_generation)},
       {"catalog_root_digest", image.authority.catalog_root_digest},
       {"previous_catalog_root_digest",
        image.authority.previous_catalog_root_digest},
       {"storage_linkage_digest", storage_linkage_digest},
       {"mga_transaction_uuid", RedactedUuidIfPresent(
                                    image.authority.mga_transaction_uuid)},
       {"database_uuid", RedactedUuidIfPresent(image.authority.database_uuid)},
       {"catalog_storage_uuid", image.authority.catalog_storage_uuid},
       {"storage_record_evidence",
        image.authority.storage_catalog_record_evidence ? "true" : "false"},
       {"transaction_inventory_bound",
        image.authority.transaction_inventory_bound ? "true" : "false"},
       {"fsync_or_checkpoint_evidence",
        image.authority.fsync_or_checkpoint_evidence ? "true" : "false"},
       {"instance_count", std::to_string(image.instances.size())},
       {"evidence_count", std::to_string(image.evidence.size())},
       {"action_count", std::to_string(image.actions.size())},
       {"lease_count", std::to_string(image.leases.size())},
       {"resource_reservation_count",
        std::to_string(image.resource_reservations.size())}});
}

void AddDurableCatalogEvidenceRows(EnginePrepareSupportBundleResult* result,
                                   const agents::DurableAgentCatalogImage& image) {
  for (const auto& evidence : image.evidence) {
    const auto validation = agents::ValidateCommercialAgentEvidence(evidence);
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "agent_durable_evidence"},
         {"agent_type_id", evidence.agent_type_id},
         {"agent_uuid", evidence.instance_uuid},
         {"evidence_uuid", evidence.evidence_uuid},
         {"evidence_kind", evidence.evidence_kind.empty()
                               ? "agent_runtime_evidence"
                               : evidence.evidence_kind},
         {"result_state", evidence.result_state.empty() ? "success"
                                                        : evidence.result_state},
         {"diagnostic_code",
          evidence.diagnostic_code.empty() ? "AGENT.NONE"
                                           : evidence.diagnostic_code},
         {"policy_generation", std::to_string(evidence.policy_generation)},
         {"metric_digest_present",
          evidence.input_metric_digest.empty() ? "false" : "true"},
         {"principal_redacted",
          evidence.principal_uuid.empty() ? "false" : "true"},
         {"rights_used_count", std::to_string(evidence.rights_used.size())},
         {"scope_uuid_count", std::to_string(evidence.scope_uuids.size())},
         {"decision_payload_digest_present",
          evidence.decision_payload_digest.empty() ? "false" : "true"},
         {"redaction_class", evidence.redaction_class},
         {"retention_class", evidence.retention_class},
         {"outcome_verification_evidence_uuid",
          evidence.outcome_verification_evidence_uuid},
         {"protected_material_suppressed",
          evidence.protected_material_suppressed ? "true" : "false"},
         {"tamper_valid", validation.tamper_valid ? "true" : "false"},
         {"tamper_digest_algorithm", evidence.tamper_digest_algorithm},
         {"tamper_chain_digest_present",
         evidence.tamper_chain_digest.empty() ? "false" : "true"},
         {"tamper_signature_algorithm",
          evidence.tamper_signature_algorithm},
         {"tamper_signature_present",
          evidence.tamper_signature.empty() ? "false" : "true"},
         {"tamper_key_id", RedactedPayloadValue(evidence.tamper_key_id)},
         {"tamper_key_provenance",
          RedactedPayloadValue(evidence.tamper_key_provenance)},
         {"tamper_key_generation",
          std::to_string(evidence.tamper_key_generation)},
         {"storage_linkage_digest_present",
          evidence.storage_linkage_digest.empty() ? "false" : "true"},
         {"authority_clean", validation.authority_clean ? "true" : "false"},
         {"forbidden_fields_absent", "true"}});
  }
}

void AddDurableCatalogActionRows(EnginePrepareSupportBundleResult* result,
                                 const agents::DurableAgentCatalogImage& image) {
  for (const auto& action : image.actions) {
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "agent_durable_action"},
         {"action_uuid", action.action_uuid},
         {"agent_uuid", action.instance_uuid},
         {"operation_id", action.operation_id},
         {"actuator_provider_id", action.actuator_provider_id},
         {"action_state", agents::DurableAgentActionStateName(action.state)},
         {"evidence_uuid", action.evidence_uuid},
         {"verification_evidence_uuid", action.verification_evidence_uuid},
         {"diagnostic_code",
          action.diagnostic_code.empty() ? "AGENT.NONE" : action.diagnostic_code},
         {"generation", std::to_string(action.generation)},
         {"retry_count", std::to_string(action.retry_count)},
         {"outcome_verified", action.outcome_verified ? "true" : "false"},
         {"compensation_required",
          action.compensation_required ? "true" : "false"},
         {"compensation_attempted",
          action.compensation_attempted ? "true" : "false"},
         {"parser_authority", "false"},
         {"client_authority", "false"},
         {"donor_authority", "false"},
         {"sidecar_authority", "false"}});
  }
}

void AddDurableCatalogLeaseRows(EnginePrepareSupportBundleResult* result,
                                const agents::DurableAgentCatalogImage& image) {
  for (const auto& lease : image.leases) {
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "agent_durable_lease"},
         {"lease_uuid", lease.lease_uuid},
         {"agent_uuid", lease.instance_uuid},
         {"owner_uuid", RedactedUuidIfPresent(lease.owner_uuid)},
         {"lease_state", agents::DurableAgentLeaseStateName(lease.state)},
         {"heartbeat_generation",
          std::to_string(lease.heartbeat_generation)},
         {"replay_generation", std::to_string(lease.replay_generation)},
         {"evidence_uuid", lease.evidence_uuid}});
  }
}

void AddDurableCatalogResourceReservationRows(
    EnginePrepareSupportBundleResult* result,
    const agents::DurableAgentCatalogImage& image) {
  for (const auto& reservation : image.resource_reservations) {
    AddApiBehaviorRow(
        result,
        {{"bundle_record_kind", "agent_durable_resource_reservation"},
         {"reservation_uuid", reservation.reservation_uuid},
         {"reservation_key", RedactedPayloadValue(reservation.reservation_key)},
         {"owner_scope", RedactedPayloadValue(reservation.owner_scope)},
         {"agent_type_id", reservation.agent_type_id},
         {"operation_id", reservation.operation_id},
         {"reservation_state",
          agents::DurableAgentResourceReservationStateName(reservation.state)},
         {"memory_bytes", std::to_string(reservation.memory_bytes)},
         {"worker_slots", std::to_string(reservation.worker_slots)},
         {"overhead_microseconds",
          std::to_string(reservation.overhead_microseconds)},
         {"evidence_uuid", reservation.evidence_uuid},
         {"release_evidence_uuid", reservation.release_evidence_uuid},
         {"release_reason", reservation.release_reason},
         {"parser_authority", "false"},
         {"client_authority", "false"},
         {"donor_authority", "false"},
         {"benchmark_authority", "false"}});
  }
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_MANAGEMENT_SUPPORT_BUNDLE_API_BEHAVIOR
EnginePrepareSupportBundleResult EnginePrepareSupportBundle(const EnginePrepareSupportBundleRequest& request) {
  constexpr const char* kOperation = "management.prepare_support_bundle";
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "OPS.SUPPORT_BUNDLE.SECURITY_CONTEXT_REQUIRED"));
  }
  if (!OptionEnabled(request, "engine_authorized_support_export")) {
    return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "OPS.SUPPORT_BUNDLE.ENGINE_AUTHORIZATION_REQUIRED"));
  }
  const auto retention_policy_ref = OptionValue(request, "retention_policy_ref:");
  const auto redaction_profile_ref = OptionValue(request, "redaction_profile_ref:");
  if (!OptionBool(request, "support_bundle_policy_installed:", true) ||
      retention_policy_ref == "none" ||
      retention_policy_ref == "disabled" ||
      redaction_profile_ref == "none" ||
      redaction_profile_ref == "disabled") {
    return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "OPS.SUPPORT_BUNDLE.POLICY_REQUIRED"));
  }
  if (OptionEnabled(request, "include_protected_material") ||
      OptionEnabled(request, "include_plaintext_secret") ||
      OptionEnabled(request, "redaction_disabled")) {
    return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN"));
  }
  const bool production_agent_bundle =
      OptionBool(request, "agent_support_bundle_production_live:", false) ||
      OptionEnabledAny(request,
                       {"agent_support_bundle_production_live",
                        "agent_durable_catalog_required",
                        "agent_durable_catalog_store_required"});
  const bool durable_catalog_required =
      production_agent_bundle ||
      OptionBool(request, "agent_durable_catalog_required:", false) ||
      OptionBool(request, "agent_durable_catalog_store_required:", false);
  const bool load_durable_catalog =
      durable_catalog_required ||
      OptionBool(request, "agent_durable_catalog_load:", false) ||
      OptionBool(request, "agent_durable_catalog_store_load:", false);
  const bool allow_caller_agent_evidence =
      OptionBool(request,
                 "allow_caller_agent_runtime_evidence:",
                 !production_agent_bundle);
  if (production_agent_bundle && !allow_caller_agent_evidence &&
      !request.agent_runtime_evidence.empty()) {
    return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(
            kOperation,
            "OPS.SUPPORT_BUNDLE.CALLER_AGENT_EVIDENCE_FORBIDDEN"));
  }
  AgentDurableCatalogStoreResult loaded_catalog;
  if (load_durable_catalog) {
    loaded_catalog =
        LoadAgentDurableCatalogImage(request.context, production_agent_bundle);
    if (!loaded_catalog.ok) {
      if (durable_catalog_required) {
        return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
            request.context,
            kOperation,
            MakeEngineApiDiagnostic(
                "OPS.SUPPORT_BUNDLE.DURABLE_AGENT_CATALOG_REQUIRED",
                "support_bundle.durable_agent_catalog_required",
                loaded_catalog.diagnostic.detail.empty()
                    ? loaded_catalog.diagnostic.code
                    : loaded_catalog.diagnostic.detail,
                true));
      }
    }
  }
  if (request.transaction_evidence_snapshot_present) {
    auto transaction_snapshot_diagnostic =
        ValidateTransactionEvidenceSnapshot(request.transaction_evidence_snapshot);
    if (transaction_snapshot_diagnostic.error) {
      return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
          request.context,
          kOperation,
          std::move(transaction_snapshot_diagnostic));
    }
  }
  auto result = MakeApiBehaviorSuccess<EnginePrepareSupportBundleResult>(request.context, kOperation);
  result.redaction_applied = true;
  result.forbidden_fields_absent = true;
  result.flush_required_before_export = true;
  result.agent_runtime_evidence_collected =
      !request.agent_runtime_evidence.empty() ||
      (loaded_catalog.ok &&
       (!loaded_catalog.image.evidence.empty() ||
        !loaded_catalog.image.actions.empty() ||
        !loaded_catalog.image.leases.empty() ||
        !loaded_catalog.image.resource_reservations.empty()));
  result.performance_optimization_surface_collected =
      request.performance_optimization_snapshot_present;
  result.transaction_evidence_collected =
      request.transaction_evidence_snapshot_present;
  result.retention_policy_ref = "support.bundle.default_retention.v1";
  result.redaction_profile_ref = "server.support_bundle.default_redaction.v1";
  result.authority_path = "engine.authorization.management.SUPPORT_EXPORT";
  result.audit_envelope_ref = "engine.supportability.audit_envelope.v1";
  if (request.performance_optimization_snapshot_present) {
    const auto validation = ValidatePerformanceOptimizationSurfaceSnapshot(
        request.performance_optimization_snapshot);
    if (!validation.ok) {
      return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
          request.context,
          kOperation,
          MakeEngineApiDiagnostic(validation.diagnostic_code,
                                  "observability.performance_optimization.invalid_snapshot",
                                  validation.detail,
                                  true));
    }
    const auto config_resolution = ResolvePerformanceOptimizationConfigSurface(
        request.performance_optimization_config_overrides);
    result.support_bundle_json =
        SerializePerformanceOptimizationSupportBundleJson(
            request.performance_optimization_snapshot, config_resolution);
  }
  TransactionBundleInventoryStats transaction_bundle_stats;
  if (request.transaction_evidence_snapshot_present) {
    transaction_bundle_stats =
        SummarizeTransactionInventory(request.transaction_evidence_snapshot
                                          .inventory);
    if (result.support_bundle_json.empty()) {
      result.support_bundle_json = RenderTransactionSupportBundleJson(
          request.transaction_evidence_snapshot, transaction_bundle_stats);
    }
  }
  AddApiBehaviorRow(&result,
                    {{"bundle_scope", "local_node"},
                     {"result_state", "success"},
                     {"diagnostic_code", "AGENT.NONE"},
                     {"redaction", request.context.security_context_present ? "permission_aware" : "self_safe"},
                     {"database_path_present", request.context.database_path.empty() ? "false" : "true"},
                     {"retention_policy_ref", result.retention_policy_ref},
                     {"redaction_profile_ref", result.redaction_profile_ref},
                     {"authority_path", result.authority_path},
                     {"audit_envelope_ref", result.audit_envelope_ref},
                     {"flush_required_before_export", result.flush_required_before_export ? "true" : "false"},
                     {"forbidden_fields_absent", result.forbidden_fields_absent ? "true" : "false"},
                     {"agent_runtime_evidence_collected",
                      result.agent_runtime_evidence_collected ? "true" : "false"},
                     {"agent_runtime_evidence_source",
                      loaded_catalog.ok ? "durable_agent_catalog_store"
                                        : "caller_supplied_or_absent"},
                     {"performance_optimization_surface_collected",
                      result.performance_optimization_surface_collected ? "true" : "false"},
                     {"transaction_evidence_collected",
                      result.transaction_evidence_collected ? "true" : "false"}});
  if (request.performance_optimization_snapshot_present) {
    const auto config_resolution = ResolvePerformanceOptimizationConfigSurface(
        request.performance_optimization_config_overrides);
    const std::string config_override_resolution =
        request.performance_optimization_config_overrides.empty()
            ? "packaged_default"
            : "request_overrides";
    AddApiBehaviorRow(&result,
                      {{"bundle_record_kind", "performance_optimization_surface"},
                       {"schema_id", PerformanceOptimizationSurfaceSchemaId()},
                       {"schema_version",
                        std::to_string(PerformanceOptimizationSurfaceSchemaVersion())},
                       {"cleanup_horizon_authority_status",
                        RedactedPayloadValue(
                            request.performance_optimization_snapshot
                                .cleanup_horizon_authority_status)},
                       {"oldest_interesting_transaction_id",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .oldest_interesting_transaction_id)},
                       {"oldest_active_transaction_id",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .oldest_active_transaction_id)},
                       {"oldest_snapshot_transaction_id",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .oldest_snapshot_transaction_id)},
                       {"storage_row_version_backlog_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .storage_row_version_backlog_count)},
                       {"index_delta_backlog_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .index_delta_backlog_count)},
                       {"index_garbage_backlog_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .index_garbage_backlog_count)},
                       {"last_agent_decision",
                        RedactedPayloadValue(
                            request.performance_optimization_snapshot
                                .last_agent_decision)},
                       {"secondary_index_state",
                        RedactedPayloadValue(
                            request.performance_optimization_snapshot
                                .secondary_index_state)},
                       {"exact_refusal_diagnostic_code",
                        RedactedPayloadValue(
                            request.performance_optimization_snapshot
                                .exact_refusal_diagnostic_code)},
                       {"exact_refusal_message_vector",
                        RedactedPayloadValue(
                            request.performance_optimization_snapshot
                                .exact_refusal_message_vector)},
                       {"support_bundle_completeness_state",
                        RedactedPayloadValue(
                            request.performance_optimization_snapshot
                                .support_bundle_completeness_state)},
                       {"support_bundle_forbidden_fields_absent",
                        request.performance_optimization_snapshot
                                .support_bundle_forbidden_fields_absent
                            ? "true"
                            : "false"},
                       {"odf108_surface_ready", "true"},
                       {"odf108_selected_path_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .odf108_selected_paths.size())},
                       {"odf108_feature_gate_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .odf108_feature_gates.size())},
                       {"odf108_fallback_reason_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .odf108_fallbacks.size())},
                       {"odf108_quota_state_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .odf108_quotas.size())},
                       {"odf108_runtime_compatibility_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .odf108_runtime_compatibility.size())},
                       {"odf108_rebuild_state_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .odf108_rebuild_states.size())},
                       {"odf108_exact_refusal_count",
                        std::to_string(
                            request.performance_optimization_snapshot
                                .odf108_exact_refusals.size())},
                       {"config_defaults_packaging_ready", "true"},
                       {"config_override_resolution",
                        config_override_resolution},
                       {"config_defaults_field_count",
                        std::to_string(config_resolution.fields.size())},
                       {"parser_finality_authority",
                        request.performance_optimization_snapshot
                                .parser_finality_authority
                            ? "true"
                            : "false"},
                       {"donor_finality_authority", request.performance_optimization_snapshot.donor_finality_authority ? "true" : "false"},
                       {"wal_recovery_authority",
                        request.performance_optimization_snapshot
                                .wal_recovery_authority
                            ? "true"
                            : "false"}});
  }
  if (request.transaction_evidence_snapshot_present) {
    AddTransactionSupportBundleRows(&result,
                                    request.transaction_evidence_snapshot);
  }
  for (const auto& evidence : request.agent_runtime_evidence) {
    auto uuid_diagnostic = ValidateEvidenceUuids(evidence);
    if (uuid_diagnostic.error) {
      return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
          request.context,
          kOperation,
          std::move(uuid_diagnostic));
    }
    auto zero_grey_diagnostic = ValidateSupportBundleZeroGrey(evidence);
    if (zero_grey_diagnostic.error) {
      return MakeApiBehaviorDiagnostic<EnginePrepareSupportBundleResult>(
          request.context,
          kOperation,
          std::move(zero_grey_diagnostic));
    }
    AddApiBehaviorRow(&result,
                      {{"bundle_record_kind", "agent_runtime_evidence"},
                       {"agent_type_id", evidence.agent_type_id},
                       {"agent_uuid", evidence.agent_uuid},
                       {"filespace_uuid", evidence.filespace_uuid},
                       {"policy_uuid", evidence.policy_uuid},
                       {"evidence_uuid", evidence.evidence_uuid},
                       {"evidence_kind", RedactedPayloadValue(
                                             evidence.evidence_kind.empty()
                                                 ? "agent_runtime_evidence"
                                                 : evidence.evidence_kind)},
                       {"result_state", SupportBundleResultStateOrDefault(evidence)},
                       {"diagnostic_code",
                        RedactedPayloadValue(SupportBundleDiagnosticOrDefault(evidence))},
                       {"payload_digest", RedactedPayloadValue(evidence.payload_digest)},
                       {"payload_redacted", evidence.payload_redacted ? "YES" : "NO"},
                       {"retention_class", RedactedPayloadValue(
                                               evidence.retention_class.empty()
                                                   ? "support_bundle_evidence"
                                                   : evidence.retention_class)},
                       {"retention_policy_ref",
                        RedactedPayloadValue(evidence.retention_policy_ref.empty()
                                                 ? result.retention_policy_ref
                                                 : evidence.retention_policy_ref)},
                       {"retention_deadline", RedactedPayloadValue(evidence.retention_deadline)},
                       {"legal_hold", evidence.legal_hold ? "true" : "false"},
                       {"maintenance_hold", evidence.maintenance_hold ? "true" : "false"},
                       {"purge_eligibility", evidence.purge_eligible ? "eligible" : "retained"},
                       {"physical_path_present", evidence.physical_path.empty() ? "false" : "true"},
                       {"physical_path", evidence.physical_path.empty() ? "" : "<redacted>"},
                       {"unsafe_payload", evidence.unsafe_payload.empty() ? "" : "<redacted>"},
                       {"forbidden_fields_absent", "true"}});
  }
  if (loaded_catalog.ok) {
    AddDurableCatalogSummaryRows(&result,
                                 loaded_catalog.image,
                                 loaded_catalog.storage_linkage_digest);
    AddDurableCatalogEvidenceRows(&result, loaded_catalog.image);
    AddDurableCatalogActionRows(&result, loaded_catalog.image);
    AddDurableCatalogLeaseRows(&result, loaded_catalog.image);
    AddDurableCatalogResourceReservationRows(&result, loaded_catalog.image);
  }
  AddApiBehaviorEvidence(&result, "support_bundle_manifest", "local_node_redacted");
  AddApiBehaviorEvidence(&result, "supportability_flush", "required_before_export");
  AddApiBehaviorEvidence(&result, "support_bundle_redaction", result.redaction_profile_ref);
  AddApiBehaviorEvidence(&result, "support_bundle_authority", result.authority_path);
  AddApiBehaviorEvidence(&result, "support_bundle_audit_envelope", result.audit_envelope_ref);
  if (result.agent_runtime_evidence_collected) {
    AddApiBehaviorEvidence(&result, "support_bundle_agent_runtime_evidence", "redacted");
  }
  if (loaded_catalog.ok) {
    AddApiBehaviorEvidence(&result,
                           "support_bundle_agent_durable_catalog",
                           loaded_catalog.image.authority.catalog_root_digest);
    AddApiBehaviorEvidence(&result,
                           "support_bundle_agent_evidence_tamper_chain",
                           loaded_catalog.image.evidence.empty()
                               ? "empty"
                               : "validated");
  }
  if (result.performance_optimization_surface_collected) {
    AddApiBehaviorEvidence(&result,
                           "support_bundle_performance_optimization_surface",
                           PerformanceOptimizationSurfaceSchemaId());
    AddApiBehaviorEvidence(&result,
                           "support_bundle_config_defaults",
                           "performance_optimization_config");
    AddApiBehaviorEvidence(
        &result,
        "config_override_resolution",
        request.performance_optimization_config_overrides.empty()
            ? "packaged_default"
            : "request_overrides");
  }
  if (result.transaction_evidence_collected) {
    AddApiBehaviorEvidence(&result,
                           "support_bundle_transaction_evidence",
                           TransactionSupportBundleSchemaId());
    AddApiBehaviorEvidence(&result,
                           "transaction_authority_source",
                           "durable_mga_transaction_inventory");
    AddApiBehaviorEvidence(&result,
                           "transaction_support_bundle_authority",
                           "false");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
