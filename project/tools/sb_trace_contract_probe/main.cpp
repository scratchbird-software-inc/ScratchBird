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
  std::string evidence;
  std::string contract;
  std::string report;
  std::vector<std::string> required_fields;
  std::vector<std::string> forbidden_terms;
};

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::string field;
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

    if (key == "check") {
      continue;
    } else if (key == "--profile") {
      if (!require_value(&args->profile)) return false;
    } else if (key == "--evidence") {
      if (!require_value(&args->evidence)) return false;
    } else if (key == "--contract") {
      if (!require_value(&args->contract)) return false;
    } else if (key == "--report") {
      if (!require_value(&args->report)) return false;
    } else if (key == "--required-field") {
      std::string value;
      if (!require_value(&value)) return false;
      args->required_fields.push_back(value);
    } else if (key == "--forbidden-term") {
      std::string value;
      if (!require_value(&value)) return false;
      args->forbidden_terms.push_back(value);
    } else {
      *error = "unknown argument: " + key;
      return false;
    }
  }

  if (args->profile.empty() || args->evidence.empty() || args->contract.empty() || args->report.empty()) {
    *error = "--profile, --evidence, --contract, and --report are required";
    return false;
  }

  args->profile = ToLower(args->profile);
  args->contract = ToLower(args->contract);
  return true;
}

void AddDefaultContractFields(Args* args) {
  args->required_fields.push_back("status");
  args->required_fields.push_back("profile");

  if (args->contract == "registry_snapshot") {
    args->required_fields.push_back("snapshot_hash");
    args->required_fields.push_back("entries");
  } else if (args->contract == "command_surface") {
    args->required_fields.push_back("lookup_key");
    args->required_fields.push_back("registry_snapshot_hash");
    args->required_fields.push_back("matches");
  } else if (args->contract == "package_gate") {
    args->required_fields.push_back("manifest");
    args->required_fields.push_back("artifact_list");
    args->required_fields.push_back("snapshot_hash");
    args->required_fields.push_back("artifacts");
  } else if (args->contract == "fixture_manifest") {
    args->required_fields.push_back("fixture_root");
    args->required_fields.push_back("manifest_hash");
    args->required_fields.push_back("fixtures");
  } else if (args->contract == "parse_vertical_slice") {
    args->required_fields.push_back("stage");
    args->required_fields.push_back("input");
    args->required_fields.push_back("engine_api_bridge");
    args->required_fields.push_back("accepted_by_engine_api");
  }

  if (args->profile == "public_node") {
    args->forbidden_terms.push_back("private_cluster_command_authority");
    args->forbidden_terms.push_back("cluster_decision_service");
    args->forbidden_terms.push_back("cluster_epoch_control");
    args->forbidden_terms.push_back("cluster_route_publish");
    args->forbidden_terms.push_back("cluster_recovery_resolution");
    args->forbidden_terms.push_back("project/src/cluster");
    args->forbidden_terms.push_back("sbmc_manager");
    args->forbidden_terms.push_back("trade_secret_detail");
  }
}

bool HasJsonFieldLike(const std::string& evidence, const std::string& field) {
  return Contains(evidence, "\"" + field + "\"") || Contains(evidence, field + ":") || Contains(evidence, field + "=");
}

std::string BuildReport(const Args& args,
                        const std::vector<Diagnostic>& diagnostics,
                        std::uint64_t evidence_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"status\": \"" << (diagnostics.empty() ? "pass" : "fail") << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(args.profile) << "\",\n";
  out << "  \"contract\": \"" << JsonEscape(args.contract) << "\",\n";
  out << "  \"evidence\": \"" << JsonEscape(args.evidence) << "\",\n";
  out << "  \"evidence_hash\": \"" << evidence_hash << "\",\n";
  out << "  \"required_fields\": [";
  for (std::size_t i = 0; i < args.required_fields.size(); ++i) {
    out << "\"" << JsonEscape(args.required_fields[i]) << "\"";
    if (i + 1 != args.required_fields.size()) out << ", ";
  }
  out << "],\n";
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
    std::cerr << "usage: sb_trace_contract_probe check --profile <profile> --evidence <path> --contract <name> --report <path> [--required-field <field>] [--forbidden-term <term>]\n";
    return 5;
  }

  AddDefaultContractFields(&args);

  std::string evidence_text;
  if (!ReadFile(args.evidence, &evidence_text)) {
    std::cerr << "failed to read evidence: " << args.evidence << "\n";
    return 4;
  }

  std::vector<Diagnostic> diagnostics;
  for (const auto& field : args.required_fields) {
    if (!HasJsonFieldLike(evidence_text, field)) {
      diagnostics.push_back({"SB-TRACE-CONTRACT-MISSING-FIELD", "error", "evidence artifact is missing required trace field", field});
    }
  }

  for (const auto& term : args.forbidden_terms) {
    if (Contains(evidence_text, term)) {
      diagnostics.push_back({"SB-TRACE-CONTRACT-FORBIDDEN-TERM", "error", "evidence artifact contains a forbidden term for the selected profile", term});
    }
  }

  std::uint64_t hash = 1469598103934665603ull;
  hash = FnvaUpdate(hash, args.profile);
  hash = FnvaUpdate(hash, args.contract);
  hash = FnvaUpdate(hash, evidence_text);

  std::ofstream out(args.report);
  if (!out) {
    std::cerr << "failed to write report: " << args.report << "\n";
    return 4;
  }
  out << BuildReport(args, diagnostics, hash);

  return diagnostics.empty() ? 0 : 1;
}
