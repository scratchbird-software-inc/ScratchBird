// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "narrow_query_typed_result_publication.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <tuple>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kInvalidArgument = "SB_ENGINE_STATUS_INVALID_ARGUMENT";
constexpr const char* kProfileUnavailable =
    "PROFILE.BUILTIN_PROFILE_UNAVAILABLE";
constexpr const char* kOperandInvalid = "SBLR.OPERAND.INVALID";
constexpr const char* kAccessDenied = "SECURITY.ACCESS_DENIED";
constexpr const char* kTransactionInvalid = "MGA.TRANSACTION.INVALID";
constexpr const char* kTransactionStale = "MGA.TRANSACTION.STALE";
constexpr const char* kDatatypeInvalid = "DATATYPE.DESCRIPTOR_INVALID";
constexpr const char* kProjectionOutputInvalid =
    "PROJECTION.OUTPUT_ROWSET.INVALID";
constexpr const char* kResultShapeInvalid = "RESULT_SET.SHAPE_INVALID";
constexpr const char* kResourceExceeded = "RESOURCE.BUDGET_EXCEEDED";
constexpr const char* kCancelled = "PROCESS.CANCELLED";
constexpr const char* kExecutionFailed = "SBLR.EXECUTION_FAILED";

using OutputKey = std::pair<wire::NarrowQueryUuid, u64>;
using SourceColumnKey =
    std::tuple<wire::NarrowQueryUuid, u64, wire::NarrowQueryUuid,
               std::uint32_t>;

template <typename Result>
Result Refuse(NarrowQueryTypedResultPublicationStatusV1 status,
              std::string diagnostic_code,
              std::string detail) {
  Result result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic_code);
  result.detail = std::move(detail);
  return result;
}

template <typename Result>
Result Success() {
  Result result;
  result.status = NarrowQueryTypedResultPublicationStatusV1::ok;
  return result;
}

bool UuidPresent(const wire::NarrowQueryUuid& uuid) {
  return std::any_of(uuid.begin(), uuid.end(),
                     [](byte octet) { return octet != 0; });
}

bool HashPresent(const wire::NarrowQueryHash& hash) {
  return std::any_of(hash.begin(), hash.end(),
                     [](byte octet) { return octet != 0; });
}

bool ValidProfileCode(u16 code) {
  return code ==
             static_cast<u16>(wire::NarrowQueryProfile::ordered_projection) ||
         code == static_cast<u16>(
                     wire::NarrowQueryProfile::projection_occurrence) ||
         code == static_cast<u16>(
                     wire::NarrowQueryProfile::alias_distinct_self_join);
}

bool ValidValueState(wire::TypedResultValueState state) {
  return state == wire::TypedResultValueState::value_present ||
         state == wire::TypedResultValueState::sql_null;
}

bool QueryHandlePresent(const wire::TypedResultQueryHandleV1& handle) {
  return UuidPresent(handle.execution_uuid) &&
         UuidPresent(handle.result_set_uuid) &&
         UuidPresent(handle.row_descriptor_uuid) &&
         UuidPresent(handle.snapshot_uuid);
}

bool SameQueryHandle(const wire::TypedResultQueryHandleV1& left,
                     const wire::TypedResultQueryHandleV1& right) {
  return left.execution_uuid == right.execution_uuid &&
         left.result_set_uuid == right.result_set_uuid &&
         left.row_descriptor_uuid == right.row_descriptor_uuid &&
         left.snapshot_uuid == right.snapshot_uuid;
}

NarrowQueryTypedResultPublicationStatusV1 StatusForDiagnostic(
    std::string_view diagnostic,
    NarrowQueryTypedResultPublicationStatusV1 fallback) {
  if (diagnostic == kProfileUnavailable) {
    return NarrowQueryTypedResultPublicationStatusV1::profile_unavailable;
  }
  if (diagnostic == kTransactionInvalid ||
      diagnostic == kTransactionStale || diagnostic == kAccessDenied) {
    return NarrowQueryTypedResultPublicationStatusV1::result_receipt_mismatch;
  }
  if (diagnostic == kDatatypeInvalid) {
    return NarrowQueryTypedResultPublicationStatusV1::descriptor_invalid;
  }
  if (diagnostic == kProjectionOutputInvalid ||
      diagnostic == kResultShapeInvalid) {
    return NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid;
  }
  if (diagnostic == kResourceExceeded ||
      diagnostic == "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED") {
    return NarrowQueryTypedResultPublicationStatusV1::resource_budget_exceeded;
  }
  if (diagnostic == kCancelled) {
    return NarrowQueryTypedResultPublicationStatusV1::cancelled;
  }
  return fallback;
}

bool CanonicalPublicationDiagnostic(std::string_view diagnostic) {
  return diagnostic == kAccessDenied || diagnostic == kTransactionInvalid ||
         diagnostic == kTransactionStale || diagnostic == kDatatypeInvalid ||
         diagnostic == kProjectionOutputInvalid ||
         diagnostic == kResultShapeInvalid ||
         diagnostic == kResourceExceeded || diagnostic == kCancelled ||
         diagnostic == kExecutionFailed;
}

NarrowQueryTypedResultPublicationAuthorityDecisionV1 ObserveAuthority(
    const NarrowQueryTypedResultPublicationAuthorityValidatorV1& authority,
    const NarrowQueryTypedResultPublicationAuthorityRequestV1& request) {
  NarrowQueryTypedResultPublicationAuthorityDecisionV1 decision;
  if (!authority) {
    decision.detail = "publication_authority_validator_required";
    return decision;
  }
  try {
    decision = authority(request);
  } catch (...) {
    decision.accepted = false;
    decision.detail = "publication_authority_exception";
  }
  if (!decision.accepted &&
      !CanonicalPublicationDiagnostic(decision.diagnostic_code)) {
    decision.diagnostic_code = kExecutionFailed;
    if (decision.detail.empty()) {
      decision.detail =
          "publication_authority_refused_without_canonical_diagnostic";
    }
  }
  return decision;
}

wire::TypedResultDescriptorAuthorityDecision FrozenDescriptorDecision(
    const std::vector<byte>& frozen_vector,
    const wire::TypedResultDescriptorAuthorityValidator& authority,
    const wire::TypedResultRowDescriptor& descriptor) {
  auto encoded = wire::EncodeTypedResultRowDescriptor(descriptor);
  if (!encoded.ok() || encoded.encoded != frozen_vector) {
    wire::TypedResultDescriptorAuthorityDecision decision;
    decision.diagnostic_code = kDatatypeInvalid;
    decision.detail = "immutable_result_descriptor_vector_mismatch";
    return decision;
  }
  if (!authority) {
    wire::TypedResultDescriptorAuthorityDecision decision;
    decision.diagnostic_code = kDatatypeInvalid;
    decision.detail = "descriptor_authority_validator_required";
    return decision;
  }
  try {
    auto decision = authority(encoded.descriptor);
    if (!decision.accepted && decision.diagnostic_code.empty()) {
      decision.diagnostic_code = kDatatypeInvalid;
    }
    if (!decision.accepted && decision.detail.empty()) {
      decision.detail = "descriptor_authority_refused";
    }
    return decision;
  } catch (...) {
    wire::TypedResultDescriptorAuthorityDecision decision;
    decision.diagnostic_code = kDatatypeInvalid;
    decision.detail = "descriptor_authority_exception";
    return decision;
  }
}

bool DescriptorMatchesOutputs(
    const wire::TypedResultRowDescriptor& descriptor,
    const wire::NarrowQueryBinding& binding) {
  if (descriptor.descriptor_uuid != binding.row_descriptor_uuid ||
      descriptor.descriptor_generation != binding.row_descriptor_generation ||
      descriptor.datatype_catalog_snapshot_uuid !=
          binding.datatype_catalog_snapshot_uuid ||
      descriptor.datatype_catalog_generation !=
          binding.datatype_catalog_generation ||
      descriptor.datatype_registry_generation !=
          binding.datatype_registry_generation ||
      descriptor.columns.size() != binding.outputs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < binding.outputs.size(); ++index) {
    const auto& output = binding.outputs[index];
    const auto& column = descriptor.columns[index];
    if (column.ordinal != output.output_ordinal ||
        column.name_occurrence != output.name_occurrence ||
        column.name != output.name ||
        static_cast<std::uint8_t>(column.nullability) != output.nullability ||
        column.descriptor_uuid != output.datatype_descriptor_uuid ||
        column.descriptor_generation !=
            output.datatype_descriptor_generation ||
        column.type_uuid != output.datatype_type_uuid ||
        column.type_generation != output.datatype_type_generation ||
        static_cast<std::uint32_t>(column.canonical_type_id) !=
            output.datatype_binary_type_code ||
        column.codec_id != output.codec_id ||
        column.codec_version != output.codec_version ||
        column.codec_generation != output.codec_generation ||
        column.canonical_value_bytes != output.canonical_value_bytes ||
        output.null_encoding != 1) {
      return false;
    }
  }
  return true;
}

wire::TypedResultRowDescriptor DescriptorFromBinding(
    const wire::NarrowQueryBinding& binding) {
  wire::TypedResultRowDescriptor descriptor;
  descriptor.descriptor_uuid = binding.row_descriptor_uuid;
  descriptor.descriptor_generation = binding.row_descriptor_generation;
  descriptor.datatype_catalog_snapshot_uuid =
      binding.datatype_catalog_snapshot_uuid;
  descriptor.datatype_catalog_generation =
      binding.datatype_catalog_generation;
  descriptor.datatype_registry_generation =
      binding.datatype_registry_generation;
  descriptor.columns.reserve(binding.outputs.size());
  for (const auto& output : binding.outputs) {
    wire::TypedResultColumnDescriptor column;
    column.ordinal = output.output_ordinal;
    column.name_occurrence = output.name_occurrence;
    column.name = output.name;
    column.nullability =
        static_cast<wire::TypedResultNullability>(output.nullability);
    column.descriptor_uuid = output.datatype_descriptor_uuid;
    column.descriptor_generation = output.datatype_descriptor_generation;
    column.type_uuid = output.datatype_type_uuid;
    column.type_generation = output.datatype_type_generation;
    column.canonical_type_id =
        static_cast<scratchbird::core::datatypes::CanonicalTypeId>(
            output.datatype_binary_type_code);
    column.codec_id = output.codec_id;
    column.codec_version = output.codec_version;
    column.codec_generation = output.codec_generation;
    column.canonical_value_bytes = output.canonical_value_bytes;
    descriptor.columns.push_back(std::move(column));
  }
  return descriptor;
}

template <typename Result>
bool CheckFrozenReceipt(
    const NarrowQueryTypedResultPublicationBindingV1& binding,
    const NarrowQueryTypedResultPublicationReceiptV1& supplied,
    Result* refusal) {
  const auto& frozen = binding.receipt();
  if (supplied.version != kNarrowQueryTypedResultPublicationVersionV1) {
    *refusal = Refuse<Result>(
        NarrowQueryTypedResultPublicationStatusV1::invalid_argument,
        kInvalidArgument, "publication_receipt_version_invalid");
    return false;
  }
  if (!ValidProfileCode(supplied.profile_code) ||
      supplied.profile_code != frozen.profile_code) {
    *refusal = Refuse<Result>(
        NarrowQueryTypedResultPublicationStatusV1::profile_unavailable,
        kProfileUnavailable, "publication_profile_receipt_mismatch");
    return false;
  }
  if (supplied.statement_receipt_uuid != frozen.statement_receipt_uuid) {
    *refusal = Refuse<Result>(
        NarrowQueryTypedResultPublicationStatusV1::result_receipt_mismatch,
        kTransactionStale, "publication_statement_receipt_mismatch");
    return false;
  }
  if (!SameQueryHandle(supplied.query_handle, frozen.query_handle) ||
      supplied.row_descriptor_generation !=
          frozen.row_descriptor_generation ||
      supplied.profile_binding_evidence_sha256 !=
          frozen.profile_binding_evidence_sha256) {
    *refusal = Refuse<Result>(
        NarrowQueryTypedResultPublicationStatusV1::result_receipt_mismatch,
        kResultShapeInvalid, "publication_result_handle_or_evidence_mismatch");
    return false;
  }
  return true;
}

class OccurrenceProducerAdapter final : public TypedResultProducerSourceV1 {
 public:
  OccurrenceProducerAdapter(
      std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1> binding,
      std::unique_ptr<NarrowQueryTypedResultOccurrenceSourceV1> source)
      : binding_(std::move(binding)), source_(std::move(source)) {}

  TypedResultProducerStageResultV1 Stage(
      const TypedResultProducerStageRequestV1& request) override {
    auto staged = source_->Stage(request);
    TypedResultProducerStageResultV1 result;
    result.outcome = staged.outcome;
    result.end_of_cursor = staged.end_of_cursor;
    result.detail = std::move(staged.detail);
    result.lease = std::move(staged.lease);

    if (staged.outcome != TypedResultProducerStageOutcomeV1::batch) {
      if (!staged.rows.empty()) {
        result.outcome = TypedResultProducerStageOutcomeV1::refused;
        result.end_of_cursor = false;
        result.detail =
            "SBLR.EXECUTION_FAILED:nonbatch_occurrence_stage_has_rows";
        result.lease.Abort();
      }
      return result;
    }
    if (staged.rows.empty()) {
      result.outcome = TypedResultProducerStageOutcomeV1::refused;
      result.end_of_cursor = false;
      result.detail = "RESULT_SET.SHAPE_INVALID:empty_batch_stage";
      result.lease.Abort();
      return result;
    }

    result.rows.reserve(staged.rows.size());
    for (std::size_t index = 0; index < staged.rows.size(); ++index) {
      if (staged.rows[index].row_ordinal != index) {
        result.rows.clear();
        result.outcome = TypedResultProducerStageOutcomeV1::refused;
        result.end_of_cursor = false;
        result.detail = "RESULT_SET.SHAPE_INVALID:row_ordinal_not_dense";
        result.lease.Abort();
        return result;
      }
      auto materialized = MaterializeNarrowQueryTypedResultRowV1(
          *binding_, staged.rows[index]);
      if (!materialized.ok()) {
        result.rows.clear();
        result.outcome = TypedResultProducerStageOutcomeV1::refused;
        result.end_of_cursor = false;
        result.detail = materialized.diagnostic_code + ":" +
                        materialized.detail;
        result.lease.Abort();
        return result;
      }
      result.rows.push_back(std::move(materialized.row));
    }
    return result;
  }

  void Close(TypedResultProducerReleaseReasonV1 reason) noexcept override {
    source_->Close(reason);
  }

 private:
  std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1> binding_;
  std::unique_ptr<NarrowQueryTypedResultOccurrenceSourceV1> source_;
};

}  // namespace

struct NarrowQueryTypedResultPublicationBindingV1::State {
  wire::NarrowQueryBinding profile_binding;
  std::vector<byte> canonical_profile_binding;
  NarrowQueryTypedResultPublicationReceiptV1 receipt;
  wire::TypedResultRowDescriptor row_descriptor;
  std::vector<byte> result_descriptor_vector;
  wire::TypedResultDescriptorAuthorityValidator descriptor_authority;
  std::map<OutputKey, std::size_t> output_by_occurrence;
};

NarrowQueryTypedResultPublicationBindingV1::
    NarrowQueryTypedResultPublicationBindingV1(
        std::shared_ptr<const State> state)
    : state_(std::move(state)) {}

wire::NarrowQueryProfile
NarrowQueryTypedResultPublicationBindingV1::profile() const {
  return state_->profile_binding.profile;
}

const NarrowQueryTypedResultPublicationReceiptV1&
NarrowQueryTypedResultPublicationBindingV1::receipt() const {
  return state_->receipt;
}

const std::vector<byte>&
NarrowQueryTypedResultPublicationBindingV1::canonical_profile_binding() const {
  return state_->canonical_profile_binding;
}

const std::vector<byte>&
NarrowQueryTypedResultPublicationBindingV1::result_descriptor_vector() const {
  return state_->result_descriptor_vector;
}

const wire::TypedResultRowDescriptor&
NarrowQueryTypedResultPublicationBindingV1::row_descriptor() const {
  return state_->row_descriptor;
}

NarrowQueryTypedResultPrepareResultV1
PrepareNarrowQueryTypedResultPublicationV1(
    const NarrowQueryTypedResultPrepareRequestV1& request) {
  if (request.version != kNarrowQueryTypedResultPublicationVersionV1 ||
      request.expected_receipt.version !=
          kNarrowQueryTypedResultPublicationVersionV1 ||
      request.canonical_profile_binding.empty() ||
      !ValidProfileCode(request.expected_receipt.profile_code) ||
      !UuidPresent(request.expected_receipt.statement_receipt_uuid) ||
      !QueryHandlePresent(request.expected_receipt.query_handle) ||
      request.expected_receipt.row_descriptor_generation == 0 ||
      !request.descriptor_authority) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::invalid_argument,
        kInvalidArgument, "malformed_typed_result_prepare_request");
  }

  wire::NarrowQueryBinding decoded;
  wire::NarrowQueryBindingError binding_error;
  if (!wire::DecodeAndValidateNarrowQueryBinding(
          request.canonical_profile_binding, request.validation_context,
          &decoded, &binding_error)) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        StatusForDiagnostic(
            binding_error.diagnostic_code,
            NarrowQueryTypedResultPublicationStatusV1::binding_invalid),
        binding_error.diagnostic_code.empty() ? kOperandInvalid
                                              : binding_error.diagnostic_code,
        binding_error.field + ":" + binding_error.detail);
  }

  if (static_cast<u16>(decoded.profile) !=
      request.expected_receipt.profile_code) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::profile_unavailable,
        kProfileUnavailable, "selected_profile_does_not_match_live_receipt");
  }
  if (decoded.statement_receipt_uuid !=
      request.expected_receipt.statement_receipt_uuid) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::result_receipt_mismatch,
        kTransactionStale,
        "binding_statement_receipt_does_not_match_result_receipt");
  }
  wire::TypedResultQueryHandleV1 decoded_handle;
  decoded_handle.execution_uuid = decoded.execution_uuid;
  decoded_handle.result_set_uuid = decoded.result_set_uuid;
  decoded_handle.row_descriptor_uuid = decoded.row_descriptor_uuid;
  decoded_handle.snapshot_uuid = decoded.statement_snapshot_uuid;
  if (!SameQueryHandle(decoded_handle,
                       request.expected_receipt.query_handle) ||
      decoded.row_descriptor_generation !=
          request.expected_receipt.row_descriptor_generation) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::result_receipt_mismatch,
        kResultShapeInvalid,
        "binding_query_handle_does_not_match_result_receipt");
  }

  std::vector<byte> canonical_binding;
  wire::NarrowQueryBindingError encode_error;
  if (!wire::EncodeNarrowQueryBinding(decoded, &canonical_binding,
                                      &encode_error) ||
      canonical_binding.size() != request.canonical_profile_binding.size() ||
      !std::equal(canonical_binding.begin(), canonical_binding.end(),
                  request.canonical_profile_binding.begin())) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::binding_invalid,
        kOperandInvalid, "profile_binding_canonical_roundtrip_failed");
  }

  auto descriptor = DescriptorFromBinding(decoded);
  auto encoded_descriptor = wire::EncodeTypedResultRowDescriptor(descriptor);
  if (!encoded_descriptor.ok()) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        StatusForDiagnostic(
            encoded_descriptor.diagnostic_code,
            NarrowQueryTypedResultPublicationStatusV1::descriptor_invalid),
        encoded_descriptor.diagnostic_code.empty()
            ? kDatatypeInvalid
            : encoded_descriptor.diagnostic_code,
        "result_descriptor_encode:" + encoded_descriptor.detail);
  }
  auto verified_descriptor = wire::DecodeTypedResultRowDescriptor(
      encoded_descriptor.encoded);
  if (!verified_descriptor.ok() ||
      verified_descriptor.encoded != encoded_descriptor.encoded ||
      !DescriptorMatchesOutputs(verified_descriptor.descriptor, decoded)) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::descriptor_invalid,
        kDatatypeInvalid,
        "result_descriptor_output_vector_byte_comparison_failed");
  }

  auto descriptor_decision = FrozenDescriptorDecision(
      verified_descriptor.encoded, request.descriptor_authority,
      verified_descriptor.descriptor);
  if (!descriptor_decision.accepted) {
    return Refuse<NarrowQueryTypedResultPrepareResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::descriptor_invalid,
        descriptor_decision.diagnostic_code.empty()
            ? kDatatypeInvalid
            : descriptor_decision.diagnostic_code,
        descriptor_decision.detail.empty()
            ? "descriptor_authority_refused"
            : descriptor_decision.detail);
  }

  auto state = std::make_shared<NarrowQueryTypedResultPublicationBindingV1::
                                    State>();
  state->profile_binding = std::move(decoded);
  state->canonical_profile_binding = std::move(canonical_binding);
  state->row_descriptor = std::move(verified_descriptor.descriptor);
  state->result_descriptor_vector = std::move(verified_descriptor.encoded);
  state->descriptor_authority = request.descriptor_authority;
  state->receipt.profile_code =
      static_cast<u16>(state->profile_binding.profile);
  state->receipt.statement_receipt_uuid =
      state->profile_binding.statement_receipt_uuid;
  state->receipt.query_handle = decoded_handle;
  state->receipt.row_descriptor_generation =
      state->profile_binding.row_descriptor_generation;
  state->receipt.profile_binding_evidence_sha256 =
      state->profile_binding.descriptor_evidence_sha256;
  for (std::size_t index = 0;
       index < state->profile_binding.outputs.size(); ++index) {
    const auto& output = state->profile_binding.outputs[index];
    if (!state->output_by_occurrence
             .emplace(OutputKey{output.output_occurrence_uuid,
                                output.output_occurrence_generation},
                      index)
             .second) {
      return Refuse<NarrowQueryTypedResultPrepareResultV1>(
          NarrowQueryTypedResultPublicationStatusV1::binding_invalid,
          kProjectionOutputInvalid,
          "duplicate_output_occurrence_after_binding_validation");
    }
  }

  auto result = Success<NarrowQueryTypedResultPrepareResultV1>();
  result.binding =
      std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1>(
          new NarrowQueryTypedResultPublicationBindingV1(std::move(state)));
  return result;
}

NarrowQueryTypedResultMaterializeResultV1
MaterializeNarrowQueryTypedResultRowV1(
    const NarrowQueryTypedResultPublicationBindingV1& binding,
    const NarrowQueryTypedResultOccurrenceRowV1& row) {
  const auto& state = *binding.state_;
  if (row.cells.size() != state.profile_binding.outputs.size()) {
    return Refuse<NarrowQueryTypedResultMaterializeResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid,
        kResultShapeInvalid, "occurrence_cell_count_mismatch");
  }

  std::vector<const NarrowQueryTypedResultOccurrenceCellV1*> ordered(
      state.profile_binding.outputs.size(), nullptr);
  for (const auto& supplied : row.cells) {
    if (!UuidPresent(supplied.output_occurrence_uuid) ||
        supplied.output_occurrence_generation == 0 ||
        !ValidValueState(supplied.state)) {
      return Refuse<NarrowQueryTypedResultMaterializeResultV1>(
          NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid,
          kResultShapeInvalid,
          "occurrence_cell_identity_or_value_state_invalid");
    }
    const auto found = state.output_by_occurrence.find(
        OutputKey{supplied.output_occurrence_uuid,
                  supplied.output_occurrence_generation});
    if (found == state.output_by_occurrence.end() ||
        ordered[found->second] != nullptr) {
      return Refuse<NarrowQueryTypedResultMaterializeResultV1>(
          NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid,
          kResultShapeInvalid,
          "unknown_or_duplicate_output_occurrence_cell");
    }
    ordered[found->second] = &supplied;
  }

  wire::TypedResultRow materialized;
  materialized.row_ordinal = row.row_ordinal;
  materialized.cells.reserve(ordered.size());
  std::map<SourceColumnKey, std::size_t> first_source_value;
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    if (ordered[index] == nullptr) {
      return Refuse<NarrowQueryTypedResultMaterializeResultV1>(
          NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid,
          kResultShapeInvalid, "missing_output_occurrence_cell");
    }
    const auto& output = state.profile_binding.outputs[index];
    const auto& supplied = *ordered[index];
    if ((supplied.state == wire::TypedResultValueState::sql_null &&
         (!supplied.canonical_payload.empty() || output.nullability == 0)) ||
        (supplied.state == wire::TypedResultValueState::value_present &&
         output.canonical_value_bytes != 0 &&
         supplied.canonical_payload.size() !=
             output.canonical_value_bytes)) {
      return Refuse<NarrowQueryTypedResultMaterializeResultV1>(
          NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid,
          kDatatypeInvalid,
          "occurrence_cell_nullability_or_fixed_width_mismatch");
    }

    wire::TypedResultCell cell;
    cell.column_ordinal = output.output_ordinal;
    cell.name_occurrence = output.name_occurrence;
    cell.state = supplied.state;
    cell.canonical_payload = supplied.canonical_payload;

    const SourceColumnKey source_key{
        output.source_occurrence_uuid, output.source_occurrence_generation,
        output.source_column_uuid, output.source_column_ordinal};
    const auto [first, inserted] =
        first_source_value.emplace(source_key, materialized.cells.size());
    if (!inserted) {
      const auto& earlier = materialized.cells[first->second];
      if (earlier.state != cell.state ||
          earlier.canonical_payload != cell.canonical_payload) {
        return Refuse<NarrowQueryTypedResultMaterializeResultV1>(
            NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid,
            kProjectionOutputInvalid,
            "repeated_source_cell_value_or_null_state_mismatch");
      }
    }
    materialized.cells.push_back(std::move(cell));
  }

  auto result = Success<NarrowQueryTypedResultMaterializeResultV1>();
  result.row = std::move(materialized);
  return result;
}

NarrowQueryTypedResultDirectResultV1
PublishNarrowQueryTypedResultDirectBatchV1(
    const std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1>&
        binding,
    const NarrowQueryTypedResultDirectRequestV1& request) {
  if (!binding ||
      request.version != kNarrowQueryTypedResultPublicationVersionV1 ||
      !UuidPresent(request.server_request_uuid) ||
      !UuidPresent(request.batch_uuid) || request.rows.empty() ||
      request.rows.size() > wire::kTypedResultCarrierMaximumRows ||
      request.maximum_descriptor_bytes == 0 ||
      request.maximum_descriptor_bytes > wire::kTypedResultCarrierMaximumBytes ||
      request.maximum_row_packet_bytes == 0 ||
      request.maximum_row_packet_bytes > wire::kTypedResultCarrierMaximumBytes ||
      !request.publication_authority) {
    return Refuse<NarrowQueryTypedResultDirectResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::invalid_argument,
        kInvalidArgument, "malformed_direct_publication_request");
  }
  NarrowQueryTypedResultDirectResultV1 receipt_refusal;
  if (!CheckFrozenReceipt(*binding, request.receipt, &receipt_refusal)) {
    return receipt_refusal;
  }

  NarrowQueryTypedResultPublicationAuthorityRequestV1 authority_request;
  authority_request.phase = NarrowQueryTypedResultPublicationPhaseV1::
      before_row_materialization;
  authority_request.receipt = request.receipt;
  authority_request.row_count = request.rows.size();
  auto authority =
      ObserveAuthority(request.publication_authority, authority_request);
  if (!authority.accepted) {
    return Refuse<NarrowQueryTypedResultDirectResultV1>(
        StatusForDiagnostic(
            authority.diagnostic_code,
            NarrowQueryTypedResultPublicationStatusV1::publication_failed),
        authority.diagnostic_code, authority.detail);
  }

  std::vector<wire::TypedResultRow> rows;
  rows.reserve(request.rows.size());
  for (std::size_t index = 0; index < request.rows.size(); ++index) {
    if (request.rows[index].row_ordinal != index) {
      return Refuse<NarrowQueryTypedResultDirectResultV1>(
          NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid,
          kResultShapeInvalid, "direct_batch_row_ordinal_not_dense");
    }
    auto materialized =
        MaterializeNarrowQueryTypedResultRowV1(*binding, request.rows[index]);
    if (!materialized.ok()) {
      return Refuse<NarrowQueryTypedResultDirectResultV1>(
          materialized.status, materialized.diagnostic_code,
          materialized.detail);
    }
    rows.push_back(std::move(materialized.row));
  }

  const auto& state = *binding->state_;
  wire::TypedResultBatch batch;
  batch.execution_uuid = state.receipt.query_handle.execution_uuid;
  batch.result_set_uuid = state.receipt.query_handle.result_set_uuid;
  batch.batch_uuid = request.batch_uuid;
  batch.batch_ordinal = 0;
  batch.end_of_rowset = true;
  batch.cursor_bound = false;
  batch.row_descriptor_uuid = state.row_descriptor.descriptor_uuid;
  batch.row_descriptor_generation =
      state.row_descriptor.descriptor_generation;
  batch.descriptor_evidence_sha256 =
      state.row_descriptor.descriptor_evidence_sha256;
  batch.snapshot_uuid = state.receipt.query_handle.snapshot_uuid;
  batch.rows = std::move(rows);

  wire::TypedResultCarrierBinding carrier_binding;
  carrier_binding.kind = wire::TypedResultCarrierKind::ps_execute_result_v1;
  carrier_binding.row_count = batch.rows.size();
  carrier_binding.end_of_rowset = true;
  carrier_binding.execution_uuid = batch.execution_uuid;
  carrier_binding.result_set_uuid = batch.result_set_uuid;
  carrier_binding.snapshot_uuid = batch.snapshot_uuid;
  auto encoded_batch = wire::EncodeTypedResultBatch(
      batch, state.row_descriptor, carrier_binding);
  if (!encoded_batch.ok()) {
    return Refuse<NarrowQueryTypedResultDirectResultV1>(
        StatusForDiagnostic(
            encoded_batch.diagnostic_code,
            NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid),
        encoded_batch.diagnostic_code.empty() ? kResultShapeInvalid
                                              : encoded_batch.diagnostic_code,
        "direct_row_packet_encode:" + encoded_batch.detail);
  }
  if (state.result_descriptor_vector.size() >
          request.maximum_descriptor_bytes ||
      encoded_batch.encoded.size() > request.maximum_row_packet_bytes) {
    return Refuse<NarrowQueryTypedResultDirectResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::resource_budget_exceeded,
        kResourceExceeded, "direct_typed_result_exceeds_admitted_bound");
  }

  wire::TypedResultExecuteCarrierV1 carrier;
  carrier.outcome = wire::TypedResultExecuteOutcome::row_batch;
  carrier.server_request_uuid = request.server_request_uuid;
  carrier.transaction_uuid = state.profile_binding.owning_transaction_uuid;
  carrier.local_transaction_id =
      state.profile_binding.owning_local_transaction_id;
  carrier.row_count = encoded_batch.batch.rows.size();
  carrier.result_descriptor_vector = state.result_descriptor_vector;
  carrier.row_data_packet = encoded_batch.encoded;
  carrier.query_handle = state.receipt.query_handle;

  const auto frozen_vector = state.result_descriptor_vector;
  const auto descriptor_authority = state.descriptor_authority;
  wire::TypedResultExecuteRequestAuthorityV1 carrier_authority;
  carrier_authority.expected_server_request_uuid = request.server_request_uuid;
  carrier_authority.maximum_descriptor_bytes =
      request.maximum_descriptor_bytes;
  carrier_authority.maximum_row_packet_bytes =
      request.maximum_row_packet_bytes;
  auto verified = wire::ValidateTypedResultExecuteCarrierV1(
      carrier_authority, carrier,
      [frozen_vector, descriptor_authority](const auto& descriptor) {
        return FrozenDescriptorDecision(frozen_vector, descriptor_authority,
                                        descriptor);
      });
  if (!verified.ok() || verified.batch.batch_uuid != request.batch_uuid ||
      verified.batch.rows.size() != request.rows.size()) {
    return Refuse<NarrowQueryTypedResultDirectResultV1>(
        StatusForDiagnostic(
            verified.diagnostic_code,
            NarrowQueryTypedResultPublicationStatusV1::publication_failed),
        verified.diagnostic_code.empty() ? kExecutionFailed
                                         : verified.diagnostic_code,
        verified.detail.empty() ? "direct_carrier_verification_failed"
                                : verified.detail);
  }

  authority_request.phase = NarrowQueryTypedResultPublicationPhaseV1::
      immediately_before_publication_barrier;
  authority_request.encoded_row_packet_bytes = carrier.row_data_packet.size();
  authority = ObserveAuthority(request.publication_authority,
                               authority_request);
  if (!authority.accepted) {
    return Refuse<NarrowQueryTypedResultDirectResultV1>(
        StatusForDiagnostic(
            authority.diagnostic_code,
            NarrowQueryTypedResultPublicationStatusV1::publication_failed),
        authority.diagnostic_code, authority.detail);
  }

  // Sole direct publication barrier: the already-verified descriptor, packet,
  // and receipt become visible together in the returned value.
  auto result = Success<NarrowQueryTypedResultDirectResultV1>();
  result.execute_carrier = std::move(carrier);
  result.batch = std::move(verified.batch);
  return result;
}

NarrowQueryTypedResultCursorOpenResultV1 OpenNarrowQueryTypedResultCursorV1(
    const std::shared_ptr<const NarrowQueryTypedResultPublicationBindingV1>&
        binding,
    NarrowQueryTypedResultCursorOpenRequestV1 request) {
  if (!binding ||
      request.version != kNarrowQueryTypedResultPublicationVersionV1 ||
      !UuidPresent(request.server_request_uuid) ||
      request.maximum_descriptor_bytes == 0 ||
      request.maximum_descriptor_bytes > wire::kTypedResultCarrierMaximumBytes ||
      !request.producer_state) {
    return Refuse<NarrowQueryTypedResultCursorOpenResultV1>(
        NarrowQueryTypedResultPublicationStatusV1::invalid_argument,
        kInvalidArgument, "malformed_cursor_publication_request");
  }
  NarrowQueryTypedResultCursorOpenResultV1 receipt_refusal;
  if (!CheckFrozenReceipt(*binding, request.receipt, &receipt_refusal)) {
    return receipt_refusal;
  }

  const auto& state = *binding->state_;
  const auto frozen_vector = state.result_descriptor_vector;
  const auto descriptor_authority = state.descriptor_authority;
  TypedResultProducerOpenRequestV1 open;
  open.carrier_uuid = request.carrier_uuid;
  open.carrier_generation = request.carrier_generation;
  open.cursor_uuid = request.cursor_uuid;
  open.session_uuid = request.session_uuid;
  open.statement_receipt_uuid = state.receipt.statement_receipt_uuid;
  open.statement_snapshot_uuid = state.receipt.query_handle.snapshot_uuid;
  open.cancellation_receipt_uuid =
      state.profile_binding.cancellation_receipt_uuid;
  open.cancellation_generation =
      state.profile_binding.cancellation_generation;
  open.resource_grant_receipt_uuid =
      state.profile_binding.resource_grant_receipt_uuid;
  open.resource_grant_generation =
      state.profile_binding.resource_grant_generation;
  open.resource_grant_bytes = request.resource_grant_bytes;
  open.execution_uuid = state.receipt.query_handle.execution_uuid;
  open.result_set_uuid = state.receipt.query_handle.result_set_uuid;
  open.row_descriptor_uuid = state.receipt.query_handle.row_descriptor_uuid;
  open.row_descriptor_generation = state.receipt.row_descriptor_generation;
  open.snapshot_uuid = state.receipt.query_handle.snapshot_uuid;
  open.cursor_stream_descriptor_uuid =
      request.cursor_stream_descriptor.descriptor_uuid;
  open.cursor_stream_descriptor_version =
      request.cursor_stream_descriptor.descriptor_version;
  open.cursor_stream_descriptor_generation =
      request.cursor_stream_descriptor.descriptor_generation;
  open.max_chunk_rows = request.cursor_stream_descriptor.maximum_chunk_rows;
  open.max_chunk_bytes = request.cursor_stream_descriptor.maximum_chunk_bytes;
  open.row_descriptor = state.row_descriptor;
  open.descriptor_authority =
      [frozen_vector, descriptor_authority](const auto& descriptor) {
        return FrozenDescriptorDecision(frozen_vector, descriptor_authority,
                                        descriptor);
      };
  open.statement_receipt = std::move(request.statement_receipt);
  open.mga_snapshot_pin = std::move(request.mga_snapshot_pin);
  open.cancellation_receipt = std::move(request.cancellation_receipt);
  open.resource_grant_receipt = std::move(request.resource_grant_receipt);
  open.producer_state = std::make_unique<OccurrenceProducerAdapter>(
      binding, std::move(request.producer_state));

  auto opened = OpenTypedResultProducerCursorV1(std::move(open));
  if (!opened.ok()) {
    return Refuse<NarrowQueryTypedResultCursorOpenResultV1>(
        StatusForDiagnostic(
            opened.diagnostic_code,
            opened.status == TypedResultProducerCursorStatusV1::cancelled
                ? NarrowQueryTypedResultPublicationStatusV1::cancelled
                : NarrowQueryTypedResultPublicationStatusV1::cursor_refused),
        opened.diagnostic_code.empty() ? kExecutionFailed
                                       : opened.diagnostic_code,
        opened.detail);
  }

  wire::TypedResultExecuteCarrierV1 carrier;
  carrier.outcome = wire::TypedResultExecuteOutcome::cursor_open;
  carrier.server_request_uuid = request.server_request_uuid;
  carrier.transaction_uuid = state.profile_binding.owning_transaction_uuid;
  carrier.local_transaction_id =
      state.profile_binding.owning_local_transaction_id;
  carrier.cursor_uuid = request.cursor_uuid;
  carrier.result_descriptor_vector = state.result_descriptor_vector;
  carrier.cursor_stream_descriptor = request.cursor_stream_descriptor;
  carrier.query_handle = state.receipt.query_handle;

  wire::TypedResultExecuteRequestAuthorityV1 carrier_authority;
  carrier_authority.expected_server_request_uuid = request.server_request_uuid;
  carrier_authority.maximum_descriptor_bytes =
      request.maximum_descriptor_bytes;
  carrier_authority.maximum_row_packet_bytes =
      request.cursor_stream_descriptor.maximum_chunk_bytes;
  auto verified = wire::ValidateTypedResultExecuteCarrierV1(
      carrier_authority, carrier,
      [frozen_vector, descriptor_authority](const auto& descriptor) {
        return FrozenDescriptorDecision(frozen_vector, descriptor_authority,
                                        descriptor);
      });
  if (!verified.ok() ||
      verified.cursor_state.encoded_row_descriptor != frozen_vector) {
    CloseTypedResultProducerCursorV1(
        *opened.carrier,
        TypedResultProducerCloseReasonV1::receipt_invalidation);
    return Refuse<NarrowQueryTypedResultCursorOpenResultV1>(
        StatusForDiagnostic(
            verified.diagnostic_code,
            NarrowQueryTypedResultPublicationStatusV1::cursor_refused),
        verified.diagnostic_code.empty() ? kExecutionFailed
                                         : verified.diagnostic_code,
        verified.detail.empty() ? "cursor_open_carrier_verification_failed"
                                : verified.detail);
  }

  auto result = Success<NarrowQueryTypedResultCursorOpenResultV1>();
  result.execute_carrier = std::move(carrier);
  result.cursor = std::move(opened.carrier);
  return result;
}

const char* NarrowQueryTypedResultPublicationStatusNameV1(
    NarrowQueryTypedResultPublicationStatusV1 status) {
  switch (status) {
    case NarrowQueryTypedResultPublicationStatusV1::ok:
      return "ok";
    case NarrowQueryTypedResultPublicationStatusV1::invalid_argument:
      return "invalid_argument";
    case NarrowQueryTypedResultPublicationStatusV1::profile_unavailable:
      return "profile_unavailable";
    case NarrowQueryTypedResultPublicationStatusV1::binding_invalid:
      return "binding_invalid";
    case NarrowQueryTypedResultPublicationStatusV1::descriptor_invalid:
      return "descriptor_invalid";
    case NarrowQueryTypedResultPublicationStatusV1::result_receipt_mismatch:
      return "result_receipt_mismatch";
    case NarrowQueryTypedResultPublicationStatusV1::row_shape_invalid:
      return "row_shape_invalid";
    case NarrowQueryTypedResultPublicationStatusV1::resource_budget_exceeded:
      return "resource_budget_exceeded";
    case NarrowQueryTypedResultPublicationStatusV1::cancelled:
      return "cancelled";
    case NarrowQueryTypedResultPublicationStatusV1::cursor_refused:
      return "cursor_refused";
    case NarrowQueryTypedResultPublicationStatusV1::publication_failed:
      return "publication_failed";
  }
  return "unknown";
}

}  // namespace scratchbird::engine::internal_api
