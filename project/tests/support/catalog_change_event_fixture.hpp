// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2b) test support: the writer refuses a
// definition version that does not cite a catalog.metadata_change event of its
// own transaction, and builds that event from the create's construction intent
// row. Component fixtures made with the creation primitive get the intent row
// the production create writes (database_lifecycle: role 6, unnamed, its own
// audit anchor), and a transaction writes its event before changing a
// definition.
#include "catalog_construction_intent.hpp"
#include "native_catalog_version_writer.hpp"
#include "native_construction_audit.hpp"

#include <array>
#include <optional>
#include <string>

namespace scratchbird::tests::catalog_change_event {
namespace cat = scratchbird::core::catalog;
namespace sdb = scratchbird::storage::database;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

// The construction intent row for `request` (as AcquireNativeConstructionIntent
// gives it), its metadata from `like` (owner, owning schema, security policy,
// epochs), parented to that schema; nullopt when the intent cannot be acquired.
inline std::optional<cat::CatalogMetadataVersion> IntentSeedRow(const sdb::NativeFilespaceInitializationRequest& request,
                                                                const Uuid& engine_uuid,
                                                                scratchbird::core::uuid::StandaloneUuidV7Issuer& issuer,
                                                                cat::CatalogMetadataVersion like) {
  const auto audit = sdb::AcquireNativeConstructionIntent(request, engine_uuid, issuer);
  if (!audit.ok()) return std::nullopt;
  std::array<scratchbird::core::platform::byte, cat::kCatalogConstructionIntentMaxBytes> bytes{};
  const auto encoded = cat::EncodeCatalogConstructionIntentInto(*audit.event, bytes);
  if (!encoded.ok()) return std::nullopt;
  auto& m = like;
  m.record.header.kind = cat::CatalogRecordKind::audit_evidence;
  m.record.header.object_uuid = {UuidKind::object, audit.event->event_uuid};
  m.record.header.row_uuid = audit.row_uuid;
  m.record.header.parent_uuid = {UuidKind::object, m.owning_schema_uuid.value};
  m.record.header.deleted = false;
  m.record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()), encoded.bytes.size());
  m.object_subtype = "node_construction_intent";
  m.default_name_uuid = {};
  m.name_vector_uuid = {};
  m.audit_uuid = m.record.header.object_uuid;
  m.creator_transaction_uuid = request.creator.transaction_uuid;
  m.creator_local_transaction_id = 1;
  m.definition_version = 1;
  m.catalog_generation = 1;
  return m;
}

// Transaction `order`'s event (identities the caller's), written through the
// writer; the time is a fixed fixture time, not derived from any identity.
inline sdb::CatalogVersionWriteResult WriteEvent(scratchbird::storage::disk::CarefulWriteSink& sink,
                                                 const sdb::CatalogVersionWriterBinding& binding, u64 order,
                                                 const Uuid& transaction, const Uuid& event, const Uuid& row,
                                                 const Uuid& version, const Uuid& engine, u64 time_utc_us) {
  sdb::CatalogChangeEventRequest r;
  r.transaction_order = order;
  r.transaction_uuid = transaction;
  r.event_uuid = event;
  r.row_uuid = row;
  r.version_uuid = version;
  r.engine_uuid = engine;
  r.event_time_utc_us = time_utc_us;
  return sdb::WriteCatalogChangeEvent(sink, binding, r);
}
}  // namespace scratchbird::tests::catalog_change_event
