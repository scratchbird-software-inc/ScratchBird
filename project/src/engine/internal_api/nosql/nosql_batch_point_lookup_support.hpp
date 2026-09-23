// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-NOSQL-BATCH-POINT-LOOKUP-SUPPORT-ANCHOR
#include "api_diagnostics.hpp"
#include "api_types.hpp"
#include "batch_point_lookup.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"
#include "uuid.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

struct EngineNoSqlBatchPointLookupItem {
  std::variant<std::string, EngineUuid> encoded_key;
  EngineUuid row_uuid;
  double score = 0.0;
  std::string payload;
  std::vector<std::pair<std::string, std::string>> attributes;
};

inline scratchbird::core::index::CandidateSetAuthorityContext
EngineNoSqlBatchLookupAuthorityFromSelection(
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

inline scratchbird::core::platform::TypedUuid EngineNoSqlLookupRowUuid(
    const EngineUuid& candidate) {
  const auto admitted = core::uuid::MakeDurableEngineIdentityUuid(
      core::platform::UuidKind::row, candidate);
  return admitted.ok() ? admitted.value : core::platform::TypedUuid{};
}

inline std::string EncodeNoSqlBatchLookupKey(
    const std::variant<std::string, EngineUuid>& key) {
  if (const auto* uuid = std::get_if<EngineUuid>(&key)) {
    std::string encoded(1, '\2');
    encoded.append(reinterpret_cast<const char*>(uuid->bytes.data()), uuid->bytes.size());
    return encoded;
  }
  std::string encoded(1, '\1');
  encoded += std::get<std::string>(key);
  return encoded;
}

inline void AddEngineNoSqlBatchLookupEvidence(
    EngineApiResult* result,
    const std::string& family,
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
  AddApiBehaviorEvidence(result,
                         "nosql_ordered_batch_lookup_primitive",
                         family + ":ODF-092");
}

template <typename TResult>
std::optional<TResult> AddEngineNoSqlOrderedBatchLookupEvidence(
    const EngineRequestContext& context,
    const std::string& operation_id,
    const std::string& family,
    scratchbird::core::index::BatchPointLookupPurpose purpose,
    const EngineNoSqlPhysicalProviderSelection& selection,
    const std::vector<EngineNoSqlBatchPointLookupItem>& items,
    EngineApiResult* result) {
  scratchbird::core::index::BatchPointLookupPlan plan;
  plan.purpose = purpose;
  plan.plan_id = operation_id + ":" + family + "_ordered_batch_lookup";
  plan.caller_evidence = selection.evidence;
  plan.keys.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    plan.keys.push_back({EncodeNoSqlBatchLookupKey(items[i].encoded_key),
                         static_cast<EngineApiU64>(i)});
  }

  std::map<std::string, std::vector<EngineNoSqlBatchPointLookupItem>> by_key;
  for (const auto& item : items) {
    by_key[EncodeNoSqlBatchLookupKey(item.encoded_key)].push_back(item);
  }

  auto lookup = scratchbird::core::index::RunBatchPointLookup(
      plan,
      EngineNoSqlBatchLookupAuthorityFromSelection(selection),
      [by_key = std::move(by_key), family](
          const scratchbird::core::index::BatchPointLookupProviderRequest&
              provider_request) {
        scratchbird::core::index::BatchPointLookupProviderResult provider_result;
        provider_result.status = {scratchbird::core::platform::StatusCode::ok,
                                  scratchbird::core::platform::Severity::info,
                                  scratchbird::core::platform::Subsystem::engine};
        provider_result.evidence.push_back(
            "batch_point_lookup.provider=nosql_" + family +
            "_ordered_point_provider");
        provider_result.evidence.push_back(
            "batch_point_lookup.provider.transaction_finality_authority=false");
        provider_result.evidence.push_back(
            "batch_point_lookup.provider.visibility_authority=false");
        for (const auto& key : provider_request.ordered_unique_keys) {
          const auto found = by_key.find(key.encoded_key);
          if (found == by_key.end()) {
            continue;
          }
          for (const auto& item : found->second) {
            scratchbird::core::index::BatchPointLookupProviderRow row;
            row.encoded_key = key.encoded_key;
            row.candidate.row_uuid = EngineNoSqlLookupRowUuid(item.row_uuid);
            row.candidate.score = item.score;
            row.candidate.exact_predicate_match = true;
            row.candidate.mga_visible = true;
            row.candidate.security_authorized = true;
            row.candidate.exact_payload_available = true;
            row.candidate.source = "nosql." + family;
            row.payload = item.payload;
            row.attributes = item.attributes;
            row.exact_row_uuid = row.candidate.row_uuid.valid();
            provider_result.rows.push_back(std::move(row));
          }
        }
        return provider_result;
      });
  if (!lookup.ok()) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id,
                                     lookup.diagnostic.diagnostic_code));
    AddEngineNoSqlBatchLookupEvidence(&failure, family, lookup);
    return failure;
  }

  AddEngineNoSqlBatchLookupEvidence(result, family, lookup);
  return std::nullopt;
}

}  // namespace scratchbird::engine::internal_api
