// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "descriptor_value_runtime.hpp"
#include "sbl_numeric.hpp"
#include <cfenv>
#include <iostream>
#include <stdexcept>

namespace ex=scratchbird::engine::executor;
namespace api=scratchbird::engine::internal_api;
namespace n=scratchbird::libraries::sbl_numeric;
unsigned checks=0;
void Check(bool condition,const char* detail) {
  ++checks;if(!condition) throw std::runtime_error(detail);
}
api::EngineTypedValue Value(std::uint64_t bits) {
  auto value=ex::EncodeReal64Value(0);
  for(unsigned i=0;i<8;++i) value.binary_value[i]=bits>>(8*i);
  return value;
}
bool Bits(const api::EngineTypedValue& value,std::uint64_t expected) {
  if(value.state!=api::EngineValueState::value || value.is_null ||
     !value.encoded_value.empty() || value.binary_value.size()!=8) return false;
  for(unsigned i=0;i<8;++i) if(value.binary_value[i]!=static_cast<std::uint8_t>(expected>>(8*i))) return false;
  return true;
}
int main() try {
  using Op=ex::DescriptorExpressionOperator;
  const auto one=ex::EncodeReal64Value(1), half_ulp=Value(0x3ca0000000000000ULL);
  Check(Bits(one,0x3ff0000000000000ULL) && ex::DecodeReal64Value(one).ok(),"native executor encoder and decoder");
  ex::DescriptorRuntimeDiagnostic diagnostic;
  const auto prior=std::fegetround();
  std::fesetround(FE_UPWARD);
  auto sum=ex::EvaluateDescriptorExpression(Op::kReal64Add,one,half_ulp,&diagnostic);
  if(!diagnostic.ok || !diagnostic.numeric_facts.inexact || !Bits(sum,0x3ff0000000000000ULL))
    std::cerr<<"arithmetic "<<diagnostic.diagnostic_code<<":"<<diagnostic.detail
             <<" bytes="<<sum.binary_value.size()<<" inexact="<<diagnostic.numeric_facts.inexact<<'\n';
  Check(diagnostic.ok && diagnostic.numeric_facts.inexact && Bits(sum,0x3ff0000000000000ULL),
        "executor arithmetic follows half-even independently of host rounding");
  std::fesetround(prior);
  auto divided=ex::EvaluateDescriptorExpression(Op::kReal64Divide,one,Value(0),&diagnostic);
  Check(!diagnostic.ok && diagnostic.numeric_facts.divide_by_zero && divided.binary_value.empty() &&
        divided.state==api::EngineValueState::error &&
        diagnostic.diagnostic_code=="NUMERIC.REAL64.DIVIDE_BY_ZERO","executor division preserves facts and diagnostic");
  auto tiny=ex::EvaluateDescriptorExpression(Op::kReal64Divide,Value(1),ex::EncodeReal64Value(2),&diagnostic);
  Check(diagnostic.ok && diagnostic.numeric_facts.inexact && diagnostic.numeric_facts.underflow && Bits(tiny,0),
        "executor gradual underflow facts");
  auto integer=ex::EncodeInt64Value(9007199254740993LL);
  auto cast=ex::CastDescriptorValue(integer,one.descriptor,&diagnostic);
  Check(diagnostic.ok && diagnostic.numeric_facts.inexact && Bits(cast,0x4340000000000000ULL),
        "executor native integer cast reports rounding");
  cast=ex::CastDescriptorValue(ex::EncodeReal64Value(1.5),integer.descriptor,&diagnostic);
  Check(!diagnostic.ok && diagnostic.numeric_facts.invalid && diagnostic.numeric_facts.inexact &&
        cast.binary_value.empty() && cast.state==api::EngineValueState::error,
        "executor fractional integral cast refuses without truncation or success sentinel");
  for(unsigned variant=0;variant<5;++variant) {
    auto invalid=one;
    if(variant==0) invalid.encoded_value="1";
    if(variant==1) invalid.binary_value.pop_back();
    if(variant==2) invalid.binary_value.push_back(0);
    if(variant==3) invalid.descriptor.datatype_descriptor_generation++;
    if(variant==4) invalid.descriptor.type_uuid={};
    Check(!ex::DecodeReal64Value(invalid).ok(),"executor rejects malformed native value");
    const auto failed=ex::EvaluateDescriptorExpression(Op::kReal64Add,invalid,one,&diagnostic);
    Check(!diagnostic.ok && failed.binary_value.empty() && failed.state==api::EngineValueState::error,
          "malformed operand does not publish arithmetic output");
  }
  ex::DescriptorBatch left,right;
  left.columns={{"value",one.descriptor,false}};right.columns=left.columns;
  left.rows={{{Value(0)}}};right.rows={{{Value(0x8000000000000000ULL)}}};
  const auto merged=ex::SetUnionDistinctDescriptorBatch(left,right,&diagnostic);
  Check(diagnostic.ok && merged.rows.size()==1,"distinct native keys coalesce signed zeros");
  ex::CanonicalDescriptorOrderTerm term;term.expression_descriptor_id=1;
  auto order=ex::CompareCanonicalDescriptorOrderValues(Value(0x8000000000000001ULL),Value(0),term);
  Check(order.diagnostic.ok && order.comparison<0,"physical native REAL64 order");
  auto positive_key=ex::MakeCanonicalDescriptorEqualityKey(Value(0),term);
  auto negative_key=ex::MakeCanonicalDescriptorEqualityKey(Value(0x8000000000000000ULL),term);
  Check(positive_key.diagnostic.ok && negative_key.diagnostic.ok &&
        positive_key.equality_key==negative_key.equality_key,"physical equality key uses native zero normalization");
  const auto plan=ex::PlanCanonicalDescriptorEqualityKey(one,term);
  Check(plan.diagnostic.ok && plan.retained_key_bytes<1024 && plan.peak_workspace_bytes<4096,
        "fixed-width REAL64 key does not reserve decimal exponent workspace");
  term.direction=ex::CanonicalDescriptorOrderDirection::descending;
  order=ex::CompareCanonicalDescriptorOrderValues(Value(0x8000000000000001ULL),Value(0),term);
  Check(order.diagnostic.ok && order.comparison>0,"physical descending order");
  auto malformed=one;malformed.encoded_value="1";
  Check(!ex::MakeCanonicalDescriptorEqualityKey(malformed,term).diagnostic.ok,"physical key rejects mixed carriers");
  n::ReleaseReal128ThreadCache();
  std::cout<<"REAL64 executor checks="<<checks<<" failures=0\n";
  return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
