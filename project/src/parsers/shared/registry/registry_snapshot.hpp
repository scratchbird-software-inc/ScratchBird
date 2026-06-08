// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::parser::registry {

struct RegistryFile {
  std::string logical_name;
  std::string path;
  bool required = true;
};

struct RegistryDiagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::string path;
};

struct RegistrySurfaceEntry {
  std::string source_file;
  std::string surface_key;
  std::string search_key;
  std::string operation_key;
  std::string sblr_operation;
  std::string authority_family;
  std::string result_shape;
  std::string diagnostic_shape;
};

struct RegistrySnapshotRequest {
  std::string profile;
  std::vector<RegistryFile> files;
};

struct RegistrySnapshot {
  std::string profile;
  std::uint64_t snapshot_hash = 0;
  std::vector<RegistryDiagnostic> diagnostics;
  std::vector<RegistrySurfaceEntry> entries;

  bool ok() const;
};

RegistrySnapshot LoadRegistrySnapshot(const RegistrySnapshotRequest& request);
std::string RegistrySnapshotToJson(const RegistrySnapshot& snapshot);

}  // namespace scratchbird::parser::registry
