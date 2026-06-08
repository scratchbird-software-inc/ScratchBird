// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/descriptor_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/pinned_descriptor_cache.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <cctype>
#include <optional>
#include <sstream>
#include <utility>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

bool IsBareIdentifier(std::string_view value) {
  if (value.empty()) return false;
  const auto first = static_cast<unsigned char>(value.front());
  if (!std::isalpha(first) && value.front() != '_') return false;
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    if (!std::isalnum(c) && ch != '_') return false;
  }
  return true;
}

std::string RenderIdentifier(std::string_view value) {
  if (IsBareIdentifier(value)) return std::string(value);
  std::string rendered = "\"";
  for (const char ch : value) {
    if (ch == '"') rendered.push_back('"');
    rendered.push_back(ch);
  }
  rendered.push_back('"');
  return rendered;
}

std::string RenderCreateTableStatement(const CrudTableRecord& table) {
  std::ostringstream out;
  out << "CREATE TABLE " << RenderIdentifier(table.default_name.empty()
                                                 ? std::string_view("unnamed_table")
                                                 : std::string_view(table.default_name))
      << " (";
  for (std::size_t index = 0; index < table.columns.size(); ++index) {
    if (index != 0) out << ", ";
    const auto& column = table.columns[index];
    out << RenderIdentifier(column.first) << ' '
        << (column.second.empty() ? "text" : column.second);
  }
  out << ')';
  return out.str();
}

std::string OptionValue(const EngineApiRequest& request, std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) {
      return option.substr(prefix.size());
    }
  }
  return {};
}

bool DescriptorMetadataCacheEnabled(const EngineApiRequest& request) {
  return OptionValue(request, "descriptor_metadata_cache:") == "enabled" ||
         OptionValue(request, "descriptor_cache:") == "enabled";
}

bool DescriptorMetadataCacheDisabled(const EngineApiRequest& request) {
  return OptionValue(request, "descriptor_metadata_cache:") == "disabled" ||
         OptionValue(request, "descriptor_cache:") == "disabled";
}

std::uint64_t OptionU64(const EngineApiRequest& request, std::string_view prefix) {
  const std::string value = OptionValue(request, prefix);
  if (value.empty()) return 0;
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

std::vector<std::string> ObjectUuidsForPinnedDescriptorKey(const EngineGetDescriptorRequest& request,
                                                           const std::string& descriptor_uuid) {
  std::vector<std::string> uuids;
  if (!descriptor_uuid.empty()) uuids.push_back(descriptor_uuid);
  if (!request.target_object.uuid.canonical.empty()) uuids.push_back(request.target_object.uuid.canonical);
  if (!request.bound_object_identity.object_uuid.canonical.empty()) {
    uuids.push_back(request.bound_object_identity.object_uuid.canonical);
  }
  for (const auto& object : request.related_objects) {
    if (!object.uuid.canonical.empty()) uuids.push_back(object.uuid.canonical);
  }
  return uuids;
}

std::vector<std::string> IndexUuidsForPinnedDescriptorKey(const EngineGetDescriptorRequest& request) {
  std::vector<std::string> uuids;
  for (const auto& index : request.indexes) {
    if (!index.requested_index_uuid.canonical.empty()) {
      uuids.push_back(index.requested_index_uuid.canonical);
    }
  }
  return uuids;
}

CatalogPinnedDescriptorCacheKey DescriptorCacheKey(const EngineGetDescriptorRequest& request,
                                                   const std::string& descriptor_uuid) {
  CatalogPinnedDescriptorCacheKey key;
  key.descriptor_family = OptionValue(request, "descriptor_family:");
  if (key.descriptor_family.empty()) key.descriptor_family = "catalog_descriptor";
  key.catalog_epoch = request.context.catalog_generation_id;
  key.security_epoch = request.context.security_epoch;
  key.resource_policy_epoch = request.context.resource_epoch;
  key.name_resolution_epoch = request.context.name_resolution_epoch;
  key.stats_epoch = OptionU64(request, "stats_epoch:");
  key.stats_epoch_relevant = key.descriptor_family == "statistics_descriptor" || key.stats_epoch != 0;
  key.descriptor_set_digest = OptionValue(request, "descriptor_set_digest:");
  if (key.descriptor_set_digest.empty()) {
    key.descriptor_set_digest = CatalogPinnedDescriptorSetDigest(request.descriptors, request.columns);
  }
  key.object_uuids = ObjectUuidsForPinnedDescriptorKey(request, descriptor_uuid);
  key.index_uuids = IndexUuidsForPinnedDescriptorKey(request);
  key.security_policy_identity = OptionValue(request, "security_policy_identity:");
  if (key.security_policy_identity.empty()) {
    key.security_policy_identity = request.context.principal_uuid.canonical.empty()
                                       ? "security_policy:default"
                                       : "principal:" + request.context.principal_uuid.canonical;
  }
  key.redaction_policy_identity = OptionValue(request, "redaction_policy_identity:");
  if (key.redaction_policy_identity.empty()) key.redaction_policy_identity = "redaction_policy:default";
  key.resource_policy_identity = OptionValue(request, "resource_policy_identity:");
  if (key.resource_policy_identity.empty()) {
    key.resource_policy_identity = "resource_epoch:" + std::to_string(request.context.resource_epoch);
  }
  return key;
}

void AddShowCreateTableEvidence(EngineGetDescriptorResult* result,
                                const CrudTableRecord& table) {
  if (result == nullptr) return;
  AddApiBehaviorEvidence(result, "show_create_statement", table.table_uuid);
  AddApiBehaviorEvidence(result, "show_create_object_kind", "table");
  AddApiBehaviorRow(result,
                    {{"object_uuid", table.table_uuid},
                     {"object_kind", "table"},
                     {"object_name", table.default_name},
                     {"create_statement", RenderCreateTableStatement(table)}});
  result->result_shape.result_kind = "descriptor";
}

EngineGetDescriptorResult EngineGetDescriptorUncachedImpl(const EngineGetDescriptorRequest& request);

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_DESCRIPTOR_API_BEHAVIOR
namespace {

EngineGetDescriptorResult EngineGetDescriptorUncachedImpl(const EngineGetDescriptorRequest& request) {
  const std::string descriptor_uuid = !request.target_object.uuid.canonical.empty()
                                          ? request.target_object.uuid.canonical
                                          : (!request.descriptors.empty() ? request.descriptors.front().descriptor_uuid.canonical : std::string{});
  if (descriptor_uuid.empty()) {
    return MakeCrudDiagnosticResult<EngineGetDescriptorResult>(
        request.context,
        "catalog.get_descriptor",
        MakeInvalidRequestDiagnostic("catalog.get_descriptor", "descriptor_uuid_required"));
  }
  const auto observer_tx = request.context.local_transaction_id;
  const auto catalog_objects = LoadCatalogObjectLifecycleState(request.context);
  if (catalog_objects.ok) {
    for (const auto& object : catalog_objects.state.objects) {
      if (object.object_uuid != descriptor_uuid) { continue; }
      auto result = MakeCrudSuccessResult<EngineGetDescriptorResult>(request.context, "catalog.get_descriptor");
      result.primary_object.uuid.canonical = object.object_uuid;
      result.primary_object.object_kind = object.object_kind;
      result.descriptor_owner = result.primary_object;
      result.descriptor.descriptor_uuid.canonical = object.object_uuid;
      result.descriptor.descriptor_kind = object.object_kind == "synonym" ? "sys.catalog.synonym" : object.object_kind;
      result.descriptor.canonical_type_name = object.object_kind;
      result.descriptor.encoded_descriptor = "object_uuid=" + object.object_uuid + ";object_kind=" +
                                            object.object_kind + ";schema_uuid=" + object.schema_uuid +
                                            ";state=" + object.lifecycle_state + ";payload=" + object.payload;
      result.result_shape.result_kind = "descriptor";
      result.result_shape.columns.push_back(result.descriptor);
      result.evidence.push_back({"catalog_object_descriptor_lookup", object.object_uuid});
      if (object.object_kind == "synonym") {
        result.evidence.push_back({"catalog_table", "sys.catalog.synonym"});
      }
      return result;
    }
  }
  if (const auto domain = FindVisibleDomain(request.context, descriptor_uuid, observer_tx)) {
    auto result = MakeCrudSuccessResult<EngineGetDescriptorResult>(request.context, "catalog.get_descriptor");
    result.primary_object.uuid.canonical = domain->domain_uuid;
    result.primary_object.object_kind = "domain";
    result.descriptor_owner = result.primary_object;
    result.descriptor = DomainDescriptor(*domain);
    result.result_shape.result_kind = "descriptor";
    result.result_shape.columns.push_back(result.descriptor);
    result.evidence.push_back({"domain_descriptor_lookup", domain->domain_uuid});
    return result;
  }
  const auto crud = LoadCrudState(request.context);
  if (crud.ok) {
    if (const auto table = FindVisibleCrudTable(crud.state, descriptor_uuid, observer_tx)) {
      auto result = MakeCrudSuccessResult<EngineGetDescriptorResult>(request.context, "catalog.get_descriptor");
      result.primary_object.uuid.canonical = table->table_uuid;
      result.primary_object.object_kind = "table";
      result.descriptor_owner = result.primary_object;
      result.descriptor.descriptor_uuid.canonical = table->table_uuid;
      result.descriptor.descriptor_kind = "table";
      result.descriptor.canonical_type_name = table->default_name;
      result.descriptor.encoded_descriptor = "table_uuid=" + table->table_uuid + ";columns=" + EncodeCrudPairs(table->columns);
      result.result_shape.result_kind = "descriptor";
      result.result_shape.columns.push_back(result.descriptor);
      result.evidence.push_back({"table_descriptor_lookup", table->table_uuid});
      AddShowCreateTableEvidence(&result, *table);
      return result;
    }
  }
  const auto mga_relations = LoadMgaRelationStoreState(request.context);
  if (mga_relations.ok) {
    const CrudState mga_crud = BuildCrudCompatibilityStateFromMga(mga_relations.state);
    if (const auto table = FindVisibleCrudTable(mga_crud, descriptor_uuid, observer_tx)) {
      auto result = MakeCrudSuccessResult<EngineGetDescriptorResult>(request.context, "catalog.get_descriptor");
      result.primary_object.uuid.canonical = table->table_uuid;
      result.primary_object.object_kind = "table";
      result.descriptor_owner = result.primary_object;
      result.descriptor.descriptor_uuid.canonical = table->table_uuid;
      result.descriptor.descriptor_kind = "table";
      result.descriptor.canonical_type_name = table->default_name;
      result.descriptor.encoded_descriptor = "table_uuid=" + table->table_uuid + ";columns=" + EncodeCrudPairs(table->columns);
      result.result_shape.result_kind = "descriptor";
      result.result_shape.columns.push_back(result.descriptor);
      result.evidence.push_back({"table_descriptor_lookup", table->table_uuid});
      AddShowCreateTableEvidence(&result, *table);
      return result;
    }
  }
  if (const auto record = FindVisibleApiBehaviorRecord(request.context, descriptor_uuid, observer_tx)) {
    auto result = MakeCrudSuccessResult<EngineGetDescriptorResult>(request.context, "catalog.get_descriptor");
    result.primary_object.uuid.canonical = record->object_uuid;
    result.primary_object.object_kind = record->object_kind;
    result.descriptor_owner = result.primary_object;
    result.descriptor = ApiBehaviorDescriptor(*record);
    result.result_shape.result_kind = "descriptor";
    result.result_shape.columns.push_back(result.descriptor);
    result.evidence.push_back({"api_behavior_descriptor_lookup", record->object_uuid});
    return result;
  }
  return MakeCrudDiagnosticResult<EngineGetDescriptorResult>(
      request.context,
      "catalog.get_descriptor",
      MakeInvalidRequestDiagnostic("catalog.get_descriptor", "descriptor_not_visible"));
}

}  // namespace

EngineGetDescriptorResult EngineGetDescriptor(const EngineGetDescriptorRequest& request) {
  const std::string descriptor_uuid = !request.target_object.uuid.canonical.empty()
                                          ? request.target_object.uuid.canonical
                                          : (!request.descriptors.empty() ? request.descriptors.front().descriptor_uuid.canonical : std::string{});
  if (descriptor_uuid.empty()) {
    return EngineGetDescriptorUncachedImpl(request);
  }

  const bool enabled = DescriptorMetadataCacheEnabled(request);
  if (!enabled || DescriptorMetadataCacheDisabled(request)) {
    auto result = EngineGetDescriptorUncachedImpl(request);
    if (DescriptorMetadataCacheDisabled(request)) {
      AddApiBehaviorEvidence(&result, "descriptor_metadata_cache", "disabled");
      AddApiBehaviorEvidence(&result, "descriptor_lookup_fallback", "uncached_uuid_descriptor_lookup");
    }
    return result;
  }

  const CatalogPinnedDescriptorCacheKey cache_key = DescriptorCacheKey(request, descriptor_uuid);
  auto cached = GlobalCatalogPinnedDescriptorCache().Lookup(cache_key);
  if (cached.ok && cached.snapshot) {
    EngineGetDescriptorResult result;
    result.ok = true;
    result.operation_id = "catalog.get_descriptor";
    result.transaction_uuid = request.context.transaction_uuid;
    result.local_transaction_id = request.context.local_transaction_id;
    result.embedded_trust_mode_observed =
        request.context.trust_mode == EngineTrustMode::embedded_in_process;
    result.descriptor = cached.snapshot->descriptor;
    result.descriptor_owner = cached.snapshot->descriptor_owner;
    result.primary_object = cached.snapshot->primary_object;
    result.result_shape = cached.snapshot->result_shape;
    AddApiBehaviorEvidence(&result, "descriptor_metadata_cache", "hit");
    AddApiBehaviorEvidence(&result, "descriptor_metadata_cache_key", cached.cache_key);
    AddApiBehaviorEvidence(&result, "descriptor_metadata_cache_snapshot", "read_only");
    AddApiBehaviorEvidence(&result, "mga_visibility_recheck", "preserved");
    AddApiBehaviorEvidence(&result, "security_authorization_recheck", "preserved");
    AddApiBehaviorEvidence(&result, "descriptor_uuid_resolution", descriptor_uuid);
    return result;
  }
  if (!cached.diagnostic_code.empty() &&
      cached.diagnostic_code != "SB_CATALOG_PINNED_DESCRIPTOR_CACHE_MISS") {
    auto result = MakeCrudDiagnosticResult<EngineGetDescriptorResult>(
        request.context,
        "catalog.get_descriptor",
        MakeEngineApiDiagnostic(cached.diagnostic_code,
                                "catalog.pinned_descriptor_cache",
                                cached.detail));
    AddApiBehaviorEvidence(&result, "descriptor_metadata_cache", "refused");
    AddApiBehaviorEvidence(&result, "descriptor_metadata_cache_key", cached.cache_key);
    AddApiBehaviorEvidence(&result, "descriptor_metadata_cache_diagnostic", cached.diagnostic_code);
    return result;
  }

  auto result = EngineGetDescriptorUncachedImpl(request);
  AddApiBehaviorEvidence(&result, "descriptor_metadata_cache", result.ok ? "miss" : "miss_not_cached");
  AddApiBehaviorEvidence(&result, "descriptor_metadata_cache_key", cached.cache_key);
  if (!cached.diagnostic_code.empty()) {
    AddApiBehaviorEvidence(&result, "descriptor_metadata_cache_diagnostic", cached.diagnostic_code);
  }
  AddApiBehaviorEvidence(&result, "descriptor_uuid_resolution", descriptor_uuid);
  if (result.ok) {
    CatalogPinnedDescriptorSnapshot snapshot;
    snapshot.key = cache_key;
    snapshot.descriptor = result.descriptor;
    snapshot.descriptors = result.result_shape.columns;
    snapshot.descriptor_owner = result.descriptor_owner;
    snapshot.primary_object = result.primary_object;
    snapshot.result_shape = result.result_shape;
    snapshot.evidence = {
        "descriptor_metadata_cache_snapshot=read_only",
        "mga_visibility_recheck=preserved",
        "security_authorization_recheck=preserved",
    };
    const auto put = GlobalCatalogPinnedDescriptorCache().Put(std::move(snapshot));
    AddApiBehaviorEvidence(&result, "descriptor_metadata_cache_put", put.diagnostic_code);
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
