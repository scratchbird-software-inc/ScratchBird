// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "startup_credential_transport.hpp"
#include "../core/platform/checked_fifo_mutex.hpp"
#include <utility>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::server {
// Process-local generation serialization, not provisioning admission or BEGIN
// authority. Coordinator owns this nonmoving object and joins ALL callers and
// releases ALL leases before destruction; manager outlives them as well.
class StartupCredentialSource {
  static std::uint64_t CurrentProcess() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
  }
 public:
  using Mutex = core::platform::CheckedFifoMutex;
  using Uuid = core::platform::Uuid;
  struct Request {
    Uuid incarnation, task;
    std::uint64_t generation = 0;
    std::optional<Mutex::Clock::time_point> deadline;
    std::stop_token stop;
  };
  enum class Error { none, invalid_request, stale, revoked, transport, wait };
  struct Outcome {
    Error error = Error::invalid_request;
    Mutex::Result wait = Mutex::Result::failed;
    StartupCredentialTransportError transport = StartupCredentialTransportError::none;
    int native_error = 0;
    bool ok() const noexcept { return error == Error::none; }
  };
  class Lease {
   public:
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept
        : source_(TakeSource(other)), task_(other.task_),
          process_(other.process_), thread_(other.thread_), material_(std::move(other.material_)) {}
    Lease& operator=(Lease&&) = delete;
    ~Lease() { Reset(); }
    std::string_view credential() const noexcept {
      CheckOwner();
      return source_ ? material_.credential() : std::string_view{};
    }
    // A value copy of non-secret selectors, only while this private-issued use
    // lease still holds the actual generation lock. Never expose a dangling
    // reference to a later replacement's mutable binding. No source after reset
    // or move: old frame bytes cannot manufacture current-generation evidence.
    std::optional<StartupCredentialTransportBinding> binding() const noexcept {
      CheckOwner();
      return source_ ? std::optional{source_->binding_} : std::nullopt;
    }
    void Reset() noexcept {
      CheckOwner();
      if (!source_) return;
      material_ = {}; // Erase protected material BEFORE allowing revocation.
      if (!source_->mutex_.Unlock(task_.bytes)) std::terminate();
      source_ = nullptr;
    }
   private:
    friend class StartupCredentialSource;
    friend class NativeHostedDatabaseRuntime;
    // Successful engine authentication no longer needs plaintext. Keep the
    // generation lock until its decision retires, but erase protected material
    // before publishing that decision to its owning native host.
    void EraseMaterial() noexcept {CheckOwner();material_={};}
    Lease(StartupCredentialSource& source, Uuid task,
          StartupCredentialTransportResult&& material) noexcept
        : source_(&source), task_(task), process_(source.process_), thread_(std::this_thread::get_id()),
          material_(std::move(material)) {}
    void CheckOwner() const noexcept {
      // Fork can preserve this native thread identity. Check the immutable
      // process first, before secret access, inherited mutex/provider cleanup
      // or any mutable source inspection. Reset/move never erase this check.
      if (process_ != CurrentProcess() || thread_ != std::this_thread::get_id()) std::terminate();
    }
    static StartupCredentialSource* TakeSource(Lease& other) noexcept {
      // Member initialization is ordered: reject a foreign move before reading
      // or transferring custody, just as credential() and Reset() do. A move
      // is not permission to transfer a held source lock to another thread.
      other.CheckOwner();
      return std::exchange(other.source_, nullptr);
    }
    StartupCredentialSource* source_;
    Uuid task_;
    const std::uint64_t process_;
    const std::thread::id thread_;
    StartupCredentialTransportResult material_;
  };
  struct Acquisition {
    Outcome outcome;
    std::optional<Lease> lease;
  };
  StartupCredentialSource(Uuid incarnation, std::uint32_t waiter_limit)
      : incarnation_(incarnation), mutex_(waiter_limit) {}
  StartupCredentialSource(const StartupCredentialSource&) = delete;
  StartupCredentialSource& operator=(const StartupCredentialSource&) = delete;
  ~StartupCredentialSource() {
    CheckProcess();
    const auto state = mutex_.Observe();
    if (state.held || state.waiters || state.calls) std::terminate();
    CloseDescriptor(fd_);
  }
  // This closes admission, NOT issued leases or callers' lifetime references.
  void Close() { CheckProcess(); mutex_.Close(); }

  Outcome Publish(const Request& request, int borrowed_fd,
                  const StartupCredentialTransportBinding& next,
                  core::memory::MemoryManager& manager,
                  const core::memory::MemoryTag& tag) {
    CheckProcess();
    // Parking/import/provider hooks may edit the caller's argument storage.
    // Validate and publish one immutable command and frame binding, rather
    // than rereading aliases after callbacks while retaining the source lock.
    const Request command = request;
    const StartupCredentialTransportBinding next_binding = next;
    auto outcome = Enter(command);
    if (!outcome.ok()) return outcome;
    Unlocker unlock{*this, command.task};
    if (command.generation != binding_.generation ||
        next_binding.generation <= binding_.generation) return Failure(Error::stale);
    if (binding_.generation && !SameIdentity(next_binding)) return Failure(Error::invalid_request);
#if defined(__linux__)
    struct Descriptor {
      int fd;
      ~Descriptor() { CloseDescriptor(fd); }
    } candidate{::fcntl(borrowed_fd, F_DUPFD_CLOEXEC, 0)};
    if (candidate.fd < 0) {
      outcome.error = Error::transport;
      outcome.transport = StartupCredentialTransportError::source_io;
      outcome.native_error = errno;
      return outcome;
    }
    { // Validation material is erased before the candidate becomes current.
      auto imported = ImportStartupCredentialTransport(candidate.fd, next_binding, manager, tag);
      if (!imported.ok()) return TransportFailure(imported);
    }
    const int previous = fd_;
    fd_ = std::exchange(candidate.fd, -1);
    binding_ = next_binding;
    CloseDescriptor(previous);
    return outcome;
#else
    (void)borrowed_fd; (void)manager; (void)tag;
    outcome.error = Error::transport;
    outcome.transport = StartupCredentialTransportError::unsupported_platform;
    return outcome;
#endif
  }
  Outcome Revoke(const Request& request) {
    CheckProcess();
    const Request command = request;
    auto outcome = Enter(command);
    if (!outcome.ok()) return outcome;
    Unlocker unlock{*this, command.task};
    if (!command.generation || command.generation != binding_.generation)
      return Failure(Error::stale);
    CloseDescriptor(std::exchange(fd_, -1));
    return outcome; // Idempotent for this exact tombstone, not an older source.
  }
  Acquisition Acquire(const Request& request, core::memory::MemoryManager& manager,
                      const core::memory::MemoryTag& tag) {
    CheckProcess();
    const Request command = request;
    auto outcome = Enter(command);
    if (!outcome.ok()) return {outcome, {}};
    Unlocker unlock{*this, command.task};
    if (!command.generation || command.generation != binding_.generation)
      return {Failure(Error::stale), {}};
    if (fd_ < 0) return {Failure(Error::revoked), {}};
    auto imported = ImportStartupCredentialTransport(fd_, binding_, manager, tag);
    if (!imported.ok()) return {TransportFailure(imported), {}};
    Lease lease(*this, command.task, std::move(imported));
    unlock.armed = false;
    return {outcome, std::move(lease)};
  }
 private:
  struct Unlocker {
    StartupCredentialSource& source;
    Uuid task;
    bool armed = true;
    ~Unlocker() { if (armed && !source.mutex_.Unlock(task.bytes)) std::terminate(); }
  };
  static void CloseDescriptor(int fd) noexcept {
#if defined(__linux__)
    if (fd >= 0) ::close(fd); // Never retry close: the descriptor may be reused.
#else
    (void)fd;
#endif
  }
  static Outcome Failure(Error error) { return {error, Mutex::Result::acquired}; }
  static Outcome TransportFailure(const StartupCredentialTransportResult& result) {
    return {Error::transport, Mutex::Result::acquired, result.error, result.native_error};
  }
  Outcome Enter(const Request& request) {
    CheckProcess();
    if (!core::uuid::IsEngineIdentityUuid(incarnation_) ||
        !core::uuid::IsEngineIdentityUuid(request.task)) return {};
    if (request.incarnation.bytes != incarnation_.bytes)
      return {Error::stale, Mutex::Result::failed}; // No native grant was attempted.
    try {
      const auto wait = mutex_.Lock(request.deadline, request.stop, request.task.bytes);
      return {wait == Mutex::Result::acquired ? Error::none : Error::wait, wait};
    } catch (const std::system_error&) {
      return {Error::wait, Mutex::Result::failed};
    }
  }
  bool SameIdentity(const StartupCredentialTransportBinding& next) const noexcept {
    return next.database.bytes == binding_.database.bytes &&
           next.principal.bytes == binding_.principal.bytes &&
           next.provider.bytes == binding_.provider.bytes &&
           next.credential_reference.bytes == binding_.credential_reference.bytes &&
           next.custodian_uid == binding_.custodian_uid;
  }
  void CheckProcess() const noexcept {
    // No ordinary source result or destructor can safely enter inherited
    // synchronization or manager state. A child must acquire a fresh source
    // in its own process; inherited credential authority is never reusable.
    if (process_ != CurrentProcess()) std::terminate();
  }
  const std::uint64_t process_ = CurrentProcess();
  const Uuid incarnation_;
  Mutex mutex_;
  StartupCredentialTransportBinding binding_;
  int fd_ = -1;
};
} // namespace scratchbird::server
