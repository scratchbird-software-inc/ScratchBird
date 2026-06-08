// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_RUNTIME_SNAPSHOT_MODULE

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::manager::node {

struct ManagerStatusSnapshot {
  std::string lifecycle_state;
  std::size_t active_clients = 0;
  std::uint64_t accepted_clients = 0;
  std::uint64_t rejected_clients = 0;
  std::uint64_t proxy_bytes_client_to_backend = 0;
  std::uint64_t proxy_bytes_backend_to_client = 0;
  std::size_t management_clients_active = 0;
  std::uint64_t management_clients_rejected = 0;
  std::uint64_t management_requests_total = 0;
  std::uint64_t audit_sequence = 0;
  std::uint64_t audit_bytes = 0;
  std::uint64_t audit_write_failures = 0;
  std::uint64_t metrics_publish_failures = 0;
  std::string health_state;
  std::uint64_t heartbeat_success = 0;
  std::uint64_t heartbeat_failure = 0;
  std::uint64_t missed_heartbeat_count = 0;
  bool restart_enabled = false;
  std::uint64_t restart_attempts = 0;
  std::uint64_t restart_refusals = 0;
  bool restart_quarantined = false;
  std::uint64_t next_restart_allowed_ms = 0;
  std::string last_restart_reason;
};

struct ManagerMetricsSnapshot {
  std::uint64_t wall_time_ms = 0;
  std::string lifecycle_state;
  std::size_t active_clients = 0;
  std::uint64_t accepted_clients = 0;
  std::uint64_t rejected_clients = 0;
  std::uint64_t proxy_bytes_client_to_backend = 0;
  std::uint64_t proxy_bytes_backend_to_client = 0;
  std::size_t management_clients_active = 0;
  std::uint64_t management_clients_rejected = 0;
  std::uint64_t management_requests_total = 0;
  std::uint64_t audit_sequence = 0;
  std::uint64_t audit_bytes = 0;
  std::uint64_t audit_write_failures = 0;
  std::uint64_t metrics_publish_failures = 0;
  std::string listener_profile_state;
  std::uint64_t listener_control_requests_total = 0;
  std::uint64_t listener_control_failures_total = 0;
  std::uint64_t support_bundle_requests_total = 0;
  std::uint64_t support_bundle_failures_total = 0;
  std::uint64_t heartbeat_success = 0;
  std::uint64_t heartbeat_failure = 0;
  std::string health_state;
  std::uint64_t restart_attempts = 0;
  std::uint64_t restart_refusals = 0;
  bool restart_quarantined = false;
};

struct ManagerAuditRecord {
  std::string audit_event_uuid_hex;
  std::uint64_t audit_sequence = 0;
  std::uint64_t wall_time_ms = 0;
  std::string operation;
  bool success = false;
  std::string diagnostic_code;
  std::string lifecycle_state;
  std::string record_checksum;
  std::vector<std::pair<std::string, std::string>> fields;
};

std::string RenderManagerStatusJson(const ManagerStatusSnapshot& snapshot);
std::string RenderManagerMetricsJson(const ManagerMetricsSnapshot& snapshot);
std::string RenderManagerAuditJsonLine(const ManagerAuditRecord& record);

} // namespace scratchbird::manager::node
