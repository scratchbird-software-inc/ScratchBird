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
bool Text(const std::string& s) {return MetricScalarValid(MetricScalar(s));}
bool Key(const std::string& s) {
  return !s.empty()&&s.size()<=4096&&s.find('\0')==s.npos&&Text(s);
}
bool Less(const std::string& a,const std::string& b) {
  return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
      [](unsigned char x,unsigned char y){return x<y;});
}
const MetricLabelDescriptor* Label(const MetricDescriptorDefinition& d,const std::string& key) {
  const auto it=std::find_if(d.labels.begin(),d.labels.end(),[&](const auto& l){return l.key==key;});
  return it==d.labels.end()?nullptr:&*it;
}
bool Definition(const MetricDescriptorDefinition& d) {
  if(!Code(d.type,classes)||!Code(d.value_type,types)||d.labels.size()>1024||d.histogram_buckets.size()>4096)
    return false;
  for(std::size_t i=0;i<d.labels.size();++i) {
    if(!Key(d.labels[i].key)||!Code(d.labels[i].value_type,labels))return false;
    for(std::size_t j=0;j<i;++j)if(d.labels[j].key==d.labels[i].key)return false;
  }
  return true;
}
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
struct Reader {
  std::span<const byte> bytes;
  std::size_t offset=kMetricValueHeaderBytes;
  std::span<const byte> Raw(std::size_t n) {
    if(n>bytes.size()-offset)throw E::invalid_framing;
    auto r=bytes.subspan(offset,n);offset+=n;return r;
  }
  byte U8(){return Raw(1)[0];}
  std::uint32_t U32(){return platform::LoadLittle32(Raw(4).data());}
  u64 U64(){return platform::LoadLittle64(Raw(8).data());}
  std::string String(){auto b=Raw(U32());return {reinterpret_cast<const char*>(b.data()),b.size()};}
  MetricUuid Uuid(){MetricUuid id;auto b=Raw(16);std::copy(b.begin(),b.end(),id.bytes.begin());return id;}
  MetricScalar Scalar(MetricScalarType t) {
    switch(t) {
      case MetricScalarType::uint64:return U64();
      case MetricScalarType::int64:return std::bit_cast<std::int64_t>(U64());
      case MetricScalarType::float64:return std::bit_cast<double>(U64());
      case MetricScalarType::float128:{MetricFloat128 v;auto b=Raw(16);std::copy(b.begin(),b.end(),v.bytes.begin());return v;}
      case MetricScalarType::decimal128:{MetricDecimal128 v;auto b=Raw(16);std::copy(b.begin(),b.end(),v.bytes.begin());return v;}
      case MetricScalarType::boolean:{auto b=U8();if(b>1)throw E::invalid_value;return b!=0;}
      case MetricScalarType::text:return String();
      case MetricScalarType::uuid:return Uuid();
      case MetricScalarType::enumeration:return MetricEnumValue{U64()};
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
    if(b.size()>kMetricValueMaxBytes)return {E::size_limit,{}};
    if(b.size()<kMetricValueHeaderBytes||std::memcmp(b.data(),"SBMV",4)||
        platform::LoadLittle16(b.data()+6)!=kMetricValueHeaderBytes||
        platform::LoadLittle32(b.data()+8)!=b.size()||b[15]||(b[14]&~3u))return {E::invalid_framing,{}};
    if(platform::LoadLittle16(b.data()+4)!=1)return {E::unsupported_version,{}};
    const auto count=platform::LoadLittle32(b.data()+16),bounds=platform::LoadLittle32(b.data()+20);
    if(count>1024||bounds>4096)return {E::size_limit,{}};
    if(!Definition(d)||b[12]!=Code(d.type,classes)||b[13]!=Code(d.value_type,types)||
        bounds!=d.histogram_buckets.size())return {E::descriptor_mismatch,{}};
    Reader r{b};MetricValue v;v.family=d.family;v.type=d.type;
    v.buckets_cumulative=b[14]&1;v.arithmetic_inexact=b[14]&2;
    v.value=r.Scalar(d.value_type);v.count=r.U64();
    if(d.type==MetricType::histogram) {
      v.sum=r.Scalar(d.value_type);
      for(std::size_t i=0;i<bounds;++i){v.bucket_bounds.push_back(r.Scalar(d.value_type));v.buckets.push_back(r.U64());}
      v.buckets.push_back(r.U64());
    }
    v.state_text=r.String();
    for(std::size_t i=0;i<count;++i) {
      auto key=r.String();
      if(!Key(key)||(i&&!Less(v.labels.back().key,key)))return {};
      const auto* schema=Label(d,key);const auto tag=r.U8();
      if(!schema||tag!=Code(schema->value_type,labels))return {E::descriptor_mismatch,{}};
      MetricLabelValue value;
      if(tag==1){auto text=r.String();if(!Text(text))return {};value=std::move(text);}
      else value=r.Uuid();
      v.labels.push_back({std::move(key),std::move(value)});
    }
    if(r.offset!=b.size())return {E::invalid_framing,{}};
    if(!ValidateStoredMetricValueShape(d,v))return {};
    return {E::none,std::move(v)};
  } catch(E e){return {e,{}};}
    catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
    catch(const std::length_error&){return {E::resource_exhausted,{}};}
    catch(...){return {};}
}
}  // namespace scratchbird::core::metrics
