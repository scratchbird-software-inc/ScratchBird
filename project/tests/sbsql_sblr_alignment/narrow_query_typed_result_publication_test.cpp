// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/query/narrow_query_typed_result_publication.hpp"

#include "core/datatypes/datatype_descriptor.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace datatypes = scratchbird::core::datatypes;
namespace wire = scratchbird::wire;
using scratchbird::core::platform::byte;

void Require(bool condition, const std::string& detail) {
  if (!condition) throw std::runtime_error(detail);
}

wire::NarrowQueryUuid Uuid(unsigned seed) {
  wire::NarrowQueryUuid uuid{};
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    uuid[index] = static_cast<byte>((seed * 37u + index * 19u) & 0xffu);
  }
  uuid[0] |= 1u;
  uuid[6] = static_cast<byte>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<byte>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

wire::NarrowQueryHash Hash(unsigned seed) {
  wire::NarrowQueryHash hash{};
  for (std::size_t index = 0; index < hash.size(); ++index) {
    hash[index] = static_cast<byte>((seed * 23u + index * 11u) & 0xffu);
  }
  hash[0] |= 1u;
  return hash;
}

std::vector<byte> Int64(std::int64_t value) {
  std::vector<byte> bytes(8);
  const auto bits = static_cast<std::uint64_t>(value);
  for (unsigned shift = 0; shift < 64; shift += 8) {
    bytes[shift / 8] = static_cast<byte>((bits >> shift) & 0xffu);
  }
  return bytes;
}

wire::NarrowQuerySourceOccurrence Source(
    unsigned ordinal,
    const wire::NarrowQueryUuid& object_uuid,
    std::string alias) {
  wire::NarrowQuerySourceOccurrence source;
  source.source_ordinal = ordinal;
  source.source_occurrence_uuid = Uuid(100 + ordinal);
  source.source_occurrence_generation = 1000 + ordinal;
  source.relation_descriptor_uuid = Uuid(200);
  source.relation_descriptor_generation = 2000;
  source.relation_object_uuid = object_uuid;
  source.schema_uuid = Uuid(201);
  source.validated_resource_epoch = 2001;
  source.relation_projection_sha256 = Hash(202);
  source.alias = std::move(alias);
  return source;
}

wire::NarrowQueryOutputOccurrence Output(
    unsigned ordinal,
    const wire::NarrowQuerySourceOccurrence& source,
    const wire::NarrowQueryUuid& source_column_uuid,
    unsigned source_column_ordinal,
    std::string name,
    unsigned name_occurrence) {
  wire::NarrowQueryOutputOccurrence output;
  output.output_ordinal = ordinal;
  output.name_occurrence = name_occurrence;
  output.output_occurrence_uuid = Uuid(300 + ordinal);
  output.output_occurrence_generation = 3000 + ordinal;
  output.source_occurrence_uuid = source.source_occurrence_uuid;
  output.source_occurrence_generation = source.source_occurrence_generation;
  output.source_column_uuid = source_column_uuid;
  output.source_column_ordinal = source_column_ordinal;
  output.output_descriptor_uuid = Uuid(400 + ordinal);
  output.output_descriptor_generation = 4000 + ordinal;
  output.datatype_descriptor_uuid = Uuid(500);
  output.datatype_descriptor_generation = 5000;
  output.datatype_type_uuid = Uuid(501);
  output.datatype_type_generation = 5001;
  output.datatype_binary_type_code =
      static_cast<std::uint32_t>(datatypes::CanonicalTypeId::int64);
  output.codec_version = 1;
  output.nullability = 1;
  output.null_encoding = 1;
  output.codec_generation = 5002;
  output.canonical_value_bytes = 8;
  output.name = std::move(name);
  output.codec_id = "datatype.int64.le.v1";
  return output;
}

wire::NarrowQueryOrderingTerm Term(
    unsigned ordinal,
    const wire::NarrowQuerySourceOccurrence& source,
    const wire::NarrowQueryUuid& source_column_uuid,
    unsigned source_column_ordinal) {
  wire::NarrowQueryOrderingTerm term;
  term.term_ordinal = ordinal;
  term.ordering_term_uuid = Uuid(600 + ordinal);
  term.ordering_term_generation = 6000 + ordinal;
  term.source_occurrence_uuid = source.source_occurrence_uuid;
  term.source_occurrence_generation = source.source_occurrence_generation;
  term.source_column_uuid = source_column_uuid;
  term.source_column_ordinal = source_column_ordinal;
  term.direction = ordinal == 0 ? wire::NarrowQueryDirection::ascending
                                : wire::NarrowQueryDirection::descending;
  term.null_placement = ordinal == 0 ? wire::NarrowQueryNullPlacement::last
                                     : wire::NarrowQueryNullPlacement::first;
  return term;
}

wire::NarrowQueryBinding BaseBinding(wire::NarrowQueryProfile profile) {
  wire::NarrowQueryBinding binding;
  binding.profile = profile;
  binding.statement_receipt_uuid = Uuid(1);
  binding.owning_transaction_uuid = Uuid(2);
  binding.owning_local_transaction_id = 3;
  binding.statement_snapshot_uuid = Uuid(4);
  binding.datatype_catalog_snapshot_uuid = Uuid(5);
  binding.datatype_catalog_generation = 6;
  binding.datatype_registry_generation = 7;
  binding.security_context_uuid = Uuid(8);
  binding.policy_snapshot_uuid = Uuid(9);
  binding.policy_generation = 10;
  binding.resource_grant_receipt_uuid = Uuid(11);
  binding.resource_grant_generation = 12;
  binding.cancellation_receipt_uuid = Uuid(13);
  binding.cancellation_generation = 14;
  binding.execution_uuid = Uuid(15);
  binding.result_set_uuid = Uuid(16);
  binding.row_descriptor_uuid = Uuid(17);
  binding.row_descriptor_generation = 18;
  binding.source_vector_uuid = Uuid(19);
  binding.source_vector_generation = 20;
  binding.output_vector_uuid = Uuid(21);
  binding.output_vector_generation = 22;
  binding.maximum_mga_relation_decoded_bytes_per_pass =
      64ull * 1024ull * 1024ull;
  if (profile == wire::NarrowQueryProfile::ordered_projection) {
    binding.ordering_vector_uuid = Uuid(23);
    binding.ordering_vector_generation = 24;
  }
  return binding;
}

wire::NarrowQueryBinding OrderedBinding() {
  auto binding =
      BaseBinding(wire::NarrowQueryProfile::ordered_projection);
  binding.sources.push_back(Source(0, Uuid(700), "ordered"));
  const auto first = Uuid(701);
  const auto second = Uuid(702);
  binding.outputs.push_back(
      Output(0, binding.sources[0], first, 0, "id", 0));
  binding.outputs.push_back(
      Output(1, binding.sources[0], second, 1, "payload;=literal", 0));
  binding.ordering_terms.push_back(
      Term(0, binding.sources[0], first, 0));
  binding.ordering_terms.push_back(
      Term(1, binding.sources[0], second, 1));
  return binding;
}

wire::NarrowQueryBinding ProjectionOccurrenceBinding() {
  auto binding =
      BaseBinding(wire::NarrowQueryProfile::projection_occurrence);
  binding.sources.push_back(Source(0, Uuid(710), "projection"));
  const auto column = Uuid(711);
  binding.outputs.push_back(Output(0, binding.sources[0], column, 2,
                                   "payload;=literal", 0));
  binding.outputs.push_back(Output(1, binding.sources[0], column, 2,
                                   "payload;=literal", 1));
  return binding;
}

wire::NarrowQueryBinding SelfJoinBinding() {
  auto binding =
      BaseBinding(wire::NarrowQueryProfile::alias_distinct_self_join);
  const auto object_uuid = Uuid(720);
  binding.sources.push_back(Source(0, object_uuid, "left_alias"));
  binding.sources.push_back(Source(1, object_uuid, "middle_alias"));
  binding.sources.push_back(Source(2, object_uuid, "right_alias"));
  const auto column = Uuid(721);
  for (unsigned index = 0; index < binding.sources.size(); ++index) {
    binding.outputs.push_back(
        Output(index, binding.sources[index], column, 0, "id", index));
  }
  return binding;
}

wire::NarrowQueryBindingValidationContext Context(
    const wire::NarrowQueryBinding& binding,
    bool accept_any_datatype = false) {
  wire::NarrowQueryBindingValidationContext context;
  context.statement_receipt_uuid = binding.statement_receipt_uuid;
  context.owning_transaction_uuid = binding.owning_transaction_uuid;
  context.owning_local_transaction_id = binding.owning_local_transaction_id;
  context.statement_snapshot_uuid = binding.statement_snapshot_uuid;
  context.datatype_catalog_snapshot_uuid =
      binding.datatype_catalog_snapshot_uuid;
  context.datatype_catalog_generation = binding.datatype_catalog_generation;
  context.datatype_registry_generation = binding.datatype_registry_generation;
  context.security_context_uuid = binding.security_context_uuid;
  context.policy_snapshot_uuid = binding.policy_snapshot_uuid;
  context.policy_generation = binding.policy_generation;
  context.resource_grant_receipt_uuid = binding.resource_grant_receipt_uuid;
  context.resource_grant_generation = binding.resource_grant_generation;
  context.cancellation_receipt_uuid = binding.cancellation_receipt_uuid;
  context.cancellation_generation = binding.cancellation_generation;
  context.execution_uuid = binding.execution_uuid;
  context.result_set_uuid = binding.result_set_uuid;
  context.row_descriptor_uuid = binding.row_descriptor_uuid;
  context.row_descriptor_generation = binding.row_descriptor_generation;
  context.maximum_mga_relation_decoded_bytes_per_pass =
      binding.maximum_mga_relation_decoded_bytes_per_pass;
  context.maximum_result_rows =
      wire::kNarrowQueryMaximumExplicitResultRows;
  context.validate_canonical_alias = [](std::string_view alias) {
    return !alias.empty();
  };
  context.validate_source = [](const auto&) {
    return wire::NarrowQueryAuthorityDecision::accepted;
  };
  context.validate_output_datatype = [accept_any_datatype](const auto& output) {
    return accept_any_datatype ||
                   output.datatype_binary_type_code ==
                       static_cast<std::uint32_t>(
                           datatypes::CanonicalTypeId::int64)
               ? wire::NarrowQueryAuthorityDecision::accepted
               : wire::NarrowQueryAuthorityDecision::stale_or_mismatched;
  };
  context.validate_collation = [](const auto&) {
    return wire::NarrowQueryAuthorityDecision::accepted;
  };
  return context;
}

std::vector<byte> Encode(const wire::NarrowQueryBinding& binding) {
  std::vector<byte> encoded;
  wire::NarrowQueryBindingError error;
  Require(wire::EncodeNarrowQueryBinding(binding, &encoded, &error),
          "SBQNPB01 encode failed: " + error.field + ":" + error.detail);
  return encoded;
}

api::NarrowQueryTypedResultExpectedReceiptV1 Expected(
    const wire::NarrowQueryBinding& binding) {
  api::NarrowQueryTypedResultExpectedReceiptV1 expected;
  expected.profile_code = static_cast<std::uint16_t>(binding.profile);
  expected.statement_receipt_uuid = binding.statement_receipt_uuid;
  expected.query_handle.execution_uuid = binding.execution_uuid;
  expected.query_handle.result_set_uuid = binding.result_set_uuid;
  expected.query_handle.row_descriptor_uuid = binding.row_descriptor_uuid;
  expected.query_handle.snapshot_uuid = binding.statement_snapshot_uuid;
  expected.row_descriptor_generation = binding.row_descriptor_generation;
  return expected;
}

struct DescriptorAuthorityControl {
  bool live = true;
  int calls = 0;
};

wire::TypedResultDescriptorAuthorityValidator DescriptorAuthority(
    const std::shared_ptr<DescriptorAuthorityControl>& control) {
  return [control](const wire::TypedResultRowDescriptor& descriptor) {
    ++control->calls;
    wire::TypedResultDescriptorAuthorityDecision decision;
    decision.accepted = control->live && !descriptor.columns.empty() &&
                        descriptor.columns[0].canonical_type_id ==
                            datatypes::CanonicalTypeId::int64;
    decision.diagnostic_code = "DATATYPE.DESCRIPTOR_INVALID";
    decision.detail = decision.accepted ? "" : "fixture_descriptor_stale";
    return decision;
  };
}

api::NarrowQueryTypedResultPrepareResultV1 Prepare(
    const wire::NarrowQueryBinding& binding,
    const std::shared_ptr<DescriptorAuthorityControl>& authority,
    bool accept_any_datatype = false) {
  const auto encoded = Encode(binding);
  api::NarrowQueryTypedResultPrepareRequestV1 request;
  request.canonical_profile_binding = encoded;
  request.validation_context = Context(binding, accept_any_datatype);
  request.expected_receipt = Expected(binding);
  request.descriptor_authority = DescriptorAuthority(authority);
  return api::PrepareNarrowQueryTypedResultPublicationV1(request);
}

api::NarrowQueryTypedResultOccurrenceCellV1 Cell(
    const wire::NarrowQueryOutputOccurrence& output,
    std::vector<byte> payload,
    wire::TypedResultValueState state =
        wire::TypedResultValueState::value_present) {
  api::NarrowQueryTypedResultOccurrenceCellV1 cell;
  cell.output_occurrence_uuid = output.output_occurrence_uuid;
  cell.output_occurrence_generation = output.output_occurrence_generation;
  cell.state = state;
  cell.canonical_payload = std::move(payload);
  return cell;
}

api::NarrowQueryTypedResultOccurrenceRowV1 Row(
    const wire::NarrowQueryBinding& binding,
    std::vector<std::vector<byte>> payloads,
    bool reverse = false,
    std::uint64_t ordinal = 0) {
  Require(payloads.size() == binding.outputs.size(),
          "fixture payload count mismatch");
  api::NarrowQueryTypedResultOccurrenceRowV1 row;
  row.row_ordinal = ordinal;
  for (std::size_t index = 0; index < payloads.size(); ++index) {
    row.cells.push_back(Cell(binding.outputs[index], std::move(payloads[index])));
  }
  if (reverse) std::reverse(row.cells.begin(), row.cells.end());
  return row;
}

void TestImmutableDescriptorsAndOccurrences() {
  auto authority = std::make_shared<DescriptorAuthorityControl>();
  const auto ordered = OrderedBinding();
  auto ordered_prepared = Prepare(ordered, authority);
  Require(ordered_prepared.ok() && ordered_prepared.binding,
          "ordered publication prepare failed: " + ordered_prepared.detail);
  Require(!ordered_prepared.generic_fallback_permitted(),
          "successful narrow preparation exposed a fallback path");
  const auto& ordered_descriptor =
      ordered_prepared.binding->row_descriptor();
  Require(ordered_descriptor.descriptor_uuid == ordered.row_descriptor_uuid &&
              ordered_descriptor.descriptor_generation ==
                  ordered.row_descriptor_generation &&
              ordered_descriptor.columns.size() == 2 &&
              ordered_descriptor.columns[1].name == "payload;=literal" &&
              ordered_descriptor.columns[1].descriptor_uuid ==
                  ordered.outputs[1].datatype_descriptor_uuid &&
              ordered_descriptor.columns[1].canonical_type_id ==
                  datatypes::CanonicalTypeId::int64,
          "ordered immutable descriptor did not exactly mirror outputs");
  const auto descriptor_again = wire::EncodeTypedResultRowDescriptor(
      ordered_descriptor);
  Require(descriptor_again.ok() &&
              descriptor_again.encoded ==
                  ordered_prepared.binding->result_descriptor_vector(),
          "frozen result descriptor is not a canonical immutable vector");

  const auto duplicate = ProjectionOccurrenceBinding();
  auto duplicate_prepared = Prepare(duplicate, authority);
  Require(duplicate_prepared.ok(),
          "projection occurrence prepare failed: " +
              duplicate_prepared.detail);
  const auto& duplicate_descriptor =
      duplicate_prepared.binding->row_descriptor();
  Require(duplicate_descriptor.columns[0].name == "payload;=literal" &&
              duplicate_descriptor.columns[1].name == "payload;=literal" &&
              duplicate_descriptor.columns[0].name_occurrence == 0 &&
              duplicate_descriptor.columns[1].name_occurrence == 1,
          "duplicate output name occurrences were collapsed");
  auto duplicate_row = Row(duplicate, {Int64(41), Int64(41)}, true);
  auto materialized = api::MaterializeNarrowQueryTypedResultRowV1(
      *duplicate_prepared.binding, duplicate_row);
  Require(materialized.ok() && materialized.row.cells.size() == 2 &&
              materialized.row.cells[0].column_ordinal == 0 &&
              materialized.row.cells[0].name_occurrence == 0 &&
              materialized.row.cells[1].column_ordinal == 1 &&
              materialized.row.cells[1].name_occurrence == 1 &&
              materialized.row.cells[0].canonical_payload ==
                  materialized.row.cells[1].canonical_payload,
          "occurrence-keyed duplicate projection did not preserve order");

  duplicate_row = Row(duplicate, {Int64(41), Int64(42)});
  materialized = api::MaterializeNarrowQueryTypedResultRowV1(
      *duplicate_prepared.binding, duplicate_row);
  Require(!materialized.ok() &&
              materialized.diagnostic_code ==
                  "PROJECTION.OUTPUT_ROWSET.INVALID" &&
              !materialized.generic_fallback_permitted(),
          "contradictory repeated source values were not refused exactly");

  duplicate_row = Row(duplicate, {Int64(41), Int64(41)});
  duplicate_row.cells[1].output_occurrence_uuid = Uuid(999);
  materialized = api::MaterializeNarrowQueryTypedResultRowV1(
      *duplicate_prepared.binding, duplicate_row);
  Require(!materialized.ok() &&
              materialized.diagnostic_code == "RESULT_SET.SHAPE_INVALID",
          "unknown output occurrence was not refused");

  const auto self_join = SelfJoinBinding();
  auto self_join_prepared = Prepare(self_join, authority);
  Require(self_join_prepared.ok(),
          "self join prepare failed: " + self_join_prepared.detail);
  auto self_join_row = Row(
      self_join, {Int64(10), Int64(20), Int64(30)}, true);
  materialized = api::MaterializeNarrowQueryTypedResultRowV1(
      *self_join_prepared.binding, self_join_row);
  Require(materialized.ok() && materialized.row.cells.size() == 3 &&
              materialized.row.cells[0].name_occurrence == 0 &&
              materialized.row.cells[1].name_occurrence == 1 &&
              materialized.row.cells[2].name_occurrence == 2 &&
              materialized.row.cells[0].canonical_payload == Int64(10) &&
              materialized.row.cells[2].canonical_payload == Int64(30),
          "alias-distinct self-join occurrences were merged or reordered");

  auto invalid_datatype = OrderedBinding();
  invalid_datatype.outputs[0].datatype_binary_type_code = 7;
  auto invalid_prepared = Prepare(invalid_datatype, authority, true);
  Require(!invalid_prepared.ok() &&
              invalid_prepared.status == api::
                  NarrowQueryTypedResultPublicationStatusV1::descriptor_invalid &&
              invalid_prepared.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR_INVALID" &&
              !invalid_prepared.generic_fallback_permitted(),
          "unsupported datatype code was inferred or fell back");
}

api::NarrowQueryTypedResultDirectRequestV1 DirectRequest(
    const wire::NarrowQueryBinding& binding,
    const std::shared_ptr<const api::NarrowQueryTypedResultPublicationBindingV1>&
        prepared) {
  api::NarrowQueryTypedResultDirectRequestV1 request;
  request.receipt = prepared->receipt();
  request.server_request_uuid = Uuid(800);
  request.batch_uuid = Uuid(801);
  request.rows.push_back(
      Row(binding, {Int64(1), Int64(2)}, true));
  request.publication_authority =
      [receipt = prepared->receipt()](const auto& observed) {
        api::NarrowQueryTypedResultPublicationAuthorityDecisionV1 decision;
        decision.accepted =
            observed.receipt.profile_code == receipt.profile_code &&
            observed.receipt.statement_receipt_uuid ==
                receipt.statement_receipt_uuid &&
            observed.row_count == 1;
        decision.diagnostic_code = "MGA.TRANSACTION.STALE";
        decision.detail = decision.accepted ? "" : "fixture_receipt_stale";
        return decision;
      };
  return request;
}

void TestDirectPublicationAndNoFallback() {
  auto descriptor_authority =
      std::make_shared<DescriptorAuthorityControl>();
  const auto ordered = OrderedBinding();
  auto prepared = Prepare(ordered, descriptor_authority);
  Require(prepared.ok(), "direct fixture preparation failed");
  auto request = DirectRequest(ordered, prepared.binding);
  auto published = api::PublishNarrowQueryTypedResultDirectBatchV1(
      prepared.binding, request);
  Require(published.ok() &&
              published.execute_carrier.outcome ==
                  wire::TypedResultExecuteOutcome::row_batch &&
              published.execute_carrier.result_descriptor_vector ==
                  prepared.binding->result_descriptor_vector() &&
              !published.execute_carrier.row_data_packet.empty() &&
              published.batch.rows.size() == 1 &&
              published.batch.rows[0].cells[1].canonical_payload == Int64(2),
          "verified direct RowDataPacketV1 publication failed: " +
              published.detail);

  request = DirectRequest(ordered, prepared.binding);
  request.receipt.profile_code = static_cast<std::uint16_t>(
      wire::NarrowQueryProfile::alias_distinct_self_join);
  auto refused = api::PublishNarrowQueryTypedResultDirectBatchV1(
      prepared.binding, request);
  Require(!refused.ok() &&
              refused.diagnostic_code ==
                  "PROFILE.BUILTIN_PROFILE_UNAVAILABLE" &&
              refused.execute_carrier.row_data_packet.empty() &&
              !refused.generic_fallback_permitted(),
          "cross-profile direct publication was not fail-closed");

  request = DirectRequest(ordered, prepared.binding);
  request.receipt.query_handle.result_set_uuid = Uuid(997);
  refused = api::PublishNarrowQueryTypedResultDirectBatchV1(
      prepared.binding, request);
  Require(!refused.ok() &&
              refused.diagnostic_code == "RESULT_SET.SHAPE_INVALID" &&
              refused.execute_carrier.row_data_packet.empty(),
          "cross-result direct publication was not refused before bytes");

  request = DirectRequest(ordered, prepared.binding);
  request.publication_authority = [calls = 0](const auto& observed) mutable {
    ++calls;
    api::NarrowQueryTypedResultPublicationAuthorityDecisionV1 decision;
    decision.accepted =
        observed.phase == api::NarrowQueryTypedResultPublicationPhaseV1::
                              before_row_materialization;
    if (!decision.accepted) {
      decision.diagnostic_code = "PROCESS.CANCELLED";
      decision.detail = "cancelled_before_direct_publication_barrier";
    }
    return decision;
  };
  refused = api::PublishNarrowQueryTypedResultDirectBatchV1(
      prepared.binding, request);
  Require(!refused.ok() &&
              refused.status == api::
                  NarrowQueryTypedResultPublicationStatusV1::cancelled &&
              refused.diagnostic_code == "PROCESS.CANCELLED" &&
              refused.execute_carrier.row_data_packet.empty(),
          "cancellation immediately before direct barrier published bytes");

  request = DirectRequest(ordered, prepared.binding);
  request.rows.clear();
  refused = api::PublishNarrowQueryTypedResultDirectBatchV1(
      prepared.binding, request);
  Require(!refused.ok() &&
              refused.diagnostic_code ==
                  "SB_ENGINE_STATUS_INVALID_ARGUMENT" &&
              !refused.generic_fallback_permitted(),
          "empty direct batch silently changed result profile");
}

struct CursorAuthorityControl {
  wire::TypedResultUuid session_uuid{};
  wire::TypedResultUuid statement_receipt_uuid{};
  wire::TypedResultUuid snapshot_uuid{};
  wire::TypedResultUuid cancellation_uuid{};
  std::uint64_t cancellation_generation = 0;
  wire::TypedResultUuid resource_uuid{};
  std::uint64_t resource_generation = 0;
  std::uint64_t resource_bytes = 0;
  int statement_releases = 0;
  int snapshot_releases = 0;
  int cancellation_releases = 0;
  int resource_releases = 0;
  int source_closes = 0;
  int stage_commits = 0;
  int stage_aborts = 0;
};

class OccurrenceStageLease final
    : public api::TypedResultProducerStageLeaseActionV1 {
 public:
  explicit OccurrenceStageLease(
      std::shared_ptr<CursorAuthorityControl> control)
      : control_(std::move(control)) {}

  CommitStatus Commit() noexcept override {
    ++control_->stage_commits;
    return CommitStatus::committed;
  }

  void Abort() noexcept override { ++control_->stage_aborts; }

 private:
  std::shared_ptr<CursorAuthorityControl> control_;
};

class StatementReceipt final : public api::TypedResultStatementReceiptHandleV1 {
 public:
  explicit StatementReceipt(std::shared_ptr<CursorAuthorityControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerOwnerObservationV1 ObserveOwner(
      const wire::TypedResultUuid& session) override {
    return session == control_->session_uuid
               ? api::TypedResultProducerOwnerObservationV1::authorized
               : api::TypedResultProducerOwnerObservationV1::denied;
  }
  api::TypedResultProducerReceiptObservationV1 ObserveReceipt(
      const wire::TypedResultUuid& receipt) override {
    return receipt == control_->statement_receipt_uuid
               ? api::TypedResultProducerReceiptObservationV1::live
               : api::TypedResultProducerReceiptObservationV1::stale;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->statement_releases;
  }

 private:
  std::shared_ptr<CursorAuthorityControl> control_;
};

class SnapshotPin final : public api::TypedResultMgaSnapshotPinHandleV1 {
 public:
  explicit SnapshotPin(std::shared_ptr<CursorAuthorityControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerMgaObservationV1 ObserveSnapshot(
      const wire::TypedResultUuid& statement_snapshot,
      const wire::TypedResultUuid& result_snapshot) override {
    return statement_snapshot == control_->snapshot_uuid &&
                   result_snapshot == control_->snapshot_uuid
               ? api::TypedResultProducerMgaObservationV1::live_and_equal
               : api::TypedResultProducerMgaObservationV1::stale_or_unequal;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->snapshot_releases;
  }

 private:
  std::shared_ptr<CursorAuthorityControl> control_;
};

class CancellationReceipt final
    : public api::TypedResultCancellationReceiptHandleV1 {
 public:
  explicit CancellationReceipt(std::shared_ptr<CursorAuthorityControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerCancellationObservationV1 ObserveCancellation(
      const wire::TypedResultUuid& uuid,
      std::uint64_t generation) override {
    return uuid == control_->cancellation_uuid &&
                   generation == control_->cancellation_generation
               ? api::TypedResultProducerCancellationObservationV1::live
               : api::TypedResultProducerCancellationObservationV1::stale;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->cancellation_releases;
  }

 private:
  std::shared_ptr<CursorAuthorityControl> control_;
};

class ResourceGrant final
    : public api::TypedResultResourceGrantReceiptHandleV1 {
 public:
  explicit ResourceGrant(std::shared_ptr<CursorAuthorityControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerGrantObservationV1 ObserveGrant(
      const wire::TypedResultUuid& uuid,
      std::uint64_t generation,
      std::uint64_t retained,
      std::uint64_t requested) override {
    return uuid == control_->resource_uuid &&
                   generation == control_->resource_generation &&
                   retained == control_->resource_bytes &&
                   requested <= retained
               ? api::TypedResultProducerGrantObservationV1::live
               : api::TypedResultProducerGrantObservationV1::stale_or_released;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->resource_releases;
  }

 private:
  std::shared_ptr<CursorAuthorityControl> control_;
};

class OccurrenceSource final
    : public api::NarrowQueryTypedResultOccurrenceSourceV1 {
 public:
  OccurrenceSource(
      std::shared_ptr<CursorAuthorityControl> control,
      api::NarrowQueryTypedResultOccurrenceStageResultV1 staged)
      : control_(std::move(control)), staged_(std::move(staged)) {}

  api::NarrowQueryTypedResultOccurrenceStageResultV1 Stage(
      const api::TypedResultProducerStageRequestV1& request) override {
    Require(request.next_batch_ordinal == 0 && request.row_position == 0,
            "cursor adapter changed initial producer position");
    api::NarrowQueryTypedResultOccurrenceStageResultV1 result;
    result.outcome = staged_.outcome;
    result.end_of_cursor = staged_.end_of_cursor;
    result.rows = staged_.rows;
    result.detail = staged_.detail;
    if (result.outcome == api::TypedResultProducerStageOutcomeV1::batch ||
        result.outcome == api::TypedResultProducerStageOutcomeV1::empty_eos) {
      result.lease = api::TypedResultProducerStageLeaseV1(
          std::make_unique<OccurrenceStageLease>(control_));
    }
    return result;
  }
  void Close(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->source_closes;
  }

 private:
  std::shared_ptr<CursorAuthorityControl> control_;
  api::NarrowQueryTypedResultOccurrenceStageResultV1 staged_;
};

api::NarrowQueryTypedResultCursorOpenRequestV1 CursorRequest(
    const wire::NarrowQueryBinding& binding,
    const std::shared_ptr<const api::NarrowQueryTypedResultPublicationBindingV1>&
        prepared,
    const std::shared_ptr<CursorAuthorityControl>& control,
    api::NarrowQueryTypedResultOccurrenceStageResultV1 staged) {
  api::NarrowQueryTypedResultCursorOpenRequestV1 request;
  request.receipt = prepared->receipt();
  request.server_request_uuid = Uuid(900);
  request.carrier_uuid = Uuid(901);
  request.carrier_generation = 902;
  request.cursor_uuid = Uuid(903);
  request.session_uuid = Uuid(904);
  request.resource_grant_bytes = 65536;
  request.cursor_stream_descriptor.descriptor_uuid = Uuid(905);
  request.cursor_stream_descriptor.descriptor_version = 1;
  request.cursor_stream_descriptor.descriptor_generation = 906;
  request.cursor_stream_descriptor.maximum_chunk_rows = 16;
  request.cursor_stream_descriptor.maximum_chunk_bytes = 65536;

  control->session_uuid = request.session_uuid;
  control->statement_receipt_uuid = binding.statement_receipt_uuid;
  control->snapshot_uuid = binding.statement_snapshot_uuid;
  control->cancellation_uuid = binding.cancellation_receipt_uuid;
  control->cancellation_generation = binding.cancellation_generation;
  control->resource_uuid = binding.resource_grant_receipt_uuid;
  control->resource_generation = binding.resource_grant_generation;
  control->resource_bytes = request.resource_grant_bytes;
  request.statement_receipt =
      std::make_unique<StatementReceipt>(control);
  request.mga_snapshot_pin = std::make_unique<SnapshotPin>(control);
  request.cancellation_receipt =
      std::make_unique<CancellationReceipt>(control);
  request.resource_grant_receipt =
      std::make_unique<ResourceGrant>(control);
  request.producer_state =
      std::make_unique<OccurrenceSource>(control, std::move(staged));
  return request;
}

void TestCursorOccurrenceProducerPublication() {
  auto descriptor_authority =
      std::make_shared<DescriptorAuthorityControl>();
  const auto duplicate = ProjectionOccurrenceBinding();
  auto prepared = Prepare(duplicate, descriptor_authority);
  Require(prepared.ok(), "cursor fixture preparation failed");

  api::NarrowQueryTypedResultOccurrenceStageResultV1 staged;
  staged.outcome = api::TypedResultProducerStageOutcomeV1::batch;
  staged.end_of_cursor = true;
  staged.rows.push_back(Row(duplicate, {Int64(77), Int64(77)}, true));
  auto control = std::make_shared<CursorAuthorityControl>();
  auto open_request =
      CursorRequest(duplicate, prepared.binding, control, std::move(staged));
  const auto cursor_uuid = open_request.cursor_uuid;
  const auto carrier_generation = open_request.carrier_generation;
  const auto stream = open_request.cursor_stream_descriptor;
  auto opened = api::OpenNarrowQueryTypedResultCursorV1(
      prepared.binding, std::move(open_request));
  Require(opened.ok() && opened.cursor &&
              opened.execute_carrier.outcome ==
                  wire::TypedResultExecuteOutcome::cursor_open &&
              opened.execute_carrier.result_descriptor_vector ==
                  prepared.binding->result_descriptor_vector(),
          "cursor-open typed publication failed: " + opened.detail);

  api::TypedResultProducerPullRequestV1 pull;
  pull.cursor_uuid = cursor_uuid;
  pull.carrier_generation = carrier_generation;
  pull.cursor_stream_descriptor_uuid = stream.descriptor_uuid;
  pull.cursor_stream_descriptor_version = stream.descriptor_version;
  pull.cursor_stream_descriptor_generation = stream.descriptor_generation;
  pull.expected_batch_ordinal = 0;
  pull.maximum_rows = 16;
  pull.maximum_bytes = 65536;
  pull.timeout_millis = 1000;
  auto pulled = api::PullTypedResultProducerCursorV1(*opened.cursor, pull);
  Require(pulled.ok() &&
              pulled.outcome == api::TypedResultProducerPullOutcomeV1::batch &&
              pulled.end_of_cursor && pulled.row_count == 1 &&
              pulled.batch.rows[0].cells[0].name_occurrence == 0 &&
              pulled.batch.rows[0].cells[1].name_occurrence == 1 &&
              pulled.batch.rows[0].cells[0].canonical_payload == Int64(77) &&
              pulled.batch.rows[0].cells[1].canonical_payload == Int64(77),
          "occurrence-keyed cursor batch was not atomically published: " +
              pulled.detail);
  Require(control->statement_releases == 1 &&
              control->snapshot_releases == 1 &&
              control->cancellation_releases == 1 &&
              control->resource_releases == 1 &&
              control->source_closes == 1 && control->stage_commits == 1 &&
              control->stage_aborts == 0,
          "cursor terminal publication did not release authority exactly once");

  staged = {};
  staged.outcome = api::TypedResultProducerStageOutcomeV1::batch;
  staged.end_of_cursor = true;
  staged.rows.push_back(Row(duplicate, {Int64(88), Int64(88)}));
  staged.rows[0].cells[1].output_occurrence_uuid = Uuid(998);
  control = std::make_shared<CursorAuthorityControl>();
  open_request =
      CursorRequest(duplicate, prepared.binding, control, std::move(staged));
  const auto bad_cursor_uuid = open_request.cursor_uuid;
  const auto bad_carrier_generation = open_request.carrier_generation;
  const auto bad_stream = open_request.cursor_stream_descriptor;
  opened = api::OpenNarrowQueryTypedResultCursorV1(
      prepared.binding, std::move(open_request));
  Require(opened.ok(), "malformed staged row incorrectly failed cursor open");
  pull.cursor_uuid = bad_cursor_uuid;
  pull.carrier_generation = bad_carrier_generation;
  pull.cursor_stream_descriptor_uuid = bad_stream.descriptor_uuid;
  pull.cursor_stream_descriptor_version = bad_stream.descriptor_version;
  pull.cursor_stream_descriptor_generation = bad_stream.descriptor_generation;
  pulled = api::PullTypedResultProducerCursorV1(*opened.cursor, pull);
  Require(!pulled.ok() && pulled.diagnostic_code == "CURSOR.FETCH_FAILED" &&
              pulled.detail.find("RESULT_SET.SHAPE_INVALID") !=
                  std::string::npos &&
              pulled.row_data_packet.empty() && control->stage_commits == 0 &&
              control->stage_aborts == 1,
          "cursor occurrence mismatch published a partial or fallback batch");
  const auto first_refusal_detail = pulled.detail;
  pulled = api::PullTypedResultProducerCursorV1(*opened.cursor, pull);
  const auto refused_snapshot = opened.cursor->Snapshot();
  Require(!pulled.ok() && pulled.diagnostic_code == "CURSOR.FETCH_FAILED" &&
              pulled.detail == first_refusal_detail &&
              pulled.detail.find("RESULT_SET.SHAPE_INVALID") !=
                  std::string::npos &&
              pulled.row_data_packet.empty() && control->stage_commits == 0 &&
              control->stage_aborts == 2 &&
              refused_snapshot.lifecycle ==
                  api::TypedResultProducerCursorLifecycleV1::open &&
              refused_snapshot.row_position == 0 &&
              refused_snapshot.next_batch_ordinal == 0,
          "cursor occurrence mismatch did not abort and replay exactly");
  const auto closed = api::CloseTypedResultProducerCursorV1(
      *opened.cursor, api::TypedResultProducerCloseReasonV1::explicit_close);
  Require(closed.ok(), "cursor with refused batch could not close cleanly");

  staged = {};
  staged.outcome = api::TypedResultProducerStageOutcomeV1::empty_eos;
  staged.end_of_cursor = true;
  control = std::make_shared<CursorAuthorityControl>();
  open_request =
      CursorRequest(duplicate, prepared.binding, control, std::move(staged));
  open_request.receipt.statement_receipt_uuid = Uuid(996);
  opened = api::OpenNarrowQueryTypedResultCursorV1(
      prepared.binding, std::move(open_request));
  Require(!opened.ok() &&
              opened.diagnostic_code == "MGA.TRANSACTION.STALE" &&
              !opened.generic_fallback_permitted() && !opened.cursor,
          "cross-receipt cursor open was not refused before publication");
}

}  // namespace

int main() {
  try {
    TestImmutableDescriptorsAndOccurrences();
    TestDirectPublicationAndNoFallback();
    TestCursorOccurrenceProducerPublication();
    std::cout
        << "PASS narrow_query_typed_result_publication profiles=3 "
           "immutable_descriptor=1 occurrence_rows=1 direct=1 cursor=1 "
           "no_fallback=1\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL narrow_query_typed_result_publication: "
              << error.what() << '\n';
    return 1;
  }
}
