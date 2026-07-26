// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/name_resolution_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/global_aggregate_view.hpp"
#include "catalog/name_registry.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "database_lifecycle.hpp"
#include "resource_seed_pack.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

struct TemporaryFilteredNameMatches {
  bool ok = true;
  EngineApiDiagnostic diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  std::vector<NameRegistryEntry> temporary_matches;
  std::vector<NameRegistryEntry> durable_matches;
};

std::string LowerResourceClass(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool IsEngineResourceClass(const std::string& object_class) {
  const std::string normalized = LowerResourceClass(object_class);
  return normalized == "charset" || normalized == "collation";
}

EngineResolveNameResult ResourceResolutionFailure(
    const EngineResolveNameRequest& request,
    std::string code,
    std::string message_key,
    std::string detail) {
  return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
      request.context,
      "catalog.resolve_name",
      MakeEngineApiDiagnostic(std::move(code),
                              std::move(message_key),
                              std::move(detail)));
}

bool IsResourceReadableTransactionState(
    scratchbird::transaction::mga::TransactionState state) {
  using scratchbird::transaction::mga::TransactionState;
  return state == TransactionState::active ||
         state == TransactionState::read_only_active;
}

std::optional<EngineResolveNameResult> ResolveEngineResourceName(
    const EngineResolveNameRequest& request) {
  const std::string resource_class =
      LowerResourceClass(request.sql_object_reference.expected_object_type);
  if (!IsEngineResourceClass(resource_class)) {
    return std::nullopt;
  }
  if (!request.sql_object_reference.path_components.empty() ||
      request.sql_object_reference.object_name.raw_text.empty()) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.NAME_INVALID",
        "catalog.resource.name_invalid",
        "resource_names_must_be_unqualified_and_nonempty");
  }
  if (request.context.database_path.empty()) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.DATABASE_REQUIRED",
        "catalog.resource.database_required",
        "database_path_required");
  }
  if (request.context.local_transaction_id == 0 ||
      request.context.transaction_uuid.canonical.empty()) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.TRANSACTION_REQUIRED",
        "catalog.resource.transaction_required",
        "exact_active_transaction_identity_required");
  }

  scratchbird::storage::database::DatabaseOpenConfig open_config;
  open_config.path = request.context.database_path;
  open_config.read_only = true;
  open_config.suppress_background_agents = true;
  const auto opened =
      scratchbird::storage::database::OpenDatabaseFile(open_config);
  if (!opened.ok()) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.CATALOG_UNAVAILABLE",
        "catalog.resource.catalog_unavailable",
        opened.diagnostic.diagnostic_code);
  }
  if (!opened.state.resource_seed_catalog_present ||
      !opened.state.resource_seed_catalog.active) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.CATALOG_REQUIRED",
        "catalog.resource.catalog_required",
        "durable_resource_seed_catalog_required");
  }

  const auto parsed_transaction = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      request.context.transaction_uuid.canonical);
  if (!parsed_transaction.ok()) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.TRANSACTION_INVALID",
        "catalog.resource.transaction_invalid",
        "transaction_uuid_malformed");
  }
  const scratchbird::transaction::mga::TransactionInventoryEntry*
      transaction_entry = nullptr;
  for (const auto& entry : opened.state.local_transaction_inventory.entries) {
    if (entry.identity.local_id.value == request.context.local_transaction_id) {
      transaction_entry = &entry;
      break;
    }
  }
  if (transaction_entry == nullptr ||
      transaction_entry->identity.transaction_uuid.value !=
          parsed_transaction.value.value ||
      !IsResourceReadableTransactionState(transaction_entry->state)) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.TRANSACTION_NOT_ACTIVE",
        "catalog.resource.transaction_not_active",
        "exact_active_transaction_identity_required");
  }

  if (!request.context.database_uuid.canonical.empty() &&
      request.context.database_uuid.canonical !=
          scratchbird::core::uuid::UuidToString(
              opened.state.database_uuid.value)) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.DATABASE_IDENTITY_MISMATCH",
        "catalog.resource.database_identity_mismatch",
        "database_uuid_does_not_match_catalog_authority");
  }

  const auto& image = opened.state.resource_seed_catalog;
  if (request.context.resource_epoch != 0 &&
      request.context.resource_epoch != image.resource_epoch) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.EPOCH_STALE",
        "catalog.resource.epoch_stale",
        "requested=" + std::to_string(request.context.resource_epoch) +
            ";current=" + std::to_string(image.resource_epoch));
  }

  EngineResolvedResourceDescriptor descriptor;
  descriptor.present = true;
  descriptor.resource_family = resource_class;
  descriptor.seed_pack_name = image.seed_pack_name;
  descriptor.seed_pack_version = image.seed_pack_version;
  descriptor.resource_epoch = image.resource_epoch;
  const std::string& requested_name =
      request.sql_object_reference.object_name.raw_text;
  if (resource_class == "charset") {
    const auto* charset =
        scratchbird::core::resources::FindResourceSeedCharset(image,
                                                               requested_name);
    if (charset == nullptr) {
      return ResourceResolutionFailure(
          request,
          "CATALOG.NAME.NOT_FOUND",
          "message_vector.item_not_found_or_does_not_exist",
          "charset_not_found_or_not_visible");
    }
    descriptor.canonical_name = charset->canonical_name;
    descriptor.resource_uuid.canonical = charset->resource_uuid;
    descriptor.default_collation_uuid.canonical =
        charset->default_collation_uuid;
    descriptor.default_collation_name = charset->default_collation_name;
    descriptor.family_epoch = charset->family_epoch;
    descriptor.family_version = charset->family_version;
    descriptor.min_bytes = charset->min_bytes;
    descriptor.max_bytes = charset->max_bytes;
    descriptor.variable_width = charset->variable_width;
  } else {
    const auto* collation =
        scratchbird::core::resources::FindResourceSeedCollation(image,
                                                                 requested_name);
    if (collation == nullptr) {
      return ResourceResolutionFailure(
          request,
          "CATALOG.NAME.NOT_FOUND",
          "message_vector.item_not_found_or_does_not_exist",
          "collation_not_found_or_not_visible");
    }
    descriptor.canonical_name = collation->canonical_name;
    descriptor.resource_uuid.canonical = collation->resource_uuid;
    descriptor.parent_resource_uuid.canonical = collation->charset_uuid;
    descriptor.parent_canonical_name = collation->charset_name;
    descriptor.family_epoch = collation->family_epoch;
    descriptor.family_version = collation->family_version;
    descriptor.default_for_parent = collation->default_for_charset;
    descriptor.case_insensitive = collation->case_insensitive;
    descriptor.accent_insensitive = collation->accent_insensitive;
  }

  if (descriptor.resource_uuid.canonical.empty() ||
      descriptor.resource_epoch == 0 || descriptor.family_epoch == 0 ||
      descriptor.family_version.empty() ||
      (resource_class == "charset" &&
       (descriptor.min_bytes == 0 ||
        descriptor.max_bytes < descriptor.min_bytes)) ||
      (resource_class == "collation" &&
       descriptor.parent_resource_uuid.canonical.empty())) {
    return ResourceResolutionFailure(
        request,
        "CATALOG.RESOURCE.DESCRIPTOR_INVALID",
        "catalog.resource.descriptor_invalid",
        descriptor.canonical_name);
  }

  auto result = MakeApiBehaviorSuccess<EngineResolveNameResult>(
      request.context, "catalog.resolve_name");
  result.primary_object.uuid = descriptor.resource_uuid;
  result.primary_object.object_kind = resource_class;
  result.bound_object_identity.object_uuid = descriptor.resource_uuid;
  result.bound_object_identity.resolved_object_type = resource_class;
  result.bound_object_identity.parent_object_uuid =
      descriptor.parent_resource_uuid;
  result.bound_object_identity.catalog_generation_id =
      request.context.catalog_generation_id;
  result.bound_object_identity.security_epoch = request.context.security_epoch;
  result.bound_object_identity.resource_epoch = descriptor.resource_epoch;
  result.resource_descriptor = descriptor;
  AddApiBehaviorRow(
      &result,
      {{"object_uuid", descriptor.resource_uuid.canonical},
       {"object_kind", resource_class},
       {"name", descriptor.canonical_name},
       {"resource_epoch", std::to_string(descriptor.resource_epoch)},
       {"family_epoch", std::to_string(descriptor.family_epoch)},
       {"family_version", descriptor.family_version},
       {"parent_resource_uuid", descriptor.parent_resource_uuid.canonical},
       {"default_collation_uuid", descriptor.default_collation_uuid.canonical},
       {"default_collation_name", descriptor.default_collation_name},
       {"min_bytes", std::to_string(descriptor.min_bytes)},
       {"max_bytes", std::to_string(descriptor.max_bytes)}});
  AddApiBehaviorEvidence(&result,
                         "resource_catalog_authority",
                         "durable_database_seed_catalog");
  AddApiBehaviorEvidence(&result,
                         "resource_family_version",
                         descriptor.family_version);
  return result;
}

bool ObjectClassCanBeTemporaryTable(const std::string& object_class) {
  return object_class == "table" || object_class == "relation";
}

bool NameRegistryMatchCanBeTemporaryTable(const NameRegistryEntry& match) {
  return ObjectClassCanBeTemporaryTable(match.object_class);
}

TemporaryFilteredNameMatches FilterTemporaryNameMatches(
    const EngineResolveNameRequest& request,
    const std::vector<NameRegistryEntry>& matches) {
  TemporaryFilteredNameMatches filtered;
  for (const auto& match : matches) {
    if (!NameRegistryMatchCanBeTemporaryTable(match)) {
      filtered.durable_matches.push_back(match);
      continue;
    }
    const auto visibility =
        CheckMgaTemporaryTableVisibility(request.context, match.object_uuid);
    if (!visibility.ok) {
      filtered.ok = false;
      filtered.diagnostic = visibility.diagnostic;
      return filtered;
    }
    if (!visibility.table_visible) {
      continue;
    }
    if (!visibility.known_temporary) {
      filtered.durable_matches.push_back(match);
      continue;
    }
    if (visibility.visible_to_session) {
      filtered.temporary_matches.push_back(match);
    }
  }
  return filtered;
}

EngineApiDiagnostic AttachGlobalAggregateViewSemanticProjection(
    const EngineResolveNameRequest& request,
    const std::string& object_kind,
    const std::string& object_uuid,
    EngineResolveNameResult* result) {
  if (object_kind != "view") {
    return MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  if (result == nullptr || object_uuid.empty()) {
    return MakeInvalidRequestDiagnostic(
        "catalog.resolve_name", "semantic_projection_output_required");
  }

  const auto view =
      DescribeEngineGlobalAggregateView(request.context, object_uuid);
  if (view.diagnostic.error) return view.diagnostic;
  if (!view.present) {
    return MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  const auto semantic = EngineGlobalAggregateViewSemanticDescriptor(view);
  if (semantic.descriptor_uuid.canonical.empty() ||
      semantic.descriptor_kind != "global_aggregate_view" ||
      semantic.canonical_type_name != kEngineGlobalAggregateViewMarkerV1 ||
      semantic.encoded_descriptor.empty()) {
    return MakeInvalidRequestDiagnostic(
        "catalog.resolve_name",
        "global_aggregate_view_semantic_descriptor_invalid");
  }

  result->semantic_projection.present = true;
  result->semantic_projection.marker = kEngineGlobalAggregateViewMarkerV1;
  result->semantic_projection.projection_descriptor = semantic;
  result->semantic_projection.descriptor_generation =
      view.view_descriptor_generation;
  result->semantic_projection.result_alias = view.result_alias;
  result->semantic_projection.result_descriptor = view.result_descriptor;
  AddApiBehaviorRow(
      result,
      {{"semantic_projection_marker", kEngineGlobalAggregateViewMarkerV1},
       {"view_descriptor_uuid", semantic.descriptor_uuid.canonical},
       {"view_descriptor_generation",
        std::to_string(view.view_descriptor_generation)},
       {"result_alias", view.result_alias},
       {"result_type", view.result_descriptor.canonical_type_name},
       {"result_nullable", "true"}});
  AddApiBehaviorEvidence(result,
                         "semantic_projection_authority",
                         "engine_global_aggregate_view_descriptor");
  AddApiBehaviorEvidence(result,
                         "semantic_projection_marker",
                         kEngineGlobalAggregateViewMarkerV1);
  return MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool SafeSemanticOutputName(std::string_view name) {
  if (name.empty() || name.size() > 128) return false;
  const auto ascii_alpha = [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
  };
  const auto ascii_digit = [](unsigned char ch) {
    return ch >= '0' && ch <= '9';
  };
  const unsigned char first = static_cast<unsigned char>(name.front());
  if (!ascii_alpha(first) && first != '_') return false;
  for (const unsigned char ch : name) {
    if (!ascii_alpha(ch) && !ascii_digit(ch) && ch != '_') return false;
  }
  return true;
}

bool CanonicalSemanticObjectUuid(std::string_view value) {
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::object,
      std::string(value));
  return parsed.ok() &&
         scratchbird::core::uuid::UuidToString(parsed.value.value) == value;
}

EngineApiDiagnostic AttachRelationProjectionViewSemanticProjection(
    const EngineResolveNameRequest& request,
    const std::string& object_kind,
    const std::string& object_uuid,
    EngineResolveNameResult* result) {
  if (object_kind != "view") {
    return MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  if (result == nullptr || object_uuid.empty()) {
    return MakeInvalidRequestDiagnostic(
        "catalog.resolve_name", "semantic_projection_output_required");
  }
  if (result->semantic_projection.present) {
    return MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }

  const auto view =
      DescribeEngineRelationProjectionView(request.context, object_uuid);
  if (view.diagnostic.error) return view.diagnostic;
  if (!view.present) {
    return MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  const auto semantic =
      EngineRelationProjectionViewSemanticDescriptor(view);
  const auto outputs = EngineRelationProjectionViewSemanticOutputs(view);
  const bool v1 = view.marker == kEngineRelationProjectionViewMarkerV1;
  const bool v2 = view.marker == kEngineRelationProjectionViewMarkerV2;
  const std::size_t expected_output_count = v1 ? 2u : 1u;
  if (semantic.descriptor_uuid.canonical.empty() ||
      semantic.descriptor_kind != "relation_projection_view" ||
      (!v1 && !v2) || semantic.canonical_type_name != view.marker ||
      semantic.encoded_descriptor.empty() ||
      outputs.size() != expected_output_count ||
      !CanonicalSemanticObjectUuid(view.view_uuid.canonical) ||
      !CanonicalSemanticObjectUuid(semantic.descriptor_uuid.canonical)) {
    return MakeInvalidRequestDiagnostic(
        "catalog.resolve_name",
        "relation_projection_view_semantic_descriptor_invalid");
  }
  std::set<std::string> semantic_identities = {
      view.view_uuid.canonical, semantic.descriptor_uuid.canonical};
  for (std::size_t index = 0; index < outputs.size(); ++index) {
    const auto& output = outputs[index];
    if (output.ordinal != index ||
        !SafeSemanticOutputName(output.output_name) ||
        !CanonicalSemanticObjectUuid(output.output_column_uuid.canonical) ||
        !CanonicalSemanticObjectUuid(
            output.output_type.type_descriptor_uuid.canonical)) {
      return MakeInvalidRequestDiagnostic(
          "catalog.resolve_name",
          "relation_projection_view_semantic_descriptor_invalid");
    }
    semantic_identities.insert(output.output_column_uuid.canonical);
    semantic_identities.insert(
        output.output_type.type_descriptor_uuid.canonical);
  }
  if (semantic_identities.size() != 2u + outputs.size() * 2u) {
    return MakeInvalidRequestDiagnostic(
        "catalog.resolve_name",
        "relation_projection_view_semantic_identity_collision");
  }

  result->semantic_projection.present = true;
  result->semantic_projection.marker = view.marker;
  result->semantic_projection.projection_descriptor = semantic;
  result->semantic_projection.descriptor_generation =
      view.view_descriptor_generation;
  result->semantic_projection.ordered_outputs = outputs;
  AddApiBehaviorRow(
      result,
      {{"semantic_projection_marker",
        view.marker},
       {"view_descriptor_uuid", semantic.descriptor_uuid.canonical},
       {"view_descriptor_generation",
        std::to_string(view.view_descriptor_generation)},
       {"output_count", std::to_string(outputs.size())}});
  for (const auto& output : outputs) {
    AddApiBehaviorRow(
        result,
        {{"semantic_projection_output_ordinal",
          std::to_string(output.ordinal)},
         {"semantic_projection_output_name", output.output_name},
         {"semantic_projection_output_column_uuid",
          output.output_column_uuid.canonical},
         {"semantic_projection_output_type_descriptor_uuid",
          output.output_type.type_descriptor_uuid.canonical},
         {"semantic_projection_output_type",
          output.output_type.canonical_type_name},
         {"semantic_projection_output_nullable",
          output.nullable ? "true" : "false"}});
  }
  AddApiBehaviorEvidence(
      result,
      "semantic_projection_authority",
      "engine_relation_projection_view_descriptor");
  AddApiBehaviorEvidence(result,
                         "semantic_projection_marker",
                         view.marker);
  return MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic AttachViewSemanticProjection(
    const EngineResolveNameRequest& request,
    const std::string& object_kind,
    const std::string& object_uuid,
    EngineResolveNameResult* result) {
  const auto aggregate = AttachGlobalAggregateViewSemanticProjection(
      request, object_kind, object_uuid, result);
  if (aggregate.error ||
      (result != nullptr && result->semantic_projection.present)) {
    return aggregate;
  }
  return AttachRelationProjectionViewSemanticProjection(
      request, object_kind, object_uuid, result);
}

EngineResolveNameResult MakeNameRegistryResolveResult(
    const EngineResolveNameRequest& request,
    const NameRegistryEntry& match,
    bool temporary_shadow_match) {
  auto result = MakeApiBehaviorSuccess<EngineResolveNameResult>(
      request.context,
      "catalog.resolve_name");
  result.primary_object.uuid.canonical = match.object_uuid;
  result.primary_object.object_kind = match.object_class;
  result.bound_object_identity.object_uuid.canonical = match.object_uuid;
  result.bound_object_identity.resolved_object_type = match.object_class;
  result.bound_object_identity.resolved_schema_uuid.canonical = match.scope_uuid;
  result.bound_object_identity.parent_object_uuid.canonical = match.parent_object_uuid;
  result.bound_object_identity.catalog_generation_id = match.catalog_generation_id;
  result.bound_object_identity.security_epoch = request.context.security_epoch;
  result.bound_object_identity.resource_epoch = match.resource_epoch;
  AddApiBehaviorRow(&result, {{"object_uuid", match.object_uuid},
                              {"object_kind", match.object_class},
                              {"name", match.display_name},
                              {"scope_uuid", match.scope_uuid},
                              {"identifier_profile_uuid", match.identifier_profile_uuid},
                              {"language_tag", match.language_tag}});
  AddApiBehaviorEvidence(&result, "name_resolution", match.normalized_lookup_key);
  AddApiBehaviorEvidence(&result, "name_entry", match.name_entry_uuid);
  if (temporary_shadow_match) {
    AddApiBehaviorEvidence(&result,
                           "temporary_name_resolution",
                           "session_visible_shadow");
  }
  const auto semantic = AttachViewSemanticProjection(
      request, match.object_class, match.object_uuid, &result);
  if (semantic.error) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
        request.context, "catalog.resolve_name", semantic);
  }
  return result;
}

}  // namespace

EngineResourceDescriptorLookupResult LookupEngineResourceDescriptorByUuid(
    const EngineRequestContext& context,
    const EngineUuid& resource_uuid,
    const std::string& expected_resource_family) {
  EngineResourceDescriptorLookupResult result;
  auto fail = [&](std::string code,
                  std::string message_key,
                  std::string detail) {
    result.diagnostic = MakeEngineApiDiagnostic(std::move(code),
                                                std::move(message_key),
                                                std::move(detail));
    return result;
  };

  const std::string resource_family =
      LowerResourceClass(expected_resource_family);
  if (!IsEngineResourceClass(resource_family)) {
    return fail("CATALOG.RESOURCE.FAMILY_INVALID",
                "catalog.resource.family_invalid",
                expected_resource_family);
  }
  if (resource_uuid.canonical.empty()) {
    return fail("CATALOG.RESOURCE.UUID_REQUIRED",
                "catalog.resource.uuid_required",
                resource_family + "_uuid_required");
  }
  const auto parsed_resource = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::object,
      resource_uuid.canonical);
  if (!parsed_resource.ok()) {
    return fail("CATALOG.RESOURCE.UUID_INVALID",
                "catalog.resource.uuid_invalid",
                resource_family + "_uuid_malformed");
  }
  const std::string canonical_resource_uuid =
      scratchbird::core::uuid::UuidToString(parsed_resource.value.value);

  if (context.database_path.empty()) {
    return fail("CATALOG.RESOURCE.DATABASE_REQUIRED",
                "catalog.resource.database_required",
                "database_path_required");
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return fail("CATALOG.RESOURCE.TRANSACTION_REQUIRED",
                "catalog.resource.transaction_required",
                "exact_active_transaction_identity_required");
  }

  scratchbird::storage::database::DatabaseOpenConfig open_config;
  open_config.path = context.database_path;
  open_config.read_only = true;
  open_config.suppress_background_agents = true;
  const auto opened =
      scratchbird::storage::database::OpenDatabaseFile(open_config);
  if (!opened.ok()) {
    return fail("CATALOG.RESOURCE.CATALOG_UNAVAILABLE",
                "catalog.resource.catalog_unavailable",
                opened.diagnostic.diagnostic_code);
  }
  if (!opened.state.resource_seed_catalog_present ||
      !opened.state.resource_seed_catalog.active) {
    return fail("CATALOG.RESOURCE.CATALOG_REQUIRED",
                "catalog.resource.catalog_required",
                "durable_resource_seed_catalog_required");
  }

  const auto parsed_transaction = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      context.transaction_uuid.canonical);
  if (!parsed_transaction.ok()) {
    return fail("CATALOG.RESOURCE.TRANSACTION_INVALID",
                "catalog.resource.transaction_invalid",
                "transaction_uuid_malformed");
  }
  const scratchbird::transaction::mga::TransactionInventoryEntry*
      transaction_entry = nullptr;
  for (const auto& entry : opened.state.local_transaction_inventory.entries) {
    if (entry.identity.local_id.value == context.local_transaction_id) {
      transaction_entry = &entry;
      break;
    }
  }
  if (transaction_entry == nullptr ||
      transaction_entry->identity.transaction_uuid.value !=
          parsed_transaction.value.value ||
      !IsResourceReadableTransactionState(transaction_entry->state)) {
    return fail("CATALOG.RESOURCE.TRANSACTION_NOT_ACTIVE",
                "catalog.resource.transaction_not_active",
                "exact_active_transaction_identity_required");
  }

  if (!context.database_uuid.canonical.empty() &&
      context.database_uuid.canonical !=
          scratchbird::core::uuid::UuidToString(
              opened.state.database_uuid.value)) {
    return fail("CATALOG.RESOURCE.DATABASE_IDENTITY_MISMATCH",
                "catalog.resource.database_identity_mismatch",
                "database_uuid_does_not_match_catalog_authority");
  }

  const auto& image = opened.state.resource_seed_catalog;
  if (context.resource_epoch == 0) {
    return fail("CATALOG.RESOURCE.EPOCH_REQUIRED",
                "catalog.resource.epoch_required",
                "exact_nonzero_resource_epoch_required");
  }
  if (context.resource_epoch != image.resource_epoch) {
    return fail("CATALOG.RESOURCE.EPOCH_STALE",
                "catalog.resource.epoch_stale",
                "requested=" + std::to_string(context.resource_epoch) +
                    ";current=" + std::to_string(image.resource_epoch));
  }

  EngineResolvedResourceDescriptor descriptor;
  descriptor.present = true;
  descriptor.resource_family = resource_family;
  descriptor.seed_pack_name = image.seed_pack_name;
  descriptor.seed_pack_version = image.seed_pack_version;
  descriptor.resource_epoch = image.resource_epoch;
  if (resource_family == "charset") {
    const scratchbird::core::resources::ResourceSeedCharsetDescriptor*
        matched = nullptr;
    for (const auto& charset : image.charsets) {
      if (charset.resource_uuid == canonical_resource_uuid) {
        matched = &charset;
        break;
      }
    }
    if (matched == nullptr) {
      for (const auto& collation : image.collations) {
        if (collation.resource_uuid == canonical_resource_uuid) {
          return fail("CATALOG.RESOURCE.FAMILY_MISMATCH",
                      "catalog.resource.family_mismatch",
                      "expected=charset;actual=collation");
        }
      }
      return fail("CATALOG.RESOURCE.UUID_NOT_FOUND",
                  "catalog.resource.uuid_not_found",
                  canonical_resource_uuid);
    }
    descriptor.canonical_name = matched->canonical_name;
    descriptor.resource_uuid.canonical = matched->resource_uuid;
    descriptor.default_collation_uuid.canonical =
        matched->default_collation_uuid;
    descriptor.default_collation_name = matched->default_collation_name;
    descriptor.family_epoch = matched->family_epoch;
    descriptor.family_version = matched->family_version;
    descriptor.min_bytes = matched->min_bytes;
    descriptor.max_bytes = matched->max_bytes;
    descriptor.variable_width = matched->variable_width;
  } else {
    const scratchbird::core::resources::ResourceSeedCollationDescriptor*
        matched = nullptr;
    for (const auto& collation : image.collations) {
      if (collation.resource_uuid == canonical_resource_uuid) {
        matched = &collation;
        break;
      }
    }
    if (matched == nullptr) {
      for (const auto& charset : image.charsets) {
        if (charset.resource_uuid == canonical_resource_uuid) {
          return fail("CATALOG.RESOURCE.FAMILY_MISMATCH",
                      "catalog.resource.family_mismatch",
                      "expected=collation;actual=charset");
        }
      }
      return fail("CATALOG.RESOURCE.UUID_NOT_FOUND",
                  "catalog.resource.uuid_not_found",
                  canonical_resource_uuid);
    }
    descriptor.canonical_name = matched->canonical_name;
    descriptor.resource_uuid.canonical = matched->resource_uuid;
    descriptor.parent_resource_uuid.canonical = matched->charset_uuid;
    descriptor.parent_canonical_name = matched->charset_name;
    descriptor.family_epoch = matched->family_epoch;
    descriptor.family_version = matched->family_version;
    descriptor.default_for_parent = matched->default_for_charset;
    descriptor.case_insensitive = matched->case_insensitive;
    descriptor.accent_insensitive = matched->accent_insensitive;
  }

  if (descriptor.resource_uuid.canonical.empty() ||
      descriptor.resource_epoch == 0 || descriptor.family_epoch == 0 ||
      descriptor.family_version.empty() ||
      (resource_family == "charset" &&
       (descriptor.min_bytes == 0 ||
        descriptor.max_bytes < descriptor.min_bytes)) ||
      (resource_family == "collation" &&
       descriptor.parent_resource_uuid.canonical.empty())) {
    return fail("CATALOG.RESOURCE.DESCRIPTOR_INVALID",
                "catalog.resource.descriptor_invalid",
                descriptor.canonical_name);
  }

  result.ok = true;
  result.diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.resource_descriptor = std::move(descriptor);
  return result;
}

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_NAME_RESOLUTION_API_BEHAVIOR
EngineResolveNameResult EngineResolveName(const EngineResolveNameRequest& request) {
  if (auto resource_result = ResolveEngineResourceName(request)) {
    return std::move(*resource_result);
  }
  EngineCatalogResolveObjectNameRequest catalog_request;
  static_cast<EngineApiRequest&>(catalog_request) = static_cast<const EngineApiRequest&>(request);
  const auto catalog_resolved = EngineCatalogResolveObjectName(catalog_request);
  if (catalog_resolved.ok) {
    if (ObjectClassCanBeTemporaryTable(catalog_resolved.primary_object.object_kind)) {
      const auto visibility = CheckMgaTemporaryTableVisibility(
          request.context,
          catalog_resolved.primary_object.uuid.canonical);
      if (!visibility.ok) {
        return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
            request.context,
            "catalog.resolve_name",
            visibility.diagnostic);
      }
      if (visibility.hidden_by_temporary_visibility ||
          (visibility.known_temporary && !visibility.visible_to_session)) {
        return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
            request.context,
            "catalog.resolve_name",
            MakeEngineApiDiagnostic("CATALOG.NAME.NOT_FOUND",
                                    "message_vector.item_not_found_or_does_not_exist",
                                    "item_not_found_or_does_not_exist"));
      }
    }
    auto result = MakeApiBehaviorSuccess<EngineResolveNameResult>(request.context, "catalog.resolve_name");
    result.primary_object = catalog_resolved.primary_object;
    result.bound_object_identity = catalog_resolved.bound_object_identity;
    result.result_shape = catalog_resolved.result_shape;
    result.evidence = catalog_resolved.evidence;
    AddApiBehaviorEvidence(&result, "catalog_object_lifecycle_resolver", "SBCATOBJ1");
    const auto semantic = AttachViewSemanticProjection(
        request,
        result.primary_object.object_kind,
        result.primary_object.uuid.canonical,
        &result);
    if (semantic.error) {
      return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
          request.context, "catalog.resolve_name", semantic);
    }
    return result;
  }
  if (!catalog_resolved.diagnostics.empty() &&
      catalog_resolved.diagnostics.front().code.rfind("CATALOG.SYNONYM_", 0) == 0) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
        request.context,
        "catalog.resolve_name",
        catalog_resolved.diagnostics.front());
  }
  const auto resolved = ResolveNameRegistryPublic(request, request.sql_object_reference.expected_object_type);
  if (!resolved.ok) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(request.context, "catalog.resolve_name", resolved.diagnostic);
  }
  const auto filtered = FilterTemporaryNameMatches(request, resolved.matches);
  if (!filtered.ok) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
        request.context,
        "catalog.resolve_name",
        filtered.diagnostic);
  }
  const bool use_temporary_matches = !filtered.temporary_matches.empty();
  const auto& effective_matches =
      use_temporary_matches ? filtered.temporary_matches : filtered.durable_matches;
  if (effective_matches.empty()) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
        request.context,
        "catalog.resolve_name",
        MakeEngineApiDiagnostic("CATALOG.NAME.NOT_FOUND",
                                "message_vector.item_not_found_or_does_not_exist",
                                "item_not_found_or_does_not_exist"));
  }
  if (effective_matches.size() > 1) {
    return MakeApiBehaviorDiagnostic<EngineResolveNameResult>(
        request.context,
        "catalog.resolve_name",
        MakeEngineApiDiagnostic("CATALOG.NAME.AMBIGUOUS", "catalog.name.ambiguous", "ambiguous_name"));
  }
  return MakeNameRegistryResolveResult(
      request,
      effective_matches.front(),
      use_temporary_matches);
}

EngineMapUuidToNameResult EngineMapUuidToName(const EngineMapUuidToNameRequest& request) {
  const auto mapped = MapNameRegistryUuidToNamePublic(request,
                                                     request.target_object.uuid.canonical,
                                                     request.target_object.object_kind);
  if (!mapped.ok) {
    return MakeApiBehaviorDiagnostic<EngineMapUuidToNameResult>(
        request.context,
        "catalog.map_uuid_to_name",
        mapped.diagnostic);
  }
  auto result = MakeApiBehaviorSuccess<EngineMapUuidToNameResult>(request.context, "catalog.map_uuid_to_name");
  result.primary_object.uuid.canonical = mapped.entry.object_uuid;
  result.primary_object.object_kind = mapped.entry.object_class;
  result.bound_object_identity.object_uuid.canonical = mapped.entry.object_uuid;
  result.bound_object_identity.resolved_object_type = mapped.entry.object_class;
  result.bound_object_identity.resolved_schema_uuid.canonical = mapped.entry.scope_uuid;
  result.bound_object_identity.parent_object_uuid.canonical = mapped.entry.parent_object_uuid;
  result.bound_object_identity.catalog_generation_id = mapped.entry.catalog_generation_id;
  result.bound_object_identity.security_epoch = request.context.security_epoch;
  result.bound_object_identity.resource_epoch = mapped.entry.resource_epoch;
  AddApiBehaviorRow(&result, {{"object_uuid", mapped.entry.object_uuid},
                              {"object_kind", mapped.entry.object_class},
                              {"name", mapped.entry.display_name},
                              {"raw_name_text", mapped.entry.raw_name_text},
                              {"scope_uuid", mapped.entry.scope_uuid},
                              {"identifier_profile_uuid", mapped.entry.identifier_profile_uuid},
                              {"language_tag", mapped.entry.language_tag},
                              {"name_class", mapped.entry.name_class},
                              {"was_quoted", mapped.entry.was_quoted ? "true" : "false"},
                              {"requires_exact_match", mapped.entry.requires_exact_match ? "true" : "false"}});
  AddApiBehaviorEvidence(&result, "uuid_to_name", mapped.entry.object_uuid);
  AddApiBehaviorEvidence(&result, "name_entry", mapped.entry.name_entry_uuid);
  return result;
}

}  // namespace scratchbird::engine::internal_api
