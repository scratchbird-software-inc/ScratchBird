// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_UUID_IDENTITY_PROBE_MAIN

#include "uuid.hpp"

#include <array>
#include <iostream>
#include <string>

namespace {

using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using namespace scratchbird::core::uuid;

const char* Bool(bool value) { return value ? "true" : "false"; }

bool CheckVersion(const UuidResult& result, unsigned expected) {
  return result.ok() && IsValidUuidVariant(result.value) && UuidVersion(result.value) == expected;
}

}  // namespace

int main() {
  const std::array<byte, 6> node{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  const auto v1 = GenerateCompatibilityTimeNodeV1(0x01b21dd213814000ull, 7, node);
  const auto v2 = GenerateCompatibilityDceSecurityV2(0x01b21dd213814000ull, 7, node, 1, 42);
  const auto v3 = GenerateCompatibilityNameBasedV3(v1.value, "scratchbird");
  const auto v4 = GenerateCompatibilityRandomV4();
  const auto v5 = GenerateCompatibilityNameBasedV5(v1.value, "scratchbird");
  const auto v6 = GenerateCompatibilityReorderedTimeV6(0x01b21dd213814000ull, 7, node);
  const auto v7 = GenerateCompatibilityUnixTimeV7(1700000000000ull);

  const auto row_identity = GenerateEngineIdentityV7(UuidKind::row, 1700000000000ull);
  const auto object_identity = GenerateDurableEngineIdentityV7(UuidKind::object, 1700000000001ull);
  const auto session_identity = GenerateEngineIdentityV7(UuidKind::session, 1700000000002ull);
  const auto typed_v4_row = MakeTypedUuid(UuidKind::row, v4.value);
  const auto durable_v4_row = MakeDurableEngineIdentityUuid(UuidKind::row, v4.value);

  const bool versions_ok = CheckVersion(v1, 1) && CheckVersion(v2, 2) && CheckVersion(v3, 3) &&
                           CheckVersion(v4, 4) && CheckVersion(v5, 5) && CheckVersion(v6, 6) &&
                           CheckVersion(v7, 7);
  const bool identity_ok = row_identity.ok() && object_identity.ok() &&
                           IsEngineIdentityUuid(row_identity.value.value) &&
                           IsEngineIdentityUuid(object_identity.value.value);
  const bool non_v7_rejected = !typed_v4_row.ok() && !durable_v4_row.ok();
  const bool non_durable_kind_rejected = !session_identity.ok() &&
                                         session_identity.diagnostic.diagnostic_code == "SB-UUID-DURABLE-IDENTITY-KIND";
  const bool compatibility_not_identity = !IsEngineIdentityUuid(v4.value) && UuidVersion(v4.value) == 4;
  const bool ok = versions_ok && identity_ok && non_v7_rejected && non_durable_kind_rejected && compatibility_not_identity;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << Bool(ok) << ",\n";
  std::cout << "  \"compatibility_v1_to_v7\": " << Bool(versions_ok) << ",\n";
  std::cout << "  \"durable_v7_identity\": " << Bool(identity_ok) << ",\n";
  std::cout << "  \"non_v7_identity_rejected\": " << Bool(non_v7_rejected) << ",\n";
  std::cout << "  \"non_durable_kind_rejected\": " << Bool(non_durable_kind_rejected) << ",\n";
  std::cout << "  \"compatibility_uuid_not_identity\": " << Bool(compatibility_not_identity) << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
