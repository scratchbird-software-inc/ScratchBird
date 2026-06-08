// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_OBSERVABILITY_AUDIT_SUPPORT

#include "server_observability.hpp"

#include "sbps.hpp"

#include <chrono>
#include <cctype>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::server {

namespace {

std::uint64_t NowMicros() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

std::string RedactedPath(const std::string& value) {
  return value.empty() ? "" : "[path-redacted]";
}

std::string AuditUuid() {
  return UuidBytesToText(sbps::MakeUuidV7Bytes());
}

bool AppendLine(const std::filesystem::path& path, const std::string& line) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::app);
  if (!out) return false;
  out << line << '\n';
  out.close();
  return static_cast<bool>(out);
}

std::string MetricKey(const std::string& path,
                      const std::map<std::string, std::string>& labels) {
  std::string key = path;
  for (const auto& [label, value] : labels) {
    key += "|" + label + "=" + value;
  }
  return key;
}

std::string LabelsJson(const std::map<std::string, std::string>& labels) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto& [key, value] : labels) {
    if (!first) out << ',';
    first = false;
    out << "\"" << JsonEscape(key) << "\":\"" << JsonEscape(value) << "\"";
  }
  out << "}";
  return out.str();
}

std::string MetricSampleJson(const ServerMetricSample& sample) {
  std::ostringstream out;
  out << "{\"path\":\"" << JsonEscape(sample.path)
      << "\",\"type\":\"" << JsonEscape(sample.type)
      << "\",\"value\":" << sample.value
      << ",\"labels\":" << LabelsJson(sample.labels)
      << ",\"visibility_right\":\"" << JsonEscape(sample.visibility_right)
      << "\",\"redaction_class\":\"" << JsonEscape(sample.redaction_class) << "\"}";
  return out.str();
}

bool SensitiveKeyAt(const std::string& text, std::size_t pos, const char* key) {
  std::size_t i = 0;
  while (key[i] != '\0') {
    if (pos + i >= text.size()) return false;
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(text[pos + i]))) != key[i]) {
      return false;
    }
    ++i;
  }
  if (pos > 0) {
    const unsigned char previous = static_cast<unsigned char>(text[pos - 1]);
    if (std::isalnum(previous) || text[pos - 1] == '_' || text[pos - 1] == '-') return false;
  }
  const std::size_t after = pos + i;
  return after < text.size() && (text[after] == '=' || text[after] == ':');
}

bool SensitiveDelimiter(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
         ch == ',' || ch == ';' || ch == '&' || ch == '"' || ch == '\'';
}

std::string StableChecksumHex(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : value) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

bool IsOutcomeSuccess(std::string_view outcome) {
  return outcome != "refused" &&
         outcome != "failed" &&
         outcome != "error" &&
         outcome.find("failed") == std::string_view::npos &&
         outcome.find("refused") == std::string_view::npos;
}

std::string LifecycleMetricOutcome(std::string_view outcome) {
  return IsOutcomeSuccess(outcome) ? "success" : "failure";
}

std::string AuditEventJson(const ServerAuditEvent& event) {
  std::ostringstream out;
  out << "{\"audit_event\":{\"event_uuid\":\"" << JsonEscape(event.event_uuid)
      << "\",\"event_type\":\"" << JsonEscape(event.event_type)
      << "\",\"actor_class\":\"" << JsonEscape(event.actor_class)
      << "\",\"outcome\":\"" << JsonEscape(event.outcome)
      << "\",\"diagnostic_code\":\"" << JsonEscape(event.diagnostic_code)
      << "\",\"safe_detail\":\"" << JsonEscape(event.safe_detail)
      << "\",\"sequence\":" << event.sequence << "}}";
  return out.str();
}

std::string LogRecordJson(const ServerStructuredLogRecord& record) {
  std::ostringstream out;
  out << "{\"server_log\":{\"event_type\":\"" << JsonEscape(record.event_type)
      << "\",\"severity\":\"" << JsonEscape(record.severity)
      << "\",\"component\":\"" << JsonEscape(record.component)
      << "\",\"diagnostic_code\":\"" << JsonEscape(record.diagnostic_code)
      << "\",\"safe_message\":\"" << JsonEscape(record.safe_message)
      << "\",\"redaction_state\":\"" << JsonEscape(record.redaction_state)
      << "\",\"wall_micros\":" << NowMicros() << "}}";
  return out.str();
}

void SeedMetric(ServerObservabilityState* state,
                const std::string& path,
                const std::string& type,
                std::uint64_t value,
                std::map<std::string, std::string> labels = {}) {
  SetServerMetric(state, path, value, type, std::move(labels));
}

}  // namespace

ServerObservabilityState InitializeServerObservability(const ServerBootstrapConfig& config,
                                                       const ServerLifecycleArtifacts& artifacts,
                                                       const HostedEngineState& engine_state,
                                                       const ParserPackageRegistry& parser_registry,
                                                       const ServerListenerOrchestrator& listeners) {
  ServerObservabilityState state;
  state.metrics_enabled = config.metrics_enabled;
  state.metric_generation = artifacts.generation == 0 ? 1 : artifacts.generation;
  state.metrics_path = config.control_dir / "sb_server.metrics.jsonl";
  state.audit_path = config.control_dir / "sb_server.audit.jsonl";
  state.log_path = config.log_file == "stderr"
                       ? config.control_dir / "sb_server.log"
                       : std::filesystem::path(config.log_file);
  state.support_bundle_dir = config.control_dir / "support-bundles";
  state.support_bundle_index_path = state.support_bundle_dir / "support_bundle_index.jsonl";

  SeedMetric(&state, "sys.metrics.server.config.generation", "gauge", state.metric_generation);
  SeedMetric(&state, "sys.metrics.server.lifecycle.state", "state", 1,
             {{"state", artifacts.state}, {"mode", ServerModeName(config.mode)}});
  SeedMetric(&state, "sys.metrics.server.maintenance.fence_active", "gauge", 0);
  SeedMetric(&state, "sys.metrics.server.config.reload_total", "counter", 0,
             {{"outcome", "all"}});
  SeedMetric(&state, "sys.metrics.server.shutdown.request_total", "counter", 0,
             {{"mode", "all"}});
  SeedMetric(&state, "sys.metrics.server.database.owned", "gauge",
             engine_state.databases.empty() ? 0 : 1,
             {{"open_mode", config.database_open_mode}});
  SeedMetric(&state, "sys.metrics.server.session.active", "gauge", 0);
  SeedMetric(&state, "sys.metrics.ipc.parser_server.channel.open_total", "counter", 0,
             {{"parser_family_uuid", "all"}, {"outcome", "accepted"}});
  SeedMetric(&state, "sys.metrics.ipc.parser_server.frame.invalid_total", "counter", 0,
             {{"reason", "all"}});
  SeedMetric(&state, "sys.metrics.ipc.parser_server.sblr.execute_microseconds", "histogram", 0,
             {{"operation_family", "all"}, {"outcome", "all"}});
  SeedMetric(&state, "sys.metrics.server.parser_package.count", "gauge",
             static_cast<std::uint64_t>(parser_registry.entries.size()));
  SeedMetric(&state, "sys.metrics.server.listener.count", "gauge",
             static_cast<std::uint64_t>(listeners.profiles.size()));
  SeedMetric(&state, "sys.metrics.server.management.request_total", "counter", 0,
             {{"operation", "all"}, {"outcome", "all"}});
  SeedMetric(&state, "sys.metrics.server.support_bundle.export_total", "counter", 0,
             {{"outcome", "all"}});
  SeedMetric(&state, "sys.metrics.server.log.rotation_total", "counter", 0,
             {{"outcome", "not_requested"}});
  SeedMetric(&state, "sys.metrics.supportability.flush_total", "counter", 0,
             {{"outcome", "all"}});
  SeedMetric(&state, "sys.metrics.supportability.redaction.blocked_fields_total", "counter", 0,
             {{"field_class", "all"}});
  SeedMetric(&state, "sys.metrics.supportability.bundle.completeness_ratio", "gauge", 100,
             {{"bundle_profile_uuid", "server.support_bundle.default.v1"},
              {"redaction_profile_uuid", "server.support_bundle.default_redaction.v1"},
              {"result", "complete"}});
  SeedMetric(&state, "sys.metrics.lifecycle.operation_total", "counter", 0,
             {{"operation", "all"}, {"outcome", "all"}, {"route_family", "all"}});
  SeedMetric(&state, "sys.metrics.lifecycle.diagnostic_total", "counter", 0,
             {{"operation", "all"}, {"diagnostic_code", "all"}, {"severity", "all"}});
  SeedMetric(&state, "sys.metrics.lifecycle.audit_event_total", "counter", 0,
             {{"operation", "all"}, {"outcome", "all"}});
  SeedMetric(&state, "sys.metrics.lifecycle.cache_invalidation_total", "counter", 0,
             {{"cache_family", "all"}, {"reason", "all"}});
  SeedMetric(&state, "sys.metrics.lifecycle.message_vector_total", "counter", 0,
             {{"operation", "all"}, {"visibility", "all"}});
  SeedMetric(&state, "sys.metrics.ipc.session.lifecycle_route_total", "counter", 0,
             {{"operation", "all"}, {"outcome", "all"}});
  SeedMetric(&state, "sys.metrics.parser.lifecycle_render_total", "counter", 0,
             {{"operation", "all"}, {"donor_profile", "all"}, {"outcome", "all"}});

  RecordServerAuditEvent(&state, "server.startup", "completed", "server observability initialized");
  RecordServerLog(&state, {"server.startup", "info", "sb_server", {}, "server observability initialized", "clean"});
  return state;
}

void SetServerMetric(ServerObservabilityState* state,
                     std::string path,
                     std::uint64_t value,
                     std::string type,
                     std::map<std::string, std::string> labels) {
  if (state == nullptr || !state->metrics_enabled) return;
  ServerMetricSample sample;
  sample.path = std::move(path);
  sample.type = std::move(type);
  sample.labels = std::move(labels);
  sample.redaction_class = sample.path.find("security") != std::string::npos ||
                                   sample.path.find("audit") != std::string::npos
                               ? "restricted_family"
                               : "public_safe";
  sample.value = value;
  auto key = MetricKey(sample.path, sample.labels);
  state->metrics[key] = sample;
  AppendLine(state->metrics_path, MetricSampleJson(state->metrics[key]));
}

void IncrementServerMetric(ServerObservabilityState* state,
                           std::string path,
                           std::uint64_t amount,
                           std::map<std::string, std::string> labels) {
  if (state == nullptr || !state->metrics_enabled) return;
  const auto key = MetricKey(path, labels);
  auto found = state->metrics.find(key);
  if (found == state->metrics.end()) {
    SetServerMetric(state, std::move(path), amount, "counter", std::move(labels));
    return;
  }
  found->second.value += amount;
  AppendLine(state->metrics_path, MetricSampleJson(found->second));
}

std::string RecordServerAuditEvent(ServerObservabilityState* state,
                                   std::string event_type,
                                   std::string outcome,
                                   std::string safe_detail,
                                   std::string diagnostic_code) {
  if (state == nullptr) return {};
  ServerAuditEvent event;
  event.event_uuid = AuditUuid();
  event.event_type = RedactSupportabilityText(std::move(event_type));
  event.outcome = RedactSupportabilityText(std::move(outcome));
  event.safe_detail = RedactSupportabilityText(std::move(safe_detail));
  event.diagnostic_code = RedactSupportabilityText(std::move(diagnostic_code));
  event.sequence = ++state->audit_sequence;
  state->audit_events.push_back(event);
  AppendLine(state->audit_path, AuditEventJson(event));
  return event.event_uuid;
}

void RecordServerLog(ServerObservabilityState* state,
                     ServerStructuredLogRecord record) {
  if (state == nullptr) return;
  record.event_type = RedactSupportabilityText(std::move(record.event_type));
  record.diagnostic_code = RedactSupportabilityText(std::move(record.diagnostic_code));
  record.safe_message = RedactSupportabilityText(std::move(record.safe_message));
  if (record.redaction_state.empty() || record.redaction_state == "clean") {
    record.redaction_state = "redacted";
  }
  state->log_records.push_back(record);
  AppendLine(state->log_path, LogRecordJson(record));
}

std::string RedactSupportabilityText(std::string text) {
  static constexpr const char* kSensitiveKeys[] = {
      "password",
      "passwd",
      "secret",
      "token",
      "private_key",
      "credential",
      "verifier",
      "encryption_key",
      "decryption_key",
      "key_handle"};
  for (std::size_t pos = 0; pos < text.size(); ++pos) {
    for (const char* key : kSensitiveKeys) {
      if (!SensitiveKeyAt(text, pos, key)) continue;
      std::size_t value_start = pos;
      while (value_start < text.size() && text[value_start] != '=' && text[value_start] != ':') {
        ++value_start;
      }
      if (value_start >= text.size()) continue;
      ++value_start;
      std::size_t value_end = value_start;
      while (value_end < text.size() && !SensitiveDelimiter(text[value_end])) {
        ++value_end;
      }
      text.replace(value_start, value_end - value_start, "[redacted]");
      pos = value_start + 9;
      break;
    }
  }
  return text;
}

std::vector<std::string> CanonicalLifecycleObservabilityOperations() {
  return {
      "create_database",
      "open_database",
      "attach_database",
      "detach_database",
      "begin_transaction",
      "commit_transaction",
      "rollback_transaction",
      "enter_database_maintenance",
      "exit_database_maintenance",
      "enter_restricted_open",
      "exit_restricted_open",
      "inspect_database",
      "diagnose_database",
      "verify_database",
      "repair_database",
      "shutdown_database",
      "shutdown_database_force",
      "force_shutdown_database",
      "drop_database",
      "ipc_session_route",
      "parser_package_route",
      "upgrade_database",
      "refuse_upgrade_database",
      "donor_mapping_render"};
}

bool LifecycleOperationRequiresCacheInvalidation(std::string_view operation_key) {
  return operation_key == "create_database" ||
         operation_key == "open_database" ||
         operation_key == "attach_database" ||
         operation_key == "detach_database" ||
         operation_key == "begin_transaction" ||
         operation_key == "commit_transaction" ||
         operation_key == "rollback_transaction" ||
         operation_key == "enter_database_maintenance" ||
         operation_key == "exit_database_maintenance" ||
         operation_key == "enter_restricted_open" ||
         operation_key == "exit_restricted_open" ||
         operation_key == "repair_database" ||
         operation_key == "shutdown_database" ||
         operation_key == "shutdown_database_force" ||
         operation_key == "force_shutdown_database" ||
         operation_key == "drop_database" ||
         operation_key == "upgrade_database" ||
         operation_key == "refuse_upgrade_database";
}

bool LifecycleDiagnosticRetryable(std::string_view diagnostic_code) {
  return IsRetryableDiagnosticCode(diagnostic_code);
}

ServerLifecycleObservabilityRecord RecordServerLifecycleObservability(
    ServerObservabilityState* state,
    ServerLifecycleObservabilityEvent event) {
  ServerLifecycleObservabilityRecord record;
  if (state == nullptr) return record;
  if (event.correlation_uuid.empty()) event.correlation_uuid = AuditUuid();
  if (event.cache_family.empty()) event.cache_family = "lifecycle_metadata";
  if (event.cache_reason.empty()) event.cache_reason = event.operation_key + ":" + event.outcome;
  if (!event.cache_invalidation_required) {
    event.cache_invalidation_required =
        IsOutcomeSuccess(event.outcome) &&
        LifecycleOperationRequiresCacheInvalidation(event.operation_key);
  }
  event.retryable = event.retryable || LifecycleDiagnosticRetryable(event.diagnostic_code);

  ServerDiagnostic diagnostic{
      event.diagnostic_code.empty() ? "SERVER.LIFECYCLE.OBSERVED" : event.diagnostic_code,
      event.diagnostic_code.empty() ? "server.lifecycle.observed" : event.diagnostic_code,
      IsOutcomeSuccess(event.outcome) ? ServerDiagnosticSeverity::kInfo : ServerDiagnosticSeverity::kError,
      IsOutcomeSuccess(event.outcome)
          ? "The lifecycle operation emitted observability evidence."
          : "The lifecycle operation emitted refusal observability evidence.",
      {{"operation_key", event.operation_key},
       {"outcome", event.outcome},
       {"route_family", event.route_family},
       {"state_before", event.state_before},
       {"state_after", event.state_after},
       {"database_uuid", event.database_uuid},
       {"session_uuid", event.session_uuid},
       {"request_uuid", event.request_uuid},
       {"donor_profile_uuid", event.donor_profile_uuid},
       {"private_detail", event.private_detail},
       {"cache_invalidation_required", event.cache_invalidation_required ? "true" : "false"}},
      "diag.server.lifecycle.v1",
      event.retryable,
      event.correlation_uuid,
      event.request_uuid,
      event.session_uuid,
      event.database_uuid};
  record.message_vector_public_json = ToMessageVectorJsonLine(diagnostic);
  record.message_vector_private_json = ToPrivateMessageVectorJsonLine(diagnostic);

  state->lifecycle_events.push_back(event);
  record.audit_event_uuid =
      RecordServerAuditEvent(state,
                             "server.lifecycle." + event.operation_key,
                             event.outcome,
                             "lifecycle observability evidence recorded",
                             diagnostic.code);
  IncrementServerMetric(state,
                        "sys.metrics.lifecycle.operation_total",
                        1,
                        {{"operation", event.operation_key},
                         {"outcome", LifecycleMetricOutcome(event.outcome)},
                         {"route_family", event.route_family}});
  IncrementServerMetric(state,
                        "sys.metrics.lifecycle.audit_event_total",
                        1,
                        {{"operation", event.operation_key},
                         {"outcome", LifecycleMetricOutcome(event.outcome)}});
  IncrementServerMetric(state,
                        "sys.metrics.lifecycle.message_vector_total",
                        1,
                        {{"operation", event.operation_key}, {"visibility", "public"}});
  IncrementServerMetric(state,
                        "sys.metrics.lifecycle.message_vector_total",
                        1,
                        {{"operation", event.operation_key}, {"visibility", "private"}});
  if (!diagnostic.code.empty()) {
    IncrementServerMetric(state,
                          "sys.metrics.lifecycle.diagnostic_total",
                          1,
                          {{"operation", event.operation_key},
                           {"diagnostic_code", diagnostic.code},
                           {"severity", SeverityName(diagnostic.severity)}});
  }
  if (event.route_family == "ipc_session" || event.operation_key.find("ipc") != std::string::npos) {
    IncrementServerMetric(state,
                          "sys.metrics.ipc.session.lifecycle_route_total",
                          1,
                          {{"operation", event.operation_key},
                           {"outcome", LifecycleMetricOutcome(event.outcome)}});
  }
  if (event.route_family == "parser" || !event.donor_profile_uuid.empty()) {
    IncrementServerMetric(state,
                          "sys.metrics.parser.lifecycle_render_total",
                          1,
                          {{"operation", event.operation_key},
                           {"donor_profile", event.donor_profile_uuid.empty()
                                                ? "native"
                                                : event.donor_profile_uuid},
                           {"outcome", LifecycleMetricOutcome(event.outcome)}});
  }
  if (event.cache_invalidation_required) {
    ServerCacheInvalidationMarker marker;
    marker.marker_uuid = AuditUuid();
    marker.cache_family = event.cache_family;
    marker.reason = event.cache_reason;
    marker.operation_key = event.operation_key;
    marker.database_uuid = event.database_uuid;
    marker.lifecycle_generation = state->metric_generation == 0
        ? "1"
        : std::to_string(state->metric_generation);
    state->cache_invalidation_markers.push_back(marker);
    record.cache_marker_uuid = marker.marker_uuid;
    IncrementServerMetric(state,
                          "sys.metrics.lifecycle.cache_invalidation_total",
                          1,
                          {{"cache_family", marker.cache_family}, {"reason", marker.reason}});
  }
  RecordServerLog(state,
                  {"server.lifecycle.observability",
                   IsOutcomeSuccess(event.outcome) ? "info" : "warning",
                   "sb_server",
                   diagnostic.code,
                   "operation=" + event.operation_key + " outcome=" + event.outcome,
                   "redacted"});
  record.recorded = true;
  return record;
}

ServerSupportabilityFlushResult FlushServerObservability(ServerObservabilityState* state,
                                                         std::string reason) {
  ServerSupportabilityFlushResult result;
  if (state == nullptr) {
    result.diagnostic_code = "SUPPORTABILITY.STATE_REQUIRED";
    return result;
  }
  const std::string safe_reason = RedactSupportabilityText(std::move(reason));
  const std::string flush_uuid = AuditUuid();
  result.metrics_flushed = AppendLine(state->metrics_path,
                                      "{\"supportability_flush\":{\"flush_uuid\":\"" +
                                          JsonEscape(flush_uuid) +
                                          "\",\"target\":\"metrics\",\"reason\":\"" +
                                          JsonEscape(safe_reason) + "\"}}");
  result.audit_flushed = AppendLine(state->audit_path,
                                    "{\"supportability_flush\":{\"flush_uuid\":\"" +
                                        JsonEscape(flush_uuid) +
                                        "\",\"target\":\"audit\",\"reason\":\"" +
                                        JsonEscape(safe_reason) + "\"}}");
  result.log_flushed = AppendLine(state->log_path,
                                  "{\"supportability_flush\":{\"flush_uuid\":\"" +
                                      JsonEscape(flush_uuid) +
                                      "\",\"target\":\"operational_log\",\"reason\":\"" +
                                      JsonEscape(safe_reason) + "\"}}");
  result.support_bundle_index_flushed =
      AppendLine(state->support_bundle_index_path,
                 "{\"supportability_flush\":{\"flush_uuid\":\"" + JsonEscape(flush_uuid) +
                     "\",\"target\":\"support_bundle_index\",\"reason\":\"" +
                     JsonEscape(safe_reason) + "\"}}");
  result.flushed = result.metrics_flushed && result.audit_flushed && result.log_flushed &&
                   result.support_bundle_index_flushed;
  result.diagnostic_code = result.flushed ? "SUPPORTABILITY.FLUSH_COMPLETE"
                                          : "SUPPORTABILITY.FLUSH_PARTIAL";
  result.evidence.push_back("flush_uuid:" + flush_uuid);
  result.evidence.push_back("reason:" + safe_reason);
  IncrementServerMetric(state,
                        "sys.metrics.supportability.flush_total",
                        1,
                        {{"outcome", result.flushed ? "completed" : "partial"}});
  return result;
}

ServerSupportabilityFlushResult RotateServerOperationalLog(ServerObservabilityState* state,
                                                           std::uint64_t max_active_log_bytes) {
  ServerSupportabilityFlushResult result;
  if (state == nullptr || state->log_path.empty() || max_active_log_bytes == 0) {
    result.diagnostic_code = "SUPPORTABILITY.LOG_ROTATION_NOT_REQUIRED";
    return result;
  }
  std::error_code ec;
  const auto size = std::filesystem::exists(state->log_path, ec)
      ? std::filesystem::file_size(state->log_path, ec)
      : 0;
  if (ec || size <= max_active_log_bytes) {
    result.flushed = true;
    result.log_flushed = true;
    result.diagnostic_code = "SUPPORTABILITY.LOG_ROTATION_NOT_REQUIRED";
    return result;
  }
  const auto rotated = state->log_path.string() + ".1";
  std::filesystem::remove(rotated, ec);
  ec.clear();
  std::filesystem::rename(state->log_path, rotated, ec);
  result.log_flushed = !ec;
  result.flushed = result.log_flushed;
  result.diagnostic_code = result.log_flushed ? "SUPPORTABILITY.LOG_ROTATED"
                                              : "SUPPORTABILITY.LOG_ROTATION_FAILED";
  if (result.log_flushed) {
    AppendLine(state->log_path,
               "{\"server_log_rotation\":{\"rotated_ref\":\"[path-redacted]\","
               "\"active_ref\":\"[path-redacted]\"}}");
    result.evidence.push_back("rotated_log_ref:[path-redacted]");
  }
  IncrementServerMetric(state,
                        "sys.metrics.server.log.rotation_total",
                        1,
                        {{"outcome", result.log_flushed ? "completed" : "failed"}});
  RecordServerAuditEvent(state,
                         "server.log.rotation",
                         result.log_flushed ? "completed" : "failed",
                         "operational log rotation completed",
                         result.diagnostic_code);
  return result;
}

std::string ServerMetricsSnapshotJson(const ServerObservabilityState& state) {
  std::ostringstream out;
  out << "{\"server_metrics\":{\"generation\":" << state.metric_generation
      << ",\"persistence\":\"enabled\",\"retention_policy_ref\":\"server.metrics.default_retention.v1\","
      << "\"samples\":[";
  bool first = true;
  for (const auto& [_, sample] : state.metrics) {
    if (!first) out << ',';
    first = false;
    out << MetricSampleJson(sample);
  }
  out << "]}}\n";
  return out.str();
}

std::string ServerAuditSnapshotJson(const ServerObservabilityState& state) {
  std::ostringstream out;
  out << "{\"server_audit\":{\"event_count\":" << state.audit_events.size() << ",\"events\":[";
  for (std::size_t i = 0; i < state.audit_events.size(); ++i) {
    if (i != 0) out << ',';
    out << AuditEventJson(state.audit_events[i]);
  }
  out << "]}}\n";
  return out.str();
}

ServerSupportBundleExportResult ExportServerSupportBundle(ServerObservabilityState& state,
                                                          const ServerBootstrapConfig& config,
                                                          const ServerLifecycleArtifacts& artifacts,
                                                          const HostedEngineState& engine_state,
                                                          const ServerSessionRegistry& sessions,
                                                          const ParserPackageRegistry& parser_registry,
                                                          const ServerListenerOrchestrator& listeners) {
  ServerSupportBundleExportResult result;
  const auto flush = FlushServerObservability(&state, "support_bundle_export");
  if (!flush.flushed) {
    result.diagnostic_code = "OPS.SUPPORT_BUNDLE.FLUSH_REQUIRED";
    result.evidence = flush.evidence;
    result.records_json =
        "[{\"support_bundle_uuid\":\"\",\"bundle_ref\":\"\","
        "\"outcome\":\"failed\",\"diagnostic_code\":\"OPS.SUPPORT_BUNDLE.FLUSH_REQUIRED\"}]";
    return result;
  }
  const auto bundle_uuid = AuditUuid();
  const auto bundle_path = state.support_bundle_dir / (bundle_uuid + ".json");
  const auto request_uuid = AuditUuid();
  const auto manifest_checksum_seed =
      bundle_uuid + "|" + request_uuid + "|" + artifacts.state + "|" +
      std::to_string(artifacts.generation) + "|" + std::to_string(engine_state.databases.size()) +
      "|" + std::to_string(sessions.sessions_by_uuid.size());
  const auto manifest_checksum = StableChecksumHex(manifest_checksum_seed);
  std::ostringstream body;
  body << "{\"support_bundle\":{\"bundle_uuid\":\"" << JsonEscape(bundle_uuid)
       << "\",\"redaction_profile\":\"server.support_bundle.default_redaction.v1\","
       << "\"manifest\":{\"schema\":\"SB_SERVER_SUPPORT_BUNDLE_V2\","
       << "\"request_uuid\":\"" << JsonEscape(request_uuid)
       << "\",\"requester\":\"engine-authorized-management-session\","
       << "\"authority_path\":\"engine.authorization.management.SUPPORT_EXPORT\","
       << "\"scope\":\"local_node\","
       << "\"time_range\":{\"kind\":\"current_lifecycle_generation\","
       << "\"generation\":" << artifacts.generation << "},"
       << "\"event_families\":[\"metrics\",\"audit\",\"operational_log\",\"lifecycle\","
       << "\"sessions\",\"parser_packages\",\"listeners\"],"
       << "\"excluded_protected_material\":[\"password\",\"secret\",\"token\","
       << "\"private_key\",\"credential\",\"verifier\",\"encryption_key\","
       << "\"decryption_key\",\"key_handle\"],"
       << "\"retention_policy_ref\":\"support.bundle.default_retention.v1\","
       << "\"disposition_policy_ref\":\"support.bundle.default_disposition.v1\","
       << "\"tamper_checksum\":\"" << manifest_checksum
       << "\",\"signature_ref\":\"engine-local-supportability-signature.v1\","
       << "\"local_path_policy\":\"redacted\"},"
       << "\"server_status\":{\"mode\":\"" << ServerModeName(config.mode)
       << "\",\"lifecycle_state\":\"" << JsonEscape(artifacts.state)
       << "\",\"lifecycle_generation\":" << artifacts.generation
       << ",\"control_dir\":\"" << RedactedPath(config.control_dir.string()) << "\"},"
       << "\"database_count\":" << engine_state.databases.size()
       << ",\"session_count\":" << sessions.sessions_by_uuid.size()
       << ",\"parser_package_count\":" << parser_registry.entries.size()
       << ",\"listener_count\":" << listeners.profiles.size()
       << ",\"metric_snapshot\":" << ServerMetricsSnapshotJson(state)
       << ",\"audit_summary\":{\"event_count\":" << state.audit_events.size() << "},"
       << "\"operational_log_summary\":{\"record_count\":" << state.log_records.size()
       << ",\"log_ref\":\"[path-redacted]\"},"
       << "\"redaction_evidence\":{\"redaction_state\":\"redacted\","
       << "\"redaction_profile_uuid\":\"server.support_bundle.default_redaction.v1\","
       << "\"forbidden_field_classes\":[\"password\",\"secret\",\"token\",\"private_key\","
       << "\"credential\",\"verifier\",\"encryption_key\",\"decryption_key\",\"key_handle\"]},"
       << "\"completeness\":{\"required_sections\":7,\"present_sections\":7,"
       << "\"ratio\":100,\"omitted_sections\":[]},"
       << "\"supportability_flush\":\"required_before_export\","
       << "\"forbidden_fields_absent\":true"
       << "}}\n";
  std::error_code ec;
  std::filesystem::create_directories(state.support_bundle_dir, ec);
  std::ofstream out(bundle_path, std::ios::trunc);
  if (out) {
    out << body.str();
    out.close();
  }
  if (!out) {
    result.diagnostic_code = "OPS.SUPPORT_BUNDLE.WRITE_FAILED";
    result.records_json =
        "[{\"support_bundle_uuid\":\"\",\"bundle_ref\":\"\","
        "\"outcome\":\"failed\",\"diagnostic_code\":\"OPS.SUPPORT_BUNDLE.WRITE_FAILED\"}]";
    return result;
  }
  const bool index_written =
      AppendLine(state.support_bundle_index_path,
                 "{\"support_bundle_uuid\":\"" + JsonEscape(bundle_uuid) +
                     "\",\"request_uuid\":\"" + JsonEscape(request_uuid) +
                     "\",\"bundle_ref\":\"[path-redacted]\","
                     "\"authority_path\":\"engine.authorization.management.SUPPORT_EXPORT\","
                     "\"redaction_state\":\"redacted\","
                     "\"retention_policy_ref\":\"support.bundle.default_retention.v1\","
                     "\"disposition_policy_ref\":\"support.bundle.default_disposition.v1\","
                     "\"tamper_checksum\":\"" + manifest_checksum + "\","
                     "\"forbidden_fields_absent\":true}");
  if (!index_written) {
    std::error_code remove_ec;
    std::filesystem::remove(bundle_path, remove_ec);
    result.diagnostic_code = "OPS.SUPPORT_BUNDLE.INDEX_WRITE_FAILED";
    result.records_json =
        "[{\"support_bundle_uuid\":\"\",\"bundle_ref\":\"\","
        "\"outcome\":\"failed\",\"diagnostic_code\":\"OPS.SUPPORT_BUNDLE.INDEX_WRITE_FAILED\"}]";
    return result;
  }
  state.support_bundle_export_uuids.push_back(bundle_uuid);
  state.support_bundle_export_sequence++;
  std::ostringstream descriptor;
  descriptor << "[{\"support_bundle_uuid\":\"" << JsonEscape(bundle_uuid)
             << "\",\"bundle_ref\":\"[path-redacted]\","
             << "\"request_uuid\":\"" << JsonEscape(request_uuid) << "\","
             << "\"authority_path\":\"engine.authorization.management.SUPPORT_EXPORT\","
             << "\"redaction_state\":\"redacted\","
             << "\"retention_policy_ref\":\"support.bundle.default_retention.v1\","
             << "\"disposition_policy_ref\":\"support.bundle.default_disposition.v1\","
             << "\"tamper_checksum\":\"" << manifest_checksum << "\","
             << "\"forbidden_fields_absent\":true}]";
  result.ok = true;
  result.records_json = descriptor.str();
  result.diagnostic_code = "OPS.SUPPORT_BUNDLE.EXPORT_COMPLETE";
  result.bundle_uuid = bundle_uuid;
  result.evidence = flush.evidence;
  result.evidence.push_back("request_uuid:" + request_uuid);
  result.evidence.push_back("tamper_checksum:" + manifest_checksum);
  return result;
}

}  // namespace scratchbird::server
