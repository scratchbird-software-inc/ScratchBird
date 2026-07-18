// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Creates the minimal real database used by the Linux manager full-path
// smoke.  It deliberately seeds only the first principal that is committed by
// database-create transaction 1; it never writes security sidecars, synthetic
// grants, or parser-owned authorization state.

#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "bootstrap_password_verifier.hpp"

#include <openssl/crypto.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void ClearSensitive(std::string* value) {
  if (value != nullptr && !value->empty()) {
    OPENSSL_cleanse(value->data(), value->size());
    value->clear();
  }
}

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool NoSecuritySidecars(const std::filesystem::path& database_path) {
  for (const char* suffix : {".sb.security_principal_events",
                             ".sb.local_password_auth"}) {
    std::error_code error;
    if (std::filesystem::exists(database_path.string() + suffix, error) && !error) {
      return false;
    }
  }
  return true;
}

bool CreateCanonicalManagerFixture(const std::filesystem::path& database_path,
                                   const std::string& principal,
                                   std::string* password) {
  if (password == nullptr ||
      !scratchbird::cli::BootstrapPasswordSecretValid(*password)) {
    std::cerr << "manager fixture password does not meet bootstrap policy\n";
    return false;
  }
  if (std::filesystem::exists(database_path)) {
    std::cerr << "manager fixture database already exists\n";
    return false;
  }

  std::string fingerprint;
  if (!scratchbird::cli::DeriveBootstrapPasswordVerifier(*password, &fingerprint)) {
    std::cerr << "manager fixture password verifier derivation failed\n";
    return false;
  }

  const auto configured_memory = memory::ConfigureDefaultMemoryManager(
      memory::DefaultLocalEngineMemoryPolicy(), "sbsql_manager_database_seed");
  if (!configured_memory.ok()) {
    std::cerr << configured_memory.diagnostic.diagnostic_code << ':'
              << configured_memory.diagnostic.message_key << '\n';
    ClearSensitive(&fingerprint);
    return false;
  }

  const auto now = CurrentUnixMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  if (!database_uuid.ok() || !filespace_uuid.ok()) {
    std::cerr << "manager fixture UUID generation failed\n";
    ClearSensitive(&fingerprint);
    return false;
  }

  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16u * 1024u;
  create.creation_unix_epoch_millis = now;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.require_policy_seed_pack = false;
  create.bootstrap_principal_name = principal;
  create.bootstrap_credential_fingerprint = fingerprint;
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.allow_overwrite = false;
  const auto created = db::CreateDatabaseFile(create);
  ClearSensitive(&create.bootstrap_credential_fingerprint);
  ClearSensitive(&fingerprint);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
    return false;
  }

  const auto catalog = db::ReadDatabaseBootstrapSecurityCatalog(create.path);
  if (!catalog.ok() || !catalog.state.present ||
      !catalog.state.committed_by_inventory ||
      catalog.state.principal_name != principal ||
      uuid::UuidToString(catalog.state.sysarch_role_uuid.value) !=
          db::kCanonicalSysarchRoleObjectUuid ||
      catalog.state.credential_fingerprint.empty()) {
    std::cerr << "manager fixture canonical Tx1 security catalog verification failed\n";
    return false;
  }
  if (!NoSecuritySidecars(database_path)) {
    std::cerr << "manager fixture created forbidden security sidecar\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: sbsql_manager_database_seed <database> <principal> <password-stdin>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path database_path = argv[1];
  const std::string principal = argv[2];
  std::string password;
  if (!std::getline(std::cin, password)) {
    std::cerr << "manager fixture password must be supplied on stdin\n";
    return EXIT_FAILURE;
  }
  const bool created = CreateCanonicalManagerFixture(database_path, principal, &password);
  ClearSensitive(&password);
  if (!created) {
    return EXIT_FAILURE;
  }

  std::cout << "sbsql_manager_database_seed=passed database=" << database_path
            << " principal=" << principal
            << " security_catalog=canonical_tx1 sidecars=absent\n";
  return EXIT_SUCCESS;
}
