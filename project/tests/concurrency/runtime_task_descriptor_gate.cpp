// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime_task_descriptor.hpp"
#include "runtime_permit_fixture.hpp"

namespace r = scratchbird::core::runtime;
using E = r::TaskDescriptorShapeError;
using S = scratchbird::core::concurrency::RuntimeTaskState;
static_assert(!std::is_copy_constructible_v<r::TaskDescriptorRecord>);
static_assert(!std::is_move_constructible_v<r::TaskDescriptorRecord>);
static_assert(!std::is_default_constructible_v<r::TaskDescriptorRecord>);
static_assert(!std::is_constructible_v<r::TaskDescriptorRecord, std::pmr::memory_resource&>);

int main() {
  Fixture fixture(false);
  {
    r::TaskDescriptorRecord task(*fixture.metadata);
    r::TaskDescriptorShapeRequirements requirements;
    auto expect = [&](E error, const char* why) {
      Check(r::CheckTaskDescriptorShape(task, requirements) == error, why);
    };
    expect(E::task_invalid, "default descriptor is not valid");
    task.header.identity = {Id(100), Id(1), Id(101), Id(102), 7};
    task.scheduler_uuid = Id(101);
    expect(E::family_invalid, "family explicitly required");
    task.task_family = r::RuntimeTaskFamily::verification_task;
    expect(E::class_invalid, "class explicitly required");
    task.task_class = r::RuntimeTaskClass::background_normal;
    expect(E::authority_missing, "authority reference required");
    task.authority_uuid = Id(103);
    expect(E::policy_missing, "policy vector required");
    task.policy_uuids.push_back(Id(104));
    expect(E::target_invalid, "target content/hash required");
    task.target_descriptor_hash = r::RuntimeDependencyHash{};
    expect(E::target_invalid, "hash alone cannot stand in for target");
    // Shape fixture bytes only: no claim this is an admitted target packet.
    task.target_descriptor.push_back(std::byte{1});
    expect(E::budget_missing, "budget identity required before waiting");
    task.budget_uuid = Id(105);
    expect(E::budget_missing, "budget class explicitly bound");
    task.resource_budget_class = "fixture-owning-budget-class";
    expect(E::none, "record shape complete; no admission claim");
    Check(task.header.lifecycle_state == S::created && task.priority_class == r::RuntimeTaskPriority::normal,
          "canonical descriptor defaults");
    Check(task.Identity() == r::RuntimeTaskIdentityRecord{task.header.identity, Id(101)},
          "single header identity projection");
    for (unsigned value = 0; value != 256; ++value) {
      task.task_family = static_cast<r::RuntimeTaskFamily>(value);
      expect(value < 24 ? E::none : E::family_invalid, "all family byte values");
    }
    task.task_family = r::RuntimeTaskFamily::verification_task;
    for (unsigned value = 0; value != 256; ++value) {
      task.task_class = static_cast<r::RuntimeTaskClass>(value);
      expect(value < 10 ? E::none : E::class_invalid, "all class byte values");
    }
    task.task_class = r::RuntimeTaskClass::background_normal;
    for (unsigned value = 0; value != 256; ++value) {
      task.priority_class = static_cast<r::RuntimeTaskPriority>(value);
      expect(value < 9 ? E::none : E::priority_invalid, "all priority byte values");
    }
    task.priority_class = r::RuntimeTaskPriority::normal;
    task.queue_uuid = Id(106);
    for (unsigned value = 0; value != 256; ++value) {
      task.header.lifecycle_state = static_cast<S>(value);
      expect(value < 19 ? E::none : E::state_invalid, "all lifecycle byte values");
    }
    task.header.lifecycle_state.reset();
    expect(E::state_invalid, "absent lifecycle");
    task.header.lifecycle_state = S::created;

    // Independently enumerated states which necessarily crossed a queue; other
    // states rely on the owner's retained history, not on a guessed transition.
    const std::array<S, 6> queued_states{S::queued, S::running, S::yielding,
                                        S::throttled, S::paused, S::completed};
    for (unsigned mask = 0; mask != 32; ++mask) {
      requirements = {bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8), bool(mask & 16)};
      task.operation_uuid = Id(107); task.durable_state_uuid = Id(108);
      task.evidence_root_uuid = Id(109); task.idempotency_key_hash = r::RuntimeDependencyHash{};
      task.queue_uuid = Id(106);
      expect(E::none, "all conditional bindings present");
      task.operation_uuid.reset();
      expect(requirements.management_operation_bound ? E::operation_missing : E::none,
             "management operation condition");
      task.operation_uuid = Id(107);
      task.durable_state_uuid.reset();
      expect((requirements.durable_progress_required || requirements.retryable_durable) ?
             E::durable_state_missing : E::none, "durable state condition");
      task.durable_state_uuid = Id(108);
      task.evidence_root_uuid.reset();
      expect(requirements.evidence_required ? E::evidence_missing : E::none, "evidence condition");
      task.evidence_root_uuid = Id(109);
      task.idempotency_key_hash.reset();
      expect(requirements.retryable_durable ? E::idempotency_key_missing : E::none, "idempotency condition");
      task.idempotency_key_hash = r::RuntimeDependencyHash{};
      task.queue_uuid.reset();
      for (unsigned value = 0; value != 19; ++value) {
        const auto state = static_cast<S>(value); task.header.lifecycle_state = state;
        bool requires_queue = requirements.has_been_queued;
        for (const auto queued : queued_states) requires_queue |= state == queued;
        expect(requires_queue ? E::queue_invalid : E::none, "state and historical queue binding");
      }
      task.header.lifecycle_state = S::created;
    }
    requirements = {};
    for (const auto& [field, error] : std::array{
        std::pair{&r::TaskDescriptorRecord::operation_uuid, E::operation_missing},
        std::pair{&r::TaskDescriptorRecord::queue_uuid, E::queue_invalid},
        std::pair{&r::TaskDescriptorRecord::durable_state_uuid, E::durable_state_missing},
        std::pair{&r::TaskDescriptorRecord::evidence_root_uuid, E::evidence_missing}}) {
      for (auto invalid : {Uuid{}, Id(500)}) {
        if (!invalid.is_nil()) invalid.bytes[6] = 0x40;
        task.*field = invalid;
        expect(error, "present optional invalid identity is not absence");
      }
      (task.*field).reset();
    }
    task.policy_uuids.push_back({});
    expect(E::policy_missing, "every policy UUID validated");
    task.policy_uuids.pop_back();
    task.scheduler_uuid = {};
    expect(E::scheduler_invalid, "missing scheduler diagnostic");
    task.scheduler_uuid = Id(101);
    task.header.lifecycle_state = S::waiting_resource;
    task.budget_uuid = {};
    expect(E::budget_missing, "waiting does not excuse absent budget");
    task.budget_uuid = Id(105);
    expect(E::none, "valid binding independent of current grant availability");

    constexpr std::array<std::string_view, 16> diagnostic_codes{
      "", "MGA.SCHED.TASK_INVALID", "MGA.SCHED.SCHEDULER_INVALID", "MGA.SCHED.TASK_FAMILY_INVALID",
      "MGA.SCHED.TASK_CLASS_INVALID", "MGA.SCHED.OPERATION_MISSING", "MGA.SCHED.AUTHORITY_MISSING",
      "MGA.SCHED.TARGET_INVALID", "MGA.SCHED.PRIORITY_INVALID", "MGA.SCHED.BUDGET_MISSING",
      "MGA.SCHED.QUEUE_INVALID", "MGA.SCHED.TASK_STATE_INVALID", "MGA.SCHED.DURABLE_STATE_MISSING",
      "MGA.SCHED.EVIDENCE_MISSING", "MGA.SCHED.IDEMPOTENCY_KEY_MISSING", "MGA.SCHED.POLICY_MISSING"};
    for (unsigned index = 0; index != diagnostic_codes.size(); ++index)
      Check(r::TaskDescriptorShapeDiagnostic(static_cast<E>(index)) == diagnostic_codes[index],
            "exact canonical shape diagnostic code");
    Check(r::TaskDescriptorShapeDiagnostic(static_cast<E>(255)) == "MGA.SCHED.TASK_INVALID",
          "unknown error cannot report success");
    const auto charged = fixture.resource->Snapshot().allocated_bytes;
    fault::remaining = 0; fault::hit = false;
    expect(E::none, "shape validation performs no allocation");
    Check(!fault::hit && fault::remaining == 0, "allocation fault remains unused by shape check");
    fault::remaining = -1;
    Check(fixture.resource->Snapshot().allocated_bytes == charged, "validation does not change accounting");
    Check(task.policy_uuids.get_allocator().resource() == fixture.metadata.get() &&
          task.target_descriptor.get_allocator().resource() == fixture.metadata.get() &&
          task.resource_budget_class.get_allocator().resource() == fixture.metadata.get(),
          "all descriptor variable storage governed");
  }
  fixture.Empty();
  std::cout << "PASS task descriptor " << checks << " checks; shape only, not admission\n";
}
