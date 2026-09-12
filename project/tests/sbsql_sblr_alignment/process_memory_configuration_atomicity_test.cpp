// SPDX-License-Identifier: MPL-2.0
// Linux process isolation keeps each startup test independent of global state.
#include "memory.hpp"
#include <atomic>
#include <barrier>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
#ifndef SB_PROCESS_MEMORY_NO_FAULTS
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
namespace {
namespace mem = scratchbird::core::memory;
struct Report { unsigned checks = 0, failures = 0, injected = 0; };
Report report;
void Check(bool ok, const char* why) {
  ++report.checks;
  if (!ok) { ++report.failures; std::fprintf(stderr, "FAIL %s\n", why); }
}
mem::AllocationPolicy Policy() {
  auto p = mem::DefaultLocalEngineMemoryPolicy();
  p.policy_name = std::string(128, 'p');
  p.hard_limit_bytes = 4096; p.soft_limit_bytes = 4096;
  p.per_context_limit_bytes = 4096;
  return p;
}
mem::DefaultMemoryManagerConfigurationResult Configure(mem::AllocationPolicy p, std::string name, bool fixture) {
  return fixture ? mem::ConfigureDefaultMemoryManagerForFixture(std::move(p), std::move(name))
                 : mem::ConfigureDefaultMemoryManager(std::move(p), std::move(name));
}
void Startup(long index, int mode) {
  const bool fixture = mode == 1 || mode == 5;
  auto p = Policy();
  std::string name(256, 's');
  if (mode == 2 || mode == 5) name.clear();
  if (mode == 3) p.refuse_all_allocations = true;
  if (mode == 4) {
    p.hard_limit_bytes = 0; p.byte_limit = 0;
  }
  const auto expected = fixture ? "fixture:" + name : name;
  mem::DefaultMemoryManagerConfigurationResult result;
  bool escaped = false;
  fault::remaining = index; fault::hit = false;
  try { result = Configure(std::move(p), std::move(name), fixture); }
  catch (const std::bad_alloc&) { escaped = true; }
  fault::remaining = -1;
  report.injected = fault::hit ? 1 : 0;
  Check(!escaped, "allocation failure escaped configuration");
  auto state = mem::DefaultMemoryManagerState();
  if (fault::hit || mode >= 2) {
    if (fault::hit && !escaped)
      Check(result.status.code == scratchbird::core::platform::StatusCode::memory_allocation_failed,
            "allocation failure lost typed memory status");
    Check(!result.ok() || escaped, "failed startup returned success");
    Check(!state.initialized && !state.explicitly_configured &&
          !state.fixture_mode && state.provenance.empty(),
          "failed startup partially published process manager");
    Check(!result.applied, "failed startup reported applied");
    Check(state.active_policy.refuse_all_allocations, "failed startup changed refusal policy");
    auto retry = Configure(Policy(), std::string(256, 's'), fixture);
    Check(retry.ok() && retry.applied && !retry.already_initialized,
          "failed startup prevented clean retry");
  } else {
    Check(result.ok() && result.applied && !result.already_initialized,
          "clean startup not applied exactly once");
    Check(state.provenance == expected && state.fixture_mode == fixture,
          "published startup provenance/mode mismatch");
  }
  state = mem::DefaultMemoryManagerState();
  Check(state.initialized && state.explicitly_configured &&
        state.active_policy.policy_name == Policy().policy_name &&
        state.active_policy.hard_limit_bytes == 4096, "active policy inconsistent");
  auto* manager = &mem::DefaultMemoryManager();
  mem::MemoryTag tag;
  auto allocation = manager->Allocate(64, alignof(std::max_align_t), tag);
  Check(allocation.ok() && allocation.pointer, "actual configured manager cannot allocate");
  auto replay = Configure(Policy(), std::string(256, 'r'), fixture);
  Check(replay.ok() && replay.already_initialized && !replay.applied,
        "idempotent replay changed initialization");
  Check(mem::DefaultMemoryManagerState().provenance == state.provenance,
        "replay replaced original provenance");
  auto changed = Policy(); changed.hard_limit_bytes = 8192;
  auto refused = Configure(std::move(changed), "changed", fixture);
  Check(!refused.ok() && refused.already_initialized && !refused.applied,
        "different policy did not refuse");
  Check(!Configure(Policy(), "different-mode", !fixture).ok(), "different mode did not refuse");
  Check(&mem::DefaultMemoryManager() == manager, "configuration replaced live manager");
  auto too_large = manager->Allocate(4097, alignof(std::max_align_t), tag);
  Check(!too_large.ok(), "configured hard limit not enforced");
  if (allocation.pointer) Check(manager->Deallocate(allocation.pointer, tag).ok(), "actual allocation release failed");
}
void Existing(long index, int mode) {
  Check(Configure(Policy(), std::string(256, 's'), false).ok(), "initial configuration failed");
  auto* manager = &mem::DefaultMemoryManager();
  mem::MemoryTag tag;
  auto allocation = manager->Allocate(64, alignof(std::max_align_t), tag);
  Check(allocation.ok() && allocation.pointer, "existing-owner allocation failed");
  if (allocation.pointer) *static_cast<unsigned char*>(allocation.pointer) = 0xa5;
  auto p = Policy();
  if (mode == 9) p.hard_limit_bytes = 8192;
  std::string name(256, 'r');
  mem::DefaultMemoryManagerConfigurationResult result;
  bool escaped = false;
  fault::remaining = index; fault::hit = false;
  try { result = Configure(std::move(p), std::move(name), mode == 10); }
  catch (const std::bad_alloc&) { escaped = true; }
  fault::remaining = -1;
  report.injected = fault::hit ? 1 : 0;
  Check(!escaped, "existing configuration allocation failure escaped");
  Check(!result.applied, "existing manager was reapplied");
  if (fault::hit) Check(!result.ok() || escaped, "faulted replay returned success");
  else Check(result.already_initialized && result.ok() == (mode == 8),
             "existing configuration replay/refusal changed");
  const auto state = mem::DefaultMemoryManagerState();
  Check(&mem::DefaultMemoryManager() == manager && state.initialized &&
        state.explicitly_configured && !state.fixture_mode &&
        state.provenance == std::string(256, 's') && state.active_policy.hard_limit_bytes == 4096,
        "existing configuration changed published owner or provenance");
  if (allocation.pointer) {
    Check(*static_cast<unsigned char*>(allocation.pointer) == 0xa5, "live allocation invalidated");
    Check(manager->Deallocate(allocation.pointer, tag).ok(), "existing allocation release failed");
  }
}
void Concurrent(bool distinct) {
  constexpr unsigned count = 8;
  std::barrier gate(count);
  std::atomic<unsigned> applied{0}, okay{0}, invalid{0};
  std::vector<std::thread> workers;
  for (unsigned i = 0; i != count; ++i) workers.emplace_back([&, i] {
    auto p = Policy();
    if (distinct) p.hard_limit_bytes += i;
    auto name = std::string(128, 'a' + i);
    gate.arrive_and_wait();
    auto r = Configure(std::move(p), std::move(name), false);
    if (r.applied) ++applied;
    if (r.ok()) ++okay;
    if (!r.ok() && !r.already_initialized) ++invalid;
  });
  for (auto& t : workers) t.join();
  Check(applied == 1 && okay == (distinct ? 1u : count) && invalid == 0,
        "concurrent startup did not publish exactly one owner");
  const auto state = mem::DefaultMemoryManagerState();
  Check(state.initialized && state.explicitly_configured && state.provenance.size() == 128,
        "concurrent startup published partial state");
  const auto winner = static_cast<unsigned>(state.provenance[0] - 'a');
  Check(winner < count && state.provenance == std::string(128, state.provenance[0]) &&
        state.active_policy.hard_limit_bytes == 4096 + (distinct ? winner : 0),
        "concurrent startup mixed policy/provenance");
}
Report Child(long index, int mode) {
  int fds[2];
  if (::pipe(fds)) std::abort();
  const auto pid = ::fork();
  if (pid < 0) std::abort();
  if (pid == 0) {
    ::close(fds[0]); report = {};
    try {
      if (mode >= 8) Existing(index, mode);
      else if (mode >= 6) Concurrent(mode == 7);
      else Startup(index, mode);
    }
    catch (...) { fault::remaining = -1; Check(false, "unexpected child exception"); }
    const auto n = ::write(fds[1], &report, sizeof report);
    ::close(fds[1]);
    std::exit(n == sizeof report && !report.failures ? 0 : 1);
  }
  ::close(fds[1]);
  Report r{};
  ssize_t n;
  do { n = ::read(fds[0], &r, sizeof r); } while (n < 0 && errno == EINTR);
  ::close(fds[0]);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) if (errno != EINTR) std::abort();
  if (n != sizeof r || !WIFEXITED(status) ||
      (WEXITSTATUS(status) != 0 && r.failures == 0)) ++r.failures;
  return r;
}
}
int main(int argc, char**) {
  Report total;
  auto add = [&](Report r) { total.checks += r.checks; total.failures += r.failures; total.injected += r.injected; };
  if (argc == 1) {
    for (int mode = 0; mode != 11; ++mode) {
      if (mode == 6 || mode == 7) continue;
      bool complete = false;
      for (long index = 0; index != 1024 && !complete; ++index) {
        const auto r = Child(index, mode); add(r); complete = !r.injected;
      }
      if (!complete) ++total.failures;
    }
  }
  add(Child(-1, 6)); add(Child(-1, 7));
  std::printf("process memory startup checks=%u allocation_faults=%u failures=%u\n",
              total.checks, total.injected, total.failures);
  return total.failures ? 1 : 0;
}
