// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_NODE_MANAGER_SUPPORT_BUNDLE

#pragma once

#include "manager_runtime.hpp"

#include <filesystem>
#include <string>

namespace scratchbird::manager::node {

struct SupportBundleInputs {
  std::filesystem::path bundle_dir;
  std::string scope;
  std::string redaction_profile;
  std::string status_json;
  std::string metrics_json;
  std::filesystem::path audit_file;
  std::filesystem::path metrics_file;
  std::filesystem::path lifecycle_state_file;
  std::filesystem::path lifecycle_journal_file;
  std::string agent_observability_json;
};

bool GenerateManagerSupportBundle(const ManagerConfig& config,
                                  const SupportBundleInputs& inputs,
                                  std::string* error_code);

}  // namespace scratchbird::manager::node
