// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "current_row_map.hpp"

#include <utility>

namespace scratchbird::transaction::mga {
namespace {

bool EngineProvenance(CurrentRowMapProvenance provenance) {
  return provenance == CurrentRowMapProvenance::engine_mga_transaction_inventory ||
         provenance == CurrentRowMapProvenance::engine_authoritative_base_rows;
}

void AddEvidence(CurrentRowMapDecision* decision,
                 std::string name,
                 std::string value) {
  decision->evidence.push_back({std::move(name), std::move(value)});
}

void AddEvidence(CurrentRowMapRebuildResult* result,
                 std::string name,
                 std::string value) {
  result->evidence.push_back({std::move(name), std::move(value)});
}

void MergeCounters(CurrentRowMapCounters* target,
                   const CurrentRowMapCounters& source) {
  target->probes += source.probes;
  target->accepted += source.accepted;
  target->refused += source.refused;
  target->stale_refusals += source.stale_refusals;
  target->epoch_refusals += source.epoch_refusals;
  target->horizon_refusals += source.horizon_refusals;
  target->authority_refusals += source.authority_refusals;
  target->rebuilds += source.rebuilds;
}

CurrentRowMapDecision Refuse(CurrentRowMapDecision decision,
                             std::string reason) {
  decision.accepted = false;
  decision.current_visible = false;
  decision.normal_mga_recheck_required = true;
  decision.security_recheck_required = true;
  decision.evidence_name = "mga_current_row_map.refused";
  decision.refusal_reason = std::move(reason);
  ++decision.counters.refused;
  return decision;
}

CurrentRowMapDecision StartDecision(const CurrentRowMapEntry& entry,
                                    const CurrentRowMapObservedFacts& observed) {
  CurrentRowMapDecision decision;
  decision.counters.probes = 1;
  AddEvidence(&decision, "accelerator", "current_row_map");
  AddEvidence(&decision, "entry_status", CurrentRowMapStatusName(entry.status));
  AddEvidence(&decision, "provenance", CurrentRowMapProvenanceName(entry.provenance));
  AddEvidence(&decision, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(&decision, "map_visibility_authority", "false");
  AddEvidence(&decision, "map_transaction_finality_authority", "false");
  AddEvidence(&decision,
              "observed_map_generation",
              std::to_string(observed.map_generation));
  AddEvidence(&decision,
              "entry_map_generation",
              std::to_string(entry.map_generation));
  return decision;
}

CurrentRowMapRebuildResult RebuildRefused(CurrentRowMapRebuildResult result,
                                          std::string reason) {
  result.ok = false;
  result.refusal_reason = std::move(reason);
  ++result.counters.refused;
  AddEvidence(&result, "diagnostic", result.diagnostic_code);
  AddEvidence(&result, "refusal", result.refusal_reason);
  AddEvidence(&result, "map_self_authority", "false_required");
  AddEvidence(&result, "authority_source", "durable_mga_transaction_inventory");
  return result;
}

}  // namespace

const char* CurrentRowMapStatusName(CurrentRowMapStatus status) {
  switch (status) {
    case CurrentRowMapStatus::current: return "current";
    case CurrentRowMapStatus::missing: return "missing";
    case CurrentRowMapStatus::stale: return "stale";
    case CurrentRowMapStatus::uncertain: return "uncertain";
    case CurrentRowMapStatus::incompatible: return "incompatible";
    case CurrentRowMapStatus::corrupt: return "corrupt";
  }
  return "unknown";
}

const char* CurrentRowMapProvenanceName(CurrentRowMapProvenance provenance) {
  switch (provenance) {
    case CurrentRowMapProvenance::engine_mga_transaction_inventory:
      return "engine_mga_transaction_inventory";
    case CurrentRowMapProvenance::engine_authoritative_base_rows:
      return "engine_authoritative_base_rows";
    case CurrentRowMapProvenance::parser_claim: return "parser_claim";
    case CurrentRowMapProvenance::client_claim: return "client_claim";
    case CurrentRowMapProvenance::reference_claim: return "reference_claim";
    case CurrentRowMapProvenance::index_or_cache_claim:
      return "index_or_cache_claim";
    case CurrentRowMapProvenance::unknown: return "unknown";
  }
  return "unknown";
}

CurrentRowMap MakeCurrentRowMap(u64 map_generation,
                                u64 invalidation_generation) {
  CurrentRowMap map;
  map.map_generation = map_generation;
  map.invalidation_generation = invalidation_generation;
  return map;
}

CurrentRowMapDecision EvaluateCurrentRowMapEntry(
    CurrentRowMap* map,
    const CurrentRowMapEntry& entry,
    const CurrentRowMapObservedFacts& observed) {
  auto decision = StartDecision(entry, observed);
  if (observed.parser_client_or_reference_authority) {
    ++decision.counters.authority_refusals;
    return Refuse(std::move(decision),
                  "parser_client_or_reference_authority_forbidden");
  }
  if (!observed.durable_mga_inventory_proof ||
      !observed.transaction_horizon_authoritative ||
      !observed.normal_mga_visibility_authority_available) {
    ++decision.counters.authority_refusals;
    return Refuse(std::move(decision),
                  "durable_mga_inventory_authority_required");
  }
  if (!observed.security_recheck_planned) {
    ++decision.counters.authority_refusals;
    return Refuse(std::move(decision), "security_recheck_required");
  }
  if (entry.status == CurrentRowMapStatus::stale ||
      entry.status == CurrentRowMapStatus::uncertain ||
      entry.status == CurrentRowMapStatus::incompatible) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "current_row_map_not_current");
  }
  if (entry.status == CurrentRowMapStatus::corrupt || !entry.checksum_valid) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "current_row_map_corrupt");
  }
  if (entry.status == CurrentRowMapStatus::missing ||
      !entry.persisted_record_present) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "current_row_map_missing");
  }
  if (!EngineProvenance(entry.provenance)) {
    ++decision.counters.authority_refusals;
    return Refuse(std::move(decision),
                  "current_row_map_external_provenance_refused");
  }
  if (entry.relation_uuid.empty() || observed.relation_uuid.empty() ||
      entry.row_uuid.empty() || observed.row_uuid.empty() ||
      entry.current_version_uuid.empty() ||
      entry.relation_uuid != observed.relation_uuid ||
      entry.row_uuid != observed.row_uuid) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "current_row_identity_mismatch");
  }
  if (entry.row_generation == 0 ||
      entry.relation_epoch == 0 ||
      entry.catalog_epoch == 0 ||
      entry.security_epoch == 0 ||
      entry.redaction_epoch == 0 ||
      entry.map_generation == 0 ||
      entry.invalidation_generation == 0 ||
      observed.relation_epoch == 0 ||
      observed.catalog_epoch == 0 ||
      observed.security_epoch == 0 ||
      observed.redaction_epoch == 0 ||
      observed.map_generation == 0 ||
      observed.invalidation_generation == 0) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "current_row_generation_or_epoch_missing");
  }
  if (entry.relation_epoch != observed.relation_epoch ||
      entry.catalog_epoch != observed.catalog_epoch ||
      entry.security_epoch != observed.security_epoch ||
      entry.redaction_epoch != observed.redaction_epoch ||
      entry.map_generation != observed.map_generation ||
      entry.invalidation_generation != observed.invalidation_generation) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "current_row_generation_or_epoch_mismatch");
  }
  if (!entry.visible_through_local_transaction_id.valid() ||
      !observed.reader_visible_through_local_transaction_id.valid() ||
      !observed.oldest_active_local_transaction_id.valid() ||
      observed.reader_visible_through_local_transaction_id.value <
          entry.visible_through_local_transaction_id.value ||
      observed.oldest_active_local_transaction_id.value <=
          entry.visible_through_local_transaction_id.value) {
    ++decision.counters.horizon_refusals;
    return Refuse(std::move(decision), "current_row_horizon_not_closed");
  }
  if (!entry.row_is_current_visible) {
    return Refuse(std::move(decision), "current_row_visible_evidence_missing");
  }

  decision.accepted = true;
  decision.current_visible = true;
  decision.normal_mga_recheck_required = true;
  decision.security_recheck_required = true;
  decision.evidence_name = "mga_current_row_map.current_visible.accepted";
  decision.refusal_reason = "none";
  ++decision.counters.accepted;
  AddEvidence(&decision, "row_uuid", entry.row_uuid);
  AddEvidence(&decision, "current_version_uuid", entry.current_version_uuid);
  AddEvidence(&decision, "visible_through_local_transaction_id",
              std::to_string(entry.visible_through_local_transaction_id.value));
  AddEvidence(&decision, "normal_mga_recheck", "required");
  AddEvidence(&decision, "security_recheck", "required");
  if (map != nullptr) {
    MergeCounters(&map->counters, decision.counters);
  }
  return decision;
}

CurrentRowMapDecision LookupCurrentRowMap(
    CurrentRowMap* map,
    const CurrentRowMapObservedFacts& observed) {
  CurrentRowMapEntry missing;
  missing.status = CurrentRowMapStatus::missing;
  missing.relation_uuid = observed.relation_uuid;
  missing.row_uuid = observed.row_uuid;
  if (map == nullptr || map->entries.empty()) {
    auto decision = EvaluateCurrentRowMapEntry(map, missing, observed);
    if (map != nullptr) {
      MergeCounters(&map->counters, decision.counters);
    }
    return decision;
  }
  if (map->map_generation != observed.map_generation ||
      map->invalidation_generation != observed.invalidation_generation) {
    missing.status = CurrentRowMapStatus::stale;
    missing.map_generation = map->map_generation;
    missing.invalidation_generation = map->invalidation_generation;
    auto decision = EvaluateCurrentRowMapEntry(map, missing, observed);
    if (map != nullptr) {
      MergeCounters(&map->counters, decision.counters);
    }
    return decision;
  }
  CurrentRowMapDecision last_refusal;
  bool saw_refusal = false;
  for (const auto& entry : map->entries) {
    if (entry.relation_uuid != observed.relation_uuid ||
        entry.row_uuid != observed.row_uuid) {
      continue;
    }
    auto decision = EvaluateCurrentRowMapEntry(map, entry, observed);
    if (decision.accepted) {
      return decision;
    }
    MergeCounters(&map->counters, decision.counters);
    last_refusal = decision;
    saw_refusal = true;
  }
  if (saw_refusal) {
    return last_refusal;
  }
  auto decision = EvaluateCurrentRowMapEntry(map, missing, observed);
  MergeCounters(&map->counters, decision.counters);
  return decision;
}

CurrentRowMapRebuildResult RebuildCurrentRowMapFromAuthoritativeBaseRows(
    const CurrentRowMapRebuildRequest& request) {
  CurrentRowMapRebuildResult result;
  result.map = MakeCurrentRowMap(request.map_generation,
                                 request.invalidation_generation);
  AddEvidence(&result, "operation", "current_row_map_rebuild");
  AddEvidence(&result, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(&result, "repair_truth_source", "authoritative_base_rows");
  if (request.map_self_authoritative) {
    ++result.counters.authority_refusals;
    return RebuildRefused(std::move(result),
                          "current_row_map_self_repair_authority_refused");
  }
  if (!request.authoritative_base_rows_proof ||
      !request.durable_mga_inventory_proof) {
    ++result.counters.authority_refusals;
    return RebuildRefused(std::move(result),
                          "authoritative_base_rows_and_mga_inventory_required");
  }
  if (request.relation_uuid.empty() ||
      request.relation_epoch == 0 ||
      request.catalog_epoch == 0 ||
      request.security_epoch == 0 ||
      request.redaction_epoch == 0 ||
      request.map_generation == 0 ||
      request.invalidation_generation == 0) {
    ++result.counters.epoch_refusals;
    return RebuildRefused(std::move(result),
                          "current_row_rebuild_identity_or_epoch_missing");
  }

  for (const auto& row : request.base_rows) {
    if (row.deleted || !row.visible || row.row_uuid.empty() ||
        row.version_uuid.empty() || row.row_generation == 0 ||
        !row.visible_through_local_transaction_id.valid()) {
      continue;
    }
    CurrentRowMapEntry entry;
    entry.status = CurrentRowMapStatus::current;
    entry.provenance = CurrentRowMapProvenance::engine_authoritative_base_rows;
    entry.relation_uuid = request.relation_uuid;
    entry.row_uuid = row.row_uuid;
    entry.current_version_uuid = row.version_uuid;
    entry.row_generation = row.row_generation;
    entry.relation_epoch = request.relation_epoch;
    entry.catalog_epoch = request.catalog_epoch;
    entry.security_epoch = request.security_epoch;
    entry.redaction_epoch = request.redaction_epoch;
    entry.map_generation = request.map_generation;
    entry.invalidation_generation = request.invalidation_generation;
    entry.visible_through_local_transaction_id =
        row.visible_through_local_transaction_id;
    entry.persisted_record_present = true;
    entry.checksum_valid = true;
    entry.row_is_current_visible = true;
    result.map.entries.push_back(std::move(entry));
  }

  result.ok = true;
  result.diagnostic_code = "SB_ENGINE_API_OK";
  result.refusal_reason = "none";
  result.rebuilt_entry_count = result.map.entries.size();
  result.counters.rebuilds = 1;
  result.counters.accepted = 1;
  AddEvidence(&result, "rebuilt_entry_count",
              std::to_string(result.rebuilt_entry_count));
  AddEvidence(&result, "map_self_authority", "false");
  AddEvidence(&result,
              "durable_mga_inventory_remains_authority",
              "true");
  return result;
}

}  // namespace scratchbird::transaction::mga
