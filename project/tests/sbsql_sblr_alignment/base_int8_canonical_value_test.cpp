// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_layout.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"
#include "sblr_special_forms.hpp"
#include "query/expression_api.hpp"
#include "../support/exact_datatype_descriptor_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace dt = scratchbird::core::datatypes;
namespace disk = scratchbird::storage::disk;
namespace engine = scratchbird::engine;
namespace sblr = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace fs = std::filesystem;

namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const std::string& why) {
  ++checks;
  if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}

engine::ExecutionTypeDescriptor Descriptor() {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto row = manifest.ok()
      ? dt::LookupDatatypeCatalogRow(manifest.manifest, dt::CanonicalTypeId::int8)
      : dt::DatatypeCatalogManifestResult{};
  Check(row.ok() && row.manifest.descriptor_rows.size() == 1,
        "unique int8 catalog descriptor row");
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows[0].descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows[0].descriptor_epoch;
  const auto built = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::int8, metadata);
  Check(built.ok(), "execution descriptor from exact int8 catalog row");
  return built.descriptor;
}

dt::DatatypeOperationValue Int8(std::uint8_t raw) {
  return {dt::CanonicalTypeId::int8,
          std::string(1, static_cast<char>(raw)), false};
}

bool IsInt8(const dt::DatatypeOperationValue& value, std::uint8_t raw) {
  return value.type_id == dt::CanonicalTypeId::int8 && !value.is_null &&
      value.encoded_value.size() == 1 &&
      static_cast<unsigned char>(value.encoded_value[0]) == raw;
}

dt::DatatypeCastResult Cast(dt::DatatypeOperationValue value,
                            dt::CanonicalTypeId target,
                            dt::DatatypeCastContext context) {
  dt::DatatypeCastRequest request;
  request.value = std::move(value);
  request.target_type_id = target;
  request.context = context;
  request.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
  if (target == dt::CanonicalTypeId::int8) request.target_descriptor = Descriptor();
  return dt::CastDatatypeValue(request);
}

void IdentityAndPhysicalCodecs() {
  constexpr platform::Uuid descriptor_uuid{{
      0x64,0,0,0,0x69,0x6e,0x74,0x38,0x80,0,0,0,0,0,0,0}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x00}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x01}};
  const auto descriptor = Descriptor();
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes),
                   descriptor_uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.stable_name == "int8" && descriptor.bit_width == 8,
        "exact int8 descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::int8);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid == codec_uuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.int8.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 1 &&
            found->canonical_value_maximum_bytes == 1 &&
            found->canonical_value_exact_bytes == 1 && found->null_supported &&
            found->canonical_byte_order == "little_endian" &&
            found->canonical_representation == "twos_complement_integer",
        "exact int8 type-codec tuple");

  constexpr std::array<platform::byte,25> minimum_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x64,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0x42,0x10,0x3e,0xcc,0x80}};
  constexpr std::array<platform::byte,25> zero_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x64,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0xc2,0x46,0x3d,0x4c,0x00}};
  constexpr std::array<platform::byte,25> maximum_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x64,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0x2b,0x9b,0x3c,0xdf,0x7f}};
  constexpr std::array<platform::byte,24> null_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x64,0,0,0,0,0,0,0,
      0,0,0,0,0x09,0x61,0x1d,0xb9}};
  for (unsigned candidate = 0; candidate != 256; ++candidate) {
    const auto raw = static_cast<platform::byte>(candidate);
    std::string decimal_text, operation_bytes;
    const std::string raw_string(1, static_cast<char>(raw));
    const bool operation_round_trip = dt::DecodeCanonicalInt8Value(
        raw_string, &decimal_text) &&
        dt::EncodeCanonicalInt8Value(decimal_text, &operation_bytes) &&
        operation_bytes == raw_string;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::int8, false, false, {raw}});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int8, dt::DatatypePhysicalValueState::value,
         {raw}});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(operation_round_trip && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == std::vector<platform::byte>{raw} &&
              physical.ok() && physical_back.ok() &&
              physical_back.value.payload == std::vector<platform::byte>{raw},
          "all 256 canonical int8 patterns round trip through value, binary, and physical codecs");
  }
  for (const auto& bad : {std::vector<platform::byte>{},
                          std::vector<platform::byte>{0, 1}}) {
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::int8, false, false, bad}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::int8,
                dt::DatatypePhysicalValueState::value, bad}).ok(),
          "int8 codecs reject non-one-byte values");
  }
  for (const auto& [raw, oracle] :
       {std::pair{platform::byte{0x80}, &minimum_oracle},
        std::pair{platform::byte{0x00}, &zero_oracle},
        std::pair{platform::byte{0x7f}, &maximum_oracle}}) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int8, dt::DatatypePhysicalValueState::value,
         {raw}});
    Check(encoded.ok() &&
              std::equal(encoded.bytes.begin(), encoded.bytes.end(),
                         oracle->begin(), oracle->end()),
          "int8 physical boundary equals independent oracle");
  }
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::int8,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(physical_null.ok() &&
            std::equal(physical_null.bytes.begin(), physical_null.bytes.end(),
                       null_oracle.begin(), null_oracle.end()),
        "int8 SQL NULL has zero-payload independent oracle");
  const auto layout = dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::int8);
  Check(layout.ok() && layout.layout.inline_bytes == 1 &&
            layout.layout.alignment_bytes == 1 &&
            layout.layout.encoding ==
                dt::DatatypeBinaryEncoding::twos_complement_little_endian,
        "int8 storage layout is exact one-byte two's-complement");
}

void OperationsAndSerialization() {
  for (const auto& [text, raw] :
       {std::pair{"-128", 0x80u}, {"-1", 0xffu}, {"0", 0x00u},
        {"1", 0x01u}, {"127", 0x7fu}}) {
    const auto cast = Cast({dt::CanonicalTypeId::character, text, false},
                           dt::CanonicalTypeId::int8,
                           dt::DatatypeCastContext::explicit_cast);
    Check(cast.ok() && IsInt8(cast.value, raw),
          "text boundary emits canonical int8 byte");
  }
  for (const char* text : {"-129", "128", "", "1x"}) {
    Check(!Cast({dt::CanonicalTypeId::character, text, false},
                dt::CanonicalTypeId::int8,
                dt::DatatypeCastContext::explicit_cast).ok(),
          "out-of-range or malformed text is rejected");
  }
  for (auto context : {dt::DatatypeCastContext::implicit,
                       dt::DatatypeCastContext::assignment,
                       dt::DatatypeCastContext::explicit_cast}) {
    const auto identity = Cast(Int8(0x80), dt::CanonicalTypeId::int8, context);
    Check(identity.ok() && IsInt8(identity.value, 0x80),
          "int8 identity preserves the canonical byte");
  }
  const auto widened = Cast(Int8(0x80), dt::CanonicalTypeId::int16,
                            dt::DatatypeCastContext::implicit);
  const auto rendered = Cast(Int8(0x80), dt::CanonicalTypeId::character,
                             dt::DatatypeCastContext::explicit_cast);
  Check(widened.ok() && widened.value.encoded_value == "-128",
        "int8 widening boundary emits signed decimal text");
  Check(rendered.ok() && rendered.value.encoded_value == "-128",
        "int8 character boundary emits signed decimal text");
  const auto exact_numeric = Cast(Int8(0x80), dt::CanonicalTypeId::decimal,
                                  dt::DatatypeCastContext::explicit_cast);
  const auto approximate_numeric = Cast(
      Int8(0x80), dt::CanonicalTypeId::real64,
      dt::DatatypeCastContext::explicit_cast);
  Check(exact_numeric.ok() && exact_numeric.value.encoded_value == "-128" &&
            approximate_numeric.ok() &&
            approximate_numeric.value.encoded_value == "-128",
        "int8 numeric output casts decode the canonical byte");
  const auto narrowed = Cast({dt::CanonicalTypeId::int16, "127", false},
                             dt::CanonicalTypeId::int8,
                             dt::DatatypeCastContext::explicit_cast);
  Check(narrowed.ok() && IsInt8(narrowed.value, 0x7f),
        "narrowing cast emits one canonical byte");
  const auto assigned = Cast({dt::CanonicalTypeId::int16, "-128", false},
                             dt::CanonicalTypeId::int8,
                             dt::DatatypeCastContext::assignment);
  Check(assigned.ok() && IsInt8(assigned.value, 0x80),
        "checked assignment cast emits one canonical byte");
  Check(!Cast({dt::CanonicalTypeId::int16, "128", false},
              dt::CanonicalTypeId::int8,
              dt::DatatypeCastContext::explicit_cast).ok(),
        "narrowing overflow fails closed");
  Check(!Cast({dt::CanonicalTypeId::int16, "128", false},
              dt::CanonicalTypeId::int8,
              dt::DatatypeCastContext::assignment).ok(),
        "assignment overflow fails closed");
  for (const auto& source :
       {dt::DatatypeOperationValue{dt::CanonicalTypeId::decimal, "12.0", false},
        dt::DatatypeOperationValue{dt::CanonicalTypeId::decimal_float, "-12e0", false},
        dt::DatatypeOperationValue{dt::CanonicalTypeId::real64, "127.0", false}}) {
    const auto converted = Cast(source, dt::CanonicalTypeId::int8,
                                dt::DatatypeCastContext::explicit_cast);
    Check(converted.ok() && converted.value.encoded_value.size() == 1,
          "integral numeric cast emits one canonical int8 byte");
  }
  const auto assigned_numeric = Cast(
      {dt::CanonicalTypeId::decimal, "12", false},
      dt::CanonicalTypeId::int8, dt::DatatypeCastContext::assignment);
  Check(assigned_numeric.ok() && IsInt8(assigned_numeric.value, 0x0c),
        "checked exact-numeric assignment emits one canonical int8 byte");
  for (const auto& source :
       {dt::DatatypeOperationValue{dt::CanonicalTypeId::decimal, "12.5", false},
        dt::DatatypeOperationValue{dt::CanonicalTypeId::decimal_float, "NaN", false},
        dt::DatatypeOperationValue{dt::CanonicalTypeId::real64, "128", false}}) {
    Check(!Cast(source, dt::CanonicalTypeId::int8,
                dt::DatatypeCastContext::explicit_cast).ok(),
          "non-integral, special, or out-of-range numeric cast fails closed");
  }
  Check(!Cast({dt::CanonicalTypeId::int8, {}, false},
              dt::CanonicalTypeId::int8,
              dt::DatatypeCastContext::explicit_cast).ok() &&
            !Cast({dt::CanonicalTypeId::int8, std::string(2, '\0'), false},
                  dt::CanonicalTypeId::int8,
                  dt::DatatypeCastContext::explicit_cast).ok(),
        "operation layer rejects non-one-byte int8 carriers");

  const auto minimum = Int8(0x80), zero = Int8(0x00), maximum = Int8(0x7f);
  Check(dt::CompareDatatypeValues({minimum, zero}).comparison < 0 &&
            dt::CompareDatatypeValues({zero, maximum}).comparison < 0,
        "int8 comparison uses signed order");
  const auto min_key = dt::MakeDatatypeSortKey({minimum});
  const auto zero_key = dt::MakeDatatypeSortKey({zero});
  const auto max_key = dt::MakeDatatypeSortKey({maximum});
  Check(min_key.ok() && zero_key.ok() && max_key.ok() &&
            min_key.sort_key == std::string({char(1), char(0x00)}) &&
            zero_key.sort_key == std::string({char(1), char(0x80)}) &&
            max_key.sort_key == std::string({char(1), char(0xff)}) &&
            min_key.sort_key < zero_key.sort_key &&
            zero_key.sort_key < max_key.sort_key,
        "int8 signed sort key is sign-bit transformed big-endian byte");
  const auto min_hash = dt::HashDatatypeValue({minimum});
  const auto zero_hash = dt::HashDatatypeValue({zero});
  const auto max_hash = dt::HashDatatypeValue({maximum});
  Check(min_hash.ok() && zero_hash.ok() && max_hash.ok() &&
            min_hash.stable_hash_hex != zero_hash.stable_hash_hex &&
            zero_hash.stable_hash_hex != max_hash.stable_hash_hex,
        "int8 hash consumes distinct canonical bytes");
  Check(dt::RenderDatatypeValueForDisplay({minimum}).display_value == "-128" &&
            dt::RenderDatatypeValueForDisplay({zero}).display_value == "0" &&
            dt::RenderDatatypeValueForDisplay({maximum}).display_value == "127",
        "display boundary renders signed decimal text");

  const auto descriptor = Descriptor();
  for (const auto& [raw, hex] :
       {std::pair{0x80u, "80"}, {0x00u, "00"}, {0x7fu, "7f"}}) {
    auto value = Int8(raw); value.descriptor = descriptor;
    const auto encoded = dt::SerializeDatatypeValue({value});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int8;
    request.expected_descriptor = descriptor;
    request.serialized_value = encoded.serialized_value;
    const auto decoded = dt::DeserializeDatatypeValue(request);
    Check(encoded.ok() && encoded.serialized_value ==
              std::string("SBDV1;type=int8;state=value;payload=") + hex &&
              decoded.ok() && IsInt8(decoded.value, raw),
          "serialized int8 preserves the exact canonical byte");
  }
  for (const char* bad : {"SBDV1;type=int8;state=value;payload=",
                          "SBDV1;type=int8;state=value;payload=0001",
                          "SBDV1;type=int8;state=null;payload=00"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int8;
    request.expected_descriptor = descriptor;
    request.serialized_value = bad;
    Check(!dt::DeserializeDatatypeValue(request).ok(),
          "serialized int8 rejects invalid size or NULL payload");
  }
}

void SblrBoundaryAdapters() {
  const auto signed_value = [](std::int64_t number) {
    sblr::SblrValue value;
    value.descriptor_id = "int8";
    value.payload_kind = sblr::SblrValuePayloadKind::signed_integer;
    value.int64_value = number;
    value.has_int64_value = true;
    value.encoded_value = std::to_string(number);
    value.text_value = value.encoded_value;
    value.is_null = false;
    return value;
  };
  for (const auto number : {-128, 0, 127}) {
    const auto result = sblr::EvaluateSblrCastForm(
        "int8_identity", signed_value(number), "int8", {}, false, false);
    const auto* value = result.ok() && result.scalar_values.size() == 1
        ? &result.scalar_values.front() : nullptr;
    Check(value && value->descriptor_id == "int8" &&
              value->payload_kind == sblr::SblrValuePayloadKind::signed_integer &&
              value->has_int64_value && value->int64_value == number &&
              value->encoded_value == std::to_string(number) &&
              value->text_value == std::to_string(number) &&
              value->binary_value.empty(),
          "SBLR int8 adapter round trips through the canonical byte carrier");
  }
  Check(!sblr::EvaluateSblrCastForm(
             "int8_invalid", signed_value(128), "int8", {}, false, false).ok(),
        "SBLR int8 adapter rejects an out-of-range source");

  sblr::SblrValue text;
  text.descriptor_id = "character";
  text.payload_kind = sblr::SblrValuePayloadKind::text;
  text.encoded_value = "-128";
  text.text_value = "-128";
  text.is_null = false;
  const auto converted = sblr::EvaluateSblrCastForm(
      "text_to_int8", text, "int8", {}, true, false);
  Check(converted.ok() && converted.scalar_values.size() == 1 &&
            converted.scalar_values.front().int64_value == -128 &&
            converted.scalar_values.front().encoded_value == "-128",
        "SBLR text boundary publishes decoded int8 without raw-byte leakage");
}

api::EngineDescriptor EngineDescriptor(const std::string& uuid,
                                       const std::string& type,
                                       bool nullable) {
  const auto type_id = type == "int8" ? dt::CanonicalTypeId::int8
                                       : dt::CanonicalTypeId::boolean;
  return scratchbird::tests::ExactScalarDescriptorFixture(
      type_id, type, scratchbird::tests::ParsedFixtureUuid(uuid),
      "nullability=" + std::string(nullable ? "nullable" : "non_null"));
}

api::EngineTypedValue EngineValue(const api::EngineDescriptor& descriptor,
                                  std::string text) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(text);
  value.state = api::EngineValueState::value;
  return value;
}

void EngineBoundaryAdapters() {
  const auto int8_descriptor = EngineDescriptor(
      "019dffbc-1000-7000-8000-000000000201", "int8", false);
  const auto boolean_descriptor = EngineDescriptor(
      "019dffbc-1000-7000-8000-000000000202", "boolean", true);
  api::EngineCanonicalExpressionEvaluationRequest add;
  add.consumer = api::EngineCanonicalExpressionConsumer::projection;
  add.operation = api::EngineCanonicalExpressionOperation::numeric_add;
  add.left_value = EngineValue(int8_descriptor, "120");
  add.right_value = EngineValue(int8_descriptor, "7");
  add.result_descriptor = int8_descriptor;
  add.numeric_context.precision = 3;
  api::EngineCanonicalExpressionEvaluationResult result;
  std::string refusal;
  const bool add_ok = api::QowEvaluateCanonicalTypedExpressionV1(
      add, &result, &refusal);
  Check(add_ok, "Engine int8 arithmetic accepted exact in-range result: " + refusal);
  Check(result.value.encoded_value == "127" &&
            result.value.binary_value.empty() &&
            result.value.state == api::EngineValueState::value,
        "Engine int8 adapter decodes the canonical result at publication: value=" +
            result.value.encoded_value);
  add.right_value = EngineValue(int8_descriptor, "8");
  refusal.clear();
  const bool overflow_ok = api::QowEvaluateCanonicalTypedExpressionV1(
      add, &result, &refusal);
  Check(!overflow_ok && refusal.find("overflow") != std::string::npos,
        "Engine int8 adapter fails closed on arithmetic overflow: accepted=" +
            std::to_string(overflow_ok) + " value=" + result.value.encoded_value +
            " refusal=" + refusal);

  api::EngineCanonicalExpressionEvaluationRequest comparison;
  comparison.consumer = api::EngineCanonicalExpressionConsumer::filter;
  comparison.operation = api::EngineCanonicalExpressionOperation::less_than;
  comparison.left_value = EngineValue(int8_descriptor, "-128");
  comparison.right_value = EngineValue(int8_descriptor, "127");
  comparison.result_descriptor = boolean_descriptor;
  refusal.clear();
  const bool compare_ok = api::QowEvaluateCanonicalTypedExpressionV1(
      comparison, &result, &refusal);
  Check(compare_ok && result.truth == api::EngineSqlTruthValue::true_value,
        "Engine int8 adapter preserves signed comparison order: accepted=" +
            std::to_string(compare_ok) + " refusal=" + refusal);
}

void Persistence() {
  const auto descriptor = Descriptor();
  std::array<platform::byte, 115> expected{};
  const std::array<platform::byte,8> magic{{'S','B','I','N','T','8','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(std::begin(descriptor.descriptor_uuid.bytes),
            std::end(descriptor.descriptor_uuid.bytes), expected.begin() + 8);
  platform::StoreLittle64(expected.data() + 24, descriptor.descriptor_epoch);
  platform::StoreLittle32(expected.data() + 32, descriptor.canonical_type_id);
  platform::StoreLittle32(expected.data() + 36, 25);
  std::size_t offset = 40;
  for (platform::byte raw : {platform::byte{0x80}, platform::byte{0x00},
                             platform::byte{0x7f}}) {
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int8, dt::DatatypePhysicalValueState::value,
         {raw}});
    Check(physical.ok() && physical.bytes.size() == 25,
          "production int8 physical boundary encoding");
    if (physical.ok()) {
      std::copy(physical.bytes.begin(), physical.bytes.end(),
                expected.begin() + offset);
    }
    offset += 25;
  }
#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-int8-" + std::to_string(pid) + ".codec");
  struct Cleanup { fs::path p; ~Cleanup(){ std::error_code e; fs::remove(p,e); } } cleanup{path};
  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create int8 persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact int8 bytes");
  disk::FileDevice reader;
  Check(reader.Open(path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen int8 persistence fixture");
  std::array<platform::byte, 115> actual{};
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = true;
  offset = 40;
  for (platform::byte raw : {platform::byte{0x80}, platform::byte{0x00},
                             platform::byte{0x7f}}) {
    const auto decoded = dt::DecodeDatatypePhysicalValue(actual.data() + offset, 25);
    decoded_all = decoded_all && decoded.ok() &&
        decoded.value.payload == std::vector<platform::byte>{raw};
    offset += 25;
  }
  Check(read.ok() && actual == expected && decoded_all,
        "reopen preserves descriptor identity and min/zero/max bytes");
  Check(reader.Close().ok(), "close reopened int8 fixture");
}
}  // namespace

int main() {
  IdentityAndPhysicalCodecs();
  OperationsAndSerialization();
  SblrBoundaryAdapters();
  EngineBoundaryAdapters();
  Persistence();
  std::cout << "base int8 checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
