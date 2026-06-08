// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CODEC_PROPERTY_GATE

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "cluster_catalog_crypto_evidence.hpp"
#include "cluster_catalog_record_codec.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::byte;
using platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

template <typename Result>
void RequireDiagnostic(const Result& result,
                       std::string_view diagnostic_code,
                       std::string_view message) {
  Require(!result.ok(), message);
  Require(result.diagnostic.diagnostic_code == diagnostic_code, message);
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1770000000000ull + offset);
  Require(generated.ok(), "failed to generate engine UUID");
  return generated.value;
}

std::string MakeDurableUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateDurableEngineIdentityV7(kind, 1770000000000ull + offset);
  Require(generated.ok(), "failed to generate durable engine UUID");
  return uuid::UuidToString(generated.value.value);
}

std::string Sanitize(std::string value) {
  for (char& ch : value) {
    if (ch == '.') {
      ch = '_';
    }
  }
  return value;
}

std::string ValueForColumn(const catalog::ClusterCatalogTableManifest& table,
                           const catalog::ClusterCatalogColumnManifest& column,
                           u64 ordinal) {
  if (column.type_name == "uuid") {
    const UuidKind kind =
        column.column_name == "cluster_uuid" ? UuidKind::cluster
                                             : UuidKind::object;
    return MakeDurableUuid(kind, ordinal);
  }
  if (column.type_name == "uint64") {
    return std::to_string(1000 + ordinal);
  }
  if (column.type_name == "status_code") {
    return "active";
  }
  if (column.column_name == "role_code" &&
      table.record_family == "node_role_profile") {
    const std::string prefix = "node_role_profile_";
    Require(table.table_name.find(prefix) == 0,
            "role profile table name did not include role prefix");
    return table.table_name.substr(prefix.size());
  }
  if (column.column_name == "role_code") {
    return "storage";
  }
  if (column.type_name == "digest") {
    return "sha256:" + Sanitize(table.stable_table_id) + ":" +
           std::to_string(ordinal);
  }
  return Sanitize(column.column_name) + "_" + std::to_string(ordinal);
}

catalog::ClusterCatalogRecord ClusterRecordFixture() {
  const auto& tables = catalog::BuiltinClusterCatalogTableManifests();
  Require(!tables.empty(), "cluster catalog table manifest set is empty");
  const auto& table = tables.front();

  catalog::ClusterCatalogRecord record;
  record.table_path = catalog::ClusterCatalogFullTablePath(table);
  record.codec_version = catalog::kClusterCatalogRecordCodecVersionCurrent;
  record.schema_version = table.manifest_version;
  u64 ordinal = 1;
  for (const auto& column : table.columns) {
    record.fields.push_back(
        {column.column_name, ValueForColumn(table, column, ordinal++)});
  }
  return record;
}

void CatalogPageBodyProperties() {
  std::vector<page::CatalogPageRow> rows;
  rows.push_back(
      {page::CatalogPageRowKind::typed_catalog_record, 1, "alpha=one"});
  rows.push_back(
      {page::CatalogPageRowKind::typed_catalog_record, 2, "beta=two"});

  const auto too_small = page::BuildCatalogPageSet(rows, 64, 1, 2);
  RequireDiagnostic(too_small,
                    "SB-CATALOG-PAGE-BODY-PAGE-SIZE-TOO-SMALL",
                    "catalog page builder accepted undersized page");

  const auto row_too_large = page::BuildCatalogPageSet(
      {{page::CatalogPageRowKind::typed_catalog_record,
        1,
        std::string(512, 'x')}},
      512,
      1,
      2);
  RequireDiagnostic(row_too_large,
                    "SB-CATALOG-PAGE-BODY-ROW-TOO-LARGE",
                    "catalog page builder accepted oversized row");

  const auto page_set = page::BuildCatalogPageSet(rows, 1024, 10, 20);
  Require(page_set.ok(), "catalog page body did not build");
  Require(page_set.pages.size() == 1, "catalog page body unexpectedly split");

  const auto parsed =
      page::ParseCatalogPageBody(page_set.pages.front().body, 10);
  Require(parsed.ok(), "catalog page body did not parse");
  Require(parsed.body.rows.size() == rows.size(),
          "catalog page row count changed across round trip");

  auto bad_format = page_set.pages.front().body;
  platform::StoreLittle16(bad_format.data() + 8, 2);
  RequireDiagnostic(page::ParseCatalogPageBody(bad_format, 10),
                    "SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
                    "catalog page parser accepted unsupported format");

  auto checksum_mismatch = page_set.pages.front().body;
  checksum_mismatch.back() ^= static_cast<byte>(0x01u);
  RequireDiagnostic(page::ParseCatalogPageBody(checksum_mismatch, 10),
                    "SB-CATALOG-PAGE-BODY-CHECKSUM-MISMATCH",
                    "catalog page parser accepted body checksum mismatch");

  auto payload_mismatch = page_set.pages.front().body;
  const std::size_t payload_offset =
      page::kCatalogPageBodyHeaderBytes + 20;
  Require(payload_offset < payload_mismatch.size(),
          "catalog page test payload offset outside body");
  payload_mismatch[payload_offset] ^= static_cast<byte>(0x01u);
  platform::StoreLittle64(
      payload_mismatch.data() + 40,
      page::ComputeCatalogPageBodyChecksum(payload_mismatch));
  RequireDiagnostic(page::ParseCatalogPageBody(payload_mismatch, 10),
                    "SB-CATALOG-PAGE-BODY-PAYLOAD-CHECKSUM-MISMATCH",
                    "catalog page parser accepted row payload checksum mismatch");
}

void CatalogTypedRecordProperties() {
  catalog::CatalogTypedRecord record;
  record.header.kind = catalog::CatalogRecordKind::database;
  record.header.record_version = catalog::kCatalogRecordSchemaVersionCurrent;
  record.header.row_uuid = MakeUuid(UuidKind::row, 100);
  record.header.object_uuid = MakeUuid(UuidKind::object, 101);
  record.payload = "database_uuid=primary";

  const auto encoded = catalog::EncodeCatalogTypedRecord(record, 1);
  Require(encoded.ok(), "typed catalog record did not encode");
  const auto decoded = catalog::DecodeCatalogTypedRecord(encoded.row);
  Require(decoded.ok(), "typed catalog record did not decode");
  Require(decoded.record.header.kind == record.header.kind,
          "typed catalog record kind changed across round trip");
  Require(decoded.record.payload == record.payload,
          "typed catalog record payload changed across round trip");

  auto unsupported = record;
  unsupported.header.record_version =
      catalog::kCatalogRecordSchemaVersionMaxSupported + 1;
  RequireDiagnostic(
      catalog::EncodeCatalogTypedRecord(unsupported, 2),
      "SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED",
      "typed catalog record accepted unsupported schema version");

  auto wrong_row = encoded.row;
  wrong_row.kind = page::CatalogPageRowKind::bootstrap_object;
  RequireDiagnostic(catalog::DecodeCatalogTypedRecord(wrong_row),
                    "SB-CATALOG-RECORD-CODEC-ROW-KIND-INVALID",
                    "typed catalog record decoded wrong row kind");

  page::CatalogPageRow missing_fields;
  missing_fields.kind = page::CatalogPageRowKind::typed_catalog_record;
  missing_fields.ordinal = 3;
  missing_fields.payload = "kind=1\n";
  RequireDiagnostic(catalog::DecodeCatalogTypedRecord(missing_fields),
                    "SB-CATALOG-RECORD-CODEC-FIELDS-MISSING",
                    "typed catalog record decoded missing required fields");

  page::CatalogPageRow malformed_uuid;
  malformed_uuid.kind = page::CatalogPageRowKind::typed_catalog_record;
  malformed_uuid.ordinal = 4;
  malformed_uuid.payload =
      "kind=1\nrecord_version=1\ndeleted=0\nrow_uuid=not-a-uuid\n";
  Require(!catalog::DecodeCatalogTypedRecord(malformed_uuid).ok(),
          "typed catalog record decoded malformed row UUID");
}

void ClusterCatalogRecordProperties() {
  const auto record = ClusterRecordFixture();
  const auto encoded = catalog::EncodeClusterCatalogRecord(record);
  Require(encoded.ok(), "cluster catalog record did not encode");

  const auto decoded = catalog::DecodeClusterCatalogRecord(encoded.encoded);
  Require(decoded.ok(), "cluster catalog record did not decode");
  Require(decoded.record.table_path == record.table_path,
          "cluster catalog table path changed across round trip");
  Require(decoded.record.fields.size() == record.fields.size(),
          "cluster catalog field count changed across round trip");

  const auto row = catalog::EncodeClusterCatalogRecordPageRow(record, 1);
  Require(row.ok(), "cluster catalog record page row did not encode");
  Require(catalog::DecodeClusterCatalogRecordPageRow(row.row).ok(),
          "cluster catalog record page row did not decode");
  auto wrong_row = row.row;
  wrong_row.kind = page::CatalogPageRowKind::typed_catalog_record;
  RequireDiagnostic(catalog::DecodeClusterCatalogRecordPageRow(wrong_row),
                    "SB-CLUSTER-CATALOG-RECORD-ROW-KIND-INVALID",
                    "cluster catalog record decoded wrong row kind");

  auto unsupported = encoded.encoded;
  platform::StoreLittle32(unsupported.data() + 8, 99);
  RequireDiagnostic(catalog::DecodeClusterCatalogRecord(unsupported),
                    "SB-CLUSTER-CATALOG-RECORD-VERSION-UNSUPPORTED",
                    "cluster catalog record accepted unsupported codec version");

  auto length_mismatch = encoded.encoded;
  platform::StoreLittle32(length_mismatch.data() + 20, 1);
  RequireDiagnostic(catalog::DecodeClusterCatalogRecord(length_mismatch),
                    "SB-CLUSTER-CATALOG-RECORD-LENGTH-MISMATCH",
                    "cluster catalog record accepted payload length mismatch");

  auto checksum_mismatch = encoded.encoded;
  checksum_mismatch.back() ^= static_cast<byte>(0x01u);
  RequireDiagnostic(catalog::DecodeClusterCatalogRecord(checksum_mismatch),
                    "SB-CLUSTER-CATALOG-RECORD-CHECKSUM-MISMATCH",
                    "cluster catalog record accepted payload checksum mismatch");

  auto user_text = record;
  user_text.fields.push_back({"display_name", "do not embed"});
  RequireDiagnostic(catalog::EncodeClusterCatalogRecord(user_text),
                    "SB-CLUSTER-CATALOG-RECORD-USER-TEXT-REFUSED",
                    "cluster catalog record accepted embedded user text");

  auto property_bag = record;
  property_bag.fields.push_back({"role_properties", "k=v"});
  RequireDiagnostic(catalog::EncodeClusterCatalogRecord(property_bag),
                    "SB-CLUSTER-CATALOG-RECORD-PROPERTY-BAG-REFUSED",
                    "cluster catalog record accepted property bag");

  const auto* table =
      catalog::FindBuiltinClusterCatalogTableManifestByPath(record.table_path);
  Require(table != nullptr && !table->required_columns.empty(),
          "cluster catalog test table missing required columns");
  auto missing_required = record;
  const std::string required_column = table->required_columns.front();
  missing_required.fields.erase(
      std::remove_if(missing_required.fields.begin(),
                     missing_required.fields.end(),
                     [&required_column](
                         const catalog::ClusterCatalogFieldValue& field) {
                       return field.column_name == required_column;
                     }),
      missing_required.fields.end());
  RequireDiagnostic(catalog::EncodeClusterCatalogRecord(missing_required),
                    "SB-CLUSTER-CATALOG-RECORD-REQUIRED-FIELD-MISSING",
                    "cluster catalog record accepted missing required field");
}

void ClusterEvidenceIntegrityProperties() {
  catalog::ClusterCatalogEvidenceSubject subject;
  subject.kind = catalog::ClusterCatalogEvidenceKind::route;
  subject.subject_id = "pcr114-cluster-route";
  subject.table_path = "cluster.sys.catalog.route";
  subject.catalog_epoch = 114;
  subject.catalog_generation = 11401;
  subject.fields.push_back({"zeta", "2"});
  subject.fields.push_back({"alpha", "1"});

  auto reversed = subject;
  std::reverse(reversed.fields.begin(), reversed.fields.end());
  Require(catalog::CanonicalSerializeClusterCatalogEvidenceSubject(subject) ==
              catalog::CanonicalSerializeClusterCatalogEvidenceSubject(reversed),
          "cluster evidence canonical serialization changed with field order");

  catalog::ClusterCatalogCryptoEvidenceMetadata weak;
  weak.subject = subject;
  weak.canonical_payload =
      catalog::CanonicalSerializeClusterCatalogEvidenceSubject(subject);
  weak.algorithm = "crc32";
  weak.digest = "crc32:00112233";
  weak.provider_authority_claim = true;
  const auto validation =
      catalog::ValidateClusterCatalogCryptoEvidenceMetadata(weak);
  RequireDiagnostic(validation,
                    "SB-CLUSTER-CATALOG-CRYPTO-EVIDENCE-WEAK",
                    "cluster catalog accepted weak authority evidence");
  Require(validation.weak_evidence_rejected,
          "cluster crypto evidence did not mark weak evidence rejection");
}

}  // namespace

int main() {
  CatalogPageBodyProperties();
  CatalogTypedRecordProperties();
  ClusterCatalogRecordProperties();
  ClusterEvidenceIntegrityProperties();
  return EXIT_SUCCESS;
}
