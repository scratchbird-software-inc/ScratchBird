// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "security/security_principal_lifecycle.hpp"
#include <memory>

namespace scratchbird::engine::internal_api {
struct EngineDmlDeleteSecurityAuthorityResultV1;
class EngineDmlDeleteSecurityAuthorityHandleV1 final {
 public:
  bool valid() const noexcept { return authority_ != nullptr; }
 private:
  struct Authority;
  std::shared_ptr<const Authority> authority_;
  friend EngineDmlDeleteSecurityAuthorityResultV1 CaptureDmlDeleteSecurityAuthorityV1(
      const EngineRequestContext&, const std::string&);
  friend EngineApiDiagnostic RevalidateDmlDeleteSecurityAuthorityV1(
      const EngineRequestContext&, const EngineDmlDeleteSecurityAuthorityResultV1&);
};

struct EngineDmlDeleteSecurityAuthorityResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineSecurityPolicySnapshotAuthorityV1 snapshot;
  // Privilege-source UUIDs: exact durable grants, or the independently
  // authenticated immutable SysArch bootstrap membership (never text tokens).
  std::vector<std::string> matched_grant_uuids;
  EngineDmlDeleteSecurityAuthorityHandleV1 handle;
};

// Independent DELETE privilege provider. The initial admitted profile requires
// absence of all target row policies in BOTH durable and materialized catalogs.
// UPDATE USING/WITH_CHECK projections do not confer DELETE policy authority.
// This handle alone grants neither mutation, replay nor transaction finality.
EngineDmlDeleteSecurityAuthorityResultV1 CaptureDmlDeleteSecurityAuthorityV1(
    const EngineRequestContext&, const std::string& target_relation_uuid);
EngineApiDiagnostic RevalidateDmlDeleteSecurityAuthorityV1(
    const EngineRequestContext&, const EngineDmlDeleteSecurityAuthorityResultV1&);
// Read-only source comparison for an independently authenticated durable
// owner. Does not register a snapshot, issue a handle, or authorize replay.
EngineApiDiagnostic RevalidateRecoveredDmlDeleteSecurityProjectionV1(
    const EngineRequestContext&, const EngineSecurityPolicySnapshotAuthorityV1&,
    const std::vector<std::string>& matched_grant_uuids);
}  // namespace scratchbird::engine::internal_api
