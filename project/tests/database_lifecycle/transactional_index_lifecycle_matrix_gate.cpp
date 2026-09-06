// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "dml/transactional_index_provider.hpp"
#include "dml/update_api.hpp"
#include "index_family_registry.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction/local_commit_publication.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kSearchKey =
    "DML_TRANSACTIONAL_INDEX_LIFECYCLE_MATRIX";

[[noreturn]] void Fail(std::string_view family, std::string_view message) {
  std::cerr << "family=" << family << ':' << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition,
             std::string_view family,
             std::string_view message) {
  if (!condition) Fail(family, message);
}

template <typename TResult>
void RequireOk(const TResult& result,
               std::string_view family,
               std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(family, message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view family,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Fail(family, message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  if (!generated.ok()) Fail("fixture", "UUID creation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "canonical=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string row_uuid,
                        std::string key,
                        std::string payload) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"key_value", TextValue(std::move(key))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

struct FamilyCase {
  const idx::IndexFamilyDescriptor* descriptor = nullptr;
  const idx::IndexFamilyPhysicalCapabilityState* capability = nullptr;
  std::string crud_family;
  bool unique = false;
  std::string old_key;
  std::string new_key;
  std::string rollback_key;
};

std::string CrudFamily(const idx::IndexFamilyDescriptor& descriptor) {
  if (descriptor.family == idx::IndexFamily::unique_btree) {
    return api::kCrudIndexFamilyBtree;
  }
  if (descriptor.family == idx::IndexFamily::graph) {
    return api::kCrudIndexFamilyGraphAdjacency;
  }
  return descriptor.id;
}

bool IsTokenFamily(idx::IndexFamily family) {
  return family == idx::IndexFamily::full_text ||
         family == idx::IndexFamily::gin ||
         family == idx::IndexFamily::inverted ||
         family == idx::IndexFamily::ngram ||
         family == idx::IndexFamily::sparse_wand ||
         family == idx::IndexFamily::document_path;
}

FamilyCase BuildCase(const idx::IndexFamilyDescriptor& descriptor) {
  FamilyCase test_case;
  test_case.descriptor = &descriptor;
  test_case.capability =
      idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
  test_case.crud_family = CrudFamily(descriptor);
  test_case.unique = descriptor.family == idx::IndexFamily::unique_btree ||
                     descriptor.family == idx::IndexFamily::hash;
  if (descriptor.key_model == idx::IndexKeyModel::spatial_key) {
    test_case.old_key = "0,0,1,1";
    test_case.new_key = "2,2,3,3";
    test_case.rollback_key = "4,4,5,5";
  } else if (descriptor.key_model == idx::IndexKeyModel::vector_key) {
    test_case.old_key = "0,1,2,3";
    test_case.new_key = "4,5,6,7";
    test_case.rollback_key = "8,9,10,11";
  } else if (descriptor.family == idx::IndexFamily::ngram) {
    test_case.old_key = "old";
    test_case.new_key = "new";
    test_case.rollback_key = "bad";
  } else if (IsTokenFamily(descriptor.family)) {
    test_case.old_key = "alpha old";
    test_case.new_key = "bravo new";
    test_case.rollback_key = "charlie ghost";
  } else {
    test_case.old_key = "old-key";
    test_case.new_key = "new-key";
    test_case.rollback_key = "rollback-key";
  }
  return test_case;
}

api::EnginePredicateEnvelope PredicateFor(const FamilyCase& test_case,
                                          const std::string& key) {
  api::EnginePredicateEnvelope predicate;
  const auto family = test_case.descriptor->family;
  if (family == idx::IndexFamily::full_text ||
      family == idx::IndexFamily::gin ||
      family == idx::IndexFamily::inverted ||
      family == idx::IndexFamily::sparse_wand ||
      family == idx::IndexFamily::document_path) {
    predicate.predicate_kind = "text_term_contains";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(
        TextValue(key.substr(0, key.find(' '))));
  } else if (family == idx::IndexFamily::ngram) {
    predicate.predicate_kind = "text_term_contains";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (test_case.descriptor->key_model ==
             idx::IndexKeyModel::spatial_key) {
    predicate.predicate_kind = "spatial_bbox_intersects";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (family == idx::IndexFamily::vector_exact) {
    predicate.predicate_kind = "vector_exact_nearest";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (family == idx::IndexFamily::vector_hnsw ||
             family == idx::IndexFamily::vector_ivf) {
    predicate.predicate_kind = "vector_approx_nearest";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (family == idx::IndexFamily::expression) {
    predicate.predicate_kind = "expression_equals";
    predicate.canonical_predicate_envelope = "lower:key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (family == idx::IndexFamily::partial) {
    predicate.predicate_kind = "partial_index_probe";
    predicate.canonical_predicate_envelope = "key_value=" + key;
  } else {
    predicate.predicate_kind = "column_equals";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  }
  return predicate;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id,
                                std::string isolation = "read_committed") {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = isolation;
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "fixture", "begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context, std::string_view family) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), family, "commit failed");
}

void Rollback(const api::EngineRequestContext& context,
              std::string_view family) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), family, "rollback failed");
}

api::CrudIndexRecord IndexRecord(const Fixture& fixture,
                                 const FamilyCase& test_case,
                                 const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "key_value";
  index.family = test_case.crud_family;
  index.profile = test_case.descriptor->default_semantic_profile;
  index.unique = test_case.unique;
  index.approximate = test_case.descriptor->approximate;
  index.exact_fallback = index.approximate;
  if (test_case.descriptor->family == idx::IndexFamily::expression) {
    index.key_envelopes.push_back("lower:key_value");
  } else {
    index.key_envelopes.push_back("key_value");
  }
  if (test_case.descriptor->family == idx::IndexFamily::partial) {
    index.predicate_kind = "where_true";
  }
  if (test_case.descriptor->family == idx::IndexFamily::covering) {
    index.include_columns.push_back("payload");
  }
  if (index.unique) index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture(const FamilyCase& test_case, platform::u64 salt) {
  const std::string& family = test_case.descriptor->id;
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_tx_index_matrix_" + family + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "matrix.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), family, "database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto context = Begin(fixture, "matrix-metadata");
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "transactional_index_matrix_" + family;
  table.columns.push_back({"key_value", "canonical=text"});
  table.columns.push_back({"payload", "canonical=text"});
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, table),
                      family, "table metadata append failed");
  RequireDiagnosticOk(
      api::AppendMgaIndexMetadata(context,
                                  IndexRecord(fixture, test_case, context)),
      family, "index metadata append failed");
  Commit(context, family);
  return fixture;
}

api::EngineApiResult DispatchDml(const api::EngineRequestContext& context,
                                 std::string operation_id,
                                 std::string opcode,
                                 api::EngineApiRequest request) {
  auto envelope =
      sblr::MakeSblrEnvelope(operation_id, opcode, std::string(kSearchKey));
  const auto* registered = sblr::LookupSblrOperation(operation_id);
  Require(registered != nullptr && registered->opcode == opcode &&
              registered->code != 0,
          "sblr", "canonical DML opcode is not registered");
  envelope.opcode_code = registered->code;
  envelope.parser_package_uuid =
      "12340000-0000-7000-8000-000000000031";
  envelope.registry_snapshot_uuid =
      "12340000-0000-7000-8000-000000000032";
  envelope.requires_transaction_context = true;
  request.context = context;
  request.operation_id = operation_id;
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = context;
  dispatch.envelope = std::move(envelope);
  dispatch.api_request = std::move(request);
  auto result = sblr::DispatchSblrOperation(std::move(dispatch));
  Require(result.accepted && result.envelope_validated &&
              result.dispatched_to_api,
          "sblr", "canonical DML dispatch failed before the engine API");
  return std::move(result.api_result);
}

api::EngineApiResult Insert(const Fixture& fixture,
                            const api::EngineRequestContext& context,
                            std::string row_uuid,
                            std::string key,
                            std::string payload) {
  api::EngineApiRequest request;
  request.context = context;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(
      Row(std::move(row_uuid), std::move(key), std::move(payload)));
  return DispatchDml(context, "dml.insert_rows", "SBLR_DML_INSERT_ROWS",
                     std::move(request));
}

api::EngineApiResult Update(const Fixture& fixture,
                            const api::EngineRequestContext& context,
                            std::string row_uuid,
                            std::string key) {
  api::EngineApiRequest request;
  request.context = context;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "row_uuid_match";
  request.predicate.canonical_predicate_envelope = std::move(row_uuid);
  request.assignments.push_back({"key_value", TextValue(std::move(key))});
  return DispatchDml(context, "dml.update_rows", "SBLR_DML_UPDATE_ROWS",
                     std::move(request));
}

api::EngineApiResult Delete(const Fixture& fixture,
                            const api::EngineRequestContext& context,
                            std::string row_uuid) {
  api::EngineApiRequest request;
  request.context = context;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "row_uuid_match";
  request.predicate.canonical_predicate_envelope = std::move(row_uuid);
  request.option_envelopes.push_back("delete_mode:tombstone_only");
  return DispatchDml(context, "dml.delete_rows", "SBLR_DML_DELETE_ROWS",
                     std::move(request));
}

api::MgaRelationReadView LoadState(
    const api::EngineRequestContext& context,
    std::string_view family) {
  auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, family, "relation state reload failed");
  return api::BuildMgaRelationReadView(std::move(loaded.state));
}

const api::CrudIndexRecord& FindIndex(const api::MgaRelationReadView& state,
                                      const Fixture& fixture,
                                      std::string_view family) {
  for (const auto& index : state.indexes) {
    if (index.index_uuid == fixture.index_uuid) return index;
  }
  Fail(family, "durable index metadata missing");
}

bool HasMutation(const api::LocalCommitPublicationResult& publication,
                 const Fixture& fixture,
                 std::string_view kind) {
  const bool found = std::any_of(
      publication.mutations.begin(), publication.mutations.end(),
      [&](const auto& mutation) {
        return mutation.mutation_domain == "index" &&
               mutation.object_identity == fixture.index_uuid &&
               mutation.mutation_kind == kind &&
               mutation.finality_authority ==
                   "durable_transaction_inventory";
      });
  if (!found) {
    for (const auto& mutation : publication.mutations) {
      std::cerr << "manifest=" << mutation.mutation_domain << ':'
                << mutation.mutation_kind << ':' << mutation.object_identity
                << ':' << mutation.finality_authority << '\n';
    }
  }
  return found;
}

void ValidateAdmittedFamily(const FamilyCase& test_case,
                            platform::u64 salt) {
  const std::string& family = test_case.descriptor->id;
  auto fixture = MakeFixture(test_case, salt);

  auto seed = Begin(fixture, "matrix-seed");
  const std::string row_uuid =
      NewUuidText(platform::UuidKind::object, salt + 200);
  const auto inserted =
      Insert(fixture, seed, row_uuid, test_case.old_key, "seed");
  RequireOk(inserted, family, "insert failed");
  Require(inserted.dml_summary.rows_changed == 1, family,
          "insert did not publish one stable row identity");
  const auto seed_publication = api::RunLocalCommitPageBarrier(seed);
  Require(seed_publication.ok &&
              HasMutation(seed_publication, fixture, "insert"),
          family, "insert was absent from the publication manifest");
  Commit(seed, family);

  if (test_case.unique) {
    auto duplicate = Begin(fixture, "matrix-duplicate");
    const auto rejected = Insert(
        fixture,
        duplicate,
        NewUuidText(platform::UuidKind::object, salt + 201),
        test_case.old_key,
        "duplicate");
    Require(!rejected.ok, family, "unique duplicate was accepted");
    Rollback(duplicate, family);
  }

  auto old_snapshot = Begin(fixture, "matrix-old-snapshot", "snapshot");
  auto updater = Begin(fixture, "matrix-update");
  const auto updated = Update(fixture, updater, row_uuid, test_case.new_key);
  RequireOk(updated, family, "key-changing update failed");
  Require(updated.dml_summary.rows_changed == 1, family,
          "key-changing update did not match one row");
  const auto update_publication = api::RunLocalCommitPageBarrier(updater);
  Require(update_publication.ok &&
              HasMutation(update_publication, fixture, "retire") &&
              HasMutation(update_publication, fixture, "insert"),
          family, "update insert/retire pair was absent from publication");
  Commit(updater, family);

  auto current_reader = Begin(fixture, "matrix-current-reader");
  const auto state = LoadState(current_reader, family);
  const auto& index = FindIndex(state, fixture, family);
  api::MgaTransactionalIndexProvider old_provider(old_snapshot, nullptr);
  api::MgaTransactionalIndexProvider current_provider(current_reader, nullptr);
  const auto old_validated = old_provider.ValidateAgainstRelation(state, index);
  const auto current_validated =
      current_provider.ValidateAgainstRelation(state, index);
  Require(old_validated.ok && old_validated.visible_entry_count != 0,
          family, "old snapshot lost pre-update index membership");
  Require(current_validated.ok && current_validated.visible_entry_count != 0,
          family, "current snapshot lost post-update index membership");
  const auto old_lookup = old_provider.ResolveVisibleEntry(
      state, index, PredicateFor(test_case, test_case.old_key), 1);
  const auto current_lookup = current_provider.ResolveVisibleEntry(
      state, index, PredicateFor(test_case, test_case.new_key), 1);
  Require(old_lookup.ok && old_lookup.rows.size() == 1,
          family, "old snapshot index lookup lost its visible row");
  Require(current_lookup.ok && current_lookup.rows.size() == 1,
          family, "current snapshot index lookup lost its visible row");
  if (test_case.descriptor->key_model != idx::IndexKeyModel::vector_key) {
    const auto retired_lookup = current_provider.ResolveVisibleEntry(
        state, index, PredicateFor(test_case, test_case.old_key), 1);
    Require(retired_lookup.ok && retired_lookup.rows.empty(),
            family, "current snapshot index lookup retained the old key");
  }
  const auto recovered =
      api::MgaTransactionalIndexProvider(updater, nullptr)
          .RecoverInterruptedMutation(state);
  Require(recovered.ok && recovered.lifecycle_state == "committed_by_inventory",
          family, "committed update recovery classification changed");
  const auto published =
      api::MgaTransactionalIndexProvider(updater, nullptr)
          .PublishTransaction(state);
  Require(published.ok && published.lifecycle_state == "published_by_inventory",
          family, "provider publication did not follow MGA inventory");
  Commit(old_snapshot, family);
  Commit(current_reader, family);

  auto rebuild = Begin(fixture, "matrix-rebuild");
  const auto rebuild_state = LoadState(rebuild, family);
  const auto& rebuild_index = FindIndex(rebuild_state, fixture, family);
  auto append = api::MgaRelationHotAppendContext(rebuild);
  api::MgaTransactionalIndexProvider rebuild_provider(rebuild, &append);
  const auto horizon_refused = rebuild_provider.RebuildFromRelation(
      rebuild_state, rebuild_index, false);
  Require(!horizon_refused.ok &&
              horizon_refused.diagnostic.code ==
                  "INDEX.TRANSACTIONAL_PROVIDER.REBUILD_HORIZON_REQUIRED",
          family, "rebuild did not require authoritative cleanup horizon");
  const auto rebuilt = rebuild_provider.RebuildFromRelation(
      rebuild_state, rebuild_index, true);
  Require(rebuilt.ok && rebuilt.rebuilt_entry_count != 0,
          family, "deterministic rebuild produced no entries");
  RequireDiagnosticOk(append.FlushIndexEntries(), family,
                      "rebuild index flush failed");
  Commit(rebuild, family);

  auto reopen = Begin(fixture, "matrix-reopen");
  const auto reopened_state = LoadState(reopen, family);
  const auto& reopened_index = FindIndex(reopened_state, fixture, family);
  Require(api::MgaTransactionalIndexProvider(reopen, nullptr)
              .ValidateAgainstRelation(reopened_state, reopened_index)
              .ok,
          family, "rebuilt index failed validation after durable reopen");
  Commit(reopen, family);

  auto deleter = Begin(fixture, "matrix-delete");
  const auto deleted = Delete(fixture, deleter, row_uuid);
  RequireOk(deleted, family, "delete failed");
  Require(deleted.dml_summary.rows_changed == 1, family,
          "delete did not match one row");
  const auto delete_publication = api::RunLocalCommitPageBarrier(deleter);
  Require(delete_publication.ok &&
              HasMutation(delete_publication, fixture, "retire"),
          family, "delete retirement was absent from publication");
  Commit(deleter, family);

  auto rollback_writer = Begin(fixture, "matrix-rollback-writer");
  const auto rolled_back_insert =
      Insert(fixture,
             rollback_writer,
             NewUuidText(platform::UuidKind::object, salt + 202),
             test_case.rollback_key,
             "rollback");
  RequireOk(rolled_back_insert, family, "rollback insert failed");
  Rollback(rollback_writer, family);

  auto final_reader = Begin(fixture, "matrix-final-reader");
  const auto final_state = LoadState(final_reader, family);
  const auto& final_index = FindIndex(final_state, fixture, family);
  const auto final_validation =
      api::MgaTransactionalIndexProvider(final_reader, nullptr)
          .ValidateAgainstRelation(final_state, final_index);
  Require(final_validation.ok && final_validation.visible_entry_count == 0,
          family, "delete or rollback left visible index membership");
  const auto final_lookup =
      api::MgaTransactionalIndexProvider(final_reader, nullptr)
          .ResolveVisibleEntry(final_state,
                               final_index,
                               PredicateFor(test_case, test_case.new_key),
                               1);
  Require(final_lookup.ok && final_lookup.rows.empty(),
          family, "delete left a visible index lookup ghost");
  for (const auto& entry : final_state.index_entries) {
    if (entry.index_uuid == fixture.index_uuid &&
        entry.creator_tx == rollback_writer.local_transaction_id) {
      Require(!api::CrudCreatorVisible(final_state,
                                       entry.creator_tx,
                                       entry.event_sequence,
                                       final_reader.local_transaction_id),
              family, "rolled-back index entry remained MGA-visible");
    }
  }
  const auto aborted =
      api::MgaTransactionalIndexProvider(rollback_writer, nullptr)
          .AbortTransaction(final_state);
  Require(aborted.ok && aborted.lifecycle_state == "invisible_by_inventory",
          family, "rollback classification did not follow MGA inventory");
  const auto rollback_recovery =
      api::MgaTransactionalIndexProvider(rollback_writer, nullptr)
          .RecoverInterruptedMutation(final_state);
  Require(rollback_recovery.ok &&
              rollback_recovery.lifecycle_state == "abandoned_by_inventory",
          family, "rolled-back recovery classification changed");
  Commit(final_reader, family);
}

void ValidateNonAdmittedFamily(const FamilyCase& test_case,
                               platform::u64 salt) {
  const std::string& family = test_case.descriptor->id;
  api::CrudIndexRecord index;
  index.index_uuid = "non-admitted-index";
  index.table_uuid = "non-admitted-table";
  index.family = test_case.crud_family;
  index.profile = test_case.descriptor->default_semantic_profile;
  index.key_envelopes.push_back("key_value");
  Require(!api::IsAdmittedMgaTransactionalIndexFamily(index), family,
          "non-admitted family was exposed by the transactional provider");
  api::EngineRequestContext context;
  context.local_transaction_id = 1;
  context.transaction_uuid.canonical = "non-admitted-transaction";
  api::MgaTransactionalIndexProvider provider(context, nullptr);
  api::DmlTransactionalIndexEntryRequest request;
  request.index = index;
  request.table_uuid = index.table_uuid;
  request.row_uuid = "row";
  request.version_uuid = "version";
  request.key_value = "key";
  const auto refused = provider.PrepareInsertEntry(request);
  Require(!refused.ok &&
              refused.diagnostic.code ==
                  "INDEX.TRANSACTIONAL_PROVIDER.FAMILY_NOT_ADMITTED",
          family, "non-admitted provider did not fail closed");

  auto fixture = MakeFixture(test_case, salt);
  auto writer = Begin(fixture, "matrix-non-admitted-insert");
  const auto dml_refused =
      Insert(fixture,
             writer,
             NewUuidText(platform::UuidKind::object, salt + 203),
             test_case.old_key,
             "must-not-publish");
  Require(!dml_refused.ok, family,
          "ordinary DML silently accepted a non-admitted index family");
  Rollback(writer, family);
  auto reader = Begin(fixture, "matrix-non-admitted-reader");
  const auto state = LoadState(reader, family);
  Require(VisibleCrudRowsForContext(state,
                                    fixture.table_uuid,
                                    reader).empty(),
          family, "refused non-admitted DML left a visible row");
  Commit(reader, family);
}

}  // namespace

int main() {
  Require(kSearchKey == "DML_TRANSACTIONAL_INDEX_LIFECYCLE_MATRIX",
          "matrix", "search key drifted");
  std::size_t admitted = 0;
  std::size_t admitted_btree_profiles = 0;
  std::size_t admitted_non_btree_profiles = 0;
  std::size_t refused = 0;
  platform::u64 salt = 10000;
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    const auto test_case = BuildCase(descriptor);
    Require(test_case.capability != nullptr, descriptor.id,
            "capability record missing from registry");
    api::CrudIndexRecord probe;
    probe.family = test_case.crud_family;
    probe.profile = descriptor.default_semantic_profile;
    probe.unique = test_case.unique;
    if (api::IsAdmittedMgaTransactionalIndexFamily(probe)) {
      Require(test_case.capability->runtime_available, descriptor.id,
              "provider admitted a registry-unavailable family");
      ValidateAdmittedFamily(test_case, salt);
      if (descriptor.native_physical_family == "btree") {
        ++admitted_btree_profiles;
      } else {
        ++admitted_non_btree_profiles;
      }
      ++admitted;
      std::cout << "profile=" << descriptor.id
                << " physical_family=" << descriptor.native_physical_family
                << " admission=admitted lifecycle=passed\n";
    } else {
      ValidateNonAdmittedFamily(test_case, salt);
      ++refused;
      std::cout << "profile=" << descriptor.id
                << " physical_family=" << descriptor.native_physical_family
                << " admission=refused lifecycle=fail_closed\n";
    }
    salt += 1000;
  }
  Require(admitted != 0, "matrix", "no native family was admitted");
  Require(admitted_btree_profiles != 0, "matrix",
          "no B-tree profile was covered");
  Require(admitted_non_btree_profiles != 0, "matrix",
          "no non-B-tree profile was covered");
  Require(admitted + refused == idx::BuiltinIndexFamilyDescriptors().size(),
          "matrix", "registry family was omitted from lifecycle matrix");
  std::cout << "transactional_index_lifecycle_matrix admitted=" << admitted
            << " btree_profiles=" << admitted_btree_profiles
            << " non_btree_profiles=" << admitted_non_btree_profiles
            << " refused=" << refused
            << " total=" << (admitted + refused) << '\n';
  return EXIT_SUCCESS;
}
