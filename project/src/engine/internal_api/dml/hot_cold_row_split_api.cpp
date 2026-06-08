// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/hot_cold_row_split_api.hpp"

#include "api_diagnostics.hpp"
#include "uuid.hpp"

#include <algorithm>

namespace scratchbird::engine::internal_api {
namespace {

namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;

platform::TypedUuid ParseUuid(platform::UuidKind kind, const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(kind, text);
  return parsed.ok() ? parsed.value : platform::TypedUuid{};
}

EngineApiDiagnostic StorageDiagnostic(const std::string& operation,
                                      const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  return MakeInvalidRequestDiagnostic(operation, diagnostic.diagnostic_code);
}

const EngineHotColdFieldPolicy* PolicyForField(
    const std::vector<EngineHotColdFieldPolicy>& policies,
    const std::string& field_name) {
  const auto found = std::find_if(policies.begin(), policies.end(), [&](const auto& policy) {
    return policy.field_name == field_name;
  });
  return found == policies.end() ? nullptr : &(*found);
}

std::vector<page::HotColdFieldInput> BuildFieldInputs(
    const EngineDmlHotColdSplitRequest& request) {
  std::vector<page::HotColdFieldInput> fields;
  fields.reserve(request.row.fields.size());
  for (const auto& [name, value] : request.row.fields) {
    page::HotColdFieldInput field;
    field.field_name = name;
    field.encoded_value = value.encoded_value;
    if (const auto* policy = PolicyForField(request.field_policy, name)) {
      field.metadata = policy->metadata;
      field.indexed = policy->indexed;
      field.frequently_filtered = policy->frequently_filtered;
      field.rare_projection = policy->rare_projection;
      field.force_hot = policy->force_hot;
      field.force_cold = policy->force_cold;
    }
    fields.push_back(std::move(field));
  }
  return fields;
}

void AddStorageEvidence(const std::vector<std::string>& source,
                        std::vector<EngineEvidenceReference>* target) {
  for (const auto& item : source) {
    target->push_back({"hot_cold_row_split", item});
  }
}

page::HotColdRowSplitRequest BuildStorageRequest(
    const EngineDmlHotColdSplitRequest& request) {
  page::HotColdRowSplitRequest storage;
  storage.large_payload_store = request.large_payload_store;
  storage.database_uuid = ParseUuid(platform::UuidKind::database,
                                    request.context.database_uuid.canonical);
  storage.filespace_uuid = ParseUuid(platform::UuidKind::filespace,
                                     request.filespace_uuid.canonical);
  storage.owner_object_uuid = ParseUuid(platform::UuidKind::object,
                                        request.owner_object_uuid.canonical);
  storage.row_uuid = ParseUuid(platform::UuidKind::row,
                               request.row.requested_row_uuid.canonical);
  storage.transaction_uuid = ParseUuid(platform::UuidKind::transaction,
                                       request.context.transaction_uuid.canonical);
  storage.chunk_policy_uuid = storage.owner_object_uuid;
  storage.local_transaction_id = request.context.local_transaction_id;
  storage.family = page::LargePayloadFamily::blob;
  storage.cold_threshold_bytes = request.cold_threshold_bytes;
  storage.fields = BuildFieldInputs(request);
  storage.engine_storage_admission_authorized =
      request.engine_storage_admission_authorized;
  storage.mga_write_admitted_by_transaction_inventory =
      request.context.local_transaction_id != 0;
  storage.reason =
      "dml_hot_cold_row_split;diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory";
  return storage;
}

}  // namespace

EngineDmlHotColdSplitResult EngineDmlSplitHotColdRow(
    const EngineDmlHotColdSplitRequest& request) {
  constexpr const char* kOperation = "dml.hot_cold_row_split";
  EngineDmlHotColdSplitResult result;
  auto split = page::SplitHotColdRow(BuildStorageRequest(request));
  if (!split.ok()) {
    result.diagnostic = StorageDiagnostic(kOperation, split.diagnostic);
    AddStorageEvidence(split.evidence, &result.evidence);
    result.evidence.push_back({"dml_hot_cold_split_fail_closed", "true"});
    return result;
  }

  result.ok = true;
  result.hot_head = std::move(split.hot_head);
  result.serialized_hot_head = page::SerializeHotColdRowHead(result.hot_head);
  for (const auto& hot : result.hot_head.hot_fields) {
    result.storage_values.push_back({hot.field_name, hot.encoded_value});
  }
  for (const auto& cold : result.hot_head.cold_fields) {
    result.storage_values.push_back({cold.field_name, cold.descriptor_text});
  }
  AddStorageEvidence(split.evidence, &result.evidence);
  result.evidence.push_back({"dml_hot_cold_split_routed", "true"});
  result.evidence.push_back({"dml_hot_cold_split_serialized_head", "true"});
  result.evidence.push_back({"descriptor_finality_authority", "false"});
  result.evidence.push_back({"descriptor_visibility_authority", "false"});
  return result;
}

EngineDmlHotColdMaterializeResult EngineDmlMaterializeColdFields(
    const EngineDmlHotColdMaterializeRequest& request) {
  constexpr const char* kOperation = "dml.hot_cold_materialize";
  EngineDmlHotColdMaterializeResult result;
  page::HotColdRowMaterializeRequest storage;
  storage.large_payload_store = request.large_payload_store;
  storage.hot_head = request.hot_head;
  storage.cold_field_names = request.cold_field_names;
  storage.observer_snapshot_visible_through_local_transaction_id =
      request.context.snapshot_visible_through_local_transaction_id;
  storage.transaction_context_present =
      request.context.local_transaction_id != 0 &&
      !request.context.transaction_uuid.canonical.empty();
  storage.engine_storage_admission_authorized =
      request.engine_storage_admission_authorized;
  storage.use_cache = request.use_cache;
  storage.prefetch_on_miss = request.prefetch_on_miss;
  storage.reason =
      "dml_hot_cold_materialize;cache_evidence_not_visibility_authority";
  auto materialized = page::MaterializeColdFields(storage);
  if (!materialized.ok()) {
    result.diagnostic = StorageDiagnostic(kOperation, materialized.diagnostic);
    AddStorageEvidence(materialized.evidence, &result.evidence);
    result.evidence.push_back({"dml_hot_cold_materialize_fail_closed", "true"});
    return result;
  }

  result.ok = true;
  for (const auto& field : materialized.cold_fields) {
    result.cold_values.push_back({field.field_name, field.encoded_value});
  }
  AddStorageEvidence(materialized.evidence, &result.evidence);
  result.evidence.push_back({"dml_hot_cold_materialize_routed", "true"});
  result.evidence.push_back({"descriptor_finality_authority", "false"});
  result.evidence.push_back({"descriptor_visibility_authority", "false"});
  return result;
}

EngineDmlHotColdUpdateResult EngineDmlUpdateHotColdRow(
    const EngineDmlHotColdUpdateRequest& request) {
  constexpr const char* kOperation = "dml.hot_cold_update";
  EngineDmlHotColdUpdateResult result;
  page::HotColdRowUpdateRequest storage;
  storage.previous_hot_head = request.previous_hot_head;
  storage.replacement = BuildStorageRequest(request.replacement);
  auto updated = page::UpdateHotColdRow(storage);
  if (!updated.ok()) {
    result.diagnostic = StorageDiagnostic(kOperation, updated.diagnostic);
    AddStorageEvidence(updated.evidence, &result.evidence);
    result.evidence.push_back({"dml_hot_cold_update_fail_closed", "true"});
    return result;
  }

  result.ok = true;
  result.hot_head = std::move(updated.hot_head);
  result.retired_descriptors = std::move(updated.retired_descriptors);
  AddStorageEvidence(updated.evidence, &result.evidence);
  result.evidence.push_back({"dml_hot_cold_update_routed", "true"});
  result.evidence.push_back({"descriptor_finality_authority", "false"});
  result.evidence.push_back({"descriptor_visibility_authority", "false"});
  return result;
}

}  // namespace scratchbird::engine::internal_api
