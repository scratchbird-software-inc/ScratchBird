// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_DIAGNOSTIC_REGISTRY_TEST (S4h part 1, owner option A, 2026-12-07).
//
// The NATIVE diagnostic identity registry is a per-build constant table: every identity is derived (domain-separated SHA-256) from the
// compiled diagnostic code catalog, so it is the same after a restart and in every process of the same build, and no file is read or
// written. This test is an INDEPENDENT oracle: it re-derives the snapshot uuid, the generation and every row identity itself, from the
// catalog, with the documented formula, and compares them with what the engine returns.
//   modes:  (none)            the whole contract, in this process, plus two CHILD processes that dump the table (determinism)
//           dump              prints "TABLE <sha256 of the whole table>" (the child mode)
//           nofile <dir>      loads through a native context whose database_path names <dir>/absent.sdb and prints "NOFILE <n rows>"
//                             (the strace wrapper proves no syscall names <dir>)
//   The V11 acquire through the SERVER route on a native session is native_receipt's (it needs a served database); this file owns
//   the table's contract.
#include "api_types.hpp"
#include "core/diagnostics/canonical_diagnostic_catalog.hpp"
#include "engine/internal_api/native_served_database_facts.hpp"
#include "engine/internal_api/sblr_diagnostic_identity_registry.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace api = scratchbird::engine::internal_api;
namespace hash = scratchbird::core::hash;
namespace catalog = scratchbird::core::diagnostics;
using Bytes = std::vector<std::uint8_t>;
using Uuid = std::array<std::uint8_t, 16>;

namespace {
int checks = 0;
int failures = 0;
void Check(bool ok, const std::string& why) {
  ++checks;
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << why << '\n';
  }
}

Uuid Derive(std::string_view domain, const std::uint8_t* data, std::size_t size) {
  Bytes material(domain.begin(), domain.end());
  material.insert(material.end(), data, data + size);
  const auto digest = hash::ComputeSha256Digest(material);
  Check(digest.ok() && digest.digest_bytes == 32, "oracle SHA-256");
  Uuid id{};
  std::copy_n(digest.digest.begin(), 16, id.begin());
  id[6] = static_cast<std::uint8_t>((id[6] & 0x0f) | 0x70);
  id[8] = static_cast<std::uint8_t>((id[8] & 0x3f) | 0x80);
  return id;
}

api::EngineUuid DatabaseIdentity() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  auto id = scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::object, static_cast<std::uint64_t>(now));
  Check(id.ok(), "fixture identity");
  return id.value.value;
}

api::EngineRequestContext NativeContext(const std::string& path) {
  api::EngineRequestContext c;
  c.database_path = path;
  c.database_uuid = DatabaseIdentity();
  c.principal_uuid = DatabaseIdentity();
  c.session_uuid = DatabaseIdentity();
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  c.security_epoch = 1;
  c.catalog_generation_id = 1;
  auto facts = std::make_shared<api::NativeServedDatabaseFacts>();
  facts->database_uuid = scratchbird::core::platform::Uuid{c.database_uuid.bytes};
  c.native_served_facts = facts;
  return c;
}

// The whole table as bytes, so two processes can compare one digest.
std::string TableDigest(const api::SblrDiagnosticIdentitySnapshotV1& s) {
  Bytes material;
  material.insert(material.end(), s.snapshot_uuid.begin(), s.snapshot_uuid.end());
  for (unsigned i = 0; i < 8; ++i) material.push_back(static_cast<std::uint8_t>(s.generation >> (8 * i)));
  material.insert(material.end(), s.source_sha256.begin(), s.source_sha256.end());
  material.insert(material.end(), s.evidence_sha256.begin(), s.evidence_sha256.end());
  for (const auto& row : s.rows) {
    material.insert(material.end(), row.diagnostic_uuid.begin(), row.diagnostic_uuid.end());
    for (unsigned i = 0; i < 8; ++i) material.push_back(static_cast<std::uint8_t>(row.diagnostic_generation >> (8 * i)));
    for (unsigned i = 0; i < 4; ++i) material.push_back(static_cast<std::uint8_t>(row.precedence_ordinal >> (8 * i)));
    material.push_back(row.severity_code);
    material.push_back(row.redaction_class);
    for (unsigned i = 0; i < 4; ++i) material.push_back(static_cast<std::uint8_t>(row.maximum_safe_field_count >> (8 * i)));
    material.insert(material.end(), row.row_identity_sha256.begin(), row.row_identity_sha256.end());
    material.insert(material.end(), row.canonical_code.begin(), row.canonical_code.end());
    material.push_back(0);
  }
  const auto digest = hash::ComputeSha256Digest(material);
  std::string out;
  char buffer[3];
  for (std::size_t i = 0; i < 32; ++i) {
    std::snprintf(buffer, sizeof buffer, "%02x", digest.digest[i]);
    out += buffer;
  }
  return out;
}

std::string RunChild(const char* self, const char* mode, const char* arg = nullptr) {
  int fds[2];
  if (::pipe(fds) != 0) return {};
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    if (arg) ::execl(self, self, mode, arg, static_cast<char*>(nullptr));
    else ::execl(self, self, mode, static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(fds[1]);
  std::string out;
  char buffer[512];
  ssize_t n;
  while ((n = ::read(fds[0], buffer, sizeof buffer)) > 0) out.append(buffer, static_cast<std::size_t>(n));
  ::close(fds[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? out : std::string{};
}

api::SblrDiagnosticIdentitySnapshotV1 TableFor(const Uuid& database) {
  return api::BuildNativeSblrDiagnosticIdentitySnapshotV1(database);
}

void Contract() {
  const auto definitions = catalog::CanonicalDiagnosticCodeCatalog();
  const auto& source = catalog::CanonicalDiagnosticCodeSourceSha256();
  const auto database = DatabaseIdentity().bytes;
  const auto table = TableFor(database);

  // ---- derived, not minted: the oracle re-derives every identity from the catalog ----
  Check(definitions.size > 0 && table.rows.size() == definitions.size, "one row per canonical code");
  std::array<std::size_t, 16> by_severity{};
  for (const auto& row : table.rows) ++by_severity[row.severity_code & 15];
  std::cerr << "severity histogram:";
  for (unsigned i = 0; i < 16; ++i) std::cerr << ' ' << i << '=' << by_severity[i];
  std::cerr << '\n';
  Check(table.source_sha256 == source, "the snapshot carries the compiled catalog's source hash");
  Check(table.snapshot_uuid == Derive("ScratchBird.NativeDiagnosticRegistry.Snapshot.V1", source.data(), source.size()),
        "the snapshot uuid is the documented derivation of the source hash");
  std::uint64_t expected_generation = 0;
  for (unsigned i = 0; i < 8; ++i) expected_generation |= std::uint64_t(source[i]) << (8 * i);
  expected_generation &= 0x7fffffffffffffffULL;
  if (expected_generation == 0) expected_generation = 1;
  Check(table.generation == expected_generation && table.generation != 0, "the generation is the documented derivation of the source hash");
  Check(table.database_uuid == database, "the database identity is the caller's");
  const auto expected_generation_of = [](const auto& row) {
    const std::uint8_t metadata[6] = {row.severity_code, row.redaction_class, static_cast<std::uint8_t>(row.maximum_safe_field_count),
                                      static_cast<std::uint8_t>(row.maximum_safe_field_count >> 8), static_cast<std::uint8_t>(row.maximum_safe_field_count >> 16),
                                      static_cast<std::uint8_t>(row.maximum_safe_field_count >> 24)};
    const auto id = Derive("ScratchBird.NativeDiagnosticRegistry.RowGeneration.V1", metadata, sizeof metadata);
    std::uint64_t g = 0;
    for (unsigned i = 0; i < 8; ++i) g |= std::uint64_t(id[i]) << (8 * i);
    g &= 0x7fffffffffffffffULL;
    return g == 0 ? std::uint64_t{1} : g;
  };
  for (std::size_t i = 0; i < definitions.size && i < table.rows.size(); ++i) {
    const auto& row = table.rows[i];
    const std::string code(definitions.data[i].code);
    const auto expected = Derive("ScratchBird.NativeDiagnosticRegistry.Code.V1", reinterpret_cast<const std::uint8_t*>(code.data()), code.size());
    if (row.canonical_code != code || row.diagnostic_uuid != expected || row.diagnostic_generation != expected_generation_of(row) || row.diagnostic_generation == 0 ||
        row.precedence_ordinal != i + 1 || row.severity_code != static_cast<std::uint8_t>(definitions.data[i].severity)) {
      Check(false, "row " + std::to_string(i) + " (" + code + ") differs from the oracle");
      break;
    }
  }
  Check(true, "every row equals the oracle's derivation");

  // ---- uniqueness and well-formedness ----
  std::set<Uuid> identities;
  bool well_formed = true;
  for (const auto& row : table.rows) {
    identities.insert(row.diagnostic_uuid);
    well_formed = well_formed && (row.diagnostic_uuid[6] & 0xf0) == 0x70 && (row.diagnostic_uuid[8] & 0xc0) == 0x80;
  }
  Check(identities.size() == table.rows.size(), "every diagnostic uuid is unique (one identity per code)");
  Check(well_formed && (table.snapshot_uuid[6] & 0xf0) == 0x70 && (table.snapshot_uuid[8] & 0xc0) == 0x80, "every identity is a well-formed v7 value");
  Check(identities.count(table.snapshot_uuid) == 0 && identities.count(database) == 0, "the snapshot and database identities are distinct from every row identity");
  Check(std::is_sorted(table.rows.begin(), table.rows.end(), [](const auto& a, const auto& b) { return a.canonical_code < b.canonical_code; }),
        "rows are in the catalog's code order");
  // the row identity hash is the wire codec's own (an independent encode of each row must reproduce it), and the whole table is a valid cohort
  {
    std::vector<scratchbird::wire::DiagnosticIdentityProjectionV1> projected;
    bool hashes_equal = true;
    for (const auto& row : table.rows) {
      scratchbird::wire::DiagnosticIdentityProjectionV1 copy = row;
      scratchbird::wire::DiagnosticIdentityProjectionBytesV1 bytes{};
      hashes_equal = hashes_equal && scratchbird::wire::EncodeDiagnosticIdentityProjectionV1(&copy, &bytes, true) &&
                     copy.row_identity_sha256 == row.row_identity_sha256;
      projected.push_back(row);
    }
    Check(hashes_equal, "every row's identity hash equals an independent encode through the wire codec");
    Check(scratchbird::wire::ValidateDiagnosticIdentityCohortV1(projected, true), "the whole table is a valid diagnostic identity cohort (the parser's own validator)");
    std::vector<scratchbird::wire::DiagnosticIdentityProjectionV1> visible;
    for (const auto& row : table.rows) if (row.severity_code != 13) visible.push_back(row);
    Check(scratchbird::wire::ValidateDiagnosticIdentityCohortV1(visible), "the parser-visible rows (no internal severity) form a valid cohort without the internal allowance");
  }

  // ---- the same table through the same interface the legacy registry implements ----
  const auto context = NativeContext("/nonexistent-dir-for-native-registry/absent.sdb");
  auto loaded = api::LoadSblrDiagnosticIdentitySnapshotV1(context);
  Check(loaded.ok, "a native context loads the registry: " + loaded.diagnostic.code);
  if (loaded.ok) {
    Check(loaded.snapshot.snapshot_uuid == table.snapshot_uuid && loaded.snapshot.generation == table.generation &&
              loaded.snapshot.source_sha256 == table.source_sha256,
          "the load carries the table's snapshot uuid, generation and source hash");
    Check(!loaded.snapshot.rows.empty() && loaded.snapshot.rows.size() < table.rows.size(), "the visibility filter removed the restricted severities");
    std::size_t visible = 0;
    bool in_order = true;
    for (const auto& row : table.rows) {
      if (row.severity_code == 9 || row.severity_code == 11 || row.severity_code == 12 || row.severity_code == 13) continue;
      in_order = in_order && visible < loaded.snapshot.rows.size() && loaded.snapshot.rows[visible].diagnostic_uuid == row.diagnostic_uuid &&
                 loaded.snapshot.rows[visible].row_identity_sha256 == row.row_identity_sha256;
      ++visible;
    }
    Check(in_order && visible == loaded.snapshot.rows.size(), "an ordinary context sees exactly the rows of severities 1-8 and 10, none restricted");
    const auto row = loaded.snapshot.rows.empty() ? api::SblrDiagnosticIdentityRowV1{} : loaded.snapshot.rows.front();
    const auto found = api::LookupSblrDiagnosticIdentityV1(context, loaded.snapshot, row.diagnostic_uuid, row.diagnostic_generation);
    Check(found.ok && found.row.diagnostic_uuid == row.diagnostic_uuid, "a visible identity is found through LookupSblrDiagnosticIdentityV1");
    const auto restricted = std::find_if(table.rows.begin(), table.rows.end(), [](const auto& r) { return r.severity_code == 13; });
    if (restricted != table.rows.end()) {
      const auto hidden = api::LookupSblrDiagnosticIdentityV1(context, loaded.snapshot, restricted->diagnostic_uuid, restricted->diagnostic_generation);
      Check(!hidden.ok && hidden.diagnostic.code == "SECURITY.ACCESS_DENIED", "the internal (severity 13) identity is hidden from an ordinary context");
    }
  }

  // ---- the visibility filter follows the context's AUTHORIZATION (the legacy registry's materialized-authorization rules) ----
  {
    auto privileged = NativeContext("/nonexistent-dir-for-native-registry/absent.sdb");
    auto& a = privileged.authorization_context;
    a.present = true;
    a.principal_uuid = privileged.principal_uuid;
    a.authority_uuid = DatabaseIdentity();
    a.security_context_generation = 1;
    a.security_epoch = 1;
    a.policy_epoch = 1;
    a.catalog_generation_id = 1;
    a.effective_subjects.push_back({privileged.principal_uuid, "principal"});
    api::EngineMaterializedAuthorizationGrant g;
    g.grant_uuid = DatabaseIdentity();
    g.subject_uuid = privileged.principal_uuid;
    g.subject_kind = "principal";
    g.target_uuid = privileged.database_uuid;
    g.right = "READ_DIAGNOSTIC_DETAIL";
    g.security_epoch = 1;
    a.grants.push_back(g);
    const auto detail = api::LoadSblrDiagnosticIdentitySnapshotV1(privileged);
    Check(api::SblrDiagnosticIdentityVisibilityMaskV1(privileged) == 0x17fe, "READ_DIAGNOSTIC_DETAIL widens the mask");
    // The compiled catalog holds no severity 9, 11 or 12 row today (histogram above), so a grant widens the MASK but not the row set;
    // the oracle therefore compares each context's rows with the table filtered by that context's mask.
    const auto expected_rows = [&](std::uint16_t mask) {
      std::size_t n = 0;
      for (const auto& row : table.rows) n += (mask & (std::uint16_t(1) << row.severity_code)) != 0 ? 1 : 0;
      return n;
    };
    Check(detail.ok && detail.snapshot.rows.size() == expected_rows(0x17fe), "a READ_DIAGNOSTIC_DETAIL context sees exactly the rows its mask permits");
    g.right = "AUDIT_READ";
    a.grants.push_back(g);
    const auto audit = api::LoadSblrDiagnosticIdentitySnapshotV1(privileged);
    Check(api::SblrDiagnosticIdentityVisibilityMaskV1(privileged) == 0x1ffe && audit.ok && audit.snapshot.rows.size() == expected_rows(0x1ffe),
          "an AUDIT_READ context sees exactly the rows its mask permits");
    bool no_internal = true;
    for (const auto& row : audit.snapshot.rows) no_internal = no_internal && row.severity_code != 13;
    Check(no_internal, "no internal severity row is ever returned");
  }

  // ---- refusals: the same context requirements as the legacy load, minus the path ----
  {
    auto no_security = context;
    no_security.security_context_present = false;
    const auto refused = api::LoadSblrDiagnosticIdentitySnapshotV1(no_security);
    Check(!refused.ok && refused.diagnostic.code == "SECURITY.ACCESS_DENIED", "no security context: refused (hidden)");
    auto not_owned = context;
    not_owned.statement_metadata_snapshot_engine_owned = false;
    Check(!api::LoadSblrDiagnosticIdentitySnapshotV1(not_owned).ok, "a statement snapshot the engine does not own: refused");
    auto no_session = context;
    no_session.session_uuid = {};
    Check(!api::LoadSblrDiagnosticIdentitySnapshotV1(no_session).ok, "a nil session identity: refused");
    auto no_database = context;
    no_database.database_uuid = {};
    Check(!api::LoadSblrDiagnosticIdentitySnapshotV1(no_database).ok, "a nil database identity: refused");
  }

  // ---- stable across calls ----
  const auto again = TableFor(database);
  Check(TableDigest(again) == TableDigest(table), "two builds of the table in one process are equal");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "dump") {
    const Uuid fixed{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x77, 0x08, 0x89, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
    std::cout << "TABLE " << TableDigest(TableFor(fixed)) << '\n';
    return 0;
  }
  if (argc >= 3 && std::string_view(argv[1]) == "nofile") {
    const auto context = NativeContext(std::string(argv[2]) + "/absent.sdb");
    const auto loaded = api::LoadSblrDiagnosticIdentitySnapshotV1(context);
    std::cout << "NOFILE " << (loaded.ok ? loaded.snapshot.rows.size() : 0) << '\n';
    return loaded.ok ? 0 : 1;
  }
  Contract();
  // ---- determinism across processes: the same table in two separate processes of this build ----
  const Uuid fixed{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x77, 0x08, 0x89, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
  const auto here = "TABLE " + TableDigest(TableFor(fixed)) + "\n";
  const auto first = RunChild(argv[0], "dump");
  const auto second = RunChild(argv[0], "dump");
  Check(!first.empty() && first == second, "two separate processes dump the same table");
  Check(first == here, "and it equals this process's table");
  std::cout << "native_diagnostic_registry: " << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
