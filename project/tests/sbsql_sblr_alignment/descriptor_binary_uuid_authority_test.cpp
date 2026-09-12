// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_binary.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_physical_encoding.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <iterator>

namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
using Metadata = dt::CatalogExecutionTypeMetadata;
using platform::TypedUuid;
using platform::UuidKind;
static_assert(sizeof(scratchbird::engine::Uuid) == 16);
static_assert(sizeof(platform::Uuid) == 16);
int failures = 0;
int checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok && ++failures <= 12) std::cerr << "FAIL " << message << '\n';
}

constexpr std::array<TypedUuid Metadata::*, 7> fields = {
    &Metadata::descriptor_uuid, &Metadata::domain_uuid, &Metadata::charset_uuid,
    &Metadata::collation_uuid, &Metadata::timezone_uuid,
    &Metadata::element_descriptor_uuid, &Metadata::security_policy_uuid};

void Set(Metadata& metadata, unsigned slot, TypedUuid value) {
  if (slot < fields.size()) metadata.*fields[slot] = value;
  else metadata.domain_stack = {metadata.descriptor_uuid, value};
}

void RefusedWithoutAuthority(const dt::ExecutionTypeDescriptorResult& result) {
  Check(!result.ok(), "invalid binary UUID admitted as descriptor authority");
  Check(!result.diagnostic.diagnostic_code.empty(), "missing identity refusal diagnostic");
  const auto& bytes = result.descriptor.descriptor_uuid.bytes;
  Check(std::all_of(std::begin(bytes), std::end(bytes), [](auto b) { return b == 0; }) &&
        result.descriptor.domain_stack.empty() && result.descriptor.modifier_flags == 0,
        "refusal leaked partially assembled descriptor authority");
}

int main() {
  const auto millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto generated = uuid::GenerateEngineIdentityV7(UuidKind::object, millis);
  if (!generated.ok()) return 2;
  Metadata base;
  base.descriptor_uuid = generated.value;
  base.descriptor_epoch = 1;
  Check(dt::LookupExecutionTypeDescriptorFromCatalog(dt::CanonicalTypeId::uuid, base).ok(),
        "valid descriptor with absent optional fields refused");
  for (unsigned slot = 0; slot < 8; ++slot) {
    for (unsigned version = 0; version < 16; ++version) {
      for (unsigned variant = 0; variant < 4; ++variant) {
        auto value = generated.value;
        value.value.bytes[6] = static_cast<platform::byte>(
            (value.value.bytes[6] & 15) | (version << 4));
        value.value.bytes[8] = static_cast<platform::byte>(
            (value.value.bytes[8] & 63) | (variant << 6));
        auto metadata = base;
        Set(metadata, slot, value);
        const auto result = dt::LookupExecutionTypeDescriptorFromCatalog(
            dt::CanonicalTypeId::uuid, metadata);
        if (version == 7 && variant == 2) Check(result.ok(), "valid binary v7 reference refused");
        else RefusedWithoutAuthority(result);
      }
    }
    for (unsigned invalid = 0; invalid < 4; ++invalid) {
      auto metadata = base;
      auto value = generated.value;
      if (invalid == 0) value.value = {};
      if (invalid == 1) value.kind = UuidKind::unknown;
      if (invalid == 2) value.kind = static_cast<UuidKind>(255);
      if (invalid == 3) value.kind = UuidKind::session;
      Set(metadata, slot, value);
      RefusedWithoutAuthority(dt::LookupExecutionTypeDescriptorFromCatalog(
          dt::CanonicalTypeId::uuid, metadata));
    }
  }
  Metadata all = base;
  for (unsigned slot = 1; slot < 8; ++slot) {
    const auto reference = uuid::GenerateEngineIdentityV7(UuidKind::object, millis);
    if (!reference.ok()) return 2;
    Set(all, slot, reference.value);
  }
  const auto attached = dt::LookupExecutionTypeDescriptorFromCatalog(dt::CanonicalTypeId::uuid, all);
  Check(attached.ok(), "all valid binary reference fields refused");
  const auto& d = attached.descriptor;
  unsigned slot = 0;
  for (const auto* reference : {&d.descriptor_uuid, &d.domain_uuid, &d.charset_uuid,
                               &d.collation_uuid, &d.timezone_uuid,
                               &d.element_descriptor_uuid, &d.security_policy_uuid}) {
    Check(std::equal(std::begin(reference->bytes), std::end(reference->bytes),
                     (all.*fields[slot++]).value.bytes.begin()), "binary reference bytes changed");
  }
  Check(d.domain_stack.size() == 2, "valid domain stack lost");
  slot = 0;
  for (const auto& reference : d.domain_stack)
    Check(std::equal(std::begin(reference.bytes), std::end(reference.bytes),
                     all.domain_stack[slot++].value.bytes.begin()), "domain reference bytes changed");

  // User UUID payload is data, not catalog reference authority. Test the real
  // binary/physical codecs, without a UUID parser or text representation.
  for (unsigned version = 1; version <= 7; ++version) {
    auto bytes = generated.value.value.bytes;
    bytes[6] = static_cast<platform::byte>((bytes[6] & 15) | (version << 4));
    dt::DatatypeBinaryValue value;
    value.type_id = dt::CanonicalTypeId::uuid;
    value.payload.assign(bytes.begin(), bytes.end());
    const auto encoded = dt::EncodeDatatypeBinaryValue(value);
    Check(encoded.ok(), "user UUID binary encoding refused");
    const auto decoded = dt::DecodeDatatypeBinaryValue(encoded.encoded);
    Check(decoded.ok() && decoded.value.type_id == value.type_id &&
          decoded.value.payload == value.payload, "user UUID binary roundtrip changed bytes");
    dt::DatatypePhysicalValue physical;
    physical.type_id = value.type_id;
    physical.state = dt::DatatypePhysicalValueState::value;
    physical.payload = value.payload;
    const auto stored = dt::EncodeDatatypePhysicalValue(physical);
    Check(stored.ok(), "user UUID physical encoding refused");
    const auto restored = dt::DecodeDatatypePhysicalValue(stored.bytes.data(), stored.bytes.size());
    Check(restored.ok() && restored.value.type_id == physical.type_id &&
          restored.value.payload == physical.payload, "user UUID physical roundtrip changed bytes");
    for (const auto size : {15u, 17u}) {
      value.payload.resize(size);
      physical.payload.resize(size);
      Check(!dt::EncodeDatatypeBinaryValue(value).ok(), "non-binary16 user UUID accepted");
      Check(!dt::EncodeDatatypePhysicalValue(physical).ok(), "non-binary16 physical UUID accepted");
    }
  }
  std::cout << "checks=" << checks << " failures=" << failures
            << " reference_cases=544 user_versions=7\n";
  return failures ? 1 : 0;
}
