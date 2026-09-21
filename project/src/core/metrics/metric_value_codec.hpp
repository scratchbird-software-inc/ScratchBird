// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"
#include <span>

namespace scratchbird::core::metrics {
constexpr std::size_t kMetricValueHeaderBytes=24;
constexpr std::size_t kMetricValueMaxBytes=1048576;
enum class MetricValueCodecError {
  none, invalid_value, invalid_framing, unsupported_version, descriptor_mismatch,
  size_limit, resource_exhausted
};
struct MetricValueEncodeResult {
  MetricValueCodecError error=MetricValueCodecError::invalid_value;
  std::vector<platform::byte> bytes;
  bool ok() const {return error==MetricValueCodecError::none&&!bytes.empty();}
};
struct MetricValueDecodeResult {
  MetricValueCodecError error=MetricValueCodecError::invalid_value;
  std::optional<MetricValue> value;
  bool ok() const {return error==MetricValueCodecError::none&&value.has_value();}
};
// SBMV v1: lossless value inside a native observation, not a sidecar, catalog
// lookup, rate calculation, security receipt or proof of MGA publication.
MetricValueEncodeResult EncodeMetricValue(const MetricDescriptorDefinition&,const MetricValue&) noexcept;
MetricValueDecodeResult DecodeMetricValue(const MetricDescriptorDefinition&,std::span<const platform::byte>) noexcept;
}  // namespace scratchbird::core::metrics
