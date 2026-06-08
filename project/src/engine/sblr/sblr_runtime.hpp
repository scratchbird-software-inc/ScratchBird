// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

enum class SblrStatusCode {
  ok,
  invalid_envelope,
  unsupported_feature,
  security_refused,
  policy_refused,
  dependency_unavailable,
  execution_failed,
  resource_exhausted,
  internal_error,
};

enum class SblrDiagnosticSeverity {
  info,
  warning,
  error,
};

enum class SblrValuePayloadKind {
  none,
  boolean,
  signed_integer,
  unsigned_integer,
  real64,
  high_precision_numeric_text,
  text,
  binary,
  uuid_text,
  temporal_text,
  descriptor_payload,
};

struct SblrDiagnosticField {
  std::string key;
  std::string value;
};

struct SblrRuntimeDiagnostic {
  std::string diagnostic_id;
  std::string message_key;
  std::string detail;
  SblrDiagnosticSeverity severity = SblrDiagnosticSeverity::error;
  std::vector<SblrDiagnosticField> fields;
};

struct SblrValue {
  std::string descriptor_id;
  std::string text_value;
  std::string encoded_value;
  std::string charset_name;
  std::string collation_name;
  std::vector<std::uint8_t> binary_value;
  SblrValuePayloadKind payload_kind = SblrValuePayloadKind::none;
  std::int64_t int64_value = 0;
  std::uint64_t uint64_value = 0;
  double real64_value = 0.0;
  bool is_null = true;
  bool has_int64_value = false;
  bool has_uint64_value = false;
  bool has_real64_value = false;
};

struct SblrResultRow {
  std::vector<SblrValue> values;
};

struct SblrResult {
  SblrStatusCode status = SblrStatusCode::ok;
  std::string operation_id;
  std::vector<SblrRuntimeDiagnostic> diagnostics;
  std::vector<SblrValue> scalar_values;
  std::vector<SblrResultRow> rows;
  bool mutation_attempted = false;
  bool mutation_committed = false;

  [[nodiscard]] bool ok() const { return status == SblrStatusCode::ok && diagnostics.empty(); }
};

struct SblrSessionConfigEntry {
  std::string name;
  std::string value;
  bool is_local = true;
};

struct SblrSessionAdvisoryLockEntry {
  std::int64_t key = 0;
  std::string owner_session_uuid;
  std::uint64_t acquisition_count = 0;
};

struct SblrTransactionAdvisoryLockEntry {
  std::int64_t key = 0;
  std::string owner_transaction_token;
  std::uint64_t acquisition_count = 0;
};

struct SblrSessionRuntimeState {
  std::vector<SblrSessionConfigEntry> config_entries;
  std::vector<std::uint64_t> cancellable_backend_pids;
  std::vector<std::uint64_t> terminable_backend_pids;
  std::vector<std::uint64_t> cancel_requested_backend_pids;
  std::vector<std::uint64_t> terminate_requested_backend_pids;
  std::vector<std::string> backend_control_evidence;
  std::vector<SblrSessionAdvisoryLockEntry> advisory_lock_entries;
  std::vector<std::string> advisory_lock_evidence;
  std::vector<SblrTransactionAdvisoryLockEntry> transaction_advisory_lock_entries;
  std::vector<std::string> transaction_advisory_lock_evidence;
};

struct SblrExecutionContext {
  std::string database_path;
  std::string cluster_uuid;
  std::string node_uuid;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_isolation_level = "read_committed";
  std::string statement_uuid;
  std::string user_uuid;
  std::string current_role_uuid;
  std::string current_group_uuid_set;
  std::string current_schema_uuid;
  std::string session_uuid;
  std::string attachment_uuid;
  std::string statement_timestamp;
  std::string transaction_timestamp;
  std::string current_timestamp;
  std::string current_monotonic_ns;
  std::uint64_t deterministic_random_u64 = 0;
  std::string deterministic_random_bytes_hex;
  std::string deterministic_uuid_text;
  std::string current_sqlstate;
  std::string current_diagnostic_id;
  std::string current_diagnostic_uuid;
  std::string last_identity_value;
  std::string parser_profile_uuid;
  std::string client_protocol_uuid;
  std::string application_name;
  std::string security_snapshot_uuid;
  std::vector<std::string> active_savepoint_names;
  std::shared_ptr<SblrSessionRuntimeState> session_runtime_state =
      std::make_shared<SblrSessionRuntimeState>();
  std::uint64_t backend_process_id = 0;
  std::uint64_t last_row_count = 0;
  bool restricted_open_mode = false;
  bool read_only_mode = false;
  bool deterministic_random_u64_present = false;
  bool last_row_count_present = false;
  bool last_identity_value_present = false;
  bool security_context_present = false;
  bool transaction_context_present = false;
  bool cluster_authority_available = false;
};

struct SblrFrame {
  std::string frame_uuid;
  std::string routine_object_uuid;
  std::string package_object_uuid;
  std::size_t depth = 0;
  bool rollback_region_open = false;
};

struct SblrFrameStack {
  std::vector<SblrFrame> frames;
  std::size_t max_depth = 1024;
};

SblrRuntimeDiagnostic MakeSblrDiagnostic(std::string diagnostic_id,
                                         std::string message_key,
                                         std::string detail = {},
                                         SblrDiagnosticSeverity severity = SblrDiagnosticSeverity::error);
SblrRuntimeDiagnostic MakeSblrRefusalDiagnostic(std::string diagnostic_id,
                                                const SblrExecutionContext& context,
                                                std::string detail = {});
SblrResult MakeSblrSuccess(std::string operation_id = {});
SblrResult MakeSblrFailure(SblrStatusCode status,
                           std::string operation_id,
                           SblrRuntimeDiagnostic diagnostic);
bool ValidateDiagnosticCompleteness(const SblrRuntimeDiagnostic& diagnostic,
                                    std::vector<std::string>* missing_fields);
bool PushSblrFrame(SblrFrameStack* stack, SblrFrame frame, SblrResult* failure);
bool PopSblrFrame(SblrFrameStack* stack, SblrResult* failure);
std::string ToString(SblrStatusCode status);
std::string ToString(SblrDiagnosticSeverity severity);

}  // namespace scratchbird::engine::sblr
