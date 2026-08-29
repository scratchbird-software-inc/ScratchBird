// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/public_abi_typed_result.hpp"
#include "scratchbird/engine/result.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
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

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

wire::TypedResultUuid Uuid(byte tail) {
  wire::TypedResultUuid uuid{};
  uuid[0] = 0x01;
  uuid[1] = 0x9f;
  uuid[6] = 0x70;
  uuid[8] = 0x80;
  uuid[15] = tail;
  return uuid;
}

bool SameUuid(const sb_engine_uuid_t& actual,
              const wire::TypedResultUuid& expected) {
  return std::equal(expected.begin(), expected.end(),
                    std::begin(actual.bytes), std::end(actual.bytes));
}

bool ZeroUuid(const sb_engine_uuid_t& uuid) {
  return std::all_of(std::begin(uuid.bytes), std::end(uuid.bytes),
                     [](std::uint8_t value) { return value == 0; });
}

wire::TypedResultRowDescriptor Descriptor() {
  wire::TypedResultRowDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(0x31);
  descriptor.descriptor_generation = 7;
  descriptor.datatype_catalog_snapshot_uuid = Uuid(0x32);
  descriptor.datatype_catalog_generation = 8;
  descriptor.datatype_registry_generation = 9;

  wire::TypedResultColumnDescriptor column;
  column.ordinal = 0;
  column.name_occurrence = 0;
  column.name = "value;name=literal";
  column.nullability = wire::TypedResultNullability::nullable;
  column.descriptor_uuid = Uuid(0x33);
  column.descriptor_generation = 10;
  column.type_uuid = Uuid(0x34);
  column.type_generation = 11;
  column.canonical_type_id = datatypes::CanonicalTypeId::character;
  column.codec_id = "datatype.character.utf8.v1";
  column.codec_version = 1;
  column.codec_generation = 12;
  descriptor.columns.push_back(std::move(column));
  return descriptor;
}

wire::TypedResultDescriptorAuthorityValidator AuthorityFor(
    const wire::TypedResultRowDescriptor& expected) {
  return [expected](const wire::TypedResultRowDescriptor& actual) {
    wire::TypedResultDescriptorAuthorityDecision decision;
    decision.accepted =
        actual.descriptor_uuid == expected.descriptor_uuid &&
        actual.descriptor_generation == expected.descriptor_generation &&
        actual.datatype_catalog_snapshot_uuid ==
            expected.datatype_catalog_snapshot_uuid &&
        actual.datatype_catalog_generation ==
            expected.datatype_catalog_generation &&
        actual.datatype_registry_generation ==
            expected.datatype_registry_generation &&
        actual.descriptor_evidence_sha256 ==
            expected.descriptor_evidence_sha256 &&
        actual.columns.size() == expected.columns.size() &&
        actual.columns[0].descriptor_uuid ==
            expected.columns[0].descriptor_uuid &&
        actual.columns[0].type_uuid == expected.columns[0].type_uuid &&
        actual.columns[0].codec_id == expected.columns[0].codec_id;
    if (!decision.accepted) {
      decision.diagnostic_code = "DATATYPE.DESCRIPTOR_INVALID";
      decision.detail = "public_abi_fixture_descriptor_drift";
    }
    return decision;
  };
}

wire::TypedResultRow Row(std::string value) {
  wire::TypedResultCell cell;
  cell.column_ordinal = 0;
  cell.name_occurrence = 0;
  cell.state = wire::TypedResultValueState::value_present;
  cell.canonical_payload.assign(value.begin(), value.end());
  wire::TypedResultRow row;
  row.row_ordinal = 0;
  row.cells.push_back(std::move(cell));
  return row;
}

wire::TypedResultQueryHandleV1 QueryHandle(
    const wire::TypedResultRowDescriptor& descriptor) {
  wire::TypedResultQueryHandleV1 handle;
  handle.execution_uuid = Uuid(0x41);
  handle.result_set_uuid = Uuid(0x42);
  handle.row_descriptor_uuid = descriptor.descriptor_uuid;
  handle.snapshot_uuid = Uuid(0x43);
  return handle;
}

struct DirectFixture {
  api::PublicAbiTypedResultAdoptionV1 adoption;
  wire::TypedResultRowDescriptor descriptor;
  wire::TypedResultBatch batch;
  std::vector<byte> descriptor_bytes;
  std::vector<byte> packet_bytes;
};

DirectFixture MakeDirectFixture() {
  DirectFixture fixture;
  auto encoded_descriptor = wire::EncodeTypedResultRowDescriptor(Descriptor());
  Require(encoded_descriptor.ok(), "direct descriptor encode failed");
  fixture.descriptor = encoded_descriptor.descriptor;
  fixture.descriptor_bytes = encoded_descriptor.encoded;
  const auto handle = QueryHandle(fixture.descriptor);

  wire::TypedResultBatch batch;
  batch.execution_uuid = handle.execution_uuid;
  batch.result_set_uuid = handle.result_set_uuid;
  batch.batch_uuid = Uuid(0x44);
  batch.batch_ordinal = 0;
  batch.end_of_rowset = true;
  batch.cursor_bound = false;
  batch.row_descriptor_uuid = handle.row_descriptor_uuid;
  batch.row_descriptor_generation = fixture.descriptor.descriptor_generation;
  batch.descriptor_evidence_sha256 =
      fixture.descriptor.descriptor_evidence_sha256;
  batch.snapshot_uuid = handle.snapshot_uuid;
  batch.rows.push_back(Row("direct;value=1"));
  wire::TypedResultCarrierBinding binding;
  binding.kind = wire::TypedResultCarrierKind::ps_execute_result_v1;
  binding.row_count = 1;
  binding.end_of_rowset = true;
  binding.execution_uuid = handle.execution_uuid;
  binding.result_set_uuid = handle.result_set_uuid;
  binding.snapshot_uuid = handle.snapshot_uuid;
  auto encoded_batch =
      wire::EncodeTypedResultBatch(batch, fixture.descriptor, binding);
  Require(encoded_batch.ok(), "direct batch encode failed");
  fixture.batch = encoded_batch.batch;
  fixture.packet_bytes = encoded_batch.encoded;

  auto& carrier = fixture.adoption.execute_carrier;
  carrier.outcome = wire::TypedResultExecuteOutcome::row_batch;
  carrier.server_request_uuid = Uuid(0x45);
  carrier.transaction_uuid = Uuid(0x46);
  carrier.local_transaction_id = 77;
  carrier.row_count = 1;
  carrier.result_descriptor_vector = fixture.descriptor_bytes;
  carrier.row_data_packet = fixture.packet_bytes;
  carrier.finality_token = {0xf1};
  carrier.message_vector_set = {0xa1, 0xa2};
  carrier.query_handle = handle;
  fixture.adoption.request_authority.expected_server_request_uuid =
      carrier.server_request_uuid;
  fixture.adoption.request_authority.finality_sensitive = true;
  fixture.adoption.descriptor_authority = AuthorityFor(fixture.descriptor);
  return fixture;
}

sb_engine_result_descriptor_view_v1_t DescriptorView() {
  sb_engine_result_descriptor_view_v1_t view{};
  view.struct_size = sizeof(view);
  view.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  return view;
}

sb_engine_row_batch_view_v2_t BatchView() {
  sb_engine_row_batch_view_v2_t view{};
  view.struct_size = sizeof(view);
  view.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  return view;
}

sb_engine_batch_request_v1_t BatchRequest(std::uint64_t rows = 4,
                                          std::uint64_t bytes = 65536) {
  sb_engine_batch_request_v1_t request{};
  request.struct_size = sizeof(request);
  request.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  request.max_rows = rows;
  request.max_bytes = bytes;
  request.timeout_ms = 1000;
  return request;
}

void DirectBinaryViewAndLegacyConflict() {
  auto fixture = MakeDirectFixture();
  const auto expected_descriptor = fixture.descriptor;
  const auto expected_batch = fixture.batch;
  const auto descriptor_bytes = fixture.descriptor_bytes;
  const auto packet_bytes = fixture.packet_bytes;
  const auto handle = fixture.adoption.execute_carrier.query_handle;
  sb_engine_result_t raw = nullptr;
  Require(api::AdoptTypedResultPublicAbiV1(std::move(fixture.adoption), &raw) ==
              SB_ENGINE_STATUS_OK &&
              raw != nullptr,
          "direct typed result adoption failed");
  scratchbird::engine::Result result(raw);

  sb_engine_string_view_t legacy_payload{
      reinterpret_cast<const char*>(static_cast<std::uintptr_t>(1)), 9};
  Require(sb_engine_result_payload(result.get(), &legacy_payload) ==
              SB_ENGINE_STATUS_CONFLICT &&
              legacy_payload.size_bytes == 9,
          "legacy payload did not refuse typed handle before mutation");
  auto legacy_request = BatchRequest();
  sb_engine_row_batch_view_v1_t legacy_batch{};
  Require(sb_engine_result_next_batch(result.get(), &legacy_request,
                                      &legacy_batch) ==
              SB_ENGINE_STATUS_CONFLICT,
          "legacy next_batch did not refuse typed handle");

  auto descriptor_view = DescriptorView();
  Require(result.descriptor(descriptor_view) == SB_ENGINE_STATUS_OK,
          "C++ descriptor wrapper failed");
  Require(descriptor_view.result_descriptor_vector.struct_size ==
                  sizeof(sb_engine_binary_view_v1_t) &&
              descriptor_view.result_descriptor_vector.abi_version ==
                  SB_ENGINE_ABI_VERSION_PACKED &&
              descriptor_view.result_descriptor_vector.size_bytes ==
                  descriptor_bytes.size() &&
              std::equal(descriptor_bytes.begin(), descriptor_bytes.end(),
                         descriptor_view.result_descriptor_vector.bytes) &&
              SameUuid(descriptor_view.row_descriptor_uuid,
                       handle.row_descriptor_uuid) &&
              descriptor_view.row_descriptor_generation ==
                  expected_descriptor.descriptor_generation &&
              std::equal(
                  expected_descriptor.descriptor_evidence_sha256.begin(),
                  expected_descriptor.descriptor_evidence_sha256.end(),
                  std::begin(descriptor_view.descriptor_evidence_sha256)),
          "descriptor view changed canonical bytes or identity");

  auto malformed_view = BatchView();
  malformed_view.reserved_bytes[0] = 1;
  auto request = BatchRequest();
  Require(sb_engine_result_next_typed_batch_v2(
              result.get(), &request, &malformed_view) ==
              SB_ENGINE_STATUS_INVALID_ARGUMENT,
          "typed batch admitted nonzero reserved output bytes");
  auto bounded_view = BatchView();
  auto too_small = BatchRequest(1, 1);
  Require(sb_engine_result_next_typed_batch_v2(
              result.get(), &too_small, &bounded_view) ==
              SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
          "direct packet crossed caller byte bound");

  auto batch_view = BatchView();
  Require(result.next_typed_batch(&request, batch_view) == SB_ENGINE_STATUS_OK,
          "C++ direct typed-batch wrapper failed");
  Require(batch_view.row_count == 1 && batch_view.end_of_stream == 1 &&
              batch_view.cursor_bound == 0 &&
              batch_view.row_data_packet.size_bytes == packet_bytes.size() &&
              std::equal(packet_bytes.begin(), packet_bytes.end(),
                         batch_view.row_data_packet.bytes) &&
              SameUuid(batch_view.execution_uuid, handle.execution_uuid) &&
              SameUuid(batch_view.result_set_uuid, handle.result_set_uuid) &&
              SameUuid(batch_view.row_descriptor_uuid,
                       handle.row_descriptor_uuid) &&
              SameUuid(batch_view.snapshot_uuid, handle.snapshot_uuid) &&
              SameUuid(batch_view.batch_uuid, expected_batch.batch_uuid) &&
              batch_view.batch_ordinal == 0 &&
              ZeroUuid(batch_view.cursor_uuid) &&
              std::equal(expected_batch.batch_evidence_sha256.begin(),
                         expected_batch.batch_evidence_sha256.end(),
                         std::begin(batch_view.batch_evidence_sha256)),
          "direct typed batch changed bytes, UUIDs, evidence, or EOS");
  auto replay = BatchView();
  Require(sb_engine_result_next_typed_batch_v2(result.get(), &request,
                                                &replay) ==
              SB_ENGINE_STATUS_CONFLICT,
          "direct typed result admitted a post-EOS replay");
}

struct CursorControl {
  std::vector<api::TypedResultProducerStageResultV1> stages;
  std::size_t next_stage = 0;
  int statement_releases = 0;
  int snapshot_releases = 0;
  int cancellation_releases = 0;
  int grant_releases = 0;
  int source_closes = 0;
};

class CursorStageLease final
    : public api::TypedResultProducerStageLeaseActionV1 {
 public:
  CursorStageLease(std::shared_ptr<CursorControl> control,
                   std::size_t staged_index)
      : control_(std::move(control)), staged_index_(staged_index) {}

  CommitStatus Commit() noexcept override {
    if (!control_ || control_->next_stage != staged_index_) {
      return CommitStatus::stale;
    }
    ++control_->next_stage;
    return CommitStatus::committed;
  }

  void Abort() noexcept override {}

 private:
  std::shared_ptr<CursorControl> control_;
  std::size_t staged_index_ = 0;
};

class StatementReceipt final : public api::TypedResultStatementReceiptHandleV1 {
 public:
  explicit StatementReceipt(std::shared_ptr<CursorControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerOwnerObservationV1 ObserveOwner(
      const wire::TypedResultUuid&) override {
    return api::TypedResultProducerOwnerObservationV1::authorized;
  }
  api::TypedResultProducerReceiptObservationV1 ObserveReceipt(
      const wire::TypedResultUuid&) override {
    return api::TypedResultProducerReceiptObservationV1::live;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->statement_releases;
  }

 private:
  std::shared_ptr<CursorControl> control_;
};

class SnapshotPin final : public api::TypedResultMgaSnapshotPinHandleV1 {
 public:
  explicit SnapshotPin(std::shared_ptr<CursorControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerMgaObservationV1 ObserveSnapshot(
      const wire::TypedResultUuid&,
      const wire::TypedResultUuid&) override {
    return api::TypedResultProducerMgaObservationV1::live_and_equal;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->snapshot_releases;
  }

 private:
  std::shared_ptr<CursorControl> control_;
};

class CancellationReceipt final
    : public api::TypedResultCancellationReceiptHandleV1 {
 public:
  explicit CancellationReceipt(std::shared_ptr<CursorControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerCancellationObservationV1 ObserveCancellation(
      const wire::TypedResultUuid&,
      std::uint64_t) override {
    return api::TypedResultProducerCancellationObservationV1::live;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->cancellation_releases;
  }

 private:
  std::shared_ptr<CursorControl> control_;
};

class ResourceGrant final
    : public api::TypedResultResourceGrantReceiptHandleV1 {
 public:
  explicit ResourceGrant(std::shared_ptr<CursorControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerGrantObservationV1 ObserveGrant(
      const wire::TypedResultUuid&,
      std::uint64_t,
      std::uint64_t,
      std::uint64_t) override {
    return api::TypedResultProducerGrantObservationV1::live;
  }
  void Release(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->grant_releases;
  }

 private:
  std::shared_ptr<CursorControl> control_;
};

class CursorSource final : public api::TypedResultProducerSourceV1 {
 public:
  explicit CursorSource(std::shared_ptr<CursorControl> control)
      : control_(std::move(control)) {}
  api::TypedResultProducerStageResultV1 Stage(
      const api::TypedResultProducerStageRequestV1&) override {
    if (control_->next_stage >= control_->stages.size()) {
      api::TypedResultProducerStageResultV1 refused;
      refused.outcome = api::TypedResultProducerStageOutcomeV1::refused;
      refused.detail = "fixture_exhausted";
      return refused;
    }
    const auto& staged = control_->stages[control_->next_stage];
    api::TypedResultProducerStageResultV1 result;
    result.outcome = staged.outcome;
    result.end_of_cursor = staged.end_of_cursor;
    result.rows = staged.rows;
    result.detail = staged.detail;
    if (result.outcome == api::TypedResultProducerStageOutcomeV1::batch ||
        result.outcome == api::TypedResultProducerStageOutcomeV1::empty_eos) {
      result.lease = api::TypedResultProducerStageLeaseV1(
          std::make_unique<CursorStageLease>(control_, control_->next_stage));
    } else if (result.outcome ==
               api::TypedResultProducerStageOutcomeV1::empty_open) {
      ++control_->next_stage;
    }
    return result;
  }
  void Close(api::TypedResultProducerReleaseReasonV1) noexcept override {
    ++control_->source_closes;
  }

 private:
  std::shared_ptr<CursorControl> control_;
};

api::TypedResultProducerStageResultV1 EmptyOpen() {
  api::TypedResultProducerStageResultV1 result;
  result.outcome = api::TypedResultProducerStageOutcomeV1::empty_open;
  return result;
}

api::TypedResultProducerStageResultV1 CursorBatch() {
  api::TypedResultProducerStageResultV1 result;
  result.outcome = api::TypedResultProducerStageOutcomeV1::batch;
  result.rows.push_back(Row("cursor;value=2"));
  return result;
}

api::TypedResultProducerStageResultV1 EmptyEos() {
  api::TypedResultProducerStageResultV1 result;
  result.outcome = api::TypedResultProducerStageOutcomeV1::empty_eos;
  result.end_of_cursor = true;
  return result;
}

void CursorBinaryViewAndExactRelease() {
  auto encoded_descriptor = wire::EncodeTypedResultRowDescriptor(Descriptor());
  Require(encoded_descriptor.ok(), "cursor descriptor encode failed");
  const auto descriptor = encoded_descriptor.descriptor;
  const auto handle = QueryHandle(descriptor);
  const auto control = std::make_shared<CursorControl>();
  control->stages.push_back(EmptyOpen());
  control->stages.push_back(CursorBatch());
  control->stages.push_back(EmptyEos());

  api::TypedResultProducerOpenRequestV1 open;
  open.carrier_uuid = Uuid(0x51);
  open.carrier_generation = 13;
  open.cursor_uuid = Uuid(0x52);
  open.session_uuid = Uuid(0x53);
  open.statement_receipt_uuid = Uuid(0x54);
  open.statement_snapshot_uuid = handle.snapshot_uuid;
  open.cancellation_receipt_uuid = Uuid(0x55);
  open.cancellation_generation = 14;
  open.resource_grant_receipt_uuid = Uuid(0x56);
  open.resource_grant_generation = 15;
  open.resource_grant_bytes = 65536;
  open.execution_uuid = handle.execution_uuid;
  open.result_set_uuid = handle.result_set_uuid;
  open.row_descriptor_uuid = handle.row_descriptor_uuid;
  open.row_descriptor_generation = descriptor.descriptor_generation;
  open.snapshot_uuid = handle.snapshot_uuid;
  open.cursor_stream_descriptor_uuid = Uuid(0x57);
  open.cursor_stream_descriptor_version = 1;
  open.cursor_stream_descriptor_generation = 16;
  open.max_chunk_rows = 8;
  open.max_chunk_bytes = 65536;
  open.row_descriptor = descriptor;
  open.descriptor_authority = AuthorityFor(descriptor);
  open.statement_receipt = std::make_unique<StatementReceipt>(control);
  open.mga_snapshot_pin = std::make_unique<SnapshotPin>(control);
  open.cancellation_receipt =
      std::make_unique<CancellationReceipt>(control);
  open.resource_grant_receipt = std::make_unique<ResourceGrant>(control);
  open.producer_state = std::make_unique<CursorSource>(control);
  auto opened = api::OpenTypedResultProducerCursorV1(std::move(open));
  Require(opened.ok(), "producer cursor open failed");
  const auto cursor_snapshot = opened.carrier->Snapshot();

  api::PublicAbiTypedResultAdoptionV1 adoption;
  adoption.request_authority.expected_server_request_uuid = Uuid(0x58);
  adoption.execute_carrier.outcome =
      wire::TypedResultExecuteOutcome::cursor_open;
  adoption.execute_carrier.server_request_uuid = Uuid(0x58);
  adoption.execute_carrier.cursor_uuid = cursor_snapshot.cursor_uuid;
  adoption.execute_carrier.result_descriptor_vector =
      opened.carrier->result_descriptor_vector();
  adoption.execute_carrier.message_vector_set = {0xb1};
  adoption.execute_carrier.cursor_stream_descriptor.descriptor_uuid =
      cursor_snapshot.cursor_stream_descriptor_uuid;
  adoption.execute_carrier.cursor_stream_descriptor.descriptor_version =
      cursor_snapshot.cursor_stream_descriptor_version;
  adoption.execute_carrier.cursor_stream_descriptor.descriptor_generation =
      cursor_snapshot.cursor_stream_descriptor_generation;
  adoption.execute_carrier.cursor_stream_descriptor.maximum_chunk_rows = 8;
  adoption.execute_carrier.cursor_stream_descriptor.maximum_chunk_bytes =
      65536;
  adoption.execute_carrier.query_handle = handle;
  adoption.descriptor_authority = AuthorityFor(descriptor);
  adoption.producer_cursor = std::move(opened.carrier);

  sb_engine_result_t result = nullptr;
  Require(api::AdoptTypedResultPublicAbiV1(std::move(adoption), &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "cursor typed result adoption failed");
  auto request = BatchRequest();

  auto empty_open = BatchView();
  Require(sb_engine_result_next_typed_batch_v2(
              result, &request, &empty_open) == SB_ENGINE_STATUS_OK &&
              empty_open.row_count == 0 && empty_open.end_of_stream == 0 &&
              empty_open.cursor_bound == 1 &&
              empty_open.row_data_packet.bytes == nullptr &&
              empty_open.row_data_packet.size_bytes == 0 &&
              SameUuid(empty_open.execution_uuid, handle.execution_uuid) &&
              SameUuid(empty_open.result_set_uuid, handle.result_set_uuid) &&
              SameUuid(empty_open.row_descriptor_uuid,
                       handle.row_descriptor_uuid) &&
              SameUuid(empty_open.snapshot_uuid, handle.snapshot_uuid) &&
              SameUuid(empty_open.cursor_uuid, cursor_snapshot.cursor_uuid) &&
              SameUuid(empty_open.cursor_stream_descriptor_uuid,
                       cursor_snapshot.cursor_stream_descriptor_uuid) &&
              empty_open.cursor_stream_descriptor_version == 1 &&
              empty_open.cursor_stream_descriptor_generation == 16 &&
              ZeroUuid(empty_open.batch_uuid),
          "empty-open cursor state or four UUIDs drifted");

  auto batch = BatchView();
  Require(sb_engine_result_next_typed_batch_v2(result, &request, &batch) ==
                  SB_ENGINE_STATUS_OK &&
              batch.row_count == 1 && batch.end_of_stream == 0 &&
              batch.cursor_bound == 1 &&
              batch.row_data_packet.bytes != nullptr &&
              batch.row_data_packet.size_bytes >= 224 &&
              std::memcmp(batch.row_data_packet.bytes, "SBTRBT01", 8) == 0 &&
              batch.batch_ordinal == 0 && !ZeroUuid(batch.batch_uuid) &&
              SameUuid(batch.execution_uuid, handle.execution_uuid) &&
              SameUuid(batch.result_set_uuid, handle.result_set_uuid) &&
              SameUuid(batch.row_descriptor_uuid,
                       handle.row_descriptor_uuid) &&
              SameUuid(batch.snapshot_uuid, handle.snapshot_uuid),
          "cursor batch was not one atomic typed packet");

  auto eos = BatchView();
  Require(sb_engine_result_next_typed_batch_v2(result, &request, &eos) ==
                  SB_ENGINE_STATUS_OK &&
              eos.row_count == 0 && eos.end_of_stream == 1 &&
              eos.cursor_bound == 1 && eos.row_data_packet.bytes == nullptr &&
              eos.batch_ordinal == 1 && ZeroUuid(eos.batch_uuid),
          "cursor EOS state drifted or fabricated a packet");
  auto replay = BatchView();
  Require(sb_engine_result_next_typed_batch_v2(result, &request, &replay) ==
              SB_ENGINE_STATUS_CONFLICT,
          "cursor admitted a post-EOS pull");
  Require(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK,
          "cursor result release failed");
  Require(control->statement_releases == 1 &&
              control->snapshot_releases == 1 &&
              control->cancellation_releases == 1 &&
              control->grant_releases == 1 && control->source_closes == 1,
          "cursor authorities were not released exactly once");
}

void MalformedAndLimitRefusalMapping() {
  {
    auto fixture = MakeDirectFixture();
    fixture.adoption.execute_carrier.result_descriptor_vector[96] ^= 0x01;
    sb_engine_result_t result = nullptr;
    Require(api::AdoptTypedResultPublicAbiV1(std::move(fixture.adoption),
                                             &result) ==
                    SB_ENGINE_STATUS_INVALID_ARGUMENT &&
                result == nullptr,
            "descriptor evidence drift did not map to INVALID_ARGUMENT");
  }
  {
    auto fixture = MakeDirectFixture();
    fixture.adoption.request_authority.maximum_row_packet_bytes = 1;
    sb_engine_result_t result = nullptr;
    Require(api::AdoptTypedResultPublicAbiV1(std::move(fixture.adoption),
                                             &result) ==
                    SB_ENGINE_STATUS_RESOURCE_EXHAUSTED &&
                result == nullptr,
            "typed packet limit did not map to RESOURCE_EXHAUSTED");
  }
}

}  // namespace

int main() {
  try {
    DirectBinaryViewAndLegacyConflict();
    CursorBinaryViewAndExactRelease();
    MalformedAndLimitRefusalMapping();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
