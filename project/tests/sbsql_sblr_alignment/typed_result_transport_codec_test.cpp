// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "core/hash/hash_digest.hpp"
#include "wire/typed_result_transport_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace wire = scratchbird::wire;
namespace datatypes = scratchbird::core::datatypes;
namespace core_hash = scratchbird::core::hash;
namespace platform = scratchbird::core::platform;
using platform::byte;

constexpr std::size_t kDescriptorEvidenceOffset = 96;
constexpr std::size_t kBatchEvidenceOffset = 192;
constexpr std::string_view kDescriptorDomain =
    "ScratchBird.PsResultDescriptorVector.V1";
constexpr std::string_view kBatchDomain =
    "ScratchBird.PsRowDataPacket.V1";

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

wire::TypedResultUuid Uuid(byte discriminator) {
  wire::TypedResultUuid uuid{};
  uuid[0] = 0x01;
  uuid[1] = 0x9d;
  uuid[6] = 0x70;
  uuid[8] = 0x80;
  uuid[15] = discriminator;
  return uuid;
}

std::vector<byte> Bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

wire::TypedResultEvidenceHash IndependentEvidence(
    std::string_view domain,
    const std::vector<byte>& zeroed_canonical_bytes) {
  std::vector<byte> material;
  material.reserve(domain.size() + zeroed_canonical_bytes.size());
  material.insert(material.end(), domain.begin(), domain.end());
  material.insert(material.end(), zeroed_canonical_bytes.begin(),
                  zeroed_canonical_bytes.end());
  const auto digest = core_hash::ComputeSha256Digest(material);
  Require(digest.ok(), "independent SHA-256 oracle failed");
  wire::TypedResultEvidenceHash evidence{};
  std::copy(digest.digest.begin(), digest.digest.end(), evidence.begin());
  return evidence;
}

wire::TypedResultEvidenceHash EvidenceAt(const std::vector<byte>& encoded,
                                         std::size_t offset) {
  Require(offset + wire::kTypedResultEvidenceHashBytes <= encoded.size(),
          "evidence offset outside frame");
  wire::TypedResultEvidenceHash evidence{};
  std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
              evidence.size(), evidence.begin());
  return evidence;
}

void Rehash(std::vector<byte>* encoded,
            std::size_t offset,
            std::string_view domain) {
  std::fill(encoded->begin() + static_cast<std::ptrdiff_t>(offset),
            encoded->begin() + static_cast<std::ptrdiff_t>(
                                   offset + wire::kTypedResultEvidenceHashBytes),
            0);
  const auto evidence = IndependentEvidence(domain, *encoded);
  std::copy(evidence.begin(), evidence.end(),
            encoded->begin() + static_cast<std::ptrdiff_t>(offset));
}

wire::TypedResultColumnDescriptor TextColumn(std::uint32_t ordinal,
                                             std::string name,
                                             std::uint32_t occurrence,
                                             byte identity) {
  wire::TypedResultColumnDescriptor column;
  column.ordinal = ordinal;
  column.name_occurrence = occurrence;
  column.name = std::move(name);
  column.nullability = wire::TypedResultNullability::nullable;
  column.descriptor_uuid = Uuid(identity);
  column.descriptor_generation = 1000u + identity;
  column.type_uuid = Uuid(static_cast<byte>(identity + 0x20u));
  column.type_generation = 2000u + identity;
  column.canonical_type_id = datatypes::CanonicalTypeId::character;
  column.codec_id = "datatype.character.utf8.v1";
  column.codec_version = 1;
  column.codec_generation = 3000u + identity;
  column.canonical_value_bytes = 0;
  return column;
}

wire::TypedResultColumnDescriptor Int128Column(std::uint32_t ordinal,
                                               std::string name,
                                               std::uint32_t occurrence,
                                               byte identity) {
  wire::TypedResultColumnDescriptor column;
  column.ordinal = ordinal;
  column.name_occurrence = occurrence;
  column.name = std::move(name);
  column.nullability = wire::TypedResultNullability::nullable;
  column.descriptor_uuid = Uuid(identity);
  column.descriptor_generation = 4000u + identity;
  column.type_uuid = Uuid(static_cast<byte>(identity + 0x20u));
  column.type_generation = 5000u + identity;
  column.canonical_type_id = datatypes::CanonicalTypeId::int128;
  column.codec_id = "datatype.int128.le.v1";
  column.codec_version = 1;
  column.codec_generation = 6000u + identity;
  column.canonical_value_bytes = 16;
  return column;
}

wire::TypedResultRowDescriptor Descriptor(
    std::vector<wire::TypedResultColumnDescriptor> columns,
    byte identity = 0x10,
    std::uint64_t generation = 77) {
  wire::TypedResultRowDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(identity);
  descriptor.descriptor_generation = generation;
  descriptor.datatype_catalog_snapshot_uuid = Uuid(0x70);
  descriptor.datatype_catalog_generation = 81;
  descriptor.datatype_registry_generation = 82;
  descriptor.columns = std::move(columns);
  return descriptor;
}

wire::TypedResultCell Present(std::uint32_t ordinal,
                              std::uint32_t occurrence,
                              std::vector<byte> payload) {
  wire::TypedResultCell cell;
  cell.column_ordinal = ordinal;
  cell.name_occurrence = occurrence;
  cell.state = wire::TypedResultValueState::value_present;
  cell.canonical_payload = std::move(payload);
  return cell;
}

wire::TypedResultCell Null(std::uint32_t ordinal,
                           std::uint32_t occurrence) {
  wire::TypedResultCell cell;
  cell.column_ordinal = ordinal;
  cell.name_occurrence = occurrence;
  cell.state = wire::TypedResultValueState::sql_null;
  return cell;
}

wire::TypedResultRow Row(
    std::uint64_t ordinal,
    std::vector<wire::TypedResultCell> cells) {
  wire::TypedResultRow row;
  row.row_ordinal = ordinal;
  row.cells = std::move(cells);
  return row;
}

wire::TypedResultBatch Batch(
    const wire::TypedResultRowDescriptor& descriptor,
    std::vector<wire::TypedResultRow> rows,
    std::uint64_t batch_ordinal = 0,
    bool end_of_rowset = true,
  bool cursor_bound = false) {
  wire::TypedResultBatch batch;
  batch.execution_uuid = Uuid(0x60);
  batch.result_set_uuid = Uuid(0x40);
  batch.batch_uuid = Uuid(static_cast<byte>(0x50u + batch_ordinal));
  batch.batch_ordinal = batch_ordinal;
  batch.end_of_rowset = end_of_rowset;
  batch.cursor_bound = cursor_bound;
  batch.row_descriptor_uuid = descriptor.descriptor_uuid;
  batch.row_descriptor_generation = descriptor.descriptor_generation;
  batch.snapshot_uuid = Uuid(0x61);
  if (cursor_bound) {
    batch.cursor_uuid = Uuid(0x62);
  }
  batch.rows = std::move(rows);
  return batch;
}

wire::TypedResultCarrierBinding ExecuteBinding(
    const wire::TypedResultBatch& batch) {
  wire::TypedResultCarrierBinding binding;
  binding.kind = wire::TypedResultCarrierKind::ps_execute_result_v1;
  binding.row_count = batch.rows.size();
  binding.end_of_rowset = batch.end_of_rowset;
  binding.execution_uuid = batch.execution_uuid;
  binding.result_set_uuid = batch.result_set_uuid;
  binding.snapshot_uuid = batch.snapshot_uuid;
  return binding;
}

wire::TypedResultCarrierBinding FetchBinding(
    const wire::TypedResultBatch& batch) {
  wire::TypedResultCarrierBinding binding;
  binding.kind = wire::TypedResultCarrierKind::ps_fetch_result_v1;
  binding.row_count = batch.rows.size();
  binding.end_of_rowset = batch.end_of_rowset;
  binding.execution_uuid = batch.execution_uuid;
  binding.result_set_uuid = batch.result_set_uuid;
  binding.snapshot_uuid = batch.snapshot_uuid;
  binding.cursor_uuid = batch.cursor_uuid;
  binding.cursor_stream_descriptor_uuid = Uuid(0x63);
  binding.cursor_stream_descriptor_version = 1;
  binding.cursor_stream_descriptor_generation = 91;
  return binding;
}

void DescriptorEvidenceAndMalformedFrames() {
  const auto descriptor =
      Descriptor({TextColumn(0, "field;with=delimiters", 0, 0x11)});
  const auto encoded = wire::EncodeTypedResultRowDescriptor(descriptor);
  Require(encoded.ok(), "descriptor encoding failed: " + encoded.detail);
  Require(platform::LoadLittle16(encoded.encoded.data() + 10) == 128 &&
              platform::LoadLittle32(encoded.encoded.data() + 80) == 1,
          "descriptor exact header offsets changed");

  auto zeroed = encoded.encoded;
  std::fill(zeroed.begin() + kDescriptorEvidenceOffset,
            zeroed.begin() + kDescriptorEvidenceOffset +
                wire::kTypedResultEvidenceHashBytes,
            0);
  const auto oracle = IndependentEvidence(kDescriptorDomain, zeroed);
  Require(oracle == EvidenceAt(encoded.encoded, kDescriptorEvidenceOffset),
          "descriptor evidence disagrees with independent domain oracle");

  const auto decoded =
      wire::DecodeTypedResultRowDescriptor(encoded.encoded);
  Require(decoded.ok(), "descriptor decoding failed: " + decoded.detail);
  Require(decoded.encoded == encoded.encoded,
          "descriptor decode/re-encode identity changed");
  Require(decoded.descriptor.descriptor_generation == 77 &&
              decoded.descriptor.datatype_catalog_generation == 81 &&
              decoded.descriptor.datatype_registry_generation == 82,
          "descriptor/catalog/registry generations were not preserved");
  Require(decoded.descriptor.columns[0].descriptor_generation == 1017 &&
              decoded.descriptor.columns[0].type_generation == 2017 &&
              decoded.descriptor.columns[0].codec_generation == 3017,
          "descriptor/type/codec generations were not preserved exactly");
  Require(decoded.descriptor.columns[0].name == "field;with=delimiters",
          "length-framed descriptor name was corrupted");

  auto truncated = encoded.encoded;
  truncated.pop_back();
  Require(!wire::DecodeTypedResultRowDescriptor(truncated).ok(),
          "truncated descriptor was accepted");
  auto trailing = encoded.encoded;
  trailing.push_back(0);
  Require(!wire::DecodeTypedResultRowDescriptor(trailing).ok(),
          "descriptor trailing bytes were accepted");
  auto wrong_magic = encoded.encoded;
  wrong_magic[0] ^= 0x01u;
  Require(!wire::DecodeTypedResultRowDescriptor(wrong_magic).ok(),
          "descriptor with invalid magic was accepted");
  auto wrong_version = encoded.encoded;
  platform::StoreLittle16(wrong_version.data() + 8, 2);
  Require(!wire::DecodeTypedResultRowDescriptor(wrong_version).ok(),
          "descriptor with unsupported version was accepted");
  auto changed_body = encoded.encoded;
  changed_body.back() ^= 0x01u;
  const auto changed_result =
      wire::DecodeTypedResultRowDescriptor(changed_body);
  Require(!changed_result.ok() &&
              changed_result.status ==
                  wire::TypedResultCodecStatus::evidence_mismatch &&
              changed_result.diagnostic_code ==
                  "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
          "descriptor mutation did not fail SHA evidence");

  auto wrong_domain = encoded.encoded;
  std::fill(wrong_domain.begin() + kDescriptorEvidenceOffset,
            wrong_domain.begin() + kDescriptorEvidenceOffset +
                wire::kTypedResultEvidenceHashBytes,
            0);
  const auto wrong_evidence =
      IndependentEvidence("ScratchBird.WrongDomain.V1", wrong_domain);
  std::copy(wrong_evidence.begin(), wrong_evidence.end(),
            wrong_domain.begin() + kDescriptorEvidenceOffset);
  Require(!wire::DecodeTypedResultRowDescriptor(wrong_domain).ok(),
          "descriptor evidence from the wrong domain was admitted");

  auto reserved = encoded.encoded;
  platform::StoreLittle32(reserved.data() + 84, 1);
  Rehash(&reserved, kDescriptorEvidenceOffset, kDescriptorDomain);
  Require(!wire::DecodeTypedResultRowDescriptor(reserved).ok(),
          "nonzero descriptor reserved bytes were admitted");
  auto impossible_length = encoded.encoded;
  platform::StoreLittle64(impossible_length.data() + 16,
                          impossible_length.size() + 1);
  Rehash(&impossible_length, kDescriptorEvidenceOffset, kDescriptorDomain);
  Require(!wire::DecodeTypedResultRowDescriptor(impossible_length).ok(),
          "impossible descriptor total length was admitted");

  auto inconsistent_evidence = encoded.descriptor;
  inconsistent_evidence.descriptor_evidence_sha256[0] ^= 1u;
  Require(!wire::EncodeTypedResultRowDescriptor(inconsistent_evidence).ok(),
          "caller-supplied inconsistent descriptor evidence was admitted");

  auto invalid_variable_width = descriptor;
  invalid_variable_width.columns[0].canonical_value_bytes = 1;
  const auto invalid_width_result =
      wire::EncodeTypedResultRowDescriptor(invalid_variable_width);
  Require(!invalid_width_result.ok() &&
              invalid_width_result.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR_INVALID",
          "variable-width descriptor admitted a fixed payload width");
}

void DelimiterPayloadAndDuplicateNameRoundTrip() {
  const auto descriptor = Descriptor(
      {TextColumn(0, "duplicate;name=key", 0, 0x12),
       TextColumn(1, "duplicate;name=key", 1, 0x13)});
  const std::string first = "alpha=1;beta=two;;empty=;tail===";
  const std::string second = ";=;=literal-not-structure=";
  const auto batch = Batch(
      descriptor,
      {Row(0, {Present(0, 0, Bytes(first)),
               Present(1, 1, Bytes(second))})});
  const auto binding = ExecuteBinding(batch);
  const auto encoded =
      wire::EncodeTypedResultBatch(batch, descriptor, binding);
  Require(encoded.ok(), "delimiter batch encoding failed: " + encoded.detail);
  const auto decoded = wire::DecodeTypedResultBatch(
      encoded.encoded, descriptor, binding);
  Require(decoded.ok(), "delimiter batch decoding failed: " + decoded.detail);
  Require(decoded.batch.rows.size() == 1 &&
              decoded.batch.rows[0].cells.size() == 2,
          "duplicate-name result shape changed");
  Require(decoded.batch.rows[0].cells[0].canonical_payload == Bytes(first) &&
              decoded.batch.rows[0].cells[1].canonical_payload == Bytes(second),
          "semicolon/equal-sign payload was corrupted");

  auto invalid_occurrence = descriptor;
  invalid_occurrence.columns[1].name_occurrence = 0;
  Require(!wire::EncodeTypedResultRowDescriptor(invalid_occurrence).ok(),
          "duplicate name without a distinct occurrence was admitted");

  auto invalid_cell = batch;
  invalid_cell.rows[0].cells[1].name_occurrence = 0;
  Require(!wire::EncodeTypedResultBatch(invalid_cell, descriptor,
                                        ExecuteBinding(invalid_cell))
               .ok(),
          "cell with the wrong duplicate-name occurrence was admitted");
}

void EmptyAndNullRemainDistinct() {
  const auto descriptor =
      Descriptor({TextColumn(0, "empty_or_null", 0, 0x14)});
  const auto batch = Batch(
      descriptor,
      {Row(0, {Present(0, 0, {})}), Row(1, {Null(0, 0)})});
  const auto binding = ExecuteBinding(batch);
  const auto encoded =
      wire::EncodeTypedResultBatch(batch, descriptor, binding);
  Require(encoded.ok(), "empty/null batch encoding failed: " + encoded.detail);
  const auto decoded = wire::DecodeTypedResultBatch(
      encoded.encoded, descriptor, binding);
  Require(decoded.ok(), "empty/null batch decoding failed: " + decoded.detail);
  Require(decoded.batch.rows[0].cells[0].state ==
              wire::TypedResultValueState::value_present &&
              decoded.batch.rows[0].cells[0].canonical_payload.empty(),
          "empty non-null value did not round-trip as present");
  Require(decoded.batch.rows[1].cells[0].state ==
              wire::TypedResultValueState::sql_null &&
              decoded.batch.rows[1].cells[0].canonical_payload.empty(),
          "SQL NULL did not round-trip as explicit null state");

  auto null_with_payload = batch;
  null_with_payload.rows[1].cells[0].canonical_payload = Bytes("not-null");
  Require(!wire::EncodeTypedResultBatch(null_with_payload, descriptor,
                                        ExecuteBinding(null_with_payload))
               .ok(),
          "SQL NULL with payload was admitted");

  auto not_null_descriptor = descriptor;
  not_null_descriptor.columns[0].nullability =
      wire::TypedResultNullability::not_null;
  auto null_batch = Batch(not_null_descriptor, {Row(0, {Null(0, 0)})});
  Require(!wire::EncodeTypedResultBatch(null_batch, not_null_descriptor,
                                        ExecuteBinding(null_batch))
               .ok(),
          "SQL NULL was admitted for a NOT NULL descriptor");

  // Row header (16) + cell header (20) places the first DatatypeBinaryValue
  // envelope at byte 260.  A forbidden toast bit must still fail after valid
  // outer SHA evidence is recomputed.
  auto forbidden_toast = encoded.encoded;
  constexpr std::size_t first_value_envelope =
      wire::kTypedResultBatchHeaderBytes + 16 + 20;
  platform::StoreLittle16(forbidden_toast.data() + first_value_envelope + 12,
                          2);
  Rehash(&forbidden_toast, kBatchEvidenceOffset, kBatchDomain);
  const auto forbidden_result = wire::DecodeTypedResultBatch(
      forbidden_toast, descriptor, binding);
  Require(!forbidden_result.ok() &&
              forbidden_result.status ==
                  wire::TypedResultCodecStatus::value_invalid,
          "forbidden DatatypeBinaryValue toast flag was admitted");
}

void Int128MinimumMaximumRoundTrip() {
  const auto descriptor =
      Descriptor({Int128Column(0, "signed_128", 0, 0x15)});
  std::vector<byte> minimum(16, 0);
  minimum[15] = 0x80u;
  std::vector<byte> maximum(16, 0xffu);
  maximum[15] = 0x7fu;

  const auto batch = Batch(
      descriptor,
      {Row(0, {Present(0, 0, minimum)}),
       Row(1, {Present(0, 0, maximum)})});
  const auto binding = ExecuteBinding(batch);
  const auto encoded =
      wire::EncodeTypedResultBatch(batch, descriptor, binding);
  Require(encoded.ok(), "int128 batch encoding failed: " + encoded.detail);
  const auto decoded = wire::DecodeTypedResultBatch(
      encoded.encoded, descriptor, binding);
  Require(decoded.ok(), "int128 batch decoding failed: " + decoded.detail);
  Require(decoded.batch.rows[0].cells[0].canonical_payload == minimum,
          "INT128 minimum was corrupted");
  Require(decoded.batch.rows[1].cells[0].canonical_payload == maximum,
          "INT128 maximum was corrupted");

  auto short_value = batch;
  short_value.rows[0].cells[0].canonical_payload.resize(15);
  Require(!wire::EncodeTypedResultBatch(short_value, descriptor,
                                        ExecuteBinding(short_value))
               .ok(),
          "short INT128 payload was admitted");
}

void BatchEvidenceShapeAndCursorConsistency() {
  const auto descriptor = Descriptor(
      {TextColumn(0, "left", 0, 0x16),
       TextColumn(1, "right", 0, 0x17)});
  const auto row = Row(
      0, {Present(0, 0, Bytes("L")), Present(1, 0, Bytes("R"))});

  auto missing_cell = Batch(descriptor, {row});
  missing_cell.rows[0].cells.pop_back();
  Require(!wire::EncodeTypedResultBatch(missing_cell, descriptor,
                                        ExecuteBinding(missing_cell))
               .ok(),
          "row with a missing cell was admitted");

  auto wrong_ordinal = Batch(descriptor, {row});
  wrong_ordinal.rows[0].cells[1].column_ordinal = 0;
  Require(!wire::EncodeTypedResultBatch(wrong_ordinal, descriptor,
                                        ExecuteBinding(wrong_ordinal))
               .ok(),
          "row with a duplicate cell ordinal was admitted");

  const auto execute_batch = Batch(descriptor, {row});
  const auto execute_binding = ExecuteBinding(execute_batch);
  const auto execute_encoded = wire::EncodeTypedResultBatch(
      execute_batch, descriptor, execute_binding);
  Require(execute_encoded.ok(), "execute batch encoding failed");
  Require(platform::LoadLittle16(execute_encoded.encoded.data() + 10) == 224 &&
              platform::LoadLittle32(execute_encoded.encoded.data() + 136) == 1,
          "batch exact header offsets changed");
  auto zeroed = execute_encoded.encoded;
  std::fill(zeroed.begin() + kBatchEvidenceOffset,
            zeroed.begin() + kBatchEvidenceOffset +
                wire::kTypedResultEvidenceHashBytes,
            0);
  Require(IndependentEvidence(kBatchDomain, zeroed) ==
              EvidenceAt(execute_encoded.encoded, kBatchEvidenceOffset),
          "batch evidence disagrees with independent domain oracle");

  auto cursor_as_execute = Batch(descriptor, {row}, 0, true, true);
  Require(!wire::EncodeTypedResultBatch(cursor_as_execute, descriptor,
                                        ExecuteBinding(cursor_as_execute))
               .ok(),
          "execute carrier admitted a cursor-bound packet");

  auto first_batch = Batch(descriptor, {row}, 0, false, true);
  const auto first_binding = FetchBinding(first_batch);
  const auto first_encoded = wire::EncodeTypedResultBatch(
      first_batch, descriptor, first_binding);
  Require(first_encoded.ok(), "first cursor batch encoding failed");
  auto wrong_outer_cursor = first_binding;
  wrong_outer_cursor.cursor_uuid = Uuid(0x6e);
  wire::TypedResultCursorBatchState wrong_cursor_state;
  const auto wrong_cursor_result = wire::DecodeTypedResultBatch(
      first_encoded.encoded, descriptor, wrong_outer_cursor,
      &wrong_cursor_state);
  Require(!wrong_cursor_result.ok() &&
              wrong_cursor_result.diagnostic_code ==
                  "PARSER_SERVER_IPC.CONNECTION_MISMATCH",
          "wrong outer cursor did not use the canonical ownership diagnostic");
  wire::TypedResultCursorBatchState state;
  Require(wire::DecodeTypedResultBatch(first_encoded.encoded, descriptor,
                                       first_binding, &state)
              .ok(),
          "first cursor batch failed stateful decoding");
  Require(state.initialized && !state.terminal && state.next_batch_ordinal == 1,
          "first cursor batch did not initialize sequence state");

  auto second_batch = Batch(descriptor, {row}, 1, true, true);
  const auto second_binding = FetchBinding(second_batch);
  const auto second_encoded = wire::EncodeTypedResultBatch(
      second_batch, descriptor, second_binding);
  Require(second_encoded.ok(), "second cursor batch encoding failed");
  Require(wire::DecodeTypedResultBatch(second_encoded.encoded, descriptor,
                                       second_binding, &state)
              .ok(),
          "contiguous cursor batch was rejected");
  Require(state.terminal, "terminal cursor batch was not recorded");
  Require(!wire::DecodeTypedResultBatch(second_encoded.encoded, descriptor,
                                        second_binding, &state)
               .ok(),
          "batch after terminal cursor state was admitted");

  wire::TypedResultCursorBatchState sequence_state;
  Require(wire::DecodeTypedResultBatch(first_encoded.encoded, descriptor,
                                       first_binding, &sequence_state)
              .ok(),
          "sequence state setup failed");
  auto skipped_batch = Batch(descriptor, {row}, 2, true, true);
  const auto skipped_binding = FetchBinding(skipped_batch);
  const auto skipped_encoded = wire::EncodeTypedResultBatch(
      skipped_batch, descriptor, skipped_binding);
  Require(skipped_encoded.ok(), "skipped batch encoding failed");
  wire::TypedResultCursorBatchState nonzero_first_state;
  Require(!wire::DecodeTypedResultBatch(skipped_encoded.encoded, descriptor,
                                        skipped_binding,
                                        &nonzero_first_state)
               .ok(),
          "cursor sequence admitted a nonzero first batch ordinal");
  const auto skipped_result = wire::DecodeTypedResultBatch(
      skipped_encoded.encoded, descriptor, skipped_binding, &sequence_state);
  Require(!skipped_result.ok() &&
              skipped_result.diagnostic_code ==
                  "PARSER_SERVER_IPC.SEQUENCE_INVALID",
          "non-contiguous cursor batch was admitted");

  auto reused_uuid_batch = second_batch;
  reused_uuid_batch.batch_uuid = first_batch.batch_uuid;
  const auto reused_uuid_encoded = wire::EncodeTypedResultBatch(
      reused_uuid_batch, descriptor, FetchBinding(reused_uuid_batch));
  Require(reused_uuid_encoded.ok(), "reused UUID fixture encoding failed");
  wire::TypedResultCursorBatchState reused_uuid_state;
  Require(wire::DecodeTypedResultBatch(first_encoded.encoded, descriptor,
                                       first_binding, &reused_uuid_state)
              .ok(),
          "reused UUID state setup failed");
  Require(!wire::DecodeTypedResultBatch(reused_uuid_encoded.encoded,
                                        descriptor,
                                        FetchBinding(reused_uuid_batch),
                                        &reused_uuid_state)
               .ok(),
          "cursor batch UUID reuse was admitted");

  auto drift_batch = second_batch;
  drift_batch.snapshot_uuid = Uuid(0x6f);
  const auto drift_encoded = wire::EncodeTypedResultBatch(
      drift_batch, descriptor, FetchBinding(drift_batch));
  Require(drift_encoded.ok(), "snapshot-drift fixture encoding failed");
  wire::TypedResultCursorBatchState drift_state;
  Require(wire::DecodeTypedResultBatch(first_encoded.encoded, descriptor,
                                       first_binding, &drift_state)
              .ok(),
          "snapshot-drift state setup failed");
  Require(!wire::DecodeTypedResultBatch(drift_encoded.encoded, descriptor,
                                        FetchBinding(drift_batch),
                                        &drift_state)
               .ok(),
          "cursor snapshot drift was admitted");

  wire::TypedResultCursorBatchState descriptor_state;
  Require(wire::DecodeTypedResultBatch(first_encoded.encoded, descriptor,
                                       first_binding, &descriptor_state)
              .ok(),
          "cursor descriptor drift state setup failed");
  auto drifted_cursor_descriptor = second_binding;
  ++drifted_cursor_descriptor.cursor_stream_descriptor_generation;
  const auto cursor_descriptor_result = wire::DecodeTypedResultBatch(
      second_encoded.encoded, descriptor, drifted_cursor_descriptor,
      &descriptor_state);
  Require(!cursor_descriptor_result.ok() &&
              cursor_descriptor_result.diagnostic_code ==
                  "PARSER_SERVER_IPC.CONNECTION_MISMATCH",
          "cursor stream descriptor generation drift was admitted");
  wire::TypedResultCursorBatchState version_state;
  Require(wire::DecodeTypedResultBatch(first_encoded.encoded, descriptor,
                                       first_binding, &version_state)
              .ok(),
          "cursor descriptor version state setup failed");
  auto wrong_descriptor_version = second_binding;
  wrong_descriptor_version.cursor_stream_descriptor_version = 2;
  Require(!wire::DecodeTypedResultBatch(second_encoded.encoded, descriptor,
                                        wrong_descriptor_version,
                                        &version_state)
               .ok(),
          "cursor stream descriptor version drift was admitted");

  auto wrong_descriptor = descriptor;
  wrong_descriptor.descriptor_generation += 1;
  const auto mismatch = wire::DecodeTypedResultBatch(
      execute_encoded.encoded, wrong_descriptor, execute_binding);
  Require(!mismatch.ok() &&
              mismatch.status ==
                  wire::TypedResultCodecStatus::descriptor_mismatch,
          "batch descriptor generation drift was admitted");

  std::vector<wire::TypedResultRowDescriptor> authority_drifts;
  auto catalog_generation_drift = descriptor;
  ++catalog_generation_drift.datatype_catalog_generation;
  authority_drifts.push_back(catalog_generation_drift);
  auto registry_generation_drift = descriptor;
  ++registry_generation_drift.datatype_registry_generation;
  authority_drifts.push_back(registry_generation_drift);
  auto column_descriptor_generation_drift = descriptor;
  ++column_descriptor_generation_drift.columns[0].descriptor_generation;
  authority_drifts.push_back(column_descriptor_generation_drift);
  auto type_generation_drift = descriptor;
  ++type_generation_drift.columns[0].type_generation;
  authority_drifts.push_back(type_generation_drift);
  auto codec_generation_drift = descriptor;
  ++codec_generation_drift.columns[0].codec_generation;
  authority_drifts.push_back(codec_generation_drift);
  for (const auto& drift : authority_drifts) {
    const auto drift_result = wire::DecodeTypedResultBatch(
        execute_encoded.encoded, drift, execute_binding);
    Require(!drift_result.ok() &&
                drift_result.status ==
                    wire::TypedResultCodecStatus::descriptor_mismatch,
            "descriptor catalog/type/codec generation drift was admitted");
  }

  auto outer_count_mismatch = execute_binding;
  outer_count_mismatch.row_count = 2;
  Require(!wire::DecodeTypedResultBatch(execute_encoded.encoded, descriptor,
                                        outer_count_mismatch)
               .ok(),
          "outer row count mismatch was admitted");
  auto outer_end_mismatch = execute_binding;
  outer_end_mismatch.end_of_rowset = false;
  Require(!wire::DecodeTypedResultBatch(execute_encoded.encoded, descriptor,
                                        outer_end_mismatch)
               .ok(),
          "outer end-state mismatch was admitted");
  auto crossed_result_handle = execute_binding;
  crossed_result_handle.result_set_uuid = Uuid(0x6d);
  const auto crossed_handle_result = wire::DecodeTypedResultBatch(
      execute_encoded.encoded, descriptor, crossed_result_handle);
  Require(!crossed_handle_result.ok() &&
              crossed_handle_result.diagnostic_code ==
                  "PARSER_SERVER_IPC.CONNECTION_MISMATCH",
          "crossed four-UUID query result handle was admitted");

  auto trailing = execute_encoded.encoded;
  trailing.push_back(0);
  Require(!wire::DecodeTypedResultBatch(trailing, descriptor,
                                        execute_binding)
               .ok(),
          "batch trailing bytes were admitted");
  auto wrong_batch_magic = execute_encoded.encoded;
  wrong_batch_magic[0] ^= 1u;
  Require(!wire::DecodeTypedResultBatch(wrong_batch_magic, descriptor,
                                        execute_binding)
               .ok(),
          "batch with wrong magic was admitted");
  auto wrong_batch_version = execute_encoded.encoded;
  platform::StoreLittle16(wrong_batch_version.data() + 8, 2);
  Require(!wire::DecodeTypedResultBatch(wrong_batch_version, descriptor,
                                        execute_binding)
               .ok(),
          "batch with unsupported version was admitted");
  auto unknown_flags = execute_encoded.encoded;
  platform::StoreLittle32(unknown_flags.data() + 12, 0x80000001u);
  Rehash(&unknown_flags, kBatchEvidenceOffset, kBatchDomain);
  Require(!wire::DecodeTypedResultBatch(unknown_flags, descriptor,
                                        execute_binding)
               .ok(),
          "batch with unknown flags was admitted");
  auto nonzero_batch_reserved = execute_encoded.encoded;
  platform::StoreLittle64(nonzero_batch_reserved.data() + 184, 1);
  Rehash(&nonzero_batch_reserved, kBatchEvidenceOffset, kBatchDomain);
  Require(!wire::DecodeTypedResultBatch(nonzero_batch_reserved, descriptor,
                                        execute_binding)
               .ok(),
          "batch with nonzero reserved bytes was admitted");
  auto zero_row_count = execute_encoded.encoded;
  platform::StoreLittle32(zero_row_count.data() + 136, 0);
  Rehash(&zero_row_count, kBatchEvidenceOffset, kBatchDomain);
  Require(!wire::DecodeTypedResultBatch(zero_row_count, descriptor,
                                        execute_binding)
               .ok(),
          "nonempty packet with zero row count was admitted");
  auto corrupted = execute_encoded.encoded;
  corrupted.back() ^= 0x01u;
  const auto corrupted_result = wire::DecodeTypedResultBatch(
      corrupted, descriptor, execute_binding);
  Require(!corrupted_result.ok() &&
              corrupted_result.status ==
                  wire::TypedResultCodecStatus::evidence_mismatch,
          "batch payload mutation did not fail SHA evidence");

  auto descriptor_hash_drift = execute_encoded.encoded;
  descriptor_hash_drift[104] ^= 1u;
  Rehash(&descriptor_hash_drift, kBatchEvidenceOffset, kBatchDomain);
  const auto descriptor_hash_result = wire::DecodeTypedResultBatch(
      descriptor_hash_drift, descriptor, execute_binding);
  Require(!descriptor_hash_result.ok() &&
              descriptor_hash_result.status ==
                  wire::TypedResultCodecStatus::descriptor_mismatch,
          "batch with changed descriptor evidence was admitted");
}

}  // namespace

int main() {
  try {
    DescriptorEvidenceAndMalformedFrames();
    DelimiterPayloadAndDuplicateNameRoundTrip();
    EmptyAndNullRemainDistinct();
    Int128MinimumMaximumRoundTrip();
    BatchEvidenceShapeAndCursorConsistency();
  } catch (const std::exception& error) {
    std::cerr << "typed result transport codec test failed: " << error.what()
              << '\n';
    return 1;
  }
  return 0;
}
