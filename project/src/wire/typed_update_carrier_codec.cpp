// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "typed_update_carrier_codec.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::wire {
namespace {

namespace core_hash = scratchbird::core::hash;

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;

constexpr std::string_view kDescriptorDomain =
    "ScratchBird.SblrDmlUpdateRowsDescriptor.V1";
constexpr std::string_view kAssignmentVectorDomain =
    "ScratchBird.SblrDmlUpdateRowsAssignmentVector.V1";
constexpr std::string_view kPredicateVectorDomain =
    "ScratchBird.SblrDmlUpdateRowsPredicateVector.V1";
constexpr std::string_view kPredicateNodeDomain =
    "ScratchBird.SblrDmlUpdateRowsPredicateNode.V1";
constexpr std::string_view kRowPolicyVectorDomain =
    "ScratchBird.SblrDmlUpdateRowsRowPolicyVector.V1";
constexpr std::string_view kRowPolicyRecordDomain =
    "ScratchBird.SblrDmlUpdateRowsRowPolicyRecord.V1";
constexpr std::string_view kConstraintVectorDomain =
    "ScratchBird.SblrDmlUpdateRowsConstraintVector.V1";
constexpr std::string_view kConstraintRecordDomain =
    "ScratchBird.SblrDmlUpdateRowsConstraintRecord.V1";
constexpr std::string_view kTriggerVectorDomain =
    "ScratchBird.SblrDmlUpdateRowsTriggerVector.V1";
constexpr std::string_view kTriggerRecordDomain =
    "ScratchBird.SblrDmlUpdateRowsTriggerRecord.V1";
constexpr std::string_view kTargetOrderDomain =
    "ScratchBird.SblrDmlUpdateRowsTargetOrder.V1";
constexpr std::string_view kResourceBudgetDomain =
    "ScratchBird.SblrDmlUpdateRowsResourceBudget.V1";
constexpr std::string_view kRecoveryTokenDomain =
    "ScratchBird.SblrDmlUpdateRowsRecoveryToken.V1";
constexpr std::string_view kEffectSetDomain =
    "ScratchBird.SblrDmlUpdateRowsEffectSet.V1";
constexpr std::string_view kExecutorEvidenceDomain =
    "ScratchBird.SblrDmlUpdateRowsExecutorEvidence.V1";
constexpr std::string_view kResultDomain =
    "ScratchBird.SblrDmlUpdateRowsResult.V1";
constexpr std::string_view kJournalDomain =
    "ScratchBird.SblrDmlUpdateRowsDurableJournal.V1";
constexpr std::string_view kSecurityPolicySourceVectorDomain =
    "ScratchBird.SblrDmlUpdateRowsSecurityPolicySnapshotVector.V1";
constexpr std::string_view kSecurityPolicyCatalogRowDomain =
    "ScratchBird.SblrDmlUpdateRowsSecurityPolicyCatalogRow.V1";
constexpr std::string_view kSecurityPolicySourceRecordDomain =
    "ScratchBird.SblrDmlUpdateRowsSecurityPolicySnapshotRecord.V1";
constexpr std::string_view kSecuritySnapshotProofDomain =
    "ScratchBird.SblrDmlUpdateRowsSecuritySnapshotProof.V1";
constexpr std::string_view kMgaRecoveryReplayIdentityDomain =
    "ScratchBird.SblrDmlUpdateRowsMgaRecoveryReplayIdentity.V1";
constexpr std::string_view kMgaRecoveryObservationDomain =
    "ScratchBird.SblrDmlUpdateRowsMgaRecoveryObservation.V1";
constexpr std::string_view kDatatypeAuthorityVectorDomain =
    "ScratchBird.SblrDmlUpdateRowsDatatypeAuthorityVector.V1";
constexpr std::string_view kDatatypeAuthorityRecordDomain =
    "ScratchBird.SblrDmlUpdateRowsDatatypeAuthorityRecord.V1";
constexpr std::string_view kBuiltinOperatorAuthorityVectorDomain =
    "ScratchBird.SblrDmlUpdateRowsBuiltinOperatorAuthorityVector.V1";
constexpr std::string_view kBuiltinOperatorAuthorityRecordDomain =
    "ScratchBird.SblrDmlUpdateRowsBuiltinOperatorAuthorityRecord.V1";

constexpr std::string_view kBooleanCodec = "datatype.boolean.u8.v1";

constexpr const char* kOperandInvalid = "SBLR.OPERAND_INVALID";
constexpr const char* kAssignmentInvalid = "DML.ASSIGNMENT_SHAPE_INVALID";
constexpr const char* kDatatypeInvalid = "DATATYPE.DESCRIPTOR_INVALID";
constexpr const char* kResourceExceeded = "RESOURCE.BUDGET_EXCEEDED";
constexpr const char* kUpdateFailed = "DML.UPDATE_FAILED";
constexpr const char* kTransactionStale = "MGA.TRANSACTION.STALE";

bool Fail(TypedUpdateCarrierError* error,
          TypedUpdateCarrierErrorCode code,
          std::string diagnostic,
          TypedUpdateCarrierKind carrier,
          std::string field,
          u32 record_index,
          std::string detail) {
  if (error != nullptr) {
    error->code = code;
    error->diagnostic_code = std::move(diagnostic);
    error->carrier = carrier;
    error->field = std::move(field);
    error->record_index = record_index;
    error->detail = std::move(detail);
  }
  return false;
}

void ClearError(TypedUpdateCarrierError* error) {
  if (error != nullptr) {
    *error = {};
  }
}

bool UuidPresent(const TypedUpdateUuid& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool HashPresent(const TypedUpdateHash& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool AllZero(std::span<const byte> bytes) {
  return std::none_of(bytes.begin(), bytes.end(),
                      [](byte octet) { return octet != 0; });
}

bool NilGenerationPair(const TypedUpdateUuid& uuid, u64 generation) {
  return UuidPresent(uuid) == (generation != 0);
}

bool ValidCodecId(std::string_view value) {
  if (value.empty() || value.size() > 255) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-' || character == ':';
  });
}

bool ValidUtf8EvidenceField(std::string_view value, bool forbid_equals) {
  if (value.empty()) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  std::size_t offset = 0;
  while (offset < value.size()) {
    const unsigned char first = bytes[offset];
    if (first <= 0x7f) {
      if (first == 0 || (forbid_equals && first == '=')) {
        return false;
      }
      ++offset;
      continue;
    }
    std::size_t length = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
    } else {
      return false;
    }
    if (offset + length > value.size()) {
      return false;
    }
    for (std::size_t index = 1; index < length; ++index) {
      if ((bytes[offset + index] & 0xc0) != 0x80) {
        return false;
      }
    }
    if ((first == 0xe0 && bytes[offset + 1] < 0xa0) ||
        (first == 0xed && bytes[offset + 1] > 0x9f) ||
        (first == 0xf0 && bytes[offset + 1] < 0x90) ||
        (first == 0xf4 && bytes[offset + 1] > 0x8f)) {
      return false;
    }
    offset += length;
  }
  return true;
}

void StoreUuid(std::vector<byte>* bytes,
               std::size_t offset,
               const TypedUpdateUuid& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

void StoreHash(std::vector<byte>* bytes,
               std::size_t offset,
               const TypedUpdateHash& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

TypedUpdateUuid LoadUuid(std::span<const byte> bytes, std::size_t offset) {
  TypedUpdateUuid value{};
  std::copy_n(bytes.begin() + offset, value.size(), value.begin());
  return value;
}

TypedUpdateHash LoadHash(std::span<const byte> bytes, std::size_t offset) {
  TypedUpdateHash value{};
  std::copy_n(bytes.begin() + offset, value.size(), value.begin());
  return value;
}

bool ComputeHash(std::span<const byte> material,
                 TypedUpdateHash* result,
                 TypedUpdateCarrierError* error,
                 TypedUpdateCarrierKind carrier,
                 std::string field,
                 u32 record_index = 0,
                 const char* diagnostic = kOperandInvalid) {
  const auto digest =
      core_hash::ComputeSha256Digest(material.data(), material.size());
  if (!digest.ok()) {
    return Fail(error, TypedUpdateCarrierErrorCode::hash_failure,
                diagnostic, carrier, std::move(field), record_index,
                "SHA-256 computation failed");
  }
  std::copy(digest.digest.begin(), digest.digest.end(), result->begin());
  return true;
}

bool ComputeEvidence(std::string_view domain,
                     std::span<const byte> material,
                     TypedUpdateHash* result,
                     TypedUpdateCarrierError* error,
                     TypedUpdateCarrierKind carrier,
                     std::string field,
                     u32 record_index = 0,
                     const char* diagnostic = kOperandInvalid) {
  if (domain.size() > std::numeric_limits<std::size_t>::max() -
                          material.size()) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                diagnostic, carrier, std::move(field), record_index,
                "domain plus material length overflows size_t");
  }
  std::vector<byte> input;
  input.reserve(domain.size() + material.size());
  for (char character : domain) {
    input.push_back(static_cast<byte>(character));
  }
  input.insert(input.end(), material.begin(), material.end());
  return ComputeHash(input, result, error, carrier, std::move(field),
                     record_index, diagnostic);
}

void StoreHeader(std::vector<byte>* encoded,
                 std::string_view magic,
                 u16 header_bytes,
                 u32 total_bytes) {
  std::copy(magic.begin(), magic.end(), encoded->begin());
  StoreLittle16(encoded->data() + 4, kTypedUpdateCarrierVersion);
  StoreLittle16(encoded->data() + 6, header_bytes);
  StoreLittle32(encoded->data() + 8, total_bytes);
  StoreLittle32(encoded->data() + 12, 0);
}

bool ValidateHeader(std::span<const byte> encoded,
                    std::string_view magic,
                    u16 header_bytes,
                    std::optional<u32> exact_total_bytes,
                    TypedUpdateCarrierKind carrier,
                    TypedUpdateCarrierError* error,
                    const char* diagnostic = kOperandInvalid) {
  if (encoded.size() < 4) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                diagnostic, carrier, "header", 0,
                "carrier is shorter than its magic");
  }
  if (!std::equal(magic.begin(), magic.end(), encoded.begin())) {
    return Fail(error, TypedUpdateCarrierErrorCode::magic_invalid,
                diagnostic, carrier, "magic", 0,
                "carrier magic does not match its Core layout");
  }
  if (encoded.size() < 16) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                diagnostic, carrier, "header", 0,
                "carrier is shorter than the fixed header");
  }
  if (LoadLittle16(encoded.data() + 4) != kTypedUpdateCarrierVersion) {
    return Fail(error, TypedUpdateCarrierErrorCode::version_invalid,
                diagnostic, carrier, "version", 0,
                "carrier version is not exact v1");
  }
  const u32 total_bytes = LoadLittle32(encoded.data() + 8);
  if (LoadLittle16(encoded.data() + 6) != header_bytes ||
      total_bytes != encoded.size() ||
      (exact_total_bytes.has_value() &&
       total_bytes != exact_total_bytes.value())) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                diagnostic, carrier, "extent", 0,
                "header or total extent is not exact");
  }
  if (LoadLittle32(encoded.data() + 12) != 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::flags_invalid,
                diagnostic, carrier, "flags", 0,
                "v1 flags must be zero");
  }
  return true;
}

bool RequireUuidGeneration(const TypedUpdateUuid& uuid,
                           u64 generation,
                           TypedUpdateCarrierKind carrier,
                           std::string field,
                           TypedUpdateCarrierError* error,
                           u32 record_index = 0) {
  if (!UuidPresent(uuid)) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kOperandInvalid, carrier, std::move(field), record_index,
                "required UUID is zero");
  }
  if (generation == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kOperandInvalid, carrier, std::move(field), record_index,
                "required generation is zero");
  }
  return true;
}

bool ValidateDescriptorFields(const TypedUpdateDescriptorCarrier& value,
                              TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::descriptor;
  const std::pair<const TypedUpdateUuid*, u64> identities[] = {
      {&value.descriptor_uuid, value.descriptor_generation},
      {&value.operation_uuid, value.operation_generation},
      {&value.owning_transaction_uuid, value.owning_local_transaction_id},
      {&value.catalog_snapshot_uuid, value.catalog_generation},
      {&value.security_context_uuid, value.security_generation},
      {&value.target_relation_uuid, value.target_relation_generation},
      {&value.target_relation_occurrence_uuid,
       value.target_relation_occurrence_generation},
      {&value.assignment_vector_uuid, value.assignment_vector_generation},
      {&value.predicate_expression_uuid,
       value.predicate_expression_generation},
      {&value.row_policy_set_uuid, value.row_policy_set_generation},
      {&value.constraint_set_uuid, value.constraint_set_generation},
      {&value.trigger_set_uuid, value.trigger_set_generation},
      {&value.deterministic_target_order_uuid,
       value.deterministic_target_order_generation},
      {&value.resource_budget_uuid, value.resource_budget_generation},
      {&value.recovery_token_uuid, value.recovery_generation},
      {&value.builtin_operator_snapshot_uuid,
       value.builtin_operator_registry_generation},
  };
  for (const auto& [uuid, generation] : identities) {
    if (!UuidPresent(*uuid)) {
      return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                  kOperandInvalid, carrier, "identity_uuid", 0,
                  "one required descriptor UUID is zero");
    }
    if (generation == 0) {
      return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                  kOperandInvalid, carrier, "identity_generation", 0,
                  "one required descriptor generation is zero");
    }
  }
  const TypedUpdateUuid* uuid_only[] = {
      &value.authenticated_statement_receipt_uuid,
      &value.statement_snapshot_uuid,
      &value.security_snapshot_uuid,
  };
  if (std::any_of(std::begin(uuid_only), std::end(uuid_only),
                  [](const TypedUpdateUuid* uuid) {
                    return !UuidPresent(*uuid);
                  })) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kOperandInvalid, carrier, "snapshot_or_receipt_uuid", 0,
                "receipt and snapshot UUIDs must be nonzero");
  }
  if (value.structural_occurrence_id == 0 ||
      value.owning_local_transaction_id == 0 ||
      value.datatype_registry_generation == 0 ||
      value.executor_availability_generation == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kOperandInvalid, carrier, "scalar_generation", 0,
                "occurrence, transaction, registry, and executor values are nonzero");
  }
  if (value.assignment_count == 0 ||
      value.assignment_count > kTypedUpdateMaximumAssignments ||
      (value.predicate_node_count != 1 && value.predicate_node_count != 3) ||
      value.predicate_root_node_id != value.predicate_node_count ||
      value.row_policy_count > 2 ||
      value.constraint_count > kTypedUpdateMaximumFrozenRecords ||
      value.trigger_count > kTypedUpdateMaximumFrozenRecords) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kOperandInvalid, carrier, "counts", 0,
                "descriptor counts or predicate root are outside v1 bounds");
  }
  const TypedUpdateHash* hashes[] = {
      &value.assignment_vector_sha256, &value.predicate_vector_sha256,
      &value.row_policy_set_sha256, &value.ordered_constraint_set_sha256,
      &value.ordered_trigger_set_sha256,
  };
  if (std::any_of(std::begin(hashes), std::end(hashes),
                  [](const TypedUpdateHash* hash) {
                    return !HashPresent(*hash);
                  })) {
    return Fail(error, TypedUpdateCarrierErrorCode::vector_evidence_mismatch,
                kOperandInvalid, carrier, "vector_sha256", 0,
                "every referenced vector hash must be nonzero");
  }
  if (value.builtin_operator_snapshot_uuid !=
          kTypedUpdateOperatorSnapshotUuid ||
      value.builtin_operator_registry_generation != 1) {
    return Fail(error, TypedUpdateCarrierErrorCode::operator_identity_invalid,
                kDatatypeInvalid, carrier, "builtin_operator_snapshot", 0,
                "DUDC v1 requires the exact admitted operator snapshot");
  }
  return true;
}

}  // namespace

bool EncodeTypedUpdateDescriptor(const TypedUpdateDescriptorCarrier& value,
                                 std::vector<byte>* encoded,
                                 TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::descriptor,
                "encoded", 0, "output pointer is null");
  }
  if (!ValidateDescriptorFields(value, error)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateDescriptorBytes, 0);
  StoreHeader(&result, "DUDC", kTypedUpdateDescriptorBytes,
              kTypedUpdateDescriptorBytes);
  StoreUuid(&result, 16, value.descriptor_uuid);
  StoreLittle64(result.data() + 32, value.descriptor_generation);
  StoreUuid(&result, 40, value.authenticated_statement_receipt_uuid);
  StoreLittle64(result.data() + 56, value.structural_occurrence_id);
  StoreUuid(&result, 64, value.operation_uuid);
  StoreLittle64(result.data() + 80, value.operation_generation);
  StoreUuid(&result, 88, value.owning_transaction_uuid);
  StoreLittle64(result.data() + 104, value.owning_local_transaction_id);
  StoreUuid(&result, 112, value.statement_snapshot_uuid);
  StoreUuid(&result, 128, value.catalog_snapshot_uuid);
  StoreLittle64(result.data() + 144, value.catalog_generation);
  StoreLittle64(result.data() + 152, value.datatype_registry_generation);
  StoreUuid(&result, 160, value.security_context_uuid);
  StoreUuid(&result, 176, value.security_snapshot_uuid);
  StoreLittle64(result.data() + 192, value.security_generation);
  StoreUuid(&result, 200, value.target_relation_uuid);
  StoreLittle64(result.data() + 216, value.target_relation_generation);
  StoreUuid(&result, 224, value.target_relation_occurrence_uuid);
  StoreLittle64(result.data() + 240,
                value.target_relation_occurrence_generation);
  StoreUuid(&result, 248, value.assignment_vector_uuid);
  StoreLittle64(result.data() + 264, value.assignment_vector_generation);
  StoreLittle32(result.data() + 272, value.assignment_count);
  StoreHash(&result, 280, value.assignment_vector_sha256);
  StoreUuid(&result, 312, value.predicate_expression_uuid);
  StoreLittle64(result.data() + 328,
                value.predicate_expression_generation);
  StoreLittle64(result.data() + 336, value.predicate_root_node_id);
  StoreLittle32(result.data() + 344, value.predicate_node_count);
  StoreHash(&result, 352, value.predicate_vector_sha256);
  StoreUuid(&result, 384, value.row_policy_set_uuid);
  StoreLittle64(result.data() + 400, value.row_policy_set_generation);
  StoreLittle32(result.data() + 408, value.row_policy_count);
  StoreHash(&result, 416, value.row_policy_set_sha256);
  StoreUuid(&result, 448, value.constraint_set_uuid);
  StoreLittle64(result.data() + 464, value.constraint_set_generation);
  StoreLittle32(result.data() + 472, value.constraint_count);
  StoreHash(&result, 480, value.ordered_constraint_set_sha256);
  StoreUuid(&result, 512, value.trigger_set_uuid);
  StoreLittle64(result.data() + 528, value.trigger_set_generation);
  StoreLittle32(result.data() + 536, value.trigger_count);
  StoreHash(&result, 544, value.ordered_trigger_set_sha256);
  StoreUuid(&result, 576, value.deterministic_target_order_uuid);
  StoreLittle64(result.data() + 592,
                value.deterministic_target_order_generation);
  StoreUuid(&result, 600, value.resource_budget_uuid);
  StoreLittle64(result.data() + 616, value.resource_budget_generation);
  StoreUuid(&result, 624, value.recovery_token_uuid);
  StoreLittle64(result.data() + 640, value.recovery_generation);
  StoreLittle64(result.data() + 648,
                value.executor_availability_generation);
  StoreUuid(&result, 656, value.builtin_operator_snapshot_uuid);
  StoreLittle64(result.data() + 672,
                value.builtin_operator_registry_generation);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kDescriptorDomain,
                       std::span<const byte>(result.data(), 680), &evidence,
                       error, TypedUpdateCarrierKind::descriptor,
                       "descriptor_evidence_sha256")) {
    return false;
  }
  StoreHash(&result, 680, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateDescriptor(
    std::span<const byte> encoded,
    TypedUpdateDescriptorCarrier* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::descriptor,
                "decoded", 0, "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUDC", kTypedUpdateDescriptorBytes,
                      kTypedUpdateDescriptorBytes,
                      TypedUpdateCarrierKind::descriptor, error)) {
    return false;
  }
  if (!AllZero(encoded.subspan(276, 4)) ||
      !AllZero(encoded.subspan(348, 4)) ||
      !AllZero(encoded.subspan(412, 4)) ||
      !AllZero(encoded.subspan(476, 4)) ||
      !AllZero(encoded.subspan(540, 4))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::descriptor,
                "reserved", 0, "DUDC reserved bytes must be zero");
  }
  TypedUpdateDescriptorCarrier value;
  value.descriptor_uuid = LoadUuid(encoded, 16);
  value.descriptor_generation = LoadLittle64(encoded.data() + 32);
  value.authenticated_statement_receipt_uuid = LoadUuid(encoded, 40);
  value.structural_occurrence_id = LoadLittle64(encoded.data() + 56);
  value.operation_uuid = LoadUuid(encoded, 64);
  value.operation_generation = LoadLittle64(encoded.data() + 80);
  value.owning_transaction_uuid = LoadUuid(encoded, 88);
  value.owning_local_transaction_id = LoadLittle64(encoded.data() + 104);
  value.statement_snapshot_uuid = LoadUuid(encoded, 112);
  value.catalog_snapshot_uuid = LoadUuid(encoded, 128);
  value.catalog_generation = LoadLittle64(encoded.data() + 144);
  value.datatype_registry_generation = LoadLittle64(encoded.data() + 152);
  value.security_context_uuid = LoadUuid(encoded, 160);
  value.security_snapshot_uuid = LoadUuid(encoded, 176);
  value.security_generation = LoadLittle64(encoded.data() + 192);
  value.target_relation_uuid = LoadUuid(encoded, 200);
  value.target_relation_generation = LoadLittle64(encoded.data() + 216);
  value.target_relation_occurrence_uuid = LoadUuid(encoded, 224);
  value.target_relation_occurrence_generation =
      LoadLittle64(encoded.data() + 240);
  value.assignment_vector_uuid = LoadUuid(encoded, 248);
  value.assignment_vector_generation = LoadLittle64(encoded.data() + 264);
  value.assignment_count = LoadLittle32(encoded.data() + 272);
  value.assignment_vector_sha256 = LoadHash(encoded, 280);
  value.predicate_expression_uuid = LoadUuid(encoded, 312);
  value.predicate_expression_generation = LoadLittle64(encoded.data() + 328);
  value.predicate_root_node_id = LoadLittle64(encoded.data() + 336);
  value.predicate_node_count = LoadLittle32(encoded.data() + 344);
  value.predicate_vector_sha256 = LoadHash(encoded, 352);
  value.row_policy_set_uuid = LoadUuid(encoded, 384);
  value.row_policy_set_generation = LoadLittle64(encoded.data() + 400);
  value.row_policy_count = LoadLittle32(encoded.data() + 408);
  value.row_policy_set_sha256 = LoadHash(encoded, 416);
  value.constraint_set_uuid = LoadUuid(encoded, 448);
  value.constraint_set_generation = LoadLittle64(encoded.data() + 464);
  value.constraint_count = LoadLittle32(encoded.data() + 472);
  value.ordered_constraint_set_sha256 = LoadHash(encoded, 480);
  value.trigger_set_uuid = LoadUuid(encoded, 512);
  value.trigger_set_generation = LoadLittle64(encoded.data() + 528);
  value.trigger_count = LoadLittle32(encoded.data() + 536);
  value.ordered_trigger_set_sha256 = LoadHash(encoded, 544);
  value.deterministic_target_order_uuid = LoadUuid(encoded, 576);
  value.deterministic_target_order_generation =
      LoadLittle64(encoded.data() + 592);
  value.resource_budget_uuid = LoadUuid(encoded, 600);
  value.resource_budget_generation = LoadLittle64(encoded.data() + 616);
  value.recovery_token_uuid = LoadUuid(encoded, 624);
  value.recovery_generation = LoadLittle64(encoded.data() + 640);
  value.executor_availability_generation = LoadLittle64(encoded.data() + 648);
  value.builtin_operator_snapshot_uuid = LoadUuid(encoded, 656);
  value.builtin_operator_registry_generation =
      LoadLittle64(encoded.data() + 672);
  value.descriptor_evidence_sha256 = LoadHash(encoded, 680);
  if (!ValidateDescriptorFields(value, error)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kDescriptorDomain, encoded.first(680), &evidence,
                       error, TypedUpdateCarrierKind::descriptor,
                       "descriptor_evidence_sha256") ||
      evidence != value.descriptor_evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::descriptor_evidence_mismatch,
           kOperandInvalid, TypedUpdateCarrierKind::descriptor,
           "descriptor_evidence_sha256", 0,
           "DUDC descriptor evidence does not match bytes [0,680)");
    }
    return false;
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

bool ValidateVectorIdentity(const TypedUpdateVectorIdentity& identity,
                            TypedUpdateCarrierKind carrier,
                            TypedUpdateCarrierError* error) {
  if (!RequireUuidGeneration(identity.vector_uuid,
                             identity.vector_generation, carrier,
                             "vector_identity", error) ||
      !RequireUuidGeneration(identity.owner_descriptor_uuid,
                             identity.owner_descriptor_generation, carrier,
                             "owner_descriptor_identity", error)) {
    return false;
  }
  return true;
}

bool EncodeVectorHeader(std::string_view magic,
                        std::string_view domain,
                        TypedUpdateCarrierKind carrier,
                        const TypedUpdateVectorIdentity& identity,
                        u32 record_count,
                        std::span<const byte> record_bytes,
                        std::vector<byte>* encoded,
                        TypedUpdateCarrierError* error) {
  if (!ValidateVectorIdentity(identity, carrier, error)) {
    return false;
  }
  if (record_bytes.size() > std::numeric_limits<u32>::max() -
                                kTypedUpdateVectorHeaderBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kResourceExceeded, carrier, "record_bytes", 0,
                "vector extent exceeds uint32");
  }
  const auto total_bytes = static_cast<u32>(
      kTypedUpdateVectorHeaderBytes + record_bytes.size());
  std::vector<byte> result(total_bytes, 0);
  StoreHeader(&result, magic, kTypedUpdateVectorHeaderBytes, total_bytes);
  StoreUuid(&result, 16, identity.vector_uuid);
  StoreLittle64(result.data() + 32, identity.vector_generation);
  StoreUuid(&result, 40, identity.owner_descriptor_uuid);
  StoreLittle64(result.data() + 56, identity.owner_descriptor_generation);
  StoreLittle32(result.data() + 64, record_count);
  StoreLittle32(result.data() + 68,
                static_cast<u32>(record_bytes.size()));
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(domain, record_bytes, &evidence, error, carrier,
                       "vector_sha256")) {
    return false;
  }
  StoreHash(&result, 72, evidence);
  std::copy(record_bytes.begin(), record_bytes.end(),
            result.begin() + kTypedUpdateVectorHeaderBytes);
  *encoded = std::move(result);
  return true;
}

bool DecodeVectorHeader(std::span<const byte> encoded,
                        std::string_view magic,
                        std::string_view domain,
                        TypedUpdateCarrierKind carrier,
                        u32 maximum_count,
                        TypedUpdateVectorIdentity* identity,
                        u32* record_count,
                        std::span<const byte>* records,
                        TypedUpdateCarrierError* error) {
  if (!ValidateHeader(encoded, magic, kTypedUpdateVectorHeaderBytes,
                      std::nullopt, carrier, error)) {
    return false;
  }
  const u32 count = LoadLittle32(encoded.data() + 64);
  const u32 record_bytes = LoadLittle32(encoded.data() + 68);
  if (count > maximum_count ||
      record_bytes != encoded.size() - kTypedUpdateVectorHeaderBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kOperandInvalid, carrier, "record_count_or_bytes", 0,
                "vector count or concatenated record extent is invalid");
  }
  TypedUpdateVectorIdentity parsed;
  parsed.vector_uuid = LoadUuid(encoded, 16);
  parsed.vector_generation = LoadLittle64(encoded.data() + 32);
  parsed.owner_descriptor_uuid = LoadUuid(encoded, 40);
  parsed.owner_descriptor_generation = LoadLittle64(encoded.data() + 56);
  parsed.vector_sha256 = LoadHash(encoded, 72);
  if (!ValidateVectorIdentity(parsed, carrier, error)) {
    return false;
  }
  const auto record_span = encoded.subspan(kTypedUpdateVectorHeaderBytes);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(domain, record_span, &evidence, error, carrier,
                       "vector_sha256") ||
      evidence != parsed.vector_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::vector_evidence_mismatch,
           kOperandInvalid, carrier, "vector_sha256", 0,
           "vector evidence does not match exact concatenated records");
    }
    return false;
  }
  *identity = parsed;
  *record_count = count;
  *records = record_span;
  return true;
}

bool ValidateAssignmentRecord(const TypedUpdateAssignmentRecord& record,
                              u32 expected_ordinal,
                              TypedUpdateCarrierError* error,
                              u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::assignment_vector;
  if (record.assignment_ordinal != expected_ordinal) {
    return Fail(error, TypedUpdateCarrierErrorCode::ordinal_invalid,
                kAssignmentInvalid, carrier, "assignment_ordinal",
                record_index, "assignment ordinals must be dense from one");
  }
  const std::pair<const TypedUpdateUuid*, u64> identities[] = {
      {&record.assignment_occurrence_uuid,
       record.assignment_occurrence_generation},
      {&record.target_column_occurrence_uuid,
       record.target_column_occurrence_generation},
      {&record.target_column_uuid, record.target_column_generation},
      {&record.value_descriptor_uuid, record.value_descriptor_generation},
      {&record.value_type_uuid, record.value_type_generation},
  };
  for (const auto& [uuid, generation] : identities) {
    if (!RequireUuidGeneration(*uuid, generation, carrier,
                               "assignment_identity", error,
                               record_index)) {
      return false;
    }
  }
  if (!ValidCodecId(record.codec_id) || record.codec_version == 0 ||
      record.codec_generation == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::codec_id_invalid,
                kDatatypeInvalid, carrier, "codec_identity", record_index,
                "codec ID/version/generation is not canonical and nonzero");
  }
  if (record.value_state != TypedUpdateValueState::value &&
      record.value_state != TypedUpdateValueState::null_value) {
    return Fail(error, TypedUpdateCarrierErrorCode::value_state_invalid,
                kDatatypeInvalid, carrier, "value_state", record_index,
                "assignment value state must be VALUE or NULL");
  }
  if (record.value_state == TypedUpdateValueState::null_value &&
      !record.canonical_value.empty()) {
    return Fail(error, TypedUpdateCarrierErrorCode::canonical_value_invalid,
                kDatatypeInvalid, carrier, "canonical_value", record_index,
                "NULL assignments have zero canonical value bytes");
  }
  if (record.canonical_value.size() >
      kTypedUpdateMaximumCanonicalValueBytesPerValue) {
    return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                kResourceExceeded, carrier, "canonical_value", record_index,
                "canonical value exceeds the v1 65536-byte per-value limit");
  }
  return true;
}

bool EncodeAssignmentRecord(const TypedUpdateAssignmentRecord& record,
                            u32 expected_ordinal,
                            std::vector<byte>* encoded,
                            TypedUpdateCarrierError* error,
                            u32 record_index) {
  if (!ValidateAssignmentRecord(record, expected_ordinal, error,
                                record_index)) {
    return false;
  }
  const u64 total = static_cast<u64>(kTypedUpdateAssignmentPrefixBytes) +
                    record.codec_id.size() + record.canonical_value.size();
  if (total > std::numeric_limits<u32>::max()) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kResourceExceeded, TypedUpdateCarrierKind::assignment_vector,
                "record_bytes", record_index,
                "assignment record exceeds uint32");
  }
  std::vector<byte> result(static_cast<std::size_t>(total), 0);
  StoreLittle32(result.data(), static_cast<u32>(total));
  StoreLittle32(result.data() + 4, record.assignment_ordinal);
  StoreUuid(&result, 8, record.assignment_occurrence_uuid);
  StoreLittle64(result.data() + 24,
                record.assignment_occurrence_generation);
  StoreUuid(&result, 32, record.target_column_occurrence_uuid);
  StoreLittle64(result.data() + 48,
                record.target_column_occurrence_generation);
  StoreUuid(&result, 56, record.target_column_uuid);
  StoreLittle64(result.data() + 72, record.target_column_generation);
  StoreUuid(&result, 80, record.value_descriptor_uuid);
  StoreLittle64(result.data() + 96, record.value_descriptor_generation);
  StoreUuid(&result, 104, record.value_type_uuid);
  StoreLittle64(result.data() + 120, record.value_type_generation);
  StoreLittle16(result.data() + 128,
                static_cast<u16>(record.codec_id.size()));
  StoreLittle16(result.data() + 130, record.codec_version);
  StoreLittle64(result.data() + 132, record.codec_generation);
  result[140] = static_cast<byte>(record.value_state);
  StoreLittle32(result.data() + 144,
                static_cast<u32>(record.canonical_value.size()));
  TypedUpdateHash value_hash{};
  if (!ComputeHash(record.canonical_value, &value_hash, error,
                   TypedUpdateCarrierKind::assignment_vector,
                   "canonical_value_sha256", record_index)) {
    return false;
  }
  StoreHash(&result, 148, value_hash);
  std::copy(record.codec_id.begin(), record.codec_id.end(),
            result.begin() + kTypedUpdateAssignmentPrefixBytes);
  std::copy(record.canonical_value.begin(), record.canonical_value.end(),
            result.begin() + kTypedUpdateAssignmentPrefixBytes +
                record.codec_id.size());
  *encoded = std::move(result);
  return true;
}

bool DecodeAssignmentRecord(std::span<const byte> encoded,
                            u32 expected_ordinal,
                            TypedUpdateAssignmentRecord* decoded,
                            TypedUpdateCarrierError* error,
                            u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::assignment_vector;
  if (encoded.size() < kTypedUpdateAssignmentPrefixBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kAssignmentInvalid, carrier, "record_bytes", record_index,
                "assignment record is shorter than 180 bytes");
  }
  const u32 record_bytes = LoadLittle32(encoded.data());
  const u16 codec_bytes = LoadLittle16(encoded.data() + 128);
  const u32 value_bytes = LoadLittle32(encoded.data() + 144);
  const u64 expected = static_cast<u64>(kTypedUpdateAssignmentPrefixBytes) +
                       codec_bytes + value_bytes;
  if (record_bytes != encoded.size() || expected != encoded.size()) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kAssignmentInvalid, carrier, "record_bytes", record_index,
                "assignment record extent is inconsistent");
  }
  if (!AllZero(encoded.subspan(141, 3))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kAssignmentInvalid, carrier, "reserved", record_index,
                "assignment reserved bytes must be zero");
  }
  TypedUpdateAssignmentRecord value;
  value.assignment_ordinal = LoadLittle32(encoded.data() + 4);
  value.assignment_occurrence_uuid = LoadUuid(encoded, 8);
  value.assignment_occurrence_generation = LoadLittle64(encoded.data() + 24);
  value.target_column_occurrence_uuid = LoadUuid(encoded, 32);
  value.target_column_occurrence_generation =
      LoadLittle64(encoded.data() + 48);
  value.target_column_uuid = LoadUuid(encoded, 56);
  value.target_column_generation = LoadLittle64(encoded.data() + 72);
  value.value_descriptor_uuid = LoadUuid(encoded, 80);
  value.value_descriptor_generation = LoadLittle64(encoded.data() + 96);
  value.value_type_uuid = LoadUuid(encoded, 104);
  value.value_type_generation = LoadLittle64(encoded.data() + 120);
  value.codec_version = LoadLittle16(encoded.data() + 130);
  value.codec_generation = LoadLittle64(encoded.data() + 132);
  value.value_state =
      static_cast<TypedUpdateValueState>(encoded[140]);
  value.canonical_value_sha256 = LoadHash(encoded, 148);
  value.codec_id.assign(
      reinterpret_cast<const char*>(encoded.data() +
                                    kTypedUpdateAssignmentPrefixBytes),
      codec_bytes);
  const auto value_offset = kTypedUpdateAssignmentPrefixBytes + codec_bytes;
  value.canonical_value.assign(encoded.begin() + value_offset, encoded.end());
  if (!ValidateAssignmentRecord(value, expected_ordinal, error,
                                record_index)) {
    return false;
  }
  TypedUpdateHash value_hash{};
  if (!ComputeHash(value.canonical_value, &value_hash, error, carrier,
                   "canonical_value_sha256", record_index) ||
      value_hash != value.canonical_value_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error,
           TypedUpdateCarrierErrorCode::canonical_value_hash_mismatch,
           kDatatypeInvalid, carrier, "canonical_value_sha256", record_index,
           "assignment canonical value SHA-256 does not match");
    }
    return false;
  }
  *decoded = std::move(value);
  return true;
}

}  // namespace

bool EncodeTypedUpdateAssignmentVector(const TypedUpdateAssignmentVector& value,
                                       std::vector<byte>* encoded,
                                       TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::assignment_vector,
                "encoded", 0, "output pointer is null");
  }
  if (value.records.empty() ||
      value.records.size() > kTypedUpdateMaximumAssignments) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kAssignmentInvalid,
                TypedUpdateCarrierKind::assignment_vector, "record_count", 0,
                "DUAV requires 1 through 1024 records");
  }
  std::set<TypedUpdateUuid> occurrences;
  std::set<TypedUpdateUuid> column_occurrences;
  std::set<TypedUpdateUuid> columns;
  std::vector<byte> records;
  u64 total_value_bytes = 0;
  for (u32 index = 0; index < value.records.size(); ++index) {
    const auto& record = value.records[index];
    if (!occurrences.insert(record.assignment_occurrence_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_occurrence,
                  kAssignmentInvalid,
                  TypedUpdateCarrierKind::assignment_vector,
                  "assignment_occurrence_uuid", index,
                  "duplicate assignment occurrence UUID");
    }
    if (!column_occurrences.insert(
            record.target_column_occurrence_uuid).second ||
        !columns.insert(record.target_column_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_target,
                  kAssignmentInvalid,
                  TypedUpdateCarrierKind::assignment_vector,
                  "target_column_uuid", index,
                  "duplicate target column occurrence or object UUID");
    }
    total_value_bytes += record.canonical_value.size();
    if (total_value_bytes > kTypedUpdateMaximumCanonicalValueBytes) {
      return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                  kResourceExceeded,
                  TypedUpdateCarrierKind::assignment_vector,
                  "canonical_value_bytes", index,
                  "assignment vector canonical values exceed 16 MiB");
    }
    std::vector<byte> encoded_record;
    if (!EncodeAssignmentRecord(record, index + 1, &encoded_record, error,
                                index)) {
      return false;
    }
    records.insert(records.end(), encoded_record.begin(),
                   encoded_record.end());
  }
  return EncodeVectorHeader("DUAV", kAssignmentVectorDomain,
                            TypedUpdateCarrierKind::assignment_vector,
                            value.identity,
                            static_cast<u32>(value.records.size()), records,
                            encoded, error);
}

bool DecodeAndValidateTypedUpdateAssignmentVector(
    std::span<const byte> encoded,
    TypedUpdateAssignmentVector* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::assignment_vector,
                "decoded", 0, "output pointer is null");
  }
  TypedUpdateAssignmentVector value;
  u32 count = 0;
  std::span<const byte> records;
  if (!DecodeVectorHeader(encoded, "DUAV", kAssignmentVectorDomain,
                          TypedUpdateCarrierKind::assignment_vector,
                          kTypedUpdateMaximumAssignments, &value.identity,
                          &count, &records, error)) {
    return false;
  }
  if (count == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kAssignmentInvalid,
                TypedUpdateCarrierKind::assignment_vector, "record_count", 0,
                "DUAV cannot be empty");
  }
  std::set<TypedUpdateUuid> occurrences;
  std::set<TypedUpdateUuid> column_occurrences;
  std::set<TypedUpdateUuid> columns;
  std::size_t offset = 0;
  u64 total_value_bytes = 0;
  value.records.reserve(count);
  for (u32 index = 0; index < count; ++index) {
    if (records.size() - offset < 4) {
      return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                  kAssignmentInvalid,
                  TypedUpdateCarrierKind::assignment_vector,
                  "record_bytes", index,
                  "assignment record length is truncated");
    }
    const u32 bytes = LoadLittle32(records.data() + offset);
    if (bytes < kTypedUpdateAssignmentPrefixBytes ||
        bytes > records.size() - offset) {
      return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                  kAssignmentInvalid,
                  TypedUpdateCarrierKind::assignment_vector,
                  "record_bytes", index,
                  "assignment record length exceeds vector extent");
    }
    TypedUpdateAssignmentRecord record;
    if (!DecodeAssignmentRecord(records.subspan(offset, bytes), index + 1,
                                &record, error, index)) {
      return false;
    }
    if (!occurrences.insert(record.assignment_occurrence_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_occurrence,
                  kAssignmentInvalid,
                  TypedUpdateCarrierKind::assignment_vector,
                  "assignment_occurrence_uuid", index,
                  "duplicate assignment occurrence UUID");
    }
    if (!column_occurrences.insert(
            record.target_column_occurrence_uuid).second ||
        !columns.insert(record.target_column_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_target,
                  kAssignmentInvalid,
                  TypedUpdateCarrierKind::assignment_vector,
                  "target_column_uuid", index,
                  "duplicate target column occurrence or object UUID");
    }
    total_value_bytes += record.canonical_value.size();
    if (total_value_bytes > kTypedUpdateMaximumCanonicalValueBytes) {
      return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                  kResourceExceeded,
                  TypedUpdateCarrierKind::assignment_vector,
                  "canonical_value_bytes", index,
                  "assignment vector canonical values exceed 16 MiB");
    }
    value.records.push_back(std::move(record));
    offset += bytes;
  }
  if (offset != records.size()) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kAssignmentInvalid,
                TypedUpdateCarrierKind::assignment_vector,
                "record_bytes", count,
                "DUAV has trailing or unclaimed bytes");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

bool BooleanOutputIdentity(const TypedUpdatePredicateRecord& record) {
  return record.output_descriptor_uuid == kTypedUpdateBooleanUuid &&
         record.output_descriptor_generation == 1 &&
         record.output_type_uuid == kTypedUpdateBooleanUuid &&
         record.output_type_generation == 1 &&
         record.output_codec_id == kBooleanCodec &&
         record.output_codec_version == 1 &&
         record.output_codec_generation == 1;
}

bool ValidatePredicateRecord(const TypedUpdatePredicateRecord& record,
                             u64 expected_node_id,
                             TypedUpdateCarrierError* error,
                             u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::predicate_vector;
  if (record.node_id != expected_node_id) {
    return Fail(error, TypedUpdateCarrierErrorCode::ordinal_invalid,
                kOperandInvalid, carrier, "node_id", record_index,
                "predicate node IDs must be dense from one in postorder");
  }
  if (!RequireUuidGeneration(record.node_occurrence_uuid,
                             record.node_occurrence_generation, carrier,
                             "node_occurrence_identity", error,
                             record_index) ||
      !RequireUuidGeneration(record.output_descriptor_uuid,
                             record.output_descriptor_generation, carrier,
                             "output_descriptor_identity", error,
                             record_index) ||
      !RequireUuidGeneration(record.output_type_uuid,
                             record.output_type_generation, carrier,
                             "output_type_identity", error, record_index)) {
    return false;
  }
  if (!ValidCodecId(record.output_codec_id) ||
      record.output_codec_version == 0 ||
      record.output_codec_generation == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::codec_id_invalid,
                kDatatypeInvalid, carrier, "output_codec_identity",
                record_index, "predicate output codec identity is invalid");
  }
  if (record.canonical_value.size() >
      kTypedUpdateMaximumCanonicalValueBytesPerValue) {
    return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                kResourceExceeded, carrier, "canonical_value", record_index,
                "predicate canonical value exceeds the v1 65536-byte per-value limit");
  }
  const bool relation_pair =
      NilGenerationPair(record.referenced_relation_occurrence_uuid,
                        record.referenced_relation_occurrence_generation);
  const bool column_occurrence_pair =
      NilGenerationPair(record.referenced_column_occurrence_uuid,
                        record.referenced_column_occurrence_generation);
  const bool column_pair =
      NilGenerationPair(record.referenced_column_uuid,
                        record.referenced_column_generation);
  const bool operator_pair =
      NilGenerationPair(record.operator_uuid, record.operator_generation);
  if (!relation_pair || !column_occurrence_pair || !column_pair ||
      !operator_pair) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kOperandInvalid, carrier, "optional_identity", record_index,
                "optional UUID and generation must be nil/zero together");
  }
  switch (record.node_kind) {
    case TypedUpdatePredicateNodeKind::column_reference:
      if (record.left_child_node_id != 0 ||
          record.right_child_node_id != 0 ||
          !UuidPresent(record.referenced_relation_occurrence_uuid) ||
          !UuidPresent(record.referenced_column_occurrence_uuid) ||
          !UuidPresent(record.referenced_column_uuid) ||
          UuidPresent(record.operator_uuid) ||
          record.value_state != TypedUpdateValueState::absent ||
          !record.canonical_value.empty()) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::predicate_shape_invalid,
                    kOperandInvalid, carrier, "column_reference", record_index,
                    "column-reference node has noncanonical fields");
      }
      break;
    case TypedUpdatePredicateNodeKind::typed_literal:
      if (record.left_child_node_id != 0 ||
          record.right_child_node_id != 0 ||
          UuidPresent(record.referenced_relation_occurrence_uuid) ||
          UuidPresent(record.referenced_column_occurrence_uuid) ||
          UuidPresent(record.referenced_column_uuid) ||
          UuidPresent(record.operator_uuid) ||
          record.value_state != TypedUpdateValueState::value) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::predicate_shape_invalid,
                    kOperandInvalid, carrier, "typed_literal", record_index,
                    "typed literal must be non-NULL and reference-free");
      }
      break;
    case TypedUpdatePredicateNodeKind::comparison:
      if (record.left_child_node_id == 0 ||
          record.right_child_node_id == 0 ||
          UuidPresent(record.referenced_relation_occurrence_uuid) ||
          UuidPresent(record.referenced_column_occurrence_uuid) ||
          UuidPresent(record.referenced_column_uuid) ||
          record.value_state != TypedUpdateValueState::absent ||
          !record.canonical_value.empty()) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::predicate_shape_invalid,
                    kOperandInvalid, carrier, "comparison_shape",
                    record_index,
                    "comparison children, references, and value fields are not canonical");
      }
      if (record.operator_uuid != kTypedUpdateEqualOperatorUuid ||
          record.operator_generation != 1) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::operator_identity_invalid,
                    kOperandInvalid, carrier, "comparison", record_index,
                    "comparison must use exact equality generation 1");
      }
      if (!BooleanOutputIdentity(record)) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::boolean_identity_invalid,
                    kDatatypeInvalid, carrier, "comparison_output",
                    record_index,
                    "comparison output is not exact datatype.boolean.v1");
      }
      break;
    case TypedUpdatePredicateNodeKind::canonical_boolean_constant:
      if (record.left_child_node_id != 0 ||
          record.right_child_node_id != 0 ||
          UuidPresent(record.referenced_relation_occurrence_uuid) ||
          UuidPresent(record.referenced_column_occurrence_uuid) ||
          UuidPresent(record.referenced_column_uuid) ||
          UuidPresent(record.operator_uuid)) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::predicate_shape_invalid,
                    kOperandInvalid, carrier, "canonical_TRUE_shape",
                    record_index,
                    "canonical TRUE has children, references, or operator identity");
      }
      if (record.value_state != TypedUpdateValueState::value ||
          record.canonical_value != std::vector<byte>{1} ||
          !BooleanOutputIdentity(record)) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::boolean_identity_invalid,
                    kDatatypeInvalid, carrier, "canonical_TRUE", record_index,
                    "canonical TRUE must be exact boolean byte 01");
      }
      break;
    default:
      return Fail(error, TypedUpdateCarrierErrorCode::predicate_shape_invalid,
                  kOperandInvalid, carrier, "node_kind", record_index,
                  "predicate node kind is not admitted by v1");
  }
  return true;
}

bool EncodePredicateRecord(const TypedUpdatePredicateRecord& record,
                           u64 expected_node_id,
                           std::vector<byte>* encoded,
                           TypedUpdateCarrierError* error,
                           u32 record_index) {
  if (!ValidatePredicateRecord(record, expected_node_id, error,
                               record_index)) {
    return false;
  }
  const u64 total = static_cast<u64>(kTypedUpdatePredicatePrefixBytes) +
                    record.output_codec_id.size() +
                    record.canonical_value.size() +
                    kTypedUpdatePredicateEvidenceBytes;
  if (total > std::numeric_limits<u32>::max()) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kResourceExceeded, TypedUpdateCarrierKind::predicate_vector,
                "record_bytes", record_index,
                "predicate record exceeds uint32");
  }
  std::vector<byte> result(static_cast<std::size_t>(total), 0);
  StoreLittle32(result.data(), static_cast<u32>(total));
  StoreLittle64(result.data() + 4, record.node_id);
  StoreUuid(&result, 12, record.node_occurrence_uuid);
  StoreLittle64(result.data() + 28, record.node_occurrence_generation);
  result[36] = static_cast<byte>(record.node_kind);
  result[37] = static_cast<byte>(record.value_state);
  StoreLittle16(result.data() + 38,
                static_cast<u16>(record.output_codec_id.size()));
  StoreUuid(&result, 40, record.output_descriptor_uuid);
  StoreLittle64(result.data() + 56, record.output_descriptor_generation);
  StoreUuid(&result, 64, record.output_type_uuid);
  StoreLittle64(result.data() + 80, record.output_type_generation);
  StoreLittle16(result.data() + 88, record.output_codec_version);
  StoreLittle64(result.data() + 96, record.output_codec_generation);
  StoreLittle64(result.data() + 104, record.left_child_node_id);
  StoreLittle64(result.data() + 112, record.right_child_node_id);
  StoreUuid(&result, 120, record.referenced_relation_occurrence_uuid);
  StoreLittle64(result.data() + 136,
                record.referenced_relation_occurrence_generation);
  StoreUuid(&result, 144, record.referenced_column_occurrence_uuid);
  StoreLittle64(result.data() + 160,
                record.referenced_column_occurrence_generation);
  StoreUuid(&result, 168, record.referenced_column_uuid);
  StoreLittle64(result.data() + 184, record.referenced_column_generation);
  StoreUuid(&result, 192, record.operator_uuid);
  StoreLittle64(result.data() + 208, record.operator_generation);
  StoreLittle32(result.data() + 216,
                static_cast<u32>(record.canonical_value.size()));
  TypedUpdateHash value_hash{};
  if (!ComputeHash(record.canonical_value, &value_hash, error,
                   TypedUpdateCarrierKind::predicate_vector,
                   "canonical_value_sha256", record_index)) {
    return false;
  }
  StoreHash(&result, 220, value_hash);
  std::copy(record.output_codec_id.begin(), record.output_codec_id.end(),
            result.begin() + kTypedUpdatePredicatePrefixBytes);
  const auto value_offset = kTypedUpdatePredicatePrefixBytes +
                            record.output_codec_id.size();
  std::copy(record.canonical_value.begin(), record.canonical_value.end(),
            result.begin() + value_offset);
  const auto evidence_offset = result.size() -
                               kTypedUpdatePredicateEvidenceBytes;
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kPredicateNodeDomain,
                       std::span<const byte>(result.data(), evidence_offset),
                       &evidence, error,
                       TypedUpdateCarrierKind::predicate_vector,
                       "node_evidence_sha256", record_index)) {
    return false;
  }
  StoreHash(&result, evidence_offset, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodePredicateRecord(std::span<const byte> encoded,
                           u64 expected_node_id,
                           TypedUpdatePredicateRecord* decoded,
                           TypedUpdateCarrierError* error,
                           u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::predicate_vector;
  constexpr u32 kMinimumRecordBytes =
      kTypedUpdatePredicatePrefixBytes + kTypedUpdatePredicateEvidenceBytes;
  if (encoded.size() < kMinimumRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kOperandInvalid, carrier, "record_bytes", record_index,
                "predicate record is shorter than 284 bytes");
  }
  const u32 record_bytes = LoadLittle32(encoded.data());
  const u16 codec_bytes = LoadLittle16(encoded.data() + 38);
  const u32 value_bytes = LoadLittle32(encoded.data() + 216);
  const u64 expected = static_cast<u64>(kMinimumRecordBytes) + codec_bytes +
                       value_bytes;
  if (record_bytes != encoded.size() || expected != encoded.size()) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kOperandInvalid, carrier, "record_bytes", record_index,
                "predicate record extent is inconsistent");
  }
  if (!AllZero(encoded.subspan(90, 6))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kOperandInvalid, carrier, "reserved", record_index,
                "predicate reserved bytes must be zero");
  }
  TypedUpdatePredicateRecord value;
  value.node_id = LoadLittle64(encoded.data() + 4);
  value.node_occurrence_uuid = LoadUuid(encoded, 12);
  value.node_occurrence_generation = LoadLittle64(encoded.data() + 28);
  value.node_kind =
      static_cast<TypedUpdatePredicateNodeKind>(encoded[36]);
  value.value_state = static_cast<TypedUpdateValueState>(encoded[37]);
  value.output_descriptor_uuid = LoadUuid(encoded, 40);
  value.output_descriptor_generation = LoadLittle64(encoded.data() + 56);
  value.output_type_uuid = LoadUuid(encoded, 64);
  value.output_type_generation = LoadLittle64(encoded.data() + 80);
  value.output_codec_version = LoadLittle16(encoded.data() + 88);
  value.output_codec_generation = LoadLittle64(encoded.data() + 96);
  value.left_child_node_id = LoadLittle64(encoded.data() + 104);
  value.right_child_node_id = LoadLittle64(encoded.data() + 112);
  value.referenced_relation_occurrence_uuid = LoadUuid(encoded, 120);
  value.referenced_relation_occurrence_generation =
      LoadLittle64(encoded.data() + 136);
  value.referenced_column_occurrence_uuid = LoadUuid(encoded, 144);
  value.referenced_column_occurrence_generation =
      LoadLittle64(encoded.data() + 160);
  value.referenced_column_uuid = LoadUuid(encoded, 168);
  value.referenced_column_generation = LoadLittle64(encoded.data() + 184);
  value.operator_uuid = LoadUuid(encoded, 192);
  value.operator_generation = LoadLittle64(encoded.data() + 208);
  value.canonical_value_sha256 = LoadHash(encoded, 220);
  value.output_codec_id.assign(
      reinterpret_cast<const char*>(encoded.data() +
                                    kTypedUpdatePredicatePrefixBytes),
      codec_bytes);
  const auto value_offset = kTypedUpdatePredicatePrefixBytes + codec_bytes;
  value.canonical_value.assign(encoded.begin() + value_offset,
                               encoded.begin() + value_offset + value_bytes);
  const auto evidence_offset = encoded.size() -
                               kTypedUpdatePredicateEvidenceBytes;
  value.node_evidence_sha256 = LoadHash(encoded, evidence_offset);
  if (!ValidatePredicateRecord(value, expected_node_id, error,
                               record_index)) {
    return false;
  }
  TypedUpdateHash value_hash{};
  if (!ComputeHash(value.canonical_value, &value_hash, error, carrier,
                   "canonical_value_sha256", record_index) ||
      value_hash != value.canonical_value_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error,
           TypedUpdateCarrierErrorCode::canonical_value_hash_mismatch,
           kDatatypeInvalid, carrier, "canonical_value_sha256", record_index,
           "predicate canonical value SHA-256 does not match");
    }
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kPredicateNodeDomain, encoded.first(evidence_offset),
                       &evidence, error, carrier,
                       "node_evidence_sha256", record_index) ||
      evidence != value.node_evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::record_evidence_mismatch,
           kOperandInvalid, carrier, "node_evidence_sha256", record_index,
           "predicate node evidence does not match exact record bytes");
    }
    return false;
  }
  *decoded = std::move(value);
  return true;
}

bool ValidatePredicateGraph(const std::vector<TypedUpdatePredicateRecord>& rows,
                            TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::predicate_vector;
  std::set<TypedUpdateUuid> occurrences;
  for (u32 index = 0; index < rows.size(); ++index) {
    if (!occurrences.insert(rows[index].node_occurrence_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_occurrence,
                  kOperandInvalid, carrier, "node_occurrence_uuid", index,
                  "duplicate predicate node occurrence UUID");
    }
  }
  if (rows.size() == 1) {
    if (rows[0].node_kind !=
        TypedUpdatePredicateNodeKind::canonical_boolean_constant) {
      return Fail(error, TypedUpdateCarrierErrorCode::predicate_shape_invalid,
                  kOperandInvalid, carrier, "predicate_graph", 0,
                  "one-node predicate must be canonical TRUE");
    }
    return true;
  }
  if (rows.size() != 3 ||
      rows[0].node_kind !=
          TypedUpdatePredicateNodeKind::column_reference ||
      rows[1].node_kind != TypedUpdatePredicateNodeKind::typed_literal ||
      rows[2].node_kind != TypedUpdatePredicateNodeKind::comparison ||
      rows[2].left_child_node_id != 1 ||
      rows[2].right_child_node_id != 2 ||
      rows[0].output_descriptor_uuid != rows[1].output_descriptor_uuid ||
      rows[0].output_descriptor_generation !=
          rows[1].output_descriptor_generation ||
      rows[0].output_type_uuid != rows[1].output_type_uuid ||
      rows[0].output_type_generation != rows[1].output_type_generation ||
      rows[0].output_codec_id != rows[1].output_codec_id ||
      rows[0].output_codec_version != rows[1].output_codec_version ||
      rows[0].output_codec_generation != rows[1].output_codec_generation) {
    return Fail(error, TypedUpdateCarrierErrorCode::predicate_shape_invalid,
                kOperandInvalid, carrier, "predicate_graph", 0,
                "three-node predicate must be exact column-equals-literal");
  }
  return true;
}

}  // namespace

bool EncodeTypedUpdatePredicateVector(const TypedUpdatePredicateVector& value,
                                      std::vector<byte>* encoded,
                                      TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::predicate_vector,
                "encoded", 0, "output pointer is null");
  }
  if (value.records.size() != 1 && value.records.size() != 3) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::predicate_vector,
                "record_count", 0, "DUEV requires exactly one or three nodes");
  }
  std::vector<byte> records;
  u64 total_value_bytes = 0;
  for (u32 index = 0; index < value.records.size(); ++index) {
    total_value_bytes += value.records[index].canonical_value.size();
    if (total_value_bytes > kTypedUpdateMaximumCanonicalValueBytes) {
      return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                  kResourceExceeded,
                  TypedUpdateCarrierKind::predicate_vector,
                  "canonical_value_bytes", index,
                  "predicate values exceed 16 MiB");
    }
    std::vector<byte> encoded_record;
    if (!EncodePredicateRecord(value.records[index], index + 1,
                               &encoded_record, error, index)) {
      return false;
    }
    records.insert(records.end(), encoded_record.begin(),
                   encoded_record.end());
  }
  if (!ValidatePredicateGraph(value.records, error)) {
    return false;
  }
  return EncodeVectorHeader("DUEV", kPredicateVectorDomain,
                            TypedUpdateCarrierKind::predicate_vector,
                            value.identity,
                            static_cast<u32>(value.records.size()), records,
                            encoded, error);
}

bool DecodeAndValidateTypedUpdatePredicateVector(
    std::span<const byte> encoded,
    TypedUpdatePredicateVector* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::predicate_vector,
                "decoded", 0, "output pointer is null");
  }
  TypedUpdatePredicateVector value;
  u32 count = 0;
  std::span<const byte> records;
  if (!DecodeVectorHeader(encoded, "DUEV", kPredicateVectorDomain,
                          TypedUpdateCarrierKind::predicate_vector, 3,
                          &value.identity, &count, &records, error)) {
    return false;
  }
  if (count != 1 && count != 3) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::predicate_vector,
                "record_count", 0, "DUEV requires exactly one or three nodes");
  }
  std::size_t offset = 0;
  value.records.reserve(count);
  for (u32 index = 0; index < count; ++index) {
    if (records.size() - offset < 4) {
      return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                  kOperandInvalid,
                  TypedUpdateCarrierKind::predicate_vector,
                  "record_bytes", index,
                  "predicate record length is truncated");
    }
    const u32 bytes = LoadLittle32(records.data() + offset);
    if (bytes < kTypedUpdatePredicatePrefixBytes +
                    kTypedUpdatePredicateEvidenceBytes ||
        bytes > records.size() - offset) {
      return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                  kOperandInvalid,
                  TypedUpdateCarrierKind::predicate_vector,
                  "record_bytes", index,
                  "predicate record length exceeds vector extent");
    }
    TypedUpdatePredicateRecord record;
    if (!DecodePredicateRecord(records.subspan(offset, bytes), index + 1,
                               &record, error, index)) {
      return false;
    }
    value.records.push_back(std::move(record));
    offset += bytes;
  }
  if (offset != records.size() ||
      !ValidatePredicateGraph(value.records, error)) {
    if (offset != records.size() && error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
           kOperandInvalid, TypedUpdateCarrierKind::predicate_vector,
           "record_bytes", count, "DUEV has trailing or unclaimed bytes");
    }
    return false;
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

bool ValidateRowPolicyRecord(const TypedUpdateRowPolicyRecord& record,
                             u32 expected_ordinal,
                             TypedUpdateCarrierError* error,
                             u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::row_policy_vector;
  if (record.policy_ordinal != expected_ordinal) {
    return Fail(error, TypedUpdateCarrierErrorCode::ordinal_invalid,
                kOperandInvalid, carrier, "policy_ordinal", record_index,
                "policy ordinals must be dense from one");
  }
  if (record.phase != 1 && record.phase != 2) {
    return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                kOperandInvalid, carrier, "phase", record_index,
                "row-policy phase must be USING or WITH CHECK");
  }
  if (!RequireUuidGeneration(record.effective_policy_uuid,
                             record.effective_policy_generation, carrier,
                             "effective_policy_identity", error,
                             record_index) ||
      !RequireUuidGeneration(record.expression_uuid,
                             record.expression_generation, carrier,
                             "expression_identity", error, record_index) ||
      !RequireUuidGeneration(record.security_snapshot_uuid,
                             record.security_generation, carrier,
                             "security_snapshot_identity", error,
                             record_index)) {
    return false;
  }
  if (!HashPresent(record.expression_evidence_sha256) ||
      !HashPresent(record.source_policy_catalog_vector_sha256)) {
    return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                kOperandInvalid, carrier, "source_evidence", record_index,
                "row-policy source evidence must be nonzero");
  }
  return true;
}

bool EncodeRowPolicyRecord(const TypedUpdateRowPolicyRecord& record,
                           u32 expected_ordinal,
                           std::vector<byte>* encoded,
                           TypedUpdateCarrierError* error,
                           u32 record_index) {
  if (!ValidateRowPolicyRecord(record, expected_ordinal, error,
                               record_index)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateRowPolicyRecordBytes, 0);
  StoreLittle32(result.data(), record.policy_ordinal);
  result[4] = record.phase;
  result[5] = 1;
  StoreUuid(&result, 8, record.effective_policy_uuid);
  StoreLittle64(result.data() + 24, record.effective_policy_generation);
  StoreUuid(&result, 32, record.expression_uuid);
  StoreLittle64(result.data() + 48, record.expression_generation);
  StoreHash(&result, 56, record.expression_evidence_sha256);
  StoreUuid(&result, 88, record.security_snapshot_uuid);
  StoreLittle64(result.data() + 104, record.security_generation);
  StoreHash(&result, 112, record.source_policy_catalog_vector_sha256);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kRowPolicyRecordDomain,
                       std::span<const byte>(result.data(), 144), &evidence,
                       error, TypedUpdateCarrierKind::row_policy_vector,
                       "record_evidence_sha256", record_index)) {
    return false;
  }
  StoreHash(&result, 144, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeRowPolicyRecord(std::span<const byte> encoded,
                           u32 expected_ordinal,
                           TypedUpdateRowPolicyRecord* decoded,
                           TypedUpdateCarrierError* error,
                           u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::row_policy_vector;
  if (encoded.size() != kTypedUpdateRowPolicyRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kOperandInvalid, carrier, "record_bytes", record_index,
                "DUPV record must be exactly 176 bytes");
  }
  if (encoded[5] != 1 || !AllZero(encoded.subspan(6, 2))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kOperandInvalid, carrier, "combination_or_reserved",
                record_index,
                "row-policy combination must be one and reserved bytes zero");
  }
  TypedUpdateRowPolicyRecord value;
  value.policy_ordinal = LoadLittle32(encoded.data());
  value.phase = encoded[4];
  value.effective_policy_uuid = LoadUuid(encoded, 8);
  value.effective_policy_generation = LoadLittle64(encoded.data() + 24);
  value.expression_uuid = LoadUuid(encoded, 32);
  value.expression_generation = LoadLittle64(encoded.data() + 48);
  value.expression_evidence_sha256 = LoadHash(encoded, 56);
  value.security_snapshot_uuid = LoadUuid(encoded, 88);
  value.security_generation = LoadLittle64(encoded.data() + 104);
  value.source_policy_catalog_vector_sha256 = LoadHash(encoded, 112);
  value.record_evidence_sha256 = LoadHash(encoded, 144);
  if (!ValidateRowPolicyRecord(value, expected_ordinal, error,
                               record_index)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kRowPolicyRecordDomain, encoded.first(144), &evidence,
                       error, carrier, "record_evidence_sha256",
                       record_index) ||
      evidence != value.record_evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::record_evidence_mismatch,
           kOperandInvalid, carrier, "record_evidence_sha256", record_index,
           "DUPV record evidence mismatch");
    }
    return false;
  }
  *decoded = std::move(value);
  return true;
}

bool ValidateConstraintRecord(const TypedUpdateConstraintRecord& record,
                              u32 expected_ordinal,
                              TypedUpdateCarrierError* error,
                              u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::constraint_vector;
  if (record.constraint_ordinal != expected_ordinal) {
    return Fail(error, TypedUpdateCarrierErrorCode::ordinal_invalid,
                kOperandInvalid, carrier, "constraint_ordinal", record_index,
                "constraint ordinals must be dense from one");
  }
  if (record.constraint_class < 1 || record.constraint_class > 9 ||
      record.timing < 1 || record.timing > 5 ||
      record.reservation_mode < 1 || record.reservation_mode > 4) {
    return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                kOperandInvalid, carrier, "constraint_codes", record_index,
                "constraint class/timing/reservation code is outside v1");
  }
  if (!RequireUuidGeneration(record.constraint_uuid,
                             record.constraint_generation, carrier,
                             "constraint_identity", error, record_index) ||
      !RequireUuidGeneration(record.reservation_profile_uuid,
                             record.reservation_profile_generation, carrier,
                             "reservation_profile_identity", error,
                             record_index)) {
    return false;
  }
  if (!NilGenerationPair(record.expression_uuid,
                         record.expression_generation)) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kOperandInvalid, carrier, "expression_identity", record_index,
                "optional constraint expression UUID/generation mismatch");
  }
  if (!HashPresent(record.dependency_set_sha256)) {
    return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                kOperandInvalid, carrier, "dependency_set_sha256",
                record_index, "constraint dependency evidence is zero");
  }
  return true;
}

bool EncodeConstraintRecord(const TypedUpdateConstraintRecord& record,
                            u32 expected_ordinal,
                            std::vector<byte>* encoded,
                            TypedUpdateCarrierError* error,
                            u32 record_index) {
  if (!ValidateConstraintRecord(record, expected_ordinal, error,
                                record_index)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateConstraintRecordBytes, 0);
  StoreLittle32(result.data(), record.constraint_ordinal);
  result[4] = record.constraint_class;
  result[5] = record.timing;
  result[6] = record.reservation_mode;
  StoreUuid(&result, 8, record.constraint_uuid);
  StoreLittle64(result.data() + 24, record.constraint_generation);
  StoreUuid(&result, 32, record.expression_uuid);
  StoreLittle64(result.data() + 48, record.expression_generation);
  StoreUuid(&result, 56, record.reservation_profile_uuid);
  StoreLittle64(result.data() + 72,
                record.reservation_profile_generation);
  StoreHash(&result, 80, record.dependency_set_sha256);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kConstraintRecordDomain,
                       std::span<const byte>(result.data(), 112), &evidence,
                       error, TypedUpdateCarrierKind::constraint_vector,
                       "record_evidence_sha256", record_index)) {
    return false;
  }
  StoreHash(&result, 112, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeConstraintRecord(std::span<const byte> encoded,
                            u32 expected_ordinal,
                            TypedUpdateConstraintRecord* decoded,
                            TypedUpdateCarrierError* error,
                            u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::constraint_vector;
  if (encoded.size() != kTypedUpdateConstraintRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kOperandInvalid, carrier, "record_bytes", record_index,
                "DUCV record must be exactly 160 bytes");
  }
  if (encoded[7] != 0 || !AllZero(encoded.subspan(144, 16))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kOperandInvalid, carrier, "reserved", record_index,
                "constraint reserved bytes must be zero");
  }
  TypedUpdateConstraintRecord value;
  value.constraint_ordinal = LoadLittle32(encoded.data());
  value.constraint_class = encoded[4];
  value.timing = encoded[5];
  value.reservation_mode = encoded[6];
  value.constraint_uuid = LoadUuid(encoded, 8);
  value.constraint_generation = LoadLittle64(encoded.data() + 24);
  value.expression_uuid = LoadUuid(encoded, 32);
  value.expression_generation = LoadLittle64(encoded.data() + 48);
  value.reservation_profile_uuid = LoadUuid(encoded, 56);
  value.reservation_profile_generation = LoadLittle64(encoded.data() + 72);
  value.dependency_set_sha256 = LoadHash(encoded, 80);
  value.record_evidence_sha256 = LoadHash(encoded, 112);
  if (!ValidateConstraintRecord(value, expected_ordinal, error,
                                record_index)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kConstraintRecordDomain, encoded.first(112), &evidence,
                       error, carrier, "record_evidence_sha256",
                       record_index) ||
      evidence != value.record_evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::record_evidence_mismatch,
           kOperandInvalid, carrier, "record_evidence_sha256", record_index,
           "DUCV record evidence mismatch");
    }
    return false;
  }
  *decoded = std::move(value);
  return true;
}

bool ValidateTriggerRecord(const TypedUpdateTriggerRecord& record,
                           u32 expected_ordinal,
                           TypedUpdateCarrierError* error,
                           u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::trigger_vector;
  if (record.trigger_ordinal != expected_ordinal) {
    return Fail(error, TypedUpdateCarrierErrorCode::ordinal_invalid,
                kOperandInvalid, carrier, "trigger_ordinal", record_index,
                "trigger ordinals must be dense from one");
  }
  if (record.timing < 1 || record.timing > 4 ||
      record.security_mode < 1 || record.security_mode > 2 ||
      record.maximum_depth > kTypedUpdateMaximumTriggerDepth) {
    return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                kOperandInvalid, carrier, "trigger_profile", record_index,
                "trigger timing/security/depth is outside v1");
  }
  const std::pair<const TypedUpdateUuid*, u64> identities[] = {
      {&record.trigger_uuid, record.trigger_generation},
      {&record.body_sblr_uuid, record.body_sblr_generation},
      {&record.execution_security_context_uuid,
       record.execution_security_generation},
      {&record.recursion_profile_uuid, record.recursion_profile_generation},
  };
  for (const auto& [uuid, generation] : identities) {
    if (!RequireUuidGeneration(*uuid, generation, carrier,
                               "trigger_identity", error, record_index)) {
      return false;
    }
  }
  if (!HashPresent(record.dependency_set_sha256)) {
    return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                kOperandInvalid, carrier, "dependency_set_sha256",
                record_index, "trigger dependency evidence is zero");
  }
  return true;
}

bool EncodeTriggerRecord(const TypedUpdateTriggerRecord& record,
                         u32 expected_ordinal,
                         std::vector<byte>* encoded,
                         TypedUpdateCarrierError* error,
                         u32 record_index) {
  if (!ValidateTriggerRecord(record, expected_ordinal, error, record_index)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateTriggerRecordBytes, 0);
  StoreLittle32(result.data(), record.trigger_ordinal);
  result[4] = 2;
  result[5] = record.timing;
  result[6] = record.security_mode;
  result[7] = 0;
  StoreUuid(&result, 8, record.trigger_uuid);
  StoreLittle64(result.data() + 24, record.trigger_generation);
  StoreUuid(&result, 32, record.body_sblr_uuid);
  StoreLittle64(result.data() + 48, record.body_sblr_generation);
  StoreUuid(&result, 56, record.execution_security_context_uuid);
  StoreLittle64(result.data() + 72,
                record.execution_security_generation);
  StoreUuid(&result, 80, record.recursion_profile_uuid);
  StoreLittle64(result.data() + 96, record.recursion_profile_generation);
  StoreLittle32(result.data() + 104, record.maximum_depth);
  StoreHash(&result, 112, record.dependency_set_sha256);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kTriggerRecordDomain,
                       std::span<const byte>(result.data(), 144), &evidence,
                       error, TypedUpdateCarrierKind::trigger_vector,
                       "record_evidence_sha256", record_index)) {
    return false;
  }
  StoreHash(&result, 144, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeTriggerRecord(std::span<const byte> encoded,
                         u32 expected_ordinal,
                         TypedUpdateTriggerRecord* decoded,
                         TypedUpdateCarrierError* error,
                         u32 record_index) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::trigger_vector;
  if (encoded.size() != kTypedUpdateTriggerRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kOperandInvalid, carrier, "record_bytes", record_index,
                "DUTV record must be exactly 192 bytes");
  }
  if (encoded[4] != 2 || encoded[7] != 0 ||
      !AllZero(encoded.subspan(108, 4)) ||
      !AllZero(encoded.subspan(176, 16))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kOperandInvalid, carrier, "event_or_reserved", record_index,
                "trigger event must be UPDATE and reserved bytes zero");
  }
  TypedUpdateTriggerRecord value;
  value.trigger_ordinal = LoadLittle32(encoded.data());
  value.timing = encoded[5];
  value.security_mode = encoded[6];
  value.trigger_uuid = LoadUuid(encoded, 8);
  value.trigger_generation = LoadLittle64(encoded.data() + 24);
  value.body_sblr_uuid = LoadUuid(encoded, 32);
  value.body_sblr_generation = LoadLittle64(encoded.data() + 48);
  value.execution_security_context_uuid = LoadUuid(encoded, 56);
  value.execution_security_generation = LoadLittle64(encoded.data() + 72);
  value.recursion_profile_uuid = LoadUuid(encoded, 80);
  value.recursion_profile_generation = LoadLittle64(encoded.data() + 96);
  value.maximum_depth = LoadLittle32(encoded.data() + 104);
  value.dependency_set_sha256 = LoadHash(encoded, 112);
  value.record_evidence_sha256 = LoadHash(encoded, 144);
  if (!ValidateTriggerRecord(value, expected_ordinal, error, record_index)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kTriggerRecordDomain, encoded.first(144), &evidence,
                       error, carrier, "record_evidence_sha256",
                       record_index) ||
      evidence != value.record_evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::record_evidence_mismatch,
           kOperandInvalid, carrier, "record_evidence_sha256", record_index,
           "DUTV record evidence mismatch");
    }
    return false;
  }
  *decoded = std::move(value);
  return true;
}

template <typename Record, typename EncodeRecord>
bool EncodeFixedVector(std::string_view magic,
                       std::string_view domain,
                       TypedUpdateCarrierKind carrier,
                       const TypedUpdateVectorIdentity& identity,
                       const std::vector<Record>& rows,
                       u32 record_bytes,
                       EncodeRecord encode_record,
                       std::vector<byte>* encoded,
                       TypedUpdateCarrierError* error) {
  if (rows.size() > kTypedUpdateMaximumFrozenRecords ||
      rows.size() > std::numeric_limits<u32>::max()) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kResourceExceeded, carrier, "record_count", 0,
                "frozen vector count exceeds v1");
  }
  std::vector<byte> records;
  records.reserve(rows.size() * static_cast<std::size_t>(record_bytes));
  for (u32 index = 0; index < rows.size(); ++index) {
    std::vector<byte> bytes;
    if (!encode_record(rows[index], index + 1, &bytes, error, index)) {
      return false;
    }
    if (bytes.size() != record_bytes) {
      return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                  kOperandInvalid, carrier, "record_bytes", index,
                  "fixed record encoder returned a wrong extent");
    }
    records.insert(records.end(), bytes.begin(), bytes.end());
  }
  return EncodeVectorHeader(magic, domain, carrier, identity,
                            static_cast<u32>(rows.size()), records,
                            encoded, error);
}

}  // namespace

bool EncodeTypedUpdateRowPolicyVector(const TypedUpdateRowPolicyVector& value,
                                      std::vector<byte>* encoded,
                                      TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::row_policy_vector,
                "encoded", 0, "output pointer is null");
  }
  if (value.records.size() > 2 ||
      (value.records.size() == 2 &&
       (value.records[0].phase != 1 || value.records[1].phase != 2))) {
    return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::row_policy_vector,
                "policy_order", 0,
                "DUPV permits at most one USING then one WITH CHECK row");
  }
  return EncodeFixedVector("DUPV", kRowPolicyVectorDomain,
                           TypedUpdateCarrierKind::row_policy_vector,
                           value.identity, value.records,
                           kTypedUpdateRowPolicyRecordBytes,
                           EncodeRowPolicyRecord, encoded, error);
}

bool DecodeAndValidateTypedUpdateRowPolicyVector(
    std::span<const byte> encoded,
    TypedUpdateRowPolicyVector* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::row_policy_vector,
                "decoded", 0, "output pointer is null");
  }
  TypedUpdateRowPolicyVector value;
  u32 count = 0;
  std::span<const byte> records;
  if (!DecodeVectorHeader(encoded, "DUPV", kRowPolicyVectorDomain,
                          TypedUpdateCarrierKind::row_policy_vector, 2,
                          &value.identity, &count, &records, error)) {
    return false;
  }
  if (records.size() !=
      static_cast<std::size_t>(count) * kTypedUpdateRowPolicyRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::row_policy_vector,
                "record_bytes", 0, "DUPV fixed record extent mismatch");
  }
  value.records.reserve(count);
  for (u32 index = 0; index < count; ++index) {
    TypedUpdateRowPolicyRecord row;
    if (!DecodeRowPolicyRecord(
            records.subspan(index * kTypedUpdateRowPolicyRecordBytes,
                            kTypedUpdateRowPolicyRecordBytes),
            index + 1, &row, error, index)) {
      return false;
    }
    value.records.push_back(std::move(row));
  }
  if (value.records.size() == 2 &&
      (value.records[0].phase != 1 || value.records[1].phase != 2)) {
    return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::row_policy_vector,
                "policy_order", 0,
                "DUPV order must be USING then WITH CHECK");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

bool EncodeTypedUpdateConstraintVector(const TypedUpdateConstraintVector& value,
                                       std::vector<byte>* encoded,
                                       TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::constraint_vector,
                "encoded", 0, "output pointer is null");
  }
  if (value.records.size() > kTypedUpdateMaximumFrozenRecords) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kResourceExceeded,
                TypedUpdateCarrierKind::constraint_vector, "record_count", 0,
                "DUCV record count exceeds v1");
  }
  std::set<TypedUpdateUuid> constraint_uuids;
  for (u32 index = 0; index < value.records.size(); ++index) {
    if (!ValidateConstraintRecord(value.records[index], index + 1, error,
                                  index)) {
      return false;
    }
    if (!constraint_uuids.insert(value.records[index].constraint_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_occurrence,
                  kOperandInvalid,
                  TypedUpdateCarrierKind::constraint_vector,
                  "constraint_uuid", index,
                  "DUCV contains a duplicate constraint UUID");
    }
  }
  return EncodeFixedVector("DUCV", kConstraintVectorDomain,
                           TypedUpdateCarrierKind::constraint_vector,
                           value.identity, value.records,
                           kTypedUpdateConstraintRecordBytes,
                           EncodeConstraintRecord, encoded, error);
}

bool DecodeAndValidateTypedUpdateConstraintVector(
    std::span<const byte> encoded,
    TypedUpdateConstraintVector* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::constraint_vector,
                "decoded", 0, "output pointer is null");
  }
  TypedUpdateConstraintVector value;
  u32 count = 0;
  std::span<const byte> records;
  if (!DecodeVectorHeader(encoded, "DUCV", kConstraintVectorDomain,
                          TypedUpdateCarrierKind::constraint_vector,
                          kTypedUpdateMaximumFrozenRecords, &value.identity,
                          &count, &records, error)) {
    return false;
  }
  if (records.size() !=
      static_cast<std::size_t>(count) * kTypedUpdateConstraintRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::constraint_vector,
                "record_bytes", 0, "DUCV fixed record extent mismatch");
  }
  value.records.reserve(count);
  for (u32 index = 0; index < count; ++index) {
    TypedUpdateConstraintRecord row;
    if (!DecodeConstraintRecord(
            records.subspan(index * kTypedUpdateConstraintRecordBytes,
                            kTypedUpdateConstraintRecordBytes),
            index + 1, &row, error, index)) {
      return false;
    }
    value.records.push_back(std::move(row));
  }
  std::set<TypedUpdateUuid> constraint_uuids;
  for (u32 index = 0; index < value.records.size(); ++index) {
    if (!constraint_uuids.insert(value.records[index].constraint_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_occurrence,
                  kOperandInvalid,
                  TypedUpdateCarrierKind::constraint_vector,
                  "constraint_uuid", index,
                  "DUCV contains a duplicate constraint UUID");
    }
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

bool EncodeTypedUpdateTriggerVector(const TypedUpdateTriggerVector& value,
                                    std::vector<byte>* encoded,
                                    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::trigger_vector,
                "encoded", 0, "output pointer is null");
  }
  if (value.records.size() > kTypedUpdateMaximumFrozenRecords) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kResourceExceeded, TypedUpdateCarrierKind::trigger_vector,
                "record_count", 0, "DUTV record count exceeds v1");
  }
  std::set<TypedUpdateUuid> trigger_uuids;
  u8 prior_timing = 0;
  for (u32 index = 0; index < value.records.size(); ++index) {
    const auto& row = value.records[index];
    if (!ValidateTriggerRecord(row, index + 1, error, index)) {
      return false;
    }
    if (!trigger_uuids.insert(row.trigger_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_occurrence,
                  kOperandInvalid, TypedUpdateCarrierKind::trigger_vector,
                  "trigger_uuid", index,
                  "DUTV contains a duplicate trigger UUID");
    }
    if (index != 0 && row.timing < prior_timing) {
      return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                  kOperandInvalid, TypedUpdateCarrierKind::trigger_vector,
                  "trigger_order", index,
                  "DUTV timing codes must be nondecreasing");
    }
    prior_timing = row.timing;
  }
  return EncodeFixedVector("DUTV", kTriggerVectorDomain,
                           TypedUpdateCarrierKind::trigger_vector,
                           value.identity, value.records,
                           kTypedUpdateTriggerRecordBytes,
                           EncodeTriggerRecord, encoded, error);
}

bool DecodeAndValidateTypedUpdateTriggerVector(
    std::span<const byte> encoded,
    TypedUpdateTriggerVector* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::trigger_vector,
                "decoded", 0, "output pointer is null");
  }
  TypedUpdateTriggerVector value;
  u32 count = 0;
  std::span<const byte> records;
  if (!DecodeVectorHeader(encoded, "DUTV", kTriggerVectorDomain,
                          TypedUpdateCarrierKind::trigger_vector,
                          kTypedUpdateMaximumFrozenRecords, &value.identity,
                          &count, &records, error)) {
    return false;
  }
  if (records.size() !=
      static_cast<std::size_t>(count) * kTypedUpdateTriggerRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::trigger_vector,
                "record_bytes", 0, "DUTV fixed record extent mismatch");
  }
  value.records.reserve(count);
  for (u32 index = 0; index < count; ++index) {
    TypedUpdateTriggerRecord row;
    if (!DecodeTriggerRecord(
            records.subspan(index * kTypedUpdateTriggerRecordBytes,
                            kTypedUpdateTriggerRecordBytes),
            index + 1, &row, error, index)) {
      return false;
    }
    value.records.push_back(std::move(row));
  }
  std::set<TypedUpdateUuid> trigger_uuids;
  u8 prior_timing = 0;
  for (u32 index = 0; index < value.records.size(); ++index) {
    const auto& row = value.records[index];
    if (!trigger_uuids.insert(row.trigger_uuid).second) {
      return Fail(error, TypedUpdateCarrierErrorCode::duplicate_occurrence,
                  kOperandInvalid, TypedUpdateCarrierKind::trigger_vector,
                  "trigger_uuid", index,
                  "DUTV contains a duplicate trigger UUID");
    }
    if (index != 0 && row.timing < prior_timing) {
      return Fail(error, TypedUpdateCarrierErrorCode::frozen_set_invalid,
                  kOperandInvalid, TypedUpdateCarrierKind::trigger_vector,
                  "trigger_order", index,
                  "DUTV timing codes must be nondecreasing");
    }
    prior_timing = row.timing;
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

bool ValidateTargetOrder(const TypedUpdateTargetOrderCarrier& value,
                         TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::target_order;
  if (!RequireUuidGeneration(value.target_order_uuid,
                             value.target_order_generation, carrier,
                             "target_order_identity", error) ||
      !RequireUuidGeneration(value.target_relation_occurrence_uuid,
                             value.target_relation_occurrence_generation,
                             carrier, "relation_occurrence_identity", error)) {
    return false;
  }
  if (!UuidPresent(value.authenticated_statement_receipt_uuid) ||
      !UuidPresent(value.statement_snapshot_uuid)) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kOperandInvalid, carrier, "receipt_or_snapshot", 0,
                "target-order receipt and statement snapshot are nonzero");
  }
  if (value.maximum_candidate_rows > kTypedUpdateMaximumCandidateRows) {
    return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                kResourceExceeded, carrier, "maximum_candidate_rows", 0,
                "target-order candidate limit is outside v1");
  }
  return true;
}

bool ValidateResourceBudget(const TypedUpdateResourceBudgetCarrier& value,
                            TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::resource_budget;
  if (!RequireUuidGeneration(value.resource_budget_uuid,
                             value.resource_budget_generation, carrier,
                             "resource_budget_identity", error) ||
      !RequireUuidGeneration(value.cancellation_token_uuid,
                             value.cancellation_generation, carrier,
                             "cancellation_identity", error) ||
      !RequireUuidGeneration(value.grant_receipt_uuid,
                             value.grant_receipt_generation, carrier,
                             "grant_receipt_identity", error)) {
    return false;
  }
  if (!UuidPresent(value.authenticated_statement_receipt_uuid) ||
      !UuidPresent(value.owning_transaction_uuid)) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kOperandInvalid, carrier, "receipt_or_transaction", 0,
                "resource receipt and transaction UUIDs are nonzero");
  }
  if (value.maximum_assignments > kTypedUpdateMaximumAssignments ||
      value.maximum_predicate_nodes > kTypedUpdateMaximumPredicateNodes ||
      value.maximum_candidate_rows > kTypedUpdateMaximumCandidateRows ||
      value.maximum_trigger_depth > kTypedUpdateMaximumTriggerDepth ||
      value.maximum_effects > kTypedUpdateMaximumEffects ||
      value.maximum_total_canonical_value_bytes >
          kTypedUpdateMaximumCanonicalValueBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                kResourceExceeded, carrier, "limits", 0,
                "resource budget exceeds the admitted hard caps");
  }
  return true;
}

bool ValidateRecoveryToken(const TypedUpdateRecoveryTokenCarrier& value,
                           TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier =
      TypedUpdateCarrierKind::recovery_token;
  const std::pair<const TypedUpdateUuid*, u64> identities[] = {
      {&value.recovery_token_uuid, value.recovery_generation},
      {&value.descriptor_uuid, value.descriptor_generation},
      {&value.statement_savepoint_profile_uuid,
       value.statement_savepoint_profile_generation},
      {&value.durable_registry_uuid, value.durable_registry_generation},
  };
  for (const auto& [uuid, generation] : identities) {
    if (!RequireUuidGeneration(*uuid, generation, carrier,
                               "recovery_identity", error)) {
      return false;
    }
  }
  if (!UuidPresent(value.authenticated_statement_receipt_uuid) ||
      !UuidPresent(value.owning_transaction_uuid) ||
      !UuidPresent(value.operation_uuid)) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kOperandInvalid, carrier, "receipt_transaction_operation", 0,
                "recovery owner identities are nonzero");
  }
  return true;
}

}  // namespace

bool EncodeTypedUpdateTargetOrder(const TypedUpdateTargetOrderCarrier& value,
                                  std::vector<byte>* encoded,
                                  TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::target_order,
                "encoded", 0, "output pointer is null");
  }
  if (!ValidateTargetOrder(value, error)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateTargetOrderBytes, 0);
  StoreHeader(&result, "DUOR", kTypedUpdateTargetOrderBytes,
              kTypedUpdateTargetOrderBytes);
  StoreUuid(&result, 16, value.target_order_uuid);
  StoreLittle64(result.data() + 32, value.target_order_generation);
  StoreUuid(&result, 40, value.authenticated_statement_receipt_uuid);
  StoreUuid(&result, 56, value.target_relation_occurrence_uuid);
  StoreLittle64(result.data() + 72,
                value.target_relation_occurrence_generation);
  StoreUuid(&result, 80, value.statement_snapshot_uuid);
  result[96] = 1;
  result[97] = 1;
  StoreLittle64(result.data() + 104, value.maximum_candidate_rows);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kTargetOrderDomain,
                       std::span<const byte>(result.data(), 112), &evidence,
                       error, TypedUpdateCarrierKind::target_order,
                       "evidence_sha256")) {
    return false;
  }
  StoreHash(&result, 112, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateTargetOrder(
    std::span<const byte> encoded,
    TypedUpdateTargetOrderCarrier* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::target_order,
                "decoded", 0, "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUOR", kTypedUpdateTargetOrderBytes,
                      kTypedUpdateTargetOrderBytes,
                      TypedUpdateCarrierKind::target_order, error)) {
    return false;
  }
  if (encoded[96] != 1 || encoded[97] != 1 ||
      !AllZero(encoded.subspan(98, 6)) ||
      !AllZero(encoded.subspan(144, 16))) {
    return Fail(error, TypedUpdateCarrierErrorCode::target_order_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::target_order,
                "order_tie_or_reserved", 0,
                "DUOR requires lineage order, duplicate refusal, and zero reserved bytes");
  }
  TypedUpdateTargetOrderCarrier value;
  value.target_order_uuid = LoadUuid(encoded, 16);
  value.target_order_generation = LoadLittle64(encoded.data() + 32);
  value.authenticated_statement_receipt_uuid = LoadUuid(encoded, 40);
  value.target_relation_occurrence_uuid = LoadUuid(encoded, 56);
  value.target_relation_occurrence_generation =
      LoadLittle64(encoded.data() + 72);
  value.statement_snapshot_uuid = LoadUuid(encoded, 80);
  value.maximum_candidate_rows = LoadLittle64(encoded.data() + 104);
  value.evidence_sha256 = LoadHash(encoded, 112);
  if (!ValidateTargetOrder(value, error)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kTargetOrderDomain, encoded.first(112), &evidence,
                       error, TypedUpdateCarrierKind::target_order,
                       "evidence_sha256") ||
      evidence != value.evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::record_evidence_mismatch,
           kOperandInvalid, TypedUpdateCarrierKind::target_order,
           "evidence_sha256", 0, "DUOR evidence mismatch");
    }
    return false;
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

bool EncodeTypedUpdateResourceBudget(
    const TypedUpdateResourceBudgetCarrier& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::resource_budget,
                "encoded", 0, "output pointer is null");
  }
  if (!ValidateResourceBudget(value, error)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateResourceBudgetBytes, 0);
  StoreHeader(&result, "DUBR", kTypedUpdateResourceBudgetBytes,
              kTypedUpdateResourceBudgetBytes);
  StoreUuid(&result, 16, value.resource_budget_uuid);
  StoreLittle64(result.data() + 32, value.resource_budget_generation);
  StoreUuid(&result, 40, value.authenticated_statement_receipt_uuid);
  StoreUuid(&result, 56, value.owning_transaction_uuid);
  StoreUuid(&result, 72, value.cancellation_token_uuid);
  StoreLittle64(result.data() + 88, value.cancellation_generation);
  StoreUuid(&result, 96, value.grant_receipt_uuid);
  StoreLittle64(result.data() + 112, value.grant_receipt_generation);
  StoreLittle32(result.data() + 120, value.maximum_assignments);
  StoreLittle32(result.data() + 124, value.maximum_predicate_nodes);
  StoreLittle64(result.data() + 128, value.maximum_candidate_rows);
  StoreLittle32(result.data() + 136, value.maximum_trigger_depth);
  StoreLittle32(result.data() + 140, value.maximum_effects);
  StoreLittle64(result.data() + 144,
                value.maximum_total_canonical_value_bytes);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kResourceBudgetDomain,
                       std::span<const byte>(result.data(), 152), &evidence,
                       error, TypedUpdateCarrierKind::resource_budget,
                       "evidence_sha256")) {
    return false;
  }
  StoreHash(&result, 152, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateResourceBudget(
    std::span<const byte> encoded,
    TypedUpdateResourceBudgetCarrier* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::resource_budget,
                "decoded", 0, "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUBR", kTypedUpdateResourceBudgetBytes,
                      kTypedUpdateResourceBudgetBytes,
                      TypedUpdateCarrierKind::resource_budget, error)) {
    return false;
  }
  if (!AllZero(encoded.subspan(184, 24))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::resource_budget,
                "reserved", 0, "DUBR reserved bytes must be zero");
  }
  TypedUpdateResourceBudgetCarrier value;
  value.resource_budget_uuid = LoadUuid(encoded, 16);
  value.resource_budget_generation = LoadLittle64(encoded.data() + 32);
  value.authenticated_statement_receipt_uuid = LoadUuid(encoded, 40);
  value.owning_transaction_uuid = LoadUuid(encoded, 56);
  value.cancellation_token_uuid = LoadUuid(encoded, 72);
  value.cancellation_generation = LoadLittle64(encoded.data() + 88);
  value.grant_receipt_uuid = LoadUuid(encoded, 96);
  value.grant_receipt_generation = LoadLittle64(encoded.data() + 112);
  value.maximum_assignments = LoadLittle32(encoded.data() + 120);
  value.maximum_predicate_nodes = LoadLittle32(encoded.data() + 124);
  value.maximum_candidate_rows = LoadLittle64(encoded.data() + 128);
  value.maximum_trigger_depth = LoadLittle32(encoded.data() + 136);
  value.maximum_effects = LoadLittle32(encoded.data() + 140);
  value.maximum_total_canonical_value_bytes =
      LoadLittle64(encoded.data() + 144);
  value.evidence_sha256 = LoadHash(encoded, 152);
  if (!ValidateResourceBudget(value, error)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kResourceBudgetDomain, encoded.first(152), &evidence,
                       error, TypedUpdateCarrierKind::resource_budget,
                       "evidence_sha256") ||
      evidence != value.evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::record_evidence_mismatch,
           kOperandInvalid, TypedUpdateCarrierKind::resource_budget,
           "evidence_sha256", 0, "DUBR evidence mismatch");
    }
    return false;
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

bool EncodeTypedUpdateRecoveryToken(
    const TypedUpdateRecoveryTokenCarrier& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::recovery_token,
                "encoded", 0, "output pointer is null");
  }
  if (!ValidateRecoveryToken(value, error)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateRecoveryTokenBytes, 0);
  StoreHeader(&result, "DURC", kTypedUpdateRecoveryTokenBytes,
              kTypedUpdateRecoveryTokenBytes);
  StoreUuid(&result, 16, value.recovery_token_uuid);
  StoreLittle64(result.data() + 32, value.recovery_generation);
  StoreUuid(&result, 40, value.authenticated_statement_receipt_uuid);
  StoreUuid(&result, 56, value.owning_transaction_uuid);
  StoreUuid(&result, 72, value.operation_uuid);
  StoreUuid(&result, 88, value.descriptor_uuid);
  StoreLittle64(result.data() + 104, value.descriptor_generation);
  StoreUuid(&result, 112, value.statement_savepoint_profile_uuid);
  StoreLittle64(result.data() + 128,
                value.statement_savepoint_profile_generation);
  StoreUuid(&result, 136, value.durable_registry_uuid);
  StoreLittle64(result.data() + 152, value.durable_registry_generation);
  result[160] = 1;
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kRecoveryTokenDomain,
                       std::span<const byte>(result.data(), 168), &evidence,
                       error, TypedUpdateCarrierKind::recovery_token,
                       "evidence_sha256")) {
    return false;
  }
  StoreHash(&result, 168, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateRecoveryToken(
    std::span<const byte> encoded,
    TypedUpdateRecoveryTokenCarrier* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kOperandInvalid, TypedUpdateCarrierKind::recovery_token,
                "decoded", 0, "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DURC", kTypedUpdateRecoveryTokenBytes,
                      kTypedUpdateRecoveryTokenBytes,
                      TypedUpdateCarrierKind::recovery_token, error)) {
    return false;
  }
  if (encoded[160] != 1 || !AllZero(encoded.subspan(161, 7)) ||
      !AllZero(encoded.subspan(200, 8))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::recovery_token,
                "decision_or_reserved", 0,
                "DURC decision profile must be one and reserved bytes zero");
  }
  TypedUpdateRecoveryTokenCarrier value;
  value.recovery_token_uuid = LoadUuid(encoded, 16);
  value.recovery_generation = LoadLittle64(encoded.data() + 32);
  value.authenticated_statement_receipt_uuid = LoadUuid(encoded, 40);
  value.owning_transaction_uuid = LoadUuid(encoded, 56);
  value.operation_uuid = LoadUuid(encoded, 72);
  value.descriptor_uuid = LoadUuid(encoded, 88);
  value.descriptor_generation = LoadLittle64(encoded.data() + 104);
  value.statement_savepoint_profile_uuid = LoadUuid(encoded, 112);
  value.statement_savepoint_profile_generation =
      LoadLittle64(encoded.data() + 128);
  value.durable_registry_uuid = LoadUuid(encoded, 136);
  value.durable_registry_generation = LoadLittle64(encoded.data() + 152);
  value.evidence_sha256 = LoadHash(encoded, 168);
  if (!ValidateRecoveryToken(value, error)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kRecoveryTokenDomain, encoded.first(168), &evidence,
                       error, TypedUpdateCarrierKind::recovery_token,
                       "evidence_sha256") ||
      evidence != value.evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::record_evidence_mismatch,
           kOperandInvalid, TypedUpdateCarrierKind::recovery_token,
           "evidence_sha256", 0, "DURC evidence mismatch");
    }
    return false;
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

bool RequireSecurityUuidGeneration(const TypedUpdateUuid& uuid,
                                   u64 generation,
                                   TypedUpdateCarrierKind carrier,
                                   std::string field,
                                   TypedUpdateCarrierError* error,
                                   u32 record_index = 0) {
  if (!UuidPresent(uuid)) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kUpdateFailed, carrier, std::move(field), record_index,
                "required durable authority UUID is zero");
  }
  if (generation == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kUpdateFailed, carrier, std::move(field), record_index,
                "required durable authority generation is zero");
  }
  return true;
}

bool SecurityPolicyKeyLess(
    const TypedUpdateSecurityPolicySourceRecord& left,
    const TypedUpdateSecurityPolicySourceRecord& right) {
  if (left.phase != right.phase) {
    return left.phase < right.phase;
  }
  if (left.policy_uuid != right.policy_uuid) {
    return std::lexicographical_compare(
        left.policy_uuid.begin(), left.policy_uuid.end(),
        right.policy_uuid.begin(), right.policy_uuid.end());
  }
  return left.policy_generation < right.policy_generation;
}

bool ValidateSecurityPolicySourceRecord(
    const TypedUpdateSecurityPolicySourceRecord& record,
    u32 expected_ordinal,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::security_policy_source_vector;
  if (record.source_policy_ordinal != expected_ordinal) {
    return Fail(error, TypedUpdateCarrierErrorCode::ordinal_invalid,
                kUpdateFailed, carrier, "source_policy_ordinal",
                record_index, "DUSR ordinals must be dense from one");
  }
  if (record.phase != 1 && record.phase != 2) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::security_policy_source_invalid,
                kUpdateFailed, carrier, "phase", record_index,
                "DUSR phase must be USING(1) or WITH_CHECK(2)");
  }
  if (record.source_state != 1) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::security_policy_source_invalid,
                kUpdateFailed, carrier, "source_state", record_index,
                "DUSR source state must be committed-visible-applicable(1)");
  }
  if (!RequireSecurityUuidGeneration(
          record.policy_uuid, record.policy_generation, carrier,
          "policy_identity", error, record_index) ||
      !RequireSecurityUuidGeneration(
          record.target_relation_uuid, record.target_relation_generation,
          carrier, "target_relation_identity", error, record_index) ||
      !RequireSecurityUuidGeneration(
          record.source_expression_uuid,
          record.source_expression_generation, carrier,
          "source_expression_identity", error, record_index) ||
      !RequireSecurityUuidGeneration(
          record.catalog_snapshot_uuid, record.catalog_generation, carrier,
          "catalog_snapshot_identity", error, record_index) ||
      !RequireSecurityUuidGeneration(
          record.security_snapshot_uuid,
          record.security_snapshot_generation, carrier,
          "security_snapshot_identity", error, record_index)) {
    return false;
  }
  if (!UuidPresent(record.policy_version_uuid)) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kUpdateFailed, carrier, "policy_version_uuid",
                record_index, "DUSR policy version UUID is zero");
  }
  if (record.effective_transaction_number == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kUpdateFailed, carrier, "effective_transaction_number",
                record_index,
                "DUSR effective transaction number must be nonzero");
  }
  if (!HashPresent(record.source_expression_evidence_sha256)) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::security_policy_source_invalid,
                kUpdateFailed, carrier,
                "source_expression_evidence_sha256", record_index,
                "DUSR source expression evidence must be nonzero");
  }
  return true;
}

bool EncodeSecurityPolicySourceRecord(
    const TypedUpdateSecurityPolicySourceRecord& record,
    u32 expected_ordinal,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::security_policy_source_vector;
  if (!ValidateSecurityPolicySourceRecord(record, expected_ordinal, error,
                                          record_index)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateSecurityPolicySourceRecordBytes, 0);
  StoreLittle32(result.data(), record.source_policy_ordinal);
  result[4] = record.phase;
  result[5] = record.source_state;
  StoreUuid(&result, 8, record.policy_uuid);
  StoreLittle64(result.data() + 24, record.policy_generation);
  StoreUuid(&result, 32, record.policy_version_uuid);
  StoreLittle64(result.data() + 48,
                record.effective_transaction_number);
  StoreUuid(&result, 56, record.target_relation_uuid);
  StoreLittle64(result.data() + 72, record.target_relation_generation);
  StoreUuid(&result, 80, record.source_expression_uuid);
  StoreLittle64(result.data() + 96,
                record.source_expression_generation);
  StoreHash(&result, 104, record.source_expression_evidence_sha256);
  StoreUuid(&result, 136, record.catalog_snapshot_uuid);
  StoreLittle64(result.data() + 152, record.catalog_generation);
  StoreUuid(&result, 160, record.security_snapshot_uuid);
  StoreLittle64(result.data() + 176,
                record.security_snapshot_generation);
  TypedUpdateHash catalog_row_hash{};
  if (!ComputeEvidence(
          kSecurityPolicyCatalogRowDomain,
          std::span<const byte>(result.data(), 184), &catalog_row_hash,
          error, carrier, "source_policy_catalog_row_sha256", record_index,
          kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 184, catalog_row_hash);
  TypedUpdateHash record_hash{};
  if (!ComputeEvidence(
          kSecurityPolicySourceRecordDomain,
          std::span<const byte>(result.data(), 216), &record_hash, error,
          carrier, "record_evidence_sha256", record_index, kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 216, record_hash);
  *encoded = std::move(result);
  return true;
}

bool DecodeSecurityPolicySourceRecord(
    std::span<const byte> encoded,
    u32 expected_ordinal,
    TypedUpdateSecurityPolicySourceRecord* decoded,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::security_policy_source_vector;
  if (encoded.size() != kTypedUpdateSecurityPolicySourceRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kUpdateFailed, carrier, "record_bytes", record_index,
                "DUSR record must be exactly 256 bytes");
  }
  if (!AllZero(encoded.subspan(6, 2)) ||
      !AllZero(encoded.subspan(248, 8))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kUpdateFailed, carrier, "reserved", record_index,
                "DUSR reserved bytes must be zero");
  }
  TypedUpdateSecurityPolicySourceRecord value;
  value.source_policy_ordinal = LoadLittle32(encoded.data());
  value.phase = encoded[4];
  value.source_state = encoded[5];
  value.policy_uuid = LoadUuid(encoded, 8);
  value.policy_generation = LoadLittle64(encoded.data() + 24);
  value.policy_version_uuid = LoadUuid(encoded, 32);
  value.effective_transaction_number = LoadLittle64(encoded.data() + 48);
  value.target_relation_uuid = LoadUuid(encoded, 56);
  value.target_relation_generation = LoadLittle64(encoded.data() + 72);
  value.source_expression_uuid = LoadUuid(encoded, 80);
  value.source_expression_generation = LoadLittle64(encoded.data() + 96);
  value.source_expression_evidence_sha256 = LoadHash(encoded, 104);
  value.catalog_snapshot_uuid = LoadUuid(encoded, 136);
  value.catalog_generation = LoadLittle64(encoded.data() + 152);
  value.security_snapshot_uuid = LoadUuid(encoded, 160);
  value.security_snapshot_generation = LoadLittle64(encoded.data() + 176);
  value.source_policy_catalog_row_sha256 = LoadHash(encoded, 184);
  value.record_evidence_sha256 = LoadHash(encoded, 216);
  if (!ValidateSecurityPolicySourceRecord(value, expected_ordinal, error,
                                          record_index)) {
    return false;
  }
  TypedUpdateHash catalog_row_hash{};
  if (!ComputeEvidence(kSecurityPolicyCatalogRowDomain, encoded.first(184),
                       &catalog_row_hash, error, carrier,
                       "source_policy_catalog_row_sha256", record_index,
                       kUpdateFailed)) {
    return false;
  }
  if (catalog_row_hash != value.source_policy_catalog_row_sha256) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::record_evidence_mismatch,
                kUpdateFailed, carrier,
                "source_policy_catalog_row_sha256", record_index,
                "DUSR catalog-row evidence does not match bytes [0,184)");
  }
  TypedUpdateHash record_hash{};
  if (!ComputeEvidence(kSecurityPolicySourceRecordDomain,
                       encoded.first(216), &record_hash, error, carrier,
                       "record_evidence_sha256", record_index,
                       kUpdateFailed)) {
    return false;
  }
  if (record_hash != value.record_evidence_sha256) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::record_evidence_mismatch,
                kUpdateFailed, carrier, "record_evidence_sha256",
                record_index,
                "DUSR record evidence does not match bytes [0,216)");
  }
  *decoded = std::move(value);
  return true;
}

bool ValidateSecurityPolicyOrdering(
    std::span<const TypedUpdateSecurityPolicySourceRecord> records,
    TypedUpdateCarrierError* error) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::security_policy_source_vector;
  std::set<TypedUpdateUuid> policy_versions;
  for (u32 index = 0; index < records.size(); ++index) {
    if (index != 0 &&
        !SecurityPolicyKeyLess(records[index - 1], records[index])) {
      return Fail(error,
                  TypedUpdateCarrierErrorCode::security_policy_source_duplicate,
                  kUpdateFailed, carrier, "source_policy_order", index,
                  "DUSR keys must be strictly ordered by phase, policy UUID, and generation");
    }
    if (!policy_versions.insert(records[index].policy_version_uuid).second) {
      return Fail(error,
                  TypedUpdateCarrierErrorCode::security_policy_source_duplicate,
                  kUpdateFailed, carrier, "policy_version_uuid", index,
                  "DUSR policy version UUIDs must be unique");
    }
  }
  return true;
}

}  // namespace

bool EncodeTypedUpdateSecurityPolicySourceVector(
    const TypedUpdateSecurityPolicySourceVector& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier =
      TypedUpdateCarrierKind::security_policy_source_vector;
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "encoded", 0,
                "output pointer is null");
  }
  if (value.records.size() > kTypedUpdateMaximumFrozenRecords) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kUpdateFailed, carrier, "record_count", 0,
                "DUSV count exceeds the v1 maximum");
  }
  if (!RequireSecurityUuidGeneration(
          value.identity.vector_uuid, value.identity.vector_generation,
          carrier, "vector_identity", error) ||
      !RequireSecurityUuidGeneration(
          value.identity.owner_descriptor_uuid,
          value.identity.owner_descriptor_generation, carrier,
          "owner_descriptor_identity", error)) {
    return false;
  }
  std::vector<byte> record_bytes;
  record_bytes.reserve(value.records.size() *
                       kTypedUpdateSecurityPolicySourceRecordBytes);
  for (u32 index = 0; index < value.records.size(); ++index) {
    std::vector<byte> record;
    if (!EncodeSecurityPolicySourceRecord(value.records[index], index + 1,
                                          &record, error, index)) {
      return false;
    }
    record_bytes.insert(record_bytes.end(), record.begin(), record.end());
  }
  if (!ValidateSecurityPolicyOrdering(value.records, error)) {
    return false;
  }
  const u32 total_bytes = static_cast<u32>(
      kTypedUpdateVectorHeaderBytes + record_bytes.size());
  std::vector<byte> result(total_bytes, 0);
  StoreHeader(&result, "DUSV", kTypedUpdateVectorHeaderBytes, total_bytes);
  StoreUuid(&result, 16, value.identity.vector_uuid);
  StoreLittle64(result.data() + 32, value.identity.vector_generation);
  StoreUuid(&result, 40, value.identity.owner_descriptor_uuid);
  StoreLittle64(result.data() + 56,
                value.identity.owner_descriptor_generation);
  StoreLittle32(result.data() + 64,
                static_cast<u32>(value.records.size()));
  StoreLittle32(result.data() + 68,
                static_cast<u32>(record_bytes.size()));
  TypedUpdateHash vector_hash{};
  if (!ComputeEvidence(kSecurityPolicySourceVectorDomain, record_bytes,
                       &vector_hash, error, carrier, "vector_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 72, vector_hash);
  std::copy(record_bytes.begin(), record_bytes.end(),
            result.begin() + kTypedUpdateVectorHeaderBytes);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
    std::span<const byte> encoded,
    TypedUpdateSecurityPolicySourceVector* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier =
      TypedUpdateCarrierKind::security_policy_source_vector;
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "decoded", 0,
                "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUSV", kTypedUpdateVectorHeaderBytes,
                      std::nullopt, carrier, error, kUpdateFailed)) {
    return false;
  }
  const u32 count = LoadLittle32(encoded.data() + 64);
  const u32 record_bytes = LoadLittle32(encoded.data() + 68);
  const u64 expected_record_bytes =
      static_cast<u64>(count) *
      kTypedUpdateSecurityPolicySourceRecordBytes;
  if (count > kTypedUpdateMaximumFrozenRecords ||
      expected_record_bytes != record_bytes ||
      record_bytes != encoded.size() - kTypedUpdateVectorHeaderBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kUpdateFailed, carrier, "record_count_or_bytes", 0,
                "DUSV count and exact fixed-record extent disagree");
  }
  TypedUpdateSecurityPolicySourceVector value;
  value.identity.vector_uuid = LoadUuid(encoded, 16);
  value.identity.vector_generation = LoadLittle64(encoded.data() + 32);
  value.identity.owner_descriptor_uuid = LoadUuid(encoded, 40);
  value.identity.owner_descriptor_generation =
      LoadLittle64(encoded.data() + 56);
  value.identity.vector_sha256 = LoadHash(encoded, 72);
  if (!RequireSecurityUuidGeneration(
          value.identity.vector_uuid, value.identity.vector_generation,
          carrier, "vector_identity", error) ||
      !RequireSecurityUuidGeneration(
          value.identity.owner_descriptor_uuid,
          value.identity.owner_descriptor_generation, carrier,
          "owner_descriptor_identity", error)) {
    return false;
  }
  value.records.reserve(count);
  const auto records = encoded.subspan(kTypedUpdateVectorHeaderBytes);
  for (u32 index = 0; index < count; ++index) {
    TypedUpdateSecurityPolicySourceRecord record;
    if (!DecodeSecurityPolicySourceRecord(
            records.subspan(index *
                                kTypedUpdateSecurityPolicySourceRecordBytes,
                            kTypedUpdateSecurityPolicySourceRecordBytes),
            index + 1, &record, error, index)) {
      return false;
    }
    value.records.push_back(std::move(record));
  }
  if (!ValidateSecurityPolicyOrdering(value.records, error)) {
    return false;
  }
  TypedUpdateHash vector_hash{};
  if (!ComputeEvidence(kSecurityPolicySourceVectorDomain, records,
                       &vector_hash, error, carrier, "vector_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  if (vector_hash != value.identity.vector_sha256) {
    return Fail(error, TypedUpdateCarrierErrorCode::vector_evidence_mismatch,
                kUpdateFailed, carrier, "vector_sha256", 0,
                "DUSV evidence does not match exact concatenated records");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

bool ValidateSecuritySnapshotProofFields(
    const TypedUpdateSecuritySnapshotProof& value,
    TypedUpdateCarrierError* error) {
  constexpr auto carrier = TypedUpdateCarrierKind::security_snapshot_proof;
  const std::pair<const TypedUpdateUuid*, u64> identities[] = {
      {&value.security_snapshot_uuid,
       value.security_snapshot_generation},
      {&value.security_context_uuid, value.security_context_generation},
      {&value.operation_uuid, value.operation_generation},
      {&value.recovery_token_uuid, value.recovery_generation},
      {&value.catalog_snapshot_uuid, value.catalog_generation},
      {&value.target_relation_uuid, value.target_relation_generation},
      {&value.target_relation_occurrence_uuid,
       value.target_relation_occurrence_generation},
      {&value.descriptor_uuid, value.descriptor_generation},
      {&value.row_policy_set_uuid, value.row_policy_set_generation},
      {&value.source_policy_vector_uuid,
       value.source_policy_vector_generation},
  };
  for (const auto& [uuid, generation] : identities) {
    if (!RequireSecurityUuidGeneration(*uuid, generation, carrier,
                                       "authority_identity", error)) {
      return false;
    }
  }
  const TypedUpdateUuid* uuid_only[] = {
      &value.database_uuid,
      &value.authenticated_statement_receipt_uuid,
      &value.owning_transaction_uuid,
      &value.statement_snapshot_uuid,
  };
  if (std::any_of(std::begin(uuid_only), std::end(uuid_only),
                  [](const TypedUpdateUuid* uuid) {
                    return !UuidPresent(*uuid);
                  })) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kUpdateFailed, carrier, "authority_uuid", 0,
                "DUSP database, receipt, transaction, and statement snapshot UUIDs are nonzero");
  }
  if (value.owning_local_transaction_id == 0 ||
      value.security_epoch == 0 || value.policy_generation == 0 ||
      value.policy_catalog_epoch == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kUpdateFailed, carrier, "authority_generation", 0,
                "DUSP transaction and security/catalog epochs are nonzero");
  }
  if (value.snapshot_state != 1) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::security_snapshot_invalid,
                kUpdateFailed, carrier, "snapshot_state", 0,
                "DUSP snapshot state must be committed-immutable(1)");
  }
  if (value.row_policy_count > 2 ||
      value.source_policy_count > kTypedUpdateMaximumFrozenRecords) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kUpdateFailed, carrier, "policy_count", 0,
                "DUSP policy counts exceed the admitted profile");
  }
  if ((value.row_policy_count == 0) !=
      (value.source_policy_count == 0)) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::security_snapshot_invalid,
                kUpdateFailed, carrier, "policy_count", 0,
                "DUSP admits only both-empty or both-nonempty policy vectors");
  }
  const TypedUpdateHash* hashes[] = {
      &value.descriptor_evidence_sha256,
      &value.row_policy_set_sha256,
      &value.exact_dudc_sha256,
      &value.exact_dupv_sha256,
      &value.source_policy_catalog_vector_sha256,
  };
  if (std::any_of(std::begin(hashes), std::end(hashes),
                  [](const TypedUpdateHash* hash) {
                    return !HashPresent(*hash);
                  })) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::security_snapshot_invalid,
                kUpdateFailed, carrier, "evidence_sha256", 0,
                "DUSP referenced hashes must be nonzero");
  }
  return true;
}

}  // namespace

bool EncodeTypedUpdateSecuritySnapshotProof(
    const TypedUpdateSecuritySnapshotProof& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier = TypedUpdateCarrierKind::security_snapshot_proof;
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "encoded", 0,
                "output pointer is null");
  }
  if (!ValidateSecuritySnapshotProofFields(value, error)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateSecuritySnapshotProofBytes, 0);
  StoreHeader(&result, "DUSP", kTypedUpdateSecuritySnapshotProofBytes,
              kTypedUpdateSecuritySnapshotProofBytes);
  StoreUuid(&result, 16, value.security_snapshot_uuid);
  StoreLittle64(result.data() + 32,
                value.security_snapshot_generation);
  StoreUuid(&result, 40, value.security_context_uuid);
  StoreLittle64(result.data() + 56,
                value.security_context_generation);
  StoreLittle64(result.data() + 64, value.security_epoch);
  StoreLittle64(result.data() + 72, value.policy_generation);
  StoreUuid(&result, 80, value.database_uuid);
  StoreUuid(&result, 96,
            value.authenticated_statement_receipt_uuid);
  StoreUuid(&result, 112, value.owning_transaction_uuid);
  StoreLittle64(result.data() + 128,
                value.owning_local_transaction_id);
  StoreUuid(&result, 136, value.operation_uuid);
  StoreLittle64(result.data() + 152, value.operation_generation);
  StoreUuid(&result, 160, value.recovery_token_uuid);
  StoreLittle64(result.data() + 176, value.recovery_generation);
  StoreUuid(&result, 184, value.statement_snapshot_uuid);
  StoreUuid(&result, 200, value.catalog_snapshot_uuid);
  StoreLittle64(result.data() + 216, value.catalog_generation);
  StoreLittle64(result.data() + 224, value.policy_catalog_epoch);
  StoreUuid(&result, 232, value.target_relation_uuid);
  StoreLittle64(result.data() + 248,
                value.target_relation_generation);
  StoreUuid(&result, 256, value.target_relation_occurrence_uuid);
  StoreLittle64(result.data() + 272,
                value.target_relation_occurrence_generation);
  StoreUuid(&result, 280, value.descriptor_uuid);
  StoreLittle64(result.data() + 296, value.descriptor_generation);
  StoreUuid(&result, 304, value.row_policy_set_uuid);
  StoreLittle64(result.data() + 320,
                value.row_policy_set_generation);
  StoreUuid(&result, 328, value.source_policy_vector_uuid);
  StoreLittle64(result.data() + 344,
                value.source_policy_vector_generation);
  StoreLittle32(result.data() + 352, value.row_policy_count);
  StoreLittle32(result.data() + 356, value.source_policy_count);
  result[360] = value.snapshot_state;
  StoreHash(&result, 368, value.descriptor_evidence_sha256);
  StoreHash(&result, 400, value.row_policy_set_sha256);
  StoreHash(&result, 432, value.exact_dudc_sha256);
  StoreHash(&result, 464, value.exact_dupv_sha256);
  StoreHash(&result, 496,
            value.source_policy_catalog_vector_sha256);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kSecuritySnapshotProofDomain,
                       std::span<const byte>(result.data(), 528),
                       &evidence, error, carrier, "evidence_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 528, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateSecuritySnapshotProof(
    std::span<const byte> encoded,
    TypedUpdateSecuritySnapshotProof* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier = TypedUpdateCarrierKind::security_snapshot_proof;
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "decoded", 0,
                "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUSP",
                      kTypedUpdateSecuritySnapshotProofBytes,
                      kTypedUpdateSecuritySnapshotProofBytes, carrier,
                      error, kUpdateFailed)) {
    return false;
  }
  if (!AllZero(encoded.subspan(361, 7)) ||
      !AllZero(encoded.subspan(560, 16))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kUpdateFailed, carrier, "reserved", 0,
                "DUSP reserved bytes must be zero");
  }
  TypedUpdateSecuritySnapshotProof value;
  value.security_snapshot_uuid = LoadUuid(encoded, 16);
  value.security_snapshot_generation = LoadLittle64(encoded.data() + 32);
  value.security_context_uuid = LoadUuid(encoded, 40);
  value.security_context_generation = LoadLittle64(encoded.data() + 56);
  value.security_epoch = LoadLittle64(encoded.data() + 64);
  value.policy_generation = LoadLittle64(encoded.data() + 72);
  value.database_uuid = LoadUuid(encoded, 80);
  value.authenticated_statement_receipt_uuid = LoadUuid(encoded, 96);
  value.owning_transaction_uuid = LoadUuid(encoded, 112);
  value.owning_local_transaction_id = LoadLittle64(encoded.data() + 128);
  value.operation_uuid = LoadUuid(encoded, 136);
  value.operation_generation = LoadLittle64(encoded.data() + 152);
  value.recovery_token_uuid = LoadUuid(encoded, 160);
  value.recovery_generation = LoadLittle64(encoded.data() + 176);
  value.statement_snapshot_uuid = LoadUuid(encoded, 184);
  value.catalog_snapshot_uuid = LoadUuid(encoded, 200);
  value.catalog_generation = LoadLittle64(encoded.data() + 216);
  value.policy_catalog_epoch = LoadLittle64(encoded.data() + 224);
  value.target_relation_uuid = LoadUuid(encoded, 232);
  value.target_relation_generation = LoadLittle64(encoded.data() + 248);
  value.target_relation_occurrence_uuid = LoadUuid(encoded, 256);
  value.target_relation_occurrence_generation =
      LoadLittle64(encoded.data() + 272);
  value.descriptor_uuid = LoadUuid(encoded, 280);
  value.descriptor_generation = LoadLittle64(encoded.data() + 296);
  value.row_policy_set_uuid = LoadUuid(encoded, 304);
  value.row_policy_set_generation = LoadLittle64(encoded.data() + 320);
  value.source_policy_vector_uuid = LoadUuid(encoded, 328);
  value.source_policy_vector_generation =
      LoadLittle64(encoded.data() + 344);
  value.row_policy_count = LoadLittle32(encoded.data() + 352);
  value.source_policy_count = LoadLittle32(encoded.data() + 356);
  value.snapshot_state = encoded[360];
  value.descriptor_evidence_sha256 = LoadHash(encoded, 368);
  value.row_policy_set_sha256 = LoadHash(encoded, 400);
  value.exact_dudc_sha256 = LoadHash(encoded, 432);
  value.exact_dupv_sha256 = LoadHash(encoded, 464);
  value.source_policy_catalog_vector_sha256 = LoadHash(encoded, 496);
  value.evidence_sha256 = LoadHash(encoded, 528);
  if (!ValidateSecuritySnapshotProofFields(value, error)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kSecuritySnapshotProofDomain, encoded.first(528),
                       &evidence, error, carrier, "evidence_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  if (evidence != value.evidence_sha256) {
    return Fail(
        error,
        TypedUpdateCarrierErrorCode::security_snapshot_evidence_mismatch,
        kUpdateFailed, carrier, "evidence_sha256", 0,
        "DUSP evidence does not match bytes [0,528)");
  }
  std::vector<byte> canonical;
  if (!EncodeTypedUpdateSecuritySnapshotProof(value, &canonical, error)) {
    return false;
  }
  if (canonical.size() != encoded.size() ||
      !std::equal(canonical.begin(), canonical.end(), encoded.begin())) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::security_snapshot_invalid,
                kUpdateFailed, carrier, "exact_bytes", 0,
                "DUSP decode/validate/re-encode is not byte-identical");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

bool ValidMgaJournalState(TypedUpdateJournalState state) {
  const auto code = static_cast<u8>(state);
  return code >= static_cast<u8>(TypedUpdateJournalState::bound) &&
         code <= static_cast<u8>(TypedUpdateJournalState::aborted);
}

bool ValidTransactionState(TypedUpdateTransactionState state) {
  const auto code = static_cast<u8>(state);
  return code >= static_cast<u8>(TypedUpdateTransactionState::active_live) &&
         code <= static_cast<u8>(TypedUpdateTransactionState::quarantined);
}

bool ValidSavepointState(TypedUpdateSavepointState state) {
  const auto code = static_cast<u8>(state);
  return code <=
         static_cast<u8>(
             TypedUpdateSavepointState::released_at_statement_barrier);
}

bool ValidateMgaRecoveryObservationFields(
    const TypedUpdateMgaRecoveryObservation& value,
    TypedUpdateCarrierError* error) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::mga_recovery_observation;
  const std::pair<const TypedUpdateUuid*, u64> identities[] = {
      {&value.observation_uuid, value.observation_generation},
      {&value.validated_mga_durable_handle_uuid,
       value.validated_mga_durable_handle_generation},
      {&value.descriptor_uuid, value.descriptor_generation},
      {&value.operation_uuid, value.operation_generation},
      {&value.recovery_token_uuid, value.recovery_generation},
      {&value.reserved_statement_barrier_uuid,
       value.reserved_statement_barrier_generation},
      {&value.catalog_snapshot_uuid, value.catalog_generation},
      {&value.security_snapshot_uuid,
       value.security_snapshot_generation},
  };
  for (const auto& [uuid, generation] : identities) {
    if (!RequireSecurityUuidGeneration(*uuid, generation, carrier,
                                       "authority_identity", error)) {
      return false;
    }
  }
  if (!UuidPresent(value.database_uuid) ||
      !UuidPresent(value.authenticated_statement_receipt_uuid) ||
      !UuidPresent(value.owning_transaction_uuid)) {
    return Fail(error, TypedUpdateCarrierErrorCode::uuid_invalid,
                kUpdateFailed, carrier, "authority_uuid", 0,
                "DUMO database, receipt, and transaction UUIDs are nonzero");
  }
  if (value.owning_local_transaction_id == 0 ||
      value.durable_chain_head_sequence == 0 ||
      value.security_epoch == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kUpdateFailed, carrier, "authority_generation", 0,
                "DUMO local transaction, chain sequence, and security epoch are nonzero");
  }
  if (!HashPresent(value.durable_chain_head_record_evidence_sha256)) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                kUpdateFailed, carrier,
                "durable_chain_head_record_evidence_sha256", 0,
                "DUMO chain-head evidence must be nonzero");
  }
  if (!ValidMgaJournalState(value.latest_journal_state) ||
      !ValidTransactionState(value.transaction_state) ||
      !ValidSavepointState(value.savepoint_state)) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                kUpdateFailed, carrier, "state_code", 0,
                "DUMO journal, transaction, or savepoint state is outside v1");
  }
  const bool savepoint_present =
      UuidPresent(value.statement_savepoint_uuid);
  if (value.savepoint_state == TypedUpdateSavepointState::absent) {
    if (savepoint_present || value.statement_savepoint_generation != 0) {
      return Fail(error,
                  TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                  kUpdateFailed, carrier, "statement_savepoint_identity", 0,
                  "absent DUMO savepoint requires nil UUID and zero generation");
    }
  } else if (!RequireSecurityUuidGeneration(
                 value.statement_savepoint_uuid,
                 value.statement_savepoint_generation, carrier,
                 "statement_savepoint_identity", error)) {
    return false;
  }
  const bool active_savepoint =
      value.savepoint_state == TypedUpdateSavepointState::active;
  const bool released_savepoint =
      value.savepoint_state ==
      TypedUpdateSavepointState::released_at_statement_barrier;
  const bool rolled_back_savepoint =
      value.savepoint_state ==
      TypedUpdateSavepointState::rolled_back_final;
  const bool absent_or_rolled_back =
      value.savepoint_state == TypedUpdateSavepointState::absent ||
      rolled_back_savepoint;
  if ((active_savepoint && value.statement_barrier_present) ||
      (released_savepoint && !value.statement_barrier_present) ||
      (absent_or_rolled_back && value.statement_barrier_present)) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                kUpdateFailed, carrier, "savepoint_barrier_state", 0,
                "DUMO savepoint state contradicts its statement barrier flag");
  }
  switch (value.latest_journal_state) {
    case TypedUpdateJournalState::bound:
      if (!absent_or_rolled_back || value.statement_barrier_present ||
          !value.no_surviving_effect_proven) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                    kUpdateFailed, carrier, "bound_cutpoint", 0,
                    "bound DUMO requires absent/rolled-back savepoint, no barrier, and no surviving effect");
      }
      break;
    case TypedUpdateJournalState::intent:
      if (value.statement_barrier_present ||
          (!active_savepoint &&
           !(absent_or_rolled_back &&
             value.no_surviving_effect_proven))) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                    kUpdateFailed, carrier, "intent_cutpoint", 0,
                    "intent DUMO requires active savepoint or provider-proven rolled-back/absent state without a barrier");
      }
      break;
    case TypedUpdateJournalState::prepared:
      if (!((active_savepoint && !value.statement_barrier_present) ||
            (rolled_back_savepoint && !value.statement_barrier_present &&
             value.no_surviving_effect_proven) ||
            (released_savepoint && value.statement_barrier_present))) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                    kUpdateFailed, carrier, "prepared_cutpoint", 0,
                    "prepared DUMO requires active prebarrier, provider-proven rolled-back prebarrier, or released postbarrier state");
      }
      break;
    case TypedUpdateJournalState::published:
      if (!released_savepoint || !value.statement_barrier_present) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                    kUpdateFailed, carrier, "published_cutpoint", 0,
                    "published DUMO requires released savepoint and crossed barrier");
      }
      break;
    case TypedUpdateJournalState::aborted:
      if (!absent_or_rolled_back || value.statement_barrier_present ||
          !value.no_surviving_effect_proven) {
        return Fail(error,
                    TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                    kUpdateFailed, carrier, "aborted_cutpoint", 0,
                    "aborted DUMO requires absent/rolled-back savepoint, no barrier, and no surviving effect");
      }
      break;
  }
  return true;
}

bool ComputeMgaReplayIdentity(
    const TypedUpdateMgaRecoveryObservation& value,
    TypedUpdateHash* result,
    TypedUpdateCarrierError* error) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::mga_recovery_observation;
  std::vector<byte> material(128, 0);
  StoreUuid(&material, 0, value.database_uuid);
  StoreUuid(&material, 16, value.descriptor_uuid);
  StoreLittle64(material.data() + 32, value.descriptor_generation);
  StoreUuid(&material, 40, value.operation_uuid);
  StoreLittle64(material.data() + 56, value.operation_generation);
  StoreUuid(&material, 64,
            value.authenticated_statement_receipt_uuid);
  StoreUuid(&material, 80, value.owning_transaction_uuid);
  StoreLittle64(material.data() + 96,
                value.owning_local_transaction_id);
  StoreUuid(&material, 104, value.recovery_token_uuid);
  StoreLittle64(material.data() + 120, value.recovery_generation);
  return ComputeEvidence(kMgaRecoveryReplayIdentityDomain, material, result,
                         error, carrier,
                         "time_independent_replay_identity_sha256", 0,
                         kUpdateFailed);
}

}  // namespace

bool EncodeTypedUpdateMgaRecoveryObservation(
    const TypedUpdateMgaRecoveryObservation& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier =
      TypedUpdateCarrierKind::mga_recovery_observation;
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "encoded", 0,
                "output pointer is null");
  }
  if (!ValidateMgaRecoveryObservationFields(value, error)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateMgaRecoveryObservationBytes, 0);
  StoreHeader(&result, "DUMO", kTypedUpdateMgaRecoveryObservationBytes,
              kTypedUpdateMgaRecoveryObservationBytes);
  StoreUuid(&result, 16, value.observation_uuid);
  StoreLittle64(result.data() + 32, value.observation_generation);
  StoreUuid(&result, 40, value.validated_mga_durable_handle_uuid);
  StoreLittle64(result.data() + 56,
                value.validated_mga_durable_handle_generation);
  StoreUuid(&result, 64, value.database_uuid);
  StoreUuid(&result, 80, value.descriptor_uuid);
  StoreLittle64(result.data() + 96, value.descriptor_generation);
  StoreUuid(&result, 104, value.operation_uuid);
  StoreLittle64(result.data() + 120, value.operation_generation);
  StoreUuid(&result, 128,
            value.authenticated_statement_receipt_uuid);
  StoreUuid(&result, 144, value.owning_transaction_uuid);
  StoreLittle64(result.data() + 160,
                value.owning_local_transaction_id);
  StoreUuid(&result, 168, value.recovery_token_uuid);
  StoreLittle64(result.data() + 184, value.recovery_generation);
  result[192] = static_cast<u8>(value.latest_journal_state);
  result[193] = static_cast<u8>(value.transaction_state);
  result[194] = static_cast<u8>(value.savepoint_state);
  result[195] = value.statement_barrier_present ? 1 : 0;
  result[196] = value.no_surviving_effect_proven ? 1 : 0;
  StoreUuid(&result, 200, value.statement_savepoint_uuid);
  StoreLittle64(result.data() + 216,
                value.statement_savepoint_generation);
  StoreUuid(&result, 224,
            value.reserved_statement_barrier_uuid);
  StoreLittle64(result.data() + 240,
                value.reserved_statement_barrier_generation);
  StoreLittle64(result.data() + 248,
                value.durable_chain_head_sequence);
  StoreHash(&result, 256,
            value.durable_chain_head_record_evidence_sha256);
  StoreUuid(&result, 288, value.catalog_snapshot_uuid);
  StoreLittle64(result.data() + 304, value.catalog_generation);
  StoreUuid(&result, 312, value.security_snapshot_uuid);
  StoreLittle64(result.data() + 328,
                value.security_snapshot_generation);
  StoreLittle64(result.data() + 336, value.security_epoch);
  TypedUpdateHash replay_identity{};
  if (!ComputeMgaReplayIdentity(value, &replay_identity, error)) {
    return false;
  }
  StoreHash(&result, 344, replay_identity);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kMgaRecoveryObservationDomain,
                       std::span<const byte>(result.data(), 376),
                       &evidence, error, carrier,
                       "observation_evidence_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 376, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateMgaRecoveryObservation(
    std::span<const byte> encoded,
    TypedUpdateMgaRecoveryObservation* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier =
      TypedUpdateCarrierKind::mga_recovery_observation;
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "decoded", 0,
                "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUMO",
                      kTypedUpdateMgaRecoveryObservationBytes,
                      kTypedUpdateMgaRecoveryObservationBytes, carrier,
                      error, kUpdateFailed)) {
    return false;
  }
  if ((encoded[195] != 0 && encoded[195] != 1) ||
      (encoded[196] != 0 && encoded[196] != 1)) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                kUpdateFailed, carrier, "boolean_state", 0,
                "DUMO boolean fields must be encoded as exact zero or one");
  }
  if (!AllZero(encoded.subspan(197, 3)) ||
      !AllZero(encoded.subspan(408, 8))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kUpdateFailed, carrier, "reserved", 0,
                "DUMO reserved bytes must be zero");
  }
  TypedUpdateMgaRecoveryObservation value;
  value.observation_uuid = LoadUuid(encoded, 16);
  value.observation_generation = LoadLittle64(encoded.data() + 32);
  value.validated_mga_durable_handle_uuid = LoadUuid(encoded, 40);
  value.validated_mga_durable_handle_generation =
      LoadLittle64(encoded.data() + 56);
  value.database_uuid = LoadUuid(encoded, 64);
  value.descriptor_uuid = LoadUuid(encoded, 80);
  value.descriptor_generation = LoadLittle64(encoded.data() + 96);
  value.operation_uuid = LoadUuid(encoded, 104);
  value.operation_generation = LoadLittle64(encoded.data() + 120);
  value.authenticated_statement_receipt_uuid = LoadUuid(encoded, 128);
  value.owning_transaction_uuid = LoadUuid(encoded, 144);
  value.owning_local_transaction_id = LoadLittle64(encoded.data() + 160);
  value.recovery_token_uuid = LoadUuid(encoded, 168);
  value.recovery_generation = LoadLittle64(encoded.data() + 184);
  value.latest_journal_state =
      static_cast<TypedUpdateJournalState>(encoded[192]);
  value.transaction_state =
      static_cast<TypedUpdateTransactionState>(encoded[193]);
  value.savepoint_state =
      static_cast<TypedUpdateSavepointState>(encoded[194]);
  value.statement_barrier_present = encoded[195] == 1;
  value.no_surviving_effect_proven = encoded[196] == 1;
  value.statement_savepoint_uuid = LoadUuid(encoded, 200);
  value.statement_savepoint_generation = LoadLittle64(encoded.data() + 216);
  value.reserved_statement_barrier_uuid = LoadUuid(encoded, 224);
  value.reserved_statement_barrier_generation =
      LoadLittle64(encoded.data() + 240);
  value.durable_chain_head_sequence = LoadLittle64(encoded.data() + 248);
  value.durable_chain_head_record_evidence_sha256 = LoadHash(encoded, 256);
  value.catalog_snapshot_uuid = LoadUuid(encoded, 288);
  value.catalog_generation = LoadLittle64(encoded.data() + 304);
  value.security_snapshot_uuid = LoadUuid(encoded, 312);
  value.security_snapshot_generation = LoadLittle64(encoded.data() + 328);
  value.security_epoch = LoadLittle64(encoded.data() + 336);
  value.time_independent_replay_identity_sha256 = LoadHash(encoded, 344);
  value.observation_evidence_sha256 = LoadHash(encoded, 376);
  if (!ValidateMgaRecoveryObservationFields(value, error)) {
    return false;
  }
  TypedUpdateHash replay_identity{};
  if (!ComputeMgaReplayIdentity(value, &replay_identity, error)) {
    return false;
  }
  if (replay_identity != value.time_independent_replay_identity_sha256) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::replay_identity_mismatch,
                kUpdateFailed, carrier,
                "time_independent_replay_identity_sha256", 0,
                "DUMO replay identity does not match its exact 128-byte identity material");
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kMgaRecoveryObservationDomain, encoded.first(376),
                       &evidence, error, carrier,
                       "observation_evidence_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  if (evidence != value.observation_evidence_sha256) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::record_evidence_mismatch,
                kUpdateFailed, carrier,
                "observation_evidence_sha256", 0,
                "DUMO observation evidence does not match bytes [0,376)");
  }
  std::vector<byte> canonical;
  if (!EncodeTypedUpdateMgaRecoveryObservation(value, &canonical, error)) {
    return false;
  }
  if (canonical.size() != encoded.size() ||
      !std::equal(canonical.begin(), canonical.end(), encoded.begin())) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::recovery_observation_invalid,
                kUpdateFailed, carrier, "exact_bytes", 0,
                "DUMO decode/validate/re-encode is not byte-identical");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

constexpr TypedUpdateUuid kDatatypeInt32DescriptorUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x16}};
constexpr TypedUpdateUuid kDatatypeInt32TypeUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x17}};
constexpr TypedUpdateUuid kDatatypeBigintDescriptorUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x11}};
constexpr TypedUpdateUuid kDatatypeBigintTypeUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x12}};
constexpr TypedUpdateUuid kDatatypeDecimalDescriptorUuid{{
    0xa0, 0x00, 0x00, 0x00, 0x64, 0x65, 0x73, 0x69,
    0xad, 0x61, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00}};
constexpr TypedUpdateUuid kDatatypeDecimalTypeUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x13}};
constexpr TypedUpdateUuid kDatatypeInt128DescriptorUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x14}};
constexpr TypedUpdateUuid kDatatypeInt128TypeUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x15}};

struct DatatypeAuthoritySpec {
  TypedUpdateDatatypeIdentityCode code;
  std::string_view canonical_name;
  TypedUpdateUuid descriptor_uuid;
  TypedUpdateUuid type_uuid;
  std::string_view codec_id;
  u32 width;
  TypedUpdateNullEncodingCode null_encoding;
  TypedUpdateByteOrderCode byte_order;
  bool is_signed;
  TypedUpdateRepresentationCode representation;
};

constexpr std::array<DatatypeAuthoritySpec, 5> kDatatypeAuthoritySpecs{{
    {TypedUpdateDatatypeIdentityCode::boolean_v1, "boolean",
     kTypedUpdateBooleanUuid, kTypedUpdateBooleanUuid,
     "datatype.boolean.u8.v1", 1,
     TypedUpdateNullEncodingCode::containing_slot_value_or_null_state,
     TypedUpdateByteOrderCode::single_byte, false,
     TypedUpdateRepresentationCode::canonical_boolean},
    {TypedUpdateDatatypeIdentityCode::int32_v1, "int32",
     kDatatypeInt32DescriptorUuid, kDatatypeInt32TypeUuid,
     "datatype.int32.le.v1", 4,
     TypedUpdateNullEncodingCode::containing_slot_value_or_null_state,
     TypedUpdateByteOrderCode::little_endian, true,
     TypedUpdateRepresentationCode::twos_complement},
    {TypedUpdateDatatypeIdentityCode::bigint_v1, "bigint",
     kDatatypeBigintDescriptorUuid, kDatatypeBigintTypeUuid,
     "datatype.int64.le.v1", 8,
     TypedUpdateNullEncodingCode::unsupported_in_sblr_literal_v1,
     TypedUpdateByteOrderCode::little_endian, true,
     TypedUpdateRepresentationCode::twos_complement},
    {TypedUpdateDatatypeIdentityCode::decimal_v1, "decimal",
     kDatatypeDecimalDescriptorUuid, kDatatypeDecimalTypeUuid,
     "datatype.decimal.base1e9.le.v1", 24,
     TypedUpdateNullEncodingCode::unsupported_in_sblr_literal_v1,
     TypedUpdateByteOrderCode::little_endian, true,
     TypedUpdateRepresentationCode::decimal_base1e9},
    {TypedUpdateDatatypeIdentityCode::int128_v1, "int128",
     kDatatypeInt128DescriptorUuid, kDatatypeInt128TypeUuid,
     "datatype.int128.le.v1", 16,
     TypedUpdateNullEncodingCode::containing_slot_value_or_null_state,
     TypedUpdateByteOrderCode::little_endian, true,
     TypedUpdateRepresentationCode::twos_complement},
}};

const DatatypeAuthoritySpec* DatatypeSpec(
    TypedUpdateDatatypeIdentityCode code) {
  const auto iterator = std::find_if(
      kDatatypeAuthoritySpecs.begin(), kDatatypeAuthoritySpecs.end(),
      [code](const DatatypeAuthoritySpec& row) { return row.code == code; });
  return iterator == kDatatypeAuthoritySpecs.end() ? nullptr : &*iterator;
}

bool ValidFixedAscii(std::string_view text,
                     std::size_t maximum,
                     bool codec) {
  if (text.empty() || text.size() > maximum) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [codec](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') ||
           character == '_' ||
           (codec && (character == '.' || character == '-'));
  });
}

void StoreFixedAscii(std::vector<byte>* encoded,
                     std::size_t offset,
                     std::string_view text) {
  std::transform(text.begin(), text.end(), encoded->begin() + offset,
                 [](char character) { return static_cast<byte>(character); });
}

bool LoadFixedAscii(std::span<const byte> encoded,
                    std::size_t offset,
                    std::size_t width,
                    u8 length,
                    bool codec,
                    TypedUpdateCarrierKind carrier,
                    std::string field,
                    std::string* decoded,
                    TypedUpdateCarrierError* error,
                    u32 record_index) {
  if (length == 0 || length > width ||
      !AllZero(encoded.subspan(offset + length, width - length))) {
    return Fail(error, TypedUpdateCarrierErrorCode::fixed_text_invalid,
                kUpdateFailed, carrier, std::move(field), record_index,
                "fixed ASCII length or zero padding is not canonical");
  }
  std::string value;
  value.reserve(length);
  for (std::size_t index = 0; index < length; ++index) {
    value.push_back(static_cast<char>(encoded[offset + index]));
  }
  if (!ValidFixedAscii(value, width, codec)) {
    return Fail(error, TypedUpdateCarrierErrorCode::fixed_text_invalid,
                kUpdateFailed, carrier, std::move(field), record_index,
                "fixed ASCII contains an unadmitted byte");
  }
  *decoded = std::move(value);
  return true;
}

bool ValidateDatatypeAuthorityRecord(
    const TypedUpdateDatatypeAuthorityRecord& record,
    u32 expected_ordinal,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier = TypedUpdateCarrierKind::datatype_authority_vector;
  if (record.datatype_ordinal != expected_ordinal) {
    return Fail(error, TypedUpdateCarrierErrorCode::ordinal_invalid,
                kUpdateFailed, carrier, "datatype_ordinal", record_index,
                "DUDR ordinals must be dense from one");
  }
  const auto* spec = DatatypeSpec(record.datatype_identity_code);
  if (spec == nullptr) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::datatype_authority_invalid,
                kUpdateFailed, carrier, "datatype_identity_code",
                record_index, "DUDR datatype identity code is not admitted");
  }
  if (!ValidFixedAscii(record.canonical_name, 32, false) ||
      !ValidFixedAscii(record.codec_id, 64, true)) {
    return Fail(error, TypedUpdateCarrierErrorCode::fixed_text_invalid,
                kUpdateFailed, carrier, "canonical_name_or_codec_id",
                record_index, "DUDR fixed ASCII field is not canonical");
  }
  if (record.canonical_name != spec->canonical_name ||
      record.descriptor_uuid != spec->descriptor_uuid ||
      record.descriptor_generation != 1 ||
      record.type_uuid != spec->type_uuid || record.type_generation != 1 ||
      record.codec_id != spec->codec_id || record.codec_version != 1 ||
      record.codec_generation != 1 ||
      record.canonical_value_minimum_bytes != spec->width ||
      record.canonical_value_maximum_bytes != spec->width ||
      record.canonical_value_exact_bytes != spec->width ||
      record.null_encoding_code != spec->null_encoding ||
      record.byte_order_code != spec->byte_order ||
      record.is_signed != spec->is_signed ||
      record.representation_code != spec->representation ||
      record.datatype_snapshot_uuid != kTypedUpdateDatatypeSnapshotUuid ||
      record.datatype_catalog_generation != 1 ||
      record.datatype_registry_generation != 1) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::datatype_authority_invalid,
                kUpdateFailed, carrier, "registry_row", record_index,
                "DUDR does not equal its exact admitted Core datatype row");
  }
  return true;
}

bool DatatypeAuthorityKeyLess(
    const TypedUpdateDatatypeAuthorityRecord& left,
    const TypedUpdateDatatypeAuthorityRecord& right) {
  if (left.descriptor_uuid != right.descriptor_uuid) {
    return std::lexicographical_compare(
        left.descriptor_uuid.begin(), left.descriptor_uuid.end(),
        right.descriptor_uuid.begin(), right.descriptor_uuid.end());
  }
  if (left.descriptor_generation != right.descriptor_generation) {
    return left.descriptor_generation < right.descriptor_generation;
  }
  return std::lexicographical_compare(
      left.type_uuid.begin(), left.type_uuid.end(),
      right.type_uuid.begin(), right.type_uuid.end());
}

bool ValidateDatatypeAuthorityOrdering(
    std::span<const TypedUpdateDatatypeAuthorityRecord> records,
    TypedUpdateCarrierError* error) {
  constexpr auto carrier = TypedUpdateCarrierKind::datatype_authority_vector;
  for (u32 index = 1; index < records.size(); ++index) {
    if (!DatatypeAuthorityKeyLess(records[index - 1], records[index])) {
      return Fail(error,
                  TypedUpdateCarrierErrorCode::datatype_authority_duplicate,
                  kUpdateFailed, carrier, "datatype_order", index,
                  "DUDR rows must be strictly ordered and unique by descriptor/type identity");
    }
  }
  return true;
}

bool EncodeDatatypeAuthorityRecord(
    const TypedUpdateDatatypeAuthorityRecord& record,
    u32 expected_ordinal,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier = TypedUpdateCarrierKind::datatype_authority_vector;
  if (!ValidateDatatypeAuthorityRecord(record, expected_ordinal, error,
                                       record_index)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateDatatypeAuthorityRecordBytes, 0);
  StoreLittle32(result.data(), record.datatype_ordinal);
  result[4] = static_cast<u8>(record.datatype_identity_code);
  result[5] = static_cast<u8>(record.null_encoding_code);
  result[6] = static_cast<u8>(record.byte_order_code);
  result[7] = record.is_signed ? 1 : 0;
  StoreUuid(&result, 8, record.descriptor_uuid);
  StoreLittle64(result.data() + 24, record.descriptor_generation);
  StoreUuid(&result, 32, record.type_uuid);
  StoreLittle64(result.data() + 48, record.type_generation);
  StoreLittle16(result.data() + 56, record.codec_version);
  result[58] = static_cast<u8>(record.canonical_name.size());
  result[59] = static_cast<u8>(record.codec_id.size());
  result[60] = static_cast<u8>(record.representation_code);
  StoreLittle64(result.data() + 64, record.codec_generation);
  StoreLittle32(result.data() + 72,
                record.canonical_value_minimum_bytes);
  StoreLittle32(result.data() + 76,
                record.canonical_value_maximum_bytes);
  StoreLittle32(result.data() + 80,
                record.canonical_value_exact_bytes);
  StoreUuid(&result, 88, record.datatype_snapshot_uuid);
  StoreLittle64(result.data() + 104,
                record.datatype_catalog_generation);
  StoreLittle64(result.data() + 112,
                record.datatype_registry_generation);
  StoreFixedAscii(&result, 120, record.canonical_name);
  StoreFixedAscii(&result, 152, record.codec_id);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kDatatypeAuthorityRecordDomain,
                       std::span<const byte>(result.data(), 216),
                       &evidence, error, carrier,
                       "record_evidence_sha256", record_index,
                       kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 216, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeDatatypeAuthorityRecord(
    std::span<const byte> encoded,
    u32 expected_ordinal,
    TypedUpdateDatatypeAuthorityRecord* decoded,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier = TypedUpdateCarrierKind::datatype_authority_vector;
  if (encoded.size() != kTypedUpdateDatatypeAuthorityRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kUpdateFailed, carrier, "record_bytes", record_index,
                "DUDR record must be exactly 256 bytes");
  }
  if ((encoded[7] != 0 && encoded[7] != 1) ||
      !AllZero(encoded.subspan(61, 3)) ||
      !AllZero(encoded.subspan(84, 4)) ||
      !AllZero(encoded.subspan(248, 8))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kUpdateFailed, carrier, "signed_or_reserved",
                record_index,
                "DUDR signed flag is zero/one and reserved bytes are zero");
  }
  TypedUpdateDatatypeAuthorityRecord value;
  value.datatype_ordinal = LoadLittle32(encoded.data());
  value.datatype_identity_code =
      static_cast<TypedUpdateDatatypeIdentityCode>(encoded[4]);
  value.null_encoding_code =
      static_cast<TypedUpdateNullEncodingCode>(encoded[5]);
  value.byte_order_code =
      static_cast<TypedUpdateByteOrderCode>(encoded[6]);
  value.is_signed = encoded[7] == 1;
  value.descriptor_uuid = LoadUuid(encoded, 8);
  value.descriptor_generation = LoadLittle64(encoded.data() + 24);
  value.type_uuid = LoadUuid(encoded, 32);
  value.type_generation = LoadLittle64(encoded.data() + 48);
  value.codec_version = LoadLittle16(encoded.data() + 56);
  value.representation_code =
      static_cast<TypedUpdateRepresentationCode>(encoded[60]);
  value.codec_generation = LoadLittle64(encoded.data() + 64);
  value.canonical_value_minimum_bytes = LoadLittle32(encoded.data() + 72);
  value.canonical_value_maximum_bytes = LoadLittle32(encoded.data() + 76);
  value.canonical_value_exact_bytes = LoadLittle32(encoded.data() + 80);
  value.datatype_snapshot_uuid = LoadUuid(encoded, 88);
  value.datatype_catalog_generation = LoadLittle64(encoded.data() + 104);
  value.datatype_registry_generation = LoadLittle64(encoded.data() + 112);
  if (!LoadFixedAscii(encoded, 120, 32, encoded[58], false, carrier,
                      "canonical_name", &value.canonical_name, error,
                      record_index) ||
      !LoadFixedAscii(encoded, 152, 64, encoded[59], true, carrier,
                      "codec_id", &value.codec_id, error, record_index)) {
    return false;
  }
  value.record_evidence_sha256 = LoadHash(encoded, 216);
  if (!ValidateDatatypeAuthorityRecord(value, expected_ordinal, error,
                                       record_index)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kDatatypeAuthorityRecordDomain, encoded.first(216),
                       &evidence, error, carrier,
                       "record_evidence_sha256", record_index,
                       kUpdateFailed)) {
    return false;
  }
  if (evidence != value.record_evidence_sha256) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::record_evidence_mismatch,
                kUpdateFailed, carrier, "record_evidence_sha256",
                record_index,
                "DUDR evidence does not match bytes [0,216)");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

bool ValidateBuiltinOperatorAuthorityRecord(
    const TypedUpdateBuiltinOperatorAuthorityRecord& record,
    u32 expected_ordinal,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::builtin_operator_authority_vector;
  if (record.operator_ordinal != expected_ordinal) {
    return Fail(error, TypedUpdateCarrierErrorCode::ordinal_invalid,
                kUpdateFailed, carrier, "operator_ordinal", record_index,
                "DUOE ordinal must be dense from one");
  }
  if (!ValidFixedAscii(record.result_codec_id, 32, true)) {
    return Fail(error, TypedUpdateCarrierErrorCode::fixed_text_invalid,
                kUpdateFailed, carrier, "result_codec_id", record_index,
                "DUOE result codec fixed ASCII is invalid");
  }
  if (record.semantic_code != 1 || record.operand_arity != 2 ||
      record.null_behavior_code != 1 || record.accepted_state != 1 ||
      record.operator_uuid != kTypedUpdateEqualOperatorUuid ||
      record.operator_generation != 1 ||
      record.operator_snapshot_uuid != kTypedUpdateOperatorSnapshotUuid ||
      record.operator_registry_generation != 1 ||
      record.left_descriptor_uuid != record.right_descriptor_uuid ||
      record.left_descriptor_generation !=
          record.right_descriptor_generation ||
      record.left_type_uuid != record.right_type_uuid ||
      record.left_type_generation != record.right_type_generation ||
      !UuidPresent(record.left_descriptor_uuid) ||
      record.left_descriptor_generation == 0 ||
      !UuidPresent(record.left_type_uuid) || record.left_type_generation == 0 ||
      record.result_descriptor_uuid != kTypedUpdateBooleanUuid ||
      record.result_descriptor_generation != 1 ||
      record.result_type_uuid != kTypedUpdateBooleanUuid ||
      record.result_type_generation != 1 ||
      record.result_codec_version != 1 ||
      record.operator_family_code != 1 ||
      record.result_codec_generation != 1 ||
      record.result_codec_id != kBooleanCodec ||
      record.operand_identity_rule != 1 ||
      record.result_null_encoding_code != 1) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::builtin_operator_authority_invalid,
                kUpdateFailed, carrier, "equality_authority", record_index,
                "DUOE does not equal the admitted builtin equality semantic, operand, and result identity");
  }
  return true;
}

bool EncodeBuiltinOperatorAuthorityRecord(
    const TypedUpdateBuiltinOperatorAuthorityRecord& record,
    u32 expected_ordinal,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::builtin_operator_authority_vector;
  if (!ValidateBuiltinOperatorAuthorityRecord(
          record, expected_ordinal, error, record_index)) {
    return false;
  }
  std::vector<byte> result(
      kTypedUpdateBuiltinOperatorAuthorityRecordBytes, 0);
  StoreLittle32(result.data(), record.operator_ordinal);
  result[4] = record.semantic_code;
  result[5] = record.operand_arity;
  result[6] = record.null_behavior_code;
  result[7] = record.accepted_state;
  StoreUuid(&result, 8, record.operator_uuid);
  StoreLittle64(result.data() + 24, record.operator_generation);
  StoreUuid(&result, 32, record.operator_snapshot_uuid);
  StoreLittle64(result.data() + 48,
                record.operator_registry_generation);
  StoreUuid(&result, 56, record.left_descriptor_uuid);
  StoreLittle64(result.data() + 72,
                record.left_descriptor_generation);
  StoreUuid(&result, 80, record.left_type_uuid);
  StoreLittle64(result.data() + 96, record.left_type_generation);
  StoreUuid(&result, 104, record.right_descriptor_uuid);
  StoreLittle64(result.data() + 120,
                record.right_descriptor_generation);
  StoreUuid(&result, 128, record.right_type_uuid);
  StoreLittle64(result.data() + 144, record.right_type_generation);
  StoreUuid(&result, 152, record.result_descriptor_uuid);
  StoreLittle64(result.data() + 168,
                record.result_descriptor_generation);
  StoreUuid(&result, 176, record.result_type_uuid);
  StoreLittle64(result.data() + 192, record.result_type_generation);
  StoreLittle16(result.data() + 200, record.result_codec_version);
  result[202] = static_cast<u8>(record.result_codec_id.size());
  result[203] = record.operator_family_code;
  StoreLittle64(result.data() + 204, record.result_codec_generation);
  StoreFixedAscii(&result, 212, record.result_codec_id);
  result[244] = record.operand_identity_rule;
  result[245] = record.result_null_encoding_code;
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kBuiltinOperatorAuthorityRecordDomain,
                       std::span<const byte>(result.data(), 248),
                       &evidence, error, carrier,
                       "record_evidence_sha256", record_index,
                       kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 248, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeBuiltinOperatorAuthorityRecord(
    std::span<const byte> encoded,
    u32 expected_ordinal,
    TypedUpdateBuiltinOperatorAuthorityRecord* decoded,
    TypedUpdateCarrierError* error,
    u32 record_index) {
  constexpr auto carrier =
      TypedUpdateCarrierKind::builtin_operator_authority_vector;
  if (encoded.size() != kTypedUpdateBuiltinOperatorAuthorityRecordBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kUpdateFailed, carrier, "record_bytes", record_index,
                "DUOE record must be exactly 288 bytes");
  }
  if (!AllZero(encoded.subspan(246, 2)) ||
      !AllZero(encoded.subspan(280, 8))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kUpdateFailed, carrier, "reserved", record_index,
                "DUOE reserved bytes must be zero");
  }
  TypedUpdateBuiltinOperatorAuthorityRecord value;
  value.operator_ordinal = LoadLittle32(encoded.data());
  value.semantic_code = encoded[4];
  value.operand_arity = encoded[5];
  value.null_behavior_code = encoded[6];
  value.accepted_state = encoded[7];
  value.operator_uuid = LoadUuid(encoded, 8);
  value.operator_generation = LoadLittle64(encoded.data() + 24);
  value.operator_snapshot_uuid = LoadUuid(encoded, 32);
  value.operator_registry_generation = LoadLittle64(encoded.data() + 48);
  value.left_descriptor_uuid = LoadUuid(encoded, 56);
  value.left_descriptor_generation = LoadLittle64(encoded.data() + 72);
  value.left_type_uuid = LoadUuid(encoded, 80);
  value.left_type_generation = LoadLittle64(encoded.data() + 96);
  value.right_descriptor_uuid = LoadUuid(encoded, 104);
  value.right_descriptor_generation = LoadLittle64(encoded.data() + 120);
  value.right_type_uuid = LoadUuid(encoded, 128);
  value.right_type_generation = LoadLittle64(encoded.data() + 144);
  value.result_descriptor_uuid = LoadUuid(encoded, 152);
  value.result_descriptor_generation = LoadLittle64(encoded.data() + 168);
  value.result_type_uuid = LoadUuid(encoded, 176);
  value.result_type_generation = LoadLittle64(encoded.data() + 192);
  value.result_codec_version = LoadLittle16(encoded.data() + 200);
  value.operator_family_code = encoded[203];
  value.result_codec_generation = LoadLittle64(encoded.data() + 204);
  if (!LoadFixedAscii(encoded, 212, 32, encoded[202], true, carrier,
                      "result_codec_id", &value.result_codec_id, error,
                      record_index)) {
    return false;
  }
  value.operand_identity_rule = encoded[244];
  value.result_null_encoding_code = encoded[245];
  value.record_evidence_sha256 = LoadHash(encoded, 248);
  if (!ValidateBuiltinOperatorAuthorityRecord(
          value, expected_ordinal, error, record_index)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kBuiltinOperatorAuthorityRecordDomain,
                       encoded.first(248), &evidence, error, carrier,
                       "record_evidence_sha256", record_index,
                       kUpdateFailed)) {
    return false;
  }
  if (evidence != value.record_evidence_sha256) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::record_evidence_mismatch,
                kUpdateFailed, carrier, "record_evidence_sha256",
                record_index,
                "DUOE evidence does not match bytes [0,248)");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

}  // namespace

bool EncodeTypedUpdateDatatypeAuthorityVector(
    const TypedUpdateDatatypeAuthorityVector& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier = TypedUpdateCarrierKind::datatype_authority_vector;
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "encoded", 0,
                "output pointer is null");
  }
  if (value.records.empty() ||
      value.records.size() > kDatatypeAuthoritySpecs.size()) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kUpdateFailed, carrier, "record_count", 0,
                "DUDV requires one through five exact datatype rows");
  }
  if (value.identity.vector_uuid != kTypedUpdateDatatypeSnapshotUuid ||
      value.identity.vector_generation != 1 ||
      !RequireSecurityUuidGeneration(
          value.identity.owner_descriptor_uuid,
          value.identity.owner_descriptor_generation, carrier,
          "owner_descriptor_identity", error)) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::datatype_authority_invalid,
           kUpdateFailed, carrier, "vector_identity", 0,
           "DUDV vector identity must equal the exact datatype snapshot generation");
    }
    return false;
  }
  std::vector<byte> records;
  records.reserve(value.records.size() *
                  kTypedUpdateDatatypeAuthorityRecordBytes);
  for (u32 index = 0; index < value.records.size(); ++index) {
    std::vector<byte> record;
    if (!EncodeDatatypeAuthorityRecord(value.records[index], index + 1,
                                       &record, error, index)) {
      return false;
    }
    records.insert(records.end(), record.begin(), record.end());
  }
  if (!ValidateDatatypeAuthorityOrdering(value.records, error)) {
    return false;
  }
  const u32 total_bytes = static_cast<u32>(
      kTypedUpdateVectorHeaderBytes + records.size());
  std::vector<byte> result(total_bytes, 0);
  StoreHeader(&result, "DUDV", kTypedUpdateVectorHeaderBytes, total_bytes);
  StoreUuid(&result, 16, value.identity.vector_uuid);
  StoreLittle64(result.data() + 32, value.identity.vector_generation);
  StoreUuid(&result, 40, value.identity.owner_descriptor_uuid);
  StoreLittle64(result.data() + 56,
                value.identity.owner_descriptor_generation);
  StoreLittle32(result.data() + 64,
                static_cast<u32>(value.records.size()));
  StoreLittle32(result.data() + 68,
                static_cast<u32>(records.size()));
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kDatatypeAuthorityVectorDomain, records, &evidence,
                       error, carrier, "vector_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 72, evidence);
  std::copy(records.begin(), records.end(),
            result.begin() + kTypedUpdateVectorHeaderBytes);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
    std::span<const byte> encoded,
    TypedUpdateDatatypeAuthorityVector* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier = TypedUpdateCarrierKind::datatype_authority_vector;
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "decoded", 0,
                "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUDV", kTypedUpdateVectorHeaderBytes,
                      std::nullopt, carrier, error, kUpdateFailed)) {
    return false;
  }
  const u32 count = LoadLittle32(encoded.data() + 64);
  const u32 record_bytes = LoadLittle32(encoded.data() + 68);
  if (count == 0 || count > kDatatypeAuthoritySpecs.size() ||
      record_bytes != count * kTypedUpdateDatatypeAuthorityRecordBytes ||
      record_bytes != encoded.size() - kTypedUpdateVectorHeaderBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kUpdateFailed, carrier, "record_count_or_bytes", 0,
                "DUDV count and fixed DUDR extent disagree");
  }
  TypedUpdateDatatypeAuthorityVector value;
  value.identity.vector_uuid = LoadUuid(encoded, 16);
  value.identity.vector_generation = LoadLittle64(encoded.data() + 32);
  value.identity.owner_descriptor_uuid = LoadUuid(encoded, 40);
  value.identity.owner_descriptor_generation =
      LoadLittle64(encoded.data() + 56);
  value.identity.vector_sha256 = LoadHash(encoded, 72);
  if (value.identity.vector_uuid != kTypedUpdateDatatypeSnapshotUuid ||
      value.identity.vector_generation != 1 ||
      !RequireSecurityUuidGeneration(
          value.identity.owner_descriptor_uuid,
          value.identity.owner_descriptor_generation, carrier,
          "owner_descriptor_identity", error)) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::datatype_authority_invalid,
           kUpdateFailed, carrier, "vector_identity", 0,
           "DUDV vector identity is not the exact datatype snapshot");
    }
    return false;
  }
  const auto records = encoded.subspan(kTypedUpdateVectorHeaderBytes);
  value.records.reserve(count);
  for (u32 index = 0; index < count; ++index) {
    TypedUpdateDatatypeAuthorityRecord record;
    if (!DecodeDatatypeAuthorityRecord(
            records.subspan(index * kTypedUpdateDatatypeAuthorityRecordBytes,
                            kTypedUpdateDatatypeAuthorityRecordBytes),
            index + 1, &record, error, index)) {
      return false;
    }
    value.records.push_back(std::move(record));
  }
  if (!ValidateDatatypeAuthorityOrdering(value.records, error)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kDatatypeAuthorityVectorDomain, records, &evidence,
                       error, carrier, "vector_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  if (evidence != value.identity.vector_sha256) {
    return Fail(error, TypedUpdateCarrierErrorCode::vector_evidence_mismatch,
                kUpdateFailed, carrier, "vector_sha256", 0,
                "DUDV evidence does not match exact DUDR records");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

bool EncodeTypedUpdateBuiltinOperatorAuthorityVector(
    const TypedUpdateBuiltinOperatorAuthorityVector& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier =
      TypedUpdateCarrierKind::builtin_operator_authority_vector;
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "encoded", 0,
                "output pointer is null");
  }
  if (value.records.size() > 1) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kUpdateFailed, carrier, "record_count", 0,
                "DUOV admits zero or one equality authority row");
  }
  if (value.identity.vector_uuid != kTypedUpdateOperatorSnapshotUuid ||
      value.identity.vector_generation != 1 ||
      !RequireSecurityUuidGeneration(
          value.identity.owner_descriptor_uuid,
          value.identity.owner_descriptor_generation, carrier,
          "owner_descriptor_identity", error)) {
    if (error != nullptr && error->ok()) {
      Fail(error,
           TypedUpdateCarrierErrorCode::builtin_operator_authority_invalid,
           kUpdateFailed, carrier, "vector_identity", 0,
           "DUOV vector identity must equal the exact builtin snapshot generation");
    }
    return false;
  }
  std::vector<byte> records;
  if (!value.records.empty()) {
    if (!EncodeBuiltinOperatorAuthorityRecord(value.records.front(), 1,
                                              &records, error, 0)) {
      return false;
    }
  }
  const u32 total_bytes = static_cast<u32>(
      kTypedUpdateVectorHeaderBytes + records.size());
  std::vector<byte> result(total_bytes, 0);
  StoreHeader(&result, "DUOV", kTypedUpdateVectorHeaderBytes, total_bytes);
  StoreUuid(&result, 16, value.identity.vector_uuid);
  StoreLittle64(result.data() + 32, value.identity.vector_generation);
  StoreUuid(&result, 40, value.identity.owner_descriptor_uuid);
  StoreLittle64(result.data() + 56,
                value.identity.owner_descriptor_generation);
  StoreLittle32(result.data() + 64,
                static_cast<u32>(value.records.size()));
  StoreLittle32(result.data() + 68,
                static_cast<u32>(records.size()));
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kBuiltinOperatorAuthorityVectorDomain, records,
                       &evidence, error, carrier, "vector_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  StoreHash(&result, 72, evidence);
  std::copy(records.begin(), records.end(),
            result.begin() + kTypedUpdateVectorHeaderBytes);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
    std::span<const byte> encoded,
    TypedUpdateBuiltinOperatorAuthorityVector* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier =
      TypedUpdateCarrierKind::builtin_operator_authority_vector;
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "decoded", 0,
                "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUOV", kTypedUpdateVectorHeaderBytes,
                      std::nullopt, carrier, error, kUpdateFailed)) {
    return false;
  }
  const u32 count = LoadLittle32(encoded.data() + 64);
  const u32 record_bytes = LoadLittle32(encoded.data() + 68);
  if (count > 1 ||
      record_bytes != count *
                          kTypedUpdateBuiltinOperatorAuthorityRecordBytes ||
      record_bytes != encoded.size() - kTypedUpdateVectorHeaderBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::count_invalid,
                kUpdateFailed, carrier, "record_count_or_bytes", 0,
                "DUOV count and fixed DUOE extent disagree");
  }
  TypedUpdateBuiltinOperatorAuthorityVector value;
  value.identity.vector_uuid = LoadUuid(encoded, 16);
  value.identity.vector_generation = LoadLittle64(encoded.data() + 32);
  value.identity.owner_descriptor_uuid = LoadUuid(encoded, 40);
  value.identity.owner_descriptor_generation =
      LoadLittle64(encoded.data() + 56);
  value.identity.vector_sha256 = LoadHash(encoded, 72);
  if (value.identity.vector_uuid != kTypedUpdateOperatorSnapshotUuid ||
      value.identity.vector_generation != 1 ||
      !RequireSecurityUuidGeneration(
          value.identity.owner_descriptor_uuid,
          value.identity.owner_descriptor_generation, carrier,
          "owner_descriptor_identity", error)) {
    if (error != nullptr && error->ok()) {
      Fail(error,
           TypedUpdateCarrierErrorCode::builtin_operator_authority_invalid,
           kUpdateFailed, carrier, "vector_identity", 0,
           "DUOV vector identity is not the exact builtin snapshot");
    }
    return false;
  }
  const auto records = encoded.subspan(kTypedUpdateVectorHeaderBytes);
  if (count == 1) {
    TypedUpdateBuiltinOperatorAuthorityRecord record;
    if (!DecodeBuiltinOperatorAuthorityRecord(records, 1, &record, error,
                                              0)) {
      return false;
    }
    value.records.push_back(std::move(record));
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kBuiltinOperatorAuthorityVectorDomain, records,
                       &evidence, error, carrier, "vector_sha256", 0,
                       kUpdateFailed)) {
    return false;
  }
  if (evidence != value.identity.vector_sha256) {
    return Fail(error, TypedUpdateCarrierErrorCode::vector_evidence_mismatch,
                kUpdateFailed, carrier, "vector_sha256", 0,
                "DUOV evidence does not match exact DUOE records");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

namespace {

bool ValidateResultFields(const TypedUpdateResultCarrier& value,
                          TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier = TypedUpdateCarrierKind::result;
  if (!RequireUuidGeneration(value.update_descriptor_uuid,
                             value.update_descriptor_generation, carrier,
                             "update_descriptor_identity", error) ||
      !RequireUuidGeneration(value.relation_uuid,
                             value.relation_generation, carrier,
                             "relation_identity", error) ||
      !RequireUuidGeneration(value.publication_barrier_uuid,
                             value.publication_barrier_generation, carrier,
                             "publication_barrier_identity", error)) {
    return false;
  }
  if (!UuidPresent(value.operation_uuid) ||
      !UuidPresent(value.owning_transaction_uuid) ||
      value.owning_local_transaction_id == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::result_invalid,
                kUpdateFailed, carrier, "result_identity", 0,
                "DURS operation/transaction identity is invalid");
  }
  if (value.updated_count > value.matched_count) {
    return Fail(error, TypedUpdateCarrierErrorCode::result_invalid,
                kUpdateFailed, carrier, "updated_count", 0,
                "updated count exceeds matched count");
  }
  if (!HashPresent(value.effect_set_sha256) ||
      !HashPresent(value.executor_evidence_sha256)) {
    return Fail(error, TypedUpdateCarrierErrorCode::result_invalid,
                kUpdateFailed, carrier, "inner_evidence", 0,
                "DURS inner evidence hashes must be nonzero");
  }
  return true;
}

bool LegalJournalTransition(TypedUpdateJournalState prior,
                            TypedUpdateJournalState next) {
  switch (prior) {
    case TypedUpdateJournalState::bound:
      return next == TypedUpdateJournalState::intent ||
             next == TypedUpdateJournalState::aborted;
    case TypedUpdateJournalState::intent:
      return next == TypedUpdateJournalState::prepared ||
             next == TypedUpdateJournalState::aborted;
    case TypedUpdateJournalState::prepared:
      return next == TypedUpdateJournalState::published ||
             next == TypedUpdateJournalState::aborted;
    case TypedUpdateJournalState::published:
    case TypedUpdateJournalState::aborted:
      return false;
  }
  return false;
}

bool ValidJournalState(TypedUpdateJournalState state) {
  return state >= TypedUpdateJournalState::bound &&
         state <= TypedUpdateJournalState::aborted;
}

bool ValidateJournalPreEvidence(const TypedUpdateJournalRecord& value,
                                const TypedUpdateJournalChainContext& chain,
                                bool result_present,
                                TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier = TypedUpdateCarrierKind::journal;
  if (!ValidJournalState(value.lifecycle_state)) {
    return Fail(error, TypedUpdateCarrierErrorCode::journal_state_invalid,
                kUpdateFailed, carrier, "lifecycle_state", 0,
                "DUJR lifecycle state is outside 1 through 5");
  }
  if (!UuidPresent(value.database_uuid) ||
      !UuidPresent(value.authenticated_statement_receipt_uuid) ||
      !UuidPresent(value.owning_transaction_uuid) ||
      !UuidPresent(value.operation_uuid) ||
      !UuidPresent(value.recovery_token_uuid) ||
      value.owning_local_transaction_id == 0 ||
      value.recovery_generation == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::recovery_identity_invalid,
                kUpdateFailed, carrier, "journal_identity", 0,
                "DUJR owner identities and generations are nonzero");
  }
  const bool savepoint_present = UuidPresent(value.statement_savepoint_uuid);
  if (savepoint_present != (value.statement_savepoint_generation != 0)) {
    return Fail(error, TypedUpdateCarrierErrorCode::generation_invalid,
                kUpdateFailed, carrier, "statement_savepoint_identity", 0,
                "savepoint UUID and generation must be nil/zero together");
  }
  if (chain.first_record) {
    if (value.lifecycle_state != TypedUpdateJournalState::bound ||
        value.journal_sequence != 1 ||
        !AllZero(value.prior_record_sha256)) {
      return Fail(error, TypedUpdateCarrierErrorCode::journal_sequence_invalid,
                  kUpdateFailed, carrier, "first_record", 0,
                  "first DUJR must be bound sequence one with zero predecessor");
    }
  } else {
    if (chain.prior_sequence == std::numeric_limits<u64>::max() ||
        value.journal_sequence != chain.prior_sequence + 1) {
      return Fail(error, TypedUpdateCarrierErrorCode::journal_sequence_invalid,
                  kUpdateFailed, carrier, "journal_sequence", 0,
                  "DUJR sequence must increment by exactly one");
    }
    if (value.prior_record_sha256 !=
        chain.prior_record_evidence_sha256) {
      return Fail(error, TypedUpdateCarrierErrorCode::journal_chain_mismatch,
                  kUpdateFailed, carrier, "prior_record_sha256", 0,
                  "DUJR predecessor hash does not copy prior evidence");
    }
    if (!LegalJournalTransition(chain.prior_state,
                                value.lifecycle_state)) {
      return Fail(error,
                  TypedUpdateCarrierErrorCode::journal_transition_invalid,
                  kUpdateFailed, carrier, "lifecycle_transition", 0,
                  "DUJR lifecycle transition is not admitted");
    }
  }
  if (value.lifecycle_state == TypedUpdateJournalState::bound &&
      savepoint_present) {
    return Fail(error, TypedUpdateCarrierErrorCode::journal_state_invalid,
                kUpdateFailed, carrier, "statement_savepoint_identity", 0,
                "bound state has nil savepoint identity");
  }
  if ((value.lifecycle_state == TypedUpdateJournalState::intent ||
       value.lifecycle_state == TypedUpdateJournalState::prepared ||
       value.lifecycle_state == TypedUpdateJournalState::published) &&
      !savepoint_present) {
    return Fail(error, TypedUpdateCarrierErrorCode::journal_state_invalid,
                kUpdateFailed, carrier, "statement_savepoint_identity", 0,
                "intent/prepared/published state requires a savepoint identity");
  }
  if (!chain.first_record &&
      value.lifecycle_state == TypedUpdateJournalState::aborted &&
      chain.prior_state == TypedUpdateJournalState::bound) {
    const bool provider_savepoint_present =
        UuidPresent(chain.prior_savepoint_uuid);
    if (provider_savepoint_present !=
        (chain.prior_savepoint_generation != 0)) {
      return Fail(
          error, TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          kUpdateFailed, carrier, "prior_savepoint_authority", 0,
          "bound-to-aborted provider savepoint UUID and generation must be nil/zero or nonzero together");
    }
    if (savepoint_present != provider_savepoint_present ||
        (savepoint_present &&
         (value.statement_savepoint_uuid != chain.prior_savepoint_uuid ||
          value.statement_savepoint_generation !=
              chain.prior_savepoint_generation))) {
      return Fail(
          error, TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          kUpdateFailed, carrier, "statement_savepoint_identity", 0,
          "bound-to-aborted must carry nil/zero when no savepoint opened or the exact MGA provider savepoint after rollback");
    }
  }
  if (!chain.first_record &&
      (chain.prior_state == TypedUpdateJournalState::intent ||
       chain.prior_state == TypedUpdateJournalState::prepared) &&
      (value.statement_savepoint_uuid != chain.prior_savepoint_uuid ||
       value.statement_savepoint_generation !=
           chain.prior_savepoint_generation)) {
    return Fail(error, TypedUpdateCarrierErrorCode::journal_chain_mismatch,
                kUpdateFailed, carrier, "statement_savepoint_identity", 0,
                "successor does not preserve the exact savepoint identity");
  }
  const bool result_required =
      value.lifecycle_state == TypedUpdateJournalState::prepared ||
      value.lifecycle_state == TypedUpdateJournalState::published;
  if (result_present != result_required) {
    return Fail(error, TypedUpdateCarrierErrorCode::journal_state_invalid,
                kUpdateFailed, carrier, "prior_DURS_bytes", 0,
                "DURS presence does not match the journal lifecycle state");
  }
  return true;
}

bool ValidateJournalChain(const TypedUpdateJournalRecord& value,
                          const TypedUpdateJournalChainContext& chain,
                          std::span<const byte> descriptor_bytes,
                          TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier = TypedUpdateCarrierKind::journal;
  if (!ValidateJournalPreEvidence(value, chain,
                                  value.prior_result.has_value(), error)) {
    return false;
  }
  if (chain.require_same_descriptor &&
      !std::equal(descriptor_bytes.begin(), descriptor_bytes.end(),
                  chain.expected_descriptor_bytes.begin())) {
    return Fail(error, TypedUpdateCarrierErrorCode::journal_chain_mismatch,
                kUpdateFailed, carrier, "embedded_DUDC", 0,
                "DUJR successor changed its exact embedded DUDC bytes");
  }
  if (!chain.first_record && !chain.require_same_descriptor) {
    return Fail(error, TypedUpdateCarrierErrorCode::journal_chain_mismatch,
                kUpdateFailed, carrier, "embedded_DUDC", 0,
                "DUJR successor validation requires the prior exact DUDC bytes");
  }
  if (!chain.first_record &&
      chain.prior_state == TypedUpdateJournalState::prepared &&
      value.lifecycle_state == TypedUpdateJournalState::published) {
    if (!chain.expected_prepared_result_bytes.has_value() ||
        !value.prior_result.has_value()) {
      return Fail(error, TypedUpdateCarrierErrorCode::journal_chain_mismatch,
                  kUpdateFailed, carrier, "embedded_DURS", 0,
                  "prepared-to-published validation requires prior exact DURS bytes");
    }
    std::vector<byte> result_bytes;
    if (!EncodeTypedUpdateResult(value.prior_result.value(), &result_bytes,
                                 error)) {
      if (error != nullptr) {
        error->carrier = carrier;
        error->field = "embedded_DURS." + error->field;
      }
      return false;
    }
    if (!std::equal(result_bytes.begin(), result_bytes.end(),
                    chain.expected_prepared_result_bytes->begin())) {
      return Fail(error, TypedUpdateCarrierErrorCode::journal_chain_mismatch,
                  kUpdateFailed, carrier, "embedded_DURS", 0,
                  "published DUJR changed the prepared byte-identical DURS");
    }
  }
  return true;
}

bool ValidateJournalOwnership(const TypedUpdateJournalRecord& value,
                              TypedUpdateCarrierError* error) {
  constexpr TypedUpdateCarrierKind carrier = TypedUpdateCarrierKind::journal;
  const auto& descriptor = value.descriptor;
  if (!UuidPresent(value.database_uuid) ||
      !UuidPresent(value.authenticated_statement_receipt_uuid) ||
      !UuidPresent(value.owning_transaction_uuid) ||
      !UuidPresent(value.operation_uuid) ||
      !UuidPresent(value.recovery_token_uuid) ||
      value.owning_local_transaction_id == 0 ||
      value.recovery_generation == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::recovery_identity_invalid,
                kUpdateFailed, carrier, "journal_identity", 0,
                "DUJR owner identities and generations are nonzero");
  }
  if (value.authenticated_statement_receipt_uuid !=
          descriptor.authenticated_statement_receipt_uuid ||
      value.owning_transaction_uuid != descriptor.owning_transaction_uuid ||
      value.owning_local_transaction_id !=
          descriptor.owning_local_transaction_id ||
      value.operation_uuid != descriptor.operation_uuid ||
      value.recovery_token_uuid != descriptor.recovery_token_uuid ||
      value.recovery_generation != descriptor.recovery_generation) {
    return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                kUpdateFailed, carrier, "embedded_DUDC_owner", 0,
                "DUJR owner fields do not equal embedded DUDC");
  }
  if (value.prior_result.has_value()) {
    const auto& result = value.prior_result.value();
    if (result.update_descriptor_uuid != descriptor.descriptor_uuid ||
        result.update_descriptor_generation !=
            descriptor.descriptor_generation ||
        result.operation_uuid != descriptor.operation_uuid ||
        result.owning_transaction_uuid != descriptor.owning_transaction_uuid ||
        result.owning_local_transaction_id !=
            descriptor.owning_local_transaction_id ||
        result.relation_uuid != descriptor.target_relation_uuid ||
        result.relation_generation != descriptor.target_relation_generation) {
      return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                  kUpdateFailed, carrier, "embedded_DURS_owner", 0,
                  "embedded DURS owner fields do not equal DUDC");
    }
  }
  return true;
}

bool ComputeJournalEvidence(std::span<const byte> encoded,
                            TypedUpdateHash* result,
                            TypedUpdateCarrierError* error) {
  std::vector<byte> material;
  material.reserve(224 + encoded.size() - kTypedUpdateJournalHeaderBytes);
  material.insert(material.end(), encoded.begin(), encoded.begin() + 224);
  material.insert(material.end(),
                  encoded.begin() + kTypedUpdateJournalHeaderBytes,
                  encoded.end());
  return ComputeEvidence(kJournalDomain, material, result, error,
                         TypedUpdateCarrierKind::journal,
                         "record_evidence_sha256");
}

}  // namespace

bool EncodeTypedUpdateResult(const TypedUpdateResultCarrier& value,
                             std::vector<byte>* encoded,
                             TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, TypedUpdateCarrierKind::result,
                "encoded", 0, "output pointer is null");
  }
  if (!ValidateResultFields(value, error)) {
    return false;
  }
  std::vector<byte> result(kTypedUpdateResultBytes, 0);
  StoreHeader(&result, "DURS", kTypedUpdateResultBytes,
              kTypedUpdateResultBytes);
  StoreUuid(&result, 16, value.update_descriptor_uuid);
  StoreLittle64(result.data() + 32, value.update_descriptor_generation);
  StoreUuid(&result, 40, value.operation_uuid);
  StoreUuid(&result, 56, value.owning_transaction_uuid);
  StoreLittle64(result.data() + 72, value.owning_local_transaction_id);
  StoreUuid(&result, 80, value.relation_uuid);
  StoreLittle64(result.data() + 96, value.relation_generation);
  StoreLittle64(result.data() + 104, value.matched_count);
  StoreLittle64(result.data() + 112, value.updated_count);
  result[120] = 1;
  StoreHash(&result, 128, value.effect_set_sha256);
  StoreHash(&result, 160, value.executor_evidence_sha256);
  StoreUuid(&result, 192, value.publication_barrier_uuid);
  StoreLittle64(result.data() + 208,
                value.publication_barrier_generation);
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kResultDomain,
                       std::span<const byte>(result.data(), 216), &evidence,
                       error, TypedUpdateCarrierKind::result,
                       "result_evidence_sha256")) {
    return false;
  }
  StoreHash(&result, 216, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateResult(
    std::span<const byte> encoded,
    TypedUpdateResultCarrier* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, TypedUpdateCarrierKind::result,
                "decoded", 0, "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DURS", kTypedUpdateResultBytes,
                      kTypedUpdateResultBytes,
                      TypedUpdateCarrierKind::result, error,
                      kUpdateFailed)) {
    return false;
  }
  if (encoded[120] != 1 || !AllZero(encoded.subspan(121, 7)) ||
      !AllZero(encoded.subspan(248, 8))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kUpdateFailed, TypedUpdateCarrierKind::result,
                "completion_or_reserved", 0,
                "DURS completion must be one and reserved bytes zero");
  }
  TypedUpdateResultCarrier value;
  value.update_descriptor_uuid = LoadUuid(encoded, 16);
  value.update_descriptor_generation = LoadLittle64(encoded.data() + 32);
  value.operation_uuid = LoadUuid(encoded, 40);
  value.owning_transaction_uuid = LoadUuid(encoded, 56);
  value.owning_local_transaction_id = LoadLittle64(encoded.data() + 72);
  value.relation_uuid = LoadUuid(encoded, 80);
  value.relation_generation = LoadLittle64(encoded.data() + 96);
  value.matched_count = LoadLittle64(encoded.data() + 104);
  value.updated_count = LoadLittle64(encoded.data() + 112);
  value.effect_set_sha256 = LoadHash(encoded, 128);
  value.executor_evidence_sha256 = LoadHash(encoded, 160);
  value.publication_barrier_uuid = LoadUuid(encoded, 192);
  value.publication_barrier_generation = LoadLittle64(encoded.data() + 208);
  value.result_evidence_sha256 = LoadHash(encoded, 216);
  if (!ValidateResultFields(value, error)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeEvidence(kResultDomain, encoded.first(216), &evidence,
                       error, TypedUpdateCarrierKind::result,
                       "result_evidence_sha256") ||
      evidence != value.result_evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::result_evidence_mismatch,
           kUpdateFailed, TypedUpdateCarrierKind::result,
           "result_evidence_sha256", 0, "DURS result evidence mismatch");
    }
    return false;
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

bool EncodeTypedUpdateResultEvidenceMaterial(
    const TypedUpdateResultCarrier& value,
    std::span<const TypedUpdateResultEvidenceReference> evidence,
    std::vector<byte>* material,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr TypedUpdateCarrierKind carrier = TypedUpdateCarrierKind::result;
  if (material == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "material", 0,
                "evidence material output pointer is null");
  }
  if (!UuidPresent(value.update_descriptor_uuid) ||
      !UuidPresent(value.operation_uuid) ||
      !UuidPresent(value.owning_transaction_uuid) ||
      !UuidPresent(value.relation_uuid)) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::result_evidence_material_invalid,
                kUpdateFailed, carrier, "fixed_identity", 0,
                "result evidence fixed UUID identities must be nonzero");
  }
  std::vector<std::vector<byte>> encoded_records;
  encoded_records.reserve(evidence.size());
  std::size_t total_bytes = 64;
  for (u32 index = 0; index < evidence.size(); ++index) {
    const auto& row = evidence[index];
    if (!ValidUtf8EvidenceField(row.evidence_kind, true) ||
        !ValidUtf8EvidenceField(row.evidence_id, false)) {
      return Fail(
          error,
          TypedUpdateCarrierErrorCode::result_evidence_material_invalid,
          kUpdateFailed, carrier, "evidence_kind_or_id", index,
          "evidence kind/id must be nonempty canonical UTF-8; kind also forbids equals");
    }
    const auto maximum_size = std::numeric_limits<std::size_t>::max();
    if (row.evidence_id.size() > maximum_size - 2 ||
        row.evidence_kind.size() > maximum_size - row.evidence_id.size() - 2) {
      return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                  kResourceExceeded, carrier, "evidence_record_bytes", index,
                  "evidence record extent overflows size_t");
    }
    std::vector<byte> encoded_record;
    encoded_record.reserve(row.evidence_kind.size() + row.evidence_id.size() +
                           1);
    encoded_record.insert(encoded_record.end(), row.evidence_kind.begin(),
                          row.evidence_kind.end());
    encoded_record.push_back(static_cast<byte>('='));
    encoded_record.insert(encoded_record.end(), row.evidence_id.begin(),
                          row.evidence_id.end());
    if (encoded_record.size() > maximum_size - 1 ||
        total_bytes > maximum_size - encoded_record.size() - 1) {
      return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                  kResourceExceeded, carrier, "evidence_material_bytes",
                  index, "evidence material extent overflows size_t");
    }
    total_bytes += encoded_record.size() + 1;
    encoded_records.push_back(std::move(encoded_record));
  }
  std::sort(encoded_records.begin(), encoded_records.end(),
            [](const std::vector<byte>& left,
               const std::vector<byte>& right) {
              const auto common = std::min(left.size(), right.size());
              for (std::size_t index = 0; index < common; ++index) {
                if (left[index] != right[index]) {
                  return left[index] < right[index];
                }
              }
              return left.size() < right.size();
            });
  std::vector<byte> result;
  result.reserve(total_bytes);
  result.insert(result.end(), value.update_descriptor_uuid.begin(),
                value.update_descriptor_uuid.end());
  result.insert(result.end(), value.operation_uuid.begin(),
                value.operation_uuid.end());
  result.insert(result.end(), value.owning_transaction_uuid.begin(),
                value.owning_transaction_uuid.end());
  result.insert(result.end(), value.relation_uuid.begin(),
                value.relation_uuid.end());
  for (const auto& row : encoded_records) {
    result.insert(result.end(), row.begin(), row.end());
    result.push_back(0);
  }
  *material = std::move(result);
  return true;
}

bool ComputeTypedUpdateResultInnerEvidence(
    const TypedUpdateResultCarrier& value,
    std::span<const TypedUpdateResultEvidenceReference> evidence,
    TypedUpdateHash* effect_set_sha256,
    TypedUpdateHash* executor_evidence_sha256,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr TypedUpdateCarrierKind carrier = TypedUpdateCarrierKind::result;
  if (effect_set_sha256 == nullptr || executor_evidence_sha256 == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "inner_evidence_output", 0,
                "effect/executor evidence output pointer is null");
  }
  std::vector<byte> material;
  if (!EncodeTypedUpdateResultEvidenceMaterial(value, evidence, &material,
                                               error)) {
    return false;
  }
  TypedUpdateHash effect{};
  TypedUpdateHash executor{};
  if (!ComputeEvidence(kEffectSetDomain, material, &effect, error, carrier,
                       "effect_set_sha256") ||
      !ComputeEvidence(kExecutorEvidenceDomain, material, &executor, error,
                       carrier, "executor_evidence_sha256")) {
    return false;
  }
  *effect_set_sha256 = effect;
  *executor_evidence_sha256 = executor;
  return true;
}

bool EncodeTypedUpdateJournalRecord(
    const TypedUpdateJournalRecord& value,
    const TypedUpdateJournalChainContext& chain,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, TypedUpdateCarrierKind::journal,
                "encoded", 0, "output pointer is null");
  }
  std::vector<byte> descriptor_bytes;
  if (!EncodeTypedUpdateDescriptor(value.descriptor, &descriptor_bytes,
                                   error)) {
    if (error != nullptr) {
      error->carrier = TypedUpdateCarrierKind::journal;
      error->diagnostic_code = kUpdateFailed;
      error->field = "embedded_DUDC." + error->field;
    }
    return false;
  }
  if (!ValidateJournalOwnership(value, error) ||
      !ValidateJournalChain(value, chain, descriptor_bytes, error)) {
    return false;
  }
  std::vector<byte> result_bytes;
  if (value.prior_result.has_value() &&
      !EncodeTypedUpdateResult(value.prior_result.value(), &result_bytes,
                              error)) {
    if (error != nullptr) {
      error->carrier = TypedUpdateCarrierKind::journal;
      error->field = "embedded_DURS." + error->field;
    }
    return false;
  }
  const u32 total_bytes = value.prior_result.has_value()
                              ? kTypedUpdateJournalWithResultBytes
                              : kTypedUpdateJournalWithoutResultBytes;
  std::vector<byte> result(total_bytes, 0);
  StoreHeader(&result, "DUJR", kTypedUpdateJournalHeaderBytes, total_bytes);
  result[16] = static_cast<byte>(value.lifecycle_state);
  StoreLittle64(result.data() + 24, value.journal_sequence);
  StoreUuid(&result, 32, value.database_uuid);
  StoreUuid(&result, 48, value.descriptor.descriptor_uuid);
  StoreLittle64(result.data() + 64,
                value.descriptor.descriptor_generation);
  StoreUuid(&result, 72, value.authenticated_statement_receipt_uuid);
  StoreUuid(&result, 88, value.owning_transaction_uuid);
  StoreLittle64(result.data() + 104, value.owning_local_transaction_id);
  StoreUuid(&result, 112, value.operation_uuid);
  StoreUuid(&result, 128, value.recovery_token_uuid);
  StoreLittle64(result.data() + 144, value.recovery_generation);
  StoreUuid(&result, 152, value.statement_savepoint_uuid);
  StoreLittle64(result.data() + 168,
                value.statement_savepoint_generation);
  StoreLittle32(result.data() + 176, kTypedUpdateDescriptorBytes);
  StoreLittle32(result.data() + 180,
                value.prior_result.has_value() ? kTypedUpdateResultBytes : 0);
  StoreLittle32(result.data() + 184,
                kTypedUpdateDescriptorBytes +
                    (value.prior_result.has_value()
                         ? kTypedUpdateResultBytes
                         : 0));
  StoreHash(&result, 192, value.prior_record_sha256);
  std::copy(descriptor_bytes.begin(), descriptor_bytes.end(),
            result.begin() + kTypedUpdateJournalHeaderBytes);
  if (!result_bytes.empty()) {
    std::copy(result_bytes.begin(), result_bytes.end(),
              result.begin() + kTypedUpdateJournalWithoutResultBytes);
  }
  TypedUpdateHash evidence{};
  if (!ComputeJournalEvidence(result, &evidence, error)) {
    return false;
  }
  StoreHash(&result, 224, evidence);
  *encoded = std::move(result);
  return true;
}

bool DecodeAndValidateTypedUpdateJournalRecord(
    std::span<const byte> encoded,
    const TypedUpdateJournalChainContext& chain,
    TypedUpdateJournalRecord* decoded,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, TypedUpdateCarrierKind::journal,
                "decoded", 0, "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUJR", kTypedUpdateJournalHeaderBytes,
                      std::nullopt, TypedUpdateCarrierKind::journal, error,
                      kUpdateFailed)) {
    return false;
  }
  if (encoded.size() != kTypedUpdateJournalWithoutResultBytes &&
      encoded.size() != kTypedUpdateJournalWithResultBytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kUpdateFailed, TypedUpdateCarrierKind::journal,
                "total_bytes", 0, "DUJR total must be exactly 968 or 1224");
  }
  if (!AllZero(encoded.subspan(17, 7)) ||
      !AllZero(encoded.subspan(188, 4))) {
    return Fail(error, TypedUpdateCarrierErrorCode::reserved_invalid,
                kUpdateFailed, TypedUpdateCarrierKind::journal,
                "reserved", 0, "DUJR reserved bytes must be zero");
  }
  const u32 dudc_bytes = LoadLittle32(encoded.data() + 176);
  const u32 durs_bytes = LoadLittle32(encoded.data() + 180);
  const u32 payload_bytes = LoadLittle32(encoded.data() + 184);
  if (dudc_bytes != kTypedUpdateDescriptorBytes ||
      (durs_bytes != 0 && durs_bytes != kTypedUpdateResultBytes) ||
      payload_bytes != dudc_bytes + durs_bytes ||
      encoded.size() != kTypedUpdateJournalHeaderBytes + payload_bytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::extent_invalid,
                kUpdateFailed, TypedUpdateCarrierKind::journal,
                "payload_extent", 0,
                "DUJR payload is not exact DUDC712 plus optional DURS256");
  }
  TypedUpdateJournalRecord value;
  value.lifecycle_state =
      static_cast<TypedUpdateJournalState>(encoded[16]);
  value.journal_sequence = LoadLittle64(encoded.data() + 24);
  value.database_uuid = LoadUuid(encoded, 32);
  const auto header_descriptor_uuid = LoadUuid(encoded, 48);
  const u64 header_descriptor_generation =
      LoadLittle64(encoded.data() + 64);
  value.authenticated_statement_receipt_uuid = LoadUuid(encoded, 72);
  value.owning_transaction_uuid = LoadUuid(encoded, 88);
  value.owning_local_transaction_id = LoadLittle64(encoded.data() + 104);
  value.operation_uuid = LoadUuid(encoded, 112);
  value.recovery_token_uuid = LoadUuid(encoded, 128);
  value.recovery_generation = LoadLittle64(encoded.data() + 144);
  value.statement_savepoint_uuid = LoadUuid(encoded, 152);
  value.statement_savepoint_generation = LoadLittle64(encoded.data() + 168);
  value.prior_record_sha256 = LoadHash(encoded, 192);
  value.record_evidence_sha256 = LoadHash(encoded, 224);
  if (!UuidPresent(header_descriptor_uuid) ||
      header_descriptor_generation == 0) {
    return Fail(error, TypedUpdateCarrierErrorCode::recovery_identity_invalid,
                kUpdateFailed, TypedUpdateCarrierKind::journal,
                "descriptor_identity", 0,
                "DUJR header descriptor UUID and generation are nonzero");
  }
  if (!ValidateJournalPreEvidence(value, chain, durs_bytes != 0, error)) {
    return false;
  }
  TypedUpdateHash evidence{};
  if (!ComputeJournalEvidence(encoded, &evidence, error) ||
      evidence != value.record_evidence_sha256) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::journal_evidence_mismatch,
           kUpdateFailed, TypedUpdateCarrierKind::journal,
           "record_evidence_sha256", 0, "DUJR evidence mismatch");
    }
    return false;
  }
  const auto descriptor_span = encoded.subspan(
      kTypedUpdateJournalHeaderBytes, kTypedUpdateDescriptorBytes);
  if (!DecodeAndValidateTypedUpdateDescriptor(descriptor_span,
                                              &value.descriptor, error)) {
    if (error != nullptr) {
      error->carrier = TypedUpdateCarrierKind::journal;
      error->diagnostic_code = kUpdateFailed;
      error->field = "embedded_DUDC." + error->field;
    }
    return false;
  }
  if (header_descriptor_uuid != value.descriptor.descriptor_uuid ||
      header_descriptor_generation !=
          value.descriptor.descriptor_generation) {
    return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                kUpdateFailed, TypedUpdateCarrierKind::journal,
                "descriptor_identity", 0,
                "DUJR header descriptor identity differs from DUDC");
  }
  if (durs_bytes != 0) {
    TypedUpdateResultCarrier result;
    const auto result_span = encoded.last(kTypedUpdateResultBytes);
    if (!DecodeAndValidateTypedUpdateResult(result_span, &result, error)) {
      if (error != nullptr) {
        error->carrier = TypedUpdateCarrierKind::journal;
        error->field = "embedded_DURS." + error->field;
      }
      return false;
    }
    value.prior_result = std::move(result);
    value.embedded_result_bytes = std::vector<byte>(result_span.begin(),
                                                    result_span.end());
  }
  if (!ValidateJournalOwnership(value, error) ||
      !ValidateJournalChain(value, chain, descriptor_span, error)) {
    return false;
  }
  value.embedded_descriptor_bytes.assign(descriptor_span.begin(),
                                         descriptor_span.end());
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

bool ExtractTypedUpdateJournalRecordEvidence(
    std::span<const byte> encoded,
    TypedUpdateHash* record_evidence_sha256,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  if (record_evidence_sha256 == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, TypedUpdateCarrierKind::journal,
                "record_evidence_sha256", 0, "output pointer is null");
  }
  if (!ValidateHeader(encoded, "DUJR", kTypedUpdateJournalHeaderBytes,
                      std::nullopt, TypedUpdateCarrierKind::journal, error,
                      kUpdateFailed) ||
      (encoded.size() != kTypedUpdateJournalWithoutResultBytes &&
       encoded.size() != kTypedUpdateJournalWithResultBytes)) {
    return false;
  }
  const auto stored = LoadHash(encoded, 224);
  TypedUpdateHash computed{};
  if (!ComputeJournalEvidence(encoded, &computed, error) ||
      computed != stored) {
    if (error != nullptr && error->ok()) {
      Fail(error, TypedUpdateCarrierErrorCode::journal_evidence_mismatch,
           kUpdateFailed, TypedUpdateCarrierKind::journal,
           "record_evidence_sha256", 0, "DUJR evidence mismatch");
    }
    return false;
  }
  *record_evidence_sha256 = stored;
  return true;
}

namespace {

bool CompareVectorBinding(const TypedUpdateVectorIdentity& identity,
                          const TypedUpdateUuid& expected_uuid,
                          u64 expected_generation,
                          const TypedUpdateUuid& descriptor_uuid,
                          u64 descriptor_generation,
                          TypedUpdateCarrierKind carrier,
                          TypedUpdateCarrierError* error) {
  if (identity.vector_uuid != expected_uuid ||
      identity.vector_generation != expected_generation ||
      identity.owner_descriptor_uuid != descriptor_uuid ||
      identity.owner_descriptor_generation != descriptor_generation) {
    return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                kOperandInvalid, carrier, "vector_owner_or_identity", 0,
                "vector identity/owner does not equal DUDC reference");
  }
  return true;
}

bool ExactIfPresent(std::span<const byte> expected,
                    const std::vector<byte>& retained,
                    TypedUpdateCarrierKind carrier,
                    TypedUpdateCarrierError* error) {
  if (!retained.empty() &&
      (retained.size() != expected.size() ||
       !std::equal(retained.begin(), retained.end(), expected.begin()))) {
    return Fail(error, TypedUpdateCarrierErrorCode::carrier_set_mismatch,
                kOperandInvalid, carrier, "exact_bytes", 0,
                "decode/re-encode byte identity failed");
  }
  return true;
}

}  // namespace

bool ValidateTypedUpdateCarrierSet(const TypedUpdateCarrierSet& carriers,
                                   TypedUpdateCarrierError* error) {
  ClearError(error);
  const auto& descriptor = carriers.descriptor;
  std::vector<byte> assignments;
  std::vector<byte> predicate;
  std::vector<byte> policies;
  std::vector<byte> constraints;
  std::vector<byte> triggers;
  std::vector<byte> order;
  std::vector<byte> budget;
  std::vector<byte> recovery;
  if (!EncodeTypedUpdateAssignmentVector(carriers.assignments, &assignments,
                                         error) ||
      !EncodeTypedUpdatePredicateVector(carriers.predicate, &predicate,
                                        error) ||
      !EncodeTypedUpdateRowPolicyVector(carriers.row_policies, &policies,
                                        error) ||
      !EncodeTypedUpdateConstraintVector(carriers.constraints, &constraints,
                                         error) ||
      !EncodeTypedUpdateTriggerVector(carriers.triggers, &triggers, error) ||
      !EncodeTypedUpdateTargetOrder(carriers.target_order, &order, error) ||
      !EncodeTypedUpdateResourceBudget(carriers.resource_budget, &budget,
                                       error) ||
      !EncodeTypedUpdateRecoveryToken(carriers.recovery_token, &recovery,
                                      error)) {
    return false;
  }
  if (!CompareVectorBinding(carriers.assignments.identity,
                            descriptor.assignment_vector_uuid,
                            descriptor.assignment_vector_generation,
                            descriptor.descriptor_uuid,
                            descriptor.descriptor_generation,
                            TypedUpdateCarrierKind::assignment_vector,
                            error) ||
      !CompareVectorBinding(carriers.predicate.identity,
                            descriptor.predicate_expression_uuid,
                            descriptor.predicate_expression_generation,
                            descriptor.descriptor_uuid,
                            descriptor.descriptor_generation,
                            TypedUpdateCarrierKind::predicate_vector,
                            error) ||
      !CompareVectorBinding(carriers.row_policies.identity,
                            descriptor.row_policy_set_uuid,
                            descriptor.row_policy_set_generation,
                            descriptor.descriptor_uuid,
                            descriptor.descriptor_generation,
                            TypedUpdateCarrierKind::row_policy_vector,
                            error) ||
      !CompareVectorBinding(carriers.constraints.identity,
                            descriptor.constraint_set_uuid,
                            descriptor.constraint_set_generation,
                            descriptor.descriptor_uuid,
                            descriptor.descriptor_generation,
                            TypedUpdateCarrierKind::constraint_vector,
                            error) ||
      !CompareVectorBinding(carriers.triggers.identity,
                            descriptor.trigger_set_uuid,
                            descriptor.trigger_set_generation,
                            descriptor.descriptor_uuid,
                            descriptor.descriptor_generation,
                            TypedUpdateCarrierKind::trigger_vector,
                            error)) {
    return false;
  }
  const TypedUpdateHash assignment_hash = LoadHash(assignments, 72);
  const TypedUpdateHash predicate_hash = LoadHash(predicate, 72);
  const TypedUpdateHash policy_hash = LoadHash(policies, 72);
  const TypedUpdateHash constraint_hash = LoadHash(constraints, 72);
  const TypedUpdateHash trigger_hash = LoadHash(triggers, 72);
  if (descriptor.assignment_count != carriers.assignments.records.size() ||
      descriptor.predicate_node_count != carriers.predicate.records.size() ||
      descriptor.row_policy_count != carriers.row_policies.records.size() ||
      descriptor.constraint_count != carriers.constraints.records.size() ||
      descriptor.trigger_count != carriers.triggers.records.size() ||
      descriptor.assignment_vector_sha256 != assignment_hash ||
      descriptor.predicate_vector_sha256 != predicate_hash ||
      descriptor.row_policy_set_sha256 != policy_hash ||
      descriptor.ordered_constraint_set_sha256 != constraint_hash ||
      descriptor.ordered_trigger_set_sha256 != trigger_hash) {
    return Fail(error, TypedUpdateCarrierErrorCode::carrier_set_mismatch,
                kOperandInvalid, TypedUpdateCarrierKind::descriptor,
                "vector_count_or_sha256", 0,
                "DUDC vector count/hash does not equal exact vector bytes");
  }
  if (descriptor.predicate_root_node_id !=
      carriers.predicate.records.back().node_id) {
    return Fail(error, TypedUpdateCarrierErrorCode::predicate_shape_invalid,
                kOperandInvalid, TypedUpdateCarrierKind::predicate_vector,
                "predicate_root_node_id", 0,
                "DUDC predicate root is not the exact final postorder node");
  }
  for (u32 index = 0; index < carriers.predicate.records.size(); ++index) {
    const auto& row = carriers.predicate.records[index];
    if (row.node_kind ==
            TypedUpdatePredicateNodeKind::column_reference &&
        (row.referenced_relation_occurrence_uuid !=
             descriptor.target_relation_occurrence_uuid ||
         row.referenced_relation_occurrence_generation !=
             descriptor.target_relation_occurrence_generation)) {
      return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                  kOperandInvalid,
                  TypedUpdateCarrierKind::predicate_vector,
                  "referenced_relation_occurrence", index,
                  "predicate column reference uses another relation occurrence");
    }
  }
  for (u32 index = 0; index < carriers.row_policies.records.size(); ++index) {
    const auto& row = carriers.row_policies.records[index];
    if (row.security_snapshot_uuid != descriptor.security_snapshot_uuid ||
        row.security_generation != descriptor.security_generation) {
      return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                  kOperandInvalid,
                  TypedUpdateCarrierKind::row_policy_vector,
                  "security_snapshot", index,
                  "row-policy record uses another security snapshot");
    }
  }
  if (carriers.target_order.target_order_uuid !=
          descriptor.deterministic_target_order_uuid ||
      carriers.target_order.target_order_generation !=
          descriptor.deterministic_target_order_generation ||
      carriers.target_order.authenticated_statement_receipt_uuid !=
          descriptor.authenticated_statement_receipt_uuid ||
      carriers.target_order.target_relation_occurrence_uuid !=
          descriptor.target_relation_occurrence_uuid ||
      carriers.target_order.target_relation_occurrence_generation !=
          descriptor.target_relation_occurrence_generation ||
      carriers.target_order.statement_snapshot_uuid !=
          descriptor.statement_snapshot_uuid) {
    return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                kOperandInvalid, TypedUpdateCarrierKind::target_order,
                "DUDC_binding", 0, "DUOR does not equal DUDC authority");
  }
  if (carriers.resource_budget.resource_budget_uuid !=
          descriptor.resource_budget_uuid ||
      carriers.resource_budget.resource_budget_generation !=
          descriptor.resource_budget_generation ||
      carriers.resource_budget.authenticated_statement_receipt_uuid !=
          descriptor.authenticated_statement_receipt_uuid ||
      carriers.resource_budget.owning_transaction_uuid !=
          descriptor.owning_transaction_uuid) {
    return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                kOperandInvalid, TypedUpdateCarrierKind::resource_budget,
                "DUDC_binding", 0, "DUBR does not equal DUDC authority");
  }
  if (carriers.recovery_token.recovery_token_uuid !=
          descriptor.recovery_token_uuid ||
      carriers.recovery_token.recovery_generation !=
          descriptor.recovery_generation ||
      carriers.recovery_token.authenticated_statement_receipt_uuid !=
          descriptor.authenticated_statement_receipt_uuid ||
      carriers.recovery_token.owning_transaction_uuid !=
          descriptor.owning_transaction_uuid ||
      carriers.recovery_token.operation_uuid != descriptor.operation_uuid ||
      carriers.recovery_token.descriptor_uuid != descriptor.descriptor_uuid ||
      carriers.recovery_token.descriptor_generation !=
          descriptor.descriptor_generation) {
    return Fail(error, TypedUpdateCarrierErrorCode::ownership_mismatch,
                kOperandInvalid, TypedUpdateCarrierKind::recovery_token,
                "DUDC_binding", 0, "DURC does not equal DUDC authority");
  }
  if (carriers.target_order.maximum_candidate_rows !=
          carriers.resource_budget.maximum_candidate_rows ||
      descriptor.assignment_count >
          carriers.resource_budget.maximum_assignments ||
      descriptor.predicate_node_count >
          carriers.resource_budget.maximum_predicate_nodes) {
    return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                kResourceExceeded, TypedUpdateCarrierKind::resource_budget,
                "cross_carrier_limits", 0,
                "DUDC/vector counts exceed DUBR or DUOR differs from DUBR");
  }
  u64 total_value_bytes = 0;
  for (const auto& row : carriers.assignments.records) {
    total_value_bytes += row.canonical_value.size();
  }
  for (const auto& row : carriers.predicate.records) {
    total_value_bytes += row.canonical_value.size();
  }
  if (total_value_bytes >
      carriers.resource_budget.maximum_total_canonical_value_bytes) {
    return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                kResourceExceeded, TypedUpdateCarrierKind::resource_budget,
                "maximum_total_canonical_value_bytes", 0,
                "exact typed values exceed the bound resource budget");
  }
  for (u32 index = 0; index < carriers.triggers.records.size(); ++index) {
    if (carriers.triggers.records[index].maximum_depth >
        carriers.resource_budget.maximum_trigger_depth) {
      return Fail(error, TypedUpdateCarrierErrorCode::resource_limit_exceeded,
                  kResourceExceeded,
                  TypedUpdateCarrierKind::trigger_vector, "maximum_depth",
                  index, "trigger depth exceeds DUBR");
    }
  }
  std::vector<byte> dudc;
  if (!EncodeTypedUpdateDescriptor(descriptor, &dudc, error)) {
    return false;
  }
  if (!ExactIfPresent(assignments, carriers.assignments.exact_bytes,
                      TypedUpdateCarrierKind::assignment_vector, error) ||
      !ExactIfPresent(predicate, carriers.predicate.exact_bytes,
                      TypedUpdateCarrierKind::predicate_vector, error) ||
      !ExactIfPresent(policies, carriers.row_policies.exact_bytes,
                      TypedUpdateCarrierKind::row_policy_vector, error) ||
      !ExactIfPresent(constraints, carriers.constraints.exact_bytes,
                      TypedUpdateCarrierKind::constraint_vector, error) ||
      !ExactIfPresent(triggers, carriers.triggers.exact_bytes,
                      TypedUpdateCarrierKind::trigger_vector, error) ||
      !ExactIfPresent(order, carriers.target_order.exact_bytes,
                      TypedUpdateCarrierKind::target_order, error) ||
      !ExactIfPresent(budget, carriers.resource_budget.exact_bytes,
                      TypedUpdateCarrierKind::resource_budget, error) ||
      !ExactIfPresent(recovery, carriers.recovery_token.exact_bytes,
                      TypedUpdateCarrierKind::recovery_token, error) ||
      !ExactIfPresent(dudc, descriptor.exact_bytes,
                      TypedUpdateCarrierKind::descriptor, error)) {
    return false;
  }
  return true;
}

namespace {

struct DatatypeAuthorityReference {
  TypedUpdateUuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  TypedUpdateUuid type_uuid{};
  u64 type_generation = 0;
  std::string codec_id;
  u16 codec_version = 0;
  u64 codec_generation = 0;
};

bool SameDatatypeAuthorityReference(
    const DatatypeAuthorityReference& left,
    const DatatypeAuthorityReference& right) {
  return left.descriptor_uuid == right.descriptor_uuid &&
         left.descriptor_generation == right.descriptor_generation &&
         left.type_uuid == right.type_uuid &&
         left.type_generation == right.type_generation &&
         left.codec_id == right.codec_id &&
         left.codec_version == right.codec_version &&
         left.codec_generation == right.codec_generation;
}

bool DatatypeRecordMatches(
    const TypedUpdateDatatypeAuthorityRecord& record,
    const DatatypeAuthorityReference& reference) {
  return record.descriptor_uuid == reference.descriptor_uuid &&
         record.descriptor_generation == reference.descriptor_generation &&
         record.type_uuid == reference.type_uuid &&
         record.type_generation == reference.type_generation &&
         record.codec_id == reference.codec_id &&
         record.codec_version == reference.codec_version &&
         record.codec_generation == reference.codec_generation;
}

void AddDatatypeAuthorityReference(
    const DatatypeAuthorityReference& reference,
    std::vector<DatatypeAuthorityReference>* references) {
  const auto iterator = std::find_if(
      references->begin(), references->end(),
      [&reference](const DatatypeAuthorityReference& existing) {
        return SameDatatypeAuthorityReference(existing, reference);
      });
  if (iterator == references->end()) {
    references->push_back(reference);
  }
}

bool DatatypeOperatorBindingFailure(
    TypedUpdateCarrierError* error,
    TypedUpdateCarrierKind carrier,
    std::string field,
    u32 record_index,
    std::string detail) {
  return Fail(
      error,
      TypedUpdateCarrierErrorCode::datatype_operator_binding_mismatch,
      kUpdateFailed, carrier, std::move(field), record_index,
      std::move(detail));
}

void RemapDatatypeOperatorDiagnostic(TypedUpdateCarrierError* error) {
  if (error != nullptr && !error->ok()) {
    error->diagnostic_code = kUpdateFailed;
  }
}

bool RequireExactDatatypeOperatorBytes(
    std::span<const byte> canonical,
    const std::vector<byte>& retained,
    TypedUpdateCarrierKind carrier,
    std::string field,
    u32 record_index,
    TypedUpdateCarrierError* error) {
  if (retained.size() != canonical.size() ||
      !std::equal(retained.begin(), retained.end(), canonical.begin())) {
    return DatatypeOperatorBindingFailure(
        error, carrier, std::move(field), record_index,
        "durable datatype/operator authority requires retained byte-identical carrier bytes");
  }
  return true;
}

bool ValidateCanonicalValueExtent(
    const TypedUpdateDatatypeAuthorityRecord& datatype,
    TypedUpdateValueState state,
    std::span<const byte> canonical_value,
    TypedUpdateCarrierKind carrier,
    u32 record_index,
    TypedUpdateCarrierError* error) {
  if (state == TypedUpdateValueState::value &&
      canonical_value.size() != datatype.canonical_value_exact_bytes) {
    return DatatypeOperatorBindingFailure(
        error, carrier, "canonical_value", record_index,
        "typed VALUE extent does not equal the exact DUDR canonical width");
  }
  if (state != TypedUpdateValueState::value && !canonical_value.empty()) {
    return DatatypeOperatorBindingFailure(
        error, carrier, "canonical_value", record_index,
        "ABSENT or NULL typed state must not carry canonical value bytes");
  }
  return true;
}

bool SamePredicateDatatypeIdentity(
    const TypedUpdateBuiltinOperatorAuthorityRecord& authority,
    const TypedUpdatePredicateRecord& node,
    bool left) {
  return (left ? authority.left_descriptor_uuid
               : authority.right_descriptor_uuid) ==
             node.output_descriptor_uuid &&
         (left ? authority.left_descriptor_generation
               : authority.right_descriptor_generation) ==
             node.output_descriptor_generation &&
         (left ? authority.left_type_uuid : authority.right_type_uuid) ==
             node.output_type_uuid &&
         (left ? authority.left_type_generation
               : authority.right_type_generation) ==
             node.output_type_generation;
}

}  // namespace

bool ValidateTypedUpdateDatatypeOperatorAuthority(
    const TypedUpdateDescriptorCarrier& descriptor,
    const TypedUpdateAssignmentVector& assignments,
    const TypedUpdatePredicateVector& predicate,
    const TypedUpdateDatatypeAuthorityVector& datatypes,
    const TypedUpdateBuiltinOperatorAuthorityVector& operators,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  std::vector<byte> dudc;
  std::vector<byte> duav;
  std::vector<byte> duev;
  std::vector<byte> dudv;
  std::vector<byte> duov;
  if (!EncodeTypedUpdateDescriptor(descriptor, &dudc, error) ||
      !EncodeTypedUpdateAssignmentVector(assignments, &duav, error) ||
      !EncodeTypedUpdatePredicateVector(predicate, &duev, error) ||
      !EncodeTypedUpdateDatatypeAuthorityVector(datatypes, &dudv, error) ||
      !EncodeTypedUpdateBuiltinOperatorAuthorityVector(operators, &duov,
                                                       error)) {
    RemapDatatypeOperatorDiagnostic(error);
    return false;
  }

  if (!RequireExactDatatypeOperatorBytes(
          dudc, descriptor.exact_bytes,
          TypedUpdateCarrierKind::descriptor, "exact_bytes", 0, error) ||
      !RequireExactDatatypeOperatorBytes(
          duav, assignments.exact_bytes,
          TypedUpdateCarrierKind::assignment_vector, "exact_bytes", 0,
          error) ||
      !RequireExactDatatypeOperatorBytes(
          duev, predicate.exact_bytes,
          TypedUpdateCarrierKind::predicate_vector, "exact_bytes", 0,
          error) ||
      !RequireExactDatatypeOperatorBytes(
          dudv, datatypes.exact_bytes,
          TypedUpdateCarrierKind::datatype_authority_vector, "exact_bytes",
          0, error) ||
      !RequireExactDatatypeOperatorBytes(
          duov, operators.exact_bytes,
          TypedUpdateCarrierKind::builtin_operator_authority_vector,
          "exact_bytes", 0, error)) {
    return false;
  }

  if (assignments.identity.vector_uuid !=
          descriptor.assignment_vector_uuid ||
      assignments.identity.vector_generation !=
          descriptor.assignment_vector_generation ||
      assignments.identity.owner_descriptor_uuid != descriptor.descriptor_uuid ||
      assignments.identity.owner_descriptor_generation !=
          descriptor.descriptor_generation ||
      assignments.records.size() != descriptor.assignment_count ||
      LoadHash(duav, 72) != descriptor.assignment_vector_sha256) {
    return DatatypeOperatorBindingFailure(
        error, TypedUpdateCarrierKind::assignment_vector, "DUDC_binding", 0,
        "DUAV identity, owner, count, or SHA-256 does not equal DUDC authority");
  }
  if (predicate.identity.vector_uuid !=
          descriptor.predicate_expression_uuid ||
      predicate.identity.vector_generation !=
          descriptor.predicate_expression_generation ||
      predicate.identity.owner_descriptor_uuid != descriptor.descriptor_uuid ||
      predicate.identity.owner_descriptor_generation !=
          descriptor.descriptor_generation ||
      predicate.records.size() != descriptor.predicate_node_count ||
      predicate.records.back().node_id != descriptor.predicate_root_node_id ||
      LoadHash(duev, 72) != descriptor.predicate_vector_sha256) {
    return DatatypeOperatorBindingFailure(
        error, TypedUpdateCarrierKind::predicate_vector, "DUDC_binding", 0,
        "DUEV identity, owner, root, count, or SHA-256 does not equal DUDC authority");
  }

  for (u32 index = 0; index < datatypes.records.size(); ++index) {
    const auto record_offset = kTypedUpdateVectorHeaderBytes +
                               index *
                                   kTypedUpdateDatatypeAuthorityRecordBytes;
    if (!RequireExactDatatypeOperatorBytes(
            std::span<const byte>(dudv).subspan(
                record_offset, kTypedUpdateDatatypeAuthorityRecordBytes),
            datatypes.records[index].exact_bytes,
            TypedUpdateCarrierKind::datatype_authority_vector,
            "records.exact_bytes", index, error)) {
      return false;
    }
  }
  for (u32 index = 0; index < operators.records.size(); ++index) {
    const auto record_offset = kTypedUpdateVectorHeaderBytes +
                               index *
                                   kTypedUpdateBuiltinOperatorAuthorityRecordBytes;
    if (!RequireExactDatatypeOperatorBytes(
            std::span<const byte>(duov).subspan(
                record_offset,
                kTypedUpdateBuiltinOperatorAuthorityRecordBytes),
            operators.records[index].exact_bytes,
            TypedUpdateCarrierKind::builtin_operator_authority_vector,
            "records.exact_bytes", index, error)) {
      return false;
    }
  }

  if (datatypes.identity.vector_uuid !=
          kTypedUpdateDatatypeSnapshotUuid ||
      datatypes.identity.vector_generation !=
          descriptor.datatype_registry_generation ||
      datatypes.identity.owner_descriptor_uuid !=
          descriptor.descriptor_uuid ||
      datatypes.identity.owner_descriptor_generation !=
          descriptor.descriptor_generation) {
    return DatatypeOperatorBindingFailure(
        error, TypedUpdateCarrierKind::datatype_authority_vector,
        "DUDC_binding", 0,
        "DUDV snapshot generation or owner does not equal DUDC authority");
  }
  if (operators.identity.vector_uuid !=
          descriptor.builtin_operator_snapshot_uuid ||
      operators.identity.vector_generation !=
          descriptor.builtin_operator_registry_generation ||
      operators.identity.owner_descriptor_uuid !=
          descriptor.descriptor_uuid ||
      operators.identity.owner_descriptor_generation !=
          descriptor.descriptor_generation) {
    return DatatypeOperatorBindingFailure(
        error, TypedUpdateCarrierKind::builtin_operator_authority_vector,
        "DUDC_binding", 0,
        "DUOV snapshot generation or owner does not equal DUDC authority");
  }

  std::vector<DatatypeAuthorityReference> references;
  references.reserve(assignments.records.size() + predicate.records.size());
  for (const auto& record : assignments.records) {
    AddDatatypeAuthorityReference(
        {record.value_descriptor_uuid, record.value_descriptor_generation,
         record.value_type_uuid, record.value_type_generation,
         record.codec_id, record.codec_version, record.codec_generation},
        &references);
  }
  for (const auto& record : predicate.records) {
    AddDatatypeAuthorityReference(
        {record.output_descriptor_uuid,
         record.output_descriptor_generation,
         record.output_type_uuid,
         record.output_type_generation,
         record.output_codec_id,
         record.output_codec_version,
         record.output_codec_generation},
        &references);
  }

  std::vector<bool> referenced_datatypes(datatypes.records.size(), false);
  for (u32 reference_index = 0; reference_index < references.size();
       ++reference_index) {
    const auto iterator = std::find_if(
        datatypes.records.begin(), datatypes.records.end(),
        [&references, reference_index](
            const TypedUpdateDatatypeAuthorityRecord& record) {
          return DatatypeRecordMatches(record,
                                       references[reference_index]);
        });
    if (iterator == datatypes.records.end()) {
      return DatatypeOperatorBindingFailure(
          error, TypedUpdateCarrierKind::datatype_authority_vector,
          "referenced_datatype", reference_index,
          "a DUAV or DUEV descriptor/type/codec identity has no exact DUDR row");
    }
    referenced_datatypes[static_cast<std::size_t>(
        std::distance(datatypes.records.begin(), iterator))] = true;
  }
  const auto unreferenced = std::find(referenced_datatypes.begin(),
                                      referenced_datatypes.end(), false);
  if (unreferenced != referenced_datatypes.end()) {
    return DatatypeOperatorBindingFailure(
        error, TypedUpdateCarrierKind::datatype_authority_vector,
        "unreferenced_datatype",
        static_cast<u32>(std::distance(referenced_datatypes.begin(),
                                       unreferenced)),
        "DUDV contains a datatype row not referenced by DUAV or DUEV");
  }

  for (u32 index = 0; index < assignments.records.size(); ++index) {
    const auto& assignment = assignments.records[index];
    const auto iterator = std::find_if(
        datatypes.records.begin(), datatypes.records.end(),
        [&assignment](const TypedUpdateDatatypeAuthorityRecord& record) {
          return DatatypeRecordMatches(
              record,
              {assignment.value_descriptor_uuid,
               assignment.value_descriptor_generation,
               assignment.value_type_uuid,
               assignment.value_type_generation,
               assignment.codec_id,
               assignment.codec_version,
               assignment.codec_generation});
        });
    if (iterator == datatypes.records.end() ||
        !ValidateCanonicalValueExtent(*iterator, assignment.value_state,
                                      assignment.canonical_value,
                                      TypedUpdateCarrierKind::assignment_vector,
                                      index, error)) {
      if (iterator == datatypes.records.end()) {
        DatatypeOperatorBindingFailure(
            error, TypedUpdateCarrierKind::assignment_vector,
            "datatype_identity", index,
            "assignment has no exact DUDR authority row");
      }
      return false;
    }
  }
  for (u32 index = 0; index < predicate.records.size(); ++index) {
    const auto& node = predicate.records[index];
    const auto iterator = std::find_if(
        datatypes.records.begin(), datatypes.records.end(),
        [&node](const TypedUpdateDatatypeAuthorityRecord& record) {
          return DatatypeRecordMatches(
              record,
              {node.output_descriptor_uuid,
               node.output_descriptor_generation,
               node.output_type_uuid,
               node.output_type_generation,
               node.output_codec_id,
               node.output_codec_version,
               node.output_codec_generation});
        });
    if (iterator == datatypes.records.end() ||
        !ValidateCanonicalValueExtent(*iterator, node.value_state,
                                      node.canonical_value,
                                      TypedUpdateCarrierKind::predicate_vector,
                                      index, error)) {
      if (iterator == datatypes.records.end()) {
        DatatypeOperatorBindingFailure(
            error, TypedUpdateCarrierKind::predicate_vector,
            "datatype_identity", index,
            "predicate node has no exact DUDR authority row");
      }
      return false;
    }
  }

  if (predicate.records.size() == 1) {
    if (predicate.records.front().node_kind !=
            TypedUpdatePredicateNodeKind::canonical_boolean_constant ||
        !operators.records.empty()) {
      return DatatypeOperatorBindingFailure(
          error, TypedUpdateCarrierKind::builtin_operator_authority_vector,
          "operator_inclusion", 0,
          "one-node canonical TRUE requires an exact empty DUOV");
    }
  } else {
    if (predicate.records.size() != 3 || operators.records.size() != 1) {
      return DatatypeOperatorBindingFailure(
          error, TypedUpdateCarrierKind::builtin_operator_authority_vector,
          "operator_inclusion", 0,
          "three-node equality requires exactly one DUOE authority row");
    }
    const auto& left = predicate.records[0];
    const auto& right = predicate.records[1];
    const auto& comparison = predicate.records[2];
    const auto& authority = operators.records.front();
    if (left.node_id != comparison.left_child_node_id ||
        right.node_id != comparison.right_child_node_id ||
        comparison.operator_uuid != authority.operator_uuid ||
        comparison.operator_generation != authority.operator_generation ||
        !SamePredicateDatatypeIdentity(authority, left, true) ||
        !SamePredicateDatatypeIdentity(authority, right, false) ||
        authority.result_descriptor_uuid !=
            comparison.output_descriptor_uuid ||
        authority.result_descriptor_generation !=
            comparison.output_descriptor_generation ||
        authority.result_type_uuid != comparison.output_type_uuid ||
        authority.result_type_generation !=
            comparison.output_type_generation ||
        authority.result_codec_id != comparison.output_codec_id ||
        authority.result_codec_version != comparison.output_codec_version ||
        authority.result_codec_generation !=
            comparison.output_codec_generation) {
      return DatatypeOperatorBindingFailure(
          error, TypedUpdateCarrierKind::builtin_operator_authority_vector,
          "DUEV_binding", 0,
          "DUOE equality identity, children, operands, or result differs from exact DUEV");
    }
  }
  return true;
}

namespace {

void RemapRecoveryDiagnostic(TypedUpdateCarrierError* error) {
  if (error != nullptr && !error->ok()) {
    error->diagnostic_code = kUpdateFailed;
  }
}

bool RequireExactRecoveryBytes(std::span<const byte> canonical,
                               const std::vector<byte>& retained,
                               TypedUpdateCarrierKind carrier,
                               TypedUpdateCarrierError* error) {
  if (retained.size() != canonical.size() ||
      !std::equal(retained.begin(), retained.end(), canonical.begin())) {
    return Fail(
        error,
        TypedUpdateCarrierErrorCode::security_recovery_binding_mismatch,
        kUpdateFailed, carrier, "exact_bytes", 0,
        "durable recovery authority requires retained byte-identical carrier bytes");
  }
  return true;
}

bool SecurityRecoveryBindingFailure(TypedUpdateCarrierError* error,
                                    TypedUpdateCarrierKind carrier,
                                    std::string field,
                                    u32 record_index,
                                    std::string detail) {
  return Fail(
      error,
      TypedUpdateCarrierErrorCode::security_recovery_binding_mismatch,
      kUpdateFailed, carrier, std::move(field), record_index,
      std::move(detail));
}

}  // namespace

bool ValidateTypedUpdateSecurityRecoveryAuthority(
    const TypedUpdateDescriptorCarrier& descriptor,
    const TypedUpdateRowPolicyVector& row_policies,
    const TypedUpdateRecoveryTokenCarrier& recovery_token,
    const TypedUpdateSecurityPolicySourceVector& source_policies,
    const TypedUpdateSecuritySnapshotProof& snapshot_proof,
    const TypedUpdateJournalRecord& journal_head,
    const TypedUpdateMgaRecoveryObservation& recovery_observation,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  std::vector<byte> dudc;
  std::vector<byte> dupv;
  std::vector<byte> durc;
  std::vector<byte> dusv;
  std::vector<byte> dusp;
  std::vector<byte> dumo;
  if (!EncodeTypedUpdateDescriptor(descriptor, &dudc, error) ||
      !EncodeTypedUpdateRowPolicyVector(row_policies, &dupv, error) ||
      !EncodeTypedUpdateRecoveryToken(recovery_token, &durc, error) ||
      !EncodeTypedUpdateSecurityPolicySourceVector(source_policies, &dusv,
                                                   error) ||
      !EncodeTypedUpdateSecuritySnapshotProof(snapshot_proof, &dusp,
                                              error) ||
      !EncodeTypedUpdateMgaRecoveryObservation(recovery_observation, &dumo,
                                               error)) {
    RemapRecoveryDiagnostic(error);
    return false;
  }
  if (!RequireExactRecoveryBytes(dudc, descriptor.exact_bytes,
                                 TypedUpdateCarrierKind::descriptor,
                                 error) ||
      !RequireExactRecoveryBytes(dupv, row_policies.exact_bytes,
                                 TypedUpdateCarrierKind::row_policy_vector,
                                 error) ||
      !RequireExactRecoveryBytes(durc, recovery_token.exact_bytes,
                                 TypedUpdateCarrierKind::recovery_token,
                                 error) ||
      !RequireExactRecoveryBytes(
          dusv, source_policies.exact_bytes,
          TypedUpdateCarrierKind::security_policy_source_vector, error) ||
      !RequireExactRecoveryBytes(
          dusp, snapshot_proof.exact_bytes,
          TypedUpdateCarrierKind::security_snapshot_proof, error) ||
      !RequireExactRecoveryBytes(
          dumo, recovery_observation.exact_bytes,
          TypedUpdateCarrierKind::mga_recovery_observation, error)) {
    return false;
  }
  if (journal_head.exact_bytes.empty() ||
      journal_head.embedded_descriptor_bytes.size() != dudc.size()) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::journal, "exact_bytes", 0,
        "DUJR exact bytes and exact embedded DUDC are required");
  }
  TypedUpdateHash journal_evidence{};
  if (!ExtractTypedUpdateJournalRecordEvidence(journal_head.exact_bytes,
                                               &journal_evidence, error)) {
    RemapRecoveryDiagnostic(error);
    return false;
  }
  const auto journal_bytes =
      std::span<const byte>(journal_head.exact_bytes);
  if (journal_bytes.size() < kTypedUpdateJournalHeaderBytes + dudc.size() ||
      !std::equal(dudc.begin(), dudc.end(),
                  journal_bytes.begin() +
                      kTypedUpdateJournalHeaderBytes) ||
      !std::equal(dudc.begin(), dudc.end(),
                  journal_head.embedded_descriptor_bytes.begin()) ||
      journal_evidence != journal_head.record_evidence_sha256 ||
      journal_head.lifecycle_state !=
          static_cast<TypedUpdateJournalState>(journal_bytes[16]) ||
      journal_head.journal_sequence !=
          LoadLittle64(journal_bytes.data() + 24) ||
      journal_head.database_uuid != LoadUuid(journal_bytes, 32) ||
      descriptor.descriptor_uuid != LoadUuid(journal_bytes, 48) ||
      descriptor.descriptor_generation !=
          LoadLittle64(journal_bytes.data() + 64) ||
      journal_head.authenticated_statement_receipt_uuid !=
          LoadUuid(journal_bytes, 72) ||
      journal_head.owning_transaction_uuid !=
          LoadUuid(journal_bytes, 88) ||
      journal_head.owning_local_transaction_id !=
          LoadLittle64(journal_bytes.data() + 104) ||
      journal_head.operation_uuid != LoadUuid(journal_bytes, 112) ||
      journal_head.recovery_token_uuid != LoadUuid(journal_bytes, 128) ||
      journal_head.recovery_generation !=
          LoadLittle64(journal_bytes.data() + 144) ||
      journal_head.statement_savepoint_uuid !=
          LoadUuid(journal_bytes, 152) ||
      journal_head.statement_savepoint_generation !=
          LoadLittle64(journal_bytes.data() + 168)) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::journal, "DUJR_binding", 0,
        "DUJR retained fields, exact payload, or evidence disagree");
  }

  const auto& proof = snapshot_proof;
  if (proof.authenticated_statement_receipt_uuid !=
          descriptor.authenticated_statement_receipt_uuid ||
      proof.owning_transaction_uuid != descriptor.owning_transaction_uuid ||
      proof.owning_local_transaction_id !=
          descriptor.owning_local_transaction_id ||
      proof.operation_uuid != descriptor.operation_uuid ||
      proof.operation_generation != descriptor.operation_generation ||
      proof.recovery_token_uuid != descriptor.recovery_token_uuid ||
      proof.recovery_generation != descriptor.recovery_generation ||
      proof.statement_snapshot_uuid != descriptor.statement_snapshot_uuid ||
      proof.catalog_snapshot_uuid != descriptor.catalog_snapshot_uuid ||
      proof.catalog_generation != descriptor.catalog_generation ||
      proof.security_context_uuid != descriptor.security_context_uuid ||
      proof.security_snapshot_uuid != descriptor.security_snapshot_uuid ||
      proof.security_epoch != descriptor.security_generation ||
      proof.target_relation_uuid != descriptor.target_relation_uuid ||
      proof.target_relation_generation !=
          descriptor.target_relation_generation ||
      proof.target_relation_occurrence_uuid !=
          descriptor.target_relation_occurrence_uuid ||
      proof.target_relation_occurrence_generation !=
          descriptor.target_relation_occurrence_generation ||
      proof.descriptor_uuid != descriptor.descriptor_uuid ||
      proof.descriptor_generation != descriptor.descriptor_generation ||
      proof.row_policy_set_uuid != descriptor.row_policy_set_uuid ||
      proof.row_policy_set_generation !=
          descriptor.row_policy_set_generation ||
      proof.row_policy_count != descriptor.row_policy_count ||
      proof.descriptor_evidence_sha256 !=
          descriptor.descriptor_evidence_sha256 ||
      proof.row_policy_set_sha256 != descriptor.row_policy_set_sha256) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::security_snapshot_proof,
        "DUDC_binding", 0,
        "DUSP receipt, transaction, operation, snapshot, relation, descriptor, or row-policy authority differs from DUDC");
  }
  if (proof.database_uuid != journal_head.database_uuid ||
      proof.authenticated_statement_receipt_uuid !=
          journal_head.authenticated_statement_receipt_uuid ||
      proof.owning_transaction_uuid !=
          journal_head.owning_transaction_uuid ||
      proof.owning_local_transaction_id !=
          journal_head.owning_local_transaction_id ||
      proof.operation_uuid != journal_head.operation_uuid ||
      proof.recovery_token_uuid != journal_head.recovery_token_uuid ||
      proof.recovery_generation != journal_head.recovery_generation) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::security_snapshot_proof,
        "DUJR_binding", 0,
        "DUSP durable owner identity differs from the DUJR chain head");
  }
  if (recovery_token.recovery_token_uuid != proof.recovery_token_uuid ||
      recovery_token.recovery_generation != proof.recovery_generation ||
      recovery_token.authenticated_statement_receipt_uuid !=
          proof.authenticated_statement_receipt_uuid ||
      recovery_token.owning_transaction_uuid !=
          proof.owning_transaction_uuid ||
      recovery_token.operation_uuid != proof.operation_uuid ||
      recovery_token.descriptor_uuid != proof.descriptor_uuid ||
      recovery_token.descriptor_generation != proof.descriptor_generation) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::recovery_token, "DUSP_binding", 0,
        "DURC recovery identity differs from DUSP/DUDC authority");
  }
  if (row_policies.identity.vector_uuid != proof.row_policy_set_uuid ||
      row_policies.identity.vector_generation !=
          proof.row_policy_set_generation ||
      row_policies.identity.owner_descriptor_uuid != proof.descriptor_uuid ||
      row_policies.identity.owner_descriptor_generation !=
          proof.descriptor_generation ||
      row_policies.records.size() != proof.row_policy_count ||
      LoadHash(dupv, 72) != proof.row_policy_set_sha256) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::row_policy_vector, "DUSP_binding", 0,
        "DUPV identity, owner, count, or hash differs from DUSP");
  }
  if (source_policies.identity.vector_uuid !=
          proof.source_policy_vector_uuid ||
      source_policies.identity.vector_generation !=
          proof.source_policy_vector_generation ||
      source_policies.identity.owner_descriptor_uuid !=
          proof.descriptor_uuid ||
      source_policies.identity.owner_descriptor_generation !=
          proof.descriptor_generation ||
      source_policies.records.size() != proof.source_policy_count ||
      LoadHash(dusv, 72) !=
          proof.source_policy_catalog_vector_sha256) {
    return SecurityRecoveryBindingFailure(
        error,
        TypedUpdateCarrierKind::security_policy_source_vector,
        "DUSP_binding", 0,
        "DUSV identity, owner, count, or vector hash differs from DUSP");
  }
  TypedUpdateHash exact_dudc_hash{};
  TypedUpdateHash exact_dupv_hash{};
  if (!ComputeHash(dudc, &exact_dudc_hash, error,
                   TypedUpdateCarrierKind::security_snapshot_proof,
                   "exact_DUDC_sha256", 0, kUpdateFailed) ||
      !ComputeHash(dupv, &exact_dupv_hash, error,
                   TypedUpdateCarrierKind::security_snapshot_proof,
                   "exact_DUPV_sha256", 0, kUpdateFailed)) {
    return false;
  }
  if (exact_dudc_hash != proof.exact_dudc_sha256 ||
      exact_dupv_hash != proof.exact_dupv_sha256) {
    return Fail(error,
                TypedUpdateCarrierErrorCode::exact_byte_hash_mismatch,
                kUpdateFailed,
                TypedUpdateCarrierKind::security_snapshot_proof,
                "exact_DUDC_or_DUPV_sha256", 0,
                "DUSP raw exact-carrier SHA-256 does not match DUDC/DUPV bytes");
  }
  for (u32 index = 0; index < row_policies.records.size(); ++index) {
    const auto& row = row_policies.records[index];
    if (row.security_snapshot_uuid != proof.security_snapshot_uuid ||
        row.security_generation != proof.security_epoch ||
        row.source_policy_catalog_vector_sha256 !=
            proof.source_policy_catalog_vector_sha256) {
      return SecurityRecoveryBindingFailure(
          error, TypedUpdateCarrierKind::row_policy_vector,
          "security_source_binding", index,
          "DUPV security epoch or source-policy vector hash differs from DUSP/DUSV");
    }
  }
  for (u32 index = 0; index < source_policies.records.size(); ++index) {
    const auto& row = source_policies.records[index];
    if (row.target_relation_uuid != proof.target_relation_uuid ||
        row.target_relation_generation !=
            proof.target_relation_generation ||
        row.catalog_snapshot_uuid != proof.catalog_snapshot_uuid ||
        row.catalog_generation != proof.catalog_generation ||
        row.security_snapshot_uuid != proof.security_snapshot_uuid ||
        row.security_snapshot_generation !=
            proof.security_snapshot_generation) {
      return SecurityRecoveryBindingFailure(
          error,
          TypedUpdateCarrierKind::security_policy_source_vector,
          "snapshot_binding", index,
          "DUSR relation, catalog, or security snapshot differs from DUSP");
    }
  }

  const auto& observation = recovery_observation;
  if (observation.validated_mga_durable_handle_uuid !=
          recovery_token.durable_registry_uuid ||
      observation.validated_mga_durable_handle_generation !=
          recovery_token.durable_registry_generation ||
      observation.database_uuid != proof.database_uuid ||
      observation.descriptor_uuid != proof.descriptor_uuid ||
      observation.descriptor_generation != proof.descriptor_generation ||
      observation.operation_uuid != proof.operation_uuid ||
      observation.operation_generation != proof.operation_generation ||
      observation.authenticated_statement_receipt_uuid !=
          proof.authenticated_statement_receipt_uuid ||
      observation.owning_transaction_uuid !=
          proof.owning_transaction_uuid ||
      observation.owning_local_transaction_id !=
          proof.owning_local_transaction_id ||
      observation.recovery_token_uuid != proof.recovery_token_uuid ||
      observation.recovery_generation != proof.recovery_generation ||
      observation.catalog_snapshot_uuid != proof.catalog_snapshot_uuid ||
      observation.catalog_generation != proof.catalog_generation ||
      observation.security_snapshot_uuid != proof.security_snapshot_uuid ||
      observation.security_snapshot_generation !=
          proof.security_snapshot_generation ||
      observation.security_epoch != proof.security_epoch ||
      observation.latest_journal_state != journal_head.lifecycle_state ||
      observation.durable_chain_head_sequence !=
          journal_head.journal_sequence ||
      observation.durable_chain_head_record_evidence_sha256 !=
          journal_head.record_evidence_sha256) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::mga_recovery_observation,
        "authority_binding", 0,
        "DUMO handle, owner, snapshot, or durable chain head differs from DURC/DUSP/DUJR");
  }
  if (journal_head.lifecycle_state != TypedUpdateJournalState::bound &&
      (observation.statement_savepoint_uuid !=
           journal_head.statement_savepoint_uuid ||
       observation.statement_savepoint_generation !=
           journal_head.statement_savepoint_generation)) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::mga_recovery_observation,
        "statement_savepoint_identity", 0,
        "DUMO savepoint differs from the non-bound DUJR chain head");
  }
  if (journal_head.prior_result.has_value() &&
      (journal_head.prior_result->publication_barrier_uuid !=
           observation.reserved_statement_barrier_uuid ||
       journal_head.prior_result->publication_barrier_generation !=
           observation.reserved_statement_barrier_generation)) {
    return SecurityRecoveryBindingFailure(
        error, TypedUpdateCarrierKind::mga_recovery_observation,
        "reserved_statement_barrier_identity", 0,
        "DUMO reserved statement barrier differs from embedded DURS");
  }
  return true;
}

bool DecideTypedUpdateRecovery(
    const TypedUpdateMgaRecoveryObservation& observation,
    TypedUpdateRecoveryDecision* decision,
    TypedUpdateCarrierError* error) {
  ClearError(error);
  constexpr auto carrier =
      TypedUpdateCarrierKind::mga_recovery_observation;
  if (decision == nullptr) {
    return Fail(error, TypedUpdateCarrierErrorCode::invalid_argument,
                kUpdateFailed, carrier, "decision", 0,
                "output pointer is null");
  }
  std::vector<byte> canonical;
  if (!EncodeTypedUpdateMgaRecoveryObservation(observation, &canonical,
                                               error)) {
    *decision = TypedUpdateRecoveryDecision::quarantine_update_failed;
    return false;
  }
  if (!RequireExactRecoveryBytes(canonical, observation.exact_bytes,
                                 carrier, error)) {
    *decision = TypedUpdateRecoveryDecision::quarantine_update_failed;
    return false;
  }
  if (observation.transaction_state ==
      TypedUpdateTransactionState::quarantined) {
    *decision = TypedUpdateRecoveryDecision::quarantine_update_failed;
    return Fail(error,
                TypedUpdateCarrierErrorCode::recovery_authority_invalid,
                kUpdateFailed, carrier, "transaction_state", 0,
                "quarantined MGA transaction authority cannot recover or replay");
  }
  if (observation.transaction_state !=
      TypedUpdateTransactionState::active_live) {
    if (observation.latest_journal_state ==
            TypedUpdateJournalState::bound ||
        observation.latest_journal_state ==
            TypedUpdateJournalState::intent ||
        observation.latest_journal_state ==
            TypedUpdateJournalState::prepared) {
      *decision = TypedUpdateRecoveryDecision::append_aborted_no_result;
      return true;
    }
    *decision = TypedUpdateRecoveryDecision::stale_replay;
    return Fail(error,
                TypedUpdateCarrierErrorCode::recovery_replay_stale,
                kTransactionStale, carrier, "transaction_state", 0,
                "ended MGA transaction is not active replay authority");
  }
  switch (observation.latest_journal_state) {
    case TypedUpdateJournalState::bound:
    case TypedUpdateJournalState::intent:
      *decision = TypedUpdateRecoveryDecision::append_aborted_no_result;
      return true;
    case TypedUpdateJournalState::prepared:
      if (observation.savepoint_state ==
              TypedUpdateSavepointState::active ||
          observation.savepoint_state ==
              TypedUpdateSavepointState::rolled_back_final) {
        *decision = TypedUpdateRecoveryDecision::append_aborted_no_result;
      } else {
        *decision =
            TypedUpdateRecoveryDecision::append_published_and_replay_result;
      }
      return true;
    case TypedUpdateJournalState::published:
      *decision = TypedUpdateRecoveryDecision::replay_published_result;
      return true;
    case TypedUpdateJournalState::aborted:
      *decision = TypedUpdateRecoveryDecision::stale_replay;
      return Fail(error,
                  TypedUpdateCarrierErrorCode::recovery_replay_stale,
                  kTransactionStale, carrier, "latest_DUJR_state", 0,
                  "aborted durable operation has no replayable result");
  }
  *decision = TypedUpdateRecoveryDecision::quarantine_update_failed;
  return Fail(error,
              TypedUpdateCarrierErrorCode::recovery_observation_invalid,
              kUpdateFailed, carrier, "latest_DUJR_state", 0,
              "unknown DUMO durable journal state");
}

const char* TypedUpdateCarrierErrorCodeName(TypedUpdateCarrierErrorCode code) {
  switch (code) {
    case TypedUpdateCarrierErrorCode::ok: return "ok";
    case TypedUpdateCarrierErrorCode::invalid_argument: return "invalid_argument";
    case TypedUpdateCarrierErrorCode::extent_invalid: return "extent_invalid";
    case TypedUpdateCarrierErrorCode::magic_invalid: return "magic_invalid";
    case TypedUpdateCarrierErrorCode::version_invalid: return "version_invalid";
    case TypedUpdateCarrierErrorCode::flags_invalid: return "flags_invalid";
    case TypedUpdateCarrierErrorCode::reserved_invalid: return "reserved_invalid";
    case TypedUpdateCarrierErrorCode::uuid_invalid: return "uuid_invalid";
    case TypedUpdateCarrierErrorCode::generation_invalid: return "generation_invalid";
    case TypedUpdateCarrierErrorCode::count_invalid: return "count_invalid";
    case TypedUpdateCarrierErrorCode::ordinal_invalid: return "ordinal_invalid";
    case TypedUpdateCarrierErrorCode::duplicate_occurrence: return "duplicate_occurrence";
    case TypedUpdateCarrierErrorCode::duplicate_target: return "duplicate_target";
    case TypedUpdateCarrierErrorCode::codec_id_invalid: return "codec_id_invalid";
    case TypedUpdateCarrierErrorCode::value_state_invalid: return "value_state_invalid";
    case TypedUpdateCarrierErrorCode::canonical_value_invalid: return "canonical_value_invalid";
    case TypedUpdateCarrierErrorCode::canonical_value_hash_mismatch:
      return "canonical_value_hash_mismatch";
    case TypedUpdateCarrierErrorCode::record_evidence_mismatch: return "record_evidence_mismatch";
    case TypedUpdateCarrierErrorCode::vector_evidence_mismatch: return "vector_evidence_mismatch";
    case TypedUpdateCarrierErrorCode::descriptor_evidence_mismatch:
      return "descriptor_evidence_mismatch";
    case TypedUpdateCarrierErrorCode::ownership_mismatch: return "ownership_mismatch";
    case TypedUpdateCarrierErrorCode::predicate_shape_invalid: return "predicate_shape_invalid";
    case TypedUpdateCarrierErrorCode::boolean_identity_invalid: return "boolean_identity_invalid";
    case TypedUpdateCarrierErrorCode::operator_identity_invalid: return "operator_identity_invalid";
    case TypedUpdateCarrierErrorCode::frozen_set_invalid: return "frozen_set_invalid";
    case TypedUpdateCarrierErrorCode::target_order_invalid: return "target_order_invalid";
    case TypedUpdateCarrierErrorCode::resource_limit_exceeded: return "resource_limit_exceeded";
    case TypedUpdateCarrierErrorCode::recovery_identity_invalid: return "recovery_identity_invalid";
    case TypedUpdateCarrierErrorCode::result_invalid: return "result_invalid";
    case TypedUpdateCarrierErrorCode::result_evidence_material_invalid:
      return "result_evidence_material_invalid";
    case TypedUpdateCarrierErrorCode::result_evidence_mismatch: return "result_evidence_mismatch";
    case TypedUpdateCarrierErrorCode::journal_state_invalid: return "journal_state_invalid";
    case TypedUpdateCarrierErrorCode::journal_transition_invalid:
      return "journal_transition_invalid";
    case TypedUpdateCarrierErrorCode::journal_sequence_invalid: return "journal_sequence_invalid";
    case TypedUpdateCarrierErrorCode::journal_chain_mismatch: return "journal_chain_mismatch";
    case TypedUpdateCarrierErrorCode::journal_evidence_mismatch: return "journal_evidence_mismatch";
    case TypedUpdateCarrierErrorCode::carrier_set_mismatch: return "carrier_set_mismatch";
    case TypedUpdateCarrierErrorCode::recovery_authority_invalid:
      return "recovery_authority_invalid";
    case TypedUpdateCarrierErrorCode::recovery_replay_stale: return "recovery_replay_stale";
    case TypedUpdateCarrierErrorCode::security_policy_source_invalid:
      return "security_policy_source_invalid";
    case TypedUpdateCarrierErrorCode::security_policy_source_duplicate:
      return "security_policy_source_duplicate";
    case TypedUpdateCarrierErrorCode::security_snapshot_invalid:
      return "security_snapshot_invalid";
    case TypedUpdateCarrierErrorCode::security_snapshot_evidence_mismatch:
      return "security_snapshot_evidence_mismatch";
    case TypedUpdateCarrierErrorCode::exact_byte_hash_mismatch:
      return "exact_byte_hash_mismatch";
    case TypedUpdateCarrierErrorCode::recovery_observation_invalid:
      return "recovery_observation_invalid";
    case TypedUpdateCarrierErrorCode::replay_identity_mismatch:
      return "replay_identity_mismatch";
    case TypedUpdateCarrierErrorCode::security_recovery_binding_mismatch:
      return "security_recovery_binding_mismatch";
    case TypedUpdateCarrierErrorCode::fixed_text_invalid:
      return "fixed_text_invalid";
    case TypedUpdateCarrierErrorCode::datatype_authority_invalid:
      return "datatype_authority_invalid";
    case TypedUpdateCarrierErrorCode::datatype_authority_duplicate:
      return "datatype_authority_duplicate";
    case TypedUpdateCarrierErrorCode::builtin_operator_authority_invalid:
      return "builtin_operator_authority_invalid";
    case TypedUpdateCarrierErrorCode::datatype_operator_binding_mismatch:
      return "datatype_operator_binding_mismatch";
    case TypedUpdateCarrierErrorCode::hash_failure: return "hash_failure";
  }
  return "unknown";
}

}  // namespace scratchbird::wire
