// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "row_page_compact_encoding.hpp"

#include "runtime_platform.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace scratchbird::core::index {
namespace {

namespace page = scratchbird::storage::page;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr byte kMagic[8] = {'S', 'B', 'R', 'P', 'C', '0', '0', '1'};
inline constexpr u16 kFormatVersion = 1;
inline constexpr u32 kEnvelopeHeaderBytes = 48;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 ChecksumWithoutTrailer(const std::vector<byte>& bytes) {
  if (bytes.size() < sizeof(u64)) {
    return 0;
  }
  return Fnv1a64(bytes.data(), bytes.size() - sizeof(u64));
}

void AppendPolicyEvidence(std::vector<std::string>* evidence,
                          const CompressionPolicyDecision& decision) {
  evidence->push_back("row_page_compact.policy_accepted=" +
                      std::string(decision.accepted ? "true" : "false"));
  evidence->push_back("row_page_compact.policy_fallback=" +
                      std::string(decision.fallback ? "true" : "false"));
  evidence->push_back("row_page_compact.policy_family=" +
                      std::string(CompressionFamilyName(decision.family)));
  evidence->push_back("row_page_compact.policy_method=" +
                      std::string(CompressionMethodName(decision.method)));
  evidence->push_back("row_page_compact.policy_costed_decision=true");
  for (const auto& item : decision.diagnostics) {
    evidence->push_back("row_page_compact.policy_diagnostic=" + item);
  }
}

std::vector<std::string> BaseEvidence(RowPageCompactEncodingKind kind) {
  return {
      "row_page_compact.encoding=" +
          std::string(RowPageCompactEncodingKindName(kind)),
      "row_page_compact.storage_cpu_only=true",
      "row_page_compact.exact_uncompressed_fallback_available=true",
      "row_page_compact.binary_semantic_equivalence_required=true",
      "row_page_compact.repair_backup_restore_equivalence_required=true",
      "row_page_compact.mga_visibility_recheck_required=true",
      "row_page_compact.security_recheck_required=true",
      "row_page_compact.visibility_authority=false",
      "row_page_compact.transaction_finality_authority=false",
      "row_page_compact.recovery_authority=false",
      "row_page_compact.authorization_authority=false",
      "row_page_compact.parser_client_or_donor_authority=false",
  };
}

bool ValidateAuthority(const RowPageCompactAuthorityContext& authority) {
  return authority.authoritative_row_page_source_proven &&
         authority.binary_semantic_equivalence_required &&
         authority.repair_backup_restore_equivalence_required &&
         authority.durable_mga_inventory_authority_available &&
         authority.normal_mga_visibility_authority_available &&
         authority.security_recheck_required &&
         !authority.parser_client_or_donor_authority &&
         !authority.compact_form_visibility_authority &&
         !authority.compact_form_finality_authority &&
         !authority.compact_form_recovery_authority;
}

RowPageCompactResult Refuse(std::string code,
                            std::string key,
                            std::string reason) {
  RowPageCompactResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.repair_state = RowPageCompactRepairState::refused;
  result.refusal_reasons.push_back(std::move(reason));
  result.diagnostic = MakeRowPageCompactEncodingDiagnostic(
      result.status, std::move(code), std::move(key),
      result.refusal_reasons.back());
  result.evidence = BaseEvidence(result.encoding);
  result.evidence.push_back("row_page_compact.fail_closed=true");
  result.evidence.push_back("row_page_compact.refusal_reason=" +
                            result.refusal_reasons.back());
  return result;
}

bool BodiesEqual(const page::RowDataPageBody& left,
                 const page::RowDataPageBody& right) {
  if (left.relation_uuid.kind != right.relation_uuid.kind ||
      left.relation_uuid.value != right.relation_uuid.value ||
      left.segment_id != right.segment_id ||
      left.segment_generation != right.segment_generation ||
      left.page_number != right.page_number ||
      left.page_generation != right.page_generation ||
      left.next_page_number != right.next_page_number ||
      left.rows.size() != right.rows.size()) {
    return false;
  }
  for (std::size_t row_index = 0; row_index < left.rows.size(); ++row_index) {
    const auto& left_row = left.rows[row_index];
    const auto& right_row = right.rows[row_index];
    if (left_row.row_uuid.kind != right_row.row_uuid.kind ||
        left_row.row_uuid.value != right_row.row_uuid.value ||
        left_row.transaction_uuid.kind != right_row.transaction_uuid.kind ||
        left_row.transaction_uuid.value != right_row.transaction_uuid.value ||
        left_row.local_transaction_id != right_row.local_transaction_id ||
        left_row.internal_row_ordinal != right_row.internal_row_ordinal ||
        left_row.row_version != right_row.row_version ||
        left_row.deleted != right_row.deleted ||
        left_row.cells.size() != right_row.cells.size()) {
      return false;
    }
    for (std::size_t cell_index = 0; cell_index < left_row.cells.size();
         ++cell_index) {
      const auto& left_cell = left_row.cells[cell_index];
      const auto& right_cell = right_row.cells[cell_index];
      if (left_cell.column_ordinal != right_cell.column_ordinal ||
          left_cell.value.type_id != right_cell.value.type_id ||
          left_cell.value.is_null != right_cell.value.is_null ||
          left_cell.value.payload_is_toast_reference !=
              right_cell.value.payload_is_toast_reference ||
          left_cell.value.payload != right_cell.value.payload) {
        return false;
      }
    }
  }
  return true;
}

std::size_t TrimmedSize(const std::vector<byte>& bytes) {
  std::size_t size = bytes.size();
  while (size > 0 && bytes[size - 1] == 0) {
    --size;
  }
  return size;
}

std::vector<byte> SerializeEnvelope(RowPageCompactEncodingKind kind,
                                    u32 page_body_size,
                                    u64 page_number,
                                    u64 canonical_checksum,
                                    const std::vector<byte>& payload) {
  std::vector<byte> out(kEnvelopeHeaderBytes + payload.size() + sizeof(u64),
                        0);
  std::memcpy(out.data(), kMagic, sizeof(kMagic));
  StoreLittle16(out.data() + 8, kFormatVersion);
  StoreLittle16(out.data() + 10, static_cast<u16>(kind));
  StoreLittle32(out.data() + 12, page_body_size);
  StoreLittle64(out.data() + 16, page_number);
  StoreLittle64(out.data() + 24, canonical_checksum);
  StoreLittle64(out.data() + 32, static_cast<u64>(payload.size()));
  StoreLittle64(out.data() + 40, 0);
  std::copy(payload.begin(), payload.end(), out.begin() + kEnvelopeHeaderBytes);
  StoreLittle64(out.data() + out.size() - sizeof(u64),
                ChecksumWithoutTrailer(out));
  return out;
}

RowPageCompactResult DecodeInternal(
    const std::vector<byte>& serialized,
    const RowPageCompactAuthorityContext& authority) {
  if (!ValidateAuthority(authority)) {
    return Refuse("ROW_PAGE_COMPACT.UNSAFE_AUTHORITY",
                  "row_page.compact.unsafe_authority",
                  "row_page_compact_authority_context_invalid");
  }
  if (serialized.size() < kEnvelopeHeaderBytes + sizeof(u64) ||
      std::memcmp(serialized.data(), kMagic, sizeof(kMagic)) != 0) {
    return Refuse("ROW_PAGE_COMPACT.BAD_MAGIC",
                  "row_page.compact.bad_magic",
                  "row_page_compact_bad_magic");
  }
  if (LoadLittle64(serialized.data() + serialized.size() - sizeof(u64)) !=
      ChecksumWithoutTrailer(serialized)) {
    return Refuse("ROW_PAGE_COMPACT.CHECKSUM_MISMATCH",
                  "row_page.compact.checksum_mismatch",
                  "row_page_compact_checksum_mismatch");
  }
  const u16 version = LoadLittle16(serialized.data() + 8);
  const auto kind =
      static_cast<RowPageCompactEncodingKind>(LoadLittle16(serialized.data() + 10));
  const u32 page_body_size = LoadLittle32(serialized.data() + 12);
  const u64 page_number = LoadLittle64(serialized.data() + 16);
  const u64 canonical_checksum = LoadLittle64(serialized.data() + 24);
  const u64 payload_size = LoadLittle64(serialized.data() + 32);
  if (version != kFormatVersion ||
      (kind != RowPageCompactEncodingKind::row_page_uncompressed &&
       kind != RowPageCompactEncodingKind::row_page_trimmed_zero_tail) ||
      page_body_size == 0 ||
      payload_size > std::numeric_limits<u32>::max() ||
      kEnvelopeHeaderBytes + payload_size + sizeof(u64) != serialized.size()) {
    return Refuse("ROW_PAGE_COMPACT.HEADER_INVALID",
                  "row_page.compact.header_invalid",
                  "row_page_compact_header_invalid");
  }
  if (kind == RowPageCompactEncodingKind::row_page_uncompressed &&
      payload_size != page_body_size) {
    return Refuse("ROW_PAGE_COMPACT.PAYLOAD_SIZE_INVALID",
                  "row_page.compact.payload_size_invalid",
                  "row_page_uncompressed_payload_size_invalid");
  }
  if (kind == RowPageCompactEncodingKind::row_page_trimmed_zero_tail &&
      payload_size > page_body_size) {
    return Refuse("ROW_PAGE_COMPACT.PAYLOAD_SIZE_INVALID",
                  "row_page.compact.payload_size_invalid",
                  "row_page_compact_payload_size_invalid");
  }

  std::vector<byte> canonical(page_body_size, 0);
  std::copy(serialized.begin() + kEnvelopeHeaderBytes,
            serialized.begin() + kEnvelopeHeaderBytes + payload_size,
            canonical.begin());
  if (Fnv1a64(canonical.data(), canonical.size()) != canonical_checksum) {
    return Refuse("ROW_PAGE_COMPACT.CANONICAL_MISMATCH",
                  "row_page.compact.canonical_mismatch",
                  "row_page_canonical_checksum_mismatch");
  }
  auto parsed = page::ParseRowDataPageBody(canonical, page_number);
  if (!parsed.ok()) {
    return Refuse("ROW_PAGE_COMPACT.ROW_PAGE_PARSE_REFUSED",
                  "row_page.compact.row_page_parse_refused",
                  parsed.diagnostic.diagnostic_code);
  }

  RowPageCompactResult result;
  result.status = OkStatus();
  result.compressed = kind == RowPageCompactEncodingKind::row_page_trimmed_zero_tail;
  result.fallback_uncompressed =
      kind == RowPageCompactEncodingKind::row_page_uncompressed;
  result.exact_round_trip = true;
  result.backup_restore_equivalent = true;
  result.repair_state = RowPageCompactRepairState::validated;
  result.encoding = kind;
  result.body = std::move(parsed.body);
  result.serialized = serialized;
  result.canonical_row_page = std::move(canonical);
  result.diagnostic = MakeRowPageCompactEncodingDiagnostic(
      result.status, "ROW_PAGE_COMPACT.OK", "row_page.compact.ok",
      RowPageCompactEncodingKindName(kind));
  result.evidence = BaseEvidence(kind);
  result.evidence.push_back("row_page_compact.round_trip=true");
  result.evidence.push_back("row_page_compact.backup_restore_equivalent=true");
  result.evidence.push_back("row_page_compact.row_identity_preserved=true");
  result.evidence.push_back("row_page_compact.page_body_size=" +
                            std::to_string(page_body_size));
  result.evidence.push_back("row_page_compact.payload_size=" +
                            std::to_string(payload_size));
  return result;
}

}  // namespace

const char* RowPageCompactEncodingKindName(RowPageCompactEncodingKind kind) {
  switch (kind) {
    case RowPageCompactEncodingKind::row_page_uncompressed:
      return "row_page_uncompressed";
    case RowPageCompactEncodingKind::row_page_trimmed_zero_tail:
      return "row_page_trimmed_zero_tail";
  }
  return "unknown";
}

const char* RowPageCompactRepairStateName(RowPageCompactRepairState state) {
  switch (state) {
    case RowPageCompactRepairState::validated:
      return "validated";
    case RowPageCompactRepairState::repaired_from_authoritative_row_page:
      return "repaired_from_authoritative_row_page";
    case RowPageCompactRepairState::refused:
      return "refused";
  }
  return "unknown";
}

RowPageCompactResult BuildRowPageCompactEncoding(
    const RowPageCompactRequest& request) {
  if (!ValidateAuthority(request.authority)) {
    return Refuse("ROW_PAGE_COMPACT.UNSAFE_AUTHORITY",
                  "row_page.compact.unsafe_authority",
                  "row_page_compact_authority_context_invalid");
  }
  auto built = page::BuildRowDataPageBody(request.body, request.page_size);
  if (!built.ok()) {
    return Refuse("ROW_PAGE_COMPACT.ROW_PAGE_BUILD_REFUSED",
                  "row_page.compact.row_page_build_refused",
                  built.diagnostic.diagnostic_code);
  }
  const std::size_t trimmed = TrimmedSize(built.serialized);
  const std::vector<byte> compact_payload(built.serialized.begin(),
                                          built.serialized.begin() + trimmed);
  const u64 canonical_checksum =
      Fnv1a64(built.serialized.data(), built.serialized.size());

  auto policy = request.policy;
  policy.family = CompressionFamily::kRowPage;
  policy.exact_uncompressed_fallback_available = true;
  policy.exact_semantic_equivalence_proven = true;
  policy.exact_binary_equivalence_proven = true;
  policy.parser_or_donor_authority = false;
  policy.wal_or_finality_authority = false;
  policy.uncompressed_bytes = built.serialized.size();
  policy.estimated_compressed_bytes = kEnvelopeHeaderBytes + compact_payload.size() +
                                      sizeof(u64);
  auto decision = EvaluateCompressionPolicy(policy);
  const bool use_compact = !request.use_policy || decision.accepted;
  const auto kind = use_compact
                        ? RowPageCompactEncodingKind::row_page_trimmed_zero_tail
                        : RowPageCompactEncodingKind::row_page_uncompressed;
  const auto payload = use_compact ? compact_payload : built.serialized;
  auto decoded = DecodeInternal(
      SerializeEnvelope(kind,
                        static_cast<u32>(built.serialized.size()),
                        built.body.page_number,
                        canonical_checksum,
                        payload),
      request.authority);
  if (!decoded.ok()) {
    return decoded;
  }
  decoded.policy_decision = std::move(decision);
  decoded.compressed = use_compact;
  decoded.fallback_uncompressed = !use_compact;
  decoded.exact_round_trip = decoded.canonical_row_page == built.serialized &&
                             BodiesEqual(decoded.body, built.body);
  decoded.backup_restore_equivalent = decoded.exact_round_trip;
  if (!decoded.exact_round_trip) {
    return Refuse("ROW_PAGE_COMPACT.ROUND_TRIP_MISMATCH",
                  "row_page.compact.round_trip_mismatch",
                  "row_page_compact_round_trip_mismatch");
  }
  decoded.evidence = BaseEvidence(kind);
  decoded.evidence.push_back("row_page_compact.round_trip=true");
  decoded.evidence.push_back("row_page_compact.backup_restore_equivalent=true");
  decoded.evidence.push_back("row_page_compact.costed_decision=true");
  decoded.evidence.push_back("row_page_compact.row_count=" +
                             std::to_string(decoded.body.rows.size()));
  if (!use_compact) {
    decoded.evidence.push_back("row_page_compact.uncompressed_fallback_used=true");
  }
  AppendPolicyEvidence(&decoded.evidence, decoded.policy_decision);
  decoded.diagnostic = MakeRowPageCompactEncodingDiagnostic(
      decoded.status, "ROW_PAGE_COMPACT.OK", "row_page.compact.ok",
      RowPageCompactEncodingKindName(kind));
  return decoded;
}

RowPageCompactResult DecodeRowPageCompactEncoding(
    const std::vector<byte>& serialized,
    const RowPageCompactAuthorityContext& authority) {
  return DecodeInternal(serialized, authority);
}

RowPageCompactResult RepairOrValidateRowPageCompactEncoding(
    const std::vector<byte>& serialized,
    const RowPageCompactAuthorityContext& authority,
    const page::RowDataPageBody* authoritative_source,
    u32 page_size,
    const RowPageCompactRepairAdmission& admission) {
  auto opened = DecodeInternal(serialized, authority);
  if (opened.ok()) {
    opened.repair_state = RowPageCompactRepairState::validated;
    opened.evidence.push_back("row_page_compact.repair_state=validated");
    return opened;
  }
  const auto reason = opened.refusal_reasons.empty()
                          ? std::string("unknown")
                          : opened.refusal_reasons.front();
  if (!admission.repair_admitted ||
      !admission.authoritative_row_page_source_available ||
      !admission.same_page_identity_proven ||
      !admission.backup_restore_manifest_equivalence_proven ||
      authoritative_source == nullptr) {
    auto refused = Refuse("ROW_PAGE_COMPACT.REPAIR_REFUSED",
                          "row_page.compact.repair_refused",
                          "row_page_compact_repair_admission_not_proven");
    refused.evidence.push_back("row_page_compact.original_reason=" + reason);
    refused.evidence.push_back("row_page_compact.repair_state=refused");
    return refused;
  }
  RowPageCompactRequest rebuild;
  rebuild.body = *authoritative_source;
  rebuild.page_size = page_size;
  rebuild.authority = authority;
  rebuild.use_policy = false;
  auto repaired = BuildRowPageCompactEncoding(rebuild);
  if (!repaired.ok()) {
    auto refused = Refuse("ROW_PAGE_COMPACT.REPAIR_REFUSED",
                          "row_page.compact.repair_refused",
                          repaired.refusal_reasons.empty()
                              ? "row_page_compact_rebuild_refused"
                              : repaired.refusal_reasons.front());
    refused.evidence.push_back("row_page_compact.original_reason=" + reason);
    return refused;
  }
  repaired.repaired = true;
  repaired.repair_state =
      RowPageCompactRepairState::repaired_from_authoritative_row_page;
  repaired.diagnostic = MakeRowPageCompactEncodingDiagnostic(
      repaired.status, "ROW_PAGE_COMPACT.REPAIRED",
      "row_page.compact.repaired",
      "row_page_compact_repaired_from_authoritative_row_page");
  repaired.evidence.push_back("row_page_compact.original_reason=" + reason);
  repaired.evidence.push_back(
      "row_page_compact.repair_state=repaired_from_authoritative_row_page");
  repaired.evidence.push_back(
      "row_page_compact.repair.authoritative_row_page_source_used=true");
  repaired.evidence.push_back("row_page_compact.repair.non_authoritative=true");
  if (!admission.proof_detail.empty()) {
    repaired.evidence.push_back("row_page_compact.repair.proof_detail=" +
                                admission.proof_detail);
  }
  return repaired;
}

DiagnosticRecord MakeRowPageCompactEncodingDiagnostic(
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
                        "core.index.row_page_compact");
}

}  // namespace scratchbird::core::index
