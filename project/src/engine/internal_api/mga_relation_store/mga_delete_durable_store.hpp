// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "typed_delete_carrier_codec.hpp"
#include <memory>
#include <span>

namespace scratchbird::engine::internal_api {
struct DmlDeleteDurableAuthorityBundleV1;

// Private, statement-serialized storage primitive, not a descriptor/security
// capability. Its caller must authenticate the receipt and complete mutation
// graph and hold the engine transaction execution guard. The operation lock
// serializes durable chain replacement, not transaction finality.
// Payloads are canonical DDJR records and immutable DDAB companion material,
// with binary16 UUIDs. Raw storage success does not confer provider authority.
enum class MgaDmlDeletePublicationStatusV1 {
  refused, published, release_uncertain, publication_uncertain,
};
enum class MgaDmlDeleteAbortStatusV1 {
  refused, aborted, rewind_uncertain, release_uncertain, journal_uncertain,
};

class MgaDmlDeleteDurableStoreV1 final {
 public:
  static std::unique_ptr<MgaDmlDeleteDurableStoreV1> Open(
      const EngineRequestContext&, const wire::TypedUpdateUuid& descriptor_uuid,
      std::uint64_t descriptor_generation, EngineApiDiagnostic*);
  ~MgaDmlDeleteDurableStoreV1();
  MgaDmlDeleteDurableStoreV1(const MgaDmlDeleteDurableStoreV1&) = delete;
  MgaDmlDeleteDurableStoreV1& operator=(const MgaDmlDeleteDurableStoreV1&) = delete;

  std::span<const std::vector<std::uint8_t>> chain() const;
  // Structural persistence only, under the same operation lock as DDJR.
  // The authenticated coordinator must validate live providers first, then
  // fence this immutable bundle before appending bound. A standalone bundle
  // (including one left by failed binding) never permits execution.
  bool StoreAuthorityBundle(const DmlDeleteDurableAuthorityBundleV1&, EngineApiDiagnostic*);
  bool LoadAuthorityBundle(DmlDeleteDurableAuthorityBundleV1*, EngineApiDiagnostic*) const;
  // Admission/intent/prepared/aborted only. Published has its own MGA barrier.
  bool Append(const wire::TypedDeleteJournalRecord&, EngineApiDiagnostic*);
  // Encode, validate, write and fence the complete published successor BEFORE
  // release. The exact staged result is retained across process death.
  bool StagePublication(EngineApiDiagnostic*);
  // No encode/hash/allocation after successful native savepoint release.
  // Uncertain outcomes MUST be recovered, never reported as rolled back.
  MgaDmlDeletePublicationStatusV1 ReleaseAndPublish();
  // Reopen/recovery path: requires exact released native marker history and
  // prepared result. Never executes DELETE or decides transaction finality.
  MgaDmlDeletePublicationStatusV1 CompleteReleasedPublication();
  // Uses actual native marker history to rewind at most once, then retires
  // the internal boundary and stages' authority before durable aborted.
  // Exact retry of aborted is read-only. Never aborts a released publication.
  MgaDmlDeleteAbortStatusV1 AbortBeforePublication();

 private:
  struct Impl;
  explicit MgaDmlDeleteDurableStoreV1(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
}  // namespace scratchbird::engine::internal_api
