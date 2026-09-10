// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "typed_update_carrier_codec.hpp"
#include <memory>
#include <atomic>

namespace scratchbird::engine::internal_api {

// Engine-runtime policy input only. No parser, client or carrier may configure
// this governor. Zero means unconfigured, never an unlimited/default grant.
struct EngineDmlUpdateResourcePolicyV1 {
  std::uint32_t maximum_assignments = 0;
  std::uint32_t maximum_predicate_nodes = 0;
  std::uint64_t maximum_candidate_rows = 0;
  std::uint32_t maximum_trigger_depth = 0;
  std::uint32_t maximum_effects = 0;
  std::uint64_t maximum_total_canonical_value_bytes = 0;
  std::uint64_t aggregate_canonical_value_bytes = 0;
  std::uint32_t maximum_active_grants = 0;
};

class EngineDmlUpdateResourceGovernorV1;
class EngineDmlUpdateResourceHandleV1 final {
 public:
  struct Lease;
  bool valid() const noexcept { return lease_ != nullptr; }
  const scratchbird::wire::TypedUpdateResourceBudgetCarrier* carrier() const noexcept;
 private:
  std::shared_ptr<Lease> lease_;
  friend class EngineDmlUpdateResourceGovernorV1;
};

struct EngineDmlUpdateResourceCaptureV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDmlUpdateResourceHandleV1 handle;
};

struct EngineDmlUpdateResourceObservationV1 {
  std::uint64_t policy_generation = 0;
  std::uint64_t active_grants = 0;
  std::uint64_t reserved_canonical_bytes = 0;
  std::uint64_t released_grants = 0;
};

enum class EngineDmlUpdateResourceReleaseV1 {
  abandoned_before_publication, published, aborted
};

enum class EngineDmlUpdateResourceReleaseCodeV1 {
  released, already_released, owner_mismatch, invalid_disposition,
  conflicting_disposition, phase_mismatch, ledger_failure, synchronization_failed
};

enum class EngineDmlUpdateResourcePublicationOutcomeV1 { published, not_published };
enum class EngineDmlUpdateResourcePublicationCodeV1 {
  completed, already_completed, invalid_handle, invalid_disposition,
  conflicting_disposition, phase_mismatch, synchronization_failed
};

// Opaque process-local handoff to the durable journal owner. Preparation pins
// capacity across the write; it is not evidence that the write occurred.
// Copies share one decision. Dropping a ticket does NOT resolve an uncertain
// write or return capacity. The owner must retain it until recovery resolves it.
class EngineDmlUpdateResourcePublicationV1 final {
 public:
  struct Pending;
  bool valid() const noexcept { return pending_ != nullptr; }
  // Call published only after durable bound success, or not_published only
  // after proving no durable bound record exists. No allocation, callback,
  // live admission check or I/O; revocation cannot undo an already durable write.
  EngineDmlUpdateResourcePublicationCodeV1 CompleteNoAlloc(
      EngineDmlUpdateResourcePublicationOutcomeV1 outcome) const noexcept;
 private:
  std::shared_ptr<Pending> pending_;
  friend class EngineDmlUpdateResourceGovernorV1;
};

struct EngineDmlUpdateResourcePublicationPreparationV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDmlUpdateResourcePublicationV1 publication;
};

// Process-local canonical-value capacity governor. Reservations are logical
// capacity, not allocated payload, MGA finality, security grants or recovery.
// The owning engine runtime must retain this governor and release each grant.
// Journal owners prepare before writing, complete the ticket after resolving
// the write, and release only after the corresponding durable terminal state.
// None of these methods attest that a journal was written.
class EngineDmlUpdateResourceGovernorV1 final {
 public:
  struct State;
  EngineDmlUpdateResourceGovernorV1();
  EngineDmlUpdateResourceGovernorV1(const EngineDmlUpdateResourceGovernorV1&) = delete;
  EngineDmlUpdateResourceGovernorV1& operator=(const EngineDmlUpdateResourceGovernorV1&) = delete;
  EngineApiDiagnostic Configure(const EngineDmlUpdateResourcePolicyV1& policy);
  // Permanently revoke new admission for this engine instance. Outstanding
  // resource-only terminal cleanup remains available to retained owners.
  void StopAdmission() noexcept;
  EngineDmlUpdateResourceCaptureV1 Capture(const EngineRequestContext& context);
  EngineApiDiagnostic Revalidate(const EngineRequestContext& context,
      const EngineDmlUpdateResourceHandleV1& handle,
      const scratchbird::wire::TypedUpdateResourceBudgetCarrier& supplied) const;
  EngineApiDiagnostic Cancel(const EngineRequestContext& context,
                            const EngineDmlUpdateResourceHandleV1& handle);
  EngineDmlUpdateResourcePublicationPreparationV1 PrepareDescriptorPublication(
      const EngineRequestContext& context, const EngineDmlUpdateResourceHandleV1& handle);
  // Allocating convenience for component callers only. Durable-write owners
  // must use the split prepare/complete interface around the actual write.
  EngineApiDiagnostic PublishDescriptor(const EngineRequestContext& context,
                                       const EngineDmlUpdateResourceHandleV1& handle);
  EngineApiDiagnostic Release(const EngineRequestContext& context,
      const EngineDmlUpdateResourceHandleV1& handle, EngineDmlUpdateResourceReleaseV1 reason);
  // Use with retained/prebuilt context and handle after the owner's durable
  // terminal transition. No allocation, callback, carrier encoding or I/O.
  // This releases resources only; it neither performs nor proves that transition.
  // A failure retains the grant for retry; it must not trigger post-barrier rollback.
  EngineDmlUpdateResourceReleaseCodeV1 ReleaseNoAlloc(const EngineRequestContext& context,
      const EngineDmlUpdateResourceHandleV1& handle, EngineDmlUpdateResourceReleaseV1 reason) noexcept;
  EngineDmlUpdateResourceObservationV1 Observe() const;
 private:
  std::shared_ptr<State> state_;
};

// Receipt issuance binds an immutable engine context to one runtime governor.
// Receipt release revokes admission, not outstanding durable-operation grants.
class EngineDmlUpdateResourceReceiptV1 final {
 public:
  EngineDmlUpdateResourceReceiptV1(const EngineRequestContext& context,
      std::shared_ptr<EngineDmlUpdateResourceGovernorV1> governor);
  EngineDmlUpdateResourceCaptureV1 Capture(const EngineRequestContext& context);
  void Revoke() noexcept;
  // Revocation is retained with outstanding owners after public receipt
  // removal. These observations grant no execution or durable authority.
  bool IsRevoked() const noexcept;
  bool SharesRuntimeWith(const EngineDmlUpdateResourceReceiptV1& other) const noexcept;
  bool BelongsToRuntime(const EngineDmlUpdateResourceGovernorV1* governor) const noexcept;
  // Live receipt owner check without capturing/revalidating a capacity grant.
  // Used only to authenticate terminal replay; never permits new mutation.
  bool AuthenticatesContext(const EngineRequestContext& context) const noexcept;
  EngineApiDiagnostic Revalidate(const EngineRequestContext& context,
      const EngineDmlUpdateResourceHandleV1& handle) const;
  EngineDmlUpdateResourcePublicationPreparationV1 PrepareDescriptorPublication(
      const EngineRequestContext& context, const EngineDmlUpdateResourceHandleV1& handle);
  EngineDmlUpdateResourceReleaseCodeV1 ReleaseNoAlloc(const EngineRequestContext& context,
      const EngineDmlUpdateResourceHandleV1& handle, EngineDmlUpdateResourceReleaseV1 reason) noexcept;
  bool Matches(const EngineRequestContext& context,
      const EngineDmlUpdateResourceGovernorV1* governor) const noexcept;
 private:
  const EngineRequestContext context_;
  const std::shared_ptr<EngineDmlUpdateResourceGovernorV1> governor_;
  std::atomic<bool> active_{true};
};

// Engine receipt teardown only: best-effort durable abortion of bound,
// unexecuted UPDATE descriptors. Revocation itself is never terminal evidence.
// Busy/recovery-required work retains its owner for the execution/recovery path.
void RetireDmlUpdateResourceReceiptDescriptorsV1(const EngineRequestContext& context) noexcept;
// Called under the transaction inventory guard before admitting another
// statement or finality. An unresolved owner is not permission to continue.
bool PrepareDmlUpdateTransactionFinalityV1(const EngineRequestContext& context) noexcept;

// Opportunistic engine cleanup: retry revoked owners in the caller receipt's
// engine instance and database only. Busy/failed owners remain retryable.
void RetryRetiredDmlUpdateResourceOwnersV1(const EngineRequestContext& context) noexcept;

// Shutdown is retryable, not an unconditional capacity release. Stop new
// admission, revoke this runtime's outstanding owners and reconcile their
// actual durable terminal states. False retains the runtime for a later drain
// after an executing statement or ambiguous I/O has settled.
bool DrainDmlUpdateResourceOwnersForRuntimeV1(
    const std::shared_ptr<EngineDmlUpdateResourceGovernorV1>& governor) noexcept;

}  // namespace scratchbird::engine::internal_api
