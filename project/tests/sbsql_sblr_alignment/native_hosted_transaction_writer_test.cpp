// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_TRANSACTION_INVENTORY_WRITER
// The in-place transaction writer through the hosted database custody, on a
// database written by the production create, at every page size: the server
// takes route ownership of the file, the custody adopts the writer, and every
// read, write and sync of the writer goes through the custody's retained
// operation (NativeHostedCarefulWriteSink). Checked:
//   - the root of the transaction-state chain comes from the custody's own
//     fixed-root read;
//   - while that read lease is live the owned writer refuses the writer's
//     first read: nothing is written and the writer stays usable;
//   - restart, BEGIN, COMMIT, ROLLBACK through the custody, each with its
//     writes and a barrier before every dependent write, counted by the
//     custody's own physical-effect counters;
//   - after the custody is released, an independent read-only open sees
//     exactly those states, the production fixed-root lease reads the file and
//     admits its stored horizons;
//   - a second hosted open: its restart marks the transaction left in
//     progress dead, and the next BEGIN takes the stored Next.
// The writer itself and every crash, tear and I/O failure are swept on the
// device directly by native_transaction_inventory_writer_test.
#include "../../src/server/native_hosted_careful_write_sink.hpp"
#include "native_database_create.hpp"
#include "native_catalog_relation_lease_reader.hpp"
#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"
#include "native_fixed_root_memory_lease.hpp"
#include "transaction_inventory_writer.hpp"
#include "transaction_inventory_reader.hpp"
#include "transaction_inventory_page.hpp"
#include "transaction_horizon_projection.hpp"
#include "uuid.hpp"
#include "../support/owned_temp_directory.hpp"

#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace server = scratchbird::server;
namespace db = scratchbird::storage::database;
namespace d = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
namespace mga = scratchbird::transaction::mga;
namespace m = scratchbird::core::memory;
namespace u = scratchbird::core::uuid;
using namespace scratchbird::core::platform;
using Host = server::NativeHostedDatabaseCustody;
using pg::TipStateCode;
using WE = pg::TransactionWriterError;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
Uuid Fixed(unsigned n) {
  Uuid id;
  id.bytes[0] = 1;
  id.bytes[6] = 0x70;
  id.bytes[8] = 0x80;
  id.bytes[14] = static_cast<byte>(n >> 8);
  id.bytes[15] = static_cast<byte>(n);
  return id;
}
Uuid V7(u64 ms, unsigned salt) {
  Uuid id;
  for (int i = 0; i < 6; ++i) id.bytes[i] = static_cast<byte>(ms >> (8 * (5 - i)));
  id.bytes[6] = 0x70;
  id.bytes[7] = static_cast<byte>(salt);
  id.bytes[8] = 0x80;
  id.bytes[9] = 0x6c;
  id.bytes[15] = static_cast<byte>(salt);
  return id;
}
constexpr const char* kFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

// A node-issued memory grant of exactly `bytes` bound to `database`.
struct Grant {
  m::MemoryManager manager;
  m::HierarchicalMemoryBudgetLedger ledger{3, 5};
  db::NativeStorageMemoryBinding binding;
  db::NativeStorageMemory memory;
  static auto Policy() {
    auto p = m::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes = 1ull << 30;
    p.per_context_limit_bytes = 1ull << 30;
    return p;
  }
  Grant(const Uuid& database, unsigned owner, u64 bytes)
      : manager(Policy()), binding{database, Fixed(owner), Fixed(owner + 1), Fixed(owner + 2)} {
    m::ReservationBackedMemoryResourceRequest r;
    r.memory_manager = &manager;
    r.reservation_ledger = &ledger;
    r.consumer_kind = m::ReservationBackedMemoryConsumerKind::background_maintenance;
    r.category = m::MemoryCategory::page_buffer;
    r.requested_bytes = bytes;
    r.memory_class = "page_buffer";
    r.route_label = "storage.transaction_inventory.hosted";
    r.purpose = "hosted transaction writer";
    r.binary_operation_uuid = binding.operation_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::database] = binding.database_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::owner] = binding.owner_uuid.bytes;
    r.binary_ownership[m::MemoryBinaryScopeKind::context] = binding.context_uuid.bytes;
    r.scope_chain = {{m::HierarchicalMemoryScopeKind::process, {}, Fixed(64).bytes},
                     {m::HierarchicalMemoryScopeKind::database, {}, binding.database_uuid.bytes}};
    r.provenance.source = m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    r.provenance.source_label = "hosted transaction writer";
    for (const auto& scope : r.scope_chain) {
      m::HierarchicalMemoryBudget b;
      b.scope = scope;
      b.hard_limit_bytes = bytes;
      b.provenance = r.provenance;
      Check(ledger.SetBudget(b).ok(), "parent budget");
    }
    auto grant = m::AcquireReservationBackedMemoryResource(r);
    Check(grant.ok(), "node-issued grant");
    auto adopted = db::AdoptNativeStorageMemory(binding, grant.resource);
    Check(adopted.ok(), "grant adoption");
    memory = std::move(adopted.memory);
  }
};

struct Created {
  scratchbird::tests::OwnedTempDirectory dir;
  std::string path;
  u32 size = 0;
  Uuid database, primary, profile;
  u64 creator_commit_ms = 0;
  explicit Created(unsigned index) {
    const auto& p = d::kCanonicalFilespacePageProfiles[index];
    size = p.page_size_bytes;
    profile = p.uuid;
    db::NativeDatabaseCreateRequest r;
    r.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
    r.database_name = "orders";
    r.database_default_language = "en";
    r.page_size_profile_uuid = p.uuid;
    r.page_size_bytes = p.page_size_bytes;
    r.total_pages = p.page_size_bytes == 8192 ? 1024 : 512;
    r.creation_utc_millis = 1789357072000ULL;
    r.engine_uuid = Fixed(4);
    r.datatype_cohort = {{UuidKind::object, scratchbird::core::datatypes::kDatatypeCohortV11}, 11, 11};
    r.bootstrap_principal_name = "BootstrapAdmin";
    r.bootstrap_credential_fingerprint = kFingerprint;
    r.service_principal_name = "NodeService";
    r.service_credential_fingerprint = kFingerprint;
    r.maximum_retained_image_bytes = db::NativeHostedSourceBackingBytes(p.page_size_bytes);
    path = (dir.path() / "hosted").string();
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::create_new,d::DatabaseFileOwnership::native_data_file).ok(), "node device");
    u::StandaloneUuidV7Issuer issuer({Fixed(1), Fixed(10)}, {{}, 0, 1000});
    auto made = db::CreateNativeDatabaseOnOpenDevice(device, r, issuer);
    Check(made.ok(), "the production create writes a complete native database");
    database = made.receipt->database_uuid;
    primary = made.receipt->filespace_uuid;
    Check(device.Close().ok(), "close the created file");
  }
};

// The custody of one hosted open, with its operation.
struct Hosted {
  Grant persistent, transient, reading;
  server::DatabaseOwnershipResult route;
  std::shared_ptr<Host> custody;
  std::vector<std::unique_ptr<d::FileDevice>> secondary;
  Host::OperationResult operation;
  explicit Hosted(const Created& c)
      : persistent(c.database, 100, 64 * 1024), transient(c.database, 110, 32u << 20), reading(c.database, 120, 64u << 20) {
    server::DatabaseOwnershipRequest ownership;
    ownership.database_path = c.path;
    ownership.storage_access = d::RouteStorageAccess::read_write;
    route = server::AcquireDatabaseOwnership(ownership);
    Check(route.acquired && route.lock, "the server owns the database file");
    auto begun = Host::Begin(route.lock);
    Check(begun.ok(), "the custody retains the route");
    custody = begun.custody;
    const u64 budget = 512 * u64{c.size};
    const auto adopted = Host::AdoptWriter(custody, c.database, c.primary, secondary, {64 * 1024, {budget, 32u << 20}},
                                           persistent.memory, persistent.binding, transient.memory,
                                           transient.binding);
    if (!adopted.ok()) std::cerr << "adopt error=" << int(adopted.error) << '\n';
    Check(adopted.ok(), "the custody adopts the writer");
    operation = Host::TryBeginOperation(custody, Fixed(234));
    Check(operation.ok(), "a custody operation");
  }
  Host::Operation& op() { return operation.operation; }
};

pg::TransactionHeaderRecord Header(u64 ms, unsigned salt, bool read_only = false) {
  pg::TransactionHeaderRecord r;
  r.transaction_uuid = V7(ms, salt);
  r.read_only = read_only;
  return r;
}

void Run(unsigned profile) {
  const Created c(profile);
  d::FilespaceRootReference root;
  u64 next = 0;
  {
    Hosted h(c);
    {
      auto read = h.op().ReadFixedRootWithMemory({256ull * c.size, 32u << 20}, h.reading.memory, h.reading.binding);
      Check(read.ok() && read.lease.source().Root(4), "the custody's fixed-root read names the state chain");
      root = *read.lease.source().Root(4);
      server::NativeHostedCarefulWriteSink sink(h.op(), c.primary, c.profile);
      pg::TransactionWriterBinding binding;
      binding.database_uuid = c.database;
      binding.state_root = root;
      pg::TransactionInventoryWriter w(binding);
      const auto before = h.op().effects();
      const auto refused = w.Restart(sink);
      Check(refused.error == WE::io_failure && sink.last_writer_error() == db::NativeWriterError::read_handle_live &&
                !w.failed() && !w.restarted() && h.op().effects().write_attempts == before.write_attempts,
            "while the custody's read lease is live its operation refuses the writer, which writes nothing");
    }
    server::NativeHostedCarefulWriteSink sink(h.op(), c.primary, c.profile);
    pg::TransactionWriterBinding binding;
    binding.database_uuid = c.database;
    binding.state_root = root;
    pg::TransactionInventoryWriter w(binding);
    const auto restart = w.Restart(sink);
    Check(restart.ok() && restart.marked_dead == 0 && restart.next_transaction == 2, "restart through the custody");
    const auto start = h.op().effects();
    const u64 t = 1789357100000ULL;
    const auto b2 = w.Begin(sink, Header(t + 2000, 2));
    Check(b2.ok() && b2.transaction_order == 2u && b2.writes == 3 && b2.syncs == 3,
          "BEGIN through the custody: three writes, a barrier before each dependent one and a final one");
    // Commit times are made later than the creator's by the writer's clamp;
    // the device test checks the clock arithmetic.
    Check(w.Commit(sink, 2, 1).ok(), "COMMIT through the custody");
    Check(w.Begin(sink, Header(t + 3000, 3, true)).ok() && w.Rollback(sink, 3, 1).ok(), "read-only BEGIN and ROLLBACK");
    Check(w.Begin(sink, Header(t + 4000, 4)).ok(), "BEGIN 4, left in progress");
    const auto end = h.op().effects();
    Check(end.write_attempts - start.write_attempts == 13 && end.write_bytes - start.write_bytes == 13 * 512 &&
              end.successful_syncs - start.successful_syncs == 13 && !end.uncertain_write,
          "the custody counts exactly the writer's 13 unit writes and 13 syncs");
    next = 5;
  }
  // Independent: the file as an unrelated read-only process sees it.
  const auto check_file = [&](TipStateCode four, const char* label) {
    d::FileDevice device;
    Check(device.Open(c.path, d::FileOpenMode::open_existing_read_only).ok(), "independent read-only open");
    {
      const std::vector<d::NativeFilespaceDevice> files{{c.primary, c.profile, &device}};
      std::vector<byte> backing(64u << 20);
      auto fixed = db::AcquireNativeFixedRootBackedLease(c.database, files, c.primary, {256ull * c.size, backing.size()},
                                                         backing);
      Check(fixed.ok(), "the production fixed-root lease reads the hosted writer's file");
      const auto& lease = *fixed.lease;
      const auto& inventory = lease.inventory();
      Check(inventory.next_local_transaction_id == next && inventory.entries.size() == 4, label);
      const auto state = [&](u64 n) {
        for (const auto& e : inventory.entries)
          if (e.identity.local_id.value == n) return e.state;
        return mga::TransactionState::none;
      };
      Check(state(1) == mga::TransactionState::committed && state(2) == mga::TransactionState::committed &&
                state(3) == mga::TransactionState::rolled_back &&
                state(4) == (four == TipStateCode::active ? mga::TransactionState::active
                                                          : mga::TransactionState::failed_terminal),
            label);
      const pg::NativeTransactionInventoryConstView view{inventory.next_local_transaction_id,
                                                        inventory.next_commit_sequence, inventory.entries};
      mga::LocalTransactionHorizons projected;
      Check(!mga::detail::ProjectValidatedLocalHorizons(view, {}, projected) &&
                pg::StoredTransactionHorizonsAdmissible(lease.counters(), inventory.next_local_transaction_id, projected),
            "the reader admits the stored horizons");
      fixed.lease.Reset();
    }
    Check(device.Close().ok(), "close");
  };
  check_file(TipStateCode::active, "an independent reader sees committed 1 and 2, rolled-back 3, active 4");
  {
    // A second hosted open: restart marks 4 dead; the next BEGIN takes Next.
    Hosted h(c);
    server::NativeHostedCarefulWriteSink sink(h.op(), c.primary, c.profile);
    pg::TransactionWriterBinding binding;
    binding.database_uuid = c.database;
    binding.state_root = root;
    pg::TransactionInventoryWriter w(binding);
    const auto restart = w.Restart(sink);
    Check(restart.ok() && restart.marked_dead == 1 && restart.writes == 1, "restart marks 4 dead");
  }
  check_file(TipStateCode::dead, "after the second open's restart, 4 is dead");
  {
    Hosted h(c);
    server::NativeHostedCarefulWriteSink sink(h.op(), c.primary, c.profile);
    pg::TransactionWriterBinding binding;
    binding.database_uuid = c.database;
    binding.state_root = root;
    pg::TransactionInventoryWriter w(binding);
    Check(w.Restart(sink).ok(), "restart");
    const auto b5 = w.Begin(sink, Header(1789357105000ULL, 5));
    Check(b5.ok() && b5.transaction_order == 5u && w.Commit(sink, 5, 1).ok(), "the next BEGIN takes 5 and commits");
  }
  std::cout << "hosted transaction writer page_size=" << c.size << " ok\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    for (unsigned profile = 0; profile < d::kCanonicalFilespacePageProfiles.size(); ++profile)
      if (argc < 2 || std::to_string(profile) == argv[1]) Run(profile);
    std::cout << "PASS native hosted transaction writer checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native hosted transaction writer: " << e.what() << " checks=" << checks << '\n';
    return 1;
  }
}
