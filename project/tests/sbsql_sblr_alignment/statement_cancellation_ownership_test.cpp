// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "engine/statement_cancellation.hpp"
#include "engine/statement_scratch_memory.hpp"
#include "transaction/transaction_api.hpp"
#include "storage/database/local_transaction_store.hpp"
#include "server_engine_bridge/diagnostic_fields.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <type_traits>
#include <cstdlib>

thread_local long allocation_budget = -1;
thread_local bool allocation_fault_triggered = false;
void* operator new(std::size_t n) {
  if (allocation_budget >= 0 && allocation_budget-- == 0) {
    allocation_fault_triggered = true; throw std::bad_alloc();
  }
  if (auto* p=std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
#include "../support/aligned_allocation_fault_bridge.hpp"

namespace api=scratchbird::engine::internal_api;
namespace bridge=scratchbird::server_engine_bridge;
namespace tests=scratchbird::tests;
namespace mem=scratchbird::core::memory;
namespace db=scratchbird::storage::database;
namespace platform=scratchbird::core::platform;
using Owner=std::optional<api::RetainedStatementCancellationV1>;
using Observation=api::StatementCancellationObservationV1;
std::atomic<unsigned> checks{0};
void Check(bool ok,const char* why) { ++checks; if(!ok) throw std::runtime_error(why); }
static_assert(!std::is_default_constructible_v<api::RetainedStatementCancellationV1>);
static_assert(!std::is_copy_constructible_v<api::RetainedStatementCancellationV1>);

struct Statement {
  bridge::StatementContextReceiptHandle handle;
  api::EngineRequestContext context;
  Statement(tests::FixtureEngineSession& session,const api::EngineRequestContext& base) {
    bridge::StatementContextAcquireRequest request;
    request.engine_context=&base;request.exact_transaction_uuid=base.transaction_uuid;
    bridge::StatementContextReceiptView view;
    tests::FixtureEngineSession::Check(bridge::AcquireStatementContextReceipt(
        session.get(),&request,&handle,&view,nullptr),nullptr,"cancellation receipt");
    try {
      tests::FixtureEngineSession::Check(bridge::CopyStatementContextEngineContextV1(
          handle,&context,nullptr),nullptr,"cancellation context copy");
    } catch (...) { Release();throw; }
  }
  void Release() {
    if(!handle)return;
    tests::FixtureEngineSession::Check(bridge::ReleaseStatementContextReceipt(handle),nullptr,"cancellation receipt release");
    handle={};
  }
  ~Statement() { Release(); }
  sb_engine_status_t Retain(Owner& output) {
    return api::RetainStatementCancellationV1(handle,context.session_uuid.bytes,
        context.statement_receipt_uuid.bytes,&output);
  }
  sb_engine_status_t Signal() {
    return bridge::RequestStatementContextCancellationV1(handle,context.session_uuid.bytes,
        context.statement_receipt_uuid.bytes);
  }
};

void ExactIdentityAndFailure(tests::FixtureEngineSession& session,const api::EngineRequestContext& base) {
  Statement statement(session,base);Owner owner;
  Check(api::RetainStatementCancellationV1({}, {}, {}, &owner)==SB_ENGINE_STATUS_INVALID_ARGUMENT && !owner,
        "empty receipt issued cancellation owner");
  Check(api::RetainStatementCancellationV1(statement.handle,statement.context.session_uuid.bytes,
      statement.context.statement_receipt_uuid.bytes,nullptr)==SB_ENGINE_STATUS_INVALID_ARGUMENT,
      "null cancellation output admitted");
  for(unsigned i=0;i<16;++i) {
    auto foreign=statement.context.session_uuid.bytes;foreign[i]^=0x80;
    Check(api::RetainStatementCancellationV1(statement.handle,foreign,
        statement.context.statement_receipt_uuid.bytes,&owner)==SB_ENGINE_STATUS_SECURITY_DENIED && !owner,
        "foreign session retained signal");
    Check(bridge::RequestStatementContextCancellationV1(statement.handle,foreign,
        statement.context.statement_receipt_uuid.bytes)==SB_ENGINE_STATUS_SECURITY_DENIED,
        "foreign session signalled cancellation");
    foreign=statement.context.statement_receipt_uuid.bytes;foreign[i]^=0x80;
    Check(api::RetainStatementCancellationV1(statement.handle,statement.context.session_uuid.bytes,
        foreign,&owner)==SB_ENGINE_STATUS_INVALID_HANDLE && !owner,"foreign receipt retained signal");
    Check(bridge::RequestStatementContextCancellationV1(statement.handle,statement.context.session_uuid.bytes,
        foreign)==SB_ENGINE_STATUS_INVALID_HANDLE,"foreign receipt signalled cancellation");
  }
  allocation_budget=0;allocation_fault_triggered=false;
  const auto retained=statement.Retain(owner);allocation_budget=-1;
  Check(retained==SB_ENGINE_STATUS_OK && !allocation_fault_triggered && owner &&
        owner->Observe()==Observation::not_requested,"native cancellation retention allocated or failed to issue clear source");
  const auto* binding=owner->binding();
  Check(binding && binding->database==base.database_uuid.bytes && binding->transaction==base.transaction_uuid.bytes &&
        binding->session==statement.context.session_uuid.bytes && binding->statement==statement.context.statement_uuid.bytes &&
        binding->receipt==statement.context.statement_receipt_uuid.bytes,"signal binding not actual binary receipt tuple");
  Check(statement.Retain(owner)==SB_ENGINE_STATUS_INVALID_ARGUMENT,
        "populated retained signal replaced");
  Owner second;Check(statement.Retain(second)==SB_ENGINE_STATUS_OK,"second independent cancellation owner");
  allocation_budget=0;allocation_fault_triggered=false;
  const auto signalled=statement.Signal();
  const auto observed=owner->Observe();
  const auto repeated=second->Request();
  const auto context_observed=statement.context.query_cancellation_requested();
  allocation_budget=-1;
  Check(signalled==SB_ENGINE_STATUS_OK && observed==Observation::requested && !repeated && context_observed &&
        !allocation_fault_triggered,"actual signal/observation allocated, cleared or missed native request");
  Check(statement.Signal()==SB_ENGINE_STATUS_OK,"duplicate signal ceased being idempotent");
  api::RetainedStatementCancellationV1 moved(std::move(*second));
  Check(!second->binding() && !second->Request() && second->Observe()==Observation::source_unavailable &&
        moved.binding() && moved.Observe()==Observation::requested,"moved signal fabricated state or duplicated custody");
}

void InFlightReceiptGuard(tests::FixtureEngineSession& session,const api::EngineRequestContext& base) {
  Statement statement(session,base);
  std::atomic<bool> entered{false},observed{false},unexpected{false};
  std::atomic<sb_engine_status_t> status{SB_ENGINE_STATUS_INTERNAL_ERROR};
  const auto before=mem::DefaultMemoryManager().Snapshot();
  std::thread worker([&] {
    try {
      status=api::RunWithStatementScratchMemoryV1(statement.context,[&](auto& scratch) {
        auto* bytes=static_cast<unsigned char*>(scratch.memory()->allocate(128,alignof(std::max_align_t)));
        std::memset(bytes,0xa5,128);entered.store(true,std::memory_order_release);
        const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(std::chrono::steady_clock::now()<limit) {
          if(scratch.cancelled()) { observed=true;break; }
          std::this_thread::yield();
        }
        for(unsigned i=0;i<128;++i) if(bytes[i]!=0xa5)unexpected=true;
        scratch.memory()->deallocate(bytes,128,alignof(std::max_align_t));
      });
    }catch(...){unexpected=true;entered=true;}
  });
  const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<limit)std::this_thread::yield();
  const auto signal=statement.Signal(); // MUST not wait for the worker's receipt/lifetime guard.
  worker.join();
  Check(signal==SB_ENGINE_STATUS_OK && status==SB_ENGINE_STATUS_OK && observed && !unexpected,
        "cancellation blocked on in-flight receipt or destroyed live physical work");
  const auto after=mem::DefaultMemoryManager().Snapshot();
  Check(before.current_bytes==after.current_bytes && before.reserved_capacity_bytes==after.reserved_capacity_bytes,
        "cancelled scratch physical resources leaked");
  const auto inventory=db::AcquireStrongLocalTransactionInventorySnapshot(base.database_path);
  Check(inventory.ok(),"actual post-cancel MGA inventory unavailable");
  namespace mga=scratchbird::transaction::mga;
  const auto transaction=mga::LookupLocalTransaction(inventory.snapshot->inventory,
      mga::MakeLocalTransactionId(base.local_transaction_id));
  Check(transaction.ok() && transaction.entry.identity.transaction_uuid.value==base.transaction_uuid &&
      !transaction.entry.rollback_only && transaction.entry.state==mga::TransactionState::active,
      "cancellation request changed MGA transaction finality");
}

void ActualSourceAllocationFailure(tests::FixtureEngineSession& session,const api::EngineRequestContext& base) {
  auto manager=mem::RetainDefaultMemoryManagerV1();
  Check(manager && manager.get()==&mem::DefaultMemoryManager(),"actual manager owner is not configured process manager");
  const auto before=manager->Snapshot();
  mem::MemoryFailureInjectionConfiguration fault;
  fault.test_guard=mem::MakeMemoryFailureInjectionTestGuard();fault.fixture_enabled=true;
  fault.fixture_name="statement_cancellation_ownership";fault.evidence_note="actual native source/control-block backing";
  mem::MemoryFailureInjectionRule rule;rule.rule_id="statement_cancel_source";
  rule.category=mem::MemoryCategory::core_runtime;rule.purpose="stmt_cancel";fault.rules.push_back(rule);
  Check(manager->EnableAllocationFailureInjection(fault).ok(),"actual cancellation-source allocator fault enable");
  bridge::StatementContextAcquireRequest request;
  request.engine_context=&base;request.exact_transaction_uuid=base.transaction_uuid;
  bridge::StatementContextReceiptHandle handle;
  bridge::StatementContextReceiptView view;sb_engine_result_t result=nullptr;
  const auto status=bridge::AcquireStatementContextReceipt(session.get(),&request,&handle,&view,&result);
  const auto observed=manager->FailureInjectionSnapshot();
  Check(manager->DisableAllocationFailureInjection().ok(),"actual cancellation-source allocator fault disable");
  Check(status==SB_ENGINE_STATUS_RESOURCE_EXHAUSTED && !handle && view.receipt_uuid.is_nil() && result &&
      observed.rules.size()==1 && observed.rules.front().failure_count==1,
      "real source allocation fault did not refuse before receipt publication");
  bridge::EngineDiagnosticSnapshot diagnostic;
  Check(bridge::CopyEngineDiagnosticSnapshot(result,0,&diagnostic) &&
      diagnostic.code=="ENGINE.STATEMENT_CONTEXT.RECEIPT_ALLOCATION_FAILED" &&
      diagnostic.message_key=="engine.statement_context.cancellation_source_allocation_failed" &&
      diagnostic.native_source && !diagnostic.native_source->record.diagnostic_code.empty() &&
      !diagnostic.native_source->record.arguments.empty(),"actual source allocator vector lost at receipt boundary");
  Check(sb_engine_result_release(result)==SB_ENGINE_STATUS_OK,"source refusal diagnostic release failed");
  const auto after=manager->Snapshot();
  Check(before.current_bytes==after.current_bytes && before.active_allocation_count==after.active_allocation_count,
      "failed source/control-block allocation leaked physical bytes");
  Statement retry(session,base);Owner owner;
  Check(retry.Retain(owner)==SB_ENGINE_STATUS_OK && owner->Observe()==Observation::not_requested,
      "failed native source issuance poisoned successful retry");
  const auto charged=manager->Snapshot();
  Check(charged.current_bytes>after.current_bytes && charged.active_allocation_count>after.active_allocation_count,
      "native source and shared-control block are not physically accounted");
}

void ReentryAndRetirement(tests::FixtureEngineSession& session,const api::EngineRequestContext& base) {
  Statement statement(session,base);Owner owner;
  Check(statement.Retain(owner)==SB_ENGINE_STATUS_OK,"reentry owner acquisition");
  Check(api::RunWithStatementScratchMemoryV1(statement.context,[&](auto& scratch) {
    Owner recursive;
    Check(statement.Retain(recursive)==SB_ENGINE_STATUS_CONFLICT && !recursive,
          "retained signal lookup recursively entered protected scope");
    Check(statement.Signal()==SB_ENGINE_STATUS_CONFLICT,"public signal lookup entered protected scope");
    Check(owner->Observe()==Observation::not_requested && owner->Request() &&
          owner->Observe()==Observation::requested && scratch.cancelled(),
          "already-owned atomic signal failed under executing receipt guard");
  })==SB_ENGINE_STATUS_OK,"reentry native scratch refused");
  const auto old=statement.handle;statement.Release();
  Owner late;
  Check(api::RetainStatementCancellationV1(old,statement.context.session_uuid.bytes,
      statement.context.statement_receipt_uuid.bytes,&late)==SB_ENGINE_STATUS_INVALID_HANDLE && !late,
      "retired public receipt issued fresh cancellation owner");
  Check(bridge::RequestStatementContextCancellationV1(old,statement.context.session_uuid.bytes,
      statement.context.statement_receipt_uuid.bytes)==SB_ENGINE_STATUS_INVALID_HANDLE,
      "retired public receipt admitted fresh signal lookup");
  Check(owner->Observe()==Observation::requested && statement.context.query_cancellation_requested(),
        "retirement lost actual already-issued signal/context ownership");
}

void LegacyProbeDoesNotEscape(tests::FixtureEngineSession& session,const api::EngineRequestContext& base) {
  auto context=base;
  unsigned calls=0;bool callback_active=true,answer=false,throwing=false;
  context.query_cancellation_requested=[&] {
    ++calls;if(!callback_active)throw std::runtime_error("ended lexical probe invoked");
    if(throwing)throw std::runtime_error("probe failure fixture");
    return answer;
  };
  {
    Statement statement(session,context);Owner owner;
    Check(statement.Retain(owner)==SB_ENGINE_STATUS_OK,"lexical source channel acquisition");
    Check(owner->Observe()==Observation::source_unavailable && calls==0,
          "retained channel invoked raw callback or fabricated not-cancelled");
    Check(!statement.context.query_cancellation_requested() && calls==1 &&
          owner->Observe()==Observation::source_unavailable,
          "false lexical observation became transferable negative authority");
    throwing=true;bool threw=false;
    try{(void)statement.context.query_cancellation_requested();}catch(const std::runtime_error&){threw=true;}
    Check(threw && calls==2 && owner->Observe()==Observation::source_unavailable,
          "throwing lexical source fabricated cancellation");
    throwing=false;answer=true;
    Check(statement.context.query_cancellation_requested() && calls==3 && owner->Observe()==Observation::requested,
          "actual positive lexical observation not latched into native signal");
    statement.Release();callback_active=false;
    Check(statement.context.query_cancellation_requested() && calls==3 && owner->Observe()==Observation::requested,
          "retired positive source invoked ended lexical probe");
  }
  callback_active=true;answer=false;calls=0;
  {
    Statement statement(session,context);Owner owner;
    Check(statement.Retain(owner)==SB_ENGINE_STATUS_OK,"second lexical source channel acquisition");
    statement.Release();callback_active=false;
    Check(!statement.context.query_cancellation_requested() && calls==0 &&
          owner->Observe()==Observation::source_unavailable,
          "retired unknown lexical source invoked expired capture or fabricated false observation");
    Check(owner->Request() && owner->Observe()==Observation::requested &&
          statement.context.query_cancellation_requested() && calls==0,
          "actual retained request did not supersede unavailable lexical source");
  }
}

void ConcurrentOwnedSignalAfterSessionClose(tests::FixtureEngineSession& session,const api::EngineRequestContext& base) {
  Statement statement(session,base);Owner owner;
  Check(statement.Retain(owner)==SB_ENGINE_STATUS_OK,"shutdown retained cancellation acquisition");
  const auto handle=statement.handle;
  std::barrier start(9);std::atomic<unsigned> winners{0};std::vector<std::thread> workers;
  for(unsigned i=0;i<8;++i)workers.emplace_back([&] {
    start.arrive_and_wait();
    for(unsigned n=0;n<256;++n)if(owner->Request())++winners;
  });
  start.arrive_and_wait();
  session.Close();
  for(auto& worker:workers)worker.join();
  Check(winners==1 && owner->Observe()==Observation::requested && statement.context.query_cancellation_requested(),
        "concurrent retained signal lost one-way state or used deleted session");
  Check(bridge::ReleaseStatementContextReceipt(handle)==SB_ENGINE_STATUS_ALREADY_RELEASED,
        "authoritatively closed session retained public receipt");
  statement.handle={}; // Only after actual session close plus explicit release confirmation.
  Check(bridge::RequestStatementContextCancellationV1(handle,statement.context.session_uuid.bytes,
      statement.context.statement_receipt_uuid.bytes)==SB_ENGINE_STATUS_INVALID_HANDLE,
      "closed session admitted public cancellation");
}

int main() try {
  Check(!mem::RetainDefaultMemoryManagerV1() && !mem::DefaultMemoryManagerState().initialized,
      "manager lifetime lookup silently initialized a policy or refusal pool");
  Check(mem::ConfigureDefaultMemoryManagerForFixture(mem::DefaultLocalEngineMemoryPolicy(),
      "statement_cancellation_ownership").ok(),"actual physical memory fixture policy");
  const auto initial_memory=mem::DefaultMemoryManager().Snapshot();
  tests::OwnedTempDirectory directory;
  db::DatabaseCreateConfig create;create.path=(directory.path()/"cancel.sbdb").string();create.page_size=8192;
  create.database_uuid={platform::UuidKind::database,api::GenerateCrudEngineUuid("database")};
  create.filespace_uuid={platform::UuidKind::filespace,api::GenerateCrudEngineUuid("filespace")};
  tests::ConfigureCredentialedFixtureBootstrap(create);
  Check(db::CreateDatabaseFile(create).ok(),"actual credentialed cancellation database");
  auto context=tests::BootstrapFixtureOwnerContext(create);tests::FixtureEngineSession session(context);
  api::EngineBeginTransactionRequest begin;begin.context=context;begin.isolation_level="read_committed";
  const auto begun=api::EngineBeginTransaction(begin);Check(begun.ok,"actual cancellation MGA BEGIN");
  context.transaction_uuid=begun.transaction_uuid;context.local_transaction_id=begun.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id=begun.snapshot_visible_through_local_transaction_id;
  ExactIdentityAndFailure(session,context);ActualSourceAllocationFailure(session,context);
  InFlightReceiptGuard(session,context);
  ReentryAndRetirement(session,context);LegacyProbeDoesNotEscape(session,context);
  ConcurrentOwnedSignalAfterSessionClose(session,context);
  const auto final_memory=mem::DefaultMemoryManager().Snapshot();
  Check(final_memory.current_bytes==initial_memory.current_bytes &&
        final_memory.active_allocation_count==initial_memory.active_allocation_count &&
        final_memory.reserved_capacity_bytes==initial_memory.reserved_capacity_bytes &&
        final_memory.active_capacity_reservation_count==initial_memory.active_capacity_reservation_count,
        "session close and final cancellation-owner destruction leaked actual backing or capacity");
  directory.Cleanup();std::cout<<"statement_cancellation_ownership_checks="<<checks.load()<<'\n';return 0;
}catch(const std::exception& error) {
  allocation_budget=-1;std::cerr<<"statement cancellation: "<<error.what()<<'\n';return 1;
}
