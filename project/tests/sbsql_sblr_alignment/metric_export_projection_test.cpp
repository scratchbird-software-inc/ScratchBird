// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define METRIC_VALUE_CODEC_MAIN MetricExportValueBaseMain
#include "metric_value_codec_test.cpp"
#undef METRIC_VALUE_CODEC_MAIN
#include "metric_export.hpp"
#include <cfenv>
#include <charconv>
#include <locale>
namespace {
using XE=m::MetricExportError;
m::MetricExportContext Context(){return {Id(1),Id(2),Id(3),1,100,200,"local"};}
m::MetricExportSample ExportSample(m::MetricScalar value=U(9007199254740993ULL)){
  m::MetricExportSample s;s.descriptor=Descriptor(m::MetricScalarTypeOf(value));
  s.descriptor.metric_uuid=Id(4);s.descriptor.descriptor_generation=1;s.descriptor.help="example";
  if(s.descriptor.value_type==T::enumeration)s.descriptor.enum_values={std::get<m::MetricEnumValue>(value).code};
  s.value=Value(s.descriptor,std::move(value));s.export_name="example";return s;
}
m::MetricExportResult Render(const m::MetricExportSample& s,std::size_t limit=m::kMetricExportMaximumBytes){
  return m::RenderOpenMetricsProjection(Context(),std::span(&s,1),limit);
}
void ExportRefused(const m::MetricExportResult& r){Check(!r.ok()&&r.text.empty(),"invalid export exposed an output prefix");}
void Has(const m::MetricExportResult& r,const std::string& text){Check(r.ok()&&r.text.find(text)!=std::string::npos,"missing exact export bytes");}
void ProjectionBasics(){
  auto s=ExportSample();const auto r=Render(s);
  const std::string expected=
    "# SCRATCHBIRD export_profile_uuid 00000000-0000-7000-8000-000000000001\n"
    "# SCRATCHBIRD schema_version 1\n"
    "# SCRATCHBIRD source_scope_uuid 00000000-0000-7000-8000-000000000002\n"
    "# SCRATCHBIRD observation_time 100\n# SCRATCHBIRD export_time 200\n"
    "# SCRATCHBIRD redaction_policy_uuid 00000000-0000-7000-8000-000000000003\n"
    "# SCRATCHBIRD residency_decision local\n"
    "# HELP example example\n# TYPE example gauge\nexample 9007199254740993\n# EOF\n";
  Check(r.ok()&&r.text==expected,"full export differs from independent byte oracle");
  Check(Render(s,expected.size()).text==expected,"exact export limit rejected");
  for(std::size_t n=0;n<expected.size();++n)ExportRefused(Render(s,n));
  ExportRefused(Render(s,m::kMetricExportMaximumBytes+1));
  for(unsigned n=0;n<9;++n){auto c=Context();
    if(n==0)c.export_profile_uuid={};if(n==1)c.source_scope_uuid=Id(2,4);if(n==2)c.redaction_policy_uuid.bytes[8]=0;
    if(n==3)c.schema_version=2;if(n==4)c.observation_time_utc_ns=0;if(n==5)c.export_time_utc_ns=0;
    if(n==6)c.residency_decision="";if(n==7)c.residency_decision="x\n# EOF";if(n==8)c.residency_decision=std::string("a\0b",3);
    ExportRefused(m::RenderOpenMetricsProjection(c,std::span(&s,1)));
  }
  for(const auto& name:{"","_total","a.b","x\n# EOF","1bad","x:y"}){auto bad=s;bad.export_name=name;
    if(bad.export_name=="_total"){bad.descriptor.type=bad.value.type=m::MetricType::counter;}
    ExportRefused(Render(bad));}
  auto bad=s;bad.descriptor.metric_uuid=Id(4,1);ExportRefused(Render(bad));
  bad=s;bad.descriptor.descriptor_generation=0;ExportRefused(Render(bad));
  bad=s;bad.value.value=std::numeric_limits<double>::infinity();ExportRefused(Render(bad));
  bad=s;bad.descriptor.help=std::string("\xc0\xaf",2);ExportRefused(Render(bad));
  bad=s;bad.export_name=std::string(257,'a');ExportRefused(Render(bad));
  s.descriptor.help="h\\\n# EOF";Has(Render(s),"# HELP example h\\\\\\n# EOF\n# TYPE example gauge");
  const auto empty=m::RenderOpenMetricsProjection(Context(),{});
  Check(empty.ok()&&empty.text.ends_with("# EOF\n")&&empty.text.find("# TYPE")==std::string::npos,"empty selected projection manufactured a sample");
}
void ProjectionScalars(){
  for(const auto& pair:std::vector<std::pair<m::MetricScalar,std::string>>{
      {U(-1),"18446744073709551615"},{std::numeric_limits<I>::min(),"-9223372036854775808"},
      {-0.,"-0"},{true,"1"},{false,"0"},{m::MetricEnumValue{U(-1)},"18446744073709551615"},
      {Decimal("9999999999999999999999999999999999",6111),"9999999999999999999999999999999999e6111"},
      {Decimal("0",-6176,true),"-0e-6176"}})Has(Render(ExportSample(pair.first)),"example "+pair.second+"\n");
  for(const auto& number:{0.,-0.,std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max(),1.0000000000000002}){
    auto r=Render(ExportSample(number));Check(r.ok(),"binary64 export failed");if(!r.ok())continue;
    const auto at=r.text.find("\nexample ")+9,end=r.text.find('\n',at);double decoded=0;
    const auto parsed=std::from_chars(r.text.data()+at,r.text.data()+end,decoded);
    Check(parsed.ec==std::errc{}&&parsed.ptr==r.text.data()+end&&std::bit_cast<U>(decoded)==std::bit_cast<U>(number),"binary64 export lost exact value or signed zero");
  }
  for(const auto& value:{Binary("1e4000"),Binary("-0"),Binary("1e-4950"),Binary("1.0000000000000000000000000000000002")}){
    const auto r=Render(ExportSample(value));Check(r.ok(),"binary128 export failed");if(!r.ok())continue;
    const auto at=r.text.find("\nexample ")+9,end=r.text.find('\n',at);
    const auto parsed=numeric::EncodeReal128LittleEndian(r.text.substr(at,end-at));
    Check(parsed.bytes&&*parsed.bytes==value.bytes,"binary128 text export narrowed value");
  }
  const auto text=std::string("x\0\"\\\n\xc3\xa9",7);auto s=ExportSample(text);
  const auto r=Render(s);const std::string needle=std::string("example_info{sb_value=\"x")+std::string(1,'\0')+"\\\"\\\\\\n\xc3\xa9\"} 1\n";
  Has(r,needle);Has(r,"# TYPE example info\n");
  Has(Render(ExportSample(Id(42,1))),"example_info{sb_value=\"00000000-0000-1000-8000-00000000002a\"} 1\n");
  s=ExportSample(m::MetricEnumValue{7});s.descriptor.type=s.value.type=m::MetricType::state;s.value.state_text="ready\nnow";
  Has(Render(s),"example{sb_state_text=\"ready\\nnow\"} 7\n");
  s=ExportSample(U(42));s.descriptor.type=s.value.type=m::MetricType::counter;s.export_name="observed_total";
  Has(Render(s),"# TYPE observed counter\nobserved_total 42\n");
  s=ExportSample(U(4));s.descriptor.type=s.value.type=m::MetricType::rate;s.descriptor.rate_window_nanoseconds=100;
  Has(Render(s),"# TYPE example gauge\nexample 4\n");
}
void ProjectionHistograms(){
  for(bool cumulative:{false,true}){auto s=ExportSample(U(3));auto& d=s.descriptor;auto& v=s.value;
    d.type=v.type=m::MetricType::histogram;d.histogram_buckets={U(1),U(2)};d.histogram_cumulative=cumulative;
    v.bucket_bounds=d.histogram_buckets;v.buckets_cumulative=cumulative;v.buckets=cumulative?std::vector<U>{1,3,4}:std::vector<U>{1,2,1};v.sum=U(7);v.count=4;
    Has(Render(s),"# TYPE example histogram\nexample_bucket{le=\"1\"} 1\nexample_bucket{le=\"2\"} 3\nexample_bucket{le=\"+Inf\"} 4\nexample_sum 7\nexample_count 4\n# EOF\n");
    v.buckets.back()=0;ExportRefused(Render(s));
  }
  for(const auto& numbers:std::vector<std::vector<m::MetricScalar>>{
      {I(-1),I(3),I(2),I(4)},{-1.,3.,2.,4.},
      {Binary("-1"),Binary("3"),Binary("2"),Binary("4")},
      {Decimal("1",0,true),Decimal("3"),Decimal("2"),Decimal("4")}}){
    auto s=ExportSample(numbers[2]);s.descriptor.type=s.value.type=m::MetricType::histogram;
    s.descriptor.histogram_buckets={numbers[0],numbers[1]};s.value.bucket_bounds=s.descriptor.histogram_buckets;
    s.value.buckets={1,3,3};s.value.count=3;s.value.sum=numbers[3];
    const auto r=Render(s);Has(r,"example_bucket{le=\"+Inf\"} 3\n");Has(r,"example_count 3\n");
  }
}
void ProjectionLabelsAndCollisions(){
  auto s=ExportSample();s.descriptor.labels={{"secret",true,true,m::MetricLabelType::system_uuid},{"text",false,false,m::MetricLabelType::text}};
  s.value.labels={{"secret",Id(44)},{"text",std::string("v\n\"\\")}};
  s.label_rules={{"secret","",true},{"text","label",false}};
  const auto r=Render(s);Has(r,"example{label=\"v\\n\\\"\\\\\"} 9007199254740993\n");
  Check(r.text.find("00000000002c")==std::string::npos,"sensitive UUID leaked");
  auto bad=s;bad.label_rules[0]={"secret","exposed",false};ExportRefused(Render(bad));
  bad=s;bad.label_rules.pop_back();ExportRefused(Render(bad));
  bad=s;bad.label_rules[1].source_key="unknown";ExportRefused(Render(bad));
  bad=s;bad.label_rules[1].export_key="bad\nkey";ExportRefused(Render(bad));
  std::vector<m::MetricExportSample> all{s,s};ExportRefused(m::RenderOpenMetricsProjection(Context(),all));
  all[1].value.labels[0].value=Id(45);ExportRefused(m::RenderOpenMetricsProjection(Context(),all));
  all[1].value.labels[1].value=std::string("other");Check(m::RenderOpenMetricsProjection(Context(),all).ok(),"distinct nonredacted series refused");
  all[1].descriptor.help="conflicting";ExportRefused(m::RenderOpenMetricsProjection(Context(),all));
  all={ExportSample(U(1)),ExportSample(U(2))};all[0].descriptor.type=all[0].value.type=m::MetricType::counter;
  all[1].descriptor.metric_uuid=Id(5);all[1].export_name="example_total";ExportRefused(m::RenderOpenMetricsProjection(Context(),all));
  auto info=ExportSample(std::string("test"));info.descriptor.labels={{"optional",false,false,m::MetricLabelType::text}};
  info.label_rules={{"optional","sb_value",false}};ExportRefused(Render(info));
  s=ExportSample();s.descriptor.labels={{"\xc3\xa9",true,false,m::MetricLabelType::text}};
  s.value.labels={{"\xc3\xa9",std::string("value")}};s.label_rules={{"\xc3\xa9","unicode_alias",false}};
  Has(Render(s),"example{unicode_alias=\"value\"} 9007199254740993\n");
  s.label_rules[0].export_key=std::string(256,'a');Check(Render(s).ok(),"maximum external key rejected");
  s.label_rules[0].export_key.push_back('a');ExportRefused(Render(s));
  s=ExportSample();s.export_name=std::string(256,'a');Check(Render(s).ok(),"maximum external name rejected");
}
struct CommaPunctuation:std::numpunct<char>{char do_decimal_point()const override{return ',';}char do_thousands_sep()const override{return '.';}std::string do_grouping()const override{return "\3";}};
void ProjectionEnvironment(){
  const std::vector<m::MetricExportSample> samples={ExportSample(1.2345678901234567),ExportSample(U(-1)),
      ExportSample(Decimal("1234567890123456789012345678901234",-17)),ExportSample(Binary("1e4000"))};
  std::vector<std::string> expected;for(const auto& s:samples)expected.push_back(Render(s).text);
  const auto locale=std::locale();std::locale::global(std::locale(locale,new CommaPunctuation));
  const auto rounding=std::fegetround();
  for(int mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO,FE_TONEAREST}){Check(std::fesetround(mode)==0,"set floating environment");
    for(std::size_t i=0;i<samples.size();++i)Check(Render(samples[i]).text==expected[i],"export depends on locale or floating environment");}
  std::fesetround(rounding);std::locale::global(locale);
}
void ProjectionFaults(){
  auto s=ExportSample(Decimal("1234567890123456789012345678901234",-7));
  s.descriptor.labels={{"label",true,false,m::MetricLabelType::text}};
  s.value.labels={{"label",std::string(200,'x')}};s.label_rules={{"label","label",false}};
  const auto before=Render(s).text;unsigned faults=0;bool complete=false;
  for(long point=0;point<1000;++point){codec_fault::remaining=point;codec_fault::fired=false;
    auto r=Render(s);const auto fired=codec_fault::fired;codec_fault::remaining=-1;
    if(fired){++faults;ExportRefused(r);}else{Check(r.ok()&&r.text==before,"unfaulted export changed");complete=true;break;}
    Check(Render(s).text==before,"export recovery mutated its input");
  }
  Check(complete&&faults,"export allocation sweep incomplete");std::cout<<"export allocation sites="<<faults<<'\n';
}
}
int main(){MetricExportValueBaseMain();const auto before=checks;ProjectionBasics();ProjectionScalars();ProjectionHistograms();ProjectionLabelsAndCollisions();ProjectionEnvironment();ProjectionFaults();
  numeric::ReleaseReal128ThreadCache();std::cout<<"metric export checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
