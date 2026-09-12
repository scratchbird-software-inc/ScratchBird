// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "embedded/embedded_engine_client.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
#include "database_lifecycle.hpp"
#include "../database_lifecycle/database_lifecycle_test_memory.hpp"
#include "local_transaction_store.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"
#include <filesystem>
#include <unistd.h>
#endif

namespace p = scratchbird::parser::sbsql;

void Require(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

bool HasDiagnostic(const p::MessageVectorSet& messages, const std::string& code) {
  return std::any_of(messages.diagnostics.begin(), messages.diagnostics.end(),
      [&](const auto& d) { return d.code == code; });
}

#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
namespace tx = scratchbird::transaction::mga;
using scratchbird::core::platform::UuidKind;

void RequireState(const std::string& path, std::uint64_t local_id, tx::TransactionState state) {
  const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(path);
  Require(inventory.ok(), "independent embedded inventory read failed");
  const auto found = std::find_if(inventory.inventory.entries.begin(), inventory.inventory.entries.end(),
      [&](const auto& entry) { return entry.identity.local_id.value == local_id; });
  Require(found != inventory.inventory.entries.end() && found->state == state,
          "embedded result disagrees with independent inventory");
}

void VerifyEnabled() {
  char temp[] = "/tmp/sb-embedded-disconnect-XXXXXX";
  Require(::mkdtemp(temp) != nullptr, "embedded fixture directory creation failed");
  const std::filesystem::path directory(temp);
  const std::string path = (directory / "node.sbdb").string();
  std::cerr << "embedded_disconnect_fixture=" << directory.string() << '\n';
  db::DatabaseCreateConfig create;
  create.path = path;
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779200001000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779200001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779200001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.bootstrap_principal_name = "alice";
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  Require(db::CreateDatabaseFile(create).ok(), "embedded fixture database creation failed");
  {
    p::ParserConfig config;
    config.embedded_engine_direct = true;
    config.embedded_database_path = path;
    p::EmbeddedEngineClient client(config);
    p::SessionContext none;
    p::MessageVectorSet messages;
    Require(client.DisconnectSession(none, &messages) && messages.diagnostics.empty(),
            "unauthenticated embedded disconnect should be a no-op");
    p::AuthCredentialEnvelope credentials;
    credentials.principal = "alice";
    credentials.credential_evidence = "DBLC013G-fixture-password";
    credentials.credential_evidence_present = true;
    p::SessionContext first, sibling;
    if (!client.AuthenticateAndAttach(credentials, &first, &messages)) {
      for (const auto& d : messages.diagnostics) std::cerr << d.code << ':' << d.message << '\n';
      Require(false, "real embedded authentication failed");
    }
    messages = {};
    Require(client.AuthenticateAndAttach(credentials, &sibling, &messages),
            "sibling embedded authentication failed");
    Require(first.authenticated && sibling.authenticated && first.session_uuid != sibling.session_uuid,
            "embedded sessions did not receive separate engine identities");
    RequireState(path, first.local_transaction_id, tx::TransactionState::active);
    RequireState(path, sibling.local_transaction_id, tx::TransactionState::active);
    auto malformed = first;
    malformed.session_uuid.clear();
    messages = {};
    Require(!client.DisconnectSession(malformed, &messages) &&
                HasDiagnostic(messages, "PARSER_SERVER_IPC.SESSION_MISMATCH"),
            "authenticated embedded session with missing identity reported success");
    malformed = first;
    malformed.connection_uuid.clear();
    messages = {};
    Require(!client.DisconnectSession(malformed, &messages) &&
                HasDiagnostic(messages, "PARSER_SERVER_IPC.CONNECTION_MISMATCH"),
            "authenticated embedded session with missing connection reported success");
    malformed = first;
    malformed.session_uuid[14] = '4';
    messages = {};
    Require(!client.DisconnectSession(malformed, &messages) &&
                HasDiagnostic(messages, "PARSER_SERVER_IPC.SESSION_MISMATCH"),
            "non-v7 system session identity was admitted to embedded cleanup");
    RequireState(path, first.local_transaction_id, tx::TransactionState::active);
    Require(::geteuid() != 0, "embedded permission fault requires unprivileged Linux");
    const auto permissions = std::filesystem::status(path).permissions();
    std::filesystem::permissions(path, std::filesystem::perms::owner_read);
    messages = {};
    const bool refused = client.DisconnectSession(first, &messages);
    std::filesystem::permissions(path, permissions);
    Require(!refused && HasDiagnostic(messages, "PARSER_SERVER_IPC.DISCONNECT_OUTCOME_UNKNOWN"),
            "embedded quarantine was reported as complete cleanup");
    RequireState(path, first.local_transaction_id, tx::TransactionState::active);
    RequireState(path, sibling.local_transaction_id, tx::TransactionState::active);
    const auto before_retry = db::LoadLocalTransactionInventoryFromDatabase(path);
    Require(before_retry.ok(), "embedded retry inventory baseline unavailable");
    messages = {};
    const bool retry = client.DisconnectSession(first, &messages);
    // The Core ordinary ACID outcome rule forbids automatic retry
    // while prior transaction finality is unknown. Restored file permission
    // is not engine recovery authority and cannot authorize a new cleanup.
    Require(!retry && HasDiagnostic(messages, "PARSER_SERVER_IPC.DISCONNECT_OUTCOME_UNKNOWN"),
            "repeat embedded disconnect invented known transaction finality");
    const auto after_retry = db::LoadLocalTransactionInventoryFromDatabase(path);
    Require(after_retry.ok() &&
                after_retry.inventory.next_local_transaction_id == before_retry.inventory.next_local_transaction_id &&
                after_retry.inventory.entries.size() == before_retry.inventory.entries.size(),
            "repeat embedded disconnect began new work before engine recovery");
    RequireState(path, first.local_transaction_id, tx::TransactionState::active);
    RequireState(path, sibling.local_transaction_id, tx::TransactionState::active);
    messages = {};
    Require(client.DisconnectSession(sibling, &messages) && !messages.has_errors(),
            "healthy embedded sibling cleanup failed or informational finality became an error");
    RequireState(path, sibling.local_transaction_id, tx::TransactionState::rolled_back);
    RequireState(path, first.local_transaction_id, tx::TransactionState::active);
    messages = {};
    Require(!client.DisconnectSession(sibling, &messages), "stale embedded session acknowledged cleanup twice");
  }
  std::filesystem::remove_all(directory);
  std::cout << "embedded_disconnect enabled=true real_auth=true quarantine_refused=true unknown_retry_refused=true sibling_cleanup=true\n";
}
#endif

int main() {
#if defined(SCRATCHBIRD_SBSQL_ENABLE_EMBEDDED_ENGINE_DIRECT)
  VerifyEnabled();
#else
  p::EmbeddedEngineClient client(p::ParserConfig{});
  p::SessionContext session;
  p::MessageVectorSet messages;
  Require(client.DisconnectSession(session, &messages) && messages.diagnostics.empty(),
          "unbound disabled embedded route should be a no-op");
  session.authenticated = true;
  Require(!client.DisconnectSession(session, &messages) &&
              HasDiagnostic(messages, "PARSER_SERVER_IPC.SESSION_NOT_BOUND"),
          "disabled embedded route fabricated authenticated cleanup success");
  Require(!client.DisconnectSession(session, nullptr),
          "disabled embedded route fabricated success when diagnostic sink absent");
  std::cout << "embedded_disconnect enabled=false authenticated_success_refused=true\n";
#endif
}
