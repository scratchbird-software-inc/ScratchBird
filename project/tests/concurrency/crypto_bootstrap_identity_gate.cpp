// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "uuid.hpp"
#include "time.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>

namespace u=scratchbird::core::uuid;
namespace p=scratchbird::core::platform;
namespace t=scratchbird::core::time;
std::string_view mode;
std::atomic<unsigned> entropy_calls{0}, rand_calls{0}, clock_calls{0};
bool recovery=false;
bool diagnostic_test=false;
unsigned checks=0;
void Check(bool value,const char* label) {
  ++checks; if (!value) {std::cerr<<"FAIL "<<label<<'\n'; std::abort();}
}
extern "C" ssize_t __real_getrandom(void*,size_t,unsigned);
extern "C" ssize_t __wrap_getrandom(void* output,size_t bytes,unsigned flags) {
  const auto call=++entropy_calls;
  if(flags!=GRND_NONBLOCK || bytes>16 || !bytes) std::abort();
  if(mode=="real") return __real_getrandom(output,bytes,flags);
  if(!recovery) {
    if(mode=="seal_busy")
      Check(u::SealCryptoBootstrapDiagnosticIdentities()==u::CryptoBootstrapDiagnosticError::busy,
            "seal cannot race an in-flight diagnostic issuance");
    if(mode=="fork_inflight" && call==1) {
      const auto child=fork(); Check(child>=0,"fork during claimed issuance");
      if(child==0) {
        if(diagnostic_test) _exit(u::IssueCryptoBootstrapDiagnosticIdentityV7().error==
            u::CryptoBootstrapDiagnosticError::foreign_process ? 0 : 1);
        _exit(u::IssueCryptoBootstrapIdentitiesV7()?1:0);
      }
      int status=0; pid_t reaped;
      do {reaped=waitpid(child,&status,0);} while(reaped<0 && errno==EINTR);
      Check(reaped==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
            "inflight fork inherits refusing claim before parent failure");
      errno=EIO; return -1;
    }
    if(mode=="unavailable") {errno=EAGAIN;return -1;}
    if(mode=="zero") return 0;
    if(mode=="interrupted") {errno=EINTR;return -1;}
    if(mode=="partial_failure" && call>1) {errno=EIO;return -1;}
    if(mode=="batch_failure" && call==3) {errno=EIO;return -1;}
    if(mode=="short" && call%2==1) {errno=EINTR;return -1;}
    if(mode=="partial_failure" || mode=="short") bytes=std::min(bytes,size_t{3});
  }
  std::memset(output,mode=="exhaustion"&&!recovery?0xff:0xa5,bytes);
  return static_cast<ssize_t>(bytes);
}
extern "C" int __wrap_RAND_bytes(unsigned char* out,int bytes) {
  ++rand_calls; if(bytes>0) std::memset(out,0x3c,static_cast<size_t>(bytes)/2);
  return 0; // ordinary source fails; OS bootstrap must never be its fallback
}
extern "C" t::ClockSnapshotResult __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
extern "C" t::ClockSnapshotResult __wrap__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv() {
  const auto call=++clock_calls;
  if(mode=="real") return __real__ZN11scratchbird4core4time26ReadLocalNodeClockSnapshotEv();
  t::ClockSnapshotResult result;
  std::uint64_t millis=123456;
  if (!recovery) {
    if(mode=="clock_failure") {
      result.status={p::StatusCode::time_source_unavailable,p::Severity::error,p::Subsystem::time};return result;
    }
    if(mode=="range") millis=0x1000000000000ULL;
    if(mode=="regression" && call>=2) --millis;
    if(mode=="batch_failure") millis+=call;
  }
  result.value.wall_clock={static_cast<p::i64>(millis/1000),static_cast<p::u32>(millis%1000*1000000)};
  result.value.monotonic.ticks=call;
  return result;
}
void Layout(const u::CryptoBootstrapIdentities& batch) {
  const std::array ids{batch.process,batch.operation,batch.owner,batch.context};
  for (unsigned i=0;i<4;++i) {
    Check(u::IsEngineIdentityUuid(ids[i]),"binary UUIDv7 variant");
    if(i) Check(ids[i-1]<ids[i],"batch order and distinct identities");
    if(mode!="real") {
      p::Uuid expected; expected.bytes.fill(0xa5);
      for(unsigned j=0;j<6;++j) expected.bytes[j]=static_cast<p::byte>(123456ULL>>(40-8*j));
      expected.bytes[6]=0x75; expected.bytes[8]=0xa5; expected.bytes[15]+=i;
      Check(ids[i]==expected,"exact timestamp and74-bit sequence preserves OS entropy");
    }
  }
}
void Diagnostics() {
  using E=u::CryptoBootstrapDiagnosticError;
  if(mode=="concurrent") {
    std::array<u::CryptoBootstrapDiagnosticIdentityResult,8> results;
    std::array<std::thread,8> threads;
    for(unsigned i=0;i<8;++i) threads[i]=std::thread([&,i]{
      results[i]=u::IssueCryptoBootstrapDiagnosticIdentityV7();
    });
    for(auto& thread:threads) thread.join();
    unsigned successes=0;
    for(unsigned i=0;i<8;++i) {
      Check(results[i].ok() || results[i].error==E::busy,"concurrent diagnostic is actual success or busy");
      if(!results[i].ok()) continue;
      ++successes;
      Check(u::IsEngineIdentityUuid(results[i].value),"concurrent diagnostic binary v7");
      for(unsigned j=0;j<i;++j) if(results[j].ok())
        Check(results[i].value!=results[j].value,"concurrent diagnostic occurrences distinct");
    }
    Check(successes>0,"diagnostic concurrency makes progress");
  } else {
    auto result=u::IssueCryptoBootstrapDiagnosticIdentityV7();
    const bool fail=mode=="unavailable" || mode=="zero" || mode=="interrupted" ||
        mode=="partial_failure" || mode=="clock_failure" || mode=="range" || mode=="fork_inflight";
    Check(fail ? result.error==E::source_failure && result.value==p::Uuid{} : result.ok(),
          "typed startup diagnostic source outcome");
    if(mode=="interrupted") Check(entropy_calls==32,"diagnostic EINTR bound");
    if(mode=="clock_failure" || mode=="range") Check(entropy_calls==0,"bad diagnostic clock precedes entropy");
    if(fail) {
      recovery=true;
      result=u::IssueCryptoBootstrapDiagnosticIdentityV7();
      Check(result.ok(),"fresh diagnostic attempt after source failure");
    }
    Check(u::IsEngineIdentityUuid(result.value),"actual diagnostic binary occurrence");
    if(mode=="regression") {
      const auto next=u::IssueCryptoBootstrapDiagnosticIdentityV7();
      Check(next.error==E::source_failure,"diagnostic issuer retains accepted clock history");
    }
    if(mode=="quota" || mode=="failure_quota") {
      auto previous=result.value;
      if(mode=="failure_quota") mode="clock_failure";
      for(unsigned i=1;i<u::kCryptoBootstrapDiagnosticAttemptLimit;++i) {
        const auto next=u::IssueCryptoBootstrapDiagnosticIdentityV7();
        if(mode=="clock_failure") Check(next.error==E::source_failure,"source failures consume attempt quota");
        else {Check(next.ok() && previous<next.value,"retained74-bit diagnostic sequence");previous=next.value;}
      }
      const auto before=entropy_calls.load();
      Check(u::IssueCryptoBootstrapDiagnosticIdentityV7().error==E::exhausted && entropy_calls==before,
            "bounded process diagnostic quota never wraps or obtains extra entropy");
    }
    if(mode=="fork") {
      const auto child=fork(); Check(child>=0,"diagnostic fork");
      if(child==0) _exit(u::IssueCryptoBootstrapDiagnosticIdentityV7().error==E::foreign_process &&
          u::SealCryptoBootstrapDiagnosticIdentities()==E::foreign_process ? 0 : 1);
      int status=0; pid_t reaped;
      do {reaped=waitpid(child,&status,0);} while(reaped<0 && errno==EINTR);
      Check(reaped==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"diagnostic fork ownership refuses");
    }
  }
  Check(rand_calls==0,"startup diagnostic never invokes ordinary RAND");
  Check(u::SealCryptoBootstrapDiagnosticIdentities()==E::none &&
        u::SealCryptoBootstrapDiagnosticIdentities()==E::none,"diagnostic seal is terminal and idempotent");
  const auto before=entropy_calls.load();
  Check(u::IssueCryptoBootstrapDiagnosticIdentityV7().error==E::sealed && entropy_calls==before,
        "sealed issuer never restarts");
  Check(!u::GenerateCompatibilityUnixTimeV7(123456).ok() && rand_calls>0 && entropy_calls==before,
        "ordinary RAND failure is not a diagnostic-source fallback");
}
int main(int argc,char** argv) {
  if(argc>2 && std::string_view(argv[1])=="diagnostic") {
    diagnostic_test=true; mode=argv[2]; Diagnostics();
    std::cout<<"PASS startup diagnostic "<<mode<<" checks="<<checks<<'\n'; return 0;
  }
  mode=argc>1?argv[1]:"layout";
  if(mode=="concurrent") {
    std::array<std::optional<u::CryptoBootstrapIdentities>,8> results;
    std::array<std::thread,8> workers;
    for(unsigned i=0;i<8;++i) workers[i]=std::thread([&,i]{results[i]=u::IssueCryptoBootstrapIdentitiesV7();});
    for(auto& worker:workers) worker.join();
    Check(std::count_if(results.begin(),results.end(),[](const auto& r){return bool(r);})==1,
          "concurrent callers publish exactly one complete batch");
    for(const auto& result:results) if(result) Layout(*result);
  } else {
    auto result=u::IssueCryptoBootstrapIdentitiesV7();
    const bool should_fail=mode!="layout" && mode!="real" && mode!="short" && mode!="fork";
    Check(bool(result)!=should_fail,"exact bootstrap result");
    if(should_fail) {
      Check(rand_calls==0,"failure did not invoke provider or error UUID recursion");
      if(mode=="clock_failure" || mode=="range") Check(entropy_calls==0,"clock refusal precedes entropy");
      if(mode=="interrupted") Check(entropy_calls==32,"bounded interruption retry");
      if(mode=="partial_failure") Check(entropy_calls==2,"partial entropy never publishes");
      if(mode=="batch_failure") Check(entropy_calls==3,"late batch failure publishes no prefix");
      recovery=true;
      result=u::IssueCryptoBootstrapIdentitiesV7();
      Check(bool(result),"failed attempt releases claim for a fresh complete batch");
    }
    Layout(*result);
  }
  Check(rand_calls==0,"bootstrap did not touch OpenSSL RAND");
  const auto before=entropy_calls.load();
  Check(!u::IssueCryptoBootstrapIdentitiesV7() && entropy_calls==before,"successful issuer cannot restart");
  if(mode=="fork") {
    const auto child=fork(); Check(child>=0,"fork for inherited one-shot restriction");
    if(child==0) _exit(u::IssueCryptoBootstrapIdentitiesV7()?1:0);
    int status=0; pid_t reaped;
    do {reaped=waitpid(child,&status,0);} while(reaped<0 && errno==EINTR);
    Check(reaped==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"fork cannot reuse successful bootstrap issuer");
  }
  Check(!u::GenerateCompatibilityUnixTimeV7(123456).ok(),"ordinary provider failure still refuses");
  Check(rand_calls>0 && entropy_calls==before,"ordinary generation never falls back to OS bootstrap");
  std::cout<<"PASS "<<mode<<" checks="<<checks<<'\n';
}
