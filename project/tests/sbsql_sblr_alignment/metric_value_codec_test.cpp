// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main MetricScalarCodecBaseMain
#include "metric_scalar_test.cpp"
#undef main
#include "metric_value_codec.hpp"
#include "metric_value_update.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <new>

namespace codec_fault {
thread_local long remaining=-1;
thread_local bool fired=false;
void* Allocate(std::size_t n) {
  if(remaining==0){fired=true;throw std::bad_alloc();}
  if(remaining>0)--remaining;
  if(void* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
}
void* operator new(std::size_t n){return codec_fault::Allocate(n);}
void* operator new[](std::size_t n){return codec_fault::Allocate(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,const std::nothrow_t&) noexcept{try{return codec_fault::Allocate(n);}catch(...){return nullptr;}}
void* operator new[](std::size_t n,const std::nothrow_t&) noexcept{try{return codec_fault::Allocate(n);}catch(...){return nullptr;}}
void operator delete(void* p,const std::nothrow_t&) noexcept{std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept{std::free(p);}

namespace {
using Bytes=std::vector<unsigned char>;
void Put(Bytes& b,std::size_t at,U n,unsigned width){for(unsigned i=0;i<width;++i)b.at(at+i)=n>>(8*i);}
void Append(Bytes& b,U n,unsigned width){auto at=b.size();b.resize(at+width);Put(b,at,n,width);}
void TextBytes(Bytes& b,const std::string& s){Append(b,s.size(),4);b.insert(b.end(),s.begin(),s.end());}
void ScalarBytes(Bytes& b,const m::MetricScalar& v) {
  switch(m::MetricScalarTypeOf(v)){
    case T::uint64:Append(b,std::get<U>(v),8);break;
    case T::int64:Append(b,static_cast<U>(std::get<I>(v)),8);break;
    case T::float64:Append(b,std::bit_cast<U>(std::get<double>(v)),8);break;
    case T::float128:{auto& a=std::get<m::MetricFloat128>(v).bytes;b.insert(b.end(),a.begin(),a.end());break;}
    case T::decimal128:{auto& a=std::get<m::MetricDecimal128>(v).bytes;b.insert(b.end(),a.begin(),a.end());break;}
    case T::boolean:b.push_back(std::get<bool>(v));break;
    case T::text:TextBytes(b,std::get<std::string>(v));break;
    case T::uuid:{auto& a=std::get<m::MetricUuid>(v).bytes;b.insert(b.end(),a.begin(),a.end());break;}
    case T::enumeration:Append(b,std::get<m::MetricEnumValue>(v).code,8);break;
    default:throw "invalid oracle scalar";
  }
}
Bytes Golden(const m::MetricDescriptorDefinition& d,const m::MetricValue& v) {
  Bytes b(24,0);b[0]='S';b[1]='B';b[2]='M';b[3]='V';Put(b,4,1,2);Put(b,6,24,2);
  switch(d.type){case m::MetricType::counter:b[12]=1;break;case m::MetricType::gauge:b[12]=2;break;
    case m::MetricType::histogram:b[12]=3;break;case m::MetricType::rate:b[12]=4;break;
    case m::MetricType::state:b[12]=5;break;case m::MetricType::sample:b[12]=6;break;default:throw "invalid class";}
  b[13]=static_cast<unsigned>(d.value_type);b[14]=v.buckets_cumulative+2*v.arithmetic_inexact;
  Put(b,16,v.labels.size(),4);Put(b,20,v.bucket_bounds.size(),4);
  ScalarBytes(b,v.value);Append(b,v.count,8);
  if(d.type==m::MetricType::histogram){
    ScalarBytes(b,v.sum);
    for(std::size_t i=0;i<v.bucket_bounds.size();++i){ScalarBytes(b,v.bucket_bounds[i]);Append(b,v.buckets[i],8);}
    Append(b,v.buckets.back(),8);
  }
  TextBytes(b,v.state_text);
  auto ls=v.labels;std::sort(ls.begin(),ls.end(),[](const auto& a,const auto& z){return a.key<z.key;});
  for(const auto& l:ls){
    TextBytes(b,l.key);auto schema=std::find_if(d.labels.begin(),d.labels.end(),[&](const auto& s){return s.key==l.key;});
    b.push_back(static_cast<unsigned>(schema->value_type)+1);
    if(const auto* t=std::get_if<std::string>(&l.value))TextBytes(b,*t);
    else {auto& id=std::get<m::MetricUuid>(l.value).bytes;b.insert(b.end(),id.begin(),id.end());}
  }
  Put(b,8,b.size(),4);return b;
}
m::MetricValue Value(const m::MetricDescriptorDefinition& d,m::MetricScalar scalar) {
  m::MetricValue v;v.family=d.family;v.type=d.type;v.value=std::move(scalar);return v;
}
void Rejected(const m::MetricValueDecodeResult& r){Check(!r.ok()&&!r.value,"decode failure exposed a value");}
void EncodeRejected(const m::MetricDescriptorDefinition& d,const m::MetricValue& v){
  auto r=m::EncodeMetricValue(d,v);Check(!r.ok()&&r.bytes.empty(),"invalid value encoded");
}
void RoundTrip(const m::MetricDescriptorDefinition& d,const m::MetricValue& v) {
  const auto expected=Golden(d,v);auto encoded=m::EncodeMetricValue(d,v);
  Check(encoded.ok()&&encoded.bytes==expected,"encoding disagrees with independent SBMV layout");
  auto decoded=m::DecodeMetricValue(d,expected);
  Check(decoded.ok(),"independent SBMV bytes rejected");
  if(decoded.ok())Check(Golden(d,*decoded.value)==expected,"decoder narrowed or changed bits/labels");
  for(std::size_t n=0;n<expected.size();++n)Rejected(m::DecodeMetricValue(d,std::span(expected).first(n)));
}
void ScalarsAndClasses() {
  std::vector<m::MetricScalar> values={U(0),U(-1),U(9007199254740993ULL),std::numeric_limits<I>::min(),
    std::numeric_limits<I>::max(),0.,-0.,std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max(),
    Binary("1e4000"),Binary("-0"),Decimal("9999999999999999999999999999999999",6111),
    Decimal("0",-6176,true),true,false,std::string(),std::string("x\0y",3),std::string("\xc3\xa9"),
    Id(1,1),Id(2,7),m::MetricEnumValue{U(-1)}};
  for(const auto& s:values)for(auto cls:{m::MetricType::gauge,m::MetricType::sample}) {
    auto d=Descriptor(m::MetricScalarTypeOf(s));d.type=cls;if(d.value_type==T::enumeration)d.enum_values={U(-1)};
    RoundTrip(d,Value(d,s));
  }
  auto d=Descriptor(T::enumeration);d.type=m::MetricType::state;d.enum_values={0,U(-1)};
  auto v=Value(d,m::MetricEnumValue{U(-1)});v.state_text="state\n\xc3\xa9";RoundTrip(d,v);
  for(auto cls:{m::MetricType::counter,m::MetricType::rate})for(const auto& s:
      std::vector<m::MetricScalar>{U(-1),I(1),-0.,Binary("1e4000"),Decimal("1",-6176)}) {
    d=Descriptor(m::MetricScalarTypeOf(s));d.type=cls;if(cls==m::MetricType::rate)d.rate_window_nanoseconds=1000000000;
    v=Value(d,s);v.arithmetic_inexact=true;RoundTrip(d,v);
    if(cls==m::MetricType::rate)Check(!m::StageMetricValueUpdate(d,{},nullptr,s).ok(),"stored-rate support enabled raw rate production");
  }
  for(bool cumulative:{false,true}) {
    d=Descriptor(T::uint64);d.type=m::MetricType::histogram;d.histogram_buckets={U(1),U(-2)};d.histogram_cumulative=cumulative;
    v=Value(d,U(-1));v.count=3;v.sum=U(-1);v.bucket_bounds=d.histogram_buckets;
    v.buckets=cumulative?std::vector<U>{1,2,3}:std::vector<U>{1,1,1};v.buckets_cumulative=cumulative;v.arithmetic_inexact=true;
    RoundTrip(d,v);v.buckets.back()=0;EncodeRejected(d,v);
  }
  for(const auto& numbers:std::vector<std::vector<m::MetricScalar>>{
      {I(-1),I(3),I(2),I(4)},{-1.,3.,2.,4.},
      {Binary("-1"),Binary("3"),Binary("2"),Binary("4")},
      {Decimal("1",0,true),Decimal("3"),Decimal("2"),Decimal("4")}}) {
    d=Descriptor(m::MetricScalarTypeOf(numbers[0]));d.type=m::MetricType::histogram;
    d.histogram_buckets={numbers[0],numbers[1]};v=Value(d,numbers[2]);
    v.count=3;v.sum=numbers[3];v.bucket_bounds=d.histogram_buckets;v.buckets={1,3,3};
    RoundTrip(d,v);
    auto mismatch=Golden(d,v);mismatch[20]=1;Rejected(m::DecodeMetricValue(d,mismatch));
  }
}
void LabelsAndInvalid() {
  auto d=Descriptor(T::uint64);d.labels={{"z",true,true,m::MetricLabelType::system_uuid},
    {"a",true,false,m::MetricLabelType::uuid_value},{"t",false,false,m::MetricLabelType::text}};
  auto v=Value(d,U(-1));v.labels={{"z",Id(1)},{"a",Id(2,1)},{"t",std::string("=;\0\xc3\xa9",6)}};
  RoundTrip(d,v);for(unsigned version=1;version<=7;++version){v.labels[1].value=Id(2,version);RoundTrip(d,v);}
  const auto bytes=Golden(d,v);
  for(auto at:{0u,6u,8u,12u,13u,15u}){auto bad=bytes;bad[at]^=0x80;Rejected(m::DecodeMetricValue(d,bad));}
  for(unsigned flag=4;flag<256;++flag){auto bad=bytes;bad[14]=flag;Rejected(m::DecodeMetricValue(d,bad));}
  auto bad=bytes;Put(bad,4,2,2);Rejected(m::DecodeMetricValue(d,bad));
  for(auto at:{16,20}){bad=bytes;Put(bad,at,0xffffffff,4);Rejected(m::DecodeMetricValue(d,bad));}
  bad=bytes;bad.push_back(0);Put(bad,8,bad.size(),4);Rejected(m::DecodeMetricValue(d,bad));
  // First label begins at byte44: key length, key a, type, UUID.
  bad=bytes;bad[49]=1;Rejected(m::DecodeMetricValue(d,bad));
  bad=bytes;bad[56]=0xf0;Rejected(m::DecodeMetricValue(d,bad));
  bad=bytes;Put(bad,44,0xffffffff,4);Rejected(m::DecodeMetricValue(d,bad));
  // Same three valid label entries in a noncanonical order.
  bad.assign(bytes.begin(),bytes.begin()+44);
  bad.insert(bad.end(),bytes.end()-22,bytes.end());
  bad.insert(bad.end(),bytes.begin()+44,bytes.end()-22);
  Rejected(m::DecodeMetricValue(d,bad));
  auto wrong=d;wrong.labels[0].value_type=m::MetricLabelType::text;Rejected(m::DecodeMetricValue(wrong,bytes));
  v.labels[0].value=std::string("00000000-0000-7000-8000-000000000001");EncodeRejected(d,v);
  v.labels[0].value=Id(1,4);EncodeRejected(d,v);v.labels[0].value=Id(1);
  v.labels[2].value=std::string("\xc0\x80",2);EncodeRejected(d,v);
  v.labels[2].value=std::string();EncodeRejected(d,v);
  v.labels.pop_back();v.labels.push_back(v.labels[0]);EncodeRejected(d,v);
  auto boolean=Descriptor(T::boolean);auto bv=Value(boolean,true);bad=Golden(boolean,bv);bad[24]=2;Rejected(m::DecodeMetricValue(boolean,bad));
  auto real=Descriptor(T::float64);bad=Golden(real,Value(real,1.));Put(bad,24,0x7ff0000000000000ULL,8);Rejected(m::DecodeMetricValue(real,bad));
  auto rate=Descriptor(T::float64);rate.type=m::MetricType::rate;rate.rate_window_nanoseconds=1;
  EncodeRejected(rate,Value(rate,-1.));rate.rate_window_nanoseconds=0;EncodeRejected(rate,Value(rate,1.));
  auto text=Descriptor(T::text);auto tv=Value(text,std::string(1048576-40,'x'));
  auto max=m::EncodeMetricValue(text,tv);Check(max.ok()&&max.bytes.size()==1048576,"exact carrier cap rejected");
  if(max.ok())Check(m::DecodeMetricValue(text,max.bytes).ok(),"maximum carrier decode");
  std::get<std::string>(tv.value).push_back('x');EncodeRejected(text,tv);
  Bytes oversized(1048577,0);Rejected(m::DecodeMetricValue(text,oversized));
  auto bounded=Descriptor(T::uint64);bounded.min_value=U(9);bounded.max_value=U(10);
  EncodeRejected(bounded,Value(bounded,U(8)));EncodeRejected(bounded,Value(bounded,U(11)));
  auto enum_d=Descriptor(T::enumeration);enum_d.enum_values={1};
  EncodeRejected(enum_d,Value(enum_d,m::MetricEnumValue{2}));
  auto malformed=Descriptor(T::uint64);malformed.labels={{std::string("\xc0\x80",2),false,false,m::MetricLabelType::text}};
  EncodeRejected(malformed,Value(malformed,U(1)));
  malformed.labels={{"x",false,false,m::MetricLabelType::text},{"x",false,false,m::MetricLabelType::text}};
  EncodeRejected(malformed,Value(malformed,U(1)));
  auto histogram=Descriptor(T::uint64);histogram.type=m::MetricType::histogram;
  for(U n=0;n<4096;++n)histogram.histogram_buckets.push_back(n);
  auto hv=Value(histogram,U(4096));hv.count=1;hv.sum=U(4096);hv.bucket_bounds=histogram.histogram_buckets;
  hv.buckets.resize(4097);hv.buckets.back()=1;
  auto max_hist=m::EncodeMetricValue(histogram,hv);Check(max_hist.ok(),"4096-bound histogram refused");
  if(max_hist.ok())Check(m::DecodeMetricValue(histogram,max_hist.bytes).ok(),"4096-bound histogram decode");
  histogram.histogram_buckets.push_back(U(4096));hv.bucket_bounds=histogram.histogram_buckets;hv.buckets.push_back(1);EncodeRejected(histogram,hv);
  auto many=Descriptor(T::uint64);auto mv=Value(many,U(1));
  for(unsigned n=0;n<1024;++n){auto key="label-"+std::to_string(n);many.labels.push_back({key,true,false,m::MetricLabelType::text});mv.labels.push_back({key,std::string("v")});}
  auto max_labels=m::EncodeMetricValue(many,mv);Check(max_labels.ok(),"1024-label value refused");
  if(max_labels.ok())Check(m::DecodeMetricValue(many,max_labels.bytes).ok(),"1024-label value decode");
  many.labels.push_back({"extra",false,false,m::MetricLabelType::text});EncodeRejected(many,mv);
}
void Faults() {
  auto d=Descriptor(T::text);d.family=std::string(80,'f');d.labels={{"label",true,false,m::MetricLabelType::text}};
  auto v=Value(d,std::string(80,'v'));v.labels={{"label",std::string(80,'l')}};const auto expected=Golden(d,v);
  for(unsigned operation=0;operation<2;++operation) {
    unsigned failures_seen=0;bool completed=false;
    for(long point=0;point<1000;++point) {
      codec_fault::remaining=point;codec_fault::fired=false;
      bool ok=false,partial=false;
      if(operation==0){auto r=m::EncodeMetricValue(d,v);ok=r.ok();partial=!ok&&!r.bytes.empty();}
      else {auto r=m::DecodeMetricValue(d,expected);ok=r.ok();partial=!ok&&r.value.has_value();}
      const bool fired=codec_fault::fired;codec_fault::remaining=-1;
      Check(!partial,"allocation failure exposed partial result");
      if(fired){++failures_seen;Check(!ok,"allocation failure reported success");}
      else {Check(ok,"unfaulted operation failed");completed=true;break;}
      Check(Golden(d,v)==expected,"allocation failure mutated input");
    }
    Check(completed&&failures_seen>0,"fault sweep did not cover/recover allocations");
    std::cout<<"codec allocation operation="<<operation<<" injected="<<failures_seen<<'\n';
  }
}
}
#ifndef METRIC_VALUE_CODEC_MAIN
#define METRIC_VALUE_CODEC_MAIN main
#endif
int METRIC_VALUE_CODEC_MAIN() {
  MetricScalarCodecBaseMain();auto before=checks;
  ScalarsAndClasses();LabelsAndInvalid();Faults();
  std::cout<<"metric binary value checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
