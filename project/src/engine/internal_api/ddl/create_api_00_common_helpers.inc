// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by create_api.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
#include "ddl/create_api.hpp"

#include "api_unsupported.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_model.hpp"
#include "sblr_sequence_runtime.hpp"
#include "sorted_bulk_index_build.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

namespace {

bool StartsWithLocal(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string LowerAsciiLocal(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string TrimAsciiLocal(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::map<std::string, std::string> DescriptorFieldsLocal(const std::string& descriptor) {
  std::map<std::string, std::string> fields;
  std::string part;
  for (char ch : descriptor) {
    if (ch == ';') {
      const std::string trimmed = TrimAsciiLocal(part);
      if (!trimmed.empty()) {
        const auto pos = trimmed.find('=');
        if (pos == std::string::npos) {
          fields[LowerAsciiLocal(trimmed)] = "true";
        } else {
          fields[LowerAsciiLocal(TrimAsciiLocal(trimmed.substr(0, pos)))] =
              TrimAsciiLocal(trimmed.substr(pos + 1));
        }
      }
      part.clear();
      continue;
    }
    part.push_back(ch);
  }
  const std::string trimmed = TrimAsciiLocal(part);
  if (!trimmed.empty()) {
    const auto pos = trimmed.find('=');
    if (pos == std::string::npos) {
      fields[LowerAsciiLocal(trimmed)] = "true";
    } else {
      fields[LowerAsciiLocal(TrimAsciiLocal(trimmed.substr(0, pos)))] =
          TrimAsciiLocal(trimmed.substr(pos + 1));
    }
  }
  return fields;
}

std::string DescriptorFieldLocal(const std::map<std::string, std::string>& fields,
                                 std::initializer_list<const char*> names) {
  for (const auto* name : names) {
    const auto found = fields.find(name);
    if (found != fields.end()) { return found->second; }
  }
  return {};
}

std::vector<std::string> SplitQualifiedIdentifierLocal(const std::string& path) {
  std::vector<std::string> parts;
  std::string current;
  bool quoted = false;
  for (std::size_t index = 0; index < path.size(); ++index) {
    const char ch = path[index];
    if (quoted) {
      if (ch == '"') {
        if (index + 1 < path.size() && path[index + 1] == '"') {
          current.push_back('"');
          ++index;
        } else {
          quoted = false;
        }
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '"') {
      quoted = true;
      continue;
    }
    if (ch == '.') {
      const std::string trimmed = TrimAsciiLocal(current);
      if (!trimmed.empty()) { parts.push_back(trimmed); }
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  const std::string trimmed = TrimAsciiLocal(current);
  if (!trimmed.empty()) { parts.push_back(trimmed); }
  return parts;
}

std::string ResolveDomainTypeUuidFromColumnDescriptor(const EngineApiRequest& request,
                                                      const std::string& column_descriptor,
                                                      bool* lookup_required,
                                                      std::string* requested_type_name) {
  if (lookup_required != nullptr) { *lookup_required = false; }
  if (requested_type_name != nullptr) { requested_type_name->clear(); }
  const auto descriptor_fields = DescriptorFieldsLocal(column_descriptor);
  const std::string type_name =
      DescriptorFieldLocal(descriptor_fields, {"domain_uuid", "domain", "type", "canonical"});
  if (type_name.empty() || type_name.find('.') == std::string::npos) { return {}; }
  if (type_name.find('(') != std::string::npos ||
      type_name.find(')') != std::string::npos) {
    return {};
  }
  if (requested_type_name != nullptr) { *requested_type_name = type_name; }
  if (lookup_required != nullptr) { *lookup_required = true; }
  if (StartsWithLocal(type_name, "019")) { return type_name; }
  const auto parts = SplitQualifiedIdentifierLocal(type_name);
  if (parts.empty()) { return {}; }

  EngineApiRequest resolve_request = request;
  resolve_request.localized_names.clear();
  resolve_request.option_envelopes.clear();
  resolve_request.sql_object_reference = {};
  resolve_request.sql_object_reference.expected_object_type = "domain";
  resolve_request.sql_object_reference.path_type = parts.size() > 1 ? "qualified" : "unqualified";
  resolve_request.sql_object_reference.no_search_path = parts.size() > 1;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    EngineIdentifierAtom segment;
    segment.raw_text = parts[index];
    resolve_request.sql_object_reference.path_components.push_back(std::move(segment));
  }
  resolve_request.sql_object_reference.object_name.raw_text = parts.back();

  const auto resolved = ResolveNameRegistryPublic(resolve_request, "domain");
  if (!resolved.ok || resolved.matches.size() != 1) { return {}; }
  return resolved.matches.front().object_uuid;
}

bool DescriptorBoolLocal(const std::map<std::string, std::string>& fields,
                         std::initializer_list<const char*> names) {
  const std::string value = LowerAsciiLocal(DescriptorFieldLocal(fields, names));
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool SupportedExpressionEnvelope(const std::string& envelope) {
  return StartsWithLocal(envelope, "lower:") || StartsWithLocal(envelope, "upper:") ||
         StartsWithLocal(envelope, "length:") || StartsWithLocal(envelope, "identity:") ||
         (StartsWithLocal(envelope, "lower(") && envelope.back() == ')') ||
         (StartsWithLocal(envelope, "upper(") && envelope.back() == ')') ||
         (StartsWithLocal(envelope, "length(") && envelope.back() == ')');
}

std::string LocalizedNamesEnvelope(const std::vector<EngineLocalizedName>& names) {
  std::string out;
  for (const auto& name : names) {
    if (!out.empty()) { out.push_back('|'); }
    out.append(EncodeCrudText(name.language_tag));
    out.push_back(',');
    out.append(EncodeCrudText(name.name_class));
    out.push_back(',');
    out.append(EncodeCrudText(name.path));
    out.push_back(',');
    out.append(EncodeCrudText(name.name));
    out.push_back(',');
    out.append(name.default_name ? "1" : "0");
  }
  return out;
}

std::string DefaultLocalizedName(const std::vector<EngineLocalizedName>& names, const std::string& fallback) {
  for (const auto& name : names) {
    if (name.default_name && !name.name.empty()) { return name.name; }
  }
  for (const auto& name : names) {
    if (!name.name.empty()) { return name.name; }
  }
  return fallback;
}

bool HasProfileValue(const EngineProfileSet& profile, const std::string& value) {
  return std::find(profile.names.begin(), profile.names.end(), value) != profile.names.end() ||
         std::find(profile.encoded_profiles.begin(), profile.encoded_profiles.end(), value) != profile.encoded_profiles.end();
}

bool HasOptionValue(const std::vector<std::string>& options, const std::string& value) {
  return std::find(options.begin(), options.end(), value) != options.end();
}

scratchbird::core::index::IndexFamily CoreIndexFamilyForCreateIndex(
    const CrudIndexRecord& index) {
  namespace idx = scratchbird::core::index;
  const std::string family =
      index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  if (family == kCrudIndexFamilyBtree) {
    return index.unique ? idx::IndexFamily::unique_btree : idx::IndexFamily::btree;
  }
  if (family == kCrudIndexFamilyExpression) {
    return idx::IndexFamily::expression;
  }
  if (family == kCrudIndexFamilyPartial) {
    return idx::IndexFamily::partial;
  }
  if (family == kCrudIndexFamilyCovering) {
    return idx::IndexFamily::covering;
  }
  return idx::IndexFamily::unknown;
}

std::string DiagnosticDetail(const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      return argument.value;
    }
  }
  return diagnostic.remediation_hint;
}

EngineApiDiagnostic SortedBulkBuildDiagnosticToEngine(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    const std::string& fallback_detail) {
  std::string detail = DiagnosticDetail(diagnostic);
  if (detail.empty()) {
    detail = fallback_detail;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty()
                                     ? "SB_ENGINE_API_INVALID_REQUEST"
                                     : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty()
                                     ? "engine.api.invalid_request"
                                     : diagnostic.message_key,
                                 std::move(detail),
                                 true);
}

struct CreateIndexSortedBuildResult {
  bool ok = false;
  bool selected = false;
  EngineApiDiagnostic diagnostic;
  MgaExactIndexEntryAppendBatch batch;
  std::vector<EngineEvidenceReference> evidence;
};

CreateIndexSortedBuildResult BuildCreateIndexSortedBulkArtifact(
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    const std::string& table_uuid,
    const std::vector<CrudRowVersionRecord>& rows) {
  CreateIndexSortedBuildResult result;
  const auto family = CoreIndexFamilyForCreateIndex(index);
  if (family == scratchbird::core::index::IndexFamily::unknown) {
    result.ok = true;
    result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK",
                                               "engine.api.ok",
                                               {},
                                               false);
    return result;
  }
  result.selected = true;
  const auto parsed_index = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::object,
      index.index_uuid);
  const auto parsed_table = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::object,
      table_uuid);
  if (!parsed_index.ok() || !parsed_table.ok()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("ddl.create_index",
                                                     "sorted_bulk_index_uuid_invalid");
    return result;
  }

  scratchbird::core::index::SortedBulkIndexBuildRequest build;
  build.metadata.index_uuid = parsed_index.value;
  build.metadata.table_uuid = parsed_table.value;
  build.metadata.family = family;
  build.metadata.family_name =
      index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  build.metadata.semantic_profile = NormalizeCrudIndexProfile(index.profile);
  build.metadata.unique = index.unique;
  build.metadata.rebuild = true;
  build.metadata.input_presorted = false;
  build.metadata.order_proof_valid = false;
  build.metadata.policy_allows_mutation = true;
  build.metadata.leaf_entry_capacity = 128;

  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
    for (const auto& key : CrudIndexKeysForValues(index, rows[row_index].values)) {
      scratchbird::core::index::SortedBulkIndexRowInput input;
      input.encoded_key = key;
      input.row_uuid = rows[row_index].row_uuid;
      input.version_uuid = rows[row_index].version_uuid;
      input.payload_value = CrudFieldValue(rows[row_index].values,
                                           index.column_name);
      input.source_ordinal = static_cast<std::uint64_t>(row_index);
      build.rows.push_back(std::move(input));
    }
  }

  const auto built = scratchbird::core::index::BuildSortedExactBulkIndex(build);
  if (!built.ok()) {
    result.diagnostic = SortedBulkBuildDiagnosticToEngine(
        built.diagnostic,
        built.uniqueness_refused ? "sorted_bulk_index_unique_duplicate"
                                 : "sorted_bulk_index_build_refused");
    return result;
  }

  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK",
                                             "engine.api.ok",
                                             {},
                                             false);
  result.batch.index = index;
  result.batch.table_uuid = table_uuid;
  result.batch.entries.reserve(built.entries.size());
  for (const auto& entry : built.entries) {
    result.batch.entries.push_back({entry.encoded_key,
                                    entry.payload_value,
                                    entry.row_uuid,
                                    entry.version_uuid});
  }
  for (const auto& item : built.evidence) {
    result.evidence.push_back({item.evidence_kind, item.evidence_id});
  }
  result.evidence.push_back({"sorted_bulk_index_build_route",
                             "ddl.create_index"});
  result.evidence.push_back({"sorted_bulk_index_uuid", index.index_uuid});
  return result;
}

struct TemporaryTableOptions {
  bool temporary = false;
  std::string scope = "private";
  std::string on_commit_action = "delete_rows";
  EngineApiDiagnostic diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
};

TemporaryTableOptions ResolveTemporaryTableOptions(const EngineCreateTableRequest& request) {
  TemporaryTableOptions options;
  options.temporary = HasProfileValue(request.table_physical_profile, "temporary:true") ||
                      HasProfileValue(request.table_policy_profile, "temporary:true") ||
                      HasProfileValue(request.table_compatibility_profile, "temporary:true") ||
                      HasOptionValue(request.option_envelopes, "temporary:true");
  for (const auto& option : request.option_envelopes) {
    if (option == "on_commit:delete_rows" || option == "on_commit_delete_rows:true") {
      options.on_commit_action = "delete_rows";
    } else if (option == "on_commit:preserve_rows" || option == "on_commit_preserve_rows:true") {
      options.on_commit_action = "preserve_rows";
    } else if (option.rfind("on_commit:", 0) == 0 ||
               option.rfind("on_commit_", 0) == 0) {
      options.diagnostic = MakeEngineApiDiagnostic("SB-MGA-TEMPORARY-ON-COMMIT-UNSUPPORTED",
                                                   "temporary_object.on_commit_unsupported",
                                                   option,
                                                   true);
      return options;
    } else if (option == "temporary_scope:global") {
      options.scope = "global";
    } else if (option == "temporary_scope:private" ||
               option == "temporary_scope:session" ||
               option == "temporary_scope:local") {
      options.scope = "private";
    } else if (option.rfind("temporary_scope:", 0) == 0) {
      options.diagnostic = MakeEngineApiDiagnostic("SB-MGA-TEMPORARY-SCOPE-UNSUPPORTED",
                                                   "temporary_object.scope_unsupported",
                                                   option,
                                                   true);
      return options;
    }
  }
  if (options.temporary && options.scope != "global" &&
      request.context.session_uuid.canonical.empty()) {
    options.diagnostic = MakeInvalidRequestDiagnostic("ddl.create_table", "temporary_table_requires_session_uuid");
  }
  return options;
}

bool DomainHasMetadataPolicy(const DomainRecord& record) {
  return !record.default_expression_envelope.empty() ||
         !record.check_constraint_envelope.empty() ||
         !record.cast_policy_envelope.empty() ||
         !record.mutation_policy_envelope.empty() ||
         !record.masking_policy_envelope.empty() ||
         !record.visibility_policy_envelope.empty() ||
         !record.encryption_policy_ref.empty() ||
         !record.driver_metadata_envelope.empty() ||
         !record.wire_metadata_envelope.empty() ||
         !record.element_path_envelope.empty() ||
         !record.method_binding_envelope.empty() ||
         !record.localized_names_envelope.empty() ||
         !record.comment_envelope.empty() ||
         !record.reference_alias_envelope.empty();
}

void SetDomainValidationHookStatus(DomainRecord* record) {
  if (!record->default_expression_envelope.empty() || !record->check_constraint_envelope.empty()) {
    record->validation_hook_status = "engine_builtin";
  } else if (DomainHasMetadataPolicy(*record)) {
    record->validation_hook_status = "engine_metadata";
  } else {
    record->validation_hook_status = "not_required";
  }
}

std::string ColumnDescriptorForName(const std::vector<std::pair<std::string, std::string>>& columns,
                                    const std::string& column_name) {
  for (const auto& [name, descriptor] : columns) {
    if (name == column_name) { return descriptor; }
  }
  return {};
}

bool DescriptorIsOpaqueRenderOnly(const std::string& descriptor) {
  return descriptor == "opaque_extension" || StartsWithLocal(descriptor, "opaque_extension;") ||
         descriptor.find("canonical=opaque_extension") != std::string::npos ||
         descriptor.find("base_type=opaque_extension") != std::string::npos;
}

std::string ColumnNameFromIndexEnvelope(const std::string& envelope) {
  if (envelope.empty() || envelope == "unique" || StartsWithLocal(envelope, "include:") ||
      StartsWithLocal(envelope, "where_eq:")) {
    return {};
  }
  if (StartsWithLocal(envelope, "lower:")) { return envelope.substr(6); }
  if (StartsWithLocal(envelope, "upper:")) { return envelope.substr(6); }
  if (StartsWithLocal(envelope, "length:")) { return envelope.substr(7); }
  if (StartsWithLocal(envelope, "identity:")) { return envelope.substr(9); }
  const auto open = envelope.find('(');
  const auto close = envelope.rfind(')');
  if (open != std::string::npos && close == envelope.size() - 1 && close > open + 1) {
    const std::string fn = envelope.substr(0, open);
    if (fn == "lower" || fn == "upper" || fn == "length") { return envelope.substr(open + 1, close - open - 1); }
  }
  return envelope;
