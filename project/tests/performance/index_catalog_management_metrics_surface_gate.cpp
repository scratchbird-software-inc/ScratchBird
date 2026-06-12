// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_family_management_surface.hpp"
#include "index_management.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace idx = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "index_catalog_management_metrics_surface_gate: " << message
              << '\n';
    std::exit(1);
  }
}

const idx::IndexFamilyManagementSurfaceRow& RowFor(
    const idx::IndexFamilyManagementSurface& surface,
    const std::string& family_id) {
  const auto it = std::find_if(
      surface.rows.begin(),
      surface.rows.end(),
      [&](const idx::IndexFamilyManagementSurfaceRow& row) {
        return row.family_id == family_id;
      });
  Require(it != surface.rows.end(), "missing family row " + family_id);
  return *it;
}

const idx::IndexFamilySupportBundleRow& BundleRowFor(
    const idx::IndexFamilyManagementSurface& surface,
    const std::string& family_id) {
  const auto it = std::find_if(
      surface.support_bundle_rows.begin(),
      surface.support_bundle_rows.end(),
      [&](const idx::IndexFamilySupportBundleRow& row) {
        return row.family_id == family_id;
      });
  Require(it != surface.support_bundle_rows.end(),
          "missing support bundle row " + family_id);
  return *it;
}

std::string Field(const idx::IndexFamilySupportBundleRow& row,
                  const std::string& key) {
  const auto it = std::find_if(
      row.fields.begin(),
      row.fields.end(),
      [&](const idx::IndexFamilySupportBundleField& field) {
        return field.key == key;
      });
  Require(it != row.fields.end(),
          "missing support bundle field " + row.family_id + "." + key);
  return it->value;
}

std::string Label(const idx::IndexFamilyMetricRecord& record,
                  const std::string& key) {
  const auto it = std::find_if(
      record.labels.begin(),
      record.labels.end(),
      [&](const idx::IndexFamilyMetricLabel& label) {
        return label.key == key;
      });
  Require(it != record.labels.end(),
          "missing metric label " + record.family + "." + key);
  return it->value;
}

const idx::IndexFamilyMetricRecord& MetricFor(
    const std::vector<idx::IndexFamilyMetricRecord>& records,
    const std::string& metric_family,
    const std::string& family_id) {
  const auto it = std::find_if(
      records.begin(),
      records.end(),
      [&](const idx::IndexFamilyMetricRecord& record) {
        return record.family == metric_family &&
               Label(record, "family_id") == family_id;
      });
  Require(it != records.end(),
          "missing metric " + metric_family + " for " + family_id);
  return *it;
}

bool ContainsMarker(const std::string& value) {
  const std::string lowered = [&] {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return out;
  }();
  static const std::vector<std::string> markers = {
      "todo", "stub", "execution_plan", "findings", "implementation_packet"};
  return std::any_of(markers.begin(), markers.end(),
                     [&](const std::string& marker) {
                       return lowered.find(marker) != std::string::npos;
                     });
}

void RequireSurfaceCompletenessAndTruthfulness(
    const idx::IndexFamilyManagementSurface& surface) {
  const auto& descriptors = idx::BuiltinIndexFamilyDescriptors();
  Require(surface.rows.size() == descriptors.size(),
          "surface row count must match descriptor count");
  Require(surface.support_bundle_rows.size() == descriptors.size(),
          "support bundle row count must match descriptor count");

  std::set<std::string> seen;
  for (const auto& descriptor : descriptors) {
    const auto& row = RowFor(surface, descriptor.id);
    const auto* state =
        idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    Require(state != nullptr,
            "descriptor lacks capability state " + descriptor.id);
    Require(seen.insert(row.family_id).second,
            "duplicate surface row " + row.family_id);
    Require(row.family_name == descriptor.canonical_name,
            "family name mismatch " + row.family_id);
    Require(row.persistence ==
                idx::IndexPersistenceClassName(descriptor.persistence),
            "persistence mismatch " + row.family_id);
    Require(row.key_model == idx::IndexKeyModelName(descriptor.key_model),
            "key model mismatch " + row.family_id);
    Require(row.native_physical_family == descriptor.native_physical_family,
            "native physical family mismatch " + row.family_id);
    Require(row.semantic_profile == descriptor.default_semantic_profile,
            "semantic profile mismatch " + row.family_id);
    Require(row.declared_capability == state->declared_capability,
            "declared capability mismatch " + row.family_id);
    Require(row.planner_contract == state->planner_contract_capability,
            "planner contract mismatch " + row.family_id);
    Require(row.implemented == state->implemented,
            "implemented mismatch " + row.family_id);
    Require(row.physical_reader == state->physical_reader,
            "physical reader mismatch " + row.family_id);
    Require(row.physical_writer == state->physical_writer,
            "physical writer mismatch " + row.family_id);
    Require(row.maintenance == state->maintenance,
            "maintenance mismatch " + row.family_id);
    Require(row.validate == state->validate,
            "validate mismatch " + row.family_id);
    Require(row.repair == state->repair,
            "repair mismatch " + row.family_id);
    Require(row.recovery_reopen == state->recovery_reopen,
            "recovery reopen mismatch " + row.family_id);
    Require(row.rebuild == state->rebuild,
            "rebuild mismatch " + row.family_id);
    Require(row.runtime_available == state->runtime_available,
            "runtime availability mismatch " + row.family_id);
    Require(row.benchmark_clean == state->benchmark_clean,
            "benchmark clean mismatch " + row.family_id);
    Require(row.blocker ==
                idx::IndexFamilyPhysicalCapabilityBlockerName(state->blocker),
            "blocker mismatch " + row.family_id);
    Require(!row.blocker_diagnostic_code.empty(),
            "blocker diagnostic missing " + row.family_id);
    Require(!row.blocker_message_key.empty(),
            "blocker message key missing " + row.family_id);
    Require(!row.blocker_detail.empty(),
            "blocker detail missing " + row.family_id);
    Require(row.observational_only, "row must be observational only");
    Require(!row.catalog_authority && !row.execution_authority &&
                !row.visibility_authority && !row.security_authority &&
                !row.transaction_finality_authority &&
                !row.recovery_authority && !row.parser_authority &&
                !row.reference_authority && !row.provider_authority,
            "row claimed authority " + row.family_id);

    if (state->blocker == idx::IndexFamilyPhysicalCapabilityBlocker::none) {
      Require(row.runtime_available && row.benchmark_clean,
              "complete family did not expose runtime state " + row.family_id);
      Require(row.validation_state == "validation_available",
              "complete family validation state mismatch " + row.family_id);
      Require(row.repair_state == "repair_available",
              "complete family repair state mismatch " + row.family_id);
    } else {
      Require(!row.runtime_available && !row.benchmark_clean,
              "incomplete family exposed runtime state " + row.family_id);
      Require(row.validation_state.find("available") == std::string::npos,
              "incomplete family exposed validation availability " +
                  row.family_id);
      Require(row.repair_state.find("available") == std::string::npos,
              "incomplete family exposed repair availability " +
                  row.family_id);
    }
  }
}

void RequireReferenceAndPolicyBlockedStayNonPhysical(
    const idx::IndexFamilyManagementSurface& surface) {
  const auto& reference = RowFor(surface, "reference_emulated");
  Require(reference.persistence == "reference_emulated",
          "reference_emulated persistence changed");
  Require(reference.native_physical_family == "reference_emulated",
          "reference_emulated native family changed");
  Require(!reference.runtime_available && !reference.benchmark_clean,
          "reference_emulated became runtime or benchmark-clean");
  Require(reference.validation_state == "reference_mapping_only_non_physical",
          "reference_emulated validation state is not mapping-only");
  Require(reference.repair_state == "reference_mapping_only_non_physical",
          "reference_emulated repair state is not mapping-only");
  Require(!reference.reference_authority && !reference.provider_authority &&
              !reference.transaction_finality_authority && !reference.recovery_authority,
          "reference_emulated row claimed authority");

  const auto& policy = RowFor(surface, "advanced_vector_policy_blocked");
  Require(policy.persistence == "policy_blocked",
          "policy_blocked persistence changed");
  Require(policy.native_physical_family == "policy_blocked",
          "policy_blocked native family changed");
  Require(!policy.runtime_available && !policy.benchmark_clean,
          "policy_blocked became runtime or benchmark-clean");
  Require(policy.validation_state == "policy_blocked_non_physical",
          "policy_blocked validation state mismatch");
  Require(policy.repair_state == "policy_blocked_non_physical",
          "policy_blocked repair state mismatch");
  Require(!policy.provider_authority && !policy.execution_authority,
          "policy_blocked row claimed provider or execution authority");
}

idx::IndexFamilyManagementSurface BuildSurfaceWithSensitiveLastError() {
  platform::DiagnosticRecord diagnostic;
  diagnostic.status = {platform::StatusCode::platform_required_feature_missing,
                       platform::Severity::error,
                       platform::Subsystem::engine};
  diagnostic.diagnostic_code = "INDEX.TEST.LAST_ERROR";
  diagnostic.message_key = "index.test.last_error";
  diagnostic.arguments.push_back(
      {"detail", "private_path_token token=plain-secret"});
  diagnostic.source_component = "test.index.management.surface";

  idx::IndexFamilyManagementSurfaceRequest request;
  request.redact_diagnostic_details = true;
  request.last_errors.push_back({idx::IndexFamily::btree, diagnostic});
  return idx::BuildIndexFamilyManagementSupportBundle(request);
}

void RequireLastErrorIsRedactedAndNonAuthoritative(
    const idx::IndexFamilyManagementSurface& surface) {
  const auto& btree = RowFor(surface, "btree");
  const auto* registry_state =
      idx::FindBuiltinIndexFamilyPhysicalCapabilityState(idx::IndexFamily::btree);
  Require(registry_state != nullptr, "btree registry state missing");
  Require(btree.last_error_present, "last error not attached");
  Require(btree.last_diagnostic_code == "INDEX.TEST.LAST_ERROR",
          "last diagnostic code missing");
  Require(btree.last_message_key == "index.test.last_error",
          "last message key missing");
  Require(btree.last_error_detail == "<redacted-detail-present>",
          "sensitive last error detail was not redacted");
  Require(btree.diagnostic_detail_redacted,
          "redaction marker was not retained");
  Require(btree.runtime_available == registry_state->runtime_available,
          "last error changed runtime availability");
  Require(btree.benchmark_clean == registry_state->benchmark_clean,
          "last error changed benchmark-clean state");
  Require(!btree.catalog_authority && !btree.execution_authority &&
              !btree.visibility_authority && !btree.security_authority &&
              !btree.transaction_finality_authority &&
              !btree.recovery_authority,
          "last error became authority");
}

void RequireSupportBundleRowsKeepRequiredState(
    const idx::IndexFamilyManagementSurface& surface) {
  const auto& btree = BundleRowFor(surface, "btree");
  const std::vector<std::string> required_fields = {
      "family_id",
      "family_name",
      "persistence",
      "key_model",
      "native_physical_family",
      "semantic_profile",
      "declared_capability",
      "planner_contract",
      "implemented",
      "physical_reader",
      "physical_writer",
      "maintenance",
      "validate",
      "repair",
      "recovery_reopen",
      "rebuild",
      "runtime_available",
      "benchmark_clean",
      "blocker",
      "blocker_diagnostic_code",
      "blocker_message_key",
      "blocker_detail",
      "validation_state",
      "repair_state",
      "last_error_present",
      "last_diagnostic_code",
      "last_message_key",
      "last_error_detail",
      "observational_only",
      "transaction_finality_authority",
      "recovery_authority"};
  for (const auto& field : required_fields) {
    Require(!Field(btree, field).empty(),
            "required support bundle field dropped " + field);
  }
  Require(Field(btree, "last_error_detail") == "<redacted-detail-present>",
          "support bundle did not carry redacted last error state");
  Require(Field(btree, "last_diagnostic_code") == "INDEX.TEST.LAST_ERROR",
          "support bundle dropped last diagnostic code");
  Require(Field(btree, "transaction_finality_authority") == "false",
          "support bundle claimed finality authority");
  Require(Field(btree, "recovery_authority") == "false",
          "support bundle claimed recovery authority");
}

void RequireMetricsAreStableAndComplete(
    const idx::IndexFamilyManagementSurface& surface) {
  const auto records = idx::ExportIndexFamilyManagementMetricRecords(surface);
  const std::size_t expected_per_family = 7;
  Require(records.size() == surface.rows.size() * expected_per_family,
          "metric record count mismatch");

  const std::vector<std::string> expected_order = {
      "sb_index_family_surface_rows_total",
      "sb_index_family_declared_capability",
      "sb_index_family_implemented",
      "sb_index_family_runtime_available",
      "sb_index_family_benchmark_clean",
      "sb_index_family_blocked",
      "sb_index_family_last_error_present"};
  for (std::size_t i = 0; i < expected_order.size(); ++i) {
    Require(records[i].family == expected_order[i],
            "metric order drift at first family");
  }

  for (const auto& row : surface.rows) {
    const auto& row_count = MetricFor(
        records, "sb_index_family_surface_rows_total", row.family_id);
    Require(row_count.type == metrics::MetricType::counter,
            "surface row metric is not a counter");
    Require(row_count.value == 1.0, "surface row counter value mismatch");
    Require(Label(row_count, "redaction_class") == "management_safe",
            "metric redaction label missing");

    const auto& declared = MetricFor(
        records, "sb_index_family_declared_capability", row.family_id);
    Require(declared.type == metrics::MetricType::gauge,
            "declared metric is not a gauge");
    Require(declared.value == (row.declared_capability ? 1.0 : 0.0),
            "declared metric value mismatch " + row.family_id);

    Require(MetricFor(records,
                      "sb_index_family_implemented",
                      row.family_id)
                .value == (row.implemented ? 1.0 : 0.0),
            "implemented metric value mismatch " + row.family_id);
    Require(MetricFor(records,
                      "sb_index_family_runtime_available",
                      row.family_id)
                .value == (row.runtime_available ? 1.0 : 0.0),
            "runtime metric value mismatch " + row.family_id);
    Require(MetricFor(records,
                      "sb_index_family_benchmark_clean",
                      row.family_id)
                .value == (row.benchmark_clean ? 1.0 : 0.0),
            "benchmark metric value mismatch " + row.family_id);
    Require(MetricFor(records,
                      "sb_index_family_blocked",
                      row.family_id)
                .value == (row.blocker == "none" ? 0.0 : 1.0),
            "blocker metric value mismatch " + row.family_id);
    Require(MetricFor(records,
                      "sb_index_family_last_error_present",
                      row.family_id)
                .value == (row.last_error_present ? 1.0 : 0.0),
            "last error metric value mismatch " + row.family_id);
  }

  Require(MetricFor(records,
                    "sb_index_family_runtime_available",
                    "graph")
              .value == 1.0,
          "complete graph runtime metric missing");
  Require(MetricFor(records,
                    "sb_index_family_runtime_available",
                    "btree")
              .value == 1.0,
          "complete btree runtime metric missing");
  Require(MetricFor(records,
                    "sb_index_family_benchmark_clean",
                    "btree")
              .value == 1.0,
          "complete btree benchmark metric missing");
  Require(MetricFor(records,
                    "sb_index_family_last_error_present",
                    "btree")
              .value == 1.0,
          "last error metric missing");
  Require(MetricFor(records,
                    "sb_index_family_last_error_present",
                    "graph")
              .value == 0.0,
          "last error metric leaked to other family");
}

void RequireNoRuntimeLeakMarkers(
    const idx::IndexFamilyManagementSurface& surface) {
  for (const auto& row : surface.rows) {
    Require(!ContainsMarker(row.family_id), "row family id leaked marker");
    Require(!ContainsMarker(row.family_name), "row family name leaked marker");
    Require(!ContainsMarker(row.persistence), "row persistence leaked marker");
    Require(!ContainsMarker(row.native_physical_family),
            "row native family leaked marker");
    Require(!ContainsMarker(row.semantic_profile),
            "row semantic profile leaked marker");
    Require(!ContainsMarker(row.blocker_diagnostic_code),
            "row blocker diagnostic leaked marker");
    Require(!ContainsMarker(row.blocker_message_key),
            "row blocker message leaked marker");
    Require(!ContainsMarker(row.last_diagnostic_code),
            "row last diagnostic leaked marker");
    Require(!ContainsMarker(row.last_message_key),
            "row last message leaked marker");
  }
  const auto records = idx::ExportIndexFamilyManagementMetricRecords(surface);
  for (const auto& record : records) {
    Require(!ContainsMarker(record.family), "metric family leaked marker");
    for (const auto& label : record.labels) {
      Require(!ContainsMarker(label.key), "metric label key leaked marker");
      Require(!ContainsMarker(label.value), "metric label value leaked marker");
    }
  }
}

}  // namespace

int main() {
  const auto surface = BuildSurfaceWithSensitiveLastError();
  RequireSurfaceCompletenessAndTruthfulness(surface);
  RequireReferenceAndPolicyBlockedStayNonPhysical(surface);
  RequireLastErrorIsRedactedAndNonAuthoritative(surface);
  RequireSupportBundleRowsKeepRequiredState(surface);
  RequireMetricsAreStableAndComplete(surface);
  RequireNoRuntimeLeakMarkers(surface);
  std::cout << "index_catalog_management_metrics_surface_gate=passed\n";
  return 0;
}
