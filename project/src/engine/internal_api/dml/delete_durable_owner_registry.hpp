// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "dml/delete_binding_authority.hpp"
#include "dml/delete_recovery_authority.hpp"
#include "mga_relation_store/mga_delete_durable_store.hpp"

namespace scratchbird::engine::internal_api {
struct MgaVisibleHeapRelationStreamRequest;
struct MgaVisibleHeapRelationStreamResult;
struct DmlDeleteBoundPublicationV1 {
  bool ok = false;
  // Once true, the registry retains the original grant across all exceptions
  // and uncertain I/O. The caller must never release it independently.
  bool resource_ownership_taken = false;
  EngineApiDiagnostic diagnostic;
};
DmlDeleteBoundPublicationV1 PublishDmlDeleteBoundAuthorityV1(const EngineRequestContext&,
    const EngineDmlDeleteBindingAuthorityV1&, const EngineDmlUpdateResourceHandleV1&);

// Move-only descriptor/operation lock lease. Recovery leases have no live
// binding handle or manufactured resource grant and can never execute rows.
class DmlDeleteExecutionLeaseV1 final {
 public:
  struct Impl;
  ~DmlDeleteExecutionLeaseV1();
  const DmlDeleteDurableAuthorityBundleV1& bundle() const;
  MgaDmlDeleteDurableStoreV1& store();
  bool has_live_binding() const noexcept;
  EngineApiDiagnostic RevalidateLive(const EngineRequestContext&) const;
  EngineApiDiagnostic RevalidateResource(const EngineRequestContext&) const;
  EngineApiDiagnostic RevalidateRecovered(const EngineRequestContext&) const;
  MgaVisibleHeapRelationStreamResult StreamCandidates(
      const EngineRequestContext&, const MgaVisibleHeapRelationStreamRequest&) const;
  // Reconcile DDJR against actual MGA history. No row execution or new grant.
  EngineDmlDeleteRecoveryObservationV1 Reconcile();
  // Native barrier and exact staged DDJR publication followed by resource
  // completion. No allocation after the barrier; ambiguity retains ownership.
  MgaDmlDeletePublicationStatusV1 PublishAndRelease() noexcept;
 private:
  explicit DmlDeleteExecutionLeaseV1(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
  friend std::unique_ptr<DmlDeleteExecutionLeaseV1> AcquireDmlDeleteExecutionLeaseV1(
      const EngineRequestContext&, const wire::TypedUpdateUuid&, std::uint64_t,
      std::uint64_t, EngineApiDiagnostic*);
};
std::unique_ptr<DmlDeleteExecutionLeaseV1> AcquireDmlDeleteExecutionLeaseV1(
    const EngineRequestContext&, const wire::TypedUpdateUuid& descriptor_uuid,
    std::uint64_t descriptor_generation, std::uint64_t structural_occurrence_id,
    EngineApiDiagnostic*);
void RetireDmlDeleteResourceReceiptDescriptorsV1(const EngineRequestContext&) noexcept;
void RetryRetiredDmlDeleteResourceOwnersV1(const EngineRequestContext&) noexcept;
bool DrainDmlDeleteResourceOwnersForRuntimeV1(
    const std::shared_ptr<EngineDmlUpdateResourceGovernorV1>&) noexcept;
void ResetDmlDeleteBindingCacheForTestV1();
// Reconcile unfinished statements before publishing their owning transaction.
// A busy/uncertain owner refuses commit; no new grant or execution is issued.
bool PrepareDmlDeleteTransactionFinalityV1(const EngineRequestContext&) noexcept;
}  // namespace scratchbird::engine::internal_api
