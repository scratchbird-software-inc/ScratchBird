// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "parsers/sbsql_worker/binder/descriptor_authority.hpp"
#include <array>
#include <cstdlib>
#include <iostream>

namespace sb = scratchbird::parser::sbsql;
namespace ipc = scratchbird::parser::ipc;
namespace dt = scratchbird::core::datatypes;
namespace {
std::size_t checks = 0;
void Check(bool pass, const char* why) {
  ++checks;
  if (!pass) { std::cerr << "FAIL checks=" << checks << ' ' << why << '\n'; std::exit(1); }
}
struct Expected { const char* descriptor; const char* type; const char* codec; };
constexpr std::array<Expected, 6> expected{{
  {"01000000-626f-7f6c-a561-6e0000000000", "01000000-626f-7f6c-a561-6e0000000000", "datatype.boolean.u8.v1"},
  {"019d0000-0000-7000-8000-00000000d716", "019d0000-0000-7000-8000-00000000d717", "datatype.int32.le.v1"},
  {"019d0000-0000-7000-8000-00000000d711", "019d0000-0000-7000-8000-00000000d712", "datatype.int64.le.v1"},
  {"a0000000-6465-7369-ad61-6c0000000000", "019d0000-0000-7000-8000-00000000d713", "datatype.decimal.base1e9.le.v1"},
  {"019d0000-0000-7000-8000-00000000d714", "019d0000-0000-7000-8000-00000000d715", "datatype.int128.le.v1"},
  {"019d0000-0000-7000-8000-00000000d718", "019d0000-0000-7000-8000-00000000d719", "datatype.text.utf8.v1"},
}};
constexpr const char* receipt = "019d0000-0000-7000-8000-000000004901";
constexpr const char* snapshot = "019d0000-0000-7000-8000-00000000d701";
constexpr const char* binding = "019d0000-0000-7000-8000-000000004902";
ipc::ParserStatementContext Context() {
  ipc::ParserStatementContext c;
  c.literal_preliminary_receipt_uuid = receipt;
  c.literal_catalog_snapshot_uuid = snapshot;
  c.literal_catalog_generation = 1;
  return c;
}
sb::NativeDescriptorBindingInput Descriptor(const Expected& row, bool nullable) {
  sb::NativeDescriptorBindingInput d;
  d.descriptor_id = 7;
  d.descriptor_uuid = binding;
  d.type_uuid = row.type;
  d.canonical_type_name = "deliberately_not_a_type_or_authority";
  d.nullability = nullable ? sb::BoundNullability::kNullable : sb::BoundNullability::kNonNull;
  return d;
}
void Exact(const sb::NativeDescriptorBindingInput& d, const Expected& row) {
  Check(d.descriptor_id == 7 && d.descriptor_uuid == binding && d.type_uuid == row.type,
        "bound identity retained separately from registry identity");
  Check(d.descriptor_generation == 1 && d.type_generation == 1 &&
        d.codec_id == row.codec && d.codec_version == 1 && d.codec_generation == 1,
        "exact independent type codec generation oracle");
  Check(d.statement_receipt_uuid == receipt && d.datatype_catalog_snapshot_uuid == snapshot &&
        d.datatype_catalog_generation == 1 && d.datatype_registry_generation == 1,
        "actual projected receipt and immutable registry cohort");
}
}
int main() {
  std::cout << "EXPECTED statement_profiles=384 literal_profiles=12 registry_rows=6\n";
  Check(dt::CurrentDatatypeTypeCodecIdentityRowsV1().size() == 6, "fixed registry population");
  for (const auto& row : expected) {
    const auto lookup = dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 1, 1, row.descriptor, 1);
    Check(lookup.ok && lookup.row.type_uuid == row.type && lookup.row.codec_id == row.codec,
          "old exact lookup and shared registry parity");
    const auto stale = dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 1, 2, row.descriptor, 1);
    Check(!stale.ok && stale.diagnostic_id == "DATATYPE.DESCRIPTOR.INVALID",
          "no nearest registry generation and exact canonical diagnostic");
    for (bool nullable : {false, true}) {
      for (std::uint16_t slot = 0; slot < 32; ++slot) {
        auto c = Context();
        auto d = Descriptor(row, nullable);
        ipc::ParserStatementContext::DescriptorProfile p;
        p.slot = slot; p.profile_kind = 1; p.descriptor_uuid = binding;
        p.type_uuid = row.type; p.nullable = nullable;
        c.descriptor_profiles.push_back(p);
        Check(sb::PreserveNativeDescriptorAuthority(&d, c), "statement descriptor projection");
        Exact(d, row);
        Check(sb::PreserveNativeDescriptorAuthority(&d, c), "complete descriptor exact revalidation");
        for (int mutation = 0; mutation < 10; ++mutation) {
          auto invalid = d;
          switch (mutation) {
            case 0: invalid.descriptor_generation = 0; break;
            case 1: invalid.type_generation = 2; break;
            case 2: invalid.codec_id = "datatype.fake.v1"; break;
            case 3: invalid.codec_version = 2; break;
            case 4: invalid.codec_generation = 2; break;
            case 5: invalid.statement_receipt_uuid = binding; break;
            case 6: invalid.datatype_catalog_snapshot_uuid = binding; break;
            case 7: invalid.datatype_catalog_generation = 2; break;
            case 8: invalid.datatype_registry_generation = 2; break;
            case 9: invalid.statement_receipt_uuid.clear(); break;
          }
          Check(!sb::PreserveNativeDescriptorAuthority(&invalid, c), "partial/stale authority cannot be defaulted");
        }
        auto invalid = Descriptor(row, nullable);
        c.descriptor_profiles.push_back(p);
        Check(!sb::PreserveNativeDescriptorAuthority(&invalid, c) && invalid.codec_id.empty(),
              "duplicate issued handle refuses without publication");
      }
      auto c = Context();
      auto d = Descriptor(row, nullable);
      ipc::ParserStatementContext::LiteralStatementDescriptorProfileV1 p;
      p.profile_version = 1; p.binding_descriptor_uuid = binding;
      p.statement_receipt_uuid = receipt; p.catalog_snapshot_uuid = snapshot;
      p.catalog_generation = 1; p.descriptor_uuid = row.descriptor;
      p.descriptor_generation = 1; p.type_uuid = row.type;
      p.codec_id = row.codec; p.codec_version = 1; p.codec_generation = 1;
      p.nullable = nullable;
      c.literal_statement_descriptor_profiles.push_back(p);
      Check(sb::PreserveNativeDescriptorAuthority(&d, c), "literal authenticated projection retained");
      Exact(d, row);
      for (int mutation = 0; mutation < 13; ++mutation) {
        auto invalid = Descriptor(row, nullable);
        auto altered = c;
        auto& bad = altered.literal_statement_descriptor_profiles.front();
        switch (mutation) {
          case 0: bad.profile_version = 0; break;
          case 1: bad.binding_descriptor_uuid = receipt; break;
          case 2: bad.statement_receipt_uuid = binding; break;
          case 3: bad.catalog_snapshot_uuid = binding; break;
          case 4: bad.catalog_generation = 2; break;
          case 5: bad.descriptor_uuid = binding; break;
          case 6: bad.descriptor_generation = 2; break;
          case 7: bad.type_uuid = binding; break;
          case 8: bad.codec_id = "datatype.fake.v1"; break;
          case 9: bad.codec_version = 2; break;
          case 10: bad.codec_generation = 2; break;
          case 11: bad.nullable = !nullable; break;
          case 12: altered.literal_statement_descriptor_profiles.push_back(p); break;
        }
        Check(!sb::PreserveNativeDescriptorAuthority(&invalid, altered) && invalid.codec_id.empty(),
              "literal tuple substitution refuses atomically");
      }
    }
  }
  auto c = Context();
  auto d = Descriptor(expected[2], false);
  Check(!sb::PreserveNativeDescriptorAuthority(&d, c), "unissued descriptor handle");
  Check(!sb::PreserveNativeDescriptorAuthority(nullptr, c), "null destination");
  ipc::ParserStatementContext::DescriptorProfile profile;
  profile.descriptor_uuid = binding;
  profile.type_uuid = expected[2].type;
  c.descriptor_profiles.push_back(profile);
  auto valid = d;
  Check(sb::PreserveNativeDescriptorAuthority(&valid, c), "invalid-case baseline is admitted");
  for (int mutation = 0; mutation < 8; ++mutation) {
    auto invalid = d;
    auto altered = c;
    auto& bad = altered.descriptor_profiles.front();
    switch (mutation) {
      case 0: bad.type_uuid = binding; break;
      case 1: bad.nullable = true; break;
      case 2: bad.width = 12; break;
      case 3: bad.precision = 12; break;
      case 4: bad.scale = 2; break;
      case 5: bad.collation_uuid = receipt; break;
      case 6: invalid.timezone_profile_id = "not_projected"; break;
      case 7: invalid.descriptor_uuid = receipt; break;
    }
    Check(!sb::PreserveNativeDescriptorAuthority(&invalid, altered) && invalid.codec_id.empty(),
          "statement profile substitution refuses atomically");
  }
  for (int mutation = 0; mutation < 7; ++mutation) {
    auto invalid = d;
    auto altered = c;
    switch (mutation) {
      case 0: invalid.descriptor_uuid[14] = '4'; break;
      case 1: invalid.type_uuid[19] = '0'; break;
      case 2: invalid.nullability = sb::BoundNullability::kUnknown; break;
      case 3: invalid.nullability = static_cast<sb::BoundNullability>(200); break;
      case 4: altered.literal_catalog_snapshot_uuid = binding; break;
      case 5: altered.literal_catalog_generation = 2; break;
      case 6: altered.literal_preliminary_receipt_uuid.clear(); break;
    }
    Check(!sb::PreserveNativeDescriptorAuthority(&invalid, altered), "invalid cohort/system identity");
  }
  for (std::size_t index : {2U, 3U}) {
    auto numeric = Descriptor(expected[index], false);
    numeric.descriptor_generation = 1; numeric.type_generation = 1;
    numeric.codec_id = expected[index].codec; numeric.codec_version = 1;
    numeric.codec_generation = 1; numeric.statement_receipt_uuid = receipt;
    numeric.datatype_catalog_snapshot_uuid = snapshot;
    numeric.datatype_catalog_generation = 1; numeric.datatype_registry_generation = 1;
    if (index == 3) {
      numeric.width_precision_scale.precision = 7;
      numeric.width_precision_scale.scale = 2;
    }
    std::array<std::string_view, 17> fields{{
        binding, "1", expected[index].type, "1", expected[index].codec, "1", "1",
        "0", "-", "-", "-", index == 3 ? "7" : "-", index == 3 ? "2" : "-",
        receipt, snapshot, "1", "1"}};
    Check(sb::MatchesNativeNumericDescriptorRecord(fields, numeric), "exact numeric SBXN descriptor tuple");
    for (std::size_t field = 0; field < fields.size(); ++field) {
      auto mutated = fields;
      mutated[field] = "substitution";
      Check(!sb::MatchesNativeNumericDescriptorRecord(mutated, numeric), "every SBXN descriptor field checked");
    }
    for (std::size_t size = 0; size < fields.size(); ++size)
      Check(!sb::MatchesNativeNumericDescriptorRecord(std::span(fields).first(size), numeric),
            "every numeric descriptor truncation");
  }
  Check(checks == 6449, "fixed assertion population");
  std::cout << "PASS checks=" << checks << "; component projection only; not runtime admission\n";
}
