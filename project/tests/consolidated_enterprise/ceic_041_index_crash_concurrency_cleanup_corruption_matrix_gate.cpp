// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-041 focused validation for index crash/concurrency/cleanup/corruption matrix.
#include "index_fault_injection_matrix.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace index = scratchbird::core::index;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const char* message) {
  if (!condition) {
    Fail(message);
  }
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const index::IndexFaultInjectionMatrixRow& row,
                 std::string_view token) {
  for (const auto& evidence : row.evidence) {
    if (evidence.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool IsBlocked(const index::IndexFamilyDescriptor& descriptor) {
  return descriptor.persistence == index::IndexPersistenceClass::donor_emulated ||
         descriptor.persistence == index::IndexPersistenceClass::policy_blocked;
}

bool RequiresPersistentScenarioCoverage(
    const index::IndexFamilyDescriptor& descriptor) {
  return descriptor.persistence == index::IndexPersistenceClass::persistent ||
         descriptor.persistence ==
             index::IndexPersistenceClass::memory_primary_persisted_cold_start;
}

void ValidateAuthorityBoundary(
    const index::IndexFaultInjectionMatrixRow& row) {
  Require(!row.parser_authority, "CEIC-041 row claimed parser authority");
  Require(!row.donor_authority, "CEIC-041 row claimed donor authority");
  Require(!row.provider_authority, "CEIC-041 row claimed provider authority");
  Require(!row.storage_authority, "CEIC-041 row claimed storage authority");
  Require(!row.visibility_authority, "CEIC-041 row claimed visibility authority");
  Require(!row.security_authority, "CEIC-041 row claimed security authority");
  Require(!row.transaction_finality_authority,
          "CEIC-041 row claimed transaction finality authority");
  Require(!row.recovery_authority, "CEIC-041 row claimed recovery authority");
  Require(EvidenceHas(row, "ceic_042_readiness_drift_claimed=false") ||
              row.scenario_class == "family_capability_blocker" ||
              row.scenario_class == "unsafe_half_published_refusal" ||
              row.scenario_class == "crash_cleanup_overlay" ||
              row.scenario_class == "reopen" ||
              row.scenario_class == "corruption_classification" ||
              row.scenario_class == "donor_policy_refusal" ||
              row.scenario_class == "cluster_external_provider_only",
          "CEIC-041 scenario rows must explicitly avoid CEIC-042 drift claims");
}

}  // namespace

int main() {
  const auto matrix = index::BuildIndexFaultInjectionCrashMatrix();
  Require(matrix.ok(), "CEIC-041 matrix did not build cleanly");
  Require(!matrix.rows.empty(), "CEIC-041 matrix rows missing");

  std::map<std::string, std::set<std::string>> scenarios_by_family;
  std::map<std::string, std::vector<const index::IndexFaultInjectionMatrixRow*>>
      rows_by_family;
  bool saw_unsafe_half_publish_refusal = false;
  bool saw_concrete_execution = false;
  bool saw_repair_rebuild_recommendation = false;
  bool saw_backup_restore_identity = false;
  bool saw_cleanup_horizon_binding = false;
  bool saw_corruption_classification = false;
  bool saw_concurrent_serialization = false;
  bool saw_deterministic_diagnostics = false;

  for (const auto& row : matrix.rows) {
    ValidateAuthorityBoundary(row);
    Require(row.runtime_dependency_free,
            "CEIC-041 matrix row must be runtime dependency free");
    Require(row.concrete_execution_result,
            "CEIC-041 matrix row must carry an executed deterministic result");
    Require(!row.diagnostic_code.empty(),
            "CEIC-041 matrix row missing diagnostic code");
    Require(!row.message_key.empty(), "CEIC-041 matrix row missing message key");
    Require(!row.scenario_class.empty(),
            "CEIC-041 matrix row missing scenario class");

    scenarios_by_family[row.family_id].insert(row.scenario_class);
    rows_by_family[row.family_id].push_back(&row);
    saw_unsafe_half_publish_refusal =
        saw_unsafe_half_publish_refusal ||
        row.unsafe_half_published_state_refused;
    saw_concrete_execution = saw_concrete_execution || row.concrete_execution_result;
    saw_repair_rebuild_recommendation =
        saw_repair_rebuild_recommendation || row.repair_rebuild_recommendation;
    saw_backup_restore_identity =
        saw_backup_restore_identity || row.backup_restore_identity_validated;
    saw_cleanup_horizon_binding =
        saw_cleanup_horizon_binding || row.cleanup_horizon_bound;
    saw_corruption_classification =
        saw_corruption_classification || row.corruption_classified;
    saw_concurrent_serialization =
        saw_concurrent_serialization || row.concurrent_mutation_serialized;
    saw_deterministic_diagnostics =
        saw_deterministic_diagnostics || row.deterministic_diagnostics;
  }

  Require(saw_concrete_execution, "CEIC-041 concrete execution proof missing");
  Require(saw_repair_rebuild_recommendation,
          "CEIC-041 repair/rebuild recommendation proof missing");
  Require(saw_backup_restore_identity,
          "CEIC-041 backup/restore identity proof missing");
  Require(saw_cleanup_horizon_binding,
          "CEIC-041 cleanup horizon binding proof missing");
  Require(saw_corruption_classification,
          "CEIC-041 corruption classification proof missing");
  Require(saw_concurrent_serialization,
          "CEIC-041 concurrent mutation serialization proof missing");
  Require(saw_deterministic_diagnostics,
          "CEIC-041 deterministic diagnostics proof missing");
  Require(saw_unsafe_half_publish_refusal,
          "CEIC-041 unsafe half-published refusal proof missing");

  const std::set<std::string> required_persistent_scenarios = {
      "crash_before_generation_publish",
      "crash_after_generation_publish",
      "reopen",
      "repair",
      "rebuild",
      "backup_restore_identity",
      "cleanup_horizon",
      "corruption_classification",
      "concurrent_mutation_serialization",
  };

  for (const auto& descriptor : index::BuiltinIndexFamilyDescriptors()) {
    const std::string family = descriptor.id;
    Require(rows_by_family.count(family) > 0,
            family + " has no CEIC-041 matrix coverage");

    if (IsBlocked(descriptor)) {
      bool refused = false;
      for (const auto* row : rows_by_family[family]) {
        refused = refused ||
                  (row->scenario_class == "donor_policy_refusal" &&
                   row->donor_policy_refused && row->refused &&
                   row->fail_closed && !row->planner_visible);
      }
      Require(refused, family + " donor/policy refusal coverage missing");
      continue;
    }

    if (descriptor.persistence == index::IndexPersistenceClass::memory_only) {
      Require(scenarios_by_family[family].count("temporary_memory_cleanup") > 0,
              family + " temporary cleanup coverage missing");
      continue;
    }

    if (RequiresPersistentScenarioCoverage(descriptor)) {
      for (const auto& scenario : required_persistent_scenarios) {
        Require(scenarios_by_family[family].count(scenario) > 0,
                family + " missing scenario " + scenario);
      }
      bool has_cluster_refusal = false;
      for (const auto* row : rows_by_family[family]) {
        has_cluster_refusal =
            has_cluster_refusal ||
            (row->scenario_class == "cluster_external_provider_only" &&
             row->cluster_external_provider_only && row->refused &&
             row->fail_closed && !row->planner_visible);
      }
      if (descriptor.persistence == index::IndexPersistenceClass::persistent) {
        Require(has_cluster_refusal,
                family + " cluster external-provider-only refusal missing");
      }
    }
  }

  for (const auto& [family, rows] : rows_by_family) {
    for (const auto* row : rows) {
      Require(!EvidenceHas(*row, "all_index_readiness_claimed=true"),
              family + " claimed all-index readiness");
      Require(!EvidenceHas(*row, "enterprise_readiness_claimed=true"),
              family + " claimed enterprise readiness");
      Require(!EvidenceHas(*row, "ceic_042_readiness_drift_claimed=true"),
              family + " claimed CEIC-042 readiness drift");
    }
  }

  std::cout
      << "ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate=pass\n";
  return EXIT_SUCCESS;
}
