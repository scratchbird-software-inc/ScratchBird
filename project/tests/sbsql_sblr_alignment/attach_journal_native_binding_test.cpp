// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/sblr_database_attach_journal.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("sb_attach_native_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Check(std::filesystem::create_directory(root));
  a::EngineRequestContext context;
  context.database_path = (root / "fixture.db").string();
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags = {"private_database_attach_journal"};
  a::SblrDatabaseAttachJournalKeyV1 key;
  unsigned ordinal = 1;
  for (auto* field : {&key.database_uuid, &key.session_uuid, &key.statement_receipt_uuid,
       &key.attach_uuid, &key.storage_uuid, &key.alias_uuid, &key.catalog_snapshot_uuid,
       &key.security_context_uuid, &key.policy_snapshot_uuid, &key.transaction_uuid,
       &key.resource_admission_uuid}) *field = scratchbird::tests::FixtureUuid(1120, ordinal++).bytes;
  key.session_uuid[9] = 0; key.session_uuid[15] = 0xff;
  key.alias_name_sha256 = a::SblrDatabaseAttachAliasNameSha256V1("alias", false);
  key.descriptor_sha256[0] = 1; key.storage_alias_binding_sha256[0] = 2;
  key.catalog_generation = key.security_epoch = key.policy_generation = key.transaction_generation =
      key.resource_epoch = key.executor_availability_generation = 1;
  key.mode = key.alias_scope = 1;
  context.database_uuid.bytes = key.database_uuid;
  context.session_uuid.bytes = key.session_uuid;
  context.statement_metadata_snapshot_uuid.bytes = key.catalog_snapshot_uuid;
  context.authorization_context.authority_uuid.bytes = key.security_context_uuid;
  context.transaction_policy_snapshot_uuid.bytes = key.policy_snapshot_uuid;
  context.transaction_uuid.bytes = key.transaction_uuid;
  context.resource_admission_uuid.bytes = key.resource_admission_uuid;
  context.catalog_generation_id = context.security_epoch = context.transaction_policy_snapshot_generation =
      context.local_transaction_id = context.resource_epoch = 1;
  Check(a::HasAuthority(context, key));
  auto crossed = context; crossed.session_uuid.bytes[9] = 1;
  Check(!a::HasAuthority(crossed, key));
  Check(!a::LookupSblrDatabaseAttachJournalV1(crossed, key).ok);
  auto changed = key; changed.session_uuid[9] = 1;
  Check(a::Path(context, key) != a::Path(context, changed));
  changed = key; changed.alias_name_sha256[31] ^= 1;
  Check(a::Path(context, key) != a::Path(context, changed));
  const auto absent = a::LookupSblrDatabaseAttachJournalV1(context, key);
  Check(absent.ok && !absent.found);
  const auto created = a::EnsureSblrDatabaseAttachJournalV1(context, key);
  Check(created.ok && created.found && a::SameKey(created.snapshot.key, key));
  const auto loaded = a::LookupSblrDatabaseAttachJournalV1(context, key);
  Check(loaded.ok && loaded.found && a::SameKey(loaded.snapshot.key, key));
  changed = key; changed.descriptor_sha256[0] ^= 2; // Different, still nonzero digest.
  const auto conflict = a::EnsureSblrDatabaseAttachJournalV1(context, changed);
  Check(!conflict.ok && conflict.diagnostic.code == "DATABASE.ALIAS_CONFLICT");
  // Legacy filenames are discovered without parsing or rendering any UUID.
  const auto legacy = context.database_path + ".sb.sblr_database_attach_journal.v1.legacy";
  { std::ofstream output(legacy); output << "legacy"; }
  const auto refused = a::EnsureSblrDatabaseAttachJournalV1(context, key);
  Check(!refused.ok && refused.diagnostic.message_key == "sblr.database_attach.legacy_journal_present");
  std::filesystem::remove_all(root);
}
