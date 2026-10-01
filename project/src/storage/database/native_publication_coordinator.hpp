// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "node_uuid_issuer.hpp"
#include "disk_device.hpp"
#include "native_checkpoint_selection.hpp"
#include "native_publication_watermark.hpp"
#include <memory>

namespace scratchbird::storage::database {
enum class NativePublicationError {
  none, invalid_request, invalid_device, checkpoint_failure, bootstrap_failure,
  allocation_mismatch, image_failure, binding_mismatch, repair_required,
  stale_base, generation_exhausted, hash_failure, resource_exhausted,
  io_failure, preimage_changed, readback_mismatch, cluster_requires_authority,
  encrypted_requires_authority, operation_pending, request_mismatch,
  identity_failure, allocation_exhausted
};
struct NativePublicationSnapshot {
  NativeCheckpointSelection selection;
  NativePublicationWatermark watermark;
  std::array<byte,32> state_sha256{};
};
// Actual I/O observations, not authorization or a management-operation outcome.
// Lease receipts are cumulative for that lease (including its reservation or
// resume call), not durable history or unique bytes. A resumed lease begins a
// new observation interval; prior/crash effects require native reconciliation.
struct NativePublicationEffects {
  bool observed=false;
  u64 write_attempts=0, attempted_bytes=0, confirmed_bytes=0;
  u64 sync_attempts=0, successful_syncs=0;
  bool uncertain_write=false;
  bool selector_write_attempted=false;
  bool installed_graph_verified=false, selected_graph_verified=false;
  bool operator==(const NativePublicationEffects&) const=default;
};
struct NativePublicationInspection {
  NativePublicationError error=NativePublicationError::invalid_request;
  std::optional<NativePublicationSnapshot> snapshot;
  NativePublicationEffects effects;
  bool ok() const noexcept {return error==NativePublicationError::none&&snapshot.has_value();}
};
struct NativePublicationReservation;
struct NativePublicationPlan;
struct NativeManagementOperation;
struct NativePreallocationPublicationResult;
class NativePublicationLease {
 public:
  ~NativePublicationLease();
  NativePublicationLease(const NativePublicationLease&)=delete;
  NativePublicationLease& operator=(const NativePublicationLease&)=delete;
  const NativePublicationSnapshot& snapshot() const noexcept;
  const NativePublicationEffects& effects() const noexcept;
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
  friend NativePublicationInspection InstallNativeManagementControlGraphOnLease(
    NativePublicationLease&,const NativePublicationPlan&,const std::vector<byte>&,
    const std::vector<std::vector<byte>>&,const std::vector<std::vector<byte>>&,u64) noexcept;
  friend NativePublicationInspection ResumeNativeManagementControlGraphOnLease(
    NativePublicationLease&,u64) noexcept;
  friend NativePublicationInspection PublishNativeManagementControlGraphOnLease(
    NativePublicationLease&,u64) noexcept;
  friend NativePublicationInspection PublishNativeInventoryOnLease(
    NativePublicationLease&,const NativeManagementOperation&,
    const transaction::mga::LocalTransactionInventory&,u64,
    core::uuid::StandaloneUuidV7Issuer&) noexcept;
  friend NativePreallocationPublicationResult PublishNativePreallocationOnLease(
    NativePublicationLease&,const NativeManagementOperation&,u64,
    core::uuid::StandaloneUuidV7Issuer&) noexcept;
};
struct NativePublicationReservation {
  NativePublicationError error=NativePublicationError::invalid_request;
  std::unique_ptr<NativePublicationLease> lease;
  NativePublicationEffects effects;
  bool ok() const noexcept {return error==NativePublicationError::none&&lease!=nullptr;}
};
// Resolves only an explicitly profiled, unselected metadata publication.
// Keeps the consumed generation, original intent/anchor and every non-watermark
// byte. No management cancellation, page reclamation or user-effect authority.
NativePublicationInspection AbandonNativeMetadataPublicationOnOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>&,
  const Uuid& primary,const NativePublicationSnapshot& expected_pending,
  const Uuid& original_attempt,const NativePublicationIntent& expected_intent,
  const Uuid& resolution_uuid,u64 maximum_verification_image_bytes) noexcept;
// Resolves only an unselected profile2 inventory/control candidate. Preserves
// the selected inventory, including durable starting allocations and counters.
// This is not transaction rollback/finality, page reclamation or BEGIN success.
NativePublicationInspection AbandonNativeInventoryPublicationOnOpenDevices(
  const Uuid& database,const std::vector<disk::NativeFilespaceDevice>&,
  const Uuid& primary,const NativePublicationSnapshot& expected_pending,
  const Uuid& original_attempt,const NativePublicationIntent& expected_intent,
  const Uuid& resolution_uuid,u64 maximum_verification_image_bytes) noexcept;
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
// Installs and verifies the complete version3 plan/extent/bundle/maps/checkpoint.
// Does not change checkpoint selectors or confer kernel/client authority.
NativePublicationInspection InstallNativeManagementControlGraphOnLease(
  NativePublicationLease&,const NativePublicationPlan&,const std::vector<byte>& target_checkpoint,
  const std::vector<std::vector<byte>>& extent_pages,const std::vector<std::vector<byte>>& bundle_pages,
  u64 maximum_retained_image_bytes) noexcept;
// Reconstructs from an intact actual anchored plan/extent/bundle and bound base.
// Missing input leaves the pending intent intact; no guessed UUIDs or success.
NativePublicationInspection ResumeNativeManagementControlGraphOnLease(
  NativePublicationLease&,u64 maximum_retained_image_bytes) noexcept;
// Publishes the actual installed V4 graph, first selector then second, with
// sync/full readback and final ordinary selected admission. No caller images,
// authentication, operation-effect completion or SQL receipt. Ambiguous I/O
// poisons the lease and requires explicit owning recovery.
NativePublicationInspection PublishNativeManagementControlGraphOnLease(
  NativePublicationLease&,u64 maximum_verification_image_bytes) noexcept;
// Constructs actual profile2 pages/allocations from the lease, installs and
// selects them. No caller-created slots/images/identities, implicit growth or
// reuse. The owning kernel must authorize the exact inventory delta and record;
// this primitive grants neither execution permission nor a SQL completion.
// Anchored/ambiguous attempts require existing exact-graph recovery, not rebuild.
// Requires the owning node/policy-bound allocation instance, not raw time or a
// context-free runtime generator. The kernel retains policy-selection authority.
NativePublicationInspection PublishNativeInventoryOnLease(
  NativePublicationLease&,const NativeManagementOperation&,
  const transaction::mga::LocalTransactionInventory&,
  u64 maximum_verification_image_bytes,core::uuid::StandaloneUuidV7Issuer&) noexcept;

struct NativePreallocationPublicationResult {
  NativePublicationInspection publication;
  bool physical_attempted=false, physical_sync_completed=false;
  std::optional<disk::PreallocateExtentResult> physical;
  // An attempted call without a returned physical result has unknown effects.
  // Publication effects and a returned failed physical result remain available.
  bool ok() const noexcept {
    return publication.ok()&&physical&&physical->ok()&&physical_sync_completed;
  }
};
// Owned profile3 primary preallocation: retain exact intent/control graph,
// perform actual contained physical reserve, sync, then publish native states.
// The owning kernel supplies security/policy/MGA/resource authority separately.
// A pending exact anchored attempt can be resumed; a failed physical/sync phase
// poisons its lease until explicit release/reinspection/resume. This is not a
// runtime API, transaction completion or a proof of physical reserve on fallback.
NativePreallocationPublicationResult PublishNativePreallocationOnLease(
  NativePublicationLease&,const NativeManagementOperation&,
  u64 maximum_verification_image_bytes,core::uuid::StandaloneUuidV7Issuer&) noexcept;
} // namespace scratchbird::storage::database
