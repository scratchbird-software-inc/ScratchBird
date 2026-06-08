// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "registry_snapshot.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Args {
  std::string profile;
  std::string lookup_key;
  std::string report;
  scratchbird::parser::registry::RegistrySnapshotRequest request;
};

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
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

bool SplitRegistrySpec(const std::string& spec,
                       bool required,
                       scratchbird::parser::registry::RegistryFile* file,
                       std::string* error) {
  const std::size_t marker = spec.find('=');
  if (marker == std::string::npos || marker == 0 || marker + 1 >= spec.size()) {
    *error = "registry argument must use logical-name=path: " + spec;
    return false;
  }
  file->logical_name = spec.substr(0, marker);
  file->path = spec.substr(marker + 1);
  file->required = required;
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
    } else if (key == "--lookup-key") {
      if (!require_value(&args->lookup_key)) return false;
    } else if (key == "--report") {
      if (!require_value(&args->report)) return false;
    } else if (key == "--registry" || key == "--optional-registry") {
      std::string value;
      if (!require_value(&value)) return false;
      scratchbird::parser::registry::RegistryFile file;
      if (!SplitRegistrySpec(value, key == "--registry", &file, error)) {
        return false;
      }
      args->request.files.push_back(file);
    } else {
      *error = "unknown argument: " + key;
      return false;
    }
  }

  if (args->profile.empty() || args->lookup_key.empty() || args->report.empty() || args->request.files.empty()) {
    *error = "--profile, --lookup-key, at least one --registry, and --report are required";
    return false;
  }
  args->request.profile = args->profile;
  return true;
}

bool EntryMatches(const scratchbird::parser::registry::RegistrySurfaceEntry& entry,
                  const std::string& normalized_key) {
  return ToLower(entry.search_key) == normalized_key ||
         ToLower(entry.surface_key) == normalized_key ||
         ToLower(entry.operation_key) == normalized_key;
}

std::string BuildReport(const Args& args,
                        const scratchbird::parser::registry::RegistrySnapshot& snapshot,
                        const std::vector<scratchbird::parser::registry::RegistrySurfaceEntry>& matches) {
  const bool snapshot_ok = snapshot.ok();
  const bool unique_match = matches.size() == 1;
  std::ostringstream out;
  out << "{\n";
  out << "  \"status\": \"" << ((snapshot_ok && unique_match) ? "pass" : "fail") << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(args.profile) << "\",\n";
  out << "  \"lookup_key\": \"" << JsonEscape(args.lookup_key) << "\",\n";
  out << "  \"registry_snapshot_hash\": \"" << snapshot.snapshot_hash << "\",\n";
  out << "  \"match_count\": " << matches.size() << ",\n";
  out << "  \"diagnostics\": [\n";
  bool wrote = false;
  for (const auto& diagnostic : snapshot.diagnostics) {
    if (wrote) out << ",\n";
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"severity\": \"" << JsonEscape(diagnostic.severity)
        << "\", \"message\": \"" << JsonEscape(diagnostic.message)
        << "\", \"path\": \"" << JsonEscape(diagnostic.path) << "\"}";
    wrote = true;
  }
  if (matches.empty()) {
    if (wrote) out << ",\n";
    out << "    {\"code\": \"SB-COMMAND-SURFACE-NOT-FOUND\", \"severity\": \"error\", \"message\": \"lookup key did not resolve to a registry command surface\", \"path\": \"\"}";
    wrote = true;
  } else if (matches.size() > 1) {
    if (wrote) out << ",\n";
    out << "    {\"code\": \"SB-COMMAND-SURFACE-AMBIGUOUS\", \"severity\": \"error\", \"message\": \"lookup key resolved to more than one registry command surface\", \"path\": \"\"}";
    wrote = true;
  }
  out << "\n  ],\n";
  out << "  \"matches\": [\n";
  for (std::size_t i = 0; i < matches.size(); ++i) {
    const auto& entry = matches[i];
    out << "    {\"source_file\": \"" << JsonEscape(entry.source_file)
        << "\", \"surface_key\": \"" << JsonEscape(entry.surface_key)
        << "\", \"search_key\": \"" << JsonEscape(entry.search_key)
        << "\", \"operation_key\": \"" << JsonEscape(entry.operation_key)
        << "\", \"sblr_operation\": \"" << JsonEscape(entry.sblr_operation)
        << "\", \"authority_family\": \"" << JsonEscape(entry.authority_family)
        << "\", \"result_shape\": \"" << JsonEscape(entry.result_shape)
        << "\", \"diagnostic_shape\": \"" << JsonEscape(entry.diagnostic_shape) << "\"}";
    if (i + 1 != matches.size()) out << ",";
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
    std::cerr << "usage: sb_command_surface_probe --profile <profile> --lookup-key <key> --registry <logical-name=path> --report <path>\n";
    return 5;
  }

  scratchbird::parser::registry::RegistrySnapshot snapshot =
      scratchbird::parser::registry::LoadRegistrySnapshot(args.request);

  const std::string normalized_key = ToLower(args.lookup_key);
  std::vector<scratchbird::parser::registry::RegistrySurfaceEntry> matches;
  for (const auto& entry : snapshot.entries) {
    if (EntryMatches(entry, normalized_key)) {
      matches.push_back(entry);
    }
  }

  std::ofstream out(args.report);
  if (!out) {
    std::cerr << "failed to write report: " << args.report << "\n";
    return 4;
  }
  out << BuildReport(args, snapshot, matches);

  return snapshot.ok() && matches.size() == 1 ? 0 : 1;
}
