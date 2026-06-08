// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_support_bundle.hpp"

#include "listener_diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

namespace scratchbird::listener {
namespace {

std::string LowerAscii(std::string_view value) {
  std::string out(value);
  for (char& ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

bool ContainsAny(std::string_view value, std::initializer_list<std::string_view> needles) {
  const auto lower = LowerAscii(value);
  for (const auto needle : needles) {
    if (lower.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool LooksLikeLocalPath(std::string_view value) {
  return value.find("/home/") != std::string_view::npos ||
         value.find("/tmp/") != std::string_view::npos ||
         value.find("\\") != std::string_view::npos ||
         value.find(".sock") != std::string_view::npos ||
         value.find(".sbdb") != std::string_view::npos;
}

std::string RedactedConfigValue(std::string_view value, std::string_view empty_value = "") {
  if (value.empty()) return std::string(empty_value);
  const auto redacted = RedactListenerSupportabilityText(value);
  if (redacted != value) return redacted;
  return "[redacted]";
}

std::string RedactedMetricsJson(std::string_view value) {
  if (value.empty()) return "{}";
  const auto redacted = RedactListenerSupportabilityText(value);
  if (redacted == value) return std::string(value);
  return "{\"redacted\":true,\"reason\":\"" + QuoteJson(redacted) + "\"}";
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

template <typename T>
std::vector<T> LastBounded(const std::vector<T>& input, std::size_t max_count) {
  if (input.size() <= max_count) return input;
  return std::vector<T>(input.end() - static_cast<std::ptrdiff_t>(max_count), input.end());
}

void AppendSupportEventJson(std::ostringstream* out, const ListenerSupportBundleEvent& event) {
  *out << "{\"timestamp_ms\":" << event.timestamp_ms
       << ",\"event_type\":\"" << QuoteJson(event.event_type) << "\""
       << ",\"operation\":\"" << QuoteJson(event.operation) << "\""
       << ",\"outcome\":\"" << QuoteJson(event.outcome) << "\""
       << ",\"diagnostic_code\":\"" << QuoteJson(event.diagnostic_code) << "\""
       << ",\"safe_detail\":\""
       << QuoteJson(RedactListenerSupportabilityText(event.safe_detail)) << "\"}";
}

void AppendFaultHistoryJson(std::ostringstream* out, const ParserPoolStatus& status) {
  const auto faults = LastBounded(status.fault_history, kListenerSupportBundleHistoryMax);
  *out << "\"parser_worker_faults\":{\"count\":" << faults.size()
       << ",\"source_count\":" << status.fault_history.size()
       << ",\"max\":" << kListenerSupportBundleHistoryMax
       << ",\"bounded\":true,\"events\":[";
  for (std::size_t i = 0; i < faults.size(); ++i) {
    const auto& fault = faults[i];
    if (i != 0) *out << ',';
    *out << "{\"timestamp_ms\":" << fault.timestamp_ms
         << ",\"worker_id\":\"" << QuoteJson(fault.worker_id) << "\""
         << ",\"generation\":" << fault.generation
         << ",\"numeric_worker_id\":" << fault.numeric_worker_id
         << ",\"event\":\"" << QuoteJson(fault.event) << "\""
         << ",\"diagnostic\":\""
         << QuoteJson(RedactListenerSupportabilityText(fault.diagnostic)) << "\""
         << ",\"backoff_ms\":" << fault.backoff_ms
         << ",\"next_retry_at_ms\":" << fault.next_retry_at_ms
         << ",\"quarantine_active\":" << BoolText(fault.quarantine_active)
         << ",\"quarantine_until_ms\":" << fault.quarantine_until_ms
         << ",\"recent_failure_count\":" << fault.recent_failure_count
         << ",\"intentional\":" << BoolText(fault.intentional) << "}";
  }
  *out << "]}";
}

void AppendSupportEventsSection(std::ostringstream* out,
                                std::string_view key,
                                const std::vector<ListenerSupportBundleEvent>& events) {
  const auto bounded = LastBounded(events, kListenerSupportBundleHistoryMax);
  *out << "\"" << key << "\":{\"count\":" << bounded.size()
       << ",\"source_count\":" << events.size()
       << ",\"max\":" << kListenerSupportBundleHistoryMax
       << ",\"bounded\":true,\"events\":[";
  for (std::size_t i = 0; i < bounded.size(); ++i) {
    if (i != 0) *out << ',';
    AppendSupportEventJson(out, bounded[i]);
  }
  *out << "]}";
}

bool FileExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

}  // namespace

std::string RedactListenerSupportabilityText(std::string_view text) {
  if (text.empty()) return "";
  if (ContainsAny(text,
                  {"password",
                   "secret",
                   "token",
                   "private_key",
                   "credential",
                   "verifier",
                   "encryption_key",
                   "decryption_key",
                   "key_handle",
                   "auth"})) {
    return "[redacted:security]";
  }
  if (LooksLikeLocalPath(text)) return "[path-redacted]";
  return std::string(text);
}

std::string BuildListenerSupportBundleJson(const ListenerSupportBundleSnapshot& snapshot) {
  const auto runtime_events = LastBounded(snapshot.runtime_events, kListenerSupportBundleHistoryMax);
  std::uint64_t handoff_failures = 0;
  std::uint64_t auth_refusals = 0;
  for (const auto& event : runtime_events) {
    if (event.event_type == "handoff_failure") ++handoff_failures;
    if (event.event_type == "auth_refusal") ++auth_refusals;
  }

  std::ostringstream out;
  out << "{\"listener_support_bundle\":{"
      << "\"schema\":\"SB_LISTENER_SUPPORT_BUNDLE_V1\","
      << "\"redaction_profile\":\"listener.support_bundle.default_redaction.v1\","
      << "\"support_bundle_is_authority\":false,"
      << "\"authority_path\":\"listener.management.SUPPORT_EXPORT\","
      << "\"scope\":\"local_listener\","
      << "\"forbidden_fields_absent\":true,"
      << "\"local_path_policy\":\"redacted\","
      << "\"excluded_protected_material\":[\"password\",\"secret\",\"token\","
      << "\"private_key\",\"credential\",\"verifier\",\"encryption_key\","
      << "\"decryption_key\",\"key_handle\"],"
      << "\"listener_status\":{\"listener_uuid\":\""
      << QuoteJson(snapshot.identity.listener_uuid)
      << "\",\"profile\":\"" << QuoteJson(snapshot.identity.profile)
      << "\",\"protocol_family\":\"" << QuoteJson(snapshot.config.protocol_family)
      << "\",\"lifecycle_state\":\"" << QuoteJson(snapshot.lifecycle_state)
      << "\",\"draining\":" << BoolText(snapshot.draining)
      << ",\"stop_requested\":" << BoolText(snapshot.stop_requested)
      << ",\"accepting_new_connections\":" << BoolText(snapshot.accepting_new_connections)
      << ",\"last_accept_sequence\":" << snapshot.last_accept_sequence
      << ",\"open_connections\":" << snapshot.open_connections
      << ",\"queue_depth\":" << snapshot.queue_depth
      << ",\"pending_handoff_bindings\":" << snapshot.pending_handoff_bindings
      << ",\"handoff_complete_total\":" << snapshot.handoff_complete_total
      << ",\"reject_total\":" << snapshot.reject_total
      << ",\"identity\":{\"endpoint_hash\":\"" << QuoteJson(snapshot.identity.endpoint_hash)
      << "\",\"generation\":\"" << QuoteJson(snapshot.identity.generation)
      << "\",\"control_socket\":\"[path-redacted]\","
      << "\"management_socket\":\"[path-redacted]\"}},"
      << "\"configuration_redacted\":{\"database_selector\":\""
      << QuoteJson(RedactedConfigValue(snapshot.config.database_selector))
      << "\",\"server_endpoint\":\"" << QuoteJson(RedactedConfigValue(snapshot.config.server_endpoint))
      << "\",\"parser_executable\":\"" << QuoteJson(RedactedConfigValue(snapshot.config.parser_executable))
      << "\",\"control_dir\":\"[path-redacted]\","
      << "\"runtime_dir\":\"[path-redacted]\","
      << "\"tls_cert_file\":\"" << QuoteJson(RedactedConfigValue(snapshot.config.tls_cert_file))
      << "\",\"tls_key_file\":\"" << QuoteJson(RedactedConfigValue(snapshot.config.tls_key_file))
      << "\",\"tls_ca_file\":\"" << QuoteJson(RedactedConfigValue(snapshot.config.tls_ca_file))
      << "\",\"bundle_contract_id\":\""
      << QuoteJson(RedactListenerSupportabilityText(snapshot.config.bundle_contract_id))
      << "\"},"
      << "\"lifecycle_evidence\":{\"owner_artifact_present\":"
      << BoolText(FileExists(snapshot.identity.owner_file))
      << ",\"lifecycle_artifact_present\":"
      << BoolText(FileExists(snapshot.identity.lifecycle_file))
      << ",\"artifact_paths_redacted\":true},"
      << "\"parser_pool\":{\"running\":" << BoolText(snapshot.pool_status.running)
      << ",\"draining\":" << BoolText(snapshot.pool_status.draining)
      << ",\"active_worker_count\":" << snapshot.pool_status.active_worker_count
      << ",\"busy_worker_count\":" << snapshot.pool_status.busy_worker_count
      << ",\"running_worker_count\":" << snapshot.pool_status.running_worker_count
      << ",\"recent_failure_count\":" << snapshot.pool_status.recent_failure_count
      << ",\"last_backoff_ms\":" << snapshot.pool_status.last_backoff_ms
      << ",\"quarantine_active\":" << BoolText(snapshot.pool_status.quarantine_active)
      << ",\"worker_count\":" << snapshot.pool_status.workers.size() << "},";
  AppendFaultHistoryJson(&out, snapshot.pool_status);
  out << ',';
  AppendSupportEventsSection(&out, "management_decisions", snapshot.management_decisions);
  out << ',';
  AppendSupportEventsSection(&out, "runtime_events", snapshot.runtime_events);
  out << ",\"runtime_summary\":{\"handoff_failures\":" << handoff_failures
      << ",\"auth_refusals\":" << auth_refusals
      << ",\"metrics\":" << RedactedMetricsJson(snapshot.metrics_json)
      << "},\"redaction_evidence\":{\"redaction_state\":\"redacted\","
      << "\"redacted_markers\":[\"[redacted:security]\",\"[path-redacted]\"],"
      << "\"forbidden_field_classes\":[\"password\",\"secret\",\"token\","
      << "\"private_key\",\"credential\",\"verifier\",\"encryption_key\","
      << "\"decryption_key\",\"key_handle\"]},"
      << "\"completeness\":{\"required_sections\":8,\"present_sections\":8,"
      << "\"ratio\":100,\"omitted_sections\":[]}}}";
  return out.str();
}

const char* listener_support_bundle_implementation_anchor() {
  return "listener_support_bundle.v1";
}

}  // namespace scratchbird::listener
