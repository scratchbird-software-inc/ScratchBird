// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_export.hpp"
#include "metric_value_codec.hpp"
#include "uuid.hpp"
#include "sbl_numeric.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace scratchbird::client::metrics {
using namespace scratchbird::core::metrics;
namespace {
using E=MetricExportError;
bool Name(const std::string& name) {
  const auto letter=[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
  return !name.empty()&&name.size()<=256&&letter(name[0])&&
      std::all_of(name.begin(),name.end(),[&](unsigned char c){return letter(c)||(c>='0'&&c<='9');});
}
bool Text(const std::string& text,std::size_t maximum) {
  return text.size()<=maximum&&MetricScalarValid(MetricScalar(text));
}
std::string Escape(const std::string& text,bool label) {
  std::string out;
  for(char c:text){if(c=='\n')out+="\\n";else{if(c=='\\'||(label&&c=='"'))out+='\\';out+=c;}}
  return out;
}
std::string Scalar(const MetricScalar& value) {
  if(!MetricScalarValid(value))throw E::invalid_value;
  switch(MetricScalarTypeOf(value)){
    case MetricScalarType::uint64:return std::to_string(std::get<std::uint64_t>(value));
    case MetricScalarType::int64:return std::to_string(std::get<std::int64_t>(value));
    case MetricScalarType::enumeration:return std::to_string(std::get<MetricEnumValue>(value).code);
    case MetricScalarType::boolean:return std::get<bool>(value)?"1":"0";
    case MetricScalarType::text:return std::get<std::string>(value);
    case MetricScalarType::uuid:return core::uuid::UuidToString(std::get<MetricUuid>(value));
    case MetricScalarType::float64:{
      char buffer[128];const auto r=std::to_chars(buffer,buffer+sizeof(buffer),std::get<double>(value),
          std::chars_format::general,std::numeric_limits<double>::max_digits10);
      if(r.ec!=std::errc{})throw E::numeric_failure;
      return {buffer,r.ptr};}
    case MetricScalarType::float128:{
      namespace n=scratchbird::libraries::sbl_numeric;
      const auto& bytes=std::get<MetricFloat128>(value).bytes;
      const auto r=n::DecodeReal128LittleEndian(bytes.data(),bytes.size(),{},true);
      if(r.numeric.status!=n::NumericStatusCode::ok||!r.bytes||r.numeric.value.encoded.empty())throw E::numeric_failure;
      return r.numeric.value.encoded;}
    case MetricScalarType::decimal128:{
      const auto& b=std::get<MetricDecimal128>(value).bytes;
      std::uint64_t high=0;for(unsigned i=0;i<8;++i)high|=std::uint64_t(b[i+8])<<(8*i);
      boost::multiprecision::uint128_t coefficient=high&UINT64_C(0x0001ffffffffffff);
      for(int i=7;i>=0;--i)coefficient=(coefficient<<8)|b[i];
      return std::string((high>>63)?"-":"")+coefficient.convert_to<std::string>()+"e"+
          std::to_string(int((high>>49)&0x3fff)-6176);}
    default:throw E::invalid_value;
  }
}
using Labels=std::map<std::string,std::string>;
std::string RenderLabels(const Labels& labels) {
  std::string text;for(const auto& [key,value]:labels){text+=text.empty()?"{":",";text+=key+"=\""+Escape(value,true)+"\"";}
  if(!text.empty())text+='}';
  return text;
}
bool SameDefinition(const MetricExportSample& a,const MetricExportSample& b) {
  const auto& x=a.descriptor;const auto& y=b.descriptor;
  if(static_cast<const MetricDescriptorBinding&>(x)!=static_cast<const MetricDescriptorBinding&>(y)||
      x.family!=y.family||x.type!=y.type||x.unit!=y.unit||x.value_type!=y.value_type||x.help!=y.help||
      x.namespace_path!=y.namespace_path||x.cluster_only!=y.cluster_only||x.producer_owner!=y.producer_owner||
      x.security_family!=y.security_family||x.visibility!=y.visibility||x.min_value!=y.min_value||x.max_value!=y.max_value||
      x.enum_values!=y.enum_values||x.histogram_buckets!=y.histogram_buckets||x.histogram_cumulative!=y.histogram_cumulative||
      x.rate_window_nanoseconds!=y.rate_window_nanoseconds||x.labels.size()!=y.labels.size()||a.label_rules!=b.label_rules)return false;
  for(std::size_t i=0;i<x.labels.size();++i){const auto& l=x.labels[i];const auto& r=y.labels[i];
    if(l.key!=r.key||l.required!=r.required||l.sensitive!=r.sensitive||l.value_type!=r.value_type)return false;}
  return true;
}
Labels ProjectLabels(const MetricExportSample& sample) {
  if(sample.label_rules.size()!=sample.descriptor.labels.size())throw E::invalid_projection;
  Labels result;std::set<std::string> sources,targets;
  for(const auto& rule:sample.label_rules){
    const auto d=std::find_if(sample.descriptor.labels.begin(),sample.descriptor.labels.end(),[&](const auto& l){return l.key==rule.source_key;});
    if(d==sample.descriptor.labels.end()||!sources.insert(rule.source_key).second||
        (rule.omit?!rule.export_key.empty():(!Name(rule.export_key)||d->sensitive||!targets.insert(rule.export_key).second)))throw E::invalid_projection;
    if(rule.omit)continue;
    if((sample.descriptor.type==MetricType::histogram&&rule.export_key=="le")||
        (sample.descriptor.type==MetricType::state&&rule.export_key=="sb_state_text")||
        ((sample.descriptor.value_type==MetricScalarType::text||sample.descriptor.value_type==MetricScalarType::uuid)&&rule.export_key=="sb_value"))throw E::name_collision;
    const auto v=std::find_if(sample.value.labels.begin(),sample.value.labels.end(),[&](const auto& l){return l.key==rule.source_key;});
    if(v!=sample.value.labels.end())result.emplace(rule.export_key,std::visit([](const auto& value)->std::string {
      using T=std::decay_t<decltype(value)>;if constexpr(std::is_same_v<T,std::string>)return value;
      else return core::uuid::UuidToString(value);},static_cast<const MetricLabelValue::Base&>(v->value)));
  }
  return result;
}
}
MetricExportResult RenderOpenMetricsProjection(const MetricExportContext& context,
    std::span<const MetricExportSample> samples,std::size_t maximum) noexcept {
  try {
    if(!core::uuid::IsEngineIdentityUuid(context.export_profile_uuid)||!core::uuid::IsEngineIdentityUuid(context.source_scope_uuid)||
        !core::uuid::IsEngineIdentityUuid(context.redaction_policy_uuid)||context.schema_version!=1||
        !context.observation_time_utc_ns||!context.export_time_utc_ns||context.residency_decision.empty()||
        !Text(context.residency_decision,256)||context.residency_decision.find_first_of(std::string("\n\r\0",3))!=std::string::npos)
      return {E::invalid_context,{}};
    if(!maximum||maximum>kMetricExportMaximumBytes||samples.size()>65536)return {E::size_limit,{}};
    std::string out;const auto append=[&](const std::string& s){if(s.size()>maximum-out.size())throw E::size_limit;out+=s;};
    const auto metadata=[&](const char* key,const std::string& value){append(std::string("# SCRATCHBIRD ")+key+" "+value+"\n");};
    metadata("export_profile_uuid",core::uuid::UuidToString(context.export_profile_uuid));
    metadata("schema_version","1");metadata("source_scope_uuid",core::uuid::UuidToString(context.source_scope_uuid));
    metadata("observation_time",std::to_string(context.observation_time_utc_ns));metadata("export_time",std::to_string(context.export_time_utc_ns));
    metadata("redaction_policy_uuid",core::uuid::UuidToString(context.redaction_policy_uuid));metadata("residency_decision",context.residency_decision);
    std::map<std::string,const MetricExportSample*> families;
    std::map<std::string,std::string> generated_names;
    std::map<std::pair<MetricUuid,u64>,std::string> identities;
    std::set<std::string> series;
    for(const auto& s:samples){const auto& d=s.descriptor;const auto& v=s.value;
      if(!Name(s.export_name)||!core::uuid::IsEngineIdentityUuid(d.metric_uuid)||!d.descriptor_generation||!Text(d.help,4096))throw E::invalid_projection;
      const auto encoded=EncodeMetricValue(d,v);if(!encoded.ok())throw encoded.error==MetricValueCodecError::resource_exhausted?E::resource_exhausted:E::invalid_value;
      auto labels=ProjectLabels(s);std::string base=s.export_name;
      const bool histogram=d.type==MetricType::histogram,counter=d.type==MetricType::counter;
      const bool info=d.value_type==MetricScalarType::text||d.value_type==MetricScalarType::uuid;
      if(counter&&base.ends_with("_total"))base.resize(base.size()-6);
      if(base.empty())throw E::invalid_projection;
      const auto identity=identities.emplace(std::make_pair(d.metric_uuid,d.descriptor_generation),base);
      if(!identity.second&&identity.first->second!=base)throw E::name_collision;
      const auto family=families.emplace(base,&s);
      if(!family.second&&!SameDefinition(*family.first->second,s))throw E::name_collision;
      const std::vector<std::string> names=histogram?std::vector<std::string>{base,base+"_bucket",base+"_sum",base+"_count"}:
          std::vector<std::string>{base,base+(counter?"_total":info?"_info":"")};
      for(const auto& name:names){const auto n=generated_names.emplace(name,base);if(!n.second&&n.first->second!=base)throw E::name_collision;}
      if(family.second){append("# HELP "+base+" "+Escape(d.help,false)+"\n");append("# TYPE "+base+" "+(histogram?"histogram":counter?"counter":info?"info":"gauge")+"\n");}
      const auto emit=[&](const std::string& name,const Labels& l,const std::string& value){const auto key=name+RenderLabels(l);
        if(!series.insert(key).second)throw E::duplicate_series;
        append(key+" "+value+"\n");};
      const auto add=[&](const char* key,std::string value){if(!labels.emplace(key,std::move(value)).second)throw E::name_collision;};
      if(histogram){u64 cumulative=0;
        for(std::size_t i=0;i<v.buckets.size();++i){const auto count=v.buckets[i];
          if(v.buckets_cumulative)cumulative=count;else{if(count>std::numeric_limits<u64>::max()-cumulative)throw E::invalid_value;cumulative+=count;}
          auto bucket=labels;if(!bucket.emplace("le",i<v.bucket_bounds.size()?Scalar(v.bucket_bounds[i]):"+Inf").second)throw E::name_collision;
          emit(base+"_bucket",bucket,std::to_string(cumulative));}
        emit(base+"_sum",labels,Scalar(v.sum));emit(base+"_count",labels,std::to_string(v.count));
      }else if(info){add("sb_value",Scalar(v.value));emit(base+"_info",labels,"1");}
      else{if(d.type==MetricType::state)add("sb_state_text",v.state_text);emit(base+(counter?"_total":""),labels,Scalar(v.value));}
    }
    append("# EOF\n");return {E::none,std::move(out)};
  }catch(E error){return {error,{}};}
   catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::size_limit,{}};}
   catch(...){return {E::invalid_projection,{}};}
}
} // namespace scratchbird::client::metrics
