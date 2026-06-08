// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_OBSERVABILITY_AUDIT_SUPPORT

#pragma once

#include "config.hpp"
#include "diagnostics.hpp"
#include "engine_host.hpp"
#include "lifecycle.hpp"
#include "listener_orchestrator.hpp"
#include "parser_package_registry.hpp"
#include "session_registry.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::server {

struct ServerMetricSample {
  std::string path;
  std::string type = "counter";
  std::map<std::string, std::string> labels;
  std::uint64_t value = 0;
  std::string visibility_right = "METRICS_INSPECT";
  std::string redaction_class = "public_safe";
};

struct ServerAuditEvent {
  std::string event_uuid;
  std::string event_type;
  std::string actor_class = "server";
  std::string outcome = "completed";
  std::string diagnostic_code;
  std::string safe_detail;
  std::uint64_t sequence = 0;
};

struct ServerStructuredLogRecord {
  std::string event_type;
  std::string severity = "info";
  std::string component = "sb_server";
  std::string diagnostic_code;
  std::string safe_message;
  std::string redaction_state = "clean";
};

struct ServerCacheInvalidationMarker {
  std::string marker_uuid;
  std::string cache_family;
  std::string reason;
  std::string operation_key;
  std::string database_uuid;
  std::string lifecycle_generation;
};

struct ServerLifecycleObservabilityEvent {
  std::string operation_key;
  std::string outcome;
  std::string diagnostic_code;
  std::string route_family = "server_management";
  std::string request_uuid;
  std::string session_uuid;
  std::string database_uuid;
  std::string correlation_uuid;
  std::string state_before;
  std::string state_after;
  std::string private_detail;
  std::string donor_profile_uuid;
  bool cache_invalidation_required = false;
  std::string cache_family;
  std::string cache_reason;
  bool retryable = false;
};

struct ServerLifecycleObservabilityRecord {
  bool recorded = false;
  std::string audit_event_uuid;
  std::string message_vector_public_json;
  std::string message_vector_private_json;
  std::string cache_marker_uuid;
};

struct ServerObservabilityState {
  bool metrics_enabled = true;
  std::uint64_t metric_generation = 1;
  std::uint64_t audit_sequence = 0;
  std::uint64_t support_bundle_export_sequence = 0;
  std::filesystem::path metrics_path;
  std::filesystem::path audit_path;
  std::filesystem::path log_path;
  std::filesystem::path support_bundle_dir;
  std::filesystem::path support_bundle_index_path;
  std::map<std::string, ServerMetricSample> metrics;
  std::vector<ServerAuditEvent> audit_events;
  std::vector<ServerStructuredLogRecord> log_records;
  std::vector<std::string> support_bundle_export_uuids;
  std::vector<ServerLifecycleObservabilityEvent> lifecycle_events;
  std::vector<ServerCacheInvalidationMarker> cache_invalidation_markers;
};

struct ServerSupportabilityFlushResult {
  bool flushed = false;
  bool metrics_flushed = false;
  bool audit_flushed = false;
  bool log_flushed = false;
  bool support_bundle_index_flushed = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

struct ServerSupportBundleExportResult {
  bool ok = false;
  std::string records_json = "[]";
  std::string diagnostic_code;
  std::string bundle_uuid;
  std::vector<std::string> evidence;
};

ServerObservabilityState InitializeServerObservability(const ServerBootstrapConfig& config,
                                                       const ServerLifecycleArtifacts& artifacts,
                                                       const HostedEngineState& engine_state,
                                                       const ParserPackageRegistry& parser_registry,
                                                       const ServerListenerOrchestrator& listeners);
void SetServerMetric(ServerObservabilityState* state,
                     std::string path,
                     std::uint64_t value,
                     std::string type = "gauge",
                     std::map<std::string, std::string> labels = {});
void IncrementServerMetric(ServerObservabilityState* state,
                           std::string path,
                           std::uint64_t amount = 1,
                           std::map<std::string, std::string> labels = {});
std::string RecordServerAuditEvent(ServerObservabilityState* state,
                                   std::string event_type,
                                   std::string outcome,
                                   std::string safe_detail,
                                   std::string diagnostic_code = {});
void RecordServerLog(ServerObservabilityState* state,
                     ServerStructuredLogRecord record);
std::string RedactSupportabilityText(std::string text);
bool LifecycleOperationRequiresCacheInvalidation(std::string_view operation_key);
bool LifecycleDiagnosticRetryable(std::string_view diagnostic_code);
std::vector<std::string> CanonicalLifecycleObservabilityOperations();
ServerLifecycleObservabilityRecord RecordServerLifecycleObservability(
    ServerObservabilityState* state,
    ServerLifecycleObservabilityEvent event);
ServerSupportabilityFlushResult FlushServerObservability(ServerObservabilityState* state,
                                                         std::string reason);
ServerSupportabilityFlushResult RotateServerOperationalLog(ServerObservabilityState* state,
                                                           std::uint64_t max_active_log_bytes);
std::string ServerMetricsSnapshotJson(const ServerObservabilityState& state);
std::string ServerAuditSnapshotJson(const ServerObservabilityState& state);
ServerSupportBundleExportResult ExportServerSupportBundle(ServerObservabilityState& state,
                                                          const ServerBootstrapConfig& config,
                                                          const ServerLifecycleArtifacts& artifacts,
                                                          const HostedEngineState& engine_state,
                                                          const ServerSessionRegistry& sessions,
                                                          const ParserPackageRegistry& parser_registry,
                                                          const ServerListenerOrchestrator& listeners);

}  // namespace scratchbird::server
