// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/database_ownership.hpp"
#include "../support/owned_temp_directory.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void (*allocation_hook)() = nullptr;
}
void* operator new(std::size_t bytes) {
  if (auto hook = allocation_hook) { allocation_hook = nullptr; hook(); }
  if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
namespace server = scratchbird::server;
namespace disk = scratchbird::storage::disk;
server::DatabaseOwnershipLock* active_route = nullptr;
disk::RouteSourceTransitionResult nested;
std::shared_ptr<disk::RouteOwnershipLease> borrower;
std::string route_path;
unsigned operation = 0;
bool observed = false;
void Reenter() {
  switch (operation) {
    case 0: observed = active_route->ObserveWriteCustody() == disk::RouteWriteCustodyState::retained; break;
    case 1: nested = active_route->BeginNativeSourceTransition(); break;
    case 2: active_route->release(); break;
    case 3: borrower = disk::RouteOwnershipLease::Borrow(route_path); break;
  }
}
bool RunChild(const std::filesystem::path& path, unsigned mode) {
  { std::ofstream file(path, std::ios::binary); file << "physical ownership fixture"; if (!file) return false; }
  server::DatabaseOwnershipRequest request; request.database_path = path;
  request.storage_access = disk::RouteStorageAccess::read_write;
  auto route = server::AcquireDatabaseOwnership(request);
  if (!route.acquired || !route.lock) return false;
  active_route = route.lock.get(); operation = mode;
  route_path = server::DatabaseOwnershipLockPath(path).string();
  ::alarm(2); // Turns the old actual registry deadlock into a bounded failure.
  allocation_hook = Reenter;
  auto transition = route.lock->BeginNativeSourceTransition();
  allocation_hook = nullptr; ::alarm(0);
  if (mode == 0) return observed && transition.ok();
  if (mode == 1) return nested.ok() && transition.ok() && nested.transition == transition.transition;
  if (mode == 2) {
    if (transition.ok() || transition.error != disk::RouteSourceTransitionError::withdrawn) return false;
    auto retry = server::AcquireDatabaseOwnership(request);
    return retry.acquired; // Failed final admission leaves no provisional pin.
  }
  const auto state = transition.ok() ? transition.transition->ObserveDrain() : disk::RouteSourceDrainObservation{};
  return borrower && transition.ok() && state.state == disk::RouteSourceDrainState::pending && state.legacy_borrowers == 1 &&
      !disk::RouteOwnershipLease::Borrow(route_path);
}
}
int main() {
  scratchbird::tests::OwnedTempDirectory artifacts;
  unsigned failures = 0;
  // Each child constructs its actual route registry after fork. No inherited
  // mutex/provider is entered; only the owner's allocation is instrumented.
  for (unsigned mode = 0; mode < 4; ++mode) {
    const auto path = artifacts.path() / ("route-" + std::to_string(mode));
    const auto child = ::fork();
    if (!child) {
      try { ::_exit(RunChild(path, mode) ? 0 : 1); } catch (...) { ::_exit(2); }
    }
    int status = 0; pid_t waited;
    do { waited = child > 0 ? ::waitpid(child, &status, 0) : -1; } while (waited < 0 && errno == EINTR);
    const bool passed = child > 0 && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!passed) { ++failures; std::cerr << "route transition allocation mode=" << mode << " status=" << status << '\n'; }
  }
  artifacts.Cleanup();
  std::cout << "route_source_transition_allocation checks=4 failures=" << failures << '\n';
  return failures ? 1 : 0;
}
