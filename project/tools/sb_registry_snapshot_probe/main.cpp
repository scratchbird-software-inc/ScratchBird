// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "registry_snapshot.hpp"

#include <fstream>
#include <iostream>
#include <string>

namespace {

struct Args {
  std::string profile;
  std::string report;
  scratchbird::parser::registry::RegistrySnapshotRequest request;
};

void PrintUsage() {
  std::cerr
      << "usage: sb_registry_snapshot_probe --profile <profile> "
      << "--registry <logical-name=path> [--optional-registry <logical-name=path>] "
      << "--report <path>\n";
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

  if (args->profile.empty() || args->report.empty() || args->request.files.empty()) {
    *error = "--profile, at least one --registry, and --report are required";
    return false;
  }

  args->request.profile = args->profile;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string error;
  if (!ParseArgs(argc, argv, &args, &error)) {
    std::cerr << error << "\n";
    PrintUsage();
    return 5;
  }

  scratchbird::parser::registry::RegistrySnapshot snapshot =
      scratchbird::parser::registry::LoadRegistrySnapshot(args.request);

  std::ofstream out(args.report);
  if (!out) {
    std::cerr << "failed to write report: " << args.report << "\n";
    return 4;
  }
  out << scratchbird::parser::registry::RegistrySnapshotToJson(snapshot);

  return snapshot.ok() ? 0 : 1;
}
