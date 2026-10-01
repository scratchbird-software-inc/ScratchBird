// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;
namespace executor = scratchbird::engine::executor;

namespace {

unsigned checks = 0;
unsigned failures = 0;

void Check(bool ok, const std::string& reason) {
  ++checks;
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << reason << '\n';
  }
}

scratchbird::engine::ExecutionTypeDescriptor Int16Descriptor() {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto row = dt::LookupDatatypeCatalogRow(
      manifest.manifest, dt::CanonicalTypeId::int16);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};

  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto descriptor = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::int16, metadata);
  return descriptor.ok()
      ? descriptor.descriptor
      : scratchbird::engine::ExecutionTypeDescriptor{};
}

dt::DatatypeOperationValue Int16(std::int32_t value) {
  const auto raw = static_cast<std::uint16_t>(value);
  return {dt::CanonicalTypeId::int16,
          std::string{static_cast<char>(raw & 0xffu),
                      static_cast<char>((raw >> 8u) & 0xffu)},
          false};
}

std::vector<platform::byte> Payload(const dt::DatatypeOperationValue& value) {
  return {static_cast<platform::byte>(value.encoded_value[0]),
          static_cast<platform::byte>(value.encoded_value[1])};
}

void ExactIdentity() {
  constexpr platform::Uuid descriptor_uuid{{
      0x65,0,0,0,0x69,0x6e,0x74,0x31,0xb6,0,0,0,0,0,0,0}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x02}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x03}};

  const auto descriptor = Int16Descriptor();
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes),
                   descriptor_uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.stable_name == "int16" &&
            descriptor.bit_width == 16,
        "exact int16 descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::int16);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid == codec_uuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.int16.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 2 &&
            found->canonical_value_maximum_bytes == 2 &&
            found->canonical_value_exact_bytes == 2 && found->null_supported &&
            found->canonical_byte_order == "little_endian" &&
            found->canonical_representation == "twos_complement_integer",
        "exact int16 type-codec tuple");

  const auto v4 = dt::LookupDatatypeTypeCodecIdentityV1(
      dt::kDatatypeCohortV4, 4, 4, descriptor_uuid, 1);
  const auto v5 = dt::LookupDatatypeTypeCodecIdentityV1(
      dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 1);
  Check(v4.ok && v5.ok && v4.row.type_uuid == type_uuid &&
            v5.row.type_uuid == type_uuid,
        "int16 identity is admitted only through exact cohort receipts");
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV4, 5, 5, descriptor_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 2).ok,
        "int16 identity rejects mismatched generation receipts");
}

void ExhaustiveRepresentationAndOrder() {
  std::string previous_key;
  auto previous_value = Int16(std::numeric_limits<std::int16_t>::min());

  for (std::int32_t number = std::numeric_limits<std::int16_t>::min();
       number <= std::numeric_limits<std::int16_t>::max(); ++number) {
    const auto value = Int16(number);
    const auto expected_payload = Payload(value);

    std::string encoded;
    std::int64_t decoded_number = 0;
    const bool operation_round_trip =
        dt::EncodeCanonicalInt16Value(number, &encoded) &&
        encoded == value.encoded_value &&
        dt::DecodeCanonicalInt16Value(encoded, &decoded_number) &&
        decoded_number == number;

    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::int16, false, false, expected_payload});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::int16,
         dt::DatatypePhysicalValueState::value, expected_payload});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};

    const auto key = dt::MakeDatatypeSortKey({value});
    const std::string expected_key{
        char(1),
        static_cast<char>(
            static_cast<unsigned char>(value.encoded_value[1]) ^ 0x80u),
        value.encoded_value[0]};
    bool ordered = key.ok() && key.sort_key == expected_key;
    if (number != std::numeric_limits<std::int16_t>::min()) {
      const auto compared = dt::CompareDatatypeValues({previous_value, value});
      ordered = ordered && compared.ok() && compared.comparison == -1 &&
          previous_key < key.sort_key;
    }

    Check(operation_round_trip && binary.ok() && binary_back.ok() &&
              binary_back.value.type_id == dt::CanonicalTypeId::int16 &&
              !binary_back.value.is_null &&
              binary_back.value.payload == expected_payload &&
              physical.ok() && physical_back.ok() &&
              physical_back.value.type_id == dt::CanonicalTypeId::int16 &&
              physical_back.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              physical_back.value.payload == expected_payload && ordered,
          "all 65536 int16 values preserve exact bytes and signed order");
    previous_key = key.sort_key;
    previous_value = value;
  }

  std::string encoded;
  std::int64_t decoded = 0;
  Check(!dt::EncodeCanonicalInt16Value(-32769, &encoded) &&
            !dt::EncodeCanonicalInt16Value(32768, &encoded),
        "int16 mathematical encoder rejects values outside its range");
  Check(!dt::DecodeCanonicalInt16Value({}, &decoded) &&
            !dt::DecodeCanonicalInt16Value(std::string(1, '\0'), &decoded) &&
            !dt::DecodeCanonicalInt16Value(std::string(3, '\0'), &decoded),
        "int16 mathematical decoder requires exactly two bytes");

  for (const auto& bad : {std::vector<platform::byte>{},
                          std::vector<platform::byte>{0},
                          std::vector<platform::byte>{0, 1, 2}}) {
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::int16, false, false, bad}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::int16,
                dt::DatatypePhysicalValueState::value, bad}).ok(),
          "int16 codecs reject non-two-byte present values");
  }

  constexpr std::array<std::int32_t, 5> probes{{-32768, -1, 0, 1, 32767}};
  for (std::size_t left = 0; left < probes.size(); ++left) {
    for (std::size_t right = 0; right < probes.size(); ++right) {
      const auto compared = dt::CompareDatatypeValues(
          {Int16(probes[left]), Int16(probes[right])});
      const int expected = left < right ? -1 : (left > right ? 1 : 0);
      Check(compared.ok() && compared.comparison == expected,
            "int16 signed comparison matrix");
    }
  }

  struct LiteralVector {
    std::int32_t value;
    std::string payload;
    std::string key;
  };
  const std::array<LiteralVector, 5> literals{{
      {-32768, std::string{"\x00\x80", 2}, std::string{"\x01\x00\x00", 3}},
      {-1, std::string{"\xff\xff", 2}, std::string{"\x01\x7f\xff", 3}},
      {0, std::string{"\x00\x00", 2}, std::string{"\x01\x80\x00", 3}},
      {1, std::string{"\x01\x00", 2}, std::string{"\x01\x80\x01", 3}},
      {32767, std::string{"\xff\x7f", 2}, std::string{"\x01\xff\xff", 3}},
  }};
  for (const auto& literal : literals) {
    std::string encoded_value;
    const auto key = dt::MakeDatatypeSortKey({Int16(literal.value)});
    Check(dt::EncodeCanonicalInt16Value(literal.value, &encoded_value) &&
              encoded_value == literal.payload && key.ok() &&
              key.sort_key == literal.key,
          "int16 literal payload and ordered-key vector");
  }

  for (const auto& malformed :
       {std::string{}, std::string(1, '\0'), std::string(3, '\0')}) {
    const dt::DatatypeOperationValue value{
        dt::CanonicalTypeId::int16, malformed, false};
    Check(!dt::CompareDatatypeValues({value, Int16(0)}).ok() &&
              !dt::MakeDatatypeSortKey({value}).ok() &&
              !dt::SerializeDatatypeValue({value}).ok() &&
              !dt::HashDatatypeValue({value}).ok() &&
              !dt::RenderDatatypeValueForDisplay({value}).ok(),
          "int16 operation surfaces reject malformed payload widths");
  }

  for (const auto value : {-32768, -1, 0, 1, 32767}) {
    const auto input = Int16(value);
    const auto serialized = dt::SerializeDatatypeValue({input});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int16;
    request.serialized_value = serialized.serialized_value;
    const auto decoded_value = dt::DeserializeDatatypeValue(request);
    Check(serialized.ok() && decoded_value.ok() &&
              decoded_value.value.type_id == dt::CanonicalTypeId::int16 &&
              !decoded_value.value.is_null &&
              decoded_value.value.encoded_value == input.encoded_value,
          "generic serialization preserves exact int16 payload bytes");
  }
  for (const char* malformed_frame :
       {"SBDV1;type=int16;state=value;payload=",
        "SBDV1;type=int16;state=value;payload=00",
        "SBDV1;type=int16;state=value;payload=000000"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::int16;
    request.serialized_value = malformed_frame;
    Check(!dt::DeserializeDatatypeValue(request).ok(),
          "generic deserialization rejects malformed int16 payload widths");
  }
}

void NullState() {
  auto descriptor = Int16Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue null_value{
      dt::CanonicalTypeId::int16, {}, true};
  null_value.descriptor = descriptor;

  const auto null_first = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_last});
  Check(null_first.ok() && null_first.sort_key == std::string(1, '\0') &&
            null_last.ok() && null_last.sort_key == std::string(1, '\2'),
        "int16 NULL sort keys use exact external state bytes");

  for (const auto boundary : {-32768, 32767}) {
    const auto first_left = dt::CompareDatatypeValues(
        {null_value, Int16(boundary), dt::DatatypeNullOrdering::nulls_first});
    const auto first_right = dt::CompareDatatypeValues(
        {Int16(boundary), null_value, dt::DatatypeNullOrdering::nulls_first});
    const auto last_left = dt::CompareDatatypeValues(
        {null_value, Int16(boundary), dt::DatatypeNullOrdering::nulls_last});
    const auto last_right = dt::CompareDatatypeValues(
        {Int16(boundary), null_value, dt::DatatypeNullOrdering::nulls_last});
    Check(first_left.ok() && first_left.comparison == -1 &&
              first_right.ok() && first_right.comparison == 1 &&
              last_left.ok() && last_left.comparison == 1 &&
              last_right.ok() && last_right.comparison == -1,
          "int16 NULL comparison follows requested ordering");
  }
  const auto null_equal = dt::CompareDatatypeValues({null_value, null_value});
  Check(null_equal.ok() && null_equal.comparison == 0,
        "two typed int16 NULL values compare equal");

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::int16, true, false, {}});
  const auto binary_back = binary_null.ok()
      ? dt::DecodeDatatypeBinaryValue(binary_null.encoded)
      : dt::DatatypeBinaryResult{};
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::int16,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto physical_back = physical_null.ok()
      ? dt::DecodeDatatypePhysicalValue(physical_null.bytes.data(),
                                        physical_null.bytes.size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(binary_null.ok() && binary_back.ok() &&
            binary_back.value.type_id == dt::CanonicalTypeId::int16 &&
            binary_back.value.is_null && binary_back.value.payload.empty() &&
            physical_null.ok() && physical_back.ok() &&
            physical_back.value.type_id == dt::CanonicalTypeId::int16 &&
            physical_back.value.state ==
                dt::DatatypePhysicalValueState::sql_null &&
            physical_back.value.payload.empty(),
        "int16 codecs preserve typed NULL as external zero-payload state");
  Check(!dt::EncodeDatatypeBinaryValue(
             {dt::CanonicalTypeId::int16, true, false, {0, 0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
             {dt::CanonicalTypeId::int16,
              dt::DatatypePhysicalValueState::sql_null, {0, 0}}).ok(),
        "int16 codecs reject payload-bearing NULL");

  const auto present_zero = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::int16, false, false, {0, 0}});
  Check(present_zero.ok() && binary_null.ok() &&
            present_zero.encoded != binary_null.encoded,
        "present int16 zero remains distinct from typed NULL");

  const auto serialized_null = dt::SerializeDatatypeValue({null_value});
  dt::DatatypeDeserializationRequest null_decode;
  null_decode.expected_type_id = dt::CanonicalTypeId::int16;
  null_decode.expected_descriptor = descriptor;
  null_decode.serialized_value = serialized_null.serialized_value;
  const auto deserialized_null = dt::DeserializeDatatypeValue(null_decode);
  Check(serialized_null.ok() && deserialized_null.ok() &&
            deserialized_null.value.type_id == dt::CanonicalTypeId::int16 &&
            deserialized_null.value.is_null &&
            deserialized_null.value.encoded_value.empty(),
        "generic serialization preserves typed int16 NULL state");
  null_decode.serialized_value =
      "SBDV1;type=int16;state=null;payload=0000";
  Check(!dt::DeserializeDatatypeValue(null_decode).ok(),
        "generic deserialization rejects payload-bearing int16 NULL");
  Check(!dt::HashDatatypeValue({null_value}).ok(),
        "typed int16 NULL refuses absent containing hash profile");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = null_value;
    identity.target_type_id = dt::CanonicalTypeId::int16;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast =
        context == dt::DatatypeCastContext::explicit_cast;
    const auto result = dt::CastDatatypeValue(identity);
    Check(result.ok() && result.category == dt::DatatypeCastCategory::identity &&
              result.value.type_id == dt::CanonicalTypeId::int16 &&
              result.value.is_null && result.value.encoded_value.empty(),
          "typed int16 NULL identity validates in every cast context");
  }

  dt::DatatypeCastRequest contextual_null;
  contextual_null.value = {dt::CanonicalTypeId::null_type, {}, true};
  contextual_null.target_type_id = dt::CanonicalTypeId::int16;
  contextual_null.target_descriptor = descriptor;
  contextual_null.context = dt::DatatypeCastContext::implicit;
  const auto bound = dt::CastDatatypeValue(contextual_null);
  Check(bound.ok() &&
            bound.category == dt::DatatypeCastCategory::lossless_implicit &&
            bound.value.type_id == dt::CanonicalTypeId::int16 &&
            bound.value.is_null && bound.value.encoded_value.empty(),
        "contextual base.null binds to nullable int16 with zero payload");
  contextual_null.target_descriptor.nullable_allowed = false;
  Check(!dt::CastDatatypeValue(contextual_null).ok(),
        "contextual base.null cannot bind to non-nullable int16");

  dt::DatatypeCastRequest standalone_target;
  standalone_target.value = Int16(0);
  standalone_target.target_type_id = dt::CanonicalTypeId::null_type;
  Check(!dt::CastDatatypeValue(standalone_target).ok(),
        "int16 cannot cast to the standalone null sentinel");
}

void UnresolvedPoliciesFailClosed() {
  Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::int16,
                                dt::CanonicalTypeId::int16) ==
            dt::DatatypeCastCategory::forbidden &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::int16,
                                     dt::CanonicalTypeId::int8) ==
                dt::DatatypeCastCategory::forbidden &&
            dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint8,
                                     dt::CanonicalTypeId::int16) ==
                dt::DatatypeCastCategory::forbidden,
        "unregistered non-NULL int16 cast pairs remain fail-closed");
  Check(!dt::HashDatatypeValue({Int16(0)}).ok(),
        "unregistered int16 hash profile remains fail-closed");
  Check(!dt::RenderDatatypeValueForDisplay({Int16(0)}).ok(),
        "unregistered int16 render profile remains fail-closed");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    for (const auto& pair :
         {std::pair{Int16(1), dt::CanonicalTypeId::int16},
          std::pair{Int16(1), dt::CanonicalTypeId::int8},
          std::pair{dt::DatatypeOperationValue{
                         dt::CanonicalTypeId::int8, std::string(1, '\1'), false},
                    dt::CanonicalTypeId::int16}}) {
      dt::DatatypeCastRequest request;
      request.value = pair.first;
      request.target_type_id = pair.second;
      request.context = context;
      request.explicit_cast =
          context == dt::DatatypeCastContext::explicit_cast;
      Check(!dt::CastDatatypeValue(request).ok(),
            "unregistered present int16 cast execution remains fail-closed");
    }
  }
}

void ExecutorMixedNullCarrierRefuses() {
  const auto descriptor = executor::MakeExecutorDescriptor(
      "int16", "nullability=nullable;width=16");
  const auto present = executor::MakeExecutorValue(descriptor, "12", false);
  const auto sql_null = executor::MakeExecutorValue(descriptor, {}, true);
  executor::CanonicalDescriptorOrderTerm term;
  term.expression_descriptor_id = 1;

  const auto present_null = executor::CompareCanonicalDescriptorOrderValues(
      present, sql_null, term);
  const auto null_present = executor::CompareCanonicalDescriptorOrderValues(
      sql_null, present, term);
  const auto null_null = executor::CompareCanonicalDescriptorOrderValues(
      sql_null, sql_null, term);
  const auto present_present = executor::CompareCanonicalDescriptorOrderValues(
      present, present, term);
  Check(!present_null.diagnostic.ok && !null_present.diagnostic.ok &&
            null_null.diagnostic.ok && null_null.comparison == 0 &&
            !present_present.diagnostic.ok,
        "executor refuses unresolved decimal-text int16 carriers while preserving NULL equality");
}

}  // namespace

int main() {
  ExactIdentity();
  ExhaustiveRepresentationAndOrder();
  NullState();
  UnresolvedPoliciesFailClosed();
  ExecutorMixedNullCarrierRefuses();
  std::cout << "base int16 checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
