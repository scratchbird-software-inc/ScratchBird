// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/security_principal_lifecycle.hpp"

#include "api_diagnostics.hpp"
#include "catalog/name_registry.hpp"
#include "database_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/database_local_security_event_store.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_model.hpp"
#include "typed_update_carrier_codec.hpp"
#include "local_transaction_store.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionState;

struct LoadOptions {
  bool enforce_visibility = true;
};

std::string EventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.security_principal_events";
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string UpperAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
  return value;
}

std::string HexEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (const unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

std::string HexEncode(const std::array<std::uint8_t, 32>& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (const auto byte : value) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecode(const std::string& value) {
  if ((value.size() % 2) != 0) { return {}; }
  std::string out;
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

bool HexDecodeSha256(const std::string& value,
                     std::array<std::uint8_t, 32>* out) {
  if (out == nullptr || value.size() != 64) return false;
  for (std::size_t index = 0; index < out->size(); ++index) {
    const int hi = HexValue(value[index * 2]);
    const int lo = HexValue(value[index * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    (*out)[index] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool NonzeroSha256(const std::array<std::uint8_t, 32>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

std::uint64_t ParseU64(const std::string& value) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE";
}

std::string StableToken(std::string_view prefix, std::string_view payload) {
  return std::string(prefix) + ":v1:sha256:" + SecuritySha256Hex(payload);
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic PrincipalDiagnostic(const char* code, std::string detail = {}) {
  std::string key = "security.principal.lifecycle";
  const std::string code_text = code;
  if (code_text == kSecurityPrincipalDiagnosticDatabasePathRequired) {
    key = "security.principal.database_path_required";
  } else if (code_text == kSecurityPrincipalDiagnosticDatabaseWriteFailed) {
    key = "security.principal.database_write_failed";
  } else if (code_text == kSecurityPrincipalDiagnosticMgaTransactionRequired) {
    key = "security.principal.mga_transaction_required";
  } else if (code_text == kSecurityPrincipalDiagnosticAuthorityRequired) {
    key = "security.principal.authority_required";
  } else if (code_text == kSecurityPrincipalDiagnosticAuthorityBypassRefused) {
    key = "security.principal.authority_bypass_refused";
  } else if (code_text == kSecurityPrincipalDiagnosticPrincipalInvalid) {
    key = "security.principal.invalid";
  } else if (code_text == kSecurityPrincipalDiagnosticPrincipalDisabled) {
    key = "security.principal.disabled";
  } else if (code_text == kSecurityPrincipalDiagnosticRoleInvalid) {
    key = "security.role.invalid";
  } else if (code_text == kSecurityPrincipalDiagnosticGroupInvalid) {
    key = "security.group.invalid";
  } else if (code_text == kSecurityPrincipalDiagnosticGrantInvalid) {
    key = "security.grant.invalid";
  } else if (code_text == kSecurityPrincipalDiagnosticAccessDenied) {
    key = "security.access_denied";
  } else if (code_text == kSecurityPrincipalDiagnosticDefaultDeny) {
    key = "security.privilege.default_deny";
  } else if (code_text == kSecurityPrincipalDiagnosticGrantNotVisible) {
    key = "security.privilege.grant_not_visible";
  } else if (code_text == kSecurityPrincipalDiagnosticPolicyMissing) {
    key = "security.policy.missing";
  } else if (code_text == kSecurityPrincipalDiagnosticPolicyDuplicate) {
    key = "security.policy.duplicate";
  } else if (code_text == kSecurityPrincipalDiagnosticPolicyStale) {
    key = "security.policy.stale";
  } else if (code_text == kSecurityPrincipalDiagnosticCacheStale) {
    key = "security.policy.cache_stale";
  } else if (code_text == kSecurityPrincipalDiagnosticCacheMissing) {
    key = "security.policy.cache_missing";
  } else if (code_text == kSecurityPrincipalDiagnosticProtectedMaterialPlaintextRefused) {
    key = "security.protected_material.plaintext_refused";
  } else if (code_text == kSecurityPrincipalDiagnosticAuditEvidenceRequired) {
    key = "security.audit.evidence_required";
  } else if (code_text == kSecurityPrincipalDiagnosticCatalogAuthorityRequired) {
    key = "security.catalog_authority_required";
  }
  return MakeEngineApiDiagnostic(code_text, std::move(key), std::move(detail), true);
}

template <typename TResult>
TResult SuccessResult(const EngineRequestContext& context, std::string operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  return result;
}

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         std::string operation_id,
                         EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

void AddRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields) {
  EngineRowValue row;
  row.requested_row_uuid.canonical =
      "security-row-" + std::to_string(result->result_shape.rows.size() + 1);
  for (auto& field : fields) {
    row.fields.push_back({std::move(field.first), Value(std::move(field.second))});
  }
  result->result_shape.result_kind = "security_principal_lifecycle_rows";
  result->result_shape.rows.push_back(std::move(row));
}

void AddEvidence(EngineApiResult* result, std::string kind, std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

bool EventVisible(const EngineRequestContext& context, std::uint64_t creator_tx) {
  if (creator_tx == 0) { return true; }
  if (context.local_transaction_id != 0 && creator_tx == context.local_transaction_id) { return true; }
  if (context.snapshot_visible_through_local_transaction_id != 0) {
    return creator_tx <= context.snapshot_visible_through_local_transaction_id;
  }
  if (context.local_transaction_id != 0) { return creator_tx <= context.local_transaction_id; }
  return true;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

std::string OperationIdOr(const EngineApiRequest& request, const std::string& fallback) {
  return request.operation_id.empty() ? fallback : request.operation_id;
}

std::string ResultShapeContract(const EngineApiRequest& request, const std::string& fallback) {
  const auto result_shape = OptionValue(request, "result_shape_contract:");
  return result_shape.empty() ? fallback : result_shape;
}

void AddSecurityInspectionOperationResult(EngineApiResult* result,
                                          const EngineApiRequest& request,
                                          const std::string& operation_id,
                                          const std::string& result_shape) {
  AddEvidence(result, "public_sbsql_operation", operation_id);
  AddEvidence(result, "engine_api_function", "EngineSecurityInspectOperation");
  AddEvidence(result, "parser_executes_sql", "false");
  AddEvidence(result, "cluster_provider_dispatch", "false");
  AddEvidence(result, "private_cluster_execution", "false");
  AddEvidence(result, "result_shape_contract", result_shape);
  AddRow(result,
         {{"operation_id", operation_id},
          {"result_shape", result_shape},
          {"route_kind", "security_inspection"},
          {"target_ref_kind", OptionValue(request, "target_ref_kind:")},
          {"target_ref_visible", OptionValue(request, "target_ref:").empty() ? "false" : "true"},
          {"security_epoch", std::to_string(request.context.security_epoch)},
          {"catalog_generation_id", std::to_string(request.context.catalog_generation_id)}});
  result->result_shape.result_kind = result_shape;
}

bool HasTraceTag(const EngineRequestContext& context, const std::string& tag) {
  for (const auto& candidate : context.trace_tags) {
    if (candidate == tag) { return true; }
  }
  return false;
}

bool ContextHasRight(const EngineRequestContext& context, const std::string& right) {
  return SecurityContextHasRight(context, right);
}

EngineApiDiagnostic ValidateEngineAuthorityBoundary(const EngineApiRequest& request,
                                                    const std::string& operation_id) {
  const std::vector<std::string> prefixes = {
      "auth_authority:", "security_authority:", "principal_authority:",
      "role_authority:", "group_authority:", "grant_authority:",
      "row_security_authority:", "policy_authority:",
      "definer_rights_authority:", "authorization_authority:"};
  for (const auto& prefix : prefixes) {
    const std::string value = LowerAscii(OptionValue(request, prefix));
    if (!value.empty() && value != "engine" && value != "engine_internal") {
      return PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityBypassRefused,
                                 operation_id + ":" + prefix + "not_engine");
    }
  }
  for (const auto& tag : request.context.trace_tags) {
    const std::string lower = LowerAscii(tag);
    if (StartsWith(lower, "authority:parser") ||
        StartsWith(lower, "authority:driver") ||
        StartsWith(lower, "authority:reference") ||
        StartsWith(lower, std::string("authority:sql") + "ite")) {
      return PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityBypassRefused,
                                 operation_id + ":non_engine_trace_authority");
    }
  }
  const std::string forbidden_embedded = std::string("sql") + "ite";
  const std::string forbidden_log = std::string("authoritative_") + "wal";
  for (const auto& option : request.option_envelopes) {
    const std::string lower = LowerAscii(option);
    if (StartsWith(lower, "reference_shortcut:") ||
        StartsWith(lower, forbidden_embedded + "_shortcut:") ||
        StartsWith(lower, forbidden_log + ":")) {
      if (lower.find(":true") != std::string::npos || lower.find(":1") != std::string::npos ||
          lower.find(":yes") != std::string::npos || lower.find(":on") != std::string::npos) {
        return PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityBypassRefused,
                                   operation_id + ":non_engine_shortcut_forbidden");
      }
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateSecurityAuthority(const EngineApiRequest& request,
                                              const std::string& operation_id,
                                              const std::string& right) {
  const auto boundary = ValidateEngineAuthorityBoundary(request, operation_id);
  if (boundary.error) { return boundary; }
  if (!request.context.security_context_present) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                               operation_id + ":security_context_required");
  }
  if (!ContextHasRight(request.context, right)) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                               operation_id + ":" + right);
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateMutatingContext(const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticDatabasePathRequired, "database_path");
  }
  if (context.local_transaction_id == 0) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticMgaTransactionRequired,
                               "local_transaction_id");
  }
  return OkDiagnostic();
}

bool ContainsProtectedMaterialMarker(const std::string& text) {
  const std::string lower = LowerAscii(text);
  const std::vector<std::string> markers = {
      "secret", "password", "passwd", "pwd=", "credential", "verifier",
      "private_key", "key_material", "plaintext", "cleartext",
      "protected_material", "bearer ", "token=", "apikey", "api_key"};
  for (const auto& marker : markers) {
    if (lower.find(marker) != std::string::npos) { return true; }
  }
  return false;
}

bool PlaintextCredentialRefused(const EngineSecurityCreatePrincipalRequest& request) {
  const std::string ref = LowerAscii(request.credential_protected_material_ref);
  const std::vector<std::string> refused = {
      "plaintext:", "cleartext:", "password:", "password=", "passwd=",
      "raw_password=", "secret=", "private_key=", "key_material="};
  for (const auto& marker : refused) {
    if (ref.find(marker) != std::string::npos) { return true; }
  }
  for (const auto& option : request.option_envelopes) {
    const std::string lower = LowerAscii(option);
    if (StartsWith(lower, "password:") || StartsWith(lower, "plaintext_password:") ||
        StartsWith(lower, "credential_plaintext:") || StartsWith(lower, "credential_password:") ||
        lower.find("password=") != std::string::npos ||
        lower.find("credential_plaintext") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool PlaintextCredentialRefused(const EngineSecurityAlterPrincipalRequest& request) {
  const std::string ref = LowerAscii(request.credential_protected_material_ref);
  const std::vector<std::string> refused = {
      "plaintext:", "cleartext:", "password:", "password=", "passwd=",
      "raw_password=", "secret=", "private_key=", "key_material="};
  for (const auto& marker : refused) {
    if (ref.find(marker) != std::string::npos) { return true; }
  }
  for (const auto& option : request.option_envelopes) {
    const std::string lower = LowerAscii(option);
    if (StartsWith(lower, "password:") || StartsWith(lower, "plaintext_password:") ||
        StartsWith(lower, "credential_plaintext:") || StartsWith(lower, "credential_password:") ||
        lower.find("password=") != std::string::npos ||
        lower.find("credential_plaintext") != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string PrincipalUuid(const EngineSecurityCreatePrincipalRequest& request) {
  if (!request.principal_uuid.empty()) { return request.principal_uuid; }
  return request.target_object.uuid.canonical;
}

std::string PrincipalUuid(const EngineSecurityAlterPrincipalRequest& request) {
  if (!request.principal_uuid.empty()) { return request.principal_uuid; }
  return request.target_object.uuid.canonical;
}

std::string RoleUuid(const EngineSecurityCreateRoleRequest& request) {
  if (!request.role_uuid.empty()) { return request.role_uuid; }
  return request.target_object.uuid.canonical;
}

bool IsEngineOwnedSysarchRoleUuid(const std::string& uuid) {
  return uuid ==
         scratchbird::storage::database::kCanonicalSysarchRoleObjectUuid;
}

bool IsEngineOwnedBootstrapPrincipal(const EngineRequestContext& context,
                                     const std::string& principal_uuid) {
  if (principal_uuid.empty()) return false;
  const auto identity = ResolveEngineOwnedSysarchRoleIdentity(context);
  return identity.ok && identity.present &&
         identity.principal_uuid == principal_uuid;
}

std::string GroupUuid(const EngineSecurityCreateGroupRequest& request) {
  if (!request.group_uuid.empty()) { return request.group_uuid; }
  return request.target_object.uuid.canonical;
}

std::string PrimaryName(const EngineApiRequest& request, const std::string& explicit_name) {
  if (!explicit_name.empty()) { return explicit_name; }
  for (const auto& name : request.localized_names) {
    if (name.default_name && !name.name.empty()) { return name.name; }
  }
  for (const auto& name : request.localized_names) {
    if (!name.name.empty()) { return name.name; }
  }
  const std::string option_name = OptionValue(request, "name:");
  if (!option_name.empty()) { return option_name; }
  return {};
}

EngineApiDiagnostic PersistSecurityNameAliases(
    const EngineRequestContext& context,
    const std::string& operation_id,
    const std::string& object_uuid,
    const std::vector<std::string>& object_classes,
    const std::vector<EngineLocalizedName>& names,
    const std::string& fallback_name,
    const std::string& scope_uuid = {}) {
  for (const auto& object_class : object_classes) {
    const auto persisted = PersistNameRegistryEntriesForObject(
        context,
        operation_id,
        object_uuid,
        object_class,
        scope_uuid,
        names,
        fallback_name);
    if (persisted.error) { return persisted; }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string NormalizePrivilege(std::string privilege) {
  return UpperAscii(std::move(privilege));
}

bool PrivilegeAllowsGlobalGrant(const std::string& privilege) {
  if (!IsKnownSecurityRight(privilege)) return false;
  return privilege == "CONNECT" || privilege == "SELECT" ||
         privilege.rfind("OBS_", 0) == 0 ||
         privilege.rfind("SEC_", 0) == 0 ||
         privilege.rfind("MGA_", 0) == 0 ||
         privilege.rfind("AUTH_", 0) == 0 ||
         privilege.rfind("AUDIT_", 0) == 0 ||
         privilege.rfind("UDR_", 0) == 0 ||
         privilege.rfind("EVENT_", 0) == 0 ||
         privilege.rfind("BACKUP_", 0) == 0 ||
         privilege == "POLICY_ADMIN" ||
         privilege == "PROTECTED_MATERIAL_RELEASE" ||
         privilege == "KEY_RELEASE_APPROVE" ||
         privilege == "SUPPORT_EXPORT" ||
         privilege == "FILESPACE_LIFECYCLE_CONTROL" ||
         privilege == "MIGRATE_DATABASE" ||
         privilege == "MANAGER_ADMISSION_ADMIN";
}

std::string NormalizePrincipalKind(std::string kind) {
  if (kind.empty()) return {};
  return LowerAscii(std::move(kind));
}

bool PrincipalKindValid(const std::string& kind) {
  return kind == "user" || kind == "service" || kind == "system_actor";
}

std::string NormalizePrincipalLifecycle(std::string state) {
  if (state.empty()) return {};
  state = LowerAscii(std::move(state));
  if (state == "enabled") return "active";
  if (state == "disable") return "disabled";
  return state;
}

bool PrincipalLifecycleValid(const std::string& state) {
  return state == "active" || state == "disabled";
}

std::string NormalizePolicyLifecycle(std::string state) {
  if (state.empty()) return {};
  state = LowerAscii(std::move(state));
  if (state == "enabled") return "active";
  if (state == "disabled") return "inactive";
  return state;
}

bool PolicyLifecycleValid(const std::string& state) {
  return state == "active" || state == "inactive";
}

std::string PrincipalEvent(const EngineSecurityPrincipalRecord& record) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tPRINCIPAL\t" +
         std::to_string(record.creator_tx) + "\t" + record.principal_uuid + "\t" +
         HexEncode(record.principal_name) + "\t" + record.principal_kind + "\t" +
         record.lifecycle_state + "\t" + HexEncode(record.credential_fingerprint) + "\t" +
         std::to_string(record.security_generation) + "\t" +
         (record.deleted ? "1" : "0");
}

std::string RoleEvent(const EngineSecurityRoleRecord& record) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tROLE\t" +
         std::to_string(record.creator_tx) + "\t" + record.role_uuid + "\t" +
         HexEncode(record.role_name) + "\t" + record.owner_principal_uuid + "\t" +
         record.lifecycle_state + "\t" + std::to_string(record.security_generation) + "\t" +
         (record.deleted ? "1" : "0");
}

std::string GroupEvent(const EngineSecurityGroupRecord& record) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tGROUP\t" +
         std::to_string(record.creator_tx) + "\t" + record.group_uuid + "\t" +
         HexEncode(record.group_name) + "\t" + HexEncode(record.external_authority_ref) + "\t" +
         record.lifecycle_state + "\t" + std::to_string(record.security_generation) + "\t" +
         (record.deleted ? "1" : "0");
}

std::string MembershipEvent(const EngineSecurityMembershipRecord& record) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tMEMBERSHIP\t" +
         std::to_string(record.creator_tx) + "\t" + record.membership_uuid + "\t" +
         record.member_principal_uuid + "\t" + record.container_uuid + "\t" +
         record.container_kind + "\t" + record.grantor_principal_uuid + "\t" +
         std::to_string(record.security_generation) + "\t" +
         (record.revoked ? "1" : "0");
}

std::string GrantEvent(const EngineSecurityPrivilegeGrantRecord& record) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tGRANT\t" +
         std::to_string(record.creator_tx) + "\t" + record.grant_uuid + "\t" +
         record.grantee_uuid + "\t" + record.grantee_kind + "\t" +
         record.target_object_uuid + "\t" + record.target_object_kind + "\t" +
         record.privilege + "\t" + record.grantor_principal_uuid + "\t" +
         record.grant_effect + "\t" + std::to_string(record.security_generation) + "\t" +
         (record.revoked ? "1" : "0");
}

std::string RevokeEvent(std::uint64_t creator_tx,
                        const std::string& grantee_uuid,
                        const std::string& target_uuid,
                        const std::string& privilege,
                        const std::string& revoker,
                        std::uint64_t generation) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tREVOKE\t" +
         std::to_string(creator_tx) + "\t" + grantee_uuid + "\t" + target_uuid + "\t" +
         privilege + "\t" + revoker + "\t" + std::to_string(generation);
}

std::string RowPolicyEvent(const EngineSecurityRowPolicyRecord& record) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tROW_POLICY\t" +
         std::to_string(record.creator_tx) + "\t" + record.policy_uuid + "\t" +
         record.target_object_uuid + "\t" + record.target_object_kind + "\t" +
         record.policy_effect + "\t" + HexEncode(record.predicate_envelope) + "\t" +
         record.definer_principal_uuid + "\t" + record.lifecycle_state + "\t" +
         std::to_string(record.policy_generation) + "\t" +
         (record.deleted ? "1" : "0") + "\t" +
         record.policy_version_uuid + "\t" +
         std::to_string(record.effective_transaction_number) + "\t" +
         std::to_string(record.target_object_generation) + "\t" +
         std::to_string(record.update_policy_phase) + "\t" +
         record.effective_policy_uuid + "\t" +
         std::to_string(record.effective_policy_generation) + "\t" +
         record.effective_expression_uuid + "\t" +
         std::to_string(record.effective_expression_generation) + "\t" +
         HexEncode(record.effective_expression_evidence_sha256) + "\t" +
         record.source_expression_uuid + "\t" +
         std::to_string(record.source_expression_generation) + "\t" +
         HexEncode(record.source_expression_evidence_sha256) + "\t" +
         record.source_catalog_snapshot_uuid + "\t" +
         std::to_string(record.source_catalog_generation) + "\t" +
         std::to_string(record.source_security_generation);
}

bool CanonicalNonzeroUuid(std::string_view text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == text;
}

bool NativeRowPolicyAuthorityValid(
    const EngineRequestContext& context,
    const EngineSecurityRowPolicyNativeAuthorityV1& authority) {
  return authority.present && context.local_transaction_id != 0 &&
         context.statement_metadata_snapshot_engine_owned &&
         authority.target_relation_generation != 0 &&
         (authority.phase == 1 || authority.phase == 2) &&
         CanonicalNonzeroUuid(authority.policy_version_uuid) &&
         CanonicalNonzeroUuid(authority.effective_policy_uuid) &&
         authority.effective_policy_generation != 0 &&
         CanonicalNonzeroUuid(authority.effective_expression_uuid) &&
         authority.effective_expression_generation != 0 &&
         NonzeroSha256(authority.effective_expression_evidence_sha256) &&
         CanonicalNonzeroUuid(authority.source_expression_uuid) &&
         authority.source_expression_generation != 0 &&
         NonzeroSha256(authority.source_expression_evidence_sha256) &&
         // Current DUSR catalog/security snapshot bindings are issued when a
         // typed UPDATE snapshot is frozen.  A policy mutation caller may not
         // predeclare those future identities or generations.
         authority.catalog_snapshot_uuid.empty() &&
         authority.catalog_generation == 0 &&
         authority.security_generation == 0;
}

void ApplyNativeRowPolicyAuthority(
    const EngineSecurityRowPolicyNativeAuthorityV1& authority,
    const EngineRequestContext& context,
    std::uint64_t policy_event_generation,
    EngineSecurityRowPolicyRecord* record) {
  record->policy_version_uuid = authority.policy_version_uuid;
  // Final effective-transaction authority does not exist until the owning
  // transaction is committed in the MGA inventory.  Resolver code projects
  // that committed inventory identity; the pending catalog row stores zero.
  record->effective_transaction_number = 0;
  record->target_object_generation = authority.target_relation_generation;
  record->update_policy_phase = authority.phase;
  record->effective_policy_uuid = authority.effective_policy_uuid;
  record->effective_policy_generation = authority.effective_policy_generation;
  record->effective_expression_uuid = authority.effective_expression_uuid;
  record->effective_expression_generation =
      authority.effective_expression_generation;
  record->effective_expression_evidence_sha256 =
      authority.effective_expression_evidence_sha256;
  record->source_expression_uuid = authority.source_expression_uuid;
  record->source_expression_generation =
      authority.source_expression_generation;
  record->source_expression_evidence_sha256 =
      authority.source_expression_evidence_sha256;
  record->source_catalog_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  record->source_catalog_generation = context.catalog_generation_id;
  record->source_security_generation = policy_event_generation;
}

void ClearNativeRowPolicyAuthority(EngineSecurityRowPolicyRecord* record) {
  record->policy_version_uuid.clear();
  record->effective_transaction_number = 0;
  record->target_object_generation = 0;
  record->update_policy_phase = 0;
  record->effective_policy_uuid.clear();
  record->effective_policy_generation = 0;
  record->effective_expression_uuid.clear();
  record->effective_expression_generation = 0;
  record->effective_expression_evidence_sha256.fill(0);
  record->source_expression_uuid.clear();
  record->source_expression_generation = 0;
  record->source_expression_evidence_sha256.fill(0);
  record->source_catalog_snapshot_uuid.clear();
  record->source_catalog_generation = 0;
  record->source_security_generation = 0;
}

std::string DefinerCacheEvent(const EngineSecurityDefinerRightsCacheRecord& record) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tDEFINER_CACHE\t" +
         std::to_string(record.creator_tx) + "\t" + record.cache_key + "\t" +
         record.definer_principal_uuid + "\t" + record.target_object_uuid + "\t" +
         record.privilege + "\t" + record.decision + "\t" +
         std::to_string(record.policy_generation);
}

std::string CacheInvalidationEvent(const EngineRequestContext& context,
                                   const std::string& reason,
                                   const std::string& target_uuid,
                                   std::uint64_t generation) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tCACHE_INVALIDATE\t" +
         std::to_string(context.local_transaction_id) + "\t" + HexEncode(reason) + "\t" +
         target_uuid + "\t" + std::to_string(generation);
}

std::string AuditEvent(const EngineSecurityAuditRecord& record) {
  return std::string(kSecurityPrincipalLifecycleEventMagic) + "\tAUDIT\t" +
         std::to_string(record.creator_tx) + "\t" + record.audit_uuid + "\t" +
         HexEncode(record.operation_id) + "\t" + record.actor_principal_uuid + "\t" +
         record.target_uuid + "\t" + record.outcome + "\t" +
         HexEncode(record.redacted_detail) + "\t" +
         std::to_string(record.security_generation);
}

std::mutex g_security_lifecycle_event_append_mutex;

bool AdvancesAuthorizationContext(const std::string& event) {
  static constexpr std::array<std::string_view, 7> kAuthorityEvents = {
      "PRINCIPAL", "ROLE", "GROUP", "MEMBERSHIP", "GRANT", "REVOKE",
      "ROW_POLICY"};
  for (const auto kind : kAuthorityEvents) {
    const std::string prefix =
        std::string(kSecurityPrincipalLifecycleEventMagic) + "\t" +
        std::string(kind) + "\t";
    if (StartsWith(event, prefix)) return true;
  }
  return false;
}

std::string AuthorizationContextSuccessorEvent(
    const EngineRequestContext& context,
    std::uint64_t generation,
    const std::vector<std::string>& authority_events) {
  std::string evidence_source;
  for (const auto& event : authority_events) {
    evidence_source.append(event);
    evidence_source.push_back('\n');
  }
  return std::string(kSecurityPrincipalLifecycleEventMagic) +
         "\tAUTH_CONTEXT_SUCCESSOR\t" +
         std::to_string(context.local_transaction_id) + "\t" +
         std::to_string(generation) + "\t" +
         StableToken("security-context-successor", evidence_source);
}

bool LoadLastAuthorizationContextGeneration(
    const EngineRequestContext& context,
    std::uint64_t* generation) {
  if (generation == nullptr) return false;
  *generation = 0;
  std::ifstream in(EventPath(context), std::ios::binary);
  if (!in) return true;
  std::string line;
  while (std::getline(in, line)) {
    const auto parts = Split(line, '\t');
    if (parts.size() < 2 ||
        parts[0] != kSecurityPrincipalLifecycleEventMagic ||
        parts[1] != "AUTH_CONTEXT_SUCCESSOR") {
      continue;
    }
    if (parts.size() != 5) return false;
    const auto next = ParseU64(parts[3]);
    if (next == 0 || next <= *generation || parts[4].empty()) return false;
    *generation = next;
  }
  return true;
}

EngineSecurityAuditRecord MakeAudit(const EngineRequestContext& context,
                                    const std::string& operation_id,
                                    const std::string& target_uuid,
                                    std::uint64_t generation,
                                    std::string detail) {
  EngineSecurityAuditRecord audit;
  audit.creator_tx = context.local_transaction_id;
  audit.audit_uuid = StableToken("security-audit",
                                 operation_id + "|" + target_uuid + "|" +
                                     std::to_string(context.local_transaction_id) + "|" +
                                     std::to_string(generation));
  audit.operation_id = operation_id;
  audit.actor_principal_uuid = context.principal_uuid.canonical;
  audit.target_uuid = target_uuid;
  audit.outcome = "success";
  audit.redacted_detail = RedactSecurityPrincipalProtectedMaterialForDiagnostics(std::move(detail));
  audit.security_generation = generation;
  return audit;
}

EngineApiDiagnostic AppendEvents(const EngineRequestContext& context,
                                 const std::vector<std::string>& events) {
  if (context.database_path.empty()) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticDatabasePathRequired, "database_path");
  }
  std::error_code exists_error;
  const bool database_exists =
      std::filesystem::exists(context.database_path, exists_error);
  if (exists_error) {
    return PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
        "database_identity_check_failed");
  }
  if (database_exists) {
    EngineRequestContext provider_context = context;
    if (!HasTraceTag(
            provider_context,
            std::string(kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1))) {
      provider_context.trace_tags.emplace_back(
          kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1);
    }
    const auto appended = AppendDatabaseLocalSecurityEventBatchV1(
        provider_context, std::span<const std::string>(events));
    return appended.diagnostic;
  }
  std::lock_guard<std::mutex> append_guard(
      g_security_lifecycle_event_append_mutex);
  std::vector<std::string> committed_events = events;
  if (std::any_of(events.begin(), events.end(), AdvancesAuthorizationContext)) {
    std::uint64_t current_generation = 0;
    if (!LoadLastAuthorizationContextGeneration(context,
                                                &current_generation) ||
        current_generation == std::numeric_limits<std::uint64_t>::max()) {
      return PrincipalDiagnostic(
          kSecurityPrincipalDiagnosticDatabaseWriteFailed,
          "security_context_generation_authority_invalid");
    }
    committed_events.push_back(AuthorizationContextSuccessorEvent(
        context, current_generation + 1, events));
  }
  std::ofstream out(EventPath(context), std::ios::binary | std::ios::app);
  if (!out) { return PrincipalDiagnostic(kSecurityPrincipalDiagnosticDatabaseWriteFailed, "open"); }
  for (const auto& event : committed_events) { out << event << '\n'; }
  out.flush();
  if (!out) { return PrincipalDiagnostic(kSecurityPrincipalDiagnosticDatabaseWriteFailed, "flush"); }
  return OkDiagnostic();
}

std::string GrantKey(const std::string& grantee_uuid,
                     const std::string& target_uuid,
                     const std::string& privilege) {
  return grantee_uuid + "\t" + target_uuid + "\t" + NormalizePrivilege(privilege);
}

std::string MembershipKey(const std::string& member_uuid,
                          const std::string& container_uuid,
                          const std::string& container_kind) {
  return member_uuid + "\t" + container_uuid + "\t" + container_kind;
}

EngineLoadSecurityPrincipalLifecycleStateResult LoadState(const EngineRequestContext& context,
                                                          LoadOptions options) {
  EngineLoadSecurityPrincipalLifecycleStateResult result;
  if (context.database_path.empty()) {
    result.diagnostic = PrincipalDiagnostic(kSecurityPrincipalDiagnosticDatabasePathRequired,
                                            "database_path");
    return result;
  }

  std::error_code exists_error;
  const bool database_exists =
      std::filesystem::exists(context.database_path, exists_error);
  if (exists_error) {
    result.diagnostic = PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
        "database_identity_check_failed");
    return result;
  }

  std::map<std::string, EngineSecurityPrincipalRecord> principals;
  std::map<std::string, EngineSecurityRoleRecord> roles;
  std::map<std::string, EngineSecurityGroupRecord> groups;
  std::map<std::string, EngineSecurityMembershipRecord> memberships;
  std::map<std::string, EngineSecurityPrivilegeGrantRecord> grants;
  std::map<std::string, EngineSecurityRowPolicyRecord> row_policies;
  std::map<std::string, EngineSecurityDefinerRightsCacheRecord> cache;
  std::vector<std::string> event_lines;
  std::uint64_t initial_security_context_generation = 0;
  std::uint64_t expected_security_context_generation = 0;

  if (database_exists) {
    const auto catalog =
        scratchbird::storage::database::ReadDatabaseBootstrapSecurityCatalog(
            context.database_path);
    if (!catalog.ok()) {
      result.diagnostic = PrincipalDiagnostic(
          kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
          catalog.diagnostic.diagnostic_code.empty()
              ? "bootstrap_security_catalog_unavailable"
              : catalog.diagnostic.diagnostic_code);
      return result;
    }
    const auto& state = catalog.state;
    if (state.sysarch_role_uuid.valid()) {
      EngineSecurityRoleRecord role;
      role.creator_tx = state.creator_tx;
      role.event_sequence = 1;
      role.role_uuid = scratchbird::core::uuid::UuidToString(
          state.sysarch_role_uuid.value);
      role.role_name = "ROLE_SYSARCH";
      role.lifecycle_state = "active";
      role.security_generation = state.policy_generation;
      roles.emplace(role.role_uuid, std::move(role));
    }
    if (state.present) {
      EngineSecurityPrincipalRecord principal;
      principal.creator_tx = state.creator_tx;
      principal.event_sequence = 1;
      principal.principal_uuid = scratchbird::core::uuid::UuidToString(
          state.principal_uuid.value);
      principal.principal_name = state.principal_name;
      principal.principal_kind = "user";
      principal.lifecycle_state = "active";
      principal.credential_fingerprint = state.credential_fingerprint;
      principal.security_generation = state.policy_generation;
      principals.emplace(principal.principal_uuid, std::move(principal));

      EngineSecurityMembershipRecord membership;
      membership.creator_tx = state.creator_tx;
      membership.event_sequence = 1;
      membership.membership_uuid = scratchbird::core::uuid::UuidToString(
          state.membership_uuid.value);
      membership.member_principal_uuid =
          scratchbird::core::uuid::UuidToString(state.principal_uuid.value);
      membership.container_uuid =
          scratchbird::storage::database::kCanonicalSysarchRoleObjectUuid;
      membership.container_kind = "role";
      membership.grantor_principal_uuid = membership.member_principal_uuid;
      membership.security_generation = state.policy_generation;
      memberships.emplace(
          MembershipKey(membership.member_principal_uuid,
                        membership.container_uuid,
                        membership.container_kind),
          std::move(membership));
    }
    result.state.security_generation =
        std::max<std::uint64_t>(1, state.policy_generation);
    result.state.policy_generation =
        std::max<std::uint64_t>(1, state.policy_generation);
    result.state.security_context_generation =
        state.security_context_generation;
    if (result.state.security_context_generation == 0) {
      result.diagnostic = PrincipalDiagnostic(
          kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
          "bootstrap_security_context_generation_missing");
      return result;
    }
    result.state.cache_invalidation_epoch = result.state.security_generation;
    initial_security_context_generation =
        result.state.security_context_generation;

    EngineRequestContext provider_context = context;
    if (!HasTraceTag(
            provider_context,
            std::string(kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1))) {
      provider_context.trace_tags.emplace_back(
          kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1);
    }
    const auto visibility =
        provider_context.local_transaction_id == 0
            ? DatabaseLocalSecurityEventVisibilityV1::latest_committed
            : DatabaseLocalSecurityEventVisibilityV1::
                  include_reader_own_uncommitted;
    auto loaded = LoadDatabaseLocalSecurityEventStoreV1(provider_context,
                                                        visibility);
    if (!loaded.ok) {
      result.diagnostic = loaded.diagnostic;
      return result;
    }
    if (loaded.state.security_context_generation <
        initial_security_context_generation) {
      result.diagnostic = PrincipalDiagnostic(
          kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
          "page_backed_security_context_generation_regressed");
      return result;
    }
    expected_security_context_generation =
        loaded.state.security_context_generation;
    event_lines = std::move(loaded.state.events);
  } else {
    std::ifstream in(EventPath(context), std::ios::binary);
    std::string line;
    while (in && std::getline(in, line)) {
      event_lines.push_back(std::move(line));
    }
  }

  std::uint64_t event_sequence = 0;
  std::uint64_t durable_security_context_generation =
      initial_security_context_generation;
  bool collecting_authority_successor_batch = false;
  std::vector<std::string> authority_successor_batch;
  for (const auto& line : event_lines) {
    ++event_sequence;
    if (!StartsWith(line, kSecurityPrincipalLifecycleEventMagic)) { continue; }
    const auto parts = Split(line, '\t');
    if (parts.size() < 3) { continue; }
    const std::string& event = parts[1];
    const std::uint64_t creator_tx = ParseU64(parts[2]);
    if (event == "AUTH_CONTEXT_SUCCESSOR") {
      std::string evidence_source;
      for (const auto& authority_event : authority_successor_batch) {
        evidence_source.append(authority_event);
        evidence_source.push_back('\n');
      }
      const auto generation = parts.size() == 5 ? ParseU64(parts[3]) : 0;
      const auto expected = StableToken("security-context-successor",
                                        evidence_source);
      if (!collecting_authority_successor_batch || parts.size() != 5 ||
          generation == 0 ||
          generation <= durable_security_context_generation ||
          parts[4] != expected) {
        result.diagnostic = PrincipalDiagnostic(
            kSecurityPrincipalDiagnosticDatabaseWriteFailed,
            "security_context_successor_evidence_invalid");
        return result;
      }
      durable_security_context_generation = generation;
      collecting_authority_successor_batch = false;
      authority_successor_batch.clear();
    } else if (AdvancesAuthorizationContext(line)) {
      if (collecting_authority_successor_batch) {
        result.diagnostic = PrincipalDiagnostic(
            kSecurityPrincipalDiagnosticDatabaseWriteFailed,
            "security_context_successor_missing");
        return result;
      }
      collecting_authority_successor_batch = true;
      authority_successor_batch.clear();
      authority_successor_batch.push_back(line);
    } else if (collecting_authority_successor_batch) {
      authority_successor_batch.push_back(line);
    }
    if (options.enforce_visibility && !EventVisible(context, creator_tx)) { continue; }

    if (event == "PRINCIPAL" && parts.size() >= 10) {
      EngineSecurityPrincipalRecord record;
      record.creator_tx = creator_tx;
      record.event_sequence = event_sequence;
      record.principal_uuid = parts[3];
      record.principal_name = HexDecode(parts[4]);
      record.principal_kind = parts[5].empty() ? "user" : parts[5];
      record.lifecycle_state = parts[6].empty() ? "active" : parts[6];
      record.credential_fingerprint = HexDecode(parts[7]);
      record.security_generation = ParseU64(parts[8]);
      record.deleted = ParseBool(parts[9]);
      result.state.security_generation =
          std::max(result.state.security_generation, record.security_generation);
      result.state.policy_generation =
          std::max(result.state.policy_generation, record.security_generation);
      if (record.deleted || record.lifecycle_state != "active") {
        principals.erase(record.principal_uuid);
      } else {
        principals[record.principal_uuid] = std::move(record);
      }
    } else if (event == "ROLE" && parts.size() >= 9) {
      EngineSecurityRoleRecord record;
      record.creator_tx = creator_tx;
      record.event_sequence = event_sequence;
      record.role_uuid = parts[3];
      record.role_name = HexDecode(parts[4]);
      record.owner_principal_uuid = parts[5];
      record.lifecycle_state = parts[6].empty() ? "active" : parts[6];
      record.security_generation = ParseU64(parts[7]);
      record.deleted = ParseBool(parts[8]);
      result.state.security_generation =
          std::max(result.state.security_generation, record.security_generation);
      result.state.policy_generation =
          std::max(result.state.policy_generation, record.security_generation);
      if (record.deleted || record.lifecycle_state != "active") {
        roles.erase(record.role_uuid);
      } else {
        roles[record.role_uuid] = std::move(record);
      }
    } else if (event == "GROUP" && parts.size() >= 9) {
      EngineSecurityGroupRecord record;
      record.creator_tx = creator_tx;
      record.event_sequence = event_sequence;
      record.group_uuid = parts[3];
      record.group_name = HexDecode(parts[4]);
      record.external_authority_ref = HexDecode(parts[5]);
      record.lifecycle_state = parts[6].empty() ? "active" : parts[6];
      record.security_generation = ParseU64(parts[7]);
      record.deleted = ParseBool(parts[8]);
      result.state.security_generation =
          std::max(result.state.security_generation, record.security_generation);
      result.state.policy_generation =
          std::max(result.state.policy_generation, record.security_generation);
      if (record.deleted || record.lifecycle_state != "active") {
        groups.erase(record.group_uuid);
      } else {
        groups[record.group_uuid] = std::move(record);
      }
    } else if (event == "MEMBERSHIP" && parts.size() >= 10) {
      EngineSecurityMembershipRecord record;
      record.creator_tx = creator_tx;
      record.event_sequence = event_sequence;
      record.membership_uuid = parts[3];
      record.member_principal_uuid = parts[4];
      record.container_uuid = parts[5];
      record.container_kind = parts[6];
      record.grantor_principal_uuid = parts[7];
      record.security_generation = ParseU64(parts[8]);
      record.revoked = ParseBool(parts[9]);
      result.state.security_generation =
          std::max(result.state.security_generation, record.security_generation);
      result.state.policy_generation =
          std::max(result.state.policy_generation, record.security_generation);
      const std::string key = MembershipKey(record.member_principal_uuid,
                                            record.container_uuid,
                                            record.container_kind);
      if (record.revoked) {
        memberships.erase(key);
      } else {
        memberships[key] = std::move(record);
      }
    } else if (event == "GRANT" && parts.size() >= 13) {
      EngineSecurityPrivilegeGrantRecord record;
      record.creator_tx = creator_tx;
      record.event_sequence = event_sequence;
      record.grant_uuid = parts[3];
      record.grantee_uuid = parts[4];
      record.grantee_kind = parts[5].empty() ? "principal" : parts[5];
      record.target_object_uuid = parts[6];
      record.target_object_kind = parts[7];
      record.privilege = NormalizePrivilege(parts[8]);
      record.grantor_principal_uuid = parts[9];
      record.grant_effect = parts[10].empty() ? "allow" : parts[10];
      record.security_generation = ParseU64(parts[11]);
      record.revoked = ParseBool(parts[12]);
      result.state.security_generation =
          std::max(result.state.security_generation, record.security_generation);
      result.state.policy_generation =
          std::max(result.state.policy_generation, record.security_generation);
      const std::string key = GrantKey(record.grantee_uuid,
                                       record.target_object_uuid,
                                       record.privilege);
      if (record.revoked) {
        grants.erase(key);
      } else {
        grants[key] = std::move(record);
      }
    } else if (event == "REVOKE" && parts.size() >= 8) {
      const std::string grantee_uuid = parts[3];
      const std::string target_uuid = parts[4];
      const std::string privilege = NormalizePrivilege(parts[5]);
      const std::uint64_t generation = ParseU64(parts[7]);
      result.state.security_generation = std::max(result.state.security_generation, generation);
      result.state.policy_generation = std::max(result.state.policy_generation, generation);
      grants.erase(GrantKey(grantee_uuid, target_uuid, privilege));
    } else if (event == "ROW_POLICY" && parts.size() >= 12) {
      EngineSecurityRowPolicyRecord record;
      record.creator_tx = creator_tx;
      record.event_sequence = event_sequence;
      record.policy_uuid = parts[3];
      record.target_object_uuid = parts[4];
      record.target_object_kind = parts[5];
      record.policy_effect = parts[6].empty() ? "deny_all" : parts[6];
      record.predicate_envelope = HexDecode(parts[7]);
      record.definer_principal_uuid = parts[8];
      record.lifecycle_state = parts[9].empty() ? "active" : parts[9];
      record.policy_generation = ParseU64(parts[10]);
      record.deleted = ParseBool(parts[11]);
      if (parts.size() >= 27) {
        record.policy_version_uuid = parts[12];
        record.effective_transaction_number = ParseU64(parts[13]);
        record.target_object_generation = ParseU64(parts[14]);
        const auto phase = ParseU64(parts[15]);
        record.update_policy_phase =
            phase <= std::numeric_limits<std::uint8_t>::max()
                ? static_cast<std::uint8_t>(phase)
                : 0;
        record.effective_policy_uuid = parts[16];
        record.effective_policy_generation = ParseU64(parts[17]);
        record.effective_expression_uuid = parts[18];
        record.effective_expression_generation = ParseU64(parts[19]);
        if (!HexDecodeSha256(parts[20],
                            &record.effective_expression_evidence_sha256)) {
          result.diagnostic = PrincipalDiagnostic(
              kSecurityPrincipalDiagnosticDatabaseWriteFailed,
              "row_policy_effective_evidence_invalid");
          return result;
        }
        record.source_expression_uuid = parts[21];
        record.source_expression_generation = ParseU64(parts[22]);
        if (!HexDecodeSha256(parts[23],
                            &record.source_expression_evidence_sha256)) {
          result.diagnostic = PrincipalDiagnostic(
              kSecurityPrincipalDiagnosticDatabaseWriteFailed,
              "row_policy_native_evidence_invalid");
          return result;
        }
        record.source_catalog_snapshot_uuid = parts[24];
        record.source_catalog_generation = ParseU64(parts[25]);
        record.source_security_generation = ParseU64(parts[26]);
      } else if (parts.size() >= 22) {
        // Pre-native-effective-projection rows remain readable for their
        // historical security surfaces.  Their missing typed authority keeps
        // them fail-closed for typed UPDATE.
        record.policy_version_uuid = parts[12];
        record.effective_transaction_number = ParseU64(parts[13]);
        record.target_object_generation = ParseU64(parts[14]);
        const auto phase = ParseU64(parts[15]);
        record.update_policy_phase =
            phase <= std::numeric_limits<std::uint8_t>::max()
                ? static_cast<std::uint8_t>(phase)
                : 0;
        record.source_expression_uuid = parts[16];
        record.source_expression_generation = ParseU64(parts[17]);
        if (!HexDecodeSha256(parts[18],
                            &record.source_expression_evidence_sha256)) {
          result.diagnostic = PrincipalDiagnostic(
              kSecurityPrincipalDiagnosticDatabaseWriteFailed,
              "row_policy_native_evidence_invalid");
          return result;
        }
        record.source_catalog_snapshot_uuid = parts[19];
        record.source_catalog_generation = ParseU64(parts[20]);
        record.source_security_generation = ParseU64(parts[21]);
      }
      result.state.security_generation =
          std::max(result.state.security_generation, record.policy_generation);
      result.state.policy_generation =
          std::max(result.state.policy_generation, record.policy_generation);
      if (record.deleted || record.lifecycle_state != "active") {
        row_policies.erase(record.policy_uuid);
      } else {
        row_policies[record.policy_uuid] = std::move(record);
      }
    } else if (event == "DEFINER_CACHE" && parts.size() >= 9) {
      EngineSecurityDefinerRightsCacheRecord record;
      record.creator_tx = creator_tx;
      record.event_sequence = event_sequence;
      record.cache_key = parts[3];
      record.definer_principal_uuid = parts[4];
      record.target_object_uuid = parts[5];
      record.privilege = NormalizePrivilege(parts[6]);
      record.decision = parts[7].empty() ? "deny" : parts[7];
      record.policy_generation = ParseU64(parts[8]);
      result.state.policy_generation =
          std::max(result.state.policy_generation, record.policy_generation);
      cache[record.cache_key] = std::move(record);
    } else if (event == "CACHE_INVALIDATE" && parts.size() >= 6) {
      const std::uint64_t generation = ParseU64(parts[5]);
      result.state.cache_invalidation_epoch =
          std::max(result.state.cache_invalidation_epoch, generation);
      result.state.security_generation =
          std::max(result.state.security_generation, generation);
      result.state.policy_generation =
          std::max(result.state.policy_generation, generation);
    } else if (event == "AUTH_CONTEXT_SUCCESSOR") {
      result.state.security_context_generation = ParseU64(parts[3]);
    } else if (event == "AUDIT" && parts.size() >= 10) {
      EngineSecurityAuditRecord audit;
      audit.creator_tx = creator_tx;
      audit.event_sequence = event_sequence;
      audit.audit_uuid = parts[3];
      audit.operation_id = HexDecode(parts[4]);
      audit.actor_principal_uuid = parts[5];
      audit.target_uuid = parts[6];
      audit.outcome = parts[7];
      audit.redacted_detail = HexDecode(parts[8]);
      audit.security_generation = ParseU64(parts[9]);
      result.state.audit_records.push_back(std::move(audit));
    }
  }
  if (collecting_authority_successor_batch) {
    result.diagnostic = PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticDatabaseWriteFailed,
        "security_context_successor_missing_at_eof");
    return result;
  }
  if (database_exists && durable_security_context_generation !=
                             expected_security_context_generation) {
    result.diagnostic = PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticDatabaseWriteFailed,
        "page_backed_security_context_generation_mismatch");
    return result;
  }

  for (auto& [_, record] : principals) { result.state.principals.push_back(std::move(record)); }
  for (auto& [_, record] : roles) { result.state.roles.push_back(std::move(record)); }
  for (auto& [_, record] : groups) { result.state.groups.push_back(std::move(record)); }
  for (auto& [_, record] : memberships) { result.state.memberships.push_back(std::move(record)); }
  for (auto& [_, record] : grants) { result.state.grants.push_back(std::move(record)); }
  for (auto& [_, record] : row_policies) { result.state.row_policies.push_back(std::move(record)); }
  for (auto& [_, record] : cache) { result.state.definer_rights_cache.push_back(std::move(record)); }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

std::uint64_t NextGeneration(const EngineSecurityPrincipalLifecycleState& state) {
  return std::max({state.security_generation,
                   state.policy_generation,
                   state.cache_invalidation_epoch}) + 1;
}

const EngineSecurityPrincipalRecord* FindPrincipal(const EngineSecurityPrincipalLifecycleState& state,
                                                   const std::string& principal_uuid) {
  for (const auto& principal : state.principals) {
    if (principal.principal_uuid == principal_uuid) { return &principal; }
  }
  return nullptr;
}

const EngineSecurityRoleRecord* FindRole(const EngineSecurityPrincipalLifecycleState& state,
                                         const std::string& role_uuid) {
  for (const auto& role : state.roles) {
    if (role.role_uuid == role_uuid) { return &role; }
  }
  return nullptr;
}

const EngineSecurityGroupRecord* FindGroup(const EngineSecurityPrincipalLifecycleState& state,
                                           const std::string& group_uuid) {
  for (const auto& group : state.groups) {
    if (group.group_uuid == group_uuid) { return &group; }
  }
  return nullptr;
}

bool FindAnySecuritySubject(const EngineSecurityPrincipalLifecycleState& state,
                            const std::string& subject_uuid) {
  return FindPrincipal(state, subject_uuid) != nullptr ||
         FindRole(state, subject_uuid) != nullptr ||
         FindGroup(state, subject_uuid) != nullptr;
}

std::set<std::string> EffectiveGranteeSet(const EngineSecurityPrincipalLifecycleState& state,
                                          const std::string& principal_uuid) {
  std::set<std::string> grantees;
  std::vector<std::string> pending;
  pending.push_back(principal_uuid);
  while (!pending.empty()) {
    const std::string current = pending.back();
    pending.pop_back();
    if (!grantees.insert(current).second) { continue; }
    for (const auto& membership : state.memberships) {
      if (membership.revoked || membership.member_principal_uuid != current ||
          membership.container_uuid.empty()) {
        continue;
      }
      pending.push_back(membership.container_uuid);
    }
  }
  return grantees;
}

bool GrantApplies(const EngineSecurityPrivilegeGrantRecord& grant,
                  const std::set<std::string>& grantees,
                  const std::string& target_uuid,
                  const std::string& privilege) {
  return grantees.count(grant.grantee_uuid) != 0 &&
         grant.target_object_uuid == target_uuid &&
         grant.privilege == NormalizePrivilege(privilege) &&
         !grant.revoked;
}

struct GrantDecision {
  bool allowed = false;
  bool explicit_deny = false;
  std::vector<std::string> matched_grants;
};

GrantDecision EvaluateGrantState(const EngineSecurityPrincipalLifecycleState& state,
                                 const std::string& principal_uuid,
                                 const std::string& target_uuid,
                                 const std::string& privilege) {
  GrantDecision decision;
  const auto grantees = EffectiveGranteeSet(state, principal_uuid);
  for (const auto& grant : state.grants) {
    if (!GrantApplies(grant, grantees, target_uuid, privilege)) { continue; }
    if (grant.grant_effect == "deny") {
      decision.explicit_deny = true;
      decision.allowed = false;
      decision.matched_grants.push_back(grant.grant_uuid);
      return decision;
    }
    decision.allowed = true;
    decision.matched_grants.push_back(grant.grant_uuid);
  }
  return decision;
}

const EngineSecurityRowPolicyRecord* FindRowPolicy(
    const EngineSecurityPrincipalLifecycleState& state,
    const std::string& policy_uuid) {
  for (const auto& policy : state.row_policies) {
    if (policy.policy_uuid == policy_uuid) { return &policy; }
  }
  return nullptr;
}

void FillMutationEvidence(EngineApiResult* result,
                          const std::string& operation_id,
                          const std::string& target_uuid,
                          std::uint64_t generation) {
  AddEvidence(result, "security_generation", std::to_string(generation));
  AddEvidence(result, "security_audit", operation_id + ":" + target_uuid);
  AddEvidence(result, "security_cache_invalidation", std::to_string(generation));
}

std::string CredentialFingerprint(const EngineSecurityCreatePrincipalRequest& request,
                                  const std::string& principal_uuid) {
  if (!request.credential_fingerprint.empty()) { return request.credential_fingerprint; }
  if (request.credential_protected_material_ref.empty()) { return {}; }
  return StableToken("credential-fingerprint",
                     principal_uuid + "|" + request.credential_protected_material_ref);
}

std::string CredentialFingerprint(const EngineSecurityAlterPrincipalRequest& request,
                                  const std::string& principal_uuid,
                                  const std::string& existing_fingerprint) {
  if (!request.credential_fingerprint.empty()) { return request.credential_fingerprint; }
  if (request.credential_protected_material_ref.empty()) { return existing_fingerprint; }
  return StableToken("credential-fingerprint",
                     principal_uuid + "|" + request.credential_protected_material_ref);
}

template <typename TResult>
TResult MutatingSetupFailure(const EngineApiRequest& request,
                             const std::string& operation_id,
                             const std::string& right) {
  const auto authority = ValidateSecurityAuthority(request, operation_id, right);
  if (authority.error) { return DiagnosticResult<TResult>(request.context, operation_id, authority); }
  const auto context = ValidateMutatingContext(request.context);
  if (context.error) { return DiagnosticResult<TResult>(request.context, operation_id, context); }
  return SuccessResult<TResult>(request.context, operation_id);
}

std::mutex g_security_policy_snapshot_authority_mutex;
std::unordered_map<std::string, EngineSecurityPolicySnapshotAuthorityV1>
    g_security_policy_snapshot_authorities;
std::uint64_t g_security_policy_snapshot_ordinal = 0;

bool ExactNonzeroUuid(std::string_view text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == text;
}

std::string TypedUpdateUuidText(
    const scratchbird::wire::TypedUpdateUuid& bytes) {
  if (std::all_of(bytes.begin(), bytes.end(),
                  [](std::uint8_t value) { return value == 0; })) {
    return {};
  }
  scratchbird::core::platform::Uuid value{};
  std::copy(bytes.begin(), bytes.end(), value.bytes.begin());
  return scratchbird::core::uuid::UuidToString(value);
}

std::string FreshSecurityPolicySnapshotUuid() {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object,
      now + (++g_security_policy_snapshot_ordinal));
  return generated.ok()
      ? scratchbird::core::uuid::UuidToString(generated.value.value)
      : std::string{};
}

bool PolicyIdentityLess(
    const EngineSecurityPolicyCatalogRowIdentityV1& left,
    const EngineSecurityPolicyCatalogRowIdentityV1& right) {
  if (left.phase != right.phase) return left.phase < right.phase;
  const auto left_uuid = scratchbird::core::uuid::ParseUuid(left.policy_uuid);
  const auto right_uuid = scratchbird::core::uuid::ParseUuid(right.policy_uuid);
  if (!left_uuid.ok() || !right_uuid.ok()) {
    return left.policy_uuid < right.policy_uuid;
  }
  if (left_uuid.value.bytes != right_uuid.value.bytes) {
    return left_uuid.value.bytes < right_uuid.value.bytes;
  }
  return left.policy_generation < right.policy_generation;
}

EngineApiDiagnostic ResolveSecurityPolicySnapshotSource(
    const EngineRequestContext& context,
    const std::string& target_relation_uuid,
    EngineSecurityPolicySnapshotAuthorityV1* snapshot) {
  if (snapshot == nullptr || !context.security_context_present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !ExactNonzeroUuid(context.statement_receipt_uuid.canonical) ||
      !ExactNonzeroUuid(target_relation_uuid) ||
      !context.authorization_context.present ||
      !ExactNonzeroUuid(context.authorization_context.authority_uuid.canonical) ||
      context.authorization_context.security_context_generation == 0 ||
      context.security_epoch == 0 || context.catalog_generation_id == 0 ||
      context.authorization_context.security_epoch != context.security_epoch ||
      context.authorization_context.catalog_generation_id !=
          context.catalog_generation_id ||
      context.authorization_context.policy_epoch == 0) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                               "policy_snapshot_context_invalid");
  }

  const auto loaded = LoadState(context, {.enforce_visibility = true});
  if (!loaded.ok) return loaded.diagnostic;
  if (loaded.state.security_generation == 0 ||
      loaded.state.policy_generation == 0 ||
      loaded.state.security_context_generation == 0 ||
      loaded.state.security_context_generation !=
          context.authorization_context.security_context_generation ||
      loaded.state.security_generation !=
          context.authorization_context.security_epoch ||
      loaded.state.policy_generation !=
          context.authorization_context.policy_epoch) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyStale,
                               "durable_materialized_generation_mismatch");
  }

  std::vector<EngineSecurityPolicyCatalogRowIdentityV1> admitted;
  for (const auto& policy : context.authorization_context.policies) {
    if (policy.target_uuid.canonical != target_relation_uuid) continue;
    if (!ExactNonzeroUuid(policy.policy_uuid.canonical) ||
        policy.source_policy_generation == 0 ||
        policy.policy_epoch !=
            context.authorization_context.policy_epoch) {
      return PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyStale,
                                 "materialized_policy_identity_invalid");
    }
    const EngineSecurityRowPolicyRecord* durable = nullptr;
    for (const auto& candidate : loaded.state.row_policies) {
      if (candidate.policy_uuid != policy.policy_uuid.canonical) continue;
      if (durable != nullptr) {
        return PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyDuplicate,
                                   "durable_policy_identity_duplicate");
      }
      durable = &candidate;
    }
    if (durable == nullptr || durable->deleted ||
        durable->lifecycle_state != "active" ||
        durable->target_object_uuid != target_relation_uuid ||
        durable->policy_generation != policy.source_policy_generation ||
        !ExactNonzeroUuid(durable->policy_version_uuid) ||
        durable->target_object_generation == 0 ||
        (durable->update_policy_phase != 1 &&
         durable->update_policy_phase != 2) ||
        !ExactNonzeroUuid(durable->effective_policy_uuid) ||
        durable->effective_policy_generation == 0 ||
        !ExactNonzeroUuid(durable->effective_expression_uuid) ||
        durable->effective_expression_generation == 0 ||
        !NonzeroSha256(durable->effective_expression_evidence_sha256) ||
        policy.update_policy_phase != durable->update_policy_phase ||
        policy.effective_policy_uuid.canonical !=
            durable->effective_policy_uuid ||
        policy.effective_policy_generation !=
            durable->effective_policy_generation ||
        policy.effective_expression_uuid.canonical !=
            durable->effective_expression_uuid ||
        policy.effective_expression_generation !=
            durable->effective_expression_generation ||
        policy.effective_expression_evidence_sha256 !=
            durable->effective_expression_evidence_sha256 ||
        !ExactNonzeroUuid(durable->source_expression_uuid) ||
        durable->source_expression_generation == 0 ||
        !NonzeroSha256(durable->source_expression_evidence_sha256) ||
        !ExactNonzeroUuid(durable->source_catalog_snapshot_uuid) ||
        durable->source_catalog_generation == 0 ||
        durable->source_security_generation == 0 ||
        durable->source_security_generation > loaded.state.security_generation) {
      return PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyStale,
                                 "durable_materialized_policy_mismatch");
    }
    const auto transaction_inventory =
        LoadLocalTransactionInventoryFromDatabase(context.database_path);
    if (!transaction_inventory.ok()) {
      return PrincipalDiagnostic(
          kSecurityPrincipalDiagnosticPolicyStale,
          "policy_effective_transaction_inventory_unavailable");
    }
    const auto effective_transaction = LookupLocalTransaction(
        transaction_inventory.inventory,
        MakeLocalTransactionId(durable->creator_tx));
    if (!effective_transaction.ok() ||
        (effective_transaction.entry.state != TransactionState::committed &&
         effective_transaction.entry.state != TransactionState::archived) ||
        effective_transaction.entry.identity.local_id.value == 0) {
      return PrincipalDiagnostic(
          kSecurityPrincipalDiagnosticPolicyStale,
          "policy_effective_transaction_not_committed");
    }
    EngineSecurityPolicyCatalogRowIdentityV1 identity;
    identity.policy_uuid = durable->policy_uuid;
    identity.policy_generation = durable->policy_generation;
    identity.policy_version_uuid = durable->policy_version_uuid;
    identity.effective_transaction_number =
        effective_transaction.entry.identity.local_id.value;
    identity.target_relation_uuid = durable->target_object_uuid;
    identity.target_relation_generation =
        durable->target_object_generation;
    identity.phase = durable->update_policy_phase;
    identity.effective_policy_uuid = durable->effective_policy_uuid;
    identity.effective_policy_generation =
        durable->effective_policy_generation;
    identity.effective_expression_uuid =
        durable->effective_expression_uuid;
    identity.effective_expression_generation =
        durable->effective_expression_generation;
    identity.effective_expression_evidence_sha256 =
        durable->effective_expression_evidence_sha256;
    identity.source_expression_uuid = durable->source_expression_uuid;
    identity.source_expression_generation =
        durable->source_expression_generation;
    identity.source_expression_evidence_sha256 =
        durable->source_expression_evidence_sha256;
    // DUSR binds the UPDATE statement's current catalog/security snapshot,
    // not the policy-creation statement's historical provenance fields.
    identity.catalog_snapshot_uuid =
        context.statement_metadata_snapshot_uuid.canonical;
    identity.catalog_generation = context.catalog_generation_id;
    identity.security_generation = loaded.state.security_generation;
    admitted.push_back(std::move(identity));
  }
  std::sort(admitted.begin(), admitted.end(), PolicyIdentityLess);
  if (std::adjacent_find(admitted.begin(), admitted.end(),
                         [](const auto& left, const auto& right) {
                           return left.policy_uuid == right.policy_uuid;
                         }) != admitted.end()) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyDuplicate,
                               "materialized_policy_identity_duplicate");
  }

  snapshot->authenticated_statement_receipt_uuid =
      context.statement_receipt_uuid.canonical;
  snapshot->security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  snapshot->security_context_generation =
      context.authorization_context.security_context_generation;
  snapshot->security_generation = loaded.state.security_generation;
  snapshot->policy_generation = loaded.state.policy_generation;
  snapshot->target_relation_uuid = target_relation_uuid;
  snapshot->admitted_policy_rows = std::move(admitted);
  return OkDiagnostic();
}

}  // namespace

std::string RedactSecurityPrincipalProtectedMaterialForDiagnostics(std::string text) {
  if (text.empty()) { return text; }
  if (ContainsProtectedMaterialMarker(text)) { return "<protected-material-redacted>"; }
  return text;
}

EngineLoadSecurityPrincipalLifecycleStateResult LoadSecurityPrincipalLifecycleState(
    const EngineRequestContext& context) {
  return LoadState(context, {.enforce_visibility = true});
}

EngineSecurityPolicySnapshotAuthorityResultV1
IssueEngineSecurityPolicySnapshotAuthorityV1(
    const EngineRequestContext& context,
    const std::string& target_relation_uuid) {
  EngineSecurityPolicySnapshotAuthorityResultV1 result;
  EngineSecurityPolicySnapshotAuthorityV1 snapshot;
  result.diagnostic = ResolveSecurityPolicySnapshotSource(
      context, target_relation_uuid, &snapshot);
  if (result.diagnostic.error) return result;

  std::lock_guard<std::mutex> guard(
      g_security_policy_snapshot_authority_mutex);
  snapshot.snapshot_uuid = FreshSecurityPolicySnapshotUuid();
  snapshot.snapshot_generation = 1;
  if (!ExactNonzeroUuid(snapshot.snapshot_uuid) ||
      g_security_policy_snapshot_authorities.contains(snapshot.snapshot_uuid)) {
    result.diagnostic = PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticDatabaseWriteFailed,
        "policy_snapshot_identity_issue_failed");
    return result;
  }
  g_security_policy_snapshot_authorities.emplace(snapshot.snapshot_uuid,
                                                 snapshot);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.snapshot = std::move(snapshot);
  return result;
}

EngineApiDiagnostic RevalidateEngineSecurityPolicySnapshotAuthorityV1(
    const EngineRequestContext& context,
    const EngineSecurityPolicySnapshotAuthorityV1& admitted) {
  if (!ExactNonzeroUuid(admitted.snapshot_uuid) ||
      admitted.snapshot_generation != 1) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyStale,
                               "policy_snapshot_identity_invalid");
  }
  EngineSecurityPolicySnapshotAuthorityV1 current;
  auto diagnostic = ResolveSecurityPolicySnapshotSource(
      context, admitted.target_relation_uuid, &current);
  if (diagnostic.error) return diagnostic;

  std::lock_guard<std::mutex> guard(
      g_security_policy_snapshot_authority_mutex);
  const auto found =
      g_security_policy_snapshot_authorities.find(admitted.snapshot_uuid);
  if (found == g_security_policy_snapshot_authorities.end() ||
      found->second != admitted) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyStale,
                               "policy_snapshot_unknown_or_forged");
  }
  current.snapshot_uuid = admitted.snapshot_uuid;
  current.snapshot_generation = admitted.snapshot_generation;
  if (current != admitted) {
    return PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyStale,
                               "policy_snapshot_source_changed");
  }
  return OkDiagnostic();
}

EngineSecurityPolicySnapshotRecoveryResultV1
RecoverEngineSecurityPolicySnapshotFromValidatedDmlUpdateDurableAuthorityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateValidatedDurableAuthorityHandleV1& validated_handle,
    std::span<const std::uint8_t> exact_dumo_bytes) {
  EngineSecurityPolicySnapshotRecoveryResultV1 result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic = PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticPolicyStale, std::move(detail));
    return result;
  };
  if (!validated_handle.valid() || validated_handle.impl_ == nullptr ||
      exact_dumo_bytes.empty() ||
      exact_dumo_bytes.size() !=
          validated_handle.impl_->exact_dumo.size() ||
      !std::equal(exact_dumo_bytes.begin(), exact_dumo_bytes.end(),
                  validated_handle.impl_->exact_dumo.begin())) {
    return refuse("validated_durable_security_handle_invalid");
  }

  const auto& durable = *validated_handle.impl_;
  scratchbird::wire::TypedUpdateSecurityPolicySourceVector source;
  scratchbird::wire::TypedUpdateSecuritySnapshotProof proof;
  scratchbird::wire::TypedUpdateMgaRecoveryObservation observation;
  scratchbird::wire::TypedUpdateCarrierError error;
  if (!scratchbird::wire::DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
          durable.snapshot.source_policy_vector_dusv, &source, &error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          durable.snapshot.security_snapshot_proof_dusp, &proof, &error) ||
      !scratchbird::wire::DecodeAndValidateTypedUpdateMgaRecoveryObservation(
          exact_dumo_bytes, &observation, &error)) {
    return refuse("validated_durable_security_carrier_invalid:" +
                  error.field + ":" + error.detail);
  }
  const std::string snapshot_uuid =
      TypedUpdateUuidText(proof.security_snapshot_uuid);
  const std::string target_relation_uuid =
      TypedUpdateUuidText(proof.target_relation_uuid);
  if (!ExactNonzeroUuid(snapshot_uuid) ||
      proof.security_snapshot_generation == 0 ||
      !ExactNonzeroUuid(target_relation_uuid) ||
      source.records.size() != proof.source_policy_count ||
      durable.identity.database_uuid !=
          TypedUpdateUuidText(proof.database_uuid) ||
      durable.identity.authenticated_statement_receipt_uuid !=
          TypedUpdateUuidText(proof.authenticated_statement_receipt_uuid) ||
      durable.identity.owning_transaction_uuid !=
          TypedUpdateUuidText(proof.owning_transaction_uuid) ||
      durable.identity.owning_local_transaction_id !=
          proof.owning_local_transaction_id ||
      durable.identity.operation_uuid !=
          TypedUpdateUuidText(proof.operation_uuid) ||
      durable.identity.operation_generation != proof.operation_generation ||
      durable.identity.descriptor_uuid !=
          TypedUpdateUuidText(proof.descriptor_uuid) ||
      durable.identity.descriptor_generation != proof.descriptor_generation ||
      durable.identity.recovery_token_uuid !=
          TypedUpdateUuidText(proof.recovery_token_uuid) ||
      durable.identity.recovery_generation != proof.recovery_generation) {
    return refuse("validated_durable_security_identity_mismatch");
  }

  EngineSecurityPolicySnapshotAuthorityV1 current;
  auto diagnostic = ResolveSecurityPolicySnapshotSource(
      context, target_relation_uuid, &current);
  if (diagnostic.error) {
    result.diagnostic = std::move(diagnostic);
    return result;
  }
  if (current.security_context_uuid !=
          TypedUpdateUuidText(proof.security_context_uuid) ||
      current.security_context_generation !=
          proof.security_context_generation ||
      current.security_generation != proof.security_epoch ||
      current.policy_generation != proof.policy_generation ||
      current.policy_generation != proof.policy_catalog_epoch ||
      current.admitted_policy_rows.size() != source.records.size()) {
    return refuse("durable_security_snapshot_source_stale");
  }
  for (std::size_t index = 0; index < source.records.size(); ++index) {
    const auto& encoded = source.records[index];
    const auto& admitted = current.admitted_policy_rows[index];
    if (encoded.source_policy_ordinal != index + 1 ||
        encoded.phase != admitted.phase || encoded.source_state != 1 ||
        TypedUpdateUuidText(encoded.policy_uuid) != admitted.policy_uuid ||
        encoded.policy_generation != admitted.policy_generation ||
        TypedUpdateUuidText(encoded.policy_version_uuid) !=
            admitted.policy_version_uuid ||
        encoded.effective_transaction_number !=
            admitted.effective_transaction_number ||
        TypedUpdateUuidText(encoded.target_relation_uuid) !=
            admitted.target_relation_uuid ||
        encoded.target_relation_generation !=
            admitted.target_relation_generation ||
        TypedUpdateUuidText(encoded.source_expression_uuid) !=
            admitted.source_expression_uuid ||
        encoded.source_expression_generation !=
            admitted.source_expression_generation ||
        encoded.source_expression_evidence_sha256 !=
            admitted.source_expression_evidence_sha256 ||
        TypedUpdateUuidText(encoded.catalog_snapshot_uuid) !=
            admitted.catalog_snapshot_uuid ||
        encoded.catalog_generation != admitted.catalog_generation ||
        TypedUpdateUuidText(encoded.security_snapshot_uuid) != snapshot_uuid ||
        encoded.security_snapshot_generation !=
            proof.security_snapshot_generation) {
      return refuse("durable_security_policy_source_row_stale:" +
                    std::to_string(index));
    }
  }

  current.snapshot_uuid = snapshot_uuid;
  current.snapshot_generation = proof.security_snapshot_generation;
  std::lock_guard<std::mutex> guard(
      g_security_policy_snapshot_authority_mutex);
  const auto found =
      g_security_policy_snapshot_authorities.find(snapshot_uuid);
  if (found != g_security_policy_snapshot_authorities.end()) {
    if (found->second != current) {
      return refuse("durable_security_snapshot_identity_conflict");
    }
  } else {
    g_security_policy_snapshot_authorities.emplace(snapshot_uuid, current);
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.snapshot = std::move(current);
  return result;
}

void ResetEngineSecurityPolicySnapshotAuthorityForTestV1() {
  std::lock_guard<std::mutex> guard(
      g_security_policy_snapshot_authority_mutex);
  g_security_policy_snapshot_authorities.clear();
  g_security_policy_snapshot_ordinal = 0;
}

EngineOwnedSysarchRoleIdentityResult ResolveEngineOwnedSysarchRoleIdentity(
    const EngineRequestContext& context) {
  EngineOwnedSysarchRoleIdentityResult result;
  if (context.database_path.empty()) {
    result.diagnostic = PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticDatabasePathRequired, "database_path");
    return result;
  }
  std::error_code exists_error;
  if (!std::filesystem::exists(context.database_path, exists_error)) {
    if (exists_error) {
      result.diagnostic = PrincipalDiagnostic(
          kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
          "database_identity_check_failed");
      return result;
    }
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto catalog =
      scratchbird::storage::database::ReadDatabaseBootstrapSecurityCatalog(
          context.database_path);
  if (!catalog.ok()) {
    result.diagnostic = PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
        catalog.diagnostic.diagnostic_code.empty()
            ? "bootstrap_security_catalog_unavailable"
            : catalog.diagnostic.diagnostic_code);
    return result;
  }
  const std::string role_uuid = scratchbird::core::uuid::UuidToString(
      catalog.state.sysarch_role_uuid.value);
  if (role_uuid !=
      scratchbird::storage::database::kCanonicalSysarchRoleObjectUuid) {
    result.diagnostic = PrincipalDiagnostic(
        kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
        "engine_owned_sysarch_uuid_mismatch");
    return result;
  }
  result.ok = true;
  result.present = catalog.state.present;
  result.role_uuid = role_uuid;
  if (catalog.state.present) {
    result.principal_uuid = scratchbird::core::uuid::UuidToString(
        catalog.state.principal_uuid.value);
  }
  result.policy_generation = catalog.state.policy_generation;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineSecurityCreatePrincipalResult EngineSecurityCreatePrincipal(
    const EngineSecurityCreatePrincipalRequest& request) {
  constexpr const char* kOperation = "security.principal.create";
  auto preflight =
      MutatingSetupFailure<EngineSecurityCreatePrincipalResult>(request,
                                                                kOperation,
                                                                "SEC_IDENTITY_ADMIN");
  if (!preflight.ok) { return preflight; }
  if (PlaintextCredentialRefused(request)) {
    return DiagnosticResult<EngineSecurityCreatePrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticProtectedMaterialPlaintextRefused,
                            "plaintext_credential_material_is_forbidden"));
  }
  const std::string principal_uuid = PrincipalUuid(request);
  if (principal_uuid.empty()) {
    return DiagnosticResult<EngineSecurityCreatePrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid, "principal_uuid_required"));
  }
  const std::string principal_name = PrimaryName(request, request.principal_name);
  if (principal_name.empty()) {
    return DiagnosticResult<EngineSecurityCreatePrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid, "principal_name_required"));
  }
  const std::string kind = request.principal_kind.empty() ? "user" : request.principal_kind;
  if (kind != "user" && kind != "service" && kind != "system_actor") {
    return DiagnosticResult<EngineSecurityCreatePrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid, "principal_kind:" + kind));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityCreatePrincipalResult>(request.context,
                                                                kOperation,
                                                                loaded.diagnostic);
  }
  if (FindPrincipal(loaded.state, principal_uuid) != nullptr) {
    return DiagnosticResult<EngineSecurityCreatePrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalDuplicate, principal_uuid));
  }

  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityPrincipalRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.principal_uuid = principal_uuid;
  record.principal_name = principal_name;
  record.principal_kind = kind;
  record.credential_fingerprint = CredentialFingerprint(request, principal_uuid);
  record.security_generation = generation;
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              principal_uuid,
                              generation,
                              "principal_kind=" + kind + ";credential_ref=" +
                                  request.credential_protected_material_ref);
  const auto appended = AppendEvents(
      request.context,
      {PrincipalEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, principal_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityCreatePrincipalResult>(request.context,
                                                                kOperation,
                                                                appended);
  }
  const auto resolver = PersistSecurityNameAliases(
      request.context,
      kOperation,
      principal_uuid,
      {"principal", "user", "security_principal"},
      request.localized_names,
      principal_name);
  if (resolver.error) {
    return DiagnosticResult<EngineSecurityCreatePrincipalResult>(request.context,
                                                                kOperation,
                                                                resolver);
  }

  auto result = SuccessResult<EngineSecurityCreatePrincipalResult>(request.context, kOperation);
  result.principal_created = true;
  result.plaintext_material_stored = false;
  result.protected_material_redacted = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = principal_uuid;
  result.primary_object.object_kind = "security_principal";
  FillMutationEvidence(&result, kOperation, principal_uuid, generation);
  AddRow(&result,
         {{"principal_uuid", principal_uuid},
          {"principal_name", principal_name},
          {"principal_kind", kind},
          {"credential_protected_material_ref", "<protected-material-redacted>"},
          {"plaintext_material_stored", "false"},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityAlterPrincipalResult EngineSecurityAlterPrincipal(
    const EngineSecurityAlterPrincipalRequest& request) {
  constexpr const char* kOperation = "security.principal.alter";
  auto preflight =
      MutatingSetupFailure<EngineSecurityAlterPrincipalResult>(request,
                                                               kOperation,
                                                               "SEC_IDENTITY_ADMIN");
  if (!preflight.ok) { return preflight; }
  if (PlaintextCredentialRefused(request)) {
    return DiagnosticResult<EngineSecurityAlterPrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticProtectedMaterialPlaintextRefused,
                            "plaintext_credential_material_is_forbidden"));
  }
  const std::string principal_uuid = PrincipalUuid(request);
  if (principal_uuid.empty()) {
    return DiagnosticResult<EngineSecurityAlterPrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid,
                            "principal_uuid_required"));
  }
  if (IsEngineOwnedBootstrapPrincipal(request.context, principal_uuid)) {
    return DiagnosticResult<EngineSecurityAlterPrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
                            "bootstrap_principal_is_create_time_catalog_owned"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityAlterPrincipalResult>(request.context,
                                                               kOperation,
                                                               loaded.diagnostic);
  }
  const auto* existing = FindPrincipal(loaded.state, principal_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<EngineSecurityAlterPrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid, principal_uuid));
  }

  std::string principal_name = PrimaryName(request, request.principal_name);
  if (principal_name.empty()) { principal_name = existing->principal_name; }
  std::string kind = NormalizePrincipalKind(request.principal_kind);
  if (kind.empty()) { kind = existing->principal_kind.empty() ? "user" : existing->principal_kind; }
  if (!PrincipalKindValid(kind)) {
    return DiagnosticResult<EngineSecurityAlterPrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid,
                            "principal_kind:" + kind));
  }
  std::string lifecycle = NormalizePrincipalLifecycle(request.lifecycle_state);
  if (lifecycle.empty()) {
    lifecycle = existing->lifecycle_state.empty() ? "active" : existing->lifecycle_state;
  }
  if (!PrincipalLifecycleValid(lifecycle)) {
    return DiagnosticResult<EngineSecurityAlterPrincipalResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid,
                            "lifecycle_state:" + lifecycle));
  }

  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityPrincipalRecord record = *existing;
  record.creator_tx = request.context.local_transaction_id;
  record.principal_name = principal_name;
  record.principal_kind = kind;
  record.lifecycle_state = lifecycle;
  record.credential_fingerprint =
      CredentialFingerprint(request, principal_uuid, existing->credential_fingerprint);
  record.security_generation = generation;
  record.deleted = false;
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              principal_uuid,
                              generation,
                              "principal_kind=" + kind + ";lifecycle_state=" + lifecycle +
                                  ";credential_ref=" +
                                  request.credential_protected_material_ref);
  const auto appended = AppendEvents(
      request.context,
      {PrincipalEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, principal_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityAlterPrincipalResult>(request.context,
                                                               kOperation,
                                                               appended);
  }

  auto result = SuccessResult<EngineSecurityAlterPrincipalResult>(request.context, kOperation);
  result.principal_altered = true;
  result.plaintext_material_stored = false;
  result.protected_material_redacted = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = principal_uuid;
  result.primary_object.object_kind = "security_principal";
  FillMutationEvidence(&result, kOperation, principal_uuid, generation);
  AddRow(&result,
         {{"principal_uuid", principal_uuid},
          {"principal_name", principal_name},
          {"principal_kind", kind},
          {"lifecycle_state", lifecycle},
          {"credential_protected_material_ref", "<protected-material-redacted>"},
          {"plaintext_material_stored", "false"},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityCreateRoleResult EngineSecurityCreateRole(
    const EngineSecurityCreateRoleRequest& request) {
  constexpr const char* kOperation = "security.role.create";
  auto preflight =
      MutatingSetupFailure<EngineSecurityCreateRoleResult>(request,
                                                           kOperation,
                                                           "SEC_IDENTITY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string role_uuid = RoleUuid(request);
  const std::string role_name = PrimaryName(request, request.role_name);
  if (role_uuid.empty() || role_name.empty()) {
    return DiagnosticResult<EngineSecurityCreateRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticRoleInvalid,
                            "role_uuid_and_role_name_required"));
  }
  if (IsEngineOwnedSysarchRoleUuid(role_uuid)) {
    return DiagnosticResult<EngineSecurityCreateRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
                            "engine_owned_sysarch_role_is_immutable"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityCreateRoleResult>(request.context, kOperation, loaded.diagnostic);
  }
  if (FindRole(loaded.state, role_uuid) != nullptr) {
    return DiagnosticResult<EngineSecurityCreateRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticRoleInvalid, "duplicate_role:" + role_uuid));
  }
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRoleRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.role_uuid = role_uuid;
  record.role_name = role_name;
  record.owner_principal_uuid = request.context.principal_uuid.canonical;
  record.security_generation = generation;
  const auto audit = MakeAudit(request.context, kOperation, role_uuid, generation, "role=" + role_name);
  const auto appended = AppendEvents(
      request.context,
      {RoleEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, role_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityCreateRoleResult>(request.context, kOperation, appended);
  }
  const auto resolver = PersistSecurityNameAliases(
      request.context,
      kOperation,
      role_uuid,
      {"role", "principal", "security_role"},
      request.localized_names,
      role_name);
  if (resolver.error) {
    return DiagnosticResult<EngineSecurityCreateRoleResult>(request.context, kOperation, resolver);
  }

  auto result = SuccessResult<EngineSecurityCreateRoleResult>(request.context, kOperation);
  result.role_created = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = role_uuid;
  result.primary_object.object_kind = "security_role";
  FillMutationEvidence(&result, kOperation, role_uuid, generation);
  AddRow(&result,
         {{"role_uuid", role_uuid},
          {"role_name", role_name},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityCreateGroupResult EngineSecurityCreateGroup(
    const EngineSecurityCreateGroupRequest& request) {
  constexpr const char* kOperation = "security.group.create";
  auto preflight =
      MutatingSetupFailure<EngineSecurityCreateGroupResult>(request,
                                                            kOperation,
                                                            "SEC_MEMBERSHIP_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string group_uuid = GroupUuid(request);
  const std::string group_name = PrimaryName(request, request.group_name);
  if (group_uuid.empty() || group_name.empty()) {
    return DiagnosticResult<EngineSecurityCreateGroupResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGroupInvalid,
                            "group_uuid_and_group_name_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityCreateGroupResult>(request.context, kOperation, loaded.diagnostic);
  }
  if (FindGroup(loaded.state, group_uuid) != nullptr) {
    return DiagnosticResult<EngineSecurityCreateGroupResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGroupInvalid,
                            "duplicate_group:" + group_uuid));
  }
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityGroupRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.group_uuid = group_uuid;
  record.group_name = group_name;
  record.external_authority_ref =
      RedactSecurityPrincipalProtectedMaterialForDiagnostics(request.external_authority_ref);
  record.security_generation = generation;
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              group_uuid,
                              generation,
                              "group=" + group_name + ";external=" +
                                  request.external_authority_ref);
  const auto appended = AppendEvents(
      request.context,
      {GroupEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, group_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityCreateGroupResult>(request.context, kOperation, appended);
  }
  const auto resolver = PersistSecurityNameAliases(
      request.context,
      kOperation,
      group_uuid,
      {"group", "principal", "security_group"},
      request.localized_names,
      group_name);
  if (resolver.error) {
    return DiagnosticResult<EngineSecurityCreateGroupResult>(request.context, kOperation, resolver);
  }

  auto result = SuccessResult<EngineSecurityCreateGroupResult>(request.context, kOperation);
  result.group_created = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = group_uuid;
  result.primary_object.object_kind = "security_group";
  FillMutationEvidence(&result, kOperation, group_uuid, generation);
  AddRow(&result,
         {{"group_uuid", group_uuid},
          {"group_name", group_name},
          {"external_authority_ref", record.external_authority_ref},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityDropRoleResult EngineSecurityDropRole(
    const EngineSecurityDropRoleRequest& request) {
  constexpr const char* kOperation = "security.role.drop";
  auto preflight =
      MutatingSetupFailure<EngineSecurityDropRoleResult>(request,
                                                         kOperation,
                                                         "SEC_IDENTITY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string role_uuid = !request.role_uuid.empty()
      ? request.role_uuid
      : request.target_object.uuid.canonical;
  if (role_uuid.empty()) {
    return DiagnosticResult<EngineSecurityDropRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticRoleInvalid, "role_uuid_required"));
  }
  if (IsEngineOwnedSysarchRoleUuid(role_uuid)) {
    return DiagnosticResult<EngineSecurityDropRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
                            "engine_owned_sysarch_role_is_immutable"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityDropRoleResult>(request.context,
                                                         kOperation,
                                                         loaded.diagnostic);
  }
  const auto* existing = FindRole(loaded.state, role_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<EngineSecurityDropRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticRoleInvalid, role_uuid));
  }

  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRoleRecord record = *existing;
  record.creator_tx = request.context.local_transaction_id;
  record.lifecycle_state = "dropped";
  record.security_generation = generation;
  record.deleted = true;
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              role_uuid,
                              generation,
                              "role=" + existing->role_name + ";lifecycle_state=dropped");
  const auto appended = AppendEvents(
      request.context,
      {RoleEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, role_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityDropRoleResult>(request.context,
                                                         kOperation,
                                                         appended);
  }
  const auto resolver = RetireNameRegistryEntriesForObject(request.context,
                                                           kOperation,
                                                           role_uuid);
  if (resolver.error) {
    return DiagnosticResult<EngineSecurityDropRoleResult>(request.context,
                                                         kOperation,
                                                         resolver);
  }

  auto result = SuccessResult<EngineSecurityDropRoleResult>(request.context, kOperation);
  result.role_dropped = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = role_uuid;
  result.primary_object.object_kind = "security_role";
  FillMutationEvidence(&result, kOperation, role_uuid, generation);
  AddRow(&result,
         {{"role_uuid", role_uuid},
          {"role_name", existing->role_name},
          {"lifecycle_state", "dropped"},
          {"deleted", "true"},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityDropGroupResult EngineSecurityDropGroup(
    const EngineSecurityDropGroupRequest& request) {
  constexpr const char* kOperation = "security.group.drop";
  auto preflight =
      MutatingSetupFailure<EngineSecurityDropGroupResult>(request,
                                                          kOperation,
                                                          "SEC_MEMBERSHIP_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string group_uuid = !request.group_uuid.empty()
      ? request.group_uuid
      : request.target_object.uuid.canonical;
  if (group_uuid.empty()) {
    return DiagnosticResult<EngineSecurityDropGroupResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGroupInvalid, "group_uuid_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityDropGroupResult>(request.context,
                                                          kOperation,
                                                          loaded.diagnostic);
  }
  const auto* existing = FindGroup(loaded.state, group_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<EngineSecurityDropGroupResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGroupInvalid, group_uuid));
  }

  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityGroupRecord record = *existing;
  record.creator_tx = request.context.local_transaction_id;
  record.lifecycle_state = "dropped";
  record.security_generation = generation;
  record.deleted = true;
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              group_uuid,
                              generation,
                              "group=" + existing->group_name + ";lifecycle_state=dropped");
  const auto appended = AppendEvents(
      request.context,
      {GroupEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, group_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityDropGroupResult>(request.context,
                                                          kOperation,
                                                          appended);
  }
  const auto resolver = RetireNameRegistryEntriesForObject(request.context,
                                                           kOperation,
                                                           group_uuid);
  if (resolver.error) {
    return DiagnosticResult<EngineSecurityDropGroupResult>(request.context,
                                                          kOperation,
                                                          resolver);
  }

  auto result = SuccessResult<EngineSecurityDropGroupResult>(request.context, kOperation);
  result.group_dropped = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = group_uuid;
  result.primary_object.object_kind = "security_group";
  FillMutationEvidence(&result, kOperation, group_uuid, generation);
  AddRow(&result,
         {{"group_uuid", group_uuid},
          {"group_name", existing->group_name},
          {"lifecycle_state", "dropped"},
          {"deleted", "true"},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityGrantMembershipResult EngineSecurityGrantMembership(
    const EngineSecurityGrantMembershipRequest& request) {
  constexpr const char* kOperation = "security.membership.grant";
  auto preflight =
      MutatingSetupFailure<EngineSecurityGrantMembershipResult>(request,
                                                                kOperation,
                                                                "SEC_MEMBERSHIP_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string container_kind =
      request.container_kind.empty() ? "role" : LowerAscii(request.container_kind);
  if (request.member_principal_uuid.empty() || request.container_uuid.empty() ||
      (container_kind != "role" && container_kind != "group")) {
    return DiagnosticResult<EngineSecurityGrantMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGrantInvalid,
                            "member_principal_container_required"));
  }
  if (container_kind == "role" &&
      IsEngineOwnedSysarchRoleUuid(request.container_uuid)) {
    return DiagnosticResult<EngineSecurityGrantMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
                            "engine_owned_sysarch_membership_is_create_time_only"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityGrantMembershipResult>(request.context,
                                                                kOperation,
                                                                loaded.diagnostic);
  }
  if (!FindAnySecuritySubject(loaded.state, request.member_principal_uuid)) {
    return DiagnosticResult<EngineSecurityGrantMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid,
                            request.member_principal_uuid));
  }
  if (container_kind == "role" && FindRole(loaded.state, request.container_uuid) == nullptr) {
    return DiagnosticResult<EngineSecurityGrantMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticRoleInvalid, request.container_uuid));
  }
  if (container_kind == "group" && FindGroup(loaded.state, request.container_uuid) == nullptr) {
    return DiagnosticResult<EngineSecurityGrantMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGroupInvalid, request.container_uuid));
  }
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityMembershipRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.membership_uuid = request.membership_uuid.empty()
      ? StableToken("security-membership",
                    request.member_principal_uuid + "|" + request.container_uuid + "|" + container_kind)
      : request.membership_uuid;
  record.member_principal_uuid = request.member_principal_uuid;
  record.container_uuid = request.container_uuid;
  record.container_kind = container_kind;
  record.grantor_principal_uuid = request.context.principal_uuid.canonical;
  record.security_generation = generation;
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              record.container_uuid,
                              generation,
                              "member=" + record.member_principal_uuid +
                                  ";container_kind=" + record.container_kind);
  const auto appended = AppendEvents(
      request.context,
      {MembershipEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, record.container_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityGrantMembershipResult>(request.context,
                                                                kOperation,
                                                                appended);
  }

  auto result = SuccessResult<EngineSecurityGrantMembershipResult>(request.context, kOperation);
  result.membership_granted = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = record.membership_uuid;
  result.primary_object.object_kind = "security_membership";
  FillMutationEvidence(&result, kOperation, record.membership_uuid, generation);
  AddRow(&result,
         {{"membership_uuid", record.membership_uuid},
          {"member_principal_uuid", record.member_principal_uuid},
          {"container_uuid", record.container_uuid},
          {"container_kind", record.container_kind},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityRevokeMembershipResult EngineSecurityRevokeMembership(
    const EngineSecurityRevokeMembershipRequest& request) {
  constexpr const char* kOperation = "security.membership.revoke";
  auto preflight =
      MutatingSetupFailure<EngineSecurityRevokeMembershipResult>(request,
                                                                 kOperation,
                                                                 "SEC_MEMBERSHIP_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string container_kind =
      request.container_kind.empty() ? "role" : LowerAscii(request.container_kind);
  if (request.member_principal_uuid.empty() || request.container_uuid.empty() ||
      (container_kind != "role" && container_kind != "group")) {
    return DiagnosticResult<EngineSecurityRevokeMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGrantInvalid,
                            "member_principal_container_required"));
  }
  if (container_kind == "role" &&
      IsEngineOwnedSysarchRoleUuid(request.container_uuid)) {
    return DiagnosticResult<EngineSecurityRevokeMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
                            "engine_owned_sysarch_membership_is_immutable"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityRevokeMembershipResult>(request.context,
                                                                 kOperation,
                                                                 loaded.diagnostic);
  }
  if (!FindAnySecuritySubject(loaded.state, request.member_principal_uuid)) {
    return DiagnosticResult<EngineSecurityRevokeMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid,
                            request.member_principal_uuid));
  }
  if (container_kind == "role" && FindRole(loaded.state, request.container_uuid) == nullptr) {
    return DiagnosticResult<EngineSecurityRevokeMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticRoleInvalid, request.container_uuid));
  }
  if (container_kind == "group" && FindGroup(loaded.state, request.container_uuid) == nullptr) {
    return DiagnosticResult<EngineSecurityRevokeMembershipResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGroupInvalid, request.container_uuid));
  }
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityMembershipRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.membership_uuid = StableToken("security-membership",
                                       request.member_principal_uuid + "|" +
                                           request.container_uuid + "|" + container_kind);
  record.member_principal_uuid = request.member_principal_uuid;
  record.container_uuid = request.container_uuid;
  record.container_kind = container_kind;
  record.grantor_principal_uuid = request.context.principal_uuid.canonical;
  record.security_generation = generation;
  record.revoked = true;
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              record.container_uuid,
                              generation,
                              "member=" + record.member_principal_uuid +
                                  ";container_kind=" + record.container_kind);
  const auto appended = AppendEvents(
      request.context,
      {MembershipEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, record.container_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityRevokeMembershipResult>(request.context,
                                                                 kOperation,
                                                                 appended);
  }

  auto result = SuccessResult<EngineSecurityRevokeMembershipResult>(request.context, kOperation);
  result.membership_revoked = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = record.membership_uuid;
  result.primary_object.object_kind = "security_membership";
  FillMutationEvidence(&result, kOperation, record.membership_uuid, generation);
  AddRow(&result,
         {{"membership_uuid", record.membership_uuid},
          {"member_principal_uuid", record.member_principal_uuid},
          {"container_uuid", record.container_uuid},
          {"container_kind", record.container_kind},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityGrantPrivilegeResult EngineSecurityGrantPrivilege(
    const EngineSecurityGrantPrivilegeRequest& request) {
  constexpr const char* kOperation = "security.privilege.grant";
  auto preflight =
      MutatingSetupFailure<EngineSecurityGrantPrivilegeResult>(request,
                                                               kOperation,
                                                               "SEC_GRANT_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string privilege = NormalizePrivilege(request.privilege);
  const std::string effect = request.grant_effect.empty() ? "allow" : request.grant_effect;
  const bool global_grant = request.target_object_uuid.empty();
  if (request.grantee_uuid.empty() ||
      (global_grant && !PrivilegeAllowsGlobalGrant(privilege)) ||
      privilege.empty() || (effect != "allow" && effect != "deny")) {
    return DiagnosticResult<EngineSecurityGrantPrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGrantInvalid,
                            global_grant ? "grantee_global_privilege_not_allowed"
                                         : "grantee_target_privilege_required"));
  }
  if (IsEngineOwnedSysarchRoleUuid(request.grantee_uuid)) {
    return DiagnosticResult<EngineSecurityGrantPrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
                            "engine_owned_sysarch_grants_are_catalog_derived"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityGrantPrivilegeResult>(request.context,
                                                               kOperation,
                                                               loaded.diagnostic);
  }
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityPrivilegeGrantRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.grant_uuid = request.grant_uuid.empty()
      ? StableToken("security-grant",
                    request.grantee_uuid + "|" + request.target_object_uuid + "|" + privilege)
      : request.grant_uuid;
  record.grantee_uuid = request.grantee_uuid;
  record.grantee_kind = request.grantee_kind.empty() ? "principal" : request.grantee_kind;
  record.target_object_uuid = request.target_object_uuid;
  record.target_object_kind = request.target_object_kind;
  record.privilege = privilege;
  record.grantor_principal_uuid = request.context.principal_uuid.canonical;
  record.grant_effect = effect;
  record.security_generation = generation;
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              record.target_object_uuid,
                              generation,
                              "grantee=" + record.grantee_uuid +
                                  ";privilege=" + record.privilege +
                                  ";effect=" + record.grant_effect);
  const auto appended = AppendEvents(
      request.context,
      {GrantEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, record.target_object_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityGrantPrivilegeResult>(request.context,
                                                               kOperation,
                                                               appended);
  }

  auto result = SuccessResult<EngineSecurityGrantPrivilegeResult>(request.context, kOperation);
  result.privilege_granted = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = record.grant_uuid;
  result.primary_object.object_kind = "security_privilege_grant";
  FillMutationEvidence(&result, kOperation, record.grant_uuid, generation);
  AddRow(&result,
         {{"grant_uuid", record.grant_uuid},
          {"grantee_uuid", record.grantee_uuid},
          {"grantee_kind", record.grantee_kind},
          {"target_object_uuid", record.target_object_uuid},
          {"privilege", record.privilege},
          {"grant_effect", record.grant_effect},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityRevokePrivilegeResult EngineSecurityRevokePrivilege(
    const EngineSecurityRevokePrivilegeRequest& request) {
  constexpr const char* kOperation = "security.privilege.revoke";
  auto preflight =
      MutatingSetupFailure<EngineSecurityRevokePrivilegeResult>(request,
                                                                kOperation,
                                                                "SEC_GRANT_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string privilege = NormalizePrivilege(request.privilege);
  if (request.grantee_uuid.empty() || request.target_object_uuid.empty() || privilege.empty()) {
    return DiagnosticResult<EngineSecurityRevokePrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGrantInvalid,
                            "grantee_target_privilege_required"));
  }
  if (IsEngineOwnedSysarchRoleUuid(request.grantee_uuid)) {
    return DiagnosticResult<EngineSecurityRevokePrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCatalogAuthorityRequired,
                            "engine_owned_sysarch_grants_are_catalog_derived"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityRevokePrivilegeResult>(request.context,
                                                                kOperation,
                                                                loaded.diagnostic);
  }
  const std::uint64_t generation = NextGeneration(loaded.state);
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              request.target_object_uuid,
                              generation,
                              "grantee=" + request.grantee_uuid + ";privilege=" + privilege);
  const auto appended = AppendEvents(
      request.context,
      {RevokeEvent(request.context.local_transaction_id,
                   request.grantee_uuid,
                   request.target_object_uuid,
                   privilege,
                   request.context.principal_uuid.canonical,
                   generation),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, request.target_object_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityRevokePrivilegeResult>(request.context,
                                                                kOperation,
                                                                appended);
  }

  auto result = SuccessResult<EngineSecurityRevokePrivilegeResult>(request.context, kOperation);
  result.privilege_revoked = true;
  result.security_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical =
      StableToken("security-revoke", request.grantee_uuid + "|" + request.target_object_uuid + "|" + privilege);
  result.primary_object.object_kind = "security_privilege_revoke";
  FillMutationEvidence(&result, kOperation, result.primary_object.uuid.canonical, generation);
  AddRow(&result,
         {{"grantee_uuid", request.grantee_uuid},
          {"target_object_uuid", request.target_object_uuid},
          {"privilege", privilege},
          {"security_generation", std::to_string(generation)}});
  return result;
}

EngineSecuritySetRoleResult EngineSecuritySetRole(
    const EngineSecuritySetRoleRequest& request) {
  constexpr const char* kOperation = "security.session.set_role";
  const auto boundary = ValidateEngineAuthorityBoundary(request, kOperation);
  if (boundary.error) {
    return DiagnosticResult<EngineSecuritySetRoleResult>(request.context,
                                                        kOperation,
                                                        boundary);
  }
  if (!request.context.security_context_present) {
    return DiagnosticResult<EngineSecuritySetRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                            "security_context_required"));
  }
  const std::string role_mode = request.role_mode.empty() ? "explicit" : request.role_mode;
  if (role_mode == "none") {
    auto result = SuccessResult<EngineSecuritySetRoleResult>(request.context, kOperation);
    result.role_set = true;
    result.security_generation = 0;
    AddEvidence(&result, "active_role", "none");
    AddRow(&result, {{"role_mode", "none"}, {"active_role_uuid", ""}});
    return result;
  }
  if (request.role_uuid.empty()) {
    return DiagnosticResult<EngineSecuritySetRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticRoleInvalid, "role_uuid_required"));
  }

  std::uint64_t security_generation = 0;
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecuritySetRoleResult>(request.context,
                                                        kOperation,
                                                        loaded.diagnostic);
  }
  security_generation = loaded.state.security_generation;
  if (FindRole(loaded.state, request.role_uuid) == nullptr) {
    return DiagnosticResult<EngineSecuritySetRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticRoleInvalid, request.role_uuid));
  }
  const auto grantees =
      EffectiveGranteeSet(loaded.state, request.context.principal_uuid.canonical);
  if (grantees.count(request.role_uuid) == 0) {
    return DiagnosticResult<EngineSecuritySetRoleResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticAccessDenied,
                            "role_not_granted:" + request.role_uuid));
  }

  auto result = SuccessResult<EngineSecuritySetRoleResult>(request.context, kOperation);
  result.role_set = true;
  result.active_role_uuid = request.role_uuid;
  result.security_generation = security_generation;
  result.primary_object.uuid.canonical = request.role_uuid;
  result.primary_object.object_kind = "security_role";
  AddEvidence(&result, "active_role", request.role_uuid);
  AddEvidence(&result, "security_generation", std::to_string(security_generation));
  AddRow(&result,
         {{"role_mode", role_mode},
          {"active_role_uuid", request.role_uuid},
          {"security_generation", std::to_string(security_generation)}});
  return result;
}

EngineSecurityAttachPolicyResult EngineSecurityAttachPolicy(
    const EngineSecurityAttachPolicyRequest& request) {
  constexpr const char* kOperation = "security.policy.attach";
  auto preflight =
      MutatingSetupFailure<EngineSecurityAttachPolicyResult>(request,
                                                             kOperation,
                                                             "POLICY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string policy_uuid = request.policy_uuid.empty()
      ? request.target_object.uuid.canonical
      : request.policy_uuid;
  const std::string target_uuid = request.target_object_uuid.empty()
      ? request.target_schema.uuid.canonical
      : request.target_object_uuid;
  if (policy_uuid.empty() || target_uuid.empty()) {
    return DiagnosticResult<EngineSecurityAttachPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing,
                            "policy_uuid_and_target_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityAttachPolicyResult>(request.context,
                                                             kOperation,
                                                             loaded.diagnostic);
  }
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRowPolicyRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.policy_uuid = policy_uuid;
  record.target_object_uuid = target_uuid;
  record.target_object_kind =
      request.target_object_kind.empty() ? "object" : request.target_object_kind;
  record.policy_effect = request.policy_effect.empty() ? "attach" : request.policy_effect;
  record.predicate_envelope =
      RedactSecurityPrincipalProtectedMaterialForDiagnostics(request.predicate_envelope);
  record.definer_principal_uuid = request.definer_principal_uuid.empty()
      ? request.context.principal_uuid.canonical
      : request.definer_principal_uuid;
  record.lifecycle_state = "active";
  record.policy_generation = generation;
  if (request.native_authority.present) {
    if (!NativeRowPolicyAuthorityValid(request.context,
                                       request.native_authority)) {
      return DiagnosticResult<EngineSecurityAttachPolicyResult>(
          request.context, kOperation,
          PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                              "native_policy_authority_invalid"));
    }
    ApplyNativeRowPolicyAuthority(request.native_authority,
                                  request.context, generation,
                                  &record);
  }
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              target_uuid,
                              generation,
                              "policy=" + policy_uuid +
                                  ";target_kind=" + record.target_object_kind +
                                  ";scope=" + request.policy_scope);
  const auto appended = AppendEvents(
      request.context,
      {RowPolicyEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, target_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityAttachPolicyResult>(request.context,
                                                             kOperation,
                                                             appended);
  }

  auto result = SuccessResult<EngineSecurityAttachPolicyResult>(request.context, kOperation);
  result.policy_attached = true;
  result.policy_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_policy";
  FillMutationEvidence(&result, kOperation, policy_uuid, generation);
  AddRow(&result,
         {{"policy_uuid", policy_uuid},
          {"target_object_uuid", target_uuid},
          {"target_object_kind", record.target_object_kind},
          {"policy_scope", request.policy_scope},
          {"policy_effect", record.policy_effect},
          {"policy_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityCreatePolicyResult EngineSecurityCreatePolicy(
    const EngineSecurityCreatePolicyRequest& request) {
  constexpr const char* kOperation = "security.policy.create";
  auto preflight =
      MutatingSetupFailure<EngineSecurityCreatePolicyResult>(request,
                                                             kOperation,
                                                             "POLICY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string policy_uuid = request.policy_uuid.empty()
      ? request.target_object.uuid.canonical
      : request.policy_uuid;
  const std::string target_uuid = request.target_object_uuid.empty()
      ? request.target_schema.uuid.canonical
      : request.target_object_uuid;
  const std::string policy_name = PrimaryName(request, request.policy_name);
  if (policy_uuid.empty() || target_uuid.empty()) {
    return DiagnosticResult<EngineSecurityCreatePolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing,
                            "policy_uuid_and_target_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityCreatePolicyResult>(request.context,
                                                             kOperation,
                                                             loaded.diagnostic);
  }
  if (FindRowPolicy(loaded.state, policy_uuid) != nullptr) {
    return DiagnosticResult<EngineSecurityCreatePolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyDuplicate, policy_uuid));
  }

  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRowPolicyRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.policy_uuid = policy_uuid;
  record.target_object_uuid = target_uuid;
  record.target_object_kind =
      request.target_object_kind.empty() ? "object" : request.target_object_kind;
  record.policy_effect = request.policy_effect.empty() ? "row_filter" : request.policy_effect;
  record.predicate_envelope = request.predicate_envelope.empty()
      ? "predicate:true"
      : RedactSecurityPrincipalProtectedMaterialForDiagnostics(request.predicate_envelope);
  record.definer_principal_uuid = request.definer_principal_uuid.empty()
      ? request.context.principal_uuid.canonical
      : request.definer_principal_uuid;
  record.lifecycle_state = "active";
  record.policy_generation = generation;
  if (request.native_authority.present) {
    if (!NativeRowPolicyAuthorityValid(request.context,
                                       request.native_authority)) {
      return DiagnosticResult<EngineSecurityCreatePolicyResult>(
          request.context, kOperation,
          PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                              "native_policy_authority_invalid"));
    }
    ApplyNativeRowPolicyAuthority(request.native_authority,
                                  request.context, generation,
                                  &record);
  }
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              policy_uuid,
                              generation,
                              "target=" + target_uuid +
                                  ";target_kind=" + record.target_object_kind +
                                  ";effect=" + record.policy_effect);
  const auto appended = AppendEvents(
      request.context,
      {RowPolicyEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, target_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityCreatePolicyResult>(request.context,
                                                             kOperation,
                                                             appended);
  }
  std::vector<std::string> policy_classes = {"policy", "security_policy"};
  const std::string effect = LowerAscii(record.policy_effect);
  if (effect.find("mask") != std::string::npos) {
    policy_classes.push_back("mask");
  }
  if (effect.find("rls") != std::string::npos) {
    policy_classes.push_back("rls");
  }
  std::string policy_name_scope_uuid = request.target_schema_uuid;
  if (policy_name_scope_uuid.empty()) {
    policy_name_scope_uuid = request.context.current_schema_uuid.canonical;
  }
  const auto resolver = PersistSecurityNameAliases(
      request.context,
      kOperation,
      policy_uuid,
      policy_classes,
      request.localized_names,
      policy_name.empty() ? policy_uuid : policy_name,
      policy_name_scope_uuid);
  if (resolver.error) {
    return DiagnosticResult<EngineSecurityCreatePolicyResult>(request.context,
                                                             kOperation,
                                                             resolver);
  }

  auto result = SuccessResult<EngineSecurityCreatePolicyResult>(request.context, kOperation);
  result.policy_created = true;
  result.policy_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_policy";
  FillMutationEvidence(&result, kOperation, policy_uuid, generation);
  AddRow(&result,
         {{"policy_uuid", policy_uuid},
          {"policy_name", policy_name},
          {"target_object_uuid", target_uuid},
          {"target_object_kind", record.target_object_kind},
          {"policy_effect", record.policy_effect},
          {"predicate_envelope", record.predicate_envelope},
          {"definer_principal_uuid", record.definer_principal_uuid},
          {"lifecycle_state", record.lifecycle_state},
          {"policy_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityAlterPolicyResult EngineSecurityAlterPolicy(
    const EngineSecurityAlterPolicyRequest& request) {
  constexpr const char* kOperation = "security.policy.alter";
  auto preflight =
      MutatingSetupFailure<EngineSecurityAlterPolicyResult>(request,
                                                            kOperation,
                                                            "POLICY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string policy_uuid = request.policy_uuid.empty()
      ? request.target_object.uuid.canonical
      : request.policy_uuid;
  if (policy_uuid.empty()) {
    return DiagnosticResult<EngineSecurityAlterPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, "policy_uuid_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityAlterPolicyResult>(request.context,
                                                            kOperation,
                                                            loaded.diagnostic);
  }
  const auto* existing = FindRowPolicy(loaded.state, policy_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<EngineSecurityAlterPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, policy_uuid));
  }
  std::string lifecycle = NormalizePolicyLifecycle(request.lifecycle_state);
  if (lifecycle.empty()) {
    lifecycle = existing->lifecycle_state.empty() ? "active" : existing->lifecycle_state;
  }
  if (!PolicyLifecycleValid(lifecycle)) {
    return DiagnosticResult<EngineSecurityAlterPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing,
                            "lifecycle_state:" + lifecycle));
  }

  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRowPolicyRecord record = *existing;
  record.creator_tx = request.context.local_transaction_id;
  if (!request.target_object_uuid.empty()) { record.target_object_uuid = request.target_object_uuid; }
  if (!request.target_object_kind.empty()) { record.target_object_kind = request.target_object_kind; }
  if (!request.policy_effect.empty()) { record.policy_effect = request.policy_effect; }
  if (!request.predicate_envelope.empty()) {
    record.predicate_envelope =
        RedactSecurityPrincipalProtectedMaterialForDiagnostics(request.predicate_envelope);
    if (!request.native_authority.present) {
      ClearNativeRowPolicyAuthority(&record);
    }
  }
  if (!request.definer_principal_uuid.empty()) {
    record.definer_principal_uuid = request.definer_principal_uuid;
  }
  record.lifecycle_state = lifecycle;
  record.policy_generation = generation;
  record.deleted = false;
  if (request.native_authority.present) {
    if (!NativeRowPolicyAuthorityValid(request.context,
                                       request.native_authority)) {
      return DiagnosticResult<EngineSecurityAlterPolicyResult>(
          request.context, kOperation,
          PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                              "native_policy_authority_invalid"));
    }
    ApplyNativeRowPolicyAuthority(request.native_authority,
                                  request.context, generation,
                                  &record);
  }
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              policy_uuid,
                              generation,
                              "target=" + record.target_object_uuid +
                                  ";target_kind=" + record.target_object_kind +
                                  ";effect=" + record.policy_effect +
                                  ";lifecycle_state=" + lifecycle);
  const auto appended = AppendEvents(
      request.context,
      {RowPolicyEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, policy_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityAlterPolicyResult>(request.context,
                                                            kOperation,
                                                            appended);
  }

  auto result = SuccessResult<EngineSecurityAlterPolicyResult>(request.context, kOperation);
  result.policy_altered = true;
  result.policy_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_policy";
  FillMutationEvidence(&result, kOperation, policy_uuid, generation);
  AddRow(&result,
         {{"policy_uuid", policy_uuid},
          {"target_object_uuid", record.target_object_uuid},
          {"target_object_kind", record.target_object_kind},
          {"policy_effect", record.policy_effect},
          {"predicate_envelope", record.predicate_envelope},
          {"definer_principal_uuid", record.definer_principal_uuid},
          {"lifecycle_state", lifecycle},
          {"policy_generation", std::to_string(generation)}});
  return result;
}

namespace {

template <typename TResult, typename MarkDropped>
TResult DropPolicyLike(const EngineApiRequest& request,
                       std::string policy_uuid,
                       const char* operation_id,
                       std::string object_kind,
                       std::string uuid_field_name,
                       MarkDropped mark_dropped) {
  auto preflight =
      MutatingSetupFailure<TResult>(request, operation_id, "POLICY_ADMIN");
  if (!preflight.ok) { return preflight; }
  if (policy_uuid.empty()) {
    policy_uuid = request.target_object.uuid.canonical;
  }
  if (policy_uuid.empty()) {
    return DiagnosticResult<TResult>(
        request.context,
        operation_id,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, "policy_uuid_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<TResult>(request.context,
                                     operation_id,
                                     loaded.diagnostic);
  }
  const auto* existing = FindRowPolicy(loaded.state, policy_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<TResult>(
        request.context,
        operation_id,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, policy_uuid));
  }

  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRowPolicyRecord record = *existing;
  record.creator_tx = request.context.local_transaction_id;
  record.lifecycle_state = "dropped";
  record.policy_generation = generation;
  record.deleted = true;
  const std::string cache_target = record.target_object_uuid.empty()
      ? policy_uuid
      : record.target_object_uuid;
  const auto audit = MakeAudit(request.context,
                              operation_id,
                              policy_uuid,
                              generation,
                              "target=" + record.target_object_uuid +
                                  ";target_kind=" + record.target_object_kind +
                                  ";effect=" + record.policy_effect +
                                  ";lifecycle_state=dropped");
  const auto appended = AppendEvents(
      request.context,
      {RowPolicyEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, operation_id, cache_target, generation)});
  if (appended.error) {
    return DiagnosticResult<TResult>(request.context, operation_id, appended);
  }
  const auto resolver = RetireNameRegistryEntriesForObject(request.context,
                                                           operation_id,
                                                           policy_uuid);
  if (resolver.error) {
    return DiagnosticResult<TResult>(request.context, operation_id, resolver);
  }

  auto result = SuccessResult<TResult>(request.context, operation_id);
  mark_dropped(&result);
  result.policy_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = std::move(object_kind);
  FillMutationEvidence(&result, operation_id, policy_uuid, generation);
  AddRow(&result,
         {{std::move(uuid_field_name), policy_uuid},
          {"target_object_uuid", record.target_object_uuid},
          {"target_object_kind", record.target_object_kind},
          {"policy_effect", record.policy_effect},
          {"lifecycle_state", "dropped"},
          {"deleted", "true"},
          {"policy_generation", std::to_string(generation)}});
  return result;
}

}  // namespace

EngineSecurityDropPolicyResult EngineSecurityDropPolicy(
    const EngineSecurityDropPolicyRequest& request) {
  return DropPolicyLike<EngineSecurityDropPolicyResult>(
      request,
      request.policy_uuid,
      "security.policy.drop",
      "security_policy",
      "policy_uuid",
      [](EngineSecurityDropPolicyResult* result) { result->policy_dropped = true; });
}

EngineSecurityDropMaskResult EngineSecurityDropMask(
    const EngineSecurityDropMaskRequest& request) {
  return DropPolicyLike<EngineSecurityDropMaskResult>(
      request,
      request.mask_uuid,
      "security.mask.drop",
      "security_mask",
      "mask_uuid",
      [](EngineSecurityDropMaskResult* result) { result->mask_dropped = true; });
}

EngineSecurityDropRlsResult EngineSecurityDropRls(
    const EngineSecurityDropRlsRequest& request) {
  return DropPolicyLike<EngineSecurityDropRlsResult>(
      request,
      request.rls_uuid,
      "security.rls.drop",
      "security_rls",
      "rls_uuid",
      [](EngineSecurityDropRlsResult* result) { result->rls_dropped = true; });
}

EngineSecurityActivatePolicyResult EngineSecurityActivatePolicy(
    const EngineSecurityActivatePolicyRequest& request) {
  constexpr const char* kOperation = "security.policy.activate";
  auto preflight =
      MutatingSetupFailure<EngineSecurityActivatePolicyResult>(request,
                                                               kOperation,
                                                               "POLICY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string policy_uuid = request.policy_uuid.empty()
      ? request.target_object.uuid.canonical
      : request.policy_uuid;
  if (policy_uuid.empty()) {
    return DiagnosticResult<EngineSecurityActivatePolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, "policy_uuid_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityActivatePolicyResult>(request.context,
                                                               kOperation,
                                                               loaded.diagnostic);
  }
  const auto* existing = FindRowPolicy(loaded.state, policy_uuid);
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRowPolicyRecord record =
      existing == nullptr ? EngineSecurityRowPolicyRecord{} : *existing;
  record.creator_tx = request.context.local_transaction_id;
  record.policy_uuid = policy_uuid;
  record.target_object_uuid = existing == nullptr ? policy_uuid : existing->target_object_uuid;
  record.target_object_kind = existing == nullptr ? "security_policy"
                                                  : existing->target_object_kind;
  record.policy_effect = existing == nullptr ? "activate" : existing->policy_effect;
  record.predicate_envelope = existing == nullptr ? "" : existing->predicate_envelope;
  record.definer_principal_uuid = existing == nullptr
      ? request.context.principal_uuid.canonical
      : existing->definer_principal_uuid;
  record.lifecycle_state = "active";
  record.policy_generation = generation;
  const auto audit = MakeAudit(request.context, kOperation, policy_uuid, generation, "activate");
  const auto appended = AppendEvents(
      request.context,
      {RowPolicyEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, policy_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityActivatePolicyResult>(request.context,
                                                               kOperation,
                                                               appended);
  }
  auto result = SuccessResult<EngineSecurityActivatePolicyResult>(request.context, kOperation);
  result.policy_activated = true;
  result.policy_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_policy";
  FillMutationEvidence(&result, kOperation, policy_uuid, generation);
  AddRow(&result,
         {{"policy_uuid", policy_uuid},
          {"lifecycle_state", "active"},
          {"policy_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityDeactivatePolicyResult EngineSecurityDeactivatePolicy(
    const EngineSecurityDeactivatePolicyRequest& request) {
  constexpr const char* kOperation = "security.policy.deactivate";
  auto preflight =
      MutatingSetupFailure<EngineSecurityDeactivatePolicyResult>(request,
                                                                 kOperation,
                                                                 "POLICY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string policy_uuid = request.policy_uuid.empty()
      ? request.target_object.uuid.canonical
      : request.policy_uuid;
  if (policy_uuid.empty()) {
    return DiagnosticResult<EngineSecurityDeactivatePolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, "policy_uuid_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityDeactivatePolicyResult>(request.context,
                                                                 kOperation,
                                                                 loaded.diagnostic);
  }
  const auto* existing = FindRowPolicy(loaded.state, policy_uuid);
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRowPolicyRecord record =
      existing == nullptr ? EngineSecurityRowPolicyRecord{} : *existing;
  record.creator_tx = request.context.local_transaction_id;
  record.policy_uuid = policy_uuid;
  record.target_object_uuid = existing == nullptr ? policy_uuid : existing->target_object_uuid;
  record.target_object_kind = existing == nullptr ? "security_policy"
                                                  : existing->target_object_kind;
  record.policy_effect = existing == nullptr ? "deactivate" : existing->policy_effect;
  record.predicate_envelope = existing == nullptr ? "" : existing->predicate_envelope;
  record.definer_principal_uuid = existing == nullptr
      ? request.context.principal_uuid.canonical
      : existing->definer_principal_uuid;
  record.lifecycle_state = "inactive";
  record.policy_generation = generation;
  const auto audit = MakeAudit(request.context, kOperation, policy_uuid, generation, "deactivate");
  const auto appended = AppendEvents(
      request.context,
      {RowPolicyEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, policy_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityDeactivatePolicyResult>(request.context,
                                                                 kOperation,
                                                                 appended);
  }
  auto result = SuccessResult<EngineSecurityDeactivatePolicyResult>(request.context, kOperation);
  result.policy_deactivated = true;
  result.policy_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_policy";
  FillMutationEvidence(&result, kOperation, policy_uuid, generation);
  AddRow(&result,
         {{"policy_uuid", policy_uuid},
          {"lifecycle_state", "inactive"},
          {"policy_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityValidatePolicyResult EngineSecurityValidatePolicy(
    const EngineSecurityValidatePolicyRequest& request) {
  constexpr const char* kOperation = "security.policy.validate";
  const auto authority = ValidateSecurityAuthority(request, kOperation, "POLICY_ADMIN");
  if (authority.error) {
    return DiagnosticResult<EngineSecurityValidatePolicyResult>(request.context,
                                                               kOperation,
                                                               authority);
  }
  const std::string policy_uuid = request.policy_uuid.empty()
      ? request.target_object.uuid.canonical
      : request.policy_uuid;
  if (policy_uuid.empty()) {
    return DiagnosticResult<EngineSecurityValidatePolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, "policy_uuid_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityValidatePolicyResult>(request.context,
                                                               kOperation,
                                                               loaded.diagnostic);
  }
  const bool stale =
      (request.observed_policy_generation != 0 &&
       request.observed_policy_generation < loaded.state.policy_generation) ||
      (request.observed_cache_invalidation_epoch != 0 &&
       request.observed_cache_invalidation_epoch < loaded.state.cache_invalidation_epoch);
  if (stale) {
    auto result = DiagnosticResult<EngineSecurityValidatePolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyStale,
                            std::to_string(request.observed_policy_generation) + "<" +
                                std::to_string(loaded.state.policy_generation)));
    result.stale_policy_refused = true;
    result.current_policy_generation = loaded.state.policy_generation;
    result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
    return result;
  }
  if (FindRowPolicy(loaded.state, policy_uuid) == nullptr) {
    return DiagnosticResult<EngineSecurityValidatePolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, policy_uuid));
  }
  auto result = SuccessResult<EngineSecurityValidatePolicyResult>(request.context, kOperation);
  result.policy_valid = true;
  result.current_policy_generation = loaded.state.policy_generation;
  result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_policy";
  AddEvidence(&result, "security_policy_valid", policy_uuid);
  AddEvidence(&result, "policy_generation", std::to_string(loaded.state.policy_generation));
  AddRow(&result,
         {{"policy_uuid", policy_uuid},
          {"decision", "valid"},
          {"policy_generation", std::to_string(loaded.state.policy_generation)},
          {"cache_invalidation_epoch",
           std::to_string(loaded.state.cache_invalidation_epoch)}});
  return result;
}

EngineSecurityShowPolicyResult EngineSecurityShowPolicy(
    const EngineSecurityShowPolicyRequest& request) {
  constexpr const char* kOperation = "security.policy.show";
  const auto authority = ValidateSecurityAuthority(request, kOperation, "POLICY_ADMIN");
  if (authority.error) {
    return DiagnosticResult<EngineSecurityShowPolicyResult>(request.context,
                                                           kOperation,
                                                           authority);
  }
  const std::string policy_uuid = request.policy_uuid.empty()
      ? request.target_object.uuid.canonical
      : request.policy_uuid;
  if (policy_uuid.empty()) {
    return DiagnosticResult<EngineSecurityShowPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, "policy_uuid_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityShowPolicyResult>(request.context,
                                                           kOperation,
                                                           loaded.diagnostic);
  }
  const auto* policy = FindRowPolicy(loaded.state, policy_uuid);
  if (policy == nullptr) {
    return DiagnosticResult<EngineSecurityShowPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing, policy_uuid));
  }
  auto result = SuccessResult<EngineSecurityShowPolicyResult>(request.context, kOperation);
  result.policy_found = true;
  result.policy = *policy;
  result.current_policy_generation = loaded.state.policy_generation;
  result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_policy";
  AddEvidence(&result, "security_policy", policy_uuid);
  AddEvidence(&result, "policy_generation", std::to_string(loaded.state.policy_generation));
  if (request.include_rows) {
    AddRow(&result,
           {{"policy_uuid", policy->policy_uuid},
            {"target_object_uuid", policy->target_object_uuid},
            {"target_object_kind", policy->target_object_kind},
            {"policy_effect", policy->policy_effect},
            {"predicate_envelope", policy->predicate_envelope},
            {"definer_principal_uuid", policy->definer_principal_uuid},
            {"lifecycle_state", policy->lifecycle_state},
            {"policy_generation", std::to_string(policy->policy_generation)}});
  }
  return result;
}

EngineSecurityEvaluatePrivilegeResult EngineSecurityEvaluatePrivilege(
    const EngineSecurityEvaluatePrivilegeRequest& request) {
  constexpr const char* kOperation = "security.privilege.evaluate";
  const auto boundary = ValidateEngineAuthorityBoundary(request, kOperation);
  if (boundary.error) {
    return DiagnosticResult<EngineSecurityEvaluatePrivilegeResult>(request.context,
                                                                  kOperation,
                                                                  boundary);
  }
  if (!request.context.security_context_present) {
    return DiagnosticResult<EngineSecurityEvaluatePrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                            "security_context_required"));
  }
  const std::string principal_uuid = !request.principal_uuid.empty()
      ? request.principal_uuid
      : request.context.principal_uuid.canonical;
  const std::string target_uuid = !request.target_object_uuid.empty()
      ? request.target_object_uuid
      : request.target_object.uuid.canonical;
  const std::string privilege = NormalizePrivilege(request.privilege);
  if (principal_uuid.empty() || target_uuid.empty() || privilege.empty()) {
    return DiagnosticResult<EngineSecurityEvaluatePrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGrantInvalid,
                            "principal_target_privilege_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityEvaluatePrivilegeResult>(request.context,
                                                                  kOperation,
                                                                  loaded.diagnostic);
  }
  if (FindPrincipal(loaded.state, principal_uuid) == nullptr) {
    return DiagnosticResult<EngineSecurityEvaluatePrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPrincipalInvalid, principal_uuid));
  }
  const auto decision = EvaluateGrantState(loaded.state, principal_uuid, target_uuid, privilege);
  if (decision.allowed && !decision.explicit_deny) {
    auto result = SuccessResult<EngineSecurityEvaluatePrivilegeResult>(request.context, kOperation);
    result.authorized = true;
    result.decision = "allow";
    result.matched_grant_uuids = decision.matched_grants;
    result.security_generation = loaded.state.security_generation;
    AddEvidence(&result, "authorization_decision", "allow:" + privilege);
    for (const auto& grant_uuid : decision.matched_grants) {
      AddEvidence(&result, "matched_grant", grant_uuid);
    }
    AddRow(&result,
           {{"decision", "allow"},
            {"principal_uuid", principal_uuid},
            {"target_object_uuid", target_uuid},
            {"privilege", privilege},
            {"security_generation", std::to_string(loaded.state.security_generation)}});
    return result;
  }
  if (decision.explicit_deny) {
    auto result = DiagnosticResult<EngineSecurityEvaluatePrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticAccessDenied, "explicit_deny:" + privilege));
    result.decision = "deny";
    result.security_generation = loaded.state.security_generation;
    return result;
  }

  const auto all = LoadState(request.context, {.enforce_visibility = false});
  if (all.ok) {
    const auto invisible = EvaluateGrantState(all.state, principal_uuid, target_uuid, privilege);
    if (invisible.allowed) {
      auto result = DiagnosticResult<EngineSecurityEvaluatePrivilegeResult>(
          request.context,
          kOperation,
          PrincipalDiagnostic(kSecurityPrincipalDiagnosticGrantNotVisible,
                              target_uuid + ":" + privilege));
      result.decision = "deny";
      result.security_generation = loaded.state.security_generation;
      return result;
    }
  }

  auto result = DiagnosticResult<EngineSecurityEvaluatePrivilegeResult>(
      request.context,
      kOperation,
      PrincipalDiagnostic(kSecurityPrincipalDiagnosticDefaultDeny, target_uuid + ":" + privilege));
  result.decision = "deny";
  result.security_generation = loaded.state.security_generation;
  AddRow(&result,
         {{"decision", "deny"},
          {"principal_uuid", principal_uuid},
          {"target_object_uuid", target_uuid},
          {"privilege", privilege},
          {"reason", "default_deny"}});
  return result;
}

EngineSecurityPutRowPolicyResult EngineSecurityPutRowPolicy(
    const EngineSecurityPutRowPolicyRequest& request) {
  constexpr const char* kOperation = "security.row_policy.put";
  auto preflight =
      MutatingSetupFailure<EngineSecurityPutRowPolicyResult>(request,
                                                             kOperation,
                                                             "POLICY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string policy_uuid = request.policy_uuid.empty()
      ? request.target_object.uuid.canonical
      : request.policy_uuid;
  const std::string target_uuid = request.target_object_uuid.empty()
      ? request.target_schema.uuid.canonical
      : request.target_object_uuid;
  if (policy_uuid.empty() || target_uuid.empty()) {
    return DiagnosticResult<EngineSecurityPutRowPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyMissing,
                            "policy_uuid_and_target_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityPutRowPolicyResult>(request.context,
                                                             kOperation,
                                                             loaded.diagnostic);
  }
  const std::uint64_t generation = NextGeneration(loaded.state);
  EngineSecurityRowPolicyRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.policy_uuid = policy_uuid;
  record.target_object_uuid = target_uuid;
  record.target_object_kind = request.target_object_kind;
  record.policy_effect = request.policy_effect.empty() ? "deny_all" : request.policy_effect;
  record.predicate_envelope =
      RedactSecurityPrincipalProtectedMaterialForDiagnostics(request.predicate_envelope);
  record.definer_principal_uuid = request.definer_principal_uuid;
  record.policy_generation = generation;
  if (request.native_authority.present) {
    if (!NativeRowPolicyAuthorityValid(request.context,
                                       request.native_authority)) {
      return DiagnosticResult<EngineSecurityPutRowPolicyResult>(
          request.context, kOperation,
          PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                              "native_policy_authority_invalid"));
    }
    ApplyNativeRowPolicyAuthority(request.native_authority,
                                  request.context, generation,
                                  &record);
  }
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              target_uuid,
                              generation,
                              "policy=" + policy_uuid + ";effect=" + record.policy_effect +
                                  ";predicate=" + request.predicate_envelope);
  const auto appended = AppendEvents(
      request.context,
      {RowPolicyEvent(record),
       AuditEvent(audit),
       CacheInvalidationEvent(request.context, kOperation, target_uuid, generation)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityPutRowPolicyResult>(request.context,
                                                             kOperation,
                                                             appended);
  }

  auto result = SuccessResult<EngineSecurityPutRowPolicyResult>(request.context, kOperation);
  result.policy_persisted = true;
  result.policy_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_row_policy";
  FillMutationEvidence(&result, kOperation, policy_uuid, generation);
  AddRow(&result,
         {{"policy_uuid", policy_uuid},
          {"target_object_uuid", target_uuid},
          {"policy_effect", record.policy_effect},
          {"predicate_envelope", record.predicate_envelope},
          {"policy_generation", std::to_string(generation)}});
  return result;
}

EngineSecurityEvaluateRowPolicyResult EngineSecurityEvaluateRowPolicy(
    const EngineSecurityEvaluateRowPolicyRequest& request) {
  constexpr const char* kOperation = "security.row_policy.evaluate";
  const auto boundary = ValidateEngineAuthorityBoundary(request, kOperation);
  if (boundary.error) {
    return DiagnosticResult<EngineSecurityEvaluateRowPolicyResult>(request.context,
                                                                  kOperation,
                                                                  boundary);
  }
  if (!request.context.security_context_present) {
    return DiagnosticResult<EngineSecurityEvaluateRowPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                            "security_context_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityEvaluateRowPolicyResult>(request.context,
                                                                  kOperation,
                                                                  loaded.diagnostic);
  }
  if (request.observed_policy_generation != 0 &&
      request.observed_policy_generation < loaded.state.policy_generation) {
    auto result = DiagnosticResult<EngineSecurityEvaluateRowPolicyResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticPolicyStale,
                            std::to_string(request.observed_policy_generation) + "<" +
                                std::to_string(loaded.state.policy_generation)));
    result.stale_policy_refused = true;
    result.policy_generation = loaded.state.policy_generation;
    return result;
  }
  const std::string principal_uuid = !request.principal_uuid.empty()
      ? request.principal_uuid
      : request.context.principal_uuid.canonical;
  const std::string target_uuid = !request.target_object_uuid.empty()
      ? request.target_object_uuid
      : request.target_object.uuid.canonical;
  for (const auto& policy : loaded.state.row_policies) {
    if (policy.target_object_uuid != target_uuid) { continue; }
    bool allow = false;
    if (policy.policy_effect == "allow_all") {
      allow = true;
    } else if (policy.policy_effect == "allow_owner") {
      allow = request.row_owner_principal_uuid == principal_uuid;
    } else if (policy.policy_effect == "allow_tag") {
      allow = HasTraceTag(request.context, policy.predicate_envelope);
    } else if (policy.policy_effect == "deny_all") {
      allow = false;
    }
    if (allow) {
      auto result = SuccessResult<EngineSecurityEvaluateRowPolicyResult>(request.context, kOperation);
      result.row_visible = true;
      result.decision = "allow";
      result.policy_generation = loaded.state.policy_generation;
      AddEvidence(&result, "row_security_policy", policy.policy_uuid);
      AddRow(&result,
             {{"decision", "allow"},
              {"policy_uuid", policy.policy_uuid},
              {"target_object_uuid", target_uuid},
              {"policy_generation", std::to_string(loaded.state.policy_generation)}});
      return result;
    }
  }

  auto result = DiagnosticResult<EngineSecurityEvaluateRowPolicyResult>(
      request.context,
      kOperation,
      PrincipalDiagnostic(kSecurityPrincipalDiagnosticAccessDenied,
                          "row_policy_denied:" + target_uuid));
  result.decision = "deny";
  result.policy_generation = loaded.state.policy_generation;
  AddRow(&result,
         {{"decision", "deny"},
          {"target_object_uuid", target_uuid},
          {"policy_generation", std::to_string(loaded.state.policy_generation)}});
  return result;
}

EngineSecurityPrimeDefinerRightsCacheResult EngineSecurityPrimeDefinerRightsCache(
    const EngineSecurityPrimeDefinerRightsCacheRequest& request) {
  constexpr const char* kOperation = "security.definer_rights_cache.prime";
  auto preflight =
      MutatingSetupFailure<EngineSecurityPrimeDefinerRightsCacheResult>(request,
                                                                        kOperation,
                                                                        "POLICY_ADMIN");
  if (!preflight.ok) { return preflight; }
  const std::string privilege = NormalizePrivilege(request.privilege);
  if (request.definer_principal_uuid.empty() || request.target_object_uuid.empty() ||
      privilege.empty()) {
    return DiagnosticResult<EngineSecurityPrimeDefinerRightsCacheResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGrantInvalid,
                            "definer_target_privilege_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityPrimeDefinerRightsCacheResult>(request.context,
                                                                        kOperation,
                                                                        loaded.diagnostic);
  }
  const auto grant = EvaluateGrantState(loaded.state,
                                       request.definer_principal_uuid,
                                       request.target_object_uuid,
                                       privilege);
  if (!grant.allowed || grant.explicit_deny) {
    return DiagnosticResult<EngineSecurityPrimeDefinerRightsCacheResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticDefaultDeny,
                            request.target_object_uuid + ":" + privilege));
  }
  EngineSecurityDefinerRightsCacheRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.definer_principal_uuid = request.definer_principal_uuid;
  record.target_object_uuid = request.target_object_uuid;
  record.privilege = privilege;
  record.decision = "allow";
  record.policy_generation = loaded.state.policy_generation;
  record.cache_key = StableToken("definer-cache",
                                 record.definer_principal_uuid + "|" +
                                     record.target_object_uuid + "|" + record.privilege +
                                     "|" + std::to_string(record.policy_generation));
  const auto audit = MakeAudit(request.context,
                              kOperation,
                              record.target_object_uuid,
                              loaded.state.policy_generation,
                              "definer=" + record.definer_principal_uuid +
                                  ";privilege=" + record.privilege);
  const auto appended = AppendEvents(request.context,
                                     {DefinerCacheEvent(record), AuditEvent(audit)});
  if (appended.error) {
    return DiagnosticResult<EngineSecurityPrimeDefinerRightsCacheResult>(request.context,
                                                                        kOperation,
                                                                        appended);
  }

  auto result = SuccessResult<EngineSecurityPrimeDefinerRightsCacheResult>(request.context,
                                                                          kOperation);
  result.cached = true;
  result.cache_key = record.cache_key;
  result.policy_generation = record.policy_generation;
  AddEvidence(&result, "definer_rights_cache", record.cache_key);
  AddEvidence(&result, "policy_generation", std::to_string(record.policy_generation));
  AddRow(&result,
         {{"cache_key", record.cache_key},
          {"definer_principal_uuid", record.definer_principal_uuid},
          {"target_object_uuid", record.target_object_uuid},
          {"privilege", record.privilege},
          {"policy_generation", std::to_string(record.policy_generation)}});
  return result;
}

EngineSecurityValidateDefinerRightsCacheResult EngineSecurityValidateDefinerRightsCache(
    const EngineSecurityValidateDefinerRightsCacheRequest& request) {
  constexpr const char* kOperation = "security.definer_rights_cache.validate";
  const auto boundary = ValidateEngineAuthorityBoundary(request, kOperation);
  if (boundary.error) {
    return DiagnosticResult<EngineSecurityValidateDefinerRightsCacheResult>(request.context,
                                                                           kOperation,
                                                                           boundary);
  }
  if (!request.context.security_context_present) {
    return DiagnosticResult<EngineSecurityValidateDefinerRightsCacheResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                            "security_context_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityValidateDefinerRightsCacheResult>(request.context,
                                                                           kOperation,
                                                                           loaded.diagnostic);
  }
  const bool stale_generation =
      (request.observed_policy_generation != 0 &&
       request.observed_policy_generation < loaded.state.policy_generation) ||
      (request.observed_cache_invalidation_epoch != 0 &&
       request.observed_cache_invalidation_epoch < loaded.state.cache_invalidation_epoch);
  if (stale_generation) {
    auto result = DiagnosticResult<EngineSecurityValidateDefinerRightsCacheResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCacheStale,
                            std::to_string(request.observed_policy_generation) + "<" +
                                std::to_string(loaded.state.policy_generation)));
    result.stale_policy_refused = true;
    result.current_policy_generation = loaded.state.policy_generation;
    result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
    return result;
  }
  const EngineSecurityDefinerRightsCacheRecord* found = nullptr;
  for (const auto& cache : loaded.state.definer_rights_cache) {
    if (cache.cache_key == request.cache_key) {
      found = &cache;
      break;
    }
  }
  if (found == nullptr) {
    auto result = DiagnosticResult<EngineSecurityValidateDefinerRightsCacheResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCacheMissing, request.cache_key));
    result.current_policy_generation = loaded.state.policy_generation;
    result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
    return result;
  }
  const std::string definer = request.definer_principal_uuid.empty()
      ? found->definer_principal_uuid
      : request.definer_principal_uuid;
  const std::string target = request.target_object_uuid.empty()
      ? found->target_object_uuid
      : request.target_object_uuid;
  const std::string privilege = request.privilege.empty()
      ? found->privilege
      : NormalizePrivilege(request.privilege);
  const auto current_grant = EvaluateGrantState(loaded.state, definer, target, privilege);
  if (!current_grant.allowed || current_grant.explicit_deny) {
    auto result = DiagnosticResult<EngineSecurityValidateDefinerRightsCacheResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCacheStale,
                            "definer_rights_no_longer_match"));
    result.stale_policy_refused = true;
    result.current_policy_generation = loaded.state.policy_generation;
    result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
    return result;
  }

  auto result = SuccessResult<EngineSecurityValidateDefinerRightsCacheResult>(request.context,
                                                                             kOperation);
  result.cache_valid = true;
  result.current_policy_generation = loaded.state.policy_generation;
  result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
  AddEvidence(&result, "definer_rights_cache_valid", found->cache_key);
  AddRow(&result,
         {{"cache_key", found->cache_key},
          {"decision", "allow"},
          {"policy_generation", std::to_string(loaded.state.policy_generation)},
          {"cache_invalidation_epoch", std::to_string(loaded.state.cache_invalidation_epoch)}});
  return result;
}

EngineSecurityValidatePolicyCacheResult EngineSecurityValidatePolicyCache(
    const EngineSecurityValidatePolicyCacheRequest& request) {
  constexpr const char* kOperation = "security.policy_cache.validate";
  const auto boundary = ValidateEngineAuthorityBoundary(request, kOperation);
  if (boundary.error) {
    return DiagnosticResult<EngineSecurityValidatePolicyCacheResult>(request.context,
                                                                    kOperation,
                                                                    boundary);
  }
  if (!request.context.security_context_present) {
    return DiagnosticResult<EngineSecurityValidatePolicyCacheResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                            "security_context_required"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityValidatePolicyCacheResult>(request.context,
                                                                    kOperation,
                                                                    loaded.diagnostic);
  }
  const bool policy_missing = request.observed_policy_generation == 0 &&
                              loaded.state.policy_generation != 0;
  const bool invalidation_missing = request.observed_cache_invalidation_epoch == 0 &&
                                    loaded.state.cache_invalidation_epoch != 0;
  const bool stale = policy_missing || invalidation_missing ||
                     request.observed_policy_generation < loaded.state.policy_generation ||
                     request.observed_cache_invalidation_epoch < loaded.state.cache_invalidation_epoch;
  if (stale) {
    auto result = DiagnosticResult<EngineSecurityValidatePolicyCacheResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticCacheStale,
                            std::to_string(request.observed_policy_generation) + "<" +
                                std::to_string(loaded.state.policy_generation)));
    result.stale_policy_refused = true;
    result.current_policy_generation = loaded.state.policy_generation;
    result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
    return result;
  }
  auto result = SuccessResult<EngineSecurityValidatePolicyCacheResult>(request.context,
                                                                      kOperation);
  result.cache_valid = true;
  result.current_policy_generation = loaded.state.policy_generation;
  result.current_cache_invalidation_epoch = loaded.state.cache_invalidation_epoch;
  AddEvidence(&result, "security_policy_cache_current",
              std::to_string(loaded.state.policy_generation));
  return result;
}

EngineSecurityInspectAuditResult EngineSecurityInspectAudit(
    const EngineSecurityInspectAuditRequest& request) {
  constexpr const char* kOperation = "security.audit.inspect";
  const auto authority = ValidateSecurityAuthority(request, kOperation, "AUDIT_READ");
  if (authority.error) {
    return DiagnosticResult<EngineSecurityInspectAuditResult>(request.context,
                                                             kOperation,
                                                             authority);
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityInspectAuditResult>(request.context,
                                                             kOperation,
                                                             loaded.diagnostic);
  }
  auto result = SuccessResult<EngineSecurityInspectAuditResult>(request.context, kOperation);
  result.protected_material_redacted = true;
  result.audit_records = loaded.state.audit_records;
  AddEvidence(&result, "security_audit_record_count",
              std::to_string(result.audit_records.size()));
  if (request.include_rows) {
    for (const auto& audit : result.audit_records) {
      AddRow(&result,
             {{"audit_uuid", audit.audit_uuid},
              {"operation_id", audit.operation_id},
              {"actor_principal_uuid", audit.actor_principal_uuid},
              {"target_uuid", audit.target_uuid},
              {"outcome", audit.outcome},
              {"detail", RedactSecurityPrincipalProtectedMaterialForDiagnostics(audit.redacted_detail)},
              {"security_generation", std::to_string(audit.security_generation)}});
    }
  }
  return result;
}

EngineSecurityInspectOperationResult EngineSecurityInspectOperation(
    const EngineSecurityInspectOperationRequest& request) {
  const std::string operation_id = OperationIdOr(request, "security.inspect_operation");
  if (!request.context.security_context_present) {
    return DiagnosticResult<EngineSecurityInspectOperationResult>(
        request.context,
        operation_id,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityRequired,
                            operation_id + ":security_context_required"));
  }
  auto result = SuccessResult<EngineSecurityInspectOperationResult>(request.context, operation_id);
  AddSecurityInspectionOperationResult(
      &result,
      request,
      operation_id,
      ResultShapeContract(request, "rs.security.principal.v1"));
  return result;
}

}  // namespace scratchbird::engine::internal_api
