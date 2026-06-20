// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/ipar_fast_path_support.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kRowLayoutAnchor =
    "IPAR-P6-17_ROW_LAYOUT_PARAMETER_ENCODER_CACHE";
constexpr const char* kCapabilityAnchor =
    "IPAR-P6-22_PER_OBJECT_FAST_PATH_CAPABILITY_BITSETS";
constexpr const char* kEpochAnchor = "IPAR-P6-23_EPOCH_VECTOR_COMPRESSION";
constexpr const char* kNoopAnchor = "IPAR-P6-24_NOOP_BRANCH_ELIMINATION_TABLES";
constexpr const char* kWarmOpenAnchor = "IPAR-P6-31_DATABASE_OPEN_WARM_PROFILE_AGENT";
constexpr const char* kAuthorityScope =
    "ipar_fast_path_support.descriptor_cache_only_no_transaction_visibility_security_recovery_or_parser_authority";

bool EpochComplete(const IparFastPathEpochVector& vector) {
  return vector.catalog_epoch != 0 &&
         vector.security_epoch != 0 &&
         vector.policy_epoch != 0 &&
         vector.resource_epoch != 0 &&
         vector.language_epoch != 0 &&
         vector.descriptor_epoch != 0 &&
         vector.capability_epoch != 0;
}

std::string UInt64Hex(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string StableDigest(const std::vector<std::string>& parts) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& part : parts) {
    for (const unsigned char ch : part) {
      hash ^= static_cast<std::uint64_t>(ch);
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  return "fnv1a64:" + UInt64Hex(hash);
}

std::uint64_t CapabilityMask(IparCapability capability) {
  return 1ull << static_cast<std::uint8_t>(capability);
}

void SetCapability(std::uint64_t* bits,
                   IparCapability capability,
                   bool enabled) {
  if (enabled) {
    *bits |= CapabilityMask(capability);
  }
}

IparParameterEncoderLookupResult Refusal(std::string code,
                                         std::string detail) {
  IparParameterEncoderLookupResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  return result;
}

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
  if (alignment == 0) {
    return value;
  }
  const std::uint32_t remainder = value % alignment;
  if (remainder == 0) {
    return value;
  }
  return value + (alignment - remainder);
}

std::string SlotDigestText(const IparRowLayoutSlot& slot) {
  return slot.column_uuid + ":" + std::to_string(slot.ordinal) + ":" +
         std::to_string(slot.fixed_offset) + ":" +
         std::to_string(slot.fixed_width_bytes) + ":" +
         std::to_string(slot.variable_index) + ":" +
         (slot.variable_width ? "var" : "fixed") + ":" +
         (slot.nullable ? "nullable" : "required") + ":" +
         (slot.uses_default_or_generated ? "default_or_generated" : "plain") + ":" +
         (slot.coercion_required ? "coerce" : "no_coerce");
}

}  // namespace

IparCompressedEpochVector CompressIparFastPathEpochVector(
    IparFastPathEpochVector vector) {
  IparCompressedEpochVector compressed;
  compressed.source = vector;
  compressed.complete = EpochComplete(vector);
  compressed.digest = StableDigest({
      "catalog=" + std::to_string(vector.catalog_epoch),
      "security=" + std::to_string(vector.security_epoch),
      "policy=" + std::to_string(vector.policy_epoch),
      "resource=" + std::to_string(vector.resource_epoch),
      "language=" + std::to_string(vector.language_epoch),
      "descriptor=" + std::to_string(vector.descriptor_epoch),
      "capability=" + std::to_string(vector.capability_epoch),
      kEpochAnchor,
      kAuthorityScope});
  return compressed;
}

bool IparCompressedEpochVectorMatches(const IparCompressedEpochVector& left,
                                      const IparCompressedEpochVector& right) {
  return left.complete &&
         right.complete &&
         left.digest == right.digest &&
         left.source.catalog_epoch == right.source.catalog_epoch &&
         left.source.security_epoch == right.source.security_epoch &&
         left.source.policy_epoch == right.source.policy_epoch &&
         left.source.resource_epoch == right.source.resource_epoch &&
         left.source.language_epoch == right.source.language_epoch &&
         left.source.descriptor_epoch == right.source.descriptor_epoch &&
         left.source.capability_epoch == right.source.capability_epoch;
}

IparObjectCapabilitySet BuildIparObjectCapabilitySet(
    IparCapabilityInput input) {
  IparObjectCapabilitySet capabilities;
  capabilities.object_uuid = std::move(input.object_uuid);
  capabilities.epoch = std::move(input.epoch);
  SetCapability(&capabilities.bits, IparCapability::triggers, input.has_triggers);
  SetCapability(&capabilities.bits, IparCapability::defaults, input.has_defaults);
  SetCapability(&capabilities.bits,
                IparCapability::generated_columns,
                input.has_generated_columns);
  SetCapability(&capabilities.bits, IparCapability::constraints, input.has_constraints);
  SetCapability(&capabilities.bits, IparCapability::foreign_keys, input.has_foreign_keys);
  SetCapability(&capabilities.bits,
                IparCapability::row_level_security,
                input.has_row_level_security);
  SetCapability(&capabilities.bits, IparCapability::security_masks, input.has_security_masks);
  SetCapability(&capabilities.bits, IparCapability::unique_indexes, input.has_unique_indexes);
  SetCapability(&capabilities.bits, IparCapability::large_values, input.has_large_values);
  SetCapability(&capabilities.bits, IparCapability::external_storage, input.has_external_storage);
  SetCapability(&capabilities.bits, IparCapability::audit_hooks, input.has_audit_hooks);
  SetCapability(&capabilities.bits, IparCapability::replication_hooks, input.has_replication_hooks);
  SetCapability(&capabilities.bits, IparCapability::optional_hooks, input.has_optional_hooks);
  return capabilities;
}

bool IparObjectCapabilityHas(const IparObjectCapabilitySet& capabilities,
                             IparCapability capability) {
  return (capabilities.bits & CapabilityMask(capability)) != 0;
}

std::vector<IparNoopBranchDecision> BuildIparNoopBranchTable(
    const IparObjectCapabilitySet& capabilities) {
  const struct Branch {
    const char* name;
    IparCapability capability;
  } branches[] = {
      {"triggers", IparCapability::triggers},
      {"defaults", IparCapability::defaults},
      {"generated_columns", IparCapability::generated_columns},
      {"constraints", IparCapability::constraints},
      {"foreign_keys", IparCapability::foreign_keys},
      {"row_level_security", IparCapability::row_level_security},
      {"security_masks", IparCapability::security_masks},
      {"unique_indexes", IparCapability::unique_indexes},
      {"large_values", IparCapability::large_values},
      {"external_storage", IparCapability::external_storage},
      {"audit_hooks", IparCapability::audit_hooks},
      {"replication_hooks", IparCapability::replication_hooks},
      {"optional_hooks", IparCapability::optional_hooks}};

  std::vector<IparNoopBranchDecision> table;
  table.reserve(sizeof(branches) / sizeof(branches[0]));
  for (const auto& branch : branches) {
    const bool required = IparObjectCapabilityHas(capabilities, branch.capability);
    table.push_back({branch.name, required, !required});
  }
  return table;
}

IparRowLayoutBuildResult BuildIparRowLayoutDescriptor(
    std::string table_uuid,
    std::string statement_uuid,
    IparCompressedEpochVector epoch,
    std::vector<IparRowLayoutColumn> columns) {
  IparRowLayoutBuildResult result;
  if (table_uuid.empty() || statement_uuid.empty() || !epoch.complete ||
      columns.empty()) {
    result.diagnostic_code = "SB_IPAR_ROW_LAYOUT.REQUEST_INVALID";
    result.detail = "table_statement_epoch_and_columns_required";
    return result;
  }

  std::sort(columns.begin(),
            columns.end(),
            [](const IparRowLayoutColumn& left,
               const IparRowLayoutColumn& right) {
              return left.ordinal < right.ordinal;
            });

  result.layout.table_uuid = std::move(table_uuid);
  result.layout.statement_uuid = std::move(statement_uuid);
  result.layout.epoch = std::move(epoch);
  result.layout.null_bitmap_bytes =
      static_cast<std::uint32_t>((columns.size() + 7u) / 8u);
  result.layout.fixed_row_bytes = result.layout.null_bitmap_bytes;
  std::uint32_t variable_index = 0;
  for (const auto& column : columns) {
    if (column.column_uuid.empty() ||
        column.descriptor.descriptor_uuid.canonical.empty() ||
        column.descriptor.canonical_type_name.empty()) {
      result.diagnostic_code = "SB_IPAR_ROW_LAYOUT.COLUMN_INVALID";
      result.detail = "column_uuid_and_descriptor_required";
      result.layout = {};
      return result;
    }

    IparRowLayoutSlot slot;
    slot.column_uuid = column.column_uuid;
    slot.ordinal = column.ordinal;
    slot.fixed_width_bytes = column.variable_width ? sizeof(std::uint32_t)
                                                   : column.fixed_width_bytes;
    slot.variable_width = column.variable_width;
    slot.nullable = column.nullable;
    slot.uses_default_or_generated =
        column.has_default || column.has_generated_value;
    slot.coercion_required = column.coercion_required;
    const std::uint32_t alignment =
        std::min<std::uint32_t>(std::max<std::uint32_t>(slot.fixed_width_bytes, 1), 8);
    result.layout.fixed_row_bytes =
        AlignUp(result.layout.fixed_row_bytes, alignment);
    slot.fixed_offset = result.layout.fixed_row_bytes;
    result.layout.fixed_row_bytes += slot.fixed_width_bytes;
    if (column.variable_width) {
      slot.variable_index = variable_index++;
    }
    result.layout.slots.push_back(slot);
    result.layout.parameter_bind_map.push_back(
        column.column_uuid + ":" + std::to_string(column.ordinal) + ":" +
        std::to_string(slot.fixed_offset));
  }
  result.layout.variable_column_count = variable_index;

  std::vector<std::string> digest_parts = {
      kRowLayoutAnchor,
      kAuthorityScope,
      result.layout.table_uuid,
      result.layout.statement_uuid,
      result.layout.epoch.digest,
      std::to_string(result.layout.fixed_row_bytes),
      std::to_string(result.layout.null_bitmap_bytes),
      std::to_string(result.layout.variable_column_count)};
  for (const auto& slot : result.layout.slots) {
    digest_parts.push_back(SlotDigestText(slot));
  }
  result.layout.encoder_digest = StableDigest(digest_parts);
  result.ok = true;
  result.diagnostic_code = "SB_IPAR_ROW_LAYOUT.BUILT";
  result.detail = "row_layout_and_parameter_encoder_ready";
  return result;
}

std::string IparRowLayoutCacheKey(const std::string& table_uuid,
                                  const std::string& statement_uuid,
                                  const IparCompressedEpochVector& epoch,
                                  const std::string& encoder_digest) {
  return table_uuid + "|" + statement_uuid + "|" + epoch.digest + "|" +
         encoder_digest;
}

IparParameterEncoderLookupResult IparParameterEncoderCache::Put(
    IparRowLayoutDescriptor layout) {
  if (layout.table_uuid.empty() ||
      layout.statement_uuid.empty() ||
      !layout.epoch.complete ||
      layout.encoder_digest.empty() ||
      !layout.security_recheck_required ||
      !layout.visibility_recheck_required ||
      layout.parser_authority) {
    return Refusal("SB_IPAR_ENCODER_CACHE.UNSAFE_OR_INVALID",
                   "layout requires table, statement, complete epoch, digest, and rechecks");
  }
  const std::string key = IparRowLayoutCacheKey(
      layout.table_uuid, layout.statement_uuid, layout.epoch, layout.encoder_digest);
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[key] = std::move(layout);
  IparParameterEncoderLookupResult result;
  result.ok = true;
  result.diagnostic_code = "SB_IPAR_ENCODER_CACHE.PUT";
  return result;
}

IparParameterEncoderLookupResult IparParameterEncoderCache::Lookup(
    const std::string& table_uuid,
    const std::string& statement_uuid,
    const IparCompressedEpochVector& epoch,
    const std::string& encoder_digest) {
  if (table_uuid.empty() || statement_uuid.empty() || !epoch.complete ||
      encoder_digest.empty()) {
    return Refusal("SB_IPAR_ENCODER_CACHE.REQUEST_INVALID",
                   "table_statement_epoch_and_digest_required");
  }
  const std::string key =
      IparRowLayoutCacheKey(table_uuid, statement_uuid, epoch, encoder_digest);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return Refusal("SB_IPAR_ENCODER_CACHE.MISS", "layout_not_cached");
  }
  if (!IparCompressedEpochVectorMatches(found->second.epoch, epoch)) {
    auto result = Refusal("SB_IPAR_ENCODER_CACHE.STALE", "epoch_mismatch");
    result.stale = true;
    return result;
  }
  IparParameterEncoderLookupResult result;
  result.ok = true;
  result.cache_hit = true;
  result.diagnostic_code = "SB_IPAR_ENCODER_CACHE.HIT";
  result.layout = found->second;
  return result;
}

std::uint64_t IparParameterEncoderCache::InvalidateStale(
    const IparCompressedEpochVector& current_epoch) {
  std::uint64_t count = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (IparCompressedEpochVectorMatches(it->second.epoch, current_epoch)) {
      ++it;
      continue;
    }
    it = entries_.erase(it);
    ++count;
  }
  return count;
}

void IparParameterEncoderCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

IparWarmProfilePlan PlanIparDatabaseOpenWarmProfile(
    IparWarmProfileRequest request) {
  IparWarmProfilePlan plan;
  if (request.database_uuid.empty() || !request.open_epoch.complete ||
      request.budget_bytes == 0) {
    plan.diagnostic_code = "SB_IPAR_WARM_OPEN.REQUEST_INVALID";
    return plan;
  }
  std::sort(request.items.begin(),
            request.items.end(),
            [](const IparWarmProfileItem& left,
               const IparWarmProfileItem& right) {
              if (left.priority != right.priority) {
                return left.priority > right.priority;
              }
              return left.bytes < right.bytes;
            });
  for (const auto& item : request.items) {
    if (item.item_id.empty() || item.item_kind.empty() ||
        item.object_uuid.empty()) {
      plan.skipped.push_back("identity_required");
      continue;
    }
    if (!item.authorization_checked || !item.policy_checked) {
      plan.skipped.push_back(item.item_id + ":authorization_or_policy_missing");
      continue;
    }
    if (!IparCompressedEpochVectorMatches(item.epoch, request.open_epoch)) {
      plan.skipped.push_back(item.item_id + ":epoch_mismatch");
      continue;
    }
    if (item.bytes == 0 ||
        plan.selected_bytes + item.bytes > request.budget_bytes) {
      plan.skipped.push_back(item.item_id + ":budget");
      continue;
    }
    plan.selected.push_back(item);
    plan.selected_bytes += item.bytes;
  }
  plan.ok = true;
  plan.diagnostic_code = "SB_IPAR_WARM_OPEN.PLANNED";
  plan.evidence.push_back(kWarmOpenAnchor);
  plan.evidence.push_back(kAuthorityScope);
  plan.evidence.push_back(kCapabilityAnchor);
  plan.evidence.push_back(kNoopAnchor);
  plan.evidence.push_back("ipar.database_open_warm_profile.policy_bounded=true");
  plan.evidence.push_back("ipar.database_open_warm_profile.authorization_checked=true");
  return plan;
}

}  // namespace scratchbird::engine::internal_api
