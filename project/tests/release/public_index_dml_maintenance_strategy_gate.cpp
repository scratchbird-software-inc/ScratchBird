// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_index_write_path.hpp"
#include "index_family_registry.hpp"
#include "index_maintenance.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace index = scratchbird::core::index;

struct MatrixRow {
  std::string family_id;
  std::string strategy_kind;
  std::string strategy_id;
  bool admitted = false;
  bool fail_closed = true;
  bool dml_route_supported = false;
  bool maintenance_route_supported = false;
  bool exact_recheck_required = false;
  bool exact_recheck_strategy_bound = false;
  bool exact_recheck_gate_passed = false;
  bool mga_recheck_required = false;
  bool security_recheck_required = false;
  bool synchronous = false;
  bool deferred_delta = false;
  bool buffered = false;
  bool provider_specific = false;
  bool rebuild_required = false;
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

const char* BoolText(bool value) {
  return value ? "true" : "false";
}

bool AcceptedFamily(const index::IndexFamilyDescriptor& descriptor) {
  return descriptor.completion ==
             index::IndexCompletionStatus::accepted_requires_full_implementation &&
         descriptor.persistence != index::IndexPersistenceClass::reference_emulated &&
         descriptor.persistence != index::IndexPersistenceClass::policy_blocked;
}

bool BtreeAuthorityFamily(index::IndexFamily family) {
  return family == index::IndexFamily::btree ||
         family == index::IndexFamily::unique_btree;
}

bool AllowedStrategy(index::IndexDmlMaintenanceStrategyKind strategy) {
  switch (strategy) {
    case index::IndexDmlMaintenanceStrategyKind::synchronous_physical_rewrite:
    case index::IndexDmlMaintenanceStrategyKind::deferred_secondary_delta_ledger:
    case index::IndexDmlMaintenanceStrategyKind::page_aware_change_buffer:
    case index::IndexDmlMaintenanceStrategyKind::provider_specific:
    case index::IndexDmlMaintenanceStrategyKind::rebuild_from_authoritative_base:
      return true;
    case index::IndexDmlMaintenanceStrategyKind::refused:
      return false;
  }
  return false;
}

std::string Csv(std::string_view value) {
  if (value.find_first_of(",\"\n") == std::string_view::npos) {
    return std::string(value);
  }
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      out += "\"\"";
    } else {
      out += ch;
    }
  }
  out += '"';
  return out;
}

void WriteMatrix(const std::filesystem::path& path,
                 const std::vector<MatrixRow>& rows) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(out.good(), "PCR-053 could not open strategy matrix");
  out << "family_id,strategy_kind,strategy_id,admitted,fail_closed,"
         "dml_route_supported,maintenance_route_supported,"
         "exact_recheck_required,exact_recheck_strategy_bound,"
         "exact_recheck_gate_passed,"
         "mga_recheck_required,security_recheck_required,synchronous,"
         "deferred_delta,buffered,provider_specific,rebuild_required\n";
  for (const auto& row : rows) {
    out << Csv(row.family_id) << ','
        << Csv(row.strategy_kind) << ','
        << Csv(row.strategy_id) << ','
        << BoolText(row.admitted) << ','
        << BoolText(row.fail_closed) << ','
        << BoolText(row.dml_route_supported) << ','
        << BoolText(row.maintenance_route_supported) << ','
        << BoolText(row.exact_recheck_required) << ','
        << BoolText(row.exact_recheck_strategy_bound) << ','
        << BoolText(row.exact_recheck_gate_passed) << ','
        << BoolText(row.mga_recheck_required) << ','
        << BoolText(row.security_recheck_required) << ','
        << BoolText(row.synchronous) << ','
        << BoolText(row.deferred_delta) << ','
        << BoolText(row.buffered) << ','
        << BoolText(row.provider_specific) << ','
        << BoolText(row.rebuild_required) << '\n';
  }
  out.close();
  Require(out.good(), "PCR-053 could not write strategy matrix");
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view key,
                 std::string_view value = {}) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == key &&
        (value.empty() || item.evidence_id == value)) {
      return true;
    }
  }
  return false;
}

void RequireDmlDecisionEvidence() {
  api::DmlUpdateIndexMaintenanceRequest ordered;
  ordered.family = "expression";
  ordered.indexed_keys_changed = true;
  const auto ordered_decision = api::DecideDmlUpdateIndexMaintenance(ordered);
  Require(ordered_decision.ok,
          "PCR-053 ordered expression DML strategy should admit synchronous fallback");
  Require(HasEvidence(ordered_decision.evidence,
                      "dml_index_maintenance_strategy_admitted", "true"),
          "PCR-053 DML decision missing admitted strategy evidence");
  Require(HasEvidence(ordered_decision.evidence,
                      "dml_index_maintenance_strategy_exact_recheck_gate",
                      "true"),
          "PCR-053 DML decision missing exact recheck gate evidence");

  api::DmlUpdateIndexMaintenanceRequest search;
  search.family = "full_text";
  search.indexed_keys_changed = true;
  search.option_envelopes = {
      index::kDeferredSecondaryIndexRuntimeOption,
      index::kSecondaryIndexDeltaLedgerFeatureOption,
      index::kDeltaLedgerReaderOverlayOption,
      index::kDeltaLedgerCleanupHorizonBoundOption,
      index::kDeltaLedgerRecoveryClassifiableOption};
  search.cleanup_horizon_present = true;
  search.durable_mga_inventory_proof = true;
  search.delta_overlay_read_proof = true;
  search.recovery_classification_proof = true;
  const auto search_decision = api::DecideDmlUpdateIndexMaintenance(search);
  Require(search_decision.ok,
          "PCR-053 full_text DML route should admit deferred ledger mutation");
  Require(HasEvidence(search_decision.evidence,
                      "dml_index_maintenance_strategy",
                      "deferred_secondary_delta_ledger"),
          "PCR-053 full_text decision missing deferred strategy evidence");
  Require(HasEvidence(search_decision.evidence,
                      "dml_index_maintenance_strategy_dml_route_supported",
                      "true"),
          "PCR-053 full_text decision must disclose admitted local DML route");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: public_index_dml_maintenance_strategy_gate <matrix-csv>\n";
    return EXIT_FAILURE;
  }

  std::vector<MatrixRow> rows;
  std::set<std::string> seen_families;
  std::size_t accepted_non_btree = 0;
  std::size_t refused_non_runtime = 0;

  for (const auto& descriptor : index::BuiltinIndexFamilyDescriptors()) {
    Require(seen_families.insert(descriptor.id).second,
            "PCR-053 duplicate family in strategy matrix");
    const auto strategy =
        index::ClassifyIndexDmlMaintenanceStrategy(descriptor.family);
    MatrixRow row;
    row.family_id = descriptor.id;
    row.strategy_kind =
        index::IndexDmlMaintenanceStrategyKindName(strategy.strategy);
    row.strategy_id = strategy.strategy_id;
    row.admitted = strategy.admitted;
    row.fail_closed = strategy.fail_closed;
    row.dml_route_supported = strategy.dml_route_supported;
    row.maintenance_route_supported = strategy.maintenance_route_supported;
    row.exact_recheck_required = strategy.exact_recheck_required;
    row.exact_recheck_strategy_bound = strategy.exact_recheck_strategy_bound;
    row.exact_recheck_gate_passed = strategy.exact_recheck_gate_passed;
    row.mga_recheck_required = strategy.mga_recheck_required;
    row.security_recheck_required = strategy.security_recheck_required;
    row.synchronous = strategy.synchronous;
    row.deferred_delta = strategy.deferred_delta;
    row.buffered = strategy.buffered;
    row.provider_specific = strategy.provider_specific;
    row.rebuild_required = strategy.rebuild_required;

    if (AcceptedFamily(descriptor)) {
      Require(strategy.ok(),
              "PCR-053 accepted family missing admitted DML maintenance strategy");
      Require(AllowedStrategy(strategy.strategy),
              "PCR-053 accepted family selected refused strategy");
      Require(strategy.maintenance_route_supported,
              "PCR-053 accepted family missing maintenance route support");
      Require(strategy.dml_route_supported,
              "PCR-053 accepted family missing DML route support");
      Require(strategy.mga_recheck_required && strategy.security_recheck_required,
              "PCR-053 strategy missing MGA/security recheck obligation");
      if (!BtreeAuthorityFamily(descriptor.family)) {
        ++accepted_non_btree;
        Require(strategy.exact_recheck_required,
                "PCR-053 non-B-tree strategy missing exact recheck requirement");
        Require(strategy.exact_recheck_gate_passed,
                "PCR-053 non-B-tree strategy missing exact recheck gate");
        Require(strategy.exact_recheck_strategy_bound,
                "PCR-053 non-B-tree strategy missing exact recheck binding");
        Require(strategy.synchronous || strategy.deferred_delta ||
                    strategy.buffered || strategy.provider_specific ||
                    strategy.rebuild_required,
                "PCR-053 non-B-tree strategy has no concrete maintenance mode");
      }
    } else {
      ++refused_non_runtime;
      Require(!strategy.ok() && strategy.fail_closed,
              "PCR-053 non-runtime family strategy did not fail closed");
    }
    rows.push_back(std::move(row));
  }

  Require(accepted_non_btree != 0,
          "PCR-053 no accepted non-B-tree families were strategy-tested");
  Require(refused_non_runtime != 0,
          "PCR-053 no refused non-runtime families were strategy-tested");
  RequireDmlDecisionEvidence();
  WriteMatrix(argv[1], rows);

  std::cout << "public_index_dml_maintenance_strategy_gate=passed rows="
            << rows.size() << " accepted_non_btree=" << accepted_non_btree
            << " refused_non_runtime=" << refused_non_runtime << '\n';
  return EXIT_SUCCESS;
}
