// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_diagnostics.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "crud_support/composite_logical_key.hpp"
#include "crud_support/crud_store.hpp"
#include "datatype_storage_identity.hpp"
#include "datatype_date.hpp"
#include "engine/executor/descriptor_value_runtime.hpp"
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
  core::datatypes::DatatypeStorageIdentityV3 datatype;
  core::datatypes::DatatypeTextSeedAuthority text_seed;
  core::platform::TypedUuid descriptor;
  core::platform::TypedUuid collation;
  engine::ExecutionTypeDescriptor execution_descriptor;
  std::optional<core::datatypes::DateValidatedProfileHandleV3> date_profile;
};

// A datatype profile is representation authority only. Production callers
// obtain the containing column through the fresh publication scope below.
inline bool BindOrderedDateProfile(OrderedIndexColumn* column) {
  if (!column || column->datatype.type_id != core::datatypes::CanonicalTypeId::date ||
      !column->datatype.codec) return false;
  const auto& row = *column->datatype.codec;
  if (!core::datatypes::ValidateDateExecutionDescriptorV3(
          column->execution_descriptor, row).ok()) return false;
  const auto& identity = row.legacy_fields;
  auto profile = core::datatypes::BuildDateValidatedProfileHandleV3(
      {identity.catalog_snapshot_uuid, identity.catalog_snapshot_uuid,
       identity.catalog_generation, identity.registry_generation}, row);
  if (!profile.ok()) return false;
  column->date_profile = std::move(profile.profile);
  return true;
}

struct PublicationBindingStatistics {
  std::uint64_t resource_lookups = 0;
  std::uint64_t resource_reuses = 0;
};

// Pure projection of a supplied column, not catalog admission. Effectful
// callers must first load it through their fresh PublicationBindingScope.
inline bool BuildOrderedColumnExecutionDescriptor(
    const EngineDescriptor& source, const core::datatypes::DatatypeStorageIdentityV3& datatype,
    bool nullable, engine::ExecutionTypeDescriptor* output, std::string* detail) {
  if (!output || !detail) return false;
  const auto type = datatype.type_id;
  CatalogColumnMetadata fields;
  if (!AdmitCatalogColumnMetadata(source.encoded_descriptor, &fields) ||
      source.datatype_descriptor_uuid != datatype.descriptor_uuid ||
      source.datatype_descriptor_generation != datatype.descriptor_generation ||
      source.type_uuid != datatype.type_uuid) {
    *detail = "index column datatype authorities disagree";
    return false;
  }
  const auto agrees = [&](const char* name, const char* expected) {
    const auto found = fields.text.find(name);
    return found == fields.text.end() || found->second == expected;
  };
  if (!agrees("nullable", nullable ? "true" : "false") ||
      !agrees("nullability", nullable ? "nullable" : "non_null") ||
      !agrees("not_null", nullable ? "false" : "true") ||
      (fields.text.contains("nullable") && fields.text.contains("nullability"))) {
    *detail = "index column nullability authorities disagree";
    return false;
  }
  if (type == core::datatypes::CanonicalTypeId::decimal &&
      (fields.text.contains("precision") != fields.text.contains("scale") ||
       (fields.text.contains("precision") && fields.text.at("precision") == "0"))) {
    *detail = "decimal occurrence requires a complete nonzero precision/scale binding";
    return false;
  }
  if (type == core::datatypes::CanonicalTypeId::int64 ||
      type == core::datatypes::CanonicalTypeId::uuid ||
      type == core::datatypes::CanonicalTypeId::date ||
      type == core::datatypes::CanonicalTypeId::decimal) {
    if (!source.charset_uuid.is_nil() ||
        !source.collation_uuid.is_nil()) {
      *detail = "base index column has unsupported resource or domain metadata";
      return false;
    }
    for (const auto& [name, identity] : fields.identities) {
      if (type == core::datatypes::CanonicalTypeId::decimal &&
          (name == "decimal_codec_uuid" || name == "codec_uuid")) continue;
      if ((name == "type_uuid" && identity == datatype.type_uuid) ||
          (name == "datatype_descriptor_uuid" && identity == datatype.descriptor_uuid) ||
          (name == "codec_uuid" && datatype.codec && identity == datatype.codec->legacy_fields.codec_uuid)) continue;
      *detail = "base index column identity metadata disagrees with its binding";
      return false;
    }
    for (const auto& [name, value] : fields.text) {
      // These are redundant declaration labels, not datatype identities. A
      // compiler may emit any of the three keys, but it may not contradict
      // the supplied column's label. The UUID/generation checks above and
      // the exact registry lookup below remain the datatype authority.
      if (name == "canonical" || name == "canonical_type" || name == "type") {
        if (value != source.canonical_type_name) {
          *detail = "index column datatype declaration labels disagree";
          return false;
        }
        continue;
      }
      if (type == core::datatypes::CanonicalTypeId::decimal &&
          (name == "precision" || name == "scale" || name == "decimal_codec_generation" ||
           name == "codec_generation" || name == "codec_version" || name == "codec_id")) continue;
      if (datatype.codec) {
        const auto& codec = datatype.codec->legacy_fields;
        if ((name == "datatype_descriptor_generation" && value == std::to_string(codec.descriptor_generation)) ||
            (name == "type_generation" && value == std::to_string(codec.type_generation)) ||
            (name == "codec_id" && value == codec.codec_id) ||
            (name == "codec_version" && value == std::to_string(codec.codec_version)) ||
            (name == "codec_generation" && value == std::to_string(codec.codec_generation)) ||
            (name == "null_encoding" && value == std::to_string(codec.null_encoding_code))) continue;
      }
      // Catalog declaration attributes do not change the base comparison
      // profile. Semantic modifiers must not be dropped by the projection.
      if (name != "nullable" &&
          name != "nullability" && name != "not_null" && name != "primary_key" && name != "pk" &&
          name != "unique" && name != "generated" && name != "identity" &&
          name != "default" && name != "default_value") {
        *detail = "base index column has an unsupported modifier";
        return false;
      }
    }
  }
  // The typed storage column owns nullability. Legacy metadata can omit its
  // duplicate spelling, but any supplied spelling must agree with that owner.
  auto projection = source;
  if (!fields.text.contains("nullable") && !fields.text.contains("nullability"))
    fields.text["nullable"] = nullable ? "true" : "false";
  if (!EncodeCatalogColumnMetadata(fields, &projection.encoded_descriptor)) {
    *detail = "index column metadata projection failed";
    return false;
  }
  engine::ExecutionTypeDescriptor staged;
  if (!executor::BuildBoundExecutionTypeDescriptor(projection, type, &staged, detail)) return false;
  if (staged.nullable_allowed != nullable) {
    *detail = "index column nullability authorities disagree";
    return false;
  }
  if (type == core::datatypes::CanonicalTypeId::decimal) {
    core::datatypes::DatatypeSortKeyRequest expected;
    if (!core::datatypes::BindExactDecimalSortKeyProfile(staged, &expected)) {
      *detail = "decimal occurrence profile is invalid";
      return false;
    }
    const bool wide = staged.precision > 38;
    const bool explicit_codec = fields.identities.contains("decimal_codec_uuid") ||
                                fields.identities.contains("codec_uuid");
    if (wide && !explicit_codec) {
      *detail = "wide decimal occurrence requires an explicit binary codec identity";
      return false;
    }
    for (const auto name : {"decimal_codec_uuid", "codec_uuid"}) {
      const auto found = fields.identities.find(name);
      const char* generation = std::string_view(name) == "codec_uuid"
          ? "codec_generation" : "decimal_codec_generation";
      if ((found != fields.identities.end() &&
           (found->second != expected.decimal_codec_uuid ||
            !fields.text.contains(generation) || fields.text.at(generation) != "1")) ||
          (found == fields.identities.end() && fields.text.contains(generation))) {
        *detail = "decimal occurrence codec identity or generation disagrees";
        return false;
      }
    }
    if (!agrees("codec_version", "1") ||
        !agrees("codec_id", wide ? "datatype.decimal.base1e9.le40.v1" : "datatype.decimal.base1e9.le.v1")) {
      *detail = "decimal occurrence codec format disagrees";
      return false;
    }
  }
  *output = std::move(staged);
  return true;
}

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
    if (!core::datatypes::LookupDatatypeStorageIdentityV3(
            context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
            context.datatype_registry_generation, value.datatype_descriptor_uuid,
            value.datatype_descriptor_generation, &column.datatype) ||
        column.datatype.type_uuid != value.type_uuid)
      return refuse("sorted_index_datatype_authority_mismatch");
    const auto descriptor = core::uuid::MakeTypedUuid(core::platform::UuidKind::object,
                                                     column.datatype.descriptor_uuid);
    if (!descriptor.ok()) return refuse("sorted_index_datatype_identity_invalid");
    column.descriptor = descriptor.value;
    std::string descriptor_detail;
    if (!BuildOrderedColumnExecutionDescriptor(value, column.datatype, found->nullable,
            &column.execution_descriptor, &descriptor_detail)) {
      return refuse("sorted_index_execution_descriptor_invalid:" + descriptor_detail);
    }
    if (column.datatype.type_id == core::datatypes::CanonicalTypeId::date &&
        !BindOrderedDateProfile(&column))
      return refuse("sorted_index_date_profile_unbound");
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
    // Storage-only identity cannot authorize semantic comparison, including
    // a NULL key. Absence is not a fallback to an execution descriptor.
    if (!binding.datatype.codec) {
      *diagnostic = MakeInvalidRequestDiagnostic(
          "mga.index_store", "sorted_index_codec_provenance_unbound");
      return false;
    }
    if (binding.datatype.codec) {
      // Retain and check the full policy-bearing row. Comparing only the
      // legacy projection would silently discard V3 policy and width facts.
      // This is representation validation, not permission to publish: the
      // effectful path still obtains a fresh PublicationBindingScope.
      const auto& codec = *binding.datatype.codec;
      const auto& legacy = codec.legacy_fields;
      if (!core::datatypes::IsExactRegisteredDatatypeTypeCodecIdentityV3(codec) ||
          legacy.descriptor_uuid != binding.datatype.descriptor_uuid ||
          legacy.descriptor_generation != binding.datatype.descriptor_generation ||
          legacy.type_uuid != binding.datatype.type_uuid ||
          legacy.canonical_binary_type_code != static_cast<std::uint32_t>(binding.datatype.type_id)) {
        *diagnostic = MakeInvalidRequestDiagnostic(
            "mga.index_store", "sorted_index_codec_provenance_unbound");
        return false;
      }
    }
    if (binding.datatype.type_id ==
        core::datatypes::CanonicalTypeId::uint16) {
      // This pure encoding seam is not publication authority. Production
      // callers rebind through PublicationBindingScope on every operation.
      // Admit native LE2 only with the complete exact retained codec row,
      // never infer its representation from digit-like bytes or a name.
      const auto& codec = binding.datatype.codec;
      if (!codec || codec->legacy_fields.canonical_value_exact_bytes != 2) {
        *diagnostic = MakeInvalidRequestDiagnostic(
            "mga.index_store", "sorted_index_uint16_carrier_provenance_unbound");
        return false;
      }
    }
    core::datatypes::DatatypeSortKeyRequest request;
    request.value = {binding.datatype.type_id, value.bytes, value.isSqlNull()};
    if (!std::equal(std::begin(binding.execution_descriptor.descriptor_uuid.bytes),
                   std::end(binding.execution_descriptor.descriptor_uuid.bytes),
                   binding.datatype.descriptor_uuid.bytes.begin()) ||
        binding.execution_descriptor.descriptor_epoch != binding.datatype.descriptor_generation ||
        binding.descriptor.value != binding.datatype.descriptor_uuid) {
      *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_execution_binding_mismatch");
      return false;
    }
    request.value.descriptor = binding.execution_descriptor;
    if (binding.datatype.type_id == core::datatypes::CanonicalTypeId::uuid &&
        !core::datatypes::ResolveCanonicalUuidOrderingProfileV1(
            binding.execution_descriptor, &request.uuid_ordering)) {
      *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_uuid_ordering_profile_invalid");
      return false;
    }
    request.text_seed = binding.text_seed;
    if (binding.datatype.type_id == core::datatypes::CanonicalTypeId::decimal &&
        !core::datatypes::BindExactDecimalSortKeyProfile(binding.execution_descriptor, &request)) {
      *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_decimal_profile_invalid");
      return false;
    }
    std::string sort_key;
    if (binding.datatype.type_id == core::datatypes::CanonicalTypeId::date) {
      namespace dt = core::datatypes;
      if (!binding.datatype.codec || !binding.date_profile ||
          !dt::SameDatatypeTypeCodecIdentityV3(binding.date_profile->identity, *binding.datatype.codec) ||
          !dt::ValidateDateExecutionDescriptorV3(
              binding.execution_descriptor, *binding.datatype.codec).ok()) {
        *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_date_profile_unbound");
        return false;
      }
      const auto decoded = dt::DecodeCanonicalDateComponentNoAllocV3(*binding.date_profile,
          value.isSqlNull() ? dt::DateValueStateV3::sql_null : dt::DateValueStateV3::value,
          binding.execution_descriptor.nullable_allowed,
          {reinterpret_cast<const core::platform::byte*>(value.bytes.data()), value.bytes.size()});
      if (!decoded.ok()) {
        *diagnostic = MakeEngineApiDiagnostic(std::string(decoded.diagnostic.diagnostic_code),
            "datatype.sort_key.rejected", std::string(decoded.diagnostic.detail), true);
        return false;
      }
      std::array<core::platform::byte, dt::kDateValueSortKeyBytesV3> bytes{};
      const auto encoded = dt::MakeDateSortKeyViewIntoNoAllocV3(decoded.value,
          dt::DateSortDirectionV3::ascending, dt::DateNullModeV3::nulls_first,
          bytes.data(), bytes.size(), {bytes.size()});
      if (!encoded.ok()) {
        *diagnostic = MakeEngineApiDiagnostic(std::string(encoded.diagnostic.diagnostic_code),
            "datatype.sort_key.rejected", std::string(encoded.diagnostic.detail), true);
        return false;
      }
      sort_key.assign(reinterpret_cast<const char*>(bytes.data()), encoded.bytes_written);
    } else if (binding.datatype.type_id == core::datatypes::CanonicalTypeId::decimal_float) {
      if (!binding.datatype.codec ||
          !core::datatypes::IsExactCanonicalDecimal128TypeCodecIdentityV1(binding.datatype.codec->legacy_fields)) {
        *diagnostic = MakeInvalidRequestDiagnostic("mga.index_store", "sorted_index_decimal128_policy_unbound");
        return false;
      }
      if (!value.isSqlNull()) {
        const auto sorted = libraries::sbl_numeric::MakeDecimal128OrderKey(
            reinterpret_cast<const std::uint8_t*>(value.bytes.data()), value.bytes.size(),
            libraries::sbl_numeric::Decimal128OrderProfile::numeric_total_nan_last,
            binding.datatype.codec->legacy_fields.allow_special_values);
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
