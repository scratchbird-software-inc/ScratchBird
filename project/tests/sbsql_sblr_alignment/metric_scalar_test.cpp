// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_history.hpp"
#include "sbl_numeric.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <type_traits>

namespace m=scratchbird::core::metrics;
namespace numeric=scratchbird::libraries::sbl_numeric;
using U=std::uint64_t;
using I=std::int64_t;
using T=m::MetricScalarType;
using E=m::MetricScalarError;
using Big=boost::multiprecision::cpp_int;
namespace {
unsigned checks=0,failures=0;
void Check(bool ok,const char* why){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<why<<'\n';}}
// Independent bit-by-bit decimal oracle: no production coefficient helpers.
m::MetricDecimal128 Decimal(const std::string& digits,int exponent=0,bool negative=false){
  m::MetricDecimal128 out;
  for(char digit:digits){unsigned carry=unsigned(digit-'0');for(auto& byte:out.bytes){carry+=unsigned(byte)*10;byte=carry&255;carry>>=8;}Check(carry==0,"decimal oracle overflow");}
  const unsigned biased=unsigned(exponent+6176);
  for(unsigned i=0;i<14;++i)if(biased&(1u<<i))out.bytes[(113+i)/8]|=1u<<((113+i)%8);
  if(negative)out.bytes[15]|=0x80;
  return out;
}
m::MetricFloat128 Binary(const std::string& text){
  const auto encoded=numeric::EncodeReal128LittleEndian(text);
  Check(encoded.numeric.status==numeric::NumericStatusCode::ok&&encoded.bytes.has_value(),"independent numeric binary128 construction");
  m::MetricFloat128 out;if(encoded.bytes)out.bytes=*encoded.bytes;return out;
}
m::MetricUuid Id(unsigned tag,unsigned version=7){m::MetricUuid id;id.bytes[6]=version<<4;id.bytes[8]=0x80;id.bytes[15]=tag;return id;}
m::MetricDescriptor Descriptor(T type){m::MetricDescriptor d;d.value_type=type;d.type=m::MetricType::gauge;d.unit=m::MetricUnit::none;d.family="exact-test";return d;}
void Order(const m::MetricScalar& a,const m::MetricScalar& b,int expected){
  const auto forward=m::CompareMetricScalars(a,b),reverse=m::CompareMetricScalars(b,a);
  Check(forward&&*forward==expected,"exact forward numeric ordering");
  Check(reverse&&*reverse==-expected,"exact reverse numeric ordering");
}
void Shapes(){
  static_assert(!std::is_convertible_v<m::MetricScalar,double>);
  static_assert(sizeof(m::MetricFloat128)==16&&sizeof(m::MetricDecimal128)==16);
  Check(!m::MetricScalarValid({}),"unset scalar is not zero");
  Check(m::MetricScalarValid(U(-1))&&m::MetricScalarValid(std::numeric_limits<I>::min()),"full integer ranges");
  for(double value:{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})Check(!m::MetricScalarValid(value),"nonfinite double admission");
  for(double value:{0.,-0.,std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max()})Check(m::MetricScalarValid(value),"finite double rejection");
  for(const auto& text:{std::string(),std::string("a\0b",3),std::string("\xc2\x80"),std::string("\xf4\x8f\xbf\xbf")})Check(m::MetricScalarValid(text),"valid UTF8 data rejection");
  for(const auto& text:{std::string("\xc0\x80"),std::string("\x80"),std::string("\xc2"),std::string("\xe0\x80\x80"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),std::string("\xf5\x80\x80\x80"),std::string("\xe2\x28\xa1")})Check(!m::MetricScalarValid(text),"malformed UTF8 admission");
  for(unsigned version=0;version<16;++version)for(unsigned variant:{0u,0x40u,0x80u,0xc0u}){
    auto id=Id(1,version);id.bytes[8]=variant;
    Check(m::MetricScalarValid(id)==(version>=1&&version<=7&&variant==0x80),"UUID data version/variant policy");
  }
  Check(!m::MetricScalarValid(m::MetricUuid{}),"nil UUID scalar accepted");
  for(unsigned exponent=0;exponent<32768;++exponent){m::MetricFloat128 f;f.bytes[14]=exponent&255;f.bytes[15]=(exponent>>8)|0x80;Check(m::MetricScalarValid(f)==(exponent!=32767),"binary128 exponent validation");}
  auto nan=Binary("1");nan.bytes[14]=0xff;nan.bytes[15]=0x7f;nan.bytes[0]=1;Check(!m::MetricScalarValid(nan),"binary128 NaN admission");
  for(unsigned exponent=0;exponent<16384;++exponent){const auto d=Decimal("1",int(exponent)-6176);Check(m::MetricScalarValid(d)==(exponent<=12287),"decimal exponent validation");}
  Check(m::MetricScalarValid(Decimal("9999999999999999999999999999999999",6111)),"max decimal128 coefficient and exponent");
  Check(!m::MetricScalarValid(Decimal("10000000000000000000000000000000000")),"noncanonical coefficient coerced to zero");
  auto large=Decimal("0");for(unsigned bit=0;bit<113;++bit)large.bytes[bit/8]|=1u<<(bit%8);Check(!m::MetricScalarValid(large),"113-bit maximum coefficient accepted");
}
void Ordering(){
  Order(U(-2),U(-1),-1);Order(U(9007199254740992),U(9007199254740993),-1);
  Order(std::numeric_limits<I>::min(),std::numeric_limits<I>::max(),-1);
  Order(I(-9007199254740993LL),I(-9007199254740992LL),-1);
  Order(-0.,0.,0);Order(std::numeric_limits<double>::denorm_min(),0.,1);
  Order(Binary("1"),Binary("1.0000000000000000000000000000000002"),-1);
  Order(Binary("1e4000"),Binary("2e4000"),-1);Order(Binary("-1e4000"),Binary("-2e4000"),1);
  Order(Binary("-0"),Binary("0"),0);Order(Binary("-0"),Binary("1e-4900"),-1);
  Order(Decimal("1",-6176),Decimal("1",6111),-1);
  Order(Decimal("0",6111,true),Decimal("0",-6176),0);
  Order(Decimal("1",0,true),Decimal("0",-6176,true),-1);
  Order(Decimal("100",-2),Decimal("1"),0);
  Order(Decimal("9999999999999999999999999999999998"),Decimal("9999999999999999999999999999999999"),-1);
  Order(Decimal("1",33),Decimal("999999999999999999999999999999999"),1);
  for(unsigned shift=1;shift<=33;++shift){
    const std::string power="1"+std::string(shift,'0');
    Order(Decimal("1",int(shift)),Decimal(power),0);
    Order(Decimal("1",int(shift),true),Decimal(power,0,true),0);
    Order(Decimal("1",int(shift)),Decimal(std::string(shift,'9')),1);
  }
  for(unsigned bit=0;bit<112;++bit){auto larger=Binary("1");larger.bytes[bit/8]|=1u<<(bit%8);Order(Binary("1"),larger,-1);}
  // Independent unbounded integer oracle at a common exponent, not adjusted
  // exponent/digit-count comparison as used by production.
  U rng=0x83489721;
  for(unsigned n=0;n<1000;++n){
    rng=rng*6364136223846793005ULL+1;const U ac=rng;const int ae=int((rng>>32)%41)-20;const bool an=rng>>63;
    rng=rng*6364136223846793005ULL+1;const U bc=rng;const int be=int((rng>>32)%41)-20;const bool bn=rng>>63;
    Big av=ac,bv=bc;for(int i=-20;i<ae;++i)av*=10;for(int i=-20;i<be;++i)bv*=10;if(an)av=-av;if(bn)bv=-bv;
    Order(Decimal(std::to_string(ac),ae,an),Decimal(std::to_string(bc),be,bn),av<bv?-1:av>bv?1:0);
  }
  Check(!m::CompareMetricScalars(U(1),1.),"cross-type implicit numeric conversion");
  Check(!m::CompareMetricScalars(true,false)&&!m::CompareMetricScalars(std::string("a"),std::string("b")),"non-numeric scalar ordering");
}
void DescriptorAdmission(){
  auto d=Descriptor(T::uint64);d.min_value=U(9007199254740993ULL);d.max_value=U(-1);
  Check(m::ValidateMetricObservationScalar(d,U(9007199254740992ULL))==E::out_of_range,"large integer lower bound narrowed");
  Check(m::ValidateMetricObservationScalar(d,U(9007199254740993ULL))==E::none,"inclusive lower bound");
  Check(m::ValidateMetricObservationScalar(d,U(-1))==E::none,"inclusive uint64 maximum");
  Check(m::ValidateMetricObservationScalar(d,9007199254740993.)==E::type_mismatch,"double inferred as exact integer");
  d.max_value=U(0);Check(m::ValidateMetricScalarDescriptor(d)==E::invalid_descriptor,"inverted descriptor bounds");
  d=Descriptor(T::float64);d.min_value=std::numeric_limits<double>::quiet_NaN();Check(m::ValidateMetricScalarDescriptor(d)==E::invalid_descriptor,"NaN descriptor bound");
  d=Descriptor(T::float128);d.min_value=Binary("1.0000000000000000000000000000000002");
  Check(m::ValidateMetricObservationScalar(d,Binary("1"))==E::out_of_range,"binary128 lower bound narrowed");
  d=Descriptor(T::decimal128);d.min_value=Decimal("9999999999999999999999999999999999");
  Check(m::ValidateMetricObservationScalar(d,Decimal("9999999999999999999999999999999998"))==E::out_of_range,"decimal128 lower bound narrowed");
  d=Descriptor(T::enumeration);Check(m::ValidateMetricScalarDescriptor(d)==E::invalid_descriptor,"empty enum");
  d.enum_values={0,U(-1)};Check(m::ValidateMetricObservationScalar(d,m::MetricEnumValue{U(-1)})==E::none,"full uint64 enum code");
  Check(m::ValidateMetricObservationScalar(d,m::MetricEnumValue{1})==E::out_of_range,"unknown enum member");
  d.enum_values.push_back(0);Check(m::ValidateMetricScalarDescriptor(d)==E::invalid_descriptor,"duplicate enum member");
  for(auto type:{T::boolean,T::text,T::uuid}){d=Descriptor(type);d.min_value=U(0);Check(m::ValidateMetricScalarDescriptor(d)==E::invalid_descriptor,"non-numeric descriptor bound");}
  d=Descriptor(T::uint64);d.enum_values={1};Check(m::ValidateMetricScalarDescriptor(d)==E::invalid_descriptor,"non-enum member list");
  d=Descriptor(static_cast<T>(999));Check(m::ValidateMetricScalarDescriptor(d)==E::invalid_descriptor,"unknown descriptor type");
  const std::vector<std::array<m::MetricScalar,4>> percentages={
    {U(0),U(100),U(101),U(-1)}, {I(0),I(100),I(101),I(-1)}, {0.,100.,101.,-1.},
    {Binary("-0"),Binary("100"),Binary("100.00000000000000000000000000000002"),Binary("-1e-4000")},
    {Decimal("0",-6176,true),Decimal("100"),Decimal("1000000000000000000000000000000001",-31),Decimal("1",-6176,true)}};
  for(const auto& values:percentages){d=Descriptor(m::MetricScalarTypeOf(values[0]));d.unit=m::MetricUnit::percent;
    for(unsigned i=0;i<4;++i)Check(m::ValidateMetricObservationScalar(d,values[i])==(i<2?E::none:E::out_of_range),"exact percent bounds");}
}
void ActualHistory(){
  m::MetricHistoryBinding binding;binding.metric_uuid=Id(1);binding.descriptor_generation=1;
  binding.retention_policy_uuid=Id(2);binding.retention_policy_generation=1;
  binding.visibility_policy_uuid=Id(3);binding.visibility_policy_generation=1;binding.database_uuid=Id(4);binding.node_uuid=Id(5);
  m::MetricRetentionPolicy policy;policy.policy_uuid=Id(2);policy.generation=1;policy.policy_name="exact-policy";
  const std::vector<m::MetricScalar> values={U(-1),std::numeric_limits<I>::min(),-0.,Binary("1e4000"),
    Decimal("9999999999999999999999999999999999",6111),true,std::string("a\0b",3),Id(6,1),m::MetricEnumValue{U(-1)}};
  for(const auto& scalar:values){auto d=Descriptor(m::MetricScalarTypeOf(scalar));if(d.value_type==T::enumeration)d.enum_values={U(-1)};
    static_cast<m::MetricDescriptorBinding&>(d)=binding;
    const auto series=m::MakeMetricSeriesIdentity(d,{},policy,binding,Id(7),1);Check(series.ok(),"exact scalar series construction");if(!series.ok())continue;
    m::MetricValue value;value.family=d.family;value.type=d.type;value.value=scalar;
    const auto result=m::MakeMetricRawSampleRecord(d,*series.record,value,1,2,1);
    Check(result.ok()&&result.record->value.value==scalar,"actual sample lost exact typed scalar");
    value.value=std::monostate{};const auto absent=m::MakeMetricRawSampleRecord(d,*series.record,value,1,2,1);
    Check(!absent.ok()&&!absent.record,"missing scalar created sample identity");
    for(const auto& wrong:values)if(m::MetricScalarTypeOf(wrong)!=d.value_type){value.value=wrong;const auto rejected=m::MakeMetricRawSampleRecord(d,*series.record,value,1,2,1);Check(!rejected.ok()&&!rejected.record,"wrong scalar alternative created sample");}
    if(const auto* number=std::get_if<double>(&scalar))Check(std::signbit(std::get<double>(result.record->value.value))==std::signbit(*number),"signed zero sample narrowed");
  }
}
}  // namespace
int main(){Shapes();Ordering();DescriptorAdmission();ActualHistory();std::cout<<"metric exact scalar checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
