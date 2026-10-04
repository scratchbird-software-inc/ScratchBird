// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_scheduler_policy.hpp"
#include "catalog_scheduler_fairness.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <source_location>
#include <stdexcept>

namespace {
bool deny_heap = false, count_allocations = false;
unsigned heap_attempts = 0, allocation_count = 0;
long allocation_budget = -1;
}
void* operator new(std::size_t n) {
  if (deny_heap) { ++heap_attempts; throw std::bad_alloc(); }
  if (count_allocations) ++allocation_count;
  if (allocation_budget == 0) { allocation_budget = -1; throw std::bad_alloc(); }
  if (allocation_budget > 0) --allocation_budget;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
namespace c = scratchbird::core::catalog;
using namespace scratchbird::core::platform;
using Policy = c::CatalogSchedulerPolicy;
unsigned checks = 0;
void Check(bool value, const char* why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(why) + " line=" + std::to_string(at.line()));
}
TypedUuid Id(UuidKind kind, byte tag) {
  return {kind, Uuid{{0,0,0,0,0,tag,0x70,0,0x80,0,0,0,0,0,0,tag}}};
}
void Put(std::string& bytes, std::size_t offset, u64 value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) { bytes.at(offset+i) = static_cast<char>(value & 255); value >>= 8; }
}
std::string Number(u64 value) { std::string b(8,0); Put(b,0,value,8); return b; }
std::string Identity(Uuid id) { return {reinterpret_cast<const char*>(id.bytes.data()),16}; }
void Field(std::string& b, unsigned id, byte type, const std::string& value) {
  const auto at = b.size(); b.resize(at+8,0);
  Put(b,at,id,2); Put(b,at+2,type,1); Put(b,at+4,value.size(),4); b += value;
}
// Independent exact SBCV field/type/order oracle, not the production schema.
std::string Oracle(const Policy& p) {
  std::string b(24,0); b.replace(0,4,"SBCV"); Put(b,4,1,2); Put(b,6,24,2);
  Put(b,8,248,4); Put(b,12,11,4); Put(b,16,786433,4); Put(b,20,1,2);
  Field(b,1,5,Identity(p.policy_uuid)); Field(b,2,1,Number(p.generation));
  Field(b,3,5,Identity(p.database_uuid)); Field(b,4,5,Identity(p.queue_profile_uuid));
  Field(b,5,5,Identity(p.resource_profile_uuid)); Field(b,6,5,Identity(p.fairness_profile_uuid));
  Field(b,7,1,Number(p.generation_maximum)); Field(b,8,1,Number(p.generation_warning_threshold));
  Field(b,9,1,Number(p.generation_admission_stop_threshold));
  Field(b,10,5,Identity(p.origin_transaction_uuid.value)); Field(b,11,1,Number(p.origin_local_transaction_id));
  return b;
}
Policy Example() {
  Policy p;
  p.policy_uuid = Id(UuidKind::object,1).value; p.generation = 1;
  p.database_uuid = Id(UuidKind::database,2).value;
  p.queue_profile_uuid = Id(UuidKind::object,3).value;
  p.resource_profile_uuid = Id(UuidKind::object,4).value;
  p.fairness_profile_uuid = Id(UuidKind::object,5).value;
  p.generation_maximum = std::numeric_limits<u64>::max();
  p.generation_warning_threshold = p.generation_maximum-2;
  p.generation_admission_stop_threshold = p.generation_maximum-1;
  p.origin_transaction_uuid = Id(UuidKind::transaction,6); p.origin_local_transaction_id = 1;
  return p;
}
c::CatalogMetadataVersion Metadata(const Policy& p) {
  c::CatalogMetadataVersion m;
  m.record.header = {c::CatalogRecordKind::policy,Id(UuidKind::row,10),
      {UuidKind::object,p.policy_uuid},Id(UuidKind::object,11),1,false};
  m.record.payload = Oracle(p); m.owning_schema_uuid = Id(UuidKind::schema,11);
  m.owner_uuid = Id(UuidKind::principal,12); m.audit_uuid = Id(UuidKind::object,13);
  m.default_name_uuid = Id(UuidKind::object,14); m.name_vector_uuid = Id(UuidKind::object,15);
  m.creator_transaction_uuid = p.origin_transaction_uuid; m.creator_local_transaction_id = p.origin_local_transaction_id;
  m.definition_version = p.generation;
  m.schema_epoch = m.security_epoch = m.catalog_generation = m.dependency_generation = m.invalidation_generation = 1;
  m.lifecycle = c::CatalogObjectLifecycle::active; m.status = c::CatalogObjectStatus::active;
  m.object_subtype = "scheduler_runtime"; m.trace_search_key = "SCHEDULER-POLICY-ORACLE";
  m.retention_class = "catalog_history";
  return m;
}
void Good(const Policy& p) {
  const auto expected = Oracle(p);
  const auto encoded = c::EncodeCatalogSchedulerPolicy(p);
  Check(encoded.ok() && std::string(encoded.bytes.begin(),encoded.bytes.end()) == expected,"independent full bytes");
  const auto decoded = c::DecodeCatalogSchedulerPolicy(expected);
  Check(decoded.ok() && Oracle(*decoded.record) == expected,"all native fields exact");
  Check(c::IsCatalogSchedulerPolicyPayload(expected),"family discriminator");
  Check(c::CatalogSchedulerPolicyMatchesMetadata(Metadata(p)),"metadata binding");
}
void Refused(std::string_view b) {
  deny_heap = true;
  const auto decoded = c::DecodeCatalogSchedulerPolicy(b);
  deny_heap = false;
  Check(!decoded.ok() && !decoded.record,"refusal has no partial record");
}
void Bad(const Policy& p) {
  const auto encoded = c::EncodeCatalogSchedulerPolicy(p);
  Check(!encoded.ok() && encoded.bytes.empty(),"invalid native value has no encoded prefix");
  Refused(Oracle(p));
}
void FieldsAndFraming() {
  auto p = Example(); const auto original = Oracle(p);
  constexpr std::array<std::size_t,11> offsets{24,48,64,88,112,136,160,176,192,208,232};
  for (std::size_t n = 0; n < original.size(); ++n) Refused(std::string_view(original).substr(0,n));
  auto b = original; b.push_back(0); Refused(b);
  for (const auto offset : {0u,4u,6u,8u,12u,16u,20u,22u}) { b = original; b[offset] ^= 0x40; Refused(b); }
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const auto at = offsets[i];
    for (const auto byte_offset : {0u,2u,3u,4u}) { b = original; b[at+byte_offset] ^= 0x40; Refused(b); }
    const auto size = (i+1 < offsets.size() ? offsets[i+1] : original.size())-at;
    b = original; b.erase(at,size); Put(b,8,b.size(),4); Put(b,12,10,4); Refused(b);
    b = original; Put(b,at,0,2); Refused(b);
    if (i != 0) { b = original; Put(b,at,i,2); Refused(b); }
  }
  for (const auto at : {24u,64u,88u,112u,136u,208u}) {
    for (unsigned mode = 0; mode < 3; ++mode) {
      b = original;
      if (mode == 0) std::fill_n(b.begin()+at+8,16,0);
      if (mode == 1) b[at+8+6] = 0x40;
      if (mode == 2) b[at+8+8] = 0;
      Refused(b);
    }
  }
  // Decode with the heap denied even on first successful invocation; fixed
  // schema setup must not hide lazy vector allocations.
  deny_heap = true; const auto decoded = c::DecodeCatalogSchedulerPolicy(original); deny_heap = false;
  Check(decoded.ok() && heap_attempts == 0,"fixed decoder needs no C++ heap");
  b = original; const auto retained = c::DecodeCatalogSchedulerPolicy(b); b.assign(b.size(),0);
  Check(retained.ok() && Oracle(*retained.record) == original,"native result owns values after input destruction");
  Check(Oracle(p) == original,"input remains immutable");
}
void BoundariesAndOrigin() {
  auto p = Example(); Good(p);
  for (const auto member : {&Policy::generation,&Policy::origin_local_transaction_id}) {
    auto bad = p; bad.*member = 0; Bad(bad);
    auto large = p; large.*member = std::numeric_limits<u64>::max(); Good(large);
  }
  // Complete small-domain relation plus uint64 boundaries, not arithmetic that
  // wraps a fixture into an accidentally valid threshold.
  for (u64 maximum = 0; maximum < 5; ++maximum)
    for (u64 stop = 0; stop < 5; ++stop)
      for (u64 warning = 0; warning < 5; ++warning) {
        auto value = p; value.generation_maximum = maximum;
        value.generation_warning_threshold = warning; value.generation_admission_stop_threshold = stop;
        if (warning <= stop && stop < maximum) Good(value); else Bad(value);
      }
  auto bad = p; bad.generation_admission_stop_threshold = p.generation_maximum; Bad(bad);
  bad = p; bad.generation_warning_threshold = p.generation_maximum; Bad(bad);
  bad = p; bad.origin_transaction_uuid.kind = UuidKind::object;
  Check(!c::EncodeCatalogSchedulerPolicy(bad).ok(),"origin kind cannot be coerced");
  const auto first = Metadata(p);
  auto successor = p; successor.generation = 2; successor.queue_profile_uuid = Id(UuidKind::object,20).value;
  auto next = Metadata(successor); next.creator_transaction_uuid = Id(UuidKind::transaction,21);
  next.creator_local_transaction_id = 2;
  Check(c::CatalogSchedulerPolicyPreservesOrigin(first,next),"new selected references preserve origin");
  for (const auto member : {&Policy::policy_uuid,&Policy::database_uuid}) {
    auto changed = successor; changed.*member = Id(UuidKind::object,30).value;
    Check(!c::CatalogSchedulerPolicyPreservesOrigin(first,Metadata(changed)),"immutable identity rejected");
  }
  auto changed = successor; ++changed.origin_local_transaction_id;
  Check(!c::CatalogSchedulerPolicyPreservesOrigin(first,Metadata(changed)),"origin number preserved");
  changed = successor; changed.origin_transaction_uuid = Id(UuidKind::transaction,31);
  Check(!c::CatalogSchedulerPolicyPreservesOrigin(first,Metadata(changed)),"origin UUID preserved");
  for (unsigned which = 0; which < 9; ++which) {
    auto wrong = first;
    if (which == 0) wrong.object_subtype = "storage_action";
    if (which == 1) wrong.record.header.kind = c::CatalogRecordKind::config_profile;
    if (which == 2) wrong.record.header.object_uuid = Id(UuidKind::object,32);
    if (which == 3) ++wrong.definition_version;
    if (which == 4) wrong.owning_schema_uuid = Id(UuidKind::schema,33);
    if (which == 5) wrong.default_name_uuid = {};
    if (which == 6) wrong.name_vector_uuid = {};
    if (which == 7) ++wrong.creator_local_transaction_id;
    if (which == 8) wrong.authority_scope = static_cast<c::CatalogAuthorityScope>(255);
    Check(!c::CatalogSchedulerPolicyMatchesMetadata(wrong),"exact envelope binding");
  }
  auto unrelated = first; unrelated.object_subtype = "unrelated"; unrelated.record.payload = "unrelated";
  Check(c::CatalogSchedulerPolicyPreservesOrigin(unrelated,unrelated),"unrelated family remains owner's concern");
  deny_heap = true;
  const bool matches = c::CatalogSchedulerPolicyMatchesMetadata(first);
  const bool preserves = c::CatalogSchedulerPolicyPreservesOrigin(first,next);
  deny_heap = false;
  Check(matches && preserves && heap_attempts == 0,"family hooks use fixed native decoding");
}
void AllocationFailures() {
  const auto p = Example(); const auto expected = Oracle(p);
  allocation_count = 0; count_allocations = true;
  const auto original = c::EncodeCatalogSchedulerPolicy(p);
  count_allocations = false;
  Check(original.ok() && allocation_count != 0,"owning encoder allocation sites observed");
  const auto sites = allocation_count;
  for (unsigned site = 0; site < sites; ++site) {
    bool failed = false; allocation_budget = site;
    try { (void)c::EncodeCatalogSchedulerPolicy(p); }
    catch (const std::bad_alloc&) { failed = true; }
    allocation_budget = -1;
    Check(failed,"every observed owning allocation failure propagates");
    Check(Oracle(p) == expected,"allocation failure preserves input policy");
    Good(p);
  }
  std::cout << "owning_encoder_allocation_sites=" << sites << '\n';
}
using QueueProfile = c::CatalogSchedulerQueueProfile;
QueueProfile Queues() {
  QueueProfile p;
  p.profile_uuid = Id(UuidKind::object,50).value; p.database_uuid = Example().database_uuid;
  p.generation = 1; p.origin_transaction_uuid = Example().origin_transaction_uuid;
  p.origin_local_transaction_id = 1;
  for (unsigned i = 0; i < p.queues.size(); ++i) {
    auto& q = p.queues[i]; q.max_depth = i*3; q.max_concurrent = i*5;
    q.admission_policy_uuid = Id(UuidKind::object,static_cast<byte>(100+i)).value;
    q.fairness_profile_uuid = Id(UuidKind::object,static_cast<byte>(150+i)).value;
  }
  return p;
}
std::string QueueOracle(const QueueProfile& p) {
  std::string b(24,0); b.replace(0,4,"SBCV"); Put(b,4,1,2); Put(b,6,24,2);
  Put(b,8,1088,4); Put(b,12,53,4); Put(b,16,786434,4); Put(b,20,1,2);
  Field(b,1,5,Identity(p.profile_uuid)); Field(b,2,1,Number(p.generation));
  Field(b,3,5,Identity(p.database_uuid)); Field(b,4,5,Identity(p.origin_transaction_uuid.value));
  Field(b,5,1,Number(p.origin_local_transaction_id));
  for (unsigned i = 0; i < 12; ++i) {
    const auto& q = p.queues[i];
    Field(b,16+4*i,1,Number(q.max_depth)); Field(b,17+4*i,1,Number(q.max_concurrent));
    Field(b,18+4*i,5,Identity(q.admission_policy_uuid)); Field(b,19+4*i,5,Identity(q.fairness_profile_uuid));
  }
  return b;
}
c::CatalogMetadataVersion QueueMetadata(const QueueProfile& p) {
  auto m = Metadata(Example()); m.record.header.object_uuid = {UuidKind::object,p.profile_uuid};
  m.record.payload = QueueOracle(p); m.object_subtype = "scheduler_queues"; m.definition_version = p.generation;
  m.creator_transaction_uuid = p.origin_transaction_uuid; m.creator_local_transaction_id = p.origin_local_transaction_id;
  return m;
}
void QueueGood(const QueueProfile& p) {
  const auto expected = QueueOracle(p); const auto encoded = c::EncodeCatalogSchedulerQueueProfile(p);
  Check(encoded.ok() && std::string(encoded.bytes.begin(),encoded.bytes.end()) == expected,"independent whole queue profile bytes");
  deny_heap = true; const auto decoded = c::DecodeCatalogSchedulerQueueProfile(expected); deny_heap = false;
  Check(decoded.ok() && QueueOracle(*decoded.record) == expected,"all twelve native queue slots and uint64 limits");
  const auto metadata = QueueMetadata(p);
  deny_heap = true;
  const bool matches = c::CatalogSchedulerQueueProfileMatchesMetadata(metadata);
  deny_heap = false;
  Check(matches,"queue definition metadata binding");
}
void QueueRefused(std::string_view bytes) {
  deny_heap = true; const auto result = c::DecodeCatalogSchedulerQueueProfile(bytes); deny_heap = false;
  Check(!result.ok() && !result.record,"queue refusal has no partial profile");
}
void QueueDefinition() {
  const auto p = Queues(); const auto original = QueueOracle(p); QueueGood(p);
  std::array<std::size_t,53> offsets{24,48,64,88,112};
  for (unsigned i = 0; i < 12; ++i) {
    offsets[5+4*i] = 128+80*i; offsets[6+4*i] = 144+80*i;
    offsets[7+4*i] = 160+80*i; offsets[8+4*i] = 184+80*i;
    for (const auto limit : {u64{0},u64{1},std::numeric_limits<u64>::max()}) {
      auto value = p; value.queues[i].max_depth = limit; QueueGood(value);
      value = p; value.queues[i].max_concurrent = limit; QueueGood(value);
    }
    for (const bool fairness : {false,true}) {
      auto invalid = p;
      (fairness ? invalid.queues[i].fairness_profile_uuid : invalid.queues[i].admission_policy_uuid) = {};
      Check(!c::EncodeCatalogSchedulerQueueProfile(invalid).ok(),"each queue identity required");
      QueueRefused(QueueOracle(invalid));
    }
  }
  for (std::size_t n = 0; n < original.size(); ++n) QueueRefused(std::string_view(original).substr(0,n));
  auto b = original; b.push_back(0); QueueRefused(b);
  for (const auto offset : {0u,4u,6u,8u,12u,16u,20u,22u}) { b = original; b[offset] ^= 0x40; QueueRefused(b); }
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const auto at = offsets[i]; const auto width = (i+1 < offsets.size() ? offsets[i+1] : original.size())-at;
    for (const auto offset : {0u,2u,3u,4u}) { b = original; b[at+offset] ^= 0x40; QueueRefused(b); }
    b = original; b.erase(at,width); Put(b,8,b.size(),4); Put(b,12,52,4); QueueRefused(b);
    b = original; Put(b,at,6,2); QueueRefused(b); // Reserved gap cannot replace a required field.
    if (i != 0) {
      b = original; b[at] = original[offsets[i-1]]; b[at+1] = original[offsets[i-1]+1]; QueueRefused(b);
    }
    if (original[at+2] == 5) {
      for (unsigned mode = 0; mode < 3; ++mode) {
        b = original;
        if (mode == 0) std::fill_n(b.begin()+at+8,16,0);
        if (mode == 1) b[at+8+6] = 0x40;
        if (mode == 2) b[at+8+8] = 0;
        QueueRefused(b);
      }
    }
  }
  QueueRefused(Oracle(Example())); Refused(original);
  const auto first = QueueMetadata(p);
  auto changed = p; changed.generation = 2; changed.queues[11].max_depth = 0;
  auto next = QueueMetadata(changed); next.creator_transaction_uuid = Id(UuidKind::transaction,60);
  next.creator_local_transaction_id = 2;
  deny_heap = true; const bool preserves = c::CatalogSchedulerQueueProfilePreservesOrigin(first,next); deny_heap = false;
  Check(preserves,"queue limit update preserves original identity");
  for (unsigned field = 0; field < 4; ++field) {
    auto wrong = changed;
    if (field == 0) wrong.database_uuid = Id(UuidKind::database,61).value;
    if (field == 1) wrong.profile_uuid = Id(UuidKind::object,62).value;
    if (field == 2) wrong.origin_transaction_uuid = Id(UuidKind::transaction,63);
    if (field == 3) ++wrong.origin_local_transaction_id;
    Check(!c::CatalogSchedulerQueueProfilePreservesOrigin(first,QueueMetadata(wrong)),"queue origin cannot change");
  }
  for (unsigned field = 0; field < 9; ++field) {
    auto wrong = first;
    if (field == 0) wrong.object_subtype = "scheduler_runtime";
    if (field == 1) wrong.record.header.kind = c::CatalogRecordKind::config_profile;
    if (field == 2) wrong.record.header.object_uuid = Id(UuidKind::object,32);
    if (field == 3) ++wrong.definition_version;
    if (field == 4) wrong.owning_schema_uuid = Id(UuidKind::schema,33);
    if (field == 5) wrong.default_name_uuid = {};
    if (field == 6) wrong.name_vector_uuid = {};
    if (field == 7) ++wrong.creator_local_transaction_id;
    if (field == 8) wrong.authority_scope = static_cast<c::CatalogAuthorityScope>(255);
    Check(!c::CatalogSchedulerQueueProfileMatchesMetadata(wrong),"exact queue metadata binding");
  }
  for (unsigned field = 0; field < 2; ++field) {
    auto value = p;
    (field == 0 ? value.generation : value.origin_local_transaction_id) = 0;
    Check(!c::EncodeCatalogSchedulerQueueProfile(value).ok(),"queue generation and origin required");
    QueueRefused(QueueOracle(value));
    (field == 0 ? value.generation : value.origin_local_transaction_id) = std::numeric_limits<u64>::max();
    QueueGood(value);
  }
  auto detached = original; const auto copied = c::DecodeCatalogSchedulerQueueProfile(detached); detached.assign(detached.size(),0);
  Check(copied.ok() && QueueOracle(*copied.record) == original,"native queue values outlive source");
  Check(original == QueueOracle(p) && heap_attempts == 0,"immutable queue source and allocation-free decode");
  allocation_count = 0; count_allocations = true;
  const auto encoded = c::EncodeCatalogSchedulerQueueProfile(p); count_allocations = false;
  const auto sites = allocation_count; Check(encoded.ok() && sites != 0,"queue encoder allocation sites observed");
  for (unsigned site = 0; site < sites; ++site) {
    bool failed = false; allocation_budget = site;
    try { (void)c::EncodeCatalogSchedulerQueueProfile(p); } catch (const std::bad_alloc&) { failed = true; }
    allocation_budget = -1;
    Check(failed && QueueOracle(p) == original,"queue allocation failure atomicity"); QueueGood(p);
  }
  std::cout << "queue_encoder_allocation_sites=" << sites << '\n';
}
using Fairness = c::CatalogSchedulerFairnessProfile;
Fairness FairnessExample() {
  Fairness p;
  p.profile_uuid = Id(UuidKind::object,70).value; p.database_uuid = Id(UuidKind::database,2).value;
  p.generation = 1; p.origin_transaction_uuid = Id(UuidKind::transaction,6); p.origin_local_transaction_id = 1;
  p.aging_interval_us = 100; p.aging_rank_ceiling = 1000; p.per_target_max_concurrent = 3;
  p.foreground_reserved_worker_slots = 2; p.maintenance_reserved_worker_slots = 1;
  for (std::size_t i = 0; i < 24; ++i) p.family_max_concurrent[i] = i;
  for (std::size_t i = 0; i < 9; ++i) p.base_priority_rank[i] = i*10;
  for (std::size_t i = 0; i < 12; ++i) p.queue_waits[i] = {i*100,i%2 == 0};
  return p;
}
std::string FairnessOracle(const Fairness& p) {
  std::string b(24,0); b.replace(0,4,"SBCV"); Put(b,4,1,2); Put(b,6,24,2);
  Put(b,8,p.emergency_evidence_policy_uuid ? 1069 : 1045,4);
  Put(b,12,p.emergency_evidence_policy_uuid ? 69 : 68,4); Put(b,16,786435,4); Put(b,20,1,2);
  Field(b,1,5,Identity(p.profile_uuid)); Field(b,2,1,Number(p.generation));
  Field(b,3,5,Identity(p.database_uuid)); Field(b,4,5,Identity(p.origin_transaction_uuid.value));
  Field(b,5,1,Number(p.origin_local_transaction_id)); Field(b,6,1,Number(p.aging_interval_us));
  Field(b,7,1,Number(p.aging_rank_ceiling)); Field(b,8,1,Number(p.per_target_max_concurrent));
  Field(b,9,1,Number(p.foreground_reserved_worker_slots)); Field(b,10,1,Number(p.maintenance_reserved_worker_slots));
  Field(b,11,2,std::string(1,p.emergency_override_allowed ? 1 : 0));
  if (p.emergency_evidence_policy_uuid) Field(b,12,5,Identity(*p.emergency_evidence_policy_uuid));
  for (unsigned i = 0; i < 24; ++i) Field(b,16+i,1,Number(p.family_max_concurrent[i]));
  for (unsigned i = 0; i < 9; ++i) Field(b,48+i,1,Number(p.base_priority_rank[i]));
  for (unsigned i = 0; i < 12; ++i) {
    Field(b,64+2*i,1,Number(p.queue_waits[i].maximum_queue_wait_us));
    Field(b,65+2*i,2,std::string(1,p.queue_waits[i].allow_indefinite_deferral ? 1 : 0));
  }
  return b;
}
c::CatalogMetadataVersion FairnessMetadata(const Fairness& p) {
  auto m = Metadata(Example()); m.record.header.object_uuid = {UuidKind::object,p.profile_uuid};
  m.record.payload = FairnessOracle(p); m.definition_version = p.generation;
  m.creator_transaction_uuid = p.origin_transaction_uuid; m.creator_local_transaction_id = p.origin_local_transaction_id;
  m.object_subtype = "scheduler_fairness"; return m;
}
void FairnessGood(const Fairness& p) {
  const auto bytes = FairnessOracle(p); const auto metadata = FairnessMetadata(p);
  const auto encoded = c::EncodeCatalogSchedulerFairnessProfile(p);
  Check(encoded.ok() && std::string(encoded.bytes.begin(),encoded.bytes.end()) == bytes,"fairness exact independent bytes");
  deny_heap = true;
  const auto decoded = c::DecodeCatalogSchedulerFairnessProfile(bytes);
  const bool bound = c::CatalogSchedulerFairnessProfileMatchesMetadata(metadata);
  deny_heap = false;
  Check(decoded.ok() && FairnessOracle(*decoded.record) == bytes,"all native fairness slots exact");
  Check(bound && c::IsCatalogSchedulerFairnessProfilePayload(bytes),"fairness metadata and discriminator");
}
void FairnessRefused(std::string_view bytes) {
  deny_heap = true; const auto decoded = c::DecodeCatalogSchedulerFairnessProfile(bytes); deny_heap = false;
  Check(!decoded.ok() && !decoded.record,"invalid fairness has no partial output");
}
void FairnessBad(const Fairness& p) {
  Check(!c::EncodeCatalogSchedulerFairnessProfile(p).ok(),"invalid fairness definition refused");
  FairnessRefused(FairnessOracle(p));
}
void FairnessDefinition() {
  const auto base = FairnessExample(); FairnessGood(base);
  auto p = base; p.emergency_override_allowed = true; FairnessBad(p);
  p.emergency_evidence_policy_uuid = Id(UuidKind::object,71).value; FairnessGood(p);
  p.emergency_override_allowed = false; FairnessGood(p);
  p.emergency_evidence_policy_uuid = Uuid{}; FairnessBad(p);
  p = base; p.origin_transaction_uuid.kind = UuidKind::object;
  Check(!c::EncodeCatalogSchedulerFairnessProfile(p).ok(),"fairness transaction kind cannot be coerced");
  for (const auto member : {&Fairness::generation,&Fairness::origin_local_transaction_id,&Fairness::aging_interval_us}) {
    p = base; p.*member = 0; FairnessBad(p);
    p.*member = std::numeric_limits<u64>::max(); FairnessGood(p);
  }
  for (const auto member : {&Fairness::per_target_max_concurrent,&Fairness::foreground_reserved_worker_slots,
                            &Fairness::maintenance_reserved_worker_slots})
    for (const u64 value : {u64{0},u64{1},std::numeric_limits<u64>::max()}) {
      p = base; p.*member = value; FairnessGood(p);
    }
  for (std::size_t i = 0; i < 24; ++i)
    for (const u64 limit : {u64{0},u64{1},std::numeric_limits<u64>::max()}) {
      p = base; p.family_max_concurrent[i] = limit; FairnessGood(p);
    }
  for (std::size_t i = 0; i < 9; ++i) {
    p = base; p.base_priority_rank[i] = p.aging_rank_ceiling+1; FairnessBad(p);
    p.base_priority_rank[i] = p.aging_rank_ceiling; FairnessGood(p);
    p.aging_rank_ceiling = std::numeric_limits<u64>::max(); p.base_priority_rank[i] = p.aging_rank_ceiling; FairnessGood(p);
  }
  p = base; p.aging_rank_ceiling = 0; p.base_priority_rank.fill(0); FairnessGood(p);
  for (std::size_t i = 0; i < 12; ++i)
    for (const bool indefinite : {false,true})
      for (const u64 wait : {u64{0},u64{1},std::numeric_limits<u64>::max()}) {
        p = base; p.queue_waits[i] = {wait,indefinite}; FairnessGood(p);
      }
  for (const bool evidence : {false,true}) {
    p = base; if (evidence) p.emergency_evidence_policy_uuid = Id(UuidKind::object,71).value;
    const auto original = FairnessOracle(p);
    for (std::size_t n = 0; n < original.size(); ++n) FairnessRefused(std::string_view(original).substr(0,n));
    auto b = original; b.push_back(0); FairnessRefused(b);
    for (const auto offset : {0u,4u,6u,8u,12u,16u,20u,22u}) { b = original; b[offset] ^= 0x40; FairnessRefused(b); }
    std::size_t previous = 0;
    for (std::size_t at = 24; at < original.size();) {
      const auto type = original[at+2]; const std::size_t length = type == 5 ? 16 : (type == 2 ? 1 : 8);
      for (const auto offset : {0u,2u,3u,4u}) { b = original; b[at+offset] ^= 0x80; FairnessRefused(b); }
      b = original; Put(b,at,13,2); FairnessRefused(b);
      if (previous) { b = original; b[at] = original[previous]; b[at+1] = original[previous+1]; FairnessRefused(b); }
      b = original; b.erase(at,length+8); Put(b,8,b.size(),4); Put(b,12,evidence ? 68 : 67,4);
      if (original[at] == 12) Check(c::DecodeCatalogSchedulerFairnessProfile(b).ok(),"optional evidence omission admitted when disabled");
      else FairnessRefused(b);
      if (type == 2) { b = original; b[at+8] = 2; FairnessRefused(b); }
      if (type == 5) for (unsigned mode = 0; mode < 3; ++mode) {
        b = original;
        if (mode == 0) std::fill_n(b.begin()+at+8,16,0);
        if (mode == 1) b[at+8+6] = 0x40;
        if (mode == 2) b[at+8+8] = 0;
        FairnessRefused(b);
      }
      previous = at; at += length+8;
    }
    auto detached = original; const auto copied = c::DecodeCatalogSchedulerFairnessProfile(detached); detached.assign(detached.size(),0);
    Check(copied.ok() && FairnessOracle(*copied.record) == original,"fairness native values outlive source");
    allocation_count = 0; count_allocations = true;
    const auto encoded = c::EncodeCatalogSchedulerFairnessProfile(p); count_allocations = false;
    const auto sites = allocation_count; Check(encoded.ok() && sites != 0,"fairness encoder allocation sites observed");
    for (unsigned site = 0; site < sites; ++site) {
      bool failed = false; allocation_budget = site;
      try { (void)c::EncodeCatalogSchedulerFairnessProfile(p); } catch (const std::bad_alloc&) { failed = true; }
      allocation_budget = -1;
      Check(failed && FairnessOracle(p) == original,"fairness allocation failure atomicity"); FairnessGood(p);
    }
    std::cout << "fairness_encoder_allocation_sites=" << sites << " optional=" << evidence << '\n';
  }
  const auto first = FairnessMetadata(base);
  p = base; p.generation = 2; p.family_max_concurrent[23] = 0;
  auto next = FairnessMetadata(p); next.creator_transaction_uuid = Id(UuidKind::transaction,72); next.creator_local_transaction_id = 2;
  deny_heap = true; const auto preserves = c::CatalogSchedulerFairnessProfilePreservesOrigin(first,next); deny_heap = false;
  Check(preserves,"fairness update preserves original identity");
  for (unsigned field = 0; field < 4; ++field) {
    auto wrong = p;
    if (field == 0) wrong.profile_uuid = Id(UuidKind::object,73).value;
    if (field == 1) wrong.database_uuid = Id(UuidKind::database,74).value;
    if (field == 2) wrong.origin_transaction_uuid = Id(UuidKind::transaction,75);
    if (field == 3) ++wrong.origin_local_transaction_id;
    Check(!c::CatalogSchedulerFairnessProfilePreservesOrigin(first,FairnessMetadata(wrong)),"fairness origin cannot change");
  }
  for (unsigned field = 0; field < 11; ++field) {
    auto wrong = first;
    if (field == 0) wrong.object_subtype = "scheduler_queues";
    if (field == 1) wrong.record.header.kind = c::CatalogRecordKind::config_profile;
    if (field == 2) wrong.record.header.object_uuid = Id(UuidKind::object,32);
    if (field == 3) ++wrong.definition_version;
    if (field == 4) wrong.owning_schema_uuid = Id(UuidKind::schema,33);
    if (field == 5) wrong.default_name_uuid = {};
    if (field == 6) wrong.name_vector_uuid = {};
    if (field == 7) ++wrong.creator_local_transaction_id;
    if (field == 8) wrong.authority_scope = static_cast<c::CatalogAuthorityScope>(255);
    if (field == 9) wrong.record.header.parent_uuid.kind = UuidKind::schema;
    if (field == 10) wrong.creator_transaction_uuid = Id(UuidKind::transaction,76);
    Check(!c::CatalogSchedulerFairnessProfileMatchesMetadata(wrong),"fairness exact envelope binding");
  }
  FairnessRefused(Oracle(Example())); FairnessRefused(QueueOracle(Queues()));
  Refused(FairnessOracle(base)); QueueRefused(FairnessOracle(base));
  Check(heap_attempts == 0,"fairness native inspection does not allocate");
}
void SharedRegistration() {
  const std::array originals{Metadata(Example()),QueueMetadata(Queues()),FairnessMetadata(FairnessExample())};
  auto policy = Example(); policy.generation = 2;
  auto queues = Queues(); queues.generation = 2;
  auto fairness = FairnessExample(); fairness.generation = 2;
  std::array successors{Metadata(policy),QueueMetadata(queues),FairnessMetadata(fairness)};
  const std::array<std::string_view,3> details{"scheduler_runtime_definition_binding_invalid",
      "scheduler_queues_definition_binding_invalid","scheduler_fairness_definition_binding_invalid"};
  const auto invalid_metadata = [&](const auto& m, std::string_view detail) {
    deny_heap = true;
    const auto error = c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(m));
    deny_heap = false;
    Check(error && error->diagnostic_code == "CATALOG.INVALID_INPUT" && error->detail == detail,
          "common metadata exact family diagnostic");
    Check(!c::EncodeCatalogMetadataVersion(m).ok(),"owning common metadata refuses family mismatch");
  };
  for (std::size_t i = 0; i < originals.size(); ++i) {
    const auto& m = originals[i]; auto& next = successors[i];
    next.creator_transaction_uuid = Id(UuidKind::transaction,90); next.creator_local_transaction_id = 2;
    const auto encoded = c::EncodeCatalogMetadataVersion(m);
    Check(encoded.ok(),"registered common metadata accepts each family");
    deny_heap = true;
    const auto view = c::DecodeCatalogMetadataVersionView(encoded.bytes);
    const auto typed = c::ValidateCatalogTypedRecordView(c::BorrowCatalogTypedRecord(m.record));
    const auto error = c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(m));
    const bool origin = c::CatalogMetadataPreservesFamilyOrigin(c::BorrowCatalogMetadataVersion(m),
                                                              c::BorrowCatalogMetadataVersion(next));
    deny_heap = false;
    Check(view.ok() && view.record->record.payload == m.record.payload && typed.ok() && !error && origin,
          "native shared family admission decode and origin without allocation");
    Check(c::DecodeCatalogMetadataVersion(encoded.bytes).ok() && c::EncodeCatalogMetadataVersion(next).ok(),
          "owning compatibility and successor accept registered families");
    auto wrong = m; wrong.object_subtype = "unrelated"; invalid_metadata(wrong,details[i]);
    wrong = m; wrong.record.payload = "opaque unrelated payload"; invalid_metadata(wrong,details[i]);
    wrong = m; wrong.record.header.object_uuid = Id(UuidKind::object,91); invalid_metadata(wrong,details[i]);
    deny_heap = true;
    const auto bad_header = c::ValidateCatalogTypedRecordView(c::BorrowCatalogTypedRecord(wrong.record));
    deny_heap = false;
    Check(!bad_header.ok() && !bad_header.record && bad_header.diagnostic.diagnostic_code == "CATALOG.INVALID_INPUT",
          "common typed header invokes family hook");
    wrong.record.header.row_uuid = {};
    deny_heap = true;
    const auto precedence = c::ValidateCatalogTypedRecordView(c::BorrowCatalogTypedRecord(wrong.record));
    deny_heap = false;
    Check(!precedence.ok() && precedence.diagnostic.diagnostic_code == "SB-CATALOG-RECORD-CODEC-ROW-UUID-MUST-BE-V7",
          "common identity error retains precedence over new family error");
    wrong = m; wrong.object_subtype = "metric_retention";
    invalid_metadata(wrong,"metric_retention_definition_binding_invalid");
    wrong = m; wrong.record.header.kind = c::CatalogRecordKind::storage_descriptor;
    deny_heap = true;
    const auto existing = c::ValidateCatalogTypedRecordView(c::BorrowCatalogTypedRecord(wrong.record));
    deny_heap = false;
    Check(!existing.ok() && existing.diagnostic.detail == "storage_binary_payload_or_header_invalid",
          "existing record-kind family diagnostic retains precedence");
    // Independently alter immutable payload fields while keeping successor
    // metadata valid. The common evolution path, not a direct family helper,
    // must reject each attempted origin substitution.
    for (const unsigned id : {1u,3u,i == 0 ? 10u : 4u,i == 0 ? 11u : 5u}) {
      wrong = next;
      for (std::size_t at = 24; at < wrong.record.payload.size();) {
        const auto field = static_cast<unsigned char>(wrong.record.payload[at]);
        const auto width = static_cast<unsigned char>(wrong.record.payload[at+4]);
        if (field == id) {
          if (width == 16) {
            const auto replacement = Id(UuidKind::object,92).value;
            wrong.record.payload.replace(at+8,16,Identity(replacement));
            if (id == 1) wrong.record.header.object_uuid.value = replacement;
          } else Put(wrong.record.payload,at+8,99,8);
          break;
        }
        at += 8+width;
      }
      Check(c::EncodeCatalogMetadataVersion(wrong).ok(),"origin mutant remains a structurally valid successor");
      deny_heap = true;
      const bool same = c::CatalogMetadataPreservesFamilyOrigin(c::BorrowCatalogMetadataVersion(m),
                                                               c::BorrowCatalogMetadataVersion(wrong));
      deny_heap = false;
      Check(!same && !c::CatalogMetadataPreservesFamilyOrigin(m,wrong),"common family origin substitution refused");
    }
    wrong = next; wrong.object_subtype = "unrelated"; wrong.record.payload = "unrelated";
    Check(!c::CatalogMetadataPreservesFamilyOrigin(m,wrong) && !c::CatalogMetadataPreservesFamilyOrigin(wrong,m),
          "common family cannot morph to or from unrelated definition");
    Check(c::CatalogMetadataPreservesFamilyOrigin(wrong,wrong),"unrelated family origin behavior retained");
    for (std::size_t j = 0; j < originals.size(); ++j) if (j != i) {
      wrong = m; wrong.record.payload = originals[j].record.payload;
      Check(!c::EncodeCatalogMetadataVersion(wrong).ok(),"cross-family payload cannot pass subtype admission");
      Check(!c::CatalogMetadataPreservesFamilyOrigin(m,originals[j]),"cross-family evolution refused");
    }
  }
  Check(heap_attempts == 0,"shared registration adds no native heap allocation");
}
} // namespace
int main(int argc, char** argv) {
  try {
    FieldsAndFraming(); BoundariesAndOrigin(); AllocationFailures(); QueueDefinition(); FairnessDefinition();
    SharedRegistration();
    if (argc == 2 && std::string_view(argv[1]) == "--require-family-registration") {
      auto invalid = Metadata(Example());
      auto invalid_policy = Example(); invalid_policy.generation_warning_threshold = invalid_policy.generation_maximum;
      invalid.record.payload = Oracle(invalid_policy);
      Check(!c::EncodeCatalogMetadataVersion(invalid).ok(),"common codec must invoke scheduler-family admission");
    } else if (argc == 2 && std::string_view(argv[1]) == "--require-queue-registration") {
      auto invalid = Queues(); invalid.queues[11].admission_policy_uuid = {};
      Check(!c::EncodeCatalogMetadataVersion(QueueMetadata(invalid)).ok(),"common codec must invoke queue-profile admission");
    } else if (argc == 2 && std::string_view(argv[1]) == "--require-fairness-registration") {
      auto invalid = FairnessExample(); invalid.emergency_override_allowed = true;
      Check(!c::EncodeCatalogMetadataVersion(FairnessMetadata(invalid)).ok(),"common codec must invoke fairness-profile admission");
    } else if (argc != 1) return 2;
    std::cout << "catalog_scheduler_policy_gate=" << checks << " checks passed; definition codec only\n";
    return 0;
  } catch (const std::exception& error) {
    deny_heap = false; std::cerr << error.what() << '\n'; return 1;
  }
}
