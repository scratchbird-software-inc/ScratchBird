// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "damaged_page_quarantine.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status QuarantineOkStatus(Severity severity = Severity::info) {
  return {StatusCode::ok, severity, Subsystem::storage_page};
}

Status QuarantineErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_page};
}

bool EvidenceIndicatesDamage(const DamagedPageEvidence& evidence) {
  return evidence.agreement_kind != PageBodyAgreementKind::accepted ||
         !evidence.page_header_valid ||
         !evidence.page_body_checksum_valid ||
         !evidence.page_body_parse_valid ||
         !evidence.page_body_family_agreement_valid ||
         !evidence.page_uuid_valid;
}

void AddCommonEvidence(DamagedPageQuarantineDecision* decision,
                       const DamagedPageQuarantineRequest& request,
                       bool damaged) {
  decision->repair_event_digest = request.evidence.repair_event_digest;
  decision->repair_evidence_is_transaction_authority = false;
  decision->evidence.push_back(std::string("intent=") +
                               DamagedPageAccessIntentName(request.intent));
  decision->evidence.push_back(std::string("damaged_page=") +
                               (damaged ? "true" : "false"));
  decision->evidence.push_back(
      std::string("agreement_kind=") +
      PageBodyAgreementKindName(request.evidence.agreement_kind));
  decision->evidence.push_back(
      "durable_mga_inventory_authority_available=" +
      std::string(request.evidence.durable_mga_inventory_authority_available
                      ? "true"
                      : "false"));
  decision->evidence.push_back(
      "repair_event_persisted_before_access=" +
      std::string(request.evidence.repair_event_persisted_before_access
                      ? "true"
                      : "false"));
  decision->evidence.push_back("repair_event_digest=" +
                               std::to_string(request.evidence.repair_event_digest));
  decision->evidence.push_back("repair_evidence_transaction_authority=false");
}

DamagedPageQuarantineDecision Blocked(
    const DamagedPageQuarantineRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  const bool damaged = EvidenceIndicatesDamage(request.evidence);
  DamagedPageQuarantineDecision decision;
  decision.status = QuarantineErrorStatus();
  decision.classification = DamagedPageClassification::review_blocked;
  decision.review_blocked = true;
  decision.fail_closed = true;
  decision.quarantine_required = damaged;
  decision.diagnostic = MakeDamagedPageQuarantineDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  AddCommonEvidence(&decision, request, damaged);
  return decision;
}

DamagedPageQuarantineDecision Quarantined(
    const DamagedPageQuarantineRequest& request,
    DamagedPageClassification classification,
    std::string diagnostic_code,
    std::string message_key) {
  DamagedPageQuarantineDecision decision;
  decision.status = QuarantineOkStatus(Severity::warning);
  decision.classification = classification;
  decision.quarantine_required = true;
  decision.review_blocked = false;
  decision.fail_closed = false;
  decision.repair_scan_allowed =
      classification == DamagedPageClassification::repair_scan_allowed;
  decision.repair_mutation_allowed =
      classification == DamagedPageClassification::repair_mutation_allowed;
  decision.diagnostic = MakeDamagedPageQuarantineDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::to_string(request.page_number));
  AddCommonEvidence(&decision, request, true);
  return decision;
}

DamagedPageQuarantineDecision CleanNormalAccess(
    const DamagedPageQuarantineRequest& request) {
  DamagedPageQuarantineDecision decision;
  decision.status = QuarantineOkStatus();
  decision.classification = DamagedPageClassification::normal_access_allowed;
  decision.normal_access_allowed = true;
  decision.review_blocked = false;
  decision.fail_closed = false;
  decision.diagnostic = MakeDamagedPageQuarantineDiagnostic(
      decision.status,
      "SB-DAMAGED-PAGE-NORMAL-ACCESS-ALLOWED",
      "storage.damaged_page_quarantine.normal_access_allowed",
      std::to_string(request.page_number));
  AddCommonEvidence(&decision, request, false);
  return decision;
}

}  // namespace

const char* DamagedPageAccessIntentName(DamagedPageAccessIntent intent) {
  switch (intent) {
    case DamagedPageAccessIntent::normal_access: return "normal_access";
    case DamagedPageAccessIntent::repair_scan: return "repair_scan";
    case DamagedPageAccessIntent::repair_mutation: return "repair_mutation";
  }
  return "normal_access";
}

const char* DamagedPageClassificationName(
    DamagedPageClassification classification) {
  switch (classification) {
    case DamagedPageClassification::normal_access_allowed:
      return "normal_access_allowed";
    case DamagedPageClassification::repair_scan_allowed:
      return "repair_scan_allowed";
    case DamagedPageClassification::repair_mutation_allowed:
      return "repair_mutation_allowed";
    case DamagedPageClassification::quarantine_required:
      return "quarantine_required";
    case DamagedPageClassification::review_blocked:
      return "review_blocked";
  }
  return "review_blocked";
}

DamagedPageEvidence MakeDamagedPageEvidenceFromAgreement(
    const PageBodyAgreementResult& agreement) {
  DamagedPageEvidence evidence;
  evidence.agreement_kind = agreement.kind;
  evidence.page_header_valid =
      agreement.kind != PageBodyAgreementKind::header_refused;
  evidence.page_body_checksum_valid =
      agreement.kind != PageBodyAgreementKind::checksum_refused;
  evidence.page_body_parse_valid =
      agreement.kind != PageBodyAgreementKind::body_parse_refused &&
      agreement.kind != PageBodyAgreementKind::unsupported_body_kind;
  evidence.page_body_family_agreement_valid =
      agreement.kind == PageBodyAgreementKind::accepted &&
      agreement.body_family_agreement_valid;
  evidence.page_uuid_valid = !agreement.header.page_uuid.is_nil();
  return evidence;
}

DamagedPageQuarantineDecision ClassifyDamagedPageAccess(
    const DamagedPageQuarantineRequest& request) {
  const bool damaged = EvidenceIndicatesDamage(request.evidence);
  if (request.page_number == 0) {
    return Blocked(request,
                   "SB-DAMAGED-PAGE-NUMBER-REQUIRED",
                   "storage.damaged_page_quarantine.page_number_required");
  }
  if (!request.evidence.durable_mga_inventory_authority_available ||
      request.evidence.repair_evidence_is_transaction_authority ||
      request.evidence.parser_or_reference_authority ||
      request.evidence.names_are_authority) {
    return Blocked(request,
                   "SB-DAMAGED-PAGE-AUTHORITY-REFUSED",
                   "storage.damaged_page_quarantine.authority_refused");
  }

  if (!damaged) {
    if (request.intent == DamagedPageAccessIntent::normal_access) {
      return CleanNormalAccess(request);
    }
    if (!request.evidence.repair_event_persisted_before_access ||
        request.evidence.repair_event_digest == 0) {
      return Blocked(request,
                     "SB-DAMAGED-PAGE-REPAIR-EVENT-REQUIRED",
                     "storage.damaged_page_quarantine.repair_event_required");
    }
    return Quarantined(request,
                       request.intent == DamagedPageAccessIntent::repair_scan
                           ? DamagedPageClassification::repair_scan_allowed
                           : DamagedPageClassification::repair_mutation_allowed,
                       "SB-DAMAGED-PAGE-REPAIR-ACCESS-ALLOWED",
                       "storage.damaged_page_quarantine.repair_access_allowed");
  }

  if (!request.evidence.repair_event_persisted_before_access ||
      request.evidence.repair_event_digest == 0) {
    return Blocked(request,
                   "SB-DAMAGED-PAGE-REPAIR-EVENT-REQUIRED",
                   "storage.damaged_page_quarantine.repair_event_required");
  }
  if (request.evidence.operator_review_required ||
      !request.evidence.safe_quarantine_boundary_available) {
    return Blocked(request,
                   "SB-DAMAGED-PAGE-REVIEW-REQUIRED",
                   "storage.damaged_page_quarantine.review_required");
  }

  switch (request.intent) {
    case DamagedPageAccessIntent::normal_access:
      return Quarantined(request,
                         DamagedPageClassification::quarantine_required,
                         "SB-DAMAGED-PAGE-NORMAL-ACCESS-QUARANTINED",
                         "storage.damaged_page_quarantine.normal_access_quarantined");
    case DamagedPageAccessIntent::repair_scan:
      return Quarantined(request,
                         DamagedPageClassification::repair_scan_allowed,
                         "SB-DAMAGED-PAGE-REPAIR-SCAN-ALLOWED",
                         "storage.damaged_page_quarantine.repair_scan_allowed");
    case DamagedPageAccessIntent::repair_mutation:
      if (!request.evidence.normal_mga_visibility_recheck_available) {
        return Blocked(request,
                       "SB-DAMAGED-PAGE-MGA-RECHECK-REQUIRED",
                       "storage.damaged_page_quarantine.mga_recheck_required");
      }
      return Quarantined(request,
                         DamagedPageClassification::repair_mutation_allowed,
                         "SB-DAMAGED-PAGE-REPAIR-MUTATION-ALLOWED",
                         "storage.damaged_page_quarantine.repair_mutation_allowed");
  }
  return Blocked(request,
                 "SB-DAMAGED-PAGE-INTENT-INVALID",
                 "storage.damaged_page_quarantine.intent_invalid");
}

DiagnosticRecord MakeDamagedPageQuarantineDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.damaged_page_quarantine");
}

}  // namespace scratchbird::storage::page
