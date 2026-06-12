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
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
  std::string command;
  fs::path spec_root;
  fs::path output_root;
  std::string profile;
  std::string package_name;
  std::string package_kind;
  std::string package_version = "0.0.0-skeleton";
};

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

const std::vector<std::string> kProfiles = {
    "public_node", "private_cluster", "dev_only", "test_only"};

const std::vector<std::string> kArtifactDirs = {
    "grammar",
    "ast",
    "bound_ast",
    "lowering",
    "diagnostics",
    "results",
    "conformance",
    "trace",
    "manifests",
    "package",
};

const std::vector<std::string> kRegistryInputs = {
    "registries/unified-surface-registry-schema.yaml",
    "registries/sbsql-native-surface-registry.yaml",
    "registries/sbsql-show-command-surface-matrix.yaml",
    "registries/sbsql-management-metrics-cluster-surface-matrix.yaml",
    "registries/reference-unified-surface-normalization-matrix.yaml",
    "registries/parser-ast-boundast-node-registry.yaml",
    "registries/sblr-operation-matrix.yaml",
    "registries/result-shape-registry.yaml",
    "registries/diagnostic-shape-registry.yaml",
    "registries/sbsql-unified-surface-release-gates.yaml",
    "conformance_manifests/sbsql_unified_surface_sblr_matrix.yaml",
    "trace/sbsql-unified-surface-sblr-matrix-trace.yaml",
};

void HashBytes(std::uint64_t& hash, std::string_view bytes) {
  for (const unsigned char ch : bytes) {
    hash ^= ch;
    hash *= kFnvPrime;
  }
}

std::string HexHash(std::uint64_t hash) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

std::optional<std::string> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

fs::path ResolveSpecRoot(const fs::path& input) {
  if (fs::is_directory(input / "registries")) return input;
  if (fs::is_directory(input / "docs" / "contracts" / "registries")) {
    return input / "docs" / "contracts";
  }
  return input;
}

bool IsSupportedProfile(const std::string& profile) {
  return std::find(kProfiles.begin(), kProfiles.end(), profile) != kProfiles.end();
}

std::string MakeDeterministicUuid(std::string_view seed) {
  std::uint64_t a = kFnvOffset;
  std::uint64_t b = kFnvOffset ^ 0x9e3779b97f4a7c15ull;
  HashBytes(a, seed);
  HashBytes(b, seed);
  HashBytes(b, HexHash(a));

  std::ostringstream hex;
  hex << std::hex << std::setfill('0') << std::setw(16) << a << std::setw(16) << b;
  std::string raw = hex.str();
  raw.resize(32, '0');
  raw[12] = '7';
  raw[16] = "89ab"[static_cast<unsigned>((raw[16] - '0') & 0x3u)];

  return raw.substr(0, 8) + "-" + raw.substr(8, 4) + "-" + raw.substr(12, 4) +
         "-" + raw.substr(16, 4) + "-" + raw.substr(20, 12);
}

std::uint64_t ComputeRegistrySnapshot(const fs::path& spec_root,
                                      std::vector<std::string>& missing_inputs) {
  std::uint64_t hash = kFnvOffset;
  for (const auto& relative : kRegistryInputs) {
    HashBytes(hash, relative);
    const fs::path path = spec_root / relative;
    auto text = ReadFile(path);
    if (!text) {
      missing_inputs.push_back(relative);
      continue;
    }
    HashBytes(hash, *text);
  }
  return hash;
}

void PrintUsage(std::ostream& out) {
  out << "Usage:\n"
      << "  sb_parser_gen package-skeleton --spec-root <path> --profile <profile> --package-name <name> --package-kind <kind> --out <dir> [--package-version <version>]\n";
}

std::optional<Options> ParseArgs(int argc, char** argv) {
  if (argc < 2) return std::nullopt;
  Options options;
  options.command = argv[1];

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) return std::nullopt;
      ++i;
      return std::string(argv[i]);
    };

    if (arg == "--spec-root") {
      auto v = value(); if (!v) return std::nullopt; options.spec_root = *v;
    } else if (arg == "--profile") {
      auto v = value(); if (!v) return std::nullopt; options.profile = *v;
    } else if (arg == "--package-name") {
      auto v = value(); if (!v) return std::nullopt; options.package_name = *v;
    } else if (arg == "--package-kind") {
      auto v = value(); if (!v) return std::nullopt; options.package_kind = *v;
    } else if (arg == "--package-version") {
      auto v = value(); if (!v) return std::nullopt; options.package_version = *v;
    } else if (arg == "--out") {
      auto v = value(); if (!v) return std::nullopt; options.output_root = *v;
    } else {
      return std::nullopt;
    }
  }
  return options;
}

bool WriteText(const fs::path& path, const std::string& text) {
  if (!path.parent_path().empty()) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

int RunPackageSkeleton(const Options& options) {
  if (options.spec_root.empty() || options.output_root.empty() || options.profile.empty() ||
      options.package_name.empty() || options.package_kind.empty() ||
      !IsSupportedProfile(options.profile)) {
    PrintUsage(std::cerr);
    return 5;
  }

  const fs::path spec_root = ResolveSpecRoot(options.spec_root);
  if (!fs::is_directory(spec_root)) {
    std::cerr << "spec root is not a directory: " << spec_root << "\n";
    return 4;
  }

  std::vector<std::string> missing_inputs;
  const std::uint64_t snapshot = ComputeRegistrySnapshot(spec_root, missing_inputs);
  if (!missing_inputs.empty()) {
    std::cerr << "missing registry inputs:\n";
    for (const auto& missing : missing_inputs) std::cerr << "  " << missing << "\n";
    return 4;
  }

  const std::string snapshot_hash = HexHash(snapshot);
  const std::string seed = options.package_name + ":" + options.package_kind + ":" +
                           options.profile + ":" + snapshot_hash;
  const std::string package_uuid = MakeDeterministicUuid("package:" + seed);
  const std::string build_uuid = MakeDeterministicUuid("build:" + seed);

  std::error_code ec;
  fs::create_directories(options.output_root, ec);
  if (ec) {
    std::cerr << "failed to create output root: " << options.output_root << "\n";
    return 6;
  }

  for (const auto& dir : kArtifactDirs) {
    fs::create_directories(options.output_root / dir, ec);
    if (ec) {
      std::cerr << "failed to create artifact directory: " << dir << "\n";
      return 6;
    }
  }

  std::ostringstream manifest;
  manifest << "manifest_format_version: 1\n";
  manifest << "parser_package_uuid: " << package_uuid << "\n";
  manifest << "parser_package_name: " << options.package_name << "\n";
  manifest << "parser_package_version: " << options.package_version << "\n";
  manifest << "build_uuid: " << build_uuid << "\n";
  manifest << "package_kind: " << options.package_kind << "\n";
  manifest << "edition_scope: " << options.profile << "\n";
  manifest << "registry_snapshot_hash: " << snapshot_hash << "\n";
  manifest << "spec_root: " << spec_root.generic_string() << "\n";
  manifest << "generated_artifact_inventory:\n";
  for (const auto& dir : kArtifactDirs) manifest << "  - " << dir << "/\n";
  manifest << "required_libraries: []\n";
  manifest << "required_parser_support_udrs: []\n";
  manifest << "supported_parser_modes: []\n";
  manifest << "supported_reference_modes: []\n";
  manifest << "supported_result_renderers: []\n";

  if (!WriteText(options.output_root / "manifests" / "package-manifest.yaml", manifest.str())) {
    std::cerr << "failed to write package manifest\n";
    return 6;
  }

  std::ostringstream readme;
  readme << "# Generated Parser Package Skeleton\n\n";
  readme << "This directory was created by `sb_parser_gen package-skeleton`.\n\n";
  readme << "Package: `" << options.package_name << "`\n\n";
  readme << "Profile: `" << options.profile << "`\n\n";
  readme << "Registry snapshot: `" << snapshot_hash << "`\n";
  if (!WriteText(options.output_root / "README.md", readme.str())) {
    std::cerr << "failed to write README\n";
    return 6;
  }

  std::cout << "sb_parser_gen: package skeleton created at " << options.output_root
            << " (snapshot=" << snapshot_hash << ")\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto options = ParseArgs(argc, argv);
  if (!options) {
    PrintUsage(std::cerr);
    return 5;
  }
  if (options->command == "package-skeleton") {
    return RunPackageSkeleton(*options);
  }
  PrintUsage(std::cerr);
  return 5;
}
