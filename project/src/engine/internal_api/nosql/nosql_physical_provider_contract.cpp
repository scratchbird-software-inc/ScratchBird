// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_physical_provider_contract.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace index = scratchbird::core::index;

void Add(std::vector<std::string>* values, std::string value) {
  values->push_back(std::move(value));
}

void AddBoolEvidence(std::vector<std::string>* evidence,
                     const std::string& key,
                     bool value) {
  Add(evidence, key + "=" + (value ? "true" : "false"));
}

void AddMissing(EngineNoSqlPhysicalProviderSelection* selection,
                const char* diagnostic) {
  Add(&selection->missing_diagnostics, diagnostic);
  Add(&selection->evidence, std::string("missing=") + diagnostic);
}

void AddRefusal(EngineNoSqlPhysicalProviderSelection* selection,
                const char* diagnostic) {
  Add(&selection->refusal_diagnostics, diagnostic);
  Add(&selection->evidence, std::string("refusal=") + diagnostic);
}

void AddPolicyRefusal(EngineNoSqlPhysicalProviderSelection* selection,
                      const std::string& reason) {
  Add(&selection->refusal_diagnostics,
      std::string(kNoSqlProviderPolicyRefused) + ":" + reason);
  Add(&selection->evidence, "policy_refusal=" + reason);
}

void AddAuthorityEvidence(EngineNoSqlPhysicalProviderSelection* selection,
                          const EngineNoSqlMgaRecheckProof& proof) {
  selection->row_mga_recheck_required = proof.row_mga_recheck_required;
  selection->row_security_recheck_required = proof.row_security_recheck_required;
  selection->provider_transaction_finality_authority =
      proof.provider_claims_transaction_finality_authority;
  selection->provider_visibility_authority =
      proof.provider_claims_visibility_authority;
  selection->index_transaction_finality_authority =
      proof.index_claims_transaction_finality_authority;
  selection->delta_overlay_transaction_finality_authority =
      proof.delta_overlay_claims_transaction_finality_authority;
  selection->parser_transaction_finality_authority =
      proof.parser_claims_transaction_finality_authority;
  selection->write_ahead_log_transaction_finality_authority =  // wal-not-authority
      proof.write_ahead_log_claims_transaction_finality_authority;  // wal-not-authority

  Add(&selection->evidence, "transaction_authority_source=" + proof.authority_source);
  AddBoolEvidence(&selection->evidence,
                  "row_mga_recheck_required",
                  selection->row_mga_recheck_required);
  AddBoolEvidence(&selection->evidence,
                  "row_security_recheck_required",
                  selection->row_security_recheck_required);
  AddBoolEvidence(&selection->evidence,
                  "provider_transaction_finality_authority",
                  selection->provider_transaction_finality_authority);
  AddBoolEvidence(&selection->evidence,
                  "provider_visibility_authority",
                  selection->provider_visibility_authority);
  AddBoolEvidence(&selection->evidence,
                  "index_transaction_finality_authority",
                  selection->index_transaction_finality_authority);
  AddBoolEvidence(&selection->evidence,
                  "delta_overlay_transaction_finality_authority",
                  selection->delta_overlay_transaction_finality_authority);
  AddBoolEvidence(&selection->evidence,
                  "parser_transaction_finality_authority",
                  selection->parser_transaction_finality_authority);
  AddBoolEvidence(&selection->evidence,
                  "write_ahead_log_transaction_finality_authority",  // wal-not-authority
                  selection->write_ahead_log_transaction_finality_authority);  // wal-not-authority
}

bool EqualsFamily(std::string_view value, std::string_view expected) {
  return value == expected || value == ("nosql." + std::string(expected));
}

}  // namespace

const char* EngineNoSqlProviderFamilyName(EngineNoSqlProviderFamily family) {
  switch (family) {
    case EngineNoSqlProviderFamily::kKeyValue: return "key_value";
    case EngineNoSqlProviderFamily::kDocument: return "document";
    case EngineNoSqlProviderFamily::kSearch: return "search";
    case EngineNoSqlProviderFamily::kVector: return "vector";
    case EngineNoSqlProviderFamily::kGraph: return "graph";
    case EngineNoSqlProviderFamily::kTimeSeries: return "time_series";
    case EngineNoSqlProviderFamily::kSpatial: return "spatial";
    case EngineNoSqlProviderFamily::kColumnar: return "columnar";
    case EngineNoSqlProviderFamily::kUnknown: return "unknown";
  }
  return "unknown";
}

EngineNoSqlProviderFamily EngineNoSqlProviderFamilyFromString(
    std::string_view family) {
  if (EqualsFamily(family, "kv") || EqualsFamily(family, "key_value") ||
      EqualsFamily(family, "key-value")) {
    return EngineNoSqlProviderFamily::kKeyValue;
  }
  if (EqualsFamily(family, "document")) {
    return EngineNoSqlProviderFamily::kDocument;
  }
  if (EqualsFamily(family, "search") || EqualsFamily(family, "text")) {
    return EngineNoSqlProviderFamily::kSearch;
  }
  if (EqualsFamily(family, "vector")) {
    return EngineNoSqlProviderFamily::kVector;
  }
  if (EqualsFamily(family, "graph")) {
    return EngineNoSqlProviderFamily::kGraph;
  }
  if (EqualsFamily(family, "time_series") || EqualsFamily(family, "timeseries")) {
    return EngineNoSqlProviderFamily::kTimeSeries;
  }
  if (EqualsFamily(family, "spatial")) {
    return EngineNoSqlProviderFamily::kSpatial;
  }
  if (EqualsFamily(family, "columnar")) {
    return EngineNoSqlProviderFamily::kColumnar;
  }
  return EngineNoSqlProviderFamily::kUnknown;
}

const char* EngineNoSqlProviderScopeName(EngineNoSqlProviderScope scope) {
  switch (scope) {
    case EngineNoSqlProviderScope::kLocal: return "local";
    case EngineNoSqlProviderScope::kClusterOnly: return "cluster_only";
    case EngineNoSqlProviderScope::kDistributed: return "distributed";
  }
  return "unknown";
}

index::CompressionFamily EngineNoSqlProviderCompressionFamily(
    EngineNoSqlProviderFamily family) {
  switch (family) {
    case EngineNoSqlProviderFamily::kKeyValue:
      return index::CompressionFamily::kBlobPayload;
    case EngineNoSqlProviderFamily::kDocument:
      return index::CompressionFamily::kDocumentShape;
    case EngineNoSqlProviderFamily::kSearch:
      return index::CompressionFamily::kSearchPosting;
    case EngineNoSqlProviderFamily::kVector:
      return index::CompressionFamily::kVectorCode;
    case EngineNoSqlProviderFamily::kGraph:
      return index::CompressionFamily::kPostingList;
    case EngineNoSqlProviderFamily::kTimeSeries:
      return index::CompressionFamily::kTimeSeriesMetricPage;
    case EngineNoSqlProviderFamily::kSpatial:
      return index::CompressionFamily::kExactIndexPage;
    case EngineNoSqlProviderFamily::kColumnar:
    case EngineNoSqlProviderFamily::kUnknown:
      return index::CompressionFamily::kRowPage;
  }
  return index::CompressionFamily::kRowPage;
}

const char* EngineNoSqlProviderCompressionPolicyFamilyName(
    EngineNoSqlProviderFamily family) {
  return index::CompressionFamilyName(
      EngineNoSqlProviderCompressionFamily(family));
}

std::vector<std::string> EngineNoSqlProviderCompressionPolicyEvidence(
    EngineNoSqlProviderFamily family) {
  return {
      std::string(index::kCompressionPolicyByFamilySearchKey),
      std::string("nosql_provider_family=") +
          EngineNoSqlProviderFamilyName(family),
      std::string("compression_family=") +
          EngineNoSqlProviderCompressionPolicyFamilyName(family),
      "compression_adapter=nosql_physical_provider_contract",
      "compression_metadata_only=true",
      "provider_finality_authority=false",
      "parser_or_donor_authority=false",
  };
}

EngineNoSqlPhysicalProviderSelection SelectLocalNoSqlPhysicalProvider(
    const EngineNoSqlPhysicalProviderContract& contract) {
  EngineNoSqlPhysicalProviderSelection selection;
  selection.family = contract.family;
  selection.scope = contract.scope;
  selection.estimated_rows = contract.estimated_rows;
  selection.fallback_provider_id = contract.fallback_provider_id;
  selection.descriptor_scan_selected =
      contract.descriptor_visibility.descriptor_scan_selected;
  selection.behavior_store_scan_selected =
      contract.descriptor_visibility.behavior_store_scan_selected;
  selection.required_facts = {
      "local_provider",
      "descriptor_visibility_proof",
      "security_redaction_proof",
      "index_generation_proof",
      "persistent_provider_generation_when_required",
      "delta_overlay_proof_when_required",
      "policy_allowance_proof",
      "mga_and_security_recheck_proof",
  };

  Add(&selection.evidence,
      std::string("provider_family=") + EngineNoSqlProviderFamilyName(contract.family));
  const auto compression_evidence =
      EngineNoSqlProviderCompressionPolicyEvidence(contract.family);
  selection.evidence.insert(selection.evidence.end(),
                            compression_evidence.begin(),
                            compression_evidence.end());
  Add(&selection.evidence,
      std::string("provider_scope=") + EngineNoSqlProviderScopeName(contract.scope));
  Add(&selection.evidence, "provider_id=" + contract.provider_id);
  AddBoolEvidence(&selection.evidence,
                  "local_provider_available",
                  contract.local_provider_available);
  AddBoolEvidence(&selection.evidence,
                  "exact_fallback_available",
                  contract.exact_fallback_available);
  AddBoolEvidence(&selection.evidence,
                  "descriptor_scan_selected",
                  selection.descriptor_scan_selected);
  AddBoolEvidence(&selection.evidence,
                  "behavior_store_scan_selected",
                  selection.behavior_store_scan_selected);
  AddBoolEvidence(&selection.evidence,
                  "provider_generation_required",
                  contract.provider_generation.required);
  if (contract.provider_generation.required ||
      contract.provider_generation.proof_present) {
    AddBoolEvidence(&selection.evidence,
                    "provider_generation_proof_present",
                    contract.provider_generation.proof_present);
    Add(&selection.evidence,
        "provider_generation_required_id=" +
            std::to_string(contract.provider_generation.required_generation));
    Add(&selection.evidence,
        "provider_generation_available_id=" +
            std::to_string(contract.provider_generation.available_generation));
    Add(&selection.evidence,
        "provider_generation_uuid=" +
            (contract.provider_generation.generation_uuid.empty()
                 ? std::string("none")
                 : contract.provider_generation.generation_uuid));
    Add(&selection.evidence,
        "provider_generation_publish_state=" +
            contract.provider_generation.publish_state);
    Add(&selection.evidence,
        "provider_generation_validation_state=" +
            contract.provider_generation.validation_state);
  }
  AddAuthorityEvidence(&selection, contract.mga_recheck);

  if (contract.family == EngineNoSqlProviderFamily::kUnknown) {
    AddRefusal(&selection, kNoSqlProviderFamilyUnsupported);
  }
  if (!contract.local_provider_available) {
    AddMissing(&selection, kNoSqlProviderLocalProviderMissing);
  }
  if (contract.scope == EngineNoSqlProviderScope::kClusterOnly) {
    AddRefusal(&selection, kNoSqlProviderClusterScopeRefusedLocalOnly);
  }
  if (contract.scope == EngineNoSqlProviderScope::kDistributed) {
    AddRefusal(&selection, kNoSqlProviderDistributedScopeRefusedLocalOnly);
  }

  if (!contract.descriptor_visibility.proof_present) {
    AddMissing(&selection, kNoSqlProviderDescriptorVisibilityProofMissing);
  }
  if (contract.descriptor_visibility.proof_present &&
      !contract.descriptor_visibility.visible_to_snapshot) {
    AddRefusal(&selection, kNoSqlProviderDescriptorNotVisibleToSnapshot);
  }
  if (!contract.descriptor_visibility.descriptor_shape_compatible) {
    AddMissing(&selection, kNoSqlProviderDescriptorCompatibilityMissing);
  }
  if (selection.descriptor_scan_selected) {
    AddRefusal(&selection, kNoSqlProviderDescriptorScanNotPhysicalProvider);
  }
  if (selection.behavior_store_scan_selected) {
    AddRefusal(&selection, kNoSqlProviderBehaviorScanNotPhysicalProvider);
  }

  if (!contract.security_redaction.proof_present ||
      !contract.security_redaction.redaction_policy_bound) {
    AddMissing(&selection, kNoSqlProviderSecurityProofMissing);
  }
  if (!contract.security_redaction.security_snapshot_bound) {
    AddMissing(&selection, kNoSqlProviderSecuritySnapshotProofMissing);
  }

  if (!contract.index_generation.proof_present) {
    AddMissing(&selection, kNoSqlProviderIndexGenerationProofMissing);
  }
  if (contract.index_generation.proof_present &&
      contract.index_generation.available_generation <
          contract.index_generation.required_generation) {
    AddRefusal(&selection, kNoSqlProviderIndexGenerationStale);
  }
  if (contract.index_generation.proof_present &&
      !contract.index_generation.visible_to_snapshot) {
    AddRefusal(&selection, kNoSqlProviderIndexGenerationNotVisible);
  }
  if (contract.index_generation.proof_present &&
      !contract.index_generation.covers_predicate) {
    AddRefusal(&selection, kNoSqlProviderIndexPredicateCoverageMissing);
  }

  if (contract.provider_generation.required) {
    if (!contract.provider_generation.proof_present) {
      AddMissing(&selection, kNoSqlProviderGenerationProofMissing);
    }
    if (contract.provider_generation.proof_present &&
        contract.provider_generation.available_generation <
            contract.provider_generation.required_generation) {
      AddRefusal(&selection, kNoSqlProviderGenerationStale);
    }
    if (contract.provider_generation.proof_present &&
        !contract.provider_generation.visible_to_snapshot) {
      AddRefusal(&selection, kNoSqlProviderGenerationUnavailable);
    }
    if (contract.provider_generation.proof_present &&
        (!contract.provider_generation.publish_state_bound ||
         !contract.provider_generation.validation_state_bound ||
         contract.provider_generation.publish_state != "published" ||
         contract.provider_generation.validation_state != "validated")) {
      AddRefusal(&selection, kNoSqlProviderGenerationStateUnvalidated);
    }
    if (contract.provider_generation.proof_present &&
        (!contract.provider_generation.backup_restore_repair_metadata_bound ||
         !contract.provider_generation.support_bundle_evidence_bound ||
         contract.provider_generation.backup_metadata_ref.empty() ||
         contract.provider_generation.restore_metadata_ref.empty() ||
         contract.provider_generation.repair_metadata_ref.empty() ||
         contract.provider_generation.support_bundle_evidence_id.empty())) {
      AddMissing(&selection, kNoSqlProviderGenerationMetadataMissing);
    }
    if (contract.provider_generation.proof_present &&
        (contract.provider_generation.provider_claims_transaction_finality_authority ||
         contract.provider_generation.provider_claims_visibility_authority)) {
      AddRefusal(&selection, kNoSqlProviderGenerationAuthorityRefused);
    }
  }

  if (contract.delta_overlay.required) {
    if (!contract.delta_overlay.proof_present) {
      AddMissing(&selection, kNoSqlProviderDeltaOverlayProofMissing);
    } else if (!contract.delta_overlay.covers_snapshot) {
      AddRefusal(&selection, kNoSqlProviderDeltaOverlaySnapshotCoverageMissing);
    }
  }

  if (!contract.policy.proof_present) {
    AddMissing(&selection, kNoSqlProviderPolicyProofMissing);
  }
  if (contract.policy.proof_present && !contract.policy.allowed) {
    AddRefusal(&selection, kNoSqlProviderPolicyRefused);
  }
  for (const auto& reason : contract.policy.refusal_reasons) {
    AddPolicyRefusal(&selection, reason);
  }

  if (!contract.mga_recheck.proof_present) {
    AddMissing(&selection, kNoSqlProviderMgaRecheckProofMissing);
  }
  if (contract.mga_recheck.proof_present &&
      !contract.mga_recheck.row_mga_recheck_required) {
    AddRefusal(&selection, kNoSqlProviderRowMgaRecheckRequired);
  }
  if (contract.mga_recheck.proof_present &&
      !contract.mga_recheck.row_security_recheck_required) {
    AddRefusal(&selection, kNoSqlProviderSecurityRecheckRequired);
  }
  if (contract.mga_recheck.provider_claims_transaction_finality_authority ||
      contract.mga_recheck.index_claims_transaction_finality_authority ||
      contract.mga_recheck.delta_overlay_claims_transaction_finality_authority) {
    AddRefusal(&selection, kNoSqlProviderFinalityAuthorityRefused);
  }
  if (contract.mga_recheck.provider_claims_visibility_authority) {
    AddRefusal(&selection, kNoSqlProviderVisibilityAuthorityRefused);
  }
  if (contract.mga_recheck.parser_claims_transaction_finality_authority) {
    AddRefusal(&selection, kNoSqlProviderParserFinalityAuthorityRefused);
  }
  if (contract.mga_recheck.write_ahead_log_claims_transaction_finality_authority) {  // wal-not-authority
    AddRefusal(&selection, kNoSqlProviderWriteAheadFinalityAuthorityRefused);
  }

  selection.ok = selection.missing_diagnostics.empty() &&
                 selection.refusal_diagnostics.empty();
  selection.selected = selection.ok;
  selection.fail_closed = !selection.ok;
  if (selection.selected) {
    selection.selected_provider_id = contract.provider_id;
    Add(&selection.evidence, "selected_provider_id=" + contract.provider_id);
    Add(&selection.evidence, "selected_access=local_physical_provider");
    AddBoolEvidence(&selection.evidence, "fallback_selected", false);
    if (!contract.fallback_provider_id.empty()) {
      Add(&selection.evidence, "fallback_provider_id=" + contract.fallback_provider_id);
    } else {
      Add(&selection.evidence, "fallback_provider_id=none");
    }
  } else {
    selection.selected_provider_id = "none";
    Add(&selection.evidence, "selected_provider_id=none");
    Add(&selection.evidence, "selected_access=fail_closed");
    AddBoolEvidence(&selection.evidence, "fallback_selected", false);
    Add(&selection.evidence, "fallback_reason=fail_closed_missing_or_refused_provider_proof");
  }
  AddBoolEvidence(&selection.evidence, "provider_selected", selection.selected);
  AddBoolEvidence(&selection.evidence, "fail_closed", selection.fail_closed);
  return selection;
}

bool EngineNoSqlSelectionHasDiagnostic(
    const EngineNoSqlPhysicalProviderSelection& selection,
    std::string_view diagnostic_code) {
  const auto matches = [&](const std::string& value) {
    return value == diagnostic_code ||
           (value.size() > diagnostic_code.size() &&
            value.compare(0, diagnostic_code.size(), diagnostic_code) == 0 &&
            value[diagnostic_code.size()] == ':');
  };
  return std::any_of(selection.missing_diagnostics.begin(),
                     selection.missing_diagnostics.end(),
                     matches) ||
         std::any_of(selection.refusal_diagnostics.begin(),
                     selection.refusal_diagnostics.end(),
                     matches);
}

}  // namespace scratchbird::engine::internal_api
