// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/agent_catalog_runtime_schema_versioning.hpp"
#include "catalog/sys_information_projection.hpp"
#include "database_lifecycle.hpp"
#include "engine_database_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

const api::AgentCatalogRuntimeSchemaContract& RequireContract(std::string_view surface_id) {
  const auto* contract = api::FindAgentCatalogRuntimeSchemaContract(surface_id);
  Require(contract != nullptr, std::string("missing schema contract: ") + std::string(surface_id));
  return *contract;
}

bool HasKind(api::AgentCatalogRuntimeSchemaSurfaceKind kind) {
  for (const auto& contract : api::BuiltinAgentCatalogRuntimeSchemaContracts()) {
    if (contract.surface_kind == kind) { return true; }
  }
  return false;
}

bool HasRowDiagnostic(const api::AgentCatalogRuntimeSchemaValidationResult& result,
                      std::string_view code) {
  for (const auto& row : result.rows) {
    if (row.diagnostic_code == code) { return true; }
  }
  return false;
}

api::AgentCatalogRuntimeSchemaObservation& ObservationFor(
    std::vector<api::AgentCatalogRuntimeSchemaObservation>* observations,
    std::string_view surface_id) {
  for (auto& observation : *observations) {
    if (observation.surface_id == surface_id) { return observation; }
  }
  Require(false, std::string("observation not found: ") + std::string(surface_id));
  return observations->front();
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1915000000000ull + seed);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
}

db::DatabaseLifecycleState RuntimeDatabase(db::DatabaseLifecyclePhase phase) {
  db::DatabaseLifecycleState database;
  database.path = "/tmp/pfar015b-runtime-route.sbdb";
  database.phase = phase;
  database.database_uuid = MakeUuid(platform::UuidKind::database, 1);
  database.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 2);
  return database;
}

void TestContractInventoryUsesImplementedSources() {
  const auto& contracts = api::BuiltinAgentCatalogRuntimeSchemaContracts();
  Require(!contracts.empty(), "PFAR-015B schema contract inventory is empty");
  Require(HasKind(api::AgentCatalogRuntimeSchemaSurfaceKind::catalog_sys_view_definition),
          "PFAR-015B sys view schema contracts missing");
  Require(HasKind(api::AgentCatalogRuntimeSchemaSurfaceKind::durable_queue_record),
          "PFAR-015B durable queue schema contracts missing");
  Require(HasKind(api::AgentCatalogRuntimeSchemaSurfaceKind::runtime_record),
          "PFAR-015B runtime record schema contracts missing");
  Require(HasKind(api::AgentCatalogRuntimeSchemaSurfaceKind::policy_schema),
          "PFAR-015B policy schema contracts missing");
  Require(HasKind(api::AgentCatalogRuntimeSchemaSurfaceKind::metric_schema),
          "PFAR-015B metric schema contracts missing");
  Require(HasKind(api::AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record),
          "PFAR-015B storage-agent runtime schema contracts missing");

  const auto& agents_contract = RequireContract("catalog.sys_view.sys.agents");
  const auto* agents_definition =
      api::FindSysInformationProjectionDefinition("sys.agents");
  Require(agents_definition != nullptr, "sys.agents projection definition missing");
  for (const auto& column : agents_definition->columns) {
    const std::string required_prefix =
        "column:" + column.column_name + ":" + column.logical_type + ":";
    const auto found = std::find_if(agents_contract.required_fields.begin(),
                                    agents_contract.required_fields.end(),
                                    [&](const auto& field) {
                                      return field.rfind(required_prefix, 0) == 0;
                                    });
    Require(found != agents_contract.required_fields.end(),
            "sys.agents schema contract does not track implemented projection column");
  }

  RequireContract("agent.policy.schema.page_preallocation_policy");
  RequireContract("agent.metric.schema.sb_page_free_count");
  RequireContract("storage.page_filespace_handoff.record.request_queue");
  RequireContract("storage.filespace_growth.record.ledger");
}

void TestRuntimeOpenRouteDefaultCurrentPasses() {
  const auto result = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened));
  Require(result.ok(), "PFAR-015B runtime open route refused current schema");
  Require(result.state.database_open, "PFAR-015B runtime open route did not open");
  Require(result.state.agent_catalog_schema_validated,
          "PFAR-015B runtime open route did not validate schema");
  Require(result.state.agent_catalog_schema_validation.ok,
          "PFAR-015B runtime open route schema validation failed");
  Require(result.state.agent_catalog_schema_validation.rows.size() ==
              api::BuiltinAgentCatalogRuntimeSchemaContracts().size(),
          "PFAR-015B runtime open route schema evidence row count mismatch");
}

void TestRuntimeCreatedFreshBaselineAndModeDenials() {
  const auto created = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::created));
  Require(created.ok(), "PFAR-015B runtime created route refused writable baseline");
  Require(created.state.agent_catalog_schema_validation.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaBaselineCreated,
          "PFAR-015B runtime created route baseline diagnostic mismatch");
  for (const auto& row : created.state.agent_catalog_schema_validation.rows) {
    Require(row.state == api::AgentCatalogRuntimeSchemaRowState::baseline_created,
            "PFAR-015B runtime created route row was not baseline_created");
    Require(row.mutation_required && !row.mutation_attempted,
            "PFAR-015B runtime created route must classify baseline without direct mutation");
  }

  auto read_only_database = RuntimeDatabase(db::DatabaseLifecyclePhase::created);
  read_only_database.read_only_open = true;
  const auto read_only = api::MakeEngineDatabaseRuntimeState(read_only_database);
  Require(!read_only.ok(), "PFAR-015B runtime created read-only route was admitted");
  Require(read_only.diagnostic.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaReadOnlyRepairDenied,
          "PFAR-015B runtime created read-only diagnostic mismatch");
  Require(read_only.state.agent_catalog_schema_validated,
          "PFAR-015B runtime created read-only route did not carry validation evidence");
  Require(!read_only.state.database_open,
          "PFAR-015B runtime created read-only route opened after schema refusal");

  api::EngineDatabaseRuntimeSchemaAdmissionOptions restricted_options;
  restricted_options.open_mode_override_present = true;
  restricted_options.open_mode = api::AgentCatalogRuntimeSchemaOpenMode::restricted;
  const auto restricted = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::created),
      {},
      {},
      restricted_options);
  Require(!restricted.ok(), "PFAR-015B runtime created restricted route was admitted");
  Require(restricted.diagnostic.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaRestrictedRepairDenied,
          "PFAR-015B runtime created restricted diagnostic mismatch");
  Require(!restricted.state.database_open,
          "PFAR-015B runtime created restricted route opened after schema refusal");
}

void TestRuntimeOpenRouteObservedSchemaRefusalsAndMigrationIntent() {
  auto observations = api::CurrentAgentCatalogRuntimeSchemaObservations();
  observations.erase(std::remove_if(observations.begin(),
                                    observations.end(),
                                    [](const auto& observation) {
                                      return observation.surface_id ==
                                             "catalog.sys_view.sys.agent_audit";
                                    }),
                     observations.end());
  api::EngineDatabaseRuntimeSchemaAdmissionOptions options;
  options.observed_schema_surfaces_present = true;
  options.observed_schema_surfaces = observations;
  const auto missing = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      options);
  Require(!missing.ok(), "PFAR-015B runtime open route admitted missing schema surface");
  Require(missing.diagnostic.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaMissingRequiredSurface,
          "PFAR-015B runtime open route missing surface diagnostic mismatch");
  Require(!missing.state.database_open,
          "PFAR-015B runtime open route opened after missing schema refusal");

  observations = api::CurrentAgentCatalogRuntimeSchemaObservations();
  ObservationFor(&observations, "storage.filespace_growth.record.ledger").observed_version = 0;
  options.observed_schema_surfaces = observations;
  options.migration_requested = false;
  const auto old_without_plan = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      options);
  Require(!old_without_plan.ok(),
          "PFAR-015B runtime open route admitted old schema without migration intent");
  Require(old_without_plan.diagnostic.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaMigrationRequired,
          "PFAR-015B runtime open route old schema diagnostic mismatch");

  options.migration_requested = true;
  const auto old_with_plan = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      options);
  Require(old_with_plan.ok(),
          "PFAR-015B runtime open route refused writable migration intent");
  Require(old_with_plan.state.agent_catalog_schema_validated,
          "PFAR-015B runtime open route did not preserve migration validation evidence");
  Require(HasRowDiagnostic(old_with_plan.state.agent_catalog_schema_validation,
                           api::kAgentCatalogRuntimeSchemaMigrated),
          "PFAR-015B runtime open route migration diagnostic row missing");
  for (const auto& row : old_with_plan.state.agent_catalog_schema_validation.rows) {
    Require(!row.mutation_attempted,
            "PFAR-015B runtime open route performed parser/client schema mutation");
  }

  observations = api::CurrentAgentCatalogRuntimeSchemaObservations();
  ObservationFor(&observations, "agent.policy.schema.page_preallocation_policy")
      .observed_version = api::kAgentCatalogRuntimeSchemaCurrentVersion + 1;
  options = {};
  options.observed_schema_surfaces_present = true;
  options.observed_schema_surfaces = observations;
  const auto future = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened),
      {},
      {},
      options);
  Require(!future.ok(), "PFAR-015B runtime open route admitted future schema");
  Require(future.diagnostic.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaIncompatibleVersion,
          "PFAR-015B runtime open route future schema diagnostic mismatch");
}

void TestFreshInstallBaselineAndModeRefusals() {
  api::AgentCatalogRuntimeSchemaValidationRequest fresh;
  fresh.fresh_install = true;
  fresh.open_mode = api::AgentCatalogRuntimeSchemaOpenMode::read_write;
  const auto baseline = api::ValidateAgentCatalogRuntimeSchema(fresh);
  Require(baseline.ok, "fresh writable PFAR-015B schema baseline was refused");
  Require(baseline.diagnostic_code == api::kAgentCatalogRuntimeSchemaBaselineCreated,
          "fresh writable PFAR-015B schema baseline diagnostic mismatch");
  Require(baseline.rows.size() == api::BuiltinAgentCatalogRuntimeSchemaContracts().size(),
          "fresh writable PFAR-015B schema baseline row count mismatch");
  for (const auto& row : baseline.rows) {
    Require(row.state == api::AgentCatalogRuntimeSchemaRowState::baseline_created,
            "fresh writable PFAR-015B row was not baseline_created");
    Require(row.mutation_required && !row.mutation_attempted,
            "fresh writable PFAR-015B row must require engine-owned baseline mutation only");
  }

  fresh.open_mode = api::AgentCatalogRuntimeSchemaOpenMode::restricted;
  const auto restricted = api::ValidateAgentCatalogRuntimeSchema(fresh);
  Require(!restricted.ok, "restricted PFAR-015B fresh baseline was admitted");
  Require(restricted.diagnostic_code == api::kAgentCatalogRuntimeSchemaRestrictedRepairDenied,
          "restricted PFAR-015B fresh baseline diagnostic mismatch");
  Require(HasRowDiagnostic(restricted, api::kAgentCatalogRuntimeSchemaRestrictedRepairDenied),
          "restricted PFAR-015B fresh baseline row diagnostic missing");
}

void TestCurrentOpenReadOnlyAccepted() {
  api::AgentCatalogRuntimeSchemaValidationRequest request;
  request.open_mode = api::AgentCatalogRuntimeSchemaOpenMode::read_only;
  request.observed_surfaces = api::CurrentAgentCatalogRuntimeSchemaObservations();
  const auto result = api::ValidateAgentCatalogRuntimeSchema(request);
  Require(result.ok, "read-only current PFAR-015B schema open was refused");
  Require(result.diagnostic_code == api::kAgentCatalogRuntimeSchemaOk,
          "read-only current PFAR-015B schema open diagnostic mismatch");
  for (const auto& row : result.rows) {
    Require(row.state == api::AgentCatalogRuntimeSchemaRowState::current,
            "read-only current PFAR-015B row was not current");
    Require(!row.mutation_required && !row.mutation_attempted,
            "read-only current PFAR-015B row requested mutation");
  }
}

void TestMissingSurfaceAndFieldRefuseWithoutMutation() {
  auto observations = api::CurrentAgentCatalogRuntimeSchemaObservations();
  observations.erase(std::remove_if(observations.begin(),
                                    observations.end(),
                                    [](const auto& observation) {
                                      return observation.surface_id ==
                                             "catalog.sys_view.sys.agent_evidence";
                                    }),
                     observations.end());
  api::AgentCatalogRuntimeSchemaValidationRequest request;
  request.observed_surfaces = observations;
  const auto missing_surface = api::ValidateAgentCatalogRuntimeSchema(request);
  Require(!missing_surface.ok, "missing PFAR-015B schema surface was admitted");
  Require(missing_surface.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaMissingRequiredSurface,
          "missing PFAR-015B schema surface diagnostic mismatch");
  Require(HasRowDiagnostic(missing_surface,
                           api::kAgentCatalogRuntimeSchemaMissingRequiredSurface),
          "missing PFAR-015B schema surface row diagnostic missing");
  for (const auto& row : missing_surface.rows) {
    Require(!row.mutation_attempted,
            "missing PFAR-015B schema surface attempted mutation before refusal");
  }

  observations = api::CurrentAgentCatalogRuntimeSchemaObservations();
  auto& agents = ObservationFor(&observations, "catalog.sys_view.sys.agents");
  Require(!agents.present_fields.empty(), "sys.agents observation fields are empty");
  agents.present_fields.pop_back();
  request.observed_surfaces = observations;
  const auto missing_field = api::ValidateAgentCatalogRuntimeSchema(request);
  Require(!missing_field.ok, "missing PFAR-015B schema field was admitted");
  Require(missing_field.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaMissingRequiredField,
          "missing PFAR-015B schema field diagnostic mismatch");
  Require(HasRowDiagnostic(missing_field,
                           api::kAgentCatalogRuntimeSchemaMissingRequiredField),
          "missing PFAR-015B schema field row diagnostic missing");
  for (const auto& row : missing_field.rows) {
    Require(!row.mutation_attempted,
            "missing PFAR-015B schema field attempted mutation before refusal");
  }
}

void TestOldFutureAndMigrationBehavior() {
  auto observations = api::CurrentAgentCatalogRuntimeSchemaObservations();
  ObservationFor(&observations, "storage.filespace_growth.record.ledger").observed_version = 0;
  api::AgentCatalogRuntimeSchemaValidationRequest request;
  request.observed_surfaces = observations;
  auto old_result = api::ValidateAgentCatalogRuntimeSchema(request);
  Require(!old_result.ok, "old PFAR-015B schema version was admitted without migration");
  Require(old_result.diagnostic_code == api::kAgentCatalogRuntimeSchemaMigrationRequired,
          "old PFAR-015B schema version diagnostic mismatch");
  Require(HasRowDiagnostic(old_result, api::kAgentCatalogRuntimeSchemaMigrationRequired),
          "old PFAR-015B schema version row diagnostic missing");

  request.migration_requested = true;
  request.open_mode = api::AgentCatalogRuntimeSchemaOpenMode::read_write;
  const auto migrated = api::ValidateAgentCatalogRuntimeSchema(request);
  Require(migrated.ok, "old PFAR-015B schema migration was refused in writable mode");
  Require(HasRowDiagnostic(migrated, api::kAgentCatalogRuntimeSchemaMigrated),
          "old PFAR-015B schema migration row diagnostic missing");
  for (const auto& row : migrated.rows) {
    Require(!row.mutation_attempted,
            "PFAR-015B schema migration validation performed mutation directly");
  }

  request.open_mode = api::AgentCatalogRuntimeSchemaOpenMode::read_only;
  const auto read_only_migration = api::ValidateAgentCatalogRuntimeSchema(request);
  Require(!read_only_migration.ok, "read-only PFAR-015B schema migration was admitted");
  Require(read_only_migration.diagnostic_code ==
              api::kAgentCatalogRuntimeSchemaReadOnlyRepairDenied,
          "read-only PFAR-015B schema migration diagnostic mismatch");

  observations = api::CurrentAgentCatalogRuntimeSchemaObservations();
  auto& future_policy = ObservationFor(&observations,
                                       "agent.policy.schema.page_preallocation_policy");
  future_policy.observed_version =
      api::kAgentCatalogRuntimeSchemaCurrentVersion + 1;
  request = {};
  request.observed_surfaces = observations;
  const auto future = api::ValidateAgentCatalogRuntimeSchema(request);
  Require(!future.ok, "future PFAR-015B schema version was admitted");
  Require(future.diagnostic_code == api::kAgentCatalogRuntimeSchemaIncompatibleVersion,
          "future PFAR-015B schema version diagnostic mismatch");
  Require(HasRowDiagnostic(future, api::kAgentCatalogRuntimeSchemaIncompatibleVersion),
          "future PFAR-015B schema version row diagnostic missing");
}

void TestUnknownSurfaceRefusal() {
  api::AgentCatalogRuntimeSchemaValidationRequest request;
  request.observed_surfaces = api::CurrentAgentCatalogRuntimeSchemaObservations();
  request.observed_surfaces.push_back(
      {"agent.catalog.record.unowned_parser_shape",
       api::kAgentCatalogRuntimeSchemaCurrentVersion,
       {"sql_text"},
       true});
  const auto result = api::ValidateAgentCatalogRuntimeSchema(request);
  Require(!result.ok, "unknown PFAR-015B schema surface was admitted");
  Require(result.diagnostic_code == api::kAgentCatalogRuntimeSchemaUnknownSurface,
          "unknown PFAR-015B schema surface diagnostic mismatch");
  Require(HasRowDiagnostic(result, api::kAgentCatalogRuntimeSchemaUnknownSurface),
          "unknown PFAR-015B schema surface row diagnostic missing");
}

}  // namespace

int main() {
  TestContractInventoryUsesImplementedSources();
  TestRuntimeOpenRouteDefaultCurrentPasses();
  TestRuntimeCreatedFreshBaselineAndModeDenials();
  TestRuntimeOpenRouteObservedSchemaRefusalsAndMigrationIntent();
  TestFreshInstallBaselineAndModeRefusals();
  TestCurrentOpenReadOnlyAccepted();
  TestMissingSurfaceAndFieldRefuseWithoutMutation();
  TestOldFutureAndMigrationBehavior();
  TestUnknownSurfaceRefusal();
  return EXIT_SUCCESS;
}
