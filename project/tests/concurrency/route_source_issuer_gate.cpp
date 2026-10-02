// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_ownership.hpp"
#include "disk_device.hpp"
#include "route_ownership_lease.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

namespace disk = scratchbird::storage::disk;
namespace server = scratchbird::server;
using Error = disk::RouteSourceTransitionError;
using Drain = disk::RouteSourceDrainState;
thread_local long allocation_budget = -1;
thread_local bool forbid_mutex = false;
void* operator new(std::size_t n) {
  if (allocation_budget >= 0 && allocation_budget-- == 0) throw std::bad_alloc();
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (forbid_mutex) ::_exit(77);
  return __real_pthread_mutex_lock(mutex);
}
unsigned checks = 0;
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) throw std::runtime_error(why);
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    auto pattern = (std::filesystem::temp_directory_path() / "sb_route_issuer.XXXXXX").string();
    auto* created = ::mkdtemp(pattern.data());
    Check(created != nullptr, "isolated issuer fixture"); root = created;
  }
  ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  std::string Seed(const char* name) {
    const auto path = (root / name).string(); disk::FileDevice file;
    const unsigned char value = 0x5a;
    Check(file.Open(path, disk::FileOpenMode::create_new).ok() &&
          file.WriteAt(0, &value, 1).ok() && file.Close().ok(), "real source file");
    return path;
  }
};
auto Own(const std::string& path) {
  server::DatabaseOwnershipRequest request; request.database_path = path;
  auto result = server::AcquireDatabaseOwnership(request);
  Check(result.acquired && result.lock && result.lock->valid(), "actual server ownership");
  return result.lock;
}
void Locks(const std::string& path, bool held) {
  // Probe each actual OS owner lock; a data lock must not mask lost route ownership.
  for (const auto* suffix : {".sb.route.owner.lock", ".sb.owner.lock"}) {
    const int fd = ::open((path + suffix).c_str(), O_RDWR | O_CLOEXEC);
    Check(fd >= 0, "open owner lock probe");
    const int result = ::flock(fd, LOCK_EX | LOCK_NB), error = errno;
    ::close(fd);
    Check(held ? result < 0 && (error == EWOULDBLOCK || error == EAGAIN) : result == 0,
          "actual OS lock retention matches owner lifetime");
  }
}
void IssuanceAndLifetime(Fixture& f) {
  const auto path = f.Seed("main"), alias = path + ".alias";
  std::filesystem::create_hard_link(path, alias);
  auto owner = Own(path);
  // Both fallible allocations (object and shared control block) precede fencing.
  for (long point : {0, 1}) {
    allocation_budget = point;
    const auto failed = owner->BeginNativeSourceTransition();
    const auto consumed = allocation_budget; allocation_budget = -1;
    Check(consumed == -1 && failed.error == Error::resource_exhausted && !failed.transition,
          "issuer allocation failure is typed and publishes no transition");
    auto borrow = disk::RouteOwnershipLease::Borrow(path + ".sb.route.owner.lock");
    Check(borrow && borrow->ObserveLegacyBorrowers() == 1, "failed issuance preserves admission");
  }
  disk::FileDevice reader;
  Check(reader.Open(path, disk::FileOpenMode::open_existing_read_only).ok(), "real active borrower");
  auto issued = owner->BeginNativeSourceTransition();
  Check(issued.ok() && issued.transition->ObserveDrain().state == Drain::pending &&
        issued.transition->ObserveDrain().legacy_borrowers == 1, "issuer preserves active borrower");
  auto repeated = owner->BeginNativeSourceTransition();
  Check(repeated.ok() && repeated.transition == issued.transition, "repeat shares original transition");
  Check(!disk::RouteOwnershipLease::Borrow(path + ".sb.route.owner.lock"), "fence direct borrowing");
  for (const auto& candidate : {path, alias}) {
    disk::FileDevice late;
    Check(!late.Open(candidate, disk::FileOpenMode::open_existing_read_only).ok(), "fence direct and alias opens");
  }
  unsigned char value = 0;
  Check(reader.ReadAt(0, &value, 1).ok() && value == 0x5a, "old borrower remains usable");
  Locks(path, true);
  repeated.transition.reset(); issued.transition.reset();
  allocation_budget = 0;
  const auto failed_retry = owner->BeginNativeSourceTransition(); allocation_budget = -1;
  Check(failed_retry.error == Error::resource_exhausted &&
        !disk::RouteOwnershipLease::Borrow(path + ".sb.route.owner.lock"), "failed observer recreation never reopens fence");
  issued = owner->BeginNativeSourceTransition();
  Check(issued.ok() && issued.transition->ObserveDrain().legacy_borrowers == 1, "recreate observer of same fenced state");
  const auto child = ::fork(); Check(child >= 0, "fork inherited issuer");
  if (child == 0) {
    forbid_mutex = true;
    const auto wrong = owner->BeginNativeSourceTransition();
    const bool refused = wrong.error == Error::wrong_process && !wrong.transition &&
        issued.transition->ObserveDrain().state == Drain::wrong_process;
    owner->release(); issued.transition.reset();
    ::_exit(refused ? 0 : 78);
  }
  int status = 0;
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "inherited issuer and observer refuse before mutex access");
  Locks(path, true);
  Check(reader.Close().ok() && issued.transition->ObserveDrain().state == Drain::drained,
        "actual last borrower close changes observation to drained");
  owner->release();
  Check(owner->BeginNativeSourceTransition().error == Error::invalid_owner &&
        issued.transition->ObserveDrain().state == Drain::withdrawn,
        "release revokes issuance and prior drain observation is not transfer authority");
  Locks(path, true);
  std::thread final_release([retained = std::move(issued.transition)]() mutable { retained.reset(); });
  final_release.join(); Locks(path, false);
  disk::FileDevice reopened;
  Check(reopened.Open(path, disk::FileOpenMode::open_existing_read_only).ok() && reopened.Close().ok(),
        "worker-final transition release permits independent reopen");
}
void MoveOwnership(Fixture& f) {
  const auto a = f.Seed("move-a"), b = f.Seed("move-b");
  auto first = Own(a), second = Own(b);
  auto original = first->BeginNativeSourceTransition(), replaced = second->BeginNativeSourceTransition();
  Check(original.ok() && replaced.ok(), "two independent real issuers");
  server::DatabaseOwnershipLock moved(std::move(*first));
  Check(first->BeginNativeSourceTransition().error == Error::invalid_owner &&
        moved.BeginNativeSourceTransition().transition == original.transition, "move construction transfers issuing lease");
  *second = std::move(moved);
  Check(moved.BeginNativeSourceTransition().error == Error::invalid_owner &&
        second->BeginNativeSourceTransition().transition == original.transition &&
        replaced.transition->ObserveDrain().state == Drain::withdrawn, "move assignment withdraws old owner and transfers new issuer");
  Locks(a, true); Locks(b, true); replaced.transition.reset(); Locks(b, false);
  second->release(); Check(original.transition->ObserveDrain().state == Drain::withdrawn, "moved owner releases actual issuer");
  Locks(a, true); original.transition.reset(); Locks(a, false);
}
int main() {
  try {
    Fixture fixture; IssuanceAndLifetime(fixture); MoveOwnership(fixture);
    std::cout << "route source issuer checks=" << checks << " PASS; issuer only, not source transfer\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
