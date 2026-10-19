// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_owned_publication_watermark.hpp"
#include "native_inventory_graph_builder.hpp"
#include "native_management_control_allocation.hpp"

namespace scratchbird::storage::database {
struct NativeOwnedInventoryPublicationLimits {
  NativeSelectedCheckpointMemoryLimits selected;
  NativeManagementHistoryMemoryLimits history;
  NativeManagementControlAuthorityMemoryLimits control;
  usize graph_backing_bytes=0,workspace_backing_bytes=0,validation_backing_bytes=0;
  u64 maximum_image_bytes=0;
};
enum class NativeOwnedInventoryPublicationPhase {
  idle,preparing,prepared,group_write,group_sync,group_readback,anchor,
  verify_installed,selector_prepare,selector_members_sync,selector_write,
  selector_sync,selector_readback,verify_selected,complete,preparation_failed,handed_off
};
struct NativeOwnedInventoryPublicationResult {
  NativePublicationError error=NativePublicationError::invalid_request;
  NativeOwnedInventoryPublicationPhase phase=NativeOwnedInventoryPublicationPhase::idle;
  NativeWriterPhysicalEffects effects;
  std::optional<NativeWriterIoResult> io;
  std::optional<NativeOwnedWatermarkResult> watermark;
  NativeManagementControlAuthorityError control_error=NativeManagementControlAuthorityError::none;
  NativeManagementHistoryError history_error=NativeManagementHistoryError::none;
  NativeManagementControlAllocationError allocation_error=NativeManagementControlAllocationError::none;
  bool mutation_started=false,installed_graph_verified=false,selected_graph_verified=false;
  usize group=0,index=0;
  std::optional<NativePublicationSnapshot> snapshot;
  bool ok() const noexcept{return error==NativePublicationError::none;}
};
// An actual original-operation owner, with admitted immutable graph backing.
// Prepare reads and validates the complete selected ancestry before any graph
// write. Publish/Resume retain exact bytes, progress, preimages and custody
// across short writes, sync/read failures and caller-facade retirement. Startup
// reconstruction pages are durable before their watermark anchor is written.
// Successful selection is physical evidence; engine MGA owns transaction
// acceptance/finality and this object grants no security or startup readiness.
// The supplied memory manager must outlive this owner and all Resume calls.
class NativeOwnedInventoryPublication final {
 public:
  NativeOwnedInventoryPublication() noexcept=default;
  ~NativeOwnedInventoryPublication();
  NativeOwnedInventoryPublication(const NativeOwnedInventoryPublication&)=delete;
  NativeOwnedInventoryPublication& operator=(const NativeOwnedInventoryPublication&)=delete;
  template<class Operation>
  NativeOwnedInventoryPublicationResult Prepare(Operation& operation,
      NativeOwnedPublicationWatermark&& reservation,const NativeManagementOperationView& record,
      const page::NativeTransactionInventoryConstView& inventory,core::uuid::StandaloneUuidV7Issuer& issuer,
      NativeOwnedInventoryPublicationLimits limits,NativeStorageMemory& memory,
      const NativeStorageMemoryBinding& binding) noexcept {
    CheckOwner();reservation.CheckOwner();
    if(entering_||phase_!=NativeOwnedInventoryPublicationPhase::idle)return Result(NativePublicationError::operation_pending);
    if(!operation||reservation.entering_||reservation.phase_!=NativeOwnedWatermarkPhase::complete||
        !reservation.target_||!operation.HasContinuity(reservation.continuity_))return Result(NativePublicationError::request_mismatch);
    entering_=true;struct Leave {bool& flag;~Leave(){flag=false;}} leave{entering_};
    reservation_=std::move(reservation);phase_=NativeOwnedInventoryPublicationPhase::preparing;
    memory_=&memory;binding_=binding;limits_=limits;
    return PrepareImpl(record,inventory,issuer);
  }
  NativeOwnedInventoryPublicationResult Publish() noexcept;
  NativeOwnedInventoryPublicationResult Resume() noexcept{return Publish();}
  // Preparation can fail after the tuple reservation is durable but before
  // any graph image is written. Rebuild only that unwritten graph under the
  // SAME retained reservation. The original owner retains the exact record,
  // candidate and issuer; no new physical attempt or generation is admitted.
  NativeOwnedInventoryPublicationResult RetryPreparation(
      const NativeManagementOperationView&,const page::NativeTransactionInventoryConstView&,
      core::uuid::StandaloneUuidV7Issuer&) noexcept;
  // Advance an actually completed physical publication without an idle writer
  // interval. No copied result or UUID can authorize the transfer. Failure
  // preserves the original completed publisher and facade for the same retry.
  // The next phase must inspect its actual source; no MGA finality is inferred.
  template<class Operation>
  NativeWriterError Advance(Operation& operation,const Uuid& next_identity) noexcept {
    CheckOwner();
    if(entering_)return NativeWriterError::busy;
    if(phase_!=NativeOwnedInventoryPublicationPhase::complete||!selected_graph_verified_||!selected_||
        !operation.HasContinuity(reservation_.continuity_))return NativeWriterError::invalid_operation;
    entering_=true;struct Leave {bool& flag;~Leave(){flag=false;}} leave{entering_};
    const auto effects=operation.effects();
    const auto result=operation.PublicationOperation().AdvancePublication(reservation_.continuity_,next_identity);
    if(result==NativeWriterError::none){completed_effects_=effects;phase_=NativeOwnedInventoryPublicationPhase::handed_off;}
    return result;
  }
 private:
  using Image=std::span<const byte>;
  struct Artifact {disk::NativeCommonPageHeader header;Image image;};
  // Original full custody retires last, after all admitted backing.
  NativeOwnedPublicationWatermark reservation_;
  NativeOwnedCheckpointWriter::ReadObservation source_;
  NativeStorageArena graph_arena_,workspace_arena_;
  NativeStorageMemory* memory_=nullptr;
  NativeStorageMemoryBinding binding_;
  NativeOwnedInventoryPublicationLimits limits_;
  NativeInventoryGraphView graph_;
  NativeManagementCheckpointAnchor anchor_;
  std::span<Artifact> artifacts_;
  std::span<byte> scratch_;
  std::array<std::span<byte>,2> selectors_,selector_preimages_;
  std::array<u64,2> selector_pages_{};
  std::array<usize,7> groups_{};
  NativeOwnedInventoryPublicationPhase phase_=NativeOwnedInventoryPublicationPhase::idle;
  usize group_=0,index_=0,replica_=0;
  bool entering_=false,artifact_started_=false,anchored_=false,mutation_started_=false;
  bool installed_graph_verified_=false,selected_graph_verified_=false;
  std::optional<NativePublicationSnapshot> selected_;
  NativeWriterPhysicalEffects completed_effects_;
  void CheckOwner() const noexcept{reservation_.CheckOwner();}
  NativeOwnedInventoryPublicationResult Result(NativePublicationError) const noexcept;
  NativeOwnedInventoryPublicationResult PrepareImpl(const NativeManagementOperationView&,
      const page::NativeTransactionInventoryConstView&,core::uuid::StandaloneUuidV7Issuer&) noexcept;
  NativeOwnedInventoryPublicationResult IoFailure(NativeWriterIoResult&&) const noexcept;
  NativePublicationError PrepareAnchor() noexcept;
};
} // namespace scratchbird::storage::database
