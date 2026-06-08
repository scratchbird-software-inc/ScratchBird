// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_target_access_plan.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace dml = scratchbird::engine::internal_api;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool Contains(const std::string& text, const std::string& token) {
  return text.find(token) != std::string::npos;
}

dml::DmlTargetAccessPlanRequest BaseRequest() {
  dml::DmlTargetAccessPlanRequest request;
  request.relation_uuid = "rel.odf030.customer";
  request.access_descriptor_present = true;
  request.predicate_descriptor_digest = "predicate.digest.odf030.v1";
  request.observed_catalog_epoch = 10;
  request.current_catalog_epoch = 10;
  request.observed_security_epoch = 20;
  request.current_security_epoch = 20;
  request.observed_policy_epoch = 30;
  request.current_policy_epoch = 30;
  request.observed_stats_epoch = 40;
  request.current_stats_epoch = 40;
  return request;
}

bool AcceptedPlanHasRequiredRecheckEvidence(const dml::DmlTargetAccessPlan& plan) {
  return Require(plan.ok, "plan was refused") &&
         Require(Has(plan.evidence, "mga_visibility_recheck=required"),
                 "accepted plan missing MGA visibility recheck evidence") &&
         Require(Has(plan.evidence, "security_recheck=required"),
                 "accepted plan missing security recheck evidence") &&
         Require(Has(plan.evidence, "grants_security_context=present"),
                 "accepted plan missing grants/security evidence") &&
         Require(Has(plan.evidence, "parser_or_donor_authority=false"),
                 "accepted plan missing parser/donor non-authority evidence") &&
         Require(Has(plan.evidence,
                     "mga_finality_authority=engine_transaction_inventory"),
                 "accepted plan missing engine finality authority evidence");
}

bool AcceptedKindsAreClassified() {
  {
    auto request = BaseRequest();
    request.predicate_kind = "row_uuid_eq";
    request.row_uuid = "row.odf030.1";
    const auto plan = dml::BuildDmlTargetAccessPlan(request);
    if (!AcceptedPlanHasRequiredRecheckEvidence(plan) ||
        !Require(plan.access_kind == dml::DmlTargetAccessKind::row_uuid_singleton,
                 "row_uuid predicate did not select row_uuid singleton") ||
        !Require(plan.executor_capability == "row_uuid_lookup",
                 "row_uuid executor capability mismatch") ||
        !Require(plan.estimated_rows == 1, "row_uuid estimate was not singleton")) {
      return false;
    }
  }
  {
    auto request = BaseRequest();
    request.predicate_kind = "unique_eq";
    request.index_uuid = "idx.odf030.customer.email.unique";
    request.index_unique = true;
    request.estimated_rows = 7;
    const auto plan = dml::BuildDmlTargetAccessPlan(request);
    if (!AcceptedPlanHasRequiredRecheckEvidence(plan) ||
        !Require(plan.access_kind == dml::DmlTargetAccessKind::unique_index_lookup,
                 "unique predicate did not select unique index lookup") ||
        !Require(plan.executor_capability == "index_lookup",
                 "unique executor capability mismatch") ||
        !Require(plan.estimated_rows == 1, "unique estimate was not singleton")) {
      return false;
    }
  }
  {
    auto request = BaseRequest();
    request.predicate_kind = "scalar_eq";
    request.index_uuid = "idx.odf030.customer.region";
    request.index_unique = false;
    request.estimated_rows = 12;
    const auto plan = dml::BuildDmlTargetAccessPlan(request);
    if (!AcceptedPlanHasRequiredRecheckEvidence(plan) ||
        !Require(plan.access_kind ==
                     dml::DmlTargetAccessKind::nonunique_index_lookup,
                 "scalar equality did not select nonunique index lookup") ||
        !Require(plan.estimated_rows == 12,
                 "nonunique estimate was not preserved")) {
      return false;
    }
  }
  {
    auto request = BaseRequest();
    request.predicate_kind = "scalar_range";
    request.index_uuid = "idx.odf030.customer.created_at";
    request.estimated_rows = 48;
    const auto plan = dml::BuildDmlTargetAccessPlan(request);
    if (!AcceptedPlanHasRequiredRecheckEvidence(plan) ||
        !Require(plan.access_kind == dml::DmlTargetAccessKind::range_index_lookup,
                 "range predicate did not select range index lookup") ||
        !Require(plan.executor_capability == "index_range_scan",
                 "range executor capability mismatch")) {
      return false;
    }
  }
  {
    auto request = BaseRequest();
    request.predicate_kind = "scalar_range";
    request.summary_prune.requested = true;
    request.summary_prune.summary_present = true;
    request.summary_prune.predicate_supported = true;
    request.summary_prune.summary_generation = 55;
    request.summary_prune.relation_generation = 55;
    request.summary_prune.candidate_ranges = 8;
    request.summary_prune.ranges_pruned = 6;
    request.summary_prune.pages_considered = 64;
    request.summary_prune.pages_pruned = 44;
    request.estimated_rows = 16;
    const auto plan = dml::BuildDmlTargetAccessPlan(request);
    if (!AcceptedPlanHasRequiredRecheckEvidence(plan) ||
        !Require(plan.access_kind == dml::DmlTargetAccessKind::summary_pruned,
                 "safe summary descriptor did not select summary-pruned access") ||
        !Require(plan.executor_capability == "bitmap_summary_scan",
                 "summary executor capability mismatch") ||
        !Require(Has(plan.evidence, "summary_prune_authority=metadata_only"),
                 "summary pruning became row visibility authority")) {
      return false;
    }
  }
  {
    auto request = BaseRequest();
    request.predicate_kind.clear();
    request.explicit_table_scan_fallback = true;
    request.estimated_rows = 128;
    const auto plan = dml::BuildDmlTargetAccessPlan(request);
    if (!AcceptedPlanHasRequiredRecheckEvidence(plan) ||
        !Require(plan.access_kind == dml::DmlTargetAccessKind::table_scan,
                 "explicit fallback did not select table scan") ||
        !Require(plan.executor_capability == "table_scan",
                 "table scan executor capability mismatch")) {
      return false;
    }
  }
  return true;
}

bool UnsafeRoutesRefuseWithExactDiagnostics() {
  {
    auto request = BaseRequest();
    request.relation_uuid.clear();
    request.relation_present = false;
    request.access_descriptor_present = false;
    request.predicate_descriptor_digest.clear();
    request.mga_visibility_recheck_planned = false;
    request.security_recheck_planned = false;
    request.grants_proven = false;
    request.security_context_present = false;
    request.parser_or_donor_authority = true;
    request.observed_catalog_epoch = 9;
    request.current_catalog_epoch = 10;
    request.observed_security_epoch = 19;
    request.current_security_epoch = 20;
    request.observed_policy_epoch = 29;
    request.current_policy_epoch = 30;
    request.observed_stats_epoch = 39;
    request.current_stats_epoch = 40;
    const auto plan = dml::BuildDmlTargetAccessPlan(request);
    if (!Require(!plan.ok, "unsafe request was accepted") ||
        !Require(Has(plan.diagnostics, "missing relation"),
                 "missing relation diagnostic absent") ||
        !Require(Has(plan.diagnostics, "missing predicate/access descriptor"),
                 "missing predicate/access descriptor diagnostic absent") ||
        !Require(Has(plan.diagnostics, "missing MGA recheck"),
                 "missing MGA recheck diagnostic absent") ||
        !Require(Has(plan.diagnostics, "missing security recheck"),
                 "missing security recheck diagnostic absent") ||
        !Require(Has(plan.diagnostics, "missing grants/security context"),
                 "missing grants/security context diagnostic absent") ||
        !Require(Has(plan.diagnostics, "stale catalog epoch"),
                 "stale catalog epoch diagnostic absent") ||
        !Require(Has(plan.diagnostics, "stale security epoch"),
                 "stale security epoch diagnostic absent") ||
        !Require(Has(plan.diagnostics, "stale policy epoch"),
                 "stale policy epoch diagnostic absent") ||
        !Require(Has(plan.diagnostics, "stale stats epoch"),
                 "stale stats epoch diagnostic absent") ||
        !Require(Has(plan.diagnostics, "unsafe parser/donor authority"),
                 "unsafe parser/donor authority diagnostic absent")) {
      return false;
    }
  }
  {
    auto request = BaseRequest();
    request.predicate_kind = "scalar_range";
    request.summary_prune.requested = true;
    request.summary_prune.summary_present = true;
    request.summary_prune.predicate_supported = true;
    request.summary_prune.summary_generation = 5;
    request.summary_prune.relation_generation = 6;
    request.summary_prune.candidate_ranges = 2;
    request.summary_prune.ranges_pruned = 3;
    const auto plan = dml::BuildDmlTargetAccessPlan(request);
    if (!Require(!plan.ok, "unsafe summary pruning was accepted") ||
        !Require(Has(plan.diagnostics, "unsafe summary pruning"),
                 "unsafe summary pruning diagnostic absent")) {
      return false;
    }
  }
  return true;
}

bool EvidenceHasNoRuntimeDocDependency() {
  auto request = BaseRequest();
  request.predicate_kind = "row_uuid_eq";
  request.row_uuid = "row.odf030.no.docs";
  const auto serialized =
      dml::SerializeDmlTargetAccessPlanEvidence(dml::BuildDmlTargetAccessPlan(request));
  for (const auto& forbidden : {"docs/", "execution-plans", "findings", "audit",
                                "contracts", "references"}) {
    if (!Require(!Contains(serialized, forbidden),
                 std::string("runtime evidence leaked forbidden token: ") +
                     forbidden)) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  if (!AcceptedKindsAreClassified()) return 1;
  if (!UnsafeRoutesRefuseWithExactDiagnostics()) return 1;
  if (!EvidenceHasNoRuntimeDocDependency()) return 1;
  return 0;
}
