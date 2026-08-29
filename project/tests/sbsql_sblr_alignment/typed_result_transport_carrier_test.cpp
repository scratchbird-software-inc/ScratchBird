// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "wire/typed_result_transport_carrier.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace datatypes = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;
namespace wire = scratchbird::wire;
using platform::byte;

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

wire::TypedResultUuid Uuid(byte discriminator) {
  wire::TypedResultUuid uuid{};
  uuid[0] = 0x01;
  uuid[1] = 0x9f;
  uuid[6] = 0x70;
  uuid[8] = 0x80;
  uuid[15] = discriminator;
  return uuid;
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

wire::TypedResultQueryHandleV1 QueryHandle(
    const wire::TypedResultRowDescriptor& descriptor) {
  wire::TypedResultQueryHandleV1 handle;
  handle.execution_uuid = Uuid(0x41);
  handle.result_set_uuid = Uuid(0x42);
  handle.row_descriptor_uuid = descriptor.descriptor_uuid;
  handle.snapshot_uuid = Uuid(0x43);
  return handle;
}

wire::TypedResultCursorStreamDescriptorV1 StreamDescriptor() {
  wire::TypedResultCursorStreamDescriptorV1 descriptor;
  descriptor.descriptor_uuid = Uuid(0x44);
  descriptor.descriptor_version = 1;
  descriptor.descriptor_generation = 5;
  descriptor.maximum_chunk_rows = 4;
  descriptor.maximum_chunk_bytes = 65536;
  return descriptor;
}

wire::TypedResultDescriptorAuthorityValidator AuthorityFor(
    const wire::TypedResultRowDescriptor& expected,
    std::size_t* calls = nullptr) {
  return [expected, calls](const wire::TypedResultRowDescriptor& actual) {
    if (calls != nullptr) ++*calls;
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
        actual.columns.size() == expected.columns.size() &&
        actual.columns[0].descriptor_uuid ==
            expected.columns[0].descriptor_uuid &&
        actual.columns[0].descriptor_generation ==
            expected.columns[0].descriptor_generation &&
        actual.columns[0].type_uuid == expected.columns[0].type_uuid &&
        actual.columns[0].type_generation ==
            expected.columns[0].type_generation &&
        actual.columns[0].codec_id == expected.columns[0].codec_id &&
        actual.columns[0].codec_version == expected.columns[0].codec_version &&
        actual.columns[0].codec_generation ==
            expected.columns[0].codec_generation;
    if (!decision.accepted) {
      decision.diagnostic_code = "DATATYPE.DESCRIPTOR_INVALID";
      decision.detail = "test_registry_tuple_mismatch";
    }
    return decision;
  };
}

wire::TypedResultBatch Batch(
    const wire::TypedResultRowDescriptor& descriptor,
    const wire::TypedResultQueryHandleV1& handle,
    std::uint64_t batch_ordinal,
    bool end_of_rowset,
    bool cursor_bound) {
  wire::TypedResultBatch batch;
  batch.execution_uuid = handle.execution_uuid;
  batch.result_set_uuid = handle.result_set_uuid;
  batch.batch_uuid = Uuid(static_cast<byte>(0x50 + batch_ordinal));
  batch.batch_ordinal = batch_ordinal;
  batch.end_of_rowset = end_of_rowset;
  batch.cursor_bound = cursor_bound;
  batch.row_descriptor_uuid = descriptor.descriptor_uuid;
  batch.row_descriptor_generation = descriptor.descriptor_generation;
  batch.snapshot_uuid = handle.snapshot_uuid;
  if (cursor_bound) batch.cursor_uuid = Uuid(0x45);

  wire::TypedResultCell cell;
  cell.column_ordinal = 0;
  cell.name_occurrence = 0;
  cell.state = wire::TypedResultValueState::value_present;
  const std::string value = "batch=" + std::to_string(batch_ordinal) + ";x=y";
  cell.canonical_payload.assign(value.begin(), value.end());
  wire::TypedResultRow row;
  row.row_ordinal = 0;
  row.cells.push_back(std::move(cell));
  batch.rows.push_back(std::move(row));
  return batch;
}

wire::TypedResultExecuteRequestAuthorityV1 ExecuteAuthority() {
  wire::TypedResultExecuteRequestAuthorityV1 authority;
  authority.expected_server_request_uuid = Uuid(0x46);
  return authority;
}

wire::TypedResultExecuteCarrierV1 CursorOpenCarrier(
    const wire::TypedResultRowDescriptor& descriptor,
    const std::vector<byte>& encoded_descriptor) {
  wire::TypedResultExecuteCarrierV1 carrier;
  carrier.outcome = wire::TypedResultExecuteOutcome::cursor_open;
  carrier.server_request_uuid = Uuid(0x46);
  carrier.transaction_uuid = Uuid(0x47);
  carrier.local_transaction_id = 99;
  carrier.cursor_uuid = Uuid(0x45);
  carrier.result_descriptor_vector = encoded_descriptor;
  carrier.cursor_stream_descriptor = StreamDescriptor();
  carrier.query_handle = QueryHandle(descriptor);
  return carrier;
}

wire::TypedResultCarrierBinding ExecuteBinding(
    const wire::TypedResultQueryHandleV1& handle,
    std::uint64_t rows) {
  wire::TypedResultCarrierBinding binding;
  binding.kind = wire::TypedResultCarrierKind::ps_execute_result_v1;
  binding.row_count = rows;
  binding.end_of_rowset = true;
  binding.execution_uuid = handle.execution_uuid;
  binding.result_set_uuid = handle.result_set_uuid;
  binding.snapshot_uuid = handle.snapshot_uuid;
  return binding;
}

wire::TypedResultCarrierBinding FetchBinding(
    const wire::TypedResultQueryHandleV1& handle,
    const wire::TypedResultCursorStreamDescriptorV1& stream,
    std::uint64_t rows,
    bool end_of_cursor) {
  wire::TypedResultCarrierBinding binding;
  binding.kind = wire::TypedResultCarrierKind::ps_fetch_result_v1;
  binding.row_count = rows;
  binding.end_of_rowset = end_of_cursor;
  binding.execution_uuid = handle.execution_uuid;
  binding.result_set_uuid = handle.result_set_uuid;
  binding.snapshot_uuid = handle.snapshot_uuid;
  binding.cursor_uuid = Uuid(0x45);
  binding.cursor_stream_descriptor_uuid = stream.descriptor_uuid;
  binding.cursor_stream_descriptor_version = stream.descriptor_version;
  binding.cursor_stream_descriptor_generation = stream.descriptor_generation;
  return binding;
}

wire::TypedResultFetchRequestAuthorityV1 FetchAuthority(
    const wire::TypedResultCursorStreamDescriptorV1& stream) {
  wire::TypedResultFetchRequestAuthorityV1 request;
  request.cursor_uuid = Uuid(0x45);
  request.maximum_rows = 2;
  request.maximum_bytes = stream.maximum_chunk_bytes;
  request.timeout_millis = 1000;
  request.direction = wire::TypedResultFetchDirection::forward;
  request.cursor_stream_descriptor_uuid = stream.descriptor_uuid;
  request.cursor_stream_descriptor_version = stream.descriptor_version;
  request.cursor_stream_descriptor_generation = stream.descriptor_generation;
  return request;
}

void ExecuteOutcomeMatrixAndAuthority() {
  const auto source_descriptor = Descriptor();
  const auto encoded =
      wire::EncodeTypedResultRowDescriptor(source_descriptor);
  Require(encoded.ok(), "carrier descriptor fixture did not encode");
  const auto descriptor = encoded.descriptor;
  auto carrier = CursorOpenCarrier(descriptor, encoded.encoded);
  std::size_t authority_calls = 0;
  const auto opened = wire::ValidateTypedResultExecuteCarrierV1(
      ExecuteAuthority(), carrier,
      AuthorityFor(descriptor, &authority_calls));
  Require(opened.ok() && opened.cursor_state.initialized &&
              !opened.cursor_state.terminal && authority_calls == 1,
          "valid cursor-open carrier was rejected");
  Require(opened.cursor_state.encoded_row_descriptor == encoded.encoded &&
              opened.cursor_state.query_handle.row_descriptor_uuid ==
                  descriptor.descriptor_uuid,
          "cursor-open did not retain the exact typed descriptor authority");

  const auto missing_authority = wire::ValidateTypedResultExecuteCarrierV1(
      ExecuteAuthority(), carrier, {});
  Require(!missing_authority.ok() &&
              missing_authority.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "cursor-open admitted an absent datatype registry validator");

  auto forbidden_packet = carrier;
  forbidden_packet.row_data_packet = {0x01};
  authority_calls = 0;
  const auto forbidden = wire::ValidateTypedResultExecuteCarrierV1(
      ExecuteAuthority(), forbidden_packet,
      AuthorityFor(descriptor, &authority_calls));
  Require(!forbidden.ok() && authority_calls == 0 &&
              forbidden.diagnostic_code ==
                  "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
          "cursor-open packet matrix was not refused before descriptor decode");

  auto crossed_request = carrier;
  crossed_request.server_request_uuid = Uuid(0x7f);
  const auto crossed = wire::ValidateTypedResultExecuteCarrierV1(
      ExecuteAuthority(), crossed_request, AuthorityFor(descriptor));
  Require(!crossed.ok() &&
              crossed.diagnostic_code ==
                  "PARSER_SERVER_IPC.CONNECTION_MISMATCH",
          "crossed execute request authority was admitted");

  auto row_batch = Batch(descriptor, QueryHandle(descriptor), 0, true, false);
  const auto encoded_batch = wire::EncodeTypedResultBatch(
      row_batch, descriptor, ExecuteBinding(QueryHandle(descriptor), 1));
  Require(encoded_batch.ok(), "execute row-batch fixture did not encode");
  wire::TypedResultExecuteCarrierV1 row_carrier;
  row_carrier.outcome = wire::TypedResultExecuteOutcome::row_batch;
  row_carrier.server_request_uuid = Uuid(0x46);
  row_carrier.row_count = 1;
  row_carrier.result_descriptor_vector = encoded.encoded;
  row_carrier.row_data_packet = encoded_batch.encoded;
  row_carrier.query_handle = QueryHandle(descriptor);
  const auto row_result = wire::ValidateTypedResultExecuteCarrierV1(
      ExecuteAuthority(), row_carrier, AuthorityFor(descriptor));
  Require(row_result.ok() && row_result.batch.rows.size() == 1,
          "valid execute row-batch carrier was rejected");

  auto count_drift = row_carrier;
  count_drift.row_count = 2;
  Require(!wire::ValidateTypedResultExecuteCarrierV1(
               ExecuteAuthority(), count_drift, AuthorityFor(descriptor))
               .ok(),
          "execute outer row-count drift was admitted");

  wire::TypedResultExecuteCarrierV1 rejected;
  rejected.outcome = wire::TypedResultExecuteOutcome::rejected;
  rejected.server_request_uuid = Uuid(0x46);
  Require(!wire::ValidateTypedResultExecuteCarrierV1(
               ExecuteAuthority(), rejected, AuthorityFor(descriptor))
               .ok(),
          "rejected execute outcome omitted its message vector");
  rejected.message_vector_set = {0x01};
  Require(wire::ValidateTypedResultExecuteCarrierV1(
              ExecuteAuthority(), rejected, AuthorityFor(descriptor))
              .ok(),
          "closed rejected execute outcome matrix was refused");

  auto unknown = rejected;
  unknown.outcome = wire::TypedResultExecuteOutcome::unknown_outcome;
  Require(!wire::ValidateTypedResultExecuteCarrierV1(
               ExecuteAuthority(), unknown, AuthorityFor(descriptor))
               .ok(),
          "unknown execute outcome omitted its finality token");
  unknown.finality_token = {0x02};
  Require(wire::ValidateTypedResultExecuteCarrierV1(
              ExecuteAuthority(), unknown, AuthorityFor(descriptor))
              .ok(),
          "closed unknown execute outcome matrix was refused");

  wire::TypedResultExecuteCarrierV1 complete;
  complete.outcome = wire::TypedResultExecuteOutcome::complete;
  complete.server_request_uuid = Uuid(0x46);
  auto finality_authority = ExecuteAuthority();
  finality_authority.finality_sensitive = true;
  Require(!wire::ValidateTypedResultExecuteCarrierV1(
               finality_authority, complete, AuthorityFor(descriptor))
               .ok(),
          "finality-sensitive completion omitted its token");
  complete.finality_token = {0x03};
  Require(wire::ValidateTypedResultExecuteCarrierV1(
              finality_authority, complete, AuthorityFor(descriptor))
              .ok(),
          "finality-sensitive completion with a token was refused");
}

void CursorFetchSequenceAndAtomicity() {
  const auto encoded_descriptor =
      wire::EncodeTypedResultRowDescriptor(Descriptor());
  Require(encoded_descriptor.ok(), "cursor descriptor fixture did not encode");
  const auto descriptor = encoded_descriptor.descriptor;
  const auto opened = wire::ValidateTypedResultExecuteCarrierV1(
      ExecuteAuthority(),
      CursorOpenCarrier(descriptor, encoded_descriptor.encoded),
      AuthorityFor(descriptor));
  Require(opened.ok(), "cursor state setup failed");
  auto state = opened.cursor_state;
  const auto request = FetchAuthority(state.cursor_stream_descriptor);

  const auto first_batch =
      Batch(descriptor, state.query_handle, 0, false, true);
  const auto first_encoded = wire::EncodeTypedResultBatch(
      first_batch, descriptor,
      FetchBinding(state.query_handle, state.cursor_stream_descriptor, 1,
                   false));
  Require(first_encoded.ok(), "first fetch batch fixture did not encode");
  wire::TypedResultFetchCarrierV1 first;
  first.cursor_uuid = state.cursor_uuid;
  first.row_count = 1;
  first.row_data_packet = first_encoded.encoded;
  const auto first_result = wire::ValidateTypedResultFetchCarrierV1(
      request, first, state);
  Require(first_result.ok() &&
              first_result.cursor_state.batch_state.next_batch_ordinal == 1 &&
              !first_result.cursor_state.terminal,
          "first cursor batch did not initialize exact sequence state");
  state = first_result.cursor_state;

  wire::TypedResultFetchCarrierV1 empty_poll;
  empty_poll.cursor_uuid = state.cursor_uuid;
  const auto polled = wire::ValidateTypedResultFetchCarrierV1(
      request, empty_poll, state);
  Require(polled.ok() &&
              polled.cursor_state.batch_state.next_batch_ordinal == 1 &&
              !polled.cursor_state.terminal,
          "empty nonterminal fetch advanced typed batch sequence");
  state = polled.cursor_state;

  const auto skipped_batch =
      Batch(descriptor, state.query_handle, 2, true, true);
  const auto skipped_encoded = wire::EncodeTypedResultBatch(
      skipped_batch, descriptor,
      FetchBinding(state.query_handle, state.cursor_stream_descriptor, 1,
                   true));
  Require(skipped_encoded.ok(), "skipped cursor batch fixture did not encode");
  auto skipped = first;
  skipped.row_data_packet = skipped_encoded.encoded;
  skipped.end_of_cursor = true;
  const auto refused_skip = wire::ValidateTypedResultFetchCarrierV1(
      request, skipped, state);
  Require(!refused_skip.ok() &&
              refused_skip.diagnostic_code ==
                  "PARSER_SERVER_IPC.SEQUENCE_INVALID" &&
              refused_skip.cursor_state.batch_state.next_batch_ordinal == 1 &&
              !refused_skip.cursor_state.terminal,
          "skipped cursor batch mutated live sequence state");

  const auto second_batch =
      Batch(descriptor, state.query_handle, 1, true, true);
  const auto second_encoded = wire::EncodeTypedResultBatch(
      second_batch, descriptor,
      FetchBinding(state.query_handle, state.cursor_stream_descriptor, 1,
                   true));
  Require(second_encoded.ok(), "terminal cursor batch fixture did not encode");
  auto second = first;
  second.row_data_packet = second_encoded.encoded;
  second.end_of_cursor = true;
  const auto terminal = wire::ValidateTypedResultFetchCarrierV1(
      request, second, state);
  Require(terminal.ok() && terminal.cursor_state.terminal &&
              terminal.cursor_state.batch_state.terminal,
          "terminal cursor batch did not close typed sequence state");
  Require(!wire::ValidateTypedResultFetchCarrierV1(
               request, second, terminal.cursor_state)
               .ok(),
          "post-terminal cursor packet was admitted");

  auto descriptor_drift = request;
  ++descriptor_drift.cursor_stream_descriptor_generation;
  const auto descriptor_refusal = wire::ValidateTypedResultFetchCarrierV1(
      descriptor_drift, first, opened.cursor_state);
  Require(!descriptor_refusal.ok() &&
              descriptor_refusal.diagnostic_code ==
                  "PARSER_SERVER_IPC.CONNECTION_MISMATCH",
          "fetch stream-descriptor generation drift was admitted");

  auto bounded_request = request;
  bounded_request.maximum_bytes = first.row_data_packet.size() - 1;
  const auto bounded = wire::ValidateTypedResultFetchCarrierV1(
      bounded_request, first, opened.cursor_state);
  Require(!bounded.ok() &&
              bounded.diagnostic_code ==
                  "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
          "fetch packet exceeded its admitted byte bound");

  auto count_drift = first;
  count_drift.row_count = 2;
  Require(!wire::ValidateTypedResultFetchCarrierV1(
               request, count_drift, opened.cursor_state)
               .ok(),
          "fetch outer row-count drift was admitted");

  auto empty_terminal = empty_poll;
  empty_terminal.end_of_cursor = true;
  const auto empty_closed = wire::ValidateTypedResultFetchCarrierV1(
      request, empty_terminal, opened.cursor_state);
  Require(empty_closed.ok() && empty_closed.cursor_state.terminal &&
              !empty_closed.cursor_state.batch_state.initialized,
          "empty terminal fetch changed batch ordinal or remained live");
}

}  // namespace

int main() {
  try {
    ExecuteOutcomeMatrixAndAuthority();
    CursorFetchSequenceAndAtomicity();
  } catch (const std::exception& error) {
    std::cerr << "typed result transport carrier test failed: "
              << error.what() << '\n';
    return 1;
  }
  return 0;
}
