// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"
#include <mpfr.h>
#include <atomic>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace n = scratchbird::libraries::sbl_numeric;
using U = std::uint64_t;
unsigned checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
n::Real64Bytes Bytes(U bits) {
  n::Real64Bytes bytes{};
  for (unsigned i = 0; i < 8; ++i) bytes[i] = bits >> (8*i);
  return bytes;
}
U Bits(const n::Real64Bytes& bytes) {
  U bits=0; for(unsigned i=0;i<8;++i) bits |= U{bytes[i]} << (8*i);
  return bits;
}
U Parse(const char* text, n::NumericContext context = {}) {
  const auto r=n::EncodeReal64LittleEndian(text,context);
  Check(r.numeric.status==n::NumericStatusCode::ok && r.bytes.has_value(),"parse refused");
  return Bits(*r.bytes);
}
n::Real64BinaryResult Op(n::NumericOperation op, U a, U b, n::NumericContext context={}) {
  return n::ApplyReal64BinaryOperation({op,Bytes(a),Bytes(b),context});
}
int main() try {
  constexpr U one=0x3ff0000000000000ULL, negative=U{1}<<63;
  Check(Parse("1")==one && Parse("-0")==negative,"identity/zero bits");
  Check(Parse("0x1p-1074")==1 && Parse("0x1p-1022")==0x0010000000000000ULL,"finite bounds");
  Check(Parse("0x1.fffffffffffffp1023")==0x7fefffffffffffffULL,"maximum finite");
  auto r=Op(n::NumericOperation::add,one,0x3ca0000000000000ULL);
  Check(r.bytes && Bits(*r.bytes)==one && r.numeric.inexact,"half-even tie");
  n::NumericContext away; away.rounding=n::RoundingMode::half_up;
  r=Op(n::NumericOperation::add,one,0x3ca0000000000000ULL,away);
  Check(r.bytes && Bits(*r.bytes)==one+1 && r.numeric.inexact,"half-away tie");
  n::NumericContext trunc; trunc.rounding=n::RoundingMode::truncate;
  r=Op(n::NumericOperation::add,one,0x3ca8000000000000ULL,trunc);
  Check(r.bytes && Bits(*r.bytes)==one && r.numeric.inexact,"truncate");
  r=Op(n::NumericOperation::divide,1,0x4000000000000000ULL);
  Check(r.bytes && Bits(*r.bytes)==0 && r.numeric.underflow && r.numeric.inexact,"tiny inexact tie");
  r=Op(n::NumericOperation::divide,0x0010000000000000ULL,0x4000000000000000ULL);
  Check(r.bytes && Bits(*r.bytes)==0x0008000000000000ULL && r.numeric.subnormal &&
        !r.numeric.underflow && !r.numeric.inexact,"exact subnormal");
  r=Op(n::NumericOperation::multiply,0x7fefffffffffffffULL,0x4000000000000000ULL);
  Check(!r.bytes && r.numeric.overflow && r.numeric.diagnostic_code=="NUMERIC.REAL64.OVERFLOW","overflow");
  r=Op(n::NumericOperation::divide,one,negative);
  Check(!r.bytes && r.numeric.divide_by_zero && r.numeric.diagnostic_code=="NUMERIC.REAL64.DIVIDE_BY_ZERO","zero division");
  r=Op(n::NumericOperation::compare,0,negative);
  Check(!r.bytes && r.numeric.status==n::NumericStatusCode::ok && r.numeric.comparison==0,"signed zero equality");
  n::NumericContext special; special.allow_special_values=true;
  const U qnan=0xfff8000000000345ULL,snan=0x7ff0000000000123ULL;
  auto raw=Bytes(qnan);
  r=n::DecodeReal64LittleEndian(raw.data(),8,special);
  Check(r.bytes && Bits(*r.bytes)==qnan,"NaN preservation");
  r=Op(n::NumericOperation::add,qnan,one,special);
  Check(r.bytes && Bits(*r.bytes)==qnan,"quiet payload propagation");
  r=Op(n::NumericOperation::compare,qnan,one,special);
  Check(!r.bytes && r.numeric.status==n::NumericStatusCode::unordered && !r.numeric.invalid,"unordered comparison");
  r=Op(n::NumericOperation::add,snan,one,special);
  Check(!r.bytes && r.numeric.invalid,"signaling invalid");
  r=Op(n::NumericOperation::add,qnan,one);
  Check(!r.bytes && r.numeric.invalid,"default special refusal");
  for (const char* bad : {"", "1x", "1 2", "0x", "1e+", "--1", "1,2"})
    Check(!n::EncodeReal64LittleEndian(bad).bytes,"malformed lexical accepted");
  for (auto size : {0u,1u,7u,9u,16u})
    Check(!n::DecodeReal64LittleEndian(raw.data(),size,special).bytes,"wrong width accepted");
  Check(!n::DecodeReal64LittleEndian(nullptr,8,special).bytes,"null span");
  auto invalid=special; invalid.rounding=static_cast<n::RoundingMode>(99);
  Check(!n::DecodeReal64LittleEndian(raw.data(),8,invalid).bytes,"invalid context");
  for(auto width:{1u,2u,4u,8u,16u}) for(bool sign:{false,true}) {
    std::vector<std::uint8_t> integer(width,0xff);
    auto converted=n::IntegerLittleEndianToReal64(integer.data(),integer.size(),sign);
    Check(converted.bytes.has_value(),"native integer conversion");
    if(sign) Check(Bits(*converted.bytes)==0xbff0000000000000ULL,"negative integer sign extension");
    auto exact=n::Real64ToIntegerLittleEndian(Bytes(one),width,sign);
    Check(exact.bytes.size()==width && exact.bytes[0]==1 &&
          exact.numeric.status==n::NumericStatusCode::ok,"exact integer destination");
    auto loss=n::Real64ToIntegerLittleEndian(Bytes(0x3ff8000000000000ULL),width,sign);
    Check(loss.bytes.empty() && loss.numeric.invalid && loss.numeric.inexact,"fractional destination cannot truncate");
    const unsigned exponent=width*8-(sign?1:0);
    auto boundary=n::EncodeReal64LittleEndian("0x1p"+std::to_string(exponent));
    auto outside=n::Real64ToIntegerLittleEndian(*boundary.bytes,width,sign);
    Check(outside.bytes.empty() && outside.numeric.overflow,"exclusive positive integer bound");
    if(sign) {
      (*boundary.bytes)[7] |= 0x80;
      auto minimum=n::Real64ToIntegerLittleEndian(*boundary.bytes,width,sign);
      Check(minimum.bytes.size()==width && minimum.bytes.back()==0x80,"inclusive negative integer bound");
    }
  }
  auto wide=n::EncodeReal128LittleEndian("0x1.00000000000008p0");
  r=n::Real128ToReal64(*wide.bytes);
  Check(r.bytes && Bits(*r.bytes)==one && r.numeric.inexact,"native narrowing tie");
  r=n::Real128ToReal64(*wide.bytes,away);
  Check(r.bytes && Bits(*r.bytes)==one+1 && r.numeric.inexact,"native narrowing half-up");
  wide=n::EncodeReal128LittleEndian("0x1.0000000000000800000000000001p0");
  r=n::Real128ToReal64(*wide.bytes);
  Check(r.bytes && Bits(*r.bytes)==one+1 && r.numeric.inexact,"native narrowing sticky bit");
  const auto widened=n::Real64ToReal128(Bytes(1));
  r=n::Real128ToReal64(*widened.bytes);
  Check(r.bytes && Bits(*r.bytes)==1 && !r.numeric.inexact,"native subnormal widening roundtrip");
  wide=n::Real64ToReal128(Bytes(qnan),special);
  r=n::Real128ToReal64(*wide.bytes,special);
  Check(r.bytes && Bits(*r.bytes)==qnan && !r.numeric.inexact,"NaN sign/payload widening roundtrip");
  U state=0x12de3456789abcdfULL;
  for(unsigned i=0;i<10000;++i) {
    state^=state<<13;state^=state>>7;state^=state<<17;
    const auto b=Bytes(state);
    auto decoded=n::DecodeReal64LittleEndian(b.data(),b.size(),special,true);
    Check(decoded.bytes && *decoded.bytes==b,"opaque binary round trip");
    if(((state>>52)&2047)!=2047) {
      auto encoded=n::EncodeReal64LittleEndian(decoded.numeric.value.encoded,special);
      Check(encoded.bytes && *encoded.bytes==b,"finite decimal round trip");
    }
  }
  const auto old_round=std::fegetround();
  const auto old_flags=mpfr_flags_save(); const auto old_min=mpfr_get_emin(),old_max=mpfr_get_emax();
  std::fesetround(FE_UPWARD); mpfr_set_emin(-100);mpfr_set_emax(100);mpfr_set_divby0();
  const auto flags=mpfr_flags_save();
  Check(Parse("0x1.00000000000008p0")==one,"host rounding leaked");
  Check(mpfr_get_emin()==-100 && mpfr_get_emax()==100 && mpfr_flags_save()==flags &&
        std::fegetround()==FE_UPWARD,"caller environment changed");
  mpfr_set_emin(old_min);mpfr_set_emax(old_max);mpfr_flags_restore(old_flags,MPFR_FLAGS_ALL);std::fesetround(old_round);
  std::atomic<bool> valid{true}; std::vector<std::thread> threads;
  for(unsigned i=0;i<4;++i) threads.emplace_back([&]{
    for(unsigned j=0;j<500;++j) {auto x=Op(n::NumericOperation::add,one,one);
      if(!x.bytes || Bits(*x.bytes)!=0x4000000000000000ULL) valid=false;}
    n::ReleaseReal128ThreadCache();
  });
  for(auto& t:threads)t.join();Check(valid,"concurrent numeric corruption");
  n::ReleaseReal128ThreadCache();
  std::cout<<"REAL64 checks="<<checks<<" failures=0\n";
  return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
