// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "query/expression_api.hpp"
#include "uuid.hpp"
#include "sbl_numeric.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {
long allocation_budget = -1;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok && failures++ < 20) std::cerr << "FAIL " << message << '\n';
}
}
void* operator new(std::size_t size) {
  if (allocation_budget == 0) throw std::bad_alloc();
  if (allocation_budget > 0) --allocation_budget;
  if (void* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
using Op = dt::DatatypeNumericOperationKind;
using Mode = dt::DatatypeRoundingMode;
using State = api::EngineValueState;
using Bytes = scratchbird::libraries::sbl_numeric::Real128Bytes;
static_assert(std::is_nothrow_move_assignable_v<api::EngineTypedValue>);
Bytes Bits(unsigned exponent, unsigned low = 0) {
  Bytes bits{};
  bits[0] = static_cast<std::uint8_t>(low);
  bits[14] = static_cast<std::uint8_t>(exponent);
  bits[15] = static_cast<std::uint8_t>(exponent >> 8);
  return bits;
}
api::EngineDescriptor Descriptor() {
  api::EngineDescriptor out;
  const auto identity = scratchbird::core::uuid::GenerateCompatibilityUnixTimeV7(1789310000000ULL);
  const auto type = scratchbird::core::uuid::GenerateCompatibilityUnixTimeV7(1789310000000ULL);
  if (!identity.ok() || !type.ok()) throw std::runtime_error("fixture UUID generation failed");
  out.descriptor_uuid = identity.value;
  out.type_uuid = type.value;
  out.descriptor_kind = "scalar";
  out.canonical_type_name = "real128";
  out.encoded_descriptor = "width=128;nullability=nullable;fixture=typed-binary128";
  return out;
}
api::EngineTypedValue Value(const api::EngineDescriptor& descriptor, Bytes bits) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.binary_value.assign(bits.begin(), bits.end());
  value.setState(State::value);
  return value;
}
bool Equal(const api::EngineTypedValue& value, Bytes bits) {
  return value.state == State::value && !value.is_null && value.encoded_value.empty() &&
      value.binary_value == std::vector<std::uint8_t>(bits.begin(), bits.end());
}
bool Failed(const api::EngineTypedValue& value) {
  return value.state == State::error && !value.is_null &&
      value.binary_value.empty() && value.encoded_value.empty();
}
void Execution() {
  const auto descriptor = Descriptor();
  const auto one = Value(descriptor, Bits(16383));
  const auto half_ulp = Value(descriptor, Bits(16383 - 113));
  const auto zero = Value(descriptor, Bits(0));
  api::EngineTypedValue out;
  std::string detail;
  dt::DatatypeNumericFacts facts;
  dt::DatatypeNumericContext context;
  for (const auto mode : {Mode::half_even, Mode::half_up, Mode::truncate}) {
    context.rounding = mode;
    Check(api::QowApplyCanonicalNumericScalarV1(one, half_ulp, descriptor, Op::add,
        context, &out, &detail, &facts), "typed arithmetic executes");
    Check(Equal(out, Bits(16383, mode == Mode::half_up ? 1 : 0)) && facts.inexact &&
        !facts.invalid && detail.empty(), "typed rounding and facts");
    Check(out.descriptor == descriptor, "all descriptor fields retained");
    const auto minsub = Value(descriptor, Bits(0, 1));
    const auto half = Value(descriptor, Bits(16382));
    Check(api::QowApplyCanonicalNumericScalarV1(minsub, half, descriptor, Op::multiply,
        context, &out, &detail, &facts) && facts.underflow && facts.inexact &&
        Equal(out, Bits(0, mode == Mode::half_up ? 1 : 0)), "typed subnormal tie");
  }
  Check(api::QowApplyCanonicalNumericScalarV1(one, {}, descriptor, Op::canonicalize,
      context, &out, &detail, &facts) && Equal(out, Bits(16383)), "unary needs no right operand");
  auto alias = one;
  Check(api::QowApplyCanonicalNumericScalarV1(alias, one, alias.descriptor, Op::add,
      context, &alias, &detail, &facts) && Equal(alias, Bits(16384)), "output may alias left and descriptor");
  alias = one;
  Check(api::QowApplyCanonicalNumericScalarV1(one, alias, alias.descriptor, Op::add,
      context, &alias, &detail, &facts) && Equal(alias, Bits(16384)), "output may alias right");
  Check(!api::QowApplyCanonicalNumericScalarV1(one, zero, descriptor, Op::divide,
      context, &out, &detail, &facts) && Failed(out) && facts.divide_by_zero &&
      detail == "NUMERIC.REAL128.DIVIDE_BY_ZERO", "division has failure facts without payload");
  auto maximum = Bits(0x7ffe);
  for (unsigned i = 0; i < 14; ++i) maximum[i] = 0xff;
  Check(!api::QowApplyCanonicalNumericScalarV1(Value(descriptor, maximum), Value(descriptor, Bits(16384)),
      descriptor, Op::multiply, context, &out, &detail, &facts) && Failed(out) && facts.overflow &&
      detail == "NUMERIC.REAL128.OVERFLOW", "overflow preserves code and no payload");
  auto nan_bits = Bits(0xffff, 0x57); nan_bits[13] = 0x80;
  auto nan = Value(descriptor, nan_bits);
  Check(!api::QowApplyCanonicalNumericScalarV1(nan, one, descriptor, Op::add,
      context, &out, &detail, &facts) && facts.invalid && Failed(out), "special policy not overridden");
  context.allow_special_values = true;
  Check(api::QowApplyCanonicalNumericScalarV1(nan, one, descriptor, Op::add,
      context, &out, &detail, &facts) && Equal(out, nan_bits), "typed NaN payload retained");
  nan_bits[13] = 0;
  nan = Value(descriptor, nan_bits);
  Check(!api::QowApplyCanonicalNumericScalarV1(nan, one, descriptor, Op::add,
      context, &out, &detail, &facts) && facts.invalid && Failed(out), "typed signaling NaN fails");
  Check(api::QowApplyCanonicalNumericScalarV1(nan, {}, descriptor, Op::canonicalize,
      context, &out, &detail, &facts) && Equal(out, nan_bits), "typed signaling NaN codec is lossless");
}
void InvalidAndNull() {
  const auto descriptor = Descriptor();
  const auto one = Value(descriptor, Bits(16383));
  auto null = one; null.binary_value.clear(); null.setState(State::sql_null);
  api::EngineTypedValue out;
  std::string detail;
  dt::DatatypeNumericFacts facts;
  dt::DatatypeNumericContext context;
  for (unsigned variant = 0; variant < 11; ++variant) {
    auto left = one;
    if (variant == 0) left.encoded_value = "1";
    if (variant == 1) left.binary_value.pop_back();
    if (variant == 2) left.binary_value.push_back(0);
    if (variant == 3) left.descriptor.type_uuid = {};
    if (variant == 4) left.descriptor.descriptor_uuid.bytes[6] = 0x40;
    if (variant == 5) left.descriptor.canonical_type_name = "int128";
    if (variant == 6) left.descriptor.encoded_descriptor = "width=64";
    if (variant == 7) left.descriptor.encoded_descriptor = "width=128;width=128";
    if (variant == 8) left.setState(State::protected_value);
    if (variant == 9) left.is_null = true;
    if (variant == 10) { left.state = State::sql_null; left.binary_value.clear(); }
    out = one;
    Check(!api::QowApplyCanonicalNumericScalarV1(left, null, descriptor, Op::add,
        context, &out, &detail, &facts) && Failed(out) && facts.invalid,
        "NULL does not bypass required structural validation");
  }
  Check(api::QowApplyCanonicalNumericScalarV1(null, one, descriptor, Op::add,
      context, &out, &detail, &facts) && out.state == State::sql_null && out.is_null &&
      out.binary_value.empty() && out.encoded_value.empty() && out.descriptor == descriptor,
      "strict typed NULL preserves descriptor");
  auto nonnull = descriptor; nonnull.encoded_descriptor = "width=128;nullability=non_null";
  Check(!api::QowApplyCanonicalNumericScalarV1(null, one, nonnull, Op::add,
      context, &out, &detail, &facts) && Failed(out), "NULL respects result nullability");
  Check(!api::QowApplyCanonicalNumericScalarV1(null, null, descriptor, static_cast<Op>(99),
      context, &out, &detail, &facts) && Failed(out), "NULL does not bypass operation validation");
  context.rounding = static_cast<Mode>(99);
  Check(!api::QowApplyCanonicalNumericScalarV1(null, null, descriptor, Op::add,
      context, &out, &detail, &facts) && Failed(out), "NULL does not bypass context validation");
}
void Consumers() {
  api::EngineCanonicalExpressionEvaluationRequest request;
  request.result_descriptor = Descriptor();
  request.left_value = Value(request.result_descriptor, Bits(16383));
  request.right_value = Value(request.result_descriptor, Bits(16383 - 113));
  request.operation = api::EngineCanonicalExpressionOperation::numeric_add;
  request.numeric_context.rounding = Mode::half_up;
  for (auto consumer : {api::EngineCanonicalExpressionConsumer::filter,
      api::EngineCanonicalExpressionConsumer::projection, api::EngineCanonicalExpressionConsumer::join,
      api::EngineCanonicalExpressionConsumer::aggregate, api::EngineCanonicalExpressionConsumer::window,
      api::EngineCanonicalExpressionConsumer::subquery}) {
    request.consumer = consumer;
    api::EngineCanonicalExpressionEvaluationResult result;
    std::string detail;
    Check(api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) &&
        Equal(result.value, Bits(16383, 1)) && result.numeric_facts.inexact,
        "each typed expression consumer retains bytes and facts");
  }
  request.right_value = Value(request.result_descriptor, Bits(0));
  request.operation = api::EngineCanonicalExpressionOperation::numeric_divide;
  api::EngineCanonicalExpressionEvaluationResult result;
  std::string detail;
  Check(!api::QowEvaluateCanonicalTypedExpressionV1(request, &result, &detail) && Failed(result.value) &&
      result.numeric_facts.divide_by_zero && result.diagnostic_id == "NUMERIC.REAL128.DIVIDE_BY_ZERO",
      "expression failure retains actual numeric diagnostic and facts");
}
void AllocationFailure() {
  const auto descriptor = Descriptor();
  const auto one = Value(descriptor, Bits(16383));
  unsigned faults = 0;
  for (bool alias : {false, true}) {
    bool success = false;
    for (long budget = 0; budget < 100; ++budget) {
      auto out = one;
      std::string detail;
      dt::DatatypeNumericFacts facts;
      allocation_budget = budget;
      try {
        const bool ok = api::QowApplyCanonicalNumericScalarV1(alias ? out : one, one,
            alias ? out.descriptor : descriptor, Op::add, {}, &out, &detail, &facts);
        allocation_budget = -1;
        Check(ok && Equal(out, Bits(16384)), "allocation sweep succeeds");
        success = true;
      } catch (const std::bad_alloc&) {
        allocation_budget = -1;
        ++faults;
        Check(Failed(out), "allocation unwind publishes no stale value");
      }
      if (success) break;
    }
    Check(success, "allocation sweep terminates");
  }
  Check(faults >= 4, "descriptor and result allocation faults exercised");
  std::cout << "cpp_allocation_faults=" << faults << '\n';
}
}
int main() {
  Execution(); InvalidAndNull(); Consumers(); AllocationFailure();
  scratchbird::libraries::sbl_numeric::ReleaseReal128ThreadCache();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
