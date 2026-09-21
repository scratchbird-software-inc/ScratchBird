// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/executor/prepared_execution_template.hpp"
#include "../../src/engine/internal_api/catalog/pinned_descriptor_cache.hpp"
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
api::CatalogPinnedDescriptorSnapshot CatalogSnapshot(unsigned char n = 4) {
  api::CatalogPinnedDescriptorSnapshot snapshot;
  snapshot.descriptor = Descriptor();
  snapshot.descriptors = {snapshot.descriptor};
  snapshot.key.catalog_epoch = 1; snapshot.key.security_epoch = 2;
  snapshot.key.resource_policy_epoch = 3; snapshot.key.name_resolution_epoch = 4;
  snapshot.key.descriptor_set_digest =
      api::CatalogPinnedDescriptorSetDigest(snapshot.descriptors, {});
  snapshot.key.object_uuids = {Id(n), Id(9), Id(n)};
  snapshot.key.index_uuids = {Id(8)};
  snapshot.key.security_policy_identity = "policy|redaction_policy_identity=x";
  snapshot.key.redaction_policy_identity = "redaction";
  snapshot.key.resource_policy_identity = "resource";
  return snapshot;
}

void BinaryCatalogKeysAndContent() {
  auto snapshot = CatalogSnapshot();
  Check(snapshot.key.descriptor_set_digest ==
            ex::PreparedDescriptorSetDigest(snapshot.descriptors, {}),
        "catalog descriptor digest differs from shared canonical content");
  auto descriptor = snapshot.descriptor;
  descriptor.type_uuid = Id(11);
  Check(api::CatalogPinnedDescriptorSetDigest({descriptor}, {}) != snapshot.key.descriptor_set_digest,
        "datatype identity missing from catalog descriptor digest");
  descriptor = snapshot.descriptor; descriptor.collation_uuid = Id(12);
  Check(api::CatalogPinnedDescriptorSetDigest({descriptor}, {}) != snapshot.key.descriptor_set_digest,
        "collation identity missing from catalog descriptor digest");
  auto left = snapshot.descriptor, right = snapshot.descriptor;
  left.descriptor_kind = "a:b"; left.canonical_type_name = "c";
  right.descriptor_kind = "a"; right.canonical_type_name = "b:c";
  Check(api::CatalogPinnedDescriptorSetDigest({left}, {}) !=
            api::CatalogPinnedDescriptorSetDigest({right}, {}),
        "delimiter-aliased descriptor content");
  left.encoded_descriptor = std::string("x\0y", 3);
  right = left; right.encoded_descriptor = "x";
  Check(api::CatalogPinnedDescriptorSetDigest({left}, {}) !=
            api::CatalogPinnedDescriptorSetDigest({right}, {}),
        "embedded NUL lost from descriptor content");
  api::CatalogPinnedDescriptorCache cache;
  const auto stored = cache.Put(snapshot);
  Check(stored.ok, "binary catalog snapshot not stored");
  auto key = snapshot.key;
  key.object_uuids = {Id(9), Id(4)};
  Check(cache.Lookup(key).snapshot == stored.snapshot,
        "binary dependency sets did not normalize exact duplicates/order");
  for (unsigned version = 0; version != 16; ++version) {
    auto invalid = key;
    invalid.object_uuids[0].bytes[6] = static_cast<std::uint8_t>(version << 4);
    Check(api::ValidateCatalogPinnedDescriptorKey(invalid).ok == (version == 7),
          "catalog key admitted non-system dependency");
    invalid = key; invalid.index_uuids[0].bytes[6] = static_cast<std::uint8_t>(version << 4);
    Check(api::ValidateCatalogPinnedDescriptorKey(invalid).ok == (version == 7),
          "catalog key admitted non-system index dependency");
  }
  auto different = snapshot;
  different.key.security_policy_identity = "policy";
  different.key.redaction_policy_identity = "x|redaction_policy_identity=redaction";
  Check(api::CatalogPinnedDescriptorCacheKeyText(different.key) != stored.cache_key,
        "cache diagnostic digest has delimiter collision");
  Check(!cache.Lookup(different.key).ok, "different policy bindings hit cached metadata");
  different.key = key; different.key.object_uuids = {Id(5), Id(9)};
  const auto second = cache.Put(different);
  Check(second.ok && second.snapshot != stored.snapshot, "different UUID dependencies merged");
  api::CatalogPinnedDescriptorInvalidationEvent event;
  event.event_kind = "catalog_alter"; event.dependency_uuid = Id(4); event.event_epoch = 2;
  const auto invalidated = cache.Invalidate(event);
  Check(invalidated.invalidated_entries.size() == 1 &&
            invalidated.invalidated_entries[0].object_uuids == snapshot.key.object_uuids,
        "dependency invalidation lost binary identities");
  Check(!cache.Lookup(key).ok && cache.Lookup(different.key).snapshot == second.snapshot &&
            stored.snapshot->descriptor == snapshot.descriptor,
        "invalidation changed unrelated cache state or returned immutable owner");
}

void CatalogInvalidationAllocationSweep() {
  bool completed = false;
  for (long n = 0; n != 256 && !completed; ++n) {
    api::CatalogPinnedDescriptorCache cache;
    const auto first = CatalogSnapshot(4), second = CatalogSnapshot(5);
    Check(cache.Put(first).ok && cache.Put(second).ok, "catalog fault baseline");
    api::CatalogPinnedDescriptorInvalidationEvent event;
    event.event_kind = "catalog_epoch"; event.event_epoch = 2;
    fault::remaining = n; fault::hit = false;
    try {
      const auto invalidated = cache.Invalidate(event);
      fault::remaining = -1;
      Check(invalidated.invalidated_entries.size() == 2, "incomplete catalog invalidation");
      completed = true;
    } catch (const std::bad_alloc&) {
      fault::remaining = -1;
      ++faults;
      Check(cache.Stats().invalidations == 0 && cache.Lookup(first.key).ok &&
                cache.Lookup(second.key).ok,
            "allocation failure partially invalidated catalog cache");
    } catch (...) { fault::remaining = -1; throw; }
  }
  Check(completed, "catalog invalidation allocation sweep did not complete");
}
}
int main() try {
  AdmissionAndBinding(); GovernedLifetime(); AllocationSweeps(); BindAllocationSweeps(); ConcurrentAdmission();
  BinaryCatalogKeysAndContent(); CatalogInvalidationAllocationSweep();
  std::cout << "PASS " << checks << " prepared cache checks, " << faults << " allocation faults; component, not SQL/IPC acceptance\n";
} catch (const std::exception& e) {
  fault::remaining = -1;
  std::cerr << "FAIL " << e.what() << '\n'; return 1;
}
