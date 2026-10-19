// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_owned_inventory_publication.hpp"

namespace scratchbird::storage::database {
enum class NativeOwnedInventoryRecoveryPhase {
  idle, preparing, prepared, preimages, members_sync, selector_write,
  selector_sync, selector_readback, verify_selected, complete, preparation_failed
};
struct NativeOwnedInventoryRecoveryLimits {
  NativeSelectedCheckpointMemoryLimits selected;
  NativeManagementHistoryMemoryLimits history;
  NativeManagementControlAuthorityMemoryLimits control;
  usize workspace_backing_bytes=0;
};
struct NativeOwnedInventoryRecoveryResult {
  NativePublicationError error=NativePublicationError::invalid_request;
  NativeOwnedInventoryRecoveryPhase phase=NativeOwnedInventoryRecoveryPhase::idle;
  NativeWriterPhysicalEffects effects;
  std::optional<NativeWriterIoResult> io;
  NativeManagementHistoryError history_error=NativeManagementHistoryError::none;
  NativeManagementControlAuthorityError control_error=NativeManagementControlAuthorityError::none;
  bool installed_graph_verified=false,selected_graph_verified=false,mutation_started=false;
  std::optional<NativePublicationSnapshot> snapshot;
  bool ok()const noexcept{return error==NativePublicationError::none;}
};

// Resume the exact installed profile-2 graph through the original mounted
// writer. Preparation requires two intact current selectors and watermarks;
// a torn-current pair remains a separate explicit recovery obligation. No raw
// file, fresh graph, generation reservation or transaction decision is issued.
// The supplied intent is compared with the complete durable request, plan,
// management history and control allocations before any selector changes.
// Memory and the issuing manager must outlive this thread-affine owner.
class NativeOwnedInventoryRecovery final {
 public:
  NativeOwnedInventoryRecovery() noexcept=default;
  ~NativeOwnedInventoryRecovery();
  NativeOwnedInventoryRecovery(const NativeOwnedInventoryRecovery&)=delete;
  NativeOwnedInventoryRecovery& operator=(const NativeOwnedInventoryRecovery&)=delete;
  template<class Operation>
  NativeOwnedInventoryRecoveryResult Prepare(Operation& operation,
      NativeOwnedPublicationWatermark&& inspected,const Uuid& expected_attempt,
      const NativePublicationIntent& expected_intent,NativeOwnedInventoryRecoveryLimits limits,
      NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
    CheckOwner();inspected.CheckOwner();
    if(entering_||phase_!=NativeOwnedInventoryRecoveryPhase::idle)
      return Result(NativePublicationError::operation_pending);
    if(!operation||inspected.entering_||!inspected.stable_||!inspected.observed_||
        inspected.phase_!=NativeOwnedWatermarkPhase::inspected||
        !operation.HasContinuity(inspected.continuity_))
      return Result(NativePublicationError::request_mismatch);
    const auto& w=inspected.observed_->watermark;
    if(w.operation_uuid!=expected_attempt||!w.intent||*w.intent!=expected_intent||
        expected_intent.recovery_profile!=2||!w.publication_plan||w.abandonment)
      return Result(NativePublicationError::request_mismatch);
    entering_=true;struct Leave{bool& flag;~Leave(){flag=false;}}leave{entering_};
    reservation_=std::move(inspected);limits_=limits;memory_=&memory;binding_=binding;
    phase_=NativeOwnedInventoryRecoveryPhase::preparing;
    return PrepareImpl();
  }
  NativeOwnedInventoryRecoveryResult Publish() noexcept;
  NativeOwnedInventoryRecoveryResult Resume() noexcept{return Publish();}
 private:
  NativeOwnedPublicationWatermark reservation_;
  NativeStorageArena workspace_;
  NativeStorageMemory* memory_=nullptr;
  NativeStorageMemoryBinding binding_;
  NativeOwnedInventoryRecoveryLimits limits_;
  NativePublicationPlan plan_;
  NativeManagementCheckpointAnchor anchor_;
  std::optional<NativePublicationSnapshot> selected_;
  std::array<std::span<byte>,2> preimages_,selectors_;
  std::array<u64,2> selector_pages_{};
  std::span<byte> scratch_;
  NativeOwnedInventoryRecoveryPhase phase_=NativeOwnedInventoryRecoveryPhase::idle;
  usize member_=0,replica_=0;
  bool entering_=false,mutation_started_=false,installed_graph_verified_=false,selected_graph_verified_=false;
  void CheckOwner()const noexcept{reservation_.CheckOwner();}
  NativeOwnedInventoryRecoveryResult Result(NativePublicationError)const noexcept;
  NativeOwnedInventoryRecoveryResult IoFailure(NativeWriterIoResult&&)const noexcept;
  NativeOwnedInventoryRecoveryResult PrepareImpl() noexcept;
};
} // namespace scratchbird::storage::database
