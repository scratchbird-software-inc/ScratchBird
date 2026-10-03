// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_binary.hpp"
#include "datatype_bit_string.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_layout.hpp"
#include "datatype_operations.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"
#include "sbl_numeric.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dt = scratchbird::core::datatypes;
namespace disk = scratchbird::storage::disk;
namespace engine = scratchbird::engine;
namespace platform = scratchbird::core::platform;
namespace fs = std::filesystem;
namespace numeric_backend = scratchbird::libraries::sbl_numeric;

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const std::string& message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

bool IsCanonicalBoolean(const dt::DatatypeOperationValue& value,
                        bool expected) {
  return value.type_id == dt::CanonicalTypeId::boolean && !value.is_null &&
         value.encoded_value.size() == 1 &&
         static_cast<unsigned char>(value.encoded_value[0]) ==
             (expected ? 1u : 0u);
}

template <typename Result>
void CheckRejectedAs(const Result& result, const std::string& code,
                     const std::string& message) {
  Check(!result.ok(), message + " was admitted");
  Check(result.diagnostic.diagnostic_code == code,
        message + " reported " + result.diagnostic.diagnostic_code +
            " instead of " + code);
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

platform::TypedUuid TypedObjectUuid(std::uint8_t seed) {
  platform::TypedUuid uuid;
  uuid.kind = platform::UuidKind::object;
  for (std::size_t index = 0; index < uuid.value.bytes.size(); ++index) {
    uuid.value.bytes[index] =
        static_cast<platform::byte>(seed + static_cast<std::uint8_t>(index));
  }
  uuid.value.bytes[6] =
      static_cast<platform::byte>((uuid.value.bytes[6] & 0x0fu) | 0x70u);
  uuid.value.bytes[8] =
      static_cast<platform::byte>((uuid.value.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

engine::ExecutionTypeDescriptor DescriptorFor(
    dt::CanonicalTypeId type_id,
    std::uint8_t seed,
    bool full_metadata = false) {
  dt::CatalogExecutionTypeMetadata metadata;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Check(manifest.ok(), "load current datatype catalog authority");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  Check(row.ok() && row.manifest.descriptor_rows.size() == 1,
        std::string("lookup current catalog descriptor for ") +
            dt::CanonicalTypeName(type_id));
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) {
    return {};
  }
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch =
      row.manifest.descriptor_rows.front().descriptor_epoch;
  if (full_metadata) {
    if (type_id == dt::CanonicalTypeId::decimal ||
        type_id == dt::CanonicalTypeId::decimal_float) {
      metadata.precision = 34;
      metadata.scale = 8;
    }
    metadata.domain_uuid = TypedObjectUuid(seed + 0x10u);
    metadata.domain_stack = {metadata.domain_uuid,
                             TypedObjectUuid(seed + 0x12u)};
    metadata.security_policy_uuid = TypedObjectUuid(seed + 0x24u);
  }
  const auto built =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  Check(built.ok(), std::string("build execution descriptor for ") +
                        dt::CanonicalTypeName(type_id));
  return built.descriptor;
}

dt::DatatypeOperationValue DecimalValue(
    std::string_view lexical,
    const engine::ExecutionTypeDescriptor& descriptor) {
  const auto encoded =
      numeric_backend::EncodeExactDecimalLittleEndian(lexical);
  Check(encoded.ok, "exact decimal fixture encoding");
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::decimal,
      std::string(reinterpret_cast<const char*>(encoded.canonical_bytes.data()),
                  encoded.canonical_bytes.size()),
      false};
  value.descriptor = descriptor;
  return value;
}

bool EngineUuidEquals(const engine::Uuid& left,
                      const engine::Uuid& right) {
  return std::equal(std::begin(left.bytes), std::end(left.bytes),
                    std::begin(right.bytes), std::end(right.bytes));
}

bool DescriptorEquals(const engine::ExecutionTypeDescriptor& left,
                      const engine::ExecutionTypeDescriptor& right) {
  if (!EngineUuidEquals(left.descriptor_uuid, right.descriptor_uuid) ||
      left.descriptor_epoch != right.descriptor_epoch ||
      left.canonical_type_id != right.canonical_type_id ||
      left.family != right.family || left.width_class != right.width_class ||
      left.stable_name != right.stable_name ||
      left.bit_width != right.bit_width ||
      left.precision != right.precision || left.scale != right.scale ||
      left.length != right.length ||
      left.vector_dimensions != right.vector_dimensions ||
      left.container_rank != right.container_rank ||
      left.modifier_flags != right.modifier_flags ||
      !EngineUuidEquals(left.domain_uuid, right.domain_uuid) ||
      left.domain_stack.size() != right.domain_stack.size() ||
      !EngineUuidEquals(left.charset_uuid, right.charset_uuid) ||
      !EngineUuidEquals(left.collation_uuid, right.collation_uuid) ||
      !EngineUuidEquals(left.timezone_uuid, right.timezone_uuid) ||
      !EngineUuidEquals(left.element_descriptor_uuid,
                        right.element_descriptor_uuid) ||
      !EngineUuidEquals(left.security_policy_uuid,
                        right.security_policy_uuid) ||
      left.nullable_allowed != right.nullable_allowed ||
      left.descriptor_authoritative != right.descriptor_authoritative ||
      left.parser_independent != right.parser_independent) {
    return false;
  }
  for (std::size_t index = 0; index < left.domain_stack.size(); ++index) {
    if (!EngineUuidEquals(left.domain_stack[index], right.domain_stack[index])) {
      return false;
    }
  }
  return true;
}

dt::DatatypeOperationValue TypedNull(
    dt::CanonicalTypeId type_id,
    const engine::ExecutionTypeDescriptor& descriptor) {
  dt::DatatypeOperationValue value{type_id, {}, true};
  value.descriptor = descriptor;
  return value;
}

dt::DatatypeOperationValue PresentInt64(
    std::int64_t number,
    const engine::ExecutionTypeDescriptor& descriptor = {}) {
  std::string encoded;
  Check(dt::EncodeCanonicalInt64Value(number, &encoded),
        "encode canonical int64 fixture");
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::int64, std::move(encoded), false};
  value.descriptor = descriptor;
  return value;
}

void StandaloneNullIsNotADatatype() {
  const auto& descriptors = dt::BuiltinDatatypeDescriptors();
  Check(std::none_of(descriptors.begin(), descriptors.end(), [](const auto& descriptor) {
          return descriptor.type_id == dt::CanonicalTypeId::null_type;
        }),
        "builtin descriptor registry advertises standalone null_type");
  Check(!dt::LookupDatatypeDescriptor(dt::CanonicalTypeId::null_type).ok(),
        "standalone null_type descriptor lookup succeeded");

  const auto& layouts = dt::BuiltinDatatypeStorageLayouts();
  Check(std::none_of(layouts.begin(), layouts.end(), [](const auto& layout) {
          return layout.type_id == dt::CanonicalTypeId::null_type;
        }),
        "storage layout registry advertises standalone null_type");
  Check(!dt::LookupDatatypeStorageLayout(dt::CanonicalTypeId::null_type).ok(),
        "standalone null_type storage layout lookup succeeded");
  Check(dt::CanonicalTypeIdFromStableName("null") == dt::CanonicalTypeId::unknown,
        "NULL resolved as a datatype name");
  Check(dt::CanonicalTypeIdFromStableName("null_type") == dt::CanonicalTypeId::unknown,
        "null_type resolved as a datatype name");
}

void ContextualBindingPreservesTargetType() {
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    const auto target_descriptor = DescriptorFor(descriptor.type_id, 0x10u);
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      dt::DatatypeCastRequest bind;
      bind.value = {dt::CanonicalTypeId::null_type, {}, true};
      bind.target_type_id = descriptor.type_id;
      bind.context = context;
      bind.target_descriptor = target_descriptor;
      const auto bound = dt::CastDatatypeValue(bind);
      const std::string label = std::string(descriptor.stable_name) + "/" +
                                dt::DatatypeCastContextName(context);
      if (descriptor.type_id == dt::CanonicalTypeId::bit_string) {
        CheckRejectedAs(bound, "CTB.BIT.DESCRIPTOR_INVALID",
                        "generic contextual bit-string NULL without V3 profile");
        dt::DatatypeCastRequest identity;
        identity.value = TypedNull(descriptor.type_id, target_descriptor);
        identity.target_type_id = descriptor.type_id;
        identity.context = context;
        identity.target_descriptor = target_descriptor;
        CheckRejectedAs(dt::CastDatatypeValue(identity),
                        "CTB.BIT.DESCRIPTOR_INVALID",
                        "generic typed bit-string NULL identity without V3 profile");
        continue;
      }
      Check(bound.ok(), "contextual NULL did not bind in " + label);
      Check(bound.value.type_id == descriptor.type_id,
            "contextual NULL lost target descriptor in " + label);
      Check(bound.value.is_null && bound.value.encoded_value.empty(),
            "contextual NULL produced a payload/present state in " + label);
      Check(DescriptorEquals(bound.value.descriptor, target_descriptor),
            "contextual NULL lost its exact target descriptor in " + label);

      dt::DatatypeCastRequest identity;
      identity.value = TypedNull(descriptor.type_id, target_descriptor);
      identity.target_type_id = descriptor.type_id;
      identity.context = context;
      identity.target_descriptor = target_descriptor;
      const auto identity_result = dt::CastDatatypeValue(identity);
      Check(identity_result.ok() &&
                identity_result.category == dt::DatatypeCastCategory::identity,
            "typed NULL identity cast failed in " + label);
      Check(identity_result.value.type_id == descriptor.type_id &&
                identity_result.value.is_null &&
                identity_result.value.encoded_value.empty(),
            "typed NULL identity cast lost canonical type/state in " + label);
      Check(DescriptorEquals(identity_result.value.descriptor,
                             target_descriptor),
            "typed NULL identity cast lost its exact descriptor in " + label);
    }
  }

  for (const dt::DatatypeOperationValue malformed : {
           dt::DatatypeOperationValue{dt::CanonicalTypeId::null_type, "payload", true},
           dt::DatatypeOperationValue{dt::CanonicalTypeId::null_type, {}, false}}) {
    dt::DatatypeCastRequest bind;
    bind.value = malformed;
    bind.target_type_id = dt::CanonicalTypeId::int64;
    bind.target_descriptor =
        DescriptorFor(dt::CanonicalTypeId::int64, 0x2eu);
    CheckRejectedAs(dt::CastDatatypeValue(bind), "DATATYPE.NULL_STATE.INVALID",
                    "malformed contextual NULL");
  }

  dt::DatatypeCastRequest malformed_without_target_descriptor;
  malformed_without_target_descriptor.value =
      {dt::CanonicalTypeId::null_type, "payload", true};
  malformed_without_target_descriptor.target_type_id =
      dt::CanonicalTypeId::int64;
  CheckRejectedAs(dt::CastDatatypeValue(malformed_without_target_descriptor),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "missing target descriptor before malformed NULL state");

  dt::DatatypeCastRequest unresolved;
  unresolved.value = {dt::CanonicalTypeId::null_type, {}, true};
  unresolved.target_type_id = dt::CanonicalTypeId::null_type;
  CheckRejectedAs(dt::CastDatatypeValue(unresolved), "DATATYPE.CONTEXT_REQUIRED",
                  "unresolved NULL-to-NULL binding");

  unresolved.value = {dt::CanonicalTypeId::null_type, "payload", true};
  CheckRejectedAs(dt::CastDatatypeValue(unresolved),
                  "DATATYPE.NULL_STATE.INVALID",
                  "payload-bearing unresolved NULL-to-NULL binding");

  dt::DatatypeCastRequest sentinel_with_descriptor;
  sentinel_with_descriptor.value =
      {dt::CanonicalTypeId::null_type, {}, true};
  sentinel_with_descriptor.value.descriptor.modifier_flags = 1;
  sentinel_with_descriptor.target_type_id = dt::CanonicalTypeId::int64;
  sentinel_with_descriptor.target_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x2fu);
  CheckRejectedAs(dt::CastDatatypeValue(sentinel_with_descriptor),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "contextual NULL sentinel carrying descriptor metadata");

  dt::DatatypeCastRequest missing_descriptor;
  missing_descriptor.value = {dt::CanonicalTypeId::unknown, {}, true};
  missing_descriptor.target_type_id = dt::CanonicalTypeId::int64;
  CheckRejectedAs(dt::CastDatatypeValue(missing_descriptor),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "typed NULL with unknown source descriptor");

  const auto invalid_type = static_cast<dt::CanonicalTypeId>(0xfffffffeu);
  Check(dt::ClassifyDatatypeCast(dt::CanonicalTypeId::null_type,
                                 invalid_type, true) ==
            dt::DatatypeCastCategory::forbidden,
        "contextual NULL classifier admitted an out-of-range target type");
  Check(dt::ClassifyDatatypeCast(invalid_type,
                                 dt::CanonicalTypeId::int64, true) ==
            dt::DatatypeCastCategory::forbidden,
        "cast classifier admitted an out-of-range source type");

  dt::DatatypeCastRequest invalid_target;
  invalid_target.value = PresentInt64(1);
  invalid_target.target_type_id = invalid_type;
  invalid_target.context = dt::DatatypeCastContext::explicit_cast;
  invalid_target.reference_compatibility_profile = true;
  CheckRejectedAs(dt::CastDatatypeValue(invalid_target),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "out-of-range cast target descriptor");

  dt::DatatypeCastRequest invalid_source;
  invalid_source.value = {invalid_type, "1", false};
  invalid_source.target_type_id = dt::CanonicalTypeId::int64;
  invalid_source.context = dt::DatatypeCastContext::explicit_cast;
  invalid_source.reference_compatibility_profile = true;
  CheckRejectedAs(dt::CastDatatypeValue(invalid_source),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "out-of-range cast source descriptor");

  dt::DatatypeCastRequest concrete_to_unknown;
  concrete_to_unknown.value = PresentInt64(1);
  concrete_to_unknown.target_type_id = dt::CanonicalTypeId::unknown;
  CheckRejectedAs(dt::CastDatatypeValue(concrete_to_unknown),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "concrete source cast to unknown target descriptor");

  auto dirty_to_unknown = concrete_to_unknown;
  dirty_to_unknown.value =
      TypedNull(dt::CanonicalTypeId::int64,
                DescriptorFor(dt::CanonicalTypeId::int64, 0x1eu));
  dirty_to_unknown.value.encoded_value = "payload";
  CheckRejectedAs(dt::CastDatatypeValue(dirty_to_unknown),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "unknown target descriptor authority before dirty NULL state");

  dt::DatatypeCastRequest dirty_to_described_null;
  dirty_to_described_null.value = dirty_to_unknown.value;
  dirty_to_described_null.target_type_id = dt::CanonicalTypeId::null_type;
  dirty_to_described_null.target_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x1fu);
  CheckRejectedAs(dt::CastDatatypeValue(dirty_to_described_null),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "forbidden null target descriptor before dirty NULL state");

  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    const auto source_descriptor = DescriptorFor(descriptor.type_id, 0x20u);
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      dt::DatatypeCastRequest cast_to_null;
      cast_to_null.value = TypedNull(descriptor.type_id, source_descriptor);
      cast_to_null.target_type_id = dt::CanonicalTypeId::null_type;
      cast_to_null.context = context;
      cast_to_null.explicit_cast = context == dt::DatatypeCastContext::explicit_cast;
      cast_to_null.reference_compatibility_profile = true;
      CheckRejectedAs(dt::CastDatatypeValue(cast_to_null),
                      descriptor.type_id == dt::CanonicalTypeId::bit_string
                          ? "CTB.BIT.DESCRIPTOR_INVALID"
                          : "DATATYPE.CAST_FORBIDDEN",
                      std::string("concrete typed NULL cast to null_type from ") +
                          descriptor.stable_name + "/" +
                          dt::DatatypeCastContextName(context));
    }
  }

  dt::DatatypeCastRequest malformed_to_null;
  malformed_to_null.value =
      TypedNull(dt::CanonicalTypeId::int64,
                DescriptorFor(dt::CanonicalTypeId::int64, 0x30u));
  malformed_to_null.value.encoded_value = "payload";
  malformed_to_null.target_type_id = dt::CanonicalTypeId::null_type;
  CheckRejectedAs(dt::CastDatatypeValue(malformed_to_null),
                  "DATATYPE.NULL_STATE.INVALID",
                  "payload-bearing typed NULL cast to null_type");

  dt::DatatypeCastRequest invalid_context;
  invalid_context.value = {dt::CanonicalTypeId::null_type, {}, true};
  invalid_context.target_type_id = dt::CanonicalTypeId::int64;
  invalid_context.target_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x31u);
  invalid_context.context = static_cast<dt::DatatypeCastContext>(0xffffu);
  Check(std::string(dt::DatatypeCastContextName(invalid_context.context)) ==
            "unknown",
        "out-of-range cast context rendered as a valid context");
  CheckRejectedAs(dt::CastDatatypeValue(invalid_context),
                  "DATATYPE.CAST_FORBIDDEN", "out-of-range cast context");

  auto descriptor_precedes_context = invalid_context;
  descriptor_precedes_context.value =
      {dt::CanonicalTypeId::int64, "payload", true};
  CheckRejectedAs(dt::CastDatatypeValue(descriptor_precedes_context),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "descriptor precedence over out-of-range cast context");

  auto nullability_precedes_context = invalid_context;
  const auto nullable_source =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x32u);
  nullability_precedes_context.value =
      TypedNull(dt::CanonicalTypeId::int64, nullable_source);
  nullability_precedes_context.target_descriptor = nullable_source;
  nullability_precedes_context.target_descriptor.nullable_allowed = false;
  CheckRejectedAs(dt::CastDatatypeValue(nullability_precedes_context),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "typed NULL descriptor mismatch before cast context");
}

void ExactDescriptorFidelityAndRefusal() {
  const auto full_descriptor =
      DescriptorFor(dt::CanonicalTypeId::decimal, 0x50u, true);

  dt::DatatypeCastRequest contextual;
  contextual.value = {dt::CanonicalTypeId::null_type, {}, true};
  contextual.target_type_id = dt::CanonicalTypeId::decimal;
  contextual.target_descriptor = full_descriptor;
  const auto bound = dt::CastDatatypeValue(contextual);
  Check(bound.ok() && bound.value.is_null &&
            DescriptorEquals(bound.value.descriptor, full_descriptor),
        "contextual binding did not preserve every descriptor field");

  dt::DatatypeCastRequest identity;
  identity.value = TypedNull(dt::CanonicalTypeId::decimal, full_descriptor);
  identity.target_type_id = dt::CanonicalTypeId::decimal;
  identity.target_descriptor = full_descriptor;
  const auto identity_result = dt::CastDatatypeValue(identity);
  Check(identity_result.ok() &&
            identity_result.category == dt::DatatypeCastCategory::identity &&
            DescriptorEquals(identity_result.value.descriptor,
                             full_descriptor),
        "full-fidelity typed NULL identity cast failed");

  identity.target_descriptor =
      DescriptorFor(dt::CanonicalTypeId::decimal, 0x51u, true);
  CheckRejectedAs(dt::CastDatatypeValue(identity),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "identity cast across unequal exact descriptors");

  const auto int64_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x60u);
  const auto real64_descriptor =
      DescriptorFor(dt::CanonicalTypeId::real64, 0x61u);
  dt::DatatypeCastRequest cross_type;
  cross_type.value =
      TypedNull(dt::CanonicalTypeId::int64, int64_descriptor);
  cross_type.target_type_id = dt::CanonicalTypeId::real64;
  cross_type.context = dt::DatatypeCastContext::explicit_cast;
  cross_type.target_descriptor = real64_descriptor;
  CheckRejectedAs(dt::CastDatatypeValue(cross_type),
                  "DATATYPE.CAST_FORBIDDEN",
                  "unresolved int64-to-real64 typed NULL cast");

  cross_type.value =
      TypedNull(dt::CanonicalTypeId::decimal, full_descriptor);
  CheckRejectedAs(dt::CastDatatypeValue(cross_type),
                  "DATATYPE.CAST_FORBIDDEN",
                  "domain-bearing cross-type typed NULL cast without policy");

  const auto check_bad_descriptor = [&](auto mutate,
                                        const std::string& label) {
    auto bad = int64_descriptor;
    mutate(&bad);
    dt::DatatypeSerializationRequest request;
    request.value = TypedNull(dt::CanonicalTypeId::int64, bad);
    CheckRejectedAs(dt::SerializeDatatypeValue(request),
                    "DATATYPE.DESCRIPTOR.INVALID", label);
  };
  check_bad_descriptor(
      [](auto* descriptor) { descriptor->descriptor_uuid = {}; },
      "typed NULL descriptor with nil UUID");
  check_bad_descriptor(
      [](auto* descriptor) { descriptor->descriptor_epoch = 0; },
      "typed NULL descriptor with zero generation");
  check_bad_descriptor(
      [](auto* descriptor) { descriptor->descriptor_uuid.bytes[15] ^= 0x01u; },
      "typed NULL descriptor with non-authoritative UUID");
  check_bad_descriptor(
      [](auto* descriptor) { ++descriptor->descriptor_epoch; },
      "typed NULL descriptor with stale generation");
  check_bad_descriptor(
      [](auto* descriptor) {
        descriptor->canonical_type_id =
            static_cast<std::uint32_t>(dt::CanonicalTypeId::int32);
      },
      "typed NULL descriptor with mismatched canonical type");
  check_bad_descriptor(
      [](auto* descriptor) {
        descriptor->family = engine::ExecutionTypeFamily::character;
      },
      "typed NULL descriptor with altered family");
  check_bad_descriptor(
      [](auto* descriptor) {
        descriptor->width_class = engine::ExecutionTypeWidthClass::variable;
      },
      "typed NULL descriptor with altered width class");
  check_bad_descriptor(
      [](auto* descriptor) { descriptor->bit_width = 32; },
      "typed NULL descriptor with altered bit width");
  check_bad_descriptor(
      [](auto* descriptor) { descriptor->descriptor_authoritative = false; },
      "typed NULL descriptor without authority");
  check_bad_descriptor(
      [](auto* descriptor) {
        descriptor->precision = 34;
        descriptor->modifier_flags |= engine::ExecutionTypeModifierFlagBit(
            engine::ExecutionTypeModifierFlag::precision);
      },
      "integer descriptor with decimal precision modifier");
  check_bad_descriptor(
      [](auto* descriptor) {
        descriptor->domain_uuid = descriptor->descriptor_uuid;
        auto different_domain = descriptor->domain_uuid;
        different_domain.bytes[15] ^= 0x01u;
        descriptor->domain_stack = {different_domain};
        descriptor->modifier_flags |=
            engine::ExecutionTypeModifierFlagBit(
                engine::ExecutionTypeModifierFlag::domain_uuid) |
            engine::ExecutionTypeModifierFlagBit(
                engine::ExecutionTypeModifierFlag::domain_stack);
      },
      "domain descriptor with mismatched stack head");
  check_bad_descriptor(
      [](auto* descriptor) {
        descriptor->domain_uuid = descriptor->descriptor_uuid;
        descriptor->domain_stack = {descriptor->domain_uuid,
                                    descriptor->domain_uuid};
        descriptor->modifier_flags |=
            engine::ExecutionTypeModifierFlagBit(
                engine::ExecutionTypeModifierFlag::domain_uuid) |
            engine::ExecutionTypeModifierFlagBit(
                engine::ExecutionTypeModifierFlag::domain_stack);
      },
      "domain descriptor with duplicate stack entry");

  dt::DatatypeSerializationRequest missing_and_payload;
  missing_and_payload.value =
      {dt::CanonicalTypeId::int64, "payload", true};
  CheckRejectedAs(dt::SerializeDatatypeValue(missing_and_payload),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "typed NULL with missing descriptor and payload");
  missing_and_payload.value =
      TypedNull(dt::CanonicalTypeId::int64, int64_descriptor);
  missing_and_payload.value.encoded_value = "payload";
  CheckRejectedAs(dt::SerializeDatatypeValue(missing_and_payload),
                  "DATATYPE.NULL_STATE.INVALID",
                  "typed NULL with valid descriptor and payload");

  auto nonnullable = int64_descriptor;
  nonnullable.nullable_allowed = false;
  dt::DatatypeSerializationRequest not_admitted;
  not_admitted.value = TypedNull(dt::CanonicalTypeId::int64, nonnullable);
  CheckRejectedAs(dt::SerializeDatatypeValue(not_admitted),
                  "DATATYPE.NULL_NOT_ADMITTED",
                  "typed NULL with nonnullable descriptor");

  dt::DatatypeNumericOperationRequest numeric;
  numeric.operation = dt::DatatypeNumericOperationKind::canonicalize;
  numeric.type_id = dt::CanonicalTypeId::decimal;
  numeric.left = TypedNull(dt::CanonicalTypeId::decimal, full_descriptor);
  CheckRejectedAs(dt::ApplyNumericOperation(numeric),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "numeric NULL result without declared descriptor");

  dt::DatatypeExtractRequest extract;
  extract.value = TypedNull(
      dt::CanonicalTypeId::date,
      DescriptorFor(dt::CanonicalTypeId::date, 0x62u));
  extract.field = "year";
  CheckRejectedAs(dt::ExtractDatatypeField(extract),
                  "SB_DATATYPE_EXTRACT_REJECTED",
                  "unresolved temporal NULL extraction without result profile");
}

void BoundOperationsRetainConcreteTypeIds() {
  const auto decimal_descriptor =
      DescriptorFor(dt::CanonicalTypeId::decimal, 0x40u);
  dt::DatatypeNumericOperationRequest numeric;
  numeric.operation = dt::DatatypeNumericOperationKind::canonicalize;
  numeric.type_id = dt::CanonicalTypeId::decimal;
  numeric.left = TypedNull(dt::CanonicalTypeId::decimal, decimal_descriptor);
  numeric.result_descriptor = decimal_descriptor;
  const auto numeric_result = dt::ApplyNumericOperation(numeric);
  Check(!numeric_result.ok() &&
            numeric_result.diagnostic.diagnostic_code ==
                "SB_DATATYPE_NUMERIC_OPERATION_REJECTED" &&
            DiagnosticDetail(numeric_result.diagnostic) ==
                "decimal_numeric_policy_unresolved" &&
            numeric_result.value.encoded_value.empty(),
        "typed decimal NULL canonicalization remained admitted without operation policy");

  numeric.left = TypedNull(dt::CanonicalTypeId::decimal, decimal_descriptor);
  numeric.left.encoded_value = "payload";
  CheckRejectedAs(dt::ApplyNumericOperation(numeric),
                  "DATATYPE.NULL_STATE.INVALID",
                  "payload-bearing numeric NULL");

  numeric.operation = dt::DatatypeNumericOperationKind::compare;
  numeric.left = TypedNull(dt::CanonicalTypeId::decimal, decimal_descriptor);
  numeric.right = DecimalValue("1", decimal_descriptor);
  const auto boolean_descriptor =
      DescriptorFor(dt::CanonicalTypeId::boolean, 0x44u);
  numeric.result_descriptor = boolean_descriptor;
  const auto null_comparison = dt::ApplyNumericOperation(numeric);
  Check(!null_comparison.ok() &&
            null_comparison.diagnostic.diagnostic_code ==
                "SB_DATATYPE_NUMERIC_OPERATION_REJECTED" &&
            DiagnosticDetail(null_comparison.diagnostic) ==
                "decimal_numeric_policy_unresolved" &&
            null_comparison.value.encoded_value.empty(),
        "decimal NULL comparison remained admitted without comparison policy");

  numeric.result_descriptor = decimal_descriptor;
  CheckRejectedAs(dt::ApplyNumericOperation(numeric),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "numeric NULL comparison with non-Boolean result descriptor");

  numeric.result_descriptor = {};
  CheckRejectedAs(dt::ApplyNumericOperation(numeric),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "numeric NULL comparison without result descriptor");

  numeric.left = DecimalValue("1", decimal_descriptor);
  numeric.result_descriptor = boolean_descriptor;
  const auto present_comparison = dt::ApplyNumericOperation(numeric);
  Check(!present_comparison.ok() &&
            present_comparison.diagnostic.diagnostic_code ==
                "SB_DATATYPE_NUMERIC_OPERATION_REJECTED" &&
            DiagnosticDetail(present_comparison.diagnostic) ==
                "decimal_numeric_policy_unresolved" &&
            present_comparison.value.encoded_value.empty(),
        "present decimal comparison remained admitted without comparison policy");

  dt::DatatypeExtractRequest extract;
  extract.value = TypedNull(dt::CanonicalTypeId::date,
                            DescriptorFor(dt::CanonicalTypeId::date, 0x41u));
  extract.field = "year";
  extract.result_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int32, 0x42u);
  CheckRejectedAs(dt::ExtractDatatypeField(extract),
                  "SB_DATATYPE_EXTRACT_REJECTED",
                  "unresolved temporal-to-int32 NULL extraction");

  extract.value.encoded_value = "payload";
  CheckRejectedAs(dt::ExtractDatatypeField(extract),
                  "DATATYPE.NULL_STATE.INVALID",
                  "payload-bearing extraction NULL");

  dt::DatatypeComparisonRequest comparison;
  comparison.left = {dt::CanonicalTypeId::null_type, {}, true};
  comparison.right = {dt::CanonicalTypeId::null_type, {}, true};
  CheckRejectedAs(dt::CompareDatatypeValues(comparison),
                  "DATATYPE.DESCRIPTOR.INVALID", "standalone NULL comparison");

  const auto int64_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x43u);
  comparison.left = TypedNull(dt::CanonicalTypeId::int64, int64_descriptor);
  comparison.right = PresentInt64(1, int64_descriptor);
  comparison.null_ordering = dt::DatatypeNullOrdering::nulls_first;
  CheckRejectedAs(dt::CompareDatatypeValues(comparison),
                  "SB_DATATYPE_COMPARISON_REJECTED",
                  "unresolved int64 NULLS FIRST comparison");
  comparison.null_ordering = dt::DatatypeNullOrdering::nulls_last;
  CheckRejectedAs(dt::CompareDatatypeValues(comparison),
                  "SB_DATATYPE_COMPARISON_REJECTED",
                  "unresolved int64 NULLS LAST comparison");

  auto descriptor_precedes_bad_null_order = comparison;
  ++descriptor_precedes_bad_null_order.left.descriptor.descriptor_epoch;
  descriptor_precedes_bad_null_order.null_ordering =
      static_cast<dt::DatatypeNullOrdering>(0xffffu);
  CheckRejectedAs(dt::CompareDatatypeValues(descriptor_precedes_bad_null_order),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "comparison descriptor precedence over NULL ordering");

  comparison.right = TypedNull(
      dt::CanonicalTypeId::date,
      DescriptorFor(dt::CanonicalTypeId::date, 0x44u));
  CheckRejectedAs(dt::CompareDatatypeValues(comparison),
                  "SB_DATATYPE_COMPARISON_REJECTED",
                  "different concrete typed NULL comparison");
  comparison.right = PresentInt64(1, int64_descriptor);

  dt::DatatypeSortKeyRequest sort;
  sort.value = TypedNull(dt::CanonicalTypeId::int64, int64_descriptor);
  sort.null_ordering = dt::DatatypeNullOrdering::nulls_first;
  const auto null_first = dt::MakeDatatypeSortKey(sort);
  sort.null_ordering = dt::DatatypeNullOrdering::nulls_last;
  const auto null_last = dt::MakeDatatypeSortKey(sort);
  Check(null_first.ok() && null_last.ok() && null_first.sort_key < null_last.sort_key,
        "typed NULL sort keys do not preserve explicit NULL placement");
  sort.value = TypedNull(dt::CanonicalTypeId::int64, int64_descriptor);
  ++sort.value.descriptor.descriptor_epoch;
  sort.null_ordering = static_cast<dt::DatatypeNullOrdering>(0xffffu);
  CheckRejectedAs(dt::MakeDatatypeSortKey(sort), "DATATYPE.DESCRIPTOR.INVALID",
                  "sort descriptor precedence over NULL ordering");
  sort.value = {dt::CanonicalTypeId::null_type, {}, true};
  CheckRejectedAs(dt::MakeDatatypeSortKey(sort), "DATATYPE.DESCRIPTOR.INVALID",
                  "standalone NULL sort key");

  dt::DatatypeHashRequest hash;
  hash.value = TypedNull(dt::CanonicalTypeId::int64, int64_descriptor);
  CheckRejectedAs(dt::HashDatatypeValue(hash),
                  "SB_DATATYPE_HASH_REJECTED",
                  "unresolved typed int64 NULL hash");
  hash.value = {dt::CanonicalTypeId::null_type, {}, true};
  CheckRejectedAs(dt::HashDatatypeValue(hash), "DATATYPE.DESCRIPTOR.INVALID",
                  "standalone NULL hash");

  dt::DatatypeDisplayRenderRequest display;
  display.value = TypedNull(dt::CanonicalTypeId::int64, int64_descriptor);
  const auto rendered = dt::RenderDatatypeValueForDisplay(display);
  Check(rendered.ok() && rendered.canonical_type_name == "int64" &&
            rendered.display_value == "NULL",
        "typed NULL display lost its concrete type");
  display.value = {dt::CanonicalTypeId::null_type, {}, true};
  CheckRejectedAs(dt::RenderDatatypeValueForDisplay(display),
                  "DATATYPE.DESCRIPTOR.INVALID", "standalone NULL display");

  comparison.left = TypedNull(dt::CanonicalTypeId::int64, int64_descriptor);
  comparison.left.encoded_value = "payload";
  CheckRejectedAs(dt::CompareDatatypeValues(comparison),
                  "DATATYPE.NULL_STATE.INVALID", "payload-bearing typed NULL comparison");

  dt::DatatypeSetDescriptor set_descriptor;
  set_descriptor.element_type_id = dt::CanonicalTypeId::int64;
  set_descriptor.element_descriptor = int64_descriptor;
  set_descriptor.allow_null_elements = true;
  Check(dt::EncodeSetValue(
            set_descriptor,
            {TypedNull(dt::CanonicalTypeId::int64, int64_descriptor)}).ok(),
        "set codec refused descriptor-bound typed NULL element");
  CheckRejectedAs(
      dt::EncodeSetValue(
          set_descriptor,
          {dt::DatatypeOperationValue{dt::CanonicalTypeId::int64, {}, true}}),
      "DATATYPE.DESCRIPTOR.INVALID",
      "set codec accepted typed NULL element without descriptor");
  auto disallow_null_set_descriptor = set_descriptor;
  disallow_null_set_descriptor.allow_null_elements = false;
  CheckRejectedAs(
      dt::EncodeSetValue(
          disallow_null_set_descriptor,
          {TypedNull(dt::CanonicalTypeId::int64, int64_descriptor)}),
      "DATATYPE.NULL_NOT_ADMITTED",
      "set codec used a generic rejection for disallowed SQL NULL element");

  dt::DatatypeSetDescriptor character_set_descriptor;
  character_set_descriptor.element_type_id =
      dt::CanonicalTypeId::character;
  character_set_descriptor.element_descriptor =
      DescriptorFor(dt::CanonicalTypeId::character, 0x45u);
  character_set_descriptor.allow_null_elements = true;

  auto printable_null = dt::DatatypeOperationValue{
      dt::CanonicalTypeId::character, "<NULL>", false};
  printable_null.descriptor = character_set_descriptor.element_descriptor;
  const auto character_null = TypedNull(
      dt::CanonicalTypeId::character,
      character_set_descriptor.element_descriptor);
  const auto refused_character_set = dt::EncodeSetValue(
      character_set_descriptor, {character_null, printable_null});
  CheckRejectedAs(refused_character_set,
                  "SB_DATATYPE_SET_OPERATION_REJECTED",
                  "character set without collation authority");
  Check(DiagnosticDetail(refused_character_set.diagnostic) ==
            "character_set_collation_policy_unresolved" &&
            refused_character_set.encoded_set.empty(),
        "character set refusal published output or lost policy detail");

  auto descriptorless_character_set = character_set_descriptor;
  descriptorless_character_set.element_descriptor = {};
  CheckRejectedAs(
      dt::EncodeSetValue(descriptorless_character_set,
                         {character_null, printable_null}),
      "DATATYPE.DESCRIPTOR.INVALID",
      "character set without exact element descriptor");

  dt::DatatypeSetOperationRequest character_set_operation;
  character_set_operation.descriptor = character_set_descriptor;
  character_set_operation.operation =
      dt::DatatypeSetOperationKind::membership;
  character_set_operation.left_encoded_set =
      "SBSET2;element=character;descriptor=unresolved";
  character_set_operation.right_value = printable_null;
  const auto refused_character_operation =
      dt::ApplySetOperation(character_set_operation);
  CheckRejectedAs(refused_character_operation,
                  "SB_DATATYPE_SET_OPERATION_REJECTED",
                  "character set operation without collation authority");
  Check(DiagnosticDetail(refused_character_operation.diagnostic) ==
            "character_set_collation_policy_unresolved" &&
            refused_character_operation.value.type_id ==
                dt::CanonicalTypeId::unknown &&
            refused_character_operation.value.encoded_value.empty() &&
            refused_character_operation.encoded_set.empty(),
        "character set operation refusal published output or lost policy detail");
}

void DurableCodecsRequireConcreteTypes() {
  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    const std::string label = descriptor.stable_name;
    dt::DatatypeBinaryValue binary_null;
    binary_null.type_id = descriptor.type_id;
    binary_null.is_null = true;
    const auto encoded = dt::EncodeDatatypeBinaryValue(binary_null);
    if (descriptor.type_id == dt::CanonicalTypeId::bit_string) {
      CheckRejectedAs(encoded, "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
                      "generic bit-string NULL binary encode without V3 profile");
      dt::DatatypePhysicalValue physical_null;
      physical_null.type_id = descriptor.type_id;
      physical_null.state = dt::DatatypePhysicalValueState::sql_null;
      CheckRejectedAs(dt::EncodeDatatypePhysicalValue(physical_null),
                      "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
                      "generic bit-string NULL physical encode without V3 profile");
      continue;
    }
    Check(encoded.ok(), "typed NULL binary encoding failed for " + label);
    Check(encoded.encoded.size() == dt::kDatatypeBinaryEnvelopeHeaderBytes,
          "typed NULL binary envelope contains payload bytes for " + label);
    if (encoded.ok()) {
      const auto decoded = dt::DecodeDatatypeBinaryValue(encoded.encoded);
      Check(decoded.ok() && decoded.value.type_id == descriptor.type_id &&
                decoded.value.is_null && decoded.value.payload.empty(),
            "typed NULL binary round trip lost concrete type/state for " + label);
    }

    dt::DatatypePhysicalValue physical_null;
    physical_null.type_id = descriptor.type_id;
    physical_null.state = dt::DatatypePhysicalValueState::sql_null;
    const auto physical_encoded = dt::EncodeDatatypePhysicalValue(physical_null);
    Check(physical_encoded.ok(), "typed NULL physical encoding failed for " + label);
    if (physical_encoded.ok()) {
      const auto decoded = dt::DecodeDatatypePhysicalValue(
          physical_encoded.bytes.data(), physical_encoded.bytes.size());
      Check(decoded.ok() && decoded.value.type_id == descriptor.type_id &&
                decoded.value.state == dt::DatatypePhysicalValueState::sql_null &&
                decoded.value.payload.empty(),
            "typed NULL physical round trip lost concrete type/state for " + label);
    }
  }

  dt::DatatypeBinaryValue binary_null;
  binary_null.type_id = dt::CanonicalTypeId::null_type;
  binary_null.is_null = true;
  CheckRejectedAs(dt::EncodeDatatypeBinaryValue(binary_null),
                  "DATATYPE.DESCRIPTOR.INVALID", "standalone NULL binary encoding");
  binary_null.type_id = dt::CanonicalTypeId::int64;
  binary_null.payload = {0x01};
  CheckRejectedAs(dt::EncodeDatatypeBinaryValue(binary_null),
                  "DATATYPE.NULL_STATE.INVALID", "payload-bearing typed NULL binary encoding");

  dt::DatatypePhysicalValue physical_null;
  physical_null.type_id = dt::CanonicalTypeId::null_type;
  physical_null.state = dt::DatatypePhysicalValueState::sql_null;
  CheckRejectedAs(dt::EncodeDatatypePhysicalValue(physical_null),
                  "DATATYPE.DESCRIPTOR.INVALID", "standalone NULL physical encoding");
  physical_null.type_id = dt::CanonicalTypeId::int64;
  physical_null.payload = {0x01};
  CheckRejectedAs(dt::EncodeDatatypePhysicalValue(physical_null),
                  "DATATYPE.NULL_STATE.INVALID", "payload-bearing typed NULL physical encoding");
}

void SerializationRetainsConcreteType() {
  const auto present_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x6fu);
  dt::DatatypeSerializationRequest present_request;
  present_request.value = PresentInt64(1, present_descriptor);
  const auto present_serialized = dt::SerializeDatatypeValue(present_request);
  Check(present_serialized.ok() &&
            present_serialized.serialized_value ==
                "SBDV1;type=int64;state=value;payload=0100000000000000" &&
            DescriptorEquals(present_serialized.descriptor,
                             present_descriptor),
        "present value serialization changed bytes or descriptor sidecar");
  dt::DatatypeDeserializationRequest present_decode;
  present_decode.expected_type_id = dt::CanonicalTypeId::int64;
  present_decode.serialized_value = present_serialized.serialized_value;
  present_decode.expected_descriptor = present_descriptor;
  const auto present_decoded = dt::DeserializeDatatypeValue(present_decode);
  Check(present_decoded.ok() && !present_decoded.value.is_null &&
            present_decoded.value.encoded_value ==
                std::string("\x01\x00\x00\x00\x00\x00\x00\x00", 8) &&
            DescriptorEquals(present_decoded.value.descriptor,
                             present_descriptor),
        "present value deserialization lost exact expected descriptor");
  present_decode.expected_descriptor = {};
  const auto legacy_present = dt::DeserializeDatatypeValue(present_decode);
  Check(legacy_present.ok() && !legacy_present.value.is_null &&
            legacy_present.value.type_id == dt::CanonicalTypeId::int64 &&
            legacy_present.value.encoded_value ==
                std::string("\x01\x00\x00\x00\x00\x00\x00\x00", 8),
        "legacy present value decode without descriptor regressed during NULL work");

  for (const auto& descriptor : dt::BuiltinDatatypeDescriptors()) {
    const auto execution_descriptor = DescriptorFor(descriptor.type_id, 0x70u);
    dt::DatatypeSerializationRequest typed_null;
    typed_null.value = TypedNull(descriptor.type_id, execution_descriptor);
    const auto serialized = dt::SerializeDatatypeValue(typed_null);
    const std::string label = descriptor.stable_name;
    if (descriptor.type_id == dt::CanonicalTypeId::bit_string) {
      CheckRejectedAs(serialized, "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
                      "generic bit-string NULL serialization without V3 profile");
      Check(serialized.serialized_value.empty(),
            "generic bit-string NULL serialization published output");
      continue;
    }
    if (descriptor.type_id == dt::CanonicalTypeId::uuid) {
      CheckRejectedAs(serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                      "typed UUID NULL serialization without codec policy");
      Check(DiagnosticDetail(serialized.diagnostic) ==
                    "uuid_serialization_policy_unresolved" &&
                serialized.serialized_value.empty(),
            "typed UUID NULL serialization failure published output or wrong detail");
      continue;
    }
    if (descriptor.type_id == dt::CanonicalTypeId::ip_address) {
      CheckRejectedAs(serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
                      "typed IP address NULL serialization without codec policy");
      Check(DiagnosticDetail(serialized.diagnostic) ==
                    "ip_address_serialization_policy_unresolved" &&
                serialized.serialized_value.empty(),
            "typed IP address NULL serialization failure published output or wrong detail");
      continue;
    }
    if (descriptor.type_id == dt::CanonicalTypeId::network_prefix) {
      CheckRejectedAs(
          serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
          "typed network prefix NULL serialization without codec policy");
      Check(DiagnosticDetail(serialized.diagnostic) ==
                    "network_prefix_serialization_policy_unresolved" &&
                serialized.serialized_value.empty(),
            "typed network prefix NULL serialization failure published output or wrong detail");
      continue;
    }
    if (descriptor.type_id == dt::CanonicalTypeId::mac_address) {
      CheckRejectedAs(
          serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
          "typed MAC address NULL serialization without codec policy");
      Check(DiagnosticDetail(serialized.diagnostic) ==
                    "mac_address_serialization_policy_unresolved" &&
                serialized.serialized_value.empty(),
            "typed MAC address NULL serialization failure published output or wrong detail");
      continue;
    }
    if (descriptor.type_id == dt::CanonicalTypeId::character) {
      CheckRejectedAs(
          serialized, "SB_DATATYPE_SERIALIZATION_REJECTED",
          "typed character NULL serialization without codec policy");
      Check(DiagnosticDetail(serialized.diagnostic) ==
                    "character_serialization_policy_unresolved" &&
                serialized.serialized_value.empty(),
            "typed character NULL serialization failure published output or wrong detail");
      continue;
    }
    if (descriptor.type_id == dt::CanonicalTypeId::binary) {
      CheckRejectedAs(
          serialized, "CTB.BINARY.SERIALIZATION_PROFILE_MISSING",
          "typed binary NULL generic serialization without receipt profile");
      Check(serialized.serialized_value.empty(),
            "typed binary NULL generic serialization published output");

      dt::DatatypeDeserializationRequest decode;
      decode.expected_type_id = dt::CanonicalTypeId::binary;
      decode.serialized_value =
          "SBDV1;type=binary;state=null;payload=";
      decode.expected_descriptor = execution_descriptor;
      const auto decoded = dt::DeserializeDatatypeValue(decode);
      CheckRejectedAs(
          decoded, "CTB.BINARY.SERIALIZATION_PROFILE_MISSING",
          "typed binary NULL generic deserialization without receipt profile");
      Check(decoded.value.type_id == dt::CanonicalTypeId::unknown &&
                !decoded.value.is_null &&
                decoded.value.encoded_value.empty(),
            "typed binary NULL generic deserialization published a value");
      continue;
    }
    Check(serialized.ok(), "typed NULL serialization failed for " + label);
    Check(DescriptorEquals(serialized.descriptor, execution_descriptor),
          "typed NULL serialization lost descriptor sidecar for " + label);
    if (serialized.ok()) {
      dt::DatatypeDeserializationRequest decode;
      decode.expected_type_id = descriptor.type_id;
      decode.serialized_value = serialized.serialized_value;
      decode.expected_descriptor = execution_descriptor;
      const auto decoded = dt::DeserializeDatatypeValue(decode);
      Check(decoded.ok() && decoded.value.type_id == descriptor.type_id &&
                decoded.value.is_null && decoded.value.encoded_value.empty(),
            "typed NULL serialization lost concrete type/state for " + label);
      Check(DescriptorEquals(decoded.value.descriptor, execution_descriptor),
            "typed NULL deserialization lost expected descriptor for " + label);
    }
  }

  dt::DatatypeSerializationRequest request;
  request.value = {dt::CanonicalTypeId::null_type, {}, true};
  CheckRejectedAs(dt::SerializeDatatypeValue(request),
                  "DATATYPE.DESCRIPTOR.INVALID", "standalone NULL serialization");

  request.value = TypedNull(
      dt::CanonicalTypeId::int64,
      DescriptorFor(dt::CanonicalTypeId::int64, 0x71u));
  request.value.encoded_value = "payload";
  CheckRejectedAs(dt::SerializeDatatypeValue(request),
                  "DATATYPE.NULL_STATE.INVALID", "payload-bearing typed NULL serialization");

  const auto payload_null = dt::DeserializeDatatypeValue(
      {dt::CanonicalTypeId::int64,
       "SBDV1;type=int64;state=null;payload=01"});
  CheckRejectedAs(payload_null, "DATATYPE.DESCRIPTOR.INVALID",
                  "payload-bearing typed NULL without expected descriptor");
  Check(payload_null.value.type_id == dt::CanonicalTypeId::unknown &&
            !payload_null.value.is_null &&
            payload_null.value.encoded_value.empty(),
        "payload-bearing typed NULL deserialization published a value");

  dt::DatatypeDeserializationRequest payload_null_with_descriptor;
  payload_null_with_descriptor.expected_type_id = dt::CanonicalTypeId::int64;
  payload_null_with_descriptor.serialized_value =
      "SBDV1;type=int64;state=null;payload=01";
  payload_null_with_descriptor.expected_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x73u);
  CheckRejectedAs(dt::DeserializeDatatypeValue(payload_null_with_descriptor),
                  "DATATYPE.NULL_STATE.INVALID",
                  "payload-bearing typed NULL with expected descriptor");

  dt::DatatypeSerializationRequest uuid_null_request;
  const auto uuid_descriptor =
      DescriptorFor(dt::CanonicalTypeId::uuid, 0x72u);
  uuid_null_request.value =
      TypedNull(dt::CanonicalTypeId::uuid, uuid_descriptor);
  const auto uuid_null_encoded = dt::SerializeDatatypeValue(uuid_null_request);
  CheckRejectedAs(uuid_null_encoded, "SB_DATATYPE_SERIALIZATION_REJECTED",
                  "typed UUID NULL serialization without codec policy");
  Check(DiagnosticDetail(uuid_null_encoded.diagnostic) ==
            "uuid_serialization_policy_unresolved" &&
            uuid_null_encoded.serialized_value.empty(),
        "typed UUID NULL serialization failure published output or wrong detail");
  const std::string uuid_null_frame("SBDVUUID\0\0", 10);
  auto uuid_payload_null_frame = uuid_null_frame;
  uuid_payload_null_frame[9] = '\1';
  uuid_payload_null_frame.push_back('\0');
  dt::DatatypeDeserializationRequest uuid_payload_null_request;
  uuid_payload_null_request.expected_type_id = dt::CanonicalTypeId::uuid;
  uuid_payload_null_request.serialized_value = uuid_payload_null_frame;
  uuid_payload_null_request.expected_descriptor = uuid_descriptor;
  const auto uuid_payload_null =
      dt::DeserializeDatatypeValue(uuid_payload_null_request);
  CheckRejectedAs(uuid_payload_null, "DATATYPE.NULL_STATE.INVALID",
                  "payload-bearing typed UUID NULL deserialization");
  Check(DiagnosticDetail(uuid_payload_null.diagnostic) ==
            "null_payload_present",
        "payload-bearing typed UUID NULL lost state precedence");
  Check(uuid_payload_null.value.type_id == dt::CanonicalTypeId::unknown &&
            !uuid_payload_null.value.is_null &&
            uuid_payload_null.value.encoded_value.empty(),
        "payload-bearing typed UUID NULL deserialization published a value");

  const auto uuid_descriptor_mismatch = dt::DeserializeDatatypeValue(
      {dt::CanonicalTypeId::int64, uuid_null_frame});
  CheckRejectedAs(uuid_descriptor_mismatch, "DATATYPE.DESCRIPTOR.INVALID",
                  "typed UUID NULL descriptor mismatch");

  auto uuid_unsupported_state_frame = uuid_null_frame;
  uuid_unsupported_state_frame[8] = '\2';
  dt::DatatypeDeserializationRequest uuid_unsupported_state_request;
  uuid_unsupported_state_request.expected_type_id = dt::CanonicalTypeId::uuid;
  uuid_unsupported_state_request.serialized_value =
      uuid_unsupported_state_frame;
  uuid_unsupported_state_request.expected_descriptor = uuid_descriptor;
  const auto uuid_unsupported_state =
      dt::DeserializeDatatypeValue(uuid_unsupported_state_request);
  CheckRejectedAs(uuid_unsupported_state, "DTYPE.VALUE.STATE_UNHANDLED",
                  "typed UUID NULL unsupported state");
  Check(DiagnosticDetail(uuid_unsupported_state.diagnostic) ==
            "value_state_invalid",
        "typed UUID NULL state validation did not precede policy refusal");

  auto uuid_malformed_precedes_descriptor = uuid_null_frame;
  uuid_malformed_precedes_descriptor[8] = '\1';
  uuid_malformed_precedes_descriptor[9] = '\20';
  uuid_malformed_precedes_descriptor.append(17, '\0');
  CheckRejectedAs(
      dt::DeserializeDatatypeValue(
          {dt::CanonicalTypeId::int64, uuid_malformed_precedes_descriptor}),
      "SB_DATATYPE_DESERIALIZATION_REJECTED",
      "typed UUID malformed framing precedes descriptor mismatch");

  auto uuid_malformed_precedes_state = uuid_null_frame;
  uuid_malformed_precedes_state[8] = '\2';
  uuid_malformed_precedes_state[9] = '\1';
  CheckRejectedAs(
      dt::DeserializeDatatypeValue(
          {dt::CanonicalTypeId::uuid, uuid_malformed_precedes_state}),
      "SB_DATATYPE_DESERIALIZATION_REJECTED",
      "typed UUID malformed framing precedes unsupported state");

  dt::DatatypeDeserializationRequest uuid_round_trip;
  uuid_round_trip.expected_type_id = dt::CanonicalTypeId::uuid;
  uuid_round_trip.serialized_value = uuid_null_frame;
  uuid_round_trip.expected_descriptor = uuid_descriptor;
  const auto uuid_decoded = dt::DeserializeDatatypeValue(uuid_round_trip);
  CheckRejectedAs(uuid_decoded, "SB_DATATYPE_DESERIALIZATION_REJECTED",
                  "typed UUID NULL deserialization without codec policy");
  Check(DiagnosticDetail(uuid_decoded.diagnostic) ==
            "uuid_deserialization_policy_unresolved" &&
            uuid_decoded.value.type_id == dt::CanonicalTypeId::unknown &&
            !uuid_decoded.value.is_null &&
            uuid_decoded.value.encoded_value.empty(),
        "typed UUID NULL deserialization failure published output or wrong detail");

  uuid_round_trip.expected_descriptor = {};
  CheckRejectedAs(dt::DeserializeDatatypeValue(uuid_round_trip),
                  "DATATYPE.DESCRIPTOR.INVALID",
                  "typed UUID NULL decode without expected descriptor");

  for (const auto& standalone_type : {"null", "NULL", "null_type", "NULL_TYPE"}) {
    const auto decoded = dt::DeserializeDatatypeValue(
        {dt::CanonicalTypeId::int64,
         std::string("SBDV1;type=") + standalone_type +
             ";state=null;payload="});
    CheckRejectedAs(decoded, "DATATYPE.DESCRIPTOR.INVALID",
                    "standalone NULL descriptor deserialization");
    Check(decoded.value.type_id == dt::CanonicalTypeId::unknown &&
              !decoded.value.is_null && decoded.value.encoded_value.empty(),
          "standalone NULL descriptor deserialization published a value");
  }

  for (const auto& invalid_descriptor : {
           std::string("SBDV1;type=unknown_type;state=null;payload="),
           std::string("SBDV1;type=int32;state=null;payload=")}) {
    const auto decoded = dt::DeserializeDatatypeValue(
        {dt::CanonicalTypeId::int64, invalid_descriptor});
    CheckRejectedAs(decoded, "DATATYPE.DESCRIPTOR.INVALID",
                    "invalid or mismatched SBDV1 descriptor");
    Check(decoded.value.type_id == dt::CanonicalTypeId::unknown &&
              !decoded.value.is_null && decoded.value.encoded_value.empty(),
          "invalid or mismatched SBDV1 descriptor published a value");
  }

  dt::DatatypeDeserializationRequest unsupported_state_request;
  unsupported_state_request.expected_type_id = dt::CanonicalTypeId::int64;
  unsupported_state_request.serialized_value =
      "SBDV1;type=int64;state=missing;payload=";
  unsupported_state_request.expected_descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x74u);
  const auto unsupported_state =
      dt::DeserializeDatatypeValue(unsupported_state_request);
  CheckRejectedAs(unsupported_state, "DTYPE.VALUE.STATE_UNHANDLED",
                  "unsupported non-NULL SBDV1 state");
  Check(unsupported_state.value.type_id == dt::CanonicalTypeId::unknown &&
            !unsupported_state.value.is_null &&
            unsupported_state.value.encoded_value.empty(),
        "unsupported non-NULL SBDV1 state published a value");

  auto invalid_descriptor_precedes_state = unsupported_state_request;
  ++invalid_descriptor_precedes_state.expected_descriptor.descriptor_epoch;
  CheckRejectedAs(
      dt::DeserializeDatatypeValue(invalid_descriptor_precedes_state),
      "DATATYPE.DESCRIPTOR.INVALID",
      "invalid expected descriptor precedes unsupported SBDV1 state");

  for (const auto malformed : {
           std::string("SBDV1;type=int64;state=null"),
           std::string("SBDV1;type=int64;state=null;payload=;"),
           std::string("SBDV1;type=int64;state=null;payload=;state=null"),
           std::string("SBDV1;type=int64;state=null;payload=;payload="),
           std::string("SBDV1;type=int64;state=null;payload=;extra=x"),
           std::string("SBDV1;type=int64;broken;payload=")}) {
    const auto decoded = dt::DeserializeDatatypeValue(
        {dt::CanonicalTypeId::int64, malformed});
    CheckRejectedAs(decoded, "SB_DATATYPE_DESERIALIZATION_REJECTED",
                    "malformed SBDV1 framing");
    Check(decoded.value.type_id == dt::CanonicalTypeId::unknown &&
              !decoded.value.is_null && decoded.value.encoded_value.empty(),
          "malformed typed NULL deserialization published a value");
  }
}

// This fixed record is test-owned framing only. It is not a ScratchBird page,
// row, slot, descriptor codec, or production disk format. Its fixed fields
// retain the complete ExecutionTypeDescriptor solely so the independent test
// reader can verify the descriptor that owns the unchanged SBDPV001 image.
constexpr std::size_t kFixtureRecordBytes = 288;
constexpr std::size_t kFixtureCodecCapacity = 32;
constexpr std::size_t kFixtureStableNameCapacity = 30;
constexpr std::size_t kFixtureDomainStackCapacity = 2;
constexpr std::size_t kFixtureDescriptorUuidOffset = 8;
constexpr std::size_t kFixtureDescriptorEpochOffset = 24;
constexpr std::size_t kFixtureCanonicalTypeOffset = 32;
constexpr std::size_t kFixtureFamilyOffset = 36;
constexpr std::size_t kFixtureWidthOffset = 38;
constexpr std::size_t kFixtureStableNameLengthOffset = 40;
constexpr std::size_t kFixtureStableNameOffset = 42;
constexpr std::size_t kFixtureBitWidthOffset = 72;
constexpr std::size_t kFixturePrecisionOffset = 76;
constexpr std::size_t kFixtureScaleOffset = 80;
constexpr std::size_t kFixtureLengthOffset = 84;
constexpr std::size_t kFixtureDimensionsOffset = 88;
constexpr std::size_t kFixtureRankOffset = 92;
constexpr std::size_t kFixtureModifierFlagsOffset = 96;
constexpr std::size_t kFixtureDomainUuidOffset = 104;
constexpr std::size_t kFixtureDomainStackCountOffset = 120;
constexpr std::size_t kFixtureDomainStackOffset = 124;
constexpr std::size_t kFixtureCharsetUuidOffset = 156;
constexpr std::size_t kFixtureCollationUuidOffset = 172;
constexpr std::size_t kFixtureTimezoneUuidOffset = 188;
constexpr std::size_t kFixtureElementUuidOffset = 204;
constexpr std::size_t kFixtureSecurityUuidOffset = 220;
constexpr std::size_t kFixtureNullableOffset = 236;
constexpr std::size_t kFixtureAuthoritativeOffset = 237;
constexpr std::size_t kFixtureParserIndependentOffset = 238;
constexpr std::size_t kFixtureValueStateOffset = 240;
constexpr std::size_t kFixtureCodecLengthOffset = 244;
constexpr std::size_t kFixtureCodecComplementOffset = 248;
constexpr std::size_t kFixtureCodecOffset = 252;

constexpr std::array<platform::byte, 24> kInt64NullPhysicalOracle{{
    0x53, 0x42, 0x44, 0x50, 0x56, 0x30, 0x30, 0x31,
    0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x72, 0xdb, 0x1f, 0xdf}};

constexpr std::array<platform::byte, 32> kInt64PresentPhysicalOracle{{
    0x53, 0x42, 0x44, 0x50, 0x56, 0x30, 0x30, 0x31,
    0x67, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0xb5, 0xdd, 0xa2, 0x5e,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}};

engine::Uuid FixtureUuidOracle(std::uint8_t seed) {
  engine::Uuid uuid;
  for (std::size_t index = 0; index < sizeof(uuid.bytes); ++index) {
    uuid.bytes[index] =
        static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
  }
  uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0fu) | 0x70u);
  uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

engine::ExecutionTypeDescriptor PersistenceDescriptorOracle() {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = engine::Uuid{{
      0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
      0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x11}};
  descriptor.descriptor_epoch = 1;
  descriptor.canonical_type_id = 103;
  descriptor.family = engine::ExecutionTypeFamily::signed_integer;
  descriptor.width_class = engine::ExecutionTypeWidthClass::fixed;
  descriptor.stable_name = "int64";
  descriptor.bit_width = 64;
  descriptor.precision = 0;
  descriptor.scale = 0;
  descriptor.length = 0;
  descriptor.vector_dimensions = 0;
  descriptor.container_rank = 0;
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_stack) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::security_policy_uuid);
  descriptor.domain_uuid = FixtureUuidOracle(0x50u);
  descriptor.domain_stack = {FixtureUuidOracle(0x50u),
                             FixtureUuidOracle(0x52u)};
  descriptor.charset_uuid = {};
  descriptor.collation_uuid = {};
  descriptor.timezone_uuid = {};
  descriptor.element_descriptor_uuid = {};
  descriptor.security_policy_uuid = FixtureUuidOracle(0x64u);
  descriptor.nullable_allowed = true;
  descriptor.descriptor_authoritative = true;
  descriptor.parser_independent = true;
  return descriptor;
}

bool CheckPersistenceDescriptorFields(
    const engine::ExecutionTypeDescriptor& actual,
    const engine::ExecutionTypeDescriptor& expected,
    const std::string& label) {
  Check(EngineUuidEquals(actual.descriptor_uuid, expected.descriptor_uuid),
        label + " descriptor UUID mismatch");
  Check(actual.descriptor_epoch == expected.descriptor_epoch,
        label + " descriptor epoch mismatch");
  Check(actual.canonical_type_id == expected.canonical_type_id,
        label + " canonical type mismatch");
  Check(actual.family == expected.family, label + " family mismatch");
  Check(actual.width_class == expected.width_class,
        label + " width class mismatch");
  Check(actual.stable_name == expected.stable_name,
        label + " stable name mismatch");
  Check(actual.bit_width == expected.bit_width,
        label + " bit width mismatch");
  Check(actual.precision == expected.precision,
        label + " precision mismatch");
  Check(actual.scale == expected.scale, label + " scale mismatch");
  Check(actual.length == expected.length, label + " length mismatch");
  Check(actual.vector_dimensions == expected.vector_dimensions,
        label + " vector dimensions mismatch");
  Check(actual.container_rank == expected.container_rank,
        label + " container rank mismatch");
  Check(actual.modifier_flags == expected.modifier_flags,
        label + " modifier flags mismatch");
  Check(EngineUuidEquals(actual.domain_uuid, expected.domain_uuid),
        label + " domain UUID mismatch");
  Check(actual.domain_stack.size() == expected.domain_stack.size(),
        label + " domain stack length mismatch");
  const std::size_t stack_size =
      std::min(actual.domain_stack.size(), expected.domain_stack.size());
  for (std::size_t index = 0; index < stack_size; ++index) {
    Check(EngineUuidEquals(actual.domain_stack[index],
                           expected.domain_stack[index]),
          label + " domain stack entry " + std::to_string(index) +
              " mismatch");
  }
  Check(EngineUuidEquals(actual.charset_uuid, expected.charset_uuid),
        label + " charset UUID mismatch");
  Check(EngineUuidEquals(actual.collation_uuid, expected.collation_uuid),
        label + " collation UUID mismatch");
  Check(EngineUuidEquals(actual.timezone_uuid, expected.timezone_uuid),
        label + " timezone UUID mismatch");
  Check(EngineUuidEquals(actual.element_descriptor_uuid,
                         expected.element_descriptor_uuid),
        label + " element descriptor UUID mismatch");
  Check(EngineUuidEquals(actual.security_policy_uuid,
                         expected.security_policy_uuid),
        label + " security policy UUID mismatch");
  Check(actual.nullable_allowed == expected.nullable_allowed,
        label + " nullability flag mismatch");
  Check(actual.descriptor_authoritative ==
            expected.descriptor_authoritative,
        label + " descriptor authority flag mismatch");
  Check(actual.parser_independent == expected.parser_independent,
        label + " parser independence flag mismatch");
  return DescriptorEquals(actual, expected);
}

void StoreFixtureUuid(platform::byte* destination,
                      const engine::Uuid& uuid) {
  std::copy(std::begin(uuid.bytes), std::end(uuid.bytes), destination);
}

engine::Uuid LoadFixtureUuid(const platform::byte* source) {
  engine::Uuid uuid;
  std::copy_n(source, sizeof(uuid.bytes), std::begin(uuid.bytes));
  return uuid;
}

template <std::size_t PhysicalBytes>
std::array<platform::byte, kFixtureRecordBytes> MakeFixtureRecord(
    const engine::ExecutionTypeDescriptor& descriptor,
    dt::DatatypePhysicalValueState state,
    const std::array<platform::byte, PhysicalBytes>& physical) {
  std::array<platform::byte, kFixtureRecordBytes> record{};
  constexpr std::array<platform::byte, 8> magic{{
      0x53, 0x42, 0x54, 0x4e, 0x55, 0x4c, 0x30, 0x32}};
  Check(descriptor.stable_name.size() <= kFixtureStableNameCapacity,
        "persistence descriptor stable name exceeds fixture capacity");
  Check(descriptor.domain_stack.size() <= kFixtureDomainStackCapacity,
        "persistence descriptor domain stack exceeds fixture capacity");
  Check(physical.size() <= kFixtureCodecCapacity,
        "physical codec image exceeds fixture capacity");
  if (descriptor.stable_name.size() > kFixtureStableNameCapacity ||
      descriptor.domain_stack.size() > kFixtureDomainStackCapacity ||
      physical.size() > kFixtureCodecCapacity) {
    return record;
  }
  std::copy(magic.begin(), magic.end(), record.begin());
  StoreFixtureUuid(record.data() + kFixtureDescriptorUuidOffset,
                   descriptor.descriptor_uuid);
  platform::StoreLittle64(record.data() + kFixtureDescriptorEpochOffset,
                          descriptor.descriptor_epoch);
  platform::StoreLittle32(record.data() + kFixtureCanonicalTypeOffset,
                          descriptor.canonical_type_id);
  platform::StoreLittle16(record.data() + kFixtureFamilyOffset,
                          static_cast<std::uint16_t>(descriptor.family));
  platform::StoreLittle16(record.data() + kFixtureWidthOffset,
                          static_cast<std::uint16_t>(descriptor.width_class));
  platform::StoreLittle16(
      record.data() + kFixtureStableNameLengthOffset,
      static_cast<std::uint16_t>(descriptor.stable_name.size()));
  std::copy(descriptor.stable_name.begin(), descriptor.stable_name.end(),
            record.begin() + kFixtureStableNameOffset);
  platform::StoreLittle32(record.data() + kFixtureBitWidthOffset,
                          descriptor.bit_width);
  platform::StoreLittle32(record.data() + kFixturePrecisionOffset,
                          descriptor.precision);
  platform::StoreLittle32(record.data() + kFixtureScaleOffset,
                          descriptor.scale);
  platform::StoreLittle32(record.data() + kFixtureLengthOffset,
                          descriptor.length);
  platform::StoreLittle32(record.data() + kFixtureDimensionsOffset,
                          descriptor.vector_dimensions);
  platform::StoreLittle32(record.data() + kFixtureRankOffset,
                          descriptor.container_rank);
  platform::StoreLittle64(record.data() + kFixtureModifierFlagsOffset,
                          descriptor.modifier_flags);
  StoreFixtureUuid(record.data() + kFixtureDomainUuidOffset,
                   descriptor.domain_uuid);
  platform::StoreLittle16(
      record.data() + kFixtureDomainStackCountOffset,
      static_cast<std::uint16_t>(descriptor.domain_stack.size()));
  for (std::size_t index = 0; index < descriptor.domain_stack.size(); ++index) {
    StoreFixtureUuid(record.data() + kFixtureDomainStackOffset + index * 16,
                     descriptor.domain_stack[index]);
  }
  StoreFixtureUuid(record.data() + kFixtureCharsetUuidOffset,
                   descriptor.charset_uuid);
  StoreFixtureUuid(record.data() + kFixtureCollationUuidOffset,
                   descriptor.collation_uuid);
  StoreFixtureUuid(record.data() + kFixtureTimezoneUuidOffset,
                   descriptor.timezone_uuid);
  StoreFixtureUuid(record.data() + kFixtureElementUuidOffset,
                   descriptor.element_descriptor_uuid);
  StoreFixtureUuid(record.data() + kFixtureSecurityUuidOffset,
                   descriptor.security_policy_uuid);
  record[kFixtureNullableOffset] = descriptor.nullable_allowed ? 1 : 0;
  record[kFixtureAuthoritativeOffset] =
      descriptor.descriptor_authoritative ? 1 : 0;
  record[kFixtureParserIndependentOffset] =
      descriptor.parser_independent ? 1 : 0;
  platform::StoreLittle16(record.data() + kFixtureValueStateOffset,
                          static_cast<std::uint16_t>(state));
  platform::StoreLittle32(record.data() + kFixtureCodecLengthOffset,
                          static_cast<std::uint32_t>(physical.size()));
  platform::StoreLittle32(record.data() + kFixtureCodecComplementOffset,
                          ~static_cast<std::uint32_t>(physical.size()));
  std::copy(physical.begin(), physical.end(),
            record.begin() + kFixtureCodecOffset);
  return record;
}

struct ParsedFixtureRecord {
  engine::ExecutionTypeDescriptor descriptor;
  dt::DatatypePhysicalValueState state =
      dt::DatatypePhysicalValueState::unknown;
  const platform::byte* codec_bytes = nullptr;
  std::size_t codec_size = 0;
};

bool ParseFixtureRecord(const platform::byte* bytes, std::size_t size,
                        ParsedFixtureRecord* parsed) {
  if (parsed != nullptr) {
    *parsed = {};
  }
  constexpr std::array<platform::byte, 8> magic{{
      0x53, 0x42, 0x54, 0x4e, 0x55, 0x4c, 0x30, 0x32}};
  if (bytes == nullptr || parsed == nullptr || size != kFixtureRecordBytes ||
      std::memcmp(bytes, magic.data(), magic.size()) != 0) {
    return false;
  }
  const std::uint16_t stable_name_size =
      platform::LoadLittle16(bytes + kFixtureStableNameLengthOffset);
  const std::uint16_t domain_stack_size =
      platform::LoadLittle16(bytes + kFixtureDomainStackCountOffset);
  const std::uint32_t codec_size =
      platform::LoadLittle32(bytes + kFixtureCodecLengthOffset);
  const std::uint32_t complement =
      platform::LoadLittle32(bytes + kFixtureCodecComplementOffset);
  if (stable_name_size > kFixtureStableNameCapacity ||
      domain_stack_size > kFixtureDomainStackCapacity ||
      complement != ~codec_size || codec_size > kFixtureCodecCapacity ||
      bytes[kFixtureNullableOffset] > 1 ||
      bytes[kFixtureAuthoritativeOffset] > 1 ||
      bytes[kFixtureParserIndependentOffset] > 1 ||
      bytes[122] != 0 || bytes[123] != 0 || bytes[239] != 0 ||
      bytes[242] != 0 || bytes[243] != 0) {
    return false;
  }
  if (!std::all_of(bytes + kFixtureStableNameOffset + stable_name_size,
                   bytes + kFixtureBitWidthOffset,
                   [](platform::byte value) { return value == 0; }) ||
      !std::all_of(bytes + kFixtureDomainStackOffset +
                             domain_stack_size * 16,
                   bytes + kFixtureCharsetUuidOffset,
                   [](platform::byte value) { return value == 0; }) ||
      !std::all_of(bytes + kFixtureCodecOffset + codec_size,
                   bytes + kFixtureRecordBytes,
                   [](platform::byte value) { return value == 0; })) {
    return false;
  }
  parsed->descriptor.descriptor_uuid =
      LoadFixtureUuid(bytes + kFixtureDescriptorUuidOffset);
  parsed->descriptor.descriptor_epoch =
      platform::LoadLittle64(bytes + kFixtureDescriptorEpochOffset);
  parsed->descriptor.canonical_type_id =
      platform::LoadLittle32(bytes + kFixtureCanonicalTypeOffset);
  parsed->descriptor.family = static_cast<engine::ExecutionTypeFamily>(
      platform::LoadLittle16(bytes + kFixtureFamilyOffset));
  parsed->descriptor.width_class =
      static_cast<engine::ExecutionTypeWidthClass>(
          platform::LoadLittle16(bytes + kFixtureWidthOffset));
  parsed->descriptor.stable_name.assign(
      reinterpret_cast<const char*>(bytes + kFixtureStableNameOffset),
      stable_name_size);
  parsed->descriptor.bit_width =
      platform::LoadLittle32(bytes + kFixtureBitWidthOffset);
  parsed->descriptor.precision =
      platform::LoadLittle32(bytes + kFixturePrecisionOffset);
  parsed->descriptor.scale =
      platform::LoadLittle32(bytes + kFixtureScaleOffset);
  parsed->descriptor.length =
      platform::LoadLittle32(bytes + kFixtureLengthOffset);
  parsed->descriptor.vector_dimensions =
      platform::LoadLittle32(bytes + kFixtureDimensionsOffset);
  parsed->descriptor.container_rank =
      platform::LoadLittle32(bytes + kFixtureRankOffset);
  parsed->descriptor.modifier_flags =
      platform::LoadLittle64(bytes + kFixtureModifierFlagsOffset);
  parsed->descriptor.domain_uuid =
      LoadFixtureUuid(bytes + kFixtureDomainUuidOffset);
  parsed->descriptor.domain_stack.reserve(domain_stack_size);
  for (std::size_t index = 0; index < domain_stack_size; ++index) {
    parsed->descriptor.domain_stack.push_back(
        LoadFixtureUuid(bytes + kFixtureDomainStackOffset + index * 16));
  }
  parsed->descriptor.charset_uuid =
      LoadFixtureUuid(bytes + kFixtureCharsetUuidOffset);
  parsed->descriptor.collation_uuid =
      LoadFixtureUuid(bytes + kFixtureCollationUuidOffset);
  parsed->descriptor.timezone_uuid =
      LoadFixtureUuid(bytes + kFixtureTimezoneUuidOffset);
  parsed->descriptor.element_descriptor_uuid =
      LoadFixtureUuid(bytes + kFixtureElementUuidOffset);
  parsed->descriptor.security_policy_uuid =
      LoadFixtureUuid(bytes + kFixtureSecurityUuidOffset);
  parsed->descriptor.nullable_allowed = bytes[kFixtureNullableOffset] != 0;
  parsed->descriptor.descriptor_authoritative =
      bytes[kFixtureAuthoritativeOffset] != 0;
  parsed->descriptor.parser_independent =
      bytes[kFixtureParserIndependentOffset] != 0;
  parsed->state = static_cast<dt::DatatypePhysicalValueState>(
      platform::LoadLittle16(bytes + kFixtureValueStateOffset));
  parsed->codec_bytes = bytes + kFixtureCodecOffset;
  parsed->codec_size = codec_size;
  return true;
}

std::uint32_t PhysicalOracleChecksum(std::uint32_t type,
                                     std::uint16_t state,
                                     const platform::byte* payload,
                                     std::size_t payload_size) {
  std::uint32_t checksum = 2166136261u;
  const auto mix = [&checksum](std::uint32_t value) {
    checksum ^= value;
    checksum *= 16777619u;
  };
  mix(type);
  mix(state);
  for (std::size_t index = 0; index < payload_size; ++index) {
    mix(payload[index]);
  }
  return checksum;
}

bool DecodesAsInt64State(const std::array<platform::byte,
                                          kFixtureRecordBytes>& fixture,
                         const engine::ExecutionTypeDescriptor& descriptor,
                         dt::DatatypePhysicalValueState state,
                         const std::vector<platform::byte>& payload,
                         const std::string& label) {
  ParsedFixtureRecord parsed;
  const bool framed = ParseFixtureRecord(fixture.data(), fixture.size(), &parsed);
  Check(framed, label + " test-owned framing refused");
  if (!framed) {
    return false;
  }
  const bool descriptor_equal = CheckPersistenceDescriptorFields(
      parsed.descriptor, descriptor, label + " persisted");
  Check(parsed.state == state, label + " containing value state mismatch");
  const bool exact_descriptor = descriptor_equal && parsed.state == state;
  const auto decoded =
      dt::DecodeDatatypePhysicalValue(parsed.codec_bytes, parsed.codec_size);
  const bool exact = decoded.ok() &&
                     decoded.value.type_id == dt::CanonicalTypeId::int64 &&
                     decoded.value.state == state &&
                     decoded.value.payload == payload;
  Check(exact, label + " production physical decode mismatch");
  return exact_descriptor && exact;
}

bool WriteFixture(disk::FileDevice* device,
                  const std::array<platform::byte,
                                   kFixtureRecordBytes>& fixture,
                  const std::string& label) {
  const auto write = device->WriteAt(0, fixture.data(), fixture.size());
  Check(write.ok() && write.bytes_transferred == fixture.size(),
        label + " full FileDevice write failed");
  if (!write.ok() || write.bytes_transferred != fixture.size()) {
    return false;
  }
  const auto sync = device->Sync();
  Check(sync.ok(), label + " FileDevice sync failed");
  return sync.ok();
}

bool ReadFixture(disk::FileDevice* device,
                 const std::array<platform::byte,
                                  kFixtureRecordBytes>& expected,
                 const engine::ExecutionTypeDescriptor& descriptor,
                 dt::DatatypePhysicalValueState state,
                 const std::vector<platform::byte>& payload,
                 const std::string& label) {
  std::array<platform::byte, kFixtureRecordBytes> actual{};
  const auto read = device->ReadAt(0, actual.data(), actual.size());
  Check(read.ok() && read.bytes_transferred == actual.size(),
        label + " full FileDevice read failed");
  if (!read.ok() || read.bytes_transferred != actual.size()) {
    return false;
  }
  Check(actual == expected, label + " persisted bytes differ from oracle");
  return actual == expected &&
         DecodesAsInt64State(actual, descriptor, state, payload, label);
}

void PhysicalMalformedRecordsAreRefused() {
  const auto descriptor = PersistenceDescriptorOracle();
  const auto null_fixture = MakeFixtureRecord(
      descriptor, dt::DatatypePhysicalValueState::sql_null,
      kInt64NullPhysicalOracle);
  for (std::size_t size = 0; size < kInt64NullPhysicalOracle.size(); ++size) {
    const auto decoded = dt::DecodeDatatypePhysicalValue(
        size == 0 ? nullptr : kInt64NullPhysicalOracle.data(), size);
    CheckRejectedAs(decoded, "SB-DATATYPE-PHYSICAL-TRUNCATED",
                    "truncated physical typed NULL prefix " +
                        std::to_string(size));
    Check(decoded.value.type_id == dt::CanonicalTypeId::unknown &&
              decoded.value.state == dt::DatatypePhysicalValueState::unknown &&
              decoded.value.payload.empty() && decoded.bytes.empty(),
          "truncated physical typed NULL published output");
  }

  auto bad_magic = kInt64NullPhysicalOracle;
  bad_magic[0] ^= 0xff;
  CheckRejectedAs(
      dt::DecodeDatatypePhysicalValue(bad_magic.data(), bad_magic.size()),
      "SB-DATATYPE-PHYSICAL-BAD-MAGIC", "bad physical magic");

  auto bad_flags = kInt64NullPhysicalOracle;
  bad_flags[14] = 1;
  CheckRejectedAs(
      dt::DecodeDatatypePhysicalValue(bad_flags.data(), bad_flags.size()),
      "SB-DATATYPE-PHYSICAL-BAD-FLAGS", "bad physical flags");

  auto bad_length = kInt64NullPhysicalOracle;
  bad_length[16] = 1;
  CheckRejectedAs(
      dt::DecodeDatatypePhysicalValue(bad_length.data(), bad_length.size()),
      "SB-DATATYPE-PHYSICAL-LENGTH-MISMATCH",
      "bad physical payload length");

  auto bad_checksum = kInt64NullPhysicalOracle;
  bad_checksum[20] ^= 1;
  CheckRejectedAs(
      dt::DecodeDatatypePhysicalValue(bad_checksum.data(),
                                      bad_checksum.size()),
      "SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
      "bad physical checksum");

  std::array<platform::byte, 25> payload_null{};
  std::copy(kInt64NullPhysicalOracle.begin(),
            kInt64NullPhysicalOracle.end(), payload_null.begin());
  payload_null[16] = 1;
  payload_null[24] = 0x7f;
  platform::StoreLittle32(
      payload_null.data() + 20,
      PhysicalOracleChecksum(103, 0, payload_null.data() + 24, 1));
  const auto invalid_null = dt::DecodeDatatypePhysicalValue(
      payload_null.data(), payload_null.size());
  CheckRejectedAs(invalid_null, "DATATYPE.NULL_STATE.INVALID",
                  "checksum-valid payload-bearing physical NULL");
  Check(invalid_null.value.type_id == dt::CanonicalTypeId::unknown &&
            invalid_null.value.state == dt::DatatypePhysicalValueState::unknown &&
            invalid_null.value.payload.empty() && invalid_null.bytes.empty(),
        "payload-bearing physical NULL published output");

  auto payload_null_bad_checksum = payload_null;
  payload_null_bad_checksum[20] ^= 1;
  CheckRejectedAs(
      dt::DecodeDatatypePhysicalValue(payload_null_bad_checksum.data(),
                                      payload_null_bad_checksum.size()),
      "SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
      "bad checksum precedes payload-bearing physical NULL state");

  auto standalone_null = kInt64NullPhysicalOracle;
  platform::StoreLittle32(standalone_null.data() + 8, 0);
  platform::StoreLittle32(
      standalone_null.data() + 20,
      PhysicalOracleChecksum(0, 0, nullptr, 0));
  const auto invalid_descriptor = dt::DecodeDatatypePhysicalValue(
      standalone_null.data(), standalone_null.size());
  CheckRejectedAs(invalid_descriptor, "DATATYPE.DESCRIPTOR.INVALID",
                  "checksum-valid standalone physical NULL");
  Check(invalid_descriptor.value.type_id == dt::CanonicalTypeId::unknown &&
            invalid_descriptor.value.state ==
                dt::DatatypePhysicalValueState::unknown &&
            invalid_descriptor.value.payload.empty() &&
            invalid_descriptor.bytes.empty(),
        "standalone physical NULL published output");

  auto standalone_bad_length = standalone_null;
  standalone_bad_length[16] = 1;
  CheckRejectedAs(
      dt::DecodeDatatypePhysicalValue(standalone_bad_length.data(),
                                      standalone_bad_length.size()),
      "SB-DATATYPE-PHYSICAL-LENGTH-MISMATCH",
      "bad framing length precedes standalone physical NULL descriptor");

  auto standalone_bad_checksum = standalone_null;
  standalone_bad_checksum[20] ^= 1;
  CheckRejectedAs(
      dt::DecodeDatatypePhysicalValue(standalone_bad_checksum.data(),
                                      standalone_bad_checksum.size()),
      "SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
      "bad checksum precedes standalone physical NULL descriptor");

  ParsedFixtureRecord parsed;
  Check(!ParseFixtureRecord(null_fixture.data(),
                            kFixtureRecordBytes - 1, &parsed) &&
            parsed.codec_bytes == nullptr && parsed.codec_size == 0,
        "truncated test-owned framing was admitted");
  auto bad_outer_count = null_fixture;
  bad_outer_count[kFixtureCodecLengthOffset] = 0x20;
  Check(!ParseFixtureRecord(bad_outer_count.data(), bad_outer_count.size(),
                            &parsed),
        "test-owned count/complement mismatch was admitted");
  auto oversized_outer = null_fixture;
  platform::StoreLittle32(
      oversized_outer.data() + kFixtureCodecLengthOffset, 33);
  platform::StoreLittle32(
      oversized_outer.data() + kFixtureCodecComplementOffset,
      ~std::uint32_t{33});
  Check(!ParseFixtureRecord(oversized_outer.data(), oversized_outer.size(),
                            &parsed),
        "oversized test-owned codec frame was admitted");
  auto dirty_padding = null_fixture;
  dirty_padding.back() = 1;
  Check(!ParseFixtureRecord(dirty_padding.data(), dirty_padding.size(),
                            &parsed),
        "nonzero test-owned padding was admitted");
  auto oversized_name = null_fixture;
  platform::StoreLittle16(
      oversized_name.data() + kFixtureStableNameLengthOffset,
      kFixtureStableNameCapacity + 1);
  Check(!ParseFixtureRecord(oversized_name.data(), oversized_name.size(),
                            &parsed),
        "oversized test-owned descriptor name was admitted");
  auto oversized_stack = null_fixture;
  platform::StoreLittle16(
      oversized_stack.data() + kFixtureDomainStackCountOffset,
      kFixtureDomainStackCapacity + 1);
  Check(!ParseFixtureRecord(oversized_stack.data(), oversized_stack.size(),
                            &parsed),
        "oversized test-owned domain stack was admitted");
  auto invalid_boolean = null_fixture;
  invalid_boolean[kFixtureAuthoritativeOffset] = 2;
  Check(!ParseFixtureRecord(invalid_boolean.data(), invalid_boolean.size(),
                            &parsed),
        "invalid test-owned descriptor authority flag was admitted");
}

fs::path MakePersistenceFixtureDirectory() {
#ifdef _WIN32
  const auto path = fs::temp_directory_path() /
                    ("sb-base-null-codec-" +
                     std::to_string(::GetCurrentProcessId()) + "-" +
                     std::to_string(reinterpret_cast<std::uintptr_t>(&checks)));
  std::error_code error;
  Check(fs::create_directory(path, error) && !error,
        "create isolated persistence fixture directory");
  return path;
#else
  std::string pattern =
      (fs::temp_directory_path() / "sb-base-null-codec.XXXXXX").string();
  Check(::mkdtemp(pattern.data()) != nullptr,
        "create isolated persistence fixture directory");
  return pattern;
#endif
}

int RunIndependentNullReader(const fs::path& path) {
  failures = 0;
  checks = 0;
  const auto descriptor = PersistenceDescriptorOracle();
  const auto null_fixture = MakeFixtureRecord(
      descriptor, dt::DatatypePhysicalValueState::sql_null,
      kInt64NullPhysicalOracle);
  disk::FileDevice device;
  const auto opened =
      device.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  Check(opened.ok(), "independent reader opened persisted fixture");
  if (opened.ok()) {
    const auto size = device.Size();
    Check(size.ok() && size.size_bytes == kFixtureRecordBytes,
          "independent reader observed exact fixture size");
    ReadFixture(&device, null_fixture, descriptor,
                dt::DatatypePhysicalValueState::sql_null, {},
                "independent reopened typed NULL");
    const auto closed = device.Close();
    Check(closed.ok(), "independent reader closed fixture");
  }
  return failures == 0 ? 0 : 1;
}

bool RunFreshExecutable(const fs::path& executable, const fs::path& path) {
#ifdef _WIN32
  std::wstring command = L"\"" + executable.wstring() +
                         L"\" --reopen-null \"" + path.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &startup, &process) == 0) {
    Check(false, "launch independent reader process");
    return false;
  }
  const DWORD waited = ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = std::numeric_limits<DWORD>::max();
  const BOOL got_exit = ::GetExitCodeProcess(process.hProcess, &exit_code);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  const bool ok = waited == WAIT_OBJECT_0 && got_exit != 0 && exit_code == 0;
  Check(ok, "independent reader process succeeded");
  return ok;
#else
  const pid_t child = ::fork();
  Check(child >= 0, "fork independent reader process");
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    ::execl(executable.c_str(), executable.c_str(), "--reopen-null",
            path.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  const bool ok = ::waitpid(child, &status, 0) == child &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0;
  Check(ok, "independent reader process succeeded");
  return ok;
#endif
}

void PhysicalCodecPersistsThroughFileDevice(const fs::path& executable) {
  const auto descriptor =
      DescriptorFor(dt::CanonicalTypeId::int64, 0x40u, true);
  const auto descriptor_oracle = PersistenceDescriptorOracle();
  CheckPersistenceDescriptorFields(descriptor, descriptor_oracle,
                                   "production persistence descriptor");
  const dt::DatatypePhysicalValue present{
      dt::CanonicalTypeId::int64, dt::DatatypePhysicalValueState::value,
      {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}};
  const dt::DatatypePhysicalValue typed_null{
      dt::CanonicalTypeId::int64,
      dt::DatatypePhysicalValueState::sql_null, {}};
  const auto encoded_present = dt::EncodeDatatypePhysicalValue(present);
  const auto encoded_null = dt::EncodeDatatypePhysicalValue(typed_null);
  Check(encoded_present.ok() &&
            encoded_present.bytes.size() == kInt64PresentPhysicalOracle.size() &&
            std::equal(encoded_present.bytes.begin(), encoded_present.bytes.end(),
                       kInt64PresentPhysicalOracle.begin()),
        "present int64 encoder differs from independent physical oracle");
  Check(encoded_null.ok() &&
            encoded_null.bytes.size() == kInt64NullPhysicalOracle.size() &&
            std::equal(encoded_null.bytes.begin(), encoded_null.bytes.end(),
                       kInt64NullPhysicalOracle.begin()),
        "typed NULL encoder differs from independent physical oracle");
  if (!encoded_present.ok() || !encoded_null.ok()) {
    return;
  }

  const auto present_fixture = MakeFixtureRecord(
      descriptor, dt::DatatypePhysicalValueState::value,
      kInt64PresentPhysicalOracle);
  const auto null_fixture = MakeFixtureRecord(
      descriptor, dt::DatatypePhysicalValueState::sql_null,
      kInt64NullPhysicalOracle);

  DecodesAsInt64State(present_fixture, descriptor_oracle,
                      dt::DatatypePhysicalValueState::value,
                      present.payload, "literal present oracle");
  DecodesAsInt64State(null_fixture, descriptor_oracle,
                      dt::DatatypePhysicalValueState::sql_null, {},
                      "literal typed NULL oracle");

  const fs::path root = MakePersistenceFixtureDirectory();
  if (root.empty()) {
    return;
  }
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  } cleanup{root};
  const fs::path path = root / "typed-null.codec-fixture";

  disk::FileDevice device;
  const auto opened = device.Open(path.string(), disk::FileOpenMode::create_new);
  Check(opened.ok(), "open production FileDevice persistence fixture");
  if (!opened.ok()) {
    return;
  }

  if (WriteFixture(&device, present_fixture, "initial present") &&
      ReadFixture(&device, present_fixture, descriptor_oracle,
                  dt::DatatypePhysicalValueState::value, present.payload,
                  "initial present")) {
    WriteFixture(&device, null_fixture, "present-to-NULL");
    ReadFixture(&device, null_fixture, descriptor_oracle,
                dt::DatatypePhysicalValueState::sql_null, {},
                "present-to-NULL");
    WriteFixture(&device, present_fixture, "NULL-to-present");
    ReadFixture(&device, present_fixture, descriptor_oracle,
                dt::DatatypePhysicalValueState::value, present.payload,
                "NULL-to-present");
    WriteFixture(&device, null_fixture,
                 "final present-to-NULL");
    ReadFixture(&device, null_fixture, descriptor_oracle,
                dt::DatatypePhysicalValueState::sql_null, {},
                "final present-to-NULL");
  }

  const auto size = device.Size();
  Check(size.ok() && size.size_bytes == kFixtureRecordBytes,
        "replacement sequence changed fixed fixture extent");

  std::array<platform::byte, kFixtureRecordBytes + 1> short_read{};
  const auto short_result =
      device.ReadAt(0, short_read.data(), short_read.size());
  Check(!short_result.ok() &&
            short_result.diagnostic.diagnostic_code ==
                "SB-STORAGE-DISK-READ-SHORT" &&
            short_result.bytes_transferred == kFixtureRecordBytes,
        "natural FileDevice short read was not reported exactly");
  Check(std::equal(null_fixture.begin(),
                   null_fixture.end(), short_read.begin()),
        "natural short read changed the persisted prefix");

  const auto failed_write =
      device.WriteAt(0, nullptr, kFixtureRecordBytes);
  Check(!failed_write.ok() &&
            failed_write.diagnostic.diagnostic_code ==
                "SB-STORAGE-DISK-WRITE-BUFFER-NULL" &&
            failed_write.bytes_transferred == 0,
        "preflight failed FileDevice write was not reported exactly");
  ReadFixture(&device, null_fixture, descriptor_oracle,
              dt::DatatypePhysicalValueState::sql_null, {},
              "after preflight failed write");

  const auto closed = device.Close();
  Check(closed.ok(), "close persisted typed NULL before independent reopen");
  if (closed.ok()) {
    RunFreshExecutable(executable, path);
  }
}

void BitStringNullRequiresExactV3Profile() {
  const auto descriptor = platform::Uuid{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x29}};
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV6, 6, 6, descriptor, 1);
  Check(identity.ok, "lookup bit-string V3 identity for typed NULL");
  dt::BitStringAuthorityReceiptV1 receipt{
      TypedObjectUuid(0x62).value, dt::kDatatypeCohortV6, 6, 6};
  const auto profile = dt::BuildBitStringDescriptorProfileV1(
      {receipt, identity.row,
       dt::BitStringSurfaceProfileKindV1::unqualified,
       dt::kBitStringMaximumLogicalBitsV1});
  Check(profile.ok(), "build exact bit-string profile for typed NULL");
  dt::BitStringValueViewV1 typed_null{
      &profile.profile, dt::BitStringValueStateV1::sql_null, 0, {},
      dt::BitStringOwnershipV1::borrowed};
  Check(dt::ValidateBitStringValueViewV1(typed_null, true).ok(),
        "exact-profile typed bit-string NULL was refused");
  Check(!dt::ValidateBitStringValueViewV1(typed_null, false).ok(),
        "nonnullable bit-string NULL was admitted");
  std::array<platform::byte, 1> dirty{0};
  typed_null.packed_msb0 = dirty;
  const auto invalid = dt::ValidateBitStringValueViewV1(typed_null, true);
  Check(!invalid.ok() && invalid.diagnostic.diagnostic_code ==
                             "DATATYPE.NULL_STATE.INVALID",
        "payload-bearing bit-string NULL lost null-state precedence");

  dt::DatatypeOperationValue raw_null{dt::CanonicalTypeId::bit_string, {}, true};
  const auto raw = dt::SerializeDatatypeValue({raw_null});
  Check(!raw.ok() && raw.diagnostic.diagnostic_code ==
                         "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
        "generic SBDV1 admitted typed bit-string NULL without V3 profile");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--reopen-null") {
    return RunIndependentNullReader(argv[2]);
  }
  StandaloneNullIsNotADatatype();
  ContextualBindingPreservesTargetType();
  ExactDescriptorFidelityAndRefusal();
  BoundOperationsRetainConcreteTypeIds();
  DurableCodecsRequireConcreteTypes();
  SerializationRetainsConcreteType();
  PhysicalMalformedRecordsAreRefused();
  PhysicalCodecPersistsThroughFileDevice(fs::absolute(argv[0]));
  BitStringNullRequiresExactV3Profile();
  std::cout << "base NULL contextual-state checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
