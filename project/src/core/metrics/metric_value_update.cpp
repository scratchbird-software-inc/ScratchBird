// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_value_update.hpp"
#include "metric_label_key.hpp"
#include <limits>
#include <new>

namespace scratchbird::core::metrics {
namespace {
using E=MetricValueUpdateError;
E ArithmeticError(MetricScalarError error) {
  switch(error){
    case MetricScalarError::overflow:return E::overflow;
    case MetricScalarError::allocation_failure:return E::allocation_failure;
    case MetricScalarError::arithmetic_failure:return E::arithmetic_failure;
    default:return E::invalid_observation;
  }
}
bool Numeric(MetricScalarType type) {return type>=MetricScalarType::uint64&&type<=MetricScalarType::decimal128;}
bool Negative(const MetricScalar& value) {
  const auto zero=MetricScalarZero(MetricScalarTypeOf(value));
  if(!zero)return false;
  const auto order=CompareMetricScalars(value,*zero);return order&&*order<0;
}
}

bool ValidateMetricHistogramDescriptor(const MetricDescriptorDefinition& d) noexcept {
  if(d.type!=MetricType::histogram)return d.histogram_buckets.empty()&&d.histogram_cumulative;
  if(!Numeric(d.value_type)||d.histogram_buckets.empty())return false;
  for(std::size_t i=0;i<d.histogram_buckets.size();++i){
    const auto& bound=d.histogram_buckets[i];
    if(MetricScalarTypeOf(bound)!=d.value_type||!MetricScalarValid(bound))return false;
    if(i&&*CompareMetricScalars(d.histogram_buckets[i-1],bound)>=0)return false;
  }
  return true;
}
bool ValidateMetricValueDescriptor(const MetricDescriptorDefinition& d) noexcept {
  return ValidateMetricScalarDescriptor(d)==MetricScalarError::none&&ValidateMetricHistogramDescriptor(d)&&
      (d.type!=MetricType::counter||Numeric(d.value_type))&&
      (d.type!=MetricType::state||d.value_type==MetricScalarType::enumeration)&&
      (d.type==MetricType::counter||d.type==MetricType::gauge||d.type==MetricType::histogram||
       d.type==MetricType::state||d.type==MetricType::sample)&&!d.rate_window_nanoseconds;
}
bool ValidateStoredMetricValueDescriptor(const MetricDescriptorDefinition& d) noexcept {
  return d.type==MetricType::rate ? Numeric(d.value_type)&&d.rate_window_nanoseconds&&
      ValidateMetricScalarDescriptor(d)==MetricScalarError::none&&ValidateMetricHistogramDescriptor(d) :
      ValidateMetricValueDescriptor(d);
}
namespace {
bool ValueShape(const MetricDescriptorDefinition& d,const MetricValue& v,bool stored) {
  const bool rate=stored&&d.type==MetricType::rate;
  const bool descriptor_valid=stored?ValidateStoredMetricValueDescriptor(d):ValidateMetricValueDescriptor(d);
  if(!descriptor_valid||
      v.family!=d.family||v.type!=d.type||!ValidateMetricLabelSet(d,v.labels).ok||
      ValidateMetricObservationScalar(d,v.value)!=MetricScalarError::none)return false;
  if((d.type==MetricType::counter||rate)&&(!Numeric(d.value_type)||Negative(v.value)))return false;
  if(d.type==MetricType::state&&d.value_type!=MetricScalarType::enumeration)return false;
  if(d.type!=MetricType::state&&!v.state_text.empty())return false;
  if(!MetricScalarValid(MetricScalar(v.state_text)))return false;
  if(d.type!=MetricType::histogram)return v.count==0&&v.buckets.empty()&&v.bucket_bounds.empty()&&v.buckets_cumulative&&
      std::holds_alternative<std::monostate>(v.sum)&&
      (d.type==MetricType::counter||rate||!v.arithmetic_inexact);
  if(!v.count||v.buckets.size()!=d.histogram_buckets.size()+1||
      v.bucket_bounds!=d.histogram_buckets||v.buckets_cumulative!=d.histogram_cumulative||
      MetricScalarTypeOf(v.sum)!=d.value_type||!MetricScalarValid(v.sum))return false;
  u64 accumulated=0;
  for(std::size_t i=0;i<v.buckets.size();++i){
    const auto count=v.buckets[i];if(count>v.count)return false;
    if(d.histogram_cumulative){if(i&&count<v.buckets[i-1])return false;}
    else {if(count>std::numeric_limits<u64>::max()-accumulated)return false;accumulated+=count;}
  }
  return d.histogram_cumulative?v.buckets.back()==v.count:accumulated==v.count;
}
}
bool ValidateMetricValueShape(const MetricDescriptorDefinition& d,const MetricValue& v) {
  return ValueShape(d,v,false);
}
bool ValidateStoredMetricValueShape(const MetricDescriptorDefinition& d,const MetricValue& v) {
  return ValueShape(d,v,true);
}
MetricValueUpdateResult StageMetricValueUpdate(const MetricDescriptorDefinition& d,
    const MetricLabelSet& labels,const MetricValue* previous,const MetricScalar& observation,
    const std::string& state_text) {
  try {
    if(!ValidateMetricValueDescriptor(d))return {E::invalid_descriptor,{}};
    if(!ValidateMetricLabelSet(d,labels).ok||MetricScalarTypeOf(observation)!=d.value_type||
        !MetricScalarValid(observation)||(d.type!=MetricType::state&&!state_text.empty())||
        !MetricScalarValid(MetricScalar(state_text)))return {E::invalid_observation,{}};
    if(d.type==MetricType::counter){if(Negative(observation))return {E::negative_delta,{}};}
    else if(ValidateMetricObservationScalar(d,observation)!=MetricScalarError::none)return {E::invalid_observation,{}};
    if(previous&&(!ValidateMetricValueShape(d,*previous)||
        MakeMetricSeriesKey(d.family,labels)!=MakeMetricSeriesKey(d.family,previous->labels)))return {E::invalid_current,{}};
    MetricValue next;
    if(previous)next=*previous;
    else {next.family=d.family;next.labels=labels;next.type=d.type;}
    if(d.type==MetricType::counter){
      const auto zero=MetricScalarZero(d.value_type);
      const auto sum=AddMetricScalars(previous?previous->value:*zero,observation);
      if(!sum.ok())return {ArithmeticError(sum.error),{}};
      next.value=*sum.value;next.arithmetic_inexact|=sum.inexact;
    }else if(d.type==MetricType::histogram){
      if(next.count==std::numeric_limits<u64>::max())return {E::overflow,{}};
      if(!previous){next.sum=*MetricScalarZero(d.value_type);next.buckets.resize(d.histogram_buckets.size()+1);
        next.bucket_bounds=d.histogram_buckets;next.buckets_cumulative=d.histogram_cumulative;}
      const auto sum=AddMetricScalars(next.sum,observation);
      if(!sum.ok())return {ArithmeticError(sum.error),{}};
      next.sum=*sum.value;next.arithmetic_inexact|=sum.inexact;next.value=observation;
      std::size_t bucket=0;
      while(bucket<d.histogram_buckets.size()&&*CompareMetricScalars(observation,d.histogram_buckets[bucket])>0)++bucket;
      const auto end=d.histogram_cumulative?next.buckets.size():bucket+1;
      for(auto i=bucket;i<end;++i){if(next.buckets[i]==std::numeric_limits<u64>::max())return {E::overflow,{}};++next.buckets[i];}
      ++next.count;
    }else {next.value=observation;next.state_text=state_text;next.arithmetic_inexact=false;}
    if(!ValidateMetricValueShape(d,next))return {E::invalid_observation,{}};
    return {E::none,std::move(next)};
  }catch(const std::bad_alloc&){return {E::allocation_failure,{}};}
}
}  // namespace scratchbird::core::metrics
