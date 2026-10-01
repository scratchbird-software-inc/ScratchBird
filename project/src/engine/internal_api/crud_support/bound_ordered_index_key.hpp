// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "crud_support/composite_logical_key.hpp"
#include "crud_support/crud_store.hpp"
#include "datatype_storage_identity.hpp"
#include "index_key_encoding.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sbl_numeric.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api::bound_index_key {
// Direct B-tree memberships share one bound comparison-key profile across
// unique/non-unique DDL, native/typed mutation and rebuild producers. Builder
// route selection and uniqueness enforcement remain separate decisions.
inline bool UsesBoundOrderedProfile(const CrudIndexRecord& index) {
  const auto family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  if (family != kCrudIndexFamilyBtree ||
      !index.predicate_kind.empty() || !index.include_columns.empty()) return false;
  for (const auto& key : index.key_envelopes)
    for (const auto prefix : {"lower:", "upper:", "length:", "identity:", "cast:", "desc:", "sum:"})
      if (key.starts_with(prefix)) return false;
  return true;
}

struct OrderedIndexColumn {
  core::datatypes::DatatypeStorageIdentityV1 datatype;
  core::datatypes::DatatypeTextSeedAuthority text_seed;
  core::platform::TypedUuid descriptor;
  core::platform::TypedUuid collation;
};

struct PublicationBindingStatistics {
  std::uint64_t resource_lookups = 0;
  std::uint64_t resource_reuses = 0;
};

// This scope cannot be constructed or retained by callers. Each public helper
// creates it on the stack for one fixed context and destroys it before return.
// No binding survives an append call, transaction, node or resource epoch.
class PublicationBindingScope {
  explicit PublicationBindingScope(const EngineRequestContext& context) : context_(context) {}
  PublicationBindingScope(const PublicationBindingScope&) = delete;
  PublicationBindingScope& operator=(const PublicationBindingScope&) = delete;

  friend bool BindOrderedIndexColumns(const EngineRequestContext&,
      const CrudIndexRecord&, const EngineUuid&, std::vector<OrderedIndexColumn>*,
      EngineApiDiagnostic*);
  friend bool CanonicalizePublicationBatch(const EngineRequestContext&,
      MgaExactIndexEntryAppendBatch*, EngineApiDiagnostic*);
  friend bool CanonicalizePublicationBatches(const EngineRequestContext&,
      std::vector<MgaExactIndexEntryAppendBatch>*, EngineApiDiagnostic*,
      PublicationBindingStatistics*);

  bool CanonicalizeBatch(MgaExactIndexEntryAppendBatch*, EngineApiDiagnostic*);
  bool BindColumns(
    const CrudIndexRecord& index, const EngineUuid& table,
    std::vector<OrderedIndexColumn>* output, EngineApiDiagnostic* diagnostic) {
  const auto& context = context_;
  const auto refuse = [&](std::string reason) {
    *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", std::move(reason));
    return false;
  };
  const auto loaded = LoadMgaRelationStorageDescriptor(context, table);
  if (!loaded.ok) { *diagnostic = loaded.diagnostic; return false; }
  if (loaded.descriptor.database_uuid != context.database_uuid ||
      loaded.descriptor.relation_uuid != table)
    return refuse("sorted_index_relation_identity_mismatch");
  auto names = CrudIndexKeyColumnNames(index);
  if (names.empty()) return refuse("sorted_index_key_columns_missing");
  std::vector<OrderedIndexColumn> staged;
  for (const auto& name : names) {
    const auto found = std::find_if(loaded.descriptor.columns.begin(), loaded.descriptor.columns.end(),
        [&](const auto& column) { return column.canonical_name_key == name; });
    if (found == loaded.descriptor.columns.end() || !found->column_generation ||
        !core::uuid::IsEngineIdentityUuid(found->column_uuid) ||
        !core::uuid::IsEngineIdentityUuid(found->value_descriptor.descriptor_uuid))
      return refuse("sorted_index_bound_column_missing");
    const auto& value = found->value_descriptor;
    OrderedIndexColumn column;
    if (!core::datatypes::LookupDatatypeStorageIdentityV1(
            context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
            context.datatype_registry_generation, value.datatype_descriptor_uuid,
            value.datatype_descriptor_generation, &column.datatype) ||
        column.datatype.type_uuid != value.type_uuid)
      return refuse("sorted_index_datatype_authority_mismatch");
    const auto descriptor = core::uuid::MakeTypedUuid(core::platform::UuidKind::object,
                                                     column.datatype.descriptor_uuid);
    if (!descriptor.ok()) return refuse("sorted_index_datatype_identity_invalid");
    column.descriptor = descriptor.value;
    if (column.datatype.type_id == core::datatypes::CanonicalTypeId::character) {
      EngineResourceDescriptorLookupResult resource;
      const auto cached = collations_.find(found->collation_uuid);
      if (cached != collations_.end()) {
        ++statistics_.resource_reuses;
        resource = cached->second;
      } else {
        ++statistics_.resource_lookups;
        resource = LookupEngineResourceDescriptorByUuid(context, found->collation_uuid, "collation");
        if (resource.ok) collations_.emplace(found->collation_uuid, resource);
      }
      if (!resource.ok) { *diagnostic = resource.diagnostic; return false; }
      if (!resource.resource_descriptor.present ||
          resource.resource_descriptor.database_uuid != context.database_uuid ||
          resource.resource_descriptor.resource_uuid != found->collation_uuid ||
          resource.resource_descriptor.parent_resource_uuid != found->charset_uuid ||
          resource.resource_descriptor.resource_epoch != context.resource_epoch ||
          resource.resource_descriptor.family_epoch == 0)
        return refuse("sorted_index_collation_authority_mismatch");
      column.text_seed = TextSeedFromResource(resource.resource_descriptor);
      const auto collation = core::uuid::MakeTypedUuid(core::platform::UuidKind::object,
                                                      found->collation_uuid);
      if (!collation.ok()) return refuse("sorted_index_collation_identity_invalid");
      column.collation = collation.value;
    }
    staged.push_back(std::move(column));
  }
  *output = std::move(staged);
  return true;
}

  const EngineRequestContext& context_;
  std::map<EngineUuid, EngineResourceDescriptorLookupResult> collations_;
  PublicationBindingStatistics statistics_;
};

inline bool BindOrderedIndexColumns(const EngineRequestContext& context,
    const CrudIndexRecord& index, const EngineUuid& table,
    std::vector<OrderedIndexColumn>* output, EngineApiDiagnostic* diagnostic) {
  PublicationBindingScope scope(context);
  return scope.BindColumns(index, table, output, diagnostic);
}

inline bool EncodeOrderedIndexKey(std::string_view logical_key,
    const std::vector<OrderedIndexColumn>& columns, std::string* output,
    bool* null_key, EngineApiDiagnostic* diagnostic) {
  const auto values = DecodeStoredLogicalKey(logical_key, columns.size());
  if (!values) {
    *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_logical_key_invalid");
    return false;
  }
  std::vector<core::index::IndexKeyEncodingComponent> components;
  bool contains_null = false;
  for (std::size_t ordinal = 0; ordinal < columns.size(); ++ordinal) {
    const auto& binding = columns[ordinal];
    const auto& value = (*values)[ordinal];
    if (binding.datatype.type_id ==
        core::datatypes::CanonicalTypeId::uint16) {
      *diagnostic = MakeInvalidRequestDiagnostic(
          "mga.index_store", "sorted_index_uint16_carrier_provenance_unbound");
      return false;
    }
    core::datatypes::DatatypeSortKeyRequest request;
    request.value = {binding.datatype.type_id, value.bytes, value.isSqlNull()};
    request.text_seed = binding.text_seed;
    if (binding.datatype.type_id == core::datatypes::CanonicalTypeId::decimal && !value.isSqlNull()) {
      // Decimal retained values carry the canonical 24-byte coefficient/scale
      // codec. Numeric decoding is explicit; never guess from byte contents
      // whether a storage value might instead be a decimal spelling.
      const auto decoded = libraries::sbl_numeric::DecodeExactDecimalLittleEndian(
          reinterpret_cast<const std::uint8_t*>(value.bytes.data()), value.bytes.size());
      if (!decoded.ok) {
        *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_decimal_value_invalid");
        return false;
      }
      request.value.encoded_value = decoded.canonical_lexical;
    }
    std::string sort_key;
    if (binding.datatype.type_id == core::datatypes::CanonicalTypeId::decimal_float) {
      if (!binding.datatype.codec ||
          !core::datatypes::IsExactCanonicalDecimal128TypeCodecIdentityV1(*binding.datatype.codec)) {
        *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_decimal128_policy_unbound");
        return false;
      }
      if (!value.isSqlNull()) {
        const auto sorted = libraries::sbl_numeric::MakeDecimal128OrderKey(
            reinterpret_cast<const std::uint8_t*>(value.bytes.data()), value.bytes.size(),
            libraries::sbl_numeric::Decimal128OrderProfile::numeric_total_nan_last,
            binding.datatype.codec->allow_special_values);
        if (!sorted.key) {
          *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_decimal128_value_invalid");
          return false;
        }
        sort_key.assign(1, '\1');
        sort_key.append(reinterpret_cast<const char*>(sorted.key->data()), sorted.key->size());
      }
    } else if (binding.datatype.type_id == core::datatypes::CanonicalTypeId::real128 && !value.isSqlNull()) {
      // Retained real128 owns canonical binary128 bytes. The operation-value
      // API above is lexical for numerics; it cannot reinterpret this carrier.
      // This index profile uses the numeric backend's explicit IEEE total order.
      libraries::sbl_numeric::NumericContext numeric_context;
      numeric_context.allow_special_values = true;
      const auto sorted = libraries::sbl_numeric::MakeReal128TotalOrderKey(
          reinterpret_cast<const std::uint8_t*>(value.bytes.data()), value.bytes.size(), numeric_context);
      if (!sorted.key) {
        *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_binary128_value_invalid");
        return false;
      }
      sort_key.assign(1, '\1');
      sort_key.append(reinterpret_cast<const char*>(sorted.key->data()), sorted.key->size());
    } else {
      const auto sorted = core::datatypes::MakeDatatypeSortKey(request);
      if (!sorted.ok()) {
        *diagnostic = MakeEngineApiDiagnosticFromNative(sorted.diagnostic,
            "SB_DATATYPE_SORT_KEY_REJECTED", "datatype.sort_key.rejected",
            std::string("sorted_index_bound_value_refused:") + core::datatypes::CanonicalTypeName(binding.datatype.type_id), true);
        return false;
      }
      sort_key = sorted.sort_key;
    }
    core::index::IndexKeyEncodingComponent component;
    component.ordinal = ordinal;
    component.kind = binding.datatype.type_id == core::datatypes::CanonicalTypeId::character
        ? core::index::IndexKeyComponentKind::collation_key : core::index::IndexKeyComponentKind::scalar;
    component.type_descriptor_uuid = binding.descriptor;
    component.type_descriptor_epoch = binding.datatype.descriptor_generation;
    component.collation_uuid = binding.collation;
    component.null_placement = core::index::IndexKeyNullPlacement::nulls_first;
    component.is_null = value.isSqlNull();
    contains_null = contains_null || component.is_null;
    if (!component.is_null) component.payload.assign(sort_key.begin(), sort_key.end());
    components.push_back(std::move(component));
  }
  const auto encoded = core::index::EncodeIndexKey(components, {});
  if (!encoded.ok()) {
    *diagnostic = MakeEngineApiDiagnosticFromNative(encoded.diagnostic,
        "SB_ENGINE_API_INVALID_REQUEST", "engine.api.invalid_request",
        "sorted_index_physical_key_refused", true);
    return false;
  }
  output->assign(encoded.encoded.begin(), encoded.encoded.end());
  *null_key = contains_null;
  return true;
}

inline bool PublicationBindingScope::CanonicalizeBatch(
    MgaExactIndexEntryAppendBatch* batch, EngineApiDiagnostic* diagnostic) {
  if (batch->entries.empty() || !UsesBoundOrderedProfile(batch->index)) return true;
  const auto table = batch->index.table_uuid.is_nil() ? batch->table_uuid : batch->index.table_uuid;
  if (!batch->table_uuid.is_nil() && batch->table_uuid != table) {
    *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "ordered_index_table_identity_mismatch");
    return false;
  }
  std::vector<OrderedIndexColumn> columns;
  if (!BindColumns(batch->index, table, &columns, diagnostic)) return false;
  for (auto& entry : batch->entries) {
    // Physical keys are derived, never logical value authority. Native bulk
    // and DDL carry the same lossless logical tuple in the payload field.
    const auto logical = entry.encoded_key.starts_with("SBKOBIN:")
        ? entry.payload_value : entry.encoded_key;
    std::string physical;
    bool null_key = false;
    if (!EncodeOrderedIndexKey(logical, columns, &physical, &null_key, diagnostic)) return false;
    entry.encoded_key = "SBKOBIN:" + physical;
    entry.payload_value = logical;
  }
  return true;
}

inline bool CanonicalizePublicationBatch(const EngineRequestContext& context,
    MgaExactIndexEntryAppendBatch* batch, EngineApiDiagnostic* diagnostic) {
  PublicationBindingScope scope(context);
  auto staged = *batch;
  if (!scope.CanonicalizeBatch(&staged, diagnostic)) return false;
  *batch = std::move(staged);
  return true;
}

inline bool CanonicalizePublicationBatches(const EngineRequestContext& context,
    std::vector<MgaExactIndexEntryAppendBatch>* batches, EngineApiDiagnostic* diagnostic,
    PublicationBindingStatistics* statistics = nullptr) {
  PublicationBindingScope scope(context);
  // Preserve input as well as durable state if any later batch is invalid.
  auto staged = *batches;
  for (auto& batch : staged) {
    if (!scope.CanonicalizeBatch(&batch, diagnostic)) {
      if (statistics) *statistics = scope.statistics_;
      return false;
    }
  }
  *batches = std::move(staged);
  if (statistics) *statistics = scope.statistics_;
  return true;
}
}  // namespace scratchbird::engine::internal_api::bound_index_key
