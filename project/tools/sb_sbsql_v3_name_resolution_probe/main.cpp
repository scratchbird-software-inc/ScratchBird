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

  const auto localized = binding::ResolveNameEvidence("app.sales.Customer", "en-CA", "en-US", "catalog-epoch-1");
  Expect(localized.ok, "localized name should resolve", &errors);
  Expect(binding::IsUuidV7(localized.object_uuid), "localized name should resolve to UUIDv7 evidence", &errors);
  Expect(localized.resolution_language == "en-CA", "explicit language should be retained", &errors);

  const auto fallback = binding::ResolveNameEvidence("app.sales.Customer", "", "en-US", "catalog-epoch-1");
  Expect(fallback.ok, "default-language fallback name should resolve", &errors);
  Expect(fallback.resolution_language == "en-US", "default language should be used when no source language is supplied", &errors);

  const auto explicit_uuid = binding::ResolveNameEvidence("UUID:018f0000-0000-7000-8000-000000000999", "", "en-US", "catalog-epoch-1");
  Expect(explicit_uuid.ok, "explicit UUIDv7 identity should resolve", &errors);
  Expect(explicit_uuid.object_uuid == "018f0000-0000-7000-8000-000000000999", "explicit UUIDv7 identity should be preserved", &errors);

  const auto ambiguous = binding::ResolveNameEvidence("ambiguous.Customer", "en-US", "en-US", "catalog-epoch-1");
  Expect(!ambiguous.ok, "ambiguous name should fail", &errors);
  Expect(ambiguous.diagnostic_code == "SBSQL_BIND_AMBIGUOUS_LOCALIZED_NAME", "ambiguous name diagnostic mismatch", &errors);

  const auto non_v7_uuid = binding::ResolveNameEvidence("UUID:018f0000-0000-4000-8000-000000000999", "", "en-US", "catalog-epoch-1");
  Expect(!non_v7_uuid.ok, "non-v7 identity UUID should fail", &errors);
  Expect(non_v7_uuid.diagnostic_code == "SBSQL_BIND_IDENTITY_UUID_MUST_BE_V7", "non-v7 identity UUID diagnostic mismatch", &errors);

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"resolved_uuid\": \"" << JsonEscape(localized.object_uuid) << "\",\n";
  std::cout << "  \"explicit_uuid\": \"" << JsonEscape(explicit_uuid.object_uuid) << "\",\n";
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
