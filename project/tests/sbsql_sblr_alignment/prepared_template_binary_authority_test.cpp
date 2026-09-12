// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/executor/prepared_execution_template.hpp"
#include "uuid.hpp"
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace mem = scratchbird::core::memory;
using Uuid = api::EngineUuid;
namespace {
unsigned checks = 0, faults = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
constexpr Uuid Id(unsigned char n) { return {{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,n}}; }
api::EngineDescriptor Descriptor() {
  api::EngineDescriptor d;
  d.descriptor_uuid = Id(1); d.type_uuid = Id(2);
  d.descriptor_kind = "scalar"; d.canonical_type_name = "int64";
  d.encoded_descriptor = "nullability=non_null";
  return d;
}
ex::PreparedTemplateAdmission Admission() {
  ex::PreparedTemplateAdmission a;
  a.key.operation_id = "test.typed.metadata";
  a.key.sblr_digest_or_trace_key = ex::PreparedTemplateStableDigest({"component typed input"});
  a.key.catalog_epoch_uuid = Id(3); a.key.epochs = {1,2,3,4};
  a.descriptor_slots = {{"value", Descriptor(), 0}};
  a.result_shape = {"rows", a.descriptor_slots, {}};
  a.result_shape.digest = ex::PreparedResultShapeDigest(a.result_shape);
  a.key.result_shape_digest = a.result_shape.digest;
  a.key.descriptor_set_digest = ex::PreparedDescriptorSetDigest({Descriptor()}, {});
  a.key.dependency_uuids = {Id(4)};
  a.field_offsets = {{"value", "data", 0, 8}};
  return a;
}
ex::PhysicalMgaStatementContext Statement() {
  ex::PhysicalMgaStatementContext c;
  c.statement_uuid = Id(10); c.owning_transaction_uuid = Id(11);
  c.statement_snapshot_uuid = Id(12); c.statement_metadata_snapshot_uuid = Id(13);
  c.owning_local_transaction_id = 2; c.visible_committed_high_watermark = 1;
  c.oldest_active_transaction_id = 2; c.oldest_interesting_transaction_id = 2;
  c.oldest_snapshot_transaction_id = 2; c.retention_horizon_transaction_id = 2;
  c.active_excluded_local_transaction_ids = {2};
  c.snapshot_kind = "statement_stable"; c.publication_inventory_next_local_transaction_id = 3;
  c.inventory_authoritative = c.complete = c.current = true;
  return c;
}
ex::PreparedTemplateBindContext BindInput(const ex::PreparedTemplateAdmission& a,
                                        const std::shared_ptr<ex::PhysicalMgaStatementContext>& current) {
  ex::PreparedTemplateBindContext b;
  b.request.descriptors = {Descriptor()};
  b.descriptor_set_digest = a.key.descriptor_set_digest;
  b.result_shape_digest = a.key.result_shape_digest; b.dependency_uuids = a.key.dependency_uuids;
  auto& c = b.engine_context;
  c.security_context_present = true; c.principal_uuid = Id(14); c.current_role_uuid = Id(15);
  c.catalog_epoch_uuid = a.key.catalog_epoch_uuid; c.catalog_generation_id = 1;
  c.security_epoch = 2; c.resource_epoch = 3; c.name_resolution_epoch = 4;
  c.statement_uuid = current->statement_uuid; c.transaction_uuid = current->owning_transaction_uuid;
  c.statement_snapshot_uuid = current->statement_snapshot_uuid;
  c.statement_metadata_snapshot_uuid = current->statement_metadata_snapshot_uuid;
  c.local_transaction_id = 2; c.snapshot_visible_through_local_transaction_id = 1;
  b.mga_authority.statement_context = *current;
  // Controlled input to the real cache checks, not an engine/IPC execution test.
  b.mga_authority.origin = ex::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  b.mga_authority.resolve_current = [current] { ex::CanonicalMgaCurrentResolution r; r.statement_context = *current; return r; };
  return b;
}
ex::PreparedTemplateMemoryGovernanceRequest Governance(mem::ResultCursorPlanMemoryGovernor& g,
                                                     mem::HierarchicalMemoryBudgetLedger& l) {
  ex::PreparedTemplateMemoryGovernanceRequest r;
  r.governor = &g; r.ledger = &l;
  r.scope.process_id = Id(20); r.scope.database_id = Id(21); r.scope.session_id = Id(22);
  r.epochs = {1,2,2,3,3,1,3}; r.estimated_template_bytes = 4096;
  r.estimated_descriptor_snapshot_bytes = 512;
  return r;
}
void AdmissionAndBinding() {
  static_assert(sizeof(ex::PreparedUuid) == 16 && !std::is_convertible_v<ex::PreparedUuid, std::string>);
  static_assert(std::is_same_v<decltype(ex::PreparedTemplateKey::catalog_epoch_uuid), Uuid>);
  static_assert(!std::is_copy_constructible_v<ex::PreparedExecutionTemplate>);
  ex::PreparedTemplateCache cache, other_cache;
  auto a = Admission(); auto result = cache.Prepare(a);
  Check(result.ok && scratchbird::core::uuid::IsEngineIdentityUuid(result.prepared_template->template_id), "actual prepared UUIDv7 issuance");
  auto again = cache.Prepare(a), other = other_cache.Prepare(a);
  Check(again.ok && again.reused_existing_template && again.prepared_template == result.prepared_template, "actual cache reuse");
  Check(other.ok && other.prepared_template->template_id != result.prepared_template->template_id, "content digest reused as template identity");
  auto forged = a;
  forged.result_shape.columns[0].descriptor.type_uuid = Id(99);
  Check(!cache.Prepare(forged).ok, "supplied shape digest replaced actual descriptor bytes");
  ex::PreparedTemplateCache fresh;
  Check(!fresh.Prepare(forged).ok, "forged shape digest accepted by an empty cache");
  forged = a; forged.field_offsets[0].byte_width = 9;
  Check(!cache.Prepare(forged).ok, "same-key changed field offset accepted as cache hit");
  forged = a; forged.policy_metadata.requires_transaction_context = true;
  Check(!cache.Prepare(forged).ok, "same-key changed policy accepted as cache hit");
  auto pinned = a;
  ex::PreparedPinnedDescriptorReference pin;
  pin.cache_key = "typed descriptor content"; pin.catalog_epoch_uuid = a.key.catalog_epoch_uuid;
  pin.descriptor_uuid = Descriptor().descriptor_uuid; pin.object_uuid = Id(4); pin.index_uuid = Id(5);
  pin.descriptor_set_digest = a.key.descriptor_set_digest;
  pin.security_policy_identity = "policy-content"; pin.redaction_policy_identity = "redaction-content";
  pinned.pinned_descriptors = {pin};
  pinned.key.pinned_descriptor_set_digest = ex::PreparedPinnedDescriptorDigest(pinned.pinned_descriptors);
  Check(cache.Prepare(pinned).ok, "valid binary pinned metadata refused");
  pinned.pinned_descriptors[0].object_uuid = Id(100);
  Check(!cache.Prepare(pinned).ok, "caller pinned digest replaced actual identity");
  Check(!fresh.Prepare(pinned).ok, "forged pinned digest accepted by an empty cache");
  for (unsigned v = 0; v != 16; ++v) {
    forged = a; forged.key.catalog_epoch_uuid.bytes[6] = (v << 4) | 7;
    Check(cache.Prepare(forged).ok == (v == 7), "catalog UUID version admission");
    forged = a; forged.key.dependency_uuids[0].bytes[6] = (v << 4) | 7;
    Check(cache.Prepare(forged).ok == (v == 7), "dependency UUID version admission");
  }
  auto current = std::make_shared<ex::PhysicalMgaStatementContext>(Statement());
  auto b = BindInput(a, current);
  auto bound = cache.Bind(*result.prepared_template, b);
  auto rebound = cache.Bind(*result.prepared_template, b);
  Check(bound.ok && rebound.ok, "real bind of admitted object failed");
  Check(bound.statement_use_receipt->receipt_id() != rebound.statement_use_receipt->receipt_id(), "use receipt ID derived from content");
  Check(ex::RevalidatePreparedTemplateStatementUse(*result.prepared_template, bound.statement_use_receipt).ok, "exact owner/context use refused");
  ex::PreparedExecutionTemplate imposter;
  imposter.template_id = result.prepared_template->template_id; imposter.key = result.prepared_template->key;
  Check(!cache.Bind(imposter, b).ok && !ex::RevalidatePreparedTemplateStatementUse(imposter, bound.statement_use_receipt).ok, "forged equal UUID replaced actual owner");
  Check(!other_cache.Bind(*result.prepared_template, b).ok, "foreign cache accepted owner");
  b.request.descriptors[0].type_uuid = Id(99);
  Check(!cache.Bind(*result.prepared_template, b).ok, "supplied bind digest replaced actual current descriptor bytes");
  current->statement_timestamp = "changed";
  Check(!ex::RevalidatePreparedTemplateStatementUse(*result.prepared_template, bound.statement_use_receipt).ok, "MGA field substitution accepted");
}
void GovernedLifetime() {
  mem::HierarchicalMemoryBudgetLedger ledger;
  mem::ResultCursorPlanMemoryGovernor governor;
  ex::PreparedTemplateCache cache;
  auto a = Admission(); auto g = Governance(governor, ledger);
  auto prepared = cache.PrepareGoverned(a, g);
  Check(prepared.ok && ledger.Snapshot().current_bytes == 4608, "governed metadata reservation missing");
  auto current = std::make_shared<ex::PhysicalMgaStatementContext>(Statement());
  auto bound = cache.Bind(*prepared.prepared_template, BindInput(a, current));
  Check(bound.ok, "governed bind refused");
  auto wrong = g; wrong.scope.database_id = Id(99);
  Check(!cache.PrepareGoverned(a, wrong).ok, "governed cache lookup transferred database owner");
  Check(cache.InvalidateGovernedByEpoch({2,2,2,3,3,1,3}, &governor) == 1, "epoch invalidation did not remove entry");
  Check(!cache.Lookup(prepared.prepared_template->key), "invalidated entry still in cache");
  Check(ledger.Snapshot().current_bytes == 4608, "eviction uncharged still-live metadata");
  Check(!ex::RevalidatePreparedTemplateStatementUse(*prepared.prepared_template, bound.statement_use_receipt).ok, "evicted metadata remained executable");
  prepared = {}; bound.prepared_template.reset();
  Check(ledger.Snapshot().current_bytes == 4608, "receipt stopped owning its retained template");
  bound = {};
  Check(ledger.Snapshot().current_bytes == 0 && governor.Snapshot().active_lease_count == 0, "last actual owner destruction leaked leases");
}
void AllocationSweeps() {
  for (bool governed : {false, true}) {
    bool complete = false;
    for (long n = 0; n != 3000; ++n) {
      mem::HierarchicalMemoryBudgetLedger ledger;
      mem::ResultCursorPlanMemoryGovernor governor;
      ex::PreparedTemplateCache cache;
      auto a = Admission(); const auto key = a.key; auto g = Governance(governor, ledger);
      ex::PreparedTemplatePrepareResult result;
      fault::remaining = n; fault::hit = false;
      try { result = governed ? cache.PrepareGoverned(std::move(a), std::move(g)) : cache.Prepare(std::move(a)); }
      catch (...) { fault::remaining = -1; throw; }
      fault::remaining = -1;
      if (fault::hit) {
        ++faults;
        Check(!result.ok, "allocation failure published success");
        Check(!cache.Lookup(key), "failed prepare registered a cache entry");
        Check(ledger.Snapshot().current_bytes == 0 && governor.Snapshot().active_lease_count == 0, "failed prepare lost unpublished reservation");
      } else {
        Check(result.ok, "uninjected prepare failed"); complete = true; break;
      }
    }
    Check(complete, "prepare allocation sweep incomplete");
  }
}

void BindAllocationSweeps() {
  ex::PreparedTemplateCache cache;
  auto a = Admission(); auto p = cache.Prepare(a);
  Check(p.ok, "bind fault baseline owner");
  auto current = std::make_shared<ex::PhysicalMgaStatementContext>(Statement());
  auto b = BindInput(a, current);
  bool complete = false;
  for (long n = 0; n != 3000; ++n) {
    fault::remaining = n; fault::hit = false;
    ex::PreparedTemplateBindResult result;
    try { result = cache.Bind(*p.prepared_template, b); }
    catch (...) { fault::remaining = -1; throw; }
    fault::remaining = -1;
    if (fault::hit) {
      ++faults;
      Check(!result.ok && !result.statement_use_receipt && !result.prepared_template,
            "failed bind published partial authority");
      Check(cache.Lookup(a.key) == p.prepared_template, "failed bind consumed the shared owner");
    } else { Check(result.ok, "uninjected bind failed"); complete = true; break; }
  }
  Check(complete, "bind allocation sweep incomplete");
  auto bound = cache.Bind(*p.prepared_template, b);
  complete = false;
  for (long n = 0; n != 3000; ++n) {
    fault::remaining = n; fault::hit = false;
    ex::PreparedTemplateUseValidationResult result;
    try { result = ex::RevalidatePreparedTemplateStatementUse(*p.prepared_template, bound.statement_use_receipt); }
    catch (...) { fault::remaining = -1; throw; }
    fault::remaining = -1;
    if (fault::hit) {
      ++faults;
      Check(!result.ok && !result.executable_receipt, "failed revalidation published executable authority");
    } else { Check(result.ok, "uninjected use failed"); complete = true; break; }
  }
  Check(complete, "use allocation sweep incomplete");
}
void ConcurrentAdmission() {
  ex::PreparedTemplateCache cache;
  std::barrier gate(5);
  ex::PreparedTemplatePrepareResult results[4];
  std::thread threads[4];
  for (unsigned i = 0; i != 4; ++i) threads[i] = std::thread([&, i] {
    auto a = Admission(); gate.arrive_and_wait(); results[i] = cache.Prepare(std::move(a));
  });
  gate.arrive_and_wait(); for (auto& thread : threads) thread.join();
  unsigned created = 0;
  for (const auto& r : results) {
    Check(r.ok && r.prepared_template == results[0].prepared_template, "concurrent publication split cache owner");
    created += !r.reused_existing_template;
  }
  Check(created == 1, "concurrent identical admissions created multiple owners");
}
}
int main() try {
  AdmissionAndBinding(); GovernedLifetime(); AllocationSweeps(); BindAllocationSweeps(); ConcurrentAdmission();
  std::cout << "PASS " << checks << " prepared cache checks, " << faults << " allocation faults; component, not SQL/IPC acceptance\n";
} catch (const std::exception& e) {
  fault::remaining = -1;
  std::cerr << "FAIL " << e.what() << '\n'; return 1;
}
