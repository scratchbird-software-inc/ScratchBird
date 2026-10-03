// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"

namespace scratchbird::core::metrics::detail {
inline MetricScalar IntegerConstant(MetricScalarType type, unsigned n) {
  switch(type){
    case MetricScalarType::uint64:return std::uint64_t(n);
    case MetricScalarType::int64:return std::int64_t(n);
    case MetricScalarType::float64:return double(n);
    case MetricScalarType::float128:{
      MetricFloat128 value;
      // The only callers require exactly0 and100. 100 =1.5625 *2^6.
      if(n){value.bytes[13]=0x90;value.bytes[14]=0x05;value.bytes[15]=0x40;}
      return value;
    }
    case MetricScalarType::decimal128:{
      MetricDecimal128 value;value.bytes[0]=static_cast<std::uint8_t>(n);
      const std::uint64_t high=std::uint64_t(6176)<<49;
      for(unsigned i=0;i<8;++i)value.bytes[i+8]=static_cast<std::uint8_t>(high>>(i*8));
      return value;
    }
    default:return {};
  }
}
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
template<class Definition>
MetricScalarError ValidateObservationScalar(const Definition& d,const MetricScalar& value) noexcept {
  using E=MetricScalarError;
  const auto definition=ValidateScalarDescriptor(d);if(definition!=E::none)return definition;
  if(MetricScalarTypeOf(value)!=d.value_type)return E::type_mismatch;
  if(!MetricScalarValid(value))return E::invalid_value;
  if(d.value_type==MetricScalarType::enumeration){
    const auto code=std::get<MetricEnumValue>(value).code;
    for(std::size_t i=0;i<d.enum_values.size();++i)if(d.enum_values[i]==code)return E::none;
    return E::out_of_range;
  }
  if(d.min_value&&*CompareMetricScalars(value,*d.min_value)<0)return E::out_of_range;
  if(d.max_value&&*CompareMetricScalars(value,*d.max_value)>0)return E::out_of_range;
  if(d.unit==MetricUnit::percent&&(*CompareMetricScalars(value,IntegerConstant(d.value_type,0))<0||
      *CompareMetricScalars(value,IntegerConstant(d.value_type,100))>0))return E::out_of_range;
  return E::none;
}
}  // namespace scratchbird::core::metrics::detail
