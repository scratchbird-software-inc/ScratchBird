// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../../core/platform/runtime_platform.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scratchbird::engine::sblr {

using SblrUuid = scratchbird::core::platform::Uuid;
static_assert(sizeof(SblrUuid) == 16);

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
  uuid_binary,
  temporal_text,
  descriptor_payload,
  uuid_array_binary,
};

struct SblrDiagnosticField {
  std::string key;
  // Private canonical runtime metadata. UUID parameters retain their binary
  // type; presentation adapters may not flatten them into public text fields.
  std::variant<std::string, SblrUuid> value;
};

struct SblrRuntimeDiagnostic {
  // Issued once by MakeSblrDiagnostic. Default nil means no emitted record;
  // copies and trusted bridges retain the source occurrence unchanged.
  SblrUuid occurrence_uuid;
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
  // Native UUID value bytes. UUID payloads never use text_value, encoded_value
  // or binary_value as a second identity authority. Descriptor binding still
  // controls user UUID version/render policy and system UUIDv7 admission.
  SblrUuid uuid_value;
  // A UUID collection retains one native value per element; no text shadow.
  std::vector<SblrUuid> uuid_array_value;
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

SblrValue MakeSblrUuidValue(const SblrUuid& uuid);
// Payload transfer only, not descriptor/catalog/identity-policy admission.
// Conflicting legacy representations fail without changing the destination.
bool CopySblrUuidPayload(const SblrValue& value,
                         std::vector<std::uint8_t>* destination);

// Private projection payload transfer: consecutive binary16 elements. The
// enclosing payload length supplies the element count; this is not a persisted
// ARRAY datatype encoding or descriptor/catalog admission.
bool SblrUuidArrayPayloadValid(const SblrValue& value) noexcept;
bool CopySblrUuidArrayPayload(const SblrValue& value,
                              std::vector<std::uint8_t>* destination);

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
  SblrUuid owner_session_uuid;
  std::uint64_t acquisition_count = 0;
  std::uint64_t owner_process_id = 0;
};

struct SblrTransactionAdvisoryLockEntry {
  std::int64_t key = 0;
  SblrUuid owner_transaction_uuid;
  std::uint64_t acquisition_count = 0;
  std::uint64_t owner_local_transaction_id = 0;
};

enum class SblrAdvisoryLockOwnerKind : std::uint8_t {
  session, process, transaction, local_transaction
};
struct SblrAdvisoryLockOwner {
  SblrAdvisoryLockOwnerKind kind = SblrAdvisoryLockOwnerKind::session;
  SblrUuid uuid;
  std::uint64_t numeric_reference = 0;
};
struct SblrAdvisoryLockEvidence {
  std::string function_name;
  std::string action;
  std::int64_t key = 0;
  SblrAdvisoryLockOwner owner;
  std::uint64_t remaining_acquisitions = 0;
  std::optional<std::int64_t> timeout_seconds;
};

struct SblrSessionRuntimeState {
  std::vector<SblrSessionConfigEntry> config_entries;
  std::vector<std::uint64_t> cancellable_backend_pids;
  std::vector<std::uint64_t> terminable_backend_pids;
  std::vector<std::uint64_t> cancel_requested_backend_pids;
  std::vector<std::uint64_t> terminate_requested_backend_pids;
  std::vector<std::string> backend_control_evidence;
  std::vector<SblrSessionAdvisoryLockEntry> advisory_lock_entries;
  std::vector<SblrAdvisoryLockEvidence> advisory_lock_evidence;
  std::vector<SblrTransactionAdvisoryLockEntry> transaction_advisory_lock_entries;
  std::vector<SblrAdvisoryLockEvidence> transaction_advisory_lock_evidence;
};

struct SblrExecutionContext {
  std::string database_path;
  SblrUuid cluster_uuid;
  SblrUuid node_uuid;
  SblrUuid database_uuid;
  SblrUuid transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_isolation_level = "read_committed";
  SblrUuid statement_uuid;
  SblrUuid user_uuid;
  SblrUuid current_role_uuid;
  std::vector<SblrUuid> current_group_uuid_set;
  SblrUuid current_schema_uuid;
  SblrUuid session_uuid;
  SblrUuid attachment_uuid;
  std::string statement_timestamp;
  std::string transaction_timestamp;
  std::string current_timestamp;
  std::string current_monotonic_ns;
  std::uint64_t deterministic_random_u64 = 0;
  std::string deterministic_random_bytes_hex;
  std::optional<scratchbird::core::platform::Uuid> deterministic_uuid;
  std::string current_sqlstate;
  std::string current_diagnostic_id;
  SblrUuid current_diagnostic_uuid;
  std::string last_identity_value;
  SblrUuid parser_profile_uuid;
  SblrUuid client_protocol_uuid;
  std::string application_name;
  SblrUuid security_snapshot_uuid;
  std::vector<std::string> active_savepoint_names;
  SblrRuntimeDiagnostic savepoint_authority_diagnostic;
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
  SblrUuid frame_uuid;
  SblrUuid routine_object_uuid;
  SblrUuid package_object_uuid;
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
