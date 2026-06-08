// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_binding_catalog.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace binding = scratchbird::parser::sbsql_v3_binding;

namespace {

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}

void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) {
    errors->push_back(std::move(message));
  }
}

}  // namespace

int main() {
  std::vector<std::string> errors;

  const auto int128 = binding::BindDescriptorAlias("int128", "scalar_numeric");
  Expect(int128.ok, "int128 descriptor should bind", &errors);
  Expect(binding::IsUuidV7(int128.descriptor_uuid), "int128 descriptor should bind to UUIDv7 evidence", &errors);
  Expect(int128.canonical_type_name == "int128", "int128 canonical type mismatch", &errors);

  const auto uint128 = binding::BindDescriptorAlias("uint128", "scalar_numeric");
  Expect(uint128.ok, "uint128 descriptor should bind", &errors);
  Expect(uint128.canonical_type_name == "uint128", "uint128 canonical type mismatch", &errors);

  const auto real128 = binding::BindDescriptorAlias("real128", "scalar_numeric");
  Expect(real128.ok, "real128 descriptor should bind", &errors);
  Expect(real128.canonical_type_name == "real128", "real128 canonical type mismatch", &errors);

  const auto missing_context = binding::BindDescriptorAlias("int128", "");
  Expect(!missing_context.ok, "missing descriptor context should fail", &errors);
  Expect(missing_context.diagnostic_code == "SBSQL_BIND_DESCRIPTOR_CONTEXT_REQUIRED", "missing descriptor context diagnostic mismatch", &errors);

  const auto ambiguous = binding::BindDescriptorAlias("ambiguous", "operator");
  Expect(!ambiguous.ok, "ambiguous descriptor should fail", &errors);
  Expect(ambiguous.diagnostic_code == "SBSQL_BIND_AMBIGUOUS_DESCRIPTOR_OVERLOAD", "ambiguous descriptor diagnostic mismatch", &errors);

  const auto unknown = binding::BindDescriptorAlias("unknown_type", "scalar_numeric");
  Expect(!unknown.ok, "unknown descriptor should fail", &errors);
  Expect(unknown.diagnostic_code == "SBSQL_BIND_UNKNOWN_TYPE_ALIAS", "unknown descriptor diagnostic mismatch", &errors);

  std::vector<std::string> profile_errors;
  const auto cluster_profile = binding::BindingProfileForCommandFamily("sbsql.private_cluster");
  Expect(binding::ValidateBindingProfile(cluster_profile, &profile_errors), "cluster binding profile should validate", &errors);
  Expect(cluster_profile.cluster_authority_required, "cluster binding profile should require cluster authority", &errors);
  Expect(cluster_profile.fail_closed_without_cluster_authority, "cluster binding profile should fail closed", &errors);
  for (const auto& error : profile_errors) {
    errors.push_back("profile: " + error);
  }

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"int128_descriptor_uuid\": \"" << JsonEscape(int128.descriptor_uuid) << "\",\n";
  std::cout << "  \"real128_descriptor_uuid\": \"" << JsonEscape(real128.descriptor_uuid) << "\",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) {
      std::cout << ", ";
    }
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";

  return errors.empty() ? 0 : 1;
}
