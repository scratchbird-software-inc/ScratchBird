// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_binary.hpp"
#include "row_data_page.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::byte;
using platform::u32;
using platform::u64;

inline constexpr u64 kBaseMillis = 1770000000000ull;
inline constexpr u32 kBodyChecksumOffset = 32;
inline constexpr u32 kHeaderFreeSpaceBytesOffset = 92;
inline constexpr u32 kRowVersionOffset = 40;
inline constexpr u32 kSlotEntryBytes = 24;
inline constexpr u32 kSlotStableSlotIdOffset = 0;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

dt::DatatypeBinaryValue TextValue(std::string_view text) {
  dt::DatatypeBinaryValue value;
  value.type_id = dt::CanonicalTypeId::character;
  value.payload.assign(text.begin(), text.end());
  return value;
}

dt::DatatypeBinaryValue Int32Value(platform::u32 value) {
  dt::DatatypeBinaryValue binary;
  binary.type_id = dt::CanonicalTypeId::int32;
  binary.payload.assign(4, 0);
  platform::StoreLittle32(binary.payload.data(), value);
  return binary;
}

page::RowDataRecord Row(TypedUuid row_uuid,
                        TypedUuid transaction_uuid,
                        u64 local_transaction_id,
                        u32 stable_slot_id,
                        u32 row_version,
                        bool deleted,
                        std::string_view text) {
  page::RowDataRecord row;
  row.row_uuid = row_uuid;
  row.transaction_uuid = transaction_uuid;
  row.local_transaction_id = local_transaction_id;
  row.stable_slot_id = stable_slot_id;
  row.row_version = row_version;
  row.deleted = deleted;
  row.cells.push_back({1, TextValue(text)});
  row.cells.push_back({2, Int32Value(row_version)});
  return row;
}

page::RowDataPageBody FixtureBody() {
  page::RowDataPageBody body;
  body.relation_uuid = MakeUuid(UuidKind::object, 10);
  body.segment_id = 3;
  body.segment_generation = 17;
  body.page_number = 42;
  body.page_generation = 9;
  body.compaction_generation = 11;
  body.next_page_number = 43;

  const TypedUuid row_a = MakeUuid(UuidKind::row, 20);
  body.rows.push_back(Row(row_a,
                          MakeUuid(UuidKind::transaction, 30),
                          1001,
                          101,
                          1,
                          false,
                          "inserted row payload"));
  body.rows.push_back(Row(row_a,
                          MakeUuid(UuidKind::transaction, 31),
                          1002,
                          202,
                          2,
                          false,
                          "updated row payload"));
  body.rows.push_back(Row(MakeUuid(UuidKind::row, 21),
                          MakeUuid(UuidKind::transaction, 32),
                          1003,
                          303,
                          3,
                          true,
                          "deleted row tombstone"));
  return body;
}

void RefreshBodyChecksum(std::vector<byte>* serialized) {
  platform::StoreLittle64(serialized->data() + kBodyChecksumOffset,
                          page::ComputeRowDataPageChecksum(*serialized));
}

bool HasDiagnostic(const page::RowDataPageResult& result,
                   std::string_view diagnostic_code) {
  return !result.ok() && result.diagnostic.diagnostic_code == diagnostic_code;
}

bool SlottedLayoutRoundTrips() {
  bool ok = true;
  const page::RowDataPageBody body = FixtureBody();
  const auto built = page::BuildRowDataPageBody(body, 8192);
  ok = Expect(built.ok(), "slotted row page body should build") && ok;
  if (!built.ok()) {
    return false;
  }

  const auto parsed = page::ParseRowDataPageBody(built.serialized,
                                                 body.page_number);
  ok = Expect(parsed.ok(), "slotted row page body should parse") && ok;
  if (!parsed.ok()) {
    return false;
  }
  ok = Expect(parsed.body.rows.size() == 3,
              "slotted row page should preserve row count") && ok;
  ok = Expect(parsed.body.slots.size() == parsed.body.rows.size(),
              "slot directory should have one entry per row") && ok;
  if (parsed.body.rows.size() != 3 ||
      parsed.body.slots.size() != parsed.body.rows.size()) {
    return false;
  }

  ok = Expect(parsed.body.compaction_generation == body.compaction_generation,
              "compaction generation should round trip") && ok;
  ok = Expect(parsed.body.free_space_offset > page::kRowDataPageBodyHeaderBytes,
              "free-space offset should follow serialized rows") && ok;
  ok = Expect(parsed.body.free_space_bytes > 0,
              "free-space byte count should be recorded") && ok;
  ok = Expect(parsed.body.free_space_offset + parsed.body.free_space_bytes ==
                  built.serialized.size(),
              "free-space accounting should cover the body buffer exactly") && ok;

  for (std::size_t index = 0; index < parsed.body.rows.size(); ++index) {
    const page::RowDataRecord& row = parsed.body.rows[index];
    const page::RowDataSlot& slot = parsed.body.slots[index];
    ok = Expect(row.internal_row_ordinal == index + 1,
                "dense internal row ordinal should remain positional") && ok;
    ok = Expect(row.stable_slot_id == (index + 1) * 101,
                "stable slot id should round trip independently") && ok;
    ok = Expect(slot.stable_slot_id == row.stable_slot_id,
                "slot entry should carry stable slot id") && ok;
    ok = Expect(slot.row_offset >= page::kRowDataPageBodyHeaderBytes,
                "slot entry should point at a row body") && ok;
    ok = Expect(slot.row_bytes > 0, "slot entry should carry row bytes") && ok;
    ok = Expect(slot.row_offset + slot.row_bytes <= parsed.body.free_space_offset,
                "slot row extent should stay before free space") && ok;
    ok = Expect(slot.row_checksum != 0,
                "slot entry should carry row checksum") && ok;
    ok = Expect(slot.deleted == row.deleted,
                "slot deleted flag should match row tombstone state") && ok;
  }

  ok = Expect(parsed.body.rows[1].row_uuid.value ==
                  parsed.body.rows[0].row_uuid.value,
              "updated row version should preserve durable row UUID") && ok;
  ok = Expect(parsed.body.rows[1].transaction_uuid.value !=
                  parsed.body.rows[0].transaction_uuid.value,
              "updated row version should carry distinct transaction UUID") && ok;
  ok = Expect(parsed.body.rows[1].row_version == 2,
              "updated row version should round trip") && ok;
  ok = Expect(parsed.body.rows[2].deleted,
              "tombstone row should round trip") && ok;

  const auto scope = page::MakeDenseRowOrdinalScope(parsed.body);
  const auto locator =
      page::MakeDenseRowOrdinalLocator(scope, parsed.body.rows[1], true, true);
  const auto accepted =
      page::ValidateDenseRowOrdinalLocator(parsed.body, locator);
  ok = Expect(accepted.accepted,
              "dense row locator should validate with MGA authority") && ok;
  ok = Expect(!accepted.ordinal_is_visibility_or_finality_authority,
              "dense row locator should not become finality authority") && ok;

  auto missing_mga = locator;
  missing_mga.durable_mga_inventory_authority_available = false;
  const auto refused_missing_mga =
      page::ValidateDenseRowOrdinalLocator(parsed.body, missing_mga);
  ok = Expect(!refused_missing_mga.accepted,
              "dense row locator should fail closed without durable MGA") && ok;
  ok = Expect(refused_missing_mga.durable_mga_inventory_remains_authority,
              "MGA inventory should remain authority after locator refusal") && ok;

  auto stale_ordinal = locator;
  stale_ordinal.internal_row_ordinal = 1;
  const auto refused_stale =
      page::ValidateDenseRowOrdinalLocator(parsed.body, stale_ordinal);
  ok = Expect(!refused_stale.accepted,
              "stale dense row ordinal should fail closed") && ok;
  return ok;
}

bool CorruptionFailsClosed() {
  bool ok = true;
  const auto built = page::BuildRowDataPageBody(FixtureBody(), 8192);
  ok = Expect(built.ok(), "corruption fixture should build") && ok;
  if (!built.ok()) {
    return false;
  }
  const auto parsed = page::ParseRowDataPageBody(built.serialized,
                                                 FixtureBody().page_number);
  ok = Expect(parsed.ok(), "corruption fixture should parse") && ok;
  if (!parsed.ok() || parsed.body.slots.empty()) {
    return false;
  }

  std::vector<byte> row_corrupt = built.serialized;
  row_corrupt[parsed.body.slots[0].row_offset + kRowVersionOffset] ^= 0x01u;
  RefreshBodyChecksum(&row_corrupt);
  const auto row_refused =
      page::ParseRowDataPageBody(row_corrupt, FixtureBody().page_number);
  ok = Expect(HasDiagnostic(row_refused,
                            "SB-ROW-DATA-PAGE-ROW-CHECKSUM-MISMATCH"),
              "row checksum corruption should fail closed before acceptance") && ok;

  const u32 slot_directory_offset =
      parsed.body.slots.back().row_offset + parsed.body.slots.back().row_bytes;
  std::vector<byte> slot_corrupt = built.serialized;
  slot_corrupt[slot_directory_offset + kSlotStableSlotIdOffset] ^= 0x01u;
  RefreshBodyChecksum(&slot_corrupt);
  const auto slot_refused =
      page::ParseRowDataPageBody(slot_corrupt, FixtureBody().page_number);
  ok = Expect(HasDiagnostic(slot_refused,
                            "SB-ROW-DATA-PAGE-SLOT-DIRECTORY-MISMATCH"),
              "slot directory mismatch should fail closed") && ok;

  std::vector<byte> free_space_corrupt = built.serialized;
  platform::StoreLittle32(free_space_corrupt.data() + kHeaderFreeSpaceBytesOffset,
                          0);
  RefreshBodyChecksum(&free_space_corrupt);
  const auto free_space_refused =
      page::ParseRowDataPageBody(free_space_corrupt, FixtureBody().page_number);
  ok = Expect(HasDiagnostic(free_space_refused,
                            "SB-ROW-DATA-PAGE-SLOT-DIRECTORY-INVALID"),
              "free-space accounting mismatch should fail closed") && ok;

  ok = Expect(slot_directory_offset + parsed.body.slots.size() * kSlotEntryBytes ==
                  parsed.body.free_space_offset,
              "slot directory should terminate at free-space offset") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = SlottedLayoutRoundTrips() && ok;
  ok = CorruptionFailsClosed() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
