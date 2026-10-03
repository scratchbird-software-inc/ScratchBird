// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "startup_credential_transport.hpp"
#include "../core/platform/checked_fifo_mutex.hpp"
#include <utility>

namespace scratchbird::server {
// Process-local generation serialization, not provisioning admission or BEGIN
// authority. Coordinator owns this nonmoving object and joins ALL callers and
// releases ALL leases before destruction; manager outlives them as well.
class StartupCredentialSource {
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
        : source_(std::exchange(other.source_, nullptr)), task_(other.task_),
          thread_(other.thread_), material_(std::move(other.material_)) {}
    Lease& operator=(Lease&&) = delete;
    ~Lease() { Reset(); }
    std::string_view credential() const noexcept {
      CheckThread();
      return source_ ? material_.credential() : std::string_view{};
    }
    void Reset() noexcept {
      if (!source_) return;
      CheckThread();
      material_ = {}; // Erase protected material BEFORE allowing revocation.
      if (!source_->mutex_.Unlock(task_.bytes)) std::terminate();
      source_ = nullptr;
    }
   private:
    friend class StartupCredentialSource;
    Lease(StartupCredentialSource& source, Uuid task,
          StartupCredentialTransportResult&& material) noexcept
        : source_(&source), task_(task), thread_(std::this_thread::get_id()),
          material_(std::move(material)) {}
    void CheckThread() const noexcept {
      if (source_ && thread_ != std::this_thread::get_id()) std::terminate();
    }
    StartupCredentialSource* source_;
    Uuid task_;
    std::thread::id thread_;
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
    const auto state = mutex_.Observe();
    if (state.held || state.waiters || state.calls) std::terminate();
    CloseDescriptor(fd_);
  }
  // This closes admission, NOT issued leases or callers' lifetime references.
  void Close() { mutex_.Close(); }

  Outcome Publish(const Request& request, int borrowed_fd,
                  const StartupCredentialTransportBinding& next,
                  core::memory::MemoryManager& manager,
                  const core::memory::MemoryTag& tag) {
    auto outcome = Enter(request);
    if (!outcome.ok()) return outcome;
    Unlocker unlock{*this, request.task};
    if (request.generation != binding_.generation ||
        next.generation <= binding_.generation) return Failure(Error::stale);
    if (binding_.generation && !SameIdentity(next)) return Failure(Error::invalid_request);
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
      auto imported = ImportStartupCredentialTransport(candidate.fd, next, manager, tag);
      if (!imported.ok()) return TransportFailure(imported);
    }
    const int previous = fd_;
    fd_ = std::exchange(candidate.fd, -1);
    binding_ = next;
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
    auto outcome = Enter(request);
    if (!outcome.ok()) return outcome;
    Unlocker unlock{*this, request.task};
    if (!request.generation || request.generation != binding_.generation)
      return Failure(Error::stale);
    CloseDescriptor(std::exchange(fd_, -1));
    return outcome; // Idempotent for this exact tombstone, not an older source.
  }
  Acquisition Acquire(const Request& request, core::memory::MemoryManager& manager,
                      const core::memory::MemoryTag& tag) {
    auto outcome = Enter(request);
    if (!outcome.ok()) return {outcome, {}};
    Unlocker unlock{*this, request.task};
    if (!request.generation || request.generation != binding_.generation)
      return {Failure(Error::stale), {}};
    if (fd_ < 0) return {Failure(Error::revoked), {}};
    auto imported = ImportStartupCredentialTransport(fd_, binding_, manager, tag);
    if (!imported.ok()) return {TransportFailure(imported), {}};
    Lease lease(*this, request.task, std::move(imported));
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
  const Uuid incarnation_;
  Mutex mutex_;
  StartupCredentialTransportBinding binding_;
  int fd_ = -1;
};
} // namespace scratchbird::server
