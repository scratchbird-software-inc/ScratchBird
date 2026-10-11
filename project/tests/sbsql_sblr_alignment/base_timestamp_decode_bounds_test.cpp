// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/timestamp_successor_fixture.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace dt=scratchbird::core::datatypes;
namespace p=scratchbird::core::platform;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why) {++checks;if(!ok)throw std::runtime_error(why);}
std::array<p::byte,16> Component(std::int64_t seconds,p::u32 nanos=0) {
  std::array<p::byte,16> bytes{};
  const auto bits=std::bit_cast<p::u64>(seconds);
  for(unsigned i=0;i<8;++i)bytes[i]=bits>>(8*i);
  for(unsigned i=0;i<4;++i)bytes[8+i]=nanos>>(8*i);
  return bytes;
}
void Invalid(const auto& result) {
  Check(!result.ok() && result.diagnostic.diagnostic_code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "invalid timestamp did not retain canonical-corruption diagnostic");
  Check(result.value.profile==nullptr && result.value.civil_day==0 &&
        result.value.nanoseconds_since_midnight==0,"invalid decoder published profile/value");
}
void Bounds(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  constexpr std::int64_t minimum=-185542587187200LL,maximum=185542587187199LL;
  dt::TimestampOwnedValueV3 epoch{profile,dt::TimestampValueStateV3::value,0,0};
  const auto val=dt::EncodeTimestampSbdvalComposedV3(epoch,true);
  const auto pv=dt::EncodeTimestampSbdpvComposedV3(epoch,true);
  Check(val.ok() && pv.ok() && val.bytes.size()==48 && pv.bytes.size()==40,"frame fixture failed");
  // First vector triggers the historical signed multiplication overflow.
  for(const auto seconds:{std::numeric_limits<std::int64_t>::min(),
       std::numeric_limits<std::int64_t>::min()+1,std::numeric_limits<std::int64_t>::min()+86399,
       minimum-1,maximum+1,std::numeric_limits<std::int64_t>::max()}) {
    const auto bytes=Component(seconds);
    Invalid(dt::DecodeCanonicalTimestampComponentNoAllocV3(*profile,dt::TimestampValueStateV3::value,true,bytes));
    auto sbdval=val.bytes,sbdpv=pv.bytes;
    std::copy(bytes.begin(),bytes.end(),sbdval.end()-16);
    std::copy(bytes.begin(),bytes.end(),sbdpv.end()-16);
    Invalid(dt::DecodeTimestampSbdvalComposedNoAllocV3(*profile,true,sbdval));
    Invalid(dt::DecodeTimestampSbdpvComposedNoAllocV3(*profile,true,sbdpv));
    for(const auto direction:{dt::TimestampSortDirectionV3::ascending,dt::TimestampSortDirectionV3::descending}) {
      auto key=dt::MakeTimestampSortKeyV3(epoch,direction,dt::TimestampNullModeV3::nulls_first);
      Check(key.ok() && key.bytes.size()==112,"key fixture failed");
      const auto bits=std::bit_cast<p::u64>(seconds)^0x8000000000000000ULL;
      for(unsigned i=0;i<8;++i) {
        auto octet=static_cast<p::byte>(bits>>(8*(7-i)));
        key.bytes[100+i]=direction==dt::TimestampSortDirectionV3::descending?static_cast<p::byte>(~octet):octet;
      }
      Invalid(dt::DecodeTimestampSortKeyNoAllocV3(*profile,key.bytes));
    }
  }
  for(const auto seconds:std::array<std::int64_t,10>{minimum,minimum+1,-86401,-86400,-1,0,1,86399,86400,maximum})
      for(p::u32 nanos:{0u,1u,999999999u}) {
    const auto bytes=Component(seconds,nanos);
    const auto result=dt::DecodeCanonicalTimestampComponentNoAllocV3(*profile,dt::TimestampValueStateV3::value,true,bytes);
    Check(result.ok(),"valid timestamp boundary refused");
    const std::int64_t quotient=seconds/86400;
    const std::int64_t remainder=seconds%86400;
    Check(result.value.civil_day==quotient-(remainder<0) &&
          result.value.nanoseconds_since_midnight==static_cast<p::u64>(remainder<0?remainder+86400:remainder)*1000000000+nanos,
          "negative timestamp normalization changed");
    dt::TimestampOwnedValueV3 owned{profile,result.value.state,result.value.civil_day,result.value.nanoseconds_since_midnight};
    const auto encoded=dt::EncodeCanonicalTimestampComponentV3(owned);
    Check(encoded.ok() && std::equal(encoded.bytes.begin(),encoded.bytes.end(),bytes.begin(),bytes.end()),
          "valid timestamp did not roundtrip exact bytes");
  }
}
}
int main() try {
  const p::Uuid cohort{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10}};
  auto built=dt::BuildCurrentTimestampValidatedProfileHandleV3(cohort);
  Check(built.ok(),"D710 profile failed");
  Bounds(std::make_shared<const dt::TimestampValidatedProfileHandleV3>(std::move(built.profile)));
  Bounds(scratchbird::tests::D711TimestampProfile());
  std::cout<<"timestamp decode bounds checks="<<checks<<'\n';
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
