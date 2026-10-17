// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../database_lifecycle/database_lifecycle_test_memory.hpp"
#include "large_payload.hpp"
#include "database_lifecycle.hpp"
#include "../database_lifecycle/credentialed_database_fixture.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction/local_commit_publication.hpp"
#include "hash_digest.hpp"
#include <filesystem>
#include "nosql/key_value_api.hpp"
#include "../support/native_key_value_collection_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "mga_relation_store/mga_row_version_reader.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_large_value_codec.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "uuid.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1779621000000ull + salt);
  Require(generated.ok(), "ODF-062 UUID generation failed");
  return generated.value;
}

std::string IdentityOptionBytes(const platform::Uuid& value) {
  return {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
}

std::vector<platform::byte> Bytes(std::size_t count, char value) {
  return std::vector<platform::byte>(count, static_cast<platform::byte>(value));
}

struct Ids {
  platform::TypedUuid database_uuid = NewUuid(platform::UuidKind::database, 1);
  platform::TypedUuid filespace_uuid = NewUuid(platform::UuidKind::filespace, 2);
  platform::TypedUuid owner_uuid = NewUuid(platform::UuidKind::object, 3);
  platform::TypedUuid transaction_uuid = NewUuid(platform::UuidKind::transaction, 4);
  platform::TypedUuid chunk_policy_uuid = NewUuid(platform::UuidKind::object, 5);
};

page::LargePayloadStoreRequest Request(const Ids& ids,
                                       page::LargePayloadFamily family,
                                       std::vector<platform::byte> payload,
                                       platform::u64 tx,
                                       platform::u64 owner_salt = 0) {
  page::LargePayloadStoreRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.owner_object_uuid = owner_salt == 0 ? ids.owner_uuid
                                              : NewUuid(platform::UuidKind::object, owner_salt);
  request.transaction_uuid = ids.transaction_uuid;
  request.chunk_policy_uuid = ids.chunk_policy_uuid;
  request.local_transaction_id = tx;
  request.family = family;
  request.payload_bytes = std::move(payload);
  request.inline_threshold_bytes = 128;
  request.chunk_size = 512;
  request.reason =
      "diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory";
  request.mga_write_admitted_by_transaction_inventory = true;
  return request;
}

std::string RowField(const api::EngineApiResult& result,
                     const std::string& field_name,
                     std::size_t row_index = 0) {
  Require(result.result_shape.rows.size() > row_index, "ODF-062 expected API row was missing");
  for (const auto& field : result.result_shape.rows[row_index].fields) {
    if (field.first == field_name) {
      return field.second.encoded_value;
    }
  }
  Fail("ODF-062 expected API field was missing");
}

bool EvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                      const std::string& kind,
                      const std::string& id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && std::holds_alternative<std::string>(item.evidence_id) && std::get<std::string>(item.evidence_id) == id) {
      return true;
    }
  }
  return false;
}

void RequireDiagnosticEvidenceOnly(const page::LargePayloadEvidenceRecord& evidence) {
  Require(evidence.diagnostic_only, "ODF-062 storage evidence was not marked diagnostic-only");
  Require(!evidence.finality_authority, "ODF-062 storage evidence became finality authority");
  Require(!evidence.visibility_authority, "ODF-062 storage evidence became visibility authority");
  Require(evidence.evidence_token.find("timestamp") == std::string::npos,
          "ODF-062 evidence token used timestamp finality hint");
  Require(evidence.evidence_token.find("uuid") == std::string::npos,
          "ODF-062 evidence token used UUID ordering hint");
  Require(evidence.evidence_token.find("order") == std::string::npos,
          "ODF-062 evidence token used order finality hint");
}

void AllLargeFamiliesUseBlobDescriptors() {
  const Ids ids;
  page::LargePayloadStore store;
  const std::vector<page::LargePayloadFamily> families = {
      page::LargePayloadFamily::document,
      page::LargePayloadFamily::key_value,
      page::LargePayloadFamily::vector,
      page::LargePayloadFamily::text,
      page::LargePayloadFamily::blob,
      page::LargePayloadFamily::graph,
  };

  platform::u64 salt = 100;
  for (const auto family : families) {
    auto request = Request(ids, family, Bytes(2048, 'A'), 10 + salt, salt);
    const auto stored = page::StoreLargePayloadGeneration(&store, request);
    Require(stored.ok(), "ODF-062 large payload store refused a required family");
    Require(stored.descriptor_only, "ODF-062 large payload was not descriptor-only");
    Require(!stored.descriptor.inline_payload, "ODF-062 large payload stayed inline");
    Require(stored.descriptor.filespace_class == "large_blob",
            "ODF-062 large payload did not use large_blob filespace class");
    Require(stored.descriptor.generation == 1,
            "ODF-062 first large payload generation was not one");
    const auto* record = page::FindLargePayloadGeneration(store, stored.descriptor);
    Require(record != nullptr, "ODF-062 stored generation was not findable");
    Require(record->payload_bytes.empty(),
            "ODF-062 descriptor-only generation kept a duplicate hot payload copy");
    RequireDiagnosticEvidenceOnly(stored.evidence);
    ++salt;
  }
}

void RoundTripInlineCacheAndPrefetch() {
  const Ids ids;
  page::LargePayloadStore store;

  const auto large = page::StoreLargePayloadGeneration(
      &store, Request(ids, page::LargePayloadFamily::document, Bytes(8192, 'D'), 10));
  Require(large.ok() && large.descriptor_only,
          "ODF-062 large payload descriptor setup failed");

  page::LargePayloadReadRequest read;
  read.descriptor = large.descriptor;
  read.observer_snapshot_visible_through_local_transaction_id = 10;
  read.use_cache = true;
  const auto first_read = page::ReadLargePayloadGeneration(&store, read);
  Require(first_read.ok(), "ODF-062 large payload first read failed");
  Require(!first_read.cache_hit && store.cache.miss_count == 1,
          "ODF-062 cache miss was not observable");
  Require(first_read.payload_bytes == Bytes(8192, 'D'),
          "ODF-062 large payload round trip bytes changed");
  RequireDiagnosticEvidenceOnly(first_read.evidence);

  page::LargePayloadPrefetchRequest prefetch;
  prefetch.descriptors.push_back(large.descriptor);
  prefetch.observer_snapshot_visible_through_local_transaction_id = 10;
  const auto prefetched = page::PrefetchLargePayloadGenerations(&store, prefetch);
  Require(prefetched.ok() && prefetched.loaded_count == 1,
          "ODF-062 prefetch did not load descriptor");
  Require(store.cache.prefetch_request_count != 0 && store.cache.prefetch_loaded_count != 0,
          "ODF-062 prefetch counters were not observable");
  RequireDiagnosticEvidenceOnly(prefetched.evidence);

  const auto second_read = page::ReadLargePayloadGeneration(&store, read);
  Require(second_read.ok() && second_read.cache_hit && store.cache.hit_count == 1,
          "ODF-062 cache hit was not observable after prefetch");

  auto small_request = Request(ids, page::LargePayloadFamily::key_value, Bytes(12, 's'), 11, 900);
  small_request.inline_threshold_bytes = 128;
  const auto small = page::StoreLargePayloadGeneration(&store, small_request);
  Require(small.ok(), "ODF-062 small payload store failed");
  Require(small.descriptor.inline_payload && !small.descriptor_only,
          "ODF-062 small payload could not remain inline");
  Require(small.descriptor.filespace_class == "inline_hot_payload",
          "ODF-062 inline payload was misclassified as blob");
}

void GcRespectsMgaHorizonAndStaleLookupFailsClosed() {
  const Ids ids;
  page::LargePayloadStore store;

  const auto first = page::StoreLargePayloadGeneration(
      &store, Request(ids, page::LargePayloadFamily::blob, Bytes(2048, '1'), 10));
  const auto second = page::StoreLargePayloadGeneration(
      &store, Request(ids, page::LargePayloadFamily::blob, Bytes(2048, '2'), 20));
  Require(first.ok() && second.ok(), "ODF-062 generation setup failed");

  page::LargePayloadReadRequest old_snapshot;
  old_snapshot.descriptor = first.descriptor;
  old_snapshot.observer_snapshot_visible_through_local_transaction_id = 15;
  const auto old_read = page::ReadLargePayloadGeneration(&store, old_snapshot);
  Require(old_read.ok(), "ODF-062 old MGA snapshot could not see retired generation");

  page::LargePayloadGcRequest blocked_gc;
  blocked_gc.cleanup_horizon_authoritative = true;
  blocked_gc.authoritative_mga_horizon_local_transaction_id = 20;
  const auto blocked = page::CollectLargePayloadGarbage(&store, blocked_gc);
  Require(blocked.ok() && blocked.reclaimed_count == 0 && blocked.retained_count != 0,
          "ODF-062 GC reclaimed while MGA horizon could still see older generation");

  page::LargePayloadReadRequest stale_snapshot;
  stale_snapshot.descriptor = first.descriptor;
  stale_snapshot.observer_snapshot_visible_through_local_transaction_id = 20;
  const auto stale = page::ReadLargePayloadGeneration(&store, stale_snapshot);
  Require(!stale.ok() &&
              stale.diagnostic.diagnostic_code ==
                  "large_payload_read_not_visible_fail_closed",
          "ODF-062 stale generation lookup did not fail closed");

  page::LargePayloadGcRequest allowed_gc;
  allowed_gc.cleanup_horizon_authoritative = true;
  allowed_gc.authoritative_mga_horizon_local_transaction_id = 21;
  const auto allowed = page::CollectLargePayloadGarbage(&store, allowed_gc);
  Require(allowed.ok() && allowed.reclaimed_count == 1,
          "ODF-062 GC did not reclaim after safe MGA horizon advance");
  RequireDiagnosticEvidenceOnly(allowed.evidence);

  auto invalid_descriptor = second.descriptor;
  invalid_descriptor.generation += 100;
  page::LargePayloadReadRequest invalid_read;
  invalid_read.descriptor = invalid_descriptor;
  invalid_read.observer_snapshot_visible_through_local_transaction_id = 21;
  const auto invalid = page::ReadLargePayloadGeneration(&store, invalid_read);
  Require(!invalid.ok() &&
              invalid.diagnostic.diagnostic_code ==
                  "large_payload_read_generation_not_found",
          "ODF-062 invalid generation lookup did not fail closed");

  page::LargePayloadGcRequest no_authority;
  no_authority.authoritative_mga_horizon_local_transaction_id = 100;
  const auto refused = page::CollectLargePayloadGarbage(&store, no_authority);
  Require(!refused.ok(), "ODF-062 GC ran without authoritative MGA horizon");
}


template<class Result>
void RequireOk(const Result& result, std::string_view message) {
  if (!result.ok) for (const auto& d : result.diagnostics)
    std::cerr << d.code << ':' << d.message_key << ':' << d.detail << '\n';
  Require(result.ok, message);
}

class NativeKvTransaction {
 public:
  NativeKvTransaction(const api::EngineRequestContext& base,
                      const scratchbird::tests::FixtureEngineSession& owner,
                      std::string isolation = "read_committed", bool read_only = false)
      : session(owner), context(base) {
    api::EngineBeginTransactionRequest begin;
    begin.context = base; begin.isolation_level = std::move(isolation);
    begin.transaction_policy_profile.encoded_profiles = {
        read_only ? "transaction_read_mode:read_only" : "transaction_read_mode:read_write"};
    const auto result = api::EngineBeginTransaction(begin);
    RequireOk(result, "ODF-062 actual native transaction BEGIN failed");
    context.local_transaction_id = result.local_transaction_id;
    context.transaction_uuid = result.transaction_uuid;
    context.snapshot_visible_through_local_transaction_id = result.snapshot_visible_through_local_transaction_id;
    context.transaction_isolation_level = result.isolation_level;
    context.read_only_mode = result.read_only;
  }
  ~NativeKvTransaction() {
    if (!active) return;
    api::EngineRollbackTransactionRequest rollback; rollback.context = context;
    if (!api::EngineRollbackTransaction(rollback).ok) std::terminate();
  }
  void Finish(bool commit = true) {
    if (commit) {
      api::EngineCommitTransactionRequest request; request.context = context;
      RequireOk(api::EngineCommitTransaction(request), "ODF-062 actual native COMMIT failed");
    } else {
      api::EngineRollbackTransactionRequest request; request.context = context;
      RequireOk(api::EngineRollbackTransaction(request), "ODF-062 actual native ROLLBACK failed");
    }
    active = false;
  }
  api::EngineKeyValuePutResult Put(api::EngineUuid collection, const std::string& bytes) {
    scratchbird::tests::FixtureEngineRequest<api::EngineKeyValuePutRequest> request(session, context);
    request.target_object = {collection, "table"}; request.key = "odf062-key"; request.value = bytes;
    auto result = api::EngineKeyValuePut(request);
    RequireOk(result, "ODF-062 native KV PUT failed");
    Require(RowField(result, "value") == bytes, "ODF-062 PUT replaced logical TEXT with storage metadata");
    for (const auto& evidence : result.evidence)
      Require(evidence.evidence_kind.find("uuid_ordering") == std::string::npos &&
                  evidence.evidence_kind.find("timestamp") == std::string::npos,
              "ODF-062 native KV result uses UUID/time ordering as visibility authority");
    return result;
  }
  api::EngineKeyValueGetResult Get(api::EngineUuid collection, const std::string& expected) {
    scratchbird::tests::FixtureEngineRequest<api::EngineKeyValueGetRequest> request(session, context);
    request.target_object = {collection, "table"}; request.key = "odf062-key";
    auto result = api::EngineKeyValueGet(request);
    RequireOk(result, "ODF-062 native KV GET failed");
    Require(RowField(result, "value") == expected, "ODF-062 GET did not resolve the actual snapshot's logical bytes");
    return result;
  }
  const scratchbird::tests::FixtureEngineSession& session;
  api::EngineRequestContext context;
 private:
  bool active = true;
};

api::EngineUuid NativeResultIdentity(const api::EngineApiResult& result) {
  Require(result.result_shape.rows.size() == 1, "ODF-062 actual stored row missing");
  const auto id = result.result_shape.rows[0].requested_row_uuid;
  Require(uuid::IsEngineIdentityUuid(id), "ODF-062 actual stored UUID is not UUIDv7");
  for (const auto& name : {"key_uuid", "row_uuid"}) {
    bool found = false;
    for (const auto& [field, value] : result.result_shape.rows[0].fields) if (field == name) {
      found = true;
      Require(value.descriptor.canonical_type_name == "uuid" && value.encoded_value.empty() &&
          value.binary_value.size() == 16 &&
          std::equal(value.binary_value.begin(), value.binary_value.end(), id.bytes.begin()),
          "ODF-062 key identity is not the exact actual binary row identity");
    }
    Require(found, "ODF-062 binary identity field absent");
  }
  return id;
}

struct NativeLargeWitness {
  api::EngineUuid version, previous_version, overflow;
  std::uint64_t creator = 0, checksum = 0;
};

// Observe physical bytes only after a genuine MGA-visible read identifies the
// exact row/version. A raw locator or companion record is never visibility.
NativeLargeWitness NativeLargeValue(NativeKvTransaction& transaction,
    api::EngineUuid collection, api::EngineUuid row_id, const std::string& expected) {
  scratchbird::tests::FixtureEngineStatement statement(transaction.session, transaction.context);
  const auto ordering = api::AcquireTransactionInventoryGuard(statement.context.database_path);
  api::MgaVisibleHeapRelationReadRequest request;
  request.relation_uuid = collection; request.maximum_scanned_row_versions = 128;
  request.maximum_decoded_bytes = 1024*1024; request.maximum_output_rows = 128;
  request.maximum_memory_bytes = 8*1024*1024;
  request.cancellation_requested = statement.context.query_cancellation_requested;
  Require(bool(request.cancellation_requested), "ODF-062 missing actual statement cancellation owner");
  const auto visible = api::ReadVisibleMgaHeapRelation(statement.context, request);
  Require(visible.ok && visible.visible_rows.size() == 1, "ODF-062 actual visible native row absent");
  const auto& live = visible.visible_rows.front();
  Require(live.table_uuid == collection && live.row_uuid == row_id &&
      uuid::IsEngineIdentityUuid(live.version_uuid), "ODF-062 actual visible owner identity changed");
  NativeLargeWitness witness;
  witness.version = live.version_uuid; witness.previous_version = live.previous_version_uuid;
  witness.creator = live.creator_tx;
  bool logical = false;
  for (const auto& [name, value] : live.values) if (name == "value") {
    logical = true;
    Require(value.isPresent() && value.bytes == expected,
        "ODF-062 visible reader did not materialize exact logical large TEXT");
  }
  Require(logical, "ODF-062 visible value field absent");

  api::BoundedScopedRowReadControl control;
  control.maximum_bytes = 1024*1024; control.maximum_row_versions = 128;
  control.maximum_memory_bytes = 8*1024*1024;
  control.cancellation_requested = &request.cancellation_requested;
  std::vector<api::CrudRowVersionRecord> rows; bool used = false;
  Require(api::LoadDecodedScopedRowsForTableBounded(statement.context, collection, &control, &rows, &used) &&
      used, "ODF-062 actual bounded physical row read failed");
  unsigned exact = 0;
  for (const auto& row : rows) if (row.row_uuid == row_id && row.version_uuid == witness.version) {
    ++exact;
    Require(row.table_uuid == collection && row.creator_tx == witness.creator && !row.deleted,
        "ODF-062 raw physical row owner does not match actual visible row");
    bool located = false;
    for (const auto& [name, value] : row.values) if (name == "value") {
      located = true;
      std::uint64_t size = 0;
      Require(value.state == api::EngineValueState::lob_handle && value.bytes.size() == 40 &&
          api::ReadMgaLargeValueLocator(value.bytes, &witness.overflow, &witness.checksum, &size) &&
          uuid::IsEngineIdentityUuid(witness.overflow) && size == expected.size(),
          "ODF-062 physical large value is not a native 40-byte locator");
      Require(value.bytes.find(expected.substr(0, 64)) == std::string::npos,
          "ODF-062 raw row duplicated the full cold payload");
    }
    Require(located, "ODF-062 physical value field absent");
  }
  Require(exact == 1, "ODF-062 physical row/version identity ambiguous");

  std::vector<std::string> records;
  Require(api::ReadCompleteMgaMetadataRecords(statement.context.database_path+".sb.mga_large_values", &records),
      "ODF-062 actual companion file is missing or incomplete");
  unsigned headers = 0;
  std::map<std::uint64_t, std::string> chunks;
  const auto checksum = [](std::string_view bytes) {
    std::uint64_t result = 1469598103934665603ull;
    for (unsigned char byte : bytes) { result ^= byte; result *= 1099511628211ull; }
    return result;
  };
  for (const auto& record : records) {
    std::vector<std::string> fields; api::EngineUuid identity;
    Require(api::DecodeMgaMetadataFields(record, &fields) && api::ValidateMgaLargeValueFields(fields) &&
        api::ReadMetadataUuid(fields[3], &identity), "ODF-062 invalid actual companion record");
    if (identity != witness.overflow) continue;
    std::uint64_t creator = 0;
    Require(api::LargeValueNumber(fields[2], &creator) && creator == witness.creator,
        "ODF-062 companion creator does not match actual visible version");
    if (fields[1] == "LARGE_VALUE") {
      ++headers;
      api::EngineUuid table, row, version; std::uint64_t size = 0, hash = 0;
      Require(api::ReadMetadataUuid(fields[4], &table) && table == collection &&
          api::ReadMetadataUuid(fields[5], &row) && row == row_id &&
          api::ReadMetadataUuid(fields[6], &version) && version == witness.version &&
          fields[7] == "value" && api::LargeValueNumber(fields[8], &size) && size == expected.size() &&
          api::LargeValueNumber(fields[9], &hash) && hash == witness.checksum &&
          fields[10] == "durable_uncommitted", "ODF-062 actual companion owner/header mismatch");
    } else if (fields[1] == "LARGE_VALUE_CHUNK") {
      std::uint64_t ordinal = 0, hash = 0;
      Require(api::LargeValueNumber(fields[4], &ordinal) && fields[5].size() <= 2048 &&
          api::LargeValueNumber(fields[6], &hash) && hash == checksum(fields[5]) &&
          chunks.emplace(ordinal, fields[5]).second, "ODF-062 actual chunk duplicate/checksum invalid");
    } else Fail("ODF-062 live visible version has a physical reclaim marker");
  }
  Require(headers == 1 && chunks.size() == (expected.size()+2047)/2048,
      "ODF-062 actual companion header/chunk coverage incomplete");
  std::string reconstructed;
  for (std::uint64_t i = 0; i < chunks.size(); ++i) {
    const auto chunk = chunks.find(i);
    Require(chunk != chunks.end(), "ODF-062 actual companion chunk sequence has a hole");
    reconstructed += chunk->second;
  }
  Require(reconstructed == expected && checksum(reconstructed) == witness.checksum,
      "ODF-062 actual companion payload differs from logical bytes");
  return witness;
}

void RequireCommittedPayloadBarrier(const api::EngineRequestContext& context,
    api::EngineUuid collection, api::EngineUuid row, const NativeLargeWitness& witness) {
  const auto inventory = scratchbird::storage::database::AcquireStrongLocalTransactionInventorySnapshot(
      context.database_path);
  Require(inventory.ok(), "ODF-062 actual committed native inventory unavailable");
  const auto publication = api::ClassifyLocalCommitPublicationForRecovery(context, inventory.snapshot->inventory);
  Require(publication.ok && publication.recovery_class == api::LocalCommitPublicationRecoveryClass::committed_by_inventory,
      "ODF-062 large-value publication is not committed by actual inventory");
  Require(std::any_of(publication.mutations.begin(), publication.mutations.end(),
      [&](const auto& mutation) { return mutation.object_identity == collection &&
          mutation.record_identity == row && mutation.version_identity == witness.version; }),
      "ODF-062 real commit manifest omitted actual row/version");
  const std::filesystem::path path(context.database_path + ".sb.mga_large_values");
  unsigned matches = 0;
  for (const auto& artifact : publication.artifacts)
    if (artifact.artifact_identity == path.filename().string()) {
      ++matches;
      Require(artifact.mutation_domain == "overflow" && artifact.durable_size_bytes == std::filesystem::file_size(path),
          "ODF-062 actual overflow publication extent does not cover companion");
      std::ifstream input(path, std::ios::binary);
      Require(bool(input), "ODF-062 actual synced companion cannot reopen");
      const auto digest = scratchbird::core::hash::ComputeSha256Stream(input, artifact.durable_size_bytes);
      Require(digest.ok() && scratchbird::core::hash::HexLower(digest.digest) == artifact.postcondition_sha256,
          "ODF-062 owner-bound commit manifest hash does not match actual companion extent");
    }
  Require(matches == 1, "ODF-062 commit omitted or duplicated actual companion artifact");
}

void NoSqlKeyValuePersistsNativeLargeValues() {
  const Ids ids;
  scratchbird::tests::OwnedTempDirectory directory;
  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = (directory.path()/"native_kv.sbdb").string();
  create.database_uuid = ids.database_uuid; create.filespace_uuid = ids.filespace_uuid;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = scratchbird::storage::database::CreateDatabaseFile(create);
  Require(created.ok() && created.create_finality == scratchbird::storage::database::DatabaseCreateFinalityClass::committed,
      "ODF-062 actual credentialed native database creation failed");
  auto base = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  api::EngineUuid collection, row;
  NativeLargeWitness first, second;
  const std::string initial(6000, 'K'), replacement(6000, 'L'), aborted(6000, 'M');
  {
    scratchbird::tests::FixtureEngineSession session(base);
    {
      NativeKvTransaction setup(base, session);
      collection = scratchbird::tests::PublishNativeKeyValueCollectionFixture(setup.context, "odf062_collection", 4096, 65536);
      setup.Finish();
    }
    {
      NativeKvTransaction write(base, session);
      row = NativeResultIdentity(write.Put(collection, initial));
      first = NativeLargeValue(write, collection, row, initial);
      write.Finish();
      RequireCommittedPayloadBarrier(write.context, collection, row, first);
    }
    NativeKvTransaction stable(base, session, "repeatable_read", true);
    Require(NativeResultIdentity(stable.Get(collection, initial)) == row,
        "ODF-062 initial committed large value changed row identity");
    {
      NativeKvTransaction write(base, session);
      Require(NativeResultIdentity(write.Put(collection, replacement)) == row,
          "ODF-062 native update replaced persistent row identity");
      second = NativeLargeValue(write, collection, row, replacement);
      Require(second.version != first.version && second.previous_version == first.version &&
          second.overflow != first.overflow, "ODF-062 actual large-value successor chain did not advance");
      write.Finish();
      RequireCommittedPayloadBarrier(write.context, collection, row, second);
    }
    Require(NativeLargeValue(stable, collection, row, initial).version == first.version &&
        NativeResultIdentity(stable.Get(collection, initial)) == row,
        "ODF-062 repeatable reader lost retained committed large-value predecessor");
    {
      NativeKvTransaction current(base, session);
      Require(NativeResultIdentity(current.Get(collection, replacement)) == row,
          "ODF-062 current reader missed actual committed successor");
      Require(NativeResultIdentity(current.Put(collection, replacement)) == row &&
          NativeLargeValue(current, collection, row, replacement).version == second.version,
          "ODF-062 equal-image upsert manufactured another physical large-value version");
      current.Finish();
    }
    {
      NativeKvTransaction rollback(base, session);
      rollback.Put(collection, aborted);
      Require(NativeLargeValue(rollback, collection, row, aborted).version != second.version,
          "ODF-062 rollback probe did not actually stage a distinct physical version");
      rollback.Finish(false);
    }
    stable.Finish(false);
    session.Close();
  }
  {
    // A new owned engine/session and actual BEGIN supply reopen authority.
    auto reopened_base = scratchbird::tests::BootstrapFixtureOwnerContext(create);
    scratchbird::tests::FixtureEngineSession session(reopened_base);
    NativeKvTransaction reopened(reopened_base, session, "read_committed", true);
    Require(NativeResultIdentity(reopened.Get(collection, replacement)) == row,
        "ODF-062 independent reopen lost committed value or admitted rolled-back value");
    const auto persisted = NativeLargeValue(reopened, collection, row, replacement);
    Require(persisted.version == second.version && persisted.overflow == second.overflow,
        "ODF-062 reopened actual version/overflow identity changed");
    reopened.Finish();
    session.Close();
  }
  directory.Cleanup();
}

}  // namespace

int main() try {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("optimizer_deficiency_odf_062_gate");
  AllLargeFamiliesUseBlobDescriptors();
  RoundTripInlineCacheAndPrefetch();
  GcRespectsMgaHorizonAndStaleLookupFailsClosed();
  NoSqlKeyValuePersistsNativeLargeValues();
  return EXIT_SUCCESS;
}
catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
