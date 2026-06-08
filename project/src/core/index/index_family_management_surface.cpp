// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_family_management_surface.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::metrics::MetricType;
using scratchbird::core::metrics::MetricUnit;

const char* BoolText(bool value) { return value ? "true" : "false"; }

double BoolValue(bool value) { return value ? 1.0 : 0.0; }

std::string LowerAscii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

bool ContainsSensitiveDiagnosticMaterial(std::string_view value) {
  if (value.find('/') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos) {
    return true;
  }
  const std::string lowered = LowerAscii(value);
  static constexpr std::array<std::string_view, 8> kSensitive = {
      "password",
      "secret",
      "token",
      "credential",
      "private_key",
      "encryption_key",
      "api_key",
      "key="};
  return std::any_of(kSensitive.begin(), kSensitive.end(),
                     [&](std::string_view needle) {
                       return lowered.find(needle) != std::string::npos;
                     });
}

std::string DiagnosticDetail(const DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      return argument.value;
    }
  }
  if (!diagnostic.remediation_hint.empty()) {
    return diagnostic.remediation_hint;
  }
  return {};
}

const IndexFamilySurfaceLastError* LastErrorForFamily(
    const IndexFamilyManagementSurfaceRequest& request,
    IndexFamily family) {
  const IndexFamilySurfaceLastError* found = nullptr;
  for (const auto& last_error : request.last_errors) {
    if (last_error.family == family &&
        (!last_error.diagnostic.diagnostic_code.empty() ||
         !last_error.diagnostic.message_key.empty() ||
         !DiagnosticDetail(last_error.diagnostic).empty())) {
      found = &last_error;
    }
  }
  return found;
}

std::string ValidationStateFor(const IndexFamilyDescriptor& descriptor,
                               const IndexFamilyPhysicalCapabilityState& state) {
  if (descriptor.persistence == IndexPersistenceClass::policy_blocked) {
    return "policy_blocked_non_physical";
  }
  if (descriptor.persistence == IndexPersistenceClass::donor_emulated) {
    return "donor_mapping_only_non_physical";
  }
  if (state.validate && state.runtime_available) {
    return "validation_available";
  }
  return std::string("validation_blocked_by_") +
         IndexFamilyPhysicalCapabilityBlockerName(state.blocker);
}

std::string RepairStateFor(const IndexFamilyDescriptor& descriptor,
                           const IndexFamilyPhysicalCapabilityState& state) {
  if (descriptor.persistence == IndexPersistenceClass::policy_blocked) {
    return "policy_blocked_non_physical";
  }
  if (descriptor.persistence == IndexPersistenceClass::donor_emulated) {
    return "donor_mapping_only_non_physical";
  }
  if (state.repair && state.runtime_available) {
    return "repair_available";
  }
  return std::string("repair_blocked_by_") +
         IndexFamilyPhysicalCapabilityBlockerName(state.blocker);
}

IndexFamilyPhysicalCapabilityState MissingState(IndexFamily family) {
  IndexFamilyPhysicalCapabilityState state;
  state.family = family;
  state.blocker = IndexFamilyPhysicalCapabilityBlocker::unknown_family;
  state.blocker_diagnostic_code = "INDEX.CAPABILITY.UNKNOWN_FAMILY";
  state.blocker_message_key = "index.capability.unknown_family";
  state.blocker_detail = "family lacks a built-in physical capability state";
  return state;
}

std::string BlockerDiagnosticCode(
    const IndexFamilyPhysicalCapabilityState& state) {
  if (!state.blocker_diagnostic_code.empty()) {
    return state.blocker_diagnostic_code;
  }
  return state.blocker == IndexFamilyPhysicalCapabilityBlocker::none
             ? "INDEX.CAPABILITY.NONE"
             : "INDEX.CAPABILITY.UNKNOWN_FAMILY";
}

std::string BlockerMessageKey(const IndexFamilyPhysicalCapabilityState& state) {
  if (!state.blocker_message_key.empty()) {
    return state.blocker_message_key;
  }
  return state.blocker == IndexFamilyPhysicalCapabilityBlocker::none
             ? "index.capability.none"
             : "index.capability.unknown_family";
}

std::string BlockerDetail(const IndexFamilyPhysicalCapabilityState& state) {
  if (!state.blocker_detail.empty()) {
    return state.blocker_detail;
  }
  return state.blocker == IndexFamilyPhysicalCapabilityBlocker::none
             ? "no capability blocker"
             : "family capability state is missing";
}

IndexFamilyManagementSurfaceRow BuildRow(
    const IndexFamilyDescriptor& descriptor,
    const IndexFamilyPhysicalCapabilityState& state,
    const IndexFamilyManagementSurfaceRequest& request) {
  IndexFamilyManagementSurfaceRow row;
  row.family_id = descriptor.id;
  row.family_name = descriptor.canonical_name.empty() ? descriptor.id
                                                      : descriptor.canonical_name;
  row.persistence = IndexPersistenceClassName(descriptor.persistence);
  row.key_model = IndexKeyModelName(descriptor.key_model);
  row.native_physical_family = descriptor.native_physical_family;
  row.semantic_profile = descriptor.default_semantic_profile;
  row.declared_capability = state.declared_capability;
  row.planner_contract = state.planner_contract_capability;
  row.implemented = state.implemented;
  row.physical_reader = state.physical_reader;
  row.physical_writer = state.physical_writer;
  row.maintenance = state.maintenance;
  row.validate = state.validate;
  row.repair = state.repair;
  row.recovery_reopen = state.recovery_reopen;
  row.rebuild = state.rebuild;
  row.runtime_available = state.runtime_available;
  row.benchmark_clean = state.benchmark_clean;
  row.blocker = IndexFamilyPhysicalCapabilityBlockerName(state.blocker);
  row.blocker_diagnostic_code = BlockerDiagnosticCode(state);
  row.blocker_message_key = BlockerMessageKey(state);
  row.blocker_detail = BlockerDetail(state);
  row.validation_state = ValidationStateFor(descriptor, state);
  row.repair_state = RepairStateFor(descriptor, state);
  row.last_diagnostic_code = "INDEX.LAST_ERROR.NONE";
  row.last_message_key = "index.last_error.none";
  row.last_error_detail = "none";

  if (const auto* last_error = LastErrorForFamily(request, descriptor.family)) {
    row.last_error_present = true;
    row.last_diagnostic_code =
        last_error->diagnostic.diagnostic_code.empty()
            ? "INDEX.LAST_ERROR.UNKNOWN"
            : last_error->diagnostic.diagnostic_code;
    row.last_message_key =
        last_error->diagnostic.message_key.empty()
            ? "index.last_error.unknown"
            : last_error->diagnostic.message_key;
    const std::string detail = DiagnosticDetail(last_error->diagnostic);
    if (request.redact_diagnostic_details &&
        ContainsSensitiveDiagnosticMaterial(detail)) {
      row.last_error_detail = "<redacted-detail-present>";
      row.diagnostic_detail_redacted = true;
    } else {
      row.last_error_detail = detail.empty() ? "present_without_detail" : detail;
    }
  }
  return row;
}

void AddField(IndexFamilySupportBundleRow* out,
              std::string key,
              std::string value) {
  out->fields.push_back({std::move(key), std::move(value)});
}

IndexFamilySupportBundleRow SupportBundleRow(
    const IndexFamilyManagementSurfaceRow& row) {
  IndexFamilySupportBundleRow out;
  out.family_id = row.family_id;
  AddField(&out, "surface", "sys.index_family_runtime_state");
  AddField(&out, "surface_version", "irc170.v1");
  AddField(&out, "family_id", row.family_id);
  AddField(&out, "family_name", row.family_name);
  AddField(&out, "persistence", row.persistence);
  AddField(&out, "key_model", row.key_model);
  AddField(&out, "native_physical_family", row.native_physical_family);
  AddField(&out, "semantic_profile", row.semantic_profile);
  AddField(&out, "declared_capability", BoolText(row.declared_capability));
  AddField(&out, "planner_contract", BoolText(row.planner_contract));
  AddField(&out, "implemented", BoolText(row.implemented));
  AddField(&out, "physical_reader", BoolText(row.physical_reader));
  AddField(&out, "physical_writer", BoolText(row.physical_writer));
  AddField(&out, "maintenance", BoolText(row.maintenance));
  AddField(&out, "validate", BoolText(row.validate));
  AddField(&out, "repair", BoolText(row.repair));
  AddField(&out, "recovery_reopen", BoolText(row.recovery_reopen));
  AddField(&out, "rebuild", BoolText(row.rebuild));
  AddField(&out, "runtime_available", BoolText(row.runtime_available));
  AddField(&out, "benchmark_clean", BoolText(row.benchmark_clean));
  AddField(&out, "blocker", row.blocker);
  AddField(&out, "blocker_diagnostic_code", row.blocker_diagnostic_code);
  AddField(&out, "blocker_message_key", row.blocker_message_key);
  AddField(&out, "blocker_detail", row.blocker_detail);
  AddField(&out, "validation_state", row.validation_state);
  AddField(&out, "repair_state", row.repair_state);
  AddField(&out, "last_error_present", BoolText(row.last_error_present));
  AddField(&out, "last_diagnostic_code", row.last_diagnostic_code);
  AddField(&out, "last_message_key", row.last_message_key);
  AddField(&out, "last_error_detail", row.last_error_detail);
  AddField(&out, "diagnostic_detail_redacted",
           BoolText(row.diagnostic_detail_redacted));
  AddField(&out, "observational_only", BoolText(row.observational_only));
  AddField(&out, "catalog_authority", BoolText(row.catalog_authority));
  AddField(&out, "execution_authority", BoolText(row.execution_authority));
  AddField(&out, "visibility_authority", BoolText(row.visibility_authority));
  AddField(&out, "security_authority", BoolText(row.security_authority));
  AddField(&out, "transaction_finality_authority",
           BoolText(row.transaction_finality_authority));
  AddField(&out, "recovery_authority", BoolText(row.recovery_authority));
  AddField(&out, "parser_authority", BoolText(row.parser_authority));
  AddField(&out, "donor_authority", BoolText(row.donor_authority));
  AddField(&out, "provider_authority", BoolText(row.provider_authority));
  return out;
}

std::vector<IndexFamilyMetricLabel> MetricLabels(
    const IndexFamilyManagementSurfaceRow& row,
    std::string state_name) {
  return {{"family_id", row.family_id},
          {"family_name", row.family_name},
          {"persistence", row.persistence},
          {"native_physical_family", row.native_physical_family},
          {"semantic_profile", row.semantic_profile},
          {"blocker", row.blocker},
          {"state", std::move(state_name)},
          {"redaction_class", "management_safe"}};
}

IndexFamilyMetricRecord MetricRecord(
    const IndexFamilyManagementSurfaceRow& row,
    std::string family,
    MetricType type,
    std::string state_name,
    double value) {
  IndexFamilyMetricRecord record;
  record.family = std::move(family);
  record.type = type;
  record.unit = MetricUnit::count;
  record.value = value;
  record.labels = MetricLabels(row, std::move(state_name));
  return record;
}

}  // namespace

IndexFamilyManagementSurface BuildIndexFamilyManagementSurface(
    const IndexFamilyManagementSurfaceRequest& request) {
  IndexFamilyManagementSurface surface;
  surface.redaction_applied = request.redact_diagnostic_details;
  for (const auto& descriptor : BuiltinIndexFamilyDescriptors()) {
    const auto* found_state =
        FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    const auto state =
        found_state == nullptr ? MissingState(descriptor.family) : *found_state;
    surface.rows.push_back(BuildRow(descriptor, state, request));
  }
  if (request.include_support_bundle_rows) {
    for (const auto& row : surface.rows) {
      surface.support_bundle_rows.push_back(SupportBundleRow(row));
    }
  }
  return surface;
}

std::vector<IndexFamilyMetricRecord> ExportIndexFamilyManagementMetricRecords(
    const std::vector<IndexFamilyManagementSurfaceRow>& rows) {
  std::vector<IndexFamilyMetricRecord> records;
  records.reserve(rows.size() * 7);
  for (const auto& row : rows) {
    records.push_back(MetricRecord(row,
                                   "sb_index_family_surface_rows_total",
                                   MetricType::counter,
                                   "surface_row",
                                   1.0));
    records.push_back(MetricRecord(row,
                                   "sb_index_family_declared_capability",
                                   MetricType::gauge,
                                   "declared_capability",
                                   BoolValue(row.declared_capability)));
    records.push_back(MetricRecord(row,
                                   "sb_index_family_implemented",
                                   MetricType::gauge,
                                   "implemented",
                                   BoolValue(row.implemented)));
    records.push_back(MetricRecord(row,
                                   "sb_index_family_runtime_available",
                                   MetricType::gauge,
                                   "runtime_available",
                                   BoolValue(row.runtime_available)));
    records.push_back(MetricRecord(row,
                                   "sb_index_family_benchmark_clean",
                                   MetricType::gauge,
                                   "benchmark_clean",
                                   BoolValue(row.benchmark_clean)));
    records.push_back(MetricRecord(row,
                                   "sb_index_family_blocked",
                                   MetricType::gauge,
                                   "blocked",
                                   BoolValue(row.blocker != "none")));
    records.push_back(MetricRecord(row,
                                   "sb_index_family_last_error_present",
                                   MetricType::gauge,
                                   "last_error_present",
                                   BoolValue(row.last_error_present)));
  }
  return records;
}

std::vector<IndexFamilyMetricRecord> ExportIndexFamilyManagementMetricRecords(
    const IndexFamilyManagementSurface& surface) {
  return ExportIndexFamilyManagementMetricRecords(surface.rows);
}

}  // namespace scratchbird::core::index
