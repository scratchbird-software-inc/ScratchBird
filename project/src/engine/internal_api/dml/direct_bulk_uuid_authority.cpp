// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/direct_bulk_uuid_authority.hpp"
#include "crud_support/crud_store.hpp"
#include "uuid.hpp"
#include <limits>
#include <set>
#include <stdexcept>

namespace scratchbird::engine::internal_api::dml::detail {
namespace {
[[noreturn]] void InvalidIdentity(const char* code,const char* key) {
  namespace p=core::platform;
  throw CrudIdentityIssuanceError(core::uuid::MakeUuidDiagnostic(
      {p::StatusCode::uuid_invalid,p::Severity::error,p::Subsystem::uuid},code,key));
}
}
// This is binary allocation, not database/cluster time or MGA authority.
// The shared CRUD issuer's owning time-policy integration remains required;
// never reintroduce a local salted PRNG or a context-free runtime fallback.
DirectBulkUuidBatch BuildDirectBulkUuidBatch(
    const DirectPhysicalBulkAppendRequest& request,std::size_t row_count) {
  DirectBulkUuidBatch batch;
  const bool images=static_cast<bool>(request.before_row_publication);
  if(row_count>batch.row_uuids.max_size() ||
     row_count>batch.version_uuids.max_size() ||
     (images && row_count>batch.row_image_uuids.max_size()) ||
     row_count>std::numeric_limits<std::size_t>::max()/(images?3:2))
    throw std::length_error("direct bulk UUID cohort extent");
  // Reserve caller-supplied IDs first: a generated value must not alias an
  // ID belonging to a later input row. No fresh issuance repairs a bad input.
  std::set<EngineUuid> identities;
  for(std::size_t i=0;i<row_count && i<request.borrowed_input_rows.size();++i) {
    const auto& id=request.borrowed_input_rows[i].requested_row_uuid;
    if(id.is_nil())continue;
    if(!core::uuid::IsEngineIdentityUuid(id))
      InvalidIdentity("UUID.ENGINE_IDENTITY_NOT_V7","dml.bulk_uuid.input_requires_v7");
    if(!identities.insert(id).second)
      InvalidIdentity("BULK.IMPORT.RECOVERY_CONFLICT","dml.bulk_uuid.duplicate_input_identity");
    ++batch.caller_row_uuids;
  }
  batch.row_uuids.reserve(row_count);batch.version_uuids.reserve(row_count);
  if(images)batch.row_image_uuids.reserve(row_count);
  batch.batch_evidence_id="direct-bulk-uuid-batch:"+request.context.request_id+":"+std::to_string(row_count);
  const auto fresh=[&](std::string_view kind) {
    const auto id=GenerateCrudEngineUuid(kind);
    if(!core::uuid::IsEngineIdentityUuid(id))
      InvalidIdentity("UUID.ENGINE_IDENTITY_NOT_V7","dml.bulk_uuid.generated_requires_v7");
    if(!identities.insert(id).second)
      InvalidIdentity("BULK.IMPORT.RECOVERY_CONFLICT","dml.bulk_uuid.generated_identity_collision");
    ++batch.reservoir_sync_generated_uuids;
    return id;
  };
  // All allocations and identities are private until this complete batch is
  // returned. Entropy/clock/allocation/collision failure returns no partial batch.
  for(std::size_t i=0;i<row_count;++i) {
    const bool supplied=i<request.borrowed_input_rows.size() &&
        !request.borrowed_input_rows[i].requested_row_uuid.is_nil();
    if(supplied)batch.row_uuids.push_back(request.borrowed_input_rows[i].requested_row_uuid);
    else {batch.row_uuids.push_back(fresh("row"));++batch.generated_row_uuids;}
    batch.version_uuids.push_back(fresh("row"));
    if(images)batch.row_image_uuids.push_back(fresh("row"));
  }
  return batch;
}

void AddDirectBulkUuidBatchEvidence(const DirectBulkUuidBatch& batch,
                                    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back({"direct_bulk_uuid_generation_mode", "binary_shared_crypto"});
  result->evidence.push_back({"direct_bulk_uuid_batch", batch.batch_evidence_id});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_row_capacity",
       std::to_string(batch.row_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_version_capacity",
       std::to_string(batch.version_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_row_image_capacity",
       std::to_string(batch.row_image_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_generated_row_uuids",
       std::to_string(batch.generated_row_uuids)});
  result->evidence.push_back(
      {"direct_bulk_caller_row_uuids",
       std::to_string(batch.caller_row_uuids)});
  result->evidence.push_back(
      {"direct_bulk_version_uuid_generation_mode", "binary_shared_crypto"});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_served",
       std::to_string(batch.reservoir_served_uuids)});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_sync_generated",
       std::to_string(batch.reservoir_sync_generated_uuids)});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_async_refill_requested",
       batch.reservoir_async_refill_requested ? "true" : "false"});
  result->evidence.push_back(
      {"orh_210_batched_uuid_generation", "row_and_version_batch"});
}

}  // namespace scratchbird::engine::internal_api::dml::detail
