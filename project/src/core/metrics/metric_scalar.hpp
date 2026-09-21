// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "runtime_platform.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <type_traits>
#include <utility>

namespace scratchbird::core::metrics {

using MetricUuid = scratchbird::core::platform::Uuid;
enum class MetricScalarType {
  invalid, uint64, int64, float64, float128, decimal128, boolean, text, uuid, enumeration
};
struct MetricFloat128 {
  std::array<std::uint8_t,16> bytes{};
  bool operator==(const MetricFloat128& other) const { return bytes == other.bytes; }
  bool operator!=(const MetricFloat128& other) const { return !(*this == other); }
};
// Finite decimal floating carrier, not the fixed exact DECIMAL datatype codec.
struct MetricDecimal128 {
  std::array<std::uint8_t,16> bytes{};
  bool operator==(const MetricDecimal128& other) const { return bytes == other.bytes; }
  bool operator!=(const MetricDecimal128& other) const { return !(*this == other); }
};
struct MetricEnumValue {
  std::uint64_t code = 0;
  bool operator==(const MetricEnumValue& other) const { return code == other.code; }
  bool operator!=(const MetricEnumValue& other) const { return !(*this == other); }
};
struct MetricScalar : std::variant<std::monostate,std::uint64_t,std::int64_t,double,
    MetricFloat128,MetricDecimal128,bool,std::string,MetricUuid,MetricEnumValue> {
  using Base=std::variant<std::monostate,std::uint64_t,std::int64_t,double,
      MetricFloat128,MetricDecimal128,bool,std::string,MetricUuid,MetricEnumValue>;
  using Base::Base;
  using Base::operator=;
  MetricScalar()=default;
  // Construct the chosen alternative directly. A throwing string copy must
  // not unwind a partially constructed variant through a valueless visitor.
  MetricScalar(const MetricScalar& other):Base(Clone(other)){}
  MetricScalar(MetricScalar&&) noexcept=default;
  MetricScalar& operator=(const MetricScalar& other) {
    if(this!=&other){MetricScalar copy(other);*this=std::move(copy);}return *this;
  }
  MetricScalar& operator=(MetricScalar&&) noexcept=default;
 private:
  static Base Clone(const MetricScalar& other) {
    return std::visit([](const auto& value)->Base {
      return Base(std::in_place_type<std::decay_t<decltype(value)>>,value);
    },static_cast<const Base&>(other));
  }
};

enum class MetricScalarError {
  none, invalid_descriptor, type_mismatch, invalid_value, out_of_range,
  overflow, allocation_failure, arithmetic_failure
};
struct MetricScalarResult {
  MetricScalarError error = MetricScalarError::invalid_value;
  std::optional<MetricScalar> value;
  bool inexact = false;
  bool ok() const { return error == MetricScalarError::none && value.has_value(); }
};
// Checked addition in the declared type. Never changes either operand.
MetricScalarResult AddMetricScalars(const MetricScalar&, const MetricScalar&);
std::optional<MetricScalar> MetricScalarZero(MetricScalarType);
struct MetricDescriptorDefinition;
MetricScalarType MetricScalarTypeOf(const MetricScalar&) noexcept;
bool MetricScalarValid(const MetricScalar&) noexcept;
// Only valid numeric values of identical type are comparable. No conversion.
std::optional<int> CompareMetricScalars(const MetricScalar&, const MetricScalar&) noexcept;
MetricScalarError ValidateMetricScalarDescriptor(const MetricDescriptorDefinition&) noexcept;
MetricScalarError ValidateMetricObservationScalar(const MetricDescriptorDefinition&, const MetricScalar&) noexcept;

}  // namespace scratchbird::core::metrics
