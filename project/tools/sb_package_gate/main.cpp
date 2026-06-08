// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Args {
  std::string profile;
  std::string manifest;
  std::string artifact_list;
  std::string report;
};

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string message;
};

struct ArtifactDecision {
  std::string path;
  std::string declared_class;
  std::string inferred_class;
  bool allowed = false;
};

std::string Trim(const std::string& value) {
  std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  std::size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << c; break;
    }
  }
  return out.str();
}

std::uint64_t FnvaUpdate(std::uint64_t hash, const std::string& value) {
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *out = buffer.str();
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args, std::string* error) {
  if (argc < 2 || std::string(argv[1]) != "check") {
    *error = "usage: sb_package_gate check --profile <profile> --manifest <path> --artifact-list <path> --report <path>";
    return false;
  }

  for (int i = 2; i < argc; ++i) {
    std::string key = argv[i];
    auto require_value = [&](std::string* target) -> bool {
      if (i + 1 >= argc) {
        *error = "missing value for " + key;
        return false;
      }
      *target = argv[++i];
      return true;
    };

    if (key == "--profile") {
      if (!require_value(&args->profile)) return false;
    } else if (key == "--manifest") {
      if (!require_value(&args->manifest)) return false;
    } else if (key == "--artifact-list") {
      if (!require_value(&args->artifact_list)) return false;
    } else if (key == "--report") {
      if (!require_value(&args->report)) return false;
    } else {
      *error = "unknown argument: " + key;
      return false;
    }
  }

  if (args->profile.empty() || args->manifest.empty() || args->artifact_list.empty() || args->report.empty()) {
    *error = "--profile, --manifest, --artifact-list, and --report are required";
    return false;
  }

  const std::string profile = ToLower(args->profile);
  if (profile != "public_node" && profile != "private_cluster" && profile != "dev_only" && profile != "test_only") {
    *error = "unsupported profile: " + args->profile;
    return false;
  }

  args->profile = profile;
  return true;
}

std::string InferClass(const std::string& raw_path) {
  const std::string path = ToLower(raw_path);

  if (path.empty()) {
    return "unknown";
  }
  if (path.rfind("build/", 0) == 0 || Contains(path, "/build/") || Contains(path, "cmakefiles/")) {
    return "build_artifact";
  }
  if (Contains(path, "generated_private")) {
    return "generated_private";
  }
  if (Contains(path, "trade_secret") || Contains(path, "private/trade") || Contains(path, "enterprise")) {
    return "enterprise_deployment";
  }
  if (Contains(path, "sbmc_manager") || Contains(path, "/manager/cluster") || Contains(path, "cluster_manager")) {
    return "private_manager_source";
  }
  if (Contains(path, "/public_release_evidence") || path.rfind("public_release_evidence", 0) == 0) {
    return "private_spec";
  }
  if (Contains(path, "/" "docs" "/findings/") || path.rfind("docs" "/findings/", 0) == 0) {
    return "private_doc";
  }
  if (Contains(path, "/tests/private") || Contains(path, "/tests/cluster") || Contains(path, "private_cluster_test")) {
    return "private_test";
  }
  if (Contains(path, "/config/private") || Contains(path, "private_cluster_config")) {
    return "private_config";
  }
  if (Contains(path, "/src/cluster/") || path.rfind("project/src/cluster/", 0) == 0 || Contains(path, "cluster_decision_service") || Contains(path, "cluster_epoch_control") || Contains(path, "cluster_route_publish")) {
    return "private_cluster_source";
  }
  if (path.rfind("project/src/engine/internal_api/", 0) == 0 || Contains(path, "/project/src/engine/internal_api/")) {
    return "public_engine_internal_api";
  }
  if (path.rfind("project/src/parsers/shared/lowering/engine_api_bridge", 0) == 0 || Contains(path, "/project/src/parsers/shared/lowering/engine_api_bridge")) {
    return "public_parser_engine_bridge";
  }
  if (path.rfind("project/src/", 0) == 0 || Contains(path, "/project/src/")) {
    return "public_source";
  }
  if (path.rfind("project/include/", 0) == 0 || Contains(path, "/project/include/")) {
    return "public_header";
  }
  if (path.rfind("project/tools/", 0) == 0 || Contains(path, "/project/tools/")) {
    return "public_tool";
  }
  if (path.rfind("project/tests/conformance/", 0) == 0 || Contains(path, "/project/tests/conformance/")) {
    return "public_conformance";
  }
  if (path.rfind("project/resources/", 0) == 0 || Contains(path, "/project/resources/")) {
    return "public_resource";
  }
  if (path.rfind("project/drivers/", 0) == 0 || Contains(path, "/project/drivers/")) {
    return "public_driver";
  }
  if (path.rfind("project/", 0) == 0 || Contains(path, "/project/")) {
    return "public_project_file";
  }
  return "unknown";
}

bool IsPrivateClass(const std::string& artifact_class) {
  return artifact_class == "private_spec" ||
         artifact_class == "private_cluster_source" ||
         artifact_class == "private_cluster_command_authority" ||
         artifact_class == "private_manager_source" ||
         artifact_class == "private_config" ||
         artifact_class == "private_test" ||
         artifact_class == "private_doc" ||
         artifact_class == "enterprise_deployment" ||
         artifact_class == "generated_private";
}

bool IsBuildArtifact(const std::string& artifact_class) {
  return artifact_class == "build_artifact";
}

bool IsAllowedForProfile(const std::string& profile, const std::string& artifact_class) {
  if (artifact_class == "unknown" || IsBuildArtifact(artifact_class)) {
    return false;
  }
  if (profile == "public_node") {
    return !IsPrivateClass(artifact_class);
  }
  if (profile == "private_cluster") {
    return true;
  }
  if (profile == "dev_only" || profile == "test_only") {
    return artifact_class != "enterprise_deployment";
  }
  return false;
}

std::vector<ArtifactDecision> LoadArtifactList(const std::string& content) {
  std::vector<ArtifactDecision> artifacts;
  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    if (ToLower(trimmed) == "artifact_class,path") {
      continue;
    }

    ArtifactDecision artifact;
    const std::size_t comma = trimmed.find(',');
    if (comma == std::string::npos) {
      artifact.path = trimmed;
      artifact.declared_class = "";
    } else {
      artifact.declared_class = Trim(trimmed.substr(0, comma));
      artifact.path = Trim(trimmed.substr(comma + 1));
    }
    artifact.inferred_class = InferClass(artifact.path);
    artifacts.push_back(artifact);
  }
  return artifacts;
}

void CheckManifestTerms(const std::string& profile, const std::string& manifest_text, std::vector<Diagnostic>* diagnostics) {
  const std::vector<std::string> forbidden_public_terms = {
      "private_cluster_command_authority",
      "cluster_decision_service",
      "cluster_epoch_control",
      "cluster_route_publish",
      "cluster_recovery_resolution",
      "cluster_authority_active: true",
      "\"cluster_authority_active\": true",
      "parser_is_trusted: true",
      "\"parser_is_trusted\": true",
      "project/src/cluster",
      "sbmc_manager",
      "trade_secret_detail"};

  if (profile != "public_node") {
    return;
  }

  for (const auto& term : forbidden_public_terms) {
    if (Contains(manifest_text, term)) {
      diagnostics->push_back({"SB-PACKAGE-GATE-PRIVATE-MANIFEST-TERM", "error", "public_node manifest contains forbidden private term: " + term});
    }
  }
}

std::string BuildReport(const Args& args,
                        const std::vector<Diagnostic>& diagnostics,
                        const std::vector<ArtifactDecision>& artifacts,
                        std::uint64_t hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"status\": \"" << (diagnostics.empty() ? "pass" : "fail") << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(args.profile) << "\",\n";
  out << "  \"manifest\": \"" << JsonEscape(args.manifest) << "\",\n";
  out << "  \"artifact_list\": \"" << JsonEscape(args.artifact_list) << "\",\n";
  out << "  \"snapshot_hash\": \"" << hash << "\",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const auto& diagnostic = diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"severity\": \"" << JsonEscape(diagnostic.severity)
        << "\", \"message\": \"" << JsonEscape(diagnostic.message) << "\"}";
    if (i + 1 != diagnostics.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"artifacts\": [\n";
  for (std::size_t i = 0; i < artifacts.size(); ++i) {
    const auto& artifact = artifacts[i];
    out << "    {\"path\": \"" << JsonEscape(artifact.path)
        << "\", \"declared_class\": \"" << JsonEscape(artifact.declared_class)
        << "\", \"inferred_class\": \"" << JsonEscape(artifact.inferred_class)
        << "\", \"allowed\": " << (artifact.allowed ? "true" : "false") << "}";
    if (i + 1 != artifacts.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string arg_error;
  if (!ParseArgs(argc, argv, &args, &arg_error)) {
    std::cerr << arg_error << "\n";
    return 5;
  }

  std::string manifest_text;
  std::string artifact_text;
  if (!ReadFile(args.manifest, &manifest_text)) {
    std::cerr << "failed to read manifest: " << args.manifest << "\n";
    return 4;
  }
  if (!ReadFile(args.artifact_list, &artifact_text)) {
    std::cerr << "failed to read artifact list: " << args.artifact_list << "\n";
    return 4;
  }

  std::vector<Diagnostic> diagnostics;
  std::vector<ArtifactDecision> artifacts = LoadArtifactList(artifact_text);

  if (artifacts.empty()) {
    diagnostics.push_back({"SB-PACKAGE-GATE-EMPTY-ARTIFACT-LIST", "error", "artifact list contains no package artifacts"});
  }

  CheckManifestTerms(args.profile, manifest_text, &diagnostics);

  for (auto& artifact : artifacts) {
    if (!artifact.declared_class.empty()) {
      const std::string declared = ToLower(artifact.declared_class);
      if (declared != artifact.inferred_class) {
        diagnostics.push_back({"SB-PACKAGE-GATE-CLASS-DRIFT", "error", "declared class " + artifact.declared_class + " does not match inferred class " + artifact.inferred_class + " for " + artifact.path});
      }
    }

    artifact.allowed = IsAllowedForProfile(args.profile, artifact.inferred_class);
    if (!artifact.allowed) {
      diagnostics.push_back({"SB-PACKAGE-GATE-ARTIFACT-REJECTED", "error", "artifact is not allowed for profile " + args.profile + ": " + artifact.path + " [" + artifact.inferred_class + "]"});
    }
  }

  std::uint64_t hash = 1469598103934665603ull;
  hash = FnvaUpdate(hash, args.profile);
  hash = FnvaUpdate(hash, manifest_text);
  hash = FnvaUpdate(hash, artifact_text);
  for (const auto& artifact : artifacts) {
    hash = FnvaUpdate(hash, artifact.path);
    hash = FnvaUpdate(hash, artifact.inferred_class);
  }

  const std::string report = BuildReport(args, diagnostics, artifacts, hash);
  std::ofstream out(args.report);
  if (!out) {
    std::cerr << "failed to write report: " << args.report << "\n";
    return 4;
  }
  out << report;

  return diagnostics.empty() ? 0 : 1;
}
