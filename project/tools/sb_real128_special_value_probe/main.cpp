// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"

#include <iostream>

using namespace scratchbird::core::datatypes;

namespace {

bool Expect(bool condition, const char* name) {
  std::cout << "  \"" << name << "\": " << (condition ? "true" : "false") << ",\n";
  return condition;
}

DatatypeCastResult CastReal128(const char* value) {
  DatatypeCastRequest request;
  request.value = {CanonicalTypeId::character, value, false};
  request.target_type_id = CanonicalTypeId::real128;
  request.explicit_cast = true;
  return CastDatatypeValue(request);
}

DatatypeNumericOperationResult CompareReal128(const char* left, const char* right) {
  DatatypeNumericOperationRequest request;
  request.operation = DatatypeNumericOperationKind::compare;
  request.type_id = CanonicalTypeId::real128;
  request.left = {CanonicalTypeId::real128, left, false};
  request.right = {CanonicalTypeId::real128, right, false};
  return ApplyNumericOperation(request);
}

}  // namespace

int main() {
  auto positive_zero = CastReal128("+0.0");
  auto negative_zero = CastReal128("-0.0");
  auto infinity = CastReal128("+inf");
  auto negative_infinity = CastReal128("-Infinity");
  auto quiet_nan = CastReal128("NaN");
  auto signaling_nan = CastReal128("sNaN");
  auto invalid = CastReal128("not-a-number");

  auto infinity_compare = CompareReal128("Infinity", "1");
  auto negative_infinity_compare = CompareReal128("-Infinity", "1");
  auto signed_zero_compare = CompareReal128("-0", "0");
  auto nan_compare = CompareReal128("NaN", "1");

  DatatypeNumericOperationRequest rejected_arithmetic;
  rejected_arithmetic.operation = DatatypeNumericOperationKind::add;
  rejected_arithmetic.type_id = CanonicalTypeId::real128;
  rejected_arithmetic.left = {CanonicalTypeId::real128, "1", false};
  rejected_arithmetic.right = {CanonicalTypeId::real128, "2", false};
  auto arithmetic = ApplyNumericOperation(rejected_arithmetic);

  const bool ok = positive_zero.ok() && positive_zero.value.encoded_value == "0" &&
                  negative_zero.ok() && negative_zero.value.encoded_value == "-0" &&
                  infinity.ok() && infinity.value.encoded_value == "Infinity" &&
                  negative_infinity.ok() && negative_infinity.value.encoded_value == "-Infinity" &&
                  quiet_nan.ok() && quiet_nan.value.encoded_value == "NaN" &&
                  signaling_nan.ok() && signaling_nan.value.encoded_value == "sNaN" &&
                  !invalid.ok() &&
                  infinity_compare.ok() && infinity_compare.comparison == 1 &&
                  negative_infinity_compare.ok() && negative_infinity_compare.comparison == -1 &&
                  signed_zero_compare.ok() && signed_zero_compare.comparison == 0 &&
                  !nan_compare.ok() && !arithmetic.ok();

  std::cout << "{\n";
  Expect(ok, "ok");
  Expect(positive_zero.ok() && positive_zero.value.encoded_value == "0", "positive_zero_canonical");
  Expect(negative_zero.ok() && negative_zero.value.encoded_value == "-0", "negative_zero_canonical");
  Expect(infinity.ok() && infinity.value.encoded_value == "Infinity", "positive_infinity_canonical");
  Expect(negative_infinity.ok() && negative_infinity.value.encoded_value == "-Infinity", "negative_infinity_canonical");
  Expect(quiet_nan.ok() && quiet_nan.value.encoded_value == "NaN", "quiet_nan_canonical");
  Expect(signaling_nan.ok() && signaling_nan.value.encoded_value == "sNaN", "signaling_nan_canonical");
  Expect(!invalid.ok(), "invalid_real128_rejected");
  Expect(infinity_compare.ok() && infinity_compare.comparison == 1, "infinity_compare");
  Expect(negative_infinity_compare.ok() && negative_infinity_compare.comparison == -1, "negative_infinity_compare");
  Expect(signed_zero_compare.ok() && signed_zero_compare.comparison == 0, "signed_zero_compare_equal");
  Expect(!nan_compare.ok(), "nan_compare_rejected");
  std::cout << "  \"arithmetic_requires_backend\": " << (!arithmetic.ok() ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
