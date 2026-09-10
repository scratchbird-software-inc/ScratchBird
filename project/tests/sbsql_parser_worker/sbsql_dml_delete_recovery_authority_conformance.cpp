// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_recovery_authority.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "mga_relation_store/mga_delete_durable_store.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "physical_mga_cow_store.hpp"
#include "uuid.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#if !defined(_WIN32)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(SB_DELETE_TEST_WRAP_IO)
#include "../common/single_tu_allocation_fault.hpp"
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
namespace {
enum class IoFault { none, allocation_after_release, release_sync, publication_rename, publication_directory_sync, rewind_sync };
IoFault io_fault = IoFault::none;
struct stat marker_stat{}, directory_stat{};
bool marker_synced = false, release_synced = false, io_fault_hit = false;
unsigned marker_sync_calls = 0;
bool SameFile(const struct stat& lhs, const struct stat& rhs) {
  return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino;
}
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  struct stat file{};
  const bool inspected = io_fault != IoFault::none && ::fstat(fd, &file) == 0;
  if (inspected && SameFile(file, marker_stat)) ++marker_sync_calls;
  if (inspected && SameFile(file, marker_stat) && io_fault == IoFault::rewind_sync && marker_sync_calls == 2) {
    io_fault_hit = true; io_fault = IoFault::none; errno = EIO; return -1;
  }
  if (inspected && SameFile(file, marker_stat) && io_fault == IoFault::release_sync) {
    io_fault_hit = true; io_fault = IoFault::none; errno = EIO; return -1;
  }
  if (inspected && release_synced && io_fault == IoFault::publication_directory_sync) {
    io_fault_hit = true; io_fault = IoFault::none; errno = EIO; return -1;
  }
  const auto result = __real_fsync(fd);
  if (inspected && result == 0) {
    if (SameFile(file, marker_stat)) marker_synced = true;
    if (marker_synced && SameFile(file, directory_stat)) {
      release_synced = true;
      if (io_fault == IoFault::allocation_after_release) {
        io_fault_hit = true; io_fault = IoFault::none;
        allocations_before_failure = 0;
      }
    }
  }
  return result;
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
  if (io_fault == IoFault::publication_rename && release_synced && std::strstr(from, ".publication")) {
    io_fault_hit = true; io_fault = IoFault::none; errno = EIO; return -1;
  }
  return __real_rename(from, to);
}
#endif

namespace api = scratchbird::engine::internal_api;
namespace w = scratchbird::wire;
namespace db = scratchbird::storage::database;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using Kind = scratchbird::core::platform::UuidKind;
namespace {
unsigned checks = 0;
void Require(bool ok, const char* message) {
  ++checks; if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
std::string NewUuid(Kind kind) {
  static std::uint64_t time = 1788210000100;
  const auto id = uuid::GenerateDurableEngineIdentityV7(kind, time++);
  Require(id.ok(), "UUID generation"); return uuid::UuidToString(id.value.value);
}
w::TypedUpdateUuid Binary(const std::string& text) {
  const auto id = uuid::ParseUuid(text);
  Require(id.ok(), "UUID parse");
  w::TypedUpdateUuid out{};
  std::copy(id.value.bytes.begin(), id.value.bytes.end(), out.begin()); return out;
}
struct Fixture {
  std::filesystem::path root;
  api::EngineRequestContext context;
  Fixture() {
    root = std::filesystem::temp_directory_path() /
        ("sb_delete_recovery_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(root), "fixture directory");
    db::DatabaseCreateConfig config;
    config.path = (root / "recovery.sbdb").string();
    const auto database = uuid::GenerateDurableEngineIdentityV7(Kind::database, 1788210000000);
    const auto filespace = uuid::GenerateDurableEngineIdentityV7(Kind::filespace, 1788210000001);
    Require(database.ok() && filespace.ok(), "database UUIDs");
    config.database_uuid = database.value; config.filespace_uuid = filespace.value;
    config.page_size = 16384; config.creation_unix_epoch_millis = 1788210000002;
    config.allow_minimal_resource_bootstrap = true; config.require_resource_seed_pack = false;
    Require(db::CreateDatabaseFile(config).ok(), "database creation");
    Require(db::OpenDatabaseFile({config.path, false, false, false}).ok(), "database open");
    Require(db::MarkDatabaseCleanShutdown(config.path).ok(), "database clean marker");
    const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(config.path);
    Require(inventory.ok(), "inventory load");
    const auto transaction = uuid::GenerateDurableEngineIdentityV7(Kind::transaction, 1788210000003);
    Require(transaction.ok(), "transaction UUID");
    const auto begin = mga::BeginLocalTransaction(inventory.inventory, transaction.value, 1788210000004);
    Require(begin.ok() && db::PersistLocalTransactionInventoryToDatabase(config.path, begin.inventory).ok(), "active inventory");
    context.database_path = config.path;
    context.database_uuid.canonical = uuid::UuidToString(database.value.value);
    context.transaction_uuid.canonical = uuid::UuidToString(transaction.value.value);
    context.local_transaction_id = begin.entry.identity.local_id.value;
    context.database_page_size_bytes = 16384;
    context.statement_receipt_uuid.canonical = NewUuid(Kind::object);
    context.statement_snapshot_uuid.canonical = NewUuid(Kind::object);
    context.statement_metadata_snapshot_uuid.canonical = NewUuid(Kind::object);
    context.catalog_generation_id = context.datatype_registry_generation = 1;
  }
  ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
  void Finalize(db::PhysicalMgaCowFinalizeDecision decision) {
    db::PhysicalMgaCowFinalizeRequest request;
    request.database_path = context.database_path;
    request.local_transaction_id = mga::MakeLocalTransactionId(context.local_transaction_id);
    request.decision = decision; request.final_unix_epoch_millis = 1788210002000;
    Require(db::FinalizePhysicalMgaCowTransaction(request).ok(), "inventory finalization");
  }
};
w::TypedDeleteDescriptorCarrier Descriptor(const api::EngineRequestContext& c) {
  w::TypedDeleteDescriptorCarrier v;
  v.descriptor_uuid = Binary(NewUuid(Kind::object));
  v.authenticated_statement_receipt_uuid = Binary(NewUuid(Kind::object));
  v.operation_uuid = Binary(NewUuid(Kind::object));
  v.owning_transaction_uuid = Binary(NewUuid(Kind::object));
  v.statement_snapshot_uuid = Binary(NewUuid(Kind::object));
  v.catalog_snapshot_uuid = Binary(NewUuid(Kind::object));
  v.security_context_uuid = Binary(NewUuid(Kind::object));
  v.security_snapshot_uuid = Binary(NewUuid(Kind::object));
  v.target_relation_uuid = Binary(NewUuid(Kind::object));
  v.target_relation_occurrence_uuid = Binary(NewUuid(Kind::object));
  v.predicate_expression_uuid = Binary(NewUuid(Kind::object));
  v.row_policy_set_uuid = Binary(NewUuid(Kind::object));
  v.constraint_set_uuid = Binary(NewUuid(Kind::object));
  v.trigger_set_uuid = Binary(NewUuid(Kind::object));
  v.deterministic_target_order_uuid = Binary(NewUuid(Kind::object));
  v.resource_budget_uuid = Binary(NewUuid(Kind::object));
  v.recovery_token_uuid = Binary(NewUuid(Kind::object));
  v.builtin_operator_snapshot_uuid = Binary(NewUuid(Kind::object));
  v.descriptor_generation = 1;
  v.structural_occurrence_id = 1;
  v.operation_generation = 1;
  v.owning_local_transaction_id = 1;
  v.catalog_generation = 1;
  v.datatype_registry_generation = 1;
  v.security_generation = 1;
  v.target_relation_generation = 1;
  v.target_relation_occurrence_generation = 1;
  v.predicate_expression_generation = 1;
  v.predicate_root_node_id = 1;
  v.row_policy_set_generation = 1;
  v.constraint_set_generation = 1;
  v.trigger_set_generation = 1;
  v.deterministic_target_order_generation = 1;
  v.resource_budget_generation = 1;
  v.recovery_generation = 1;
  v.executor_availability_generation = 1;
  v.builtin_operator_registry_generation = 1;
  v.predicate_vector_sha256.fill(1);
  v.row_policy_set_sha256.fill(2);
  v.ordered_constraint_set_sha256.fill(3);
  v.ordered_trigger_set_sha256.fill(4);
  v.owning_transaction_uuid = Binary(c.transaction_uuid.canonical);
  v.owning_local_transaction_id = c.local_transaction_id;
  v.authenticated_statement_receipt_uuid = Binary(c.statement_receipt_uuid.canonical);
  v.statement_snapshot_uuid = Binary(c.statement_snapshot_uuid.canonical);
  v.catalog_snapshot_uuid = Binary(c.statement_metadata_snapshot_uuid.canonical);
  v.predicate_node_count = 1; v.builtin_operator_snapshot_uuid = w::kTypedUpdateOperatorSnapshotUuid;
  return v;
}
struct Operation {
  Fixture& f;
  std::vector<std::vector<std::uint8_t>> chain;
  w::TypedDeleteJournalRecord head;
  std::string marker_uuid, marker_key;
  Operation(Fixture& fixture) : f(fixture) {
    head.descriptor = Descriptor(f.context); head.journal_sequence = 1;
    head.database_uuid = Binary(f.context.database_uuid.canonical);
    head.authenticated_statement_receipt_uuid = head.descriptor.authenticated_statement_receipt_uuid;
    head.owning_transaction_uuid = head.descriptor.owning_transaction_uuid;
    head.owning_local_transaction_id = head.descriptor.owning_local_transaction_id;
    head.operation_uuid = head.descriptor.operation_uuid;
    head.recovery_token_uuid = head.descriptor.recovery_token_uuid;
    head.recovery_generation = head.descriptor.recovery_generation;
    Append(head);
  }
  void Append(w::TypedDeleteJournalRecord next) {
    std::vector<std::uint8_t> bytes; w::TypedDeleteCarrierError error;
    Require(w::EncodeTypedDeleteJournal(next, chain.empty() ? nullptr : &head, &bytes, &error), error.field.c_str());
    w::TypedDeleteJournalRecord decoded;
    Require(w::DecodeAndValidateTypedDeleteJournal(bytes, chain.empty() ? nullptr : &head, &decoded, &error), error.field.c_str());
    head = std::move(decoded); chain.push_back(std::move(bytes));
  }
  void Next(w::TypedUpdateJournalState state) {
    auto next = head; ++next.journal_sequence; next.prior_record_sha256 = head.record_evidence_sha256;
    next.lifecycle_state = state;
    if (state == w::TypedUpdateJournalState::aborted) next.prior_result.reset();
    Append(std::move(next));
  }
  void Intent() {
    marker_uuid = NewUuid(Kind::object); marker_key = api::MgaSavepointUuidKey(marker_uuid);
    Require(!api::CreateMgaSavepointMarker(f.context, marker_key).error, "native marker create");
    auto next = head; ++next.journal_sequence; next.prior_record_sha256 = head.record_evidence_sha256;
    next.lifecycle_state = w::TypedUpdateJournalState::intent;
    next.statement_savepoint_uuid = Binary(marker_uuid);
    next.statement_savepoint_generation = api::ParseSavepoints(f.context).active_savepoints.at(f.context.local_transaction_id).at(marker_key).creation_ordinal;
    Append(std::move(next));
  }
  void Prepared() {
    auto next = head; ++next.journal_sequence; next.prior_record_sha256 = head.record_evidence_sha256;
    next.lifecycle_state = w::TypedUpdateJournalState::prepared;
    auto& r = next.prior_result.emplace(); const auto& d = head.descriptor;
    r.delete_descriptor_uuid = d.descriptor_uuid; r.delete_descriptor_generation = d.descriptor_generation;
    r.operation_uuid = d.operation_uuid; r.owning_transaction_uuid = d.owning_transaction_uuid;
    r.owning_local_transaction_id = d.owning_local_transaction_id; r.relation_uuid = d.target_relation_uuid;
    r.relation_generation = d.target_relation_generation; r.effect_set_sha256.fill(1); r.executor_evidence_sha256.fill(2);
    r.publication_barrier_uuid = Binary(marker_uuid); r.publication_barrier_generation = head.statement_savepoint_generation;
    Append(std::move(next));
  }
  api::EngineDmlDeleteRecoveryObservationV1 Observe() { return api::ObserveDmlDeleteRecoveryAuthorityV1(f.context, chain); }
  void Expect(api::EngineDmlDeleteRecoveryDispositionV1 disposition) {
    const auto observed = Observe();
    if (!observed.ok) std::cerr << observed.diagnostic.detail << '\n';
    Require(observed.ok && observed.disposition == disposition, "MGA recovery disposition");
  }
};

std::unique_ptr<api::MgaDmlDeleteDurableStoreV1> OpenStore(Operation& operation) {
  api::EngineApiDiagnostic diagnostic;
  auto store = api::MgaDmlDeleteDurableStoreV1::Open(operation.f.context,
      operation.head.descriptor.descriptor_uuid,
      operation.head.descriptor.descriptor_generation, &diagnostic);
  if (!store) std::cerr << diagnostic.detail << '\n';
  Require(static_cast<bool>(store), "durable store open"); return store;
}
void Persist(api::MgaDmlDeleteDurableStoreV1& store, const Operation& operation) {
  api::EngineApiDiagnostic diagnostic;
  const bool ok = store.Append(operation.head, &diagnostic);
  if (!ok) std::cerr << diagnostic.detail << '\n';
  Require(ok, "durable successor append");
}
void Stage(api::MgaDmlDeleteDurableStoreV1& store) {
  api::EngineApiDiagnostic diagnostic;
  const bool ok = store.StagePublication(&diagnostic);
  if (!ok) std::cerr << diagnostic.detail << '\n';
  Require(ok, "exact publication stage");
}
void DurableStoreCases() {
  using P = api::MgaDmlDeletePublicationStatusV1;
  using A = api::MgaDmlDeleteAbortStatusV1;
  using S = w::TypedUpdateJournalState;
  {
    Fixture f; Operation operation(f); auto store = OpenStore(operation);
    Require(store->chain().empty(), "new store empty");
    Persist(*store, operation);
    api::EngineApiDiagnostic diagnostic;
    Require(!store->Append(operation.head, &diagnostic), "no duplicate bound append");
    Require(!store->StagePublication(&diagnostic), "no stage without prepared result");
    operation.Intent(); Persist(*store, operation);
    operation.Prepared(); Persist(*store, operation);
    Stage(*store); Stage(*store);
    Require(store->CompleteReleasedPublication() == P::refused, "pending bytes are not a release");
    Require(store->ReleaseAndPublish() == P::published, "real native release then exact publication");
    const auto exact = std::vector<std::vector<std::uint8_t>>(store->chain().begin(), store->chain().end());
    Require(store->ReleaseAndPublish() == P::refused, "live publication owner consumed");
    store.reset(); store = OpenStore(operation);
    Require(std::equal(exact.begin(), exact.end(), store->chain().begin(), store->chain().end()), "reopen byte identity");
    Require(store->CompleteReleasedPublication() == P::published, "read-only terminal replay");
    Require(store->AbortBeforePublication() == A::refused, "published statement never aborts");
    const auto marker_bytes = api::CurrentMgaSavepointAuthorityGeneration(f.context);
    Require(store->CompleteReleasedPublication() == P::published &&
        api::CurrentMgaSavepointAuthorityGeneration(f.context) == marker_bytes,
        "repeated replay does not append marker");
    f.Finalize(db::PhysicalMgaCowFinalizeDecision::commit);
    Require(store->CompleteReleasedPublication() == P::published, "inventory commit retained");
  }
  for (unsigned cut = 0; cut != 4; ++cut) {
    Fixture f;
    Require(!api::CreateMgaSavepointMarker(f.context, "user_outer").error, "user boundary before DELETE");
    Operation operation(f); auto store = OpenStore(operation); Persist(*store, operation);
    if (cut) { operation.Intent(); Persist(*store, operation); }
    if (cut >= 2) { operation.Prepared(); Persist(*store, operation); }
    if (cut == 3) Stage(*store);
    Require(store->AbortBeforePublication() == A::aborted, "engine abort coordinator owns rewind");
    Require(!api::ValidateMgaSavepointExists(f.context, "user_outer", "test").error,
        "abort preserves outer user boundary");
    const auto parsed = api::ParseSavepoints(f.context);
    Require(parsed.active_savepoints.at(f.context.local_transaction_id).size() == 1,
        "aborted internal boundary retired");
    const auto marker_bytes = api::CurrentMgaSavepointAuthorityGeneration(f.context);
    Require(store->AbortBeforePublication() == A::aborted &&
        api::CurrentMgaSavepointAuthorityGeneration(f.context) == marker_bytes,
        "same-owner abort retry is mutation-free");
    store.reset(); store = OpenStore(operation);
    Require(store->AbortBeforePublication() == A::aborted &&
        api::CurrentMgaSavepointAuthorityGeneration(f.context) == marker_bytes,
        "reopened abort retry is mutation-free");
  }
  {
    Fixture f; Operation operation(f); auto store = OpenStore(operation);
    Persist(*store, operation); operation.Intent(); Persist(*store, operation);
    operation.Prepared(); Persist(*store, operation); Stage(*store);
    Require(!api::RollbackToMgaSavepointMarker(f.context, operation.marker_key).error, "staged operation rollback");
    Require(store->ReleaseAndPublish() == P::refused, "cannot publish rolled-back stage");
    operation.Next(S::aborted); Persist(*store, operation);
    store.reset(); store = OpenStore(operation);
    const auto observation = api::ObserveDmlDeleteRecoveryAuthorityV1(f.context, store->chain());
    Require(observation.ok && observation.disposition == api::EngineDmlDeleteRecoveryDispositionV1::aborted,
        "staged successor invalidated before durable abort");
    Require(store->CompleteReleasedPublication() == P::refused, "no abandoned stage resurrection");
  }
  {
    Fixture f; Operation operation(f); auto store = OpenStore(operation);
    Persist(*store, operation); operation.Intent(); Persist(*store, operation);
    operation.Prepared(); Persist(*store, operation); Stage(*store); store.reset();
    auto wrong = f.context; wrong.statement_receipt_uuid.canonical = NewUuid(Kind::object);
    api::EngineApiDiagnostic diagnostic;
    Require(!api::MgaDmlDeleteDurableStoreV1::Open(wrong, operation.head.descriptor.descriptor_uuid,
        operation.head.descriptor.descriptor_generation, &diagnostic), "durable cross-receipt refusal");
    store = OpenStore(operation);
    Require(!api::ReleaseMgaSavepointMarker(f.context, operation.marker_key).error, "release before recovery reopen");
    store.reset(); store = OpenStore(operation);
    Require(store->CompleteReleasedPublication() == P::published, "exact staged successor recovered after release");
  }
  {
    Fixture f; Operation operation(f); auto store = OpenStore(operation);
    Persist(*store, operation); operation.Intent(); Persist(*store, operation);
    operation.Prepared(); Persist(*store, operation);
    Require(!api::ReleaseMgaSavepointMarker(f.context, operation.marker_key).error, "release without durable staged bytes");
    Require(store->CompleteReleasedPublication() == P::refused, "never reconstruct unstaged published bytes");
  }
  {
    Fixture f; Operation operation(f); auto store = OpenStore(operation);
    Persist(*store, operation); store.reset();
    const auto directory = f.context.database_path + ".sb.mga_delete_operations.v1";
    std::filesystem::path chain_path;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
      if (entry.path().extension() == ".ddjr") chain_path = entry.path();
    Require(!chain_path.empty(), "durable binary chain path");
    std::ofstream corrupt(chain_path, std::ios::binary | std::ios::app); corrupt << 'x'; corrupt.close();
    api::EngineApiDiagnostic diagnostic;
    Require(!api::MgaDmlDeleteDurableStoreV1::Open(f.context, operation.head.descriptor.descriptor_uuid,
        operation.head.descriptor.descriptor_generation, &diagnostic), "torn head quarantined");
  }
}

void ProcessKillDurableStoreCases() {
#if !defined(_WIN32)
  // These are real process-death tests of DDJR + native MGA marker persistence,
  // not a substitute for the server DELETE row/index/trigger route matrix.
  for (unsigned cut = 0; cut != 9; ++cut) {
    Fixture f; Operation operation(f);
    int checkpoint[2]; Require(::pipe(checkpoint) == 0, "checkpoint pipe");
    const pid_t child = ::fork(); Require(child >= 0, "recovery fork");
    if (child == 0) {
      ::close(checkpoint[0]);
      const auto stop = [&] {
        if (::write(checkpoint[1], "1", 1) != 1) ::_exit(2);
        for (;;) ::pause();
      };
      auto store = OpenStore(operation); Persist(*store, operation);
      if (cut == 0) stop();
      operation.Intent(); Persist(*store, operation);
      if (cut == 1) stop();
      operation.Prepared(); Persist(*store, operation);
      if (cut == 2) stop();
      Stage(*store);
      if (cut == 3) stop();
      if (cut >= 6) {
        Require(!api::RollbackToMgaSavepointMarker(f.context, operation.marker_key).error, "child rewind");
        if (cut == 6) stop();
        Require(!api::ReleaseMgaSavepointMarker(f.context, operation.marker_key).error, "child retire rewound boundary");
        if (cut == 7) stop();
        Require(store->AbortBeforePublication() == api::MgaDmlDeleteAbortStatusV1::aborted, "child durable abort");
        stop();
      }
      Require(!api::ReleaseMgaSavepointMarker(f.context, operation.marker_key).error, "child release");
      if (cut == 4) stop();
      Require(store->CompleteReleasedPublication() == api::MgaDmlDeletePublicationStatusV1::published,
          "child terminal publication");
      stop();
    }
    ::close(checkpoint[1]);
    pollfd ready{checkpoint[0], POLLIN, 0};
    const int polled = ::poll(&ready, 1, 20000);
    char byte = 0;
    const bool reached = polled == 1 && (ready.revents & POLLIN) && ::read(checkpoint[0], &byte, 1) == 1 && byte == '1';
    ::close(checkpoint[0]);
    const bool killed = ::kill(child, SIGKILL) == 0;
    int status = 0; const auto waited = ::waitpid(child, &status, 0);
    Require(reached && killed && waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
        "child killed at exact durable checkpoint");
    auto store = OpenStore(operation);
    const auto observed = api::ObserveDmlDeleteRecoveryAuthorityV1(f.context, store->chain());
    Require(observed.ok, "post-kill actual MGA observation");
    if (cut == 4 || cut == 5) {
      Require(store->CompleteReleasedPublication() == api::MgaDmlDeletePublicationStatusV1::published,
          "post-barrier process kill completes exact result");
      store.reset(); store = OpenStore(operation);
      const auto marker_bytes = api::CurrentMgaSavepointAuthorityGeneration(f.context);
      Require(store->CompleteReleasedPublication() == api::MgaDmlDeletePublicationStatusV1::published &&
          api::CurrentMgaSavepointAuthorityGeneration(f.context) == marker_bytes,
          "second recovery does not repeat release or mutation");
    } else {
      const auto before = api::ParseSavepoints(f.context);
      const auto ranges = before.rollback_ranges.find(f.context.local_transaction_id);
      const auto rewind_count = ranges == before.rollback_ranges.end() ? 0 : ranges->second.size();
      Require(store->CompleteReleasedPublication() == api::MgaDmlDeletePublicationStatusV1::refused,
          "pre-barrier process kill cannot publish");
      Require(store->AbortBeforePublication() == api::MgaDmlDeleteAbortStatusV1::aborted,
          "durable coordinator owns post-kill rewind and abort");
      if (cut >= 6) {
        const auto after = api::ParseSavepoints(f.context);
        Require(rewind_count == 1 && after.rollback_ranges.at(f.context.local_transaction_id).size() == rewind_count,
            "kill after rewind cannot append a second compensation range");
      }
      const auto marker_bytes = api::CurrentMgaSavepointAuthorityGeneration(f.context);
      store.reset(); store = OpenStore(operation);
      const auto retry = api::ObserveDmlDeleteRecoveryAuthorityV1(f.context, store->chain());
      Require(retry.ok && retry.disposition == api::EngineDmlDeleteRecoveryDispositionV1::aborted &&
          api::CurrentMgaSavepointAuthorityGeneration(f.context) == marker_bytes,
          "second recovery observes abort without double compensation");
    }
  }
#else
  std::cout << "POSIX SIGKILL cutpoints not executed on Windows\n";
#endif
}

void PublicationFaultCases() {
#if defined(SB_DELETE_TEST_WRAP_IO)
  using P = api::MgaDmlDeletePublicationStatusV1;
  for (const auto fault : {IoFault::allocation_after_release, IoFault::release_sync,
       IoFault::publication_rename, IoFault::publication_directory_sync}) {
    Fixture f; Operation operation(f); auto store = OpenStore(operation);
    Persist(*store, operation); operation.Intent(); Persist(*store, operation);
    operation.Prepared(); Persist(*store, operation); Stage(*store);
    Require(::stat((f.context.database_path + ".sb.mga_savepoints").c_str(), &marker_stat) == 0 &&
        ::stat(f.root.c_str(), &directory_stat) == 0, "fault boundary file identities");
    marker_synced = release_synced = io_fault_hit = false;
    marker_sync_calls = 0;
    allocation_attempts = 0; io_fault = fault;
    P outcome = P::refused; bool threw = false;
    try { outcome = store->ReleaseAndPublish(); }
    catch (const std::bad_alloc&) { threw = true; }
    allocations_before_failure = -1; io_fault = IoFault::none;
    Require(io_fault_hit && !threw, "exact durable fault triggered without escaping exception");
    if (fault == IoFault::allocation_after_release) {
      Require(outcome == P::published && allocation_attempts == 0,
          "zero C++ allocations from native release fence through publication return");
    } else {
      Require(outcome == (fault == IoFault::release_sync ? P::release_uncertain : P::publication_uncertain),
          "I/O ambiguity is not rollback or ordinary failure");
    }
    store.reset(); store = OpenStore(operation);
    Require(store->CompleteReleasedPublication() == P::published, "uncertain outcome fenced and completed on reopen");
    store.reset(); store = OpenStore(operation);
    Require(store->CompleteReleasedPublication() == P::published, "exact repeat recovery after I/O ambiguity");
  }
  {
    Fixture f; Operation operation(f); auto store = OpenStore(operation);
    Persist(*store, operation); operation.Intent(); Persist(*store, operation);
    operation.Prepared(); Persist(*store, operation); Stage(*store);
    Require(::stat((f.context.database_path + ".sb.mga_savepoints").c_str(), &marker_stat) == 0 &&
        ::stat(f.root.c_str(), &directory_stat) == 0, "rewind fault file identities");
    marker_synced = release_synced = io_fault_hit = false; marker_sync_calls = 0;
    io_fault = IoFault::rewind_sync;
    const auto outcome = store->AbortBeforePublication(); io_fault = IoFault::none;
    Require(io_fault_hit && outcome == api::MgaDmlDeleteAbortStatusV1::rewind_uncertain,
        "lost rewind sync acknowledgement retains uncertainty");
    const auto before = api::ParseSavepoints(f.context);
    Require(before.rollback_ranges.at(f.context.local_transaction_id).size() == 1, "written rewind is present");
    store.reset(); store = OpenStore(operation);
    Require(store->AbortBeforePublication() == api::MgaDmlDeleteAbortStatusV1::aborted,
        "uncertain rewind fenced and completed on reopen");
    const auto after = api::ParseSavepoints(f.context);
    Require(after.rollback_ranges.at(f.context.local_transaction_id).size() == 1,
        "uncertain rewind recovery does not repeat compensation");
  }
#else
  std::cout << "Linux linker-wrapped publication I/O/allocation cases not executed on this platform\n";
#endif
}
}
int main() {
  using D = api::EngineDmlDeleteRecoveryDispositionV1;
  using S = w::TypedUpdateJournalState;
  {
    Fixture f; Operation operation(f);
    operation.Expect(D::abandon_unexecuted);
    auto wrong = f.context; wrong.statement_receipt_uuid.canonical = NewUuid(Kind::object);
    Require(!api::ObserveDmlDeleteRecoveryAuthorityV1(wrong, operation.chain).ok, "cross receipt refusal");
    operation.Intent(); operation.Expect(D::rollback_statement);
    operation.Prepared(); operation.Expect(D::rollback_statement);
    Require(!api::ReleaseMgaSavepointMarker(f.context, operation.marker_key).error, "publish marker");
    operation.Expect(D::publish_prepared_result);
    operation.Next(S::published); operation.Expect(D::published);
    f.Finalize(db::PhysicalMgaCowFinalizeDecision::commit);
    operation.Expect(D::published);
    auto corrupt = operation.chain; corrupt.back()[300] ^= 1;
    Require(!api::ObserveDmlDeleteRecoveryAuthorityV1(f.context, corrupt).ok, "committed inventory cannot waive corrupt chain");
  }
  {
    Fixture f; Operation operation(f); operation.Intent(); operation.Prepared();
    Require(!api::RollbackToMgaSavepointMarker(f.context, operation.marker_key).error, "rollback marker");
    operation.Expect(D::statement_already_rewound);
    Require(!api::ReleaseMgaSavepointMarker(f.context, operation.marker_key).error, "release after rollback");
    operation.Expect(D::statement_already_rewound);
    operation.Next(S::aborted); operation.Expect(D::aborted);
    f.Finalize(db::PhysicalMgaCowFinalizeDecision::rollback);
    operation.Expect(D::transaction_rolled_back);
  }
  {
    Fixture f;
    Require(!api::CreateMgaSavepointMarker(f.context, "outer").error, "outer create");
    Operation operation(f); operation.Intent();
    Require(!api::RollbackToMgaSavepointMarker(f.context, "outer").error, "ancestor rollback");
    operation.Expect(D::statement_already_rewound);
    operation.Next(S::aborted); operation.Expect(D::aborted);
  }
  {
    Fixture f; Operation operation(f); operation.Intent();
    Require(!api::ReleaseMgaSavepointMarker(f.context, operation.marker_key).error, "release without prepared outcome");
    Require(!operation.Observe().ok, "unprepared release must quarantine");
  }
  {
    Fixture f; Operation operation(f); operation.Intent(); operation.Next(S::aborted);
    Require(!operation.Observe().ok, "journal abort cannot assert MGA rewind");
  }
  {
    Fixture f; Operation operation(f); operation.Intent(); operation.Prepared(); operation.Next(S::published);
    Require(!operation.Observe().ok, "journal publication cannot assert MGA release");
  }
  {
    Fixture f; Operation operation(f); operation.Intent(); operation.Prepared();
    f.Finalize(db::PhysicalMgaCowFinalizeDecision::commit);
    Require(!operation.Observe().ok, "committed inventory must not authorize undo of unpublished statement");
  }
  {
    Fixture f; Operation operation(f); operation.Intent();
    std::ofstream file(f.context.database_path + ".sb.mga_savepoints", std::ios::binary | std::ios::app);
    file << "torn"; file.close();
    Require(!operation.Observe().ok, "corrupt marker cannot supply barrier");
  }
  DurableStoreCases();
  ProcessKillDurableStoreCases();
  PublicationFaultCases();
  std::cout << "DELETE MGA recovery observation and durable store checks=" << checks << " failures=0\n";
}
