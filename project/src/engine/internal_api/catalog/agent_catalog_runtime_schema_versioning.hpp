// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-AGENT-CATALOG-RUNTIME-SCHEMA-VERSIONING-ANCHOR
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr std::uint32_t kAgentCatalogRuntimeSchemaCurrentVersion = 1;
inline constexpr std::uint32_t kAgentCatalogRuntimeSchemaMinReadableVersion = 1;
inline constexpr std::uint32_t kAgentCatalogRuntimeSchemaMinWritableVersion = 1;

inline constexpr const char* kAgentCatalogRuntimeSchemaOk =
    "AGENT.CATALOG_SCHEMA.OK";
inline constexpr const char* kAgentCatalogRuntimeSchemaBaselineCreated =
    "AGENT.CATALOG_SCHEMA.BASELINE_CREATED";
inline constexpr const char* kAgentCatalogRuntimeSchemaMissingRequiredSurface =
    "AGENT.CATALOG_SCHEMA.MISSING_REQUIRED_SURFACE";
inline constexpr const char* kAgentCatalogRuntimeSchemaMissingRequiredField =
    "AGENT.CATALOG_SCHEMA.MISSING_REQUIRED_FIELD";
inline constexpr const char* kAgentCatalogRuntimeSchemaMigrationRequired =
    "AGENT.CATALOG_SCHEMA.MIGRATION_REQUIRED";
inline constexpr const char* kAgentCatalogRuntimeSchemaMigrated =
    "AGENT.CATALOG_SCHEMA.MIGRATED";
inline constexpr const char* kAgentCatalogRuntimeSchemaIncompatibleVersion =
    "AGENT.CATALOG_SCHEMA.INCOMPATIBLE_VERSION";
inline constexpr const char* kAgentCatalogRuntimeSchemaReadOnlyRepairDenied =
    "AGENT.CATALOG_SCHEMA.READ_ONLY_REPAIR_DENIED";
inline constexpr const char* kAgentCatalogRuntimeSchemaRestrictedRepairDenied =
    "AGENT.CATALOG_SCHEMA.RESTRICTED_REPAIR_DENIED";
inline constexpr const char* kAgentCatalogRuntimeSchemaUnknownSurface =
    "AGENT.CATALOG_SCHEMA.UNKNOWN_SURFACE";

enum class AgentCatalogRuntimeSchemaSurfaceKind : std::uint16_t {
  catalog_sys_view_definition,
  durable_queue_record,
  runtime_record,
  policy_schema,
  metric_schema,
  storage_agent_runtime_record,
};

enum class AgentCatalogRuntimeSchemaOpenMode : std::uint16_t {
  read_write,
  read_only,
  restricted,
};

enum class AgentCatalogRuntimeSchemaRowState : std::uint16_t {
  current,
  baseline_created,
  missing_required_surface,
  missing_required_field,
  migration_required,
  migrated,
  incompatible_version,
  read_only_repair_denied,
  restricted_repair_denied,
  unknown_surface,
};

struct AgentCatalogRuntimeSchemaContract {
  std::string surface_id;
  AgentCatalogRuntimeSchemaSurfaceKind surface_kind =
      AgentCatalogRuntimeSchemaSurfaceKind::runtime_record;
  std::uint32_t current_version = kAgentCatalogRuntimeSchemaCurrentVersion;
  std::uint32_t min_readable_version = kAgentCatalogRuntimeSchemaMinReadableVersion;
  std::uint32_t min_writable_version = kAgentCatalogRuntimeSchemaMinWritableVersion;
  std::vector<std::string> required_fields;
  bool required_on_open = true;
  bool migratable_from_old = true;
  bool cluster_scoped = false;
  std::string derived_from;
};

struct AgentCatalogRuntimeSchemaObservation {
  std::string surface_id;
  std::uint32_t observed_version = kAgentCatalogRuntimeSchemaCurrentVersion;
  std::vector<std::string> present_fields;
  bool present = true;
};

struct AgentCatalogRuntimeSchemaValidationRequest {
  AgentCatalogRuntimeSchemaOpenMode open_mode =
      AgentCatalogRuntimeSchemaOpenMode::read_write;
  bool fresh_install = false;
  bool migration_requested = false;
  std::vector<AgentCatalogRuntimeSchemaObservation> observed_surfaces;
};

struct AgentCatalogRuntimeSchemaValidationRow {
  std::string surface_id;
  AgentCatalogRuntimeSchemaSurfaceKind surface_kind =
      AgentCatalogRuntimeSchemaSurfaceKind::runtime_record;
  AgentCatalogRuntimeSchemaRowState state =
      AgentCatalogRuntimeSchemaRowState::current;
  std::uint32_t expected_version = kAgentCatalogRuntimeSchemaCurrentVersion;
  std::uint32_t observed_version = 0;
  std::string diagnostic_code = kAgentCatalogRuntimeSchemaOk;
  std::string diagnostic_detail;
  std::vector<std::string> missing_fields;
  bool mutation_required = false;
  bool mutation_attempted = false;
};

struct AgentCatalogRuntimeSchemaValidationResult {
  bool ok = true;
  std::string diagnostic_code = kAgentCatalogRuntimeSchemaOk;
  std::string diagnostic_detail;
  std::vector<AgentCatalogRuntimeSchemaValidationRow> rows;
};

const char* AgentCatalogRuntimeSchemaSurfaceKindName(
    AgentCatalogRuntimeSchemaSurfaceKind kind);
const char* AgentCatalogRuntimeSchemaOpenModeName(
    AgentCatalogRuntimeSchemaOpenMode mode);
const char* AgentCatalogRuntimeSchemaRowStateName(
    AgentCatalogRuntimeSchemaRowState state);

const std::vector<AgentCatalogRuntimeSchemaContract>&
BuiltinAgentCatalogRuntimeSchemaContracts();

const AgentCatalogRuntimeSchemaContract* FindAgentCatalogRuntimeSchemaContract(
    std::string_view surface_id);

std::vector<AgentCatalogRuntimeSchemaObservation>
CurrentAgentCatalogRuntimeSchemaObservations();

AgentCatalogRuntimeSchemaValidationResult ValidateAgentCatalogRuntimeSchema(
    const AgentCatalogRuntimeSchemaValidationRequest& request);

}  // namespace scratchbird::engine::internal_api
