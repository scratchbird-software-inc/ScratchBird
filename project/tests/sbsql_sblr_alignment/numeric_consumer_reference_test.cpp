// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Links the production archive: no alternative operation implementation.
#include "datatype_operations.hpp"
#include "sbl_numeric.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace {
long allocation_budget = -1;
unsigned checks = 0, failures = 0;
void Check(bool good, const std::string& message) {
  ++checks;
  if (!good && failures++ < 20) std::cerr << "FAIL " << message << '\n';
}
}
void* operator new(std::size_t n) {
  if (allocation_budget == 0) throw std::bad_alloc();
  if (allocation_budget > 0) --allocation_budget;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace dt = scratchbird::core::datatypes;
namespace numeric = scratchbird::libraries::sbl_numeric;
dt::DatatypeOperationValue Real(std::string text, bool null = false) {
  return {dt::CanonicalTypeId::real128, std::move(text), null};
}
dt::DatatypeNumericOperationRequest Request(
    dt::DatatypeNumericOperationKind op, std::string a, std::string b = "0") {
  dt::DatatypeNumericOperationRequest request;
  request.type_id = dt::CanonicalTypeId::real128;
  request.operation = op;
  request.left = Real(std::move(a));
  request.right = Real(std::move(b));
  return request;
}
bool SameFacts(const dt::DatatypeNumericFacts& got, const numeric::NumericResult& expected) {
  return got.inexact == expected.inexact && got.underflow == expected.underflow &&
      got.overflow == expected.overflow && got.invalid == expected.invalid &&
      got.divide_by_zero == expected.divide_by_zero && got.subnormal == expected.subnormal &&
      got.unordered == (expected.status == numeric::NumericStatusCode::unordered);
}
std::string Canonical(const std::string& text) {
  numeric::NumericRequest request;
  request.type = numeric::NumericType::real128;
  request.left = {numeric::NumericType::real128, text, false};
  const auto got = numeric::ApplyNumericOperation(request);
  Check(got.status == numeric::NumericStatusCode::ok, "reference fixture conversion");
  return got.value.encoded;
}

void PublicOwners() {
  // The old archive cannot jointly link the name helper and ordinary numeric
  // operations because that pulls two strong definitions of five APIs.
  Check(std::string(dt::DatatypeNullOrderingName(dt::DatatypeNullOrdering::nulls_first)) ==
        "nulls_first", "null ordering helper");
  Check(std::string(dt::DatatypeNullOrderingName(dt::DatatypeNullOrdering::nulls_last)) ==
        "nulls_last", "null ordering helper last");
  auto numeric = dt::ApplyNumericOperation(Request(dt::DatatypeNumericOperationKind::add, "1", "2"));
  Check(numeric.ok() && numeric.value.encoded_value == "3", "ordinary numeric owner");
  dt::DatatypeSortKeyRequest key;
  key.value = {dt::CanonicalTypeId::int32, "7", false};
  Check(dt::MakeDatatypeSortKey(key).ok(), "sort owner linked");
  dt::DatatypeHashRequest hash;
  hash.value = key.value;
  Check(dt::HashDatatypeValue(hash).ok(), "hash owner linked");
  dt::DatatypeSerializationRequest serialize;
  serialize.value = key.value;
  const auto encoded = dt::SerializeDatatypeValue(serialize);
  dt::DatatypeDeserializationRequest deserialize;
  deserialize.expected_type_id = key.value.type_id;
  deserialize.serialized_value = encoded.serialized_value;
  const auto decoded = dt::DeserializeDatatypeValue(deserialize);
  Check(encoded.ok() && decoded.ok() && decoded.value.encoded_value == "7",
        "serialization owners linked and preserve value");
  // These linkage calls do NOT qualify the retained prototype sort/hash/
  // serialization formats as canonical durable or wire representations.
}

void PrecisionAndContext() {
  std::vector<std::string> values;
  for (unsigned i = 0; i < 64; ++i) {
    const char* digits = "0123456789abcdef";
    // Exactly 1 + i * 2^-112; ordering oracle is the integer index, not a
    // host float conversion or a second comparison implementation.
    std::string text = "0x1." + std::string(26, '0');
    text += digits[i >> 4]; text += digits[i & 15]; text += "p0";
    values.push_back(Canonical(text));
  }
  for (unsigned i = 0; i < values.size(); ++i) {
    for (unsigned j = 0; j < values.size(); ++j) {
      dt::DatatypeComparisonRequest request;
      request.left = Real(values[i]); request.right = Real(values[j]);
      const auto result = dt::CompareDatatypeValues(request);
      Check(result.ok() && result.comparison == (i < j ? -1 : i > j ? 1 : 0),
            "113-bit comparison at indices " + std::to_string(i) + "," + std::to_string(j));
    }
  }
  dt::DatatypeComparisonRequest request;
  request.left = Real("0x1.00000000000000000000000000008p0");
  request.right = Real("1");
  auto result = dt::CompareDatatypeValues(request);
  Check(result.ok() && result.comparison == 0 && result.numeric_facts.inexact,
        "comparison ties-even context and conversion fact");
  request.numeric_context.rounding = dt::DatatypeRoundingMode::half_up;
  result = dt::CompareDatatypeValues(request);
  Check(result.ok() && result.comparison == 1 && result.numeric_facts.inexact,
        "comparison ties-away context");
  request.left = Real("NaN");
  request.numeric_context.allow_special_values = true;
  result = dt::CompareDatatypeValues(request);
  Check(!result.ok() && result.numeric_facts.unordered && !result.numeric_facts.invalid &&
        result.diagnostic.diagnostic_code == "NUMERIC.REAL128.INVALID", "unordered is not equal");
  request.left = Real("sNaN");
  result = dt::CompareDatatypeValues(request);
  Check(!result.ok() && result.numeric_facts.invalid && !result.numeric_facts.unordered,
        "signaling comparison invalid");
  request.left = Real("Infinity");
  result = dt::CompareDatatypeValues(request);
  Check(result.ok() && result.comparison == 1, "admitted infinity comparison");
  request.numeric_context.allow_special_values = false;
  Check(!dt::CompareDatatypeValues(request).ok(), "special comparison policy");
}

void AdapterFacts() {
  struct Case { dt::DatatypeNumericOperationKind op; const char* a; const char* b; bool special; };
  for (const auto& entry : std::vector<Case>{
      {dt::DatatypeNumericOperationKind::canonicalize, "0.1", "0", false},
      {dt::DatatypeNumericOperationKind::canonicalize, "0x1p-16494", "0", false},
      {dt::DatatypeNumericOperationKind::canonicalize, "0x1p-16495", "0", false},
      {dt::DatatypeNumericOperationKind::canonicalize, "0x1p16384", "0", false},
      {dt::DatatypeNumericOperationKind::add, "1", "0x1.8p-112", false},
      {dt::DatatypeNumericOperationKind::subtract, "1", "1", false},
      {dt::DatatypeNumericOperationKind::multiply, "0x1p-16494", "0.5", false},
      {dt::DatatypeNumericOperationKind::divide, "1", "3", false},
      {dt::DatatypeNumericOperationKind::divide, "1", "0", false},
      {dt::DatatypeNumericOperationKind::divide, "0", "0", false},
      {dt::DatatypeNumericOperationKind::compare, "NaN", "1", true},
      {dt::DatatypeNumericOperationKind::add, "sNaN", "1", true},
      {dt::DatatypeNumericOperationKind::add, "0x1p-16494", "bad", false}}) {
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto request = Request(entry.op, entry.a, entry.b);
      request.context.allow_special_values = entry.special;
      request.context.rounding = static_cast<dt::DatatypeRoundingMode>(mode);
      numeric::NumericRequest backend;
      backend.type = numeric::NumericType::real128;
      backend.operation = static_cast<numeric::NumericOperation>(entry.op);
      backend.left = {backend.type, entry.a, false}; backend.right = {backend.type, entry.b, false};
      backend.context.allow_special_values = entry.special;
      backend.context.rounding = static_cast<numeric::RoundingMode>(mode);
      const auto expected = numeric::ApplyNumericOperation(backend);
      const auto actual = dt::ApplyNumericOperation(request);
      Check(actual.ok() == (expected.status == numeric::NumericStatusCode::ok), "adapter status");
      Check(SameFacts(actual.numeric_facts, expected), "adapter retains every numeric fact");
      if (actual.ok()) Check(actual.value.encoded_value == expected.value.encoded, "adapter value");
      else Check(actual.diagnostic.diagnostic_code == expected.diagnostic_code, "adapter diagnostic code");
    }
  }
}

void StructuralValidation() {
  for (bool null : {false, true}) {
    auto request = Request(dt::DatatypeNumericOperationKind::add, "1", "2");
    request.left.is_null = null;
    request.context.rounding = static_cast<dt::DatatypeRoundingMode>(99);
    auto result = dt::ApplyNumericOperation(request);
    Check(!result.ok() && result.numeric_facts.invalid &&
          result.diagnostic.diagnostic_code == "NUMERIC.REAL128.INVALID", "unknown rounding rejected");
    request.context.rounding = dt::DatatypeRoundingMode::half_even;
    request.operation = static_cast<dt::DatatypeNumericOperationKind>(99);
    result = dt::ApplyNumericOperation(request);
    Check(!result.ok() && result.numeric_facts.invalid, "unknown operation rejected");
    request.operation = dt::DatatypeNumericOperationKind::add;
    for (bool left : {false, true}) {
      auto mismatch = request;
      (left ? mismatch.left : mismatch.right).type_id = dt::CanonicalTypeId::decimal;
      result = dt::ApplyNumericOperation(mismatch);
      Check(!result.ok() && result.numeric_facts.invalid, "argument type not overwritten");
    }
  }
  auto null = Request(dt::DatatypeNumericOperationKind::add, "invalid", "invalid");
  null.left.is_null = true;
  const auto result = dt::ApplyNumericOperation(null);
  Check(result.ok() && result.value.is_null && !result.numeric_facts.invalid, "strict null suppresses parsing");
  Check(std::string(dt::DatatypeNumericOperationKindName(static_cast<dt::DatatypeNumericOperationKind>(99))) ==
        "unknown", "unknown operation name");
  Check(std::string(dt::DatatypeRoundingModeName(static_cast<dt::DatatypeRoundingMode>(99))) ==
        "unknown", "unknown rounding name");
  Check(std::string(dt::DatatypeNullOrderingName(static_cast<dt::DatatypeNullOrdering>(99))) ==
        "unknown", "unknown null ordering name");
  dt::DatatypeComparisonRequest compare;
  compare.left = Real("0", true); compare.right = Real("1");
  for (const auto mode : {dt::DatatypeNullOrdering::nulls_first, dt::DatatypeNullOrdering::nulls_last}) {
    compare.null_ordering = mode;
    const auto ordered = dt::CompareDatatypeValues(compare);
    Check(ordered.ok() && ordered.comparison == (mode == dt::DatatypeNullOrdering::nulls_first ? -1 : 1),
          "explicit generic null placement");
  }
  compare.null_ordering = static_cast<dt::DatatypeNullOrdering>(99);
  Check(!dt::CompareDatatypeValues(compare).ok(), "invalid null placement rejected");
  dt::DatatypeSortKeyRequest key;
  key.value = compare.left; key.null_ordering = compare.null_ordering;
  Check(!dt::MakeDatatypeSortKey(key).ok(), "invalid null sort placement rejected");
}

void AllocationFailure() {
  const auto request = Request(dt::DatatypeNumericOperationKind::divide,
      "1.23456789012345678901234567890123456", "3");
  unsigned faults = 0;
  bool success = false;
  for (long budget = 0; budget < 300; ++budget) {
    allocation_budget = budget;
    try {
      const auto result = dt::ApplyNumericOperation(request);
      allocation_budget = -1;
      Check(result.ok() && result.numeric_facts.inexact, "allocation sweep result");
      success = true;
    } catch (const std::bad_alloc&) {
      allocation_budget = -1;
      ++faults;
    }
    Check(request.left.encoded_value == "1.23456789012345678901234567890123456" &&
          request.right.encoded_value == "3", "allocation fault preserves operands");
    if (success) break;
  }
  Check(success && faults > 3, "Core and backend C++ allocations swept");
  std::cout << "cpp_allocation_faults=" << faults << '\n';
}
}
int main() {
  PublicOwners();
  PrecisionAndContext();
  AdapterFacts();
  StructuralValidation();
  AllocationFailure();
  numeric::ReleaseReal128ThreadCache();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
