// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Links the production archive: no alternative operation implementation.
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "sbl_numeric.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
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

scratchbird::engine::ExecutionTypeDescriptor Descriptor(
    dt::CanonicalTypeId type_id) {
  static const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Check(manifest.ok(), "load datatype descriptor authority");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  Check(row.ok() && row.manifest.descriptor_rows.size() == 1,
        "lookup datatype descriptor authority");
  if (!row.ok() || row.manifest.descriptor_rows.size() != 1) return {};
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch =
      row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto descriptor =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  Check(descriptor.ok(), "build datatype execution descriptor");
  return descriptor.ok() ? descriptor.descriptor
                         : scratchbird::engine::ExecutionTypeDescriptor{};
}

numeric::NumericContext BackendContext(const dt::DatatypeNumericContext& input) {
  numeric::NumericContext context;
  context.precision = input.precision;
  context.scale = input.scale;
  context.allow_special_values = input.allow_special_values;
  switch (input.rounding) {
    case dt::DatatypeRoundingMode::half_even:
      context.rounding = numeric::RoundingMode::half_even;
      break;
    case dt::DatatypeRoundingMode::half_up:
      context.rounding = numeric::RoundingMode::half_up;
      break;
    case dt::DatatypeRoundingMode::truncate:
      context.rounding = numeric::RoundingMode::truncate;
      break;
    default:
      context.rounding = static_cast<numeric::RoundingMode>(99);
      break;
  }
  return context;
}

std::string Bytes(const numeric::Real128Bytes& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

numeric::Real128BinaryResult Encode(
    std::string_view text, const dt::DatatypeNumericContext& context = {}) {
  return numeric::EncodeReal128LittleEndian(text, BackendContext(context));
}

dt::DatatypeOperationValue Real(
    std::string_view text, const dt::DatatypeNumericContext& context = {}) {
  const auto encoded = Encode(text, context);
  Check(encoded.numeric.status == numeric::NumericStatusCode::ok &&
            encoded.bytes.has_value(),
        "reference fixture converts to exact LE16");
  dt::DatatypeOperationValue value{
      dt::CanonicalTypeId::real128,
      encoded.bytes ? Bytes(*encoded.bytes) : std::string{}, false};
  value.descriptor = Descriptor(dt::CanonicalTypeId::real128);
  return value;
}

dt::DatatypeOperationValue RealNull() {
  dt::DatatypeOperationValue value{dt::CanonicalTypeId::real128, {}, true};
  value.descriptor = Descriptor(dt::CanonicalTypeId::real128);
  return value;
}

dt::DatatypeNumericOperationRequest Request(
    dt::DatatypeNumericOperationKind operation, std::string_view left,
    std::string_view right = "0",
    const dt::DatatypeNumericContext& context = {}) {
  dt::DatatypeNumericOperationRequest request;
  request.type_id = dt::CanonicalTypeId::real128;
  request.operation = operation;
  request.context = context;
  request.left = Real(left, context);
  request.right = Real(right, context);
  request.result_descriptor = Descriptor(dt::CanonicalTypeId::real128);
  return request;
}

numeric::Real128BinaryResult BinaryReference(
    const dt::DatatypeNumericOperationRequest& input) {
  numeric::Real128BinaryRequest request;
  request.operation = static_cast<numeric::NumericOperation>(input.operation);
  std::copy_n(reinterpret_cast<const std::uint8_t*>(
                  input.left.encoded_value.data()),
              16, request.left.emplace().begin());
  if (input.operation != dt::DatatypeNumericOperationKind::canonicalize) {
    std::copy_n(reinterpret_cast<const std::uint8_t*>(
                    input.right.encoded_value.data()),
                16, request.right.emplace().begin());
  }
  request.context = BackendContext(input.context);
  return numeric::ApplyReal128BinaryOperation(request);
}

bool SameFacts(const dt::DatatypeNumericFacts& got,
               const numeric::NumericResult& expected) {
  return got.inexact == expected.inexact &&
      got.underflow == expected.underflow &&
      got.overflow == expected.overflow && got.invalid == expected.invalid &&
      got.divide_by_zero == expected.divide_by_zero &&
      got.subnormal == expected.subnormal &&
      got.unordered ==
          (expected.status == numeric::NumericStatusCode::unordered);
}

void PublicOwners() {
  Check(std::string(dt::DatatypeNullOrderingName(
                        dt::DatatypeNullOrdering::nulls_first)) ==
            "nulls_first" &&
            std::string(dt::DatatypeNullOrderingName(
                            dt::DatatypeNullOrdering::nulls_last)) ==
                "nulls_last",
        "null ordering helpers");
  auto request = Request(dt::DatatypeNumericOperationKind::add, "1", "2");
  const auto result = dt::ApplyNumericOperation(request);
  const auto expected = Encode("3");
  Check(result.ok() && expected.bytes &&
            result.value.encoded_value == Bytes(*expected.bytes) &&
            result.value.encoded_value.size() == 16 &&
            result.value.descriptor.canonical_type_id ==
                static_cast<std::uint32_t>(dt::CanonicalTypeId::real128),
        "ordinary numeric owner publishes descriptor-bound LE16");

  const std::string int32_seven{"\x07\x00\x00\x00", 4};
  dt::DatatypeSortKeyRequest key;
  key.value = {dt::CanonicalTypeId::int32, int32_seven, false};
  const auto ordered = dt::MakeDatatypeSortKey(key);
  Check(ordered.ok() &&
            ordered.sort_key == std::string{"\x01\x80\x00\x00\x07", 5},
        "unrelated int32 sort behavior is retained");
  dt::DatatypeHashRequest hash;
  hash.value = key.value;
  Check(!dt::HashDatatypeValue(hash).ok(),
        "unresolved int32 hash policy still refuses");
  dt::DatatypeSerializationRequest serialize;
  serialize.value = key.value;
  const auto encoded = dt::SerializeDatatypeValue(serialize);
  dt::DatatypeDeserializationRequest deserialize;
  deserialize.expected_type_id = key.value.type_id;
  deserialize.serialized_value = encoded.serialized_value;
  const auto decoded = dt::DeserializeDatatypeValue(deserialize);
  Check(encoded.ok() && decoded.ok() &&
            decoded.value.encoded_value == int32_seven,
        "unrelated int32 serialization is retained");
}

void LexicalBoundaryAndPrecision() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    dt::DatatypeNumericContext context;
    context.rounding = static_cast<dt::DatatypeRoundingMode>(mode);
    for (const char* text : {"0", "-0", "0.1", "9007199254740993",
                             "0x1p-16494", "0x1p-16495",
                             "0x1.00000000000000000000000000008p0"}) {
      const auto encoded = Encode(text, context);
      const auto decoded = encoded.bytes
          ? numeric::DecodeReal128LittleEndian(
                encoded.bytes->data(), encoded.bytes->size(),
                BackendContext(context), true)
          : numeric::Real128BinaryResult{};
      Check(encoded.numeric.status == numeric::NumericStatusCode::ok &&
                encoded.bytes && decoded.numeric.status ==
                    numeric::NumericStatusCode::ok &&
                decoded.bytes == encoded.bytes,
            "public lexical boundary round trips exact LE16");
    }
  }

  std::vector<dt::DatatypeOperationValue> values;
  for (unsigned index = 0; index < 64; ++index) {
    const char* digits = "0123456789abcdef";
    std::string text = "0x1." + std::string(26, '0');
    text += digits[index >> 4];
    text += digits[index & 15];
    text += "p0";
    values.push_back(Real(text));
  }
  for (unsigned left = 0; left < values.size(); ++left) {
    for (unsigned right = 0; right < values.size(); ++right) {
      dt::DatatypeComparisonRequest request;
      request.left = values[left];
      request.right = values[right];
      const auto result = dt::CompareDatatypeValues(request);
      Check(result.ok() &&
                result.comparison ==
                    (left < right ? -1 : left > right ? 1 : 0),
            "binary comparison retains 113-bit ordering");
    }
  }

  dt::DatatypeNumericContext upward;
  upward.rounding = dt::DatatypeRoundingMode::half_up;
  const auto tie_even = Encode("0x1.00000000000000000000000000008p0");
  const auto tie_up = Encode("0x1.00000000000000000000000000008p0",
                             upward);
  Check(tie_even.bytes && tie_up.bytes && tie_even.numeric.inexact &&
            tie_up.numeric.inexact && *tie_even.bytes != *tie_up.bytes,
        "text conversion owns rounding and inexact facts");
}

void BinaryAdapterFacts() {
  struct Case {
    dt::DatatypeNumericOperationKind operation;
    const char* left;
    const char* right;
    bool special;
  };
  const std::vector<Case> cases{
      {dt::DatatypeNumericOperationKind::canonicalize, "0.1", "0", false},
      {dt::DatatypeNumericOperationKind::add, "1", "0x1.8p-112", false},
      {dt::DatatypeNumericOperationKind::subtract, "1", "1", false},
      {dt::DatatypeNumericOperationKind::multiply, "0x1p-16494", "0.5", false},
      {dt::DatatypeNumericOperationKind::divide, "1", "3", false},
      {dt::DatatypeNumericOperationKind::divide, "1", "0", false},
      {dt::DatatypeNumericOperationKind::divide, "0", "0", false},
      {dt::DatatypeNumericOperationKind::add, "NaN", "1", true},
      {dt::DatatypeNumericOperationKind::add, "sNaN", "1", true}};
  for (const auto& entry : cases) {
    for (unsigned mode = 0; mode < 3; ++mode) {
      dt::DatatypeNumericContext context;
      context.allow_special_values = entry.special;
      context.rounding = static_cast<dt::DatatypeRoundingMode>(mode);
      const auto request = Request(entry.operation, entry.left, entry.right,
                                   context);
      const auto expected = BinaryReference(request);
      const auto actual = dt::ApplyNumericOperation(request);
      Check(actual.ok() ==
                (expected.numeric.status == numeric::NumericStatusCode::ok) &&
                SameFacts(actual.numeric_facts, expected.numeric),
            "binary adapter preserves status and every numeric fact");
      if (actual.ok()) {
        Check(expected.bytes &&
                  actual.value.encoded_value == Bytes(*expected.bytes),
              "binary adapter preserves exact result bytes");
      } else {
        Check(actual.value.type_id == dt::CanonicalTypeId::unknown &&
                  actual.value.encoded_value.empty() &&
                  actual.diagnostic.diagnostic_code ==
                      expected.numeric.diagnostic_code,
              "failed binary adapter publishes no value");
      }
    }
  }

  dt::DatatypeNumericContext special_context;
  special_context.allow_special_values = true;
  dt::DatatypeComparisonRequest compare;
  compare.left = Real("NaN", special_context);
  compare.right = Real("1");
  compare.numeric_context = special_context;
  const auto unordered = dt::CompareDatatypeValues(compare);
  compare.left = Real("sNaN", special_context);
  const auto invalid = dt::CompareDatatypeValues(compare);
  Check(!unordered.ok() && unordered.numeric_facts.unordered &&
            !unordered.numeric_facts.invalid && !invalid.ok() &&
            invalid.numeric_facts.invalid,
        "qNaN unordered and sNaN invalid remain distinct");
}

void CastAndPolicyRefusal() {
  const auto exact = Real("1.25");
  dt::DatatypeCastRequest identity;
  identity.value = exact;
  identity.target_type_id = dt::CanonicalTypeId::real128;
  identity.target_descriptor = exact.descriptor;
  const auto identity_result = dt::CastDatatypeValue(identity);
  Check(identity_result.ok() &&
            identity_result.category == dt::DatatypeCastCategory::identity &&
            identity_result.value.encoded_value == exact.encoded_value,
        "exact PRESENT identity byte-preserves LE16");

  std::vector<dt::DatatypeOperationValue> sources{
      {dt::CanonicalTypeId::character, "1.25", false},
      {dt::CanonicalTypeId::int8, std::string(1, '\1'), false},
      {dt::CanonicalTypeId::uint8, std::string(1, '\1'), false},
      {dt::CanonicalTypeId::int128, std::string(16, '\1'), false},
      {dt::CanonicalTypeId::uint128, std::string(16, '\1'), false}};
  for (auto& source : sources) source.descriptor = Descriptor(source.type_id);
  for (const auto& source : sources) {
    dt::DatatypeCastRequest incoming;
    incoming.value = source;
    incoming.target_type_id = dt::CanonicalTypeId::real128;
    incoming.target_descriptor = exact.descriptor;
    incoming.context = dt::DatatypeCastContext::explicit_cast;
    incoming.explicit_cast = true;
    const auto result = dt::CastDatatypeValue(incoming);
    Check(!result.ok() && result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty(),
          "unregistered PRESENT character/int/uint cast refuses");
  }

  const auto hash = dt::HashDatatypeValue({exact});
  const auto key = dt::MakeDatatypeSortKey({exact});
  const auto display = dt::RenderDatatypeValueForDisplay({exact});
  dt::DatatypeSetDescriptor set_descriptor;
  set_descriptor.element_type_id = dt::CanonicalTypeId::real128;
  set_descriptor.element_descriptor = exact.descriptor;
  const auto set = dt::EncodeSetValue(set_descriptor, {exact});
  Check(!hash.ok() && hash.stable_hash_hex.empty() && !key.ok() &&
            key.sort_key.empty() && !display.ok() &&
            display.display_value.empty() && !set.ok() &&
            set.encoded_set.empty(),
        "unbound hash/key/display/set surfaces fail closed");

  const auto serialized = dt::SerializeDatatypeValue({exact});
  dt::DatatypeDeserializationRequest restore;
  restore.expected_type_id = dt::CanonicalTypeId::real128;
  restore.expected_descriptor = exact.descriptor;
  restore.serialized_value = serialized.serialized_value;
  const auto restored = serialized.ok()
      ? dt::DeserializeDatatypeValue(restore)
      : dt::DatatypeDeserializationResult{};
  Check(serialized.ok() && !restored.ok() &&
            restored.value.type_id == dt::CanonicalTypeId::unknown &&
            restored.value.encoded_value.empty(),
        "PRESENT serialization preserves bytes but deserialize refuses");
}

void StructuralValidation() {
  for (bool null : {false, true}) {
    auto request = Request(dt::DatatypeNumericOperationKind::add, "1", "2");
    if (null) request.left = RealNull();
    request.context.rounding = static_cast<dt::DatatypeRoundingMode>(99);
    auto result = dt::ApplyNumericOperation(request);
    Check(!result.ok() && result.numeric_facts.invalid &&
              result.value.type_id == dt::CanonicalTypeId::unknown,
          "unknown rounding rejects before NULL propagation");
    request.context.rounding = dt::DatatypeRoundingMode::half_even;
    request.operation = static_cast<dt::DatatypeNumericOperationKind>(99);
    result = dt::ApplyNumericOperation(request);
    Check(!result.ok() && result.numeric_facts.invalid,
          "unknown operation rejects");
  }
  auto null_request = Request(dt::DatatypeNumericOperationKind::add, "1", "2");
  null_request.left = RealNull();
  const auto null_result = dt::ApplyNumericOperation(null_request);
  Check(null_result.ok() && null_result.value.is_null &&
            null_result.value.encoded_value.empty(),
        "valid strict NULL suppresses arithmetic");

  auto malformed_request = Request(
      dt::DatatypeNumericOperationKind::canonicalize, "1");
  malformed_request.left.encoded_value.assign(15, '\0');
  const auto malformed_result = dt::ApplyNumericOperation(malformed_request);
  Check(!malformed_result.ok() &&
            malformed_result.diagnostic.diagnostic_code ==
                "NUMERIC.ENCODING.NONCANONICAL" &&
            malformed_result.value.type_id == dt::CanonicalTypeId::unknown,
        "malformed LE16 refuses without stale output");

  dt::DatatypeComparisonRequest compare;
  compare.left = RealNull();
  compare.right = Real("1");
  for (const auto mode : {dt::DatatypeNullOrdering::nulls_first,
                          dt::DatatypeNullOrdering::nulls_last}) {
    compare.null_ordering = mode;
    const auto ordered = dt::CompareDatatypeValues(compare);
    Check(ordered.ok() &&
              ordered.comparison ==
                  (mode == dt::DatatypeNullOrdering::nulls_first ? -1 : 1),
          "explicit generic NULL placement remains caller-owned");
  }
  compare.null_ordering = static_cast<dt::DatatypeNullOrdering>(99);
  Check(!dt::CompareDatatypeValues(compare).ok(),
        "invalid NULL placement rejects");
}

void AllocationFailure() {
  const auto request = Request(dt::DatatypeNumericOperationKind::divide,
                               "1.23456789012345678901234567890123456", "3");
  const auto left_before = request.left.encoded_value;
  const auto right_before = request.right.encoded_value;
  unsigned faults = 0;
  bool success = false;
  for (long budget = 0; budget < 8192; ++budget) {
    allocation_budget = budget;
    try {
      const auto result = dt::ApplyNumericOperation(request);
      allocation_budget = -1;
      Check(result.ok() && result.numeric_facts.inexact &&
                result.value.encoded_value.size() == 16,
            "allocation sweep binary result");
      success = true;
    } catch (const std::bad_alloc&) {
      allocation_budget = -1;
      ++faults;
    }
    Check(request.left.encoded_value == left_before &&
              request.right.encoded_value == right_before,
          "allocation fault preserves exact operands");
    if (success) break;
  }
  Check(success && faults > 3, "Core and backend C++ allocations swept");
  std::cout << "cpp_allocation_faults=" << faults << '\n';
}

}  // namespace
int main() {
  PublicOwners();
  LexicalBoundaryAndPrecision();
  BinaryAdapterFacts();
  CastAndPolicyRefusal();
  StructuralValidation();
  AllocationFailure();
  numeric::ReleaseReal128ThreadCache();
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
