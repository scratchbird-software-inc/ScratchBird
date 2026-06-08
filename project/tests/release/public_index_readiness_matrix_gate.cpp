// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_access_method.hpp"
#include "index_family_registry.hpp"
#include "index_route_capability.hpp"
#include "uuid.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace index = scratchbird::core::index;
namespace uuid = scratchbird::core::uuid;

constexpr std::array<index::IndexRouteKind, 11> kRoutes = {
    index::IndexRouteKind::dml_insert,
    index::IndexRouteKind::dml_update,
    index::IndexRouteKind::dml_delete,
    index::IndexRouteKind::sql_select,
    index::IndexRouteKind::bulk_build,
    index::IndexRouteKind::nosql_document,
    index::IndexRouteKind::nosql_graph,
    index::IndexRouteKind::nosql_vector,
    index::IndexRouteKind::nosql_search,
    index::IndexRouteKind::maintenance,
    index::IndexRouteKind::validate_repair};

struct MatrixRow {
  std::string family_id;
  std::string family_uuid;
  std::string persistence;
  std::string key_model;
  std::string completion;
  bool declared_static_capability = false;
  bool planner_contract_capability = false;
  bool static_contract = false;
  bool provider_present = false;
  bool evidence_required = false;
  bool provider_admitted_from_state = false;
  bool durable_closure_admitted_from_state = false;
  bool physical_implemented = false;
  bool physical_reader = false;
  bool physical_writer = false;
  bool dml_route_complete = false;
  bool bulk_build_route_complete = false;
  bool select_route_complete = false;
  bool nosql_route_complete = false;
  bool maintenance_route_complete = false;
  bool validate_repair_route_complete = false;
  bool recovery_reopen = false;
  bool validate_supported = false;
  bool repair_supported = false;
  bool exact_recheck_required = false;
  bool mga_recheck_required = false;
  bool security_recheck_required = false;
  std::string provider_evidence_status;
  bool provider_admitted = false;
  std::string provider_admission_status;
  std::string benchmark_evidence_status;
  bool benchmark_clean = false;
  bool enterprise_ready_claim = false;
  bool durable_closure_claimed = false;
  std::string readiness_claim;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

const index::IndexRouteCapabilityState& RouteState(index::IndexFamily family,
                                                   index::IndexRouteKind route) {
  const auto* state = index::FindBuiltinIndexRouteCapabilityState(route, family);
  Require(state != nullptr, "PCR-050 missing route capability state");
  return *state;
}

bool RouteComplete(index::IndexFamily family, index::IndexRouteKind route) {
  return RouteState(family, route).route_complete();
}

bool DmlRoutesComplete(index::IndexFamily family) {
  return RouteComplete(family, index::IndexRouteKind::dml_insert) &&
         RouteComplete(family, index::IndexRouteKind::dml_update) &&
         RouteComplete(family, index::IndexRouteKind::dml_delete);
}

bool AnyNosqlRouteComplete(index::IndexFamily family) {
  return RouteComplete(family, index::IndexRouteKind::nosql_document) ||
         RouteComplete(family, index::IndexRouteKind::nosql_graph) ||
         RouteComplete(family, index::IndexRouteKind::nosql_vector) ||
         RouteComplete(family, index::IndexRouteKind::nosql_search);
}

bool AnyCompleteRouteRequiresExactRecheck(index::IndexFamily family) {
  bool required = false;
  for (const auto route : kRoutes) {
    const auto& state = RouteState(family, route);
    required = required || (state.route_complete() && state.requires_exact_recheck);
  }
  return required;
}

bool AnyCompleteRouteRequiresMgaRecheck(index::IndexFamily family) {
  bool required = false;
  for (const auto route : kRoutes) {
    const auto& state = RouteState(family, route);
    required = required || (state.route_complete() && state.requires_mga_recheck);
  }
  return required;
}

bool AnyCompleteRouteRequiresSecurityRecheck(index::IndexFamily family) {
  bool required = false;
  for (const auto route : kRoutes) {
    const auto& state = RouteState(family, route);
    required =
        required || (state.route_complete() && state.requires_security_recheck);
  }
  return required;
}

index::IndexProviderAdmissionResult StaticOnlyAdmission(
    index::IndexFamily family,
    const index::IndexFamilyPhysicalCapabilityState& state) {
  index::IndexProviderAccessMethodContract contract;
  contract.family = family;
  contract.route = index::IndexRouteKind::sql_select;
  contract.route_boundary.static_registry_complete_capability_seen =
      state.physically_complete();
  contract.route_boundary.route_capability_present =
      index::FindBuiltinIndexRouteCapabilityState(contract.route, family) !=
      nullptr;
  contract.route_boundary.route_specific_boundary_declared = true;
  return index::AdmitIndexProviderAccessMethod(contract);
}

std::string Csv(std::string_view value) {
  bool quote = value.find_first_of(",\"\n") != std::string_view::npos;
  if (!quote) {
    return std::string(value);
  }
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"') {
      out += "\"\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

const char* BoolText(bool value) {
  return value ? "true" : "false";
}

void WriteMatrix(const std::filesystem::path& path,
                 const std::vector<MatrixRow>& rows) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(out.good(), "PCR-050 could not open index readiness matrix output");
  out << "family_id,family_uuid,persistence,key_model,completion,"
         "declared_static_capability,planner_contract_capability,"
         "static_contract,provider_present,evidence_required,"
         "provider_admitted_from_state,durable_closure_admitted_from_state,"
         "physical_implemented,physical_reader,physical_writer,"
         "dml_route_complete,bulk_build_route_complete,select_route_complete,"
         "nosql_route_complete,maintenance_route_complete,"
         "validate_repair_route_complete,recovery_reopen,validate_supported,"
         "repair_supported,exact_recheck_required,mga_recheck_required,"
         "security_recheck_required,provider_evidence_status,"
         "provider_admitted,provider_admission_status,"
         "benchmark_evidence_status,benchmark_clean,enterprise_ready_claim,"
         "durable_closure_claimed,readiness_claim\n";
  for (const auto& row : rows) {
    out << Csv(row.family_id) << ','
        << Csv(row.family_uuid) << ','
        << Csv(row.persistence) << ','
        << Csv(row.key_model) << ','
        << Csv(row.completion) << ','
        << BoolText(row.declared_static_capability) << ','
        << BoolText(row.planner_contract_capability) << ','
        << BoolText(row.static_contract) << ','
        << BoolText(row.provider_present) << ','
        << BoolText(row.evidence_required) << ','
        << BoolText(row.provider_admitted_from_state) << ','
        << BoolText(row.durable_closure_admitted_from_state) << ','
        << BoolText(row.physical_implemented) << ','
        << BoolText(row.physical_reader) << ','
        << BoolText(row.physical_writer) << ','
        << BoolText(row.dml_route_complete) << ','
        << BoolText(row.bulk_build_route_complete) << ','
        << BoolText(row.select_route_complete) << ','
        << BoolText(row.nosql_route_complete) << ','
        << BoolText(row.maintenance_route_complete) << ','
        << BoolText(row.validate_repair_route_complete) << ','
        << BoolText(row.recovery_reopen) << ','
        << BoolText(row.validate_supported) << ','
        << BoolText(row.repair_supported) << ','
        << BoolText(row.exact_recheck_required) << ','
        << BoolText(row.mga_recheck_required) << ','
        << BoolText(row.security_recheck_required) << ','
        << Csv(row.provider_evidence_status) << ','
        << BoolText(row.provider_admitted) << ','
        << Csv(row.provider_admission_status) << ','
        << Csv(row.benchmark_evidence_status) << ','
        << BoolText(row.benchmark_clean) << ','
        << BoolText(row.enterprise_ready_claim) << ','
        << BoolText(row.durable_closure_claimed) << ','
        << Csv(row.readiness_claim) << '\n';
  }
  out.close();
  Require(out.good(), "PCR-050 could not write index readiness matrix");
}

MatrixRow BuildRow(const index::IndexFamilyDescriptor& descriptor) {
  const auto* state =
      index::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
  Require(state != nullptr, "PCR-050 missing family capability state");

  const auto admission = StaticOnlyAdmission(descriptor.family, *state);
  Require(!admission.enterprise_ready_claimed,
          "PCR-050 provider admission overclaimed enterprise readiness");
  Require(!admission.durable_family_closure_claimed,
          "PCR-050 provider admission overclaimed durable family closure");
  Require(!admission.ok(),
          "PCR-050 static registry capability admitted as provider evidence");

  MatrixRow row;
  row.family_id = descriptor.id;
  row.family_uuid = uuid::UuidToString(descriptor.family_uuid.value);
  row.persistence = index::IndexPersistenceClassName(descriptor.persistence);
  row.key_model = index::IndexKeyModelName(descriptor.key_model);
  row.completion = index::IndexCompletionStatusName(descriptor.completion);
  row.declared_static_capability = state->declared_capability;
  row.planner_contract_capability = state->planner_contract_capability;
  row.static_contract = state->static_contract;
  row.provider_present = state->provider_present;
  row.evidence_required = state->evidence_required;
  row.provider_admitted_from_state = state->provider_admitted;
  row.durable_closure_admitted_from_state = state->durable_closure_admitted;
  row.physical_implemented = state->implemented;
  row.physical_reader = state->physical_reader;
  row.physical_writer = state->physical_writer;
  row.dml_route_complete = DmlRoutesComplete(descriptor.family);
  row.bulk_build_route_complete =
      RouteComplete(descriptor.family, index::IndexRouteKind::bulk_build);
  row.select_route_complete =
      RouteComplete(descriptor.family, index::IndexRouteKind::sql_select);
  row.nosql_route_complete = AnyNosqlRouteComplete(descriptor.family);
  row.maintenance_route_complete =
      RouteComplete(descriptor.family, index::IndexRouteKind::maintenance);
  row.validate_repair_route_complete =
      RouteComplete(descriptor.family, index::IndexRouteKind::validate_repair);
  row.recovery_reopen = state->recovery_reopen;
  row.validate_supported = state->validate;
  row.repair_supported = state->repair;
  row.exact_recheck_required =
      AnyCompleteRouteRequiresExactRecheck(descriptor.family);
  row.mga_recheck_required =
      AnyCompleteRouteRequiresMgaRecheck(descriptor.family);
  row.security_recheck_required =
      AnyCompleteRouteRequiresSecurityRecheck(descriptor.family);
  row.provider_admitted = admission.admitted;
  row.provider_admission_status =
      index::IndexProviderAdmissionStatusName(admission.admission_status);
  switch (admission.admission_status) {
    case index::IndexProviderAdmissionStatus::admitted:
      row.provider_evidence_status = "provider_evidence_admitted";
      break;
    case index::IndexProviderAdmissionStatus::static_capability_only:
      row.provider_evidence_status =
          "static_capability_not_provider_evidence";
      break;
    case index::IndexProviderAdmissionStatus::non_persistent_family:
    case index::IndexProviderAdmissionStatus::donor_emulated_non_runtime:
    case index::IndexProviderAdmissionStatus::policy_blocked_non_runtime:
      row.provider_evidence_status = "non_runtime_not_admitted";
      break;
    default:
      row.provider_evidence_status =
          "provider_evidence_required_not_admitted";
      break;
  }
  row.benchmark_clean = state->benchmark_clean;
  row.benchmark_evidence_status =
      state->benchmark_clean ? "static_registry_benchmark_flag_only" : "none";
  row.enterprise_ready_claim = admission.enterprise_ready_claimed;
  row.durable_closure_claimed = admission.durable_family_closure_claimed;
  row.readiness_claim =
      row.provider_admitted ? "provider_admitted_without_enterprise_claim"
                            : "not_enterprise_ready";
  return row;
}

std::vector<MatrixRow> BuildMatrix() {
  std::vector<MatrixRow> rows;
  std::set<std::string> family_ids;
  std::set<std::string> family_uuids;
  for (const auto& descriptor : index::BuiltinIndexFamilyDescriptors()) {
    Require(descriptor.family != index::IndexFamily::unknown,
            "PCR-050 unknown family leaked into built-in descriptor matrix");
    Require(!descriptor.id.empty(),
            "PCR-050 index family descriptor has empty id");
    Require(descriptor.family_uuid.valid(),
            "PCR-050 index family descriptor has invalid UUID");
    Require(family_ids.insert(descriptor.id).second,
            "PCR-050 duplicate index family id");
    const auto uuid_text = uuid::UuidToString(descriptor.family_uuid.value);
    Require(family_uuids.insert(uuid_text).second,
            "PCR-050 duplicate index family UUID");

    auto row = BuildRow(descriptor);
    Require(row.static_contract == row.declared_static_capability,
            "PCR-051 static contract must be separate but aligned with the "
            "registry declaration");
    Require(row.evidence_required == row.static_contract,
            "PCR-051 provider evidence requirement must be explicit for every "
            "static contract");
    Require(!row.provider_present,
            "PCR-051 static registry rows cannot claim provider presence");
    Require(!row.provider_admitted_from_state,
            "PCR-051 static registry rows cannot claim provider admission");
    Require(!row.durable_closure_admitted_from_state,
            "PCR-051 static registry rows cannot claim durable closure");
    if (row.select_route_complete || row.dml_route_complete ||
        row.bulk_build_route_complete || row.nosql_route_complete ||
        row.maintenance_route_complete ||
        row.validate_repair_route_complete) {
      Require(row.mga_recheck_required,
              "PCR-050 complete route missing MGA recheck obligation");
      Require(row.security_recheck_required,
              "PCR-050 complete route missing security recheck obligation");
    }
    Require(!row.enterprise_ready_claim,
            "PCR-050 row overclaimed enterprise readiness");
    Require(!row.durable_closure_claimed,
            "PCR-050 row overclaimed durable closure");
    rows.push_back(std::move(row));
  }
  Require(rows.size() == index::BuiltinIndexFamilyDescriptors().size(),
          "PCR-050 matrix row count mismatch");
  return rows;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_index_readiness_matrix_gate <matrix-csv>\n";
    return EXIT_FAILURE;
  }
  const auto rows = BuildMatrix();
  WriteMatrix(argv[1], rows);
  std::cout << "public_index_readiness_matrix_gate=passed rows="
            << rows.size() << '\n';
  return EXIT_SUCCESS;
}
