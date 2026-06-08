// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_optimization_surface.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "security/authorization_api.hpp"
#include "security/security_model.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool HasPerformanceSurfaceInspectRight(const EngineRequestContext& context,
                                       std::string* granted_right,
                                       EngineApiDiagnostic* diagnostic) {
  static const std::vector<std::string> kRights = {
      "OBS_MANAGEMENT_INSPECT",
      "OBS_INDEX_PROFILE_READ",
      "MGA_CLEANUP_INSPECT"};
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
        "OBS_MANAGEMENT_INSPECT|OBS_INDEX_PROFILE_READ|MGA_CLEANUP_INSPECT");
  }
  return false;
}

template <typename TResult>
TResult PerformanceSurfaceAuthorizationDenied(
    const EngineRequestContext& context,
    const std::string& operation_id,
    EngineApiDiagnostic diagnostic) {
  auto result = MakeApiBehaviorDiagnostic<TResult>(context, operation_id,
                                                  std::move(diagnostic));
  AddApiBehaviorEvidence(&result, "engine_authorization_authority",
                         "EngineAuthorize");
  AddApiBehaviorEvidence(&result, "authorization_required_rights",
                         "OBS_MANAGEMENT_INSPECT|OBS_INDEX_PROFILE_READ|MGA_CLEANUP_INSPECT");
  AddApiBehaviorEvidence(&result, "authorization_decision",
                         "deny:performance_optimization_surface");
  AddApiBehaviorRow(&result,
                    {{"record_kind", "engine_authorization_decision"},
                     {"authorization_authority", "engine_internal_api"},
                     {"decision", "deny"},
                     {"operation_class", "performance_backlog_inspect"},
                     {"required_rights",
                      "OBS_MANAGEMENT_INSPECT|OBS_INDEX_PROFILE_READ|MGA_CLEANUP_INSPECT"},
                     {"message_vector",
                      result.diagnostics.empty()
                          ? "SECURITY.AUTHORIZATION.DENIED"
                          : result.diagnostics.front().code}});
  return result;
}

void AddMissing(std::vector<std::string>* missing, std::string field) {
  if (missing != nullptr) {
    missing->push_back(std::move(field));
  }
}

void RequireString(const std::string& value,
                   const std::string& field,
                   std::vector<std::string>* missing) {
  if (value.empty()) {
    AddMissing(missing, field);
  }
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream out;
  for (const char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out << "\\u00";
          static constexpr char kHex[] = "0123456789abcdef";
          out << kHex[(static_cast<unsigned char>(ch) >> 4) & 0x0f];
          out << kHex[static_cast<unsigned char>(ch) & 0x0f];
        } else {
          out << ch;
        }
        break;
    }
  }
  return out.str();
}

void Comma(std::ostringstream* out, bool* first) {
  if (!*first) {
    *out << ',';
  }
  *first = false;
}

void AddJsonString(std::ostringstream* out,
                   bool* first,
                   std::string_view key,
                   std::string_view value) {
  Comma(out, first);
  *out << '"' << key << "\":\"" << JsonEscape(value) << '"';
}

void AddJsonU64(std::ostringstream* out,
                bool* first,
                std::string_view key,
                EngineApiU64 value) {
  Comma(out, first);
  *out << '"' << key << "\":" << value;
}

void AddJsonBool(std::ostringstream* out,
                 bool* first,
                 std::string_view key,
                 bool value) {
  Comma(out, first);
  *out << '"' << key << "\":" << (value ? "true" : "false");
}

constexpr std::string_view kPerformanceConfigPrecedence =
    "admin_override > cli_option > environment > config_file > packaged_default";

int ConfigSourceRank(std::string_view source) {
  if (source == "admin_override") return 4;
  if (source == "cli_option") return 3;
  if (source == "environment") return 2;
  if (source == "config_file") return 1;
  if (source == "packaged_default") return 0;
  return -1;
}

std::string DefaultConfiguredValue(std::string_view value) {
  return std::string("packaged_default:") + std::string(value);
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(),
                 value.end(),
                 value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool LooksSupportBundleProtected(std::string_view value) {
  const std::string lower = LowerAscii(std::string(value));
  static constexpr std::string_view kNeedles[] = {
      "secret",      "password",       "passwd",       "pwd=",
      "token",       "private_key",    "credential",   "verifier",
      "encryption_key", "decryption_key", "key_handle",   "cleartext",
      "plaintext",   "api_key",        "apikey",       "raw_key",
      "key_material", "kms_plaintext", "bearer ",      "protected_reference"};
  for (const auto needle : kNeedles) {
    if (lower.find(needle) != std::string::npos) {
      return true;
    }
  }
  return value.find("/home/") != std::string_view::npos ||
         value.find("/tmp/") != std::string_view::npos ||
         value.find('\\') != std::string_view::npos ||
         value.find(".sock") != std::string_view::npos ||
         value.find(".sbdb") != std::string_view::npos;
}

std::string RedactSupportBundleString(std::string value) {
  if (value.empty() || !LooksSupportBundleProtected(value)) {
    return value;
  }
  if (value.find("/home/") != std::string::npos ||
      value.find("/tmp/") != std::string::npos ||
      value.find('\\') != std::string::npos ||
      value.find(".sock") != std::string::npos ||
      value.find(".sbdb") != std::string::npos) {
    return "[path-redacted]";
  }
  return "[redacted:security]";
}

void RedactSupportBundleStringField(std::string* value) {
  if (value != nullptr) {
    *value = RedactSupportBundleString(std::move(*value));
  }
}

PerformanceOptimizationConfigResolution RedactedConfigResolutionForSupportBundle(
    PerformanceOptimizationConfigResolution resolution) {
  for (auto& field : resolution.fields) {
    RedactSupportBundleStringField(&field.metadata.surface_name);
    RedactSupportBundleStringField(&field.metadata.value_type);
    RedactSupportBundleStringField(&field.metadata.packaged_default);
    RedactSupportBundleStringField(&field.metadata.config_key);
    RedactSupportBundleStringField(&field.metadata.env_var);
    RedactSupportBundleStringField(&field.metadata.cli_option);
    RedactSupportBundleStringField(&field.metadata.admin_override_operation);
    RedactSupportBundleStringField(&field.metadata.admin_override_key);
    RedactSupportBundleStringField(&field.metadata.management_view_field);
    RedactSupportBundleStringField(&field.metadata.support_bundle_field);
    RedactSupportBundleStringField(&field.metadata.disabled_behavior);
    RedactSupportBundleStringField(&field.configured_value);
    RedactSupportBundleStringField(&field.effective_value);
    RedactSupportBundleStringField(&field.value_source);
    RedactSupportBundleStringField(&field.precedence_order);
    RedactSupportBundleStringField(&field.override_refusal_code);
    RedactSupportBundleStringField(&field.override_refusal_reason);
    RedactSupportBundleStringField(&field.override_refusal_message_vector);
  }
  return resolution;
}

PerformanceOptimizationSurfaceSnapshot RedactedSnapshotForSupportBundle(
    PerformanceOptimizationSurfaceSnapshot snapshot) {
  RedactSupportBundleStringField(&snapshot.optimization_profile);
  RedactSupportBundleStringField(&snapshot.copy_batching_status);
  RedactSupportBundleStringField(&snapshot.native_ingest_state);
  RedactSupportBundleStringField(&snapshot.native_ingest_refusal_code);
  RedactSupportBundleStringField(&snapshot.plan_cache_last_invalidation_reason);
  RedactSupportBundleStringField(&snapshot.stale_statistics_fail_safe_reason);
  RedactSupportBundleStringField(&snapshot.selected_join_algorithm);
  RedactSupportBundleStringField(&snapshot.selected_join_plan_summary);
  RedactSupportBundleStringField(&snapshot.selected_join_statistics_version);
  RedactSupportBundleStringField(&snapshot.summary_prune_status);
  RedactSupportBundleStringField(&snapshot.summary_prune_last_reason);
  RedactSupportBundleStringField(&snapshot.summary_prune_fallback_reason);
  RedactSupportBundleStringField(&snapshot.summary_prune_summary_status);
  RedactSupportBundleStringField(&snapshot.summary_prune_authority_source);
  RedactSupportBundleStringField(&snapshot.cleanup_horizon_authority_status);
  RedactSupportBundleStringField(&snapshot.agent_worker_status);
  RedactSupportBundleStringField(&snapshot.last_agent_type_id);
  RedactSupportBundleStringField(&snapshot.last_agent_action);
  RedactSupportBundleStringField(&snapshot.last_agent_decision);
  RedactSupportBundleStringField(&snapshot.last_agent_diagnostic_code);
  RedactSupportBundleStringField(&snapshot.secondary_index_state);
  RedactSupportBundleStringField(&snapshot.shadow_index_state);
  RedactSupportBundleStringField(&snapshot.summary_index_state);
  RedactSupportBundleStringField(&snapshot.specialized_index_state);
  RedactSupportBundleStringField(&snapshot.index_state_authority_source);
  RedactSupportBundleStringField(&snapshot.resource_governor_state);
  RedactSupportBundleStringField(&snapshot.backpressure_state);
  RedactSupportBundleStringField(&snapshot.backpressure_reason);
  RedactSupportBundleStringField(&snapshot.benchmark_correlation_id);
  RedactSupportBundleStringField(&snapshot.support_bundle_correlation_id);
  RedactSupportBundleStringField(&snapshot.request_correlation_id);
  RedactSupportBundleStringField(&snapshot.exact_refusal_diagnostic_code);
  RedactSupportBundleStringField(&snapshot.exact_refusal_message_vector);
  RedactSupportBundleStringField(&snapshot.exact_refusal_source);
  RedactSupportBundleStringField(&snapshot.metric_family);
  RedactSupportBundleStringField(&snapshot.audit_event_family);
  RedactSupportBundleStringField(&snapshot.audit_last_decision);
  RedactSupportBundleStringField(&snapshot.support_bundle_redaction_state);
  RedactSupportBundleStringField(&snapshot.support_bundle_completeness_state);

  for (auto& path : snapshot.odf108_selected_paths) {
    RedactSupportBundleStringField(&path.path_id);
    RedactSupportBundleStringField(&path.path_kind);
    RedactSupportBundleStringField(&path.selected_path);
    RedactSupportBundleStringField(&path.path_state);
    RedactSupportBundleStringField(&path.authority_source);
    RedactSupportBundleStringField(&path.fallback_reason);
  }
  for (auto& gate : snapshot.odf108_feature_gates) {
    RedactSupportBundleStringField(&gate.gate_id);
    RedactSupportBundleStringField(&gate.gate_state);
    RedactSupportBundleStringField(&gate.authority_source);
    RedactSupportBundleStringField(&gate.refusal_code);
  }
  for (auto& fallback : snapshot.odf108_fallbacks) {
    RedactSupportBundleStringField(&fallback.route_id);
    RedactSupportBundleStringField(&fallback.reason_code);
    RedactSupportBundleStringField(&fallback.detail);
    RedactSupportBundleStringField(&fallback.scalar_fallback_state);
  }
  for (auto& quota : snapshot.odf108_quotas) {
    RedactSupportBundleStringField(&quota.quota_family);
    RedactSupportBundleStringField(&quota.quota_state);
    RedactSupportBundleStringField(&quota.action);
    RedactSupportBundleStringField(&quota.diagnostic_code);
  }
  for (auto& compatibility : snapshot.odf108_runtime_compatibility) {
    RedactSupportBundleStringField(&compatibility.route_id);
    RedactSupportBundleStringField(&compatibility.compatibility_status);
    RedactSupportBundleStringField(&compatibility.compatibility_action);
    RedactSupportBundleStringField(&compatibility.required_capabilities);
    RedactSupportBundleStringField(&compatibility.provider_capabilities);
    RedactSupportBundleStringField(&compatibility.diagnostic_code);
    RedactSupportBundleStringField(&compatibility.fallback_reason);
  }
  for (auto& rebuild : snapshot.odf108_rebuild_states) {
    RedactSupportBundleStringField(&rebuild.object_kind);
    RedactSupportBundleStringField(&rebuild.rebuild_state);
    RedactSupportBundleStringField(&rebuild.rebuild_phase);
    RedactSupportBundleStringField(&rebuild.refusal_code);
  }
  for (auto& refusal : snapshot.odf108_exact_refusals) {
    RedactSupportBundleStringField(&refusal.source);
    RedactSupportBundleStringField(&refusal.diagnostic_code);
    RedactSupportBundleStringField(&refusal.message_vector);
    RedactSupportBundleStringField(&refusal.refusal_state);
    RedactSupportBundleStringField(&refusal.redaction_state);
  }
  return snapshot;
}

void AddConfigEffectiveValueJson(
    std::ostringstream* out,
    const PerformanceOptimizationConfigEffectiveValue& field) {
  bool first = true;
  *out << '{';
  AddJsonString(out, &first, "surface_name", field.metadata.surface_name);
  AddJsonString(out, &first, "value_type", field.metadata.value_type);
  AddJsonString(out,
                &first,
                "packaged_default",
                field.metadata.packaged_default);
  AddJsonString(out, &first, "config_key", field.metadata.config_key);
  AddJsonString(out, &first, "env_var", field.metadata.env_var);
  AddJsonString(out, &first, "cli_option", field.metadata.cli_option);
  AddJsonString(out,
                &first,
                "admin_override_operation",
                field.metadata.admin_override_operation);
  AddJsonString(out,
                &first,
                "admin_override_key",
                field.metadata.admin_override_key);
  AddJsonString(out,
                &first,
                "management_view_field",
                field.metadata.management_view_field);
  AddJsonString(out,
                &first,
                "support_bundle_field",
                field.metadata.support_bundle_field);
  AddJsonString(out,
                &first,
                "disabled_behavior",
                field.metadata.disabled_behavior);
  AddJsonString(out, &first, "configured_value", field.configured_value);
  AddJsonString(out, &first, "effective_value", field.effective_value);
  AddJsonString(out, &first, "value_source", field.value_source);
  AddJsonString(out, &first, "precedence_order", field.precedence_order);
  AddJsonString(out,
                &first,
                "override_refusal_code",
                field.override_refusal_code);
  AddJsonString(out,
                &first,
                "override_refusal_reason",
                field.override_refusal_reason);
  AddJsonString(out,
                &first,
                "override_refusal_message_vector",
                field.override_refusal_message_vector);
  *out << '}';
}

bool ContainsOdf108ForbiddenToken(std::string_view value) {
  for (const auto token :
       {"docs/", "execution-plans", "findings", "contracts", "references",
        "parser_finality_authority=true", "donor_finality_authority=true",
        "client_finality_authority=true",
        "storage_shortcut_finality_authority=true",
        "wal_recovery_authority=true", "write_ahead_log"}) {
    if (value.find(token) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

void RequireSafeOdf108Text(const std::string& value,
                           const std::string& field,
                           std::vector<std::string>* missing) {
  RequireString(value, field, missing);
  if (ContainsOdf108ForbiddenToken(value)) {
    AddMissing(missing, field + ":forbidden_runtime_token");
  }
}

void AddJsonSelectedPath(std::ostringstream* out,
                         const PerformanceOptimizationSelectedPathSurface& path) {
  bool first = true;
  *out << '{';
  AddJsonString(out, &first, "path_id", path.path_id);
  AddJsonString(out, &first, "path_kind", path.path_kind);
  AddJsonString(out, &first, "selected_path", path.selected_path);
  AddJsonString(out, &first, "path_state", path.path_state);
  AddJsonString(out, &first, "authority_source", path.authority_source);
  AddJsonString(out, &first, "fallback_reason", path.fallback_reason);
  *out << '}';
}

void AddJsonFeatureGate(std::ostringstream* out,
                        const PerformanceOptimizationFeatureGateSurface& gate) {
  bool first = true;
  *out << '{';
  AddJsonString(out, &first, "gate_id", gate.gate_id);
  AddJsonBool(out, &first, "enabled", gate.enabled);
  AddJsonString(out, &first, "gate_state", gate.gate_state);
  AddJsonString(out, &first, "authority_source", gate.authority_source);
  AddJsonString(out, &first, "refusal_code", gate.refusal_code);
  *out << '}';
}

void AddJsonFallback(std::ostringstream* out,
                     const PerformanceOptimizationFallbackSurface& fallback) {
  bool first = true;
  *out << '{';
  AddJsonString(out, &first, "route_id", fallback.route_id);
  AddJsonString(out, &first, "reason_code", fallback.reason_code);
  AddJsonString(out, &first, "detail", fallback.detail);
  AddJsonString(out,
                &first,
                "scalar_fallback_state",
                fallback.scalar_fallback_state);
  *out << '}';
}

void AddJsonQuota(std::ostringstream* out,
                  const PerformanceOptimizationQuotaSurface& quota) {
  bool first = true;
  *out << '{';
  AddJsonString(out, &first, "quota_family", quota.quota_family);
  AddJsonString(out, &first, "quota_state", quota.quota_state);
  AddJsonU64(out, &first, "quota_limit", quota.quota_limit);
  AddJsonU64(out, &first, "quota_requested", quota.quota_requested);
  AddJsonU64(out, &first, "quota_in_use", quota.quota_in_use);
  AddJsonU64(out, &first, "grants", quota.grants);
  AddJsonU64(out, &first, "refusals", quota.refusals);
  AddJsonString(out, &first, "action", quota.action);
  AddJsonString(out, &first, "diagnostic_code", quota.diagnostic_code);
  *out << '}';
}

void AddJsonRuntimeCompatibility(
    std::ostringstream* out,
    const PerformanceOptimizationRuntimeCompatibilitySurface& compatibility) {
  bool first = true;
  *out << '{';
  AddJsonString(out, &first, "route_id", compatibility.route_id);
  AddJsonString(out,
                &first,
                "compatibility_status",
                compatibility.compatibility_status);
  AddJsonString(out,
                &first,
                "compatibility_action",
                compatibility.compatibility_action);
  AddJsonU64(out,
             &first,
             "runtime_generation",
             compatibility.runtime_generation);
  AddJsonString(out,
                &first,
                "required_capabilities",
                compatibility.required_capabilities);
  AddJsonString(out,
                &first,
                "provider_capabilities",
                compatibility.provider_capabilities);
  AddJsonString(out, &first, "diagnostic_code", compatibility.diagnostic_code);
  AddJsonString(out, &first, "fallback_reason", compatibility.fallback_reason);
  *out << '}';
}

void AddJsonRebuildState(std::ostringstream* out,
                         const PerformanceOptimizationRebuildSurface& rebuild) {
  bool first = true;
  *out << '{';
  AddJsonString(out, &first, "object_kind", rebuild.object_kind);
  AddJsonString(out, &first, "rebuild_state", rebuild.rebuild_state);
  AddJsonString(out, &first, "rebuild_phase", rebuild.rebuild_phase);
  AddJsonU64(out, &first, "generation", rebuild.generation);
  AddJsonU64(out, &first, "progress_numerator", rebuild.progress_numerator);
  AddJsonU64(out, &first, "progress_denominator", rebuild.progress_denominator);
  AddJsonString(out, &first, "refusal_code", rebuild.refusal_code);
  *out << '}';
}

void AddJsonExactRefusal(
    std::ostringstream* out,
    const PerformanceOptimizationExactRefusalSurface& refusal) {
  bool first = true;
  *out << '{';
  AddJsonString(out, &first, "source", refusal.source);
  AddJsonString(out, &first, "diagnostic_code", refusal.diagnostic_code);
  AddJsonString(out, &first, "message_vector", refusal.message_vector);
  AddJsonString(out, &first, "refusal_state", refusal.refusal_state);
  AddJsonString(out, &first, "redaction_state", refusal.redaction_state);
  AddJsonBool(out, &first, "public_safe", refusal.public_safe);
  *out << '}';
}

template <typename T, typename TWriter>
void AddJsonArray(std::ostringstream* out,
                  bool* first,
                  std::string_view key,
                  const std::vector<T>& items,
                  TWriter writer) {
  Comma(out, first);
  *out << '"' << key << "\":[";
  bool first_item = true;
  for (const auto& item : items) {
    if (!first_item) {
      *out << ',';
    }
    first_item = false;
    writer(out, item);
  }
  *out << ']';
}

void AddOdf108SurfaceRows(
    EngineApiResult* result,
    const PerformanceOptimizationSurfaceSnapshot& snapshot) {
  for (const auto& path : snapshot.odf108_selected_paths) {
    AddApiBehaviorRow(result,
                      {{"record_kind", "odf108_selected_path"},
                       {"path_id", path.path_id},
                       {"path_kind", path.path_kind},
                       {"selected_path", path.selected_path},
                       {"path_state", path.path_state},
                       {"authority_source", path.authority_source},
                       {"fallback_reason", path.fallback_reason}});
  }
  for (const auto& gate : snapshot.odf108_feature_gates) {
    AddApiBehaviorRow(result,
                      {{"record_kind", "odf108_feature_gate"},
                       {"gate_id", gate.gate_id},
                       {"enabled", BoolText(gate.enabled)},
                       {"gate_state", gate.gate_state},
                       {"authority_source", gate.authority_source},
                       {"refusal_code", gate.refusal_code}});
  }
  for (const auto& fallback : snapshot.odf108_fallbacks) {
    AddApiBehaviorRow(result,
                      {{"record_kind", "odf108_fallback_reason"},
                       {"route_id", fallback.route_id},
                       {"reason_code", fallback.reason_code},
                       {"detail", fallback.detail},
                       {"scalar_fallback_state",
                        fallback.scalar_fallback_state}});
  }
  for (const auto& quota : snapshot.odf108_quotas) {
    AddApiBehaviorRow(result,
                      {{"record_kind", "odf108_quota_state"},
                       {"quota_family", quota.quota_family},
                       {"quota_state", quota.quota_state},
                       {"quota_limit", std::to_string(quota.quota_limit)},
                       {"quota_requested", std::to_string(quota.quota_requested)},
                       {"quota_in_use", std::to_string(quota.quota_in_use)},
                       {"grants", std::to_string(quota.grants)},
                       {"refusals", std::to_string(quota.refusals)},
                       {"action", quota.action},
                       {"diagnostic_code", quota.diagnostic_code}});
  }
  for (const auto& compatibility : snapshot.odf108_runtime_compatibility) {
    AddApiBehaviorRow(result,
                      {{"record_kind", "odf108_runtime_compatibility"},
                       {"route_id", compatibility.route_id},
                       {"compatibility_status",
                        compatibility.compatibility_status},
                       {"compatibility_action",
                        compatibility.compatibility_action},
                       {"runtime_generation",
                        std::to_string(compatibility.runtime_generation)},
                       {"required_capabilities",
                        compatibility.required_capabilities},
                       {"provider_capabilities",
                        compatibility.provider_capabilities},
                       {"diagnostic_code", compatibility.diagnostic_code},
                       {"fallback_reason", compatibility.fallback_reason}});
  }
  for (const auto& rebuild : snapshot.odf108_rebuild_states) {
    AddApiBehaviorRow(result,
                      {{"record_kind", "odf108_rebuild_state"},
                       {"object_kind", rebuild.object_kind},
                       {"rebuild_state", rebuild.rebuild_state},
                       {"rebuild_phase", rebuild.rebuild_phase},
                       {"generation", std::to_string(rebuild.generation)},
                       {"progress_numerator",
                        std::to_string(rebuild.progress_numerator)},
                       {"progress_denominator",
                        std::to_string(rebuild.progress_denominator)},
                       {"refusal_code", rebuild.refusal_code}});
  }
  for (const auto& refusal : snapshot.odf108_exact_refusals) {
    AddApiBehaviorRow(result,
                      {{"record_kind", "odf108_exact_refusal"},
                       {"source", refusal.source},
                       {"diagnostic_code", refusal.diagnostic_code},
                       {"message_vector", refusal.message_vector},
                       {"refusal_state", refusal.refusal_state},
                       {"redaction_state", refusal.redaction_state},
                       {"public_safe", BoolText(refusal.public_safe)}});
  }
}

}  // namespace

void AddPerformanceOptimizationSurfaceRow(
    EngineApiResult* result,
    const PerformanceOptimizationSurfaceSnapshot& snapshot) {
  AddApiBehaviorRow(result,
                    {{"schema_id", PerformanceOptimizationSurfaceSchemaId()},
                     {"schema_version", std::to_string(PerformanceOptimizationSurfaceSchemaVersion())},
                     {"optimization_profile", snapshot.optimization_profile},
                     {"optimizer_enabled", BoolText(snapshot.optimizer_enabled)},
                     {"copy_append_batching_enabled", BoolText(snapshot.copy_append_batching_enabled)},
                     {"native_ingest_enabled", BoolText(snapshot.native_ingest_enabled)},
                     {"plan_cache_enabled", BoolText(snapshot.plan_cache_enabled)},
                     {"descriptor_metadata_cache_enabled",
                      BoolText(snapshot.descriptor_metadata_cache_enabled)},
                     {"statistics_enabled", BoolText(snapshot.statistics_enabled)},
                     {"summary_prune_enabled", BoolText(snapshot.summary_prune_enabled)},
                     {"agent_workers_enabled", BoolText(snapshot.agent_workers_enabled)},
                     {"resource_governor_enabled", BoolText(snapshot.resource_governor_enabled)},
                     {"page_filespace_preallocation_enabled",
                      BoolText(snapshot.page_filespace_preallocation_enabled)},
                     {"cancellation_enabled", BoolText(snapshot.cancellation_enabled)},
                     {"backpressure_enabled", BoolText(snapshot.backpressure_enabled)},
                     {"copy_batching_status", snapshot.copy_batching_status},
                     {"copy_batch_rows_configured",
                      std::to_string(snapshot.copy_batch_rows_configured)},
                     {"copy_batches_started", std::to_string(snapshot.copy_batches_started)},
                     {"copy_batches_completed", std::to_string(snapshot.copy_batches_completed)},
                     {"copy_rows_batched", std::to_string(snapshot.copy_rows_batched)},
                     {"copy_singleton_fallback_batches",
                      std::to_string(snapshot.copy_singleton_fallback_batches)},
                     {"native_ingest_state", snapshot.native_ingest_state},
                     {"native_ingest_rows_processed",
                      std::to_string(snapshot.native_ingest_rows_processed)},
                     {"native_ingest_rows_total", std::to_string(snapshot.native_ingest_rows_total)},
                     {"native_ingest_refused", BoolText(snapshot.native_ingest_refused)},
                     {"native_ingest_refusal_code", snapshot.native_ingest_refusal_code},
                     {"plan_cache_hits", std::to_string(snapshot.plan_cache_hits)},
                     {"plan_cache_misses", std::to_string(snapshot.plan_cache_misses)},
                     {"plan_cache_invalidations",
                      std::to_string(snapshot.plan_cache_invalidations)},
                     {"plan_cache_last_invalidation_reason",
                      snapshot.plan_cache_last_invalidation_reason},
                     {"descriptor_metadata_cache_hits",
                      std::to_string(snapshot.descriptor_metadata_cache_hits)},
                     {"descriptor_metadata_cache_misses",
                      std::to_string(snapshot.descriptor_metadata_cache_misses)},
                     {"descriptor_metadata_cache_epoch",
                      std::to_string(snapshot.descriptor_metadata_cache_epoch)},
                     {"statistics_epoch", std::to_string(snapshot.statistics_epoch)},
                     {"stale_statistics_fail_safe_active",
                      BoolText(snapshot.stale_statistics_fail_safe_active)},
                     {"stale_statistics_fail_safe_reason",
                      snapshot.stale_statistics_fail_safe_reason},
                     {"catalog_generation_id",
                      std::to_string(snapshot.catalog_generation_id)},
                     {"name_resolution_epoch",
                      std::to_string(snapshot.name_resolution_epoch)},
                     {"security_epoch", std::to_string(snapshot.security_epoch)},
                     {"resource_epoch", std::to_string(snapshot.resource_epoch)},
                     {"optimization_state_epoch",
                      std::to_string(snapshot.optimization_state_epoch)},
                     {"selected_join_algorithm", snapshot.selected_join_algorithm},
                     {"selected_join_plan_summary", snapshot.selected_join_plan_summary},
                     {"selected_join_left_rows",
                      std::to_string(snapshot.selected_join_left_rows)},
                     {"selected_join_right_rows",
                      std::to_string(snapshot.selected_join_right_rows)},
                     {"selected_join_from_statistics",
                      BoolText(snapshot.selected_join_from_statistics)},
                     {"selected_join_statistics_version",
                      snapshot.selected_join_statistics_version},
                     {"summary_prune_status", snapshot.summary_prune_status},
                     {"summary_prune_last_reason", snapshot.summary_prune_last_reason},
                     {"summary_prune_fallback_reason",
                      snapshot.summary_prune_fallback_reason},
                     {"summary_prune_summary_status",
                      snapshot.summary_prune_summary_status},
                     {"summary_prune_generation",
                      std::to_string(snapshot.summary_prune_generation)},
                     {"summary_prune_ranges_considered",
                      std::to_string(snapshot.summary_prune_ranges_considered)},
                     {"summary_prune_ranges_pruned",
                      std::to_string(snapshot.summary_prune_ranges_pruned)},
                     {"summary_prune_ranges_scanned",
                      std::to_string(snapshot.summary_prune_ranges_scanned)},
                     {"summary_prune_pages_considered",
                      std::to_string(snapshot.summary_prune_pages_considered)},
                     {"summary_prune_pages_pruned",
                      std::to_string(snapshot.summary_prune_pages_pruned)},
                     {"summary_prune_pages_scanned",
                      std::to_string(snapshot.summary_prune_pages_scanned)},
                     {"summary_prune_authority_source",
                      snapshot.summary_prune_authority_source},
                     {"summary_prune_base_row_mga_recheck_required",
                      BoolText(snapshot.summary_prune_base_row_mga_recheck_required)},
                     {"summary_prune_base_row_security_recheck_required",
                      BoolText(snapshot.summary_prune_base_row_security_recheck_required)},
                     {"cleanup_horizon_authority_status",
                      snapshot.cleanup_horizon_authority_status},
                     {"cleanup_horizon_authoritative",
                      BoolText(snapshot.cleanup_horizon_authoritative)},
                     {"cleanup_horizon_local_transaction_id",
                      std::to_string(snapshot.cleanup_horizon_local_transaction_id)},
                     {"oldest_interesting_transaction_id",
                      std::to_string(snapshot.oldest_interesting_transaction_id)},
                     {"oldest_active_transaction_id",
                      std::to_string(snapshot.oldest_active_transaction_id)},
                     {"oldest_snapshot_transaction_id",
                      std::to_string(snapshot.oldest_snapshot_transaction_id)},
                     {"oldest_cleanup_transaction_id",
                      std::to_string(snapshot.oldest_cleanup_transaction_id)},
                     {"agent_worker_status", snapshot.agent_worker_status},
                     {"agent_worker_thread_count",
                      std::to_string(snapshot.agent_worker_thread_count)},
                     {"agent_worker_actions_accepted",
                      std::to_string(snapshot.agent_worker_actions_accepted)},
                     {"agent_worker_actions_refused",
                      std::to_string(snapshot.agent_worker_actions_refused)},
                     {"agent_work_backlog_count",
                      std::to_string(snapshot.agent_work_backlog_count)},
                     {"last_agent_type_id", snapshot.last_agent_type_id},
                     {"last_agent_action", snapshot.last_agent_action},
                     {"last_agent_decision", snapshot.last_agent_decision},
                     {"last_agent_diagnostic_code",
                      snapshot.last_agent_diagnostic_code},
                     {"storage_row_version_backlog_count",
                      std::to_string(snapshot.storage_row_version_backlog_count)},
                     {"index_delta_backlog_count",
                      std::to_string(snapshot.index_delta_backlog_count)},
                     {"index_garbage_backlog_count",
                      std::to_string(snapshot.index_garbage_backlog_count)},
                     {"page_summary_backlog_count",
                      std::to_string(snapshot.page_summary_backlog_count)},
                     {"secondary_index_state", snapshot.secondary_index_state},
                     {"shadow_index_state", snapshot.shadow_index_state},
                     {"summary_index_state", snapshot.summary_index_state},
                     {"specialized_index_state", snapshot.specialized_index_state},
                     {"index_state_authority_source",
                      snapshot.index_state_authority_source},
                     {"resource_governor_state", snapshot.resource_governor_state},
                     {"resource_quota_grants", std::to_string(snapshot.resource_quota_grants)},
                     {"resource_quota_refusals",
                      std::to_string(snapshot.resource_quota_refusals)},
                     {"resource_quota_in_use", std::to_string(snapshot.resource_quota_in_use)},
                     {"page_preallocation_demand_pages",
                      std::to_string(snapshot.page_preallocation_demand_pages)},
                     {"page_preallocation_granted_pages",
                      std::to_string(snapshot.page_preallocation_granted_pages)},
                     {"filespace_preallocation_demand_pages",
                      std::to_string(snapshot.filespace_preallocation_demand_pages)},
                     {"filespace_preallocation_granted_pages",
                      std::to_string(snapshot.filespace_preallocation_granted_pages)},
                     {"preallocation_refusal_count",
                      std::to_string(snapshot.preallocation_refusal_count)},
                     {"cancellation_requested", BoolText(snapshot.cancellation_requested)},
                     {"backpressure_active", BoolText(snapshot.backpressure_active)},
                     {"backpressure_state", snapshot.backpressure_state},
                     {"backpressure_reason", snapshot.backpressure_reason},
                     {"cancellation_checkpoint_count",
                      std::to_string(snapshot.cancellation_checkpoint_count)},
                     {"backpressure_deferral_count",
                     std::to_string(snapshot.backpressure_deferral_count)},
                     {"benchmark_correlation_id", snapshot.benchmark_correlation_id},
                     {"support_bundle_correlation_id", snapshot.support_bundle_correlation_id},
                     {"request_correlation_id", snapshot.request_correlation_id},
                     {"exact_refusal_diagnostic_code",
                      snapshot.exact_refusal_diagnostic_code},
                     {"exact_refusal_message_vector",
                      snapshot.exact_refusal_message_vector},
                     {"exact_refusal_source", snapshot.exact_refusal_source},
                     {"message_vector_ready", BoolText(snapshot.message_vector_ready)},
                     {"metric_family", snapshot.metric_family},
                     {"metric_sample_count",
                      std::to_string(snapshot.metric_sample_count)},
                     {"audit_event_family", snapshot.audit_event_family},
                     {"audit_event_count",
                      std::to_string(snapshot.audit_event_count)},
                     {"audit_last_decision", snapshot.audit_last_decision},
                     {"support_bundle_redaction_state",
                      snapshot.support_bundle_redaction_state},
                     {"support_bundle_completeness_state",
                      snapshot.support_bundle_completeness_state},
                     {"support_bundle_forbidden_fields_absent",
                      BoolText(snapshot.support_bundle_forbidden_fields_absent)},
                     {"odf108_surface_ready", "true"},
                     {"odf108_selected_path_count",
                      std::to_string(snapshot.odf108_selected_paths.size())},
                     {"odf108_feature_gate_count",
                      std::to_string(snapshot.odf108_feature_gates.size())},
                     {"odf108_fallback_reason_count",
                      std::to_string(snapshot.odf108_fallbacks.size())},
                     {"odf108_quota_state_count",
                      std::to_string(snapshot.odf108_quotas.size())},
                     {"odf108_runtime_compatibility_count",
                      std::to_string(
                          snapshot.odf108_runtime_compatibility.size())},
                     {"odf108_rebuild_state_count",
                      std::to_string(snapshot.odf108_rebuild_states.size())},
                     {"odf108_exact_refusal_count",
                      std::to_string(snapshot.odf108_exact_refusals.size())},
                     {"odf108_selected_path",
                      snapshot.odf108_selected_paths.empty()
                          ? "missing"
                          : snapshot.odf108_selected_paths.front()
                                .selected_path},
                     {"odf108_feature_gate",
                      snapshot.odf108_feature_gates.empty()
                          ? "missing"
                          : snapshot.odf108_feature_gates.front().gate_id},
                     {"odf108_fallback_reason",
                      snapshot.odf108_fallbacks.empty()
                          ? "missing"
                          : snapshot.odf108_fallbacks.front().reason_code},
                     {"odf108_quota_state",
                      snapshot.odf108_quotas.empty()
                          ? "missing"
                          : snapshot.odf108_quotas.front().quota_state},
                     {"odf108_runtime_compatibility_status",
                      snapshot.odf108_runtime_compatibility.empty()
                          ? "missing"
                          : snapshot.odf108_runtime_compatibility.front()
                                .compatibility_status},
                     {"odf108_rebuild_state",
                      snapshot.odf108_rebuild_states.empty()
                          ? "missing"
                          : snapshot.odf108_rebuild_states.front()
                                .rebuild_state},
                     {"odf108_exact_refusal_state",
                      snapshot.odf108_exact_refusals.empty()
                          ? "missing"
                          : snapshot.odf108_exact_refusals.front()
                                .refusal_state},
                     {"config_defaults_packaging_ready", "true"},
                     {"config_precedence_order",
                      std::string(kPerformanceConfigPrecedence)},
                     {"parser_finality_authority",
                      BoolText(snapshot.parser_finality_authority)},
                     {"donor_finality_authority",
                      BoolText(snapshot.donor_finality_authority)},
                     {"client_finality_authority",
                      BoolText(snapshot.client_finality_authority)},
                     {"storage_shortcut_finality_authority",
                      BoolText(snapshot.storage_shortcut_finality_authority)},
                     {"wal_recovery_authority",
                      BoolText(snapshot.wal_recovery_authority)},
                     {"catalog_uuid_authority", "engine"}});
  AddOdf108SurfaceRows(result, snapshot);
}

std::string ManagementJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot,
    const PerformanceOptimizationConfigResolution& config_resolution) {
  return "{\"management_api\":{\"surface\":\"performance_optimization\","
         "\"snapshot\":" +
         SerializePerformanceOptimizationSurfaceJson(snapshot) + ","
         "\"config_defaults\":" +
         SerializePerformanceOptimizationConfigResolutionJson(config_resolution) +
         "}}";
}

std::string SupportBundleJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot,
    const PerformanceOptimizationConfigResolution& config_resolution) {
  return "{\"support_bundle\":{\"section\":\"performance_optimization_surface\","
         "\"redaction_state\":\"" +
         JsonEscape(snapshot.support_bundle_redaction_state) + "\","
         "\"completeness_state\":\"" +
         JsonEscape(snapshot.support_bundle_completeness_state) + "\","
         "\"forbidden_fields_absent\":" +
         BoolText(snapshot.support_bundle_forbidden_fields_absent) + ","
         "\"snapshot\":" +
         SerializePerformanceOptimizationSurfaceJson(snapshot) + ","
         "\"config_defaults\":" +
         SerializePerformanceOptimizationConfigResolutionJson(config_resolution) +
         "}}";
}

EngineApiU64 PerformanceOptimizationSurfaceSchemaVersion() {
  return 1;
}

const char* PerformanceOptimizationSurfaceSchemaId() {
  return "scratchbird.performance_optimization_surface.v1";
}

const std::vector<PerformanceOptimizationSurfaceField>&
PerformanceOptimizationSurfaceSchema() {
  static const std::vector<PerformanceOptimizationSurfaceField> kSchema = {
      {"optimization_profile", "text", true, "flags"},
      {"optimizer_enabled", "bool", true, "flags"},
      {"copy_append_batching_enabled", "bool", true, "flags"},
      {"native_ingest_enabled", "bool", true, "flags"},
      {"plan_cache_enabled", "bool", true, "flags"},
      {"descriptor_metadata_cache_enabled", "bool", true, "flags"},
      {"statistics_enabled", "bool", true, "flags"},
      {"summary_prune_enabled", "bool", true, "flags"},
      {"agent_workers_enabled", "bool", true, "flags"},
      {"resource_governor_enabled", "bool", true, "flags"},
      {"page_filespace_preallocation_enabled", "bool", true, "flags"},
      {"cancellation_enabled", "bool", true, "flags"},
      {"backpressure_enabled", "bool", true, "flags"},
      {"copy_batching_status", "text", true, "copy"},
      {"copy_batch_rows_configured", "uint64", true, "copy"},
      {"copy_batches_started", "uint64", true, "copy"},
      {"copy_batches_completed", "uint64", true, "copy"},
      {"copy_rows_batched", "uint64", true, "copy"},
      {"copy_singleton_fallback_batches", "uint64", true, "copy"},
      {"native_ingest_state", "text", true, "native_ingest"},
      {"native_ingest_rows_processed", "uint64", true, "native_ingest"},
      {"native_ingest_rows_total", "uint64", true, "native_ingest"},
      {"native_ingest_refused", "bool", true, "native_ingest"},
      {"native_ingest_refusal_code", "text", true, "native_ingest"},
      {"plan_cache_hits", "uint64", true, "plan_cache"},
      {"plan_cache_misses", "uint64", true, "plan_cache"},
      {"plan_cache_invalidations", "uint64", true, "plan_cache"},
      {"plan_cache_last_invalidation_reason", "text", true, "plan_cache"},
      {"descriptor_metadata_cache_hits", "uint64", true, "metadata_cache"},
      {"descriptor_metadata_cache_misses", "uint64", true, "metadata_cache"},
      {"descriptor_metadata_cache_epoch", "uint64", true, "metadata_cache"},
      {"statistics_epoch", "uint64", true, "statistics"},
      {"stale_statistics_fail_safe_active", "bool", true, "statistics"},
      {"stale_statistics_fail_safe_reason", "text", true, "statistics"},
      {"catalog_generation_id", "uint64", true, "epochs"},
      {"name_resolution_epoch", "uint64", true, "epochs"},
      {"security_epoch", "uint64", true, "epochs"},
      {"resource_epoch", "uint64", true, "epochs"},
      {"optimization_state_epoch", "uint64", true, "epochs"},
      {"selected_join_algorithm", "text", true, "planner"},
      {"selected_join_plan_summary", "text", true, "planner"},
      {"selected_join_left_rows", "uint64", true, "planner"},
      {"selected_join_right_rows", "uint64", true, "planner"},
      {"selected_join_from_statistics", "bool", true, "planner"},
      {"selected_join_statistics_version", "text", true, "planner"},
      {"summary_prune_status", "text", true, "summary_prune"},
      {"summary_prune_last_reason", "text", true, "summary_prune"},
      {"summary_prune_fallback_reason", "text", true, "summary_prune"},
      {"summary_prune_summary_status", "text", true, "summary_prune"},
      {"summary_prune_generation", "uint64", true, "summary_prune"},
      {"summary_prune_ranges_considered", "uint64", true, "summary_prune"},
      {"summary_prune_ranges_pruned", "uint64", true, "summary_prune"},
      {"summary_prune_ranges_scanned", "uint64", true, "summary_prune"},
      {"summary_prune_pages_considered", "uint64", true, "summary_prune"},
      {"summary_prune_pages_pruned", "uint64", true, "summary_prune"},
      {"summary_prune_pages_scanned", "uint64", true, "summary_prune"},
      {"summary_prune_authority_source", "text", true, "summary_prune"},
      {"summary_prune_base_row_mga_recheck_required", "bool", true, "summary_prune"},
      {"summary_prune_base_row_security_recheck_required", "bool", true, "summary_prune"},
      {"cleanup_horizon_authority_status", "text", true, "mga_horizons"},
      {"cleanup_horizon_authoritative", "bool", true, "mga_horizons"},
      {"cleanup_horizon_local_transaction_id", "uint64", true, "mga_horizons"},
      {"oldest_interesting_transaction_id", "uint64", true, "mga_horizons"},
      {"oldest_active_transaction_id", "uint64", true, "mga_horizons"},
      {"oldest_snapshot_transaction_id", "uint64", true, "mga_horizons"},
      {"oldest_cleanup_transaction_id", "uint64", true, "mga_horizons"},
      {"agent_worker_status", "text", true, "agents"},
      {"agent_worker_thread_count", "uint64", true, "agents"},
      {"agent_worker_actions_accepted", "uint64", true, "agents"},
      {"agent_worker_actions_refused", "uint64", true, "agents"},
      {"agent_work_backlog_count", "uint64", true, "backlogs"},
      {"last_agent_type_id", "text", true, "agent_decisions"},
      {"last_agent_action", "text", true, "agent_decisions"},
      {"last_agent_decision", "text", true, "agent_decisions"},
      {"last_agent_diagnostic_code", "text", true, "agent_decisions"},
      {"storage_row_version_backlog_count", "uint64", true, "backlogs"},
      {"index_delta_backlog_count", "uint64", true, "backlogs"},
      {"index_garbage_backlog_count", "uint64", true, "backlogs"},
      {"page_summary_backlog_count", "uint64", true, "backlogs"},
      {"secondary_index_state", "text", true, "index_states"},
      {"shadow_index_state", "text", true, "index_states"},
      {"summary_index_state", "text", true, "index_states"},
      {"specialized_index_state", "text", true, "index_states"},
      {"index_state_authority_source", "text", true, "index_states"},
      {"resource_governor_state", "text", true, "resource_governor"},
      {"resource_quota_grants", "uint64", true, "resource_governor"},
      {"resource_quota_refusals", "uint64", true, "resource_governor"},
      {"resource_quota_in_use", "uint64", true, "resource_governor"},
      {"page_preallocation_demand_pages", "uint64", true, "preallocation"},
      {"page_preallocation_granted_pages", "uint64", true, "preallocation"},
      {"filespace_preallocation_demand_pages", "uint64", true, "preallocation"},
      {"filespace_preallocation_granted_pages", "uint64", true, "preallocation"},
      {"preallocation_refusal_count", "uint64", true, "preallocation"},
      {"cancellation_requested", "bool", true, "cancellation_backpressure"},
      {"backpressure_active", "bool", true, "cancellation_backpressure"},
      {"backpressure_state", "text", true, "cancellation_backpressure"},
      {"backpressure_reason", "text", true, "cancellation_backpressure"},
      {"cancellation_checkpoint_count", "uint64", true, "cancellation_backpressure"},
      {"backpressure_deferral_count", "uint64", true, "cancellation_backpressure"},
      {"benchmark_correlation_id", "text", true, "correlation"},
      {"support_bundle_correlation_id", "text", true, "correlation"},
      {"request_correlation_id", "text", true, "correlation"},
      {"exact_refusal_diagnostic_code", "text", true, "message_vectors"},
      {"exact_refusal_message_vector", "text", true, "message_vectors"},
      {"exact_refusal_source", "text", true, "message_vectors"},
      {"message_vector_ready", "bool", true, "message_vectors"},
      {"metric_family", "text", true, "metrics"},
      {"metric_sample_count", "uint64", true, "metrics"},
      {"audit_event_family", "text", true, "audit"},
      {"audit_event_count", "uint64", true, "audit"},
      {"audit_last_decision", "text", true, "audit"},
      {"support_bundle_redaction_state", "text", true, "support_bundle"},
      {"support_bundle_completeness_state", "text", true, "support_bundle"},
      {"support_bundle_forbidden_fields_absent", "bool", true, "support_bundle"},
      {"odf108_surface_ready", "bool", true, "odf108_management_explain_support"},
      {"odf108_selected_path_count", "uint64", true, "odf108_management_explain_support"},
      {"odf108_feature_gate_count", "uint64", true, "odf108_management_explain_support"},
      {"odf108_fallback_reason_count", "uint64", true, "odf108_management_explain_support"},
      {"odf108_quota_state_count", "uint64", true, "odf108_management_explain_support"},
      {"odf108_runtime_compatibility_count", "uint64", true, "odf108_management_explain_support"},
      {"odf108_rebuild_state_count", "uint64", true, "odf108_management_explain_support"},
      {"odf108_exact_refusal_count", "uint64", true, "odf108_management_explain_support"},
      {"odf108_selected_path", "text", true, "odf108_management_explain_support"},
      {"odf108_feature_gate", "text", true, "odf108_management_explain_support"},
      {"odf108_fallback_reason", "text", true, "odf108_management_explain_support"},
      {"odf108_quota_state", "text", true, "odf108_management_explain_support"},
      {"odf108_runtime_compatibility_status", "text", true, "odf108_management_explain_support"},
      {"odf108_rebuild_state", "text", true, "odf108_management_explain_support"},
      {"odf108_exact_refusal_state", "text", true, "odf108_management_explain_support"},
      {"config_defaults_packaging_ready", "bool", true, "config_defaults"},
      {"config_precedence_order", "text", true, "config_defaults"},
      {"parser_finality_authority", "bool", true, "authority_boundaries"},
      {"donor_finality_authority", "bool", true, "authority_boundaries"},
      {"client_finality_authority", "bool", true, "authority_boundaries"},
      {"storage_shortcut_finality_authority", "bool", true, "authority_boundaries"},
      {"wal_recovery_authority", "bool", true, "authority_boundaries"}};
  return kSchema;
}

const std::vector<PerformanceOptimizationConfigSurfaceField>&
PerformanceOptimizationConfigSurface() {
  static const std::vector<PerformanceOptimizationConfigSurfaceField> kConfig = {
      {"optimizer_enabled",
       "bool",
       "true",
       "scratchbird.performance.optimizer.enabled",
       "SCRATCHBIRD_DPC_OPTIMIZER_ENABLED",
       "--dpc-optimizer",
       "performance.config.set",
       "dpc.optimizer_enabled",
       "sys.management.performance_optimization_config.optimizer_enabled",
       "performance_optimization_config.optimizer_enabled",
       "use deterministic baseline planner without cost-based rewrites"},
      {"copy_append_batching_enabled",
       "bool",
       "true",
       "scratchbird.performance.copy_append_batching.enabled",
       "SCRATCHBIRD_DPC_COPY_APPEND_BATCHING_ENABLED",
       "--dpc-copy-append-batching",
       "performance.config.set",
       "dpc.copy_append_batching_enabled",
       "sys.management.performance_optimization_config.copy_append_batching_enabled",
       "performance_optimization_config.copy_append_batching_enabled",
       "route COPY append through singleton append path with identical row order"},
      {"native_ingest_enabled",
       "bool",
       "false",
       "scratchbird.performance.native_ingest.enabled",
       "SCRATCHBIRD_DPC_NATIVE_INGEST_ENABLED",
       "--dpc-native-ingest",
       "performance.config.set",
       "dpc.native_ingest_enabled",
       "sys.management.performance_optimization_config.native_ingest_enabled",
       "performance_optimization_config.native_ingest_enabled",
       "refuse native ingest path and use ordinary engine-owned ingest routes"},
      {"plan_cache_enabled",
       "bool",
       "true",
       "scratchbird.performance.plan_cache.enabled",
       "SCRATCHBIRD_DPC_PLAN_CACHE_ENABLED",
       "--dpc-plan-cache",
       "performance.config.set",
       "dpc.plan_cache_enabled",
       "sys.management.performance_optimization_config.plan_cache_enabled",
       "performance_optimization_config.plan_cache_enabled",
       "compile plans per request and skip cache reuse"},
      {"descriptor_metadata_cache_enabled",
       "bool",
       "true",
       "scratchbird.performance.descriptor_metadata_cache.enabled",
       "SCRATCHBIRD_DPC_DESCRIPTOR_METADATA_CACHE_ENABLED",
       "--dpc-descriptor-metadata-cache",
       "performance.config.set",
       "dpc.descriptor_metadata_cache_enabled",
       "sys.management.performance_optimization_config.descriptor_metadata_cache_enabled",
       "performance_optimization_config.descriptor_metadata_cache_enabled",
       "read descriptor metadata from catalog authority on each use"},
      {"statistics_enabled",
       "bool",
       "true",
       "scratchbird.performance.statistics.enabled",
       "SCRATCHBIRD_DPC_STATISTICS_ENABLED",
       "--dpc-statistics",
       "performance.config.set",
       "dpc.statistics_enabled",
       "sys.management.performance_optimization_config.statistics_enabled",
       "performance_optimization_config.statistics_enabled",
       "use conservative estimates and stale-statistics fail-safe planning"},
      {"summary_prune_enabled",
       "bool",
       "true",
       "scratchbird.performance.summary_prune.enabled",
       "SCRATCHBIRD_DPC_SUMMARY_PRUNE_ENABLED",
       "--dpc-summary-prune",
       "performance.config.set",
       "dpc.summary_prune_enabled",
       "sys.management.performance_optimization_config.summary_prune_enabled",
       "performance_optimization_config.summary_prune_enabled",
       "scan candidate base ranges with mandatory MGA and security rechecks"},
      {"agent_workers_enabled",
       "bool",
       "true",
       "scratchbird.performance.agent_workers.enabled",
       "SCRATCHBIRD_DPC_AGENT_WORKERS_ENABLED",
       "--dpc-agent-workers",
       "performance.config.set",
       "dpc.agent_workers_enabled",
       "sys.management.performance_optimization_config.agent_workers_enabled",
       "performance_optimization_config.agent_workers_enabled",
       "perform required work synchronously or leave background work unstarted"},
      {"resource_governor_enabled",
       "bool",
       "true",
       "scratchbird.performance.resource_governor.enabled",
       "SCRATCHBIRD_DPC_RESOURCE_GOVERNOR_ENABLED",
       "--dpc-resource-governor",
       "performance.config.set",
       "dpc.resource_governor_enabled",
       "sys.management.performance_optimization_config.resource_governor_enabled",
       "performance_optimization_config.resource_governor_enabled",
       "disable optimization-specific throttles while core safety admission remains fail-closed"},
      {"page_filespace_preallocation_enabled",
       "bool",
       "true",
       "scratchbird.performance.page_filespace_preallocation.enabled",
       "SCRATCHBIRD_DPC_PAGE_FILESPACE_PREALLOCATION_ENABLED",
       "--dpc-page-filespace-preallocation",
       "performance.config.set",
       "dpc.page_filespace_preallocation_enabled",
       "sys.management.performance_optimization_config.page_filespace_preallocation_enabled",
       "performance_optimization_config.page_filespace_preallocation_enabled",
       "allocate pages on demand through ordinary filespace growth"},
      {"cancellation_enabled",
       "bool",
       "true",
       "scratchbird.performance.cancellation.enabled",
       "SCRATCHBIRD_DPC_CANCELLATION_ENABLED",
       "--dpc-cancellation",
       "performance.config.set",
       "dpc.cancellation_enabled",
       "sys.management.performance_optimization_config.cancellation_enabled",
       "performance_optimization_config.cancellation_enabled",
       "ignore optimization checkpoints while preserving transaction-layer failure authority"},
      {"backpressure_enabled",
       "bool",
       "true",
       "scratchbird.performance.backpressure.enabled",
       "SCRATCHBIRD_DPC_BACKPRESSURE_ENABLED",
       "--dpc-backpressure",
       "performance.config.set",
       "dpc.backpressure_enabled",
       "sys.management.performance_optimization_config.backpressure_enabled",
       "performance_optimization_config.backpressure_enabled",
       "avoid optimization deferrals and rely on normal admission/refusal paths"},
      {"copy_batch_rows_configured",
       "uint64",
       "1024",
       "scratchbird.performance.copy_batch_rows",
       "SCRATCHBIRD_DPC_COPY_BATCH_ROWS",
       "--dpc-copy-batch-rows",
       "performance.config.set",
       "dpc.copy_batch_rows_configured",
       "sys.management.performance_optimization_config.copy_batch_rows_configured",
       "performance_optimization_config.copy_batch_rows_configured",
       "clamp COPY append batching to singleton rows when batching is disabled"}};
  return kConfig;
}

PerformanceOptimizationConfigResolution
ResolvePerformanceOptimizationConfigSurface(
    const std::vector<PerformanceOptimizationConfigOverride>& overrides) {
  PerformanceOptimizationConfigResolution resolution;
  for (const auto& metadata : PerformanceOptimizationConfigSurface()) {
    PerformanceOptimizationConfigEffectiveValue field;
    field.metadata = metadata;
    field.configured_value = DefaultConfiguredValue(metadata.packaged_default);
    field.effective_value = metadata.packaged_default;
    field.value_source = "packaged_default";
    field.precedence_order = std::string(kPerformanceConfigPrecedence);
    field.override_refusal_code = "none";
    field.override_refusal_reason = "none";
    field.override_refusal_message_vector = "none";

    int effective_rank = ConfigSourceRank(field.value_source);
    for (const auto& override : overrides) {
      if (override.surface_name != metadata.surface_name) {
        continue;
      }
      const int override_rank = ConfigSourceRank(override.source);
      if (override_rank < 0) {
        continue;
      }
      if (!override.allowed_by_policy) {
        if (override_rank >= effective_rank) {
          field.override_refusal_code =
              override.refusal_code.empty()
                  ? "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY"
                  : override.refusal_code;
          field.override_refusal_reason =
              override.refusal_reason.empty()
                  ? "higher-precedence override denied by policy"
                  : override.refusal_reason;
          field.override_refusal_message_vector =
              override.refusal_message_vector.empty()
                  ? field.override_refusal_code + "|policy_denied"
                  : override.refusal_message_vector;
        }
        continue;
      }
      if (override_rank >= effective_rank) {
        field.configured_value = override.source + ":" + override.value;
        field.effective_value = override.value;
        field.value_source = override.source;
        effective_rank = override_rank;
      }
    }

    resolution.fields.push_back(std::move(field));
  }
  return resolution;
}

PerformanceOptimizationSurfaceSnapshot DefaultPerformanceOptimizationSurfaceSnapshot() {
  return PerformanceOptimizationSurfaceSnapshot{};
}

PerformanceOptimizationSurfaceValidationResult
ValidatePerformanceOptimizationSurfaceSnapshot(
    const PerformanceOptimizationSurfaceSnapshot& snapshot) {
  PerformanceOptimizationSurfaceValidationResult result;
  result.diagnostic_code = "CDP.USER_OBSERVABILITY_SURFACE.OK";

  RequireString(snapshot.optimization_profile,
                "optimization_profile",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.copy_batching_status,
                "copy_batching_status",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.native_ingest_state,
                "native_ingest_state",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.native_ingest_refusal_code,
                "native_ingest_refusal_code",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.plan_cache_last_invalidation_reason,
                "plan_cache_last_invalidation_reason",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.stale_statistics_fail_safe_reason,
                "stale_statistics_fail_safe_reason",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.cleanup_horizon_authority_status,
                "cleanup_horizon_authority_status",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.selected_join_algorithm,
                "selected_join_algorithm",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.selected_join_plan_summary,
                "selected_join_plan_summary",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.selected_join_statistics_version,
                "selected_join_statistics_version",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.summary_prune_status,
                "summary_prune_status",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.summary_prune_last_reason,
                "summary_prune_last_reason",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.summary_prune_fallback_reason,
                "summary_prune_fallback_reason",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.summary_prune_summary_status,
                "summary_prune_summary_status",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.summary_prune_authority_source,
                "summary_prune_authority_source",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.agent_worker_status,
                "agent_worker_status",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.last_agent_type_id,
                "last_agent_type_id",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.last_agent_action,
                "last_agent_action",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.last_agent_decision,
                "last_agent_decision",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.last_agent_diagnostic_code,
                "last_agent_diagnostic_code",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.secondary_index_state,
                "secondary_index_state",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.shadow_index_state,
                "shadow_index_state",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.summary_index_state,
                "summary_index_state",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.specialized_index_state,
                "specialized_index_state",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.index_state_authority_source,
                "index_state_authority_source",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.resource_governor_state,
                "resource_governor_state",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.backpressure_state,
                "backpressure_state",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.backpressure_reason,
                "backpressure_reason",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.benchmark_correlation_id,
                "benchmark_correlation_id",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.support_bundle_correlation_id,
                "support_bundle_correlation_id",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.request_correlation_id,
                "request_correlation_id",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.exact_refusal_diagnostic_code,
                "exact_refusal_diagnostic_code",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.exact_refusal_message_vector,
                "exact_refusal_message_vector",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.exact_refusal_source,
                "exact_refusal_source",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.metric_family,
                "metric_family",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.audit_event_family,
                "audit_event_family",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.audit_last_decision,
                "audit_last_decision",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.support_bundle_redaction_state,
                "support_bundle_redaction_state",
                &result.missing_or_invalid_fields);
  RequireString(snapshot.support_bundle_completeness_state,
                "support_bundle_completeness_state",
                &result.missing_or_invalid_fields);

  if (snapshot.copy_batches_completed > snapshot.copy_batches_started) {
    AddMissing(&result.missing_or_invalid_fields, "copy_batches_completed");
  }
  if (snapshot.native_ingest_rows_processed > snapshot.native_ingest_rows_total &&
      snapshot.native_ingest_rows_total != 0) {
    AddMissing(&result.missing_or_invalid_fields, "native_ingest_rows_processed");
  }
  if (snapshot.native_ingest_refused &&
      (snapshot.native_ingest_refusal_code.empty() ||
       snapshot.native_ingest_refusal_code == "none")) {
    AddMissing(&result.missing_or_invalid_fields, "native_ingest_refusal_code");
  }
  if (snapshot.backpressure_active &&
      (snapshot.backpressure_state == "none" || snapshot.backpressure_reason == "none")) {
    AddMissing(&result.missing_or_invalid_fields, "backpressure_state");
  }
  if (snapshot.summary_prune_ranges_pruned +
          snapshot.summary_prune_ranges_scanned >
      snapshot.summary_prune_ranges_considered) {
    AddMissing(&result.missing_or_invalid_fields,
               "summary_prune_ranges_considered");
  }
  if (snapshot.summary_prune_pages_pruned +
          snapshot.summary_prune_pages_scanned >
      snapshot.summary_prune_pages_considered) {
    AddMissing(&result.missing_or_invalid_fields,
               "summary_prune_pages_considered");
  }
  if (snapshot.summary_prune_status == "summary_prune" &&
      (!snapshot.summary_prune_base_row_mga_recheck_required ||
       !snapshot.summary_prune_base_row_security_recheck_required)) {
    AddMissing(&result.missing_or_invalid_fields,
               "summary_prune_base_row_recheck_required");
  }
  if (snapshot.cleanup_horizon_authoritative &&
      snapshot.cleanup_horizon_authority_status != "authoritative") {
    AddMissing(&result.missing_or_invalid_fields,
               "cleanup_horizon_authority_status");
  }
  if (snapshot.cleanup_horizon_authoritative &&
      (snapshot.oldest_interesting_transaction_id == 0 ||
       snapshot.oldest_active_transaction_id == 0 ||
       snapshot.oldest_snapshot_transaction_id == 0 ||
       snapshot.oldest_cleanup_transaction_id == 0)) {
    AddMissing(&result.missing_or_invalid_fields,
               "oldest_transaction_horizon_fields");
  }
  if (snapshot.last_agent_decision != "none" &&
      snapshot.last_agent_diagnostic_code == "none") {
    AddMissing(&result.missing_or_invalid_fields,
               "last_agent_diagnostic_code");
  }
  if (snapshot.exact_refusal_diagnostic_code != "none" &&
      (snapshot.exact_refusal_message_vector == "none" ||
       !snapshot.message_vector_ready)) {
    AddMissing(&result.missing_or_invalid_fields,
               "exact_refusal_message_vector");
  }
  if (snapshot.support_bundle_completeness_state == "complete" &&
      !snapshot.support_bundle_forbidden_fields_absent) {
    AddMissing(&result.missing_or_invalid_fields,
               "support_bundle_forbidden_fields_absent");
  }

  if (snapshot.odf108_selected_paths.empty()) {
    AddMissing(&result.missing_or_invalid_fields, "odf108_selected_paths");
  }
  for (const auto& path : snapshot.odf108_selected_paths) {
    RequireSafeOdf108Text(path.path_id,
                          "odf108_selected_paths.path_id",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(path.path_kind,
                          "odf108_selected_paths.path_kind",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(path.selected_path,
                          "odf108_selected_paths.selected_path",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(path.path_state,
                          "odf108_selected_paths.path_state",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(path.authority_source,
                          "odf108_selected_paths.authority_source",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(path.fallback_reason,
                          "odf108_selected_paths.fallback_reason",
                          &result.missing_or_invalid_fields);
  }

  if (snapshot.odf108_feature_gates.empty()) {
    AddMissing(&result.missing_or_invalid_fields, "odf108_feature_gates");
  }
  for (const auto& gate : snapshot.odf108_feature_gates) {
    RequireSafeOdf108Text(gate.gate_id,
                          "odf108_feature_gates.gate_id",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(gate.gate_state,
                          "odf108_feature_gates.gate_state",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(gate.authority_source,
                          "odf108_feature_gates.authority_source",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(gate.refusal_code,
                          "odf108_feature_gates.refusal_code",
                          &result.missing_or_invalid_fields);
    if (!gate.enabled && gate.refusal_code == "none") {
      AddMissing(&result.missing_or_invalid_fields,
                 "odf108_feature_gates.refusal_code");
    }
  }

  if (snapshot.odf108_fallbacks.empty()) {
    AddMissing(&result.missing_or_invalid_fields, "odf108_fallbacks");
  }
  for (const auto& fallback : snapshot.odf108_fallbacks) {
    RequireSafeOdf108Text(fallback.route_id,
                          "odf108_fallbacks.route_id",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(fallback.reason_code,
                          "odf108_fallbacks.reason_code",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(fallback.detail,
                          "odf108_fallbacks.detail",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(fallback.scalar_fallback_state,
                          "odf108_fallbacks.scalar_fallback_state",
                          &result.missing_or_invalid_fields);
  }

  if (snapshot.odf108_quotas.empty()) {
    AddMissing(&result.missing_or_invalid_fields, "odf108_quotas");
  }
  for (const auto& quota : snapshot.odf108_quotas) {
    RequireSafeOdf108Text(quota.quota_family,
                          "odf108_quotas.quota_family",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(quota.quota_state,
                          "odf108_quotas.quota_state",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(quota.action,
                          "odf108_quotas.action",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(quota.diagnostic_code,
                          "odf108_quotas.diagnostic_code",
                          &result.missing_or_invalid_fields);
    if ((quota.refusals > 0 || quota.quota_state == "refused") &&
        quota.diagnostic_code == "none") {
      AddMissing(&result.missing_or_invalid_fields,
                 "odf108_quotas.diagnostic_code");
    }
    if (quota.quota_limit != 0 && quota.quota_in_use > quota.quota_limit &&
        quota.diagnostic_code == "none") {
      AddMissing(&result.missing_or_invalid_fields,
                 "odf108_quotas.quota_limit");
    }
  }

  if (snapshot.odf108_runtime_compatibility.empty()) {
    AddMissing(&result.missing_or_invalid_fields,
               "odf108_runtime_compatibility");
  }
  for (const auto& compatibility : snapshot.odf108_runtime_compatibility) {
    RequireSafeOdf108Text(compatibility.route_id,
                          "odf108_runtime_compatibility.route_id",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(compatibility.compatibility_status,
                          "odf108_runtime_compatibility.compatibility_status",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(compatibility.compatibility_action,
                          "odf108_runtime_compatibility.compatibility_action",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(compatibility.required_capabilities,
                          "odf108_runtime_compatibility.required_capabilities",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(compatibility.provider_capabilities,
                          "odf108_runtime_compatibility.provider_capabilities",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(compatibility.diagnostic_code,
                          "odf108_runtime_compatibility.diagnostic_code",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(compatibility.fallback_reason,
                          "odf108_runtime_compatibility.fallback_reason",
                          &result.missing_or_invalid_fields);
    if (compatibility.compatibility_status != "compatible" &&
        compatibility.compatibility_status != "unknown" &&
        compatibility.diagnostic_code == "none") {
      AddMissing(&result.missing_or_invalid_fields,
                 "odf108_runtime_compatibility.diagnostic_code");
    }
  }

  if (snapshot.odf108_rebuild_states.empty()) {
    AddMissing(&result.missing_or_invalid_fields, "odf108_rebuild_states");
  }
  for (const auto& rebuild : snapshot.odf108_rebuild_states) {
    RequireSafeOdf108Text(rebuild.object_kind,
                          "odf108_rebuild_states.object_kind",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(rebuild.rebuild_state,
                          "odf108_rebuild_states.rebuild_state",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(rebuild.rebuild_phase,
                          "odf108_rebuild_states.rebuild_phase",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(rebuild.refusal_code,
                          "odf108_rebuild_states.refusal_code",
                          &result.missing_or_invalid_fields);
    if (rebuild.progress_denominator != 0 &&
        rebuild.progress_numerator > rebuild.progress_denominator) {
      AddMissing(&result.missing_or_invalid_fields,
                 "odf108_rebuild_states.progress");
    }
    if (rebuild.rebuild_state == "refused" && rebuild.refusal_code == "none") {
      AddMissing(&result.missing_or_invalid_fields,
                 "odf108_rebuild_states.refusal_code");
    }
  }

  if (snapshot.odf108_exact_refusals.empty()) {
    AddMissing(&result.missing_or_invalid_fields, "odf108_exact_refusals");
  }
  for (const auto& refusal : snapshot.odf108_exact_refusals) {
    RequireSafeOdf108Text(refusal.source,
                          "odf108_exact_refusals.source",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(refusal.diagnostic_code,
                          "odf108_exact_refusals.diagnostic_code",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(refusal.message_vector,
                          "odf108_exact_refusals.message_vector",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(refusal.refusal_state,
                          "odf108_exact_refusals.refusal_state",
                          &result.missing_or_invalid_fields);
    RequireSafeOdf108Text(refusal.redaction_state,
                          "odf108_exact_refusals.redaction_state",
                          &result.missing_or_invalid_fields);
    if (refusal.diagnostic_code != "none" &&
        (refusal.message_vector == "none" || !refusal.public_safe)) {
      AddMissing(&result.missing_or_invalid_fields,
                 "odf108_exact_refusals.message_vector");
    }
  }

  if (snapshot.parser_finality_authority ||
      snapshot.donor_finality_authority ||
      snapshot.client_finality_authority ||
      snapshot.storage_shortcut_finality_authority ||
      snapshot.wal_recovery_authority) {
    AddMissing(&result.missing_or_invalid_fields,
               "non_authority_boundary");
  }

  result.ok = result.missing_or_invalid_fields.empty();
  if (!result.ok) {
    result.diagnostic_code = "CDP.USER_OBSERVABILITY_SURFACE.INVALID_SNAPSHOT";
    std::ostringstream detail;
    detail << "invalid performance optimization surface fields:";
    for (const auto& field : result.missing_or_invalid_fields) {
      detail << ' ' << field;
    }
    result.detail = detail.str();
  }
  return result;
}

std::string SerializePerformanceOptimizationConfigResolutionJson(
    const PerformanceOptimizationConfigResolution& resolution) {
  std::ostringstream out;
  out << "{\"config_surface\":\"performance_optimization\","
      << "\"precedence_order\":\"" << JsonEscape(kPerformanceConfigPrecedence)
      << "\",\"fields\":[";
  bool first = true;
  for (const auto& field : resolution.fields) {
    if (!first) {
      out << ',';
    }
    first = false;
    AddConfigEffectiveValueJson(&out, field);
  }
  out << "]}";
  return out.str();
}

std::string SerializePerformanceOptimizationSurfaceJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  AddJsonString(&out, &first, "schema_id", PerformanceOptimizationSurfaceSchemaId());
  AddJsonU64(&out, &first, "schema_version", PerformanceOptimizationSurfaceSchemaVersion());
  AddJsonString(&out, &first, "optimization_profile", snapshot.optimization_profile);
  AddJsonBool(&out, &first, "optimizer_enabled", snapshot.optimizer_enabled);
  AddJsonBool(&out, &first, "copy_append_batching_enabled", snapshot.copy_append_batching_enabled);
  AddJsonBool(&out, &first, "native_ingest_enabled", snapshot.native_ingest_enabled);
  AddJsonBool(&out, &first, "plan_cache_enabled", snapshot.plan_cache_enabled);
  AddJsonBool(&out,
              &first,
              "descriptor_metadata_cache_enabled",
              snapshot.descriptor_metadata_cache_enabled);
  AddJsonBool(&out, &first, "statistics_enabled", snapshot.statistics_enabled);
  AddJsonBool(&out, &first, "summary_prune_enabled", snapshot.summary_prune_enabled);
  AddJsonBool(&out, &first, "agent_workers_enabled", snapshot.agent_workers_enabled);
  AddJsonBool(&out, &first, "resource_governor_enabled", snapshot.resource_governor_enabled);
  AddJsonBool(&out,
              &first,
              "page_filespace_preallocation_enabled",
              snapshot.page_filespace_preallocation_enabled);
  AddJsonBool(&out, &first, "cancellation_enabled", snapshot.cancellation_enabled);
  AddJsonBool(&out, &first, "backpressure_enabled", snapshot.backpressure_enabled);
  AddJsonString(&out, &first, "copy_batching_status", snapshot.copy_batching_status);
  AddJsonU64(&out, &first, "copy_batch_rows_configured", snapshot.copy_batch_rows_configured);
  AddJsonU64(&out, &first, "copy_batches_started", snapshot.copy_batches_started);
  AddJsonU64(&out, &first, "copy_batches_completed", snapshot.copy_batches_completed);
  AddJsonU64(&out, &first, "copy_rows_batched", snapshot.copy_rows_batched);
  AddJsonU64(&out,
             &first,
             "copy_singleton_fallback_batches",
             snapshot.copy_singleton_fallback_batches);
  AddJsonString(&out, &first, "native_ingest_state", snapshot.native_ingest_state);
  AddJsonU64(&out,
             &first,
             "native_ingest_rows_processed",
             snapshot.native_ingest_rows_processed);
  AddJsonU64(&out, &first, "native_ingest_rows_total", snapshot.native_ingest_rows_total);
  AddJsonBool(&out, &first, "native_ingest_refused", snapshot.native_ingest_refused);
  AddJsonString(&out, &first, "native_ingest_refusal_code", snapshot.native_ingest_refusal_code);
  AddJsonU64(&out, &first, "plan_cache_hits", snapshot.plan_cache_hits);
  AddJsonU64(&out, &first, "plan_cache_misses", snapshot.plan_cache_misses);
  AddJsonU64(&out, &first, "plan_cache_invalidations", snapshot.plan_cache_invalidations);
  AddJsonString(&out,
                &first,
                "plan_cache_last_invalidation_reason",
                snapshot.plan_cache_last_invalidation_reason);
  AddJsonU64(&out,
             &first,
             "descriptor_metadata_cache_hits",
             snapshot.descriptor_metadata_cache_hits);
  AddJsonU64(&out,
             &first,
             "descriptor_metadata_cache_misses",
             snapshot.descriptor_metadata_cache_misses);
  AddJsonU64(&out,
             &first,
             "descriptor_metadata_cache_epoch",
             snapshot.descriptor_metadata_cache_epoch);
  AddJsonU64(&out, &first, "statistics_epoch", snapshot.statistics_epoch);
  AddJsonBool(&out,
              &first,
              "stale_statistics_fail_safe_active",
              snapshot.stale_statistics_fail_safe_active);
  AddJsonString(&out,
                &first,
                "stale_statistics_fail_safe_reason",
                snapshot.stale_statistics_fail_safe_reason);
  AddJsonU64(&out, &first, "catalog_generation_id", snapshot.catalog_generation_id);
  AddJsonU64(&out, &first, "name_resolution_epoch", snapshot.name_resolution_epoch);
  AddJsonU64(&out, &first, "security_epoch", snapshot.security_epoch);
  AddJsonU64(&out, &first, "resource_epoch", snapshot.resource_epoch);
  AddJsonU64(&out,
             &first,
             "optimization_state_epoch",
             snapshot.optimization_state_epoch);
  AddJsonString(&out, &first, "selected_join_algorithm", snapshot.selected_join_algorithm);
  AddJsonString(&out, &first, "selected_join_plan_summary", snapshot.selected_join_plan_summary);
  AddJsonU64(&out, &first, "selected_join_left_rows", snapshot.selected_join_left_rows);
  AddJsonU64(&out, &first, "selected_join_right_rows", snapshot.selected_join_right_rows);
  AddJsonBool(&out,
              &first,
              "selected_join_from_statistics",
              snapshot.selected_join_from_statistics);
  AddJsonString(&out,
                &first,
                "selected_join_statistics_version",
                snapshot.selected_join_statistics_version);
  AddJsonString(&out, &first, "summary_prune_status", snapshot.summary_prune_status);
  AddJsonString(&out,
                &first,
                "summary_prune_last_reason",
                snapshot.summary_prune_last_reason);
  AddJsonString(&out,
                &first,
                "summary_prune_fallback_reason",
                snapshot.summary_prune_fallback_reason);
  AddJsonString(&out,
                &first,
                "summary_prune_summary_status",
                snapshot.summary_prune_summary_status);
  AddJsonU64(&out, &first, "summary_prune_generation", snapshot.summary_prune_generation);
  AddJsonU64(&out,
             &first,
             "summary_prune_ranges_considered",
             snapshot.summary_prune_ranges_considered);
  AddJsonU64(&out,
             &first,
             "summary_prune_ranges_pruned",
             snapshot.summary_prune_ranges_pruned);
  AddJsonU64(&out,
             &first,
             "summary_prune_ranges_scanned",
             snapshot.summary_prune_ranges_scanned);
  AddJsonU64(&out,
             &first,
             "summary_prune_pages_considered",
             snapshot.summary_prune_pages_considered);
  AddJsonU64(&out,
             &first,
             "summary_prune_pages_pruned",
             snapshot.summary_prune_pages_pruned);
  AddJsonU64(&out,
             &first,
             "summary_prune_pages_scanned",
             snapshot.summary_prune_pages_scanned);
  AddJsonString(&out,
                &first,
                "summary_prune_authority_source",
                snapshot.summary_prune_authority_source);
  AddJsonBool(&out,
              &first,
              "summary_prune_base_row_mga_recheck_required",
              snapshot.summary_prune_base_row_mga_recheck_required);
  AddJsonBool(&out,
              &first,
              "summary_prune_base_row_security_recheck_required",
              snapshot.summary_prune_base_row_security_recheck_required);
  AddJsonString(&out,
                &first,
                "cleanup_horizon_authority_status",
                snapshot.cleanup_horizon_authority_status);
  AddJsonBool(&out,
              &first,
              "cleanup_horizon_authoritative",
              snapshot.cleanup_horizon_authoritative);
  AddJsonU64(&out,
             &first,
             "cleanup_horizon_local_transaction_id",
             snapshot.cleanup_horizon_local_transaction_id);
  AddJsonU64(&out,
             &first,
             "oldest_interesting_transaction_id",
             snapshot.oldest_interesting_transaction_id);
  AddJsonU64(&out,
             &first,
             "oldest_active_transaction_id",
             snapshot.oldest_active_transaction_id);
  AddJsonU64(&out,
             &first,
             "oldest_snapshot_transaction_id",
             snapshot.oldest_snapshot_transaction_id);
  AddJsonU64(&out,
             &first,
             "oldest_cleanup_transaction_id",
             snapshot.oldest_cleanup_transaction_id);
  AddJsonString(&out, &first, "agent_worker_status", snapshot.agent_worker_status);
  AddJsonU64(&out, &first, "agent_worker_thread_count", snapshot.agent_worker_thread_count);
  AddJsonU64(&out,
             &first,
             "agent_worker_actions_accepted",
             snapshot.agent_worker_actions_accepted);
  AddJsonU64(&out,
             &first,
             "agent_worker_actions_refused",
             snapshot.agent_worker_actions_refused);
  AddJsonU64(&out,
             &first,
             "agent_work_backlog_count",
             snapshot.agent_work_backlog_count);
  AddJsonString(&out, &first, "last_agent_type_id", snapshot.last_agent_type_id);
  AddJsonString(&out, &first, "last_agent_action", snapshot.last_agent_action);
  AddJsonString(&out, &first, "last_agent_decision", snapshot.last_agent_decision);
  AddJsonString(&out,
                &first,
                "last_agent_diagnostic_code",
                snapshot.last_agent_diagnostic_code);
  AddJsonU64(&out,
             &first,
             "storage_row_version_backlog_count",
             snapshot.storage_row_version_backlog_count);
  AddJsonU64(&out,
             &first,
             "index_delta_backlog_count",
             snapshot.index_delta_backlog_count);
  AddJsonU64(&out,
             &first,
             "index_garbage_backlog_count",
             snapshot.index_garbage_backlog_count);
  AddJsonU64(&out,
             &first,
             "page_summary_backlog_count",
             snapshot.page_summary_backlog_count);
  AddJsonString(&out, &first, "secondary_index_state", snapshot.secondary_index_state);
  AddJsonString(&out, &first, "shadow_index_state", snapshot.shadow_index_state);
  AddJsonString(&out, &first, "summary_index_state", snapshot.summary_index_state);
  AddJsonString(&out,
                &first,
                "specialized_index_state",
                snapshot.specialized_index_state);
  AddJsonString(&out,
                &first,
                "index_state_authority_source",
                snapshot.index_state_authority_source);
  AddJsonString(&out, &first, "resource_governor_state", snapshot.resource_governor_state);
  AddJsonU64(&out, &first, "resource_quota_grants", snapshot.resource_quota_grants);
  AddJsonU64(&out, &first, "resource_quota_refusals", snapshot.resource_quota_refusals);
  AddJsonU64(&out, &first, "resource_quota_in_use", snapshot.resource_quota_in_use);
  AddJsonU64(&out,
             &first,
             "page_preallocation_demand_pages",
             snapshot.page_preallocation_demand_pages);
  AddJsonU64(&out,
             &first,
             "page_preallocation_granted_pages",
             snapshot.page_preallocation_granted_pages);
  AddJsonU64(&out,
             &first,
             "filespace_preallocation_demand_pages",
             snapshot.filespace_preallocation_demand_pages);
  AddJsonU64(&out,
             &first,
             "filespace_preallocation_granted_pages",
             snapshot.filespace_preallocation_granted_pages);
  AddJsonU64(&out, &first, "preallocation_refusal_count", snapshot.preallocation_refusal_count);
  AddJsonBool(&out, &first, "cancellation_requested", snapshot.cancellation_requested);
  AddJsonBool(&out, &first, "backpressure_active", snapshot.backpressure_active);
  AddJsonString(&out, &first, "backpressure_state", snapshot.backpressure_state);
  AddJsonString(&out, &first, "backpressure_reason", snapshot.backpressure_reason);
  AddJsonU64(&out,
             &first,
             "cancellation_checkpoint_count",
             snapshot.cancellation_checkpoint_count);
  AddJsonU64(&out,
             &first,
             "backpressure_deferral_count",
             snapshot.backpressure_deferral_count);
  AddJsonString(&out, &first, "benchmark_correlation_id", snapshot.benchmark_correlation_id);
  AddJsonString(&out,
                &first,
                "support_bundle_correlation_id",
                snapshot.support_bundle_correlation_id);
  AddJsonString(&out, &first, "request_correlation_id", snapshot.request_correlation_id);
  AddJsonString(&out,
                &first,
                "exact_refusal_diagnostic_code",
                snapshot.exact_refusal_diagnostic_code);
  AddJsonString(&out,
                &first,
                "exact_refusal_message_vector",
                snapshot.exact_refusal_message_vector);
  AddJsonString(&out,
                &first,
                "exact_refusal_source",
                snapshot.exact_refusal_source);
  AddJsonBool(&out, &first, "message_vector_ready", snapshot.message_vector_ready);
  AddJsonString(&out, &first, "metric_family", snapshot.metric_family);
  AddJsonU64(&out, &first, "metric_sample_count", snapshot.metric_sample_count);
  AddJsonString(&out, &first, "audit_event_family", snapshot.audit_event_family);
  AddJsonU64(&out, &first, "audit_event_count", snapshot.audit_event_count);
  AddJsonString(&out, &first, "audit_last_decision", snapshot.audit_last_decision);
  AddJsonString(&out,
                &first,
                "support_bundle_redaction_state",
                snapshot.support_bundle_redaction_state);
  AddJsonString(&out,
                &first,
                "support_bundle_completeness_state",
                snapshot.support_bundle_completeness_state);
  AddJsonBool(&out,
              &first,
              "support_bundle_forbidden_fields_absent",
              snapshot.support_bundle_forbidden_fields_absent);
  AddJsonBool(&out, &first, "odf108_surface_ready", true);
  AddJsonU64(&out,
             &first,
             "odf108_selected_path_count",
             snapshot.odf108_selected_paths.size());
  AddJsonU64(&out,
             &first,
             "odf108_feature_gate_count",
             snapshot.odf108_feature_gates.size());
  AddJsonU64(&out,
             &first,
             "odf108_fallback_reason_count",
             snapshot.odf108_fallbacks.size());
  AddJsonU64(&out,
             &first,
             "odf108_quota_state_count",
             snapshot.odf108_quotas.size());
  AddJsonU64(&out,
             &first,
             "odf108_runtime_compatibility_count",
             snapshot.odf108_runtime_compatibility.size());
  AddJsonU64(&out,
             &first,
             "odf108_rebuild_state_count",
             snapshot.odf108_rebuild_states.size());
  AddJsonU64(&out,
             &first,
             "odf108_exact_refusal_count",
             snapshot.odf108_exact_refusals.size());
  AddJsonArray(&out,
               &first,
               "odf108_selected_paths",
               snapshot.odf108_selected_paths,
               AddJsonSelectedPath);
  AddJsonArray(&out,
               &first,
               "odf108_feature_gates",
               snapshot.odf108_feature_gates,
               AddJsonFeatureGate);
  AddJsonArray(&out,
               &first,
               "odf108_fallbacks",
               snapshot.odf108_fallbacks,
               AddJsonFallback);
  AddJsonArray(&out,
               &first,
               "odf108_quotas",
               snapshot.odf108_quotas,
               AddJsonQuota);
  AddJsonArray(&out,
               &first,
               "odf108_runtime_compatibility",
               snapshot.odf108_runtime_compatibility,
               AddJsonRuntimeCompatibility);
  AddJsonArray(&out,
               &first,
               "odf108_rebuild_states",
               snapshot.odf108_rebuild_states,
               AddJsonRebuildState);
  AddJsonArray(&out,
               &first,
               "odf108_exact_refusals",
               snapshot.odf108_exact_refusals,
               AddJsonExactRefusal);
  AddJsonBool(&out, &first, "config_defaults_packaging_ready", true);
  AddJsonString(&out, &first, "config_precedence_order", kPerformanceConfigPrecedence);
  AddJsonBool(&out,
              &first,
              "parser_finality_authority",
              snapshot.parser_finality_authority);
  AddJsonBool(&out,
              &first,
              "donor_finality_authority",
              snapshot.donor_finality_authority);
  AddJsonBool(&out,
              &first,
              "client_finality_authority",
              snapshot.client_finality_authority);
  AddJsonBool(&out,
              &first,
              "storage_shortcut_finality_authority",
              snapshot.storage_shortcut_finality_authority);
  AddJsonBool(&out,
              &first,
              "wal_recovery_authority",
              snapshot.wal_recovery_authority);
  AddJsonString(&out, &first, "catalog_uuid_authority", "engine");
  out << '}';
  return out.str();
}

std::string SerializePerformanceOptimizationManagementJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot) {
  return ManagementJson(snapshot, ResolvePerformanceOptimizationConfigSurface({}));
}

std::string SerializePerformanceOptimizationManagementJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot,
    const PerformanceOptimizationConfigResolution& config_resolution) {
  return ManagementJson(snapshot, config_resolution);
}

std::string SerializePerformanceOptimizationSupportBundleJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot) {
  return SupportBundleJson(
      RedactedSnapshotForSupportBundle(snapshot),
      RedactedConfigResolutionForSupportBundle(
          ResolvePerformanceOptimizationConfigSurface({})));
}

std::string SerializePerformanceOptimizationSupportBundleJson(
    const PerformanceOptimizationSurfaceSnapshot& snapshot,
    const PerformanceOptimizationConfigResolution& config_resolution) {
  return SupportBundleJson(
      RedactedSnapshotForSupportBundle(snapshot),
      RedactedConfigResolutionForSupportBundle(config_resolution));
}

EngineInspectPerformanceOptimizationSurfaceResult
EngineInspectPerformanceOptimizationSurface(
    const EngineInspectPerformanceOptimizationSurfaceRequest& request) {
  constexpr const char* kOperation = "observability.performance_optimization_surface";
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineInspectPerformanceOptimizationSurfaceResult>(
        request.context,
        kOperation,
        MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  std::string granted_right;
  EngineApiDiagnostic authorization_diagnostic;
  if (!HasPerformanceSurfaceInspectRight(request.context,
                                         &granted_right,
                                         &authorization_diagnostic)) {
    return PerformanceSurfaceAuthorizationDenied<
        EngineInspectPerformanceOptimizationSurfaceResult>(
        request.context,
        kOperation,
        authorization_diagnostic);
  }

  const auto snapshot = request.snapshot_present
                            ? request.snapshot
                            : DefaultPerformanceOptimizationSurfaceSnapshot();
  const auto validation = ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  if (!validation.ok) {
    return MakeApiBehaviorDiagnostic<EngineInspectPerformanceOptimizationSurfaceResult>(
        request.context,
        kOperation,
        MakeEngineApiDiagnostic(validation.diagnostic_code,
                                "observability.performance_optimization.invalid_snapshot",
                                validation.detail,
                                true));
  }

  auto result =
      MakeApiBehaviorSuccess<EngineInspectPerformanceOptimizationSurfaceResult>(
          request.context,
          kOperation);
  result.surface_schema_version = PerformanceOptimizationSurfaceSchemaVersion();
  result.management_api_ready = true;
  result.support_bundle_ready = true;
  result.sys_view_contract_ready = true;
  result.parser_finality_authority = false;
  result.donor_finality_authority = false;
  const auto config_resolution =
      ResolvePerformanceOptimizationConfigSurface(request.config_overrides);
  result.management_api_json =
      SerializePerformanceOptimizationManagementJson(snapshot,
                                                     config_resolution);
  result.support_bundle_json =
      SerializePerformanceOptimizationSupportBundleJson(snapshot,
                                                        config_resolution);
  result.result_shape.result_kind = "rs.performance_optimization_surface.v1";

  AddPerformanceOptimizationSurfaceRow(&result, snapshot);
  AddApiBehaviorEvidence(&result, "user_observability_surface", "CDP-033");
  AddApiBehaviorEvidence(&result, "user_observability_surface", "DPC-061");
  AddApiBehaviorEvidence(&result,
                         "management_explain_support_surface",
                         "ODF-108");
  AddApiBehaviorEvidence(&result,
                         "odf108_selected_path_count",
                         std::to_string(snapshot.odf108_selected_paths.size()));
  AddApiBehaviorEvidence(&result,
                         "odf108_feature_gate_count",
                         std::to_string(snapshot.odf108_feature_gates.size()));
  AddApiBehaviorEvidence(&result,
                         "odf108_quota_state_count",
                         std::to_string(snapshot.odf108_quotas.size()));
  AddApiBehaviorEvidence(
      &result,
      "odf108_runtime_compatibility_count",
      std::to_string(snapshot.odf108_runtime_compatibility.size()));
  AddApiBehaviorEvidence(&result,
                         "odf108_rebuild_state_count",
                         std::to_string(snapshot.odf108_rebuild_states.size()));
  AddApiBehaviorEvidence(&result, "config_defaults_packaging_gate", "DPC-068");
  AddApiBehaviorEvidence(&result, "management_api_surface", "performance_optimization");
  AddApiBehaviorEvidence(&result, "engine_authorization_authority",
                         "EngineAuthorize");
  AddApiBehaviorEvidence(&result, "authorization_required_rights",
                         "OBS_MANAGEMENT_INSPECT|OBS_INDEX_PROFILE_READ|MGA_CLEANUP_INSPECT");
  AddApiBehaviorEvidence(&result, "authorization_decision",
                         "allow:" + granted_right);
  AddApiBehaviorRow(&result,
                    {{"record_kind", "engine_authorization_decision"},
                     {"authorization_authority", "engine_internal_api"},
                     {"decision", "allow"},
                     {"operation_class", "performance_backlog_inspect"},
                     {"required_rights",
                      "OBS_MANAGEMENT_INSPECT|OBS_INDEX_PROFILE_READ|MGA_CLEANUP_INSPECT"},
                     {"granted_right", granted_right},
                     {"message_vector", "SECURITY.AUTHORIZATION.ALLOW"}});
  AddApiBehaviorEvidence(&result, "support_bundle_surface", "performance_optimization_surface");
  AddApiBehaviorEvidence(&result, "support_bundle_config_defaults",
                         "performance_optimization_config");
  AddApiBehaviorEvidence(&result, "config_override_resolution",
                         request.config_overrides.empty()
                             ? "packaged_default"
                             : "request_overrides");
  AddApiBehaviorEvidence(&result, "sys_view_contract", "performance_optimization_surface");
  AddApiBehaviorEvidence(&result, "performance_optimization_metrics_surface",
                         snapshot.metric_family);
  AddApiBehaviorEvidence(&result, "performance_optimization_audit_surface",
                         snapshot.audit_event_family);
  AddApiBehaviorEvidence(&result, "performance_optimization_message_vector",
                         snapshot.exact_refusal_message_vector);
  AddApiBehaviorEvidence(&result, "mga_authority_boundary", "engine_owned");
  AddApiBehaviorEvidence(&result, "parser_finality_authority", "false");
  AddApiBehaviorEvidence(&result, "donor_finality_authority", "false");
  AddApiBehaviorEvidence(&result, "wal_recovery_authority", "false");
  return result;
}

}  // namespace scratchbird::engine::internal_api
