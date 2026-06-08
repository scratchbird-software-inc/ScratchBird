// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Args {
  std::string profile;
  fs::path fixture_root;
  std::string report;
};

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::string path;
};

struct FixtureEntry {
  std::string path;
  std::string fixture_class;
  std::string inferred_profile;
  std::uint64_t content_hash = 0;
  bool allowed = false;
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

bool ReadFile(const fs::path& path, std::string* content) {
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
  for (int i = 1; i < argc; ++i) {
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
    } else if (key == "--fixture-root") {
      std::string value;
      if (!require_value(&value)) return false;
      args->fixture_root = value;
    } else if (key == "--report") {
      if (!require_value(&args->report)) return false;
    } else {
      *error = "unknown argument: " + key;
      return false;
    }
  }

  if (args->profile.empty() || args->fixture_root.empty() || args->report.empty()) {
    *error = "--profile, --fixture-root, and --report are required";
    return false;
  }

  args->profile = ToLower(args->profile);
  return true;
}

bool IsFixtureFile(const fs::path& path) {
  const std::string extension = ToLower(path.extension().string());
  return extension == ".yaml" || extension == ".yml" || extension == ".json";
}

std::string ClassifyFixture(const std::string& path, const std::string& content) {
  const std::string lower_path = ToLower(path);
  const std::string lower_content = ToLower(content);

  if (Contains(lower_path, "/refusal/") || Contains(lower_content, "expected_status: refusal") || Contains(lower_content, "fixture_class: refusal")) {
    return "refusal";
  }
  if (Contains(lower_content, "row_kind: refusal_command") || Contains(lower_content, "row_kind: refusal")) {
    return "refusal";
  }
  if (Contains(lower_path, "/package_manifest/") || Contains(lower_content, "package_manifest") || Contains(lower_content, "fixture_class: package_manifest")) {
    return "package_manifest";
  }
  if (Contains(lower_path, "/alias/") || Contains(lower_content, "fixture_class: alias")) {
    return "alias";
  }
  if (Contains(lower_content, "row_kind: alias_command") || Contains(lower_content, "row_kind: alias")) {
    return "alias";
  }
  if (Contains(lower_content, "expected_status: accepted") || Contains(lower_content, "fixture_class: accepted")) {
    return "accepted";
  }
  if (Contains(lower_content, "row_kind: accepted_command") || Contains(lower_content, "row_kind: accepted")) {
    return "accepted";
  }
  return "unknown";
}

std::string InferProfile(const std::string& path, const std::string& content) {
  const std::string lower_path = ToLower(path);
  const std::string lower_content = ToLower(content);

  if (Contains(lower_path, "private_cluster") || Contains(lower_content, "profile: private_cluster")) {
    return "private_cluster";
  }
  if (Contains(lower_path, "public_node") || Contains(lower_content, "profile: public_node")) {
    return "public_node";
  }
  if (Contains(lower_path, "dev_only") || Contains(lower_content, "profile: dev_only")) {
    return "dev_only";
  }
  if (Contains(lower_path, "test_only") || Contains(lower_content, "profile: test_only")) {
    return "test_only";
  }
  return "unspecified";
}

bool IsAllowedForProfile(const std::string& requested_profile, const std::string& fixture_profile) {
  if (fixture_profile == "unspecified") {
    return true;
  }
  if (requested_profile == fixture_profile) {
    return true;
  }
  if (requested_profile == "private_cluster" && fixture_profile == "public_node") {
    return true;
  }
  return false;
}

std::string RelativePath(const fs::path& root, const fs::path& path) {
  std::error_code ec;
  fs::path relative = fs::relative(path, root, ec);
  if (ec) {
    return path.generic_string();
  }
  return relative.generic_string();
}

std::string BuildReport(const Args& args,
                        const std::vector<Diagnostic>& diagnostics,
                        const std::vector<FixtureEntry>& fixtures,
                        std::uint64_t manifest_hash) {
  const bool ok = diagnostics.empty();
  std::ostringstream out;
  out << "{\n";
  out << "  \"status\": \"" << (ok ? "pass" : "fail") << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(args.profile) << "\",\n";
  out << "  \"fixture_root\": \"" << JsonEscape(args.fixture_root.generic_string()) << "\",\n";
  out << "  \"manifest_hash\": \"" << manifest_hash << "\",\n";
  out << "  \"fixture_count\": " << fixtures.size() << ",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const auto& diagnostic = diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"severity\": \"" << JsonEscape(diagnostic.severity)
        << "\", \"message\": \"" << JsonEscape(diagnostic.message)
        << "\", \"path\": \"" << JsonEscape(diagnostic.path) << "\"}";
    if (i + 1 != diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"fixtures\": [\n";
  for (std::size_t i = 0; i < fixtures.size(); ++i) {
    const auto& fixture = fixtures[i];
    out << "    {\"path\": \"" << JsonEscape(fixture.path)
        << "\", \"fixture_class\": \"" << JsonEscape(fixture.fixture_class)
        << "\", \"inferred_profile\": \"" << JsonEscape(fixture.inferred_profile)
        << "\", \"content_hash\": \"" << fixture.content_hash
        << "\", \"allowed\": " << (fixture.allowed ? "true" : "false") << "}";
    if (i + 1 != fixtures.size()) out << ",";
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
    std::cerr << "usage: sb_fixture_manifest_probe --profile <profile> --fixture-root <path> --report <path>\n";
    return 5;
  }

  std::vector<Diagnostic> diagnostics;
  std::vector<FixtureEntry> fixtures;
  std::uint64_t manifest_hash = 1469598103934665603ull;
  manifest_hash = FnvaUpdate(manifest_hash, args.profile);
  manifest_hash = FnvaUpdate(manifest_hash, args.fixture_root.generic_string());

  std::error_code ec;
  if (!fs::exists(args.fixture_root, ec) || !fs::is_directory(args.fixture_root, ec)) {
    diagnostics.push_back({"SB-FIXTURE-MANIFEST-MISSING-ROOT", "error", "fixture root does not exist or is not a directory", args.fixture_root.generic_string()});
  } else {
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(args.fixture_root, ec)) {
      if (ec) {
        diagnostics.push_back({"SB-FIXTURE-MANIFEST-WALK-FAILED", "error", "failed while walking fixture root", args.fixture_root.generic_string()});
        break;
      }
      if (entry.is_regular_file() && IsFixtureFile(entry.path())) {
        paths.push_back(entry.path());
      }
    }

    std::sort(paths.begin(), paths.end(), [](const fs::path& left, const fs::path& right) {
      return left.generic_string() < right.generic_string();
    });

    for (const auto& path : paths) {
      std::string content;
      const std::string relative = RelativePath(args.fixture_root, path);
      if (!ReadFile(path, &content)) {
        diagnostics.push_back({"SB-FIXTURE-MANIFEST-READ-FAILED", "error", "fixture file could not be read", path.generic_string()});
        continue;
      }

      FixtureEntry fixture;
      fixture.path = relative;
      fixture.fixture_class = ClassifyFixture(relative, content);
      fixture.inferred_profile = InferProfile(relative, content);
      fixture.content_hash = FnvaUpdate(1469598103934665603ull, content);
      fixture.allowed = IsAllowedForProfile(args.profile, fixture.inferred_profile);

      if (fixture.fixture_class == "unknown") {
        diagnostics.push_back({"SB-FIXTURE-MANIFEST-UNKNOWN-FIXTURE-CLASS", "error", "fixture class could not be inferred", relative});
      }
      if (!fixture.allowed) {
        diagnostics.push_back({"SB-FIXTURE-MANIFEST-PROFILE-MISMATCH", "error", "fixture is not allowed for requested profile", relative});
      }

      manifest_hash = FnvaUpdate(manifest_hash, fixture.path);
      manifest_hash = FnvaUpdate(manifest_hash, fixture.fixture_class);
      manifest_hash = FnvaUpdate(manifest_hash, fixture.inferred_profile);
      manifest_hash = FnvaUpdate(manifest_hash, std::to_string(fixture.content_hash));
      fixtures.push_back(fixture);
    }
  }

  if (fixtures.empty()) {
    diagnostics.push_back({"SB-FIXTURE-MANIFEST-EMPTY", "error", "no fixture files were found", args.fixture_root.generic_string()});
  }

  std::ofstream out(args.report);
  if (!out) {
    std::cerr << "failed to write report: " << args.report << "\n";
    return 4;
  }
  out << BuildReport(args, diagnostics, fixtures, manifest_hash);

  return diagnostics.empty() ? 0 : 1;
}
