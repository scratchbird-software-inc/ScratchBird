#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/published_ddl_table_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/metric_projection_fixture.hpp"
#include "database_lifecycle_test_memory.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "ddl/alter_api.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "dml/transactional_relation_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "mga_relation_store/mga_contextual_text_descriptor.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "query/contextual_text_policy_registry_v2.hpp"
#include "metric_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "catalog/constraint_metadata_codec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

static_assert(!std::is_constructible_v<api::CrudState,
                                       api::RelationReadSnapshot>,
              "compatibility read snapshots must not feed mutable CRUD state");

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::u64 MillisSeed() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, [[maybe_unused]] platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, MillisSeed());
  Require(generated.ok(), "IPAR relation-state UUID generation failed");
  return generated.value;
}

api::EngineUuid NewIdentity(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(item.evidence_id, id)) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind, const api::EngineUuid& identity) {
  for (const auto& item : evidence) {
    const auto* value = std::get_if<api::EngineUuid>(&item.evidence_id);
    if (item.evidence_kind == kind && value && *value == identity) return true;
  }
  return false;
}

const api::TransactionalRelationStoreAuthorityRecord* FindAuthority(
    std::string_view artifact) {
  for (const auto& record : api::TransactionalRelationStoreAuthorityMap()) {
    if (record.artifact == artifact) {
      return &record;
    }
  }
  return nullptr;
}

void VerifyCanonicalStoreAuthorityMap() {
  const auto* rows = FindAuthority("row_versions");
  Require(rows != nullptr && rows->classification == "canonical_durable",
          "canonical row-version authority is not explicit");
  const auto* finality = FindAuthority("transaction_finality");
  Require(finality != nullptr &&
              finality->authority == "durable_transaction_inventory" &&
              finality->classification == "canonical_durable",
          "durable transaction inventory finality authority is not explicit");
  const auto* cache = FindAuthority("relation_caches");
  Require(cache != nullptr &&
              cache->classification == "derived_non_authoritative",
          "relation cache was not classified as derived");
  const auto* compatibility = FindAuthority("crud_compatibility_state");
  Require(compatibility != nullptr &&
              compatibility->classification ==
                  "read_only_subordinate_projection",
          "compatibility projection was not classified as read-only/subordinate");
}

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      const auto* text = std::get_if<std::string>(&item.evidence_id);
      Require(text != nullptr, "expected scalar text evidence");
      return *text;
    }
  }
  return {};
}

bool HasMetric(const std::vector<metrics::MetricValue>& values,
               std::string_view family,
               const api::EngineUuid& object_uuid,
               std::string_view operation,
               std::string_view result,
               std::uint64_t minimum_value) {
  for (const auto& value : values) {
    const auto* count = std::get_if<std::uint64_t>(&value.value);
    if (value.family != family || !count || *count < minimum_value) { continue; }
    bool object_matches = false;
    bool operation_matches = false;
    bool result_matches = false;
    for (const auto& label : value.labels) {
      object_matches = object_matches ||
                       (label.key == "object_uuid" &&
                        std::get_if<metrics::MetricUuid>(&label.value) &&
                        *std::get_if<metrics::MetricUuid>(&label.value) == object_uuid);
      operation_matches = operation_matches ||
                          (label.key == "operation" &&
                           std::get_if<std::string>(&label.value) &&
                           *std::get_if<std::string>(&label.value) == operation);
      result_matches = result_matches ||
                       (label.key == "result" && std::get_if<std::string>(&label.value) &&
                        *std::get_if<std::string>(&label.value) == result);
    }
    if (object_matches && operation_matches && result_matches) { return true; }
  }
  return false;
}

void RequireSameRows(std::vector<api::CrudRowVersionRecord> left,
                     std::vector<api::CrudRowVersionRecord> right) {
  const auto order = [](const api::CrudRowVersionRecord& lhs,
                        const api::CrudRowVersionRecord& rhs) {
    return std::tie(lhs.row_uuid, lhs.version_uuid, lhs.event_sequence) <
           std::tie(rhs.row_uuid, rhs.version_uuid, rhs.event_sequence);
  };
  std::sort(left.begin(), left.end(), order);
  std::sort(right.begin(), right.end(), order);
  Require(left.size() == right.size(),
          "IPAR scoped/full parity row count differs");
  for (std::size_t i = 0; i < left.size(); ++i) {
    Require(left[i].creator_tx == right[i].creator_tx &&
                left[i].event_sequence == right[i].event_sequence &&
                left[i].sequence == right[i].sequence &&
                left[i].table_uuid == right[i].table_uuid &&
                left[i].row_uuid == right[i].row_uuid &&
                left[i].version_uuid == right[i].version_uuid &&
                left[i].previous_version_uuid == right[i].previous_version_uuid &&
                left[i].previous_sequence == right[i].previous_sequence &&
                left[i].deleted == right[i].deleted &&
                left[i].values == right[i].values,
            "IPAR scoped/full parity row image differs");
  }
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view detail) {
  const std::string needle(detail);
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail == needle ||
        diagnostic.detail.find(needle) != std::string::npos ||
        diagnostic.code.find(needle) != std::string::npos ||
        diagnostic.message_key.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void DumpDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
}

struct Fixture {
  std::shared_ptr<scratchbird::tests::OwnedTempDirectory> temporary =
      std::make_shared<scratchbird::tests::OwnedTempDirectory>();
  std::filesystem::path dir;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineUuid target_table_uuid;
  api::EngineUuid child_table_uuid;
  api::EngineUuid unrelated_table_uuid;
  api::EngineUuid target_index_uuid;
  api::EngineUuid schema_uuid;
  platform::u64 salt = 0;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string value, std::string note = {}) {
  api::EngineRowValue row;
  row.fields.push_back({"payload", TextValue(std::move(value))});
  if (!note.empty()) {
    row.fields.push_back({"note", TextValue(std::move(note))});
  }
  return row;
}

api::CrudTableRecord Table(api::EngineUuid table_uuid, std::string name,
                           std::uint64_t creator_tx,
                           bool primary_key = false) {
  api::CrudTableRecord table;
  table.creator_tx = creator_tx;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(name);
  table.columns.push_back({"payload",
                           primary_key ? "canonical=character;primary_key=true"
                                       : "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudTableRecord ChildTable(const Fixture& fixture, std::uint64_t creator_tx) {
  api::CrudTableRecord table;
  table.creator_tx = creator_tx;
  table.table_uuid = fixture.child_table_uuid;
  table.default_name = "ipar_relation_state_child";
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

api::EngineAlterConstraintResult PublishForeignKey(
    const Fixture& fixture, const api::EngineRequestContext& context, bool require_success = true) {
  const auto child = api::LoadMgaRelationStorageDescriptor(context, fixture.child_table_uuid);
  const auto parent = api::LoadMgaRelationStorageDescriptor(context, fixture.target_table_uuid);
  Require(child.ok && parent.ok && child.descriptor.columns.size() == 1 &&
              parent.descriptor.columns.size() == 2,
          "IPAR foreign key requires published relation cohorts");
  api::CatalogColumnMetadata fields;
  fields.text = {{"descriptor_version", "neutral_fk_single_column_v1"},
      {"child_relation_descriptor_generation", std::to_string(child.descriptor.descriptor_generation)},
      {"parent_relation_descriptor_generation", std::to_string(parent.descriptor.descriptor_generation)},
      {"referenced_column", "payload"}, {"child_column", "payload"},
      {"constraint_name_quoted", "false"}, {"on_update", "no_action"},
      {"on_delete", "no_action"}, {"referential_action", "no_action"},
      {"enforcement_timing", "immediate"}, {"deferrable", "false"}};
  fields.identities = {{"child_table_uuid", fixture.child_table_uuid},
      {"child_column_uuid", child.descriptor.columns.front().column_uuid},
      {"child_relation_descriptor_uuid", child.descriptor.descriptor_uuid},
      {"parent_table_uuid", fixture.target_table_uuid},
      {"parent_column_uuid", parent.descriptor.columns.front().column_uuid},
      {"parent_relation_descriptor_uuid", parent.descriptor.descriptor_uuid},
      {"referenced_table_uuid", fixture.target_table_uuid},
      {"referenced_column_uuid", parent.descriptor.columns.front().column_uuid}};
  api::EngineAlterConstraintRequest request;
  request.context = context;
  request.target_object.uuid = fixture.child_table_uuid;
  request.target_object.object_kind = "table";
  api::EngineConstraintDefinition definition;
  definition.constraint_kind = "foreign_key";
  definition.names.push_back({"en", "primary", "", "ipar_child_parent_fk", true});
  Require(api::EncodeCatalogConstraintMetadata(fields, &definition.canonical_constraint_envelope),
          "IPAR binary foreign-key descriptor encoding failed");
  request.constraints.push_back(std::move(definition));
  const auto published = api::EngineAlterConstraint(request);
  if (require_success) {
    if (!published.ok) DumpDiagnostics(published);
    Require(published.ok, "IPAR foreign-key cohort publication failed");
  }
  return published;
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  auto context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.current_schema_uuid = fixture.schema_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "IPAR relation-state begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) {
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "IPAR relation-state commit failed");
}

Fixture MakeFixture(bool publish_foreign_key = true) {
  Fixture fixture;
  fixture.salt = MillisSeed();
  fixture.dir = fixture.temporary->path();
  fixture.database_path = fixture.dir / "ipar_relation_state.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = MillisSeed();
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR relation-state database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.target_table_uuid = NewIdentity(platform::UuidKind::object, fixture.salt + 20);
  fixture.child_table_uuid = NewIdentity(platform::UuidKind::object, fixture.salt + 21);
  fixture.unrelated_table_uuid = NewIdentity(platform::UuidKind::object, fixture.salt + 30);

  auto metadata = Begin(fixture, "ipar-relation-state-metadata");
  const auto target = scratchbird::tests::PublishDdlTableFixture(
      metadata,
      Table(fixture.target_table_uuid, "ipar_relation_state_target",
            metadata.local_transaction_id, true), {"character", "character"});
  Require(target.table_uuid == fixture.target_table_uuid && target.columns.size() == 2,
          "IPAR relation-state target publication changed identity or shape");
  api::CatalogColumnMetadata primary;
  Require(api::DecodeCatalogColumnMetadata(target.columns.front().second, &primary) &&
              primary.identities.contains("candidate_key_constraint_uuid") &&
              primary.identities.contains("support_uuid"),
          "IPAR relation-state published primary-key identities missing");
  fixture.target_index_uuid = primary.identities.at("support_uuid");
  fixture.schema_uuid = metadata.current_schema_uuid;
  const auto child = scratchbird::tests::PublishDdlTableFixture(
      metadata,
      ChildTable(fixture, metadata.local_transaction_id), {"character"});
  Require(child.table_uuid == fixture.child_table_uuid && child.columns.size() == 1,
          "IPAR relation-state child publication changed identity or shape");
  if (publish_foreign_key) PublishForeignKey(fixture, metadata);
  const auto unrelated = scratchbird::tests::PublishMgaTableFixture(
      metadata,
      Table(fixture.unrelated_table_uuid, "ipar_relation_state_unrelated",
            metadata.local_transaction_id), {"character", "character"});
  Require(!unrelated.error, "IPAR relation-state unrelated metadata append failed");
  Commit(metadata);
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(fixture.owner_context);
  return fixture;
}

api::EngineInsertRowsResult InsertRows(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       const api::EngineUuid& table_uuid,
                                       std::vector<api::EngineRowValue> rows,
                                       std::vector<std::string> options = {}) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.session, context);
  request.target_schema.uuid = fixture.schema_uuid;
  request.target_table.uuid = table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid = table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = rows.size();
  request.input_rows = std::move(rows);
  const auto descriptor = api::LoadMgaRelationStorageDescriptor(request.context, table_uuid);
  if (!descriptor.ok)
    std::cerr << descriptor.diagnostic.code << ':' << descriptor.diagnostic.detail << '\n';
  Require(descriptor.ok, "IPAR insert requires its fresh published descriptor");
  for (auto& row : request.input_rows) for (auto& field : row.fields) {
    const auto column = std::find_if(descriptor.descriptor.columns.begin(), descriptor.descriptor.columns.end(),
                                    [&](const auto& item) { return item.canonical_name_key == field.first; });
    Require(column != descriptor.descriptor.columns.end(), "IPAR insert column is not in its catalog cohort");
    field.second.descriptor = column->value_descriptor;
  }
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

api::EngineInsertRowsResult InsertInto(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       const api::EngineUuid& table_uuid,
                                       std::string payload,
                                       std::vector<std::string> options = {},
                                       std::string note = {}) {
  return InsertRows(fixture, context, table_uuid,
                    {Row(std::move(payload), std::move(note))}, std::move(options));
}

void RequireInsertOk(const api::EngineInsertRowsResult& result,
                     std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

api::MgaVisibleContextualTextSidecarSnapshotV2 ReadChildSidecar(
    const Fixture& fixture, const api::EngineRequestContext& context) {
  const auto descriptor = api::LoadMgaRelationStorageDescriptor(context, fixture.child_table_uuid);
  if (!descriptor.ok) std::cerr << descriptor.diagnostic.detail << '\n';
  Require(descriptor.ok, "FK child physical descriptor unavailable");
  const auto snapshot = api::LoadVisibleMgaContextualTextSidecarSnapshotV2(
      context, fixture.child_table_uuid, descriptor.descriptor.descriptor_uuid,
      descriptor.descriptor.descriptor_generation);
  if (!snapshot.ok) std::cerr << snapshot.diagnostic.detail << '\n';
  Require(snapshot.ok, "FK child contextual sidecar cohort unavailable");
  const auto policy = api::LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();
  Require(policy.ok, "FK contextual policy unavailable");
  api::MgaContextualTextProjectionMaterialV2 projection;
  api::EngineApiDiagnostic projection_diagnostic;
  Require(api::BuildMgaContextualTextProjectionMaterialV2(context, descriptor.descriptor,
              policy.rows, &projection, &projection_diagnostic),
          "FK reopened contextual projection invalid");
  api::MgaContextualTextSidecarSetDiagnosticV2 seal_diagnostic;
  Require(api::ValidateMgaContextualTextSidecarSetV2(snapshot.snapshot.owner,
              snapshot.snapshot.base_descriptor_fields, projection.projected_columns,
              snapshot.snapshot.sealed_sidecar_set, &seal_diagnostic),
          "FK contextual sidecar complete-vector seal is invalid");
  return snapshot.snapshot;
}

// Decode the published fixture record for adversarial rehashing. This helper
// confers no authority; each candidate goes through the production reader.
api::MgaConstraintMutationBatch BatchFromFields(const std::vector<std::string>& f) {
  Require(f.size() == 47, "expected complete v2 fixture record");
  api::MgaConstraintMutationBatch b;
  b.format_version = f[4]; b.batch_hash = f[7]; b.mutation_count = std::stoul(f[8]);
  for (const auto& [index, output] : std::vector<std::pair<std::size_t, api::EngineUuid*>>{
      {5,&b.batch_uuid},{9,&b.database_uuid},{10,&b.constraint_uuid},{11,&b.owner_table_uuid},
      {12,&b.child_schema_uuid},{13,&b.child_relation_descriptor_uuid},{15,&b.child_column_uuid},
      {16,&b.parent_table_uuid},{17,&b.parent_schema_uuid},{18,&b.parent_relation_descriptor_uuid},
      {20,&b.parent_column_uuid},{21,&b.parent_candidate_key_constraint_uuid},{22,&b.key_descriptor_uuid},
      {23,&b.support_uuid},{36,&b.updated_table.table_uuid},{41,&b.updated_table.temporary_session_uuid}})
    Require(api::ReadMetadataUuid(f[index], output, index == 41), "fixture binary identity decode failed");
  b.child_relation_descriptor_generation = std::stoull(f[14]);
  b.parent_relation_descriptor_generation = std::stoull(f[19]);
  b.support_family=f[24]; b.support_policy=f[25]; b.match_policy=f[26];
  b.on_update_action=f[27]; b.on_delete_action=f[28]; b.enforcement_timing=f[29];
  b.constraint_metadata_generation=std::stoull(f[30]);
  b.base_table_event_sequence=std::stoull(f[31]); b.parent_base_table_event_sequence=std::stoull(f[32]);
  b.constraint_name=f[33]; b.constraint_kind=f[34]; b.canonical_constraint_envelope=f[35];
  b.updated_table.default_name=f[37]; b.updated_table.temporary=f[39]=="1";
  b.updated_table.temporary_scope=f[40]; b.updated_table.on_commit_action=f[42];
  b.descriptor_field_count=std::stoull(f[43]); b.descriptor_field_bytes=std::stoull(f[44]);
  b.contextual_sidecar_count=std::stoul(f[45]);
  Require(api::DecodeMetadataPairs(f[38], &b.updated_table.columns) &&
          api::DecodeMetadataPairs(f[46], &b.sealed_descriptor_fields), "fixture pair decoding failed");
  return b;
}

void VerifyRehashedConstraintRefusals(const Fixture& fixture,
    const api::EngineRequestContext& context, const std::vector<std::string>& records,
    std::size_t selected) {
  std::vector<std::string> fields;
  Require(api::DecodeMgaMetadataFields(records[selected], &fields), "fixture frame decode failed");
  const auto original = BatchFromFields(fields);
  const auto creator = std::stoull(fields[2]), event = std::stoull(fields[3]);
  Require(api::ConstraintMutationBatchSha256(original, creator, event) == original.batch_hash,
          "fixture reconstruction changed the original hash");
  // Replay against the real resource/catalog cohort. A metadata-only copied
  // path cannot establish charset authority. This fixture is private and
  // quiescent; restore its exact journal even when a negative assertion throws.
  struct RestoreJournal {
    std::string path;
    const std::vector<std::string>& records;
    bool active = true;
    bool Restore() {
      std::ofstream out(path, std::ios::binary|std::ios::trunc);
      for (const auto& frame : records) out.write(frame.data(), frame.size());
      out.flush();
      return out.good();
    }
    ~RestoreJournal() {
      if (active && !Restore()) std::cerr << "cannot restore adversarial fixture journal\n";
    }
  } restore{fixture.database_path.string()+".sb.mga_relation_metadata", records};
  for (unsigned variant = 0; variant != 5; ++variant) {
    auto candidate = original;
    const char* expected = "constraint_sealed_descriptor_invalid";
    if (variant == 0) {
      candidate.sealed_descriptor_fields.clear(); candidate.descriptor_field_count = 0;
    } else if (variant == 1) {
      for (auto& [key, value] : candidate.sealed_descriptor_fields)
        if (key == "relation_generation") value = "0";
    } else if (variant == 2 || variant == 3) {
      auto descriptor = api::DeserializeMgaRelationStorageDescriptor(candidate.sealed_descriptor_fields);
      if (variant == 2) ++descriptor.root_page_number;
      else ++descriptor.allocation_root_page_number;
      auto table = candidate.updated_table;
      table.creator_tx=creator; table.event_sequence=event;
      table.bound_relation_generation=descriptor.relation_generation;
      const auto policy=api::LoadCurrentEngineContextualTextPolicyRowSetForPublicationV2();
      Require(policy.ok, "adversarial fixture policy unavailable");
      api::MgaSealedContextualTextDescriptorMaterialV2 material;
      api::EngineApiDiagnostic diagnostic;
      Require(api::BuildMgaSealedContextualTextDescriptorMaterialV2(
          context, table, descriptor, policy.rows, &material, &diagnostic), "adversarial reseal failed");
      candidate.sealed_descriptor_fields.clear();
      for (const auto& f : material.sealed_set.descriptor_fields)
        candidate.sealed_descriptor_fields.emplace_back(
            std::string(f.key_raw_bytes.begin(), f.key_raw_bytes.end()),
            std::string(f.value_raw_bytes.begin(), f.value_raw_bytes.end()));
      candidate.descriptor_field_count=material.sealed_set.descriptor_field_count;
      candidate.descriptor_field_bytes=material.sealed_set.descriptor_field_bytes;
      expected = "constraint_physical_descriptor_changed";
    } else {
      candidate.sealed_descriptor_fields.back().second[0] ^= 1;
      expected = "constraint_contextual_descriptor_seal_invalid";
    }
    candidate.batch_hash=api::ConstraintMutationBatchSha256(candidate,creator,event);
    const auto altered=api::EncodeMgaMetadataFields(api::ConstraintMutationBatchLineFields(candidate,creator,event));
    {
      std::ofstream out(restore.path, std::ios::binary|std::ios::trunc);
      for (std::size_t i=0; i<records.size(); ++i) {
        const auto& frame = i == selected ? altered : records[i];
        out.write(frame.data(), frame.size());
      }
      Require(out.good(), "adversarial fixture write failed");
    }
    api::RelationReadSnapshot state;
    const auto result=api::LoadMgaMetadata(&state,context);
    if (!result.error || result.detail.find(expected)==std::string::npos)
      std::cerr << "variant=" << variant << " " << result.code << ":" << result.detail << '\n';
    Require(result.error && result.detail.find(expected)!=std::string::npos,
            "rehashed constraint record escaped its exact replay refusal");
    Require(state.tables.empty() && state.sealed_relation_descriptor_snapshots.empty(),
            "failed replay published partial metadata");
    Require(restore.Restore(), "adversarial journal restoration failed");
    api::RelationReadSnapshot restored;
    Require(!api::LoadMgaMetadata(&restored,context).error,
            "original metadata failed after adversarial journal restoration");
  }
  restore.active = false;
}

void VerifyForeignKeyDescriptorLifecycle() {
  auto fixture = MakeFixture(false);
  auto writer = Begin(fixture, "fk-cohort-writer");
  auto sibling = Begin(fixture, "fk-cohort-sibling");
  const auto original = ReadChildSidecar(fixture, writer);
  Require(!api::CreateMgaSavepointMarker(writer, "before_fk").error,
          "FK savepoint creation failed");
  // The current generic catalog producer has no savepoint admission. The
  // descriptor-cohort repair must preserve that guard, not create a waiver.
  const auto refused = PublishForeignKey(fixture, writer, false);
  Require(!refused.ok && HasDiagnostic(refused, "SBLR.OPERATION_UNSUPPORTED") &&
              ReadChildSidecar(fixture, writer).owner == original.owner,
          "FK descriptor repair bypassed catalog savepoint admission");
  Require(!api::RollbackToMgaSavepointMarker(writer, "before_fk").error,
          "FK savepoint rollback failed");
  Require(!api::ReleaseMgaSavepointMarker(writer, "before_fk").error,
          "FK savepoint release failed");

  // Use real filesystem refusal at each publication boundary, not caller
  // options asserting a pretend failure. Restore modes even on exceptions.
  class ReadOnlyFile final {
   public:
    explicit ReadOnlyFile(std::filesystem::path path) : path_(std::move(path)),
        permissions_(std::filesystem::status(path_).permissions()) {
      std::filesystem::permissions(path_, std::filesystem::perms::owner_write |
          std::filesystem::perms::group_write | std::filesystem::perms::others_write,
          std::filesystem::perm_options::remove);
    }
    ~ReadOnlyFile() {
      std::error_code error;
      std::filesystem::permissions(path_, permissions_, error);
      if (error) std::cerr << "cannot restore fixture permissions: " << error.message() << '\n';
    }
   private:
    std::filesystem::path path_;
    std::filesystem::perms permissions_;
  };
  const auto metadata_path=fixture.database_path.string()+".sb.mga_relation_metadata";
  const auto allocator_path=fixture.database_path.string()+".sb.mga_event_sequence_allocator";
  const auto metadata_size=std::filesystem::file_size(metadata_path);
  const auto metadata_records = [&] {
    std::vector<std::string> records;
    Require(api::ReadCompleteMgaMetadataRecords(metadata_path, &records),
            "fault fixture metadata is unreadable");
    return records;
  };
  const auto before_failure_records=metadata_records();
  {
    const auto failure = [&] {
      ReadOnlyFile guard(allocator_path);
      return PublishForeignKey(fixture, writer, false);
    }();
    if (failure.ok || !HasDiagnostic(failure,"sealed_batch_allocator_append_failed")) DumpDiagnostics(failure);
    Require(!failure.ok && HasDiagnostic(failure,"sealed_batch_allocator_append_failed") &&
            std::filesystem::file_size(metadata_path)==metadata_size && metadata_records()==before_failure_records,
            "allocator refusal exposed constraint metadata");
    Require(ReadChildSidecar(fixture,writer).owner==original.owner,
            "allocator refusal changed descriptor visibility");
  }
  const auto failed_event=api::NextMetadataEventSequence(writer);
  {
    const auto failure = [&] {
      ReadOnlyFile guard(metadata_path);
      return PublishForeignKey(fixture, writer, false);
    }();
    if (failure.ok || !HasDiagnostic(failure,"sealed_batch_append_failed")) DumpDiagnostics(failure);
    Require(!failure.ok && HasDiagnostic(failure,"sealed_batch_append_failed") &&
            std::filesystem::file_size(metadata_path)==metadata_size && metadata_records()==before_failure_records,
            "metadata append refusal reported or exposed successful publication");
    api::ClearMgaEventSequenceRangeCacheForTesting();
    Require(api::NextMetadataEventSequence(writer)>failed_event &&
            ReadChildSidecar(fixture,writer).owner==original.owner,
            "allocator reload reused the failed event or published its descriptor");
  }
  PublishForeignKey(fixture, writer);
  const auto staged = ReadChildSidecar(fixture, writer);
  Require(staged.owner.event_sequence > original.owner.event_sequence &&
              staged.owner.creator_transaction_id == writer.local_transaction_id &&
              staged.owner.relation_descriptor_uuid == original.owner.relation_descriptor_uuid &&
              staged.owner.relation_descriptor_generation == original.owner.relation_descriptor_generation &&
              staged.sealed_sidecar_set.seal_sha256 != original.sealed_sidecar_set.seal_sha256,
          "FK metadata did not atomically reseal the unchanged physical descriptor");
  Require(ReadChildSidecar(fixture, sibling).owner == original.owner,
          "uncommitted FK descriptor leaked to a sibling transaction");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = writer;
  Require(api::EngineRollbackTransaction(rollback).ok, "FK transaction rollback failed");
  Require(ReadChildSidecar(fixture, sibling).owner == original.owner,
          "rolled-back FK descriptor leaked to a sibling transaction");
  Commit(sibling);

  writer = Begin(fixture, "fk-cohort-commit");
  PublishForeignKey(fixture, writer);
  const auto committed = ReadChildSidecar(fixture, writer);
  Commit(writer);
  fixture.session.reset();
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(fixture.owner_context);
  auto reopened = Begin(fixture, "fk-cohort-reopened");
  const auto recovered = ReadChildSidecar(fixture, reopened);
  Require(recovered.owner == committed.owner &&
              recovered.sealed_sidecar_set.descriptor_fields == committed.sealed_sidecar_set.descriptor_fields,
          "committed FK descriptor changed or disappeared after engine reopen");
  std::vector<std::string> records;
  Require(api::ReadCompleteMgaMetadataRecords(
              fixture.database_path.string() + ".sb.mga_relation_metadata", &records),
          "reopened constraint publication journal is unreadable");
  std::size_t committed_records = 0;
  std::size_t committed_record_index = 0;
  for (const auto& record : records) {
    std::vector<std::string> fields;
    Require(api::DecodeMgaMetadataFields(record, &fields), "constraint journal frame is malformed");
    if (fields.size() < 4 || fields[1] != "CONSTRAINT_MUTATION_BATCH" ||
        fields[2] != std::to_string(committed.owner.creator_transaction_id)) continue;
    ++committed_records;
    committed_record_index = &record - records.data();
    Require(fields.size() == 47 && fields[4] == "neutral_fk_mutation_batch_v2" &&
                fields[3] == std::to_string(committed.owner.event_sequence) &&
                fields[43] == std::to_string(committed.sealed_sidecar_set.descriptor_field_count) &&
                fields[44] == std::to_string(committed.sealed_sidecar_set.descriptor_field_bytes) &&
                fields[45] == std::to_string(committed.sealed_sidecar_set.contextual_sidecar_count),
            "FK successor is not one complete v2 publication record");
    std::vector<std::pair<std::string, std::string>> descriptor_fields;
    Require(api::DecodeMetadataPairs(fields[46], &descriptor_fields) &&
                descriptor_fields.size() == committed.sealed_sidecar_set.descriptor_fields.size(),
            "FK journal omitted the complete binary descriptor vector");
    for (std::size_t i = 0; i < descriptor_fields.size(); ++i) {
      const auto& expected = committed.sealed_sidecar_set.descriptor_fields[i];
      Require(descriptor_fields[i].first == std::string(expected.key_raw_bytes.begin(), expected.key_raw_bytes.end()) &&
                  descriptor_fields[i].second == std::string(expected.value_raw_bytes.begin(), expected.value_raw_bytes.end()),
              "FK journal descriptor differs from the reopened admitted cohort");
    }
  }
  Require(committed_records == 1, "FK commit did not retain exactly one atomic publication record");
  VerifyRehashedConstraintRefusals(fixture, reopened, records, committed_record_index);
  RequireInsertOk(InsertInto(fixture, reopened, fixture.target_table_uuid, "reopened-parent"),
                  "reopened FK parent native insert failed");
  RequireInsertOk(InsertInto(fixture, reopened, fixture.child_table_uuid, "reopened-parent"),
                  "reopened FK child native insert failed");
  const auto orphan = InsertInto(fixture, reopened, fixture.child_table_uuid, "absent-parent");
  Require(!orphan.ok && HasDiagnostic(orphan, "FOREIGN_KEY"),
          "reopened FK accepted an orphan or lost its constraint diagnostic");
  Commit(reopened);
}

void VerifyRelationStateLoadRoutes() {
  auto fixture = MakeFixture();

  auto seed = Begin(fixture, "ipar-relation-state-seed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.target_table_uuid, "target-1", {}, "seed"),
                  "IPAR relation-state target seed insert failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.child_table_uuid, "target-1"),
                  "IPAR relation-state child seed insert failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.unrelated_table_uuid, "unrelated-1"),
                  "IPAR relation-state unrelated seed insert 1 failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.unrelated_table_uuid, "unrelated-2"),
                  "IPAR relation-state unrelated seed insert 2 failed");
  Commit(seed);

  auto verify = Begin(fixture, "ipar-relation-state-verify");
  const auto diagnostic_full_load = [&] {
    scratchbird::tests::FixtureEngineStatement statement(*fixture.session, verify);
    api::TransactionalRelationStore relation_store(statement.context);
    return relation_store.LoadDiagnosticFullState();
  }();
  Require(diagnostic_full_load.ok,
          "IPAR relation-state diagnostic full loader failed");
  Require(diagnostic_full_load.full_state_load,
          "IPAR relation-state diagnostic loader did not mark full load");
  Require(!diagnostic_full_load.scoped_state_load,
          "IPAR relation-state diagnostic loader incorrectly marked scoped load");
  Require(diagnostic_full_load.row_versions_retained == 4,
          "IPAR relation-state diagnostic full loader did not retain all rows");
  Require(HasEvidence(diagnostic_full_load.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.diagnostic_full_state.v1"),
          "diagnostic full load did not traverse the canonical store facade");

  const auto refused = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-refused",
      {"relation_state_load=full"});
  Require(!refused.ok,
          "IPAR relation-state mutation accepted caller-forced full load");
  if (!HasDiagnostic(refused, "relation_state_full_load_diagnostic_only")) {
    DumpDiagnostics(refused);
    Fail("IPAR relation-state full-load refusal diagnostic missing");
  }
  Require(HasEvidence(refused.evidence,
                      "relation_state_full_load_refused",
                      "diagnostic_only"),
          "IPAR relation-state full-load refusal evidence missing");
  Require(HasEvidence(refused.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state refusal incorrectly loaded full state");

  const auto normal = InsertInto(fixture,
                                 verify,
                                 fixture.target_table_uuid,
                                 "target-2");
  RequireInsertOk(normal, "IPAR relation-state normal scoped insert failed");
  Require(HasEvidence(normal.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state normal insert performed full load");
  Require(HasEvidence(normal.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state normal insert did not perform scoped load");
  Require(HasEvidence(normal.evidence,
                      "relation_state_load_reason",
                      "target_table_insert_scope"),
          "IPAR relation-state scoped reason evidence missing");
  Require(HasEvidence(normal.evidence,
                      "transactional_relation_store",
                      "canonical_normal_dml_v1"),
          "IPAR relation-state insert did not identify the canonical store");
  Require(HasEvidence(normal.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.insert_target.v1"),
          "IPAR relation-state insert did not identify its runtime route");
  Require(HasEvidence(normal.evidence,
                      "transactional_relation_store_finality_authority",
                      "durable_transaction_inventory"),
          "IPAR relation-state insert did not identify MGA finality authority");
  Require(HasEvidence(normal.evidence,
                      "transactional_relation_store_read_model",
                      "mga_scoped_read_view_v1"),
          "IPAR relation-state insert did not use the MGA read model");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_row_versions_retained") == "2",
          "IPAR relation-state scoped loader did not retain only target and child row state");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_row_versions_scanned") == "2",
          "IPAR relation-state scoped loader scanned unrelated row state");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_index_entries_scanned") == "1",
          "IPAR relation-state scoped loader scanned unrelated index state");
  Require(HasEvidence(normal.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state scoped physical segment evidence missing");
  Require(HasEvidence(normal.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state scoped loader used global fallback");

  const auto do_nothing = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-1",
      {"on_conflict_action:do_nothing", "conflict_target_column:payload"},
      "ignored");
  RequireInsertOk(do_nothing, "IPAR relation-state ON CONFLICT DO NOTHING failed");
  Require(do_nothing.skipped_count == 1,
          "IPAR relation-state ON CONFLICT DO NOTHING did not skip duplicate");
  Require(HasEvidence(do_nothing.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state ON CONFLICT DO NOTHING performed full load");
  Require(HasEvidence(do_nothing.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state ON CONFLICT DO NOTHING did not perform scoped load");
  Require(HasEvidence(do_nothing.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state ON CONFLICT DO NOTHING did not use scoped physical segments");
  Require(HasEvidence(do_nothing.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state ON CONFLICT DO NOTHING used global fallback");

  const auto do_update = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-1",
      {"on_conflict_action:do_update",
       "conflict_target_column:payload",
       "on_conflict_update_column:note"},
      "updated");
  RequireInsertOk(do_update, "IPAR relation-state ON CONFLICT DO UPDATE failed");
  Require(do_update.updated_count == 1,
          "IPAR relation-state ON CONFLICT DO UPDATE did not update duplicate");
  Require(HasEvidence(do_update.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state ON CONFLICT DO UPDATE performed full load");
  Require(HasEvidence(do_update.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state ON CONFLICT DO UPDATE did not perform scoped load");
  Require(HasEvidence(do_update.evidence,
                      "relation_state_load_reason",
                      "target_table_insert_and_child_reference_scope"),
          "IPAR relation-state ON CONFLICT DO UPDATE reference-scope evidence missing");
  Require(EvidenceValue(do_update.evidence,
                        "mga_relation_state_row_versions_retained") == "3",
          "IPAR relation-state ON CONFLICT DO UPDATE retained unrelated row state");
  Require(EvidenceValue(do_update.evidence,
                        "mga_relation_state_row_versions_scanned") == "3",
          "IPAR relation-state ON CONFLICT DO UPDATE scanned unrelated row state");
  Require(HasEvidence(do_update.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state ON CONFLICT DO UPDATE did not use scoped physical segments");
  Require(HasEvidence(do_update.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state ON CONFLICT DO UPDATE used global fallback");
  Commit(verify);
}

void VerifyScopedMaterializationAndCursorParity() {
  auto fixture = MakeFixture();

  auto seed = Begin(fixture, "ipar-relation-state-shape-seed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.target_table_uuid,
                             "target-shape-1"),
                  "IPAR relation-state shape target insert 1 failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.target_table_uuid,
                             "target-shape-2"),
                  "IPAR relation-state shape target insert 2 failed");
  constexpr std::size_t kUnrelatedRows = 48;
  // This is the unrelated comparison corpus, not the route under test. One
  // native batch preserves every physical row while avoiding 48 separate
  // statement/cohort admissions just to arrange the scan comparison.
  std::vector<api::EngineRowValue> unrelated_rows;
  unrelated_rows.reserve(kUnrelatedRows);
  for (std::size_t i = 0; i < kUnrelatedRows; ++i)
    unrelated_rows.push_back(Row("unrelated-shape-" + std::to_string(i)));
  const auto unrelated_insert = InsertRows(fixture, seed, fixture.unrelated_table_uuid,
                                         std::move(unrelated_rows));
  RequireInsertOk(unrelated_insert, "IPAR relation-state shape unrelated insert failed");
  Require(unrelated_insert.inserted_count == kUnrelatedRows,
          "IPAR relation-state shape batch omitted physical rows");
  Commit(seed);

  auto verify = Begin(fixture, "ipar-relation-state-shape-verify");
  {
  scratchbird::tests::FixtureEngineStatement statement(*fixture.session, verify);
  // Admit telemetry explicitly at its producer boundary. These are real scan
  // observations, not descriptor-registration side effects or synthetic zeros.
  scratchbird::tests::MetricProjectionFixture observations(
      fixture.database_uuid, scratchbird::tests::FixtureUuid(1490, 1), 1491);
  const metrics::MetricLabelSet labels = {
      {"component", "engine.mga_relation_store"}, {"operation", "select"},
      {"result", "scoped"}, {"reason", "transaction_visible_relation_scan"},
      {"object_uuid", fixture.target_table_uuid}};
  for (const auto* family : {"sb_mga_relation_state_load_total",
                            "sb_mga_relation_state_rows_materialized_total",
                            "sb_mga_relation_state_bytes_materialized_total",
                            "sb_mga_relation_state_allocation_units_materialized_total"})
    observations.Admit(family, labels);
  api::TransactionalRelationStore relation_store(statement.context);
  const auto full = relation_store.LoadDiagnosticFullState();
  const auto scoped = relation_store.OpenRelationScan(fixture.target_table_uuid);
  Require(full.ok && scoped.ok,
          "IPAR relation-state shape loaders failed");
  Require(full.full_state_load && !full.scoped_state_load,
          "IPAR reference loader did not report full-state materialization");
  Require(!scoped.full_state_load && scoped.scoped_state_load,
          "IPAR relation scan did not report scoped materialization");
  Require(full.rows_materialized >= kUnrelatedRows + 2,
          "IPAR full-state shape did not contain the unrelated corpus");
  Require(scoped.rows_materialized == 2,
          "IPAR relation scan materialized rows outside its UUID scope");
  Require(scoped.rows_materialized < full.rows_materialized,
          "IPAR relation scan row materialization was not bounded");
  Require(scoped.metadata_records_materialized <
              full.metadata_records_materialized,
          "IPAR relation scan metadata materialization was not UUID scoped");
  Require(scoped.bytes_materialized < full.bytes_materialized,
          "IPAR relation scan byte materialization was not bounded");
  Require(scoped.allocation_units_materialized <
              full.allocation_units_materialized,
          "IPAR relation scan allocation footprint was not bounded");
  Require(EvidenceValue(scoped.evidence,
                        "mga_relation_state_rows_materialized") == "2",
          "IPAR relation scan actual-row evidence is missing");
  Require(EvidenceValue(scoped.evidence,
                        "mga_relation_state_bytes_materialized") ==
              std::to_string(scoped.bytes_materialized),
          "IPAR relation scan actual-byte evidence is missing");
  Require(EvidenceValue(scoped.evidence,
                        "mga_relation_state_allocation_units_materialized") ==
              std::to_string(scoped.allocation_units_materialized),
          "IPAR relation scan allocation evidence is missing");
  Require(HasEvidence(scoped.evidence,
                      "mga_relation_state_operation_family", "select") &&
              HasEvidence(scoped.evidence,
                          "mga_relation_state_target_relation_uuid",
                          fixture.target_table_uuid) &&
              HasEvidence(scoped.evidence,
                          "mga_relation_state_load_reason",
                          "transaction_visible_relation_scan"),
          "IPAR relation scan identity/reason evidence is incomplete");
  Require(HasEvidence(full.evidence,
                      "mga_relation_state_full_load_policy_reason",
                      "explicit_diagnostic_inventory") &&
              !EvidenceValue(full.evidence,
                             "mga_relation_state_full_load_maximum_rows")
                   .empty() &&
              !EvidenceValue(full.evidence,
                             "mga_relation_state_full_load_maximum_bytes")
                   .empty() &&
              !EvidenceValue(
                   full.evidence,
                   "mga_relation_state_full_load_maximum_allocation_units")
                   .empty(),
          "IPAR diagnostic full load lacks its named bounded policy");

  const auto full_view = api::BuildMgaRelationReadView(full.state);
  const auto scoped_view = api::BuildMgaRelationReadView(scoped.state);
  const auto full_rows = api::VisibleMgaRowsForContext(
      full_view, fixture.target_table_uuid, statement.context);
  const auto scoped_rows = api::VisibleMgaRowsForContext(
      scoped_view, fixture.target_table_uuid, statement.context);
  RequireSameRows(full_rows, scoped_rows);
  Require(!scoped_rows.empty(),
          "IPAR scoped/full parity corpus unexpectedly has no rows");

  const auto point = relation_store.OpenRelationPointCursor(
      fixture.target_table_uuid, scoped_rows.front().row_uuid);
  Require(point.ok && point.scoped_state_load && !point.full_state_load,
          "IPAR point cursor did not remain scoped");
  Require(point.state.row_versions.size() == 1 &&
              point.state.row_versions.front().row_uuid ==
                  scoped_rows.front().row_uuid,
          "IPAR point cursor retained rows outside the stable row UUID");
  Require(point.row_versions_scanned == 2 &&
              point.row_versions_retained == 1 &&
              point.rows_materialized == 1,
          "IPAR point cursor scan/retention accounting is not exact");
  Require(HasEvidence(point.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.relation_point_cursor.v1"),
          "IPAR point cursor route evidence is missing");

  const auto index =
      relation_store.OpenRelationIndexCursor(fixture.target_table_uuid);
  Require(index.ok && index.scoped_state_load && !index.full_state_load,
          "IPAR index cursor did not remain relation scoped");
  Require(index.index_entries_retained == 2,
          "IPAR index cursor did not retain the target relation entries");
  Require(index.rows_materialized == 0 && index.state.row_versions.empty(),
          "IPAR index cursor unnecessarily materialized relation rows");
  Require(HasEvidence(index.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.relation_index_cursor.v1"),
          "IPAR index cursor route evidence is missing");

  const auto constraint =
      relation_store.LoadConstraintScope(fixture.target_table_uuid);
  Require(constraint.ok && constraint.scoped_state_load &&
              !constraint.full_state_load,
          "IPAR constraint lookup did not remain scoped");
  Require(constraint.rows_materialized == 2 &&
              constraint.metadata_records_materialized <
                  full.metadata_records_materialized,
          "IPAR constraint lookup retained unrelated relation state");
  Require(HasEvidence(constraint.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.constraint_scope.v1"),
          "IPAR constraint lookup route evidence is missing");

  const auto trigger =
      relation_store.LoadTriggerMetadataScope(fixture.target_table_uuid);
  Require(trigger.ok && trigger.scoped_state_load &&
              !trigger.full_state_load && trigger.rows_materialized == 0 &&
              trigger.state.relation_metadata.tables.size() == 1,
          "IPAR trigger metadata lookup retained non-target relation state");
  Require(HasEvidence(trigger.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.trigger_metadata_scope.v1"),
          "IPAR trigger metadata lookup route evidence is missing");

  const auto metric_values =
      metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  Require(HasMetric(metric_values, "sb_mga_relation_state_load_total",
                    fixture.target_table_uuid, "select", "scoped", 1),
          "IPAR scoped relation-load counter was not published");
  Require(HasMetric(metric_values,
                    "sb_mga_relation_state_rows_materialized_total",
                    fixture.target_table_uuid, "select", "scoped", 2),
          "IPAR scoped rows-materialized counter was not published");
  Require(HasMetric(metric_values,
                    "sb_mga_relation_state_bytes_materialized_total",
                    fixture.target_table_uuid, "select", "scoped", 1),
          "IPAR scoped bytes-materialized counter was not published");
  Require(HasMetric(
              metric_values,
              "sb_mga_relation_state_allocation_units_materialized_total",
              fixture.target_table_uuid, "select", "scoped", 1),
          "IPAR scoped allocation-materialized counter was not published");
  for (const auto& [family, expected] : std::array<std::pair<const char*, std::uint64_t>, 4>{{
      {"sb_mga_relation_state_load_total", 1},
      {"sb_mga_relation_state_rows_materialized_total", scoped.rows_materialized},
      {"sb_mga_relation_state_bytes_materialized_total", scoped.bytes_materialized},
      {"sb_mga_relation_state_allocation_units_materialized_total", scoped.allocation_units_materialized}}}) {
    const auto value = std::find_if(metric_values.begin(), metric_values.end(),
                                    [&](const auto& item) { return item.family == family; });
    Require(value != metric_values.end() && value->value == metrics::MetricScalar{expected},
            "IPAR native telemetry differs from its actual storage receipt");
  }
  observations.ExpectProduced(4);
  observations.Seal();
  observations.VerifyAndDrain();
  }
  Commit(verify);
}

}  // namespace

int main() {
  try {
    scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("ipar-bound-relation-state");
    VerifyCanonicalStoreAuthorityMap();
    VerifyRelationStateLoadRoutes();
    VerifyScopedMaterializationAndCursorParity();
    VerifyForeignKeyDescriptorLifecycle();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
