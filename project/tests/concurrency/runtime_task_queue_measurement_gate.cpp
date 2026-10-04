// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "retained_runtime_task_queue.hpp"
#include "runtime_permit_fixture.hpp"
#include <algorithm>
#include <ctime>

namespace r = scratchbird::core::runtime;
using Q = r::NativeTaskQueueCode;
using S = m::SafeRetirementStatus;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::uint64_t Ns(Clock::duration elapsed) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}
// A failing worker cannot leave peers parked at a measurement barrier. Fail the
// process rather than fabricate progress or run the ordinary success teardown.
void WorkerRequire(bool condition) {
  if (!condition) { std::fputs("FAIL queue measurement ownership\n", stderr); std::_Exit(1); }
}

template<std::size_t Workers, bool Backlog> void Measure() {
  constexpr std::size_t warmup = 32, samples = 256;
  struct Sample { std::uint64_t reserve = 0, publish = 0, residence = 0, cycle = 0; };
  std::array<Sample, Workers * samples> timings{};
  std::array<std::uint64_t, Workers> completions{}, worker_ns{};
  std::array<std::uint64_t, 256> calibration{};
  for (auto& timing : calibration) { const auto start = Clock::now(); timing = Ns(Clock::now() - start); }
  std::sort(calibration.begin(), calibration.end());
  Fixture f(false);
  f.policy.queue_capacity = Workers;
  Check(f.Bind() == Code::bound, "selected actual queue capacity bound before use");
  Clock::time_point cleanup;
  {
    m::MemorySafeRetirement domain(*f.resource, Id(900).bytes, 4, Workers + 2);
    Check(domain.Initialize() == S::ok, "measurement real retirement domain");
    {
      r::RetainedRuntimeTaskQueueOwner owner(domain);
      const r::NativeTaskQueueBinding binding{Id(200), Id(201), Id(1), Id(202), 17,
                                             Workers, f.policy.authority, f.queue};
      Check(owner.Initialize(binding, {Workers}, f.governor, *f.metadata,
                             {Id(901).bytes, Id(999).bytes}) == S::ok, "measurement real retained queue");
      std::latch ready(Workers), go(1);
      std::barrier phase(Workers);
      std::array<std::jthread, Workers> threads;
      try {
        for (std::size_t worker = 0; worker < Workers; ++worker) {
          threads[worker] = std::jthread([&, worker] {
            r::RetainedRuntimeTaskQueueOperation operation;
            WorkerRequire(owner.AcquireOperation(Id(200), Id(202), 17,
                {Id(1000 + worker).bytes, Id(20 + worker).bytes}, operation) == S::ok);
            const auto cycle = [&](std::size_t i, bool measured) {
              auto request = f.Request(false, 20 + worker);
              request.attempt = Id(20000 + worker * (warmup + samples) + i + (measured ? warmup : 0));
              const r::NativeTaskQueueKey key{
                  {{request.task, Id(1), Id(201), Id(202), 19}, Id(201)}, request.attempt, Id(200), 17};
              const auto start = Clock::now();
              auto credit = f.governor.AcquireRuntimePermit(request);
              const auto reserved = Clock::now();
              WorkerRequire(credit.ok() && bool(credit.permit));
              WorkerRequire(operation.Push(key, credit.permit).code == Q::inserted && !credit.permit);
              const auto published = Clock::now();
              if constexpr (Backlog) {
                phase.arrive_and_wait();
                // This point proves a real full backlog, not just N concurrent
                // callers. No thread can remove before every observation ends.
                WorkerRequire(f.governor.Snapshot().active.backlog_items == Workers);
                WorkerRequire(operation.Front().code == Q::observed);
                phase.arrive_and_wait();
              }
              WorkerRequire(operation.Remove(key).code == Q::removed);
              const auto removed = Clock::now();
              if constexpr (Backlog) phase.arrive_and_wait();
              if (measured) {
                timings[worker * samples + i] = {Ns(reserved - start), Ns(published - reserved),
                                                Ns(removed - published), Ns(Clock::now() - start)};
                ++completions[worker];
              }
            };
            for (std::size_t i = 0; i < warmup; ++i) cycle(i, false);
            ready.count_down(); go.wait();
            const auto start = Clock::now();
            for (std::size_t i = 0; i < samples; ++i) cycle(i, true);
            worker_ns[worker] = Ns(Clock::now() - start);
          });
        }
      } catch (...) {
        // A partial launch cannot satisfy Backlog's fixed-participant barrier.
        // No clean result is possible; process exit is an explicit failed gate.
        WorkerRequire(false);
      }
      ready.wait();
      Check(f.governor.Snapshot().active.backlog_items == 0, "warmup releases every real queue credit");
      const auto baseline_bytes = f.manager.Snapshot().current_bytes;
      const auto cpu_start = std::clock(); const auto start = Clock::now();
      go.count_down();
      for (auto& thread : threads) thread.join();
      const auto wall = Ns(Clock::now() - start); const auto cpu_end = std::clock();
      Check(wall > 0 && cpu_start != std::clock_t(-1) && cpu_end != std::clock_t(-1), "valid measured clocks");
      for (auto count : completions) Check(count == samples, "every queue worker completes its finite workload");
      Check(owner.Snapshot().operation_references == 0 && owner.Snapshot().front.code == Q::empty,
            "joined queue operations and empty actual queue");
      Check(f.governor.Snapshot().active.backlog_items == 0 && domain.Snapshot().readers == 1,
            "no residual credits or operation guards");
      for (auto [name, member] : {std::pair{"reserve", &Sample::reserve}, {"publish", &Sample::publish},
                                 {"residence", &Sample::residence}, {"cycle", &Sample::cycle}}) {
        std::array<std::uint64_t, Workers * samples> sorted{};
        std::transform(timings.begin(), timings.end(), sorted.begin(), [member](const auto& value) { return value.*member; });
        std::sort(sorted.begin(), sorted.end());
        std::cout << "measurement=retained_queue profile=" << (Backlog ? "full_backlog" : "roundtrip")
                  << " workers=" << Workers << " stage=" << name << " samples=" << sorted.size()
                  << " p50_ns=" << sorted[(sorted.size()-1)/2] << " p99_ns=" << sorted[(sorted.size()-1)*99/100]
                  << " max_ns=" << sorted.back() << '\n';
      }
      std::cout << "measurement=retained_queue_run profile=" << (Backlog ? "full_backlog" : "roundtrip")
                << " workers=" << Workers << " capacity=" << Workers << " wall_ns=" << wall
                << " process_cpu_ns=" << static_cast<double>(cpu_end-cpu_start)*1000000000.0/CLOCKS_PER_SEC
                << " cycles_per_second=" << timings.size()*1000000000.0/wall
                << " slowest_worker_ns=" << *std::max_element(worker_ns.begin(), worker_ns.end())
                << " baseline_governed_bytes=" << baseline_bytes << " peak_governed_bytes=" << f.manager.Snapshot().peak_bytes
                << " caller_timing_bytes=" << sizeof(timings) << " clock_pair_p50_ns=" << calibration[127]
                << " clock_pair_p99_ns=" << calibration[252] << '\n';
      cleanup = Clock::now();
      Check(owner.Close() && owner.FenceAdmission() && owner.Drain(Clock::now()+10s).drained,
            "measured queue closed fenced and drained");
    }
    Check(domain.Collect() == S::ok && !domain.Snapshot().retained_payload_bytes,
          "measured queue physically collected");
    domain.Close();
    Check(domain.Drain(Clock::now()+10s) == S::ok, "measured actual domain drained");
  }
  f.Empty();
  std::cout << "measurement=retained_queue_cleanup workers=" << Workers << " backlog=" << Backlog
            << " cleanup_ns=" << Ns(Clock::now()-cleanup) << " final_governed_bytes=0\n";
}
int main() {
  std::cout << "measurement_profile=component_only clock=steady_clock cpu_clock=process"
               " warmup_per_worker=32 measured_per_worker=256"
               " includes_real_governor_allocation_uuid_issuance_and_queue_mutex"
               " full_backlog_includes_three_barriers_per_cycle"
               " timing_storage_external no_dispatch_or_fairness_SLO\n";
  Measure<1,false>(); Measure<2,false>(); Measure<4,false>(); Measure<8,false>();
  Measure<1,true>(); Measure<2,true>(); Measure<4,true>(); Measure<8,true>();
  std::cout << "PASS retained task queue measurements " << checks << " checks\n";
}
