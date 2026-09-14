// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_checkpoint_selection.hpp"
#include "native_publication_watermark.hpp"
#include <memory>

namespace scratchbird::storage::database {
enum class NativePublicationError {
  none, invalid_request, invalid_device, checkpoint_failure, bootstrap_failure,
  allocation_mismatch, image_failure, binding_mismatch, repair_required,
  stale_base, generation_exhausted, hash_failure, resource_exhausted,
  io_failure, preimage_changed, readback_mismatch, cluster_requires_authority,
  encrypted_requires_authority, operation_pending, request_mismatch
};
struct NativePublicationSnapshot {
  NativeCheckpointSelection selection;
  NativePublicationWatermark watermark;
  std::array<byte,32> state_sha256{};
};
struct NativePublicationInspection {
  NativePublicationError error=NativePublicationError::invalid_request;
  std::optional<NativePublicationSnapshot> snapshot;
  bool ok() const noexcept {return error==NativePublicationError::none&&snapshot.has_value();}
};
struct NativePublicationReservation;
struct NativePublicationPlan;
class NativePublicationLease {
 public:
  ~NativePublicationLease();
  NativePublicationLease(const NativePublicationLease&)=delete;
  NativePublicationLease& operator=(const NativePublicationLease&)=delete;
  const NativePublicationSnapshot& snapshot() const noexcept;
 private:
  struct Impl;
  explicit NativePublicationLease(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
  friend NativePublicationReservation ReserveNativePublicationGenerationOnOpenDevices(
    const Uuid&,const std::vector<disk::NativeFilespaceDevice>&,const Uuid&,
    const NativePublicationSnapshot&,const Uuid&,u64,const NativePublicationIntent*) noexcept;
  friend NativePublicationReservation ResumeNativePublicationGenerationOnOpenDevices(
    const Uuid&,const std::vector<disk::NativeFilespaceDevice>&,const Uuid&,
    const NativePublicationSnapshot&,const Uuid&,const NativePublicationIntent&,u64) noexcept;
  friend NativePublicationInspection InstallNativePublicationPlanOnLease(
    NativePublicationLease&,const NativePublicationPlan&,const std::vector<byte>&,u64) noexcept;
  friend NativePublicationInspection InstallNativeManagementPublicationOnLease(
    NativePublicationLease&,const NativePublicationPlan&,const std::vector<byte>&,
    const std::vector<std::vector<byte>>&,u64) noexcept;
};
struct NativePublicationReservation {
  NativePublicationError error=NativePublicationError::invalid_request;
  std::unique_ptr<NativePublicationLease> lease;
  bool ok() const noexcept {return error==NativePublicationError::none&&lease!=nullptr;}
};
// Actual bootstrap, allocation, inventory and checkpoint binding; no mutation.
NativePublicationInspection InspectNativePublicationGenerationOnOpenDevices(
  const Uuid&,const std::vector<disk::NativeFilespaceDevice>&,const Uuid&,u64) noexcept;
// Explicit ordered duplex repair. Does not allocate a generation or transaction.
NativePublicationInspection RecoverNativePublicationGenerationOnOpenDevices(
  const Uuid&,const std::vector<disk::NativeFilespaceDevice>&,const Uuid&,u64) noexcept;
// Burns one durable generation and retains all device guards in the lease.
// Thread-affine: consume and destroy the lease on the reserving thread.
// Caller-owned devices must remain open and alive until the lease is released.
// No page reservation, checkpoint publication, transaction finality or SQL receipt.
NativePublicationReservation ReserveNativePublicationGenerationOnOpenDevices(
  const Uuid&,const std::vector<disk::NativeFilespaceDevice>&,const Uuid&,
  const NativePublicationSnapshot&,const Uuid& operation_uuid,u64,
  const NativePublicationIntent* intent=nullptr) noexcept;
// Reacquire an exact pending request after explicit inspection/recovery. Copies
// all caller data, verifies unchanged durable replicas and retains device guards.
// Does not increment a counter or grant authentication, execution or publication.
NativePublicationReservation ResumeNativePublicationGenerationOnOpenDevices(
  const Uuid&,const std::vector<disk::NativeFilespaceDevice>&,const Uuid&,
  const NativePublicationSnapshot&,const Uuid& operation_uuid,
  const NativePublicationIntent&,u64) noexcept;
// Durable primary plan ownership, write, sync and full readback. Does not
// publish the target checkpoint or authenticate/complete a management request.
// Ambiguous failure requires explicit inspection/recovery and a resumed lease.
NativePublicationInspection InstallNativePublicationPlanOnLease(
  NativePublicationLease&,const NativePublicationPlan&,
  const std::vector<byte>& target_checkpoint,u64 maximum_retained_image_bytes) noexcept;
// Installs the exact plan-owned operation extent after durable range anchoring.
// No checkpoint selection or execution authority is granted. An I/O-phase
// failure poisons this lease until it is released and explicitly resumed.
NativePublicationInspection InstallNativeManagementPublicationOnLease(
  NativePublicationLease&,const NativePublicationPlan&,const std::vector<byte>& target_checkpoint,
  const std::vector<std::vector<byte>>& extent_pages,u64 maximum_retained_image_bytes) noexcept;
} // namespace scratchbird::storage::database
