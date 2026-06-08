// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_RUNTIME_SNAPSHOT_MODULE

#include "manager_runtime_snapshot.hpp"

#include "manager_protocol.hpp"

#include <cctype>
#include <sstream>
#include <string_view>

namespace scratchbird::manager::node {
namespace proto = scratchbird::manager::protocol;
namespace {

std::string BoolText(bool value) { return value ? "true" : "false"; }

bool ContainsInsensitive(std::string_view text, std::string_view needle) {
  if (needle.empty() || text.size() < needle.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      const auto lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i + j])));
      const auto rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
      if (lhs != rhs) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

std::string RedactAuditFieldValue(const std::string& key, const std::string& value) {
  static constexpr std::string_view kSensitiveKeys[] = {
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
  static constexpr std::string_view kPathKeys[] = {
      "config_ref",
      "path",
      "dir",
      "file",
      "socket"};
  for (const auto sensitive : kSensitiveKeys) {
    if (ContainsInsensitive(key, sensitive)) return value.empty() ? "" : "[redacted]";
  }
  for (const auto path_key : kPathKeys) {
    if (ContainsInsensitive(key, path_key)) return value.empty() ? "" : "[path-redacted]";
  }
  return value;
}

} // namespace

std::string RenderManagerStatusJson(const ManagerStatusSnapshot& snapshot) {
  std::ostringstream out;
  out << "{\"product\":\"sbmn_manager\",\"state\":\"" << proto::JsonEscape(snapshot.lifecycle_state) << "\",\"proxy_clients_active\":" << snapshot.active_clients
      << ",\"proxy_clients_accepted\":" << snapshot.accepted_clients
      << ",\"proxy_clients_rejected\":" << snapshot.rejected_clients
      << ",\"proxy_bytes_client_to_backend\":" << snapshot.proxy_bytes_client_to_backend
      << ",\"proxy_bytes_backend_to_client\":" << snapshot.proxy_bytes_backend_to_client
      << ",\"management_clients_active\":" << snapshot.management_clients_active
      << ",\"management_clients_rejected\":" << snapshot.management_clients_rejected
      << ",\"management_requests_total\":" << snapshot.management_requests_total
      << ",\"audit_sequence\":" << snapshot.audit_sequence
      << ",\"audit_bytes\":" << snapshot.audit_bytes
      << ",\"audit_write_failures\":" << snapshot.audit_write_failures
      << ",\"metrics_publish_failures\":" << snapshot.metrics_publish_failures
      << ",\"server_health_state\":\"" << proto::JsonEscape(snapshot.health_state) << "\",\"heartbeat_success\":" << snapshot.heartbeat_success
      << ",\"heartbeat_failure\":" << snapshot.heartbeat_failure
      << ",\"heartbeat_missed_current\":" << snapshot.missed_heartbeat_count
      << ",\"restart_enabled\":" << BoolText(snapshot.restart_enabled)
      << ",\"restart_attempts\":" << snapshot.restart_attempts
      << ",\"restart_refusals\":" << snapshot.restart_refusals
      << ",\"restart_quarantined\":" << BoolText(snapshot.restart_quarantined)
      << ",\"restart_next_allowed_ms\":" << snapshot.next_restart_allowed_ms
      << ",\"restart_last_reason\":\"" << proto::JsonEscape(snapshot.last_restart_reason) << "\"}\n";
  return out.str();
}

std::string RenderManagerMetricsJson(const ManagerMetricsSnapshot& snapshot) {
  std::ostringstream out;
  out << "{\"format\":\"SBMN_MANAGER_METRICS_V1\",\"wall_time_ms\":" << snapshot.wall_time_ms
      << ",\"metrics\":["
      << "{\"name\":\"sb_manager_lifecycle_state\",\"value\":\"" << proto::JsonEscape(snapshot.lifecycle_state) << "\"},"
      << "{\"name\":\"sb_manager_proxy_clients_active\",\"value\":" << snapshot.active_clients << "},"
      << "{\"name\":\"sb_manager_proxy_clients_accepted_total\",\"value\":" << snapshot.accepted_clients << "},"
      << "{\"name\":\"sb_manager_proxy_clients_rejected_total\",\"value\":" << snapshot.rejected_clients << "},"
      << "{\"name\":\"sb_manager_proxy_bytes_total\",\"labels\":{\"direction\":\"client_to_backend\"},\"value\":" << snapshot.proxy_bytes_client_to_backend << "},"
      << "{\"name\":\"sb_manager_proxy_bytes_total\",\"labels\":{\"direction\":\"backend_to_client\"},\"value\":" << snapshot.proxy_bytes_backend_to_client << "},"
      << "{\"name\":\"sb_manager_management_clients_active\",\"value\":" << snapshot.management_clients_active << "},"
      << "{\"name\":\"sb_manager_management_clients_rejected_total\",\"value\":" << snapshot.management_clients_rejected << "},"
      << "{\"name\":\"sb_manager_management_requests_total\",\"value\":" << snapshot.management_requests_total << "},"
      << "{\"name\":\"sb_manager_audit_events_total\",\"value\":" << snapshot.audit_sequence << "},"
      << "{\"name\":\"sb_manager_audit_bytes\",\"value\":" << snapshot.audit_bytes << "},"
      << "{\"name\":\"sb_manager_audit_write_failures_total\",\"value\":" << snapshot.audit_write_failures << "},"
      << "{\"name\":\"sb_manager_metrics_publish_failures_total\",\"value\":" << snapshot.metrics_publish_failures << "},"
      << "{\"name\":\"sb_manager_listener_profile_state\",\"value\":\"" << proto::JsonEscape(snapshot.listener_profile_state) << "\"},"
      << "{\"name\":\"sb_manager_listener_control_requests_total\",\"value\":" << snapshot.listener_control_requests_total << "},"
      << "{\"name\":\"sb_manager_listener_control_failures_total\",\"value\":" << snapshot.listener_control_failures_total << "},"
      << "{\"name\":\"sb_manager_support_bundle_requests_total\",\"value\":" << snapshot.support_bundle_requests_total << "},"
      << "{\"name\":\"sb_manager_support_bundle_failures_total\",\"value\":" << snapshot.support_bundle_failures_total << "},"
      << "{\"name\":\"sb_manager_server_heartbeat_success_total\",\"value\":" << snapshot.heartbeat_success << "},"
      << "{\"name\":\"sb_manager_server_heartbeat_failure_total\",\"value\":" << snapshot.heartbeat_failure << "},"
      << "{\"name\":\"sb_manager_server_health_state\",\"value\":\"" << proto::JsonEscape(snapshot.health_state) << "\"},"
      << "{\"name\":\"sb_manager_server_restart_attempts_total\",\"value\":" << snapshot.restart_attempts << "},"
      << "{\"name\":\"sb_manager_server_restart_refused_total\",\"value\":" << snapshot.restart_refusals << "},"
      << "{\"name\":\"sb_manager_server_crash_loop_quarantine_state\",\"value\":\"" << (snapshot.restart_quarantined ? "quarantined" : "clear") << "\"},"
      << "{\"name\":\"sb_manager_no_spin_violation_detected_total\",\"value\":0}"
      << "]}\n";
  return out.str();
}

std::string RenderManagerAuditJsonLine(const ManagerAuditRecord& record) {
  std::ostringstream out;
  out << "{\"format\":\"SBMN_MANAGER_AUDIT_V1\""
      << ",\"audit_event_uuid\":\"" << proto::JsonEscape(record.audit_event_uuid_hex) << "\""
      << ",\"audit_sequence\":" << record.audit_sequence
      << ",\"wall_time_ms\":" << record.wall_time_ms
      << ",\"operation\":\"" << proto::JsonEscape(record.operation) << "\""
      << ",\"success\":" << BoolText(record.success)
      << ",\"diagnostic_code\":\"" << proto::JsonEscape(record.diagnostic_code) << "\""
      << ",\"redaction_class\":\"operational\""
      << ",\"state\":\"" << proto::JsonEscape(record.lifecycle_state) << "\""
      << ",\"record_checksum\":\"" << proto::JsonEscape(record.record_checksum) << "\""
      << ",\"fields\":{";
  bool first = true;
  for (const auto& field : record.fields) {
    if (!first) out << ",";
    first = false;
    out << "\"" << proto::JsonEscape(field.first) << "\":\""
        << proto::JsonEscape(RedactAuditFieldValue(field.first, field.second)) << "\"";
  }
  out << "}}\n";
  return out.str();
}

} // namespace scratchbird::manager::node
