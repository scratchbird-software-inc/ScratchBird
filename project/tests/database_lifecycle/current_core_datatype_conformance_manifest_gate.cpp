// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_conformance_manifest.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
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

dt::BitStringAuthorityReceiptV3 BitStringReceipt() {
  dt::BitStringAuthorityReceiptV3 receipt;
  receipt.statement_receipt_uuid.bytes = {
      0x01, 0xa0, 0xff, 0x27, 0x45, 0x62, 0x7a, 0x11,
      0x8b, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  receipt.catalog_snapshot_uuid = dt::kDatatypeCohortV8;
  receipt.catalog_generation = 8;
  receipt.registry_generation = 8;
  return receipt;
}

dt::DateAuthorityReceiptV3 DateReceipt() {
  dt::DateAuthorityReceiptV3 receipt;
  receipt.statement_receipt_uuid = dt::kDatatypeCohortV8;
  receipt.catalog_snapshot_uuid = dt::kDatatypeCohortV8;
  receipt.catalog_generation = 8;
  receipt.registry_generation = 8;
  return receipt;
}

dt::TimeAuthorityReceiptV3 TimeReceipt() {
  dt::TimeAuthorityReceiptV3 receipt;
  receipt.statement_receipt_uuid = dt::kDatatypeCohortV8;
  receipt.catalog_snapshot_uuid = dt::kDatatypeCohortV8;
  receipt.catalog_generation = 8;
  receipt.registry_generation = 8;
  return receipt;
}

dt::DatatypeConformanceManifestResult LoadManifest() {
  return dt::LoadCurrentCoreDatatypeConformanceManifest(
      BitStringReceipt(), false, DateReceipt(), false, TimeReceipt(), false);
}

dt::SerializedDatatypeDescriptor EncodeDescriptorFixture(
    const dt::DatatypeDescriptor& descriptor) {
  dt::SerializedDatatypeDescriptor encoded{};
  std::copy(dt::kDatatypeDescriptorMagic.begin(),
            dt::kDatatypeDescriptorMagic.end(), encoded.begin());
  scratchbird::core::platform::StoreLittle32(
      encoded.data() + 8, static_cast<std::uint32_t>(descriptor.type_id));
  scratchbird::core::platform::StoreLittle16(
      encoded.data() + 12, static_cast<std::uint16_t>(descriptor.family));
  scratchbird::core::platform::StoreLittle16(
      encoded.data() + 14,
      static_cast<std::uint16_t>(descriptor.width_class));
  scratchbird::core::platform::StoreLittle32(encoded.data() + 16,
                                              descriptor.bit_width);
  scratchbird::core::platform::StoreLittle32(encoded.data() + 20,
                                              descriptor.default_precision);
  scratchbird::core::platform::StoreLittle32(encoded.data() + 24,
                                              descriptor.default_scale);
  std::uint32_t flags = descriptor.nullable_allowed ? 1u : 0u;
  flags |= descriptor.descriptor_authoritative ? 2u : 0u;
  flags |= descriptor.reference_name_is_alias_only ? 4u : 0u;
  flags |= descriptor.requires_mandatory_library ? 8u : 0u;
  scratchbird::core::platform::StoreLittle32(encoded.data() + 28, flags);
  const auto stable_name_bytes =
      std::min<std::size_t>(descriptor.stable_name.size(), 63);
  std::memcpy(encoded.data() + 32, descriptor.stable_name.data(),
              stable_name_bytes);
  return encoded;
}

void TestManifestLoadsAndExecutesAllCurrentCoreRows() {
  const auto loaded = LoadManifest();
  Require(loaded.ok(), "MDF-015 manifest loader must build without diagnostics");
  Require(loaded.manifest.manifest_key ==
              dt::kCurrentCoreDatatypeConformanceManifestKey,
          "MDF-015 manifest key mismatch");
  Require(loaded.manifest.inventory_source_path ==
              "project/src/core/datatypes/datatype_descriptor.cpp",
          "MDF-015 manifest inventory must be project source evidence");
  Require(loaded.manifest.examples.size() +
                  loaded.manifest.bit_string_examples.size() +
                  loaded.manifest.date_examples.size() +
                  loaded.manifest.time_examples.size() ==
              dt::BuiltinDatatypeDescriptors().size(),
          "MDF-015 manifest must inventory every canonical datatype row");
  Require(loaded.manifest.bit_string_examples.size() == 1,
          "MDF-015 must carry exactly one separate V3 bit-string example");
  const auto& bit = loaded.manifest.bit_string_examples.front();
  Require(dt::IsExactCanonicalBitStringTypeCodecIdentityV3(bit.identity),
          "MDF-015 bit-string example must carry the exact V3 identity");
  Require(bit.canonical_component.size() == 4,
          "MDF-015 bit-string example must use canonical empty PRESENT bytes");
  Require(loaded.manifest.date_examples.size() == 1,
          "MDF-015 must carry exactly one separate d708 date example");
  const auto& date = loaded.manifest.date_examples.front();
  Require(dt::IsExactCanonicalDateTypeCodecIdentityV3(date.identity),
          "MDF-015 date example must carry the exact d708 identity");
  Require(date.canonical_component == std::vector<scratchbird::core::platform::byte>(4, 0),
          "MDF-015 date example must use the exact epoch LE4 component");
  Require(loaded.manifest.time_examples.size() == 1,
          "MDF-015 must carry exactly one separate d708 time example");
  const auto& time = loaded.manifest.time_examples.front();
  Require(dt::IsExactCanonicalTimeTypeCodecIdentityV3(time.identity),
          "MDF-015 time example must carry the exact d708 identity");
  Require(time.canonical_component ==
              std::vector<scratchbird::core::platform::byte>(8, 0),
          "MDF-015 time example must use the exact midnight LE8 component");

  const auto executed =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(executed.ok(), "MDF-015 manifest examples must execute cleanly");
  Require(executed.executed_examples == dt::BuiltinDatatypeDescriptors().size(),
          "MDF-015 did not execute every encoded datatype example");
  Require(executed.executed_bit_string_examples == 1,
          "MDF-015 did not execute the exact V3 bit-string example");
  Require(executed.executed_date_examples == 1,
          "MDF-015 did not execute the exact d708 date example");
  Require(executed.executed_time_examples == 1,
          "MDF-015 did not execute the exact d708 time example");
}

void TestLegacyTimeEvidenceIsRefused() {
  const auto descriptor =
      dt::LookupDatatypeDescriptor(dt::CanonicalTypeId::time);
  Require(descriptor.ok(), "MDF-015 time descriptor row missing");
  const auto encoded = dt::SerializeDatatypeDescriptor(descriptor.descriptor);
  Require(!encoded.ok(), "MDF-015 admitted time through SBDTV001");
  Require(encoded.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy time serialization diagnostic mismatch");

  const auto exact_legacy = EncodeDescriptorFixture(descriptor.descriptor);
  const auto parsed = dt::ParseDatatypeDescriptor(exact_legacy);
  Require(!parsed.ok(), "MDF-015 parsed time through SBDTV001");
  Require(parsed.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy time parse diagnostic mismatch");

  const auto conversion = dt::DescribeDatatypeConversion(
      dt::CanonicalTypeId::time, dt::CanonicalTypeId::time);
  Require(!conversion.ok(),
          "MDF-015 admitted enum-derived time conversion evidence");
  Require(conversion.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy time conversion diagnostic mismatch");

  auto loaded = LoadManifest();
  loaded.manifest.time_examples[0].profile.profile_fingerprint[0] ^= 1u;
  const auto corrupt =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!corrupt.ok(), "MDF-015 accepted a mutated time profile");
  Require(HasDiagnostic(corrupt, "CTI.TEMPORAL.DESCRIPTOR_INVALID"),
          "MDF-015 mutated time profile diagnostic missing");

  loaded = LoadManifest();
  auto& dirty_null = loaded.manifest.time_examples[0];
  dirty_null.null_allowed = true;
  dirty_null.state = dt::TimeValueStateV3::sql_null;
  const auto invalid_null =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!invalid_null.ok(), "MDF-015 accepted time SQL NULL with bytes");
  Require(HasDiagnostic(invalid_null, "DATATYPE.NULL_STATE.INVALID"),
          "MDF-015 time NULL_STATE diagnostic missing");
}

void TestLegacyDateEvidenceIsRefused() {
  const auto descriptor =
      dt::LookupDatatypeDescriptor(dt::CanonicalTypeId::date);
  Require(descriptor.ok(), "MDF-015 date descriptor row missing");
  const auto encoded = dt::SerializeDatatypeDescriptor(descriptor.descriptor);
  Require(!encoded.ok(), "MDF-015 admitted date through SBDTV001");
  Require(encoded.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy date serialization diagnostic mismatch");

  const auto exact_legacy = EncodeDescriptorFixture(descriptor.descriptor);
  const auto parsed = dt::ParseDatatypeDescriptor(exact_legacy);
  Require(!parsed.ok(), "MDF-015 parsed date through SBDTV001");
  Require(parsed.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy date parse diagnostic mismatch");

  const auto conversion = dt::DescribeDatatypeConversion(
      dt::CanonicalTypeId::date, dt::CanonicalTypeId::date);
  Require(!conversion.ok(),
          "MDF-015 admitted enum-derived date conversion evidence");
  Require(conversion.diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy date conversion diagnostic mismatch");

  auto loaded = LoadManifest();
  loaded.manifest.date_examples[0].profile.profile_fingerprint[0] ^= 1u;
  const auto corrupt =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!corrupt.ok(), "MDF-015 accepted a mutated date profile");
  Require(HasDiagnostic(corrupt, "CTI.TEMPORAL.DESCRIPTOR_INVALID"),
          "MDF-015 mutated date profile diagnostic missing");

  loaded = LoadManifest();
  auto& dirty_null = loaded.manifest.date_examples[0];
  dirty_null.null_allowed = true;
  dirty_null.state = dt::DateValueStateV3::sql_null;
  const auto invalid_null =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!invalid_null.ok(), "MDF-015 accepted date SQL NULL with bytes");
  Require(HasDiagnostic(invalid_null, "DATATYPE.NULL_STATE.INVALID"),
          "MDF-015 date NULL_STATE diagnostic missing");
}

void TestLegacyBitStringEvidenceIsRefused() {
  const auto descriptor =
      dt::LookupDatatypeDescriptor(dt::CanonicalTypeId::bit_string);
  Require(descriptor.ok(), "MDF-015 bit-string descriptor row missing");
  const auto encoded = dt::SerializeDatatypeDescriptor(descriptor.descriptor);
  Require(!encoded.ok(), "MDF-015 admitted bit string through SBDTV001");
  Require(encoded.diagnostic.diagnostic_code ==
              "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy bit-string serialization diagnostic mismatch");

  const auto exact_legacy = EncodeDescriptorFixture(descriptor.descriptor);
  const auto parsed = dt::ParseDatatypeDescriptor(exact_legacy);
  Require(!parsed.ok(), "MDF-015 parsed bit string through SBDTV001");
  Require(parsed.diagnostic.diagnostic_code ==
              "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy bit-string parse diagnostic mismatch");
  auto malformed_legacy = exact_legacy;
  malformed_legacy[12] ^= 1u;
  const auto malformed = dt::ParseDatatypeDescriptor(malformed_legacy);
  Require(!malformed.ok(), "MDF-015 parsed malformed bit SBDTV001");
  Require(malformed.diagnostic.diagnostic_code ==
              "SB-DATATYPE-SERIALIZED-DESCRIPTOR-MISMATCH",
          "MDF-015 bit refusal masked malformed descriptor precedence");

  const auto conversion = dt::DescribeDatatypeConversion(
      dt::CanonicalTypeId::bit_string, dt::CanonicalTypeId::bit_string);
  Require(!conversion.ok(),
          "MDF-015 admitted name/enum-derived bit conversion evidence");
  Require(conversion.diagnostic.diagnostic_code ==
              "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
          "MDF-015 legacy bit conversion diagnostic mismatch");
  const auto unknown = static_cast<dt::CanonicalTypeId>(0xffffffffu);
  const auto unknown_source = dt::DescribeDatatypeConversion(
      unknown, dt::CanonicalTypeId::bit_string);
  const auto unknown_target = dt::DescribeDatatypeConversion(
      dt::CanonicalTypeId::bit_string, unknown);
  Require(!unknown_source.ok() && !unknown_target.ok(),
          "MDF-015 admitted unknown type paired with bit string");
  Require(unknown_source.diagnostic.diagnostic_code ==
              "SB-DATATYPE-CONVERSION-UNKNOWN-TYPE" &&
              unknown_target.diagnostic.diagnostic_code ==
              "SB-DATATYPE-CONVERSION-UNKNOWN-TYPE",
          "MDF-015 bit refusal masked unknown-type precedence");

  auto loaded = LoadManifest();
  Require(loaded.manifest.bit_string_examples.size() == 1,
          "MDF-015 bit-string example missing");
  loaded.manifest.bit_string_examples[0].profile.profile_fingerprint[0] ^= 1u;
  const auto corrupt =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!corrupt.ok(), "MDF-015 accepted a mutated V3 bit profile");
  Require(HasDiagnostic(corrupt, "CTB.BIT.DESCRIPTOR_INVALID"),
          "MDF-015 mutated V3 bit profile diagnostic missing");

  loaded = LoadManifest();
  loaded.manifest.bit_string_examples[0].receipt.statement_receipt_uuid
      .bytes[15] ^= 1u;
  const auto crossed_receipt =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!crossed_receipt.ok(),
          "MDF-015 accepted a substituted statement receipt");
  Require(HasDiagnostic(crossed_receipt, "CTB.BIT.DESCRIPTOR_INVALID"),
          "MDF-015 receipt-substitution diagnostic missing");

  loaded = LoadManifest();
  auto& nullable = loaded.manifest.bit_string_examples[0];
  nullable.state = dt::BitStringValueStateV3::sql_null;
  nullable.canonical_component.clear();
  const auto unauthorized_null =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!unauthorized_null.ok(),
          "MDF-015 silently granted bit-string nullability");
  Require(HasDiagnostic(unauthorized_null, "DATATYPE.NULL_NOT_ADMITTED"),
          "MDF-015 did not preserve NULL_NOT_ADMITTED ownership");

  loaded = LoadManifest();
  auto& dirty_null = loaded.manifest.bit_string_examples[0];
  dirty_null.null_allowed = true;
  dirty_null.state = dt::BitStringValueStateV3::sql_null;
  const auto invalid_null =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!invalid_null.ok(), "MDF-015 accepted SQL NULL with payload");
  Require(HasDiagnostic(invalid_null, "DATATYPE.NULL_STATE.INVALID"),
          "MDF-015 did not preserve NULL_STATE.INVALID ownership");
}

void TestManifestFailsWhenCanonicalRowIsMissing() {
  auto loaded = LoadManifest();
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
  auto loaded = LoadManifest();
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

  loaded = LoadManifest();
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
  auto loaded = LoadManifest();
  loaded.manifest.parser_authority_allowed = true;
  const auto parser_authority =
      dt::ExecuteDatatypeConformanceManifest(loaded.manifest);
  Require(!parser_authority.ok(),
          "MDF-015 accepted parser-authoritative manifest");
  Require(HasDiagnostic(
              parser_authority,
              "SB-DATATYPE-CONFORMANCE-PARSER-AUTHORITY-REFUSED"),
          "MDF-015 parser authority diagnostic not emitted");

  loaded = LoadManifest();
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
  TestLegacyBitStringEvidenceIsRefused();
  TestLegacyDateEvidenceIsRefused();
  TestLegacyTimeEvidenceIsRefused();
  std::cout << "current_core_datatype_conformance_manifest_gate=passed\n";
  return EXIT_SUCCESS;
}
