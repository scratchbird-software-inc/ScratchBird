// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
// CATALOG-CRYPTO-INLINE-PRIVATE-KEY-POLICY-20261017. Definition only: neither
// a true field nor persisted bytes confer EXECUTE, policy admission, key-use,
// resource custody, activation or publication authority. No key is stored here.
struct CatalogCryptoInlinePrivateKeyPolicy {
  Uuid body_uuid;
  u64 generation = 0;
  Uuid database_uuid;
  u64 purpose = 1;  // Inline OpenPGP private-key decryption only.
  bool inline_private_key_use_allowed = false;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogCryptoInlinePrivateKeyPolicyResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogCryptoInlinePrivateKeyPolicy> record;
  bool ok() const noexcept { return error == CatalogValueError::none && record.has_value(); }
};
inline constexpr u32 kCatalogCryptoInlinePrivateKeyPolicySchemaId = 65620;
inline constexpr u16 kCatalogCryptoInlinePrivateKeyPolicySchemaVersion = 1;
inline constexpr std::size_t kCatalogCryptoInlinePrivateKeyPolicyBytes = 153;
namespace crypto_inline_private_key_policy_detail {
inline constexpr CatalogValueFieldSchema fields[]{
  {1,CatalogValueType::engine_identity,true,16,UuidKind::object},
  {2,CatalogValueType::unsigned_integer,true,8},
  {3,CatalogValueType::engine_identity,true,16,UuidKind::database},
  {4,CatalogValueType::unsigned_integer,true,8},
  {5,CatalogValueType::boolean,true,1},
  {6,CatalogValueType::engine_identity,true,16,UuidKind::transaction},
  {7,CatalogValueType::unsigned_integer,true,8}};
inline bool Identity(const TypedUuid& id, UuidKind kind) noexcept {
  return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
}
inline bool Valid(const CatalogCryptoInlinePrivateKeyPolicy& r) noexcept {
  return uuid::IsEngineIdentityUuid(r.body_uuid) && uuid::IsEngineIdentityUuid(r.database_uuid) &&
      Identity(r.origin_transaction_uuid,UuidKind::transaction) && r.generation && r.purpose == 1 &&
      r.origin_local_transaction_id && r.body_uuid != r.database_uuid &&
      r.body_uuid != r.origin_transaction_uuid.value && r.database_uuid != r.origin_transaction_uuid.value;
}
struct Fields {
  std::array<CatalogValueFieldView,7> views{};
  std::array<std::array<byte,8>,3> numbers{};
  std::array<byte,1> permission{};
  explicit Fields(const CatalogCryptoInlinePrivateKeyPolicy& r) noexcept {
    platform::StoreLittle64(numbers[0].data(),r.generation);
    platform::StoreLittle64(numbers[1].data(),r.purpose);
    platform::StoreLittle64(numbers[2].data(),r.origin_local_transaction_id);
    permission[0] = r.inline_private_key_use_allowed ? 1 : 0;
    views = {{{1,CatalogValueType::engine_identity,UuidKind::object,r.body_uuid.bytes},
      {2,CatalogValueType::unsigned_integer,UuidKind::unknown,numbers[0]},
      {3,CatalogValueType::engine_identity,UuidKind::database,r.database_uuid.bytes},
      {4,CatalogValueType::unsigned_integer,UuidKind::unknown,numbers[1]},
      {5,CatalogValueType::boolean,UuidKind::unknown,permission},
      {6,CatalogValueType::engine_identity,UuidKind::transaction,r.origin_transaction_uuid.value.bytes},
      {7,CatalogValueType::unsigned_integer,UuidKind::unknown,numbers[2]}}};
  }
};
inline bool Owner(const TypedUuid& id) noexcept {
  switch(id.kind) {
    case UuidKind::principal: case UuidKind::object: case UuidKind::schema:
    case UuidKind::database: case UuidKind::cluster: return uuid::IsEngineIdentityUuid(id.value);
    default: return false;
  }
}
inline bool Header(const CatalogTypedRecordView& m,const CatalogCryptoInlinePrivateKeyPolicy& r) noexcept {
  return m.header.kind == CatalogRecordKind::policy && Identity(m.header.object_uuid,UuidKind::object) &&
      m.header.object_uuid.value == r.body_uuid && Identity(m.header.row_uuid,UuidKind::row) &&
      m.header.row_uuid.value != r.body_uuid;
}
inline bool Metadata(const CatalogMetadataVersionView& m,const CatalogCryptoInlinePrivateKeyPolicy& r) noexcept {
  return Header(m.record,r) && m.object_subtype == "crypto_inline_private_key" &&
      m.definition_version == r.generation && Owner(m.owner_uuid) &&
      Identity(m.owning_schema_uuid,UuidKind::schema) && Identity(m.record.header.parent_uuid,UuidKind::object) &&
      m.owning_schema_uuid.value == m.record.header.parent_uuid.value && Identity(m.audit_uuid,UuidKind::object) &&
      Identity(m.creator_transaction_uuid,UuidKind::transaction) && m.creator_local_transaction_id &&
      m.creator_local_transaction_id >= r.origin_local_transaction_id &&
      (r.generation != 1 || (m.creator_transaction_uuid.value == r.origin_transaction_uuid.value &&
                            m.creator_local_transaction_id == r.origin_local_transaction_id));
}
} // namespace crypto_inline_private_key_policy_detail

inline constexpr CatalogValueSchemaView CatalogCryptoInlinePrivateKeyPolicySchemaView() noexcept {
  return {kCatalogCryptoInlinePrivateKeyPolicySchemaId,kCatalogCryptoInlinePrivateKeyPolicySchemaVersion,
          crypto_inline_private_key_policy_detail::fields};
}
inline std::optional<std::size_t> CatalogCryptoInlinePrivateKeyPolicyEncodedBytes(
    const CatalogCryptoInlinePrivateKeyPolicy& r) noexcept {
  if(!crypto_inline_private_key_policy_detail::Valid(r)) return {};
  const crypto_inline_private_key_policy_detail::Fields fields(r);
  if(ValidateCatalogValueFields(CatalogCryptoInlinePrivateKeyPolicySchemaView(),fields.views) != CatalogValueError::none) return {};
  return kCatalogCryptoInlinePrivateKeyPolicyBytes;
}
// Complete borrowed admission precedes effects. All output, including unused
// tail, is unchanged on refusal. The entire source object cannot overlap it.
inline CatalogValueEncodeIntoResult EncodeCatalogCryptoInlinePrivateKeyPolicyInto(
    const CatalogCryptoInlinePrivateKeyPolicy& r,std::span<byte> output) {
  if(!crypto_inline_private_key_policy_detail::Valid(r)) return {CatalogValueError::invalid_value,{}};
  const auto at = reinterpret_cast<std::uintptr_t>(output.data());
  const auto input = reinterpret_cast<std::uintptr_t>(&r);
  if((output.size() && !output.data()) || output.size() > UINTPTR_MAX-at ||
      (output.size() && at < input+sizeof(r) && input < at+output.size()))
    return {CatalogValueError::invalid_backing,{}};
  const crypto_inline_private_key_policy_detail::Fields fields(r);
  return EncodeCatalogValueBlockInto(CatalogCryptoInlinePrivateKeyPolicySchemaView(),fields.views,output);
}
// All returned fields are fixed native values, not views or authority. The
// complete source remains borrowed and immutable for this synchronous decode.
inline CatalogCryptoInlinePrivateKeyPolicyResult DecodeCatalogCryptoInlinePrivateKeyPolicy(std::string_view bytes) {
  if(bytes.size() != kCatalogCryptoInlinePrivateKeyPolicyBytes) return {CatalogValueError::invalid_framing,{}};
  std::array<CatalogValueFieldView,7> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogCryptoInlinePrivateKeyPolicySchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},fields);
  if(!decoded.ok()) return {decoded.error,{}};
  const auto& f = decoded.fields;
  CatalogCryptoInlinePrivateKeyPolicy r;
  r.body_uuid = f[0].identity()->value; r.generation = *f[1].unsigned_value();
  r.database_uuid = f[2].identity()->value; r.purpose = *f[3].unsigned_value();
  r.inline_private_key_use_allowed = f[4].bytes[0] != 0;
  r.origin_transaction_uuid = *f[5].identity(); r.origin_local_transaction_id = *f[6].unsigned_value();
  if(!crypto_inline_private_key_policy_detail::Valid(r)) return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,r};
}
inline bool IsCatalogCryptoInlinePrivateKeyPolicyPayload(std::string_view bytes) noexcept {
  const auto at = reinterpret_cast<std::uintptr_t>(bytes.data());
  return bytes.data() && bytes.size() <= UINTPTR_MAX-at &&
      bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0,4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16) == kCatalogCryptoInlinePrivateKeyPolicySchemaId;
}
inline bool CatalogCryptoInlinePrivateKeyPolicyMatchesHeader(const CatalogTypedRecordView& m) {
  const auto r = DecodeCatalogCryptoInlinePrivateKeyPolicy(m.payload);
  return r.ok() && crypto_inline_private_key_policy_detail::Header(m,*r.record);
}
inline bool CatalogCryptoInlinePrivateKeyPolicyMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto r = DecodeCatalogCryptoInlinePrivateKeyPolicy(m.record.payload);
  return r.ok() && crypto_inline_private_key_policy_detail::Metadata(m,*r.record);
}
inline bool IsCatalogCryptoInlinePrivateKeyPolicyFamily(const CatalogMetadataVersionView& m) noexcept {
  return m.object_subtype == "crypto_inline_private_key" || IsCatalogCryptoInlinePrivateKeyPolicyPayload(m.record.payload);
}
inline bool CatalogCryptoInlinePrivateKeyPolicyMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& database) {
  if(!IsCatalogCryptoInlinePrivateKeyPolicyFamily(m)) return true;
  const auto r = DecodeCatalogCryptoInlinePrivateKeyPolicy(m.record.payload);
  return uuid::IsEngineIdentityUuid(database) && r.ok() && r.record->database_uuid == database &&
      crypto_inline_private_key_policy_detail::Metadata(m,*r.record);
}
inline bool CatalogCryptoInlinePrivateKeyPolicyPreservesOrigin(
    const CatalogMetadataVersionView& previous,const CatalogMetadataVersionView& successor) {
  if(!IsCatalogCryptoInlinePrivateKeyPolicyFamily(previous) && !IsCatalogCryptoInlinePrivateKeyPolicyFamily(successor)) return true;
  const auto a = DecodeCatalogCryptoInlinePrivateKeyPolicy(previous.record.payload);
  const auto b = DecodeCatalogCryptoInlinePrivateKeyPolicy(successor.record.payload);
  if(!a.ok() || !b.ok() || !crypto_inline_private_key_policy_detail::Metadata(previous,*a.record) ||
      !crypto_inline_private_key_policy_detail::Metadata(successor,*b.record)) return false;
  const auto& p = *a.record; const auto& q = *b.record;
  return p.body_uuid == q.body_uuid && p.database_uuid == q.database_uuid && p.purpose == q.purpose &&
      p.origin_transaction_uuid.value == q.origin_transaction_uuid.value &&
      p.origin_local_transaction_id == q.origin_local_transaction_id &&
      (q.generation > p.generation || (q.generation == p.generation && previous.record.payload == successor.record.payload));
}
inline bool CatalogCryptoInlinePrivateKeyPolicyMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogCryptoInlinePrivateKeyPolicyMatchesHeader(BorrowCatalogTypedRecord(r));
}
inline bool CatalogCryptoInlinePrivateKeyPolicyMatchesMetadata(const CatalogMetadataVersion& m) {
  return CatalogCryptoInlinePrivateKeyPolicyMatchesMetadata(BorrowCatalogMetadataVersion(m));
}
inline bool CatalogCryptoInlinePrivateKeyPolicyMatchesDatabase(const CatalogMetadataVersion& m,const Uuid& database) {
  return CatalogCryptoInlinePrivateKeyPolicyMatchesDatabase(BorrowCatalogMetadataVersion(m),database);
}
inline bool CatalogCryptoInlinePrivateKeyPolicyPreservesOrigin(
    const CatalogMetadataVersion& a,const CatalogMetadataVersion& b) {
  return CatalogCryptoInlinePrivateKeyPolicyPreservesOrigin(BorrowCatalogMetadataVersion(a),BorrowCatalogMetadataVersion(b));
}
} // namespace scratchbird::core::catalog
