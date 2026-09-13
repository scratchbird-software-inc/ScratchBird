// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_page.hpp"

#include "database_format.hpp"
#include "page_header.hpp"
#include "disk_device.hpp"
#include "hash_digest_parts.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kCatalogMagic = {'S', 'B', 'C', 'A', 'T', '0', '0', '1'};
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetFormatMajor = 8;
inline constexpr u32 kOffsetFormatMinor = 10;
inline constexpr u32 kOffsetHeaderBytes = 12;
inline constexpr u32 kOffsetPageSequence = 16;
inline constexpr u32 kOffsetRowCount = 20;
inline constexpr u32 kOffsetBodyBytes = 24;
inline constexpr u32 kOffsetNextPageNumber = 32;
inline constexpr u32 kOffsetBodyChecksum = 40;

inline constexpr u32 kRowHeaderBytes = 20;
inline constexpr u32 kRowOffsetKind = 0;
inline constexpr u32 kRowOffsetFlags = 2;
inline constexpr u32 kRowOffsetOrdinal = 4;
inline constexpr u32 kRowOffsetPayloadBytes = 8;
inline constexpr u32 kRowOffsetPayloadChecksum = 12;

Status CatalogPageOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status CatalogPageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

CatalogPageBodyResult CatalogPageBodyError(std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {}) {
  CatalogPageBodyResult result;
  result.status = CatalogPageErrorStatus();
  result.diagnostic = MakeCatalogPageDiagnostic(result.status,
                                                std::move(diagnostic_code),
                                                std::move(message_key),
                                                std::move(detail));
  return result;
}

CatalogPageSetResult CatalogPageSetError(std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail = {}) {
  CatalogPageSetResult result;
  result.status = CatalogPageErrorStatus();
  result.diagnostic = MakeCatalogPageDiagnostic(result.status,
                                                std::move(diagnostic_code),
                                                std::move(message_key),
                                                std::move(detail));
  return result;
}

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 Fnv1a64(const std::string& payload) {
  return Fnv1a64(reinterpret_cast<const byte*>(payload.data()), payload.size());
}

std::vector<byte> SerializeBody(const CatalogPageBody& body, u32 page_size) {
  std::vector<byte> result(page_size - scratchbird::storage::disk::kPageHeaderSerializedBytes, 0);
  std::copy(kCatalogMagic.begin(), kCatalogMagic.end(), result.begin() + kOffsetMagic);
  StoreLittle16(result.data() + kOffsetFormatMajor, kCatalogPageBodyFormatMajor);
  StoreLittle16(result.data() + kOffsetFormatMinor, kCatalogPageBodyFormatMinor);
  StoreLittle32(result.data() + kOffsetHeaderBytes, kCatalogPageBodyHeaderBytes);
  StoreLittle32(result.data() + kOffsetPageSequence, body.page_sequence);
  StoreLittle32(result.data() + kOffsetRowCount, static_cast<u32>(body.rows.size()));
  StoreLittle64(result.data() + kOffsetNextPageNumber, body.next_page_number);

  u32 offset = kCatalogPageBodyHeaderBytes;
  for (const CatalogPageRow& row : body.rows) {
    StoreLittle16(result.data() + offset + kRowOffsetKind, static_cast<u16>(row.kind));
    StoreLittle16(result.data() + offset + kRowOffsetFlags, 0);
    StoreLittle32(result.data() + offset + kRowOffsetOrdinal, row.ordinal);
    StoreLittle32(result.data() + offset + kRowOffsetPayloadBytes, static_cast<u32>(row.payload.size()));
    StoreLittle64(result.data() + offset + kRowOffsetPayloadChecksum, Fnv1a64(row.payload));
    offset += kRowHeaderBytes;
    std::memcpy(result.data() + offset, row.payload.data(), row.payload.size());
    offset += static_cast<u32>(row.payload.size());
  }

  StoreLittle32(result.data() + kOffsetBodyBytes, offset);
  StoreLittle64(result.data() + kOffsetBodyChecksum, ComputeCatalogPageBodyChecksum(result));
  return result;
}

u32 SerializedRowBytes(const CatalogPageRow& row) {
  return kRowHeaderBytes + static_cast<u32>(row.payload.size());
}

}  // namespace

const char* CatalogPageRowKindName(CatalogPageRowKind kind) {
  switch (kind) {
    case CatalogPageRowKind::resource_seed_pack: return "resource_seed_pack";
    case CatalogPageRowKind::resource_seed_artifact: return "resource_seed_artifact";
    case CatalogPageRowKind::resource_family_summary: return "resource_family_summary";
    case CatalogPageRowKind::typed_catalog_record: return "typed_catalog_record";
    case CatalogPageRowKind::policy_seed_pack: return "policy_seed_pack";
    case CatalogPageRowKind::charset_record: return "charset_record";
    case CatalogPageRowKind::charset_alias_record: return "charset_alias_record";
    case CatalogPageRowKind::collation_record: return "collation_record";
    case CatalogPageRowKind::collation_tailoring_record: return "collation_tailoring_record";
    case CatalogPageRowKind::timezone_record: return "timezone_record";
    case CatalogPageRowKind::timezone_transition_record: return "timezone_transition_record";
    case CatalogPageRowKind::timezone_leap_second_record: return "timezone_leap_second_record";
    case CatalogPageRowKind::bootstrap_object: return "bootstrap_object";
    case CatalogPageRowKind::cluster_catalog_record: return "cluster_catalog_record";
    case CatalogPageRowKind::unknown: return "unknown";
  }
  return "unknown";
}

u64 ComputeCatalogPageBodyChecksum(const std::vector<byte>& body) {
  std::vector<byte> normalized = body;
  if (normalized.size() >= kOffsetBodyChecksum + sizeof(u64)) {
    StoreLittle64(normalized.data() + kOffsetBodyChecksum, 0);
  }
  return Fnv1a64(normalized.data(), normalized.size());
}

CatalogPageSetResult BuildCatalogPageSet(const std::vector<CatalogPageRow>& rows,
                                         u32 page_size,
                                         u64 first_page_number,
                                         u64 overflow_first_page_number) {
  if (page_size <= scratchbird::storage::disk::kPageHeaderSerializedBytes + kCatalogPageBodyHeaderBytes) {
    return CatalogPageSetError("SB-CATALOG-PAGE-BODY-PAGE-SIZE-TOO-SMALL",
                               "storage.catalog_page_body.page_size_too_small",
                               std::to_string(page_size));
  }
  if (first_page_number == 0 || overflow_first_page_number == 0) {
    return CatalogPageSetError("SB-CATALOG-PAGE-BODY-PAGE-NUMBER-INVALID",
                               "storage.catalog_page_body.page_number_invalid");
  }

  const u32 capacity = page_size - scratchbird::storage::disk::kPageHeaderSerializedBytes;
  std::vector<CatalogPageBody> bodies;
  CatalogPageBody current;
  current.page_sequence = 0;
  current.page_number = first_page_number;
  u32 used = kCatalogPageBodyHeaderBytes;

  for (const CatalogPageRow& row : rows) {
    if (row.kind == CatalogPageRowKind::unknown) {
      return CatalogPageSetError("SB-CATALOG-PAGE-BODY-ROW-KIND-UNKNOWN",
                                 "storage.catalog_page_body.row_kind_unknown");
    }
    const u32 row_bytes = SerializedRowBytes(row);
    if (row_bytes + kCatalogPageBodyHeaderBytes > capacity) {
      return CatalogPageSetError("SB-CATALOG-PAGE-BODY-ROW-TOO-LARGE",
                                 "storage.catalog_page_body.row_too_large",
                                 std::to_string(row.ordinal));
    }
    if (used + row_bytes > capacity) {
      bodies.push_back(std::move(current));
      current = CatalogPageBody{};
      current.page_sequence = static_cast<u32>(bodies.size());
      current.page_number = overflow_first_page_number + current.page_sequence - 1;
      used = kCatalogPageBodyHeaderBytes;
    }
    current.rows.push_back(row);
    used += row_bytes;
  }
  bodies.push_back(std::move(current));

  for (std::size_t i = 0; i < bodies.size(); ++i) {
    bodies[i].next_page_number = (i + 1 < bodies.size()) ? bodies[i + 1].page_number : 0;
  }

  CatalogPageSetResult result;
  result.status = CatalogPageOkStatus();
  for (const CatalogPageBody& body : bodies) {
    SerializedCatalogPageBody serialized;
    serialized.page_number = body.page_number;
    serialized.next_page_number = body.next_page_number;
    serialized.body = SerializeBody(body, page_size);
    result.pages.push_back(std::move(serialized));
  }
  return result;
}

CatalogPageBodyResult ParseCatalogPageBody(const std::vector<byte>& body, u64 page_number) {
  if (body.size() < kCatalogPageBodyHeaderBytes) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-SHORT",
                                "storage.catalog_page_body.short",
                                std::to_string(page_number));
  }
  if (!std::equal(kCatalogMagic.begin(), kCatalogMagic.end(), body.begin() + kOffsetMagic)) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-MAGIC-INVALID",
                                "storage.catalog_page_body.magic_invalid",
                                std::to_string(page_number));
  }
  const u16 format_major = LoadLittle16(body.data() + kOffsetFormatMajor);
  const u16 format_minor = LoadLittle16(body.data() + kOffsetFormatMinor);
  if (format_major < kCatalogPageBodyFormatMajorMinSupported ||
      format_major > kCatalogPageBodyFormatMajorMaxSupported ||
      (format_major == kCatalogPageBodyFormatMajor && format_minor > kCatalogPageBodyFormatMinorMaxSupported)) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
                                "storage.catalog_page_body.format_unsupported",
                                std::to_string(page_number));
  }
  if (LoadLittle32(body.data() + kOffsetHeaderBytes) != kCatalogPageBodyHeaderBytes) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-HEADER-SIZE-INVALID",
                                "storage.catalog_page_body.header_size_invalid",
                                std::to_string(page_number));
  }

  const u64 stored_checksum = LoadLittle64(body.data() + kOffsetBodyChecksum);
  const u64 expected_checksum = ComputeCatalogPageBodyChecksum(body);
  if (stored_checksum != expected_checksum) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-CHECKSUM-MISMATCH",
                                "storage.catalog_page_body.checksum_mismatch",
                                std::to_string(page_number));
  }

  const u32 row_count = LoadLittle32(body.data() + kOffsetRowCount);
  const u32 body_bytes = LoadLittle32(body.data() + kOffsetBodyBytes);
  if (body_bytes > body.size() || body_bytes < kCatalogPageBodyHeaderBytes) {
    return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-BYTES-INVALID",
                                "storage.catalog_page_body.bytes_invalid",
                                std::to_string(page_number));
  }

  CatalogPageBody parsed;
  parsed.page_sequence = LoadLittle32(body.data() + kOffsetPageSequence);
  parsed.page_number = page_number;
  parsed.next_page_number = LoadLittle64(body.data() + kOffsetNextPageNumber);

  u32 offset = kCatalogPageBodyHeaderBytes;
  for (u32 i = 0; i < row_count; ++i) {
    if (offset + kRowHeaderBytes > body_bytes) {
      return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-ROW-SHORT",
                                  "storage.catalog_page_body.row_short",
                                  std::to_string(page_number));
    }
    CatalogPageRow row;
    row.kind = static_cast<CatalogPageRowKind>(LoadLittle16(body.data() + offset + kRowOffsetKind));
    row.ordinal = LoadLittle32(body.data() + offset + kRowOffsetOrdinal);
    const u32 payload_bytes = LoadLittle32(body.data() + offset + kRowOffsetPayloadBytes);
    const u64 payload_checksum = LoadLittle64(body.data() + offset + kRowOffsetPayloadChecksum);
    offset += kRowHeaderBytes;
    if (offset + payload_bytes > body_bytes) {
      return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-PAYLOAD-SHORT",
                                  "storage.catalog_page_body.payload_short",
                                  std::to_string(page_number));
    }
    row.payload.assign(reinterpret_cast<const char*>(body.data() + offset), payload_bytes);
    if (payload_checksum != Fnv1a64(row.payload)) {
      return CatalogPageBodyError("SB-CATALOG-PAGE-BODY-PAYLOAD-CHECKSUM-MISMATCH",
                                  "storage.catalog_page_body.payload_checksum_mismatch",
                                  std::to_string(row.ordinal));
    }
    parsed.rows.push_back(std::move(row));
    offset += payload_bytes;
  }

  CatalogPageBodyResult result;
  result.status = CatalogPageOkStatus();
  result.body = std::move(parsed);
  return result;
}

DiagnosticRecord MakeCatalogPageDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.catalog_body");
}

namespace native_catalog {
namespace disk = scratchbird::storage::disk;
using scratchbird::core::platform::Uuid;
using Error = NativeCatalogRootError;
constexpr std::size_t family = 128, entries = 384, digest_at = 304;
constexpr std::array<byte,8> magic{{'S','B','C','R','O','O','T','1'}};
bool Same(const Uuid& a, const Uuid& b) noexcept { return a.bytes == b.bytes; }
bool V7(const Uuid& a) noexcept {
  return (a.bytes[6] & 0xf0u) == 0x70u && (a.bytes[8] & 0xc0u) == 0x80u;
}
bool Zero(const byte* b, std::size_t n) noexcept {
  return std::all_of(b, b+n, [](byte v) { return v == 0; });
}
void Put(byte* b, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), b); }
Uuid Get(const byte* b) { Uuid id; std::copy_n(b,16,id.bytes.begin()); return id; }
void PutRef(byte* b, const NativeCatalogPageReference& r) {
  Put(b,r.filespace_uuid); StoreLittle64(b+16,r.page_number);
  StoreLittle64(b+24,r.page_generation); Put(b+32,r.page_size_profile_uuid);
}
NativeCatalogPageReference GetRef(const byte* b) {
  return {Get(b),LoadLittle64(b+16),LoadLittle64(b+24),Get(b+32)};
}
NativeCatalogRootResult Fail(Error e) { return {e,std::nullopt,{}}; }
bool Reference(const NativeCatalogPageReference& r) {
  const auto* p = disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);
  return V7(r.filespace_uuid) && r.page_number && r.page_generation && p
      && r.page_number < std::numeric_limits<u64>::max()/p->page_size_bytes
      && disk::CheckFileDeviceExtent(r.page_number*p->page_size_bytes,p->page_size_bytes).ok();
}
bool SameSlot(const NativeCatalogPageReference& a, const NativeCatalogPageReference& b) {
  return Same(a.filespace_uuid,b.filespace_uuid) && a.page_number==b.page_number;
}
bool ProfilesAgree(const NativeCatalogPageReference& a, const NativeCatalogPageReference& b) {
  return !Same(a.filespace_uuid,b.filespace_uuid) || Same(a.page_size_profile_uuid,b.page_size_profile_uuid);
}
Error Validate(const NativeCatalogRoot& r) {
  if (!disk::EncodeNativeCommonPageHeader(r.header).ok() || r.header.page_type!=5)
    return Error::invalid_header;
  if ((r.root_kind!=2 && r.root_kind!=8) || !V7(r.object_uuid)
      || !V7(r.creator_transaction_uuid) || !r.creator_local_transaction_id
      || !r.catalog_generation || !r.schema_epoch || !r.security_epoch)
    return Error::invalid_family;
  const NativeCatalogPageReference self{r.header.filespace_uuid,r.header.page_number,
      r.header.page_generation,r.header.page_size_profile_uuid};
  if (!Reference(self)) return Error::invalid_reference;
  const bool empty_digest = Zero(r.predecessor_sha256.data(),r.predecessor_sha256.size());
  if ((r.catalog_generation==1 && (r.predecessor || !empty_digest))
      || (r.catalog_generation>1 && (!r.predecessor || empty_digest))) return Error::invalid_reference;
  if (r.predecessor && (!Reference(*r.predecessor) || SameSlot(self,*r.predecessor)
      || !ProfilesAgree(self,*r.predecessor))) return Error::invalid_reference;
  if (r.roots.size()!=(r.root_kind==2?6u:1u)) return Error::invalid_roots;
  for (std::size_t i=0;i<r.roots.size();++i) {
    const auto& target=r.roots[i];
    if (target.role!=(r.root_kind==2?i+1:6) || (target.page_type!=6 && target.page_type!=0x200)
        || !V7(target.object_uuid) || !Reference(target.page) || SameSlot(self,target.page)
        || !ProfilesAgree(self,target.page)) return Error::invalid_roots;
    if (r.predecessor && (SameSlot(*r.predecessor,target.page)
        || !ProfilesAgree(*r.predecessor,target.page))) return Error::invalid_roots;
    for (std::size_t j=0;j<i;++j)
      if (SameSlot(target.page,r.roots[j].page) || Same(target.object_uuid,r.roots[j].object_uuid)
          || !ProfilesAgree(target.page,r.roots[j].page)) return Error::invalid_roots;
  }
  return Error::none;
}
scratchbird::core::hash::HashDigestResult Digest(const std::vector<byte>& b) {
  const std::array<byte,32> zero{};
  const scratchbird::core::hash::HashDigestSegment parts[] = {
      {b.data(),digest_at},{zero.data(),zero.size()},
      {b.data()+digest_at+32,b.size()-digest_at-32}};
  return scratchbird::core::hash::ComputeSha256DigestParts(parts,3);
}
}  // namespace native_catalog

NativeCatalogRootResult EncodeNativeCatalogRoot(const NativeCatalogRoot& r) noexcept {
  using namespace native_catalog;
  try {
    const auto error=Validate(r); if (error!=Error::none) return Fail(error);
    const auto common=disk::EncodeNativeCommonPageHeader(r.header);
    std::vector<byte> b(r.header.page_size_bytes,0);
    std::copy(common.bytes->begin(),common.bytes->end(),b.begin());
    byte* f=b.data()+family; std::copy(magic.begin(),magic.end(),f);
    StoreLittle16(f+8,1); StoreLittle16(f+10,256);
    StoreLittle32(f+12,static_cast<u32>(entries+80*r.roots.size()));
    StoreLittle16(f+16,r.root_kind); StoreLittle16(f+18,static_cast<u16>(r.roots.size()));
    StoreLittle64(f+24,r.catalog_generation); StoreLittle64(f+32,r.schema_epoch);
    StoreLittle64(f+40,r.security_epoch); StoreLittle64(f+48,r.resource_epoch);
    StoreLittle64(f+56,r.creator_local_transaction_id); Put(f+64,r.object_uuid);
    Put(f+80,r.creator_transaction_uuid);
    if (r.predecessor) PutRef(f+96,*r.predecessor);
    std::copy(r.predecessor_sha256.begin(),r.predecessor_sha256.end(),f+144);
    for (std::size_t i=0;i<r.roots.size();++i) {
      byte* e=b.data()+entries+80*i; const auto& t=r.roots[i];
      StoreLittle16(e,t.role); StoreLittle32(e+4,t.page_type);
      PutRef(e+8,t.page); Put(e+56,t.object_uuid);
    }
    const auto digest=Digest(b); if (!digest.ok()) return Fail(Error::hash_failure);
    std::copy(digest.digest.begin(),digest.digest.end(),b.begin()+digest_at);
    return {Error::none,r,std::move(b)};
  } catch (const std::bad_alloc&) { return Fail(Error::resource_exhausted); }
    catch (const std::length_error&) { return Fail(Error::resource_exhausted); }
    catch (...) { return Fail(Error::invalid_family); }
}

NativeCatalogRootResult DecodeNativeCatalogRoot(const std::vector<byte>& b) noexcept {
  using namespace native_catalog;
  try {
    if (b.size()<entries) return Fail(Error::invalid_header);
    const auto common=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if (!common.ok() || common.header->page_type!=5 || b.size()!=common.header->page_size_bytes)
      return Fail(Error::invalid_header);
    const byte* f=b.data()+family;
    const auto count=LoadLittle16(f+18); const std::size_t used=entries+80*count;
    if (!std::equal(magic.begin(),magic.end(),f) || LoadLittle16(f+8)!=1
        || LoadLittle16(f+10)!=256 || used>b.size() || LoadLittle32(f+12)!=used
        || !Zero(f+20,4) || !Zero(f+208,48) || !Zero(b.data()+used,b.size()-used))
      return Fail(Error::invalid_family);
    const auto digest=Digest(b); if (!digest.ok()) return Fail(Error::hash_failure);
    if (!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+digest_at))
      return Fail(Error::invalid_integrity);
    NativeCatalogRoot r; r.header=*common.header; r.root_kind=LoadLittle16(f+16);
    r.catalog_generation=LoadLittle64(f+24); r.schema_epoch=LoadLittle64(f+32);
    r.security_epoch=LoadLittle64(f+40); r.resource_epoch=LoadLittle64(f+48);
    r.creator_local_transaction_id=LoadLittle64(f+56); r.object_uuid=Get(f+64);
    r.creator_transaction_uuid=Get(f+80);
    if (!Zero(f+96,48)) r.predecessor=GetRef(f+96);
    std::copy_n(f+144,32,r.predecessor_sha256.begin());
    // Bound cardinality before allocating or interpreting any target.
    if ((r.root_kind==2 && count!=6) || (r.root_kind==8 && count!=1)
        || (r.root_kind!=2 && r.root_kind!=8)) return Fail(Error::invalid_roots);
    for (unsigned i=0;i<count;++i) {
      const byte* e=b.data()+entries+80*i;
      if (!Zero(e+2,2) || !Zero(e+72,8)) return Fail(Error::invalid_roots);
      r.roots.push_back({LoadLittle16(e),LoadLittle32(e+4),GetRef(e+8),Get(e+56)});
    }
    const auto error=Validate(r); if (error!=Error::none) return Fail(error);
    return {Error::none,std::move(r),b};
  } catch (const std::bad_alloc&) { return Fail(Error::resource_exhausted); }
    catch (const std::length_error&) { return Fail(Error::resource_exhausted); }
    catch (...) { return Fail(Error::invalid_family); }
}

NativeCatalogRootResult ReadNativeCatalogRootFromOpenDevice(
    scratchbird::storage::disk::FileDevice& device,
    const scratchbird::core::platform::Uuid& database_uuid,
    const scratchbird::storage::disk::FilespaceRootReference& ref) noexcept {
  using namespace native_catalog;
  try {
    if (!V7(database_uuid) || !V7(ref.object_uuid) || ref.page_type!=5
        || (ref.kind!=2 && ref.kind!=8)
        || !Reference({ref.filespace_uuid,ref.page_number,ref.page_generation,ref.page_size_profile_uuid}))
      return Fail(Error::invalid_reference);
    auto guard=device.AcquireOperationGuard();
    const disk::FilespaceBootstrapBinding binding{database_uuid,ref.filespace_uuid,ref.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device,&binding);
    if (!zero.ok()) {
      if (zero.error==disk::FilespacePageZeroError::resource_exhausted) return Fail(Error::resource_exhausted);
      if (zero.error==disk::FilespacePageZeroError::hash_provider_failure) return Fail(Error::hash_failure);
      if (zero.error==disk::FilespacePageZeroError::io_failure) return Fail(Error::io_failure);
      return Fail(Error::invalid_filespace);
    }
    const auto& z=*zero.record;
    if (z.bootstrap.filespace_role>4 || ref.page_number>=z.total_pages) return Fail(Error::invalid_filespace);
    if (z.bootstrap.flags & disk::FilespaceBootstrapFlag::payload_encrypted)
      return Fail(Error::encrypted_requires_crypto_authority);
    std::vector<byte> bytes(z.bootstrap.page_size_bytes);
    const auto io=device.ReadAt(ref.page_number*z.bootstrap.page_size_bytes,bytes.data(),bytes.size());
    if (!io.ok() || io.bytes_transferred!=bytes.size()) return Fail(Error::io_failure);
    const auto raw_header=disk::DecodeNativeCommonPageHeader(bytes.data(),128);
    if (!raw_header.ok()) return Fail(Error::invalid_header);
    if (raw_header.header->flags & 1u) return Fail(Error::encrypted_requires_crypto_authority);
    auto result=DecodeNativeCatalogRoot(bytes); if (!result.ok()) return result;
    const auto& r=*result.root; const auto& h=r.header;
    if (!Same(h.database_uuid,database_uuid) || !Same(h.filespace_uuid,ref.filespace_uuid)
        || !Same(h.page_size_profile_uuid,ref.page_size_profile_uuid)
        || h.page_number!=ref.page_number || h.page_generation!=ref.page_generation
        || !Same(r.object_uuid,ref.object_uuid) || (ref.kind==2 && r.root_kind!=2))
      return Fail(Error::binding_mismatch);
    if (r.predecessor && Same(r.predecessor->filespace_uuid,h.filespace_uuid)
        && r.predecessor->page_number>=z.total_pages) return Fail(Error::invalid_reference);
    for (const auto& target:r.roots)
      if (Same(target.page.filespace_uuid,h.filespace_uuid) && target.page.page_number>=z.total_pages)
        return Fail(Error::invalid_reference);
    return result;
  } catch (const std::bad_alloc&) { return Fail(Error::resource_exhausted); }
    catch (const std::length_error&) { return Fail(Error::resource_exhausted); }
    catch (...) { return Fail(Error::io_failure); }
}

}  // namespace scratchbird::storage::page
