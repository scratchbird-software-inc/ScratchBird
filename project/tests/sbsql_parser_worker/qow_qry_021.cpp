// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-021-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid,
                                 const std::string& canonical_type,
                                 const std::string& nullability,
                                 const std::string& extra = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=" + nullability + extra;
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

api::EngineTypedValue SqlNull(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.setState(api::EngineValueState::sql_null);
  return value;
}

exec::CanonicalResultPublicationRequest RowsRequest() {
  const auto id = Descriptor(
      "019f0000-0000-7200-8000-000000002101",
      "019f0000-0000-7300-8000-000000002101", "int64", "non_null");
  const auto hidden = Descriptor(
      "019f0000-0000-7200-8000-000000002102",
      "019f0000-0000-7300-8000-000000002102", "text", "non_null");
  const auto label = Descriptor(
      "019f0000-0000-7200-8000-000000002103",
      "019f0000-0000-7300-8000-000000002103", "text", "nullable",
      ";collation_uuid=019f0000-0000-7400-8000-000000002103");

  exec::CanonicalResultPublicationRequest request;
  request.statement_uuid = "019f0000-0000-7100-8000-000000002101";
  request.execution_attempt_uuid =
      "019f0000-0000-7110-8000-000000002101";
  request.transaction_effect_evidence_uuid =
      "019f0000-0000-7120-8000-000000002101";
  request.result_kind = exec::CanonicalResultKind::kRows;
  request.physical_output_batch = exec::MakeDescriptorBatch(
      {{"dup", id, false, 2101},
       {"internal_sort_key", hidden, false, 2102},
       {"dup", label, true, 2103}},
      {{{Value(id, "1"), Value(hidden, "z"), Value(label, "alpha")}},
       {{Value(id, "2"), Value(hidden, "y"), SqlNull(label)}}});
  request.column_bindings = {
      {0,
       true,
       exec::CanonicalResultColumnDescriptor{
           0, "dup", id.descriptor_uuid.canonical,
           "019f0000-0000-7300-8000-000000002101",
           exec::CanonicalResultNullability::kNonNull, std::nullopt,
           std::nullopt}},
      {1, false, std::nullopt},
      {2,
       true,
       exec::CanonicalResultColumnDescriptor{
           1, "dup", label.descriptor_uuid.canonical,
           "019f0000-0000-7300-8000-000000002103",
           exec::CanonicalResultNullability::kNullable,
           "019f0000-0000-7400-8000-000000002103", std::nullopt}},
  };
  return request;
}

bool ValidateRowsEmptyCursorAndParity() {
  bool passed = true;
  const auto direct = exec::PublishCanonicalResultEnvelope(RowsRequest());
  passed &= Require(
      direct.diagnostic.ok && direct.published &&
          direct.envelope.abi_version == 1 &&
          direct.envelope.result_kind == exec::CanonicalResultKind::kRows &&
          direct.envelope.row_count == 2 &&
          direct.envelope.column_descriptors.size() == 2 &&
          direct.envelope.column_descriptors[0].ordinal == 0 &&
          direct.envelope.column_descriptors[1].ordinal == 1 &&
          direct.envelope.column_descriptors[0].name_utf8 == "dup" &&
          direct.envelope.column_descriptors[1].name_utf8 == "dup",
      "row result metadata, row count, or duplicate names differ");
  passed &= Require(
      direct.row_stream.columns.size() == 2 &&
          direct.row_stream.rows.size() == 2 &&
          direct.row_stream.rows[0].values[0].encoded_value == "1" &&
          direct.row_stream.rows[0].values[1].encoded_value == "alpha" &&
          direct.row_stream.rows[1].values[1].state ==
              api::EngineValueState::sql_null &&
          direct.row_stream.rows[1].values[1].encoded_value.empty() &&
          direct.delivery_records.size() == 3 &&
          direct.delivery_records.front().kind ==
              exec::CanonicalResultDeliveryKind::kMetadata &&
          direct.delivery_records[1].kind ==
              exec::CanonicalResultDeliveryKind::kRow,
      "hidden projection, typed NULL, or metadata-before-row delivery differs");
  passed &= Require(
      direct.canonical_envelope_bytes.find("internal_sort_key") ==
              std::string::npos &&
          direct.canonical_envelope_bytes.find(
              "QOW-RESULT-DIAGNOSTIC-ABI-V1") != std::string::npos,
      "canonical envelope leaked a hidden column or lost its ABI identity");

  auto prepared_request = RowsRequest();
  prepared_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kPrepared;
  const auto prepared =
      exec::PublishCanonicalResultEnvelope(prepared_request);
  passed &= Require(
      prepared.diagnostic.ok && prepared.published &&
          prepared.canonical_envelope_bytes == direct.canonical_envelope_bytes &&
          prepared.row_stream.rows.size() == direct.row_stream.rows.size(),
      "prepared execution changed canonical result bytes or typed rows");

  auto empty_request = RowsRequest();
  empty_request.result_kind = exec::CanonicalResultKind::kEmpty;
  empty_request.physical_output_batch.rows.clear();
  const auto empty = exec::PublishCanonicalResultEnvelope(empty_request);
  passed &= Require(
      empty.diagnostic.ok && empty.published && empty.envelope.row_count == 0 &&
          empty.envelope.column_descriptors.size() == 2 &&
          empty.row_stream.rows.empty() &&
          empty.delivery_records.size() == 1 &&
          empty.delivery_records.front().kind ==
              exec::CanonicalResultDeliveryKind::kMetadata,
      "empty result lost metadata or published a row");

  auto cursor_request = RowsRequest();
  cursor_request.result_kind = exec::CanonicalResultKind::kCursor;
  cursor_request.cursor_state = exec::CanonicalResultCursorState::kOpen;
  const auto cursor = exec::PublishCanonicalResultEnvelope(cursor_request);
  passed &= Require(
      cursor.diagnostic.ok && cursor.published &&
          !cursor.envelope.row_count.has_value() &&
          cursor.envelope.cursor_state ==
              exec::CanonicalResultCursorState::kOpen &&
          cursor.row_stream.rows.size() == 2,
      "cursor result lost state, metadata, or its typed row chunk");

  auto command_request = RowsRequest();
  command_request.result_kind = exec::CanonicalResultKind::kCommand;
  command_request.physical_output_batch = {};
  command_request.column_bindings.clear();
  command_request.command_tag = "UPDATE 2";
  const auto command = exec::PublishCanonicalResultEnvelope(command_request);
  passed &= Require(
      command.diagnostic.ok && command.published &&
          command.envelope.command_tag == "UPDATE 2" &&
          !command.envelope.row_count.has_value() &&
          command.envelope.column_descriptors.empty(),
      "command result did not use the shared envelope fields");
  return passed;
}

bool ValidateDiagnosticAndCancellation() {
  auto request = RowsRequest();
  const auto argument_descriptor =
      request.physical_output_batch.columns.front().descriptor;
  request.result_kind = exec::CanonicalResultKind::kEmpty;
  request.physical_output_batch = {};
  request.column_bindings.clear();
  request.diagnostics = {{
      "QOW-DIAGNOSTIC-INSTANCE-2101",
      "SB_EXECUTION_CANCELLED",
      exec::CanonicalResultDiagnosticSeverity::kError,
      "57014",
      "query.execution.cancelled",
      {Value(argument_descriptor, "17")},
      exec::CanonicalResultDiagnosticPhase::kExecute,
      "physical.nodes[0]",
      "cancellation_probe",
      2101,
      exec::CanonicalResultTransactionEffect::
          kStatementFailedTransactionUsable,
      exec::CanonicalResultRetryability::kNotRetryable,
  }};
  const auto result = exec::PublishCanonicalResultEnvelope(request);
  bool passed = true;
  passed &= Require(
      result.diagnostic.ok && result.published &&
          result.envelope.diagnostics.size() == 1 &&
          result.envelope.diagnostics.front().transaction_effect ==
              exec::CanonicalResultTransactionEffect::
                  kStatementFailedTransactionUsable &&
          result.envelope.row_count == 0 && result.row_stream.rows.empty() &&
          result.delivery_records.size() == 2 &&
          result.delivery_records[0].kind ==
              exec::CanonicalResultDeliveryKind::kMetadata &&
          result.delivery_records[1].kind ==
              exec::CanonicalResultDeliveryKind::kDiagnostics,
      "cancellation diagnostic or engine transaction effect was not preserved");
  passed &= Require(
      result.canonical_envelope_bytes.find("query.execution.cancelled") !=
              std::string::npos &&
          result.canonical_envelope_bytes.find("statement_failed_transaction_usable") !=
              std::string::npos,
      "diagnostic fields were omitted from canonical bytes");
  return passed;
}

bool RefusedAtomically(const exec::CanonicalResultPublicationRequest& request) {
  const auto result = exec::PublishCanonicalResultEnvelope(request);
  return !result.diagnostic.ok && !result.published &&
         result.diagnostic.diagnostic_code ==
             "QOW-DIAG-QRY-021-REFUSAL-V1" &&
         result.envelope.statement_uuid.empty() &&
         result.row_stream.columns.empty() && result.row_stream.rows.empty() &&
         result.delivery_records.empty() && result.canonical_envelope_bytes.empty();
}

bool ValidateAtomicRefusals() {
  bool passed = true;
  auto request = RowsRequest();
  request.abi_version = 2;
  passed &= Require(RefusedAtomically(request),
                    "unknown ABI version was published");

  request = RowsRequest();
  request.transaction_effect_evidence_uuid.clear();
  passed &= Require(RefusedAtomically(request),
                    "missing engine transaction-effect evidence was accepted");

  request = RowsRequest();
  request.column_bindings[1].published_descriptor =
      exec::CanonicalResultColumnDescriptor{};
  passed &= Require(RefusedAtomically(request),
                    "hidden column published descriptor metadata");

  request = RowsRequest();
  request.column_bindings[2].published_descriptor->type_uuid =
      "019f0000-0000-7300-8000-000000002199";
  passed &= Require(RefusedAtomically(request),
                    "result descriptor drifted from physical authority");

  request = RowsRequest();
  request.physical_output_batch.rows[1].values[2].setState(
      api::EngineValueState::missing);
  passed &= Require(RefusedAtomically(request),
                    "malformed later row published partial metadata or rows");

  request = RowsRequest();
  request.maximum_row_count = 1;
  passed &= Require(RefusedAtomically(request),
                    "result publication ignored the row bound");

  request = RowsRequest();
  request.result_kind = exec::CanonicalResultKind::kCursor;
  passed &= Require(RefusedAtomically(request),
                    "cursor without state was published");

  request = RowsRequest();
  request.diagnostics = {{
      "QOW-DIAGNOSTIC-INSTANCE-2199",
      "SB_BAD_DIAGNOSTIC",
      static_cast<exec::CanonicalResultDiagnosticSeverity>(255),
      std::nullopt,
      "bad.diagnostic",
      {},
      exec::CanonicalResultDiagnosticPhase::kExecute,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      exec::CanonicalResultTransactionEffect::kUnchanged,
      exec::CanonicalResultRetryability::kNotRetryable,
  }};
  passed &= Require(RefusedAtomically(request),
                    "unknown diagnostic enum was published");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-021-V1
// QOW-TEST-IAS-010-V1
int main() {
  bool passed = true;
  passed &= ValidateRowsEmptyCursorAndParity();
  passed &= ValidateDiagnosticAndCancellation();
  passed &= ValidateAtomicRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
