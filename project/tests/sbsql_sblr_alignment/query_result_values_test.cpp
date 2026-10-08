// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "engine/sblr/canonical_query_result_values.hpp"
#include "sbl_numeric.hpp"
#include "datatype_binary.hpp"
#include "datatype_binary_view.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "canonical_utf8.hpp"
#include "wire/typed_update_carrier_codec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace engine = scratchbird::engine::sblr;
namespace wire = scratchbird::wire;
namespace dt = scratchbird::core::datatypes;
namespace numeric = scratchbird::libraries::sbl_numeric;
namespace {
std::size_t checks = 0;
void Check(bool value, const std::string& label) {
  ++checks;
  if (!value) throw std::runtime_error(label);
}
wire::TypedResultUuid Bytes(const api::EngineUuid& identity) {
  return identity.bytes;
}
struct Vector {
  api::EngineUuid descriptor;
  api::EngineUuid type;
  const char* codec;
  dt::CanonicalTypeId code;
  unsigned width;
  std::string lexical;
  std::vector<std::uint8_t> bytes;
};
// Independent fixed identities and binary values; no registry/encoder lookup
// participates in these expected values. These are synthetic internal contexts,
// not receipt issuance, public-route or all-datatype acceptance evidence.
const std::array<Vector, 14> vectors{{
  {scratchbird::tests::FixtureUuidLiteral("01000000-626f-7f6c-a561-6e0000000000"), scratchbird::tests::FixtureUuidLiteral("01000000-626f-7f6c-a561-6e0000000000"), "datatype.boolean.u8.v1", dt::CanonicalTypeId::boolean, 1, "true", {1}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d716"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d717"), "datatype.int32.le.v1", dt::CanonicalTypeId::int32, 4, "-2147483648", {0,0,0,128}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d711"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d712"), "datatype.int64.le.v1", dt::CanonicalTypeId::int64, 8, "-9223372036854775808", {0,0,0,0,0,0,0,128}},
  {scratchbird::tests::FixtureUuidLiteral("a0000000-6465-7369-ad61-6c0000000000"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d713"), "datatype.decimal.base1e9.le.v1", dt::CanonicalTypeId::decimal, 24, "-12.34", {130,4,1,0,210,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d714"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d715"), "datatype.int128.le.v1", dt::CanonicalTypeId::int128, 16, "-170141183460469231731687303715884105728", {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,128}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d718"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d719"), "datatype.text.utf8.v1", dt::CanonicalTypeId::character, 0, std::string("9;=\0é",7), {57,59,61,0,195,169,0}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d731"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d732"), "datatype.real64.ieee754.le.v1", dt::CanonicalTypeId::real64, 8, "1.5", {0,0,0,0,0,0,248,63}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d734"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d735"), "datatype.uuid.binary16.v1", dt::CanonicalTypeId::uuid, 16, "", {0,124,10,59,61,255,0,0,0,0,0,0,0,0,0,0}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d737"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d738"), "datatype.geometry.sbp1.v1", dt::CanonicalTypeId::geometry, 0, "", {'S','B','P','1',1,2,0,0,63,240,0,0,0,0,0,0,64,0,0,0,0,0,0,0}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d73a"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d73b"), "datatype.uint64.le.v1", dt::CanonicalTypeId::uint64, 8, "18446744073709551615", {255,255,255,255,255,255,255,255}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d73d"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d73e"), "datatype.json.utf8.v1", dt::CanonicalTypeId::json_document, 0, "[true,null]", {'[','t','r','u','e',',','n','u','l','l',']'}},
  {scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d740"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d741"), "datatype.list.text.framed.v1", dt::CanonicalTypeId::list, 0, std::string("SBTL0001\x02\x00\x00\x00\x01\x03\x00\x00\x00;]\0\x00\x00\x00\x00\x00",25), {'S','B','T','L','0','0','0','1',2,0,0,0,1,3,0,0,0,';',']',0,0,0,0,0,0}}
  ,{scratchbird::tests::FixtureUuidLiteral("2d010000-6269-7e61-b279-000000000000"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d743"), "datatype.binary.octets.v1", dt::CanonicalTypeId::binary, 0, std::string("\0\xff\x10",3), {0,255,16}}
  ,{scratchbird::tests::FixtureUuidLiteral("92010000-7469-7d65-b374-616d70000000"), scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d820"), "datatype.timestamp.utc_tuple.le.v1", dt::CanonicalTypeId::timestamp, 16, "1969-12-31T23:59:59.999999999Z", {255,255,255,255,255,255,255,255,255,201,154,59,0,0,0,0}}
}};
struct Fixture {
  api::EngineRequestContext context;
  api::EngineResultShape shape;
  std::shared_ptr<api::EngineQueryResultMetadataV1> metadata = std::make_shared<api::EngineQueryResultMetadataV1>();
  std::string code, detail;
  Fixture(std::size_t type = 2, std::size_t rows = 1, std::size_t columns = 1,
          bool binary = false, bool null = false) {
    context.statement_receipt_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000001001");
    context.statement_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000001002");
    context.datatype_catalog_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d701");
    if (type >= 6) context.datatype_catalog_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d702");
    context.datatype_catalog_generation = context.datatype_registry_generation = type >= 6 ? 2 : 1;
    if (type == 12) {
      context.datatype_catalog_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d703");
      context.datatype_catalog_generation = context.datatype_registry_generation = 3;
    }
    if (type == 13) {
      context.datatype_catalog_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d704");
      context.datatype_catalog_generation = context.datatype_registry_generation = 4;
    }
    context.maximum_typed_result_transport_bytes_per_packet = 65536;
    metadata->statement_receipt_uuid = Bytes(context.statement_receipt_uuid);
    metadata->statement_snapshot_uuid = Bytes(context.statement_snapshot_uuid);
    metadata->datatype_catalog_snapshot_uuid = Bytes(context.datatype_catalog_snapshot_uuid);
    metadata->datatype_catalog_generation = metadata->datatype_registry_generation = context.datatype_catalog_generation;
    const auto& v = vectors[type];
    for (std::size_t i = 0; i < columns; ++i) {
      api::EngineQueryResultColumnV1 c;
      c.bound_descriptor_uuid = Bytes(v.descriptor);
      c.transport = {static_cast<unsigned>(i),static_cast<unsigned>(i),"same;=é",wire::TypedResultNullability::nullable,
                     Bytes(v.descriptor),1,Bytes(v.type),1,v.code,v.codec,1,1,v.width};
      if (type == 3) { c.precision = 10; c.scale = 2; }
      metadata->columns.push_back(c);
      api::EngineDescriptor d;
      d.descriptor_uuid = v.descriptor;
      d.type_uuid = v.type;
      d.canonical_type_name = "not_type_authority";
      shape.columns.push_back(d);
    }
    for (std::size_t r = 0; r < rows; ++r) {
      api::EngineRowValue row;
      for (const auto& d : shape.columns) {
        api::EngineTypedValue value;
        value.descriptor = d;
        if (null) value.setState(api::EngineValueState::sql_null);
        else if (binary) value.binary_value = v.bytes;
        else value.encoded_value = v.lexical;
        row.fields.emplace_back("same;=é", std::move(value));
      }
      shape.rows.push_back(std::move(row));
    }
    shape.result_kind = "rows";
    shape.query_metadata = metadata;
  }
  bool Run() { return engine::PreserveCanonicalQueryResultValuesV1(context, &shape, &code, &detail); }
  api::EngineTypedValue& Value() { return shape.rows[0].fields[0].second; }
};
template<class Mutation>
void Reject(std::size_t type, Mutation mutation, const char* code = "DATATYPE.DESCRIPTOR.INVALID") {
  Fixture f(type);
  Check(f.Run(), "negative baseline");
  mutation(f);
  Check(!f.Run(), "negative must refuse");
  Check(!f.shape.query_values, "no stale or partial typed publication");
  Check(f.code == code && !f.detail.empty(), "canonical refusal identity");
}
}

int main() try {
  Fixture historical_timestamp(13);
  const auto historical_identity = dt::LookupDatatypeTypeCodecIdentityV1(
      historical_timestamp.context.datatype_catalog_snapshot_uuid, 4, 4,
      vectors[13].descriptor, 1);
  Check(historical_identity.ok, "historical UTC exact registry identity");
  std::array<std::uint8_t,48> historical_frame{};
  const dt::DatatypeBinaryValueView historical_view{dt::CanonicalTypeId::timestamp,
      false,false,vectors[13].bytes.data(),16};
  const auto historical_encoded = dt::EncodeHistoricalTimestampUtcValueIntoV1(
      historical_identity.row,historical_view,historical_frame.data(),historical_frame.size());
  Check(historical_encoded.ok() && historical_encoded.bytes_written == historical_frame.size(),
        "bound historical UTC SBDVAL frame encoded: " +
        historical_encoded.diagnostic.diagnostic_code + ":" +
        historical_encoded.diagnostic.message_key);
  const auto historical_decoded = dt::DecodeHistoricalTimestampUtcValueViewV1(
      historical_identity.row,historical_frame.data(),historical_frame.size());
  Check(historical_decoded.ok() && historical_decoded.value.payload_bytes == 16 &&
        std::equal(vectors[13].bytes.begin(),vectors[13].bytes.end(),
                   historical_decoded.value.payload_data), "bound historical UTC frame round trip");
  for (unsigned size = 0; size < historical_frame.size(); ++size) {
    std::array<std::uint8_t,48> target;
    target.fill(0xa5);
    const auto before = target;
    Check(!dt::EncodeHistoricalTimestampUtcValueIntoV1(historical_identity.row,
              historical_view,target.data(),size).ok() && target == before,
          "historical UTC short destination remains untouched");
    auto corrupt = historical_frame;
    corrupt[size] ^= 1;
    const auto rejected = dt::DecodeHistoricalTimestampUtcValueViewV1(
        historical_identity.row,corrupt.data(),corrupt.size());
    Check(!rejected.ok() && rejected.value.payload_data == nullptr &&
          rejected.value.payload_bytes == 0, "historical UTC corrupt frame exposes no view");
  }
  // Independent Gregorian calendar oracle, not the production temporal parser.
  for (const int year : {1,4,100,400,1600,1900,1969,1970,2000,2100,2400,9999})
  for (unsigned month = 1; month <= 12; ++month)
  for (unsigned day = 1; day <= 31; ++day) {
    const std::chrono::year_month_day date{std::chrono::year(year),
        std::chrono::month(month), std::chrono::day(day)};
    std::ostringstream lexical;
    lexical << std::setfill('0') << std::setw(4) << year << '-'
            << std::setw(2) << month << '-' << std::setw(2) << day
            << "T12:34:56.123456789Z";
    Fixture f(13);
    f.Value().encoded_value = lexical.str();
    Check(f.Run() == date.ok(), "timestamp Gregorian day admission");
    if (!date.ok()) continue;
    const auto seconds = static_cast<std::uint64_t>(
        std::chrono::sys_days(date).time_since_epoch().count() * 86400LL + 45296);
    std::vector<std::uint8_t> expected(16,0);
    for (unsigned i = 0; i < 8; ++i) expected[i] = static_cast<std::uint8_t>(seconds >> (8*i));
    expected[8]=21; expected[9]=205; expected[10]=91; expected[11]=7;
    Check(f.shape.query_values->rows[0].cells[0].canonical_payload == expected,
          "timestamp exact signed seconds/nanos/reserved-byte oracle");
  }
  for (const char* invalid : {"", "1970-01-01", "0000-01-01T00:00:00Z",
       "1970-01-01 00:00:00Z", "1970-01-01T00:00:00z", "1970-01-01T00:00:00+00:00",
       "1970-01-01T24:00:00Z", "1970-01-01T00:60:00Z", "1970-01-01T00:00:60Z",
       "1970-01-01T00:00:00.Z", "1970-01-01T00:00:00.0000000001Z",
       "1970-01-01T00:00:00Z ", " 1970-01-01T00:00:00Z", "<NULL>"})
    Reject(13, [&](auto& f) { f.Value().encoded_value = invalid; });
  for (unsigned size = 0; size <= 32; ++size) {
    if (size == 16) continue;
    Fixture f(13,1,1,true); f.Value().binary_value.resize(size);
    Check(!f.Run(), "timestamp exact binary extent");
  }
  for (unsigned bit = 0; bit < 32; ++bit) {
    Fixture f(13,1,1,true);
    f.Value().binary_value[12 + bit/8] = static_cast<std::uint8_t>(1U << (bit%8));
    Check(!f.Run(), "timestamp every reserved bit refused");
  }
  for (std::uint32_t nanos : {0U,1U,999999999U,1000000000U,0xffffffffU}) {
    Fixture f(13,1,1,true);
    for (unsigned i = 0; i < 4; ++i) f.Value().binary_value[8+i] = nanos >> (8*i);
    Check(f.Run() == (nanos < 1000000000U), "timestamp nanosecond range");
    dt::DatatypeBinaryValue value;
    value.type_id = dt::CanonicalTypeId::timestamp; value.payload = f.Value().binary_value;
    Check(dt::ValidateHistoricalTimestampUtcValueViewV1(historical_identity.row,
              {value.type_id,false,false,value.payload.data(),value.payload.size()}).ok()
              == (nanos < 1000000000U),
          "bound historical timestamp validator uses same canonical range");
    Check(!dt::ValidateDatatypeBinaryValue(value).ok(),
          "enum-only timestamp cannot establish temporal profile authority");
  }
  for (std::int64_t seconds : {std::numeric_limits<std::int64_t>::min(),
                              std::int64_t{-185542587187201}, std::int64_t{-1},
                              std::int64_t{0}, std::int64_t{185542587187200},
                              std::numeric_limits<std::int64_t>::max()}) {
    Fixture f(13,1,1,true);
    for (unsigned i = 0; i < 8; ++i)
      f.Value().binary_value[i] = static_cast<std::uint64_t>(seconds) >> (8*i);
    Check(f.Run(), "historical UTC seconds are not current civil coordinates");
    Check(f.shape.query_values->rows[0].cells[0].canonical_payload == f.Value().binary_value,
          "historical UTC full signed domain preserved exactly");
  }
  for (unsigned mutation = 0; mutation < 18; ++mutation) {
    auto identity = historical_identity.row;
    switch (mutation) {
      case 0: identity.catalog_snapshot_uuid = {}; break;
      case 1: ++identity.catalog_generation; break;
      case 2: ++identity.registry_generation; break;
      case 3: identity.descriptor_uuid = {}; break;
      case 4: ++identity.descriptor_generation; break;
      case 5: identity.type_uuid = {}; break;
      case 6: ++identity.type_generation; break;
      case 7: identity.codec_uuid = {}; break;
      case 8: ++identity.codec_generation; break;
      case 9: ++identity.codec_version; break;
      case 10: identity.codec_id += "wrong"; break;
      case 11: --identity.canonical_value_bytes; break;
      case 12: identity.canonical_name += "wrong"; break;
      case 13: ++identity.canonical_binary_type_code; break;
      case 14: identity.canonical_representation += "wrong"; break;
      case 15: identity.null_supported = !identity.null_supported; break;
      case 16: identity.numeric_context_generation = 1; break;
      case 17: identity.comparison_profile += "wrong"; break;
    }
    for (bool null : {false,true}) {
      const auto result = dt::ValidateHistoricalTimestampUtcValueViewV1(identity,
          {dt::CanonicalTypeId::timestamp,null,false,
           null ? nullptr : vectors[13].bytes.data(),null ? std::size_t{0} : std::size_t{16}});
      Check(!result.ok() && result.diagnostic.diagnostic_code == "CTI.TEMPORAL.DESCRIPTOR_INVALID",
            "complete historical identity checked before present/NULL payload");
    }
    std::array<std::uint8_t,48> target;
    target.fill(0xa5);
    const auto before = target;
    Check(!dt::EncodeHistoricalTimestampUtcValueIntoV1(identity,historical_view,
              target.data(),target.size()).ok() && target == before,
          "historical identity failure cannot modify frame destination");
    const auto decoded = dt::DecodeHistoricalTimestampUtcValueViewV1(
        identity,historical_frame.data(),historical_frame.size());
    Check(!decoded.ok() && decoded.value.payload_data == nullptr &&
          decoded.value.payload_bytes == 0,
          "historical identity failure cannot expose decoded frame");
  }
  const auto current_identity = dt::LookupDatatypeTypeCodecIdentityV3(
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d710"),
      10,10,vectors[13].descriptor,1);
  Check(current_identity.ok, "current local-civil identity exists independently");
  Check(!dt::ValidateHistoricalTimestampUtcValueViewV1(current_identity.row.legacy_fields,
            {dt::CanonicalTypeId::timestamp,false,false,
             vectors[13].bytes.data(),16}).ok(),
        "current local-civil identity never admits historical UTC bytes");
  for (const auto view : {
       dt::DatatypeBinaryValueView{dt::CanonicalTypeId::timestamp,false,false,nullptr,16},
       dt::DatatypeBinaryValueView{dt::CanonicalTypeId::timestamp,false,true,vectors[13].bytes.data(),16},
       dt::DatatypeBinaryValueView{dt::CanonicalTypeId::timestamp,true,false,vectors[13].bytes.data(),16},
       dt::DatatypeBinaryValueView{dt::CanonicalTypeId::timestamp,true,true,nullptr,0}})
    Check(!dt::ValidateHistoricalTimestampUtcValueViewV1(historical_identity.row,view).ok(),
          "historical UTC malformed borrowed/NULL/TOAST state refused");
  Check(dt::ValidateHistoricalTimestampUtcValueViewV1(historical_identity.row,
            {dt::CanonicalTypeId::timestamp,true,false,nullptr,0}).ok(),
        "historical UTC clean containing NULL supported");
  for (unsigned mutation = 0; mutation < 6; ++mutation) {
    dt::DatatypeBinaryValue value;
    value.type_id = dt::CanonicalTypeId::geometry;
    value.payload = vectors[8].bytes;
    if (mutation == 0) value.payload.pop_back();
    if (mutation == 1) value.payload.push_back(0);
    if (mutation == 2) value.payload[6] = 1;
    if (mutation == 3) { value.payload[8] = 0x7f; value.payload[9] = 0xf0; }
    if (mutation == 4) { value.payload[8] = 0xff; value.payload[9] = 0xf8; }
    if (mutation == 5) { std::fill(value.payload.begin()+8,value.payload.begin()+16,0); value.payload[8] = 0x80; }
    Check(!dt::ValidateDatatypeBinaryValue(value).ok(), "malformed SBP1 refused at binary codec boundary");
  }
  // Independent malformed structured payloads, including ambiguity, UTF8,
  // bounds and depth. Rejection must precede any typed row publication.
  for (const auto& json : {"", "[1,]", "01", "1.", "1e+", "{x:1}", "true false", "\"\\uD800\"", "\"\\uDC00\"", "[NaN]"}) {
    Fixture f(10); f.Value().encoded_value = json;
    Check(!f.Run(), "malformed JSON refused");
  }
  for (const auto& json : {"null", "-1.2e+3", " {\"x\": [true, false]} ", "\"\\uD834\\uDD1E\""}) {
    Fixture f(10); f.Value().encoded_value = json;
    Check(f.Run(), "valid JSON grammar admitted");
  }
  {
    Fixture f(10); f.Value().encoded_value = std::string(257,'[')+"0"+std::string(257,']');
    Check(!f.Run(), "JSON nesting bound enforced");
    f.Value().encoded_value = std::string("\"\xc0\x80\"",4);
    Check(!f.Run(), "JSON noncanonical UTF8 refused");
  }
  for (std::size_t n = 0; n < vectors[11].bytes.size(); ++n) {
    Fixture f(11,1,1,true); f.Value().binary_value.resize(n);
    Check(!f.Run(), "every text-list truncation refused");
  }
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    Fixture f(11,1,1,true); auto& bytes = f.Value().binary_value;
    if (mutation == 0) bytes.push_back(0);
    if (mutation == 1) bytes[8] = 255;
    if (mutation == 2) bytes[12] = 2;
    if (mutation == 3) bytes[13] = 255;
    if (mutation == 4) bytes[17] = 255;
    Check(!f.Run(), "malformed text-list state/bounds/UTF8 refused");
  }
  for (const auto text : {"-1", "18446744073709551616", "1x"}) {
    Fixture f(9); f.Value().encoded_value = text;
    Check(!f.Run(), "UINT64 overflow or malformed value refused");
  }
  for (bool binary : {false, true}) {
    for (std::size_t size : {0U, 1U, 128U, 256U, 1024U}) {
      Fixture f(12,1,1,binary);
      std::vector<std::uint8_t> bytes(size);
      for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(i);
      f.Value().encoded_value.clear(); f.Value().binary_value.clear();
      if (binary) f.Value().binary_value = bytes;
      else f.Value().encoded_value.assign(bytes.begin(), bytes.end());
      Check(f.Run(), "BINARY all octets and empty value publish");
      Check(f.shape.query_values->rows[0].cells[0].canonical_payload == bytes,
            "BINARY payload is not text or hexadecimal");
      Check(f.shape.query_values->rows[0].cells[0].state == wire::TypedResultValueState::value_present,
            "empty BINARY is not NULL");
      if (size > 0) {
        f.metadata->columns[0].width = size - 1;
        Check(!f.Run(), "declared BINARY width enforced without truncation");
      }
    }
  }
  Reject(12, [](auto& f) { f.Value().binary_value = {0,255,16}; });
  constexpr std::size_t expected = 12 * 2 * 2 * 3 * 3 + 2 * 2 * 3 * 3;
  std::cout << "expected_value_tuples=" << expected << '\n';
  std::size_t observed = 0;
  for (std::size_t type = 0; type < vectors.size(); ++type)
  for (bool binary : {false, true})
  for (bool null : {false, true})
  for (std::size_t rows : {0U,1U,3U})
  for (std::size_t columns : {1U,2U,4U}) {
    if ((type == 7 || type == 8) && !binary) continue;  // Binary string carriers are checked separately below.
    Fixture f(type, rows, columns, binary, null);
    Check(f.Run(), f.detail);
    const auto owned = f.shape.query_values;
    Check(owned && owned->metadata == f.shape.query_metadata && owned->rows.size() == rows, "retained exact schema and row extent");
    for (std::size_t r = 0; r < rows; ++r) {
      Check(owned->rows[r].row_ordinal == r && owned->rows[r].cells.size() == columns, "whole-result row order and extent");
      for (std::size_t c = 0; c < columns; ++c) {
        const auto& cell = owned->rows[r].cells[c];
        Check(cell.column_ordinal == c && cell.name_occurrence == c, "duplicate-name cells remain ordered");
        Check(cell.state == (null ? wire::TypedResultValueState::sql_null : wire::TypedResultValueState::value_present), "exact SQL NULL state");
        Check(cell.canonical_payload == (null ? std::vector<std::uint8_t>{} : vectors[type].bytes), "independent exact canonical bytes");
      }
    }

    wire::TypedResultRowDescriptor descriptor;
    descriptor.descriptor_uuid = Bytes(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002001"));
    descriptor.descriptor_generation = 1;
    descriptor.datatype_catalog_snapshot_uuid = f.metadata->datatype_catalog_snapshot_uuid;
    descriptor.datatype_catalog_generation = descriptor.datatype_registry_generation = f.context.datatype_catalog_generation;
    for (const auto& c : f.metadata->columns) descriptor.columns.push_back(c.transport);
    if (type == 13) {
      for (unsigned mutation = 0; mutation < 10; ++mutation) {
        auto invalid = descriptor;
        auto& column = invalid.columns[0];
        switch (mutation) {
          case 0: invalid.datatype_catalog_snapshot_uuid[15] ^= 1; break;
          case 1: ++invalid.datatype_catalog_generation; break;
          case 2: ++invalid.datatype_registry_generation; break;
          case 3: column.descriptor_uuid[15] ^= 1; break;
          case 4: ++column.descriptor_generation; break;
          case 5: column.type_uuid[15] ^= 1; break;
          case 6: ++column.type_generation; break;
          case 7: column.codec_id += "wrong"; break;
          case 8: ++column.codec_generation; break;
          case 9: ++column.codec_version; break;
        }
        Check(!wire::EncodeTypedResultRowDescriptor(invalid).ok(),
              "historical wire identity required independently of row/NULL count");
      }
    }
    const auto encoded_descriptor = wire::EncodeTypedResultRowDescriptor(descriptor);
    Check(encoded_descriptor.ok(), "all six registry codec widths encode including DECIMAL");
    const auto decoded_descriptor = wire::DecodeTypedResultRowDescriptor(encoded_descriptor.encoded);
    Check(decoded_descriptor.ok(), "all six descriptor byte decoders agree");
    if (rows != 0) {
      wire::TypedResultBatch batch;
      batch.execution_uuid = Bytes(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002002"));
      batch.result_set_uuid = Bytes(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002003"));
      batch.batch_uuid = Bytes(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002004"));
      batch.snapshot_uuid = f.metadata->statement_snapshot_uuid;
      batch.row_descriptor_uuid = descriptor.descriptor_uuid;
      batch.row_descriptor_generation = 1;
      batch.descriptor_evidence_sha256 = encoded_descriptor.descriptor.descriptor_evidence_sha256;
      batch.rows = owned->rows;
      batch.end_of_rowset = true;
      // Independently supplied synthetic outer carrier; not authentication.
      wire::TypedResultCarrierBinding carrier;
      carrier.kind = wire::TypedResultCarrierKind::ps_execute_result_v1;
      carrier.row_count = rows;
      carrier.end_of_rowset = true;
      carrier.execution_uuid = Bytes(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002002"));
      carrier.result_set_uuid = Bytes(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000002003"));
      carrier.snapshot_uuid = Bytes(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000001002"));
      const auto encoded = wire::EncodeTypedResultBatch(batch, encoded_descriptor.descriptor, carrier);
      Check(encoded.ok(), "canonical cells accepted by actual row packet encoder");
      const auto decoded = wire::DecodeTypedResultBatch(encoded.encoded, encoded_descriptor.descriptor, carrier);
      Check(decoded.ok() && decoded.batch.rows.size() == rows, "actual packet decode extent");
      const auto& expected_bytes = null ? std::vector<std::uint8_t>{} : vectors[type].bytes;
      Check(encoded.encoded.size() >= 292 + expected_bytes.size() &&
            std::equal(expected_bytes.begin(), expected_bytes.end(), encoded.encoded.begin() + 292),
            "independent first-cell payload offset and exact bytes");
      Check(encoded.encoded[252] == (null ? 1 : 0) && encoded.encoded[272] == (null ? 1 : 0),
            "independent cell-state and envelope NULL flag offsets");
    }
    f.shape.rows.clear(); f.shape.columns.clear(); f.shape.query_metadata.reset(); f.metadata.reset();
    Check(owned->rows.size() == rows && owned->metadata->columns.size() == columns, "owned result survives input release");
    ++observed;
  }
  Check(observed == expected, "expected-versus-observed finite tuple count");
  Reject(2, [](auto& f) { f.shape.query_metadata.reset(); });
  Reject(2, [](auto& f) { f.context.statement_receipt_uuid.bytes.back() ^= 1; });
  Reject(2, [](auto& f) { f.metadata->statement_snapshot_uuid[15] ^= 1; });
  Reject(2, [](auto& f) { f.metadata->datatype_catalog_snapshot_uuid[15] ^= 1; });
  Reject(2, [](auto& f) { ++f.context.datatype_catalog_generation; });
  Reject(2, [](auto& f) { ++f.metadata->datatype_registry_generation; });
  Reject(2, [](auto& f) { f.metadata->columns[0].bound_descriptor_uuid[15] ^= 1; });
  Reject(2, [](auto& f) { f.metadata->columns[0].transport.descriptor_uuid[15] ^= 1; });
  Reject(2, [](auto& f) { ++f.metadata->columns[0].transport.descriptor_generation; });
  Reject(2, [](auto& f) { f.metadata->columns[0].transport.type_uuid[15] ^= 1; });
  Reject(2, [](auto& f) { ++f.metadata->columns[0].transport.type_generation; });
  Reject(2, [](auto& f) { ++f.metadata->columns[0].transport.codec_generation; });
  Reject(2, [](auto& f) { ++f.metadata->columns[0].transport.codec_version; });
  Reject(2, [](auto& f) { f.metadata->columns[0].transport.codec_id += ".fake"; });
  Reject(2, [](auto& f) { f.metadata->columns[0].transport.canonical_type_id = dt::CanonicalTypeId::character; });
  Reject(2, [](auto& f) { ++f.metadata->columns[0].transport.canonical_value_bytes; });
  Reject(2, [](auto& f) { ++f.metadata->columns[0].transport.name_occurrence; });
  Reject(2, [](auto& f) { ++f.metadata->columns[0].transport.ordinal; });
  Reject(2, [](auto& f) { f.shape.rows[0].fields[0].first = "other"; });
  Reject(2, [](auto& f) { f.shape.rows[0].fields.clear(); });
  Reject(2, [](auto& f) { f.Value().descriptor.canonical_type_name = "other"; });
  Reject(2, [](auto& f) { f.Value().binary_value.assign(8, 0); });
  for (unsigned state = 2; state < 10; ++state)
    Reject(2, [state](auto& f) { f.Value().state = static_cast<api::EngineValueState>(state); });
  Reject(2, [](auto& f) { f.Value().setState(api::EngineValueState::sql_null); });
  Reject(2, [](auto& f) {
    f.Value().encoded_value.clear(); f.Value().setState(api::EngineValueState::sql_null);
    f.metadata->columns[0].transport.nullability = wire::TypedResultNullability::not_null;
  });
  for (std::size_t type : {1U,2U,4U}) {
    for (const std::string text : {"", "-0", "+1", "01", "1 ", " 1", "1x", "--1"})
      Reject(type, [&](auto& f) { f.Value().encoded_value = text; });
    for (std::size_t n = 1; n <= 25; ++n) if (n != vectors[type].width)
      Reject(type, [n](auto& f) { f.Value().encoded_value.clear(); f.Value().binary_value.assign(n, 0); });
  }
  Reject(1, [](auto& f) { f.Value().encoded_value = "2147483648"; });
  Reject(1, [](auto& f) { f.Value().encoded_value = "-2147483649"; });
  Reject(2, [](auto& f) { f.Value().encoded_value = "9223372036854775808"; });
  Reject(2, [](auto& f) { f.Value().encoded_value = "-9223372036854775809"; });
  Reject(4, [](auto& f) { f.Value().encoded_value = "170141183460469231731687303715884105728"; });
  Reject(4, [](auto& f) { f.Value().encoded_value = "-170141183460469231731687303715884105729"; });
  for (unsigned byte = 2; byte <= 255; ++byte)
    Reject(0, [byte](auto& f) { f.Value().encoded_value.clear(); f.Value().binary_value = {static_cast<std::uint8_t>(byte)}; });
  for (const std::string text : {"0", "1", "TRUE", "False", " true"})
    Reject(0, [&](auto& f) { f.Value().encoded_value = text; });
  for (std::size_t offset : {0U,1U,2U,3U,7U,23U})
    Reject(3, [offset](auto& f) { f.Value().encoded_value.clear(); f.Value().binary_value = vectors[3].bytes; f.Value().binary_value[offset] = 255; });
  Reject(3, [](auto& f) { f.metadata->columns[0].precision.reset(); });
  for (std::size_t rows : {0U,1U})
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    Fixture f(3,rows,1,false,true);
    switch (invalid) {
      case 0: f.metadata->columns[0].precision.reset(); break;
      case 1: f.metadata->columns[0].scale.reset(); break;
      case 2: f.metadata->columns[0].precision = 0; break;
      case 3: f.metadata->columns[0].precision = 39; break;
      case 4: f.metadata->columns[0].scale = 11; break;
    }
    Check(!f.Run() && !f.shape.query_values && f.code == "DATATYPE.DESCRIPTOR.INVALID",
          "empty or NULL decimal results still require valid type modifiers");
  }
  Reject(3, [](auto& f) { f.metadata->columns[0].scale = 1; });
  Reject(3, [](auto& f) { f.metadata->columns[0].precision = 3; });
  for (const std::string text : {"nan", "1.2.3", "1e100", "", "infinity"})
    Reject(3, [&](auto& f) { f.Value().encoded_value = text; });
  for (unsigned byte = 128; byte <= 255; ++byte)
    Reject(5, [byte](auto& f) { f.Value().encoded_value.assign(1, static_cast<char>(byte)); });
  for (const std::string& bytes : {std::string("\xc0\x80",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4)})
    Reject(5, [&](auto& f) { f.Value().encoded_value = bytes; });
  { Fixture f(5); f.Value().encoded_value.clear(); Check(f.Run(), "empty TEXT is a value");
    Check(f.shape.query_values->rows[0].cells[0].canonical_payload.empty() && f.shape.query_values->rows[0].cells[0].state == wire::TypedResultValueState::value_present, "empty TEXT not NULL"); }
  { Fixture f(5); f.metadata->columns[0].width = 6; Check(f.Run(), "TEXT width counts scalars not bytes"); }
  Reject(5, [](auto& f) { f.metadata->columns[0].width = 5; });
  for (auto ceiling : {0ULL,16777217ULL,299ULL})
    Reject(2, [ceiling](auto& f) { f.context.maximum_typed_result_transport_bytes_per_packet = ceiling; }, "RESOURCE.BUDGET_EXCEEDED");
  { Fixture f; f.context.maximum_typed_result_transport_bytes_per_packet = 300; Check(f.Run(), "exact single-row packet boundary admits long numeric spelling"); }
  { Fixture f(2, 100); f.context.maximum_typed_result_transport_bytes_per_packet = 300;
    Check(f.Run() && f.shape.query_values->rows.size() == 100, "packet ceiling is not total-query limit"); }
  for (std::size_t phase = 1; phase <= 13; ++phase) {
    Fixture f(2, 3, 2); std::size_t polls = 0;
    f.context.query_cancellation_requested = [&] { return ++polls == phase; };
    Check(!f.Run() && !f.shape.query_values && f.code == "PROCESS.CANCELLED", "cancellation before partial publication");
  }
  for (const auto& [text, bytes] : std::vector<std::pair<std::string,std::vector<std::uint8_t>>>{
      {"0",std::vector<std::uint8_t>(16,0)}, {"-1",std::vector<std::uint8_t>(16,255)},
      {"170141183460469231731687303715884105727",{255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,127}},
      {vectors[4].lexical,vectors[4].bytes}}) {
    const auto encoded = numeric::EncodeInt128LittleEndian(text);
    Check(encoded.status == numeric::NumericStatusCode::ok && encoded.payload == bytes, "independent signed128 bytes");
    const auto decoded = numeric::DecodeInt128LittleEndian(bytes);
    Check(decoded.status == numeric::NumericStatusCode::ok && decoded.value.encoded == text, "signed128 existing decoder parity");
  }

  // Independently enumerate all two-octet combinations: either two ASCII
  // scalars or one shortest-form two-octet scalar. No producer output oracle.
  for (unsigned first = 0; first < 256; ++first)
  for (unsigned second = 0; second < 256; ++second) {
    std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(first),static_cast<std::uint8_t>(second)};
    const bool ascii = first < 128 && second < 128;
    const bool multibyte = first >= 194 && first <= 223 && second >= 128 && second <= 191;
    const bool expected_valid = ascii || multibyte;
    std::uint64_t scalars = 99, update_scalars = 99;
    Check(dt::ValidateCanonicalUtf8(bytes.data(), bytes.size(), &scalars) == expected_valid,
          "complete two-byte canonical UTF8 truth table");
    Check(wire::ValidateTypedUpdateTextUtf8V2(bytes, &update_scalars) == expected_valid &&
          update_scalars == scalars, "actual update consumer shares UTF8 authority");
    Check(scalars == (ascii ? 2U : multibyte ? 1U : 0U), "exact scalar count including NUL");
    const std::string label(bytes.begin(), bytes.end());
    Check(wire::ValidTypedResultColumnName(label) == (expected_valid && first != 0 && second != 0),
          "result name restriction distinct from value codec");
    dt::DatatypeBinaryValue value;
    value.type_id = dt::CanonicalTypeId::character; value.payload = bytes;
    Check(dt::ValidateDatatypeBinaryValue(value).ok() == expected_valid,
          "actual generic binary envelope validates TEXT semantics");
  }
  std::size_t decimal_profiles = 0;
  for (unsigned precision = 1; precision <= 38; ++precision)
  for (unsigned scale = 0; scale <= precision; ++scale) {
    Fixture f(3);
    f.metadata->columns[0].precision = precision;
    f.metadata->columns[0].scale = scale;
    f.Value().encoded_value = "0";
    Check(f.Run(), "canonical zero fits every admitted DECIMAL precision/scale");
    std::vector<std::uint8_t> zero(24,0); zero[1] = zero[2] = 1;
    Check(f.shape.query_values->rows[0].cells[0].canonical_payload == zero,
          "zero coefficient uses canonical scale0 independently of declared scale");
    f.Value().encoded_value = "1";
    Check(f.Run() == (precision > scale), "integer coefficient obeys declared integer capacity");
    f.Value().encoded_value = "0.1";
    Check(f.Run() == (scale > 0), "fractional coefficient obeys declared scale");
    ++decimal_profiles;
  }
  Check(decimal_profiles == 779, "complete finite DECIMAL precision/scale grid");
  for (const std::string text : {"1D","1DECIMAL","1_000","1e2","+1","-0"})
    Reject(3, [&](auto& f) { f.Value().encoded_value = text; });
  { dt::DatatypeBinaryValue value; value.type_id = dt::CanonicalTypeId::decimal;
    value.payload.assign(16,0);
    Check(!dt::EncodeDatatypeBinaryValue(value).ok(), "storage descriptor is not a decimal value");
    value.payload = vectors[3].bytes;
    Check(dt::EncodeDatatypeBinaryValue(value).ok(), "24-byte decimal envelope positive");
    value.payload[3] = 1;
    Check(!dt::EncodeDatatypeBinaryValue(value).ok(), "decimal reserved byte rejected by generic envelope");
  }
  for (const std::size_t type : {7U, 8U}) {
    Fixture f(type, 1, 1, true);
    Check(f.Run(), "binary-only datatype baseline");
    f.Value().binary_value.clear();
    f.Value().encoded_value.assign(vectors[type].bytes.begin(), vectors[type].bytes.end());
    Check(f.Run() && f.shape.query_values->rows[0].cells[0].canonical_payload == vectors[type].bytes,
          "retained binary string carrier preserves exact UUID/geometry bytes");
    f.Value().encoded_value = type == 7 ? "019d0000-0000-7000-8000-00000000d734" : "POINT(1 2)";
    Check(!f.Run() && !f.shape.query_values, "UUID/geometry text carrier refused atomically");
    f.Value().encoded_value.clear(); f.Value().binary_value = vectors[type].bytes;
    f.Value().binary_value.pop_back();
    Check(!f.Run(), "truncated binary datatype payload refused");
    f.Value().binary_value = vectors[type].bytes;
    f.context.datatype_catalog_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d701");
    f.context.datatype_catalog_generation = f.context.datatype_registry_generation = 1;
    Check(!f.Run(), "successor value cannot cross predecessor cohort");
  }
  { Fixture f(8, 1, 1, true); f.Value().binary_value[6] = 1;
    Check(!f.Run(), "geometry reserved byte refuses"); }
  { Fixture f(8, 1, 1, true); f.Value().binary_value[8] = 127; f.Value().binary_value[9] = 240;
    Check(!f.Run(), "nonfinite geometry coordinate refuses"); }
  { Fixture f(7, 1, 1, true); f.Value().binary_value.assign(16, 0);
    Check(f.Run(), "UUID value nil is distinct from forbidden nil system identity"); }
  std::cout << "PASS tuples=" << observed << " checks=" << checks << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "FAIL check=" << checks << " " << e.what() << '\n';
  return 1;
}
