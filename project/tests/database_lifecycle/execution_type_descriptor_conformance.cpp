// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_descriptor.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace engine = scratchbird::engine;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid TypedObjectUuid(std::uint8_t seed) {
  platform::TypedUuid uuid;
  uuid.kind = platform::UuidKind::object;
  for (std::size_t index = 0; index < uuid.value.bytes.size(); ++index) {
    uuid.value.bytes[index] = static_cast<platform::byte>(seed + index);
  }
  uuid.value.bytes[6] = static_cast<platform::byte>((uuid.value.bytes[6] & 0x0fu) | 0x70u);
  uuid.value.bytes[8] = static_cast<platform::byte>((uuid.value.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

bool EngineUuidIsNil(const engine::Uuid& uuid) {
  for (const std::uint8_t byte : uuid.bytes) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

bool EngineUuidEquals(const engine::Uuid& left, const platform::TypedUuid& right) {
  for (std::size_t index = 0; index < right.value.bytes.size(); ++index) {
    if (left.bytes[index] != right.value.bytes[index]) {
      return false;
    }
  }
  return true;
}

bool HasFlag(const engine::ExecutionTypeDescriptor& descriptor,
             engine::ExecutionTypeModifierFlag flag) {
  return (descriptor.modifier_flags & engine::ExecutionTypeModifierFlagBit(flag)) != 0;
}

dt::CatalogExecutionTypeMetadata Metadata(std::uint8_t seed) {
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = TypedObjectUuid(seed);
  metadata.descriptor_epoch = 7;
  return metadata;
}

void TestScalarDescriptorFromCatalogMetadata() {
  const auto descriptor = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::int64,
      Metadata(0x10));
  Require(descriptor.ok(), "EDR-001 int64 descriptor did not build");
  Require(descriptor.descriptor.parser_independent,
          "EDR-001 descriptor must be parser independent");
  Require(descriptor.descriptor.descriptor_epoch == 7,
          "EDR-001 descriptor epoch was not preserved");
  Require(descriptor.descriptor.canonical_type_id ==
              static_cast<std::uint32_t>(dt::CanonicalTypeId::int64),
          "EDR-001 canonical type id mismatch");
  Require(descriptor.descriptor.family == engine::ExecutionTypeFamily::signed_integer,
          "EDR-001 type family mismatch");
  Require(descriptor.descriptor.width_class == engine::ExecutionTypeWidthClass::fixed,
          "EDR-001 width class mismatch");
  Require(descriptor.descriptor.bit_width == 64,
          "EDR-001 bit width mismatch");
  Require(descriptor.descriptor.stable_name == "int64",
          "EDR-001 stable name mismatch");
  Require(EngineUuidEquals(descriptor.descriptor.descriptor_uuid, Metadata(0x10).descriptor_uuid),
          "EDR-001 descriptor uuid mismatch");
}

void TestTextDescriptorCarriesResourceIdentity() {
  auto metadata = Metadata(0x20);
  metadata.length = 128;
  metadata.charset_uuid = TypedObjectUuid(0x30);
  metadata.collation_uuid = TypedObjectUuid(0x40);
  metadata.security_policy_uuid = TypedObjectUuid(0x50);

  const auto descriptor = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::character,
      metadata);
  Require(descriptor.ok(), "EDR-001 character descriptor did not build");
  Require(descriptor.descriptor.family == engine::ExecutionTypeFamily::character,
          "EDR-001 character family mismatch");
  Require(descriptor.descriptor.length == 128,
          "EDR-001 character length was not preserved");
  Require(HasFlag(descriptor.descriptor, engine::ExecutionTypeModifierFlag::length),
          "EDR-001 length modifier flag missing");
  Require(HasFlag(descriptor.descriptor, engine::ExecutionTypeModifierFlag::charset_uuid),
          "EDR-001 charset uuid flag missing");
  Require(HasFlag(descriptor.descriptor, engine::ExecutionTypeModifierFlag::collation_uuid),
          "EDR-001 collation uuid flag missing");
  Require(HasFlag(descriptor.descriptor, engine::ExecutionTypeModifierFlag::security_policy_uuid),
          "EDR-001 security policy uuid flag missing");
  Require(EngineUuidEquals(descriptor.descriptor.charset_uuid, metadata.charset_uuid),
          "EDR-001 charset uuid mismatch");
  Require(EngineUuidEquals(descriptor.descriptor.collation_uuid, metadata.collation_uuid),
          "EDR-001 collation uuid mismatch");
  Require(EngineUuidEquals(descriptor.descriptor.security_policy_uuid, metadata.security_policy_uuid),
          "EDR-001 security uuid mismatch");
}

void TestDomainAndVectorDescriptorMetadata() {
  auto decimal_metadata = Metadata(0x60);
  decimal_metadata.precision = 34;
  decimal_metadata.scale = 8;
  decimal_metadata.domain_uuid = TypedObjectUuid(0x70);
  decimal_metadata.domain_stack.push_back(TypedObjectUuid(0x71));

  const auto decimal = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::decimal,
      decimal_metadata);
  Require(decimal.ok(), "EDR-001 decimal domain descriptor did not build");
  Require(decimal.descriptor.precision == 34,
          "EDR-001 decimal precision mismatch");
  Require(decimal.descriptor.scale == 8,
          "EDR-001 decimal scale mismatch");
  Require(decimal.descriptor.domain_stack.size() == 1,
          "EDR-001 domain stack missing");
  Require(HasFlag(decimal.descriptor, engine::ExecutionTypeModifierFlag::domain_uuid),
          "EDR-001 domain uuid flag missing");
  Require(HasFlag(decimal.descriptor, engine::ExecutionTypeModifierFlag::domain_stack),
          "EDR-001 domain stack flag missing");

  auto vector_metadata = Metadata(0x80);
  vector_metadata.vector_dimensions = 1536;
  vector_metadata.element_descriptor_uuid = TypedObjectUuid(0x90);

  const auto vector = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::dense_vector,
      vector_metadata);
  Require(vector.ok(), "EDR-001 vector descriptor did not build");
  Require(vector.descriptor.family == engine::ExecutionTypeFamily::vector,
          "EDR-001 vector family mismatch");
  Require(vector.descriptor.vector_dimensions == 1536,
          "EDR-001 vector dimensions mismatch");
  Require(HasFlag(vector.descriptor, engine::ExecutionTypeModifierFlag::vector_dimensions),
          "EDR-001 vector dimension flag missing");
  Require(HasFlag(vector.descriptor, engine::ExecutionTypeModifierFlag::element_descriptor_uuid),
          "EDR-001 element descriptor flag missing");
}

void TestFailClosedCatalogMetadataValidation() {
  auto missing_uuid = Metadata(0xa0);
  missing_uuid.descriptor_uuid = {};
  const auto uuid_result = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::boolean,
      missing_uuid);
  Require(!uuid_result.ok(),
          "EDR-001 accepted missing descriptor uuid");
  Require(uuid_result.diagnostic.diagnostic_code == "SB-EDR-DESCRIPTOR-MISSING-UUID",
          "EDR-001 missing uuid diagnostic mismatch");

  auto missing_epoch = Metadata(0xb0);
  missing_epoch.descriptor_epoch = 0;
  const auto epoch_result = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::boolean,
      missing_epoch);
  Require(!epoch_result.ok(),
          "EDR-001 accepted missing descriptor epoch");
  Require(epoch_result.diagnostic.diagnostic_code == "SB-EDR-DESCRIPTOR-MISSING-EPOCH",
          "EDR-001 missing epoch diagnostic mismatch");

  auto bad_stack = Metadata(0xc0);
  bad_stack.domain_stack.push_back({});
  const auto stack_result = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::decimal,
      bad_stack);
  Require(!stack_result.ok(),
          "EDR-001 accepted invalid domain stack uuid");
  Require(stack_result.diagnostic.diagnostic_code == "SB-EDR-DESCRIPTOR-BAD-DOMAIN-STACK",
          "EDR-001 bad domain stack diagnostic mismatch");

  const auto unknown = dt::LookupExecutionTypeDescriptorFromCatalog(
      dt::CanonicalTypeId::unknown,
      Metadata(0xd0));
  Require(!unknown.ok(),
          "EDR-001 accepted unknown canonical type");
  Require(EngineUuidIsNil(unknown.descriptor.descriptor_uuid),
          "EDR-001 failed descriptor should not expose a descriptor uuid");
}

}  // namespace

int main() {
  TestScalarDescriptorFromCatalogMetadata();
  TestTextDescriptorCarriesResourceIdentity();
  TestDomainAndVectorDescriptorMetadata();
  TestFailClosedCatalogMetadataValidation();
  return EXIT_SUCCESS;
}
