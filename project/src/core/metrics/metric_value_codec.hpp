// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"
#include "metric_value_validation.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
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
struct MetricValueViewResult {
  MetricValueCodecError error=MetricValueCodecError::invalid_value;
  std::optional<MetricValueView> value;
  bool ok() const { return error==MetricValueCodecError::none&&value.has_value(); }
};
namespace detail {
inline constexpr std::array value_classes{MetricType::counter,MetricType::gauge,MetricType::histogram,
    MetricType::rate,MetricType::state,MetricType::sample};
inline constexpr std::array value_types{MetricScalarType::uint64,MetricScalarType::int64,MetricScalarType::float64,
    MetricScalarType::float128,MetricScalarType::decimal128,MetricScalarType::boolean,
    MetricScalarType::text,MetricScalarType::uuid,MetricScalarType::enumeration};
inline constexpr std::array value_labels{MetricLabelType::text,MetricLabelType::system_uuid,MetricLabelType::uuid_value};
template<class T,std::size_t N> platform::byte ValueCode(T t,const std::array<T,N>& values) {
  const auto it=std::find(values.begin(),values.end(),t);
  return it==values.end()?0:static_cast<platform::byte>(it-values.begin()+1);
}
inline bool ValueKey(std::string_view s) {
  return !s.empty()&&s.size()<=4096&&s.find('\0')==s.npos&&MetricTextValid(s);
}
inline bool ValueKeyLess(std::string_view a,std::string_view b) {
  return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
      [](unsigned char x,unsigned char y){return x<y;});
}
template<class D> bool ValueCodecDefinition(const D& d) {
  if(!ValueCode(d.type,value_classes)||!ValueCode(d.value_type,value_types)||
      d.labels.size()>1024||d.histogram_buckets.size()>4096)return false;
  for(auto at=d.labels.begin();at!=d.labels.end();++at){
    const auto& label=*at;
    if(!ValueKey(label.key)||!ValueCode(label.value_type,value_labels))return false;
    for(auto before=d.labels.begin();before!=at;++before)if((*before).key==label.key)return false;
  }
  return true;
}
struct ValueViewReader {
  std::span<const platform::byte> bytes;
  std::size_t offset=0;
  MetricValueCodecError error=MetricValueCodecError::none;
  std::span<const platform::byte> Raw(std::size_t n) {
    if(error!=MetricValueCodecError::none)return {};
    if(offset>bytes.size()||n>bytes.size()-offset){error=MetricValueCodecError::invalid_framing;return {};}
    const auto r=bytes.subspan(offset,n);offset+=n;return r;
  }
  platform::byte U8(){auto b=Raw(1);return b.empty()?0:b[0];}
  std::uint32_t U32(){auto b=Raw(4);return b.empty()?0:platform::LoadLittle32(b.data());}
  u64 U64(){auto b=Raw(8);return b.empty()?0:platform::LoadLittle64(b.data());}
  std::string_view String(){auto b=Raw(U32());return {reinterpret_cast<const char*>(b.data()),b.size()};}
  MetricUuid Uuid(){MetricUuid id;auto b=Raw(16);std::copy(b.begin(),b.end(),id.bytes.begin());return id;}
  MetricScalarView Scalar(MetricScalarType t) {
    MetricScalarView v;
    switch(t){
      case MetricScalarType::uint64:v.native=U64();break;
      case MetricScalarType::int64:v.native=std::bit_cast<std::int64_t>(U64());break;
      case MetricScalarType::float64:v.native=std::bit_cast<double>(U64());break;
      case MetricScalarType::float128:{MetricFloat128 n;auto b=Raw(16);std::copy(b.begin(),b.end(),n.bytes.begin());v.native=n;break;}
      case MetricScalarType::decimal128:{MetricDecimal128 n;auto b=Raw(16);std::copy(b.begin(),b.end(),n.bytes.begin());v.native=n;break;}
      case MetricScalarType::boolean:{auto n=U8();if(n>1)error=MetricValueCodecError::invalid_value;v.native=n!=0;break;}
      case MetricScalarType::text:v.text=String();break;
      case MetricScalarType::uuid:v.native=Uuid();break;
      case MetricScalarType::enumeration:v.native=MetricEnumValue{U64()};break;
      default:if(error==MetricValueCodecError::none)error=MetricValueCodecError::invalid_value;
    }
    return v;
  }
};
template<class Definition> MetricValueViewResult DecodeValueView(
    const Definition& d,std::span<const platform::byte> b) noexcept {
  using E=MetricValueCodecError;
    if(b.size()>kMetricValueMaxBytes)return {E::size_limit,{}};
    if(b.size()<kMetricValueHeaderBytes||std::memcmp(b.data(),"SBMV",4)||
        platform::LoadLittle16(b.data()+6)!=kMetricValueHeaderBytes||
        platform::LoadLittle32(b.data()+8)!=b.size()||b[15]||(b[14]&~3u))return {E::invalid_framing,{}};
    if(platform::LoadLittle16(b.data()+4)!=1)return {E::unsupported_version,{}};
    const auto count=platform::LoadLittle32(b.data()+16),bounds=platform::LoadLittle32(b.data()+20);
    if(count>1024||bounds>4096)return {E::size_limit,{}};
    if(!ValueCodecDefinition(d)||b[12]!=ValueCode(d.type,value_classes)||b[13]!=ValueCode(d.value_type,value_types)||
        bounds!=d.histogram_buckets.size())return {E::descriptor_mismatch,{}};
    ValueViewReader r{b,kMetricValueHeaderBytes};MetricValueView v;v.family=d.family;v.type=d.type;
    v.buckets_cumulative=b[14]&1;v.arithmetic_inexact=b[14]&2;
    v.value=r.Scalar(d.value_type);v.count=r.U64();
    if(r.error!=E::none)return {r.error,{}};
    if(d.type==MetricType::histogram){
      v.sum=r.Scalar(d.value_type);const auto start=r.offset;
      for(std::size_t i=0;i<bounds;++i){(void)r.Scalar(d.value_type);(void)r.U64();}
      (void)r.U64();if(r.error!=E::none)return {r.error,{}};
      v.histogram=MetricHistogramView(b.subspan(start,r.offset-start),d.value_type,bounds);
    }
    v.state_text=r.String();const auto start=r.offset;std::string_view previous;
    if(r.error!=E::none)return {r.error,{}};
    for(std::size_t i=0;i<count;++i){
      const auto key=r.String();if(r.error!=E::none)return {r.error,{}};
      if(!ValueKey(key)||(i&&!ValueKeyLess(previous,key)))return {};
      const auto schema=std::find_if(d.labels.begin(),d.labels.end(),[&](const auto& l){return l.key==key;});
      const auto tag=r.U8();
      if(r.error!=E::none)return {r.error,{}};
      if(schema==d.labels.end()||tag!=ValueCode((*schema).value_type,value_labels))return {E::descriptor_mismatch,{}};
      if(tag==1){const auto text=r.String();if(r.error!=E::none)return {r.error,{}};if(!MetricTextValid(text))return {};}
      else (void)r.Uuid();
      if(r.error!=E::none)return {r.error,{}};
      previous=key;
    }
    if(r.offset!=b.size())return {E::invalid_framing,{}};
    v.labels=MetricValueLabelsView(b.subspan(start),count);
    if(!ValueShape(d,v,true))return {};
    return {E::none,std::move(v)};
}
}  // namespace detail
// Complete immutable borrowed validation, not framing-only admission. Nonempty
// sequence views are constructed only by this decoder. No allocation occurs.
template<class Definition> MetricValueViewResult DecodeMetricValueView(
    const Definition& d,std::span<const platform::byte> bytes) noexcept {
  return detail::DecodeValueView(d,bytes);
}
// Materializes a successfully validated view; the caller owns allocation/error handling.
MetricValue MaterializeMetricValue(const MetricValueView&);
// SBMV v1: lossless value inside a native observation, not a sidecar, catalog
// lookup, rate calculation, security receipt or proof of MGA publication.
MetricValueEncodeResult EncodeMetricValue(const MetricDescriptorDefinition&,const MetricValue&) noexcept;
MetricValueDecodeResult DecodeMetricValue(const MetricDescriptorDefinition&,std::span<const platform::byte>) noexcept;
}  // namespace scratchbird::core::metrics
