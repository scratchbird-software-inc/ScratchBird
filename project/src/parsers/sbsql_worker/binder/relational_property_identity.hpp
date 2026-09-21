// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "binder/binder.hpp"
#include "core/uuid/uuid.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace scratchbird::parser::sbsql {
namespace property_identity_detail {
inline constexpr std::size_t kMaximumPropertyIdentities = 524288;
using Key = BoundRelationalPropertyKey;
using Role = BoundRelationalPropertyRole;

inline std::optional<std::set<Key>> RequiredKeys(const BoundNativeRelationalDocument& bound) {
  std::set<Key> keys;
  std::map<std::uint32_t, const BoundRelationAstRecord*> relations;
  std::map<std::uint32_t, const BoundWindowInvocationAstRecord*> invocations;
  std::set<std::uint32_t> definitions, patterns;
  for (const auto& invocation : bound.window_invocations)
    if (!invocation.invocation_id || !invocations.emplace(invocation.invocation_id, &invocation).second) return std::nullopt;
  for (const auto& definition : bound.window_definitions)
    if (!definition.window_id || !definitions.insert(definition.window_id).second) return std::nullopt;
  for (const auto& relation : bound.relations) {
    if (!relation.relation_id || !relations.emplace(relation.relation_id, &relation).second) return std::nullopt;
    if (relation.relation_kind == NativeRelationAstKind::kSort) keys.insert({relation.relation_id, 0, Role::kSortOrdering});
    if (relation.relation_kind == NativeRelationAstKind::kWindow) {
      for (const auto id : relation.window_invocation_ids) {
        const auto invocation = invocations.find(id);
        if (invocation == invocations.end() || !definitions.contains(invocation->second->window_definition_id)) return std::nullopt;
        for (const auto role : {Role::kWindowPartition, Role::kWindowOrdering, Role::kWindowResult, Role::kWindowFrame})
          keys.insert({relation.relation_id, invocation->second->window_definition_id, role});
        if (keys.size() > kMaximumPropertyIdentities) return std::nullopt;
      }
    }
    if (keys.size() > kMaximumPropertyIdentities) return std::nullopt;
  }
  for (const auto& pattern : bound.row_patterns) {
    const auto relation = relations.find(pattern.relation_id);
    if (!pattern.pattern_id || !patterns.insert(pattern.pattern_id).second || relation == relations.end() ||
        relation->second->relation_kind != NativeRelationAstKind::kMatchRecognize) return std::nullopt;
    keys.insert({pattern.relation_id, pattern.pattern_id, Role::kPatternPartition});
    keys.insert({pattern.relation_id, pattern.pattern_id, Role::kPatternOrdering});
    if (keys.size() > kMaximumPropertyIdentities) return std::nullopt;
  }
  return keys;
}

inline bool ExistingIdentitiesValid(const std::vector<BoundRelationalPropertyIdentity>& identities,
                                     const std::set<Key>& required, bool complete) {
  if (identities.size() > kMaximumPropertyIdentities || (complete && identities.size() != required.size())) return false;
  std::set<core::platform::Uuid> unique;
  std::optional<Key> previous;
  for (const auto& identity : identities) {
    if (!required.contains(identity.key) || (previous && !(*previous < identity.key)) ||
        !core::uuid::IsEngineIdentityUuid(identity.uuid) || !unique.insert(identity.uuid).second) return false;
    previous = identity.key;
  }
  return true;
}

// The injectable call is for fault qualification of the same publication path;
// production below always uses the actual runtime UUID issuer.
template <class Issuer>
bool Finalize(BoundNativeRelationalDocument* bound, Issuer issue) {
  if (!bound || !bound->bound) return false;
  const auto required = RequiredKeys(*bound);
  if (!required || !ExistingIdentitiesValid(bound->property_identities, *required, false)) return false;
  if (bound->property_identities.size() == required->size()) return true;
  std::map<Key, core::platform::Uuid> retained;
  std::set<core::platform::Uuid> unique;
  for (const auto& record : bound->property_identities) {
    retained.emplace(record.key, record.uuid); unique.insert(record.uuid);
  }
  std::vector<BoundRelationalPropertyIdentity> staged;
  staged.reserve(required->size());
  for (const auto& key : *required) {
    if (const auto found = retained.find(key); found != retained.end()) staged.push_back({key, found->second});
    else {
      const auto id = issue();
      if (!id || !core::uuid::IsEngineIdentityUuid(*id) || !unique.insert(*id).second) return false;
      staged.push_back({key, *id});
    }
  }
  bound->property_identities.swap(staged);
  return true;
}
}  // namespace property_identity_detail

inline bool FinalizeRelationalPropertyIdentities(BoundNativeRelationalDocument* bound) {
  return property_identity_detail::Finalize(bound, [] { return core::uuid::IssueRuntimeIdentityV7(); });
}

inline bool ValidateRelationalPropertyIdentities(const BoundNativeRelationalDocument& bound) {
  const auto required = property_identity_detail::RequiredKeys(bound);
  return required && property_identity_detail::ExistingIdentitiesValid(bound.property_identities, *required, true);
}

inline std::optional<core::platform::Uuid> FindRelationalPropertyIdentity(
    const BoundNativeRelationalDocument& bound, BoundRelationalPropertyKey key) {
  const auto found = std::ranges::lower_bound(bound.property_identities, key, {}, &BoundRelationalPropertyIdentity::key);
  if (found == bound.property_identities.end() || found->key != key || !core::uuid::IsEngineIdentityUuid(found->uuid))
    return std::nullopt;
  return found->uuid;
}
}  // namespace scratchbird::parser::sbsql
