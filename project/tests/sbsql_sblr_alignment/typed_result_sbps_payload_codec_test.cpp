// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "server/sbps.hpp"
#include "wire/parser_server_ipc/typed_result_payload_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace datatypes = scratchbird::core::datatypes;
namespace ipc = scratchbird::parser::ipc;
namespace sbps = scratchbird::server::sbps;
namespace wire = scratchbird::wire;
using scratchbird::core::platform::byte;

static_assert(ipc::kPsExecuteResultV1SchemaId == 1043);
static_assert(ipc::kPsFetchResultV1SchemaId == 1045);
static_assert(sbps::kSchemaPsExecuteResultV1 ==
              ipc::kPsExecuteResultV1SchemaId);
static_assert(sbps::kSchemaPsFetchResultV1 ==
              ipc::kPsFetchResultV1SchemaId);

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

wire::TypedResultUuid Uuid(byte discriminator) {
  wire::TypedResultUuid uuid{};
  uuid[0] = 0x01;
  uuid[1] = 0xa0;
  uuid[6] = 0x70;
  uuid[8] = 0x80;
  uuid[15] = discriminator;
  return uuid;
}

wire::TypedResultRowDescriptor Descriptor() {
  wire::TypedResultRowDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(0x31);
  descriptor.descriptor_generation = 17;
  descriptor.datatype_catalog_snapshot_uuid = Uuid(0x32);
  descriptor.datatype_catalog_generation = 18;
  descriptor.datatype_registry_generation = 19;
  for (std::uint32_t ordinal = 0; ordinal < 3; ++ordinal) {
    wire::TypedResultColumnDescriptor column;
    column.ordinal = ordinal;
    column.name_occurrence = ordinal;
    column.name = "payload";
    column.nullability = wire::TypedResultNullability::nullable;
    column.descriptor_uuid = Uuid(static_cast<byte>(0x40 + ordinal));
    column.descriptor_generation = 20 + ordinal;
    column.type_uuid = Uuid(static_cast<byte>(0x50 + ordinal));
    column.type_generation = 30 + ordinal;
    column.canonical_type_id = datatypes::CanonicalTypeId::character;
    column.codec_id = "datatype.character.utf8.v1";
    column.codec_version = 1;
    column.codec_generation = 40 + ordinal;
    descriptor.columns.push_back(std::move(column));
  }
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
        actual.columns.size() == 3 && actual.columns[0].ordinal == 0 &&
        actual.columns[1].ordinal == 1 && actual.columns[2].ordinal == 2 &&
        actual.columns[0].name == "payload" &&
        actual.columns[1].name == "payload" &&
        actual.columns[2].name == "payload" &&
        actual.columns[0].name_occurrence == 0 &&
        actual.columns[1].name_occurrence == 1 &&
        actual.columns[2].name_occurrence == 2;
    if (!decision.accepted) {
      decision.diagnostic_code = "DATATYPE.DESCRIPTOR_INVALID";
      decision.detail = "typed_result_sbps_test_descriptor_mismatch";
    }
    return decision;
  };
}

wire::TypedResultQueryHandleV1 QueryHandle(
    const wire::TypedResultRowDescriptor& descriptor) {
  wire::TypedResultQueryHandleV1 handle;
  handle.execution_uuid = Uuid(0x61);
  handle.result_set_uuid = Uuid(0x62);
  handle.row_descriptor_uuid = descriptor.descriptor_uuid;
  handle.snapshot_uuid = Uuid(0x63);
  return handle;
}

wire::TypedResultCursorStreamDescriptorV1 StreamDescriptor() {
  wire::TypedResultCursorStreamDescriptorV1 descriptor;
  descriptor.descriptor_uuid = Uuid(0x64);
  descriptor.descriptor_version = 1;
  descriptor.descriptor_generation = 21;
  descriptor.maximum_chunk_rows = 8;
  descriptor.maximum_chunk_bytes = 65536;
  return descriptor;
}

wire::TypedResultExecuteRequestAuthorityV1 ExecuteAuthority() {
  wire::TypedResultExecuteRequestAuthorityV1 authority;
  authority.expected_server_request_uuid = Uuid(0x65);
  return authority;
}

wire::TypedResultCarrierBinding ExecuteBinding(
    const wire::TypedResultQueryHandleV1& handle) {
  wire::TypedResultCarrierBinding binding;
  binding.kind = wire::TypedResultCarrierKind::ps_execute_result_v1;
  binding.row_count = 1;
  binding.end_of_rowset = true;
  binding.execution_uuid = handle.execution_uuid;
  binding.result_set_uuid = handle.result_set_uuid;
  binding.snapshot_uuid = handle.snapshot_uuid;
  return binding;
}

wire::TypedResultCarrierBinding FetchBinding(
    const wire::TypedResultCursorCarrierStateV1& state,
    bool end_of_cursor) {
  wire::TypedResultCarrierBinding binding;
  binding.kind = wire::TypedResultCarrierKind::ps_fetch_result_v1;
  binding.row_count = 1;
  binding.end_of_rowset = end_of_cursor;
  binding.execution_uuid = state.query_handle.execution_uuid;
  binding.result_set_uuid = state.query_handle.result_set_uuid;
  binding.snapshot_uuid = state.query_handle.snapshot_uuid;
  binding.cursor_uuid = state.cursor_uuid;
  binding.cursor_stream_descriptor_uuid =
      state.cursor_stream_descriptor.descriptor_uuid;
  binding.cursor_stream_descriptor_version =
      state.cursor_stream_descriptor.descriptor_version;
  binding.cursor_stream_descriptor_generation =
      state.cursor_stream_descriptor.descriptor_generation;
  return binding;
}

wire::TypedResultBatch DirectBatch(
    const wire::TypedResultRowDescriptor& descriptor,
    const wire::TypedResultQueryHandleV1& handle) {
  wire::TypedResultBatch batch;
  batch.execution_uuid = handle.execution_uuid;
  batch.result_set_uuid = handle.result_set_uuid;
  batch.batch_uuid = Uuid(0x66);
  batch.batch_ordinal = 0;
  batch.end_of_rowset = true;
  batch.row_descriptor_uuid = descriptor.descriptor_uuid;
  batch.row_descriptor_generation = descriptor.descriptor_generation;
  batch.snapshot_uuid = handle.snapshot_uuid;

  wire::TypedResultRow row;
  row.row_ordinal = 0;
  wire::TypedResultCell delimiter_value;
  delimiter_value.column_ordinal = 0;
  delimiter_value.name_occurrence = 0;
  const std::string delimiter_text = "semi;equals=payload=left=right";
  delimiter_value.canonical_payload.assign(delimiter_text.begin(),
                                           delimiter_text.end());
  row.cells.push_back(std::move(delimiter_value));

  wire::TypedResultCell empty_value;
  empty_value.column_ordinal = 1;
  empty_value.name_occurrence = 1;
  empty_value.state = wire::TypedResultValueState::value_present;
  row.cells.push_back(std::move(empty_value));

  wire::TypedResultCell null_value;
  null_value.column_ordinal = 2;
  null_value.name_occurrence = 2;
  null_value.state = wire::TypedResultValueState::sql_null;
  row.cells.push_back(std::move(null_value));
  batch.rows.push_back(std::move(row));
  return batch;
}

wire::TypedResultBatch CursorBatch(
    const wire::TypedResultCursorCarrierStateV1& state,
    bool end_of_cursor) {
  auto batch = DirectBatch(state.row_descriptor, state.query_handle);
  batch.batch_uuid = Uuid(0x67);
  batch.batch_ordinal = state.batch_state.next_batch_ordinal;
  batch.end_of_rowset = end_of_cursor;
  batch.cursor_bound = true;
  batch.cursor_uuid = state.cursor_uuid;
  return batch;
}

wire::TypedResultFetchRequestAuthorityV1 FetchAuthority(
    const wire::TypedResultCursorCarrierStateV1& state) {
  wire::TypedResultFetchRequestAuthorityV1 authority;
  authority.cursor_uuid = state.cursor_uuid;
  authority.maximum_rows = state.cursor_stream_descriptor.maximum_chunk_rows;
  authority.maximum_bytes = state.cursor_stream_descriptor.maximum_chunk_bytes;
  authority.timeout_millis = 1000;
  authority.direction = wire::TypedResultFetchDirection::forward;
  authority.cursor_stream_descriptor_uuid =
      state.cursor_stream_descriptor.descriptor_uuid;
  authority.cursor_stream_descriptor_version =
      state.cursor_stream_descriptor.descriptor_version;
  authority.cursor_stream_descriptor_generation =
      state.cursor_stream_descriptor.descriptor_generation;
  return authority;
}

std::uint16_t LoadU16(const std::vector<byte>& payload, std::size_t offset) {
  return static_cast<std::uint16_t>(payload[offset]) |
         (static_cast<std::uint16_t>(payload[offset + 1]) << 8u);
}

std::uint32_t LoadU32(const std::vector<byte>& payload, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(payload[offset + shift / 8]) << shift;
  }
  return value;
}

std::optional<std::size_t> FieldHeaderOffset(const std::vector<byte>& payload,
                                             std::uint16_t wanted) {
  if (payload.size() < 2) return std::nullopt;
  std::size_t offset = 2;
  while (offset + 6 <= payload.size()) {
    const auto id = LoadU16(payload, offset);
    const auto length = LoadU32(payload, offset + 2);
    if (id == wanted) return offset;
    offset += 6;
    if (length > payload.size() - offset) return std::nullopt;
    offset += length;
  }
  return std::nullopt;
}

void StoreU16(std::vector<byte>* payload,
              std::size_t offset,
              std::uint16_t value) {
  (*payload)[offset] = static_cast<byte>(value & 0xffu);
  (*payload)[offset + 1] = static_cast<byte>((value >> 8u) & 0xffu);
}

void StoreU64(std::vector<byte>* payload,
              std::size_t offset,
              std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    (*payload)[offset + shift / 8] =
        static_cast<byte>((value >> shift) & 0xffu);
  }
}

bool SameCursorState(const wire::TypedResultCursorCarrierStateV1& left,
                     const wire::TypedResultCursorCarrierStateV1& right) {
  return left.initialized == right.initialized &&
         left.terminal == right.terminal && left.cursor_uuid == right.cursor_uuid &&
         left.cursor_stream_descriptor.descriptor_uuid ==
             right.cursor_stream_descriptor.descriptor_uuid &&
         left.cursor_stream_descriptor.descriptor_version ==
             right.cursor_stream_descriptor.descriptor_version &&
         left.cursor_stream_descriptor.descriptor_generation ==
             right.cursor_stream_descriptor.descriptor_generation &&
         left.cursor_stream_descriptor.maximum_chunk_rows ==
             right.cursor_stream_descriptor.maximum_chunk_rows &&
         left.cursor_stream_descriptor.maximum_chunk_bytes ==
             right.cursor_stream_descriptor.maximum_chunk_bytes &&
         left.query_handle.execution_uuid == right.query_handle.execution_uuid &&
         left.query_handle.result_set_uuid == right.query_handle.result_set_uuid &&
         left.query_handle.row_descriptor_uuid ==
             right.query_handle.row_descriptor_uuid &&
         left.query_handle.snapshot_uuid == right.query_handle.snapshot_uuid &&
         left.encoded_row_descriptor == right.encoded_row_descriptor &&
         left.batch_state.initialized == right.batch_state.initialized &&
         left.batch_state.terminal == right.batch_state.terminal &&
         left.batch_state.next_batch_ordinal ==
             right.batch_state.next_batch_ordinal &&
         left.batch_state.seen_batch_uuids ==
             right.batch_state.seen_batch_uuids;
}

struct Fixture {
  wire::TypedResultRowDescriptor descriptor;
  std::vector<byte> encoded_descriptor;
  wire::TypedResultQueryHandleV1 query_handle;
};

Fixture MakeFixture() {
  const auto encoded = wire::EncodeTypedResultRowDescriptor(Descriptor());
  Require(encoded.ok(), "typed result descriptor fixture did not encode");
  return {encoded.descriptor, encoded.encoded, QueryHandle(encoded.descriptor)};
}

void DirectRoundTripAndOpaqueCellPreservation() {
  const auto fixture = MakeFixture();
  const auto batch = DirectBatch(fixture.descriptor, fixture.query_handle);
  const auto encoded_batch = wire::EncodeTypedResultBatch(
      batch, fixture.descriptor, ExecuteBinding(fixture.query_handle));
  Require(encoded_batch.ok(), "direct typed batch fixture did not encode");

  wire::TypedResultExecuteCarrierV1 carrier;
  carrier.outcome = wire::TypedResultExecuteOutcome::row_batch;
  carrier.server_request_uuid = ExecuteAuthority().expected_server_request_uuid;
  carrier.row_count = 1;
  carrier.result_descriptor_vector = fixture.encoded_descriptor;
  carrier.row_data_packet = encoded_batch.encoded;
  carrier.query_handle = fixture.query_handle;

  const auto encoded = ipc::EncodeAndValidatePsExecuteResultV1Payload(
      ExecuteAuthority(), carrier, AuthorityFor(fixture.descriptor));
  Require(encoded.ok() && !encoded.canonical_payload.empty(),
          "valid ps_execute_result_v1 did not encode");
  const auto decoded = ipc::DecodeAndValidatePsExecuteResultV1Payload(
      encoded.canonical_payload, ExecuteAuthority(),
      AuthorityFor(fixture.descriptor));
  Require(decoded.ok() &&
              decoded.canonical_payload == encoded.canonical_payload,
          "ps_execute_result_v1 did not round trip byte exactly");
  Require(decoded.carrier.query_handle.execution_uuid ==
                  fixture.query_handle.execution_uuid &&
              decoded.carrier.query_handle.result_set_uuid ==
                  fixture.query_handle.result_set_uuid &&
              decoded.carrier.query_handle.row_descriptor_uuid ==
                  fixture.query_handle.row_descriptor_uuid &&
              decoded.carrier.query_handle.snapshot_uuid ==
                  fixture.query_handle.snapshot_uuid,
          "ps_execute_result_v1 lost four-UUID authority");
  Require(decoded.validated.batch.batch_ordinal == 0 &&
              decoded.validated.batch.end_of_rowset &&
              !decoded.validated.batch.cursor_bound &&
              decoded.validated.batch.rows.size() == 1,
          "direct result was not one atomic non-cursor EOS batch");
  const auto& cells = decoded.validated.batch.rows[0].cells;
  Require(cells.size() == 3 && cells[0].name_occurrence == 0 &&
              cells[1].name_occurrence == 1 &&
              cells[2].name_occurrence == 2,
          "duplicate output-name occurrence identity was not preserved");
  const std::string first(cells[0].canonical_payload.begin(),
                          cells[0].canonical_payload.end());
  Require(first == "semi;equals=payload=left=right",
          "semicolon/equals value was interpreted as a delimiter payload");
  Require(cells[1].state == wire::TypedResultValueState::value_present &&
              cells[1].canonical_payload.empty() &&
              cells[2].state == wire::TypedResultValueState::sql_null &&
              cells[2].canonical_payload.empty(),
          "empty value and SQL NULL were conflated");

  auto row_count_drift = encoded.canonical_payload;
  const auto row_count_field = FieldHeaderOffset(row_count_drift, 6);
  Require(row_count_field.has_value(), "row-count field not found");
  StoreU64(&row_count_drift, *row_count_field + 6, 2);
  Require(!ipc::DecodeAndValidatePsExecuteResultV1Payload(
               row_count_drift, ExecuteAuthority(),
               AuthorityFor(fixture.descriptor))
               .ok(),
          "outer/inner direct row-count drift was admitted");

  auto execution_drift = encoded.canonical_payload;
  const auto execution_field = FieldHeaderOffset(execution_drift, 16);
  Require(execution_field.has_value(), "execution UUID field not found");
  execution_drift[*execution_field + 6] ^= 0x01;
  Require(!ipc::DecodeAndValidatePsExecuteResultV1Payload(
               execution_drift, ExecuteAuthority(),
               AuthorityFor(fixture.descriptor))
               .ok(),
          "outer/inner four-UUID drift was admitted");
}

wire::TypedResultCursorCarrierStateV1 CursorOpenState(const Fixture& fixture,
                                                       std::vector<byte>* payload) {
  wire::TypedResultExecuteCarrierV1 carrier;
  carrier.outcome = wire::TypedResultExecuteOutcome::cursor_open;
  carrier.server_request_uuid = ExecuteAuthority().expected_server_request_uuid;
  carrier.transaction_uuid = Uuid(0x68);
  carrier.local_transaction_id = 99;
  carrier.cursor_uuid = Uuid(0x69);
  carrier.result_descriptor_vector = fixture.encoded_descriptor;
  carrier.cursor_stream_descriptor = StreamDescriptor();
  carrier.query_handle = fixture.query_handle;
  const auto encoded = ipc::EncodeAndValidatePsExecuteResultV1Payload(
      ExecuteAuthority(), carrier, AuthorityFor(fixture.descriptor));
  Require(encoded.ok(), "valid cursor-open payload did not encode");
  const auto decoded = ipc::DecodeAndValidatePsExecuteResultV1Payload(
      encoded.canonical_payload, ExecuteAuthority(),
      AuthorityFor(fixture.descriptor));
  Require(decoded.ok() && decoded.carrier.row_data_packet.empty() &&
              decoded.carrier.row_count == 0 &&
              decoded.validated.cursor_state.initialized,
          "cursor-open carried eager rows or lost typed cursor state");
  if (payload != nullptr) *payload = encoded.canonical_payload;
  return decoded.validated.cursor_state;
}

void CursorFetchRoundTripSequenceAndAtomicity() {
  const auto fixture = MakeFixture();
  std::vector<byte> cursor_open_payload;
  const auto initial_state = CursorOpenState(fixture, &cursor_open_payload);
  const auto request = FetchAuthority(initial_state);
  const auto batch = CursorBatch(initial_state, true);
  const auto encoded_batch = wire::EncodeTypedResultBatch(
      batch, initial_state.row_descriptor, FetchBinding(initial_state, true));
  Require(encoded_batch.ok(), "cursor-bound typed batch did not encode");

  wire::TypedResultFetchCarrierV1 carrier;
  carrier.cursor_uuid = initial_state.cursor_uuid;
  carrier.row_count = 1;
  carrier.row_data_packet = encoded_batch.encoded;
  carrier.end_of_cursor = true;
  const auto encoded = ipc::EncodeAndValidatePsFetchResultV1Payload(
      request, carrier, initial_state);
  Require(encoded.ok(), "valid ps_fetch_result_v1 did not encode");
  const auto decoded = ipc::DecodeAndValidatePsFetchResultV1Payload(
      encoded.canonical_payload, request, initial_state);
  Require(decoded.ok() && decoded.validated.cursor_state.terminal &&
              decoded.validated.cursor_state.batch_state.terminal &&
              decoded.validated.cursor_state.batch_state.next_batch_ordinal == 1,
          "cursor-bound EOS batch did not advance atomically to terminal");
  Require(!ipc::DecodeAndValidatePsFetchResultV1Payload(
               encoded.canonical_payload, request,
               decoded.validated.cursor_state)
               .ok(),
          "terminal cursor batch replay was admitted");

  auto eos_drift = encoded.canonical_payload;
  const auto eos_field = FieldHeaderOffset(eos_drift, 4);
  Require(eos_field.has_value(), "fetch EOS field not found");
  eos_drift[*eos_field + 6] = 0;
  const auto prior_copy = initial_state;
  const auto refused_eos = ipc::DecodeAndValidatePsFetchResultV1Payload(
      eos_drift, request, initial_state);
  Require(!refused_eos.ok() && SameCursorState(initial_state, prior_copy) &&
              refused_eos.canonical_payload.empty() &&
              refused_eos.carrier.row_data_packet.empty(),
          "fetch EOS drift partially exposed rows or mutated cursor state");

  auto cursor_drift = encoded.canonical_payload;
  const auto cursor_field = FieldHeaderOffset(cursor_drift, 1);
  Require(cursor_field.has_value(), "fetch cursor field not found");
  cursor_drift[*cursor_field + 6] ^= 0x01;
  Require(!ipc::DecodeAndValidatePsFetchResultV1Payload(
               cursor_drift, request, initial_state)
               .ok(),
          "cross-cursor fetch result was admitted");
}

void OuterTlvRefusalsAreClosed() {
  const auto fixture = MakeFixture();
  const auto batch = DirectBatch(fixture.descriptor, fixture.query_handle);
  const auto encoded_batch = wire::EncodeTypedResultBatch(
      batch, fixture.descriptor, ExecuteBinding(fixture.query_handle));
  Require(encoded_batch.ok(), "outer-TLV batch fixture did not encode");
  wire::TypedResultExecuteCarrierV1 carrier;
  carrier.outcome = wire::TypedResultExecuteOutcome::row_batch;
  carrier.server_request_uuid = ExecuteAuthority().expected_server_request_uuid;
  carrier.row_count = 1;
  carrier.result_descriptor_vector = fixture.encoded_descriptor;
  carrier.row_data_packet = encoded_batch.encoded;
  carrier.query_handle = fixture.query_handle;
  const auto encoded = ipc::EncodeAndValidatePsExecuteResultV1Payload(
      ExecuteAuthority(), carrier, AuthorityFor(fixture.descriptor));
  Require(encoded.ok(), "outer-TLV fixture did not encode");

  auto future_revision = encoded.canonical_payload;
  StoreU16(&future_revision, 0, 2);
  Require(!ipc::DecodeAndValidatePsExecuteResultV1Payload(
               future_revision, ExecuteAuthority(),
               AuthorityFor(fixture.descriptor))
               .ok(),
          "future TLV layout revision was admitted");

  const auto field_two = FieldHeaderOffset(encoded.canonical_payload, 2);
  Require(field_two.has_value(), "second TLV field not found");
  auto duplicate = encoded.canonical_payload;
  StoreU16(&duplicate, *field_two, 1);
  Require(!ipc::DecodeAndValidatePsExecuteResultV1Payload(
               duplicate, ExecuteAuthority(), AuthorityFor(fixture.descriptor))
               .ok(),
          "duplicate/out-of-order TLV field was admitted");

  auto unknown = encoded.canonical_payload;
  StoreU16(&unknown, *field_two, 20);
  Require(!ipc::DecodeAndValidatePsExecuteResultV1Payload(
               unknown, ExecuteAuthority(), AuthorityFor(fixture.descriptor))
               .ok(),
          "unknown TLV field was admitted");

  auto missing = encoded.canonical_payload;
  missing.resize(missing.size() - 22);
  Require(!ipc::DecodeAndValidatePsExecuteResultV1Payload(
               missing, ExecuteAuthority(), AuthorityFor(fixture.descriptor))
               .ok(),
          "missing required TLV field was admitted");

  auto trailing = encoded.canonical_payload;
  trailing.push_back(0);
  Require(!ipc::DecodeAndValidatePsExecuteResultV1Payload(
               trailing, ExecuteAuthority(), AuthorityFor(fixture.descriptor))
               .ok(),
          "trailing TLV bytes were admitted");
}

}  // namespace

int main() {
  try {
    DirectRoundTripAndOpaqueCellPreservation();
    CursorFetchRoundTripSequenceAndAtomicity();
    OuterTlvRefusalsAreClosed();
  } catch (const std::exception& error) {
    std::cerr << "typed_result_sbps_payload_codec_test failed: "
              << error.what() << '\n';
    return 1;
  }
  std::cout << "typed_result_sbps_payload_codec_test passed\n";
  return 0;
}
