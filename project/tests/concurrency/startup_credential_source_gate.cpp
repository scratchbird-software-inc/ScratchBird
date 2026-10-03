// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/startup_credential_source.hpp"
#include <sys/mman.h>
#include <atomic>
#include <iostream>
#include <semaphore>
#include <thread>
namespace srv = scratchbird::server;
namespace mem = scratchbird::core::memory;
using Source = srv::StartupCredentialSource;
namespace {
unsigned checks = 0, failures = 0;
std::atomic<bool> watch_park{false};
std::binary_semaphore parked{0};
std::atomic<void*> secret{nullptr};
std::atomic<unsigned> erased{0};
std::atomic<bool> clean{true}, deny_lock{false};
void Check(bool pass, const char* label) {
  ++checks;
  if (!pass) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
Source::Uuid Id(unsigned n) {
  Source::Uuid id;
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80; id.bytes[15] = n;
  return id;
}
struct Fd {
  int fd;
  ~Fd() { if (fd >= 0) ::close(fd); }
};
int Sealed(const srv::StartupCredentialTransportBinding& binding) {
  std::array<unsigned char, 95> bytes{};
  std::memcpy(bytes.data(), "SBACRED1", 8);
  const std::array ids{binding.database, binding.principal, binding.provider, binding.credential_reference};
  for (size_t i = 0; i < ids.size(); ++i)
    std::memcpy(bytes.data() + 8 + 16*i, ids[i].bytes.data(), 16);
  auto generation = binding.generation;
  for (size_t i = 80; i > 72;) { bytes[--i] = generation & 255; generation >>= 8; }
  bytes[83] = 1; bytes[87] = 7;
  std::memcpy(bytes.data() + 88, "fixture", 7);
  const int fd = ::memfd_create("source-gate", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0 || ::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()) ||
      ::fchmod(fd, 0400) || ::fcntl(fd, F_ADD_SEALS,
        F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) std::terminate();
  return fd;
}
}
extern "C" int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" int __real_munlock(const void*, size_t);
extern "C" int __real_mlock(const void*, size_t);
extern "C" ssize_t __wrap_pread(int fd, void* p, size_t n, off_t at) {
  if (at == 88) secret = p;
  return __real_pread(fd, p, n, at);
}
extern "C" int __wrap_munlock(const void* p, size_t n) {
  if (secret.load() == p) {
    for (size_t i = 0; i < n; ++i)
      if (static_cast<const unsigned char*>(p)[i]) clean = false;
    secret = nullptr; ++erased;
  }
  return __real_munlock(p, n);
}
extern "C" int __wrap_mlock(const void* p, size_t n) {
  if (deny_lock.exchange(false)) { errno = EPERM; return -1; }
  return __real_mlock(p, n);
}
extern "C" int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
  if (watch_park.exchange(false)) parked.release();
  return __real_pthread_cond_timedwait(c, m, t);
}
int main() {
  const auto page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.hard_limit_bytes = page * 2; policy.per_context_limit_bytes = page * 2;
  policy.soft_limit_bytes = 0;
  mem::MemoryManager manager(policy);
  srv::StartupCredentialTransportBinding binding{Id(1), Id(2), Id(3), Id(4), 7, ::geteuid()};
  mem::MemoryTag tag;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::context] = binding.database.bytes;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::owner] = binding.principal.bytes;
  tag.binary_ownership[mem::MemoryBinaryScopeKind::database] = binding.database.bytes;
  Fd first{Sealed(binding)};
  Source source(Id(5), 4);
  Source::Request request{Id(5), Id(6), 0, {}, {}};
  Check(source.Publish(request, first.fd, binding, manager, tag).ok(), "initial publication");
  Check(manager.Snapshot().current_bytes == 0, "publication erases validation copy");
  request.generation = 7;
  auto wrong = request; wrong.incarnation = Id(9);
  Check(source.Acquire(wrong, manager, tag).outcome.error == Source::Error::stale, "wrong incarnation");
  wrong = request; wrong.generation = 6;
  Check(source.Acquire(wrong, manager, tag).outcome.error == Source::Error::stale, "wrong generation");
  auto next = binding; next.generation = 8;
  Check(source.Publish(request, first.fd, next, manager, tag).error == Source::Error::transport,
        "wrong framed generation refuses replacement");
  {
    auto held = source.Acquire(request, manager, tag);
    Check(held.outcome.ok() && held.lease && held.lease->credential() == "fixture", "old source survives failed replacement");
    Check(source.Revoke(request).wait == Source::Mutex::Result::recursive, "recursive revocation refused");
    Source::Outcome revoked;
    std::atomic<bool> completed{false};
    auto competitor = request; competitor.task = Id(7);
    competitor.deadline = Source::Mutex::Clock::now() + std::chrono::seconds(10);
    watch_park = true;
    bool erased_before_revoke = false;
    const auto before = erased.load();
    std::thread worker([&] {
      revoked = source.Revoke(competitor);
      erased_before_revoke = erased.load() > before && clean.load();
      completed = true;
    });
    Check(parked.try_acquire_for(std::chrono::seconds(5)), "revocation enters actual native wait");
    Check(!completed.load(), "revocation cannot complete while lease retained");
    held.lease.reset();
    worker.join();
    Check(revoked.ok() && completed, "revocation completes after lease release");
    Check(erased_before_revoke, "actual zeroization precedes revocation completion");
  }
  Check(manager.Snapshot().current_bytes == 0, "lease release clears governed secret allocation");
  Check(source.Acquire(request, manager, tag).outcome.error == Source::Error::revoked, "revoked source refuses use");
  Check(srv::ImportStartupCredentialTransport(first.fd, binding, manager, tag).ok(), "old immutable bytes still exist without current authority");
  Check(source.Publish(request, first.fd, binding, manager, tag).error == Source::Error::stale, "tombstone prevents same generation replay");
  Fd second{Sealed(next)};
  Check(source.Publish(request, second.fd, next, manager, tag).ok(), "higher generation reinstallation");
  Check(source.Revoke(request).error == Source::Error::stale, "old revoker cannot revoke replacement");
  request.generation = 8;
  ::close(second.fd); second.fd = -1;
  Check(source.Acquire(request, manager, tag).outcome.ok(), "owner retains its own descriptor");
  next.generation = 9;
  Fd third{Sealed(next)};
  deny_lock = true;
  const auto failed = source.Publish(request, third.fd, next, manager, tag);
  Check(failed.error == Source::Error::transport &&
        failed.transport == srv::StartupCredentialTransportError::protected_memory_unavailable,
        "replacement refuses native protection failure");
  Check(source.Acquire(request, manager, tag).outcome.ok(), "protection failure preserves prior generation");
  auto changed_identity = next; changed_identity.principal = Id(9);
  Fd changed{Sealed(changed_identity)};
  Check(source.Publish(request, changed.fd, changed_identity, manager, tag).error == Source::Error::invalid_request,
        "replacement cannot change fixed source identity");
  {
    auto held = source.Acquire(request, manager, tag);
    auto competitor = request; competitor.task = Id(7);
    competitor.deadline = Source::Mutex::Clock::now() + std::chrono::seconds(10);
    Source::Outcome published;
    std::atomic<bool> completed{false};
    watch_park = true;
    std::thread worker([&] {
      published = source.Publish(competitor, third.fd, next, manager, tag);
      completed = true;
    });
    Check(parked.try_acquire_for(std::chrono::seconds(5)), "replacement enters actual native wait");
    Check(!completed.load(), "replacement waits for current use");
    held.lease.reset(); worker.join();
    Check(published.ok(), "replacement commits after prior use ends");
  }
  Check(source.Acquire(request, manager, tag).outcome.error == Source::Error::stale, "replaced generation cannot be reacquired");
  request.generation = 9;
  wrong = request; wrong.deadline = Source::Mutex::Clock::now();
  Check(source.Acquire(wrong, manager, tag).outcome.wait == Source::Mutex::Result::timed_out, "deadline before available grant");
  std::stop_source cancellation;
  cancellation.request_stop(); wrong.stop = cancellation.get_token();
  Check(source.Acquire(wrong, manager, tag).outcome.wait == Source::Mutex::Result::cancelled, "cancelled before expired");
  {
    auto held = source.Acquire(request, manager, tag);
    Check(held.outcome.ok(), "replacement source usable");
    auto competitor = request; competitor.task = Id(7);
    competitor.deadline = Source::Mutex::Clock::now() + std::chrono::seconds(10);
    std::stop_source stop;
    competitor.stop = stop.get_token();
    Source::Outcome stopped;
    watch_park = true;
    std::thread cancelled([&] { stopped = source.Acquire(competitor, manager, tag).outcome; });
    Check(parked.try_acquire_for(std::chrono::seconds(5)), "acquisition parks behind lease");
    stop.request_stop(); cancelled.join();
    Check(stopped.wait == Source::Mutex::Result::cancelled, "parked cancellation does not issue lease");
    competitor.stop = {};
    watch_park = true;
    std::thread closed([&] { stopped = source.Acquire(competitor, manager, tag).outcome; });
    Check(parked.try_acquire_for(std::chrono::seconds(5)), "second acquisition parks behind lease");
    source.Close();
    closed.join();
    Check(stopped.wait == Source::Mutex::Result::closed, "close wakes parked acquisition without grant");
    Check(held.lease && held.lease->credential() == "fixture", "close preserves issued lease");
  }
  Check(source.Acquire(wrong, manager, tag).outcome.wait == Source::Mutex::Result::closed, "close before cancellation");
  Check(manager.Snapshot().current_bytes == 0, "all protected memory released");
  Check(clean.load() && !secret.load(), "all imported secrets erased before native unlock");
  {
    Source bounded(Id(8), 0);
    auto initial = request; initial.incarnation = Id(8); initial.generation = 0;
    auto maximum = binding; maximum.generation = UINT64_MAX;
    Fd last{Sealed(maximum)};
    Check(bounded.Publish(initial, last.fd, maximum, manager, tag).ok(), "explicit maximum generation can be published");
    initial.generation = UINT64_MAX;
    auto held = bounded.Acquire(initial, manager, tag);
    Check(held.outcome.ok(), "zero queue budget permits uncontended use");
    auto competitor = initial; competitor.task = Id(7);
    Source::Outcome exhausted;
    std::thread worker([&] { exhausted = bounded.Acquire(competitor, manager, tag).outcome; });
    worker.join();
    Check(exhausted.wait == Source::Mutex::Result::exhausted, "bounded queue refuses excess waiter");
    held.lease.reset();
    Check(bounded.Revoke(initial).ok(), "maximum generation revocation");
    Check(bounded.Publish(initial, first.fd, binding, manager, tag).error == Source::Error::stale,
          "generation exhaustion never wraps to lower source generation");
  }
  Check(manager.Snapshot().active_allocation_count == 0, "all owner tests release protected allocations");
  std::cout << checks << " source checks; failures=" << failures << '\n';
  return failures ? 1 : 0;
}
