// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime_scheduler_descriptor.hpp"
#include "runtime_permit_fixture.hpp"

namespace r = scratchbird::core::runtime;
using E = r::SchedulerDescriptorError;
using Q = r::QueueDescriptorError;
static_assert(!std::is_copy_constructible_v<r::SchedulerDescriptorRecord>);
static_assert(!std::is_move_constructible_v<r::TaskQueueRecord>);
static_assert(!std::is_default_constructible_v<r::SchedulerDescriptorRecord>);

int main() {
  Fixture f;
  {
    r::SchedulerDescriptorRecord scheduler(*f.metadata);
    r::TaskQueueRecord queue(*f.metadata);
    Check(scheduler.header.lifecycle_state == r::RuntimeSchedulerState::starting &&
          queue.header.lifecycle_state == r::RuntimeQueueState::active, "canonical initial states");
    Check(r::CheckSchedulerDescriptorShape(scheduler) == E::scheduler_invalid &&
          r::CheckTaskQueueShape(queue) == Q::queue_invalid, "unbound constructors do not grant validity");
    scheduler.header.identity = {Id(20), Id(1), Id(21), Id(22), 0};
    scheduler.engine_uuid = Id(23);
    scheduler.database_uuid = Id(24);
    scheduler.scheduler_class = r::RuntimeSchedulerClass::database_local;
    scheduler.header.policy_uuid = Id(25);
    scheduler.resource_profile_uuid = Id(26); scheduler.queue_profile_uuid = Id(27);
    queue.header.identity = {Id(30), Id(1), Id(20), Id(22), 0};
    queue.scheduler_uuid = Id(20); queue.header.policy_uuid = Id(31);
    queue.fairness_profile_uuid = Id(32); queue.queue_name = r::RuntimeQueueName::short_background_queue;
    Check(r::CheckSchedulerDescriptorShape(scheduler) == E::none && r::CheckTaskQueueShape(queue) == Q::none,
          "valid binary descriptor shape with zero generation and capacities");

    for (unsigned value = 0; value < 256; ++value) {
      scheduler.scheduler_class = static_cast<r::RuntimeSchedulerClass>(value);
      scheduler.cluster_uuid = Id(28);
      Check(r::CheckSchedulerDescriptorShape(scheduler, {false, true}) == (value < 6 ? E::none : E::class_invalid),
            "every scheduler class byte qualified");
      scheduler.scheduler_class = r::RuntimeSchedulerClass::database_local;
      scheduler.cluster_uuid.reset();
      scheduler.header.lifecycle_state = static_cast<r::RuntimeSchedulerState>(value);
      Check(r::CheckSchedulerDescriptorShape(scheduler) == (value < 9 ? E::none : E::state_invalid),
            "every scheduler state byte qualified");
      scheduler.header.lifecycle_state = r::RuntimeSchedulerState::starting;
      queue.queue_name = static_cast<r::RuntimeQueueName>(value);
      Check(r::CheckTaskQueueShape(queue) == (value < 12 ? Q::none : Q::name_invalid), "every queue name byte qualified");
      queue.queue_name = r::RuntimeQueueName::short_background_queue;
      queue.header.lifecycle_state = static_cast<r::RuntimeQueueState>(value);
      Check(r::CheckTaskQueueShape(queue) == (value < 5 ? Q::none : Q::state_invalid), "every queue state byte qualified");
      queue.header.lifecycle_state = r::RuntimeQueueState::active;
    }
    for (unsigned kind = 0; kind < 6; ++kind) {
      scheduler.scheduler_class = static_cast<r::RuntimeSchedulerClass>(kind);
      for (bool exists : {false, true}) for (bool present : {false, true}) {
        scheduler.cluster_uuid = present ? std::optional{Id(28)} : std::nullopt;
        const bool invalid = (present && !exists) || ((kind == 2 || kind == 3) && (!exists || !present));
        Check(r::CheckSchedulerDescriptorShape(scheduler, {false, exists}) ==
              (invalid ? E::cluster_path_absent : E::none), "all class/cluster presence combinations");
      }
    }
    scheduler.scheduler_class = r::RuntimeSchedulerClass::offline_tool;
    scheduler.cluster_uuid.reset(); scheduler.database_uuid.reset();
    Check(r::CheckSchedulerDescriptorShape(scheduler) == E::none &&
          r::CheckSchedulerDescriptorShape(scheduler, {true, false}) == E::database_invalid,
          "database absence follows owning profile requirement");
    scheduler.scheduler_class = r::RuntimeSchedulerClass::database_local;
    Check(r::CheckSchedulerDescriptorShape(scheduler) == E::database_invalid, "database-local requires database");
    scheduler.database_uuid = Uuid{};
    Check(r::CheckSchedulerDescriptorShape(scheduler) == E::database_invalid, "present nil database is invalid");
    scheduler.database_uuid = Id(24); scheduler.cluster_uuid = Uuid{};
    Check(r::CheckSchedulerDescriptorShape(scheduler, {false, true}) == E::cluster_path_absent,
          "present nil cluster is not a valid cluster binding");
    scheduler.cluster_uuid.reset();
    const auto bad_id = [] { auto id = Id(50); id.bytes[6] = 0x40; return id; }();
    for (auto invalid : {Uuid{}, bad_id}) {
      for (auto [field, expected] : {std::pair{&r::SchedulerDescriptorRecord::engine_uuid, E::engine_invalid},
          {&r::SchedulerDescriptorRecord::resource_profile_uuid, E::resource_profile_missing},
          {&r::SchedulerDescriptorRecord::queue_profile_uuid, E::queue_profile_missing}}) {
        const auto saved = scheduler.*field; scheduler.*field = invalid;
        Check(r::CheckSchedulerDescriptorShape(scheduler) == expected, "required scheduler identity malformed");
        scheduler.*field = saved;
      }
      for (auto [field, expected] : {std::pair{&r::TaskQueueRecord::scheduler_uuid, Q::scheduler_invalid},
          {&r::TaskQueueRecord::fairness_profile_uuid, Q::fairness_profile_missing}}) {
        const auto saved = queue.*field; queue.*field = invalid;
        Check(r::CheckTaskQueueShape(queue) == expected, "required queue identity malformed");
        queue.*field = saved;
      }
      scheduler.header.policy_uuid = queue.header.policy_uuid = invalid;
      Check(r::CheckSchedulerDescriptorShape(scheduler) == E::policy_missing &&
            r::CheckTaskQueueShape(queue) == Q::admission_policy_missing, "common policy field is authoritative shape binding");
      scheduler.header.policy_uuid = Id(25); queue.header.policy_uuid = Id(31);
    }
    scheduler.scheduler_class.reset(); queue.queue_name.reset();
    Check(r::CheckSchedulerDescriptorShape(scheduler) == E::class_invalid &&
          r::CheckTaskQueueShape(queue) == Q::name_invalid, "required enum absence refused");
    scheduler.scheduler_class = r::RuntimeSchedulerClass::database_local;
    queue.queue_name = r::RuntimeQueueName::short_background_queue;
    scheduler.header.lifecycle_state.reset(); queue.header.lifecycle_state.reset();
    Check(r::CheckSchedulerDescriptorShape(scheduler) == E::state_invalid &&
          r::CheckTaskQueueShape(queue) == Q::state_invalid, "state absence is not default success");
    scheduler.header.lifecycle_state = r::RuntimeSchedulerState::starting;
    queue.header.lifecycle_state = r::RuntimeQueueState::active;

    for (auto value : {std::uint64_t{0}, std::uint64_t{1}, std::numeric_limits<std::uint64_t>::max()}) {
      queue.max_depth = queue.max_concurrent = value;
      Check(r::CheckTaskQueueShape(queue) == Q::none && queue.max_depth == value && queue.max_concurrent == value,
            "full canonical numeric range retained without narrowing or policy invention");
      const r::SchedulerGenerationBinding current{value, value, value};
      Check(r::CompareSchedulerGenerations(current, current) == E::none, "exact generation tuple including zero and maximum");
      for (auto [field, expected] : {std::pair{&r::SchedulerGenerationBinding::startup, E::startup_generation_stale},
          {&r::SchedulerGenerationBinding::recovery, E::recovery_generation_stale},
          {&r::SchedulerGenerationBinding::metrics, E::metrics_generation_stale}}) {
        auto stale = current; stale.*field = value ^ 1;
        Check(r::CompareSchedulerGenerations(stale, current) == expected, "every independent stale generation refused");
      }
    }
    const std::array scheduler_codes{std::string_view{}, std::string_view{"MGA.SCHED.SCHEDULER_INVALID"},
      std::string_view{"MGA.SCHED.ENGINE_INVALID"}, std::string_view{"MGA.SCHED.DATABASE_INVALID"},
      std::string_view{"PROCESS.CLUSTER_PATH_ABSENT"}, std::string_view{"MGA.SCHED.CLASS_INVALID"},
      std::string_view{"MGA.SCHED.POLICY_MISSING"}, std::string_view{"MGA.SCHED.RESOURCE_PROFILE_MISSING"},
      std::string_view{"MGA.SCHED.QUEUE_PROFILE_MISSING"}, std::string_view{"MGA.SCHED.SCHEDULER_STATE_INVALID"},
      std::string_view{"MGA.SCHED.STARTUP_GENERATION_STALE"}, std::string_view{"MGA.SCHED.RECOVERY_GENERATION_STALE"},
      std::string_view{"MGA.SCHED.METRICS_GENERATION_STALE"}};
    for (unsigned i = 0; i < scheduler_codes.size(); ++i)
      Check(r::SchedulerDescriptorDiagnostic(static_cast<E>(i)) == scheduler_codes[i], "exact scheduler diagnostic selection");
    const std::array queue_codes{std::string_view{}, std::string_view{"MGA.SCHED.QUEUE_INVALID"},
      std::string_view{"MGA.SCHED.QUEUE_NAME_INVALID"}, std::string_view{"MGA.SCHED.SCHEDULER_INVALID"},
      std::string_view{"MGA.SCHED.ADMISSION_POLICY_MISSING"}, std::string_view{"MGA.SCHED.FAIRNESS_PROFILE_MISSING"},
      std::string_view{"MGA.SCHED.QUEUE_STATE_INVALID"}};
    for (unsigned i = 0; i < queue_codes.size(); ++i)
      Check(r::QueueDescriptorDiagnostic(static_cast<Q>(i)) == queue_codes[i], "exact queue diagnostic selection");

    scheduler.header.runtime_subtype.assign(128, 's'); queue.header.runtime_subtype.assign(128, 'q');
    Check(f.metadata->Snapshot().allocated_bytes > 0, "descriptor metadata uses real governed backing");
    fault::hit = false; fault::remaining = 0;
    const auto valid_scheduler = r::CheckSchedulerDescriptorShape(scheduler);
    const auto valid_queue = r::CheckTaskQueueShape(queue);
    fault::remaining = -1;
    Check(!fault::hit && valid_scheduler == E::none && valid_queue == Q::none, "shape checks allocate nothing");
    const auto old_size = scheduler.header.runtime_subtype.size();
    fault::remaining = 0; fault::hit = false;
    bool caught = false;
    try { scheduler.header.runtime_subtype.append(1024, 'x'); } catch (const std::bad_alloc&) { caught = true; }
    fault::remaining = -1;
    Check(caught && fault::hit && scheduler.header.runtime_subtype.size() == old_size,
          "actual descriptor allocation failure preserves existing metadata");
  }
  f.Empty();
  std::cout << "PASS scheduler and queue descriptor shapes " << checks << " checks; no admission authority\n";
}
