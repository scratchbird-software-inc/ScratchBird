// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_unsupported.hpp"
#include "../../core/datatypes/datatype_time_diagnostic.hpp"
#include "../../core/datatypes/datatype_timestamp_diagnostic.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace scratchbird::engine::internal_api {

EngineApiDiagnostic MakeEngineApiDiagnostic(std::string code, std::string message_key, std::string detail, bool error) {
  EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.detail = std::move(detail);
  diagnostic.error = error;
  diagnostic.canonical_metadata =
      scratchbird::core::diagnostics::CaptureCanonicalDiagnosticMetadata(
          diagnostic.code);
  return diagnostic;
}

EngineApiDiagnostic MakeEngineApiDiagnosticFromNative(
    const scratchbird::core::platform::DiagnosticRecord& source,
    std::string code, std::string message_key, std::string detail, bool error) {
  auto diagnostic = MakeEngineApiDiagnostic(
      std::move(code), std::move(message_key), std::move(detail), error);
  diagnostic.native_source = scratchbird::core::diagnostics::NativeDiagnosticSource{
      source, scratchbird::core::diagnostics::CaptureCanonicalDiagnosticMetadata(
                  source.diagnostic_code)};
  return diagnostic;
}

namespace {
template <typename Fact>
void PreserveTemporalDiagnosticCause(
    EngineApiDiagnostic& diagnostic, const Fact& cause, std::string_view message_key) {
  using Kind = decltype(cause.parameters[0].kind);
  namespace cd = scratchbird::core::diagnostics;
  static_assert(std::is_nothrow_move_assignable_v<cd::NativeDiagnosticSource>);
  static_assert(std::is_nothrow_move_assignable_v<cd::NativeDatatypeDiagnosticFact>);
  if (cause.parameter_count > cause.parameters.size())
    throw std::invalid_argument("invalid temporal diagnostic parameter count");
  cd::NativeDatatypeDiagnosticFact owned;
  owned.status = cause.status;
  owned.diagnostic_code = cause.diagnostic_code;
  owned.detail = cause.detail;
  owned.parameter_count = cause.parameter_count;
  for (std::size_t i = 0; i < cause.parameters.size(); ++i) {
    const auto& source = cause.parameters[i];
    auto& target = owned.parameters[i];
    switch (source.kind) {
      case Kind::none: target.kind = cd::NativeDatatypeParameterKind::none; break;
      case Kind::unsigned_u64: target.kind = cd::NativeDatatypeParameterKind::unsigned_u64; break;
      case Kind::signed_i64: target.kind = cd::NativeDatatypeParameterKind::signed_i64; break;
      case Kind::uuid: target.kind = cd::NativeDatatypeParameterKind::uuid; break;
      case Kind::token: target.kind = cd::NativeDatatypeParameterKind::token; break;
      default: throw std::invalid_argument("invalid temporal diagnostic parameter kind");
    }
    target.name = source.name;
    target.unsigned_value = source.unsigned_value;
    target.signed_value = source.signed_value;
    target.uuid_value = source.uuid_value;
    target.token_value = source.token_value;
  }
  if (diagnostic.native_source) {
    diagnostic.native_source->datatype_cause = std::move(owned);
  } else {
    cd::NativeDiagnosticSource source;
    source.record.status = cause.status;
    source.record.diagnostic_code = cause.diagnostic_code;
    source.record.message_key = message_key;
    source.canonical_metadata = cd::CaptureCanonicalDiagnosticMetadata(cause.diagnostic_code);
    source.datatype_cause = std::move(owned);
    diagnostic.native_source = std::move(source);
  }
}

}  // namespace

void PreserveEngineApiTimeDiagnosticCause(
    EngineApiDiagnostic& diagnostic,
    const scratchbird::core::datatypes::TimeDiagnosticFactV3& cause) {
  PreserveTemporalDiagnosticCause(diagnostic, cause, "datatype.time.rejected");
}
void PreserveEngineApiTimestampDiagnosticCause(
    EngineApiDiagnostic& diagnostic,
    const scratchbird::core::datatypes::TimestampDiagnosticFactV3& cause) {
  PreserveTemporalDiagnosticCause(diagnostic, cause, "datatype.timestamp.rejected");
}

EngineApiDiagnostic MakeUnavailableDiagnostic(std::string operation_id) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_UNAVAILABLE",
                                 "engine.api.unavailable",
                                 std::move(operation_id));
}

EngineApiDiagnostic MakeUnsupportedProfileDiagnostic(std::string operation_id, std::string profile) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_UNSUPPORTED_PROFILE",
                                 "engine.api.unsupported_profile",
                                 operation_id + ":" + profile);
}

EngineApiDiagnostic MakeClusterAuthorityUnavailableDiagnostic(std::string operation_id) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE",
                                 "engine.api.cluster_authority_unavailable",
                                 std::move(operation_id));
}

EngineApiDiagnostic MakeSecurityContextRequiredDiagnostic(std::string operation_id) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED",
                                 "engine.api.security_context_required",
                                 std::move(operation_id));
}

EngineApiDiagnostic MakeInvalidRequestDiagnostic(std::string operation_id, std::string detail) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_INVALID_REQUEST",
                                 "engine.api.invalid_request",
                                 operation_id + ":" + detail);
}

EngineApiDiagnostic MakeEmbeddedTrustModeDiagnostic(std::string operation_id) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_EMBEDDED_TRUST_MODE",
                                 "engine.api.embedded_trust_mode",
                                 std::move(operation_id),
                                 false);
}

}  // namespace scratchbird::engine::internal_api
