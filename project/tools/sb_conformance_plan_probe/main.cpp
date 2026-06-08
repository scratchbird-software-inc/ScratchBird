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
  std::string fixture_manifest;
  std::string registry_snapshot;
  std::string out_path;
};

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::string field;
};

struct PlanEntry {
  std::size_t sequence = 0;
  std::string fixture_path;
  std::string expected_stage;
};

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool Contains(const std::string& value, const std::string& needle) {
  return ToLower(value).find(ToLower(needle)) != std::string::npos;
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

bool ReadFile(const std::string& path, std::string* content) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *content = buffer.str();
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args, std::string* error) {
  if (argc < 2 || std::string(argv[1]) != "create") {
    *error = "usage: sb_conformance_plan_probe create --profile <profile> --fixture-manifest <path> --registry-snapshot <path> --out <path>";
    return false;
  }

  for (int i = 2; i < argc; ++i) {
    const std::string key = argv[i];
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
    } else if (key == "--fixture-manifest") {
      if (!require_value(&args->fixture_manifest)) return false;
    } else if (key == "--registry-snapshot") {
      if (!require_value(&args->registry_snapshot)) return false;
    } else if (key == "--out") {
      if (!require_value(&args->out_path)) return false;
    } else {
      *error = "unknown argument: " + key;
      return false;
    }
  }

  if (args->profile.empty() || args->fixture_manifest.empty() || args->registry_snapshot.empty() || args->out_path.empty()) {
    *error = "--profile, --fixture-manifest, --registry-snapshot, and --out are required";
    return false;
  }

  args->profile = ToLower(args->profile);
  return true;
}

std::string ExtractJsonStringValue(const std::string& text, const std::string& field) {
  const std::string needle = "\"" + field + "\"";
  const std::size_t key_pos = text.find(needle);
  if (key_pos == std::string::npos) {
    return "";
  }
  const std::size_t colon = text.find(':', key_pos + needle.size());
  if (colon == std::string::npos) {
    return "";
  }
  const std::size_t first_quote = text.find('"', colon + 1);
  if (first_quote == std::string::npos) {
    return "";
  }
  const std::size_t second_quote = text.find('"', first_quote + 1);
  if (second_quote == std::string::npos) {
    return "";
  }
  return text.substr(first_quote + 1, second_quote - first_quote - 1);
}

std::vector<std::string> ExtractFixturePaths(const std::string& fixture_manifest_text) {
  std::vector<std::string> paths;
  const std::size_t fixtures_pos = fixture_manifest_text.find("\"fixtures\"");
  if (fixtures_pos == std::string::npos) {
    return paths;
  }

  std::size_t pos = fixtures_pos;
  const std::string needle = "\"path\"";
  while ((pos = fixture_manifest_text.find(needle, pos)) != std::string::npos) {
    const std::size_t colon = fixture_manifest_text.find(':', pos + needle.size());
    if (colon == std::string::npos) {
      break;
    }
    const std::size_t first_quote = fixture_manifest_text.find('"', colon + 1);
    if (first_quote == std::string::npos) {
      break;
    }
    const std::size_t second_quote = fixture_manifest_text.find('"', first_quote + 1);
    if (second_quote == std::string::npos) {
      break;
    }
    paths.push_back(fixture_manifest_text.substr(first_quote + 1, second_quote - first_quote - 1));
    pos = second_quote + 1;
  }

  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

std::string ExpectedStageForFixture(const std::string& fixture_path) {
  const std::string lower = ToLower(fixture_path);
  if (Contains(lower, "package_manifest")) {
    return "package_gate";
  }
  if (Contains(lower, "refusal")) {
    return "parse_refusal";
  }
  return "engine_api_bridge";
}

std::string BuildReport(const Args& args,
                        const std::string& fixture_manifest_hash,
                        const std::string& registry_snapshot_hash,
                        const std::vector<Diagnostic>& diagnostics,
                        const std::vector<PlanEntry>& plan,
                        std::uint64_t plan_hash) {
  const bool ok = diagnostics.empty();
  std::ostringstream out;
  out << "{\n";
  out << "  \"status\": \"" << (ok ? "pass" : "fail") << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(args.profile) << "\",\n";
  out << "  \"fixture_manifest\": \"" << JsonEscape(args.fixture_manifest) << "\",\n";
  out << "  \"fixture_manifest_hash\": \"" << JsonEscape(fixture_manifest_hash) << "\",\n";
  out << "  \"registry_snapshot\": \"" << JsonEscape(args.registry_snapshot) << "\",\n";
  out << "  \"registry_snapshot_hash\": \"" << JsonEscape(registry_snapshot_hash) << "\",\n";
  out << "  \"plan_hash\": \"" << plan_hash << "\",\n";
  out << "  \"plan_count\": " << plan.size() << ",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const auto& diagnostic = diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"severity\": \"" << JsonEscape(diagnostic.severity)
        << "\", \"message\": \"" << JsonEscape(diagnostic.message)
        << "\", \"field\": \"" << JsonEscape(diagnostic.field) << "\"}";
    if (i + 1 != diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"plan\": [\n";
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const auto& entry = plan[i];
    out << "    {\"sequence\": " << entry.sequence
        << ", \"fixture_path\": \"" << JsonEscape(entry.fixture_path)
        << "\", \"expected_stage\": \"" << JsonEscape(entry.expected_stage)
        << "\", \"profile\": \"" << JsonEscape(args.profile) << "\"}";
    if (i + 1 != plan.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string error;
  if (!ParseArgs(argc, argv, &args, &error)) {
    std::cerr << error << "\n";
    return 5;
  }

  std::string fixture_manifest_text;
  std::string registry_snapshot_text;
  if (!ReadFile(args.fixture_manifest, &fixture_manifest_text)) {
    std::cerr << "failed to read fixture manifest: " << args.fixture_manifest << "\n";
    return 4;
  }
  if (!ReadFile(args.registry_snapshot, &registry_snapshot_text)) {
    std::cerr << "failed to read registry snapshot: " << args.registry_snapshot << "\n";
    return 4;
  }

  std::vector<Diagnostic> diagnostics;
  if (!Contains(fixture_manifest_text, "\"manifest_hash\"") || !Contains(fixture_manifest_text, "\"fixtures\"")) {
    diagnostics.push_back({"SB-CONFORMANCE-PLAN-BAD-FIXTURE-MANIFEST", "error", "fixture manifest is missing manifest_hash or fixtures", "fixture_manifest"});
  }
  if (!Contains(registry_snapshot_text, "\"snapshot_hash\"") || !Contains(registry_snapshot_text, "\"entries\"")) {
    diagnostics.push_back({"SB-CONFORMANCE-PLAN-BAD-REGISTRY-SNAPSHOT", "error", "registry snapshot is missing snapshot_hash or entries", "registry_snapshot"});
  }
  if (args.profile == "public_node" && (Contains(fixture_manifest_text, "private_cluster_command_authority") || Contains(registry_snapshot_text, "private_cluster_command_authority") || Contains(registry_snapshot_text, "cluster_decision_service"))) {
    diagnostics.push_back({"SB-CONFORMANCE-PLAN-PUBLIC-PRIVATE-LEAK", "error", "public_node conformance plan input contains private cluster authority terms", "profile"});
  }

  std::vector<PlanEntry> plan;
  const std::vector<std::string> fixture_paths = ExtractFixturePaths(fixture_manifest_text);
  for (const auto& path : fixture_paths) {
    plan.push_back({plan.size() + 1, path, ExpectedStageForFixture(path)});
  }

  if (plan.empty()) {
    diagnostics.push_back({"SB-CONFORMANCE-PLAN-EMPTY", "error", "no fixture entries were available for conformance planning", "fixtures"});
  }

  const std::string fixture_manifest_hash = ExtractJsonStringValue(fixture_manifest_text, "manifest_hash");
  const std::string registry_snapshot_hash = ExtractJsonStringValue(registry_snapshot_text, "snapshot_hash");

  std::uint64_t plan_hash = 1469598103934665603ull;
  plan_hash = FnvaUpdate(plan_hash, args.profile);
  plan_hash = FnvaUpdate(plan_hash, fixture_manifest_hash);
  plan_hash = FnvaUpdate(plan_hash, registry_snapshot_hash);
  for (const auto& entry : plan) {
    plan_hash = FnvaUpdate(plan_hash, entry.fixture_path);
    plan_hash = FnvaUpdate(plan_hash, entry.expected_stage);
  }

  std::ofstream out(args.out_path);
  if (!out) {
    std::cerr << "failed to write conformance plan: " << args.out_path << "\n";
    return 4;
  }
  out << BuildReport(args, fixture_manifest_hash, registry_snapshot_hash, diagnostics, plan, plan_hash);

  return diagnostics.empty() ? 0 : 1;
}
