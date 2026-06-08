// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_key_encoding.hpp"
#include "physical_columnar_zone.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "physical_columnar_zone_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid TypeUuid(platform::byte seed) {
  platform::TypedUuid typed;
  typed.kind = platform::UuidKind::object;
  for (std::size_t i = 0; i < typed.value.bytes.size(); ++i) {
    typed.value.bytes[i] = static_cast<platform::byte>(seed + i + 1);
  }
  typed.value.bytes[6] =
      static_cast<platform::byte>((typed.value.bytes[6] & 0x0f) | 0x70);
  typed.value.bytes[8] =
      static_cast<platform::byte>((typed.value.bytes[8] & 0x3f) | 0x80);
  return typed;
}

std::vector<platform::byte> SignedPayload(std::int64_t value) {
  const auto sortable = static_cast<std::uint64_t>(value) ^ 0x8000000000000000ull;
  std::vector<platform::byte> out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(7 - i)] =
        static_cast<platform::byte>((sortable >> (i * 8)) & 0xffu);
  }
  return out;
}

std::string EncodedScalar(std::vector<platform::byte> payload,
                          platform::byte type_seed) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = TypeUuid(type_seed);
  component.sort_direction = idx::IndexKeySortDirection::ascending;
  component.null_placement = idx::IndexKeyNullPlacement::nulls_last;
  component.payload = std::move(payload);
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "order-preserving key encode failed");
  return std::string(reinterpret_cast<const char*>(encoded.encoded.data()),
                     encoded.encoded.size());
}

std::string IntKey(std::int64_t value) {
  return EncodedScalar(SignedPayload(value), 0x40);
}

std::string TextKey(std::string_view value) {
  return EncodedScalar({value.begin(), value.end()}, 0x60);
}

idx::PhysicalColumnarZoneColumnValueEvidence IntColumn(std::int64_t value) {
  idx::PhysicalColumnarZoneColumnValueEvidence column;
  column.column_ordinal = 0;
  column.scalar_type_key = "sb.i64";
  column.encoded_scalar = IntKey(value);
  return column;
}

idx::PhysicalColumnarZoneColumnValueEvidence TextColumn(std::string_view value) {
  idx::PhysicalColumnarZoneColumnValueEvidence column;
  column.column_ordinal = 1;
  column.scalar_type_key = "sb.text";
  column.encoded_scalar = TextKey(value);
  return column;
}

idx::PhysicalColumnarZoneColumnValueEvidence NullTextColumn() {
  idx::PhysicalColumnarZoneColumnValueEvidence column;
  column.column_ordinal = 1;
  column.scalar_type_key = "sb.text";
  column.value_is_null = true;
  return column;
}

idx::PhysicalColumnarZoneRowEvidence Row(
    platform::u64 page_id,
    platform::u64 row_group_id,
    platform::u64 row_ordinal,
    std::vector<idx::PhysicalColumnarZoneColumnValueEvidence> columns,
    bool deleted = false,
    platform::u64 base_generation = 7) {
  idx::PhysicalColumnarZoneRowEvidence row;
  row.page_id = page_id;
  row.row_group_id = row_group_id;
  row.row_ordinal = row_ordinal;
  row.base_generation = base_generation;
  row.physically_deleted = deleted;
  row.columns = std::move(columns);
  return row;
}

idx::PhysicalColumnarZoneCompressionPolicy Compression(
    platform::u32 ordinal,
    std::string scalar_type,
    std::string codec,
    platform::u64 uncompressed,
    platform::u64 compressed) {
  idx::PhysicalColumnarZoneCompressionPolicy policy;
  policy.column_ordinal = ordinal;
  policy.scalar_type_key = std::move(scalar_type);
  policy.codec_id = std::move(codec);
  policy.uncompressed_bytes = uncompressed;
  policy.compressed_bytes = compressed;
  policy.estimated_cpu_cost = ordinal == 0 ? 1.25 : 1.75;
  policy.estimated_read_cost = ordinal == 0 ? 0.30 : 0.45;
  policy.estimated_write_cost = ordinal == 0 ? 0.50 : 0.80;
  return policy;
}

std::vector<idx::PhysicalColumnarZoneCompressionPolicy> Policies() {
  return {Compression(0, "sb.i64", "delta_bitpack_v1", 800, 240),
          Compression(1, "sb.text", "dictionary_lz4_v1", 1200, 360)};
}

std::vector<idx::PhysicalColumnarZoneRowEvidence> BaseRows() {
  return {Row(0, 10, 0, {IntColumn(10), TextColumn("blue")}),
          Row(1, 10, 1, {IntColumn(20), TextColumn("red")}),
          Row(1, 10, 2, {IntColumn(15), NullTextColumn()}, true),
          Row(2, 11, 10, {IntColumn(30), TextColumn("green")}),
          Row(3, 11, 11, {IntColumn(40), TextColumn("orange")}),
          Row(3, 11, 12, {IntColumn(50), TextColumn("purple")})};
}

idx::PhysicalColumnarZoneBuildRequest BuildRequest() {
  idx::PhysicalColumnarZoneBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.segment_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.summary_generation = 11;
  request.dictionary_limit = 2;
  request.rows = BaseRows();
  request.compression_policies = Policies();
  return request;
}

idx::PhysicalColumnarZoneSegment BuildSegment() {
  const auto built = idx::BuildPhysicalColumnarZoneFromPageEvidence(BuildRequest());
  Require(built.ok(), "physical columnar-zone build failed");
  return built.segment;
}

const idx::PhysicalColumnarZoneColumnSummary& Column(
    const idx::PhysicalColumnarZoneRowGroupSummary& group,
    platform::u32 ordinal) {
  auto iter = std::find_if(group.columns.begin(),
                           group.columns.end(),
                           [ordinal](const idx::PhysicalColumnarZoneColumnSummary& column) {
                             return column.column_ordinal == ordinal;
                           });
  Require(iter != group.columns.end(), "expected column summary missing");
  return *iter;
}

bool ContainsForbiddenRuntimeArtifact(std::string_view value) {
  return value.find("docs" "/execution-plans") != std::string_view::npos ||
         value.find("execution_plan") != std::string_view::npos ||
         value.find("public_release_evidence") != std::string_view::npos ||
         value.find("docs/reference") != std::string_view::npos;
}

platform::u64 FnvChecksum(std::vector<platform::byte> bytes) {
  if (bytes.size() >= 24) {
    platform::StoreLittle64(bytes.data() + 16, 0);
  }
  platform::u64 hash = 14695981039346656037ull;
  for (platform::byte value : bytes) {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash == 0 ? 1 : hash;
}

void Rechecksum(std::vector<platform::byte>* bytes) {
  platform::StoreLittle64(bytes->data() + 16, FnvChecksum(*bytes));
}

std::size_t SkipString(const std::vector<platform::byte>& bytes,
                       std::size_t offset) {
  Require(offset + 4 <= bytes.size(), "test string length outside payload");
  const auto size = platform::LoadLittle32(bytes.data() + offset);
  offset += 4;
  Require(offset + size <= bytes.size(), "test string data outside payload");
  return offset + size;
}

std::size_t FirstGroupOffset(const std::vector<platform::byte>& bytes) {
  std::size_t offset = 24;
  offset = SkipString(bytes, offset);
  offset = SkipString(bytes, offset);
  offset = SkipString(bytes, offset);
  offset += 8 + 8 + 8;
  Require(offset + 4 <= bytes.size(), "test row-group count outside payload");
  return offset + 4;
}

std::size_t FirstColumnOffset(const std::vector<platform::byte>& bytes) {
  std::size_t offset = FirstGroupOffset(bytes);
  const auto candidate_count = platform::LoadLittle32(bytes.data() + offset + 77);
  return offset + 81 + static_cast<std::size_t>(candidate_count) * 8 + 4;
}

std::size_t EncodedMinDataOffset(const std::vector<platform::byte>& bytes) {
  std::size_t offset = FirstColumnOffset(bytes);
  offset += 4;
  offset = SkipString(bytes, offset);
  offset += 8 + 8 + 1 + 1;
  Require(offset + 4 <= bytes.size(), "encoded-min length outside payload");
  return offset + 4;
}

void VerifyBuildAndArtifact(const idx::PhysicalColumnarZoneSegment& segment) {
  Require(segment.row_groups.size() == 2, "expected two row groups");
  Require(segment.base_generation == 7 && segment.summary_generation == 11,
          "segment generations drifted");
  const auto& first = segment.row_groups[0];
  Require(first.boundary.row_group_id == 10 &&
              first.boundary.first_page_id == 0 &&
              first.boundary.page_count == 2 &&
              first.boundary.first_row_ordinal == 0 &&
              first.row_count == 3 &&
              first.deleted_row_count == 1,
          "first row-group boundary/counts drifted");
  Require(first.candidate_row_ordinals == std::vector<platform::u64>({0, 1}),
          "candidate row ordinals did not exclude deleted rows");
  Require(Column(first, 0).encoded_min == IntKey(10) &&
              Column(first, 0).encoded_max == IntKey(20),
          "first row-group integer min/max drifted");
  Require(Column(first, 1).dictionary_exact &&
              Column(first, 1).dictionary_values.size() == 2,
          "bounded exact dictionary missing");
  const auto& second = segment.row_groups[1];
  Require(second.candidate_row_ordinals ==
              std::vector<platform::u64>({10, 11, 12}),
          "second row-group candidate ordinals drifted");
  Require(Column(second, 0).dictionary_overflow &&
              !Column(second, 0).dictionary_exact &&
              Column(second, 0).dictionary_values.empty(),
          "integer dictionary overflow did not fail exact set pruning closed");
  Require(segment.compression_policies.size() == 2 &&
              segment.compression_policies[0].codec_id == "delta_bitpack_v1" &&
              segment.compression_policies[0].exact_fallback_equivalence &&
              segment.compression_policies[0].storage_runtime_evidence_only,
          "compression policy metadata missing");
  Require(!segment.visibility_finality_authority &&
              segment.mga_recheck_required &&
              segment.security_recheck_required &&
              segment.exact_recheck_required,
          "segment authority/recheck evidence drifted");
}

idx::PhysicalColumnarZoneSegment VerifySerializationReopen(
    const idx::PhysicalColumnarZoneSegment& segment) {
  const auto serialized = idx::SerializePhysicalColumnarZoneSegment(segment);
  Require(serialized.ok(), "columnar-zone serialization failed");
  const auto serialized_again = idx::SerializePhysicalColumnarZoneSegment(segment);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "columnar-zone serialization is not deterministic");

  idx::PhysicalColumnarZoneOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = segment.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = segment.index_uuid;
  open.expected_segment_uuid_present = true;
  open.expected_segment_uuid = segment.segment_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = segment.base_generation;
  const auto opened = idx::OpenPhysicalColumnarZoneSegment(open);
  Require(opened.ok(), "clean columnar-zone reopen failed");
  const auto reserialized = idx::SerializePhysicalColumnarZoneSegment(opened.segment);
  Require(reserialized.ok() && reserialized.bytes == serialized.bytes,
          "reopen serialization equivalence failed");
  return opened.segment;
}

void VerifyPruningAndLateMaterialization(
    const idx::PhysicalColumnarZoneSegment& segment) {
  idx::PhysicalColumnarZonePredicate int_range;
  int_range.column_ordinal = 0;
  int_range.scalar_type_key = "sb.i64";
  int_range.lower_present = true;
  int_range.upper_present = true;
  int_range.encoded_lower = IntKey(25);
  int_range.encoded_upper = IntKey(35);
  const auto pruned = idx::PrunePhysicalColumnarZone({segment, {int_range}});
  Require(pruned.ok(), "columnar-zone prune failed");
  Require(pruned.any_pruned && pruned.groups.size() == 2,
          "expected row-group pruning evidence missing");
  Require(pruned.groups[0].decision ==
              idx::PhysicalColumnarZonePruneDecision::pruned_by_min_max &&
              !pruned.groups[0].scan_required &&
              pruned.groups[0].candidate_row_ordinals.empty(),
          "first row group was not pruned by min/max");
  Require(pruned.groups[1].scan_required &&
              pruned.groups[1].candidate_page_ids ==
                  std::vector<platform::u64>({2, 3}) &&
              pruned.groups[1].candidate_row_ordinals ==
                  std::vector<platform::u64>({10, 11, 12}) &&
              pruned.groups[1].mga_recheck_required &&
              pruned.groups[1].security_recheck_required &&
              pruned.groups[1].exact_recheck_required,
          "non-pruned row group did not preserve candidate stream rechecks");

  const auto stream = idx::OpenPhysicalColumnarZoneCandidateStream(
      {segment, pruned, {1, 0}});
  Require(stream.ok(), "late materialization candidate stream refused");
  Require(!stream.fetches_non_candidate_rows &&
              stream.candidate_row_ordinals ==
                  std::vector<platform::u64>({10, 11, 12}) &&
              stream.projection_column_ordinals ==
                  std::vector<platform::u32>({0, 1}) &&
              stream.mga_recheck_required &&
              stream.security_recheck_required &&
              stream.exact_recheck_required &&
              !stream.visibility_finality_authority,
          "late materialization stream authority or candidate contract drifted");

  auto forged_prune = pruned;
  forged_prune.groups[1].candidate_row_ordinals.push_back(999);
  const auto forged_stream = idx::OpenPhysicalColumnarZoneCandidateStream(
      {segment, forged_prune, {0, 1}});
  Require(!forged_stream.ok(),
          "late materialization accepted a forged non-segment candidate ordinal");

  idx::PhysicalColumnarZonePredicate text_absent;
  text_absent.column_ordinal = 1;
  text_absent.scalar_type_key = "sb.text";
  text_absent.lower_present = true;
  text_absent.upper_present = true;
  text_absent.encoded_lower = TextKey("cyan");
  text_absent.encoded_upper = TextKey("cyan");
  const auto dictionary_pruned =
      idx::PrunePhysicalColumnarZone({segment, {text_absent}});
  Require(dictionary_pruned.ok() &&
              dictionary_pruned.groups[0].decision ==
                  idx::PhysicalColumnarZonePruneDecision::pruned_by_exact_dictionary,
          "bounded exact dictionary did not prune absent value");

  idx::PhysicalColumnarZonePredicate int_absent_inside_range;
  int_absent_inside_range.column_ordinal = 0;
  int_absent_inside_range.scalar_type_key = "sb.i64";
  int_absent_inside_range.lower_present = true;
  int_absent_inside_range.upper_present = true;
  int_absent_inside_range.encoded_lower = IntKey(45);
  int_absent_inside_range.encoded_upper = IntKey(45);
  const auto overflow_scan =
      idx::PrunePhysicalColumnarZone({segment, {int_absent_inside_range}});
  Require(overflow_scan.ok() &&
              overflow_scan.groups[1].decision ==
                  idx::PhysicalColumnarZonePruneDecision::exact_dictionary_overflow_scan &&
              overflow_scan.groups[1].scan_required &&
              overflow_scan.groups[1].exact_recheck_required,
          "overflowed dictionary was used for exact set pruning");
}

void VerifyMaintenanceAndRepair(const idx::PhysicalColumnarZoneSegment& segment) {
  idx::PhysicalColumnarZoneMutation append;
  append.kind = idx::PhysicalColumnarZoneMutationKind::append_row;
  append.after_row_present = true;
  append.after_row = Row(3, 11, 13, {IntColumn(60), TextColumn("zebra")}, false, 8);
  const auto appended = idx::ApplyPhysicalColumnarZoneMutation(segment, append, 2);
  Require(appended.ok(), "append maintenance failed");
  Require(Column(appended.segment.row_groups[1], 0).encoded_max == IntKey(60) &&
              appended.segment.row_groups[1].candidate_row_ordinals ==
                  std::vector<platform::u64>({10, 11, 12, 13}),
          "append did not widen summary and candidate row stream");

  idx::PhysicalColumnarZoneMutation delete_boundary;
  delete_boundary.kind = idx::PhysicalColumnarZoneMutationKind::delete_row;
  delete_boundary.before_row_present = true;
  delete_boundary.before_row = Row(2, 11, 10, {IntColumn(30), TextColumn("green")});
  const auto refused =
      idx::ApplyPhysicalColumnarZoneMutation(appended.segment, delete_boundary, 2);
  Require(!refused.applied && refused.segment_invalidated && refused.scan_required,
          "delete touching boundary/dictionary/candidate ordinal did not fail closed");

  auto rebuilt_rows = BaseRows();
  rebuilt_rows.erase(rebuilt_rows.begin() + 3);
  rebuilt_rows.push_back(Row(3, 11, 13, {IntColumn(60), TextColumn("zebra")}, false, 8));
  delete_boundary.rebuild_admitted = true;
  delete_boundary.authoritative_base_rows = rebuilt_rows;
  delete_boundary.compression_policies = Policies();
  const auto repaired =
      idx::ApplyPhysicalColumnarZoneMutation(appended.segment, delete_boundary, 2);
  Require(repaired.applied && repaired.rebuild_performed,
          "admitted authoritative rebuild failed");
  Require(Column(repaired.segment.row_groups[1], 0).encoded_min == IntKey(40),
          "rebuild did not remove deleted boundary value");
}

void VerifyOpenRepairClassification(const idx::PhysicalColumnarZoneSegment& segment) {
  const auto serialized = idx::SerializePhysicalColumnarZoneSegment(segment);
  Require(serialized.ok(), "classification fixture serialization failed");

  auto bad_checksum = serialized.bytes;
  bad_checksum.back() ^= 0x01;
  const auto bad_open = idx::OpenPhysicalColumnarZoneSegment({bad_checksum});
  Require(bad_open.open_class == idx::PhysicalColumnarZoneOpenClass::bad_checksum &&
              bad_open.restricted_repair_required,
          "bad checksum classification drifted");

  auto stale_format = serialized.bytes;
  platform::StoreLittle32(stale_format.data() + 8, 0);
  const auto stale_open = idx::OpenPhysicalColumnarZoneSegment({stale_format});
  Require(stale_open.open_class == idx::PhysicalColumnarZoneOpenClass::stale_format,
          "stale format classification drifted");

  idx::PhysicalColumnarZoneOpenRequest stale_generation;
  stale_generation.bytes = serialized.bytes;
  stale_generation.expected_base_generation_present = true;
  stale_generation.expected_base_generation = 99;
  const auto stale_generation_open =
      idx::OpenPhysicalColumnarZoneSegment(stale_generation);
  Require(stale_generation_open.open_class ==
              idx::PhysicalColumnarZoneOpenClass::stale_generation,
          "stale generation classification drifted");

  idx::PhysicalColumnarZoneOpenRequest identity_mismatch;
  identity_mismatch.bytes = serialized.bytes;
  identity_mismatch.expected_segment_uuid_present = true;
  identity_mismatch.expected_segment_uuid = "99999999-9999-7999-8999-999999999999";
  const auto identity_open =
      idx::OpenPhysicalColumnarZoneSegment(identity_mismatch);
  Require(identity_open.open_class ==
              idx::PhysicalColumnarZoneOpenClass::identity_mismatch,
          "identity mismatch classification drifted");

  auto malformed_row_group = serialized.bytes;
  platform::StoreLittle32(malformed_row_group.data() + FirstGroupOffset(malformed_row_group) + 16,
                          0);
  Rechecksum(&malformed_row_group);
  const auto malformed_group_open =
      idx::OpenPhysicalColumnarZoneSegment({malformed_row_group});
  Require(malformed_group_open.open_class ==
              idx::PhysicalColumnarZoneOpenClass::malformed_row_group_payload,
          "malformed row-group classification drifted");

  auto malformed_column = serialized.bytes;
  std::size_t column_offset = FirstColumnOffset(malformed_column);
  column_offset += 4;
  column_offset = SkipString(malformed_column, column_offset);
  const auto row_count = platform::LoadLittle64(malformed_column.data() + column_offset);
  platform::StoreLittle64(malformed_column.data() + column_offset + 8, row_count + 1);
  Rechecksum(&malformed_column);
  const auto malformed_column_open =
      idx::OpenPhysicalColumnarZoneSegment({malformed_column});
  Require(malformed_column_open.open_class ==
              idx::PhysicalColumnarZoneOpenClass::malformed_column_payload,
          "malformed column classification drifted");

  auto unsafe_encoding = serialized.bytes;
  const auto encoded_min = EncodedMinDataOffset(unsafe_encoding);
  std::memcpy(unsafe_encoding.data() + encoded_min, "SBK1", 4);
  Rechecksum(&unsafe_encoding);
  const auto unsafe_open =
      idx::OpenPhysicalColumnarZoneSegment({unsafe_encoding});
  Require(unsafe_open.open_class ==
              idx::PhysicalColumnarZoneOpenClass::unsafe_encoding,
          "unsafe encoding classification drifted");

  auto truncated = serialized.bytes;
  truncated.resize(truncated.size() - 5);
  Rechecksum(&truncated);
  const auto truncated_open =
      idx::OpenPhysicalColumnarZoneSegment({truncated});
  Require(truncated_open.open_class ==
              idx::PhysicalColumnarZoneOpenClass::truncated_payload,
          "truncated payload classification drifted");

  const auto refused_repair = idx::RepairPhysicalColumnarZoneFromPageEvidence(
      segment, BaseRows(), Policies(), false, 2);
  Require(!refused_repair.applied && refused_repair.scan_required,
          "repair without admission did not fail closed");
  const auto admitted_repair = idx::RepairPhysicalColumnarZoneFromPageEvidence(
      segment, BaseRows(), Policies(), true, 2);
  Require(admitted_repair.applied && admitted_repair.rebuild_performed,
          "admitted repair did not rebuild from authoritative evidence");
}

void VerifyBuildRefusals() {
  auto invalid_uuid = BuildRequest();
  invalid_uuid.segment_uuid = "not-a-uuid";
  Require(!idx::BuildPhysicalColumnarZoneFromPageEvidence(invalid_uuid).ok(),
          "invalid UUID was accepted");

  auto duplicate_ordinal = BuildRequest();
  duplicate_ordinal.rows[0].columns.push_back(IntColumn(99));
  Require(!idx::BuildPhysicalColumnarZoneFromPageEvidence(duplicate_ordinal).ok(),
          "duplicate column ordinal was accepted");

  auto unsafe = BuildRequest();
  unsafe.rows[0].columns[0].encoded_scalar = "SBK1legacy";
  Require(!idx::BuildPhysicalColumnarZoneFromPageEvidence(unsafe).ok(),
          "unsafe legacy encoded key was accepted");

  auto missing_generation = BuildRequest();
  missing_generation.base_generation = 0;
  Require(!idx::BuildPhysicalColumnarZoneFromPageEvidence(missing_generation).ok(),
          "missing base generation was accepted");

  auto authority = BuildRequest();
  authority.rows[0].provider_finality_authority_claimed = true;
  Require(!idx::BuildPhysicalColumnarZoneFromPageEvidence(authority).ok(),
          "provider finality authority claim was accepted");

  auto log_authority = BuildRequest();
  log_authority.rows[0].write_ahead_log_finality_authority_claimed = true;
  Require(!idx::BuildPhysicalColumnarZoneFromPageEvidence(log_authority).ok(),
          "write-ahead-log authority claim was accepted");

  auto compression_authority = BuildRequest();
  compression_authority.compression_policies[0].compression_value_authority_claimed = true;
  Require(!idx::BuildPhysicalColumnarZoneFromPageEvidence(compression_authority).ok(),
          "compression value authority claim was accepted");

  auto compression_type_mismatch = BuildRequest();
  compression_type_mismatch.compression_policies[0].scalar_type_key = "sb.text";
  Require(!idx::BuildPhysicalColumnarZoneFromPageEvidence(compression_type_mismatch).ok(),
          "compression policy scalar type mismatch was accepted");

  auto candidate_outside_boundary = BuildSegment();
  candidate_outside_boundary.row_groups[0].candidate_row_ordinals[0] = 999;
  Require(!idx::SerializePhysicalColumnarZoneSegment(candidate_outside_boundary).ok(),
          "candidate ordinal outside row-group boundary was accepted");
}

void VerifyNoExecution_PlanRuntimeArtifacts(
    const idx::PhysicalColumnarZoneSegment& segment) {
  for (const auto& evidence : segment.evidence) {
    Require(!ContainsForbiddenRuntimeArtifact(evidence),
            "runtime evidence contains execution_plan/doc artifact token");
  }
  const auto serialized = idx::SerializePhysicalColumnarZoneSegment(segment);
  Require(serialized.ok(), "no-execution_plan serialization fixture failed");
  const std::string payload(reinterpret_cast<const char*>(serialized.bytes.data()),
                            serialized.bytes.size());
  Require(!ContainsForbiddenRuntimeArtifact(payload),
          "serialized runtime payload contains execution_plan/doc artifact token");
}

}  // namespace

int main() {
  const auto segment = BuildSegment();
  VerifyBuildAndArtifact(segment);
  const auto reopened = VerifySerializationReopen(segment);
  VerifyPruningAndLateMaterialization(reopened);
  VerifyMaintenanceAndRepair(reopened);
  VerifyOpenRepairClassification(reopened);
  VerifyBuildRefusals();
  VerifyNoExecution_PlanRuntimeArtifacts(reopened);
  return EXIT_SUCCESS;
}
