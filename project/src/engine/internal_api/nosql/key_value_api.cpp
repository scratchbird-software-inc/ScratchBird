// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/key_value_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "batch_point_lookup.hpp"
#include "datatype_catalog_manifest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/nosql_surface_support.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

struct PhysicalKeyValueRecord {
  std::string key;
  std::string object_uuid;
  std::string row_uuid;
  std::string value;
  EngineApiU64 creator_tx = 0;
  EngineApiU64 expires_after_tx = 0;
};

using KeyValueMap = std::map<std::string, PhysicalKeyValueRecord>;

std::map<std::string, KeyValueMap>& PhysicalStores() {
  static std::map<std::string, KeyValueMap> stores;
  return stores;
}

std::string StoreKey(const EngineRequestContext& context) {
  if (!context.database_path.empty()) { return context.database_path; }
  if (!context.database_uuid.canonical.empty()) { return context.database_uuid.canonical; }
  return "embedded_transient_kv_provider";
}

std::string RequestKey(const EngineApiRequest& request,
                       const std::string& explicit_key) {
  if (!explicit_key.empty()) { return explicit_key; }
  if (!request.localized_names.empty() && !request.localized_names.front().name.empty()) {
    return request.localized_names.front().name;
  }
  if (!request.target_object.uuid.canonical.empty()) {
    return request.target_object.uuid.canonical;
  }
  return {};
}

std::string RowField(const EngineApiResult& result, const std::string& field) {
  if (result.result_shape.rows.empty()) { return {}; }
  for (const auto& [name, value] : result.result_shape.rows.front().fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

EngineNoSqlPhysicalProviderContract DefaultKvProviderContract() {
  EngineNoSqlPhysicalProviderContract contract;
  contract.family = EngineNoSqlProviderFamily::kKeyValue;
  contract.scope = EngineNoSqlProviderScope::kLocal;
  contract.provider_id = "nosql.local.kv_exact_prefix_provider";
  contract.fallback_provider_id = "none";
  contract.local_provider_available = true;
  contract.exact_fallback_available = false;
  contract.estimated_rows = 1;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.descriptor_visibility.proof_id = "kv-descriptor-visible";
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.security_redaction.proof_id = "kv-security-bound";
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = 1;
  contract.index_generation.available_generation = 1;
  contract.index_generation.index_uuid = "kv-exact-prefix-index";
  contract.index_generation.proof_id = "kv-index-generation:1";
  contract.delta_overlay.required = false;
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.policy.policy_snapshot_uuid = "kv-policy-snapshot";
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

EngineKeyValuePhysicalProof DefaultExactProof() {
  EngineKeyValuePhysicalProof proof;
  proof.provider_contract = DefaultKvProviderContract();
  proof.proof_supplied = true;
  proof.exact_key_index_proof = true;
  proof.ttl_visibility_proof = true;
  return proof;
}

EngineKeyValuePhysicalProof DefaultPrefixProof() {
  auto proof = DefaultExactProof();
  proof.prefix_index_proof = true;
  return proof;
}

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         const std::string& operation_id,
                         const char* diagnostic_code) {
  return MakeApiBehaviorDiagnostic<TResult>(
      context,
      operation_id,
      MakeInvalidRequestDiagnostic(operation_id, diagnostic_code));
}

void AddSelectionEvidence(const EngineNoSqlPhysicalProviderSelection& selection,
                          EngineApiResult* result) {
  for (const auto& item : selection.evidence) {
    AddApiBehaviorEvidence(result, "kv_physical_provider", item);
  }
}

template <typename TResult>
std::optional<TResult> ValidatePhysicalProof(const EngineApiRequest& request,
                                             const std::string& operation_id,
                                             const EngineKeyValuePhysicalProof& proof,
                                             bool require_exact,
                                             bool require_prefix) {
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  if (!proof.proof_supplied) {
    return DiagnosticResult<TResult>(
        request.context,
        operation_id,
        require_prefix ? kKeyValuePrefixProofMissing : kKeyValueExactKeyProofMissing);
  }
  if (!selection.selected) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id,
                                     selection.missing_diagnostics.empty()
                                         ? selection.refusal_diagnostics.front()
                                         : selection.missing_diagnostics.front()));
    AddSelectionEvidence(selection, &failure);
    return failure;
  }
  if (require_exact && !proof.exact_key_index_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kKeyValueExactKeyProofMissing);
  }
  if (require_prefix && !proof.prefix_index_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kKeyValuePrefixProofMissing);
  }
  if (!proof.ttl_visibility_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kKeyValueTtlVisibilityProofMissing);
  }
  return std::nullopt;
}

void AddKvEvidence(EngineApiResult* result,
                   const EngineNoSqlPhysicalProviderSelection& selection,
                   const std::string& access_kind) {
  AddEngineNoSqlSurfaceEvidence(result, "key_value", access_kind);
  AddSelectionEvidence(selection, result);
  AddApiBehaviorEvidence(result, "kv_physical_access", access_kind);
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "ttl_visibility_evidence", "deterministic_local_transaction_id");
  AddApiBehaviorEvidence(result, "mga_finality_authority", "engine_transaction_inventory");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
}

bool VisibleByTtl(const PhysicalKeyValueRecord& record, EngineApiU64 observer_tx) {
  return record.expires_after_tx == 0 || observer_tx < record.expires_after_tx;
}

template <typename TResult>
void AddKvRow(TResult* result, const PhysicalKeyValueRecord& record) {
  AddApiBehaviorRow(result,
                    {{"surface", "key_value"},
                     {"key_uuid", record.object_uuid},
                     {"row_uuid", record.row_uuid},
                     {"key", record.key},
                     {"state", "active"},
                     {"value", record.value},
                     {"ttl_expires_after_tx", std::to_string(record.expires_after_tx)}});
}

scratchbird::core::platform::TypedUuid ParseStoredRowUuid(
    const std::string& row_uuid) {
  const auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
      scratchbird::core::platform::UuidKind::row, row_uuid);
  return parsed.ok() ? parsed.value
                     : scratchbird::core::platform::TypedUuid{};
}

scratchbird::core::index::CandidateSetAuthorityContext
BatchLookupAuthorityFromSelection(
    const EngineNoSqlPhysicalProviderSelection& selection) {
  scratchbird::core::index::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = selection.selected;
  authority.security_context_bound =
      selection.selected && selection.row_security_recheck_required;
  authority.row_mga_recheck_required = selection.row_mga_recheck_required;
  authority.row_security_recheck_required =
      selection.row_security_recheck_required;
  authority.exact_recheck_available = selection.selected;
  authority.exact_rerank_source_available = selection.selected;
  authority.provider_finality_or_visibility_authority =
      selection.provider_transaction_finality_authority ||
      selection.provider_visibility_authority ||
      selection.index_transaction_finality_authority ||
      selection.delta_overlay_transaction_finality_authority;
  authority.parser_or_reference_finality_or_visibility_authority =
      selection.parser_transaction_finality_authority;
  authority.wal_recovery_or_finality_authority =  // wal-not-authority
      selection.write_ahead_log_transaction_finality_authority;  // wal-not-authority
  return authority;
}

scratchbird::core::index::BatchPointLookupPlan MakeKvBatchLookupPlan(
    const std::vector<std::string>& keys,
    const std::string& operation_id,
    const EngineNoSqlPhysicalProviderSelection& selection) {
  scratchbird::core::index::BatchPointLookupPlan plan;
  plan.purpose = scratchbird::core::index::BatchPointLookupPurpose::key_value;
  plan.plan_id = operation_id + ":kv_batch_point_lookup";
  plan.cluster_route_requested = false;
  plan.cluster_guard_checked = true;
  plan.cluster_provider_authorized = false;
  plan.caller_evidence = selection.evidence;
  plan.keys.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    plan.keys.push_back({keys[i], static_cast<EngineApiU64>(i)});
  }
  return plan;
}

void AddKvLookupRow(EngineApiResult* result,
                    const scratchbird::core::index::BatchPointLookupRow& row) {
  std::vector<std::pair<std::string, std::string>> fields = {
      {"surface", "key_value"},
      {"row_uuid", scratchbird::core::uuid::UuidToString(row.row_uuid.value)},
      {"key", row.encoded_key},
      {"state", "active"},
      {"value", row.payload},
      {"duplicate_key", row.duplicate_key ? "true" : "false"},
      {"duplicate_ordinal", std::to_string(row.duplicate_ordinal)}};
  for (const auto& [name, value] : row.attributes) {
    fields.push_back({name, value});
  }
  AddApiBehaviorRow(result, std::move(fields));
}

void AddKvBatchLookupEvidence(
    EngineApiResult* result,
    const scratchbird::core::index::BatchPointLookupResult& lookup) {
  for (const auto& item : lookup.evidence) {
    AddApiBehaviorEvidence(result, "batch_point_lookup", item);
  }
  for (const auto& miss : lookup.misses) {
    AddApiBehaviorEvidence(result,
                           "batch_point_lookup_miss",
                           std::to_string(miss.input_ordinal) + ":" +
                               miss.encoded_key + ":" + miss.reason);
  }
}

template <typename TResult, typename TRequest>
std::optional<TResult> AddKvBatchLookupRowsFromStore(
    const TRequest& request,
    const std::string& operation_id,
    const std::vector<std::string>& keys,
    const EngineNoSqlPhysicalProviderSelection& selection,
    const KeyValueMap& lookup_store,
    TResult* result) {
  auto plan = MakeKvBatchLookupPlan(keys, operation_id, selection);
  auto authority = BatchLookupAuthorityFromSelection(selection);
  auto lookup = scratchbird::core::index::RunBatchPointLookup(
      plan,
      authority,
      [&lookup_store, &request](
          const scratchbird::core::index::BatchPointLookupProviderRequest&
              provider_request) {
        scratchbird::core::index::BatchPointLookupProviderResult provider_result;
        provider_result.status = {scratchbird::core::platform::StatusCode::ok,
                                  scratchbird::core::platform::Severity::info,
                                  scratchbird::core::platform::Subsystem::engine};
        provider_result.evidence.push_back(
            "batch_point_lookup.provider=kv_exact_key_index");
        provider_result.evidence.push_back(
            "batch_point_lookup.provider.transaction_finality_authority=false");
        for (const auto& key : provider_request.ordered_unique_keys) {
          const auto it = lookup_store.find(key.encoded_key);
          if (it == lookup_store.end() ||
              !VisibleByTtl(it->second,
                            request.context.local_transaction_id)) {
            continue;
          }
          scratchbird::core::index::BatchPointLookupProviderRow row;
          row.encoded_key = key.encoded_key;
          row.candidate.row_uuid = ParseStoredRowUuid(it->second.row_uuid);
          row.candidate.exact_predicate_match = true;
          row.candidate.mga_visible = true;
          row.candidate.security_authorized = true;
          row.candidate.exact_payload_available = true;
          row.candidate.source = "nosql.key_value";
          row.exact_row_uuid = row.candidate.row_uuid.valid();
          row.payload = it->second.value;
          row.attributes.push_back({"key_uuid", it->second.object_uuid});
          row.attributes.push_back(
              {"ttl_expires_after_tx",
               std::to_string(it->second.expires_after_tx)});
          provider_result.rows.push_back(std::move(row));
        }
        return provider_result;
      });
  if (!lookup.ok()) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id,
                                     lookup.diagnostic.diagnostic_code));
    AddKvBatchLookupEvidence(&failure, lookup);
    return failure;
  }
  AddKvBatchLookupEvidence(result, lookup);
  for (const auto& row : lookup.rows) {
    AddKvLookupRow(result, row);
  }
  AddApiBehaviorEvidence(result,
                         "kv_ordered_batch_lookup_primitive",
                         "ODF-092");
  return std::nullopt;
}

template <typename TResult, typename TRequest>
std::optional<TResult> AddKvBatchLookupRows(
    const TRequest& request,
    const std::string& operation_id,
    const std::vector<std::string>& keys,
    const EngineNoSqlPhysicalProviderSelection& selection,
    TResult* result) {
  const auto& store = PhysicalStores()[StoreKey(request.context)];
  return AddKvBatchLookupRowsFromStore(
      request, operation_id, keys, selection, store, result);
}

std::string RequestPayloadValue(const EngineKeyValuePutRequest& request,
                                const EngineKeyValuePutResult& result) {
  if (!request.value.empty()) { return request.value; }
  return RowField(result, "payload");
}

void UpsertPhysicalRecord(const EngineKeyValuePutRequest& request,
                          const EngineKeyValuePutResult& result) {
  PhysicalKeyValueRecord record;
  record.key = RequestKey(request, request.key);
  record.object_uuid = result.primary_object.uuid.canonical;
  record.row_uuid = result.catalog_row_uuid.canonical;
  if (record.row_uuid.empty()) { record.row_uuid = GenerateCrudEngineUuid("row"); }
  record.value = RequestPayloadValue(request, result);
  record.creator_tx = request.context.local_transaction_id;
  record.expires_after_tx = request.expires_after_local_transaction_id;
  const auto option_ttl =
      EngineNoSqlOptionU64(request, "kv.ttl.expires_after_tx");
  if (option_ttl.first) { record.expires_after_tx = option_ttl.second; }
  const auto logical_key = record.key;
  if (!logical_key.empty()) {
    auto& stored = PhysicalStores()[StoreKey(request.context)];
    stored[logical_key] = std::move(record);
    const auto canonical_key = result.primary_object.uuid.canonical;
    if (!canonical_key.empty() && canonical_key != logical_key) {
      auto alias = stored[logical_key];
      alias.key = canonical_key;
      stored[canonical_key] = std::move(alias);
    }
  }
}

EngineKeyValueGetResult PhysicalGet(const EngineKeyValueGetRequest& request,
                                    const std::string& operation_id,
                                    const EngineKeyValuePhysicalProof& proof,
                                    bool prefix) {
  if (auto failure = ValidatePhysicalProof<EngineKeyValueGetResult>(
          request, operation_id, proof, !prefix, prefix)) {
    return *failure;
  }
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  auto result = MakeApiBehaviorSuccess<EngineKeyValueGetResult>(
      request.context, operation_id);
  AddKvEvidence(&result, selection, prefix ? "prefix_index_probe" : "exact_key_index_probe");

  const auto& store = PhysicalStores()[StoreKey(request.context)];
  if (!prefix) {
    const auto it = store.find(RequestKey(request, request.key));
    if (it != store.end() && VisibleByTtl(it->second, request.context.local_transaction_id)) {
      AddKvRow(&result, it->second);
    }
  } else {
    const std::string prefix_key = request.prefix;
    for (const auto& [key, record] : store) {
      if (key.rfind(prefix_key, 0) == 0 &&
          VisibleByTtl(record, request.context.local_transaction_id)) {
        AddKvRow(&result, record);
      }
    }
  }
  result.dml_summary.index_probes = prefix ? store.size() : 1;
  result.dml_summary.visible_rows_scanned = 0;
  AddApiBehaviorEvidence(&result, "kv_rows_returned",
                         std::to_string(result.result_shape.rows.size()));
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_KEY_VALUE_API_BEHAVIOR
EngineKeyValueGetResult EngineKeyValueGet(const EngineKeyValueGetRequest& request) {
  constexpr const char* kOperation = "nosql.key_value_get";
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineKeyValueGetResult>(
        request, kOperation);
  }
  EngineKeyValuePhysicalProof proof =
      request.physical_proof.proof_supplied
          ? request.physical_proof
          : (request.prefix.empty() ? DefaultExactProof() : DefaultPrefixProof());
  return PhysicalGet(request, kOperation, proof, !request.prefix.empty());
}

EngineKeyValuePutResult EngineKeyValuePut(const EngineKeyValuePutRequest& request) {
  constexpr const char* kOperation = "nosql.key_value_put";
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineKeyValuePutResult>(
        request, kOperation);
  }
  auto result = EngineNoSqlPayloadAwarePersistedWriteResult<EngineKeyValuePutResult>(
      request, kOperation, "key_value", true, "active");
  if (result.ok) {
    UpsertPhysicalRecord(request, result);
    AddEngineNoSqlSurfaceEvidence(&result, "key_value", "physical_provider_put");
    AddApiBehaviorEvidence(&result, "kv_physical_provider", "provider_family=key_value");
    AddApiBehaviorEvidence(&result, "kv_physical_access", "write_through_exact_prefix_provider");
    AddApiBehaviorEvidence(&result, "mga_finality_authority", "engine_transaction_inventory");
    AddApiBehaviorEvidence(&result, "client_autocommit_authority", "false");
    result.dml_summary.rows_changed = 1;
  }
  return result;
}

EngineKeyValueMultiGetResult EngineKeyValueMultiGet(
    const EngineKeyValueMultiGetRequest& request) {
  constexpr const char* kOperation = "nosql.key_value_multiget";
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineKeyValueMultiGetResult>(
        request, kOperation);
  }
  const auto proof =
      request.physical_proof.proof_supplied ? request.physical_proof : DefaultExactProof();
  if (auto failure = ValidatePhysicalProof<EngineKeyValueMultiGetResult>(
          request, kOperation, proof, true, false)) {
    return *failure;
  }
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  auto result = MakeApiBehaviorSuccess<EngineKeyValueMultiGetResult>(
      request.context, kOperation);
  AddKvEvidence(&result, selection, "multiget_exact_key_index_probe");
  if (auto failure = AddKvBatchLookupRows(
          request, kOperation, request.keys, selection, &result)) {
    return *failure;
  }
  result.dml_summary.index_probes = request.keys.size();
  AddApiBehaviorEvidence(&result, "kv_multiget_keys", std::to_string(request.keys.size()));
  AddApiBehaviorEvidence(&result, "kv_rows_returned",
                         std::to_string(result.result_shape.rows.size()));
  return result;
}

EngineKeyValuePipelineResult EngineKeyValuePipeline(
    const EngineKeyValuePipelineRequest& request) {
  constexpr const char* kOperation = "nosql.key_value_pipeline";
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineKeyValuePipelineResult>(
        request, kOperation);
  }
  const auto operation_count = request.puts.size() + request.get_keys.size();
  if (request.max_admitted_operations != 0 &&
      operation_count > request.max_admitted_operations) {
    return DiagnosticResult<EngineKeyValuePipelineResult>(
        request.context, kOperation, kKeyValuePipelineAdmissionRefused);
  }
  const auto proof =
      request.physical_proof.proof_supplied ? request.physical_proof : DefaultExactProof();
  if (auto failure = ValidatePhysicalProof<EngineKeyValuePipelineResult>(
          request, kOperation, proof, true, false)) {
    return *failure;
  }
  if (request.context.local_transaction_id == 0 && !request.puts.empty()) {
    return MakeApiBehaviorDiagnostic<EngineKeyValuePipelineResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "local_transaction_id_required"));
  }
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  auto result = MakeApiBehaviorSuccess<EngineKeyValuePipelineResult>(
      request.context, kOperation);
  AddKvEvidence(&result, selection, "pipeline_batch_exact_key_provider");
  auto& store = PhysicalStores()[StoreKey(request.context)];
  auto staged_store = store;
  EngineApiU64 staged_rows_changed = 0;
  for (const auto& put : request.puts) {
    PhysicalKeyValueRecord record;
    record.key = put.key;
    record.object_uuid = put.key;
    record.row_uuid = GenerateCrudEngineUuid("row");
    record.value = put.value;
    record.creator_tx = request.context.local_transaction_id;
    record.expires_after_tx = put.expires_after_local_transaction_id;
    staged_store[record.key] = record;
    ++staged_rows_changed;
  }
  if (auto failure = AddKvBatchLookupRowsFromStore(
          request, kOperation, request.get_keys, selection, staged_store,
          &result)) {
    return *failure;
  }
  store = std::move(staged_store);
  result.dml_summary.rows_changed = staged_rows_changed;
  result.dml_summary.index_probes = request.get_keys.size();
  AddApiBehaviorEvidence(&result, "kv_pipeline_admitted_operations",
                         std::to_string(operation_count));
  return result;
}

EngineKeyValueAtomicProgramResult EngineKeyValueAtomicProgram(
    const EngineKeyValueAtomicProgramRequest& request) {
  constexpr const char* kOperation = "nosql.key_value_atomic_program";
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineKeyValueAtomicProgramResult>(
        request, kOperation);
  }
  if (request.context.local_transaction_id == 0) {
    return MakeApiBehaviorDiagnostic<EngineKeyValueAtomicProgramResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "local_transaction_id_required"));
  }
  const auto proof =
      request.physical_proof.proof_supplied ? request.physical_proof : DefaultExactProof();
  if (auto failure = ValidatePhysicalProof<EngineKeyValueAtomicProgramResult>(
          request, kOperation, proof, true, false)) {
    return *failure;
  }
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  auto result = MakeApiBehaviorSuccess<EngineKeyValueAtomicProgramResult>(
      request.context, kOperation);
  AddKvEvidence(&result, selection, "sblr_atomic_read_compute_write");
  auto& store = PhysicalStores()[StoreKey(request.context)];
  for (const auto& step : request.steps) {
    auto& record = store[step.key];
    record.key = step.key;
    record.object_uuid = step.key;
    if (record.row_uuid.empty()) { record.row_uuid = GenerateCrudEngineUuid("row"); }
    record.creator_tx = request.context.local_transaction_id;
    if (step.opcode == "set") {
      record.value = step.operand;
    } else if (step.opcode == "append") {
      record.value += step.operand;
    } else if (step.opcode == "increment_i64") {
      try {
        const auto current = record.value.empty() ? 0 : std::stoll(record.value);
        record.value = std::to_string(current + std::stoll(step.operand));
      } catch (...) {
        return DiagnosticResult<EngineKeyValueAtomicProgramResult>(
            request.context, kOperation, kKeyValueAtomicProgramRefused);
      }
    } else {
      return DiagnosticResult<EngineKeyValueAtomicProgramResult>(
          request.context, kOperation, kKeyValueAtomicProgramRefused);
    }
    AddKvRow(&result, record);
    ++result.dml_summary.rows_changed;
  }
  AddApiBehaviorEvidence(&result, "kv_atomic_program", "deterministic_sblr_read_compute_write");
  AddApiBehaviorEvidence(&result, "parser_transaction_finality_authority", "false");
  return result;
}

namespace {

constexpr const char* kBoundKeyValueOperation =
    "model.key_value.read.v1";

EngineBoundKeyValueReadResultV1 BoundKeyValueRefusal(
    const EngineBoundKeyValueReadRequestV1& request,
    const char* diagnostic_id,
    std::string detail) {
  auto result = MakeApiBehaviorDiagnostic<EngineBoundKeyValueReadResultV1>(
      request.context, kBoundKeyValueOperation,
      MakeEngineApiDiagnostic(diagnostic_id,
                              "engine.model.key_value.read.refused",
                              std::move(detail)));
  result.rows.clear();
  result.result_shape = {};
  return result;
}

bool CheckedAddU64(const std::uint64_t value, std::uint64_t* total) {
  if (total == nullptr ||
      value > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

bool CanonicalKeyValueUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto byte = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(byte) || std::isupper(byte)) return false;
  }
  return true;
}

bool WellFormedUtf8(const std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::uint32_t code_point = 0;
    std::size_t continuation_count = 0;
    if (first <= 0x7f) {
      code_point = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code_point = first & 0x1f;
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      code_point = first & 0x0f;
      continuation_count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code_point = first & 0x07;
      continuation_count = 3;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1) return false;
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto next = static_cast<unsigned char>(value[offset + index]);
      if ((next & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

bool CanonicalKeyValueTimestamp(const std::string_view value,
                                const bool stored_expiry) {
  if (stored_expiry) {
    if (value.size() != 30 || value[19] != '.') return false;
  } else if (value.size() != 20 &&
             (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigits[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigits) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](const std::size_t begin,
                           const std::size_t count) {
    unsigned out = 0;
    for (std::size_t index = 0; index < count; ++index) {
      out = out * 10 + static_cast<unsigned>(value[begin + index] - '0');
    }
    return out;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDays[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDays[month];
  if (month == 2 &&
      ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    ++maximum_day;
  }
  return day != 0 && day <= maximum_day;
}

bool ExactKeyValueValueDescriptor(const EngineDescriptor& descriptor,
                                  const std::string_view expected_type,
                                  const std::string_view expected_type_uuid,
                                  const bool expected_nullable) {
  if (!QowCanonicalDescriptorIdentityV1(descriptor) ||
      descriptor.descriptor_kind != "canonical_type_descriptor" ||
      descriptor.canonical_type_name != expected_type) {
    return false;
  }
  std::map<std::string_view, std::string_view> fields;
  const auto encoded = std::string_view(descriptor.encoded_descriptor);
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto end = encoded.find(';', offset);
    const auto field = encoded.substr(
        offset, end == std::string_view::npos ? std::string_view::npos
                                              : end - offset);
    const auto equal = field.find('=');
    if (field.empty() || equal == std::string_view::npos || equal == 0 ||
        equal + 1 == field.size() ||
        !fields.emplace(field.substr(0, equal), field.substr(equal + 1)).second)
      return false;
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return fields.size() == 3 && fields.contains("canonical") &&
         fields.contains("type_uuid") && fields.contains("nullable") &&
         fields.at("canonical") == expected_type &&
         fields.at("type_uuid") == expected_type_uuid &&
         fields.at("nullable") ==
             (expected_nullable ? "true" : "false");
}

bool ExactKeyValueStorageDescriptor(
    const MgaRelationStorageDescriptor& descriptor) {
  static constexpr std::array<std::string_view, 3> kNames{
      "key", "value", "expires_at"};
  static constexpr std::array<std::string_view, 3> kTypes{
      "text", "text", "timestamp_tz"};
  static constexpr std::array<bool, 3> kNullable{false, false, true};
  if (descriptor.columns.size() != kNames.size()) return false;
  const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return false;
  std::unordered_set<std::string> column_uuids;
  std::unordered_set<std::string> descriptor_uuids;
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto& column = descriptor.columns[ordinal];
    // TIMESTAMP_TZ is the signed key/value storage semantic carried by the
    // canonical engine timestamp catalog identity.  Resolve that identity
    // explicitly rather than treating the carrier spelling as a new type.
    const auto type_id = kTypes[ordinal] == "timestamp_tz"
                             ? scratchbird::core::datatypes::CanonicalTypeId::timestamp
                             : scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
                                   std::string(kTypes[ordinal]));
    const auto type_row = scratchbird::core::datatypes::LookupDatatypeCatalogRow(
        manifest.manifest, type_id);
    if (!type_row.ok() || type_row.manifest.descriptor_rows.size() != 1 ||
        !type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
      return false;
    }
    const auto expected_type_uuid = scratchbird::core::uuid::UuidToString(
        type_row.manifest.descriptor_rows.front().descriptor_uuid.value);
    if (column.ordinal != ordinal ||
        column.canonical_name_key != kNames[ordinal] ||
        column.value_descriptor.canonical_type_name != kTypes[ordinal] ||
        column.nullable != kNullable[ordinal] || column.generated ||
        column.identity_column || column.storage_class != "inline_row_value" ||
        column.max_inline_bytes != 4096 ||
        column.overflow_policy != "mga_large_value_locator" ||
        !CanonicalKeyValueUuid(column.column_uuid.canonical) ||
        !CanonicalKeyValueUuid(
            column.value_descriptor.descriptor_uuid.canonical) ||
        !column_uuids.insert(column.column_uuid.canonical).second ||
        !descriptor_uuids
             .insert(column.value_descriptor.descriptor_uuid.canonical)
             .second ||
        !ExactKeyValueValueDescriptor(column.value_descriptor,
                                      kTypes[ordinal], expected_type_uuid,
                                      kNullable[ordinal])) {
      return false;
    }
  }
  return true;
}

std::string NormalizeStatementTimestamp(const std::string_view value) {
  if (!CanonicalKeyValueTimestamp(value, false)) return {};
  if (value.size() == 20) {
    return std::string(value.substr(0, 19)) + ".000000000Z";
  }
  const auto digits = value.size() - 21;
  std::string normalized(value.substr(0, value.size() - 1));
  normalized.append(9 - digits, '0');
  normalized.push_back('Z');
  return normalized;
}

bool UnsignedUtf8Less(const std::string& left, const std::string& right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](const char l, const char r) {
        return static_cast<unsigned char>(l) <
               static_cast<unsigned char>(r);
      });
}

}  // namespace

EngineBoundKeyValueReadResultV1 EngineBoundKeyValueReadV1(
    const EngineBoundKeyValueReadRequestV1& request) {
  bool data_access_observed = false;
  const auto refuse = [&](const char* diagnostic, std::string detail) {
    auto result =
        BoundKeyValueRefusal(request, diagnostic, std::move(detail));
    result.data_access_observed = data_access_observed;
    return result;
  };
  const auto cancelled = [&]() {
    try {
      return request.cancellation_requested &&
             request.cancellation_requested();
    } catch (...) {
      return true;
    }
  };
  const bool get =
      request.operation == EngineBoundKeyValueReadOperationV1::kGet;
  const bool multi_get =
      request.operation == EngineBoundKeyValueReadOperationV1::kMultiGet;
  const bool prefix =
      request.operation == EngineBoundKeyValueReadOperationV1::kPrefixRange;
  if (request.abi_version != 1 || (!get && !multi_get && !prefix) ||
      !CanonicalKeyValueUuid(request.object_uuid) ||
      request.expected_descriptor_generation == 0 ||
      !CanonicalKeyValueUuid(request.expected_descriptor_uuid) ||
      !CanonicalKeyValueUuid(request.selected_alternative_uuid) ||
      !CanonicalKeyValueUuid(request.capability_uuid) ||
      !CanonicalKeyValueUuid(request.provider_uuid) ||
      request.provider_generation == 0 ||
      !request.cancellation_requested) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "engine-bound key/value request identity is incomplete");
  }
  if (request.statement_timestamp.empty() ||
      request.statement_timestamp != request.context.statement_timestamp ||
      NormalizeStatementTimestamp(request.statement_timestamp).empty()) {
    return refuse("SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1",
                  "engine-bound key/value statement timestamp is invalid");
  }
  if (request.maximum_request_keys == 0 ||
      request.maximum_request_bytes == 0 ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_value_bytes == 0 ||
      request.maximum_result_bytes == 0 ||
      request.maximum_memory_bytes == 0) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "key/value resource contract is incomplete");
  }
  if ((get || prefix) && request.request_values.size() != 1) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "key/value operation operand cardinality is invalid");
  }
  if (multi_get && request.request_values.empty()) {
    return refuse("SB_MODEL_KEY_VALUE_MULTI_GET_EMPTY_REFUSED_V1",
                  "key/value multi-get requires at least one key");
  }
  if (request.request_values.size() > request.maximum_request_keys) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "key/value request-key count exceeded its bound");
  }

  std::uint64_t request_bytes = 0;
  std::vector<std::string> first_distinct_values;
  std::unordered_set<std::string> distinct_values;
  first_distinct_values.reserve(request.request_values.size());
  for (const auto& value : request.request_values) {
    if (value.state != EngineValueState::value || value.is_null ||
        value.descriptor.canonical_type_name != "text" ||
        !value.binary_value.empty()) {
      return refuse("SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1",
                    "key/value request operand is not non-null TEXT");
    }
    if (!WellFormedUtf8(value.encoded_value)) {
      return refuse("SB_MODEL_KEY_VALUE_TEXT_INVALID_V1",
                    "key/value request operand is malformed UTF-8");
    }
    if (!CheckedAddU64(value.encoded_value.size(), &request_bytes) ||
        request_bytes > request.maximum_request_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "key/value request bytes exceeded their bound");
    }
    if (distinct_values.insert(value.encoded_value).second) {
      first_distinct_values.push_back(value.encoded_value);
    }
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value request normalization was cancelled");
    }
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "key/value execution was cancelled before data access");
  }
  const auto authorization = EvaluateMaterializedAuthorization(
      request.context, request.context.authorization_context, "SELECT",
      request.object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "key/value SELECT authorization was refused");
  }

  MgaVisibleHeapRelationReadRequest read_request;
  read_request.relation_uuid = request.object_uuid;
  read_request.maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  read_request.maximum_decoded_bytes = request.maximum_decoded_bytes;
  read_request.maximum_output_rows = request.maximum_output_rows;
  read_request.cancellation_requested = request.cancellation_requested;
  data_access_observed = true;
  const auto read = ReadVisibleMgaHeapRelation(request.context, read_request);
  if (!read.ok) {
    if (read.cancellation_observed || cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value MGA-visible row read was cancelled");
    }
    const auto detail = read.diagnostic.detail.empty()
                            ? std::string(
                                  "bounded key/value MGA-visible row read failed")
                            : read.diagnostic.detail;
    const auto resource_failure =
        detail.find("maximum") != std::string::npos ||
        detail.find("resource") != std::string::npos ||
        detail.find("bound") != std::string::npos ||
        detail.find("decoded") != std::string::npos ||
        detail.find("overflow") != std::string::npos ||
        detail.find("large_value") != std::string::npos;
    const auto mga_failure =
        detail.find("transaction") != std::string::npos ||
        detail.find("snapshot") != std::string::npos ||
        detail.find("visibility") != std::string::npos;
    return refuse(resource_failure
                      ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                      : mga_failure ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                                    : "SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  detail);
  }

  EngineBoundKeyValueReadResultV1 result =
      MakeApiBehaviorSuccess<EngineBoundKeyValueReadResultV1>(
          request.context, kBoundKeyValueOperation);
  result.data_access_observed = true;
  result.exact_fallback_observed = request.exact_fallback_selected;
  result.scanned_row_version_count = read.scanned_row_version_count;
  result.selected_visible_row_count = read.visible_rows.size();
  result.descriptor_uuid = read.descriptor.descriptor_uuid.canonical;
  result.descriptor_generation = read.descriptor.descriptor_generation;
  result.selected_alternative_uuid = request.selected_alternative_uuid;
  result.capability_uuid = request.capability_uuid;
  result.provider_uuid = request.provider_uuid;
  result.provider_generation = request.provider_generation;
  if (result.descriptor_uuid != request.expected_descriptor_uuid ||
      result.descriptor_generation !=
          request.expected_descriptor_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "key/value relation descriptor generation changed");
  }

  if (!ExactKeyValueStorageDescriptor(read.descriptor)) {
    return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                  "key/value storage descriptor is not key/value/expires_at");
  }

  struct VisibleRow {
    EngineBoundKeyValueRowV1 public_row;
    std::optional<std::string> expires_at;
  };
  std::vector<VisibleRow> visible_rows;
  visible_rows.reserve(read.visible_rows.size());
  std::unordered_set<std::string> visible_keys;
  std::uint64_t retained_value_bytes = 0;
  const auto normalized_statement =
      NormalizeStatementTimestamp(request.statement_timestamp);
  for (const auto& row : read.visible_rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value visible-row validation was cancelled");
    }
    if (!CanonicalKeyValueUuid(row.row_uuid)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "key/value row identity is not canonical");
    }
    if (row.values.size() != read.descriptor.columns.size()) {
      return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                    "key/value stored row width differs from its descriptor");
    }
    std::unordered_map<std::string, std::string> fields;
    for (const auto& [name, encoded] : row.values) {
      if (!fields.emplace(name, encoded).second) {
        return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                      "key/value stored row has a duplicated field");
      }
    }
    const auto stored_key = fields.find("key");
    const auto stored_value = fields.find("value");
    const auto stored_expiry = fields.find("expires_at");
    if (stored_key == fields.end() || stored_value == fields.end() ||
        stored_expiry == fields.end() || fields.size() != 3 ||
        stored_key->second == "<NULL>" || stored_value->second == "<NULL>") {
      return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                    "key/value stored row has null or unknown fields");
    }
    if (!WellFormedUtf8(stored_key->second) ||
        !WellFormedUtf8(stored_value->second)) {
      return refuse("SB_MODEL_KEY_VALUE_TEXT_INVALID_V1",
                    "key/value stored row contains malformed UTF-8");
    }
    if (!visible_keys.insert(stored_key->second).second) {
      return refuse("SB_MODEL_KEY_VALUE_DUPLICATE_VISIBLE_KEY_REFUSED_V1",
                    "more than one MGA-visible logical row has the same key");
    }
    VisibleRow selected;
    selected.public_row = {row.row_uuid, stored_key->second,
                           stored_value->second};
    if (stored_expiry->second != "<NULL>") {
      if (!CanonicalKeyValueTimestamp(stored_expiry->second, true)) {
        return refuse("SB_MODEL_KEY_VALUE_EXPIRES_AT_INVALID_V1",
                      "key/value expires_at is not canonical TIMESTAMP_TZ");
      }
      selected.expires_at = stored_expiry->second;
    }
    if (!CheckedAddU64(selected.public_row.key.size(),
                       &retained_value_bytes) ||
        !CheckedAddU64(selected.public_row.value.size(),
                       &retained_value_bytes) ||
        retained_value_bytes > request.maximum_value_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "key/value selected value bytes exceeded their bound");
    }
    visible_rows.push_back(std::move(selected));
  }

  std::unordered_map<std::string, const VisibleRow*> unexpired;
  unexpired.reserve(visible_rows.size());
  for (const auto& row : visible_rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value TTL evaluation was cancelled");
    }
    if (!row.expires_at.has_value() ||
        normalized_statement < *row.expires_at) {
      unexpired.emplace(row.public_row.key, &row);
    }
  }

  std::vector<EngineBoundKeyValueRowV1> output;
  if (get) {
    result.ordering_id = "key_value_unordered_v1";
    const auto found = unexpired.find(first_distinct_values.front());
    if (found != unexpired.end()) output.push_back(found->second->public_row);
  } else if (multi_get) {
    result.ordering_id = "first_distinct_request_order_v1";
    for (const auto& key : first_distinct_values) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "key/value multi-get lookup was cancelled");
      }
      const auto found = unexpired.find(key);
      if (found != unexpired.end()) output.push_back(found->second->public_row);
    }
  } else {
    result.ordering_id = "key_utf8_byte_ascending_v1";
    const auto& requested_prefix = first_distinct_values.front();
    for (const auto& [key, row] : unexpired) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "key/value prefix scan was cancelled");
      }
      if (key.starts_with(requested_prefix)) output.push_back(row->public_row);
    }
    std::ranges::sort(output, [](const auto& left, const auto& right) {
      return UnsignedUtf8Less(left.key, right.key);
    });
  }

  std::uint64_t result_bytes = 0;
  std::uint64_t memory_bytes = sizeof(result);
  if (output.size() > request.maximum_output_rows) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "key/value output row count exceeded its bound");
  }
  for (const auto& row : output) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "key/value result materialization was cancelled");
    }
    const auto row_bytes = static_cast<std::uint64_t>(
        row.row_uuid.size() + row.key.size() + row.value.size() + 3);
    if (!CheckedAddU64(row_bytes, &result_bytes) ||
        !CheckedAddU64(sizeof(EngineBoundKeyValueRowV1), &memory_bytes) ||
        !CheckedAddU64(row_bytes, &memory_bytes) ||
        result_bytes > request.maximum_result_bytes ||
        memory_bytes > request.maximum_memory_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "key/value atomic result preflight exceeded its bound");
    }
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "key/value execution was cancelled before publication");
  }
  result.result_byte_count = result_bytes;
  result.rows = std::move(output);
  result.residual_recheck_complete = true;
  result.base_row_mga_recheck_complete = true;
  result.security_recheck_complete = true;
  AddApiBehaviorEvidence(&result, "key_value_visibility_authority",
                         "engine_mga_statement_snapshot_first");
  AddApiBehaviorEvidence(
      &result, "key_value_ttl_statement_timestamp",
      request.statement_timestamp);
  AddApiBehaviorEvidence(
      &result, "key_value_access_route",
      result.exact_fallback_observed
          ? "KEY_VALUE_ORDERED_STORE_SCAN_EXACT_V1"
          : "key_value.local.v1");
  AddApiBehaviorEvidence(&result, "key_value_ordering", result.ordering_id);
  return result;
}

}  // namespace scratchbird::engine::internal_api
