// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_filespace_initialization.hpp"
#include "catalog_construction_intent.hpp"

namespace scratchbird::storage::database {
enum class NativeConstructionAuditError {
  // time_source_failure maps to registered TIME.SOURCE_FAILED and no record.
  none,invalid_request,cluster_requires_authority,identity_failure,time_source_failure
};
struct NativeConstructionAuditResult {
  NativeConstructionAuditError error=NativeConstructionAuditError::invalid_request;
  core::uuid::StandaloneUuidV7Error identity_error=core::uuid::StandaloneUuidV7Error::none;
  std::optional<core::catalog::CatalogConstructionIntent> event;
  core::platform::TypedUuid row_uuid;
  std::optional<core::time::ClockSnapshot> event_observation;
  bool ok()const noexcept{return error==NativeConstructionAuditError::none&&event&&event_observation;}
};
// Checked conversion of a real already-admitted observation; never reads a
// clock and never decodes UUID time. Only the acquiring owner supplies provenance.
std::optional<core::platform::u64> NativeConstructionAuditTimeMicros(core::platform::WallClockTime) noexcept;
// Retain the returned event/row with the original operation across physical
// failure/retry. Do not call again to hide unresolved effects. No effects,
// security admission, memory grant, publication or successful construction here.
// Standalone only, no actor until owning security authentication is available.
NativeConstructionAuditResult AcquireNativeConstructionIntent(
    const NativeFilespaceInitializationRequest&,const core::platform::Uuid& engine_uuid,
    core::uuid::StandaloneUuidV7Issuer&) noexcept;
bool NativeConstructionIntentMatchesRequest(
    const core::catalog::CatalogMetadataVersionView&,const NativeFilespaceInitializationRequest&);
} // namespace scratchbird::storage::database
