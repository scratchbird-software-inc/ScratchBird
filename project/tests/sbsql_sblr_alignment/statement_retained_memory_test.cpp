// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "engine/statement_retained_memory.hpp"
#include "engine/statement_blob_input.hpp"
#include "engine/statement_blob_component.hpp"
#include "engine/statement_blob_encoded_input.hpp"
#include "engine/statement_blob_hash.hpp"
#include "engine/statement_blob_row_value.hpp"
#include "../support/blob_persistence_fixture.hpp"
#include "engine/statement_cancellation.hpp"
#include "datatype_operations.hpp"
#include "engine/statement_scratch_memory.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_snapshot.hpp"
#include "engine/internal_api/dml/update_resource_authority_provider.hpp"
#include <atomic>
#include <algorithm>
#include <barrier>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <thread>
#include <cstdlib>
#if defined(__linux__)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
namespace {
thread_local unsigned protected_lock_calls=0,protected_no_dump_calls=0,protected_zeroized_unlocks=0;
thread_local bool refuse_protected_lock=false,refuse_protected_no_dump=false;
thread_local const void* watched_protected_pointer=nullptr;
thread_local std::size_t watched_protected_bytes=0;
thread_local bool watched_protected_zero=true;
}
extern "C" int __real_mlock(const void*,size_t);
extern "C" int __wrap_mlock(const void* pointer,size_t bytes){
  ++protected_lock_calls;if(refuse_protected_lock){refuse_protected_lock=false;errno=EPERM;return -1;}
  return __real_mlock(pointer,bytes);
}
extern "C" int __real_madvise(void*,size_t,int);
extern "C" int __wrap_madvise(void* pointer,size_t bytes,int advice){
  if(advice==MADV_DONTDUMP){++protected_no_dump_calls;
    if(refuse_protected_no_dump){refuse_protected_no_dump=false;errno=EPERM;return -1;}}
  return __real_madvise(pointer,bytes,advice);
}
extern "C" int __real_munlock(const void*,size_t);
extern "C" int __wrap_munlock(const void* pointer,size_t bytes){
  if(pointer==watched_protected_pointer){++protected_zeroized_unlocks;
    watched_protected_zero=watched_protected_zero&&bytes==watched_protected_bytes&&
        std::all_of(static_cast<const unsigned char*>(pointer),static_cast<const unsigned char*>(pointer)+bytes,[](auto b){return b==0;});}
  return __real_munlock(pointer,bytes);
}
#endif

thread_local long allocation_budget = -1;
thread_local unsigned allocation_count = 0;
thread_local bool allocation_fault_triggered = false;
void* operator new(std::size_t n) {
  ++allocation_count;
  if (allocation_budget >= 0 && allocation_budget-- == 0) {
    allocation_fault_triggered = true;
    throw std::bad_alloc();
  }
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#include "../support/aligned_allocation_fault_bridge.hpp"

namespace api = scratchbird::engine::internal_api;
namespace tests = scratchbird::tests;
namespace mm = scratchbird::core::memory;
namespace db = scratchbird::storage::database;
namespace bridge = scratchbird::server_engine_bridge;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
using Owner = std::unique_ptr<api::RetainedStatementMemoryV1>;
using Observation = api::RetainedStatementMemoryObservationV1;
std::atomic<unsigned> checks{0};
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }

struct Statement {
  bridge::StatementContextReceiptHandle handle;
  api::EngineRequestContext context;
  Statement(const tests::FixtureEngineSession& session, const api::EngineRequestContext& base) {
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &base; request.exact_transaction_uuid = base.transaction_uuid;
    bridge::StatementContextReceiptView view;
    tests::FixtureEngineSession::Check(bridge::AcquireStatementContextReceipt(
        session.get(), &request, &handle, &view, nullptr), nullptr, "retained memory statement");
    try {
      tests::FixtureEngineSession::Check(bridge::CopyStatementContextEngineContextV1(handle, &context, nullptr),
          nullptr, "retained memory issued context");
    } catch (...) { Release(); throw; }
  }
  void Release() {
    if (handle) {
      const auto result = bridge::ReleaseStatementContextReceipt(handle);
      tests::FixtureEngineSession::Check(result, nullptr, "retained memory statement release");
      handle = {};
    }
  }
  ~Statement() { Release(); }
  sb_engine_status_t Retain(std::uint64_t n, Owner& owner) {
    return api::RetainStatementMemoryForResultV1(handle, context.session_uuid.bytes,
        context.statement_receipt_uuid.bytes, n, &owner);
  }
  sb_engine_status_t RetainProtected(std::uint64_t n, mm::ProtectedMemoryPlatformPolicy protection, Owner& owner) {
    return api::RetainStatementMemoryForProtectedResultV1(handle, context.session_uuid.bytes,
        context.statement_receipt_uuid.bytes, n, protection, &owner);
  }
};
void AllocationRefuses(api::RetainedStatementMemoryV1& owner, std::size_t n) {
  const auto before = owner.Snapshot();
  bool refused = false;
  try { (void)owner.memory()->allocate(n, alignof(std::max_align_t)); }
  catch (const std::bad_alloc&) { refused = true; }
  Check(refused && owner.Snapshot().allocated_bytes == before.allocated_bytes,
        "refused payload allocation changed owned bytes");
}

void ProtectedRetainedPayload(tests::FixtureEngineSession& session,const api::EngineRequestContext& context){
  using P=mm::ProtectedMemoryPlatformPolicy;
  Statement statement(session,context);const auto baseline=mm::DefaultMemoryManager().Snapshot();
  Owner owner;
  Check(statement.RetainProtected(4096,static_cast<P>(255),owner)==SB_ENGINE_STATUS_INVALID_ARGUMENT&&!owner,
      "invalid protected-result requirement must refuse before grant acquisition");
  Check(statement.RetainProtected(4096,P::require_lock_and_no_dump,owner)==SB_ENGINE_STATUS_OK&&owner&&
      owner->protection_policy()==P::require_lock_and_no_dump&&owner->Observe()==Observation::live,
      "private actual receipt issues an explicitly protected retained output owner");
  auto* unchanged=owner.get();Check(statement.RetainProtected(4096,P::best_effort,owner)==SB_ENGINE_STATUS_INVALID_ARGUMENT&&owner.get()==unchanged,
      "protected acquisition never replaces a populated owner or its selected requirement");
#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
  for(unsigned failure=0;failure!=2;++failure){
    if(!failure)refuse_protected_lock=true;else refuse_protected_no_dump=true;
    AllocationRefuses(*owner,64);
    Check(!refuse_protected_lock&&!refuse_protected_no_dump&&owner->Snapshot().allocated_bytes==0,
        "required platform protection failure has no ordinary-allocation fallback or retained charge");
    const auto observed=owner->AllocationSnapshot();
    Check(observed.last_failure.diagnostic_code=="SB-MEMORY-PROTECTED-PLATFORM-PROTECTION-REQUIRED"&&
        observed.last_failure.message_key=="memory.protected.platform_protection_required"&&
        observed.last_failure.status.code==platform::StatusCode::memory_invalid_request,
        "protected retained telemetry keeps native cause independently of the allocation exception");
  }
  const auto locks=protected_lock_calls,no_dump=protected_no_dump_calls,zeroes=protected_zeroized_unlocks;
#endif
  auto* payload=owner->memory()->allocate(4096,alignof(std::max_align_t));std::memset(payload,0xa5,4096);
  Check(owner->Snapshot().allocated_bytes==4096&&owner->AllocationSnapshot().allocated_bytes==4096&&
      owner->Release()==SB_ENGINE_STATUS_CONFLICT,"protected owner consumes exact actual grant credit and retains live payload");
#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
  const auto page=::sysconf(_SC_PAGESIZE);Check(page>0&&reinterpret_cast<std::uintptr_t>(payload)%page==0&&
      protected_lock_calls>locks&&protected_no_dump_calls>no_dump,"protected result uses actual independent OS-page backing and both native protections");
  watched_protected_pointer=payload;watched_protected_bytes=4096;watched_protected_zero=true;
#endif
  const auto original=statement.handle;const auto binding=owner->binding();statement.Release();
  Check(owner->Observe()==Observation::live&&std::all_of(static_cast<const unsigned char*>(payload),static_cast<const unsigned char*>(payload)+4096,
      [](auto n){return n==0xa5;}),"ordinary receipt retirement does not reclaim protected result bytes");
  Owner late;Check(api::RetainStatementMemoryForProtectedResultV1(original,binding.session,binding.receipt,4096,P::require_lock_and_no_dump,&late)==
      SB_ENGINE_STATUS_INVALID_HANDLE&&!late,"retired public receipt cannot issue another protected owner");
  owner->memory()->deallocate(payload,4096,alignof(std::max_align_t));
#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
  Check(protected_zeroized_unlocks==zeroes+1&&watched_protected_zero,"actual protected result bytes are erased before native unlock and release");
  watched_protected_pointer=nullptr;watched_protected_bytes=0;
#endif
  Check(owner->Release()==SB_ENGINE_STATUS_OK&&owner->Release()==SB_ENGINE_STATUS_OK,"protected result releases capacity and retained authorities after erasure");
  AllocationRefuses(*owner,1);owner.reset();
  const auto after=mm::DefaultMemoryManager().Snapshot();
  Check(after.current_bytes==baseline.current_bytes&&after.reserved_capacity_bytes==baseline.reserved_capacity_bytes&&
      after.active_capacity_reservation_count==baseline.active_capacity_reservation_count,"protected result retirement has no physical-byte or capacity leak");

  Statement revoked(session,context);Owner retained;
  Check(revoked.RetainProtected(128,P::require_lock_and_no_dump,retained)==SB_ENGINE_STATUS_OK,"actual protected revocation fixture");
  auto* data=retained->memory()->allocate(128,alignof(std::max_align_t));std::memset(data,0x5a,128);
  mga::RevokePublishedSnapshotVector({platform::UuidKind::object,platform::Uuid{retained->binding().snapshot}});
  Check(retained->Observe()==Observation::snapshot_revoked,"actual MGA revocation reaches protected result admission");
  AllocationRefuses(*retained,1);Check(static_cast<unsigned char*>(data)[127]==0x5a,"revocation preserves already owned protected bytes for cleanup");
  retained->memory()->deallocate(data,128,alignof(std::max_align_t));Check(retained->Release()==SB_ENGINE_STATUS_OK,"revoked protected owner can still erase and release");
}

void ProtectedScratchFailureDiagnostics(tests::FixtureEngineSession& session,const api::EngineRequestContext& context){
#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
  Statement statement(session,context);
  for(unsigned failure=0;failure!=2;++failure){
    const auto before=mm::DefaultMemoryManager().Snapshot();
    platform::DiagnosticRecord diagnostic;
    const auto status=api::RunWithStatementScratchMemoryV1(statement.context,[&](auto& scratch){
      if(!failure)refuse_protected_lock=true;else refuse_protected_no_dump=true;
      (void)scratch.protected_memory()->allocate(137,256);
    },&diagnostic);
    const auto argument=[&](std::string_view key)->std::string_view{
      for(const auto& field:diagnostic.arguments)if(field.key==key)if(const auto* text=field.text())return *text;
      return {};
    };
    Check(status==SB_ENGINE_STATUS_RESOURCE_EXHAUSTED&&!refuse_protected_lock&&!refuse_protected_no_dump&&
        diagnostic.diagnostic_code=="SB-MEMORY-PROTECTED-PLATFORM-PROTECTION-REQUIRED"&&
        diagnostic.message_key=="memory.protected.platform_protection_required"&&
        diagnostic.status.code==platform::StatusCode::memory_invalid_request&&
        argument("platform_policy")=="require_lock_and_no_dump"&&
        argument("required_lock_failed")==(!failure?"true":"false")&&
        argument("required_no_dump_failed")==(!failure?"false":"true"),
        "escaped native protected scratch failure preserves its exact typed cause and both protection outcomes");
    const auto after=mm::DefaultMemoryManager().Snapshot();
    Check(after.current_bytes==before.current_bytes&&after.reserved_capacity_bytes==before.reserved_capacity_bytes&&
        after.active_capacity_reservation_count==before.active_capacity_reservation_count&&
        after.active_allocation_count==before.active_allocation_count,
        "escaped protected scratch failure releases all actual backing and capacity after cause transfer");
  }
  platform::DiagnosticRecord unrelated;
  const auto status=api::RunWithStatementScratchMemoryV1(statement.context,[&](auto& scratch){
    refuse_protected_lock=true;
    try{(void)scratch.protected_memory()->allocate(137,256);}
    catch(const std::bad_alloc&){}
    throw std::bad_alloc();
  },&unrelated);
  Check(status==SB_ENGINE_STATUS_RESOURCE_EXHAUSTED&&!refuse_protected_lock&&unrelated.diagnostic_code.empty(),
      "a handled protected failure is not misreported as the cause of a later unrelated exception");
#endif
}

void ProtectedDeferredCleanup(tests::FixtureEngineSession& session,const api::EngineRequestContext& context){
  using P=mm::ProtectedMemoryPlatformPolicy;
  Statement statement(session,context);
  auto& manager=mm::DefaultMemoryManager();
  for(bool throwing:{false,true}){
    const auto baseline=manager.Snapshot();
    Owner deferred;
    Check(statement.RetainProtected(4096,P::require_lock_and_no_dump,deferred)==SB_ENGINE_STATUS_OK,
        "actual protected deferred cleanup grant");
    auto* payload=deferred->memory()->allocate(137,alignof(std::max_align_t));
    std::memset(payload,0x6b,137);
#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
    const auto zeroes=protected_zeroized_unlocks;
    watched_protected_pointer=payload;watched_protected_bytes=137;watched_protected_zero=true;
#endif
    const auto status=api::RunWithStatementScratchMemoryV1(statement.context,[&](auto&){
      Owner recursive;
      Check(statement.RetainProtected(4096,P::require_lock_and_no_dump,recursive)==SB_ENGINE_STATUS_CONFLICT&&!recursive,
          "protected retained acquisition refuses synchronous receipt reentry");
      const auto before=manager.Snapshot();
      deferred.reset();
      const auto held=manager.Snapshot();
      Check(held.current_bytes==before.current_bytes&&held.reserved_capacity_bytes==before.reserved_capacity_bytes&&
          held.active_capacity_reservation_count==before.active_capacity_reservation_count&&
          std::all_of(static_cast<const unsigned char*>(payload),static_cast<const unsigned char*>(payload)+137,
              [](auto n){return n==0x6b;}),
          "protected retained destructor keeps live bytes and grant until receipt guards unwind");
#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
      Check(protected_zeroized_unlocks==zeroes,"protected retained memory was unlocked inside receipt guards");
#endif
      if(throwing)throw std::runtime_error("protected retained deferred exception fixture");
    });
    Check(status==(throwing?SB_ENGINE_STATUS_INTERNAL_ERROR:SB_ENGINE_STATUS_OK),
        "actual normal or exceptional callback unwind completes protected deferred cleanup");
#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
    Check(protected_zeroized_unlocks==zeroes+1&&watched_protected_zero,
        "deferred protected result is erased before actual native unlock exactly once");
    watched_protected_pointer=nullptr;watched_protected_bytes=0;
#endif
    const auto after=manager.Snapshot();
    Check(after.current_bytes==baseline.current_bytes&&after.reserved_capacity_bytes==baseline.reserved_capacity_bytes&&
        after.active_capacity_reservation_count==baseline.active_capacity_reservation_count&&
        after.active_allocation_count==baseline.active_allocation_count,
        "protected deferred cleanup restores exact actual bytes, allocations and credits");
  }
}

void RetainedAdmissionAllocationFailures(tests::FixtureEngineSession&,const api::EngineRequestContext&,bool);
void ProtectedAdmissionAllocationFailures(tests::FixtureEngineSession& session,const api::EngineRequestContext& context){
  RetainedAdmissionAllocationFailures(session,context,true);
}

void IdentityAndRetirement(tests::FixtureEngineSession& session, const api::EngineRequestContext& context) {
  Statement statement(session, context);
  const auto baseline = mm::DefaultMemoryManager().Snapshot();
  Owner owner;
  Check(statement.Retain(0, owner) == SB_ENGINE_STATUS_INVALID_ARGUMENT && !owner, "zero allowance admitted");
  const auto policy = mm::DefaultMemoryManager().policy();
  const auto ceiling = std::min(policy.hard_limit_bytes, policy.per_context_limit_bytes);
  Check(ceiling < UINT64_MAX && statement.Retain(ceiling+1, owner) == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED && !owner,
        "over-policy grant admitted");
  for (std::size_t i=0; i<16; ++i) {
    auto foreign = context.session_uuid.bytes; foreign[i] ^= 1;
    Check(api::RetainStatementMemoryForResultV1(statement.handle, foreign, statement.context.statement_receipt_uuid.bytes,
        4096, &owner) == SB_ENGINE_STATUS_SECURITY_DENIED && !owner, "foreign binary owner admitted");
    foreign = statement.context.statement_receipt_uuid.bytes; foreign[i] ^= 1;
    Check(api::RetainStatementMemoryForResultV1(statement.handle, statement.context.session_uuid.bytes, foreign,
        4096, &owner) == SB_ENGINE_STATUS_INVALID_HANDLE && !owner, "foreign binary receipt admitted");
  }
  Check(statement.Retain(4096, owner) == SB_ENGINE_STATUS_OK && owner, "actual retained grant refused");
  const auto& b = owner->binding();
  Check(b.database == context.database_uuid.bytes && b.session == context.session_uuid.bytes &&
      b.transaction == context.transaction_uuid.bytes && b.statement == statement.context.statement_uuid.bytes &&
      b.receipt == statement.context.statement_receipt_uuid.bytes &&
      b.snapshot == statement.context.statement_snapshot_uuid.bytes && b.snapshot_generation != 0 &&
      b.security_generation != 0 && b.security_context == statement.context.authorization_context.authority_uuid.bytes,
      "actual binary binding changed or absent");
  Check(b.datatype_catalog_snapshot == statement.context.datatype_catalog_snapshot_uuid.bytes &&
      b.datatype_catalog_generation == statement.context.datatype_catalog_generation &&
      b.datatype_registry_generation == statement.context.datatype_registry_generation &&
      b.datatype_catalog_generation != 0 && b.datatype_registry_generation != 0,
      "retained owner lost the actual persisted datatype cohort");
  // Mutating an observation cannot rewrite the private retained binding.
  statement.context.datatype_catalog_snapshot_uuid.bytes[0] ^= 1;
  ++statement.context.datatype_catalog_generation;
  ++statement.context.datatype_registry_generation;
  Owner cohort_owner;
  Check(statement.Retain(4096, cohort_owner) == SB_ENGINE_STATUS_OK && cohort_owner &&
      cohort_owner->binding().datatype_catalog_snapshot == b.datatype_catalog_snapshot &&
      cohort_owner->binding().datatype_catalog_generation == b.datatype_catalog_generation &&
      cohort_owner->binding().datatype_registry_generation == b.datatype_registry_generation,
      "caller datatype observations became retained cohort authority");
  Check(cohort_owner->Release() == SB_ENGINE_STATUS_OK, "cohort observation owner cleanup");
  cohort_owner.reset();
  Check(owner->Observe() == Observation::live && owner->Snapshot().active &&
        owner->Snapshot().reserved_bytes == 4096 && owner->Snapshot().binary_operation_uuid == b.statement,
        "retained owner not backed by actual live reservation");
  allocation_count = 0; allocation_budget = 0;
  const auto observed = owner->Observe();
  const auto observation_allocations = allocation_count;
  allocation_budget = -1;
  Check(observed == Observation::live && observation_allocations == 0,
        "live retained MGA/receipt observation allocated or copied snapshot vectors");
  auto* unchanged = owner.get();
  Check(statement.Retain(4096, owner) == SB_ENGINE_STATUS_INVALID_ARGUMENT && owner.get() == unchanged,
        "populated output overwritten");
  AllocationRefuses(*owner, 4097);
  Check(owner->AllocationSnapshot().failed_allocation_count == 1 &&
        !owner->AllocationSnapshot().last_failure.diagnostic_code.empty(),
        "actual allocator failure diagnostic was lost");
  Owner over_capacity;
  platform::DiagnosticRecord capacity_diagnostic;
  Check(api::RetainStatementMemoryForResultV1(statement.handle, b.session, b.receipt, ceiling,
        &over_capacity, &capacity_diagnostic) == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED && !over_capacity &&
        !capacity_diagnostic.diagnostic_code.empty() && owner->Observe() == Observation::live,
        "actual shared-budget refusal changed existing grant or lost diagnostic");
  void* payload = owner->memory()->allocate(4096, alignof(std::max_align_t));
  std::memset(payload, 0xa5, 4096);
  Check(owner->Snapshot().allocated_bytes == 4096 && owner->Release() == SB_ENGINE_STATUS_CONFLICT &&
        owner->Observe() == Observation::live, "release reclaimed live payload");
  const auto original = statement.handle;
  const auto receipt_id = statement.context.statement_receipt_uuid.bytes;
  const auto actual_resources = statement.context.dml_update_resource_receipt.lock();
  Check(actual_resources && !actual_resources->IsRevoked(), "actual private statement resource receipt missing");
  statement.Release();
  Check(owner->Observe() == Observation::live, "ordinary receipt retirement revoked retained grant");
  Owner late;
  Check(api::RetainStatementMemoryForResultV1(original, b.session, receipt_id, 4096, &late) ==
        SB_ENGINE_STATUS_INVALID_HANDLE && !late, "retired receipt minted a new grant");
  Check(std::all_of(static_cast<unsigned char*>(payload), static_cast<unsigned char*>(payload)+4096,
      [](auto n){return n==0xa5;}), "retirement corrupted retained payload");
  owner->memory()->deallocate(payload, 4096, alignof(std::max_align_t));
  // A retired public receipt prevents acquisition, but does not prevent an
  // existing independently retained owner using its original live grant.
  payload = owner->memory()->allocate(4096, alignof(std::max_align_t));
  owner->memory()->deallocate(payload, 4096, alignof(std::max_align_t));
  Check(owner->Release() == SB_ENGINE_STATUS_OK && owner->Release() == SB_ENGINE_STATUS_OK &&
      owner->Observe() == Observation::released && !owner->Snapshot().active, "retained grant release failed");
  AllocationRefuses(*owner, 1);
  owner.reset();
  Check(actual_resources->IsRevoked(), "failed/successful retained acquisition stranded a private owner");
  const auto after = mm::DefaultMemoryManager().Snapshot();
  Check(after.current_bytes == baseline.current_bytes && after.reserved_capacity_bytes == baseline.reserved_capacity_bytes &&
        after.active_capacity_reservation_count == baseline.active_capacity_reservation_count,
        "retained grant leaked physical allocation or capacity");
}

void FaultAndReentry(tests::FixtureEngineSession& session, const api::EngineRequestContext& context) {
  Statement statement(session, context);
  Owner owner;
  Check(statement.Retain(4096, owner) == SB_ENGINE_STATUS_OK, "fault grant acquisition");
  mm::MemoryFailureInjectionConfiguration fault;
  fault.test_guard = mm::MakeMemoryFailureInjectionTestGuard(); fault.fixture_enabled = true;
  fault.fixture_name = "statement_retained_memory"; fault.evidence_note = "real retained payload allocator failure";
  mm::MemoryFailureInjectionRule rule; rule.rule_id = "first_retained_payload_allocation";
  rule.scope_kind = mm::MemoryFailureInjectionScopeKind::context;
  rule.binary_scope_uuid = statement.context.statement_receipt_uuid.bytes;
  fault.rules.push_back(rule);
  auto& manager = mm::DefaultMemoryManager();
  Check(manager.EnableAllocationFailureInjection(fault).ok(), "physical fault enable");
  AllocationRefuses(*owner, 128);
  Check(manager.DisableAllocationFailureInjection().ok(), "physical fault disable");
  auto* p = owner->memory()->allocate(128, alignof(std::max_align_t));
  owner->memory()->deallocate(p,128,alignof(std::max_align_t));
  auto* held = owner->memory()->allocate(128, alignof(std::max_align_t));
  Owner deferred;
  Check(statement.Retain(4096, deferred) == SB_ENGINE_STATUS_OK, "second actual retained grant");
  Check(owner->Observe() == Observation::live && deferred->Observe() == Observation::live,
        "second grant invalidated first reservation");
  Owner recursive;
  const auto before_reentry = manager.Snapshot();
  Check(api::RunWithStatementScratchMemoryV1(statement.context, [&](auto& scratch) {
    Check(scratch.memory() && scratch.protected_memory() &&
          scratch.memory() != scratch.protected_memory(), "protected scratch adapter absent or ordinary alias");
    const auto baseline = scratch.Snapshot();
#ifdef SB_RETAINED_PROTECTED_MEMORY_WRAP
    for(unsigned failure=0;failure!=2;++failure){
      if(!failure)refuse_protected_lock=true;else refuse_protected_no_dump=true;
      bool refused=false;
      try{(void)scratch.protected_memory()->allocate(64,alignof(std::max_align_t));}
      catch(const std::bad_alloc&){refused=true;}
      Check(refused&&!refuse_protected_lock&&!refuse_protected_no_dump&&
          scratch.Snapshot().allocated_bytes==baseline.allocated_bytes&&
          scratch.Snapshot().allocation_count==baseline.allocation_count,
          "native protected scratch requires both OS protections without ordinary fallback or charge leak");
    }
#endif
    auto* secret = scratch.protected_memory()->allocate(137, 256);
    Check(std::all_of(static_cast<unsigned char*>(secret), static_cast<unsigned char*>(secret)+137,
        [](auto byte){ return byte == 0; }), "protected statement scratch was not zeroed");
    std::memset(secret, 0xa5, 137);
    auto* ordinary = scratch.memory()->allocate(79, 64);
    Check(scratch.Snapshot().allocated_bytes == baseline.allocated_bytes+216 &&
          scratch.Snapshot().allocation_count == baseline.allocation_count+2,
          "protected statement allocation bypassed the shared physical grant");
    bool over_limit = false;
    try { (void)scratch.protected_memory()->allocate(scratch.limit_bytes()+1); }
    catch (const std::bad_alloc&) { over_limit = true; }
    Check(over_limit && scratch.Snapshot().allocated_bytes == baseline.allocated_bytes+216,
          "protected scratch escaped its exact shared capacity");
    scratch.protected_memory()->deallocate(secret, 137, 256);
    scratch.memory()->deallocate(ordinary, 79, 64);
    Check(scratch.Snapshot().allocated_bytes == baseline.allocated_bytes &&
          scratch.Snapshot().allocation_count == baseline.allocation_count,
          "protected statement scratch failed actual physical cleanup");
    Check(statement.Retain(4096, recursive) == SB_ENGINE_STATUS_CONFLICT && !recursive,
          "retained acquisition recursively acquired live scratch lock");
    Check(owner->Observe() == Observation::unavailable && owner->Release() == SB_ENGINE_STATUS_CONFLICT,
          "existing owner entered synchronous receipt lock");
    bool refused = false;
    try { (void)owner->Snapshot(); } catch (const std::bad_alloc&) { refused = true; }
    Check(refused, "existing owner snapshot entered inverse lock order");
    refused = false;
    try { (void)owner->memory()->allocate(1); } catch (const std::bad_alloc&) { refused = true; }
    Check(refused, "existing PMR allocation entered recursive receipt lock");
    refused = false;
    try { owner->memory()->deallocate(held,128,alignof(std::max_align_t)); }
    catch (const std::bad_alloc&) { refused = true; }
    Check(refused, "existing PMR deallocation entered inverse lock order");
    const auto before = manager.Snapshot().reserved_capacity_bytes;
    deferred.reset(); // Must retain custody until this scope's locks unwind.
    Check(manager.Snapshot().reserved_capacity_bytes == before,
          "destructor released retained authority under synchronous receipt locks");
  }) == SB_ENGINE_STATUS_OK, "scratch reentry fixture refused");
  const auto after_reentry = manager.Snapshot();
  Check(after_reentry.reserved_capacity_bytes + 4096 == before_reentry.reserved_capacity_bytes &&
        after_reentry.active_capacity_reservation_count + 1 == before_reentry.active_capacity_reservation_count,
        "normal unwind left deferred physical capacity queued");
  Check(owner->Snapshot().allocated_bytes == 128 && owner->Observe() == Observation::live,
        "refused reentry damaged existing allocation");
  owner->memory()->deallocate(held,128,alignof(std::max_align_t));
  Check(owner->Release() == SB_ENGINE_STATUS_OK, "fault owner cleanup");
  Owner exceptional;
  Check(statement.Retain(4096, exceptional) == SB_ENGINE_STATUS_OK, "exception unwind grant");
  const auto before_exception = manager.Snapshot();
  Check(api::RunWithStatementScratchMemoryV1(statement.context, [&](auto& scratch) {
    auto* secret = scratch.protected_memory()->allocate(137, 256);
    std::memset(secret, 0x5a, 137);
    // Deliberately leave this owned block to the actual protected resource:
    // exception unwind must erase/free it before dropping the receipt locks.
    exceptional.reset();
    throw std::runtime_error("owned callback exception fixture");
  }) == SB_ENGINE_STATUS_INTERNAL_ERROR, "callback exception was not contained");
  const auto after_exception = manager.Snapshot();
  Check(after_exception.reserved_capacity_bytes + 4096 == before_exception.reserved_capacity_bytes &&
        after_exception.active_capacity_reservation_count + 1 == before_exception.active_capacity_reservation_count &&
        after_exception.current_bytes == before_exception.current_bytes &&
        after_exception.active_allocation_count == before_exception.active_allocation_count,
        "exception unwind left deferred physical capacity queued");
}

void RetainedAdmissionAllocationFailures(tests::FixtureEngineSession& session,
    const api::EngineRequestContext& context,bool protected_result) {
  using P=mm::ProtectedMemoryPlatformPolicy;
  const auto retain=[&](Statement& statement,Owner& owner) {
    return protected_result?statement.RetainProtected(4096,P::require_lock_and_no_dump,owner):statement.Retain(4096,owner);
  };
  const auto release_owner=[&](Owner& owner) {
    Check(owner && owner->Observe()==Observation::live &&
          (protected_result?owner->protection_policy()==P::require_lock_and_no_dump:!owner->protection_policy()),
          "successful actual admission preserves exact selected ordinary/protected owner and live authority");
    const auto snapshot=owner->Snapshot();
    Check(snapshot.active && snapshot.reserved_bytes==4096 && snapshot.allocated_bytes==0 &&
          snapshot.allocation_count==0 && owner->binding().memory_token_shard_count>0,
          "actual admitted owner has exact quota, observed issuing ledger and no provisional payload");
    Check(owner->Release()==SB_ENGINE_STATUS_OK,"measured/faulted actual owner releases its grant");
    owner.reset();
  };
  const auto release_statement=[](Statement& statement) {
    const auto resources=statement.context.dml_update_resource_receipt.lock();
    statement.Release();
    Check(resources && resources->IsRevoked(),"allocation sweep leaves no retained statement authority after actual receipt retirement");
  };
  const auto fault=[&](Statement& statement,unsigned position,unsigned sites,bool cold) {
    Owner owner;
    const auto before=mm::DefaultMemoryManager().Snapshot();
    allocation_fault_triggered=false;allocation_budget=position;
    const auto status=retain(statement,owner);
    allocation_budget=-1;
    if(!allocation_fault_triggered)
      std::cerr<<"unconsumed retained admission fault: profile="<<(protected_result?"protected":"ordinary")
               <<" state="<<(cold?"cold":"warm")<<" index="<<position<<" measured_sites="<<sites
               <<" status="<<static_cast<unsigned>(status)<<'\n';
    Check(allocation_fault_triggered,"each measured actual cold/warm admission allocation fault is consumed");
    if(status==SB_ENGINE_STATUS_OK)release_owner(owner);
    else Check(!owner && status==SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
        "failed actual admission publishes no partial ordinary/protected owner or wrong outcome");
    const auto after=mm::DefaultMemoryManager().Snapshot();
    Check(after.current_bytes==before.current_bytes && after.reserved_capacity_bytes==before.reserved_capacity_bytes &&
          after.active_capacity_reservation_count==before.active_capacity_reservation_count &&
          after.active_allocation_count==before.active_allocation_count,
          "each actual admission fault restores all physical bytes, allocations and reserved credits");
  };
  const auto terminal=[&](Statement& statement,unsigned sites) {
    Owner owner;allocation_fault_triggered=false;allocation_budget=sites;
    const auto status=retain(statement,owner);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(status==SB_ENGINE_STATUS_OK && !allocation_fault_triggered && remaining==0,
          "complete actual allocation inventory reaches exact unfaulted terminal admission");
    release_owner(owner);
  };
  // Every cold attempt gets a NEW issued receipt/ledger outside the injected
  // window. Reusing a ledger would consume token IDs and lazily initialize
  // different shard buckets, changing the measured path during the sweep.
  unsigned cold_sites=0;
  {
    Statement statement(session,context);Owner owner;
    allocation_count=0;
    const auto status=retain(statement,owner);
    cold_sites=allocation_count;
    Check(status==SB_ENGINE_STATUS_OK,"measure actual cold admission including private ledger initialization");
    release_owner(owner);release_statement(statement);
  }
  Check(cold_sites>0 && cold_sites<4096,"bounded complete actual cold admission allocation inventory");
  for(unsigned position=0;position<cold_sites;++position) {
    Statement statement(session,context);
    fault(statement,position,cold_sites,true);
    release_statement(statement);
  }
  {
    Statement statement(session,context);
    terminal(statement,cold_sites);release_statement(statement);
  }
  Statement warm(session,context);Owner owner;
  Check(retain(warm,owner)==SB_ENGINE_STATUS_OK,"actual warm admission discovers issuing ledger sharding");
  const auto shards=owner->binding().memory_token_shard_count;
  Check(shards>0 && shards<4096,"bounded observation of actual issuing token ledger sharding");
  release_owner(owner);
  // Successful consecutive admissions traverse every ACTUAL token shard.
  // The count comes from the retained issuer, not a separate default ledger
  // or a guessed number of warmups; cold first-use sites remain tested above.
  for(std::uint64_t shard=0;shard<shards;++shard) {
    Check(retain(warm,owner)==SB_ENGINE_STATUS_OK && owner->binding().memory_token_shard_count==shards,
          "successful actual admissions warm all observed token shards without replacing the ledger");
    release_owner(owner);
  }
  allocation_count=0;
  const auto status=retain(warm,owner);
  const auto warm_sites=allocation_count;
  Check(status==SB_ENGINE_STATUS_OK,"measure actual fully warmed admission");
  release_owner(owner);
  Check(warm_sites>0 && warm_sites<4096,"bounded complete actual warm admission allocation inventory");
  for(unsigned position=0;position<warm_sites;++position)fault(warm,position,warm_sites,false);
  terminal(warm,warm_sites);
  release_statement(warm);
  std::cout<<"actual "<<(protected_result?"protected ":"")<<"retained admission fault positions cold="<<cold_sites
           <<" warm="<<warm_sites<<'\n';
}
void AdmissionAllocationFailures(tests::FixtureEngineSession& session,const api::EngineRequestContext& context) {
  RetainedAdmissionAllocationFailures(session,context,false);
}

void SnapshotRevocation(tests::FixtureEngineSession& session, const api::EngineRequestContext& context) {
  Statement statement(session,context); Owner owner;
  Check(statement.Retain(4096,owner) == SB_ENGINE_STATUS_OK,"snapshot revocation grant");
  auto* p = owner->memory()->allocate(128,alignof(std::max_align_t));
  mga::RevokePublishedSnapshotVector({platform::UuidKind::object,
                                    platform::Uuid{statement.context.statement_snapshot_uuid.bytes}});
  Check(owner->Observe() == Observation::snapshot_revoked,"actual MGA revocation not observed");
  AllocationRefuses(*owner,1);
  owner->memory()->deallocate(p,128,alignof(std::max_align_t));
  Check(owner->Release() == SB_ENGINE_STATUS_OK,"revoked snapshot cleanup refused");
}

void ConcurrentAllocation(tests::FixtureEngineSession& session, const api::EngineRequestContext& context) {
  Statement statement(session, context); Owner owner;
  Check(statement.Retain(8192, owner) == SB_ENGINE_STATUS_OK, "concurrent real grant");
  std::barrier start(3);
  std::atomic<unsigned> live{0}; std::atomic<bool> failed{false};
  auto worker = [&] {
    start.arrive_and_wait();
    try { for (unsigned i=0;i<256;++i) {
      auto* p = owner->memory()->allocate(256, alignof(std::max_align_t));
      std::memset(p,0x6a,256); ++live;
      Check(owner->Release() == SB_ENGINE_STATUS_CONFLICT, "concurrent release reclaimed a live allocation");
      Check(static_cast<unsigned char*>(p)[255] == 0x6a, "concurrent payload corrupted");
      owner->memory()->deallocate(p,256,alignof(std::max_align_t));
    }} catch (...) { failed.store(true); }
  };
  std::thread first(worker), second(worker); start.arrive_and_wait(); first.join(); second.join();
  Check(!failed && live == 512 && owner->Snapshot().allocated_bytes == 0 &&
      owner->Release() == SB_ENGINE_STATUS_OK, "concurrent retained cleanup failed");
}

#include "statement_blob_input_test.inc"

int main() try {
  Check(mm::ConfigureDefaultMemoryManagerForFixture(mm::DefaultLocalEngineMemoryPolicy(),
      "statement_retained_memory").ok(), "physical fixture policy");
  tests::OwnedTempDirectory directory;
  db::DatabaseCreateConfig create;
  create.path = (directory.path()/"retained.sbdb").string(); create.page_size = 8192;
  create.database_uuid = {platform::UuidKind::database, api::GenerateCrudEngineUuid("database")};
  create.filespace_uuid = {platform::UuidKind::filespace, api::GenerateCrudEngineUuid("filespace")};
  tests::ConfigureCredentialedFixtureBootstrap(create);
  Check(db::CreateDatabaseFile(create).ok(), "credentialed real database");
  auto context = tests::BootstrapFixtureOwnerContext(create);
  tests::FixtureEngineSession session(context);
  api::EngineBeginTransactionRequest begin; begin.context = context; begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin); Check(begun.ok, "actual MGA BEGIN");
  context.transaction_uuid = begun.transaction_uuid; context.local_transaction_id = begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  IdentityAndRetirement(session, context);
#if defined(__linux__)
  ProtectedRetainedPayload(session,context);
  ProtectedScratchFailureDiagnostics(session,context);
  ProtectedDeferredCleanup(session,context);
  ProtectedAdmissionAllocationFailures(session,context);
#endif
  FaultAndReentry(session, context);
  AdmissionAllocationFailures(session, context);
  ConcurrentAllocation(session, context);
  SnapshotRevocation(session, context);
  NativeBlobInput(session, context);
  {
    Statement statement(session, context); Owner owner;
    Check(statement.Retain(4096, owner) == SB_ENGINE_STATUS_OK, "shutdown grant");
    api::StatementBlobInputV3 blob;
    const std::array<platform::byte,3> blob_bytes{0,0xff,0x41};
    Check(blob_input_test::Receive(statement,blob_bytes,blob_input_test::Bounds(),blob)==SB_ENGINE_STATUS_OK,
        "native BLOB shutdown source");
    scratchbird::core::datatypes::BlobRetainedLifetimeLeaseV3 blob_lease;
    Check(blob.Retain(blob_lease).ok(),"native BLOB shutdown retain");
    api::StatementBlobInputV3 encoding;
    auto encoding_bounds=blob_input_test::Bounds(true); encoding_bounds.operation=SB_BLOB_OP_COMPONENT_ENCODE_V3;
    Check(blob_input_test::Receive(statement,blob_bytes,encoding_bounds,encoding)==SB_ENGINE_STATUS_OK,
        "native artifact shutdown source");
    auto encoding_descriptor=blob_input_test::Descriptor(*encoding.profile());
    api::StatementBlobValueV3 encoding_value;
    Check(encoding.RetainTyped(std::move(encoding_descriptor),encoding_value).ok(),"native artifact shutdown typed source");
    api::StatementBlobComponentV3 artifact;
    tests::BlobPersistenceFixture decode_fixture;
    scratchbird::core::datatypes::BlobOwnedPersistenceBytesV3 decode_plain,decode_frame;
    decode_fixture.Copy(blob_bytes,decode_plain); auto decode_grant=decode_fixture.Grant();
    Check(scratchbird::core::datatypes::EncodeOwnedBlobComponentV3(decode_fixture.profile,
        scratchbird::core::datatypes::BlobValueStateV3::value,decode_plain,decode_grant,decode_frame).ok(),
        "native decoded shutdown fixture");
    api::StatementBlobEncodedInputV3 decoding;
    Check(api::ReceiveStatementBlobEncodedInputV3(statement.handle,statement.context.session_uuid.bytes,
        statement.context.statement_receipt_uuid.bytes,decode_frame.bytes(),blob_input_test::EncodedBounds(),&decoding).ok(),
        "native decoded shutdown encoded owner");
    auto decoded_descriptor=blob_input_test::Descriptor(*decoding.profile());
    const auto expected_decoded_descriptor=decoded_descriptor;
    api::StatementBlobDecodedComponentV3 verified;
    api::StatementBlobInputV3 hashing;
    auto hash_bounds=blob_input_test::Bounds(true); hash_bounds.operation=SB_BLOB_OP_HASH_V3;
    Check(blob_input_test::Receive(statement,blob_bytes,hash_bounds,hashing)==SB_ENGINE_STATUS_OK,
        "native hash shutdown source");
    api::StatementBlobValueV3 hash_value; auto hash_descriptor=blob_input_test::Descriptor(*hashing.profile());
    Check(hashing.RetainTyped(std::move(hash_descriptor),hash_value).ok(),"native hash shutdown actual owner");
    hashing.Reset();
    auto* p = owner->memory()->allocate(128, alignof(std::max_align_t));
    statement.Release();
    // Ordinary public retirement must not revoke the actual retained
    // allocation/clock/MGA owner. Encode now, then retain artifact custody
    // independently through forced session teardown.
    Check(encoding_value.EncodeComponent(artifact).ok() && artifact.bound(),
        "native retained component could not encode after public receipt retirement");
    const auto retained_hash=hash_value.HashLogical();
    Check(retained_hash.ok() && std::equal(retained_hash.digest.begin(),retained_hash.digest.end(),artifact.bytes().begin()+248),
        "native logical hash failed after actual root/public receipt retirement");
    Check(encoding_value.Release().ok(),"native artifact source release before session shutdown");
    encoding.Reset();
    Check(decoding.DecodeComponent(std::move(decoded_descriptor),verified).ok() && verified.bound(),
        "native retained decoder failed after ordinary public receipt retirement");
    decoding.Reset();
    std::barrier start(2);
    std::atomic<unsigned> completed{0}, refused{0}; std::atomic<bool> unexpected{false};
    std::thread allocate([&] {
      start.arrive_and_wait();
      for (unsigned i=0;i<256;++i) {
        try {
          auto* q = owner->memory()->allocate(64,alignof(std::max_align_t));
          std::memset(q,0x53,64);
          Check(static_cast<unsigned char*>(q)[63] == 0x53,"shutdown allocation corrupted");
          owner->memory()->deallocate(q,64,alignof(std::max_align_t)); ++completed;
        } catch (const std::bad_alloc&) { ++refused; }
          catch (...) { unexpected = true; }
      }
    });
    start.arrive_and_wait();
    try { session.Close(); } catch (...) { allocate.join(); throw; }
    allocate.join();
    Check(!unexpected && completed+refused==256,"racing actual shutdown failed allocator/cleanup protocol");
    Check(owner->Observe() == Observation::receipt_revoked, "session close did not revoke retained allocation admission");
    const auto stopped_hash=hash_value.HashLogical();
    Check(!stopped_hash.ok() && stopped_hash.failure==api::StatementBlobHashFailureV3::lifetime &&
        stopped_hash.lifetime.diagnostic_code=="CINL.LOB.HANDLE_EXPIRED" && blob_input_test::EmptyHash(stopped_hash) &&
        !stopped_hash.bytes_visited && static_cast<unsigned>(stopped_hash.lifetime.fact.parameters.lifetime_enum)==5 &&
        static_cast<unsigned>(stopped_hash.lifetime.fact.parameters.expiry_reason_enum)==5,
        "actual shutdown did not fence retained hashing with exact native5/5 revocation and last-owner cleanup");
    if(hash_value.bound()) Check(hash_value.Release().ok(),"native hash forced-shutdown cleanup");
    const auto blob_stopped=blob_lease.Probe();
    // Public retirement closes cancellation lookup, but does not invent a
    // positive signal. Forced session teardown revokes this retained receipt.
    Check(!blob_stopped.ok() && blob_stopped.diagnostic_code=="CINL.LOB.HANDLE_EXPIRED" &&
        static_cast<unsigned>(blob_stopped.fact.parameters.lifetime_enum)==5 &&
        static_cast<unsigned>(blob_stopped.fact.parameters.expiry_reason_enum)==5,
        "actual session shutdown failed to revoke native BLOB access");
    if(blob_lease.retained()) Check(blob_lease.Release().ok(),"native BLOB shutdown cleanup");
    blob.Reset();
    Check(artifact.bound() && artifact.bytes().size()==323 &&
        std::equal(blob_bytes.begin(),blob_bytes.end(),artifact.bytes().begin()+320),
        "session shutdown reclaimed or changed already-published native artifact bytes");
    const auto before_artifact_cleanup=mm::DefaultMemoryManager().Snapshot();
    artifact.Reset();
    const auto after_artifact_cleanup=mm::DefaultMemoryManager().Snapshot();
    Check(!artifact.bound() && artifact.bytes().empty() &&
        after_artifact_cleanup.current_bytes<before_artifact_cleanup.current_bytes &&
        after_artifact_cleanup.active_capacity_reservation_count+1==before_artifact_cleanup.active_capacity_reservation_count,
        "native artifact shutdown cleanup did not release its actual physical backing and capacity owner");
    Check(verified.bound() && verified.descriptor() && blob_input_test::ExactDescriptor(*verified.descriptor(),expected_decoded_descriptor) &&
        std::equal(verified.bytes().begin(),verified.bytes().end(),blob_bytes.begin(),blob_bytes.end()),
        "session shutdown reclaimed decoded verified artifact or exact occurrence metadata");
    const auto before_verified_cleanup=mm::DefaultMemoryManager().Snapshot(); verified.Reset();
    const auto after_verified_cleanup=mm::DefaultMemoryManager().Snapshot();
    Check(!verified.bound() && !verified.descriptor() && !verified.profile() && !verified.state() &&
        after_verified_cleanup.current_bytes<before_verified_cleanup.current_bytes &&
        after_verified_cleanup.active_capacity_reservation_count+1==before_verified_cleanup.active_capacity_reservation_count,
        "native decoded artifact teardown failed actual physical/metadata/capacity cleanup");
    AllocationRefuses(*owner,1);
    owner->memory()->deallocate(p,128,alignof(std::max_align_t));
    Check(owner->Release() == SB_ENGINE_STATUS_OK, "post-session retained cleanup");
  }
  directory.Cleanup();
  std::cout << "PASS real retained statement physical memory checks=" << checks.load() << '\n';
  return 0;
} catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
