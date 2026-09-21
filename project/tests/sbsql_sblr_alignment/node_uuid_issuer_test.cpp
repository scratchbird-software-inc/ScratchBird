// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "node_uuid_issuer.hpp"
#include "canonical_diagnostic_catalog.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace p = scratchbird::core::platform;
namespace t = scratchbird::core::time;
namespace u = scratchbird::core::uuid;
namespace {
using E = u::StandaloneUuidV7Error;
unsigned checks = 0;
std::atomic<bool> controlled{true}, entropy_failed{false}, clock_failed{false}, allocation_failed{false};
std::atomic<bool> hold_clock{false}, in_clock{false}, release_clock{false};
std::atomic<unsigned> entropy_calls{0};
std::atomic<p::u64> millis{123456}, ticks{100};
std::array<unsigned char,16> seed{};
std::atomic<bool> negative_wall{false}, bad_nanos{false};
void Check(bool value, const char* detail) { ++checks; if (!value) throw std::runtime_error(detail); }
p::Uuid Id(unsigned value) { p::Uuid id; id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=value;return id; }
u::StandaloneUuidV7Binding Binding() { return {Id(1),Id(2)}; }
u::StandaloneUuidV7Policy Policy() { return {{},0,3}; }
void Reset() {
  controlled=true;entropy_failed=clock_failed=allocation_failed=false;
  negative_wall=bad_nanos=hold_clock=in_clock=release_clock=false;
  millis=123456;ticks=100;seed.fill(0);entropy_calls=0;
}
void Refused(const u::StandaloneUuidV7Issue& result,E error) {
  Check(!result.ok()&&result.error==error&&!result.value&&!result.observation,"refusal has exact error and no identity/observation prefix");
}
p::Uuid Expected(p::u64 timestamp,unsigned high,p::u64 low) {
  p::Uuid id;
  for(unsigned n=0;n<6;++n)id.bytes[n]=timestamp>>(40-8*n);
  id.bytes[6]=0x70|(high>>8);id.bytes[7]=high;
  id.bytes[8]=0x80|(low>>56);
  for(unsigned n=9;n<16;++n)id.bytes[n]=low>>(8*(15-n));
  return id;
}
void SetSeed(unsigned high,p::u64 low) { seed=Expected(0,high,low).bytes; }
void BasicAndCarry() {
  Reset();u::StandaloneUuidV7Issuer issuer(Binding(),Policy());
  Check(issuer.binding()==Binding(),"exact immutable node and policy binding");
  Refused(issuer.Issue(p::UuidKind::unknown),E::invalid_kind);
  Check(!entropy_calls,"invalid kind never issues seed");
  for(unsigned n=0;n<2048;++n) {
    const auto result=issuer.Issue(p::UuidKind::object);
    Check(result.ok()&&result.value->value==Expected(millis,0,n)&&result.value->kind==p::UuidKind::object,
      "independent exact same-millisecond increment layout");
  }
  Check(entropy_calls==1,"same millisecond retains one seed");
  entropy_failed=true;
  Check(issuer.Issue(p::UuidKind::page).ok()&&entropy_calls==1,"same-millisecond increment does not consult entropy");
  ++millis;++ticks;Refused(issuer.Issue(p::UuidKind::page),E::randomness_unavailable);
  --millis;--ticks;
  const auto retained=issuer.Issue(p::UuidKind::page);
  Check(retained.ok()&&retained.value->value==Expected(millis,0,2049),"failed future seed preserves prior allocation and observation");
  entropy_failed=false;++millis;++ticks;
  const auto next=issuer.Issue(p::UuidKind::row);
  Check(next.ok()&&next.value->value==Expected(millis,0,0),"later accepted time obtains fresh seed");
  constexpr p::u64 maximum=(p::u64{1}<<62)-1;
  for(unsigned high:{0u,255u,256u,4094u}) {
    Reset();SetSeed(high,maximum);u::StandaloneUuidV7Issuer carry(Binding(),Policy());
    const auto first=carry.Issue(p::UuidKind::object),second=carry.Issue(p::UuidKind::object);
    Check(first.ok()&&first.value->value==Expected(millis,high,maximum),"exact pre-carry74-bit seed");
    Check(second.ok()&&second.value->value==Expected(millis,high+1,0),"full rand_b to rand_a carry preserves fixed bits");
  }
}
void AdmissionAndClocks() {
  Reset();
  for(unsigned version=0;version<16;++version)if(version!=7)for(unsigned field=0;field<2;++field) {
    auto binding=Binding();auto& id=field?binding.policy_snapshot_uuid:binding.database_uuid;id.bytes[6]=version<<4;
    u::StandaloneUuidV7Issuer issuer(binding,Policy());Refused(issuer.Issue(p::UuidKind::object),E::invalid_binding);
  }
  for(unsigned field=0;field<2;++field) {
    auto binding=Binding();(field?binding.policy_snapshot_uuid:binding.database_uuid)={};
    u::StandaloneUuidV7Issuer issuer(binding,Policy());Refused(issuer.Issue(p::UuidKind::object),E::invalid_binding);
  }
  for(unsigned field=0;field<3;++field) {
    auto policy=Policy();if(field==0)policy.same_ms_wait_timeout_ms=0;
    if(field==1)policy.same_ms_wait_timeout_ms=(std::numeric_limits<p::u64>::max)();
    if(field==2)policy.max_uuid_regression_ms=p::u64{1}<<48;
    u::StandaloneUuidV7Issuer issuer(Binding(),policy);Refused(issuer.Issue(p::UuidKind::object),E::invalid_policy);
  }
  Check(!entropy_calls,"invalid binding and policy do not consume identity sources");
  u::StandaloneUuidV7Issuer issuer(Binding(),Policy());
  clock_failed=true;Refused(issuer.Issue(p::UuidKind::object),E::clock_failure);clock_failed=false;
  allocation_failed=true;Refused(issuer.Issue(p::UuidKind::object),E::resource_exhausted);allocation_failed=false;
  negative_wall=true;Refused(issuer.Issue(p::UuidKind::object),E::timestamp_out_of_range);negative_wall=false;
  bad_nanos=true;Refused(issuer.Issue(p::UuidKind::object),E::timestamp_out_of_range);bad_nanos=false;
  millis=p::u64{1}<<48;Refused(issuer.Issue(p::UuidKind::object),E::timestamp_out_of_range);millis=123456;
  const auto first=issuer.Issue(p::UuidKind::object);Check(first.ok()&&first.value->value==Expected(millis,0,0),"source failures never advance instance");
  --millis;Refused(issuer.Issue(p::UuidKind::object),E::time_regression);++millis;
  --ticks;Refused(issuer.Issue(p::UuidKind::object),E::time_regression);++ticks;
  const auto after=issuer.Issue(p::UuidKind::object);Check(after.ok()&&after.value->value==Expected(millis,0,1),"failed regressions preserve exact counter");
  auto policy=Policy();policy.clock.max_wall_clock_backward_nanoseconds=2000000;policy.max_uuid_regression_ms=2;
  u::StandaloneUuidV7Issuer bounded(Binding(),policy);Check(bounded.Issue(p::UuidKind::object).ok(),"bounded regression starting allocation");
  const auto accepted_millis=millis.load();millis-=2;
  const auto permitted=bounded.Issue(p::UuidKind::object);
  Check(permitted.ok()&&permitted.value->value==Expected(accepted_millis,0,1)&&
    permitted.observation->wall_clock.unix_seconds==p::i64(millis/1000),"permitted regression retains UUID prefix but actual physical observation");
  --millis;Refused(bounded.Issue(p::UuidKind::object),E::time_regression);
  policy.clock.fail_closed_on_wall_clock_rollback=false;u::StandaloneUuidV7Issuer warning(Binding(),policy);
  millis=123456;Check(warning.Issue(p::UuidKind::object).ok(),"policy warning baseline");
  policy.clock.max_wall_clock_backward_nanoseconds=0;
  u::StandaloneUuidV7Issuer warns(Binding(),policy);Check(warns.Issue(p::UuidKind::object).ok(),"explicit warning policy baseline");
  --millis;const auto allowed=warns.Issue(p::UuidKind::object);
  Check(allowed.ok()&&allowed.clock_decision==t::LocalClockObservationDecision::wall_clock_rollback_detected,
    "allowed clock warning is observable and UUID regression still bounded");
  Reset();t::LocalTimeAuthorityState state{true,{100},{1000,0},(std::numeric_limits<p::u64>::max)()};
  const auto exhausted=t::ObserveLocalNodeClock(state,{{101},{1001,0}},{});
  Check(!exhausted.ok()&&exhausted.decision==t::LocalClockObservationDecision::counter_exhausted&&
    exhausted.state.accepted_observations==state.accepted_observations&&exhausted.state.last_monotonic.ticks==100,
    "actual core observation counter overflow does not wrap or publish state");
  const auto* diagnostic=scratchbird::core::diagnostics::FindCanonicalDiagnosticCode(exhausted.diagnostic.diagnostic_code);
  Check(diagnostic&&diagnostic->is_failure&&diagnostic->sqlstate=="55000", "counter exhaustion uses actual admitted diagnostic registration");
  state.accepted_observations=1;auto clock_policy=t::LocalTimeAuthorityPolicy{};
  clock_policy.max_wall_clock_forward_jump_nanoseconds=(std::numeric_limits<p::u64>::max)();clock_policy.fail_closed_on_wall_clock_forward_jump=true;
  const auto wide=t::ObserveLocalNodeClock(state,{{101},{1001,0}},clock_policy);
  Check(wide.ok()&&wide.state.accepted_observations==2,"forward-jump policy addition cannot overflow into false refusal");
}
void ExhaustionAndConcurrency() {
  Reset();seed.fill(255);u::StandaloneUuidV7Issuer issuer(Binding(),Policy());
  const auto first=issuer.Issue(p::UuidKind::object);Check(first.ok()&&first.value->value==Expected(millis,4095,(p::u64{1}<<62)-1),"maximum74-bit value is issued once");
  const auto start=std::chrono::steady_clock::now();Refused(issuer.Issue(p::UuidKind::object),E::sequence_exhausted);
  Check(std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(3)&&entropy_calls==1,"exhaustion waits bounded real duration without reseeding or synthetic tick");
  ++millis;++ticks;seed.fill(0);const auto recovered=issuer.Issue(p::UuidKind::object);
  Check(recovered.ok()&&recovered.value->value==Expected(millis,0,0)&&entropy_calls==2,"later accepted physical millisecond recovers exhausted instance");
  Reset();u::StandaloneUuidV7Issuer shared(Binding(),Policy());std::array<std::vector<p::Uuid>,4> values;
  std::array<std::thread,4> workers;std::atomic<bool> failed=false;
  for(unsigned n=0;n<workers.size();++n)workers[n]=std::thread([&,n] {
    for(unsigned i=0;i<500;++i){const auto result=shared.Issue(p::UuidKind::object);if(!result.ok())failed=true;else values[n].push_back(result.value->value);}
  });
  for(auto& worker:workers)worker.join();Check(!failed&&entropy_calls==1,"shared allocation instance concurrency retains one seed");
  std::vector<p::Uuid> ordered;for(const auto& batch:values)ordered.insert(ordered.end(),batch.begin(),batch.end());std::sort(ordered.begin(),ordered.end());
  Check(ordered.size()==2000,"every concurrent allocation returned");
  for(unsigned n=0;n<ordered.size();++n)Check(ordered[n]==Expected(millis,0,n),"concurrent allocation is complete unique independent sequence");
  // The issuer is inside its mutex in another thread when fork occurs.
  hold_clock=true;release_clock=false;std::thread holder([&]{(void)shared.Issue(p::UuidKind::object);});
  while(!in_clock)std::this_thread::yield();const auto child=fork();
  if(!child){const auto refused=shared.Issue(p::UuidKind::object);_exit(refused.error==E::wrong_process&&!refused.value?0:80);}
  release_clock=true;holder.join();Check(child>0,"fork owning issuer refusal fixture");int status=0;
  Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"inherited context refuses before touching inherited locked mutex");
  hold_clock=false;Check(shared.Issue(p::UuidKind::object).ok(),"parent allocation lifetime remains usable after fork");
}
} // namespace
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* bytes,int count) {
  if(!controlled)return __real_RAND_bytes(bytes,count);
  ++entropy_calls;if(entropy_failed){if(count>0)bytes[0]=0xff;return 0;}
  if(count!=16)return 0;std::copy(seed.begin(),seed.end(),bytes);return 1;
}
extern "C" t::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" t::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv() {
  if(!controlled)return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
  if(hold_clock){in_clock=true;while(!release_clock)std::this_thread::yield();}
  if(allocation_failed)throw std::bad_alloc();
  t::ClockSnapshotResult result;
  if(clock_failed){result.status={p::StatusCode::time_source_unavailable,p::Severity::error,p::Subsystem::time};return result;}
  result.value={{ticks.load()},{negative_wall?-1:static_cast<p::i64>(millis/1000),bad_nanos?1000000000u:static_cast<p::u32>((millis%1000)*1000000)}};
  return result;
}
int main() {
  try {
    BasicAndCarry();AdmissionAndClocks();ExhaustionAndConcurrency();
    controlled=false;u::StandaloneUuidV7Issuer real(Binding(),Policy());
    const auto first=real.Issue(p::UuidKind::page),second=real.Issue(p::UuidKind::page);
    Check(first.ok()&&second.ok()&&first.value->value<second.value->value,"real core clock and cryptographic entropy allocate actual ordered binary UUIDv7");
    std::cout<<"PASS node UUID issuer checks="<<checks<<" not_SQL_E2E=true\n";return 0;
  }catch(const std::exception& error){std::cerr<<"FAIL node UUID issuer checks="<<checks<<" "<<error.what()<<'\n';return 1;}
}
