// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::cli {

inline constexpr const char* kBootstrapPlatformProfileSchema =
    "scratchbird.bootstrap_platform_profile.v1";
inline constexpr const char* kBootstrapDeniedDiagnostic =
    "SECURITY.BOOTSTRAP_DENIED";

struct BootstrapPlatformProfile {
  std::string schema_id;
  std::string platform;
  std::string service_identity;
  std::string service_group;
};

struct BootstrapPlatformProfileLoadResult {
  bool ok = false;
  BootstrapPlatformProfile profile;
  // Protected local detail. Callers must never render this through the public
  // CLI or network diagnostic surface.
  std::string protected_audit_detail;
};

struct BootstrapOsAuthorityResult {
  bool ok = false;
  bool service_identity_assumed = false;
  bool ownership_handoff_ready = false;
  std::string service_identity;
  std::string service_group;
  std::string safe_diagnostic_code = kBootstrapDeniedDiagnostic;
  // Protected local detail. Callers must never render this through the public
  // CLI or network diagnostic surface.
  std::string protected_audit_detail;
};

BootstrapPlatformProfileLoadResult LoadBootstrapPlatformProfile(
    const std::string& path);

// Validate the complete profile contract for an explicitly selected platform.
// This is also used by cross-platform tests so the Windows identity contract
// can be verified without requiring a Windows host.
bool BootstrapPlatformProfileContractValidForPlatform(
    const BootstrapPlatformProfile& profile,
    std::string_view expected_platform);

// A process group set must contain the configured ScratchBird group and no
// other group. There is intentionally no exception surface: the dedicated OS
// identity carries filesystem/process authority only.
bool BootstrapServiceIdentityGroupSetIsLeastAuthority(
    const std::vector<std::uint64_t>& resolved_group_ids,
    std::uint64_t configured_group_id);

// Verify the current POSIX process credential through the platform's kernel
// group-list surface. On Darwin this deliberately bypasses the unlimited
// directory-service getgroups variant, which reports account defaults rather
// than a group set changed by launchd or setgroups.
bool BootstrapCurrentProcessHasOnlyConfiguredGroup(
    std::uint64_t configured_group_id);

// Windows managed virtual service accounts receive filesystem access by
// direct ACL only. An incomplete local-group inventory or any explicit local
// group membership therefore fails closed. This pure contract is testable on
// every build platform; the Windows runtime populates it through NetAPI.
bool BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
    bool inventory_complete,
    const std::vector<bool>& explicit_membership_rows);

// Verify the non-cacheable bootstrap.os_administrator_service_handoff method.
// Root/Administrator is the sole create-time OS authorization gate. The
// configured service identity and group carry no database or security
// authority: on POSIX the authorized root caller permanently drops to that
// least-authority identity for ownership/execution, while Windows validates the
// exact NT SERVICE\scratchbird SID and performs an explicit ACL handoff.
BootstrapOsAuthorityResult VerifyAndAssumeBootstrapServiceIdentity(
    const BootstrapPlatformProfile& profile);

BootstrapOsAuthorityResult EnsureBootstrapServiceIdentityAccess(
    const BootstrapPlatformProfile& profile,
    const std::string& database_path);

}  // namespace scratchbird::cli
