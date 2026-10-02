// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_descriptor_validation.hpp"
#include "metric_label_key.hpp"
#include "metric_value_view.hpp"
#include <limits>

namespace scratchbird::core::metrics::detail {
inline bool ValueNumeric(MetricScalarType t) {
  return t>=MetricScalarType::uint64 && t<=MetricScalarType::decimal128;
}
template<class D> bool ValueDescriptor(const D& d,bool stored) noexcept {
  if(stored && d.type==MetricType::rate)
    return ValueNumeric(d.value_type)&&d.rate_window_nanoseconds&&
        ValidateScalarDescriptor(d)==MetricScalarError::none&&ValidateHistogramDescriptor(d);
  return ValidateScalarDescriptor(d)==MetricScalarError::none&&ValidateHistogramDescriptor(d)&&
      (d.type!=MetricType::counter||ValueNumeric(d.value_type))&&
      (d.type!=MetricType::state||d.value_type==MetricScalarType::enumeration)&&
      (d.type==MetricType::counter||d.type==MetricType::gauge||d.type==MetricType::histogram||
       d.type==MetricType::state||d.type==MetricType::sample)&&!d.rate_window_nanoseconds;
}
inline const MetricScalar& Native(const MetricScalar& v) { return v; }
inline const MetricScalar& Native(const MetricScalarView& v) { return v.native; }
inline MetricScalarType ScalarType(const MetricScalar& v) { return MetricScalarTypeOf(v); }
inline MetricScalarType ScalarType(const MetricScalarView& v) {
  return v.text?MetricScalarType::text:MetricScalarTypeOf(v.native);
}
inline bool ScalarValid(const MetricScalar& v) { return MetricScalarValid(v); }
inline bool ScalarValid(const MetricScalarView& v) {
  return v.text?MetricTextValid(*v.text):MetricScalarValid(v.native);
}
inline bool EmptyScalar(const MetricScalar& v) { return std::holds_alternative<std::monostate>(v); }
inline bool EmptyScalar(const MetricScalarView& v) { return !v.text&&EmptyScalar(v.native); }
template<class D> MetricScalarError ValidateObservationScalar(const D& d,const MetricScalarView& v) noexcept {
  if(!v.text)return ValidateObservationScalar(d,v.native);
  const auto definition=ValidateScalarDescriptor(d);
  if(definition!=MetricScalarError::none)return definition;
  if(d.value_type!=MetricScalarType::text)return MetricScalarError::type_mismatch;
  return MetricTextValid(*v.text)?MetricScalarError::none:MetricScalarError::invalid_value;
}
inline std::size_t Bounds(const MetricValue& v) { return v.bucket_bounds.size(); }
inline std::size_t Bounds(const MetricValueView& v) { return v.histogram.size(); }
inline std::size_t Buckets(const MetricValue& v) { return v.buckets.size(); }
inline std::size_t Buckets(const MetricValueView& v) { return v.histogram.empty()?0:v.histogram.size()+1; }
inline const MetricScalar& Bound(const MetricValue& v,std::size_t i) { return v.bucket_bounds[i]; }
inline MetricScalar Bound(const MetricValueView& v,std::size_t i) { return v.histogram.bound(i); }
inline u64 Bucket(const MetricValue& v,std::size_t i) { return v.buckets[i]; }
inline u64 Bucket(const MetricValueView& v,std::size_t i) { return v.histogram.bucket(i); }
template<class D,class V> bool ValueShape(const D& d,const V& v,bool stored) {
  const bool rate=stored&&d.type==MetricType::rate;
  if(!ValueDescriptor(d,stored)||v.family!=d.family||v.type!=d.type||
      ValidateLabels(d,v.labels).issue!=LabelIssue::none||
      ValidateObservationScalar(d,v.value)!=MetricScalarError::none)return false;
  if(d.type==MetricType::counter||rate) {
    if(!ValueNumeric(d.value_type))return false;
    if(*CompareMetricScalars(Native(v.value),*MetricScalarZero(d.value_type))<0)return false;
  }
  if(d.type==MetricType::state&&d.value_type!=MetricScalarType::enumeration)return false;
  if(d.type!=MetricType::state&&!v.state_text.empty())return false;
  if(!MetricTextValid(v.state_text))return false;
  if(d.type!=MetricType::histogram)return v.count==0&&Buckets(v)==0&&Bounds(v)==0&&v.buckets_cumulative&&
      EmptyScalar(v.sum)&&
      (d.type==MetricType::counter||rate||!v.arithmetic_inexact);
  if(!v.count||Buckets(v)!=d.histogram_buckets.size()+1||Bounds(v)!=d.histogram_buckets.size()||
      v.buckets_cumulative!=d.histogram_cumulative||ScalarType(v.sum)!=d.value_type||!ScalarValid(v.sum))return false;
  for(std::size_t i=0;i<Bounds(v);++i)if(Bound(v,i)!=d.histogram_buckets[i])return false;
  u64 accumulated=0;
  for(std::size_t i=0;i<Buckets(v);++i){
    const auto count=Bucket(v,i);if(count>v.count)return false;
    if(d.histogram_cumulative){if(i&&count<Bucket(v,i-1))return false;}
    else {if(count>std::numeric_limits<u64>::max()-accumulated)return false;accumulated+=count;}
  }
  return d.histogram_cumulative?Bucket(v,Buckets(v)-1)==v.count:accumulated==v.count;
}
}  // namespace scratchbird::core::metrics::detail
