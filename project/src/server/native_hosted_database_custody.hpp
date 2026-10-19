// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "database_ownership.hpp"
#include "../storage/database/native_owned_checkpoint_writer.hpp"
#include <stop_token>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace scratchbird::server {
enum class NativeHostedCustodyError {
  none, invalid_owner, wrong_process, wrong_thread, busy, route_failure,
  not_mounted, already_mounted, writer_failure
};
enum class NativeHostedCustodyStage { awaiting_transfer, writer_retained, writer_retired };
class NativeHostedDatabaseCustody;
struct NativeHostedCustodyBeginResult;

// Physical custody only, not an activated node, service session or BEGIN
// permit. In particular writer close does NOT release the original route/OS
// owner, complete worker drain or decide transaction finality. The shutdown
// coordinator must separately reconcile every retained owner and close error.
// Issuing memory managers outlive this owner and all its operations/readers.
class NativeHostedDatabaseCustody final {
  using Writer = storage::database::NativeOwnedCheckpointWriter;
  static std::uint64_t CurrentProcess() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
  }
 public:
  using Uuid = core::platform::Uuid;
  using Error = NativeHostedCustodyError;
  class Operation;
  class ReadLease;
  struct OperationResult;
  struct ReadResult;
  NativeHostedDatabaseCustody(const NativeHostedDatabaseCustody&) = delete;
  NativeHostedDatabaseCustody& operator=(const NativeHostedDatabaseCustody&) = delete;
  ~NativeHostedDatabaseCustody() {
    if (InOwningProcess() && busy_.load(std::memory_order_acquire)) std::terminate();
    // In a fork child the underlying native owners perform descriptor-only
    // abandonment. Never inspect inherited control/provider synchronization.
  }
  // Pin the actual initial writable owner before allocation/callbacks. This
  // fences legacy borrowing once, without reopening any diagnostic pathname.
  // Other existing route pins remain the coordinator's explicit responsibility.
  static NativeHostedCustodyBeginResult Begin(std::shared_ptr<DatabaseOwnershipLock>) noexcept;
  struct Observation {
    Error error = Error::invalid_owner;
    NativeHostedCustodyStage stage = NativeHostedCustodyStage::awaiting_transfer;
    Uuid database_uuid, primary_filespace_uuid;
    bool initial_route_owner_retained = false;
  };
  static Observation Observe(std::shared_ptr<NativeHostedDatabaseCustody>) noexcept;
  struct ProbeResult {
    Error error = Error::invalid_owner;
    storage::database::NativeOwnedSourceBootstrapResult source;
    bool ok() const noexcept { return error == Error::none && source.ok(); }
  };
  static ProbeResult ProbeBootstrap(std::shared_ptr<NativeHostedDatabaseCustody>) noexcept;
  struct DrainResult {
    Error error = Error::invalid_owner;
    storage::disk::RouteSourceTransitionError route_error = storage::disk::RouteSourceTransitionError::none;
    storage::disk::RouteSourceDrainWaitObservation observation;
    bool ok() const noexcept {
      return error == Error::none && observation.state == storage::disk::RouteSourceDrainWaitState::drained;
    }
  };
  // One finite native parking attempt; no wait handle/transition escapes and
  // no control mutex is held. Cancellation affects only this wait attempt.
  static DrainResult WaitForLegacyDrain(std::shared_ptr<NativeHostedDatabaseCustody>,
      std::chrono::steady_clock::time_point, std::stop_token = {}) noexcept;
  struct AdoptionResult {
    Error error = Error::invalid_owner;
    storage::database::NativeOwnedSourceDiagnostics source;
    bool ok() const noexcept { return error == Error::none && source.error == storage::database::NativeOwnedSourceError::none; }
  };
  static AdoptionResult AdoptWriter(std::shared_ptr<NativeHostedDatabaseCustody>, const Uuid&, const Uuid&,
      std::vector<std::unique_ptr<storage::disk::FileDevice>>&, storage::database::NativeOwnedCheckpointMemoryLimits,
      storage::database::NativeStorageMemory&, const storage::database::NativeStorageMemoryBinding&,
      storage::database::NativeStorageMemory&, const storage::database::NativeStorageMemoryBinding&) noexcept;
  static OperationResult TryBeginOperation(std::shared_ptr<NativeHostedDatabaseCustody>, const Uuid&) noexcept;
  struct CloseResult {
    Error error = Error::invalid_owner;
    storage::database::NativeWriterCloseResult writer;
    bool ok() const noexcept { return error == Error::none && writer.ok(); }
  };
  static CloseResult RetireWriter(std::shared_ptr<NativeHostedDatabaseCustody>,
      std::span<storage::database::NativeWriterMemberCloseReceipt>) noexcept;
  // After a successful physical close, release the writer's own backing while
  // retaining the original OS owner. External memory users drain separately.
  static Error RetireWriterBacking(std::shared_ptr<NativeHostedDatabaseCustody>) noexcept;

  class ReadLease final {
   public:
    ReadLease() noexcept = default;
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
    ReadLease(ReadLease&& other) noexcept
        : owner_(TakeOwner(other)), reader_(std::move(other.reader_)) {}
    ReadLease& operator=(ReadLease&& other) noexcept {
      if (this != &other) { other.CheckOwner(); Reset(); owner_ = std::move(other.owner_); reader_ = std::move(other.reader_); }
      return *this;
    }
    ~ReadLease() { Reset(); }
    explicit operator bool() const noexcept { CheckOwner(); return bool(reader_); }
    const storage::database::NativeSelectedCheckpointMemoryLease& source() const noexcept { CheckOwner(); return reader_.source(); }
    const Uuid& operation_uuid() const noexcept { CheckOwner(); return reader_.operation_uuid(); }
    void Reset() noexcept { CheckOwner(); reader_.Reset(); owner_.reset(); }
   private:
    friend class Operation;
    ReadLease(std::shared_ptr<NativeHostedDatabaseCustody> owner, Writer::ReadLease&& reader) noexcept
        : owner_(std::move(owner)), reader_(std::move(reader)) {}
    void CheckOwner() const noexcept {
      if (process_ != CurrentProcess() || thread_ != std::this_thread::get_id()) std::terminate();
    }
    static std::shared_ptr<NativeHostedDatabaseCustody> TakeOwner(ReadLease& other) noexcept {
      other.CheckOwner(); return std::move(other.owner_);
    }
    const std::uint64_t process_ = CurrentProcess();
    const std::thread::id thread_ = std::this_thread::get_id();
    std::shared_ptr<NativeHostedDatabaseCustody> owner_;
    Writer::ReadLease reader_; // Release device guards BEFORE the full host pin.
  };
  struct ReadResult : storage::database::NativeOwnedSourceDiagnostics {
    storage::database::NativeWriterError writer_error = storage::database::NativeWriterError::invalid_operation;
    ReadLease lease;
    bool ok() const noexcept {
      return writer_error == storage::database::NativeWriterError::none &&
          error == storage::database::NativeOwnedSourceError::none && bool(lease);
    }
  };
  class Operation final {
   public:
    Operation() noexcept = default;
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Operation(Operation&& other) noexcept
        : owner_(TakeOwner(other)), operation_(std::move(other.operation_)) {}
    Operation& operator=(Operation&& other) noexcept {
      if (this != &other) { other.CheckOwner(); Reset(); owner_ = std::move(other.owner_); operation_ = std::move(other.operation_); }
      return *this;
    }
    ~Operation() { Reset(); }
    explicit operator bool() const noexcept { CheckOwner(); return bool(operation_); }
    const Uuid& identity() const noexcept { CheckOwner(); return operation_.identity(); }
    const storage::database::NativeWriterPhysicalEffects& effects() const noexcept { CheckOwner(); return operation_.effects(); }
    storage::database::NativeOwnedCheckpointWriter::OperationContinuity RetainContinuity() const noexcept {
      CheckOwner();return operation_.RetainContinuity(owner_);
    }
    bool HasContinuity(const storage::database::NativeOwnedCheckpointWriter::OperationContinuity& pin) const noexcept {
      CheckOwner();return operation_.HasContinuity(pin);
    }
    void Reset() noexcept { CheckOwner(); operation_.Reset(); owner_.reset(); }
    ReadResult ReadWithMemory(storage::database::NativeSelectedCheckpointMemoryLimits limits,
        storage::database::NativeStorageMemory& memory, const storage::database::NativeStorageMemoryBinding& binding) noexcept {
      CheckOwner(); auto retained = owner_; // Before any callback can Reset this handle.
      auto raw = operation_.ReadWithMemory(limits, memory, binding);
      ReadResult out; static_cast<storage::database::NativeOwnedSourceDiagnostics&>(out) =
          std::move(static_cast<storage::database::NativeOwnedSourceDiagnostics&>(raw));
      out.writer_error = raw.writer_error;
      if (raw.ok()) out.lease = ReadLease(std::move(retained), std::move(raw.lease));
      return out;
    }
    Writer::ObservationResult ObserveWithMemory(storage::database::NativeSelectedCheckpointMemoryLimits limits,
        storage::database::NativeStorageMemory& memory,const storage::database::NativeStorageMemoryBinding& binding) noexcept {
      CheckOwner();auto retained=owner_;
      return operation_.ObserveWithMemory(limits,memory,binding,std::move(retained));
    }
    storage::database::NativeWriterIoResult ReadAtBounded(const Uuid& member, const Uuid& profile,
        std::uint64_t offset, std::span<core::platform::byte> bytes) noexcept {
      CheckOwner(); auto retained = owner_; return operation_.ReadAtBounded(member, profile, offset, bytes);
    }
    storage::database::NativeOwnedHistoryReadResult ReadHistoryWithMemory(
        storage::database::NativeManagementHistoryMemoryLimits limits,
        storage::database::NativeStorageMemory& memory,const storage::database::NativeStorageMemoryBinding& binding,
        const storage::database::NativeManagementCheckpointAnchor* anchor=nullptr) noexcept {
      CheckOwner();auto retained=owner_;
      return operation_.ReadHistoryWithMemory(limits,memory,binding,anchor);
    }
    storage::database::NativeOwnedControlReadResult ReadControlWithMemory(
        storage::database::NativeManagementControlAuthorityMemoryLimits limits,
        storage::database::NativeStorageMemory& memory,const storage::database::NativeStorageMemoryBinding& binding) noexcept {
      CheckOwner();auto retained=owner_;
      return operation_.ReadControlWithMemory(limits,memory,binding);
    }
    storage::database::NativeWriterIoResult WriteAtBounded(const Uuid& member, const Uuid& profile,
        std::uint64_t offset, std::span<const core::platform::byte> bytes) noexcept {
      CheckOwner(); auto retained = owner_; return operation_.WriteAtBounded(member, profile, offset, bytes);
    }
    storage::database::NativeWriterIoResult Sync(const Uuid& member, const Uuid& profile) noexcept {
      CheckOwner(); auto retained = owner_; return operation_.Sync(member, profile);
    }
   private:
    friend class NativeHostedDatabaseCustody;
    friend class storage::database::NativeOwnedInventoryPublication;
    Writer::Operation& PublicationOperation() noexcept { CheckOwner();return operation_; }
    Operation(std::shared_ptr<NativeHostedDatabaseCustody> owner, Writer::Operation&& operation) noexcept
        : owner_(std::move(owner)), operation_(std::move(operation)) {}
    void CheckOwner() const noexcept {
      if (process_ != CurrentProcess() || thread_ != std::this_thread::get_id()) std::terminate();
    }
    static std::shared_ptr<NativeHostedDatabaseCustody> TakeOwner(Operation& other) noexcept {
      other.CheckOwner(); return std::move(other.owner_);
    }
    const std::uint64_t process_ = CurrentProcess();
    const std::thread::id thread_ = std::this_thread::get_id();
    std::shared_ptr<NativeHostedDatabaseCustody> owner_;
    Writer::Operation operation_;
  };
  struct OperationResult {
    Error error = Error::invalid_owner;
    storage::database::NativeWriterError writer_error = storage::database::NativeWriterError::invalid_operation;
    storage::disk::RouteSourceTransitionError route_error = storage::disk::RouteSourceTransitionError::none;
    Operation operation;
    bool ok() const noexcept { return error == Error::none && bool(operation); }
  };
 private:
  NativeHostedDatabaseCustody() = default;
  bool InOwningProcess() const noexcept { return process_ == CurrentProcess(); }
  // Nonblocking operation reservation; never a mutex across callbacks or I/O.
  struct Control {
    NativeHostedDatabaseCustody& owner;
    bool acquired;
    explicit Control(NativeHostedDatabaseCustody& o) : owner(o), acquired(false) {
      bool idle = false; acquired = owner.busy_.compare_exchange_strong(idle, true, std::memory_order_acq_rel);
    }
    ~Control() { if (acquired) owner.busy_.store(false, std::memory_order_release); }
  };
  const std::uint64_t process_ = CurrentProcess();
  const std::thread::id opening_thread_ = std::this_thread::get_id();
  std::atomic<bool> busy_{false};
  NativeHostedCustodyStage stage_ = NativeHostedCustodyStage::awaiting_transfer;
  Uuid database_,primary_;
  bool writer_close_succeeded_=false;
  // Reverse destruction retires adopted devices/backing BEFORE initial OS custody.
  std::shared_ptr<DatabaseOwnershipLock> route_;
  std::shared_ptr<storage::disk::RouteSourceTransition> transition_;
  std::shared_ptr<Writer> writer_;
};
struct NativeHostedCustodyBeginResult {
  NativeHostedCustodyError error = NativeHostedCustodyError::invalid_owner;
  storage::disk::RouteSourceTransitionError route_error = storage::disk::RouteSourceTransitionError::none;
  std::shared_ptr<NativeHostedDatabaseCustody> custody;
  bool ok() const noexcept { return error == NativeHostedCustodyError::none && custody != nullptr; }
};
inline NativeHostedCustodyBeginResult NativeHostedDatabaseCustody::Begin(std::shared_ptr<DatabaseOwnershipLock> route) noexcept {
  NativeHostedCustodyBeginResult out;
  if (!route) return out;
  const auto capability = route->ObserveWriteCustody();
  if (capability == storage::disk::RouteWriteCustodyState::wrong_process) { out.error = Error::wrong_process; return out; }
  if (capability != storage::disk::RouteWriteCustodyState::retained) {
    out.error = Error::route_failure;
    using Capability = storage::disk::RouteWriteCustodyState;
    using Cause = storage::disk::RouteSourceTransitionError;
    switch (capability) {
      case Capability::not_requested: out.route_error = Cause::writable_not_requested; break;
      case Capability::primary_not_bound: out.route_error = Cause::primary_not_bound; break;
      case Capability::withdrawn: out.route_error = Cause::withdrawn; break;
      default: out.route_error = Cause::lock_failure; break;
    }
    return out;
  }
  try {
    auto owner = std::shared_ptr<NativeHostedDatabaseCustody>(new NativeHostedDatabaseCustody);
    auto transition = route->BeginNativeSourceTransition();
    out.route_error = transition.error;
    if (!transition.ok()) { out.error = Error::route_failure; return out; }
    owner->route_ = std::move(route); owner->transition_ = std::move(transition.transition);
    out.custody = std::move(owner); out.error = Error::none;
  } catch (const std::bad_alloc&) {
    out.error = Error::route_failure; out.route_error = storage::disk::RouteSourceTransitionError::resource_exhausted;
  }
  return out;
}
inline NativeHostedDatabaseCustody::Observation NativeHostedDatabaseCustody::Observe(std::shared_ptr<NativeHostedDatabaseCustody> owner) noexcept {
  Observation out; if (!owner) return out;
  if (!owner->InOwningProcess()) { out.error = Error::wrong_process; return out; }
  Control control(*owner); if (!control.acquired) { out.error = Error::busy; return out; }
  out.stage = owner->stage_; out.initial_route_owner_retained = bool(owner->route_);
  out.database_uuid = owner->database_; out.primary_filespace_uuid = owner->primary_;
  out.error = Error::none; return out;
}
inline NativeHostedDatabaseCustody::ProbeResult NativeHostedDatabaseCustody::ProbeBootstrap(std::shared_ptr<NativeHostedDatabaseCustody> owner) noexcept {
  ProbeResult out; if (!owner) return out;
  if (!owner->InOwningProcess()) { out.error = Error::wrong_process; return out; }
  if (owner->opening_thread_ != std::this_thread::get_id()) { out.error = Error::wrong_thread; return out; }
  Control control(*owner); if (!control.acquired) { out.error = Error::busy; return out; }
  if (owner->stage_ != NativeHostedCustodyStage::awaiting_transfer) { out.error = Error::already_mounted; return out; }
  out.source = storage::database::NativeOwnedCheckpointSource::ProbeRouteBootstrap(owner->transition_);
  out.error = out.source.ok() ? Error::none : Error::route_failure; return out;
}
inline NativeHostedDatabaseCustody::DrainResult NativeHostedDatabaseCustody::WaitForLegacyDrain(
    std::shared_ptr<NativeHostedDatabaseCustody> owner, std::chrono::steady_clock::time_point deadline, std::stop_token stop) noexcept {
  DrainResult out; if (!owner) return out;
  if (!owner->InOwningProcess()) { out.error = Error::wrong_process; return out; }
  if (owner->opening_thread_ != std::this_thread::get_id()) { out.error = Error::wrong_thread; return out; }
  Control control(*owner); if (!control.acquired) { out.error = Error::busy; return out; }
  if (owner->stage_ != NativeHostedCustodyStage::awaiting_transfer) { out.error = Error::already_mounted; return out; }
  auto waiting = owner->transition_->BeginDrainWait(deadline); out.route_error = waiting.error;
  if (!waiting.ok()) { out.error = Error::route_failure; return out; }
  std::stop_callback cancel(stop, [&] { (void)waiting.wait->Cancel(); });
  out.observation = waiting.wait->Wait(); out.error = Error::none; return out;
}
inline NativeHostedDatabaseCustody::AdoptionResult NativeHostedDatabaseCustody::AdoptWriter(
    std::shared_ptr<NativeHostedDatabaseCustody> owner, const Uuid& database, const Uuid& primary,
    std::vector<std::unique_ptr<storage::disk::FileDevice>>& secondary, storage::database::NativeOwnedCheckpointMemoryLimits limits,
    storage::database::NativeStorageMemory& persistent, const storage::database::NativeStorageMemoryBinding& persistent_binding,
    storage::database::NativeStorageMemory& transient, const storage::database::NativeStorageMemoryBinding& transient_binding) noexcept {
  AdoptionResult out; if (!owner) return out;
  if (!owner->InOwningProcess()) { out.error = Error::wrong_process; return out; }
  if (owner->opening_thread_ != std::this_thread::get_id()) { out.error = Error::wrong_thread; return out; }
  Control control(*owner); if (!control.acquired) { out.error = Error::busy; return out; }
  if (owner->stage_ != NativeHostedCustodyStage::awaiting_transfer) { out.error = Error::already_mounted; return out; }
  auto adopted = Writer::AdoptRouteWithMemory(owner->transition_, database, primary, secondary,
      limits, persistent, persistent_binding, transient, transient_binding);
  const bool accepted = adopted.ok();
  out.source = std::move(static_cast<storage::database::NativeOwnedSourceDiagnostics&>(adopted));
  if (!accepted) { out.error = Error::writer_failure; return out; }
  owner->writer_ = std::move(adopted.writer); owner->stage_ = NativeHostedCustodyStage::writer_retained;
  owner->database_=owner->writer_->database_uuid();owner->primary_=owner->writer_->primary_uuid();
  out.error = Error::none; return out;
}
inline NativeHostedDatabaseCustody::OperationResult NativeHostedDatabaseCustody::TryBeginOperation(
    std::shared_ptr<NativeHostedDatabaseCustody> owner, const Uuid& identity) noexcept {
  OperationResult out; if (!owner) return out;
  if (!owner->InOwningProcess()) { out.error = Error::wrong_process; return out; }
  Control control(*owner); if (!control.acquired) { out.error = Error::busy; return out; }
  if (owner->stage_ != NativeHostedCustodyStage::writer_retained) { out.error = Error::not_mounted; return out; }
  auto begun = Writer::TryBeginOperation(owner->writer_, identity);
  out.writer_error = begun.error; out.route_error = begun.route_error;
  if (!begun.ok()) { out.error = Error::writer_failure; return out; }
  out.operation = Operation(owner, std::move(begun.operation)); out.error = Error::none; return out;
}
inline NativeHostedDatabaseCustody::CloseResult NativeHostedDatabaseCustody::RetireWriter(
    std::shared_ptr<NativeHostedDatabaseCustody> owner, std::span<storage::database::NativeWriterMemberCloseReceipt> receipts) noexcept {
  CloseResult out; if (!owner) return out;
  if (!owner->InOwningProcess()) { out.error = Error::wrong_process; return out; }
  Control control(*owner); if (!control.acquired) { out.error = Error::busy; return out; }
  if (!owner->writer_) { out.error = Error::not_mounted; return out; }
  out.writer = Writer::Close(owner->writer_, receipts);
  if (out.writer.all_members_retired) owner->stage_ = NativeHostedCustodyStage::writer_retired;
  owner->writer_close_succeeded_=out.writer.ok();
  out.error = out.writer.ok() ? Error::none : Error::writer_failure;
  return out; // Retain initial route custody even after successful writer close.
}
inline NativeHostedDatabaseCustody::Error NativeHostedDatabaseCustody::RetireWriterBacking(
    std::shared_ptr<NativeHostedDatabaseCustody> owner) noexcept {
  if(!owner)return Error::invalid_owner;
  if(!owner->InOwningProcess())return Error::wrong_process;
  if(owner->opening_thread_!=std::this_thread::get_id())return Error::wrong_thread;
  Control control(*owner);if(!control.acquired)return Error::busy;
  if(owner->stage_!=NativeHostedCustodyStage::writer_retired||!owner->writer_close_succeeded_)
    return Error::writer_failure;
  owner->writer_.reset();
  return Error::none;
}
} // namespace scratchbird::server
