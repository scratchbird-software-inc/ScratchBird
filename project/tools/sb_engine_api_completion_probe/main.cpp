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

struct Operation {
  std::map<std::string, std::string> fields;
};

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

std::vector<Operation> ReadOperations(const std::filesystem::path& registry) {
  std::ifstream in(registry);
  std::vector<Operation> ops;
  Operation current;
  bool in_operation = false;
  std::string line;
  while (std::getline(in, line)) {
    const auto trimmed = Trim(line);
    if (StartsWith(trimmed, "- operation_id:")) {
      if (in_operation) { ops.push_back(current); }
      current = Operation{};
      current.fields["operation_id"] = ValueAfterColon(trimmed);
      in_operation = true;
      continue;
    }
    if (!in_operation) { continue; }
    const auto pos = trimmed.find(':');
    if (pos == std::string::npos) { continue; }
    current.fields[trimmed.substr(0, pos)] = ValueAfterColon(trimmed);
  }
  if (in_operation) { ops.push_back(current); }
  return ops;
}

std::string Field(const Operation& op, const std::string& field) {
  const auto it = op.fields.find(field);
  return it == op.fields.end() ? std::string{} : it->second;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path registry;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--registry" && i + 1 < argc) { registry = argv[++i]; }
  }
  if (registry.empty()) { registry = "project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml"; }
  const auto ops = ReadOperations(registry);
  const std::set<std::string> allowed_statuses = {
      "stubbed_fail_closed", "contract_defined", "vertical_slice_implemented", "current_stage_complete",
      "cluster_placeholder_fail_closed", "deferred_by_architecture", "behavior_implemented"};
  const std::set<std::string> required_fields = {
      "operation_id", "function_name", "request_type", "result_type", "family", "authority_domain", "header",
      "implementation", "implementation_status", "default_diagnostic", "cluster_only", "accepts_names",
      "requires_transaction_context", "requires_security_context", "requires_cluster_authority", "sblr_mapping_required", "notes"};
  const std::set<std::string> required_families = {
      "catalog", "ddl", "dml", "transaction", "query", "security", "observability", "management", "cluster",
      "extensibility", "nosql"};
  std::set<std::string> families_seen;
  std::set<std::string> operation_ids_seen;
  std::size_t missing_required_fields = 0;
  std::size_t unknown_statuses = 0;
  std::size_t ambiguous_notes = 0;
  std::size_t cluster_contract_errors = 0;
  std::size_t duplicate_operation_ids = 0;
  std::size_t current_stage_complete = 0;
  std::size_t behavior_implemented = 0;
  std::size_t non_cluster_remaining = 0;
  std::size_t cluster_excluded = 0;
  for (const auto& op : ops) {
    const auto operation_id = Field(op, "operation_id");
    if (!operation_id.empty() && !operation_ids_seen.insert(operation_id).second) { ++duplicate_operation_ids; }
    for (const auto& field : required_fields) {
      if (Field(op, field).empty()) { ++missing_required_fields; }
    }
    const auto status = Field(op, "implementation_status");
    if (!allowed_statuses.contains(status)) { ++unknown_statuses; }
    if (Field(op, "notes").find("deterministic stub") != std::string::npos) { ++ambiguous_notes; }
    const auto family = Field(op, "family");
    families_seen.insert(family);
    if (family == "cluster" && (Field(op, "requires_cluster_authority") != "true" || status != "cluster_placeholder_fail_closed")) {
      ++cluster_contract_errors;
    }
    if (status == "current_stage_complete") { ++current_stage_complete; }
    if (status == "behavior_implemented") { ++behavior_implemented; }
    if (family == "cluster" || Field(op, "requires_cluster_authority") == "true") { ++cluster_excluded; }
    else if (status != "behavior_implemented") { ++non_cluster_remaining; }
  }
  std::size_t missing_families = 0;
  for (const auto& family : required_families) {
    if (!families_seen.contains(family)) { ++missing_families; }
  }
  const bool ok = !ops.empty() && missing_required_fields == 0 && unknown_statuses == 0 && ambiguous_notes == 0 &&
                  cluster_contract_errors == 0 && duplicate_operation_ids == 0 && missing_families == 0 &&
                  (current_stage_complete >= 0) && behavior_implemented >= 70 && non_cluster_remaining == 0 &&
                  cluster_excluded == 5;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"operations\": " << ops.size() << ",\n";
  std::cout << "  \"current_stage_complete\": " << current_stage_complete << ",\n";
  std::cout << "  \"behavior_implemented\": " << behavior_implemented << ",\n";
  std::cout << "  \"non_cluster_remaining\": " << non_cluster_remaining << ",\n";
  std::cout << "  \"cluster_excluded\": " << cluster_excluded << ",\n";
  std::cout << "  \"missing_required_fields\": " << missing_required_fields << ",\n";
  std::cout << "  \"unknown_statuses\": " << unknown_statuses << ",\n";
  std::cout << "  \"ambiguous_notes\": " << ambiguous_notes << ",\n";
  std::cout << "  \"cluster_contract_errors\": " << cluster_contract_errors << ",\n";
  std::cout << "  \"duplicate_operation_ids\": " << duplicate_operation_ids << ",\n";
  std::cout << "  \"missing_families\": " << missing_families << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
