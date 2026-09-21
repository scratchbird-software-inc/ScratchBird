// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "dml/insert_batch.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {

// An in-memory content key, not a UUID, persisted format or authority receipt.
// Cache equality compares the complete bytes. Digests below are labels only.
using InsertDescriptorKey = std::vector<std::uint8_t>;

struct InsertDescriptorKeyLess {
  bool operator()(const InsertDescriptorKey& left,
                  const InsertDescriptorKey& right) const noexcept {
    const auto count = std::min(left.size(), right.size());
    for (std::size_t i = 0; i != count; ++i)
      if (left[i] != right[i]) return left[i] < right[i];
    return left.size() < right.size();
  }
};

inline const char* InsertDescriptorExpectationFailure(
    const EngineInsertDescriptorExpectation& expected,
    const EngineUuid& principal, const EngineUuid& role,
    const EngineUuid& session, const InsertDescriptorKey& content_key) {
  if (expected.principal_uuid && *expected.principal_uuid != principal) return "cross_user";
  if (expected.role_uuid && *expected.role_uuid != role) return "cross_role";
  if (expected.session_uuid && *expected.session_uuid != session) return "cross_session";
  if (expected.content_key && *expected.content_key != content_key) return "stale_descriptor_key";
  return nullptr;
}

class InsertDescriptorKeyEncoder {
 public:
  explicit InsertDescriptorKeyEncoder(std::uint64_t domain) { Number(domain); }
  void Number(std::uint64_t value) {
    bytes_.push_back(1);
    Length(value);
  }
  void Uuid(const EngineUuid& value) {
    bytes_.push_back(2);
    bytes_.insert(bytes_.end(), value.bytes.begin(), value.bytes.end());
  }
  void Text(std::string_view value) {
    bytes_.push_back(3);
    Length(value.size());
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  void Bytes(const InsertDescriptorKey& value) {
    bytes_.push_back(4);
    Length(value.size());
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  void Texts(const std::vector<std::string>& values) {
    Number(values.size());
    for (const auto& value : values) Text(value);
  }
  void Unordered(std::vector<InsertDescriptorKey> values) {
    std::sort(values.begin(), values.end(), InsertDescriptorKeyLess{});
    Number(values.size());
    for (const auto& value : values) Bytes(value);
  }
  InsertDescriptorKey Finish() && { return std::move(bytes_); }

 private:
  void Length(std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i)
      bytes_.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  InsertDescriptorKey bytes_;
};

inline std::string InsertDescriptorKeyLabel(std::string_view kind,
                                           const InsertDescriptorKey& key) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(key);
  if (!digest.ok()) throw std::runtime_error("insert descriptor digest failed");
  return std::string(kind) + ":" + scratchbird::core::hash::HexLower(digest.digest);
}

inline InsertDescriptorKey InsertPolicyKey(const EngineMaterializedAuthorizationPolicy& policy) {
  InsertDescriptorKeyEncoder out(3);
  out.Uuid(policy.policy_uuid);
  out.Uuid(policy.subject_uuid);
  out.Text(policy.subject_kind);
  out.Uuid(policy.target_uuid);
  out.Text(policy.right);
  out.Text(policy.policy_kind);
  out.Number(policy.deny);
  out.Number(policy.requires_runtime_recheck);
  out.Number(policy.source_policy_generation);
  out.Number(policy.policy_epoch);
  out.Text(policy.canonical_policy_envelope);
  out.Number(policy.update_policy_phase);
  out.Uuid(policy.effective_policy_uuid);
  out.Number(policy.effective_policy_generation);
  out.Uuid(policy.effective_expression_uuid);
  out.Number(policy.effective_expression_generation);
  out.Bytes(InsertDescriptorKey(policy.effective_expression_evidence_sha256.begin(),
                                policy.effective_expression_evidence_sha256.end()));
  return std::move(out).Finish();
}

inline InsertDescriptorKey InsertAuthorizationKey(const EngineRequestContext& context) {
  InsertDescriptorKeyEncoder out(1);
  out.Number(context.security_context_present);
  out.Uuid(context.principal_uuid);
  out.Uuid(context.current_role_uuid);
  out.Number(context.security_epoch);
  out.Number(context.resource_epoch);
  out.Number(context.catalog_generation_id);
  const auto& auth = context.authorization_context;
  out.Number(auth.present);
  out.Uuid(auth.authority_uuid);
  out.Number(auth.security_context_generation);
  out.Uuid(auth.principal_uuid);
  out.Number(auth.security_epoch);
  out.Number(auth.policy_epoch);
  out.Number(auth.catalog_generation_id);
  std::vector<InsertDescriptorKey> subjects, grants, policies;
  for (const auto& subject : auth.effective_subjects) {
    InsertDescriptorKeyEncoder item(2);
    item.Uuid(subject.subject_uuid);
    item.Text(subject.subject_kind);
    subjects.push_back(std::move(item).Finish());
  }
  for (const auto& grant : auth.grants) {
    InsertDescriptorKeyEncoder item(4);
    item.Uuid(grant.grant_uuid);
    item.Uuid(grant.subject_uuid);
    item.Text(grant.subject_kind);
    item.Uuid(grant.target_uuid);
    item.Text(grant.right);
    item.Number(grant.deny);
    item.Number(grant.security_epoch);
    grants.push_back(std::move(item).Finish());
  }
  for (const auto& policy : auth.policies) policies.push_back(InsertPolicyKey(policy));
  out.Unordered(std::move(subjects));
  out.Unordered(std::move(grants));
  out.Unordered(std::move(policies));
  auto tags = auth.evidence_tags;
  std::sort(tags.begin(), tags.end());
  out.Texts(tags);
  return std::move(out).Finish();
}

inline InsertDescriptorKey InsertIndexKey(const CrudIndexRecord& index) {
  InsertDescriptorKeyEncoder out(5);
  out.Uuid(index.index_uuid);
  out.Uuid(index.table_uuid);
  out.Number(index.creator_tx);
  out.Number(index.event_sequence);
  out.Text(index.column_name);
  out.Text(index.family);
  out.Text(index.profile);
  out.Number(index.unique);
  out.Number(index.approximate);
  out.Number(index.exact_fallback);
  out.Text(index.predicate_kind);
  out.Text(index.predicate_column);
  out.Text(index.predicate_value);
  // Key order and the distinction between keys and included fields matter.
  out.Texts(index.key_envelopes);
  out.Texts(index.include_columns);
  return std::move(out).Finish();
}

inline InsertDescriptorKey BuildInsertDescriptorKey(
    const EngineInsertRowsRequest& request, const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes, const InsertFeatureGates& gates,
    const SecondaryIndexDeltaLedgerPolicy& delta,
    InsertBatchMode mode, InsertDuplicateMode duplicate, bool strict_bulk) {
  InsertDescriptorKeyEncoder out(6);
  const auto& context = request.context;
  out.Text(context.database_path);
  out.Uuid(context.database_uuid);
  out.Uuid(context.session_uuid);
  out.Uuid(context.principal_uuid);
  out.Uuid(context.current_role_uuid);
  out.Uuid(context.transaction_uuid);
  out.Number(context.local_transaction_id);
  out.Uuid(context.statement_snapshot_uuid);
  out.Number(context.statement_snapshot_generation);
  out.Uuid(context.catalog_epoch_uuid);
  out.Number(context.catalog_generation_id);
  out.Number(context.security_epoch);
  out.Number(context.resource_epoch);
  out.Number(context.name_resolution_epoch);
  out.Bytes(InsertAuthorizationKey(context));
  const auto& bound = request.bound_object_identity;
  out.Uuid(bound.object_uuid);
  out.Uuid(bound.resolved_schema_uuid);
  out.Uuid(bound.parent_object_uuid);
  out.Text(bound.resolved_object_type);
  out.Number(bound.object_descriptor_generation);
  out.Number(bound.catalog_generation_id);
  out.Number(bound.security_epoch);
  out.Number(bound.resource_epoch);
  out.Uuid(request.target_table.uuid);
  out.Uuid(request.target_object.uuid);
  out.Uuid(request.target_schema.uuid);
  out.Uuid(table.table_uuid);
  out.Number(table.creator_tx);
  out.Number(table.event_sequence);
  out.Number(table.bound_relation_generation);
  out.Number(table.bound_column_generation);
  out.Number(table.temporary);
  out.Text(table.temporary_scope);
  out.Uuid(table.temporary_session_uuid);
  out.Text(table.on_commit_action);
  out.Number(table.columns.size());
  for (const auto& column : table.columns) {
    out.Text(column.first);
    out.Text(column.second);
  }
  out.Number(indexes.size());
  for (const auto& index : indexes) out.Bytes(InsertIndexKey(index));
  out.Number(request.require_generated_row_uuid);
  out.Number(static_cast<std::uint64_t>(mode));
  out.Number(static_cast<std::uint64_t>(duplicate));
  out.Number(strict_bulk);
  out.Number(request.strict_bulk_load_requested);
  out.Number(static_cast<std::uint64_t>(gates.insert_batch_context));
  out.Number(static_cast<std::uint64_t>(gates.bound_row_template));
  out.Number(static_cast<std::uint64_t>(gates.page_reservation));
  out.Number(static_cast<std::uint64_t>(gates.identity_range_reservation));
  out.Number(static_cast<std::uint64_t>(gates.exact_unique_preflight));
  out.Number(static_cast<std::uint64_t>(gates.deferred_secondary_index_runtime));
  out.Number(static_cast<std::uint64_t>(gates.secondary_index_delta_ledger));
  out.Number(static_cast<std::uint64_t>(gates.strict_bulk_load));
  out.Number(static_cast<std::uint64_t>(gates.sorted_run_shadow_load));
  out.Number(delta.enabled);
  out.Number(delta.runtime_enabled);
  out.Number(delta.readers_overlay_committed_deltas);
  out.Number(delta.cleanup_horizon_bound);
  out.Number(delta.recovery_classifiable);
  out.Number(delta.synchronous_fallback_required);
  out.Text(delta.fallback_reason);
  std::vector<std::string> options;
  for (const auto& option : request.option_envelopes)
    if (!option.starts_with("prepared_descriptor.")) options.push_back(option);
  // Repeated options can have first/last-wins semantics: never sort them.
  out.Texts(options);
  return std::move(out).Finish();
}

}  // namespace scratchbird::engine::internal_api
