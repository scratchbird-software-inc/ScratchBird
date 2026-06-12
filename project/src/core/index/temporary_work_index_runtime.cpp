// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "temporary_work_index_runtime.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <set>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'T', 'W',
                                               'I', 'D', '1', '5'};
inline constexpr u32 kHeaderBytes = 80;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;
inline constexpr u64 kRequiredFlags = 0x7ull;
inline constexpr u64 kFlagSpilled = 1ull << 3u;
inline constexpr u64 kFlagCandidateOnly = 1ull << 4u;
inline constexpr u64 kEstimatedRecordOverhead = 48;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

Status MemoryDeniedStatus() {
  return {StatusCode::memory_limit_exceeded, Severity::error,
          Subsystem::memory};
}

bool AuthorityClaimed(const TemporaryWorkAuthorityProof& proof) {
  return proof.parser_finality_authority_claimed ||
         proof.reference_finality_authority_claimed ||
         proof.provider_finality_authority_claimed ||
         proof.index_finality_authority_claimed ||
         proof.transaction_finality_authority_claimed ||
         proof.recovery_finality_authority_claimed ||
         proof.visibility_authority_claimed ||
         proof.security_authority_claimed ||
         proof.log_finality_authority_claimed;
}

bool UnsafeFallbackClaimed(const TemporaryWorkAuthorityProof& proof) {
  return proof.contract_only_fallback ||
         proof.lifecycle_only_fallback ||
         proof.provider_only_fallback ||
         proof.unsafe_materialized_final_rows;
}

bool RecheckProofValid(const TemporaryWorkAuthorityProof& proof) {
  return proof.proof_supplied &&
         proof.exact_recheck_required &&
         proof.exact_recheck_available &&
         proof.mga_visibility_recheck_required &&
         proof.mga_visibility_recheck_available &&
         proof.security_recheck_required &&
         proof.security_context_bound &&
         !proof.evidence_ref.empty() &&
         !AuthorityClaimed(proof) &&
         !UnsafeFallbackClaimed(proof);
}

std::vector<std::string> BaseEvidence(TemporaryWorkFamily family) {
  return {std::string(kTemporaryWorkIndexRuntimeSearchKey),
          std::string("temporary_work.family=") +
              TemporaryWorkFamilyName(family),
          "temporary_work.temporary=true",
          "temporary_work.candidate_rows_only=true",
          "temporary_work.final_rows_authorized=false",
          "temporary_work.visibility_authority=false",
          "temporary_work.security_authority=false",
          "temporary_work.transaction_finality_authority=false",
          "temporary_work.recovery_finality_authority=false",
          "temporary_work.parser_or_reference_authority=false",
          "temporary_work.provider_authority=false",
          "temporary_work.index_authority=false",
          "temporary_work.exact_recheck.required=true",
          "temporary_work.mga_recheck.required=true",
          "temporary_work.security_recheck.required=true"};
}

void AppendEvidence(std::vector<std::string>* target,
                    const std::vector<std::string>& source) {
  target->insert(target->end(), source.begin(), source.end());
}

std::string CountEvidence(const char* key, u64 value) {
  return std::string(key) + "=" + std::to_string(value);
}

TemporaryWorkResult Refuse(TemporaryWorkFamily family,
                           TemporaryWorkOpenClass open_class,
                           Status status,
                           std::string diagnostic_code,
                           std::string message_key,
                           std::string reason) {
  TemporaryWorkResult result;
  result.status = status;
  result.fail_closed = true;
  result.open_class = open_class;
  result.diagnostic = MakeTemporaryWorkIndexRuntimeDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence(family);
  result.evidence.push_back(std::string("temporary_work.open_class=") +
                            TemporaryWorkOpenClassName(open_class));
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

TemporaryWorkCleanupResult CleanupRefuse(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string reason) {
  TemporaryWorkCleanupResult result;
  result.status = status;
  result.fail_closed = true;
  result.diagnostic = MakeTemporaryWorkIndexRuntimeDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = {"temporary_work.cleanup.fail_closed=true",
                     "fallback_refusal_reason=" + reason};
  return result;
}

TemporaryWorkResult ValidateBuildAdmission(
    TemporaryWorkRuntimeState* runtime,
    TemporaryWorkFamily family,
    const TemporaryWorkAuthorityProof& proof) {
  if (runtime == nullptr) {
    return Refuse(family, TemporaryWorkOpenClass::refused, ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.RUNTIME_MISSING",
                  "index.temporary_work.runtime_missing",
                  "runtime_state_missing");
  }
  if (runtime->cancelled) {
    return Refuse(family, TemporaryWorkOpenClass::cancelled, ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.CANCELLED",
                  "index.temporary_work.cancelled",
                  "runtime_cancelled");
  }
  if (AuthorityClaimed(proof)) {
    return Refuse(family, TemporaryWorkOpenClass::authority_claim_refused,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.AUTHORITY_CLAIM_REFUSED",
                  "index.temporary_work.authority_claim_refused",
                  "temporary_work_cannot_own_visibility_security_or_finality");
  }
  if (UnsafeFallbackClaimed(proof)) {
    return Refuse(family, TemporaryWorkOpenClass::unsafe_fallback_refused,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.UNSAFE_FALLBACK_REFUSED",
                  "index.temporary_work.unsafe_fallback_refused",
                  "unsafe_temporary_work_fallback_mode");
  }
  if (!RecheckProofValid(proof)) {
    return Refuse(family, TemporaryWorkOpenClass::missing_recheck_proof,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.MISSING_RECHECK_PROOF",
                  "index.temporary_work.missing_recheck_proof",
                  "missing_exact_mga_or_security_recheck_proof");
  }
  TemporaryWorkResult ok;
  ok.status = OkStatus();
  ok.open_class = TemporaryWorkOpenClass::current;
  return ok;
}

u64 ComputeHash(const std::vector<byte>& bytes) {
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= value;
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
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

void AppendString(std::vector<byte>* out, const std::string& value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadU32(const std::vector<byte>& bytes, std::size_t* offset, u32* out) {
  if (*offset + sizeof(u32) > bytes.size()) return false;
  *out = LoadLittle32(bytes.data() + *offset);
  *offset += sizeof(u32);
  return true;
}

bool ReadU64(const std::vector<byte>& bytes, std::size_t* offset, u64* out) {
  if (*offset + sizeof(u64) > bytes.size()) return false;
  *out = LoadLittle64(bytes.data() + *offset);
  *offset += sizeof(u64);
  return true;
}

bool ReadString(const std::vector<byte>& bytes,
                std::size_t* offset,
                std::string* out) {
  u32 size = 0;
  if (!ReadU32(bytes, offset, &size) || *offset + size > bytes.size()) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(bytes.data() + *offset), size);
  *offset += size;
  return true;
}

std::vector<byte> PayloadFromSortRows(
    const std::vector<TemporaryWorkRecord>& rows) {
  std::vector<byte> payload;
  AppendU64(&payload, static_cast<u64>(rows.size()));
  for (const auto& row : rows) {
    AppendString(&payload, row.key);
    AppendString(&payload, row.payload);
    AppendU64(&payload, row.row_ordinal);
  }
  return payload;
}

std::vector<byte> PayloadFromHashRows(
    const std::vector<TemporaryHashBuildRow>& rows) {
  std::vector<byte> payload;
  AppendU64(&payload, static_cast<u64>(rows.size()));
  for (const auto& row : rows) {
    AppendString(&payload, row.key);
    AppendString(&payload, row.payload);
    AppendU64(&payload, row.row_ordinal);
  }
  return payload;
}

std::vector<byte> PayloadFromBulkSortEntries(
    const std::vector<TemporaryBulkSortBufferEntry>& entries) {
  std::vector<byte> payload;
  AppendU64(&payload, static_cast<u64>(entries.size()));
  for (const auto& entry : entries) {
    AppendString(&payload, entry.key);
    AppendString(&payload, entry.payload);
    AppendU64(&payload, entry.sequence);
  }
  return payload;
}

std::vector<byte> PayloadFromCandidateSet(const CandidateSet& set) {
  auto serialized = SerializeCompressedBitmapCandidateSet(set);
  if (serialized.ok()) return serialized.serialized;
  return {};
}

bool ParseSortRows(const std::vector<byte>& payload,
                   std::vector<TemporaryWorkRecord>* rows) {
  std::size_t offset = 0;
  u64 count = 0;
  if (!ReadU64(payload, &offset, &count)) return false;
  rows->clear();
  rows->reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    TemporaryWorkRecord row;
    if (!ReadString(payload, &offset, &row.key) ||
        !ReadString(payload, &offset, &row.payload) ||
        !ReadU64(payload, &offset, &row.row_ordinal)) {
      return false;
    }
    rows->push_back(std::move(row));
  }
  return offset == payload.size();
}

bool ParseHashRows(const std::vector<byte>& payload,
                   std::vector<TemporaryHashBuildRow>* rows) {
  std::size_t offset = 0;
  u64 count = 0;
  if (!ReadU64(payload, &offset, &count)) return false;
  rows->clear();
  rows->reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    TemporaryHashBuildRow row;
    if (!ReadString(payload, &offset, &row.key) ||
        !ReadString(payload, &offset, &row.payload) ||
        !ReadU64(payload, &offset, &row.row_ordinal)) {
      return false;
    }
    rows->push_back(std::move(row));
  }
  return offset == payload.size();
}

bool ParseBulkSortEntries(
    const std::vector<byte>& payload,
    std::vector<TemporaryBulkSortBufferEntry>* entries) {
  std::size_t offset = 0;
  u64 count = 0;
  if (!ReadU64(payload, &offset, &count)) return false;
  entries->clear();
  entries->reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    TemporaryBulkSortBufferEntry entry;
    if (!ReadString(payload, &offset, &entry.key) ||
        !ReadString(payload, &offset, &entry.payload) ||
        !ReadU64(payload, &offset, &entry.sequence)) {
      return false;
    }
    entries->push_back(std::move(entry));
  }
  return offset == payload.size();
}

u64 EstimateSortRows(const std::vector<TemporaryWorkRecord>& rows) {
  u64 bytes = 0;
  for (const auto& row : rows) {
    bytes += kEstimatedRecordOverhead + row.key.size() + row.payload.size();
  }
  return bytes;
}

u64 EstimateHashRows(const std::vector<TemporaryHashBuildRow>& rows) {
  u64 bytes = 0;
  for (const auto& row : rows) {
    bytes += kEstimatedRecordOverhead + row.key.size() + row.payload.size();
  }
  return bytes * 2u;
}

u64 EstimateBulkEntries(const std::vector<TemporaryBulkSortBufferEntry>& rows) {
  u64 bytes = 0;
  for (const auto& row : rows) {
    bytes += kEstimatedRecordOverhead + row.key.size() + row.payload.size();
  }
  return bytes;
}

u64 EstimateCandidateSet(const CandidateSet& set) {
  u64 bytes = kEstimatedRecordOverhead;
  bytes += static_cast<u64>(set.compressed_bitmap_containers.size()) * 256u;
  bytes += set.compressed_bitmap_cardinality / 8u;
  for (const auto& container : set.compressed_bitmap_containers) {
    bytes += static_cast<u64>(container.array_offsets.size()) * sizeof(u16);
    bytes += static_cast<u64>(container.runs.size()) *
             (sizeof(u16) + sizeof(u32));
    bytes += static_cast<u64>(container.bitmap_words.size()) * sizeof(u64);
  }
  return bytes;
}

bool AcquireMemoryGrant(TemporaryWorkRuntimeState* runtime,
                        u64 estimated_bytes,
                        bool spill_allowed,
                        u64* granted_bytes,
                        bool* spilled) {
  const u64 quota = runtime->options.memory_quota_bytes;
  const u64 remaining = quota > runtime->live_granted_bytes
                            ? quota - runtime->live_granted_bytes
                            : 0;
  if (estimated_bytes <= remaining) {
    *granted_bytes = estimated_bytes;
    *spilled = false;
  } else if (spill_allowed && remaining > 0) {
    *granted_bytes = remaining;
    *spilled = true;
  } else {
    runtime->total_denied_bytes += estimated_bytes;
    return false;
  }
  runtime->live_granted_bytes += *granted_bytes;
  runtime->total_granted_bytes += *granted_bytes;
  runtime->peak_granted_bytes =
      std::max(runtime->peak_granted_bytes, runtime->live_granted_bytes);
  return true;
}

std::string ArtifactId(TemporaryWorkFamily family,
                       u64 generation,
                       u64 ordinal) {
  return std::string("tw-") + TemporaryWorkFamilyName(family) + "-g" +
         std::to_string(generation) + "-a" + std::to_string(ordinal);
}

std::filesystem::path ArtifactPath(const TemporaryWorkRuntimeState& runtime,
                                   const std::string& artifact_id) {
  return runtime.options.spill_directory /
         (runtime.options.artifact_prefix + "-" + artifact_id + ".sbtmpidx");
}

std::vector<byte> MakeArtifactBytes(TemporaryWorkFamily family,
                                    u64 generation,
                                    u64 ordinal,
                                    u64 row_count,
                                    u64 grant_bytes,
                                    bool spilled,
                                    const std::vector<byte>& payload) {
  std::vector<byte> artifact;
  artifact.insert(artifact.end(), kMagic.begin(), kMagic.end());
  AppendU32(&artifact, kTemporaryWorkIndexRuntimeFormatVersion);
  AppendU32(&artifact, static_cast<u32>(family));
  AppendU64(&artifact, generation);
  AppendU64(&artifact, ordinal);
  AppendU64(&artifact, row_count);
  u64 flags = kRequiredFlags | kFlagCandidateOnly;
  if (spilled) flags |= kFlagSpilled;
  AppendU64(&artifact, flags);
  AppendU64(&artifact, grant_bytes);
  AppendU64(&artifact, static_cast<u64>(payload.size()));
  const u64 payload_hash = ComputeHash(payload);
  AppendU64(&artifact, payload_hash);
  AppendU64(&artifact, 0);
  artifact.insert(artifact.end(), payload.begin(), payload.end());
  const u64 artifact_hash = ComputeHash(artifact);
  StoreLittle64(artifact.data() + 72, artifact_hash);
  return artifact;
}

bool WriteArtifact(const std::filesystem::path& path,
                   const std::vector<byte>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) return false;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

std::vector<byte> ReadArtifactPath(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) return {};
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

bool Cleaned(const TemporaryWorkRuntimeState& runtime,
             const std::string& artifact_id) {
  return std::find(runtime.cleaned_artifact_ids.begin(),
                   runtime.cleaned_artifact_ids.end(),
                   artifact_id) != runtime.cleaned_artifact_ids.end();
}

void RememberActive(TemporaryWorkRuntimeState* runtime,
                    const TemporaryWorkArtifactDescriptor& descriptor) {
  runtime->active_artifacts.push_back(descriptor);
}

TemporaryWorkResult FinishBuild(TemporaryWorkRuntimeState* runtime,
                                TemporaryWorkFamily family,
                                u64 estimated_bytes,
                                u64 row_count,
                                bool spill_allowed,
                                std::vector<byte> payload) {
  u64 grant_bytes = 0;
  bool spilled = false;
  if (!AcquireMemoryGrant(runtime, estimated_bytes, spill_allowed,
                          &grant_bytes, &spilled)) {
    return Refuse(family, TemporaryWorkOpenClass::memory_grant_denied,
                  MemoryDeniedStatus(),
                  "INDEX.TEMPORARY_WORK.MEMORY_GRANT_DENIED",
                  "index.temporary_work.memory_grant_denied",
                  "temporary_work_memory_grant_denied");
  }
  const u64 ordinal = runtime->next_artifact_ordinal++;
  const std::string id =
      ArtifactId(family, runtime->options.runtime_generation, ordinal);
  const auto artifact = MakeArtifactBytes(
      family, runtime->options.runtime_generation, ordinal, row_count,
      grant_bytes, spilled, payload);
  std::filesystem::path path;
  if (spilled) {
    path = ArtifactPath(*runtime, id);
    if (!WriteArtifact(path, artifact)) {
      runtime->live_granted_bytes -= grant_bytes;
      return Refuse(family, TemporaryWorkOpenClass::corrupt_spill_payload,
                    ErrorStatus(),
                    "INDEX.TEMPORARY_WORK.SPILL_CREATE_FAILED",
                    "index.temporary_work.spill_create_failed",
                    "temporary_work_spill_create_failed");
    }
  }

  TemporaryWorkArtifactDescriptor descriptor;
  descriptor.artifact_id = id;
  descriptor.family = family;
  descriptor.runtime_generation = runtime->options.runtime_generation;
  descriptor.artifact_ordinal = ordinal;
  descriptor.row_count = row_count;
  descriptor.memory_grant_bytes = grant_bytes;
  descriptor.spilled = spilled;
  descriptor.path = path;
  descriptor.artifact = artifact;
  descriptor.evidence = BaseEvidence(family);
  descriptor.evidence.push_back(
      "temporary_work.artifact_format=SBTWID15");
  descriptor.evidence.push_back(
      std::string("temporary_work.spill_created=") +
          (spilled ? "true" : "false"));
  descriptor.evidence.push_back(
      std::string("temporary_work.spilled=") + (spilled ? "true" : "false"));
  descriptor.evidence.push_back(
      CountEvidence("temporary_work.memory.estimated_bytes",
                    estimated_bytes));
  descriptor.evidence.push_back(
      CountEvidence("temporary_work.memory.granted_bytes", grant_bytes));
  descriptor.evidence.push_back(
      CountEvidence("temporary_work.memory.live_granted_bytes",
                    runtime->live_granted_bytes));
  descriptor.evidence.push_back(
      CountEvidence("temporary_work.row_count", row_count));
  RememberActive(runtime, descriptor);

  TemporaryWorkResult result;
  result.status = OkStatus();
  result.open_class = TemporaryWorkOpenClass::current;
  result.descriptor = descriptor;
  result.evidence = descriptor.evidence;
  return result;
}

struct ParsedArtifact {
  TemporaryWorkFamily family = TemporaryWorkFamily::sort_run;
  u64 generation = 0;
  u64 ordinal = 0;
  u64 row_count = 0;
  u64 flags = 0;
  u64 grant_bytes = 0;
  bool spilled = false;
  std::vector<byte> payload;
};

bool ParseArtifact(const std::vector<byte>& artifact, ParsedArtifact* parsed) {
  if (artifact.size() < kHeaderBytes) return false;
  if (!std::equal(kMagic.begin(), kMagic.end(), artifact.begin())) {
    return false;
  }
  const u64 stored_artifact_hash = LoadLittle64(artifact.data() + 72);
  auto checksum_bytes = artifact;
  StoreLittle64(checksum_bytes.data() + 72, 0);
  if (ComputeHash(checksum_bytes) != stored_artifact_hash) {
    return false;
  }
  std::size_t offset = kMagic.size();
  u32 version = 0;
  u32 family_raw = 0;
  u64 payload_size = 0;
  u64 payload_hash = 0;
  u64 ignored_artifact_hash = 0;
  if (!ReadU32(artifact, &offset, &version) ||
      !ReadU32(artifact, &offset, &family_raw) ||
      !ReadU64(artifact, &offset, &parsed->generation) ||
      !ReadU64(artifact, &offset, &parsed->ordinal) ||
      !ReadU64(artifact, &offset, &parsed->row_count) ||
      !ReadU64(artifact, &offset, &parsed->flags) ||
      !ReadU64(artifact, &offset, &parsed->grant_bytes) ||
      !ReadU64(artifact, &offset, &payload_size) ||
      !ReadU64(artifact, &offset, &payload_hash) ||
      !ReadU64(artifact, &offset, &ignored_artifact_hash)) {
    return false;
  }
  if (version != kTemporaryWorkIndexRuntimeFormatVersion ||
      (parsed->flags & kRequiredFlags) != kRequiredFlags ||
      (parsed->flags & kFlagCandidateOnly) == 0 ||
      offset + payload_size != artifact.size()) {
    return false;
  }
  switch (static_cast<TemporaryWorkFamily>(family_raw)) {
    case TemporaryWorkFamily::sort_run:
    case TemporaryWorkFamily::hash_join_build_table:
    case TemporaryWorkFamily::temporary_bitmap_candidate_set:
    case TemporaryWorkFamily::bulk_sort_buffer:
      parsed->family = static_cast<TemporaryWorkFamily>(family_raw);
      break;
    default:
      return false;
  }
  parsed->spilled = (parsed->flags & kFlagSpilled) != 0;
  parsed->payload.assign(artifact.begin() + static_cast<std::ptrdiff_t>(offset),
                         artifact.end());
  return ComputeHash(parsed->payload) == payload_hash;
}

}  // namespace

TemporaryWorkRuntimeState CreateTemporaryWorkRuntime(
    TemporaryWorkRuntimeOptions options) {
  if (options.spill_directory.empty()) {
    options.spill_directory = std::filesystem::temp_directory_path();
  }
  TemporaryWorkRuntimeState state;
  state.options = std::move(options);
  state.evidence = {"temporary_work.runtime_created=true",
                    CountEvidence("temporary_work.runtime_generation",
                                  state.options.runtime_generation),
                    CountEvidence("temporary_work.memory.quota_bytes",
                                  state.options.memory_quota_bytes)};
  return state;
}

const char* TemporaryWorkFamilyName(TemporaryWorkFamily family) {
  switch (family) {
    case TemporaryWorkFamily::sort_run:
      return "sort_run";
    case TemporaryWorkFamily::hash_join_build_table:
      return "hash_join_build_table";
    case TemporaryWorkFamily::temporary_bitmap_candidate_set:
      return "temporary_bitmap_candidate_set";
    case TemporaryWorkFamily::bulk_sort_buffer:
      return "bulk_sort_buffer";
  }
  return "unknown";
}

const char* TemporaryWorkOpenClassName(TemporaryWorkOpenClass open_class) {
  switch (open_class) {
    case TemporaryWorkOpenClass::current:
      return "current";
    case TemporaryWorkOpenClass::stale_runtime_generation:
      return "stale_runtime_generation";
    case TemporaryWorkOpenClass::corrupt_spill_payload:
      return "corrupt_spill_payload";
    case TemporaryWorkOpenClass::missing_recheck_proof:
      return "missing_recheck_proof";
    case TemporaryWorkOpenClass::unsafe_fallback_refused:
      return "unsafe_fallback_refused";
    case TemporaryWorkOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case TemporaryWorkOpenClass::memory_grant_denied:
      return "memory_grant_denied";
    case TemporaryWorkOpenClass::cleaned_artifact:
      return "cleaned_artifact";
    case TemporaryWorkOpenClass::cancelled:
      return "cancelled";
    case TemporaryWorkOpenClass::refused:
      return "refused";
  }
  return "refused";
}

TemporaryWorkResult BuildTemporarySortRun(
    TemporaryWorkRuntimeState* runtime,
    std::vector<TemporaryWorkRecord> rows,
    const TemporaryWorkAuthorityProof& proof,
    bool spill_allowed) {
  auto admission =
      ValidateBuildAdmission(runtime, TemporaryWorkFamily::sort_run, proof);
  if (!admission.ok()) return admission;
  if (rows.empty()) {
    return Refuse(TemporaryWorkFamily::sort_run,
                  TemporaryWorkOpenClass::corrupt_spill_payload,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.EMPTY_SORT_RUN",
                  "index.temporary_work.empty_sort_run",
                  "temporary_sort_run_empty");
  }
  std::stable_sort(rows.begin(), rows.end(), [](const auto& left,
                                                const auto& right) {
    return std::tie(left.key, left.row_ordinal, left.payload) <
           std::tie(right.key, right.row_ordinal, right.payload);
  });
  auto result = FinishBuild(runtime, TemporaryWorkFamily::sort_run,
                            EstimateSortRows(rows), rows.size(),
                            spill_allowed, PayloadFromSortRows(rows));
  result.sorted_rows = std::move(rows);
  result.evidence.push_back("temporary_work.sort_run.sorted=true");
  return result;
}

TemporaryWorkResult BuildTemporaryHashJoinTable(
    TemporaryWorkRuntimeState* runtime,
    std::vector<TemporaryHashBuildRow> rows,
    const TemporaryWorkAuthorityProof& proof,
    bool spill_allowed) {
  auto admission = ValidateBuildAdmission(
      runtime, TemporaryWorkFamily::hash_join_build_table, proof);
  if (!admission.ok()) return admission;
  if (rows.empty()) {
    return Refuse(TemporaryWorkFamily::hash_join_build_table,
                  TemporaryWorkOpenClass::corrupt_spill_payload,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.EMPTY_HASH_BUILD",
                  "index.temporary_work.empty_hash_build",
                  "temporary_hash_build_empty");
  }
  std::stable_sort(rows.begin(), rows.end(), [](const auto& left,
                                                const auto& right) {
    return std::tie(left.key, left.row_ordinal, left.payload) <
           std::tie(right.key, right.row_ordinal, right.payload);
  });
  auto result =
      FinishBuild(runtime, TemporaryWorkFamily::hash_join_build_table,
                  EstimateHashRows(rows), rows.size(), spill_allowed,
                  PayloadFromHashRows(rows));
  result.hash_build_rows = std::move(rows);
  result.evidence.push_back("temporary_work.hash_join.build_table=true");
  return result;
}

TemporaryWorkResult BuildTemporaryBitmapCandidateSet(
    TemporaryWorkRuntimeState* runtime,
    CandidateSet candidate_set,
    const TemporaryWorkAuthorityProof& proof,
    bool spill_allowed) {
  auto admission = ValidateBuildAdmission(
      runtime, TemporaryWorkFamily::temporary_bitmap_candidate_set, proof);
  if (!admission.ok()) return admission;
  if (candidate_set.encoding != CandidateSetEncoding::compressed_bitmap ||
      !candidate_set.compressed ||
      !candidate_set.approximate ||
      !candidate_set.rows.empty() ||
      candidate_set.candidate_set_finality_authority ||
      candidate_set.final_rows_authorized ||
      !candidate_set.requires_exact_recheck ||
      !candidate_set.requires_mga_visibility_recheck ||
      !candidate_set.requires_security_authorization_recheck) {
    return Refuse(TemporaryWorkFamily::temporary_bitmap_candidate_set,
                  TemporaryWorkOpenClass::missing_recheck_proof,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.CANDIDATE_ONLY_PROOF_REQUIRED",
                  "index.temporary_work.candidate_only_proof_required",
                  "temporary_bitmap_candidate_only_proof_missing");
  }
  auto payload = PayloadFromCandidateSet(candidate_set);
  if (payload.empty()) {
    return Refuse(TemporaryWorkFamily::temporary_bitmap_candidate_set,
                  TemporaryWorkOpenClass::corrupt_spill_payload,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.CANDIDATE_SERIALIZE_REFUSED",
                  "index.temporary_work.candidate_serialize_refused",
                  "temporary_bitmap_candidate_serialize_refused");
  }
  const u64 row_count = candidate_set.compressed_bitmap_cardinality;
  auto result =
      FinishBuild(runtime, TemporaryWorkFamily::temporary_bitmap_candidate_set,
                  EstimateCandidateSet(candidate_set), row_count,
                  spill_allowed, std::move(payload));
  result.bitmap_candidate_set = std::move(candidate_set);
  result.evidence.push_back(
      "temporary_work.temporary_bitmap.candidate_set=true");
  return result;
}

TemporaryWorkResult BuildTemporaryBulkSortBuffer(
    TemporaryWorkRuntimeState* runtime,
    std::vector<TemporaryBulkSortBufferEntry> entries,
    const TemporaryWorkAuthorityProof& proof,
    bool spill_allowed) {
  auto admission =
      ValidateBuildAdmission(runtime, TemporaryWorkFamily::bulk_sort_buffer,
                             proof);
  if (!admission.ok()) return admission;
  if (entries.empty()) {
    return Refuse(TemporaryWorkFamily::bulk_sort_buffer,
                  TemporaryWorkOpenClass::corrupt_spill_payload,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.EMPTY_BULK_SORT_BUFFER",
                  "index.temporary_work.empty_bulk_sort_buffer",
                  "temporary_bulk_sort_buffer_empty");
  }
  std::stable_sort(entries.begin(), entries.end(), [](const auto& left,
                                                      const auto& right) {
    return std::tie(left.key, left.sequence, left.payload) <
           std::tie(right.key, right.sequence, right.payload);
  });
  auto result =
      FinishBuild(runtime, TemporaryWorkFamily::bulk_sort_buffer,
                  EstimateBulkEntries(entries), entries.size(), spill_allowed,
                  PayloadFromBulkSortEntries(entries));
  result.bulk_sort_buffer = std::move(entries);
  result.evidence.push_back("temporary_work.bulk_sort_buffer.sorted=true");
  return result;
}

TemporaryWorkResult OpenTemporaryWorkArtifact(
    TemporaryWorkRuntimeState* runtime,
    const TemporaryWorkArtifactDescriptor& descriptor,
    TemporaryWorkFamily expected_family,
    const TemporaryWorkAuthorityProof& proof) {
  auto admission = ValidateBuildAdmission(runtime, expected_family, proof);
  if (!admission.ok()) return admission;
  if (Cleaned(*runtime, descriptor.artifact_id)) {
    return Refuse(expected_family, TemporaryWorkOpenClass::cleaned_artifact,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.CLEANED_ARTIFACT",
                  "index.temporary_work.cleaned_artifact",
                  "temporary_work_artifact_cleaned");
  }
  std::vector<byte> artifact;
  if (descriptor.spilled) {
    if (descriptor.path.empty()) {
      return Refuse(expected_family,
                    TemporaryWorkOpenClass::corrupt_spill_payload,
                    ErrorStatus(),
                    "INDEX.TEMPORARY_WORK.CORRUPT_SPILL_PAYLOAD",
                    "index.temporary_work.corrupt_spill_payload",
                    "temporary_work_missing_spill_path");
    }
    artifact = ReadArtifactPath(descriptor.path);
  } else {
    artifact = descriptor.artifact;
  }
  ParsedArtifact parsed;
  if (!ParseArtifact(artifact, &parsed)) {
    return Refuse(expected_family,
                  TemporaryWorkOpenClass::corrupt_spill_payload,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.CORRUPT_SPILL_PAYLOAD",
                  "index.temporary_work.corrupt_spill_payload",
                  "temporary_work_corrupt_spill_payload");
  }
  if (parsed.generation != runtime->options.runtime_generation ||
      parsed.generation != descriptor.runtime_generation) {
    return Refuse(expected_family,
                  TemporaryWorkOpenClass::stale_runtime_generation,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.STALE_RUNTIME_GENERATION",
                  "index.temporary_work.stale_runtime_generation",
                  "temporary_work_stale_runtime_generation");
  }
  if (parsed.family != expected_family || parsed.family != descriptor.family ||
      parsed.ordinal != descriptor.artifact_ordinal ||
      parsed.row_count != descriptor.row_count ||
      parsed.grant_bytes != descriptor.memory_grant_bytes ||
      parsed.spilled != descriptor.spilled ||
      descriptor.artifact_id !=
          ArtifactId(parsed.family, parsed.generation, parsed.ordinal) ||
      !descriptor.candidate_rows_only ||
      !descriptor.exact_recheck_required ||
      !descriptor.mga_recheck_required ||
      !descriptor.security_recheck_required) {
    return Refuse(expected_family,
                  TemporaryWorkOpenClass::corrupt_spill_payload,
                  ErrorStatus(),
                  "INDEX.TEMPORARY_WORK.ARTIFACT_IDENTITY_MISMATCH",
                  "index.temporary_work.artifact_identity_mismatch",
                  "temporary_work_artifact_identity_mismatch");
  }

  TemporaryWorkResult result;
  result.status = OkStatus();
  result.open_class = TemporaryWorkOpenClass::current;
  result.descriptor = descriptor;
  result.descriptor.artifact = artifact;
  result.evidence = BaseEvidence(expected_family);
  result.evidence.push_back("temporary_work.spill_reopen=true");
  result.evidence.push_back("temporary_work.spill_payload_checksum=validated");
  result.evidence.push_back(
      "temporary_work.spill_artifact_checksum=validated");
  result.evidence.push_back(CountEvidence("temporary_work.row_count",
                                          parsed.row_count));

  if (expected_family == TemporaryWorkFamily::sort_run) {
    if (!ParseSortRows(parsed.payload, &result.sorted_rows)) {
      return Refuse(expected_family,
                    TemporaryWorkOpenClass::corrupt_spill_payload,
                    ErrorStatus(),
                    "INDEX.TEMPORARY_WORK.CORRUPT_SPILL_PAYLOAD",
                    "index.temporary_work.corrupt_spill_payload",
                    "temporary_work_corrupt_sort_run_payload");
    }
  } else if (expected_family ==
             TemporaryWorkFamily::hash_join_build_table) {
    if (!ParseHashRows(parsed.payload, &result.hash_build_rows)) {
      return Refuse(expected_family,
                    TemporaryWorkOpenClass::corrupt_spill_payload,
                    ErrorStatus(),
                    "INDEX.TEMPORARY_WORK.CORRUPT_SPILL_PAYLOAD",
                    "index.temporary_work.corrupt_spill_payload",
                    "temporary_work_corrupt_hash_build_payload");
    }
  } else if (expected_family ==
             TemporaryWorkFamily::bulk_sort_buffer) {
    if (!ParseBulkSortEntries(parsed.payload, &result.bulk_sort_buffer)) {
      return Refuse(expected_family,
                    TemporaryWorkOpenClass::corrupt_spill_payload,
                    ErrorStatus(),
                    "INDEX.TEMPORARY_WORK.CORRUPT_SPILL_PAYLOAD",
                    "index.temporary_work.corrupt_spill_payload",
                    "temporary_work_corrupt_bulk_sort_payload");
    }
  } else {
    CandidateSetAuthorityContext authority;
    authority.engine_mga_authoritative = true;
    authority.security_context_bound = true;
    authority.row_mga_recheck_required = true;
    authority.row_security_recheck_required = true;
    authority.exact_recheck_available = true;
    authority.exact_rerank_source_available = true;
    auto decoded = DeserializeCompressedBitmapCandidateSet(parsed.payload,
                                                           authority);
    if (!decoded.ok()) {
      return Refuse(expected_family,
                    TemporaryWorkOpenClass::corrupt_spill_payload,
                    ErrorStatus(),
                    "INDEX.TEMPORARY_WORK.CORRUPT_SPILL_PAYLOAD",
                    "index.temporary_work.corrupt_spill_payload",
                    "temporary_work_corrupt_bitmap_payload");
    }
    result.bitmap_candidate_set = std::move(decoded.output);
  }
  return result;
}

TemporaryWorkCleanupResult CleanupTemporaryWorkArtifact(
    TemporaryWorkRuntimeState* runtime,
    const std::string& artifact_id) {
  if (runtime == nullptr) {
    return CleanupRefuse(ErrorStatus(),
                         "INDEX.TEMPORARY_WORK.RUNTIME_MISSING",
                         "index.temporary_work.runtime_missing",
                         "runtime_state_missing");
  }
  auto it = std::find_if(runtime->active_artifacts.begin(),
                         runtime->active_artifacts.end(),
                         [&](const auto& artifact) {
                           return artifact.artifact_id == artifact_id;
                         });
  if (it == runtime->active_artifacts.end()) {
    return CleanupRefuse(ErrorStatus(),
                         "INDEX.TEMPORARY_WORK.CLEANED_ARTIFACT",
                         "index.temporary_work.cleaned_artifact",
                         "temporary_work_artifact_not_active");
  }
  std::error_code ignored;
  if (!it->path.empty()) {
    std::filesystem::remove(it->path, ignored);
  }
  const u64 released = it->memory_grant_bytes;
  runtime->live_granted_bytes =
      released > runtime->live_granted_bytes ? 0
                                             : runtime->live_granted_bytes -
                                                   released;
  runtime->cleaned_artifact_ids.push_back(it->artifact_id);
  runtime->active_artifacts.erase(it);

  TemporaryWorkCleanupResult result;
  result.status = OkStatus();
  result.cleaned = true;
  result.released_grant_bytes = released;
  result.cleaned_artifact_ids.push_back(artifact_id);
  result.evidence = {"temporary_work.cleanup.cleaned=true",
                     "temporary_work.cleanup.cancel_safe=true",
                     CountEvidence("temporary_work.memory.released_bytes",
                                   released),
                     CountEvidence(
                         "temporary_work.memory.live_granted_bytes",
                         runtime->live_granted_bytes)};
  return result;
}

TemporaryWorkCleanupResult CancelTemporaryWorkRuntime(
    TemporaryWorkRuntimeState* runtime) {
  if (runtime == nullptr) {
    return CleanupRefuse(ErrorStatus(),
                         "INDEX.TEMPORARY_WORK.RUNTIME_MISSING",
                         "index.temporary_work.runtime_missing",
                         "runtime_state_missing");
  }
  TemporaryWorkCleanupResult result;
  result.status = OkStatus();
  result.cleaned = true;
  result.evidence = {"temporary_work.cancel.requested=true",
                     "temporary_work.cleanup.cancel_safe=true"};
  auto active = runtime->active_artifacts;
  for (const auto& artifact : active) {
    auto cleanup = CleanupTemporaryWorkArtifact(runtime, artifact.artifact_id);
    if (!cleanup.ok()) {
      return cleanup;
    }
    result.released_grant_bytes += cleanup.released_grant_bytes;
    AppendEvidence(&result.evidence, cleanup.evidence);
    result.cleaned_artifact_ids.push_back(artifact.artifact_id);
  }
  runtime->cancelled = true;
  result.evidence.push_back("temporary_work.cancel.completed=true");
  result.evidence.push_back(
      CountEvidence("temporary_work.memory.live_granted_bytes",
                    runtime->live_granted_bytes));
  return result;
}

DiagnosticRecord MakeTemporaryWorkIndexRuntimeDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!detail.empty()) {
    record.arguments.push_back(DiagnosticArgument{"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.temporary_work_runtime";
  return record;
}

}  // namespace scratchbird::core::index
