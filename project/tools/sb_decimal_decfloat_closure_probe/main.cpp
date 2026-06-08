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

DatatypeNumericOperationRequest DecimalRequest(DatatypeNumericOperationKind operation,
                                               const char* left,
                                               const char* right = "") {
  DatatypeNumericOperationRequest request;
  request.operation = operation;
  request.type_id = CanonicalTypeId::decimal;
  request.left = {CanonicalTypeId::decimal, left, false};
  request.right = {CanonicalTypeId::decimal, right, false};
  request.context.precision = 38;
  request.context.scale = 2;
  request.context.rounding = DatatypeRoundingMode::half_even;
  return request;
}

DatatypeNumericOperationRequest DecfloatRequest(DatatypeNumericOperationKind operation,
                                                const char* left,
                                                const char* right = "") {
  DatatypeNumericOperationRequest request;
  request.operation = operation;
  request.type_id = CanonicalTypeId::decimal_float;
  request.left = {CanonicalTypeId::decimal_float, left, false};
  request.right = {CanonicalTypeId::decimal_float, right, false};
  request.context.precision = 34;
  request.context.scale = 2;
  request.context.allow_special_values = true;
  return request;
}

}  // namespace

int main() {
  auto canonical = ApplyNumericOperation(DecimalRequest(DatatypeNumericOperationKind::canonicalize, "00123.455"));
  auto overflow = ApplyNumericOperation(DecimalRequest(DatatypeNumericOperationKind::canonicalize,
                                                       "999999999999999999999999999999999999999.00"));
  auto add = ApplyNumericOperation(DecimalRequest(DatatypeNumericOperationKind::add, "100.25", "0.75"));
  auto subtract = ApplyNumericOperation(DecimalRequest(DatatypeNumericOperationKind::subtract, "100.25", "0.75"));
  auto multiply = ApplyNumericOperation(DecimalRequest(DatatypeNumericOperationKind::multiply, "12.50", "2"));

  auto divide_request = DecimalRequest(DatatypeNumericOperationKind::divide, "1", "4");
  divide_request.context.scale = 4;
  auto divide = ApplyNumericOperation(divide_request);

  auto compare = ApplyNumericOperation(DecimalRequest(DatatypeNumericOperationKind::compare, "1.20", "1.2"));
  auto divide_by_zero = ApplyNumericOperation(DecimalRequest(DatatypeNumericOperationKind::divide, "1", "0"));

  auto decfloat = ApplyNumericOperation(DecfloatRequest(DatatypeNumericOperationKind::canonicalize, "00123.4500"));
  auto decfloat_inf = ApplyNumericOperation(DecfloatRequest(DatatypeNumericOperationKind::canonicalize, "+Infinity"));
  auto decfloat_nan_compare = ApplyNumericOperation(DecfloatRequest(DatatypeNumericOperationKind::compare, "NaN", "1"));
  auto decfloat_special_arith = ApplyNumericOperation(DecfloatRequest(DatatypeNumericOperationKind::add, "Infinity", "1"));

  DatatypeCastRequest cast_decimal;
  cast_decimal.value = {CanonicalTypeId::character, "42", false};
  cast_decimal.target_type_id = CanonicalTypeId::decimal;
  cast_decimal.explicit_cast = true;
  auto decimal_cast = CastDatatypeValue(cast_decimal);

  DatatypeCastRequest cast_decfloat;
  cast_decfloat.value = {CanonicalTypeId::character, "-0.00", false};
  cast_decfloat.target_type_id = CanonicalTypeId::decimal_float;
  cast_decfloat.explicit_cast = true;
  auto decfloat_cast = CastDatatypeValue(cast_decfloat);

  const bool ok = canonical.ok() && canonical.value.encoded_value == "123.46" && !overflow.ok() &&
                  add.ok() && add.value.encoded_value == "101.00" &&
                  subtract.ok() && subtract.value.encoded_value == "99.50" &&
                  multiply.ok() && multiply.value.encoded_value == "25.00" &&
                  divide.ok() && divide.value.encoded_value == "0.2500" &&
                  compare.ok() && compare.comparison == 0 && !divide_by_zero.ok() &&
                  decfloat.ok() && decfloat.value.encoded_value == "1.2345E+2" &&
                  decfloat_inf.ok() && decfloat_inf.value.encoded_value == "Infinity" &&
                  !decfloat_nan_compare.ok() && !decfloat_special_arith.ok() &&
                  decimal_cast.ok() && decimal_cast.value.encoded_value == "42" &&
                  decfloat_cast.ok() && decfloat_cast.value.encoded_value == "-0E+0";

  std::cout << "{\n";
  Expect(ok, "ok");
  Expect(canonical.ok() && canonical.value.encoded_value == "123.46", "decimal_half_even_canonicalize");
  Expect(!overflow.ok(), "decimal_overflow_rejected");
  Expect(add.ok() && add.value.encoded_value == "101.00", "decimal_add");
  Expect(subtract.ok() && subtract.value.encoded_value == "99.50", "decimal_subtract");
  Expect(multiply.ok() && multiply.value.encoded_value == "25.00", "decimal_multiply");
  Expect(divide.ok() && divide.value.encoded_value == "0.2500", "decimal_divide");
  Expect(compare.ok() && compare.comparison == 0, "decimal_compare_equal");
  Expect(!divide_by_zero.ok(), "decimal_divide_by_zero_rejected");
  Expect(decfloat.ok() && decfloat.value.encoded_value == "1.2345E+2", "decfloat_canonicalize");
  Expect(decfloat_inf.ok() && decfloat_inf.value.encoded_value == "Infinity", "decfloat_infinity_canonicalize");
  Expect(!decfloat_nan_compare.ok(), "decfloat_nan_compare_rejected");
  Expect(!decfloat_special_arith.ok(), "decfloat_special_arithmetic_rejected");
  Expect(decimal_cast.ok() && decimal_cast.value.encoded_value == "42", "decimal_cast");
  std::cout << "  \"decfloat_cast\": " << (decfloat_cast.ok() && decfloat_cast.value.encoded_value == "-0E+0" ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
