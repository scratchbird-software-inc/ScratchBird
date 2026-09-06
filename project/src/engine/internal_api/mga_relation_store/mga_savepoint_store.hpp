// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

struct SavepointCutoffs {
  std::uint64_t row_event_sequence = 0;
  std::uint64_t metadata_event_sequence = 0;
  std::uint64_t index_event_sequence = 0;
};

struct SavepointRollbackRange {
  SavepointCutoffs cutoffs;
  std::uint64_t row_upper_event_sequence =
      std::numeric_limits<std::uint64_t>::max();
  std::uint64_t metadata_upper_event_sequence =
      std::numeric_limits<std::uint64_t>::max();
  std::uint64_t index_upper_event_sequence =
      std::numeric_limits<std::uint64_t>::max();
};

struct SavepointParsedState {
  std::map<std::uint64_t, std::map<std::string, SavepointCutoffs>>
      active_savepoints;
  std::map<std::uint64_t, std::vector<SavepointRollbackRange>> rollback_ranges;
  std::map<std::uint64_t,
           std::vector<std::pair<std::uint64_t, std::uint64_t>>>
      normalized_row_rollback_ranges;
  bool row_rollback_ranges_normalized = false;
  bool update_statement_authority_corrupt = false;
};

SavepointParsedState ParseSavepoints(const EngineRequestContext& context);
bool ParseSavepointsBounded(const EngineRequestContext& context,
                            BoundedScopedRowReadControl* control,
                            std::uint64_t retained_memory_bytes,
                            SavepointParsedState* state);
bool RowEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                   std::uint64_t creator_tx,
                                   std::uint64_t event_sequence);
bool MetadataEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                        std::uint64_t creator_tx,
                                        std::uint64_t event_sequence);
bool IndexEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                     std::uint64_t creator_tx,
                                     std::uint64_t event_sequence);

}  // namespace scratchbird::engine::internal_api
