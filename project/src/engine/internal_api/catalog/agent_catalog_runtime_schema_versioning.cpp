// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/agent_catalog_runtime_schema_versioning.hpp"

#include "agent_runtime.hpp"
#include "catalog/sys_information_projection.hpp"
#include "metric_registry.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents;
namespace metrics = scratchbird::core::metrics;

constexpr std::string_view kDirectAgentStorageViews[] = {
    "sys.agents",
    "sys.agent_metric_dependencies",
    "sys.agent_policies",
    "sys.agent_actions",
    "sys.agent_overrides",
    "sys.agent_evidence",
    "sys.agent_audit",
    "sys.filespace_capacity_agent_state",
    "sys.page_allocation_agent_state",
    "sys.filespace_shrink_readiness",
};

std::string KindPrefix(AgentCatalogRuntimeSchemaSurfaceKind kind) {
  switch (kind) {
    case AgentCatalogRuntimeSchemaSurfaceKind::catalog_sys_view_definition:
      return "catalog_sys_view";
    case AgentCatalogRuntimeSchemaSurfaceKind::durable_queue_record:
      return "durable_queue";
    case AgentCatalogRuntimeSchemaSurfaceKind::runtime_record:
      return "runtime_record";
    case AgentCatalogRuntimeSchemaSurfaceKind::policy_schema:
      return "policy_schema";
    case AgentCatalogRuntimeSchemaSurfaceKind::metric_schema:
      return "metric_schema";
    case AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record:
      return "storage_agent_runtime_record";
  }
  return "runtime_record";
}

AgentCatalogRuntimeSchemaContract Contract(
    std::string surface_id,
    AgentCatalogRuntimeSchemaSurfaceKind kind,
    std::vector<std::string> required_fields,
    std::string derived_from,
    bool cluster_scoped = false) {
  AgentCatalogRuntimeSchemaContract contract;
  contract.surface_id = std::move(surface_id);
  contract.surface_kind = kind;
  contract.required_fields = std::move(required_fields);
  contract.derived_from = std::move(derived_from);
  contract.cluster_scoped = cluster_scoped;
  return contract;
}

std::vector<std::string> UniqueSorted(std::vector<std::string> values) {
  values.erase(std::remove(values.begin(), values.end(), std::string{}), values.end());
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::vector<std::string> SplitLayoutFields(std::string_view layout) {
  const auto open = layout.find('(');
  const auto close = layout.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
    return {};
  }
  std::vector<std::string> fields;
  std::string_view remaining = layout.substr(open + 1, close - open - 1);
  while (!remaining.empty()) {
    const auto comma = remaining.find(',');
    const auto token =
        comma == std::string_view::npos ? remaining : remaining.substr(0, comma);
    if (!token.empty()) { fields.emplace_back(token); }
    if (comma == std::string_view::npos) { break; }
    remaining.remove_prefix(comma + 1);
  }
  return fields;
}

std::string SplitLayoutName(std::string_view layout) {
  const auto open = layout.find('(');
  return std::string(open == std::string_view::npos ? layout : layout.substr(0, open));
}

void AddSysViewContracts(std::vector<AgentCatalogRuntimeSchemaContract>* contracts) {
  for (const auto view_path : kDirectAgentStorageViews) {
    const auto* definition = FindSysInformationProjectionDefinition(view_path);
    if (definition == nullptr) { continue; }
    std::vector<std::string> fields = {
        "view_path",
        "view_scope",
        "family",
        "authorization_filter_required",
        "redaction_required",
        "mga_snapshot_visibility_required",
        "cluster_path_fail_closed",
    };
    for (const auto& column : definition->columns) {
      std::ostringstream field;
      field << "column:" << column.column_name << ':' << column.logical_type << ':'
            << (column.nullable ? "nullable" : "required");
      if (column.exposes_internal_uuid) { field << ":resolver_uuid"; }
      fields.push_back(field.str());
    }
    for (const auto& key : definition->key_columns) {
      fields.push_back("key:" + key);
    }
    contracts->push_back(Contract(std::string("catalog.sys_view.") +
                                      std::string(view_path),
                                  AgentCatalogRuntimeSchemaSurfaceKind::
                                      catalog_sys_view_definition,
                                  UniqueSorted(std::move(fields)),
                                  "BuiltinSysInformationProjectionDefinitions"));
  }
}

void AddAgentRuntimeRecordContracts(
    std::vector<AgentCatalogRuntimeSchemaContract>* contracts) {
  for (const auto& layout : agents::AgentCatalogRecordLayouts()) {
    const std::string name = SplitLayoutName(layout);
    contracts->push_back(Contract("agent.catalog.record." + name,
                                  AgentCatalogRuntimeSchemaSurfaceKind::runtime_record,
                                  UniqueSorted(SplitLayoutFields(layout)),
                                  "AgentCatalogRecordLayouts"));
  }
  contracts->push_back(Contract(
      "agent.runtime.record.agent_action_request",
      AgentCatalogRuntimeSchemaSurfaceKind::runtime_record,
      {"action_class", "action_uuid", "actuator_id", "agent_type_id", "dry_run",
       "idempotency_key", "inputs", "instance_uuid", "manual_approval_present",
       "operation_id", "operator_override"},
      "AgentActionRequest"));
  contracts->push_back(Contract(
      "agent.runtime.record.agent_action_decision",
      AgentCatalogRuntimeSchemaSurfaceKind::runtime_record,
      {"detail", "diagnostic_code", "evidence_uuid", "mutates_state", "result_class"},
      "AgentActionDecision"));
}

void AddAgentPolicyContracts(std::vector<AgentCatalogRuntimeSchemaContract>* contracts) {
  std::map<std::string, std::vector<std::string>> policies;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    for (const auto& family : agents::RequiredPolicyFamiliesForAgent(descriptor)) {
      policies.try_emplace(family, agents::RequiredPolicyConfigFieldsForFamily(family));
    }
  }
  for (auto& [family, fields] : policies) {
    fields.push_back("schema_version");
    fields.push_back("policy_family");
    fields.push_back("policy_generation");
    contracts->push_back(Contract("agent.policy.schema." + family,
                                  AgentCatalogRuntimeSchemaSurfaceKind::policy_schema,
                                  UniqueSorted(std::move(fields)),
                                  "RequiredPolicyConfigFieldsForFamily"));
  }
}

void AddAgentMetricContracts(std::vector<AgentCatalogRuntimeSchemaContract>* contracts) {
  std::set<std::string> metric_families;
  for (const auto& row : agents::AgentMetricDependencyContractRegistry()) {
    const auto& dep = row.dependency;
    metric_families.insert(dep.metric_family);
    contracts->push_back(Contract(
        "agent.metric_dependency.schema." + row.agent_type_id + "." + dep.metric_family,
        AgentCatalogRuntimeSchemaSurfaceKind::metric_schema,
        {"agent_type_id", "aggregation", "cluster_only", "decision_use",
         "dependency_kind", "evidence_field", "fail_behavior", "max_freshness_microseconds",
         "metric_family", "namespace_prefix", "policy_field", "required",
         "required_quality", "required_source_quality"},
        "AgentMetricDependencyContractRegistry",
        dep.cluster_only));
  }
  for (const auto& family : metric_families) {
    const auto* descriptor = metrics::DefaultMetricRegistry().FindDescriptorOrAlias(family);
    if (descriptor == nullptr) { continue; }
    std::vector<std::string> fields = {"family",
                                       "type",
                                       "unit",
                                       "namespace_path",
                                       "producer_owner",
                                       "security_family",
                                       "visibility",
                                       "readiness",
                                       "cluster_only"};
    for (const auto& label : descriptor->labels) {
      fields.push_back(std::string("label:") + label.key +
                       (label.required ? ":required" : ":optional") +
                       (label.sensitive ? ":sensitive" : ":public"));
    }
    contracts->push_back(Contract("agent.metric.schema." + descriptor->family,
                                  AgentCatalogRuntimeSchemaSurfaceKind::metric_schema,
                                  UniqueSorted(std::move(fields)),
                                  "DefaultMetricRegistry",
                                  descriptor->cluster_only));
  }
}

void AddStorageAgentRecordContracts(
    std::vector<AgentCatalogRuntimeSchemaContract>* contracts) {
  contracts->push_back(Contract(
      "storage.page_allocation.record.ledger",
      AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record,
      {"allocations", "database_uuid", "evidence", "filespace_uuid", "free_extents",
       "next_allocation_seed", "next_evidence_sequence"},
      "PageAllocationLedger"));
  contracts->push_back(Contract(
      "storage.page_allocation.record.evidence",
      AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record,
      {"action", "allocation_uuid", "capacity_evidence_accepted",
       "capacity_evidence_uuid", "database_uuid", "diagnostic_code",
       "durability_fence_satisfied", "durable_page_generation", "durable_state_changed",
       "filespace_uuid", "local_transaction_id", "new_state", "owner_object_uuid",
       "page_count", "policy_uuid", "previous_state", "published_page_generation",
       "reason", "sequence", "start_page"},
      "PageAllocationEvidenceRecord"));
  contracts->push_back(Contract(
      "storage.page_filespace_handoff.record.request_queue",
      AgentCatalogRuntimeSchemaSurfaceKind::durable_queue_record,
      {"next_sequence", "records"},
      "PageFilespaceAgentRequestQueue"));
  contracts->push_back(Contract(
      "storage.page_filespace_handoff.record.queue_record",
      AgentCatalogRuntimeSchemaSurfaceKind::durable_queue_record,
      {"allowed", "diagnostic_code", "evidence_id", "evidence_state",
       "explicit_evidence", "filespace_agent_action_required", "low_water_pages",
       "page_agent_action_required", "request", "sequence", "target_free_pages",
       "transitions", "violation"},
      "PageFilespaceAgentQueueRecord"));
  contracts->push_back(Contract(
      "storage.filespace_growth.record.ledger",
      AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record,
      {"evidence", "member_capacity_windows", "next_evidence_sequence", "operations",
       "physical_growth_evidence", "physical_growth_operations",
       "preallocated_extents", "preallocation_evidence", "preallocation_operations"},
      "FilespaceGrowthLedger"));
  contracts->push_back(Contract(
      "storage.filespace_growth.record.preallocation_entry",
      AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record,
      {"bytes_preallocated", "cache_invalidation_required", "catalog_generation",
       "database_uuid", "durable_state_changed", "file_member_uuid", "filespace_role",
       "filespace_uuid", "member_maximum_page_count", "member_page_count_after",
       "member_page_count_before", "member_preallocated_pages_after",
       "member_preallocated_pages_before", "metrics_emitted", "page_size_bytes",
       "policy_generation", "policy_uuid", "preallocated_page_count",
       "preallocation_operation_id", "requested_page_count", "request_uuid",
       "start_page_number", "state", "storage_profile_uuid", "transaction_number",
       "transaction_uuid"},
      "FilespacePreallocationEntry"));
  contracts->push_back(Contract(
      "storage.filespace_growth.record.physical_growth_entry",
      AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record,
      {"allocated_logical_pages", "bytes_grown", "cache_invalidation_required",
       "caller_mode", "catalog_generation", "database_uuid", "durable_state_changed",
       "file_member_uuid", "filespace_role", "filespace_uuid", "grown_page_count",
       "growth_operation_id", "growth_start_page_number",
       "member_maximum_page_count", "member_physical_page_count_after",
       "member_physical_page_count_before", "member_preallocated_pages_after",
       "member_preallocated_pages_before", "metrics_emitted",
       "page_allocation_authority_bypassed", "page_size_bytes", "policy_generation",
       "policy_uuid", "requested_growth_pages", "request_uuid",
       "reserve_growth_as_preallocated", "state", "storage_profile_uuid",
       "transaction_number", "transaction_uuid"},
      "FilespacePhysicalGrowthEntry"));
  contracts->push_back(Contract(
      "storage.filespace_capacity_agent.record.metric_snapshot",
      AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record,
      {"available_capacity_window_pages", "database_uuid", "expand_capacity_proof_fresh",
       "expand_capacity_proof_present", "expand_device_proof_fresh",
       "expand_device_proof_present", "filespace_uuid", "free_pages", "health_state",
       "metrics_fresh", "metrics_present", "metrics_trusted", "policy_uuid",
       "reserved_pages", "role_state", "scope_compatible", "total_pages", "used_pages"},
      "FilespaceCapacityManagerMetricSnapshot"));
  contracts->push_back(Contract(
      "storage.page_allocation_agent.record.metric_snapshot",
      AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record,
      {"allocated_pages", "allocation_failure_signal", "allocation_failures_total",
       "database_uuid", "filespace_uuid", "free_pages", "metrics_fresh",
       "metrics_present", "metrics_trusted", "page_family", "policy_uuid",
       "preallocated_pages", "preallocation_deficit_pages", "preallocation_target_pages",
       "released_pages", "reserved_pages", "scope_compatible",
       "target_free_deficit_pages"},
      "PageAllocationManagerMetricSnapshot"));
}

std::vector<AgentCatalogRuntimeSchemaContract> BuildContracts() {
  std::vector<AgentCatalogRuntimeSchemaContract> contracts;
  AddSysViewContracts(&contracts);
  AddAgentRuntimeRecordContracts(&contracts);
  AddAgentPolicyContracts(&contracts);
  AddAgentMetricContracts(&contracts);
  AddStorageAgentRecordContracts(&contracts);
  std::sort(contracts.begin(), contracts.end(), [](const auto& left, const auto& right) {
    return left.surface_id < right.surface_id;
  });
  return contracts;
}

bool ContainsField(const std::vector<std::string>& fields, const std::string& field) {
  return std::find(fields.begin(), fields.end(), field) != fields.end();
}

bool MutationAllowed(AgentCatalogRuntimeSchemaOpenMode mode) {
  return mode == AgentCatalogRuntimeSchemaOpenMode::read_write;
}

std::string MutationDeniedDiagnostic(AgentCatalogRuntimeSchemaOpenMode mode) {
  if (mode == AgentCatalogRuntimeSchemaOpenMode::restricted) {
    return kAgentCatalogRuntimeSchemaRestrictedRepairDenied;
  }
  return kAgentCatalogRuntimeSchemaReadOnlyRepairDenied;
}

AgentCatalogRuntimeSchemaRowState MutationDeniedState(
    AgentCatalogRuntimeSchemaOpenMode mode) {
  if (mode == AgentCatalogRuntimeSchemaOpenMode::restricted) {
    return AgentCatalogRuntimeSchemaRowState::restricted_repair_denied;
  }
  return AgentCatalogRuntimeSchemaRowState::read_only_repair_denied;
}

void Fail(AgentCatalogRuntimeSchemaValidationResult* result,
          const std::string& code,
          const std::string& detail) {
  if (result->ok) {
    result->diagnostic_code = code;
    result->diagnostic_detail = detail;
  }
  result->ok = false;
}

AgentCatalogRuntimeSchemaValidationRow BaseRow(
    const AgentCatalogRuntimeSchemaContract& contract) {
  AgentCatalogRuntimeSchemaValidationRow row;
  row.surface_id = contract.surface_id;
  row.surface_kind = contract.surface_kind;
  row.expected_version = contract.current_version;
  row.diagnostic_code = kAgentCatalogRuntimeSchemaOk;
  return row;
}

}  // namespace

const char* AgentCatalogRuntimeSchemaSurfaceKindName(
    AgentCatalogRuntimeSchemaSurfaceKind kind) {
  switch (kind) {
    case AgentCatalogRuntimeSchemaSurfaceKind::catalog_sys_view_definition:
      return "catalog_sys_view_definition";
    case AgentCatalogRuntimeSchemaSurfaceKind::durable_queue_record:
      return "durable_queue_record";
    case AgentCatalogRuntimeSchemaSurfaceKind::runtime_record:
      return "runtime_record";
    case AgentCatalogRuntimeSchemaSurfaceKind::policy_schema:
      return "policy_schema";
    case AgentCatalogRuntimeSchemaSurfaceKind::metric_schema:
      return "metric_schema";
    case AgentCatalogRuntimeSchemaSurfaceKind::storage_agent_runtime_record:
      return "storage_agent_runtime_record";
  }
  return "runtime_record";
}

const char* AgentCatalogRuntimeSchemaOpenModeName(
    AgentCatalogRuntimeSchemaOpenMode mode) {
  switch (mode) {
    case AgentCatalogRuntimeSchemaOpenMode::read_write: return "read_write";
    case AgentCatalogRuntimeSchemaOpenMode::read_only: return "read_only";
    case AgentCatalogRuntimeSchemaOpenMode::restricted: return "restricted";
  }
  return "read_write";
}

const char* AgentCatalogRuntimeSchemaRowStateName(
    AgentCatalogRuntimeSchemaRowState state) {
  switch (state) {
    case AgentCatalogRuntimeSchemaRowState::current: return "current";
    case AgentCatalogRuntimeSchemaRowState::baseline_created: return "baseline_created";
    case AgentCatalogRuntimeSchemaRowState::missing_required_surface:
      return "missing_required_surface";
    case AgentCatalogRuntimeSchemaRowState::missing_required_field:
      return "missing_required_field";
    case AgentCatalogRuntimeSchemaRowState::migration_required: return "migration_required";
    case AgentCatalogRuntimeSchemaRowState::migrated: return "migrated";
    case AgentCatalogRuntimeSchemaRowState::incompatible_version:
      return "incompatible_version";
    case AgentCatalogRuntimeSchemaRowState::read_only_repair_denied:
      return "read_only_repair_denied";
    case AgentCatalogRuntimeSchemaRowState::restricted_repair_denied:
      return "restricted_repair_denied";
    case AgentCatalogRuntimeSchemaRowState::unknown_surface: return "unknown_surface";
  }
  return "current";
}

const std::vector<AgentCatalogRuntimeSchemaContract>&
BuiltinAgentCatalogRuntimeSchemaContracts() {
  static const std::vector<AgentCatalogRuntimeSchemaContract> contracts =
      BuildContracts();
  return contracts;
}

const AgentCatalogRuntimeSchemaContract* FindAgentCatalogRuntimeSchemaContract(
    std::string_view surface_id) {
  const auto& contracts = BuiltinAgentCatalogRuntimeSchemaContracts();
  const auto found = std::find_if(contracts.begin(), contracts.end(), [&](const auto& c) {
    return c.surface_id == surface_id;
  });
  return found == contracts.end() ? nullptr : &*found;
}

std::vector<AgentCatalogRuntimeSchemaObservation>
CurrentAgentCatalogRuntimeSchemaObservations() {
  std::vector<AgentCatalogRuntimeSchemaObservation> observations;
  for (const auto& contract : BuiltinAgentCatalogRuntimeSchemaContracts()) {
    AgentCatalogRuntimeSchemaObservation observation;
    observation.surface_id = contract.surface_id;
    observation.observed_version = contract.current_version;
    observation.present_fields = contract.required_fields;
    observation.present = true;
    observations.push_back(std::move(observation));
  }
  return observations;
}

AgentCatalogRuntimeSchemaValidationResult ValidateAgentCatalogRuntimeSchema(
    const AgentCatalogRuntimeSchemaValidationRequest& request) {
  AgentCatalogRuntimeSchemaValidationResult result;
  const auto& contracts = BuiltinAgentCatalogRuntimeSchemaContracts();
  std::map<std::string, AgentCatalogRuntimeSchemaObservation> observed;
  for (const auto& observation : request.observed_surfaces) {
    if (FindAgentCatalogRuntimeSchemaContract(observation.surface_id) == nullptr) {
      AgentCatalogRuntimeSchemaValidationRow row;
      row.surface_id = observation.surface_id;
      row.surface_kind = AgentCatalogRuntimeSchemaSurfaceKind::runtime_record;
      row.state = AgentCatalogRuntimeSchemaRowState::unknown_surface;
      row.observed_version = observation.observed_version;
      row.diagnostic_code = kAgentCatalogRuntimeSchemaUnknownSurface;
      row.diagnostic_detail = "schema observation is not part of the engine contract";
      result.rows.push_back(row);
      Fail(&result, row.diagnostic_code, row.diagnostic_detail);
      continue;
    }
    observed[observation.surface_id] = observation;
  }

  if (request.fresh_install) {
    for (const auto& contract : contracts) {
      auto row = BaseRow(contract);
      row.state = AgentCatalogRuntimeSchemaRowState::baseline_created;
      row.observed_version = 0;
      row.mutation_required = true;
      row.diagnostic_code = kAgentCatalogRuntimeSchemaBaselineCreated;
      row.diagnostic_detail = KindPrefix(contract.surface_kind) + " baseline version created";
      if (!MutationAllowed(request.open_mode)) {
        row.state = MutationDeniedState(request.open_mode);
        row.diagnostic_code = MutationDeniedDiagnostic(request.open_mode);
        row.diagnostic_detail = "fresh baseline creation requires writable engine catalog mode";
        Fail(&result, row.diagnostic_code, row.diagnostic_detail);
      }
      result.rows.push_back(std::move(row));
    }
    if (result.ok) {
      result.diagnostic_code = kAgentCatalogRuntimeSchemaBaselineCreated;
      result.diagnostic_detail = "fresh agent catalog runtime schema baseline accepted";
    }
    return result;
  }

  for (const auto& contract : contracts) {
    auto row = BaseRow(contract);
    const auto found = observed.find(contract.surface_id);
    if (found == observed.end() || !found->second.present) {
      row.state = AgentCatalogRuntimeSchemaRowState::missing_required_surface;
      row.observed_version = 0;
      row.diagnostic_code = kAgentCatalogRuntimeSchemaMissingRequiredSurface;
      row.diagnostic_detail = "required agent catalog runtime schema surface is missing";
      Fail(&result, row.diagnostic_code, row.diagnostic_detail);
      result.rows.push_back(std::move(row));
      continue;
    }

    const auto& observation = found->second;
    row.observed_version = observation.observed_version;
    for (const auto& field : contract.required_fields) {
      if (!ContainsField(observation.present_fields, field)) {
        row.missing_fields.push_back(field);
      }
    }
    if (!row.missing_fields.empty()) {
      row.state = AgentCatalogRuntimeSchemaRowState::missing_required_field;
      row.diagnostic_code = kAgentCatalogRuntimeSchemaMissingRequiredField;
      row.diagnostic_detail = "required agent catalog runtime schema field is missing";
      Fail(&result, row.diagnostic_code, row.diagnostic_detail);
      result.rows.push_back(std::move(row));
      continue;
    }

    if (observation.observed_version > contract.current_version) {
      row.state = AgentCatalogRuntimeSchemaRowState::incompatible_version;
      row.diagnostic_code = kAgentCatalogRuntimeSchemaIncompatibleVersion;
      row.diagnostic_detail = "future agent catalog runtime schema version is not supported";
      Fail(&result, row.diagnostic_code, row.diagnostic_detail);
      result.rows.push_back(std::move(row));
      continue;
    }

    if (observation.observed_version < contract.min_readable_version) {
      row.mutation_required = true;
      if (!request.migration_requested || !contract.migratable_from_old) {
        row.state = AgentCatalogRuntimeSchemaRowState::migration_required;
        row.diagnostic_code = kAgentCatalogRuntimeSchemaMigrationRequired;
        row.diagnostic_detail =
            "old agent catalog runtime schema version requires engine migration";
        Fail(&result, row.diagnostic_code, row.diagnostic_detail);
        result.rows.push_back(std::move(row));
        continue;
      }
      if (!MutationAllowed(request.open_mode)) {
        row.state = MutationDeniedState(request.open_mode);
        row.diagnostic_code = MutationDeniedDiagnostic(request.open_mode);
        row.diagnostic_detail =
            "agent catalog runtime schema migration requires writable engine catalog mode";
        Fail(&result, row.diagnostic_code, row.diagnostic_detail);
        result.rows.push_back(std::move(row));
        continue;
      }
      row.state = AgentCatalogRuntimeSchemaRowState::migrated;
      row.diagnostic_code = kAgentCatalogRuntimeSchemaMigrated;
      row.diagnostic_detail = "old agent catalog runtime schema version migrated";
      result.rows.push_back(std::move(row));
      continue;
    }

    row.state = AgentCatalogRuntimeSchemaRowState::current;
    row.diagnostic_code = kAgentCatalogRuntimeSchemaOk;
    row.diagnostic_detail = "agent catalog runtime schema surface is current";
    result.rows.push_back(std::move(row));
  }

  return result;
}

}  // namespace scratchbird::engine::internal_api
