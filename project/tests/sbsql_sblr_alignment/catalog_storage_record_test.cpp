// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../support/diagnostic_value_fixture.hpp"
#include "catalog_storage_record_codec.hpp"
#include "catalog_page.hpp"
#include "database_lifecycle.hpp"
#include "page_header.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace u = scratchbird::core::uuid;
namespace fs = std::filesystem;
unsigned checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
p::Uuid Id(unsigned last = 1) {
  p::Uuid id;
  id.bytes = {0, 10, 13, 32, 61, 255, 0x7f, 0, 0x80, 255, 0, 10, 61, 0, 0,
              static_cast<p::byte>(last)};
  return id;
}
void Put(std::string& s, std::size_t at, p::u64 value, unsigned count) {
  for (unsigned i = 0; i < count; ++i) s[at + i] = static_cast<char>(value >> (8 * i));
}
std::string Golden(const c::CatalogStorageRecord& r) {
  // Independent registered tags, offsets and widths. No production codec calls.
  std::string s(112 + r.descriptor_name.size(), '\0');
  s.replace(0, 4, "SBCV");
  Put(s, 4, 1, 2); Put(s, 6, 24, 2); Put(s, 8, s.size(), 4);
  Put(s, 12, 5, 4); Put(s, 16, 65616, 4); Put(s, 20, 1, 2);
  for (unsigned i = 0; i < 2; ++i) {
    const auto at = 24 + i * 24;
    Put(s, at, i + 1, 2); Put(s, at + 2, 5, 1); Put(s, at + 4, 16, 4);
    const auto& id = i ? r.filespace_uuid : r.descriptor_uuid;
    s.replace(at + 8, 16, reinterpret_cast<const char*>(id.value.bytes.data()), 16);
  }
  for (unsigned i = 0; i < 2; ++i) {
    const auto at = 72 + i * 16;
    Put(s, at, i + 3, 2); Put(s, at + 2, 1, 1); Put(s, at + 4, 8, 4);
    Put(s, at + 8, i ? r.creator_transaction_number : r.page_size, 8);
  }
  Put(s, 104, 5, 2); Put(s, 106, 3, 1); Put(s, 108, r.descriptor_name.size(), 4);
  s.replace(112, r.descriptor_name.size(), r.descriptor_name);
  return s;
}
std::string Bytes(const c::CatalogValueEncodeResult& encoded) {
  Check(encoded.ok(), "valid storage payload refused");
  return {encoded.bytes.begin(), encoded.bytes.end()};
}
void Refused(const std::string& bytes) {
  const auto decoded = c::DecodeCatalogStorageRecord(bytes);
  Check(!decoded.ok() && !decoded.record, "malformed storage payload escaped admission");
}
void Codec() {
  c::CatalogStorageRecord base{{p::UuidKind::object, Id()},
      {p::UuidKind::filespace, Id(2)}, 16384, 1, "default_storage_profile"};
  const auto golden = Golden(base);
  Check(Bytes(c::EncodeCatalogStorageRecord(base)) == golden, "independent golden mismatch");
  for (std::size_t n = 0; n < golden.size(); ++n) Refused(golden.substr(0, n));
  Refused(golden + '\0'); Refused("filespace_uuid=018f7a10-1280-7000-8000-000000000106\n");
  for (unsigned field = 0; field < 2; ++field) {
    for (unsigned position = 0; position < 16; ++position) {
      for (unsigned value = 0; value < 256; ++value) {
        auto r = base;
        auto& id = field ? r.filespace_uuid : r.descriptor_uuid;
        id.value.bytes[position] = value;
        const bool valid = (id.value.bytes[6] >> 4) == 7 && (id.value.bytes[8] >> 6) == 2;
        const auto encoded = c::EncodeCatalogStorageRecord(r);
        if (!valid) {
          Check(!encoded.ok() && encoded.bytes.empty(), "invalid identity encoded");
          Refused(Golden(r));
        } else {
          Check(Bytes(encoded) == Golden(r), "identity bits changed");
          const auto decoded = c::DecodeCatalogStorageRecord(Golden(r));
          Check(decoded.ok() && decoded.record->descriptor_uuid.kind == r.descriptor_uuid.kind &&
                decoded.record->descriptor_uuid.value == r.descriptor_uuid.value &&
                decoded.record->filespace_uuid.kind == r.filespace_uuid.kind &&
                decoded.record->filespace_uuid.value == r.filespace_uuid.value, "native roundtrip changed identity");
        }
      }
    }
    for (unsigned kind = 0; kind < 256; ++kind) {
      auto r = base;
      (field ? r.filespace_uuid : r.descriptor_uuid).kind = static_cast<p::UuidKind>(kind);
      const bool valid = kind == static_cast<unsigned>(field ? p::UuidKind::filespace : p::UuidKind::object);
      Check(c::EncodeCatalogStorageRecord(r).ok() == valid, "wrong UUID domain admitted");
    }
    auto nil = base; (field ? nil.filespace_uuid : nil.descriptor_uuid).value = {};
    Check(!c::EncodeCatalogStorageRecord(nil).ok(), "nil identity admitted"); Refused(Golden(nil));
  }
  for (p::u64 size : {0ull, 4096ull, 8191ull, 8193ull, 262144ull, ~0ull}) {
    auto r = base; r.page_size = size;
    Check(!c::EncodeCatalogStorageRecord(r).ok(), "invalid page size admitted"); Refused(Golden(r));
  }
  for (p::u64 size : {8192, 16384, 32768, 65536, 131072}) {
    auto r = base; r.page_size = size;
    Check(Bytes(c::EncodeCatalogStorageRecord(r)) == Golden(r), "current page profile refused");
  }
  auto r = base; r.creator_transaction_number = 0;
  Check(!c::EncodeCatalogStorageRecord(r).ok(), "zero creator admitted"); Refused(Golden(r));
  r = base; r.descriptor_name.clear(); Check(!c::EncodeCatalogStorageRecord(r).ok(), "empty name admitted");
  r.descriptor_name.assign(4097, 'x'); Check(!c::EncodeCatalogStorageRecord(r).ok(), "oversize name admitted");
  Refused(Golden(r)); r.descriptor_name.assign(4096, 'x');
  Check(Bytes(c::EncodeCatalogStorageRecord(r)) == Golden(r), "name bound narrowed");
  for (const auto offset : {0, 4, 6, 8, 12, 16, 20, 22, 24, 26, 27, 28, 48, 50, 51, 52, 72, 74, 75, 76, 88, 90, 91, 92, 104, 106, 107, 108}) {
    auto bytes = golden; bytes[offset] ^= 0x40; Refused(bytes);
  }
  c::CatalogTypedRecord outer;
  outer.header.kind = c::CatalogRecordKind::storage_descriptor;
  outer.header.object_uuid = base.descriptor_uuid;
  outer.header.row_uuid = {p::UuidKind::row, Id(3)};
  outer.header.parent_uuid = {p::UuidKind::object, Id(4)};
  outer.payload = golden;
  auto encoded = c::EncodeCatalogTypedRecord(outer, 1);
  Check(encoded.ok() && c::DecodeCatalogTypedRecord(encoded.row).ok(), "valid common header refused");
  encoded.row.payload[71] ^= 1;
  Check(!c::DecodeCatalogTypedRecord(encoded.row).ok(), "persisted owner mismatch admitted");
  outer.header.object_uuid.value = Id(5);
  Check(!c::EncodeCatalogTypedRecord(outer, 1).ok(), "owner mismatch encoded");
  outer.header.object_uuid = base.descriptor_uuid; outer.header.row_uuid.value = base.descriptor_uuid.value;
  Check(!c::EncodeCatalogTypedRecord(outer, 1).ok(), "row reused as storage owner");
}
std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary); Check(in.is_open(), "read fixture");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
p::u64 Fnv(const std::string& s) {
  p::u64 value = 1469598103934665603ull;
  for (unsigned char b : s) { value ^= b; value *= 1099511628211ull; }
  return value;
}
void Durable(const fs::path& root, p::u64 millis) {
  db::DatabaseCreateConfig cfg;
  cfg.path = (root / "primary.sdb").string(); cfg.page_size = 16384;
  cfg.creation_unix_epoch_millis = millis;
  cfg.database_uuid = u::GenerateEngineIdentityV7(p::UuidKind::database, millis).value;
  cfg.filespace_uuid = u::GenerateEngineIdentityV7(p::UuidKind::filespace, millis).value;
  cfg.allow_minimal_resource_bootstrap = true; cfg.require_resource_seed_pack = false;
  Check(db::CreateDatabaseFile(cfg).ok(), "actual create failed");
  db::DatabaseOpenConfig open; open.path = cfg.path; open.read_only = true; open.suppress_background_agents = true;
  Check(db::OpenDatabaseFile(open).ok(), "created node failed reopen");
  p::u64 found_page = 0; std::size_t found_offset = 0;
  std::vector<p::byte> original;
  c::CatalogStorageRecord expected;
  {
    disk::FileDevice device; Check(device.Open(cfg.path, disk::FileOpenMode::open_existing).ok(), "open device");
    auto number = db::kCatalogPageNumber;
    for (unsigned guard = 0; number && guard < 1000; ++guard) {
      std::vector<p::byte> body(cfg.page_size - disk::kPageHeaderSerializedBytes);
      Check(device.ReadAt(number * cfg.page_size + disk::kPageHeaderSerializedBytes, body.data(), body.size()).ok(), "read page");
      const auto parsed = page::ParseCatalogPageBody(body, number); Check(parsed.ok(), "parse page");
      std::size_t offset = page::kCatalogPageBodyHeaderBytes;
      for (const auto& row : parsed.body.rows) {
        if (row.kind == page::CatalogPageRowKind::typed_catalog_record) {
          const auto record = c::DecodeCatalogTypedRecord(row); Check(record.ok(), "decode persisted catalog");
          if (record.record.header.kind == c::CatalogRecordKind::storage_descriptor) {
            Check(!found_page, "duplicate storage descriptor");
            found_page = number; found_offset = offset; original = body;
            expected = {record.record.header.object_uuid, cfg.filespace_uuid, cfg.page_size, 1, "default_storage_profile"};
            Check(record.record.payload == Golden(expected), "persisted storage binding differs from independent oracle");
            Check(record.record.payload.find(u::UuidToString(cfg.filespace_uuid.value)) == std::string::npos,
                  "persisted filespace identity is still text");
          }
        }
        offset += 20 + row.payload.size();
      }
      number = parsed.body.next_page_number;
    }
  }
  Check(found_page != 0, "storage descriptor missing");
  const auto write = [&](const auto& body) {
    disk::FileDevice device; Check(device.Open(cfg.path, disk::FileOpenMode::open_existing).ok(), "tamper device open");
    Check(device.WriteAt(found_page * cfg.page_size + disk::kPageHeaderSerializedBytes, body.data(), body.size()).ok(), "write tamper");
    Check(device.Sync().ok(), "sync tamper");
  };
  const auto mutate = [&](const std::string& payload, bool wrong_parent = false, bool missing = false) {
    auto body = original;
    const auto size = p::LoadLittle32(body.data() + found_offset + 8);
    std::string record(reinterpret_cast<const char*>(body.data() + found_offset + 20), size);
    Check(record.size() == 96 + payload.size(), "tamper changes length");
    record.replace(96, payload.size(), payload);
    if (wrong_parent) record[87] ^= 1;
    if (missing) Put(record, 16, static_cast<unsigned>(c::CatalogRecordKind::toast_reference), 2);
    std::copy(record.begin(), record.end(), body.begin() + found_offset + 20);
    p::StoreLittle64(body.data() + found_offset + 12, Fnv(record));
    p::StoreLittle64(body.data() + 40, page::ComputeCatalogPageBodyChecksum(body));
    Check(page::ParseCatalogPageBody(body, found_page).ok(), "tamper checksum is invalid");
    write(body); const auto before = ReadAll(cfg.path);
    Check(!db::OpenDatabaseFile(open).ok(), "tampered node opened read-only");
    Check(ReadAll(cfg.path) == before, "read-only refusal changed node");
    auto writable = open; writable.read_only = false;
    Check(!db::OpenDatabaseFile(writable).ok(), "tampered node opened writable");
    Check(ReadAll(cfg.path) == before, "writable refusal changed node");
    write(original); Check(db::OpenDatabaseFile(open).ok(), "restored node failed reopen");
  };
  auto wrong = expected; wrong.filespace_uuid.value.bytes[15] ^= 1; mutate(Golden(wrong));
  wrong = expected; wrong.descriptor_uuid.value.bytes[15] ^= 1; mutate(Golden(wrong));
  wrong = expected; wrong.page_size = 32768; mutate(Golden(wrong));
  wrong = expected; wrong.creator_transaction_number = 2; mutate(Golden(wrong));
  auto bytes = Golden(expected); bytes[56 + 6] &= 15; mutate(bytes);
  mutate(Golden(expected), true);
  mutate(Golden(expected), false, true);
  // A second otherwise valid owner must not select or alias the primary by
  // presentation name. Replace one unrelated sufficiently large annotation
  // record in this page, then require the exact duplicate-binding diagnostic.
  auto duplicate_page = page::ParseCatalogPageBody(original, found_page);
  Check(duplicate_page.ok(), "duplicate fixture page");
  bool replaced = false;
  for (auto& row : duplicate_page.body.rows) {
    if (row.kind != page::CatalogPageRowKind::typed_catalog_record) continue;
    auto record = c::DecodeCatalogTypedRecord(row);
    Check(record.ok(), "duplicate fixture record");
    if (record.record.header.kind != c::CatalogRecordKind::policy &&
        record.record.header.kind != c::CatalogRecordKind::domain) continue;
    auto duplicate = expected;
    duplicate.descriptor_uuid = record.record.header.object_uuid;
    auto payload = Golden(duplicate);
    if (row.payload.size() < 96 + payload.size()) continue;
    record.record.header.kind = c::CatalogRecordKind::storage_descriptor;
    // Obtain the genuine parent from the original storage record.
    const auto original_size = p::LoadLittle32(original.data() + found_offset + 8);
    page::CatalogPageRow storage_row;
    storage_row.kind = page::CatalogPageRowKind::typed_catalog_record;
    storage_row.payload.assign(reinterpret_cast<const char*>(original.data() + found_offset + 20), original_size);
    record.record.header.parent_uuid = c::DecodeCatalogTypedRecord(storage_row).record.header.parent_uuid;
    record.record.payload = std::move(payload);
    const auto encoded = c::EncodeCatalogTypedRecord(record.record, row.ordinal);
    Check(encoded.ok(), "duplicate owner fixture encode");
    row = encoded.row; replaced = true; break;
  }
  Check(replaced, "duplicate fixture needs an unrelated annotation record");
  auto rebuilt = page::BuildCatalogPageSet(duplicate_page.body.rows, cfg.page_size, found_page, found_page + 1);
  Check(rebuilt.ok() && rebuilt.pages.size() == 1, "duplicate fixture must fit original page");
  auto body = rebuilt.pages[0].body;
  p::StoreLittle32(body.data() + 16, duplicate_page.body.page_sequence);
  p::StoreLittle64(body.data() + 32, duplicate_page.body.next_page_number);
  p::StoreLittle64(body.data() + 40, page::ComputeCatalogPageBodyChecksum(body));
  Check(page::ParseCatalogPageBody(body, found_page).ok(), "duplicate fixture checksum");
  write(body);
  const auto before = ReadAll(cfg.path);
  for (bool read_only : {true, false}) {
    auto request = open; request.read_only = read_only;
    const auto refused = db::OpenDatabaseFile(request);
    Check(!refused.ok() && refused.diagnostic.diagnostic_code ==
          "SB-DB-LIFECYCLE-FILESPACE-MANIFEST-FIELD-MISMATCH", "duplicate descriptor not refused by manifest gate");
    Check(std::any_of(refused.diagnostic.arguments.begin(), refused.diagnostic.arguments.end(),
        [](const auto& argument) { return scratchbird::tests::DiagnosticTextEquals(argument.value, "primary_storage_descriptor_count"); }),
        "duplicate refused for unrelated fixture mutation instead of duplicate binding");
    Check(ReadAll(cfg.path) == before, "duplicate refusal changed node");
  }
  write(original); Check(db::OpenDatabaseFile(open).ok(), "duplicate fixture restoration");
}
int main() {
  fs::path root;
  bool owns_root = false;
  try {
    Codec();
    const auto millis = static_cast<p::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto id = u::GenerateEngineIdentityV7(p::UuidKind::object, millis);
    Check(id.ok(), "workspace identity");
    root = fs::temp_directory_path() / ("sb_storage_payload_" + u::UuidToString(id.value.value));
    owns_root = fs::create_directory(root);
    Check(owns_root, "unique private test directory");
    Durable(root, millis);
    fs::remove_all(root);
    std::cout << "PASS " << checks << " checks;8192 UUID octet cases;real create/reopen/tamper\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    if (owns_root) fs::remove_all(root);
    return 1;
  }
}
