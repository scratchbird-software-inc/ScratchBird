// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_history.hpp"
#include "metric_value_codec.hpp"
#include <span>

namespace scratchbird::core::metrics {
constexpr std::size_t kMetricSampleHeaderBytes=280;
constexpr std::size_t kMetricSampleMaxBytes=kMetricSampleHeaderBytes+256+kMetricValueMaxBytes;
enum class MetricSampleCodecError {
  none, invalid_binding, invalid_sample, invalid_value, invalid_framing,
  unsupported_version, size_limit, resource_exhausted
};
struct MetricSampleEncodeResult {
  MetricSampleCodecError error=MetricSampleCodecError::invalid_sample;
  std::vector<platform::byte> bytes;
  bool ok() const {return error==MetricSampleCodecError::none&&!bytes.empty();}
};
struct MetricSampleDecodeResult {
  MetricSampleCodecError error=MetricSampleCodecError::invalid_sample;
  std::optional<MetricRawSampleRecord> record;
  bool ok() const {return error==MetricSampleCodecError::none&&record.has_value();}
};
// SBMS v1 payload only. Caller retains native relation/row, catalog, clock/rate
// source, security and MGA inventory authority. No identities/times are issued.
MetricSampleEncodeResult EncodeMetricRawSample(const MetricDescriptor&,
    const MetricSeriesIdentity&,const MetricRawSampleRecord&) noexcept;
MetricSampleDecodeResult DecodeMetricRawSample(const MetricDescriptor&,
    const MetricSeriesIdentity&,std::span<const platform::byte>) noexcept;
}  // namespace scratchbird::core::metrics
