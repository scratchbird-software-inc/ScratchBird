// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_specialized_pages.hpp"

#include <cstring>

namespace scratchbird::storage::page {
namespace {
using scratchbird::core::platform::HostToLittle32;
using scratchbird::core::platform::HostToLittle64;
using scratchbird::core::platform::LittleToHost32;
using scratchbird::core::platform::LittleToHost64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::storage_page}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page}; }

void Store32(std::vector<byte>* out, u32 value) {
  value = HostToLittle32(value);
  const auto* ptr = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), ptr, ptr + sizeof(value));
}
void Store64(std::vector<byte>* out, u64 value) {
  value = HostToLittle64(value);
  const auto* ptr = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), ptr, ptr + sizeof(value));
}
void StoreUuid(std::vector<byte>* out, const TypedUuid& uuid) {
  out->push_back(static_cast<byte>(uuid.kind));
  out->insert(out->end(), uuid.value.bytes.begin(), uuid.value.bytes.end());
}
u32 Load32(const std::vector<byte>& in, std::size_t* offset) {
  u32 value = 0;
  std::memcpy(&value, in.data() + *offset, sizeof(value));
  *offset += sizeof(value);
  return LittleToHost32(value);
}
u64 Load64(const std::vector<byte>& in, std::size_t* offset) {
  u64 value = 0;
  std::memcpy(&value, in.data() + *offset, sizeof(value));
  *offset += sizeof(value);
  return LittleToHost64(value);
}
TypedUuid LoadUuid(const std::vector<byte>& in, std::size_t* offset) {
  TypedUuid uuid;
  uuid.kind = static_cast<scratchbird::core::platform::UuidKind>(in[*offset]);
  *offset += 1;
  std::memcpy(uuid.value.bytes.data(), in.data() + *offset, uuid.value.bytes.size());
  *offset += uuid.value.bytes.size();
  return uuid;
}
}  // namespace

IndexSpecializedPageBodyResult BuildIndexSpecializedPageBody(const IndexSpecializedPageBody& body, u32 page_size) {
  IndexSpecializedPageBodyResult result;
  IndexPageFamilyHeaderResult header = BuildIndexPageFamilyHeader(body.header);
  if (!header.ok()) {
    result.status = header.status;
    result.diagnostic = header.diagnostic;
    return result;
  }
  result.serialized = header.serialized;
  Store32(&result.serialized, static_cast<u32>(body.entries.size()));
  for (const auto& entry : body.entries) {
    Store32(&result.serialized, entry.entry_kind);
    Store32(&result.serialized, entry.ordinal);
    StoreUuid(&result.serialized, entry.object_uuid);
    Store64(&result.serialized, entry.numeric_value_a);
    Store64(&result.serialized, entry.numeric_value_b);
    Store32(&result.serialized, static_cast<u32>(entry.payload.size()));
    result.serialized.insert(result.serialized.end(), entry.payload.begin(), entry.payload.end());
  }
  if (result.serialized.size() > page_size) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexSpecializedPageDiagnostic(result.status, "INDEX.PAGE.SPECIALIZED_TOO_LARGE", "index.page.specialized_too_large");
    return result;
  }
  result.serialized.resize(page_size, 0);
  result.status = OkStatus();
  result.body = body;
  return result;
}

IndexSpecializedPageBodyResult ParseIndexSpecializedPageBody(const std::vector<byte>& serialized) {
  IndexSpecializedPageBodyResult result;
  IndexPageFamilyHeaderResult header = ParseIndexPageFamilyHeader(serialized);
  if (!header.ok()) {
    result.status = header.status;
    result.diagnostic = header.diagnostic;
    return result;
  }
  std::size_t offset = kIndexPageFamilyHeaderBytes;
  if (serialized.size() < offset + sizeof(u32)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexSpecializedPageDiagnostic(result.status, "INDEX.PAGE.SPECIALIZED_TRUNCATED", "index.page.specialized_truncated");
    return result;
  }
  const u32 count = Load32(serialized, &offset);
  result.body.header = header.header;
  for (u32 i = 0; i < count; ++i) {
    if (serialized.size() < offset + 4 + 4 + 17 + 8 + 8 + 4) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexSpecializedPageDiagnostic(result.status, "INDEX.PAGE.SPECIALIZED_TRUNCATED", "index.page.specialized_truncated");
      return result;
    }
    IndexSpecializedPageEntry entry;
    entry.entry_kind = Load32(serialized, &offset);
    entry.ordinal = Load32(serialized, &offset);
    entry.object_uuid = LoadUuid(serialized, &offset);
    entry.numeric_value_a = Load64(serialized, &offset);
    entry.numeric_value_b = Load64(serialized, &offset);
    const u32 payload_size = Load32(serialized, &offset);
    if (serialized.size() < offset + payload_size) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexSpecializedPageDiagnostic(result.status, "INDEX.PAGE.SPECIALIZED_PAYLOAD_TRUNCATED", "index.page.specialized_payload_truncated");
      return result;
    }
    entry.payload.assign(serialized.begin() + static_cast<std::ptrdiff_t>(offset),
                         serialized.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
    offset += payload_size;
    result.body.entries.push_back(std::move(entry));
  }
  result.status = OkStatus();
  result.serialized = serialized;
  return result;
}

DiagnosticRecord MakeIndexSpecializedPageDiagnostic(Status status, std::string diagnostic_code,
                                                    std::string message_key, std::string detail) {
  return MakeDiagnostic(status.code, status.severity, status.subsystem, std::move(diagnostic_code),
                        std::move(message_key), detail.empty() ? std::vector<scratchbird::core::platform::DiagnosticArgument>{}
                                                               : std::vector<scratchbird::core::platform::DiagnosticArgument>{{"detail", std::move(detail)}},
                        {}, "storage.page.index_specialized");
}

}  // namespace scratchbird::storage::page
