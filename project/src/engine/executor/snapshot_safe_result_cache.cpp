// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "snapshot_safe_result_cache.hpp"

#include "uuid.hpp"
#include "../../core/hash/hash_digest.hpp"
#include <stdexcept>
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

void AddBool(std::vector<std::string>* evidence,
             const std::string& key,
             bool value) {
  Add(evidence, key + "=" + (value ? "true" : "false"));
}

bool KeyComplete(const SnapshotSafeCacheKey& key) {
  return core::uuid::IsEngineIdentityUuid(key.bound_sblr_tree_uuid) &&
         core::uuid::IsEngineIdentityUuid(key.security_context_uuid) &&
         !key.safe_parameter_digest.empty() &&
         core::uuid::IsEngineIdentityUuid(key.catalog_epoch_uuid) &&
         key.catalog_epoch != 0 &&
         key.statistics_epoch != 0 &&
         key.security_epoch != 0 &&
         key.redaction_epoch != 0 &&
         !key.mga_visibility_snapshot_class.empty() &&
         key.provider_generation != 0 &&
         !key.descriptor_identity_digest.empty() &&
         key.descriptor_epoch != 0 &&
         !key.result_contract_identity.empty() &&
         !key.result_contract_hash.empty() &&
         !key.route_compatibility.empty();
}

bool AnyUncertainty(const SnapshotSafeCacheStoreRequest& request) {
  return request.dml_uncertain || request.ddl_uncertain ||
         request.security_uncertain || request.redaction_uncertain ||
         request.statistics_uncertain ||
         request.provider_generation_uncertain || request.route_uncertain ||
         request.visibility_uncertain;
}

bool AnyUncertainty(const SnapshotSafeCacheLookupRequest& request) {
  return request.dml_uncertain || request.ddl_uncertain ||
         request.security_uncertain || request.redaction_uncertain ||
         request.statistics_uncertain ||
         request.provider_generation_uncertain || request.route_uncertain ||
         request.visibility_uncertain;
}

bool HardRefusal(const SnapshotSafeCacheStoreRequest& request,
                 std::string* code,
                 std::string* detail) {
  if (request.result_contract_uncertain) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.RESULT_CONTRACT_UNCERTAIN";
    *detail = "result contract identity and hash must be proven before caching";
    return true;
  }
  if (request.provider_generation_mutable) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.PROVIDER_GENERATION_MUTABLE";
    *detail = "mutable provider generations cannot back snapshot cache entries";
    return true;
  }
  if (request.route_mismatch) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.ROUTE_MISMATCH";
    *detail = "request route does not match cache key route compatibility";
    return true;
  }
  if (request.volatile_function_dependency) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.VOLATILE_FUNCTION_REFUSED";
    *detail = "volatile function dependencies cannot be result-cached";
    return true;
  }
  if (request.uncommitted_own_transaction_visibility_dependency) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.OWN_TRANSACTION_VISIBILITY_REFUSED";
    *detail = "uncommitted own-transaction visibility dependency is not snapshot-safe";
    return true;
  }
  if (request.negative_cache_entry && !request.negative_cache_snapshot_safe_proven) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.NEGATIVE_CACHE_REFUSED";
    *detail = "negative entry requires explicit snapshot-safe admission";
    return true;
  }
  return false;
}

bool HardRefusal(const SnapshotSafeCacheLookupRequest& request,
                 std::string* code,
                 std::string* detail) {
  if (request.result_contract_uncertain) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.RESULT_CONTRACT_UNCERTAIN";
    *detail = "result contract identity and hash must be proven before lookup";
    return true;
  }
  if (request.provider_generation_mutable) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.PROVIDER_GENERATION_MUTABLE";
    *detail = "mutable provider generations cannot back snapshot cache entries";
    return true;
  }
  if (request.route_mismatch) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.ROUTE_MISMATCH";
    *detail = "request route does not match cache key route compatibility";
    return true;
  }
  if (request.volatile_function_dependency) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.VOLATILE_FUNCTION_REFUSED";
    *detail = "volatile function dependencies cannot be result-cached";
    return true;
  }
  if (request.uncommitted_own_transaction_visibility_dependency) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.OWN_TRANSACTION_VISIBILITY_REFUSED";
    *detail = "uncommitted own-transaction visibility dependency is not snapshot-safe";
    return true;
  }
  if (request.negative_cache_entry && !request.negative_cache_snapshot_safe_proven) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.NEGATIVE_CACHE_REFUSED";
    *detail = "negative entry requires explicit snapshot-safe admission";
    return true;
  }
  return false;
}

bool Eligible(SnapshotSafeCachePayloadKind kind,
              bool candidate_set_snapshot_safe,
              bool small_final_result,
              std::uint64_t row_count,
              std::uint64_t max_small_result_rows) {
  if (kind == SnapshotSafeCachePayloadKind::kCandidateSet) {
    return candidate_set_snapshot_safe;
  }
  return kind == SnapshotSafeCachePayloadKind::kSmallFinalResult &&
         small_final_result && row_count <= max_small_result_rows;
}

// Full framed bytes are the map key and equality authority. SHA-256 is a
// display/content digest only; it never replaces full-byte comparison.
class CacheEncoding {
 public:
  explicit CacheEncoding(unsigned domain) : bytes("SBCS", 4) {
    bytes.push_back(1); bytes.push_back(static_cast<char>(domain));
  }
  void Number(std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) {
      bytes.push_back(static_cast<char>(value & 255)); value >>= 8;
    }
  }
  void Text(std::string_view value) {
    Number(value.size()); bytes.append(value);
  }
  void Uuid(const internal_api::EngineUuid& value) {
    bytes.append(reinterpret_cast<const char*>(value.bytes.data()), 16);
  }
  void Descriptor(const internal_api::EngineDescriptor& descriptor) {
    Uuid(descriptor.descriptor_uuid); Uuid(descriptor.type_uuid);
    Uuid(descriptor.collation_uuid); Text(descriptor.descriptor_kind);
    Text(descriptor.canonical_type_name); Text(descriptor.encoded_descriptor);
  }
  std::string bytes;
};

std::string DigestBytes(std::string_view bytes) {
  const auto digest = core::hash::ComputeSha256Digest(
      reinterpret_cast<const core::platform::byte*>(bytes.data()), bytes.size());
  if (!digest.ok() || digest.digest_bytes != core::hash::kSha256DigestBytes)
    throw std::runtime_error("snapshot cache content hashing failed");
  return "sha256:" + core::hash::HexLower(digest.digest);
}

std::string EncodeCacheKey(const SnapshotSafeCacheKey& key) {
  CacheEncoding out(1);
  out.Uuid(key.bound_sblr_tree_uuid); out.Text(key.safe_parameter_digest);
  out.Uuid(key.catalog_epoch_uuid); out.Uuid(key.security_context_uuid);
  out.Number(key.catalog_epoch); out.Number(key.statistics_epoch);
  out.Number(key.security_epoch); out.Number(key.redaction_epoch);
  out.Text(key.mga_visibility_snapshot_class); out.Number(key.provider_generation);
  out.Text(key.descriptor_identity_digest); out.Number(key.descriptor_epoch);
  out.Text(key.result_contract_identity); out.Text(key.result_contract_hash);
  out.Text(key.route_compatibility);
  return std::move(out.bytes);
}

std::string StatementBoundCacheIdentity(
    const SnapshotSafeCacheKey& key, const PhysicalMgaStatementContext& context,
    const internal_api::EngineUuid& catalog_epoch_uuid) {
  CacheEncoding out(2);
  out.Text(EncodeCacheKey(key)); out.Uuid(catalog_epoch_uuid);
  out.Uuid(context.statement_uuid); out.Uuid(context.owning_transaction_uuid);
  out.Uuid(context.statement_snapshot_uuid); out.Uuid(context.statement_metadata_snapshot_uuid);
  out.Number(context.owning_local_transaction_id);
  out.Number(context.visible_committed_high_watermark);
  out.Number(context.oldest_active_transaction_id);
  out.Number(context.oldest_interesting_transaction_id);
  out.Number(context.oldest_snapshot_transaction_id);
  out.Number(context.retention_horizon_transaction_id);
  out.Number(context.active_excluded_local_transaction_ids.size());
  for (const auto value : context.active_excluded_local_transaction_ids) out.Number(value);
  out.Number(context.in_doubt_excluded_local_transaction_ids.size());
  for (const auto value : context.in_doubt_excluded_local_transaction_ids) out.Number(value);
  out.Text(context.snapshot_kind);
  out.Number(context.publication_inventory_next_local_transaction_id);
  out.Number(context.inventory_authoritative); out.Number(context.complete);
  out.Number(context.current); out.Text(context.statement_timestamp);
  return std::move(out.bytes);
}

std::string EntryKey(const SnapshotSafeCacheKey& key, SnapshotSafeCachePayloadKind kind,
                     const PhysicalMgaStatementContext& context,
                     const internal_api::EngineUuid& catalog_epoch_uuid) {
  CacheEncoding out(3); out.Number(static_cast<std::uint64_t>(kind));
  out.Text(StatementBoundCacheIdentity(key, context, catalog_epoch_uuid));
  return std::move(out.bytes);
}

std::string PayloadBytes(const SnapshotSafeCachePayload& payload,
                         SnapshotSafeCachePayloadKind kind) {
  CacheEncoding out(4); out.Number(static_cast<std::uint64_t>(kind));
  out.Number(payload.final_result.columns.size());
  for (const auto& column : payload.final_result.columns) {
    out.Text(column.stable_name); out.Descriptor(column.descriptor);
    out.Number(column.nullable); out.Number(column.descriptor_id);
  }
  out.Number(payload.final_result.rows.size());
  for (const auto& row : payload.final_result.rows) {
    out.Number(row.values.size());
    for (const auto& value : row.values) {
      out.Descriptor(value.descriptor); out.Text(value.encoded_value);
      out.Number(value.binary_value.size());
      for (const auto byte : value.binary_value) out.bytes.push_back(static_cast<char>(byte));
      out.Number(value.is_null); out.Number(static_cast<std::uint64_t>(value.state));
    }
  }
  out.Number(payload.candidates.size());
  for (const auto& candidate : payload.candidates) {
    out.Uuid(candidate.candidate_uuid); out.Uuid(candidate.record_uuid);
    out.Uuid(candidate.relation_uuid); out.Uuid(candidate.visibility_decision_uuid);
    out.Number(candidate.row_version_id); out.Number(candidate.candidate_generation);
    out.Number(candidate.observed_generation); out.Number(candidate.creator_local_transaction_id);
    out.Number(static_cast<std::uint64_t>(candidate.source));
    out.Number(static_cast<std::uint64_t>(candidate.visibility));
    out.Number(static_cast<std::uint64_t>(candidate.security_decision));
    out.Number(static_cast<std::uint64_t>(candidate.residual_truth));
    out.Number(candidate.locator_identity_matches);
  }
  return std::move(out.bytes);
}

bool PayloadValid(const SnapshotSafeCachePayload& payload,
                  SnapshotSafeCachePayloadKind kind, std::uint64_t count) {
  if (kind == SnapshotSafeCachePayloadKind::kSmallFinalResult) {
    std::vector<std::uint32_t> descriptor_ids;
    descriptor_ids.reserve(payload.final_result.columns.size());
    for (const auto& column : payload.final_result.columns)
      descriptor_ids.push_back(column.descriptor_id);
    return payload.candidates.empty() && payload.final_result.rows.size() == count &&
           ValidateCanonicalDescriptorBatch(payload.final_result, descriptor_ids).ok;
  }
  if (kind != SnapshotSafeCachePayloadKind::kCandidateSet ||
      !payload.final_result.columns.empty() || !payload.final_result.rows.empty() ||
      payload.candidates.size() != count) return false;
  std::unordered_set<internal_api::EngineUuid, internal_api::EngineUuidHash> ids;
  for (const auto& candidate : payload.candidates) {
    if (!core::uuid::IsEngineIdentityUuid(candidate.candidate_uuid) ||
        !core::uuid::IsEngineIdentityUuid(candidate.record_uuid) ||
        !core::uuid::IsEngineIdentityUuid(candidate.relation_uuid) ||
        !core::uuid::IsEngineIdentityUuid(candidate.visibility_decision_uuid) ||
        !ids.insert(candidate.candidate_uuid).second || candidate.row_version_id == 0 ||
        candidate.candidate_generation == 0 || candidate.observed_generation == 0 ||
        candidate.creator_local_transaction_id == 0 ||
        (candidate.source != CanonicalScanCandidateSource::kRelationPage &&
         candidate.source != CanonicalScanCandidateSource::kIndexEntry) ||
        (candidate.visibility != CanonicalMgaVisibilityDecision::kVisible &&
         candidate.visibility != CanonicalMgaVisibilityDecision::kInvisible &&
         candidate.visibility != CanonicalMgaVisibilityDecision::kIndeterminate) ||
        (candidate.security_decision != CanonicalMgaSecurityDecision::kAllowed &&
         candidate.security_decision != CanonicalMgaSecurityDecision::kDenied &&
         candidate.security_decision != CanonicalMgaSecurityDecision::kIndeterminate) ||
        (candidate.residual_truth != internal_api::EngineSqlTruthValue::unspecified &&
         candidate.residual_truth != internal_api::EngineSqlTruthValue::false_value &&
         candidate.residual_truth != internal_api::EngineSqlTruthValue::true_value &&
         candidate.residual_truth != internal_api::EngineSqlTruthValue::unknown)) return false;
  }
  // Candidates are not final visible rows: consuming them still requires the
  // actual selected scan's MGA/security/residual/current-locator checks.
  return true;
}

DescriptorRuntimeDiagnostic ValidateStatementBinding(
    const CanonicalExecutionMgaAuthority& authority,
    const TypedPhysicalNodeDag& selected_physical_dag,
    const internal_api::EngineUuid& selected_catalog_epoch_uuid,
    const SnapshotSafeCacheKey& key) {
  DescriptorRuntimeDiagnostic diagnostic;
  const auto refuse = [&](std::string code, std::string detail) {
    diagnostic.ok = false;
    diagnostic.diagnostic_code = std::move(code);
    diagnostic.detail = std::move(detail);
    return diagnostic;
  };
  if (authority.origin !=
          CanonicalMgaAuthorityOrigin::kEngineTransactionInventory ||
      selected_physical_dag.abi_version != 2) {
    return refuse("EXECUTOR.SNAPSHOT_RESULT_CACHE.MGA_AUTHORITY_REQUIRED",
                  "ABI-v2 engine transaction inventory authority is required");
  }
  const auto revalidated = RevalidateCanonicalExecutionMgaAuthority(
      authority, selected_physical_dag);
  if (!revalidated.ok) return revalidated;
  const auto& context = authority.statement_context;
  if (!core::uuid::IsEngineIdentityUuid(selected_catalog_epoch_uuid) ||
      key.bound_sblr_tree_uuid != selected_physical_dag.bound_sblr_tree_uuid ||
      key.security_context_uuid != selected_physical_dag.security_context_uuid ||
      selected_catalog_epoch_uuid != selected_physical_dag.catalog_epoch_uuid ||
      key.catalog_epoch_uuid != selected_catalog_epoch_uuid ||
      selected_catalog_epoch_uuid == context.statement_uuid ||
      selected_catalog_epoch_uuid == context.owning_transaction_uuid ||
      selected_catalog_epoch_uuid == context.statement_snapshot_uuid ||
      selected_catalog_epoch_uuid ==
          context.statement_metadata_snapshot_uuid) {
    return refuse("EXECUTOR.SNAPSHOT_RESULT_CACHE.CATALOG_IDENTITY_REQUIRED",
                  "independent selected catalog UUID must match the key and DAG");
  }
  return diagnostic;
}

SnapshotSafeCacheDecision BaseDecision(
    const SnapshotSafeCacheKey& key,
    SnapshotSafeCachePayloadKind kind,
    const PhysicalMgaStatementContext& statement_context,
    const internal_api::EngineUuid& catalog_epoch_uuid) {
  SnapshotSafeCacheDecision decision;
  decision.cache_key_text = DigestBytes(EntryKey(key, kind, statement_context, catalog_epoch_uuid));
  Add(&decision.evidence, kSnapshotSafeCandidateResultCacheSearchKey);
  Add(&decision.evidence, "snapshot_cache_payload_kind=" +
                              std::string(SnapshotSafeCachePayloadKindName(kind)));
  Add(&decision.evidence, "snapshot_cache_key=" + decision.cache_key_text);
  Add(&decision.evidence, "operation_authority=bound_binary_sblr_tree");
  Add(&decision.evidence,
      "safe_parameter_digest=" + key.safe_parameter_digest);
  Add(&decision.evidence, "catalog_identity=binary_uuid");
  Add(&decision.evidence,
      "catalog_epoch=" + std::to_string(key.catalog_epoch));
  Add(&decision.evidence,
      "statistics_epoch=" + std::to_string(key.statistics_epoch));
  Add(&decision.evidence,
      "security_epoch=" + std::to_string(key.security_epoch));
  Add(&decision.evidence,
      "redaction_epoch=" + std::to_string(key.redaction_epoch));
  Add(&decision.evidence,
      "mga_visibility_snapshot_class=" +
          key.mga_visibility_snapshot_class);
  Add(&decision.evidence,
      "provider_generation=" + std::to_string(key.provider_generation));
  Add(&decision.evidence,
      "descriptor_identity_digest=" + key.descriptor_identity_digest);
  Add(&decision.evidence,
      "descriptor_epoch=" + std::to_string(key.descriptor_epoch));
  Add(&decision.evidence,
      "result_contract_identity=" + key.result_contract_identity);
  Add(&decision.evidence,
      "result_contract_hash=" + key.result_contract_hash);
  Add(&decision.evidence,
      "route_compatibility=" + key.route_compatibility);
  Add(&decision.evidence, "cache_authority=none");
  return decision;
}

SnapshotSafeCacheDecision Finish(SnapshotSafeCacheDecision decision,
                                 SnapshotSafeCacheAction action,
                                 std::string diagnostic_code,
                                 std::string diagnostic_detail,
                                 bool accepted,
                                 bool fail_closed,
                                 bool cache_hit) {
  // A later refusal must not retain a staged success claim as evidence.
  std::erase_if(decision.evidence, [](const std::string& value) {
    return value.starts_with("snapshot_cache_action=") ||
           value.starts_with("snapshot_cache_diagnostic=") ||
           value.starts_with("snapshot_cache_detail=") ||
           value.starts_with("snapshot_cache_hit=") ||
           value.starts_with("fail_closed=") ||
           value == "snapshot_cache_stored=true" ||
           value == "snapshot_cache_identical_to_recompute=true" ||
           value == "snapshot_cache_invalidated=true";
  });
  decision.retained_entry.reset();
  decision.action = action;
  decision.diagnostic_code = std::move(diagnostic_code);
  decision.diagnostic_detail = std::move(diagnostic_detail);
  decision.accepted = accepted;
  decision.fail_closed = fail_closed;
  decision.cache_hit = cache_hit;
  if (action == SnapshotSafeCacheAction::kStore)
    Add(&decision.evidence, "snapshot_cache_stored=true");
  if (action == SnapshotSafeCacheAction::kHit)
    Add(&decision.evidence, "snapshot_cache_identical_to_recompute=true");
  if (action == SnapshotSafeCacheAction::kInvalidateRecompute)
    Add(&decision.evidence, "snapshot_cache_invalidated=true");
  Add(&decision.evidence,
      "snapshot_cache_action=" +
          std::string(SnapshotSafeCacheActionName(action)));
  Add(&decision.evidence,
      "snapshot_cache_diagnostic=" + decision.diagnostic_code);
  if (!decision.diagnostic_detail.empty()) {
    Add(&decision.evidence,
        "snapshot_cache_detail=" + decision.diagnostic_detail);
  }
  AddBool(&decision.evidence, "snapshot_cache_hit", cache_hit);
  AddBool(&decision.evidence, "fail_closed", fail_closed);
  return decision;
}

SnapshotSafeCacheDecision Refuse(SnapshotSafeCacheDecision decision,
                                 std::string diagnostic_code,
                                 std::string detail) {
  return Finish(std::move(decision),
                SnapshotSafeCacheAction::kRefuse,
                std::move(diagnostic_code),
                std::move(detail),
                false,
                true,
                false);
}

void AddStoreBooleans(SnapshotSafeCacheDecision* decision,
                      const SnapshotSafeCacheStoreRequest& request) {
  AddBool(&decision->evidence, "executor_snapshot_result_cache_enabled",
          request.cache_enabled);
  AddBool(&decision->evidence, "read_only_operation",
          request.read_only_operation);
  AddBool(&decision->evidence, "candidate_set_snapshot_safe",
          request.candidate_set_snapshot_safe);
  AddBool(&decision->evidence, "small_final_result",
          request.small_final_result);
  AddBool(&decision->evidence, "negative_cache_entry",
          request.negative_cache_entry);
  AddBool(&decision->evidence, "negative_cache_snapshot_safe_proven",
          request.negative_cache_snapshot_safe_proven);
}

void AddLookupBooleans(SnapshotSafeCacheDecision* decision,
                       const SnapshotSafeCacheLookupRequest& request) {
  AddBool(&decision->evidence, "executor_snapshot_result_cache_enabled",
          request.cache_enabled);
  AddBool(&decision->evidence, "read_only_operation",
          request.read_only_operation);
  AddBool(&decision->evidence, "candidate_set_snapshot_safe",
          request.candidate_set_snapshot_safe);
  AddBool(&decision->evidence, "small_final_result",
          request.small_final_result);
  AddBool(&decision->evidence, "ordinary_recompute_available",
          request.ordinary_recompute_available);
  AddBool(&decision->evidence, "negative_cache_entry",
          request.negative_cache_entry);
  AddBool(&decision->evidence, "negative_cache_snapshot_safe_proven",
          request.negative_cache_snapshot_safe_proven);
}

}  // namespace

const char* SnapshotSafeCachePayloadKindName(
    SnapshotSafeCachePayloadKind kind) {
  switch (kind) {
    case SnapshotSafeCachePayloadKind::kCandidateSet:
      return "invalid";
    case SnapshotSafeCachePayloadKind::kSmallFinalResult:
      return "small_final_result";
  }
  return "candidate_set";
}

const char* SnapshotSafeCacheActionName(SnapshotSafeCacheAction action) {
  switch (action) {
    case SnapshotSafeCacheAction::kStore:
      return "store";
    case SnapshotSafeCacheAction::kHit:
      return "hit";
    case SnapshotSafeCacheAction::kMissRecompute:
      return "miss_recompute";
    case SnapshotSafeCacheAction::kInvalidateRecompute:
      return "invalidate_recompute";
    case SnapshotSafeCacheAction::kDisabledRecompute:
      return "disabled_recompute";
    case SnapshotSafeCacheAction::kRefuse:
      return "refuse";
  }
  return "refuse";
}

std::string SnapshotSafeCacheKeyText(const SnapshotSafeCacheKey& key) {
  return DigestBytes(EncodeCacheKey(key));
}

std::string SnapshotSafeCachePayloadDigest(const SnapshotSafeCachePayload& payload,
                                          SnapshotSafeCachePayloadKind kind) {
  return DigestBytes(PayloadBytes(payload, kind));
}

SnapshotSafeCacheDecision SnapshotSafeResultCache::Store(
    const SnapshotSafeCacheStoreRequest& request) {
  auto decision = BaseDecision(
      request.entry.key, request.entry.payload_kind,
      request.mga_authority.statement_context,
      request.selected_catalog_epoch_uuid);
  AddStoreBooleans(&decision, request);
  const auto binding = ValidateStatementBinding(
      request.mga_authority, request.selected_physical_dag,
      request.selected_catalog_epoch_uuid, request.entry.key);
  if (!binding.ok) {
    return Refuse(std::move(decision), binding.diagnostic_code,
                  binding.detail);
  }
  if (!request.cache_enabled) {
    return Finish(std::move(decision),
                  SnapshotSafeCacheAction::kDisabledRecompute,
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.DISABLED_RECOMPUTE",
                  "executor.snapshot_result_cache=off",
                  true,
                  false,
                  false);
  }
  if (!KeyComplete(request.entry.key)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.KEY_INCOMPLETE",
                  "strict cache key dimensions are required");
  }
  if (!request.read_only_operation ||
      !Eligible(request.entry.payload_kind,
                request.candidate_set_snapshot_safe,
                request.small_final_result,
                request.entry.row_count,
                request.max_small_result_rows)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.ELIGIBILITY_REFUSED",
                  "only read-only snapshot-safe candidates or small final results are cacheable");
  }
  std::string hard_code;
  std::string hard_detail;
  if (HardRefusal(request, &hard_code, &hard_detail)) {
    return Refuse(std::move(decision), std::move(hard_code),
                  std::move(hard_detail));
  }
  if (AnyUncertainty(request)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.UNCERTAINTY_REFUSED",
                  "DML DDL security redaction statistics provider route or visibility uncertainty");
  }
  if (!PhysicalMgaStatementContextEqual(
          request.entry.producing_statement_context,
          request.mga_authority.statement_context) ||
      request.entry.catalog_epoch_uuid !=
          request.selected_catalog_epoch_uuid) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.PRODUCING_CONTEXT_MISMATCH",
                  "entry must retain the exact producing statement context and catalog UUID");
  }
  if (!request.entry.entry_uuid.is_nil() || !request.entry.payload ||
      !PayloadValid(*request.entry.payload, request.entry.payload_kind, request.entry.row_count)) {
    return Refuse(std::move(decision), "EXECUTOR.SNAPSHOT_RESULT_CACHE.ELIGIBILITY_REFUSED",
                  "an actual typed payload and cache-issued entry identity are required");
  }
  const auto payload_bytes = PayloadBytes(*request.entry.payload, request.entry.payload_kind);
  const auto digest = DigestBytes(payload_bytes);
  if (!request.entry.cached_result_digest.empty() && request.entry.cached_result_digest != digest) {
    return Refuse(std::move(decision), "EXECUTOR.SNAPSHOT_RESULT_CACHE.DIGEST_REQUIRED",
                  "supplied content digest does not match the actual payload");
  }
  auto owned = request.entry;
  owned.cached_result_digest = digest;
  const auto issued = core::uuid::IssueRuntimeIdentityV7();
  if (!issued) throw std::runtime_error("snapshot cache runtime identity issuance failed");
  owned.entry_uuid = *issued;
  auto retained = std::make_shared<const SnapshotSafeCacheEntry>(std::move(owned));
  const auto entry_key = EntryKey(request.entry.key, request.entry.payload_kind,
                                 request.mga_authority.statement_context,
                                 request.selected_catalog_epoch_uuid);
  Add(&decision.evidence, "snapshot_cache_stored=true");
  auto staged = Finish(std::move(decision), SnapshotSafeCacheAction::kStore,
                       "EXECUTOR.SNAPSHOT_RESULT_CACHE.STORED",
                       "actual_typed_payload_retained", true, false, false);
  const auto final_binding = ValidateStatementBinding(
      request.mga_authority, request.selected_physical_dag,
      request.selected_catalog_epoch_uuid, request.entry.key);
  if (!final_binding.ok) return Refuse(std::move(staged), final_binding.diagnostic_code,
                                       final_binding.detail);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(entry_key);
  if (found != entries_.end()) {
    if (!found->second->payload || found->second->key != request.entry.key ||
        PayloadBytes(*found->second->payload, found->second->payload_kind) != payload_bytes) {
      return Refuse(std::move(staged), "EXECUTOR.SNAPSHOT_RESULT_CACHE.STORED_ENTRY_MISMATCH",
                    "same statement-bound key cannot silently replace different payload bytes");
    }
    return staged;
  }
  // Allocation failure in emplace leaves the cache unchanged. All response
  // allocations are already staged; publishing the shared owner is the commit.
  entries_.emplace(entry_key, std::move(retained));
  return staged;
}

SnapshotSafeCacheDecision SnapshotSafeResultCache::Lookup(
    const SnapshotSafeCacheLookupRequest& request) const {
  auto decision = BaseDecision(
      request.key, request.payload_kind,
      request.mga_authority.statement_context,
      request.selected_catalog_epoch_uuid);
  AddLookupBooleans(&decision, request);
  const auto binding = ValidateStatementBinding(
      request.mga_authority, request.selected_physical_dag,
      request.selected_catalog_epoch_uuid, request.key);
  if (!binding.ok) {
    return Refuse(std::move(decision), binding.diagnostic_code,
                  binding.detail);
  }
  if (!request.cache_enabled) {
    Add(&decision.evidence, "snapshot_cache_disabled_recompute=true");
    return Finish(std::move(decision),
                  SnapshotSafeCacheAction::kDisabledRecompute,
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.DISABLED_RECOMPUTE",
                  "executor.snapshot_result_cache=off",
                  true,
                  false,
                  false);
  }
  if (!KeyComplete(request.key)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.KEY_INCOMPLETE",
                  "strict cache key dimensions are required");
  }
  if (!request.read_only_operation ||
      !Eligible(request.payload_kind,
                request.candidate_set_snapshot_safe,
                request.small_final_result,
                request.row_count,
                request.max_small_result_rows)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.ELIGIBILITY_REFUSED",
                  "only read-only snapshot-safe candidates or small final results are cacheable");
  }
  std::string hard_code;
  std::string hard_detail;
  if (HardRefusal(request, &hard_code, &hard_detail)) {
    return Refuse(std::move(decision), std::move(hard_code),
                  std::move(hard_detail));
  }
  if (AnyUncertainty(request)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.UNCERTAINTY_REFUSED",
                  "DML DDL security redaction statistics provider route or visibility uncertainty");
  }
  if (!request.ordinary_recompute_available || !request.recomputed_payload ||
      !PayloadValid(*request.recomputed_payload, request.payload_kind, request.row_count)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.RECOMPUTE_PROOF_REQUIRED",
                  "actual ordinary-engine recomputed typed payload is required before a hit");
  }
  const auto recomputed_bytes = PayloadBytes(*request.recomputed_payload, request.payload_kind);
  const auto recomputed_digest = DigestBytes(recomputed_bytes);
  if (!request.recomputed_result_digest.empty() &&
      request.recomputed_result_digest != recomputed_digest) {
    return Refuse(std::move(decision), "EXECUTOR.SNAPSHOT_RESULT_CACHE.DIGEST_REQUIRED",
                  "supplied recomputation digest differs from actual payload bytes");
  }
  const auto entry_key = EntryKey(request.key, request.payload_kind,
                                 request.mga_authority.statement_context,
                                 request.selected_catalog_epoch_uuid);
  std::shared_ptr<const SnapshotSafeCacheEntry> entry;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(entry_key);
    if (found != entries_.end()) entry = found->second;
  }
  if (!entry) {
    Add(&decision.evidence, "snapshot_cache_miss_recompute=true");
    return Finish(std::move(decision),
                  SnapshotSafeCacheAction::kMissRecompute,
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.MISS_RECOMPUTE",
                  "cache_entry_missing_recompute_ordinary_engine_path",
                  true,
                  false,
                  false);
  }

  const bool result_match = entry->payload &&
      entry->cached_result_digest == recomputed_digest &&
      PayloadBytes(*entry->payload, entry->payload_kind) == recomputed_bytes;
  const bool payload_kind_match = entry->payload_kind == request.payload_kind;
  const bool row_count_match = entry->row_count == request.row_count;
  const bool producing_statement_context_match = PhysicalMgaStatementContextEqual(
      entry->producing_statement_context, request.mga_authority.statement_context);
  const bool catalog_epoch_uuid_match = entry->catalog_epoch_uuid == request.selected_catalog_epoch_uuid;
  const bool key_match = entry->key == request.key;
  AddBool(&decision.evidence, "snapshot_cache_payload_kind_match",
          payload_kind_match);
  AddBool(&decision.evidence, "snapshot_cache_row_count_match",
          row_count_match);
  AddBool(&decision.evidence, "snapshot_cache_recompute_result_match",
          result_match);
  AddBool(&decision.evidence, "snapshot_cache_statement_context_match",
          producing_statement_context_match);
  AddBool(&decision.evidence, "snapshot_cache_catalog_epoch_uuid_match",
          catalog_epoch_uuid_match);
  if (!payload_kind_match || !row_count_match ||
      !result_match || !key_match ||
      !producing_statement_context_match || !catalog_epoch_uuid_match) {
    Add(&decision.evidence, "snapshot_cache_invalidated=true");
    auto staged = Finish(std::move(decision), SnapshotSafeCacheAction::kInvalidateRecompute,
                         "EXECUTOR.SNAPSHOT_RESULT_CACHE.STORED_ENTRY_MISMATCH",
                         "stored typed payload or producing authority differs from recomputation",
                         true, false, false);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(entry_key);
    if (found != entries_.end() && found->second == entry) entries_.erase(found);
    return staged;
  }
  Add(&decision.evidence, "snapshot_cache_identical_to_recompute=true");
  auto staged = Finish(std::move(decision), SnapshotSafeCacheAction::kHit,
                       "EXECUTOR.SNAPSHOT_RESULT_CACHE.HIT_ACCEPTED",
                       "retained_typed_payload_matches_ordinary_engine_recompute",
                       true, false, true);
  const auto final_binding = ValidateStatementBinding(
      request.mga_authority, request.selected_physical_dag,
      request.selected_catalog_epoch_uuid, request.key);
  if (!final_binding.ok) return Refuse(std::move(staged), final_binding.diagnostic_code,
                                       final_binding.detail);
  staged.retained_entry = std::move(entry);
  return staged;
}

void SnapshotSafeResultCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

std::size_t SnapshotSafeResultCache::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

}  // namespace scratchbird::engine::executor
