// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "engine/statement_retained_memory.hpp"
#include "engine/statement_scratch_memory.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_snapshot.hpp"
#include "engine/internal_api/dml/update_resource_authority_provider.hpp"
#include <atomic>
#include <barrier>
#include <cstring>
#include <iostream>
#include <thread>
#include <cstdlib>

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
};
void AllocationRefuses(api::RetainedStatementMemoryV1& owner, std::size_t n) {
  const auto before = owner.Snapshot();
  bool refused = false;
  try { (void)owner.memory()->allocate(n, alignof(std::max_align_t)); }
  catch (const std::bad_alloc&) { refused = true; }
  Check(refused && owner.Snapshot().allocated_bytes == before.allocated_bytes,
        "refused payload allocation changed owned bytes");
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
  Check(api::RunWithStatementScratchMemoryV1(statement.context, [&](auto&) {
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
  Check(api::RunWithStatementScratchMemoryV1(statement.context, [&](auto&) {
    exceptional.reset();
    throw std::runtime_error("owned callback exception fixture");
  }) == SB_ENGINE_STATUS_INTERNAL_ERROR, "callback exception was not contained");
  const auto after_exception = manager.Snapshot();
  Check(after_exception.reserved_capacity_bytes + 4096 == before_exception.reserved_capacity_bytes &&
        after_exception.active_capacity_reservation_count + 1 == before_exception.active_capacity_reservation_count,
        "exception unwind left deferred physical capacity queued");
}

void AdmissionAllocationFailures(tests::FixtureEngineSession& session, const api::EngineRequestContext& context) {
  Statement statement(session, context); Owner owner;
  Check(statement.Retain(4096,owner) == SB_ENGINE_STATUS_OK && owner->Release() == SB_ENGINE_STATUS_OK,
        "allocation sweep warmup"); owner.reset();
  allocation_count = 0;
  Check(statement.Retain(4096,owner) == SB_ENGINE_STATUS_OK, "allocation sweep measurement");
  const auto count = allocation_count;
  Check(owner->Release() == SB_ENGINE_STATUS_OK, "allocation measurement release"); owner.reset();
  Check(count > 0 && count < 4096, "bounded actual owning allocation inventory");
  for (unsigned i=0;i<count;++i) {
    const auto before = mm::DefaultMemoryManager().Snapshot();
    allocation_fault_triggered = false;
    allocation_budget = i;
    const auto status = statement.Retain(4096,owner);
    allocation_budget = -1;
    Check(allocation_fault_triggered, "measured admission allocation position was not actually faulted");
    if (status == SB_ENGINE_STATUS_OK) {
      Check(owner && owner->Observe() == Observation::live && owner->Release() == SB_ENGINE_STATUS_OK,
            "optional allocation failure published incomplete owner"); owner.reset();
    } else Check(!owner && status == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
                 "owning allocation failure leaked partial output or wrong failure");
    const auto after = mm::DefaultMemoryManager().Snapshot();
    Check(after.current_bytes == before.current_bytes && after.reserved_capacity_bytes == before.reserved_capacity_bytes &&
          after.active_capacity_reservation_count == before.active_capacity_reservation_count,
          "actual owning allocation failure leaked physical bytes/capacity");
  }
  const auto resources = statement.context.dml_update_resource_receipt.lock();
  statement.Release();
  Check(resources && resources->IsRevoked(), "allocation failure stranded retained statement ownership");
  std::cout << "actual retained admission allocation fault positions=" << count << '\n';
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
  FaultAndReentry(session, context);
  AdmissionAllocationFailures(session, context);
  ConcurrentAllocation(session, context);
  SnapshotRevocation(session, context);
  {
    Statement statement(session, context); Owner owner;
    Check(statement.Retain(4096, owner) == SB_ENGINE_STATUS_OK, "shutdown grant");
    auto* p = owner->memory()->allocate(128, alignof(std::max_align_t));
    statement.Release();
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
    AllocationRefuses(*owner,1);
    owner->memory()->deallocate(p,128,alignof(std::max_align_t));
    Check(owner->Release() == SB_ENGINE_STATUS_OK, "post-session retained cleanup");
  }
  directory.Cleanup();
  std::cout << "PASS real retained statement physical memory checks=" << checks.load() << '\n';
  return 0;
} catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
