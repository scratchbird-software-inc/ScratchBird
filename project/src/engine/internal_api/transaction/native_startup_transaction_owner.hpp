// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_owned_inventory_publication.hpp"
#include "engine/internal_api/verification/native_startup_verification_request.hpp"
#include "engine/internal_api/verification/native_startup_management_definition.hpp"
#include "engine/internal_api/verification/native_verification_policy_selection.hpp"
#include "core/agents/agent_startup_clock_policy.hpp"
#include "native_transaction_policy_selection.hpp"
#include "native_startup_snapshot.hpp"
#include "native_startup_lifetime.hpp"
#include "native_startup_policy_evaluation.hpp"
#include "native_startup_lock.hpp"
#include "engine/internal_api/verification/native_startup_storage_identity.hpp"
#include "engine/internal_api/verification/native_startup_storage_scan.hpp"
#include <memory>

namespace scratchbird::server {class NativeHostedDatabaseRuntime;}
namespace scratchbird::engine::internal_api {
struct NativeStartupTransactionLimits {
  storage::database::NativeOwnedInventoryPublicationLimits publication;
  std::size_t maximum_publication_memory_bytes=0;
};
enum class NativeStartupTransactionError {
  none,not_ready,busy,cancelled,source_mismatch,policy_required,authorization_refused,
  recovery_required,invalid_limits,resource_exhausted,identity_failure,request_failure,policy_refused,
  crypto_failure,publication_failure,clock_failure,transaction_expired,snapshot_expired,verification_failure
};
enum class NativeStartupTransactionPhase { absent,prepared,reserving,building,publishing,starting_selected,boundary_selected,detail_selected,admission_evidence_selected,active_selected };
// Observation of an actual owned starting reservation. Starting selection does
// not imply active MGA admission, committed activation or ordinary readiness.
struct NativeStartupTransactionResult {
  NativeStartupTransactionError error=NativeStartupTransactionError::not_ready;
  const char* detail="";
  core::platform::Status allocation_status;
  std::size_t publication_reserved_bytes=0;
  NativeStartupTransactionPhase phase=NativeStartupTransactionPhase::absent;
  // Updated only after the corresponding physical selection is verified.
  std::optional<NativeVerificationPhase> verification_phase;
  storage::database::NativeStartupBinding binding;
  core::platform::Uuid policy_snapshot_uuid,original_publication_uuid,current_publication_uuid,maintenance_boundary_uuid;
  storage::database::NativePublicationError reservation_error=storage::database::NativePublicationError::none;
  storage::database::NativeOwnedWatermarkPhase reservation_phase=storage::database::NativeOwnedWatermarkPhase::idle;
  storage::database::NativePublicationError publication_error=storage::database::NativePublicationError::none;
  storage::database::NativeWriterError handoff_error=storage::database::NativeWriterError::none;
  storage::database::NativeWriterError read_writer_error=storage::database::NativeWriterError::none;
  storage::database::NativeSelectedCheckpointMemoryError read_memory_error=storage::database::NativeSelectedCheckpointMemoryError::none;
  storage::database::NativeCheckpointSelectionError read_selection_error=storage::database::NativeCheckpointSelectionError::none;
  storage::database::NativeCheckpointError read_checkpoint_error=storage::database::NativeCheckpointError::none;
  storage::page::NativeInventoryError read_inventory_error=storage::page::NativeInventoryError::none;
  storage::page::NativeAllocationError read_allocation_error=storage::page::NativeAllocationError::none;
  storage::database::NativeOwnedInventoryPublicationPhase publication_phase=storage::database::NativeOwnedInventoryPublicationPhase::idle;
  storage::database::NativeWriterPhysicalEffects effects;
  std::optional<storage::database::NativeWriterIoResult> io;
  std::optional<storage::database::NativePublicationSnapshot> selected;
  bool ok()const noexcept{return error==NativeStartupTransactionError::none;}
};
// Only the actual host can construct this engine-owned lifetime. It retains a
// frozen bootstrap policy source and one database/clock-bound issuer. No UUID,
// caller observation, boolean, or ordinary client handle constructs this owner.
class NativeStartupTransactionOwner final {
 public:
  ~NativeStartupTransactionOwner()=default;
  NativeStartupTransactionOwner(const NativeStartupTransactionOwner&)=delete;
  NativeStartupTransactionOwner& operator=(const NativeStartupTransactionOwner&)=delete;
 private:
  friend class scratchbird::server::NativeHostedDatabaseRuntime;
  NativeStartupTransactionOwner(core::platform::Uuid snapshot,
      const core::agents::AgentStartupClockPolicySelection& clock,NativeStartupTransactionLimits limits)
      :issuer_({clock.database_uuid,snapshot},clock.policy),limits_(limits) {
    result_.policy_snapshot_uuid=snapshot;
  }
  // The enclosing CredentialRead owns all borrowed source rows/text, private
  // session and authorization until AFTER this object has retired.
  storage::database::NativeCheckpointSelection source_;
  NativeVerificationPolicyDeclarations policies_;
  NativeTransactionPolicyDeclarations transaction_policies_;
  NativeStartupManagementDefinition management_;
  std::span<const storage::database::NativeCapturedCatalogRow> frozen_rows_;
  // Registered only after complete projection and source bindings. Exclusive
  // native custody and frozen rows retain this inventory/catalog lifetime;
  // no cleanup/recycling worker can run in the private bootstrap interval.
  std::optional<NativeStartupSnapshotObservation> snapshot_;
  // Governed request and decision retention. This bounded bootstrap profile
  // refuses a larger source instead of truncating actors or evaluated policies.
  std::array<core::platform::Uuid,128> policy_subjects_,policy_targets_;
  std::array<NativeStartupPolicyBinding,128> evaluated_policy_bindings_;
  std::optional<NativeStartupPolicyRequest> policy_evaluation_request_;
  core::platform::Uuid policy_evaluation_decision_uuid_;
  std::optional<NativeStartupPolicyEvaluation> policy_evaluation_;
  std::optional<NativeStartupPolicyObservation> policy_observation_;
  NativeStartupLockRegistration maintenance_lock_;
  std::optional<NativeStartupStorageIdentityObservation> verified_identity_;
  std::optional<NativeStartupStorageScanObservation> allocation_scan_,inventory_scan_;
  std::optional<NativeStartupCatalogRouteObservation> catalog_routes_;
  std::array<std::array<char,512>,2> verified_boundary_storage_;
  std::array<core::platform::Uuid,128> admission_versions_;
  std::array<core::platform::byte,kNativeAdmissionEvidenceMaximumBytes> admission_storage_;
  std::optional<NativeStartupAdmissionEvidenceInput> admission_input_;
  std::span<const core::platform::byte> admission_bytes_;
  core::platform::Uuid admission_publication_uuid_;
  // Process-local elapsed time never becomes a persisted wall-clock value or
  // authority after reopen. The private owner acquires every sample itself.
  core::platform::u64 started_native_nanoseconds_=0,last_native_nanoseconds_=0;
  std::optional<core::platform::u64> snapshot_native_nanoseconds_;
  NativeStartupLifetimeError CheckLifetime() noexcept {
    const auto now=core::time::ReadNativeMonotonicTime();
    if(!now.ok())return NativeStartupLifetimeError::clock_failure;
    const auto result=CheckNativeStartupLifetime(started_native_nanoseconds_,last_native_nanoseconds_,
        snapshot_native_nanoseconds_,now.nanoseconds,transaction_policies_.body.maximum_transaction_microseconds,
        transaction_policies_.body.maximum_snapshot_microseconds);
    if(result!=NativeStartupLifetimeError::invalid_request&&result!=NativeStartupLifetimeError::clock_regressed)
      last_native_nanoseconds_=now.nanoseconds;
    return result;
  }
  core::uuid::StandaloneUuidV7Issuer issuer_;
  const NativeStartupTransactionLimits limits_;
  NativeStartupVerificationRequest request_;
  NativeVerificationOperationState verification_state_;
  std::array<std::span<core::platform::byte>,2> state_storage_;
  struct BoundaryPreparation {
    NativeVerificationOperationState state;
    storage::database::NativeManagementOperationView record;
    core::platform::Uuid publication_uuid;
  };
  std::optional<BoundaryPreparation> successor_preparation_;
  NativeStartupTransactionPhase expected_selected_phase_=NativeStartupTransactionPhase::starting_selected;
  storage::database::NativeManagementOperationView record_;
  std::array<transaction::mga::TransactionInventoryEntry,2> entries_;
  std::array<transaction::mga::TransactionInventoryEntry,2> active_entries_;
  storage::page::NativeTransactionInventoryConstView candidate_;
  std::array<char,86> idempotency_{};
  NativeStartupTransactionResult result_;
  // Destruction order keeps the allocator alive until the actual publisher and
  // original reservation have retired. Exact bytes survive all I/O failures.
  storage::database::NativeStorageMemory publication_memory_;
  storage::database::NativeStorageMemoryBinding memory_binding_;
  storage::database::NativeOwnedWatermarkPreparedResult watermark_;
  storage::database::NativeOwnedWatermarkResult watermark_result_;
  std::optional<storage::database::NativeOwnedInventoryPublication> publisher_;
};
// Governed placement ownership: the state itself and its variable targets and
// canonical bytes are charged to the actual installed database allocator.
class NativeStartupTransactionHandle final {
 public:
  NativeStartupTransactionHandle() noexcept=default;
  NativeStartupTransactionHandle(const NativeStartupTransactionHandle&)=delete;
  NativeStartupTransactionHandle& operator=(const NativeStartupTransactionHandle&)=delete;
  NativeStartupTransactionHandle(NativeStartupTransactionHandle&& other) noexcept
      :backing_(std::move(other.backing_)),state_(std::exchange(other.state_,nullptr)){}
  NativeStartupTransactionHandle& operator=(NativeStartupTransactionHandle&& other) noexcept {
    if(this!=&other){Reset();backing_=std::move(other.backing_);state_=std::exchange(other.state_,nullptr);}return *this;
  }
  ~NativeStartupTransactionHandle(){Reset();}
 private:
  friend class scratchbird::server::NativeHostedDatabaseRuntime;
  void Reset() noexcept {auto* state=std::exchange(state_,nullptr);if(state)std::destroy_at(state);backing_={};}
  storage::database::NativeStorageArena backing_;
  NativeStartupTransactionOwner* state_=nullptr;
};
} // namespace scratchbird::engine::internal_api
