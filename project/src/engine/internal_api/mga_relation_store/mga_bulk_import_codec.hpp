// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_relation_store/mga_bulk_import_publication.hpp"
#include "core/hash/hash_digest.hpp"
#include "core/uuid/uuid.hpp"
#include <algorithm>
#include <span>
#include <string_view>

namespace scratchbird::engine::internal_api::bulk_import_binary {
using Bytes=std::vector<std::uint8_t>;
using Sha=MgaBulkImportSha256V1;
inline constexpr std::size_t kPublicationBytes=612,kEventBytes=388;
inline bool Valid(const EngineUuid& id) { return core::uuid::IsEngineIdentityUuid(id); }
inline bool Nonzero(const Sha& h) {
  return std::any_of(h.begin(),h.end(),[](auto b){return b!=0;});
}
inline void Number(Bytes& b,std::uint64_t value,unsigned width) {
  for(unsigned i=0;i<width;++i)b.push_back(static_cast<std::uint8_t>(value>>(8*i)));
}
template<class A> inline void Append(Bytes& b,const A& a) { b.insert(b.end(),a.begin(),a.end()); }
inline std::uint64_t ReadNumber(std::span<const std::uint8_t> b,std::size_t at,unsigned width) {
  std::uint64_t v=0;for(unsigned i=0;i<width;++i)v|=std::uint64_t(b[at+i])<<(8*i);return v;
}
template<class A> inline void Read(std::span<const std::uint8_t> b,std::size_t at,A& a) {
  std::copy_n(b.begin()+at,a.size(),a.begin());
}
inline Sha Hash(std::string_view domain,std::span<const std::uint8_t> bytes) {
  Bytes input(domain.begin(),domain.end());Append(input,bytes);
  const auto hash=core::hash::ComputeSha256Digest(input);
  return hash.ok() && hash.digest_bytes==32?hash.digest:Sha{};
}
inline bool Shape(const MgaBulkImportPublicationRecordV1& r) {
  if(!Valid(r.durable_publication_uuid) ||
      !Nonzero(r.recovery_idempotency_key) ||
      !Valid(r.stream_uuid) ||
      !Nonzero(r.descriptor_evidence) ||
      !Valid(r.target_relation_uuid) ||
      !Valid(r.owning_transaction_uuid) ||
      !Valid(r.authenticated_receipt_uuid) ||
      !Valid(r.statement_uuid) ||
      !Valid(r.mutation_uuid) ||
      !Valid(r.bulk_batch_uuid) ||
      !Nonzero(r.content_sha256) ||
      !Nonzero(r.imported_row_postcondition_sha256) ||
      !Nonzero(r.normalized_statement_effect_sha256) ||
      !Nonzero(r.column_descriptor_set_sha256) ||
      !Nonzero(r.import_policy_bundle_sha256) ||
      !Nonzero(r.default_descriptor_set_sha256) ||
      !Nonzero(r.constraint_set_sha256) ||
      !Nonzero(r.trigger_set_sha256) ||
      !Nonzero(r.index_set_sha256))return false;
  return r.durable_publication_generation==1 && r.stream_generation &&
      r.target_relation_generation && r.owning_local_transaction_id && r.savepoint_ordinal &&
      r.total_stream_bytes && r.chunk_count && r.input_row_count &&
      r.affected_rows==r.input_row_count && !r.rejected_rows &&
      r.imported_row_postcondition_count==r.input_row_count && r.executor_availability_generation &&
      r.stream_uuid!=r.durable_publication_uuid && r.stream_uuid!=r.mutation_uuid &&
      r.stream_uuid!=r.bulk_batch_uuid && r.durable_publication_uuid!=r.mutation_uuid &&
      r.durable_publication_uuid!=r.bulk_batch_uuid && r.mutation_uuid!=r.bulk_batch_uuid;
}
inline bool Shape(const MgaBulkImportImportedRowEventV1& r) {
  if(!Valid(r.durable_publication_uuid) ||
      !Nonzero(r.recovery_idempotency_key) ||
      !Valid(r.mutation_uuid) ||
      !Valid(r.bulk_batch_uuid) ||
      !Valid(r.owning_transaction_uuid) ||
      !Valid(r.statement_uuid) ||
      !Valid(r.target_relation_uuid) ||
      !Valid(r.row_uuid) ||
      !Valid(r.row_version_uuid) ||
      !Valid(r.row_image_uuid) ||
      !Nonzero(r.row_image_domain_hash) ||
      !Nonzero(r.row_image_value_hash) ||
      !Nonzero(r.column_descriptor_set_sha256) ||
      !Nonzero(r.canonical_typed_field_vector_sha256))return false;
  return r.durable_publication_generation==1 && r.owning_local_transaction_id &&
      r.savepoint_ordinal && r.target_relation_generation && r.import_ordinal &&
      r.row_image_metadata_generation && r.row_uuid!=r.row_version_uuid &&
      r.row_uuid!=r.row_image_uuid && r.row_version_uuid!=r.row_image_uuid &&
      r.row_image_domain_hash==r.column_descriptor_set_sha256 &&
      r.row_image_value_hash==r.canonical_typed_field_vector_sha256;
}
inline Bytes Encode(MgaBulkImportPublicationRecordV1& r) {
  if(!Shape(r))return {};
  Bytes bytes{1,0,0,0};bytes.reserve(kPublicationBytes);
  Append(bytes,r.durable_publication_uuid.bytes);
  Number(bytes,r.durable_publication_generation,8);
  Append(bytes,r.recovery_idempotency_key);
  Append(bytes,r.stream_uuid.bytes);
  Number(bytes,r.stream_generation,8);
  Append(bytes,r.descriptor_evidence);
  Append(bytes,r.target_relation_uuid.bytes);
  Number(bytes,r.target_relation_generation,8);
  Append(bytes,r.owning_transaction_uuid.bytes);
  Number(bytes,r.owning_local_transaction_id,8);
  Append(bytes,r.authenticated_receipt_uuid.bytes);
  Append(bytes,r.statement_uuid.bytes);
  Number(bytes,r.savepoint_ordinal,8);
  Append(bytes,r.mutation_uuid.bytes);
  Append(bytes,r.bulk_batch_uuid.bytes);
  Append(bytes,r.content_sha256);
  Number(bytes,r.total_stream_bytes,8);
  Number(bytes,r.chunk_count,8);
  Number(bytes,r.input_row_count,8);
  Number(bytes,r.affected_rows,8);
  Number(bytes,r.rejected_rows,8);
  Number(bytes,r.imported_row_postcondition_count,8);
  Append(bytes,r.imported_row_postcondition_sha256);
  Append(bytes,r.normalized_statement_effect_sha256);
  Append(bytes,r.column_descriptor_set_sha256);
  Append(bytes,r.import_policy_bundle_sha256);
  Append(bytes,r.default_descriptor_set_sha256);
  Append(bytes,r.constraint_set_sha256);
  Append(bytes,r.trigger_set_sha256);
  Append(bytes,r.index_set_sha256);
  Number(bytes,r.executor_availability_generation,8);
  const auto hash=Hash("ScratchBird.BulkImportStreamMgaPublicationRecord.V1",bytes);
  if(!Nonzero(hash))return {};
  Append(bytes,hash);r.record_evidence_sha256=hash;return bytes;
}
inline Bytes Encode(MgaBulkImportImportedRowEventV1& r) {
  if(!Shape(r))return {};
  Bytes bytes{1,0,0,0};bytes.reserve(kEventBytes);
  Append(bytes,r.durable_publication_uuid.bytes);
  Number(bytes,r.durable_publication_generation,8);
  Append(bytes,r.recovery_idempotency_key);
  Append(bytes,r.mutation_uuid.bytes);
  Append(bytes,r.bulk_batch_uuid.bytes);
  Append(bytes,r.owning_transaction_uuid.bytes);
  Number(bytes,r.owning_local_transaction_id,8);
  Append(bytes,r.statement_uuid.bytes);
  Number(bytes,r.savepoint_ordinal,8);
  Append(bytes,r.target_relation_uuid.bytes);
  Number(bytes,r.target_relation_generation,8);
  Number(bytes,r.import_ordinal,8);
  Append(bytes,r.row_uuid.bytes);
  Append(bytes,r.row_version_uuid.bytes);
  Append(bytes,r.row_image_uuid.bytes);
  Number(bytes,r.row_image_metadata_generation,8);
  Append(bytes,r.row_image_domain_hash);
  Append(bytes,r.row_image_value_hash);
  Append(bytes,r.column_descriptor_set_sha256);
  Append(bytes,r.canonical_typed_field_vector_sha256);
  const auto hash=Hash("ScratchBird.BulkImportStreamImportedRowEvent.V1",bytes);
  if(!Nonzero(hash))return {};
  Append(bytes,hash);r.event_evidence_sha256=hash;return bytes;
}
inline bool Decode(std::span<const std::uint8_t> bytes,MgaBulkImportPublicationRecordV1* output) {
  if(!output || bytes.size()!=kPublicationBytes || ReadNumber(bytes,0,4)!=1)return false;
  MgaBulkImportPublicationRecordV1 r;
  Read(bytes,4,r.durable_publication_uuid.bytes);
  r.durable_publication_generation=ReadNumber(bytes,20,8);
  Read(bytes,28,r.recovery_idempotency_key);
  Read(bytes,60,r.stream_uuid.bytes);
  r.stream_generation=ReadNumber(bytes,76,8);
  Read(bytes,84,r.descriptor_evidence);
  Read(bytes,116,r.target_relation_uuid.bytes);
  r.target_relation_generation=ReadNumber(bytes,132,8);
  Read(bytes,140,r.owning_transaction_uuid.bytes);
  r.owning_local_transaction_id=ReadNumber(bytes,156,8);
  Read(bytes,164,r.authenticated_receipt_uuid.bytes);
  Read(bytes,180,r.statement_uuid.bytes);
  r.savepoint_ordinal=ReadNumber(bytes,196,8);
  Read(bytes,204,r.mutation_uuid.bytes);
  Read(bytes,220,r.bulk_batch_uuid.bytes);
  Read(bytes,236,r.content_sha256);
  r.total_stream_bytes=ReadNumber(bytes,268,8);
  r.chunk_count=ReadNumber(bytes,276,8);
  r.input_row_count=ReadNumber(bytes,284,8);
  r.affected_rows=ReadNumber(bytes,292,8);
  r.rejected_rows=ReadNumber(bytes,300,8);
  r.imported_row_postcondition_count=ReadNumber(bytes,308,8);
  Read(bytes,316,r.imported_row_postcondition_sha256);
  Read(bytes,348,r.normalized_statement_effect_sha256);
  Read(bytes,380,r.column_descriptor_set_sha256);
  Read(bytes,412,r.import_policy_bundle_sha256);
  Read(bytes,444,r.default_descriptor_set_sha256);
  Read(bytes,476,r.constraint_set_sha256);
  Read(bytes,508,r.trigger_set_sha256);
  Read(bytes,540,r.index_set_sha256);
  r.executor_availability_generation=ReadNumber(bytes,572,8);
  Read(bytes,580,r.record_evidence_sha256);
  const auto expected=r.record_evidence_sha256;
  const auto canonical=Encode(r);
  if(canonical.size()!=bytes.size() || r.record_evidence_sha256!=expected ||
     !std::equal(canonical.begin(),canonical.end(),bytes.begin()))return false;
  *output=r;return true;
}
inline bool Decode(std::span<const std::uint8_t> bytes,MgaBulkImportImportedRowEventV1* output) {
  if(!output || bytes.size()!=kEventBytes || ReadNumber(bytes,0,4)!=1)return false;
  MgaBulkImportImportedRowEventV1 r;
  Read(bytes,4,r.durable_publication_uuid.bytes);
  r.durable_publication_generation=ReadNumber(bytes,20,8);
  Read(bytes,28,r.recovery_idempotency_key);
  Read(bytes,60,r.mutation_uuid.bytes);
  Read(bytes,76,r.bulk_batch_uuid.bytes);
  Read(bytes,92,r.owning_transaction_uuid.bytes);
  r.owning_local_transaction_id=ReadNumber(bytes,108,8);
  Read(bytes,116,r.statement_uuid.bytes);
  r.savepoint_ordinal=ReadNumber(bytes,132,8);
  Read(bytes,140,r.target_relation_uuid.bytes);
  r.target_relation_generation=ReadNumber(bytes,156,8);
  r.import_ordinal=ReadNumber(bytes,164,8);
  Read(bytes,172,r.row_uuid.bytes);
  Read(bytes,188,r.row_version_uuid.bytes);
  Read(bytes,204,r.row_image_uuid.bytes);
  r.row_image_metadata_generation=ReadNumber(bytes,220,8);
  Read(bytes,228,r.row_image_domain_hash);
  Read(bytes,260,r.row_image_value_hash);
  Read(bytes,292,r.column_descriptor_set_sha256);
  Read(bytes,324,r.canonical_typed_field_vector_sha256);
  Read(bytes,356,r.event_evidence_sha256);
  const auto expected=r.event_evidence_sha256;
  const auto canonical=Encode(r);
  if(canonical.size()!=bytes.size() || r.event_evidence_sha256!=expected ||
     !std::equal(canonical.begin(),canonical.end(),bytes.begin()))return false;
  *output=r;return true;
}
} // namespace scratchbird::engine::internal_api::bulk_import_binary
