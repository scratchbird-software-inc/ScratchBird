// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/hot_cold_row_split_api.hpp"
#include "hot_cold_row_split.hpp"
#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/search_api.hpp"
#include "nosql/vector_api.hpp"
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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1779630000000ull + salt);
  Require(generated.ok(), "ODF-063 UUID generation failed");
  return generated.value;
}

std::string UuidText(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

api::EngineTypedValue TextValue(const std::string& value) {
  api::EngineTypedValue typed;
  typed.encoded_value = value;
  return typed;
}

std::string RowField(const api::EngineApiResult& result,
                     const std::string& field_name,
                     std::size_t row_index = 0) {
  Require(result.result_shape.rows.size() > row_index, "ODF-063 expected API row was missing");
  for (const auto& field : result.result_shape.rows[row_index].fields) {
    if (field.first == field_name) {
      return field.second.encoded_value;
    }
  }
  Fail("ODF-063 expected API field was missing");
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

void RequireEvidenceHygiene(const std::vector<api::EngineEvidenceReference>& evidence) {
  for (const auto& item : evidence) {
    Require(item.evidence_kind.find("timestamp") == std::string::npos,
            "ODF-063 evidence kind used timestamp authority");
    Require(item.evidence_id.find("timestamp") == std::string::npos,
            "ODF-063 evidence id used timestamp authority");
    Require(item.evidence_kind.find("uuid_order") == std::string::npos,
            "ODF-063 evidence kind used UUID ordering authority");
    Require(item.evidence_id.find("uuid_order") == std::string::npos,
            "ODF-063 evidence id used UUID ordering authority");
    Require(item.evidence_kind.find("wal_finality") == std::string::npos,
            "ODF-063 evidence kind used WAL finality authority");
    Require(item.evidence_kind.find("wal_visibility") == std::string::npos,
            "ODF-063 evidence kind used WAL visibility authority");
    Require(item.evidence_id.find("wal_authority=true") == std::string::npos,
            "ODF-063 evidence id used WAL authority");
    Require(item.evidence_kind.find("parser_finality") == std::string::npos,
            "ODF-063 evidence kind used parser finality authority");
    Require(item.evidence_kind.find("parser_visibility") == std::string::npos,
            "ODF-063 evidence kind used parser visibility authority");
  }
}

struct Ids {
  platform::TypedUuid database_uuid = NewUuid(platform::UuidKind::database, 1);
  platform::TypedUuid filespace_uuid = NewUuid(platform::UuidKind::filespace, 2);
  platform::TypedUuid owner_object_uuid = NewUuid(platform::UuidKind::object, 3);
  platform::TypedUuid row_uuid = NewUuid(platform::UuidKind::row, 4);
  platform::TypedUuid transaction_uuid = NewUuid(platform::UuidKind::transaction, 5);
};

page::HotColdRowSplitRequest BaseStorageRequest(page::LargePayloadStore* store,
                                                const Ids& ids,
                                                platform::u64 tx) {
  page::HotColdRowSplitRequest request;
  request.large_payload_store = store;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.owner_object_uuid = ids.owner_object_uuid;
  request.row_uuid = ids.row_uuid;
  request.transaction_uuid = ids.transaction_uuid;
  request.chunk_policy_uuid = ids.owner_object_uuid;
  request.local_transaction_id = tx;
  request.cold_threshold_bytes = 64;
  request.family = page::LargePayloadFamily::blob;
  request.engine_storage_admission_authorized = true;
  request.mga_write_admitted_by_transaction_inventory = true;
  request.fields.push_back({"row_id", "row-1", true, true, true, false, true, false});
  request.fields.push_back({"tenant_id", "tenant-a", false, true, true, false, true, false});
  request.fields.push_back({"status", "open", false, true, true, false, true, false});
  request.fields.push_back({"body", std::string(4096, 'B'), false, false, false, true, false, true});
  request.fields.push_back({"audit_blob", std::string(2048, 'A'), false, false, false, true, false, true});
  return request;
}

void StorageSplitMaterializeAndUpdate() {
  const Ids ids;
  page::LargePayloadStore store;
  auto split = page::SplitHotColdRow(BaseStorageRequest(&store, ids, 10));
  Require(split.ok(), "ODF-063 storage split failed");
  Require(split.hot_head.hot_filespace_class == "hot_row",
          "ODF-063 hot head did not use hot_row filespace class");
  Require(split.hot_head.cold_row_filespace_class == "cold_row",
          "ODF-063 cold row contract did not use cold_row filespace class");
  Require(split.hot_head.hot_fields.size() == 3,
          "ODF-063 hot head did not keep exactly selected hot fields");
  Require(split.hot_head.cold_fields.size() == 2,
          "ODF-063 cold fields were not descriptor-backed");
  const auto serialized = page::SerializeHotColdRowHead(split.hot_head);
  Require(serialized.find(std::string(128, 'B')) == std::string::npos,
          "ODF-063 hot row head leaked cold body bytes");
  for (const auto& cold : split.hot_head.cold_fields) {
    Require(cold.descriptor.filespace_class == "large_blob",
            "ODF-063 cold descriptor did not use ODF-062 large_blob path");
    Require(cold.descriptor.owner_object_uuid.value == ids.owner_object_uuid.value,
            "ODF-063 cold descriptor lost table/object owner identity");
    Require(cold.descriptor.generation_scope_uuid.value == ids.row_uuid.value,
            "ODF-063 cold descriptor did not scope generations to row identity");
    Require(cold.descriptor.generation != 0,
            "ODF-063 cold descriptor did not carry generation");
    Require(cold.descriptor_text.rfind("SB_LARGE_PAYLOAD_DESCRIPTOR_V1", 0) == 0,
            "ODF-063 cold field did not serialize an ODF-062 descriptor");
  }

  page::HotColdRowMaterializeRequest materialize;
  materialize.large_payload_store = &store;
  materialize.hot_head = split.hot_head;
  materialize.cold_field_names = {"body"};
  materialize.observer_snapshot_visible_through_local_transaction_id = 10;
  materialize.transaction_context_present = true;
  materialize.engine_storage_admission_authorized = true;
  materialize.use_cache = true;
  materialize.prefetch_on_miss = true;
  auto first = page::MaterializeColdFields(materialize);
  Require(first.ok(), "ODF-063 materialize failed after visibility check");
  Require(first.cold_fields.size() == 1 &&
              first.cold_fields.front().encoded_value == std::string(4096, 'B'),
          "ODF-063 materialized cold field bytes changed");
  Require(store.cache.prefetch_loaded_count != 0,
          "ODF-063 materialize did not expose prefetch cache evidence");
  auto second = page::MaterializeColdFields(materialize);
  Require(second.ok() && store.cache.hit_count != 0,
          "ODF-063 second materialize did not hit diagnostic cache");

  auto replacement = BaseStorageRequest(&store, ids, 20);
  replacement.row_version = 2;
  replacement.fields[3].encoded_value = std::string(4096, 'C');
  page::HotColdRowUpdateRequest update;
  update.previous_hot_head = split.hot_head;
  update.replacement = replacement;
  auto updated = page::UpdateHotColdRow(update);
  Require(updated.ok(), "ODF-063 update split failed");
  Require(!updated.retired_descriptors.empty(),
          "ODF-063 update did not retire old cold generation");

  page::HotColdRowMaterializeRequest old_snapshot = materialize;
  old_snapshot.observer_snapshot_visible_through_local_transaction_id = 15;
  auto old_read = page::MaterializeColdFields(old_snapshot);
  Require(old_read.ok() &&
              old_read.cold_fields.front().encoded_value == std::string(4096, 'B'),
          "ODF-063 old MGA snapshot could not see old cold generation");

  page::HotColdRowMaterializeRequest current_snapshot = materialize;
  current_snapshot.hot_head = updated.hot_head;
  current_snapshot.observer_snapshot_visible_through_local_transaction_id = 20;
  auto new_read = page::MaterializeColdFields(current_snapshot);
  Require(new_read.ok() &&
              new_read.cold_fields.front().encoded_value == std::string(4096, 'C'),
          "ODF-063 current MGA snapshot did not see new cold generation");

  page::HotColdRowMaterializeRequest stale = materialize;
  stale.observer_snapshot_visible_through_local_transaction_id = 20;
  auto stale_read = page::MaterializeColdFields(stale);
  Require(!stale_read.ok(), "ODF-063 stale cold descriptor did not fail closed");

  auto invalid_head = updated.hot_head;
  invalid_head.cold_fields.front().descriptor.generation += 100;
  page::HotColdRowMaterializeRequest invalid = current_snapshot;
  invalid.hot_head = invalid_head;
  auto invalid_read = page::MaterializeColdFields(invalid);
  Require(!invalid_read.ok(), "ODF-063 invalid cold generation did not fail closed");

  page::HotColdRowMaterializeRequest unsafe = current_snapshot;
  unsafe.observer_snapshot_visible_through_local_transaction_id = 0;
  auto unsafe_read = page::MaterializeColdFields(unsafe);
  Require(!unsafe_read.ok(), "ODF-063 unsafe MGA snapshot did not fail closed");

  auto unauthorized_request = BaseStorageRequest(&store, ids, 30);
  unauthorized_request.engine_storage_admission_authorized = false;
  auto unauthorized = page::SplitHotColdRow(unauthorized_request);
  Require(!unauthorized.ok(), "ODF-063 non-engine storage admission did not fail closed");
}

api::EngineDmlHotColdSplitRequest BaseDmlRequest(page::LargePayloadStore* store,
                                                 const Ids& ids,
                                                 platform::u64 tx) {
  api::EngineDmlHotColdSplitRequest request;
  request.context.database_uuid.canonical = UuidText(ids.database_uuid);
  request.context.transaction_uuid.canonical = UuidText(ids.transaction_uuid);
  request.context.local_transaction_id = tx;
  request.context.snapshot_visible_through_local_transaction_id = tx;
  request.filespace_uuid.canonical = UuidText(ids.filespace_uuid);
  request.owner_object_uuid.canonical = UuidText(ids.owner_object_uuid);
  request.row.requested_row_uuid.canonical = UuidText(ids.row_uuid);
  request.row.fields.push_back({"row_id", TextValue("row-1")});
  request.row.fields.push_back({"status", TextValue("open")});
  request.row.fields.push_back({"body", TextValue(std::string(2048, 'D'))});
  request.field_policy.push_back({"row_id", true, true, true, false, true, false});
  request.field_policy.push_back({"status", false, true, true, false, true, false});
  request.field_policy.push_back({"body", false, false, false, true, false, true});
  request.large_payload_store = store;
  request.cold_threshold_bytes = 64;
  request.engine_storage_admission_authorized = true;
  return request;
}

void DmlHelperRoutesThroughSplitModel() {
  const Ids ids;
  page::LargePayloadStore store;
  auto split = api::EngineDmlSplitHotColdRow(BaseDmlRequest(&store, ids, 10));
  Require(split.ok, "ODF-063 DML split helper failed");
  Require(split.hot_head.hot_fields.size() == 2 && split.hot_head.cold_fields.size() == 1,
          "ODF-063 DML helper did not classify hot/cold fields");
  Require(split.serialized_hot_head.find(std::string(64, 'D')) == std::string::npos,
          "ODF-063 DML serialized hot head leaked cold body");
  RequireEvidenceHygiene(split.evidence);
  Require(EvidenceContains(split.evidence, "dml_hot_cold_split_routed", "true"),
          "ODF-063 DML helper did not expose routing evidence");

  api::EngineDmlHotColdMaterializeRequest materialize;
  materialize.context = BaseDmlRequest(&store, ids, 10).context;
  materialize.hot_head = split.hot_head;
  materialize.large_payload_store = &store;
  materialize.engine_storage_admission_authorized = true;
  materialize.prefetch_on_miss = true;
  auto materialized = api::EngineDmlMaterializeColdFields(materialize);
  Require(materialized.ok && materialized.cold_values.front().second == std::string(2048, 'D'),
          "ODF-063 DML helper did not materialize cold field");
  Require(EvidenceContains(materialized.evidence, "dml_hot_cold_materialize_routed", "true"),
          "ODF-063 DML materialize helper did not route through split model");

  api::EngineDmlHotColdUpdateRequest update;
  update.previous_hot_head = split.hot_head;
  update.replacement = BaseDmlRequest(&store, ids, 20);
  update.replacement.row.fields[2].second.encoded_value = std::string(2048, 'E');
  auto updated = api::EngineDmlUpdateHotColdRow(update);
  Require(updated.ok && !updated.retired_descriptors.empty(),
          "ODF-063 DML update helper did not retire old cold descriptor");
}

api::EngineRequestContext NoSqlContext(const Ids& ids,
                                       const std::string& database_path,
                                       platform::u64 tx) {
  api::EngineRequestContext context;
  context.database_path = database_path;
  context.database_uuid.canonical = UuidText(ids.database_uuid);
  context.transaction_uuid.canonical = UuidText(ids.transaction_uuid);
  context.local_transaction_id = tx;
  return context;
}

void SeedCrudFile(const std::string& database_path, const Ids& ids, platform::u64 tx) {
  std::remove(database_path.c_str());
  std::remove((database_path + ".sb.api_events").c_str());
  std::ofstream crud(database_path, std::ios::binary | std::ios::trunc);
  crud << "SBCRUD1\tTX_BEGIN\t" << tx << "\t" << UuidText(ids.transaction_uuid) << '\n';
}

template <typename TRequest>
void AddNoSqlSplitOptions(TRequest* request,
                          const Ids& ids,
                          const std::string& object_kind,
                          const std::string& payload) {
  request->target_object.uuid.canonical = UuidText(ids.owner_object_uuid);
  request->localized_names.push_back({"en", "primary", "", object_kind + "-name", true});
  request->option_envelopes.push_back("hot_cold_split.enabled=true");
  request->option_envelopes.push_back("large_payload.payload=" + payload);
  request->option_envelopes.push_back("large_payload.inline_threshold=64");
  request->option_envelopes.push_back("large_payload.database_uuid=" + UuidText(ids.database_uuid));
  request->option_envelopes.push_back("large_payload.filespace_uuid=" + UuidText(ids.filespace_uuid));
  request->option_envelopes.push_back("large_payload.owner_object_uuid=" + UuidText(ids.owner_object_uuid));
  request->option_envelopes.push_back("large_payload.transaction_uuid=" + UuidText(ids.transaction_uuid));
  request->option_envelopes.push_back("large_payload.chunk_policy_uuid=" + UuidText(ids.owner_object_uuid));
}

void RequireNoSqlHotColdPayload(const api::EngineApiResult& result,
                                const std::string& payload_field,
                                const std::string& cold_body) {
  Require(result.ok, "ODF-063 NoSQL split API call failed");
  const auto payload = RowField(result, payload_field);
  Require(payload.rfind("SB_HOT_COLD_ROW_HEAD_V1", 0) == 0,
          "ODF-063 NoSQL surface did not persist hot/cold row head");
  Require(payload.find(cold_body.substr(0, 64)) == std::string::npos,
          "ODF-063 NoSQL hot head leaked full cold payload");
  Require(EvidenceContains(result.evidence, "hot_cold_split_routed", "true"),
          "ODF-063 NoSQL surface did not route through split helper");
  Require(EvidenceContains(result.evidence, "hot_cold_split_hot_filespace_class", "hot_row"),
          "ODF-063 NoSQL hot filespace class evidence missing");
  Require(EvidenceContains(result.evidence, "hot_cold_split_cold_row_filespace_class", "cold_row"),
          "ODF-063 NoSQL cold row class evidence missing");
  Require(EvidenceContains(result.evidence, "hot_cold_split_cold_descriptor_filespace_class", "large_blob"),
          "ODF-063 NoSQL cold descriptor did not use large_blob path");
  RequireEvidenceHygiene(result.evidence);
}

void NoSqlSurfacesRouteThroughSplitModel() {
  const Ids ids;
  const std::string base_path = "/tmp/sb_odf_063_gate_api";
  const std::string body(2048, 'N');

  {
    const std::string path = base_path + "_kv.sbdb";
    SeedCrudFile(path, ids, 77);
    api::EngineKeyValuePutRequest request;
    request.context = NoSqlContext(ids, path, 77);
    AddNoSqlSplitOptions(&request, ids, "key_value", body);
    const auto result = api::EngineKeyValuePut(request);
    RequireNoSqlHotColdPayload(result, "payload", body);
    std::remove(path.c_str());
    std::remove((path + ".sb.api_events").c_str());
  }
  {
    const std::string path = base_path + "_doc.sbdb";
    SeedCrudFile(path, ids, 78);
    api::EngineDocumentInsertRequest request;
    request.context = NoSqlContext(ids, path, 78);
    AddNoSqlSplitOptions(&request, ids, "document", body);
    const auto result = api::EngineDocumentInsert(request);
    RequireNoSqlHotColdPayload(result, "payload", body);
    std::remove(path.c_str());
    std::remove((path + ".sb.api_events").c_str());
  }
  {
    const std::string path = base_path + "_vector.sbdb";
    SeedCrudFile(path, ids, 79);
    api::EngineVectorWriteRequest request;
    request.context = NoSqlContext(ids, path, 79);
    AddNoSqlSplitOptions(&request, ids, "vector", body);
    const auto result = api::EngineVectorWrite(request);
    RequireNoSqlHotColdPayload(result, "payload", body);
    std::remove(path.c_str());
    std::remove((path + ".sb.api_events").c_str());
  }
  {
    const std::string path = base_path + "_graph.sbdb";
    SeedCrudFile(path, ids, 80);
    api::EngineGraphWriteRequest request;
    request.context = NoSqlContext(ids, path, 80);
    AddNoSqlSplitOptions(&request, ids, "graph", body);
    const auto result = api::EngineGraphWrite(request);
    RequireNoSqlHotColdPayload(result, "payload", body);
    std::remove(path.c_str());
    std::remove((path + ".sb.api_events").c_str());
  }
  {
    const std::string path = base_path + "_search.sbdb";
    SeedCrudFile(path, ids, 81);
    api::EngineSearchQueryRequest request;
    request.context = NoSqlContext(ids, path, 81);
    request.target_object.uuid.canonical = UuidText(ids.owner_object_uuid);
    AddNoSqlSplitOptions(&request, ids, "search", body);
    const auto result = api::EngineSearchQuery(request);
    RequireNoSqlHotColdPayload(result, "payload", body);
    std::remove(path.c_str());
    std::remove((path + ".sb.api_events").c_str());
  }
}

}  // namespace

int main() {
  StorageSplitMaterializeAndUpdate();
  DmlHelperRoutesThroughSplitModel();
  NoSqlSurfacesRouteThroughSplitModel();
  return EXIT_SUCCESS;
}
