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
// this fixture so they cannot accidentally turn their setup path into a public
// bootstrap capability.
//
// SEARCH_KEY: SB_NATIVE_CREATE_TEST_FIXTURE (owner Q256 = 1, 2026-11-16 legacy
// cut). This fixture now creates the NATIVE single-file database through the
// production native create (tests/support/native_database_create_fixture.hpp).
// The signature is kept so its 14 includers compile; the result is a
// DatabaseLifecycleResult carrying the native receipt's database and filespace
// identities and the bootstrap principal. What the native receipt does NOT
// carry, and this fixture no longer fakes: a sysarch role UUID, a membership
// UUID, a policy generation, the legacy header, the "handoff" states. The
// `resource_seed_pack_root` argument has no native counterpart (the native
// create takes the server's policy pack; resource seeding is not part of
// create) and is ignored. Includers that then read the legacy security catalog
// or open the file through the legacy open fail at runtime: expected, owner
// 2026-11-16 ("the legacy support is dropped").
// (Was: CreateDatabaseFile with the legacy SBDBV001 format.)

#include "database_lifecycle.hpp"
#include "uuid.hpp"
#include "../support/native_database_create_fixture.hpp"

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
  (void)resource_seed_pack_root;  // no native counterpart; see the header comment
  scratchbird::tests::NativeFixtureCreateRequest request;
  request.path = path;
  request.database_name = "credentialed_fixture";
  request.bootstrap_principal_name = std::string(kCredentialedFixturePrincipal);
  request.bootstrap_credential_fingerprint = std::string(kCredentialedFixtureFingerprint);
  const auto made = scratchbird::tests::CreateNativeFixtureDatabase(request);
  storage::database::DatabaseLifecycleResult result;
  result.state.path = path.string();
  if (!made.ok) {
    // The storage lifecycle's own failing status shape (database_lifecycle.cpp:288).
    result.status = {core::platform::StatusCode::platform_required_feature_missing,
                     core::platform::Severity::error, core::platform::Subsystem::storage_disk};
    result.diagnostic.status = result.status;
    result.diagnostic.diagnostic_code = made.diagnostic_code;
    result.diagnostic.message_key = made.diagnostic_detail;
    return result;
  }
  result.state.database_uuid = {core::platform::UuidKind::database, made.database_uuid};
  result.state.filespace_uuid = {core::platform::UuidKind::filespace, made.filespace_uuid};
  result.state.header.database_uuid = made.database_uuid;
  result.state.header.page_size = request.page_size;
  result.bootstrap_principal_uuid = {core::platform::UuidKind::principal, made.bootstrap_principal_uuid};
  result.internal_service_principal_uuid = {core::platform::UuidKind::principal, made.service_principal_uuid};
  result.create_finality = storage::database::DatabaseCreateFinalityClass::committed;
  result.create_handoff = storage::database::DatabaseCreateHandoffState::verified_and_closed;
  return result;
}

}  // namespace scratchbird::tests::database_lifecycle
