// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_immutable_authority_provider.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

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
constexpr std::size_t kMaximumFrozenAuthoritySources = 1048576;

std::mutex g_update_authority_mutex;
std::unordered_map<std::string, EngineDmlUpdateImmutableAuthoritySnapshotV1>
    g_update_authorities;
std::atomic<std::uint64_t> g_update_authority_ordinal{1};

EngineApiDiagnostic Diagnostic(std::string code, std::string key,
                               std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

bool HasTraceTag(const EngineRequestContext& context, std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
         context.trace_tags.end();
}

bool ParseExactUuid(std::string_view text,
                    std::array<std::uint8_t, 16>* bytes = nullptr) {
  if (text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  if (bytes != nullptr) {
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
              bytes->begin());
  }
  return true;
}

bool UuidLess(std::string_view left, std::string_view right) {
  std::array<std::uint8_t, 16> left_bytes{};
  std::array<std::uint8_t, 16> right_bytes{};
  if (!ParseExactUuid(left, &left_bytes) ||
      !ParseExactUuid(right, &right_bytes)) {
    return left < right;
  }
  return left_bytes < right_bytes;
}

bool Nonzero(const EngineDmlUpdateSha256V1& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

void PutU32(std::vector<std::uint8_t>* bytes, std::size_t offset,
            std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    (*bytes)[offset + i] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
  }
}

void PutU64(std::vector<std::uint8_t>* bytes, std::size_t offset,
            std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    (*bytes)[offset + i] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
  }
}

bool PutUuid(std::vector<std::uint8_t>* bytes, std::size_t offset,
             std::string_view text, bool optional = false) {
  if (text.empty() && optional) return true;
  std::array<std::uint8_t, 16> parsed{};
  if (!ParseExactUuid(text, &parsed)) return false;
  std::copy(parsed.begin(), parsed.end(), bytes->begin() + offset);
  return true;
}

EngineDmlUpdateSha256V1 Hash(std::string_view domain,
                            const std::vector<std::uint8_t>& bytes) {
  std::vector<scratchbird::core::platform::byte> material;
  material.reserve(domain.size() + bytes.size());
  material.insert(material.end(), domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  return digest.ok() ? digest.digest : EngineDmlUpdateSha256V1{};
}

std::string FreshUuid() {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object,
      now + g_update_authority_ordinal.fetch_add(1,
                                                 std::memory_order_relaxed));
  return generated.ok()
      ? scratchbird::core::uuid::UuidToString(generated.value.value)
      : std::string{};
}

bool OptionalIdentityValid(std::string_view uuid, std::uint64_t generation) {
  return (uuid.empty() && generation == 0) ||
         (generation != 0 && ParseExactUuid(uuid));
}

bool EnumValid(EngineDmlUpdateRowPolicyPhaseV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 2;
}

bool EnumValid(EngineDmlUpdateConstraintClassV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 9;
}

bool EnumValid(EngineDmlUpdateConstraintTimingV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 5;
}

bool EnumValid(EngineDmlUpdateReservationModeV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 4;
}

bool EnumValid(EngineDmlUpdateTriggerTimingV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 4;
}

bool EnumValid(EngineDmlUpdateTriggerSecurityModeV1 value) {
  const auto code = static_cast<std::uint8_t>(value);
  return code >= 1 && code <= 2;
}

EngineApiDiagnostic ValidateBase(
    const EngineDmlUpdateImmutableAuthorityFreezeRequestV1& request,
    bool revalidation = false) {
  const auto& context = request.context;
  const bool binder =
      HasTraceTag(context, "private_dml_update_rows_binder");
  const bool consumer =
      HasTraceTag(context, "private_dml_update_rows_consumer");
  const bool recovery =
      HasTraceTag(context, "private_dml_update_rows_recovery");
  if (!context.security_context_present ||
      !context.statement_metadata_snapshot_engine_owned ||
      (revalidation ? (!consumer || binder || recovery)
                    : (!binder || consumer || recovery))) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticAccessDenied,
                      "sblr.dml_update_rows.authority_provider_denied");
  }
  if (request.autonomous_transaction || request.external_transaction) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticUnsupported,
                      "sblr.dml_update_rows.autonomous_authority_refused");
  }
  if (context.read_only_mode || context.cluster_transaction_active ||
      context.route_fence_present || context.local_transaction_id == 0 ||
      !ParseExactUuid(context.transaction_uuid.canonical) ||
      !ParseExactUuid(context.statement_snapshot_uuid.canonical) ||
      !ParseExactUuid(request.authenticated_statement_receipt_uuid) ||
      request.authenticated_statement_receipt_uuid !=
          context.statement_receipt_uuid.canonical ||
      request.structural_occurrence_id == 0 ||
      !ParseExactUuid(request.catalog_snapshot_uuid) ||
      request.catalog_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      request.catalog_generation == 0 ||
      request.catalog_generation != context.catalog_generation_id ||
      !ParseExactUuid(request.relation_occurrence.relation_uuid) ||
      request.relation_occurrence.relation_generation == 0 ||
      !ParseExactUuid(
          request.relation_occurrence.relation_occurrence_uuid) ||
      request.relation_occurrence.relation_occurrence_generation != 1 ||
      request.relation_occurrence.relation_uuid ==
          request.relation_occurrence.relation_occurrence_uuid) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.authority_context_invalid");
  }
  return Ok();
}

std::vector<std::uint8_t> RowPolicyBytes(
    EngineDmlUpdateFrozenRowPolicyRecordV1* record) {
  std::vector<std::uint8_t> bytes(176, 0);
  PutU32(&bytes, 0, record->policy_ordinal);
  bytes[4] = static_cast<std::uint8_t>(record->phase);
  bytes[5] = 1;
  if (!PutUuid(&bytes, 8, record->effective_policy_uuid) ||
      !PutUuid(&bytes, 32, record->expression_uuid) ||
      !PutUuid(&bytes, 88, record->security_snapshot_uuid)) {
    return {};
  }
  PutU64(&bytes, 24, record->effective_policy_generation);
  PutU64(&bytes, 48, record->expression_generation);
  std::copy(record->expression_evidence_sha256.begin(),
            record->expression_evidence_sha256.end(), bytes.begin() + 56);
  PutU64(&bytes, 104, record->security_generation);
  std::copy(record->source_policy_catalog_vector_sha256.begin(),
            record->source_policy_catalog_vector_sha256.end(),
            bytes.begin() + 112);
  std::vector<std::uint8_t> evidence_material(bytes.begin(),
                                              bytes.begin() + 144);
  record->record_evidence_sha256 =
      Hash(kRowPolicyRecordDomain, evidence_material);
  std::copy(record->record_evidence_sha256.begin(),
            record->record_evidence_sha256.end(), bytes.begin() + 144);
  return bytes;
}

std::vector<std::uint8_t> ConstraintBytes(
    EngineDmlUpdateFrozenConstraintRecordV1* record) {
  std::vector<std::uint8_t> bytes(160, 0);
  PutU32(&bytes, 0, record->constraint_ordinal);
  bytes[4] = static_cast<std::uint8_t>(record->constraint_class);
  bytes[5] = static_cast<std::uint8_t>(record->timing);
  bytes[6] = static_cast<std::uint8_t>(record->reservation_mode);
  if (!PutUuid(&bytes, 8, record->constraint_uuid) ||
      !PutUuid(&bytes, 32, record->expression_uuid, true) ||
      !PutUuid(&bytes, 56, record->reservation_profile_uuid)) {
    return {};
  }
  PutU64(&bytes, 24, record->constraint_generation);
  PutU64(&bytes, 48, record->expression_generation);
  PutU64(&bytes, 72, record->reservation_profile_generation);
  std::copy(record->dependency_set_sha256.begin(),
            record->dependency_set_sha256.end(), bytes.begin() + 80);
  std::vector<std::uint8_t> evidence_material(bytes.begin(),
                                              bytes.begin() + 112);
  record->record_evidence_sha256 =
      Hash(kConstraintRecordDomain, evidence_material);
  std::copy(record->record_evidence_sha256.begin(),
            record->record_evidence_sha256.end(), bytes.begin() + 112);
  return bytes;
}

std::vector<std::uint8_t> TriggerBytes(
    EngineDmlUpdateFrozenTriggerRecordV1* record) {
  std::vector<std::uint8_t> bytes(192, 0);
  PutU32(&bytes, 0, record->trigger_ordinal);
  bytes[4] = static_cast<std::uint8_t>(record->event);
  bytes[5] = static_cast<std::uint8_t>(record->timing);
  bytes[6] = static_cast<std::uint8_t>(record->security_mode);
  if (!PutUuid(&bytes, 8, record->trigger_uuid) ||
      !PutUuid(&bytes, 32, record->body_sblr_uuid) ||
      !PutUuid(&bytes, 56, record->execution_security_context_uuid) ||
      !PutUuid(&bytes, 80, record->recursion_profile_uuid)) {
    return {};
  }
  PutU64(&bytes, 24, record->trigger_generation);
  PutU64(&bytes, 48, record->body_sblr_generation);
  PutU64(&bytes, 72, record->execution_security_generation);
  PutU64(&bytes, 96, record->recursion_profile_generation);
  PutU32(&bytes, 104, record->maximum_depth);
  std::copy(record->dependency_set_sha256.begin(),
            record->dependency_set_sha256.end(), bytes.begin() + 112);
  std::vector<std::uint8_t> evidence_material(bytes.begin(),
                                              bytes.begin() + 144);
  record->record_evidence_sha256 =
      Hash(kTriggerRecordDomain, evidence_material);
  std::copy(record->record_evidence_sha256.begin(),
            record->record_evidence_sha256.end(), bytes.begin() + 144);
  return bytes;
}

template <typename Record, typename Encoder>
EngineDmlUpdateSha256V1 VectorHash(std::string_view domain,
                                  std::vector<Record>* records,
                                  Encoder encoder, bool* ok) {
  std::vector<std::uint8_t> material;
  *ok = true;
  for (auto& record : *records) {
    auto bytes = encoder(&record);
    if (bytes.empty() || !Nonzero(record.record_evidence_sha256)) {
      *ok = false;
      return {};
    }
    material.insert(material.end(), bytes.begin(), bytes.end());
  }
  auto digest = Hash(domain, material);
  *ok = Nonzero(digest);
  return digest;
}

EngineApiDiagnostic BuildRowPolicySet(
    const EngineDmlUpdateImmutableAuthorityFreezeRequestV1& request,
    const EngineSecurityPolicySnapshotAuthorityV1& security,
    std::string set_uuid,
    EngineDmlUpdateFrozenRowPolicySetV1* out) {
  if (out == nullptr || !ParseExactUuid(set_uuid)) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.row_policy_set_identity_invalid");
  }
  if (request.row_policies.size() > kMaximumFrozenAuthoritySources) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.row_policy_count_invalid");
  }
  std::vector<const EngineDmlUpdateRowPolicyAuthoritySourceV1*> eligible;
  for (const auto& source : request.row_policies) {
    if (!source.visible || source.deleted) continue;
    if (source.target_relation_uuid !=
            request.relation_occurrence.relation_uuid ||
        source.target_relation_generation !=
            request.relation_occurrence.relation_generation ||
        !ParseExactUuid(source.source_policy_uuid) ||
        source.source_policy_generation == 0 || !EnumValid(source.phase) ||
        !ParseExactUuid(source.source_policy_version_uuid) ||
        source.effective_transaction_number == 0 ||
        !ParseExactUuid(source.effective_policy_uuid) ||
        source.effective_policy_generation == 0 ||
        !ParseExactUuid(source.source_expression_uuid) ||
        source.source_expression_generation == 0 ||
        !Nonzero(source.source_expression_evidence_sha256) ||
        !ParseExactUuid(source.expression_uuid) ||
        source.expression_generation == 0 ||
        !Nonzero(source.expression_evidence_sha256) ||
        source.catalog_snapshot_uuid != request.catalog_snapshot_uuid ||
        source.catalog_generation != request.catalog_generation ||
        source.security_snapshot_uuid != security.snapshot_uuid ||
        source.security_snapshot_generation !=
            security.snapshot_generation ||
        source.security_generation != security.security_generation ||
        source.policy_catalog_generation != security.policy_generation ||
        !Nonzero(source.source_policy_catalog_vector_sha256)) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                        "sblr.dml_update_rows.row_policy_authority_invalid");
    }
    eligible.push_back(&source);
  }
  std::sort(eligible.begin(), eligible.end(), [](const auto* left,
                                                 const auto* right) {
    if (left->phase != right->phase) return left->phase < right->phase;
    return UuidLess(left->source_policy_uuid, right->source_policy_uuid);
  });
  std::unordered_set<std::string> seen_phase_sources;
  EngineDmlUpdateSha256V1 source_catalog_vector_sha256{};
  bool source_catalog_vector_observed = false;
  std::unordered_set<std::string> seen_policy_versions;
  for (const auto* source : eligible) {
    const std::string phase_source_key =
        std::to_string(static_cast<std::uint8_t>(source->phase)) + "\n" +
        source->source_policy_uuid;
    if (!seen_phase_sources.insert(phase_source_key).second) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                        "sblr.dml_update_rows.row_policy_source_duplicate");
    }
    if (!seen_policy_versions.insert(source->source_policy_version_uuid)
             .second) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                        "sblr.dml_update_rows.row_policy_version_duplicate");
    }
    if (!source_catalog_vector_observed) {
      source_catalog_vector_sha256 =
          source->source_policy_catalog_vector_sha256;
      source_catalog_vector_observed = true;
    } else if (source_catalog_vector_sha256 !=
               source->source_policy_catalog_vector_sha256) {
      return Diagnostic(
          kDmlUpdateAuthorityDiagnosticInvalid,
          "sblr.dml_update_rows.row_policy_catalog_vector_conflict");
    }
  }
  if (eligible.size() != security.admitted_policy_rows.size()) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticStale,
                      "sblr.dml_update_rows.row_policy_lifecycle_mismatch");
  }
  for (std::size_t index = 0; index < eligible.size(); ++index) {
    const auto& source = *eligible[index];
    const auto& admitted = security.admitted_policy_rows[index];
    if (source.source_policy_uuid != admitted.policy_uuid ||
        source.source_policy_generation != admitted.policy_generation ||
        source.source_policy_version_uuid != admitted.policy_version_uuid ||
        source.effective_transaction_number !=
            admitted.effective_transaction_number ||
        source.target_relation_uuid != admitted.target_relation_uuid ||
        source.target_relation_generation !=
            admitted.target_relation_generation ||
        static_cast<std::uint8_t>(source.phase) != admitted.phase ||
        source.effective_policy_uuid != admitted.effective_policy_uuid ||
        source.effective_policy_generation !=
            admitted.effective_policy_generation ||
        source.expression_uuid != admitted.effective_expression_uuid ||
        source.expression_generation !=
            admitted.effective_expression_generation ||
        source.expression_evidence_sha256 !=
            admitted.effective_expression_evidence_sha256 ||
        source.source_expression_uuid != admitted.source_expression_uuid ||
        source.source_expression_generation !=
            admitted.source_expression_generation ||
        source.source_expression_evidence_sha256 !=
            admitted.source_expression_evidence_sha256 ||
        source.catalog_snapshot_uuid != admitted.catalog_snapshot_uuid ||
        source.catalog_generation != admitted.catalog_generation ||
        source.security_generation != admitted.security_generation) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticStale,
                        "sblr.dml_update_rows.row_policy_lifecycle_mismatch");
    }
  }

  out->set_uuid = std::move(set_uuid);
  out->set_generation = 1;
  for (std::size_t begin = 0; begin < eligible.size();) {
    std::size_t end = begin + 1;
    while (end < eligible.size() &&
           eligible[end]->phase == eligible[begin]->phase) {
      ++end;
    }
    const auto& authority = *eligible[begin];
    for (std::size_t i = begin + 1; i < end; ++i) {
      const auto& candidate = *eligible[i];
      if (candidate.effective_policy_uuid !=
              authority.effective_policy_uuid ||
          candidate.effective_policy_generation !=
              authority.effective_policy_generation ||
          candidate.expression_uuid != authority.expression_uuid ||
          candidate.expression_generation != authority.expression_generation ||
          candidate.expression_evidence_sha256 !=
              authority.expression_evidence_sha256 ||
          candidate.source_policy_catalog_vector_sha256 !=
              authority.source_policy_catalog_vector_sha256) {
        return Diagnostic(
            kDmlUpdateAuthorityDiagnosticInvalid,
            "sblr.dml_update_rows.row_policy_effective_projection_conflict");
      }
    }
    EngineDmlUpdateFrozenRowPolicyRecordV1 record;
    record.policy_ordinal =
        static_cast<std::uint32_t>(out->records.size() + 1);
    record.phase = authority.phase;
    record.effective_policy_uuid = authority.effective_policy_uuid;
    record.effective_policy_generation =
        authority.effective_policy_generation;
    record.expression_uuid = authority.expression_uuid;
    record.expression_generation = authority.expression_generation;
    record.expression_evidence_sha256 =
        authority.expression_evidence_sha256;
    record.security_snapshot_uuid = security.snapshot_uuid;
    record.security_generation = security.security_generation;
    record.source_policy_catalog_vector_sha256 =
        authority.source_policy_catalog_vector_sha256;
    out->records.push_back(std::move(record));
    begin = end;
  }
  bool hash_ok = false;
  out->vector_sha256 = VectorHash(
      kRowPolicyVectorDomain, &out->records, RowPolicyBytes, &hash_ok);
  if (!hash_ok) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.row_policy_hash_failed");
  }
  return Ok();
}

EngineApiDiagnostic BuildConstraintSet(
    const EngineDmlUpdateImmutableAuthorityFreezeRequestV1& request,
    std::string set_uuid, EngineDmlUpdateFrozenConstraintSetV1* out) {
  if (out == nullptr || !ParseExactUuid(set_uuid)) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.constraint_set_identity_invalid");
  }
  if (request.constraints.size() > kMaximumFrozenAuthoritySources) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.constraint_count_invalid");
  }
  std::vector<const EngineDmlUpdateConstraintAuthoritySourceV1*> eligible;
  for (const auto& source : request.constraints) {
    if (!source.visible || source.deleted) continue;
    if (source.catalog_snapshot_uuid != request.catalog_snapshot_uuid ||
        source.catalog_generation != request.catalog_generation ||
        source.target_relation_uuid !=
            request.relation_occurrence.relation_uuid ||
        source.target_relation_generation !=
            request.relation_occurrence.relation_generation ||
        !source.manager_execution_order_present ||
        !EnumValid(source.constraint_class) || !EnumValid(source.timing) ||
        !EnumValid(source.reservation_mode) ||
        !ParseExactUuid(source.constraint_uuid) ||
        source.constraint_generation == 0 ||
        !OptionalIdentityValid(source.expression_uuid,
                               source.expression_generation) ||
        !ParseExactUuid(source.reservation_profile_uuid) ||
        source.reservation_profile_generation == 0 ||
        !Nonzero(source.dependency_set_sha256)) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                        "sblr.dml_update_rows.constraint_authority_invalid");
    }
    eligible.push_back(&source);
  }
  std::sort(eligible.begin(), eligible.end(), [](const auto* left,
                                                 const auto* right) {
    if (left->manager_execution_order != right->manager_execution_order) {
      return left->manager_execution_order < right->manager_execution_order;
    }
    return UuidLess(left->constraint_uuid, right->constraint_uuid);
  });
  std::unordered_set<std::string> seen;
  out->set_uuid = std::move(set_uuid);
  out->set_generation = 1;
  for (const auto* source : eligible) {
    if (!seen.insert(source->constraint_uuid).second) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                        "sblr.dml_update_rows.constraint_duplicate");
    }
    EngineDmlUpdateFrozenConstraintRecordV1 record;
    record.constraint_ordinal =
        static_cast<std::uint32_t>(out->records.size() + 1);
    record.constraint_class = source->constraint_class;
    record.timing = source->timing;
    record.reservation_mode = source->reservation_mode;
    record.constraint_uuid = source->constraint_uuid;
    record.constraint_generation = source->constraint_generation;
    record.expression_uuid = source->expression_uuid;
    record.expression_generation = source->expression_generation;
    record.reservation_profile_uuid = source->reservation_profile_uuid;
    record.reservation_profile_generation =
        source->reservation_profile_generation;
    record.dependency_set_sha256 = source->dependency_set_sha256;
    out->records.push_back(std::move(record));
  }
  bool hash_ok = false;
  out->vector_sha256 = VectorHash(
      kConstraintVectorDomain, &out->records, ConstraintBytes, &hash_ok);
  if (!hash_ok) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.constraint_hash_failed");
  }
  return Ok();
}

EngineApiDiagnostic BuildTriggerSet(
    const EngineDmlUpdateImmutableAuthorityFreezeRequestV1& request,
    const EngineSecurityPolicySnapshotAuthorityV1& security,
    std::string set_uuid, EngineDmlUpdateFrozenTriggerSetV1* out) {
  if (out == nullptr || !ParseExactUuid(set_uuid)) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.trigger_set_identity_invalid");
  }
  if (request.triggers.size() > kMaximumFrozenAuthoritySources) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.trigger_count_invalid");
  }
  std::vector<const EngineDmlUpdateTriggerAuthoritySourceV1*> eligible;
  for (const auto& source : request.triggers) {
    if (!source.visible || source.deleted) continue;
    if (source.autonomous || source.external_transaction) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticUnsupported,
                        "sblr.dml_update_rows.autonomous_trigger_refused");
    }
    if (source.catalog_snapshot_uuid != request.catalog_snapshot_uuid ||
        source.catalog_generation != request.catalog_generation ||
        source.target_relation_uuid !=
            request.relation_occurrence.relation_uuid ||
        source.target_relation_generation !=
            request.relation_occurrence.relation_generation ||
        source.security_generation != security.security_generation ||
        !source.firing_order_present ||
        source.event != EngineDmlUpdateTriggerEventV1::update ||
        !EnumValid(source.timing) || !EnumValid(source.security_mode) ||
        !ParseExactUuid(source.trigger_uuid) ||
        source.trigger_generation == 0 ||
        !ParseExactUuid(source.body_sblr_uuid) ||
        source.body_sblr_generation == 0 ||
        !ParseExactUuid(source.execution_security_context_uuid) ||
        source.execution_security_generation == 0 ||
        !ParseExactUuid(source.recursion_profile_uuid) ||
        source.recursion_profile_generation == 0 ||
        source.maximum_depth == 0 || source.maximum_depth > 64 ||
        !Nonzero(source.dependency_set_sha256)) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                        "sblr.dml_update_rows.trigger_authority_invalid");
    }
    eligible.push_back(&source);
  }
  std::sort(eligible.begin(), eligible.end(), [](const auto* left,
                                                 const auto* right) {
    if (left->timing != right->timing) return left->timing < right->timing;
    if (left->firing_order != right->firing_order) {
      return left->firing_order < right->firing_order;
    }
    return UuidLess(left->trigger_uuid, right->trigger_uuid);
  });
  std::unordered_set<std::string> seen;
  out->set_uuid = std::move(set_uuid);
  out->set_generation = 1;
  for (const auto* source : eligible) {
    if (!seen.insert(source->trigger_uuid).second) {
      return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                        "sblr.dml_update_rows.trigger_duplicate");
    }
    EngineDmlUpdateFrozenTriggerRecordV1 record;
    record.trigger_ordinal =
        static_cast<std::uint32_t>(out->records.size() + 1);
    record.event = source->event;
    record.timing = source->timing;
    record.security_mode = source->security_mode;
    record.trigger_uuid = source->trigger_uuid;
    record.trigger_generation = source->trigger_generation;
    record.body_sblr_uuid = source->body_sblr_uuid;
    record.body_sblr_generation = source->body_sblr_generation;
    record.execution_security_context_uuid =
        source->execution_security_context_uuid;
    record.execution_security_generation =
        source->execution_security_generation;
    record.recursion_profile_uuid = source->recursion_profile_uuid;
    record.recursion_profile_generation =
        source->recursion_profile_generation;
    record.maximum_depth = source->maximum_depth;
    record.dependency_set_sha256 = source->dependency_set_sha256;
    out->records.push_back(std::move(record));
  }
  bool hash_ok = false;
  out->vector_sha256 = VectorHash(kTriggerVectorDomain, &out->records,
                                  TriggerBytes, &hash_ok);
  if (!hash_ok) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.trigger_hash_failed");
  }
  return Ok();
}

std::string SnapshotKey(
    const EngineDmlUpdateImmutableAuthoritySnapshotV1& snapshot) {
  return snapshot.authenticated_statement_receipt_uuid + "\n" +
         snapshot.relation_occurrence.relation_occurrence_uuid + "\n" +
         snapshot.row_policy_set.set_uuid + "\n" +
         snapshot.constraint_set.set_uuid + "\n" +
         snapshot.trigger_set.set_uuid;
}

EngineApiDiagnostic BuildSets(
    const EngineDmlUpdateImmutableAuthorityFreezeRequestV1& request,
    const EngineSecurityPolicySnapshotAuthorityV1& security,
    std::string row_policy_set_uuid, std::string constraint_set_uuid,
    std::string trigger_set_uuid,
    EngineDmlUpdateImmutableAuthoritySnapshotV1* snapshot) {
  if (snapshot == nullptr) {
    return Diagnostic(kDmlUpdateAuthorityDiagnosticInvalid,
                      "sblr.dml_update_rows.authority_snapshot_required");
  }
  auto diagnostic = BuildRowPolicySet(request, security,
                                      std::move(row_policy_set_uuid),
                                      &snapshot->row_policy_set);
  if (diagnostic.error) return diagnostic;
  diagnostic = BuildConstraintSet(request, std::move(constraint_set_uuid),
                                  &snapshot->constraint_set);
  if (diagnostic.error) return diagnostic;
  return BuildTriggerSet(request, security, std::move(trigger_set_uuid),
                         &snapshot->trigger_set);
}

}  // namespace

EngineDmlUpdateImmutableAuthorityFreezeResultV1
FreezeDmlUpdateImmutableAuthorityV1(
    const EngineDmlUpdateImmutableAuthorityFreezeRequestV1& request) {
  EngineDmlUpdateImmutableAuthorityFreezeResultV1 result;
  result.diagnostic = ValidateBase(request);
  if (result.diagnostic.error) return result;

  EngineSecurityPolicySnapshotAuthorityV1 security_snapshot;
  if (request.security_policy_snapshot_authority.has_value()) {
    const auto revalidated =
        RevalidateEngineSecurityPolicySnapshotAuthorityV1(
            request.context,
            *request.security_policy_snapshot_authority);
    if (revalidated.error) {
      result.diagnostic = revalidated;
      return result;
    }
    security_snapshot = *request.security_policy_snapshot_authority;
  } else {
    const auto security = IssueEngineSecurityPolicySnapshotAuthorityV1(
        request.context, request.relation_occurrence.relation_uuid);
    if (!security.ok) {
      result.diagnostic = security.diagnostic;
      return result;
    }
    security_snapshot = security.snapshot;
  }

  EngineDmlUpdateImmutableAuthoritySnapshotV1 snapshot;
  snapshot.authenticated_statement_receipt_uuid =
      request.authenticated_statement_receipt_uuid;
  snapshot.structural_occurrence_id = request.structural_occurrence_id;
  snapshot.relation_occurrence = request.relation_occurrence;
  snapshot.catalog_snapshot_uuid = request.catalog_snapshot_uuid;
  snapshot.catalog_generation = request.catalog_generation;
  snapshot.security_policy_snapshot = security_snapshot;

  const std::string row_policy_set_uuid = FreshUuid();
  const std::string constraint_set_uuid = FreshUuid();
  const std::string trigger_set_uuid = FreshUuid();
  if (!ParseExactUuid(row_policy_set_uuid) ||
      !ParseExactUuid(constraint_set_uuid) ||
      !ParseExactUuid(trigger_set_uuid) ||
      row_policy_set_uuid == constraint_set_uuid ||
      row_policy_set_uuid == trigger_set_uuid ||
      constraint_set_uuid == trigger_set_uuid) {
    result.diagnostic = Diagnostic(
        kDmlUpdateAuthorityDiagnosticInvalid,
        "sblr.dml_update_rows.authority_set_identity_issue_failed");
    return result;
  }
  result.diagnostic = BuildSets(
      request, security_snapshot, row_policy_set_uuid, constraint_set_uuid,
      trigger_set_uuid, &snapshot);
  if (result.diagnostic.error) return result;

  std::lock_guard<std::mutex> guard(g_update_authority_mutex);
  const std::string key = SnapshotKey(snapshot);
  if (g_update_authorities.contains(key)) {
    result.diagnostic = Diagnostic(
        kDmlUpdateAuthorityDiagnosticInvalid,
        "sblr.dml_update_rows.authority_set_identity_duplicate");
    return result;
  }
  g_update_authorities.emplace(key, snapshot);
  result.ok = true;
  result.diagnostic = Ok();
  result.snapshot = std::move(snapshot);
  return result;
}

EngineDmlUpdateImmutableAuthorityRevalidateResultV1
RevalidateDmlUpdateImmutableAuthorityV1(
    const EngineDmlUpdateImmutableAuthorityRevalidateRequestV1& request) {
  EngineDmlUpdateImmutableAuthorityRevalidateResultV1 result;
  const auto base = ValidateBase(request.current, true);
  if (base.error) {
    result.diagnostic =
        base.code == kDmlUpdateAuthorityDiagnosticUnsupported ||
                base.code == kDmlUpdateAuthorityDiagnosticAccessDenied
            ? base
            : Diagnostic(kDmlUpdateAuthorityDiagnosticStale,
                         "sblr.dml_update_rows.authority_revalidation_stale");
    return result;
  }
  const auto& admitted = request.admitted;
  if (request.current.authenticated_statement_receipt_uuid !=
          admitted.authenticated_statement_receipt_uuid ||
      request.current.structural_occurrence_id !=
          admitted.structural_occurrence_id ||
      request.current.relation_occurrence != admitted.relation_occurrence ||
      request.current.catalog_snapshot_uuid != admitted.catalog_snapshot_uuid ||
      request.current.catalog_generation != admitted.catalog_generation ||
      admitted.row_policy_set.set_generation != 1 ||
      admitted.constraint_set.set_generation != 1 ||
      admitted.trigger_set.set_generation != 1) {
    result.diagnostic = Diagnostic(
        kDmlUpdateAuthorityDiagnosticStale,
        "sblr.dml_update_rows.authority_cross_binding_refused");
    return result;
  }

  auto security_diagnostic =
      RevalidateEngineSecurityPolicySnapshotAuthorityV1(
          request.current.context, admitted.security_policy_snapshot);
  if (security_diagnostic.error) {
    result.diagnostic = Diagnostic(
        kDmlUpdateAuthorityDiagnosticStale,
        "sblr.dml_update_rows.security_policy_snapshot_stale");
    return result;
  }

  {
    std::lock_guard<std::mutex> guard(g_update_authority_mutex);
    const auto found = g_update_authorities.find(SnapshotKey(admitted));
    if (found == g_update_authorities.end() || found->second != admitted) {
      result.diagnostic = Diagnostic(
          kDmlUpdateAuthorityDiagnosticStale,
          "sblr.dml_update_rows.authority_snapshot_unknown_or_forged");
      return result;
    }
  }

  EngineDmlUpdateImmutableAuthoritySnapshotV1 current;
  current.authenticated_statement_receipt_uuid =
      request.current.authenticated_statement_receipt_uuid;
  current.structural_occurrence_id = request.current.structural_occurrence_id;
  current.relation_occurrence = request.current.relation_occurrence;
  current.catalog_snapshot_uuid = request.current.catalog_snapshot_uuid;
  current.catalog_generation = request.current.catalog_generation;
  current.security_policy_snapshot = admitted.security_policy_snapshot;
  const auto build = BuildSets(
      request.current, admitted.security_policy_snapshot,
      admitted.row_policy_set.set_uuid, admitted.constraint_set.set_uuid,
      admitted.trigger_set.set_uuid, &current);
  if (build.error || current != admitted) {
    result.diagnostic =
        build.code == kDmlUpdateAuthorityDiagnosticUnsupported
            ? build
            : Diagnostic(kDmlUpdateAuthorityDiagnosticStale,
                         "sblr.dml_update_rows.authority_source_changed");
    return result;
  }

  result.ok = true;
  result.diagnostic = Ok();
  return result;
}

void ResetDmlUpdateImmutableAuthorityProviderForTestV1() {
  {
    std::lock_guard<std::mutex> guard(g_update_authority_mutex);
    g_update_authorities.clear();
    g_update_authority_ordinal.store(1, std::memory_order_release);
  }
  ResetEngineSecurityPolicySnapshotAuthorityForTestV1();
}

}  // namespace scratchbird::engine::internal_api
