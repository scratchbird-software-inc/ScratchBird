// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "admitted_datatype_cohort.hpp"
#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"
#include "descriptor_value_runtime.hpp"
#include "crud_support/bound_ordered_index_key.hpp"
#include "dml/direct_bulk_scalar_projection.hpp"
#include "sblr_projection_value_runtime.hpp"
#include "sblr_special_forms.hpp"
#include "../support/exact_datatype_descriptor_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace dt = scratchbird::core::datatypes;
namespace disk = scratchbird::storage::disk;
namespace executor = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace platform = scratchbird::core::platform;
namespace fs = std::filesystem;

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

scratchbird::engine::ExecutionTypeDescriptor Uint16Descriptor() {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto row = dt::LookupDatatypeCatalogRow(
      manifest.manifest, dt::CanonicalTypeId::uint16);
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto descriptor = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::uint16, metadata);
  return descriptor.ok()
      ? descriptor.descriptor
      : scratchbird::engine::ExecutionTypeDescriptor{};
}

dt::DatatypeOperationValue Uint16(std::uint32_t value) {
  return {dt::CanonicalTypeId::uint16,
          std::string{static_cast<char>(value & 0xffu),
                      static_cast<char>((value >> 8u) & 0xffu)},
          false};
}

std::vector<platform::byte> Payload(const dt::DatatypeOperationValue& value) {
  return {static_cast<platform::byte>(value.encoded_value[0]),
          static_cast<platform::byte>(value.encoded_value[1])};
}

void ExactIdentity() {
  constexpr platform::Uuid descriptor_uuid{{
      0x79,0,0,0,0x75,0x69,0x7e,0x74,0xb1,0x36,0,0,0,0,0,0}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x06}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x07}};

  const auto descriptor = Uint16Descriptor();
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes),
                   descriptor_uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 &&
            descriptor.stable_name == "uint16" &&
            descriptor.bit_width == 16,
        "exact uint16 descriptor identity");

  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.catalog_snapshot_uuid == dt::kDatatypeCohortV5 &&
        row.catalog_generation == 5 && row.registry_generation == 5 &&
        row.canonical_binary_type_code ==
            static_cast<std::uint32_t>(dt::CanonicalTypeId::uint16);
  });
  Check(found != rows.end() && found->descriptor_uuid == descriptor_uuid &&
            found->type_uuid == type_uuid && found->codec_uuid == codec_uuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.uint16.le.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 2 &&
            found->canonical_value_maximum_bytes == 2 &&
            found->canonical_value_exact_bytes == 2 && found->null_supported &&
            found->canonical_byte_order == "little_endian" &&
            found->canonical_representation == "unsigned_integer",
        "exact uint16 type-codec tuple");

  const auto v4 = dt::LookupDatatypeTypeCodecIdentityV1(
      dt::kDatatypeCohortV4, 4, 4, descriptor_uuid, 1);
  const auto v5 = dt::LookupDatatypeTypeCodecIdentityV1(
      dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 1);
  Check(v4.ok && v5.ok && v4.row.type_uuid == type_uuid &&
            v5.row.type_uuid == type_uuid,
        "uint16 exact identity survives admitted cohort inheritance");
  Check(!dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV4, 5, 5, descriptor_uuid, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV1(
             dt::kDatatypeCohortV5, 5, 5, descriptor_uuid, 2).ok,
        "uint16 identity rejects crossed generations");
}

void ExhaustiveRepresentationAndCodecs() {
  for (std::uint32_t number = 0; number <= 65535; ++number) {
    const auto value = Uint16(number);
    const std::vector<platform::byte> expected{
        static_cast<platform::byte>(number & 0xffu),
        static_cast<platform::byte>((number >> 8u) & 0xffu)};
    std::string encoded;
    std::uint64_t decoded_number = 0;
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::uint16, false, false, expected});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded)
        : dt::DatatypeBinaryResult{};
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint16,
         dt::DatatypePhysicalValueState::value, expected});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(),
                                          physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    Check(dt::EncodeCanonicalUint16Value(number, &encoded) &&
              encoded == value.encoded_value &&
              dt::DecodeCanonicalUint16Value(encoded, &decoded_number) &&
              decoded_number == number && binary.ok() && binary_back.ok() &&
              binary_back.value.type_id == dt::CanonicalTypeId::uint16 &&
              !binary_back.value.is_null &&
              binary_back.value.payload == expected && physical.ok() &&
              physical_back.ok() &&
              physical_back.value.type_id == dt::CanonicalTypeId::uint16 &&
              physical_back.value.state ==
                  dt::DatatypePhysicalValueState::value &&
              physical_back.value.payload == expected,
          "all 65536 uint16 values preserve exact two-byte payloads");
  }

  struct Vector { std::uint32_t value; platform::byte low; platform::byte high; };
  constexpr std::array<Vector, 7> vectors{{
      {0,0x00,0x00}, {1,0x01,0x00}, {255,0xff,0x00}, {256,0x00,0x01},
      {32767,0xff,0x7f}, {32768,0x00,0x80}, {65535,0xff,0xff}}};
  for (const auto& vector : vectors) {
    std::string encoded;
    std::uint64_t decoded = 0;
    Check(dt::EncodeCanonicalUint16Value(vector.value, &encoded) &&
              encoded == std::string{static_cast<char>(vector.low),
                                     static_cast<char>(vector.high)} &&
              dt::DecodeCanonicalUint16Value(encoded, &decoded) &&
              decoded == vector.value,
          "uint16 literal payload vector");
  }

  std::string unchanged = "sentinel";
  std::uint64_t unchanged_number = 0x1122334455667788ULL;
  Check(!dt::EncodeCanonicalUint16Value(-1, &unchanged) &&
            unchanged == "sentinel" &&
            !dt::EncodeCanonicalUint16Value(65536, &unchanged) &&
            unchanged == "sentinel" &&
            !dt::EncodeCanonicalUint16Value(0, nullptr),
        "uint16 mathematical encoder fails atomically outside range");
  Check(!dt::DecodeCanonicalUint16Value({}, &unchanged_number) &&
            unchanged_number == 0x1122334455667788ULL &&
            !dt::DecodeCanonicalUint16Value(std::string(1, '\0'),
                                            &unchanged_number) &&
            unchanged_number == 0x1122334455667788ULL &&
            !dt::DecodeCanonicalUint16Value(std::string(3, '\0'),
                                            &unchanged_number) &&
            unchanged_number == 0x1122334455667788ULL &&
            !dt::DecodeCanonicalUint16Value(std::string(2, '\0'), nullptr),
        "uint16 mathematical decoder rejects malformed input atomically");

  std::uint64_t native_text_like = 0;
  Check(dt::DecodeCanonicalUint16Value(std::string{"42", 2},
                                       &native_text_like) &&
            native_text_like == 12852,
        "two text-like bytes remain native uint16 0x3234");

  for (const auto& bad : {std::vector<platform::byte>{},
                          std::vector<platform::byte>{0},
                          std::vector<platform::byte>{0,1,2},
                          std::vector<platform::byte>(32, 0)}) {
    Check(!dt::EncodeDatatypeBinaryValue(
               {dt::CanonicalTypeId::uint16, false, false, bad}).ok() &&
              !dt::EncodeDatatypePhysicalValue(
               {dt::CanonicalTypeId::uint16,
                dt::DatatypePhysicalValueState::value, bad}).ok(),
          "uint16 codecs reject every non-two-byte present payload");
  }
  for (const auto& malformed :
       {std::string{}, std::string(1, '\0'), std::string(3, '\0'),
        std::string(32, '\0')}) {
    const dt::DatatypeOperationValue value{
        dt::CanonicalTypeId::uint16, malformed, false};
    Check(!dt::CompareDatatypeValues({value, Uint16(0)}).ok() &&
              !dt::MakeDatatypeSortKey({value}).ok() &&
              !dt::SerializeDatatypeValue({value}).ok() &&
              !dt::HashDatatypeValue({value}).ok() &&
              !dt::RenderDatatypeValueForDisplay({value}).ok(),
          "uint16 operation surfaces reject malformed carrier widths");
  }

  std::uint32_t previous = 0;
  std::string previous_key;
  for (std::uint32_t number = 0; number <= 65535; ++number) {
    const auto equal = dt::CompareDatatypeValues({Uint16(number), Uint16(number)});
    const auto key = dt::MakeDatatypeSortKey({Uint16(number)});
    const std::string expected_key{
        char(1), static_cast<char>((number >> 8u) & 0xffu),
        static_cast<char>(number & 0xffu)};
    bool ordered = equal.ok() && equal.comparison == 0 && key.ok() &&
        key.sort_key == expected_key;
    if (number != 0) {
      const auto next = dt::CompareDatatypeValues({Uint16(previous), Uint16(number)});
      ordered = ordered && next.ok() && next.comparison == -1 &&
          previous_key < key.sort_key;
    }
    Check(ordered, "uint16 comparison and key follow unsigned mathematical order");
    previous = number;
    previous_key = key.sort_key;
  }

  for (const auto number : {0u, 255u, 256u, 32768u, 65535u}) {
    const auto input = Uint16(number);
    const auto serialized = dt::SerializeDatatypeValue({input});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::uint16;
    request.serialized_value = serialized.serialized_value;
    const auto restored = dt::DeserializeDatatypeValue(request);
    Check(serialized.ok() && restored.ok() &&
              restored.value.type_id == dt::CanonicalTypeId::uint16 &&
              !restored.value.is_null &&
              restored.value.encoded_value == input.encoded_value,
          "generic value serialization preserves uint16 native bytes");
  }
  for (const char* bad : {"SBDV1;type=uint16;state=value;payload=",
                          "SBDV1;type=uint16;state=value;payload=00",
                          "SBDV1;type=uint16;state=value;payload=000000"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::uint16;
    request.serialized_value = bad;
    Check(!dt::DeserializeDatatypeValue(request).ok(),
          "generic value deserializer rejects malformed uint16 widths");
  }
}

void NullAndAbsentPolicies() {
  auto descriptor = Uint16Descriptor();
  descriptor.nullable_allowed = true;
  dt::DatatypeOperationValue null_value{
      dt::CanonicalTypeId::uint16, {}, true};
  null_value.descriptor = descriptor;

  const auto binary_null = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uint16, true, false, {}});
  const auto binary_back = binary_null.ok()
      ? dt::DecodeDatatypeBinaryValue(binary_null.encoded)
      : dt::DatatypeBinaryResult{};
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uint16,
       dt::DatatypePhysicalValueState::sql_null, {}});
  const auto physical_back = physical_null.ok()
      ? dt::DecodeDatatypePhysicalValue(physical_null.bytes.data(),
                                        physical_null.bytes.size())
      : dt::DatatypePhysicalEncodingResult{};
  Check(binary_null.ok() && binary_back.ok() && binary_back.value.is_null &&
            binary_back.value.payload.empty() && physical_null.ok() &&
            physical_back.ok() &&
            physical_back.value.type_id == dt::CanonicalTypeId::uint16 &&
            physical_back.value.state ==
                dt::DatatypePhysicalValueState::sql_null &&
            physical_back.value.payload.empty(),
        "uint16 typed NULL is external state with zero payload");
  Check(!dt::EncodeDatatypeBinaryValue(
             {dt::CanonicalTypeId::uint16, true, false, {0,0}}).ok() &&
            !dt::EncodeDatatypePhysicalValue(
             {dt::CanonicalTypeId::uint16,
              dt::DatatypePhysicalValueState::sql_null, {0,0}}).ok(),
        "uint16 codecs reject payload-bearing NULL");
  const auto present_zero = dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::uint16, false, false, {0,0}});
  Check(present_zero.ok() && present_zero.encoded != binary_null.encoded,
        "present uint16 zero remains distinct from typed NULL");

  const auto serialized_null = dt::SerializeDatatypeValue({null_value});
  dt::DatatypeDeserializationRequest null_decode;
  null_decode.expected_type_id = dt::CanonicalTypeId::uint16;
  null_decode.expected_descriptor = descriptor;
  null_decode.serialized_value = serialized_null.serialized_value;
  const auto restored_null = dt::DeserializeDatatypeValue(null_decode);
  Check(serialized_null.ok() && restored_null.ok() &&
            restored_null.value.type_id == dt::CanonicalTypeId::uint16 &&
            restored_null.value.is_null &&
            restored_null.value.encoded_value.empty(),
        "generic value serialization preserves typed uint16 NULL state");
  null_decode.serialized_value =
      "SBDV1;type=uint16;state=null;payload=0000";
  Check(!dt::DeserializeDatatypeValue(null_decode).ok(),
        "generic value deserialization rejects payload-bearing uint16 NULL");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest identity;
    identity.value = null_value;
    identity.target_type_id = dt::CanonicalTypeId::uint16;
    identity.target_descriptor = descriptor;
    identity.context = context;
    identity.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    const auto result = dt::CastDatatypeValue(identity);
    Check(result.ok() && result.category == dt::DatatypeCastCategory::identity &&
              result.value.is_null && result.value.encoded_value.empty(),
          "typed uint16 NULL identity validates exact descriptor/state");
  }
  dt::DatatypeCastRequest contextual;
  contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
  contextual.target_type_id = dt::CanonicalTypeId::uint16;
  contextual.target_descriptor = descriptor;
  const auto bound = dt::CastDatatypeValue(contextual);
  Check(bound.ok() && bound.value.type_id == dt::CanonicalTypeId::uint16 &&
            bound.value.is_null && bound.value.encoded_value.empty(),
        "contextual NULL binds to an exact nullable uint16 descriptor");
  contextual.target_descriptor.nullable_allowed = false;
  Check(!dt::CastDatatypeValue(contextual).ok(),
        "contextual NULL refuses a non-nullable uint16 descriptor");
  contextual.target_descriptor = descriptor;
  ++contextual.target_descriptor.descriptor_epoch;
  Check(!dt::CastDatatypeValue(contextual).ok(),
        "contextual NULL refuses a mismatched uint16 descriptor generation");

  dt::DatatypeCastRequest standalone_target;
  standalone_target.value = Uint16(0);
  standalone_target.target_type_id = dt::CanonicalTypeId::null_type;
  Check(!dt::CastDatatypeValue(standalone_target).ok(),
        "uint16 cannot cast to the standalone NULL sentinel");

  for (const auto& candidate : dt::BuiltinDatatypeDescriptors()) {
    const auto outgoing = candidate.type_id == dt::CanonicalTypeId::real64
        ? dt::DatatypeCastCategory::lossless_implicit : dt::DatatypeCastCategory::forbidden;
    const auto incoming = candidate.type_id == dt::CanonicalTypeId::real64
        ? dt::DatatypeCastCategory::lossy_explicit : dt::DatatypeCastCategory::forbidden;
    for (bool compatibility : {false, true}) {
      Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::uint16, candidate.type_id, compatibility) == outgoing &&
            dt::ClassifyDatatypeCast(candidate.type_id, dt::CanonicalTypeId::uint16, compatibility) == incoming,
            "uint16 cast matrix includes exact REAL64 widening and checked narrowing categories");
    }
  }
  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment,
                             dt::DatatypeCastContext::explicit_cast}) {
    dt::DatatypeCastRequest present;
    present.value = Uint16(42);
    present.target_type_id = dt::CanonicalTypeId::uint16;
    present.context = context;
    present.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint16 identity cast refuses in every context");

    present.target_type_id = dt::CanonicalTypeId::int32;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint16 outgoing cast refuses in every context");

    present.value = {dt::CanonicalTypeId::int32, "42", false};
    present.target_type_id = dt::CanonicalTypeId::uint16;
    Check(!dt::CastDatatypeValue(present).ok(),
          "present uint16 incoming cast refuses in every context");
  }
  const auto null_first = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_first});
  const auto null_last = dt::MakeDatatypeSortKey(
      {null_value, dt::DatatypeNullOrdering::nulls_last});
  const auto first_compare = dt::CompareDatatypeValues(
      {null_value, Uint16(0), dt::DatatypeNullOrdering::nulls_first});
  const auto last_compare = dt::CompareDatatypeValues(
      {null_value, Uint16(65535), dt::DatatypeNullOrdering::nulls_last});
  Check(null_first.ok() && null_first.sort_key == std::string(1, '\0') &&
            null_last.ok() && null_last.sort_key == std::string(1, '\2'),
        "uint16 keys honor explicit containing NULL placement");
  Check(!first_compare.ok() && !last_compare.ok() &&
            !dt::CompareDatatypeValues({null_value, null_value}).ok(),
        "uint16 NULL comparison refuses without an owning operator policy");

  Check(!dt::HashDatatypeValue({Uint16(42)}).ok() &&
            !dt::HashDatatypeValue({null_value}).ok() &&
            !dt::RenderDatatypeValueForDisplay({Uint16(42)}).ok(),
        "undefined uint16 hash and display policies refuse");
  const dt::DatatypeOperationValue signed_value{
      dt::CanonicalTypeId::int16, std::string{'\x2a', '\0'}, false};
  Check(!dt::CompareDatatypeValues({Uint16(42), signed_value}).ok(),
        "mixed unsigned/signed comparison refuses without a registered rule");
}

api::EngineDescriptor EngineUint16Descriptor(bool nullable) {
  return scratchbird::tests::ExactScalarDescriptorFixture(
      dt::CanonicalTypeId::uint16, "uint16",
      scratchbird::tests::ParsedFixtureUuid(
          nullable ? "019dffbc-1600-7000-8000-000000000002"
                   : "019dffbc-1600-7000-8000-000000000001"),
      "nullability=" + std::string(nullable ? "nullable" : "non_null"));
}

void ConsumerCarrierBoundaries() {
  constexpr std::uint32_t descriptor_id = 1;
  const auto descriptor = EngineUint16Descriptor(false);
  const auto validate = [&](api::EngineTypedValue value, bool expected_ok,
                            const std::string& label,
                            const api::EngineDescriptor& expected_descriptor,
                            bool nullable) {
    executor::DescriptorBatch batch;
    batch.columns.push_back(
        {"uint16_value", expected_descriptor, nullable, descriptor_id});
    executor::DescriptorTuple row;
    row.values.push_back(std::move(value));
    batch.rows.push_back(std::move(row));
    const auto ordinary = executor::ValidateDescriptorBatch(batch);
    const auto canonical = executor::ValidateCanonicalDescriptorBatch(
        batch, {descriptor_id});
    const bool refusal_shape = expected_ok ||
        (!ordinary.ok && !canonical.ok && ordinary.column_index == 0 &&
         canonical.column_index == 0 && !ordinary.diagnostic_code.empty() &&
         !canonical.diagnostic_code.empty());
    Check(ordinary.ok == expected_ok && canonical.ok == expected_ok &&
              refusal_shape,
          label);
  };

  api::EngineTypedValue native;
  native.descriptor = descriptor;
  native.binary_value = {0x34, 0x32};
  native.state = api::EngineValueState::value;
  validate(native, false,
           "both executor validators refuse uint16 until a carrier profile exists",
           descriptor, false);

  auto lexical = native;
  lexical.binary_value.clear();
  lexical.encoded_value = "42";
  validate(lexical, false,
           "both executor validators reject a uint16 decimal-text carrier",
           descriptor, false);
  auto empty = native;
  empty.binary_value.clear();
  validate(empty, false,
           "both executor validators reject an empty present uint16 carrier",
           descriptor, false);
  for (const auto bytes : {std::vector<std::uint8_t>{0x34},
                           std::vector<std::uint8_t>{0x34, 0x32, 0x00}}) {
    auto wrong_width = native;
    wrong_width.binary_value = bytes;
    validate(wrong_width, false,
             "both executor validators reject a wrong-width uint16 carrier",
             descriptor, false);
  }
  auto dual = native;
  dual.encoded_value = "12852";
  validate(dual, false,
           "both executor validators reject dual uint16 carriers",
           descriptor, false);

  const auto nullable = EngineUint16Descriptor(true);
  api::EngineTypedValue sql_null;
  sql_null.descriptor = nullable;
  sql_null.is_null = true;
  sql_null.state = api::EngineValueState::sql_null;
  validate(sql_null, true,
           "both executor validators admit zero-payload typed uint16 NULL",
           nullable, true);
  auto payload_null = sql_null;
  payload_null.binary_value = {0, 0};
  validate(payload_null, false,
           "both executor validators reject payload-bearing uint16 NULL",
           nullable, true);
  auto lexical_null = sql_null;
  lexical_null.encoded_value = "0";
  validate(lexical_null, false,
           "both executor validators reject text-bearing uint16 NULL",
           nullable, true);
  auto missing_flag = sql_null;
  missing_flag.is_null = false;
  validate(missing_flag, false,
           "both executor validators reject SQL NULL without the NULL flag",
           nullable, true);
  auto legacy_flag = sql_null;
  legacy_flag.state = api::EngineValueState::value;
  validate(legacy_flag, false,
           "both executor validators reject a legacy NULL flag on value state",
           nullable, true);
  auto nonnullable_null = sql_null;
  nonnullable_null.descriptor = descriptor;
  validate(nonnullable_null, false,
           "both executor validators reject uint16 NULL for a non-null column",
           descriptor, false);

  for (const auto& value : {native, lexical}) {
    bool rejected = false;
    try {
      (void)api::dml::detail::DirectTypedStoredValue(value);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected,
          "direct DML refuses uint16 until its carrier profile is registered");
  }

  api::bound_index_key::OrderedIndexColumn index_column;
  index_column.datatype.type_id = dt::CanonicalTypeId::uint16;
  for (const auto& untagged :
       {api::CrudStoredValue{std::string{"\x34\x32", 2}},
        api::CrudStoredValue{"42"},
        api::CrudStoredValue{std::string{"\0\0", 2}},
        api::CrudStoredValue{std::string{"\xff\xff", 2}},
        api::CrudStoredValue::SqlNull()}) {
    std::string output = "unchanged";
    bool null_key = true;
    api::EngineApiDiagnostic diagnostic;
    Check(!api::bound_index_key::EncodeOrderedIndexKey(
              api::EncodeStoredLogicalKey({untagged}), {index_column}, &output,
              &null_key, &diagnostic) &&
              output == "unchanged" && null_key &&
              diagnostic.detail.ends_with(
                  "sorted_index_codec_provenance_unbound") &&
              diagnostic.code == "SB_ENGINE_API_INVALID_REQUEST",
          "ordered-index adapter refuses every untagged uint16 retained state");
  }

  // Pure key encoding can use an exact codec binding. Effectful publication
  // still acquires its own fresh catalog scope; this fixture grants no lease.
  const auto execution_descriptor = Uint16Descriptor();
  platform::Uuid descriptor_uuid;
  std::copy(std::begin(execution_descriptor.descriptor_uuid.bytes),
            std::end(execution_descriptor.descriptor_uuid.bytes), descriptor_uuid.bytes.begin());
  Check(dt::LookupDatatypeStorageIdentityV3(dt::kDatatypeCohortV5, 5, 5,
            descriptor_uuid, execution_descriptor.descriptor_epoch, &index_column.datatype),
        "bind exact uint16 retained codec for ordered key encoding");
  index_column.descriptor = scratchbird::core::uuid::MakeTypedUuid(
      platform::UuidKind::object, descriptor_uuid).value;
  index_column.execution_descriptor = execution_descriptor;
  std::string previous;
  for (const unsigned number : {0u, 1u, 42u, 255u, 256u, 0x3234u, 32768u, 65535u}) {
    const std::string bytes{static_cast<char>(number), static_cast<char>(number >> 8)};
    std::string output;
    bool null_key = true;
    api::EngineApiDiagnostic diagnostic;
    Check(api::bound_index_key::EncodeOrderedIndexKey(
              api::EncodeStoredLogicalKey({api::CrudStoredValue{bytes}}),
              {index_column}, &output, &null_key, &diagnostic) && !null_key &&
              !output.empty(), "exact uint16 binding accepts native LE2 including digit bytes");
    if (!previous.empty()) {
      const auto ordered = scratchbird::core::index::CompareEncodedIndexKeyBytes(previous, output);
      Check(ordered.ok() && ordered.comparison < 0, "bound uint16 keys follow unsigned value order");
    }
    previous = output;
  }
  for (unsigned mutation = 0; mutation < 12; ++mutation) {
    auto invalid = index_column;
    auto& codec = invalid.datatype.codec->legacy_fields;
    switch (mutation) {
      case 0: codec.catalog_snapshot_uuid = {}; break;
      case 1: ++codec.catalog_generation; break;
      case 2: ++codec.registry_generation; break;
      case 3: ++codec.descriptor_generation; break;
      case 4: codec.type_uuid = {}; break;
      case 5: ++codec.type_generation; break;
      case 6: ++codec.codec_generation; break;
      case 7: codec.codec_uuid.bytes[15] ^= 1; break;
      case 8: codec.canonical_value_exact_bytes = 1; break;
      case 9: codec.canonical_representation = "text"; break;
      case 10: invalid.datatype.type_uuid = {}; break;
      case 11: invalid.execution_descriptor.descriptor_epoch++; break;
    }
    std::string output = "unchanged";
    bool null_key = true;
    api::EngineApiDiagnostic diagnostic;
    Check(!api::bound_index_key::EncodeOrderedIndexKey(
              api::EncodeStoredLogicalKey({api::CrudStoredValue{std::string("42")}}),
              {invalid}, &output, &null_key, &diagnostic) && output == "unchanged" && null_key,
          "uint16 key encoder rejects codec or descriptor drift without replacing output");
  }
  {
    auto relabeled = index_column;
    relabeled.datatype.codec->legacy_fields.codec_id = "localized codec name";
    relabeled.datatype.codec->legacy_fields.canonical_name = "localized datatype name";
    const auto logical = api::EncodeStoredLogicalKey({api::CrudStoredValue{std::string("42")}});
    std::string original, localized;
    bool original_null = true, localized_null = true;
    api::EngineApiDiagnostic diagnostic;
    Check(api::bound_index_key::EncodeOrderedIndexKey(logical, {index_column},
              &original, &original_null, &diagnostic) &&
          api::bound_index_key::EncodeOrderedIndexKey(logical, {relabeled},
              &localized, &localized_null, &diagnostic) &&
          original == localized && !original_null && !localized_null,
          "uint16 presentation labels cannot replace binary codec identity");
  }
  for (unsigned width : {0u, 1u, 3u, 8u}) {
    std::string output = "unchanged";
    bool null_key = true;
    api::EngineApiDiagnostic diagnostic;
    Check(!api::bound_index_key::EncodeOrderedIndexKey(
              api::EncodeStoredLogicalKey({api::CrudStoredValue{std::string(width, '\0')}}),
              {index_column}, &output, &null_key, &diagnostic) && output == "unchanged" && null_key,
          "uint16 native index payload width is exact even with a bound codec");
  }

  const auto expect_sblr_refusal = [](sblr::SblrValue value,
                                      const std::string& diagnostic_id,
                                      const std::string& label) {
    const auto result = sblr::EvaluateSblrCastForm(
        "uint16_carrier", value, "uint16", {}, false, false);
    Check(!result.ok() && result.scalar_values.empty() &&
              result.diagnostics.size() == 1 &&
              result.diagnostics.front().diagnostic_id == diagnostic_id,
          label);
  };
  for (const auto number : {0u, 42u, 256u, 32768u, 65535u}) {
    sblr::SblrValue value;
    value.descriptor_id = "uint16";
    value.payload_kind = sblr::SblrValuePayloadKind::unsigned_integer;
    value.uint64_value = number;
    value.has_uint64_value = true;
    value.is_null = false;
    expect_sblr_refusal(std::move(value), "SBLR.DESCRIPTOR_MISMATCH",
                        "SBLR refuses uint16 until a carrier profile exists");
  }
  for (const auto bytes : {std::vector<std::uint8_t>{0x00, 0x00},
                           std::vector<std::uint8_t>{0x34, 0x32},
                           std::vector<std::uint8_t>{0xff, 0xff}}) {
    sblr::SblrValue value;
    value.descriptor_id = "uint16";
    value.payload_kind = sblr::SblrValuePayloadKind::unsigned_integer;
    value.binary_value = bytes;
    value.is_null = false;
    expect_sblr_refusal(std::move(value), "SBLR.DESCRIPTOR_MISMATCH",
                        "SBLR refuses uint16 native bytes without a carrier profile");
  }
  sblr::SblrValue out_of_range;
  out_of_range.descriptor_id = "uint16";
  out_of_range.payload_kind = sblr::SblrValuePayloadKind::unsigned_integer;
  out_of_range.uint64_value = 65536;
  out_of_range.has_uint64_value = true;
  out_of_range.is_null = false;
  expect_sblr_refusal(out_of_range, "SBLR.DESCRIPTOR_MISMATCH",
                      "SBLR uint16 rejects an out-of-range native scalar");
  sblr::SblrValue conflict;
  conflict.descriptor_id = "uint16";
  conflict.payload_kind = sblr::SblrValuePayloadKind::unsigned_integer;
  conflict.encoded_value = "42";
  conflict.uint64_value = 42;
  conflict.has_uint64_value = true;
  conflict.is_null = false;
  expect_sblr_refusal(conflict, "SBLR.DESCRIPTOR_MISMATCH",
                      "SBLR uint16 rejects a decimal-text shadow");
  conflict.encoded_value.clear();
  conflict.text_value = "42";
  expect_sblr_refusal(conflict, "SBLR.DESCRIPTOR_MISMATCH",
                      "SBLR uint16 rejects a presentation-text shadow");
  conflict.text_value.clear();
  conflict.binary_value = {0x2a, 0x00};
  expect_sblr_refusal(conflict, "SBLR.DESCRIPTOR_MISMATCH",
                      "SBLR uint16 rejects simultaneous numeric and binary carriers");
  for (const auto bytes : {std::vector<std::uint8_t>{0x2a},
                           std::vector<std::uint8_t>{0x2a, 0x00, 0x00}}) {
    sblr::SblrValue malformed;
    malformed.descriptor_id = "uint16";
    malformed.payload_kind = sblr::SblrValuePayloadKind::unsigned_integer;
    malformed.binary_value = bytes;
    malformed.is_null = false;
    expect_sblr_refusal(std::move(malformed), "SBLR.DESCRIPTOR_MISMATCH",
                        "SBLR uint16 rejects wrong-width native bytes");
  }
  auto wrong_kind = conflict;
  wrong_kind.binary_value.clear();
  wrong_kind.payload_kind = sblr::SblrValuePayloadKind::signed_integer;
  expect_sblr_refusal(wrong_kind, "SBLR.DESCRIPTOR_MISMATCH",
                      "SBLR uint16 rejects a signed payload kind");
  auto real_shadow = conflict;
  real_shadow.binary_value.clear();
  real_shadow.uint64_value = 42;
  real_shadow.has_real64_value = true;
  real_shadow.real64_value = 42.0;
  expect_sblr_refusal(real_shadow, "SBLR.DESCRIPTOR_MISMATCH",
                      "SBLR uint16 rejects a real-number shadow");

  sblr::SblrValue native_gate_numeric;
  native_gate_numeric.descriptor_id = "uint16";
  native_gate_numeric.payload_kind =
      sblr::SblrValuePayloadKind::unsigned_integer;
  native_gate_numeric.uint64_value = 42;
  native_gate_numeric.has_uint64_value = true;
  sblr::SblrValue native_gate_bytes;
  native_gate_bytes.descriptor_id = "uint16";
  native_gate_bytes.payload_kind =
      sblr::SblrValuePayloadKind::unsigned_integer;
  native_gate_bytes.binary_value = {0x2a, 0x00};
  sblr::SblrValue native_gate_null;
  native_gate_null.descriptor_id = "uint16";
  native_gate_null.is_null = true;
  for (const auto& value :
       {native_gate_numeric, native_gate_bytes, native_gate_null}) {
    const auto comparison = sblr::CompareSblrNativeBinaryValues(value, value);
    Check(sblr::SblrHasNativeBinaryCarrier(value) &&
              !sblr::SblrNativeCastCarrierValid(value) &&
              comparison.attempted && !comparison.valid &&
              comparison.order == 0,
          "SBLR native comparison dispatch refuses every uint16 carrier and state");
  }

  api::EngineProjectionFunctionArgument projected_text;
  // The registered projection profile accepts the actual Core-bound native
  // descriptor. Unbound legacy carriers below must still fail unchanged.
  for (const std::uint32_t number : {0u, 255u, 256u, 32768u, 65535u}) {
    api::EngineTypedValue source;
    source.descriptor = EngineUint16Descriptor(true);
    source.binary_value = {static_cast<std::uint8_t>(number & 255),
                           static_cast<std::uint8_t>(number >> 8)};
    for (bool is_null : {false, true}) {
      if (is_null) {source.binary_value.clear(); source.setState(api::EngineValueState::sql_null);}
      const auto argument = api::MakeProjectionFunctionArgument("value", source);
      const auto projected = sblr::SblrValueFromProjectionArgument(argument);
      Check(sblr::ProjectionArgumentEncodingValid(argument) &&
                sblr::ProjectionSblrValueResolved(projected),
            "uint16 projection profile matches the actual Core catalog descriptor");
      const auto output = sblr::EngineTypedValueFromSblrValue(projected);
      Check(output.descriptor == source.descriptor && output.binary_value == source.binary_value &&
                output.encoded_value.empty() && output.state == source.state && output.isSqlNull() == is_null,
            "Core-bound uint16 projection preserves exact identity, value, and NULL state");
    }
  }
  projected_text.type_name = "uint16";
  projected_text.encoded_value = "42";
  api::EngineProjectionFunctionArgument projected_binary;
  projected_binary.type_name = "uint16";
  projected_binary.binary_value = {0x2a, 0x00};
  api::EngineProjectionFunctionArgument projected_argument_null;
  projected_argument_null.type_name = "uint16";
  projected_argument_null.state = api::EngineValueState::sql_null;
  projected_argument_null.is_null = true;
  for (const auto& argument :
       {projected_text, projected_binary, projected_argument_null}) {
    const auto projected = sblr::SblrValueFromProjectionArgument(argument);
    Check(!sblr::ProjectionArgumentEncodingValid(argument) &&
              !sblr::ProjectionSblrValueResolved(projected) &&
              !projected.is_null &&
              projected.payload_kind == sblr::SblrValuePayloadKind::none,
          "projection ingress refuses every uint16 carrier and state");
  }

  for (const auto state : {sblr::SblrValuePayloadKind::text,
                           sblr::SblrValuePayloadKind::binary,
                           sblr::SblrValuePayloadKind::unsigned_integer}) {
    sblr::SblrValue projected;
    projected.descriptor_id = "uint16";
    projected.payload_kind = state;
    projected.encoded_value = state == sblr::SblrValuePayloadKind::text ? "42" : "";
    projected.binary_value = state == sblr::SblrValuePayloadKind::binary
        ? std::vector<std::uint8_t>{0x2a, 0x00}
        : std::vector<std::uint8_t>{};
    projected.uint64_value = 42;
    projected.has_uint64_value = state == sblr::SblrValuePayloadKind::unsigned_integer;
    bool rejected = false;
    try {
      (void)sblr::EngineTypedValueFromSblrValue(projected);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(!sblr::ProjectionSblrValueResolved(projected) && rejected,
          "projection egress refuses every present uint16 carrier");
  }
  sblr::SblrValue projected_null;
  projected_null.descriptor_id = "uint16";
  projected_null.is_null = true;
  bool rejected_projected_null = false;
  try {
    (void)sblr::EngineTypedValueFromSblrValue(projected_null);
  } catch (const std::invalid_argument&) {
    rejected_projected_null = true;
  }
  Check(!sblr::ProjectionSblrValueResolved(projected_null) &&
            rejected_projected_null,
        "projection egress refuses uint16 NULL without a bound profile");

  executor::DescriptorBatch present_sort;
  present_sort.columns.push_back(
      {"uint16_value", descriptor, false, descriptor_id});
  present_sort.rows.push_back({{native}});
  executor::DescriptorRuntimeDiagnostic sort_diagnostic;
  const auto rejected_present_sort = executor::SortDescriptorBatchByColumn(
      present_sort, 0, true, &sort_diagnostic);
  Check(rejected_present_sort.columns.empty() &&
            rejected_present_sort.rows.empty() && !sort_diagnostic.ok,
        "executor sort refuses present uint16 before comparing text carriers");

  executor::DescriptorBatch null_sort;
  null_sort.columns.push_back(
      {"uint16_value", nullable, true, descriptor_id});
  null_sort.rows.push_back({{sql_null}});
  sort_diagnostic = {};
  const auto rejected_null_sort = executor::SortDescriptorBatchByColumn(
      null_sort, 0, true, &sort_diagnostic);
  Check(rejected_null_sort.columns.empty() && rejected_null_sort.rows.empty() &&
            !sort_diagnostic.ok &&
            sort_diagnostic.diagnostic_code ==
                "QOW-DIAG-QRY-008-RUNTIME-BREADTH-REFUSAL-V1",
        "executor sort refuses uint16 NULL without an ordering policy");
}

template <std::size_t N>
void Append(std::vector<platform::byte>* output,
            const std::array<platform::byte, N>& bytes) {
  output->insert(output->end(), bytes.begin(), bytes.end());
}

void Persistence() {
  constexpr platform::Uuid descriptor_uuid{{
      0x79,0,0,0,0x75,0x69,0x7e,0x74,0xb1,0x36,0,0,0,0,0,0}};
  constexpr platform::Uuid type_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x06}};
  constexpr platform::Uuid codec_uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x07}};
  constexpr std::array<platform::byte,26> value_0{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x79,0,0,0,1,0,0,0,
      2,0,0,0,0xc7,0x70,0xee,0xb8,0,0}};
  constexpr std::array<platform::byte,26> value_255{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x79,0,0,0,1,0,0,0,
      2,0,0,0,0xd0,0x03,0x03,0x9e,0xff,0}};
  constexpr std::array<platform::byte,26> value_256{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x79,0,0,0,1,0,0,0,
      2,0,0,0,0x34,0x6f,0xee,0xb7,0,1}};
  constexpr std::array<platform::byte,26> value_32768{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x79,0,0,0,1,0,0,0,
      2,0,0,0,0x47,0x3a,0xef,0x38,0,0x80}};
  constexpr std::array<platform::byte,26> value_65535{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x79,0,0,0,1,0,0,0,
      2,0,0,0,0x9d,0xa1,0x01,0xbd,0xff,0xff}};
  constexpr std::array<platform::byte,24> sql_null{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x79,0,0,0,0,0,0,0,
      0,0,0,0,0x1c,0x4a,0x60,0xbb}};

  struct Oracle { std::uint32_t value; const platform::byte* bytes; std::size_t size; };
  const std::array<Oracle,5> oracles{{
      {0,value_0.data(),value_0.size()}, {255,value_255.data(),value_255.size()},
      {256,value_256.data(),value_256.size()},
      {32768,value_32768.data(),value_32768.size()},
      {65535,value_65535.data(),value_65535.size()}}};
  for (const auto& oracle : oracles) {
    const auto encoded = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::uint16, dt::DatatypePhysicalValueState::value,
         Payload(Uint16(oracle.value))});
    Check(encoded.ok() && encoded.bytes.size() == oracle.size &&
              std::equal(encoded.bytes.begin(), encoded.bytes.end(), oracle.bytes),
          "production uint16 physical frame matches independent hard-coded oracle");
  }
  const auto encoded_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::uint16,
       dt::DatatypePhysicalValueState::sql_null, {}});
  Check(encoded_null.ok() && encoded_null.bytes.size() == sql_null.size() &&
            std::equal(encoded_null.bytes.begin(), encoded_null.bytes.end(),
                       sql_null.begin()),
        "production uint16 NULL frame matches independent hard-coded oracle");

  constexpr std::size_t header_bytes = 120;
  std::vector<platform::byte> expected(header_bytes, 0);
  const std::array<platform::byte,8> magic{{'S','B','U','1','6','V','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(descriptor_uuid.bytes.begin(), descriptor_uuid.bytes.end(),
            expected.begin() + 8);
  std::copy(type_uuid.bytes.begin(), type_uuid.bytes.end(), expected.begin() + 24);
  std::copy(codec_uuid.bytes.begin(), codec_uuid.bytes.end(), expected.begin() + 40);
  platform::StoreLittle32(expected.data() + 56, 1);
  platform::StoreLittle32(expected.data() + 60, 1);
  platform::StoreLittle32(expected.data() + 64, 1);
  platform::StoreLittle32(expected.data() + 68, 6);
  std::uint32_t offset = header_bytes;
  for (unsigned index = 0; index < 6; ++index) {
    const std::uint32_t size = index == 5 ? 24 : 26;
    platform::StoreLittle32(expected.data() + 72 + index * 8, offset);
    platform::StoreLittle32(expected.data() + 76 + index * 8, size);
    offset += size;
  }
  Append(&expected, value_0);
  Append(&expected, value_255);
  Append(&expected, value_256);
  Append(&expected, value_32768);
  Append(&expected, value_65535);
  Append(&expected, sql_null);

#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-uint16-" + std::to_string(pid) + ".codec");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code error; fs::remove(path, error); }
  } cleanup{path};

  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(),
        "create uint16 persistence fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() &&
            writer.Sync().ok() && writer.Close().ok(),
        "write, sync, and close exact uint16 bytes");

  disk::FileDevice reader;
  Check(reader.Open(path.string(),
                    disk::FileOpenMode::open_existing_read_only).ok(),
        "reopen uint16 persistence fixture with an independent handle");
  std::vector<platform::byte> actual(expected.size());
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  bool decoded_all = read.ok() && actual == expected &&
      std::equal(actual.begin() + 8, actual.begin() + 24,
                 descriptor_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 24, actual.begin() + 40,
                 type_uuid.bytes.begin()) &&
      std::equal(actual.begin() + 40, actual.begin() + 56,
                 codec_uuid.bytes.begin()) &&
      platform::LoadLittle32(actual.data() + 56) == 1 &&
      platform::LoadLittle32(actual.data() + 60) == 1 &&
      platform::LoadLittle32(actual.data() + 64) == 1 &&
      platform::LoadLittle32(actual.data() + 68) == 6;
  for (unsigned index = 0; index < 6 && decoded_all; ++index) {
    const auto frame_offset = platform::LoadLittle32(actual.data() + 72 + index * 8);
    const auto frame_size = platform::LoadLittle32(actual.data() + 76 + index * 8);
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        actual.data() + frame_offset, frame_size);
    decoded_all = decoded.ok() &&
        decoded.value.type_id == dt::CanonicalTypeId::uint16 &&
        (index == 5
             ? decoded.value.state == dt::DatatypePhysicalValueState::sql_null &&
                   decoded.value.payload.empty()
             : decoded.value.state == dt::DatatypePhysicalValueState::value &&
                   decoded.value.payload == Payload(Uint16(oracles[index].value)));
  }
  Check(read.ok() && read.bytes_transferred == actual.size() && decoded_all,
        "independent reopen preserves exact identities and uint16 frames");

  std::array<platform::byte,2> short_bytes{};
  const auto short_read = reader.ReadAt(
      actual.size() - 1, short_bytes.data(), short_bytes.size());
  Check(!short_read.ok() && short_read.bytes_transferred < short_bytes.size(),
        "uint16 persistence boundary refuses a short read");
  const auto rejected_write = reader.WriteAt(0, magic.data(), 1);
  Check(!rejected_write.ok() && rejected_write.bytes_transferred == 0,
        "read-only uint16 persistence handle refuses writes");
  Check(reader.Close().ok(), "close reopened uint16 fixture");

  for (const auto& oracle : oracles) {
    std::vector<platform::byte> corrupt(oracle.bytes, oracle.bytes + oracle.size);
    corrupt[20] ^= 0x01;
    Check(!dt::DecodeDatatypePhysicalValue(corrupt.data(), corrupt.size()).ok() &&
              !dt::DecodeDatatypePhysicalValue(oracle.bytes,
                                                oracle.size - 1).ok(),
          "uint16 physical decoder rejects checksum corruption and truncation");
  }
  auto corrupt_null = sql_null;
  corrupt_null[20] ^= 0x01;
  Check(!dt::DecodeDatatypePhysicalValue(
             corrupt_null.data(), corrupt_null.size()).ok() &&
            !dt::DecodeDatatypePhysicalValue(
             sql_null.data(), sql_null.size() - 1).ok(),
        "uint16 NULL physical frame rejects checksum corruption and truncation");
}

}  // namespace

int main() {
  ExactIdentity();
  ExhaustiveRepresentationAndCodecs();
  NullAndAbsentPolicies();
  ConsumerCarrierBoundaries();
  Persistence();
  std::cout << "base uint16 checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
