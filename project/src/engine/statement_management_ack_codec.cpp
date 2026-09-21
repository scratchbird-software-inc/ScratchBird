// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "statement_management_ack_codec.hpp"
#include "core/hash/hash_digest.hpp"
#include "core/uuid/uuid.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::statement_management {
using server_engine_bridge::StatementPrepareBindAckV1;
using server_engine_bridge::StatementExecuteDirectBindAckV1;
using server_engine_bridge::StatementNameResolveBindAckV1;
using server_engine_bridge::StatementParseTextBindAckV1;
using server_engine_bridge::StatementCatalogEpochCheckBindAckV1;
using server_engine_bridge::StatementDatabaseAttachBindAckV1;
using server_engine_bridge::StatementFreeBindAckV1;
using server_engine_bridge::StatementCancelBindAckV1;
namespace {
using internal_api::EngineUuid;
bool valid_engine_identity(const EngineUuid& value) {
  return core::uuid::IsEngineIdentityUuid(value);
}
bool bulk_import_nonzero_hash(const std::array<std::uint8_t, 32>& hash) {
  return std::any_of(hash.begin(), hash.end(), [](auto value) { return value != 0; });
}
void bulk_import_append_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  for (unsigned i=0;i<2;++i) out->push_back(static_cast<std::uint8_t>(value>>(8*i)));
}
void bulk_import_append_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (unsigned i=0;i<4;++i) out->push_back(static_cast<std::uint8_t>(value>>(8*i)));
}
void bulk_import_append_u64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (unsigned i=0;i<8;++i) out->push_back(static_cast<std::uint8_t>(value>>(8*i)));
}
bool bulk_import_append_uuid(std::vector<std::uint8_t>* out, const EngineUuid& value) {
  if (!valid_engine_identity(value)) return false;
  out->insert(out->end(), value.bytes.begin(), value.bytes.end());
  return true;
}
template<class Ack>
std::vector<std::uint8_t> FinishAck(Ack* ack, std::vector<std::uint8_t> bytes,
                                  std::size_t digest_offset, bool retain_bytes) {
  const auto digest = core::hash::ComputeSha256Digest(bytes);
  if (!digest.ok()) return {};
  std::copy(digest.digest.begin(), digest.digest.end(), bytes.begin()+digest_offset);
  // Stage the only fallible publication copy before modifying the caller.
  std::vector<std::uint8_t> retained;
  if (retain_bytes) retained = bytes;
  if (retain_bytes) ack->exact_bind_ack_bytes.swap(retained);
  ack->acknowledgement_evidence_sha256 = digest.digest;
  return bytes;
}
}  // namespace

std::vector<std::uint8_t> statement_management_prepare_ack(
    StatementPrepareBindAckV1* acknowledgement) {
  if (acknowledgement == nullptr ||
      !valid_engine_identity(
          acknowledgement->authenticated_receipt_uuid) ||
      acknowledgement->occurrence == 0 ||
      !valid_engine_identity(acknowledgement->binding_uuid) ||
      acknowledgement->binding_generation == 0 ||
      !valid_engine_identity(acknowledgement->statement_name_uuid) ||
      !bulk_import_nonzero_hash(acknowledgement->descriptor_sha256) ||
      !bulk_import_nonzero_hash(acknowledgement->request_evidence_sha256)) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'S', 'P', 'B', 'A'});
  bulk_import_append_u16(&bytes, 1);
  bulk_import_append_u16(&bytes, 176);
  bulk_import_append_u32(&bytes, 176);
  bulk_import_append_u32(&bytes, 0);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->authenticated_receipt_uuid)) return {};
  bulk_import_append_u64(&bytes, acknowledgement->occurrence);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->binding_uuid)) return {};
  bulk_import_append_u64(&bytes, acknowledgement->binding_generation);
  if (!bulk_import_append_uuid(&bytes,
                               acknowledgement->statement_name_uuid)) {
    return {};
  }
  bytes.insert(bytes.end(), acknowledgement->descriptor_sha256.begin(),
               acknowledgement->descriptor_sha256.end());
  bytes.insert(bytes.end(), acknowledgement->request_evidence_sha256.begin(),
               acknowledgement->request_evidence_sha256.end());
  bytes.insert(bytes.end(), 32, 0);
  if (bytes.size() != 176) return {};
  return FinishAck(acknowledgement, std::move(bytes), 144, false);
}

std::vector<std::uint8_t> statement_management_execute_direct_ack(
    StatementExecuteDirectBindAckV1* acknowledgement) {
  if (acknowledgement == nullptr ||
      !valid_engine_identity(
          acknowledgement->authenticated_receipt_uuid) ||
      acknowledgement->occurrence == 0 ||
      !valid_engine_identity(acknowledgement->binding_uuid) ||
      acknowledgement->binding_generation == 0 ||
      !bulk_import_nonzero_hash(acknowledgement->descriptor_sha256) ||
      !bulk_import_nonzero_hash(acknowledgement->request_evidence_sha256)) {
    return {};
  }
  if (!acknowledgement->result_descriptor_uuid.is_nil() &&
      !valid_engine_identity(
          acknowledgement->result_descriptor_uuid)) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'S', 'D', 'B', 'A'});
  bulk_import_append_u16(&bytes, 1);
  bulk_import_append_u16(&bytes, 176);
  bulk_import_append_u32(&bytes, 176);
  bulk_import_append_u32(&bytes, 0);
  if (!bulk_import_append_uuid(
          &bytes, acknowledgement->authenticated_receipt_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->occurrence);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->binding_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->binding_generation);
  if (acknowledgement->result_descriptor_uuid.is_nil()) {
    bytes.insert(bytes.end(), 16, 0);
  } else if (!bulk_import_append_uuid(
                 &bytes, acknowledgement->result_descriptor_uuid)) {
    return {};
  }
  bytes.insert(bytes.end(), acknowledgement->descriptor_sha256.begin(),
               acknowledgement->descriptor_sha256.end());
  bytes.insert(bytes.end(), acknowledgement->request_evidence_sha256.begin(),
               acknowledgement->request_evidence_sha256.end());
  bytes.insert(bytes.end(), 32, 0);
  if (bytes.size() != 176) return {};
  return FinishAck(acknowledgement, std::move(bytes), 144, false);
}

std::vector<std::uint8_t> statement_management_name_resolve_ack(
    StatementNameResolveBindAckV1* acknowledgement) {
  if (acknowledgement == nullptr ||
      !valid_engine_identity(
          acknowledgement->authenticated_receipt_uuid) ||
      acknowledgement->occurrence == 0 ||
      !valid_engine_identity(acknowledgement->binding_uuid) ||
      acknowledgement->binding_generation == 0 ||
      !valid_engine_identity(acknowledgement->resolution_uuid) ||
      !bulk_import_nonzero_hash(acknowledgement->descriptor_sha256) ||
      !bulk_import_nonzero_hash(acknowledgement->request_evidence_sha256)) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'S', 'N', 'B', 'A'});
  bulk_import_append_u16(&bytes, 1);
  bulk_import_append_u16(&bytes, 176);
  bulk_import_append_u32(&bytes, 176);
  bulk_import_append_u32(&bytes, 0);
  if (!bulk_import_append_uuid(
          &bytes, acknowledgement->authenticated_receipt_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->occurrence);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->binding_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->binding_generation);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->resolution_uuid)) {
    return {};
  }
  bytes.insert(bytes.end(), acknowledgement->descriptor_sha256.begin(),
               acknowledgement->descriptor_sha256.end());
  bytes.insert(bytes.end(), acknowledgement->request_evidence_sha256.begin(),
               acknowledgement->request_evidence_sha256.end());
  bytes.insert(bytes.end(), 32, 0);
  if (bytes.size() != 176) return {};
  return FinishAck(acknowledgement, std::move(bytes), 144, true);
}

std::vector<std::uint8_t> statement_management_parse_text_ack(
    StatementParseTextBindAckV1* acknowledgement) {
  if (acknowledgement == nullptr ||
      !valid_engine_identity(
          acknowledgement->authenticated_receipt_uuid) ||
      acknowledgement->occurrence == 0 ||
      !valid_engine_identity(acknowledgement->binding_uuid) ||
      acknowledgement->binding_generation == 0 ||
      !valid_engine_identity(acknowledgement->parse_uuid) ||
      !bulk_import_nonzero_hash(acknowledgement->descriptor_sha256) ||
      !bulk_import_nonzero_hash(
          acknowledgement->canonical_input_sha256) ||
      !bulk_import_nonzero_hash(
          acknowledgement->request_evidence_sha256)) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'P', 'T', 'B', 'A'});
  bulk_import_append_u16(&bytes, 1);
  bulk_import_append_u16(&bytes, 208);
  bulk_import_append_u32(&bytes, 208);
  bulk_import_append_u32(&bytes, 0);
  if (!bulk_import_append_uuid(
          &bytes, acknowledgement->authenticated_receipt_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->occurrence);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->binding_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->binding_generation);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->parse_uuid)) {
    return {};
  }
  bytes.insert(bytes.end(), acknowledgement->descriptor_sha256.begin(),
               acknowledgement->descriptor_sha256.end());
  bytes.insert(bytes.end(), acknowledgement->canonical_input_sha256.begin(),
               acknowledgement->canonical_input_sha256.end());
  bytes.insert(bytes.end(), acknowledgement->request_evidence_sha256.begin(),
               acknowledgement->request_evidence_sha256.end());
  bytes.insert(bytes.end(), 32, 0);
  if (bytes.size() != 208) return {};
  return FinishAck(acknowledgement, std::move(bytes), 176, true);
}

std::vector<std::uint8_t> statement_management_catalog_epoch_check_ack(
    StatementCatalogEpochCheckBindAckV1* acknowledgement) {
  if (acknowledgement == nullptr ||
      !valid_engine_identity(
          acknowledgement->authenticated_receipt_uuid) ||
      acknowledgement->occurrence == 0 ||
      !valid_engine_identity(acknowledgement->binding_uuid) ||
      acknowledgement->binding_generation == 0 ||
      !valid_engine_identity(acknowledgement->check_uuid) ||
      (acknowledgement->object_uuid.is_nil() !=
       (acknowledgement->object_generation == 0)) ||
      (!acknowledgement->object_uuid.is_nil() &&
       !valid_engine_identity(acknowledgement->object_uuid)) ||
      !valid_engine_identity(acknowledgement->schema_tree_uuid) ||
      acknowledgement->schema_tree_generation == 0 ||
      !bulk_import_nonzero_hash(
          acknowledgement->visibility_scope_sha256) ||
      !bulk_import_nonzero_hash(acknowledgement->descriptor_sha256) ||
      !bulk_import_nonzero_hash(
          acknowledgement->request_evidence_sha256)) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'C', 'E', 'B', 'A'});
  bulk_import_append_u16(&bytes, 1);
  bulk_import_append_u16(&bytes, 256);
  bulk_import_append_u32(&bytes, 256);
  bulk_import_append_u32(&bytes, 0);
  if (!bulk_import_append_uuid(
          &bytes, acknowledgement->authenticated_receipt_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->occurrence);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->binding_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->binding_generation);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->check_uuid)) {
    return {};
  }
  if (acknowledgement->object_uuid.is_nil()) {
    bytes.insert(bytes.end(), 16, 0);
  } else if (!bulk_import_append_uuid(&bytes, acknowledgement->object_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->object_generation);
  if (!bulk_import_append_uuid(&bytes,
                               acknowledgement->schema_tree_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes,
                         acknowledgement->schema_tree_generation);
  bytes.insert(bytes.end(),
               acknowledgement->visibility_scope_sha256.begin(),
               acknowledgement->visibility_scope_sha256.end());
  bytes.insert(bytes.end(),
               acknowledgement->descriptor_sha256.begin(),
               acknowledgement->descriptor_sha256.end());
  bytes.insert(bytes.end(),
               acknowledgement->request_evidence_sha256.begin(),
               acknowledgement->request_evidence_sha256.end());
  bytes.insert(bytes.end(), 32, 0);
  if (bytes.size() != 256) return {};
  return FinishAck(acknowledgement, std::move(bytes), 224, true);
}

std::vector<std::uint8_t> statement_management_database_attach_ack(
    StatementDatabaseAttachBindAckV1* acknowledgement) {
  if (acknowledgement == nullptr ||
      !valid_engine_identity(
          acknowledgement->authenticated_receipt_uuid) ||
      acknowledgement->occurrence == 0 ||
      !valid_engine_identity(acknowledgement->binding_uuid) ||
      acknowledgement->binding_generation == 0 ||
      !valid_engine_identity(acknowledgement->attach_uuid) ||
      !valid_engine_identity(acknowledgement->storage_uuid) ||
      !valid_engine_identity(acknowledgement->alias_uuid) ||
      !valid_engine_identity(acknowledgement->database_uuid) ||
      !valid_engine_identity(
          acknowledgement->catalog_snapshot_uuid) ||
      acknowledgement->catalog_generation == 0 ||
      !bulk_import_nonzero_hash(acknowledgement->descriptor_sha256) ||
      !bulk_import_nonzero_hash(
          acknowledgement->request_evidence_sha256)) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'D', 'A', 'B', 'A'});
  bulk_import_append_u16(&bytes, 1);
  bulk_import_append_u16(&bytes, 256);
  bulk_import_append_u32(&bytes, 256);
  bulk_import_append_u32(&bytes, 0);
  if (!bulk_import_append_uuid(
          &bytes, acknowledgement->authenticated_receipt_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->occurrence);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->binding_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->binding_generation);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->attach_uuid) ||
      !bulk_import_append_uuid(&bytes, acknowledgement->storage_uuid) ||
      !bulk_import_append_uuid(&bytes, acknowledgement->alias_uuid) ||
      !bulk_import_append_uuid(&bytes, acknowledgement->database_uuid) ||
      !bulk_import_append_uuid(&bytes,
                               acknowledgement->catalog_snapshot_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->catalog_generation);
  bytes.insert(bytes.end(), acknowledgement->descriptor_sha256.begin(),
               acknowledgement->descriptor_sha256.end());
  bytes.insert(bytes.end(), acknowledgement->request_evidence_sha256.begin(),
               acknowledgement->request_evidence_sha256.end());
  bytes.insert(bytes.end(), 32, 0);
  bytes.insert(bytes.end(), 8, 0);
  if (bytes.size() != 256) return {};
  return FinishAck(acknowledgement, std::move(bytes), 216, true);
}

std::vector<std::uint8_t> statement_management_free_ack(
    StatementFreeBindAckV1* acknowledgement) {
  if (acknowledgement == nullptr ||
      !valid_engine_identity(
          acknowledgement->authenticated_receipt_uuid) ||
      acknowledgement->occurrence == 0 ||
      !valid_engine_identity(acknowledgement->binding_uuid) ||
      acknowledgement->binding_generation == 0 ||
      !valid_engine_identity(acknowledgement->statement_uuid) ||
      !valid_engine_identity(acknowledgement->statement_name_uuid) ||
      acknowledgement->prepared_generation == 0 ||
      !bulk_import_nonzero_hash(acknowledgement->descriptor_sha256) ||
      !bulk_import_nonzero_hash(acknowledgement->request_evidence_sha256)) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'S', 'F', 'B', 'A'});
  bulk_import_append_u16(&bytes, 1);
  bulk_import_append_u16(&bytes, 208);
  bulk_import_append_u32(&bytes, 208);
  bulk_import_append_u32(&bytes, 0);
  if (!bulk_import_append_uuid(
          &bytes, acknowledgement->authenticated_receipt_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->occurrence);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->binding_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->binding_generation);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->statement_uuid) ||
      !bulk_import_append_uuid(&bytes,
                               acknowledgement->statement_name_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->prepared_generation);
  bytes.insert(bytes.end(), acknowledgement->descriptor_sha256.begin(),
               acknowledgement->descriptor_sha256.end());
  bytes.insert(bytes.end(), acknowledgement->request_evidence_sha256.begin(),
               acknowledgement->request_evidence_sha256.end());
  bytes.insert(bytes.end(), 32, 0);
  bytes.insert(bytes.end(), 8, 0);
  if (bytes.size() != 208) return {};
  return FinishAck(acknowledgement, std::move(bytes), 168, false);
}

std::vector<std::uint8_t> statement_management_cancel_ack(
    StatementCancelBindAckV1* acknowledgement) {
  if (acknowledgement == nullptr ||
      !valid_engine_identity(
          acknowledgement->authenticated_receipt_uuid) ||
      acknowledgement->occurrence == 0 ||
      !valid_engine_identity(acknowledgement->binding_uuid) ||
      acknowledgement->binding_generation == 0 ||
      !valid_engine_identity(acknowledgement->target_execution_uuid) ||
      !valid_engine_identity(acknowledgement->target_statement_uuid) ||
      !valid_engine_identity(
          acknowledgement->target_statement_receipt_uuid) ||
      !valid_engine_identity(acknowledgement->cancel_operation_uuid) ||
      (!acknowledgement->target_transaction_uuid.is_nil() &&
       !valid_engine_identity(
           acknowledgement->target_transaction_uuid)) ||
      acknowledgement->target_execution_generation == 0 ||
      acknowledgement->reason < 1 || acknowledgement->reason > 4 ||
      acknowledgement->mode < 1 || acknowledgement->mode > 2 ||
      acknowledgement->executor_availability_generation == 0 ||
      !bulk_import_nonzero_hash(acknowledgement->descriptor_sha256) ||
      !bulk_import_nonzero_hash(acknowledgement->request_evidence_sha256)) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  bytes.insert(bytes.end(), {'S', 'C', 'B', 'A'});
  bulk_import_append_u16(&bytes, 1);
  bulk_import_append_u16(&bytes, 272);
  bulk_import_append_u32(&bytes, 272);
  bulk_import_append_u32(&bytes, 0);
  if (!bulk_import_append_uuid(
          &bytes, acknowledgement->authenticated_receipt_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->occurrence);
  if (!bulk_import_append_uuid(&bytes, acknowledgement->binding_uuid)) {
    return {};
  }
  bulk_import_append_u64(&bytes, acknowledgement->binding_generation);
  for (const auto& identity : {
           acknowledgement->target_execution_uuid,
           acknowledgement->target_statement_uuid,
           acknowledgement->target_statement_receipt_uuid,
           acknowledgement->cancel_operation_uuid}) {
    if (!bulk_import_append_uuid(&bytes, identity)) return {};
  }
  if (acknowledgement->target_transaction_uuid.is_nil()) {
    bytes.insert(bytes.end(), 16, 0);
  } else if (!bulk_import_append_uuid(
                 &bytes, acknowledgement->target_transaction_uuid)) {
    return {};
  }
  bulk_import_append_u64(
      &bytes, acknowledgement->target_execution_generation);
  bytes.push_back(acknowledgement->reason);
  bytes.push_back(acknowledgement->mode);
  bytes.insert(bytes.end(), 2, 0);
  bulk_import_append_u64(&bytes, acknowledgement->deadline_monotonic_ns);
  bulk_import_append_u64(
      &bytes, acknowledgement->executor_availability_generation);
  bytes.insert(bytes.end(), acknowledgement->descriptor_sha256.begin(),
               acknowledgement->descriptor_sha256.end());
  bytes.insert(bytes.end(), acknowledgement->request_evidence_sha256.begin(),
               acknowledgement->request_evidence_sha256.end());
  bytes.insert(bytes.end(), 32, 0);
  bytes.insert(bytes.end(), 4, 0);
  if (bytes.size() != 272) return {};
  return FinishAck(acknowledgement, std::move(bytes), 236, false);
}

}  // namespace scratchbird::engine::statement_management
