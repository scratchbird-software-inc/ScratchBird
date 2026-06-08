// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "transaction_horizon.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class CurrentRowMapStatus : u16 {
  current,
  missing,
  stale,
  uncertain,
  incompatible,
  corrupt
};

enum class CurrentRowMapProvenance : u16 {
  engine_mga_transaction_inventory,
  engine_authoritative_base_rows,
  parser_claim,
  client_claim,
  donor_claim,
  index_or_cache_claim,
  unknown
};

struct CurrentRowMapEntry {
  CurrentRowMapStatus status = CurrentRowMapStatus::missing;
  CurrentRowMapProvenance provenance = CurrentRowMapProvenance::unknown;
  std::string relation_uuid;
  std::string row_uuid;
  std::string current_version_uuid;
  u64 row_generation = 0;
  u64 relation_epoch = 0;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 redaction_epoch = 0;
  u64 map_generation = 0;
  u64 invalidation_generation = 0;
  LocalTransactionId visible_through_local_transaction_id;
  bool persisted_record_present = false;
  bool checksum_valid = false;
  bool row_is_current_visible = false;
};

struct CurrentRowMapObservedFacts {
  std::string relation_uuid;
  std::string row_uuid;
  u64 relation_epoch = 0;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 redaction_epoch = 0;
  u64 map_generation = 0;
  u64 invalidation_generation = 0;
  LocalTransactionId reader_visible_through_local_transaction_id;
  LocalTransactionId oldest_active_local_transaction_id;
  bool durable_mga_inventory_proof = false;
  bool transaction_horizon_authoritative = false;
  bool normal_mga_visibility_authority_available = false;
  bool security_recheck_planned = false;
  bool parser_client_or_donor_authority = false;
};

struct CurrentRowMapEvidenceField {
  std::string name;
  std::string value;
};

struct CurrentRowMapCounters {
  u64 probes = 0;
  u64 accepted = 0;
  u64 refused = 0;
  u64 stale_refusals = 0;
  u64 epoch_refusals = 0;
  u64 horizon_refusals = 0;
  u64 authority_refusals = 0;
  u64 rebuilds = 0;
};

struct CurrentRowMapDecision {
  bool accepted = false;
  bool current_visible = false;
  bool normal_mga_recheck_required = true;
  bool security_recheck_required = true;
  bool map_is_visibility_authority = false;
  bool map_is_transaction_finality_authority = false;
  bool durable_mga_inventory_remains_authority = true;
  std::string evidence_name = "mga_current_row_map.refused";
  std::string refusal_reason = "not_evaluated";
  CurrentRowMapCounters counters;
  std::vector<CurrentRowMapEvidenceField> evidence;
};

struct CurrentRowMap {
  u64 map_generation = 1;
  u64 invalidation_generation = 1;
  std::vector<CurrentRowMapEntry> entries;
  CurrentRowMapCounters counters;
};

struct CurrentRowAuthoritativeBaseRow {
  std::string row_uuid;
  std::string version_uuid;
  u64 row_generation = 0;
  LocalTransactionId visible_through_local_transaction_id;
  bool deleted = false;
  bool visible = false;
};

struct CurrentRowMapRebuildRequest {
  std::string relation_uuid;
  u64 relation_epoch = 0;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 redaction_epoch = 0;
  u64 map_generation = 0;
  u64 invalidation_generation = 0;
  bool authoritative_base_rows_proof = false;
  bool durable_mga_inventory_proof = false;
  bool map_self_authoritative = false;
  std::vector<CurrentRowAuthoritativeBaseRow> base_rows;
};

struct CurrentRowMapRebuildResult {
  bool ok = false;
  CurrentRowMap map;
  u64 rebuilt_entry_count = 0;
  std::string diagnostic_code = "ORH_VISIBILITY_ACCELERATOR_REBUILD_REFUSED";
  std::string refusal_reason = "not_evaluated";
  std::vector<CurrentRowMapEvidenceField> evidence;
  CurrentRowMapCounters counters;
};

const char* CurrentRowMapStatusName(CurrentRowMapStatus status);
const char* CurrentRowMapProvenanceName(CurrentRowMapProvenance provenance);

CurrentRowMap MakeCurrentRowMap(u64 map_generation,
                                u64 invalidation_generation);

CurrentRowMapDecision EvaluateCurrentRowMapEntry(
    CurrentRowMap* map,
    const CurrentRowMapEntry& entry,
    const CurrentRowMapObservedFacts& observed);

CurrentRowMapDecision LookupCurrentRowMap(
    CurrentRowMap* map,
    const CurrentRowMapObservedFacts& observed);

CurrentRowMapRebuildResult RebuildCurrentRowMapFromAuthoritativeBaseRows(
    const CurrentRowMapRebuildRequest& request);

}  // namespace scratchbird::transaction::mga
