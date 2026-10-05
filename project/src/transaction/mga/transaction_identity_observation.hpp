// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction_state.hpp"
#include "uuid.hpp"
#include <utility>

namespace scratchbird::transaction::mga {
// Fixed diagnostic projection for guarded readers. All character pointers are
// static vocabulary, never caller data or textual UUID identity. Materialize
// only outside a fence that forbids allocation.
enum class MgaObservationOrigin { transaction_state, row_version };
struct MgaObservationStatus {
  Status status{core::platform::StatusCode::ok, core::platform::Severity::info,
                core::platform::Subsystem::transaction_mga};
  const char* diagnostic_code = "";
  const char* message_key = "";
  const char* detail = "";
  MgaObservationOrigin origin = MgaObservationOrigin::row_version;
  bool ok() const noexcept { return status.ok(); }
};
inline MgaObservationStatus ObserveTransactionIdentity(const TransactionIdentity& identity) noexcept {
  const auto fail=[](const char* code,const char* key) {
    return MgaObservationStatus{{core::platform::StatusCode::platform_required_feature_missing,
      core::platform::Severity::error,core::platform::Subsystem::transaction_mga},
      code,key,"",MgaObservationOrigin::transaction_state};
  };
  if(!identity.local_id.valid())
    return fail("SB-TXN-INVALID-LOCAL-TRANSACTION-ID","transaction.invalid_local_transaction_id");
  if(identity.transaction_uuid.kind!=core::platform::UuidKind::transaction||!identity.transaction_uuid.valid())
    return fail("SB-TXN-INVALID-TRANSACTION-UUID-KIND","transaction.invalid_transaction_uuid_kind");
  if(!core::uuid::IsEngineIdentityUuid(identity.transaction_uuid.value))
    return fail("SB-TXN-UUID-MUST-BE-V7","transaction.transaction_uuid_must_be_v7");
  if(identity.scope==TransactionScope::unknown)
    return fail("SB-TXN-UNKNOWN-TRANSACTION-SCOPE","transaction.unknown_transaction_scope");
  return {};
}
inline DiagnosticRecord MaterializeMgaObservationDiagnostic(const MgaObservationStatus& value) {
  if(!*value.diagnostic_code)return {};
  std::vector<core::platform::DiagnosticArgument> arguments;
  if(*value.detail)arguments.push_back({"detail",std::string(value.detail)});
  return core::platform::MakeDiagnostic(value.status.code,value.status.severity,value.status.subsystem,
    value.diagnostic_code,value.message_key,std::move(arguments),{},
    value.origin==MgaObservationOrigin::transaction_state?"transaction.mga.state":"transaction.mga.row_version");
}
} // namespace scratchbird::transaction::mga
