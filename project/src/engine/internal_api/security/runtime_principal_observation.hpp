// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "security_principal_lifecycle.hpp"
#include "uuid.hpp"
#include <optional>

namespace scratchbird::engine::internal_api {

enum class RuntimePrincipalObservationOutcome {
  observed, invalid_request, authority_unavailable, no_active_principal, invalid_source
};

struct RuntimePrincipalObservationRequest {
  std::string database_path;
  EngineUuid database_uuid;
  EngineUuid principal_uuid;
};

// Protected internal source facts only. No credential, name-based fallback,
// grant, authentication assertion, bearer receipt or recovery permission.
// A later authorization decision must revalidate under its owning fence;
// this observation does not hold that fence or keep the principal active.
struct RuntimePrincipalObservation {
  EngineUuid database_uuid;
  EngineUuid principal_uuid;
  std::string principal_kind;
  std::string lifecycle_state;
  bool deleted = false;
  std::uint64_t principal_generation = 0;
  std::uint64_t security_context_generation = 0;
  std::uint64_t security_generation = 0;
  std::uint64_t policy_generation = 0;
};

struct RuntimePrincipalObservationResult {
  RuntimePrincipalObservationOutcome outcome = RuntimePrincipalObservationOutcome::invalid_request;
  std::optional<RuntimePrincipalObservation> observation;
  // Preserve a failed owning read, without turning it into missing-principal
  // evidence. Invalid input and successful reads do not invent diagnostics.
  std::optional<EngineApiDiagnostic> source_diagnostic;
};

// Uses only current MGA-committed native security state. No caller context,
// supplied lifecycle state, transaction, trace tag or authority flag is accepted.
// The owning loader retains disabled principal records for lifecycle work;
// this active-only observation maps absence, disable and deletion to
// no_active_principal. This is not an audit reader.
// The caller must configure the owning default memory manager first. This read
// neither provisions a memory budget nor bypasses native allocation admission.
inline RuntimePrincipalObservationResult InspectRuntimePrincipal(
    const RuntimePrincipalObservationRequest& request) {
  using Outcome = RuntimePrincipalObservationOutcome;
  if (request.database_path.empty() || request.database_path.find('\0') != std::string::npos ||
      !core::uuid::IsEngineIdentityUuid(request.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(request.principal_uuid)) return {};
  EngineRequestContext context;
  context.database_path = request.database_path;
  context.database_uuid = request.database_uuid;
  // No local transaction: the owning loader selects latest committed state,
  // validates the actual database identity and authenticates its native chain.
  const auto loaded = LoadSecurityPrincipalLifecycleState(context);
  if (!loaded.ok) return {Outcome::authority_unavailable, {}, loaded.diagnostic};
  const auto& state = loaded.state;
  if (state.security_context_generation == 0 || state.security_generation == 0 ||
      state.policy_generation == 0) return {Outcome::invalid_source, {}, {}};
  const EngineSecurityPrincipalRecord* selected = nullptr;
  for (const auto& principal : state.principals) {
    if (principal.principal_uuid != request.principal_uuid) continue;
    if (selected != nullptr || principal.security_generation == 0)
      return {Outcome::invalid_source, {}, {}};
    selected = &principal;
  }
  if (selected == nullptr) return {Outcome::no_active_principal, {}, {}};
  if (selected->deleted || selected->lifecycle_state == "disabled")
    return {Outcome::no_active_principal, {}, {}};
  if (selected->lifecycle_state != "active")
    return {Outcome::invalid_source, {}, {}};
  return {Outcome::observed,
      RuntimePrincipalObservation{request.database_uuid, selected->principal_uuid,
          selected->principal_kind, selected->lifecycle_state, selected->deleted,
          selected->security_generation, state.security_context_generation,
          state.security_generation, state.policy_generation}, {}};
}

} // namespace scratchbird::engine::internal_api
