// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/owned_bootstrap_database_fixture.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "local_transaction_store.hpp"
#include "engine/statement_scratch_memory.hpp"
#include "dml/dml_ingestion_pipeline.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "nosql/key_value_api.hpp"
#include "security/security_principal_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifdef SB_NATIVE_KV_WRITE_WRAP
namespace kv_rollback_fault {
std::atomic<bool> armed{false};
std::atomic<unsigned> failures{0};
std::string exact_path;
}
namespace kv_release_fault {
enum class Phase { none, file_sync, directory_sync, file_close, partial_write };
enum class Compensation { none, truncate, file_sync };
std::atomic<bool> armed{false}, captured{false}, release_seen{false}, primary_failed{false}, compensation_started{false};
std::atomic<unsigned> full_release_writes{0}, primary_failures{0}, truncate_failures{0}, sync_failures{0};
std::atomic<unsigned> partial_bytes{0};
std::atomic<int> writer_fd{-1};
Phase phase = Phase::none;
Compensation compensation = Compensation::none;
std::array<std::uint8_t,16> operation{};
std::string exact_path, exact_directory;
bool Target(int fd, std::string_view expected) noexcept {
  char link[64], path[4096];
  const int n = std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(link)) return false;
  const auto length = ::readlink(link,path,sizeof(path));
  return length > 0 && std::string_view(path,static_cast<std::size_t>(length)) == expected;
}
bool Frame(const void* data,std::size_t count,unsigned kind) noexcept {
  if (!data || count < 124) return false;
  const auto* b = static_cast<const unsigned char*>(data);
  constexpr unsigned char magic[]{0x89,'S','B','S','P','2','\r','\n'};
  std::uint32_t framed = 0;
  for (unsigned i=0;i<4;++i) framed |= std::uint32_t(b[8+i]) << (8*i);
  return framed == count && std::equal(std::begin(magic),std::end(magic),b) && b[12] == kind && b[13] == 1;
}
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);
extern "C" int __real_ftruncate(int,off_t);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t count) {
  if (kv_rollback_fault::armed.load(std::memory_order_acquire) && data && count >= 14) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    constexpr unsigned char magic[]{0x89,'S','B','S','P','2','\r','\n'};
    if (std::equal(std::begin(magic),std::end(magic),bytes) && bytes[12] == 3 && bytes[13] == 1) {
      char link[64], path[4096];
      const int n = std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
      if (n > 0 && static_cast<std::size_t>(n) < sizeof(link)) {
        const auto length = ::readlink(link,path,sizeof(path));
        if (length > 0 && std::string_view(path,static_cast<std::size_t>(length)) ==
            kv_rollback_fault::exact_path) {
          kv_rollback_fault::failures.fetch_add(1,std::memory_order_relaxed);
          errno = EIO;
          return -1; // No marker byte was written; other journals are untouched.
        }
      }
    }
  }
  if (kv_release_fault::armed.load(std::memory_order_acquire) &&
      kv_release_fault::phase == kv_release_fault::Phase::partial_write &&
      !kv_release_fault::primary_failed.load() && kv_release_fault::captured.load() &&
      kv_release_fault::Target(fd,kv_release_fault::exact_path)) {
    if (kv_release_fault::partial_bytes.load() && fd == kv_release_fault::writer_fd.load()) {
      kv_release_fault::primary_failed.store(true); kv_release_fault::primary_failures.fetch_add(1);
      errno = EIO; return -1; // The second append call writes ZERO bytes.
    }
    const auto* b = static_cast<const unsigned char*>(data);
    if (kv_release_fault::Frame(data,count,2) &&
        std::equal(kv_release_fault::operation.begin(),kv_release_fault::operation.end(),b+76)) {
      const auto written = __real_write(fd,data,20);
      if (written > 0) {
        kv_release_fault::partial_bytes.store(static_cast<unsigned>(written));
        kv_release_fault::writer_fd.store(fd); kv_release_fault::release_seen.store(true);
      }
      return written; // Report exactly the bytes the real write completed.
    }
  }
  const auto written = __real_write(fd,data,count);
  if (written >= 0 && static_cast<std::size_t>(written) == count &&
      kv_release_fault::armed.load(std::memory_order_acquire) &&
      kv_release_fault::Target(fd,kv_release_fault::exact_path)) {
    const auto* b = static_cast<const unsigned char*>(data);
    if (!kv_release_fault::captured.load() && kv_release_fault::Frame(data,count,4)) {
      std::copy_n(b+76,16,kv_release_fault::operation.begin());
      kv_release_fault::captured.store(true,std::memory_order_release);
    } else if (kv_release_fault::captured.load(std::memory_order_acquire) &&
        kv_release_fault::Frame(data,count,2) &&
        std::equal(kv_release_fault::operation.begin(),kv_release_fault::operation.end(),b+76)) {
      kv_release_fault::writer_fd.store(fd);
      kv_release_fault::full_release_writes.fetch_add(1);
      kv_release_fault::release_seen.store(true,std::memory_order_release);
    }
  }
  return written;
}
extern "C" int __wrap_fsync(int fd) {
  using namespace kv_release_fault;
  if (armed.load(std::memory_order_acquire) && release_seen.load(std::memory_order_acquire)) {
    if (!primary_failed.load() &&
        ((phase == Phase::file_sync && fd == writer_fd.load() && Target(fd,exact_path)) ||
         (phase == Phase::directory_sync && Target(fd,exact_directory)))) {
      primary_failed.store(true); primary_failures.fetch_add(1); errno = EIO; return -1;
    }
    if (primary_failed.load() && compensation_started.load() &&
        compensation == Compensation::file_sync && Target(fd,exact_path)) {
      sync_failures.fetch_add(1); errno = ENOSPC; return -1;
    }
  }
  return __real_fsync(fd);
}
extern "C" int __wrap_close(int fd) {
  using namespace kv_release_fault;
  if (armed.load(std::memory_order_acquire) && release_seen.load(std::memory_order_acquire) &&
      !primary_failed.load() && phase == Phase::file_close && fd == writer_fd.load() && Target(fd,exact_path)) {
    const int closed = __real_close(fd);
    if (closed == 0) {
      primary_failed.store(true); primary_failures.fetch_add(1);
      errno = EIO; return -1; // Deliberately ambiguous: the writer really closed.
    }
    return closed;
  }
  return __real_close(fd);
}
extern "C" int __wrap_ftruncate(int fd,off_t length) {
  using namespace kv_release_fault;
  if (armed.load(std::memory_order_acquire) && primary_failed.load() && Target(fd,exact_path)) {
    compensation_started.store(true);
    if (compensation == Compensation::truncate) {
      truncate_failures.fetch_add(1); errno = ENOSPC; return -1;
    }
  }
  return __real_ftruncate(fd,length);
}
#endif

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace t = scratchbird::tests;
namespace {
unsigned checks = 0;
void Check(bool condition, std::string_view description) {
  ++checks;
  if (!condition) {
    // Synchronous admission catches callback exceptions, so retain the exact
    // failed assertion even when its outer ABI returns INTERNAL_ERROR.
    std::cerr << "check failed: " << description << '\n';
    throw std::runtime_error(std::string(description));
  }
}
void Good(const api::EngineApiResult& result, std::string_view description) {
  if (!result.ok) for (const auto& d : result.diagnostics)
    std::cerr << d.code << ':' << d.message_key << ':' << d.detail << '\n';
  Check(result.ok, description);
}
void Refused(const api::EngineApiResult& result, std::string_view description,
             std::string_view code, std::string_view detail,
             std::string_view key = "engine.nosql.key_value.refused") {
  Check(!result.ok && !result.diagnostics.empty() && result.result_shape.rows.empty(), description);
  const auto& actual = result.diagnostics.front();
  if (actual.code != code || actual.message_key != key || actual.detail != detail)
    std::cerr << "expected=" << code << ':' << key << ':' << detail << " actual="
              << actual.code << ':' << actual.message_key << ':' << actual.detail << '\n';
  Check(actual.error && actual.code == code && actual.message_key == key && actual.detail == detail,
      "refusal must retain the exact owning diagnostic vector");
}
const api::EngineTypedValue& Field(const api::EngineApiResult& result,
                                  std::size_t ordinal, std::string_view name) {
  Check(ordinal < result.result_shape.rows.size(), "missing result row");
  const auto& fields = result.result_shape.rows[ordinal].fields;
  const auto found = std::find_if(fields.begin(), fields.end(),
      [&](const auto& field) { return field.first == name; });
  Check(found != fields.end() && !found->second.isSqlNull(), "missing/non-value result field");
  return found->second;
}
std::string_view Payload(const api::EngineTypedValue& value) {
  if (!value.binary_value.empty()) return {reinterpret_cast<const char*>(value.binary_value.data()),
                                         value.binary_value.size()};
  return value.encoded_value;
}
api::EngineUuid RowIdentity(const api::EngineApiResult& result, std::size_t ordinal = 0) {
  const auto& key = Field(result, ordinal, "key_uuid");
  const auto& row = Field(result, ordinal, "row_uuid");
  const auto k = Payload(key), r = Payload(row);
  Check(k.size() == 16 && r.size() == 16 && k == r,
      "key/row identity must be the same actual binary16 stored identity");
  api::EngineUuid identity;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(r.data()), 16, identity.bytes.begin());
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(identity), "result row identity is not native UUIDv7");
  Check(result.result_shape.rows[ordinal].requested_row_uuid == identity,
      "outer result row identity must be the actual stored binary UUID, including duplicate results");
  return identity;
}
std::uint64_t Expiry(const api::EngineApiResult& result, std::size_t ordinal = 0) {
  const auto bytes = Payload(Field(result, ordinal, "ttl_expires_after_tx"));
  Check(bytes.size() == 8, "expiry result must be a native UINT64, not decimal text");
  std::uint64_t value = 0;
  for (unsigned i = 0; i != 8; ++i)
    value |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (i * 8);
  return value;
}
void Row(const api::EngineApiResult& result, std::size_t ordinal,
         std::string_view key, std::string_view value) {
  Check(Payload(Field(result, ordinal, "key")) == key &&
        Payload(Field(result, ordinal, "value")) == value, "exact key/value bytes changed");
  (void)RowIdentity(result, ordinal);
}

// Hex is ONLY the test child-process argument transport. Engine APIs below
// receive binary UUIDs; the child never hashes names or rebuilds identities.
std::string ArgumentUuid(const api::EngineUuid& id) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text;
  for (auto byte : id.bytes) { text += digits[byte >> 4]; text += digits[byte & 15]; }
  return text;
}
api::EngineUuid ArgumentUuid(std::string_view text) {
  Check(text.size() == 32, "child UUID argument width");
  api::EngineUuid id;
  const auto nibble = [](char ch) -> unsigned {
    if (ch >= '0' && ch <= '9') return unsigned(ch - '0');
    if (ch >= 'a' && ch <= 'f') return unsigned(ch - 'a' + 10);
    throw std::runtime_error("child UUID argument is not canonical hex");
  };
  for (std::size_t i = 0; i != 16; ++i)
    id.bytes[i] = std::uint8_t((nibble(text[2*i]) << 4) | nibble(text[2*i+1]));
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(id), "invalid child actual UUID");
  return id;
}

class Transaction final {
 public:
  explicit Transaction(const db::DatabaseCreateConfig& create, bool read_only = false) {
    context = t::BootstrapFixtureOwnerContext(create);
    const auto catalog = db::ReadDatabaseCatalogIdentity(create.path, context.database_uuid);
    Check(catalog.ok() && catalog.record->datatype_cohort.has_value(), "persisted datatype cohort missing");
    const auto& cohort = *catalog.record->datatype_cohort;
    context.datatype_catalog_snapshot_uuid = cohort.catalog_snapshot_uuid.value;
    context.datatype_catalog_generation = cohort.catalog_generation;
    context.datatype_registry_generation = cohort.registry_generation;
    session = std::make_unique<t::FixtureEngineSession>(context);
    api::EngineBeginTransactionRequest request;
    request.context = context;
    request.isolation_level = "read_committed";
    if (read_only) request.transaction_policy_profile.encoded_profiles.push_back("transaction_read_mode:read_only");
    const auto begun = api::EngineBeginTransaction(request);
    Good(begun, "actual native transaction BEGIN");
    Check(begun.local_transaction_id && !begun.transaction_uuid.is_nil(), "actual transaction identity absent");
    context.transaction_uuid = begun.transaction_uuid;
    context.local_transaction_id = begun.local_transaction_id;
    context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
    context.transaction_isolation_level = begun.isolation_level;
    context.read_only_mode = begun.read_only;
    active = true;
  }
  ~Transaction() noexcept {
    if (active) {
      api::EngineRollbackTransactionRequest request; request.context = context;
      if (!api::EngineRollbackTransaction(request).ok) std::terminate();
    }
  }
  void Finish(bool commit = true) {
    if (commit) {
      api::EngineCommitTransactionRequest request; request.context = context;
      Good(api::EngineCommitTransaction(request), "actual native transaction COMMIT");
    } else {
      api::EngineRollbackTransactionRequest request; request.context = context;
      Good(api::EngineRollbackTransaction(request), "actual native transaction ROLLBACK");
    }
    active = false;
    session->Close(); session.reset();
  }
  api::EngineRollbackTransactionResult RollbackKnownTerminal() {
    api::EngineRollbackTransactionRequest request; request.context = context;
    auto result = api::EngineRollbackTransaction(request);
    Check(result.engine_finality_known && result.transaction_uuid == context.transaction_uuid &&
        result.local_transaction_id == context.local_transaction_id &&
        ((result.ok && !result.post_inventory_secondary_failure &&
          result.rollback_finality_state == "rolled_back_by_engine_inventory") ||
         (!result.ok && result.post_inventory_secondary_failure &&
          result.rollback_finality_state == "rolled_back_post_inventory_secondary_failure")),
        "whole-TX cleanup must retain exact typed known rollback and secondary-cleanup disposition");
    active = false; // The actual inventory decision is terminal; cleanup is still owned below.
    return result;
  }
  void CloseTerminalSession() {
    Check(!active,"cannot close a session while this fixture still owns an active transaction");
    session->Close(); session.reset();
  }
  template<class Request> t::FixtureEngineRequest<Request> RequestFor(api::EngineUuid collection) {
    t::FixtureEngineRequest<Request> request(*session, context);
    request.target_object.uuid = collection;
    request.target_object.object_kind = "table";
    return request;
  }
  api::EngineKeyValueGetResult Get(api::EngineUuid collection, std::string key) {
    auto request = RequestFor<api::EngineKeyValueGetRequest>(collection);
    request.key = std::move(key); return api::EngineKeyValueGet(request);
  }
  api::EngineKeyValuePutResult Put(api::EngineUuid collection, std::string key,
                                  std::string value, std::uint64_t expiry = 0) {
    auto request = RequestFor<api::EngineKeyValuePutRequest>(collection);
    request.key = std::move(key); request.value = std::move(value);
    request.expires_after_local_transaction_id = expiry;
    return api::EngineKeyValuePut(request);
  }
  api::EngineKeyValueAtomicProgramResult Atomic(api::EngineUuid collection,
      std::vector<api::EngineKeyValueAtomicStep> steps) {
    auto request = RequestFor<api::EngineKeyValueAtomicProgramRequest>(collection);
    request.steps = std::move(steps); return api::EngineKeyValueAtomicProgram(request);
  }
  api::EngineRequestContext context;
  std::unique_ptr<t::FixtureEngineSession> session;
 private:
  bool active = false;
};

void OperationMemoryControls(Transaction& transaction) {
  namespace memory = scratchbird::core::memory;
  t::FixtureEngineStatement statement(*transaction.session,transaction.context);
  t::FixtureEngineStatement other(*transaction.session,transaction.context);
  auto wrong_principal = statement.context;
  wrong_principal.principal_uuid = api::GenerateCrudEngineUuid("object");
  auto wrong_epoch = statement.context;
  ++wrong_epoch.resource_epoch;
  auto missing_issuer = statement.context;
  missing_issuer.statement_scratch_memory_admission.reset();
  const auto baseline = memory::DefaultMemoryManager().Snapshot();
  bool zero_entered = false;
  Check(api::RunWithStatementOperationMemoryV1(statement.context,0,[&](auto&){zero_entered=true;}) ==
      SB_ENGINE_STATUS_INVALID_ARGUMENT && !zero_entered,"zero-byte operation cannot mint a grant");
  bool missing_entered = false;
  Check(api::RunWithStatementOperationMemoryV1(missing_issuer,65536,[&](auto&){missing_entered=true;}) ==
      SB_ENGINE_STATUS_INVALID_HANDLE && !missing_entered,"copied context without private issuer cannot admit memory");
  const auto admitted = api::RunWithStatementOperationMemoryV1(statement.context,65536,[&](auto& parent) {
    Check(parent.limit_bytes() > 1024 && parent.limit_bytes() <= 65536,
        "operation grant must respect both caller ceiling and actual available capacity");
    std::pmr::vector<std::byte> retained(parent.memory());
    retained.resize(256,std::byte{0x5a});
    const auto charged = parent.Snapshot();
    const auto capacity = memory::DefaultMemoryManager().Snapshot();
    Check(charged.active && charged.allocated_bytes == 256 && charged.reserved_bytes == parent.limit_bytes(),
        "operation payload must consume actual retained capacity");
    bool recursive_operation_entered = false;
    Check(api::RunWithStatementOperationMemoryV1(statement.context,65536,[&](auto&){
        recursive_operation_entered=true;}) == SB_ENGINE_STATUS_CONFLICT && !recursive_operation_entered,
        "operation recursion cannot acquire a second grant");
    bool leaf_entered = false;
    Check(api::RunWithStatementScratchMemoryV1(statement.context,[&](auto& leaf) {
      leaf_entered = true;
      Check(&leaf == &parent && leaf.memory() == parent.memory() &&
          leaf.protected_memory() == parent.protected_memory() &&
          &leaf.context() == &parent.context() && leaf.limit_bytes() == parent.limit_bytes(),
          "nested leaf must borrow the exact admitted operation owner and adapters");
      std::pmr::vector<std::byte> temporary(leaf.memory());
      temporary.resize(128,std::byte{0x33});
      Check(parent.Snapshot().allocated_bytes == 384 && retained.front() == std::byte{0x5a},
          "nested leaf must charge the same actual parent grant");
      const auto nested_capacity = memory::DefaultMemoryManager().Snapshot();
      Check(nested_capacity.current_bytes == capacity.current_bytes + 128 &&
          nested_capacity.reserved_capacity_bytes + 128 == capacity.reserved_capacity_bytes &&
          nested_capacity.active_capacity_reservation_count == capacity.active_capacity_reservation_count,
          "nested leaf must transfer exactly 128 bytes of existing capacity credit to payload without a new reservation");
      bool recursive_leaf_entered = false;
      Check(api::RunWithStatementScratchMemoryV1(statement.context,[&](auto&){recursive_leaf_entered=true;}) ==
          SB_ENGINE_STATUS_CONFLICT && !recursive_leaf_entered,"leaf recursion must refuse before lock reentry");
      bool leaf_operation_entered = false;
      Check(api::RunWithStatementOperationMemoryV1(statement.context,65536,[&](auto&){leaf_operation_entered=true;}) ==
          SB_ENGINE_STATUS_CONFLICT && !leaf_operation_entered,"leaf cannot recursively establish an operation scope");
    }) == SB_ENGINE_STATUS_OK && leaf_entered,"same-receipt nested leaf must be admitted");
    Check(parent.Snapshot().allocated_bytes == 256,"leaf cleanup must restore parent allocation credit");
    for (const auto* forged : {&wrong_principal,&wrong_epoch}) {
      bool entered = false;
      Check(api::RunWithStatementScratchMemoryV1(*forged,[&](auto&){entered=true;}) ==
          SB_ENGINE_STATUS_SECURITY_DENIED && !entered,"forged context cannot borrow actual operation memory");
    }
    bool other_entered = false;
    Check(api::RunWithStatementScratchMemoryV1(other.context,[&](auto&){other_entered=true;}) ==
        SB_ENGINE_STATUS_CONFLICT && !other_entered,"different actual receipt cannot borrow the current operation owner");
    scratchbird::core::platform::DiagnosticRecord exhaustion;
    bool exhausted_leaf_entered = false;
    const auto exhausted = api::RunWithStatementScratchMemoryV1(statement.context,[&](auto& leaf) {
      exhausted_leaf_entered = true;
      // Capacity rejects before OS protection or physical allocation. The
      // protected adapter carries this exact allocation's typed cause.
      (void)leaf.protected_memory()->allocate(static_cast<std::size_t>(leaf.limit_bytes())+1,
                                             alignof(std::max_align_t));
    },&exhaustion);
    Check(exhausted_leaf_entered && exhausted == SB_ENGINE_STATUS_RESOURCE_EXHAUSTED &&
        exhaustion.diagnostic_code == "SB_CEIC_012_MEMORY_RESOURCE.RESERVATION_EXCEEDED" &&
        exhaustion.message_key == "memory.ceic_012.resource.reservation_exceeded",
        "child exhaustion must retain the exact actual-grant diagnostic");
    Check(parent.Snapshot().allocated_bytes == 256 && retained.back() == std::byte{0x5a},
        "child exhaustion must preserve parent payload and credit");
    std::pmr::vector<std::byte> after_failure(parent.memory());
    after_failure.resize(128,std::byte{0x44});
    Check(parent.Snapshot().allocated_bytes == 384,"parent must remain usable after child exhaustion");
    bool recovered_leaf = false;
    Check(api::RunWithStatementScratchMemoryV1(statement.context,[&](auto& leaf) {
      recovered_leaf = true;
      std::pmr::vector<std::byte> child(leaf.memory()); child.resize(64,std::byte{0x22});
      Check(parent.Snapshot().allocated_bytes == 448,"later child allocation uses surviving same grant");
    }) == SB_ENGINE_STATUS_OK && recovered_leaf,"later leaf must be admitted after prior exhaustion");
    Check(parent.Snapshot().allocated_bytes == 384,"later child releases its own capacity");
  });
  Check(admitted == SB_ENGINE_STATUS_OK,"real-receipt bounded native operation memory scope");
  const auto after = memory::DefaultMemoryManager().Snapshot();
  Check(after.current_bytes == baseline.current_bytes &&
      after.reserved_capacity_bytes == baseline.reserved_capacity_bytes &&
      after.active_capacity_reservation_count == baseline.active_capacity_reservation_count &&
      after.active_allocation_count == baseline.active_allocation_count,
      "operation and nested leaves must release all real payload and capacity ownership");
}

void OperationPipelineControls(Transaction& transaction,api::EngineUuid collection) {
  t::FixtureEngineStatement statement(*transaction.session,transaction.context);
  api::DmlIngestionPipelineConfig prior_config;
  prior_config.context=statement.context; prior_config.input_row_count=1;
  api::DmlIngestionPipeline prior(prior_config);
  Check(!api::StatementOperationOwnsCurrentThreadV1(statement.context),
      "projected context alone cannot assert native owner-thread affinity");
  const auto status = api::RunWithStatementOperationMemoryV1(statement.context,65536,[&](auto& owner) {
    Check(!prior.Start() && prior.Snapshot().failed,
        "pipeline created outside actual owner scope cannot silently upgrade into it");
    Check(api::StatementOperationOwnsCurrentThreadV1(owner.context()),"actual native operation has owner-thread affinity");
    auto ordering = api::AcquireTransactionInventoryGuard(owner.context().database_path);
    api::DmlIngestionPipelineConfig config;
    config.context = owner.context(); config.target_table_uuid = collection;
    config.input_row_count = 9; // Exercise preallocator admission above its actual threshold.
    config.source_preallocation_bytes = 4096;
    config.option_envelopes = {"dml.ingest.writer=enabled","dml.ingest.writer_workers=2"};
    api::DmlIngestionPipeline pipeline(config);
    Check(pipeline.Start(),"native owner runs actual source-hint preallocation synchronously");
    std::vector<api::DmlIngestionPreallocationItem> items(9);
    for (auto& item : items) item.encoded_bytes = 64;
    Check(pipeline.EnqueuePreallocationBatch(std::move(items)),
        "native owner processes all requested preallocation items without a worker backlog");
    unsigned effects = 0;
    const auto thread = std::this_thread::get_id();
    for (unsigned i=0;i<3;++i)
      Check(pipeline.EnqueueWrite({"native_owner_test",1,[&] {
        Check(std::this_thread::get_id()==thread && api::StatementOperationOwnsCurrentThreadV1(config.context),
            "nested write must execute under its actual current-thread operation owner");
        auto nested_ordering = api::AcquireTransactionInventoryGuard(config.context.database_path);
        ++effects;
        api::EngineApiDiagnostic ok; ok.error=false; return ok;
      }}),"native writer options cannot force a cross-thread lock inversion");
    const auto stats = pipeline.Fence();
    Check(!stats.failed && !stats.writer_enabled && stats.writer_worker_count==0 &&
        stats.preallocator_enabled && !stats.preallocator_thread_started &&
        stats.source_size_hint_consumed && stats.preallocation_rows_enqueued==9 &&
        stats.write_tasks_enqueued==3 && stats.write_tasks_completed==3 && effects==3,
        "owner-thread execution must preserve real preallocation/write completion and honest worker accounting");
    api::DmlIngestionPipeline foreign(config);
    bool foreign_admitted = true, foreign_observed = true;
    std::thread outsider([&] {
      foreign_observed=api::StatementOperationOwnsCurrentThreadV1(config.context);
      foreign_admitted=foreign.Start();
    });
    outsider.join();
    Check(!foreign_observed && !foreign_admitted && foreign.Snapshot().failed,
        "copied context on another thread cannot inherit a private synchronous owner");
  });
  Check(status==SB_ENGINE_STATUS_OK && !api::StatementOperationOwnsCurrentThreadV1(statement.context),
      "native scheduling observation must end with the actual operation scope");
}

struct MemoryLockOrderState {
  std::mutex mutex;
  std::condition_variable changed;
  bool start = false, stop = false, paused = false, resume = false, done = false;
  std::atomic<bool> hook_failed{false}, hook_timed_out{false};
};
void BeforeLifetimeLock(void* opaque) noexcept {
  auto& state = *static_cast<MemoryLockOrderState*>(opaque);
  try {
    std::unique_lock guard(state.mutex);
    state.paused = true;
    state.changed.notify_all();
    if (!state.changed.wait_for(guard,std::chrono::seconds(10),[&] { return state.resume || state.stop; }))
      state.hook_timed_out.store(true);
  } catch (...) {
    state.hook_failed.store(true);
    state.changed.notify_all();
  }
}
void OperationInventoryLockOrder(Transaction& transaction, api::EngineUuid collection) {
  // Both receipts are issued before threads start; neither worker fabricates
  // a session/context or races statement acquisition with its session guard.
  t::FixtureEngineStatement held(*transaction.session,transaction.context);
  auto get = transaction.RequestFor<api::EngineKeyValueGetRequest>(collection);
  get.key = "ordering-probe-absent";
  auto inventory = api::AcquireTransactionInventoryGuard(transaction.context.database_path);
  inventory.unlock(); // Retain the actual mutex identity, not its lock.
  MemoryLockOrderState state;
  api::EngineKeyValueGetResult result;
  std::exception_ptr worker_failure, operation_failure;
  bool operation_entered = false, saw_paused_worker = false, inventory_was_free = false;
  std::thread worker([&] {
    try {
      bool ready = false;
      {
        std::unique_lock guard(state.mutex);
        ready = state.changed.wait_for(guard,std::chrono::seconds(10),[&] { return state.start || state.stop; });
        ready = ready && state.start && !state.stop;
      }
      if (ready) {
        struct ResetHook {
          ~ResetHook() { api::SetThreadStatementMemoryBeforeLifetimeLockHookForTestingV1(nullptr,nullptr); }
        } reset;
        api::SetThreadStatementMemoryBeforeLifetimeLockHookForTestingV1(BeforeLifetimeLock,&state);
        result = api::EngineKeyValueGet(get);
      }
    } catch (...) { worker_failure = std::current_exception(); }
    {
      std::lock_guard guard(state.mutex); state.done = true;
    }
    state.changed.notify_all();
  });
  sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
  try {
    status = api::RunWithStatementOperationMemoryV1(held.context,65536,[&](auto&) {
      // This callback is reached only while the actual lifetime+receipt
      // guards, MGA pin, governed memory and dispatch sentinel are retained.
      operation_entered = true;
      {
        std::unique_lock guard(state.mutex);
        state.start = true; state.changed.notify_all();
        const bool observed = state.changed.wait_for(guard,std::chrono::seconds(10),[&] {
          return state.paused || state.done || state.hook_failed.load();
        });
        saw_paused_worker = observed && state.paused && !state.done && !state.hook_failed.load();
      }
      if (saw_paused_worker) {
        // B is deterministically paused immediately before its lifetime lock.
        // An inventory-first direct API would already own this exact mutex;
        // try_lock detects that inversion without creating a test deadlock.
        inventory_was_free = inventory.try_lock();
        if (inventory_was_free) inventory.unlock();
      }
      {
        std::lock_guard guard(state.mutex); state.resume = true;
      }
      state.changed.notify_all();
    });
  } catch (...) { operation_failure = std::current_exception(); }
  // Release every test gate before join, including admission/callback failure.
  // Timeouts are cleanup bounds, never the evidence for lock ordering.
  {
    std::lock_guard guard(state.mutex); state.resume = true; state.stop = !operation_entered;
  }
  state.changed.notify_all();
  worker.join();
  if (operation_failure) std::rethrow_exception(operation_failure);
  if (worker_failure) std::rethrow_exception(worker_failure);
  Check(status == SB_ENGINE_STATUS_OK && operation_entered && saw_paused_worker &&
      !state.hook_failed.load() && !state.hook_timed_out.load(),
      "real operation and direct KV must reach the synchronized lifetime-lock boundary");
  Check(inventory_was_free,"direct KV must not seize inventory while awaiting the same session lifetime owner");
  Good(result,"direct KV must finish after operation/lifetime owner releases");
  Check(result.result_shape.rows.empty(),"ordering control must preserve the genuine absent exact-key result");
}

api::EngineUuid Collection(Transaction& transaction, std::string name,
                            std::string expiry_type = "uint64", bool nullable_key = false) {
  t::FixtureEngineStatement statement(*transaction.session, transaction.context);
  auto context = statement.context;
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = api::GenerateCrudEngineUuid("object");
  table.default_name = std::move(name);
  table.columns = {{"key", nullable_key ? "type=text;nullable=true;character_length=4096;unique=true"
                                      : "type=text;nullable=false;character_length=4096;unique=true"},
      {"value", "type=text;nullable=false;character_length=4096"},
      {"expires_after_local_transaction_id", "type=" + expiry_type + ";nullable=false"}};
  api::CrudIndexRecord unique;
  unique.creator_tx = context.local_transaction_id;
  unique.index_uuid = api::GenerateCrudEngineUuid("object");
  unique.table_uuid = table.table_uuid;
  unique.column_name = "key";
  unique.family = api::kCrudIndexFamilyBtree;
  unique.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  unique.default_name = table.default_name + "_key_unique";
  unique.key_envelopes = {"key"};
  unique.unique = true;
  api::CatalogColumnMetadata key_metadata;
  Check(api::AdmitCatalogColumnMetadata(table.columns[0].second,&key_metadata),
      "parse fixture key constraint metadata");
  key_metadata.identities["support_uuid"] = unique.index_uuid;
  key_metadata.identities["candidate_key_constraint_uuid"] = api::GenerateCrudEngineUuid("object");
  Check(api::EncodeCatalogColumnMetadata(key_metadata,&table.columns[0].second),
      "publish native binary constraint and unique backing-index identities");
  const auto diagnostic = t::PublishMgaTableFixture(context, table, {"text", "text", expiry_type}, {unique});
  if (diagnostic.error) std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':' << diagnostic.detail << '\n';
  Check(!diagnostic.error, "actual collection catalog/column/storage publication failed");
  const auto indexed = api::AppendMgaIndexMetadata(context, unique);
  if (indexed.error) std::cerr << indexed.code << ':' << indexed.message_key << ':' << indexed.detail << '\n';
  Check(!indexed.error, "actual unique key backing-index metadata publication failed");
  transaction.context.current_schema_uuid = context.current_schema_uuid;
  const auto loaded = api::LoadMgaRelationStorageDescriptor(context, table.table_uuid);
  Check(loaded.ok && loaded.descriptor.columns.size() == 3, "actual retained collection descriptor missing");
  Check(loaded.descriptor.indexes.size() == 1 && loaded.descriptor.indexes.front().unique &&
      loaded.descriptor.indexes.front().index_uuid == unique.index_uuid &&
      loaded.descriptor.indexes.front().family == api::kCrudIndexFamilyBtree,
      "collection must retain the real declared unique key backing index");
  for (unsigned i = 0; i != 2; ++i) {
    const auto& column = loaded.descriptor.columns[i];
    const auto charset = api::LookupEngineResourceDescriptorByUuid(context, column.charset_uuid, "charset");
    const auto collation = api::LookupEngineResourceDescriptorByUuid(context, column.collation_uuid, "collation");
    Check(charset.ok && collation.ok && charset.resource_descriptor.canonical_name == "UTF-8" &&
        collation.resource_descriptor.canonical_name == "SB_UTF8_BINARY" &&
        collation.resource_descriptor.parent_resource_uuid == column.charset_uuid,
        "collection must retain actual UTF8/binary-collation resources");
  }
  return table.table_uuid;
}

api::EngineUuid NativeStoredExpiry(Transaction& transaction, api::EngineUuid collection,
                         std::string_view key, api::EngineUuid expected_row, std::uint64_t expiry) {
  t::FixtureEngineStatement statement(*transaction.session, transaction.context);
  api::MgaVisibleHeapRelationReadRequest request;
  request.relation_uuid = collection; request.maximum_scanned_row_versions = 100000;
  request.maximum_decoded_bytes = 64*1024*1024; request.maximum_output_rows = 100000;
  request.maximum_memory_bytes = 64*1024*1024;
  request.cancellation_requested = statement.context.query_cancellation_requested;
  Check(static_cast<bool>(request.cancellation_requested),"independent read must retain the actual issued cancellation source");
  const auto ordering=api::AcquireTransactionInventoryGuard(statement.context.database_path);
  const auto inventory=db::AcquireStrongLocalTransactionInventorySnapshot(statement.context.database_path);
  Check(inventory.ok(),"independent heap ownership proof requires the actual native inventory");
  const auto read = api::ReadVisibleMgaHeapRelation(statement.context, request);
  if (!read.ok) std::cerr << read.diagnostic.code << ':' << read.diagnostic.detail << '\n';
  Check(read.ok, "independent native heap read of committed/staged KV rows");
  unsigned matches = 0;
  api::EngineUuid version;
  for (const auto& row : read.visible_rows) {
    const auto find = [&](std::string_view field) -> const api::CrudStoredValue& {
      const auto found = std::find_if(row.values.begin(), row.values.end(),
          [&](const auto& value) { return value.first == field; });
      Check(found != row.values.end() && found->second.isPresent(), "native stored KV field missing");
      return found->second;
    };
    if (find("key").bytes != key) continue;
    ++matches;
    version=row.version_uuid;
    const auto creator=scratchbird::transaction::mga::LookupLocalTransaction(inventory.snapshot->inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(row.creator_tx));
    Check(row.row_uuid == expected_row && scratchbird::core::uuid::IsEngineIdentityUuid(row.version_uuid) &&
        creator.ok() && creator.entry.identity.transaction_uuid.kind==scratchbird::core::platform::UuidKind::transaction &&
        scratchbird::core::uuid::IsEngineIdentityUuid(creator.entry.identity.transaction_uuid.value) &&
        (row.creator_transaction_uuid.is_nil() ||
         row.creator_transaction_uuid==creator.entry.identity.transaction_uuid.value),
        "KV ownership must match the actual persisted row/version and inventory-bound binary creator identity");
    const auto& bytes = find("expires_after_local_transaction_id").bytes;
    Check(bytes.size() == 8, "persisted expiry is not raw LE UINT64");
    for (unsigned i = 0; i != 8; ++i)
      Check(static_cast<unsigned char>(bytes[i]) == ((expiry >> (8*i)) & 255), "persisted UINT64 endian/value mismatch");
  }
  Check(matches == 1, "collection lacks exactly one actual visible version for exact key");
  return version;
}

void ExactRows(Transaction& transaction, api::EngineUuid collection) {
  const std::string nul_key("a\0b", 3), nul_value("v\0alue", 6);
  const std::string unicode_key("\xc3\xa9\xf0\x9f\x90\xa6"), unicode_value("\xe4\xb8\xad\xe6\x96\x87");
  for (const auto& [key,value] : std::vector<std::pair<std::string,std::string>>{
      {"", ""}, {nul_key,nul_value}, {unicode_key,unicode_value}, {"a", "one"}, {"ab", "two"},
      {"a\x7f", "ascii-high"}, {std::string("a\xc2\x80"), "unicode-high"}}) {
    Good(transaction.Put(collection,key,value), "exact key/value PUT");
    const auto found = transaction.Get(collection,key); Good(found,"exact key/value GET");
    Check(found.result_shape.rows.size() == 1,"exact GET cardinality"); Row(found,0,key,value);
    Check(found.dml_summary.index_probes == 0 && found.dml_summary.visible_rows_scanned > 0,
        "populated native heap GET must not fabricate an index probe");
    Check(Expiry(found) == 0,"zero TTL must mean no expiry");
  }
  auto request = transaction.RequestFor<api::EngineKeyValueMultiGetRequest>(collection);
  request.keys = {unicode_key, "", unicode_key, "not-present", nul_key};
  const auto multi = api::EngineKeyValueMultiGet(request); Good(multi,"ordered duplicate MultiGet");
  Check(multi.result_shape.rows.size() == 4,"MultiGet must omit misses but preserve duplicate positions");
  Row(multi,0,unicode_key,unicode_value); Row(multi,1,"","");
  Row(multi,2,unicode_key,unicode_value); Row(multi,3,nul_key,nul_value);
  Check(RowIdentity(multi,0) == RowIdentity(multi,2),"duplicate request acquired new row identity");
  auto prefix = transaction.RequestFor<api::EngineKeyValueGetRequest>(collection);
  prefix.prefix_scan = true; prefix.prefix = "a";
  const auto range = api::EngineKeyValueGet(prefix); Good(range,"unsigned-byte prefix GET");
  Check(range.dml_summary.index_probes == 0 && range.dml_summary.visible_rows_scanned > 0,
      "prefix heap scan must report real scan work without index claims");
  const std::array<std::string,5> order{{"a",nul_key,"ab","a\x7f",std::string("a\xc2\x80")}};
  Check(range.result_shape.rows.size() == order.size(),"prefix cardinality");
  for (std::size_t i = 0; i != order.size(); ++i)
    Check(Payload(Field(range,i,"key")) == order[i],"prefix must use unsigned canonical-byte order");
  prefix.prefix.clear();
  const auto all = api::EngineKeyValueGet(prefix); Good(all,"explicit empty prefix scan");
  Check(all.result_shape.rows.size() == 7,"empty prefix must scan all, not request empty exact key");
  Check(transaction.Get(collection,"not-present").ok &&
        transaction.Get(collection,"not-present").result_shape.rows.empty(),"missing GET must be empty success");
}

#ifdef SB_NATIVE_KV_WRITE_WRAP
class RollbackWriteFault final {
 public:
  explicit RollbackWriteFault(const std::string& database_path) {
    Check(!kv_rollback_fault::armed.load(),"rollback fault must not nest");
    kv_rollback_fault::exact_path = database_path + ".sb.mga_savepoints";
    kv_rollback_fault::failures.store(0);
    kv_rollback_fault::armed.store(true,std::memory_order_release);
  }
  ~RollbackWriteFault() { Clear(); }
  void Clear() noexcept { kv_rollback_fault::armed.store(false,std::memory_order_release); }
};
std::string JournalBytes(const std::string& path) {
  std::ifstream input(path,std::ios::binary);
  Check(bool(input),"open owned marker journal for exact physical observation");
  return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void ExactDiagnostic(const api::EngineApiDiagnostic& diagnostic,std::string_view code,
                     std::string_view key,std::string_view detail) {
  if (diagnostic.code != code || diagnostic.message_key != key || diagnostic.detail != detail)
    std::cerr << "actual diagnostic=" << diagnostic.code << ':' << diagnostic.message_key << ':' << diagnostic.detail << '\n';
  Check(diagnostic.error && diagnostic.code == code && diagnostic.message_key == key && diagnostic.detail == detail,
      "exact native marker recovery diagnostic vector");
}
std::uint64_t ReleasePhysicalCause(const api::EngineApiDiagnostic& diagnostic,
    const api::EngineRequestContext& context,api::EngineUuid operation,
    kv_release_fault::Phase phase,kv_release_fault::Compensation compensation) {
  const auto field = [&](std::string_view name) -> std::string_view {
    const auto found = std::find_if(diagnostic.fields.begin(),diagnostic.fields.end(),
        [&](const auto& field) { return field.key == name; });
    Check(found != diagnostic.fields.end(),"physical recovery cause field missing");
    return found->value;
  };
  const auto numeric = [&](std::string_view name) {
    const auto text = field(name); std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),value);
    Check(parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size(),
        "physical recovery numeric cause malformed");
    return value;
  };
  for (const auto& [name,identity] : std::array<std::pair<std::string_view,api::EngineUuid>,3>{{
      {"database_uuid",context.database_uuid},{"transaction_uuid",context.transaction_uuid},
      {"savepoint_uuid",operation}}}) {
    const auto found = std::find_if(diagnostic.identity_fields.begin(),diagnostic.identity_fields.end(),
        [&](const auto& pair) { return pair.first == name; });
    Check(found != diagnostic.identity_fields.end() && found->second == identity,
        "physical recovery cause must retain exact actual binary identities");
  }
  using Phase = kv_release_fault::Phase;
  using Compensation = kv_release_fault::Compensation;
  const std::string_view append_phase = phase == Phase::file_sync ? "file_sync" :
      phase == Phase::directory_sync ? "directory_sync" : phase == Phase::file_close ? "writer_close" : "write";
  const std::string_view compensation_phase = compensation == Compensation::none ? "restored" :
      compensation == Compensation::truncate ? "truncate" : "file_sync";
  Check(field("append_phase") == append_phase && numeric("append_native_error") == EIO &&
      numeric("writer_close_native_error") == (phase == Phase::file_close ? EIO : 0) &&
      field("compensation_phase") == compensation_phase &&
      numeric("compensation_native_error") == (compensation == Compensation::none ? 0 : ENOSPC) &&
      numeric("native_error") == (compensation == Compensation::none ? EIO : ENOSPC),
      "primary append EIO and distinct compensation ENOSPC must retain their exact phases independently");
  Check(numeric("local_transaction_id") == context.local_transaction_id,
      "physical cause must retain actual allocated local transaction number");
  const auto original_end = numeric("original_end");
  Check(original_end > 0,"physical recovery cause must retain original journal extent");
  return original_end;
}
class ReleaseAppendFault final {
 public:
  ReleaseAppendFault(const api::EngineRequestContext& context,kv_release_fault::Phase phase,
                     kv_release_fault::Compensation compensation) : context_(context) {
    using namespace kv_release_fault;
    Check(!armed.load() && !kv_rollback_fault::armed.load(),"release fault owners must not overlap");
    exact_path = context.database_path + ".sb.mga_savepoints";
    exact_directory = std::filesystem::path(exact_path).parent_path().string();
    kv_release_fault::phase = phase; kv_release_fault::compensation = compensation;
    captured.store(false); release_seen.store(false); primary_failed.store(false); compensation_started.store(false);
    full_release_writes.store(0); primary_failures.store(0); truncate_failures.store(0); sync_failures.store(0);
    partial_bytes.store(0);
    writer_fd.store(-1); operation.fill(0);
    armed.store(true,std::memory_order_release);
  }
  ~ReleaseAppendFault() noexcept {
    Clear();
    // A failed assertion must not delete a node whose real FH/prefix owner is
    // still pending. Resolve only this exact captured original operation.
    try {
      if (kv_release_fault::captured.load() && api::ValidateMgaPendingSavepointAppendRecoveryV1(context_).error) {
        const auto identity = Operation();
        if (api::ReconcileMgaPendingSavepointAppendRecoveryV1(context_,identity).error) std::terminate();
        const auto inventory = db::AcquireStrongLocalTransactionInventorySnapshot(context_.database_path);
        if (!inventory.ok()) std::terminate();
        const auto transaction = scratchbird::transaction::mga::LookupLocalTransaction(inventory.snapshot->inventory,
            scratchbird::transaction::mga::MakeLocalTransactionId(context_.local_transaction_id));
        if (!transaction.ok() || transaction.entry.identity.transaction_uuid.value != context_.transaction_uuid) std::terminate();
        // Whole-transaction rollback already excludes all effects. Physical
        // cleanup cannot upgrade that terminal transaction to marker mutation.
        if (transaction.entry.state == scratchbird::transaction::mga::TransactionState::rolled_back) return;
        const auto name = api::MgaSavepointUuidKey(identity);
        const auto marker = api::ObserveUniqueMgaSavepointMarkerV1(context_,name);
        if (!marker.ok) std::terminate();
        if (marker.lifecycle == api::MgaSavepointMarkerLifecycle::active &&
            (api::RollbackToMgaSavepointMarker(context_,name).error ||
             api::ReleaseMgaSavepointMarker(context_,name).error)) std::terminate();
      }
    } catch (...) { std::terminate(); }
  }
  void Clear() noexcept { kv_release_fault::armed.store(false,std::memory_order_release); }
  api::EngineUuid Operation() const {
    Check(kv_release_fault::captured.load(std::memory_order_acquire),"fault must capture an actual written native operation identity");
    api::EngineUuid identity; identity.bytes = kv_release_fault::operation;
    Check(scratchbird::core::uuid::IsEngineIdentityUuid(identity),"captured native operation UUID invalid");
    return identity;
  }
 private:
  const api::EngineRequestContext& context_;
};
class OriginalPrefixMutation final {
 public:
  explicit OriginalPrefixMutation(const std::string& path) {
    fd_ = ::open(path.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    Check(fd_ >= 0,"open exact owned journal for prefix integrity negative");
    if (::pread(fd_,&original_,1,0) != 1) { ::close(fd_); fd_=-1; throw std::runtime_error("read original journal prefix byte"); }
    const auto corrupt = static_cast<unsigned char>(original_ ^ 1);
    changed_ = true;
    if (::pwrite(fd_,&corrupt,1,0) != 1 || ::fsync(fd_) != 0) {
      Restore(); throw std::runtime_error("publish explicit prefix corruption fixture");
    }
  }
  ~OriginalPrefixMutation() noexcept { Restore(); }
  void Restore() noexcept {
    if (fd_ < 0) return;
    if (changed_ && (::pwrite(fd_,&original_,1,0) != 1 || ::fsync(fd_) != 0)) std::terminate();
    changed_ = false;
    if (::close(fd_) != 0) std::terminate();
    fd_ = -1;
  }
 private:
  int fd_ = -1; unsigned char original_ = 0; bool changed_ = false;
};
class JournalInodeReplacement final {
 public:
  explicit JournalInodeReplacement(const std::string& path) : path_(path), saved_(path+".owned-inode-probe") {
    const auto bytes = JournalBytes(path);
    Check(!std::filesystem::exists(saved_),"inode probe must not overwrite another file");
    std::filesystem::rename(path_,saved_); active_ = true;
    try {
      std::ofstream replacement(path_,std::ios::binary|std::ios::trunc);
      replacement.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); replacement.close();
      Check(!replacement.fail(),"write exact-byte different-inode negative fixture");
    } catch (...) { Restore(); throw; }
  }
  ~JournalInodeReplacement() noexcept { Restore(); }
  void Restore() noexcept {
    if (!active_) return;
    try { std::filesystem::remove(path_); std::filesystem::rename(saved_,path_); active_ = false; }
    catch (...) { std::terminate(); }
  }
 private:
  std::string path_,saved_; bool active_ = false;
};
void ReleaseAppendCompensation(Transaction& transaction,api::EngineUuid collection) {
  using Phase = kv_release_fault::Phase;
  using Compensation = kv_release_fault::Compensation;
  unsigned number = 0;
  for (const auto [phase,compensation] : std::array<std::pair<Phase,Compensation>,6>{{
      {Phase::file_sync,Compensation::none},{Phase::directory_sync,Compensation::none},
      {Phase::file_close,Compensation::none},{Phase::partial_write,Compensation::none},
      {Phase::file_sync,Compensation::truncate},
      {Phase::file_close,Compensation::file_sync}}}) {
    const auto key = "release-prefix-" + std::to_string(number++);
    const auto prior = transaction.Get(collection,"counter"); Good(prior,"pre-release-fault stable row");
    const auto prior_identity = RowIdentity(prior);
    const auto prior_value = std::string(Payload(Field(prior,0,"value")));
    auto pipeline = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(collection);
    pipeline.puts = {{key,"staged-until-actual-reconciliation",0},{"counter","staged-release-update",0}};
    pipeline.get_keys = {"counter"};
    // Prepare all receipts before uncertainty: issuance itself may correctly
    // refuse while the native marker source cannot prove compensation.
    auto read = transaction.RequestFor<api::EngineKeyValueGetRequest>(collection); read.key = "counter";
    auto retry = transaction.RequestFor<api::EngineKeyValuePutRequest>(collection);
    retry.key = "retry-must-not-allocate"; retry.value = "forbidden-prefix";
    ReleaseAppendFault fault(transaction.context,phase,compensation);
    const auto failed = api::EngineKeyValuePipeline(pipeline);
    const auto operation = fault.Operation();
    const auto name = api::MgaSavepointUuidKey(operation);
    Check(kv_release_fault::full_release_writes.load() >= 1 && kv_release_fault::primary_failures.load() == 1,
        "exact-operation release fault must be hit once and complete its full-frame recovery publication");
    if (phase == Phase::partial_write)
      Check(kv_release_fault::partial_bytes.load() == 20,"partial release must report exactly20 real written bytes before EIO");
    if (compensation == Compensation::none) {
      Refused(failed,"one-shot post-frame failure must compensate then rollback, not report success",
          "SB_ENGINE_API_INVALID_REQUEST","transaction.release_savepoint:savepoint_release_append_failed","engine.api.invalid_request");
      (void)ReleasePhysicalCause(failed.diagnostics.front(),transaction.context,operation,phase,compensation);
      Check(failed.dml_summary.rows_changed == 0 &&
          std::any_of(failed.evidence.begin(),failed.evidence.end(),[](const auto& evidence) {
            const auto* value = std::get_if<std::string>(&evidence.evidence_id);
            return evidence.evidence_kind == "kv_statement_atomicity" && value && *value == "rolled_back";
          }),"proven compensation must permit actual effect rollback");
      Check(kv_release_fault::compensation_started.load() &&
          kv_release_fault::full_release_writes.load() >= (phase == Phase::partial_write ? 1u : 2u) &&
          !api::ValidateMgaPendingSavepointAppendRecoveryV1(transaction.context).error,
          "one-shot failure must release all physical append custody after durable compensation");
      fault.Clear();
    } else {
      Refused(failed,"unproved compensation must retain exact native recovery responsibility",
          "MGA.STATEMENT.RECONCILIATION_REQUIRED","pending_native_savepoint_release_append","mga.statement.reconciliation_required");
      Check(failed.dml_summary.rows_changed == 2 &&
          std::any_of(failed.evidence.begin(),failed.evidence.end(),[](const auto& evidence) {
            const auto* value = std::get_if<std::string>(&evidence.evidence_id);
            return evidence.evidence_kind == "kv_statement_atomicity" && value && *value == "recovery_required";
          }),"unproved compensation cannot falsely erase actual physical row effects");
      Check(compensation == Compensation::truncate ? kv_release_fault::truncate_failures.load() > 0 :
          kv_release_fault::sync_failures.load() > 0,"persistent fault must hit actual independent compensation I/O");
      const auto pending = api::ValidateMgaPendingSavepointAppendRecoveryV1(transaction.context);
      ExactDiagnostic(pending,"MGA.STATEMENT.RECONCILIATION_REQUIRED","mga.statement.reconciliation_required",
          "pending_native_savepoint_release_append");
      const auto original_end = ReleasePhysicalCause(pending,transaction.context,operation,phase,compensation);
      Check(ReleasePhysicalCause(failed.diagnostics.front(),transaction.context,operation,phase,compensation) == original_end,
          "execution and retained-owner diagnostics must preserve the same original physical extent");
      const auto physical_end = std::filesystem::file_size(transaction.context.database_path+".sb.mga_savepoints");
      Check(original_end > 0 && (compensation == Compensation::truncate ? physical_end == original_end+124 :
          physical_end == original_end),"pending owner must distinguish failed truncate from unproved sync after real truncate");
      const auto parsed = api::ParseSavepoints(transaction.context);
      ExactDiagnostic(parsed.diagnostic,pending.code,pending.message_key,pending.detail);
      Check(parsed.active_savepoints.empty() && parsed.rollback_ranges.empty(),
          "uncertain release must not export the apparent post-release marker prefix");
      Refused(api::EngineKeyValueGet(read),"read must not bypass unproved marker compensation",
          pending.code,pending.detail,pending.message_key);
      Refused(api::EngineKeyValuePut(retry),"retry must reconcile original operation before further allocation",
          pending.code,pending.detail,pending.message_key);
      api::EngineCommitTransactionRequest commit; commit.context = transaction.context;
      Refused(api::EngineCommitTransaction(commit),"COMMIT cannot trust a fully written but unproved RELEASE",
          pending.code,pending.detail,pending.message_key);
      api::EnginePrepareTransactionRequest prepare; prepare.context = transaction.context;
      Refused(api::EnginePrepareTransaction(prepare),"PREPARE cannot bypass retained physical custody",
          pending.code,pending.detail,pending.message_key);
      api::EngineAutocommitBoundaryRequest automatic; automatic.context = transaction.context;
      automatic.statement_succeeded = true;
      Refused(api::EngineAutocommitBoundary(automatic),"successful autocommit cannot publish uncertain effects",
          pending.code,pending.detail,pending.message_key);
      fault.Clear();
      const auto journal = transaction.context.database_path+".sb.mga_savepoints";
      const auto physical = JournalBytes(journal);
      const auto wrong_operation = api::GenerateCrudEngineUuid("object");
      ExactDiagnostic(api::ReconcileMgaPendingSavepointAppendRecoveryV1(transaction.context,wrong_operation),
          "SB_ENGINE_API_INVALID_REQUEST","engine.api.invalid_request",
          "transaction.release_savepoint:native_savepoint_recovery_exact_owner_required");
      auto foreign = transaction.context; foreign.database_uuid = api::GenerateCrudEngineUuid("object");
      ExactDiagnostic(api::ReconcileMgaPendingSavepointAppendRecoveryV1(foreign,operation),
          "SB_ENGINE_API_INVALID_REQUEST","engine.api.invalid_request",
          "transaction.reconcile_savepoint_append:database_identity_mismatch");
      Check(JournalBytes(journal) == physical,"wrong recovery owner must have no physical effects");
      {
        OriginalPrefixMutation mutation(journal);
        const auto corrupted = JournalBytes(journal);
        const auto refused = api::ReconcileMgaPendingSavepointAppendRecoveryV1(transaction.context,operation);
        ExactDiagnostic(refused,pending.code,pending.message_key,pending.detail);
        Check(JournalBytes(journal) == corrupted,"changed original prefix must refuse before truncation");
      }
      Check(JournalBytes(journal) == physical,"integrity negative must restore exact owned original bytes");
      {
        JournalInodeReplacement replacement(journal);
        const auto refused = api::ReconcileMgaPendingSavepointAppendRecoveryV1(transaction.context,operation);
        ExactDiagnostic(refused,pending.code,pending.message_key,pending.detail);
        Check(JournalBytes(journal) == physical,"different inode cannot authorize compensation despite exact byte equality");
      }
      const auto retained = scratchbird::core::memory::DefaultMemoryManager().Snapshot();
      const auto reconciled = api::ReconcileMgaPendingSavepointAppendRecoveryV1(transaction.context,operation);
      Check(!reconciled.error,"explicit original-operation compensation through retained same-inode FH");
      const auto released = scratchbird::core::memory::DefaultMemoryManager().Snapshot();
      Check(released.current_bytes < retained.current_bytes &&
          released.active_allocation_count + 1 == retained.active_allocation_count,
          "explicit compensation must release the actually charged physical recovery owner");
      Check(!api::ValidateMgaPendingSavepointAppendRecoveryV1(transaction.context).error,
          "exact compensation must retire retained uncertainty");
      const auto active = api::ObserveUniqueMgaSavepointMarkerV1(transaction.context,name);
      Check(active.ok && active.lifecycle == api::MgaSavepointMarkerLifecycle::active && active.cutoffs.statement_fence,
          "physical compensation restores the original active fence, not user effects or transaction finality");
      Check(!api::RollbackToMgaSavepointMarker(transaction.context,name).error &&
          !api::ReleaseMgaSavepointMarker(transaction.context,name).error,
          "actual original fence rollback and release after physical reconciliation");
    }
    const auto observed = api::ObserveUniqueMgaSavepointMarkerV1(transaction.context,name);
    Check(observed.ok && observed.lifecycle == api::MgaSavepointMarkerLifecycle::released && observed.rolled_back,
        "every release fault must end with the same original fence explicitly rolled back and released");
    const auto restored = transaction.Get(collection,"counter"); Good(restored,"post-compensation stable native row");
    Row(restored,0,"counter",prior_value); Check(RowIdentity(restored) == prior_identity,"release failure changed stable row identity");
    const auto absent = transaction.Get(collection,key); Good(absent,"post-compensation inserted row exclusion");
    Check(absent.result_shape.rows.empty(),"failed release retained a user row after reconciliation");
    Check(!api::ValidateMgaStatementSavepointFinality(transaction.context).error,"completed exact recovery must allow later real COMMIT");
  }
}
void WholeTransactionReleaseCleanup(const db::DatabaseCreateConfig& create,api::EngineUuid collection) {
  Transaction transaction(create);
  const auto before = transaction.Get(collection,"durable"); Good(before,"whole-TX cleanup existing committed value");
  const auto identity = RowIdentity(before);
  auto pipeline = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(collection);
  pipeline.puts = {{"whole-tx-release-prefix","must-disappear",0},{"durable","must-not-commit",0}};
  ReleaseAppendFault fault(transaction.context,kv_release_fault::Phase::file_sync,
      kv_release_fault::Compensation::truncate);
  const auto failed = api::EngineKeyValuePipeline(pipeline);
  Refused(failed,"whole-TX fixture must retain actual release uncertainty",
      "MGA.STATEMENT.RECONCILIATION_REQUIRED","pending_native_savepoint_release_append","mga.statement.reconciliation_required");
  const auto operation = fault.Operation();
  (void)ReleasePhysicalCause(failed.diagnostics.front(),transaction.context,operation,
      kv_release_fault::Phase::file_sync,kv_release_fault::Compensation::truncate);
  Check(failed.dml_summary.rows_changed == 2 && kv_release_fault::truncate_failures.load() > 0,
      "whole-TX cleanup fixture requires actual effects and failed compensation");
  fault.Clear();
  const auto rollback = transaction.RollbackKnownTerminal();
  if (rollback.post_inventory_secondary_failure) {
    Refused(rollback,"known terminal rollback must preserve its outstanding secondary cleanup cause",
        "MGA.STATEMENT.RECONCILIATION_REQUIRED","pending_native_savepoint_release_append",
        "mga.statement.reconciliation_required");
  }
  const auto inventory = db::AcquireStrongLocalTransactionInventorySnapshot(create.path);
  Check(inventory.ok(),"independent actual native inventory after whole-TX rollback");
  const auto rolled_back = scratchbird::transaction::mga::LookupLocalTransaction(inventory.snapshot->inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(transaction.context.local_transaction_id));
  Check(rolled_back.ok() && rolled_back.entry.identity.transaction_uuid.value == transaction.context.transaction_uuid &&
      rolled_back.entry.state == scratchbird::transaction::mga::TransactionState::rolled_back,
      "cleanup permission requires the exact real terminal rollback decision");
  Check(api::ValidateMgaPendingSavepointAppendRecoveryV1(transaction.context).error,
      "whole-TX rollback must not silently discard unresolved physical append custody");
  ExactDiagnostic(api::ReconcileMgaPendingSavepointAppendRecoveryV1(transaction.context,api::GenerateCrudEngineUuid("object")),
      "SB_ENGINE_API_INVALID_REQUEST","engine.api.invalid_request",
      "transaction.release_savepoint:native_savepoint_recovery_exact_owner_required");
  const auto charged = scratchbird::core::memory::DefaultMemoryManager().Snapshot();
  Check(!api::ReconcileMgaPendingSavepointAppendRecoveryV1(transaction.context,operation).error,
      "actual rolled-back TX may clean up only its exact retained physical operation");
  const auto released = scratchbird::core::memory::DefaultMemoryManager().Snapshot();
  Check(released.current_bytes < charged.current_bytes &&
      released.active_allocation_count + 1 == charged.active_allocation_count &&
      !api::ValidateMgaPendingSavepointAppendRecoveryV1(transaction.context).error,
      "rolled-back cleanup must release actual owner memory/FHs and uncertainty");
  const auto no_upgrade = api::RollbackToMgaSavepointMarker(transaction.context,api::MgaSavepointUuidKey(operation));
  ExactDiagnostic(no_upgrade,"SB_ENGINE_API_INVALID_REQUEST","engine.api.invalid_request",
      "transaction.rollback_to_savepoint:active_local_transaction_required");
  Check(!api::RollbackMgaSecondaryIndexDeltaLedgerTransaction(transaction.context,
      transaction.context.local_transaction_id).error,
      "known rollback retains responsibility for real secondary-delta cleanup after physical reconciliation");
  const auto ledger = api::LoadMgaSecondaryIndexDeltaLedger(transaction.context);
  Check(ledger.ok && std::none_of(ledger.ledger.records.begin(),ledger.ledger.records.end(),
      [&](const auto& record) { return record.delta.local_transaction_id == transaction.context.local_transaction_id; }),
      "terminal cleanup must exclude every actual secondary delta belonging to the rolled-back transaction");
  transaction.CloseTerminalSession();
  Transaction read(create,true);
  const auto restored = read.Get(collection,"durable"); Good(restored,"independent reopen after terminal cleanup");
  Row(restored,0,"durable","committed"); Check(RowIdentity(restored) == identity,"terminal cleanup changed committed row identity");
  const auto absent = read.Get(collection,"whole-tx-release-prefix"); Good(absent,"terminal cleanup excludes staged insert");
  Check(absent.result_shape.rows.empty(),"whole-TX rollback failed to exclude physically staged row");
  read.Finish();
}
void RollbackFailureFence(Transaction& transaction, api::EngineUuid collection,
                          const api::EngineApiDiagnostic& length_cause) {
  const auto prior = transaction.Get(collection,"counter"); Good(prior,"pre-fault actual stable row");
  const auto prior_identity = RowIdentity(prior);
  const auto prior_value = std::string(Payload(Field(prior,0,"value")));
  const auto before = api::ParseSavepoints(transaction.context);
  Check(!before.diagnostic.error,"read actual savepoint state before rollback fault");
  const auto existing = before.active_savepoints.find(transaction.context.local_transaction_id);
  if (existing != before.active_savepoints.end()) for (const auto& [name,cutoffs] : existing->second) {
    (void)name; Check(!cutoffs.statement_fence,"fixture has no preexisting unresolved native operation");
  }
  RollbackWriteFault fault(transaction.context.database_path);
  auto pipeline = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(collection);
  pipeline.puts = {{"rollback-fault-prefix","retained-until-reconciled",0},
      {"counter","retained-update-until-reconciled",0},{"too-long",std::string(4097,'x'),0}};
  const auto failed = api::EngineKeyValuePipeline(pipeline);
  Refused(failed,"failed recovery must preserve original later-DML cause",
      length_cause.code,length_cause.detail,length_cause.message_key);
  Check(kv_rollback_fault::failures.load() == 1,"fault must hit one actual binary rollback marker write only");
  Check(failed.dml_summary.rows_changed == 2,"failed rollback must not falsely report that real preceding effects vanished");
  Check(std::any_of(failed.evidence.begin(),failed.evidence.end(),[](const auto& evidence) {
    const auto* value = std::get_if<std::string>(&evidence.evidence_id);
    return evidence.evidence_kind == "kv_statement_atomicity" && value && *value == "recovery_required";
  }),"failed rollback must retain explicit recovery responsibility");
  Check(std::any_of(failed.diagnostics.begin(),failed.diagnostics.end(),[](const auto& d) {
    return d.error && d.code == "SB_ENGINE_API_INVALID_REQUEST" && d.message_key == "engine.api.invalid_request" &&
        d.detail == "transaction.rollback_to_savepoint:savepoint_rollback_append_failed";
  }),"failed rollback must retain its exact separate physical recovery cause");
  const auto pending = api::ParseSavepoints(transaction.context);
  Check(!pending.diagnostic.error,"failed marker write must leave a readable intact native marker stream");
  const auto active = pending.active_savepoints.find(transaction.context.local_transaction_id);
  Check(active != pending.active_savepoints.end(),"failed rollback discarded its real pending statement fence");
  std::string name; std::uint64_t ordinal = 0; unsigned fences = 0;
  for (const auto& [identity,cutoffs] : active->second) if (cutoffs.statement_fence) {
    ++fences; name = identity; ordinal = cutoffs.creation_ordinal;
  }
  api::EngineUuid fence_uuid;
  Check(fences == 1 && ordinal && api::DecodeMgaSavepointUuidKey(name,&fence_uuid) &&
      scratchbird::core::uuid::IsEngineIdentityUuid(fence_uuid),"pending fence must be exact persisted binary kind4 identity");
  const auto observed = api::ObserveUniqueMgaSavepointMarkerV1(transaction.context,name);
  Check(observed.ok && observed.creation_ordinal == ordinal && observed.cutoffs.statement_fence &&
      observed.lifecycle == api::MgaSavepointMarkerLifecycle::active && !observed.rolled_back,
      "native history must retain the exact unreconciled operation, not a fabricated new identity");
  Row(transaction.Get(collection,"rollback-fault-prefix"),0,"rollback-fault-prefix","retained-until-reconciled");
  Row(transaction.Get(collection,"counter"),0,"counter","retained-update-until-reconciled");
  api::EngineCommitTransactionRequest commit; commit.context = transaction.context;
  Refused(api::EngineCommitTransaction(commit),"COMMIT must fence unresolved native effects",
      "MGA.STATEMENT.RECONCILIATION_REQUIRED","unresolved_native_statement_savepoint","mga.statement.reconciliation_required");
  api::EnginePrepareTransactionRequest prepare; prepare.context = transaction.context;
  Refused(api::EnginePrepareTransaction(prepare),"PREPARE must fence unresolved native effects",
      "MGA.STATEMENT.RECONCILIATION_REQUIRED","unresolved_native_statement_savepoint","mga.statement.reconciliation_required");
  api::EngineAutocommitBoundaryRequest autocommit; autocommit.context = transaction.context;
  autocommit.statement_succeeded = true;
  Refused(api::EngineAutocommitBoundary(autocommit),"successful-autocommit must fence unresolved native effects",
      "MGA.STATEMENT.RECONCILIATION_REQUIRED","unresolved_native_statement_savepoint","mga.statement.reconciliation_required");
  Check(kv_rollback_fault::failures.load() == 1,"refused finality must not silently retry the unresolved storage operation");
  fault.Clear();
  const auto rollback = api::RollbackToMgaSavepointMarker(transaction.context,name);
  Check(!rollback.error,"actual native rollback of the exact retained statement fence");
  const auto rewound = api::ObserveUniqueMgaSavepointMarkerV1(transaction.context,name);
  Check(rewound.ok && rewound.creation_ordinal == ordinal && rewound.rolled_back &&
      rewound.lifecycle == api::MgaSavepointMarkerLifecycle::active,
      "actual rollback must retain the same fence pending explicit release");
  const auto release = api::ReleaseMgaSavepointMarker(transaction.context,name);
  Check(!release.error,"actual native release after exact rollback completion");
  const auto released = api::ObserveUniqueMgaSavepointMarkerV1(transaction.context,name);
  Check(released.ok && released.creation_ordinal == ordinal && released.rolled_back &&
      released.lifecycle == api::MgaSavepointMarkerLifecycle::released,
      "reconciliation must release the original fence, not forget its history");
  const auto restored = transaction.Get(collection,"counter"); Good(restored,"read actually restored stable row");
  Row(restored,0,"counter",prior_value);
  Check(RowIdentity(restored) == prior_identity &&
      transaction.Get(collection,"rollback-fault-prefix").result_shape.rows.empty() &&
      transaction.Get(collection,"too-long").result_shape.rows.empty(),"actual native reconciliation must rewind all retained effects");
  NativeStoredExpiry(transaction,collection,"counter",prior_identity,0);
  Check(!api::ValidateMgaStatementSavepointFinality(transaction.context).error,
      "exact completed recovery must allow subsequent actual transaction finality");
  const std::string ordinary = "ordinary_savepoint_is_not_native_fence";
  Check(!api::CreateMgaSavepointMarker(transaction.context,ordinary).error &&
      !api::ValidateMgaStatementSavepointFinality(transaction.context).error &&
      !api::ReleaseMgaSavepointMarker(transaction.context,ordinary).error,
      "ordinary user savepoints must retain their non-fencing finality behavior");
}
#endif

void ProviderScopeRouting(Transaction& transaction,api::EngineUuid collection) {
  const auto snapshot = [&] {
    auto request = transaction.RequestFor<api::EngineKeyValueGetRequest>(collection);
    request.prefix_scan = true;
    const auto result = api::EngineKeyValueGet(request); Good(result,"local baseline around explicit provider routing");
    std::vector<std::array<std::string,4>> rows;
    for (std::size_t i = 0; i != result.result_shape.rows.size(); ++i)
      rows.push_back({std::string(Payload(Field(result,i,"key"))),std::string(Payload(Field(result,i,"value"))),
          ArgumentUuid(RowIdentity(result,i)),std::to_string(Expiry(result,i))});
    return rows;
  };
  const auto before = snapshot();
  using Scope = api::EngineNoSqlProviderScope;
  for (const auto scope : std::array<Scope,3>{Scope::kClusterOnly,Scope::kDistributed,static_cast<Scope>(255)}) {
    const bool cluster = scope != static_cast<Scope>(255);
    const auto refused = [&](const api::EngineApiResult& result,std::string_view operation) {
      if (cluster)
        Refused(result,"explicit non-local scope must never fall back to local KV execution",
            "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE",operation,"engine.api.cluster_authority_unavailable");
      else
        Refused(result,"invalid provider scope must retain its exact typed refusal",
            "SB_ENGINE_API_INVALID_REQUEST","key_value_provider_scope_invalid");
      Check(result.cluster_authority_required == cluster && result.dml_summary.rows_changed == 0,
          "routing refusal must distinguish a real cluster demand without claiming local effects");
    };
    auto get = transaction.RequestFor<api::EngineKeyValueGetRequest>(collection);
    get.key = "counter"; get.physical_proof.provider_contract.scope = scope;
    refused(api::EngineKeyValueGet(get),"nosql.key_value_get");
    auto multi = transaction.RequestFor<api::EngineKeyValueMultiGetRequest>(collection);
    multi.keys = {"counter","counter"}; multi.physical_proof.provider_contract.scope = scope;
    refused(api::EngineKeyValueMultiGet(multi),"nosql.key_value_multiget");
    auto pipeline = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(collection);
    pipeline.puts = {{"provider-routing-no-insert","forbidden",0},{"counter","forbidden-update",0}};
    pipeline.get_keys = {"counter"}; pipeline.physical_proof.provider_contract.scope = scope;
    refused(api::EngineKeyValuePipeline(pipeline),"nosql.key_value_pipeline");
    auto atomic = transaction.RequestFor<api::EngineKeyValueAtomicProgramRequest>(collection);
    atomic.steps = {{"set","provider-routing-no-insert","forbidden"},{"set","counter","forbidden-update"}};
    atomic.physical_proof.provider_contract.scope = scope;
    refused(api::EngineKeyValueAtomicProgram(atomic),"nosql.key_value_atomic_program");
  }
  Check(snapshot() == before,"all explicit non-local/invalid scope refusals must preserve exact local rows/UUIDs/expiry");
  const auto absent = transaction.Get(collection,"provider-routing-no-insert");
  Good(absent,"local read remains usable after routing refusals");
  Check(absent.result_shape.rows.empty(),"routing refusal must never stage the proposed local insert");
}
void AtomicAndLimits(Transaction& transaction, api::EngineUuid collection, api::EngineUuid empty_collection) {
  auto pipeline = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(collection);
  pipeline.puts = {{"pipe","first",0},{"pipe","last",0},{"other","other",0}};
  pipeline.get_keys = {"pipe","missing","other","pipe"};
  pipeline.max_admitted_operations = 7;
  const auto result = api::EngineKeyValuePipeline(pipeline); Good(result,"pipeline frozen PUTs before ordered GETs");
  Check(result.result_shape.rows.size() == 3,"pipeline result duplicate/miss cardinality");
  Row(result,0,"pipe","last"); Row(result,1,"other","other"); Row(result,2,"pipe","last");
  Check(RowIdentity(result,0) == RowIdentity(result,2),"pipeline duplicate identity");
  Good(transaction.Atomic(collection,{{"increment_i64","counter","2"},{"append","counter","3"}}),
      "missing increment uses zero and subsequent step sees prior effect");
  Row(transaction.Get(collection,"counter"),0,"counter","23");
  const auto counter = RowIdentity(transaction.Get(collection,"counter"));
  const auto version=NativeStoredExpiry(transaction,collection,"counter",counter,0);
  const auto noop=transaction.Put(collection,"counter","23");
  Good(noop,"validated same-value upsert remains successful without a new row version");
  Check(noop.dml_summary.rows_changed==0 && RowIdentity(noop)==counter &&
      NativeStoredExpiry(transaction,collection,"counter",counter,0)==version,
      "native no-op upsert must preserve actual row/version identity and honest zero changed count");
  Refused(transaction.Atomic(collection,{{"set","counter","partial"},{"unsupported","counter","x"}}),
      "invalid later atomic step must refuse without a usable prefix",
      api::kKeyValueAtomicProgramRefused,"key_value_unregistered_atomic_step");
  Row(transaction.Get(collection,"counter"),0,"counter","23");
  Check(RowIdentity(transaction.Get(collection,"counter")) == counter,"atomic refusal changed row identity");
  Refused(transaction.Atomic(collection,{{"set","must-not-exist","partial"},{"increment_i64","counter","1x"}}),
      "trailing operand bytes must refuse entire operation",
      api::kKeyValueAtomicProgramRefused,"key_value_increment_requires_complete_int64");
  Check(transaction.Get(collection,"must-not-exist").result_shape.rows.empty(),"atomic failure retained earlier insert");
  for (const auto& [current,operand] : std::vector<std::pair<std::string,std::string>>{
      {"9223372036854775807","1"},{"-9223372036854775808","-1"},{"","1"},{"12junk","1"},
      {"0",""},{"0","1junk"},{"0"," 1"},{"0","+1"},{"0","9223372036854775808"}}) {
    Good(transaction.Put(collection,"invalid-number",current),"prepare exact invalid increment value");
    Refused(transaction.Atomic(collection,{{"append","counter","bad"},{"increment_i64","invalid-number",operand}}),
        "integer full-parse/overflow refusal", api::kKeyValueAtomicProgramRefused,
        (current == "9223372036854775807" || current == "-9223372036854775808")
            ? "key_value_increment_int64_overflow" : "key_value_increment_requires_complete_int64");
    Row(transaction.Get(collection,"invalid-number"),0,"invalid-number",current);
    Row(transaction.Get(collection,"counter"),0,"counter","23");
  }
  Good(transaction.Put(collection,"bounds","-9223372036854775808"),"minimum signed integer text data");
  Good(transaction.Atomic(collection,{{"increment_i64","bounds","9223372036854775807"}}),"valid wide addition");
  Row(transaction.Get(collection,"bounds"),0,"bounds","-1");
  auto limited = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(collection);
  limited.puts = {{"budget-no-effect","partial",0}}; limited.get_keys = {"counter"};
  limited.resource_limits.maximum_operations = 1;
  Refused(api::EngineKeyValuePipeline(limited),"operation budget must refuse complete pipeline",
      "RESOURCE.BUDGET_EXCEEDED","key_value_operation_count_exceeded");
  Check(transaction.Get(collection,"budget-no-effect").result_shape.rows.empty(),"budget refusal retained effects");
  // A genuinely empty separate collection ensures both original heap admission
  // and the one-row post-write reload fit. Only the second duplicate GET result
  // exceeds the output bound, after the real PUT has occurred.
  auto late_limit = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(empty_collection);
  late_limit.puts = {{"result-limit-no-effect","partial",0}};
  late_limit.get_keys = {"result-limit-no-effect","result-limit-no-effect"};
  late_limit.resource_limits.maximum_output_rows = 1;
  const auto late_result = api::EngineKeyValuePipeline(late_limit);
  Refused(late_result,"result budget failure must rewind preceding frozen PUTs",
      "RESOURCE.BUDGET_EXCEEDED","key_value_output_rows_exceeded");
  Check(late_result.dml_summary.rows_changed == 0,"rolled-back pipeline must not report retained changed rows");
  Check(std::any_of(late_result.evidence.begin(),late_result.evidence.end(),[](const auto& evidence) {
    const auto* value = std::get_if<std::string>(&evidence.evidence_id);
    return evidence.evidence_kind == "kv_statement_atomicity" && value && *value == "rolled_back";
  }),"post-write result refusal must carry actual savepoint rollback disposition");
  Check(transaction.Get(empty_collection,"result-limit-no-effect").result_shape.rows.empty(),
      "pipeline output failure retained staged write");
  for (bool prefix : {false,true}) {
    auto oversized_operand = transaction.RequestFor<api::EngineKeyValueGetRequest>(empty_collection);
    oversized_operand.prefix_scan = prefix;
    oversized_operand.resource_limits.maximum_decoded_bytes = 65536;
    std::string input(131073,'x'); input.front() = char(0xff);
    if (prefix) oversized_operand.prefix = std::move(input);
    else oversized_operand.key = std::move(input);
    Refused(api::EngineKeyValueGet(oversized_operand),
        "oversized exact/prefix operand must fail its byte budget before UTF8 inspection",
        "RESOURCE.BUDGET_EXCEEDED","key_value_operand_bytes_exceeded");
  }
  // UTF8/native datatype shape validation belongs to actual DML, not merely
  // KV byte budgeting. This payload is inside every KV resource bound but
  // exceeds the retained value-column character_length=4096.
  const std::string oversized(4097,'x');
  const auto direct_length = transaction.Put(collection,"too-long",oversized);
  Check(!direct_length.ok && !direct_length.diagnostics.empty() &&
      direct_length.diagnostics.front().error && direct_length.result_shape.rows.empty(),
      "actual DML must reject the retained TEXT bound before pipeline control");
  Check(direct_length.diagnostics.front().code == "CTB.TEXT.LENGTH_EXCEEDED" &&
      direct_length.diagnostics.front().detail == "text_destination_character_limit_exceeded",
      "insert must preserve the registered owning TEXT length cause");
  const auto update_length = transaction.Put(collection,"counter",oversized);
  Refused(update_length,"existing-row update must enforce the same destination TEXT extent",
      direct_length.diagnostics.front().code,direct_length.diagnostics.front().detail,
      direct_length.diagnostics.front().message_key);
  Row(transaction.Get(collection,"counter"),0,"counter","23");
  const auto before_dml_identity = RowIdentity(transaction.Get(collection,"counter"));
  auto late_dml = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(collection);
  late_dml.puts = {{"dml-prefix-insert","must-not-remain",0},
      {"counter","must-rewind",0},{"too-long",oversized,0}};
  const auto failed_dml = api::EngineKeyValuePipeline(late_dml);
  const auto& length_cause = direct_length.diagnostics.front();
  Refused(failed_dml,"later actual DML failure must preserve the original owning length diagnostic",
      length_cause.code,length_cause.detail,length_cause.message_key);
  Check(failed_dml.dml_summary.rows_changed == 0 &&
      std::any_of(failed_dml.evidence.begin(),failed_dml.evidence.end(),[](const auto& evidence) {
        const auto* value = std::get_if<std::string>(&evidence.evidence_id);
        return evidence.evidence_kind == "kv_statement_atomicity" && value && *value == "rolled_back";
      }),"later DML failure requires actual rollback disposition and no retained effects");
  Row(transaction.Get(collection,"counter"),0,"counter","23");
  Check(RowIdentity(transaction.Get(collection,"counter")) == before_dml_identity,
      "failed DML pipeline changed prior row identity");
  Check(transaction.Get(collection,"dml-prefix-insert").result_shape.rows.empty() &&
      transaction.Get(collection,"too-long").result_shape.rows.empty(),
      "later DML failure retained the preceding inserted row or invalid row");
#ifdef SB_NATIVE_KV_WRITE_WRAP
  RollbackFailureFence(transaction,collection,length_cause);
  ReleaseAppendCompensation(transaction,collection);
#endif
  for (unsigned bound = 0; bound != 5; ++bound) {
    auto get = transaction.RequestFor<api::EngineKeyValueGetRequest>(collection);
    get.prefix_scan = true;
    if (bound == 0) get.resource_limits.maximum_output_rows = 1;
    if (bound == 1) get.resource_limits.maximum_scanned_row_versions = 1;
    if (bound == 2) get.resource_limits.maximum_decoded_bytes = 1;
    if (bound == 3) get.resource_limits.maximum_memory_bytes = 1;
    if (bound == 4) get.resource_limits.maximum_operations = 0;
    constexpr std::array<std::string_view,4> details{{
        "key_value_output_rows_exceeded",
        "mga.heap_relation_read:heap_read_maximum_row_versions_exceeded",
        "mga.heap_relation_read:heap_read_maximum_decoded_bytes_exceeded",
        "mga.heap_relation_read:heap_read_maximum_memory_bytes_exceeded"}};
    Refused(api::EngineKeyValueGet(get),"bounded native scan must refuse instead of returning truncated success",
        bound == 0 || bound == 4 ? "RESOURCE.BUDGET_EXCEEDED" : "SB_ENGINE_API_INVALID_REQUEST",
        bound == 4 ? "key_value_resource_limits_required" : details[bound],
        bound == 0 || bound == 4 ? "engine.nosql.key_value.refused" : "engine.api.invalid_request");
  }
  for (bool throwing : {false,true}) {
    auto cancelled = transaction.RequestFor<api::EngineKeyValuePipelineRequest>(collection);
    cancelled.puts = {{"cancel-no-effect","partial",0}};
    cancelled.context.query_cancellation_requested = [throwing]() -> bool {
      if (throwing) throw std::runtime_error("cancel callback failure"); return true;
    };
    Refused(api::EngineKeyValuePipeline(cancelled),"cancellation before physical effects",
        "PROCESS.CANCELLED","key_value_cancelled_before_publication");
    Check(transaction.Get(collection,"cancel-no-effect").result_shape.rows.empty(),"cancelled pipeline retained effects");
  }
  Refused(transaction.Put(collection,std::string(1,char(0xff)),"bad"),"UTF8 key invalid encoding",
      "SB_MODEL_KEY_VALUE_TEXT_INVALID_V1","key_value_malformed_utf8");
  Refused(transaction.Put(collection,"invalid-utf8-value",std::string(1,char(0xff))),"UTF8 value invalid encoding",
      "SB_MODEL_KEY_VALUE_TEXT_INVALID_V1","key_value_malformed_utf8");
  Check(transaction.Get(collection,"invalid-utf8-value").result_shape.rows.empty(),"invalid UTF8 produced stored row");
}

void Child(const db::DatabaseCreateConfig& create, api::EngineUuid a, api::EngineUuid b,
           api::EngineUuid durable_row) {
  Transaction transaction(create,true);
  const auto durable = transaction.Get(a,"durable"); Good(durable,"independent process persisted GET");
  Row(durable,0,"durable","committed");
  Check(RowIdentity(durable) == durable_row,"independent process changed persisted row identity");
  Check(transaction.Get(a,"rolled-back").result_shape.rows.empty(),"rolled back row survived independent process reopen");
  Check(transaction.Get(a,"whole-tx-release-prefix").result_shape.rows.empty(),"whole-TX release fault row survived fresh-process reopen");
  const auto expired = transaction.Get(a,"expiring"); Good(expired,"reopened original expired row read");
  Check(expired.result_shape.rows.empty(),"rolled-back resurrection cleared durable original TTL");
  Row(transaction.Get(b,"durable"),0,"durable","second collection");
  Row(transaction.Get(a,std::string("a\0b",3)),0,std::string("a\0b",3),std::string("v\0alue",6));
  NativeStoredExpiry(transaction,a,"maximum-ttl",RowIdentity(transaction.Get(a,"maximum-ttl")),
      std::numeric_limits<std::uint64_t>::max());
  transaction.Finish();
}
void ReopenProcess(const std::string& executable, const db::DatabaseCreateConfig& create,
                   api::EngineUuid a, api::EngineUuid b, api::EngineUuid durable) {
  std::vector<std::string> args{executable,"--reopen",create.path,ArgumentUuid(create.database_uuid.value),
      ArgumentUuid(create.filespace_uuid.value),ArgumentUuid(a),ArgumentUuid(b),ArgumentUuid(durable)};
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
#ifdef _WIN32
  const auto result = _spawnv(_P_WAIT,executable.c_str(),argv.data());
  Check(result == 0,"fresh executable process native KV reopen failed");
#else
  pid_t child = 0;
  Check(posix_spawn(&child,executable.c_str(),nullptr,nullptr,argv.data(),environ) == 0,
      "cannot launch independent KV reopen process");
  int status = 0; pid_t waited;
  do { waited = waitpid(child,&status,0); } while (waited < 0 && errno == EINTR);
  Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
      "fresh executable process native KV reopen failed");
#endif
}
void TextDestinationLive() {
  t::OwnedTempDirectory directory;
  db::DatabaseCreateConfig create;
  create.path = (directory.path()/"native_text.sbdb").string();
  create.database_uuid = {scratchbird::core::platform::UuidKind::database,api::GenerateCrudEngineUuid("object")};
  create.filespace_uuid = {scratchbird::core::platform::UuidKind::filespace,api::GenerateCrudEngineUuid("object")};
  t::ConfigureCredentialedFixtureBootstrap(create);
  const auto made=db::CreateDatabaseFile(create);
  Check(made.ok()&&made.create_finality==db::DatabaseCreateFinalityClass::committed,"actual TEXT node creation");
  api::EngineUuid collection;
  {Transaction transaction(create);collection=Collection(transaction,"text_bounds");transaction.Finish();}
  std::string boundary;
  for(unsigned i=0;i<4096;++i)boundary.append("\xc3\xa9");
  std::vector<std::pair<std::string,api::EngineUuid>> committed;
  {
    Transaction transaction(create);
    for(bool staged:{false,true}) {
      const std::string key=staged?"staged-boundary":"direct-boundary";
      const auto insert=[&](std::string_view row_key,std::string_view bytes,bool conflict=false) {
        t::FixtureEngineRequest<api::EngineInsertRowsRequest> request(*transaction.session,transaction.context);
        request.target_table.uuid=collection;request.target_table.object_kind="table";
        if(staged)request.option_envelopes.push_back("direct_physical_insert=disabled");
        if(conflict) {
          request.on_conflict_action="do_update";request.conflict_target_column="key";
          request.conflict_update_columns={"value"};
        }
        const auto loaded=api::LoadMgaRelationStorageDescriptor(request.context,collection);
        Check(loaded.ok,"load freshly admitted actual TEXT DML destination");
        api::EngineRowValue row;
        for(unsigned i=0;i<3;++i) {
          api::EngineTypedValue value;value.descriptor=loaded.descriptor.columns[i].value_descriptor;
          const auto payload=i==0?row_key:i==1?bytes:std::string_view{};
          value.binary_value.assign(payload.begin(),payload.end());
          if(i==2)value.binary_value.assign(8,0);
          row.fields.push_back({loaded.descriptor.columns[i].canonical_name_key,std::move(value)});
        }
        request.input_rows.push_back(std::move(row));
        return api::EngineInsertRows(request);
      };
      const auto valid=insert(key,boundary);
      Good(valid,"actual TEXT scalar boundary larger than inline threshold must spill without truncation");
      Check(valid.inserted_count==1&&valid.row_uuids.size()==1,"actual TEXT insertion identity absent");
      Row(transaction.Get(collection,key),0,key,boundary);
      const auto version=NativeStoredExpiry(transaction,collection,key,valid.row_uuids.front(),0);
      for(const auto& invalid:std::vector<std::string>{boundary+"\xc3\xa9",std::string(1,char(0xff))}) {
        const bool encoding=invalid.size()==1;
        const auto refused=insert(key+"-invalid",invalid);
        Refused(refused,"actual selected INSERT receiver must preserve TEXT owning cause",
            encoding?"CTB.TEXT.INVALID_ENCODING":"CTB.TEXT.LENGTH_EXCEEDED",
            encoding?"text_destination_invalid_utf8":"text_destination_character_limit_exceeded",
            "engine.dml.text_destination.refused");
        Check(transaction.Get(collection,key+"-invalid").result_shape.rows.empty(),"refused TEXT input published a row");
      }
      Refused(insert(key,boundary+"\xc3\xa9",true),"conflict update must not bypass TEXT extent",
          "CTB.TEXT.LENGTH_EXCEEDED","text_destination_character_limit_exceeded","engine.dml.text_destination.refused");
      Row(transaction.Get(collection,key),0,key,boundary);
      Check(NativeStoredExpiry(transaction,collection,key,valid.row_uuids.front(),0)==version,
          "failed TEXT mutation changed actual persisted row/version identity");
      const std::string long_key(4097,'k');
      Refused(insert(long_key,"valid"),"key TEXT extent must use the same actual destination check",
          "CTB.TEXT.LENGTH_EXCEEDED","text_destination_character_limit_exceeded","engine.dml.text_destination.refused");
      committed.push_back({key,valid.row_uuids.front()});
    }
    transaction.Finish();
  }
  {Transaction transaction(create,true);
    for(const auto& [key,identity]:committed) {
      const auto read=transaction.Get(collection,key);Good(read,"reopened exact spilled TEXT");
      Row(read,0,key,boundary);Check(RowIdentity(read)==identity,"TEXT reopen changed binary row identity");
    }
    transaction.Finish();
  }
  directory.Cleanup();
}
void Run(const std::string& executable) {
  t::OwnedTempDirectory directory;
  db::DatabaseCreateConfig create;
  create.path = (directory.path()/"native_kv.sbdb").string();
  create.database_uuid = {scratchbird::core::platform::UuidKind::database,api::GenerateCrudEngineUuid("object")};
  create.filespace_uuid = {scratchbird::core::platform::UuidKind::filespace,api::GenerateCrudEngineUuid("object")};
  t::ConfigureCredentialedFixtureBootstrap(create);
  const auto made = db::CreateDatabaseFile(create);
  Check(made.ok() && made.create_finality == db::DatabaseCreateFinalityClass::committed,
      "credentialed native KV node creation");
  api::EngineUuid a,b,late_collection,bad_type,bad_nullable,durable,expiring;
  {
    Transaction transaction(create);
    a = Collection(transaction,"collection_a"); b = Collection(transaction,"collection_b");
    late_collection = Collection(transaction,"late_result_limit");
    bad_type = Collection(transaction,"bad_signed_expiry","bigint");
    bad_nullable = Collection(transaction,"bad_nullable_key","uint64",true);
    transaction.Finish();
  }
  std::uint64_t threshold = 0;
  {
    Transaction transaction(create);
    OperationMemoryControls(transaction);
    OperationPipelineControls(transaction,a);
    OperationInventoryLockOrder(transaction,a);
    ExactRows(transaction,a); AtomicAndLimits(transaction,a,late_collection);
    ProviderScopeRouting(transaction,a);
    Good(transaction.Put(a,"durable","original"),"persist original stable-identity row");
    durable = RowIdentity(transaction.Get(a,"durable"));
    Good(transaction.Put(a,"durable","committed"),"upsert actual existing row");
    Check(RowIdentity(transaction.Get(a,"durable")) == durable,"upsert generated replacement row identity");
    Good(transaction.Put(b,"durable","second collection"),"isolated collection write");
    Row(transaction.Get(a,"durable"),0,"durable","committed");
    Check(RowIdentity(transaction.Get(b,"durable")) != durable,"collections shared a key identity");
    const auto immediately_expired = transaction.Put(a,"already-expired","stored-but-not-visible",
        transaction.context.local_transaction_id);
    Good(immediately_expired,"PUT may store an expiry equal to the actual reader transaction");
    Row(immediately_expired,0,"already-expired","stored-but-not-visible");
    const auto immediate_identity = RowIdentity(immediately_expired);
    Check(Expiry(immediately_expired) == transaction.context.local_transaction_id &&
        Payload(Field(immediately_expired,0,"state")) == "expired",
        "PUT result must expose exact raw expiry and honest expired state at equality");
    const auto immediate_get = transaction.Get(a,"already-expired"); Good(immediate_get,"immediately expired GET");
    Check(immediate_get.result_shape.rows.empty(),"GET must omit a row expired at the actual current transaction");
    NativeStoredExpiry(transaction,a,"already-expired",immediate_identity,transaction.context.local_transaction_id);
    Good(transaction.Atomic(a,{{"increment_i64","already-expired","5"}}),"current-transaction expired row resurrects as absent");
    const auto immediate_resurrection = transaction.Get(a,"already-expired");
    Good(immediate_resurrection,"current-transaction expired row resurrection GET");
    Row(immediate_resurrection,0,"already-expired","5");
    Check(RowIdentity(immediate_resurrection) == immediate_identity && Expiry(immediate_resurrection) == 0,
        "immediate resurrection must retain the actual stored UUID and clear expiry");
    NativeStoredExpiry(transaction,a,"already-expired",immediate_identity,0);
    threshold = transaction.context.local_transaction_id + 1;
    Check(threshold > transaction.context.local_transaction_id,"fixture transaction allocation exhausted");
    Good(transaction.Put(a,"expiring","visible-before-threshold",threshold),"store actual transaction threshold data");
    const auto before = transaction.Get(a,"expiring"); Good(before,"before-threshold read");
    Row(before,0,"expiring","visible-before-threshold"); Check(Expiry(before) == threshold,"TTL result raw value");
    expiring = RowIdentity(before); NativeStoredExpiry(transaction,a,"expiring",expiring,threshold);
    Good(transaction.Put(a,"maximum-ttl","maximum",std::numeric_limits<std::uint64_t>::max()),"maximum UINT64 expiry");
    Check(Expiry(transaction.Get(a,"maximum-ttl")) == std::numeric_limits<std::uint64_t>::max(),"maximum TTL narrowed");
    for (auto malformed : {bad_type,bad_nullable}) {
      Refused(transaction.Put(malformed,"forbidden","no effect"),"malformed collection descriptor PUT",
          "DATATYPE.DESCRIPTOR.INVALID","key_value_column_descriptor_invalid");
      Refused(transaction.Get(malformed,"forbidden"),"malformed collection descriptor GET",
          "DATATYPE.DESCRIPTOR.INVALID","key_value_column_descriptor_invalid");
    }
    auto missing = transaction.RequestFor<api::EngineKeyValueGetRequest>(api::GenerateCrudEngineUuid("object"));
    missing.key = "durable"; missing.physical_proof.proof_supplied = true;
    missing.physical_proof.exact_key_index_proof = true;
    missing.physical_proof.ttl_visibility_proof = true;
    const auto missing_source = api::LoadMgaRelationStorageDescriptor(missing.context,missing.target_object.uuid);
    Check(!missing_source.ok && missing_source.diagnostic.error,"absent collection native descriptor source must refuse");
    Refused(api::EngineKeyValueGet(missing),"caller proof flags cannot authorize absent collection",
        missing_source.diagnostic.code,missing_source.diagnostic.detail,missing_source.diagnostic.message_key);
    transaction.Finish();
  }
  {
    Transaction transaction(create);
    Check(transaction.context.local_transaction_id == threshold,"TTL equality must use actual next allocated transaction");
    const auto expired = transaction.Get(a,"expiring"); Good(expired,"at-threshold read");
    Check(expired.result_shape.rows.empty(),"TTL equality must expire row");
    NativeStoredExpiry(transaction,a,"expiring",expiring,threshold);
    Good(transaction.Atomic(a,{{"increment_i64","expiring","4"}}),"expired value acts as absent for atomic increment");
    const auto resurrected = transaction.Get(a,"expiring"); Good(resurrected,"resurrected exact read");
    Row(resurrected,0,"expiring","4");
    Check(RowIdentity(resurrected) == expiring && Expiry(resurrected) == 0,
        "resurrection must preserve stored row identity and clear TTL");
    Good(transaction.Put(a,"rolled-back","must-disappear"),"staged row before rollback");
    Good(transaction.Put(a,"durable","must-not-persist"),"staged update before rollback");
    transaction.Finish(false);
  }
  ReopenProcess(executable,create,a,b,durable);
#ifdef SB_NATIVE_KV_WRITE_WRAP
  WholeTransactionReleaseCleanup(create,a);
  ReopenProcess(executable,create,a,b,durable);
#endif
  {
    Transaction transaction(create,true);
    auto write = transaction.RequestFor<api::EngineKeyValuePutRequest>(a);
    write.key = "read-only-projection-forgery"; write.value = "must-not-persist";
    Check(write.context.read_only_mode,"actual read-only transaction must issue a read-only statement");
    Refused(api::EngineKeyValuePut(write),"actual read-only statement must preserve native DML refusal",
        "SB_ENGINE_API_INVALID_REQUEST","mga.transaction_authority:active_local_transaction_required",
        "engine.api.invalid_request");
    const auto receipt = write.statement_receipt();
    write.context.read_only_mode = false; // Attacker-controlled projection only.
    Refused(api::EngineKeyValuePut(write),"false projected read-only mode cannot enable native writes",
        "SB_ENGINE_API_INVALID_REQUEST","mga.transaction_authority:active_local_transaction_required",
        "engine.api.invalid_request");
    api::EngineRequestContext actual;
    sb_engine_result_t diagnostics = nullptr;
    const auto copied = scratchbird::server_engine_bridge::CopyStatementContextEngineContextV1(
        receipt,&actual,&diagnostics);
    t::FixtureEngineSession::Check(copied,diagnostics,"inspect actual read-only receipt after projection forgery");
    Check(actual.read_only_mode && actual.statement_receipt_uuid == write.context.statement_receipt_uuid &&
        actual.transaction_uuid == write.context.transaction_uuid,
        "projection forgery must not mutate privately issued read-only authority");
    const auto absent = transaction.Get(a,write.key); Good(absent,"read-only forgery no-effect read");
    Check(absent.result_shape.rows.empty(),"forged writable projection persisted a row");
    Row(transaction.Get(a,"durable"),0,"durable","committed");
    transaction.Finish();
  }
  {
    Transaction transaction(create);
    auto grant = transaction.RequestFor<api::EngineSecurityGrantPrivilegeRequest>(b);
    grant.grant_uuid = api::GenerateCrudEngineUuid("object");
    grant.grantee_uuid = transaction.context.principal_uuid;
    grant.grantee_kind = "principal"; grant.target_object_uuid = b;
    grant.target_object_kind = "table"; grant.privilege = "SELECT"; grant.grant_effect = "deny";
    Good(api::EngineSecurityGrantPrivilege(grant),"actual durable target-scoped deny publication");
    transaction.Finish();
  }
  {
    Transaction transaction(create);
    Refused(transaction.Get(b,"durable"),"persisted SELECT deny must override bootstrap role",
        "SECURITY.ACCESS_DENIED","key_value_collection_right_required");
    Refused(transaction.Put(b,"durable","not-authorized"),"upsert requires actual SELECT+INSERT+UPDATE authority",
        "SECURITY.ACCESS_DENIED","key_value_collection_right_required");
    Refused(transaction.Atomic(b,{{"set","durable","not-authorized"}}),"RMW cannot bypass persisted deny",
        "SECURITY.ACCESS_DENIED","key_value_collection_right_required");
    auto forged = transaction.RequestFor<api::EngineKeyValueGetRequest>(b);
    forged.key = "durable"; forged.physical_proof.proof_supplied = true;
    forged.physical_proof.exact_key_index_proof = forged.physical_proof.ttl_visibility_proof = true;
    Refused(api::EngineKeyValueGet(forged),"legacy proof booleans cannot override actual deny",
        "SECURITY.ACCESS_DENIED","key_value_collection_right_required");
    const auto forge_projected_grants = [&](auto& request) {
      auto& projected = request.context.authorization_context;
      const auto deny = std::find_if(projected.grants.begin(),projected.grants.end(),[&](const auto& grant) {
        return grant.deny && grant.subject_uuid == request.context.principal_uuid &&
            grant.target_uuid == b && grant.right == "SELECT";
      });
      Check(deny != projected.grants.end(),"negative control requires the actual committed target deny in its issued projection");
      // Deliberately corrupted DATA, never persisted or admitted authority.
      // Keep the complete identity/epoch/receipt tuple and effective subjects
      // unchanged, but erase denies and relabel the existing grant identity.
      const auto source = *deny;
      projected.grants.clear();
      for (const char* right : {"SELECT","INSERT","UPDATE"}) {
        auto claim = source; claim.right = right; claim.deny = false;
        projected.grants.push_back(std::move(claim));
      }
      for (const char* right : {"SELECT","INSERT","UPDATE"}) {
        const auto claim = api::EvaluateMaterializedAuthorization(request.context,projected,right,b);
        Check(claim.authorized && !claim.denied && claim.diagnostics.empty(),
            "corrupted projection must actually claim broad authority to exercise the private-source regression");
      }
    };
    auto forged_read = transaction.RequestFor<api::EngineKeyValueGetRequest>(b);
    forged_read.key = "durable";
    const auto issuing_receipt = forged_read.statement_receipt();
    forge_projected_grants(forged_read);
    Refused(api::EngineKeyValueGet(forged_read),"altered grant arrays cannot replace the actual issued deny",
        "SECURITY.ACCESS_DENIED","key_value_collection_right_required");
    auto forged_write = transaction.RequestFor<api::EngineKeyValuePutRequest>(b);
    forged_write.key = "durable"; forged_write.value = "not-authorized";
    forge_projected_grants(forged_write);
    Refused(api::EngineKeyValuePut(forged_write),"forged projected SELECT/INSERT/UPDATE grants cannot authorize upsert",
        "SECURITY.ACCESS_DENIED","key_value_collection_right_required");
    auto forged_atomic = transaction.RequestFor<api::EngineKeyValueAtomicProgramRequest>(b);
    forged_atomic.steps = {{"set","durable","not-authorized"}};
    forge_projected_grants(forged_atomic);
    Refused(api::EngineKeyValueAtomicProgram(forged_atomic),"forged grant arrays cannot authorize read-modify-write",
        "SECURITY.ACCESS_DENIED","key_value_collection_right_required");
    api::EngineRequestContext actual;
    sb_engine_result_t diagnostics = nullptr;
    const auto copied = scratchbird::server_engine_bridge::CopyStatementContextEngineContextV1(
        issuing_receipt,&actual,&diagnostics);
    t::FixtureEngineSession::Check(copied,diagnostics,"inspect unchanged actual receipt after grant forgery");
    Check(actual.statement_receipt_uuid == forged_read.context.statement_receipt_uuid &&
        actual.statement_uuid == forged_read.context.statement_uuid &&
        std::any_of(actual.authorization_context.grants.begin(),actual.authorization_context.grants.end(),
          [&](const auto& grant) { return grant.deny && grant.right == "SELECT" && grant.target_uuid == b; }),
        "same live private receipt must retain its actual deny despite mutated projected arrays");
    Row(transaction.Get(a,"durable"),0,"durable","committed");
    transaction.Finish();
  }
  directory.Cleanup();
}
} // namespace
int main(int argc, char** argv) {
  try {
    t::ConfigureOwnedBootstrapFixtureMemory("native-key-value-collection");
    if (argc == 8 && std::string_view(argv[1]) == "--reopen") {
      db::DatabaseCreateConfig create; create.path = argv[2];
      create.database_uuid = {scratchbird::core::platform::UuidKind::database,ArgumentUuid(std::string_view(argv[3]))};
      create.filespace_uuid = {scratchbird::core::platform::UuidKind::filespace,ArgumentUuid(std::string_view(argv[4]))};
      Child(create,ArgumentUuid(std::string_view(argv[5])),ArgumentUuid(std::string_view(argv[6])),
          ArgumentUuid(std::string_view(argv[7])));
    } else if(argc==2&&std::string_view(argv[1])=="--text-destination") {
      TextDestinationLive();
    } else {
      Check(argc == 1,"unexpected native KV test arguments");
      Run(std::filesystem::absolute(argv[0]).string());
    }
    std::cout << checks << " checks PASS\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
