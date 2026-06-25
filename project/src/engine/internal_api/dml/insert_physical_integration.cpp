// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-INSERT-PHYSICAL-INTEGRATION-ANCHOR
#include "insert_physical_integration.hpp"

#include "api_diagnostics.hpp"
#include "bulk_constraint_proof.hpp"
#include "crud_support/crud_store.hpp"
#include "dml/constraint_enforcement.hpp"
#include "dml/dml_ingestion_pipeline.hpp"
#include "dml/index_apply_locality_bridge.hpp"
#include "dml/insert_batch.hpp"
#include "dml/page_allocation_runtime_bridge.hpp"
#include "dml/write_result_policy.hpp"
#include "bulk_placement_order.hpp"
#include "datatype_operations.hpp"
#include "domain_support/domain_store.hpp"
#include "ipar_fault_injection.hpp"
#include "metric_contracts.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "ordered_ingest.hpp"
#include "observability/dml_summary_counters.hpp"
#include "security/deep_enforcement_api.hpp"
#include "index_bulk_publish_recovery.hpp"
#include "index_key_encoding.hpp"
#include "index_root_generation_publish.hpp"
#include "physical_mga_cow_store.hpp"
#include "sorted_bulk_index_build.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace scratchbird::engine::internal_api::dml {
namespace {

using DirectSteadyClock = std::chrono::steady_clock;

namespace dt = scratchbird::core::datatypes;

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

bool DirectPackTypedPayload(
    dt::CanonicalTypeId target_type,
    const EngineTypedValue& typed,
    std::vector<scratchbird::core::platform::byte>* out);

Status IntegrationOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status IntegrationErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

InsertPhysicalIntegrationResult Refuse(std::string diagnostic_code,
                                       std::string message_key,
                                       std::string detail) {
  InsertPhysicalIntegrationResult result;
  result.status = IntegrationErrorStatus();
  result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                             std::move(diagnostic_code),
                                                             std::move(message_key),
                                                             std::move(detail));
  return result;
}

std::string EvidenceRef(const std::string& kind, const TypedUuid& uuid) {
  std::ostringstream out;
  out << kind << ":" << scratchbird::core::uuid::UuidToString(uuid.value);
  return out.str();
}

bool DirectIndexIsUnique(const CrudIndexRecord& index) {
  return index.unique ||
         std::find(index.key_envelopes.begin(),
                   index.key_envelopes.end(),
                   "unique") != index.key_envelopes.end();
}

bool DirectOptionEquals(const std::string& actual, const std::string& expected) {
  if (actual == expected) {
    return true;
  }
  const auto equals = expected.find('=');
  if (equals == std::string::npos) {
    return false;
  }
  return actual == expected.substr(0, equals) + ":" + expected.substr(equals + 1);
}

bool DirectOptionEnabled(const DirectPhysicalBulkAppendRequest& request,
                         const std::string& option) {
  for (const auto& candidate : request.option_envelopes) {
    if (DirectOptionEquals(candidate, option)) {
      return true;
    }
  }
  return false;
}

TypedUuid ParseDirectTypedUuid(UuidKind kind, const std::string& text);

std::string DirectOptionValue(const DirectPhysicalBulkAppendRequest& request,
                              const std::string& key) {
  const std::string equals_prefix = key + "=";
  const std::string colon_prefix = key + ":";
  for (const auto& candidate : request.option_envelopes) {
    if (candidate.rfind(equals_prefix, 0) == 0) {
      return candidate.substr(equals_prefix.size());
    }
    if (candidate.rfind(colon_prefix, 0) == 0) {
      return candidate.substr(colon_prefix.size());
    }
  }
  return {};
}

std::vector<std::string> DirectSplitText(std::string_view text, char delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t pos = text.find(delimiter, start);
    if (pos == std::string_view::npos) {
      parts.emplace_back(text.substr(start));
      break;
    }
    parts.emplace_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool IsDirectTruthyValue(const std::string& value) {
  const std::string lowered = LowerAscii(value);
  return lowered == "1" || lowered == "true" || lowered == "enabled" ||
         lowered == "on" || lowered == "required";
}

bool IsDirectFalsyValue(const std::string& value) {
  const std::string lowered = LowerAscii(value);
  return lowered == "0" || lowered == "false" || lowered == "disabled" ||
         lowered == "off";
}

bool StartsWithDirect(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> SplitDirectText(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : value) {
    if (ch == delimiter) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  return parts;
}

bool DirectRuntimeInsertPolicyApplies(
    const EngineMaterializedAuthorizationPolicy& policy,
    const std::string& table_uuid) {
  if (!policy.requires_runtime_recheck) {
    return false;
  }
  if (!policy.right.empty() && policy.right != "INSERT") {
    return false;
  }
  return policy.target_uuid.canonical.empty() ||
         policy.target_uuid.canonical == table_uuid;
}

struct DirectRuntimeSecurityPolicyDecision {
  bool ok = true;
  bool denied = false;
  bool filtered = false;
  std::string reason = "allow";
};

DirectRuntimeSecurityPolicyDecision EvaluateDirectRuntimeSecurityPolicyEnvelope(
    const std::string& envelope,
    const std::vector<std::pair<std::string, std::string>>& values) {
  DirectRuntimeSecurityPolicyDecision decision;
  std::string normalized = LowerAscii(envelope);
  if (StartsWithDirect(normalized, "sblr_predicate:")) {
    normalized = normalized.substr(15);
  }
  if (normalized.empty() || normalized == "allow" || normalized == "true") {
    return decision;
  }
  if (normalized == "filter" || normalized == "tenant_visible") {
    decision.filtered = true;
    decision.reason = normalized;
    return decision;
  }
  if (normalized == "deny" || normalized == "false") {
    decision.denied = true;
    decision.reason = normalized;
    return decision;
  }

  const auto parts = SplitDirectText(normalized, ':');
  if (parts.size() == 3 && parts[0] == "column_equals") {
    decision.filtered = true;
    decision.denied = CrudFieldValue(values, parts[1]) != parts[2];
    decision.reason = "column_equals:" + parts[1];
    return decision;
  }
  if (parts.size() == 3 && parts[0] == "column_not_equals") {
    decision.filtered = true;
    decision.denied = CrudFieldValue(values, parts[1]) == parts[2];
    decision.reason = "column_not_equals:" + parts[1];
    return decision;
  }

  decision.ok = false;
  decision.denied = true;
  decision.reason = "unsupported_runtime_policy_envelope";
  return decision;
}

EngineEvaluateDeepSecurityResult EvaluateDirectRuntimeInsertSecurityRecheck(
    const DirectPhysicalBulkAppendRequest& request,
    const std::string& table_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    std::vector<EngineEvidenceReference>* evidence) {
  std::string rls_policy = "allow";
  for (const auto& policy : request.context.authorization_context.policies) {
    if (!DirectRuntimeInsertPolicyApplies(policy, table_uuid)) {
      continue;
    }
    const auto decision = EvaluateDirectRuntimeSecurityPolicyEnvelope(
        policy.canonical_policy_envelope,
        values);
    if (evidence != nullptr) {
      evidence->push_back({"direct_physical_runtime_security_policy_evaluated",
                           policy.policy_uuid.canonical});
      evidence->push_back({"direct_physical_runtime_security_policy_result",
                           decision.reason + ":" +
                               (decision.denied ? "deny" : "allow")});
    }
    if (!decision.ok) {
      EngineEvaluateDeepSecurityResult refused =
          MakeCrudDiagnosticResult<EngineEvaluateDeepSecurityResult>(
              request.context,
              "security.evaluate_deep_enforcement",
              MakeInvalidRequestDiagnostic("security.evaluate_deep_enforcement",
                                           decision.reason));
      refused.decision = "refused";
      refused.evidence = evidence == nullptr ? std::vector<EngineEvidenceReference>{}
                                             : *evidence;
      return refused;
    }
    if (decision.denied) {
      rls_policy = "deny";
      break;
    }
    if (decision.filtered) {
      rls_policy = "filter";
    }
  }

  EngineEvaluateDeepSecurityRequest security;
  security.context = request.context;
  security.target_object = request.target_table;
  security.phase = "executor";
  security.required_right = "INSERT";
  security.mutation = false;
  security.option_envelopes.push_back("phase:executor");
  security.option_envelopes.push_back("required_right:INSERT");
  security.option_envelopes.push_back("mutation:false");
  security.option_envelopes.push_back("rls_policy:" + rls_policy);
  auto result = EngineEvaluateDeepSecurity(security);
  if (evidence != nullptr) {
    evidence->insert(evidence->end(),
                     result.evidence.begin(),
                     result.evidence.end());
    evidence->push_back({"direct_physical_runtime_security_recheck",
                         result.decision + ":rls=" + rls_policy});
  }
  return result;
}

bool DirectPageExtentPreallocationRequired(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string mode = DirectOptionValue(request, "page_extent_preallocation");
  if (!mode.empty()) {
    return IsDirectTruthyValue(mode);
  }
  const std::string odf_mode =
      DirectOptionValue(request, "odf042.page_extent_preallocation");
  if (!odf_mode.empty()) {
    return IsDirectTruthyValue(odf_mode);
  }
  return false;
}

bool DirectPageAllocationRuntimeRequested(
    const DirectPhysicalBulkAppendRequest& request) {
  return DirectPageExtentPreallocationRequired(request) ||
         IsDirectTruthyValue(DirectOptionValue(request, "page_allocation.runtime"));
}

std::string DirectPageExtentPreallocationPrecheckFailure(
    const DirectPhysicalBulkAppendRequest& request) {
  if (!DirectPageExtentPreallocationRequired(request)) {
    return {};
  }
  if (!DirectOptionEnabled(request, "page_allocation.runtime=enabled")) {
    return "page_extent_preallocation_disabled";
  }
  const TypedUuid database_uuid =
      ParseDirectTypedUuid(UuidKind::database, request.context.database_uuid.canonical);
  const TypedUuid transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction,
                           request.context.transaction_uuid.canonical);
  const TypedUuid object_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  if (!database_uuid.valid() || !transaction_uuid.valid() ||
      !object_uuid.valid() || request.context.local_transaction_id == 0) {
    return "page_extent_preallocation_authority_missing";
  }
  const std::string filespace = DirectOptionValue(request, "page_allocation.filespace_uuid");
  if (!filespace.empty() &&
      !ParseDirectTypedUuid(UuidKind::filespace, filespace).valid()) {
    return "page_extent_preallocation_invalid_filespace";
  }
  const std::string disabled = DirectOptionValue(request, "page_extent_preallocation");
  if (!disabled.empty() && IsDirectFalsyValue(disabled)) {
    return "page_extent_preallocation_disabled";
  }
  return {};
}

TypedUuid ParseDirectTypedUuid(UuidKind kind, const std::string& text) {
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(kind, text);
  return parsed.ok() ? parsed.value : TypedUuid{};
}

std::string TypedUuidText(const TypedUuid& uuid) {
  return uuid.valid() ? scratchbird::core::uuid::UuidToString(uuid.value) : std::string{};
}

std::string DiagnosticDetail(const DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      return argument.value;
    }
  }
  return {};
}

EngineApiDiagnostic CoreBulkDiagnosticToEngine(const DiagnosticRecord& diagnostic,
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

void AddStrictBulkLifecycleEvidence(
    const scratchbird::core::bulk_load::StrictBulkLoadLedger& ledger,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr) {
    return;
  }
  for (const auto& record : ledger.evidence) {
    const std::string previous =
        scratchbird::core::bulk_load::StrictBulkLoadStateName(record.previous_state);
    const std::string next =
        scratchbird::core::bulk_load::StrictBulkLoadStateName(record.new_state);
    evidence->push_back({"strict_bulk_load_lifecycle_action", record.action});
    evidence->push_back({"strict_bulk_load_state_transition", previous + "->" + next});
    evidence->push_back({"strict_bulk_load_state", next});
    if (record.bulk_load_id.valid()) {
      evidence->push_back({"strict_bulk_load_id", TypedUuidText(record.bulk_load_id)});
    }
    if (!record.visibility_fence.empty()) {
      evidence->push_back({"strict_bulk_load_visibility_fence", record.visibility_fence});
    }
    if (!record.diagnostic_code.empty()) {
      evidence->push_back({"strict_bulk_load_diagnostic_code", record.diagnostic_code});
    }
    if (record.new_state ==
        scratchbird::core::bulk_load::StrictBulkLoadState::finalize_evidence_durable) {
      evidence->push_back({"strict_bulk_load_finalize_evidence_durable", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::published_visible) {
      evidence->push_back({"strict_bulk_load_published_visible", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::recovery_required) {
      evidence->push_back({"strict_bulk_load_recovery_required", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::rolled_back) {
      evidence->push_back({"strict_bulk_load_rollback", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::refused) {
      evidence->push_back({"strict_bulk_load_refused_state", "true"});
    } else if (record.new_state ==
               scratchbird::core::bulk_load::StrictBulkLoadState::quarantine) {
      evidence->push_back({"strict_bulk_load_quarantine", "true"});
    }
  }
}

void AddStrictBulkRecoveryEvidence(
    const scratchbird::core::bulk_load::StrictBulkLoadRecoveryResult& recovery,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr) {
    return;
  }
  for (const auto& classification : recovery.classifications) {
    evidence->push_back({"strict_bulk_load_recovery_observed_state",
                         scratchbird::core::bulk_load::StrictBulkLoadStateName(
                             classification.observed_state)});
    evidence->push_back({"strict_bulk_load_recovery_action",
                         scratchbird::core::bulk_load::StrictBulkLoadRecoveryActionName(
                             classification.action)});
    evidence->push_back({"strict_bulk_load_recovery_fail_closed",
                         classification.fail_closed ? "true" : "false"});
    evidence->push_back({"strict_bulk_load_recovery_reason",
                         classification.stable_reason});
  }
}

std::vector<std::string> DirectSplit(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (char ch : value) {
    if (ch == delimiter) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  parts.push_back(current);
  return parts;
}

std::string DirectTrimAscii(std::string value) {
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' ||
          value.front() == '\r' || value.front() == '\n')) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  return value;
}

std::map<std::string, std::string> DirectDescriptorFields(
    const std::string& descriptor) {
  std::map<std::string, std::string> fields;
  for (const auto& raw_part : DirectSplit(descriptor, ';')) {
    const std::string part = DirectTrimAscii(raw_part);
    if (part.empty()) {
      continue;
    }
    const auto equal = part.find('=');
    if (equal == std::string::npos) {
      fields[LowerAscii(part)] = "true";
    } else {
      fields[LowerAscii(DirectTrimAscii(part.substr(0, equal)))] =
          DirectTrimAscii(part.substr(equal + 1));
    }
  }
  return fields;
}

std::string DirectFieldOrEmpty(
    const std::map<std::string, std::string>& fields,
    std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto found = fields.find(key);
    if (found != fields.end()) {
      return found->second;
    }
  }
  return {};
}

bool DirectBoolField(const std::map<std::string, std::string>& fields,
                     std::initializer_list<const char*> keys) {
  const std::string value = LowerAscii(DirectFieldOrEmpty(fields, keys));
  return value == "true" || value == "1" || value == "yes" ||
         value == "required" || value == "primary" || value == "unique";
}

bool DirectNullValue(const std::string& value) {
  return value == "<NULL>";
}

inline constexpr char kDirectNullMarker[] = "<NULL>";

std::string DirectConstraintUuid(
    const std::map<std::string, std::string>& fields,
    const CrudTableRecord& table,
    const std::string& column_name,
    const std::string& constraint_class) {
  const std::string explicit_uuid =
      DirectFieldOrEmpty(fields, {"constraint_uuid", "uuid"});
  if (!explicit_uuid.empty()) {
    return explicit_uuid;
  }
  return "descriptor:" + table.table_uuid + ":" + column_name + ":" +
         constraint_class;
}

struct DirectForeignKeyReference {
  std::string parent_table_uuid;
  std::string parent_column;
};

std::optional<DirectForeignKeyReference> DirectParseForeignKeyReference(
    const std::map<std::string, std::string>& fields) {
  DirectForeignKeyReference reference;
  reference.parent_table_uuid =
      DirectFieldOrEmpty(fields,
                         {"referenced_table_uuid",
                          "foreign_table_uuid",
                          "foreign_table"});
  reference.parent_column =
      DirectFieldOrEmpty(fields,
                         {"referenced_column",
                          "foreign_column",
                          "parent_column"});
  if (!reference.parent_table_uuid.empty() && !reference.parent_column.empty()) {
    return reference;
  }
  const std::string envelope =
      DirectFieldOrEmpty(fields, {"foreign_key", "references", "fk"});
  if (envelope.empty()) {
    return std::nullopt;
  }
  const auto colon = envelope.find(':');
  const auto dot = envelope.rfind('.');
  const auto open = envelope.find('(');
  const auto close = envelope.rfind(')');
  if (colon != std::string::npos) {
    reference.parent_table_uuid = envelope.substr(0, colon);
    reference.parent_column = envelope.substr(colon + 1);
  } else if (dot != std::string::npos) {
    reference.parent_table_uuid = envelope.substr(0, dot);
    reference.parent_column = envelope.substr(dot + 1);
  } else if (open != std::string::npos && close == envelope.size() - 1 &&
             close > open + 1) {
    reference.parent_table_uuid = envelope.substr(0, open);
    reference.parent_column = envelope.substr(open + 1, close - open - 1);
  }
  if (reference.parent_table_uuid.empty() || reference.parent_column.empty()) {
    return std::nullopt;
  }
  return reference;
}

bool DirectDescriptorDeclaresForeignKey(
    const std::map<std::string, std::string>& fields) {
  return !DirectFieldOrEmpty(
              fields,
              {"foreign_key",
               "references",
               "fk",
               "referenced_table_uuid",
               "foreign_table_uuid",
               "foreign_table",
               "referenced_column",
               "foreign_column",
               "parent_column"})
              .empty();
}

bool DirectTableDeclaresForeignKey(const CrudTableRecord& table) {
  for (const auto& [column_name, descriptor] : table.columns) {
    (void)column_name;
    if (DirectDescriptorDeclaresForeignKey(DirectDescriptorFields(descriptor))) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> DirectIndexKeyColumns(const CrudIndexRecord& index) {
  std::vector<std::string> columns;
  for (const auto& envelope : index.key_envelopes) {
    const std::string lowered = LowerAscii(envelope);
    if (lowered == "unique" || lowered == "primary_key" ||
        lowered == "pk" || lowered == "not_null") {
      continue;
    }
    if (envelope.rfind("identity:", 0) == 0) {
      columns.push_back(envelope.substr(9));
    } else {
      columns.push_back(envelope);
    }
  }
  if (columns.empty() && !index.column_name.empty()) {
    columns.push_back(index.column_name);
  }
  return columns;
}

bool DirectIndexCoversColumn(const CrudIndexRecord& index,
                             const std::string& column_name) {
  const auto columns = DirectIndexKeyColumns(index);
  return columns.size() == 1 && columns.front() == column_name;
}

const std::string* DirectFieldValuePtr(
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& field) {
  for (const auto& [key, value] : values) {
    if (key == field) { return &value; }
  }
  return nullptr;
}

bool DirectSimpleScalarIndexKeyColumn(const CrudIndexRecord& index,
                                      std::string* column_name) {
  if (column_name == nullptr || index.column_name.empty()) {
    return false;
  }
  if (!index.predicate_kind.empty() && index.predicate_kind != "where_true") {
    return false;
  }
  const std::string family =
      index.family.empty() ? CrudIndexFamilyForProfile(index.profile)
                           : index.family;
  if (family != kCrudIndexFamilyBtree &&
      family != kCrudIndexFamilyHash &&
      family != "unique_btree" &&
      !family.empty()) {
    return false;
  }
  const auto columns = DirectIndexKeyColumns(index);
  if (columns.size() != 1 || columns.front() != index.column_name) {
    return false;
  }
  *column_name = columns.front();
  return true;
}

std::optional<CrudIndexRecord> DirectVisibleUniqueIndexForColumn(
    const CrudState& state,
    const std::string& table_uuid,
    const std::string& column_name,
    std::uint64_t observer_tx) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (DirectIndexIsUnique(index) && DirectIndexCoversColumn(index, column_name)) {
      return index;
    }
  }
  return std::nullopt;
}

void AddCoreProofEvidence(
    const std::vector<scratchbird::core::bulk_load::BulkConstraintProofEvidence>& source,
    std::vector<EngineEvidenceReference>* target) {
  if (target == nullptr) {
    return;
  }
  for (const auto& evidence : source) {
    target->push_back({evidence.evidence_kind, evidence.evidence_id});
  }
}

struct DirectBulkConstraintProofSelection {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<EngineEvidenceReference> evidence;
};

struct DirectPrecomputedIndexEntry {
  std::string encoded_key;
  std::string payload_value;
  std::string row_uuid;
  std::string version_uuid;
  std::uint64_t source_ordinal = 0;
};

using DirectPrecomputedIndexEntryMap =
    std::map<std::string, std::vector<DirectPrecomputedIndexEntry>>;

struct DirectTypedIndexKeyStats {
  std::uint64_t typed_key_candidates = 0;
  std::uint64_t typed_key_encoded = 0;
  std::uint64_t typed_key_fallback = 0;
  std::uint64_t sbkohex_keys = 0;
};

inline constexpr std::string_view kDirectSbkoHexPrefix = "SBKOHEX:";

int DirectCompareUnsignedText(std::string_view left, std::string_view right) {
  const auto count = std::min(left.size(), right.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto l = static_cast<unsigned char>(left[i]);
    const auto r = static_cast<unsigned char>(right[i]);
    if (l < r) {
      return -1;
    }
    if (l > r) {
      return 1;
    }
  }
  return left.size() < right.size() ? -1 : (right.size() < left.size() ? 1 : 0);
}

std::string DirectHexEncodeBytes(
    std::span<const scratchbird::core::platform::byte> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto value : bytes) {
    out.push_back(kHex[(value >> 4) & 0x0f]);
    out.push_back(kHex[value & 0x0f]);
  }
  return out;
}

bool DirectSbkoHexPayload(std::string_view key, std::string* payload) {
  if (payload == nullptr || key.rfind(kDirectSbkoHexPrefix, 0) != 0) {
    return false;
  }
  *payload = std::string(key.substr(kDirectSbkoHexPrefix.size()));
  return true;
}

int DirectCompareEncodedIndexKey(std::string_view left,
                                 std::string_view right) {
  if (scratchbird::core::index::IsOrderPreservingIndexKeyEncoding(left) &&
      scratchbird::core::index::IsOrderPreservingIndexKeyEncoding(right)) {
    const auto compare =
        scratchbird::core::index::CompareEncodedIndexKeyBytes(left, right);
    if (compare.ok()) {
      return compare.comparison;
    }
  }
  std::string left_sbkohex;
  std::string right_sbkohex;
  if (DirectSbkoHexPayload(left, &left_sbkohex) &&
      DirectSbkoHexPayload(right, &right_sbkohex)) {
    return DirectCompareUnsignedText(left_sbkohex, right_sbkohex);
  }
  return DirectCompareUnsignedText(left, right);
}

bool DirectPrecomputedIndexEntryLess(
    const DirectPrecomputedIndexEntry& left,
    const DirectPrecomputedIndexEntry& right) {
  const int key_compare =
      DirectCompareEncodedIndexKey(left.encoded_key, right.encoded_key);
  if (key_compare != 0) {
    return key_compare < 0;
  }
  const int row_compare =
      DirectCompareUnsignedText(left.row_uuid, right.row_uuid);
  if (row_compare != 0) {
    return row_compare < 0;
  }
  const int version_compare =
      DirectCompareUnsignedText(left.version_uuid, right.version_uuid);
  if (version_compare != 0) {
    return version_compare < 0;
  }
  return left.source_ordinal < right.source_ordinal;
}

bool DirectPrecomputedIndexEntryTextLess(
    const DirectPrecomputedIndexEntry& left,
    const DirectPrecomputedIndexEntry& right) {
  if (left.encoded_key != right.encoded_key) {
    return left.encoded_key < right.encoded_key;
  }
  if (left.row_uuid != right.row_uuid) {
    return left.row_uuid < right.row_uuid;
  }
  if (left.version_uuid != right.version_uuid) {
    return left.version_uuid < right.version_uuid;
  }
  return left.source_ordinal < right.source_ordinal;
}

bool DirectPrecomputedEntriesRequireEncodedCompare(
    const std::vector<DirectPrecomputedIndexEntry>& entries) {
  return std::all_of(entries.begin(),
                     entries.end(),
                     [](const auto& entry) {
                       return scratchbird::core::index::
                                  IsOrderPreservingIndexKeyEncoding(
                                      entry.encoded_key) ||
                              entry.encoded_key.rfind(
                                  kDirectSbkoHexPrefix, 0) == 0;
                     });
}

void DirectSortPrecomputedIndexEntries(DirectPrecomputedIndexEntryMap* map) {
  if (map == nullptr) {
    return;
  }
  for (auto& [unused_index_uuid, entries] : *map) {
    (void)unused_index_uuid;
    if (entries.size() <= 1) {
      continue;
    }
    const bool use_encoded_compare =
        DirectPrecomputedEntriesRequireEncodedCompare(entries);
    const auto entry_less = [&](std::size_t left, std::size_t right) {
      return use_encoded_compare
                 ? DirectPrecomputedIndexEntryLess(entries[left],
                                                   entries[right])
                 : DirectPrecomputedIndexEntryTextLess(entries[left],
                                                       entries[right]);
    };
    const auto direct_less = [&](const auto& left, const auto& right) {
      return use_encoded_compare ? DirectPrecomputedIndexEntryLess(left, right)
                                 : DirectPrecomputedIndexEntryTextLess(left, right);
    };
    if (std::is_sorted(entries.begin(), entries.end(), direct_less)) {
      continue;
    }
    std::vector<std::size_t> order(entries.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), entry_less);
    std::vector<DirectPrecomputedIndexEntry> sorted;
    sorted.reserve(entries.size());
    for (const auto ordinal : order) {
      sorted.push_back(std::move(entries[ordinal]));
    }
    entries = std::move(sorted);
  }
}

const std::vector<DirectPrecomputedIndexEntry>* DirectPrecomputedEntriesForIndex(
    const DirectPrecomputedIndexEntryMap* precomputed_entries,
    const std::string& index_uuid) {
  if (precomputed_entries == nullptr) {
    return nullptr;
  }
  const auto found = precomputed_entries->find(index_uuid);
  return found == precomputed_entries->end() ? nullptr : &found->second;
}

bool DirectPrecomputedEntriesHaveDuplicateKeys(
    const std::vector<DirectPrecomputedIndexEntry>& entries) {
  if (entries.size() < 2) {
    return false;
  }
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (DirectCompareEncodedIndexKey(entries[index - 1].encoded_key,
                                     entries[index].encoded_key) == 0) {
      return true;
    }
  }
  return false;
}

bool DirectGeneratedEmptyTargetConstraintProofEligible(
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& visible_indexes,
    const DirectPrecomputedIndexEntryMap& precomputed_entries) {
  if (DirectTableDeclaresForeignKey(table)) {
    return false;
  }
  for (const auto& index : visible_indexes) {
    if (!DirectIndexIsUnique(index)) {
      continue;
    }
    const auto* entries =
        DirectPrecomputedEntriesForIndex(&precomputed_entries, index.index_uuid);
    if (entries == nullptr ||
        DirectPrecomputedEntriesHaveDuplicateKeys(*entries)) {
      return false;
    }
  }
  return true;
}

scratchbird::core::bulk_load::BulkConstraintProofKeyRef DirectProofKey(
    std::string key,
    std::string row_uuid,
    std::string version_uuid,
    std::uint64_t source_ordinal) {
  scratchbird::core::bulk_load::BulkConstraintProofKeyRef ref;
  ref.encoded_key = std::move(key);
  ref.row_uuid = std::move(row_uuid);
  ref.version_uuid = std::move(version_uuid);
  ref.source_ordinal = source_ordinal;
  ref.null_key = ref.encoded_key.find("<NULL>") != std::string::npos ||
                 DirectNullValue(ref.encoded_key);
  return ref;
}

void AddVisibleRowKeysForProof(
    const CrudState& state,
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    std::vector<scratchbird::core::bulk_load::BulkConstraintProofKeyRef>* keys,
    bool index_entries_authoritative) {
  if (keys == nullptr) {
    return;
  }
  std::uint64_t ordinal = 0;
  if (index_entries_authoritative) {
    for (const auto& entry : state.index_entries) {
      if (entry.index_uuid != index.index_uuid ||
          entry.table_uuid != index.table_uuid ||
          !CrudCreatorVisible(state,
                              entry.creator_tx,
                              entry.event_sequence,
                              context.local_transaction_id)) {
        continue;
      }
      keys->push_back(DirectProofKey(entry.key_value,
                                     entry.row_uuid,
                                     entry.version_uuid,
                                     ordinal++));
    }
    return;
  }
  std::set<std::string> visible_row_keys;
  for (const auto& row :
       VisibleCrudRowsForContext(state, index.table_uuid, context)) {
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      keys->push_back(DirectProofKey(key,
                                     row.row_uuid,
                                     row.version_uuid,
                                     ordinal++));
      visible_row_keys.insert(row.row_uuid + "\n" + key);
    }
  }
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != index.index_uuid ||
        entry.table_uuid != index.table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id) ||
        visible_row_keys.count(entry.row_uuid + "\n" + entry.key_value) == 0) {
      continue;
    }
    keys->push_back(DirectProofKey(entry.key_value,
                                   entry.row_uuid,
                                   entry.version_uuid,
                                   ordinal++));
  }
}

void AddVisibleRowKeysForSortedBuild(
    const CrudState& state,
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    std::vector<scratchbird::core::index::SortedBulkIndexRowInput>* keys,
    bool index_entries_authoritative) {
  if (keys == nullptr) {
    return;
  }
  std::uint64_t ordinal = 0;
  if (index_entries_authoritative) {
    for (const auto& entry : state.index_entries) {
      if (entry.index_uuid != index.index_uuid ||
          entry.table_uuid != index.table_uuid ||
          !CrudCreatorVisible(state,
                              entry.creator_tx,
                              entry.event_sequence,
                              context.local_transaction_id)) {
        continue;
      }
      scratchbird::core::index::SortedBulkIndexRowInput input;
      input.encoded_key = entry.key_value;
      input.row_uuid = entry.row_uuid;
      input.version_uuid = entry.version_uuid;
      input.payload_value = entry.payload_value;
      input.source_ordinal = ordinal++;
      input.null_key = DirectNullValue(input.encoded_key) ||
                       input.encoded_key.find("<NULL>") != std::string::npos;
      keys->push_back(std::move(input));
    }
    return;
  }
  std::set<std::string> visible_row_keys;
  for (const auto& row :
       VisibleCrudRowsForContext(state, index.table_uuid, context)) {
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      scratchbird::core::index::SortedBulkIndexRowInput input;
      input.encoded_key = key;
      input.row_uuid = row.row_uuid;
      input.version_uuid = row.version_uuid;
      input.payload_value = CrudFieldValue(row.values, index.column_name);
      input.source_ordinal = ordinal++;
      input.null_key = DirectNullValue(input.encoded_key) ||
                       input.encoded_key.find("<NULL>") !=
                           std::string::npos;
      keys->push_back(std::move(input));
      visible_row_keys.insert(row.row_uuid + "\n" + key);
    }
  }
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != index.index_uuid ||
        entry.table_uuid != index.table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id) ||
        visible_row_keys.count(entry.row_uuid + "\n" + entry.key_value) ==
            0) {
      continue;
    }
    scratchbird::core::index::SortedBulkIndexRowInput input;
    input.encoded_key = entry.key_value;
    input.row_uuid = entry.row_uuid;
    input.version_uuid = entry.version_uuid;
    input.payload_value = entry.payload_value;
    input.source_ordinal = ordinal++;
    input.null_key = DirectNullValue(input.encoded_key) ||
                     input.encoded_key.find("<NULL>") != std::string::npos;
    keys->push_back(std::move(input));
  }
}

void AddCachedConflictingVisibleKeysForSortedBuild(
    const CrudIndexRecord& index,
    const std::vector<scratchbird::core::index::SortedBulkIndexRowInput>& incoming_rows,
    const std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>* entry_cache,
    std::vector<scratchbird::core::index::SortedBulkIndexRowInput>* keys) {
  if (entry_cache == nullptr || keys == nullptr) {
    return;
  }
  const auto by_index = entry_cache->find(index.index_uuid);
  if (by_index == entry_cache->end()) {
    return;
  }
  std::set<std::string> emitted;
  std::uint64_t ordinal = 0;
  for (const auto& incoming : incoming_rows) {
    const auto by_key = by_index->second.find(incoming.encoded_key);
    if (by_key == by_index->second.end() ||
        !emitted.insert(incoming.encoded_key).second) {
      continue;
    }
    scratchbird::core::index::SortedBulkIndexRowInput input;
    input.encoded_key = by_key->second.key_value;
    input.row_uuid = by_key->second.row_uuid;
    input.version_uuid = by_key->second.version_uuid;
    input.payload_value = by_key->second.payload_value;
    input.source_ordinal = ordinal++;
    input.null_key = DirectNullValue(input.encoded_key) ||
                     input.encoded_key.find("<NULL>") != std::string::npos;
    keys->push_back(std::move(input));
  }
}

void AddVisibleParentKeysForProof(
    const CrudState& state,
    const EngineRequestContext& context,
    const std::string& parent_table_uuid,
    const std::string& parent_column,
    const CrudIndexRecord& parent_index,
    std::vector<scratchbird::core::bulk_load::BulkConstraintProofKeyRef>* keys,
    bool index_entries_authoritative) {
  if (keys == nullptr) {
    return;
  }
  std::uint64_t ordinal = 0;
  if (index_entries_authoritative) {
    for (const auto& entry : state.index_entries) {
      if (entry.index_uuid != parent_index.index_uuid ||
          entry.table_uuid != parent_table_uuid ||
          !CrudCreatorVisible(state,
                              entry.creator_tx,
                              entry.event_sequence,
                              context.local_transaction_id)) {
        continue;
      }
      keys->push_back(DirectProofKey(entry.key_value,
                                     entry.row_uuid,
                                     entry.version_uuid,
                                     ordinal++));
    }
    return;
  }
  std::set<std::string> visible_parent_keys;
  for (const auto& row :
       VisibleCrudRowsForContext(state, parent_table_uuid, context)) {
    const std::string key = CrudFieldValue(row.values, parent_column);
    keys->push_back(DirectProofKey(key,
                                   row.row_uuid,
                                   row.version_uuid,
                                   ordinal++));
    visible_parent_keys.insert(row.row_uuid + "\n" + key);
  }
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != parent_index.index_uuid ||
        entry.table_uuid != parent_table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id) ||
        visible_parent_keys.count(entry.row_uuid + "\n" + entry.key_value) ==
            0) {
      continue;
    }
    keys->push_back(DirectProofKey(entry.key_value,
                                   entry.row_uuid,
                                   entry.version_uuid,
                                   ordinal++));
  }
}

void AddCachedConflictingVisibleKeysForProof(
    const CrudIndexRecord& index,
    const std::vector<scratchbird::core::bulk_load::BulkConstraintProofKeyRef>& incoming_keys,
    const std::map<std::string, std::set<std::string>>* key_cache,
    std::vector<scratchbird::core::bulk_load::BulkConstraintProofKeyRef>* keys) {
  if (key_cache == nullptr || keys == nullptr) {
    return;
  }
  const auto found = key_cache->find(index.index_uuid);
  if (found == key_cache->end()) {
    return;
  }
  std::set<std::string> emitted;
  std::uint64_t ordinal = 0;
  for (const auto& incoming : incoming_keys) {
    if (found->second.count(incoming.encoded_key) == 0 ||
        !emitted.insert(incoming.encoded_key).second) {
      continue;
    }
    keys->push_back(DirectProofKey(incoming.encoded_key,
                                   incoming.row_uuid,
                                   incoming.version_uuid,
                                   ordinal++));
  }
}

DirectBulkConstraintProofSelection BuildDirectBulkConstraintProof(
    const DirectPhysicalBulkAppendRequest& request,
    const CrudState& state,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& visible_indexes,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch,
    bool index_entries_authoritative,
    const std::map<std::string, std::set<std::string>>* append_index_key_cache,
    const DirectPrecomputedIndexEntryMap* precomputed_entries) {
  DirectBulkConstraintProofSelection selection;
  scratchbird::core::bulk_load::BulkConstraintProofRequest proof_request;
  proof_request.database_uuid =
      ParseDirectTypedUuid(UuidKind::database, request.context.database_uuid.canonical);
  proof_request.object_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  proof_request.transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction,
                           request.context.transaction_uuid.canonical);
  proof_request.local_transaction_id = request.context.local_transaction_id;
  proof_request.route = "direct_physical_bulk";
  proof_request.direct_physical_bulk = true;
  proof_request.strict_bulk_load = request.strict_bulk_load_requested;
  std::set<std::string> proofed_unique_indexes;

  auto fail_before_proof = [&](std::string reason) {
    selection.ok = false;
    selection.failure_reason = std::move(reason);
    selection.diagnostic =
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     selection.failure_reason);
    return selection;
  };

  for (const auto& [column_name, descriptor] : table.columns) {
    const auto fields = DirectDescriptorFields(descriptor);
    const bool primary_key = DirectBoolField(fields, {"primary_key", "pk"});
    const bool unique_key =
        primary_key || DirectBoolField(fields, {"unique", "unique_key"});
    if (unique_key) {
      std::optional<CrudIndexRecord> support_index;
      for (const auto& index : visible_indexes) {
        if (DirectIndexIsUnique(index) &&
            DirectIndexCoversColumn(index, column_name)) {
          support_index = index;
          break;
        }
      }
      if (!support_index) {
        return fail_before_proof("bulk_unique_proof_support_index_missing");
      }

      scratchbird::core::bulk_load::BulkUniqueProofRequest unique;
      unique.constraint_uuid =
          DirectConstraintUuid(fields,
                               table,
                               column_name,
                               primary_key ? "primary_key" : "unique_key");
      unique.index_uuid = support_index->index_uuid;
      unique.table_uuid = table.table_uuid;
      unique.column_name = column_name;
      const std::string null_policy =
          LowerAscii(DirectFieldOrEmpty(fields,
                                        {"null_policy", "unique_null_policy"}));
      unique.nulls_distinct =
          !primary_key &&
          (null_policy.empty() || null_policy == "nulls_distinct");
      if (const auto* entries =
              DirectPrecomputedEntriesForIndex(precomputed_entries,
                                               support_index->index_uuid)) {
        unique.incoming_keys_presorted = true;
        for (const auto& entry : *entries) {
          unique.incoming_keys.push_back(
              DirectProofKey(entry.encoded_key,
                             entry.row_uuid,
                             entry.version_uuid,
                             entry.source_ordinal));
        }
      } else {
        for (std::size_t row_index = 0; row_index < staged_rows.size();
             ++row_index) {
          const auto& values = logical_value_batch[row_index];
          for (const auto& key :
               CrudIndexKeysForValues(*support_index, values)) {
            unique.incoming_keys.push_back(
                DirectProofKey(key,
                               staged_rows[row_index].row_uuid,
                               staged_rows[row_index].version_uuid,
                               row_index));
          }
        }
      }
      if (index_entries_authoritative && append_index_key_cache != nullptr) {
        AddCachedConflictingVisibleKeysForProof(*support_index,
                                                unique.incoming_keys,
                                                append_index_key_cache,
                                                &unique.visible_keys);
      } else {
        AddVisibleRowKeysForProof(state,
                                  request.context,
                                  *support_index,
                                  &unique.visible_keys,
                                  index_entries_authoritative);
      }
      proofed_unique_indexes.insert(support_index->index_uuid);
      proof_request.unique_proofs.push_back(std::move(unique));
    }

    if (!DirectDescriptorDeclaresForeignKey(fields)) {
      continue;
    }
    const auto reference = DirectParseForeignKeyReference(fields);
    if (!reference) {
      return fail_before_proof("bulk_fk_proof_descriptor_invalid");
    }
    const auto parent = FindVisibleCrudTable(state,
                                             reference->parent_table_uuid,
                                             request.context.local_transaction_id);
    if (!parent) {
      return fail_before_proof("bulk_fk_proof_parent_table_not_visible");
    }
    const auto parent_index = DirectVisibleUniqueIndexForColumn(
        state,
        parent->table_uuid,
        reference->parent_column,
        request.context.local_transaction_id);
    if (!parent_index) {
      return fail_before_proof("bulk_fk_proof_parent_index_missing");
    }

    scratchbird::core::bulk_load::BulkForeignKeyProofRequest foreign_key;
    foreign_key.constraint_uuid =
        DirectConstraintUuid(fields, table, column_name, "foreign_key");
    foreign_key.child_table_uuid = table.table_uuid;
    foreign_key.child_column_name = column_name;
    foreign_key.parent_table_uuid = parent->table_uuid;
    foreign_key.parent_column_name = reference->parent_column;
    foreign_key.parent_index_uuid = parent_index->index_uuid;
    foreign_key.batch_local_parent_allowed = true;
    for (std::size_t row_index = 0; row_index < staged_rows.size();
         ++row_index) {
      const auto& values = logical_value_batch[row_index];
      foreign_key.child_keys.push_back(
          DirectProofKey(CrudFieldValue(values, column_name),
                         staged_rows[row_index].row_uuid,
                         staged_rows[row_index].version_uuid,
                         row_index));
      if (parent->table_uuid == table.table_uuid) {
        foreign_key.batch_parent_keys.push_back(
            DirectProofKey(CrudFieldValue(values, reference->parent_column),
                           staged_rows[row_index].row_uuid,
                           staged_rows[row_index].version_uuid,
                           row_index));
      }
    }
    AddVisibleParentKeysForProof(state,
                                 request.context,
                                 parent->table_uuid,
                                 reference->parent_column,
                                 *parent_index,
                                 &foreign_key.visible_parent_keys,
                                 index_entries_authoritative);
    proof_request.foreign_key_proofs.push_back(std::move(foreign_key));
  }

  for (const auto& index : visible_indexes) {
    if (!DirectIndexIsUnique(index) ||
        proofed_unique_indexes.count(index.index_uuid) != 0) {
      continue;
    }
    scratchbird::core::bulk_load::BulkUniqueProofRequest unique;
    unique.constraint_uuid = "index:" + index.index_uuid + ":unique_key";
    unique.index_uuid = index.index_uuid;
    unique.table_uuid = table.table_uuid;
    const auto columns = DirectIndexKeyColumns(index);
    unique.column_name = columns.empty() ? index.column_name : columns.front();
    unique.nulls_distinct = true;
    if (const auto* entries =
            DirectPrecomputedEntriesForIndex(precomputed_entries,
                                             index.index_uuid)) {
      unique.incoming_keys_presorted = true;
      for (const auto& entry : *entries) {
        unique.incoming_keys.push_back(
            DirectProofKey(entry.encoded_key,
                           entry.row_uuid,
                           entry.version_uuid,
                           entry.source_ordinal));
      }
    } else {
      for (std::size_t row_index = 0; row_index < staged_rows.size();
           ++row_index) {
        const auto& values = logical_value_batch[row_index];
        for (const auto& key : CrudIndexKeysForValues(index, values)) {
          unique.incoming_keys.push_back(
              DirectProofKey(key,
                             staged_rows[row_index].row_uuid,
                             staged_rows[row_index].version_uuid,
                             row_index));
        }
      }
    }
    if (index_entries_authoritative && append_index_key_cache != nullptr) {
      AddCachedConflictingVisibleKeysForProof(index,
                                              unique.incoming_keys,
                                              append_index_key_cache,
                                              &unique.visible_keys);
    } else {
      AddVisibleRowKeysForProof(state,
                                request.context,
                                index,
                                &unique.visible_keys,
                                index_entries_authoritative);
    }
    proof_request.unique_proofs.push_back(std::move(unique));
  }

  const auto proven =
      scratchbird::core::bulk_load::ProveBulkConstraints(proof_request);
  AddCoreProofEvidence(proven.evidence, &selection.evidence);
  if (!proven.ok()) {
    selection.ok = false;
    selection.failure_reason = proven.refusal_reason.empty()
                                   ? "bulk_constraint_proof_refused"
                                   : proven.refusal_reason;
    selection.diagnostic = CoreBulkDiagnosticToEngine(proven.diagnostic,
                                                      selection.failure_reason);
    return selection;
  }
  return selection;
}

std::vector<CrudIndexRecord> DirectSynchronousIndexes(
    const InsertBatchContext& batch_context) {
  std::vector<CrudIndexRecord> indexes;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action == InsertIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    indexes.push_back(entry.index);
  }
  return indexes;
}

bool DirectAllIndexesUnique(const std::vector<CrudIndexRecord>& indexes) {
  return std::all_of(indexes.begin(), indexes.end(), [](const auto& index) {
    return DirectIndexIsUnique(index);
  });
}

bool DirectHasCommittedDeltaLedgerIndexes(
    const InsertBatchContext& batch_context) {
  return std::any_of(batch_context.index_plan.entries.begin(),
                     batch_context.index_plan.entries.end(),
                     [](const auto& entry) {
                       return entry.action ==
                              InsertIndexMaintenanceAction::committed_delta_ledger;
                     });
}

EngineApiU64 DirectUniqueIndexProbeCount(
    const std::vector<CrudIndexRecord>& visible_indexes,
    std::size_t row_count) {
  EngineApiU64 unique_indexes = 0;
  for (const auto& index : visible_indexes) {
    if (DirectIndexIsUnique(index)) {
      ++unique_indexes;
    }
  }
  return unique_indexes * static_cast<EngineApiU64>(row_count);
}

std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> DirectDeltaEntries(
    const InsertBatchContext& batch_context,
    const CrudRowVersionRecord& row_record,
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action != InsertIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    MgaSecondaryIndexDeltaLedgerEntryInput input;
    input.index = entry.index;
    input.table_uuid = batch_context.target_object_uuid;
    input.row_uuid = row_record.row_uuid;
    input.version_uuid = row_record.version_uuid;
    input.values = values;
    input.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::insert;
    input.source_evidence_reference =
        "engine.dml.direct_physical_bulk.secondary_index_delta:" +
        batch_context.statement_uuid;
    entries.push_back(std::move(input));
  }
  return entries;
}

std::vector<MgaIndexEntryAppendBatch> DirectIndexAppendBatches(
    const std::vector<CrudIndexRecord>& indexes,
    const std::string& table_uuid,
    const std::vector<MgaIndexEntryRowInput>& rows) {
  std::vector<MgaIndexEntryAppendBatch> batches;
  batches.reserve(indexes.size());
  for (const auto& index : indexes) {
    MgaIndexEntryAppendBatch batch;
    batch.index = index;
    batch.table_uuid = table_uuid;
    batch.rows = rows;
    batches.push_back(std::move(batch));
  }
  return batches;
}

std::vector<scratchbird::core::platform::byte> DirectBigEndianBytes(
    const std::vector<scratchbird::core::platform::byte>& little_endian) {
  std::vector<scratchbird::core::platform::byte> out = little_endian;
  std::reverse(out.begin(), out.end());
  return out;
}

bool DirectAppendSortableLittleEndianSegment(
    const std::vector<scratchbird::core::platform::byte>& raw,
    std::size_t offset,
    std::size_t width,
    bool signed_value,
    std::vector<scratchbird::core::platform::byte>* out) {
  if (out == nullptr || width == 0 || offset > raw.size() ||
      width > raw.size() - offset) {
    return false;
  }
  const auto original_size = out->size();
  out->reserve(out->size() + width);
  for (std::size_t index = 0; index < width; ++index) {
    out->push_back(raw[offset + width - 1 - index]);
  }
  if (signed_value) {
    (*out)[original_size] =
        static_cast<scratchbird::core::platform::byte>(
            (*out)[original_size] ^ 0x80u);
  }
  return true;
}

bool DirectSortableTypedIndexPayload(
    dt::CanonicalTypeId target_type,
    const EngineTypedValue& typed,
    std::vector<scratchbird::core::platform::byte>* out) {
  if (out == nullptr || typed.isSqlNull()) {
    return false;
  }
  std::vector<scratchbird::core::platform::byte> raw;
  if (!DirectPackTypedPayload(target_type, typed, &raw)) {
    return false;
  }
  switch (target_type) {
    case dt::CanonicalTypeId::boolean:
    case dt::CanonicalTypeId::uuid:
    case dt::CanonicalTypeId::enum_value:
    case dt::CanonicalTypeId::binary:
    case dt::CanonicalTypeId::ip_address:
    case dt::CanonicalTypeId::network_prefix:
    case dt::CanonicalTypeId::mac_address:
      *out = std::move(raw);
      return true;
    case dt::CanonicalTypeId::character:
      out->assign(typed.encoded_value.begin(), typed.encoded_value.end());
      return true;
    case dt::CanonicalTypeId::int8:
    case dt::CanonicalTypeId::int16:
    case dt::CanonicalTypeId::int32:
    case dt::CanonicalTypeId::int64:
    case dt::CanonicalTypeId::int128:
    case dt::CanonicalTypeId::date: {
      auto sortable = DirectBigEndianBytes(raw);
      if (!sortable.empty()) {
        sortable.front() =
            static_cast<scratchbird::core::platform::byte>(sortable.front() ^
                                                           0x80u);
      }
      *out = std::move(sortable);
      return true;
    }
    case dt::CanonicalTypeId::uint8:
    case dt::CanonicalTypeId::uint16:
    case dt::CanonicalTypeId::uint32:
    case dt::CanonicalTypeId::uint64:
    case dt::CanonicalTypeId::uint128:
    case dt::CanonicalTypeId::time:
      *out = DirectBigEndianBytes(raw);
      return true;
    case dt::CanonicalTypeId::timestamp: {
      std::vector<scratchbird::core::platform::byte> sortable;
      if (!DirectAppendSortableLittleEndianSegment(raw, 0, 8, true, &sortable) ||
          !DirectAppendSortableLittleEndianSegment(raw, 8, 4, false, &sortable) ||
          !DirectAppendSortableLittleEndianSegment(raw, 12, 4, true, &sortable)) {
        return false;
      }
      *out = std::move(sortable);
      return true;
    }
    case dt::CanonicalTypeId::interval: {
      std::vector<scratchbird::core::platform::byte> sortable;
      if (!DirectAppendSortableLittleEndianSegment(raw, 0, 4, true, &sortable) ||
          !DirectAppendSortableLittleEndianSegment(raw, 4, 4, true, &sortable) ||
          !DirectAppendSortableLittleEndianSegment(raw, 8, 8, true, &sortable)) {
        return false;
      }
      *out = std::move(sortable);
      return true;
    }
    case dt::CanonicalTypeId::bfloat16:
    case dt::CanonicalTypeId::real16:
    case dt::CanonicalTypeId::real32:
    case dt::CanonicalTypeId::real64:
    case dt::CanonicalTypeId::real128: {
      auto sortable = DirectBigEndianBytes(raw);
      if (sortable.empty()) {
        return false;
      }
      const bool negative = (sortable.front() & 0x80u) != 0;
      if (negative) {
        for (auto& value : sortable) {
          value = static_cast<scratchbird::core::platform::byte>(~value);
        }
      } else {
        sortable.front() =
            static_cast<scratchbird::core::platform::byte>(sortable.front() ^
                                                           0x80u);
      }
      *out = std::move(sortable);
      return true;
    }
    default:
      break;
  }
  const auto layout = dt::LookupDatatypeStorageLayout(target_type);
  if (layout.ok() &&
      layout.layout.storage_class != dt::DatatypeStorageClass::inline_fixed &&
      !raw.empty()) {
    *out = std::move(raw);
    return true;
  }
  return false;
}

const EngineTypedValue* DirectTypedValueForColumn(
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan,
    const std::string& column_name,
    std::string* target_type_name) {
  for (const auto& column : row_encoder_plan.columns) {
    if (column.column_name != column_name) {
      continue;
    }
    if (target_type_name != nullptr) {
      *target_type_name = column.canonical_type_name;
    }
    if (column.ordinal < input_row.fields.size() &&
        input_row.fields[column.ordinal].first == column_name) {
      return &input_row.fields[column.ordinal].second;
    }
    for (const auto& [field_name, typed] : input_row.fields) {
      if (field_name == column_name) {
        return &typed;
      }
    }
    return nullptr;
  }
  return nullptr;
}

bool DirectBuildTypedSimpleIndexKey(
    const CrudIndexRecord& index,
    const std::string& column_name,
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan,
    std::string* encoded_key,
    DirectTypedIndexKeyStats* stats) {
  if (encoded_key == nullptr) {
    return false;
  }
  if (stats != nullptr) {
    ++stats->typed_key_candidates;
  }
  std::string target_type_name;
  const EngineTypedValue* typed = DirectTypedValueForColumn(
      input_row, row_encoder_plan, column_name, &target_type_name);
  if (typed == nullptr) {
    if (stats != nullptr) { ++stats->typed_key_fallback; }
    return false;
  }
  const dt::CanonicalTypeId target_type =
      dt::CanonicalTypeIdFromStableName(target_type_name);
  if (target_type == dt::CanonicalTypeId::unknown) {
    if (stats != nullptr) { ++stats->typed_key_fallback; }
    return false;
  }

  scratchbird::core::index::IndexKeyEncodingComponent component;
  component.kind = target_type == dt::CanonicalTypeId::character
                       ? scratchbird::core::index::IndexKeyComponentKind::
                             collation_key
                       : scratchbird::core::index::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid =
      ParseDirectTypedUuid(UuidKind::object, index.index_uuid);
  component.type_descriptor_epoch = 1;
  component.null_placement =
      scratchbird::core::index::IndexKeyNullPlacement::nulls_first;
  component.is_null = typed->isSqlNull();
  if (!component.type_descriptor_uuid.valid()) {
    if (stats != nullptr) { ++stats->typed_key_fallback; }
    return false;
  }
  if (!component.is_null &&
      !DirectSortableTypedIndexPayload(target_type, *typed,
                                       &component.payload)) {
    if (stats != nullptr) { ++stats->typed_key_fallback; }
    return false;
  }
  const auto encoded =
      scratchbird::core::index::EncodeIndexKey({component}, {});
  if (!encoded.ok()) {
    if (stats != nullptr) { ++stats->typed_key_fallback; }
    return false;
  }
  *encoded_key = std::string(kDirectSbkoHexPrefix) +
                 DirectHexEncodeBytes(encoded.encoded);
  if (stats != nullptr) {
    ++stats->typed_key_encoded;
    ++stats->sbkohex_keys;
  }
  return true;
}

DirectPrecomputedIndexEntryMap DirectPrecomputeIndexEntries(
    const std::vector<CrudIndexRecord>& indexes,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch,
    std::span<const EngineRowValue> typed_input_rows = {},
    const InsertRowEncoderPlan* row_encoder_plan = nullptr,
    DirectTypedIndexKeyStats* typed_key_stats = nullptr) {
  DirectPrecomputedIndexEntryMap entries_by_index;
  for (const auto& index : indexes) {
    auto& entries = entries_by_index[index.index_uuid];
    entries.reserve(staged_rows.size());
    std::string simple_column;
    if (DirectSimpleScalarIndexKeyColumn(index, &simple_column)) {
      std::optional<std::size_t> simple_ordinal;
      if (!staged_rows.empty()) {
        const auto& first_values = logical_value_batch.front();
        for (std::size_t ordinal = 0;
             ordinal < first_values.size();
             ++ordinal) {
          if (first_values[ordinal].first == simple_column) {
            simple_ordinal = ordinal;
            break;
          }
        }
      }
      for (std::size_t row_index = 0;
           row_index < staged_rows.size();
           ++row_index) {
        const auto& values = logical_value_batch[row_index];
        const std::string* value = nullptr;
        if (simple_ordinal.has_value() &&
            *simple_ordinal < values.size() &&
            values[*simple_ordinal].first == simple_column) {
          value = &values[*simple_ordinal].second;
        } else {
          value = DirectFieldValuePtr(values, simple_column);
        }
        if (value == nullptr) { continue; }
        std::string encoded_key = *value;
        bool typed_key_built = false;
        if (!typed_input_rows.empty() &&
            row_encoder_plan != nullptr &&
            row_index < typed_input_rows.size()) {
          typed_key_built = DirectBuildTypedSimpleIndexKey(index,
                                                           simple_column,
                                                           typed_input_rows[row_index],
                                                           *row_encoder_plan,
                                                           &encoded_key,
                                                           typed_key_stats);
        }
        if (value->empty() && !typed_key_built) { continue; }
        entries.push_back({encoded_key,
                           *value,
                           staged_rows[row_index].row_uuid,
                           staged_rows[row_index].version_uuid,
                           static_cast<std::uint64_t>(row_index)});
      }
      continue;
    }
    for (std::size_t row_index = 0; row_index < staged_rows.size(); ++row_index) {
      const auto& values = logical_value_batch[row_index];
      const std::string payload = CrudFieldValue(values, index.column_name);
      for (const auto& key : CrudIndexKeysForValues(index, values)) {
        entries.push_back({key,
                           payload,
                           staged_rows[row_index].row_uuid,
                           staged_rows[row_index].version_uuid,
                           static_cast<std::uint64_t>(row_index)});
      }
    }
  }
  DirectSortPrecomputedIndexEntries(&entries_by_index);
  return entries_by_index;
}

std::vector<MgaExactIndexEntryAppendBatch> DirectExactIndexAppendBatches(
    const std::vector<CrudIndexRecord>& indexes,
    const std::string& table_uuid,
    const DirectPrecomputedIndexEntryMap& precomputed_entries) {
  std::vector<MgaExactIndexEntryAppendBatch> batches;
  batches.reserve(indexes.size());
  for (const auto& index : indexes) {
    const auto found = precomputed_entries.find(index.index_uuid);
    if (found == precomputed_entries.end() || found->second.empty()) {
      continue;
    }
    MgaExactIndexEntryAppendBatch batch;
    batch.index = index;
    batch.table_uuid = table_uuid;
    batch.entries.reserve(found->second.size());
    for (const auto& entry : found->second) {
      batch.entries.push_back({entry.encoded_key,
                               entry.payload_value,
                               entry.row_uuid,
                               entry.version_uuid});
    }
    if (batch.entries.size() > 1 &&
        !std::is_sorted(batch.entries.begin(),
                        batch.entries.end(),
                        [](const auto& left, const auto& right) {
                          if (left.encoded_key != right.encoded_key) {
                            return left.encoded_key < right.encoded_key;
                          }
                          if (left.row_uuid != right.row_uuid) {
                            return left.row_uuid < right.row_uuid;
                          }
                          return left.version_uuid < right.version_uuid;
                        })) {
      std::sort(batch.entries.begin(),
                batch.entries.end(),
                [](const auto& left, const auto& right) {
                  if (left.encoded_key != right.encoded_key) {
                    return left.encoded_key < right.encoded_key;
                  }
                  if (left.row_uuid != right.row_uuid) {
                    return left.row_uuid < right.row_uuid;
                  }
                  return left.version_uuid < right.version_uuid;
                });
    }
    batches.push_back(std::move(batch));
  }
  return batches;
}

scratchbird::core::index::IndexFamily DirectCoreIndexFamily(
    const CrudIndexRecord& index) {
  namespace idx = scratchbird::core::index;
  const std::string family =
      index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  if (family == kCrudIndexFamilyBtree) {
    return DirectIndexIsUnique(index) ? idx::IndexFamily::unique_btree
                                      : idx::IndexFamily::btree;
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

bool DirectSortedBulkIndexBuildEnabled(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary = DirectOptionValue(request, "sorted_bulk_index_build");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string odf = DirectOptionValue(request, "odf044.sorted_bulk_index_build");
  if (!odf.empty()) {
    return IsDirectTruthyValue(odf);
  }
  return false;
}

bool DirectOptionTruthy(const DirectPhysicalBulkAppendRequest& request,
                        const std::string& key) {
  const std::string value = DirectOptionValue(request, key);
  return !value.empty() && IsDirectTruthyValue(value);
}

bool DirectBypassPostAppendCacheForSingleWindowNativeBulk(
    const DirectPhysicalBulkAppendRequest& request) {
  if (request.lane_operation == "native_bulk") {
    return DirectOptionTruthy(request, "native_bulk.single_window");
  }
  return request.lane_operation == "insert_select" &&
         DirectOptionTruthy(request, "insert_select.single_window");
}

bool DirectOpaqueColumnsAllowed(const DirectPhysicalBulkAppendRequest& request) {
  return DirectOptionEnabled(request, "bulk.allow_opaque_columns=true") ||
         DirectOptionTruthy(request, "bulk.allow_opaque_columns");
}

bool DirectDeferredIndexBenchmarkCleanRequired(
    const DirectPhysicalBulkAppendRequest& request) {
  return DirectOptionTruthy(
             request,
             "orh.deferred_index.require_benchmark_clean") ||
         DirectOptionTruthy(
             request,
             "orh.deferred_index_bulk_publish.require_benchmark_clean");
}

bool DirectDeferredIndexCallerProofFlagPresent(
    const DirectPhysicalBulkAppendRequest& request) {
  static constexpr const char* kCallerProofFlags[] = {
      "index_correctness_proven",
      "sorted_root_publish_recovery_proof",
      "rollback_proof",
      "reopen_repair_rebuild_proof",
      "mga_visibility_recheck_proof",
      "security_recheck_proof",
      "authoritative_base_repair_proof"};
  for (const auto* suffix : kCallerProofFlags) {
    if (!DirectOptionValue(request, std::string("orh.deferred_index.") + suffix)
             .empty() ||
        !DirectOptionValue(request,
                           std::string("orh.deferred_index_bulk_publish.") +
                               suffix)
             .empty()) {
      return true;
    }
  }
  return false;
}

EngineApiDiagnostic DeferredIndexRouteNotSelectedDiagnostic() {
  return MakeEngineApiDiagnostic(
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.ROUTE_NOT_SELECTED",
      "orh.deferred_index_bulk_publish.route_not_selected",
      "benchmark-clean deferred sorted bulk publish was required but the live sorted bulk route was not selected",
      true);
}

EngineApiDiagnostic DeferredIndexFamilyUnsupportedDiagnostic(
    const std::string& family) {
  return MakeEngineApiDiagnostic(
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.FAMILY_ROUTE_UNSUPPORTED",
      "orh.deferred_index_bulk_publish.family_route_unsupported",
      "deferred sorted bulk publish is not an ordered write route for index family " +
          family,
      true);
}

EngineApiDiagnostic DeferredIndexUniqueReservationProofRequiredDiagnostic() {
  return MakeEngineApiDiagnostic(
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.UNIQUE_RESERVATION_PROOF_REQUIRED",
      "orh.deferred_index_bulk_publish.unique_reservation_proof_required",
      "deferred sorted bulk publish cannot claim unique index behavior without reservation ledger proof",
      true);
}

void AddRootPublishEvidence(
    const scratchbird::core::index::IndexRootGenerationPublishResult& publish,
    std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_root_publish_provider",
       "core.index.PublishIndexRootGeneration"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_root_publish_authorized",
       publish.root_publish_authorized ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_root_reopen_safe",
       publish.reopen_safe_metadata_contract ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_root_rollback_safe",
       publish.rollback_safe_metadata_contract ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_index_metadata_recovery_authority",
       publish.recovery_authority ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_index_metadata_finality_authority",
       publish.transaction_finality_authority ? "true" : "false"});
  for (const auto& item : publish.evidence) {
    evidence->push_back(
        {"sorted_bulk_root_publish." + item.evidence_kind,
         item.evidence_id});
  }
}

std::string DirectBulkActiveRootName(
    scratchbird::core::index::IndexBulkPublishActiveRoot active_root) {
  using scratchbird::core::index::IndexBulkPublishActiveRoot;
  switch (active_root) {
    case IndexBulkPublishActiveRoot::old_root:
      return "old_root";
    case IndexBulkPublishActiveRoot::new_root:
      return "new_root";
    case IndexBulkPublishActiveRoot::none:
      return "none";
  }
  return "unknown";
}

void AddRootRecoveryEvidence(
    const std::string& crash_point,
    const scratchbird::core::index::IndexBulkPublishRecoveryResult& recovery,
    std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_recovery_provider",
       "core.index.RecoverSortedBulkRootPublish"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_crash_" + crash_point,
       recovery.crash_classification});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_crash_" + crash_point +
           "_active_root",
       DirectBulkActiveRootName(recovery.active_root)});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_crash_" + crash_point +
           "_half_root_exposed",
       recovery.half_root_exposed ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_crash_" + crash_point +
           "_repair_classification",
       recovery.repair_classification});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_recovery_authority",
       recovery.transaction_finality_authority ? "true" : "false"});
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_recovery_benchmark_clean",
       recovery.benchmark_clean ? "true" : "false"});
  for (const auto& item : recovery.evidence) {
    evidence->push_back(
        {"sorted_bulk_root_recovery." + crash_point + "." +
             item.evidence_kind,
         item.evidence_id});
  }
}

bool RequireRootRecovery(
    const scratchbird::core::index::IndexBulkPublishRecoveryResult& recovery,
    scratchbird::core::index::IndexBulkPublishActiveRoot expected_root,
    std::string* failure_reason,
    EngineApiDiagnostic* diagnostic) {
  if (!recovery.ok() ||
      recovery.active_root != expected_root ||
      recovery.half_root_exposed) {
    *failure_reason = "sorted_bulk_index_root_recovery_refused";
    *diagnostic = MakeEngineApiDiagnostic(
        recovery.diagnostic.diagnostic_code.empty()
            ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.ROOT_RECOVERY_REFUSED"
            : recovery.diagnostic.diagnostic_code,
        recovery.diagnostic.message_key.empty()
            ? "orh.deferred_index_bulk_publish.root_recovery_refused"
            : recovery.diagnostic.message_key,
        *failure_reason,
        true);
    return false;
  }
  return true;
}

bool ProveDirectSortedRootPublishRecovery(
    const scratchbird::core::index::SortedBulkIndexBuildResult& built,
    const TypedUuid& table_uuid,
    scratchbird::core::index::IndexFamily family,
    const std::string& semantic_profile,
    std::vector<EngineEvidenceReference>* evidence,
    EngineApiDiagnostic* diagnostic,
    std::string* failure_reason) {
  namespace idx = scratchbird::core::index;

  idx::SortedBulkIndexBuildRequest old_request;
  old_request.metadata.index_uuid =
      built.candidate_root_generation.tree.index_uuid;
  old_request.metadata.table_uuid = table_uuid;
  old_request.metadata.family = family;
  old_request.metadata.family_name =
      idx::IndexFamilyName(family);
  old_request.metadata.semantic_profile = semantic_profile;
  old_request.metadata.leaf_entry_capacity = 128;
  const std::string old_row_uuid =
      built.entries.empty() ? std::string{} : built.entries.front().row_uuid;
  const std::string old_version_uuid =
      built.entries.empty() ? std::string{} : built.entries.front().version_uuid;
  old_request.rows.push_back({"__orh211_old_root__",
                              old_row_uuid,
                              old_version_uuid,
                              "__orh211_old_root_payload__",
                              0,
                              false});
  const auto old_built = idx::BuildSortedExactBulkIndex(old_request);
  if (!old_built.ok()) {
    *failure_reason = "sorted_bulk_index_old_root_fixture_refused";
    *diagnostic = MakeEngineApiDiagnostic(
        old_built.diagnostic.diagnostic_code.empty()
            ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.OLD_ROOT_BUILD_REFUSED"
            : old_built.diagnostic.diagnostic_code,
        old_built.diagnostic.message_key.empty()
            ? "orh.deferred_index_bulk_publish.old_root_build_refused"
            : old_built.diagnostic.message_key,
        *failure_reason,
        true);
    return false;
  }
  const auto old_image =
      scratchbird::storage::page::ExportIndexBtreePhysicalTreeImage(
          old_built.candidate_root_generation.tree);
  if (!old_image.ok()) {
    *failure_reason = "sorted_bulk_index_old_root_export_refused";
    *diagnostic = MakeEngineApiDiagnostic(
        old_image.diagnostic.diagnostic_code.empty()
            ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.OLD_ROOT_EXPORT_REFUSED"
            : old_image.diagnostic.diagnostic_code,
        old_image.diagnostic.message_key.empty()
            ? "orh.deferred_index_bulk_publish.old_root_export_refused"
            : old_image.diagnostic.message_key,
        *failure_reason,
        true);
    return false;
  }

  idx::IndexMetapageControl current;
  current.index_uuid = old_built.candidate_root_generation.tree.index_uuid;
  current.family = family;
  current.root_page_number =
      old_built.candidate_root_generation.root_page_number;
  current.resource_epoch = 1;
  current.mutation_epoch = 1;
  current.root_generation = 0;
  current.page_count = old_built.candidate_root_generation.page_count;
  current.tuple_count_estimate =
      old_built.candidate_root_generation.live_entry_count;
  current.semantic_profile_id = semantic_profile;

  idx::IndexRootGenerationPublishRequest publish_request;
  publish_request.current_metapage = current;
  publish_request.candidate = built.candidate_root_generation;
  publish_request.candidate_tree_validation_proof = true;
  publish_request.durable_metadata_write_evidence = true;
  publish_request.mga_finality_authority_evidence = true;
  publish_request.durable_metadata_evidence_token =
      "direct_physical_bulk.sorted_root_publish_metapage";
  publish_request.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  publish_request.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";

  const auto publish =
      idx::PublishIndexRootGeneration(publish_request);
  AddRootPublishEvidence(publish, evidence);
  if (!publish.ok()) {
    *failure_reason = "sorted_bulk_index_root_publish_refused";
    *diagnostic = MakeEngineApiDiagnostic(
        publish.diagnostic.diagnostic_code.empty()
            ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.ROOT_PUBLISH_REFUSED"
            : publish.diagnostic.diagnostic_code,
        publish.diagnostic.message_key.empty()
            ? "orh.deferred_index_bulk_publish.root_publish_refused"
            : publish.diagnostic.message_key,
        *failure_reason,
        true);
    return false;
  }

  idx::IndexBulkPublishRecoveryState recovery_state;
  recovery_state.old_metapage_present = true;
  recovery_state.old_metapage = publish.old_metapage;
  recovery_state.old_tree_image_present = true;
  recovery_state.old_tree_image = old_image.image;
  recovery_state.candidate_generation = built.candidate_root_generation;
  recovery_state.candidate_tree_image = publish.published_tree_image;
  recovery_state.candidate_tree_validation_proof = true;
  recovery_state.durable_metadata_write_evidence = true;
  recovery_state.root_publish_authorization_proof = true;
  recovery_state.mga_finality_authority_evidence = true;
  recovery_state.durable_metadata_evidence_token =
      "direct_physical_bulk.sorted_root_publish_metapage";
  recovery_state.root_publish_authorization_token =
      "direct_physical_bulk.root_publish_succeeded";
  recovery_state.mga_authority_evidence_token =
      "engine_mga_transaction_inventory_finality_authority";
  recovery_state.publish_fence_token =
      "mga_index_append_path_after_bottom_up_root_generation";
  recovery_state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_before_root_publish;
  recovery_state.durable_metapage_image.reset();
  const auto before_recovery =
      idx::RecoverSortedBulkRootPublish(recovery_state);
  AddRootRecoveryEvidence("before_root_publish", before_recovery, evidence);
  if (!RequireRootRecovery(before_recovery,
                           idx::IndexBulkPublishActiveRoot::old_root,
                           failure_reason,
                           diagnostic)) {
    return false;
  }

  recovery_state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_during_root_publish;
  recovery_state.durable_metapage_image.reset();
  const auto during_recovery =
      idx::RecoverSortedBulkRootPublish(recovery_state);
  AddRootRecoveryEvidence("during_root_publish", during_recovery, evidence);
  if (!RequireRootRecovery(during_recovery,
                           idx::IndexBulkPublishActiveRoot::old_root,
                           failure_reason,
                           diagnostic)) {
    return false;
  }

  recovery_state.crash_point =
      idx::IndexBulkPublishCrashPoint::crash_after_root_publish;
  recovery_state.durable_metapage_image = publish.published_metapage_image;
  const auto after_recovery =
      idx::RecoverSortedBulkRootPublish(recovery_state);
  AddRootRecoveryEvidence("after_root_publish", after_recovery, evidence);
  if (!RequireRootRecovery(after_recovery,
                           idx::IndexBulkPublishActiveRoot::new_root,
                           failure_reason,
                           diagnostic)) {
    return false;
  }
  evidence->push_back(
      {"orh_deferred_index_bulk_publish_sorted_root_recovery_proven",
       "true"});
  return true;
}

struct DirectSortedBulkIndexBuildSelection {
  bool selected = false;
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<CrudIndexRecord> retail_indexes;
  std::vector<MgaExactIndexEntryAppendBatch> exact_batches;
  std::vector<EngineEvidenceReference> evidence;
};

DirectSortedBulkIndexBuildSelection BuildDirectSortedBulkIndexArtifacts(
    const DirectPhysicalBulkAppendRequest& request,
    const CrudState& state,
    const std::vector<CrudIndexRecord>& synchronous_indexes,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch,
    bool index_entries_authoritative,
    const std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>* append_index_entry_key_cache) {
  DirectSortedBulkIndexBuildSelection selection;
  if (!DirectSortedBulkIndexBuildEnabled(request)) {
    if (DirectDeferredIndexBenchmarkCleanRequired(request)) {
      selection.selected = false;
      selection.ok = false;
      selection.failure_reason = "deferred_index_bulk_publish_route_not_selected";
      selection.diagnostic = DeferredIndexRouteNotSelectedDiagnostic();
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_selected", "false"});
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_benchmark_clean", "blocked"});
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_caller_proof_authority", "false"});
      if (DirectDeferredIndexCallerProofFlagPresent(request)) {
        selection.evidence.push_back(
            {"orh_deferred_index_bulk_publish_caller_proof_flags_ignored",
             "true"});
      }
      return selection;
    }
    selection.retail_indexes = synchronous_indexes;
    return selection;
  }
  selection.selected = true;
  selection.evidence.push_back(
      {"orh_deferred_index_bulk_publish_selected", "true"});
  selection.evidence.push_back(
      {"orh_deferred_index_bulk_publish_caller_proof_authority", "false"});
  selection.evidence.push_back(
      {"orh_deferred_index_bulk_publish_index_metadata_finality_authority",
       "false"});
  if (DirectDeferredIndexCallerProofFlagPresent(request)) {
    selection.evidence.push_back(
        {"orh_deferred_index_bulk_publish_caller_proof_flags_ignored",
         "true"});
  }

  const TypedUuid table_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  if (!table_uuid.valid()) {
    selection.ok = false;
    selection.failure_reason = "sorted_bulk_index_table_uuid_invalid";
    selection.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.direct_physical_bulk_append",
        selection.failure_reason);
    return selection;
  }

  for (const auto& index : synchronous_indexes) {
    const auto family = DirectCoreIndexFamily(index);
    const std::string family_name =
        index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
    if (family == scratchbird::core::index::IndexFamily::unknown) {
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_family_blocked",
           family_name + "=not_ordered_write_family"});
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_benchmark_clean", "blocked"});
      if (DirectDeferredIndexBenchmarkCleanRequired(request)) {
        selection.ok = false;
        selection.failure_reason =
            "deferred_index_bulk_publish_family_route_unsupported";
        selection.diagnostic =
            DeferredIndexFamilyUnsupportedDiagnostic(family_name);
        return selection;
      }
      selection.retail_indexes.push_back(index);
      continue;
    }
    const TypedUuid index_uuid =
        ParseDirectTypedUuid(UuidKind::object, index.index_uuid);
    if (!index_uuid.valid()) {
      selection.ok = false;
      selection.failure_reason = "sorted_bulk_index_uuid_invalid";
      selection.diagnostic = MakeInvalidRequestDiagnostic(
          "dml.direct_physical_bulk_append",
          selection.failure_reason);
      return selection;
    }
    scratchbird::core::index::SortedBulkIndexBuildRequest build;
    build.metadata.index_uuid = index_uuid;
    build.metadata.table_uuid = table_uuid;
    build.metadata.family = family;
    build.metadata.family_name =
        family_name;
    build.metadata.semantic_profile = NormalizeCrudIndexProfile(index.profile);
    build.metadata.unique = DirectIndexIsUnique(index);
    build.metadata.rebuild = false;
    build.metadata.input_presorted = false;
    build.metadata.order_proof_valid = false;
    build.metadata.policy_allows_mutation = true;
    build.metadata.leaf_entry_capacity = 128;
    scratchbird::core::index::UniqueIndexReservationLedger unique_ledger;
    for (std::size_t row_index = 0; row_index < staged_rows.size(); ++row_index) {
      const auto& values = logical_value_batch[row_index];
      for (const auto& key : CrudIndexKeysForValues(index, values)) {
        scratchbird::core::index::SortedBulkIndexRowInput input;
        input.encoded_key = key;
        input.row_uuid = staged_rows[row_index].row_uuid;
        input.version_uuid = staged_rows[row_index].version_uuid;
        input.payload_value = CrudFieldValue(values, index.column_name);
        input.source_ordinal = static_cast<std::uint64_t>(row_index);
        input.null_key = DirectNullValue(input.encoded_key) ||
                         input.encoded_key.find("<NULL>") != std::string::npos;
        build.rows.push_back(std::move(input));
      }
    }
    if (build.metadata.unique) {
      if (index_entries_authoritative &&
          append_index_entry_key_cache != nullptr) {
        AddCachedConflictingVisibleKeysForSortedBuild(
            index,
            build.rows,
            append_index_entry_key_cache,
            &build.visible_unique_keys);
      } else {
        AddVisibleRowKeysForSortedBuild(state,
                                        request.context,
                                        index,
                                        &build.visible_unique_keys,
                                        index_entries_authoritative);
      }
      build.unique_reservation_ledger = &unique_ledger;
      build.validate_unique_reservation_batch = true;
      build.unique_constraint_uuid = index_uuid;
      build.transaction_uuid =
          ParseDirectTypedUuid(UuidKind::transaction,
                               request.context.transaction_uuid.canonical);
      build.local_transaction_id = request.context.local_transaction_id;
      build.unique_reservation_validation_evidence_token =
          "direct_sorted_bulk_unique_reservation_validation";
      scratchbird::core::index::UniqueIndexReservationTransactionProof proof;
      proof.transaction_uuid = build.transaction_uuid;
      proof.local_transaction_id = build.local_transaction_id;
      proof.state = scratchbird::transaction::mga::TransactionState::active;
      proof.engine_mga_authority = true;
      proof.durable_transaction_inventory_authoritative = true;
      proof.evidence_token =
          "direct_sorted_bulk_engine_transaction_inventory_active";
      build.unique_transaction_state_proofs.push_back(std::move(proof));
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_unique_deferred_gated",
           "reservation_ledger_validated"});
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_unique_constraint_uuid",
           index.index_uuid});
    }

    const auto built = scratchbird::core::index::BuildSortedExactBulkIndex(build);
    if (!built.ok()) {
      selection.ok = false;
      selection.failure_reason = built.uniqueness_refused
                                     ? "sorted_bulk_index_unique_duplicate"
                                     : "sorted_bulk_index_build_refused";
      selection.diagnostic = MakeEngineApiDiagnostic(
          built.diagnostic.diagnostic_code.empty()
              ? (build.metadata.unique
                     ? "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.UNIQUE_RESERVATION_PROOF_REQUIRED"
                     : "SB_ENGINE_API_INVALID_REQUEST")
              : built.diagnostic.diagnostic_code,
          built.diagnostic.message_key.empty()
              ? (build.metadata.unique
                     ? "orh.deferred_index_bulk_publish.unique_reservation_proof_required"
                     : "engine.api.invalid_request")
              : built.diagnostic.message_key,
          selection.failure_reason,
          true);
      return selection;
    }
    if (build.metadata.unique &&
        (!built.unique_reservation_ledger_used ||
         !built.unique_reservation_validation_passed)) {
      selection.ok = false;
      selection.failure_reason =
          "deferred_index_bulk_publish_unique_reservation_proof_required";
      selection.diagnostic = DeferredIndexUniqueReservationProofRequiredDiagnostic();
      selection.evidence.push_back(
          {"orh_deferred_index_bulk_publish_unique_deferred_gated",
           "reservation_ledger_required"});
      return selection;
    }
    if (!ProveDirectSortedRootPublishRecovery(built,
                                              table_uuid,
                                              family,
                                              build.metadata.semantic_profile,
                                              &selection.evidence,
                                              &selection.diagnostic,
                                              &selection.failure_reason)) {
      selection.ok = false;
      return selection;
    }

    MgaExactIndexEntryAppendBatch batch;
    batch.index = index;
    batch.table_uuid = request.target_table.uuid.canonical;
    batch.entries.reserve(built.entries.size());
    for (const auto& entry : built.entries) {
      batch.entries.push_back({entry.encoded_key,
                               entry.payload_value,
                               entry.row_uuid,
                               entry.version_uuid});
    }
    selection.exact_batches.push_back(std::move(batch));
    for (const auto& item : built.evidence) {
      selection.evidence.push_back({item.evidence_kind, item.evidence_id});
    }
    selection.evidence.push_back({"sorted_bulk_index_uuid", index.index_uuid});
    selection.evidence.push_back({"sorted_bulk_index_build_selected", "true"});
    if (build.metadata.unique && built.uniqueness_proven) {
      selection.evidence.push_back({"sorted_bulk_index_uniqueness_proof",
                                    "sorted_duplicate_runs_absent"});
    }
  }

  selection.evidence.push_back({"sorted_bulk_index_build_route",
                                "direct_physical_bulk"});
  selection.evidence.push_back({"sorted_bulk_index_exact_append_path",
                                "mga_index_append_path"});
  if (!selection.exact_batches.empty()) {
    selection.evidence.push_back(
        {"orh_deferred_index_bulk_publish_consumed_provider",
         "core.index.sorted_bulk_index_build"});
  }
  return selection;
}

std::uint64_t DirectOptionU64(const DirectPhysicalBulkAppendRequest& request,
                              const std::string& key,
                              std::uint64_t fallback) {
  const std::string value = DirectOptionValue(request, key);
  if (value.empty()) {
    return fallback;
  }
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return fallback;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return fallback;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed == 0 ? fallback : parsed;
}

std::optional<std::uint64_t> DirectOptionU64Optional(
    const DirectPhysicalBulkAppendRequest& request,
    const std::string& key) {
  const std::string value = DirectOptionValue(request, key);
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed;
}

enum class DirectGeneratedProjectionKind {
  unsupported,
  counter,
  literal,
  mod,
  prefix_counter,
  prefix_counter_offset,
  case_zero_literal_else_literal,
  case_zero_literal_else_prefix_counter_offset,
  cast_divide,
  counter_multiply,
  mod_equals
};

struct DirectScaledDecimalOperand {
  std::uint64_t value = 0;
  int scale = 0;
};

struct DirectGeneratedProjectionPlan {
  std::string column_name;
  std::string descriptor;
  std::string type_name;
  std::vector<std::string> parts;
  DirectGeneratedProjectionKind kind = DirectGeneratedProjectionKind::unsupported;
  std::string literal_value;
  std::string alternate_literal_value;
  std::string prefix;
  std::uint64_t modulus = 0;
  std::uint64_t expected = 0;
  long long offset = 0;
  long double factor = 0.0L;
  int scale = 0;
  bool has_scaled_operand = false;
  DirectScaledDecimalOperand scaled_operand;
  bool has_exact_scaled_result_multiplier = false;
  std::uint64_t exact_scaled_result_multiplier = 0;
  std::uint64_t output_scale_factor = 0;
};

struct DirectGeneratedCounterPlan {
  bool requested = false;
  bool ok = false;
  std::string failure_reason;
  std::uint64_t start = 0;
  std::uint64_t step = 0;
  std::uint64_t limit = 0;
  std::uint64_t row_count = 0;
  std::vector<DirectGeneratedProjectionPlan> projections;
};

bool DirectGeneratedCounterEnvelopeRequested(
    const DirectPhysicalBulkAppendRequest& request) {
  return request.lane_operation == "insert_select" &&
         DirectOptionValue(request, "insert_select_source_kind") ==
             "recursive_counter_cte";
}

std::optional<std::uint64_t> DirectGeneratedCounterRowCount(
    std::uint64_t start,
    std::uint64_t step,
    std::uint64_t limit) {
  if (step == 0 || limit < start) {
    return std::nullopt;
  }
  return ((limit - start) / step) + 1;
}

std::optional<std::uint64_t> DirectPow10U64(int scale) {
  if (scale < 0 || scale > 18) {
    return std::nullopt;
  }
  std::uint64_t value = 1;
  for (int index = 0; index < scale; ++index) {
    value *= 10;
  }
  return value;
}

std::optional<std::uint64_t> DirectCheckedMultiplyU64(std::uint64_t lhs,
                                                       std::uint64_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    return std::nullopt;
  }
  return lhs * rhs;
}

std::optional<std::uint64_t> DirectRoundDivideU64(std::uint64_t numerator,
                                                  std::uint64_t denominator) {
  if (denominator == 0) {
    return std::nullopt;
  }
  const std::uint64_t half = denominator / 2;
  if (numerator > std::numeric_limits<std::uint64_t>::max() - half) {
    return std::nullopt;
  }
  return (numerator + half) / denominator;
}

void DirectAppendU64(std::string* out, std::uint64_t value) {
  if (out == nullptr) {
    return;
  }
  char buffer[32];
  auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec == std::errc()) {
    out->append(buffer, static_cast<std::size_t>(ptr - buffer));
  }
}

std::optional<DirectScaledDecimalOperand> DirectParseScaledDecimal(
    std::string_view value) {
  if (value.empty() || value.front() == '-') {
    return std::nullopt;
  }
  DirectScaledDecimalOperand operand;
  bool seen_digit = false;
  bool seen_dot = false;
  for (const char ch : value) {
    if (ch == '.') {
      if (seen_dot) {
        return std::nullopt;
      }
      seen_dot = true;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
    seen_digit = true;
    if (operand.value > (std::numeric_limits<std::uint64_t>::max() / 10)) {
      return std::nullopt;
    }
    operand.value *= 10;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (operand.value > std::numeric_limits<std::uint64_t>::max() - digit) {
      return std::nullopt;
    }
    operand.value += digit;
    if (seen_dot) {
      ++operand.scale;
      if (operand.scale > 18) {
        return std::nullopt;
      }
    }
  }
  if (!seen_digit || operand.value == 0) {
    return std::nullopt;
  }
  return operand;
}

std::string DirectFormatScaledDecimal(std::uint64_t scaled_value, int scale) {
  if (scale <= 0) {
    std::string out;
    out.reserve(20);
    DirectAppendU64(&out, scaled_value);
    return out;
  }
  const auto divisor = DirectPow10U64(scale);
  if (!divisor || *divisor == 0) {
    return {};
  }
  const std::uint64_t whole = scaled_value / *divisor;
  const std::uint64_t fraction = scaled_value % *divisor;
  std::string out;
  out.reserve(32);
  DirectAppendU64(&out, whole);
  out.push_back('.');
  std::string fraction_text;
  fraction_text.reserve(20);
  DirectAppendU64(&fraction_text, fraction);
  if (fraction_text.size() < static_cast<std::size_t>(scale)) {
    out.append(static_cast<std::size_t>(scale) - fraction_text.size(), '0');
  }
  out.append(fraction_text);
  return out;
}

std::string DirectFormatScaledDecimalWithFactor(std::uint64_t scaled_value,
                                                int scale,
                                                std::uint64_t divisor) {
  if (scale <= 0) {
    std::string out;
    out.reserve(20);
    DirectAppendU64(&out, scaled_value);
    return out;
  }
  if (divisor == 0) {
    return {};
  }
  const std::uint64_t whole = scaled_value / divisor;
  const std::uint64_t fraction = scaled_value % divisor;
  std::string out;
  out.reserve(32);
  DirectAppendU64(&out, whole);
  out.push_back('.');
  std::string fraction_text;
  fraction_text.reserve(20);
  DirectAppendU64(&fraction_text, fraction);
  if (fraction_text.size() < static_cast<std::size_t>(scale)) {
    out.append(static_cast<std::size_t>(scale) - fraction_text.size(), '0');
  }
  out.append(fraction_text);
  return out;
}

bool DirectParseLongLong(const std::string& value, long long* out) {
  if (out == nullptr || value.empty()) {
    return false;
  }
  try {
    *out = std::stoll(value);
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<std::uint64_t> DirectParseU64(const std::string& value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed;
}

bool DirectParseLongDouble(const std::string& value, long double* out) {
  if (out == nullptr || value.empty()) {
    return false;
  }
  try {
    *out = std::stold(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool DirectParseIntScale(const std::string& value, int* out) {
  if (out == nullptr || value.empty()) {
    return false;
  }
  try {
    const int parsed = std::stoi(value);
    if (parsed < 0 || parsed > 18) {
      return false;
    }
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

std::string DirectGeneratedProjectionType(const std::string& descriptor,
                                          const std::string& target_descriptor) {
  if (descriptor == "counter") return "integer";
  if (descriptor.rfind("literal_text:", 0) == 0) return "text";
  if (descriptor.rfind("literal_boolean:", 0) == 0) return "boolean";
  if (descriptor.rfind("literal_integer:", 0) == 0) return "integer";
  if (descriptor.rfind("mod:", 0) == 0) return "integer";
  if (descriptor.rfind("prefix_counter:", 0) == 0) return "text";
  if (descriptor.rfind("prefix_counter_offset:", 0) == 0) return "text";
  if (descriptor.rfind("case_zero_literal_else_", 0) == 0) return "text";
  if (descriptor.rfind("cast_divide:", 0) == 0) return "decimal";
  if (descriptor.rfind("counter_multiply:", 0) == 0) return "decimal";
  if (descriptor.rfind("mod_equals:", 0) == 0) return "boolean";
  if (target_descriptor.rfind("type=", 0) == 0) {
    return target_descriptor.substr(5);
  }
  return target_descriptor.empty() ? "text" : target_descriptor;
}

DirectGeneratedProjectionPlan DirectBuildGeneratedProjectionPlan(
    const std::string& column_name,
    const std::string& descriptor,
    const std::string& target_descriptor) {
  DirectGeneratedProjectionPlan plan;
  plan.column_name = column_name;
  plan.descriptor = descriptor;
  plan.parts = DirectSplitText(descriptor, ':');
  plan.type_name = DirectGeneratedProjectionType(descriptor, target_descriptor);
  if (plan.type_name.empty()) {
    plan.type_name = "text";
  }
  if (descriptor == "counter") {
    plan.kind = DirectGeneratedProjectionKind::counter;
    return plan;
  }
  if (plan.parts.empty()) {
    return plan;
  }
  if (plan.parts[0] == "literal_text" && plan.parts.size() >= 2) {
    plan.kind = DirectGeneratedProjectionKind::literal;
    plan.literal_value = plan.parts[1];
    return plan;
  }
  if ((plan.parts[0] == "literal_boolean" ||
       plan.parts[0] == "literal_integer") &&
      plan.parts.size() == 2) {
    plan.kind = DirectGeneratedProjectionKind::literal;
    plan.literal_value = plan.parts[1];
    return plan;
  }
  if (plan.parts[0] == "mod" && plan.parts.size() == 2) {
    const auto modulus = DirectParseU64(plan.parts[1]);
    if (modulus && *modulus != 0) {
      plan.kind = DirectGeneratedProjectionKind::mod;
      plan.modulus = *modulus;
      return plan;
    }
  }
  if (plan.parts[0] == "prefix_counter" && plan.parts.size() >= 2) {
    plan.kind = DirectGeneratedProjectionKind::prefix_counter;
    plan.prefix = plan.parts[1];
    return plan;
  }
  if (plan.parts[0] == "prefix_counter_offset" &&
      plan.parts.size() == 3 &&
      DirectParseLongLong(plan.parts[2], &plan.offset)) {
    plan.kind = DirectGeneratedProjectionKind::prefix_counter_offset;
    plan.prefix = plan.parts[1];
    return plan;
  }
  if (plan.parts[0] == "case_zero_literal_else_literal" &&
      plan.parts.size() == 3) {
    plan.kind = DirectGeneratedProjectionKind::case_zero_literal_else_literal;
    plan.literal_value = plan.parts[1];
    plan.alternate_literal_value = plan.parts[2];
    return plan;
  }
  if (plan.parts[0] == "case_zero_literal_else_prefix_counter_offset" &&
      plan.parts.size() == 4 &&
      DirectParseLongLong(plan.parts[3], &plan.offset)) {
    plan.kind =
        DirectGeneratedProjectionKind::case_zero_literal_else_prefix_counter_offset;
    plan.literal_value = plan.parts[1];
    plan.prefix = plan.parts[2];
    return plan;
  }
  if (plan.parts[0] == "cast_divide" && plan.parts.size() >= 4 &&
      DirectParseLongDouble(plan.parts[2], &plan.factor) &&
      plan.factor != 0.0L &&
      DirectParseIntScale(plan.parts[3], &plan.scale)) {
    plan.kind = DirectGeneratedProjectionKind::cast_divide;
    if (const auto scaled = DirectParseScaledDecimal(plan.parts[2])) {
      plan.has_scaled_operand = true;
      plan.scaled_operand = *scaled;
    }
    return plan;
  }
  if (plan.parts[0] == "counter_multiply" && plan.parts.size() >= 3 &&
      DirectParseLongDouble(plan.parts[1], &plan.factor) &&
      DirectParseIntScale(plan.parts[2], &plan.scale)) {
    plan.kind = DirectGeneratedProjectionKind::counter_multiply;
    if (const auto output_scale = DirectPow10U64(plan.scale)) {
      plan.output_scale_factor = *output_scale;
    }
    if (const auto scaled = DirectParseScaledDecimal(plan.parts[1])) {
      plan.has_scaled_operand = true;
      plan.scaled_operand = *scaled;
      const auto operand_scale = DirectPow10U64(plan.scaled_operand.scale);
      const auto numerator =
          plan.output_scale_factor == 0
              ? std::nullopt
              : DirectCheckedMultiplyU64(plan.scaled_operand.value,
                                         plan.output_scale_factor);
      if (operand_scale && *operand_scale != 0 && numerator &&
          *numerator % *operand_scale == 0) {
        plan.has_exact_scaled_result_multiplier = true;
        plan.exact_scaled_result_multiplier = *numerator / *operand_scale;
      }
    }
    return plan;
  }
  if (plan.parts[0] == "mod_equals" && plan.parts.size() == 3) {
    const auto modulus = DirectParseU64(plan.parts[1]);
    const auto expected = DirectParseU64(plan.parts[2]);
    if (modulus && *modulus != 0 && expected) {
      plan.kind = DirectGeneratedProjectionKind::mod_equals;
      plan.modulus = *modulus;
      plan.expected = *expected;
      return plan;
    }
  }
  return plan;
}

std::string DirectGeneratedProjectionValue(
    const DirectGeneratedProjectionPlan& plan,
    std::uint64_t counter) {
  switch (plan.kind) {
    case DirectGeneratedProjectionKind::counter:
    {
      std::string out;
      out.reserve(20);
      DirectAppendU64(&out, counter);
      return out;
    }
    case DirectGeneratedProjectionKind::literal:
      return plan.literal_value;
    case DirectGeneratedProjectionKind::mod:
    {
      std::string out;
      out.reserve(20);
      DirectAppendU64(&out, counter % plan.modulus);
      return out;
    }
    case DirectGeneratedProjectionKind::prefix_counter: {
      std::string out;
      out.reserve(plan.prefix.size() + 20);
      out.append(plan.prefix);
      DirectAppendU64(&out, counter);
      return out;
    }
    case DirectGeneratedProjectionKind::prefix_counter_offset: {
      const auto adjusted = static_cast<long long>(counter) + plan.offset;
      if (adjusted < 0) {
        return {};
      }
      std::string out;
      out.reserve(plan.prefix.size() + 20);
      out.append(plan.prefix);
      DirectAppendU64(&out, static_cast<std::uint64_t>(adjusted));
      return out;
    }
    case DirectGeneratedProjectionKind::case_zero_literal_else_literal:
      return counter == 0 ? plan.literal_value : plan.alternate_literal_value;
    case DirectGeneratedProjectionKind::case_zero_literal_else_prefix_counter_offset: {
      if (counter == 0) {
        return plan.literal_value;
      }
      const auto adjusted = static_cast<long long>(counter) + plan.offset;
      if (adjusted < 0) {
        return {};
      }
      std::string out;
      out.reserve(plan.prefix.size() + 20);
      out.append(plan.prefix);
      DirectAppendU64(&out, static_cast<std::uint64_t>(adjusted));
      return out;
    }
    case DirectGeneratedProjectionKind::cast_divide: {
      if (plan.has_scaled_operand) {
        const auto scale_factor =
            DirectPow10U64(plan.scaled_operand.scale + plan.scale);
        if (scale_factor) {
          const auto numerator =
              DirectCheckedMultiplyU64(counter, *scale_factor);
          if (numerator) {
            const auto scaled_result =
                DirectRoundDivideU64(*numerator, plan.scaled_operand.value);
            if (scaled_result) {
              const std::string fast =
                  DirectFormatScaledDecimal(*scaled_result, plan.scale);
              if (!fast.empty()) {
                return fast;
              }
            }
          }
        }
      }
      std::ostringstream out;
      out << std::fixed << std::setprecision(plan.scale)
          << (static_cast<long double>(counter) / plan.factor);
      return out.str();
    }
    case DirectGeneratedProjectionKind::counter_multiply: {
      if (plan.has_exact_scaled_result_multiplier) {
        const auto scaled_result =
            DirectCheckedMultiplyU64(counter,
                                     plan.exact_scaled_result_multiplier);
        if (scaled_result) {
          const std::string fast = DirectFormatScaledDecimalWithFactor(
              *scaled_result,
              plan.scale,
              plan.output_scale_factor);
          if (!fast.empty()) {
            return fast;
          }
        }
      }
      if (plan.has_scaled_operand) {
        const auto output_scale = DirectPow10U64(plan.scale);
        const auto operand_scale = DirectPow10U64(plan.scaled_operand.scale);
        if (output_scale && operand_scale) {
          const auto first =
              DirectCheckedMultiplyU64(counter, plan.scaled_operand.value);
          const auto numerator =
              first ? DirectCheckedMultiplyU64(*first, *output_scale)
                    : std::nullopt;
          if (numerator) {
            const auto scaled_result =
                DirectRoundDivideU64(*numerator, *operand_scale);
            if (scaled_result) {
              const std::string fast =
                  DirectFormatScaledDecimal(*scaled_result, plan.scale);
              if (!fast.empty()) {
                return fast;
              }
            }
          }
        }
      }
      std::ostringstream out;
      out << std::fixed << std::setprecision(plan.scale)
          << (static_cast<long double>(counter) * plan.factor);
      return out.str();
    }
    case DirectGeneratedProjectionKind::mod_equals:
      return (counter % plan.modulus) == plan.expected ? "true" : "false";
    case DirectGeneratedProjectionKind::unsupported:
      break;
  }
  return {};
}

DirectGeneratedCounterPlan DirectBuildGeneratedCounterPlan(
    const DirectPhysicalBulkAppendRequest& request,
    const CrudTableRecord& table) {
  DirectGeneratedCounterPlan plan;
  plan.requested = DirectGeneratedCounterEnvelopeRequested(request);
  if (!plan.requested) {
    return plan;
  }
  const auto start = DirectOptionU64Optional(request, "insert_select_counter_start");
  const auto step = DirectOptionU64Optional(request, "insert_select_counter_step");
  const auto limit = DirectOptionU64Optional(request, "insert_select_counter_limit");
  const auto projection_count =
      DirectOptionU64Optional(request, "insert_select_projection_count");
  if (!start || !step || !limit || !projection_count || *step == 0 ||
      *projection_count == 0 || *projection_count > table.columns.size()) {
    plan.failure_reason = "insert_select_generator_descriptor_invalid";
    return plan;
  }
  const auto row_count = DirectGeneratedCounterRowCount(*start, *step, *limit);
  if (!row_count || *row_count == 0 || *row_count > 1000000ULL) {
    plan.failure_reason = "insert_select_generator_bound_refused";
    return plan;
  }
  plan.start = *start;
  plan.step = *step;
  plan.limit = *limit;
  plan.row_count = *row_count;
  plan.projections.reserve(static_cast<std::size_t>(*projection_count));
  for (std::uint64_t index = 0; index < *projection_count; ++index) {
    const std::string descriptor =
        DirectOptionValue(request,
                          "insert_select_projection_" + std::to_string(index));
    if (descriptor.empty()) {
      plan.failure_reason = "insert_select_projection_descriptor_missing";
      return plan;
    }
    const auto& column = table.columns[static_cast<std::size_t>(index)];
    auto projection =
        DirectBuildGeneratedProjectionPlan(column.first, descriptor, column.second);
    if (projection.kind == DirectGeneratedProjectionKind::unsupported) {
      plan.failure_reason = "insert_select_projection_descriptor_invalid";
      return plan;
    }
    plan.projections.push_back(std::move(projection));
  }
  plan.ok = true;
  return plan;
}

std::size_t DirectRequestRowCount(const DirectPhysicalBulkAppendRequest& request,
                                  const DirectGeneratedCounterPlan& generated) {
  if (!request.borrowed_input_rows.empty()) {
    return request.borrowed_input_rows.size();
  }
  if (generated.ok) {
    return static_cast<std::size_t>(generated.row_count);
  }
  return static_cast<std::size_t>(request.estimated_row_count);
}

struct DirectAppendIndexEntryCacheRecord {
  std::uint64_t row_version_count = 0;
  std::vector<CrudIndexEntryRecord> entries;
  std::map<std::string, std::set<std::string>> keys_by_index;
  std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>
      entry_by_index_key;
  bool entry_lookup_materialized = true;
};

struct DirectBulkAppendContextCacheRecord {
  std::uint64_t row_version_count = 0;
  std::shared_ptr<const CrudState> state;
  std::vector<CrudIndexRecord> visible_indexes;
  MgaRelationStorageDescriptor relation_descriptor;
  bool index_entries_authoritative = false;
  bool append_index_cache_hit = false;
};

std::mutex& DirectAppendIndexEntryCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, DirectAppendIndexEntryCacheRecord>&
DirectAppendIndexEntryCache() {
  static std::map<std::string, DirectAppendIndexEntryCacheRecord> cache;
  return cache;
}

std::map<std::string, DirectBulkAppendContextCacheRecord>&
DirectBulkAppendContextCache() {
  static std::map<std::string, DirectBulkAppendContextCacheRecord> cache;
  return cache;
}

std::string DirectAppendIndexEntryCacheKey(const EngineRequestContext& context,
                                           const std::string& table_uuid) {
  return context.database_path + "\n" + table_uuid;
}

std::string DirectBulkAppendContextCacheKey(const EngineRequestContext& context,
                                            const std::string& table_uuid) {
  return context.database_path + "\n" +
         std::to_string(context.local_transaction_id) + "\n" +
         context.session_uuid.canonical + "\n" +
         context.principal_uuid.canonical + "\n" +
         context.current_role_uuid.canonical + "\n" +
         std::to_string(context.catalog_generation_id) + "\n" +
         std::to_string(context.security_epoch) + "\n" +
         table_uuid;
}

std::map<std::string, std::set<std::string>> DirectBuildIndexKeyCache(
    const std::vector<CrudIndexEntryRecord>& entries) {
  std::map<std::string, std::set<std::string>> keys_by_index;
  for (const auto& entry : entries) {
    keys_by_index[entry.index_uuid].insert(entry.key_value);
  }
  return keys_by_index;
}

std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>
DirectBuildIndexEntryKeyCache(
    const std::vector<CrudIndexEntryRecord>& entries) {
  std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>
      entry_by_index_key;
  for (const auto& entry : entries) {
    entry_by_index_key[entry.index_uuid][entry.key_value] = entry;
  }
  return entry_by_index_key;
}

bool DirectLookupAppendIndexEntryCache(const EngineRequestContext& context,
                                       const std::string& table_uuid,
                                       std::uint64_t row_version_count,
                                       std::vector<CrudIndexEntryRecord>* entries,
                                       std::map<std::string, std::set<std::string>>* keys_by_index,
                                       std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>* entry_by_index_key) {
  if (entries == nullptr && keys_by_index == nullptr &&
      entry_by_index_key == nullptr) {
    return false;
  }
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectAppendIndexEntryCache().find(
      DirectAppendIndexEntryCacheKey(context, table_uuid));
  if (found == DirectAppendIndexEntryCache().end() ||
      found->second.row_version_count != row_version_count) {
    return false;
  }
  if (entries != nullptr) {
    *entries = found->second.entries;
  }
  if (keys_by_index != nullptr) {
    *keys_by_index = found->second.keys_by_index;
  }
  if (entry_by_index_key != nullptr) {
    *entry_by_index_key = found->second.entry_by_index_key;
  }
  return true;
}

bool DirectAppendIndexEntryCacheAvailable(const EngineRequestContext& context,
                                          const std::string& table_uuid,
                                          std::uint64_t row_version_count,
                                          bool require_entry_lookup = false) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectAppendIndexEntryCache().find(
      DirectAppendIndexEntryCacheKey(context, table_uuid));
  return found != DirectAppendIndexEntryCache().end() &&
         found->second.row_version_count == row_version_count &&
         (!require_entry_lookup || found->second.entry_lookup_materialized);
}

void DirectBuildAppendIndexConflictCaches(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const std::vector<CrudIndexRecord>& indexes,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch,
    std::map<std::string, std::set<std::string>>* keys_by_index,
    std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>*
        entry_by_index_key) {
  if (keys_by_index == nullptr && entry_by_index_key == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectAppendIndexEntryCache().find(
      DirectAppendIndexEntryCacheKey(context, table_uuid));
  if (found == DirectAppendIndexEntryCache().end() ||
      found->second.row_version_count != row_version_count) {
    return;
  }
  const auto& record = found->second;
  for (const auto& index : indexes) {
    if (!DirectIndexIsUnique(index)) {
      continue;
    }
    const auto cached_keys = record.keys_by_index.find(index.index_uuid);
    if (cached_keys == record.keys_by_index.end()) {
      continue;
    }
    const auto cached_entries = record.entry_by_index_key.find(index.index_uuid);
    for (const auto& values : logical_value_batch) {
      for (const auto& key : CrudIndexKeysForValues(index, values)) {
        if (cached_keys->second.count(key) == 0) {
          continue;
        }
        if (keys_by_index != nullptr) {
          (*keys_by_index)[index.index_uuid].insert(key);
        }
        if (entry_by_index_key != nullptr &&
            cached_entries != record.entry_by_index_key.end()) {
          const auto entry = cached_entries->second.find(key);
          if (entry != cached_entries->second.end()) {
            (*entry_by_index_key)[index.index_uuid][key] = entry->second;
          }
        }
      }
    }
  }
}

bool DirectLookupBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    DirectBulkAppendContextCacheRecord* record) {
  if (record == nullptr) return false;
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectBulkAppendContextCache().find(
      DirectBulkAppendContextCacheKey(context, table_uuid));
  if (found == DirectBulkAppendContextCache().end() ||
      found->second.row_version_count != row_version_count ||
      !found->second.state) {
    return false;
  }
  *record = found->second;
  return true;
}

void DirectStoreBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const CrudState& state,
    const std::vector<CrudIndexRecord>& visible_indexes,
    const MgaRelationStorageDescriptor& relation_descriptor,
    bool index_entries_authoritative,
    bool append_index_cache_hit) {
  DirectBulkAppendContextCacheRecord record;
  record.row_version_count = row_version_count;
  record.state = std::make_shared<CrudState>(state);
  record.visible_indexes = visible_indexes;
  record.relation_descriptor = relation_descriptor;
  record.index_entries_authoritative = index_entries_authoritative;
  record.append_index_cache_hit = append_index_cache_hit;
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  DirectBulkAppendContextCache()[DirectBulkAppendContextCacheKey(context,
                                                                 table_uuid)] =
      std::move(record);
}

bool DirectAdvanceBulkAppendContextCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t next_row_version_count,
    bool index_entries_authoritative,
    bool append_index_cache_hit) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  const auto found = DirectBulkAppendContextCache().find(
      DirectBulkAppendContextCacheKey(context, table_uuid));
  if (found == DirectBulkAppendContextCache().end() ||
      found->second.row_version_count != previous_row_version_count ||
      !found->second.state) {
    return false;
  }
  found->second.row_version_count = next_row_version_count;
  found->second.index_entries_authoritative = index_entries_authoritative;
  found->second.append_index_cache_hit = append_index_cache_hit;
  return true;
}

void DirectStoreAppendIndexEntryCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t row_version_count,
    const std::vector<CrudIndexEntryRecord>& entries) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  auto& record =
      DirectAppendIndexEntryCache()[DirectAppendIndexEntryCacheKey(context,
                                                                  table_uuid)];
  record.row_version_count = row_version_count;
  record.entries.clear();
  record.keys_by_index = DirectBuildIndexKeyCache(entries);
  record.entry_by_index_key = DirectBuildIndexEntryKeyCache(entries);
  record.entry_lookup_materialized = true;
}

std::vector<CrudIndexEntryRecord> DirectIndexEntriesFromExactBatches(
    const EngineRequestContext& context,
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  std::vector<CrudIndexEntryRecord> entries;
  for (const auto& batch : batches) {
    const std::string table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid
                                       : batch.index.table_uuid;
    for (const auto& exact : batch.entries) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = context.local_transaction_id;
      entry.index_uuid = batch.index.index_uuid;
      entry.table_uuid = table_uuid;
      entry.column_name = batch.index.column_name;
      entry.family = batch.index.family;
      entry.entry_kind = "exact";
      entry.key_value = exact.encoded_key;
      entry.payload_value = exact.payload_value;
      entry.row_uuid = exact.row_uuid;
      entry.version_uuid = exact.version_uuid;
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

std::vector<CrudIndexEntryRecord> DirectIndexEntriesFromRetailBatches(
    const EngineRequestContext& context,
    const std::vector<MgaIndexEntryAppendBatch>& batches) {
  std::vector<CrudIndexEntryRecord> entries;
  for (const auto& batch : batches) {
    const std::string table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid
                                       : batch.index.table_uuid;
    for (const auto& row : batch.rows) {
      for (const auto& key : CrudIndexKeysForValues(batch.index, row.values)) {
        CrudIndexEntryRecord entry;
        entry.creator_tx = context.local_transaction_id;
        entry.index_uuid = batch.index.index_uuid;
        entry.table_uuid = table_uuid;
        entry.column_name = batch.index.column_name;
        entry.family = batch.index.family;
        entry.entry_kind = "exact";
        entry.key_value = key;
        entry.payload_value = CrudFieldValue(row.values, batch.index.column_name);
        entry.row_uuid = row.row_uuid;
        entry.version_uuid = row.version_uuid;
        entries.push_back(std::move(entry));
      }
    }
  }
  return entries;
}

void DirectClearAppendIndexEntryCacheRecord(
    DirectAppendIndexEntryCacheRecord* record) {
  if (record == nullptr) { return; }
  record->entries.clear();
  record->keys_by_index.clear();
  record->entry_by_index_key.clear();
  record->entry_lookup_materialized = true;
}

void DirectAppendIndexEntryToCacheRecord(
    DirectAppendIndexEntryCacheRecord* record,
    CrudIndexEntryRecord entry,
    bool materialize_entry_lookup = true) {
  if (record == nullptr) { return; }
  record->keys_by_index[entry.index_uuid].insert(entry.key_value);
  if (materialize_entry_lookup) {
    record->entry_by_index_key[entry.index_uuid][entry.key_value] =
        std::move(entry);
  }
}

void DirectAppendIndexEntriesToCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t appended_row_count,
    const std::vector<CrudIndexEntryRecord>& appended_entries) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  auto& record =
      DirectAppendIndexEntryCache()[DirectAppendIndexEntryCacheKey(context,
                                                                  table_uuid)];
  if (record.row_version_count != previous_row_version_count) {
    DirectClearAppendIndexEntryCacheRecord(&record);
  }
  for (const auto& entry : appended_entries) {
    DirectAppendIndexEntryToCacheRecord(&record, entry);
  }
  record.row_version_count = previous_row_version_count + appended_row_count;
}

void DirectAppendIndexBatchesToCache(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::uint64_t previous_row_version_count,
    std::uint64_t appended_row_count,
    const std::vector<MgaExactIndexEntryAppendBatch>& exact_batches,
    const std::vector<MgaIndexEntryAppendBatch>& retail_batches,
    bool materialize_entry_lookup = true) {
  const std::lock_guard<std::mutex> guard(DirectAppendIndexEntryCacheMutex());
  auto& record =
      DirectAppendIndexEntryCache()[DirectAppendIndexEntryCacheKey(context,
                                                                  table_uuid)];
  if (record.row_version_count != previous_row_version_count) {
    DirectClearAppendIndexEntryCacheRecord(&record);
  }
  record.entry_lookup_materialized = materialize_entry_lookup;
  if (!materialize_entry_lookup) {
    record.entry_by_index_key.clear();
  }
  for (const auto& batch : exact_batches) {
    const std::string batch_table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid
                                       : batch.index.table_uuid;
    for (const auto& exact : batch.entries) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = context.local_transaction_id;
      entry.index_uuid = batch.index.index_uuid;
      entry.table_uuid = batch_table_uuid;
      entry.column_name = batch.index.column_name;
      entry.family = batch.index.family;
      entry.entry_kind = "exact";
      entry.key_value = exact.encoded_key;
      entry.payload_value = exact.payload_value;
      entry.row_uuid = exact.row_uuid;
      entry.version_uuid = exact.version_uuid;
      DirectAppendIndexEntryToCacheRecord(&record,
                                          std::move(entry),
                                          materialize_entry_lookup);
    }
  }
  for (const auto& batch : retail_batches) {
    const std::string batch_table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid
                                       : batch.index.table_uuid;
    for (const auto& row : batch.rows) {
      for (const auto& key : CrudIndexKeysForValues(batch.index, row.values)) {
        CrudIndexEntryRecord entry;
        entry.creator_tx = context.local_transaction_id;
        entry.index_uuid = batch.index.index_uuid;
        entry.table_uuid = batch_table_uuid;
        entry.column_name = batch.index.column_name;
        entry.family = batch.index.family;
        entry.entry_kind = "exact";
        entry.key_value = key;
        entry.payload_value = CrudFieldValue(row.values, batch.index.column_name);
        entry.row_uuid = row.row_uuid;
        entry.version_uuid = row.version_uuid;
        DirectAppendIndexEntryToCacheRecord(&record,
                                            std::move(entry),
                                            materialize_entry_lookup);
      }
    }
  }
  record.row_version_count = previous_row_version_count + appended_row_count;
}

std::uint64_t EstimateDirectPhysicalValueBytes(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::uint64_t bytes = 112;  // row header plus slot directory entry.
  for (const auto& field : values) {
    bytes += 16;  // cell header.
    bytes += static_cast<std::uint64_t>(field.second.size());
  }
  return std::max<std::uint64_t>(128, bytes);
}

std::uint64_t EstimateDirectPhysicalRowBytes(
    const CrudRowVersionRecord& row) {
  return EstimateDirectPhysicalValueBytes(row.values);
}

std::uint64_t DefaultDirectPhysicalRowsPerPage(
    const DirectPhysicalBulkAppendRequest& request,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>*
        value_batch) {
  if (staged_rows.empty()) {
    return 16;
  }
  const std::uint64_t page_size =
      DirectOptionU64(request, "physical_mga_cow.page_size_bytes", 8192);
  const std::uint64_t usable_bytes =
      page_size > 512 ? page_size - 512 : std::max<std::uint64_t>(1, page_size / 2);
  const std::size_t sample_count =
      std::min<std::size_t>(staged_rows.size(), 64);
  std::uint64_t sampled_bytes = 0;
  for (std::size_t index = 0; index < sample_count; ++index) {
    sampled_bytes += value_batch == nullptr
                         ? EstimateDirectPhysicalRowBytes(staged_rows[index])
                         : EstimateDirectPhysicalValueBytes((*value_batch)[index]);
  }
  const std::uint64_t average_row_bytes =
      std::max<std::uint64_t>(128, sampled_bytes / sample_count);
  const std::uint64_t target_bytes = (usable_bytes * 80) / 100;
  const std::uint64_t rows =
      std::max<std::uint64_t>(1, target_bytes / average_row_bytes);
  return std::clamp<std::uint64_t>(rows, 4, 256);
}

bool DirectBulkInputMatchesEncoderOrder(
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan) {
  if (row_encoder_plan.columns.empty()) {
    return true;
  }
  if (input_row.fields.size() != row_encoder_plan.columns.size()) {
    return false;
  }
  for (std::size_t index = 0; index < input_row.fields.size(); ++index) {
    if (input_row.fields[index].first !=
        row_encoder_plan.columns[index].column_name) {
      return false;
    }
  }
  return true;
}

bool DirectSharedFieldOrderMatchesEncoderOrder(
    std::span<const std::string> shared_field_order,
    const InsertRowEncoderPlan& row_encoder_plan) {
  if (shared_field_order.empty()) {
    return false;
  }
  if (row_encoder_plan.columns.empty()) {
    return true;
  }
  if (shared_field_order.size() != row_encoder_plan.columns.size()) {
    return false;
  }
  for (std::size_t index = 0; index < shared_field_order.size(); ++index) {
    if (shared_field_order[index] != row_encoder_plan.columns[index].column_name) {
      return false;
    }
  }
  return true;
}

bool DirectInputMatchesEncoderOrder(
    const DirectPhysicalBulkAppendRequest& request,
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan) {
  if (!request.shared_row_field_order.empty()) {
    return input_row.fields.size() == request.shared_row_field_order.size() &&
           DirectSharedFieldOrderMatchesEncoderOrder(request.shared_row_field_order,
                                                     row_encoder_plan);
  }
  return DirectBulkInputMatchesEncoderOrder(input_row, row_encoder_plan);
}

const std::string& DirectInputFieldName(
    const DirectPhysicalBulkAppendRequest& request,
    const EngineRowValue& input_row,
    std::size_t field_index) {
  if (field_index < request.shared_row_field_order.size() &&
      !request.shared_row_field_order[field_index].empty()) {
    return request.shared_row_field_order[field_index];
  }
  return input_row.fields[field_index].first;
}

PreparedInsertRow PrepareDirectBulkOrderedRowFast(
    const EngineRowValue& input_row,
    const BoundInsertRowTemplate& row_template,
    const std::string& row_uuid,
    bool force_large_values) {
  PreparedInsertRow row;
  row.row_uuid = row_uuid;
  row.values.reserve(input_row.fields.size());
  for (const auto& [field, typed] : input_row.fields) {
    if (typed.is_null) {
      row.values.push_back({field, kDirectNullMarker});
      row.encoded_bytes += field.size() + sizeof(kDirectNullMarker) - 1;
    } else {
      row.values.push_back({field, typed.encoded_value});
      row.encoded_bytes += field.size() + typed.encoded_value.size();
    }
  }
  row.toast_required = row.encoded_bytes > row_template.max_inline_encoded_bytes ||
                       force_large_values;
  return row;
}

PreparedInsertRow PrepareDirectBulkSharedFieldOrderRowFast(
    const DirectPhysicalBulkAppendRequest& request,
    const EngineRowValue& input_row,
    const BoundInsertRowTemplate& row_template,
    const std::string& row_uuid,
    bool force_large_values) {
  PreparedInsertRow row;
  row.row_uuid = row_uuid;
  row.values.reserve(input_row.fields.size());
  for (std::size_t field_index = 0; field_index < input_row.fields.size(); ++field_index) {
    const auto& typed = input_row.fields[field_index].second;
    const std::string& field = DirectInputFieldName(request, input_row, field_index);
    if (typed.is_null) {
      row.values.push_back({field, kDirectNullMarker});
      row.encoded_bytes += field.size() + sizeof(kDirectNullMarker) - 1;
    } else {
      row.values.push_back({field, typed.encoded_value});
      row.encoded_bytes += field.size() + typed.encoded_value.size();
    }
  }
  row.toast_required = row.encoded_bytes > row_template.max_inline_encoded_bytes ||
                       force_large_values;
  return row;
}

std::string DirectNotNullValidationFailure(
    const InsertRowEncoderPlan& row_encoder_plan,
    const std::vector<std::pair<std::string, std::string>>& values) {
  for (const auto& column : row_encoder_plan.columns) {
    if (!column.not_null_bound) {
      continue;
    }
    const auto found = std::find_if(values.begin(), values.end(), [&](const auto& value) {
      return value.first == column.column_name;
    });
    if (found == values.end() || DirectNullValue(found->second)) {
      return column.column_name;
    }
  }
  return {};
}

std::vector<std::size_t> DirectNotNullValidationOrdinals(
    const InsertRowEncoderPlan& row_encoder_plan) {
  std::vector<std::size_t> ordinals;
  ordinals.reserve(static_cast<std::size_t>(
      row_encoder_plan.not_null_validator_count));
  for (const auto& column : row_encoder_plan.columns) {
    if (column.not_null_bound) {
      ordinals.push_back(column.ordinal);
    }
  }
  return ordinals;
}

std::string DirectNotNullValidationFailureOrdered(
    const InsertRowEncoderPlan& row_encoder_plan,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::vector<std::size_t>& ordinals) {
  for (const std::size_t ordinal : ordinals) {
    if (ordinal >= values.size() || DirectNullValue(values[ordinal].second)) {
      if (ordinal < row_encoder_plan.columns.size()) {
        return row_encoder_plan.columns[ordinal].column_name;
      }
      return "ordinal:" + std::to_string(ordinal);
    }
  }
  return {};
}

bool DirectPhysicalMgaCowRequested(const DirectPhysicalBulkAppendRequest& request) {
  const std::string value = DirectOptionValue(request, "physical_mga_cow");
  if (value.empty()) {
    return true;
  }
  return !IsDirectFalsyValue(value);
}

bool DirectPhysicalMgaCowRequired(const DirectPhysicalBulkAppendRequest& request) {
  const std::string value = DirectOptionValue(request, "physical_mga_cow");
  return LowerAscii(value) == "required" ||
         IsDirectTruthyValue(DirectOptionValue(request, "physical_mga_cow.required"));
}

scratchbird::storage::page::RowDataCell DirectPhysicalCell(
    std::uint16_t ordinal,
    const std::string& encoded_value) {
  scratchbird::storage::page::RowDataCell cell;
  cell.column_ordinal = ordinal;
  cell.value.type_id = scratchbird::core::datatypes::CanonicalTypeId::character;
  cell.value.payload.assign(encoded_value.begin(), encoded_value.end());
  return cell;
}

void DirectAppendLittleUnsigned(std::vector<scratchbird::core::platform::byte>* out,
                                std::uint64_t value,
                                std::size_t bytes) {
  out->reserve(out->size() + bytes);
  for (std::size_t index = 0; index < bytes; ++index) {
    out->push_back(static_cast<scratchbird::core::platform::byte>(
        (value >> (index * 8)) & 0xffu));
  }
}

using DirectU128Bytes = std::array<scratchbird::core::platform::byte, 16>;

void DirectAppendLittleUnsigned128(std::vector<scratchbird::core::platform::byte>* out,
                                   const DirectU128Bytes& value) {
  out->reserve(out->size() + 16);
  out->insert(out->end(), value.begin(), value.end());
}

void DirectAppendLittleSigned(std::vector<scratchbird::core::platform::byte>* out,
                              std::int64_t value,
                              std::size_t bytes) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(value));
  DirectAppendLittleUnsigned(out, bits, bytes);
}

int DirectCompareU128Bytes(const DirectU128Bytes& left,
                           const DirectU128Bytes& right) {
  for (std::size_t reverse = left.size(); reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    if (left[index] < right[index]) { return -1; }
    if (left[index] > right[index]) { return 1; }
  }
  return 0;
}

DirectU128Bytes DirectMaxU128Bytes() {
  DirectU128Bytes value{};
  value.fill(0xff);
  return value;
}

DirectU128Bytes DirectMaxPositiveI128Bytes() {
  DirectU128Bytes value = DirectMaxU128Bytes();
  value.back() = 0x7f;
  return value;
}

DirectU128Bytes DirectMinNegativeI128MagnitudeBytes() {
  DirectU128Bytes value{};
  value.back() = 0x80;
  return value;
}

bool DirectParseUnsigned128Text(std::string_view text,
                                const DirectU128Bytes& max_value,
                                DirectU128Bytes* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  DirectU128Bytes parsed{};
  for (const char ch : text) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    unsigned carry = static_cast<unsigned>(ch - '0');
    for (std::size_t index = 0; index < parsed.size(); ++index) {
      const unsigned product =
          static_cast<unsigned>(parsed[index]) * 10u + carry;
      parsed[index] = static_cast<scratchbird::core::platform::byte>(product & 0xffu);
      carry = product >> 8;
    }
    if (carry != 0 || DirectCompareU128Bytes(parsed, max_value) > 0) {
      return false;
    }
  }
  *out = parsed;
  return true;
}

DirectU128Bytes DirectTwosComplementNegativeMagnitude(DirectU128Bytes magnitude) {
  for (auto& byte : magnitude) {
    byte = static_cast<scratchbird::core::platform::byte>(~byte);
  }
  unsigned carry = 1;
  for (auto& byte : magnitude) {
    const unsigned sum = static_cast<unsigned>(byte) + carry;
    byte = static_cast<scratchbird::core::platform::byte>(sum & 0xffu);
    carry = sum >> 8;
    if (carry == 0) {
      break;
    }
  }
  return magnitude;
}

bool DirectParseSigned128Payload(std::string_view text,
                                 std::vector<scratchbird::core::platform::byte>* out) {
  if (text.empty()) {
    return false;
  }
  const bool negative = text.front() == '-';
  if (negative) {
    text.remove_prefix(1);
    if (text.empty()) {
      return false;
    }
  } else if (text.front() == '+') {
    text.remove_prefix(1);
    if (text.empty()) {
      return false;
    }
  }
  const DirectU128Bytes positive_limit = DirectMaxPositiveI128Bytes();
  const DirectU128Bytes negative_limit = DirectMinNegativeI128MagnitudeBytes();
  DirectU128Bytes magnitude{};
  if (!DirectParseUnsigned128Text(text,
                                  negative ? negative_limit : positive_limit,
                                  &magnitude)) {
    return false;
  }
  const bool nonzero =
      std::any_of(magnitude.begin(), magnitude.end(),
                  [](scratchbird::core::platform::byte byte) { return byte != 0; });
  const DirectU128Bytes bits =
      negative && nonzero ? DirectTwosComplementNegativeMagnitude(magnitude)
                          : magnitude;
  DirectAppendLittleUnsigned128(out, bits);
  return true;
}

bool DirectParseUnsigned128Payload(std::string_view text,
                                   std::vector<scratchbird::core::platform::byte>* out) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  DirectU128Bytes parsed{};
  if (!DirectParseUnsigned128Text(text, DirectMaxU128Bytes(), &parsed)) {
    return false;
  }
  DirectAppendLittleUnsigned128(out, parsed);
  return true;
}

bool DirectParseFixedDigits(std::string_view text,
                            std::size_t offset,
                            std::size_t count,
                            int* out) {
  if (out == nullptr || offset + count > text.size()) {
    return false;
  }
  int parsed = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char ch = text[offset + index];
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    parsed = parsed * 10 + (ch - '0');
  }
  *out = parsed;
  return true;
}

bool DirectLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DirectDaysInMonth(int year, int month) {
  switch (month) {
    case 1: return 31;
    case 2: return DirectLeapYear(year) ? 29 : 28;
    case 3: return 31;
    case 4: return 30;
    case 5: return 31;
    case 6: return 30;
    case 7: return 31;
    case 8: return 31;
    case 9: return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 0;
  }
}

bool DirectParseDateParts(std::string_view text, int* year, int* month, int* day) {
  int parsed_year = 0;
  int parsed_month = 0;
  int parsed_day = 0;
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return false;
  }
  if (!DirectParseFixedDigits(text, 0, 4, &parsed_year) ||
      !DirectParseFixedDigits(text, 5, 2, &parsed_month) ||
      !DirectParseFixedDigits(text, 8, 2, &parsed_day)) {
    return false;
  }
  if (parsed_year < 1 || parsed_month < 1 || parsed_month > 12 ||
      parsed_day < 1 || parsed_day > DirectDaysInMonth(parsed_year, parsed_month)) {
    return false;
  }
  if (year != nullptr) { *year = parsed_year; }
  if (month != nullptr) { *month = parsed_month; }
  if (day != nullptr) { *day = parsed_day; }
  return true;
}

std::int64_t DirectDaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(day_of_era) - 719468;
}

bool DirectParseFractionNanos(std::string_view text,
                              std::size_t* offset,
                              std::uint32_t* nanos) {
  if (offset == nullptr || nanos == nullptr) {
    return false;
  }
  *nanos = 0;
  if (*offset >= text.size() || text[*offset] != '.') {
    return true;
  }
  ++(*offset);
  const std::size_t start = *offset;
  std::uint32_t parsed = 0;
  while (*offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*offset]))) {
    if (*offset - start >= 9) {
      return false;
    }
    parsed = parsed * 10 + static_cast<std::uint32_t>(text[*offset] - '0');
    ++(*offset);
  }
  const std::size_t digits = *offset - start;
  if (digits == 0) {
    return false;
  }
  for (std::size_t index = digits; index < 9; ++index) {
    parsed *= 10;
  }
  *nanos = parsed;
  return true;
}

bool DirectParseTimeParts(std::string_view text,
                          std::uint64_t* nanos_since_midnight,
                          std::uint32_t* nanos_of_second = nullptr) {
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (text.size() < 8 || text[2] != ':' || text[5] != ':') {
    return false;
  }
  if (!DirectParseFixedDigits(text, 0, 2, &hour) ||
      !DirectParseFixedDigits(text, 3, 2, &minute) ||
      !DirectParseFixedDigits(text, 6, 2, &second)) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    return false;
  }
  std::size_t offset = 8;
  std::uint32_t nanos = 0;
  if (!DirectParseFractionNanos(text, &offset, &nanos) || offset != text.size()) {
    return false;
  }
  if (nanos_since_midnight != nullptr) {
    const std::uint64_t seconds =
        static_cast<std::uint64_t>(hour) * 3600ull +
        static_cast<std::uint64_t>(minute) * 60ull +
        static_cast<std::uint64_t>(second);
    *nanos_since_midnight = seconds * 1000000000ull + nanos;
  }
  if (nanos_of_second != nullptr) {
    *nanos_of_second = nanos;
  }
  return true;
}

bool DirectParseDatePayload(std::string_view text,
                            std::vector<scratchbird::core::platform::byte>* out) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (!DirectParseDateParts(text, &year, &month, &day)) {
    return false;
  }
  const std::int64_t days = DirectDaysFromCivil(
      year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  if (days < std::numeric_limits<std::int32_t>::min() ||
      days > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  DirectAppendLittleSigned(out, days, 4);
  return true;
}

bool DirectParseTimePayload(std::string_view text,
                            std::vector<scratchbird::core::platform::byte>* out) {
  std::uint64_t nanos = 0;
  if (!DirectParseTimeParts(text, &nanos)) {
    return false;
  }
  DirectAppendLittleUnsigned(out, nanos, 8);
  return true;
}

bool DirectParseTimezoneOffsetMinutes(std::string_view text,
                                      std::size_t* end,
                                      int* offset_minutes) {
  if (end == nullptr || offset_minutes == nullptr) {
    return false;
  }
  *offset_minutes = 0;
  if (*end == 0) {
    return true;
  }
  if (text[*end - 1] == 'Z' || text[*end - 1] == 'z') {
    --(*end);
    return true;
  }
  if (*end < 6) {
    return true;
  }
  const std::size_t offset = *end - 6;
  if ((text[offset] != '+' && text[offset] != '-') || text[offset + 3] != ':') {
    return true;
  }
  int hour = 0;
  int minute = 0;
  if (!DirectParseFixedDigits(text, offset + 1, 2, &hour) ||
      !DirectParseFixedDigits(text, offset + 4, 2, &minute) ||
      hour > 23 || minute > 59) {
    return false;
  }
  *offset_minutes = (hour * 60 + minute) * (text[offset] == '-' ? -1 : 1);
  *end = offset;
  return true;
}

bool DirectParseTimestampPayload(std::string_view text,
                                 std::vector<scratchbird::core::platform::byte>* out) {
  std::size_t end = text.size();
  int offset_minutes = 0;
  if (!DirectParseTimezoneOffsetMinutes(text, &end, &offset_minutes)) {
    return false;
  }
  const std::string_view local = text.substr(0, end);
  const std::size_t separator = local.find('T') == std::string_view::npos
                                    ? local.find(' ')
                                    : local.find('T');
  if (separator == std::string_view::npos) {
    return false;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  if (!DirectParseDateParts(local.substr(0, separator), &year, &month, &day)) {
    return false;
  }
  std::uint64_t nanos_since_midnight = 0;
  std::uint32_t nanos_of_second = 0;
  if (!DirectParseTimeParts(local.substr(separator + 1),
                            &nanos_since_midnight,
                            &nanos_of_second)) {
    return false;
  }
  const std::int64_t days = DirectDaysFromCivil(
      year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const std::int64_t seconds_since_midnight =
      static_cast<std::int64_t>(nanos_since_midnight / 1000000000ull);
  const std::int64_t epoch_seconds =
      days * 86400 + seconds_since_midnight -
      static_cast<std::int64_t>(offset_minutes) * 60;
  DirectAppendLittleSigned(out, epoch_seconds, 8);
  DirectAppendLittleUnsigned(out, nanos_of_second, 4);
  DirectAppendLittleSigned(out, 0, 4);
  return true;
}

bool DirectParseI64Text(std::string_view text, std::int64_t* out) {
  if (out == nullptr) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parse = std::from_chars(begin, end, *out, 10);
  return parse.ec == std::errc{} && parse.ptr == end;
}

bool DirectParseIntervalPayload(std::string_view text,
                                std::vector<scratchbird::core::platform::byte>* out) {
  std::int64_t seconds = 0;
  if (DirectParseI64Text(text, &seconds)) {
    if (seconds > std::numeric_limits<std::int64_t>::max() / 1000000000ll ||
        seconds < std::numeric_limits<std::int64_t>::min() / 1000000000ll) {
      return false;
    }
    DirectAppendLittleSigned(out, 0, 4);
    DirectAppendLittleSigned(out, 0, 4);
    DirectAppendLittleSigned(out, seconds * 1000000000ll, 8);
    return true;
  }
  if (text.empty() || text[0] != 'P') {
    return false;
  }
  std::size_t offset = 1;
  bool in_time = false;
  std::int64_t months = 0;
  std::int64_t days = 0;
  std::int64_t nanos = 0;
  bool consumed = false;
  while (offset < text.size()) {
    if (text[offset] == 'T') {
      if (in_time) {
        return false;
      }
      in_time = true;
      ++offset;
      continue;
    }
    const std::size_t number_start = offset;
    while (offset < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[offset]))) {
      ++offset;
    }
    if (number_start == offset || offset >= text.size()) {
      return false;
    }
    std::int64_t value = 0;
    if (!DirectParseI64Text(text.substr(number_start, offset - number_start), &value)) {
      return false;
    }
    const char designator = text[offset++];
    consumed = true;
    switch (designator) {
      case 'Y':
        if (in_time || value > std::numeric_limits<std::int64_t>::max() / 12) {
          return false;
        }
        months += value * 12;
        break;
      case 'M':
        if (in_time) {
          if (value > std::numeric_limits<std::int64_t>::max() / 60 / 1000000000ll) {
            return false;
          }
          nanos += value * 60 * 1000000000ll;
        } else {
          months += value;
        }
        break;
      case 'D':
        if (in_time) {
          return false;
        }
        days += value;
        break;
      case 'H':
        if (!in_time ||
            value > std::numeric_limits<std::int64_t>::max() / 3600 / 1000000000ll) {
          return false;
        }
        nanos += value * 3600 * 1000000000ll;
        break;
      case 'S':
        if (!in_time ||
            value > std::numeric_limits<std::int64_t>::max() / 1000000000ll) {
          return false;
        }
        nanos += value * 1000000000ll;
        break;
      default:
        return false;
    }
  }
  if (!consumed ||
      months < std::numeric_limits<std::int32_t>::min() ||
      months > std::numeric_limits<std::int32_t>::max() ||
      days < std::numeric_limits<std::int32_t>::min() ||
      days > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  DirectAppendLittleSigned(out, months, 4);
  DirectAppendLittleSigned(out, days, 4);
  DirectAppendLittleSigned(out, nanos, 8);
  return true;
}

bool DirectParseSignedIntegerPayload(std::string_view text,
                                     std::size_t bytes,
                                     std::vector<scratchbird::core::platform::byte>* out) {
  if (bytes == 16) {
    return DirectParseSigned128Payload(text, out);
  }
  std::int64_t parsed = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parse = std::from_chars(begin, end, parsed, 10);
  if (parse.ec != std::errc{} || parse.ptr != end) {
    return false;
  }
  switch (bytes) {
    case 1:
      if (parsed < std::numeric_limits<std::int8_t>::min() ||
          parsed > std::numeric_limits<std::int8_t>::max()) {
        return false;
      }
      break;
    case 2:
      if (parsed < std::numeric_limits<std::int16_t>::min() ||
          parsed > std::numeric_limits<std::int16_t>::max()) {
        return false;
      }
      break;
    case 4:
      if (parsed < std::numeric_limits<std::int32_t>::min() ||
          parsed > std::numeric_limits<std::int32_t>::max()) {
        return false;
      }
      break;
    case 8:
      break;
    default:
      return false;
  }
  std::uint64_t bits = 0;
  std::memcpy(&bits, &parsed, sizeof(parsed));
  DirectAppendLittleUnsigned(out, bits, bytes);
  return true;
}

bool DirectParseUnsignedIntegerPayload(std::string_view text,
                                       std::size_t bytes,
                                       std::vector<scratchbird::core::platform::byte>* out) {
  if (bytes == 16) {
    return DirectParseUnsigned128Payload(text, out);
  }
  std::uint64_t parsed = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parse = std::from_chars(begin, end, parsed, 10);
  if (parse.ec != std::errc{} || parse.ptr != end) {
    return false;
  }
  switch (bytes) {
    case 1:
      if (parsed > std::numeric_limits<std::uint8_t>::max()) { return false; }
      break;
    case 2:
      if (parsed > std::numeric_limits<std::uint16_t>::max()) { return false; }
      break;
    case 4:
      if (parsed > std::numeric_limits<std::uint32_t>::max()) { return false; }
      break;
    case 8:
      break;
    default:
      return false;
  }
  DirectAppendLittleUnsigned(out, parsed, bytes);
  return true;
}

bool DirectParseBooleanPayload(std::string_view text,
                               std::vector<scratchbird::core::platform::byte>* out) {
  std::string normalized(text);
  for (char& c : normalized) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (normalized == "1" || normalized == "true") {
    out->push_back(1);
    return true;
  }
  if (normalized == "0" || normalized == "false") {
    out->push_back(0);
    return true;
  }
  return false;
}

bool DirectParseRealPayload(dt::CanonicalTypeId type_id,
                            std::string_view text,
                            std::vector<scratchbird::core::platform::byte>* out) {
  if (type_id == dt::CanonicalTypeId::bfloat16 ||
      type_id == dt::CanonicalTypeId::real16) {
    float parsed = 0.0f;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parse = std::from_chars(begin, end, parsed);
    if (parse.ec != std::errc{} || parse.ptr != end) { return false; }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &parsed, sizeof(parsed));
    std::uint16_t packed = 0;
    if (type_id == dt::CanonicalTypeId::bfloat16) {
      const std::uint32_t lsb = (bits >> 16) & 1u;
      packed = static_cast<std::uint16_t>((bits + 0x7fffu + lsb) >> 16);
    } else {
      const std::uint32_t sign = (bits >> 16) & 0x8000u;
      int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
      std::uint32_t mantissa = bits & 0x7fffffu;
      if (exponent <= 0) {
        if (exponent < -10) {
          packed = static_cast<std::uint16_t>(sign);
        } else {
          mantissa = (mantissa | 0x800000u) >> static_cast<unsigned>(1 - exponent);
          packed = static_cast<std::uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
        }
      } else if (exponent >= 31) {
        packed = static_cast<std::uint16_t>(sign | 0x7c00u);
      } else {
        packed = static_cast<std::uint16_t>(
            sign | (static_cast<std::uint32_t>(exponent) << 10) |
            ((mantissa + 0x1000u) >> 13));
      }
    }
    DirectAppendLittleUnsigned(out, packed, sizeof(packed));
    return true;
  }
  if (type_id == dt::CanonicalTypeId::real32) {
    float parsed = 0.0f;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parse = std::from_chars(begin, end, parsed);
    if (parse.ec != std::errc{} || parse.ptr != end) { return false; }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &parsed, sizeof(parsed));
    DirectAppendLittleUnsigned(out, bits, sizeof(bits));
    return true;
  }
  if (type_id == dt::CanonicalTypeId::real64) {
    double parsed = 0.0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parse = std::from_chars(begin, end, parsed);
    if (parse.ec != std::errc{} || parse.ptr != end) { return false; }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &parsed, sizeof(parsed));
    DirectAppendLittleUnsigned(out, bits, sizeof(bits));
    return true;
  }
  return false;
}

bool DirectParseUuidPayload(std::string_view text,
                            std::vector<scratchbird::core::platform::byte>* out) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok()) { return false; }
  out->assign(parsed.value.bytes.begin(), parsed.value.bytes.end());
  return true;
}

bool DirectPackTypedPayload(dt::CanonicalTypeId target_type,
                            const EngineTypedValue& typed,
                            std::vector<scratchbird::core::platform::byte>* out) {
  const auto layout = dt::LookupDatatypeStorageLayout(target_type);
  const std::size_t inline_bytes =
      layout.ok() ? static_cast<std::size_t>(layout.layout.inline_bytes) : 0;
  if (!typed.binary_value.empty() &&
      (inline_bytes == 0 || typed.binary_value.size() == inline_bytes)) {
    *out = typed.binary_value;
    return true;
  }
  switch (target_type) {
    case dt::CanonicalTypeId::character:
      out->assign(typed.encoded_value.begin(), typed.encoded_value.end());
      return true;
    case dt::CanonicalTypeId::boolean:
      return DirectParseBooleanPayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::int8:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 1, out);
    case dt::CanonicalTypeId::int16:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 2, out);
    case dt::CanonicalTypeId::int32:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 4, out);
    case dt::CanonicalTypeId::int64:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 8, out);
    case dt::CanonicalTypeId::int128:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 16, out);
    case dt::CanonicalTypeId::uint8:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 1, out);
    case dt::CanonicalTypeId::uint16:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 2, out);
    case dt::CanonicalTypeId::uint32:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 4, out);
    case dt::CanonicalTypeId::uint64:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 8, out);
    case dt::CanonicalTypeId::uint128:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 16, out);
    case dt::CanonicalTypeId::bfloat16:
    case dt::CanonicalTypeId::real16:
    case dt::CanonicalTypeId::real32:
    case dt::CanonicalTypeId::real64:
      return DirectParseRealPayload(target_type, typed.encoded_value, out);
    case dt::CanonicalTypeId::uuid:
    case dt::CanonicalTypeId::enum_value:
      return DirectParseUuidPayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::date:
      return DirectParseDatePayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::time:
      return DirectParseTimePayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::timestamp:
      return DirectParseTimestampPayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::interval:
      return DirectParseIntervalPayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::binary:
      out->assign(typed.encoded_value.begin(), typed.encoded_value.end());
      return true;
    default:
      if (target_type != dt::CanonicalTypeId::unknown &&
          target_type != dt::CanonicalTypeId::character &&
          layout.ok() &&
          layout.layout.storage_class != dt::DatatypeStorageClass::inline_fixed) {
        out->assign(typed.encoded_value.begin(), typed.encoded_value.end());
        return true;
      }
      return false;
  }
}

scratchbird::storage::page::RowDataCell DirectPhysicalCellFromTypedValue(
    std::uint16_t ordinal,
    const EngineTypedValue& typed,
    std::string_view target_canonical_type_name) {
  scratchbird::storage::page::RowDataCell cell;
  cell.column_ordinal = ordinal;
  if (typed.isSqlNull()) {
    cell.value.type_id = dt::CanonicalTypeId::null_type;
    cell.value.is_null = true;
    return cell;
  }
  dt::CanonicalTypeId target_type =
      dt::CanonicalTypeIdFromStableName(std::string(target_canonical_type_name));
  if (target_type == dt::CanonicalTypeId::unknown) {
    target_type = dt::CanonicalTypeIdFromStableName(
        typed.descriptor.canonical_type_name);
  }
  if (target_type != dt::CanonicalTypeId::unknown &&
      target_type != dt::CanonicalTypeId::character) {
    std::vector<scratchbird::core::platform::byte> payload;
    if (DirectPackTypedPayload(target_type, typed, &payload)) {
      cell.value.type_id = target_type;
      cell.value.payload = std::move(payload);
      return cell;
    }
    const auto layout = dt::LookupDatatypeStorageLayout(target_type);
    if (layout.ok() &&
        layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed) {
      cell.value.type_id = target_type;
      return cell;
    }
  }
  cell.value.type_id = dt::CanonicalTypeId::character;
  cell.value.payload.assign(typed.encoded_value.begin(),
                            typed.encoded_value.end());
  return cell;
}

std::vector<scratchbird::storage::page::RowDataCell> DirectPhysicalCells(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<scratchbird::storage::page::RowDataCell> cells;
  cells.reserve(values.size());
  std::uint16_t ordinal = 1;
  for (const auto& value : values) {
    cells.push_back(DirectPhysicalCell(ordinal++, value.second));
  }
  return cells;
}

struct DirectPhysicalTypedCells {
  std::vector<scratchbird::storage::page::RowDataCell> cells;
  std::uint64_t typed_binary_cells = 0;
  std::uint64_t int64_cells = 0;
  std::uint64_t null_cells = 0;
  std::uint64_t character_cells = 0;
  std::map<dt::CanonicalTypeId, std::uint64_t> typed_cell_counts;
};

DirectPhysicalTypedCells DirectPhysicalCellsFromTypedInputRow(
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan) {
  DirectPhysicalTypedCells result;
  result.cells.reserve(input_row.fields.size());
  std::uint16_t ordinal = 1;
  for (std::size_t field_index = 0; field_index < input_row.fields.size(); ++field_index) {
    const auto& field = input_row.fields[field_index];
    const auto& typed = field.second;
    const std::string_view target_type =
        field_index < row_encoder_plan.columns.size()
            ? std::string_view(row_encoder_plan.columns[field_index].canonical_type_name)
            : std::string_view{};
    const dt::CanonicalTypeId target_type_id =
        dt::CanonicalTypeIdFromStableName(std::string(target_type));
    auto cell = DirectPhysicalCellFromTypedValue(ordinal++, typed, target_type);
    if (cell.value.is_null) {
      ++result.null_cells;
    } else if (cell.value.type_id == dt::CanonicalTypeId::int64) {
      ++result.int64_cells;
    } else if (cell.value.type_id == dt::CanonicalTypeId::character &&
               target_type_id == dt::CanonicalTypeId::character) {
      ++result.character_cells;
    }
    if (!cell.value.is_null &&
        cell.value.type_id != dt::CanonicalTypeId::character) {
      ++result.typed_binary_cells;
      ++result.typed_cell_counts[cell.value.type_id];
    }
    result.cells.push_back(std::move(cell));
  }
  return result;
}

std::string DirectFixedWidthTypedPayloadFailure(
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan) {
  if (input_row.fields.size() != row_encoder_plan.columns.size()) {
    return "typed_row_shape_mismatch";
  }
  for (std::size_t field_index = 0; field_index < input_row.fields.size(); ++field_index) {
    const auto& typed = input_row.fields[field_index].second;
    if (typed.isSqlNull()) {
      continue;
    }
    const auto& column = row_encoder_plan.columns[field_index];
    const dt::CanonicalTypeId target_type =
        dt::CanonicalTypeIdFromStableName(column.canonical_type_name);
    if (target_type == dt::CanonicalTypeId::unknown ||
        target_type == dt::CanonicalTypeId::character) {
      continue;
    }
    const auto layout = dt::LookupDatatypeStorageLayout(target_type);
    if (!layout.ok() ||
        layout.layout.storage_class != dt::DatatypeStorageClass::inline_fixed) {
      continue;
    }
    std::vector<scratchbird::core::platform::byte> payload;
    if (!DirectPackTypedPayload(target_type, typed, &payload)) {
      return "typed_fixed_payload_invalid:" + column.column_name + ":" +
             std::string(dt::CanonicalTypeName(target_type));
    }
  }
  return {};
}

struct DirectPhysicalMgaCowWriteResult {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
  std::uint64_t written_rows = 0;
};

DirectPhysicalMgaCowWriteResult WriteDirectPhysicalMgaCowRows(
    const DirectPhysicalBulkAppendRequest& request,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>*
        value_batch = nullptr,
    bool engine_generated_unique_insert_rows = false,
    const InsertRowEncoderPlan* row_encoder_plan = nullptr) {
  DirectPhysicalMgaCowWriteResult result;
  if (!DirectPhysicalMgaCowRequested(request)) {
    result.evidence.push_back({"direct_physical_bulk_row_page_writer", "disabled"});
    return result;
  }
  if (value_batch != nullptr && value_batch->size() != staged_rows.size()) {
    result.ok = false;
    result.diagnostic =
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "physical_mga_cow_value_batch_shape_invalid");
    return result;
  }

  const TypedUuid relation_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  const TypedUuid transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction,
                           request.context.transaction_uuid.canonical);
  if (!relation_uuid.valid() || !transaction_uuid.valid() ||
      request.context.local_transaction_id == 0) {
    result.ok = false;
    result.diagnostic =
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "physical_mga_cow_authority_missing");
    return result;
  }

  const std::uint64_t base_page =
      DirectOptionU64(request, "physical_mga_cow.page_number", 1024);
  const std::uint64_t requested_rows_per_page =
      DirectOptionU64(request, "physical_mga_cow.rows_per_page", 0);
  const std::uint64_t rows_per_page =
      requested_rows_per_page == 0
          ? DefaultDirectPhysicalRowsPerPage(request, staged_rows, value_batch)
          : requested_rows_per_page;
  const std::uint64_t row_offset =
      DirectOptionU64(request, "physical_mga_cow.row_offset", 0);
  const bool use_typed_input_rows =
      request.borrowed_input_rows.size() == staged_rows.size() &&
      !request.shared_row_field_order.empty() &&
      row_encoder_plan != nullptr &&
      row_encoder_plan->columns.size() == request.shared_row_field_order.size() &&
      DirectSharedFieldOrderMatchesEncoderOrder(request.shared_row_field_order,
                                                *row_encoder_plan);
  std::uint64_t typed_int64_cells = 0;
  std::uint64_t typed_binary_cells = 0;
  std::uint64_t typed_null_cells = 0;
  std::uint64_t typed_character_cells = 0;
  std::map<dt::CanonicalTypeId, std::uint64_t> typed_cell_counts;
  scratchbird::storage::database::PhysicalMgaCowMutationBatchRequest batch;
  batch.mutations.reserve(staged_rows.size());
  batch.sync_after_batch = false;
  batch.engine_generated_unique_insert_rows =
      engine_generated_unique_insert_rows;
  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    const auto& row = staged_rows[index];
    const std::uint64_t absolute_index =
        row_offset + static_cast<std::uint64_t>(index);
    scratchbird::storage::database::PhysicalMgaCowMutationRequest cow;
    cow.database_path = request.context.database_path;
    cow.relation_uuid = relation_uuid;
    cow.row_uuid = ParseDirectTypedUuid(UuidKind::row, row.row_uuid);
    cow.transaction_uuid = transaction_uuid;
    cow.existing_local_transaction_id =
        scratchbird::transaction::mga::MakeLocalTransactionId(
            request.context.local_transaction_id);
    cow.use_existing_transaction = true;
    cow.kind = scratchbird::storage::database::PhysicalMgaCowMutationKind::insert;
    cow.page_number =
        base_page + (absolute_index / std::max<std::uint64_t>(1, rows_per_page));
    cow.begin_unix_epoch_millis = 0;
    cow.stable_slot_id = static_cast<std::uint32_t>(absolute_index + 1);
    if (use_typed_input_rows) {
      auto typed_cells =
          DirectPhysicalCellsFromTypedInputRow(request.borrowed_input_rows[index],
                                              *row_encoder_plan);
      typed_int64_cells += typed_cells.int64_cells;
      typed_binary_cells += typed_cells.typed_binary_cells;
      typed_null_cells += typed_cells.null_cells;
      typed_character_cells += typed_cells.character_cells;
      for (const auto& [type_id, count] : typed_cells.typed_cell_counts) {
        typed_cell_counts[type_id] += count;
      }
      cow.cells = std::move(typed_cells.cells);
    } else {
      cow.cells = DirectPhysicalCells(value_batch == nullptr
                                          ? row.values
                                          : (*value_batch)[index]);
    }
    batch.mutations.push_back(std::move(cow));
  }
  const auto written =
      scratchbird::storage::database::WritePhysicalMgaCowUnpublishedMutationBatch(
          std::move(batch));
  if (!written.ok()) {
    result.ok = false;
    result.diagnostic = MakeEngineApiDiagnostic(
        written.diagnostic.diagnostic_code.empty()
            ? "SB-IPAR-PHYSICAL-MGA-COW-WRITE-FAILED"
            : written.diagnostic.diagnostic_code,
        written.diagnostic.message_key.empty()
            ? "dml.direct_physical_bulk.physical_mga_cow_failed"
            : written.diagnostic.message_key,
        "batch_rows=" + std::to_string(staged_rows.size()),
        true);
    return result;
  }
  result.written_rows = written.written_rows;
  for (const auto& item : written.evidence) {
    result.evidence.push_back({"direct_physical_bulk_row_page_evidence", item});
  }
  result.evidence.push_back({"direct_physical_bulk_row_page_writer",
                             "physical_mga_cow"});
  result.evidence.push_back({"direct_physical_bulk_row_page_writer",
                             "physical_mga_cow_batch"});
  result.evidence.push_back({"direct_physical_bulk_row_page_written_rows",
                             std::to_string(result.written_rows)});
  result.evidence.push_back({"direct_physical_bulk_row_page_typed_input",
                             use_typed_input_rows ? "true" : "false"});
  result.evidence.push_back({"direct_physical_bulk_row_page_int64_cells",
                             std::to_string(typed_int64_cells)});
  result.evidence.push_back({"direct_physical_bulk_row_page_typed_binary_cells",
                             std::to_string(typed_binary_cells)});
  for (const auto& [type_id, count] : typed_cell_counts) {
    result.evidence.push_back(
        {"direct_physical_bulk_row_page_typed_cells." +
             std::string(dt::CanonicalTypeName(type_id)),
         std::to_string(count)});
  }
  result.evidence.push_back({"direct_physical_bulk_row_page_null_cells",
                             std::to_string(typed_null_cells)});
  result.evidence.push_back({"direct_physical_bulk_row_page_character_cells",
                             std::to_string(typed_character_cells)});
  result.evidence.push_back({"direct_physical_bulk_row_page_pages_written",
                             std::to_string(written.pages_written)});
  result.evidence.push_back({"direct_physical_bulk_row_page_finality_authority",
                             "false"});
  result.evidence.push_back({"direct_physical_bulk_row_page_visibility_authority",
                             "durable_transaction_inventory"});
  return result;
}

bool DirectOrderedIngestRequested(const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary = DirectOptionValue(request, "ordered_ingest");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string odf = DirectOptionValue(request, "odf047.ordered_ingest");
  if (!odf.empty()) {
    return IsDirectTruthyValue(odf);
  }
  return false;
}

bool DirectOrderedIngestDeriveForLargeLoad(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary =
      DirectOptionValue(request, "ordered_ingest.derive_for_large_load");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string odf =
      DirectOptionValue(request, "odf047.derive_order_for_large_load");
  if (!odf.empty()) {
    return IsDirectTruthyValue(odf);
  }
  return false;
}

std::string DirectOrderedPlacementKeyColumn(
    const DirectPhysicalBulkAppendRequest& request) {
  std::string key = DirectOptionValue(request, "ordered_ingest.placement_key");
  if (!key.empty()) {
    return key;
  }
  key = DirectOptionValue(request, "placement_key");
  if (!key.empty()) {
    return key;
  }
  return DirectOptionValue(request, "odf047.placement_key");
}

bool DirectPhysicalClusteringRequested(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary = DirectOptionValue(request, "physical_clustering");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string enabled =
      DirectOptionValue(request, "physical_clustering.enabled");
  if (!enabled.empty()) {
    return IsDirectTruthyValue(enabled);
  }
  return false;
}

std::string DirectPhysicalClusteringKeyColumn(
    const DirectPhysicalBulkAppendRequest& request,
    const std::string& placement_key_column) {
  std::string key = DirectOptionValue(request, "physical_clustering.key");
  if (!key.empty()) {
    return key;
  }
  key = DirectOptionValue(request, "physical_clustering.placement_key");
  if (!key.empty()) {
    return key;
  }
  return placement_key_column;
}

std::string DirectValueForColumn(
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& column_name) {
  for (const auto& [name, value] : values) {
    if (name == column_name) {
      return value;
    }
  }
  return {};
}

template <typename T>
std::vector<T> ApplySourceOrdinalPermutation(
    const std::vector<T>& source,
    const std::vector<std::uint64_t>& ordinals) {
  std::vector<T> reordered;
  reordered.reserve(source.size());
  for (const auto ordinal : ordinals) {
    if (ordinal < source.size()) {
      reordered.push_back(source[static_cast<std::size_t>(ordinal)]);
    }
  }
  return reordered;
}

struct DirectOrderedIngestSelection {
  bool ok = true;
  bool selected = false;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<EngineEvidenceReference> evidence;
};

DirectOrderedIngestSelection ApplyDirectOrderedIngestPlan(
    const DirectPhysicalBulkAppendRequest& request,
    std::vector<CrudRowVersionRecord>* staged_rows,
    std::vector<std::vector<std::pair<std::string, std::string>>>* logical_value_batch) {
  DirectOrderedIngestSelection selection;
  if (staged_rows == nullptr || logical_value_batch == nullptr ||
      staged_rows->size() != logical_value_batch->size()) {
    selection.ok = false;
    selection.failure_reason = "ordered_ingest_batch_shape_invalid";
    selection.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.direct_physical_bulk_append",
        selection.failure_reason);
    return selection;
  }

  const bool ordered_ingest_requested = DirectOrderedIngestRequested(request);
  const bool derive_for_large_load =
      DirectOrderedIngestDeriveForLargeLoad(request);
  const std::uint64_t large_load_threshold =
      DirectOptionU64(request, "ordered_ingest.large_load_threshold", 1024);
  const bool physical_clustering_requested =
      DirectPhysicalClusteringRequested(request);
  const bool large_load =
      large_load_threshold != 0 &&
      staged_rows->size() >= large_load_threshold;
  if (!ordered_ingest_requested &&
      !(derive_for_large_load && large_load) &&
      !physical_clustering_requested) {
    selection.evidence.push_back({"bulk_placement_order_planner",
                                  "engine_optimizer"});
    selection.evidence.push_back({"bulk_placement_order_requested", "false"});
    selection.evidence.push_back(
        {"bulk_placement_order_large_load_threshold",
         std::to_string(large_load_threshold)});
    selection.evidence.push_back({"bulk_placement_order_input_rows",
                                  std::to_string(staged_rows->size())});
    selection.evidence.push_back({"bulk_placement_order_selected", "false"});
    selection.evidence.push_back({"ordered_ingest_storage_policy",
                                  "storage_page"});
    selection.evidence.push_back({"ordered_ingest_selected", "false"});
    selection.evidence.push_back(
        {"ordered_ingest_physical_clustering_requested", "false"});
    selection.evidence.push_back(
        {"ordered_ingest_physical_clustering",
         "not_requested_descriptor_unchanged"});
    return selection;
  }

  const std::string placement_key_column =
      DirectOrderedPlacementKeyColumn(request);
  scratchbird::engine::optimizer::BulkPlacementOrderRequest plan_request;
  plan_request.ordered_ingest_requested = ordered_ingest_requested;
  plan_request.derive_for_large_load = derive_for_large_load;
  plan_request.large_load_row_threshold = large_load_threshold;
  plan_request.placement_key_column = placement_key_column;
  plan_request.rows.reserve(staged_rows->size());
  for (std::size_t index = 0; index < staged_rows->size(); ++index) {
    scratchbird::engine::optimizer::BulkPlacementOrderRow row;
    row.source_ordinal = static_cast<std::uint64_t>(index);
    row.row_uuid = (*staged_rows)[index].row_uuid;
    row.placement_key =
        DirectValueForColumn((*logical_value_batch)[index], placement_key_column);
    plan_request.rows.push_back(std::move(row));
  }

  const auto plan =
      scratchbird::engine::optimizer::PlanBulkPlacementOrder(plan_request);
  for (const auto& item : plan.evidence) {
    selection.evidence.push_back({item.first, item.second});
  }
  if (!plan.ok) {
    selection.ok = false;
    selection.failure_reason = plan.diagnostic_code.empty()
                                   ? "ordered_ingest_refused"
                                   : plan.diagnostic_code;
    selection.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.direct_physical_bulk_append",
        selection.failure_reason);
    return selection;
  }

  scratchbird::storage::page::OrderedIngestPhysicalClusteringRequest clustering;
  clustering.current_descriptor.relation_uuid =
      request.target_table.uuid.canonical;
  clustering.current_descriptor.placement_key_column =
      DirectOptionValue(request, "physical_clustering.current_key");
  clustering.current_descriptor.policy_uuid =
      DirectOptionValue(request, "physical_clustering.current_policy_uuid");
  clustering.current_descriptor.descriptor_generation =
      DirectOptionU64(request, "physical_clustering.current_generation", 0);
  clustering.current_descriptor.physical_clustering_enabled =
      !clustering.current_descriptor.placement_key_column.empty();
  clustering.requested_placement_key_column =
      DirectPhysicalClusteringKeyColumn(request, placement_key_column);
  clustering.requested_policy_uuid =
      DirectOptionValue(request, "physical_clustering.policy_uuid");
  clustering.ordered_ingest_selected = plan.ordered_ingest_selected;
  clustering.physical_clustering_requested = physical_clustering_requested;
  clustering.explicit_policy_present =
      DirectOptionEnabled(request, "physical_clustering.policy=explicit") ||
      !clustering.requested_policy_uuid.empty();
  clustering.allow_clustering_key_change =
      IsDirectTruthyValue(DirectOptionValue(request,
                                            "physical_clustering.allow_key_change"));
  const auto clustering_result =
      scratchbird::storage::page::ResolveOrderedIngestPhysicalClustering(
          clustering);
  for (const auto& item : clustering_result.evidence) {
    selection.evidence.push_back({item.first, item.second});
  }
  if (!clustering_result.ok) {
    selection.ok = false;
    selection.failure_reason = clustering_result.diagnostic_detail.empty()
                                   ? "physical_clustering_policy_refused"
                                   : clustering_result.diagnostic_detail;
    selection.diagnostic = MakeEngineApiDiagnostic(
        clustering_result.diagnostic_code.empty()
            ? "SB_ENGINE_API_INVALID_REQUEST"
            : clustering_result.diagnostic_code,
        "storage.ordered_ingest.physical_clustering_refused",
        selection.failure_reason,
        true);
    return selection;
  }

  selection.selected = plan.ordered_ingest_selected;
  if (plan.ordered_ingest_selected &&
      plan.source_ordinals_in_apply_order.size() == staged_rows->size()) {
    *staged_rows = ApplySourceOrdinalPermutation(
        *staged_rows,
        plan.source_ordinals_in_apply_order);
    *logical_value_batch = ApplySourceOrdinalPermutation(
        *logical_value_batch,
        plan.source_ordinals_in_apply_order);
    selection.evidence.push_back({"ordered_ingest_apply_order",
                                  "placement_key"});
    selection.evidence.push_back({"ordered_ingest_applied_rows",
                                  std::to_string(staged_rows->size())});
  }
  return selection;
}

EngineInsertRowsRequest SyntheticInsertRequestForDirectBulk(
    const DirectPhysicalBulkAppendRequest& request) {
  EngineInsertRowsRequest insert;
  insert.context = request.context;
  insert.operation_id = "dml.direct_physical_bulk_append";
  insert.target_table = request.target_table;
  insert.borrowed_input_rows = request.borrowed_input_rows;
  insert.require_generated_row_uuid = request.require_generated_row_uuid;
  insert.estimated_row_count = request.estimated_row_count == 0
                                   ? static_cast<EngineApiU64>(
                                         request.borrowed_input_rows.size())
                                   : request.estimated_row_count;
  if (request.lane_operation == "native_bulk") {
    insert.insert_mode = "native_bulk";
  } else if (request.lane_operation == "copy_import") {
    insert.insert_mode = "copy_import";
  } else if (request.lane_operation == "insert_select") {
    insert.insert_mode = "insert_select";
  } else {
    insert.insert_mode = "multi_values";
  }
  insert.duplicate_mode = request.duplicate_mode;
  insert.strict_bulk_load_requested = request.strict_bulk_load_requested;
  insert.option_envelopes = request.option_envelopes;
  insert.diagnostic_options = request.diagnostic_options;
  return insert;
}

void AddDirectLaneBaseEvidence(const DirectPhysicalBulkAppendRequest& request,
                               std::size_t row_count,
                               DirectPhysicalBulkAppendResult* result) {
  result->evidence.push_back({"direct_physical_bulk_lane", "direct_physical"});
  result->evidence.push_back({"direct_physical_bulk_operation", request.lane_operation});
  result->evidence.push_back({"direct_physical_bulk_delegate", "none"});
  result->evidence.push_back({"direct_physical_bulk_rows",
                              std::to_string(row_count)});
  result->evidence.push_back({"direct_physical_bulk_batch_source",
                              request.borrowed_input_rows.empty()
                                  ? "generated_counter_projection_rows"
                                  : "borrowed_binary_typed_rows"});
  result->evidence.push_back({"direct_physical_bulk_batch_consumed_by",
                              "engine.dml.direct_physical_bulk_append"});
  result->evidence.push_back({"parser_finality_authority", "false"});
  result->evidence.push_back({"reference_finality_authority", "false"});
  result->evidence.push_back({"mga_finality_authority", "engine_transaction_inventory"});
}

DirectPhysicalBulkAppendResult DirectBulkFailure(
    const DirectPhysicalBulkAppendRequest& request,
    EngineApiDiagnostic diagnostic,
    std::string reason,
    EngineDmlSummaryCounters summary = {}) {
  const std::string fallback_reason = reason;
  DirectPhysicalBulkAppendResult result;
  result.ok = false;
  result.operation_id = "dml.direct_physical_bulk_append";
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.diagnostics.push_back(std::move(diagnostic));
  result.dml_summary = std::move(summary);
  AddEmbeddedTrustModeEvidence(request.context, &result);
  AddDirectLaneBaseEvidence(
      request,
      request.borrowed_input_rows.empty()
          ? static_cast<std::size_t>(request.estimated_row_count)
          : request.borrowed_input_rows.size(),
      &result);
  result.evidence.push_back({"direct_physical_bulk_refused", std::move(reason)});
  result.evidence.push_back({"direct_physical_bulk_fail_closed", "true"});
  AddDmlSummaryFallbackReason(&result.dml_summary, fallback_reason);
  AddDmlSummaryEvidence(&result);
  return result;
}

DirectPhysicalBulkAppendResult DirectBulkFailureWithEvidence(
    const DirectPhysicalBulkAppendRequest& request,
    EngineApiDiagnostic diagnostic,
    std::string reason,
    const std::vector<EngineEvidenceReference>& evidence,
    EngineDmlSummaryCounters summary = {}) {
  auto result = DirectBulkFailure(request, std::move(diagnostic), std::move(reason), std::move(summary));
  result.evidence.insert(result.evidence.end(), evidence.begin(), evidence.end());
  return result;
}

void AddHotAppendCounterEvidence(const MgaRelationHotAppendCounters& counters,
                                 DirectPhysicalBulkAppendResult* result) {
  result->evidence.push_back({"mga_hot_append_allocator_stream_opens",
                              std::to_string(counters.allocator_stream_opens)});
  result->evidence.push_back({"mga_hot_append_allocator_stream_flushes",
                              std::to_string(counters.allocator_stream_flushes)});
  result->evidence.push_back({"mga_hot_append_allocator_records",
                              std::to_string(counters.allocator_range_records_appended)});
  result->evidence.push_back({"mga_hot_append_row_stream_opens",
                              std::to_string(counters.row_stream_opens)});
  result->evidence.push_back({"mga_hot_append_row_stream_flushes",
                              std::to_string(counters.row_stream_flushes)});
  result->evidence.push_back({"mga_hot_append_scoped_row_stream_opens",
                              std::to_string(counters.scoped_row_stream_opens)});
  result->evidence.push_back({"mga_hot_append_scoped_row_stream_flushes",
                              std::to_string(counters.scoped_row_stream_flushes)});
  result->evidence.push_back({"mga_hot_append_scoped_row_write_batches",
                              std::to_string(counters.scoped_row_write_batches)});
  result->evidence.push_back({"mga_hot_append_scoped_row_write_tickets_issued",
                              std::to_string(counters.scoped_row_write_tickets_issued)});
  result->evidence.push_back({"mga_hot_append_scoped_row_write_tickets_completed",
                              std::to_string(counters.scoped_row_write_tickets_completed)});
  result->evidence.push_back({"mga_hot_append_scoped_row_write_worker_count",
                              std::to_string(counters.scoped_row_write_worker_count)});
  result->evidence.push_back({"mga_hot_append_row_range_reservations",
                              std::to_string(counters.row_range_reservations)});
  result->evidence.push_back({"mga_hot_append_row_versions",
                              std::to_string(counters.row_versions_appended)});
  result->evidence.push_back({"mga_hot_append_index_stream_opens",
                              std::to_string(counters.index_stream_opens)});
  result->evidence.push_back({"mga_hot_append_index_stream_flushes",
                              std::to_string(counters.index_stream_flushes)});
  result->evidence.push_back({"mga_hot_append_scoped_index_stream_opens",
                              std::to_string(counters.scoped_index_stream_opens)});
  result->evidence.push_back({"mga_hot_append_scoped_index_stream_flushes",
                              std::to_string(counters.scoped_index_stream_flushes)});
  result->evidence.push_back({"mga_hot_append_scoped_index_write_batches",
                              std::to_string(counters.scoped_index_write_batches)});
  result->evidence.push_back({"mga_hot_append_scoped_index_write_tickets_issued",
                              std::to_string(counters.scoped_index_write_tickets_issued)});
  result->evidence.push_back({"mga_hot_append_scoped_index_write_tickets_completed",
                              std::to_string(counters.scoped_index_write_tickets_completed)});
  result->evidence.push_back({"mga_hot_append_scoped_index_write_worker_count",
                              std::to_string(counters.scoped_index_write_worker_count)});
  result->evidence.push_back({"mga_hot_append_index_range_reservations",
                              std::to_string(counters.index_range_reservations)});
  result->evidence.push_back({"mga_hot_append_index_entries",
                              std::to_string(counters.index_entries_appended)});
  result->evidence.push_back({"mga_hot_append_index_materialization_jobs_queued",
                              std::to_string(counters.index_materialization_jobs_queued)});
  result->evidence.push_back({"mga_hot_append_index_materialization_jobs_completed",
                              std::to_string(counters.index_materialization_jobs_completed)});
  result->evidence.push_back({"mga_hot_append_index_materialization_inline_jobs",
                              std::to_string(counters.index_materialization_inline_jobs)});
  result->evidence.push_back({"mga_hot_append_index_materialization_worker_count",
                              std::to_string(counters.index_materialization_worker_count)});
  result->evidence.push_back({"mga_hot_append_index_materialization_sort_batches",
                              std::to_string(counters.index_materialization_sort_batches)});
  result->evidence.push_back({"mga_hot_append_index_materialized_entries",
                              std::to_string(counters.index_materialized_entries)});
  result->evidence.push_back({"mga_hot_append_index_materialization_commit_barrier",
                              counters.index_materialization_jobs_queued ==
                                      counters.index_materialization_jobs_completed
                                  ? "flush_waited"
                                  : "incomplete"});
}

bool HasEvidence(const std::vector<EngineEvidenceReference>& evidence,
                 const std::string& kind,
                 const std::string& id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

std::string FirstEvidenceId(const std::vector<EngineEvidenceReference>& evidence,
                            const std::string& kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return item.evidence_id;
    }
  }
  return {};
}

std::string RequiredPreallocationFailureReason(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family) {
  if (allocation.preallocation_capped ||
      HasEvidence(allocation.evidence, "dml_demand_hint_decision", "capped")) {
    return family + "_page_extent_preallocation_cap_exceeded";
  }
  if (allocation.preallocation_refused ||
      HasEvidence(allocation.evidence, "dml_demand_runtime_outcome", "capacity_refused") ||
      HasEvidence(allocation.evidence, "dml_demand_runtime_outcome", "capacity_request_refused") ||
      HasEvidence(allocation.evidence, "dml_demand_runtime_outcome", "not_accepted")) {
    return family + "_page_extent_preallocation_refused";
  }
  if (!allocation.active) {
    return family + "_page_extent_preallocation_authority_missing";
  }
  const std::string source_kind =
      family == "index" ? "index_page_allocation_source" : "row_page_allocation_source";
  if (!HasEvidence(allocation.evidence,
                   source_kind,
                   "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT")) {
    return family + "_page_extent_preallocation_not_granted";
  }
  return {};
}

void AddPreallocationRuntimeCounters(const DmlPageAllocationRuntimeResult& allocation,
                                     EngineDmlSummaryCounters* summary) {
  if (summary == nullptr) {
    return;
  }
  summary->preallocation_requests += allocation.preallocation_requested ? 1 : 0;
  summary->preallocation_granted_pages += allocation.granted_preallocation_pages;
  summary->preallocation_capped += allocation.preallocation_capped ? 1 : 0;
  summary->preallocation_refused += allocation.preallocation_refused ? 1 : 0;
}

EngineApiU64 DirectElapsedMicros(DirectSteadyClock::time_point start,
                                 DirectSteadyClock::time_point finish) {
  return static_cast<EngineApiU64>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

void AddDirectBulkPhaseEvidence(
    const std::vector<std::pair<std::string, EngineApiU64>>& phase_micros,
    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  EngineApiU64 total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    result->evidence.push_back(
        {"direct_physical_bulk_phase_us." + phase,
         std::to_string(micros)});
  }
  result->evidence.push_back({"direct_physical_bulk_phase_us.total",
                              std::to_string(total)});
}

void WriteDirectBulkPhaseTrace(
    const DirectPhysicalBulkAppendRequest& request,
    const DirectPhysicalBulkAppendResult& result,
    const std::vector<std::pair<std::string, EngineApiU64>>& phase_micros) {
  const char* trace_path = std::getenv("SCRATCHBIRD_DML_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "operation=dml.direct_physical_bulk_append"
      << "\ttable=" << request.target_table.uuid.canonical
      << "\trows=" << result.inserted_rows
      << "\taccepted=" << result.accepted_rows
      << "\ttx=" << request.context.local_transaction_id;
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == "direct_physical_bulk_append_context_cache" ||
        evidence.evidence_kind == "direct_physical_append_index_cache" ||
        evidence.evidence_kind == "direct_physical_append_index_cache_bypass" ||
        evidence.evidence_kind == "direct_physical_bulk_index_entries_authoritative" ||
        evidence.evidence_kind == "mga_relation_index_only_row_versions" ||
        evidence.evidence_kind == "mga_relation_index_only_eligible" ||
        evidence.evidence_kind == "mga_relation_index_only_reason" ||
        evidence.evidence_kind == "relation_state_full_loads" ||
        evidence.evidence_kind == "relation_state_scoped_loads" ||
        evidence.evidence_kind == "relation_state_load_reason" ||
        evidence.evidence_kind == "mga_hot_append_index_entries" ||
        evidence.evidence_kind == "mga_hot_append_index_range_reservations" ||
        evidence.evidence_kind == "mga_hot_append_scoped_index_stream_opens" ||
        evidence.evidence_kind == "mga_hot_append_scoped_index_stream_flushes" ||
        evidence.evidence_kind == "mga_hot_append_scoped_index_write_batches" ||
        evidence.evidence_kind == "mga_hot_append_scoped_index_write_tickets_issued" ||
        evidence.evidence_kind == "mga_hot_append_scoped_index_write_tickets_completed" ||
        evidence.evidence_kind == "mga_hot_append_scoped_index_write_worker_count") {
      out << '\t' << evidence.evidence_kind << '=' << evidence.evidence_id;
    } else if (evidence.evidence_kind.rfind(
                   "direct_physical_bulk_trace.",
                   0) == 0) {
      out << '\t'
          << evidence.evidence_kind.substr(
                 std::string_view("direct_physical_bulk_trace.").size())
          << '=' << evidence.evidence_id;
    }
  }
  for (const auto& [phase, micros] : phase_micros) {
    out << '\t' << phase << "_us=" << micros;
  }
  out << '\n';
}

std::string DirectPreallocationOutcome(
    const DmlPageAllocationRuntimeResult& allocation) {
  if (!allocation.active) {
    return "runtime_inactive";
  }
  if (!allocation.preallocation_requested) {
    return "reservation_only";
  }
  if (allocation.preallocation_refused) {
    return "preallocation_refused";
  }
  if (allocation.preallocation_capped) {
    return allocation.preallocation_granted ? "preallocation_capped"
                                            : "preallocation_cap_refused";
  }
  if (allocation.preallocation_granted ||
      allocation.granted_preallocation_pages != 0) {
    return "preallocated";
  }
  return "reservation_only";
}

std::string DirectPreallocationFallbackReason(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family) {
  if (!allocation.active) {
    return family + "_page_allocation_runtime_inactive";
  }
  if (!allocation.preallocation_requested) {
    return {};
  }
  if (allocation.preallocation_refused) {
    return family + "_page_preallocation_refused";
  }
  if (allocation.preallocation_capped && !allocation.preallocation_granted) {
    return family + "_page_preallocation_cap_refused";
  }
  if (!allocation.preallocation_granted &&
      allocation.granted_preallocation_pages == 0) {
    return family + "_page_preallocation_reservation_only";
  }
  return {};
}

EngineApiU64 DirectAllocationEvidenceU64(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& kind) {
  for (const auto& item : allocation.evidence) {
    if (item.evidence_kind != kind) {
      continue;
    }
    std::istringstream in(item.evidence_id);
    EngineApiU64 value = 0;
    in >> value;
    return in.fail() ? 0 : value;
  }
  return 0;
}

EngineApiU64 DirectFilespaceGrowthPages(
    const DmlPageAllocationRuntimeResult& allocation) {
  return DirectAllocationEvidenceU64(
      allocation,
      "filespace_runtime_capacity_window_materialized");
}

void AddDirectAllocationResourceSummary(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family,
    EngineApiU64 row_count,
    EngineApiU64 elapsed_microseconds,
    const InsertBatchContext& batch_context,
    bool update_summary,
    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  if (allocation.active && update_summary) {
    if (family == "row") {
      result->dml_summary.row_extent_reservations += row_count;
      result->dml_summary.version_extent_reservations += row_count;
      result->dml_summary.page_extent_reservations += allocation.requested_pages;
    } else if (family == "index") {
      result->dml_summary.index_extent_reservations += allocation.requested_pages;
    }
    AddPreallocationRuntimeCounters(allocation, &result->dml_summary);
  }
  result->evidence.push_back({family + "_page_allocation_runtime",
                              allocation.active ? "active" : "inactive"});
  result->evidence.push_back({family + "_page_reservation_requested_pages",
                              std::to_string(allocation.requested_pages)});
  result->evidence.push_back({family + "_page_preallocation_requested",
                              allocation.preallocation_requested ? "true" : "false"});
  result->evidence.push_back({family + "_page_preallocation_granted_pages",
                              std::to_string(allocation.granted_preallocation_pages)});
  result->evidence.push_back({family + "_page_preallocation_outcome",
                              DirectPreallocationOutcome(allocation)});
  result->evidence.push_back({family + "_page_preallocation_claim",
                              allocation.granted_preallocation_pages != 0
                                  ? "physical_preallocated_pages"
                                  : "reservation_or_no_runtime_only"});
  const std::string fallback =
      DirectPreallocationFallbackReason(allocation, family);
  if (!fallback.empty()) {
    result->evidence.push_back({family + "_page_preallocation_degraded_reason",
                                fallback});
  }
  const EngineApiU64 growth_pages = DirectFilespaceGrowthPages(allocation);
  const EngineApiU64 growth_agent_pages =
      DirectAllocationEvidenceU64(allocation, "filespace_agent_granted_pages");
  result->evidence.push_back({family + "_filespace_growth_pages",
                              std::to_string(growth_pages)});
  if (growth_agent_pages != 0 || growth_pages != 0) {
    result->evidence.push_back({family + "_filespace_growth_agent_granted_pages",
                                std::to_string(growth_agent_pages)});
  }
  result->evidence.push_back({family + "_filespace_growth_claim",
                              growth_pages != 0
                                  ? "capacity_window_materialized"
                                  : (allocation.active ? "not_materialized"
                                                       : "runtime_inactive")});
  result->evidence.push_back({family + "_allocation_stall_microseconds",
                              std::to_string(elapsed_microseconds)});
  if (allocation.active && allocation.granted_preallocation_pages != 0) {
    (void)scratchbird::core::metrics::RecordInsertPreallocatedPages(
        static_cast<double>(allocation.granted_preallocation_pages),
        batch_context.target_object_uuid,
        InsertBatchModeName(batch_context.insert_mode),
        family,
        DirectPreallocationOutcome(allocation),
        fallback.empty() ? "none" : fallback);
  }
  RecordInsertBatchMetric(batch_context,
                          "sb_dml_insert_allocation_stall_microseconds",
                          static_cast<double>(elapsed_microseconds),
                          allocation.active ? "ok" : "inactive",
                          family + "_page_allocation");
  if (growth_pages != 0) {
    RecordInsertBatchMetric(batch_context,
                            "sb_filespace_insert_growth_request_total",
                            static_cast<double>(growth_pages),
                            "capacity_window_materialized",
                            family + "_filespace_growth");
    RecordInsertBatchMetric(batch_context,
                            "sb_filespace_insert_growth_wait_microseconds",
                            static_cast<double>(elapsed_microseconds),
                            "ok",
                            family + "_filespace_growth");
  }
  if (!fallback.empty() && allocation.preallocation_requested) {
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_slow_path_total",
                            1.0,
                            "resource_degraded",
                            fallback);
  }
}

void AddRequiredPreallocationSummary(
    const DmlPageAllocationRuntimeResult& row_allocation,
    const DmlPageAllocationRuntimeResult& index_allocation,
    std::size_t row_count,
    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  result->dml_summary.row_extent_reservations += static_cast<EngineApiU64>(row_count);
  result->dml_summary.version_extent_reservations += static_cast<EngineApiU64>(row_count);
  result->dml_summary.page_extent_reservations += row_allocation.requested_pages;
  result->dml_summary.index_extent_reservations += index_allocation.requested_pages;
  AddPreallocationRuntimeCounters(row_allocation, &result->dml_summary);
  AddPreallocationRuntimeCounters(index_allocation, &result->dml_summary);

  const std::string row_allocation_id =
      FirstEvidenceId(row_allocation.evidence, "row_page_allocation");
  const std::string index_allocation_id =
      FirstEvidenceId(index_allocation.evidence, "index_page_allocation");
  result->evidence.push_back({"row_extent_reservation_count", std::to_string(row_count)});
  result->evidence.push_back({"version_extent_reservation_count", std::to_string(row_count)});
  result->evidence.push_back({"page_extent_reservation_count",
                              std::to_string(row_allocation.requested_pages)});
  result->evidence.push_back({"index_extent_reservation_count",
                              std::to_string(index_allocation.requested_pages)});
  if (!row_allocation_id.empty()) {
    result->evidence.push_back({"row_extent_reservation_id", row_allocation_id});
    result->evidence.push_back({"version_extent_reservation_id",
                                row_allocation_id + ":versions"});
    result->evidence.push_back({"page_extent_reservation_id", row_allocation_id});
    result->evidence.push_back({"dml_summary.row_extent_reservation_id",
                                row_allocation_id});
    result->evidence.push_back({"dml_summary.version_extent_reservation_id",
                                row_allocation_id + ":versions"});
    result->evidence.push_back({"dml_summary.page_extent_reservation_id",
                                row_allocation_id});
  }
  if (!index_allocation_id.empty()) {
    result->evidence.push_back({"index_extent_reservation_id", index_allocation_id});
    result->evidence.push_back({"dml_summary.index_extent_reservation_id",
                                index_allocation_id});
  }
  result->evidence.push_back({"page_extent_preallocation_requested", "true"});
  result->evidence.push_back({"page_extent_preallocation_granted",
                              result->dml_summary.preallocation_granted_pages != 0
                                  ? "true"
                                  : "false"});
  result->evidence.push_back({"page_extent_preallocation_capped",
                              result->dml_summary.preallocation_capped != 0
                                  ? "true"
                                  : "false"});
  result->evidence.push_back({"page_extent_preallocation_refused",
                              result->dml_summary.preallocation_refused != 0
                                  ? "true"
                                  : "false"});
  result->evidence.push_back({"filespace_page_agent_handoff",
                              "filespace_capacity_manager->page_allocation_manager"});
  result->evidence.push_back({"page_extent_reservation_before_physical_append", "true"});
}

struct DirectBulkUuidBatch {
  std::vector<std::string> row_uuids;
  std::vector<std::string> version_uuids;
  std::size_t generated_row_uuids = 0;
  std::size_t caller_row_uuids = 0;
  std::size_t reservoir_served_uuids = 0;
  std::size_t reservoir_sync_generated_uuids = 0;
  bool reservoir_async_refill_requested = false;
  std::string batch_evidence_id;
};

std::uint64_t DirectUuidUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::uint64_t DirectUuidMix64(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

std::uint64_t DirectUuidReservoirSalt() {
  static const std::uint64_t salt = DirectUuidMix64(
      DirectUuidUnixMillis() ^
      static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(&DirectUuidReservoirSalt)));
  return salt;
}

std::string DirectFormatUuidBytes(const std::array<unsigned char, 16>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      out.push_back('-');
    }
    const auto value = bytes[index];
    out.push_back(kHex[(value >> 4) & 0x0f]);
    out.push_back(kHex[value & 0x0f]);
  }
  return out;
}

std::string DirectFastUuidV7Text(std::uint64_t sequence,
                                 std::uint64_t unix_epoch_millis) {
  const std::uint64_t millis =
      unix_epoch_millis & 0x0000ffffffffffffull;
  const std::uint64_t mixed_a =
      DirectUuidMix64(sequence ^ DirectUuidReservoirSalt());
  const std::uint64_t mixed_b =
      DirectUuidMix64(sequence + 0xd1b54a32d192ed03ull);
  std::array<unsigned char, 16> bytes{};
  bytes[0] = static_cast<unsigned char>((millis >> 40) & 0xffu);
  bytes[1] = static_cast<unsigned char>((millis >> 32) & 0xffu);
  bytes[2] = static_cast<unsigned char>((millis >> 24) & 0xffu);
  bytes[3] = static_cast<unsigned char>((millis >> 16) & 0xffu);
  bytes[4] = static_cast<unsigned char>((millis >> 8) & 0xffu);
  bytes[5] = static_cast<unsigned char>(millis & 0xffu);
  bytes[6] = static_cast<unsigned char>(0x70u | ((mixed_a >> 8) & 0x0fu));
  bytes[7] = static_cast<unsigned char>(mixed_a & 0xffu);
  bytes[8] = static_cast<unsigned char>(0x80u | ((mixed_a >> 56) & 0x3fu));
  bytes[9] = static_cast<unsigned char>((mixed_a >> 48) & 0xffu);
  bytes[10] = static_cast<unsigned char>((mixed_a >> 40) & 0xffu);
  bytes[11] = static_cast<unsigned char>((mixed_b >> 32) & 0xffu);
  bytes[12] = static_cast<unsigned char>((mixed_b >> 24) & 0xffu);
  bytes[13] = static_cast<unsigned char>((mixed_b >> 16) & 0xffu);
  bytes[14] = static_cast<unsigned char>((mixed_b >> 8) & 0xffu);
  bytes[15] = static_cast<unsigned char>(mixed_b & 0xffu);
  return DirectFormatUuidBytes(bytes);
}

class DirectUuidReservoir {
 public:
  struct AcquireStats {
    std::size_t served_from_reservoir = 0;
    std::size_t synchronously_generated = 0;
    bool async_refill_requested = false;
  };

  std::vector<std::string> Acquire(std::size_t count, AcquireStats* stats) {
    std::vector<std::string> out;
    out.reserve(count);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      while (!pool_.empty() && out.size() < count) {
        out.push_back(std::move(pool_.front()));
        pool_.pop_front();
      }
    }
    if (stats != nullptr) {
      stats->served_from_reservoir += out.size();
    }
    const std::size_t missing = count - out.size();
    const std::uint64_t millis = DirectUuidUnixMillis();
    for (std::size_t index = 0; index < missing; ++index) {
      out.push_back(GenerateOne(millis));
    }
    if (stats != nullptr) {
      stats->synchronously_generated += missing;
    }
    const bool refill_requested =
        count <= kAsyncRefillTriggerRows && MaybeStartRefill();
    if (stats != nullptr) {
      stats->async_refill_requested = refill_requested;
    }
    return out;
  }

 private:
  static constexpr std::size_t kLowWatermark = 65536;
  static constexpr std::size_t kAsyncRefillTriggerRows = 4096;
  static constexpr std::size_t kRefillBatch = 131072;
  static constexpr std::size_t kMaxPool = 262144;

  std::string GenerateOne(std::uint64_t unix_epoch_millis) {
    const std::uint64_t sequence =
        next_sequence_.fetch_add(1, std::memory_order_relaxed);
    return DirectFastUuidV7Text(sequence, unix_epoch_millis);
  }

  bool MaybeStartRefill() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pool_.size() >= kLowWatermark) {
        return false;
      }
    }
    bool expected = false;
    if (!refill_running_.compare_exchange_strong(expected,
                                                 true,
                                                 std::memory_order_acq_rel)) {
      return false;
    }
    std::thread([this]() {
      std::vector<std::string> generated;
      generated.reserve(kRefillBatch);
      const std::uint64_t millis = DirectUuidUnixMillis();
      for (std::size_t index = 0; index < kRefillBatch; ++index) {
        generated.push_back(GenerateOne(millis));
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& uuid : generated) {
          if (pool_.size() >= kMaxPool) {
            break;
          }
          pool_.push_back(std::move(uuid));
        }
      }
      refill_running_.store(false, std::memory_order_release);
      (void)MaybeStartRefill();
    }).detach();
    return true;
  }

  std::mutex mutex_;
  std::deque<std::string> pool_;
  std::atomic<std::uint64_t> next_sequence_{
      DirectUuidMix64(DirectUuidReservoirSalt())};
  std::atomic<bool> refill_running_{false};
};

DirectUuidReservoir& DirectBulkUuidReservoir() {
  static auto* reservoir = new DirectUuidReservoir();
  return *reservoir;
}

DirectBulkUuidBatch BuildDirectBulkUuidBatch(
    const DirectPhysicalBulkAppendRequest& request,
    std::size_t row_count) {
  DirectBulkUuidBatch batch;
  batch.row_uuids.reserve(row_count);
  batch.version_uuids.reserve(row_count);
  batch.batch_evidence_id =
      "direct-bulk-uuid-batch:" + request.context.request_id + ":" +
      std::to_string(row_count);
  std::size_t generated_row_count = 0;
  for (std::size_t index = 0; index < row_count; ++index) {
    const bool caller_uuid_available =
        index < request.borrowed_input_rows.size() &&
        !request.borrowed_input_rows[index].requested_row_uuid.canonical.empty();
    if (!caller_uuid_available) {
      ++generated_row_count;
    }
  }
  DirectUuidReservoir::AcquireStats acquire_stats;
  std::vector<std::string> generated_uuids =
      DirectBulkUuidReservoir().Acquire(generated_row_count + row_count,
                                        &acquire_stats);
  batch.reservoir_served_uuids = acquire_stats.served_from_reservoir;
  batch.reservoir_sync_generated_uuids = acquire_stats.synchronously_generated;
  batch.reservoir_async_refill_requested = acquire_stats.async_refill_requested;
  std::size_t generated_index = 0;
  for (std::size_t index = 0; index < row_count; ++index) {
    const bool caller_uuid_available =
        index < request.borrowed_input_rows.size() &&
        !request.borrowed_input_rows[index].requested_row_uuid.canonical.empty();
    if (!caller_uuid_available) {
      ++batch.generated_row_uuids;
      batch.row_uuids.push_back(std::move(generated_uuids[generated_index++]));
    } else {
      ++batch.caller_row_uuids;
      batch.row_uuids.push_back(
          request.borrowed_input_rows[index].requested_row_uuid.canonical);
    }
    batch.version_uuids.push_back(std::move(generated_uuids[generated_index++]));
  }
  return batch;
}

void AddDirectBulkUuidBatchEvidence(const DirectBulkUuidBatch& batch,
                                    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back({"direct_bulk_uuid_generation_mode", "batched"});
  result->evidence.push_back({"direct_bulk_uuid_batch", batch.batch_evidence_id});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_row_capacity",
       std::to_string(batch.row_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_version_capacity",
       std::to_string(batch.version_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_generated_row_uuids",
       std::to_string(batch.generated_row_uuids)});
  result->evidence.push_back(
      {"direct_bulk_caller_row_uuids",
       std::to_string(batch.caller_row_uuids)});
  result->evidence.push_back(
      {"direct_bulk_version_uuid_generation_mode", "batched"});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_served",
       std::to_string(batch.reservoir_served_uuids)});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_sync_generated",
       std::to_string(batch.reservoir_sync_generated_uuids)});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_async_refill_requested",
       batch.reservoir_async_refill_requested ? "true" : "false"});
  result->evidence.push_back(
      {"orh_210_batched_uuid_generation", "row_and_version_batch"});
}

const DirectGeneratedProjectionPlan* DirectGeneratedProjectionForColumn(
    const DirectGeneratedCounterPlan& plan,
    const std::string& column_name) {
  for (const auto& projection : plan.projections) {
    if (projection.column_name == column_name) {
      return &projection;
    }
  }
  return nullptr;
}

bool DirectBuildGeneratedCounterLexicographicIndexEntries(
    const std::vector<CrudIndexRecord>& indexes,
    const DirectGeneratedCounterPlan& generated_plan,
    const DirectBulkUuidBatch& uuid_batch,
    DirectPrecomputedIndexEntryMap* out) {
  if (out == nullptr || !generated_plan.ok || generated_plan.start != 1 ||
      generated_plan.step != 1 ||
      generated_plan.row_count != uuid_batch.row_uuids.size() ||
      generated_plan.row_count != uuid_batch.version_uuids.size()) {
    return false;
  }
  DirectPrecomputedIndexEntryMap built;
  for (const auto& index : indexes) {
    std::string simple_column;
    if (!DirectSimpleScalarIndexKeyColumn(index, &simple_column)) {
      return false;
    }
    const auto* projection =
        DirectGeneratedProjectionForColumn(generated_plan, simple_column);
    if (projection == nullptr ||
        projection->kind != DirectGeneratedProjectionKind::counter) {
      return false;
    }
    built[index.index_uuid].reserve(
        static_cast<std::size_t>(generated_plan.row_count));
  }

  const auto append_counter = [&](auto& self, std::uint64_t value) -> void {
    if (value == 0 || value > generated_plan.row_count) {
      return;
    }
    const std::size_t ordinal = static_cast<std::size_t>(value - 1);
    const std::string key = std::to_string(value);
    for (const auto& index : indexes) {
      auto& entries = built[index.index_uuid];
      entries.push_back({key,
                         key,
                         uuid_batch.row_uuids[ordinal],
                         uuid_batch.version_uuids[ordinal],
                         static_cast<std::uint64_t>(ordinal)});
    }
    if (value > (std::numeric_limits<std::uint64_t>::max() / 10)) {
      return;
    }
    const std::uint64_t base = value * 10;
    for (std::uint64_t digit = 0; digit <= 9; ++digit) {
      const std::uint64_t next = base + digit;
      if (next > generated_plan.row_count) {
        break;
      }
      self(self, next);
    }
  };
  for (std::uint64_t first = 1; first <= 9; ++first) {
    if (first > generated_plan.row_count) {
      break;
    }
    append_counter(append_counter, first);
  }
  *out = std::move(built);
  return true;
}

struct DirectStrictBulkLifecycleResult {
  bool active = false;
  bool ok = true;
  bool recovery_required = false;
  TypedUuid bulk_load_id;
  scratchbird::core::bulk_load::StrictBulkLoadLedger ledger;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<EngineEvidenceReference> evidence;
};

std::string EncodedStrictBulkRow(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::ostringstream encoded;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      encoded << ';';
    }
    encoded << values[index].first << '=' << values[index].second;
  }
  return encoded.str().empty() ? "empty-row" : encoded.str();
}

DirectStrictBulkLifecycleResult RunDirectStrictBulkLifecycle(
    const DirectPhysicalBulkAppendRequest& request,
    const InsertBatchContext& batch_context,
    const std::vector<CrudRowVersionRecord>& staged_rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& logical_value_batch) {
  DirectStrictBulkLifecycleResult result;
  if (!batch_context.strict_bulk_load_selected) {
    return result;
  }
  result.active = true;

  auto fail_before_begin = [&](std::string reason) {
    result.ok = false;
    result.failure_reason = reason;
    result.diagnostic = MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                                     result.failure_reason);
    return result;
  };

  const TypedUuid database_uuid =
      ParseDirectTypedUuid(UuidKind::database, request.context.database_uuid.canonical);
  const TypedUuid object_uuid =
      ParseDirectTypedUuid(UuidKind::object, request.target_table.uuid.canonical);
  const TypedUuid transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction, request.context.transaction_uuid.canonical);
  if (!database_uuid.valid() || !object_uuid.valid() || !transaction_uuid.valid()) {
    return fail_before_begin("strict_bulk_load_invalid_identity");
  }

  scratchbird::core::bulk_load::StrictBulkLoadLedger ledger;
  scratchbird::core::bulk_load::StrictBulkLoadPolicySnapshot policy;
  policy.policy_uuid = GeneratedId(UuidKind::object,
                                   710000 + request.context.local_transaction_id);
  policy.enabled = !DirectOptionEnabled(request, "strict_bulk_load.simulate_begin_refused=true");
  policy.require_all_constraints_valid = true;
  policy.require_all_indexes_valid = true;
  policy.require_all_domains_valid = true;
  policy.require_all_policy_gates_valid = true;

  scratchbird::core::bulk_load::StrictBulkLoadBeginRequest begin_request;
  begin_request.database_uuid = database_uuid;
  begin_request.object_uuid = object_uuid;
  begin_request.transaction_uuid = transaction_uuid;
  begin_request.local_transaction_id = request.context.local_transaction_id;
  begin_request.policy = policy;
  begin_request.staging_target =
      "direct_physical_bulk:" + request.lane_operation + ":strict_staging";
  const auto begin =
      scratchbird::core::bulk_load::BeginStrictBulkLoad(&ledger, begin_request);
  if (!begin.ok()) {
    AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
    result.ok = false;
    result.failure_reason = "strict_bulk_load_begin_refused";
    result.diagnostic = CoreBulkDiagnosticToEngine(begin.diagnostic,
                                                   result.failure_reason);
    return result;
  }
  result.bulk_load_id = begin.operation.bulk_load_id;

  std::vector<scratchbird::core::bulk_load::StrictBulkLoadRow> strict_rows;
  strict_rows.reserve(staged_rows.size());
  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    scratchbird::core::bulk_load::StrictBulkLoadRow row;
    row.row_uuid = ParseDirectTypedUuid(UuidKind::row, staged_rows[index].row_uuid);
    row.encoded_row = EncodedStrictBulkRow(logical_value_batch[index]);
    row.constraints_valid = true;
    row.indexes_valid = true;
    row.domains_valid = true;
    row.policy_gates_valid = true;
    strict_rows.push_back(std::move(row));
  }
  const auto append = scratchbird::core::bulk_load::AppendStrictBulkLoadRows(
      &ledger,
      scratchbird::core::bulk_load::StrictBulkLoadAppendRequest{
          begin.operation.bulk_load_id,
          transaction_uuid,
          request.context.local_transaction_id,
          std::move(strict_rows)});
  if (!append.ok()) {
    AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
    result.ok = false;
    result.failure_reason = "strict_bulk_load_append_refused";
    result.diagnostic = CoreBulkDiagnosticToEngine(append.diagnostic,
                                                   result.failure_reason);
    return result;
  }

  if (DirectOptionEnabled(request, "strict_bulk_load.simulate_rollback_before_publication=true")) {
    const auto rollback = scratchbird::core::bulk_load::RollbackStrictBulkLoad(
        &ledger,
        scratchbird::core::bulk_load::StrictBulkLoadRollbackRequest{
            begin.operation.bulk_load_id,
            transaction_uuid,
            request.context.local_transaction_id,
            "direct physical strict bulk rollback before publication"});
    AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
    const auto recovery =
        scratchbird::core::bulk_load::ClassifyStrictBulkLoadLedgerForRecovery(ledger);
    AddStrictBulkRecoveryEvidence(recovery, &result.evidence);
    result.ok = false;
    result.failure_reason = rollback.ok()
                                ? "strict_bulk_load_rollback_requested"
                                : "strict_bulk_load_rollback_refused";
    result.diagnostic = rollback.ok()
                            ? MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                                           result.failure_reason)
                            : CoreBulkDiagnosticToEngine(rollback.diagnostic,
                                                         result.failure_reason);
    return result;
  }

  if (DirectOptionEnabled(request, "strict_bulk_load.simulate_quarantine_before_publication=true")) {
    const auto quarantine = scratchbird::core::bulk_load::QuarantineStrictBulkLoad(
        &ledger,
        scratchbird::core::bulk_load::StrictBulkLoadQuarantineRequest{
            begin.operation.bulk_load_id,
            transaction_uuid,
            request.context.local_transaction_id,
            "direct physical strict bulk quarantine before publication"});
    AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
    const auto recovery =
        scratchbird::core::bulk_load::ClassifyStrictBulkLoadLedgerForRecovery(ledger);
    AddStrictBulkRecoveryEvidence(recovery, &result.evidence);
    result.ok = false;
    result.failure_reason = quarantine.ok()
                                ? "strict_bulk_load_quarantine_requested"
                                : "strict_bulk_load_quarantine_refused";
    result.diagnostic = quarantine.ok()
                            ? MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                                           result.failure_reason)
                            : CoreBulkDiagnosticToEngine(quarantine.diagnostic,
                                                         result.failure_reason);
    return result;
  }

  const bool simulate_finalize_failure = DirectOptionEnabled(
      request,
      "strict_bulk_load.simulate_finalize_failure_after_evidence=true");
  const auto finalize = scratchbird::core::bulk_load::FinalizeStrictBulkLoadEvidenceDurable(
      &ledger,
      scratchbird::core::bulk_load::StrictBulkLoadFinalizeRequest{
          begin.operation.bulk_load_id,
          transaction_uuid,
          request.context.local_transaction_id,
          simulate_finalize_failure,
          "direct-physical-strict-bulk-visibility-fence"});
  AddStrictBulkLifecycleEvidence(ledger, &result.evidence);
  if (!finalize.ok()) {
    const auto recovery =
        scratchbird::core::bulk_load::ClassifyStrictBulkLoadLedgerForRecovery(ledger);
    AddStrictBulkRecoveryEvidence(recovery, &result.evidence);
    result.ok = false;
    result.recovery_required = finalize.recovery_required;
    result.failure_reason = finalize.recovery_required
                                ? "strict_bulk_load_recovery_required"
                                : "strict_bulk_load_finalize_refused";
    result.diagnostic = CoreBulkDiagnosticToEngine(finalize.diagnostic,
                                                   result.failure_reason);
    return result;
  }

  result.evidence.push_back({"strict_bulk_load_direct_publication_fence",
                             "finalize_evidence_durable_before_mga_visibility"});
  result.ledger = std::move(ledger);
  return result;
}

void AddStrictPhysicalPublicationFailureEvidence(
    DirectStrictBulkLifecycleResult* lifecycle,
    const std::string& stage,
    const EngineApiDiagnostic& diagnostic,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr || lifecycle == nullptr || !lifecycle->active) {
    return;
  }
  evidence->push_back({"strict_bulk_load_physical_publication_failed", stage});
  evidence->push_back({"strict_bulk_load_physical_publication_diagnostic",
                       diagnostic.detail});
  const auto transaction_uuid = lifecycle->ledger.operations.empty()
                                    ? TypedUuid{}
                                    : lifecycle->ledger.operations.front().transaction_uuid;
  const auto local_transaction_id = lifecycle->ledger.operations.empty()
                                        ? 0
                                        : lifecycle->ledger.operations.front().local_transaction_id;
  if (lifecycle->bulk_load_id.valid() && transaction_uuid.valid() && local_transaction_id != 0) {
    (void)scratchbird::core::bulk_load::QuarantineStrictBulkLoad(
        &lifecycle->ledger,
        scratchbird::core::bulk_load::StrictBulkLoadQuarantineRequest{
            lifecycle->bulk_load_id,
            transaction_uuid,
            local_transaction_id,
            "direct physical publication failed after finalize evidence: " + stage});
    AddStrictBulkLifecycleEvidence(lifecycle->ledger, evidence);
    const auto recovery =
        scratchbird::core::bulk_load::ClassifyStrictBulkLoadLedgerForRecovery(
            lifecycle->ledger);
    AddStrictBulkRecoveryEvidence(recovery, evidence);
  }
}

DirectPhysicalBulkAppendResult DirectStrictPhysicalPublicationFailure(
    const DirectPhysicalBulkAppendRequest& request,
    DirectStrictBulkLifecycleResult* lifecycle,
    const DirectPhysicalBulkAppendResult& partial_result,
    EngineApiDiagnostic diagnostic,
    std::string reason,
    const std::string& stage) {
  auto evidence = partial_result.evidence;
  AddStrictPhysicalPublicationFailureEvidence(lifecycle, stage, diagnostic, &evidence);
  return DirectBulkFailureWithEvidence(request,
                                       std::move(diagnostic),
                                       std::move(reason),
                                       evidence,
                                       partial_result.dml_summary);
}

DirectPhysicalBulkAppendResult PublishDirectStrictBulkAfterPhysicalSuccess(
    const DirectPhysicalBulkAppendRequest& request,
    DirectStrictBulkLifecycleResult* lifecycle,
    DirectPhysicalBulkAppendResult result) {
  if (lifecycle == nullptr || !lifecycle->active) {
    return result;
  }
  result.evidence.push_back({"strict_bulk_load_physical_publication_succeeded",
                             "row_index_append_flush"});
  const auto transaction_uuid =
      ParseDirectTypedUuid(UuidKind::transaction, request.context.transaction_uuid.canonical);
  const auto published = scratchbird::core::bulk_load::PublishStrictBulkLoadVisible(
      &lifecycle->ledger,
      scratchbird::core::bulk_load::StrictBulkLoadPublishRequest{
          lifecycle->bulk_load_id,
          transaction_uuid,
          request.context.local_transaction_id,
          "direct-physical-strict-bulk-visibility-fence"});
  if (!published.ok()) {
    auto evidence = result.evidence;
    AddStrictBulkLifecycleEvidence(lifecycle->ledger, &evidence);
    return DirectBulkFailureWithEvidence(
        request,
        CoreBulkDiagnosticToEngine(published.diagnostic,
                                   "strict_bulk_load_publish_refused"),
        "strict_bulk_load_publish_refused",
        evidence,
        result.dml_summary);
  }
  AddStrictBulkLifecycleEvidence(lifecycle->ledger, &result.evidence);
  result.evidence.push_back({"strict_bulk_load_direct_lane_published_after",
                             "strict_bulk_load_physical_publication_succeeded"});
  return result;
}

}  // namespace

InsertPhysicalIntegrationResult ExecuteInsertPhysicalIntegration(
    InsertPhysicalIntegrationContext* context,
    const InsertPhysicalIntegrationRequest& request) {
  if (context == nullptr ||
      context->page_reservation_ledger == nullptr ||
      context->page_selection_ledger == nullptr) {
    return Refuse("insert_physical_integration_missing_page_authority",
                  "engine.insert.physical.missing_page_authority",
                  "page reservation and selection ledgers are required");
  }
  if (!request.database_uuid.valid() ||
      !request.object_uuid.valid() ||
      !request.transaction_uuid.valid() ||
      request.local_transaction_id == 0) {
    return Refuse("insert_physical_integration_invalid_identity",
                  "engine.insert.physical.invalid_identity",
                  "database, object, transaction UUIDs and local transaction ID are required");
  }

  InsertPhysicalIntegrationResult result;
  TypedUuid resolved_filespace_uuid = request.filespace_uuid;
  auto resolved_object_class = request.placement_object_class;
  auto resolved_growth_role = request.growth_filespace_role;

  const bool placement_resolution_required =
      request.require_placement_policy ||
      request.placement_policy.present ||
      request.placement_object_class !=
          scratchbird::storage::filespace::FilespaceObjectClass::unspecified;
  if (placement_resolution_required) {
    if (context->filespace_registry == nullptr) {
      return Refuse("insert_physical_integration_missing_placement_registry",
                    "engine.insert.physical.missing_placement_registry",
                    "filespace registry is required for placement policy resolution");
    }
    scratchbird::storage::filespace::FilespacePlacementRequest placement_request;
    placement_request.database_uuid = request.database_uuid;
    placement_request.preferred_filespace_uuid = request.filespace_uuid;
    placement_request.owner_object_uuid = request.object_uuid;
    placement_request.policy_uuid = request.policy_uuid;
    placement_request.object_class = request.placement_object_class;
    placement_request.page_family = request.page_family;
    placement_request.page_size = request.page_size;
    placement_request.require_preallocation =
        request.require_placement_preallocation;
    placement_request.requested_preallocation_pages =
        request.placement_preallocation_pages;
    placement_request.reason = "insert_physical_integration";
    placement_request.policy = request.placement_policy;
    const auto placement = scratchbird::storage::filespace::ResolveFilespacePlacement(
        *context->filespace_registry,
        placement_request);
    if (!placement.ok()) {
      return Refuse(placement.diagnostic.diagnostic_code,
                    placement.diagnostic.message_key,
                    "filespace placement refused");
    }
    resolved_filespace_uuid = placement.descriptor.filespace_uuid;
    resolved_object_class = placement.object_class;
    resolved_growth_role = placement.descriptor.role;
    result.filespace_placement_resolved = true;
    result.resolved_filespace_uuid = resolved_filespace_uuid;
    result.resolved_filespace_class =
        scratchbird::storage::filespace::FilespaceClassName(
            placement.filespace_class);
    result.resolved_filespace_role =
        scratchbird::storage::filespace::FilespaceRoleName(
            placement.descriptor.role);
    result.evidence_refs.push_back(EvidenceRef("filespace_placement",
                                               resolved_filespace_uuid));
    for (const auto& evidence : placement.evidence) {
      result.evidence_refs.push_back("filespace_placement:" + evidence);
    }

    if (placement.preallocation_required) {
      if (context->filespace_growth_ledger == nullptr) {
        return Refuse("insert_physical_integration_missing_preallocation_ledger",
                      "engine.insert.physical.missing_preallocation_ledger",
                      "filespace growth ledger is required for placement preallocation");
      }
      scratchbird::storage::filespace::FilespacePreallocationRequest preallocate;
      preallocate.request_uuid = request.request_id.valid()
                                     ? request.request_id
                                     : GeneratedId(UuidKind::object, 300010);
      preallocate.database_uuid = request.database_uuid;
      preallocate.filespace_uuid = resolved_filespace_uuid;
      preallocate.policy_uuid = request.policy_uuid;
      preallocate.storage_profile_uuid =
          placement.descriptor.writer_identity_uuid.valid()
              ? placement.descriptor.writer_identity_uuid
              : GeneratedId(UuidKind::object, 300011);
      preallocate.requested_page_count = placement.preallocation_page_count;
      preallocate.page_size_bytes = request.page_size;
      preallocate.policy_generation = 1;
      preallocate.observed_policy_generation = 1;
      preallocate.catalog_generation =
          placement.descriptor.generation == 0 ? 1 : placement.descriptor.generation;
      preallocate.observed_catalog_generation = preallocate.catalog_generation;
      preallocate.member_capacity.present = true;
      preallocate.member_capacity.explicit_capacity_context = true;
      preallocate.member_capacity.file_member_uuid =
          placement.descriptor.writer_identity_uuid.valid()
              ? placement.descriptor.writer_identity_uuid
              : GeneratedId(UuidKind::object, 300012);
      preallocate.member_capacity.start_page_number = 0;
      preallocate.member_capacity.current_page_count =
          placement.descriptor.total_pages;
      preallocate.member_capacity.preallocated_page_count =
          placement.descriptor.preallocated_pages;
      preallocate.member_capacity.maximum_page_count =
          placement.descriptor.total_pages +
          placement.descriptor.preallocated_pages +
          placement.preallocation_page_count + 1024;
      preallocate.member_capacity.physical_path = placement.descriptor.path;
      preallocate.member_capacity.online = true;
      preallocate.member_capacity.writable = !placement.descriptor.read_only;
      preallocate.transaction_context.present = true;
      preallocate.transaction_context.transaction_uuid = request.transaction_uuid;
      preallocate.transaction_context.transaction_number =
          request.local_transaction_id;
      preallocate.transaction_context.durable_inventory_admitted = true;
      preallocate.transaction_context.write_intent = true;
      preallocate.transaction_context.durability_fence_satisfied = true;
      preallocate.evidence_store_present = true;
      preallocate.evidence_before_success = true;
      preallocate.require_mga_transaction_context = true;
      preallocate.reason = "insert_physical_integration.placement_preallocation";
      const auto preallocated =
          scratchbird::storage::filespace::PreallocateFilespace(
              context->filespace_growth_ledger,
              *context->filespace_registry,
              preallocate);
      if (!preallocated.ok()) {
        return Refuse(preallocated.diagnostic.diagnostic_code,
                      preallocated.diagnostic.message_key,
                      "filespace placement preallocation refused");
      }
      result.filespace_preallocation_admitted = true;
      result.preallocation_operation_id =
          preallocated.operation.preallocation_operation_id;
      result.evidence_refs.push_back(EvidenceRef("filespace_preallocation",
                                                 result.preallocation_operation_id));
    }
  }

  scratchbird::storage::page::InsertPageReservationRequest reservation_request;
  reservation_request.database_uuid = request.database_uuid;
  reservation_request.transaction_uuid = request.transaction_uuid;
  reservation_request.local_transaction_id = request.local_transaction_id;
  reservation_request.object_uuid = request.object_uuid;
  reservation_request.page_family = request.page_family;
  reservation_request.estimated_row_count = request.estimated_row_count;
  reservation_request.estimated_payload_bytes = request.estimated_payload_bytes;
  reservation_request.preferred_filespace_uuid = resolved_filespace_uuid;
  reservation_request.policy_uuid = request.policy_uuid;
  reservation_request.request_id = request.request_id.valid() ? request.request_id : GeneratedId(UuidKind::object, 300000);
  reservation_request.object_class = resolved_object_class;
  reservation_request.page_size = request.page_size;
  reservation_request.current_time_authority_tick = request.time_authority_tick;
  reservation_request.lease_duration_ticks = request.reservation_lease_ticks;

  const auto reservation = scratchbird::storage::page::ReserveInsertPagesDurable(
      context->page_reservation_ledger,
      reservation_request);
  if (!reservation.ok()) {
    return Refuse(reservation.diagnostic.diagnostic_code,
                  reservation.diagnostic.message_key,
                  "page reservation refused");
  }
  result.page_reserved = true;
  result.reservation_id = reservation.reservation.reservation_id;
  result.evidence_refs.push_back(EvidenceRef("page_reservation", reservation.reservation.reservation_id));

  auto refuse_after_reservation = [&](std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail) {
    (void)scratchbird::storage::page::ReleaseInsertPageReservation(
        context->page_reservation_ledger,
        scratchbird::storage::page::ReleasePageReservationRequest{
            result.reservation_id,
            "insert_physical_integration_failure:" + diagnostic_code});
    return Refuse(std::move(diagnostic_code), std::move(message_key), std::move(detail));
  };

  scratchbird::storage::page::InsertPageSelectionRequest selection_request;
  selection_request.database_uuid = request.database_uuid;
  selection_request.transaction_uuid = request.transaction_uuid;
  selection_request.local_transaction_id = request.local_transaction_id;
  selection_request.object_uuid = request.object_uuid;
  selection_request.reservation_id = reservation.reservation.reservation_id;
  selection_request.page_family = request.page_family;
  selection_request.encoded_row_bytes = request.encoded_row_bytes;

  auto selection = scratchbird::storage::page::SelectInsertTargetPage(
      context->page_selection_ledger,
      context->page_reservation_ledger,
      selection_request);
  if (!selection.ok() && request.request_filespace_growth_on_missing_page) {
    if (context->filespace_growth_ledger == nullptr || context->filespace_registry == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_filespace_authority",
                                      "engine.insert.physical.missing_filespace_authority",
                                      "filespace growth ledger and registry are required for growth admission");
    }
    scratchbird::storage::filespace::InsertFilespaceGrowthRequest growth_request;
    growth_request.database_uuid = request.database_uuid;
    growth_request.filespace_uuid = resolved_filespace_uuid;
    growth_request.filespace_role = resolved_growth_role;
    growth_request.page_family = request.page_family;
    growth_request.requested_page_count = request.growth_page_count == 0 ? 1 : request.growth_page_count;
    growth_request.urgency_class = request.growth_urgency;
    growth_request.predicted_insert_pressure_pages = request.predicted_insert_pressure_pages;
    growth_request.policy_uuid = request.policy_uuid;
    const auto growth = scratchbird::storage::filespace::RequestInsertFilespaceGrowth(
        context->filespace_growth_ledger,
        *context->filespace_registry,
        growth_request);
    if (!growth.ok()) {
      return refuse_after_reservation(growth.diagnostic.diagnostic_code,
                                      growth.diagnostic.message_key,
                                      "filespace growth admission refused");
    }
    result.filespace_growth_admitted = true;
    result.growth_operation_id = growth.growth_operation_id;
    result.evidence_refs.push_back(EvidenceRef("filespace_growth", growth.growth_operation_id));
    (void)scratchbird::storage::page::ReleaseInsertPageReservation(
        context->page_reservation_ledger,
        scratchbird::storage::page::ReleasePageReservationRequest{
            result.reservation_id,
            "insert_physical_integration_growth_admitted_without_append"});
    result.status = IntegrationOkStatus();
    result.integrated = false;
    result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                               "ok",
                                                               "engine.insert.physical.growth_admitted",
                                                               "filespace growth admitted; row was not physically integrated");
    return result;
  } else if (!selection.ok()) {
    return refuse_after_reservation(selection.diagnostic.diagnostic_code,
                                    selection.diagnostic.message_key,
                                    "page selection refused");
  } else {
    result.page_selected = true;
    result.selection_fence = selection.selection.selection_fence;
    result.evidence_refs.push_back("page_selection:" + selection.selection.selection_fence);
  }

  if (request.enable_deferred_secondary_index) {
    if (!(request.deferred_index_overlay_gate &&
          request.deferred_index_merge_gate &&
          request.deferred_index_cleanup_gate &&
          request.deferred_index_recovery_gate)) {
      return refuse_after_reservation("insert_physical_integration_deferred_index_gates_unproven",
                                      "engine.insert.physical.deferred_index_gates_unproven",
                                      "deferred secondary-index maintenance requires overlay, merge, cleanup, and recovery gates");
    }
    if (context->secondary_index_overlay_ledger == nullptr ||
        context->secondary_index_merge_ledger == nullptr ||
        context->secondary_index_base_entries == nullptr ||
        context->secondary_index_delta_ledger == nullptr ||
        !request.secondary_index_uuid.valid()) {
      return refuse_after_reservation("insert_physical_integration_missing_index_authority",
                                      "engine.insert.physical.missing_index_authority",
                                      "secondary-index overlay, merge, base index, and delta ledgers are required");
    }
    scratchbird::core::index::SecondaryIndexOverlayRequest overlay_request;
    overlay_request.index_uuid = request.secondary_index_uuid;
    overlay_request.table_uuid = request.object_uuid;
    overlay_request.transaction_uuid = request.transaction_uuid;
    overlay_request.local_transaction_id = request.local_transaction_id;
    overlay_request.snapshot_high_water_local_transaction_id =
        request.secondary_index_snapshot_high_water_local_transaction_id;
    const auto overlay = scratchbird::core::index::BuildSecondaryIndexDeltaOverlay(
        context->secondary_index_overlay_ledger,
        *context->secondary_index_base_entries,
        *context->secondary_index_delta_ledger,
        overlay_request);
    if (!overlay.ok()) {
      return refuse_after_reservation(overlay.diagnostic.diagnostic_code,
                                      overlay.diagnostic.message_key,
                                      "secondary-index overlay refused");
    }
    scratchbird::core::index::SecondaryIndexMergeRequest merge_request;
    merge_request.index_uuid = request.secondary_index_uuid;
    merge_request.table_uuid = request.object_uuid;
    merge_request.merge_id = GeneratedId(UuidKind::object, 300001);
    merge_request.authoritative_cleanup_horizon_local_transaction_id =
        request.secondary_index_cleanup_horizon_local_transaction_id;
    merge_request.cleanup_horizon_authoritative = true;
    const auto merge = scratchbird::core::index::MergeSecondaryIndexDeltas(
        context->secondary_index_merge_ledger,
        context->secondary_index_base_entries,
        context->secondary_index_delta_ledger,
        merge_request);
    if (!merge.ok()) {
      return refuse_after_reservation(merge.diagnostic.diagnostic_code,
                                      merge.diagnostic.message_key,
                                      "secondary-index merge refused");
    }
    const auto recovery = scratchbird::core::index::ClassifySecondaryIndexMergeLedgerForRecovery(
        *context->secondary_index_merge_ledger);
    if (!recovery.ok()) {
      return refuse_after_reservation(recovery.diagnostic.diagnostic_code,
                                      recovery.diagnostic.message_key,
                                      "secondary-index recovery classification refused");
    }
    result.deferred_secondary_index_verified = true;
    result.evidence_refs.push_back(EvidenceRef("secondary_index_merge", merge.evidence.evidence_id));
  }

  if (request.persist_overflow_payload) {
    if (context->overflow_ledger == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_overflow_authority",
                                      "engine.insert.physical.missing_overflow_authority",
                                      "overflow ledger is required");
    }
    scratchbird::storage::page::OverflowPersistRequest overflow_request;
    overflow_request.row_uuid = request.row_uuid;
    overflow_request.object_uuid = request.object_uuid;
    overflow_request.transaction_uuid = request.transaction_uuid;
    overflow_request.local_transaction_id = request.local_transaction_id;
    overflow_request.value_descriptor = request.overflow_value_descriptor;
    overflow_request.payload_bytes = request.overflow_payload;
    overflow_request.chunk_policy_uuid = request.policy_uuid;
    overflow_request.chunk_size = request.overflow_chunk_size;
    const auto overflow = scratchbird::storage::page::PersistOverflowValue(context->overflow_ledger, overflow_request);
    if (!overflow.ok()) {
      return refuse_after_reservation(overflow.diagnostic.diagnostic_code,
                                      overflow.diagnostic.message_key,
                                      "overflow persistence refused");
    }
    const auto commit = scratchbird::storage::page::CommitOverflowValue(
        context->overflow_ledger,
        scratchbird::storage::page::OverflowCommitRequest{
            overflow.overflow_value_uuid,
            request.transaction_uuid,
            request.local_transaction_id,
            "insert_physical_integration"});
    if (!commit.ok()) {
      return refuse_after_reservation(commit.diagnostic.diagnostic_code,
                                      commit.diagnostic.message_key,
                                      "overflow commit refused");
    }
    result.overflow_persisted = true;
    result.overflow_value_uuid = overflow.overflow_value_uuid;
    result.evidence_refs.push_back(EvidenceRef("overflow", overflow.overflow_value_uuid));
  }

  if (request.run_strict_bulk_load) {
    if (context->strict_bulk_load_ledger == nullptr) {
      return refuse_after_reservation("insert_physical_integration_missing_bulk_load_authority",
                                      "engine.insert.physical.missing_bulk_load_authority",
                                      "strict bulk-load ledger is required");
    }
    scratchbird::core::bulk_load::StrictBulkLoadBeginRequest begin_request;
    begin_request.database_uuid = request.database_uuid;
    begin_request.object_uuid = request.object_uuid;
    begin_request.transaction_uuid = request.transaction_uuid;
    begin_request.local_transaction_id = request.local_transaction_id;
    begin_request.policy = request.strict_bulk_load_policy;
    begin_request.staging_target = request.strict_bulk_load_staging_target;
    const auto begin = scratchbird::core::bulk_load::BeginStrictBulkLoad(
        context->strict_bulk_load_ledger,
        begin_request);
    if (!begin.ok()) {
      return refuse_after_reservation(begin.diagnostic.diagnostic_code,
                                      begin.diagnostic.message_key,
                                      "strict bulk-load begin refused");
    }
    const auto append = scratchbird::core::bulk_load::AppendStrictBulkLoadRows(
        context->strict_bulk_load_ledger,
        scratchbird::core::bulk_load::StrictBulkLoadAppendRequest{
            begin.operation.bulk_load_id,
            request.transaction_uuid,
            request.local_transaction_id,
            request.strict_bulk_load_rows});
    if (!append.ok()) {
      return refuse_after_reservation(append.diagnostic.diagnostic_code,
                                      append.diagnostic.message_key,
                                      "strict bulk-load append refused");
    }
    const auto finalize = scratchbird::core::bulk_load::FinalizeStrictBulkLoad(
        context->strict_bulk_load_ledger,
        scratchbird::core::bulk_load::StrictBulkLoadFinalizeRequest{
            begin.operation.bulk_load_id,
            request.transaction_uuid,
            request.local_transaction_id,
            false,
            "insert-physical-strict-bulk-load-fence"});
    if (!finalize.ok()) {
      return refuse_after_reservation(finalize.diagnostic.diagnostic_code,
                                      finalize.diagnostic.message_key,
                                      "strict bulk-load finalize refused");
    }
    result.strict_bulk_load_finalized = true;
    result.strict_bulk_load_id = begin.operation.bulk_load_id;
    result.evidence_refs.push_back(EvidenceRef("strict_bulk_load", begin.operation.bulk_load_id));
  }

  if (result.page_selected) {
    const auto append = scratchbird::storage::page::AppendRowToSelectedPageWithReservationLedger(
        context->page_selection_ledger,
        context->page_reservation_ledger,
        scratchbird::storage::page::InsertPageAppendRequest{
            result.selection_fence,
            request.encoded_row_bytes,
            "insert_physical_integration"});
    if (!append.ok()) {
      return refuse_after_reservation(append.diagnostic.diagnostic_code,
                                      append.diagnostic.message_key,
                                      "selected page append refused");
    }
  }

  result.status = IntegrationOkStatus();
  result.integrated = result.page_selected;
  result.diagnostic = MakeInsertPhysicalIntegrationDiagnostic(result.status,
                                                            "ok",
                                                            "engine.insert.physical.integrated",
                                                            "insert physical integration completed");
  return result;
}

DiagnosticRecord MakeInsertPhysicalIntegrationDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail) {
  std::vector<scratchbird::core::platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(status.code,
                                                     status.severity,
                                                     status.subsystem,
                                                     std::move(diagnostic_code),
                                                     std::move(message_key),
                                                     std::move(arguments),
                                                     {},
                                                     "engine.insert.physical_integration",
                                                     status.ok() ? "" : "fall back to existing safe insert path and do not claim physical integration success");
}

DirectPhysicalBulkAppendResult ExecuteDirectPhysicalBulkAppend(
    const DirectPhysicalBulkAppendRequest& request) {
  const auto write_result_policy = ResolveWriteResultPolicyOptions(
      request.option_envelopes,
      "dml.direct_physical_bulk_append");
  if (!write_result_policy.ok) {
    auto failure = DirectBulkFailure(
        request,
        write_result_policy.diagnostic,
        "write_result_policy_refused");
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  if (!request.direct_lane_enabled) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "direct_physical_lane_disabled"),
        "direct_physical_lane_disabled");
  }
  if (request.context.local_transaction_id == 0) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "local_transaction_id_required"),
        "local_transaction_id_required");
  }
  if (request.target_table.uuid.canonical.empty()) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "target_table_uuid_required"),
        "target_table_uuid_required");
  }
  const bool generated_counter_requested =
      DirectGeneratedCounterEnvelopeRequested(request);
  if (request.borrowed_input_rows.empty() && !generated_counter_requested) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "canonical_rows_required"),
        "canonical_rows_required");
  }
  if (request.duplicate_mode != "error") {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "direct_physical_lane_duplicate_mode_unsupported"),
        "direct_physical_lane_duplicate_mode_unsupported");
  }
  if (!request.shared_row_field_order.empty()) {
    for (const auto& input_row : request.borrowed_input_rows) {
      if (input_row.fields.size() != request.shared_row_field_order.size()) {
        return DirectBulkFailure(
            request,
            MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                         "shared_row_field_order_shape_mismatch"),
            "shared_row_field_order_shape_mismatch");
      }
    }
  }

  const auto phase_start = DirectSteadyClock::now();
  auto phase_last = phase_start;
  std::vector<std::pair<std::string, EngineApiU64>> phase_micros;
  phase_micros.reserve(26);
  std::vector<EngineEvidenceReference> descriptor_trace;
  descriptor_trace.reserve(8);
  const auto mark_phase = [&](std::string phase) {
    const auto now = DirectSteadyClock::now();
    phase_micros.push_back(
        {std::move(phase), DirectElapsedMicros(phase_last, now)});
    phase_last = now;
  };
  const auto mark_descriptor_step =
      [&](std::string step,
          DirectSteadyClock::time_point start,
          DirectSteadyClock::time_point finish) {
        descriptor_trace.push_back(
            {"direct_physical_bulk_trace." + std::move(step) + "_us",
             std::to_string(DirectElapsedMicros(start, finish))});
      };

  const auto index_only_start = DirectSteadyClock::now();
  const auto index_only_eligibility =
      CanUseMgaRelationIndexOnlyProofForInsertTarget(
          request.context,
          request.target_table.uuid.canonical);
  mark_descriptor_step("index_only_eligibility",
                       index_only_start,
                       DirectSteadyClock::now());
  if (!index_only_eligibility.ok) {
    return DirectBulkFailure(request,
                             index_only_eligibility.diagnostic,
                             "mga_relation_index_only_eligibility_failed");
  }
  bool index_entries_authoritative = index_only_eligibility.eligible;
  const bool bypass_single_window_native_bulk_cache =
      DirectBypassPostAppendCacheForSingleWindowNativeBulk(request);
  const bool sorted_bulk_index_requested =
      DirectSortedBulkIndexBuildEnabled(request);
  bool append_index_cache_hit = false;
  if (index_entries_authoritative && !bypass_single_window_native_bulk_cache) {
    append_index_cache_hit = DirectAppendIndexEntryCacheAvailable(
        request.context,
        request.target_table.uuid.canonical,
        index_only_eligibility.row_version_count,
        sorted_bulk_index_requested);
  }
  DirectBulkAppendContextCacheRecord bulk_context_cache;
  std::string append_index_cache_context_note;
  bool bulk_context_cache_hit = DirectLookupBulkAppendContextCache(
      request.context,
      request.target_table.uuid.canonical,
      index_only_eligibility.row_version_count,
      &bulk_context_cache);
  if (bulk_context_cache_hit) {
    index_entries_authoritative = bulk_context_cache.index_entries_authoritative;
    if (index_entries_authoritative && !append_index_cache_hit) {
      append_index_cache_context_note = "miss_after_context_hit";
      bulk_context_cache_hit = false;
    }
  }
  MgaRelationStoreResult loaded;
  if (bulk_context_cache_hit) {
    const auto relation_load_start = DirectSteadyClock::now();
    loaded.ok = true;
    loaded.evidence.push_back({"direct_physical_bulk_append_context_cache", "hit"});
    if (!append_index_cache_context_note.empty()) {
      loaded.evidence.push_back({"direct_physical_append_index_cache",
                                 append_index_cache_context_note});
    }
    loaded.evidence.push_back({"direct_physical_bulk_append_context_scope",
                               "transaction_table_security_epoch"});
    mark_descriptor_step("relation_state_load",
                         relation_load_start,
                         DirectSteadyClock::now());
  } else {
    const auto relation_load_start = DirectSteadyClock::now();
    loaded =
        index_entries_authoritative
            ? (append_index_cache_hit
                   ? LoadMgaRelationStoreMetadataOnlyForInsertTarget(
                         request.context,
                         request.target_table.uuid.canonical)
                   : LoadMgaRelationStoreIndexesOnlyForInsertTarget(
                         request.context,
                         request.target_table.uuid.canonical))
            : LoadMgaRelationStoreStateForInsertTarget(
                  request.context,
                  request.target_table.uuid.canonical);
    mark_descriptor_step("relation_state_load",
                         relation_load_start,
                         DirectSteadyClock::now());
  }
  if (!loaded.ok) {
    return DirectBulkFailure(request,
                             loaded.diagnostic,
                             "mga_relation_store_load_failed");
  }
  CrudState state_storage;
  const CrudState* state = nullptr;
  if (bulk_context_cache_hit) {
    const auto state_build_start = DirectSteadyClock::now();
    state = bulk_context_cache.state.get();
    mark_descriptor_step("state_build",
                         state_build_start,
                         DirectSteadyClock::now());
  } else {
    const auto state_build_start = DirectSteadyClock::now();
    state_storage = BuildCrudCompatibilityStateFromMga(std::move(loaded.state));
    state = &state_storage;
    mark_descriptor_step("state_build",
                         state_build_start,
                         DirectSteadyClock::now());
  }
  if (state == nullptr) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "direct_physical_context_cache_state_missing"),
        "direct_physical_context_cache_state_missing");
  }
  if (!bulk_context_cache_hit && index_entries_authoritative && !append_index_cache_hit) {
    DirectStoreAppendIndexEntryCache(
        request.context,
        request.target_table.uuid.canonical,
        index_only_eligibility.row_version_count,
        state->index_entries);
    append_index_cache_hit = true;
  }
  const auto find_table_start = DirectSteadyClock::now();
  auto table = FindVisibleCrudTable(*state,
                                    request.target_table.uuid.canonical,
                                    request.context.local_transaction_id);
  mark_descriptor_step("find_visible_table",
                       find_table_start,
                       DirectSteadyClock::now());
  if (!table) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "target_table_not_visible"),
        "target_table_not_visible");
  }
  if (index_entries_authoritative && DirectTableDeclaresForeignKey(*table)) {
    auto reloaded = LoadMgaRelationStoreStateForInsertTarget(
        request.context,
        request.target_table.uuid.canonical);
    if (!reloaded.ok) {
      return DirectBulkFailure(request,
                               reloaded.diagnostic,
                               "mga_relation_store_load_failed");
    }
    loaded = std::move(reloaded);
    state_storage = BuildCrudCompatibilityStateFromMga(std::move(loaded.state));
    state = &state_storage;
    table = FindVisibleCrudTable(*state,
                                 request.target_table.uuid.canonical,
                                 request.context.local_transaction_id);
    if (!table) {
      return DirectBulkFailure(
          request,
          MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                       "target_table_not_visible"),
          "target_table_not_visible");
    }
    index_entries_authoritative = false;
    append_index_cache_hit = false;
  }
  (void)scratchbird::core::metrics::RecordInsertRelationStateLoad(
      request.target_table.uuid.canonical,
      "copy_import",
      loaded.full_state_load,
      loaded.scoped_state_load,
      index_entries_authoritative
          ? "direct_physical_bulk_insert_target_index_only_scoped"
          : "direct_physical_bulk_insert_target_scoped");
  if (table->temporary && request.context.session_uuid.canonical.empty()) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "temporary_table_requires_session_uuid"),
        "temporary_table_requires_session_uuid");
  }
  if (CrudRowsTouchOpaqueColumn(*table, request.borrowed_input_rows) &&
      !DirectOpaqueColumnsAllowed(request)) {
    return DirectBulkFailure(
        request,
        UnsupportedCrudFeatureDiagnostic("dml.direct_physical_bulk_append",
                                         "opaque_column_mutation_denied"),
        "opaque_column_mutation_denied");
  }
  const std::string precheck_failure =
      DirectPageExtentPreallocationPrecheckFailure(request);
  if (!precheck_failure.empty()) {
    EngineDmlSummaryCounters summary;
    summary.preallocation_refused = 1;
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     precheck_failure),
        precheck_failure,
        summary);
  }

  std::vector<CrudIndexRecord> visible_indexes;
  MgaRelationStorageDescriptor relation_descriptor;
  if (bulk_context_cache_hit) {
    const auto descriptor_start = DirectSteadyClock::now();
    visible_indexes = bulk_context_cache.visible_indexes;
    relation_descriptor = bulk_context_cache.relation_descriptor;
    mark_descriptor_step("visible_indexes_descriptor",
                         descriptor_start,
                         DirectSteadyClock::now());
  } else {
    const auto descriptor_start = DirectSteadyClock::now();
    visible_indexes = VisibleCrudIndexesForTable(
        *state,
        request.target_table.uuid.canonical,
        request.context.local_transaction_id);
    const auto descriptor_ready = EnsureMgaRelationStorageDescriptor(
        request.context,
        *table,
        visible_indexes,
        &relation_descriptor);
    if (descriptor_ready.error) {
      return DirectBulkFailure(request,
                               descriptor_ready,
                               "relation_descriptor_refused");
    }
    if (index_entries_authoritative &&
        !bypass_single_window_native_bulk_cache &&
        !DirectTableDeclaresForeignKey(*table)) {
      DirectStoreBulkAppendContextCache(request.context,
                                        request.target_table.uuid.canonical,
                                        index_only_eligibility.row_version_count,
                                        *state,
                                        visible_indexes,
                                        relation_descriptor,
                                        index_entries_authoritative,
                                        append_index_cache_hit);
    }
    mark_descriptor_step("visible_indexes_descriptor",
                         descriptor_start,
                         DirectSteadyClock::now());
  }

  const DirectGeneratedCounterPlan generated_counter_plan =
      DirectBuildGeneratedCounterPlan(request, *table);
  if (generated_counter_requested && !generated_counter_plan.ok) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     generated_counter_plan.failure_reason.empty()
                                         ? "insert_select_generator_descriptor_invalid"
                                         : generated_counter_plan.failure_reason),
        generated_counter_plan.failure_reason.empty()
            ? "insert_select_generator_descriptor_invalid"
            : generated_counter_plan.failure_reason);
  }
  const std::size_t direct_row_count =
      DirectRequestRowCount(request, generated_counter_plan);

  const EngineInsertRowsRequest synthetic_insert =
      SyntheticInsertRequestForDirectBulk(request);
  const bool force_large_values_for_insert =
      InsertBatchOptionEnabled(synthetic_insert, "large_value.force_toast=true");
  const auto begin_batch_start = DirectSteadyClock::now();
  InsertBatchContext batch_context =
      BeginInsertBatchContext(synthetic_insert, *state, *table, visible_indexes);
  mark_descriptor_step("begin_insert_batch_context",
                       begin_batch_start,
                       DirectSteadyClock::now());
  if (!batch_context.accepted) {
    const std::string reason = batch_context.fallback_reason.empty()
                                   ? "direct_physical_batch_refused"
                                   : batch_context.fallback_reason;
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_batch_fallback_total",
                            1.0,
                            "fallback",
                            reason);
    EngineDmlSummaryCounters summary;
    AddDmlSummaryFallbackReason(&summary, reason);
    auto failure = DirectBulkFailure(request,
                                     MakeInvalidRequestDiagnostic(
                                         "dml.direct_physical_bulk_append",
                                         reason),
                                     reason,
                                     summary);
    AddInsertBatchEvidenceToResult(batch_context, &failure);
    return failure;
  }
  const auto strict_eligibility_start = DirectSteadyClock::now();
  const auto bulk_validation = ValidateStrictBulkLoadEligibility(batch_context, *table);
  mark_descriptor_step("strict_bulk_eligibility",
                       strict_eligibility_start,
                       DirectSteadyClock::now());
  if (bulk_validation.error) {
    return DirectBulkFailureWithEvidence(request,
                                         bulk_validation,
                                         "strict_bulk_load_refused",
                                         batch_context.evidence);
  }
  mark_phase("load_state_descriptor");

  DirectPhysicalBulkAppendResult result;
  result.ok = true;
  result.operation_id = "dml.direct_physical_bulk_append";
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.direct_lane_selected = true;
  result.evidence.insert(result.evidence.end(),
                         descriptor_trace.begin(),
                         descriptor_trace.end());
  AddEmbeddedTrustModeEvidence(request.context, &result);
  AddDirectLaneBaseEvidence(request, direct_row_count, &result);
  result.evidence.insert(result.evidence.end(),
                         loaded.evidence.begin(),
                         loaded.evidence.end());
  result.evidence.insert(result.evidence.end(),
                         index_only_eligibility.evidence.begin(),
                         index_only_eligibility.evidence.end());
  result.evidence.push_back({"direct_physical_bulk_index_entries_authoritative",
                             index_entries_authoritative ? "true" : "false"});
  result.evidence.push_back({"direct_physical_append_index_cache",
                             append_index_cache_hit ? "hit" : "miss"});
  if (bypass_single_window_native_bulk_cache) {
    result.evidence.push_back({"direct_physical_append_index_cache_bypass",
                               request.lane_operation == "insert_select"
                                   ? "insert_select_single_window"
                                   : "native_bulk_single_window"});
  }
  result.evidence.push_back({"direct_physical_bulk_append_context_cache",
                             bulk_context_cache_hit ? "hit" : "miss"});
  result.evidence.push_back({"relation_state_full_loads",
                             loaded.full_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_scoped_loads",
                             loaded.scoped_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_load_reason",
                             bulk_context_cache_hit
                                 ? "direct_physical_bulk_append_context_reuse"
                                 : "direct_physical_bulk_insert_target_scoped"});
  result.evidence.push_back({"relation_descriptor",
                             relation_descriptor.descriptor_uuid.canonical});
  const DirectBulkUuidBatch uuid_batch =
      BuildDirectBulkUuidBatch(request, direct_row_count);
  AddDirectBulkUuidBatchEvidence(uuid_batch, &result);
  mark_phase("uuid_batch");
  if (batch_context.page_reservation.reservation_available) {
    ++result.dml_summary.page_reservations;
  }

  ConstraintDmlValidationCache constraint_cache;
	  std::vector<CrudRowVersionRecord> staged_rows;
	  std::vector<CrudRowVersionRecord> returning_rows;
	  std::vector<std::vector<std::pair<std::string, std::string>>> logical_value_batch;
  const bool page_allocation_runtime_requested =
      DirectPageAllocationRuntimeRequested(request);
  DmlIngestionPipelineConfig ingestion_config;
  ingestion_config.context = request.context;
  ingestion_config.option_envelopes = request.option_envelopes;
  ingestion_config.state = state;
  ingestion_config.operation_id = "dml.direct_physical_bulk_append";
  ingestion_config.lane_operation = request.lane_operation;
  ingestion_config.target_table_uuid = request.target_table.uuid.canonical;
  ingestion_config.input_row_count =
      static_cast<EngineApiU64>(direct_row_count);
  ingestion_config.enable_preallocator = page_allocation_runtime_requested;
  if (request.lane_operation == "insert_select" ||
      request.lane_operation == "native_bulk") {
    ingestion_config.writer_worker_count = 2;
  }
  const bool preallocation_enqueue_enabled =
      ingestion_config.enable_preallocator;
  DmlIngestionPipeline ingestion_pipeline(std::move(ingestion_config));
  (void)ingestion_pipeline.Start();
  const bool suppress_payload_rows =
      WriteResultPolicySuppressesPayloadRows(write_result_policy);
  const bool has_default_validators =
      batch_context.row_encoder_plan.default_validator_count != 0;
  const bool has_domain_validators =
      batch_context.row_encoder_plan.domain_validator_count != 0;
  const bool has_not_null_validators =
      batch_context.row_encoder_plan.not_null_validator_count != 0;
  const bool has_check_validators =
      batch_context.row_encoder_plan.check_validator_count != 0;
  const bool has_immediate_row_validators =
      has_not_null_validators || has_check_validators;
  const bool can_use_direct_not_null_validation =
      has_not_null_validators && !has_check_validators;
  const bool can_use_ordered_row_fast_path =
      !has_default_validators &&
      !has_domain_validators &&
      !has_check_validators;
  const bool rowset_shared_shape =
      DirectOptionTruthy(request, "sblr.canonical_rowset_shared_shape");
  const bool generated_counter_direct_input =
      generated_counter_plan.ok && request.borrowed_input_rows.empty();
  const auto synchronous_indexes = DirectSynchronousIndexes(batch_context);
  const bool has_delta_ledger_indexes =
      DirectHasCommittedDeltaLedgerIndexes(batch_context);
  const bool direct_retail_exact_append_candidate =
      !DirectSortedBulkIndexBuildEnabled(request) &&
      !synchronous_indexes.empty() &&
      (synchronous_indexes.size() == 1 ||
       DirectAllIndexesUnique(synchronous_indexes));
  const bool can_use_shared_ordered_row_fast_path =
      can_use_ordered_row_fast_path &&
      rowset_shared_shape &&
      !request.borrowed_input_rows.empty() &&
      DirectInputMatchesEncoderOrder(request,
                                     request.borrowed_input_rows.front(),
                                     batch_context.row_encoder_plan);
  const bool can_use_generated_counter_row_stage_fast_path =
      generated_counter_direct_input &&
      can_use_ordered_row_fast_path &&
      generated_counter_plan.projections.size() ==
          batch_context.row_encoder_plan.columns.size();
  const std::vector<std::size_t> not_null_ordinals =
      can_use_direct_not_null_validation &&
              (can_use_shared_ordered_row_fast_path ||
               can_use_generated_counter_row_stage_fast_path)
          ? DirectNotNullValidationOrdinals(batch_context.row_encoder_plan)
          : std::vector<std::size_t>{};
  std::vector<unsigned char> not_null_ordinal_mask;
  const bool can_use_shared_row_stage_fast_path =
      can_use_shared_ordered_row_fast_path &&
      !has_default_validators &&
      !has_domain_validators &&
      !has_check_validators;
  const bool can_stage_shared_values_externally =
      can_use_shared_row_stage_fast_path;
  if (can_use_shared_row_stage_fast_path && can_use_direct_not_null_validation &&
      !request.borrowed_input_rows.empty()) {
    not_null_ordinal_mask.assign(request.borrowed_input_rows.front().fields.size(), 0);
    for (const std::size_t ordinal : not_null_ordinals) {
      if (ordinal < not_null_ordinal_mask.size()) {
        not_null_ordinal_mask[ordinal] = 1;
      }
    }
  }
  if (can_use_generated_counter_row_stage_fast_path &&
      can_use_direct_not_null_validation) {
    not_null_ordinal_mask.assign(generated_counter_plan.projections.size(), 0);
    for (const std::size_t ordinal : not_null_ordinals) {
      if (ordinal < not_null_ordinal_mask.size()) {
        not_null_ordinal_mask[ordinal] = 1;
      }
    }
  }
  staged_rows.reserve(direct_row_count);
  if (request.lane_operation == "native_bulk" &&
      !request.borrowed_input_rows.empty() &&
      !batch_context.row_encoder_plan.columns.empty()) {
    for (const auto& input_row : request.borrowed_input_rows) {
      const std::string payload_failure =
          DirectFixedWidthTypedPayloadFailure(input_row,
                                              batch_context.row_encoder_plan);
      if (!payload_failure.empty()) {
        return DirectBulkFailure(
            request,
            MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                         payload_failure),
            "typed_fixed_payload_invalid",
            result.dml_summary);
      }
    }
  }
	  if (!suppress_payload_rows) {
	    returning_rows.reserve(direct_row_count);
	  }
	  logical_value_batch.reserve(direct_row_count);
  constexpr std::size_t kDirectBulkMaxStoredInsertTraceEvents = 128;
  constexpr std::size_t kDirectBulkTraceRowsToStore =
      kDirectBulkMaxStoredInsertTraceEvents / 2;
  EngineApiU64 row_stage_prepare_us = 0;
  EngineApiU64 row_stage_convert_us = 0;
  EngineApiU64 row_stage_validation_us = 0;
  EngineApiU64 row_stage_security_recheck_us = 0;
  EngineApiU64 row_stage_value_copy_us = 0;
  EngineApiU64 row_stage_logical_batch_us = 0;
  EngineApiU64 row_stage_preallocation_enqueue_us = 0;
  EngineApiU64 row_stage_row_push_us = 0;
  const char* detailed_row_stage_trace_path =
      std::getenv("SCRATCHBIRD_DML_ROW_STAGE_TRACE");
  const bool detailed_row_stage_timing =
      detailed_row_stage_trace_path != nullptr && *detailed_row_stage_trace_path != '\0';
  const auto row_stage_timer_start = [&]() {
    return detailed_row_stage_timing ? DirectSteadyClock::now()
                                     : DirectSteadyClock::time_point{};
  };
  const auto add_row_stage_elapsed =
      [&](EngineApiU64& counter, DirectSteadyClock::time_point start) {
        if (!detailed_row_stage_timing) {
          return;
        }
        counter += DirectElapsedMicros(start, DirectSteadyClock::now());
      };
  constexpr std::size_t kDirectBulkPreallocationBatchRows = 512;
  std::vector<DmlIngestionPreallocationItem> pending_preallocation_items;
  pending_preallocation_items.reserve(
      std::min<std::size_t>(kDirectBulkPreallocationBatchRows,
                            direct_row_count));
  auto flush_preallocation_items = [&]() {
    if (pending_preallocation_items.empty()) {
      return true;
    }
    const auto preallocation_enqueue_start = row_stage_timer_start();
    const bool ok = ingestion_pipeline.EnqueuePreallocationBatch(
        std::move(pending_preallocation_items));
    add_row_stage_elapsed(row_stage_preallocation_enqueue_us,
                          preallocation_enqueue_start);
    pending_preallocation_items.clear();
    pending_preallocation_items.reserve(
        std::min<std::size_t>(kDirectBulkPreallocationBatchRows,
                              direct_row_count));
    return ok;
  };
  auto queue_preallocation_item = [&](DmlIngestionPreallocationItem item) {
    if (!preallocation_enqueue_enabled) {
      return true;
    }
    pending_preallocation_items.push_back(std::move(item));
    if (pending_preallocation_items.size() <
        kDirectBulkPreallocationBatchRows) {
      return true;
    }
    return flush_preallocation_items();
  };
  auto preallocation_failure = [&]() {
    const auto ingestion_stats = ingestion_pipeline.Fence();
    AddDmlIngestionPipelineEvidence(ingestion_stats, &result);
    const EngineApiDiagnostic diagnostic =
        ingestion_stats.failed
            ? ingestion_stats.diagnostic
            : MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                           "dml_ingestion_preallocation_refused");
    return DirectBulkFailure(request,
                             diagnostic,
                             "dml_ingestion_preallocation_refused",
                             result.dml_summary);
  };

  if (generated_counter_direct_input &&
      !can_use_generated_counter_row_stage_fast_path) {
    return DirectBulkFailure(
        request,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "insert_select_generated_direct_unsupported"),
        "insert_select_generated_direct_unsupported");
  }

  std::size_t row_ordinal = 0;
  if (generated_counter_direct_input) {
    for (std::uint64_t counter = generated_counter_plan.start;
         counter <= generated_counter_plan.limit;
         counter += generated_counter_plan.step) {
      const bool store_row_trace =
          row_ordinal < kDirectBulkTraceRowsToStore;
      if (store_row_trace) {
        AddInsertTrace(&batch_context,
                       "direct_physical_bulk.row.convert",
                       "row",
                       std::to_string(batch_context.actual_row_count));
      }

      const auto convert_start = row_stage_timer_start();
      CrudRowVersionRecord row_record;
      row_record.creator_tx = request.context.local_transaction_id;
      row_record.table_uuid = request.target_table.uuid.canonical;
      row_record.row_uuid = uuid_batch.row_uuids[row_ordinal];
      row_record.version_uuid = uuid_batch.version_uuids[row_ordinal];
	      row_record.temporary_session_uuid =
	          table->temporary ? request.context.session_uuid.canonical : "";
	      row_record.deleted = false;
	      std::vector<std::pair<std::string, std::string>> row_values;
	      row_values.reserve(generated_counter_plan.projections.size());

      std::uint64_t encoded_bytes = 0;
      std::string not_null_failure;
      for (std::size_t field_index = 0;
           field_index < generated_counter_plan.projections.size();
           ++field_index) {
        const auto& projection = generated_counter_plan.projections[field_index];
        std::string value =
            DirectGeneratedProjectionValue(projection, counter);
        if (value.empty() && projection.descriptor != "prefix_counter:") {
          return DirectBulkFailure(
              request,
              MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                           "insert_select_projection_evaluation_failed"),
              "insert_select_projection_evaluation_failed");
        }
        if (field_index < not_null_ordinal_mask.size() &&
            not_null_ordinal_mask[field_index] &&
            DirectNullValue(value)) {
          not_null_failure = projection.column_name;
          break;
	        }
	        const std::size_t value_size = value.size();
	        row_values.emplace_back(projection.column_name, std::move(value));
	        encoded_bytes += projection.column_name.size() + value_size;
      }
      add_row_stage_elapsed(row_stage_convert_us, convert_start);

      const auto validation_start = row_stage_timer_start();
      if (!not_null_failure.empty()) {
        return DirectBulkFailure(
            request,
            MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                         "not_null_validation_refused"),
            "not_null_validation_refused:" + not_null_failure);
      }
      const bool toast_required =
          encoded_bytes > batch_context.row_template.max_inline_encoded_bytes ||
          force_large_values_for_insert;
      const std::uint64_t projected_memory_bytes =
          toast_required ? batch_context.row_template.max_inline_encoded_bytes
                         : encoded_bytes;
      if (!batch_context.memory_policy.spill_allowed &&
          projected_memory_bytes >
              batch_context.memory_policy.context_budget_bytes) {
        const auto memory_validation =
            ValidateInsertBatchMemoryBudget(batch_context,
                                            projected_memory_bytes);
        return DirectBulkFailure(request,
                                 memory_validation,
                                 "insert_batch_memory_budget_refused");
      }
      add_row_stage_elapsed(row_stage_validation_us, validation_start);

      if (batch_context.row_encoder_plan.runtime_policy_recheck_count != 0) {
        const auto security_recheck_start = row_stage_timer_start();
	        std::vector<EngineEvidenceReference> security_recheck_evidence;
	        const auto security_recheck =
	            EvaluateDirectRuntimeInsertSecurityRecheck(
	                request,
	                request.target_table.uuid.canonical,
	                row_values,
	                &security_recheck_evidence);
        if (!security_recheck.ok || !security_recheck.admitted) {
          std::vector<EngineEvidenceReference> evidence = result.evidence;
          evidence.insert(evidence.end(),
                          security_recheck_evidence.begin(),
                          security_recheck_evidence.end());
          const EngineApiDiagnostic diagnostic =
              security_recheck.diagnostics.empty()
                  ? MakeInvalidRequestDiagnostic(
                        "dml.direct_physical_bulk_append",
                        "runtime_security_recheck_refused")
                  : security_recheck.diagnostics.front();
          return DirectBulkFailureWithEvidence(
              request,
              diagnostic,
              "runtime_security_recheck_refused",
              evidence,
              result.dml_summary);
        }
        result.evidence.insert(result.evidence.end(),
                               security_recheck_evidence.begin(),
                               security_recheck_evidence.end());
        add_row_stage_elapsed(row_stage_security_recheck_us,
                              security_recheck_start);
      }

      if (store_row_trace) {
        AddInsertTrace(&batch_context,
                       "direct_physical_bulk.row.stage",
                       "stage",
                       row_record.row_uuid);
	      }
	      DmlIngestionPreallocationItem preallocation_item;
	      const auto logical_batch_start = row_stage_timer_start();
	      logical_value_batch.push_back(std::move(row_values));
	      preallocation_item.borrowed_logical_values = &logical_value_batch.back();
	      add_row_stage_elapsed(row_stage_logical_batch_us, logical_batch_start);
	      preallocation_item.encoded_bytes = encoded_bytes;
      if (!queue_preallocation_item(std::move(preallocation_item))) {
        return preallocation_failure();
      }
      const auto row_push_start = row_stage_timer_start();
      staged_rows.push_back(std::move(row_record));
      add_row_stage_elapsed(row_stage_row_push_us, row_push_start);
      ++row_ordinal;
      if (generated_counter_plan.limit - counter < generated_counter_plan.step) {
        break;
      }
    }
	    result.evidence.push_back({"insert_select_generated_direct_rows",
	                               std::to_string(row_ordinal)});
	    result.evidence.push_back({"insert_select_generated_direct_route",
	                               "physical_counter_projection"});
	  } else {
  for (const auto& input_row : request.borrowed_input_rows) {
    const bool store_row_trace =
        row_ordinal < kDirectBulkTraceRowsToStore;
    if (store_row_trace) {
      AddInsertTrace(&batch_context,
                     "direct_physical_bulk.row.convert",
                     "row",
                     std::to_string(batch_context.actual_row_count));
    }

    if (can_use_shared_row_stage_fast_path) {
      const auto convert_start = row_stage_timer_start();
      const bool stage_values_externally_for_shared_rowset =
          can_stage_shared_values_externally;
      CrudRowVersionRecord row_record;
      row_record.creator_tx = request.context.local_transaction_id;
      row_record.table_uuid = request.target_table.uuid.canonical;
      row_record.row_uuid = uuid_batch.row_uuids[row_ordinal];
      row_record.version_uuid = uuid_batch.version_uuids[row_ordinal];
      row_record.temporary_session_uuid =
          table->temporary ? request.context.session_uuid.canonical : "";
      row_record.deleted = false;
      std::vector<std::pair<std::string, std::string>> external_row_values;
      auto& staged_value_target =
          stage_values_externally_for_shared_rowset ? external_row_values
                                                    : row_record.values;
      staged_value_target.reserve(input_row.fields.size());

      std::uint64_t encoded_bytes = 0;
      bool saw_default_marker = false;
      std::string not_null_failure;
      for (std::size_t field_index = 0; field_index < input_row.fields.size(); ++field_index) {
        const auto& typed = input_row.fields[field_index].second;
        const std::string& field = DirectInputFieldName(request, input_row, field_index);
        if (!typed.is_null && typed.encoded_value == "<DEFAULT>") {
          saw_default_marker = true;
          break;
        }
        if (field_index < not_null_ordinal_mask.size() &&
            not_null_ordinal_mask[field_index] &&
            (typed.is_null || DirectNullValue(typed.encoded_value))) {
          not_null_failure =
              field_index < batch_context.row_encoder_plan.columns.size()
                  ? batch_context.row_encoder_plan.columns[field_index].column_name
                  : field;
          break;
        }
        if (typed.is_null) {
          staged_value_target.emplace_back(field, kDirectNullMarker);
          encoded_bytes += field.size() + sizeof(kDirectNullMarker) - 1;
        } else {
          staged_value_target.emplace_back(field, typed.encoded_value);
          encoded_bytes += field.size() + typed.encoded_value.size();
        }
      }
      add_row_stage_elapsed(row_stage_convert_us, convert_start);
      if (!saw_default_marker) {
        const auto validation_start = row_stage_timer_start();
        if (!not_null_failure.empty()) {
          return DirectBulkFailure(
              request,
              MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                           "not_null_validation_refused"),
              "not_null_validation_refused:" + not_null_failure);
        }
        const bool toast_required =
            encoded_bytes > batch_context.row_template.max_inline_encoded_bytes ||
            force_large_values_for_insert;
        const std::uint64_t projected_memory_bytes =
            toast_required ? batch_context.row_template.max_inline_encoded_bytes
                           : encoded_bytes;
        if (!batch_context.memory_policy.spill_allowed &&
            projected_memory_bytes > batch_context.memory_policy.context_budget_bytes) {
          const auto memory_validation = ValidateInsertBatchMemoryBudget(
              batch_context,
              projected_memory_bytes);
          return DirectBulkFailure(request,
                                   memory_validation,
                                   "insert_batch_memory_budget_refused");
        }
        add_row_stage_elapsed(row_stage_validation_us, validation_start);
        if (store_row_trace) {
          AddInsertTrace(&batch_context,
                         "direct_physical_bulk.row.stage",
                         "stage",
                         row_record.row_uuid);
        }
        if (batch_context.row_encoder_plan.runtime_policy_recheck_count != 0) {
          const auto security_recheck_start = row_stage_timer_start();
          std::vector<EngineEvidenceReference> security_recheck_evidence;
          const auto security_recheck =
              EvaluateDirectRuntimeInsertSecurityRecheck(
                  request,
                  request.target_table.uuid.canonical,
                  staged_value_target,
                  &security_recheck_evidence);
          if (!security_recheck.ok || !security_recheck.admitted) {
            std::vector<EngineEvidenceReference> evidence = result.evidence;
            evidence.insert(evidence.end(),
                            security_recheck_evidence.begin(),
                            security_recheck_evidence.end());
            const EngineApiDiagnostic diagnostic =
                security_recheck.diagnostics.empty()
                    ? MakeInvalidRequestDiagnostic(
                          "dml.direct_physical_bulk_append",
                          "runtime_security_recheck_refused")
                    : security_recheck.diagnostics.front();
            return DirectBulkFailureWithEvidence(
                request,
                diagnostic,
                "runtime_security_recheck_refused",
                evidence,
                result.dml_summary);
          }
          result.evidence.insert(result.evidence.end(),
                                 security_recheck_evidence.begin(),
                                 security_recheck_evidence.end());
          add_row_stage_elapsed(row_stage_security_recheck_us,
                                security_recheck_start);
        }
        const auto logical_batch_start = row_stage_timer_start();
        if (stage_values_externally_for_shared_rowset) {
          logical_value_batch.push_back(std::move(external_row_values));
        } else {
          logical_value_batch.push_back(row_record.values);
        }
        add_row_stage_elapsed(row_stage_logical_batch_us, logical_batch_start);
        DmlIngestionPreallocationItem preallocation_item;
        preallocation_item.borrowed_logical_values = &logical_value_batch.back();
        preallocation_item.encoded_bytes = encoded_bytes;
        if (!queue_preallocation_item(std::move(preallocation_item))) {
          return preallocation_failure();
        }
        const auto row_push_start = row_stage_timer_start();
        staged_rows.push_back(std::move(row_record));
        add_row_stage_elapsed(row_stage_row_push_us, row_push_start);
        ++row_ordinal;
        continue;
      }
    }

    const auto prepare_start = row_stage_timer_start();
    PreparedInsertRow prepared =
        !request.shared_row_field_order.empty()
            ? PrepareDirectBulkSharedFieldOrderRowFast(request,
                                                       input_row,
                                                       batch_context.row_template,
                                                       uuid_batch.row_uuids[row_ordinal],
                                                       force_large_values_for_insert)
        : can_use_shared_ordered_row_fast_path
            ? PrepareDirectBulkOrderedRowFast(input_row,
                                              batch_context.row_template,
                                              uuid_batch.row_uuids[row_ordinal],
                                              force_large_values_for_insert)
            : can_use_ordered_row_fast_path &&
                DirectInputMatchesEncoderOrder(request,
                                               input_row,
                                               batch_context.row_encoder_plan)
                  ? PrepareDirectBulkOrderedRowFast(input_row,
                                                    batch_context.row_template,
                                                    uuid_batch.row_uuids[row_ordinal],
                                                    force_large_values_for_insert)
                  : PrepareInsertRowForBatch(synthetic_insert,
                                             input_row,
                                             batch_context.row_template,
                                             batch_context.row_encoder_plan);
    prepared.row_uuid = uuid_batch.row_uuids[row_ordinal];
    auto values = std::move(prepared.values);
    add_row_stage_elapsed(row_stage_prepare_us, prepare_start);

    ConstraintDmlValidationOptions direct_constraint_options;
    direct_constraint_options.validate_unique_constraints = false;
    direct_constraint_options.validate_foreign_key_constraints = false;
    bool values_mutated_by_validation = false;

    const bool default_requested =
        std::any_of(values.begin(), values.end(), [](const auto& field) {
          return field.second == "<DEFAULT>";
        });
    if (has_default_validators || default_requested) {
      const auto validation_start = row_stage_timer_start();
      const auto default_validation =
          ApplyConstraintDefaultsForInsert(request.context, *table, values);
      if (!default_validation.ok) {
        return DirectBulkFailure(request,
                                 default_validation.diagnostic,
                                 "constraint_default_refused");
      }
      values = default_validation.values;
      values_mutated_by_validation = true;
      result.evidence.insert(result.evidence.end(),
                             default_validation.evidence.begin(),
                             default_validation.evidence.end());
      add_row_stage_elapsed(row_stage_validation_us, validation_start);
    }

    if (has_domain_validators) {
      const auto validation_start = row_stage_timer_start();
      const auto domain_validation = ApplyDomainRulesToCrudValues(
          request.context,
          table->columns,
          values,
          request.context.local_transaction_id,
          &constraint_cache);
      if (!domain_validation.ok) {
        return DirectBulkFailure(request,
                                 domain_validation.diagnostic,
                                 "domain_validation_refused");
      }
      values = domain_validation.values;
      values_mutated_by_validation = true;
      result.evidence.insert(result.evidence.end(),
                             domain_validation.evidence.begin(),
                             domain_validation.evidence.end());
      add_row_stage_elapsed(row_stage_validation_us, validation_start);
    }

    if (can_use_direct_not_null_validation) {
      const auto validation_start = row_stage_timer_start();
      const std::string not_null_failure =
          can_use_shared_ordered_row_fast_path
              ? DirectNotNullValidationFailureOrdered(
                    batch_context.row_encoder_plan, values, not_null_ordinals)
              : DirectNotNullValidationFailure(batch_context.row_encoder_plan,
                                               values);
      if (!not_null_failure.empty()) {
        return DirectBulkFailure(
            request,
            MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                         "not_null_validation_refused"),
            "not_null_validation_refused:" + not_null_failure);
      }
      add_row_stage_elapsed(row_stage_validation_us, validation_start);
    } else if (has_immediate_row_validators) {
      const auto validation_start = row_stage_timer_start();
      const auto constraint_validation = ValidateImmediateRowConstraintsWithOptions(
          request.context,
          *state,
          *table,
          prepared.row_uuid,
          values,
          "insert",
          direct_constraint_options,
          &constraint_cache);
      if (!constraint_validation.ok) {
        return DirectBulkFailure(request,
                                 constraint_validation.diagnostic,
                                 "constraint_validation_refused");
      }
      values = constraint_validation.values;
      values_mutated_by_validation = true;
      result.evidence.insert(result.evidence.end(),
                             constraint_validation.evidence.begin(),
                             constraint_validation.evidence.end());
      add_row_stage_elapsed(row_stage_validation_us, validation_start);
    }

    if (batch_context.row_encoder_plan.runtime_policy_recheck_count != 0) {
      const auto security_recheck_start = row_stage_timer_start();
      std::vector<EngineEvidenceReference> security_recheck_evidence;
      const auto security_recheck =
          EvaluateDirectRuntimeInsertSecurityRecheck(
              request,
              request.target_table.uuid.canonical,
              values,
              &security_recheck_evidence);
      if (!security_recheck.ok || !security_recheck.admitted) {
        std::vector<EngineEvidenceReference> evidence = result.evidence;
        evidence.insert(evidence.end(),
                        security_recheck_evidence.begin(),
                        security_recheck_evidence.end());
        const EngineApiDiagnostic diagnostic =
            security_recheck.diagnostics.empty()
                ? MakeInvalidRequestDiagnostic(
                      "dml.direct_physical_bulk_append",
                      "runtime_security_recheck_refused")
                : security_recheck.diagnostics.front();
        return DirectBulkFailureWithEvidence(
            request,
            diagnostic,
            "runtime_security_recheck_refused",
            evidence,
            result.dml_summary);
      }
      result.evidence.insert(result.evidence.end(),
                             security_recheck_evidence.begin(),
                             security_recheck_evidence.end());
      add_row_stage_elapsed(row_stage_security_recheck_us,
                            security_recheck_start);
    }

    prepared.values = std::move(values);
    if (values_mutated_by_validation) {
      prepared.encoded_bytes =
          static_cast<EngineApiU64>(EncodedValueBytes(prepared.values));
      prepared.toast_required =
          prepared.encoded_bytes > batch_context.row_template.max_inline_encoded_bytes ||
          force_large_values_for_insert;
    }
    const std::uint64_t projected_memory_bytes =
        prepared.toast_required ? batch_context.row_template.max_inline_encoded_bytes
                                : prepared.encoded_bytes;
    if (!batch_context.memory_policy.spill_allowed &&
        projected_memory_bytes > batch_context.memory_policy.context_budget_bytes) {
      const auto validation_start = row_stage_timer_start();
      const auto memory_validation = ValidateInsertBatchMemoryBudget(
          batch_context,
          projected_memory_bytes);
      add_row_stage_elapsed(row_stage_validation_us, validation_start);
      return DirectBulkFailure(request,
                               memory_validation,
                               "insert_batch_memory_budget_refused");
    }
    const auto validation_start = row_stage_timer_start();
    const auto batch_constraint =
        ValidateInsertBatchConstraints(batch_context, *state, prepared);
    if (batch_constraint.error) {
      return DirectBulkFailure(request,
                               batch_constraint,
                               "insert_batch_constraint_refused");
    }
    add_row_stage_elapsed(row_stage_validation_us, validation_start);

    CrudRowVersionRecord row_record;
    row_record.creator_tx = request.context.local_transaction_id;
    row_record.table_uuid = request.target_table.uuid.canonical;
    row_record.row_uuid = prepared.row_uuid;
    row_record.version_uuid = uuid_batch.version_uuids[row_ordinal];
    row_record.temporary_session_uuid =
        table->temporary ? request.context.session_uuid.canonical : "";
    row_record.deleted = false;
    const auto value_copy_start = row_stage_timer_start();
    row_record.values = prepared.values;
    add_row_stage_elapsed(row_stage_value_copy_us, value_copy_start);
    if (store_row_trace) {
      AddInsertTrace(&batch_context,
                     "direct_physical_bulk.row.stage",
                     "stage",
                     prepared.row_uuid);
    }
    const auto row_push_start = row_stage_timer_start();
    staged_rows.push_back(std::move(row_record));
    add_row_stage_elapsed(row_stage_row_push_us, row_push_start);
    const auto logical_batch_start = row_stage_timer_start();
    logical_value_batch.push_back(std::move(prepared.values));
    add_row_stage_elapsed(row_stage_logical_batch_us, logical_batch_start);
    DmlIngestionPreallocationItem preallocation_item;
    preallocation_item.borrowed_logical_values = &logical_value_batch.back();
    preallocation_item.encoded_bytes = prepared.encoded_bytes;
    if (!queue_preallocation_item(std::move(preallocation_item))) {
      return preallocation_failure();
    }
    ++row_ordinal;
  }
  }
  if (!flush_preallocation_items()) {
    return preallocation_failure();
  }
  if (direct_row_count > kDirectBulkTraceRowsToStore) {
    const std::uint64_t omitted =
        static_cast<std::uint64_t>(
            direct_row_count - kDirectBulkTraceRowsToStore) *
        2u;
    batch_context.trace_event_count += omitted;
    batch_context.trace_event_compacted_count += omitted;
  }
  mark_phase("row_stage_validate");
  if (detailed_row_stage_timing) {
    phase_micros.push_back({"row_stage.prepare", row_stage_prepare_us});
    phase_micros.push_back({"row_stage.convert", row_stage_convert_us});
    phase_micros.push_back({"row_stage.validation", row_stage_validation_us});
    phase_micros.push_back({"row_stage.security_recheck",
                            row_stage_security_recheck_us});
    phase_micros.push_back({"row_stage.value_copy", row_stage_value_copy_us});
    phase_micros.push_back({"row_stage.logical_batch",
                            row_stage_logical_batch_us});
    phase_micros.push_back({"row_stage.preallocation_enqueue",
                            row_stage_preallocation_enqueue_us});
    phase_micros.push_back({"row_stage.row_push", row_stage_row_push_us});
  }
  if (can_stage_shared_values_externally) {
    result.evidence.push_back({"shared_rowset_value_batch_ownership",
                               "external_logical_batch"});
  }
  if (request.lane_operation == "native_bulk" &&
      can_stage_shared_values_externally) {
    result.evidence.push_back({"native_bulk_value_batch_ownership",
                               "external_logical_batch"});
  }

  std::map<std::string, std::set<std::string>> append_index_key_cache;
  std::map<std::string, std::map<std::string, CrudIndexEntryRecord>>
      append_index_entry_key_cache;
	  if (index_entries_authoritative && append_index_cache_hit) {
	    DirectBuildAppendIndexConflictCaches(request.context,
	                                         request.target_table.uuid.canonical,
	                                         index_only_eligibility.row_version_count,
	                                         visible_indexes,
                                         logical_value_batch,
                                         &append_index_key_cache,
                                         sorted_bulk_index_requested
                                             ? &append_index_entry_key_cache
                                             : nullptr);
  }

  DirectPrecomputedIndexEntryMap direct_precomputed_index_entries;
  DirectTypedIndexKeyStats direct_typed_index_key_stats;
  if (direct_retail_exact_append_candidate) {
    bool generated_counter_index_precomputed = false;
    if (generated_counter_direct_input) {
      generated_counter_index_precomputed =
          DirectBuildGeneratedCounterLexicographicIndexEntries(
              synchronous_indexes,
              generated_counter_plan,
              uuid_batch,
              &direct_precomputed_index_entries);
    }
    if (generated_counter_index_precomputed) {
      result.evidence.push_back({"insert_select_generated_index_precompute",
                                 "counter_lexicographic"});
    } else {
		      const bool shared_typed_input_rows =
		          !request.shared_row_field_order.empty() &&
		          request.borrowed_input_rows.size() == staged_rows.size() &&
		          DirectSharedFieldOrderMatchesEncoderOrder(
		              request.shared_row_field_order,
		              batch_context.row_encoder_plan);
		      direct_precomputed_index_entries =
		          DirectPrecomputeIndexEntries(synchronous_indexes,
		                                       staged_rows,
		                                       logical_value_batch,
		                                       shared_typed_input_rows
		                                           ? request.borrowed_input_rows
		                                           : std::span<const EngineRowValue>{},
		                                       shared_typed_input_rows
		                                           ? &batch_context.row_encoder_plan
		                                           : nullptr,
		                                       &direct_typed_index_key_stats);
    }
    if (direct_typed_index_key_stats.typed_key_candidates != 0) {
      result.evidence.push_back(
          {"direct_index_key_typed_candidates",
           std::to_string(direct_typed_index_key_stats.typed_key_candidates)});
      result.evidence.push_back(
          {"direct_index_key_typed_encoded",
           std::to_string(direct_typed_index_key_stats.typed_key_encoded)});
      result.evidence.push_back(
          {"direct_index_key_typed_fallback",
           std::to_string(direct_typed_index_key_stats.typed_key_fallback)});
      result.evidence.push_back(
          {"direct_index_key_sbkohex_keys",
           std::to_string(direct_typed_index_key_stats.sbkohex_keys)});
    }
    mark_phase("index_exact_key_precompute");
  }

  const bool generated_empty_target_constraint_fast_path =
      generated_counter_direct_input &&
      index_only_eligibility.row_version_count == 0 &&
      !direct_precomputed_index_entries.empty() &&
      DirectGeneratedEmptyTargetConstraintProofEligible(
          *table,
          visible_indexes,
          direct_precomputed_index_entries);
  if (generated_empty_target_constraint_fast_path) {
    result.evidence.push_back({"bulk_constraint_proof_route_selected",
                               "direct_physical_bulk.generated_empty_target"});
    result.evidence.push_back({"bulk_constraint_proof_direct_physical_bulk",
                               "true"});
    result.evidence.push_back({"bulk_unique_proof_incoming_presorted", "true"});
    result.evidence.push_back({"bulk_unique_proof_presorted_order_valid",
                               "true"});
    result.evidence.push_back({"bulk_unique_proof_duplicate_run_absent",
                               "true"});
    result.evidence.push_back({"bulk_unique_proof_visible_keys", "0"});
    result.evidence.push_back({"bulk_constraint_proof_result", "accepted"});
    result.evidence.push_back({"mga_finality_authority",
                               "engine_transaction_inventory"});
    result.evidence.push_back({"parser_finality_authority", "false"});
  } else {
    const auto constraint_proof = BuildDirectBulkConstraintProof(
        request,
        *state,
        *table,
        visible_indexes,
        staged_rows,
        logical_value_batch,
        index_entries_authoritative,
        append_index_key_cache.empty() ? nullptr : &append_index_key_cache,
        direct_precomputed_index_entries.empty()
            ? nullptr
            : &direct_precomputed_index_entries);
    if (!constraint_proof.ok) {
      return DirectBulkFailureWithEvidence(request,
                                           constraint_proof.diagnostic,
                                           constraint_proof.failure_reason,
                                           constraint_proof.evidence,
                                           result.dml_summary);
    }
    result.evidence.insert(result.evidence.end(),
                           constraint_proof.evidence.begin(),
                           constraint_proof.evidence.end());
  }
  result.dml_summary.index_probes +=
      DirectUniqueIndexProbeCount(visible_indexes, staged_rows.size());

  const auto sorted_index_build = BuildDirectSortedBulkIndexArtifacts(
      request,
      *state,
      synchronous_indexes,
      staged_rows,
      logical_value_batch,
      index_entries_authoritative,
      append_index_entry_key_cache.empty() ? nullptr
                                           : &append_index_entry_key_cache);
  if (!sorted_index_build.ok) {
    auto evidence = result.evidence;
    evidence.insert(evidence.end(),
                    sorted_index_build.evidence.begin(),
                    sorted_index_build.evidence.end());
    return DirectBulkFailureWithEvidence(request,
                                         sorted_index_build.diagnostic,
                                         sorted_index_build.failure_reason,
                                         evidence,
                                         result.dml_summary);
  }
  result.evidence.insert(result.evidence.end(),
                         sorted_index_build.evidence.begin(),
                         sorted_index_build.evidence.end());

	  const bool direct_retail_exact_append_path =
	      !sorted_index_build.retail_indexes.empty() &&
	      (sorted_index_build.retail_indexes.size() == 1 ||
	       DirectAllIndexesUnique(sorted_index_build.retail_indexes));

	  auto ordered_ingest =
	      ApplyDirectOrderedIngestPlan(request, &staged_rows, &logical_value_batch);
	  if (!ordered_ingest.ok) {
	    return DirectBulkFailureWithEvidence(request,
	                                         ordered_ingest.diagnostic,
	                                         ordered_ingest.failure_reason,
	                                         ordered_ingest.evidence,
	                                         result.dml_summary);
	  }
	  result.evidence.insert(result.evidence.end(),
	                         ordered_ingest.evidence.begin(),
	                         ordered_ingest.evidence.end());
  mark_phase("constraint_index_plan");

  const auto ingestion_prealloc_stats = ingestion_pipeline.FencePreallocator();
  if (ingestion_prealloc_stats.failed) {
    AddDmlIngestionPipelineEvidence(ingestion_prealloc_stats, &result);
    return DirectBulkFailure(request,
                             ingestion_prealloc_stats.diagnostic,
                             "dml_ingestion_preallocation_refused",
                             result.dml_summary);
  }
  for (const auto& record : ingestion_prealloc_stats.allocations) {
    if (!record.allocation.active) {
      continue;
    }
    if (record.family == "row" || record.family == "row_source_size_hint") {
      AddDirectAllocationResourceSummary(record.allocation,
                                         "row",
                                         record.row_count,
                                         record.elapsed_microseconds,
                                         batch_context,
                                         false,
                                         &result);
    } else if (record.family == "index") {
      AddDirectAllocationResourceSummary(record.allocation,
                                         "index",
                                         record.row_count,
                                         record.elapsed_microseconds,
                                         batch_context,
                                         false,
                                         &result);
    }
  }
  const bool ingestion_row_capacity_ready =
      !page_allocation_runtime_requested ||
      ingestion_prealloc_stats.row_prework_rows >=
      static_cast<EngineApiU64>(staged_rows.size());
  result.evidence.push_back({"dml_ingestion_row_capacity_ready",
                             ingestion_row_capacity_ready ? "true" : "false"});

  DmlPageAllocationRuntimeResult row_allocation;
  EngineApiU64 row_allocation_elapsed = 0;
  if (!ingestion_row_capacity_ready) {
    const auto row_allocation_start = DirectSteadyClock::now();
    row_allocation = ReserveDmlPageAllocationRuntime(
        request.context,
        request.option_envelopes,
        request.target_table.uuid.canonical,
        DmlPageAllocationRuntimeFamily::row_data,
        static_cast<std::uint64_t>(staged_rows.size()),
        "direct_physical_bulk.row_data");
    row_allocation_elapsed =
        DirectElapsedMicros(row_allocation_start, DirectSteadyClock::now());
    if (!row_allocation.ok()) {
      AddDirectAllocationResourceSummary(
          row_allocation,
          "row",
          static_cast<EngineApiU64>(staged_rows.size()),
          row_allocation_elapsed,
          batch_context,
          false,
          &result);
      auto evidence = result.evidence;
      evidence.insert(evidence.end(),
                      row_allocation.evidence.begin(),
                      row_allocation.evidence.end());
      const std::string reason =
          DirectPageExtentPreallocationRequired(request)
              ? RequiredPreallocationFailureReason(row_allocation, "row")
              : std::string{};
      EngineDmlSummaryCounters summary = result.dml_summary;
      AddPreallocationRuntimeCounters(row_allocation, &summary);
      if (!reason.empty()) {
        summary.preallocation_refused = std::max<EngineApiU64>(
            summary.preallocation_refused,
            1);
      }
      return DirectBulkFailureWithEvidence(
          request,
          row_allocation.diagnostic,
          reason.empty() ? "row_page_allocation_refused" : reason,
          evidence,
          summary);
    }
    AddDmlPageAllocationRuntimeEvidence(row_allocation, &result);
    AddDirectAllocationResourceSummary(
        row_allocation,
        "row",
        static_cast<EngineApiU64>(staged_rows.size()),
        row_allocation_elapsed,
        batch_context,
        !DirectPageExtentPreallocationRequired(request),
        &result);
    if (row_allocation.active) {
      ++result.dml_summary.page_reservations;
    }
    if (DirectPageExtentPreallocationRequired(request)) {
      const std::string reason =
          RequiredPreallocationFailureReason(row_allocation, "row");
      if (!reason.empty()) {
        EngineDmlSummaryCounters summary = result.dml_summary;
        AddPreallocationRuntimeCounters(row_allocation, &summary);
        summary.preallocation_refused = std::max<EngineApiU64>(
            summary.preallocation_refused,
            1);
        return DirectBulkFailureWithEvidence(
            request,
            MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append", reason),
            reason,
            result.evidence,
            summary);
      }
    }
  } else if (DirectPageExtentPreallocationRequired(request)) {
    result.evidence.push_back({"page_extent_preallocation_required_satisfied_by",
                               "dml_ingestion_preallocator"});
  }
  mark_phase("row_page_allocation");

  const bool ingestion_index_capacity_ready =
      !page_allocation_runtime_requested ||
      ingestion_prealloc_stats.index_prework_rows >=
      static_cast<EngineApiU64>(staged_rows.size());
  result.evidence.push_back({"dml_ingestion_index_capacity_ready",
                             ingestion_index_capacity_ready ? "true" : "false"});

  DmlPageAllocationRuntimeResult index_allocation;
  EngineApiU64 index_allocation_elapsed = 0;
  if (!ingestion_index_capacity_ready) {
    const auto index_allocation_start = DirectSteadyClock::now();
    index_allocation = ReserveDmlIndexPageAllocationRuntimeForRows(
        request.context,
        request.option_envelopes,
        *state,
        request.target_table.uuid.canonical,
        logical_value_batch,
        "direct_physical_bulk.index");
    index_allocation_elapsed =
        DirectElapsedMicros(index_allocation_start, DirectSteadyClock::now());
    if (!index_allocation.ok()) {
      AddDirectAllocationResourceSummary(index_allocation,
                                         "index",
                                         0,
                                         index_allocation_elapsed,
                                         batch_context,
                                         false,
                                         &result);
      auto evidence = result.evidence;
      evidence.insert(evidence.end(),
                      index_allocation.evidence.begin(),
                      index_allocation.evidence.end());
      const std::string reason =
          DirectPageExtentPreallocationRequired(request)
              ? RequiredPreallocationFailureReason(index_allocation, "index")
              : std::string{};
      EngineDmlSummaryCounters summary = result.dml_summary;
      AddPreallocationRuntimeCounters(index_allocation, &summary);
      if (!reason.empty()) {
        summary.preallocation_refused = std::max<EngineApiU64>(
            summary.preallocation_refused,
            1);
      }
      return DirectBulkFailureWithEvidence(
          request,
          index_allocation.diagnostic,
          reason.empty() ? "index_page_allocation_refused" : reason,
          evidence,
          summary);
    }
    AddDmlPageAllocationRuntimeEvidence(index_allocation, &result);
    AddDirectAllocationResourceSummary(index_allocation,
                                       "index",
                                       0,
                                       index_allocation_elapsed,
                                       batch_context,
                                       !DirectPageExtentPreallocationRequired(request),
                                       &result);
    if (index_allocation.active) {
      ++result.dml_summary.page_reservations;
    }
    if (DirectPageExtentPreallocationRequired(request)) {
      const std::string reason =
          RequiredPreallocationFailureReason(index_allocation, "index");
      if (!reason.empty()) {
        EngineDmlSummaryCounters summary = result.dml_summary;
        AddPreallocationRuntimeCounters(index_allocation, &summary);
        summary.preallocation_refused = std::max<EngineApiU64>(
            summary.preallocation_refused,
            1);
        return DirectBulkFailureWithEvidence(
            request,
            MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append", reason),
            reason,
            result.evidence,
            summary);
      }
      AddRequiredPreallocationSummary(row_allocation,
                                      index_allocation,
                                      staged_rows.size(),
                                      &result);
    }
  } else if (DirectPageExtentPreallocationRequired(request)) {
    result.evidence.push_back({"index_extent_preallocation_required_satisfied_by",
                               "dml_ingestion_preallocator"});
  }
  mark_phase("index_page_allocation");

	  bool large_value_persistence_required = false;
	  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
	    const EngineApiU64 encoded_bytes =
	        static_cast<EngineApiU64>(EncodedValueBytes(logical_value_batch[index]));
	    if (force_large_values_for_insert ||
	        encoded_bytes > kCrudVerticalSliceMaxEncodedValueBytes) {
	      large_value_persistence_required = true;
	      break;
	    }
  }
  if (large_value_persistence_required) {
    std::vector<std::vector<std::pair<std::string, std::string>>>
        storage_value_batch;
	    storage_value_batch.reserve(staged_rows.size());
	    for (std::size_t index = 0; index < staged_rows.size(); ++index) {
	      storage_value_batch.push_back(logical_value_batch[index]);
	    }
    std::vector<MgaLargeValuePersistBatchRowInput> large_value_rows;
    large_value_rows.reserve(staged_rows.size());
    for (std::size_t index = 0; index < staged_rows.size(); ++index) {
      large_value_rows.push_back(
          {request.target_table.uuid.canonical,
           staged_rows[index].row_uuid,
           staged_rows[index].version_uuid,
           force_large_values_for_insert ||
               EncodedValueBytes(storage_value_batch[index]) >
                   kCrudVerticalSliceMaxEncodedValueBytes,
           &storage_value_batch[index]});
    }
    MgaLargeValuePersistBatchCounters large_value_counters;
    const auto large_value_persisted = PersistMgaLargeValuesForRows(
        request.context,
        large_value_rows,
        &large_value_counters,
        &result.evidence);
    if (large_value_persisted.error) {
      return DirectBulkFailure(request,
                               large_value_persisted,
                               "large_value_persistence_refused");
    }
    for (std::size_t index = 0; index < staged_rows.size(); ++index) {
      staged_rows[index].values = std::move(storage_value_batch[index]);
    }
    result.evidence.push_back({"mga_large_value_batch_rows_seen",
                               std::to_string(large_value_counters.rows_seen)});
  }
  mark_phase("large_value_persist");

	  DirectPhysicalMgaCowWriteResult row_page_write;
		  const auto* external_write_value_batch =
          (generated_counter_direct_input || can_stage_shared_values_externally) &&
		              !large_value_persistence_required
		          ? &logical_value_batch
		          : nullptr;
  MgaRelationHotAppendContext hot_append(request.context);
  std::vector<std::uint64_t> written_event_sequences;
  EngineApiU64 row_stream_append_us = 0;
  EngineApiU64 row_stream_flush_us = 0;
  std::string ingestion_write_failure_reason;
  std::mutex ingestion_write_failure_mutex;
  auto set_ingestion_write_failure_reason = [&](std::string reason) {
    std::lock_guard<std::mutex> lock(ingestion_write_failure_mutex);
    if (ingestion_write_failure_reason.empty()) {
      ingestion_write_failure_reason = std::move(reason);
    }
  };
  auto current_ingestion_write_failure_reason = [&]() {
    std::lock_guard<std::mutex> lock(ingestion_write_failure_mutex);
    return ingestion_write_failure_reason;
  };
  const auto ok_ingestion_write =
      []() {
        return MakeEngineApiDiagnostic("SB_ENGINE_API_OK",
                                       "engine.api.ok",
                                       {},
                                       false);
      };
  const auto rows_appended =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  bool row_append_fault_injected = false;
  const bool overlap_row_page_and_row_stream =
      (request.lane_operation == "insert_select" ||
       request.lane_operation == "native_bulk") &&
      !batch_context.strict_bulk_load_selected &&
      !IparFaultPointRequested(request.option_envelopes, "row_append");
  const bool native_bulk_scoped_only_row_stream =
      request.lane_operation == "native_bulk" &&
      !table->temporary &&
      !batch_context.strict_bulk_load_selected;
  result.evidence.push_back({"mga_row_append_storage_scope",
                             native_bulk_scoped_only_row_stream
                                 ? "scoped_only"
                                 : "global_and_scoped"});
  auto enqueue_row_stream_append = [&](bool immutable_rows) {
    return ingestion_pipeline.EnqueueWrite(
        {"mga_row_append_flush",
         static_cast<EngineApiU64>(staged_rows.size()),
         [&, immutable_rows]() {
           if (IparFaultPointRequested(request.option_envelopes, "row_append")) {
             row_append_fault_injected = true;
             set_ingestion_write_failure_reason(
                 "ipar_fault_injection_row_append");
             return IparFaultDiagnostic("dml.direct_physical_bulk_append",
                                        "row_append",
                                        "phase=direct_physical_row_append");
           }
	           const auto append_start = DirectSteadyClock::now();
	           const auto appended =
	               immutable_rows
	                   ? (native_bulk_scoped_only_row_stream
	                          ? hot_append.AppendRowVersionsReadOnlyScopedOnly(
	                                staged_rows,
	                                external_write_value_batch)
	                          : hot_append.AppendRowVersionsReadOnly(
	                                staged_rows,
	                                external_write_value_batch))
                   : hot_append.AppendRowVersions(&staged_rows,
                                                  external_write_value_batch,
                                                  &written_event_sequences);
           row_stream_append_us =
               DirectElapsedMicros(append_start, DirectSteadyClock::now());
           if (appended.error) {
             set_ingestion_write_failure_reason("mga_row_append_refused");
             return appended;
           }
           const auto flush_start = DirectSteadyClock::now();
           const auto flushed = hot_append.FlushRowVersions();
           row_stream_flush_us =
               DirectElapsedMicros(flush_start, DirectSteadyClock::now());
           if (flushed.error) {
             set_ingestion_write_failure_reason("mga_row_flush_refused");
             return flushed;
           }
           return rows_appended;
         }});
  };
	  if (!ingestion_pipeline.EnqueueWrite(
	          {"physical_cow_write",
	           static_cast<EngineApiU64>(staged_rows.size()),
	           [&]() {
	             row_page_write =
	                 WriteDirectPhysicalMgaCowRows(request,
	                                               staged_rows,
	                                               external_write_value_batch,
	                                               uuid_batch.caller_row_uuids == 0,
	                                               &batch_context.row_encoder_plan);
	             if (!row_page_write.ok && DirectPhysicalMgaCowRequired(request)) {
	               set_ingestion_write_failure_reason(
	                   "physical_mga_cow_row_page_refused");
               return row_page_write.diagnostic;
             }
             return ok_ingestion_write();
           }})) {
    const auto ingestion_stats = ingestion_pipeline.Fence();
    AddDmlIngestionPipelineEvidence(ingestion_stats, &result);
    return DirectBulkFailure(request,
                             ingestion_stats.failed
                                 ? ingestion_stats.diagnostic
                                 : MakeInvalidRequestDiagnostic(
                                       "dml.direct_physical_bulk_append",
                                       "dml_ingestion_writer_refused"),
                             current_ingestion_write_failure_reason().empty()
                                 ? "dml_ingestion_writer_refused"
                                 : current_ingestion_write_failure_reason(),
                             result.dml_summary);
  }
  if (overlap_row_page_and_row_stream &&
      !enqueue_row_stream_append(true)) {
    const auto ingestion_stats = ingestion_pipeline.Fence();
    AddDmlIngestionPipelineEvidence(ingestion_stats, &result);
    return DirectBulkFailure(request,
                             ingestion_stats.failed
                                 ? ingestion_stats.diagnostic
                                 : MakeInvalidRequestDiagnostic(
                                       "dml.direct_physical_bulk_append",
                                       "dml_ingestion_writer_refused"),
                             current_ingestion_write_failure_reason().empty()
                                 ? "dml_ingestion_writer_refused"
                                 : current_ingestion_write_failure_reason(),
                             result.dml_summary);
  }
  const auto cow_writer_stats = ingestion_pipeline.DrainWriters();
  result.evidence.insert(result.evidence.end(),
                         row_page_write.evidence.begin(),
                         row_page_write.evidence.end());
  if (cow_writer_stats.failed) {
    AddDmlIngestionPipelineEvidence(cow_writer_stats, &result);
    return DirectBulkFailureWithEvidence(
        request,
        cow_writer_stats.diagnostic,
        current_ingestion_write_failure_reason().empty()
            ? "dml_ingestion_writer_refused"
            : current_ingestion_write_failure_reason(),
        result.evidence,
        result.dml_summary);
  }
  if (!row_page_write.ok) {
    result.evidence.push_back({"direct_physical_bulk_row_page_writer",
                               "fallback"});
    result.evidence.push_back({"direct_physical_bulk_row_page_fallback_reason",
                               row_page_write.diagnostic.detail});
  }
  mark_phase("physical_cow_write");

  auto strict_lifecycle = RunDirectStrictBulkLifecycle(
      request,
      batch_context,
      staged_rows,
      logical_value_batch);
  if (!strict_lifecycle.ok) {
    return DirectBulkFailureWithEvidence(request,
                                         strict_lifecycle.diagnostic,
                                         strict_lifecycle.failure_reason,
                                         strict_lifecycle.evidence,
                                         result.dml_summary);
  }
  if (strict_lifecycle.active) {
    result.evidence.insert(result.evidence.end(),
                           strict_lifecycle.evidence.begin(),
                           strict_lifecycle.evidence.end());
    result.evidence.push_back({"strict_bulk_load_direct_lane", "enabled"});
    result.evidence.push_back({"strict_bulk_load_direct_lane_id",
                               TypedUuidText(strict_lifecycle.bulk_load_id)});
  }
  mark_phase("strict_bulk_lifecycle");

  if (strict_lifecycle.active &&
      DirectOptionEnabled(request,
                          "strict_bulk_load.simulate_physical_publication_failure_after_evidence=true")) {
    return DirectStrictPhysicalPublicationFailure(
        request,
        &strict_lifecycle,
        result,
        MakeInvalidRequestDiagnostic("dml.direct_physical_bulk_append",
                                     "strict_bulk_load_physical_publication_row_append_failed"),
        "strict_bulk_load_physical_publication_failed",
        "row_append");
  }
  if (!overlap_row_page_and_row_stream &&
      !enqueue_row_stream_append(false)) {
    const auto ingestion_stats = ingestion_pipeline.Fence();
    AddDmlIngestionPipelineEvidence(ingestion_stats, &result);
    return DirectBulkFailure(request,
                             ingestion_stats.failed
                                 ? ingestion_stats.diagnostic
                                 : MakeInvalidRequestDiagnostic(
                                       "dml.direct_physical_bulk_append",
                                       "dml_ingestion_writer_refused"),
                             current_ingestion_write_failure_reason().empty()
                                 ? "dml_ingestion_writer_refused"
                                 : current_ingestion_write_failure_reason(),
                             result.dml_summary);
  }
  const auto ingestion_writer_stats = ingestion_pipeline.Fence();
  AddDmlIngestionPipelineEvidence(ingestion_writer_stats, &result);
  if (ingestion_writer_stats.failed) {
    if (row_append_fault_injected) {
      std::vector<EngineEvidenceReference> evidence = result.evidence;
      AppendIparFaultEvidence(&evidence,
                              "row_append",
                              "rollback_required_before_direct_physical_row_append");
      return DirectBulkFailureWithEvidence(
          request,
          ingestion_writer_stats.diagnostic,
          "ipar_fault_injection_row_append",
          evidence,
          result.dml_summary);
    }
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(request,
                                                    &strict_lifecycle,
                                                    result,
                                                    ingestion_writer_stats.diagnostic,
                                                    current_ingestion_write_failure_reason().empty()
                                                        ? "mga_row_append_refused"
                                                        : current_ingestion_write_failure_reason(),
                                                    current_ingestion_write_failure_reason() ==
                                                            "mga_row_flush_refused"
                                                        ? "row_flush"
                                                        : "row_append");
    }
  return DirectBulkFailure(request,
                           ingestion_writer_stats.diagnostic,
                           current_ingestion_write_failure_reason().empty()
                               ? "mga_row_append_refused"
                               : current_ingestion_write_failure_reason());
  }
  mark_phase("row_stream_append_flush");
  phase_micros.push_back({"row_stream_append", row_stream_append_us});
  phase_micros.push_back({"row_stream_flush", row_stream_flush_us});

  std::vector<MgaIndexEntryRowInput> index_rows;
  if (!direct_retail_exact_append_path) {
    index_rows.reserve(staged_rows.size());
	  }
	  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> delta_entries;
	  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    const auto& row = staged_rows[index];
    if (index < kDirectBulkTraceRowsToStore) {
      AddInsertTrace(&batch_context,
                     "direct_physical_bulk.row.write",
                     "write",
                     row.row_uuid);
      AddInsertTrace(&batch_context,
                     "direct_physical_bulk.index.maintain",
                     "index",
                     row.row_uuid);
    }
    if (!direct_retail_exact_append_path) {
      index_rows.push_back({row.row_uuid,
                            row.version_uuid,
                            logical_value_batch[index]});
    }
    if (has_delta_ledger_indexes) {
      auto row_delta_entries =
          DirectDeltaEntries(batch_context, row, logical_value_batch[index]);
      delta_entries.insert(delta_entries.end(),
                           std::make_move_iterator(row_delta_entries.begin()),
                           std::make_move_iterator(row_delta_entries.end()));
    }
  }
  if (staged_rows.size() > kDirectBulkTraceRowsToStore) {
    const std::uint64_t omitted =
        static_cast<std::uint64_t>(
            staged_rows.size() - kDirectBulkTraceRowsToStore) *
        2u;
    batch_context.trace_event_count += omitted;
    batch_context.trace_event_compacted_count += omitted;
  }

  const auto delta_appended = AppendMgaSecondaryIndexDeltaLedgerEntries(
      request.context,
      delta_entries,
      &result.evidence);
  if (delta_appended.error) {
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(
          request,
          &strict_lifecycle,
          result,
          delta_appended,
          "secondary_index_delta_append_refused",
          "secondary_index_delta_append");
    }
    return DirectBulkFailure(request,
                             delta_appended,
                             "secondary_index_delta_append_refused");
  }
  mark_phase("index_delta_stage");

  if (!sorted_index_build.exact_batches.empty()) {
    const auto exact_appended = hot_append.AppendExactIndexEntryBatches(
        sorted_index_build.exact_batches);
    if (exact_appended.error) {
      if (strict_lifecycle.active) {
        return DirectStrictPhysicalPublicationFailure(request,
                                                      &strict_lifecycle,
                                                      result,
                                                      exact_appended,
                                                      "mga_sorted_index_append_refused",
                                                      "sorted_index_append");
      }
      return DirectBulkFailure(request,
                               exact_appended,
                               "mga_sorted_index_append_refused");
    }
    result.evidence.push_back({"sorted_bulk_index_exact_append",
                               "mga_index_append_path"});
    if (DirectDeferredIndexBenchmarkCleanRequired(request) &&
        sorted_index_build.selected &&
        sorted_index_build.retail_indexes.empty()) {
      result.evidence.push_back(
          {"orh_deferred_index_bulk_publish_benchmark_clean", "proven"});
      result.evidence.push_back(
          {"orh_deferred_index_bulk_publish_internal_proof",
           "sorted_build_root_publish_recovery_exact_append"});
      result.evidence.push_back(
          {"orh_deferred_index_bulk_publish_mga_publish_provider",
           "mga_relation_store.exact_index_entry_append"});
    }
  }
  mark_phase("index_exact_append");

  std::vector<MgaIndexEntryAppendBatch> index_apply_batches;
  std::vector<MgaExactIndexEntryAppendBatch> retail_exact_append_batches;
  if (!sorted_index_build.retail_indexes.empty()) {
    if (direct_retail_exact_append_path) {
      if (direct_precomputed_index_entries.empty()) {
        direct_precomputed_index_entries =
            DirectPrecomputeIndexEntries(sorted_index_build.retail_indexes,
                                         staged_rows,
                                         logical_value_batch);
        mark_phase("index_exact_key_precompute");
      }
      retail_exact_append_batches = DirectExactIndexAppendBatches(
          sorted_index_build.retail_indexes,
          request.target_table.uuid.canonical,
          direct_precomputed_index_entries);
      result.evidence.push_back(
          {"index_apply_planner",
           sorted_index_build.retail_indexes.size() == 1
               ? "single_index_exact_passthrough_v1"
               : "all_unique_exact_passthrough_v1"});
      result.evidence.push_back({"index_apply_grouping_before_append",
                                 "false"});
      result.evidence.push_back(
          {"index_apply_output_batch_count",
           std::to_string(retail_exact_append_batches.size())});
      result.evidence.push_back(
          {"index_apply_exact_entries_precomputed", "true"});
      result.evidence.push_back(
          {"index_apply_unique_order_preserved",
           DirectAllIndexesUnique(sorted_index_build.retail_indexes)
               ? "true"
               : "not_required"});
      result.evidence.push_back({"mga_finality_authority",
                                 "engine_transaction_inventory"});
    } else {
      auto candidate_batches = DirectIndexAppendBatches(
          sorted_index_build.retail_indexes,
          request.target_table.uuid.canonical,
          index_rows);
      if (DirectAllIndexesUnique(sorted_index_build.retail_indexes)) {
        index_apply_batches = std::move(candidate_batches);
        result.evidence.push_back({"index_apply_planner",
                                   "all_unique_passthrough_v1"});
        result.evidence.push_back({"index_apply_grouping_before_append",
                                   "false"});
        result.evidence.push_back(
            {"index_apply_output_batch_count",
             std::to_string(index_apply_batches.size())});
        result.evidence.push_back({"index_apply_unique_order_preserved",
                                   "true"});
        result.evidence.push_back({"mga_finality_authority",
                                   "engine_transaction_inventory"});
      } else {
        const auto index_apply_plan =
            PlanLocalityAwareIndexApplyBatches(candidate_batches);
        if (index_apply_plan.diagnostic.error) {
          if (strict_lifecycle.active) {
            return DirectStrictPhysicalPublicationFailure(
                request,
                &strict_lifecycle,
                result,
                index_apply_plan.diagnostic,
                "index_apply_locality_plan_refused",
                "index_apply_locality_plan");
          }
          return DirectBulkFailure(request,
                                   index_apply_plan.diagnostic,
                                   "index_apply_locality_plan_refused");
        }
        AddLocalityAwareIndexApplyEvidence(index_apply_plan, &result.evidence);
        index_apply_batches = std::move(index_apply_plan.batches);
      }
    }
  }
  mark_phase("index_apply_plan");
  if (IparFaultPointRequested(request.option_envelopes, "index_append")) {
    std::vector<EngineEvidenceReference> evidence = result.evidence;
    AppendIparFaultEvidence(&evidence,
                            "index_append",
                            "rollback_required_after_direct_physical_row_append_before_index_append");
    evidence.push_back({"ipar_fault_injection_row_versions_staged",
                        std::to_string(staged_rows.size())});
    return DirectBulkFailureWithEvidence(
        request,
        IparFaultDiagnostic("dml.direct_physical_bulk_append",
                            "index_append",
                            "phase=direct_physical_index_append"),
        "ipar_fault_injection_index_append",
        evidence,
        result.dml_summary);
  }
  const auto index_appended =
      direct_retail_exact_append_path
          ? hot_append.AppendExactIndexEntryBatches(retail_exact_append_batches)
          : hot_append.AppendIndexEntryBatches(index_apply_batches);
  if (index_appended.error) {
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(request,
                                                    &strict_lifecycle,
                                                    result,
                                                    index_appended,
                                                    "mga_index_append_refused",
                                                    "index_append");
    }
    return DirectBulkFailure(request,
                             index_appended,
                             "mga_index_append_refused");
  }
  mark_phase("index_retail_append");
  const auto index_flushed = hot_append.FlushIndexEntries();
  if (index_flushed.error) {
    if (strict_lifecycle.active) {
      return DirectStrictPhysicalPublicationFailure(request,
                                                    &strict_lifecycle,
                                                    result,
                                                    index_flushed,
                                                    "mga_index_flush_refused",
                                                    "index_flush");
    }
    return DirectBulkFailure(request,
                             index_flushed,
                             "mga_index_flush_refused");
  }
  mark_phase("index_stream_flush");

  if (index_entries_authoritative && !bypass_single_window_native_bulk_cache) {
    if (retail_exact_append_batches.empty()) {
      DirectAppendIndexBatchesToCache(
          request.context,
          request.target_table.uuid.canonical,
          index_only_eligibility.row_version_count,
          static_cast<std::uint64_t>(staged_rows.size()),
          sorted_index_build.exact_batches,
          index_apply_batches,
          sorted_bulk_index_requested);
    } else {
      std::vector<MgaExactIndexEntryAppendBatch> cache_exact_batches =
          sorted_index_build.exact_batches;
      cache_exact_batches.insert(cache_exact_batches.end(),
                                 retail_exact_append_batches.begin(),
                                 retail_exact_append_batches.end());
      DirectAppendIndexBatchesToCache(
          request.context,
          request.target_table.uuid.canonical,
          index_only_eligibility.row_version_count,
          static_cast<std::uint64_t>(staged_rows.size()),
          cache_exact_batches,
          index_apply_batches,
          sorted_bulk_index_requested);
    }
  }

  result = PublishDirectStrictBulkAfterPhysicalSuccess(request,
                                                       &strict_lifecycle,
                                                       std::move(result));
  if (!result.ok) {
    return result;
  }
  if (index_entries_authoritative &&
      !bypass_single_window_native_bulk_cache &&
      !DirectTableDeclaresForeignKey(*table)) {
    const auto next_row_version_count =
        index_only_eligibility.row_version_count +
        static_cast<std::uint64_t>(staged_rows.size());
    const bool advanced_context_cache =
        DirectAdvanceBulkAppendContextCache(request.context,
                                            request.target_table.uuid.canonical,
                                            index_only_eligibility.row_version_count,
                                            next_row_version_count,
                                            index_entries_authoritative,
                                            true);
    if (!advanced_context_cache) {
      DirectStoreBulkAppendContextCache(request.context,
                                        request.target_table.uuid.canonical,
                                        next_row_version_count,
                                        *state,
                                        visible_indexes,
                                        relation_descriptor,
                                        index_entries_authoritative,
                                        true);
    }
    result.evidence.push_back(
        {"direct_physical_bulk_append_context_cache_publish",
         advanced_context_cache ? "advance_row_count" : "store_snapshot"});
  }
  mark_phase("strict_publish");

  const auto hot_counters = hot_append.counters();
  if (hot_counters.row_versions_appended != 0) {
    ++result.dml_summary.append_calls;
  }
  if (hot_counters.index_entries_appended != 0) {
    ++result.dml_summary.append_calls;
  }
  if (!delta_entries.empty()) {
    ++result.dml_summary.append_calls;
  }
  result.dml_summary.file_opens +=
      hot_counters.row_stream_opens + hot_counters.index_stream_opens +
      hot_counters.scoped_row_stream_opens +
      hot_counters.scoped_index_stream_opens +
      hot_counters.allocator_stream_opens;
  result.dml_summary.flushes +=
      hot_counters.row_stream_flushes + hot_counters.index_stream_flushes +
      hot_counters.scoped_row_stream_flushes +
      hot_counters.scoped_index_stream_flushes +
      hot_counters.allocator_stream_flushes;
  mark_phase("hot_append_counters");

  for (std::size_t index = 0; index < staged_rows.size(); ++index) {
    const auto& row = staged_rows[index];
    if (!suppress_payload_rows) {
      CrudRowVersionRecord returning_row;
      returning_row.creator_tx = request.context.local_transaction_id;
      returning_row.event_sequence = row.event_sequence;
      returning_row.sequence = row.sequence;
      returning_row.table_uuid = request.target_table.uuid.canonical;
      returning_row.row_uuid = row.row_uuid;
      returning_row.version_uuid = row.version_uuid;
      returning_row.deleted = false;
      returning_row.values = logical_value_batch[index];
      returning_rows.push_back(std::move(returning_row));
      result.row_uuids.push_back({row.row_uuid});
    }
    ++result.inserted_rows;
    ++batch_context.actual_row_count;
  }
  mark_phase("row_accounting");

  AddInsertTrace(&batch_context,
                 "direct_physical_bulk.batch.finish",
                 "finish",
                 std::to_string(batch_context.actual_row_count));
  result.accepted_rows = static_cast<EngineApiU64>(direct_row_count);
  result.rejected_rows = 0;
  if (suppress_payload_rows) {
    result.result_shape.result_kind = "dml_direct_physical_bulk_result_suppressed";
  } else {
    result.result_shape = CrudRowsToResultShape(returning_rows);
  }
  mark_phase("result_shape");
  result.dml_summary.rows_changed = result.inserted_rows;
  result.evidence.push_back({"mga_row_version", "row_insert"});
  result.evidence.push_back({"mga_row_store", "row_insert"});
  if (hot_counters.index_entries_appended != 0) {
    result.evidence.push_back({"mga_index_store", "row_insert"});
  }
  result.evidence.push_back({"direct_mga_append", "row_version_batch"});
  result.evidence.push_back({"direct_mga_append_module",
                             "storage.mga_relation_store"});
  result.evidence.push_back({"orh_210_runtime_consumed", "true"});
  result.evidence.push_back({"orh_210_consumed_module",
                             "engine.internal_api.dml+storage.mga_relation_store"});
  result.evidence.push_back({"direct_physical_bulk_row_count",
                             std::to_string(result.inserted_rows)});
  result.evidence.push_back({"unique_index_physical_probes", "0"});
  result.evidence.push_back({"unique_index_scan_fallbacks", "0"});
  result.evidence.push_back({"unique_index_bulk_proof_probes",
                             std::to_string(result.dml_summary.index_probes)});
  result.evidence.push_back({"row_uuid_generation",
                             request.require_generated_row_uuid ? "required"
                                                                : "caller_allowed"});
  result.evidence.push_back({"trigger_udr_hooks", "inactive_checked"});
  mark_phase("base_evidence");
  AddHotAppendCounterEvidence(hot_counters, &result);
  mark_phase("hot_append_counter_evidence");
  AddInsertBatchEvidenceToResult(batch_context, &result);
  mark_phase("insert_batch_evidence");
  AddDmlSummaryEvidence(&result);
  mark_phase("dml_summary_evidence");
  ApplyWriteResultPolicy(write_result_policy, &result);
  mark_phase("write_result_policy");
  RecordInsertBatchMetric(batch_context,
                          "sb_dml_insert_batch_started_total",
                          1.0,
                          "ok");
  if (result.dml_summary.index_probes != 0) {
    RecordInsertBatchMetric(batch_context,
                            "sb_index_insert_unique_physical_probe_total",
                            static_cast<double>(result.dml_summary.index_probes),
                            "bulk_unique_proof",
                            "direct_copy_bulk_unique_proof");
  }
  RecordInsertBatchMetric(batch_context,
                          "sb_dml_insert_rows_inserted_total",
                          static_cast<double>(result.inserted_rows),
                          "ok");
  mark_phase("record_insert_metrics");
  AddDirectBulkPhaseEvidence(phase_micros, &result);
  WriteDirectBulkPhaseTrace(request, result, phase_micros);
  return result;
}

}  // namespace scratchbird::engine::internal_api::dml
