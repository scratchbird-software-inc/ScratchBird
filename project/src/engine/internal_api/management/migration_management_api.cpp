// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/management_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"

#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::string_view kMigrationKind = "migration_context";

std::string OptionValue(const EngineApiRequest& request, std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(std::string(prefix), 0) == 0) {
      return option.substr(prefix.size());
    }
  }
  return {};
}

void AddMigrationAuthorityEvidence(EngineApiResult* result,
                                   const EngineApiRequest& request,
                                   std::string_view operation_kind,
                                   std::string_view engine_api_function) {
  AddApiBehaviorEvidence(result, "migration_management_operation", std::string(operation_kind));
  AddApiBehaviorEvidence(result, "engine_api_function", std::string(engine_api_function));
  AddApiBehaviorEvidence(result, "engine_migration_authority", "local_node_management_api");
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "parser_finality_authority", "false");
  AddApiBehaviorEvidence(result, "reference_storage_authority_accepted", "false");
  AddApiBehaviorEvidence(result, "reference_finality_accepted", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
  AddApiBehaviorEvidence(result, "mga_authority_boundary", "engine_owned");
  AddApiBehaviorEvidence(result, "migration_right_required", "right.migrate_database");
}

EngineApiDiagnostic ValidateMigrationSecurity(const EngineApiRequest& request) {
  if (!request.context.security_context_present) {
    return MakeSecurityContextRequiredDiagnostic(request.operation_id);
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string MigrationActionState(std::string_view action) {
  if (action == "start") return "running";
  if (action == "pause") return "paused";
  if (action == "resume") return "running";
  if (action == "abort") return "aborted";
  if (action == "finalize") return "finalized";
  return {};
}

std::string MigrationPayload(const EngineApiRequest& request, std::string_view operation_kind) {
  std::string payload = ApiBehaviorPayloadFromRequest(request);
  if (!payload.empty()) payload.push_back(';');
  payload += "migration_operation=";
  payload += operation_kind;
  payload += ";reference_storage_authority_accepted=false;reference_finality_accepted=false";
  payload += ";mga_authority_boundary=engine_owned";
  return payload;
}

template <typename TResult>
TResult MigrationDiagnostic(const EngineApiRequest& request, EngineApiDiagnostic diagnostic) {
  return MakeApiBehaviorDiagnostic<TResult>(request.context, request.operation_id, std::move(diagnostic));
}

void AddMigrationRowsFromRecords(EngineApiResult* result,
                                 const EngineApiRequest& request,
                                 std::string_view filter_uuid) {
  const auto records = VisibleApiBehaviorRecords(request.context, std::string(kMigrationKind),
                                                request.context.local_transaction_id);
  for (const auto& record : records) {
    if (!filter_uuid.empty() && record.object_uuid != filter_uuid && record.default_name != filter_uuid) {
      continue;
    }
    AddApiBehaviorRow(result,
                      {{"migration_uuid", record.object_uuid},
                       {"migration_name", record.default_name},
                       {"state", record.state},
                       {"operation_id", record.operation_id},
                       {"payload", record.payload},
                       {"reference_storage_authority_accepted", "false"},
                       {"reference_finality_accepted", "false"},
                       {"mga_authority_boundary", "engine_owned"}});
  }
}

}  // namespace

EngineBeginMigrationResult EngineBeginMigration(const EngineBeginMigrationRequest& request) {
  if (auto diagnostic = ValidateMigrationSecurity(request); diagnostic.error) {
    return MigrationDiagnostic<EngineBeginMigrationResult>(request, std::move(diagnostic));
  }
  const std::string reference_profile = OptionValue(request, "reference_profile:");
  if (reference_profile.empty()) {
    return MigrationDiagnostic<EngineBeginMigrationResult>(
        request,
        MakeInvalidRequestDiagnostic(request.operation_id, "reference_profile_required"));
  }
  if (OptionValue(request, "reference_package:").empty()) {
    return MigrationDiagnostic<EngineBeginMigrationResult>(
        request,
        MakeInvalidRequestDiagnostic(request.operation_id, "reference_package_required"));
  }
  auto result = PersistedRecordResultWithPayload<EngineBeginMigrationResult>(
      request,
      request.operation_id.empty() ? "migration.begin_from_reference" : request.operation_id,
      std::string(kMigrationKind),
      true,
      "prepared",
      false,
      MigrationPayload(request, "begin_from_reference"));
  if (result.ok) {
    result.result_shape.result_kind = "rs.migration.status.v1";
    AddMigrationAuthorityEvidence(&result, request, "begin_from_reference", "EngineBeginMigration");
    AddApiBehaviorEvidence(&result, "reference_profile", reference_profile);
    AddApiBehaviorEvidence(&result, "reference_package_capability_validation", "passed_schema_only_no_private_execution");
  }
  return result;
}

EngineAlterMigrationResult EngineAlterMigration(const EngineAlterMigrationRequest& request) {
  if (auto diagnostic = ValidateMigrationSecurity(request); diagnostic.error) {
    return MigrationDiagnostic<EngineAlterMigrationResult>(request, std::move(diagnostic));
  }
  const std::string migration_ref = OptionValue(request, "migration_ref:");
  const std::string action = OptionValue(request, "migration_action:");
  if (migration_ref.empty()) {
    return MigrationDiagnostic<EngineAlterMigrationResult>(
        request,
        MakeInvalidRequestDiagnostic(request.operation_id, "migration_ref_required"));
  }
  const std::string state = MigrationActionState(action);
  if (state.empty()) {
    return MigrationDiagnostic<EngineAlterMigrationResult>(
        request,
        MakeInvalidRequestDiagnostic(request.operation_id, "migration_action_start_pause_resume_abort_or_finalize_required"));
  }
  EngineAlterMigrationRequest normalized = request;
  normalized.target_object.uuid.canonical = migration_ref;
  auto result = PersistedRecordResultWithPayload<EngineAlterMigrationResult>(
      normalized,
      request.operation_id.empty() ? "migration.alter" : request.operation_id,
      std::string(kMigrationKind),
      true,
      state,
      false,
      MigrationPayload(request, std::string("alter_") + action));
  if (result.ok) {
    result.result_shape.result_kind = "rs.migration.status.v1";
    AddMigrationAuthorityEvidence(&result,
                                  request,
                                  std::string("alter_") + action,
                                  "EngineAlterMigration");
    AddApiBehaviorEvidence(&result, "migration_ref", migration_ref);
  }
  return result;
}

EngineShowMigrationResult EngineShowMigration(const EngineShowMigrationRequest& request) {
  if (auto diagnostic = ValidateMigrationSecurity(request); diagnostic.error) {
    return MigrationDiagnostic<EngineShowMigrationResult>(request, std::move(diagnostic));
  }
  auto result = MakeApiBehaviorSuccess<EngineShowMigrationResult>(
      request.context,
      request.operation_id.empty() ? "migration.show" : request.operation_id);
  result.result_shape.result_kind = "rs.migration.status.v1";
  AddMigrationAuthorityEvidence(&result, request, "show_migration", "EngineShowMigration");
  const std::string migration_ref = OptionValue(request, "migration_ref:");
  AddMigrationRowsFromRecords(&result, request, migration_ref);
  if (result.result_shape.rows.empty()) {
    AddApiBehaviorRow(&result,
                      {{"migration_uuid", migration_ref},
                       {"state", "not_found_or_not_visible"},
                       {"reference_storage_authority_accepted", "false"},
                       {"reference_finality_accepted", "false"},
                       {"mga_authority_boundary", "engine_owned"}});
  }
  result.result_shape.result_kind = "rs.migration.status.v1";
  return result;
}

EngineShowMigrationsResult EngineShowMigrations(const EngineShowMigrationsRequest& request) {
  if (auto diagnostic = ValidateMigrationSecurity(request); diagnostic.error) {
    return MigrationDiagnostic<EngineShowMigrationsResult>(request, std::move(diagnostic));
  }
  auto result = MakeApiBehaviorSuccess<EngineShowMigrationsResult>(
      request.context,
      request.operation_id.empty() ? "migration.show_all" : request.operation_id);
  result.result_shape.result_kind = "rs.migration.list.v1";
  AddMigrationAuthorityEvidence(&result, request, "show_migrations", "EngineShowMigrations");
  AddMigrationRowsFromRecords(&result, request, {});
  if (result.result_shape.rows.empty()) {
    AddApiBehaviorRow(&result,
                      {{"migration_count", "0"},
                       {"state", "empty"},
                       {"reference_storage_authority_accepted", "false"},
                       {"reference_finality_accepted", "false"},
                       {"mga_authority_boundary", "engine_owned"}});
  }
  result.result_shape.result_kind = "rs.migration.list.v1";
  return result;
}

}  // namespace scratchbird::engine::internal_api
