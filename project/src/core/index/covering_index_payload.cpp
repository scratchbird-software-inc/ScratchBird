// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "covering_index_payload.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

constexpr std::array<byte, 8> kPayloadMagic = {'S', 'B', 'C', 'O',
                                               'V', 'P', '0', '1'};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() && left.kind == right.kind &&
         left.value == right.value;
}

bool ValidObjectUuid(const TypedUuid& value) {
  return value.valid() && value.kind == UuidKind::object;
}

bool ValidRowUuid(const TypedUuid& value) {
  return value.valid() && value.kind == UuidKind::row;
}

std::string UuidText(const TypedUuid& value) {
  if (!value.valid()) {
    return "invalid";
  }
  return scratchbird::core::uuid::UuidToString(value.value);
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u32));
  StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u64));
  StoreLittle64(out->data() + offset, value);
}

void AppendUuid(std::vector<byte>* out, const TypedUuid& value) {
  out->push_back(static_cast<byte>(value.kind));
  out->insert(out->end(), value.value.bytes.begin(), value.value.bytes.end());
}

void AppendBytes(std::vector<byte>* out, const std::vector<byte>& bytes) {
  AppendU64(out, static_cast<u64>(bytes.size()));
  out->insert(out->end(), bytes.begin(), bytes.end());
}

void AppendString(std::vector<byte>* out, const std::string& value) {
  AppendU64(out, static_cast<u64>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(u64 value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = digits[value & 0x0fu];
    value >>= 4u;
  }
  return out;
}

std::string RowShapeHash(const std::vector<byte>& bytes) {
  const u64 hash = bytes.empty() ? 0 : Fnv1a64(bytes.data(), bytes.size());
  return "covering-row-shape:" + Hex64(hash);
}

bool HasUnsafeAuthorityDrift(
    const CoveringIndexPayloadAssemblyRequest& request) {
  return request.parser_or_donor_finality_authority ||
         request.client_finality_authority ||
         request.provider_finality_authority;
}

bool HasUnsafeAuthorityDrift(
    const CoveringIndexPayloadValidationRequest& request) {
  return request.parser_or_donor_finality_authority ||
         request.client_finality_authority ||
         request.provider_finality_authority;
}

const CoveringIndexPayloadColumnRef* FindColumnRef(
    const std::vector<CoveringIndexPayloadColumnRef>& columns,
    const TypedUuid& column_uuid) {
  for (const auto& column : columns) {
    if (SameUuid(column.column_uuid, column_uuid)) {
      return &column;
    }
  }
  return nullptr;
}

const CoveringIndexPayloadColumnValue* FindValue(
    const std::vector<CoveringIndexPayloadColumnValue>& values,
    const TypedUuid& column_uuid) {
  for (const auto& value : values) {
    if (SameUuid(value.column_uuid, column_uuid)) {
      return &value;
    }
  }
  return nullptr;
}

bool ContainsColumn(const std::vector<TypedUuid>& columns,
                    const TypedUuid& column_uuid) {
  return std::any_of(columns.begin(), columns.end(), [&](const TypedUuid& other) {
    return SameUuid(other, column_uuid);
  });
}

bool HasDuplicateColumns(const std::vector<TypedUuid>& columns) {
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (!ValidObjectUuid(columns[i])) {
      return true;
    }
    for (std::size_t j = i + 1; j < columns.size(); ++j) {
      if (SameUuid(columns[i], columns[j])) {
        return true;
      }
    }
  }
  return false;
}

bool HasInvalidOrDuplicateColumnRefs(
    const std::vector<CoveringIndexPayloadColumnRef>& columns) {
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (!ValidObjectUuid(columns[i].column_uuid) ||
        !ValidObjectUuid(columns[i].type_descriptor_uuid)) {
      return true;
    }
    for (std::size_t j = i + 1; j < columns.size(); ++j) {
      if (SameUuid(columns[i].column_uuid, columns[j].column_uuid)) {
        return true;
      }
    }
  }
  return false;
}

bool HasDuplicateValues(
    const std::vector<CoveringIndexPayloadColumnValue>& values) {
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!ValidObjectUuid(values[i].column_uuid)) {
      return true;
    }
    for (std::size_t j = i + 1; j < values.size(); ++j) {
      if (SameUuid(values[i].column_uuid, values[j].column_uuid)) {
        return true;
      }
    }
  }
  return false;
}

bool LargePayloadReferenceValid(
    const CoveringIndexLargePayloadReference& reference) {
  return ValidObjectUuid(reference.payload_uuid) &&
         ValidObjectUuid(reference.owner_object_uuid) &&
         ValidObjectUuid(reference.generation_scope_uuid) &&
         reference.generation != 0 && reference.byte_count != 0 &&
         !reference.descriptor_hash.empty();
}

bool SameLargePayloadReference(
    const CoveringIndexLargePayloadReference& left,
    const CoveringIndexLargePayloadReference& right) {
  return SameUuid(left.payload_uuid, right.payload_uuid) &&
         SameUuid(left.owner_object_uuid, right.owner_object_uuid) &&
         SameUuid(left.generation_scope_uuid, right.generation_scope_uuid) &&
         left.generation == right.generation &&
         left.byte_count == right.byte_count &&
         left.descriptor_hash == right.descriptor_hash;
}

const CoveringIndexExpectedLargePayload* FindExpectedLargePayload(
    const std::vector<CoveringIndexExpectedLargePayload>& expected,
    const TypedUuid& column_uuid) {
  for (const auto& value : expected) {
    if (SameUuid(value.column_uuid, column_uuid)) {
      return &value;
    }
  }
  return nullptr;
}

bool PhysicalLayoutMagicValid(const std::vector<byte>& payload) {
  return payload.size() >= kPayloadMagic.size() &&
         std::equal(kPayloadMagic.begin(), kPayloadMagic.end(),
                    payload.begin());
}

void AppendColumnValue(std::vector<byte>* out,
                       const CoveringIndexPayloadColumnValue& value) {
  AppendUuid(out, value.column_uuid);
  AppendU32(out, value.projection_ordinal);
  AppendU32(out, static_cast<u32>(value.kind));
  AppendU32(out, value.binary_result_frame_compatible ? 1u : 0u);
  AppendU32(out, value.redaction_safe ? 1u : 0u);
  AppendU32(out, value.redacted ? 1u : 0u);
  AppendU32(out, value.protected_value ? 1u : 0u);
  AppendBytes(out, value.encoded_value);
  if (value.kind == CoveringIndexPayloadValueKind::large_payload_reference) {
    AppendUuid(out, value.large_payload.payload_uuid);
    AppendUuid(out, value.large_payload.owner_object_uuid);
    AppendUuid(out, value.large_payload.generation_scope_uuid);
    AppendU64(out, value.large_payload.generation);
    AppendU64(out, value.large_payload.byte_count);
    AppendString(out, value.large_payload.descriptor_hash);
  }
}

std::vector<byte> BuildPhysicalPayload(
    const CoveringIndexPayloadAssemblyRequest& request) {
  std::vector<byte> payload;
  payload.insert(payload.end(), kPayloadMagic.begin(), kPayloadMagic.end());
  AppendU32(&payload, kCoveringIndexPayloadLayoutVersion);
  AppendUuid(&payload, request.index_uuid);
  AppendUuid(&payload, request.table_uuid);
  AppendUuid(&payload, request.row_uuid);
  AppendUuid(&payload, request.version_uuid);
  AppendString(&payload, request.descriptor_result_contract_hash);
  AppendU64(&payload, request.payload_generation);
  AppendU64(&payload, request.redaction_policy_epoch);
  AppendU64(&payload, request.security_policy_epoch);
  AppendU64(&payload, request.freshness_generation);
  AppendU64(&payload, static_cast<u64>(request.values.size()));
  for (const auto& value : request.values) {
    AppendColumnValue(&payload, value);
  }
  return payload;
}

std::vector<byte> BuildPhysicalPayloadFromRecord(
    const CoveringIndexPayloadRecord& record) {
  std::vector<byte> payload;
  payload.insert(payload.end(), kPayloadMagic.begin(), kPayloadMagic.end());
  AppendU32(&payload, kCoveringIndexPayloadLayoutVersion);
  AppendUuid(&payload, record.index_uuid);
  AppendUuid(&payload, record.table_uuid);
  AppendUuid(&payload, record.row_uuid);
  AppendUuid(&payload, record.version_uuid);
  AppendString(&payload, record.descriptor_result_contract_hash);
  AppendU64(&payload, record.payload_generation);
  AppendU64(&payload, record.redaction_policy_epoch);
  AppendU64(&payload, record.security_policy_epoch);
  AppendU64(&payload, record.freshness_generation);
  AppendU64(&payload, static_cast<u64>(record.values.size()));
  for (const auto& value : record.values) {
    AppendColumnValue(&payload, value);
  }
  return payload;
}

CoveringIndexPayloadAssemblyResult RefuseAssembly(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  CoveringIndexPayloadAssemblyResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.refusal_reasons.push_back(diagnostic_code);
  result.evidence.push_back("covering_payload.fail_closed=true");
  result.evidence.push_back("covering_payload.refused=" + diagnostic_code);
  result.evidence.push_back("covering_payload.visibility_authority=false");
  result.evidence.push_back("covering_payload.transaction_finality_authority=false");
  result.diagnostic = MakeCoveringIndexPayloadDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

CoveringIndexPayloadAdmission RefuseAdmission(
    const CoveringIndexPayloadValidationRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    std::string blocker) {
  CoveringIndexPayloadAdmission result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.admission_kind = CoveringIndexPayloadAdmissionKind::refused;
  result.record = request.record;
  result.blockers.push_back(std::move(blocker));
  result.evidence.push_back("covering_payload.fail_closed=true");
  result.evidence.push_back("covering_payload.refused=" + diagnostic_code);
  result.evidence.push_back("covering_payload.visibility_authority=false");
  result.evidence.push_back("covering_payload.authorization_authority=false");
  result.evidence.push_back("covering_payload.transaction_finality_authority=false");
  result.evidence.push_back("covering_payload.cleanup_authority=false");
  result.evidence.push_back("covering_payload.recovery_authority=false");
  result.diagnostic = MakeCoveringIndexPayloadDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

CoveringIndexPayloadAssemblyResult ValidateAssemblyRequest(
    const CoveringIndexPayloadAssemblyRequest& request) {
  if (!ValidObjectUuid(request.index_uuid) ||
      !ValidObjectUuid(request.table_uuid) || !ValidRowUuid(request.row_uuid) ||
      !ValidRowUuid(request.version_uuid)) {
    return RefuseAssembly("SB_COVERING_PAYLOAD.IDENTITY_INVALID",
                          "core.index.covering_payload.identity_invalid",
                          "index table row and version UUIDs are required");
  }
  if (HasUnsafeAuthorityDrift(request)) {
    return RefuseAssembly("SB_COVERING_PAYLOAD.UNSAFE_AUTHORITY",
                          "core.index.covering_payload.unsafe_authority",
                          "parser client provider or donor finality authority is forbidden");
  }
  if (!request.projection_only) {
    return RefuseAssembly("SB_COVERING_PAYLOAD.PROJECTION_ONLY_REQUIRED",
                          "core.index.covering_payload.projection_only_required",
                          "covering payload may contain projected column values only");
  }
  if (!request.result_contract_bound ||
      request.descriptor_result_contract_hash.empty()) {
    return RefuseAssembly("SB_COVERING_PAYLOAD.RESULT_CONTRACT_REQUIRED",
                          "core.index.covering_payload.result_contract_required",
                          "descriptor/result-contract hash is required");
  }
  if (request.payload_generation == 0 ||
      request.redaction_policy_epoch == 0 ||
      request.security_policy_epoch == 0 ||
      request.freshness_generation == 0) {
    return RefuseAssembly("SB_COVERING_PAYLOAD.EPOCH_REQUIRED",
                          "core.index.covering_payload.epoch_required",
                          "payload redaction security and freshness generations are required");
  }
  if (request.projected_column_uuids.empty() ||
      HasDuplicateColumns(request.projected_column_uuids) ||
      HasInvalidOrDuplicateColumnRefs(request.descriptor_columns) ||
      HasDuplicateValues(request.values)) {
    return RefuseAssembly("SB_COVERING_PAYLOAD.COLUMN_SET_INVALID",
                          "core.index.covering_payload.column_set_invalid",
                          "projected payload columns must be valid and unique");
  }

  for (const auto& projected : request.projected_column_uuids) {
    if (FindColumnRef(request.descriptor_columns, projected) == nullptr) {
      return RefuseAssembly("SB_COVERING_PAYLOAD.UNKNOWN_COLUMN",
                            "core.index.covering_payload.unknown_column",
                            UuidText(projected));
    }
    if (FindValue(request.values, projected) == nullptr) {
      return RefuseAssembly("SB_COVERING_PAYLOAD.MISSING_COLUMN",
                            "core.index.covering_payload.missing_column",
                            UuidText(projected));
    }
  }

  for (const auto& value : request.values) {
    if (!ContainsColumn(request.projected_column_uuids, value.column_uuid)) {
      return RefuseAssembly("SB_COVERING_PAYLOAD.UNKNOWN_COLUMN",
                            "core.index.covering_payload.unknown_column",
                            UuidText(value.column_uuid));
    }
    if (value.kind == CoveringIndexPayloadValueKind::inline_value &&
        value.encoded_value.empty()) {
      return RefuseAssembly("SB_COVERING_PAYLOAD.INLINE_VALUE_EMPTY",
                            "core.index.covering_payload.inline_value_empty",
                            UuidText(value.column_uuid));
    }
    if (value.kind == CoveringIndexPayloadValueKind::large_payload_reference &&
        !LargePayloadReferenceValid(value.large_payload)) {
      return RefuseAssembly("SB_COVERING_PAYLOAD.LARGE_DESCRIPTOR_INVALID",
                            "core.index.covering_payload.large_descriptor_invalid",
                            UuidText(value.column_uuid));
    }
  }
  CoveringIndexPayloadAssemblyResult ok;
  ok.status = OkStatus();
  ok.assembled = true;
  return ok;
}

bool RequiredColumnSetsValid(
    const CoveringIndexPayloadValidationRequest& request,
    std::string* diagnostic_detail,
    std::string* diagnostic_code,
    std::string* message_key,
    std::string* blocker) {
  if (request.projected_column_uuids.empty() ||
      HasDuplicateColumns(request.projected_column_uuids) ||
      HasDuplicateValues(request.record.values)) {
    *diagnostic_code = "SB_COVERING_PAYLOAD.COLUMN_SET_INVALID";
    *message_key = "core.index.covering_payload.column_set_invalid";
    *diagnostic_detail = "projected payload columns must be valid and unique";
    *blocker = "covering_payload_column_set_invalid";
    return false;
  }
  for (const auto& projected : request.projected_column_uuids) {
    if (FindColumnRef(request.required_columns, projected) == nullptr) {
      *diagnostic_code = "SB_COVERING_PAYLOAD.UNKNOWN_COLUMN";
      *message_key = "core.index.covering_payload.unknown_column";
      *diagnostic_detail = UuidText(projected);
      *blocker = "covering_payload_unknown_column";
      return false;
    }
    if (FindValue(request.record.values, projected) == nullptr) {
      *diagnostic_code = "SB_COVERING_PAYLOAD.MISSING_COLUMN";
      *message_key = "core.index.covering_payload.missing_column";
      *diagnostic_detail = UuidText(projected);
      *blocker = "covering_payload_missing_column";
      return false;
    }
  }
  for (const auto& value : request.record.values) {
    if (!ContainsColumn(request.projected_column_uuids, value.column_uuid)) {
      *diagnostic_code = "SB_COVERING_PAYLOAD.UNKNOWN_COLUMN";
      *message_key = "core.index.covering_payload.unknown_column";
      *diagnostic_detail = UuidText(value.column_uuid);
      *blocker = "covering_payload_unknown_column";
      return false;
    }
  }
  return true;
}

bool ColumnValuesSafe(const CoveringIndexPayloadValidationRequest& request,
                      std::string* diagnostic_detail,
                      std::string* diagnostic_code,
                      std::string* message_key,
                      std::string* blocker) {
  for (const auto& value : request.record.values) {
    if (!value.redaction_safe || !request.redaction_policy_safe) {
      *diagnostic_code = "SB_COVERING_PAYLOAD.REDACTION_UNSAFE";
      *message_key = "core.index.covering_payload.redaction_unsafe";
      *diagnostic_detail = UuidText(value.column_uuid);
      *blocker = "covering_payload_redaction_policy_unsafe";
      return false;
    }
    if (value.protected_value && !value.redacted &&
        !value.unredacted_authorized) {
      *diagnostic_code = "SB_COVERING_PAYLOAD.REDACTION_UNSAFE";
      *message_key = "core.index.covering_payload.redaction_unsafe";
      *diagnostic_detail = "protected unredacted payload lacks authorization";
      *blocker = "covering_payload_redaction_policy_unsafe";
      return false;
    }
    if (value.kind == CoveringIndexPayloadValueKind::large_payload_reference) {
      if (!LargePayloadReferenceValid(value.large_payload)) {
        *diagnostic_code = "SB_COVERING_PAYLOAD.LARGE_DESCRIPTOR_INVALID";
        *message_key = "core.index.covering_payload.large_descriptor_invalid";
        *diagnostic_detail = UuidText(value.column_uuid);
        *blocker = "covering_payload_large_descriptor_invalid";
        return false;
      }
      const auto* expected =
          FindExpectedLargePayload(request.expected_large_payloads,
                                   value.column_uuid);
      if (expected != nullptr &&
          !SameLargePayloadReference(expected->descriptor,
                                     value.large_payload)) {
        *diagnostic_code = "SB_COVERING_PAYLOAD.LARGE_DESCRIPTOR_MISMATCH";
        *message_key = "core.index.covering_payload.large_descriptor_mismatch";
        *diagnostic_detail = UuidText(value.column_uuid);
        *blocker = "covering_payload_large_descriptor_mismatch";
        return false;
      }
    }
  }
  return true;
}

}  // namespace

const char* CoveringIndexPayloadValueKindName(
    CoveringIndexPayloadValueKind kind) {
  switch (kind) {
    case CoveringIndexPayloadValueKind::null_value: return "null_value";
    case CoveringIndexPayloadValueKind::inline_value: return "inline_value";
    case CoveringIndexPayloadValueKind::large_payload_reference:
      return "large_payload_reference";
  }
  return "unknown";
}

const char* CoveringIndexPayloadAdmissionKindName(
    CoveringIndexPayloadAdmissionKind kind) {
  switch (kind) {
    case CoveringIndexPayloadAdmissionKind::refused: return "refused";
    case CoveringIndexPayloadAdmissionKind::base_row_recheck:
      return "base_row_recheck";
    case CoveringIndexPayloadAdmissionKind::index_only: return "index_only";
  }
  return "unknown";
}

CoveringIndexPayloadAssemblyResult AssembleCoveringIndexPayload(
    const CoveringIndexPayloadAssemblyRequest& request) {
  auto validation = ValidateAssemblyRequest(request);
  if (!validation.ok()) {
    return validation;
  }

  CoveringIndexPayloadRecord record;
  record.index_uuid = request.index_uuid;
  record.table_uuid = request.table_uuid;
  record.row_uuid = request.row_uuid;
  record.version_uuid = request.version_uuid;
  record.descriptor_result_contract_hash =
      request.descriptor_result_contract_hash;
  record.payload_generation = request.payload_generation;
  record.redaction_policy_epoch = request.redaction_policy_epoch;
  record.security_policy_epoch = request.security_policy_epoch;
  record.freshness_generation = request.freshness_generation;
  record.values = request.values;
  record.projection_only = true;
  record.binary_result_frame_compatible =
      std::all_of(record.values.begin(), record.values.end(), [](const auto& value) {
        return value.binary_result_frame_compatible;
      });
  record.physical_payload = BuildPhysicalPayload(request);
  record.row_shape_hash = RowShapeHash(record.physical_payload);

  CoveringIndexPayloadAssemblyResult result;
  result.status = OkStatus();
  result.assembled = true;
  result.record = std::move(record);
  result.evidence.push_back("covering_payload.physical_layout_version=1");
  result.evidence.push_back("covering_payload.projection_only=true");
  result.evidence.push_back("covering_payload.column_count=" +
                            std::to_string(request.values.size()));
  result.evidence.push_back("covering_payload.row_uuid=" +
                            UuidText(request.row_uuid));
  result.evidence.push_back("covering_payload.version_uuid=" +
                            UuidText(request.version_uuid));
  result.evidence.push_back("covering_payload.descriptor_result_contract_hash=" +
                            request.descriptor_result_contract_hash);
  result.evidence.push_back("covering_payload.row_shape_hash=" +
                            result.record.row_shape_hash);
  result.evidence.push_back("covering_payload.binary_result_frame_compatible=" +
                            std::string(result.record.binary_result_frame_compatible
                                            ? "true"
                                            : "false"));
  result.evidence.push_back("covering_payload.visibility_authority=false");
  result.evidence.push_back("covering_payload.authorization_authority=false");
  result.evidence.push_back("covering_payload.transaction_finality_authority=false");
  result.evidence.push_back("covering_payload.cleanup_authority=false");
  result.evidence.push_back("covering_payload.recovery_authority=false");
  result.diagnostic = MakeCoveringIndexPayloadDiagnostic(
      result.status, "ok", "core.index.covering_payload.assembled",
      "covering payload assembled");
  return result;
}

CoveringIndexPayloadAdmission ValidateCoveringIndexPayloadForLocator(
    const CoveringIndexPayloadValidationRequest& request) {
  if (HasUnsafeAuthorityDrift(request)) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.UNSAFE_AUTHORITY",
        "core.index.covering_payload.unsafe_authority",
        "parser client provider or donor finality authority is forbidden",
        "covering_payload_unsafe_authority");
  }
  if (!request.locator.physical_btree_locator_scan ||
      !ValidRowUuid(request.locator.row_uuid) ||
      !ValidRowUuid(request.locator.version_uuid)) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.LOCATOR_REQUIRED",
        "core.index.covering_payload.locator_required",
        "physical B-tree row/version locator evidence is required",
        "covering_payload_locator_missing");
  }
  if (!SameUuid(request.locator.row_uuid, request.record.row_uuid) ||
      !SameUuid(request.locator.version_uuid, request.record.version_uuid)) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.ROW_VERSION_MISMATCH",
        "core.index.covering_payload.row_version_mismatch",
        "covering payload row/version UUID does not match locator",
        "covering_payload_row_version_mismatch");
  }
  if (!PhysicalLayoutMagicValid(request.record.physical_payload)) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.PHYSICAL_LAYOUT_INVALID",
        "core.index.covering_payload.physical_layout_invalid",
        "covering payload physical layout magic is invalid",
        "covering_payload_physical_layout_invalid");
  }
  if (!request.descriptor_epoch_current ||
      !request.result_contract_current ||
      !request.redaction_epoch_current ||
      !request.security_epoch_current ||
      !request.freshness_current) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.STALE_EPOCH",
        "core.index.covering_payload.stale_epoch",
        "descriptor result redaction security and freshness epochs must be current",
        "covering_payload_epoch_stale");
  }
  if (request.expected_descriptor_result_contract_hash.empty() ||
      request.expected_payload_generation == 0 ||
      request.expected_redaction_policy_epoch == 0 ||
      request.expected_security_policy_epoch == 0 ||
      request.expected_freshness_generation == 0) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.EXPECTED_PROOF_REQUIRED",
        "core.index.covering_payload.expected_proof_required",
        "expected descriptor contract and generation proofs are required",
        "covering_payload_expected_proof_missing");
  }
  if (!request.expected_descriptor_result_contract_hash.empty() &&
      request.record.descriptor_result_contract_hash !=
          request.expected_descriptor_result_contract_hash) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.RESULT_CONTRACT_MISMATCH",
        "core.index.covering_payload.result_contract_mismatch",
        "descriptor/result-contract hash mismatch",
        "covering_payload_result_contract_mismatch");
  }
  if (request.expected_payload_generation != 0 &&
      request.record.payload_generation != request.expected_payload_generation) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.PAYLOAD_GENERATION_MISMATCH",
        "core.index.covering_payload.payload_generation_mismatch",
        "payload generation mismatch",
        "covering_payload_generation_mismatch");
  }
  if ((request.expected_redaction_policy_epoch != 0 &&
       request.record.redaction_policy_epoch !=
           request.expected_redaction_policy_epoch) ||
      (request.expected_security_policy_epoch != 0 &&
       request.record.security_policy_epoch !=
           request.expected_security_policy_epoch) ||
      (request.expected_freshness_generation != 0 &&
       request.record.freshness_generation !=
           request.expected_freshness_generation)) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.EPOCH_MISMATCH",
        "core.index.covering_payload.epoch_mismatch",
        "redaction security or freshness generation mismatch",
        "covering_payload_epoch_mismatch");
  }

  std::string detail;
  std::string code;
  std::string key;
  std::string blocker;
  if (!RequiredColumnSetsValid(request, &detail, &code, &key, &blocker)) {
    return RefuseAdmission(request, std::move(code), std::move(key),
                           std::move(detail), std::move(blocker));
  }
  if (!ColumnValuesSafe(request, &detail, &code, &key, &blocker)) {
    return RefuseAdmission(request, std::move(code), std::move(key),
                           std::move(detail), std::move(blocker));
  }
  const auto expected_physical_payload =
      BuildPhysicalPayloadFromRecord(request.record);
  if (request.record.physical_payload != expected_physical_payload ||
      request.record.row_shape_hash !=
          RowShapeHash(request.record.physical_payload)) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.PHYSICAL_LAYOUT_MISMATCH",
        "core.index.covering_payload.physical_layout_mismatch",
        "covering payload physical bytes do not match the record envelope",
        "covering_payload_physical_layout_mismatch");
  }
  if ((!request.exact_predicate_recheck_planned &&
       !request.exact_predicate_rechecked_by_engine) ||
      (!request.mga_visibility_recheck_planned &&
       !request.mga_visibility_rechecked_by_engine) ||
      (!request.security_authorization_recheck_planned &&
       !request.security_authorized_by_engine)) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.RECHECK_REQUIRED",
        "core.index.covering_payload.recheck_required",
        "exact predicate MGA and security recheck evidence is required",
        "covering_payload_mga_security_recheck_missing");
  }

  const bool required_rechecks_proven =
      request.exact_predicate_rechecked_by_engine &&
      request.mga_visibility_rechecked_by_engine &&
      request.security_authorized_by_engine;
  const bool can_index_only =
      request.allow_index_only && required_rechecks_proven &&
      request.result_frame_contract_proven && request.redaction_policy_safe &&
      request.result_contract_current && request.redaction_epoch_current &&
      request.security_epoch_current && request.freshness_current &&
      request.record.projection_only &&
      request.record.binary_result_frame_compatible;

  CoveringIndexPayloadAdmission result;
  result.status = OkStatus();
  result.admitted = true;
  result.record = request.record;
  result.payload_projection_only = request.record.projection_only;
  result.result_frame_compatible =
      request.record.binary_result_frame_compatible;
  result.evidence.push_back("covering_payload.physical_layout_valid=true");
  result.evidence.push_back("covering_payload.locator_bound=true");
  result.evidence.push_back("covering_payload.row_uuid=" +
                            UuidText(request.record.row_uuid));
  result.evidence.push_back("covering_payload.version_uuid=" +
                            UuidText(request.record.version_uuid));
  result.evidence.push_back("covering_payload.descriptor_result_contract_current=true");
  result.evidence.push_back("covering_payload.payload_generation_current=true");
  result.evidence.push_back("covering_payload.redaction_policy_epoch_current=true");
  result.evidence.push_back("covering_payload.security_policy_epoch_current=true");
  result.evidence.push_back("covering_payload.freshness_generation_current=true");
  result.evidence.push_back("covering_payload.result_frame_compatible=" +
                            std::string(result.result_frame_compatible ? "true"
                                                                       : "false"));
  result.evidence.push_back("covering_payload.row_shape_hash=" +
                            request.record.row_shape_hash);
  result.evidence.push_back("covering_payload.visibility_authority=false");
  result.evidence.push_back("covering_payload.authorization_authority=false");
  result.evidence.push_back("covering_payload.transaction_finality_authority=false");
  result.evidence.push_back("covering_payload.cleanup_authority=false");
  result.evidence.push_back("covering_payload.recovery_authority=false");

  if (can_index_only) {
    result.admission_kind = CoveringIndexPayloadAdmissionKind::index_only;
    result.index_only_admitted = true;
    result.base_row_recheck_required = false;
    result.base_row_recheck_handoff_proven = false;
    result.evidence.push_back("covering_payload.index_only_admitted=true");
    result.evidence.push_back("covering_payload.required_rechecks_proven=true");
    result.diagnostic = MakeCoveringIndexPayloadDiagnostic(
        result.status, "ok", "core.index.covering_payload.index_only_admitted",
        "covering payload admitted for index-only projection");
    return result;
  }

  if (!request.base_row_recheck_available ||
      !request.exact_predicate_recheck_planned ||
      !request.mga_visibility_recheck_planned ||
      !request.security_authorization_recheck_planned) {
    return RefuseAdmission(
        request, "SB_COVERING_PAYLOAD.BASE_ROW_RECHECK_REQUIRED",
        "core.index.covering_payload.base_row_recheck_required",
        "base-row recheck handoff is required for non-index-only covering use",
        "covering_payload_base_row_recheck_missing");
  }

  result.admission_kind = CoveringIndexPayloadAdmissionKind::base_row_recheck;
  result.index_only_admitted = false;
  result.base_row_recheck_required = true;
  result.base_row_recheck_handoff_proven = true;
  result.evidence.push_back("covering_payload.index_only_admitted=false");
  result.evidence.push_back("covering_payload.base_row_recheck_handoff=true");
  result.evidence.push_back("covering_payload.mga_recheck_planned=true");
  result.evidence.push_back("covering_payload.security_recheck_planned=true");
  result.diagnostic = MakeCoveringIndexPayloadDiagnostic(
      result.status, "ok", "core.index.covering_payload.base_row_recheck",
      "covering payload admitted with base-row recheck handoff");
  return result;
}

DiagnosticRecord MakeCoveringIndexPayloadDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.covering_payload",
                        status.ok() ? "" : "fail closed before using covering payload");
}

}  // namespace scratchbird::core::index
