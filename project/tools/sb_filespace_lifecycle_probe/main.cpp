// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-FILESPACE-LIFECYCLE-PROBE-ANCHOR
#include "filespace_lifecycle.hpp"
#include "uuid.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::storage::filespace::ActivePinCount;
using scratchbird::storage::filespace::ApplyFilespaceOperation;
using scratchbird::storage::filespace::FilespaceLifecyclePolicy;
using scratchbird::storage::filespace::FilespaceOperation;
using scratchbird::storage::filespace::FilespaceOperationRequest;
using scratchbird::storage::filespace::FilespacePinKind;
using scratchbird::storage::filespace::FilespaceRegistry;
using scratchbird::storage::filespace::FilespaceRole;
using scratchbird::storage::filespace::FilespaceState;
using scratchbird::storage::filespace::ParseFilespaceRegistry;
using scratchbird::storage::filespace::SerializeFilespaceRegistry;

TypedUuid Generate(UuidKind kind, u64 millis) {
  const auto generated = GenerateEngineIdentityV7(kind, millis);
  if (!generated.ok()) {
    return {};
  }
  return generated.value;
}

FilespaceOperationRequest Request(FilespaceOperation operation,
                                  TypedUuid database_uuid,
                                  TypedUuid filespace_uuid,
                                  std::string path,
                                  FilespaceRole role = FilespaceRole::secondary_data) {
  FilespaceOperationRequest request;
  request.operation = operation;
  request.database_uuid = database_uuid;
  request.filespace_uuid = filespace_uuid;
  request.path = std::move(path);
  request.role = role;
  request.page_size = 16384;
  request.reason = "probe";
  return request;
}

void PrintCheck(const char* name, bool passed) {
  std::cout << name << "=" << (passed ? "true" : "false") << "\n";
}

}  // namespace

int main() {
  const u64 base_millis = 1770100000000ull;
  const TypedUuid database_uuid = Generate(UuidKind::database, base_millis);
  const TypedUuid primary_uuid = Generate(UuidKind::filespace, base_millis + 1);
  const TypedUuid secondary_uuid = Generate(UuidKind::filespace, base_millis + 2);
  const TypedUuid archive_uuid = Generate(UuidKind::filespace, base_millis + 3);
  if (!database_uuid.valid() || !primary_uuid.valid() || !secondary_uuid.valid() || !archive_uuid.valid()) {
    std::cerr << "uuid_generation_failed\n";
    return 2;
  }

  FilespaceRegistry registry;
  const auto create_primary = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::create_filespace, database_uuid, primary_uuid, "/tmp/sb_primary.fs", FilespaceRole::active_primary));
  const auto create_secondary = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::create_filespace, database_uuid, secondary_uuid, "/tmp/sb_secondary.fs", FilespaceRole::primary_candidate));
  const auto create_archive = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::create_filespace, database_uuid, archive_uuid, "/tmp/sb_archive.fs", FilespaceRole::secondary_history));

  auto pin_request = Request(FilespaceOperation::pin_filespace, database_uuid, secondary_uuid, {});
  pin_request.pin_kind = FilespacePinKind::transaction;
  pin_request.pin_owner = "txn:probe";
  pin_request.pin_count = 1;
  const auto pin_secondary = ApplyFilespaceOperation(&registry, pin_request);

  const auto detach_pinned = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::detach_filespace, database_uuid, secondary_uuid, {}));
  const auto promote_pinned = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::promote_filespace, database_uuid, secondary_uuid, {}));

  auto unpin_request = Request(FilespaceOperation::unpin_filespace, database_uuid, secondary_uuid, {});
  unpin_request.pin_kind = FilespacePinKind::transaction;
  unpin_request.pin_owner = "txn:probe";
  const auto unpin_secondary = ApplyFilespaceOperation(&registry, unpin_request);

  const auto promote_with_primary = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::promote_filespace, database_uuid, secondary_uuid, {}));
  auto replace_primary_request = Request(FilespaceOperation::promote_filespace, database_uuid, secondary_uuid, {});
  replace_primary_request.policy.allow_primary_replacement = true;
  const auto promote_secondary = ApplyFilespaceOperation(&registry, replace_primary_request);
  const auto read_only = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::set_read_only, database_uuid, secondary_uuid, {}));
  const auto read_write = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::set_read_write, database_uuid, secondary_uuid, {}));
  const auto archive_owner = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::assign_archive_owner, database_uuid, archive_uuid, {}));
  const auto archive_read_write = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::set_read_write, database_uuid, archive_uuid, {}));

  auto pin_archive_request = Request(FilespaceOperation::pin_filespace, database_uuid, archive_uuid, {});
  pin_archive_request.pin_kind = FilespacePinKind::archive;
  pin_archive_request.pin_owner = "archive:probe";
  const auto pin_archive = ApplyFilespaceOperation(&registry, pin_archive_request);
  const auto drop_pinned = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::drop_filespace, database_uuid, archive_uuid, {}));
  auto unpin_archive_request = Request(FilespaceOperation::unpin_filespace, database_uuid, archive_uuid, {});
  unpin_archive_request.pin_kind = FilespacePinKind::archive;
  unpin_archive_request.pin_owner = "archive:probe";
  const auto unpin_archive = ApplyFilespaceOperation(&registry, unpin_archive_request);
  const auto drop_unpinned = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::drop_filespace, database_uuid, archive_uuid, {}));

  const auto serialized = SerializeFilespaceRegistry(registry);
  const auto parsed = serialized.ok() ? ParseFilespaceRegistry(serialized.payload) : decltype(ParseFilespaceRegistry({})){};

  const bool create_primary_ok = create_primary.ok() &&
                                 create_primary.descriptor.role == FilespaceRole::active_primary &&
                                 create_primary.descriptor.first_filespace &&
                                 create_primary.descriptor.startup_authority &&
                                 create_primary.descriptor.catalog_persistence_owner &&
                                 create_primary.evidence.sequence == 1 &&
                                 create_primary.durable_state_changed;
  const bool create_secondary_ok = create_secondary.ok() &&
                                   create_secondary.descriptor.role == FilespaceRole::primary_candidate;
  const bool create_archive_ok = create_archive.ok() &&
                                 create_archive.descriptor.role == FilespaceRole::secondary_history;
  const bool pin_secondary_ok = pin_secondary.ok() && ActivePinCount(pin_secondary.descriptor) == 1;
  const bool detach_pinned_refused = !detach_pinned.ok() &&
                                     detach_pinned.diagnostic.diagnostic_code == "SB-FILESPACE-LIFECYCLE-DETACH-PINNED";
  const bool promote_pinned_refused = !promote_pinned.ok() &&
                                      promote_pinned.diagnostic.diagnostic_code == "SB-FILESPACE-LIFECYCLE-PROMOTE-PINNED";
  const bool unpin_secondary_ok = unpin_secondary.ok() && ActivePinCount(unpin_secondary.descriptor) == 0;
  const bool promote_with_primary_refused = !promote_with_primary.ok() &&
                                            promote_with_primary.diagnostic.diagnostic_code == "SB-FILESPACE-LIFECYCLE-PRIMARY-ALREADY-EXISTS";
  bool old_primary_shadowed = false;
  for (const auto& descriptor : registry.filespaces) {
    if (descriptor.filespace_uuid.value == primary_uuid.value) {
      old_primary_shadowed = descriptor.role == FilespaceRole::primary_shadow &&
                             !descriptor.startup_authority &&
                             !descriptor.catalog_persistence_owner;
    }
  }
  const bool promote_secondary_ok = promote_secondary.ok() &&
                                    promote_secondary.descriptor.role == FilespaceRole::active_primary &&
                                    promote_secondary.descriptor.startup_authority &&
                                    promote_secondary.descriptor.catalog_persistence_owner &&
                                    promote_secondary.descriptor.state == FilespaceState::attached;
  const bool read_only_ok = read_only.ok() && read_only.descriptor.read_only &&
                            read_only.descriptor.state == FilespaceState::read_only;
  const bool read_write_ok = read_write.ok() && !read_write.descriptor.read_only &&
                             read_write.descriptor.state == FilespaceState::attached;
  const bool archive_owner_ok = archive_owner.ok() && archive_owner.descriptor.archive_owner &&
                                archive_owner.descriptor.role == FilespaceRole::archive_history &&
                                archive_owner.descriptor.read_only &&
                                archive_owner.descriptor.state == FilespaceState::archived;
  const bool archive_read_write_refused = !archive_read_write.ok() &&
                                          archive_read_write.diagnostic.diagnostic_code == "SB-FILESPACE-LIFECYCLE-ARCHIVE-READ-WRITE-FORBIDDEN";
  const bool drop_pinned_refused = pin_archive.ok() && !drop_pinned.ok() &&
                                   drop_pinned.diagnostic.diagnostic_code == "SB-FILESPACE-LIFECYCLE-DROP-PINNED";
  const bool drop_unpinned_ok = unpin_archive.ok() && drop_unpinned.ok() &&
                                drop_unpinned.descriptor.state == FilespaceState::dropped;
  const bool evidence_roundtrip_ok = serialized.ok() && parsed.ok() &&
                                     parsed.registry.filespaces.size() == registry.filespaces.size() &&
                                     parsed.registry.evidence.size() == registry.evidence.size() &&
                                     parsed.registry.next_evidence_sequence == registry.next_evidence_sequence;

  PrintCheck("create_primary_ok", create_primary_ok);
  PrintCheck("create_secondary_ok", create_secondary_ok);
  PrintCheck("create_archive_ok", create_archive_ok);
  PrintCheck("pin_secondary_ok", pin_secondary_ok);
  PrintCheck("detach_pinned_refused", detach_pinned_refused);
  PrintCheck("promote_pinned_refused", promote_pinned_refused);
  PrintCheck("unpin_secondary_ok", unpin_secondary_ok);
  PrintCheck("promote_with_primary_refused", promote_with_primary_refused);
  PrintCheck("old_primary_shadowed", old_primary_shadowed);
  PrintCheck("promote_secondary_ok", promote_secondary_ok);
  PrintCheck("read_only_ok", read_only_ok);
  PrintCheck("read_write_ok", read_write_ok);
  PrintCheck("archive_owner_ok", archive_owner_ok);
  PrintCheck("archive_read_write_refused", archive_read_write_refused);
  PrintCheck("drop_pinned_refused", drop_pinned_refused);
  PrintCheck("drop_unpinned_ok", drop_unpinned_ok);
  PrintCheck("evidence_roundtrip_ok", evidence_roundtrip_ok);

  return create_primary_ok && create_secondary_ok && create_archive_ok && pin_secondary_ok &&
                 detach_pinned_refused && promote_pinned_refused &&
                 unpin_secondary_ok && promote_with_primary_refused &&
                 old_primary_shadowed && promote_secondary_ok && read_only_ok &&
                 read_write_ok && archive_owner_ok && archive_read_write_refused &&
                 drop_pinned_refused && drop_unpinned_ok && evidence_roundtrip_ok
             ? 0
             : 1;
}
