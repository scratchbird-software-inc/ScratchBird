// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_layout.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"
#include "descriptor_value_runtime.hpp"
#include "sbl_numeric.hpp"
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
namespace executor = scratchbird::engine::executor;
namespace sblr = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace fs = std::filesystem;
namespace numeric = scratchbird::libraries::sbl_numeric;

namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const std::string& why) {
  ++checks;
  if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}

std::string DiagnosticDetail(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      const auto* text = argument.text();
      return text == nullptr ? std::string{} : *text;
    }
  }
  return {};
}

bool SameUuid(const engine::Uuid& left, const engine::Uuid& right) {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    std::begin(right.bytes), std::end(right.bytes));
}

engine::ExecutionTypeDescriptor CatalogDescriptor(dt::CanonicalTypeId type_id) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto row = manifest.ok()
      ? dt::LookupDatatypeCatalogRow(manifest.manifest, type_id)
      : dt::DatatypeCatalogManifestResult{};
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows[0].descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows[0].descriptor_epoch;
  return dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata).descriptor;
}

engine::ExecutionTypeDescriptor Descriptor() {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto row = manifest.ok()
      ? dt::LookupDatatypeCatalogRow(manifest.manifest, dt::CanonicalTypeId::uint8)
      : dt::DatatypeCatalogManifestResult{};
  Check(row.ok() && row.manifest.descriptor_rows.size() == 1,
        "unique uint8 catalog descriptor row");
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows[0].descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows[0].descriptor_epoch;
  const auto built = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::uint8, metadata);
  Check(built.ok(), "execution descriptor from exact uint8 catalog row");
  return built.descriptor;
}

dt::DatatypeOperationValue DecimalFloat(std::string_view lexical) {
  const auto encoded = numeric::EncodeDecimal128LittleEndian(lexical);
  Check(encoded.bytes.has_value(), "exact decimal_float BID fixture encoding");
  dt::DatatypeOperationValue value;
  value.type_id = dt::CanonicalTypeId::decimal_float;
  if (encoded.bytes) {
    value.encoded_value.assign(
        reinterpret_cast<const char*>(encoded.bytes->data()),
        encoded.bytes->size());
  }
  value.descriptor = CatalogDescriptor(dt::CanonicalTypeId::decimal_float);
  return value;
}

dt::DatatypeOperationValue Uint8(std::uint8_t raw) {
  return {dt::CanonicalTypeId::uint8,
          std::string(1, static_cast<char>(raw)), false};
}

bool IsUint8(const dt::DatatypeOperationValue& value, std::uint8_t raw) {
  return value.type_id == dt::CanonicalTypeId::uint8 && !value.is_null &&
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
  if (target == dt::CanonicalTypeId::uint8) request.target_descriptor = Descriptor();
  return dt::CastDatatypeValue(request);
}

void IdentityAndPhysicalCodecs() {
  constexpr platform::Uuid descriptor_uuid{{
      0x78,0,0,0,0x75,0x69,0x7e,0x74,0xb8,0,0,0,0,0,0,0}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x04}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x05}};
  const auto descriptor = Descriptor();
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes),
                   descriptor_uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.stable_name == "uint8" && descriptor.bit_width == 8,
        "exact uint8 descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::uint8);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid == codec_uuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.uint8.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 1 &&
            found->canonical_value_maximum_bytes == 1 &&
            found->canonical_value_exact_bytes == 1 && found->null_supported &&
            found->canonical_byte_order == "little_endian" &&
            found->canonical_representation == "unsigned_integer",
        "exact uint8 type-codec tuple");

  constexpr std::array<platform::byte,25> minimum_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x78,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0xf6,0xe2,0x78,0x2d,0x00}};
  constexpr std::array<platform::byte,25> midpoint_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x78,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0x76,0x19,0x78,0xad,0x80}};
  constexpr std::array<platform::byte,25> maximum_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x78,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0x77,0x7a,0x77,0x48,0xff}};
  constexpr std::array<platform::byte,24> null_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x78,0,0,0,0,0,0,0,
      0,0,0,0,0x85,0xc4,0x62,0xe1}};
  constexpr std::array<platform::byte,33> binary_maximum_oracle{{
      0x53,0x42,0x44,0x56,0x41,0x4c,0x30,0x31,0x78,0,0,0,0,0,0x20,0,
      0x01,0,0,0,0,0,0,0,0xb4,0xeb,0xcc,0x73,0xd4,0x24,0xbd,0x44,0xff}};
  constexpr std::array<platform::byte,32> binary_null_oracle{{
      0x53,0x42,0x44,0x56,0x41,0x4c,0x30,0x31,0x78,0,0,0,0x01,0,0x20,0,
      0,0,0,0,0,0,0,0,0x83,0x03,0x9d,0x73,0xb0,0x0f,0x65,0x14}};
  for (unsigned candidate = 0; candidate != 256; ++candidate) {
    const auto raw = static_cast<platform::byte>(candidate);
    std::string decimal_text, operation_bytes;
    const std::string raw_string(1, static_cast<char>(raw));
    const bool operation_round_trip = dt::DecodeCanonicalUint8Value(
        raw_string, &decimal_text) &&
        dt::EncodeCanonicalUint8Value(decimal_text, &operation_bytes) &&
        operation_bytes == raw_string;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::uint8, false, false, {raw}});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint8, dt::DatatypePhysicalValueState::value,
         {raw}});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const auto sort_key = dt::MakeDatatypeSortKey({Uint8(raw)});
    const auto hash = dt::HashDatatypeValue({Uint8(raw)});
    const auto hash_repeat = dt::HashDatatypeValue({Uint8(raw)});
    const bool ordered_after_predecessor = candidate == 0 ||
        dt::CompareDatatypeValues(
            {Uint8(static_cast<std::uint8_t>(candidate - 1)), Uint8(raw)})
                .comparison < 0;
    Check(operation_round_trip && binary.ok() && binary_back.ok() &&
              binary_back.value.payload == std::vector<platform::byte>{raw} &&
              physical.ok() && physical_back.ok() &&
              physical_back.value.payload == std::vector<platform::byte>{raw} &&
              sort_key.ok() &&
              sort_key.sort_key == std::string({char(1), static_cast<char>(raw)}) &&
              hash.ok() && hash_repeat.ok() &&
              hash.stable_hash_hex == hash_repeat.stable_hash_hex &&
              ordered_after_predecessor,
          "all 256 uint8 values round trip with exact order and stable hash");
  }
  for (const auto& bad : {std::vector<platform::byte>{},
                          std::vector<platform::byte>{0, 1}}) {
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::uint8, false, false, bad}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::uint8,
                dt::DatatypePhysicalValueState::value, bad}).ok(),
          "uint8 codecs reject non-one-byte values");
  }
  for (const auto& [raw, oracle] :
       {std::pair{platform::byte{0x00}, &minimum_oracle},
        std::pair{platform::byte{0x80}, &midpoint_oracle},
        std::pair{platform::byte{0xff}, &maximum_oracle}}) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint8, dt::DatatypePhysicalValueState::value,
         {raw}});
    Check(encoded.ok() &&
              std::equal(encoded.bytes.begin(), encoded.bytes.end(),
                         oracle->begin(), oracle->end()),
          "uint8 physical boundary equals independent oracle");
  }
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uint8,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(physical_null.ok() &&
            std::equal(physical_null.bytes.begin(), physical_null.bytes.end(),
                       null_oracle.begin(), null_oracle.end()),
        "uint8 SQL NULL has zero-payload independent oracle");
  const auto binary_maximum = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uint8, false, false, {0xff}});
  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uint8, true, false, {}});
  Check(binary_maximum.ok() && binary_null.ok() &&
            std::equal(binary_maximum.encoded.begin(), binary_maximum.encoded.end(),
                       binary_maximum_oracle.begin(), binary_maximum_oracle.end()) &&
            std::equal(binary_null.encoded.begin(), binary_null.encoded.end(),
                       binary_null_oracle.begin(), binary_null_oracle.end()),
        "current production uint8 binary frames match independent exact oracles");
  auto corrupted_binary = binary_maximum.encoded;
  corrupted_binary.back() ^= 1;
  auto corrupted_physical = std::vector<platform::byte>(
      maximum_oracle.begin(), maximum_oracle.end());
  corrupted_physical.back() ^= 1;
  Check(!dt::DecodeDatatypeBinaryValue(corrupted_binary).ok() &&
            !dt::DecodeDatatypeBinaryValue(std::vector<platform::byte>(
                binary_maximum.encoded.begin(), binary_maximum.encoded.end() - 1)).ok() &&
            !dt::DecodeDatatypePhysicalValue(
                corrupted_physical.data(), corrupted_physical.size()).ok() &&
            !dt::DecodeDatatypePhysicalValue(
                maximum_oracle.data(), maximum_oracle.size() - 1).ok(),
        "uint8 binary and physical decoders reject corruption and truncation");
  const auto layout = dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::uint8);
  Check(layout.ok() && layout.layout.inline_bytes == 1 &&
            layout.layout.alignment_bytes == 1 &&
            layout.layout.encoding ==
                dt::DatatypeBinaryEncoding::unsigned_little_endian,
        "uint8 storage layout is exact one-byte unsigned integer");
}

void OperationsAndSerialization() {
  for (const auto& [text, raw] :
       {std::pair{"0", 0x00u}, {"1", 0x01u}, {"127", 0x7fu},
        {"128", 0x80u}, {"255", 0xffu}}) {
    std::string encoded;
    Check(dt::EncodeCanonicalUint8Value(text, &encoded) &&
              IsUint8({dt::CanonicalTypeId::uint8, encoded, false}, raw),
          "direct text bridge emits one canonical uint8 byte");
  }
  for (const char* text : {"-1", "256", "", "1x"}) {
    std::string encoded;
    Check(!dt::EncodeCanonicalUint8Value(text, &encoded),
          "direct text bridge rejects out-of-range or malformed input");
  }
  for (auto context : {dt::DatatypeCastContext::implicit,
                       dt::DatatypeCastContext::assignment,
                       dt::DatatypeCastContext::explicit_cast}) {
    const auto identity = Cast(Uint8(0xff), dt::CanonicalTypeId::uint8, context);
    Check(identity.ok() && IsUint8(identity.value, 0xff),
          "uint8 identity preserves the canonical byte");
  }
  const auto widened = Cast(Uint8(0xff), dt::CanonicalTypeId::int16,
                            dt::DatatypeCastContext::implicit);
  Check(!widened.ok() &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint8,
                                     dt::CanonicalTypeId::int16) ==
                dt::DatatypeCastCategory::forbidden,
        "unregistered uint8-to-int16 cast remains fail-closed");
  Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint8,
                                dt::CanonicalTypeId::int8) ==
            dt::DatatypeCastCategory::lossy_explicit &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::int8,
                                     dt::CanonicalTypeId::uint8) ==
                dt::DatatypeCastCategory::lossy_explicit,
        "same-width signedness changes remain checked and lossy");
  const auto to_int8 = Cast(Uint8(0x7f), dt::CanonicalTypeId::int8,
                            dt::DatatypeCastContext::assignment);
  Check(to_int8.ok() && to_int8.value.encoded_value == std::string(1, '\x7f') &&
            !Cast(Uint8(0x80), dt::CanonicalTypeId::int8,
                  dt::DatatypeCastContext::assignment).ok() &&
            !Cast(Uint8(0x80), dt::CanonicalTypeId::int8,
                  dt::DatatypeCastContext::explicit_cast).ok(),
        "uint8-to-int8 checks the signed boundary in assignment and explicit contexts");
  const dt::DatatypeOperationValue int8_max{
      dt::CanonicalTypeId::int8, std::string(1, '\x7f'), false};
  const dt::DatatypeOperationValue int8_negative{
      dt::CanonicalTypeId::int8, std::string(1, '\xff'), false};
  Check(IsUint8(Cast(int8_max, dt::CanonicalTypeId::uint8,
                     dt::DatatypeCastContext::assignment).value, 0x7f) &&
            !Cast(int8_negative, dt::CanonicalTypeId::uint8,
                  dt::DatatypeCastContext::assignment).ok() &&
            !Cast(int8_negative, dt::CanonicalTypeId::uint8,
                  dt::DatatypeCastContext::explicit_cast).ok(),
        "int8-to-uint8 checks the unsigned boundary in assignment and explicit contexts");
  Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint8,
                                dt::CanonicalTypeId::character) ==
            dt::DatatypeCastCategory::forbidden &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::character,
                                     dt::CanonicalTypeId::uint8) ==
                dt::DatatypeCastCategory::forbidden &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint8,
                                     dt::CanonicalTypeId::decimal) ==
                dt::DatatypeCastCategory::forbidden &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint8,
                                     dt::CanonicalTypeId::boolean) ==
                dt::DatatypeCastCategory::forbidden,
        "unresolved non-integer uint8 cast pairs remain fail-closed");
  const auto narrowed = Cast(
      {dt::CanonicalTypeId::int16, std::string{'\xff', '\0'}, false},
                             dt::CanonicalTypeId::uint8,
                             dt::DatatypeCastContext::explicit_cast);
  Check(!narrowed.ok(),
        "unregistered int16-to-uint8 explicit cast remains fail-closed");
  const auto assigned = Cast(
      {dt::CanonicalTypeId::int16, std::string{'\x80', '\0'}, false},
                             dt::CanonicalTypeId::uint8,
                             dt::DatatypeCastContext::assignment);
  Check(!assigned.ok(),
        "unregistered int16-to-uint8 assignment remains fail-closed");
  for (const auto& source :
       {dt::DatatypeOperationValue{dt::CanonicalTypeId::decimal, "12.0", false},
        dt::DatatypeOperationValue{dt::CanonicalTypeId::decimal,
                                   "12.000000000000000000001", false},
        dt::DatatypeOperationValue{dt::CanonicalTypeId::real64, "255.0", false}}) {
    Check(!Cast(source, dt::CanonicalTypeId::uint8,
                dt::DatatypeCastContext::explicit_cast).ok(),
          "numeric-to-uint8 remains fail-closed while exact pair policy is unresolved");
  }
  const auto decimal_float_input =
      Cast(DecimalFloat("12E0"), dt::CanonicalTypeId::uint8,
           dt::DatatypeCastContext::explicit_cast);
  Check(!decimal_float_input.ok() &&
            decimal_float_input.diagnostic.diagnostic_code ==
                "DATATYPE.CAST_FORBIDDEN" &&
            DiagnosticDetail(decimal_float_input.diagnostic) ==
                "decimal_float_present_cast_policy_unresolved" &&
            decimal_float_input.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            decimal_float_input.value.encoded_value.empty() &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::decimal_float,
                                     dt::CanonicalTypeId::uint8) ==
                dt::DatatypeCastCategory::forbidden,
        "decimal_float-to-uint8 remains fail-closed without pair policy");
  Check(!Cast({dt::CanonicalTypeId::decimal, "12", false},
              dt::CanonicalTypeId::uint8,
              dt::DatatypeCastContext::assignment).ok(),
        "decimal assignment to uint8 remains fail-closed without pair authority");
  Check(!Cast({dt::CanonicalTypeId::uint8, {}, false},
              dt::CanonicalTypeId::uint8,
              dt::DatatypeCastContext::explicit_cast).ok() &&
            !Cast({dt::CanonicalTypeId::uint8, std::string(2, '\0'), false},
                  dt::CanonicalTypeId::uint8,
                  dt::DatatypeCastContext::explicit_cast).ok(),
        "operation layer rejects non-one-byte uint8 carriers");

  auto nullable_descriptor = Descriptor();
  nullable_descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue typed_null{dt::CanonicalTypeId::uint8, {}, true};
  typed_null.descriptor = nullable_descriptor;
  dt::DatatypeCastRequest null_identity;
  null_identity.value = typed_null;
  null_identity.target_type_id = dt::CanonicalTypeId::uint8;
  null_identity.target_descriptor = nullable_descriptor;
  null_identity.context = dt::DatatypeCastContext::implicit;
  const auto null_cast = dt::CastDatatypeValue(null_identity);
  const auto null_first = dt::MakeDatatypeSortKey(
      {typed_null, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {typed_null, dt::DatatypeNullOrdering::nulls_last});
  const auto null_compare_first = dt::CompareDatatypeValues(
      {typed_null, Uint8(0), dt::DatatypeNullOrdering::nulls_first});
  const auto null_compare_last = dt::CompareDatatypeValues(
      {typed_null, Uint8(0), dt::DatatypeNullOrdering::nulls_last});
  Check(null_cast.ok() && null_cast.value.is_null &&
            null_cast.value.encoded_value.empty() &&
            null_first.ok() && null_first.sort_key == std::string(1, '\0') &&
            null_last.ok() && null_last.sort_key == std::string(1, '\2') &&
            null_compare_first.ok() && null_compare_first.comparison < 0 &&
            null_compare_last.ok() && null_compare_last.comparison > 0,
        "typed uint8 NULL preserves zero payload and exact NULL ordering states");
  const auto serialized_null = dt::SerializeDatatypeValue({typed_null});
  dt::DatatypeDeserializationRequest null_decode_request;
  null_decode_request.expected_type_id = dt::CanonicalTypeId::uint8;
  null_decode_request.expected_descriptor = nullable_descriptor;
  null_decode_request.serialized_value = serialized_null.serialized_value;
  const auto decoded_null = dt::DeserializeDatatypeValue(null_decode_request);
  const auto null_hash = dt::HashDatatypeValue({typed_null});
  const auto null_hash_repeat = dt::HashDatatypeValue({typed_null});
  Check(serialized_null.ok() && serialized_null.serialized_value ==
            "SBDV1;type=uint8;state=null;payload=" &&
            decoded_null.ok() && decoded_null.value.is_null &&
            decoded_null.value.encoded_value.empty() &&
            SameUuid(decoded_null.value.descriptor.descriptor_uuid,
                     nullable_descriptor.descriptor_uuid) &&
            null_hash.ok() && null_hash_repeat.ok() &&
            null_hash.stable_hash_hex == null_hash_repeat.stable_hash_hex &&
            dt::RenderDatatypeValueForDisplay({typed_null}).display_value == "NULL",
        "typed uint8 NULL round trips with zero payload and stable boundaries");
  auto nonnullable_descriptor = nullable_descriptor;
  nonnullable_descriptor.nullable_allowed = false;
  null_identity.target_descriptor = nonnullable_descriptor;
  Check(!dt::CastDatatypeValue(null_identity).ok(),
        "typed uint8 NULL is rejected by a non-nullable target descriptor");
  auto payload_null = typed_null;
  payload_null.encoded_value.assign(1, '\0');
  null_identity.value = payload_null;
  null_identity.target_descriptor = nullable_descriptor;
  Check(!dt::CastDatatypeValue(null_identity).ok(),
        "typed uint8 NULL rejects a substitute payload");
  auto descriptor_mismatch = Uint8(1);
  descriptor_mismatch.descriptor = CatalogDescriptor(dt::CanonicalTypeId::int8);
  Check(!Cast(descriptor_mismatch, dt::CanonicalTypeId::uint8,
              dt::DatatypeCastContext::explicit_cast).ok(),
        "uint8 identity rejects a mismatched source descriptor");

  const auto minimum = Uint8(0x00), midpoint = Uint8(0x80),
             maximum = Uint8(0xff);
  Check(dt::CompareDatatypeValues({minimum, midpoint}).comparison < 0 &&
            dt::CompareDatatypeValues({midpoint, maximum}).comparison < 0,
        "uint8 comparison uses unsigned order");
  const auto min_key = dt::MakeDatatypeSortKey({minimum});
  const auto mid_key = dt::MakeDatatypeSortKey({midpoint});
  const auto max_key = dt::MakeDatatypeSortKey({maximum});
  Check(min_key.ok() && mid_key.ok() && max_key.ok() &&
            min_key.sort_key == std::string({char(1), char(0x00)}) &&
            mid_key.sort_key == std::string({char(1), char(0x80)}) &&
            max_key.sort_key == std::string({char(1), char(0xff)}) &&
            min_key.sort_key < mid_key.sort_key &&
            mid_key.sort_key < max_key.sort_key,
        "uint8 sort key retains the unsigned canonical byte");
  const auto min_hash = dt::HashDatatypeValue({minimum});
  const auto mid_hash = dt::HashDatatypeValue({midpoint});
  const auto max_hash = dt::HashDatatypeValue({maximum});
  const auto mid_hash_repeat = dt::HashDatatypeValue({midpoint});
  Check(min_hash.ok() && mid_hash.ok() && max_hash.ok() &&
            mid_hash_repeat.ok() &&
            mid_hash.stable_hash_hex == mid_hash_repeat.stable_hash_hex,
        "equal canonical uint8 values have repeatable equal hashes");
  Check(dt::RenderDatatypeValueForDisplay({minimum}).display_value == "0" &&
            dt::RenderDatatypeValueForDisplay({midpoint}).display_value == "128" &&
            dt::RenderDatatypeValueForDisplay({maximum}).display_value == "255",
        "display boundary renders unsigned decimal text");
  const auto native_49 = Uint8(0x31);
  const auto native_49_key = dt::MakeDatatypeSortKey({native_49});
  Check(dt::CompareDatatypeValues({native_49, Uint8(0x01)}).comparison > 0 &&
            native_49_key.ok() &&
            native_49_key.sort_key == std::string({char(1), char(0x31)}) &&
            dt::RenderDatatypeValueForDisplay({native_49}).display_value == "49",
        "canonical byte 0x31 remains native 49 rather than decimal-text 1");

  const auto descriptor = Descriptor();
  for (const auto& [raw, hex] :
       {std::pair{0x00u, "00"}, {0x80u, "80"}, {0xffu, "ff"}}) {
    auto value = Uint8(raw); value.descriptor = descriptor;
    const auto encoded = dt::SerializeDatatypeValue({value});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::uint8;
    request.expected_descriptor = descriptor;
    request.serialized_value = encoded.serialized_value;
    const auto decoded = dt::DeserializeDatatypeValue(request);
    Check(encoded.ok() && encoded.serialized_value ==
              std::string("SBDV1;type=uint8;state=value;payload=") + hex &&
              decoded.ok() && IsUint8(decoded.value, raw),
          "serialized uint8 preserves the exact canonical byte");
  }
  for (const char* bad : {"SBDV1;type=uint8;state=value;payload=",
                          "SBDV1;type=uint8;state=value;payload=0001",
                          "SBDV1;type=uint8;state=null;payload=00"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::uint8;
    request.expected_descriptor = descriptor;
    request.serialized_value = bad;
    Check(!dt::DeserializeDatatypeValue(request).ok(),
          "serialized uint8 rejects invalid size or NULL payload");
  }
}

void OwnedUint8Producers() {
  const auto bound_uint8_descriptor = Descriptor();
  const auto uuid_descriptor =
      CatalogDescriptor(dt::CanonicalTypeId::uuid);
  const auto check_uuid_extract_policy = [](const auto& result,
                                            const std::string& message) {
    Check(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_EXTRACT_REJECTED" &&
              DiagnosticDetail(result.diagnostic) ==
                  "uuid_extract_policy_unresolved" &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              !result.value.is_null && result.value.encoded_value.empty(),
          message);
  };
  for (unsigned version : {0u, 7u, 15u}) {
    std::string uuid(16, '\0');
    uuid[6] = static_cast<char>(version << 4u);
    dt::DatatypeExtractRequest request;
    request.value = {dt::CanonicalTypeId::uuid, uuid, false};
    request.value.descriptor = uuid_descriptor;
    request.field = "version";
    request.result_descriptor = bound_uint8_descriptor;
    const auto extracted = dt::ExtractDatatypeField(request);
    check_uuid_extract_policy(
        extracted,
        "UUID version extraction refuses without an identity/extraction policy");
  }

  dt::DatatypeExtractRequest wrong_nonnull;
  wrong_nonnull.value = {dt::CanonicalTypeId::uuid, std::string(16, '\0'), false};
  wrong_nonnull.value.descriptor = uuid_descriptor;
  wrong_nonnull.field = "version";
  wrong_nonnull.result_descriptor = CatalogDescriptor(dt::CanonicalTypeId::int8);
  check_uuid_extract_policy(
      dt::ExtractDatatypeField(wrong_nonnull),
      "UUID version extraction refuses before inferring a result profile");
  dt::DatatypeExtractRequest malformed;
  malformed.value = {dt::CanonicalTypeId::uuid, std::string(15, '\0'), false};
  malformed.value.descriptor = uuid_descriptor;
  malformed.field = "version";
  malformed.result_descriptor = bound_uint8_descriptor;
  const auto malformed_result = dt::ExtractDatatypeField(malformed);
  Check(!malformed_result.ok() &&
            malformed_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_EXTRACT_REJECTED" &&
            DiagnosticDetail(malformed_result.diagnostic) ==
                "uuid_extract_value_invalid" &&
            malformed_result.value.type_id == dt::CanonicalTypeId::unknown &&
            !malformed_result.value.is_null &&
            malformed_result.value.encoded_value.empty(),
        "UUID extraction validates binary16 before unresolved policy");

  auto uint8_descriptor = Descriptor();
  auto nullable_uuid_descriptor = uuid_descriptor;
  nullable_uuid_descriptor.nullable_allowed = true;
  uint8_descriptor.nullable_allowed = true;
  dt::DatatypeExtractRequest null_request;
  null_request.value = {dt::CanonicalTypeId::uuid, {}, true};
  null_request.value.descriptor = nullable_uuid_descriptor;
  null_request.field = "version";
  null_request.result_descriptor = uint8_descriptor;
  const auto null_result = dt::ExtractDatatypeField(null_request);
  check_uuid_extract_policy(
      null_result,
      "typed-NULL UUID extraction refuses without an extraction policy");

  null_request.result_descriptor = CatalogDescriptor(dt::CanonicalTypeId::int8);
  null_request.result_descriptor.nullable_allowed = true;
  check_uuid_extract_policy(
      dt::ExtractDatatypeField(null_request),
      "typed-NULL UUID extraction does not infer a result descriptor");
  null_request.result_descriptor = uint8_descriptor;
  null_request.result_descriptor.nullable_allowed = false;
  check_uuid_extract_policy(
      dt::ExtractDatatypeField(null_request),
      "typed-NULL UUID extraction remains refused for a nonnullable result");
}

void SblrBoundaryAdapters() {
  const auto unsigned_value = [](std::uint64_t number) {
    sblr::SblrValue value;
    value.descriptor_id = "uint8";
    value.payload_kind = sblr::SblrValuePayloadKind::unsigned_integer;
    value.uint64_value = number;
    value.has_uint64_value = true;
    value.encoded_value = std::to_string(number);
    value.text_value = value.encoded_value;
    value.is_null = false;
    return value;
  };
  for (const auto number : {std::uint64_t{0}, std::uint64_t{128},
                            std::uint64_t{255}}) {
    const auto result = sblr::EvaluateSblrCastForm(
        "uint8_identity", unsigned_value(number), "uint8", {}, false, false);
    const auto* value = result.ok() && result.scalar_values.size() == 1
        ? &result.scalar_values.front() : nullptr;
    Check(value && value->descriptor_id == "uint8" &&
              value->payload_kind == sblr::SblrValuePayloadKind::unsigned_integer &&
              value->has_uint64_value && value->uint64_value == number &&
              value->encoded_value == std::to_string(number) &&
              value->text_value == std::to_string(number) &&
              value->binary_value.empty(),
          "SBLR uint8 adapter round trips through the canonical byte carrier");
  }
  Check(!sblr::EvaluateSblrCastForm(
             "uint8_invalid", unsigned_value(256), "uint8", {}, false, false).ok(),
        "SBLR uint8 adapter rejects an out-of-range source");

  sblr::SblrValue negative;
  negative.descriptor_id = "uint8";
  negative.payload_kind = sblr::SblrValuePayloadKind::unsigned_integer;
  negative.encoded_value = "-1";
  negative.text_value = "-1";
  negative.is_null = false;
  Check(!sblr::EvaluateSblrCastForm(
             "uint8_negative", negative, "uint8", {}, false, false).ok(),
        "SBLR uint8 adapter rejects a negative lexical value");

  auto conflicting = unsigned_value(49);
  conflicting.encoded_value = "1";
  conflicting.text_value = "1";
  Check(!sblr::EvaluateSblrCastForm(
             "uint8_conflict", conflicting, "uint8", {}, false, false).ok(),
        "SBLR uint8 adapter rejects conflicting lexical and native fields");
  auto missing_encoded_conflict = unsigned_value(0);
  missing_encoded_conflict.encoded_value.clear();
  missing_encoded_conflict.text_value = "1";
  Check(!sblr::EvaluateSblrCastForm(
             "uint8_missing_encoded_conflict", missing_encoded_conflict,
             "uint8", {}, false, false).ok(),
        "SBLR uint8 adapter reconciles text and native fields without encoded text");
  auto wrong_kind = unsigned_value(1);
  wrong_kind.payload_kind = sblr::SblrValuePayloadKind::signed_integer;
  Check(!sblr::EvaluateSblrCastForm(
             "uint8_wrong_kind", wrong_kind, "uint8", {}, false, false).ok(),
        "SBLR uint8 adapter rejects a descriptor/payload-kind mismatch");
  sblr::SblrValue bad_binary;
  bad_binary.descriptor_id = "uint8";
  bad_binary.payload_kind = sblr::SblrValuePayloadKind::unsigned_integer;
  bad_binary.binary_value = {0x00, 0x01};
  bad_binary.is_null = false;
  Check(!sblr::EvaluateSblrCastForm(
             "uint8_bad_binary", bad_binary, "uint8", {}, false, false).ok(),
        "SBLR uint8 adapter rejects a non-one-byte binary carrier");

}

api::EngineDescriptor EngineDescriptor(const std::string& uuid,
                                       const std::string& type,
                                       bool nullable) {
  const auto type_id = type == "uint8" ? dt::CanonicalTypeId::uint8
      : type == "int32" ? dt::CanonicalTypeId::int32
      : type == "character" ? dt::CanonicalTypeId::character
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
  const auto uint8_descriptor = EngineDescriptor(
      "019dffbc-1000-7000-8000-000000000201", "uint8", false);
  const auto int32_descriptor = EngineDescriptor(
      "019dffbc-1000-7000-8000-000000000202", "int32", false);
  const auto character_descriptor = EngineDescriptor(
      "019dffbc-1000-7000-8000-000000000203", "character", false);

  api::EngineTypedValue output;
  std::string category, refusal;
  Check(!api::QowApplyCanonicalDescriptorCoercionV1(
             EngineValue(uint8_descriptor, "255"), int32_descriptor, false,
             &output, &category, &refusal) &&
            output.state == api::EngineValueState::error &&
            output.encoded_value.empty() && output.binary_value.empty(),
        "Engine adapter refuses unresolved uint8-to-int32 coercion");

  Check(!api::QowApplyCanonicalDescriptorCoercionV1(
             EngineValue(character_descriptor, "255"), uint8_descriptor, true,
             &output, &category, &refusal),
        "Engine adapter keeps unresolved character-to-uint8 coercion fail-closed");

  auto native_49 = EngineValue(uint8_descriptor, {});
  native_49.binary_value = {0x31};
  Check(!api::QowApplyCanonicalDescriptorCoercionV1(
             native_49, int32_descriptor, false,
             &output, &category, &refusal) &&
            output.state == api::EngineValueState::error &&
            output.encoded_value.empty() && output.binary_value.empty(),
        "Engine adapter refuses native uint8-to-int32 coercion without a rule");
  native_49.binary_value = {0x31, 0x00};
  Check(!api::QowApplyCanonicalDescriptorCoercionV1(
             native_49, int32_descriptor, false,
             &output, &category, &refusal),
        "Engine adapter rejects a non-one-byte uint8 binary carrier");

  const auto nullable_uint8 = EngineDescriptor(
      "019dffbc-1000-7000-8000-000000000204", "uint8", true);
  const auto nullable_int32 = EngineDescriptor(
      "019dffbc-1000-7000-8000-000000000205", "int32", true);
  api::EngineTypedValue null_value;
  null_value.descriptor = nullable_uint8;
  null_value.is_null = true;
  null_value.state = api::EngineValueState::sql_null;
  Check(!api::QowApplyCanonicalDescriptorCoercionV1(
             null_value, nullable_int32, false,
             &output, &category, &refusal) &&
            output.state == api::EngineValueState::error &&
            output.encoded_value.empty() && output.binary_value.empty(),
        "Engine adapter refuses typed NULL uint8-to-int32 without a cast rule");
}

void ExecutorBoundaryAdapters() {
  constexpr std::uint32_t descriptor_id = 1;
  const auto descriptor = EngineDescriptor(
      "019dffbc-1000-7000-8000-000000000206", "uint8", false);
  const auto validate = [&](api::EngineTypedValue value, bool expected_ok,
                            const std::string& label) {
    executor::DescriptorBatch batch;
    batch.columns.push_back(
        {"uint8_value", descriptor, false, descriptor_id});
    executor::DescriptorTuple row;
    row.values.push_back(std::move(value));
    batch.rows.push_back(std::move(row));
    const auto ordinary = executor::ValidateDescriptorBatch(batch);
    const auto canonical = executor::ValidateCanonicalDescriptorBatch(
        batch, {descriptor_id});
    const bool diagnostic_ok = expected_ok ||
        (ordinary.diagnostic_code ==
             "QOW-DIAG-QRY-008-RUNTIME-BREADTH-REFUSAL-V1" &&
         canonical.diagnostic_code ==
             "QOW-DIAG-QRY-008-RUNTIME-BREADTH-REFUSAL-V1" &&
         ordinary.column_index == 0 && canonical.column_index == 0);
    Check(ordinary.ok == expected_ok && canonical.ok == expected_ok &&
              diagnostic_ok,
          label);
  };

  for (const char* text : {"0", "5", "255"}) {
    validate(EngineValue(descriptor, text), true,
             "executor accepts valid uint8 decimal text at its boundary");
  }
  for (const char* text : {"-1", "256", "not-a-number"}) {
    validate(EngineValue(descriptor, text), false,
             "executor rejects invalid uint8 decimal text");
  }
  for (const std::uint8_t raw : {std::uint8_t{0x00}, std::uint8_t{0xff}}) {
    auto binary = EngineValue(descriptor, {});
    binary.binary_value = {raw};
    validate(std::move(binary), true,
             "executor accepts an exclusive canonical uint8 byte");
  }
  for (const auto& [text, raw] :
       {std::pair{"0", std::uint8_t{0x00}},
        std::pair{"255", std::uint8_t{0xff}}}) {
    auto conflict = EngineValue(descriptor, text);
    conflict.binary_value = {raw};
    validate(std::move(conflict), false,
             "executor rejects conflicting uint8 text and binary carriers");
  }
  validate(EngineValue(descriptor, {}), false,
           "executor rejects an empty present uint8 carrier");
  auto oversized = EngineValue(descriptor, {});
  oversized.binary_value = {0x00, 0x01};
  validate(std::move(oversized), false,
           "executor rejects a multi-byte uint8 carrier");
}

void Persistence() {
  const auto descriptor = Descriptor();
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x04}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x05}};
  std::array<platform::byte, 187> expected{};
  const std::array<platform::byte,8> magic{{'S','B','U','I','N','T','8','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(std::begin(descriptor.descriptor_uuid.bytes),
            std::end(descriptor.descriptor_uuid.bytes), expected.begin() + 8);
  platform::StoreLittle64(expected.data() + 24, descriptor.descriptor_epoch);
  platform::StoreLittle32(expected.data() + 32, descriptor.canonical_type_id);
  platform::StoreLittle32(expected.data() + 36, 25);
  std::copy(type_uuid.bytes.begin(), type_uuid.bytes.end(), expected.begin() + 40);
  std::copy(codec_uuid.bytes.begin(), codec_uuid.bytes.end(), expected.begin() + 56);
  platform::StoreLittle32(expected.data() + 72, 1);  // descriptor generation
  platform::StoreLittle32(expected.data() + 76, 1);  // type generation
  platform::StoreLittle32(expected.data() + 80, 1);  // codec generation
  platform::StoreLittle32(expected.data() + 84, 24); // SQL NULL frame size
  std::size_t offset = 88;
  for (platform::byte raw : {platform::byte{0x00}, platform::byte{0x80},
                             platform::byte{0xff}}) {
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint8, dt::DatatypePhysicalValueState::value,
         {raw}});
    Check(physical.ok() && physical.bytes.size() == 25,
          "production uint8 physical boundary encoding");
    if (physical.ok()) {
      std::copy(physical.bytes.begin(), physical.bytes.end(),
                expected.begin() + offset);
    }
    offset += 25;
  }
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uint8,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(physical_null.ok() && physical_null.bytes.size() == 24,
        "production uint8 physical SQL NULL boundary encoding");
  if (physical_null.ok()) {
    std::copy(physical_null.bytes.begin(), physical_null.bytes.end(),
              expected.begin() + offset);
  }
#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-uint8-" + std::to_string(pid) + ".codec");
  struct Cleanup { fs::path p; ~Cleanup(){ std::error_code e; fs::remove(p,e); } } cleanup{path};
  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create uint8 persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact uint8 bytes");
  disk::FileDevice reader;
  Check(reader.Open(path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen uint8 persistence fixture");
  std::array<platform::byte, 187> actual{};
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = true;
  offset = 88;
  for (platform::byte raw : {platform::byte{0x00}, platform::byte{0x80},
                             platform::byte{0xff}}) {
    const auto decoded = dt::DecodeDatatypePhysicalValue(actual.data() + offset, 25);
    decoded_all = decoded_all && decoded.ok() &&
        decoded.value.payload == std::vector<platform::byte>{raw};
    offset += 25;
  }
  const auto decoded_null = dt::DecodeDatatypePhysicalValue(
      actual.data() + offset, 24);
  decoded_all = decoded_all && decoded_null.ok() &&
      decoded_null.value.type_id == dt::CanonicalTypeId::uint8 &&
      decoded_null.value.state == dt::DatatypePhysicalValueState::sql_null &&
      decoded_null.value.payload.empty();
  const bool reopened_identity =
      std::equal(actual.begin() + 8, actual.begin() + 24,
                 std::begin(descriptor.descriptor_uuid.bytes)) &&
      platform::LoadLittle64(actual.data() + 24) == descriptor.descriptor_epoch &&
      platform::LoadLittle32(actual.data() + 32) == descriptor.canonical_type_id &&
      std::equal(actual.begin() + 40, actual.begin() + 56,
                 type_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 56, actual.begin() + 72,
                 codec_uuid.bytes.begin()) &&
      platform::LoadLittle32(actual.data() + 72) == 1 &&
      platform::LoadLittle32(actual.data() + 76) == 1 &&
      platform::LoadLittle32(actual.data() + 80) == 1;
  Check(read.ok() && read.bytes_transferred == actual.size() &&
            actual == expected && reopened_identity && decoded_all,
        "reopen preserves exact test-owned identity generations and production "
        "physical value/NULL frames");
  std::array<platform::byte, 2> short_bytes{};
  const auto short_read = reader.ReadAt(
      actual.size() - 1, short_bytes.data(), short_bytes.size());
  const bool short_frame_refused =
      !short_read.ok() && short_read.bytes_transferred < short_bytes.size();
  Check(short_frame_refused,
        "persistence boundary refuses a short physical-frame read");
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only persistence boundary refuses a failed write");
  Check(reader.Close().ok(), "close reopened uint8 fixture");
}
}  // namespace

int main() {
  IdentityAndPhysicalCodecs();
  OperationsAndSerialization();
  OwnedUint8Producers();
  SblrBoundaryAdapters();
  EngineBoundaryAdapters();
  ExecutorBoundaryAdapters();
  Persistence();
  std::cout << "base uint8 checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
