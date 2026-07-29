// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cctype>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::engine::internal_api::EngineTypedValue;
using scratchbird::engine::internal_api::EngineValueState;

constexpr std::string_view kRefusalCode =
    "QOW-DIAG-QRY-021-REFUSAL-V1";

DescriptorRuntimeDiagnostic Refusal(std::string detail,
                                    std::size_t row = 0,
                                    std::size_t column = 0) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::string(kRefusalCode);
  diagnostic.detail = std::move(detail);
  diagnostic.row_index = row;
  diagnostic.column_index = column;
  return diagnostic;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

bool IsValidResultKind(const CanonicalResultKind kind) {
  switch (kind) {
    case CanonicalResultKind::kRows:
    case CanonicalResultKind::kCommand:
    case CanonicalResultKind::kEmpty:
    case CanonicalResultKind::kCursor:
    case CanonicalResultKind::kExplain: return true;
  }
  return false;
}

bool IsValidInvocationMode(const CanonicalResultInvocationMode mode) {
  switch (mode) {
    case CanonicalResultInvocationMode::kDirect:
    case CanonicalResultInvocationMode::kPrepared: return true;
  }
  return false;
}

bool IsValidNullability(const CanonicalResultNullability nullability) {
  switch (nullability) {
    case CanonicalResultNullability::kNonNull:
    case CanonicalResultNullability::kNullable:
    case CanonicalResultNullability::kUnknown: return true;
  }
  return false;
}

bool IsValidCursorState(const CanonicalResultCursorState state) {
  switch (state) {
    case CanonicalResultCursorState::kClosed:
    case CanonicalResultCursorState::kOpen:
    case CanonicalResultCursorState::kSuspended: return true;
  }
  return false;
}

bool IsValidSeverity(const CanonicalResultDiagnosticSeverity severity) {
  switch (severity) {
    case CanonicalResultDiagnosticSeverity::kInfo:
    case CanonicalResultDiagnosticSeverity::kWarning:
    case CanonicalResultDiagnosticSeverity::kError:
    case CanonicalResultDiagnosticSeverity::kFatal: return true;
  }
  return false;
}

bool IsValidPhase(const CanonicalResultDiagnosticPhase phase) {
  switch (phase) {
    case CanonicalResultDiagnosticPhase::kParse:
    case CanonicalResultDiagnosticPhase::kBind:
    case CanonicalResultDiagnosticPhase::kVerify:
    case CanonicalResultDiagnosticPhase::kPlan:
    case CanonicalResultDiagnosticPhase::kExecute:
    case CanonicalResultDiagnosticPhase::kFinalize: return true;
  }
  return false;
}

bool IsValidTransactionEffect(const CanonicalResultTransactionEffect effect) {
  switch (effect) {
    case CanonicalResultTransactionEffect::kUnchanged:
    case CanonicalResultTransactionEffect::kStatementFailedTransactionUsable:
    case CanonicalResultTransactionEffect::kEngineMarkedTransactionFailed:
      return true;
  }
  return false;
}

bool IsValidRetryability(const CanonicalResultRetryability retryability) {
  switch (retryability) {
    case CanonicalResultRetryability::kNotRetryable:
    case CanonicalResultRetryability::kRetrySameSnapshot:
    case CanonicalResultRetryability::kRetryNewSnapshot: return true;
  }
  return false;
}

std::string ResultKindName(const CanonicalResultKind kind) {
  switch (kind) {
    case CanonicalResultKind::kRows: return "rows";
    case CanonicalResultKind::kCommand: return "command";
    case CanonicalResultKind::kEmpty: return "empty";
    case CanonicalResultKind::kCursor: return "cursor";
    case CanonicalResultKind::kExplain: return "explain";
  }
  return "invalid";
}

std::string NullabilityName(const CanonicalResultNullability nullability) {
  switch (nullability) {
    case CanonicalResultNullability::kNonNull: return "non_null";
    case CanonicalResultNullability::kNullable: return "nullable";
    case CanonicalResultNullability::kUnknown: return "unknown";
  }
  return "invalid";
}

std::string CursorStateName(const CanonicalResultCursorState state) {
  switch (state) {
    case CanonicalResultCursorState::kClosed: return "closed";
    case CanonicalResultCursorState::kOpen: return "open";
    case CanonicalResultCursorState::kSuspended: return "suspended";
  }
  return "invalid";
}

std::string SeverityName(const CanonicalResultDiagnosticSeverity severity) {
  switch (severity) {
    case CanonicalResultDiagnosticSeverity::kInfo: return "info";
    case CanonicalResultDiagnosticSeverity::kWarning: return "warning";
    case CanonicalResultDiagnosticSeverity::kError: return "error";
    case CanonicalResultDiagnosticSeverity::kFatal: return "fatal";
  }
  return "invalid";
}

std::string PhaseName(const CanonicalResultDiagnosticPhase phase) {
  switch (phase) {
    case CanonicalResultDiagnosticPhase::kParse: return "parse";
    case CanonicalResultDiagnosticPhase::kBind: return "bind";
    case CanonicalResultDiagnosticPhase::kVerify: return "verify";
    case CanonicalResultDiagnosticPhase::kPlan: return "plan";
    case CanonicalResultDiagnosticPhase::kExecute: return "execute";
    case CanonicalResultDiagnosticPhase::kFinalize: return "finalize";
  }
  return "invalid";
}

std::string TransactionEffectName(
    const CanonicalResultTransactionEffect effect) {
  switch (effect) {
    case CanonicalResultTransactionEffect::kUnchanged: return "unchanged";
    case CanonicalResultTransactionEffect::kStatementFailedTransactionUsable:
      return "statement_failed_transaction_usable";
    case CanonicalResultTransactionEffect::kEngineMarkedTransactionFailed:
      return "engine_marked_transaction_failed";
  }
  return "invalid";
}

std::string RetryabilityName(const CanonicalResultRetryability retryability) {
  switch (retryability) {
    case CanonicalResultRetryability::kNotRetryable: return "not_retryable";
    case CanonicalResultRetryability::kRetrySameSnapshot:
      return "retry_same_snapshot";
    case CanonicalResultRetryability::kRetryNewSnapshot:
      return "retry_new_snapshot";
  }
  return "invalid";
}

std::optional<std::map<std::string, std::string>> ParseDescriptorFields(
    const std::string_view encoded) {
  std::map<std::string, std::string> fields;
  std::size_t begin = 0;
  while (begin < encoded.size()) {
    const auto end = encoded.find(';', begin);
    const auto part = encoded.substr(
        begin, end == std::string_view::npos ? encoded.size() - begin
                                             : end - begin);
    if (part.empty()) return std::nullopt;
    const auto separator = part.find('=');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= part.size()) {
      return std::nullopt;
    }
    if (!fields.emplace(std::string(part.substr(0, separator)),
                        std::string(part.substr(separator + 1)))
             .second) {
      return std::nullopt;
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
    if (begin == encoded.size()) break;
  }
  return fields;
}

bool OptionalFieldMatches(
    const std::map<std::string, std::string>& fields,
    const std::string_view key,
    const std::optional<std::string>& expected) {
  const auto iterator = fields.find(std::string(key));
  if (expected.has_value()) {
    return iterator != fields.end() && iterator->second == *expected;
  }
  return iterator == fields.end();
}

bool IsCanonicalSqlstate(const std::string_view value) {
  if (value.size() != 5) return false;
  for (const auto ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (!std::isdigit(byte) && !(ch >= 'A' && ch <= 'Z')) return false;
  }
  return true;
}

DescriptorRuntimeDiagnostic ValidatePublishedDescriptor(
    const ExecutorColumnDescriptor& physical,
    const CanonicalResultColumnDescriptor& published,
    const std::uint32_t expected_ordinal) {
  if (published.ordinal != expected_ordinal || published.name_utf8.empty() ||
      published.name_utf8 != physical.stable_name ||
      published.descriptor_uuid !=
          physical.descriptor.descriptor_uuid.canonical ||
      !IsCanonicalUuid(published.descriptor_uuid) ||
      !IsCanonicalUuid(published.type_uuid) ||
      !IsValidNullability(published.nullability) ||
      (published.collation_uuid.has_value() &&
       !IsCanonicalUuid(*published.collation_uuid)) ||
      (published.timezone_profile_id.has_value() &&
       published.timezone_profile_id->empty())) {
    return Refusal("published result descriptor identity is invalid", 0,
                   expected_ordinal);
  }
  const auto fields =
      ParseDescriptorFields(physical.descriptor.encoded_descriptor);
  if (!fields.has_value()) {
    return Refusal("physical descriptor encoding is malformed", 0,
                   expected_ordinal);
  }
  const auto type = fields->find("type_uuid");
  const auto canonical_nullability = fields->find("nullability");
  const auto storage_nullable = fields->find("nullable");
  std::optional<CanonicalResultNullability> admitted_nullability;
  if (canonical_nullability != fields->end()) {
    if (canonical_nullability->second == "non_null") {
      admitted_nullability = CanonicalResultNullability::kNonNull;
    } else if (canonical_nullability->second == "nullable") {
      admitted_nullability = CanonicalResultNullability::kNullable;
    } else if (canonical_nullability->second == "unknown") {
      admitted_nullability = CanonicalResultNullability::kUnknown;
    } else {
      return Refusal("physical descriptor nullability is malformed", 0,
                     expected_ordinal);
    }
  }
  // QOW-SOURCE-QRY-021-STORAGE-NULLABILITY-CARRIER-V1
  // Persisted MGA descriptors use an exact boolean carrier. It is admitted
  // without rewriting the descriptor; when both carriers exist they must
  // express the same nullability.
  if (storage_nullable != fields->end()) {
    std::optional<CanonicalResultNullability> storage_nullability;
    if (storage_nullable->second == "true") {
      storage_nullability = CanonicalResultNullability::kNullable;
    } else if (storage_nullable->second == "false") {
      storage_nullability = CanonicalResultNullability::kNonNull;
    } else {
      return Refusal("physical descriptor nullable carrier is malformed", 0,
                     expected_ordinal);
    }
    if (admitted_nullability.has_value() &&
        *admitted_nullability != *storage_nullability) {
      return Refusal("physical descriptor nullability carriers contradict", 0,
                     expected_ordinal);
    }
    admitted_nullability = storage_nullability;
  }
  if (type == fields->end() || type->second != published.type_uuid ||
      !admitted_nullability.has_value() ||
      *admitted_nullability != published.nullability ||
      !OptionalFieldMatches(*fields, "collation_uuid",
                            published.collation_uuid) ||
      !OptionalFieldMatches(*fields, "timezone_profile_id",
                            published.timezone_profile_id)) {
    return Refusal("published result descriptor differs from physical authority",
                   0, expected_ordinal);
  }
  const bool published_can_be_null =
      published.nullability != CanonicalResultNullability::kNonNull;
  if (published_can_be_null != physical.nullable) {
    return Refusal("published result nullability differs from physical authority",
                   0, expected_ordinal);
  }
  return {};
}

DescriptorRuntimeDiagnostic ValidateDiagnosticArgument(
    const EngineTypedValue& value,
    const std::size_t diagnostic_index,
    const std::size_t argument_index) {
  const auto fields =
      ParseDescriptorFields(value.descriptor.encoded_descriptor);
  if (!IsCanonicalUuid(value.descriptor.descriptor_uuid.canonical) ||
      value.descriptor.descriptor_kind != "scalar" ||
      value.descriptor.canonical_type_name.empty() ||
      !fields.has_value()) {
    return Refusal("diagnostic argument descriptor is not canonical",
                   diagnostic_index, argument_index);
  }
  const auto type = fields->find("type_uuid");
  const auto nullability = fields->find("nullability");
  if (type == fields->end() || !IsCanonicalUuid(type->second) ||
      nullability == fields->end() ||
      (nullability->second != "non_null" &&
       nullability->second != "nullable" &&
       nullability->second != "unknown")) {
    return Refusal("diagnostic argument type fields are not canonical",
                   diagnostic_index, argument_index);
  }
  const auto collation = fields->find("collation_uuid");
  if (collation != fields->end() && !IsCanonicalUuid(collation->second)) {
    return Refusal("diagnostic argument collation is not canonical",
                   diagnostic_index, argument_index);
  }
  const auto timezone = fields->find("timezone_profile_id");
  if (timezone != fields->end() && timezone->second.empty()) {
    return Refusal("diagnostic argument timezone profile is empty",
                   diagnostic_index, argument_index);
  }
  if (value.state == EngineValueState::sql_null) {
    if (!value.is_null || !value.encoded_value.empty() ||
        !value.binary_value.empty() || nullability->second == "non_null") {
      return Refusal("diagnostic SQL NULL argument carries payload",
                     diagnostic_index, argument_index);
    }
    return {};
  }
  if (value.state != EngineValueState::value || value.is_null) {
    return Refusal("diagnostic argument contains a non-result sentinel",
                   diagnostic_index, argument_index);
  }
  return {};
}

DescriptorRuntimeDiagnostic ValidateDiagnostics(
    const std::vector<CanonicalResultDiagnosticRecord>& diagnostics) {
  std::unordered_set<std::string> diagnostic_ids;
  for (std::size_t index = 0; index < diagnostics.size(); ++index) {
    const auto& diagnostic = diagnostics[index];
    if (diagnostic.diagnostic_id.empty() || diagnostic.stable_code.empty() ||
        diagnostic.message_key.empty() ||
        !diagnostic_ids.insert(diagnostic.diagnostic_id).second ||
        !IsValidSeverity(diagnostic.severity) ||
        !IsValidPhase(diagnostic.phase) ||
        !IsValidTransactionEffect(diagnostic.transaction_effect) ||
        !IsValidRetryability(diagnostic.retryability) ||
        (diagnostic.sqlstate.has_value() &&
         !IsCanonicalSqlstate(*diagnostic.sqlstate)) ||
        (diagnostic.record_path.has_value() &&
         diagnostic.record_path->empty()) ||
        (diagnostic.field_id.has_value() && diagnostic.field_id->empty()) ||
        (diagnostic.physical_node_id.has_value() &&
         *diagnostic.physical_node_id == 0)) {
      return Refusal("diagnostic record contains an invalid canonical field",
                     index, 0);
    }
    for (std::size_t argument = 0;
         argument < diagnostic.argument_values.size(); ++argument) {
      const auto validated = ValidateDiagnosticArgument(
          diagnostic.argument_values[argument], index, argument);
      if (!validated.ok) return validated;
    }
  }
  return {};
}

void AppendLengthField(std::ostringstream* out,
                       const std::string_view key,
                       const std::string_view value) {
  *out << key << '=' << value.size() << ':' << value << '\n';
}

std::string HexBytes(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    output.push_back(kHex[byte >> 4]);
    output.push_back(kHex[byte & 0x0f]);
  }
  return output;
}

std::string EncodeEnvelope(const CanonicalResultEnvelopeV1& envelope) {
  std::ostringstream out;
  AppendLengthField(&out, "abi_family_id", "QOW-RESULT-DIAGNOSTIC-ABI-V1");
  AppendLengthField(&out, "abi_version", std::to_string(envelope.abi_version));
  AppendLengthField(&out, "statement_uuid", envelope.statement_uuid);
  AppendLengthField(&out, "execution_attempt_uuid",
                    envelope.execution_attempt_uuid);
  AppendLengthField(&out, "result_kind", ResultKindName(envelope.result_kind));
  AppendLengthField(&out, "column_descriptor_count",
                    std::to_string(envelope.column_descriptors.size()));
  for (const auto& column : envelope.column_descriptors) {
    AppendLengthField(&out, "column.ordinal", std::to_string(column.ordinal));
    AppendLengthField(&out, "column.name_utf8", column.name_utf8);
    AppendLengthField(&out, "column.descriptor_uuid", column.descriptor_uuid);
    AppendLengthField(&out, "column.type_uuid", column.type_uuid);
    AppendLengthField(&out, "column.nullability",
                      NullabilityName(column.nullability));
    AppendLengthField(&out, "column.collation_uuid",
                      column.collation_uuid.value_or(""));
    AppendLengthField(&out, "column.timezone_profile_id",
                      column.timezone_profile_id.value_or(""));
  }
  AppendLengthField(&out, "row_stream_format_id",
                    envelope.row_stream_format_id);
  AppendLengthField(&out, "row_count",
                    envelope.row_count.has_value()
                        ? std::to_string(*envelope.row_count)
                        : "");
  AppendLengthField(&out, "command_tag",
                    envelope.command_tag.value_or(""));
  AppendLengthField(&out, "cursor_state",
                    envelope.cursor_state.has_value()
                        ? CursorStateName(*envelope.cursor_state)
                        : "");
  AppendLengthField(&out, "diagnostic_count",
                    std::to_string(envelope.diagnostics.size()));
  for (const auto& diagnostic : envelope.diagnostics) {
    AppendLengthField(&out, "diagnostic.id", diagnostic.diagnostic_id);
    AppendLengthField(&out, "diagnostic.stable_code", diagnostic.stable_code);
    AppendLengthField(&out, "diagnostic.severity",
                      SeverityName(diagnostic.severity));
    AppendLengthField(&out, "diagnostic.sqlstate",
                      diagnostic.sqlstate.value_or(""));
    AppendLengthField(&out, "diagnostic.message_key", diagnostic.message_key);
    AppendLengthField(&out, "diagnostic.phase", PhaseName(diagnostic.phase));
    AppendLengthField(&out, "diagnostic.record_path",
                      diagnostic.record_path.value_or(""));
    AppendLengthField(&out, "diagnostic.field_id",
                      diagnostic.field_id.value_or(""));
    AppendLengthField(
        &out, "diagnostic.physical_node_id",
        diagnostic.physical_node_id.has_value()
            ? std::to_string(*diagnostic.physical_node_id)
            : "");
    AppendLengthField(&out, "diagnostic.transaction_effect",
                      TransactionEffectName(diagnostic.transaction_effect));
    AppendLengthField(&out, "diagnostic.retryability",
                      RetryabilityName(diagnostic.retryability));
    AppendLengthField(&out, "diagnostic.argument_count",
                      std::to_string(diagnostic.argument_values.size()));
    for (const auto& argument : diagnostic.argument_values) {
      AppendLengthField(&out, "argument.descriptor_uuid",
                        argument.descriptor.descriptor_uuid.canonical);
      AppendLengthField(&out, "argument.descriptor_kind",
                        argument.descriptor.descriptor_kind);
      AppendLengthField(&out, "argument.canonical_type_name",
                        argument.descriptor.canonical_type_name);
      AppendLengthField(&out, "argument.encoded_descriptor",
                        argument.descriptor.encoded_descriptor);
      AppendLengthField(&out, "argument.state",
                        std::to_string(static_cast<std::uint8_t>(argument.state)));
      AppendLengthField(&out, "argument.encoded_value", argument.encoded_value);
      AppendLengthField(&out, "argument.binary_value",
                        HexBytes(argument.binary_value));
    }
  }
  return out.str();
}

}  // namespace

// QOW-SOURCE-QRY-021-V1
// QOW-SOURCE-IAS-010-V1
// This is the sole descriptor-to-result publication boundary for the bounded
// canonical runtime. It reports an engine-supplied transaction effect but has
// no authority to choose transaction state, visibility, recovery, or finality.
CanonicalResultPublicationResult PublishCanonicalResultEnvelope(
    const CanonicalResultPublicationRequest& request) {
  CanonicalResultPublicationResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    return result;
  };

  if (request.abi_version != 1 ||
      !IsCanonicalUuid(request.statement_uuid) ||
      !IsCanonicalUuid(request.execution_attempt_uuid) ||
      request.statement_uuid == request.execution_attempt_uuid ||
      !IsCanonicalUuid(request.transaction_effect_evidence_uuid) ||
      !IsValidResultKind(request.result_kind) ||
      !IsValidInvocationMode(request.invocation_mode) ||
      request.row_stream_format_id.empty() ||
      request.maximum_row_count == 0) {
    return refuse(Refusal("result envelope identity or version is invalid"));
  }
  if (request.physical_output_batch.rows.size() > request.maximum_row_count) {
    return refuse(Refusal("result row count exceeds the publication bound"));
  }

  const auto diagnostics = ValidateDiagnostics(request.diagnostics);
  if (!diagnostics.ok) return refuse(diagnostics);

  const bool command = request.result_kind == CanonicalResultKind::kCommand;
  const bool cursor = request.result_kind == CanonicalResultKind::kCursor;
  const bool empty = request.result_kind == CanonicalResultKind::kEmpty;
  if (command) {
    if (!request.physical_output_batch.columns.empty() ||
        !request.physical_output_batch.rows.empty() ||
        !request.column_bindings.empty() || !request.command_tag.has_value() ||
        request.command_tag->empty() || request.cursor_state.has_value()) {
      return refuse(Refusal("command result fields are contradictory"));
    }
  } else if (cursor) {
    if (request.command_tag.has_value() || !request.cursor_state.has_value() ||
        !IsValidCursorState(*request.cursor_state)) {
      return refuse(Refusal("cursor result requires one canonical cursor state"));
    }
  } else if (request.command_tag.has_value() || request.cursor_state.has_value()) {
    return refuse(Refusal("non-command/non-cursor result carries route fields"));
  }
  if (empty && !request.physical_output_batch.rows.empty()) {
    return refuse(Refusal("empty result carries physical rows"));
  }

  if (!request.physical_output_batch.columns.empty()) {
    std::vector<std::uint32_t> descriptor_ids;
    descriptor_ids.reserve(request.physical_output_batch.columns.size());
    for (const auto& column : request.physical_output_batch.columns) {
      descriptor_ids.push_back(column.descriptor_id);
    }
    const auto batch_validation = ValidateCanonicalDescriptorBatch(
        request.physical_output_batch, descriptor_ids);
    if (!batch_validation.ok) {
      return refuse(Refusal("physical result batch is not canonical",
                            batch_validation.row_index,
                            batch_validation.column_index));
    }
    if (request.column_bindings.size() !=
        request.physical_output_batch.columns.size()) {
      return refuse(Refusal("result column bindings do not cover the physical shape"));
    }
  } else if (!request.physical_output_batch.rows.empty() ||
             !request.column_bindings.empty()) {
    return refuse(Refusal("descriptor-free result carries rows or bindings"));
  }

  DescriptorBatch visible_batch;
  std::uint32_t visible_ordinal = 0;
  for (std::size_t index = 0; index < request.column_bindings.size(); ++index) {
    const auto& binding = request.column_bindings[index];
    if (binding.physical_column_ordinal != index) {
      return refuse(Refusal("result column bindings are not in physical order",
                            0, index));
    }
    if (!binding.visible) {
      if (binding.published_descriptor.has_value()) {
        return refuse(Refusal("hidden result column carries published metadata",
                              0, index));
      }
      continue;
    }
    if (!binding.published_descriptor.has_value()) {
      return refuse(Refusal("visible result column lacks published metadata",
                            0, index));
    }
    const auto validated = ValidatePublishedDescriptor(
        request.physical_output_batch.columns[index],
        *binding.published_descriptor, visible_ordinal);
    if (!validated.ok) return refuse(validated);
    visible_batch.columns.push_back(request.physical_output_batch.columns[index]);
    visible_batch.columns.back().stable_name =
        binding.published_descriptor->name_utf8;
    ++visible_ordinal;
  }
  if (!request.physical_output_batch.columns.empty() &&
      visible_batch.columns.empty()) {
    return refuse(Refusal("result shape hides every physical column"));
  }

  visible_batch.rows.reserve(request.physical_output_batch.rows.size());
  for (const auto& physical_row : request.physical_output_batch.rows) {
    DescriptorTuple visible_row;
    visible_row.values.reserve(visible_batch.columns.size());
    for (std::size_t index = 0; index < request.column_bindings.size(); ++index) {
      if (request.column_bindings[index].visible) {
        visible_row.values.push_back(physical_row.values[index]);
      }
    }
    visible_batch.rows.push_back(std::move(visible_row));
  }

  result.envelope.abi_version = 1;
  result.envelope.statement_uuid = request.statement_uuid;
  result.envelope.execution_attempt_uuid = request.execution_attempt_uuid;
  result.envelope.result_kind = request.result_kind;
  for (const auto& binding : request.column_bindings) {
    if (binding.visible) {
      result.envelope.column_descriptors.push_back(
          *binding.published_descriptor);
    }
  }
  result.envelope.row_stream_format_id = request.row_stream_format_id;
  if (request.result_kind == CanonicalResultKind::kRows || empty ||
      request.result_kind == CanonicalResultKind::kExplain) {
    result.envelope.row_count =
        static_cast<std::uint64_t>(visible_batch.rows.size());
  }
  result.envelope.command_tag = request.command_tag;
  result.envelope.cursor_state = request.cursor_state;
  result.envelope.diagnostics = request.diagnostics;
  result.row_stream = std::move(visible_batch);
  result.canonical_envelope_bytes = EncodeEnvelope(result.envelope);

  result.delivery_records.push_back(
      {CanonicalResultDeliveryKind::kMetadata, std::nullopt});
  for (std::size_t row = 0; row < result.row_stream.rows.size(); ++row) {
    result.delivery_records.push_back(
        {CanonicalResultDeliveryKind::kRow, row});
  }
  if (!result.envelope.diagnostics.empty()) {
    result.delivery_records.push_back(
        {CanonicalResultDeliveryKind::kDiagnostics, std::nullopt});
  }
  result.diagnostic = {};
  result.published = true;
  return result;
}

}  // namespace scratchbird::engine::executor
