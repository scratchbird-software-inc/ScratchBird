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
#include <sstream>

namespace scratchbird::parser::registry {
namespace {

std::string Trim(const std::string& value) {
  const std::size_t first = value.find_first_not_of(" \t\r\n`|");
  if (first == std::string::npos) {
    return "";
  }
  const std::size_t last = value.find_last_not_of(" \t\r\n`|");
  return value.substr(first, last - first + 1);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool Contains(const std::string& value, const std::string& needle) {
  return ToLower(value).find(ToLower(needle)) != std::string::npos;
}

std::uint64_t FnvaUpdate(std::uint64_t hash, const std::string& value) {
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
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

std::string ExtractValue(const std::string& line) {
  const std::size_t colon = line.find(':');
  const std::size_t equals = line.find('=');
  std::size_t marker = std::string::npos;
  if (colon != std::string::npos && equals != std::string::npos) {
    marker = std::min(colon, equals);
  } else if (colon != std::string::npos) {
    marker = colon;
  } else if (equals != std::string::npos) {
    marker = equals;
  }

  if (marker == std::string::npos) {
    return "";
  }
  return Trim(line.substr(marker + 1));
}

void ApplyField(RegistrySurfaceEntry* entry, const std::string& line) {
  const std::string lower = ToLower(line);
  const std::string value = ExtractValue(line);
  if (value.empty()) {
    return;
  }

  if (Contains(lower, "surface_key") || Contains(lower, "surface key")) {
    entry->surface_key = value;
  } else if (Contains(lower, "unique_search_key") || Contains(lower, "search_key") || Contains(lower, "search key")) {
    entry->search_key = value;
  } else if (Contains(lower, "operation_key") || Contains(lower, "operation key")) {
    entry->operation_key = value;
  } else if (Contains(lower, "sblr_operation") || Contains(lower, "sblr operation") || Contains(lower, "sblr_opcode")) {
    entry->sblr_operation = value;
  } else if (Contains(lower, "authority_family") || Contains(lower, "authority family")) {
    entry->authority_family = value;
  } else if (Contains(lower, "result_shape") || Contains(lower, "result shape")) {
    entry->result_shape = value;
  } else if (Contains(lower, "diagnostic_shape") || Contains(lower, "diagnostic shape")) {
    entry->diagnostic_shape = value;
  }
}

std::vector<RegistrySurfaceEntry> ExtractEntries(const std::string& source_file, const std::string& content) {
  std::vector<RegistrySurfaceEntry> entries;
  RegistrySurfaceEntry current;
  current.source_file = source_file;

  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string lower = ToLower(line);
    const bool starts_entry = Contains(lower, "surface_key") || Contains(lower, "surface key") || Contains(lower, "unique_search_key");

    if (starts_entry && (!current.surface_key.empty() || !current.search_key.empty() || !current.operation_key.empty())) {
      entries.push_back(current);
      current = RegistrySurfaceEntry{};
      current.source_file = source_file;
    }

    ApplyField(&current, line);
  }

  if (!current.surface_key.empty() || !current.search_key.empty() || !current.operation_key.empty()) {
    entries.push_back(current);
  }

  return entries;
}

}  // namespace

bool RegistrySnapshot::ok() const {
  return std::none_of(diagnostics.begin(), diagnostics.end(), [](const RegistryDiagnostic& diagnostic) {
    return diagnostic.severity == "error";
  });
}

RegistrySnapshot LoadRegistrySnapshot(const RegistrySnapshotRequest& request) {
  RegistrySnapshot snapshot;
  snapshot.profile = request.profile;
  snapshot.snapshot_hash = 1469598103934665603ull;
  snapshot.snapshot_hash = FnvaUpdate(snapshot.snapshot_hash, request.profile);

  if (request.profile.empty()) {
    snapshot.diagnostics.push_back({"SB-REGISTRY-SNAPSHOT-MISSING-PROFILE", "error", "registry snapshot profile is required", ""});
  }

  if (request.files.empty()) {
    snapshot.diagnostics.push_back({"SB-REGISTRY-SNAPSHOT-NO-FILES", "error", "at least one registry file is required", ""});
  }

  for (const auto& file : request.files) {
    snapshot.snapshot_hash = FnvaUpdate(snapshot.snapshot_hash, file.logical_name);
    snapshot.snapshot_hash = FnvaUpdate(snapshot.snapshot_hash, file.path);

    std::string content;
    if (!ReadFile(file.path, &content)) {
      if (file.required) {
        snapshot.diagnostics.push_back({"SB-REGISTRY-SNAPSHOT-MISSING-FILE", "error", "required registry file could not be read", file.path});
      } else {
        snapshot.diagnostics.push_back({"SB-REGISTRY-SNAPSHOT-OPTIONAL-FILE-MISSING", "warning", "optional registry file could not be read", file.path});
      }
      continue;
    }

    snapshot.snapshot_hash = FnvaUpdate(snapshot.snapshot_hash, content);
    std::vector<RegistrySurfaceEntry> entries = ExtractEntries(file.path, content);
    for (const auto& entry : entries) {
      if (entry.search_key.empty()) {
        snapshot.diagnostics.push_back({"SB-REGISTRY-SNAPSHOT-MISSING-SEARCH-KEY", "error", "registry entry is missing a unique search key", file.path});
      }
      if (entry.operation_key.empty()) {
        snapshot.diagnostics.push_back({"SB-REGISTRY-SNAPSHOT-MISSING-OPERATION-KEY", "error", "registry entry is missing an operation key", file.path});
      }
      snapshot.entries.push_back(entry);
    }
  }

  std::vector<std::string> seen_search_keys;
  for (const auto& entry : snapshot.entries) {
    if (entry.search_key.empty()) {
      continue;
    }
    const std::string key = ToLower(entry.search_key);
    if (std::find(seen_search_keys.begin(), seen_search_keys.end(), key) != seen_search_keys.end()) {
      snapshot.diagnostics.push_back({"SB-REGISTRY-SNAPSHOT-DUPLICATE-SEARCH-KEY", "error", "duplicate registry search key: " + entry.search_key, entry.source_file});
    } else {
      seen_search_keys.push_back(key);
    }
  }

  return snapshot;
}

std::string RegistrySnapshotToJson(const RegistrySnapshot& snapshot) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"status\": \"" << (snapshot.ok() ? "pass" : "fail") << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(snapshot.profile) << "\",\n";
  out << "  \"snapshot_hash\": \"" << snapshot.snapshot_hash << "\",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < snapshot.diagnostics.size(); ++i) {
    const auto& diagnostic = snapshot.diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"severity\": \"" << JsonEscape(diagnostic.severity)
        << "\", \"message\": \"" << JsonEscape(diagnostic.message)
        << "\", \"path\": \"" << JsonEscape(diagnostic.path) << "\"}";
    if (i + 1 != snapshot.diagnostics.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"entries\": [\n";
  for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
    const auto& entry = snapshot.entries[i];
    out << "    {\"source_file\": \"" << JsonEscape(entry.source_file)
        << "\", \"surface_key\": \"" << JsonEscape(entry.surface_key)
        << "\", \"search_key\": \"" << JsonEscape(entry.search_key)
        << "\", \"operation_key\": \"" << JsonEscape(entry.operation_key)
        << "\", \"sblr_operation\": \"" << JsonEscape(entry.sblr_operation)
        << "\", \"authority_family\": \"" << JsonEscape(entry.authority_family)
        << "\", \"result_shape\": \"" << JsonEscape(entry.result_shape)
        << "\", \"diagnostic_shape\": \"" << JsonEscape(entry.diagnostic_shape) << "\"}";
    if (i + 1 != snapshot.entries.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::parser::registry
