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
#include "nosql/nosql_surface_support.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
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
  authority.parser_or_donor_finality_or_visibility_authority =
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

}  // namespace scratchbird::engine::internal_api
