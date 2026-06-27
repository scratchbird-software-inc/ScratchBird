// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ddl/alter_api.hpp"

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_model.hpp"
#include "sblr_sequence_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

namespace scratchbird::engine::internal_api {
namespace {

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string StripAllConstraintPrefix(const std::string& envelope) {
  return StartsWith(envelope, "all:") ? envelope.substr(4) : envelope;
}

std::string MergeDomainCheckConstraint(const std::string& existing, const std::string& incoming) {
  if (incoming.empty()) return existing;
  if (existing.empty()) return incoming;
  std::string merged = "all:";
  merged.append(StripAllConstraintPrefix(existing));
  merged.push_back(';');
  merged.append(StripAllConstraintPrefix(incoming));
  return merged;
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

void SetDomainValidationHookStatus(DomainRecord* record) {
  if (!record->default_expression_envelope.empty() || !record->check_constraint_envelope.empty()) {
    record->validation_hook_status = "engine_builtin";
    return;
  }
  if (!record->cast_policy_envelope.empty() || !record->mutation_policy_envelope.empty() ||
      !record->masking_policy_envelope.empty() || !record->visibility_policy_envelope.empty() ||
      !record->encryption_policy_ref.empty() || !record->driver_metadata_envelope.empty() ||
      !record->wire_metadata_envelope.empty() || !record->element_path_envelope.empty() ||
      !record->method_binding_envelope.empty() || !record->localized_names_envelope.empty() ||
      !record->comment_envelope.empty() || !record->reference_alias_envelope.empty()) {
    record->validation_hook_status = "engine_metadata";
    return;
  }
  record->validation_hook_status = "not_required";
}

std::optional<std::int64_t> ParseI64(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::int64_t value = 0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) return std::nullopt;
  return value;
}

std::optional<std::uint64_t> ParseU64(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) return std::nullopt;
  return value;
}

std::string UpsertDescriptorField(std::string descriptor,
                                  const std::string& key,
                                  const std::string& value) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  bool replaced = false;
  while (start <= descriptor.size()) {
    const std::size_t end = descriptor.find(';', start);
    const std::string part = descriptor.substr(
        start,
        end == std::string::npos ? std::string::npos : end - start);
    if (part.rfind(key + "=", 0) == 0) {
      parts.push_back(key + "=" + value);
      replaced = true;
    } else if (!part.empty()) {
      parts.push_back(part);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (!replaced) parts.push_back(key + "=" + value);
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) out.push_back(';');
    out.append(part);
  }
  return out;
}

bool HasColumnValue(const std::vector<std::pair<std::string, std::string>>& values,
                    const std::string& column_name) {
  return std::any_of(values.begin(),
                     values.end(),
                     [&](const auto& value) { return value.first == column_name; });
}

void RemoveColumnValue(std::vector<std::pair<std::string, std::string>>* values,
                       const std::string& column_name) {
  if (values == nullptr) return;
  values->erase(std::remove_if(values->begin(),
                               values->end(),
                               [&](const auto& value) {
                                 return value.first == column_name;
                               }),
                values->end());
}

bool RenameColumnValue(std::vector<std::pair<std::string, std::string>>* values,
                       const std::string& column_name,
                       const std::string& new_column_name) {
  if (values == nullptr) return false;
  bool renamed = false;
  for (auto& value : *values) {
    if (value.first == column_name) {
      value.first = new_column_name;
      renamed = true;
      break;
    }
  }
  return renamed;
}

std::vector<CrudRowVersionRecord> BuildAlteredColumnRowVersions(
    const CrudState& state,
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::string& action,
    const std::string& column_name,
    const std::string& new_column_name) {
  std::vector<CrudRowVersionRecord> altered_rows;
  if (action != "add_column" && action != "drop_column" &&
      action != "rename_column") {
    return altered_rows;
  }
  for (const auto& row :
       VisibleCrudRowsForContext(state, table.table_uuid, context)) {
    CrudRowVersionRecord updated = row;
    updated.creator_tx = context.local_transaction_id;
    updated.version_uuid = GenerateCrudEngineUuid("row");
    updated.previous_version_uuid = row.version_uuid;
    updated.previous_sequence = row.sequence;
    updated.deleted = false;
    bool changed = false;
    if (action == "add_column") {
      if (!HasColumnValue(updated.values, column_name)) {
        updated.values.push_back({column_name, "<NULL>"});
        changed = true;
      }
    } else if (action == "drop_column") {
      if (HasColumnValue(updated.values, column_name)) {
        RemoveColumnValue(&updated.values, column_name);
        changed = true;
      }
    } else if (action == "rename_column") {
      changed = RenameColumnValue(&updated.values, column_name, new_column_name);
      if (!changed && !HasColumnValue(updated.values, new_column_name)) {
        updated.values.push_back({new_column_name, "<NULL>"});
        changed = true;
      }
    }
    if (changed) altered_rows.push_back(std::move(updated));
  }
  return altered_rows;
}

scratchbird::engine::sblr::SblrExecutionContext AlterSblrContext(
    const EngineRequestContext& context) {
  scratchbird::engine::sblr::SblrExecutionContext out;
  out.database_path = context.database_path;
  out.database_uuid = context.database_uuid.canonical;
  out.cluster_uuid = context.cluster_uuid.canonical;
  out.node_uuid = context.node_uuid.canonical;
  out.transaction_uuid = context.transaction_uuid.canonical;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  out.transaction_isolation_level = context.transaction_isolation_level;
  out.statement_uuid = context.statement_uuid.canonical;
  out.session_uuid = context.session_uuid.canonical;
  out.user_uuid = context.principal_uuid.canonical;
  out.current_role_uuid = context.current_role_uuid.canonical;
  out.current_schema_uuid = context.current_schema_uuid.canonical;
  out.statement_timestamp = context.statement_timestamp;
  out.transaction_timestamp = context.transaction_timestamp;
  out.current_timestamp = context.current_timestamp;
  out.current_monotonic_ns = context.current_monotonic_ns;
  out.security_context_present = context.security_context_present;
  out.transaction_context_present =
      context.local_transaction_id != 0 || !context.transaction_uuid.canonical.empty();
  out.cluster_authority_available = context.cluster_authority_available;
  out.read_only_mode = context.read_only_mode;
  return out;
}

EngineApiDiagnostic SequenceRuntimeDiagnostic(const std::string& operation_id,
                                              const scratchbird::engine::sblr::SblrResult& result) {
  if (!result.diagnostics.empty()) {
    const auto& diagnostic = result.diagnostics.front();
    return MakeEngineApiDiagnostic(diagnostic.diagnostic_id.empty()
                                       ? "SEQUENCE.RUNTIME.FAILED"
                                       : diagnostic.diagnostic_id,
                                   diagnostic.message_key.empty()
                                       ? "sequence_runtime_failed"
                                       : diagnostic.message_key,
                                   diagnostic.detail.empty()
                                       ? "sequence runtime operation failed"
                                       : diagnostic.detail);
  }
  return MakeInvalidRequestDiagnostic(operation_id, "sequence_runtime_operation_failed");
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DDL_ALTER_API_BEHAVIOR
EngineAlterObjectResult EngineAlterObject(const EngineAlterObjectRequest& request) {
  if (request.target_object.object_kind == "schema") {
    const auto context_status = ValidateApiBehaviorContext(request.context, "ddl.alter_object", true, true);
    if (context_status.error) {
      return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(request.context, "ddl.alter_object", context_status);
    }
    if (request.target_object.uuid.canonical.empty()) {
      return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "target_schema_uuid_required"));
    }
    auto existing = FindVisibleSchemaTreeRecord(request.context,
                                                request.target_object.uuid.canonical,
                                                request.context.local_transaction_id);
    if (!existing) {
      return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "target_schema_not_visible"));
    }
    EngineSchemaTreeRecord updated = *existing;
    updated.creator_tx = request.context.local_transaction_id;
    if (!request.target_schema.uuid.canonical.empty()) {
      if (!FindVisibleSchemaTreeRecord(request.context, request.target_schema.uuid.canonical, request.context.local_transaction_id)) {
        return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "parent_schema_not_visible"));
      }
      if (SchemaTreeWouldCreateCycle(request.context,
                                     request.target_object.uuid.canonical,
                                     request.target_schema.uuid.canonical,
                                     request.context.local_transaction_id)) {
        return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "schema_move_cycle_detected"));
      }
      updated.parent_schema_uuid = request.target_schema.uuid.canonical;
    }
    if (!request.localized_names.empty()) {
      updated.localized_names = request.localized_names;
      updated.default_name = SchemaTreeDefaultName(updated.localized_names, updated.default_name);
    }
    for (const auto& option : request.option_envelopes) {
      if (StartsWith(option, "comment:")) {
        updated.localized_comments.push_back({"und", option.substr(8)});
      } else if (StartsWith(option, "localized_comment:")) {
        const std::string rest = option.substr(18);
        const auto pos = rest.find(':');
        if (pos == std::string::npos) {
          return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "localized_comment_requires_language_and_text"));
        }
        updated.localized_comments.push_back({rest.substr(0, pos), rest.substr(pos + 1)});
      } else {
        return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            UnsupportedCrudFeatureDiagnostic("ddl.alter_object", "unsupported_schema_alter_option"));
      }
    }
    if (const auto conflict = SchemaTreePathConflict(request.context,
                                                    updated.schema_uuid,
                                                    updated.parent_schema_uuid,
                                                    updated.localized_names,
                                                    request.context.local_transaction_id)) {
      return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "schema_path_ambiguous:" + *conflict));
    }
    updated.payload = SchemaTreePayload(updated.parent_schema_uuid, updated.localized_names, updated.localized_comments);
    const auto appended = PersistSchemaTreeRecord(request.context, updated, "ddl.alter_schema");
    if (appended.error) {
      return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(request.context, "ddl.alter_object", appended);
    }
    if (!request.localized_names.empty()) {
      const auto retired = RetireNameRegistryEntriesForObject(request.context,
                                                             "ddl.alter_schema",
                                                             updated.schema_uuid);
      if (retired.error) {
        return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(request.context, "ddl.alter_object", retired);
      }
      const auto names_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                     "ddl.alter_schema",
                                                                     updated.schema_uuid,
                                                                     "schema",
                                                                     updated.parent_schema_uuid,
                                                                     updated.localized_names,
                                                                     updated.default_name);
      if (names_appended.error) {
        return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(request.context, "ddl.alter_object", names_appended);
      }
    }
    auto result = MakeApiBehaviorSuccess<EngineAlterObjectResult>(request.context, "ddl.alter_object");
    result.primary_object = request.target_object;
    result.catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
    AddApiBehaviorEvidence(&result, "api_behavior_event", "ddl.alter_schema");
    AddApiBehaviorEvidence(&result, "schema_identity_preserved", updated.schema_uuid);
    AddApiBehaviorRow(&result, {{"object_uuid", updated.schema_uuid},
                                {"object_kind", "schema"},
                                {"name", updated.default_name},
                                {"state", updated.state},
                                {"payload", updated.payload}});
    AddDdlPublicationResult(&result,
                            "ddl.alter_object",
                            "schema",
                            updated.schema_uuid,
                            result.catalog_row_uuid.canonical,
                            "schema_tree");
    return result;
  }
  if (request.target_object.object_kind == "sequence") {
    if (request.context.local_transaction_id == 0) {
      return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "local_transaction_id_required"));
    }
    const std::string lookup_key = SecurityOptionValue(request, "sequence_lookup_key:");
    std::vector<std::string> runtime_keys;
    if (!lookup_key.empty()) runtime_keys.push_back(lookup_key);
    if (!request.target_object.uuid.canonical.empty() &&
        request.target_object.uuid.canonical != lookup_key) {
      runtime_keys.push_back(request.target_object.uuid.canonical);
    }
    if (runtime_keys.empty()) {
      return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "target_sequence_uuid_required"));
    }
    scratchbird::engine::sblr::SblrSequenceAlteration alteration;
    const std::string cache_value = SecurityOptionValue(request, "sequence_cache:");
    if (!cache_value.empty()) {
      const auto parsed = ParseU64(cache_value);
      if (!parsed.has_value() || *parsed == 0) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "sequence_cache_invalid"));
      }
      alteration.cache_size = *parsed;
    }
    const std::string max_value = SecurityOptionValue(request, "sequence_max_value:");
    if (!max_value.empty()) {
      const auto parsed = ParseI64(max_value);
      if (!parsed.has_value()) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "sequence_maxvalue_invalid"));
      }
      alteration.maximum_value = *parsed;
    }
    const std::string restart_value = SecurityOptionValue(request, "sequence_restart_value:");
    std::optional<std::int64_t> restart;
    if (!restart_value.empty()) {
      restart = ParseI64(restart_value);
      if (!restart.has_value()) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "sequence_restart_invalid"));
      }
    }
    const auto sblr_context = AlterSblrContext(request.context);
    for (const auto& key : runtime_keys) {
      alteration.sequence_uuid = key;
      const auto altered = scratchbird::engine::sblr::AlterSblrSequence(
          &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(),
          alteration,
          sblr_context);
      if (!altered.ok()) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            SequenceRuntimeDiagnostic("ddl.alter_object", altered));
      }
      if (restart.has_value()) {
        scratchbird::engine::sblr::SblrSequenceRequest sequence_request;
        sequence_request.context = sblr_context;
        sequence_request.sequence_uuid = key;
        sequence_request.result_descriptor_id = "int64";
        sequence_request.set_value = *restart;
        sequence_request.is_called = false;
        const auto restarted = scratchbird::engine::sblr::SetSblrSequenceValue(
            &scratchbird::engine::sblr::ProcessSblrSequenceRegistry(),
            sequence_request);
        if (!restarted.ok()) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              SequenceRuntimeDiagnostic("ddl.alter_object", restarted));
        }
      }
    }
    auto result = PersistedRecordResult<EngineAlterObjectResult>(
        request,
        "ddl.alter_object",
        "sequence",
        true,
        "altered");
    if (!result.ok) return result;
    result.primary_object = request.target_object;
    AddApiBehaviorEvidence(&result, "sequence_runtime_alter", runtime_keys.front());
    if (alteration.cache_size.has_value()) {
      AddApiBehaviorEvidence(&result, "sequence_runtime_cache", std::to_string(*alteration.cache_size));
    }
    if (alteration.maximum_value.has_value()) {
      AddApiBehaviorEvidence(&result, "sequence_runtime_max_value", std::to_string(*alteration.maximum_value));
    }
    if (restart.has_value()) {
      AddApiBehaviorEvidence(&result, "sequence_runtime_restart", std::to_string(*restart));
    }
    AddDdlPublicationResult(&result,
                            "ddl.alter_object",
                            "sequence",
                            result.primary_object.uuid.canonical,
                            result.catalog_row_uuid.canonical,
                            "sequence_runtime");
    return result;
  }
  if (request.target_object.object_kind == "domain") {
    if (request.context.local_transaction_id == 0) {
      return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "local_transaction_id_required"));
    }
    if (request.target_object.uuid.canonical.empty()) {
      return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "target_domain_uuid_required"));
    }
    auto domain = FindVisibleDomain(request.context,
                                    request.target_object.uuid.canonical,
                                    request.context.local_transaction_id);
    if (!domain) {
      return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "target_domain_not_visible"));
    }
    DomainRecord updated = *domain;
    updated.creator_tx = request.context.local_transaction_id;
    if (!request.localized_names.empty()) {
      updated.localized_names_envelope = LocalizedNamesEnvelope(request.localized_names);
      updated.default_name = DefaultLocalizedName(request.localized_names, updated.default_name);
    }
    for (const auto& profile : request.policy_profile.encoded_profiles) {
      if (profile == "nullable:true") {
        updated.nullable = true;
      } else if (profile == "nullable:false") {
        if (DomainHasCrudDependencies(request.context, updated.domain_uuid, request.context.local_transaction_id)) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "domain_nullable_tightening_requires_dependency_revalidation"));
        }
        updated.nullable = false;
      } else if (StartsWith(profile, "numeric_metadata:")) {
        updated.numeric_metadata = profile.substr(17);
      } else if (StartsWith(profile, "domain_cast_policy:")) {
        updated.cast_policy_envelope = profile.substr(19);
      } else if (StartsWith(profile, "domain_mutation_policy:")) {
        updated.mutation_policy_envelope = profile.substr(23);
      } else if (StartsWith(profile, "domain_masking_policy:")) {
        updated.masking_policy_envelope = profile.substr(22);
      } else if (StartsWith(profile, "domain_visibility_policy:")) {
        updated.visibility_policy_envelope = profile.substr(25);
      } else if (StartsWith(profile, "domain_encryption_policy:")) {
        updated.encryption_policy_ref = profile.substr(25);
      } else if (StartsWith(profile, "domain_reference_alias:")) {
        if (!updated.reference_alias_envelope.empty()) { updated.reference_alias_envelope.push_back('|'); }
        updated.reference_alias_envelope.append(profile.substr(19));
      }
    }
    for (const auto& profile : request.compatibility_profile.encoded_profiles) {
      if (StartsWith(profile, "reference_alias:")) {
        if (!updated.reference_alias_envelope.empty()) { updated.reference_alias_envelope.push_back('|'); }
        updated.reference_alias_envelope.append(profile.substr(12));
      }
    }
    bool append_check_constraint = false;
    for (const auto& option : request.option_envelopes) {
      if (option == "check_constraint_append:true") {
        append_check_constraint = true;
        break;
      }
    }
    for (const auto& option : request.option_envelopes) {
      if (option == "nullable:true") {
        updated.nullable = true;
      } else if (option == "nullable:false") {
        if (DomainHasCrudDependencies(request.context, updated.domain_uuid, request.context.local_transaction_id)) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "domain_nullable_tightening_requires_dependency_revalidation"));
        }
        updated.nullable = false;
      } else if (option == "drop_default") {
        updated.default_expression_envelope.clear();
      } else if (option == "drop_check") {
        updated.check_constraint_envelope.clear();
      } else if (StartsWith(option, "default_expression:")) {
        updated.default_expression_envelope = option.substr(19);
      } else if (StartsWith(option, "check_constraint_append:")) {
        continue;
      } else if (StartsWith(option, "check_constraint:")) {
        const std::string incoming_constraint = option.substr(17);
        updated.check_constraint_envelope =
            append_check_constraint
                ? MergeDomainCheckConstraint(updated.check_constraint_envelope, incoming_constraint)
                : incoming_constraint;
        if (!IsSupportedDomainCheckEnvelope(updated.check_constraint_envelope)) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              UnsupportedCrudFeatureDiagnostic("ddl.alter_object", "unsupported_domain_check_constraint"));
        }
      } else if (StartsWith(option, "collation:")) {
        updated.charset_or_collation_ref = option.substr(10);
      } else if (StartsWith(option, "charset:")) {
        updated.charset_or_collation_ref = option.substr(8);
      } else if (StartsWith(option, "cast_policy:")) {
        updated.cast_policy_envelope = option.substr(12);
      } else if (StartsWith(option, "mutation_policy:")) {
        updated.mutation_policy_envelope = option.substr(16);
      } else if (StartsWith(option, "masking_policy:")) {
        updated.masking_policy_envelope = option.substr(15);
      } else if (StartsWith(option, "visibility_policy:")) {
        updated.visibility_policy_envelope = option.substr(18);
      } else if (StartsWith(option, "encryption_policy:")) {
        updated.encryption_policy_ref = option.substr(18);
      } else if (StartsWith(option, "driver_metadata:")) {
        updated.driver_metadata_envelope = option.substr(16);
      } else if (StartsWith(option, "wire_metadata:")) {
        updated.wire_metadata_envelope = option.substr(14);
      } else if (StartsWith(option, "element_path:")) {
        updated.element_path_envelope = option.substr(13);
      } else if (StartsWith(option, "method_binding:")) {
        updated.method_binding_envelope = option.substr(15);
      } else if (StartsWith(option, "comment:")) {
        updated.comment_envelope = option.substr(8);
      } else if (StartsWith(option, "localized_comment:")) {
        if (!updated.comment_envelope.empty()) { updated.comment_envelope.push_back('|'); }
        updated.comment_envelope.append(option.substr(18));
      } else if (StartsWith(option, "reference_alias:")) {
        if (!updated.reference_alias_envelope.empty()) { updated.reference_alias_envelope.push_back('|'); }
        updated.reference_alias_envelope.append(option.substr(12));
      } else {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            UnsupportedCrudFeatureDiagnostic("ddl.alter_object", "unsupported_domain_alter_option"));
      }
    }
    if (!request.descriptors.empty()) {
      if (DomainHasCrudDependencies(request.context, updated.domain_uuid, request.context.local_transaction_id)) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "domain_base_change_requires_dependency_revalidation"));
      }
      const auto& descriptor = request.descriptors.front();
      const std::string base_domain_uuid = DomainUuidFromDescriptor(descriptor);
      if (!base_domain_uuid.empty()) {
        if (base_domain_uuid == updated.domain_uuid ||
            DomainChainContainsUuid(request.context, base_domain_uuid, updated.domain_uuid, request.context.local_transaction_id)) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "domain_base_cycle_detected"));
        }
        const auto base_domain = FindVisibleDomain(request.context, base_domain_uuid, request.context.local_transaction_id);
        if (!base_domain) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "base_domain_not_visible"));
        }
        if (updated.nullable && !base_domain->nullable) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "domain_chain_cannot_widen_nullability"));
        }
        updated.base_descriptor_uuid = base_domain_uuid;
        updated.base_descriptor_kind = "domain";
        updated.base_canonical_type_name = base_domain->base_canonical_type_name;
        updated.base_encoded_descriptor = DomainDescriptor(*base_domain).encoded_descriptor;
      } else {
        if (descriptor.canonical_type_name.empty()) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "base_canonical_type_name_required"));
        }
        updated.base_descriptor_uuid = descriptor.descriptor_uuid.canonical.empty()
                                           ? GenerateCrudEngineUuid("object")
                                           : descriptor.descriptor_uuid.canonical;
        updated.base_descriptor_kind = descriptor.descriptor_kind.empty() ? "scalar" : descriptor.descriptor_kind;
        updated.base_canonical_type_name = descriptor.canonical_type_name;
        updated.base_encoded_descriptor = descriptor.encoded_descriptor;
      }
    }
    SetDomainValidationHookStatus(&updated);
    updated.creator_tx = request.context.local_transaction_id;
    const auto appended = AppendDomainEvent(request.context, MakeDomainAlterEvent(updated));
    if (appended.error) {
      return MakeCrudDiagnosticResult<EngineAlterObjectResult>(request.context, "ddl.alter_object", appended);
    }
    if (!request.localized_names.empty()) {
      const auto retired = RetireNameRegistryEntriesForObject(request.context,
                                                             "ddl.alter_object",
                                                             updated.domain_uuid);
      if (retired.error) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(request.context, "ddl.alter_object", retired);
      }
      const auto names_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                     "ddl.alter_object",
                                                                     updated.domain_uuid,
                                                                     "domain",
                                                                     updated.schema_uuid,
                                                                     request.localized_names,
                                                                     updated.default_name);
      if (names_appended.error) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(request.context, "ddl.alter_object", names_appended);
      }
    }
    auto result = MakeCrudSuccessResult<EngineAlterObjectResult>(request.context, "ddl.alter_object");
    result.primary_object = request.target_object;
    result.catalog_row_uuid.canonical = updated.catalog_row_uuid;
    result.evidence.push_back({"domain_event", "domain_alter"});
    AddDdlPublicationResult(&result,
                            "ddl.alter_object",
                            "domain",
                            updated.domain_uuid,
                            result.catalog_row_uuid.canonical,
                            "domain_event");
    return result;
  }
  if (request.target_object.object_kind == "table") {
    if (request.context.local_transaction_id == 0) {
      return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "local_transaction_id_required"));
    }
    if (request.target_object.uuid.canonical.empty()) {
      return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object",
          MakeInvalidRequestDiagnostic("ddl.alter_object", "target_table_uuid_required"));
    }
    const std::string action = SecurityOptionValue(request, "table_alter_action:");
    if (action == "add_column" || action == "drop_column" ||
        action == "rename_column" || action == "alter_column_default") {
      const auto loaded = LoadMgaRelationStoreState(request.context);
      if (!loaded.ok) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            loaded.diagnostic);
      }
      const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
      auto visible = FindVisibleCrudTable(state,
                                          request.target_object.uuid.canonical,
                                          request.context.local_transaction_id);
      if (!visible) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "target_table_not_visible"));
      }
      CrudTableRecord updated = *visible;
      updated.creator_tx = request.context.local_transaction_id;
      const std::string column_name = SecurityOptionValue(request, "column_name:");
      const std::string new_column_name = SecurityOptionValue(request, "new_column_name:");
      if (action == "add_column") {
        if (column_name.empty()) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "column_name_required"));
        }
        for (const auto& column : updated.columns) {
          if (column.first == column_name) {
            return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
                request.context,
                "ddl.alter_object",
                MakeInvalidRequestDiagnostic("ddl.alter_object", "column_already_exists"));
          }
        }
        std::string descriptor = SecurityOptionValue(request, "column_descriptor:");
        if (descriptor.empty()) descriptor = "type=text;nullable=true";
        updated.columns.push_back({column_name, descriptor});
      } else if (action == "drop_column") {
        const auto before = updated.columns.size();
        updated.columns.erase(std::remove_if(updated.columns.begin(),
                                             updated.columns.end(),
                                             [&](const auto& column) {
                                               return column.first == column_name;
                                             }),
                              updated.columns.end());
        if (updated.columns.size() == before) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "column_not_visible"));
        }
      } else if (action == "rename_column") {
        bool renamed = false;
        for (auto& column : updated.columns) {
          if (column.first == new_column_name) {
            return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
                request.context,
                "ddl.alter_object",
                MakeInvalidRequestDiagnostic("ddl.alter_object", "new_column_already_exists"));
          }
        }
        for (auto& column : updated.columns) {
          if (column.first == column_name) {
            column.first = new_column_name;
            renamed = true;
            break;
          }
        }
        if (!renamed) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "column_not_visible"));
        }
      } else if (action == "alter_column_default") {
        bool altered = false;
        const std::string default_expression = SecurityOptionValue(request, "default_expression:");
        for (auto& column : updated.columns) {
          if (column.first == column_name) {
            column.second = UpsertDescriptorField(column.second, "default", default_expression);
            altered = true;
            break;
          }
        }
        if (!altered) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              MakeInvalidRequestDiagnostic("ddl.alter_object", "column_not_visible"));
        }
      }
      std::vector<CrudRowVersionRecord> row_versions = BuildAlteredColumnRowVersions(
          state,
          request.context,
          *visible,
          action,
          column_name,
          new_column_name);
      const auto appended = AppendMgaTableMetadata(request.context, updated);
      if (appended.error) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            appended);
      }
      if (!row_versions.empty()) {
        const auto rows_appended =
            AppendMgaRowVersions(request.context, &row_versions, nullptr);
        if (rows_appended.error) {
          return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
              request.context,
              "ddl.alter_object",
              rows_appended);
        }
      }
      auto result = MakeCrudSuccessResult<EngineAlterObjectResult>(request.context, "ddl.alter_object");
      result.primary_object = request.target_object;
      result.catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
      AddApiBehaviorEvidence(&result, "table_alter_action", action);
      if (!row_versions.empty()) {
        AddApiBehaviorEvidence(&result,
                               "table_alter_row_versions",
                               std::to_string(row_versions.size()));
      }
      AddDdlPublicationResult(&result,
                              "ddl.alter_object",
                              "table",
                              request.target_object.uuid.canonical,
                              result.catalog_row_uuid.canonical,
                              "mga_relation_metadata");
      return result;
    }
    if (!request.localized_names.empty()) {
      const auto loaded = LoadMgaRelationStoreState(request.context);
      if (!loaded.ok) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            loaded.diagnostic);
      }
      const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
      auto visible = FindVisibleCrudTable(state,
                                          request.target_object.uuid.canonical,
                                          request.context.local_transaction_id);
      if (!visible) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "target_table_not_visible"));
      }

      CrudTableRecord updated = *visible;
      updated.creator_tx = request.context.local_transaction_id;
      updated.default_name = DefaultLocalizedName(request.localized_names, updated.default_name);
      const auto appended = AppendMgaTableMetadata(request.context, updated);
      if (appended.error) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            appended);
      }

      EngineApiRequest name_lookup_request;
      name_lookup_request.context = request.context;
      const auto existing_name = MapNameRegistryUuidToNamePublic(
          name_lookup_request,
          request.target_object.uuid.canonical,
          "table");
      const std::string existing_scope_uuid =
          existing_name.ok ? existing_name.entry.scope_uuid : std::string{};

      const auto retired = RetireNameRegistryEntriesForObject(request.context,
                                                             "ddl.alter_object",
                                                             request.target_object.uuid.canonical);
      if (retired.error) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            retired);
      }
      std::string scope_uuid = request.target_schema.uuid.canonical;
      if (scope_uuid.empty()) scope_uuid = existing_scope_uuid;
      if (scope_uuid.empty()) scope_uuid = request.context.current_schema_uuid.canonical;
      if (scope_uuid.empty()) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            MakeInvalidRequestDiagnostic("ddl.alter_object", "target_table_schema_required"));
      }
      const auto names_appended = PersistNameRegistryEntriesForObject(
          request.context,
          "ddl.alter_object",
          request.target_object.uuid.canonical,
          "table",
          scope_uuid,
          request.localized_names,
          updated.default_name);
      if (names_appended.error) {
        return MakeCrudDiagnosticResult<EngineAlterObjectResult>(
            request.context,
            "ddl.alter_object",
            names_appended);
      }

      auto result = MakeCrudSuccessResult<EngineAlterObjectResult>(
          request.context,
          "ddl.alter_object");
      result.primary_object = request.target_object;
      result.catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
      AddApiBehaviorEvidence(&result, "table_alter_action", "rename_table");
      AddApiBehaviorEvidence(&result,
                             "table_identity_preserved",
                             request.target_object.uuid.canonical);
      AddApiBehaviorEvidence(&result, "table_default_name", updated.default_name);
      AddDdlPublicationResult(&result,
                              "ddl.alter_object",
                              "table",
                              request.target_object.uuid.canonical,
                              result.catalog_row_uuid.canonical,
                              "mga_relation_metadata");
      return result;
    }
  }
  auto result = PersistedRecordResult<EngineAlterObjectResult>(request, "ddl.alter_object", "object_alteration", true, "altered");
  if (!result.ok || request.localized_names.empty() || request.target_object.uuid.canonical.empty()) { return result; }
  const auto retired = RetireNameRegistryEntriesForObject(request.context,
                                                         "ddl.alter_object",
                                                         request.target_object.uuid.canonical);
  if (retired.error) {
    return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(request.context, "ddl.alter_object", retired);
  }
  const std::string object_kind = request.target_object.object_kind.empty() ? "object" : request.target_object.object_kind;
  const std::string fallback_name = request.localized_names.front().name.empty() ? object_kind : request.localized_names.front().name;
  const auto names_appended = PersistNameRegistryEntriesForObject(request.context,
                                                                 "ddl.alter_object",
                                                                 request.target_object.uuid.canonical,
                                                                 object_kind,
                                                                 request.target_schema.uuid.canonical,
                                                                 request.localized_names,
                                                                 fallback_name);
  if (names_appended.error) {
    return MakeApiBehaviorDiagnostic<EngineAlterObjectResult>(request.context, "ddl.alter_object", names_appended);
  }
  AddApiBehaviorEvidence(&result, "name_registry", request.target_object.uuid.canonical);
  AddDdlPublicationResult(&result,
                          "ddl.alter_object",
                          object_kind,
                          request.target_object.uuid.canonical,
                          result.catalog_row_uuid.canonical,
                          object_kind);
  return result;
}

EngineAlterConstraintResult EngineAlterConstraint(const EngineAlterConstraintRequest& request) {
  constexpr const char* kOperation = "ddl.constraint.alter";
  EngineCatalogApplyConstraintsRequest catalog_request;
  static_cast<EngineApiRequest&>(catalog_request) = request;
  catalog_request.operation_id = kOperation;
  const auto applied = EngineCatalogApplyConstraintsToObject(catalog_request);

  EngineAlterConstraintResult result;
  result.ok = applied.ok;
  result.operation_id = kOperation;
  result.diagnostics = applied.diagnostics;
  result.unsupported_features = applied.unsupported_features;
  result.evidence = applied.evidence;
  result.result_shape = applied.result_shape;
  result.primary_object = applied.primary_object;
  result.catalog_row_uuid = applied.catalog_row_uuid;
  result.transaction_uuid = applied.transaction_uuid;
  result.local_transaction_id = applied.local_transaction_id;
  result.embedded_trust_mode_observed = applied.embedded_trust_mode_observed;
  result.cluster_authority_required = applied.cluster_authority_required;
  result.bound_object_identity = applied.bound_object_identity;
  result.metadata_cache_epoch = applied.metadata_cache_epoch;
  if (result.ok) {
    result.evidence.push_back({"ddl_catalog_route", "sys.constraint_descriptor"});
    AddDdlPublicationResult(&result,
                            kOperation,
                            "constraint",
                            result.primary_object.uuid.canonical,
                            result.catalog_row_uuid.canonical,
                            "constraint_descriptor");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
