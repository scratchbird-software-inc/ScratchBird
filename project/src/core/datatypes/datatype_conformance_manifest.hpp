// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "datatype_bit_string.hpp"
#include "datatype_date.hpp"
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

// Bit strings cannot use the legacy SBDTV001 descriptor example above: that
// carrier has no V3 policy identities, live receipt, or profile material.
// Their conformance evidence therefore remains a separate exact carrier.
struct BitStringConformanceExampleV1 {
  BitStringAuthorityReceiptV1 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  BitStringDescriptorProfileV1 profile;
  bool null_allowed = false;
  BitStringValueStateV1 state = BitStringValueStateV1::present;
  std::vector<byte> canonical_component;
  DatatypeConformanceExampleSource source =
      DatatypeConformanceExampleSource::unknown;
  std::string evidence_path;
  std::string source_marker;
};

// Date likewise cannot use SBDTV001: the legacy descriptor cannot carry the
// d707 receipt, policy identities, or the complete 584-byte profile handle.
struct DateConformanceExampleV1 {
  DateAuthorityReceiptV1 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  DateValidatedProfileHandleV1 profile;
  bool null_allowed = false;
  DateValueStateV1 state = DateValueStateV1::value;
  std::vector<byte> canonical_component;
  DatatypeConformanceExampleSource source =
      DatatypeConformanceExampleSource::unknown;
  std::string evidence_path;
  std::string source_marker;
};

struct DatatypeConformanceManifest {
  std::string manifest_key;
  std::string inventory_source_path;
  std::vector<DatatypeConformanceExample> examples;
  std::vector<BitStringConformanceExampleV1> bit_string_examples;
  std::vector<DateConformanceExampleV1> date_examples;
  bool parser_authority_allowed = false;
};

struct DatatypeConformanceManifestResult {
  Status status;
  DatatypeConformanceManifest manifest;
  DiagnosticRecord diagnostic;
  std::vector<DiagnosticRecord> diagnostics;
  std::size_t executed_examples = 0;
  std::size_t executed_bit_string_examples = 0;
  std::size_t executed_date_examples = 0;

  bool ok() const {
    return status.ok() && diagnostics.empty();
  }
};

const char* DatatypeConformanceExampleSourceName(
    DatatypeConformanceExampleSource source);

DatatypeConformanceManifestResult LoadCurrentCoreDatatypeConformanceManifest(
    const BitStringAuthorityReceiptV1& bit_string_receipt,
    bool bit_string_null_allowed,
    const DateAuthorityReceiptV1& date_receipt,
    bool date_null_allowed);

DatatypeConformanceManifestResult ExecuteDatatypeConformanceManifest(
    const DatatypeConformanceManifest& manifest);

}  // namespace scratchbird::core::datatypes
