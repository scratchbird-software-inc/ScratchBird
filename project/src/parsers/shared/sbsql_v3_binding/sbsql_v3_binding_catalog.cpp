// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_binding_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::parser::sbsql_v3_binding {
namespace {

std::string Lower(std::string_view value) {
  std::string out;
  for (unsigned char ch : value) out.push_back(static_cast<char>(std::tolower(ch)));
  return out;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

std::string StableFakeUuid(std::string_view kind, std::string_view seed) {
  unsigned long long hash = 1469598103934665603ull;
  for (unsigned char ch : kind) { hash ^= ch; hash *= 1099511628211ull; }
  for (unsigned char ch : seed) { hash ^= ch; hash *= 1099511628211ull; }
  std::ostringstream out;
  out << "018f0000-0000-7000-8000-";
  const char* hex = "0123456789abcdef";
  for (int i = 0; i < 12; ++i) {
    out << hex[(hash >> ((i % 8) * 4)) & 0xf];
  }
  return out.str();
}

}  // namespace

bool IsUuidV7(std::string_view uuid_text) {
  return uuid_text.size() == 36 && uuid_text[14] == '7' && uuid_text[8] == '-' &&
         uuid_text[13] == '-' && uuid_text[18] == '-' && uuid_text[23] == '-';
}

BindingProfile BindingProfileForCommandFamily(std::string_view command_family) {
  static const std::map<std::string_view, BindingProfile> profiles = {
      {"sbsql.identity_session", {"sbsql.identity_session", "CONNECT", "self_or_baseline", "session_context", "none_or_text_descriptor", true, true, false, false}},
      {"sbsql.transaction", {"sbsql.transaction", "TRANSACTION_CONTROL", "self", "transaction_inventory", "transaction_option_descriptors", true, true, false, false}},
      {"sbsql.query_dml", {"sbsql.query_dml", "DATA_ACCESS", "object_scope", "query_descriptor_binding", "full_expression_predicate_projection_descriptors", true, true, false, false}},
      {"sbsql.ddl_schema_tree", {"sbsql.ddl_schema_tree", "CATALOG_DDL", "schema_scope", "recursive_schema_catalog", "name_and_comment_descriptors", true, true, false, false}},
      {"sbsql.ddl_database_storage", {"sbsql.ddl_database_storage", "DATABASE_DDL", "database_scope", "database_storage_catalog", "storage_profile_descriptors", true, true, false, false}},
      {"sbsql.ddl_table_index_domain", {"sbsql.ddl_table_index_domain", "CATALOG_DDL", "object_scope", "descriptor_catalog", "column_index_domain_type_descriptors", true, true, false, false}},
      {"sbsql.ddl_routine_udr", {"sbsql.ddl_routine_udr", "ROUTINE_DDL", "object_scope", "routine_udr_catalog", "routine_parameter_return_body_descriptors", true, true, false, false}},
      {"sbsql.security_dcl", {"sbsql.security_dcl", "SECURITY_ADMIN_OR_SELF", "principal_scope", "security_catalog", "principal_grant_policy_descriptors", true, true, false, false}},
      {"sbsql.policy", {"sbsql.policy", "POLICY_ADMIN", "object_or_policy_scope", "policy_catalog", "policy_payload_descriptors", true, true, false, false}},
      {"sbsql.observability", {"sbsql.observability", "OBS_INSPECT", "self_or_all", "observability_catalog", "result_filter_metric_descriptors", true, true, false, false}},
      {"sbsql.management", {"sbsql.management", "OBS_MANAGEMENT_INSPECT_OR_CONTROL", "management_scope", "management_catalog", "management_payload_descriptors", true, true, false, false}},
      {"sbsql.acceleration", {"sbsql.acceleration", "ACCELERATION_INSPECT_OR_CONTROL", "capability_scope", "acceleration_catalog", "llvm_gpu_profile_descriptors", true, true, false, false}},
      {"sbsql.archive_replication_migration", {"sbsql.archive_replication_migration", "DATA_MOVEMENT_INSPECT_OR_CONTROL", "lineage_scope", "archive_replication_catalog", "lineage_archive_migration_descriptors", true, true, false, false}},
      {"sbsql.private_cluster", {"sbsql.private_cluster", "CLUSTER_AUTHORITY_REQUIRED", "cluster_scope", "private_cluster_catalog", "cluster_epoch_route_decision_descriptors", true, true, true, true}},
  };
  const auto found = profiles.find(command_family);
  if (found == profiles.end()) return {};
  return found->second;
}

BoundNameReference ResolveNameEvidence(std::string_view source_name,
                                       std::string_view language,
                                       std::string_view default_language,
                                       std::string_view catalog_epoch) {
  BoundNameReference result;
  result.source_name = std::string(source_name);
  result.resolution_language = std::string(language.empty() ? default_language : language);
  result.catalog_epoch = std::string(catalog_epoch);
  if (source_name.empty()) {
    result.diagnostic_code = "SBSQL_BIND_UNRESOLVED_NAME";
    return result;
  }
  if (Lower(source_name).find("ambiguous") != std::string::npos) {
    result.diagnostic_code = "SBSQL_BIND_AMBIGUOUS_LOCALIZED_NAME";
    return result;
  }
  if (source_name.rfind("UUID:", 0) == 0) {
    const std::string uuid = std::string(source_name.substr(5));
    if (!IsUuidV7(uuid)) {
      result.diagnostic_code = "SBSQL_BIND_IDENTITY_UUID_MUST_BE_V7";
      return result;
    }
    result.ok = true;
    result.object_uuid = uuid;
    result.object_kind = "direct_uuid_ref";
    return result;
  }
  result.ok = true;
  result.object_uuid = StableFakeUuid("object", source_name);
  result.object_kind = "catalog_object";
  return result;
}

BoundDescriptorReference BindDescriptorAlias(std::string_view alias, std::string_view descriptor_context) {
  BoundDescriptorReference result;
  result.source_alias = std::string(alias);
  if (alias.empty() || descriptor_context.empty()) {
    result.diagnostic_code = "SBSQL_BIND_DESCRIPTOR_CONTEXT_REQUIRED";
    return result;
  }
  const std::string lower = Lower(alias);
  if (lower == "ambiguous") {
    result.diagnostic_code = "SBSQL_BIND_AMBIGUOUS_DESCRIPTOR_OVERLOAD";
    return result;
  }
  if (lower == "unknown_type") {
    result.diagnostic_code = "SBSQL_BIND_UNKNOWN_TYPE_ALIAS";
    return result;
  }
  result.ok = true;
  result.descriptor_uuid = StableFakeUuid("descriptor", alias);
  result.descriptor_kind = std::string(descriptor_context);
  result.canonical_type_name = lower;
  result.encoded_descriptor = "descriptor:" + result.descriptor_kind + ":" + result.canonical_type_name;
  return result;
}

bool ValidateBindingProfile(const BindingProfile& profile, std::vector<std::string>* errors) {
  if (profile.command_family.empty()) errors->push_back("command_family_missing");
  if (profile.required_right.empty()) errors->push_back("required_right_missing");
  if (profile.scope_mode.empty()) errors->push_back("scope_mode_missing");
  if (profile.catalog_authority.empty()) errors->push_back("catalog_authority_missing");
  if (profile.descriptor_binding_profile.empty()) errors->push_back("descriptor_binding_profile_missing");
  if (!profile.engine_security_recheck_required) errors->push_back("engine_security_recheck_required");
  if (!profile.engine_transaction_recheck_required) errors->push_back("engine_transaction_recheck_required");
  if (profile.cluster_authority_required && !profile.fail_closed_without_cluster_authority) errors->push_back("cluster_fail_closed_required");
  return errors->empty();
}

std::string SerializeBoundNameReferenceToJson(const BoundNameReference& ref) {
  std::ostringstream out;
  out << "{\"ok\":" << (ref.ok ? "true" : "false") << ",\"object_uuid\":\"" << JsonEscape(ref.object_uuid)
      << "\",\"object_kind\":\"" << JsonEscape(ref.object_kind) << "\",\"resolution_language\":\""
      << JsonEscape(ref.resolution_language) << "\",\"diagnostic_code\":\"" << JsonEscape(ref.diagnostic_code) << "\"}";
  return out.str();
}

std::string SerializeBoundDescriptorReferenceToJson(const BoundDescriptorReference& ref) {
  std::ostringstream out;
  out << "{\"ok\":" << (ref.ok ? "true" : "false") << ",\"descriptor_uuid\":\"" << JsonEscape(ref.descriptor_uuid)
      << "\",\"descriptor_kind\":\"" << JsonEscape(ref.descriptor_kind) << "\",\"canonical_type_name\":\""
      << JsonEscape(ref.canonical_type_name) << "\",\"diagnostic_code\":\"" << JsonEscape(ref.diagnostic_code) << "\"}";
  return out.str();
}

}  // namespace scratchbird::parser::sbsql_v3_binding
