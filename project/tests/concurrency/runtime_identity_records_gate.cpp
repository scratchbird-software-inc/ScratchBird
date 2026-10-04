// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime_identity_records.hpp"
#include "runtime_task_state.hpp"
#include "runtime_permit_fixture.hpp"
#include <cstring>

namespace r = scratchbird::core::runtime;
using State = scratchbird::core::concurrency::RuntimeTaskState;
using Header = r::CdeRuntimeObjectHeader<State>;
using E = r::RuntimeIdentityShapeError;
static_assert(sizeof(Uuid) == 16);
static_assert(std::is_trivially_copyable_v<r::RuntimeTaskIdentityRecord>);
static_assert(std::is_trivially_copyable_v<r::RuntimeWorkerIdentityRecord>);
static_assert(!std::is_default_constructible_v<Header>);
static_assert(!std::is_copy_constructible_v<Header>);
static_assert(!std::is_move_constructible_v<Header>);
static_assert(!std::is_constructible_v<Header, std::pmr::memory_resource&>);

int main() {
  Fixture fixture(false);
  auto issued = [&] {
    const auto result = fixture.issuer.Issue(u::UuidKind::object);
    Check(result.ok(), "actual owning standalone UUID issuer");
    return result.value->value;
  };
  const auto node = issued(), open = issued(), root = issued(), scheduler = issued();
  const r::RuntimeIdentityGeneration manager{issued(), node, root, open, 7};
  const r::RuntimeTaskIdentityRecord task{{issued(), node, scheduler, open, 9}, scheduler};
  const r::RuntimeWorkerIdentityRecord worker{{issued(), node, manager.runtime_object_uuid, open, 11},
                                              manager, task};
  Check(r::CheckTaskIdentityShape(task) == E::none, "binary task record");
  Check(r::CheckWorkerIdentityShape(worker) == E::none, "separate worker manager task");
  Check(r::WorkerMatchesCurrentIdentities(worker, worker.identity, manager, task), "exact match");
  Check(!r::NodeChildIdentityShape({}), "empty record is not a node child");
  Check(r::CheckTaskIdentityShape({}) == E::invalid_task, "empty task");
  Check(r::CheckWorkerIdentityShape({}) == E::invalid_worker, "empty worker");

  // Every byte of every identity, plus every generation, participates in the
  // stale-key comparison. Mutating observations cannot change the original.
  const auto unchanged = worker;
  for (unsigned projection = 0; projection != 3; ++projection) {
    for (auto field : {&r::RuntimeIdentityGeneration::runtime_object_uuid,
                       &r::RuntimeIdentityGeneration::owner_node_uuid,
                       &r::RuntimeIdentityGeneration::parent_runtime_uuid,
                       &r::RuntimeIdentityGeneration::open_generation_uuid}) {
      auto missing = worker;
      auto& missing_target = projection == 0 ? missing.identity :
                             projection == 1 ? missing.manager : missing.task.identity;
      missing_target.*field = {};
      Check(!r::NodeChildIdentityShape(missing_target), "child profile checks every required UUID");
      Check(r::CheckWorkerIdentityShape(missing) != E::none,
            "every required child identity rejects nil");
      for (unsigned byte = 0; byte != 16; ++byte) {
        auto current = worker;
        auto& target = projection == 0 ? current.identity :
                       projection == 1 ? current.manager : current.task.identity;
        (target.*field).bytes[byte] ^= 1;
        Check(!r::WorkerMatchesCurrentIdentities(worker, current.identity, current.manager, current.task),
              "every identity byte fences stale snapshot");
      }
    }
    for (const auto generation : {std::uint64_t{0}, std::uint64_t{1},
                                  std::numeric_limits<std::uint64_t>::max()}) {
      auto current = worker;
      auto& target = projection == 0 ? current.identity :
                     projection == 1 ? current.manager : current.task.identity;
      target.runtime_generation = generation;
      Check(!r::WorkerMatchesCurrentIdentities(worker, current.identity, current.manager, current.task),
            "generation equality not wrap or ordering");
    }
  }
  for (unsigned byte = 0; byte != 16; ++byte) {
    auto current = task;
    current.scheduler_uuid.bytes[byte] ^= 1;
    Check(!r::WorkerMatchesCurrentIdentities(worker, worker.identity, manager, current),
          "scheduler binding participates in exact comparison");
  }
  Check(worker == unchanged, "all negative observations leave original unchanged");
  auto bad = worker;
  bad.identity.parent_runtime_uuid = root;
  Check(r::CheckWorkerIdentityShape(bad) == E::wrong_parent, "single L3 parent binding");
  bad = worker; bad.task.identity.owner_node_uuid = root;
  Check(r::CheckWorkerIdentityShape(bad) == E::wrong_node, "cross-node task rejected");
  bad = worker; bad.manager.open_generation_uuid = root;
  Check(r::CheckWorkerIdentityShape(bad) == E::wrong_open_generation, "cross-open manager rejected");
  bad = worker; bad.identity.runtime_object_uuid = task.identity.runtime_object_uuid;
  Check(r::CheckWorkerIdentityShape(bad) == E::aliased_identity, "worker cannot be task");
  bad = worker; bad.manager.runtime_object_uuid = task.identity.runtime_object_uuid;
  Check(r::CheckWorkerIdentityShape(bad) == E::aliased_identity, "manager cannot be task");
  bad = worker; bad.task.scheduler_uuid = bad.task.identity.runtime_object_uuid;
  Check(r::CheckWorkerIdentityShape(bad) == E::aliased_identity, "scheduler cannot be task");
  bad = worker; bad.identity.runtime_object_uuid = manager.runtime_object_uuid;
  Check(r::CheckWorkerIdentityShape(bad) == E::invalid_worker, "worker cannot be its manager");
  bad = worker; bad.manager = {};
  Check(r::CheckWorkerIdentityShape(bad) == E::invalid_manager, "missing manager");
  bad = worker; bad.task.scheduler_uuid = {};
  Check(r::CheckWorkerIdentityShape(bad) == E::invalid_scheduler, "missing scheduler");
  bad = worker; std::memset(bad.task.identity.runtime_object_uuid.bytes.data(), 'a', 16);
  Check(r::CheckWorkerIdentityShape(bad) == E::invalid_task, "ASCII bytes not engine identity");
  bad = worker; bad.task.identity.runtime_object_uuid.bytes[6] = 0x40;
  Check(r::CheckWorkerIdentityShape(bad) == E::invalid_task, "compatibility v4 not engine identity");
  bad = worker; bad.task.identity.runtime_object_uuid.bytes[8] = 0;
  Check(r::CheckWorkerIdentityShape(bad) == E::invalid_task, "invalid variant");

  // Reference storage uses real governed memory, never the process default PMR.
  struct RestoreDefault {
    std::pmr::memory_resource* previous = std::pmr::set_default_resource(std::pmr::null_memory_resource());
    ~RestoreDefault() { std::pmr::set_default_resource(previous); }
  } restore;
  {
    Header header(*fixture.metadata);
    Check(!header.runtime_kind && !header.lifecycle_state && !header.dependency_generation_hash,
          "unbound schema does not invent admission metadata");
    Check(!r::HeaderMatchesIdentity(header, task.identity), "unbound header mismatch");
    header.identity = task.identity;
    header.runtime_kind = r::RuntimeObjectFamily::job;
    header.runtime_subtype = "statistics_refresh_task";
    header.lifecycle_state = State::created;
    header.policy_uuid = issued(); header.policy_generation = 13;
    header.security_principal_uuid = issued(); header.resource_domain_uuid = issued();
    header.authority_refs.push_back(issued()); header.resource_grant_refs.push_back(issued());
    header.evidence_uuid = issued();
    header.dependency_generation_hash = r::RuntimeDependencyHash{};
    Check(r::HeaderMatchesIdentity(header, task.identity), "common identity bound without authority claim");
    Check(header.authority_refs.get_allocator().resource() == fixture.metadata.get() &&
          header.resource_grant_refs.get_allocator().resource() == fixture.metadata.get() &&
          header.runtime_subtype.get_allocator().resource() == fixture.metadata.get(), "exact governed resource");
    Check(fixture.resource->Snapshot().allocated_bytes > 0, "actual charged container backing");
    const auto saved = header.authority_refs.front();
    const auto charge = fixture.resource->Snapshot().allocated_bytes;
    bool rejected = false;
    try { header.authority_refs.reserve(65536); } catch (const std::bad_alloc&) { rejected = true; }
    Check(rejected && header.authority_refs.size() == 1 && header.authority_refs.front() == saved,
          "over-budget growth preserves prior references");
    Check(fixture.resource->Snapshot().allocated_bytes == charge, "failed growth leaves no extra charge");
    fault::remaining = 0; fault::hit = false;
    rejected = false;
    try { header.authority_refs.reserve(header.authority_refs.capacity() + 32); }
    catch (const std::bad_alloc&) { rejected = true; }
    fault::remaining = -1;
    Check(fault::hit && rejected && header.authority_refs.size() == 1 &&
          header.authority_refs.front() == saved, "allocation fault preserves original binary references");
    Check(fixture.resource->Snapshot().allocated_bytes == charge, "allocation fault leaks no governed charge");
    // The memory API requires caller-ordered quiescence; ReleaseNoAlloc is not a
    // retained-owner fence. Keep the real owner alive until all containers die.
    Check(fixture.resource->active(), "parent remains active through metadata lifetime");
    auto stale = task.identity; ++stale.runtime_generation;
    Check(!r::HeaderMatchesIdentity(header, stale), "header generation binds exactly");
    Check(header.lifecycle_state == State::created && header.evidence_uuid != Uuid{},
          "checking identity does not advance lifecycle or rewrite evidence");
  }
  fixture.Empty();
  std::cout << "PASS runtime identity records " << checks << " checks; shape only, not admission\n";
}
