// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_record_codec.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>

namespace cat = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
using p::UuidKind;
using Header = cat::CatalogRecordHeader;
constexpr std::array<p::TypedUuid Header::*, 3> fields = {
    &Header::row_uuid, &Header::object_uuid, &Header::parent_uuid};
constexpr std::array durable_kinds = {
    UuidKind::database, UuidKind::cluster, UuidKind::filespace, UuidKind::schema,
    UuidKind::object, UuidKind::row, UuidKind::page, UuidKind::transaction,
    UuidKind::principal};
unsigned checks = 0, failures = 0, cases = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition && ++failures <= 12) std::cerr << "FAIL " << message << '\n';
}
void Refused(const cat::CatalogRecordCodecResult& result) {
  Check(!result.ok(), "malformed supplied catalog UUID encoded successfully");
  Check(!result.diagnostic.diagnostic_code.empty(), "missing catalog refusal diagnostic");
  Check(result.row.payload.empty() && result.record.header.kind == cat::CatalogRecordKind::unknown,
        "failed catalog encode published partial authority");
}
void Put(std::string& out, std::size_t offset, p::u64 value, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i)
    out[offset + i] = static_cast<char>((value >> (i * 8)) & 255);
}
std::string Golden(const cat::CatalogTypedRecord& record) {
  // Independent Core offset oracle; no production encoder/integer helpers.
  std::string out(96 + record.payload.size(), '\0');
  out.replace(0, 8, "SBCTREC2");
  Put(out, 8, 2, 2); Put(out, 10, 96, 2); Put(out, 12, out.size(), 4);
  Put(out, 16, static_cast<unsigned>(record.header.kind), 2);
  Put(out, 20, record.header.record_version, 4);
  Put(out, 24, record.header.deleted ? 1 : 0, 4);
  Put(out, 28, record.payload.size(), 4);
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const auto& id = record.header.*fields[i];
    out[32 + i] = static_cast<char>(id.kind);
    out.replace(40 + i * 16, 16, reinterpret_cast<const char*>(id.value.bytes.data()), 16);
  }
  out.replace(96, record.payload.size(), record.payload);
  return out;
}
void Roundtrip(const cat::CatalogTypedRecord& record) {
  const auto encoded = cat::EncodeCatalogTypedRecord(record, 17);
  Check(encoded.ok(), "valid catalog header refused");
  if (!encoded.ok()) return;
  Check(encoded.row.payload == Golden(record), "writer differs from independent full-byte oracle");
  const auto decoded = cat::DecodeCatalogTypedRecord(encoded.row);
  Check(decoded.ok(), "writer emitted a record its decoder rejects");
  if (!decoded.ok()) return;
  Check(decoded.row.ordinal == 17 && decoded.record.header.kind == record.header.kind &&
            decoded.record.header.deleted == record.header.deleted &&
            decoded.record.header.record_version == record.header.record_version &&
            decoded.record.payload == record.payload,
        "valid record metadata or ordinary payload changed");
  for (const auto field : fields) {
    Check((decoded.record.header.*field).value == (record.header.*field).value &&
              (decoded.record.header.*field).kind == (record.header.*field).kind,
          "catalog reference bytes or kind substituted or discarded");
  }
  Check(decoded.row.payload == encoded.row.payload, "canonical binary encoding changed on roundtrip");
  Check(encoded.row.payload.size() >= 96 && encoded.row.payload.substr(0, 8) == "SBCTREC2",
        "writer still uses text identity header");
}
void DecodeAdmission(const cat::CatalogTypedRecord& base) {
  auto row = cat::EncodeCatalogTypedRecord(base, 17).row;
  const auto valid = row.payload;
  for (std::size_t length = 0; length < valid.size(); ++length) {
    row.payload = valid.substr(0, length);
    Refused(cat::DecodeCatalogTypedRecord(row));
  }
  row.payload = valid + "tail"; Refused(cat::DecodeCatalogTypedRecord(row));
  row.payload = "kind=1\nrecord_version=1\nrow_uuid=old\n" + std::string(100, 'x');
  Refused(cat::DecodeCatalogTypedRecord(row));
  const auto bad = [&](std::size_t offset, p::u64 value, std::size_t count) {
    row.payload = valid;
    Put(row.payload, offset, value, count);
    Refused(cat::DecodeCatalogTypedRecord(row));
  };
  for (std::size_t offset = 0; offset < 8; ++offset) bad(offset, 0, 1);
  bad(8, 1, 2); bad(10, 95, 2); bad(12, 0, 4); bad(12, 0xffffffffu, 4);
  bad(16, 0xffffu, 2); bad(18, 1, 2); bad(20, 0, 4); bad(20, 2, 4);
  bad(24, 2, 4); bad(28, 0xffffffffu, 4);
  for (std::size_t offset : {35, 36, 37, 38, 39, 88, 89, 90, 91, 92, 93, 94, 95})
    bad(offset, 1, 1);
  for (unsigned slot = 0; slot < 3; ++slot) {
    for (unsigned kind = 0; kind < 256; ++kind) {
      const auto typed = static_cast<UuidKind>(kind);
      const bool accepted = slot == 0 ? typed == UuidKind::row :
          slot == 1 ? typed == UuidKind::object :
          std::find(durable_kinds.begin(), durable_kinds.end(), typed) != durable_kinds.end();
      row.payload = valid;
      Put(row.payload, 32 + slot, kind, 1);
      const auto decoded = cat::DecodeCatalogTypedRecord(row);
      if (!accepted) Refused(decoded);
      else Check(decoded.ok() && (decoded.record.header.*fields[slot]).kind == typed,
                 "valid decoded durable kind changed");
    }
    for (unsigned version = 0; version < 16; ++version)
      for (unsigned variant = 0; variant < 4; ++variant) {
        if (version == 7 && variant == 2) continue;
        row.payload = valid;
        const auto offset = 40 + slot * 16;
        row.payload[offset + 6] = static_cast<char>((static_cast<unsigned char>(valid[offset + 6]) & 15) | version << 4);
        row.payload[offset + 8] = static_cast<char>((static_cast<unsigned char>(valid[offset + 8]) & 63) | variant << 6);
        Refused(cat::DecodeCatalogTypedRecord(row));
      }
    row.payload = valid;
    row.payload.replace(40 + slot * 16, 16, std::string(16, '\0'));
    Refused(cat::DecodeCatalogTypedRecord(row));
  }
}

void PageContainer(const cat::CatalogTypedRecord& record) {
  namespace page = scratchbird::storage::page;
  const auto row = cat::EncodeCatalogTypedRecord(record, 17);
  const auto pages = page::BuildCatalogPageSet({row.row}, 16384, 10, 11);
  Check(pages.ok() && pages.pages.size() == 1, "binary record does not pack into real catalog page");
  if (!pages.ok() || pages.pages.size() != 1) return;
  const auto parsed = page::ParseCatalogPageBody(pages.pages.front().body, 10);
  Check(parsed.ok() && parsed.body.rows.size() == 1, "real catalog page failed to parse");
  if (!parsed.ok() || parsed.body.rows.size() != 1) return;
  Check(parsed.body.rows[0].payload == row.row.payload, "catalog page changed binary header or payload");
  const auto decoded = cat::DecodeCatalogTypedRecord(parsed.body.rows[0]);
  Check(decoded.ok() && decoded.record.payload == record.payload, "page-to-record payload changed");
  auto corrupted = pages.pages.front().body;
  corrupted[page::kCatalogPageBodyHeaderBytes + 20 + 40] ^= 1;
  Check(!page::ParseCatalogPageBody(corrupted, 10).ok(), "enclosing page accepted altered identity");
}

int main() {
  const auto millis = static_cast<p::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  cat::CatalogTypedRecord base;
  for (unsigned slot = 0; slot < fields.size(); ++slot) {
    const auto generated = uuid::GenerateEngineIdentityV7(slot == 0 ? UuidKind::row : UuidKind::object, millis);
    if (!generated.ok()) return 2;
    base.header.*fields[slot] = generated.value;
  }
  base.payload = std::string("ordinary=value\r\nwith\0binary", 27);
  const auto& descriptors = cat::BuiltinCatalogRecordDescriptors();
  for (const auto& descriptor : descriptors) {
    base.header.kind = descriptor.kind;
    Roundtrip(base);
    DecodeAdmission(base);
    PageContainer(base);
    auto absent = base;
    if (!descriptor.requires_object_uuid) absent.header.object_uuid = {};
    if (!descriptor.requires_parent_uuid) absent.header.parent_uuid = {};
    Roundtrip(absent);
    for (unsigned slot = 0; slot < fields.size(); ++slot) {
      for (unsigned version = 0; version < 16; ++version) {
        for (unsigned variant = 0; variant < 4; ++variant) {
          ++cases;
          auto record = base;
          auto& value = (record.header.*fields[slot]).value;
          value.bytes[6] = static_cast<p::byte>((value.bytes[6] & 15) | (version << 4));
          value.bytes[8] = static_cast<p::byte>((value.bytes[8] & 63) | (variant << 6));
          if (version == 7 && variant == 2) Roundtrip(record);
          else Refused(cat::EncodeCatalogTypedRecord(record, 17));
        }
      }
      for (unsigned raw = 0; raw < 256; ++raw) {
        ++cases;
        const auto kind = static_cast<UuidKind>(raw);
        auto record = base;
        (record.header.*fields[slot]).kind = kind;
        const bool accepted = slot == 0 ? kind == UuidKind::row : slot == 1 ? kind == UuidKind::object :
            std::find(durable_kinds.begin(), durable_kinds.end(), kind) != durable_kinds.end();
        if (accepted) Roundtrip(record);
        else Refused(cat::EncodeCatalogTypedRecord(record, 17));
      }
      ++cases;
      auto nil = base;
      (nil.header.*fields[slot]).value = {};
      Refused(cat::EncodeCatalogTypedRecord(nil, 17));
    }
    // UUID-looking ordinary payloads are not header authority.
    for (unsigned version = 1; version <= 7; ++version) {
      auto record = base;
      auto user = base.header.object_uuid.value;
      user.bytes[6] = static_cast<p::byte>((user.bytes[6] & 15) | (version << 4));
      record.payload = "ordinary_uuid=" + uuid::UuidToString(user);
      record.header.deleted = true;
      Roundtrip(record);
    }
  }
  base.payload.clear(); Roundtrip(base);
  base.payload.assign(131072 - 96, '\0'); Roundtrip(base);
  base.payload.push_back('x'); Refused(cat::EncodeCatalogTypedRecord(base, 17));
  std::cout << "checks=" << checks << " failures=" << failures << " record_kinds=" << descriptors.size()
            << " header_cases=" << cases << '\n';
  return failures == 0 ? 0 : 1;
}
