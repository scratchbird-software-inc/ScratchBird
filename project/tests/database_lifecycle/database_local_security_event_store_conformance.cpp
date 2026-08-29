// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_local_private_relation_locator.hpp"
#include "security/database_local_security_event_store.hpp"
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
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace core_hash = scratchbird::core::hash;
namespace mga = scratchbird::transaction::mga;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=4ce03aa5a5657aaf221192635ed9c63a"
    "cdb76d78a0994ec6e6ab55286e29e6a5";
constexpr std::string_view kAlicePrincipal =
    "019e108d-1700-7000-8000-0000000007aa";
constexpr std::string_view kSysarchRole =
    "019e108d-1700-7000-8000-0000000007a1";
constexpr std::string_view kPublicGroup =
    "019e108d-1700-7000-8000-0000000007a2";
constexpr std::string_view kRoleMembership =
    "019e108d-1700-7000-8000-0000000007b1";
constexpr std::string_view kGroupMembership =
    "019e108d-1700-7000-8000-0000000007b2";
constexpr std::string_view kConnectGrant =
    "019e108d-1700-7000-8000-0000000007c1";
constexpr std::string_view kConnectDeny =
    "019e108d-1700-7000-8000-0000000007d1";
constexpr std::string_view kRowPolicy =
    "019e108d-1700-7000-8000-0000000007e1";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
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
  std::string pattern = "/tmp/sb_database_local_security.XXXXXX";
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
                      std::uint64_t identity_time) {
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
  create.page_size = 16384;
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
  fixture.context.database_uuid.canonical =
      uuid::UuidToString(database_uuid.value.value);
  fixture.context.database_page_size_bytes = create.page_size;
  fixture.context.principal_uuid.canonical =
      uuid::UuidToString(bootstrap.state.principal_uuid.value);
  fixture.context.security_context_present = true;
  fixture.context.trace_tags.emplace_back(
      api::kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1);
  fixture.bootstrap_generation = bootstrap.state.security_context_generation;
  return fixture;
}

api::EngineRequestContext BeginTransaction(
    const api::EngineRequestContext& base,
    std::uint64_t identity_time) {
  const auto loaded =
      db::LoadLocalTransactionInventoryFromDatabase(base.database_path);
  Require(loaded.ok(), "private security transaction inventory load failed");
  const auto transaction_uuid = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::transaction, identity_time);
  Require(transaction_uuid.ok(),
          "private security transaction identity generation failed");
  const auto begun = mga::BeginLocalTransaction(
      loaded.inventory, transaction_uuid.value, identity_time + 1);
  Require(begun.ok(), "private security transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(
              base.database_path, begun.inventory)
              .ok(),
          "private security active transaction persistence failed");

  auto context = base;
  context.local_transaction_id = begun.entry.identity.local_id.value;
  context.transaction_uuid.canonical =
      uuid::UuidToString(begun.entry.identity.transaction_uuid.value);
  context.snapshot_visible_through_local_transaction_id =
      begun.entry.begin_visible_through_local_transaction_id;
  return context;
}

void Finalize(const api::EngineRequestContext& context,
              db::PhysicalMgaCowFinalizeDecision decision,
              std::uint64_t final_time) {
  db::PhysicalMgaCowFinalizeRequest finalize;
  finalize.database_path = context.database_path;
  finalize.local_transaction_id =
      mga::MakeLocalTransactionId(context.local_transaction_id);
  finalize.decision = decision;
  finalize.final_unix_epoch_millis = final_time;
  const auto finalized = db::FinalizePhysicalMgaCowTransaction(finalize);
  Require(finalized.ok(), "private security transaction finality failed");
}

std::string AuditUuid(std::uint64_t generation) {
  const auto generated = uuid::GenerateDurableEngineIdentityV7(
      UuidKind::object, 1788100000000ull + generation);
  Require(generated.ok(), "private security audit identity generation failed");
  return uuid::UuidToString(generated.value.value);
}

std::vector<std::string> Batch(std::string authority,
                               const api::EngineRequestContext& context,
                               std::uint64_t generation,
                               std::string_view target) {
  return {
      std::move(authority),
      "SBSECPL1\tAUDIT\t" + std::to_string(context.local_transaction_id) +
          "\t" + AuditUuid(generation) +
          "\t746573742e707269766174655f6c6966656379636c65\t" +
          context.principal_uuid.canonical + "\t" + std::string(target) +
          "\tsuccess\t7265646163746564\t" + std::to_string(generation),
      "SBSECPL1\tCACHE_INVALIDATE\t" +
          std::to_string(context.local_transaction_id) +
          "\t746573742e707269766174655f6c6966656379636c65\t" +
          std::string(target) + "\t" + std::to_string(generation),
  };
}

std::string Prefix(const api::EngineRequestContext& context,
                   std::string_view kind) {
  return "SBSECPL1\t" + std::string(kind) + "\t" +
         std::to_string(context.local_transaction_id) + "\t";
}

std::vector<std::string> SingleRoleBatch(
    const api::EngineRequestContext& context,
    std::uint64_t generation,
    std::string_view role_uuid =
        "019e108d-1700-7000-8000-000000000798") {
  return Batch(Prefix(context, "ROLE") + std::string(role_uuid) +
                   "\t746573745f726f6c65\t" +
                   context.principal_uuid.canonical + "\tactive\t" +
                   std::to_string(generation) + "\t0",
               context, generation, role_uuid);
}

std::size_t CountEventKind(const std::vector<std::string>& events,
                           std::string_view kind) {
  const std::string needle = "SBSECPL1\t" + std::string(kind) + "\t";
  return static_cast<std::size_t>(
      std::count_if(events.begin(), events.end(), [&](const std::string& event) {
        return event.starts_with(needle);
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
      const std::string actor =
          uuid::UuidToString(carrier.actor_principal_uuid);
      const auto actor_offset = carrier.events[1].find(actor);
      Require(actor_offset != std::string::npos,
              "private security payload substitution actor missing");
      carrier.events[1][actor_offset] =
          carrier.events[1][actor_offset] == '0' ? '1' : '0';
    }
    std::string payload;
    for (const auto& event : carrier.events) {
      payload.append(event);
      payload.push_back('\n');
    }
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
        Prefix(transaction, "PRINCIPAL") + std::string(kAlicePrincipal) +
            "\t616c696365\tuser\tactive\t66696e6765727072696e74\t" +
            std::to_string(generation) + "\t0",
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
        Prefix(wrong_transaction, "PRINCIPAL") + std::string(kAlicePrincipal) +
            "\t616c696365\tuser\tactive\t66696e6765727072696e74\t" +
            std::to_string(generation) + "\t0",
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
        Prefix(transaction, "PRINCIPAL") + std::string(kAlicePrincipal) +
            "\t616c696365\tuser\tactive\t66696e6765727072696e74\t" +
            std::to_string(generation) + "\t0",
        transaction, generation, kAlicePrincipal);
    rows.insert(rows.begin() + 1,
                Prefix(transaction, "ROLE") + std::string(kSysarchRole) +
                    "\t73797361726368\t" +
                    transaction.principal_uuid.canonical + "\tactive\t" +
                    std::to_string(generation) + "\t0");
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
                appended.sealed_events.back().starts_with(
                    "SBSECPL1\tAUTH_CONTEXT_SUCCESSOR\t") &&
                appended.page_number >= db::kCatalogOverflowFirstPageNumber &&
                appended.page_number > unallocated_page,
            "private security exact append result invalid");
    ++generation;
    page_numbers.push_back(appended.page_number);
  };

  append(Batch(
      Prefix(transaction, "PRINCIPAL") + std::string(kAlicePrincipal) +
          "\t616c696365\tuser\tactive\t66696e6765727072696e74\t" +
          std::to_string(generation + 1) + "\t0",
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
    const auto database_uuid = uuid::ParseDurableEngineIdentityUuid(
        UuidKind::database, fixture.context.database_uuid.canonical);
    const auto transaction_uuid = uuid::ParseDurableEngineIdentityUuid(
        UuidKind::transaction,
        competing_transaction.transaction_uuid.canonical);
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
      Prefix(competing_transaction, "ROLE") +
          "019e108d-1700-7000-8000-000000000799\t636f6e63757272656e74\t" +
          competing_transaction.principal_uuid.canonical + "\tactive\t" +
          std::to_string(generation + 1) + "\t0",
      competing_transaction, generation + 1,
      "019e108d-1700-7000-8000-000000000799");
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
      Prefix(transaction, "ROLE") + std::string(kSysarchRole) +
          "\t73797361726368\t" + transaction.principal_uuid.canonical +
          "\tactive\t" + std::to_string(generation + 1) + "\t0",
      transaction, generation + 1, kSysarchRole));
  append(Batch(
      Prefix(transaction, "GROUP") + std::string(kPublicGroup) +
          "\t5055424c4943\t\tactive\t" + std::to_string(generation + 1) +
          "\t0",
      transaction, generation + 1, kPublicGroup));
  append(Batch(
      Prefix(transaction, "MEMBERSHIP") + std::string(kRoleMembership) +
          "\t" + std::string(kAlicePrincipal) + "\t" +
          std::string(kSysarchRole) + "\trole\t" +
          transaction.principal_uuid.canonical + "\t" +
          std::to_string(generation + 1) + "\t0",
      transaction, generation + 1, kRoleMembership));
  append(Batch(
      Prefix(transaction, "MEMBERSHIP") + std::string(kGroupMembership) +
          "\t" + std::string(kAlicePrincipal) + "\t" +
          std::string(kPublicGroup) + "\tgroup\t" +
          transaction.principal_uuid.canonical + "\t" +
          std::to_string(generation + 1) + "\t0",
      transaction, generation + 1, kGroupMembership));
  append(Batch(
      Prefix(transaction, "GRANT") + std::string(kConnectGrant) + "\t" +
          std::string(kSysarchRole) + "\trole\t\t\tCONNECT\t" +
          transaction.principal_uuid.canonical + "\tallow\t" +
          std::to_string(generation + 1) + "\t0",
      transaction, generation + 1, kConnectGrant));
  append(Batch(
      Prefix(transaction, "GRANT") + std::string(kConnectDeny) + "\t" +
          std::string(kAlicePrincipal) +
          "\tprincipal\t\t\tCONNECT\t" +
          transaction.principal_uuid.canonical + "\tdeny\t" +
          std::to_string(generation + 1) + "\t0",
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
      Prefix(transaction, "ROW_POLICY") + std::string(kRowPolicy) + "\t" +
      std::string(kConnectGrant) + "\trelation\tallow_if\t01020304\t" +
      transaction.principal_uuid.canonical + "\tactive\t" +
      std::to_string(generation + 1) +
      "\t0\t019e108d-1700-7000-8000-0000000007e2\t92\t7\t2\t" +
      std::string(kRowPolicy) +
      "\t3\t019e108d-1700-7000-8000-0000000007e3\t4\t" +
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\t" +
      "019e108d-1700-7000-8000-0000000007e4\t5\t" +
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\t" +
      "019e108d-1700-7000-8000-0000000007e5\t6\t7";
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
      Prefix(rollback_transaction, "ROLE") +
          "019e108d-1700-7000-8000-0000000007e1\t726f6c6c6261636b\t" +
          rollback_transaction.principal_uuid.canonical + "\tactive\t" +
          std::to_string(generation + 1) + "\t0",
      rollback_transaction, generation + 1,
      "019e108d-1700-7000-8000-0000000007e1");
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
  wrong_database.database_uuid.canonical =
      "019e108d-1700-7000-8000-0000000007ff";
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
                          "019e108d-1700-7000-8000-000000000797"));
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

int main() {
  TestPageBackedLifecycle();
  TestLocatorCrashCuts();
  TestMalformedInactiveLocatorRefusal();
  TestSealedLegacyMigration();
  std::cout << "database-local security event store conformance passed\n";
  return EXIT_SUCCESS;
}
