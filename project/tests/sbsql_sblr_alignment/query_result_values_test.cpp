// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/sblr/canonical_query_result_values.hpp"
#include "sbl_numeric.hpp"
#include "datatype_binary.hpp"
#include "canonical_utf8.hpp"
#include "wire/typed_update_carrier_codec.hpp"

#include <algorithm>
#include <array>
#include <iostream>
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
wire::TypedResultUuid Bytes(const std::string& text) {
  wire::TypedResultUuid result{};
  std::string hex;
  for (const char c : text) if (c != '-') hex += c;
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = static_cast<unsigned char>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
  return result;
}
struct Vector {
  const char* descriptor;
  const char* type;
  const char* codec;
  dt::CanonicalTypeId code;
  unsigned width;
  std::string lexical;
  std::vector<std::uint8_t> bytes;
};
// Independent fixed identities and binary values; no registry/encoder lookup
// participates in these expected values. These are synthetic internal contexts,
// not receipt issuance, public-route or all-datatype acceptance evidence.
const std::array<Vector, 6> vectors{{
  {"01000000-626f-7f6c-a561-6e0000000000", "01000000-626f-7f6c-a561-6e0000000000", "datatype.boolean.u8.v1", dt::CanonicalTypeId::boolean, 1, "true", {1}},
  {"019d0000-0000-7000-8000-00000000d716", "019d0000-0000-7000-8000-00000000d717", "datatype.int32.le.v1", dt::CanonicalTypeId::int32, 4, "-2147483648", {0,0,0,128}},
  {"019d0000-0000-7000-8000-00000000d711", "019d0000-0000-7000-8000-00000000d712", "datatype.int64.le.v1", dt::CanonicalTypeId::int64, 8, "-9223372036854775808", {0,0,0,0,0,0,0,128}},
  {"a0000000-6465-7369-ad61-6c0000000000", "019d0000-0000-7000-8000-00000000d713", "datatype.decimal.base1e9.le.v1", dt::CanonicalTypeId::decimal, 24, "-12.34", {130,4,1,0,210,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
  {"019d0000-0000-7000-8000-00000000d714", "019d0000-0000-7000-8000-00000000d715", "datatype.int128.le.v1", dt::CanonicalTypeId::int128, 16, "-170141183460469231731687303715884105728", {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,128}},
  {"019d0000-0000-7000-8000-00000000d718", "019d0000-0000-7000-8000-00000000d719", "datatype.text.utf8.v1", dt::CanonicalTypeId::character, 0, std::string("9;=\0é",7), {57,59,61,0,195,169,0}}
}};
struct Fixture {
  api::EngineRequestContext context;
  api::EngineResultShape shape;
  std::shared_ptr<api::EngineQueryResultMetadataV1> metadata = std::make_shared<api::EngineQueryResultMetadataV1>();
  std::string code, detail;
  Fixture(std::size_t type = 2, std::size_t rows = 1, std::size_t columns = 1,
          bool binary = false, bool null = false) {
    context.statement_receipt_uuid.canonical = "019d0000-0000-7000-8000-000000001001";
    context.statement_snapshot_uuid.canonical = "019d0000-0000-7000-8000-000000001002";
    context.datatype_catalog_snapshot_uuid.canonical = "019d0000-0000-7000-8000-00000000d701";
    context.datatype_catalog_generation = context.datatype_registry_generation = 1;
    context.maximum_typed_result_transport_bytes_per_packet = 65536;
    metadata->statement_receipt_uuid = Bytes(context.statement_receipt_uuid.canonical);
    metadata->statement_snapshot_uuid = Bytes(context.statement_snapshot_uuid.canonical);
    metadata->datatype_catalog_snapshot_uuid = Bytes(context.datatype_catalog_snapshot_uuid.canonical);
    metadata->datatype_catalog_generation = metadata->datatype_registry_generation = 1;
    const auto& v = vectors[type];
    for (std::size_t i = 0; i < columns; ++i) {
      api::EngineQueryResultColumnV1 c;
      c.bound_descriptor_uuid = Bytes(v.descriptor);
      c.transport = {static_cast<unsigned>(i),static_cast<unsigned>(i),"same;=é",wire::TypedResultNullability::nullable,
                     Bytes(v.descriptor),1,Bytes(v.type),1,v.code,v.codec,1,1,v.width};
      if (type == 3) { c.precision = 10; c.scale = 2; }
      metadata->columns.push_back(c);
      api::EngineDescriptor d;
      d.descriptor_uuid.canonical = v.descriptor;
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
  constexpr std::size_t expected = 6 * 2 * 2 * 3 * 3;
  std::cout << "expected_value_tuples=" << expected << '\n';
  std::size_t observed = 0;
  for (std::size_t type = 0; type < vectors.size(); ++type)
  for (bool binary : {false, true})
  for (bool null : {false, true})
  for (std::size_t rows : {0U,1U,3U})
  for (std::size_t columns : {1U,2U,4U}) {
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
    descriptor.descriptor_uuid = Bytes("019d0000-0000-7000-8000-000000002001");
    descriptor.descriptor_generation = 1;
    descriptor.datatype_catalog_snapshot_uuid = f.metadata->datatype_catalog_snapshot_uuid;
    descriptor.datatype_catalog_generation = descriptor.datatype_registry_generation = 1;
    for (const auto& c : f.metadata->columns) descriptor.columns.push_back(c.transport);
    const auto encoded_descriptor = wire::EncodeTypedResultRowDescriptor(descriptor);
    Check(encoded_descriptor.ok(), "all six registry codec widths encode including DECIMAL");
    const auto decoded_descriptor = wire::DecodeTypedResultRowDescriptor(encoded_descriptor.encoded);
    Check(decoded_descriptor.ok(), "all six descriptor byte decoders agree");
    if (rows != 0) {
      wire::TypedResultBatch batch;
      batch.execution_uuid = Bytes("019d0000-0000-7000-8000-000000002002");
      batch.result_set_uuid = Bytes("019d0000-0000-7000-8000-000000002003");
      batch.batch_uuid = Bytes("019d0000-0000-7000-8000-000000002004");
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
      carrier.execution_uuid = Bytes("019d0000-0000-7000-8000-000000002002");
      carrier.result_set_uuid = Bytes("019d0000-0000-7000-8000-000000002003");
      carrier.snapshot_uuid = Bytes("019d0000-0000-7000-8000-000000001002");
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
  Reject(2, [](auto& f) { f.context.statement_receipt_uuid.canonical.back() = '3'; });
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
  std::cout << "PASS tuples=" << observed << " checks=" << checks << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "FAIL check=" << checks << " " << e.what() << '\n';
  return 1;
}
