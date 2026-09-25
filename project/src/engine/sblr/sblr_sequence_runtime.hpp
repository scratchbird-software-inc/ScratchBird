// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrSequenceDefinition {
  SblrUuid sequence_uuid;
  std::string descriptor_id = "int64";
  std::int64_t start_value = 1;
  std::int64_t increment = 1;
  std::int64_t minimum_value = std::numeric_limits<std::int64_t>::min();
  std::int64_t maximum_value = std::numeric_limits<std::int64_t>::max();
  std::uint64_t cache_size = 1;
  bool cycle = false;
};

struct SblrSequenceState {
  SblrSequenceDefinition definition;
  std::int64_t current_value = 0;
  bool current_value_present = false;
  std::int64_t next_value_override = 0;
  bool next_value_override_present = false;
};

struct SblrSequenceEvidenceRecord {
  std::uint64_t evidence_sequence = 0;
  SblrUuid sequence_uuid;
  std::string action;
  std::string value;
  SblrUuid transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::string policy;
};

struct SblrSequenceRegistry {
  std::mutex mutex;
  std::vector<SblrSequenceState> states;
  std::vector<std::pair<std::string, SblrUuid>> aliases;
  std::vector<SblrSequenceEvidenceRecord> evidence;
  std::uint64_t next_evidence_sequence = 1;
};

struct SblrSequenceRequest {
  SblrExecutionContext context;
  SblrUuid sequence_uuid;
  std::string sequence_name_hint;
  std::string result_descriptor_id = "int64";
  std::int64_t increment_override = 0;
  bool has_increment_override = false;
  std::int64_t set_value = 0;
  bool is_called = true;
};

struct SblrSequenceAlteration {
  SblrUuid sequence_uuid;
  std::string sequence_name_hint;
  std::optional<std::int64_t> minimum_value;
  std::optional<std::int64_t> maximum_value;
  std::optional<std::uint64_t> cache_size;
  std::optional<bool> cycle;
};

struct SblrSequenceOptimizerMetadata {
  bool deterministic = false;
  bool volatile_value = true;
  bool transaction_rollback_reverses_value = false;
  bool pushdown_allowed = false;
  bool remote_execution_allowed = false;
  bool llvm_fold_allowed = false;
  std::string cost_class = "sequence_stateful";
  std::string descriptor_rule = "sequence descriptor";
};

// Explicit function-argument boundary. UUID values stay binary; text only
// selects a previously registered name in a separate alias namespace. This
// does not parse UUID text, register names, or confer transaction authority.
bool BindSblrSequenceArgumentIdentity(const SblrValue& argument,
                                      SblrSequenceRequest* request);

SblrSequenceRegistry& ProcessSblrSequenceRegistry();
SblrResult RegisterSblrSequence(SblrSequenceRegistry* registry,
                                const SblrSequenceDefinition& definition,
                                const SblrExecutionContext& context);
SblrResult RegisterSblrSequenceAlias(SblrSequenceRegistry* registry,
                                     SblrUuid canonical_sequence_uuid,
                                     std::string alias_key,
                                     const SblrExecutionContext& context);
SblrResult AlterSblrSequence(SblrSequenceRegistry* registry,
                             const SblrSequenceAlteration& alteration,
                             const SblrExecutionContext& context);
SblrResult NextSblrSequenceValue(SblrSequenceRegistry* registry, const SblrSequenceRequest& request);
SblrResult CurrentSblrSequenceValue(SblrSequenceRegistry* registry, const SblrSequenceRequest& request);
SblrResult SetSblrSequenceValue(SblrSequenceRegistry* registry, const SblrSequenceRequest& request);
SblrResult IdentityCurrentValue(const SblrExecutionContext& context);
SblrSequenceOptimizerMetadata SequenceOptimizerMetadata();
SblrResult RefuseSblrSequenceHook(const SblrExecutionContext& context, SblrUuid sequence_uuid);

}  // namespace scratchbird::engine::sblr
