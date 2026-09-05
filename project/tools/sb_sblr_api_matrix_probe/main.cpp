// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

std::string Trim(std::string value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) { value.erase(value.begin()); }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) { value.pop_back(); }
  return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string ValueAfterColon(const std::string& line) {
  const auto pos = line.find(':');
  if (pos == std::string::npos) { return {}; }
  return Trim(line.substr(pos + 1));
}

std::set<std::string> ReadApiOperations(const std::filesystem::path& registry) {
  std::ifstream in(registry);
  std::set<std::string> ops;
  std::string line;
  while (std::getline(in, line)) {
    const auto trimmed = Trim(line);
    if (StartsWith(trimmed, "- operation_id:")) { ops.insert(ValueAfterColon(trimmed)); }
  }
  return ops;
}

struct MatrixEntry {
  std::map<std::string, std::string> fields;
};

std::vector<MatrixEntry> ReadMatrix(const std::filesystem::path& matrix) {
  std::ifstream in(matrix);
  std::vector<MatrixEntry> entries;
  MatrixEntry current;
  bool in_entry = false;
  std::string line;
  while (std::getline(in, line)) {
    const auto trimmed = Trim(line);
    if (StartsWith(trimmed, "- sblr_operation:")) {
      if (in_entry) { entries.push_back(current); }
      current = MatrixEntry{};
      current.fields["sblr_operation"] = ValueAfterColon(trimmed);
      in_entry = true;
      continue;
    }
    if (!in_entry) { continue; }
    const auto pos = trimmed.find(':');
    if (pos == std::string::npos) { continue; }
    current.fields[trimmed.substr(0, pos)] = ValueAfterColon(trimmed);
  }
  if (in_entry) { entries.push_back(current); }
  return entries;
}

std::string Field(const MatrixEntry& entry, const std::string& field) {
  const auto it = entry.fields.find(field);
  return it == entry.fields.end() ? std::string{} : it->second;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path registry;
  std::filesystem::path matrix;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--api-registry" && i + 1 < argc) { registry = argv[++i]; }
    else if (arg == "--matrix" && i + 1 < argc) { matrix = argv[++i]; }
  }
  if (registry.empty()) { registry = "project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml"; }
  if (matrix.empty()) { matrix = "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"; }
  const auto api_ops = ReadApiOperations(registry);
  const auto entries = ReadMatrix(matrix);
  const std::set<std::string> allowed_statuses = {
      "mapped_ready", "not_sblr_callable", "cluster_provider_boundary_mapped", "mapped_refusal",
      "internal_engine_api_only", "cluster_fail_closed_mapped", "sblr_callable"};
  const std::set<std::string> allowed_maturities = {
      "codec_contract", "routing", "logical_implementation", "physical_implementation",
      "durability_proven", "production_qualified"};
  const std::set<std::string> required_fields = {
      "sblr_operation", "opcode_status", "api_operation_id", "api_function_name", "request_type", "result_type",
      "required_transaction_context", "result_shape", "diagnostic_mapping", "evidence_mapping",
      "current_implementation_status", "implementation_maturity",
      "executor_readiness_status"};
  std::set<std::string> matrix_ops;
  std::size_t missing_fields = 0;
  std::size_t unknown_statuses = 0;
  std::size_t unknown_maturities = 0;
  std::size_t mapped_ready = 0;
  std::size_t cluster_mapped = 0;
  for (const auto& entry : entries) {
    for (const auto& field : required_fields) {
      if (Field(entry, field).empty()) { ++missing_fields; }
    }
    matrix_ops.insert(Field(entry, "api_operation_id"));
    const auto status = Field(entry, "executor_readiness_status");
    if (!allowed_statuses.contains(status)) { ++unknown_statuses; }
    if (!allowed_maturities.contains(Field(entry, "implementation_maturity"))) { ++unknown_maturities; }
    if (status == "mapped_ready") { ++mapped_ready; }
    if (status == "cluster_provider_boundary_mapped" || status == "cluster_fail_closed_mapped") {
      ++cluster_mapped;
    }
  }
  std::size_t missing_api_ops = 0;
  for (const auto& op : api_ops) {
    if (!matrix_ops.contains(op)) { ++missing_api_ops; }
  }
  const bool ok = !api_ops.empty() && entries.size() == api_ops.size() && missing_fields == 0 &&
                  unknown_statuses == 0 && unknown_maturities == 0 &&
                  missing_api_ops == 0 && mapped_ready >= 100 && cluster_mapped >= 8;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"api_operations\": " << api_ops.size() << ",\n";
  std::cout << "  \"matrix_entries\": " << entries.size() << ",\n";
  std::cout << "  \"mapped_ready\": " << mapped_ready << ",\n";
  std::cout << "  \"cluster_mapped\": " << cluster_mapped << ",\n";
  std::cout << "  \"missing_fields\": " << missing_fields << ",\n";
  std::cout << "  \"unknown_statuses\": " << unknown_statuses << ",\n";
  std::cout << "  \"unknown_maturities\": " << unknown_maturities << ",\n";
  std::cout << "  \"missing_api_ops\": " << missing_api_ops << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
