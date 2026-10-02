// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_value_codec.hpp"
#include "metric_value_update.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <new>
#include <stdexcept>

namespace scratchbird::core::metrics {
namespace {
using platform::byte;
using E=MetricValueCodecError;
constexpr std::array classes{MetricType::counter,MetricType::gauge,MetricType::histogram,
    MetricType::rate,MetricType::state,MetricType::sample};
constexpr std::array types{MetricScalarType::uint64,MetricScalarType::int64,MetricScalarType::float64,
    MetricScalarType::float128,MetricScalarType::decimal128,MetricScalarType::boolean,
    MetricScalarType::text,MetricScalarType::uuid,MetricScalarType::enumeration};
constexpr std::array labels{MetricLabelType::text,MetricLabelType::system_uuid,MetricLabelType::uuid_value};
template<class T,std::size_t N> byte Code(T t,const std::array<T,N>& values) {
  const auto it=std::find(values.begin(),values.end(),t);
  return it==values.end()?0:static_cast<byte>(it-values.begin()+1);
}
bool Text(std::string_view s) { return MetricTextValid(s); }
bool Key(std::string_view s) { return detail::ValueKey(s); }
bool Less(std::string_view a,std::string_view b) { return detail::ValueKeyLess(a,b); }
const MetricLabelDescriptor* Label(const MetricDescriptorDefinition& d,const std::string& key) {
  const auto it=std::find_if(d.labels.begin(),d.labels.end(),[&](const auto& l){return l.key==key;});
  return it==d.labels.end()?nullptr:&*it;
}
bool Definition(const MetricDescriptorDefinition& d) { return detail::ValueCodecDefinition(d); }
struct Writer {
  std::vector<byte> bytes=std::vector<byte>(kMetricValueHeaderBytes);
  void Raw(const byte* p,std::size_t n) {
    if(n>kMetricValueMaxBytes-bytes.size())throw E::size_limit;
    if(n)bytes.insert(bytes.end(),p,p+n);
  }
  void U8(byte n){Raw(&n,1);}
  void U32(std::uint32_t n){std::array<byte,4>b{};platform::StoreLittle32(b.data(),n);Raw(b.data(),b.size());}
  void U64(u64 n){std::array<byte,8>b{};platform::StoreLittle64(b.data(),n);Raw(b.data(),b.size());}
  void String(const std::string& s) {
    if(s.size()>kMetricValueMaxBytes)throw E::size_limit;
    U32(static_cast<std::uint32_t>(s.size()));Raw(reinterpret_cast<const byte*>(s.data()),s.size());
  }
  void Scalar(const MetricScalar& v) {
    switch(MetricScalarTypeOf(v)) {
      case MetricScalarType::uint64:U64(std::get<u64>(v));break;
      case MetricScalarType::int64:U64(std::bit_cast<u64>(std::get<std::int64_t>(v)));break;
      case MetricScalarType::float64:U64(std::bit_cast<u64>(std::get<double>(v)));break;
      case MetricScalarType::float128:Raw(std::get<MetricFloat128>(v).bytes.data(),16);break;
      case MetricScalarType::decimal128:Raw(std::get<MetricDecimal128>(v).bytes.data(),16);break;
      case MetricScalarType::boolean:U8(std::get<bool>(v));break;
      case MetricScalarType::text:String(std::get<std::string>(v));break;
      case MetricScalarType::uuid:Raw(std::get<MetricUuid>(v).bytes.data(),16);break;
      case MetricScalarType::enumeration:U64(std::get<MetricEnumValue>(v).code);break;
      default:throw E::invalid_value;
    }
  }
};
}
MetricValueEncodeResult EncodeMetricValue(const MetricDescriptorDefinition& d,const MetricValue& v) noexcept {
  try {
    if(v.state_text.size()>kMetricValueMaxBytes||
        (std::holds_alternative<std::string>(v.value)&&std::get<std::string>(v.value).size()>kMetricValueMaxBytes))
      return {E::size_limit,{}};
    for(const auto& l:v.labels)
      if(std::holds_alternative<std::string>(l.value)&&std::get<std::string>(l.value).size()>kMetricValueMaxBytes)
        return {E::size_limit,{}};
    if(!Definition(d)||v.labels.size()>1024||!ValidateStoredMetricValueShape(d,v))return {};
    Writer w;w.Scalar(v.value);w.U64(v.count);
    if(d.type==MetricType::histogram) {
      w.Scalar(v.sum);
      for(std::size_t i=0;i<v.bucket_bounds.size();++i){w.Scalar(v.bucket_bounds[i]);w.U64(v.buckets[i]);}
      w.U64(v.buckets.back());
    }
    w.String(v.state_text);
    std::vector<const MetricLabel*> sorted;
    for(const auto& l:v.labels)sorted.push_back(&l);
    std::sort(sorted.begin(),sorted.end(),[](const auto* a,const auto* b){return Less(a->key,b->key);});
    for(const auto* l:sorted) {
      if(!Key(l->key))return {};
      const auto* schema=Label(d,l->key);if(!schema)return {};
      w.String(l->key);w.U8(Code(schema->value_type,labels));
      if(const auto* text=std::get_if<std::string>(&l->value)) {
        if(!Text(*text))return {};
        w.String(*text);
      } else w.Raw(std::get<MetricUuid>(l->value).bytes.data(),16);
    }
    auto& b=w.bytes;std::memcpy(b.data(),"SBMV",4);platform::StoreLittle16(b.data()+4,1);
    platform::StoreLittle16(b.data()+6,kMetricValueHeaderBytes);
    platform::StoreLittle32(b.data()+8,static_cast<std::uint32_t>(b.size()));
    b[12]=Code(d.type,classes);b[13]=Code(d.value_type,types);b[14]=v.buckets_cumulative|(v.arithmetic_inexact<<1);
    platform::StoreLittle32(b.data()+16,static_cast<std::uint32_t>(v.labels.size()));
    platform::StoreLittle32(b.data()+20,static_cast<std::uint32_t>(v.bucket_bounds.size()));
    return {E::none,std::move(b)};
  } catch(E e){return {e,{}};}
    catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
    catch(const std::length_error&){return {E::resource_exhausted,{}};}
    catch(...){return {};}
}
MetricValueDecodeResult DecodeMetricValue(const MetricDescriptorDefinition& d,std::span<const byte> b) noexcept {
  try {
    const auto decoded=DecodeMetricValueView(d,b);
    if(!decoded.ok())return {decoded.error,{}};
    return {E::none,MaterializeMetricValue(*decoded.value)};
  } catch(E e){return {e,{}};}
    catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
    catch(const std::length_error&){return {E::resource_exhausted,{}};}
    catch(...){return {};}
}
MetricScalar MetricHistogramView::bound(std::size_t index) const {
  const auto width=type_==MetricScalarType::float128||type_==MetricScalarType::decimal128?16:8;
  detail::ValueViewReader reader{bytes_,index*(width+8)};
  return reader.Scalar(type_).native;
}
u64 MetricHistogramView::bucket(std::size_t index) const {
  const auto width=type_==MetricScalarType::float128||type_==MetricScalarType::decimal128?16:8;
  return platform::LoadLittle64(bytes_.data()+index*(width+8)+(index<count_?width:0));
}
MetricValueLabelView MetricValueLabelsView::Iterator::operator*() const {
  MetricValueLabelView label;
  const auto length=platform::LoadLittle32(at_);
  label.key={reinterpret_cast<const char*>(at_+4),length};
  const auto* data=at_+4+length;
  if(*data++==1)label.value=std::string_view(reinterpret_cast<const char*>(data+4),platform::LoadLittle32(data));
  else {MetricUuid id;std::copy_n(data,16,id.bytes.begin());label.value=id;}
  return label;
}
MetricValueLabelsView::Iterator& MetricValueLabelsView::Iterator::operator++() {
  const auto* data=at_+4+platform::LoadLittle32(at_);
  const auto type=*data++;
  at_=data+(type==1?4+platform::LoadLittle32(data):16);
  return *this;
}
MetricValue MaterializeMetricValue(const MetricValueView& view) {
  auto scalar=[](const MetricScalarView& v)->MetricScalar {
    return v.text?MetricScalar(std::string(*v.text)):v.native;
  };
  MetricValue value;value.family=view.family;value.type=view.type;
  value.value=scalar(view.value);value.count=view.count;value.sum=scalar(view.sum);
  value.buckets_cumulative=view.buckets_cumulative;value.arithmetic_inexact=view.arithmetic_inexact;
  value.state_text=view.state_text;
  if(!view.histogram.empty()){
    value.bucket_bounds.reserve(view.histogram.size());value.buckets.reserve(view.histogram.size()+1);
    for(std::size_t i=0;i<view.histogram.size();++i){
      value.bucket_bounds.push_back(view.histogram.bound(i));value.buckets.push_back(view.histogram.bucket(i));
    }
    value.buckets.push_back(view.histogram.bucket(view.histogram.size()));
  }
  value.labels.reserve(view.labels.size());
  for(const auto label:view.labels){
    MetricLabel owned;owned.key=label.key;
    if(const auto* text=std::get_if<std::string_view>(&label.value))owned.value=std::string(*text);
    else owned.value=std::get<MetricUuid>(label.value);
    value.labels.push_back(std::move(owned));
  }
  return value;
}
}  // namespace scratchbird::core::metrics
