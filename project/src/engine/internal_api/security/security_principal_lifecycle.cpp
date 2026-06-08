// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/security_principal_lifecycle.hpp"

#include "api_diagnostics.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_model.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

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
        StartsWith(lower, "authority:donor") ||
        StartsWith(lower, std::string("authority:sql") + "ite")) {
      return PrincipalDiagnostic(kSecurityPrincipalDiagnosticAuthorityBypassRefused,
                                 operation_id + ":non_engine_trace_authority");
    }
  }
  const std::string forbidden_embedded = std::string("sql") + "ite";
  const std::string forbidden_log = std::string("authoritative_") + "wal";
  for (const auto& option : request.option_envelopes) {
    const std::string lower = LowerAscii(option);
    if (StartsWith(lower, "donor_shortcut:") ||
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

std::string NormalizePrivilege(std::string privilege) {
  return UpperAscii(std::move(privilege));
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
         (record.deleted ? "1" : "0");
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
  std::ofstream out(EventPath(context), std::ios::binary | std::ios::app);
  if (!out) { return PrincipalDiagnostic(kSecurityPrincipalDiagnosticDatabaseWriteFailed, "open"); }
  for (const auto& event : events) { out << event << '\n'; }
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

  std::ifstream in(EventPath(context), std::ios::binary);
  if (!in) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }

  std::map<std::string, EngineSecurityPrincipalRecord> principals;
  std::map<std::string, EngineSecurityRoleRecord> roles;
  std::map<std::string, EngineSecurityGroupRecord> groups;
  std::map<std::string, EngineSecurityMembershipRecord> memberships;
  std::map<std::string, EngineSecurityPrivilegeGrantRecord> grants;
  std::map<std::string, EngineSecurityRowPolicyRecord> row_policies;
  std::map<std::string, EngineSecurityDefinerRightsCacheRecord> cache;

  std::string line;
  std::uint64_t event_sequence = 0;
  while (std::getline(in, line)) {
    ++event_sequence;
    if (!StartsWith(line, kSecurityPrincipalLifecycleEventMagic)) { continue; }
    const auto parts = Split(line, '\t');
    if (parts.size() < 3) { continue; }
    const std::string& event = parts[1];
    const std::uint64_t creator_tx = ParseU64(parts[2]);
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

std::set<std::string> EffectiveGranteeSet(const EngineSecurityPrincipalLifecycleState& state,
                                          const std::string& principal_uuid) {
  std::set<std::string> grantees;
  grantees.insert(principal_uuid);
  for (const auto& membership : state.memberships) {
    if (membership.member_principal_uuid == principal_uuid && !membership.revoked) {
      grantees.insert(membership.container_uuid);
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
          {"credential_fingerprint", record.credential_fingerprint},
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
          {"credential_fingerprint", record.credential_fingerprint},
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
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineSecurityGrantMembershipResult>(request.context,
                                                                kOperation,
                                                                loaded.diagnostic);
  }
  if (FindPrincipal(loaded.state, request.member_principal_uuid) == nullptr) {
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
  if (request.grantee_uuid.empty() || request.target_object_uuid.empty() ||
      privilege.empty() || (effect != "allow" && effect != "deny")) {
    return DiagnosticResult<EngineSecurityGrantPrivilegeResult>(
        request.context,
        kOperation,
        PrincipalDiagnostic(kSecurityPrincipalDiagnosticGrantInvalid,
                            "grantee_target_privilege_required"));
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
  if (!HasTraceTag(request.context, "security.bootstrap") &&
      !HasTraceTag(request.context, "group:ROOT")) {
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

  auto result = SuccessResult<EngineSecurityCreatePolicyResult>(request.context, kOperation);
  result.policy_created = true;
  result.policy_generation = generation;
  result.cache_invalidation_epoch = generation;
  result.primary_object.uuid.canonical = policy_uuid;
  result.primary_object.object_kind = "security_policy";
  FillMutationEvidence(&result, kOperation, policy_uuid, generation);
  AddRow(&result,
         {{"policy_uuid", policy_uuid},
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
  }
  if (!request.definer_principal_uuid.empty()) {
    record.definer_principal_uuid = request.definer_principal_uuid;
  }
  record.lifecycle_state = lifecycle;
  record.policy_generation = generation;
  record.deleted = false;
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
  EngineSecurityRowPolicyRecord record;
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
  EngineSecurityRowPolicyRecord record;
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
