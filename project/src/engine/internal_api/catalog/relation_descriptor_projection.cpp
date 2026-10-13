// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/relation_descriptor_projection.hpp"
#include "catalog/catalog_object_lifecycle.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/binary_view_options.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "catalog/name_resolution_api.hpp"
#include "crud_support/crud_store.hpp"
#include "dml/select_api.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kOperation = "catalog.relation_descriptor_projection";

struct ProjectionViewDefinition {
  std::string variant;
  std::string source_relation_name;
  EngineUuid function_uuid;
};

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool EqualsAsciiInsensitive(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const auto lhs = static_cast<unsigned char>(left[i]);
    const auto rhs = static_cast<unsigned char>(right[i]);
    if (std::tolower(lhs) != std::tolower(rhs)) return false;
  }
  return true;
}

std::vector<std::string> OptionValues(const EngineApiRequest& request,
                                      const std::string& prefix) {
  std::vector<std::string> values;
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) {
      values.push_back(option.substr(prefix.size()));
    }
  }
  return values;
}

std::string SingleOptionValue(const EngineApiRequest& request,
                              const std::string& prefix) {
  const auto values = OptionValues(request, prefix);
  return values.size() == 1 ? values.front() : std::string{};
}

bool HasExactlyOneOption(const EngineApiRequest& request,
                         const std::string& prefix,
                         const std::string& expected) {
  const auto values = OptionValues(request, prefix);
  return values.size() == 1 && values.front() == expected;
}

bool HasRawSqlOption(const EngineApiRequest& request) {
  for (const auto& option : request.option_envelopes) {
    const std::string lowered = LowerAscii(option);
    if (lowered.rfind("sql_text:", 0) == 0 ||
        lowered.rfind("raw_sql:", 0) == 0 ||
        lowered.rfind("select_sql:", 0) == 0 ||
        lowered.rfind("view_sql:", 0) == 0 ||
        lowered.rfind("query_sql:", 0) == 0) {
      return true;
    }
  }
  return false;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ProjectionDiagnostic(std::string detail) {
  return MakeInvalidRequestDiagnostic(kOperation, std::move(detail));
}

EngineApiDiagnostic ValidateExactReadableTransaction(
    const EngineRequestContext& context) {
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.is_nil()) {
    return ProjectionDiagnostic("exact_active_transaction_identity_required");
  }
  const auto parsed_transaction = scratchbird::core::uuid::MakeTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      context.transaction_uuid);
  if (!parsed_transaction.ok()) {
    return ProjectionDiagnostic("transaction_uuid_invalid");
  }
  const auto inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          context.database_path);
  if (!inventory.ok()) {
    return MakeEngineApiDiagnostic(
        inventory.diagnostic.diagnostic_code.empty()
            ? "SB-MGA-TXN-INV-LOAD-FAILED"
            : inventory.diagnostic.diagnostic_code,
        inventory.diagnostic.message_key.empty()
            ? "mga.transaction_inventory.load_failed"
            : inventory.diagnostic.message_key,
        inventory.diagnostic.remediation_hint,
        true);
  }
  const auto exact = scratchbird::transaction::mga::LookupLocalTransaction(
      inventory.inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(
          context.local_transaction_id));
  using scratchbird::transaction::mga::TransactionState;
  if (!exact.ok() ||
      exact.entry.identity.transaction_uuid.value !=
          parsed_transaction.value.value ||
      (exact.entry.state != TransactionState::active &&
       exact.entry.state != TransactionState::read_only_active)) {
    return ProjectionDiagnostic("exact_active_transaction_identity_required");
  }
  return OkDiagnostic();
}

template <typename TResult>
TResult ProjectionFailure(const EngineRequestContext& context,
                          EngineApiDiagnostic diagnostic) {
  return MakeCrudDiagnosticResult<TResult>(
      context, "dml.select_rows", std::move(diagnostic));
}

bool IsSupportedVariant(const std::string& variant) {
  return variant == kRelationDescriptorProjectionTypeInventoryVariantV1 ||
         variant == kRelationDescriptorProjectionCharsetInventoryVariantV1;
}

bool IsSafePersistedCanonicalName(const std::string& value) {
  if (value.empty() || value.size() > 1024 ||
      value.find(';') != std::string::npos) {
    return false;
  }
  for (const unsigned char ch : value) {
    if (ch == 0 || ch == '\n' || ch == '\r' || ch == '\t') return false;
  }
  return true;
}

std::optional<std::uint64_t> ParseU64Strict(const std::string& value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed;
}

std::string PayloadFieldValue(const std::string& payload,
                              const std::string& prefix) {
  std::size_t offset = 0;
  while (offset <= payload.size()) {
    const auto delimiter = payload.find(';', offset);
    const auto end =
        delimiter == std::string::npos ? payload.size() : delimiter;
    const std::string field = payload.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      return field.substr(prefix.size());
    }
    if (delimiter == std::string::npos) break;
    offset = delimiter + 1;
  }
  return {};
}

std::vector<std::string> PayloadOptions(const std::string& payload) {
  std::vector<std::string> options;
  if (!DecodeBinaryViewOptions(payload, &options)) return {};
  return options;
}

std::optional<ProjectionViewDefinition> ParseCanonicalViewOptions(
    const std::vector<std::string>& options) {
  if (options.size() != 4 && options.size() != 5) return std::nullopt;
  if (options[0] != std::string("view_query_shape:") +
                        kRelationDescriptorProjectionMarkerV1 ||
      options[1].rfind("view_source_name:", 0) != 0) {
    return std::nullopt;
  }

  ProjectionViewDefinition definition;
  definition.source_relation_name =
      options[1].substr(std::string("view_source_name:").size());
  if (!IsSafePersistedCanonicalName(definition.source_relation_name)) {
    return std::nullopt;
  }
  if (options[2] == "view_projection_count:1" &&
      options[3] ==
          std::string("view_projection_0:variant:") +
              kRelationDescriptorProjectionCharsetInventoryVariantV1 &&
      options.size() == 4) {
    definition.variant =
        kRelationDescriptorProjectionCharsetInventoryVariantV1;
    return definition;
  }
  if (options[2] == "view_projection_count:2" &&
      options[3] ==
          std::string("view_projection_0:variant:") +
              kRelationDescriptorProjectionTypeInventoryVariantV1 &&
      options.size() == 5 &&
      options[4].rfind("view_projection_1:function_uuid:", 0) == 0) {
    definition.variant = kRelationDescriptorProjectionTypeInventoryVariantV1;
    definition.function_uuid = BinaryViewUuid(options[4].substr(
        std::string("view_projection_1:function_uuid:").size()));
    if (definition.function_uuid.is_nil()) return std::nullopt;
    return definition;
  }
  return std::nullopt;
}

bool PayloadRequestsRelationDescriptorProjection(
    const std::vector<std::string>& options) {
  return std::find(
             options.begin(),
             options.end(),
             std::string("view_query_shape:") +
                 kRelationDescriptorProjectionMarkerV1) != options.end();
}

bool NameEntryMatches(const NameRegistryEntry& entry,
                      const EngineRequestContext& context,
                      const EngineUuid& object_uuid,
                      const std::string& object_class,
                      const EngineUuid& schema_uuid,
                      const std::string& expected_name) {
  if (entry.object_uuid != object_uuid ||
      entry.object_class != object_class || entry.deleted ||
      entry.lifecycle_state != "active") {
    return false;
  }
  if (!schema_uuid.is_nil() && entry.scope_uuid != schema_uuid &&
      entry.parent_schema_uuid != schema_uuid) {
    return false;
  }
  if (EqualsAsciiInsensitive(entry.raw_name_text, expected_name) ||
      EqualsAsciiInsensitive(entry.display_name, expected_name)) {
    return true;
  }
  const std::string profile =
      entry.identifier_profile_uuid.empty()
          ? NameRegistryDefaultIdentifierProfile(context)
          : entry.identifier_profile_uuid;
  return entry.normalized_lookup_key ==
         NameRegistryLookupKey(expected_name, profile, false);
}

EngineApiDiagnostic ValidateRegisteredName(const EngineRequestContext& context,
                                           const EngineUuid& object_uuid,
                                           const std::string& object_class,
                                           const EngineUuid& schema_uuid,
                                           const std::string& expected_name,
                                           const std::string& detail) {
  const auto loaded =
      LoadNameRegistryState(context, context.local_transaction_id);
  if (!loaded.ok) return loaded.diagnostic;
  for (const auto& entry : loaded.state.entries) {
    if (NameEntryMatches(entry,
                         context,
                         object_uuid,
                         object_class,
                         schema_uuid,
                         expected_name)) {
      return OkDiagnostic();
    }
  }
  return ProjectionDiagnostic(detail);
}

std::string ExecutablePayloadField(const std::string& payload,
                                   const std::string& prefix) {
  std::vector<std::string> options;
  if(!DecodeBinaryViewOptions(payload,&options))return {};
  for(const auto& option:options)if(option.starts_with(prefix))return option.substr(prefix.size());
  return {};
}

EngineApiDiagnostic ValidateTypeNameFunction(
    const EngineRequestContext& context,
    const EngineUuid& function_uuid) {
  if (function_uuid.is_nil()) {
    return ProjectionDiagnostic("type_name_function_uuid_required");
  }
  const auto loaded = LoadExecutableObjectLifecycleState(context);
  if (!loaded.ok) return loaded.diagnostic;

  const EngineExecutableObjectRecord* function = nullptr;
  for (const auto& object : loaded.state.objects) {
    if (object.object_uuid != function_uuid) continue;
    if (function == nullptr || object.event_sequence > function->event_sequence) {
      function = &object;
    }
  }
  if (function == nullptr || function->object_kind != "function" ||
      function->lifecycle_state != "active" || function->deleted ||
      function->invalidated || function->creator_tx == 0 ||
      function->executor_kind != "metadata_only" ||
      function->side_effect_class != "none" ||
      !function->stored_sblr_hash.empty() ||
      !function->internal_procedure_id.empty() ||
      ExecutablePayloadField(function->payload,
                             "compiled_body_descriptor:") !=
          kRelationTypeNameFunctionDescriptorV1) {
    return ProjectionDiagnostic(
        "type_name_function_dependency_not_visible_or_invalid");
  }
  return OkDiagnostic();
}

EngineDescriptor ScalarDescriptor(const std::string& canonical_type_name) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type_name;
  descriptor.encoded_descriptor = "canonical=" + canonical_type_name;
  return descriptor;
}

EngineTypedValue ScalarValue(const EngineDescriptor& descriptor,
                             std::string value) {
  EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  typed.state = EngineValueState::value;
  return typed;
}

EngineTypedValue ScalarValue(const EngineDescriptor& descriptor,
                             const EngineUuid& value) {
  EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.binary_value.assign(value.bytes.begin(), value.bytes.end());
  typed.is_null = false;
  typed.state = EngineValueState::value;
  return typed;
}

EngineTypedValue NullValue(const EngineDescriptor& descriptor) {
  EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value.clear();
  typed.is_null = true;
  typed.state = EngineValueState::sql_null;
  return typed;
}

std::optional<bool> IsTextLargeObject(
    const MgaRelationColumnStorageDescriptor& column) {
  CatalogColumnMetadata metadata;
  if (!DecodeCatalogColumnMetadata(column.value_descriptor.encoded_descriptor,
                                   &metadata)) return std::nullopt;
  const auto storage = metadata.text.find("text_resource_storage");
  return storage != metadata.text.end() &&
         LowerAscii(storage->second) == "large_object";
}

struct ResolvedColumnResources {
  std::string charset_name;
  std::string collation_name;
};

EngineApiDiagnostic ResolveColumnResources(
    const EngineRequestContext& context,
    const MgaRelationColumnStorageDescriptor& column,
    ResolvedColumnResources* resources) {
  if (resources == nullptr) {
    return ProjectionDiagnostic("column_resource_output_required");
  }
  if (column.charset_uuid.is_nil() && !column.collation_uuid.is_nil()) {
    return ProjectionDiagnostic("collation_requires_charset");
  }
  if (!column.charset_uuid.is_nil()) {
    EngineUuid charset_uuid;
    charset_uuid = column.charset_uuid;
    const auto charset = LookupEngineResourceDescriptorByUuid(
        context, charset_uuid, "charset");
    if (!charset.ok) return charset.diagnostic;
    if (!charset.resource_descriptor.present ||
        charset.resource_descriptor.resource_uuid !=
            column.charset_uuid ||
        charset.resource_descriptor.canonical_name.empty()) {
      return ProjectionDiagnostic("charset_descriptor_invalid");
    }
    resources->charset_name =
        charset.resource_descriptor.canonical_name;
  }
  if (!column.collation_uuid.is_nil()) {
    EngineUuid collation_uuid;
    collation_uuid = column.collation_uuid;
    const auto collation = LookupEngineResourceDescriptorByUuid(
        context, collation_uuid, "collation");
    if (!collation.ok) return collation.diagnostic;
    if (!collation.resource_descriptor.present ||
        collation.resource_descriptor.resource_uuid !=
            column.collation_uuid ||
        collation.resource_descriptor.canonical_name.empty() ||
        collation.resource_descriptor.parent_resource_uuid !=
            column.charset_uuid) {
      return ProjectionDiagnostic("collation_descriptor_invalid");
    }
    resources->collation_name =
        collation.resource_descriptor.canonical_name;
  }
  return OkDiagnostic();
}

bool CanonicalRelationColumns(
    const MgaRelationStorageDescriptor& descriptor,
    std::vector<MgaRelationColumnStorageDescriptor>* columns) {
  if (columns == nullptr || descriptor.columns.empty()) return false;
  *columns = descriptor.columns;
  std::stable_sort(columns->begin(),
                   columns->end(),
                   [](const auto& left, const auto& right) {
                     return left.ordinal < right.ordinal;
                   });
  std::set<EngineUuid> column_uuids;
  std::set<std::string> column_names;
  for (std::size_t i = 0; i < columns->size(); ++i) {
    const auto& column = (*columns)[i];
    if (column.ordinal != i || column.column_uuid.is_nil() ||
        column.canonical_name_key.empty() ||
        column.value_descriptor.canonical_type_name.empty() ||
        column.value_descriptor.encoded_descriptor.empty() ||
        !column_uuids.insert(column.column_uuid).second ||
        !column_names.insert(LowerAscii(column.canonical_name_key)).second) {
      return false;
    }
  }
  return true;
}

bool RequestContainsOnlyOneMarker(const EngineApiRequest& request,
                                  const std::string& prefix,
                                  const std::string& value) {
  const auto values = OptionValues(request, prefix);
  return values.size() == 1 && values.front() == value;
}

}  // namespace

bool IsRelationDescriptorProjectionViewCreateRequest(
    const EngineApiRequest& request) {
  return RequestContainsOnlyOneMarker(
      request,
      "view_query_shape:",
      kRelationDescriptorProjectionMarkerV1);
}

EngineApiDiagnostic ValidateRelationDescriptorProjectionViewCreate(
    const EngineApiRequest& request) {
  if (!IsRelationDescriptorProjectionViewCreateRequest(request)) {
    return ProjectionDiagnostic("projection_marker_required");
  }
  if (request.target_object.object_kind != "view" &&
      !request.target_object.object_kind.empty()) {
    return ProjectionDiagnostic("projection_target_must_be_view");
  }
  if (request.context.local_transaction_id == 0 ||
      request.context.transaction_uuid.is_nil()) {
    return ProjectionDiagnostic("exact_active_transaction_identity_required");
  }
  const auto exact_selector =
      ValidateExecutableObjectExactMgaSelector(request.context);
  if (exact_selector.error) return exact_selector;
  if (HasRawSqlOption(request)) {
    return ProjectionDiagnostic("raw_sql_forbidden");
  }
  const std::string view_name = ApiBehaviorPrimaryName(request, {});
  const std::string source_name =
      SingleOptionValue(request, "view_source_name:");
  if (!IsSafePersistedCanonicalName(view_name)) {
    return ProjectionDiagnostic("projection_view_name_invalid");
  }
  if (!IsSafePersistedCanonicalName(source_name) ||
      OptionValues(request, "view_source_name:").size() != 1) {
    return ProjectionDiagnostic("projection_source_name_invalid");
  }

  const std::string projection_zero =
      SingleOptionValue(request, "view_projection_0:");
  ProjectionViewDefinition definition;
  if (projection_zero ==
      std::string("variant:") +
          kRelationDescriptorProjectionTypeInventoryVariantV1) {
    definition.variant = kRelationDescriptorProjectionTypeInventoryVariantV1;
    if (!HasExactlyOneOption(request, "view_projection_count:", "2")) {
      return ProjectionDiagnostic("type_projection_count_invalid");
    }
    const std::string dependency =
        SingleOptionValue(request, "view_projection_1:");
    if (dependency.rfind("function_uuid:", 0) != 0) {
      return ProjectionDiagnostic("type_name_function_uuid_required");
    }
    definition.function_uuid =
        BinaryViewUuid(dependency.substr(std::string("function_uuid:").size()));
    if (definition.function_uuid.is_nil()) {
      return ProjectionDiagnostic("type_name_function_uuid_required");
    }
  } else if (projection_zero ==
             std::string("variant:") +
                 kRelationDescriptorProjectionCharsetInventoryVariantV1) {
    definition.variant =
        kRelationDescriptorProjectionCharsetInventoryVariantV1;
    if (!HasExactlyOneOption(request, "view_projection_count:", "1") ||
        !OptionValues(request, "view_projection_1:").empty()) {
      return ProjectionDiagnostic("charset_projection_shape_invalid");
    }
  } else {
    return ProjectionDiagnostic("projection_variant_invalid");
  }
  if (!IsSupportedVariant(definition.variant)) {
    return ProjectionDiagnostic("projection_variant_invalid");
  }

  if (definition.variant ==
      kRelationDescriptorProjectionTypeInventoryVariantV1) {
    const auto dependency = ValidateTypeNameFunction(
        request.context,
        definition.function_uuid);
    if (dependency.error) return dependency;
  }
  return OkDiagnostic();
}

std::vector<std::string> CanonicalRelationDescriptorProjectionViewOptions(
    const EngineApiRequest& request) {
  const std::string projection_zero =
      SingleOptionValue(request, "view_projection_0:");
  const std::string source_name =
      SingleOptionValue(request, "view_source_name:");
  std::vector<std::string> options = {
      std::string("view_query_shape:") +
          kRelationDescriptorProjectionMarkerV1,
      std::string("view_source_name:") + source_name};
  if (projection_zero ==
      std::string("variant:") +
          kRelationDescriptorProjectionTypeInventoryVariantV1) {
    const std::string function_uuid =
        SingleOptionValue(request, "view_projection_1:")
            .substr(std::string("function_uuid:").size());
    options.push_back("view_projection_count:2");
    options.push_back(
        std::string("view_projection_0:variant:") +
        kRelationDescriptorProjectionTypeInventoryVariantV1);
    options.push_back("view_projection_1:function_uuid:" + function_uuid);
  } else {
    options.push_back("view_projection_count:1");
    options.push_back(
        std::string("view_projection_0:variant:") +
        kRelationDescriptorProjectionCharsetInventoryVariantV1);
  }
  return options;
}

EngineCatalogRelationProjectionViewPreparation PrepareRelationDescriptorProjectionViewCreate(
    const EngineApiRequest& request) {
  EngineCatalogRelationProjectionViewPreparation result;
  result.diagnostic = ValidateRelationDescriptorProjectionViewCreate(request);
  if (result.diagnostic.error) return result;
  result.options = CanonicalRelationDescriptorProjectionViewOptions(request);
  const auto parsed = ParseCanonicalViewOptions(result.options);
  if (!parsed) { result.diagnostic = ProjectionDiagnostic("projection_view_definition_invalid"); return result; }
  result.descriptor.view_uuid = request.target_object.uuid.is_nil()
      ? GenerateCrudEngineUuid("object") : request.target_object.uuid;
  result.descriptor.semantic_variant = parsed->variant;
  result.descriptor.source_relation_name = parsed->source_relation_name;
  result.descriptor.function_uuid = parsed->function_uuid;
  if (!core::uuid::IsEngineIdentityUuid(result.descriptor.view_uuid) ||
      result.descriptor.view_uuid == request.target_schema.uuid) {
    result.diagnostic = ProjectionDiagnostic("projection_view_identity_invalid"); return result;
  }
  const auto functions = LoadExecutableObjectLifecycleState(request.context);
  if (!functions.ok) { result.diagnostic = functions.diagnostic; return result; }
  for (const auto& object : functions.state.objects) {
    if (object.object_uuid == result.descriptor.view_uuid) {
      result.diagnostic = ProjectionDiagnostic("projection_view_identity_occupied"); return result;
    }
    if (object.object_uuid == parsed->function_uuid) result.admitted_function = object;
  }
  if (!parsed->function_uuid.is_nil() && !result.admitted_function) {
    result.diagnostic = ProjectionDiagnostic("type_name_function_dependency_not_visible_or_invalid"); return result;
  }
  result.descriptor.present = true;
  result.descriptor.diagnostic = OkDiagnostic();
  result.ok = true;
  return result;
}

EngineApiDiagnostic RevalidateRelationDescriptorProjectionFunction(
    const EngineRequestContext& context, const std::optional<EngineExecutableObjectRecord>& admitted) {
  if (!admitted) return OkDiagnostic();
  const auto valid = ValidateTypeNameFunction(context, admitted->object_uuid);
  if (valid.error) return valid;
  const auto current = ValidateExecutableObjectCurrentAdmission(context, *admitted);
  if (current.error) return current;
  const auto actual = LoadExecutableObjectLifecycleState(context);
  if (!actual.ok) return actual.diagnostic;
  for (const auto& object : actual.state.objects)
    if (object.object_uuid == admitted->object_uuid)
      return object == *admitted ? OkDiagnostic() : ProjectionDiagnostic("projection_function_generation_changed");
  return ProjectionDiagnostic("type_name_function_dependency_not_visible_or_invalid");
}

EngineCatalogRelationProjectionViewDescriptor
DescribeEngineCatalogRelationProjectionView(
    const EngineRequestContext& context,
    const EngineUuid& view_uuid) {
  EngineCatalogRelationProjectionViewDescriptor descriptor;
  descriptor.diagnostic = OkDiagnostic();
  if (view_uuid.is_nil()) {
    descriptor.diagnostic = ProjectionDiagnostic("projection_view_uuid_required");
    return descriptor;
  }
  const auto exact_transaction = ValidateExactReadableTransaction(context);
  if (exact_transaction.error) {
    descriptor.diagnostic = exact_transaction;
    return descriptor;
  }

  const auto catalog = LoadCatalogObjectLifecycleState(context);
  if (!catalog.ok) { descriptor.diagnostic = catalog.diagnostic; return descriptor; }
  std::optional<EngineCatalogObjectRecord> view;
  for (const auto& object : catalog.state.objects) if (object.object_uuid == view_uuid) view = object;
  if (descriptor.diagnostic.error) return descriptor;
  if (!view) {
    descriptor.diagnostic =
        ProjectionDiagnostic("projection_view_not_visible");
    return descriptor;
  }
  const auto options = PayloadOptions(view->payload);
  if (options.empty() && view->payload.starts_with("SBVIEW02")) {
    descriptor.diagnostic = ProjectionDiagnostic("projection_view_definition_invalid");
    return descriptor;
  }
  if (!PayloadRequestsRelationDescriptorProjection(options)) {
    // An ordinary persisted view is not an error for this classifier.
    return descriptor;
  }
  if (view->object_kind != "view" ||
      view->lifecycle_state != "active" || view->deleted ||
      !core::uuid::IsEngineIdentityUuid(view->catalog_row_uuid)) {
    descriptor.diagnostic =
        ProjectionDiagnostic("projection_view_not_visible_or_invalid");
    return descriptor;
  }

  const auto definition = ParseCanonicalViewOptions(options);
  const EngineUuid view_schema_uuid =
      view->schema_uuid;
  if (!definition || view_schema_uuid.is_nil()) {
    descriptor.diagnostic =
        ProjectionDiagnostic("projection_view_definition_invalid");
    return descriptor;
  }
  if (definition->variant ==
      kRelationDescriptorProjectionTypeInventoryVariantV1) {
    const auto dependency = ValidateTypeNameFunction(
        context, definition->function_uuid);
    if (dependency.error) {
      descriptor.diagnostic = dependency;
      return descriptor;
    }
  } else if (!definition->function_uuid.is_nil()) {
    descriptor.diagnostic =
        ProjectionDiagnostic("charset_projection_function_forbidden");
    return descriptor;
  }

  descriptor.present = true;
  descriptor.view_uuid = view_uuid;
  descriptor.semantic_variant = definition->variant;
  descriptor.source_relation_name = definition->source_relation_name;
  descriptor.function_uuid = definition->function_uuid;
  return descriptor;
}

bool IsRelationDescriptorProjectionSelectRequest(
    const EngineSelectRowsRequest& request) {
  return RequestContainsOnlyOneMarker(
             request,
             "source_kind:",
             kRelationDescriptorProjectionSourceKind) &&
         RequestContainsOnlyOneMarker(
             request,
             "result_projection:",
             kRelationDescriptorProjectionMarkerV1);
}

EngineSelectRowsResult EngineSelectRelationDescriptorProjection(
    const EngineSelectRowsRequest& request) {
  if (!IsRelationDescriptorProjectionSelectRequest(request)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, ProjectionDiagnostic("projection_dispatch_invalid"));
  }
  if (request.context.local_transaction_id == 0 ||
      request.context.transaction_uuid.is_nil()) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("exact_active_transaction_identity_required"));
  }
  if (HasRawSqlOption(request)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, ProjectionDiagnostic("raw_sql_forbidden"));
  }

  const std::string variant =
      SingleOptionValue(request, "dml_surface_variant:");
  if (!IsSupportedVariant(variant)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, ProjectionDiagnostic("projection_variant_invalid"));
  }
  const EngineUuid view_uuid = request.target_object.uuid;
  const EngineUuid relation_uuid =
      BinaryViewUuid(SingleOptionValue(request, "source_uuid:"));
  const EngineUuid expected_descriptor_uuid =
      BinaryViewUuid(SingleOptionValue(request, "source_fingerprint:"));
  const auto expected_descriptor_generation =
      ParseU64Strict(SingleOptionValue(request, "source_position:"));
  if (view_uuid.is_nil() || relation_uuid.is_nil() ||
      expected_descriptor_uuid.is_nil() ||
      !expected_descriptor_generation ||
      *expected_descriptor_generation == 0 || view_uuid == relation_uuid) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_identity_envelope_invalid"));
  }

  const auto catalog = LoadCatalogObjectLifecycleState(request.context);
  if (!catalog.ok) return ProjectionFailure<EngineSelectRowsResult>(request.context, catalog.diagnostic);
  std::optional<EngineCatalogObjectRecord> view;
  for (const auto& object : catalog.state.objects) if (object.object_uuid == view_uuid) view = object;
  if (!view || view->object_kind != "view" ||
      view->lifecycle_state != "active" || view->deleted ||
      !core::uuid::IsEngineIdentityUuid(view->catalog_row_uuid)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_view_not_visible_or_invalid"));
  }

  const auto persisted_definition =
      ParseCanonicalViewOptions(PayloadOptions(view->payload));
  if (!persisted_definition || persisted_definition->variant != variant ||
      view->schema_uuid.is_nil()) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_view_definition_invalid"));
  }
  const EngineUuid view_schema_uuid =
      view->schema_uuid;
  if (variant == kRelationDescriptorProjectionTypeInventoryVariantV1) {
    const auto dependency = ValidateTypeNameFunction(
        request.context,
        persisted_definition->function_uuid);
    if (dependency.error) {
      return ProjectionFailure<EngineSelectRowsResult>(
          request.context, dependency);
    }
  } else if (!persisted_definition->function_uuid.is_nil()) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("charset_projection_function_forbidden"));
  }

  const auto loaded_descriptor =
      LoadMgaRelationStorageDescriptor(request.context, relation_uuid);
  if (!loaded_descriptor.ok) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, loaded_descriptor.diagnostic);
  }
  const auto& descriptor = loaded_descriptor.descriptor;
  if (descriptor.relation_uuid != relation_uuid) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_relation_uuid_mismatch"));
  }
  if (descriptor.relation_kind != "table") {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_relation_kind_mismatch"));
  }
  if (descriptor.descriptor_uuid != expected_descriptor_uuid) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_descriptor_uuid_mismatch"));
  }
  if (descriptor.descriptor_generation !=
      *expected_descriptor_generation) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_descriptor_generation_mismatch"));
  }
  if (descriptor.schema_uuid.is_nil()) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_relation_schema_uuid_required"));
  }
  const auto source_name = ValidateRegisteredName(request.context,
                                                  relation_uuid,
                                                  "table",
                                                  descriptor.schema_uuid,
                                                  persisted_definition
                                                      ->source_relation_name,
                                                  "projection_source_name_not_visible");
  if (source_name.error) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, source_name);
  }

  std::vector<MgaRelationColumnStorageDescriptor> columns;
  if (!CanonicalRelationColumns(descriptor, &columns)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_relation_columns_invalid"));
  }

  const EngineDescriptor uuid_descriptor = ScalarDescriptor("uuid");
  const EngineDescriptor text_descriptor = ScalarDescriptor("text");
  const EngineDescriptor ordinal_descriptor = ScalarDescriptor("uint64");
  const EngineDescriptor boolean_descriptor = ScalarDescriptor("boolean");

  std::vector<EngineRowValue> rows;
  rows.reserve(columns.size());
  for (const auto& column : columns) {
    const auto text_large_object = IsTextLargeObject(column);
    if (!text_large_object.has_value()) {
      return ProjectionFailure<EngineSelectRowsResult>(
          request.context, ProjectionDiagnostic("column_metadata_invalid"));
    }
    ResolvedColumnResources resources;
    const auto resources_resolved =
        ResolveColumnResources(request.context, column, &resources);
    if (resources_resolved.error) {
      return ProjectionFailure<EngineSelectRowsResult>(
          request.context, resources_resolved);
    }

    EngineRowValue row;
    row.requested_row_uuid = column.column_uuid;
    row.fields.reserve(10);
    row.fields.push_back(
        {"column_uuid",
         ScalarValue(uuid_descriptor, column.column_uuid)});
    row.fields.push_back(
        {"canonical_name_key",
         ScalarValue(text_descriptor, column.canonical_name_key)});
    row.fields.push_back(
        {"ordinal",
         ScalarValue(ordinal_descriptor, std::to_string(column.ordinal))});
    row.fields.push_back(
        {"canonical_type_name",
         ScalarValue(text_descriptor,
                     column.value_descriptor.canonical_type_name)});
    row.fields.push_back(
        {"character_length",
         column.character_length == 0
             ? NullValue(ordinal_descriptor)
             : ScalarValue(ordinal_descriptor,
                           std::to_string(column.character_length))});
    row.fields.push_back(
        {"charset_uuid",
         column.charset_uuid.is_nil()
             ? NullValue(uuid_descriptor)
             : ScalarValue(uuid_descriptor, column.charset_uuid)});
    row.fields.push_back(
        {"charset_canonical_name",
         column.charset_uuid.is_nil()
             ? NullValue(text_descriptor)
             : ScalarValue(text_descriptor, resources.charset_name)});
    row.fields.push_back(
        {"collation_uuid",
         column.collation_uuid.is_nil()
             ? NullValue(uuid_descriptor)
             : ScalarValue(uuid_descriptor, column.collation_uuid)});
    row.fields.push_back(
        {"collation_canonical_name",
         column.collation_uuid.is_nil()
             ? NullValue(text_descriptor)
             : ScalarValue(text_descriptor, resources.collation_name)});
    row.fields.push_back(
        {"text_large_object",
         ScalarValue(boolean_descriptor,
                     *text_large_object ? "true" : "false")});
    rows.push_back(std::move(row));
  }

  auto result = MakeCrudSuccessResult<EngineSelectRowsResult>(
      request.context, "dml.select_rows");
  result.primary_object.uuid = view_uuid;
  result.primary_object.object_kind = "view";
  result.visible_count = rows.size();
  result.result_shape.result_kind =
      "catalog_relation_descriptor_projection";
  result.result_shape.columns = {uuid_descriptor,
                                 text_descriptor,
                                 ordinal_descriptor,
                                 text_descriptor,
                                 ordinal_descriptor,
                                 uuid_descriptor,
                                 text_descriptor,
                                 uuid_descriptor,
                                 text_descriptor,
                                 boolean_descriptor};
  result.result_shape.rows = std::move(rows);
  result.evidence.push_back(
      {"catalog_projection_marker",
       kRelationDescriptorProjectionMarkerV1});
  result.evidence.push_back(
      {"catalog_projection_view_uuid", view_uuid});
  result.evidence.push_back(
      {"catalog_projection_relation_uuid", relation_uuid});
  result.evidence.push_back(
      {"catalog_projection_descriptor_uuid",
       descriptor.descriptor_uuid});
  result.evidence.push_back(
      {"catalog_projection_descriptor_generation",
       std::to_string(descriptor.descriptor_generation)});
  result.evidence.push_back(
      {"catalog_projection_mga_authority", "durable_transaction_inventory"});
  result.evidence.push_back(
      {"catalog_projection_parser_sql", "false"});
  result.evidence.push_back({"catalog_projection_variant", variant});
  result.evidence.push_back(
      {"catalog_projection_source_name",
       persisted_definition->source_relation_name});
  result.evidence.push_back(
      {"catalog_projection_view_creator_transaction",
       std::to_string(view->creator_tx)});
  if (!persisted_definition->function_uuid.is_nil()) {
    result.evidence.push_back(
        {"catalog_projection_function_uuid",
         persisted_definition->function_uuid});
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
