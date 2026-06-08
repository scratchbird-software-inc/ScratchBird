// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_driver_explain_compatibility.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

bool Empty(std::string_view value) {
  return value.empty();
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool IsHashLike(std::string_view value) {
  return StartsWith(value, "sha256:");
}

bool IsPlanHashLike(std::string_view value) {
  return IsHashLike(value) || StartsWith(value, "runtime-plan-fnv64:");
}

bool PlaceholderDigest(std::string_view value) {
  return value.empty() || value == "result-contract-v1" ||
         value == "sha256:result-contract-v1" ||
         value == "sha256:placeholder" || value == "placeholder" ||
         value.find("placeholder") != std::string_view::npos ||
         value.find("dummy") != std::string_view::npos ||
         value.find("test-only") != std::string_view::npos;
}

std::string RoutePrefix(
    const OptimizerDriverVisibleExplainRouteRecord& record) {
  if (!record.route_kind.empty()) return record.route_kind;
  return record.route_label.empty() ? "unnamed_driver_explain_route"
                                    : record.route_label;
}

std::string ReportPrefix(
    const OptimizerDriverVisibleExplainCompatibilityReport& report) {
  return report.report_id.empty() ? "unnamed_driver_explain_report"
                                  : report.report_id;
}

void RequireField(OptimizerDriverVisibleExplainCompatibilityValidation*
                      validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

void AddDiagnostic(OptimizerDriverVisibleExplainCompatibilityValidation*
                       validation,
                   std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

bool HasAuthorityDrift(
    const OptimizerExplainCompatibilityAuthorityFlags& authority) {
  return authority.transaction_finality_authority ||
         authority.visibility_authority ||
         authority.authorization_security_authority ||
         authority.recovery_authority ||
         authority.parser_authority ||
         authority.donor_authority ||
         authority.wal_authority ||
         authority.benchmark_authority ||
         authority.optimizer_plan_authority ||
         authority.index_finality_authority ||
         authority.provider_finality_authority ||
         authority.local_cluster_authority ||
         authority.cluster_authority ||
         authority.agent_action_authority;
}

bool IsSupportedRoute(std::string_view route_kind) {
  return route_kind == "embedded" || route_kind == "ipc" ||
         route_kind == "inet" || route_kind == "cli" ||
         route_kind == "driver";
}

std::string LowerAscii(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const unsigned char ch : value) {
    lowered.push_back(static_cast<char>(std::tolower(ch)));
  }
  return lowered;
}

bool ContainsAny(std::string_view haystack,
                 const std::vector<std::string_view>& needles) {
  const auto lowered = LowerAscii(haystack);
  return std::any_of(needles.begin(), needles.end(), [&](auto needle) {
    return lowered.find(needle) != std::string::npos;
  });
}

bool ContainsCanonicalUuid(std::string_view value) {
  static const std::regex uuid_pattern(
      R"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})");
  return std::regex_search(value.begin(), value.end(), uuid_pattern);
}

bool ExplainJsonHasRequiredShape(
    const OptimizerDriverVisibleExplainRouteRecord& record) {
  return record.explain_json.find("\"schema_version\"") != std::string::npos &&
         record.explain_json.find(kDriverVisibleExplainJsonSchemaId) !=
             std::string::npos &&
         record.explain_json.find("\"plan_hash\"") != std::string::npos &&
         record.explain_json.find("\"result_contract_hash\"") !=
             std::string::npos &&
         record.explain_json.find("\"redaction\"") != std::string::npos;
}

bool ExplainJsonLeaksSqlText(std::string_view explain_json) {
  return ContainsAny(explain_json,
                     {"\"sql\"", " sql_text", "select ", "insert ",
                      "update ", "delete ", "merge ", "create ",
                      "alter ", "drop ", "from "});
}

bool ExplainJsonLeaksProtectedMaterial(std::string_view explain_json) {
  return ContainsAny(explain_json,
                     {"password", "secret", "token", "private_key",
                      "protected_material", "credential", "auth_header"});
}

DriverVisibleExplainRouteEvidence ToRouteEvidence(
    const OptimizerDriverVisibleExplainRouteRecord& record) {
  DriverVisibleExplainRouteEvidence route;
  route.route_kind = record.route_kind;
  route.route_label = record.route_label;
  route.driver_visible_route = record.driver_visible_route;
  route.plan_evidence_digest = record.plan_evidence_digest;
  route.explain_digest = record.explain_digest;
  route.diagnostics = record.diagnostics;
  route.result_hash = record.result_hash;
  route.redaction_digest = record.redaction_digest;
  route.redaction_applied = record.redaction_applied;
  route.driver_or_benchmark_authority =
      record.authority.benchmark_authority ||
      record.authority.optimizer_plan_authority;
  route.transaction_finality_authority =
      record.authority.transaction_finality_authority;
  route.visibility_authority = record.authority.visibility_authority;
  route.security_authority =
      record.authority.authorization_security_authority;
  route.recovery_authority = record.authority.recovery_authority;
  route.diagnostic_code = record.diagnostic_code;
  return route;
}

bool SameDiagnostics(const std::vector<std::string>& lhs,
                     const std::vector<std::string>& rhs) {
  return lhs == rhs;
}

void AddRouteDiagnostic(
    OptimizerDriverVisibleExplainCompatibilityValidation* validation,
    const OptimizerDriverVisibleExplainRouteRecord& record,
    std::string diagnostic) {
  AddDiagnostic(validation, RoutePrefix(record) + ":" + std::move(diagnostic));
}

void MergeNestedBenchmarkDiagnostics(
    OptimizerDriverVisibleExplainCompatibilityValidation* validation,
    const OptimizerDriverVisibleExplainRouteRecord& record,
    const PersistedOptimizerBenchmarkEvidenceValidation& nested) {
  AddRouteDiagnostic(validation,
                     record,
                     "SB_OPT_DRIVER_EXPLAIN_COMPAT.NESTED_CEIC051_INVALID:" +
                         nested.diagnostic_code);
  validation->missing_fields.insert(validation->missing_fields.end(),
                                    nested.missing_fields.begin(),
                                    nested.missing_fields.end());
  validation->diagnostics.insert(validation->diagnostics.end(),
                                  nested.diagnostics.begin(),
                                  nested.diagnostics.end());
}

bool BenchmarkFieldsMatchRoute(
    const OptimizerDriverVisibleExplainRouteRecord& record) {
  const auto& benchmark = record.benchmark_evidence;
  return benchmark.route_kind == record.route_kind &&
         benchmark.route_label == record.route_label &&
         benchmark.physical_plan_hash == record.plan_hash &&
         benchmark.result_contract_hash == record.result_contract_hash &&
         benchmark.result_hash == record.result_hash &&
         benchmark.optimizer_profile == record.optimizer_profile &&
         benchmark.redaction_digest == record.redaction_digest &&
         benchmark.catalog_epoch == record.catalog_epoch &&
         benchmark.security_epoch == record.security_epoch &&
         benchmark.redaction_epoch == record.redaction_epoch &&
         benchmark.statistics_epoch == record.statistics_epoch &&
         benchmark.provider_generation == record.provider_generation;
}

bool BenchmarkEvidenceIsClaimBlockedExternal(
    const PersistedOptimizerBenchmarkEvidenceRecord& benchmark) {
  return benchmark.cluster_mode ==
             OptimizerBenchmarkClusterMode::kExternalProviderDelegated &&
         !benchmark.external_cluster_provider_id.empty() &&
         benchmark.cluster_claim_blocked &&
         !benchmark.production_benchmark_clean_claim;
}

}  // namespace

OptimizerDriverVisibleExplainCompatibilityValidation
ValidateOptimizerDriverVisibleExplainRouteRecord(
    const OptimizerDriverVisibleExplainRouteRecord& record) {
  OptimizerDriverVisibleExplainCompatibilityValidation validation;
  const auto prefix = RoutePrefix(record);

  RequireField(&validation, !Empty(record.route_kind), "route_kind");
  RequireField(&validation, !Empty(record.route_label), "route_label");
  RequireField(&validation,
               record.explain_schema_id == kDriverVisibleExplainJsonSchemaId,
               "explain_schema_id");
  RequireField(&validation,
               record.explain_schema_version_major ==
                   kDriverVisibleExplainJsonSchemaMajor,
               "explain_schema_version_major");
  RequireField(&validation,
               record.explain_schema_version_minor ==
                   kDriverVisibleExplainJsonSchemaMinor,
               "explain_schema_version_minor");
  RequireField(&validation, !Empty(record.explain_json), "explain_json");
  RequireField(&validation,
               IsHashLike(record.json_canonicalization_digest) &&
                   !PlaceholderDigest(record.json_canonicalization_digest),
               "json_canonicalization_digest");
  RequireField(&validation,
               IsPlanHashLike(record.plan_hash) &&
                   !PlaceholderDigest(record.plan_hash),
               "plan_hash");
  RequireField(&validation,
               IsHashLike(record.plan_evidence_digest) &&
                   !PlaceholderDigest(record.plan_evidence_digest),
               "plan_evidence_digest");
  RequireField(&validation,
               IsHashLike(record.explain_digest) &&
                   !PlaceholderDigest(record.explain_digest),
               "explain_digest");
  RequireField(&validation,
               IsHashLike(record.result_contract_hash) &&
                   !PlaceholderDigest(record.result_contract_hash),
               "result_contract_hash");
  RequireField(&validation,
               IsHashLike(record.result_hash) &&
                   !PlaceholderDigest(record.result_hash),
               "result_hash");
  RequireField(&validation, !Empty(record.diagnostic_code), "diagnostic_code");
  RequireField(&validation,
               IsHashLike(record.redaction_digest) &&
                   !PlaceholderDigest(record.redaction_digest),
               "redaction_digest");
  RequireField(&validation,
               !Empty(record.optimizer_profile),
               "optimizer_profile");
  RequireField(&validation,
               !Empty(record.source_provenance),
               "source_provenance");
  RequireField(&validation,
               IsHashLike(record.provenance_digest) &&
                   !PlaceholderDigest(record.provenance_digest),
               "provenance_digest");
  RequireField(&validation,
               IsHashLike(record.evidence_digest) &&
                   !PlaceholderDigest(record.evidence_digest),
               "evidence_digest");

  if (!IsSupportedRoute(record.route_kind)) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.ROUTE_UNSUPPORTED");
  }
  if (!record.driver_visible_route) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.DRIVER_VISIBLE_ROUTE_MISSING");
  }
  if (record.route_kind == "driver" &&
      (record.claimed_driver_name.empty() || !record.claimed_driver_route)) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.CLAIMED_DRIVER_IDENTITY_MISSING");
  }
  if (!ExplainJsonHasRequiredShape(record)) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.JSON_CONTRACT_INVALID");
  }
  if (ExplainJsonLeaksSqlText(record.explain_json) ||
      !record.no_sql_text_leak) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.SQL_TEXT_LEAK");
  }
  if (ContainsCanonicalUuid(record.explain_json) || !record.no_raw_uuid_leak) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.RAW_UUID_LEAK");
  }
  if (ExplainJsonLeaksProtectedMaterial(record.explain_json) ||
      !record.no_protected_material_leak) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.PROTECTED_MATERIAL_LEAK");
  }
  if (!record.redaction_applied || record.redaction_proofs.empty()) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.REDACTION_PROOF_MISSING");
  }
  if (record.catalog_epoch <= 1 || record.security_epoch <= 1 ||
      record.redaction_epoch <= 1 || record.statistics_epoch <= 1 ||
      record.route_epoch <= 1 || record.provider_generation <= 1 ||
      record.source_generation <= 1) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.PLACEHOLDER_EPOCH");
  }
  if (!record.trusted_provenance || !record.fresh || !record.evidence_only ||
      record.freshness_microseconds == 0 ||
      record.max_freshness_microseconds == 0 ||
      record.freshness_microseconds > record.max_freshness_microseconds) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.PROVENANCE_FRESHNESS_INVALID");
  }
  if (record.placeholder_runtime_evidence || record.synthetic_statistics ||
      record.local_default_statistics || record.policy_default_statistics) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.SYNTHETIC_OR_PLACEHOLDER");
  }
  if (HasAuthorityDrift(record.authority)) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.FORBIDDEN_AUTHORITY");
  }
  if (!record.ceic_051_benchmark_evidence_attached) {
    AddRouteDiagnostic(
        &validation,
        record,
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.CEIC051_EVIDENCE_REQUIRED");
  } else {
    const auto nested =
        ValidatePersistedOptimizerBenchmarkEvidenceRecord(
            record.benchmark_evidence);
    const bool claim_blocked_external =
        BenchmarkEvidenceIsClaimBlockedExternal(record.benchmark_evidence);
    if (!nested.ok ||
        (!nested.benchmark_clean_admissible && !claim_blocked_external)) {
      MergeNestedBenchmarkDiagnostics(&validation, record, nested);
    } else if (!BenchmarkFieldsMatchRoute(record)) {
      AddRouteDiagnostic(
          &validation,
          record,
          "SB_OPT_DRIVER_EXPLAIN_COMPAT.CEIC051_ROUTE_MISMATCH");
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.compatibility_proven = validation.ok;
  if (validation.ok) {
    validation.diagnostic_code = "SB_OPT_DRIVER_EXPLAIN_COMPAT.ROUTE_OK";
  } else if (!validation.missing_fields.empty()) {
    validation.diagnostic_code =
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.MISSING_REQUIRED_FIELD";
  } else {
    validation.diagnostic_code =
        prefix + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.ROUTE_INVALID";
  }
  return validation;
}

OptimizerDriverVisibleExplainCompatibilityValidation
ValidateOptimizerDriverVisibleExplainCompatibilityReport(
    const OptimizerDriverVisibleExplainCompatibilityReport& report) {
  OptimizerDriverVisibleExplainCompatibilityValidation validation;
  const auto prefix = ReportPrefix(report);

  RequireField(&validation,
               report.schema_id ==
                   kOptimizerDriverExplainCompatibilitySchemaId,
               "schema_id");
  RequireField(&validation,
               report.schema_version_major ==
                   kOptimizerDriverExplainCompatibilitySchemaMajor,
               "schema_version_major");
  RequireField(&validation,
               report.schema_version_minor ==
                   kOptimizerDriverExplainCompatibilitySchemaMinor,
               "schema_version_minor");
  RequireField(&validation, !Empty(report.report_id), "report_id");
  RequireField(&validation,
               IsHashLike(report.dataset_schema_digest) &&
                   !PlaceholderDigest(report.dataset_schema_digest),
               "dataset_schema_digest");
  RequireField(&validation,
               IsHashLike(report.sblr_digest) &&
                   !PlaceholderDigest(report.sblr_digest),
               "sblr_digest");
  RequireField(&validation,
               IsHashLike(report.logical_plan_hash) &&
                   !PlaceholderDigest(report.logical_plan_hash),
               "logical_plan_hash");
  RequireField(&validation,
               !Empty(report.optimizer_profile),
               "optimizer_profile");
  RequireField(&validation,
               IsHashLike(report.evidence_digest) &&
                   !PlaceholderDigest(report.evidence_digest),
               "evidence_digest");
  RequireField(&validation,
               IsHashLike(report.provenance_digest) &&
                   !PlaceholderDigest(report.provenance_digest),
               "provenance_digest");

  if (!report.trusted_provenance || !report.fresh || !report.evidence_only) {
    AddDiagnostic(
        &validation,
        prefix + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.REPORT_PROVENANCE_INVALID");
  }
  if (HasAuthorityDrift(report.authority)) {
    AddDiagnostic(
        &validation,
        prefix + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.REPORT_FORBIDDEN_AUTHORITY");
  }
  if (report.cluster_mode ==
      OptimizerExplainCompatibilityClusterMode::kLocalClusterEvidence) {
    AddDiagnostic(
        &validation,
        prefix + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.LOCAL_CLUSTER_FORBIDDEN");
  } else if (report.cluster_mode ==
             OptimizerExplainCompatibilityClusterMode::
                 kExternalProviderDelegated) {
    if (report.external_cluster_provider_id.empty() ||
        !report.cluster_claim_blocked ||
        report.production_compatibility_claim) {
      AddDiagnostic(
          &validation,
          prefix +
              ":SB_OPT_DRIVER_EXPLAIN_COMPAT.EXTERNAL_CLUSTER_CLAIM_BLOCK_REQUIRED");
    }
    for (const auto& route : report.routes) {
      if (!BenchmarkEvidenceIsClaimBlockedExternal(route.benchmark_evidence)) {
        AddRouteDiagnostic(
            &validation,
            route,
            "SB_OPT_DRIVER_EXPLAIN_COMPAT.EXTERNAL_CLUSTER_NESTED_CLAIM_BLOCK_REQUIRED");
      }
    }
  }

  if (report.routes.empty()) {
    AddDiagnostic(&validation,
                  prefix + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.ROUTES_MISSING");
  }
  if (report.required_route_kinds.empty()) {
    AddDiagnostic(
        &validation,
        prefix + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.REQUIRED_ROUTES_MISSING");
  }

  const OptimizerDriverVisibleExplainRouteRecord* embedded = nullptr;
  std::set<std::string> seen_routes;
  std::set<std::string> seen_claimed_drivers;
  std::vector<DriverVisibleExplainRouteEvidence> route_evidence;
  for (const auto& route : report.routes) {
    const auto route_validation =
        ValidateOptimizerDriverVisibleExplainRouteRecord(route);
    if (!route_validation.ok) {
      AddDiagnostic(&validation,
                    RoutePrefix(route) + ":" +
                        route_validation.diagnostic_code);
      validation.missing_fields.insert(
          validation.missing_fields.end(),
          route_validation.missing_fields.begin(),
          route_validation.missing_fields.end());
      validation.diagnostics.insert(validation.diagnostics.end(),
                                    route_validation.diagnostics.begin(),
                                    route_validation.diagnostics.end());
    }

    if (!route.route_kind.empty() && !seen_routes.insert(route.route_kind).second) {
      AddRouteDiagnostic(
          &validation,
          route,
          "SB_OPT_DRIVER_EXPLAIN_COMPAT.DUPLICATE_ROUTE_KIND");
    }
    if (!report.optimizer_profile.empty() &&
        route.optimizer_profile != report.optimizer_profile) {
      AddRouteDiagnostic(
          &validation,
          route,
          "SB_OPT_DRIVER_EXPLAIN_COMPAT.OPTIMIZER_PROFILE_MISMATCH");
    }
    if (route.embedded_reference_route || route.route_kind == "embedded") {
      if (embedded != nullptr) {
        AddRouteDiagnostic(
            &validation,
            route,
            "SB_OPT_DRIVER_EXPLAIN_COMPAT.DUPLICATE_EMBEDDED_REFERENCE");
      } else {
        embedded = &route;
      }
    }
    if (route.claimed_driver_route || route.route_kind == "driver") {
      if (!route.claimed_driver_name.empty()) {
        seen_claimed_drivers.insert(route.claimed_driver_name);
      }
    }
    route_evidence.push_back(ToRouteEvidence(route));
  }

  if (embedded == nullptr) {
    AddDiagnostic(
        &validation,
        prefix + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.EMBEDDED_REFERENCE_MISSING");
  } else {
    for (const auto& route : report.routes) {
      if (&route == embedded) continue;
      if (route.route_label != embedded->route_label) {
        AddRouteDiagnostic(
            &validation,
            route,
            "SB_OPT_DRIVER_EXPLAIN_COMPAT.ROUTE_LABEL_MISMATCH");
      }
      if (route.optimizer_profile != embedded->optimizer_profile) {
        AddRouteDiagnostic(
            &validation,
            route,
            "SB_OPT_DRIVER_EXPLAIN_COMPAT.OPTIMIZER_PROFILE_MISMATCH");
      }
      if (route.plan_hash != embedded->plan_hash ||
          route.plan_evidence_digest != embedded->plan_evidence_digest ||
          route.explain_digest != embedded->explain_digest ||
          route.json_canonicalization_digest !=
              embedded->json_canonicalization_digest) {
        AddRouteDiagnostic(
            &validation,
            route,
            "SB_OPT_DRIVER_EXPLAIN_COMPAT.EXPLAIN_DIGEST_MISMATCH");
      }
      if (route.result_contract_hash != embedded->result_contract_hash ||
          route.result_hash != embedded->result_hash) {
        AddRouteDiagnostic(
            &validation,
            route,
            "SB_OPT_DRIVER_EXPLAIN_COMPAT.RESULT_MISMATCH");
      }
      if (route.diagnostic_code != embedded->diagnostic_code ||
          !SameDiagnostics(route.diagnostics, embedded->diagnostics)) {
        AddRouteDiagnostic(
            &validation,
            route,
            "SB_OPT_DRIVER_EXPLAIN_COMPAT.DIAGNOSTIC_MISMATCH");
      }
      if (route.redaction_digest != embedded->redaction_digest ||
          route.redaction_applied != embedded->redaction_applied ||
          route.redaction_proofs != embedded->redaction_proofs) {
        AddRouteDiagnostic(
            &validation,
            route,
            "SB_OPT_DRIVER_EXPLAIN_COMPAT.REDACTION_MISMATCH");
      }
    }
  }

  for (const auto& required : report.required_route_kinds) {
    if (seen_routes.find(required) == seen_routes.end()) {
      AddDiagnostic(
          &validation,
          required + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.MISSING_REQUIRED_ROUTE");
    }
  }
  if (report.require_claimed_driver_route && seen_claimed_drivers.empty()) {
    AddDiagnostic(
        &validation,
        prefix + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.CLAIMED_DRIVER_ROUTE_MISSING");
  }
  for (const auto& driver : report.claimed_driver_names) {
    if (seen_claimed_drivers.find(driver) == seen_claimed_drivers.end()) {
      AddDiagnostic(
          &validation,
          driver + ":SB_OPT_DRIVER_EXPLAIN_COMPAT.MISSING_CLAIMED_DRIVER");
    }
  }

  if (!route_evidence.empty()) {
    std::vector<DriverVisibleExplainRouteEvidence> ordered_route_evidence;
    if (embedded != nullptr) {
      ordered_route_evidence.push_back(ToRouteEvidence(*embedded));
      for (const auto& route : report.routes) {
        if (&route != embedded) ordered_route_evidence.push_back(ToRouteEvidence(route));
      }
    } else {
      ordered_route_evidence = route_evidence;
    }
    const auto route_validation =
        ValidateDriverVisibleExplainRouteEquivalence(
            ordered_route_evidence, report.required_route_kinds);
    if (!route_validation.ok) {
      AddDiagnostic(
          &validation,
          prefix +
              ":SB_OPT_DRIVER_EXPLAIN_COMPAT.RUNTIME_ROUTE_EQUIVALENCE_FAILED:" +
              route_validation.diagnostic_code);
      validation.diagnostics.insert(validation.diagnostics.end(),
                                    route_validation.diagnostics.begin(),
                                    route_validation.diagnostics.end());
    }
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty();
  validation.compatibility_proven =
      validation.ok && report.production_compatibility_claim &&
      report.cluster_mode == OptimizerExplainCompatibilityClusterMode::kNoCluster;
  if (validation.ok) {
    validation.diagnostic_code =
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.REPORT_OK";
  } else if (!validation.missing_fields.empty()) {
    validation.diagnostic_code =
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.MISSING_REQUIRED_FIELD";
  } else {
    validation.diagnostic_code =
        "SB_OPT_DRIVER_EXPLAIN_COMPAT.REPORT_INVALID";
  }
  return validation;
}

}  // namespace scratchbird::engine::optimizer
