// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_conformance_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace dt = scratchbird::core::datatypes;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasDiagnostic(const dt::DatatypeConformanceManifestResult& result,
                   std::string_view diagnostic_code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

void TestManifestLoadsAndExecutesAllCurrentCoreRows() {
  const auto loaded = dt::LoadCurrentCoreDatatypeConformanceManifest();
  Require(loaded.ok(), "MDF-015 manifest loader must build without diagnostics");
  Require(loaded.manifest.manifest_key ==
              dt::kCurrentCoreDatatypeConformanceManifestKey,
          "MDF-015 manifest key mismatch");
  Require(loaded.manifest.inventory_source_path ==
              "project/src/core/datatypes/datatype_descriptor.cpp",
          "MDF-015 manifest inventory must be project source evidence");
  Require(loaded.manifest.examples.size() ==
              dt::BuiltinDatatypeDescriptors().size(),
          "MDF-015 manifest must inventory every canonical datatype row");

  const auto executed =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(executed.ok(), "MDF-015 manifest examples must execute cleanly");
  Require(executed.executed_examples == dt::BuiltinDatatypeDescriptors().size(),
          "MDF-015 did not execute every encoded datatype example");
}

void TestManifestFailsWhenCanonicalRowIsMissing() {
  auto loaded = dt::LoadCurrentCoreDatatypeConformanceManifest();
  Require(!loaded.manifest.examples.empty(), "MDF-015 test manifest empty");
  loaded.manifest.examples.pop_back();

  const auto executed =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!executed.ok(), "MDF-015 accepted a missing canonical row");
  Require(HasDiagnostic(executed,
                        "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING"),
          "MDF-015 missing-row diagnostic not emitted");
}

void TestDocumentationOnlyAndPrivateExamplesAreRejected() {
  auto loaded = dt::LoadCurrentCoreDatatypeConformanceManifest();
  Require(!loaded.manifest.examples.empty(), "MDF-015 test manifest empty");
  loaded.manifest.examples[0].source =
      dt::DatatypeConformanceExampleSource::documentation_only;
  loaded.manifest.examples[0].evidence_path =
      "docs/documentation/draft/Language_Reference/data_types/type_system_overview.md";

  const auto docs_only =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!docs_only.ok(), "MDF-015 accepted documentation-only example");
  Require(HasDiagnostic(docs_only,
                        "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED"),
          "MDF-015 docs-only diagnostic not emitted");

  loaded = dt::LoadCurrentCoreDatatypeConformanceManifest();
  loaded.manifest.examples[0].source =
      dt::DatatypeConformanceExampleSource::private_tracker;
  loaded.manifest.examples[0].evidence_path =
      std::string("ScratchBird") +
      "-Private/docs/migration/final-deferred-implementation-tracker.md";

  const auto private_only =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!private_only.ok(), "MDF-015 accepted private tracker example");
  Require(HasDiagnostic(private_only,
                        "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED"),
          "MDF-015 private tracker diagnostic not emitted");
}

void TestParserAuthorityAndCorruptEncodingAreRejected() {
  auto loaded = dt::LoadCurrentCoreDatatypeConformanceManifest();
  loaded.manifest.parser_authority_allowed = true;
  const auto parser_authority =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!parser_authority.ok(),
          "MDF-015 accepted parser-authoritative manifest");
  Require(HasDiagnostic(
              parser_authority,
              "SB-DATATYPE-CONFORMANCE-PARSER-AUTHORITY-REFUSED"),
          "MDF-015 parser authority diagnostic not emitted");

  loaded = dt::LoadCurrentCoreDatatypeConformanceManifest();
  loaded.manifest.examples[0].encoded_descriptor[0] = 0;
  const auto corrupt =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!corrupt.ok(), "MDF-015 accepted corrupt encoded example");
  Require(HasDiagnostic(corrupt,
                        "SB-DATATYPE-CONFORMANCE-ENCODED-EXAMPLE-REFUSED"),
          "MDF-015 corrupt example diagnostic not emitted");
}

}  // namespace

int main() {
  // MDF-015-CURRENT-CORE-DATATYPE-CONFORMANCE-MANIFEST
  // DEFER-DPE-EXAMPLE-CORPUS
  // DEFER-DTYPE-CONFORMANCE-MANIFESTS
  TestManifestLoadsAndExecutesAllCurrentCoreRows();
  TestManifestFailsWhenCanonicalRowIsMissing();
  TestDocumentationOnlyAndPrivateExamplesAreRejected();
  TestParserAuthorityAndCorruptEncodingAreRejected();
  std::cout << "current_core_datatype_conformance_manifest_gate=passed\n";
  return EXIT_SUCCESS;
}
