// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_DIAGNOSTICS

#pragma once

#include "../core/uuid/diagnostic_identity.hpp"
#include "../core/platform/runtime_platform.hpp"
#include "../server_engine_bridge/diagnostic_fields.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <exception>

namespace scratchbird::core::uuid { enum class CryptoBootstrapDiagnosticError; }

namespace scratchbird::server {

// Trusted host entry only, before provider readiness. This is an explicit
// source selection, never a fallback from failed ordinary UUID generation.
class BootstrapDiagnosticScope final {
 public:
  BootstrapDiagnosticScope() noexcept;
  ~BootstrapDiagnosticScope();
  BootstrapDiagnosticScope(const BootstrapDiagnosticScope&)=delete;
  BootstrapDiagnosticScope& operator=(const BootstrapDiagnosticScope&)=delete;
 private:
  bool previous_;
};
class BootstrapDiagnosticIdentityFailure final : public std::exception {
 public:
  explicit BootstrapDiagnosticIdentityFailure(core::uuid::CryptoBootstrapDiagnosticError error) noexcept
      : error_(error) {}
  const char* what() const noexcept override {return "Startup diagnostic identity unavailable";}
  core::uuid::CryptoBootstrapDiagnosticError error() const noexcept {return error_;}
 private:
  core::uuid::CryptoBootstrapDiagnosticError error_;
};
std::array<std::uint8_t,16> NewServerDiagnosticOccurrenceUuid();
// Fixed, noncanonical terminal status; no UUID or recursive diagnostic. Caller
// must terminate startup regardless of delivery success. Descriptor is owned
// by the caller, not closed here; writes have a bounded retry count, not a
// storage-I/O deadline. POSIX nonregular sinks must already be nonblocking;
// flags are never changed here. The caller must retain exclusive descriptor
// lifecycle/flag control and handle broken-pipe signals. Regular-file and
// Windows sinks require a host-qualified latency contract; delivery is best
// effort and must not be used as proof of bounded shutdown completion.
bool WriteBootstrapDiagnosticFailureStatus(int descriptor,
    core::uuid::CryptoBootstrapDiagnosticError error) noexcept;

enum class ServerDiagnosticSeverity {
  kInfo,
  kWarning,
  kError,
};

struct ServerDiagnosticField {
  std::string key;
  std::string value;
};

struct ServerDiagnostic {
  std::string code;
  std::string message_key;
  ServerDiagnosticSeverity severity = ServerDiagnosticSeverity::kError;
  std::string safe_message;
  std::vector<ServerDiagnosticField> fields;
  std::string diagnostic_shape_id;
  bool retryable = false;
  std::string correlation_uuid;
  std::string request_uuid;
  std::string session_uuid;
  std::string database_uuid;
  // Server-only deterministic branch identity. Never serialized to clients.
  std::string internal_audit_key;
  // Native identity evidence is kept separate from human-readable fields.
  std::vector<std::pair<std::string, core::platform::Uuid>> identity_fields;
  // Newly emitted server records own this identity. An engine-to-server
  // adapter must preserve the engine occurrence instead of using this new ID.
  std::array<std::uint8_t, 16> occurrence_uuid =
      NewServerDiagnosticOccurrenceUuid();
  // Trusted engine source, not parser-safe payload. Retain exact key, native
  // cause, canonical severity/retry/outcome and fields until the owning bridge
  // applies actual template/redaction authority. Legacy serializers ignore it.
  std::optional<scratchbird::server_engine_bridge::EngineDiagnosticSnapshot>
      engine_source_snapshot;
  // Owned native platform cause for non-engine startup/configuration failures.
  // This is private source data, not permission for a legacy text renderer to
  // disclose UUID arguments or other protected values.
  std::optional<core::platform::DiagnosticRecord> native_platform_source;
};

// Adopt only the matching source with a valid binary occurrence identity and
// exact registration/public-severity consistency. Unregistered private sources
// remain explicitly unregistered; this does not authorize canonical rendering.
// Failure leaves the complete target unchanged, including allocation failure.
bool AdoptEngineDiagnosticSource(
    const scratchbird::server_engine_bridge::EngineDiagnosticSnapshot& source,
    ServerDiagnostic* target);

const char* SeverityName(ServerDiagnosticSeverity severity);
std::string EscapeMessageVectorText(const std::string& value);
bool LooksLikeCanonicalUuid(std::string_view value);
bool IsPublicDiagnosticFieldAllowed(std::string_view key, std::string_view value);
std::string DiagnosticShapeIdForCode(std::string_view code);
std::string ToMessageVectorJsonLine(const ServerDiagnostic& diagnostic);
std::string ToPrivateMessageVectorJsonLine(const ServerDiagnostic& diagnostic);

}  // namespace scratchbird::server
