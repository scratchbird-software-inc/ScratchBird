// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/insert_batch.hpp"

#include "api_diagnostics.hpp"
#include "deferred_secondary_index_runtime_policy.hpp"
#include "metric_contracts.hpp"
#include "metric_producer.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "query_memory_arena.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;
namespace mem = scratchbird::core::memory;

constexpr const char* kInsertMetricsProducer = "engine_insert";

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool OptionEquals(const std::string& actual, const std::string& expected) {
  if (actual == expected) {
    return true;
  }
  const auto equals = expected.find('=');
  if (equals == std::string::npos) {
    return false;
  }
  return actual == expected.substr(0, equals) + ":" + expected.substr(equals + 1);
}

std::size_t OptionValueOffset(const std::string& actual, const std::string& prefix) {
  if (StartsWith(actual, prefix)) {
    return prefix.size();
  }
  if (!prefix.empty() && prefix.back() == '=') {
    const std::string colon_prefix = prefix.substr(0, prefix.size() - 1) + ":";
    if (StartsWith(actual, colon_prefix)) {
      return colon_prefix.size();
    }
  }
  return std::string::npos;
}

std::string MakeId(const std::string& prefix, const std::string& stable) {
  return prefix + ":" + stable;
}

std::uint64_t EstimateRows(const EngineInsertRowsRequest& request) {
  if (request.estimated_row_count != 0) {
    return request.estimated_row_count;
  }
  return request.EffectiveInputRows().size();
}

std::string TargetUuid(const EngineInsertRowsRequest& request) {
  if (!request.target_table.uuid.canonical.empty()) {
    return request.target_table.uuid.canonical;
  }
  return request.target_object.uuid.canonical;
}

bool IsUniqueIndex(const CrudIndexRecord& index) {
  return index.unique || std::find(index.key_envelopes.begin(), index.key_envelopes.end(), "unique") != index.key_envelopes.end();
}

bool IsDeltaEligibleFamily(const CrudIndexRecord& index) {
  if (IsUniqueIndex(index)) {
    return false;
  }
  return index.family == kCrudIndexFamilyBtree ||
         index.family == kCrudIndexFamilyHash ||
         index.family.empty();
}

InsertIndexMaintenanceAction ActionForIndex(const CrudIndexRecord& index,
                                            bool delta_ledger_enabled,
                                            const InsertFeatureGates& feature_gates) {
  if (IsUniqueIndex(index)) {
    return InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert;
  }
  if (IsDeltaEligibleFamily(index) && delta_ledger_enabled) {
    return InsertIndexMaintenanceAction::committed_delta_ledger;
  }
  if (feature_gates.sorted_run_shadow_load == InsertFeatureState::enabled &&
      (index.family == kCrudIndexFamilyBtree || index.family == kCrudIndexFamilyHash)) {
    return InsertIndexMaintenanceAction::sorted_run_build;
  }
  if (index.family == kCrudIndexFamilyVectorHnsw ||
      index.family == kCrudIndexFamilyVectorIvf ||
      index.family == kCrudIndexFamilyFullText ||
      index.family == kCrudIndexFamilySpatial ||
      index.family == kCrudIndexFamilyGraphAdjacency) {
    return InsertIndexMaintenanceAction::synchronous_exact_insert;
  }
  return InsertIndexMaintenanceAction::synchronous_exact_insert;
}

std::string ActionReason(InsertIndexMaintenanceAction action) {
  switch (action) {
    case InsertIndexMaintenanceAction::synchronous_exact_insert: return "synchronous_exact_default";
    case InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert: return "unique_exact_preflight_required";
    case InsertIndexMaintenanceAction::batch_local_buffer_then_insert: return "batch_local_buffer";
    case InsertIndexMaintenanceAction::committed_delta_ledger: return "non_unique_secondary_delta_ledger_enabled";
    case InsertIndexMaintenanceAction::sorted_run_build: return "sorted_run_policy_enabled";
    case InsertIndexMaintenanceAction::shadow_rebuild_then_cutover: return "shadow_rebuild_policy_enabled";
    case InsertIndexMaintenanceAction::reject_batch_path: return "index_rejects_batch_path";
  }
  return "unknown";
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

mem::BoundedAllocator& InsertMemoryAllocator() {
  static mem::BoundedAllocator allocator([] {
    mem::AllocationPolicy policy;
    policy.policy_name = "engine_insert_query_memory_arena";
    policy.byte_limit = 256ull * 1024ull * 1024ull;
    policy.hard_limit_bytes = policy.byte_limit;
    policy.soft_limit_bytes = 224ull * 1024ull * 1024ull;
    policy.per_context_limit_bytes = 64ull * 1024ull * 1024ull;
    policy.failure_mode = mem::AllocationFailureMode::return_error;
    policy.track_allocations = true;
    return policy;
  }());
  return allocator;
}

bool IsOkDiagnostic(const EngineApiDiagnostic& diagnostic) {
  return !diagnostic.error || diagnostic.code == "SB_ENGINE_API_OK";
}

std::uint64_t ParseU64Option(const EngineInsertRowsRequest& request,
                             const std::string& prefix,
                             std::uint64_t fallback) {
  const auto value = InsertBatchOptionValue(request, prefix);
  if (value.empty()) {
    return fallback;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

std::uint64_t StableHashText(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : value) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

void AppendKeyPart(std::ostringstream* out,
                   const std::string& key,
                   const std::string& value) {
  *out << key << '=' << value.size() << ':' << value << ';';
}

void AppendKeyPart(std::ostringstream* out,
                   const std::string& key,
                   std::uint64_t value) {
  *out << key << '=' << value << ';';
}

void AppendStringListKeyPart(std::ostringstream* out,
                             const std::string& key,
                             std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  *out << key << "=[";
  for (const auto& value : values) {
    *out << value.size() << ':' << value << ',';
  }
  *out << "];";
}

std::string TrimAscii(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::map<std::string, std::string> DescriptorFields(const std::string& descriptor) {
  std::map<std::string, std::string> fields;
  std::string current;
  std::istringstream in(descriptor);
  while (std::getline(in, current, ';')) {
    current = TrimAscii(current);
    if (current.empty()) {
      continue;
    }
    const auto equals = current.find('=');
    if (equals == std::string::npos) {
      fields[LowerAscii(current)] = "true";
      continue;
    }
    fields[LowerAscii(TrimAscii(current.substr(0, equals)))] =
        TrimAscii(current.substr(equals + 1));
  }
  return fields;
}

std::string DescriptorField(const std::map<std::string, std::string>& fields,
                            std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto found = fields.find(key);
    if (found != fields.end()) {
      return found->second;
    }
  }
  return {};
}

bool DescriptorBool(const std::map<std::string, std::string>& fields,
                    std::initializer_list<const char*> keys) {
  const std::string value = LowerAscii(DescriptorField(fields, keys));
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool DescriptorFalse(const std::map<std::string, std::string>& fields,
                     std::initializer_list<const char*> keys) {
  const std::string value = LowerAscii(DescriptorField(fields, keys));
  return value == "0" || value == "false" || value == "no" || value == "off";
}

bool DescriptorValuePresent(const std::map<std::string, std::string>& fields,
                            std::initializer_list<const char*> keys) {
  const std::string value = DescriptorField(fields, keys);
  return !value.empty() && LowerAscii(value) != "none";
}

bool DescriptorEnvelopeUsesSblr(const std::string& value) {
  const std::string lowered = LowerAscii(value);
  return StartsWith(lowered, "sblr:") ||
         StartsWith(lowered, "sblr_expression:") ||
         StartsWith(lowered, "sblr_predicate:") ||
         StartsWith(lowered, "generated:");
}

std::string HashSignature(const std::string& prefix, const std::string& body) {
  return prefix + ":" + std::to_string(StableHashText(body));
}

InsertRowEncoderPlan BuildInsertRowEncoderPlan(
    const EngineInsertRowsRequest& request,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes) {
  InsertRowEncoderPlan plan;
  plan.table_uuid = table.table_uuid;
  std::ostringstream shape;
  std::ostringstream defaults;
  std::ostringstream domains;
  std::ostringstream checks;
  std::ostringstream validators;
  AppendKeyPart(&shape, "table", table.table_uuid);
  std::size_t ordinal = 0;
  for (const auto& column : table.columns) {
    const auto fields = DescriptorFields(column.second);
    InsertRowEncoderColumnPlan column_plan;
    column_plan.column_name = column.first;
    column_plan.ordinal = ordinal++;
    column_plan.canonical_type_name =
        DescriptorField(fields, {"canonical", "type", "base_type"});
    column_plan.descriptor_digest = HashSignature("descriptor", column.second);

    const std::string default_envelope =
        DescriptorField(fields, {"default_expression", "default_constraint", "default_value", "default"});
    column_plan.default_bound = !default_envelope.empty();
    column_plan.domain_bound =
        DescriptorValuePresent(fields, {"domain_uuid", "domain", "domain_descriptor", "domain_ref"});
    column_plan.check_bound =
        DescriptorValuePresent(fields, {"check_constraint", "check", "predicate_sblr_ref"});
    column_plan.not_null_bound =
        DescriptorBool(fields, {"not_null", "required"}) ||
        DescriptorBool(fields, {"primary_key", "pk"}) ||
        DescriptorFalse(fields, {"nullable"});
    column_plan.unique_bound =
        DescriptorBool(fields, {"primary_key", "pk"}) ||
        DescriptorBool(fields, {"unique", "unique_key"});
    column_plan.foreign_key_bound =
        DescriptorBool(fields, {"foreign_key", "fk"}) ||
        DescriptorValuePresent(fields, {"references", "referenced_table_uuid", "referenced_key_descriptor_uuid"});

    if (column_plan.default_bound) {
      ++plan.default_validator_count;
      AppendKeyPart(&defaults, column.first, default_envelope);
      if (DescriptorEnvelopeUsesSblr(default_envelope)) {
        plan.has_sblr_backed_default = true;
      }
    }
    if (column_plan.domain_bound) {
      ++plan.domain_validator_count;
      AppendKeyPart(&domains, column.first,
                    DescriptorField(fields, {"domain_uuid", "domain", "domain_descriptor", "domain_ref"}));
    }
    if (column_plan.check_bound) {
      ++plan.check_validator_count;
      const std::string check_envelope =
          DescriptorField(fields, {"check_constraint", "check", "predicate_sblr_ref"});
      AppendKeyPart(&checks, column.first, check_envelope);
      if (DescriptorEnvelopeUsesSblr(check_envelope)) {
        plan.has_sblr_backed_check = true;
      }
    }
    if (column_plan.not_null_bound) {
      ++plan.not_null_validator_count;
    }
    if (column_plan.unique_bound) {
      ++plan.unique_validator_count;
    }
    if (column_plan.foreign_key_bound) {
      ++plan.foreign_key_validator_count;
    }

    AppendKeyPart(&shape, "column", std::to_string(column_plan.ordinal) + "|" +
                                     column.first + "|" +
                                     column_plan.canonical_type_name + "|" +
                                     column_plan.descriptor_digest + "|" +
                                     column.second);
    AppendKeyPart(&validators, "column_validator",
                  column.first + "|default=" + (column_plan.default_bound ? "true" : "false") +
                      "|domain=" + (column_plan.domain_bound ? "true" : "false") +
                      "|check=" + (column_plan.check_bound ? "true" : "false") +
                      "|not_null=" + (column_plan.not_null_bound ? "true" : "false") +
                      "|unique=" + (column_plan.unique_bound ? "true" : "false") +
                      "|foreign_key=" + (column_plan.foreign_key_bound ? "true" : "false"));
    plan.columns.push_back(std::move(column_plan));
  }

  std::ostringstream index_validators;
  for (const auto& index : indexes) {
    std::vector<std::string> parts = {
        index.index_uuid,
        index.table_uuid,
        index.column_name,
        index.family,
        index.profile,
        index.unique ? "unique" : "non_unique",
        index.predicate_kind,
        index.predicate_column,
        index.predicate_value,
    };
    parts.insert(parts.end(), index.key_envelopes.begin(), index.key_envelopes.end());
    parts.insert(parts.end(), index.include_columns.begin(), index.include_columns.end());
    AppendStringListKeyPart(&index_validators, "index_validator", std::move(parts));
  }

  std::ostringstream security_policy;
  for (const auto& policy : request.context.authorization_context.policies) {
    if (!policy.requires_runtime_recheck) {
      continue;
    }
    if (!policy.target_uuid.canonical.empty() &&
        policy.target_uuid.canonical != table.table_uuid) {
      continue;
    }
    ++plan.runtime_policy_recheck_count;
    AppendKeyPart(&security_policy, "runtime_recheck_policy",
                  policy.policy_uuid.canonical + "|" +
                      policy.subject_kind + "|" +
                      policy.subject_uuid.canonical + "|" +
                      policy.target_uuid.canonical + "|" +
                      policy.right + "|" +
                      policy.policy_kind + "|" +
                      std::to_string(policy.policy_epoch) + "|" +
                      policy.canonical_policy_envelope);
  }

  plan.column_count = static_cast<std::uint64_t>(plan.columns.size());
  plan.row_shape_signature = HashSignature("row_shape", shape.str());
  plan.default_signature = HashSignature("defaults", defaults.str());
  plan.domain_signature = HashSignature("domains", domains.str());
  plan.check_signature = HashSignature("checks", checks.str());
  plan.index_validator_signature = HashSignature("index_validators", index_validators.str());
  plan.security_policy_signature = HashSignature("security_policy", security_policy.str());
  AppendKeyPart(&validators, "defaults", plan.default_signature);
  AppendKeyPart(&validators, "domains", plan.domain_signature);
  AppendKeyPart(&validators, "checks", plan.check_signature);
  AppendKeyPart(&validators, "indexes", plan.index_validator_signature);
  AppendKeyPart(&validators, "security_policy", plan.security_policy_signature);
  AppendKeyPart(&validators, "runtime_policy_recheck_count",
                plan.runtime_policy_recheck_count);
  plan.validator_signature = HashSignature("validators", validators.str());
  plan.plan_id = HashSignature("insert_row_encoder_plan",
                               plan.row_shape_signature + "|" +
                                   plan.validator_signature + "|" +
                                   plan.index_validator_signature + "|" +
                                   plan.security_policy_signature);
  return plan;
}

std::uint64_t EstimateInputRowBytes(const EngineInsertRowsRequest& request,
                                    const BoundInsertRowTemplate& row_template,
                                    bool* large_value_pressure) {
  if (large_value_pressure != nullptr) {
    *large_value_pressure = false;
  }
  const auto rows = request.EffectiveInputRows();
  if (rows.empty()) {
    return std::max<std::uint64_t>(
        64,
        static_cast<std::uint64_t>(row_template.columns.size()) * 32);
  }
  const std::size_t sample_count = std::min<std::size_t>(rows.size(), 32);
  std::uint64_t total = 0;
  for (std::size_t index = 0; index < sample_count; ++index) {
    const auto pairs = RowValuePairs(rows[index]);
    const std::uint64_t row_bytes =
        static_cast<std::uint64_t>(std::max<std::size_t>(1, EncodedValueBytes(pairs)));
    total += row_bytes;
    if (large_value_pressure != nullptr &&
        row_bytes > row_template.max_inline_encoded_bytes) {
      *large_value_pressure = true;
    }
  }
  return std::max<std::uint64_t>(1, total / sample_count);
}

struct PreparedInsertDescriptor {
  std::string cache_key;
  std::string descriptor_id;
  std::uint64_t generation = 0;
  std::string database_uuid;
  std::string table_uuid;
  std::string principal_uuid;
  std::string role_uuid;
  std::string session_uuid;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t bound_catalog_epoch = 0;
  std::uint64_t bound_security_epoch = 0;
  std::uint64_t bound_resource_epoch = 0;
  std::string authorization_digest;
  BoundInsertRowTemplate row_template;
  InsertRowEncoderPlan row_encoder_plan;
  IndexMaintenancePlan index_plan;
};

struct PreparedInsertDescriptorValidation {
  bool ok = true;
  std::string refusal_reason;
};

struct PreparedDescriptorCachePressurePolicy {
  std::uint64_t cache_limit = 128;
  std::uint64_t effective_cache_limit = 128;
  std::uint64_t trim_target_entries = 128;
  bool memory_pressure_detected = false;
  bool trim_requested = false;
  bool backoff_active = false;
  std::string pressure_reason = "within_policy";
};

struct PreparedDescriptorCacheTrimResult {
  std::uint64_t entries_before = 0;
  std::uint64_t entries_after = 0;
  std::uint64_t evictions = 0;
};

struct PreparedInsertDescriptorResolution {
  PreparedInsertDescriptor descriptor;
  PreparedDescriptorCachePressurePolicy pressure_policy;
  PreparedDescriptorCacheTrimResult trim_result;
  bool cache_hit = false;
};

std::mutex& PreparedInsertDescriptorCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, PreparedInsertDescriptor>& PreparedInsertDescriptorCache() {
  static std::map<std::string, PreparedInsertDescriptor> cache;
  return cache;
}

std::uint64_t& PreparedInsertDescriptorGenerationCounter() {
  static std::uint64_t generation = 0;
  return generation;
}

std::uint64_t& PreparedInsertDescriptorEvictionCounter() {
  static std::uint64_t eviction_count = 0;
  return eviction_count;
}

std::string BoolKey(bool value) {
  return value ? "true" : "false";
}

std::string PreparedInsertAuthorizationDigest(const EngineRequestContext& context) {
  std::ostringstream out;
  AppendKeyPart(&out, "security_context_present", BoolKey(context.security_context_present));
  AppendKeyPart(&out, "principal", context.principal_uuid.canonical);
  AppendKeyPart(&out, "role", context.current_role_uuid.canonical);
  AppendKeyPart(&out, "security_epoch", context.security_epoch);
  AppendKeyPart(&out, "policy_epoch", context.resource_epoch);
  AppendKeyPart(&out, "catalog_epoch", context.catalog_generation_id);
  const auto& auth = context.authorization_context;
  AppendKeyPart(&out, "auth_present", BoolKey(auth.present));
  AppendKeyPart(&out, "auth_authority", auth.authority_uuid.canonical);
  AppendKeyPart(&out, "auth_principal", auth.principal_uuid.canonical);
  AppendKeyPart(&out, "auth_security_epoch", auth.security_epoch);
  AppendKeyPart(&out, "auth_policy_epoch", auth.policy_epoch);
  AppendKeyPart(&out, "auth_catalog_epoch", auth.catalog_generation_id);
  std::vector<std::string> subjects;
  for (const auto& subject : auth.effective_subjects) {
    subjects.push_back(subject.subject_kind + "|" + subject.subject_uuid.canonical);
  }
  AppendStringListKeyPart(&out, "auth_subjects", std::move(subjects));
  std::vector<std::string> grants;
  for (const auto& grant : auth.grants) {
    grants.push_back(grant.grant_uuid.canonical + "|" +
                     grant.subject_kind + "|" +
                     grant.subject_uuid.canonical + "|" +
                     grant.target_uuid.canonical + "|" +
                     grant.right + "|" +
                     BoolKey(grant.deny) + "|" +
                     std::to_string(grant.security_epoch));
  }
  AppendStringListKeyPart(&out, "auth_grants", std::move(grants));
  std::vector<std::string> policies;
  for (const auto& policy : auth.policies) {
    policies.push_back(policy.policy_uuid.canonical + "|" +
                       policy.subject_kind + "|" +
                       policy.subject_uuid.canonical + "|" +
                       policy.target_uuid.canonical + "|" +
                       policy.right + "|" +
                       policy.policy_kind + "|" +
                       BoolKey(policy.deny) + "|" +
                       BoolKey(policy.requires_runtime_recheck) + "|" +
                       std::to_string(policy.policy_epoch) + "|" +
                       policy.canonical_policy_envelope);
  }
  AppendStringListKeyPart(&out, "auth_policies", std::move(policies));
  AppendStringListKeyPart(&out, "auth_evidence_tags", auth.evidence_tags);
  return "auth:" + std::to_string(StableHashText(out.str()));
}

PreparedInsertDescriptor PreparedInsertDescriptorIdentity(
    const EngineInsertRowsRequest& request,
    const CrudTableRecord& table) {
  PreparedInsertDescriptor descriptor;
  descriptor.database_uuid = request.context.database_uuid.canonical;
  descriptor.table_uuid = table.table_uuid;
  descriptor.principal_uuid = request.context.principal_uuid.canonical;
  descriptor.role_uuid = request.context.current_role_uuid.canonical;
  descriptor.session_uuid = request.context.session_uuid.canonical;
  descriptor.catalog_epoch = request.context.catalog_generation_id;
  descriptor.security_epoch = request.context.security_epoch;
  descriptor.policy_epoch = request.context.resource_epoch;
  descriptor.name_resolution_epoch = request.context.name_resolution_epoch;
  descriptor.bound_catalog_epoch = request.bound_object_identity.catalog_generation_id;
  descriptor.bound_security_epoch = request.bound_object_identity.security_epoch;
  descriptor.bound_resource_epoch = request.bound_object_identity.resource_epoch;
  descriptor.authorization_digest = PreparedInsertAuthorizationDigest(request.context);
  return descriptor;
}

bool PreparedDescriptorLifecycleOption(const std::string& option) {
  return StartsWith(option, "prepared_descriptor.");
}

std::uint64_t PreparedDescriptorCacheLimit(const EngineInsertRowsRequest& request) {
  const std::uint64_t requested =
      ParseU64Option(request, "prepared_descriptor.cache_limit=", 128);
  return std::max<std::uint64_t>(1, requested);
}

bool PreparedDescriptorMemoryPressureDetected(const EngineInsertRowsRequest& request) {
  return InsertBatchOptionEnabled(request, "prepared_descriptor.memory_pressure=true") ||
         InsertBatchOptionEnabled(request, "memory.pressure=prepared_descriptor_cache") ||
         InsertBatchOptionEnabled(request, "memory.pressure=true");
}

PreparedDescriptorCachePressurePolicy ResolvePreparedDescriptorCachePressurePolicy(
    const EngineInsertRowsRequest& request) {
  PreparedDescriptorCachePressurePolicy policy;
  policy.cache_limit = PreparedDescriptorCacheLimit(request);
  policy.effective_cache_limit = policy.cache_limit;
  policy.trim_target_entries = policy.cache_limit;
  policy.memory_pressure_detected = PreparedDescriptorMemoryPressureDetected(request);
  if (!policy.memory_pressure_detected) {
    return policy;
  }

  policy.pressure_reason = InsertBatchOptionValue(
      request,
      "prepared_descriptor.pressure_reason=");
  if (policy.pressure_reason.empty()) {
    policy.pressure_reason = "configured_memory_pressure";
  }
  const std::uint64_t default_target =
      std::max<std::uint64_t>(1, policy.cache_limit / 2);
  const std::uint64_t requested_target = ParseU64Option(
      request,
      "prepared_descriptor.trim_target_entries=",
      default_target);
  policy.effective_cache_limit =
      std::max<std::uint64_t>(1, std::min(policy.cache_limit, requested_target));
  policy.trim_target_entries = policy.effective_cache_limit;
  policy.trim_requested = true;
  policy.backoff_active = policy.effective_cache_limit < policy.cache_limit;
  return policy;
}

void TrimPreparedDescriptorCacheToLimit(
    std::map<std::string, PreparedInsertDescriptor>* cache,
    std::uint64_t target_entries,
    const std::string& protected_cache_key,
    PreparedDescriptorCacheTrimResult* result) {
  if (cache == nullptr) {
    return;
  }
  target_entries = std::max<std::uint64_t>(1, target_entries);
  while (cache->size() > target_entries) {
    auto victim = cache->begin();
    if (victim->first == protected_cache_key && cache->size() > 1) {
      ++victim;
    }
    if (victim == cache->end() || victim->first == protected_cache_key) {
      break;
    }
    cache->erase(victim);
    ++PreparedInsertDescriptorEvictionCounter();
    if (result != nullptr) {
      ++result->evictions;
    }
  }
}

std::string PreparedInsertDescriptorCacheKey(
    const EngineInsertRowsRequest& request,
    const PreparedInsertDescriptor& identity,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes,
    const InsertFeatureGates& feature_gates,
    const SecondaryIndexDeltaLedgerPolicy& delta_ledger_policy) {
  std::ostringstream out;
  AppendKeyPart(&out, "database", identity.database_uuid);
  AppendKeyPart(&out, "table", identity.table_uuid);
  AppendKeyPart(&out, "principal", identity.principal_uuid);
  AppendKeyPart(&out, "role", identity.role_uuid);
  AppendKeyPart(&out, "session", identity.session_uuid);
  AppendKeyPart(&out, "authorization_digest", identity.authorization_digest);
  AppendKeyPart(&out, "catalog_epoch", identity.catalog_epoch);
  AppendKeyPart(&out, "security_epoch", identity.security_epoch);
  AppendKeyPart(&out, "policy_epoch", identity.policy_epoch);
  AppendKeyPart(&out, "name_epoch", identity.name_resolution_epoch);
  AppendKeyPart(&out, "bound_catalog_epoch", identity.bound_catalog_epoch);
  AppendKeyPart(&out, "bound_security_epoch", identity.bound_security_epoch);
  AppendKeyPart(&out, "bound_resource_epoch", identity.bound_resource_epoch);
  AppendKeyPart(&out, "insert_mode",
                InsertBatchModeName(ResolveInsertBatchMode(request)));
  AppendKeyPart(&out, "duplicate_mode",
                InsertDuplicateModeName(ResolveInsertDuplicateMode(request)));
  AppendKeyPart(&out, "strict_bulk",
                ResolveStrictBulkLoadPolicy(request).requested ? "requested"
                                                               : "not_requested");
  AppendKeyPart(&out, "delta_ledger",
                delta_ledger_policy.enabled ? "enabled" : "disabled");
  AppendKeyPart(&out, "feature_page",
                InsertFeatureStateName(feature_gates.page_reservation));
  AppendKeyPart(&out, "feature_identity",
                InsertFeatureStateName(feature_gates.identity_range_reservation));
  for (const auto& column : table.columns) {
    AppendKeyPart(&out, "column", column.first + "|" + column.second);
  }
  for (const auto& index : indexes) {
    std::vector<std::string> index_parts = {
        index.index_uuid,
        index.table_uuid,
        index.column_name,
        index.family,
        index.profile,
        index.unique ? "unique" : "non_unique",
        index.predicate_kind,
        index.predicate_column,
        index.predicate_value,
    };
    index_parts.insert(index_parts.end(),
                       index.key_envelopes.begin(),
                       index.key_envelopes.end());
    index_parts.insert(index_parts.end(),
                       index.include_columns.begin(),
                       index.include_columns.end());
    AppendStringListKeyPart(&out, "index", std::move(index_parts));
  }
  std::vector<std::string> options;
  for (const auto& option : request.option_envelopes) {
    if (!PreparedDescriptorLifecycleOption(option)) {
      options.push_back(option);
    }
  }
  AppendStringListKeyPart(&out, "options", std::move(options));
  return "prepared_insert_descriptor:" +
         std::to_string(StableHashText(out.str()));
}

PreparedInsertDescriptorValidation ValidatePreparedInsertDescriptorAuthority(
    const EngineInsertRowsRequest& request,
    const PreparedInsertDescriptor& descriptor) {
  PreparedInsertDescriptorValidation validation;
  const auto expected = [&request](const std::string& prefix) {
    return InsertBatchOptionValue(request, prefix);
  };
  const auto expected_u64 = [&request](const std::string& prefix) {
    return ParseU64Option(request, prefix, 0);
  };
  const auto refuse = [&validation](std::string reason) {
    validation.ok = false;
    validation.refusal_reason = std::move(reason);
  };

  const std::uint64_t lease_expires_at =
      expected_u64("prepared_descriptor.lease_expires_at_epoch=");
  const std::uint64_t current_lease_epoch =
      expected_u64("prepared_descriptor.current_lease_epoch=");
  if (lease_expires_at != 0 && current_lease_epoch != 0 &&
      current_lease_epoch > lease_expires_at) {
    refuse("lease_expired");
    return validation;
  }

  const std::string expected_principal =
      expected("prepared_descriptor.expected_principal_uuid=");
  if (!expected_principal.empty() && expected_principal != descriptor.principal_uuid) {
    refuse("cross_user");
    return validation;
  }
  const std::string expected_role =
      expected("prepared_descriptor.expected_role_uuid=");
  if (!expected_role.empty() && expected_role != descriptor.role_uuid) {
    refuse("cross_role");
    return validation;
  }
  const std::string expected_session =
      expected("prepared_descriptor.expected_session_uuid=");
  if (!expected_session.empty() && expected_session != descriptor.session_uuid) {
    refuse("cross_session");
    return validation;
  }

  const std::uint64_t expected_catalog =
      expected_u64("prepared_descriptor.expected_catalog_epoch=");
  if (expected_catalog != 0 && expected_catalog != descriptor.catalog_epoch) {
    refuse("stale_catalog_epoch");
    return validation;
  }
  const std::uint64_t expected_security =
      expected_u64("prepared_descriptor.expected_security_epoch=");
  if (expected_security != 0 && expected_security != descriptor.security_epoch) {
    refuse("stale_security_epoch");
    return validation;
  }
  const std::uint64_t expected_policy =
      expected_u64("prepared_descriptor.expected_policy_epoch=");
  if (expected_policy != 0 && expected_policy != descriptor.policy_epoch) {
    refuse("stale_policy_epoch");
    return validation;
  }
  const std::string expected_authorization =
      expected("prepared_descriptor.expected_authorization_digest=");
  if (!expected_authorization.empty() &&
      expected_authorization != descriptor.authorization_digest) {
    refuse("authorization_context_changed");
    return validation;
  }
  const std::uint64_t expected_generation =
      expected_u64("prepared_descriptor.expected_generation=");
  if (expected_generation != 0 && expected_generation != descriptor.generation) {
    refuse("evicted_or_rebound");
    return validation;
  }
  const std::string expected_key =
      expected("prepared_descriptor.expected_cache_key=");
  if (!expected_key.empty() && expected_key != descriptor.cache_key) {
    refuse("stale_descriptor_key");
    return validation;
  }
  return validation;
}

PreparedInsertDescriptorResolution ResolvePreparedInsertDescriptor(
    const EngineInsertRowsRequest& request,
    const CrudState& state,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes,
    const InsertFeatureGates& feature_gates,
    const SecondaryIndexDeltaLedgerPolicy& delta_ledger_policy) {
  PreparedInsertDescriptorResolution resolution;
  resolution.pressure_policy =
      ResolvePreparedDescriptorCachePressurePolicy(request);
  PreparedInsertDescriptor identity = PreparedInsertDescriptorIdentity(request, table);
  const std::string cache_key = PreparedInsertDescriptorCacheKey(
      request,
      identity,
      table,
      indexes,
      feature_gates,
      delta_ledger_policy);
  {
    const std::lock_guard<std::mutex> guard(PreparedInsertDescriptorCacheMutex());
    auto& cache = PreparedInsertDescriptorCache();
    resolution.trim_result.entries_before =
        static_cast<std::uint64_t>(cache.size());
    if (resolution.pressure_policy.trim_requested) {
      TrimPreparedDescriptorCacheToLimit(&cache,
                                         resolution.pressure_policy.trim_target_entries,
                                         cache_key,
                                         &resolution.trim_result);
    }
    const auto found = cache.find(cache_key);
    if (found != cache.end()) {
      resolution.cache_hit = true;
      resolution.descriptor = found->second;
      resolution.trim_result.entries_after =
          static_cast<std::uint64_t>(cache.size());
      return resolution;
    }
  }

  PreparedInsertDescriptor descriptor;
  descriptor = std::move(identity);
  descriptor.cache_key = cache_key;
  descriptor.row_template = BuildBoundInsertRowTemplate(request, table);
  descriptor.row_encoder_plan = BuildInsertRowEncoderPlan(request, table, indexes);
  descriptor.index_plan = BuildIndexMaintenancePlan(request,
                                                    state,
                                                    table,
                                                    indexes,
                                                    feature_gates,
                                                    delta_ledger_policy);
  {
    const std::lock_guard<std::mutex> guard(PreparedInsertDescriptorCacheMutex());
    descriptor.generation = ++PreparedInsertDescriptorGenerationCounter();
    descriptor.descriptor_id =
        descriptor.cache_key + ":generation=" + std::to_string(descriptor.generation);
    auto& cache = PreparedInsertDescriptorCache();
    cache[descriptor.cache_key] = descriptor;
    TrimPreparedDescriptorCacheToLimit(&cache,
                                       resolution.pressure_policy.effective_cache_limit,
                                       descriptor.cache_key,
                                       &resolution.trim_result);
    while (cache.size() > resolution.pressure_policy.effective_cache_limit) {
      cache.erase(cache.begin());
      ++PreparedInsertDescriptorEvictionCounter();
      ++resolution.trim_result.evictions;
    }
    resolution.trim_result.entries_after =
        static_cast<std::uint64_t>(cache.size());
  }
  resolution.descriptor = std::move(descriptor);
  return resolution;
}

InsertAdaptiveBatchPlan PlanInsertAdaptiveBatch(
    const EngineInsertRowsRequest& request,
    const BoundInsertRowTemplate& row_template,
    const IndexMaintenancePlan& index_plan,
    const InsertBatchMemoryPolicy& memory_policy,
    std::uint64_t estimated_rows) {
  InsertAdaptiveBatchPlan plan;
  plan.requested_rows = estimated_rows == 0 ? EstimateRows(request) : estimated_rows;
  bool large_value_pressure = false;
  plan.estimated_row_bytes =
      EstimateInputRowBytes(request, row_template, &large_value_pressure);
  plan.index_count = static_cast<std::uint64_t>(index_plan.entries.size());
  plan.large_value_pressure = large_value_pressure;
  const std::uint64_t per_row_bytes =
      std::max<std::uint64_t>(1, plan.estimated_row_bytes + (plan.index_count * 64));
  plan.requested_bytes = per_row_bytes * std::max<std::uint64_t>(1, plan.requested_rows);
  const std::uint64_t memory_rows =
      memory_policy.context_budget_bytes == 0
          ? std::max<std::uint64_t>(1, plan.requested_rows)
          : std::max<std::uint64_t>(1, memory_policy.context_budget_bytes / per_row_bytes);
  std::uint64_t policy_rows = 4096;
  std::string policy_reason = "batch_policy_cap";
  if (plan.index_count >= 8) {
    policy_rows = 512;
    policy_reason = "index_fanout";
  } else if (plan.index_count >= 4) {
    policy_rows = 1024;
    policy_reason = "index_fanout";
  }
  if (large_value_pressure) {
    policy_rows = std::min<std::uint64_t>(policy_rows, 256);
    policy_reason = "large_value_pressure";
  }
  plan.page_size_bytes =
      ParseU64Option(request, "adaptive_batch.page_size_bytes=", 0);
  if (plan.page_size_bytes != 0) {
    plan.page_window_rows =
        std::max<std::uint64_t>(1, plan.page_size_bytes / per_row_bytes);
  }
  plan.commit_window_rows =
      ParseU64Option(request, "adaptive_batch.commit_window_rows=", 0);
  plan.contention_window_rows =
      ParseU64Option(request, "adaptive_batch.contention_window_rows=", 0);

  std::uint64_t admitted_rows = std::max<std::uint64_t>(1, plan.requested_rows);
  std::string reduction_reason = "within_policy";
  const auto apply_cap = [&admitted_rows, &reduction_reason](std::uint64_t cap,
                                                            const std::string& reason) {
    if (cap == 0) {
      return;
    }
    cap = std::max<std::uint64_t>(1, cap);
    if (cap < admitted_rows) {
      admitted_rows = cap;
      reduction_reason = reason;
    }
  };
  apply_cap(memory_rows, "memory_budget");
  apply_cap(policy_rows, policy_reason);
  apply_cap(plan.page_window_rows, "page_size");
  apply_cap(plan.commit_window_rows, "commit_policy");
  apply_cap(plan.contention_window_rows, "contention");

  plan.admitted_rows = admitted_rows;
  plan.admitted_bytes = per_row_bytes * plan.admitted_rows;
  plan.reduced = plan.admitted_rows < plan.requested_rows;
  if (plan.reduced) {
    plan.reason = reduction_reason;
  }
  return plan;
}

}  // namespace

const char* InsertBatchModeName(InsertBatchMode mode) {
  switch (mode) {
    case InsertBatchMode::singleton: return "singleton";
    case InsertBatchMode::multi_values: return "multi_values";
    case InsertBatchMode::insert_select: return "insert_select";
    case InsertBatchMode::copy_import: return "copy_import";
    case InsertBatchMode::reference_bulk: return "reference_bulk";
    case InsertBatchMode::native_bulk: return "native_bulk";
  }
  return "unknown";
}

const char* InsertDuplicateModeName(InsertDuplicateMode mode) {
  switch (mode) {
    case InsertDuplicateMode::error: return "error";
    case InsertDuplicateMode::ignore: return "ignore";
    case InsertDuplicateMode::replace: return "replace";
    case InsertDuplicateMode::update: return "update";
    case InsertDuplicateMode::merge_policy: return "merge_policy";
  }
  return "unknown";
}

const char* InsertFeatureStateName(InsertFeatureState state) {
  switch (state) {
    case InsertFeatureState::enabled: return "enabled";
    case InsertFeatureState::disabled: return "disabled";
    case InsertFeatureState::policy_required: return "policy_required";
    case InsertFeatureState::refused: return "refused";
  }
  return "unknown";
}

const char* InsertIndexMaintenanceActionName(InsertIndexMaintenanceAction action) {
  switch (action) {
    case InsertIndexMaintenanceAction::synchronous_exact_insert: return "synchronous_exact_insert";
    case InsertIndexMaintenanceAction::synchronous_exact_probe_then_insert: return "synchronous_exact_probe_then_insert";
    case InsertIndexMaintenanceAction::batch_local_buffer_then_insert: return "batch_local_buffer_then_insert";
    case InsertIndexMaintenanceAction::committed_delta_ledger: return "committed_delta_ledger";
    case InsertIndexMaintenanceAction::sorted_run_build: return "sorted_run_build";
    case InsertIndexMaintenanceAction::shadow_rebuild_then_cutover: return "shadow_rebuild_then_cutover";
    case InsertIndexMaintenanceAction::reject_batch_path: return "reject_batch_path";
  }
  return "unknown";
}

bool InsertBatchOptionEnabled(const EngineInsertRowsRequest& request, const std::string& option) {
  for (const auto& candidate : request.option_envelopes) {
    if (OptionEquals(candidate, option)) {
      return true;
    }
  }
  return false;
}

std::string InsertBatchOptionValue(const EngineInsertRowsRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    const auto offset = OptionValueOffset(option, prefix);
    if (offset != std::string::npos) {
      return option.substr(offset);
    }
  }
  return {};
}

InsertBatchMode ResolveInsertBatchMode(const EngineInsertRowsRequest& request) {
  if (!request.insert_mode.empty()) {
    if (request.insert_mode == "singleton") return InsertBatchMode::singleton;
    if (request.insert_mode == "multi_values") return InsertBatchMode::multi_values;
    if (request.insert_mode == "insert_select") return InsertBatchMode::insert_select;
    if (request.insert_mode == "copy_import") return InsertBatchMode::copy_import;
    if (request.insert_mode == "reference_bulk") return InsertBatchMode::reference_bulk;
    if (request.insert_mode == "native_bulk") return InsertBatchMode::native_bulk;
  }
  if (InsertBatchOptionEnabled(request, "insert_mode=insert_select")) return InsertBatchMode::insert_select;
  if (InsertBatchOptionEnabled(request, "insert_mode=copy_import")) return InsertBatchMode::copy_import;
  if (InsertBatchOptionEnabled(request, "insert_mode=reference_bulk")) return InsertBatchMode::reference_bulk;
  if (InsertBatchOptionEnabled(request, "insert_mode=native_bulk")) return InsertBatchMode::native_bulk;
  return request.input_rows.size() <= 1 ? InsertBatchMode::singleton : InsertBatchMode::multi_values;
}

InsertDuplicateMode ResolveInsertDuplicateMode(const EngineInsertRowsRequest& request) {
  if (request.duplicate_mode == "ignore") return InsertDuplicateMode::ignore;
  if (request.duplicate_mode == "replace") return InsertDuplicateMode::replace;
  if (request.duplicate_mode == "update") return InsertDuplicateMode::update;
  if (request.duplicate_mode == "merge_policy") return InsertDuplicateMode::merge_policy;
  if (InsertBatchOptionEnabled(request, "duplicate_mode=ignore")) return InsertDuplicateMode::ignore;
  if (InsertBatchOptionEnabled(request, "duplicate_mode=replace")) return InsertDuplicateMode::replace;
  if (InsertBatchOptionEnabled(request, "duplicate_mode=update")) return InsertDuplicateMode::update;
  return InsertDuplicateMode::error;
}

InsertFeatureGates ResolveInsertFeatureGates(const EngineInsertRowsRequest& request) {
  InsertFeatureGates gates;
  if (InsertBatchOptionEnabled(request, "feature.page_reservation=disabled")) {
    gates.page_reservation = InsertFeatureState::disabled;
  }
  if (InsertBatchOptionEnabled(request, "feature.identity_range_reservation=disabled")) {
    gates.identity_range_reservation = InsertFeatureState::disabled;
  }
  // DPC_DEFERRED_INDEX_FEATURE_FLAG
  if (InsertBatchOptionEnabled(request, idx::kDeferredSecondaryIndexRuntimeOption)) {
    gates.deferred_secondary_index_runtime = InsertFeatureState::enabled;
  }
  if (InsertBatchOptionEnabled(request, idx::kSecondaryIndexDeltaLedgerFeatureOption)) {
    gates.secondary_index_delta_ledger = InsertFeatureState::enabled;
  }
  if (InsertBatchOptionEnabled(request, "feature.strict_bulk_load=enabled")) {
    gates.strict_bulk_load = InsertFeatureState::enabled;
  }
  if (InsertBatchOptionEnabled(request, "feature.sorted_run_shadow_load=enabled")) {
    gates.sorted_run_shadow_load = InsertFeatureState::enabled;
  }
  return gates;
}

InsertBatchMemoryPolicy ResolveInsertMemoryPolicy(const EngineInsertRowsRequest& request) {
  InsertBatchMemoryPolicy policy;
  policy.context_budget_bytes = ParseU64Option(request, "memory.context_budget_bytes=", policy.context_budget_bytes);
  policy.unique_preflight_budget_bytes = ParseU64Option(request, "memory.unique_preflight_budget_bytes=", policy.unique_preflight_budget_bytes);
  policy.delta_ledger_stage_budget_bytes = ParseU64Option(request, "memory.delta_ledger_stage_budget_bytes=", policy.delta_ledger_stage_budget_bytes);
  policy.sorted_run_budget_bytes = ParseU64Option(request, "memory.sorted_run_budget_bytes=", policy.sorted_run_budget_bytes);
  policy.bulk_load_budget_bytes = ParseU64Option(request, "memory.bulk_load_budget_bytes=", policy.bulk_load_budget_bytes);
  policy.spill_allowed = InsertBatchOptionEnabled(request, "memory.spill_allowed=true");
  return policy;
}

StrictBulkLoadPolicy ResolveStrictBulkLoadPolicy(const EngineInsertRowsRequest& request) {
  StrictBulkLoadPolicy policy;
  policy.requested = request.strict_bulk_load_requested || InsertBatchOptionEnabled(request, "strict_bulk_load=requested");
  policy.enabled = InsertBatchOptionEnabled(request, "feature.strict_bulk_load=enabled");
  policy.allow_triggers = InsertBatchOptionEnabled(request, "bulk.allow_triggers=true");
  policy.allow_foreign_keys = InsertBatchOptionEnabled(request, "bulk.allow_foreign_keys=true");
  policy.allow_opaque_columns = InsertBatchOptionEnabled(request, "bulk.allow_opaque_columns=true");
  policy.target_empty_required = !InsertBatchOptionEnabled(request, "bulk.target_empty_required=false");
  return policy;
}

BoundInsertRowTemplate BuildBoundInsertRowTemplate(const EngineInsertRowsRequest& request,
                                                   const CrudTableRecord& table) {
  BoundInsertRowTemplate row_template;
  row_template.table_uuid = table.table_uuid;
  row_template.columns = table.columns;
  row_template.descriptor_count = table.columns.size();
  row_template.requires_generated_row_uuid = request.require_generated_row_uuid;
  row_template.template_id = MakeId("insert_template", table.table_uuid + ":" + std::to_string(table.columns.size()));
  for (const auto& column : table.columns) {
    if (CrudColumnDescriptorIsOpaqueRenderOnly(column.second)) {
      row_template.has_opaque_render_only_column = true;
    }
  }
  return row_template;
}

IndexMaintenancePlan BuildIndexMaintenancePlan(const EngineInsertRowsRequest& request,
                                               const CrudState&,
                                               const CrudTableRecord& table,
                                               const std::vector<CrudIndexRecord>& indexes,
                                               const InsertFeatureGates& feature_gates,
                                               const SecondaryIndexDeltaLedgerPolicy& delta_ledger_policy) {
  IndexMaintenancePlan plan;
  plan.table_uuid = table.table_uuid;
  plan.plan_id = MakeId("index_plan", table.table_uuid + ":" + std::to_string(indexes.size()));
  for (const auto& index : indexes) {
    IndexMaintenancePlanEntry entry;
    entry.index = index;
    entry.action = ActionForIndex(index, delta_ledger_policy.enabled, feature_gates);
    entry.reason = ActionReason(entry.action);
    if (IsUniqueIndex(index)) {
      plan.has_unique_exact = true;
    }
    if (entry.action == InsertIndexMaintenanceAction::committed_delta_ledger) {
      plan.has_delta_eligible = true;
    }
    if (entry.action == InsertIndexMaintenanceAction::reject_batch_path) {
      plan.rejected = true;
      plan.rejection_reason = entry.reason;
    }
    plan.entries.push_back(std::move(entry));
  }
  if (request.strict_bulk_load_requested && feature_gates.strict_bulk_load != InsertFeatureState::enabled) {
    plan.rejected = true;
    plan.rejection_reason = "strict_bulk_load_policy_not_enabled";
  }
  return plan;
}

IdentityReservationPlan ReserveInsertIdentityRange(const EngineInsertRowsRequest& request,
                                                   const BoundInsertRowTemplate&) {
  IdentityReservationPlan plan;
  plan.requested_count = EstimateRows(request);
  plan.reservation_id = MakeId("identity_reservation", TargetUuid(request) + ":" + std::to_string(plan.requested_count));
  if (ResolveInsertFeatureGates(request).identity_range_reservation == InsertFeatureState::enabled) {
    plan.reserved_count = plan.requested_count;
    plan.range_reserved = plan.requested_count != 0;
  } else {
    plan.refusal_reason = "identity_range_reservation_disabled";
  }
  return plan;
}

PageReservationPlan ReserveInsertPages(const EngineInsertRowsRequest& request,
                                       const BoundInsertRowTemplate&,
                                       std::uint64_t estimated_rows) {
  PageReservationPlan plan;
  plan.requested_pages = std::max<std::uint64_t>(1, (estimated_rows + 127) / 128);
  plan.reservation_id = MakeId("page_reservation", TargetUuid(request) + ":" + std::to_string(plan.requested_pages));
  if (ResolveInsertFeatureGates(request).page_reservation != InsertFeatureState::enabled) {
    plan.reservation_available = false;
    plan.refusal_reason = "page_reservation_disabled";
  }
  return plan;
}

SecondaryIndexDeltaLedgerPolicy ResolveSecondaryIndexDeltaLedgerPolicy(const EngineInsertRowsRequest& request,
                                                                       const InsertFeatureGates& feature_gates) {
  SecondaryIndexDeltaLedgerPolicy policy;
  // DPC_DEFERRED_INDEX_FEATURE_FLAG_GATE
  const auto decision =
      idx::ResolveDeferredSecondaryIndexRuntimePolicy(request.option_envelopes);
  policy.enabled = decision.enabled &&
                   feature_gates.deferred_secondary_index_runtime == InsertFeatureState::enabled &&
                   feature_gates.secondary_index_delta_ledger == InsertFeatureState::enabled;
  policy.runtime_enabled = decision.runtime_enabled;
  policy.readers_overlay_committed_deltas = decision.readers_overlay_committed_deltas;
  policy.cleanup_horizon_bound = decision.cleanup_horizon_bound;
  policy.recovery_classifiable = decision.recovery_classifiable;
  policy.synchronous_fallback_required = !policy.enabled;
  policy.fallback_reason = policy.enabled ? std::string{} : decision.fallback_reason;
  return policy;
}

void CaptureInsertMemoryArenaProof(const EngineInsertRowsRequest& request,
                                   InsertBatchContext* context) {
  if (context == nullptr || context->memory_arena_granted) {
    return;
  }

  const std::uint64_t budget =
      context->memory_policy.context_budget_bytes == 0
          ? 1024ull * 1024ull
          : context->memory_policy.context_budget_bytes;
  const std::uint64_t admitted =
      context->adaptive_batch_plan.admitted_bytes == 0
          ? std::min<std::uint64_t>(budget, 4096)
          : std::min<std::uint64_t>(budget,
                                    context->adaptive_batch_plan.admitted_bytes);
  const std::uint64_t grant_bytes = std::max<std::uint64_t>(64, admitted);
  context->memory_arena_requested_bytes = grant_bytes;

  mem::QueryMemoryContext memory_context;
  memory_context.query_id =
      request.context.request_id.empty() ? context->statement_uuid
                                         : request.context.request_id;
  memory_context.statement_id = context->statement_uuid;
  memory_context.session_id =
      request.context.session_uuid.canonical.empty()
          ? ("session:" + context->security_context_uuid)
          : request.context.session_uuid.canonical;
  memory_context.transaction_id =
      context->transaction_uuid.empty()
          ? std::to_string(context->local_transaction_id)
          : context->transaction_uuid;
  memory_context.database_id = context->database_uuid.empty()
                                   ? request.context.database_path
                                   : context->database_uuid;
  memory_context.engine_id = "scratchbird_engine_insert";
  memory_context.operation_id = "insert_batch";
  memory_context.engine_mga_authoritative = true;
  memory_context.parser_or_reference_finality_or_visibility_authority = false;
  memory_context.client_finality_or_visibility_authority = false;
  memory_context.provider_finality_or_visibility_authority = false;
  memory_context.wal_recovery_or_finality_authority = false;

  mem::QueryMemoryArenaLimits limits;
  limits.hard_limit_bytes = std::max<std::uint64_t>(budget, grant_bytes);
  limits.soft_limit_bytes =
      std::max<std::uint64_t>(grant_bytes, limits.hard_limit_bytes / 2);
  limits.family_limit_bytes = limits.hard_limit_bytes;
  limits.query_limit_bytes = limits.hard_limit_bytes;
  limits.spill_limit_bytes = 0;
  limits.allow_spill = false;
  limits.require_hierarchical_reservation = false;

  mem::QueryMemoryArena arena(memory_context,
                              limits,
                              &InsertMemoryAllocator(),
                              nullptr,
                              nullptr,
                              nullptr);
  mem::QueryMemoryGrantRequest grant_request;
  grant_request.family = mem::QueryMemoryFamily::dml;
  grant_request.bytes = grant_bytes;
  grant_request.spillable = false;
  grant_request.purpose = "insert_batch_canonical_row_scratch";

  auto grant = arena.Grant(grant_request);
  if (!grant.ok() || !grant.grant.has_value()) {
    context->memory_arena_fail_closed = true;
    context->evidence.push_back({"insert_memory_arena_fail_closed", "true"});
    context->diagnostics.push_back(MakeInvalidRequestDiagnostic(
        "dml.insert_rows",
        "insert_memory_arena_refused:" + grant.diagnostic.message_key));
    context->fallback_reason = "insert_memory_arena_refused";
    context->accepted = false;
    return;
  }

  context->memory_arena_granted = true;
  context->memory_arena_granted_bytes = grant.grant->bytes;
  context->memory_arena_peak_bytes = grant.counters.peak_bytes;
  for (const auto& evidence : grant.evidence) {
    const auto equals = evidence.find('=');
    context->evidence.push_back(
        equals == std::string::npos
            ? EngineEvidenceReference{"insert_memory_arena.evidence", evidence}
            : EngineEvidenceReference{"insert_memory_arena." +
                                          evidence.substr(0, equals),
                                      evidence.substr(equals + 1)});
  }

  auto release = arena.Release(grant.grant->grant_id);
  context->memory_arena_released = release.ok();
  context->memory_arena_leak_count = release.counters.leak_count;
  for (const auto& evidence : release.evidence) {
    const auto equals = evidence.find('=');
    context->evidence.push_back(
        equals == std::string::npos
            ? EngineEvidenceReference{"insert_memory_arena.evidence", evidence}
            : EngineEvidenceReference{"insert_memory_arena." +
                                          evidence.substr(0, equals),
                                      evidence.substr(equals + 1)});
  }
  if (!release.ok()) {
    context->memory_arena_fail_closed = true;
    context->evidence.push_back({"insert_memory_arena_fail_closed", "true"});
    context->diagnostics.push_back(MakeInvalidRequestDiagnostic(
        "dml.insert_rows",
        "insert_memory_arena_release_failed:" +
            release.diagnostic.message_key));
    context->fallback_reason = "insert_memory_arena_release_failed";
    context->accepted = false;
    return;
  }

  auto reset = arena.Reset();
  context->memory_arena_reset = reset.ok();
  context->memory_arena_leak_count = reset.counters.leak_count;
  for (const auto& evidence : reset.evidence) {
    const auto equals = evidence.find('=');
    context->evidence.push_back(
        equals == std::string::npos
            ? EngineEvidenceReference{"insert_memory_arena.evidence", evidence}
            : EngineEvidenceReference{"insert_memory_arena." +
                                          evidence.substr(0, equals),
                                      evidence.substr(equals + 1)});
  }
  if (!reset.ok()) {
    context->memory_arena_fail_closed = true;
    context->evidence.push_back({"insert_memory_arena_fail_closed", "true"});
    context->diagnostics.push_back(MakeInvalidRequestDiagnostic(
        "dml.insert_rows",
        "insert_memory_arena_reset_failed:" + reset.diagnostic.message_key));
    context->fallback_reason = "insert_memory_arena_reset_failed";
    context->accepted = false;
  }
}

InsertBatchContext BeginInsertBatchContext(const EngineInsertRowsRequest& request,
                                           const CrudState& state,
                                           const CrudTableRecord& table,
                                           const std::vector<CrudIndexRecord>& indexes) {
  InsertBatchContext context;
  context.statement_uuid = request.context.request_id.empty() ? GenerateCrudEngineUuid("transaction") : request.context.request_id;
  context.local_transaction_id = request.context.local_transaction_id;
  context.transaction_uuid = request.context.transaction_uuid.canonical;
  context.database_uuid = request.context.database_uuid.canonical;
  context.schema_uuid = request.target_schema.uuid.canonical;
  context.target_object_uuid = TargetUuid(request);
  context.estimated_row_count = EstimateRows(request);
  context.insert_mode = ResolveInsertBatchMode(request);
  context.duplicate_mode = ResolveInsertDuplicateMode(request);
  context.security_context_uuid = request.context.principal_uuid.canonical;
  context.policy_snapshot_uuid = InsertBatchOptionValue(request, "policy_snapshot_uuid=");
  context.feature_gates = ResolveInsertFeatureGates(request);
  context.memory_policy = ResolveInsertMemoryPolicy(request);
  context.delta_ledger_policy = ResolveSecondaryIndexDeltaLedgerPolicy(request, context.feature_gates);
  const auto descriptor_resolution =
      ResolvePreparedInsertDescriptor(request,
                                      state,
                                      table,
                                      indexes,
                                      context.feature_gates,
                                      context.delta_ledger_policy);
  const auto& descriptor = descriptor_resolution.descriptor;
  const bool descriptor_hit = descriptor_resolution.cache_hit;
  context.prepared_descriptor_cache_key = descriptor.cache_key;
  context.prepared_descriptor_id = descriptor.descriptor_id;
  context.prepared_descriptor_authorization_digest = descriptor.authorization_digest;
  context.prepared_descriptor_principal_uuid = descriptor.principal_uuid;
  context.prepared_descriptor_role_uuid = descriptor.role_uuid;
  context.prepared_descriptor_session_uuid = descriptor.session_uuid;
  context.prepared_descriptor_generation = descriptor.generation;
  context.prepared_descriptor_catalog_epoch = descriptor.catalog_epoch;
  context.prepared_descriptor_security_epoch = descriptor.security_epoch;
  context.prepared_descriptor_policy_epoch = descriptor.policy_epoch;
  context.prepared_descriptor_cache_limit =
      descriptor_resolution.pressure_policy.cache_limit;
  context.prepared_descriptor_effective_cache_limit =
      descriptor_resolution.pressure_policy.effective_cache_limit;
  context.prepared_descriptor_trim_target_entries =
      descriptor_resolution.pressure_policy.trim_target_entries;
  context.prepared_descriptor_trim_entries_before =
      descriptor_resolution.trim_result.entries_before;
  context.prepared_descriptor_trim_entries_after =
      descriptor_resolution.trim_result.entries_after;
  context.prepared_descriptor_trim_evictions =
      descriptor_resolution.trim_result.evictions;
  context.prepared_descriptor_memory_pressure_detected =
      descriptor_resolution.pressure_policy.memory_pressure_detected;
  context.prepared_descriptor_trim_requested =
      descriptor_resolution.pressure_policy.trim_requested;
  context.prepared_descriptor_backoff_active =
      descriptor_resolution.pressure_policy.backoff_active;
  context.prepared_descriptor_pressure_reason =
      descriptor_resolution.pressure_policy.pressure_reason;
  {
    const std::lock_guard<std::mutex> guard(PreparedInsertDescriptorCacheMutex());
    context.prepared_descriptor_cache_size =
        static_cast<std::uint64_t>(PreparedInsertDescriptorCache().size());
    context.prepared_descriptor_eviction_count =
        PreparedInsertDescriptorEvictionCounter();
  }
  context.prepared_descriptor_cache_hit = descriptor_hit;
  context.row_template = descriptor.row_template;
  context.row_encoder_plan = descriptor.row_encoder_plan;
  context.index_plan = descriptor.index_plan;
  const auto descriptor_validation =
      ValidatePreparedInsertDescriptorAuthority(request, descriptor);
  context.prepared_descriptor_authority_refused = !descriptor_validation.ok;
  context.prepared_descriptor_refusal_reason = descriptor_validation.refusal_reason;
  if (context.prepared_descriptor_memory_pressure_detected) {
    if (context.prepared_descriptor_authority_refused) {
      context.prepared_descriptor_authority_after_trim = "refused_before_execution";
    } else if (context.prepared_descriptor_cache_hit) {
      context.prepared_descriptor_authority_after_trim = "retained_and_revalidated";
    } else {
      context.prepared_descriptor_authority_after_trim = "compiled_and_revalidated";
    }
  } else {
    context.prepared_descriptor_authority_after_trim =
        context.prepared_descriptor_authority_refused
            ? "refused_before_execution"
            : "revalidated";
  }
  if (!descriptor_validation.ok) {
    context.diagnostics.push_back(MakeInvalidRequestDiagnostic(
        "dml.insert_rows",
        "prepared_descriptor_authority_refused:" +
            descriptor_validation.refusal_reason));
  }
  context.identity_reservation = ReserveInsertIdentityRange(request, context.row_template);
  context.page_reservation = ReserveInsertPages(request, context.row_template, context.estimated_row_count);
  context.bulk_load_policy = ResolveStrictBulkLoadPolicy(request);
  context.adaptive_batch_plan = PlanInsertAdaptiveBatch(request,
                                                        context.row_template,
                                                        context.index_plan,
                                                        context.memory_policy,
                                                        context.estimated_row_count);
  context.strict_bulk_load_selected = context.bulk_load_policy.requested && context.bulk_load_policy.enabled;
  context.accepted = !context.prepared_descriptor_authority_refused &&
                     !context.index_plan.rejected &&
                     context.page_reservation.reservation_available;
  CaptureInsertMemoryArenaProof(request, &context);
  if (!context.accepted) {
    if (context.prepared_descriptor_authority_refused) {
      context.fallback_reason =
          "prepared_descriptor_authority_refused:" +
          context.prepared_descriptor_refusal_reason;
    } else {
      context.fallback_reason = !context.index_plan.rejection_reason.empty()
                                    ? context.index_plan.rejection_reason
                                    : context.page_reservation.refusal_reason;
    }
  }
  AddInsertTrace(&context, "insert.batch.begin", "begin", InsertBatchModeName(context.insert_mode));
  AddInsertTrace(&context,
                 "insert.prepared_descriptor",
                 descriptor_hit ? "cache_hit" : "cache_miss",
                 context.prepared_descriptor_cache_key);
  if (context.prepared_descriptor_trim_requested) {
    AddInsertTrace(&context,
                   "insert.prepared_descriptor_trim",
                   context.prepared_descriptor_backoff_active ? "backoff" : "trim",
                   std::to_string(context.prepared_descriptor_trim_entries_before) +
                       "->" +
                       std::to_string(context.prepared_descriptor_trim_entries_after));
  }
  AddInsertTrace(&context, "insert.template.bind", "bind", context.row_template.template_id);
  AddInsertTrace(&context, "insert.identity.reserve", "reserve", context.identity_reservation.reservation_id);
  AddInsertTrace(&context, "insert.page.reserve", "reserve", context.page_reservation.reservation_id);
  context.evidence.push_back({"insert_batch_context", context.statement_uuid});
  context.evidence.push_back({"prepared_insert_descriptor",
                              context.prepared_descriptor_id});
  context.evidence.push_back({"prepared_descriptor_cache_key",
                              context.prepared_descriptor_cache_key});
  context.evidence.push_back({"prepared_descriptor_cache",
                              descriptor_hit ? "hit" : "miss"});
  context.evidence.push_back({"prepared_descriptor_memory_pressure",
                              context.prepared_descriptor_memory_pressure_detected
                                  ? context.prepared_descriptor_pressure_reason
                                  : "not_detected"});
  context.evidence.push_back({"prepared_descriptor_trim_requested",
                              context.prepared_descriptor_trim_requested ? "true" : "false"});
  context.evidence.push_back({"prepared_descriptor_trim_target_entries",
                              std::to_string(context.prepared_descriptor_trim_target_entries)});
  context.evidence.push_back({"prepared_descriptor_cache_limit",
                              std::to_string(context.prepared_descriptor_cache_limit)});
  context.evidence.push_back({"prepared_descriptor_effective_cache_limit",
                              std::to_string(context.prepared_descriptor_effective_cache_limit)});
  context.evidence.push_back({"prepared_descriptor_backoff_active",
                              context.prepared_descriptor_backoff_active ? "true" : "false"});
  context.evidence.push_back({"prepared_descriptor_trim_entries_before",
                              std::to_string(context.prepared_descriptor_trim_entries_before)});
  context.evidence.push_back({"prepared_descriptor_trim_entries_after",
                              std::to_string(context.prepared_descriptor_trim_entries_after)});
  context.evidence.push_back({"prepared_descriptor_trim_evictions",
                              std::to_string(context.prepared_descriptor_trim_evictions)});
  context.evidence.push_back({"prepared_descriptor_authority_after_trim",
                              context.prepared_descriptor_authority_after_trim});
  context.evidence.push_back({"prepared_descriptor_authority_scope",
                              "principal_role_session_epoch_authorization"});
  context.evidence.push_back({"prepared_descriptor_principal_uuid",
                              context.prepared_descriptor_principal_uuid});
  context.evidence.push_back({"prepared_descriptor_role_uuid",
                              context.prepared_descriptor_role_uuid});
  context.evidence.push_back({"prepared_descriptor_session_uuid",
                              context.prepared_descriptor_session_uuid});
  context.evidence.push_back({"prepared_descriptor_authorization_digest",
                              context.prepared_descriptor_authorization_digest});
  context.evidence.push_back({"prepared_descriptor_generation",
                              std::to_string(context.prepared_descriptor_generation)});
  context.evidence.push_back({"prepared_descriptor_cache_size",
                              std::to_string(context.prepared_descriptor_cache_size)});
  context.evidence.push_back({"prepared_descriptor_eviction_count",
                              std::to_string(context.prepared_descriptor_eviction_count)});
  context.evidence.push_back({"prepared_descriptor_security_recheck",
                              "required"});
  context.evidence.push_back({"prepared_descriptor_epoch_catalog",
                              std::to_string(request.context.catalog_generation_id)});
  context.evidence.push_back({"prepared_descriptor_epoch_security",
                              std::to_string(request.context.security_epoch)});
  context.evidence.push_back({"prepared_descriptor_epoch_policy",
                              std::to_string(request.context.resource_epoch)});
  if (context.prepared_descriptor_authority_refused) {
    context.evidence.push_back({"prepared_descriptor_authority_refusal",
                                context.prepared_descriptor_refusal_reason});
    context.evidence.push_back({"prepared_descriptor_refused_before_execution",
                                "true"});
  }
  context.evidence.push_back({"insert_row_template", context.row_template.template_id});
  context.evidence.push_back({"insert_row_encoder_plan",
                              context.row_encoder_plan.plan_id});
  context.evidence.push_back({"insert_row_encoder_shape_signature",
                              context.row_encoder_plan.row_shape_signature});
  context.evidence.push_back({"insert_row_validator_signature",
                              context.row_encoder_plan.validator_signature});
  context.evidence.push_back({"insert_row_encoder_descriptor_state",
                              descriptor_hit ? "reused" : "compiled"});
  context.evidence.push_back({"insert_row_encoder_column_count",
                              std::to_string(context.row_encoder_plan.column_count)});
  context.evidence.push_back({"insert_validator_default_count",
                              std::to_string(context.row_encoder_plan.default_validator_count)});
  context.evidence.push_back({"insert_validator_domain_count",
                              std::to_string(context.row_encoder_plan.domain_validator_count)});
  context.evidence.push_back({"insert_validator_check_count",
                              std::to_string(context.row_encoder_plan.check_validator_count)});
  context.evidence.push_back({"insert_validator_not_null_count",
                              std::to_string(context.row_encoder_plan.not_null_validator_count)});
  context.evidence.push_back({"insert_validator_unique_count",
                              std::to_string(context.row_encoder_plan.unique_validator_count)});
  context.evidence.push_back({"insert_validator_foreign_key_count",
                              std::to_string(context.row_encoder_plan.foreign_key_validator_count)});
  context.evidence.push_back({"insert_validator_runtime_policy_recheck_count",
                              std::to_string(context.row_encoder_plan.runtime_policy_recheck_count)});
  context.evidence.push_back({"insert_validator_unsupported_sblr_fail_closed",
                              context.row_encoder_plan.unsupported_sblr_validators_fail_closed ? "true" : "false"});
  context.evidence.push_back({"insert_index_plan", context.index_plan.plan_id});
  context.evidence.push_back({"insert_adaptive_batch_requested_rows",
                              std::to_string(context.adaptive_batch_plan.requested_rows)});
  context.evidence.push_back({"insert_adaptive_batch_admitted_rows",
                              std::to_string(context.adaptive_batch_plan.admitted_rows)});
  context.evidence.push_back({"insert_adaptive_batch_estimated_row_bytes",
                              std::to_string(context.adaptive_batch_plan.estimated_row_bytes)});
  context.evidence.push_back({"insert_adaptive_batch_admitted_bytes",
                              std::to_string(context.adaptive_batch_plan.admitted_bytes)});
  context.evidence.push_back({"insert_adaptive_batch_index_count",
                              std::to_string(context.adaptive_batch_plan.index_count)});
  context.evidence.push_back({"insert_adaptive_batch_page_size_bytes",
                              std::to_string(context.adaptive_batch_plan.page_size_bytes)});
  context.evidence.push_back({"insert_adaptive_batch_page_window_rows",
                              std::to_string(context.adaptive_batch_plan.page_window_rows)});
  context.evidence.push_back({"insert_adaptive_batch_commit_window_rows",
                              std::to_string(context.adaptive_batch_plan.commit_window_rows)});
  context.evidence.push_back({"insert_adaptive_batch_contention_window_rows",
                              std::to_string(context.adaptive_batch_plan.contention_window_rows)});
  context.evidence.push_back({"insert_adaptive_batch_reason",
                              context.adaptive_batch_plan.reason});
  context.evidence.push_back({"insert_adaptive_batch_reduced",
                              context.adaptive_batch_plan.reduced ? "true" : "false"});
  if (context.identity_reservation.range_reserved) {
    context.evidence.push_back({"identity_range_reservation", context.identity_reservation.reservation_id});
  }
  if (context.page_reservation.reservation_available) {
    context.evidence.push_back({"page_reservation", context.page_reservation.reservation_id});
    context.evidence.push_back({"insert_page_reservation_plan_only", "true"});
    context.evidence.push_back({"insert_page_reservation_requested_pages",
                                std::to_string(context.page_reservation.requested_pages)});
    context.evidence.push_back({"insert_page_preallocation_claim",
                                "reservation_plan_only"});
    context.evidence.push_back({"insert_page_preallocation_target_available_pages",
                                std::to_string(context.page_reservation.target_available_pages)});
    context.evidence.push_back({"insert_page_preallocation_notify_below_pages",
                                std::to_string(context.page_reservation.notify_below_pages)});
  }
  if (context.delta_ledger_policy.synchronous_fallback_required) {
    context.evidence.push_back({"insert_index_maintenance_mode", "synchronous_fallback"});
    context.evidence.push_back({"insert_deferred_secondary_index_fallback_reason",
                                context.delta_ledger_policy.fallback_reason});
  } else {
    context.evidence.push_back({"insert_index_maintenance_mode", "deferred_secondary_index"});
  }
  return context;
}

EngineApiDiagnostic ValidateStrictBulkLoadEligibility(const InsertBatchContext& context,
                                                      const CrudTableRecord& table) {
  if (!context.bulk_load_policy.requested) {
    return OkDiagnostic();
  }
  if (!context.bulk_load_policy.enabled) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "strict_bulk_load_policy_not_enabled");
  }
  if (context.row_template.has_opaque_render_only_column && !context.bulk_load_policy.allow_opaque_columns) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "strict_bulk_load_opaque_column_refused");
  }
  if (context.bulk_load_policy.target_empty_required && !table.table_uuid.empty() && context.estimated_row_count == 0) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "strict_bulk_load_row_estimate_required");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateInsertBatchMemoryBudget(const InsertBatchContext& context,
                                                    std::uint64_t projected_bytes) {
  if (projected_bytes > context.memory_policy.context_budget_bytes && !context.memory_policy.spill_allowed) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "insert_batch_memory_budget_exceeded");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateInsertBatchConstraints(const InsertBatchContext&,
                                                   const CrudState&,
                                                   const PreparedInsertRow&) {
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateInsertBatchUniquePreflight(InsertBatchContext* context,
                                                       const std::vector<std::pair<std::string, std::string>>& values) {
  if (context == nullptr) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows", "insert_batch_context_required");
  }
  for (const auto& entry : context->index_plan.entries) {
    if (!IsUniqueIndex(entry.index)) {
      continue;
    }
    for (const auto& key : CrudIndexKeysForValues(entry.index, values)) {
      const std::string request_key = entry.index.index_uuid + "|" + key;
      if (!context->unique_request_keys.insert(request_key).second) {
        return MakeInvalidRequestDiagnostic("dml.insert_rows", "unique_index_duplicate");
      }
    }
  }
  return OkDiagnostic();
}

PreparedInsertRow PrepareInsertRowForBatch(const EngineInsertRowsRequest& request,
                                           const EngineRowValue& input_row,
                                           const BoundInsertRowTemplate& row_template) {
  InsertRowEncoderPlan empty_plan;
  return PrepareInsertRowForBatch(request, input_row, row_template, empty_plan);
}

PreparedInsertRow PrepareInsertRowForBatch(const EngineInsertRowsRequest& request,
                                           const EngineRowValue& input_row,
                                           const BoundInsertRowTemplate& row_template,
                                           const InsertRowEncoderPlan& row_encoder_plan) {
  PreparedInsertRow row;
  if (row_encoder_plan.columns.empty()) {
    row.values = RowValuePairs(input_row);
  } else {
    std::vector<bool> consumed(input_row.fields.size(), false);
    for (const auto& column : row_encoder_plan.columns) {
      for (std::size_t index = 0; index < input_row.fields.size(); ++index) {
        if (consumed[index] || input_row.fields[index].first != column.column_name) {
          continue;
        }
        const auto& typed = input_row.fields[index].second;
        row.values.push_back({input_row.fields[index].first,
                              typed.is_null ? "<NULL>" : typed.encoded_value});
        consumed[index] = true;
        break;
      }
    }
    for (std::size_t index = 0; index < input_row.fields.size(); ++index) {
      if (consumed[index]) {
        continue;
      }
      const auto& typed = input_row.fields[index].second;
      row.values.push_back({input_row.fields[index].first,
                            typed.is_null ? "<NULL>" : typed.encoded_value});
    }
  }
  row.row_uuid = UuidStringOrGenerated(input_row.requested_row_uuid, "row");
  row.encoded_bytes = static_cast<std::uint64_t>(EncodedValueBytes(row.values));
  row.toast_required = row.encoded_bytes > row_template.max_inline_encoded_bytes ||
                       InsertBatchOptionEnabled(request, "large_value.force_toast=true");
  return row;
}

EngineApiDiagnostic AppendSecondaryIndexDeltaLedgerEntries(const EngineRequestContext& request_context,
                                                           const InsertBatchContext& context,
                                                           const PreparedInsertRow& row,
                                                           const std::string& version_uuid) {
  if (!context.delta_ledger_policy.enabled) {
    return OkDiagnostic();
  }
  // DPC_DEFERRED_INDEX_WRITE_PATH
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  for (const auto& plan_entry : context.index_plan.entries) {
    if (plan_entry.action != InsertIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    MgaSecondaryIndexDeltaLedgerEntryInput input;
    input.index = plan_entry.index;
    input.table_uuid = context.target_object_uuid;
    input.row_uuid = row.row_uuid;
    input.version_uuid = version_uuid;
    input.values = row.values;
    input.delta_kind = idx::SecondaryIndexDeltaKind::insert;
    input.source_evidence_reference =
        "engine.dml.insert.secondary_index_delta:" + context.statement_uuid;
    entries.push_back(std::move(input));
  }
  return AppendMgaSecondaryIndexDeltaLedgerEntries(request_context, entries, nullptr);
}

void AddInsertTrace(InsertBatchContext* context, std::string event_name, std::string phase, std::string detail) {
  if (context == nullptr) {
    return;
  }
  ++context->trace_event_count;
  constexpr std::size_t kMaxStoredInsertTraceEvents = 128;
  if (context->trace_events.size() >= kMaxStoredInsertTraceEvents) {
    ++context->trace_event_compacted_count;
    return;
  }
  context->trace_events.push_back({std::move(event_name), std::move(phase), std::move(detail)});
}

void AddInsertBatchEvidenceToResult(const InsertBatchContext& context, EngineApiResult* result) {
  if (result == nullptr) {
    return;
  }
  for (const auto& evidence : context.evidence) {
    result->evidence.push_back(evidence);
  }
  constexpr std::size_t kMaxInlineInsertTraceEvidence = 128;
  constexpr std::size_t kHeadInsertTraceEvidence = 48;
  constexpr std::size_t kTailInsertTraceEvidence = 24;
  const auto append_trace = [result](const InsertBatchTraceEvent& trace) {
    result->evidence.push_back({"insert_trace", trace.event_name + ":" + trace.phase + ":" + trace.detail});
  };
  if (context.trace_event_count <= kMaxInlineInsertTraceEvidence) {
    for (const auto& trace : context.trace_events) {
      append_trace(trace);
    }
  } else {
    result->evidence.push_back({"insert_trace_compacted", "true"});
    result->evidence.push_back({"insert_trace_event_count",
                                std::to_string(context.trace_event_count)});
    result->evidence.push_back({"insert_trace_collection_omitted_count",
                                std::to_string(context.trace_event_compacted_count)});
    const std::size_t head_count =
        std::min(kHeadInsertTraceEvidence, context.trace_events.size());
    for (std::size_t index = 0; index < head_count; ++index) {
      append_trace(context.trace_events[index]);
    }
    const std::size_t tail_start =
        context.trace_events.size() > kTailInsertTraceEvidence
            ? context.trace_events.size() - kTailInsertTraceEvidence
            : head_count;
    if (tail_start > head_count) {
      result->evidence.push_back({"insert_trace_omitted_count",
                                  std::to_string(tail_start - head_count)});
    }
    for (std::size_t index = std::max(head_count, tail_start);
         index < context.trace_events.size();
         ++index) {
      append_trace(context.trace_events[index]);
    }
  }
  result->evidence.push_back({"insert_feature_gate.secondary_index_delta_ledger", InsertFeatureStateName(context.feature_gates.secondary_index_delta_ledger)});
  result->evidence.push_back({"insert_runtime.deferred_secondary_index", context.delta_ledger_policy.runtime_enabled ? "enabled" : "disabled"});
  result->evidence.push_back({"insert_delta_ledger_policy", context.delta_ledger_policy.enabled ? "enabled" : "synchronous_fallback"});
  if (context.delta_ledger_policy.synchronous_fallback_required) {
    result->evidence.push_back({"insert_deferred_secondary_index_fallback_reason",
                                context.delta_ledger_policy.fallback_reason});
  }
  result->evidence.push_back({"insert_feature_gate.strict_bulk_load", InsertFeatureStateName(context.feature_gates.strict_bulk_load)});
  result->evidence.push_back({"insert_memory_context_budget_bytes",
                              std::to_string(context.memory_policy.context_budget_bytes)});
  result->evidence.push_back({"insert_memory_adaptive_requested_bytes",
                              std::to_string(context.adaptive_batch_plan.requested_bytes)});
  result->evidence.push_back({"insert_memory_adaptive_admitted_bytes",
                              std::to_string(context.adaptive_batch_plan.admitted_bytes)});
  result->evidence.push_back({"insert_memory_adaptive_page_size_bytes",
                              std::to_string(context.adaptive_batch_plan.page_size_bytes)});
  result->evidence.push_back({"insert_memory_adaptive_commit_window_rows",
                              std::to_string(context.adaptive_batch_plan.commit_window_rows)});
  result->evidence.push_back({"insert_memory_adaptive_contention_window_rows",
                              std::to_string(context.adaptive_batch_plan.contention_window_rows)});
  result->evidence.push_back({"insert_memory_pressure",
                              context.adaptive_batch_plan.reduced
                                  ? context.adaptive_batch_plan.reason
                                  : "within_policy"});
  result->evidence.push_back({"insert_memory_arena_reuse_claim",
                              context.prepared_descriptor_cache_hit
                                  ? "prepared_descriptor_cache_reuse"
                                  : "request_local_vectors"});
  result->evidence.push_back({"insert_memory_arena_reuse_physical_arena_claimed",
                              context.memory_arena_granted ? "true" : "false"});
  result->evidence.push_back({"insert_memory_arena_grant_state",
                              context.memory_arena_granted ? "granted" : "not_granted"});
  result->evidence.push_back({"insert_memory_arena_release_state",
                              context.memory_arena_released ? "released" : "not_released"});
  result->evidence.push_back({"insert_memory_arena_reset_state",
                              context.memory_arena_reset ? "reset" : "not_reset"});
  result->evidence.push_back({"insert_memory_arena_fail_closed",
                              context.memory_arena_fail_closed ? "true" : "false"});
  result->evidence.push_back({"insert_memory_arena_requested_bytes",
                              std::to_string(context.memory_arena_requested_bytes)});
  result->evidence.push_back({"insert_memory_arena_granted_bytes",
                              std::to_string(context.memory_arena_granted_bytes)});
  result->evidence.push_back({"insert_memory_arena_peak_bytes",
                              std::to_string(context.memory_arena_peak_bytes)});
  result->evidence.push_back({"insert_memory_arena_leak_count",
                              std::to_string(context.memory_arena_leak_count)});
  if (!context.fallback_reason.empty()) {
    result->evidence.push_back({"insert_fallback_reason", context.fallback_reason});
  }
}

void RecordInsertBatchMetric(const InsertBatchContext& context, std::string metric, double value, std::string result, std::string reason) {
  if (metric == "sb_dml_insert_batch_started_total") {
    (void)scratchbird::core::metrics::RecordInsertBatchStarted(context.target_object_uuid,
                                                               InsertBatchModeName(context.insert_mode),
                                                               result);
    (void)scratchbird::core::metrics::RecordInsertPreparedDescriptorCache(
        context.target_object_uuid,
        InsertBatchModeName(context.insert_mode),
        context.prepared_descriptor_cache_hit);
    (void)scratchbird::core::metrics::PublishInsertAdaptiveBatchPlan(
        context.target_object_uuid,
        InsertBatchModeName(context.insert_mode),
        static_cast<double>(context.adaptive_batch_plan.requested_rows),
        static_cast<double>(context.adaptive_batch_plan.admitted_rows),
        static_cast<double>(context.adaptive_batch_plan.admitted_bytes),
        context.adaptive_batch_plan.reason);
    return;
  }
  if (metric == "sb_dml_insert_batch_fallback_total" ||
      metric == "sb_dml_insert_batch_fallback_reason_total") {
    (void)scratchbird::core::metrics::RecordInsertBatchFallback(context.target_object_uuid,
                                                                InsertBatchModeName(context.insert_mode),
                                                                reason.empty() ? "unspecified" : reason);
    (void)scratchbird::core::metrics::RecordInsertSlowPath(context.target_object_uuid,
                                                           InsertBatchModeName(context.insert_mode),
                                                           "refused",
                                                           reason.empty() ? "unspecified" : reason);
    return;
  }
  if (metric == "sb_dml_insert_relation_state_full_load_total") {
    (void)scratchbird::core::metrics::RecordInsertRelationStateLoad(
        context.target_object_uuid,
        InsertBatchModeName(context.insert_mode),
        true,
        false,
        reason.empty() ? "full_relation_state" : reason);
    return;
  }
  if (metric == "sb_dml_insert_relation_state_scoped_load_total") {
    (void)scratchbird::core::metrics::RecordInsertRelationStateLoad(
        context.target_object_uuid,
        InsertBatchModeName(context.insert_mode),
        false,
        true,
        reason.empty() ? "target_scoped_relation_state" : reason);
    return;
  }
  if (metric == "sb_dml_insert_allocation_stall_microseconds") {
    (void)scratchbird::core::metrics::ObserveInsertAllocationStall(
        value,
        context.target_object_uuid,
        InsertBatchModeName(context.insert_mode),
        reason.empty() ? "allocation" : reason,
        result.empty() ? "ok" : result);
    return;
  }
  if (metric == "sb_dml_insert_slow_path_total") {
    (void)scratchbird::core::metrics::RecordInsertSlowPath(
        context.target_object_uuid,
        InsertBatchModeName(context.insert_mode),
        result.empty() ? "degraded" : result,
        reason.empty() ? "unspecified" : reason);
    return;
  }
  if (metric == "sb_filespace_insert_growth_request_total") {
    (void)scratchbird::core::metrics::IncrementCounter(
        "sb_filespace_insert_growth_request_total",
        scratchbird::core::metrics::Labels(
            {{"component", "engine.insert"},
             {"object_uuid", context.target_object_uuid},
             {"operation", InsertBatchModeName(context.insert_mode)},
             {"result", result.empty() ? "requested" : result},
             {"reason", reason.empty() ? "none" : reason}}),
        value,
        kInsertMetricsProducer);
    return;
  }
  if (metric == "sb_filespace_insert_growth_wait_microseconds") {
    (void)scratchbird::core::metrics::ObserveHistogram(
        "sb_filespace_insert_growth_wait_microseconds",
        scratchbird::core::metrics::Labels(
            {{"component", "engine.insert"},
             {"object_uuid", context.target_object_uuid},
             {"operation", InsertBatchModeName(context.insert_mode)},
             {"result", result.empty() ? "ok" : result},
             {"reason", reason.empty() ? "filespace_growth" : reason}}),
        value,
        kInsertMetricsProducer);
    return;
  }
  if (metric == "sb_dml_insert_rows_inserted_total") {
    (void)scratchbird::core::metrics::RecordInsertRowsInserted(value,
                                                               context.target_object_uuid,
                                                               InsertBatchModeName(context.insert_mode));
    (void)scratchbird::core::metrics::ObserveInsertRowsPerBatch(static_cast<double>(context.actual_row_count),
                                                                context.target_object_uuid,
                                                                InsertBatchModeName(context.insert_mode));
    return;
  }
  if (metric == "sb_index_insert_unique_physical_probe_total") {
    (void)scratchbird::core::metrics::IncrementCounter(
        "sb_index_insert_unique_physical_probe_total",
        scratchbird::core::metrics::Labels(
            {{"component", "engine.insert"},
             {"object_uuid", context.target_object_uuid},
             {"operation", InsertBatchModeName(context.insert_mode)},
             {"result", result.empty() ? "physical_probe" : result},
             {"reason", reason.empty() ? "index_backed_unique_preflight" : reason}}),
        value,
        kInsertMetricsProducer);
    return;
  }
  (void)scratchbird::core::metrics::IncrementCounter(
      metric,
      scratchbird::core::metrics::Labels({{"component", "engine.insert"},
                                          {"object_uuid", context.target_object_uuid},
                                          {"operation", InsertBatchModeName(context.insert_mode)},
                                          {"result", std::move(result)},
                                          {"reason", reason.empty() ? "none" : std::move(reason)}}),
      value,
      kInsertMetricsProducer);
}

}  // namespace scratchbird::engine::internal_api
