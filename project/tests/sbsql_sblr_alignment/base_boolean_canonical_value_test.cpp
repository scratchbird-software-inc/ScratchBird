// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_binary.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"

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
namespace platform = scratchbird::core::platform;
namespace fs = std::filesystem;

namespace {
// This is representation evidence plus an inventory of current cast/ordering
// behavior. Cast spellings, ordering admission, and display policy remain
// non-acceptance observations until their missing Core policy rows exist.
unsigned checks = 0, failures = 0;
void Check(bool ok, const std::string& why) {
  ++checks;
  if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}

engine::ExecutionTypeDescriptor Descriptor(dt::CanonicalTypeId type) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto row = manifest.ok()
      ? dt::LookupDatatypeCatalogRow(manifest.manifest, type)
      : dt::DatatypeCatalogManifestResult{};
  Check(row.ok() && row.manifest.descriptor_rows.size() == 1,
        "unique catalog descriptor row");
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows[0].descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows[0].descriptor_epoch;
  const auto built = dt::LookupExecutionTypeDescriptorFromCatalog(type, metadata);
  Check(built.ok(), "execution descriptor from exact catalog row");
  return built.descriptor;
}

dt::DatatypeOperationValue Boolean(bool bit) {
  return {dt::CanonicalTypeId::boolean,
          std::string(1, static_cast<char>(bit ? 1 : 0)), false};
}
bool IsBoolean(const dt::DatatypeOperationValue& value, bool bit) {
  return value.type_id == dt::CanonicalTypeId::boolean && !value.is_null &&
      value.encoded_value.size() == 1 &&
      static_cast<unsigned char>(value.encoded_value[0]) == (bit ? 1u : 0u);
}

std::uint32_t PhysicalChecksum(std::uint8_t payload) {
  std::uint32_t checksum = 2166136261u;
  for (const std::uint32_t value : {1u, 1u,
                                    static_cast<std::uint32_t>(payload)}) {
    checksum ^= value;
    checksum *= 16777619u;
  }
  return checksum;
}

void IdentityAndCodecs() {
  constexpr platform::Uuid uuid{{0x01,0,0,0,0x62,0x6f,0x7f,0x6c,
                                 0xa5,0x61,0x6e,0,0,0,0,0}};
  const auto descriptor = Descriptor(dt::CanonicalTypeId::boolean);
  Check(std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                   std::end(descriptor.descriptor_uuid.bytes), uuid.bytes.begin()) &&
            descriptor.descriptor_epoch == 1 && descriptor.stable_name == "boolean" &&
            descriptor.bit_width == 1,
        "exact Boolean descriptor identity");
  constexpr platform::Uuid snapshot{{0x01,0x9d,0,0,0,0,0x70,0,
                                     0x80,0,0,0,0,0,0xd7,0x01}};
  const auto lookup = dt::LookupCanonicalBooleanTypeCodecIdentityV1(
      snapshot, 1, 1);
  const auto* found = lookup.ok ? &lookup.row : nullptr;
  Check(found && found->descriptor_uuid == uuid && found->type_uuid == uuid &&
            found->descriptor_generation == 1 && found->type_generation == 1 &&
            found->codec_id == "datatype.boolean.u8.v1" &&
            found->codec_version == 1 && found->codec_generation == 1 &&
            found->canonical_value_minimum_bytes == 1 &&
            found->canonical_value_maximum_bytes == 1 &&
            found->canonical_value_exact_bytes == 1 && found->null_supported &&
            found->null_encoding_code == 1,
        "exact Boolean type-codec tuple");
  if (found) {
    Check(dt::IsExactCanonicalBooleanDescriptorTypeAliasV1(
              found->descriptor_uuid, found->descriptor_generation,
              found->type_uuid, found->type_generation, found->codec_id,
              found->codec_version, found->codec_generation, true),
          "exact Boolean descriptor/type UUID alias is admitted");
    Check(!dt::IsExactCanonicalBooleanDescriptorTypeAliasV1(
              found->descriptor_uuid, found->descriptor_generation,
              found->type_uuid, found->type_generation, found->codec_id,
              found->codec_version, found->codec_generation, false) &&
              !dt::IsExactCanonicalBooleanDescriptorTypeAliasV1(
                  found->descriptor_uuid, found->descriptor_generation + 1,
                  found->type_uuid, found->type_generation, found->codec_id,
                  found->codec_version, found->codec_generation, true),
          "Boolean UUID alias rejects missing nullability and mutated generation");
  }

  constexpr std::array<platform::byte,25> false_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x01,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0xd5,0xbe,0xcc,0x31,0x00}};
  constexpr std::array<platform::byte,25> true_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x01,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0x42,0xbd,0xcc,0x30,0x01}};
  constexpr std::array<platform::byte,24> null_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x01,0,0,0,0,0,0,0,
      0,0,0,0,0x64,0x1d,0x74,0xeb}};
  constexpr std::array<platform::byte,25> invalid_two_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x01,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0xaf,0xbb,0xcc,0x2f,0x02}};
  for (platform::byte bit : {platform::byte{0}, platform::byte{1}}) {
    const auto binary = dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::boolean, false, false, {bit}});
    const auto binary_back = binary.ok()
        ? dt::DecodeDatatypeBinaryValue(binary.encoded) : dt::DatatypeBinaryResult{};
    Check(binary.ok() && binary_back.ok() && binary_back.value.payload ==
              std::vector<platform::byte>{bit}, "binary 00/01 round trip");
    const auto physical = dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::boolean, dt::DatatypePhysicalValueState::value, {bit}});
    const auto physical_back = physical.ok()
        ? dt::DecodeDatatypePhysicalValue(physical.bytes.data(), physical.bytes.size())
        : dt::DatatypePhysicalEncodingResult{};
    const bool exact_oracle = bit == 0
        ? std::equal(physical.bytes.begin(), physical.bytes.end(), false_oracle.begin(), false_oracle.end())
        : std::equal(physical.bytes.begin(), physical.bytes.end(), true_oracle.begin(), true_oracle.end());
    Check(physical.ok() && physical_back.ok() && physical.bytes.size() == 25 &&
              exact_oracle && physical_back.value.payload ==
              std::vector<platform::byte>{bit}, "physical 00/01 exact oracle round trip");
  }
  for (const auto& bad : {std::vector<platform::byte>{},
                          std::vector<platform::byte>{2},
                          std::vector<platform::byte>{0,1}}) {
    Check(!dt::EncodeDatatypeBinaryValue(
        {dt::CanonicalTypeId::boolean, false, false, bad}).ok(),
        "binary rejects noncanonical Boolean payload");
    Check(!dt::EncodeDatatypePhysicalValue(
        {dt::CanonicalTypeId::boolean, dt::DatatypePhysicalValueState::value, bad}).ok(),
        "physical rejects noncanonical Boolean payload");
  }
  Check(dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::boolean, true, false, {}}).ok(),
      "binary typed NULL uses zero payload");
  Check(!dt::EncodeDatatypeBinaryValue(
      {dt::CanonicalTypeId::boolean, true, false, {0}}).ok(),
      "binary rejects payload-bearing typed NULL");
  const auto physical_null = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::boolean, dt::DatatypePhysicalValueState::sql_null, {}});
  Check(physical_null.ok() &&
            std::equal(physical_null.bytes.begin(), physical_null.bytes.end(),
                       null_oracle.begin(), null_oracle.end()),
        "physical SQL NULL equals independent zero-payload oracle");
  const auto invalid_decode = dt::DecodeDatatypePhysicalValue(
      invalid_two_oracle.data(), invalid_two_oracle.size());
  Check(!invalid_decode.ok() && invalid_decode.value.payload.empty(),
        "checksum-valid physical byte 02 is rejected on decode");
  auto exhaustive = true_oracle;
  for (unsigned candidate = 0; candidate != 256; ++candidate) {
    exhaustive[24] = static_cast<platform::byte>(candidate);
    platform::StoreLittle32(exhaustive.data() + 20,
                            PhysicalChecksum(candidate));
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        exhaustive.data(), exhaustive.size());
    const bool should_accept = candidate == 0 || candidate == 1;
    if (decoded.ok() != should_accept ||
        (decoded.ok() && (decoded.value.payload.size() != 1 ||
                          decoded.value.payload[0] != candidate))) {
      exhaustive = false_oracle;
      Check(false, "physical decoder finite byte partition");
      break;
    }
  }
  Check(exhaustive != false_oracle,
        "physical decoder admits exactly 00 and 01 among all 256 bytes");
  const auto layout = dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::boolean);
  Check(layout.ok() && dt::SampleDatatypePhysicalValueForLayout(layout.layout).payload ==
            std::vector<platform::byte>{0}, "physical sample is canonical FALSE");
}

dt::DatatypeCastResult Cast(dt::DatatypeOperationValue value,
                            dt::CanonicalTypeId target,
                            dt::DatatypeCastContext context) {
  dt::DatatypeCastRequest request;
  request.value = std::move(value); request.target_type_id = target;
  request.context = context; request.target_descriptor = Descriptor(target);
  return dt::CastDatatypeValue(request);
}

void OperationsAndSerialization() {
  for (auto context : {dt::DatatypeCastContext::implicit,
                       dt::DatatypeCastContext::assignment,
                       dt::DatatypeCastContext::explicit_cast}) {
    for (bool bit : {false, true}) {
      const auto out = Cast(Boolean(bit), dt::CanonicalTypeId::boolean, context);
      Check(out.ok() && out.category == dt::DatatypeCastCategory::identity &&
                IsBoolean(out.value, bit), "identity cast keeps canonical byte");
    }
  }
  for (const auto& [text, bit] :
       {std::pair{"true", true}, {"TRUE", true}, {"1", true},
        {"false", false}, {"FALSE", false}, {"0", false}}) {
    const auto out = Cast({dt::CanonicalTypeId::character, text, false},
                          dt::CanonicalTypeId::boolean,
                          dt::DatatypeCastContext::explicit_cast);
    Check(out.ok() && IsBoolean(out.value, bit),
          "existing explicit text cast emits canonical byte");
  }
  Check(!Cast({dt::CanonicalTypeId::character, "maybe", false},
              dt::CanonicalTypeId::boolean,
              dt::DatatypeCastContext::explicit_cast).ok(), "invalid text rejected");
  Check(!Cast({dt::CanonicalTypeId::int32, "2", false},
              dt::CanonicalTypeId::boolean,
              dt::DatatypeCastContext::explicit_cast).ok(), "integer 2 rejected");
  Check(!Cast({dt::CanonicalTypeId::boolean, std::string(1, '\2'), false},
              dt::CanonicalTypeId::boolean,
              dt::DatatypeCastContext::explicit_cast).ok(), "invalid source byte rejected");
  const auto text = Cast(Boolean(true), dt::CanonicalTypeId::character,
                         dt::DatatypeCastContext::explicit_cast);
  Check(text.ok() && text.value.encoded_value == "TRUE", "display cast is uppercase TRUE");
  const auto comparison = dt::CompareDatatypeValues({Boolean(false), Boolean(true)});
  const auto false_key = dt::MakeDatatypeSortKey({Boolean(false)});
  const auto true_key = dt::MakeDatatypeSortKey({Boolean(true)});
  const auto false_hash = dt::HashDatatypeValue({Boolean(false)});
  const auto true_hash = dt::HashDatatypeValue({Boolean(true)});
  Check(comparison.ok() && comparison.comparison < 0, "current core orders FALSE before TRUE");
  Check(false_key.ok() && true_key.ok() && false_key.sort_key < true_key.sort_key,
        "current core sort keys preserve Boolean order");
  Check(false_hash.ok() && true_hash.ok() &&
            false_hash.stable_hash_hex != true_hash.stable_hash_hex,
        "current core hashes distinguish Boolean values");
  Check(dt::RenderDatatypeValueForDisplay({Boolean(false)}).display_value == "FALSE",
        "display boundary renders FALSE");

  const auto descriptor = Descriptor(dt::CanonicalTypeId::boolean);
  for (bool bit : {false, true}) {
    auto value = Boolean(bit); value.descriptor = descriptor;
    const auto encoded = dt::SerializeDatatypeValue({value});
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::boolean;
    request.expected_descriptor = descriptor; request.serialized_value = encoded.serialized_value;
    const auto decoded = dt::DeserializeDatatypeValue(request);
    Check(encoded.ok() && encoded.serialized_value ==
              std::string("SBDV1;type=boolean;state=value;payload=") + (bit ? "01" : "00") &&
              decoded.ok() && IsBoolean(decoded.value, bit),
          "serialized Boolean is exact 00/01");
  }
  dt::DatatypeOperationValue null{dt::CanonicalTypeId::boolean, {}, true, descriptor};
  const auto encoded_null = dt::SerializeDatatypeValue({null});
  Check(encoded_null.ok() && encoded_null.serialized_value ==
            "SBDV1;type=boolean;state=null;payload=", "serialized NULL has zero payload");
  for (const char* bad : {"SBDV1;type=boolean;state=value;payload=02",
                          "SBDV1;type=boolean;state=value;payload=74727565",
                          "SBDV1;type=boolean;state=null;payload=00"}) {
    dt::DatatypeDeserializationRequest request;
    request.expected_type_id = dt::CanonicalTypeId::boolean;
    request.expected_descriptor = descriptor; request.serialized_value = bad;
    Check(!dt::DeserializeDatatypeValue(request).ok(), "malformed serialized Boolean rejected");
  }
}

void Persistence() {
  const auto descriptor = Descriptor(dt::CanonicalTypeId::boolean);
  const auto physical = dt::EncodeDatatypePhysicalValue(
      {dt::CanonicalTypeId::boolean, dt::DatatypePhysicalValueState::value, {1}});
  Check(physical.ok(), "production physical TRUE encoding"); if (!physical.ok()) return;
  std::array<platform::byte, 80> expected{};
  const std::array<platform::byte,8> magic{{'S','B','B','O','O','L','0','1'}};
  std::copy(magic.begin(), magic.end(), expected.begin());
  std::copy(std::begin(descriptor.descriptor_uuid.bytes),
            std::end(descriptor.descriptor_uuid.bytes), expected.begin()+8);
  platform::StoreLittle64(expected.data()+24, descriptor.descriptor_epoch);
  platform::StoreLittle32(expected.data()+32, descriptor.canonical_type_id);
  platform::StoreLittle32(expected.data()+36, physical.bytes.size());
  constexpr std::array<platform::byte,25> true_oracle{{
      0x53,0x42,0x44,0x50,0x56,0x30,0x30,0x31,0x01,0,0,0,0x01,0,0,0,
      0x01,0,0,0,0x42,0xbd,0xcc,0x30,0x01}};
  Check(physical.bytes.size() == true_oracle.size() &&
            std::equal(physical.bytes.begin(), physical.bytes.end(),
                       true_oracle.begin()),
        "production TRUE bytes equal independent oracle before persistence");
  std::copy(true_oracle.begin(), true_oracle.end(), expected.begin()+40);
#ifdef _WIN32
  const auto pid = ::_getpid();
#else
  const auto pid = ::getpid();
#endif
  const fs::path path = fs::temp_directory_path() /
      ("sb-base-boolean-" + std::to_string(pid) + ".codec");
  struct Cleanup { fs::path p; ~Cleanup(){ std::error_code e; fs::remove(p,e); } } cleanup{path};
  disk::FileDevice writer;
  Check(writer.Open(path.string(), disk::FileOpenMode::create_new).ok(), "create fixture");
  const auto write = writer.WriteAt(0, expected.data(), expected.size());
  Check(write.ok() && write.bytes_transferred == expected.size() && writer.Sync().ok(),
        "write and sync exact descriptor plus physical bytes");
  Check(writer.Close().ok(), "close fixture");
  disk::FileDevice reader;
  Check(reader.Open(path.string(), disk::FileOpenMode::open_existing_read_only).ok(), "reopen fixture");
  std::array<platform::byte,80> actual{};
  const auto read = reader.ReadAt(0, actual.data(), actual.size());
  const auto size = platform::LoadLittle32(actual.data()+36);
  const auto decoded = size <= 40
      ? dt::DecodeDatatypePhysicalValue(actual.data()+40, size)
      : dt::DatatypePhysicalEncodingResult{};
  Check(read.ok() && actual == expected &&
            std::equal(std::begin(descriptor.descriptor_uuid.bytes),
                       std::end(descriptor.descriptor_uuid.bytes), actual.begin()+8) &&
            decoded.ok() && decoded.value.payload == std::vector<platform::byte>{1},
        "reopen preserves exact identity and canonical TRUE byte");
  Check(reader.Close().ok(), "close reopened fixture");
}
}  // namespace

int main() {
  IdentityAndCodecs(); OperationsAndSerialization(); Persistence();
  std::cout << "base Boolean checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
