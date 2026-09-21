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
using Uuid = scratchbird::core::platform::Uuid;
constexpr Uuid Fixed(unsigned suffix) {
  return {{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,
      static_cast<unsigned char>(suffix>>8),static_cast<unsigned char>(suffix)}}};
}
constexpr Uuid boolean_identity{{{0x01,0,0,0,0x62,0x6f,0x7f,0x6c,0xa5,0x61,0x6e,0,0,0,0,0}}};
constexpr Uuid decimal_descriptor{{{0xa0,0,0,0,0x64,0x65,0x73,0x69,0xad,0x61,0x6c,0,0,0,0,0}}};
struct Expected { Uuid descriptor; Uuid type; const char* codec; };
constexpr std::array<Expected, 6> expected{{
  {boolean_identity, boolean_identity, "datatype.boolean.u8.v1"},
  {Fixed(0xd716), Fixed(0xd717), "datatype.int32.le.v1"},
  {Fixed(0xd711), Fixed(0xd712), "datatype.int64.le.v1"},
  {decimal_descriptor, Fixed(0xd713), "datatype.decimal.base1e9.le.v1"},
  {Fixed(0xd714), Fixed(0xd715), "datatype.int128.le.v1"},
  {Fixed(0xd718), Fixed(0xd719), "datatype.text.utf8.v1"},
}};
constexpr Uuid receipt = Fixed(0x4901);
constexpr Uuid snapshot = Fixed(0xd701);
constexpr Uuid binding = Fixed(0x4902);
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
            case 9: invalid.statement_receipt_uuid = {}; break;
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
      case 0: invalid.descriptor_uuid.bytes[6] = 0x40; break;
      case 1: invalid.type_uuid.bytes[8] = 0; break;
      case 2: invalid.nullability = sb::BoundNullability::kUnknown; break;
      case 3: invalid.nullability = static_cast<sb::BoundNullability>(200); break;
      case 4: altered.literal_catalog_snapshot_uuid = binding; break;
      case 5: altered.literal_catalog_generation = 2; break;
      case 6: altered.literal_preliminary_receipt_uuid = {}; break;
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
    namespace api = scratchbird::engine::internal_api;
    api::RelationalTypeDescriptor lowered;
    lowered.descriptor_uuid=binding;lowered.descriptor_generation=1;
    lowered.type_uuid=expected[index].type;lowered.type_generation=1;
    lowered.codec_id=expected[index].codec;lowered.codec_version=1;lowered.codec_generation=1;
    lowered.nullability=api::RelationalNullability::kNonNull;
    lowered.precision=numeric.width_precision_scale.precision;lowered.scale=numeric.width_precision_scale.scale;
    lowered.statement_receipt_uuid=receipt;lowered.datatype_catalog_snapshot_uuid=snapshot;
    lowered.datatype_catalog_generation=1;lowered.datatype_registry_generation=1;lowered.datatype_identity_authoritative=true;
    Check(sb::MatchesNativeNumericDescriptorRecord(lowered,numeric),"exact binary numeric descriptor tuple");
    for(unsigned field=0;field<18;++field){auto altered=lowered;
      switch(field){
        case 0: altered.descriptor_uuid=receipt;break;case 1: ++altered.descriptor_generation;break;
        case 2: altered.type_uuid=receipt;break;case 3: ++altered.type_generation;break;
        case 4: altered.codec_id="substitution";break;case 5: ++altered.codec_version;break;
        case 6: ++altered.codec_generation;break;case 7: altered.nullability=api::RelationalNullability::kNullable;break;
        case 8: altered.collation_uuid=receipt;break;case 9: altered.timezone_profile_id="substitution";break;
        case 10: altered.width=12;break;case 11: altered.precision=12;break;case 12: altered.scale=3;break;
        case 13: altered.statement_receipt_uuid=binding;break;case 14: altered.datatype_catalog_snapshot_uuid=binding;break;
        case 15: ++altered.datatype_catalog_generation;break;case 16: ++altered.datatype_registry_generation;break;
        case 17: altered.datatype_identity_authoritative=false;break;
      }
      Check(!sb::MatchesNativeNumericDescriptorRecord(altered,numeric),"every binary numeric descriptor field checked");
    }
    // Typed records have no text-field truncation. Exercise every missing
    // mandatory authority value instead of restoring a removed text interface.
    for(unsigned field=0;field<12;++field){auto altered=lowered;
      switch(field){
        case 0: altered.descriptor_uuid={};break;case 1: altered.descriptor_generation=0;break;
        case 2: altered.type_uuid={};break;case 3: altered.type_generation=0;break;
        case 4: altered.codec_id.clear();break;case 5: altered.codec_version=0;break;
        case 6: altered.codec_generation=0;break;case 7: altered.statement_receipt_uuid={};break;
        case 8: altered.datatype_catalog_snapshot_uuid={};break;case 9: altered.datatype_catalog_generation=0;break;
        case 10: altered.datatype_registry_generation=0;break;case 11: altered.datatype_identity_authoritative=false;break;
      }
      Check(!sb::MatchesNativeNumericDescriptorRecord(altered,numeric),"missing binary numeric authority value refuses");
    }
  }
  Check(checks == 6441, "fixed assertion population");
  std::cout << "PASS checks=" << checks << "; component projection only; not runtime admission\n";
}
