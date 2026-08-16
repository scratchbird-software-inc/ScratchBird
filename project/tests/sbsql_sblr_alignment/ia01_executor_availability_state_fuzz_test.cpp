// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_executor_availability_registry.hpp"
#include "uuid.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace api=scratchbird::engine::internal_api;
namespace uuid=scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
constexpr std::uint64_t kSeed=0x4558454346555a5aULL; // EXECFUZZ
[[noreturn]] void Fail(const char*m){std::cerr<<"seed=0x"<<std::hex<<kSeed<<std::dec<<' '<<m<<'\n';std::exit(EXIT_FAILURE);}
void Require(bool v,const char*m){if(!v)Fail(m);}
std::uint64_t Next(std::uint64_t*s){auto x=*s;x^=x<<13;x^=x>>7;x^=x<<17;return *s=x;}
int main(){
  const auto id=uuid::GenerateEngineIdentityV7(UuidKind::database,1786831000777ull);Require(id.ok(),"database UUID failed");
  const auto base=(std::filesystem::temp_directory_path()/"sb_executor_availability_state_fuzz_53424c52").string();
  const auto store=base+".sb.sblr_executor_availability_registry.v1";std::error_code ignored;std::filesystem::remove(store,ignored);
  api::EngineRequestContext context;context.database_path=base;context.database_uuid.canonical=uuid::UuidToString(id.value.value);context.security_context_present=true;context.trace_tags.push_back("right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
  auto loaded=api::LoadSblrExecutorAvailabilitySnapshot(context);Require(loaded.ok&&loaded.snapshot.generation==1,"bootstrap failed");auto current=loaded.snapshot;std::uint64_t state=kSeed;
  for(unsigned step=0;step!=128;++step){
    api::SblrExecutorAvailabilitySetRequest request;request.database_uuid=context.database_uuid.canonical;request.expected_snapshot_uuid=current.snapshot_uuid;request.expected_generation=current.generation;
    switch(Next(&state)%3){case 0:request.requested_state=api::SblrExecutorAvailabilityState::installed;break;case 1:request.requested_state=api::SblrExecutorAvailabilityState::revoked;break;default:request.requested_state=api::SblrExecutorAvailabilityState::unavailable;}
    request.reason_code="fuzz.step."+std::to_string(step);const auto prior=current;auto changed=api::SetSblrExecutorAvailability(context,request);Require(changed.ok&&changed.snapshot.generation==prior.generation+1,"valid transition failed");current=changed.snapshot;
    auto stale=request;stale.reason_code="fuzz.stale."+std::to_string(step);Require(!api::SetSblrExecutorAvailability(context,stale).ok,"stale CAS admitted");
    api::SblrExecutorAvailabilitySnapshot observed;const auto diagnostic=api::RevalidateSblrExecutorAvailability(context,prior,&observed);Require(observed.generation==current.generation,"revalidation did not observe current generation");
    if(current.availability_state==api::SblrExecutorAvailabilityState::revoked)Require(diagnostic.code=="SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","revoked precedence drifted");
    else if(current.availability_state==api::SblrExecutorAvailabilityState::unavailable)Require(diagnostic.code=="SBLR.OPCODE.EXECUTOR_UNAVAILABLE","unavailable precedence drifted");
    else Require(diagnostic.code=="SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE","installed stale precedence drifted");
    if(step%16==15){auto restart=api::LoadSblrExecutorAvailabilitySnapshot(context);Require(restart.ok&&restart.snapshot.snapshot_uuid==current.snapshot_uuid&&restart.snapshot.generation==current.generation,"restart state drifted");}
  }
  std::filesystem::remove(store,ignored);std::cout<<"seed=0x"<<std::hex<<kSeed<<std::dec<<" transitions=128 restart_interval=16\n";return EXIT_SUCCESS;
}
