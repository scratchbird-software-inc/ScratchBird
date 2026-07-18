// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Test-only database fixture creation.  Public engine APIs and SBLR must not
// create a database: first-principal creation belongs to the explicit local
// embedded bootstrap command.  Tests that need a pre-existing database use
// this direct storage fixture so they cannot accidentally turn their setup
// path into a public bootstrap capability.

#include "database_lifecycle.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace scratchbird::tests::database_lifecycle {

inline constexpr std::string_view kCredentialedFixturePrincipal =
    "fixture_sysarch";
inline constexpr std::string_view kCredentialedFixtureFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";

inline storage::database::DatabaseLifecycleResult
CreateCredentialedDatabaseFixture(const std::filesystem::path& path,
                                  std::string_view resource_seed_pack_root) {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto database_uuid = core::uuid::GenerateEngineIdentityV7(
      core::platform::UuidKind::database, now);
  const auto filespace_uuid = core::uuid::GenerateEngineIdentityV7(
      core::platform::UuidKind::filespace, now + 1);

  storage::database::DatabaseCreateConfig create;
  create.path = path.string();
  if (database_uuid.ok()) {
    create.database_uuid = database_uuid.value;
  }
  if (filespace_uuid.ok()) {
    create.filespace_uuid = filespace_uuid.value;
  }
  create.creation_unix_epoch_millis = now;
  create.resource_seed_pack_root = std::string(resource_seed_pack_root);
  create.require_resource_seed_pack = !resource_seed_pack_root.empty();
  create.allow_minimal_resource_bootstrap = resource_seed_pack_root.empty();
  create.bootstrap_principal_name = std::string(kCredentialedFixturePrincipal);
  create.bootstrap_credential_fingerprint =
      std::string(kCredentialedFixtureFingerprint);
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.allow_overwrite = false;
  return storage::database::CreateDatabaseFile(create);
}

}  // namespace scratchbird::tests::database_lifecycle
