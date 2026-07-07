// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sb_udr_runtime.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace scratchbird::udr::builtin_packages {

// SEARCH_KEY: SB_UDR_BUILTIN_PACKAGE_CATALOG
struct BuiltinUdrPackageSpec {
  std::string_view family_id;
  std::string_view category;
  std::string_view package_uuid;
  std::string_view package_name;
  std::string_view capability_role;
  std::string_view release_policy;
  std::string_view diagnostic_code;
  bool external_io{false};
  bool side_effects{false};
  bool security_definer{false};
  bool cluster_scoped{false};
};

struct BuiltinUdrPackageDeploymentManifestRow {
  std::string package_uuid;
  std::string package_name;
  std::string family_id;
  std::string category;
  std::string abi_version;
  std::string runtime_language;
  std::string source_revision;
  std::string binary_hash;
  std::string signature_policy;
  std::string capability_role;
  std::string release_policy;
  std::string diagnostic_code;
  std::string entrypoints_csv;
  std::string install_component;
  std::string public_abi_status;
};

std::span<const BuiltinUdrPackageSpec> BuiltinUdrPackageSpecs();
const BuiltinUdrPackageSpec* FindBuiltinUdrPackageSpec(std::string_view package_uuid);
runtime::UdrPackageDescriptor BuiltinUdrPackageDescriptor(
    const BuiltinUdrPackageSpec& spec);
std::vector<runtime::UdrPackageDescriptor> BuiltinUdrPackageDescriptors();
std::vector<BuiltinUdrPackageDeploymentManifestRow>
BuiltinUdrPackageDeploymentManifest();
std::string BuiltinUdrPackageDeploymentManifestJson();

}  // namespace scratchbird::udr::builtin_packages
