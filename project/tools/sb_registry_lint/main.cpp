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
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string file;
  std::string message;
};

struct CheckedFile {
  std::string path;
  bool present = false;
  bool readable = false;
  bool search_key_present = false;
  std::string hash;
};

struct Options {
  std::string command;
  fs::path spec_root;
  std::string profile;
  fs::path report_path;
  std::string diagnostic_code;
  std::string surface_key;
  fs::path package_manifest;
};

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

const std::vector<std::string> kProfiles = {
    "public_node", "private_cluster", "dev_only", "test_only"};

const std::vector<std::string> kRequiredRegistryFiles = {
    "registries/unified-surface-registry-schema.yaml",
    "registries/sbsql-native-surface-registry.yaml",
    "registries/sbsql-show-command-surface-matrix.yaml",
    "registries/sbsql-management-metrics-cluster-surface-matrix.yaml",
    "registries/donor-unified-surface-normalization-matrix.yaml",
    "registries/parser-ast-boundast-node-registry.yaml",
    "registries/sblr-operation-matrix.yaml",
    "registries/result-shape-registry.yaml",
    "registries/diagnostic-shape-registry.yaml",
    "registries/sbsql-unified-surface-release-gates.yaml",
    "conformance_manifests/sbsql_unified_surface_sblr_matrix.yaml",
    "conformance_manifests/parser_architecture_closure.yaml",
    "trace/sbsql-unified-surface-sblr-matrix-trace.yaml",
    "trace/parser-architecture-closure-trace.yaml",
};

const std::vector<std::string> kForbiddenPlaceholders = {
    "TBD",
    "TODO",
    "implementation-defined",
    "parser-defined",
    "executor-defined",
    "donor-defined",
    "implied",
    "see-code",
};

const std::vector<std::string> kRequiredPackageManifestFields = {
    "manifest_format_version",
    "parser_package_uuid",
    "parser_package_name",
    "parser_package_version",
    "build_uuid",
    "package_kind",
    "edition_scope",
    "registry_snapshot_hash",
    "generated_artifact_inventory",
};

const std::vector<std::string> kPublicProfileForbiddenManifestTerms = {
    "private_cluster_command_authority",
    "cluster_control",
    "cluster_decision_service",
    "cluster_epoch_control",
    "cluster_route_publish",
    "cluster_recovery_resolution",
};

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(ch) << std::dec << std::setfill(' ');
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

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
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool ContainsSearchKey(const std::string& text) {
  return text.find("Search key:") != std::string::npos ||
         text.find("Search key: `") != std::string::npos ||
         text.find("unique_search_key:") != std::string::npos;
}

bool IsSupportedProfile(const std::string& profile) {
  return std::find(kProfiles.begin(), kProfiles.end(), profile) != kProfiles.end();
}

fs::path ResolveSpecRoot(const fs::path& input) {
  if (fs::is_directory(input / "registries")) {
    return input;
  }
  if (fs::is_directory(input / "docs" / "contracts" / "registries")) {
    return input / "docs" / "contracts";
  }
  return input;
}

void AddDiagnostic(std::vector<Diagnostic>& diagnostics,
                   std::string code,
                   std::string severity,
                   std::string file,
                   std::string message) {
  diagnostics.push_back({std::move(code), std::move(severity), std::move(file),
                         std::move(message)});
}

std::map<std::string, std::string> BuildDiagnosticExplanations() {
  return {
      {"SBRLINT_FILE_MISSING", "Required registry or contract input is missing."},
      {"SBRLINT_SEARCH_KEY_MISSING", "Checked file does not contain Search key or unique_search_key evidence."},
      {"SBRLINT_REQUIRED_FIELD_MISSING", "Required registry row or package manifest field is missing."},
      {"SBRLINT_FORBIDDEN_PLACEHOLDER", "Checked file contains a forbidden placeholder authority term."},
      {"SBRLINT_PRIVATE_PUBLIC_LEAKAGE", "Public package profile contains private authority material."},
      {"SBRLINT_INVALID_ARGUMENT", "Command line arguments are invalid or incomplete."},
      {"SBRLINT_UNSUPPORTED_PROFILE", "Profile is not one of public_node, private_cluster, dev_only, or test_only."},
  };
}

void PrintUsage(std::ostream& out) {
  out << "Usage:\n"
      << "  sb_registry_lint check --spec-root <path> --profile <profile> --report <path>\n"
      << "  sb_registry_lint check-command --spec-root <path> --surface-key <key> --profile <profile> --report <path>\n"
      << "  sb_registry_lint check-package --spec-root <path> --package-manifest <path> --profile <profile> --report <path>\n"
      << "  sb_registry_lint explain --diagnostic <code>\n";
}

std::optional<Options> ParseArgs(int argc, char** argv, std::vector<Diagnostic>& diagnostics) {
  if (argc < 2) {
    AddDiagnostic(diagnostics, "SBRLINT_INVALID_ARGUMENT", "error", "", "missing command");
    return std::nullopt;
  }

  Options options;
  options.command = argv[1];

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& name) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        AddDiagnostic(diagnostics, "SBRLINT_INVALID_ARGUMENT", "error", "", "missing value for " + name);
        return std::nullopt;
      }
      ++i;
      return std::string(argv[i]);
    };

    if (arg == "--spec-root") {
      auto value = require_value(arg);
      if (!value) return std::nullopt;
      options.spec_root = *value;
    } else if (arg == "--profile") {
      auto value = require_value(arg);
      if (!value) return std::nullopt;
      options.profile = *value;
    } else if (arg == "--report") {
      auto value = require_value(arg);
      if (!value) return std::nullopt;
      options.report_path = *value;
    } else if (arg == "--diagnostic") {
      auto value = require_value(arg);
      if (!value) return std::nullopt;
      options.diagnostic_code = *value;
    } else if (arg == "--surface-key") {
      auto value = require_value(arg);
      if (!value) return std::nullopt;
      options.surface_key = *value;
    } else if (arg == "--package-manifest") {
      auto value = require_value(arg);
      if (!value) return std::nullopt;
      options.package_manifest = *value;
    } else {
      AddDiagnostic(diagnostics, "SBRLINT_INVALID_ARGUMENT", "error", "", "unknown argument " + arg);
      return std::nullopt;
    }
  }

  return options;
}

std::vector<CheckedFile> CheckFiles(const fs::path& spec_root,
                                    std::vector<Diagnostic>& diagnostics,
                                    std::uint64_t& snapshot_hash) {
  std::vector<CheckedFile> checked;

  for (const auto& relative : kRequiredRegistryFiles) {
    CheckedFile file;
    file.path = relative;
    const fs::path full_path = spec_root / relative;
    file.present = fs::exists(full_path);

    HashBytes(snapshot_hash, relative);
    HashBytes(snapshot_hash, "\n");

    if (!file.present) {
      AddDiagnostic(diagnostics, "SBRLINT_FILE_MISSING", "error", relative,
                    "required file is missing");
      checked.push_back(file);
      continue;
    }

    auto text = ReadFile(full_path);
    if (!text) {
      AddDiagnostic(diagnostics, "SBRLINT_FILE_MISSING", "error", relative,
                    "required file is not readable");
      checked.push_back(file);
      continue;
    }

    file.readable = true;
    std::uint64_t file_hash = kFnvOffset;
    HashBytes(file_hash, *text);
    file.hash = HexHash(file_hash);
    HashBytes(snapshot_hash, *text);

    file.search_key_present = ContainsSearchKey(*text);
    if (!file.search_key_present) {
      AddDiagnostic(diagnostics, "SBRLINT_SEARCH_KEY_MISSING", "error", relative,
                    "file lacks Search key or unique_search_key evidence");
    }

    for (const auto& placeholder : kForbiddenPlaceholders) {
      if (text->find(placeholder) != std::string::npos) {
        AddDiagnostic(diagnostics, "SBRLINT_FORBIDDEN_PLACEHOLDER", "error", relative,
                      "forbidden placeholder term found: " + placeholder);
      }
    }

    checked.push_back(file);
  }

  std::sort(checked.begin(), checked.end(), [](const auto& left, const auto& right) {
    return left.path < right.path;
  });
  return checked;
}

void ValidatePackageManifest(const fs::path& manifest_path,
                             const std::string& manifest_text,
                             const std::string& profile,
                             std::vector<Diagnostic>& diagnostics) {
  for (const auto& field : kRequiredPackageManifestFields) {
    if (manifest_text.find(field) == std::string::npos) {
      AddDiagnostic(diagnostics, "SBRLINT_REQUIRED_FIELD_MISSING", "error",
                    manifest_path.string(), "package manifest missing required field: " + field);
    }
  }

  if (manifest_text.find("edition_scope") != std::string::npos &&
      manifest_text.find(profile) == std::string::npos) {
    AddDiagnostic(diagnostics, "SBRLINT_REQUIRED_FIELD_MISSING", "error",
                  manifest_path.string(), "package manifest edition_scope does not match selected profile: " + profile);
  }

  if (profile == "public_node") {
    for (const auto& forbidden : kPublicProfileForbiddenManifestTerms) {
      if (manifest_text.find(forbidden) != std::string::npos) {
        AddDiagnostic(diagnostics, "SBRLINT_PRIVATE_PUBLIC_LEAKAGE", "error",
                      manifest_path.string(), "public profile manifest contains private authority term: " + forbidden);
      }
    }
  }
}

void SortDiagnostics(std::vector<Diagnostic>& diagnostics) {
  std::sort(diagnostics.begin(), diagnostics.end(), [](const auto& left, const auto& right) {
    return std::tie(left.file, left.code, left.message) <
           std::tie(right.file, right.code, right.message);
  });
}

bool WriteReport(const fs::path& report_path,
                 const Options& options,
                 const fs::path& resolved_spec_root,
                 const std::string& status,
                 const std::string& snapshot_hash,
                 const std::vector<CheckedFile>& checked_files,
                 const std::vector<Diagnostic>& diagnostics) {
  if (!report_path.parent_path().empty()) {
    std::error_code ec;
    fs::create_directories(report_path.parent_path(), ec);
    if (ec) {
      return false;
    }
  }

  std::ofstream out(report_path, std::ios::binary);
  if (!out) {
    return false;
  }

  out << "{\n";
  out << "  \"report_version\": 1,\n";
  out << "  \"command\": \"" << JsonEscape(options.command) << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(options.profile) << "\",\n";
  out << "  \"spec_root\": \"" << JsonEscape(resolved_spec_root.string()) << "\",\n";
  out << "  \"status\": \"" << JsonEscape(status) << "\",\n";
  out << "  \"registry_snapshot_hash\": \"" << JsonEscape(snapshot_hash) << "\",\n";
  out << "  \"summary\": {\n";
  out << "    \"checked_file_count\": " << checked_files.size() << ",\n";
  out << "    \"diagnostic_count\": " << diagnostics.size() << "\n";
  out << "  },\n";
  out << "  \"checked_files\": [\n";
  for (std::size_t i = 0; i < checked_files.size(); ++i) {
    const auto& file = checked_files[i];
    out << "    {\"path\": \"" << JsonEscape(file.path) << "\", "
        << "\"present\": " << (file.present ? "true" : "false") << ", "
        << "\"readable\": " << (file.readable ? "true" : "false") << ", "
        << "\"search_key_present\": " << (file.search_key_present ? "true" : "false") << ", "
        << "\"hash\": \"" << JsonEscape(file.hash) << "\"}";
    if (i + 1 != checked_files.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const auto& diag = diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diag.code) << "\", "
        << "\"severity\": \"" << JsonEscape(diag.severity) << "\", "
        << "\"file\": \"" << JsonEscape(diag.file) << "\", "
        << "\"message\": \"" << JsonEscape(diag.message) << "\"}";
    if (i + 1 != diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return static_cast<bool>(out);
}

int RunExplain(const Options& options) {
  const auto explanations = BuildDiagnosticExplanations();
  auto found = explanations.find(options.diagnostic_code);
  if (found == explanations.end()) {
    std::cerr << "unknown diagnostic: " << options.diagnostic_code << "\n";
    return 1;
  }
  std::cout << options.diagnostic_code << ": " << found->second << "\n";
  return 0;
}

int RunCheck(Options options) {
  std::vector<Diagnostic> diagnostics;

  if (options.spec_root.empty()) {
    AddDiagnostic(diagnostics, "SBRLINT_INVALID_ARGUMENT", "error", "", "--spec-root is required");
  }
  if (options.profile.empty()) {
    AddDiagnostic(diagnostics, "SBRLINT_INVALID_ARGUMENT", "error", "", "--profile is required");
  } else if (!IsSupportedProfile(options.profile)) {
    AddDiagnostic(diagnostics, "SBRLINT_UNSUPPORTED_PROFILE", "error", "", "unsupported profile: " + options.profile);
  }
  if (options.report_path.empty()) {
    AddDiagnostic(diagnostics, "SBRLINT_INVALID_ARGUMENT", "error", "", "--report is required");
  }
  if (options.command == "check-command" && options.surface_key.empty()) {
    AddDiagnostic(diagnostics, "SBRLINT_INVALID_ARGUMENT", "error", "", "--surface-key is required for check-command");
  }
  if (options.command == "check-package" && options.package_manifest.empty()) {
    AddDiagnostic(diagnostics, "SBRLINT_INVALID_ARGUMENT", "error", "", "--package-manifest is required for check-package");
  }

  fs::path resolved_spec_root;
  std::vector<CheckedFile> checked_files;
  std::uint64_t snapshot_hash_value = kFnvOffset;

  if (diagnostics.empty()) {
    resolved_spec_root = ResolveSpecRoot(options.spec_root);
    if (!fs::is_directory(resolved_spec_root)) {
      AddDiagnostic(diagnostics, "SBRLINT_FILE_MISSING", "error", resolved_spec_root.string(),
                    "contract root is missing or not a directory");
    } else {
      HashBytes(snapshot_hash_value, resolved_spec_root.generic_string());
      checked_files = CheckFiles(resolved_spec_root, diagnostics, snapshot_hash_value);

      if (options.command == "check-command") {
        bool found_surface = false;
        for (const auto& file : checked_files) {
          if (!file.present || !file.readable) continue;
          auto text = ReadFile(resolved_spec_root / file.path);
          if (text && text->find(options.surface_key) != std::string::npos) {
            found_surface = true;
            break;
          }
        }
        if (!found_surface) {
          AddDiagnostic(diagnostics, "SBRLINT_REQUIRED_FIELD_MISSING", "error", options.surface_key,
                        "surface key was not found in checked registry files");
        }
      }

      if (options.command == "check-package") {
        if (!fs::exists(options.package_manifest)) {
          AddDiagnostic(diagnostics, "SBRLINT_FILE_MISSING", "error", options.package_manifest.string(),
                        "package manifest is missing");
        } else {
          auto manifest_text = ReadFile(options.package_manifest);
          if (!manifest_text) {
            AddDiagnostic(diagnostics, "SBRLINT_FILE_MISSING", "error", options.package_manifest.string(),
                          "package manifest is not readable");
          } else {
            ValidatePackageManifest(options.package_manifest, *manifest_text, options.profile, diagnostics);
          }
        }
      }
    }
  }

  SortDiagnostics(diagnostics);
  const std::string status = diagnostics.empty() ? "pass" : "fail";
  const std::string snapshot_hash = HexHash(snapshot_hash_value);

  if (!options.report_path.empty()) {
    if (!WriteReport(options.report_path, options, resolved_spec_root, status, snapshot_hash,
                     checked_files, diagnostics)) {
      std::cerr << "failed to write report: " << options.report_path << "\n";
      return 4;
    }
  }

  std::cout << "sb_registry_lint: " << status << " (diagnostics=" << diagnostics.size()
            << ", snapshot=" << snapshot_hash << ")\n";

  if (diagnostics.empty()) {
    return 0;
  }
  const bool has_invalid_args = std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diag) {
    return diag.code == "SBRLINT_INVALID_ARGUMENT" || diag.code == "SBRLINT_UNSUPPORTED_PROFILE";
  });
  if (has_invalid_args) {
    return 5;
  }
  const bool has_missing_file = std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diag) {
    return diag.code == "SBRLINT_FILE_MISSING";
  });
  if (has_missing_file) {
    return 4;
  }
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<Diagnostic> parse_diagnostics;
  auto options = ParseArgs(argc, argv, parse_diagnostics);
  if (!options) {
    PrintUsage(std::cerr);
    return 5;
  }

  if (options->command == "explain") {
    if (options->diagnostic_code.empty()) {
      std::cerr << "--diagnostic is required for explain\n";
      return 5;
    }
    return RunExplain(*options);
  }

  if (options->command == "check" || options->command == "check-command" ||
      options->command == "check-package") {
    return RunCheck(*options);
  }

  std::cerr << "unknown command: " << options->command << "\n";
  PrintUsage(std::cerr);
  return 5;
}
