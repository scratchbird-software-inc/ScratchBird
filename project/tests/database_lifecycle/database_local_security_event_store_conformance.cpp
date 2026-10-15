// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "database_lifecycle.hpp"
#include "security_lifecycle_event_codec.hpp"
#include "database_local_private_relation_locator.hpp"
#include "security/database_local_security_event_store.hpp"
#include "security/database_local_security_retained_snapshot.hpp"
#include "transaction_snapshot.hpp"
#include "database_format.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "page_header.hpp"
#include "physical_mga_cow_store.hpp"
#include "row_data_page.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace source_allocation_fault {
thread_local long budget = -1;
thread_local bool hit = false, count = false;
thread_local std::size_t attempts = 0;
thread_local long revoke_after = -1;
thread_local bool revoke_fired = false;
thread_local scratchbird::core::platform::TypedUuid revoke_snapshot;
}
void* operator new(std::size_t n) {
  using namespace source_allocation_fault;
  if (count) ++attempts;
  if (revoke_after == 0) {
    revoke_after = -1; revoke_fired = true;
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(revoke_snapshot);
  } else if (revoke_after > 0) --revoke_after;
  if (budget == 0) { budget = -1; hit = true; throw std::bad_alloc(); }
  if (budget > 0) --budget;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#include "../support/aligned_allocation_fault_bridge.hpp"

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace event_codec = db::security_event_codec;
using scratchbird::tests::FixtureUuidLiteral;
namespace disk = scratchbird::storage::disk;
namespace core_hash = scratchbird::core::hash;
namespace mga = scratchbird::transaction::mga;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::UuidKind;

std::string test_executable;

constexpr std::string_view kCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=4ce03aa5a5657aaf221192635ed9c63a"
    "cdb76d78a0994ec6e6ab55286e29e6a5";
constexpr auto kAlicePrincipal = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007aa");
constexpr auto kSysarchRole = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007a1");
constexpr auto kPublicGroup = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007a2");
constexpr auto kRoleMembership = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007b1");
constexpr auto kGroupMembership = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007b2");
constexpr auto kConnectGrant = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007c1");
constexpr auto kConnectDeny = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007d1");
constexpr auto kRowPolicy = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007e1");

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

struct TempDirectory {
  std::filesystem::path path;

  ~TempDirectory() {
    if (path.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TempDirectory MakeTempDirectory() {
  std::string pattern = (std::filesystem::temp_directory_path() /
      "sb_database_local_security.XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  Require(created != nullptr, "private security test temporary directory failed");
  return {std::filesystem::path(created)};
}

struct Fixture {
  std::filesystem::path database_path;
  api::EngineRequestContext context;
  std::uint64_t bootstrap_generation = 0;
};

Fixture CreateFixture(const std::filesystem::path& path,
                      std::uint64_t identity_time,
                      std::uint32_t page_size = 16384) {
  const auto database_uuid =
      uuid::GenerateDurableEngineIdentityV7(UuidKind::database, identity_time);
  const auto filespace_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::filespace, identity_time + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "private security fixture identity generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = page_size;
  create.creation_unix_epoch_millis = identity_time + 2;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.bootstrap_principal_name = "bootstrap_admin";
  create.bootstrap_credential_fingerprint = std::string(kCredentialFingerprint);
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "private security fixture create failed");

  db::DatabaseOpenConfig open;
  open.path = path.string();
  const auto opened = db::OpenDatabaseFile(open);
  Require(opened.ok(), "private security fixture first open failed");
  Require(db::MarkDatabaseCleanShutdown(path.string()).ok(),
          "private security fixture clean marker failed");

  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
  Require(bootstrap.ok() && bootstrap.state.present &&
              bootstrap.state.security_context_generation != 0,
          "private security fixture bootstrap catalog unavailable");

  Fixture fixture;
  fixture.database_path = path;
  fixture.context.trust_mode = api::EngineTrustMode::embedded_in_process;
  fixture.context.database_path = path.string();
  fixture.context.database_uuid = database_uuid.value.value;
  fixture.context.database_page_size_bytes = create.page_size;
  fixture.context.principal_uuid = bootstrap.state.principal_uuid.value;
  fixture.context.security_context_present = true;
  fixture.context.trace_tags.emplace_back(
      api::kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1);
  fixture.bootstrap_generation = bootstrap.state.security_context_generation;
  return fixture;
}

api::EngineRequestContext BeginTransaction(
    const api::EngineRequestContext& base,
    std::uint64_t identity_time,
    bool stable = false,
    bool read_only = false) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(base.database_path);
  Require(loaded.ok(), "private security transaction inventory load failed");
  const auto transaction_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::transaction, identity_time);
  Require(transaction_uuid.ok(),
          "private security transaction identity generation failed");
  auto begun = read_only
      ? mga::BeginLocalReadOnlyTransaction(loaded.inventory, transaction_uuid.value, identity_time + 1)
      : mga::BeginLocalTransaction(loaded.inventory, transaction_uuid.value, identity_time + 1);
  Require(begun.ok(), "private security transaction begin failed");
  // Admit the selected isolation in the candidate BEFORE durable publication,
  // never rewrite an already-issued transaction's immutable isolation flags.
  for (auto& entry : begun.inventory.entries)
    if (entry.identity.local_id.value == begun.entry.identity.local_id.value)
      entry.stable_snapshot = stable;
  Require(db::PersistLocalTransactionInventoryToDatabase(
              base.database_path, begun.inventory)
              .ok(),
          "private security active transaction persistence failed");

  auto context = base;
  context.local_transaction_id = begun.entry.identity.local_id.value;
  context.transaction_uuid = begun.entry.identity.transaction_uuid.value;
  context.snapshot_visible_through_local_transaction_id =
      begun.entry.begin_visible_through_local_transaction_id;
  return context;
}

void Finalize(const api::EngineRequestContext& context,
              db::PhysicalMgaCowFinalizeDecision decision,
              std::uint64_t final_time) {
  db::PhysicalMgaCowFinalizeRequest finalize;
  finalize.database_path = context.database_path;
  finalize.transaction = {mga::MakeLocalTransactionId(context.local_transaction_id),
      {scratchbird::core::platform::UuidKind::transaction, context.transaction_uuid},
      mga::TransactionScope::local_node};
  finalize.decision = decision;
  finalize.final_unix_epoch_millis = final_time;
  const auto finalized = db::FinalizePhysicalMgaCowTransaction(finalize);
  Require(finalized.ok(), "private security transaction finality failed");
}

api::EngineUuid AuditUuid(std::uint64_t generation) {
  const auto generated = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::object, 1788100000000ull + generation);
  Require(generated.ok(), "private security audit identity generation failed");
  return generated.value.value;
}

std::vector<std::string> Batch(std::string authority,
                               const api::EngineRequestContext& context,
                               std::uint64_t generation,
                               const api::EngineUuid& target) {
  return {std::move(authority),
      event_codec::Encode("AUDIT",context.local_transaction_id,
          {AuditUuid(generation), std::string("746573742e707269766174655f6c6966656379636c65"),
           context.principal_uuid,target,std::string("success"),std::string("7265646163746564"),
           std::to_string(generation),api::EngineUuid{},std::string{}}),
      event_codec::Encode("CACHE_INVALIDATE",context.local_transaction_id,
          {std::string("746573742e707269766174655f6c6966656379636c65"),target,std::to_string(generation)})};
}

std::vector<std::string> SingleRoleBatch(
    const api::EngineRequestContext& context,
    std::uint64_t generation,
    api::EngineUuid role_uuid =
        FixtureUuidLiteral("019e108d-1700-7000-8000-000000000798")) {
  return Batch(event_codec::Encode("ROLE", context.local_transaction_id, {role_uuid, std::string("746573745f726f6c65"), context.principal_uuid, std::string("active"), std::to_string(generation), std::string("0")}),
               context, generation, role_uuid);
}

std::size_t CountEventKind(const std::vector<std::string>& events,
                           std::string_view kind) {
  return static_cast<std::size_t>(
      std::count_if(events.begin(), events.end(), [&](const std::string& event) {
        return event_codec::Decode(event)[1] == kind;
      }));
}

void CorruptSecurityPage(const std::filesystem::path& path,
                         std::uint64_t page_number,
                         std::uint32_t page_size) {
  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "private security corruption fixture open failed");
  const auto offset = disk::CheckDevicePageOffset(
      page_size, page_number, disk::kPageHeaderSerializedBytes + 40);
  Require(offset.ok(), "private security corruption offset invalid");
  std::uint8_t value = 0;
  Require(device.ReadAt(offset.offset, &value, 1).ok(),
          "private security corruption fixture read failed");
  value ^= 0x5au;
  Require(device.WriteAt(offset.offset, &value, 1).ok() && device.Sync().ok(),
          "private security corruption fixture write failed");
  Require(device.Close().ok(),
          "private security corruption fixture close failed");
}

std::uint64_t AppendUnallocatedPages(const std::filesystem::path& path,
                                     std::uint32_t page_size,
                                     std::uint64_t count) {
  Require(count != 0 &&
              count <= std::numeric_limits<std::size_t>::max() / page_size,
          "private security unallocated page count invalid");
  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "private security unallocated fixture open failed");
  const auto size = device.Size();
  Require(size.ok() && size.size_bytes % page_size == 0,
          "private security unallocated fixture size invalid");
  const std::uint64_t first_page_number = size.size_bytes / page_size;
  std::vector<std::uint8_t> zero_pages(
      static_cast<std::size_t>(count) * page_size, 0);
  Require(device.WriteAt(size.size_bytes, zero_pages.data(), zero_pages.size()).ok() &&
              device.Sync().ok(),
          "private security unallocated fixture write failed");
  Require(device.Close().ok(),
          "private security unallocated fixture close failed");
  return first_page_number + count - 1;
}

db::DatabaseLocalPrivateSecurityLocatorInspectResultV1 InspectLocator(
    const std::filesystem::path& path) {
  disk::FileDevice device;
  Require(device.Open(path.string(),
                      disk::FileOpenMode::open_existing_read_only)
              .ok(),
          "private security locator inspection open failed");
  disk::SerializedDatabaseHeader serialized{};
  Require(device.ReadAt(0, serialized.data(), serialized.size()).ok(),
          "private security locator inspection header read failed");
  const auto header = disk::ParseDatabaseHeader(serialized);
  Require(header.ok(), "private security locator inspection header invalid");
  db::DatabaseLocalPrivateSecurityLocatorInspectRequestV1 request;
  request.device = &device;
  request.marker_bytes =
      header.header.database_local_private_relation_locator_marker;
  request.expected_database_uuid.kind = UuidKind::database;
  request.expected_database_uuid.value = header.header.database_uuid;
  request.page_size = header.header.page_size;
  const auto inspected =
      db::InspectDatabaseLocalPrivateSecurityLocatorV1(request);
  Require(device.Close().ok(),
          "private security locator inspection close failed");
  return inspected;
}

void FlipDatabaseByte(const std::filesystem::path& path,
                      std::uint64_t offset,
                      std::uint8_t mask = 0x5au) {
  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "private security byte mutation open failed");
  std::uint8_t value = 0;
  Require(device.ReadAt(offset, &value, 1).ok(),
          "private security byte mutation read failed");
  value ^= mask;
  Require(device.WriteAt(offset, &value, 1).ok() && device.Sync().ok(),
          "private security byte mutation write failed");
  Require(device.Close().ok(),
          "private security byte mutation close failed");
}

std::uint64_t LocatorBodyAbsoluteOffset(std::uint32_t page_size,
                                        std::uint32_t body_offset,
                                        std::uint32_t field_offset) {
  const auto checked = disk::CheckDevicePageOffset(
      page_size, db::kDatabaseLocalPrivateSecurityRootPageNumberV1,
      disk::kPageHeaderSerializedBytes + body_offset + field_offset);
  Require(checked.ok(), "private security locator mutation offset invalid");
  return checked.offset;
}

void ConvertFreshEmptyLocatorToExactLegacy(
    const std::filesystem::path& path,
    std::uint32_t page_size) {
  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "private security legacy conversion open failed");
  disk::SerializedDatabaseHeader serialized_header{};
  Require(device.ReadAt(0, serialized_header.data(), serialized_header.size())
              .ok(),
          "private security legacy conversion header read failed");
  const auto parsed_header = disk::ParseDatabaseHeader(serialized_header);
  Require(parsed_header.ok() && parsed_header.header.page_size == page_size,
          "private security legacy conversion header invalid");
  auto legacy_header = parsed_header.header;
  legacy_header.database_local_private_relation_locator_marker.fill(0);
  const auto encoded_header = disk::SerializeDatabaseHeader(legacy_header);
  Require(encoded_header.ok() &&
              device.WriteAt(0, encoded_header.serialized.data(),
                             encoded_header.serialized.size())
                  .ok(),
          "private security legacy conversion header write failed");

  const auto page_offset = disk::CheckDevicePageOffset(
      page_size, db::kDatabaseLocalPrivateSecurityRootPageNumberV1);
  Require(page_offset.ok(),
          "private security legacy conversion page offset invalid");
  disk::SerializedPageHeader serialized_page{};
  Require(device.ReadAt(page_offset.offset, serialized_page.data(),
                        serialized_page.size())
              .ok(),
          "private security legacy conversion page header read failed");
  const auto parsed_page = disk::ParsePageHeader(serialized_page);
  Require(parsed_page.ok(),
          "private security legacy conversion page header invalid");
  auto page_header = parsed_page.header;
  page_header.page_type = disk::PageType::bootstrap_reserved;
  page_header.flags = 0;
  const auto encoded_page = disk::SerializePageHeader(page_header);
  Require(encoded_page.ok() &&
              device.WriteAt(page_offset.offset, encoded_page.serialized.data(),
                             encoded_page.serialized.size())
                  .ok(),
          "private security legacy conversion page header write failed");
  std::vector<std::uint8_t> zero_body(
      page_size - disk::kPageHeaderSerializedBytes, 0);
  Require(device.WriteAt(page_offset.offset + disk::kPageHeaderSerializedBytes,
                         zero_body.data(), zero_body.size())
              .ok() &&
              device.Sync().ok(),
          "private security legacy conversion page body write failed");
  Require(device.Close().ok(),
          "private security legacy conversion close failed");
}

std::vector<std::uint64_t> ConvertCurrentSecurityChainToLegacy(
    const std::filesystem::path& path,
    std::uint32_t page_size,
    bool substitute_authenticated_payload) {
  const auto inspected = InspectLocator(path);
  Require(inspected.ok(),
          "private security current-to-legacy locator inspection failed");
  const auto& locator =
      inspected.locators[inspected.anchored_locator_slot];
  Require(locator.chain_page_count >= 2 && locator.head_page_number != 0,
          "private security current-to-legacy multi-page chain required");

  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "private security current-to-legacy open failed");
  std::vector<std::uint64_t> page_numbers;
  std::uint64_t page_number = locator.head_page_number;
  while (page_number != 0) {
    Require(page_numbers.size() < locator.chain_page_count &&
                std::find(page_numbers.begin(), page_numbers.end(),
                          page_number) == page_numbers.end(),
            "private security current-to-legacy chain cycle invalid");
    page_numbers.push_back(page_number);
    const auto body_offset = disk::CheckDevicePageOffset(
        page_size, page_number, disk::kPageHeaderSerializedBytes);
    Require(body_offset.ok(),
            "private security current-to-legacy body offset invalid");
    std::vector<byte> body_bytes(
        page_size - disk::kPageHeaderSerializedBytes, 0);
    Require(device
                .ReadAt(body_offset.offset, body_bytes.data(),
                        body_bytes.size())
                .ok(),
            "private security current-to-legacy body read failed");
    const auto parsed = page::ParseRowDataPageBody(body_bytes, page_number);
    Require(parsed.ok() && parsed.body.rows.size() == 1 &&
                parsed.body.rows[0].cells.size() == 1,
            "private security current-to-legacy row shape invalid");
    auto body = parsed.body;
    db::DatabaseLocalSecurityBatchEnvelopeV1 carrier;
    std::string refusal;
    Require(db::DecodeDatabaseLocalSecurityBatchEnvelopeV1(
                body.rows[0].cells[0].value.payload, &carrier, &refusal),
            "private security current-to-legacy current batch invalid");
    const std::uint64_t predecessor_page_number =
        carrier.predecessor_page_number;
    if (substitute_authenticated_payload && page_numbers.size() == 1) {
      auto audit = event_codec::Decode(carrier.events[1]);
      Require(audit.size() == 12, "private security payload substitution audit missing");
      audit.fields[5] = FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007ff");
      std::vector<event_codec::Field> fields(audit.fields.begin()+3,audit.fields.end());
      carrier.events[1] = event_codec::Encode("AUDIT", carrier.creator_local_transaction_id, std::move(fields));
    }
    const std::string payload = event_codec::Frame(carrier.events);
    const std::vector<byte> payload_bytes(payload.begin(), payload.end());
    const auto digest = core_hash::ComputeSha256Digest(payload_bytes);
    Require(digest.ok(),
            "private security current-to-legacy payload digest failed");
    const auto& current = body.rows[0].cells[0].value.payload;
    Require(current.size() >=
                db::kDatabaseLocalSecurityBatchEnvelopeBytesV1,
            "private security current-to-legacy envelope too short");
    std::vector<byte> legacy;
    legacy.reserve(140 + payload_bytes.size());
    legacy.insert(legacy.end(), current.begin(), current.begin() + 108);
    scratchbird::core::platform::StoreLittle16(legacy.data() + 10, 0);
    scratchbird::core::platform::StoreLittle32(
        legacy.data() + 104,
        static_cast<std::uint32_t>(payload_bytes.size()));
    legacy.insert(legacy.end(), digest.digest.begin(), digest.digest.end());
    legacy.insert(legacy.end(), payload_bytes.begin(), payload_bytes.end());
    body.rows[0].cells[0].value.payload = std::move(legacy);
    body.next_page_number = 0;
    const auto built = page::BuildRowDataPageBody(body, page_size);
    Require(built.ok() &&
                device
                    .WriteAt(body_offset.offset, built.serialized.data(),
                             built.serialized.size())
                    .ok(),
            "private security current-to-legacy page write failed");
    page_number = predecessor_page_number;
  }
  Require(page_numbers.size() == locator.chain_page_count &&
              page_numbers.back() == locator.tail_page_number &&
              device.Sync().ok(),
          "private security current-to-legacy chain extent invalid");
  Require(device.Close().ok(),
          "private security current-to-legacy close failed");
  ConvertFreshEmptyLocatorToExactLegacy(path, page_size);
  return page_numbers;
}
void VerifyBorrowedReadOwnership(const Fixture& fixture) {
  const auto inspected = InspectLocator(fixture.database_path);
  Require(inspected.ok(), "borrowed read locator unavailable");
  const auto& locator = inspected.locators[inspected.anchored_locator_slot];
  scratchbird::core::platform::TypedUuid relation;
  relation.kind = UuidKind::object;
  relation.value = locator.relation_uuid;
  const std::string database_path = fixture.database_path.string();
  disk::FileDevice owner;
  Require(owner.Open(fixture.database_path.string(),
                     disk::FileOpenMode::open_existing_read_only).ok(),
          "borrowed read owner open failed");
  const auto require_exclusion = [&]() {
    Require(owner.is_open() && owner.read_only() &&
                owner.path() == fixture.database_path.string(),
            "borrowed read changed owner device");
    for (const auto mode : {disk::FileOpenMode::open_existing,
                            disk::FileOpenMode::open_existing_read_only}) {
      disk::FileDevice competing;
      const auto refused = competing.Open(fixture.database_path.string(), mode);
      Require(!refused.ok() && !competing.is_open() &&
                  refused.diagnostic.diagnostic_code == "SB-STORAGE-DISK-OWNER-LOCK-HELD",
              "borrowed read allowed a competing local open");
    }
    const auto child = ::fork();
    Require(child >= 0, "borrowed read competing process fork failed");
    if (child == 0) {
      // A competing server is a fresh executable, not a fork using inherited
      // engine mutexes. Do only async-signal-safe work before exec.
      ::execl(test_executable.c_str(), test_executable.c_str(),
              "--probe-owner", database_path.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    int status = 0;
    Require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "borrowed read allowed a competing process open");
  };
  require_exclusion();
  const auto read = db::ReadPhysicalMgaCowRowsFromOpenDevice(
      owner, relation, locator.head_page_number, {}, true);
  Require(read.ok() && read.visible_rows.size() == 1 &&
              read.rows.size() == 1 && read.rows.front().visible &&
              read.recovery_required_count == 0 &&
              read.visible_rows.front().cells.size() == 1 &&
              !read.visible_rows.front().cells.front().value.payload.empty(),
          "borrowed read failed to return actual committed security payload");
  require_exclusion();
  auto foreign_relation = relation;
  foreign_relation.value.bytes.back() ^= 1;
  Require(!db::ReadPhysicalMgaCowRowsFromOpenDevice(
               owner, foreign_relation, locator.head_page_number, {}, true).ok(),
          "borrowed read accepted foreign relation identity");
  Require(!db::ReadPhysicalMgaCowRowsFromOpenDevice(
               owner, relation, 0, {}, true).ok(),
          "borrowed read accepted reserved page");
  require_exclusion();
  Require(owner.Close().ok(), "borrowed read owner close failed");
  Require(!db::ReadPhysicalMgaCowRowsFromOpenDevice(
               owner, relation, locator.head_page_number, {}, true).ok(),
          "borrowed read accepted closed device");
  db::PhysicalMgaCowReadRequest request;
  request.database_path = fixture.database_path.string();
  request.relation_uuid = relation;
  request.page_number = locator.head_page_number;
  const auto reopened = db::ReadPhysicalMgaCowRows(request);
  Require(reopened.ok() && reopened.visible_rows.size() == 1 &&
              reopened.visible_rows.front().row_uuid.kind ==
                  read.visible_rows.front().row_uuid.kind &&
              reopened.visible_rows.front().row_uuid.value ==
                  read.visible_rows.front().row_uuid.value &&
              reopened.visible_rows.front().cells.front().value.payload ==
                  read.visible_rows.front().cells.front().value.payload,
          "path-owning read disagrees with borrowed committed payload");
}

void TestPageBackedLifecycle() {
  auto temp = MakeTempDirectory();
  Fixture fixture = CreateFixture(temp.path / "authority.sbdb", 1788101001000ull);
  const std::uint64_t unallocated_page =
      AppendUnallocatedPages(fixture.database_path, 16384, 512);
  auto transaction = BeginTransaction(fixture.context, 1788101002000ull);

  const auto baseline = api::LoadDatabaseLocalSecurityEventStoreV1(
      fixture.context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(baseline.ok && baseline.state.events.empty() &&
              baseline.state.security_context_generation ==
                  fixture.bootstrap_generation &&
              baseline.state.locator_page_reads == 1 &&
              baseline.state.security_chain_page_reads == 0 &&
              baseline.state.legacy_scan_page_reads == 0 &&
              baseline.state.locator_migration_count == 0,
          "private security baseline generation was not bootstrap authority");

  const auto size_before_refusals =
      std::filesystem::file_size(fixture.database_path);
  {
    auto missing_authority = transaction;
    missing_authority.trace_tags.clear();
    const std::uint64_t generation = fixture.bootstrap_generation + 1;
    auto rows = Batch(
        event_codec::Encode("PRINCIPAL", transaction.local_transaction_id, {kAlicePrincipal, std::string("616c696365"), std::string("user"), std::string("active"), std::string("66696e6765727072696e74"), std::to_string(generation), std::string("0")}),
        transaction, generation, kAlicePrincipal);
    const auto refused =
        api::AppendDatabaseLocalSecurityEventBatchV1(missing_authority, rows);
    Require(!refused.ok &&
                refused.diagnostic.code ==
                    api::kDatabaseLocalSecurityDiagnosticAuthorityRequired,
            "private security missing lifecycle authority was not refused");
  }
  {
    auto wrong_transaction = transaction;
    ++wrong_transaction.local_transaction_id;
    const std::uint64_t generation = fixture.bootstrap_generation + 1;
    auto rows = Batch(
        event_codec::Encode("PRINCIPAL", wrong_transaction.local_transaction_id, {kAlicePrincipal, std::string("616c696365"), std::string("user"), std::string("active"), std::string("66696e6765727072696e74"), std::to_string(generation), std::string("0")}),
        wrong_transaction, generation, kAlicePrincipal);
    const auto refused =
        api::AppendDatabaseLocalSecurityEventBatchV1(wrong_transaction, rows);
    Require(!refused.ok &&
                refused.diagnostic.code ==
                    api::kDatabaseLocalSecurityDiagnosticTransactionRequired,
            "private security false transaction identity was not refused");
  }
  {
    const std::uint64_t generation = fixture.bootstrap_generation + 1;
    auto rows = Batch(
        event_codec::Encode("PRINCIPAL", transaction.local_transaction_id, {kAlicePrincipal, std::string("616c696365"), std::string("user"), std::string("active"), std::string("66696e6765727072696e74"), std::to_string(generation), std::string("0")}),
        transaction, generation, kAlicePrincipal);
    rows.insert(rows.begin() + 1,
                event_codec::Encode("ROLE", transaction.local_transaction_id, {kSysarchRole, std::string("73797361726368"), transaction.principal_uuid, std::string("active"), std::to_string(generation), std::string("0")}));
    const auto refused =
        api::AppendDatabaseLocalSecurityEventBatchV1(transaction, rows);
    Require(!refused.ok &&
                refused.diagnostic.code ==
                    api::kDatabaseLocalSecurityDiagnosticBatchInvalid,
            "private security second authority event was not refused");
  }
  Require(std::filesystem::file_size(fixture.database_path) ==
              size_before_refusals,
          "private security refusal changed database bytes");

  std::uint64_t generation = fixture.bootstrap_generation;
  std::vector<std::uint64_t> page_numbers;
  auto append = [&](std::vector<std::string> rows) {
    const auto appended =
        api::AppendDatabaseLocalSecurityEventBatchV1(transaction, rows);
    Require(appended.ok &&
                appended.prior_security_context_generation == generation &&
                appended.security_context_generation == generation + 1 &&
                appended.sealed_events.size() == 4 &&
                event_codec::Decode(appended.sealed_events.back())[1] == "AUTH_CONTEXT_SUCCESSOR" &&
                appended.page_number >= db::kCatalogOverflowFirstPageNumber &&
                appended.page_number > unallocated_page,
            "private security exact append result invalid");
    ++generation;
    page_numbers.push_back(appended.page_number);
  };

  append(Batch(
      event_codec::Encode("PRINCIPAL", transaction.local_transaction_id, {kAlicePrincipal, std::string("616c696365"), std::string("user"), std::string("active"), std::string("66696e6765727072696e74"), std::to_string(generation + 1), std::string("0")}),
      transaction, generation + 1, kAlicePrincipal));

  // The exclusive private-security writer lease is checked before page
  // allocation/COW. A second active transaction cannot leave an orphan page.
  auto competing_transaction =
      BeginTransaction(fixture.context, 1788101002100ull);
  const auto size_before_competing_append =
      std::filesystem::file_size(fixture.database_path);
  {
    const auto inspected = InspectLocator(fixture.database_path);
    Require(inspected.ok(),
            "private security direct publication locator inspection failed");
    const std::uint32_t committed_slot =
        inspected.anchored_locator_slot == 0 ? 1 : 0;
    const auto database_uuid = uuid::MakeDurableEngineIdentityUuid(
        UuidKind::database, fixture.context.database_uuid);
    const auto transaction_uuid = uuid::MakeDurableEngineIdentityUuid(
        UuidKind::transaction,
        competing_transaction.transaction_uuid);
    const auto fake_head = uuid::GenerateDurableEngineIdentityV7(
        UuidKind::page, 1788101002125ull);
    Require(database_uuid.ok() && transaction_uuid.ok() && fake_head.ok(),
            "private security direct publication identities invalid");
    db::DatabaseLocalPrivateSecurityLocatorSuccessorRequestV1 direct;
    direct.database_path = fixture.database_path.string();
    direct.marker_bytes =
        db::EncodeDatabaseLocalPrivateSecurityMarkerV1(inspected.marker);
    direct.expected_database_uuid = database_uuid.value;
    direct.page_size = 16384;
    direct.selected_prior = inspected.locators[committed_slot];
    direct.creator_transaction_uuid = transaction_uuid.value;
    direct.creator_local_transaction_id =
        competing_transaction.local_transaction_id;
    direct.head_page_uuid = fake_head.value;
    direct.head_page_number = unallocated_page + 2;
    direct.head_page_generation = 1;
    direct.expected_predecessor_page_number =
        direct.selected_prior.head_page_number;
    direct.security_context_generation =
        direct.selected_prior.security_context_generation + 1;
    const auto direct_refused =
        db::PublishDatabaseLocalPrivateSecurityLocatorSuccessorV1(direct);
    Require(!direct_refused.ok() &&
                std::filesystem::file_size(fixture.database_path) ==
                    size_before_competing_append,
            "private security direct publisher bypassed writer lease");
  }
  auto competing_rows = Batch(
      event_codec::Encode("ROLE", competing_transaction.local_transaction_id, {FixtureUuidLiteral("019e108d-1700-7000-8000-000000000799"), std::string("636f6e63757272656e74"), competing_transaction.principal_uuid, std::string("active"), std::to_string(generation + 1), std::string("0")}),
      competing_transaction, generation + 1,
      FixtureUuidLiteral("019e108d-1700-7000-8000-000000000799"));
  const auto competing_refused =
      api::AppendDatabaseLocalSecurityEventBatchV1(competing_transaction,
                                                   competing_rows);
  Require(!competing_refused.ok &&
              competing_refused.diagnostic.code ==
                  api::kDatabaseLocalSecurityDiagnosticTransactionRequired &&
              std::filesystem::file_size(fixture.database_path) ==
                  size_before_competing_append,
          "private security competing writer allocated a page before refusal");
  Finalize(competing_transaction,
           db::PhysicalMgaCowFinalizeDecision::rollback,
           1788101002150ull);

  append(Batch(
      event_codec::Encode("ROLE", transaction.local_transaction_id, {kSysarchRole, std::string("73797361726368"), transaction.principal_uuid, std::string("active"), std::to_string(generation + 1), std::string("0")}),
      transaction, generation + 1, kSysarchRole));
  append(Batch(
      event_codec::Encode("GROUP", transaction.local_transaction_id, {kPublicGroup, std::string("5055424c4943"), std::string{}, std::string("active"), std::to_string(generation + 1), std::string("0")}),
      transaction, generation + 1, kPublicGroup));
  append(Batch(
      event_codec::Encode("MEMBERSHIP", transaction.local_transaction_id, {kRoleMembership, kAlicePrincipal, kSysarchRole, std::string("role"), transaction.principal_uuid, std::to_string(generation + 1), std::string("0")}),
      transaction, generation + 1, kRoleMembership));
  append(Batch(
      event_codec::Encode("MEMBERSHIP", transaction.local_transaction_id, {kGroupMembership, kAlicePrincipal, kPublicGroup, std::string("group"), transaction.principal_uuid, std::to_string(generation + 1), std::string("0")}),
      transaction, generation + 1, kGroupMembership));
  append(Batch(
      event_codec::Encode("GRANT", transaction.local_transaction_id, {kConnectGrant, kSysarchRole, std::string("role"), api::EngineUuid{}, std::string{}, std::string("CONNECT"), transaction.principal_uuid, std::string("allow"), std::to_string(generation + 1), std::string("0")}),
      transaction, generation + 1, kConnectGrant));
  append(Batch(
      event_codec::Encode("GRANT", transaction.local_transaction_id, {kConnectDeny, kAlicePrincipal, std::string("principal"), api::EngineUuid{}, std::string{}, std::string("CONNECT"), transaction.principal_uuid, std::string("deny"), std::to_string(generation + 1), std::string("0")}),
      transaction, generation + 1, kConnectDeny));
  const auto staged_after_seven = InspectLocator(fixture.database_path);
  Require(staged_after_seven.ok() &&
              staged_after_seven.anchored_locator_generation == 2 &&
              staged_after_seven
                      .locators[staged_after_seven.anchored_locator_slot]
                      .chain_page_count == 7 &&
              staged_after_seven
                      .locators[staged_after_seven.anchored_locator_slot]
                      .security_context_generation == generation,
          "private security seventh same-transaction candidate changed committed-lineage generation");
  const std::string row_policy_event =
      event_codec::Encode("ROW_POLICY", transaction.local_transaction_id, {kRowPolicy, kConnectGrant, std::string("relation"), std::string("allow_if"), std::string("01020304"), transaction.principal_uuid, std::string("active"), std::to_string(generation + 1), std::string("0"), FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007e2"), std::string("92"), std::string("7"), std::string("2"), kRowPolicy, std::string("3"), FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007e3"), std::string("4"), std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007e4"), std::string("5"), std::string("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007e5"), std::string("6"), std::string("7")});
  append(Batch(row_policy_event, transaction, generation + 1, kRowPolicy));

  const auto own_view = api::LoadDatabaseLocalSecurityEventStoreV1(
      transaction,
      api::DatabaseLocalSecurityEventVisibilityV1::
          include_reader_own_uncommitted);
  Require(own_view.ok &&
              own_view.state.security_context_generation == generation &&
              own_view.state.events.size() == 32 &&
              own_view.state.locator_page_reads == 1 &&
              own_view.state.security_chain_page_reads == 8 &&
              own_view.state.legacy_scan_page_reads == 0,
          "private security own MGA view did not include staged batches");
  Require(CountEventKind(own_view.state.events, "PRINCIPAL") == 1 &&
              CountEventKind(own_view.state.events, "ROLE") == 1 &&
              CountEventKind(own_view.state.events, "GROUP") == 1 &&
              CountEventKind(own_view.state.events, "MEMBERSHIP") == 2 &&
              CountEventKind(own_view.state.events, "GRANT") == 2 &&
              CountEventKind(own_view.state.events, "ROW_POLICY") == 1 &&
              CountEventKind(own_view.state.events,
                             "AUTH_CONTEXT_SUCCESSOR") == 8 &&
              std::find(own_view.state.events.begin(),
                        own_view.state.events.end(),
                        row_policy_event) != own_view.state.events.end(),
          "private security principal/role/group/membership/grant/deny rows missing");
  const auto staged_locator = InspectLocator(fixture.database_path);
  Require(staged_locator.ok() &&
              staged_locator.anchored_locator_generation == 2 &&
              staged_locator.locators[staged_locator.anchored_locator_slot]
                      .chain_page_count == 8 &&
              staged_locator.locators[staged_locator.anchored_locator_slot]
                      .security_context_generation == generation &&
              staged_locator.locators[staged_locator.anchored_locator_slot]
                      .creator_local_transaction_id ==
                  transaction.local_transaction_id,
          "private security eight same-transaction appends did not retain one staged lineage");

  const auto before_commit = api::LoadDatabaseLocalSecurityEventStoreV1(
      fixture.context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(before_commit.ok && before_commit.state.events.empty() &&
              before_commit.state.security_context_generation ==
                  fixture.bootstrap_generation,
          "private security uncommitted rows escaped MGA visibility");

  Finalize(transaction, db::PhysicalMgaCowFinalizeDecision::commit,
           1788101003000ull);
  const auto committed = api::LoadDatabaseLocalSecurityEventStoreV1(
      fixture.context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(committed.ok &&
              committed.state.security_context_generation == generation &&
              committed.state.events == own_view.state.events &&
              committed.state.database_identity_authenticated &&
              committed.state.page_identity_authenticated &&
              committed.state.durable_inventory_authenticated &&
              committed.state.locator_page_reads == 1 &&
              committed.state.security_chain_page_reads == 8 &&
              committed.state.legacy_scan_page_reads == 0,
          "private security committed page-backed state did not reload");
  VerifyBorrowedReadOwnership(fixture);
  Require(!std::filesystem::exists(
              fixture.database_path.string() +
              ".sb.security_principal_events") &&
              !std::filesystem::exists(
                  fixture.database_path.string() + ".sb.local_password_auth"),
          "private security page-backed path created a forbidden sidecar");

  Require(db::MarkDatabaseCleanShutdown(fixture.database_path.string()).ok(),
          "private security restart clean marker failed");
  db::DatabaseOpenConfig reopen;
  reopen.path = fixture.database_path.string();
  Require(db::OpenDatabaseFile(reopen).ok(),
          "private security database restart failed");
  const auto restarted = api::LoadDatabaseLocalSecurityEventStoreV1(
      fixture.context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(restarted.ok && restarted.state.events == committed.state.events &&
              restarted.state.security_context_generation == generation &&
              restarted.state.locator_page_reads == 1 &&
              restarted.state.security_chain_page_reads == 8 &&
              restarted.state.legacy_scan_page_reads == 0,
          "private security restart did not reconstruct committed successors");

  const auto anchor_logical_path = temp.path / "authority-anchor-logical.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path,
                                     anchor_logical_path),
          "private security anchor logical corruption copy failed");
  FlipDatabaseByte(
      anchor_logical_path,
      LocatorBodyAbsoluteOffset(
          16384, db::kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[0], 24));
  auto anchor_logical_context = fixture.context;
  anchor_logical_context.database_path = anchor_logical_path.string();
  const auto anchor_logical_recovered =
      api::LoadDatabaseLocalSecurityEventStoreV1(
          anchor_logical_context,
          api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(anchor_logical_recovered.ok &&
              anchor_logical_recovered.state.events == committed.state.events,
          "private security one-copy anchor logical corruption did not recover from peer");

  const auto anchor_digest_path = temp.path / "authority-anchor-digest.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path,
                                     anchor_digest_path),
          "private security anchor digest corruption copy failed");
  FlipDatabaseByte(
      anchor_digest_path,
      LocatorBodyAbsoluteOffset(
          16384, db::kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[0], 120));
  auto anchor_digest_context = fixture.context;
  anchor_digest_context.database_path = anchor_digest_path.string();
  Require(api::LoadDatabaseLocalSecurityEventStoreV1(
              anchor_digest_context,
              api::DatabaseLocalSecurityEventVisibilityV1::latest_committed)
              .ok,
          "private security one-copy anchor digest corruption did not recover");

  const auto both_anchor_path = temp.path / "authority-anchor-both.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path, both_anchor_path),
          "private security both-anchor corruption copy failed");
  for (std::uint32_t copy = 0; copy < 2; ++copy) {
    FlipDatabaseByte(
        both_anchor_path,
        LocatorBodyAbsoluteOffset(
            16384,
            db::kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[copy], 120));
  }
  auto both_anchor_context = fixture.context;
  both_anchor_context.database_path = both_anchor_path.string();
  Require(!api::LoadDatabaseLocalSecurityEventStoreV1(
               both_anchor_context,
               api::DatabaseLocalSecurityEventVisibilityV1::latest_committed)
               .ok,
          "private security two-copy anchor corruption did not fail closed");

  const auto locator_digest_path = temp.path / "authority-locator-digest.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path,
                                     locator_digest_path),
          "private security current locator corruption copy failed");
  const auto committed_locator = InspectLocator(locator_digest_path);
  FlipDatabaseByte(
      locator_digest_path,
      LocatorBodyAbsoluteOffset(
          16384,
          db::kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1
              [committed_locator.anchored_locator_slot],
          240));
  auto locator_digest_context = fixture.context;
  locator_digest_context.database_path = locator_digest_path.string();
  const auto locator_digest_corrupt =
      api::LoadDatabaseLocalSecurityEventStoreV1(
          locator_digest_context,
          api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(!locator_digest_corrupt.ok &&
              locator_digest_corrupt.state.legacy_scan_page_reads == 0,
          "private security anchored locator corruption did not fail without scan");

  auto rollback_transaction =
      BeginTransaction(fixture.context, 1788101004000ull);
  auto rollback_rows = Batch(
      event_codec::Encode("ROLE", rollback_transaction.local_transaction_id, {FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007e1"), std::string("726f6c6c6261636b"), rollback_transaction.principal_uuid, std::string("active"), std::to_string(generation + 1), std::string("0")}),
      rollback_transaction, generation + 1,
      FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007e1"));
  const auto staged_rollback =
      api::AppendDatabaseLocalSecurityEventBatchV1(rollback_transaction,
                                                   rollback_rows);
  Require(staged_rollback.ok &&
              staged_rollback.security_context_generation == generation + 1,
          "private security rollback fixture stage failed");
  Finalize(rollback_transaction,
           db::PhysicalMgaCowFinalizeDecision::rollback,
           1788101005000ull);
  const auto after_rollback = api::LoadDatabaseLocalSecurityEventStoreV1(
      fixture.context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(after_rollback.ok &&
              after_rollback.state.security_context_generation == generation &&
              after_rollback.state.events == committed.state.events,
          "private security rollback published rows or successor");

  const auto fallback_corrupt_path =
      temp.path / "authority-fallback-prior-corrupt.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path,
                                     fallback_corrupt_path),
          "private security fallback prior corruption copy failed");
  const auto rollback_locator = InspectLocator(fallback_corrupt_path);
  const std::uint32_t fallback_slot =
      rollback_locator.anchored_locator_slot == 0 ? 1 : 0;
  FlipDatabaseByte(
      fallback_corrupt_path,
      LocatorBodyAbsoluteOffset(
          16384,
          db::kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1[fallback_slot],
          240));
  auto fallback_corrupt_context = fixture.context;
  fallback_corrupt_context.database_path = fallback_corrupt_path.string();
  Require(!api::LoadDatabaseLocalSecurityEventStoreV1(
               fallback_corrupt_context,
               api::DatabaseLocalSecurityEventVisibilityV1::latest_committed)
               .ok,
          "private security tampered digest-linked fallback was admitted");

  const auto corrupt_path = temp.path / "authority-corrupt.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path, corrupt_path),
          "private security corruption copy failed");
  CorruptSecurityPage(corrupt_path, page_numbers.front(), 16384);
  auto corrupt_context = fixture.context;
  corrupt_context.database_path = corrupt_path.string();
  const auto corrupt = api::LoadDatabaseLocalSecurityEventStoreV1(
      corrupt_context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(!corrupt.ok &&
              corrupt.diagnostic.code ==
                  api::kDatabaseLocalSecurityDiagnosticCorrupt,
          "private security page corruption did not fail closed");

  auto wrong_database = fixture.context;
  wrong_database.database_uuid = scratchbird::tests::FixtureUuidLiteral("019e108d-1700-7000-8000-0000000007ff");
  const auto wrong_identity = api::LoadDatabaseLocalSecurityEventStoreV1(
      wrong_database,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(!wrong_identity.ok,
          "private security database identity mismatch was admitted");
}

void TestLocatorCrashCuts() {
  static constexpr std::array<std::string_view, 3> kFaultTags = {
      "engine.test.private_security_locator_fault.after_locator",
      "engine.test.private_security_locator_fault.after_anchor_copy_0",
      "engine.test.private_security_locator_fault.readback_mismatch"};
  for (std::size_t index = 0; index < kFaultTags.size(); ++index) {
    auto temp = MakeTempDirectory();
    Fixture fixture = CreateFixture(
        temp.path / ("crash-" + std::to_string(index) + ".sbdb"),
        1788102001000ull + index * 10000);
    auto transaction = BeginTransaction(
        fixture.context, 1788102002000ull + index * 10000);
    transaction.trace_tags.emplace_back(kFaultTags[index]);
    const auto rows = SingleRoleBatch(
        transaction, fixture.bootstrap_generation + 1);
    const auto faulted =
        api::AppendDatabaseLocalSecurityEventBatchV1(transaction, rows);
    Require(!faulted.ok &&
                faulted.diagnostic.code ==
                    api::kDatabaseLocalSecurityDiagnosticWriteFailed,
            "private security locator crash cut did not return non-OK");
    const auto committed_before_rollback =
        api::LoadDatabaseLocalSecurityEventStoreV1(
            fixture.context,
            api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
    Require(committed_before_rollback.ok &&
                committed_before_rollback.state.events.empty() &&
                committed_before_rollback.state.security_context_generation ==
                    fixture.bootstrap_generation,
            "private security crash cut exposed unacknowledged successor");

    if (index == 1) {
      const auto torn_path = temp.path / "torn-anchor-corrupt-target.sbdb";
      Require(std::filesystem::copy_file(fixture.database_path, torn_path),
              "private security torn-anchor corruption copy failed");
      const auto torn = InspectLocator(torn_path);
      const std::uint32_t candidate_slot =
          torn.anchored_locator_slot == 0 ? 1 : 0;
      FlipDatabaseByte(
          torn_path,
          LocatorBodyAbsoluteOffset(
              16384,
              db::kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1
                  [candidate_slot],
              240));
      auto torn_context = fixture.context;
      torn_context.database_path = torn_path.string();
      const auto recovered = api::LoadDatabaseLocalSecurityEventStoreV1(
          torn_context,
          api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
      Require(recovered.ok && recovered.state.events.empty(),
              "private security older anchor did not survive corrupt torn successor target");
    }

    Finalize(transaction, db::PhysicalMgaCowFinalizeDecision::rollback,
             1788102003000ull + index * 10000);
    auto retry = BeginTransaction(
        fixture.context, 1788102004000ull + index * 10000);
    const auto retried = api::AppendDatabaseLocalSecurityEventBatchV1(
        retry, SingleRoleBatch(retry, fixture.bootstrap_generation + 1));
    Require(retried.ok,
            "private security terminal crash candidate was not reusable");
    Finalize(retry, db::PhysicalMgaCowFinalizeDecision::commit,
             1788102005000ull + index * 10000);
    const auto committed = api::LoadDatabaseLocalSecurityEventStoreV1(
        fixture.context,
        api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
    Require(committed.ok && committed.state.events.size() == 4 &&
                committed.state.security_chain_page_reads == 1,
            "private security crash recovery successor did not commit exactly once");
  }
}

void TestMalformedInactiveLocatorRefusal() {
  auto temp = MakeTempDirectory();
  Fixture fixture = CreateFixture(temp.path / "malformed-inactive.sbdb",
                                  1788103001000ull);
  FlipDatabaseByte(
      fixture.database_path,
      LocatorBodyAbsoluteOffset(
          16384, db::kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1[1], 0),
      0x01u);
  const auto baseline = api::LoadDatabaseLocalSecurityEventStoreV1(
      fixture.context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(baseline.ok && baseline.state.events.empty(),
          "private security malformed inactive slot changed committed read");
  auto transaction = BeginTransaction(fixture.context, 1788103002000ull);
  const auto size_before = std::filesystem::file_size(fixture.database_path);
  const auto refused = api::AppendDatabaseLocalSecurityEventBatchV1(
      transaction,
      SingleRoleBatch(transaction, fixture.bootstrap_generation + 1));
  Require(!refused.ok &&
              refused.diagnostic.code ==
                  api::kDatabaseLocalSecurityDiagnosticCorrupt &&
              std::filesystem::file_size(fixture.database_path) == size_before,
          "private security malformed inactive locator was overwritten or allocated a page");
  Finalize(transaction, db::PhysicalMgaCowFinalizeDecision::rollback,
           1788103003000ull);
}

void TestSealedLegacyMigration() {
  auto temp = MakeTempDirectory();
  Fixture fixture = CreateFixture(temp.path / "legacy-source-current.sbdb",
                                  1788104001000ull);
  AppendUnallocatedPages(fixture.database_path, 16384, 256);
  auto transaction = BeginTransaction(fixture.context, 1788104002000ull);
  const auto appended = api::AppendDatabaseLocalSecurityEventBatchV1(
      transaction,
      SingleRoleBatch(transaction, fixture.bootstrap_generation + 1));
  Require(appended.ok,
          "private security legacy source append failed");
  Finalize(transaction, db::PhysicalMgaCowFinalizeDecision::commit,
           1788104003000ull);
  auto second_transaction =
      BeginTransaction(fixture.context, 1788104003100ull);
  const auto second_appended =
      api::AppendDatabaseLocalSecurityEventBatchV1(
          second_transaction,
          SingleRoleBatch(second_transaction,
                          fixture.bootstrap_generation + 2,
                          FixtureUuidLiteral("019e108d-1700-7000-8000-000000000797")));
  Require(second_appended.ok &&
              second_appended.prior_security_context_generation ==
                  fixture.bootstrap_generation + 1,
          "private security second legacy source append failed");
  Finalize(second_transaction, db::PhysicalMgaCowFinalizeDecision::commit,
           1788104003200ull);
  Require(db::MarkDatabaseCleanShutdown(fixture.database_path.string()).ok(),
          "private security legacy source clean marker failed");

  const auto invalid_payload_path = temp.path / "legacy-invalid-payload.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path,
                                     invalid_payload_path),
          "private security invalid legacy payload copy failed");
  const auto migrated_security_pages =
      ConvertCurrentSecurityChainToLegacy(fixture.database_path, 16384,
                                          false);
  ConvertCurrentSecurityChainToLegacy(invalid_payload_path, 16384, true);
  Require(migrated_security_pages.size() == 2,
          "private security legacy fixture was not a two-page chain");

  db::DatabaseOpenConfig read_only;
  read_only.path = fixture.database_path.string();
  read_only.read_only = true;
  const auto read_only_refused = db::OpenDatabaseFile(read_only);
  Require(!read_only_refused.ok() &&
              read_only_refused.diagnostic.diagnostic_code ==
                  "FORMAT.UPGRADE_REQUIRED",
          "private security read-only legacy open did not require upgrade");

  db::DatabaseOpenConfig invalid_payload_open;
  invalid_payload_open.path = invalid_payload_path.string();
  const auto invalid_payload = db::OpenDatabaseFile(invalid_payload_open);
  Require(!invalid_payload.ok(),
          "private security substituted authenticated legacy payload was sealed");
  invalid_payload_open.read_only = true;
  const auto invalid_still_legacy = db::OpenDatabaseFile(invalid_payload_open);
  Require(!invalid_still_legacy.ok() &&
              invalid_still_legacy.diagnostic.diagnostic_code ==
                  "FORMAT.UPGRADE_REQUIRED",
          "private security failed payload migration modified live source");

  static constexpr std::array<std::string_view, 2> kPreReplaceCuts = {
      "after_copy_before_temp_scan",
      "after_temp_sync_before_replace"};
  for (std::size_t index = 0; index < kPreReplaceCuts.size(); ++index) {
    const auto cut_path =
        temp.path / ("legacy-pre-replace-" + std::to_string(index) + ".sbdb");
    Require(std::filesystem::copy_file(fixture.database_path, cut_path),
            "private security pre-replace cut copy failed");
    db::DatabaseOpenConfig cut;
    cut.path = cut_path.string();
    cut.private_relation_locator_migration_fault_injection_point =
        std::string(kPreReplaceCuts[index]);
    Require(!db::OpenDatabaseFile(cut).ok(),
            "private security pre-replace migration cut did not fire");
    cut.read_only = true;
    cut.private_relation_locator_migration_fault_injection_point.clear();
    const auto source_unchanged = db::OpenDatabaseFile(cut);
    Require(!source_unchanged.ok() &&
                source_unchanged.diagnostic.diagnostic_code ==
                    "FORMAT.UPGRADE_REQUIRED",
            "private security pre-replace cut changed live legacy source");
    cut.read_only = false;
    const auto retried = db::OpenDatabaseFile(cut);
    if (!retried.ok()) {
      Fail("private security pre-replace cut retry did not migrate once:" +
           std::to_string(index) + ":" +
           retried.diagnostic.diagnostic_code + ":" +
           retried.diagnostic.message_key);
    }
  }

  const auto post_replace_path = temp.path / "legacy-post-replace.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path, post_replace_path),
          "private security post-replace cut copy failed");
  db::DatabaseOpenConfig post_replace;
  post_replace.path = post_replace_path.string();
  post_replace.private_relation_locator_migration_fault_injection_point =
      "after_replace_directory_sync_before_reopen";
  Require(!db::OpenDatabaseFile(post_replace).ok(),
          "private security post-replace migration cut did not fire");
  post_replace.private_relation_locator_migration_fault_injection_point.clear();
  post_replace.read_only = true;
  Require(db::OpenDatabaseFile(post_replace).ok(),
          "private security post-replace sealed image was not reopenable");

  const auto migrated_path = temp.path / "legacy-migrated.sbdb";
  Require(std::filesystem::copy_file(fixture.database_path, migrated_path),
          "private security normal legacy migration copy failed");
  db::DatabaseOpenConfig migrate;
  migrate.path = migrated_path.string();
  Require(db::OpenDatabaseFile(migrate).ok(),
          "private security sealed legacy migration failed");
  auto migrated_context = fixture.context;
  migrated_context.database_path = migrated_path.string();
  const auto migrated = api::LoadDatabaseLocalSecurityEventStoreV1(
      migrated_context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(migrated.ok && migrated.state.events.size() == 8 &&
              migrated.state.security_context_generation ==
                  fixture.bootstrap_generation + 2 &&
              migrated.state.locator_page_reads == 1 &&
              migrated.state.security_chain_page_reads == 2 &&
              migrated.state.legacy_scan_page_reads == 0 &&
              migrated.state.locator_migration_count == 1 &&
              migrated.state.page_numbers.size() == 2 &&
              migrated.state.page_numbers.front() ==
                  migrated_security_pages.back() &&
              migrated.state.page_numbers.back() ==
                  migrated_security_pages.front(),
          "private security migrated chain/metrics invalid");
  const auto migrated_locator = InspectLocator(migrated_path);
  const auto& migrated_head =
      migrated_locator.locators[migrated_locator.anchored_locator_slot];
  Require(migrated_locator.ok() && migrated_head.locator_generation == 1 &&
              migrated_head.lineage ==
                  db::DatabaseLocalPrivateSecurityLocatorLineageV1::
                      sealed_legacy_migration &&
              migrated_head.creator_transaction_uuid.is_nil() &&
              migrated_head.creator_local_transaction_id == 0 &&
              migrated_head.chain_page_count == 2 &&
              migrated_head.head_page_number ==
                  migrated_security_pages.front() &&
              migrated_head.tail_page_number ==
                  migrated_security_pages.back() &&
              migrated_head.security_context_generation ==
                  fixture.bootstrap_generation + 2 &&
              std::all_of(migrated_head.prior_locator_sha256.begin(),
                          migrated_head.prior_locator_sha256.end(),
                          [](byte value) { return value == 0; }) &&
              migrated_locator.marker.migration_scan_count == 1,
          "private security migrated generation-one seal invalid");
  Require(db::OpenDatabaseFile(migrate).ok(),
          "private security sealed restart failed");
  const auto migrated_restart = api::LoadDatabaseLocalSecurityEventStoreV1(
      migrated_context,
      api::DatabaseLocalSecurityEventVisibilityV1::latest_committed);
  Require(migrated_restart.ok &&
              migrated_restart.state.locator_migration_count == 1 &&
              migrated_restart.state.legacy_scan_page_reads == 0,
          "private security sealed restart repeated legacy scan");

  const auto rewritten_corrupt_path =
      temp.path / "legacy-rewritten-page-corrupt.sbdb";
  Require(std::filesystem::copy_file(migrated_path, rewritten_corrupt_path),
          "private security rewritten page corruption copy failed");
  CorruptSecurityPage(rewritten_corrupt_path,
                      migrated_security_pages.back(), 16384);
  auto rewritten_context = migrated_context;
  rewritten_context.database_path = rewritten_corrupt_path.string();
  Require(!api::LoadDatabaseLocalSecurityEventStoreV1(
               rewritten_context,
               api::DatabaseLocalSecurityEventVisibilityV1::latest_committed)
               .ok,
          "private security rewritten migrated page corruption was admitted");
}

}  // namespace

struct RetainedSecuritySnapshot {
  scratchbird::core::platform::TypedUuid id;
  mga::PublishedSnapshotPin pin;
  explicit RetainedSecuritySnapshot(const api::EngineRequestContext& context) {
    const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(context.database_path);
    Require(inventory.ok(), "retained security source inventory unavailable");
    const auto published = mga::PublishStatementStableSnapshotVector(inventory.inventory,
        mga::MakeLocalTransactionId(context.local_transaction_id), 1788109000000ull);
    Require(published.ok(), "actual retained security snapshot publication failed");
    id = published.descriptor.snapshot_uuid;
    pin = mga::RetainPublishedSnapshotVector(id);
    Require(pin.valid() && pin.Observe().ok(), "actual retained security snapshot pin failed");
  }
  ~RetainedSecuritySnapshot() { mga::ReleasePublishedSnapshotVector(id); }
};

std::array<byte, 32> ReadDatabaseDigest(const std::string& path) {
  disk::FileDevice device;
  Require(device.Open(path, disk::FileOpenMode::open_existing_read_only).ok(),
      "source no-effects digest open failed");
  const auto size = device.Size();
  Require(size.ok() && size.size_bytes && size.size_bytes <= 64 * 1024 * 1024,
      "bounded source fixture digest size invalid");
  std::vector<byte> bytes(static_cast<std::size_t>(size.size_bytes));
  Require(device.ReadAt(0, bytes.data(), bytes.size()).ok() && device.Close().ok(),
      "source no-effects digest read/close failed");
  const auto digest = core_hash::ComputeSha256Digest(bytes);
  Require(digest.ok(), "source no-effects digest computation failed");
  return digest.digest;
}

void TestRetainedSourceAllocationFailures(const api::EngineRequestContext& context,
    const mga::PublishedSnapshotPin& pin) {
  const auto expected = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(context, pin);
  Require(expected.ok, "allocation sweep reference source unavailable");
  source_allocation_fault::attempts = 0;
  source_allocation_fault::count = true;
  const auto measured = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(context, pin);
  source_allocation_fault::count = false;
  const auto count = source_allocation_fault::attempts;
  Require(measured.ok && count && count < 100000, "bounded source allocation inventory unavailable");
  const auto file_size = std::filesystem::file_size(context.database_path);
  const auto file_digest = ReadDatabaseDigest(context.database_path);
  unsigned exceptions = 0, refusals = 0, complete = 0;
  for (std::size_t i = 0; i != count; ++i) {
    std::optional<api::DatabaseLocalSecurityEventStoreLoadResultV1> output;
    source_allocation_fault::hit = false;
    source_allocation_fault::budget = static_cast<long>(i);
    try { output.emplace(api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(context, pin)); }
    catch (const std::bad_alloc&) { ++exceptions; }
    catch (...) { source_allocation_fault::budget = -1; throw; }
    source_allocation_fault::budget = -1;
    Require(source_allocation_fault::hit, "measured source allocation position did not fire");
    if (output) {
      if (output->ok) {
        ++complete;
        const auto& actual_bootstrap = output->state.bootstrap_authority;
        const auto& expected_bootstrap = expected.state.bootstrap_authority;
        Require(output->state.events == expected.state.events &&
            output->state.page_numbers == expected.state.page_numbers &&
            output->state.security_context_generation == expected.state.security_context_generation &&
            output->state.database_identity_authenticated && output->state.page_identity_authenticated &&
            output->state.durable_inventory_authenticated &&
            actual_bootstrap.authenticated == expected_bootstrap.authenticated &&
            actual_bootstrap.principal_present == expected_bootstrap.principal_present &&
            actual_bootstrap.principal_uuid == expected_bootstrap.principal_uuid &&
            actual_bootstrap.principal_name == expected_bootstrap.principal_name &&
            actual_bootstrap.credential_fingerprint == expected_bootstrap.credential_fingerprint &&
            actual_bootstrap.sysarch_role_uuid == expected_bootstrap.sysarch_role_uuid &&
            actual_bootstrap.membership_uuid == expected_bootstrap.membership_uuid &&
            actual_bootstrap.creator_tx == expected_bootstrap.creator_tx &&
            actual_bootstrap.policy_generation == expected_bootstrap.policy_generation &&
            actual_bootstrap.security_context_generation == expected_bootstrap.security_context_generation,
            "optional allocation failure published an incomplete source");
      } else {
        ++refusals;
        Require(output->state.events.empty() && output->state.page_numbers.empty() &&
            output->state.security_context_generation == 0 &&
            !output->state.database_identity_authenticated && !output->state.page_identity_authenticated &&
            !output->state.durable_inventory_authenticated && !output->diagnostic.code.empty(),
            "failed source allocation exposed partial authority or lost diagnostics");
      }
    }
    Require(std::filesystem::file_size(context.database_path) == file_size && pin.Observe().ok(),
        "source allocation failure changed physical storage or revoked its caller's pin");
    const auto retry = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(context, pin);
    Require(retry.ok && retry.state.events == expected.state.events &&
        retry.state.page_numbers == expected.state.page_numbers &&
        retry.state.security_context_generation == expected.state.security_context_generation,
        "source allocation failure stranded device/guard custody or poisoned retry");
  }
  Require(ReadDatabaseDigest(context.database_path) == file_digest,
      "source allocation sweep changed persisted bytes without extending the file");
  std::cout << "retained_security_source_allocation_faults=" << count << " exceptions=" << exceptions
      << " refusals=" << refusals << " complete_optional=" << complete << '\n';
}

void TestRetainedSecuritySnapshots() {
  using Visibility = api::DatabaseLocalSecurityEventVisibilityV1;
  for (const std::uint32_t page_size : {8192u, 16384u, 32768u, 65536u, 131072u}) {
    auto directory = MakeTempDirectory();
    const auto fixture = CreateFixture(directory.path / "retained.sbdb",
        1788109000000ull + page_size, page_size);
    auto writer = BeginTransaction(fixture.context, 1788109200000ull);
    // A lower-numbered writer commits AFTER publication. A higher-numbered
    // committed marker ensures the test requires actual captured exclusions,
    // not just comparing row numbers to the committed high watermark.
    const auto marker = BeginTransaction(fixture.context, 1788109200100ull);
    Finalize(marker, db::PhysicalMgaCowFinalizeDecision::commit, 1788109200200ull);
    auto reader = BeginTransaction(fixture.context, 1788109200300ull);
    auto stable = BeginTransaction(fixture.context, 1788109200400ull, true);
    auto read_only = BeginTransaction(fixture.context, 1788109200500ull, true, true);
    RetainedSecuritySnapshot original(reader), stable_source(stable), read_only_source(read_only);
    const auto before = original.pin.Observe();
    Require(before.ok() && before.descriptor->visible_committed_high_watermark >= writer.local_transaction_id &&
        std::binary_search(before.descriptor->active_excluded_local_transaction_ids.begin(),
            before.descriptor->active_excluded_local_transaction_ids.end(), writer.local_transaction_id),
        "retained fixture did not capture lower-numbered active-writer exclusion");
    const auto empty = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, {});
    Require(!empty.ok && empty.state.events.empty() && empty.state.page_numbers.empty() &&
        empty.diagnostic.code == "SB-MGA-SNAPSHOT-VECTOR-UNKNOWN", "empty pin fabricated security authority");
    const auto initial = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, original.pin);
    Require(initial.ok && initial.state.security_context_generation == fixture.bootstrap_generation &&
        initial.state.events.empty(), "initial pinned security source differs from bootstrap");
    std::uint64_t last_page = 0;
    std::optional<api::EngineRequestContext> mid_reader;
    std::unique_ptr<RetainedSecuritySnapshot> middle;
    for (unsigned n = 0; n != 4; ++n) {
      if (n) writer = BeginTransaction(fixture.context, 1788109300000ull + n * 1000);
      const auto role = uuid::GenerateDurableEngineIdentityV7(UuidKind::object, 1788109400000ull + n);
      Require(role.ok(), "historical security role identity issuance failed");
      const auto staged = api::AppendDatabaseLocalSecurityEventBatchV1(writer,
          SingleRoleBatch(writer, fixture.bootstrap_generation + n + 1, role.value.value));
      Require(staged.ok, "real historical security successor staging failed");
      last_page = staged.page_number;
      Finalize(writer, db::PhysicalMgaCowFinalizeDecision::commit, 1788109500000ull + n * 1000);
      if (!n) {
        mid_reader = BeginTransaction(fixture.context, 1788109600000ull);
        middle = std::make_unique<RetainedSecuritySnapshot>(*mid_reader);
      }
      const auto old = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, original.pin);
      Require(old.ok && old.state.security_context_generation == fixture.bootstrap_generation &&
          old.state.events.empty() && old.state.page_numbers.empty() &&
          old.state.security_chain_page_reads == n + 1,
          "old pin advanced or two-slot locator lost historical security state");
      const auto stable_read = api::LoadDatabaseLocalSecurityEventStoreV1(stable,
          Visibility::include_reader_own_uncommitted);
      const auto read_only_read = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(
          read_only, read_only_source.pin);
      Require(stable_read.ok && read_only_read.ok && stable_read.state.events.empty() &&
          read_only_read.state.events.empty() &&
          stable_read.state.security_context_generation == fixture.bootstrap_generation &&
          read_only_read.state.security_context_generation == fixture.bootstrap_generation,
          "stable/read-only security source did not preserve admitted visibility");
      const auto current = api::LoadDatabaseLocalSecurityEventStoreV1(reader,
          Visibility::include_reader_own_uncommitted);
      const auto midway = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(*mid_reader, middle->pin);
      Require(current.ok && current.state.security_context_generation == fixture.bootstrap_generation + n + 1 &&
          CountEventKind(current.state.events, "ROLE") == n + 1 &&
          midway.ok && midway.state.security_context_generation == fixture.bootstrap_generation + 1 &&
          CountEventKind(midway.state.events, "ROLE") == 1 && midway.state.page_numbers.size() == 1,
          "pinned middle generation was replaced by latest security authority");
    }
    const auto size_before = std::filesystem::file_size(fixture.database_path);
    const auto digest_before = ReadDatabaseDigest(fixture.database_path.string());
    const auto stale_write = api::AppendDatabaseLocalSecurityEventBatchV1(stable,
        SingleRoleBatch(stable, fixture.bootstrap_generation + 1));
    Require(!stale_write.ok && stale_write.page_number == 0 &&
        stale_write.diagnostic.detail == "security_write_snapshot_not_current" &&
        std::filesystem::file_size(fixture.database_path) == size_before &&
        ReadDatabaseDigest(fixture.database_path.string()) == digest_before,
        "obsolete stable security source staged a physical successor");
    const auto readonly_write = api::AppendDatabaseLocalSecurityEventBatchV1(read_only,
        SingleRoleBatch(read_only, fixture.bootstrap_generation + 1));
    Require(!readonly_write.ok && readonly_write.page_number == 0 &&
        std::filesystem::file_size(fixture.database_path) == size_before &&
        ReadDatabaseDigest(fixture.database_path.string()) == digest_before,
        "read-only security source acquired mutation authority");
    auto invented_observations = reader;
    invented_observations.snapshot_visible_through_local_transaction_id = std::numeric_limits<std::uint64_t>::max();
    invented_observations.statement_snapshot_uuid = kAlicePrincipal;
    const auto ignored = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(invented_observations, original.pin);
    Require(ignored.ok && ignored.state.events.empty() &&
        ignored.state.security_context_generation == fixture.bootstrap_generation,
        "caller snapshot observations overrode actual retained publication");
    if (page_size == 8192)
      TestRetainedSourceAllocationFailures(*mid_reader, middle->pin);
    const auto prepared_writer = BeginTransaction(fixture.context, 1788109800000ull);
    const auto prepared_role = uuid::GenerateDurableEngineIdentityV7(UuidKind::object, 1788109800100ull);
    Require(prepared_role.ok(), "prepared security role UUID unavailable");
    const auto prepared_batch = api::AppendDatabaseLocalSecurityEventBatchV1(prepared_writer,
        SingleRoleBatch(prepared_writer, fixture.bootstrap_generation + 5, prepared_role.value.value));
    Require(prepared_batch.ok, "prepared security batch staging failed");
    last_page = prepared_batch.page_number;
    const auto to_prepare = db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
    Require(to_prepare.ok(), "prepared security inventory unavailable");
    const auto prepared = mga::PrepareLocalTransaction(to_prepare.inventory,
        mga::MakeLocalTransactionId(prepared_writer.local_transaction_id));
    Require(prepared.ok() && db::PersistLocalTransactionInventoryToDatabase(
        fixture.database_path.string(), prepared.inventory).ok(), "prepared security inventory publication failed");
    const auto higher_marker = BeginTransaction(fixture.context, 1788109800200ull);
    Finalize(higher_marker, db::PhysicalMgaCowFinalizeDecision::commit, 1788109800300ull);
    RetainedSecuritySnapshot in_doubt_source(reader);
    const auto in_doubt = in_doubt_source.pin.Observe();
    Require(in_doubt.ok() && in_doubt.descriptor->visible_committed_high_watermark >= prepared_writer.local_transaction_id &&
        std::binary_search(in_doubt.descriptor->in_doubt_excluded_local_transaction_ids.begin(),
            in_doubt.descriptor->in_doubt_excluded_local_transaction_ids.end(), prepared_writer.local_transaction_id),
        "fixture did not capture below-boundary prepared-writer exclusion");
    const auto before_commit = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, in_doubt_source.pin);
    Require(before_commit.ok && before_commit.state.security_context_generation == fixture.bootstrap_generation + 4 &&
        CountEventKind(before_commit.state.events, "ROLE") == 4,
        "prepared security creator became visible before finality");
    Finalize(prepared_writer, db::PhysicalMgaCowFinalizeDecision::commit, 1788109800400ull);
    const auto after_commit = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, in_doubt_source.pin);
    Require(after_commit.ok && after_commit.state.events == before_commit.state.events &&
        after_commit.state.page_numbers == before_commit.state.page_numbers &&
        after_commit.state.security_context_generation == before_commit.state.security_context_generation,
        "prepared creator became visible inside an earlier retained security source after commit");
    const auto after_new_statement = api::LoadDatabaseLocalSecurityEventStoreV1(reader,
        Visibility::include_reader_own_uncommitted);
    Require(after_new_statement.ok && after_new_statement.state.security_context_generation == fixture.bootstrap_generation + 5 &&
        CountEventKind(after_new_statement.state.events, "ROLE") == 5,
        "fresh security source failed to observe real prepared-writer commit");
    if (page_size == 8192) {
      RetainedSecuritySnapshot late_revoke(reader);
      const auto warm = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, late_revoke.pin);
      Require(warm.ok && !warm.state.events.empty(), "late revocation source warmup failed");
      source_allocation_fault::attempts = 0;
      source_allocation_fault::count = true;
      const auto measured = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, late_revoke.pin);
      source_allocation_fault::count = false;
      Require(measured.ok && source_allocation_fault::attempts, "late revocation source measurement failed");
      // The final owning allocation precedes the source's final, allocation-
      // free pin observation. Publish REAL MGA revocation at that allocation,
      // rather than fabricating a provider callback or caller expiry Boolean.
      source_allocation_fault::revoke_snapshot = late_revoke.id;
      source_allocation_fault::revoke_fired = false;
      source_allocation_fault::revoke_after = static_cast<long>(source_allocation_fault::attempts - 1);
      const auto late = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, late_revoke.pin);
      source_allocation_fault::revoke_after = -1;
      Require(source_allocation_fault::revoke_fired && !late.ok && late.state.events.empty() &&
          late.state.page_numbers.empty() && late.state.security_context_generation == 0 &&
          !late.state.database_identity_authenticated && !late.state.page_identity_authenticated &&
          !late.state.durable_inventory_authenticated &&
          late.diagnostic.code == "SB-MGA-SNAPSHOT-VECTOR-REVOKED" &&
          late.diagnostic.detail == "retained_security_snapshot_revoked_during_read",
          "late real revocation published a materialized security-source prefix");
    }
    for (unsigned octet = 0; octet != 16; ++octet) {
      auto foreign = reader;
      foreign.transaction_uuid.bytes[octet] ^= 0x20;
      const auto refused = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(foreign, original.pin);
      Require(!refused.ok && refused.state.events.empty() && refused.state.page_numbers.empty(),
          "foreign binary transaction observed a retained security source");
      foreign = reader;
      foreign.database_uuid.bytes[octet] ^= 0x20;
      const auto wrong_node = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(foreign, original.pin);
      Require(!wrong_node.ok && wrong_node.state.events.empty() && wrong_node.state.page_numbers.empty(),
          "foreign binary node observed a retained security source");
    }
    const auto wrong_reader = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(*mid_reader, original.pin);
    Require(!wrong_reader.ok && wrong_reader.state.events.empty() &&
        wrong_reader.diagnostic.detail == "retained_security_snapshot_owner_mismatch",
        "another active real transaction adopted foreign security snapshot");
    mga::ReleasePublishedSnapshotVector(original.id);
    Require(!mga::RetainPublishedSnapshotVector(original.id).valid(), "retired security publication issued new pin");
    const auto retired = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, original.pin);
    Require(retired.ok && retired.state.events.empty() &&
        retired.state.security_context_generation == fixture.bootstrap_generation,
        "ordinary public retirement revoked existing security pin");
    mga::RevokePublishedSnapshotVector(middle->id);
    const auto revoked = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(*mid_reader, middle->pin);
    Require(!revoked.ok && revoked.state.events.empty() && revoked.state.page_numbers.empty() &&
        revoked.diagnostic.code == "SB-MGA-SNAPSHOT-VECTOR-REVOKED", "revoked pin retained security authority");
    Finalize(stable, db::PhysicalMgaCowFinalizeDecision::rollback, 1788109700000ull);
    const auto terminal = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(stable, stable_source.pin);
    Require(!terminal.ok && terminal.state.events.empty() && terminal.state.page_numbers.empty(),
        "terminal transaction retained security source read admission");
    CorruptSecurityPage(fixture.database_path, last_page, page_size);
    const auto corrupt_hidden = api::LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(reader, original.pin);
    Require(!corrupt_hidden.ok && corrupt_hidden.state.events.empty() && corrupt_hidden.state.page_numbers.empty() &&
        !corrupt_hidden.state.database_identity_authenticated && !corrupt_hidden.diagnostic.code.empty(),
        "invisible security bytes bypassed authenticated chain validation or exposed partial source");
    std::cout << "retained_security_snapshot_profile=" << page_size << " PASS\n";
  }
}

int main(int argc, char** argv) try {
  if (argc == 2 && std::string_view(argv[1]) == "--retained-snapshots") {
    TestRetainedSecuritySnapshots();
    return EXIT_SUCCESS;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--probe-owner") {
    for (const auto mode : {disk::FileOpenMode::open_existing,
                            disk::FileOpenMode::open_existing_read_only}) {
      disk::FileDevice competing;
      const auto refused = competing.Open(argv[2], mode);
      if (refused.ok() || competing.is_open() ||
          refused.diagnostic.diagnostic_code != "SB-STORAGE-DISK-OWNER-LOCK-HELD") {
        return EXIT_FAILURE;
      }
    }
    return EXIT_SUCCESS;
  }
  Require(argc == 1, "private security test unexpected arguments");
  test_executable = std::filesystem::absolute(argv[0]).string();
  TestPageBackedLifecycle();
  TestLocatorCrashCuts();
  TestMalformedInactiveLocatorRefusal();
  TestSealedLegacyMigration();
  std::cout << "database-local security event store conformance passed\n";
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  source_allocation_fault::budget = -1;
  source_allocation_fault::count = false;
  source_allocation_fault::revoke_after = -1;
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
