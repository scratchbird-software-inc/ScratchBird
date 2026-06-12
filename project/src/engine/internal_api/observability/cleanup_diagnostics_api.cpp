// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_CLEANUP_BACKUP_RESTORE_REPAIR_DIAGNOSTICS
#include "observability/cleanup_diagnostics_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "security/authorization_api.hpp"
#include "security/security_model.hpp"

#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;

constexpr const char* kOperation = "observability.cleanup_diagnostics.inspect";
constexpr const char* kHorizonIdentity =
    "dpc030_authoritative_cleanup_horizon_v1";
constexpr const char* kSurfaceEvidence =
    "dpc034_cleanup_backup_restore_repair_diagnostics_v1";

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool AuthorizeCleanupDiagnostics(const EngineRequestContext& context,
                                 std::string* granted_right,
                                 EngineApiDiagnostic* diagnostic) {
  static const std::vector<std::string> kRights = {
      "MGA_CLEANUP_INSPECT",
      "OBS_MANAGEMENT_INSPECT"};
  for (const auto& right : kRights) {
    EngineAuthorizeRequest authorize;
    authorize.context = context;
    authorize.required_right = right;
    const auto authorized = EngineAuthorize(authorize);
    if (authorized.ok && authorized.authorized) {
      if (granted_right != nullptr) *granted_right = right;
      return true;
    }
    if (!authorized.diagnostics.empty() &&
        authorized.diagnostics.front().code == "SECURITY.CONTEXT.EXPIRED") {
      if (diagnostic != nullptr) *diagnostic = authorized.diagnostics.front();
      return false;
    }
  }
  if (diagnostic != nullptr) {
    *diagnostic = MakeSecurityDiagnostic(
        "SECURITY.AUTHORIZATION.DENIED",
        "MGA_CLEANUP_INSPECT|OBS_MANAGEMENT_INSPECT");
  }
  return false;
}

template <typename TResult>
TResult CleanupDiagnosticsAuthorizationDenied(
    const EngineRequestContext& context,
    const std::string& operation_id,
    EngineApiDiagnostic diagnostic) {
  auto result = MakeApiBehaviorDiagnostic<TResult>(context,
                                                  operation_id,
                                                  std::move(diagnostic));
  AddApiBehaviorEvidence(&result, "engine_authorization_authority",
                         "EngineAuthorize");
  AddApiBehaviorEvidence(&result, "authorization_required_rights",
                         "MGA_CLEANUP_INSPECT|OBS_MANAGEMENT_INSPECT");
  AddApiBehaviorEvidence(&result, "authorization_decision",
                         "deny:cleanup_diagnostics");
  AddApiBehaviorRow(&result,
                    {{"record_kind", "engine_authorization_decision"},
                     {"authorization_authority", "engine_internal_api"},
                     {"decision", "deny"},
                     {"operation_class", "cleanup_backlog_inspect"},
                     {"required_rights",
                      "MGA_CLEANUP_INSPECT|OBS_MANAGEMENT_INSPECT"},
                     {"message_vector",
                      result.diagnostics.empty()
                          ? "SECURITY.AUTHORIZATION.DENIED"
                          : result.diagnostics.front().code}});
  return result;
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

std::string DiagnosticCode(const scratchbird::core::platform::DiagnosticRecord& diagnostic,
                           std::string_view fallback) {
  return diagnostic.diagnostic_code.empty() ? std::string(fallback)
                                            : diagnostic.diagnostic_code;
}

const mga::AuthoritativeCleanupHorizonResult* SelectHorizon(
    const EngineCleanupDiagnosticsRequest& request) {
  if (request.cleanup_horizon_present) {
    return &request.cleanup_horizon;
  }
  if (request.storage_cleanup_present) {
    return &request.storage_cleanup.horizon;
  }
  if (request.index_cleanup_present) {
    return &request.index_cleanup.horizon;
  }
  return nullptr;
}

bool HorizonAuthoritative(const EngineCleanupDiagnosticsRequest& request) {
  const auto* horizon = SelectHorizon(request);
  return horizon != nullptr && horizon->ok();
}

std::string HorizonAuthorityStatus(const EngineCleanupDiagnosticsRequest& request) {
  const auto* horizon = SelectHorizon(request);
  if (horizon == nullptr) {
    return "missing";
  }
  if (horizon->ok()) {
    return "authoritative";
  }
  if (!horizon->cleanup_horizon_authoritative) {
    return "refused_non_authoritative";
  }
  return "refused";
}

EngineApiU64 HorizonLocalTransactionId(
    const EngineCleanupDiagnosticsRequest& request) {
  const auto* horizon = SelectHorizon(request);
  if (horizon == nullptr || !horizon->cleanup_horizon.valid()) {
    return 0;
  }
  return horizon->cleanup_horizon.value;
}

bool ValidContextName(const std::string& context) {
  return context == "backup" || context == "restore" ||
         context == "restricted_open" || context == "repair";
}

std::vector<std::string> ContextsOrDefault(
    const EngineCleanupDiagnosticsRequest& request) {
  if (!request.context_kinds.empty()) {
    return request.context_kinds;
  }
  return {"backup", "restore", "restricted_open", "repair"};
}

enum class CleanupDiagnosticIssue {
  none,
  non_authoritative,
  validation_refused,
  cleanup_refused
};

CleanupDiagnosticIssue ClassifyIssue(
    const EngineCleanupDiagnosticsRequest& request) {
  if (!HorizonAuthoritative(request)) {
    return CleanupDiagnosticIssue::non_authoritative;
  }
  if (request.storage_cleanup_present &&
      request.storage_cleanup.decision ==
          agents::StorageVersionCleanupDecisionKind::refused_non_authoritative) {
    return CleanupDiagnosticIssue::non_authoritative;
  }
  if (request.index_cleanup_present &&
      request.index_cleanup.decision ==
          idx::SecondaryIndexGarbageCleanupDecisionKind::refused_non_authoritative) {
    return CleanupDiagnosticIssue::non_authoritative;
  }
  if (request.index_cleanup_present &&
      request.index_cleanup.decision ==
          idx::SecondaryIndexGarbageCleanupDecisionKind::validation_refused) {
    return CleanupDiagnosticIssue::validation_refused;
  }
  if ((request.storage_cleanup_present && request.storage_cleanup.fail_closed) ||
      (request.index_cleanup_present && request.index_cleanup.fail_closed)) {
    return CleanupDiagnosticIssue::cleanup_refused;
  }
  return CleanupDiagnosticIssue::none;
}

struct ContextDecision {
  std::string decision;
  std::string diagnostic_code;
  bool refused = false;
};

ContextDecision DecideForContext(std::string_view context,
                                 CleanupDiagnosticIssue issue) {
  if (issue == CleanupDiagnosticIssue::none) {
    if (context == "backup") {
      return {"backup_allowed_cleanup_backlog_documented",
              "CLEANUP_DIAGNOSTICS.BACKUP_ALLOWED",
              false};
    }
    if (context == "restore") {
      return {"restore_allowed_cleanup_backlog_documented",
              "CLEANUP_DIAGNOSTICS.RESTORE_ALLOWED",
              false};
    }
    if (context == "restricted_open") {
      return {"restricted_open_allowed_support_only",
              "CLEANUP_DIAGNOSTICS.RESTRICTED_OPEN_ALLOWED",
              false};
    }
    return {"repair_allowed_cleanup_backlog_documented",
            "CLEANUP_DIAGNOSTICS.REPAIR_ALLOWED",
            false};
  }

  if (issue == CleanupDiagnosticIssue::validation_refused) {
    if (context == "backup") {
      return {"backup_refused_index_cleanup_validation_failed",
              "CLEANUP_DIAGNOSTICS.BACKUP_REFUSED_INDEX_VALIDATION",
              true};
    }
    if (context == "restore") {
      return {"restore_refused_index_cleanup_validation_failed",
              "CLEANUP_DIAGNOSTICS.RESTORE_REFUSED_INDEX_VALIDATION",
              true};
    }
    if (context == "restricted_open") {
      return {"restricted_open_refused_index_cleanup_validation_failed",
              "CLEANUP_DIAGNOSTICS.RESTRICTED_OPEN_REFUSED_INDEX_VALIDATION",
              true};
    }
    return {"repair_refused_index_cleanup_validation_failed",
            "CLEANUP_DIAGNOSTICS.REPAIR_REFUSED_INDEX_VALIDATION",
            true};
  }

  if (issue == CleanupDiagnosticIssue::cleanup_refused) {
    if (context == "backup") {
      return {"backup_refused_cleanup_agent_refused",
              "CLEANUP_DIAGNOSTICS.BACKUP_REFUSED_CLEANUP_AGENT",
              true};
    }
    if (context == "restore") {
      return {"restore_refused_cleanup_agent_refused",
              "CLEANUP_DIAGNOSTICS.RESTORE_REFUSED_CLEANUP_AGENT",
              true};
    }
    if (context == "restricted_open") {
      return {"restricted_open_refused_cleanup_agent_refused",
              "CLEANUP_DIAGNOSTICS.RESTRICTED_OPEN_REFUSED_CLEANUP_AGENT",
              true};
    }
    return {"repair_refused_cleanup_agent_refused",
            "CLEANUP_DIAGNOSTICS.REPAIR_REFUSED_CLEANUP_AGENT",
            true};
  }

  if (context == "backup") {
    return {"backup_refused_cleanup_horizon_non_authoritative",
            "CLEANUP_DIAGNOSTICS.BACKUP_REFUSED_NON_AUTHORITATIVE_HORIZON",
            true};
  }
  if (context == "restore") {
    return {"restore_refused_cleanup_horizon_non_authoritative",
            "CLEANUP_DIAGNOSTICS.RESTORE_REFUSED_NON_AUTHORITATIVE_HORIZON",
            true};
  }
  if (context == "restricted_open") {
    return {"restricted_open_refused_cleanup_horizon_non_authoritative",
            "CLEANUP_DIAGNOSTICS.RESTRICTED_OPEN_REFUSED_NON_AUTHORITATIVE_HORIZON",
            true};
  }
  return {"repair_refused_cleanup_horizon_non_authoritative",
          "CLEANUP_DIAGNOSTICS.REPAIR_REFUSED_NON_AUTHORITATIVE_HORIZON",
          true};
}

void AddSummaryRow(EngineCleanupDiagnosticsResult* result,
                   const EngineCleanupDiagnosticsRequest& request) {
  std::string storage_decision = "not_reported";
  std::string storage_diagnostic = "not_reported";
  if (request.storage_cleanup_present) {
    storage_decision = agents::StorageVersionCleanupDecisionKindName(
        request.storage_cleanup.decision);
    storage_diagnostic = DiagnosticCode(request.storage_cleanup.diagnostic,
                                        "STORAGE_VERSION_CLEANUP.NOT_REPORTED");
  }

  std::string index_decision = "not_reported";
  std::string index_diagnostic = "not_reported";
  if (request.index_cleanup_present) {
    index_decision = idx::SecondaryIndexGarbageCleanupDecisionKindName(
        request.index_cleanup.decision);
    index_diagnostic = DiagnosticCode(request.index_cleanup.diagnostic,
                                      "INDEX_GARBAGE_CLEANUP.NOT_REPORTED");
  }

  AddApiBehaviorRow(
      result,
      {{"record_kind", "cleanup_diagnostics_summary"},
       {"schema_id", CleanupDiagnosticsSurfaceSchemaId()},
       {"schema_version", std::to_string(CleanupDiagnosticsSurfaceSchemaVersion())},
       {"cleanup_horizon_identity", result->cleanup_horizon_identity},
       {"cleanup_horizon_authority_status",
        result->cleanup_horizon_authority_status},
       {"cleanup_horizon_authoritative",
        BoolText(result->cleanup_horizon_authoritative)},
       {"cleanup_horizon_local_transaction_id",
        std::to_string(result->cleanup_horizon_local_transaction_id)},
       {"storage_cleanup_decision", storage_decision},
       {"storage_cleanup_diagnostic_code", storage_diagnostic},
       {"storage_row_version_backlog_count",
        std::to_string(result->storage_row_version_backlog_count)},
       {"storage_row_version_retained_count",
        std::to_string(result->storage_row_version_retained_count)},
       {"storage_row_version_reclaimed_count",
        std::to_string(result->storage_row_version_reclaimed_count)},
       {"storage_row_version_blocked_count",
        std::to_string(result->storage_row_version_blocked_count)},
       {"index_cleanup_decision", index_decision},
       {"index_cleanup_diagnostic_code", index_diagnostic},
       {"index_garbage_backlog_count",
        std::to_string(result->index_garbage_backlog_count)},
       {"index_garbage_cleaned_count",
        std::to_string(result->index_garbage_cleaned_count)},
       {"index_garbage_retained_count",
        std::to_string(result->index_garbage_retained_count)},
       {"index_garbage_horizon_blocked_count",
        std::to_string(result->index_garbage_horizon_blocked_count)},
       {"index_validation_refused_count",
        std::to_string(result->index_validation_refused_count)},
       {"index_non_authoritative_refused_count",
        std::to_string(result->index_non_authoritative_refused_count)},
       {"parser_finality_authority", "false"},
       {"client_finality_authority", "false"},
       {"timestamp_finality_authority", "false"},
       {"uuid_ordering_finality_authority", "false"},
       {"event_stream_finality_authority", "false"},
       {"wal_recovery_authority", "false"},
       {"authority_source", "durable_mga_transaction_inventory"}});
}

void AddContextRows(EngineCleanupDiagnosticsResult* result,
                    const std::vector<std::string>& contexts,
                    CleanupDiagnosticIssue issue) {
  for (const auto& context : contexts) {
    const auto decision = DecideForContext(context, issue);
    AddApiBehaviorRow(
        result,
        {{"record_kind", "cleanup_context_decision"},
         {"context_kind", context},
         {"context_classification", context},
         {"exact_refusal_decision", decision.decision},
         {"exact_refusal_diagnostic", decision.diagnostic_code},
         {"refused", BoolText(decision.refused)},
         {"cleanup_horizon_identity", result->cleanup_horizon_identity},
         {"cleanup_horizon_authority_status",
          result->cleanup_horizon_authority_status},
         {"storage_row_version_backlog_count",
          std::to_string(result->storage_row_version_backlog_count)},
         {"index_garbage_backlog_count",
          std::to_string(result->index_garbage_backlog_count)},
         {"parser_finality_authority", "false"},
         {"client_finality_authority", "false"},
         {"timestamp_finality_authority", "false"},
         {"uuid_ordering_finality_authority", "false"},
         {"event_stream_finality_authority", "false"}});
    result->diagnostics.push_back(MakeEngineApiDiagnostic(
        decision.diagnostic_code,
        "cleanup_diagnostics.context_decision",
        decision.decision,
        false));
  }
}

std::string SupportBundleJson(const EngineCleanupDiagnosticsResult& result) {
  std::ostringstream out;
  out << "{\"support_bundle\":{\"section\":\"cleanup_diagnostics\","
      << "\"schema_id\":\"" << CleanupDiagnosticsSurfaceSchemaId() << "\","
      << "\"redaction_state\":\"public_safe_summary\","
      << "\"forbidden_fields_absent\":true,"
      << "\"cleanup_horizon_identity\":\""
      << JsonEscape(result.cleanup_horizon_identity) << "\","
      << "\"cleanup_horizon_authority_status\":\""
      << JsonEscape(result.cleanup_horizon_authority_status) << "\","
      << "\"cleanup_horizon_local_transaction_id\":"
      << result.cleanup_horizon_local_transaction_id << ','
      << "\"storage_row_version_backlog_count\":"
      << result.storage_row_version_backlog_count << ','
      << "\"storage_row_version_retained_count\":"
      << result.storage_row_version_retained_count << ','
      << "\"storage_row_version_reclaimed_count\":"
      << result.storage_row_version_reclaimed_count << ','
      << "\"storage_row_version_blocked_count\":"
      << result.storage_row_version_blocked_count << ','
      << "\"index_garbage_backlog_count\":"
      << result.index_garbage_backlog_count << ','
      << "\"index_garbage_cleaned_count\":"
      << result.index_garbage_cleaned_count << ','
      << "\"index_garbage_retained_count\":"
      << result.index_garbage_retained_count << ','
      << "\"index_garbage_horizon_blocked_count\":"
      << result.index_garbage_horizon_blocked_count << ','
      << "\"index_validation_refused_count\":"
      << result.index_validation_refused_count << ','
      << "\"index_non_authoritative_refused_count\":"
      << result.index_non_authoritative_refused_count << ','
      << "\"parser_finality_authority\":false,"
      << "\"client_finality_authority\":false,"
      << "\"timestamp_finality_authority\":false,"
      << "\"uuid_ordering_finality_authority\":false,"
      << "\"event_stream_finality_authority\":false}}";
  return out.str();
}

void PopulateCounts(EngineCleanupDiagnosticsResult* result,
                    const EngineCleanupDiagnosticsRequest& request) {
  result->cleanup_horizon_identity = kHorizonIdentity;
  result->cleanup_horizon_authority_status = HorizonAuthorityStatus(request);
  result->cleanup_horizon_authoritative = HorizonAuthoritative(request);
  result->cleanup_horizon_local_transaction_id = HorizonLocalTransactionId(request);

  if (request.storage_cleanup_present) {
    result->storage_row_version_backlog_count =
        request.storage_cleanup.before.cleanup_candidate_row_versions;
    result->storage_row_version_retained_count =
        request.storage_cleanup.after.retained_row_versions;
    result->storage_row_version_reclaimed_count =
        request.storage_cleanup.after.reclaimed_row_versions;
    result->storage_row_version_blocked_count =
        request.storage_cleanup.after.blocked_row_versions;
  }

  if (request.index_cleanup_present) {
    result->index_garbage_backlog_count =
        request.index_cleanup.before.eligible_garbage_records +
        request.index_cleanup.before.retained_garbage_records;
    result->index_garbage_cleaned_count =
        request.index_cleanup.after.cleaned_garbage_records;
    result->index_garbage_retained_count =
        request.index_cleanup.after.retained_garbage_records +
        request.index_cleanup.after.retained_unmerged_delta_records;
    result->index_garbage_horizon_blocked_count =
        request.index_cleanup.horizon_blocked
            ? request.index_cleanup.before.retained_garbage_records
            : 0;
    result->index_validation_refused_count =
        request.index_cleanup.decision ==
                idx::SecondaryIndexGarbageCleanupDecisionKind::validation_refused
            ? 1
            : 0;
    result->index_non_authoritative_refused_count =
        request.index_cleanup.decision ==
                idx::SecondaryIndexGarbageCleanupDecisionKind::refused_non_authoritative
            ? 1
            : 0;
  }
}

void AddStandardEvidence(EngineCleanupDiagnosticsResult* result) {
  AddApiBehaviorEvidence(result, "cleanup_diagnostics_surface", kSurfaceEvidence);
  AddApiBehaviorEvidence(result, "cleanup_horizon_service", kHorizonIdentity);
  AddApiBehaviorEvidence(result,
                         "support_bundle_surface",
                         "cleanup_diagnostics");
  AddApiBehaviorEvidence(result,
                         "authority_source",
                         "durable_mga_transaction_inventory");
  AddApiBehaviorEvidence(result, "parser_finality_authority", "false");
  AddApiBehaviorEvidence(result, "client_finality_authority", "false");
  AddApiBehaviorEvidence(result, "timestamp_finality_authority", "false");
  AddApiBehaviorEvidence(result, "uuid_ordering_finality_authority", "false");
  AddApiBehaviorEvidence(result, "event_stream_finality_authority", "false");
}

}  // namespace

const char* CleanupDiagnosticsSurfaceSchemaId() {
  return "scratchbird.cleanup_diagnostics.v1";
}

EngineApiU64 CleanupDiagnosticsSurfaceSchemaVersion() {
  return 1;
}

EngineCleanupDiagnosticsResult EngineInspectCleanupDiagnostics(
    const EngineCleanupDiagnosticsRequest& request) {
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineCleanupDiagnosticsResult>(
        request.context,
        kOperation,
        MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  std::string granted_right;
  EngineApiDiagnostic authorization_diagnostic;
  if (!AuthorizeCleanupDiagnostics(request.context,
                                   &granted_right,
                                   &authorization_diagnostic)) {
    return CleanupDiagnosticsAuthorizationDenied<EngineCleanupDiagnosticsResult>(
        request.context,
        kOperation,
        authorization_diagnostic);
  }
  const auto contexts = ContextsOrDefault(request);
  for (const auto& context : contexts) {
    if (!ValidContextName(context)) {
      return MakeApiBehaviorDiagnostic<EngineCleanupDiagnosticsResult>(
          request.context,
          kOperation,
          MakeInvalidRequestDiagnostic(kOperation,
                                       "cleanup_context_kind_unsupported"));
    }
  }
  if (!request.cleanup_horizon_present && !request.storage_cleanup_present &&
      !request.index_cleanup_present) {
    return MakeApiBehaviorDiagnostic<EngineCleanupDiagnosticsResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation,
                                     "cleanup_diagnostics_input_required"));
  }

  auto result =
      MakeApiBehaviorSuccess<EngineCleanupDiagnosticsResult>(request.context,
                                                            kOperation);
  result.result_shape.result_kind = "cleanup.diagnostics.v1";
  result.cleanup_diagnostics_ready = true;
  result.support_bundle_ready = request.support_bundle_requested;
  PopulateCounts(&result, request);

  AddSummaryRow(&result, request);
  AddContextRows(&result, contexts, ClassifyIssue(request));
  AddApiBehaviorRow(&result,
                    {{"record_kind", "cleanup_non_authority_evidence"},
                     {"parser_finality_authority", "false"},
                     {"client_finality_authority", "false"},
                     {"timestamp_finality_authority", "false"},
                     {"uuid_ordering_finality_authority", "false"},
                     {"event_stream_finality_authority", "false"},
                     {"reference_finality_authority", "false"},
                     {"wal_recovery_authority", "false"}});
  AddStandardEvidence(&result);
  AddApiBehaviorEvidence(&result, "engine_authorization_authority",
                         "EngineAuthorize");
  AddApiBehaviorEvidence(&result, "authorization_required_rights",
                         "MGA_CLEANUP_INSPECT|OBS_MANAGEMENT_INSPECT");
  AddApiBehaviorEvidence(&result, "authorization_decision",
                         "allow:" + granted_right);
  AddApiBehaviorRow(&result,
                    {{"record_kind", "engine_authorization_decision"},
                     {"authorization_authority", "engine_internal_api"},
                     {"decision", "allow"},
                     {"operation_class", "cleanup_backlog_inspect"},
                     {"required_rights",
                      "MGA_CLEANUP_INSPECT|OBS_MANAGEMENT_INSPECT"},
                     {"granted_right", granted_right},
                     {"message_vector", "SECURITY.AUTHORIZATION.ALLOW"}});
  result.support_bundle_json = request.support_bundle_requested
                                   ? SupportBundleJson(result)
                                   : std::string{};
  return result;
}

}  // namespace scratchbird::engine::internal_api
