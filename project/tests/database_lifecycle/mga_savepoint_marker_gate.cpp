// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include "mga_relation_store/mga_publication_callback_scope.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "transaction/savepoint_api.hpp"
#include "sblr_savepoint_coordinator.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "mga_relation_store/mga_savepoint_marker_replay.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "catalog/catalog_object_lifecycle_codec.hpp"
#include "catalog/name_registry_codec.hpp"
#include "crud_support/crud_store.hpp"
#include "uuid.hpp"
#include "sblr_savepoint_runtime.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace {
int checks = 0;
int failures = 0;
void Check(bool value, const std::string& message) {
  ++checks;
  if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Fixture {
  scratchbird::tests::OwnedTempDirectory owned;
  std::filesystem::path root;
  api::EngineRequestContext context;
  Fixture() {
    static std::uint32_t ordinal = 10000;
    const auto identity = ordinal += 10;
    root = owned.path();
    context.database_path = (root / "markers.sbdb").string();
    namespace db = scratchbird::storage::database;
    namespace platform = scratchbird::core::platform;
    db::DatabaseCreateConfig config;
    config.path = context.database_path;
    config.database_uuid = {platform::UuidKind::database, scratchbird::tests::FixtureUuid(1606, identity)};
    config.filespace_uuid = {platform::UuidKind::filespace, scratchbird::tests::FixtureUuid(1606, identity + 1)};
    config.page_size = 16384;
    config.creation_unix_epoch_millis = 1791800000000ull;
    config.allow_minimal_resource_bootstrap = true;
    config.require_resource_seed_pack = false;
    const auto created = db::CreateDatabaseFile(config);
    if (!created.ok()) throw std::runtime_error("savepoint fixture native database creation failed");
    context.database_uuid = config.database_uuid.value;
    context.transaction_uuid = scratchbird::tests::FixtureUuid(1606, identity + 2);
    auto loaded = db::LoadLocalTransactionInventoryFromDatabase(context.database_path);
    if (!loaded.ok()) throw std::runtime_error("savepoint fixture native inventory load failed");
    // Retain the historical test coordinates, but actually allocate/publish
    // these transaction numbers instead of inventing an active owner.
    loaded.inventory.next_local_transaction_id = 17;
    const auto begun = scratchbird::transaction::mga::BeginLocalTransaction(std::move(loaded.inventory),
        {platform::UuidKind::transaction, context.transaction_uuid}, config.creation_unix_epoch_millis + 1);
    if (!begun.ok() || !db::PersistLocalTransactionInventoryToDatabase(context.database_path, begun.inventory).ok())
      throw std::runtime_error("savepoint fixture actual BEGIN publication failed");
    context.local_transaction_id = begun.entry.identity.local_id.value;
  }
  api::EngineRequestContext BeginOther(bool read_only = false) {
    namespace db = scratchbird::storage::database;
    namespace platform = scratchbird::core::platform;
    auto other = context;
    auto loaded = db::LoadLocalTransactionInventoryFromDatabase(context.database_path);
    if (!loaded.ok()) throw std::runtime_error("savepoint other owner inventory missing");
    other.transaction_uuid = api::GenerateCrudEngineUuid("transaction");
    const platform::TypedUuid identity{platform::UuidKind::transaction, other.transaction_uuid};
    const auto begun = read_only
        ? scratchbird::transaction::mga::BeginLocalReadOnlyTransaction(std::move(loaded.inventory), identity, 1791800000002ull)
        : scratchbird::transaction::mga::BeginLocalTransaction(std::move(loaded.inventory), identity, 1791800000002ull);
    if (!begun.ok() || !db::PersistLocalTransactionInventoryToDatabase(context.database_path, begun.inventory).ok())
      throw std::runtime_error("savepoint other actual BEGIN publication failed");
    other.local_transaction_id = begun.entry.identity.local_id.value;
    return other;
  }
  ~Fixture() noexcept {
    try { owned.Cleanup(); }
    catch (const std::exception& error) { Check(false, std::string("fixture cleanup failed: ") + error.what()); }
  }
  std::filesystem::path Path() const {
    return context.database_path + ".sb.mga_savepoints";
  }
  void Write(const std::string& bytes) {
    std::ofstream out(Path(), std::ios::binary | std::ios::trunc);
    out << bytes;
  }
};

void CurrentOwnerAdmission() {
  Fixture f;
  Check(!api::CreateMgaSavepointMarker(f.context, "bound").error, "actual owner marker prerequisite");
  const auto capture = [&] {
    std::vector<std::string> bytes;
    for (const auto suffix : {".sb.mga_savepoints", ".sb.mga_event_sequence_allocator",
                             ".sb.mga_relation_metadata", ".sb.catalog_object_events", ".sb.name_events.v2"}) {
      std::ifstream in(f.context.database_path + suffix, std::ios::binary);
      bytes.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return bytes;
  };
  auto before = capture();
  const auto next = api::NextMetadataEventSequence(f.context);
  const auto refuse = [&](const auto& context, const std::string& label) {
    for (auto action : {api::CreateMgaSavepointMarker, api::ReleaseMgaSavepointMarker,
                        api::RollbackToMgaSavepointMarker}) {
      Check(action(context, "bound").error, label);
      Check(capture() == before && api::NextMetadataEventSequence(f.context) == next,
            "refused native marker owner changed a journal or allocator");
    }
  };
  auto foreign = f.context;
  foreign.database_uuid = api::GenerateCrudEngineUuid("object");
  refuse(foreign, "foreign database owner refuses every marker mutation");
  foreign = f.context; foreign.transaction_uuid = api::GenerateCrudEngineUuid("transaction");
  refuse(foreign, "foreign binary transaction owner refuses every marker mutation");
  foreign = f.context; foreign.local_transaction_id = 90000;
  refuse(foreign, "unallocated transaction number refuses every marker mutation");
  foreign = f.context;
  foreign.database_path = (f.root / "." / "markers.sbdb").string();
  foreign.database_uuid = api::GenerateCrudEngineUuid("object");
  for (auto action : {api::CreateMgaSavepointMarker, api::ReleaseMgaSavepointMarker,
                      api::RollbackToMgaSavepointMarker}) {
    api::MgaPublicationCallbackScope scope(f.context);
    Check(action(foreign, "bound").error && scope.savepoint_mutation_refused(),
          "actual node identity blocks/latches alias plus forged UUID callback");
    Check(capture() == before && api::NextMetadataEventSequence(f.context) == next,
          "aliased callback mutation changed a marker cutoff or journal");
  }
  auto terminal = f.BeginOther();
  Check(!api::CreateMgaSavepointMarker(terminal, "bound").error, "terminal owner actual marker prerequisite");
  before = capture();
  namespace db = scratchbird::storage::database;
  const auto retained = db::AcquireLocalTransactionInventorySnapshot(terminal.database_path);
  if (!retained.ok()) throw std::runtime_error("terminal marker test snapshot unavailable");
  terminal.statement_transaction_inventory_snapshot = retained.snapshot;
  const auto committed = scratchbird::transaction::mga::CommitLocalTransaction(
      retained.snapshot->inventory, scratchbird::transaction::mga::MakeLocalTransactionId(terminal.local_transaction_id),
      1791800000003ull);
  if (!committed.ok() || !db::PersistLocalTransactionInventoryToDatabase(terminal.database_path, committed.inventory).ok())
    throw std::runtime_error("terminal marker test actual COMMIT failed");
  refuse(terminal, "retained active snapshot cannot authorize marker after actual COMMIT");
  for (auto action : {api::CreateMgaSavepointMarker, api::ReleaseMgaSavepointMarker,
                      api::RollbackToMgaSavepointMarker})
    Check(action(terminal, "bound").detail.find("active_local_transaction_required") != std::string::npos,
          "terminal refusal is native finality, not a missing marker");
  const auto current = db::AcquireLocalTransactionInventorySnapshot(f.context.database_path);
  if (!current.ok()) throw std::runtime_error("rollback-only marker native inventory missing");
  const auto marked = scratchbird::transaction::mga::MarkLocalTransactionRollbackOnly(current.snapshot->inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(f.context.local_transaction_id));
  if (!marked.ok() || !db::PersistLocalTransactionInventoryToDatabase(f.context.database_path, marked.inventory).ok())
    throw std::runtime_error("rollback-only marker actual native publication failed");
  refuse(f.context, "actual persisted rollback-only fence refuses all marker mutations");
  for (auto action : {api::CreateMgaSavepointMarker, api::ReleaseMgaSavepointMarker,
                      api::RollbackToMgaSavepointMarker})
    Check(action(f.context, "bound").detail.find("transaction_rollback_only") != std::string::npos,
          "marker must preserve the actual native exclusion identity");
  auto reader = f.BeginOther(true);
  Check(!api::CreateMgaSavepointMarker(reader, "readonly").error &&
            !api::RollbackToMgaSavepointMarker(reader, "readonly").error &&
            !api::ReleaseMgaSavepointMarker(reader, "readonly").error,
        "genuine native read-only transaction retains savepoint control");
}

void Stack() {
  Fixture f;
  Check(!api::CreateMgaSavepointMarker(f.context, "outer").error, "create outer");
  Check(!api::CreateMgaSavepointMarker(f.context, "inner").error, "create inner");
  auto parsed = api::ParseSavepoints(f.context);
  const auto& active = parsed.active_savepoints.at(17);
  Check(active.at("outer").row_event_sequence == active.at("inner").row_event_sequence,
        "adjacent savepoints share row cutoff");
  Check(active.at("outer").creation_ordinal < active.at("inner").creation_ordinal,
        "creation order is independent of row cutoff");
  auto other = f.BeginOther();
  Check(!api::CreateMgaSavepointMarker(other, "other").error, "create other transaction");
  Check(!api::RollbackToMgaSavepointMarker(f.context, "outer").error, "rollback outer");
  Check(api::ValidateMgaSavepointExists(f.context, "inner", "test").error,
        "rollback invalidates inner");
  Check(!api::ValidateMgaSavepointExists(f.context, "outer", "test").error,
        "rollback retains target");
  Check(!api::ValidateMgaSavepointExists(other, "other", "test").error,
        "rollback preserves other transaction");
  Check(!api::RollbackToMgaSavepointMarker(f.context, "outer").error, "repeat rollback");
  Check(!api::CreateMgaSavepointMarker(f.context, "later").error, "create after rollback");
  Check(!api::ReleaseMgaSavepointMarker(f.context, "later").error, "release named boundary");
  Check(!api::ValidateMgaSavepointExists(f.context, "outer", "test").error,
        "release does not erase outer");
  Check(!api::CreateMgaSavepointMarker(f.context, "inner").error, "recreate inner");
  Check(!api::CreateMgaSavepointMarker(f.context, "outer").error, "replace named boundary");
  Check(!api::RollbackToMgaSavepointMarker(f.context, "inner").error, "rollback older inner");
  Check(api::ValidateMgaSavepointExists(f.context, "outer", "test").error,
        "replacement has fresh creation order");
  Check(!api::ValidateMgaSavepointMarkerAuthority(f.context).error,
        "reparsed successful history remains valid");
}

void MarkerObservation() {
  Fixture f;
  const auto key = api::MgaSavepointUuidKey(scratchbird::tests::FixtureUuidLiteral("019f2100-0000-7000-8000-00000000de01"));
  Check(!api::CreateMgaSavepointMarker(f.context, "parent").error, "observation parent create");
  Check(!api::CreateMgaSavepointMarker(f.context, key).error, "observation native create");
  const auto generation = api::ParseSavepoints(f.context).active_savepoints.at(17).at(key).creation_ordinal;
  auto observe = [&] { return api::ObserveMgaSavepointMarker(f.context, key, generation); };
  auto observed = observe();
  Check(observed.ok && observed.lifecycle == api::MgaSavepointMarkerLifecycle::active &&
            !observed.rolled_back && observed.cutoffs.creation_ordinal == generation,
        "exact native generation observed active");
  auto other = f.context;
  other.local_transaction_id = 18;
  Check(api::ObserveMgaSavepointMarker(other, key, generation).lifecycle ==
            api::MgaSavepointMarkerLifecycle::missing, "observation transaction isolation");
  Check(!api::ObserveMgaSavepointMarker(f.context, key, 0).ok, "zero generation observation refused");
  Check(api::ObserveMgaSavepointMarker(f.context, key, generation + 1).lifecycle ==
            api::MgaSavepointMarkerLifecycle::missing, "unknown generation is not a release");
  Check(!api::ReleaseMgaSavepointMarker(f.context, key).error, "observation native release");
  observed = observe();
  Check(observed.ok && observed.lifecycle == api::MgaSavepointMarkerLifecycle::released &&
            !observed.rolled_back, "explicit release observed from retained history");
  Check(!api::RollbackToMgaSavepointMarker(f.context, "parent").error, "rollback after explicit release");
  Check(observe().lifecycle == api::MgaSavepointMarkerLifecycle::released,
        "later outer rollback does not rewrite historical release evidence");
  Check(!api::CreateMgaSavepointMarker(f.context, key).error, "reuse observed identity");
  const auto next_generation = api::ParseSavepoints(f.context).active_savepoints.at(17).at(key).creation_ordinal;
  Check(next_generation > generation && observe().lifecycle == api::MgaSavepointMarkerLifecycle::released,
        "identity reuse does not replace old generation observation");
  Check(!api::RollbackToMgaSavepointMarker(f.context, key).error, "rollback native boundary");
  Check(!api::ReleaseMgaSavepointMarker(f.context, key).error, "release rolled-back boundary");
  observed = api::ObserveMgaSavepointMarker(f.context, key, next_generation);
  Check(observed.ok && observed.lifecycle == api::MgaSavepointMarkerLifecycle::released && observed.rolled_back,
        "rollback then release cannot masquerade as successful publication");
  Check(!api::CreateMgaSavepointMarker(f.context, key).error, "create descendant for invalidation");
  const auto invalidated_generation = api::ParseSavepoints(f.context).active_savepoints.at(17).at(key).creation_ordinal;
  Check(!api::RollbackToMgaSavepointMarker(f.context, "parent").error, "invalidate descendant");
  observed = api::ObserveMgaSavepointMarker(f.context, key, invalidated_generation);
  Check(observed.ok && observed.lifecycle == api::MgaSavepointMarkerLifecycle::invalidated,
        "ancestor rollback is not explicit descendant release");
  Check(!api::CreateMgaSavepointMarker(f.context, key).error, "create replacement candidate");
  const auto replaced_generation = api::ParseSavepoints(f.context).active_savepoints.at(17).at(key).creation_ordinal;
  Check(!api::CreateMgaSavepointMarker(f.context, key).error, "replace live observed boundary");
  observed = api::ObserveMgaSavepointMarker(f.context, key, replaced_generation);
  Check(observed.ok && observed.lifecycle == api::MgaSavepointMarkerLifecycle::invalidated,
        "name replacement is not explicit release");
  std::ofstream tail(f.Path(), std::ios::binary | std::ios::app);
  tail << "torn"; tail.close();
  observed = observe();
  Check(!observed.ok && observed.diagnostic.code == "MGA.SAVEPOINT.AUTHORITY_CORRUPT",
        "later corruption invalidates apparently successful historical release");
}

void Ranges() {
  Fixture f;
  const std::string create = "SBMGA1\tSAVEPOINT\t17\t61\t10\t20\t30\n";
  f.Write(create + "SBMGA1\tROLLBACK_TO_SAVEPOINT\t17\t61\t10\t20\t30\t12\t22\t32\n");
  const auto state = api::ParseSavepoints(f.context);
  Check(!state.marker_authority_corrupt, "valid bounded rollback replay");
  api::BoundedScopedRowReadControl control;
  api::SavepointParsedState bounded;
  Check(api::ParseSavepointsBounded(f.context, &control, 0, &bounded),
        "bounded reader accepts valid rollback stream");
  Check(!api::RowEventRolledBackBySavepoint(state, 17, 10), "row cutoff retained");
  Check(api::RowEventRolledBackBySavepoint(state, 17, 11), "row in range hidden");
  Check(!api::RowEventRolledBackBySavepoint(state, 17, 13), "later row retained");
  Check(api::MetadataEventRolledBackBySavepoint(state, 17, 22), "metadata in range hidden");
  Check(!api::MetadataEventRolledBackBySavepoint(state, 17, 23), "later metadata retained");
  Check(api::IndexEventRolledBackBySavepoint(state, 17, 32), "index in range hidden");
  Check(!api::IndexEventRolledBackBySavepoint(state, 17, 33), "later index retained");
  Check(!api::RowEventRolledBackBySavepoint(state, 18, 11), "other transaction row retained");
}

void Coordinator() {
  Fixture f;
  auto& c = f.context;
  c.statement_uuid = scratchbird::tests::FixtureUuidLiteral("12340000-0000-7000-8000-000000000003");
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  c.trace_tags = {"private_savepoint_coordination"};
  const std::string hash = "sha256:" + std::string(64, 'a');
  const auto activate = [&](std::uint64_t occurrence) {
    const auto reserved = api::ReserveSblrSavepoint(c, c.statement_uuid,
                                                   hash, occurrence, hash);
    Check(reserved.ok, "coordinator reserve");
    const auto& s = reserved.snapshot;
    return api::ActivateSblrSavepoint(c, c.statement_uuid,
        s.descriptor_uuid, s.descriptor_generation, s.descriptor_evidence_sha256, 1);
  };
  auto outer = activate(1);
  auto inner = activate(2);
  Check(outer.ok && inner.ok, "coordinator activates two boundaries");
  if (!outer.ok || !inner.ok) return;
  auto target = outer.snapshot;
  Check(!api::ValidateMgaSavepointExists(c, api::MgaSavepointUuidKey(target.savepoint_uuid), "test").error,
        "native activation creates an MGA storage boundary");
  auto rolled = api::RollbackToSblrSavepoint(c, target.savepoint_uuid,
      target.savepoint_generation, target.transaction_ordinal,
      target.stack_generation, hash, 1);
  Check(rolled.ok, "native rollback completes storage operation");
  Check(api::ValidateMgaSavepointExists(c, api::MgaSavepointUuidKey(inner.snapshot.savepoint_uuid), "test").error,
        "native rollback invalidates inner storage boundary");
  auto state = api::ParseSavepoints(c);
  Check(state.rollback_ranges.contains(17) && state.rollback_ranges.at(17).size() == 1,
        "native rollback records a storage rollback range");
  target = rolled.snapshot;
  const auto released = api::ReleaseSblrSavepoint(c, target.savepoint_uuid,
      target.savepoint_generation, target.transaction_ordinal,
      target.stack_generation, hash, 1);
  Check(released.ok, "native release succeeds");
  Check(api::ValidateMgaSavepointExists(c, api::MgaSavepointUuidKey(target.savepoint_uuid), "test").error,
        "native release removes storage boundary");
  std::ifstream input(c.database_path + ".sb.sblr_savepoint_coordinator.v2", std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(input)), {});
  Check(!bytes.empty() && bytes.size() % 256 == 0, "fixed-size binary coordinator records");
  Check(bytes.find(scratchbird::core::uuid::UuidToString(c.transaction_uuid)) == std::string::npos &&
        bytes.find(scratchbird::core::uuid::UuidToString(target.savepoint_uuid)) == std::string::npos &&
        bytes.find(scratchbird::core::uuid::UuidToString(c.statement_uuid)) == std::string::npos,
        "coordinator disk records contain no textual system UUIDs");
  const auto uuid = c.transaction_uuid;
  Check(!uuid.is_nil() && bytes.substr(48, 16) == std::string(
      reinterpret_cast<const char*>(uuid.bytes.data()), 16),
      "coordinator transaction UUID occupies exactly binary(16)");
}

void BinaryCodec() {
  api::MgaSavepointMarkerRecord record;
  record.kind = 1;
  record.transaction = 17;
  record.uuid_identity = true;
  record.uuid = scratchbird::tests::FixtureUuidLiteral("12340000-0000-7000-8000-000000000009");
  const auto bytes = api::EncodeMgaSavepointMarker(record);
  const auto uuid = record.uuid;
  Check(bytes.size() == 124 && api::MgaSavepointMarkerFrameSize(bytes) == 124,
        "native binary marker has fixed frame size");
  Check(bytes.find(scratchbird::core::uuid::UuidToString(record.uuid)) == std::string::npos, "marker has no textual UUID copy");
  Check(!uuid.is_nil() && bytes.substr(76, 16) == std::string(
      reinterpret_cast<const char*>(uuid.bytes.data()), 16),
      "marker UUID occupies exactly binary(16)");
  api::MgaSavepointMarkerRecord decoded;
  Check(api::DecodeMgaSavepointMarker(bytes, &decoded) && decoded.uuid_identity &&
        decoded.uuid == record.uuid && decoded.identity.empty() && decoded.transaction == 17,
        "binary UUID marker round trip");
  for (std::size_t size = 0; size < bytes.size(); ++size)
    Check(!api::DecodeMgaSavepointMarker(std::string_view(bytes).substr(0, size), &decoded),
          "truncated binary frame refused");
  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    auto corrupt = bytes;
    corrupt[offset] ^= 1;
    Check(!api::DecodeMgaSavepointMarker(corrupt, &decoded), "corrupt binary frame refused");
  }
  record.uuid_identity = false;
  record.uuid = {};
  record.identity = "user savepoint label";
  Check(api::DecodeMgaSavepointMarker(api::EncodeMgaSavepointMarker(record), &decoded) &&
        !decoded.uuid_identity && decoded.identity == record.identity,
        "user labels remain distinct from system UUID fields");
  Fixture mixed;
  mixed.Write("SBMGA1\tSAVEPOINT\t17\t61\t0\t0\t0\n" + bytes);
  auto state = api::ParseSavepoints(mixed.context);
  Check(!state.marker_authority_corrupt && state.active_savepoints.at(17).size() == 2,
        "legacy named marker followed by binary native marker replays in order");
  mixed.Write(bytes.substr(0, bytes.size() - 1));
  Check(api::ValidateMgaSavepointMarkerAuthority(mixed.context).error,
        "torn binary marker file refuses storage operations");
}

void WireUuidCodec() {
  namespace wire = scratchbird::engine::sblr;
  static_assert(sizeof(wire::SpUuid) == 16);
  const std::string text = "12340000-0000-7000-8000-000000000009";
  const auto uuid = scratchbird::core::uuid::ParseUuid(text).value;
  const std::string raw(reinterpret_cast<const char*>(uuid.bytes.data()), 16);
  wire::SblrSavepointCoordinationRequestV1 request;
  request.preliminary_receipt_uuid = uuid.bytes;
  request.transaction_uuid = uuid.bytes;
  request.local_transaction_id = 17;
  request.transaction_handle_evidence_sha256.fill(1);
  request.symbol_occurrence_id = 1;
  request.canonical_symbol_sha256.fill(2);
  const auto encoded = wire::EncodeSblrSavepointCoordinationRequestV1(request);
  const std::string bytes(encoded.begin(), encoded.end());
  Check(bytes.size() == 128 && bytes.substr(16, 16) == raw &&
        bytes.substr(32, 16) == raw && bytes.find(text) == std::string::npos,
        "parser/server coordination request carries binary(16) UUIDs only");
  wire::SblrSavepointCoordinationRequestV1 decoded;
  Check(wire::DecodeSblrSavepointCoordinationRequestV1(
            encoded.data(), encoded.size(), &decoded, nullptr) &&
        decoded.transaction_uuid == request.transaction_uuid &&
        decoded.preliminary_receipt_uuid == request.preliminary_receipt_uuid,
        "binary coordination UUID request round trip");
  wire::SblrSavepointDescriptorV1 descriptor;
  descriptor.descriptor_uuid = descriptor.savepoint_uuid =
      descriptor.transaction_uuid = uuid.bytes;
  descriptor.descriptor_generation = descriptor.savepoint_generation = 1;
  descriptor.local_transaction_id = 17;
  descriptor.transaction_ordinal = 1;
  descriptor.descriptor_evidence_sha256.fill(3);
  const auto operand = wire::EncodeSblrSavepointDescriptorV1(descriptor);
  const std::string body(operand.begin(), operand.end());
  Check(body.size() == 128 && body.substr(16, 16) == raw &&
        body.substr(40, 16) == raw && body.substr(64, 16) == raw &&
        body.find(text) == std::string::npos,
        "executable savepoint descriptor carries three binary(16) UUIDs");
  wire::SblrSavepointDescriptorV1 result;
  Check(wire::DecodeSblrSavepointDescriptorV1(
            operand.data(), operand.size(), &result, nullptr) &&
        result.descriptor_uuid == descriptor.descriptor_uuid &&
        result.savepoint_uuid == descriptor.savepoint_uuid &&
        result.transaction_uuid == descriptor.transaction_uuid,
        "binary executable savepoint descriptor round trip");
}

void BinaryReplay() {
  Fixture f;
  std::string stream;
  std::vector<std::string> keys;
  auto add = [&](const scratchbird::core::platform::Uuid& uuid) {
    api::MgaSavepointMarkerRecord marker;
    marker.kind = 1;
    marker.uuid_identity = true;
    marker.uuid = uuid;
    marker.transaction = 17;
    keys.push_back(api::MgaSavepointUuidKey(uuid));
    stream += api::EncodeMgaSavepointMarker(marker);
  };
  const auto base = scratchbird::tests::FixtureUuidLiteral("01000000-0000-7000-8000-000000000000");
  add(base);
  for (unsigned byte = 0; byte < 16; ++byte)
    for (unsigned bit = 0; bit < 8; ++bit) {
      if ((byte == 6 && bit >= 4) || (byte == 8 && bit >= 6)) continue;
      auto uuid = base;
      uuid.bytes[byte] ^= static_cast<std::uint8_t>(1u << bit);
      add(uuid);
    }
  Check(keys.size() == 123 && stream.size() == 123 * 124,
        "binary marker history has every variable v7 bit and exact frame widths");
  for (const std::size_t chunk : {1u, 7u, 15u, 16u, 23u, 124u, 4096u}) {
    api::SavepointParsedState state;
    std::string pending;
    for (std::size_t offset = 0; offset < stream.size(); offset += chunk) {
      pending.append(stream, offset, std::min(chunk, stream.size() - offset));
      api::savepoint_replay::ConsumeMarkerBytes(&pending, &state, false);
    }
    api::savepoint_replay::ConsumeMarkerBytes(&pending, &state, true);
    Check(!state.marker_authority_corrupt && pending.empty() &&
              state.active_savepoints[17].size() == keys.size(),
          "chunked native replay preserves exact UUID key cohort");
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const auto found = state.active_savepoints[17].find(keys[i]);
      Check(found != state.active_savepoints[17].end() &&
                found->second.creation_ordinal == i + 1,
            "native marker ordinal follows creation order, independently of UUID bits");
    }
  }
  // The historical label decoder must not restore native authority from hex.
  f.Write("SBMGA1\tSAVEPOINT\t17\t0001000000000070008000000000000000\t0\t0\t0\n");
  Check(api::ValidateMgaSavepointMarkerAuthority(f.context).error,
        "historical text record cannot introduce a native UUID marker");
}

void Corruption() {
  const std::string valid = "SBMGA1\tSAVEPOINT\t17\t61\t0\t0\t0\n";
  const std::vector<std::string> corrupt = {
      "garbage\n", "\n", "SBMGA1\tUNKNOWN\t17\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t17x\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t0\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t17\t6z\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t17\t61\t-1\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t17\t61\t18446744073709551616\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t17\t61\t0x\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t17\t61\t0\t0\t0",
      "SBMGA1\tSAVEPOINT\t17\t61\t0\t0\t0\textra\n",
      "SBMGA1\tROLLBACK_TO_SAVEPOINT\t17\t62\t0\t0\t0\t1\t1\t1\n",
      "SBMGA1\tROLLBACK_TO_SAVEPOINT\t17\t61\t1\t0\t0\t0\t1\t1\n",
      "SBMGA1\tROLLBACK_TO_SAVEPOINT\t17\t61\t1\t0\t0\t2\t1\t1\n",
      "SBMGA1\tRELEASE_SAVEPOINT\t17\t62\t0\t0\t0\n",
  };
  for (std::size_t i = 0; i < corrupt.size(); ++i) {
    Fixture f;
    f.Write(valid + corrupt[i]);
    const auto bytes = std::filesystem::file_size(f.Path());
    const auto diagnostic = api::ValidateMgaSavepointMarkerAuthority(f.context);
    Check(diagnostic.error && diagnostic.code == "MGA.SAVEPOINT.AUTHORITY_CORRUPT",
          "corruption diagnostic " + std::to_string(i));
    Check(api::CreateMgaSavepointMarker(f.context, "new").error, "create refuses corrupt stream");
    Check(api::RollbackToMgaSavepointMarker(f.context, "a").error, "rollback refuses corrupt stream");
    Check(api::ReleaseMgaSavepointMarker(f.context, "a").error, "release refuses corrupt stream");
    Check(std::filesystem::file_size(f.Path()) == bytes, "refusal writes no marker bytes");
    api::BoundedScopedRowReadControl control;
    api::SavepointParsedState parsed;
    Check(!api::ParseSavepointsBounded(f.context, &control, 0, &parsed),
          "bounded reader refuses corrupt stream");
    Check(control.refusal_detail == "heap_read_savepoint_authority_corrupt",
          "bounded refusal identifies corrupt marker authority");
  }
  Fixture directory;
  std::filesystem::create_directory(directory.Path());
  Check(api::ValidateMgaSavepointMarkerAuthority(directory.context).error,
        "directory cannot masquerade as missing marker file");
}

void MetadataSequenceAuthority() {
  Fixture f;
  api::EngineCatalogObjectRecord object;
  object.creator_tx=17;
  object.object_uuid=scratchbird::tests::FixtureUuid(1606,1);
  object.catalog_row_uuid=scratchbird::tests::FixtureUuid(1606,2);
  object.object_kind="table";
  object.lifecycle_state="active";
  std::string catalog;
  Check(api::EncodeCatalogLifecycleRecord(object,&catalog,41), "encode sequenced catalog frame");
  api::ApiBehaviorRecord name;
  name.creator_tx=17;name.object_uuid=object.object_uuid;
  name.target_object_uuid=object.object_uuid;
  name.object_kind="name_registry.retire";name.operation_id="catalog.retire";name.state="retired";
  name.event_sequence=57;
  std::string names;
  Check(api::EncodeApiBehaviorRecord(name,&names), "encode sequenced name frame");
  api::NameRegistryEntry entry;
  entry.creator_tx=17;entry.name_entry_uuid=scratchbird::tests::FixtureUuid(1606,3);
  entry.object_uuid=object.object_uuid;entry.scope_uuid=scratchbird::tests::FixtureUuid(1606,4);
  entry.object_class="table";entry.lifecycle_state="active";entry.raw_name_text="sequence fixture";
  api::ApiBehaviorRecord entry_frame;
  entry_frame.creator_tx=entry.creator_tx;entry_frame.object_uuid=entry.name_entry_uuid;
  entry_frame.target_object_uuid=entry.object_uuid;entry_frame.target_schema_uuid=entry.scope_uuid;
  entry_frame.object_kind="name_registry.entry";entry_frame.operation_id="catalog.name";
  entry_frame.state="active";entry_frame.event_sequence=53;
  Check(api::EncodeNameRegistryEntry(entry,&entry_frame.payload), "encode complete name entry body");
  std::string entry_bytes;
  Check(api::EncodeApiBehaviorRecord(entry_frame,&entry_bytes), "encode sequenced name entry");
  names+=entry_bytes; // A lower sequence appended later must not reduce the maximum.
  entry_frame.event_sequence=0;
  Check(api::EncodeApiBehaviorRecord(entry_frame,&entry_bytes), "encode legacy name entry");
  names+=entry_bytes;
  const auto write = [](const std::string& path, const std::string& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(bytes.data(),bytes.size());out.close();
    Check(static_cast<bool>(out), "write native sequence fixture");
  };
  write(f.context.database_path+".sb.catalog_object_events",catalog);
  write(f.context.database_path+".sb.name_events.v2",names);
  Check(api::ScanNextMetadataEventSequence(f.context)==58,
        "metadata high water includes both native catalog journals");
  Check(!api::CreateMgaSavepointMarker(f.context,"cutoff").error,
        "create captures sequenced catalog high water");
  auto parsed=api::ParseSavepoints(f.context);
  Check(parsed.active_savepoints.at(17).at("cutoff").metadata_event_sequence==57,
        "catalog and name frames share the metadata cutoff domain");
  const auto marker_size=std::filesystem::file_size(f.Path());
  for (const auto kind : {"relation_metadata", "row_versions", "index_entries"}) {
    const auto suffix=std::string_view(kind)=="relation_metadata" ? ".sb.mga_relation_metadata" :
        (std::string_view(kind)=="row_versions" ? ".sb.mga_row_versions" : ".sb.mga_index_entries");
    const auto reserved=api::ReserveEventSequenceRange(f.context,kind,f.context.database_path+suffix,3,
        [&] { return std::string_view(kind)=="relation_metadata" ? api::ScanNextMetadataEventSequence(f.context) : 1; });
    Check(reserved.ok, "populate real reservation cache before corrupt observations");
  }
  const auto refused = [&] {
    for (auto action : {api::CreateMgaSavepointMarker,api::ReleaseMgaSavepointMarker,
                        api::RollbackToMgaSavepointMarker}) {
      const auto result=action(f.context,"cutoff");
      Check(result.error && result.detail.find("event_sequence_authority_invalid")!=std::string::npos,
            "all marker writers refuse invalid sequence authority");
      Check(std::filesystem::file_size(f.Path())==marker_size,
            "failed sequence observation appends no marker");
    }
  };
  for (const auto suffix : {".sb.catalog_object_events", ".sb.name_events.v2",
                           ".sb.mga_relation_metadata"}) {
    const std::string path=f.context.database_path+suffix;
    const std::string original=std::string_view(suffix)==".sb.catalog_object_events" ? catalog :
        (std::string_view(suffix)==".sb.name_events.v2" ? names : std::string{});
    write(path,original+"torn");
    Check(api::ScanNextMetadataEventSequence(f.context)==0,
          "corrupt journal cannot produce an allocation high water");
    Check(api::NextMetadataEventSequence(f.context)==0,
          "invalid scan cannot become a zero cutoff through fallback");
    refused();
    write(path,original);
  }
  for (const auto suffix : {".sb.mga_row_versions", ".sb.mga_index_entries"}) {
    const auto path=f.context.database_path+suffix;
    write(path,"torn");
    Check((std::string_view(suffix)==".sb.mga_row_versions" ? api::NextRowEventSequence(f.context) :
        api::NextIndexEventSequence(f.context))==0,"invalid row/index scan beats a warm reservation cache");
    refused();write(path,{});
  }
  const auto scope=f.context.database_path+".sb.mga_relation_scope";
  std::filesystem::create_directory(scope);
  write(scope+"/locators.v2","torn");
  Check(api::NextRowEventSequence(f.context)==0 && api::NextIndexEventSequence(f.context)==0,
        "malformed binary relation locator returns invalid observation rather than escaping marker diagnostics");
  refused();std::filesystem::remove(scope+"/locators.v2");std::filesystem::remove(scope);
  for (const auto suffix : {".sb.catalog_object_events", ".sb.name_events.v2"}) {
    const auto path=f.context.database_path+suffix;
    const auto& original=std::string_view(suffix)==".sb.catalog_object_events" ? catalog : names;
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    Check(api::NextMetadataEventSequence(f.context)==0,"nonregular journal is not an empty journal");
    refused();std::filesystem::remove(path);write(path,original);
    std::filesystem::permissions(path,std::filesystem::perms::none);
    {
      std::ifstream probe(path,std::ios::binary);
      if (!probe) {
        Check(api::NextMetadataEventSequence(f.context)==0,"unreadable journal is not absent");
        refused();
      } else {
        // Root/DAC-override hosts may still read mode000 files. Test actual
        // access, not an assumption that permission bits overrule that grant.
        Check(api::NextMetadataEventSequence(f.context)==61,"effective host read permission retains valid authority");
      }
    }
    std::filesystem::permissions(path,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
    std::filesystem::remove(path);std::filesystem::create_symlink(f.Path(),path);
    Check(api::NextMetadataEventSequence(f.context)==0,"symlink is not owned journal authority");
    refused();std::filesystem::remove(path);write(path,original);
  }
  api::ApiBehaviorRecord catalog_frame;
  Check(api::DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(catalog.data()),catalog.size()},&catalog_frame),
        "decode catalog envelope for independently resealed body mismatch");
  for (const bool is_catalog : {true,false}) {
    auto frame=is_catalog ? catalog_frame : entry_frame;
    frame.event_sequence=59;
    frame.object_uuid=scratchbird::tests::FixtureUuid(1606,9);
    std::string bad;
    Check(api::EncodeApiBehaviorRecord(frame,&bad),"reseal valid envelope with wrong nested identity");
    const auto path=f.context.database_path+(is_catalog ? ".sb.catalog_object_events" : ".sb.name_events.v2");
    write(path,bad);
    Check(api::NextMetadataEventSequence(f.context)==0,"outer digest does not qualify mismatched native body");
    refused();write(path,is_catalog ? catalog : names);
  }
  name.event_sequence=75;
  Check(api::EncodeApiBehaviorRecord(name,&names), "encode effect beyond retained reservation journal");
  write(f.context.database_path+".sb.name_events.v2",names);
  api::ClearMgaEventSequenceRangeCacheForTesting();
  const auto recovered=api::ReserveEventSequenceRange(f.context,"relation_metadata",
      f.context.database_path+".sb.mga_relation_metadata",1,[&] { return api::ScanNextMetadataEventSequence(f.context); });
  Check(recovered.ok && recovered.first==76,"cold reservation includes actual journal effects beyond durable reservation");
  name.event_sequence=std::numeric_limits<std::uint64_t>::max();
  Check(api::EncodeApiBehaviorRecord(name,&names), "encode exhausted sequence domain");
  write(f.context.database_path+".sb.name_events.v2",names);
  Check(api::NextMetadataEventSequence(f.context)==0,
        "exhausted metadata domain cannot wrap to one");
  Check(api::CreateMgaSavepointMarker(f.context,"exhausted").error &&
        std::filesystem::file_size(f.Path())==marker_size,
        "exhaustion writes no manufactured cutoff");
  api::ClearMgaEventSequenceRangeCacheForTesting();
  const auto exhausted=api::ReserveEventSequenceRange(f.context,"relation_metadata",
      f.context.database_path+".sb.mga_relation_metadata",1,[&] { return api::ScanNextMetadataEventSequence(f.context); });
  Check(!exhausted.ok,"cold reservation cannot hide exhausted source behind valid allocator record");
}

void AllocatorSequenceAuthority() {
  Fixture f;
  api::ClearMgaEventSequenceRangeCacheForTesting();
  const auto stream=f.context.database_path+".sb.mga_row_versions";
  const auto allocator=f.context.database_path+".sb.mga_event_sequence_allocator";
  std::vector<std::string> first_lines,second_lines;
  const auto first=api::ReserveEventSequenceRange(f.context,"row_versions",stream,2,[]{return 1;},&first_lines);
  const auto second=api::ReserveEventSequenceRange(f.context,"row_versions",stream,4,[]{return 1;},&second_lines);
  Check(first.ok && second.ok && first.first==1 && second.first==3,"reserve disjoint deferred ranges");
  const auto first_line=first_lines.at(0),second_line=second_lines.at(0);
  Check(api::AppendDeferredEventSequenceAllocatorLines(f.context,&second_lines,nullptr),"publish later range first");
  Check(api::AppendDeferredEventSequenceAllocatorLines(f.context,&first_lines,nullptr),"publish earlier range last");
  const auto write=[&](const std::string& bytes) {
    std::ofstream out(allocator,std::ios::binary|std::ios::trunc);out<<bytes;out.close();
    Check(static_cast<bool>(out),"write allocator qualification fixture");
  };
  const auto valid=second_line+"\n"+first_line+"\n";
  api::ClearMgaEventSequenceRangeCacheForTesting();
  Check(api::NextRowEventSequence(f.context)==7,"cold recovery uses maximum not last deferred range");
  write(valid+first_line+"\n");
  Check(api::NextRowEventSequence(f.context)==7,"exact retry record is redundant evidence");
  write(second_line+"\n");
  Check(api::NextRowEventSequence(f.context)==7,"reserved gaps do not lower recovered authority");
  write(valid);
  const auto cold=api::ReserveMgaRowEventSequenceRangeForTesting(f.context,1);
  Check(cold.ok && cold.first==7 && cold.next==8 && !cold.bootstrapped_from_store,"cold allocator reuses recovered high water");
  std::vector<std::string> tail_lines;
  const auto tail=api::ReserveEventSequenceRange(f.context,"row_versions",stream,2,[]{return 1;},&tail_lines);
  api::AbandonDeferredEventSequenceReservation(tail);
  tail_lines.clear();
  const auto retry=api::ReserveEventSequenceRange(f.context,"row_versions",stream,2,[]{return 1;},&tail_lines);
  Check(tail.ok && retry.ok && tail.first==8 && retry.first==tail.first,"unpublished effect-free tail may be abandoned");
  Check(api::AppendDeferredEventSequenceAllocatorLines(f.context,&tail_lines,nullptr),"publish only replacement after effect-free abandonment");
  api::ClearMgaEventSequenceRangeCacheForTesting();
  Check(api::NextRowEventSequence(f.context)==10,"replacement history remains valid after cache eviction");
  write(valid);
  const auto delta_path=f.context.database_path+".sb.mga_secondary_index_delta_ledger";
  const auto delta=api::ReserveEventSequenceRange(f.context,"secondary_index_delta_ledger",delta_path,4,[]{return 1;});
  Check(delta.ok && delta.first==1,"production secondary-index delta domain shares allocator journal");
  api::ClearMgaEventSequenceRangeCacheForTesting();
  Check(api::NextRowEventSequence(f.context)==7 && api::NextMetadataEventSequence(f.context)==1 &&
        api::NextIndexEventSequence(f.context)==1,"unrelated delta reservations preserve all observed domains");
  const auto delta_reopen=api::ReserveEventSequenceRange(f.context,"secondary_index_delta_ledger",delta_path,1,[]{return 1;});
  Check(delta_reopen.ok && delta_reopen.first==5,"delta domain recovers its own retained reservations");
  Check(api::ReserveMgaRowEventSequenceRangeForTesting(f.context,1).ok,"populate row reservation cache in mixed journal");
  write(valid);
  Check(!api::CreateMgaSavepointMarker(f.context,"cutoff").error,"create with valid retained range and warm cache");
  const auto marker_size=std::filesystem::file_size(f.Path());
  const auto record=[&](std::string a,std::string count,std::string next,std::string kind="row_versions",std::string path="") {
    return "SBMGAEVSEQ1\tRANGE\t"+kind+"\t"+api::EncodeCrudText(path.empty()?stream:path)+
        "\t"+a+"\t"+count+"\t"+next+"\tbootstrap_store_scan\t1\n";
  };
  const std::vector<std::string> malformed={"torn", valid.substr(0,valid.size()-1), valid+"\n",
      record("1junk","2","3"),record("-1","2","1"),record("1","0","1"),
      record("1","2","4"),record("18446744073709551615","1","0"),
      record("1","2","3junk"),record("1","2","3","unknown"),
      record("1","2","3","row_versions",stream+"foreign"),valid+record("2","2","4"),
      valid+record("1","3","4"),valid+"SBMGAEVSEQ1\tRANGE\n"};
  for (const auto& bytes : malformed) {
    write(bytes);
    Check(api::NextRowEventSequence(f.context)==0,"malformed allocator beats warm cache");
    for (auto action : {api::CreateMgaSavepointMarker,api::ReleaseMgaSavepointMarker,api::RollbackToMgaSavepointMarker}) {
      const auto result=action(f.context,"cutoff");
      Check(result.error && result.detail.find("event_sequence_authority_invalid")!=std::string::npos,
            "marker refuses malformed reservation journal");
      Check(std::filesystem::file_size(f.Path())==marker_size,"bad allocator appends no marker");
    }
    Check(std::filesystem::file_size(allocator)==bytes.size(),"observation does not repair or append allocator bytes");
  }
  api::ClearMgaEventSequenceRangeCacheForTesting();
  const auto refused=api::ReserveMgaRowEventSequenceRangeForTesting(f.context,1);
  Check(!refused.ok && refused.diagnostic.detail.find("event_sequence_authority_invalid")!=std::string::npos,
        "cold admission refuses malformed durable reservations");
  std::filesystem::remove(allocator);std::filesystem::create_directory(allocator);
  Check(api::NextRowEventSequence(f.context)==0,"nonregular allocator is not absence");
  std::filesystem::remove(allocator);write(valid);
  Check(api::NextRowEventSequence(f.context)==7,"restored valid journal recovers without poisoned observation cache");
  api::ClearMgaEventSequenceRangeCacheForTesting();
}
}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture("mga_savepoint_marker_gate");
  for (const auto test : {CurrentOwnerAdmission, Stack, MarkerObservation, Ranges, Coordinator, BinaryCodec, WireUuidCodec, BinaryReplay, Corruption, MetadataSequenceAuthority, AllocatorSequenceAuthority}) {
    try { test(); }
    catch (const std::exception& error) {
      Check(false, std::string("unexpected exception: ") + error.what());
    }
  }
  std::cout << "mga_savepoint_marker checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
