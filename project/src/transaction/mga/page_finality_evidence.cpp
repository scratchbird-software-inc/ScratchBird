// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_finality_evidence.hpp"

#include <utility>

namespace scratchbird::transaction::mga {
namespace {

bool RequiresAllVisible(PageFinalityConsumer consumer) {
  return consumer == PageFinalityConsumer::index_only_scan ||
         consumer == PageFinalityConsumer::dml_recheck;
}

bool RequiresAllFinal(PageFinalityConsumer consumer) {
  return consumer == PageFinalityConsumer::cleanup ||
         consumer == PageFinalityConsumer::summary_pruning;
}

bool IsEngineMgaProvenance(PageFinalityProvenance provenance) {
  return provenance == PageFinalityProvenance::engine_mga_transaction_inventory ||
         provenance == PageFinalityProvenance::engine_mga_cleanup_horizon;
}

void AddEvidence(PageFinalityEvidenceDecision* decision,
                 std::string name,
                 std::string value) {
  decision->evidence.push_back({std::move(name), std::move(value)});
}

void AddEvidence(ExactIndexCleanupAuthorityDecision* decision,
                 std::string name,
                 std::string value) {
  decision->evidence.push_back({std::move(name), std::move(value)});
}

PageFinalityEvidenceDecision Refuse(PageFinalityEvidenceDecision decision,
                                    std::string reason) {
  decision.accepted = false;
  decision.all_visible = false;
  decision.all_final = false;
  decision.normal_mga_recheck_required = true;
  decision.evidence_name = "mga_page_finality.refused";
  decision.refusal_reason = std::move(reason);
  ++decision.counters.refused;
  return decision;
}

}  // namespace

const char* PageFinalityScopeName(PageFinalityScope scope) {
  switch (scope) {
    case PageFinalityScope::page: return "page";
    case PageFinalityScope::extent: return "extent";
    case PageFinalityScope::unknown: return "unknown";
  }
  return "unknown";
}

const char* PageFinalityConsumerName(PageFinalityConsumer consumer) {
  switch (consumer) {
    case PageFinalityConsumer::index_only_scan: return "index_only_scan";
    case PageFinalityConsumer::dml_recheck: return "dml_recheck";
    case PageFinalityConsumer::cleanup: return "cleanup";
    case PageFinalityConsumer::summary_pruning: return "summary_pruning";
    case PageFinalityConsumer::unknown: return "unknown";
  }
  return "unknown";
}

const char* PageFinalityMapStatusName(PageFinalityMapStatus status) {
  switch (status) {
    case PageFinalityMapStatus::current: return "current";
    case PageFinalityMapStatus::missing: return "missing";
    case PageFinalityMapStatus::stale: return "stale";
    case PageFinalityMapStatus::uncertain: return "uncertain";
    case PageFinalityMapStatus::incompatible: return "incompatible";
    case PageFinalityMapStatus::corrupt: return "corrupt";
  }
  return "unknown";
}

const char* PageFinalityProvenanceName(PageFinalityProvenance provenance) {
  switch (provenance) {
    case PageFinalityProvenance::engine_mga_transaction_inventory:
      return "engine_mga_transaction_inventory";
    case PageFinalityProvenance::engine_mga_cleanup_horizon:
      return "engine_mga_cleanup_horizon";
    case PageFinalityProvenance::parser_claim: return "parser_claim";
    case PageFinalityProvenance::reference_claim: return "reference_claim";
    case PageFinalityProvenance::timestamp_claim: return "timestamp_claim";
    case PageFinalityProvenance::uuid_order_claim: return "uuid_order_claim";
    case PageFinalityProvenance::external_log_claim: return "external_log_claim";
    case PageFinalityProvenance::unknown: return "unknown";
  }
  return "unknown";
}

PageFinalityEvidenceDecision EvaluatePageFinalityEvidence(
    const PageFinalityMapEntry& entry,
    const PageFinalityObservedFacts& observed,
    PageFinalityConsumer consumer) {
  PageFinalityEvidenceDecision decision;
  decision.counters.evidence_examined = 1;
  AddEvidence(&decision, "consumer", PageFinalityConsumerName(consumer));
  AddEvidence(&decision, "requested_scope", PageFinalityScopeName(observed.requested_scope));
  AddEvidence(&decision, "entry_scope", PageFinalityScopeName(entry.scope));
  AddEvidence(&decision, "entry_status", PageFinalityMapStatusName(entry.status));
  AddEvidence(&decision, "provenance", PageFinalityProvenanceName(entry.provenance));
  AddEvidence(&decision, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(&decision, "map_transaction_finality_authority", "false");

  if (consumer == PageFinalityConsumer::unknown ||
      observed.requested_scope == PageFinalityScope::unknown ||
      entry.scope == PageFinalityScope::unknown ||
      observed.requested_scope != entry.scope) {
    return Refuse(std::move(decision), "scope_or_consumer_incompatible");
  }
  if (!entry.persisted_record_present || entry.status == PageFinalityMapStatus::missing) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "finality_map_missing");
  }
  if (!entry.checksum_valid || entry.status == PageFinalityMapStatus::corrupt) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "finality_map_corrupt");
  }
  if (entry.status == PageFinalityMapStatus::stale ||
      entry.status == PageFinalityMapStatus::uncertain ||
      entry.status == PageFinalityMapStatus::incompatible) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "finality_map_not_current");
  }
  if (!IsEngineMgaProvenance(entry.provenance)) {
    ++decision.counters.provenance_refusals;
    return Refuse(std::move(decision), "finality_map_external_provenance_refused");
  }
  if (observed.relation_uuid.empty() || entry.relation_uuid.empty() ||
      observed.relation_uuid != entry.relation_uuid) {
    return Refuse(std::move(decision), "relation_mismatch_or_missing");
  }
  if (entry.page_generation == 0 || observed.page_generation == 0 ||
      entry.extent_epoch == 0 || observed.extent_epoch == 0 ||
      entry.relation_epoch == 0 || observed.relation_epoch == 0 ||
      entry.catalog_epoch == 0 || observed.catalog_epoch == 0 ||
      entry.map_generation == 0) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "generation_or_epoch_missing");
  }
  if (entry.scope == PageFinalityScope::page &&
      (entry.page_number != observed.page_number ||
       entry.page_generation != observed.page_generation)) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "page_generation_mismatch");
  }
  if (entry.extent_id != observed.extent_id ||
      entry.extent_epoch != observed.extent_epoch) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "extent_epoch_mismatch");
  }
  if (entry.relation_epoch != observed.relation_epoch ||
      entry.catalog_epoch != observed.catalog_epoch) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "relation_or_catalog_epoch_mismatch");
  }
  if (!entry.final_through_local_transaction_id.valid() ||
      !observed.reader_visible_through_local_transaction_id.valid() ||
      !observed.oldest_active_local_transaction_id.valid() ||
      !observed.transaction_horizon_authoritative ||
      !observed.transaction_inventory_authoritative) {
    ++decision.counters.horizon_refusals;
    return Refuse(std::move(decision), "transaction_horizon_or_inventory_uncertain");
  }
  if (observed.oldest_active_local_transaction_id.value <=
      entry.final_through_local_transaction_id.value) {
    ++decision.counters.horizon_refusals;
    return Refuse(std::move(decision), "active_transaction_overlaps_finality_map");
  }
  if (observed.reader_visible_through_local_transaction_id.value <
      entry.final_through_local_transaction_id.value) {
    ++decision.counters.horizon_refusals;
    return Refuse(std::move(decision), "reader_horizon_behind_finality_map");
  }
  if (!observed.normal_mga_visibility_authority_available) {
    return Refuse(std::move(decision), "normal_mga_visibility_authority_missing");
  }
  if (RequiresAllVisible(consumer) && !entry.all_visible) {
    return Refuse(std::move(decision), "page_all_visible_evidence_missing");
  }
  if (RequiresAllFinal(consumer) && !entry.all_final) {
    return Refuse(std::move(decision), "extent_all_final_evidence_missing");
  }

  decision.accepted = true;
  decision.all_visible = entry.all_visible;
  decision.all_final = entry.all_final;
  decision.normal_mga_recheck_required = false;
  decision.evidence_name = entry.scope == PageFinalityScope::page
                               ? "mga_page_finality.page_all_visible.accepted"
                               : "mga_page_finality.extent_all_final.accepted";
  decision.refusal_reason = "none";
  ++decision.counters.accepted;
  AddEvidence(&decision, "final_through_local_transaction_id",
              std::to_string(entry.final_through_local_transaction_id.value));
  AddEvidence(&decision, "map_generation", std::to_string(entry.map_generation));
  return decision;
}

ExactIndexCleanupAuthorityDecision EvaluateExactIndexCleanupAuthority(
    const PageFinalityEvidenceDecision& page_finality,
    LocalTransactionId cleanup_horizon,
    bool cleanup_horizon_authoritative,
    bool transaction_inventory_authoritative) {
  ExactIndexCleanupAuthorityDecision decision;
  decision.cleanup_horizon_authoritative = cleanup_horizon_authoritative;
  decision.transaction_inventory_authoritative =
      transaction_inventory_authoritative;
  decision.cleanup_horizon_local_transaction_id =
      cleanup_horizon.valid() ? cleanup_horizon.value : 0;
  decision.evidence = page_finality.evidence;
  decision.counters = page_finality.counters;
  AddEvidence(&decision, "authority_consumer", "exact_index_cleanup");
  AddEvidence(&decision,
              "cleanup_horizon_authoritative",
              cleanup_horizon_authoritative ? "true" : "false");
  AddEvidence(&decision,
              "transaction_inventory_authoritative",
              transaction_inventory_authoritative ? "true" : "false");
  AddEvidence(&decision,
              "cleanup_horizon_local_transaction_id",
              std::to_string(decision.cleanup_horizon_local_transaction_id));

  if (!cleanup_horizon.valid() || !cleanup_horizon_authoritative ||
      !transaction_inventory_authoritative) {
    decision.refusal_reason =
        "cleanup_horizon_or_transaction_inventory_uncertain";
    ++decision.counters.horizon_refusals;
    return decision;
  }
  if (!page_finality.accepted) {
    decision.refusal_reason = page_finality.refusal_reason.empty()
                                  ? "page_finality_refused"
                                  : page_finality.refusal_reason;
    ++decision.counters.refused;
    return decision;
  }
  if (!page_finality.all_final) {
    decision.refusal_reason = "page_finality_all_final_required";
    ++decision.counters.horizon_refusals;
    return decision;
  }
  if (page_finality.map_is_transaction_finality_authority ||
      !page_finality.durable_mga_inventory_remains_authority) {
    decision.refusal_reason = "durable_mga_inventory_authority_required";
    ++decision.counters.provenance_refusals;
    return decision;
  }

  decision.accepted = true;
  decision.page_finality_authoritative = true;
  decision.authority_source = "durable_mga_transaction_inventory";
  decision.refusal_reason = "none";
  AddEvidence(&decision, "authority_source", decision.authority_source);
  AddEvidence(&decision, "page_finality_all_final", "true");
  return decision;
}

}  // namespace scratchbird::transaction::mga
