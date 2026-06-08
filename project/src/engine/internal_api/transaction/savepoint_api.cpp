// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction/savepoint_api.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

namespace scratchbird::engine::internal_api {

namespace {
bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string SavepointName(const EngineApiRequest& request) {
  if (!request.localized_names.empty() && !request.localized_names.front().name.empty()) { return request.localized_names.front().name; }
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "savepoint_name:")) return option.substr(15);
    if (!option.empty() && option.find(':') == std::string::npos) return option;
  }
  return {};
}

EngineApiDiagnostic ValidateSavepointName(const std::string& operation_id, const std::string& savepoint_name) {
  if (savepoint_name.empty()) {
    return MakeInvalidRequestDiagnostic(operation_id, "savepoint_name_required");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

}

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_TRANSACTION_SAVEPOINT_API_BEHAVIOR
EngineCreateSavepointResult EngineCreateSavepoint(const EngineCreateSavepointRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineCreateSavepointResult>(request.context, "transaction.create_savepoint", MakeInvalidRequestDiagnostic("transaction.create_savepoint", "local_transaction_id_required"));
  }
  const auto savepoint_name = SavepointName(request);
  const auto name_status = ValidateSavepointName("transaction.create_savepoint", savepoint_name);
  if (name_status.error) { return MakeCrudDiagnosticResult<EngineCreateSavepointResult>(request.context, "transaction.create_savepoint", name_status); }
  const auto appended = CreateMgaSavepointMarker(request.context, savepoint_name);
  if (appended.error) { return MakeCrudDiagnosticResult<EngineCreateSavepointResult>(request.context, "transaction.create_savepoint", appended); }
  auto result = MakeCrudSuccessResult<EngineCreateSavepointResult>(request.context, "transaction.create_savepoint");
  result.evidence.push_back({"mga_savepoint", "savepoint_create"});
  result.evidence.push_back({"savepoint_name_bound", "true"});
  return result;
}

EngineReleaseSavepointResult EngineReleaseSavepoint(const EngineReleaseSavepointRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineReleaseSavepointResult>(request.context, "transaction.release_savepoint", MakeInvalidRequestDiagnostic("transaction.release_savepoint", "local_transaction_id_required"));
  }
  const auto savepoint_name = SavepointName(request);
  const auto name_status = ValidateSavepointName("transaction.release_savepoint", savepoint_name);
  if (name_status.error) { return MakeCrudDiagnosticResult<EngineReleaseSavepointResult>(request.context, "transaction.release_savepoint", name_status); }
  const auto savepoint_status = ValidateMgaSavepointExists(request.context, savepoint_name, "transaction.release_savepoint");
  if (savepoint_status.error) { return MakeCrudDiagnosticResult<EngineReleaseSavepointResult>(request.context, "transaction.release_savepoint", savepoint_status); }
  const auto appended = ReleaseMgaSavepointMarker(request.context, savepoint_name);
  if (appended.error) { return MakeCrudDiagnosticResult<EngineReleaseSavepointResult>(request.context, "transaction.release_savepoint", appended); }
  auto result = MakeCrudSuccessResult<EngineReleaseSavepointResult>(request.context, "transaction.release_savepoint");
  result.evidence.push_back({"mga_savepoint", "savepoint_release"});
  result.evidence.push_back({"savepoint_name_bound", "true"});
  return result;
}

EngineRollbackToSavepointResult EngineRollbackToSavepoint(const EngineRollbackToSavepointRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineRollbackToSavepointResult>(request.context, "transaction.rollback_to_savepoint", MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "local_transaction_id_required"));
  }
  const auto savepoint_name = SavepointName(request);
  const auto name_status = ValidateSavepointName("transaction.rollback_to_savepoint", savepoint_name);
  if (name_status.error) { return MakeCrudDiagnosticResult<EngineRollbackToSavepointResult>(request.context, "transaction.rollback_to_savepoint", name_status); }
  const auto savepoint_status = ValidateMgaSavepointExists(request.context, savepoint_name, "transaction.rollback_to_savepoint");
  if (savepoint_status.error) { return MakeCrudDiagnosticResult<EngineRollbackToSavepointResult>(request.context, "transaction.rollback_to_savepoint", savepoint_status); }
  const auto appended = RollbackToMgaSavepointMarker(request.context, savepoint_name);
  if (appended.error) { return MakeCrudDiagnosticResult<EngineRollbackToSavepointResult>(request.context, "transaction.rollback_to_savepoint", appended); }
  auto result = MakeCrudSuccessResult<EngineRollbackToSavepointResult>(request.context, "transaction.rollback_to_savepoint");
  result.evidence.push_back({"mga_savepoint", "savepoint_rollback"});
  result.evidence.push_back({"savepoint_name_bound", "true"});
  return result;
}

}  // namespace scratchbird::engine::internal_api
