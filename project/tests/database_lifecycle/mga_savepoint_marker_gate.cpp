// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_savepoint_store.hpp"
#include "transaction/savepoint_api.hpp"
#include "sblr_savepoint_coordinator.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "uuid.hpp"
#include "sblr_savepoint_runtime.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
  int failures_at_start = failures;
  std::filesystem::path root;
  api::EngineRequestContext context;
  Fixture() {
    root = std::filesystem::temp_directory_path() /
        ("sb_savepoint_marker_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    context.database_path = (root / "markers.sbdb").string();
    context.local_transaction_id = 17;
  }
  ~Fixture() {
    if (failures > failures_at_start || std::uncaught_exceptions() != 0) {
      std::cerr << "preserved_marker_artifacts=" << root << '\n';
      return;
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path Path() const {
    return context.database_path + ".sb.mga_savepoints";
  }
  void Write(const std::string& bytes) {
    std::ofstream out(Path(), std::ios::binary | std::ios::trunc);
    out << bytes;
  }
};

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
  auto other = f.context;
  other.local_transaction_id = 18;
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
  const auto key = api::MgaSavepointUuidKey("019f2100-0000-7000-8000-00000000de01");
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
  c.database_uuid.canonical = "12340000-0000-7000-8000-000000000001";
  c.transaction_uuid.canonical = "12340000-0000-7000-8000-000000000002";
  c.statement_uuid.canonical = "12340000-0000-7000-8000-000000000003";
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  c.trace_tags = {"private_savepoint_coordination"};
  const std::string hash = "sha256:" + std::string(64, 'a');
  const auto activate = [&](std::uint64_t occurrence) {
    const auto reserved = api::ReserveSblrSavepoint(c, c.statement_uuid.canonical,
                                                   hash, occurrence, hash);
    Check(reserved.ok, "coordinator reserve");
    const auto& s = reserved.snapshot;
    return api::ActivateSblrSavepoint(c, c.statement_uuid.canonical,
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
  Check(bytes.find(c.transaction_uuid.canonical) == std::string::npos &&
        bytes.find(target.savepoint_uuid) == std::string::npos &&
        bytes.find(c.statement_uuid.canonical) == std::string::npos,
        "coordinator disk records contain no textual system UUIDs");
  const auto uuid = scratchbird::core::uuid::ParseUuid(c.transaction_uuid.canonical);
  Check(uuid.ok() && bytes.substr(48, 16) == std::string(
      reinterpret_cast<const char*>(uuid.value.bytes.data()), 16),
      "coordinator transaction UUID occupies exactly binary(16)");
}

void BinaryCodec() {
  api::MgaSavepointMarkerRecord record;
  record.kind = 1;
  record.transaction = 17;
  record.uuid_identity = true;
  record.identity = "12340000-0000-7000-8000-000000000009";
  const auto bytes = api::EncodeMgaSavepointMarker(record);
  const auto uuid = scratchbird::core::uuid::ParseUuid(record.identity);
  Check(bytes.size() == 124 && api::MgaSavepointMarkerFrameSize(bytes) == 124,
        "native binary marker has fixed frame size");
  Check(bytes.find(record.identity) == std::string::npos, "marker has no textual UUID copy");
  Check(uuid.ok() && bytes.substr(76, 16) == std::string(
      reinterpret_cast<const char*>(uuid.value.bytes.data()), 16),
      "marker UUID occupies exactly binary(16)");
  api::MgaSavepointMarkerRecord decoded;
  Check(api::DecodeMgaSavepointMarker(bytes, &decoded) && decoded.uuid_identity &&
        decoded.identity == record.identity && decoded.transaction == 17,
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
}  // namespace

int main() {
  for (const auto test : {Stack, MarkerObservation, Ranges, Coordinator, BinaryCodec, WireUuidCodec, Corruption}) {
    try { test(); }
    catch (const std::exception& error) {
      Check(false, std::string("unexpected exception: ") + error.what());
    }
  }
  std::cout << "mga_savepoint_marker checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
