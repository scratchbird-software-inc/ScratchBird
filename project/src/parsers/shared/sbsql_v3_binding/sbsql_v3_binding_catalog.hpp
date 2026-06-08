// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql_v3_binding {

struct BoundNameReference {
  bool ok = false;
  std::string object_uuid;
  std::string object_kind;
  std::string resolution_language;
  std::string source_name;
  std::string catalog_epoch;
  std::string diagnostic_code;
};

struct BoundDescriptorReference {
  bool ok = false;
  std::string descriptor_uuid;
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_descriptor;
  std::string source_alias;
  std::string diagnostic_code;
};

struct BindingProfile {
  std::string command_family;
  std::string required_right;
  std::string scope_mode;
  std::string catalog_authority;
  std::string descriptor_binding_profile;
  bool engine_security_recheck_required = true;
  bool engine_transaction_recheck_required = true;
  bool cluster_authority_required = false;
  bool fail_closed_without_cluster_authority = false;
};

bool IsUuidV7(std::string_view uuid_text);
BindingProfile BindingProfileForCommandFamily(std::string_view command_family);
BoundNameReference ResolveNameEvidence(std::string_view source_name,
                                       std::string_view language,
                                       std::string_view default_language,
                                       std::string_view catalog_epoch);
BoundDescriptorReference BindDescriptorAlias(std::string_view alias,
                                             std::string_view descriptor_context);
bool ValidateBindingProfile(const BindingProfile& profile, std::vector<std::string>* errors);
std::string SerializeBoundNameReferenceToJson(const BoundNameReference& ref);
std::string SerializeBoundDescriptorReferenceToJson(const BoundDescriptorReference& ref);

}  // namespace scratchbird::parser::sbsql_v3_binding
