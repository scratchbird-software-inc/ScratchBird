// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "streaming_cursor_manager.hpp"

#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace scratchbird::wire {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr std::string_view kTokenPrefix = "SBORH1";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefusalStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

DiagnosticRecord MakeDiagnostic(Status status,
                                std::string code,
                                std::string key,
                                std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.message_key = std::move(key);
  diagnostic.source_component = "wire.streaming_cursor_manager";
  diagnostic.remediation_hint = std::move(detail);
  if (!diagnostic.remediation_hint.empty()) {
    diagnostic.arguments.push_back(
        DiagnosticArgument{"detail", diagnostic.remediation_hint});
  }
  return diagnostic;
}

void AddBool(std::vector<std::string>* evidence,
             std::string key,
             bool value) {
  evidence->push_back(std::move(key) + "=" + (value ? "true" : "false"));
}

void AppendStateEvidence(std::vector<std::string>* evidence,
                         const StreamingCursorState& state) {
  evidence->push_back("ORH_STREAMING_CURSOR_MANAGER");
  evidence->push_back("cursor_id=" + state.cursor_id);
  evidence->push_back("result_contract_hash=" +
                      state.plan_result_contract_hash);
  evidence->push_back("catalog_epoch=" +
                      std::to_string(state.catalog_epoch));
  evidence->push_back("descriptor_epoch=" +
                      std::to_string(state.descriptor_epoch));
  evidence->push_back("transaction_snapshot_class=" +
                      state.transaction_snapshot_class);
  evidence->push_back("transaction_uuid=" + state.transaction_uuid);
  evidence->push_back("local_transaction_id=" +
                      std::to_string(state.local_transaction_id));
  evidence->push_back("snapshot_visible_through_local_transaction_id=" +
                      std::to_string(
                          state.snapshot_visible_through_local_transaction_id));
  evidence->push_back("security_epoch=" +
                      std::to_string(state.security_epoch));
  evidence->push_back("redaction_epoch=" +
                      std::to_string(state.redaction_epoch));
  evidence->push_back("route_kind=" + state.route_kind);
  evidence->push_back("frame_sequence=" +
                      std::to_string(state.frame_sequence));
  evidence->push_back("expiry_deadline_unix_millis=" +
                      std::to_string(state.expiry_deadline_unix_millis));
  evidence->push_back("client_frame_credit=" +
                      std::to_string(state.client_credit.frame_credit));
  evidence->push_back("client_row_credit=" +
                      std::to_string(state.client_credit.row_credit));
  evidence->push_back("client_byte_credit=" +
                      std::to_string(state.client_credit.byte_credit));
  AddBool(evidence, "client_backpressure_active",
          state.client_credit.backpressure_active);
  AddBool(evidence, "cursor_cancellation_requested",
          state.cancellation_requested);
  AddBool(evidence, "cursor_mga_visibility_or_finality_authority",
          state.mga_visibility_or_finality_authority);
  AddBool(evidence, "cursor_advisory_metadata_only",
          state.advisory_metadata_only);
  evidence->push_back("ceic_020_cursor_memory_lease_id=" +
                      state.memory_lease_id);
  evidence->push_back("ceic_020_cursor_memory_bytes=" +
                      std::to_string(state.cursor_memory_bytes));
  evidence->push_back("ceic_020_outstanding_frame_bytes=" +
                      std::to_string(state.outstanding_frame_bytes));
  evidence->push_back("ceic_020_outstanding_frame_count=" +
                      std::to_string(state.outstanding_frame_count));
  evidence->push_back(
      "ceic_020_memory_evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority");
}

StreamingCursorResult CursorOk(std::string code,
                               std::string detail,
                               StreamingCursorState state) {
  StreamingCursorResult result;
  result.status = OkStatus();
  result.state = std::move(state);
  AppendStateEvidence(&result.evidence, result.state);
  result.diagnostic =
      MakeDiagnostic(result.status, std::move(code),
                     "streaming_cursor_manager.ok", std::move(detail));
  return result;
}

StreamingCursorResult CursorRefuse(std::string code,
                                   std::string reason,
                                   StreamingCursorState state = {}) {
  StreamingCursorResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.state = std::move(state);
  result.refusal_reasons.push_back(reason);
  if (!result.state.cursor_id.empty()) {
    AppendStateEvidence(&result.evidence, result.state);
  } else {
    result.evidence.push_back("ORH_STREAMING_CURSOR_MANAGER");
  }
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  result.diagnostic =
      MakeDiagnostic(result.status, std::move(code),
                     "streaming_cursor_manager.refused", std::move(reason));
  return result;
}

bool RequiredStateFieldsPresent(const StreamingCursorState& state,
                                std::string* missing) {
  if (state.cursor_id.empty()) {
    *missing = "cursor_id_required";
  } else if (state.plan_result_contract_hash.empty()) {
    *missing = "result_contract_hash_required";
  } else if (state.catalog_epoch == 0) {
    *missing = "catalog_epoch_required";
  } else if (state.descriptor_epoch == 0) {
    *missing = "descriptor_epoch_required";
  } else if (state.transaction_snapshot_class.empty()) {
    *missing = "transaction_snapshot_class_required";
  } else if (state.transaction_uuid.empty()) {
    *missing = "transaction_uuid_required";
  } else if (state.local_transaction_id == 0) {
    *missing = "local_transaction_id_required";
  } else if (state.snapshot_visible_through_local_transaction_id == 0) {
    *missing = "snapshot_visible_through_local_transaction_id_required";
  } else if (state.security_epoch == 0) {
    *missing = "security_epoch_required";
  } else if (state.redaction_epoch == 0) {
    *missing = "redaction_epoch_required";
  } else if (state.route_kind.empty()) {
    *missing = "route_kind_required";
  } else if (state.expiry_deadline_unix_millis == 0) {
    *missing = "expiry_deadline_required";
  } else {
    return true;
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance RuntimeMemoryProvenance() {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  provenance.source_label = "wire.streaming_cursor_manager";
  return provenance;
}

memory::ResultCursorPlanMemoryEpochs MemoryEpochsFromState(
    const StreamingCursorState& state) {
  auto epochs = state.memory_epochs;
  if (epochs.catalog_epoch == 0) epochs.catalog_epoch = state.catalog_epoch;
  if (epochs.security_epoch == 0) epochs.security_epoch = state.security_epoch;
  if (epochs.redaction_epoch == 0) epochs.redaction_epoch = state.redaction_epoch;
  if (epochs.descriptor_epoch == 0) epochs.descriptor_epoch = state.descriptor_epoch;
  return epochs;
}

memory::ResultCursorPlanMemoryScope MemoryScopeFromState(
    const StreamingCursorState& state) {
  auto scope = state.memory_scope;
  if (scope.cursor_id.empty()) scope.cursor_id = state.cursor_id;
  if (scope.query_id.empty()) scope.query_id = state.cursor_id;
  if (scope.transaction_id.empty()) scope.transaction_id = state.transaction_uuid;
  return scope;
}

StreamingCursorResult CursorMemoryRefuse(
    const StreamingCursorState& state,
    const memory::ResultCursorPlanMemoryDecision& decision) {
  StreamingCursorResult result;
  result.status = decision.status;
  result.fail_closed = true;
  result.state = state;
  result.diagnostic = decision.diagnostic;
  result.refusal_reasons.push_back(
      decision.diagnostic.diagnostic_code.empty()
          ? "cursor_memory_governance_refused"
          : decision.diagnostic.diagnostic_code);
  result.evidence = decision.evidence;
  AppendStateEvidence(&result.evidence, state);
  return result;
}

StreamingCursorResult CompareBinding(const StreamingCursorState& state,
                                     const StreamingCursorBinding& expected,
                                     u64 now_unix_millis) {
  if (state.route_kind != expected.route_kind) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.ROUTE_MISMATCH",
                        "cursor_route_mismatch", state);
  }
  if (state.expiry_deadline_unix_millis !=
      expected.expiry_deadline_unix_millis) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.EXPIRY_MISMATCH",
                        "cursor_expiry_deadline_mismatch", state);
  }
  if (now_unix_millis > state.expiry_deadline_unix_millis) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.EXPIRED",
                        "cursor_expired", state);
  }
  if (state.cancellation_requested) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.CANCELLED",
                        "cursor_cancelled", state);
  }
  if (state.catalog_epoch != expected.catalog_epoch) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.CATALOG_EPOCH_MISMATCH",
                        "cursor_catalog_epoch_mismatch", state);
  }
  if (state.descriptor_epoch != expected.descriptor_epoch) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.DESCRIPTOR_EPOCH_MISMATCH",
                        "cursor_descriptor_epoch_mismatch", state);
  }
  if (state.security_epoch != expected.security_epoch) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.SECURITY_EPOCH_MISMATCH",
                        "cursor_security_epoch_mismatch", state);
  }
  if (state.redaction_epoch != expected.redaction_epoch) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.REDACTION_EPOCH_MISMATCH",
                        "cursor_redaction_epoch_mismatch", state);
  }
  if (state.plan_result_contract_hash != expected.plan_result_contract_hash) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.CONTRACT_MISMATCH",
                        "cursor_result_contract_hash_mismatch", state);
  }
  if (state.transaction_snapshot_class != expected.transaction_snapshot_class) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.SNAPSHOT_CLASS_MISMATCH",
                        "cursor_transaction_snapshot_class_mismatch", state);
  }
  if (state.transaction_uuid != expected.transaction_uuid) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.TRANSACTION_UUID_MISMATCH",
                        "cursor_transaction_uuid_mismatch", state);
  }
  if (state.local_transaction_id != expected.local_transaction_id) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.LOCAL_TRANSACTION_ID_MISMATCH",
                        "cursor_local_transaction_id_mismatch", state);
  }
  if (state.snapshot_visible_through_local_transaction_id !=
      expected.snapshot_visible_through_local_transaction_id) {
    return CursorRefuse(
        "SB_ORH_STREAMING_CURSOR.SNAPSHOT_VISIBLE_THROUGH_MISMATCH",
        "cursor_snapshot_visible_through_local_transaction_id_mismatch",
        state);
  }
  if (state.frame_sequence != expected.frame_sequence) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.SEQUENCE_MISMATCH",
                        "cursor_frame_sequence_mismatch", state);
  }
  if (state.client_credit.frame_credit == 0 ||
      state.client_credit.row_credit == 0 ||
      state.client_credit.byte_credit == 0) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.BACKPRESSURE",
                        "client_credit_unavailable", state);
  }
  return CursorOk("SB_ORH_STREAMING_CURSOR.OK",
                  "cursor_fetch_admitted", state);
}

void AppendField(std::string* out, std::string_view name, std::string value) {
  out->append(std::to_string(name.size()));
  out->push_back(':');
  out->append(name);
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::string BindingPayload(const StreamingCursorBinding& binding) {
  std::string payload;
  AppendField(&payload, "cursor_id", binding.cursor_id);
  AppendField(&payload, "result_contract_hash",
              binding.plan_result_contract_hash);
  AppendField(&payload, "catalog_epoch",
              std::to_string(binding.catalog_epoch));
  AppendField(&payload, "descriptor_epoch",
              std::to_string(binding.descriptor_epoch));
  AppendField(&payload, "transaction_snapshot_class",
              binding.transaction_snapshot_class);
  AppendField(&payload, "transaction_uuid", binding.transaction_uuid);
  AppendField(&payload, "local_transaction_id",
              std::to_string(binding.local_transaction_id));
  AppendField(&payload, "snapshot_visible_through_local_transaction_id",
              std::to_string(
                  binding.snapshot_visible_through_local_transaction_id));
  AppendField(&payload, "security_epoch",
              std::to_string(binding.security_epoch));
  AppendField(&payload, "redaction_epoch",
              std::to_string(binding.redaction_epoch));
  AppendField(&payload, "route_kind", binding.route_kind);
  AppendField(&payload, "frame_sequence",
              std::to_string(binding.frame_sequence));
  AppendField(&payload, "expiry_deadline_unix_millis",
              std::to_string(binding.expiry_deadline_unix_millis));
  return payload;
}

char HexDigit(std::uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('a' + (value - 10));
}

std::string HexEncode(std::string_view value) {
  std::string out;
  out.reserve(value.size() * 2u);
  for (const unsigned char ch : value) {
    out.push_back(HexDigit(static_cast<std::uint8_t>(ch >> 4u)));
    out.push_back(HexDigit(static_cast<std::uint8_t>(ch & 0x0fu)));
  }
  return out;
}

std::string SignatureFor(std::string_view payload,
                         const ContinuationTokenSecret& secret) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_len = 0;
  const std::string body = secret.key_id + ":" + std::string(payload);
  if (HMAC(EVP_sha256(),
           secret.secret_material.data(),
           static_cast<int>(secret.secret_material.size()),
           reinterpret_cast<const unsigned char*>(body.data()),
           body.size(),
           digest.data(),
           &digest_len) == nullptr ||
      digest_len == 0) {
    return {};
  }
  return HexEncode(std::string_view(
      reinterpret_cast<const char*>(digest.data()), digest_len));
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return 10 + (ch - 'a');
  }
  if (ch >= 'A' && ch <= 'F') {
    return 10 + (ch - 'A');
  }
  return -1;
}

bool HexDecode(std::string_view text, std::string* out) {
  if ((text.size() % 2u) != 0) {
    return false;
  }
  out->clear();
  out->reserve(text.size() / 2u);
  for (std::size_t i = 0; i < text.size(); i += 2u) {
    const int hi = HexValue(text[i]);
    const int lo = HexValue(text[i + 1u]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out->push_back(static_cast<char>((hi << 4) | lo));
  }
  return true;
}

bool ReadLength(std::string_view text, std::size_t* offset, std::size_t* length) {
  if (*offset >= text.size()) {
    return false;
  }
  std::size_t value = 0;
  bool found_digit = false;
  while (*offset < text.size() && text[*offset] != ':') {
    const char ch = text[*offset];
    if (ch < '0' || ch > '9') {
      return false;
    }
    const std::size_t digit = static_cast<std::size_t>(ch - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10u) {
      return false;
    }
    value = (value * 10u) + digit;
    found_digit = true;
    ++(*offset);
  }
  if (!found_digit || *offset >= text.size() || text[*offset] != ':') {
    return false;
  }
  ++(*offset);
  *length = value;
  return true;
}

bool ReadField(std::string_view text,
               std::size_t* offset,
               std::string* name,
               std::string* value) {
  std::size_t name_length = 0;
  if (!ReadLength(text, offset, &name_length) ||
      name_length > text.size() - *offset) {
    return false;
  }
  name->assign(text.substr(*offset, name_length));
  *offset += name_length;
  std::size_t value_length = 0;
  if (!ReadLength(text, offset, &value_length) ||
      value_length > text.size() - *offset) {
    return false;
  }
  value->assign(text.substr(*offset, value_length));
  *offset += value_length;
  return true;
}

bool ParseU64Text(const std::string& value, u64* out) {
  if (value.empty()) {
    return false;
  }
  u64 parsed = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const u64 digit = static_cast<u64>(ch - '0');
    if (parsed > (std::numeric_limits<u64>::max() - digit) / 10u) {
      return false;
    }
    parsed = (parsed * 10u) + digit;
  }
  *out = parsed;
  return true;
}

bool ParseBindingPayload(std::string_view payload,
                         StreamingCursorBinding* binding) {
  std::unordered_map<std::string, std::string> fields;
  std::size_t offset = 0;
  while (offset < payload.size()) {
    std::string name;
    std::string value;
    if (!ReadField(payload, &offset, &name, &value)) {
      return false;
    }
    fields.emplace(std::move(name), std::move(value));
  }
  constexpr std::array<std::string_view, 13> required{{
      "cursor_id",
      "result_contract_hash",
      "catalog_epoch",
      "descriptor_epoch",
      "transaction_snapshot_class",
      "transaction_uuid",
      "local_transaction_id",
      "snapshot_visible_through_local_transaction_id",
      "security_epoch",
      "redaction_epoch",
      "route_kind",
      "frame_sequence",
      "expiry_deadline_unix_millis",
  }};
  for (const auto name : required) {
    if (fields.find(std::string(name)) == fields.end()) {
      return false;
    }
  }
  binding->cursor_id = fields["cursor_id"];
  binding->plan_result_contract_hash = fields["result_contract_hash"];
  binding->transaction_snapshot_class =
      fields["transaction_snapshot_class"];
  binding->transaction_uuid = fields["transaction_uuid"];
  binding->route_kind = fields["route_kind"];
  return ParseU64Text(fields["catalog_epoch"], &binding->catalog_epoch) &&
         ParseU64Text(fields["descriptor_epoch"], &binding->descriptor_epoch) &&
         ParseU64Text(fields["local_transaction_id"],
                      &binding->local_transaction_id) &&
         ParseU64Text(fields["snapshot_visible_through_local_transaction_id"],
                      &binding->snapshot_visible_through_local_transaction_id) &&
         ParseU64Text(fields["security_epoch"], &binding->security_epoch) &&
         ParseU64Text(fields["redaction_epoch"], &binding->redaction_epoch) &&
         ParseU64Text(fields["frame_sequence"], &binding->frame_sequence) &&
         ParseU64Text(fields["expiry_deadline_unix_millis"],
                      &binding->expiry_deadline_unix_millis);
}

ContinuationTokenResult TokenRefuse(std::string code, std::string reason) {
  ContinuationTokenResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.refusal_reasons.push_back(reason);
  result.evidence.push_back("ORH_SECURE_CONTINUATION_TOKEN");
  result.evidence.push_back("continuation_token_valid=false");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  result.diagnostic =
      MakeDiagnostic(result.status, std::move(code),
                     "secure_continuation_token.refused",
                     std::move(reason));
  return result;
}

ContinuationTokenResult TokenOk(std::string code,
                                std::string detail,
                                StreamingCursorBinding binding,
                                std::string token = {}) {
  ContinuationTokenResult result;
  result.status = OkStatus();
  result.binding = std::move(binding);
  result.token = std::move(token);
  result.evidence.push_back("ORH_SECURE_CONTINUATION_TOKEN");
  result.evidence.push_back("continuation_token_valid=true");
  result.evidence.push_back("continuation_token_signature_algorithm=HMAC-SHA256");
  result.evidence.push_back("continuation_token_key_id_encoding=hex");
  result.evidence.push_back("continuation_token_bound_cursor_id=" +
                            result.binding.cursor_id);
  result.evidence.push_back("continuation_token_bound_route_kind=" +
                            result.binding.route_kind);
  result.evidence.push_back("continuation_token_bound_contract_hash=" +
                            result.binding.plan_result_contract_hash);
  result.evidence.push_back("continuation_token_bound_catalog_epoch=" +
                            std::to_string(result.binding.catalog_epoch));
  result.evidence.push_back("continuation_token_bound_descriptor_epoch=" +
                            std::to_string(result.binding.descriptor_epoch));
  result.evidence.push_back("continuation_token_bound_snapshot_class=" +
                            result.binding.transaction_snapshot_class);
  result.evidence.push_back("continuation_token_bound_transaction_uuid=" +
                            result.binding.transaction_uuid);
  result.evidence.push_back("continuation_token_bound_local_transaction_id=" +
                            std::to_string(result.binding.local_transaction_id));
  result.evidence.push_back(
      "continuation_token_bound_snapshot_visible_through_local_transaction_id=" +
      std::to_string(
          result.binding.snapshot_visible_through_local_transaction_id));
  result.evidence.push_back("continuation_token_bound_security_epoch=" +
                            std::to_string(result.binding.security_epoch));
  result.evidence.push_back("continuation_token_bound_redaction_epoch=" +
                            std::to_string(result.binding.redaction_epoch));
  result.evidence.push_back("continuation_token_bound_frame_sequence=" +
                            std::to_string(result.binding.frame_sequence));
  result.evidence.push_back("continuation_token_bound_expiry_deadline_unix_millis=" +
                            std::to_string(
                                result.binding.expiry_deadline_unix_millis));
  result.diagnostic =
      MakeDiagnostic(result.status, std::move(code),
                     "secure_continuation_token.ok", std::move(detail));
  return result;
}

ContinuationTokenResult CompareTokenBinding(
    const StreamingCursorBinding& parsed,
    const StreamingCursorBinding& expected,
    u64 now_unix_millis) {
  if (parsed.cursor_id != expected.cursor_id) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.CURSOR_MISMATCH",
                       "token_cursor_id_mismatch");
  }
  if (parsed.route_kind != expected.route_kind) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.ROUTE_MISMATCH",
                       "token_route_kind_mismatch");
  }
  if (parsed.expiry_deadline_unix_millis !=
      expected.expiry_deadline_unix_millis) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.EXPIRY_MISMATCH",
                       "token_expiry_deadline_mismatch");
  }
  if (now_unix_millis > parsed.expiry_deadline_unix_millis) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.EXPIRED",
                       "token_expired");
  }
  if (parsed.catalog_epoch != expected.catalog_epoch) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.CATALOG_EPOCH_MISMATCH",
                       "token_catalog_epoch_mismatch");
  }
  if (parsed.descriptor_epoch != expected.descriptor_epoch) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.DESCRIPTOR_EPOCH_MISMATCH",
                       "token_descriptor_epoch_mismatch");
  }
  if (parsed.security_epoch != expected.security_epoch) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.SECURITY_EPOCH_MISMATCH",
                       "token_security_epoch_mismatch");
  }
  if (parsed.redaction_epoch != expected.redaction_epoch) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.REDACTION_EPOCH_MISMATCH",
                       "token_redaction_epoch_mismatch");
  }
  if (parsed.plan_result_contract_hash !=
      expected.plan_result_contract_hash) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.CONTRACT_MISMATCH",
                       "token_result_contract_hash_mismatch");
  }
  if (parsed.transaction_snapshot_class !=
      expected.transaction_snapshot_class) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.SNAPSHOT_CLASS_MISMATCH",
                       "token_transaction_snapshot_class_mismatch");
  }
  if (parsed.transaction_uuid != expected.transaction_uuid) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.TRANSACTION_UUID_MISMATCH",
                       "token_transaction_uuid_mismatch");
  }
  if (parsed.local_transaction_id != expected.local_transaction_id) {
    return TokenRefuse(
        "SB_ORH_CONTINUATION_TOKEN.LOCAL_TRANSACTION_ID_MISMATCH",
        "token_local_transaction_id_mismatch");
  }
  if (parsed.snapshot_visible_through_local_transaction_id !=
      expected.snapshot_visible_through_local_transaction_id) {
    return TokenRefuse(
        "SB_ORH_CONTINUATION_TOKEN.SNAPSHOT_VISIBLE_THROUGH_MISMATCH",
        "token_snapshot_visible_through_local_transaction_id_mismatch");
  }
  if (parsed.frame_sequence != expected.frame_sequence) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.SEQUENCE_MISMATCH",
                       "token_frame_sequence_mismatch");
  }
  return TokenOk("SB_ORH_CONTINUATION_TOKEN.OK",
                 "continuation_token_admitted", parsed);
}

}  // namespace

StreamingCursorBinding StreamingCursorBindingFromState(
    const StreamingCursorState& state) {
  StreamingCursorBinding binding;
  binding.cursor_id = state.cursor_id;
  binding.plan_result_contract_hash = state.plan_result_contract_hash;
  binding.catalog_epoch = state.catalog_epoch;
  binding.descriptor_epoch = state.descriptor_epoch;
  binding.transaction_snapshot_class = state.transaction_snapshot_class;
  binding.transaction_uuid = state.transaction_uuid;
  binding.local_transaction_id = state.local_transaction_id;
  binding.snapshot_visible_through_local_transaction_id =
      state.snapshot_visible_through_local_transaction_id;
  binding.security_epoch = state.security_epoch;
  binding.redaction_epoch = state.redaction_epoch;
  binding.route_kind = state.route_kind;
  binding.frame_sequence = state.frame_sequence;
  binding.expiry_deadline_unix_millis = state.expiry_deadline_unix_millis;
  return binding;
}

StreamingCursorResult StreamingCursorManager::OpenCursor(
    const StreamingCursorOpenRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string missing;
  if (!RequiredStateFieldsPresent(request.state, &missing)) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.REQUIRED_FIELD_MISSING",
                        missing, request.state);
  }
  if (request.now_unix_millis > request.state.expiry_deadline_unix_millis) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.EXPIRED",
                        "cursor_expired_on_open", request.state);
  }
  if (request.state.mga_visibility_or_finality_authority ||
      !request.state.advisory_metadata_only) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.MGA_AUTHORITY_VIOLATION",
                        "cursor_state_must_not_own_visibility_or_finality",
                        request.state);
  }
  auto state = request.state;
  state.client_credit.backpressure_active =
      state.client_credit.frame_credit == 0 || state.client_credit.row_credit == 0 ||
      state.client_credit.byte_credit == 0;
  if (cursors_.find(state.cursor_id) != cursors_.end()) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.DUPLICATE_CURSOR_ID",
                        "duplicate_cursor_id", state);
  }
  if (request.require_memory_governance || state.memory_governor != nullptr ||
      state.memory_ledger != nullptr) {
    if (state.memory_governor == nullptr || state.memory_ledger == nullptr) {
      return CursorRefuse("SB_CEIC_020_STREAMING_CURSOR.MEMORY_GOVERNANCE_REQUIRED",
                          "cursor_memory_governor_and_ledger_required", state);
    }
    memory::ResultCursorPlanMemoryLeaseRequest lease;
    lease.surface = memory::ResultCursorPlanMemorySurface::cursor;
    lease.ledger = state.memory_ledger;
    lease.policy = state.memory_policy;
    lease.scope = MemoryScopeFromState(state);
    lease.epochs = MemoryEpochsFromState(state);
    lease.provenance = RuntimeMemoryProvenance();
    lease.memory_class = "ceic_020.streaming_cursor";
    lease.owner_id = "wire.cursor:" + state.cursor_id;
    lease.route_label = state.route_kind;
    lease.requested_bytes =
        request.cursor_memory_bytes != 0 ? request.cursor_memory_bytes
                                         : state.cursor_memory_bytes;
    lease.lease_expires_at_ms = state.expiry_deadline_unix_millis;
    lease.cluster_route_requested = request.cluster_route_requested;
    auto acquired = state.memory_governor->Acquire(std::move(lease));
    if (!acquired.ok()) {
      return CursorMemoryRefuse(state, acquired);
    }
    state.memory_lease_id = acquired.lease_id;
    state.cursor_memory_bytes =
        request.cursor_memory_bytes != 0 ? request.cursor_memory_bytes
                                         : state.cursor_memory_bytes;
    if (state.cursor_memory_bytes == 0) {
      state.cursor_memory_bytes = 1;
    }
  }
  cursors_.emplace(state.cursor_id, state);
  return CursorOk("SB_ORH_STREAMING_CURSOR.OPENED",
                  "cursor_opened", std::move(state));
}

StreamingCursorResult StreamingCursorManager::GrantCredit(
    const std::string& cursor_id,
    StreamingCursorCreditState credit) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = cursors_.find(cursor_id);
  if (found == cursors_.end()) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.NOT_FOUND",
                        "cursor_not_found");
  }
  credit.backpressure_active =
      credit.frame_credit == 0 || credit.row_credit == 0 ||
      credit.byte_credit == 0;
  if (found->second.memory_governor != nullptr) {
    auto released = found->second.memory_governor->ReleaseResultFramesByCursor(
        cursor_id, memory::ResultCursorPlanMemoryReleaseReason::explicit_release);
    found->second.outstanding_frame_bytes =
        found->second.outstanding_frame_bytes >= released.released_bytes
            ? found->second.outstanding_frame_bytes - released.released_bytes
            : 0;
    found->second.outstanding_frame_count =
        found->second.outstanding_frame_count >= released.released_lease_count
            ? found->second.outstanding_frame_count - released.released_lease_count
            : 0;
  }
  found->second.client_credit = credit;
  return CursorOk("SB_ORH_STREAMING_CURSOR.CREDIT_UPDATED",
                  "cursor_credit_updated", found->second);
}

StreamingCursorResult StreamingCursorManager::CancelCursor(
    const std::string& cursor_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = cursors_.find(cursor_id);
  if (found == cursors_.end()) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.NOT_FOUND",
                        "cursor_not_found");
  }
  found->second.cancellation_requested = true;
  if (found->second.memory_governor != nullptr) {
    auto released = found->second.memory_governor->ReleaseByCursor(
        cursor_id, memory::ResultCursorPlanMemoryReleaseReason::cancel);
    found->second.outstanding_frame_bytes = 0;
    found->second.outstanding_frame_count = 0;
    found->second.cursor_memory_bytes =
        found->second.cursor_memory_bytes >= released.released_bytes
            ? found->second.cursor_memory_bytes - released.released_bytes
            : 0;
    found->second.memory_lease_id.clear();
  }
  return CursorOk("SB_ORH_STREAMING_CURSOR.CANCEL_REQUESTED",
                  "cursor_cancel_requested", found->second);
}

StreamingCursorResult StreamingCursorManager::ValidateFetch(
    const StreamingCursorFetchRequest& request) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = cursors_.find(request.expected.cursor_id);
  if (found == cursors_.end()) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.NOT_FOUND",
                        "cursor_not_found");
  }
  return CompareBinding(found->second, request.expected,
                        request.now_unix_millis);
}

StreamingCursorResult StreamingCursorManager::RecordFrameDelivery(
    const StreamingCursorFrameDelivery& delivery) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = cursors_.find(delivery.expected.cursor_id);
  if (found == cursors_.end()) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.NOT_FOUND",
                        "cursor_not_found");
  }
  auto checked = CompareBinding(found->second, delivery.expected,
                                delivery.now_unix_millis);
  if (!checked.ok()) {
    return checked;
  }
  auto& state = found->second;
  if (delivery.row_count > state.client_credit.row_credit ||
      delivery.byte_count > state.client_credit.byte_credit) {
    return CursorRefuse("SB_ORH_STREAMING_CURSOR.BACKPRESSURE",
                        "client_credit_insufficient_for_frame", state);
  }
  if (delivery.require_memory_governance || state.memory_governor != nullptr ||
      state.memory_ledger != nullptr) {
    if (state.memory_governor == nullptr || state.memory_ledger == nullptr) {
      return CursorRefuse("SB_CEIC_020_STREAMING_CURSOR.FRAME_MEMORY_GOVERNANCE_REQUIRED",
                          "result_frame_memory_governor_and_ledger_required",
                          state);
    }
    memory::ResultCursorPlanMemoryLeaseRequest frame_lease;
    frame_lease.surface = memory::ResultCursorPlanMemorySurface::result_frame;
    frame_lease.ledger = state.memory_ledger;
    frame_lease.policy = state.memory_policy;
    frame_lease.scope = MemoryScopeFromState(state);
    frame_lease.epochs = MemoryEpochsFromState(state);
    frame_lease.provenance = RuntimeMemoryProvenance();
    frame_lease.memory_class = "ceic_020.result_frame";
    frame_lease.owner_id = "wire.cursor.frame:" + state.cursor_id;
    frame_lease.route_label = state.route_kind;
    frame_lease.requested_bytes = delivery.byte_count;
    frame_lease.lease_expires_at_ms = state.expiry_deadline_unix_millis;
    auto acquired = state.memory_governor->Acquire(std::move(frame_lease));
    if (!acquired.ok()) {
      auto refused = CursorMemoryRefuse(state, acquired);
      refused.refusal_reasons.push_back("result_frame_memory_backpressure");
      return refused;
    }
    state.outstanding_frame_bytes += delivery.byte_count;
    ++state.outstanding_frame_count;
  }
  --state.client_credit.frame_credit;
  state.client_credit.row_credit -= delivery.row_count;
  state.client_credit.byte_credit -= delivery.byte_count;
  ++state.frame_sequence;
  state.client_credit.backpressure_active =
      state.client_credit.frame_credit == 0 || state.client_credit.row_credit == 0 ||
      state.client_credit.byte_credit == 0;
  return CursorOk("SB_ORH_STREAMING_CURSOR.FRAME_DELIVERED",
                  "cursor_frame_delivered", state);
}

std::optional<StreamingCursorState> StreamingCursorManager::Lookup(
    const std::string& cursor_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = cursors_.find(cursor_id);
  if (found == cursors_.end()) {
    return std::nullopt;
  }
  return found->second;
}

ContinuationTokenResult IssueContinuationToken(
    const StreamingCursorBinding& binding,
    const ContinuationTokenSecret& secret) {
  if (secret.key_id.empty() || secret.secret_material.empty()) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.SECRET_REQUIRED",
                       "token_secret_required");
  }
  const auto payload = BindingPayload(binding);
  const auto signature = SignatureFor(payload, secret);
  if (signature.empty()) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.SIGNATURE_UNAVAILABLE",
                       "token_signature_unavailable");
  }
  const auto token = std::string(kTokenPrefix) + "." + HexEncode(payload) +
                     "." + HexEncode(secret.key_id) + "." + signature;
  return TokenOk("SB_ORH_CONTINUATION_TOKEN.ISSUED",
                 "continuation_token_issued", binding, token);
}

ContinuationTokenResult ValidateContinuationToken(
    const std::string& token,
    const StreamingCursorBinding& expected,
    const ContinuationTokenSecret& secret,
    u64 now_unix_millis) {
  if (secret.key_id.empty() || secret.secret_material.empty()) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.SECRET_REQUIRED",
                       "token_secret_required");
  }
  const auto first = token.find('.');
  const auto second = first == std::string::npos ? std::string::npos
                                                  : token.find('.', first + 1u);
  const auto third = second == std::string::npos ? std::string::npos
                                                 : token.find('.', second + 1u);
  if (first == std::string::npos || second == std::string::npos ||
      third == std::string::npos || token.find('.', third + 1u) != std::string::npos ||
      token.substr(0, first) != kTokenPrefix) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.MALFORMED",
                       "malformed_token");
  }
  const auto payload_hex = std::string_view(token).substr(first + 1u,
                                                          second - first - 1u);
  const auto key_id_hex = std::string_view(token).substr(
      second + 1u, third - second - 1u);
  const auto signature = token.substr(third + 1u);
  std::string key_id;
  if (!HexDecode(key_id_hex, &key_id)) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.MALFORMED",
                       "malformed_token_key_id");
  }
  if (key_id != secret.key_id) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.KEY_MISMATCH",
                       "token_key_id_mismatch");
  }
  std::string payload;
  if (!HexDecode(payload_hex, &payload)) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.MALFORMED",
                       "malformed_token_payload");
  }
  const auto expected_signature = SignatureFor(payload, secret);
  if (expected_signature.empty()) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.SIGNATURE_UNAVAILABLE",
                       "token_signature_unavailable");
  }
  if (expected_signature.size() != signature.size() ||
      CRYPTO_memcmp(expected_signature.data(),
                    signature.data(),
                    expected_signature.size()) != 0) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.TAMPERED",
                       "token_signature_mismatch");
  }
  StreamingCursorBinding parsed;
  if (!ParseBindingPayload(payload, &parsed)) {
    return TokenRefuse("SB_ORH_CONTINUATION_TOKEN.MALFORMED",
                       "malformed_token_payload");
  }
  return CompareTokenBinding(parsed, expected, now_unix_millis);
}

}  // namespace scratchbird::wire
