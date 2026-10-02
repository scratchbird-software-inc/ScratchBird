// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"

namespace scratchbird::core::metrics::detail {
// Shared predicates for owning definitions and validated input-backed catalog
// definitions. Sequence access returns native values; no materialization or
// different semantic rule is permitted in the borrowed route.
template<class Definition>
MetricScalarError ValidateScalarDescriptor(const Definition& d) noexcept {
  using E = MetricScalarError;
  if (d.value_type < MetricScalarType::uint64 || d.value_type > MetricScalarType::enumeration)
    return E::invalid_descriptor;
  if (d.value_type == MetricScalarType::enumeration) {
    if (d.enum_values.empty()) return E::invalid_descriptor;
    for (std::size_t i=0;i<d.enum_values.size();++i)
      for (std::size_t j=0;j<i;++j)
        if (d.enum_values[i]==d.enum_values[j]) return E::invalid_descriptor;
  } else if (!d.enum_values.empty()) return E::invalid_descriptor;
  const bool numeric = d.value_type >= MetricScalarType::uint64 && d.value_type <= MetricScalarType::decimal128;
  if (!numeric && (d.min_value || d.max_value || d.unit==MetricUnit::percent)) return E::invalid_descriptor;
  for (const auto* bound : {&d.min_value,&d.max_value})
    if (*bound && (MetricScalarTypeOf(**bound)!=d.value_type || !MetricScalarValid(**bound)))
      return E::invalid_descriptor;
  if (d.min_value && d.max_value && *CompareMetricScalars(*d.min_value,*d.max_value)>0)
    return E::invalid_descriptor;
  return E::none;
}
template<class Definition>
bool ValidateHistogramDescriptor(const Definition& d) noexcept {
  if (d.type!=MetricType::histogram) return d.histogram_buckets.empty() && d.histogram_cumulative;
  if (d.value_type<MetricScalarType::uint64 || d.value_type>MetricScalarType::decimal128 ||
      d.histogram_buckets.empty()) return false;
  for (std::size_t i=0;i<d.histogram_buckets.size();++i) {
    const auto& bound=d.histogram_buckets[i];
    if (MetricScalarTypeOf(bound)!=d.value_type || !MetricScalarValid(bound)) return false;
    if (i && *CompareMetricScalars(d.histogram_buckets[i-1],bound)>=0) return false;
  }
  return true;
}
}  // namespace scratchbird::core::metrics::detail
