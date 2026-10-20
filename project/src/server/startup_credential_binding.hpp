// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "database_lifecycle.hpp"
#include "startup_credential_frame.hpp"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace scratchbird::server {

// Builds the transport binding a startup credential is framed against, using
// only identities this database actually committed.
//
// Every identity comes from the committed runtime authority binding that
// database creation wrote: the database, the service principal, the provider
// and the credential reference. None is derived from a name, a key, a seed or
// a configured string. That rule is the point of this function rather than an
// incidental property of it: a derived internal identity is indistinguishable
// at the call site from a read one, and the fabrications removed from the agent
// runtime were exactly that shape.
//
// The custodian is the one field that is not catalog state, because it is not
// an authority claim. It is this process's effective user, an operating-system
// custody expectation checked against the descriptor the adapter seals. It
// grants nothing.

enum class StartupCredentialBindingError {
  none,
  binding_absent,              // No committed runtime authority binding.
  credential_reference_absent, // Committed binding carries no credential reference.
  generation_absent,           // Committed binding generation is zero.
  unsupported_platform,
};

struct StartupCredentialBindingResult {
  StartupCredentialBindingError error = StartupCredentialBindingError::binding_absent;
  StartupCredentialTransportBinding binding;

  bool ok() const noexcept {
    return error == StartupCredentialBindingError::none;
  }
};

inline const char* StartupCredentialBindingDiagnosticCode(
    StartupCredentialBindingError error) noexcept {
  using E = StartupCredentialBindingError;
  switch (error) {
    case E::none: return "";
    case E::binding_absent:
      return "BOOTSTRAP.RUNTIME_AUTHORITY_BINDING_ABSENT";
    case E::credential_reference_absent:
      return "BOOTSTRAP.RUNTIME_AUTHORITY_CREDENTIAL_REFERENCE_ABSENT";
    case E::generation_absent:
      return "BOOTSTRAP.RUNTIME_AUTHORITY_GENERATION_ABSENT";
    case E::unsupported_platform:
      return "BOOTSTRAP.STARTUP_CREDENTIAL_UNSUPPORTED_PLATFORM";
  }
  return "BOOTSTRAP.RUNTIME_AUTHORITY_BINDING_ABSENT";
}

// Returns a refusal rather than a partially populated binding whenever the
// committed state does not supply every identity. A caller cannot accidentally
// proceed on a binding whose provider or credential reference was defaulted,
// because no such binding is ever produced.
inline StartupCredentialBindingResult BuildStartupCredentialBinding(
    const storage::database::DatabaseBootstrapSecurityCatalogState& state,
    const core::platform::Uuid& database_uuid) {
  using E = StartupCredentialBindingError;
  StartupCredentialBindingResult result;
#if !defined(__linux__)
  (void)state;
  (void)database_uuid;
  result.error = E::unsupported_platform;
  return result;
#else
  if (!state.present || !state.committed_by_inventory ||
      !state.runtime_authority_binding_present) {
    result.error = E::binding_absent;
    return result;
  }
  if (!state.credential_reference_uuid.has_value()) {
    result.error = E::credential_reference_absent;
    return result;
  }
  if (state.runtime_authority_generation == 0) {
    result.error = E::generation_absent;
    return result;
  }

  result.binding.database = database_uuid;
  result.binding.principal = state.service_principal_uuid;
  result.binding.provider = state.provider_uuid;
  result.binding.credential_reference = *state.credential_reference_uuid;
  result.binding.generation = state.runtime_authority_generation;
  result.binding.custodian_uid = static_cast<std::uint32_t>(::geteuid());

  // The frame encoder requires every identity to be a real engine identity.
  // Checking here means a malformed committed binding is a refusal at the
  // point it is read, not a framing error later with less context.
  if (!ValidStartupCredentialFrameBinding(result.binding)) {
    result.binding = {};
    result.error = E::binding_absent;
    return result;
  }
  result.error = E::none;
  return result;
#endif
}

}  // namespace scratchbird::server
