// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "datatype_descriptor.hpp"
#include "datatype_exchange.hpp"
#include "datatype_layout.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

inline constexpr const char* kCurrentCoreDatatypeConformanceManifestKey =
    "MDF-015-CURRENT-CORE-DATATYPE-CONFORMANCE-MANIFEST";

enum class DatatypeConformanceExampleSource {
  current_core_registry,
  documentation_only,
  private_tracker,
  unknown
};

struct DatatypeConformanceExample {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  std::string stable_name;
  SerializedDatatypeDescriptor encoded_descriptor{};
  DatatypeStorageLayout storage_layout;
  DatatypeConformanceExampleSource source =
      DatatypeConformanceExampleSource::unknown;
  std::string evidence_path;
  std::string source_marker;
};

struct DatatypeConformanceManifest {
  std::string manifest_key;
  std::string inventory_source_path;
  std::vector<DatatypeConformanceExample> examples;
  bool parser_authority_allowed = false;
};

struct DatatypeConformanceManifestResult {
  Status status;
  DatatypeConformanceManifest manifest;
  DiagnosticRecord diagnostic;
  std::vector<DiagnosticRecord> diagnostics;
  std::size_t executed_examples = 0;

  bool ok() const {
    return status.ok() && diagnostics.empty();
  }
};

const char* DatatypeConformanceExampleSourceName(
    DatatypeConformanceExampleSource source);

DatatypeConformanceManifestResult LoadCurrentCoreDatatypeConformanceManifest();

DatatypeConformanceManifestResult ExecuteDatatypeConformanceManifest(
    const DatatypeConformanceManifest& manifest);

}  // namespace scratchbird::core::datatypes
