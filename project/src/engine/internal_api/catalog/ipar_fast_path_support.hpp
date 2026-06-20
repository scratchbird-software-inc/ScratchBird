// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// IPAR-P6-17/IPAR-P6-22/IPAR-P6-23/IPAR-P6-24/IPAR-P6-31
// descriptor fast-path support.
#include "api_types.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct IparFastPathEpochVector {
  EngineApiU64 catalog_epoch = 0;
  EngineApiU64 security_epoch = 0;
  EngineApiU64 policy_epoch = 0;
  EngineApiU64 resource_epoch = 0;
  EngineApiU64 language_epoch = 0;
  EngineApiU64 descriptor_epoch = 0;
  EngineApiU64 capability_epoch = 0;
};

struct IparCompressedEpochVector {
  IparFastPathEpochVector source;
  std::string digest;
  bool complete = false;
};

enum class IparCapability : std::uint8_t {
  triggers,
  defaults,
  generated_columns,
  constraints,
  foreign_keys,
  row_level_security,
  security_masks,
  unique_indexes,
  large_values,
  external_storage,
  audit_hooks,
  replication_hooks,
  optional_hooks
};

struct IparObjectCapabilitySet {
  std::string object_uuid;
  IparCompressedEpochVector epoch;
  std::uint64_t bits = 0;
};

struct IparCapabilityInput {
  std::string object_uuid;
  IparCompressedEpochVector epoch;
  bool has_triggers = false;
  bool has_defaults = false;
  bool has_generated_columns = false;
  bool has_constraints = false;
  bool has_foreign_keys = false;
  bool has_row_level_security = false;
  bool has_security_masks = false;
  bool has_unique_indexes = false;
  bool has_large_values = false;
  bool has_external_storage = false;
  bool has_audit_hooks = false;
  bool has_replication_hooks = false;
  bool has_optional_hooks = false;
};

struct IparNoopBranchDecision {
  std::string branch_name;
  bool required = false;
  bool skip_fast_path = true;
};

struct IparRowLayoutColumn {
  std::string column_uuid;
  EngineDescriptor descriptor;
  std::uint32_t ordinal = 0;
  std::uint32_t fixed_width_bytes = 0;
  std::uint32_t max_variable_bytes = 0;
  bool nullable = true;
  bool variable_width = false;
  bool has_default = false;
  bool has_generated_value = false;
  bool coercion_required = false;
};

struct IparRowLayoutSlot {
  std::string column_uuid;
  std::uint32_t ordinal = 0;
  std::uint32_t fixed_offset = 0;
  std::uint32_t fixed_width_bytes = 0;
  std::uint32_t variable_index = 0;
  bool variable_width = false;
  bool nullable = false;
  bool uses_default_or_generated = false;
  bool coercion_required = false;
};

struct IparRowLayoutDescriptor {
  std::string table_uuid;
  std::string statement_uuid;
  IparCompressedEpochVector epoch;
  std::uint32_t fixed_row_bytes = 0;
  std::uint32_t null_bitmap_bytes = 0;
  std::uint32_t variable_column_count = 0;
  std::string encoder_digest;
  std::vector<IparRowLayoutSlot> slots;
  std::vector<std::string> parameter_bind_map;
  bool security_recheck_required = true;
  bool visibility_recheck_required = true;
  bool parser_authority = false;
};

struct IparRowLayoutBuildResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  IparRowLayoutDescriptor layout;
};

struct IparParameterEncoderLookupResult {
  bool ok = false;
  bool cache_hit = false;
  bool stale = false;
  std::string diagnostic_code;
  std::string detail;
  IparRowLayoutDescriptor layout;
};

class IparParameterEncoderCache {
 public:
  IparParameterEncoderLookupResult Put(IparRowLayoutDescriptor layout);
  IparParameterEncoderLookupResult Lookup(const std::string& table_uuid,
                                          const std::string& statement_uuid,
                                          const IparCompressedEpochVector& epoch,
                                          const std::string& encoder_digest);
  std::uint64_t InvalidateStale(const IparCompressedEpochVector& current_epoch);
  void Clear();

 private:
  mutable std::mutex mutex_;
  std::map<std::string, IparRowLayoutDescriptor> entries_;
};

struct IparWarmProfileItem {
  std::string item_id;
  std::string item_kind;
  std::string object_uuid;
  IparCompressedEpochVector epoch;
  std::uint64_t bytes = 0;
  std::uint64_t priority = 0;
  bool authorization_checked = false;
  bool policy_checked = false;
};

struct IparWarmProfileRequest {
  std::string database_uuid;
  IparCompressedEpochVector open_epoch;
  std::uint64_t budget_bytes = 0;
  std::vector<IparWarmProfileItem> items;
};

struct IparWarmProfilePlan {
  bool ok = false;
  std::string diagnostic_code;
  std::uint64_t selected_bytes = 0;
  std::vector<IparWarmProfileItem> selected;
  std::vector<std::string> skipped;
  std::vector<std::string> evidence;
};

IparCompressedEpochVector CompressIparFastPathEpochVector(
    IparFastPathEpochVector vector);
bool IparCompressedEpochVectorMatches(const IparCompressedEpochVector& left,
                                      const IparCompressedEpochVector& right);
IparObjectCapabilitySet BuildIparObjectCapabilitySet(
    IparCapabilityInput input);
bool IparObjectCapabilityHas(const IparObjectCapabilitySet& capabilities,
                             IparCapability capability);
std::vector<IparNoopBranchDecision> BuildIparNoopBranchTable(
    const IparObjectCapabilitySet& capabilities);
IparRowLayoutBuildResult BuildIparRowLayoutDescriptor(
    std::string table_uuid,
    std::string statement_uuid,
    IparCompressedEpochVector epoch,
    std::vector<IparRowLayoutColumn> columns);
std::string IparRowLayoutCacheKey(const std::string& table_uuid,
                                  const std::string& statement_uuid,
                                  const IparCompressedEpochVector& epoch,
                                  const std::string& encoder_digest);
IparWarmProfilePlan PlanIparDatabaseOpenWarmProfile(
    IparWarmProfileRequest request);

}  // namespace scratchbird::engine::internal_api
