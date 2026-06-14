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
#include <set>
#include <string>
#include <vector>

namespace {

struct OperationSeen {
  bool operation_id = false;
  bool function_name = false;
  bool request_type = false;
  bool result_type = false;
  bool header = false;
  bool implementation = false;
  bool implementation_status = false;
  std::string header_path;
  std::string implementation_path;
  std::string status;
};

std::string Trim(std::string value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> SplitRegistryPaths(const std::string& value) {
  std::vector<std::string> paths;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(';', start);
    paths.push_back(Trim(value.substr(start, end == std::string::npos ? end : end - start)));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return paths;
}

bool IsRegistryFile(const std::vector<std::filesystem::path>& roots,
                    const std::string& value) {
  if (value.empty()) return true;
  for (const auto& root : roots) {
    if (std::filesystem::is_regular_file(root / value)) return true;
  }
  return false;
}

std::string ValueAfterColon(const std::string& line) {
  const auto pos = line.find(':');
  if (pos == std::string::npos) {
    return {};
  }
  return Trim(line.substr(pos + 1));
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path registry;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--registry" && i + 1 < argc) {
      registry = argv[++i];
    }
  }
  if (registry.empty()) {
    registry = "project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml";
  }
  std::ifstream in(registry);
  if (!in) {
    std::cerr << "registry_unreadable=" << registry << "\n";
    return 2;
  }
  const auto root = registry.parent_path();
  std::vector<std::filesystem::path> registry_roots{root};
  if (root.has_parent_path() && root.parent_path().has_parent_path()) {
    registry_roots.push_back(root.parent_path().parent_path());
  }
  std::vector<OperationSeen> operations;
  OperationSeen current;
  bool in_operation = false;
  std::string line;
  while (std::getline(in, line)) {
    const auto trimmed = Trim(line);
    if (StartsWith(trimmed, "- operation_id:")) {
      if (in_operation) {
        operations.push_back(current);
      }
      current = OperationSeen{};
      current.operation_id = !ValueAfterColon(trimmed).empty();
      in_operation = true;
      continue;
    }
    if (!in_operation) {
      continue;
    }
    if (StartsWith(trimmed, "function_name:")) {
      current.function_name = !ValueAfterColon(trimmed).empty();
    } else if (StartsWith(trimmed, "request_type:")) {
      current.request_type = !ValueAfterColon(trimmed).empty();
    } else if (StartsWith(trimmed, "result_type:")) {
      current.result_type = !ValueAfterColon(trimmed).empty();
    } else if (StartsWith(trimmed, "header:")) {
      current.header = !ValueAfterColon(trimmed).empty();
      current.header_path = ValueAfterColon(trimmed);
    } else if (StartsWith(trimmed, "implementation:")) {
      current.implementation = !ValueAfterColon(trimmed).empty();
      current.implementation_path = ValueAfterColon(trimmed);
    } else if (StartsWith(trimmed, "implementation_status:")) {
      current.implementation_status = !ValueAfterColon(trimmed).empty();
      current.status = ValueAfterColon(trimmed);
    }
  }
  if (in_operation) {
    operations.push_back(current);
  }

  std::size_t missing_fields = 0;
  std::size_t missing_files = 0;
  std::size_t unknown_statuses = 0;
  const std::set<std::string> allowed_statuses = {
      "stubbed_fail_closed", "contract_defined", "vertical_slice_implemented", "current_stage_complete",
      "cluster_placeholder_fail_closed", "deferred_by_architecture", "behavior_implemented",
      "compile_gated_provider_boundary_fail_closed", "fail_closed_cluster_mapping_unavailable",
      "physical_provider_implemented"};
  for (const auto& op : operations) {
    if (!(op.operation_id && op.function_name && op.request_type && op.result_type && op.header &&
          op.implementation && op.implementation_status)) {
      ++missing_fields;
    }
    if (!allowed_statuses.contains(op.status)) { ++unknown_statuses; }
    for (const auto& header_path : SplitRegistryPaths(op.header_path)) {
      if (!IsRegistryFile(registry_roots, header_path)) ++missing_files;
    }
    for (const auto& implementation_path : SplitRegistryPaths(op.implementation_path)) {
      if (!IsRegistryFile(registry_roots, implementation_path)) {
        ++missing_files;
      }
    }
  }
  const bool ok = !operations.empty() && missing_fields == 0 && missing_files == 0 && unknown_statuses == 0;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"operations\": " << operations.size() << ",\n";
  std::cout << "  \"missing_fields\": " << missing_fields << ",\n";
  std::cout << "  \"missing_files\": " << missing_files << ",\n";
  std::cout << "  \"unknown_statuses\": " << unknown_statuses << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
