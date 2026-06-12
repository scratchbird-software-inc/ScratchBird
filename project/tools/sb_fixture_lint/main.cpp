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

struct Options {
  fs::path fixture_path;
  fs::path report_path;
  std::string profile;
};

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

const std::vector<std::string> kProfiles = {
    "public_node", "private_cluster", "dev_only", "test_only"};

const std::vector<std::string> kRequiredFixtureFields = {
    "fixture_id",
    "fixture_version",
    "surface_key",
    "row_kind",
    "edition_scope",
    "parser_mode",
    "reference_mode",
    "input",
    "expected_parse",
    "expected_bind",
    "expected_lower",
    "expected_result_shape",
    "expected_diagnostic_shape",
    "expected_trace",
    "redaction_class",
    "package_filter_expectation",
    "evidence_refs",
};

const std::vector<std::string> kAcceptedFixtureRequiredFields = {
    "expected_parse",
    "expected_bind",
    "expected_lower",
    "expected_engine_api_bridge",
    "expected_result_shape",
};

const std::vector<std::string> kRefusalFixtureRequiredFields = {
    "expected_diagnostic_shape",
    "redaction_class",
};

const std::vector<std::string> kPublicForbiddenTerms = {
    "private_cluster_command_authority",
    "cluster_decision_service",
    "cluster_epoch_control",
    "cluster_route_publish",
    "cluster_recovery_resolution",
    "trade_secret_detail",
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
  if (!in) return std::nullopt;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool ContainsField(const std::string& text, const std::string& field) {
  return text.find(field + ":") != std::string::npos;
}

bool ContainsAnyRowKind(const std::string& text, std::initializer_list<std::string_view> kinds) {
  for (const auto kind : kinds) {
    if (text.find("row_kind: " + std::string(kind)) != std::string::npos ||
        text.find("row_kind: \"" + std::string(kind) + "\"") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool IsSupportedProfile(const std::string& profile) {
  return std::find(kProfiles.begin(), kProfiles.end(), profile) != kProfiles.end();
}

void AddDiagnostic(std::vector<Diagnostic>& diagnostics,
                   std::string code,
                   std::string severity,
                   std::string file,
                   std::string message) {
  diagnostics.push_back({std::move(code), std::move(severity), std::move(file),
                         std::move(message)});
}

void SortDiagnostics(std::vector<Diagnostic>& diagnostics) {
  std::sort(diagnostics.begin(), diagnostics.end(), [](const auto& left, const auto& right) {
    return std::tie(left.file, left.code, left.message) <
           std::tie(right.file, right.code, right.message);
  });
}

void PrintUsage(std::ostream& out) {
  out << "Usage:\n"
      << "  sb_fixture_lint check --fixture <path> --profile <profile> --report <path>\n";
}

std::optional<Options> ParseArgs(int argc, char** argv, std::vector<Diagnostic>& diagnostics) {
  if (argc < 2 || std::string(argv[1]) != "check") {
    AddDiagnostic(diagnostics, "SBFIX_INVALID_ARGUMENT", "error", "", "expected check command");
    return std::nullopt;
  }

  Options options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) return std::nullopt;
      ++i;
      return std::string(argv[i]);
    };

    if (arg == "--fixture") {
      auto v = value(); if (!v) return std::nullopt; options.fixture_path = *v;
    } else if (arg == "--profile") {
      auto v = value(); if (!v) return std::nullopt; options.profile = *v;
    } else if (arg == "--report") {
      auto v = value(); if (!v) return std::nullopt; options.report_path = *v;
    } else {
      AddDiagnostic(diagnostics, "SBFIX_INVALID_ARGUMENT", "error", "", "unknown argument " + arg);
      return std::nullopt;
    }
  }
  return options;
}

void ValidateFixture(const Options& options, const std::string& text,
                     std::vector<Diagnostic>& diagnostics) {
  const std::string file = options.fixture_path.string();

  for (const auto& field : kRequiredFixtureFields) {
    if (!ContainsField(text, field)) {
      AddDiagnostic(diagnostics, "SBFIX_REQUIRED_FIELD_MISSING", "error", file,
                    "fixture missing required field: " + field);
    }
  }

  const bool accepted = ContainsAnyRowKind(text, {"accepted_command", "accepted", "alias_command", "alias"});
  const bool refusal = ContainsAnyRowKind(text, {"refusal_command", "refusal"});

  if (!accepted && !refusal) {
    AddDiagnostic(diagnostics, "SBFIX_ROW_KIND_INVALID", "error", file,
                  "fixture row_kind must be accepted/alias/refusal");
  }

  if (accepted) {
    const bool package_manifest_fixture = text.find("fixture_class: package_manifest") != std::string::npos ||
                                          text.find("/package_manifest/") != std::string::npos;
    if (!package_manifest_fixture) {
      for (const auto& field : kAcceptedFixtureRequiredFields) {
        if (!ContainsField(text, field)) {
          AddDiagnostic(diagnostics, "SBFIX_REQUIRED_FIELD_MISSING", "error", file,
                        "accepted fixture missing field: " + field);
        }
      }
    }
  }

  if (refusal) {
    for (const auto& field : kRefusalFixtureRequiredFields) {
      if (!ContainsField(text, field)) {
        AddDiagnostic(diagnostics, "SBFIX_REQUIRED_FIELD_MISSING", "error", file,
                      "refusal fixture missing field: " + field);
      }
    }
    if (text.find("expected_lower:") != std::string::npos &&
        text.find("operation_key") != std::string::npos) {
      AddDiagnostic(diagnostics, "SBFIX_REFUSAL_EMITS_OPERATION", "error", file,
                    "refusal fixture must not expect an executable operation key");
    }
  }

  if (options.profile == "public_node") {
    for (const auto& term : kPublicForbiddenTerms) {
      if (text.find(term) != std::string::npos) {
        AddDiagnostic(diagnostics, "SBFIX_PRIVATE_PUBLIC_LEAKAGE", "error", file,
                      "public fixture contains private authority term: " + term);
      }
    }
  }
}

bool WriteReport(const Options& options, const std::string& status,
                 const std::string& fixture_hash,
                 const std::vector<Diagnostic>& diagnostics) {
  if (!options.report_path.parent_path().empty()) {
    std::error_code ec;
    fs::create_directories(options.report_path.parent_path(), ec);
    if (ec) return false;
  }

  std::ofstream out(options.report_path, std::ios::binary);
  if (!out) return false;

  out << "{\n";
  out << "  \"report_version\": 1,\n";
  out << "  \"fixture\": \"" << JsonEscape(options.fixture_path.string()) << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(options.profile) << "\",\n";
  out << "  \"status\": \"" << JsonEscape(status) << "\",\n";
  out << "  \"fixture_hash\": \"" << JsonEscape(fixture_hash) << "\",\n";
  out << "  \"diagnostic_count\": " << diagnostics.size() << ",\n";
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

}  // namespace

int main(int argc, char** argv) {
  std::vector<Diagnostic> diagnostics;
  auto options = ParseArgs(argc, argv, diagnostics);
  if (!options || options->fixture_path.empty() || options->profile.empty() || options->report_path.empty()) {
    PrintUsage(std::cerr);
    return 5;
  }

  if (!IsSupportedProfile(options->profile)) {
    AddDiagnostic(diagnostics, "SBFIX_UNSUPPORTED_PROFILE", "error", "", "unsupported profile: " + options->profile);
  }

  std::string fixture_hash = "0000000000000000";
  auto text = ReadFile(options->fixture_path);
  if (!text) {
    AddDiagnostic(diagnostics, "SBFIX_FILE_MISSING", "error", options->fixture_path.string(),
                  "fixture is missing or not readable");
  } else {
    std::uint64_t hash = kFnvOffset;
    HashBytes(hash, *text);
    fixture_hash = HexHash(hash);
    ValidateFixture(*options, *text, diagnostics);
  }

  SortDiagnostics(diagnostics);
  const std::string status = diagnostics.empty() ? "pass" : "fail";

  if (!WriteReport(*options, status, fixture_hash, diagnostics)) {
    std::cerr << "failed to write fixture report: " << options->report_path << "\n";
    return 4;
  }

  std::cout << "sb_fixture_lint: " << status << " (diagnostics=" << diagnostics.size()
            << ", fixture_hash=" << fixture_hash << ")\n";
  if (diagnostics.empty()) return 0;
  const bool input_problem = std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diag) {
    return diag.code == "SBFIX_FILE_MISSING";
  });
  if (input_problem) return 4;
  const bool argument_problem = std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diag) {
    return diag.code == "SBFIX_INVALID_ARGUMENT" || diag.code == "SBFIX_UNSUPPORTED_PROFILE";
  });
  if (argument_problem) return 5;
  return 1;
}
