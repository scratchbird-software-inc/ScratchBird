// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Component API regression for the admitted explicit UUID/BINARY boundary,
// not UUID PRESENT identity casts, SQL/IPC or comparison qualification.
#include "executor/descriptor_value_runtime.hpp"
#include "internal_api/query/expression_api.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace {
unsigned checks = 0;
void Check(bool passed, const char* detail) {
  ++checks;
  if (!passed) throw std::runtime_error(detail);
}
api::EngineDescriptor Descriptor(const char* type, unsigned ordinal) {
  auto descriptor = exec::MakeExecutorDescriptor(type);
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(2086, ordinal);
  descriptor.descriptor_kind = "scalar";
  descriptor.encoded_descriptor = "nullability=nullable";
  return descriptor;
}
void Preserved(const api::EngineTypedValue& result,
               const api::EngineTypedValue& source,
               const api::EngineDescriptor& target) {
  Check(result.descriptor == target && result.state == source.state &&
            result.is_null == source.is_null &&
            result.binary_value == source.binary_value && result.encoded_value.empty(),
        "cast changed descriptor, native bytes or SQL NULL state");
}
void Both(const api::EngineTypedValue& source, const api::EngineDescriptor& target) {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  const auto direct = exec::CastDescriptorValue(source, target, &diagnostic);
  if (!diagnostic.ok) std::cerr << diagnostic.diagnostic_code << ':' << diagnostic.detail << '\n';
  Check(diagnostic.ok, "bound executor cast refused");
  Preserved(direct, source, target);
  api::EngineTypedValue coerced;
  std::string category, detail;
  Check(api::QowApplyCanonicalDescriptorCoercionV1(
            source, target, true, &coerced, &category, &detail),
        "bound QOW explicit cast refused");
  Preserved(coerced, source, target);
  Check(!category.empty(), "successful QOW cast lost its category");
}
void Reject(const api::EngineTypedValue& source,
            const api::EngineDescriptor& target) {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  const auto direct = exec::CastDescriptorValue(source, target, &diagnostic);
  Check(!diagnostic.ok && !diagnostic.diagnostic_code.empty() &&
            direct.binary_value.empty() && direct.encoded_value.empty(),
        "executor admitted invalid binding or published a partial payload");
  api::EngineTypedValue coerced = source;
  std::string category = "stale", detail;
  Check(!api::QowApplyCanonicalDescriptorCoercionV1(
            source, target, true, &coerced, &category, &detail) &&
            !detail.empty() && category.empty() &&
            coerced.state == api::EngineValueState::error &&
            coerced.binary_value.empty() && coerced.encoded_value.empty(),
        "QOW admitted invalid binding or retained stale output");
}
}

int main() try {
  const auto uuid = Descriptor("uuid", 1), binary = Descriptor("binary", 2);
  auto stale = uuid;
  ++stale.datatype_descriptor_generation;
  auto unbound = uuid;
  unbound.datatype_descriptor_uuid = {};
  for (unsigned pattern = 0; pattern < 130; ++pattern) {
    auto value = exec::MakeExecutorValue(uuid, {}, false);
    value.binary_value.assign(16, 0);
    if (pattern == 1) value.binary_value.assign(16, 0xff);
    if (pattern >= 2) value.binary_value[(pattern - 2) / 8] =
        static_cast<std::uint8_t>(1u << ((pattern - 2) % 8));
    Both(value, binary);
    auto bytes = value;
    bytes.descriptor = binary;
    Both(bytes, uuid);
    Reject(bytes, stale);
    Reject(bytes, unbound);
    auto invalid = value;
    invalid.descriptor = stale;
    Reject(invalid, binary);
    invalid.descriptor = unbound;
    Reject(invalid, binary);
    api::EngineTypedValue result;
    std::string category, detail;
    Check(!api::QowApplyCanonicalDescriptorCoercionV1(
              value, binary, false, &result, &category, &detail) &&
              result.state == api::EngineValueState::error &&
              result.binary_value.empty() && result.encoded_value.empty(),
          "implicit UUID/BINARY cast accepted or published data");
  }
  for (unsigned width : {0u, 15u, 17u}) {
    auto invalid = exec::MakeExecutorValue(uuid, {}, false);
    invalid.binary_value.assign(width, 0);
    Reject(invalid, binary);
    invalid.descriptor = binary;
    Reject(invalid, uuid);
  }
  auto null = exec::MakeExecutorValue(uuid, {}, true);
  Both(null, binary);
  Both(null, uuid);
  Reject(null, stale);
  Reject(null, unbound);
  null.binary_value.push_back(0);
  Reject(null, binary);
  std::cout << "native_uuid_cast_binding checks=" << checks << " patterns=130 failures=0\n";
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << "FAIL check=" << checks << ' ' << error.what() << '\n';
  return EXIT_FAILURE;
}
