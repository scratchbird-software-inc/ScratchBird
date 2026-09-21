// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_scalar.hpp"
#include "metric_registry.hpp"
#include "sbl_numeric.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <cmath>
#include <limits>
#include <cstring>
#include <new>

namespace scratchbird::core::metrics {
namespace {
static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits==53, "Metric float64 requires binary64");
using U128 = boost::multiprecision::uint128_t;
using Big = boost::multiprecision::cpp_int;
using E = MetricScalarError;
bool Numeric(MetricScalarType t) noexcept {
  return t >= MetricScalarType::uint64 && t <= MetricScalarType::decimal128;
}
template<class T> int Compare(const T& a, const T& b) noexcept { return a < b ? -1 : a > b ? 1 : 0; }
bool Utf8(const std::string& text) noexcept {
  for (std::size_t i=0; i<text.size();) {
    const auto first=static_cast<unsigned char>(text[i++]);
    if (first<0x80) continue;
    unsigned count=0, code=0, minimum=0;
    if (first>=0xc2 && first<=0xdf) {count=1;code=first&0x1f;minimum=0x80;}
    else if (first>=0xe0 && first<=0xef) {count=2;code=first&0x0f;minimum=0x800;}
    else if (first>=0xf0 && first<=0xf4) {count=3;code=first&7;minimum=0x10000;}
    else return false;
    if (text.size()-i<count) return false;
    for (unsigned n=0;n<count;++n) {
      const auto c=static_cast<unsigned char>(text[i++]);
      if ((c&0xc0)!=0x80) return false;
      code=(code<<6)|(c&0x3f);
    }
    if (code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return false;
  }
  return true;
}
std::uint64_t High(const std::array<std::uint8_t,16>& bytes) noexcept {
  std::uint64_t high=0;
  for (unsigned i=0;i<8;++i) high|=std::uint64_t(bytes[i+8])<<(i*8);
  return high;
}
U128 Coefficient(const MetricDecimal128& value) noexcept {
  U128 n=High(value.bytes)&UINT64_C(0x0001ffffffffffff);
  for (int i=7;i>=0;--i) n=(n<<8)|value.bytes[i];
  return n;
}
unsigned Exponent(const MetricDecimal128& value) noexcept { return unsigned((High(value.bytes)>>49)&0x3fff); }
U128 DecimalLimit() noexcept { U128 n=1;for(unsigned i=0;i<34;++i)n*=10;return n; }
int Binary128Compare(const MetricFloat128& a, const MetricFloat128& b) noexcept {
  bool az=true,bz=true;
  for(unsigned i=0;i<16;++i){az=az&&((a.bytes[i]&(i==15?0x7f:0xff))==0);bz=bz&&((b.bytes[i]&(i==15?0x7f:0xff))==0);}
  const bool an=!az&&(a.bytes[15]&0x80),bn=!bz&&(b.bytes[15]&0x80);
  if(an!=bn)return an?-1:1;
  for(int i=15;i>=0;--i){const auto mask=i==15?0x7f:0xff;const int c=Compare(a.bytes[i]&mask,b.bytes[i]&mask);if(c)return an?-c:c;}
  return 0;
}
unsigned Digits(U128 n) noexcept {unsigned digits=1;while(n>=10){n/=10;++digits;}return digits;}
int Decimal128Compare(const MetricDecimal128& a, const MetricDecimal128& b) noexcept {
  auto ac=Coefficient(a),bc=Coefficient(b);
  const bool an=ac!=0&&(a.bytes[15]&0x80),bn=bc!=0&&(b.bytes[15]&0x80);
  if(an!=bn)return an?-1:1;
  if(ac==0||bc==0)return Compare(ac,bc);
  const auto ae=Exponent(a),be=Exponent(b);
  int c=Compare(ae+Digits(ac),be+Digits(bc));
  if(!c){
    // Equal adjusted exponents bound alignment to33 decimal places and the
    // scaled coefficient to34 digits; fixed128 arithmetic cannot overflow.
    for(auto i=ae;i<be;++i)bc*=10;
    for(auto i=be;i<ae;++i)ac*=10;
    c=Compare(ac,bc);
  }
  return an?-c:c;
}
MetricScalar IntegerConstant(MetricScalarType type, unsigned n) {
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
Big Power(unsigned radix,unsigned exponent) {
  Big value=1,base=radix;
  while(exponent){if(exponent&1)value*=base;exponent>>=1;if(exponent)base*=base;}
  return value;
}
void RoundEven(Big& coefficient,const Big& divisor,bool& inexact) {
  Big quotient=coefficient/divisor;
  const Big remainder=coefficient%divisor;
  inexact=remainder!=0;
  const Big twice=remainder*2;
  if(twice>divisor||(twice==divisor&&bool(quotient&1)))++quotient;
  coefficient=std::move(quotient);
}
MetricScalarResult AddBinary64(double a,double b) {
  std::uint64_t ab=0,bb=0;std::memcpy(&ab,&a,8);std::memcpy(&bb,&b,8);
  auto coefficient=[](std::uint64_t bits){return Big((bits&UINT64_C(0x000fffffffffffff))|
      (((bits>>52)&0x7ff)?UINT64_C(0x0010000000000000):0));};
  auto exponent=[](std::uint64_t bits){const int e=int((bits>>52)&0x7ff);return e?e-1075:-1074;};
  const auto ae=exponent(ab),be=exponent(bb);int exponent_value=std::min(ae,be);
  Big ac=coefficient(ab)<<(ae-exponent_value),bc=coefficient(bb)<<(be-exponent_value);
  if(ab>>63)ac=-ac;
  if(bb>>63)bc=-bc;
  Big sum=ac+bc;const bool negative=sum<0||(sum==0&&(ab>>63)&&(bb>>63));
  if(sum<0)sum=-sum;
  bool inexact=false;std::uint64_t bits=0;
  if(sum!=0){
    unsigned width=boost::multiprecision::msb(sum)+1;
    if(width>53){const unsigned drop=width-53;RoundEven(sum,Big(1)<<drop,inexact);exponent_value+=int(drop);
      if(boost::multiprecision::msb(sum)+1>53){sum>>=1;++exponent_value;}}
    width=boost::multiprecision::msb(sum)+1;
    const unsigned extend=std::min(unsigned(53-width),unsigned(exponent_value+1074));
    sum<<=extend;exponent_value-=int(extend);
    if(exponent_value>971)return {E::overflow,{},false};
    const auto c=sum.convert_to<std::uint64_t>();
    bits=c&UINT64_C(0x000fffffffffffff);
    if(c>=UINT64_C(0x0010000000000000))bits|=std::uint64_t(exponent_value+1075)<<52;
  }
  if(negative)bits|=UINT64_C(0x8000000000000000);
  double out=0;std::memcpy(&out,&bits,8);return {E::none,MetricScalar(out),inexact};
}
MetricScalarResult AddDecimal128(const MetricDecimal128& a,const MetricDecimal128& b) {
  const auto ae=Exponent(a),be=Exponent(b);unsigned exponent=std::min(ae,be);
  Big ac=Big(Coefficient(a))*Power(10,ae-exponent),bc=Big(Coefficient(b))*Power(10,be-exponent);
  if(a.bytes[15]&0x80)ac=-ac;
  if(b.bytes[15]&0x80)bc=-bc;
  Big sum=ac+bc;
  const bool negative=sum<0||(sum==0&&(a.bytes[15]&0x80)&&(b.bytes[15]&0x80));
  if(sum<0)sum=-sum;
  bool inexact=false;
  unsigned digits=1;for(Big remaining=sum;remaining>=10;remaining/=10)++digits;
  if(digits>34){const unsigned drop=digits-34;RoundEven(sum,Power(10,drop),inexact);exponent+=drop;
    if(sum>=Big(DecimalLimit())){sum/=10;++exponent;}}
  if(exponent>12287)return {E::overflow,{},false};
  U128 coefficient=sum.convert_to<U128>();MetricDecimal128 out;
  for(unsigned i=0;i<15;++i){out.bytes[i]=static_cast<std::uint8_t>(coefficient&255);coefficient>>=8;}
  for(unsigned i=0;i<14;++i)if(exponent&(1u<<i))out.bytes[(113+i)/8]|=1u<<((113+i)%8);
  if(negative)out.bytes[15]|=0x80;
  return {E::none,MetricScalar(out),inexact};
}
}  // namespace

std::optional<MetricScalar> MetricScalarZero(MetricScalarType type) {
  if(!Numeric(type))return {};
  return IntegerConstant(type,0);
}
MetricScalarResult AddMetricScalars(const MetricScalar& a,const MetricScalar& b) {
  if(MetricScalarTypeOf(a)!=MetricScalarTypeOf(b))return {E::type_mismatch,{},false};
  if(!MetricScalarValid(a)||!MetricScalarValid(b))return {E::invalid_value,{},false};
  try {
    switch(MetricScalarTypeOf(a)){
      case MetricScalarType::uint64:{const auto av=std::get<std::uint64_t>(a),bv=std::get<std::uint64_t>(b);
        if(bv>std::numeric_limits<std::uint64_t>::max()-av)return {E::overflow,{},false};
        return {E::none,MetricScalar(av+bv),false};}
      case MetricScalarType::int64:{const auto av=std::get<std::int64_t>(a),bv=std::get<std::int64_t>(b);
        if((bv>0&&av>std::numeric_limits<std::int64_t>::max()-bv)||
           (bv<0&&av<std::numeric_limits<std::int64_t>::min()-bv))return {E::overflow,{},false};
        return {E::none,MetricScalar(av+bv),false};}
      case MetricScalarType::float64:return AddBinary64(std::get<double>(a),std::get<double>(b));
      case MetricScalarType::decimal128:return AddDecimal128(std::get<MetricDecimal128>(a),std::get<MetricDecimal128>(b));
      case MetricScalarType::float128:{
        namespace n=scratchbird::libraries::sbl_numeric;
        n::Real128BinaryRequest request;request.operation=n::NumericOperation::add;
        request.left=std::get<MetricFloat128>(a).bytes;request.right=std::get<MetricFloat128>(b).bytes;
        request.context.rounding=n::RoundingMode::half_even;
        const auto result=n::ApplyReal128BinaryOperation(request);
        if(result.numeric.status==n::NumericStatusCode::overflow||result.numeric.overflow)return {E::overflow,{},false};
        if(result.numeric.status!=n::NumericStatusCode::ok||!result.bytes)return {E::arithmetic_failure,{},false};
        return {E::none,MetricScalar(MetricFloat128{*result.bytes}),result.numeric.inexact};}
      default:return {E::type_mismatch,{},false};
    }
  }catch(const std::bad_alloc&){return {E::allocation_failure,{},false};}
}

MetricScalarType MetricScalarTypeOf(const MetricScalar& value) noexcept {
  if(value.valueless_by_exception())return MetricScalarType::invalid;
  switch(value.index()){
    case 1:return MetricScalarType::uint64;case 2:return MetricScalarType::int64;
    case 3:return MetricScalarType::float64;case 4:return MetricScalarType::float128;
    case 5:return MetricScalarType::decimal128;case 6:return MetricScalarType::boolean;
    case 7:return MetricScalarType::text;case 8:return MetricScalarType::uuid;
    case 9:return MetricScalarType::enumeration;default:return MetricScalarType::invalid;
  }
}
bool MetricScalarValid(const MetricScalar& value) noexcept {
  switch(MetricScalarTypeOf(value)){
    case MetricScalarType::uint64:case MetricScalarType::int64:case MetricScalarType::boolean:
    case MetricScalarType::enumeration:return true;
    case MetricScalarType::float64:return std::isfinite(std::get<double>(value));
    case MetricScalarType::float128:{const auto& bytes=std::get<MetricFloat128>(value).bytes;return ((High(bytes)>>48)&0x7fff)!=0x7fff;}
    case MetricScalarType::decimal128:{const auto& v=std::get<MetricDecimal128>(value);return Exponent(v)<=12287&&Coefficient(v)<DecimalLimit();}
    case MetricScalarType::text:return Utf8(std::get<std::string>(value));
    case MetricScalarType::uuid:{const auto& v=std::get<MetricUuid>(value);return (v.bytes[8]&0xc0)==0x80&&(v.bytes[6]>>4)>=1&&(v.bytes[6]>>4)<=7;}
    default:return false;
  }
}
std::optional<int> CompareMetricScalars(const MetricScalar& a,const MetricScalar& b) noexcept {
  if(MetricScalarTypeOf(a)!=MetricScalarTypeOf(b)||!MetricScalarValid(a)||!MetricScalarValid(b))return {};
  switch(MetricScalarTypeOf(a)){
    case MetricScalarType::uint64:return Compare(std::get<std::uint64_t>(a),std::get<std::uint64_t>(b));
    case MetricScalarType::int64:return Compare(std::get<std::int64_t>(a),std::get<std::int64_t>(b));
    case MetricScalarType::float64:return Compare(std::get<double>(a),std::get<double>(b));
    case MetricScalarType::float128:return Binary128Compare(std::get<MetricFloat128>(a),std::get<MetricFloat128>(b));
    case MetricScalarType::decimal128:return Decimal128Compare(std::get<MetricDecimal128>(a),std::get<MetricDecimal128>(b));
    default:return {};
  }
}
MetricScalarError ValidateMetricScalarDescriptor(const MetricDescriptorDefinition& d) noexcept {
  if(d.value_type<MetricScalarType::uint64||d.value_type>MetricScalarType::enumeration)return E::invalid_descriptor;
  if(d.value_type==MetricScalarType::enumeration){
    if(d.enum_values.empty())return E::invalid_descriptor;
    for(std::size_t i=0;i<d.enum_values.size();++i)for(std::size_t j=0;j<i;++j)if(d.enum_values[i]==d.enum_values[j])return E::invalid_descriptor;
  }else if(!d.enum_values.empty())return E::invalid_descriptor;
  if(!Numeric(d.value_type)&&(d.min_value||d.max_value||d.unit==MetricUnit::percent))return E::invalid_descriptor;
  for(const auto* bound:{&d.min_value,&d.max_value})if(*bound&&
      (MetricScalarTypeOf(**bound)!=d.value_type||!MetricScalarValid(**bound)))return E::invalid_descriptor;
  if(d.min_value&&d.max_value&&*CompareMetricScalars(*d.min_value,*d.max_value)>0)return E::invalid_descriptor;
  return E::none;
}
MetricScalarError ValidateMetricObservationScalar(const MetricDescriptorDefinition& d,const MetricScalar& value) noexcept {
  const auto definition=ValidateMetricScalarDescriptor(d);if(definition!=E::none)return definition;
  if(MetricScalarTypeOf(value)!=d.value_type)return E::type_mismatch;
  if(!MetricScalarValid(value))return E::invalid_value;
  if(d.value_type==MetricScalarType::enumeration){
    const auto code=std::get<MetricEnumValue>(value).code;
    for(auto member:d.enum_values)if(member==code)return E::none;
    return E::out_of_range;
  }
  if(d.min_value&&*CompareMetricScalars(value,*d.min_value)<0)return E::out_of_range;
  if(d.max_value&&*CompareMetricScalars(value,*d.max_value)>0)return E::out_of_range;
  if(d.unit==MetricUnit::percent&&(*CompareMetricScalars(value,IntegerConstant(d.value_type,0))<0||
      *CompareMetricScalars(value,IntegerConstant(d.value_type,100))>0))return E::out_of_range;
  return E::none;
}
}  // namespace scratchbird::core::metrics
