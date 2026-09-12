// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "crud_support/crud_store.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "canonical_diagnostic_catalog.hpp"
#include "time.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <type_traits>

namespace api = scratchbird::engine::internal_api;
namespace p = scratchbird::core::platform;
namespace t = scratchbird::core::time;
namespace {
std::atomic<long> fail_after{-1};
unsigned checks=0,failures=0,allocation_faults=0,entropy_calls=0,clock_calls=0;
int entropy_mode=1;
p::WallClockTime clock_value{12,345000000};
bool clock_fails=false;
}
void* operator new(std::size_t n) {
  auto remaining=fail_after.load();
  if (remaining>=0) {
    if (!remaining) throw std::bad_alloc();
    --fail_after;
  }
  if (auto* ptr=std::malloc(n?n:1)) return ptr;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr,std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr,std::size_t) noexcept { std::free(ptr); }

namespace {
void Check(bool ok,const char* detail) {
  ++checks;
  if (!ok && ++failures<20) std::cerr<<"FAIL "<<detail<<'\n';
}
api::EngineUuid Expected(std::uint64_t millis) {
  api::EngineUuid value;
  value.bytes.fill(0x5c);
  for (unsigned i=0;i<6;++i)
    value.bytes[i]=static_cast<p::byte>(millis>>(40-i*8));
  value.bytes[6]=0x7c;
  value.bytes[8]=0x9c;
  return value;
}
template<class Work>
void Refused(Work work,const char* code,bool allocation_sweep=false) {
  const auto original=Expected(999);
  unsigned faults=0;
  for (long fail=allocation_sweep?0:-1;fail<16;++fail) {
    auto destination=original;
    bool refused=false,oom=false;
    fail_after=fail;
    try { destination=work(); }
    catch (const api::CrudIdentityIssuanceError& error) {
      fail_after=-1;
      refused=true;
      Check(error.diagnostic().diagnostic_code==code && std::strcmp(error.what(),code)==0,
            "source cause was lost in identity failure");
      Check(!error.diagnostic().status.ok(), "identity error carried successful status");
      const auto* registration=scratchbird::core::diagnostics::FindCanonicalDiagnosticCode(code);
      Check(registration && registration->is_failure,"identity failure code is not registered");
    } catch (const std::bad_alloc&) {
      fail_after=-1;
      refused=true;
      oom=true;
      ++faults;
    }
    fail_after=-1;
    Check(refused && destination==original,"identity failure published nil/partial/replacement value");
    if (!oom) break;
  }
  allocation_faults+=faults;
  if (allocation_sweep) Check(faults>0,"refusal allocation sweep exercised no actual allocation failure");
}
void Generation() {
  static_assert(sizeof(api::EngineUuid)==16);
  static_assert(std::is_same_v<decltype(api::GenerateCrudEngineUuid("row")),api::EngineUuid>);
  static_assert(!std::is_convertible_v<api::EngineUuid,std::string>);
  static_assert(std::is_same_v<decltype(api::UuidOrGenerated({},"row")),api::EngineUuid>);
  for (const auto kind:{"object","row","schema","database","transaction","constraint_dependency"}) {
    for (const auto millis:std::array<std::uint64_t,5>{0,1,255,0x010203040506ULL,0xffffffffffffULL}) {
      const auto before=entropy_calls;
      const auto clock_before=clock_calls;
      const auto value=api::GenerateCrudEngineUuid(kind,millis);
      Check(value==Expected(millis),"CRUD issuer changed binary source bytes");
      Check(entropy_calls==before+1 && clock_calls==clock_before,
            "explicit timestamp was replaced by current time or extra issuance");
    }
  }
  const auto before=clock_calls;
  Check(api::GenerateCrudEngineUuid("row")==Expected(12345),"default clock conversion changed source value");
  Check(clock_calls==before+1,"default issuer did not read actual clock entry point");
  Check(api::UuidOrGenerated({},"row")==Expected(12345),"absent system UUID did not generate binary identity");
  for (const auto millis:std::array<std::uint64_t,3>{
          0x1000000000000ULL,0x1000000000001ULL,(std::numeric_limits<std::uint64_t>::max)()}) {
    const auto before=entropy_calls;
    Refused([&]{return api::GenerateCrudEngineUuid("row",millis);},"TIME.UUID_TIMESTAMP_OUT_OF_RANGE",true);
    Check(entropy_calls==before,"out-of-range time consumed entropy");
  }
  for (const auto mode:{0,-1}) {
    entropy_mode=mode;
    auto before=entropy_calls;
    Refused([]{return api::GenerateCrudEngineUuid("object",0);},"TIME.UUID_RANDOMNESS_UNAVAILABLE");
    Check(entropy_calls==before+1,"entropy refusal recursively generated error UUID");
    Refused([]{return api::GenerateCrudEngineUuid("object");},"TIME.UUID_RANDOMNESS_UNAVAILABLE",true);
  }
  entropy_mode=1;
  for (const auto bad:std::array<p::WallClockTime,4>{
          p::WallClockTime{-1,0},p::WallClockTime{0,1000000000},
          p::WallClockTime{p::i64{1}<<61,0},p::WallClockTime{281474976710LL,656000000}}) {
    clock_value=bad;
    const auto before=entropy_calls;
    Refused([]{return api::GenerateCrudEngineUuid("object");},"TIME.UUID_TIMESTAMP_OUT_OF_RANGE");
    Check(entropy_calls==before,"invalid clock conversion reached entropy provider");
  }
  clock_value={12,345000000};
  clock_fails=true;
  const auto entropy_before=entropy_calls;
  Refused([]{return api::GenerateCrudEngineUuid("object");},"TIME.UUID_TIME_REGRESSION");
  Check(entropy_calls==entropy_before,"failed clock source reached UUID entropy");
  clock_fails=false;
}
void SuppliedIdentity() {
  const auto base=Expected(12345);
  clock_fails=true;
  entropy_mode=0;
  for (unsigned byte=0;byte<16;++byte) {
    for (unsigned raw=0;raw<256;++raw) {
      auto candidate=base;
      candidate.bytes[byte]=static_cast<p::byte>(raw);
      const bool valid=candidate.bytes[6]>>4==7 && candidate.bytes[8]>>6==2;
      const auto entropy_before=entropy_calls,clock_before=clock_calls;
      if (valid) {
        fail_after=0;
        const auto admitted=api::UuidOrGenerated(candidate,"row");
        fail_after=-1;
        Check(admitted==candidate,"supplied binary UUID was rewritten or allocated");
      } else {
        Refused([&]{return api::UuidOrGenerated(candidate,"row");},"UUID.ENGINE_IDENTITY_NOT_V7");
      }
      Check(entropy_calls==entropy_before && clock_calls==clock_before,
            "supplied UUID was replaced by new time/entropy");
      Check(candidate.bytes[byte]==raw,"supplied UUID input mutated");
    }
  }
  auto invalid=base;
  invalid.bytes[6]=0x4c;
  Refused([&]{return api::UuidOrGenerated(invalid,"object");},"UUID.ENGINE_IDENTITY_NOT_V7",true);
  clock_fails=false;
  entropy_mode=1;
}
void PrimaryObjectBinding() {
  static_assert(std::is_same_v<decltype(api::ApiBehaviorObjectUuid(
      std::declval<const api::EngineApiRequest&>(),std::declval<const std::string&>())),
      api::EngineUuid>);
  api::EngineApiRequest request;
  request.target_database.uuid=Expected(11);
  request.target_schema.uuid=Expected(12);
  request.related_objects.push_back({Expected(13),"object"});
  request.related_objects.push_back({Expected(14),"object"});
  for (const auto kind:{"object","database","schema","policy"}) {
    const auto before=entropy_calls;
    const auto generated=api::ApiBehaviorObjectUuid(request,kind);
    Check(generated==Expected(12345) && entropy_calls==before+1,
          "related/database/schema reference was adopted as primary identity");
    Check(request.related_objects[0].uuid==Expected(13) && request.related_objects[1].uuid==Expected(14),
          "primary identity issuance changed dependencies");
  }
  request.target_object.uuid=Expected(15);
  clock_fails=true;
  entropy_mode=0;
  const auto before=entropy_calls,clock_before=clock_calls;
  Check(api::ApiBehaviorObjectUuid(request,"object")==Expected(15),
        "explicit primary target was replaced by dependency or issuance");
  Check(entropy_calls==before && clock_calls==clock_before,
        "explicit primary target unnecessarily requested a new identity");
  request.target_object.uuid.bytes[6]=0x4c;
  Refused([&]{return api::ApiBehaviorObjectUuid(request,"object");},"UUID.ENGINE_IDENTITY_NOT_V7");
  request.target_object.uuid=Expected(15);
  Check(api::ApiBehaviorPrimaryName(request,"missing-name")=="missing-name",
        "system identity escaped as a display-cache name");
  request.option_envelopes={"unrelated:value","name:explicit-option"};
  Check(api::ApiBehaviorPrimaryName(request,"missing-name")=="explicit-option",
        "explicit name metadata changed");
  request.localized_names.push_back({"en","default","","localized-label",true});
  Check(api::ApiBehaviorPrimaryName(request,"missing-name")=="localized-label",
        "explicit localized display metadata changed");
  clock_fails=false;
  entropy_mode=1;
}
} // namespace

// Fault/clock injection is confined to this test's linker. Product sources
// have no deterministic generation path and no replacement provider switch.
extern "C" int __wrap_RAND_bytes(unsigned char* bytes,int count) {
  // No allocation in the fixture backend: failures reach product diagnostic
  // construction, not provider plumbing.
  ++entropy_calls;
  if (count>0) std::memset(bytes,0x5c,static_cast<std::size_t>(entropy_mode==1?count:count/2));
  return entropy_mode;
}
extern "C" t::ClockSnapshotResult
__wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv() {
  ++clock_calls;
  t::ClockSnapshotResult result;
  result.value.wall_clock=clock_value;
  result.value.monotonic.ticks=clock_calls;
  if (clock_fails) {
    result.status={p::StatusCode::time_source_unavailable,p::Severity::error,p::Subsystem::time};
    result.diagnostic=p::MakeDiagnostic(result.status.code,result.status.severity,result.status.subsystem,
        "TIME.UUID_TIME_REGRESSION","test.clock.refused",{},"","test.clock");
  }
  return result;
}
int main() {
  try { Generation(); SuppliedIdentity(); PrimaryObjectBinding(); }
  catch (const std::exception& error) {
    fail_after=-1;
    std::cerr<<"unexpected exception "<<error.what()<<'\n';
    return 2;
  }
  std::cout<<"checks="<<checks<<" failures="<<failures<<" allocation_faults="<<allocation_faults<<'\n';
  return failures?1:0;
}
