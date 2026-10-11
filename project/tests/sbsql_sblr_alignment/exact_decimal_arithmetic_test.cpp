// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"
#include "crud_support/bound_ordered_index_key.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "query/expression_api.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace { bool forbid_allocation = false; long allocation_budget = -1; unsigned checks = 0; }
void* operator new(std::size_t n) {
  if (forbid_allocation) throw std::bad_alloc();
  if (allocation_budget == 0) { allocation_budget = -1; throw std::bad_alloc(); }
  if (allocation_budget > 0) --allocation_budget;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace num = scratchbird::libraries::sbl_numeric;
namespace dt = scratchbird::core::datatypes;
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
using boost::multiprecision::cpp_int;
void Check(bool value, const char* message) {
  ++checks; if (!value) throw std::runtime_error(message);
}
num::ExactDecimalProfile Profile(unsigned p, unsigned s) {
  return {p <= 38 ? num::ExactDecimalCodec::le24_v1 : num::ExactDecimalCodec::le40_v1,p,s};
}
cpp_int Ten(unsigned n) { cpp_int x = 1; while (n--) x *= 10; return x; }
std::string Fixed(cpp_int n, unsigned s) {
  const bool negative = n < 0; if (negative) n = -n;
  auto digits = n.convert_to<std::string>();
  if (s) {
    if (digits.size() <= s) digits.insert(0,s-digits.size()+1,'0');
    digits.insert(digits.size()-s,1,'.');
  }
  return (negative ? "-" : "") + digits;
}
num::ExactDecimalArithmeticResult Apply(num::NumericOperation op,
    const std::vector<std::uint8_t>& l, num::ExactDecimalProfile lp,
    const std::vector<std::uint8_t>& r, num::ExactDecimalProfile rp,
    num::ExactDecimalProfile out, num::RoundingMode mode) {
  forbid_allocation = true;
  const auto result = num::ApplyExactDecimalArithmetic(op,
      {l.data(),l.size(),lp},{r.data(),r.size(),rp},out,mode);
  forbid_allocation = false;
  return result;
}
void OracleCase(num::NumericOperation op, cpp_int l, unsigned ls, cpp_int r, unsigned rs,
                unsigned lp, unsigned rp, unsigned p, unsigned s, num::RoundingMode mode) {
  const auto a = num::EncodeBoundExactDecimal(Fixed(l,ls),Profile(lp,ls));
  const auto b = num::EncodeBoundExactDecimal(Fixed(r,rs),Profile(rp,rs));
  Check(a.ok() && b.ok(),"oracle input encode");
  const auto result = Apply(op,a.bytes,Profile(lp,ls),b.bytes,Profile(rp,rs),Profile(p,s),mode);
  if(op==num::NumericOperation::compare) {
    const cpp_int al=l*Ten(rs), br=r*Ten(ls);
    const int expected=al<br ? -1 : al>br ? 1 : 0;
    Check(result.status==num::NumericStatusCode::ok && result.comparison==expected &&
          result.size==0 && !result.inexact && result.bytes==std::array<std::uint8_t,40>{},
          "exact comparison rounded operands or published numeric bytes");
    return;
  }
  cpp_int numerator, denominator = Ten(ls+rs);
  if (op == num::NumericOperation::add) numerator = l*Ten(rs)+r*Ten(ls);
  else if (op == num::NumericOperation::subtract) numerator = l*Ten(rs)-r*Ten(ls);
  else if (op == num::NumericOperation::multiply) numerator = l*r;
  else if (op == num::NumericOperation::divide) { numerator=l*Ten(rs); denominator=r*Ten(ls); }
  else { numerator=l; denominator=Ten(ls); }
  if (denominator == 0) {
    Check(result.status == num::NumericStatusCode::divide_by_zero && result.size == 0 &&
          result.bytes == std::array<std::uint8_t,40>{},"division by zero published bytes"); return;
  }
  numerator *= Ten(s);
  const bool negative = (numerator < 0) != (denominator < 0);
  if (numerator < 0) numerator = -numerator;
  if (denominator < 0) denominator = -denominator;
  cpp_int q = numerator / denominator, rem = numerator % denominator;
  const bool inexact = rem != 0;
  if (mode != num::RoundingMode::truncate &&
      (rem*2 > denominator || (rem*2 == denominator &&
       (mode == num::RoundingMode::half_up || q%2 != 0)))) ++q;
  if (q >= Ten(p)) {
    Check(result.status == num::NumericStatusCode::overflow && result.size == 0 &&
          result.bytes == std::array<std::uint8_t,40>{} && result.inexact == inexact,
          "overflow or rounding carry published bytes"); return;
  }
  if (negative) q = -q;
  const auto expected = num::EncodeBoundExactDecimal(Fixed(q,s),Profile(p,s));
  Check(expected.ok() && result.status == num::NumericStatusCode::ok &&
        result.inexact == inexact && result.size == expected.bytes.size() &&
        std::equal(expected.bytes.begin(),expected.bytes.end(),result.bytes.begin()),
        "native arithmetic differs from independent rational oracle");
  Check(num::ValidateExactDecimal(result.bytes.data(),result.size,Profile(p,s)) ==
        num::ExactDecimalError::none,"result noncanonical");
}
void Matrix() {
  for (unsigned p=1;p<=76;++p) for(unsigned s=0;s<=p;++s)
    for(auto mode : {num::RoundingMode::half_even,num::RoundingMode::half_up,num::RoundingMode::truncate}) {
      for(auto op : {num::NumericOperation::canonicalize,num::NumericOperation::add,
                    num::NumericOperation::subtract,num::NumericOperation::multiply,num::NumericOperation::divide,
                    num::NumericOperation::compare})
        for (int sign : {-1,1}) OracleCase(op,sign*9,s,3,s,p,p,p,s,mode);
      OracleCase(num::NumericOperation::add,Ten(p)-1,s,1,s,p,p,p,s,mode);
      OracleCase(num::NumericOperation::multiply,Ten(p)-1,s,Ten(p)-1,s,p,p,p,s,mode);
    }
  for(auto mode : {num::RoundingMode::half_even,num::RoundingMode::half_up,num::RoundingMode::truncate}) {
    for(int sign : {-1,1}) for(int v : {2499,2500,2501,3499,3500,3501,9999})
      OracleCase(num::NumericOperation::canonicalize,sign*v,4,0,0,5,1,2,1,mode);
    for(int denominator : {0,3,6,7,8,11,1999})
      OracleCase(num::NumericOperation::divide,1,0,denominator,0,38,76,76,75,mode);
    OracleCase(num::NumericOperation::multiply,Ten(76)-1,76,Ten(76)-1,76,76,76,76,76,mode);
    OracleCase(num::NumericOperation::subtract,Ten(38)-1,1,Ten(38)-1,1,38,39,76,38,mode);
    OracleCase(num::NumericOperation::divide,Ten(76)-1,0,1,76,76,76,76,76,mode);
    OracleCase(num::NumericOperation::compare,125,2,1250,3,38,76,1,0,mode);
  }
}
void InvalidBackend() {
  const auto profile=Profile(38,2);
  const auto good=num::EncodeBoundExactDecimal("1",profile).bytes;
  for(unsigned mutation=0;mutation<8;++mutation) {
    auto left=good, right=good; auto lp=profile, rp=profile, output=profile;
    auto operation=num::NumericOperation::add; auto rounding=num::RoundingMode::half_even;
    switch(mutation) {
      case 0: left[3]=1; break;
      case 1: right.clear(); break;
      case 2: lp.precision=0;break;
      case 3: rp.scale=39;break;
      case 4: output.precision=77;break;
      case 5: operation=static_cast<num::NumericOperation>(255);break;
      case 6: rounding=static_cast<num::RoundingMode>(255);break;
      case 7: left.pop_back();break;
    }
    const auto result=Apply(operation,left,lp,right,rp,output,rounding);
    Check(result.status!=num::NumericStatusCode::ok && result.size==0 &&
          result.bytes==std::array<std::uint8_t,40>{},"malformed backend request published bytes");
  }
}
api::EngineDescriptor Descriptor(unsigned p, unsigned s) {
  auto descriptor = exec::MakeExecutorDescriptor("decimal", "nullability=nullable;precision="+
      std::to_string(p)+";scale="+std::to_string(s));
  descriptor.descriptor_kind = "scalar";
  api::CatalogColumnMetadata metadata;
  metadata.text={{"nullability","nullable"},{"precision",std::to_string(p)},
                 {"scale",std::to_string(s)},{"decimal_codec_generation","1"}};
  metadata.identities["decimal_codec_uuid"] = p <= 38
      ? api::EngineUuid{{0x01,0xa1,0x18,0x6b,0xdf,0x04,0x76,0x0a,0xa3,0xd5,0xd0,0x2d,0x2e,0xe3,0x01,0x54}}
      : api::EngineUuid{{0x01,0xa1,0x18,0x6b,0xdf,0x04,0x7b,0x1b,0x9c,0xaf,0x87,0xb3,0xbd,0x29,0x94,0x3e}};
  Check(api::EncodeCatalogColumnMetadata(metadata,&descriptor.encoded_descriptor),"descriptor encode");
  return descriptor;
}
scratchbird::engine::ExecutionTypeDescriptor Core(const api::EngineDescriptor& descriptor) {
  api::bound_index_key::OrderedIndexColumn binding;
  Check(dt::LookupDatatypeStorageIdentityV3(api::kBootstrapDatatypeCatalogUuid,
      api::kBootstrapDatatypeCatalogGeneration,api::kBootstrapDatatypeRegistryGeneration,
      descriptor.datatype_descriptor_uuid,descriptor.datatype_descriptor_generation,&binding.datatype),"type binding");
  std::string detail;
  Check(api::bound_index_key::BuildOrderedColumnExecutionDescriptor(descriptor,binding.datatype,true,
      &binding.execution_descriptor,&detail),"execution descriptor");
  return binding.execution_descriptor;
}
void CoreAndEngine() {
  for(unsigned p : {38,39,76}) {
    const auto descriptor = Descriptor(p,2);
    const auto execution = Core(descriptor);
    const auto bytes = num::EncodeBoundExactDecimal("1.25",Profile(p,2)).bytes;
    const std::string encoded(bytes.begin(),bytes.end());
    dt::DatatypeNumericOperationRequest core;
    core.operation=dt::DatatypeNumericOperationKind::add;
    core.left={dt::CanonicalTypeId::decimal,encoded,false,execution}; core.right=core.left;
    core.result_descriptor=execution; core.context.precision=p;core.context.scale=2;
    Check(dt::ResolveExactDecimalArithmeticBindingV1(core,&core.decimal_arithmetic),"resolve arithmetic binding");
    const auto result=dt::ApplyNumericOperation(core);
    const auto expected=num::EncodeBoundExactDecimal("2.5",Profile(p,2)).bytes;
    Check(result.ok() && result.value.encoded_value==std::string(expected.begin(),expected.end()) &&
          result.value.descriptor.precision==p,"Core decimal add");
    bool reached_success=false; unsigned failures=0;
    for(long budget=0;budget<200 && !reached_success;++budget) {
      allocation_budget=budget;
      const auto attempted=dt::ApplyNumericOperation(core);
      allocation_budget=-1;
      if(attempted.ok()) {
        reached_success=true;
        Check(attempted.value.encoded_value==result.value.encoded_value,"allocation sweep success bytes");
      } else {
        ++failures;
        Check(attempted.value.encoded_value.empty() &&
              attempted.diagnostic.diagnostic_code=="DATATYPE.RESOURCE_EXHAUSTED" &&
              attempted.status.code==scratchbird::core::platform::StatusCode::memory_allocation_failed,
              "owning allocation failure lost resource status or published bytes");
      }
      Check(core.left.encoded_value==encoded && core.right.encoded_value==encoded,
            "allocation failure changed inputs");
    }
    Check(reached_success && failures>1,"allocation sweep incomplete");
    for(unsigned mutation=0;mutation<12;++mutation) {
      auto bad=core;
      switch(mutation) {
        case 0: ++bad.decimal_arithmetic.generation; break;
        case 1: bad.decimal_arithmetic.policy_uuid.bytes[0]^=1;break;
        case 2: ++bad.decimal_arithmetic.left.generation;break;
        case 3: bad.decimal_arithmetic.right.codec_uuid.bytes[0]^=1;break;
        case 4: ++bad.decimal_arithmetic.result.generation;break;
        case 5: ++bad.left.descriptor.descriptor_epoch;break;
        case 6: bad.left.encoded_value="1.25";break;
        case 7: bad.right.encoded_value[3]=1;break;
        case 8: bad.left.is_null=true;break;
        case 9: bad.left.is_null=true;bad.left.encoded_value.clear();bad.right.encoded_value="bad";break;
        case 10: ++bad.context.precision;break;
        case 11: bad.left.descriptor.domain_uuid=bad.left.descriptor.descriptor_uuid;break;
      }
      const auto refused=dt::ApplyNumericOperation(bad);
      Check(!refused.ok() && refused.value.encoded_value.empty(),"bad arithmetic authority/payload accepted");
    }
    auto null=core;null.left.is_null=true;null.left.encoded_value.clear();
    const auto null_result=dt::ApplyNumericOperation(null);
    Check(null_result.ok() && null_result.value.is_null && null_result.value.encoded_value.empty(),"typed NULL arithmetic");
    auto bad_op=core; bad_op.operation=static_cast<dt::DatatypeNumericOperationKind>(255);
    const auto invalid_operation=dt::ApplyNumericOperation(bad_op);
    Check(!invalid_operation.ok() && invalid_operation.diagnostic.diagnostic_code==
          "SB_DATATYPE_NUMERIC_OPERATION_REJECTED","unsupported operation diagnostic");
    api::EngineApplyNumericOperationRequest request;
    request.context.security_context_present=true;
    request.numeric_operation="add";request.precision=p;request.scale=2;
    request.left_value.descriptor=descriptor;request.left_value.encoded_value=encoded;
    request.right_value=request.left_value;request.descriptors={descriptor};
    auto answer=api::EngineApplyNumericOperation(request);
    if(!answer.ok) for(const auto& d:answer.diagnostics) std::cerr<<d.code<<':'<<d.detail<<'\n';
    Check(answer.ok && answer.value.encoded_value==result.value.encoded_value,"engine decimal add");
    auto context_mismatch=request; ++context_mismatch.precision;
    const auto context_refused=api::EngineApplyNumericOperation(context_mismatch);
    Check(!context_refused.ok && context_refused.value.encoded_value.empty() &&
          !context_refused.diagnostics.empty() && context_refused.diagnostics.front().code==
          "SB_DATATYPE_NUMERIC_OPERATION_REJECTED" &&
          context_refused.diagnostics.front().detail=="decimal_result_context_invalid",
          "engine silently replaced explicit result context");
    auto divide=request; divide.numeric_operation="div";
    const auto zero=num::EncodeBoundExactDecimal("0",Profile(p,2)).bytes;
    divide.right_value.encoded_value.assign(zero.begin(),zero.end());
    const auto zero_refused=api::EngineApplyNumericOperation(divide);
    Check(!zero_refused.ok && zero_refused.value.encoded_value.empty() &&
          zero_refused.numeric_facts.divide_by_zero && !zero_refused.diagnostics.empty() &&
          zero_refused.diagnostics.front().code=="SB_DATATYPE_NUMERIC_OPERATION_REJECTED" &&
          zero_refused.diagnostics.front().detail=="decimal_divide_by_zero",
          "engine division by zero lost diagnostic/facts or published result");
    divide.left_value.encoded_value.clear(); divide.left_value.setState(api::EngineValueState::sql_null);
    const auto null_division=api::EngineApplyNumericOperation(divide);
    Check(null_division.ok && null_division.value.isSqlNull() &&
          null_division.value.encoded_value.empty() && !null_division.numeric_facts.divide_by_zero,
          "engine NULL propagation evaluated division by zero");
    divide.right_value.encoded_value="0";
    const auto corrupt_null=api::EngineApplyNumericOperation(divide);
    Check(!corrupt_null.ok && corrupt_null.value.encoded_value.empty() &&
          !corrupt_null.diagnostics.empty() && corrupt_null.diagnostics.front().code==
          "NUMERIC.ENCODING.NONCANONICAL","engine NULL hid malformed PRESENT decimal");
    auto rounded=request; rounded.numeric_operation="div";
    const auto two=num::EncodeBoundExactDecimal("2",Profile(p,2)).bytes;
    rounded.right_value.encoded_value.assign(two.begin(),two.end());
    for(const auto& mode : {"half_even","half_up","truncate"}) {
      rounded.rounding_mode=mode;
      const auto quantized=api::EngineApplyNumericOperation(rounded);
      const auto wanted=num::EncodeBoundExactDecimal(std::string_view(mode)=="half_up" ? "0.63" : "0.62",
          Profile(p,2)).bytes;
      Check(quantized.ok && quantized.numeric_facts.inexact &&
            quantized.value.encoded_value==std::string(wanted.begin(),wanted.end()),
            "engine rounding mode or inexact fact lost");
    }
    auto overflow=request; overflow.numeric_operation="mul";
    const auto maximum=num::EncodeBoundExactDecimal(Fixed(Ten(p)-1,2),Profile(p,2)).bytes;
    overflow.right_value.encoded_value.assign(maximum.begin(),maximum.end());
    const auto overflow_refused=api::EngineApplyNumericOperation(overflow);
    Check(!overflow_refused.ok && overflow_refused.value.encoded_value.empty() &&
          overflow_refused.numeric_facts.overflow && !overflow_refused.diagnostics.empty() &&
          overflow_refused.diagnostics.front().code=="SB_DATATYPE_NUMERIC_OPERATION_REJECTED" &&
          overflow_refused.diagnostics.front().detail=="decimal_result_overflow",
          "engine overflow lost diagnostic/facts or published result");
    auto alias=request.left_value; std::string detail;
    dt::DatatypeNumericFacts alias_facts; int alias_comparison=99;
    Check(api::QowApplyCanonicalNumericScalarV1(alias,request.right_value,alias.descriptor,
          dt::DatatypeNumericOperationKind::add,core.context,&alias,&detail,&alias_facts,
          &alias_comparison) && alias.encoded_value==result.value.encoded_value &&
          alias_comparison==0,"engine aliased output corrupted decimal input");
    for(unsigned slot=0;slot<3;++slot) for(unsigned mutation=0;mutation<7;++mutation) {
      auto bad=request;
      auto& altered=slot==0 ? bad.left_value.descriptor : slot==1 ? bad.right_value.descriptor : bad.descriptors[0];
      api::CatalogColumnMetadata fields;
      Check(api::DecodeCatalogColumnMetadata(altered.encoded_descriptor,&fields),"mutation descriptor decode");
      switch(mutation) {
        case 0: fields.identities["decimal_codec_uuid"].bytes[0]^=1; break;
        case 1: fields.text["decimal_codec_generation"]="2";break;
        case 2: fields.identities.erase("decimal_codec_uuid");fields.text.erase("decimal_codec_generation");break;
        case 3: fields.text["codec_id"]="foreign";break;
        case 4: fields.text["numeric_override"]="ignored";break;
        case 5: fields.identities["codec_uuid"]=fields.identities["decimal_codec_uuid"];
                fields.identities["codec_uuid"].bytes[0]^=1; fields.text["codec_generation"]="1";break;
        case 6: fields.text["precision"]="038";break;
      }
      Check(api::EncodeCatalogColumnMetadata(fields,&altered.encoded_descriptor),"mutation descriptor encode");
      for(bool is_null : {false,true}) {
        if(is_null) {bad.left_value.encoded_value.clear();bad.left_value.setState(api::EngineValueState::sql_null);}
        const auto refused=api::EngineApplyNumericOperation(bad);
        Check(!refused.ok && refused.value.encoded_value.empty() && !refused.diagnostics.empty() &&
              refused.diagnostics.front().code=="DATATYPE.DESCRIPTOR.INVALID",
              "engine stripped explicit codec or semantic modifier including NULL");
      }
    }
    request.numeric_operation="cmp";
    request.descriptors={exec::MakeExecutorDescriptor("boolean","nullability=nullable")};
    request.descriptors.front().descriptor_kind="scalar";
    answer=api::EngineApplyNumericOperation(request);
    Check(answer.ok && answer.comparison==0 && answer.value.encoded_value=="true","engine decimal comparison");
    request.right_value.encoded_value=std::string(expected.begin(),expected.end());
    answer=api::EngineApplyNumericOperation(request);
    Check(answer.ok && answer.comparison==-1 && answer.value.encoded_value=="false","engine comparison sign");
    for(unsigned mutation=0;mutation<4;++mutation) for(bool is_null : {false,true}) {
      auto bad=request;
      api::CatalogColumnMetadata fields;
      fields.text["nullability"]="nullable";
      if(mutation==0) fields.text["width"]="128";
      if(mutation==1) fields.text["numeric_override"]="ignored";
      if(mutation==2) fields.identities["codec_uuid"]=descriptor.type_uuid;
      if(mutation==3) fields.identities["type_uuid"]=descriptor.type_uuid;
      Check(api::EncodeCatalogColumnMetadata(fields,&bad.descriptors[0].encoded_descriptor),
            "comparison mutation descriptor encode");
      if(is_null) {bad.left_value.encoded_value.clear();bad.left_value.setState(api::EngineValueState::sql_null);}
      const auto refused=api::EngineApplyNumericOperation(bad);
      Check(!refused.ok && refused.value.encoded_value.empty() && !refused.diagnostics.empty() &&
            refused.diagnostics.front().code=="DATATYPE.DESCRIPTOR.INVALID",
            "comparison stripped unhandled Boolean result metadata");
    }
  }
}
void Binary64DecimalOracle(std::uint64_t bits, unsigned p, unsigned s, num::RoundingMode mode) {
  num::Real64Bytes bytes{};
  for (unsigned i=0;i<8;++i) bytes[i]=bits>>(8*i);
  forbid_allocation=true;
  const auto actual=num::Real64ToExactDecimal(bytes,Profile(p,s),mode);
  forbid_allocation=false;
  const auto exponent=(bits>>52)&0x7ff;
  if (exponent==0x7ff) {
    Check(actual.status==num::NumericStatusCode::invalid_left && actual.size==0 &&
          actual.bytes==std::array<std::uint8_t,40>{},"special REAL64 published decimal"); return;
  }
  cpp_int numerator=bits&((std::uint64_t{1}<<52)-1), denominator=1;
  if(exponent) numerator+=(cpp_int(1)<<52);
  const int shift=exponent ? int(exponent)-1075 : -1074;
  if(shift>=0) numerator<<=shift; else denominator<<=-shift;
  numerator*=Ten(s);
  cpp_int q=numerator/denominator, rem=numerator%denominator;
  if(mode!=num::RoundingMode::truncate &&
     (2*rem>denominator || (2*rem==denominator &&
      (mode==num::RoundingMode::half_up || q%2!=0)))) ++q;
  if(q>=Ten(p)) {
    Check(actual.status==num::NumericStatusCode::overflow && actual.size==0 &&
          actual.bytes==std::array<std::uint8_t,40>{} && actual.inexact==(rem!=0),
          "REAL64 decimal overflow effects or facts"); return;
  }
  if(bits>>63) q=-q;
  const auto expected=num::EncodeBoundExactDecimal(Fixed(q,s),Profile(p,s));
  Check(expected.ok() && actual.status==num::NumericStatusCode::ok &&
        actual.inexact==(rem!=0) && actual.size==expected.bytes.size() &&
        std::equal(expected.bytes.begin(),expected.bytes.end(),actual.bytes.begin()),
        "exact binary REAL64 decimal rational oracle");
}
void Binary64DecimalCasts() {
  for(unsigned p=1;p<=76;++p) for(unsigned s=0;s<=p;++s)
    for(auto mode : {num::RoundingMode::half_even,num::RoundingMode::half_up,num::RoundingMode::truncate})
      Binary64DecimalOracle(0x3ff4000000000000ULL,p,s,mode); // 1.25 exact tie at scale1
  for(auto bits : {0ULL,0x8000000000000000ULL,1ULL,0x8000000000000001ULL,
      0x000fffffffffffffULL,0x0010000000000000ULL,0x7fefffffffffffffULL,
      0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000001ULL,
      0x7ff0000000000001ULL,0x3ff3ffffffffffffULL,0x3ff4000000000000ULL,
      0x3ff4000000000001ULL,0xbff4000000000000ULL,0x3ff599999999999aULL,
      0x3fb999999999999aULL,0x4023e66666666666ULL})
    for(unsigned p : {1,2,38,39,76}) for(unsigned s : {0u,1u,p})
      for(auto mode : {num::RoundingMode::half_even,num::RoundingMode::half_up,num::RoundingMode::truncate})
        Binary64DecimalOracle(bits,p,s,mode);
  // Every IEEE exponent, both signs, changing fraction patterns.
  for(unsigned e=0;e<2048;++e)
    Binary64DecimalOracle((std::uint64_t(e&1)<<63)|(std::uint64_t(e)<<52)|
        ((std::uint64_t(e)*0x5deece66dULL)&((1ULL<<52)-1)),76,e%77,num::RoundingMode::half_even);
  for(unsigned mutation=0;mutation<3;++mutation) {
    auto profile=Profile(38,2); auto rounding=num::RoundingMode::half_even;
    if(mutation==0) profile.precision=0;
    if(mutation==1) profile.scale=39;
    if(mutation==2) rounding=static_cast<num::RoundingMode>(99);
    const auto bad=num::Real64ToExactDecimal({},profile,rounding);
    Check(bad.status==num::NumericStatusCode::invalid_context && bad.size==0 &&
          bad.bytes==std::array<std::uint8_t,40>{},"invalid REAL64 decimal context");
  }
  dt::DatatypeCastRequest cast;
  cast.value.type_id=dt::CanonicalTypeId::real64;
  auto real_descriptor=exec::MakeExecutorDescriptor("real64","nullability=nullable");
  real_descriptor.descriptor_kind="scalar";
  cast.value.descriptor=Core(real_descriptor);
  cast.value.encoded_value=std::string("\0\0\0\0\0\0\xf4\x3f",8);
  cast.target_type_id=dt::CanonicalTypeId::decimal;
  cast.target_descriptor=Core(Descriptor(39,1));
  dt::DatatypeSortKeyRequest codec;
  Check(dt::BindExactDecimalSortKeyProfile(cast.target_descriptor,&codec),"cast target codec binding");
  cast.decimal_target_codec={codec.decimal_codec_uuid,codec.decimal_codec_generation};
  cast.numeric_context.precision=39; cast.numeric_context.scale=1;
  cast.context=dt::DatatypeCastContext::explicit_cast;
  auto converted=dt::CastDatatypeValue(cast);
  const auto expected=num::EncodeBoundExactDecimal("1.2",Profile(39,1));
  Check(converted.ok() && converted.numeric_facts.inexact &&
        converted.value.encoded_value==std::string(expected.bytes.begin(),expected.bytes.end()),
        "bound REAL64 to native decimal owner cast");
  bool reached_success=false;unsigned failures=0;
  for(long budget=0;budget<200 && !reached_success;++budget) {
    allocation_budget=budget;
    const auto attempted=dt::CastDatatypeValue(cast);
    allocation_budget=-1;
    if(attempted.ok()) reached_success=true;
    else {
      ++failures;
      Check(attempted.value.encoded_value.empty() &&
            attempted.diagnostic.diagnostic_code=="DATATYPE.RESOURCE_EXHAUSTED" &&
            attempted.status.code==scratchbird::core::platform::StatusCode::memory_allocation_failed,
            "REAL64 decimal allocation refusal loses native status or returns payload");
    }
  }
  Check(reached_success && failures>1,"REAL64 decimal full owner allocation sweep");
  // Include failure publication, not only successful quantization. After the
  // first computed-fact diagnostic allocation, all later faults retain facts.
  for (const std::uint64_t bits : {0x4023e66666666667ULL,0x7ff8000000000001ULL}) {
    auto failing=cast;
    failing.target_descriptor=Core(Descriptor(2,1));
    Check(dt::BindExactDecimalSortKeyProfile(failing.target_descriptor,&codec),"failure codec binding");
    failing.decimal_target_codec={codec.decimal_codec_uuid,codec.decimal_codec_generation};
    failing.numeric_context.precision=2;
    for(unsigned i=0;i<8;++i) failing.value.encoded_value[i]=static_cast<char>(bits>>(8*i));
    const bool nan=bits==0x7ff8000000000001ULL;
    bool completed=false,retained=false;
    for(long budget=0;budget<300 && !completed;++budget) {
      allocation_budget=budget;
      const auto result=dt::CastDatatypeValue(failing);
      const bool injected=allocation_budget==-1;
      allocation_budget=-1;
      Check(!result.ok() && result.value.encoded_value.empty(),"failed cast published a value");
      const bool facts=nan ? result.numeric_facts.invalid :
          result.numeric_facts.overflow && result.numeric_facts.inexact;
      if(injected) {
        Check(result.status.code==scratchbird::core::platform::StatusCode::memory_allocation_failed &&
              result.diagnostic.diagnostic_code=="DATATYPE.RESOURCE_EXHAUSTED",
              "failed cast allocation status");
        Check(!retained || facts,"later diagnostic allocation dropped computed facts");
        retained=retained || facts;
      } else {
        Check(facts,"native failed cast lost facts");completed=true;
      }
    }
    Check(completed && retained,"failure diagnostic allocation sweep missing computed-fact coverage");
  }
  for(bool is_null : {false,true}) for(bool legacy_explicit : {false,true}) {
    auto bad=cast;bad.context=dt::DatatypeCastContext::implicit;
    bad.explicit_cast=legacy_explicit;
    if(is_null) {bad.value.is_null=true;bad.value.encoded_value.clear();}
    Check(!dt::CastDatatypeValue(bad).ok(),"legacy explicit flag bypasses implicit cast refusal");
  }
  for(bool is_null : {false,true}) for(unsigned mutation=0;mutation<13;++mutation) {
    auto bad=cast;
    if(is_null) {bad.value.is_null=true;bad.value.encoded_value.clear();}
    switch(mutation) {
      case 0: bad.context=dt::DatatypeCastContext::implicit;break;
      case 1: bad.value.descriptor={};break;
      case 2: bad.target_descriptor={};break;
      case 3: bad.numeric_context.scale=2;break;
      case 4: bad.numeric_context.allow_special_values=true;break;
      case 5: bad.numeric_context.rounding=static_cast<dt::DatatypeRoundingMode>(99);break;
      case 6: if(is_null) bad.value.encoded_value="x";else bad.value.encoded_value.pop_back();break;
      case 7: bad.value.is_null=true;bad.value.encoded_value="x";break;
      case 8: bad.target_descriptor.security_policy_uuid=bad.target_descriptor.descriptor_uuid;break;
      case 9: bad.value.descriptor.descriptor_epoch++;break;
      case 10: bad.decimal_target_codec={};break;
      case 11: bad.decimal_target_codec.generation++;break;
      case 12: bad.decimal_target_codec.codec_uuid.bytes[15]^=1;break;
    }
    const auto refused=dt::CastDatatypeValue(bad);
    Check(!refused.ok() && refused.value.encoded_value.empty(),"invalid bound decimal cast published bytes");
  }
  cast.value.is_null=true;cast.value.encoded_value.clear();
  converted=dt::CastDatatypeValue(cast);
  Check(converted.ok() && converted.value.is_null && converted.value.encoded_value.empty(),
        "typed NULL REAL64 decimal cast");
}
int main() try {
  Matrix(); InvalidBackend(); CoreAndEngine(); Binary64DecimalCasts();
  std::cout<<checks<<" native decimal arithmetic checks passed\n";
  return 0;
} catch(const std::exception& e) {
  forbid_allocation=false; allocation_budget=-1; std::cerr<<e.what()<<" after "<<checks<<" checks\n";return 1;
}
