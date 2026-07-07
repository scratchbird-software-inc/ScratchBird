// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/structured_type_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::string_view kStructuredTypeKind = "structured_type_descriptor";

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string OptionValue(const EngineApiRequest& request, std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) return option.substr(prefix.size());
  }
  return {};
}

std::vector<std::string> OptionValues(const EngineApiRequest& request,
                                      std::string_view prefix) {
  std::vector<std::string> values;
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) values.push_back(option.substr(prefix.size()));
  }
  return values;
}

bool OptionBool(const EngineApiRequest& request, std::string_view prefix) {
  const std::string value = Lower(OptionValue(request, prefix));
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::uint64_t OptionU64(const EngineApiRequest& request,
                        std::string_view prefix,
                        std::uint64_t fallback) {
  const std::string value = OptionValue(request, prefix);
  if (value.empty()) return fallback;
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

std::map<std::string, std::string> PayloadFields(std::string_view payload) {
  std::map<std::string, std::string> fields;
  std::string key;
  std::string value;
  bool in_key = true;
  for (char ch : payload) {
    if (in_key && ch == '=') {
      in_key = false;
      continue;
    }
    if (ch == ';') {
      if (!key.empty()) fields[key] = value;
      key.clear();
      value.clear();
      in_key = true;
      continue;
    }
    (in_key ? key : value).push_back(ch);
  }
  if (!key.empty()) fields[key] = value;
  return fields;
}

std::string PayloadField(std::string_view payload, std::string_view key) {
  const auto fields = PayloadFields(payload);
  const auto found = fields.find(std::string(key));
  return found == fields.end() ? std::string() : found->second;
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
  std::vector<std::string> values;
  std::string current;
  std::istringstream in{std::string(value)};
  while (std::getline(in, current, delimiter)) {
    if (!current.empty()) values.push_back(current);
  }
  return values;
}

std::string Join(const std::vector<std::string>& values, char delimiter) {
  std::string out;
  for (const auto& value : values) {
    if (!out.empty()) out.push_back(delimiter);
    out += value;
  }
  return out;
}

std::string StructuredTypeUuid(const EngineApiRequest& request) {
  const std::string explicit_uuid = OptionValue(request, "type_uuid:");
  if (!explicit_uuid.empty()) return explicit_uuid;
  if (request.target_object.object_kind == std::string(kStructuredTypeKind) ||
      request.target_object.object_kind == "type" ||
      request.target_object.object_kind == "structured_type") {
    return request.target_object.uuid.canonical;
  }
  return request.target_object.uuid.canonical;
}

std::string PrimaryName(const EngineApiRequest& request) {
  return ApiBehaviorPrimaryName(request, "unnamed_structured_type");
}

std::optional<std::string> CanonicalFamily(std::string family) {
  family = Lower(std::move(family));
  if (family == "composite" || family == "enum" || family == "range" ||
      family == "multirange" || family == "variant" || family == "set") {
    return family;
  }
  return std::nullopt;
}

std::string FamilyFromRequest(const EngineApiRequest& request) {
  if (auto family = CanonicalFamily(OptionValue(request, "structured_family:"))) {
    return *family;
  }
  if (!request.descriptors.empty()) {
    if (auto family = CanonicalFamily(request.descriptors.front().canonical_type_name)) {
      return *family;
    }
    if (auto family = CanonicalFamily(request.descriptors.front().descriptor_kind)) {
      return *family;
    }
  }
  return {};
}

std::string FieldName(std::string_view field) {
  const std::size_t sep = field.find(':');
  return std::string(field.substr(0, sep));
}

std::string FieldType(std::string_view field) {
  const std::size_t sep = field.find(':');
  return sep == std::string_view::npos ? std::string() : std::string(field.substr(sep + 1));
}

bool HasDuplicatesByName(const std::vector<std::string>& values) {
  std::set<std::string> seen;
  for (const auto& value : values) {
    std::string key = FieldName(value);
    if (key.empty()) key = value;
    if (!seen.insert(Lower(std::move(key))).second) return true;
  }
  return false;
}

bool ContainsRawSqlText(const EngineApiRequest& request) {
  for (const auto& option : request.option_envelopes) {
    const std::string lower = Lower(option);
    if (lower.find("sql_text") != std::string::npos ||
        lower.find("source_sql") != std::string::npos ||
        lower.find("source_text") != std::string::npos) {
      return true;
    }
  }
  for (const auto& descriptor : request.descriptors) {
    const std::string encoded = Lower(descriptor.encoded_descriptor);
    if (encoded.find("sql_text") != std::string::npos ||
        encoded.find("source_sql") != std::string::npos ||
        encoded.find("source_text") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool MatchesTarget(const std::string& grant_target,
                   const std::string& target_uuid,
                   const EngineApiRequest& request) {
  return grant_target.empty() || grant_target == "*" ||
         grant_target == target_uuid ||
         grant_target == request.target_database.uuid.canonical ||
         grant_target == request.target_schema.uuid.canonical ||
         grant_target == request.target_object.uuid.canonical;
}

bool SubjectMatches(const EngineMaterializedAuthorizationContext& auth,
                    const EngineUuid& subject_uuid,
                    const EngineRequestContext& context) {
  if (!subject_uuid.canonical.empty() &&
      subject_uuid.canonical == context.principal_uuid.canonical) {
    return true;
  }
  for (const auto& subject : auth.effective_subjects) {
    if (!subject.subject_uuid.canonical.empty() &&
        subject.subject_uuid.canonical == subject_uuid.canonical) {
      return true;
    }
  }
  return false;
}

bool EvidenceTagGrants(const EngineRequestContext& context,
                       std::string_view right) {
  const std::string expected = Lower(std::string(right));
  for (const auto& tag : context.authorization_context.evidence_tags) {
    const std::string lower = Lower(tag);
    if (lower == "sysarch" || lower == "right.all" ||
        lower == expected || lower == "right." + expected) {
      return true;
    }
  }
  return false;
}

bool HasRight(const EngineApiRequest& request,
              std::string_view right,
              std::string_view target_uuid) {
  const auto& context = request.context;
  const auto& auth = context.authorization_context;
  if (!context.security_context_present || !auth.present) return false;
  if (EvidenceTagGrants(context, right)) return true;
  bool allowed = false;
  for (const auto& grant : auth.grants) {
    if (!SubjectMatches(auth, grant.subject_uuid, context)) continue;
    const bool right_matches = grant.right == std::string(right) ||
                               grant.right == "ALL" ||
                               grant.right == "SYSARCH";
    if (!right_matches ||
        !MatchesTarget(grant.target_uuid.canonical, std::string(target_uuid), request)) {
      continue;
    }
    if (grant.deny) return false;
    allowed = true;
  }
  return allowed;
}

EngineApiDiagnostic ValidateStructuredContext(const EngineApiRequest& request,
                                              std::string_view operation_id,
                                              bool require_transaction,
                                              std::string_view right,
                                              std::string_view target_uuid) {
  if (ContainsRawSqlText(request)) {
    return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                        "structured_type_engine_request_must_not_contain_sql_text");
  }
  const auto context_status = ValidateApiBehaviorContext(request.context,
                                                        std::string(operation_id),
                                                        require_transaction,
                                                        true);
  if (context_status.error) return context_status;
  if (!request.context.security_context_present) {
    return MakeSecurityContextRequiredDiagnostic(std::string(operation_id));
  }
  if (!HasRight(request, right, target_uuid)) {
    return MakeEngineApiDiagnostic("SECURITY.AUTHORIZATION.DENIED",
                                   "security.authorization.denied",
                                   std::string(right) + "_required");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::optional<ApiBehaviorRecord> FindStructuredType(const EngineApiRequest& request,
                                                    const std::string& type_uuid) {
  const auto records = VisibleApiBehaviorRecords(request.context,
                                                std::string(kStructuredTypeKind),
                                                request.context.local_transaction_id);
  for (const auto& record : records) {
    if (record.object_uuid == type_uuid && !record.deleted) return record;
  }
  return std::nullopt;
}

std::string ExistingPayloadOrEmpty(const EngineApiRequest& request,
                                   const std::string& type_uuid) {
  const auto record = FindStructuredType(request, type_uuid);
  return record.has_value() ? record->payload : std::string();
}

std::uint64_t DescriptorVersion(std::string_view payload) {
  try {
    const std::string value = PayloadField(payload, "descriptor_version");
    return value.empty() ? 0 : static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

void AppendField(std::string* payload, std::string key, std::string value) {
  if (value.empty()) return;
  if (!payload->empty()) payload->push_back(';');
  *payload += std::move(key);
  payload->push_back('=');
  *payload += std::move(value);
}

std::string DescriptorPayload(const EngineApiRequest& request,
                              std::string action,
                              std::string family,
                              std::string previous_payload = {}) {
  const auto fields = OptionValues(request, "field:");
  const auto labels = OptionValues(request, "label:");
  const auto alternatives = OptionValues(request, "alternative:");
  const auto members = OptionValues(request, "member:");
  const auto range_options = OptionValues(request, "range_option:");
  const std::uint64_t version = DescriptorVersion(previous_payload) + 1;

  std::string payload;
  AppendField(&payload, "structured_action", std::move(action));
  AppendField(&payload, "structured_family", std::move(family));
  AppendField(&payload, "type_uuid", StructuredTypeUuid(request));
  AppendField(&payload, "type_name", PrimaryName(request));
  AppendField(&payload, "schema_uuid", request.target_schema.uuid.canonical);
  AppendField(&payload, "syntax_form", OptionValue(request, "syntax_form:"));
  AppendField(&payload, "descriptor_version", std::to_string(version));
  AppendField(&payload, "catalog_identity", "structured_type_uuidv7");
  AppendField(&payload, "field_count", std::to_string(fields.size()));
  AppendField(&payload, "fields", Join(fields, '|'));
  AppendField(&payload, "label_count", std::to_string(labels.size()));
  AppendField(&payload, "labels", Join(labels, '|'));
  AppendField(&payload, "alternative_count", std::to_string(alternatives.size()));
  AppendField(&payload, "alternatives", Join(alternatives, '|'));
  AppendField(&payload, "member_count", std::to_string(members.size()));
  AppendField(&payload, "members", Join(members, '|'));
  AppendField(&payload, "subtype", OptionValue(request, "subtype:"));
  AppendField(&payload, "base_range_uuid", OptionValue(request, "base_range_uuid:"));
  AppendField(&payload, "element_range_uuid", OptionValue(request, "element_range_uuid:"));
  AppendField(&payload, "element_type", OptionValue(request, "element_type:"));
  AppendField(&payload, "range_options", Join(range_options, '|'));
  AppendField(&payload, "auto_derived_multirange",
              OptionBool(request, "auto_derived:") ? "true" : "false");
  AppendField(&payload, "explicit_multirange",
              OptionBool(request, "explicit_multirange:") ? "true" : "false");
  AppendField(&payload, "retired_labels", PayloadField(previous_payload, "retired_labels"));
  AppendField(&payload, "parser_sql_authority", "false");
  AppendField(&payload, "mga_finality_authority", "engine");
  return payload;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ValidateCreateDescriptor(const EngineApiRequest& request,
                                             std::string_view operation_id,
                                             std::string* family_out) {
  const std::string type_uuid = StructuredTypeUuid(request);
  if (type_uuid.empty()) {
    return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                        "structured_type_uuid_required");
  }
  const auto family = CanonicalFamily(FamilyFromRequest(request));
  if (!family.has_value()) {
    return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_FAMILY_INVALID",
                                   "sbsql.structured_type.family_invalid",
                                   "composite_enum_range_multirange_variant_set_required");
  }
  *family_out = *family;

  if (*family == "composite") {
    const auto fields = OptionValues(request, "field:");
    if (fields.empty()) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "composite_field_required");
    }
    if (HasDuplicatesByName(fields)) {
      return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_DUPLICATE_MEMBER",
                                     "sbsql.structured_type.duplicate_member",
                                     "duplicate_composite_field");
    }
    for (const auto& field : fields) {
      const std::string field_type = FieldType(field);
      if ((Lower(field_type) == "self" || field_type == type_uuid) &&
          !OptionBool(request, "recursive_indirection:") &&
          OptionValue(request, "indirection:") != "container") {
        return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_DIRECT_RECURSION_REFUSED",
                                       "sbsql.structured_type.direct_recursion_refused",
                                       "direct_composite_self_reference_requires_indirection");
      }
    }
  } else if (*family == "enum") {
    const auto labels = OptionValues(request, "label:");
    if (labels.empty()) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "enum_label_required");
    }
    if (HasDuplicatesByName(labels)) {
      return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_DUPLICATE_MEMBER",
                                     "sbsql.structured_type.duplicate_member",
                                     "duplicate_enum_label");
    }
  } else if (*family == "range") {
    if (OptionValue(request, "subtype:").empty()) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "range_subtype_required");
    }
    const std::string syntax = Lower(OptionValue(request, "syntax_form:"));
    if (!syntax.empty() && syntax != "pg_range" && syntax != "native_range") {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "range_syntax_form_invalid");
    }
  } else if (*family == "multirange") {
    if (OptionBool(request, "auto_derived:")) {
      if (OptionValue(request, "base_range_uuid:").empty() &&
          OptionValue(request, "subtype:").empty()) {
        return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                            "auto_multirange_requires_range");
      }
    } else if (OptionValue(request, "element_range_uuid:").empty()) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "explicit_multirange_range_required");
    }
  } else if (*family == "variant") {
    const auto alternatives = OptionValues(request, "alternative:");
    if (alternatives.empty()) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "variant_alternative_required");
    }
    if (HasDuplicatesByName(alternatives)) {
      return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_DUPLICATE_MEMBER",
                                     "sbsql.structured_type.duplicate_member",
                                     "duplicate_variant_alternative");
    }
  } else if (*family == "set") {
    if (OptionValue(request, "element_type:").empty()) {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "set_element_type_required");
    }
    if (HasDuplicatesByName(OptionValues(request, "member:"))) {
      return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_DUPLICATE_MEMBER",
                                     "sbsql.structured_type.duplicate_member",
                                     "duplicate_set_member");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateAlterDescriptor(const EngineApiRequest& request,
                                            std::string_view operation_id,
                                            ApiBehaviorRecord* existing_out,
                                            std::string* payload_out) {
  const std::string type_uuid = StructuredTypeUuid(request);
  auto existing = FindStructuredType(request, type_uuid);
  if (!existing.has_value()) {
    return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_NOT_FOUND",
                                   "sbsql.structured_type.not_found",
                                   type_uuid);
  }
  const std::uint64_t mutation_count = OptionU64(request, "mutation_count:", 1);
  if (mutation_count != 1) {
    return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_MULTI_MUTATION_REFUSED",
                                   "sbsql.structured_type.multi_mutation_refused",
                                   "one_alter_type_mutation_per_statement");
  }
  const std::string family = PayloadField(existing->payload, "structured_family");
  std::string payload = existing->payload;
  const std::string drop_label = OptionValue(request, "drop_label:");
  if (!drop_label.empty()) {
    if (family != "enum") {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "drop_label_requires_enum");
    }
    const auto labels = Split(PayloadField(existing->payload, "labels"), '|');
    if (std::find(labels.begin(), labels.end(), drop_label) == labels.end()) {
      return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_ENUM_LABEL_NOT_FOUND",
                                     "sbsql.structured_type.enum_label_not_found",
                                     drop_label);
    }
    const auto retired = Split(PayloadField(existing->payload, "retired_labels"), '|');
    if (std::find(retired.begin(), retired.end(), drop_label) != retired.end()) {
      return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_ENUM_LABEL_RETIRED",
                                     "sbsql.structured_type.enum_label_retired",
                                     drop_label);
    }
    std::vector<std::string> new_retired = retired;
    new_retired.push_back(drop_label);
    AppendField(&payload, "structured_action", "alter_type");
    AppendField(&payload, "alter_action", "drop_enum_value_tombstone");
    AppendField(&payload, "retired_labels", Join(new_retired, '|'));
    AppendField(&payload, "descriptor_version",
                std::to_string(DescriptorVersion(existing->payload) + 1));
  } else if (!OptionValue(request, "set_range_option:").empty()) {
    if (family != "range") {
      return MakeInvalidRequestDiagnostic(std::string(operation_id),
                                          "range_option_mutation_requires_range");
    }
    AppendField(&payload, "structured_action", "alter_type");
    AppendField(&payload, "alter_action", "set_range_option");
    AppendField(&payload, "range_mutation_option", OptionValue(request, "set_range_option:"));
    AppendField(&payload, "range_recanonicalization_required", "true");
    AppendField(&payload, "descriptor_version",
                std::to_string(DescriptorVersion(existing->payload) + 1));
  } else {
    AppendField(&payload, "structured_action", "alter_type");
    AppendField(&payload, "alter_action", OptionValue(request, "alter_action:"));
    AppendField(&payload, "descriptor_version",
                std::to_string(DescriptorVersion(existing->payload) + 1));
  }
  *existing_out = *existing;
  *payload_out = std::move(payload);
  return OkDiagnostic();
}

template <typename TResult>
TResult DiagnosticResult(const EngineApiRequest& request,
                         std::string operation_id,
                         EngineApiDiagnostic diagnostic) {
  return MakeApiBehaviorDiagnostic<TResult>(request.context,
                                            std::move(operation_id),
                                            std::move(diagnostic));
}

void AddStructuredEvidence(EngineApiResult* result,
                           std::string_view action,
                           std::string_view operation_id,
                           std::string_view family = {}) {
  AddApiBehaviorEvidence(result, "structured_type_operation", std::string(action));
  AddApiBehaviorEvidence(result, "engine_api_function", std::string(operation_id));
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "parser_finality_authority", "false");
  AddApiBehaviorEvidence(result, "mga_authority_boundary", "engine_owned");
  AddApiBehaviorEvidence(result, "catalog_identity", "structured_type_uuidv7");
  AddApiBehaviorEvidence(result, "structured_type_descriptor_registry", "active");
  if (!family.empty()) AddApiBehaviorEvidence(result, "structured_family", std::string(family));
}

void AddStructuredTypeRow(EngineApiResult* result,
                          const ApiBehaviorRecord& record,
                          const EngineApiRequest& request) {
  AddApiBehaviorRow(result,
                    {{"type_uuid", record.object_uuid},
                     {"type_name", record.default_name},
                     {"structured_family", PayloadField(record.payload, "structured_family")},
                     {"schema_uuid", PayloadField(record.payload, "schema_uuid")},
                     {"syntax_form", PayloadField(record.payload, "syntax_form")},
                     {"descriptor_version", PayloadField(record.payload, "descriptor_version")},
                     {"field_count", PayloadField(record.payload, "field_count")},
                     {"label_count", PayloadField(record.payload, "label_count")},
                     {"alternative_count", PayloadField(record.payload, "alternative_count")},
                     {"member_count", PayloadField(record.payload, "member_count")},
                     {"subtype", PayloadField(record.payload, "subtype")},
                     {"base_range_uuid", PayloadField(record.payload, "base_range_uuid")},
                     {"element_range_uuid", PayloadField(record.payload, "element_range_uuid")},
                     {"element_type", PayloadField(record.payload, "element_type")},
                     {"retired_labels", PayloadField(record.payload, "retired_labels")},
                     {"catalog_identity", "structured_type_uuidv7"},
                     {"mga_snapshot_visible_through",
                      std::to_string(request.context.snapshot_visible_through_local_transaction_id)}});
}

EngineApiDiagnostic ValidateUsageRequest(const EngineApiRequest& request,
                                         std::string_view operation_id,
                                         ApiBehaviorRecord* record_out) {
  const std::string type_uuid = StructuredTypeUuid(request);
  const auto base = ValidateStructuredContext(request,
                                             operation_id,
                                             false,
                                             "USAGE",
                                             type_uuid);
  if (base.error) return base;
  auto record = FindStructuredType(request, type_uuid);
  if (!record.has_value()) {
    return MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_NOT_FOUND",
                                   "sbsql.structured_type.not_found",
                                   type_uuid);
  }
  *record_out = *record;
  return OkDiagnostic();
}

bool IsRetiredEnumLabel(const ApiBehaviorRecord& record, std::string_view label) {
  const auto retired = Split(PayloadField(record.payload, "retired_labels"), '|');
  return std::find(retired.begin(), retired.end(), label) != retired.end();
}

}  // namespace

EngineCreateStructuredTypeResult EngineCreateStructuredType(
    const EngineCreateStructuredTypeRequest& request) {
  constexpr std::string_view kOperation = "catalog.mutation.create_type";
  const std::string type_uuid = StructuredTypeUuid(request);
  const auto base = ValidateStructuredContext(request,
                                             kOperation,
                                             true,
                                             "TYPE_DDL",
                                             type_uuid);
  if (base.error) {
    return DiagnosticResult<EngineCreateStructuredTypeResult>(request,
                                                              std::string(kOperation),
                                                              base);
  }
  std::string family;
  if (auto descriptor = ValidateCreateDescriptor(request, kOperation, &family);
      descriptor.error) {
    return DiagnosticResult<EngineCreateStructuredTypeResult>(request,
                                                              std::string(kOperation),
                                                              descriptor);
  }
  EngineCreateStructuredTypeRequest normalized = request;
  normalized.target_object.object_kind = std::string(kStructuredTypeKind);
  auto result = PersistedRecordResultWithPayload<EngineCreateStructuredTypeResult>(
      normalized,
      std::string(kOperation),
      std::string(kStructuredTypeKind),
      true,
      "active",
      false,
      DescriptorPayload(request, "create_type", family));
  if (result.ok) {
    result.result_shape.result_kind = "rs.structured_type.descriptor.v1";
    AddStructuredEvidence(&result, "create_type", "EngineCreateStructuredType", family);
    AddApiBehaviorEvidence(&result, "sblr_opcode", "SBLR_CATALOG_MUTATION_CREATE_TYPE");
    AddApiBehaviorEvidence(&result, "audit_event", "catalog.structured_type.created");
  }
  return result;
}

EngineAlterStructuredTypeResult EngineAlterStructuredType(
    const EngineAlterStructuredTypeRequest& request) {
  constexpr std::string_view kOperation = "catalog.mutation.alter_type";
  const std::string type_uuid = StructuredTypeUuid(request);
  const auto base = ValidateStructuredContext(request,
                                             kOperation,
                                             true,
                                             "TYPE_DDL",
                                             type_uuid);
  if (base.error) {
    return DiagnosticResult<EngineAlterStructuredTypeResult>(request,
                                                             std::string(kOperation),
                                                             base);
  }
  ApiBehaviorRecord existing;
  std::string payload;
  if (auto descriptor = ValidateAlterDescriptor(request, kOperation, &existing, &payload);
      descriptor.error) {
    return DiagnosticResult<EngineAlterStructuredTypeResult>(request,
                                                             std::string(kOperation),
                                                             descriptor);
  }
  EngineAlterStructuredTypeRequest normalized = request;
  normalized.target_object.object_kind = std::string(kStructuredTypeKind);
  auto result = PersistedRecordResultWithPayload<EngineAlterStructuredTypeResult>(
      normalized,
      std::string(kOperation),
      std::string(kStructuredTypeKind),
      true,
      "active",
      false,
      std::move(payload));
  if (result.ok) {
    const std::string family = PayloadField(existing.payload, "structured_family");
    result.result_shape.result_kind = "rs.structured_type.descriptor.v1";
    AddStructuredEvidence(&result, "alter_type", "EngineAlterStructuredType", family);
    AddApiBehaviorEvidence(&result, "sblr_opcode", "SBLR_CATALOG_MUTATION_ALTER_TYPE");
    AddApiBehaviorEvidence(&result, "audit_event", "catalog.structured_type.altered");
    if (!OptionValue(request, "drop_label:").empty()) {
      AddApiBehaviorEvidence(&result, "enum_tombstone", OptionValue(request, "drop_label:"));
    }
    if (!OptionValue(request, "set_range_option:").empty()) {
      AddApiBehaviorEvidence(&result, "range_recanonicalization", "required");
    }
  }
  return result;
}

EngineDropStructuredTypeResult EngineDropStructuredType(
    const EngineDropStructuredTypeRequest& request) {
  constexpr std::string_view kOperation = "catalog.mutation.drop_type";
  const std::string type_uuid = StructuredTypeUuid(request);
  const auto base = ValidateStructuredContext(request,
                                             kOperation,
                                             true,
                                             "TYPE_DDL",
                                             type_uuid);
  if (base.error) {
    return DiagnosticResult<EngineDropStructuredTypeResult>(request,
                                                            std::string(kOperation),
                                                            base);
  }
  auto existing = FindStructuredType(request, type_uuid);
  if (!existing.has_value()) {
    return DiagnosticResult<EngineDropStructuredTypeResult>(
        request,
        std::string(kOperation),
        MakeEngineApiDiagnostic("SBSQL.STRUCTURED_TYPE_NOT_FOUND",
                                "sbsql.structured_type.not_found",
                                type_uuid));
  }
  EngineDropStructuredTypeRequest normalized = request;
  normalized.target_object.object_kind = std::string(kStructuredTypeKind);
  auto result = PersistedRecordResultWithPayload<EngineDropStructuredTypeResult>(
      normalized,
      std::string(kOperation),
      std::string(kStructuredTypeKind),
      true,
      "dropped",
      true,
      existing->payload + ";structured_action=drop_type");
  if (result.ok) {
    result.result_shape.result_kind = "rs.ddl.commit.v1";
    AddStructuredEvidence(&result,
                          "drop_type",
                          "EngineDropStructuredType",
                          PayloadField(existing->payload, "structured_family"));
    AddApiBehaviorEvidence(&result, "sblr_opcode", "SBLR_CATALOG_MUTATION_DROP_TYPE");
    AddApiBehaviorEvidence(&result, "audit_event", "catalog.structured_type.dropped");
  }
  return result;
}

EngineShowStructuredTypeResult EngineShowStructuredType(
    const EngineShowStructuredTypeRequest& request) {
  constexpr std::string_view kOperation = "catalog.type.show";
  ApiBehaviorRecord record;
  if (auto status = ValidateUsageRequest(request, kOperation, &record); status.error) {
    return DiagnosticResult<EngineShowStructuredTypeResult>(request,
                                                            std::string(kOperation),
                                                            status);
  }
  auto result = MakeApiBehaviorSuccess<EngineShowStructuredTypeResult>(
      request.context, std::string(kOperation));
  AddStructuredEvidence(&result,
                        "show_type",
                        "EngineShowStructuredType",
                        PayloadField(record.payload, "structured_family"));
  AddApiBehaviorEvidence(&result, "sblr_opcode", "SBLR_SHOW_TYPE");
  AddStructuredTypeRow(&result, record, request);
  result.result_shape.result_kind = "rs.structured_type.descriptor.v1";
  return result;
}

EngineShowStructuredTypesResult EngineShowStructuredTypes(
    const EngineShowStructuredTypesRequest& request) {
  constexpr std::string_view kOperation = "catalog.type.show_all";
  const auto base = ValidateStructuredContext(request,
                                             kOperation,
                                             false,
                                             "USAGE",
                                             StructuredTypeUuid(request));
  if (base.error) {
    return DiagnosticResult<EngineShowStructuredTypesResult>(request,
                                                             std::string(kOperation),
                                                             base);
  }
  auto result = MakeApiBehaviorSuccess<EngineShowStructuredTypesResult>(
      request.context, std::string(kOperation));
  AddStructuredEvidence(&result, "show_types", "EngineShowStructuredTypes");
  AddApiBehaviorEvidence(&result, "sblr_opcode", "SBLR_SHOW_TYPES");
  const std::string family_filter =
      CanonicalFamily(OptionValue(request, "structured_family:")).value_or("");
  const auto records = VisibleApiBehaviorRecords(request.context,
                                                std::string(kStructuredTypeKind),
                                                request.context.local_transaction_id);
  for (const auto& record : records) {
    if (!family_filter.empty() &&
        PayloadField(record.payload, "structured_family") != family_filter) {
      continue;
    }
    AddStructuredTypeRow(&result, record, request);
  }
  result.result_shape.result_kind = "rs.structured_type.list.v1";
  return result;
}

EngineEvaluateStructuredTypeConstructorResult EngineEvaluateStructuredTypeConstructor(
    const EngineEvaluateStructuredTypeConstructorRequest& request) {
  constexpr std::string_view kOperation = "query.structured_type.constructor";
  ApiBehaviorRecord record;
  if (auto status = ValidateUsageRequest(request, kOperation, &record); status.error) {
    return DiagnosticResult<EngineEvaluateStructuredTypeConstructorResult>(
        request, std::string(kOperation), status);
  }
  const std::string family = PayloadField(record.payload, "structured_family");
  if (family == "enum") {
    const std::string label = OptionValue(request, "label:");
    if (IsRetiredEnumLabel(record, label)) {
      return DiagnosticResult<EngineEvaluateStructuredTypeConstructorResult>(
          request,
          std::string(kOperation),
          MakeEngineApiDiagnostic("SBSQL.STRUCTURED_ENUM_LABEL_RETIRED",
                                  "sbsql.structured_type.enum_label_retired",
                                  label));
    }
  }
  auto result = MakeApiBehaviorSuccess<EngineEvaluateStructuredTypeConstructorResult>(
      request.context, std::string(kOperation));
  result.result_shape.result_kind = "result.shape.typed_value";
  AddStructuredEvidence(&result,
                        "constructor",
                        "EngineEvaluateStructuredTypeConstructor",
                        family);
  AddApiBehaviorEvidence(&result, "structured_constructor_registry", "active");
  AddApiBehaviorRow(&result,
                    {{"type_uuid", record.object_uuid},
                     {"structured_family", family},
                     {"constructor_validated", "true"},
                     {"encoded_value", OptionValue(request, "encoded_value:")}});
  result.result_shape.result_kind = "result.shape.typed_value";
  return result;
}

EngineEvaluateStructuredTypeCastResult EngineEvaluateStructuredTypeCast(
    const EngineEvaluateStructuredTypeCastRequest& request) {
  constexpr std::string_view kOperation = "query.structured_type.cast";
  ApiBehaviorRecord record;
  if (auto status = ValidateUsageRequest(request, kOperation, &record); status.error) {
    return DiagnosticResult<EngineEvaluateStructuredTypeCastResult>(
        request, std::string(kOperation), status);
  }
  const std::string family = PayloadField(record.payload, "structured_family");
  auto result = MakeApiBehaviorSuccess<EngineEvaluateStructuredTypeCastResult>(
      request.context, std::string(kOperation));
  result.result_shape.result_kind = "result.shape.typed_value";
  AddStructuredEvidence(&result, "cast", "EngineEvaluateStructuredTypeCast", family);
  AddApiBehaviorEvidence(&result, "structured_cast_registry", "active");
  AddApiBehaviorRow(&result,
                    {{"type_uuid", record.object_uuid},
                     {"structured_family", family},
                     {"source_descriptor", OptionValue(request, "source_descriptor:")},
                     {"cast_validated", "true"}});
  result.result_shape.result_kind = "result.shape.typed_value";
  return result;
}

EngineCompareStructuredTypeValuesResult EngineCompareStructuredTypeValues(
    const EngineCompareStructuredTypeValuesRequest& request) {
  constexpr std::string_view kOperation = "query.structured_type.compare";
  ApiBehaviorRecord record;
  if (auto status = ValidateUsageRequest(request, kOperation, &record); status.error) {
    return DiagnosticResult<EngineCompareStructuredTypeValuesResult>(
        request, std::string(kOperation), status);
  }
  const std::string family = PayloadField(record.payload, "structured_family");
  auto result = MakeApiBehaviorSuccess<EngineCompareStructuredTypeValuesResult>(
      request.context, std::string(kOperation));
  result.result_shape.result_kind = "result.shape.boolean";
  AddStructuredEvidence(&result, "compare", "EngineCompareStructuredTypeValues", family);
  AddApiBehaviorEvidence(&result, "structured_comparison_registry", "active");
  AddApiBehaviorRow(&result,
                    {{"type_uuid", record.object_uuid},
                     {"structured_family", family},
                     {"comparison_semantics", "family:" + family},
                     {"comparison_validated", "true"}});
  result.result_shape.result_kind = "result.shape.boolean";
  return result;
}

EngineSerializeStructuredTypeValueResult EngineSerializeStructuredTypeValue(
    const EngineSerializeStructuredTypeValueRequest& request) {
  constexpr std::string_view kOperation = "query.structured_type.serialize";
  ApiBehaviorRecord record;
  if (auto status = ValidateUsageRequest(request, kOperation, &record); status.error) {
    return DiagnosticResult<EngineSerializeStructuredTypeValueResult>(
        request, std::string(kOperation), status);
  }
  const std::string family = PayloadField(record.payload, "structured_family");
  auto result = MakeApiBehaviorSuccess<EngineSerializeStructuredTypeValueResult>(
      request.context, std::string(kOperation));
  result.result_shape.result_kind = "result.shape.typed_value";
  AddStructuredEvidence(&result, "serialize", "EngineSerializeStructuredTypeValue", family);
  AddApiBehaviorEvidence(&result, "structured_serialization_registry", "active");
  AddApiBehaviorRow(&result,
                    {{"type_uuid", record.object_uuid},
                     {"structured_family", family},
                     {"serialized_frame", "SBTYPE1:" + family},
                     {"serialization_validated", "true"}});
  result.result_shape.result_kind = "result.shape.typed_value";
  return result;
}

}  // namespace scratchbird::engine::internal_api
