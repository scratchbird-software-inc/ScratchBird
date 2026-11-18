// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_CLI_NATIVE_BOOTSTRAP (owner Q254 = 1, 2026-11-16). The shared
// SBsql/SBsec bootstrap, end to end, on the native create: current-process
// ownership, both passwords from protected stdin, the default policy pack.
// Afterwards the path names exactly one file, it is discovered as a native
// database (first block, then page zero: the stated page count, one filespace),
// the receipt identities are engine identities, and a second bootstrap on the
// same path refuses without touching the file. Refusals: a short password, a
// mismatched service password, an unknown language, a missing policy pack.

#include "first_principal_bootstrap.hpp"
#include "native_database_open_discovery.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace cli = scratchbird::cli;
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace fs = std::filesystem;
unsigned checks = 0;
void Check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  std::cerr << "FAIL " << what << '\n';
  std::exit(1);
}

cli::FirstPrincipalBootstrapRequest Request(const fs::path& database) {
  cli::FirstPrincipalBootstrapRequest request;
  request.explicit_embedded_mode = true;
  request.ownership = cli::EmbeddedCreationOwnership::kCurrentProcess;
  request.principal_name = "qa_admin";
  request.database_path = database.string();
  request.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  request.policy_seed_pack_root = SB_BOOTSTRAP_POLICY_PACK_ROOT;
  request.metric_policy_defaults_path = std::string(SB_BOOTSTRAP_METRIC_TEMPLATES) + "/metric-policy-defaults.conf";
  request.metric_policy_families_path = std::string(SB_BOOTSTRAP_METRIC_TEMPLATES) + "/metric-policy-families.conf";
  request.password_source = cli::FirstPrincipalBootstrapPasswordSource::kProtectedStdin;
  request.database_name = "qa native database";
  request.language_tag = "en";
  request.total_pages = 512;
  request.service_principal_name = "node_service";
  return request;
}

cli::FirstPrincipalBootstrapResult Run(const cli::FirstPrincipalBootstrapRequest& request,
                                       const std::string& stdin_text, std::string* out_text) {
  std::istringstream in(stdin_text);
  std::ostringstream out;
  const auto result = cli::RunFirstPrincipalBootstrap(request, in, out);
  if (out_text) *out_text = out.str();
  return result;
}

std::vector<std::string> Entries(const fs::path& dir) {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(dir)) names.push_back(entry.path().filename().string());
  return names;
}
}  // namespace

int main() {
  std::string name = (fs::temp_directory_path() / "sb_cli_native_bootstrap.XXXXXX").string();
  Check(::mkdtemp(name.data()) != nullptr, "fixture directory");
  const fs::path root = name;
  const auto database = root / "qa.sdb";
  const std::string passwords = "CorrectHorse-Battery-Staple-77\nService-Secret-Rotated-Later-88\n";

  std::string text;
  const auto created = Run(Request(database), passwords, &text);
  Check(created.ok, "native bootstrap creates: " + created.diagnostic_code);
  Check(created.creation_handoff_ready && !created.committed_with_warning, "the handoff is ready, no warning");
  Check(Entries(root) == std::vector<std::string>{"qa.sdb"}, "exactly one file: the database");
  for (const auto* id : {&created.database_uuid, &created.filespace_uuid, &created.principal_uuid,
                         &created.service_principal_uuid, &created.construction_transaction_uuid}) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(*id);
    Check(parsed.ok() && scratchbird::core::uuid::IsEngineIdentityUuid(parsed.value),
          "receipt identity is an engine identity: " + *id);
  }
  Check(created.policy_generation == 0, "the native receipt carries no policy generation (not faked)");
  const auto discovered = db::DiscoverNativeDatabaseAtPath(database.string());
  Check(discovered.ok(), "the file is a native database: " + discovered.code + " " + discovered.detail);
  Check(discovered.page_zero->bootstrap.page_size_bytes == 16384 && discovered.page_zero->total_pages == 512,
        "page zero states the bootstrap's page size and the requested page count");
  const auto database_uuid = scratchbird::core::uuid::ParseUuid(created.database_uuid);
  Check(database_uuid.ok() && discovered.page_zero->bootstrap.database_uuid == database_uuid.value,
        "page zero names the receipt's database");
  Check(fs::file_size(database) == 512u * 16384u, "the file is exactly total_pages pages");

  // A second bootstrap on the same path: refused before any file access.
  std::error_code ec;
  const auto size_before = fs::file_size(database, ec);
  const auto again = Run(Request(database), passwords, nullptr);
  Check(!again.ok && again.diagnostic_code == "BOOTSTRAP.AUTH_DB_ALREADY_OWNED", "existing path refused");
  Check(fs::file_size(database, ec) == size_before && Entries(root).size() == 1, "existing file untouched");

  // Refusals, each leaving the directory with only the first database.
  const auto other = root / "other.sdb";
  {
    const auto short_password = Run(Request(other), "short\nService-Secret-Rotated-Later-88\n", nullptr);
    Check(!short_password.ok && short_password.diagnostic_code == "BOOTSTRAP.SYSARCH_PASSWORD_POLICY_INVALID",
          "first principal password policy");
    const auto no_service = Run(Request(other), "CorrectHorse-Battery-Staple-77\n", nullptr);
    Check(!no_service.ok && no_service.diagnostic_code == "BOOTSTRAP.AUTH_DB_INPUT_INVALID",
          "the service password is required");
    const auto short_service = Run(Request(other), "CorrectHorse-Battery-Staple-77\nx\n", nullptr);
    Check(!short_service.ok && short_service.diagnostic_code == "BOOTSTRAP.SERVICE_PASSWORD_POLICY_INVALID",
          "service password policy");
    auto unknown_language = Request(other);
    unknown_language.language_tag = "tlh";
    const auto refused_language = Run(unknown_language, passwords, nullptr);
    Check(!refused_language.ok && refused_language.diagnostic_code == "BOOTSTRAP.LANGUAGE_UNKNOWN",
          "unknown language tag");
    auto no_pack = Request(other);
    no_pack.policy_seed_pack_root = (root / "no-such-pack").string();
    const auto refused_pack = Run(no_pack, passwords, nullptr);
    Check(!refused_pack.ok && refused_pack.diagnostic_code == "DATABASE.CREATE_POLICY_PACK_INVALID",
          "missing policy pack: " + refused_pack.diagnostic_code);
    auto no_pages = Request(other);
    no_pages.total_pages = 0;
    const auto refused_pages = Run(no_pages, passwords, nullptr);
    Check(!refused_pages.ok && refused_pages.diagnostic_code == "DATABASE.CREATE_TOTAL_PAGES_REQUIRED",
          "total pages required: " + refused_pages.diagnostic_code);
    Check(Entries(root) == std::vector<std::string>{"qa.sdb"}, "refusals created nothing");
  }
  Check(text.empty(), "protected stdin writes no prompt");
  fs::remove_all(root, ec);
  std::cout << "cli_first_principal_bootstrap_native_test: " << checks << " checks passed\n";
  return 0;
}
