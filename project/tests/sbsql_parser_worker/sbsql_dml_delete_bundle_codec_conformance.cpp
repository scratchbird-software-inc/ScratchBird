// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_durable_authority_codec.hpp"
#include "dml/delete_binding_authority.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "sblr_executor_row_identity_hash.hpp"
#include "hash_digest.hpp"
#include "mga_relation_store/mga_delete_durable_store.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "uuid.hpp"
#include "../sbsql_sblr_alignment/typed_delete_test_fixture.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace e = scratchbird::engine::internal_api;
namespace f = scratchbird::tests::delete_carrier;
namespace w = scratchbird::wire;
using Bytes = std::vector<std::uint8_t>;
static unsigned checks = 0;
static void Check(bool value, const char* reason) { ++checks; f::Check(value, reason); }
static std::string Text(unsigned id) { return e::datatype_operator_projection::UuidText(f::Uuid(id)); }
static w::TypedUpdateHash Hash(std::string_view value) {
  auto result = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  Check(result.ok(), "hash"); return result.digest;
}
// Synthetic structural evidence, never registered as live execution authority.
static e::DmlDeleteDurableAuthorityBundleV1 Bundle(unsigned profile) {
  auto fixture = f::Make(profile);
  e::DmlDeleteDurableAuthorityBundleV1 b;
  b.descriptor = fixture.descriptor; b.predicate = fixture.predicate;
  b.datatypes = fixture.datatypes; b.operators = fixture.operators;
  b.database_uuid = f::Uuid(100); b.session_uuid = f::Uuid(101);
  b.principal_uuid = f::Uuid(102); b.bundle_uuid = f::Uuid(103);
  b.bundle_generation = 1; b.owner_context_sha256 = Hash("synthetic owner");
  b.reserved_statement_savepoint_uuid = f::Uuid(112);
  auto& d = b.descriptor; auto& s = b.security; auto& effect = b.effects;
  s.snapshot_uuid = Text(8); s.snapshot_generation = 1;
  s.authenticated_statement_receipt_uuid = Text(2); s.security_context_uuid = Text(7);
  s.security_context_generation = 1; s.security_generation = 1; s.policy_generation = 1;
  s.target_relation_uuid = Text(9); b.matched_grant_uuids = {Text(104), Text(105)};
  d.row_policy_set_uuid = d.security_snapshot_uuid;
  Check(e::ComputeDmlDeleteSecuritySnapshotHashV1(s, b.matched_grant_uuids, &d.row_policy_set_sha256), "security hash");
  effect.snapshot_uuid = f::Uuid(106); effect.generation = 1;
  effect.constraint_set_uuid = d.constraint_set_uuid; effect.trigger_set_uuid = d.trigger_set_uuid;
  effect.target_relation_uuid = d.target_relation_uuid; effect.target_relation_generation = 1;
  effect.relation_descriptor_uuid = f::Uuid(107); effect.relation_descriptor_generation = 1;
  effect.relation_shape_sha256 = Hash("synthetic shape"); effect.index_set_sha256 = Hash("synthetic indexes");
  effect.constraint_set_sha256 = Hash("ScratchBird.DmlDelete.NoInboundConstraintSet.V1");
  effect.trigger_set_sha256 = Hash("ScratchBird.DmlDelete.NoTriggerSet.V1");
  d.ordered_constraint_set_sha256 = effect.constraint_set_sha256;
  d.ordered_trigger_set_sha256 = effect.trigger_set_sha256;
  auto& t = b.target_order;
  t.target_order_uuid = d.deterministic_target_order_uuid; t.target_order_generation = 1;
  t.authenticated_statement_receipt_uuid = d.authenticated_statement_receipt_uuid;
  t.target_relation_occurrence_uuid = d.target_relation_occurrence_uuid; t.target_relation_occurrence_generation = 1;
  t.statement_snapshot_uuid = d.statement_snapshot_uuid; t.maximum_candidate_rows = 100;
  auto& r = b.resource_budget;
  r.resource_budget_uuid = d.resource_budget_uuid; r.resource_budget_generation = 1;
  r.authenticated_statement_receipt_uuid = d.authenticated_statement_receipt_uuid;
  r.owning_transaction_uuid = d.owning_transaction_uuid;
  r.cancellation_token_uuid = f::Uuid(108); r.cancellation_generation = 1;
  r.grant_receipt_uuid = f::Uuid(109); r.grant_receipt_generation = 1;
  r.maximum_assignments = 16; r.maximum_predicate_nodes = 16;
  r.maximum_candidate_rows = 100; r.maximum_trigger_depth = 1; r.maximum_effects = 100;
  r.maximum_total_canonical_value_bytes = 1048576;
  auto& recovery = b.recovery;
  recovery.recovery_token_uuid = d.recovery_token_uuid; recovery.recovery_generation = 1;
  recovery.authenticated_statement_receipt_uuid = d.authenticated_statement_receipt_uuid;
  recovery.owning_transaction_uuid = d.owning_transaction_uuid; recovery.operation_uuid = d.operation_uuid;
  recovery.descriptor_uuid = d.descriptor_uuid; recovery.descriptor_generation = 1;
  recovery.statement_savepoint_profile_uuid = f::Uuid(110); recovery.statement_savepoint_profile_generation = 1;
  recovery.durable_registry_uuid = b.bundle_uuid; recovery.durable_registry_generation = 1;
  auto& x = b.executor;
  x.snapshot_uuid = Text(111); x.generation = 1; x.database_uuid = Text(100);
  x.installed = true; x.availability_state = e::SblrExecutorAvailabilityState::installed;
  x.row_identity_sha256 = e::HashSblrExecutorRowIdentityMaterial(
      {"dml.delete_rows", 784, "1.0", "dml_delete_rows_descriptor", "mutation_result", 1});
  x.decision_evidence_sha256 = "sha256:" + scratchbird::core::hash::HexLower(Hash("synthetic decision"));
  return b;
}
static e::EngineRequestContext Context(const e::DmlDeleteDurableAuthorityBundleV1& b) {
  e::EngineRequestContext c;
  const auto text = e::datatype_operator_projection::UuidText;
  c.database_path = "synthetic_database";
  c.database_uuid.canonical = text(b.database_uuid); c.session_uuid.canonical = text(b.session_uuid);
  c.principal_uuid.canonical = text(b.principal_uuid);
  c.transaction_uuid.canonical = text(b.descriptor.owning_transaction_uuid);
  c.local_transaction_id = b.descriptor.owning_local_transaction_id;
  c.statement_uuid.canonical = Text(120);
  c.statement_receipt_uuid.canonical = text(b.descriptor.authenticated_statement_receipt_uuid);
  c.statement_snapshot_uuid.canonical = text(b.descriptor.statement_snapshot_uuid);
  c.statement_snapshot_generation = 1;
  c.statement_metadata_snapshot_uuid.canonical = text(b.descriptor.catalog_snapshot_uuid);
  c.statement_metadata_snapshot_engine_owned = true; c.catalog_generation_id = 1;
  c.security_context_present = true; c.security_epoch = 1;
  c.authorization_context.present = true;
  c.authorization_context.authority_uuid.canonical = b.security.security_context_uuid;
  c.authorization_context.principal_uuid = c.principal_uuid;
  c.authorization_context.security_context_generation = b.security.security_context_generation;
  c.authorization_context.catalog_generation_id = 1;
  c.authorization_context.security_epoch = c.authorization_context.policy_epoch = 1;
  c.resource_admission_uuid.canonical = Text(121); c.resource_epoch = 1;
  c.datatype_catalog_snapshot_uuid.canonical = text(b.datatypes.identity.vector_uuid);
  c.datatype_catalog_generation = c.datatype_registry_generation = 1;
  c.transaction_isolation_level = "snapshot";
  return c;
}
static void VerifyOwnerAndStorage() {
  namespace db = scratchbird::storage::database;
  namespace uuid = scratchbird::core::uuid;
  namespace mga = scratchbird::transaction::mga;
  using Kind = scratchbird::core::platform::UuidKind;
  auto b = Bundle(2); auto c = Context(b);
  Check(e::ComputeDmlDeleteOwnerContextHashV1(c, &b.owner_context_sha256) &&
        e::MatchesDmlDeleteDurableAuthorityOwnerV1(c, b), "complete structural owner");
  Check(!e::CaptureDmlDeleteBindingAuthorityV1(c, b, {}, {}, {}, {}).ok,
        "valid bundle bytes cannot manufacture authenticated binding authority");
  auto binder = c; binder.trace_tags = {"private_dml_delete_rows_binder"};
  Check(!e::CaptureDmlDeleteBindingAuthorityV1(binder, b, {}, {}, {}, {}).ok,
        "private phase alone cannot manufacture missing provider handles or resource receipt");
  auto consumer = c; consumer.trace_tags = {"private_dml_delete_rows_consumer"};
  Check(e::RevalidateDmlDeleteBindingAuthorityV1(consumer, {}).error,
        "consumer requires opaque engine binding authority");
  const auto refuse = [&](auto mutate) {
    auto wrong = c; mutate(wrong);
    Check(!e::MatchesDmlDeleteDurableAuthorityOwnerV1(wrong, b), "different owner refused");
  };
  refuse([](auto& c) { c.database_path += "_different"; });
  refuse([](auto& c) { c.session_uuid.canonical = Text(200); });
  refuse([](auto& c) { c.statement_uuid.canonical = Text(200); });
  refuse([](auto& c) { c.principal_uuid.canonical = Text(200); });
  refuse([](auto& c) { c.statement_receipt_uuid.canonical = Text(200); });
  refuse([](auto& c) { ++c.local_transaction_id; });
  refuse([](auto& c) { ++c.statement_snapshot_generation; });
  refuse([](auto& c) { ++c.snapshot_visible_through_local_transaction_id; });
  refuse([](auto& c) { c.statement_metadata_snapshot_active_excluded_local_transaction_ids.push_back(1); });
  refuse([](auto& c) { c.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids.push_back(1); });
  refuse([](auto& c) { c.current_role_uuid.canonical = Text(200); });
  refuse([](auto& c) { c.transaction_policy_snapshot_uuid.canonical = Text(200); });
  refuse([](auto& c) { ++c.resource_epoch; });
  refuse([](auto& c) { ++c.authorization_context.policy_epoch; });
  refuse([](auto& c) { c.cluster_transaction_active = true; });
  refuse([](auto& c) { c.route_fence_present = true; });
  auto phase_only = c; phase_only.trace_tags.push_back("private_dml_delete_rows_recovery");
  Check(e::MatchesDmlDeleteDurableAuthorityOwnerV1(phase_only, b), "phase is not persisted owner authority");
  const auto root = std::filesystem::temp_directory_path() /
      ("sb_delete_bundle_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Check(std::filesystem::create_directory(root), "create owned fixture");
  std::cout << "delete_bundle_fixture=" << root << '\n';
  db::DatabaseCreateConfig config;
  config.path = (root / "bundle.sbdb").string();
  auto database = uuid::GenerateDurableEngineIdentityV7(Kind::database, 1788210000000);
  auto filespace = uuid::GenerateDurableEngineIdentityV7(Kind::filespace, 1788210000001);
  Check(database.ok() && filespace.ok(), "database identities");
  config.database_uuid = database.value; config.filespace_uuid = filespace.value;
  config.page_size = 16384; config.creation_unix_epoch_millis = 1788210000002;
  config.allow_minimal_resource_bootstrap = true; config.require_resource_seed_pack = false;
  Check(db::CreateDatabaseFile(config).ok(), "database create");
  Check(db::OpenDatabaseFile({config.path, false, false, false}).ok(), "database open");
  Check(db::MarkDatabaseCleanShutdown(config.path).ok(), "clean marker");
  const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(config.path);
  auto transaction = uuid::GenerateDurableEngineIdentityV7(Kind::transaction, 1788210000003);
  Check(inventory.ok() && transaction.ok(), "inventory and transaction");
  const auto begin = mga::BeginLocalTransaction(inventory.inventory, transaction.value, 1788210000004);
  Check(begin.ok() && db::PersistLocalTransactionInventoryToDatabase(config.path, begin.inventory).ok(), "begin");
  b.database_uuid = database.value.value.bytes;
  b.descriptor.owning_transaction_uuid = transaction.value.value.bytes;
  b.descriptor.owning_local_transaction_id = begin.entry.identity.local_id.value;
  b.resource_budget.owning_transaction_uuid = b.recovery.owning_transaction_uuid = b.descriptor.owning_transaction_uuid;
  b.executor.database_uuid = uuid::UuidToString(database.value.value);
  c = Context(b); c.database_path = config.path; c.database_page_size_bytes = 16384;
  Check(e::ComputeDmlDeleteOwnerContextHashV1(c, &b.owner_context_sha256), "real owner hash");
  e::EngineApiDiagnostic diagnostic;
  auto store = e::MgaDmlDeleteDurableStoreV1::Open(c, b.descriptor.descriptor_uuid, 1, &diagnostic);
  Check(bool(store), "open bundle store");
  e::DmlDeleteDurableAuthorityBundleV1 loaded;
  Check(!store->LoadAuthorityBundle(&loaded, &diagnostic), "missing bundle refused");
  Check(store->StoreAuthorityBundle(b, &diagnostic), "fenced bundle store");
  Check(store->chain().empty(), "bundle alone never creates bound authority");
  Check(store->LoadAuthorityBundle(&loaded, &diagnostic), "load bundle");
  const auto exact = loaded.exact_bytes;
  Check(store->StoreAuthorityBundle(b, &diagnostic), "immutable exact retry");
  auto conflict = b; conflict.effects.index_count = 1;
  Check(!store->StoreAuthorityBundle(conflict, &diagnostic), "immutable conflict");
  Check(store->LoadAuthorityBundle(&loaded, &diagnostic) && loaded.exact_bytes == exact, "conflict leaves bytes unchanged");
  store.reset();
  auto wrong = c; wrong.session_uuid.canonical = Text(200);
  store = e::MgaDmlDeleteDurableStoreV1::Open(wrong, b.descriptor.descriptor_uuid, 1, &diagnostic);
  Check(store && !store->LoadAuthorityBundle(&loaded, &diagnostic), "reopened foreign session refused");
  store.reset();
  store = e::MgaDmlDeleteDurableStoreV1::Open(c, b.descriptor.descriptor_uuid, 1, &diagnostic);
  Check(store && store->LoadAuthorityBundle(&loaded, &diagnostic) && loaded.exact_bytes == exact,
        "reopen exact owner and bytes");
  w::TypedDeleteJournalRecord bound;
  bound.descriptor = loaded.descriptor; bound.database_uuid = loaded.database_uuid;
  bound.journal_sequence = 1;
  bound.authenticated_statement_receipt_uuid = loaded.descriptor.authenticated_statement_receipt_uuid;
  bound.owning_transaction_uuid = loaded.descriptor.owning_transaction_uuid;
  bound.owning_local_transaction_id = loaded.descriptor.owning_local_transaction_id;
  bound.operation_uuid = loaded.descriptor.operation_uuid;
  bound.recovery_token_uuid = loaded.descriptor.recovery_token_uuid;
  bound.recovery_generation = loaded.descriptor.recovery_generation;
  Check(store->Append(bound, &diagnostic), "bundle precedes exact bound DDJR");
  w::TypedDeleteJournalRecord head;
  w::TypedDeleteCarrierError codec_error;
  Check(w::DecodeAndValidateTypedDeleteJournal(store->chain().back(), nullptr, &head, &codec_error), "decode bound");
  const auto marker_key = e::MgaSavepointUuidKey(Text(112));
  Check(!e::ObserveUniqueMgaSavepointMarkerV1(c, "SQL_label").ok &&
            !e::ObserveUniqueMgaSavepointMarkerV1(c, e::MgaSavepointUuidKey("malformed")).ok &&
            !e::ObserveUniqueMgaSavepointMarkerV1(c, e::MgaSavepointUuidKey("00000000-0000-0000-0000-000000000000")).ok,
        "reserved-marker observation requires a nonzero canonical native UUID");
  auto native = e::ObserveUniqueMgaSavepointMarkerV1(c, marker_key);
  Check(native.ok && native.lifecycle == e::MgaSavepointMarkerLifecycle::missing && !native.creation_ordinal,
        "missing reserved marker is not release");
  Check(!e::CreateMgaSavepointMarker(c, marker_key).error, "create reserved native marker");
  native = e::ObserveUniqueMgaSavepointMarkerV1(c, marker_key);
  Check(native.ok && native.lifecycle == e::MgaSavepointMarkerLifecycle::active && native.creation_ordinal,
        "discover actual reserved marker ordinal");
  auto intent = head; intent.lifecycle_state = w::TypedUpdateJournalState::intent;
  ++intent.journal_sequence; intent.prior_record_sha256 = head.record_evidence_sha256;
  intent.statement_savepoint_uuid = f::Uuid(201); intent.statement_savepoint_generation = native.creation_ordinal;
  Check(!store->Append(intent, &diagnostic) && store->chain().size() == 1,
        "DDJR cannot substitute a different native marker from immutable DDAB");
  intent.statement_savepoint_uuid = loaded.reserved_statement_savepoint_uuid;
  Check(store->Append(intent, &diagnostic), "append exact reserved marker intent");
  Check(store->AbortBeforePublication() == e::MgaDmlDeleteAbortStatusV1::aborted, "abort exact bound statement");
  Check(store->LoadAuthorityBundle(&loaded, &diagnostic) && loaded.exact_bytes == exact,
        "immutable bundle survives aborted journal");
  const auto repeated_key = e::MgaSavepointUuidKey(Text(202));
  Check(!e::CreateMgaSavepointMarker(c, repeated_key).error && !e::ReleaseMgaSavepointMarker(c, repeated_key).error &&
        !e::CreateMgaSavepointMarker(c, repeated_key).error, "duplicate-identity observation fixture");
  Check(!e::ObserveUniqueMgaSavepointMarkerV1(c, repeated_key).ok,
        "reserved UUID cannot silently select a replacement generation");
  Check(!e::ValidateMgaSavepointMarkerAuthority(c).error,
        "read-only unique observation does not alter ordinary marker history");
  store.reset();
  Check(std::filesystem::remove_all(root) > 0, "remove owned successful fixture");
}
int main() try {
  e::EngineApiDiagnostic diagnostic;
  for (unsigned profile = 0; profile < 3; ++profile) {
    const auto source = Bundle(profile);
    Bytes bytes;
    if (!e::EncodeDmlDeleteDurableAuthorityBundleV1(source, &bytes, &diagnostic))
      throw std::runtime_error("bundle encode");
    e::DmlDeleteDurableAuthorityBundleV1 decoded;
    Check(e::DecodeDmlDeleteDurableAuthorityBundleV1(bytes, &decoded, &diagnostic), "bundle decode");
    Bytes roundtrip;
    Check(e::EncodeDmlDeleteDurableAuthorityBundleV1(decoded, &roundtrip, &diagnostic) && roundtrip == bytes,
          "canonical round trip");
    Check(decoded.exact_bytes == bytes && decoded.descriptor.descriptor_uuid == source.descriptor.descriptor_uuid,
          "exact evidence");
    Check(std::equal(source.database_uuid.begin(), source.database_uuid.end(), bytes.begin() + 40), "binary database UUID");
    const auto database_text = Text(100);
    Check(std::search(bytes.begin(), bytes.end(), database_text.begin(), database_text.end()) == bytes.end(), "no text UUID");
    // Every single-byte corruption and every truncation must fail transactionally.
    for (std::size_t n = 0; n < bytes.size(); ++n) {
      auto corrupt = bytes; corrupt[n] ^= 1;
      auto sentinel = decoded;
      Check(!e::DecodeDmlDeleteDurableAuthorityBundleV1(corrupt, &sentinel, &diagnostic) &&
            sentinel.exact_bytes == bytes, "corruption and output atomicity");
      Check(!e::DecodeDmlDeleteDurableAuthorityBundleV1(std::span(bytes).first(n), &sentinel, &diagnostic), "truncation");
    }
    auto extra = bytes; extra.push_back(0);
    Check(!e::DecodeDmlDeleteDurableAuthorityBundleV1(extra, &decoded, &diagnostic), "trailing byte");
    // Re-encoding recalculates all hashes: these test cross-provider ownership,
    // not just detection of a stale checksum.
    const auto refuse = [&](auto mutate) {
      auto bad = source; mutate(bad); Bytes output{42};
      Check(!e::EncodeDmlDeleteDurableAuthorityBundleV1(bad, &output, &diagnostic) && output == Bytes{42},
            "cross-provider refusal before output publication");
    };
    refuse([](auto& b) { b.target_order.authenticated_statement_receipt_uuid = f::Uuid(201); });
    refuse([](auto& b) { ++b.target_order.target_relation_occurrence_generation; });
    refuse([](auto& b) { b.target_order.statement_snapshot_uuid = f::Uuid(201); });
    refuse([](auto& b) { ++b.target_order.maximum_candidate_rows; });
    refuse([](auto& b) { b.resource_budget.owning_transaction_uuid = f::Uuid(201); });
    refuse([](auto& b) { b.resource_budget.maximum_predicate_nodes = 0; });
    refuse([](auto& b) { b.resource_budget.maximum_trigger_depth = 65; });
    refuse([](auto& b) { b.recovery.durable_registry_uuid = f::Uuid(201); });
    refuse([](auto& b) { ++b.recovery.durable_registry_generation; });
    refuse([](auto& b) { b.reserved_statement_savepoint_uuid = {}; });
    refuse([](auto& b) { b.reserved_statement_savepoint_uuid = b.descriptor.descriptor_uuid; });
    refuse([](auto& b) { b.recovery.operation_uuid = f::Uuid(201); });
    refuse([](auto& b) { b.security.target_relation_uuid = Text(201); });
    refuse([](auto& b) { b.security.security_context_uuid = Text(201); });
    refuse([](auto& b) { ++b.security.security_generation; });
    refuse([](auto& b) { std::swap(b.matched_grant_uuids[0], b.matched_grant_uuids[1]); });
    refuse([](auto& b) { b.matched_grant_uuids[1] = b.matched_grant_uuids[0]; });
    refuse([](auto& b) { b.effects.constraint_count = 1; });
    refuse([](auto& b) { b.effects.trigger_count = 1; });
    refuse([](auto& b) { b.effects.constraint_set_uuid = b.effects.snapshot_uuid; });
    refuse([](auto& b) { b.effects.constraint_set_sha256.fill(1); });
    refuse([](auto& b) { b.effects.target_relation_uuid = f::Uuid(201); });
    refuse([](auto& b) { b.executor.installed = false; });
    refuse([](auto& b) { b.executor.database_uuid = Text(201); });
    refuse([](auto& b) { ++b.executor.generation; });
    refuse([](auto& b) { b.executor.row_identity_sha256 = e::HashSblrExecutorRowIdentityMaterial(
        {"dml.delete", 770, "1.0", "dml_delete_descriptor", "mutation_result", 1}); });
  }
  VerifyOwnerAndStorage();
  std::cout << "delete_bundle_codec_checks=" << checks << " status=passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "delete_bundle_codec_failed=" << error.what() << " checks=" << checks << '\n'; return 1;
}
