// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {

struct CanonicalResultCursorSession::State {
  std::mutex mutex;
  std::string cursor_uuid;
  std::string statement_uuid;
  PhysicalMgaStatementContext mga_statement_context;
  std::string catalog_epoch_uuid;
  std::string execution_attempt_uuid;
  TypedPhysicalNodeDag selected_physical_dag;
  CanonicalResultInvocationMode invocation_mode =
      CanonicalResultInvocationMode::kDirect;
  std::string row_stream_format_id;
  std::vector<CanonicalResultColumnDescriptor> column_descriptors;
  std::vector<ExecutorColumnDescriptor> physical_columns;
  std::size_t maximum_rows_per_batch = 0;
  std::uint64_t next_batch_ordinal = 0;
  std::uint64_t next_row_ordinal = 0;
  bool metadata_delivered = false;
  bool released = false;
  CanonicalResultCursorCancellationProbe cancellation_requested;
  CanonicalResultDiagnosticRecord cancellation_diagnostic;
  CanonicalResultCursorReleaseCallback release;
};

CanonicalResultCursorSession::CanonicalResultCursorSession(
    std::unique_ptr<State> state)
    : state_(std::move(state)) {}

CanonicalResultCursorSession::~CanonicalResultCursorSession() {
  Release(CanonicalResultCursorReleaseReason::kAbandoned);
}

bool CanonicalResultCursorSession::Release(
    const CanonicalResultCursorReleaseReason reason) noexcept {
  CanonicalResultCursorReleaseCallback release;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->released) return true;
    state_->released = true;
    release = state_->release;
  }
  try {
    release(reason);
    return true;
  } catch (...) {
    return false;
  }
}

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

bool ResultDescriptorEqual(const CanonicalResultColumnDescriptor& left,
                           const CanonicalResultColumnDescriptor& right) {
  return left.ordinal == right.ordinal &&
         left.name_utf8 == right.name_utf8 &&
         left.descriptor_uuid == right.descriptor_uuid &&
         left.type_uuid == right.type_uuid &&
         left.nullability == right.nullability &&
         left.collation_uuid == right.collation_uuid &&
         left.timezone_profile_id == right.timezone_profile_id;
}

bool ResultDescriptorVectorsEqual(
    const std::vector<CanonicalResultColumnDescriptor>& left,
    const std::vector<CanonicalResultColumnDescriptor>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!ResultDescriptorEqual(left[index], right[index])) return false;
  }
  return true;
}

bool ExecutorColumnDescriptorEqual(const ExecutorColumnDescriptor& left,
                                   const ExecutorColumnDescriptor& right) {
  return left.stable_name == right.stable_name &&
         left.nullable == right.nullable &&
         left.descriptor_id == right.descriptor_id &&
         left.descriptor.descriptor_uuid.canonical ==
             right.descriptor.descriptor_uuid.canonical &&
         left.descriptor.descriptor_kind == right.descriptor.descriptor_kind &&
         left.descriptor.canonical_type_name ==
             right.descriptor.canonical_type_name &&
         left.descriptor.encoded_descriptor ==
             right.descriptor.encoded_descriptor;
}

bool ExecutorColumnDescriptorVectorsEqual(
    const std::vector<ExecutorColumnDescriptor>& left,
    const std::vector<ExecutorColumnDescriptor>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!ExecutorColumnDescriptorEqual(left[index], right[index])) return false;
  }
  return true;
}

bool PhysicalNodeRecordEqual(const PhysicalNodeRecord& left,
                             const PhysicalNodeRecord& right) {
  return left.physical_node_id == right.physical_node_id &&
         left.relational_node_id == right.relational_node_id &&
         left.node_kind == right.node_kind &&
         left.implementation_id == right.implementation_id &&
         left.input_physical_node_ids == right.input_physical_node_ids &&
         left.output_descriptor_ids == right.output_descriptor_ids &&
         left.shareable == right.shareable &&
         left.causal_counter_id == right.causal_counter_id &&
         left.selected_alternative_uuid == right.selected_alternative_uuid &&
         left.executor_capability_uuid == right.executor_capability_uuid &&
         left.executor_capability_abi_version ==
             right.executor_capability_abi_version &&
         left.cost_vector_uuid == right.cost_vector_uuid &&
         left.required_property_uuids == right.required_property_uuids &&
         left.delivered_property_uuids == right.delivered_property_uuids &&
         left.memory_bytes_required == right.memory_bytes_required &&
         left.spill_bytes_expected == right.spill_bytes_expected &&
         left.engine_capability_validated == right.engine_capability_validated &&
         left.retained_cost == right.retained_cost &&
         PhysicalMgaStatementContextEqual(left.mga_statement_context,
                                          right.mga_statement_context);
}

bool PhysicalDagIdentityEqual(const TypedPhysicalNodeDag& left,
                              const TypedPhysicalNodeDag& right) {
  if (left.abi_version != right.abi_version ||
      left.selected_plan_uuid != right.selected_plan_uuid ||
      left.root_physical_node_id != right.root_physical_node_id ||
      left.local_transaction_id != right.local_transaction_id ||
      left.statement_snapshot_id != right.statement_snapshot_id ||
      !PhysicalMgaStatementContextEqual(left.mga_statement_context,
                                        right.mga_statement_context) ||
      left.bound_sblr_tree_uuid != right.bound_sblr_tree_uuid ||
      left.catalog_epoch_uuid != right.catalog_epoch_uuid ||
      left.security_context_uuid != right.security_context_uuid ||
      left.capability_snapshot_uuid != right.capability_snapshot_uuid ||
      left.resource_snapshot_uuid != right.resource_snapshot_uuid ||
      left.statistics_snapshot_uuid != right.statistics_snapshot_uuid ||
      left.route_snapshot_uuid != right.route_snapshot_uuid ||
      left.catalog_generation != right.catalog_generation ||
      left.security_epoch != right.security_epoch ||
      left.policy_epoch != right.policy_epoch ||
      left.resource_epoch != right.resource_epoch ||
      left.statistics_generation != right.statistics_generation ||
      left.route_epoch != right.route_epoch ||
      left.route_generation != right.route_generation ||
      left.memory_budget_bytes != right.memory_budget_bytes ||
      left.spill_allowed != right.spill_allowed ||
      left.optimizer_published != right.optimizer_published ||
      left.immutable_node_identity_validated !=
          right.immutable_node_identity_validated ||
      left.capability_validated_before_access !=
          right.capability_validated_before_access ||
      left.data_access_observed != right.data_access_observed ||
      left.parser_execution_authority_claimed !=
          right.parser_execution_authority_claimed ||
      left.transaction_finality_authority_claimed !=
          right.transaction_finality_authority_claimed ||
      left.admission_evidence.size() != right.admission_evidence.size() ||
      left.nodes.size() != right.nodes.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.admission_evidence.size(); ++index) {
    if (left.admission_evidence[index].stage !=
            right.admission_evidence[index].stage ||
        left.admission_evidence[index].evidence_uuid !=
            right.admission_evidence[index].evidence_uuid) {
      return false;
    }
  }
  for (std::size_t index = 0; index < left.nodes.size(); ++index) {
    if (!PhysicalNodeRecordEqual(left.nodes[index], right.nodes[index])) {
      return false;
    }
  }
  return true;
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

void AppendMgaStatementContext(
    std::ostringstream* out,
    const PhysicalMgaStatementContext& context) {
  AppendLengthField(out, "mga.statement_uuid", context.statement_uuid);
  AppendLengthField(out, "mga.owning_transaction_uuid",
                    context.owning_transaction_uuid);
  AppendLengthField(out, "mga.statement_snapshot_uuid",
                    context.statement_snapshot_uuid);
  AppendLengthField(out, "mga.statement_metadata_snapshot_uuid",
                    context.statement_metadata_snapshot_uuid);
  AppendLengthField(out, "mga.owning_local_transaction_id",
                    std::to_string(context.owning_local_transaction_id));
  AppendLengthField(
      out, "mga.visible_committed_high_watermark",
      std::to_string(context.visible_committed_high_watermark));
  AppendLengthField(out, "mga.oldest_active_transaction_id",
                    std::to_string(context.oldest_active_transaction_id));
  AppendLengthField(
      out, "mga.oldest_interesting_transaction_id",
      std::to_string(context.oldest_interesting_transaction_id));
  AppendLengthField(
      out, "mga.oldest_snapshot_transaction_id",
      std::to_string(context.oldest_snapshot_transaction_id));
  AppendLengthField(
      out, "mga.retention_horizon_transaction_id",
      std::to_string(context.retention_horizon_transaction_id));
  AppendLengthField(
      out, "mga.active_excluded_local_transaction_id_count",
      std::to_string(context.active_excluded_local_transaction_ids.size()));
  for (const auto transaction_id :
       context.active_excluded_local_transaction_ids) {
    AppendLengthField(out, "mga.active_excluded_local_transaction_id",
                      std::to_string(transaction_id));
  }
  AppendLengthField(
      out, "mga.in_doubt_excluded_local_transaction_id_count",
      std::to_string(context.in_doubt_excluded_local_transaction_ids.size()));
  for (const auto transaction_id :
       context.in_doubt_excluded_local_transaction_ids) {
    AppendLengthField(out, "mga.in_doubt_excluded_local_transaction_id",
                      std::to_string(transaction_id));
  }
  AppendLengthField(out, "mga.snapshot_kind", context.snapshot_kind);
  AppendLengthField(
      out, "mga.publication_inventory_next_local_transaction_id",
      std::to_string(
          context.publication_inventory_next_local_transaction_id));
  AppendLengthField(out, "mga.inventory_authoritative",
                    context.inventory_authoritative ? "true" : "false");
  AppendLengthField(out, "mga.complete",
                    context.complete ? "true" : "false");
  AppendLengthField(out, "mga.current",
                    context.current ? "true" : "false");
}

std::string EncodeEnvelope(const CanonicalResultEnvelopeV1& envelope) {
  std::ostringstream out;
  AppendLengthField(&out, "abi_family_id", "QOW-RESULT-DIAGNOSTIC-ABI-V1");
  AppendLengthField(&out, "abi_version", std::to_string(envelope.abi_version));
  AppendLengthField(&out, "statement_uuid", envelope.statement_uuid);
  AppendMgaStatementContext(&out, envelope.mga_statement_context);
  AppendLengthField(&out, "catalog_epoch_uuid", envelope.catalog_epoch_uuid);
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
    if (request.cursor_session) {
      request.cursor_session->Release(
          CanonicalResultCursorReleaseReason::kError);
    }
    result = {};
    result.diagnostic = std::move(diagnostic);
    return result;
  };

#if defined(SCRATCHBIRD_QOW_RESULT_PUBLISHER_CLOSURE_TEST_ONLY) && \
    SCRATCHBIRD_QOW_RESULT_PUBLISHER_CLOSURE_TEST_ONLY == 1
  const bool accepted_mga_authority_origin =
      request.mga_authority.origin ==
          CanonicalMgaAuthorityOrigin::kEngineTransactionInventory ||
      request.mga_authority.origin ==
          CanonicalMgaAuthorityOrigin::kClosureTestSeam;
#else
  const bool accepted_mga_authority_origin =
      request.mga_authority.origin ==
          CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
#endif

  if (request.abi_version != 1 ||
      !IsCanonicalUuid(request.statement_uuid) ||
      !accepted_mga_authority_origin ||
      !IsCanonicalUuid(request.selected_catalog_epoch_uuid) ||
      !IsCanonicalUuid(request.execution_attempt_uuid) ||
      request.statement_uuid == request.execution_attempt_uuid ||
      !IsCanonicalUuid(request.transaction_effect_evidence_uuid) ||
      !IsValidResultKind(request.result_kind) ||
      !IsValidInvocationMode(request.invocation_mode) ||
      request.row_stream_format_id.empty() ||
      request.maximum_row_count == 0 ||
      request.maximum_rows_per_batch == 0) {
    return refuse(Refusal("result envelope identity or version is invalid"));
  }
  if (request.physical_output_batch.rows.size() > request.maximum_row_count) {
    return refuse(Refusal("result row count exceeds the publication bound"));
  }

  const auto diagnostics = ValidateDiagnostics(request.diagnostics);
  if (!diagnostics.ok) return refuse(diagnostics);
  if (request.cursor_cancellation_diagnostic.has_value()) {
    const auto cancellation_diagnostic = ValidateDiagnostics(
        {*request.cursor_cancellation_diagnostic});
    if (!cancellation_diagnostic.ok) return refuse(cancellation_diagnostic);
  }

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
        !IsValidCursorState(*request.cursor_state) ||
        !IsCanonicalUuid(request.cursor_uuid)) {
      return refuse(Refusal("cursor result requires one canonical cursor state"));
    }
    if (request.cursor_session) {
      if (request.cursor_cancellation_requested ||
          request.cursor_cancellation_diagnostic.has_value() ||
          request.cursor_release) {
        return refuse(Refusal(
            "cursor continuation cannot replace its lifecycle controls"));
      }
    } else if (!request.cursor_cancellation_requested ||
               !request.cursor_cancellation_diagnostic.has_value() ||
               !request.cursor_release || request.cursor_batch_ordinal != 0 ||
               request.cursor_first_row_ordinal != 0) {
      return refuse(Refusal(
          "initial cursor result lacks canonical lifecycle controls"));
    }
    if (*request.cursor_state == CanonicalResultCursorState::kOpen &&
        request.physical_output_batch.rows.empty()) {
      return refuse(Refusal("open cursor page cannot make zero-row progress"));
    }
  } else if (request.command_tag.has_value() || request.cursor_state.has_value()) {
    return refuse(Refusal("non-command/non-cursor result carries route fields"));
  }
  if (!cursor && (!request.cursor_uuid.empty() || request.cursor_session ||
                  request.cursor_batch_ordinal != 0 ||
                  request.cursor_first_row_ordinal != 0 ||
                  request.cursor_cancellation_requested ||
                  request.cursor_cancellation_diagnostic.has_value() ||
                  request.cursor_release)) {
    return refuse(Refusal("non-cursor result carries cursor lifecycle state"));
  }
  if (cursor && request.physical_output_batch.rows.size() >
                    request.maximum_rows_per_batch) {
    return refuse(Refusal("cursor page exceeds its fixed batch bound"));
  }
  const bool terminal_diagnostic = std::any_of(
      request.diagnostics.begin(), request.diagnostics.end(),
      [](const CanonicalResultDiagnosticRecord& diagnostic) {
        return diagnostic.severity == CanonicalResultDiagnosticSeverity::kError ||
               diagnostic.severity == CanonicalResultDiagnosticSeverity::kFatal;
      });
  if (terminal_diagnostic &&
      (!request.physical_output_batch.rows.empty() ||
       (cursor && *request.cursor_state != CanonicalResultCursorState::kClosed))) {
    return refuse(Refusal(
        "terminal result diagnostic must be row-free and close its cursor"));
  }
  if (request.cursor_cancellation_diagnostic.has_value() &&
      request.cursor_cancellation_diagnostic->severity !=
          CanonicalResultDiagnosticSeverity::kError &&
      request.cursor_cancellation_diagnostic->severity !=
          CanonicalResultDiagnosticSeverity::kFatal) {
    return refuse(Refusal("cursor cancellation diagnostic is not terminal"));
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
    ++visible_ordinal;
  }
  if (!request.physical_output_batch.columns.empty() &&
      visible_ordinal == 0) {
    return refuse(Refusal("result shape hides every physical column"));
  }

  // Result publication is a statement use. Re-resolve the current MGA
  // authority at this final boundary, after structural validation and
  // immediately before any result metadata or row is materialized. Ordinary
  // builds admit only durable engine inventory; the compile-bounded closure
  // build may also admit its immutable test resolver through the same check.
  const auto authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.selected_physical_dag);
  if (!authority.ok) return refuse(authority);
  const auto& statement_context = request.mga_authority.statement_context;
  if (request.statement_uuid != statement_context.statement_uuid ||
      request.selected_catalog_epoch_uuid !=
          request.selected_physical_dag.catalog_epoch_uuid ||
      request.selected_catalog_epoch_uuid == statement_context.statement_uuid ||
      request.selected_catalog_epoch_uuid ==
          statement_context.owning_transaction_uuid ||
      request.selected_catalog_epoch_uuid ==
          statement_context.statement_snapshot_uuid ||
      request.selected_catalog_epoch_uuid ==
          statement_context.statement_metadata_snapshot_uuid) {
    return refuse(Refusal(
        "result statement authority or independent catalog epoch is not "
        "the selected execution identity"));
  }

  DescriptorBatch visible_batch;
  std::vector<CanonicalResultColumnDescriptor> published_descriptors;
  visible_batch.columns.reserve(visible_ordinal);
  published_descriptors.reserve(visible_ordinal);
  for (std::size_t index = 0; index < request.column_bindings.size(); ++index) {
    const auto& binding = request.column_bindings[index];
    if (!binding.visible) continue;
    visible_batch.columns.push_back(request.physical_output_batch.columns[index]);
    visible_batch.columns.back().stable_name =
        binding.published_descriptor->name_utf8;
    published_descriptors.push_back(*binding.published_descriptor);
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

  std::shared_ptr<CanonicalResultCursorSession> cursor_session;
  bool publish_metadata = true;
  bool cursor_cancelled = false;
  bool release_cursor = false;
  CanonicalResultCursorReleaseReason release_reason =
      CanonicalResultCursorReleaseReason::kCompleted;
  std::uint64_t delivery_batch_ordinal = 0;
  std::uint64_t delivery_first_row_ordinal = 0;

  if (cursor) {
    cursor_session = request.cursor_session;
    if (!cursor_session) {
      auto state = std::make_unique<CanonicalResultCursorSession::State>();
      state->cursor_uuid = request.cursor_uuid;
      state->statement_uuid = request.statement_uuid;
      state->mga_statement_context = statement_context;
      state->catalog_epoch_uuid = request.selected_catalog_epoch_uuid;
      state->execution_attempt_uuid = request.execution_attempt_uuid;
      state->selected_physical_dag = request.selected_physical_dag;
      state->invocation_mode = request.invocation_mode;
      state->row_stream_format_id = request.row_stream_format_id;
      state->column_descriptors = published_descriptors;
      state->physical_columns = request.physical_output_batch.columns;
      state->maximum_rows_per_batch = request.maximum_rows_per_batch;
      state->cancellation_requested = request.cursor_cancellation_requested;
      state->cancellation_diagnostic =
          *request.cursor_cancellation_diagnostic;
      state->release = request.cursor_release;
      cursor_session = std::shared_ptr<CanonicalResultCursorSession>(
          new CanonicalResultCursorSession(std::move(state)));
    }

    const auto refuse_cursor = [&](DescriptorRuntimeDiagnostic diagnostic) {
      cursor_session->Release(CanonicalResultCursorReleaseReason::kError);
      result = {};
      result.diagnostic = std::move(diagnostic);
      return result;
    };

    try {
      if (cursor_session->state_->cancellation_requested()) {
        cursor_cancelled = true;
      }
      for (std::size_t row = 0;
           !cursor_cancelled && row < visible_batch.rows.size(); ++row) {
        if (cursor_session->state_->cancellation_requested()) {
          cursor_cancelled = true;
        }
      }
      if (!cursor_cancelled &&
          cursor_session->state_->cancellation_requested()) {
        cursor_cancelled = true;
      }
    } catch (...) {
      return refuse_cursor(
          Refusal("cursor cancellation probe raised an exception"));
    }

    std::unique_lock<std::mutex> lock(cursor_session->state_->mutex);
    auto& state = *cursor_session->state_;
    if (state.released || state.cursor_uuid != request.cursor_uuid ||
        state.statement_uuid != request.statement_uuid ||
        !PhysicalMgaStatementContextEqual(state.mga_statement_context,
                                          statement_context) ||
        state.catalog_epoch_uuid != request.selected_catalog_epoch_uuid ||
        state.execution_attempt_uuid != request.execution_attempt_uuid ||
        !PhysicalDagIdentityEqual(state.selected_physical_dag,
                                  request.selected_physical_dag) ||
        state.invocation_mode != request.invocation_mode ||
        state.row_stream_format_id != request.row_stream_format_id ||
        state.maximum_rows_per_batch != request.maximum_rows_per_batch ||
        !ResultDescriptorVectorsEqual(state.column_descriptors,
                                      published_descriptors) ||
        !ExecutorColumnDescriptorVectorsEqual(
            state.physical_columns, request.physical_output_batch.columns) ||
        state.next_batch_ordinal != request.cursor_batch_ordinal ||
        state.next_row_ordinal != request.cursor_first_row_ordinal) {
      lock.unlock();
      return refuse_cursor(Refusal(
          "cursor continuation identity, descriptor, or sequence drifted"));
    }

    publish_metadata = !state.metadata_delivered;
    delivery_batch_ordinal = state.next_batch_ordinal;
    delivery_first_row_ordinal = state.next_row_ordinal;
    if (cursor_cancelled) {
      visible_batch.rows.clear();
      result.envelope.diagnostics = request.diagnostics;
      result.envelope.diagnostics.push_back(state.cancellation_diagnostic);
      const auto cancellation_diagnostics =
          ValidateDiagnostics(result.envelope.diagnostics);
      if (!cancellation_diagnostics.ok) {
        lock.unlock();
        return refuse_cursor(cancellation_diagnostics);
      }
      release_cursor = true;
      release_reason = CanonicalResultCursorReleaseReason::kCancelled;
    } else {
      result.envelope.diagnostics = request.diagnostics;
      state.next_row_ordinal +=
          static_cast<std::uint64_t>(visible_batch.rows.size());
      if (!visible_batch.rows.empty()) ++state.next_batch_ordinal;
      if (*request.cursor_state == CanonicalResultCursorState::kClosed) {
        release_cursor = true;
        release_reason = terminal_diagnostic
                             ? CanonicalResultCursorReleaseReason::kError
                             : CanonicalResultCursorReleaseReason::kCompleted;
      }
    }
    state.metadata_delivered = true;

    result.cursor_session = cursor_session;
    result.cursor_uuid = state.cursor_uuid;
    result.cursor_batch_ordinal = delivery_batch_ordinal;
    result.cursor_first_row_ordinal = delivery_first_row_ordinal;
    result.cursor_next_batch_ordinal = state.next_batch_ordinal;
    result.cursor_next_row_ordinal = state.next_row_ordinal;
    result.cursor_metadata_delivered = publish_metadata;
    result.cursor_end_of_stream = release_cursor;
    lock.unlock();
  } else {
    result.envelope.diagnostics = request.diagnostics;
  }

  result.envelope.abi_version = 1;
  result.envelope.statement_uuid = request.statement_uuid;
  result.envelope.mga_statement_context = statement_context;
  result.envelope.catalog_epoch_uuid = request.selected_catalog_epoch_uuid;
  result.envelope.execution_attempt_uuid = request.execution_attempt_uuid;
  result.envelope.result_kind = request.result_kind;
  result.envelope.column_descriptors = published_descriptors;
  result.envelope.row_stream_format_id = request.row_stream_format_id;
  if (request.result_kind == CanonicalResultKind::kRows || empty ||
      request.result_kind == CanonicalResultKind::kExplain) {
    result.envelope.row_count =
        static_cast<std::uint64_t>(visible_batch.rows.size());
  }
  result.envelope.command_tag = request.command_tag;
  result.envelope.cursor_state = cursor_cancelled
                                     ? CanonicalResultCursorState::kClosed
                                     : request.cursor_state;
  result.row_stream = std::move(visible_batch);
  result.canonical_envelope_bytes = EncodeEnvelope(result.envelope);

  if (publish_metadata) {
    result.delivery_records.push_back(
        {CanonicalResultDeliveryKind::kMetadata, std::nullopt, std::nullopt});
  }
  const auto append_batch = [&](const std::uint64_t batch_ordinal,
                                const std::uint64_t first_row_ordinal,
                                const std::size_t row_count) {
    result.delivery_batches.push_back(
        {batch_ordinal, first_row_ordinal, row_count});
    for (std::size_t row = 0; row < row_count; ++row) {
      result.delivery_records.push_back(
          {CanonicalResultDeliveryKind::kRow,
           static_cast<std::size_t>(first_row_ordinal + row), batch_ordinal});
    }
  };
  if (cursor) {
    if (!result.row_stream.rows.empty()) {
      append_batch(delivery_batch_ordinal, delivery_first_row_ordinal,
                   result.row_stream.rows.size());
    }
  } else {
    std::size_t local_first_row = 0;
    std::uint64_t batch_ordinal = 0;
    while (local_first_row < result.row_stream.rows.size()) {
      const auto row_count = std::min(
          request.maximum_rows_per_batch,
          result.row_stream.rows.size() - local_first_row);
      append_batch(batch_ordinal,
                   static_cast<std::uint64_t>(local_first_row),
                   row_count);
      local_first_row += row_count;
      ++batch_ordinal;
    }
  }
  if (!result.envelope.diagnostics.empty()) {
    result.delivery_records.push_back(
        {CanonicalResultDeliveryKind::kDiagnostics, std::nullopt,
         std::nullopt});
  }
  if (release_cursor) {
    if (!cursor_session->Release(release_reason)) {
      result = {};
      result.diagnostic =
          Refusal("cursor resource release callback raised an exception");
      return result;
    }
    result.cursor_resource_released = true;
    result.cursor_release_reason = release_reason;
    result.delivery_records.push_back(
        {CanonicalResultDeliveryKind::kResourceRelease, std::nullopt,
         delivery_batch_ordinal});
  }
  result.diagnostic = {};
  result.published = true;
  return result;
}

}  // namespace scratchbird::engine::executor
