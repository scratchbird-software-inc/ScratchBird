// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_key_encoding.hpp"
#include "physical_zone_summary.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "physical_zone_summary_gate: " << message << '\n';
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
  typed.value.bytes[6] = static_cast<platform::byte>((typed.value.bytes[6] & 0x0f) | 0x70);
  typed.value.bytes[8] = static_cast<platform::byte>((typed.value.bytes[8] & 0x3f) | 0x80);
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
                          platform::byte type_seed = 0x40) {
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

std::string CompositeKey(std::initializer_list<std::string_view> encoded_values) {
  std::string out;
  for (std::string_view value : encoded_values) {
    Require(idx::IsOrderPreservingIndexKeyEncoding(value),
            "test composite input is not order-preserving");
    if (out.empty()) {
      out.assign(value.data(), 4);
    }
    out.append(value.data() + 4, value.size() - 4);
  }
  Require(idx::IsOrderPreservingIndexKeyEncoding(out),
          "test composite key did not retain order-preserving envelope");
  return out;
}

idx::PhysicalZoneColumnValueEvidence IntColumn(std::int64_t value) {
  idx::PhysicalZoneColumnValueEvidence column;
  column.column_ordinal = 0;
  column.scalar_type_key = "sb.i64";
  column.encoded_scalar = IntKey(value);
  return column;
}

idx::PhysicalZoneColumnValueEvidence TextColumn(std::string_view value) {
  idx::PhysicalZoneColumnValueEvidence column;
  column.column_ordinal = 1;
  column.scalar_type_key = "sb.text";
  column.encoded_scalar = TextKey(value);
  return column;
}

idx::PhysicalZoneColumnValueEvidence NullTextColumn() {
  idx::PhysicalZoneColumnValueEvidence column;
  column.column_ordinal = 1;
  column.scalar_type_key = "sb.text";
  column.value_is_null = true;
  return column;
}

idx::PhysicalZoneRowEvidence Row(platform::u64 page_id,
                                 platform::u64 base_generation,
                                 std::vector<idx::PhysicalZoneColumnValueEvidence> columns) {
  idx::PhysicalZoneRowEvidence row;
  row.page_id = page_id;
  row.extent_id = page_id / 2;
  row.base_generation = base_generation;
  row.columns = std::move(columns);
  return row;
}

idx::PhysicalZoneRangeSizingMetadata Sizing(platform::u64 base_generation = 7,
                                            platform::u64 summary_generation = 11) {
  idx::PhysicalZoneRangeSizingMetadata sizing;
  sizing.min_pages_per_range = 1;
  sizing.target_pages_per_range = 2;
  sizing.max_pages_per_range = 4;
  sizing.base_generation = base_generation;
  sizing.summary_generation = summary_generation;
  sizing.adaptive = true;
  return sizing;
}

std::vector<idx::PhysicalZoneRowEvidence> BaseRows() {
  return {
      Row(0, 7, {IntColumn(10), TextColumn("red")}),
      Row(1, 7, {IntColumn(20), TextColumn("blue")}),
      Row(1, 7, {IntColumn(15), NullTextColumn()}),
      Row(2, 7, {IntColumn(30), TextColumn("green")}),
      Row(3, 7, {IntColumn(40), TextColumn("green")}),
  };
}

idx::PhysicalZoneSummaryPage BuildPage() {
  idx::PhysicalZoneSummaryBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.summary_uuid = "22222222-2222-7222-8222-222222222222";
  request.range_sizing = Sizing();
  request.small_set_limit = 3;
  request.base_page_rows = BaseRows();
  const auto built = idx::BuildPhysicalZoneSummaryFromBasePageEvidence(request);
  Require(built.ok(), "build from base page evidence failed");
  return built.page;
}

const idx::PhysicalZoneColumnSummary& Column(
    const idx::PhysicalZoneRangeSummaryRecord& range,
    platform::u32 ordinal) {
  auto iter = std::find_if(range.columns.begin(),
                           range.columns.end(),
                           [ordinal](const idx::PhysicalZoneColumnSummary& column) {
                             return column.column_ordinal == ordinal;
                           });
  Require(iter != range.columns.end(), "expected column summary missing");
  return *iter;
}

bool ContainsForbiddenRuntimeArtifact(std::string_view value) {
  return value.find("docs" "/execution-plans") != std::string_view::npos ||
         value.find("execution_plan") != std::string_view::npos ||
         value.find("public_release_evidence") != std::string_view::npos ||
         value.find("docs/reference") != std::string_view::npos;
}

void VerifyBuildAndColumns(const idx::PhysicalZoneSummaryPage& page) {
  Require(page.ranges.size() == 2, "expected two physical summary ranges");
  Require(page.ranges[0].range.first_page_id == 0 &&
              page.ranges[0].range.page_count == 2,
          "first range identity drifted");
  Require(page.ranges[1].range.first_page_id == 2 &&
              page.ranges[1].range.page_count == 2,
          "second range identity drifted");
  const auto& c0 = Column(page.ranges[0], 0);
  const auto& c1 = Column(page.ranges[0], 1);
  Require(c0.row_count == 3 && c0.null_count == 0, "integer column counts drifted");
  Require(c0.encoded_min == IntKey(10) && c0.encoded_max == IntKey(20),
          "integer min/max drifted");
  Require(c1.row_count == 3 && c1.null_count == 1, "text null count drifted");
  Require(c1.small_set_exact && c1.small_set_values.size() == 2,
          "text small-set summary missing or unbounded");
  Require(page.ranges[0].multi_columns.size() == 1,
          "multi-column physical summary missing");
  const auto& mc0 = page.ranges[0].multi_columns[0];
  Require(mc0.column_ordinals == std::vector<platform::u32>({0, 1}),
          "multi-column summary shape drifted");
  Require(mc0.row_count == 3 && mc0.null_tuple_count == 1,
          "multi-column tuple counts drifted");
  Require(mc0.min_present && mc0.max_present &&
              mc0.encoded_min_tuple == CompositeKey({IntKey(10), TextKey("red")}) &&
              mc0.encoded_max_tuple == CompositeKey({IntKey(20), TextKey("blue")}),
          "multi-column tuple min/max drifted");
  Require(mc0.small_set_exact && mc0.small_set_values.size() == 2,
          "multi-column exact small-set summary missing");
  Require(Column(page.ranges[1], 0).encoded_min == IntKey(30) &&
              Column(page.ranges[1], 0).encoded_max == IntKey(40),
          "second range integer min/max drifted");
}

idx::PhysicalZoneSummaryPage VerifySerializationReopen(
    const idx::PhysicalZoneSummaryPage& page) {
  const auto serialized = idx::SerializePhysicalZoneSummaryPage(page);
  Require(serialized.ok(), "summary serialization failed");
  const auto serialized_again = idx::SerializePhysicalZoneSummaryPage(page);
  Require(serialized_again.ok(), "second serialization failed");
  Require(serialized.bytes == serialized_again.bytes,
          "summary serialization is not deterministic");

  idx::PhysicalZoneSummaryOpenRequest open_request;
  open_request.bytes = serialized.bytes;
  open_request.expected_base_generation_present = true;
  open_request.expected_base_generation = 7;
  const auto opened = idx::OpenPhysicalZoneSummaryPage(open_request);
  Require(opened.ok(), "clean reopen failed");
  const auto reserialized = idx::SerializePhysicalZoneSummaryPage(opened.page);
  Require(reserialized.ok() && reserialized.bytes == serialized.bytes,
          "reopen serialization equivalence failed");
  return opened.page;
}

void VerifyPrune(const idx::PhysicalZoneSummaryPage& page) {
  idx::PhysicalZonePredicate int_range;
  int_range.column_ordinal = 0;
  int_range.scalar_type_key = "sb.i64";
  int_range.lower_present = true;
  int_range.upper_present = true;
  int_range.encoded_lower = IntKey(25);
  int_range.encoded_upper = IntKey(35);
  const auto pruned = idx::PrunePhysicalZoneSummaryRanges({page, {int_range}});
  Require(pruned.ok(), "prune result failed closed unexpectedly");
  Require(pruned.any_pruned, "no range was pruned by min/max");
  Require(pruned.ranges.size() == 2, "prune evidence range count drifted");
  Require(pruned.ranges[0].decision ==
              idx::PhysicalZoneSummaryRangeDecision::pruned_by_min_max,
          "first range was not pruned by min/max");
  Require(pruned.ranges[1].decision ==
              idx::PhysicalZoneSummaryRangeDecision::scan_with_recheck,
          "second range did not retain scan with recheck");
  for (const auto& range : pruned.ranges) {
    Require(range.mga_recheck_required && range.security_recheck_required,
            "summary pruning dropped MGA/security recheck evidence");
  }

  idx::PhysicalZonePredicate text_range;
  text_range.column_ordinal = 1;
  text_range.scalar_type_key = "sb.text";
  text_range.lower_present = true;
  text_range.upper_present = true;
  text_range.encoded_lower = TextKey("cyan");
  text_range.encoded_upper = TextKey("cyan");
  const auto small_set_pruned = idx::PrunePhysicalZoneSummaryRanges({page, {text_range}});
  Require(small_set_pruned.ok(), "small-set prune failed");
  Require(small_set_pruned.ranges[0].decision ==
              idx::PhysicalZoneSummaryRangeDecision::pruned_by_small_set,
          "small-set did not prune absent low-cardinality value");

  idx::PhysicalZonePredicate composite_int;
  composite_int.column_ordinal = 0;
  composite_int.scalar_type_key = "sb.i64";
  composite_int.lower_present = true;
  composite_int.upper_present = true;
  composite_int.encoded_lower = IntKey(15);
  composite_int.encoded_upper = IntKey(15);
  idx::PhysicalZonePredicate composite_text;
  composite_text.column_ordinal = 1;
  composite_text.scalar_type_key = "sb.text";
  composite_text.lower_present = true;
  composite_text.upper_present = true;
  composite_text.encoded_lower = TextKey("red");
  composite_text.encoded_upper = TextKey("red");
  const auto composite_pruned =
      idx::PrunePhysicalZoneSummaryRanges({page, {composite_int, composite_text}});
  Require(composite_pruned.ok(), "multi-column small-set prune failed");
  Require(composite_pruned.ranges[0].decision ==
              idx::PhysicalZoneSummaryRangeDecision::pruned_by_small_set,
          "multi-column exact small-set did not prune absent tuple");
}

void VerifyMutationAndRepair(const idx::PhysicalZoneSummaryPage& page) {
  idx::PhysicalZoneSummaryMutation append;
  append.kind = idx::PhysicalZoneSummaryMutationKind::append_row;
  append.after_row_present = true;
  append.after_row = Row(3, 8, {IntColumn(50), TextColumn("orange")});
  const auto appended = idx::ApplyPhysicalZoneSummaryMutation(page, append, 3);
  Require(appended.ok(), "append summary maintenance failed");
  Require(Column(appended.page.ranges[1], 0).encoded_max == IntKey(50),
          "append did not widen range max");

  idx::PhysicalZoneSummaryMutation boundary_delete;
  boundary_delete.kind = idx::PhysicalZoneSummaryMutationKind::delete_row;
  boundary_delete.before_row_present = true;
  boundary_delete.before_row = Row(0, 8, {IntColumn(10), TextColumn("red")});
  const auto refused = idx::ApplyPhysicalZoneSummaryMutation(appended.page,
                                                            boundary_delete,
                                                            3);
  Require(!refused.applied && refused.summary_invalidated && refused.full_scan_required,
          "boundary delete did not fail closed without rebuild evidence");

  auto repaired_rows = BaseRows();
  repaired_rows.erase(repaired_rows.begin());
  repaired_rows.push_back(Row(3, 8, {IntColumn(50), TextColumn("orange")}));
  boundary_delete.rebuild_admitted = true;
  boundary_delete.authoritative_base_page_rows = repaired_rows;
  const auto repaired = idx::ApplyPhysicalZoneSummaryMutation(appended.page,
                                                             boundary_delete,
                                                             3);
  Require(repaired.applied && repaired.rebuild_performed,
          "admitted repair from base evidence failed");
  Require(Column(repaired.page.ranges[0], 0).encoded_min == IntKey(15),
          "repair did not rebuild exact min from base evidence");
}

void VerifyCorruptStaleRepair(const idx::PhysicalZoneSummaryPage& page) {
  const auto serialized = idx::SerializePhysicalZoneSummaryPage(page);
  Require(serialized.ok(), "corruption fixture serialization failed");

  auto bad_checksum = serialized.bytes;
  bad_checksum.back() ^= 0x01;
  auto bad_open = idx::OpenPhysicalZoneSummaryPage({bad_checksum});
  Require(bad_open.open_class == idx::PhysicalZoneSummaryOpenClass::bad_checksum &&
              bad_open.restricted_repair_required,
          "bad checksum was not classified as restricted repair");

  auto stale_format = serialized.bytes;
  platform::StoreLittle32(stale_format.data() + 8, 0);
  platform::StoreLittle64(stale_format.data() + 16, 0);
  platform::StoreLittle64(stale_format.data() + 16, 123);
  auto stale_open = idx::OpenPhysicalZoneSummaryPage({stale_format});
  Require(stale_open.open_class == idx::PhysicalZoneSummaryOpenClass::stale_format,
          "stale format was not refused");

  std::vector<platform::byte> truncated(serialized.bytes.begin(),
                                        serialized.bytes.end() - 3);
  auto corrupt_open = idx::OpenPhysicalZoneSummaryPage({truncated});
  Require(corrupt_open.open_class == idx::PhysicalZoneSummaryOpenClass::bad_checksum ||
              corrupt_open.open_class == idx::PhysicalZoneSummaryOpenClass::corrupt_payload,
          "corrupt payload was not refused");

  idx::PhysicalZoneSummaryOpenRequest stale_generation_request;
  stale_generation_request.bytes = serialized.bytes;
  stale_generation_request.expected_base_generation_present = true;
  stale_generation_request.expected_base_generation = 99;
  const auto stale_generation = idx::OpenPhysicalZoneSummaryPage(stale_generation_request);
  Require(stale_generation.open_class == idx::PhysicalZoneSummaryOpenClass::stale_generation,
          "stale generation was not refused");

  const auto refused_repair = idx::RepairPhysicalZoneSummaryFromBasePageEvidence(
      page, BaseRows(), false, 3);
  Require(!refused_repair.applied && refused_repair.full_scan_required,
          "repair without admission did not fail closed");
  const auto admitted_repair = idx::RepairPhysicalZoneSummaryFromBasePageEvidence(
      page, BaseRows(), true, 3);
  Require(admitted_repair.applied && admitted_repair.rebuild_performed,
          "admitted repair from authoritative base evidence failed");
}

void VerifyUnsafeLegacyRefusal() {
  idx::PhysicalZoneSummaryBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.summary_uuid = "22222222-2222-7222-8222-222222222222";
  request.range_sizing = Sizing();
  idx::PhysicalZoneColumnValueEvidence unsafe;
  unsafe.column_ordinal = 0;
  unsafe.scalar_type_key = "sb.i64";
  unsafe.encoded_scalar = "SBK1legacy";
  request.base_page_rows = {Row(0, 7, {unsafe})};
  const auto built = idx::BuildPhysicalZoneSummaryFromBasePageEvidence(request);
  Require(!built.ok(), "unsafe legacy key encoding was accepted");
}

void VerifyNoExecution_PlanRuntimeArtifacts(const idx::PhysicalZoneSummaryPage& page) {
  for (const auto& evidence : page.evidence) {
    Require(!ContainsForbiddenRuntimeArtifact(evidence),
            "runtime evidence contains execution_plan/doc artifact token");
  }
  const auto serialized = idx::SerializePhysicalZoneSummaryPage(page);
  Require(serialized.ok(), "no-execution_plan serialization fixture failed");
  const std::string payload(reinterpret_cast<const char*>(serialized.bytes.data()),
                            serialized.bytes.size());
  Require(!ContainsForbiddenRuntimeArtifact(payload),
          "serialized runtime payload contains execution_plan/doc artifact token");
}

}  // namespace

int main() {
  const auto page = BuildPage();
  VerifyBuildAndColumns(page);
  const auto reopened = VerifySerializationReopen(page);
  VerifyPrune(reopened);
  VerifyMutationAndRepair(reopened);
  VerifyCorruptStaleRepair(reopened);
  VerifyUnsafeLegacyRefusal();
  VerifyNoExecution_PlanRuntimeArtifacts(reopened);
  return EXIT_SUCCESS;
}
