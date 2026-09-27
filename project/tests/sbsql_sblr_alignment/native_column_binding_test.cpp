// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/sblr/sblr_bound_column_identity.hpp"
#include "../sbsql_parser_worker/canonical_sblr_admission_test_helper.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"
#include "core/datatypes/admitted_datatype_cohort.hpp"
#include "parsers/sbsql_worker/wire/native_insert_literals.hpp"
#include "engine/internal_api/dml/insert_batch.hpp"
#include "engine/internal_api/crud_support/native_value_payload.hpp"
#include <cstdlib>
#include <iostream>

namespace s = scratchbird::engine::sblr;
using scratchbird::tests::FixtureUuid;
void Check(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
  namespace api = scratchbird::engine::internal_api;
  for (unsigned pattern : {0U, 255U, 0x55U}) {
    api::EngineTypedValue id, payload;
    id.descriptor.canonical_type_name = "uuid";
    id.binary_value.assign(16, static_cast<std::uint8_t>(pattern));
    payload.descriptor.canonical_type_name = "binary";
    payload.binary_value = {255, 0, 1};
    api::EngineRowValue row; row.fields = {{"payload",payload},{"id",id}};
    api::InsertRowEncoderPlan plan;
    plan.columns.resize(2); plan.columns[0].column_name = "id"; plan.columns[1].column_name = "payload";
    api::BoundInsertRowTemplate row_template;
    const auto prepared = api::PrepareInsertRowForBatch(api::EngineInsertRowsRequest{}, row, row_template, plan);
    Check(prepared.values.size() == 2 && prepared.values[0].first == "id" &&
          prepared.values[0].second == std::string(16, char(pattern)) &&
          prepared.values[1].first == "payload" && prepared.values[1].second == std::string("\xff\0\1",3),
          "staged INSERT lost native values while reordering columns");
    const auto pairs = api::RowValuePairs(row);
    Check(pairs[0].second == std::string("\xff\0\1",3) && pairs[1].second == std::string(16,char(pattern)),
          "CRUD row projection erased native bytes");
  }
  namespace p = scratchbird::parser::sbsql;
  const auto nil = p::DecodeInsertUuidDataLiteral("00000000-0000-0000-0000-000000000000");
  const auto maximum = p::DecodeInsertUuidDataLiteral("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF");
  Check(nil && *nil == std::string(16, '\0') && maximum && *maximum == std::string(16, char(0xff)),
        "UUID data decoder applied system identity restrictions");
  for (unsigned bit = 0; bit < 128; ++bit) {
    std::string spelling = "00000000-0000-0000-0000-000000000000";
    unsigned nibble = bit / 4;
    unsigned offset = nibble + (nibble >= 8) + (nibble >= 12) + (nibble >= 16) + (nibble >= 20);
    spelling[offset] = "0123456789abcdef"[1u << (3 - bit % 4)];
    std::string expected_bits(16, '\0');
    expected_bits[bit / 8] = char(1u << (7 - bit % 8));
    Check(p::DecodeInsertUuidDataLiteral(spelling) == expected_bits,
          "UUID SQL literal changed a data bit");
  }
  Check(!p::DecodeInsertUuidDataLiteral("00000000-0000-0000-0000-00000000000z") &&
        !p::DecodeInsertUuidDataLiteral("00000000000000000000000000000000") &&
        p::DecodeInsertBinaryLiteral("00Ff10") == std::string("\x00\xff\x10", 3) &&
        p::DecodeInsertBinaryLiteral("") == std::string{} &&
        !p::DecodeInsertBinaryLiteral("0") && !p::DecodeInsertBinaryLiteral("xz"),
        "native SQL literal syntax validation or empty binary semantics failed");
  namespace dt = scratchbird::core::datatypes;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Check(manifest.ok(), "Core datatype manifest unavailable");
  const auto binary = dt::LookupDatatypeCatalogRow(manifest.manifest, dt::CanonicalTypeId::binary);
  Check(binary.ok() && binary.manifest.descriptor_rows.size() == 1, "Core binary descriptor unavailable");
  const auto binary_id = binary.manifest.descriptor_rows.front().descriptor_uuid.value;
  const auto codec = dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV3, 3, 3, binary_id, 1);
  Check(codec.ok && codec.row.type_uuid != binary_id && codec.row.codec_uuid != binary_id &&
        codec.row.codec_id == "datatype.binary.octets.v1" && codec.row.canonical_value_variable_width &&
        codec.row.canonical_value_minimum_bytes == 0 && codec.row.canonical_value_maximum_bytes == 16777216 &&
        codec.row.sql_null_requires_zero_payload && codec.row.empty_value_distinct_from_sql_null,
        "binary successor row lacks exact native codec semantics");
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV2, 2, 2, binary_id, 1).ok &&
        !dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV1, 1, 1, binary_id, 1).ok &&
        !dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV3, 2, 3, binary_id, 1).ok &&
        !dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV3, 3, 3, binary_id, 2).ok,
        "binary codec leaked into a predecessor or mismatched generation");
  auto envelope = scratchbird::test::sbsql::BuildCanonicalEngineSblrEnvelopeForTest(
      "ddl.create_table", "SBLR_DDL_CREATE_TABLE", "native.column.binding");
  std::vector<s::SblrColumnIdentityBinding> expected;
  for (unsigned index = 0; index < 3; ++index) {
    s::SblrColumnIdentityBinding binding;
    binding.column_uuid = FixtureUuid(2092, 1 + index * 2);
    binding.value_descriptor_uuid = FixtureUuid(2092, 2 + index * 2);
    binding.datatype_descriptor_uuid = FixtureUuid(2092, 100);
    binding.datatype_descriptor_generation = 0x0102030405060708ULL;
    binding.type_uuid = FixtureUuid(2092, 101);
    expected.push_back(binding);
    s::AppendSblrColumnIdentityBinding(envelope, index, binding);
  }
  std::vector<s::SblrColumnIdentityBinding> decoded;
  Check(s::DecodeSblrColumnIdentities(envelope, &decoded) && decoded == expected,
        "distinct native column/type identities were not preserved");
  const auto encoded = s::EncodeSblrEnvelope(envelope);
  Check(!encoded.empty(), "native column references did not encode as canonical SBOP");
  const auto roundtrip = s::DecodeSblrEnvelope(encoded);
  Check(roundtrip.ok && s::DecodeSblrColumnIdentities(roundtrip.envelope, &decoded) &&
        decoded == expected, "canonical binary transport changed a column binding");
  for (std::size_t index = 0; index < envelope.operands.size(); ++index) {
    const auto& operand = envelope.operands[index];
    Check(operand.value.empty(), "identity had a textual shadow");
    if (index % 4 == 2) {
      Check(operand.value_body.size() == 24 && operand.value_body[16] == 8 &&
            operand.value_body[23] == 1, "generation was not exact little-endian binary");
    } else Check(operand.value_body.size() == 16, "system UUID was not binary16");
    for (unsigned mutation = 0; mutation < 10; ++mutation) {
      auto bad = envelope;
      auto& value = bad.operands[index];
      switch (mutation) {
        case 0: value.value = "019d0000-0000-7000-8000-000000000001"; break;
        case 1: value.value_body.pop_back(); break;
        case 2: value.value_body.push_back(0); break;
        case 3: value.value_body[6] = 0x40; break;
        case 4: value.value_body[8] = 0; break;
        case 5: value.value_flags = 1; break;
        case 6: value.ordinal = 0; break;
        case 7: value.type = "text"; break;
        case 8: value.value_kind = s::SblrValueKind::literal_typed; break;
        case 9: value.name.insert(7, "0"); break;
      }
      decoded = expected;
      Check(!s::DecodeSblrColumnIdentities(bad, &decoded) && decoded == expected,
            "malformed native column binding admitted or modified output");
    }
    auto bad = envelope;
    bad.operands.erase(bad.operands.begin() + index);
    for (std::size_t n = 0; n < bad.operands.size(); ++n) bad.operands[n].ordinal = n + 1;
    Check(!s::DecodeSblrColumnIdentities(bad, &decoded), "missing identity role admitted");
  }
  for (unsigned mutation = 0; mutation < 10; ++mutation) {
    auto bad = envelope;
    switch (mutation) {
      case 0: bad.operation_id = "ddl.drop_table"; break;
      case 1: bad.opcode = "SBLR_DDL_DROP_TABLE"; break;
      case 2: bad.operands[1].value_body = bad.operands[0].value_body; break;
      case 3: bad.operands[4].value_body = bad.operands[0].value_body; break;
      case 4: bad.operands[5].value_body = bad.operands[1].value_body; break;
      case 5: bad.operands[0].name = "column_999999999999999999999999999_uuid"; break;
      case 6: std::fill(bad.operands[2].value_body.begin() + 16,
                        bad.operands[2].value_body.end(), 0); break;
      case 7: bad.operands[0].name = bad.operands[1].name; break;
      case 8: std::copy_n(bad.operands[0].value_body.begin(), 16, bad.operands[6].value_body.begin()); break;
      case 9: bad.operands[7].value_body = bad.operands[1].value_body; break;
    }
    decoded = expected;
    Check(!s::DecodeSblrColumnIdentities(bad, &decoded) && decoded == expected,
          "conflicting identity or operation admitted");
  }
  std::cout << "native_column_binding=passed\n";
}
