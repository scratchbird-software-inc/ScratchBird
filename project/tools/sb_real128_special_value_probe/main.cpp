// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"
#include "datatype_catalog_manifest.hpp"
#include "runtime_capabilities.hpp"
#include "sbl_numeric.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace scratchbird::core::datatypes;

namespace {
namespace numeric = scratchbird::libraries::sbl_numeric;

const scratchbird::engine::ExecutionTypeDescriptor& RealDescriptor() {
  static const auto descriptor = [] {
    const auto manifest = LoadCurrentCoreDatatypeCatalogManifest();
    const auto row = LookupDatatypeCatalogRow(manifest.manifest, CanonicalTypeId::real128);
    if (!manifest.ok() || !row.ok() || row.manifest.descriptor_rows.size() != 1)
      throw std::runtime_error("REAL128 catalog descriptor unavailable");
    CatalogExecutionTypeMetadata metadata;
    metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
    metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
    const auto bound = LookupExecutionTypeDescriptorFromCatalog(CanonicalTypeId::real128, metadata);
    if (!bound.ok()) throw std::runtime_error("REAL128 descriptor binding failed");
    return bound.descriptor;
  }();
  return descriptor;
}

std::string Bits(std::uint8_t high, std::uint8_t next = 0,
                 std::uint8_t fraction_high = 0, std::uint8_t low = 0) {
  std::string bytes(16, '\0');
  bytes[15] = static_cast<char>(high);
  bytes[14] = static_cast<char>(next);
  bytes[13] = static_cast<char>(fraction_high);
  bytes[0] = static_cast<char>(low);
  return bytes;
}

DatatypeOperationValue NativeValue(const char* lexical) {
  // This tool owns the textual input boundary. Core operations receive only
  // the bound binary128 value, never the presentation spelling.
  numeric::NumericContext context;
  context.allow_special_values = true;
  const auto parsed = numeric::EncodeReal128LittleEndian(lexical, context);
  if (parsed.numeric.status != numeric::NumericStatusCode::ok || !parsed.bytes)
    throw std::runtime_error("REAL128 probe fixture lexical input is invalid");
  DatatypeOperationValue value{CanonicalTypeId::real128,
      std::string(parsed.bytes->begin(), parsed.bytes->end()), false};
  value.descriptor = RealDescriptor();
  return value;
}

bool Expect(bool condition, const char* name) {
  std::cout << "  \"" << name << "\": " << (condition ? "true" : "false") << ",\n";
  return condition;
}

DatatypeCastResult CastReal128(const char* value, bool allow_special_values = true) {
  DatatypeCastRequest request;
  request.value = NativeValue(value);
  request.target_type_id = CanonicalTypeId::real128;
  request.target_descriptor = RealDescriptor();
  request.explicit_cast = true;
  request.numeric_context.allow_special_values = allow_special_values;
  return CastDatatypeValue(request);
}

DatatypeNumericOperationResult CompareReal128(const char* left, const char* right, bool allow_special_values = true) {
  DatatypeNumericOperationRequest request;
  request.operation = DatatypeNumericOperationKind::compare;
  request.type_id = CanonicalTypeId::real128;
  request.left = NativeValue(left);
  request.right = NativeValue(right);
  request.context.allow_special_values = allow_special_values;
  return ApplyNumericOperation(request);
}

}  // namespace

int main() {
  namespace numeric = scratchbird::libraries::sbl_numeric;
  namespace platform = scratchbird::core::platform;

  const std::string_view backend = numeric::Real128BackendName();
  const bool supported_backend =
      backend == "MPFR/GMP binary128 reference" && numeric::Real128BackendAvailable();
  std::size_t real128_capability_count = 0;
  std::string capability_provider;
  for (const auto& capability :
       platform::DetectRuntimeCapabilities().capabilities) {
    if (capability.key != "numeric.real128") { continue; }
    ++real128_capability_count;
    if (capability.state == platform::CapabilityState::present) {
      capability_provider = capability.provider;
    }
  }
  const bool provider_truth =
      real128_capability_count == 1 && capability_provider == backend;
  auto positive_zero = CastReal128("+0.0");
  auto negative_zero = CastReal128("-0.0");
  auto infinity = CastReal128("+inf");
  auto negative_infinity = CastReal128("-Infinity");
  auto quiet_nan = CastReal128("NaN");
  auto signaling_nan = CastReal128("sNaN");
  const auto invalid = numeric::EncodeReal128LittleEndian("not-a-number");
  const bool invalid_rejected = invalid.numeric.status != numeric::NumericStatusCode::ok &&
      invalid.numeric.invalid && !invalid.bytes;
  bool malformed_native_rejected = true;
  for (const auto& bytes : {std::string(15, '\0'), std::string(17, '\0'), std::string("1")}) {
    DatatypeCastRequest request;
    request.value = NativeValue("1");
    request.value.encoded_value = bytes;
    request.target_type_id = CanonicalTypeId::real128;
    request.target_descriptor = RealDescriptor();
    const auto refused = CastDatatypeValue(request);
    malformed_native_rejected &= !refused.ok() &&
        refused.value.type_id == CanonicalTypeId::unknown &&
        refused.value.encoded_value.empty() && !refused.value.is_null;
  }

  auto infinity_compare = CompareReal128("Infinity", "1");
  auto negative_infinity_compare = CompareReal128("-Infinity", "1");
  auto signed_zero_compare = CompareReal128("-0", "0");
  auto nan_compare = CompareReal128("NaN", "1");
  bool forbidden_specials_rejected = true;
  for (const auto* special : {"Infinity", "-Infinity", "NaN", "sNaN"}) {
    const auto lexical = numeric::EncodeReal128LittleEndian(special);
    const auto cast = CastReal128(special, false);
    const auto comparison = CompareReal128(special, "1", false);
    forbidden_specials_rejected = forbidden_specials_rejected &&
        lexical.numeric.status != numeric::NumericStatusCode::ok &&
        lexical.numeric.invalid && !lexical.bytes && !cast.ok() && !comparison.ok() &&
        cast.numeric_facts.invalid && comparison.numeric_facts.invalid &&
        cast.diagnostic.diagnostic_code == "NUMERIC.REAL128.INVALID";
  }

  DatatypeNumericOperationRequest backend_arithmetic;
  backend_arithmetic.operation = DatatypeNumericOperationKind::add;
  backend_arithmetic.type_id = CanonicalTypeId::real128;
  backend_arithmetic.left = NativeValue("1");
  backend_arithmetic.right = NativeValue("2");
  backend_arithmetic.result_descriptor = RealDescriptor();
  auto arithmetic = ApplyNumericOperation(backend_arithmetic);

  const bool ok = supported_backend && provider_truth && forbidden_specials_rejected &&
                  malformed_native_rejected &&
                  positive_zero.ok() && positive_zero.value.encoded_value == Bits(0) &&
                  negative_zero.ok() && negative_zero.value.encoded_value == Bits(0x80) &&
                  infinity.ok() && infinity.value.encoded_value == Bits(0x7f, 0xff) &&
                  negative_infinity.ok() && negative_infinity.value.encoded_value == Bits(0xff, 0xff) &&
                  quiet_nan.ok() && quiet_nan.value.encoded_value == Bits(0x7f, 0xff, 0x80) &&
                  signaling_nan.ok() && signaling_nan.value.encoded_value == Bits(0x7f, 0xff, 0, 1) &&
                  invalid_rejected &&
                  infinity_compare.ok() && infinity_compare.comparison == 1 &&
                  negative_infinity_compare.ok() && negative_infinity_compare.comparison == -1 &&
                  signed_zero_compare.ok() && signed_zero_compare.comparison == 0 &&
                  !nan_compare.ok() && arithmetic.ok() &&
                  arithmetic.value.encoded_value == Bits(0x40, 0, 0x80);

  std::cout << "{\n";
  Expect(ok, "ok");
  Expect(positive_zero.ok() && positive_zero.value.encoded_value == Bits(0), "positive_zero_canonical");
  Expect(negative_zero.ok() && negative_zero.value.encoded_value == Bits(0x80), "negative_zero_canonical");
  Expect(infinity.ok() && infinity.value.encoded_value == Bits(0x7f, 0xff), "positive_infinity_canonical");
  Expect(negative_infinity.ok() && negative_infinity.value.encoded_value == Bits(0xff, 0xff), "negative_infinity_canonical");
  Expect(quiet_nan.ok() && quiet_nan.value.encoded_value == Bits(0x7f, 0xff, 0x80), "quiet_nan_canonical");
  Expect(signaling_nan.ok() && signaling_nan.value.encoded_value == Bits(0x7f, 0xff, 0, 1), "signaling_nan_canonical");
  Expect(invalid_rejected, "invalid_real128_rejected");
  Expect(malformed_native_rejected, "malformed_native_real128_rejected");
  Expect(infinity_compare.ok() && infinity_compare.comparison == 1, "infinity_compare");
  Expect(negative_infinity_compare.ok() && negative_infinity_compare.comparison == -1, "negative_infinity_compare");
  Expect(signed_zero_compare.ok() && signed_zero_compare.comparison == 0, "signed_zero_compare_equal");
  Expect(!nan_compare.ok(), "nan_compare_rejected");
  Expect(forbidden_specials_rejected, "forbidden_specials_rejected");
  Expect(supported_backend, "real128_backend_supported");
  Expect(provider_truth, "real128_capability_backend_match");
  std::cout << "  \"real128_backend\": \"" << backend << "\",\n";
  std::cout << "  \"arithmetic_backend_operational\": "
            << (arithmetic.ok() && arithmetic.value.encoded_value == Bits(0x40, 0, 0x80)
                    ? "true"
                    : "false")
            << "\n";
  std::cout << "}\n";
  numeric::ReleaseReal128ThreadCache();
  return ok ? 0 : 1;
}
