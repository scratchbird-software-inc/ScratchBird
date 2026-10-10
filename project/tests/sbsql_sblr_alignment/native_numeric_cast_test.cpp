// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "registry/function_seed_registry.hpp"
#include "dispatch/function_dispatch.hpp"
#include "common/function_result_helpers.hpp"
#include "sblr/sblr_special_forms.hpp"
#include "sblr/sblr_projection_value_runtime.hpp"
#include "executor/descriptor_value_runtime.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <bit>
#include <cfenv>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace s = scratchbird::engine::sblr;
namespace f = scratchbird::engine::functions;
namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* detail) {
  ++checks;
  if (!condition) throw std::runtime_error(detail);
}
std::shared_ptr<const api::EngineDescriptor> Binding(const char* name, bool nullable = true) {
  auto descriptor = ex::MakeExecutorDescriptor(name);
  descriptor.descriptor_kind = "scalar";
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(2110, name[0] == 'i' ? 1 : 2);
  descriptor.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
  return std::make_shared<const api::EngineDescriptor>(std::move(descriptor));
}
s::SblrValue Integer(std::int64_t number) {
  auto value = f::MakeInt64Value("int64", number);
  value.projection_descriptor = Binding("int64");
  return value;
}
s::SblrValue Real(std::uint64_t bits) {
  s::SblrValue value;
  value.descriptor_id = "real64";
  value.payload_kind = s::SblrValuePayloadKind::real64;
  value.real64_value = std::bit_cast<double>(bits);
  value.has_real64_value = true;
  value.is_null = false;
  value.projection_descriptor = Binding("real64");
  return value;
}
s::SblrResult Cast(const f::FunctionRegistry& registry, const s::SblrValue& value,
                   const std::shared_ptr<const api::EngineDescriptor>& target, bool callable) {
  if (!callable)
    return s::EvaluateSblrCastForm("native.cast", value, target->canonical_type_name,
                                 {}, true, false, target);
  const auto* entry = registry.Lookup("data.scalar.cast");
  Check(entry != nullptr, "registered CAST is missing");
  f::FunctionCallRequest request;
  request.context.function_uuid = entry->function_uuid;
  request.context.security_allowed = request.context.policy_allowed = true;
  Check(registry.BindCallContext(request.context) != nullptr, "binary callable binding failed");
  request.result_descriptor = target;
  request.arguments = {{"source", value},
      {"target", f::MakeTextValue("text", target->canonical_type_name)}};
  return f::DispatchFunctionCall(registry, std::move(request)).result;
}
const s::SblrValue& Scalar(const s::SblrResult& result) {
  if (!result.ok()) for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.diagnostic_id << ':' << diagnostic.detail << '\n';
    for (const auto& field : diagnostic.fields)
      if (const auto* text = std::get_if<std::string>(&field.value))
        std::cerr << field.key << '=' << *text << '\n';
  }
  Check(result.ok() && result.scalar_values.size() == 1 && result.rows.empty() &&
        !result.mutation_attempted && !result.mutation_committed, "cast did not return one scalar");
  return result.scalar_values.front();
}
void Published(const s::SblrValue& value, std::uint64_t bits,
               const std::shared_ptr<const api::EngineDescriptor>& target) {
  Check(value.projection_descriptor == target && value.encoded_value.empty() &&
        value.text_value.empty() && value.binary_value.empty() &&
        (target->canonical_type_name == "int64" ? s::SblrInt64PayloadValid(value)
                                               : s::SblrReal64PayloadValid(value)),
        "cast lost binding or published a text shadow");
  const auto output = s::EngineTypedValueFromSblrValue(value);
  Check(output.descriptor == *target && output.encoded_value.empty() && output.binary_value.size() == 8,
        "projection adapter lost exact descriptor/native width");
  for (unsigned byte = 0; byte < 8; ++byte)
    Check(output.binary_value[byte] == static_cast<std::uint8_t>(bits >> (8 * byte)),
          "projection adapter changed a native result bit");
}
void Refused(const s::SblrResult& result) {
  Check(!result.ok() && !result.diagnostics.empty() && result.scalar_values.empty() &&
        result.rows.empty() && !result.mutation_attempted && !result.mutation_committed,
        "invalid cast produced a value or lacked diagnostics");
}
void ConversionRefused(const s::SblrResult& result, const char* code,
                       bool overflow, bool invalid, bool inexact) {
  Refused(result);
  Check(result.diagnostics.front().diagnostic_id == code &&
        result.numeric_facts.overflow == overflow &&
        result.numeric_facts.invalid == invalid &&
        result.numeric_facts.inexact == inexact,
        "conversion failed for the wrong reason or lost its numeric facts");
}
}  // namespace

int main() try {
  const auto package = f::BuildStandardFunctionSeedPackage();
  const auto integer = Binding("int64"), real = Binding("real64");
  for (const bool callable : {false, true}) {
    for (const auto number : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                              std::int64_t{0}, std::int64_t{42},
                              std::numeric_limits<std::int64_t>::max()}) {
      const auto result = Cast(package.registry, Integer(number), integer, callable);
      Published(Scalar(result), std::bit_cast<std::uint64_t>(number), integer);
    }
    // Independent bit-pattern oracles, including subnormal, signed zero and
    // exact finite endpoints; no decimal-format round trip in the test oracle.
    for (const auto bits : {0ULL, 0x8000000000000000ULL, 1ULL, 0x8000000000000001ULL,
                            0x3ff0000000000000ULL, 0x7fefffffffffffffULL,
                            0xffefffffffffffffULL}) {
      const auto result = Cast(package.registry, Real(bits), real, callable);
      Published(Scalar(result), bits, real);
    }
    auto result = Cast(package.registry, Integer(42), real, callable);
    Published(Scalar(result), 0x4045000000000000ULL, real);
    Check(!result.numeric_facts.inexact && !result.numeric_facts.invalid,
          "exact conversion manufactured numeric facts");
    auto nonnullable = Integer(42);
    nonnullable.projection_descriptor = Binding("int64", false);
    result = Cast(package.registry, nonnullable, real, callable);
    Published(Scalar(result), 0x4045000000000000ULL, real);
    result = Cast(package.registry, Real(0xc045000000000000ULL), integer, callable);
    Published(Scalar(result), std::bit_cast<std::uint64_t>(std::int64_t{-42}), integer);
    result = Cast(package.registry, Real(0xc3e0000000000000ULL), integer, callable);
    Published(Scalar(result), 0x8000000000000000ULL, integer);
    const auto rounding = std::fegetround();
    std::fesetround(FE_UPWARD);
    result = Cast(package.registry, Integer(9007199254740993LL), real, callable);
    std::fesetround(rounding);
    Published(Scalar(result), 0x4340000000000000ULL, real);
    Check(result.numeric_facts.inexact && !result.numeric_facts.invalid &&
          !result.numeric_facts.overflow, "successful rounding lost its inexact fact");
    ConversionRefused(Cast(package.registry, Real(0x3ff8000000000000ULL), integer, callable),
                      "NUMERIC.REAL64.INVALID", false, true, true);
    for (const auto bits : {0x43e0000000000000ULL, 0xc3e0000000000001ULL})
      ConversionRefused(Cast(package.registry, Real(bits), integer, callable),
                        "NUMERIC.REAL64.OVERFLOW", true, false, false);
    for (const auto bits : {0x7ff0000000000000ULL, 0x7ff8000000000001ULL})
      ConversionRefused(Cast(package.registry, Real(bits), integer, callable),
                        "NUMERIC.REAL64.INVALID", false, true, false);
    for (const char* source : {"int64", "real64"}) {
      auto null = f::MakeNullValue(source);
      null.projection_descriptor = Binding(source);
      for (const auto& target : {integer, real}) {
        result = Cast(package.registry, null, target, callable);
        Check(s::SblrNullPayloadEmpty(Scalar(result)) &&
              Scalar(result).projection_descriptor == target, "typed NULL lost binding/state");
        Refused(Cast(package.registry, null, Binding(target->canonical_type_name.c_str(), false), callable));
      }
    }
    for (unsigned variant = 0; variant < 13; ++variant) {
      auto bad = Real(0x3ff0000000000000ULL);
      auto binding = *real;
      switch (variant) {
        case 0: bad.text_value = "1"; break;
        case 1: bad.encoded_value = "1"; break;
        case 2: bad.binary_value = {0}; break;
        case 3: bad.has_int64_value = true; break;
        case 4: bad.has_uint64_value = true; break;
        case 5: bad.has_real64_value = false; break;
        case 6: bad.charset_name = "UTF-8"; break;
        case 7: bad.projection_descriptor.reset(); break;
        case 8: binding.type_uuid = {}; break;
        case 9: ++binding.datatype_descriptor_generation; break;
        case 10: binding.canonical_type_name = "int64"; break;
        case 11: binding.encoded_descriptor += ";precision=12"; break;
        case 12: bad.is_null = true; break;
      }
      if (variant >= 8 && variant <= 11)
        bad.projection_descriptor = std::make_shared<const api::EngineDescriptor>(binding);
      Refused(Cast(package.registry, bad, integer, callable));
    }
    auto bad_target = *real; bad_target.type_uuid = {};
    Refused(Cast(package.registry, Integer(1), std::make_shared<const api::EngineDescriptor>(bad_target), callable));
  }
  std::cout << "native numeric cast checks=" << checks << " failures=0\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
