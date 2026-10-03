// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
struct RuntimePrincipalCredentialRequest {
  // Exact path of the already-owned routed database, not a caller-selected alias.
  std::string database_path;
  EngineUuid database_uuid;
  EngineUuid principal_uuid;
  // Borrowed only for this synchronous verification; never retained by the lease.
  std::string_view password;
};
enum class RuntimePrincipalCredentialError {
  none, invalid_request, source_failure, credential_rejected,
  resource_exhausted, lock_failure
};
struct RuntimePrincipalCredentialResult;

// Engine-private local-password verification component, NOT provider selection,
// an authenticated session, materialized rights, startup permission or a BEGIN
// receipt. The owning admission path must separately establish and retain its
// configured provider/configuration fences; external authorities cannot use this
// as a local fallback. It creates no session, transaction, grant or generation.
//
// Holds the actual guard used by engine security append and transaction finality.
// Consume and destroy on the acquiring thread. Never recursively mutate security
// or finalize transactions on that thread while consuming the lease. Cross-process
// exclusion and exact path routing remain the database owner's responsibility.
class RuntimePrincipalCredentialLease final {
 public:
  RuntimePrincipalCredentialLease(const RuntimePrincipalCredentialLease&) = delete;
  RuntimePrincipalCredentialLease& operator=(const RuntimePrincipalCredentialLease&) = delete;
  const EngineUuid& database_uuid() const noexcept { return database_; }
  const EngineUuid& principal_uuid() const noexcept { return principal_; }
  std::uint64_t security_context_generation() const noexcept { return context_generation_; }
  std::uint64_t security_generation() const noexcept { return security_generation_; }
  std::uint64_t policy_generation() const noexcept { return policy_generation_; }
 private:
  friend RuntimePrincipalCredentialResult VerifyRuntimePrincipalCredential(
      const RuntimePrincipalCredentialRequest&) noexcept;
  RuntimePrincipalCredentialLease(std::unique_lock<std::recursive_mutex> guard,
      EngineUuid database, EngineUuid principal, std::uint64_t context_generation,
      std::uint64_t security_generation, std::uint64_t policy_generation)
      : guard_(std::move(guard)), database_(database), principal_(principal),
        context_generation_(context_generation), security_generation_(security_generation),
        policy_generation_(policy_generation) {}
  std::unique_lock<std::recursive_mutex> guard_;
  EngineUuid database_, principal_;
  std::uint64_t context_generation_, security_generation_, policy_generation_;
};
struct RuntimePrincipalCredentialResult {
  RuntimePrincipalCredentialError error = RuntimePrincipalCredentialError::invalid_request;
  std::unique_ptr<RuntimePrincipalCredentialLease> lease;
  std::optional<EngineApiDiagnostic> source_diagnostic;
  bool ok() const noexcept {
    return error == RuntimePrincipalCredentialError::none && lease != nullptr;
  }
};
// Latest committed active service principal only. No names, caller transaction,
// supplied lifecycle state, provider options, trust booleans or trace-tag rights.
// The returned event generations are not configuration epochs. The caller must
// already configure the actual owning memory manager; no budget is provisioned.
RuntimePrincipalCredentialResult VerifyRuntimePrincipalCredential(
    const RuntimePrincipalCredentialRequest&) noexcept;
} // namespace scratchbird::engine::internal_api
