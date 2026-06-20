// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_integrity_verifier.hpp"

namespace scratchbird::storage::page {

IparIntegrityVerificationReport VerifyIparBackgroundIntegrity(
    const std::vector<IparIntegritySubject>& subjects) {
  IparIntegrityVerificationReport report;
  report.evidence.push_back("IPAR-P6-30");
  report.evidence.push_back("integrity_finality_authority=durable_mga_transaction_inventory");
  if (subjects.empty()) {
    report.diagnostics.push_back("IPAR_INTEGRITY_NO_SUBJECTS");
    return report;
  }
  report.accepted = true;
  for (const auto& subject : subjects) {
    if (subject.subject_uuid.empty() || subject.subject_kind.empty() ||
        subject.page_generation == 0) {
      report.diagnostics.push_back("IPAR_INTEGRITY_SUBJECT_INCOMPLETE");
      continue;
    }
    ++report.checked_count;
    if (!subject.checksum_valid) {
      report.diagnostics.push_back("IPAR_INTEGRITY_PAGE_CHECKSUM");
    }
    if (!subject.catalog_reference_valid) {
      report.diagnostics.push_back("IPAR_INTEGRITY_CATALOG_REFERENCE");
    }
    if (!subject.policy_reference_valid) {
      report.diagnostics.push_back("IPAR_INTEGRITY_POLICY_REFERENCE");
    }
    if (!subject.dependency_map_valid) {
      report.diagnostics.push_back("IPAR_INTEGRITY_DEPENDENCY_MAP");
    }
    if (!subject.committed_mga_visible) {
      report.diagnostics.push_back("IPAR_INTEGRITY_MGA_VISIBILITY_REFUSED");
    }
  }
  report.clean = report.accepted && report.diagnostics.empty();
  report.evidence.push_back("background_integrity_foreground_discovery=false");
  return report;
}

}  // namespace scratchbird::storage::page
