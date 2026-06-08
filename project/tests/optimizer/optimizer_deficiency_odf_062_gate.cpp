// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "large_payload.hpp"
#include "nosql/key_value_api.hpp"
#include "uuid.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
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

std::string UuidText(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
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
    if (item.evidence_kind == kind && item.evidence_id == id) {
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

void NoSqlKeyValueStoresAndFetchesDescriptor() {
  const Ids ids;
  const std::string database_path = "/tmp/sb_odf_062_gate_api.sbdb";
  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
  {
    std::ofstream crud(database_path, std::ios::binary | std::ios::trunc);
    crud << "SBCRUD1\tTX_BEGIN\t77\t" << UuidText(ids.transaction_uuid) << '\n';
  }

  api::EngineKeyValuePutRequest put;
  put.context.database_path = database_path;
  put.context.local_transaction_id = 77;
  put.context.database_uuid.canonical = UuidText(ids.database_uuid);
  put.context.transaction_uuid.canonical = UuidText(ids.transaction_uuid);
  put.target_object.uuid.canonical = UuidText(ids.owner_uuid);
  put.localized_names.push_back({"en", "primary", "", "odf062-key", true});
  put.option_envelopes.push_back("large_payload.payload=" + std::string(6000, 'K'));
  put.option_envelopes.push_back("large_payload.inline_threshold=64");
  put.option_envelopes.push_back("large_payload.database_uuid=" + UuidText(ids.database_uuid));
  put.option_envelopes.push_back("large_payload.filespace_uuid=" + UuidText(ids.filespace_uuid));
  put.option_envelopes.push_back("large_payload.owner_object_uuid=" + UuidText(ids.owner_uuid));
  put.option_envelopes.push_back("large_payload.transaction_uuid=" + UuidText(ids.transaction_uuid));
  put.option_envelopes.push_back("large_payload.chunk_policy_uuid=" + UuidText(ids.chunk_policy_uuid));

  const auto put_result = api::EngineKeyValuePut(put);
  Require(put_result.ok, "ODF-062 NoSQL key/value large payload put failed");
  const auto payload = RowField(put_result, "payload");
  const auto descriptor = page::ParseLargePayloadDescriptor(payload);
  Require(descriptor.has_value(), "ODF-062 NoSQL put did not store descriptor payload");
  Require(descriptor->filespace_class == "large_blob" && descriptor->generation == 1,
          "ODF-062 NoSQL descriptor lost large_blob generation data");
  Require(EvidenceContains(put_result.evidence, "large_payload_descriptor_only", "true"),
          "ODF-062 NoSQL put lacked descriptor evidence");
  Require(EvidenceContains(put_result.evidence, "large_payload_finality_authority", "false"),
          "ODF-062 NoSQL evidence treated descriptor as finality authority");
  Require(EvidenceContains(put_result.evidence, "large_payload_visibility_authority", "false"),
          "ODF-062 NoSQL evidence treated descriptor as visibility authority");
  for (const auto& evidence : put_result.evidence) {
    Require(evidence.evidence_kind.find("uuid_ordering") == std::string::npos,
            "ODF-062 NoSQL evidence used UUID ordering as authority");
    Require(evidence.evidence_kind.find("timestamp") == std::string::npos,
            "ODF-062 NoSQL evidence used timestamp as authority");
  }

  auto second_put = put;
  second_put.option_envelopes[0] = "large_payload.payload=" + std::string(6000, 'L');
  const auto second_result = api::EngineKeyValuePut(second_put);
  Require(second_result.ok, "ODF-062 NoSQL second large payload put failed");
  const auto second_payload = RowField(second_result, "payload");
  const auto second_descriptor = page::ParseLargePayloadDescriptor(second_payload);
  Require(second_descriptor.has_value() && second_descriptor->generation == 2,
          "ODF-062 NoSQL large payload store did not advance generation");

  api::EngineKeyValueGetRequest get;
  get.context = put.context;
  get.target_object.uuid.canonical = UuidText(ids.owner_uuid);
  const auto get_result = api::EngineKeyValueGet(get);
  Require(get_result.ok, "ODF-062 NoSQL key/value get failed");
  const auto fetched = RowField(get_result, "value");
  Require(fetched == second_payload, "ODF-062 NoSQL get did not fetch latest stored descriptor");

  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
}

}  // namespace

int main() {
  AllLargeFamiliesUseBlobDescriptors();
  RoundTripInlineCacheAndPrefetch();
  GcRespectsMgaHorizonAndStaleLookupFailsClosed();
  NoSqlKeyValueStoresAndFetchesDescriptor();
  return EXIT_SUCCESS;
}
