// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_page.hpp"
#include "cluster_catalog_record_codec.hpp"
#include "uuid.hpp"

// PUBLIC_CLUSTER_CODEC_RESOLVER_GATE

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::string MakeUuid(UuidKind kind, std::uint64_t offset) {
  const auto generated =
      uuid::GenerateDurableEngineIdentityV7(kind, 1720000000000ull + offset);
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

std::string PrimaryColumn(const catalog::ClusterCatalogTableManifest& table) {
  Require(table.primary_key_columns.size() == 1,
          "cluster table should expose exactly one primary UUID column");
  return table.primary_key_columns.front();
}

std::string ValueForColumn(const catalog::ClusterCatalogTableManifest& table,
                           const catalog::ClusterCatalogColumnManifest& column,
                           std::uint64_t ordinal) {
  if (column.type_name == "uuid") {
    const UuidKind kind =
        column.column_name == "cluster_uuid" ? UuidKind::cluster : UuidKind::object;
    return MakeUuid(kind, ordinal);
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
    const auto pos = table.table_name.find(prefix);
    Require(pos == 0, "role profile table name did not include role prefix");
    return table.table_name.substr(prefix.size());
  }
  if (column.column_name == "role_code") {
    return "storage";
  }
  if (column.type_name == "digest") {
    return "digest-" + Sanitize(table.stable_table_id) + "-" +
           std::to_string(ordinal);
  }
  return Sanitize(column.column_name) + "_" + std::to_string(ordinal);
}

catalog::ClusterCatalogRecord RecordForTable(
    const catalog::ClusterCatalogTableManifest& table,
    std::uint64_t base_ordinal) {
  catalog::ClusterCatalogRecord record;
  record.table_path = catalog::ClusterCatalogFullTablePath(table);
  record.codec_version = catalog::kClusterCatalogRecordCodecVersionCurrent;
  record.schema_version = table.manifest_version;
  std::uint64_t ordinal = base_ordinal;
  for (const auto& column : table.columns) {
    record.fields.push_back(
        {column.column_name, ValueForColumn(table, column, ordinal++)});
  }
  return record;
}

std::vector<catalog::ClusterCatalogRecord> BuiltinRecords() {
  std::vector<catalog::ClusterCatalogRecord> records;
  std::uint64_t ordinal = 1;
  for (const auto& table : catalog::BuiltinClusterCatalogTableManifests()) {
    records.push_back(RecordForTable(table, ordinal));
    ordinal += 100;
  }
  for (const auto& role_profile : catalog::BuiltinClusterRoleProfileManifests()) {
    records.push_back(RecordForTable(role_profile.table, ordinal));
    ordinal += 100;
  }
  return records;
}

catalog::ClusterCatalogNameResolverRow NameResolverFor(
    const catalog::ClusterCatalogRecord& record,
    std::uint64_t ordinal) {
  const std::string primary_uuid =
      catalog::ClusterCatalogRecordPrimaryUuidValue(record);
  const std::string object_name =
      Sanitize(record.table_path) + "_" + std::to_string(ordinal);
  catalog::ClusterCatalogNameResolverRow row;
  row.row_uuid = MakeUuid(UuidKind::row, 10000 + ordinal);
  row.target_record_uuid = primary_uuid;
  row.target_table_path = record.table_path;
  row.language_tag = "und";
  row.identifier_profile_uuid = MakeUuid(UuidKind::object, 11000 + ordinal);
  row.name_class = "system_identifier";
  row.raw_name_text = object_name;
  row.display_name = object_name;
  row.normalized_lookup_key = object_name;
  row.exact_lookup_key = object_name;
  row.full_path_lookup_key = record.table_path + "." + object_name;
  row.catalog_generation = 1;
  return row;
}

catalog::ClusterCatalogCommentResolverRow CommentResolverFor(
    const catalog::ClusterCatalogRecord& record,
    std::uint64_t ordinal) {
  catalog::ClusterCatalogCommentResolverRow row;
  row.row_uuid = MakeUuid(UuidKind::row, 20000 + ordinal);
  row.comment_uuid = MakeUuid(UuidKind::object, 21000 + ordinal);
  row.target_record_uuid = catalog::ClusterCatalogRecordPrimaryUuidValue(record);
  row.target_table_path = record.table_path;
  row.language_tag = "und";
  row.comment_text = "cluster catalog public proof " + std::to_string(ordinal);
  row.catalog_generation = 1;
  return row;
}

catalog::ClusterCatalogRecordSet ValidRecordSet() {
  catalog::ClusterCatalogRecordSet set;
  set.records = BuiltinRecords();
  std::uint64_t ordinal = 1;
  for (const auto& record : set.records) {
    set.name_resolver_rows.push_back(NameResolverFor(record, ordinal));
    set.comment_resolver_rows.push_back(CommentResolverFor(record, ordinal));
    ++ordinal;
  }
  return set;
}

void TestCodecRoundTripAndCatalogPageRows() {
  const auto records = BuiltinRecords();
  std::vector<page::CatalogPageRow> rows;
  std::uint32_t ordinal = 1;
  for (const auto& record : records) {
    const auto encoded =
        catalog::EncodeClusterCatalogRecordPageRow(record, ordinal++);
    Require(encoded.ok(), "cluster catalog record failed to encode");
    Require(encoded.row.kind == page::CatalogPageRowKind::cluster_catalog_record,
            "encoded row did not use cluster catalog row kind");
    const auto decoded =
        catalog::DecodeClusterCatalogRecordPageRow(encoded.row);
    Require(decoded.ok(), "cluster catalog record failed to decode");
    Require(decoded.record.table_path == record.table_path,
            "decoded table path mismatch");
    Require(decoded.record.fields.size() == record.fields.size(),
            "decoded field count mismatch");
    Require(catalog::ClusterCatalogRecordPrimaryUuidValue(decoded.record) ==
                catalog::ClusterCatalogRecordPrimaryUuidValue(record),
            "decoded primary UUID mismatch");
    rows.push_back(encoded.row);
  }

  const auto pages = page::BuildCatalogPageSet(rows, 8192, 40, 80);
  Require(pages.ok(), "cluster catalog rows did not serialize into catalog pages");
  std::uint32_t decoded_rows = 0;
  for (const auto& serialized : pages.pages) {
    const auto parsed =
        page::ParseCatalogPageBody(serialized.body, serialized.page_number);
    Require(parsed.ok(), "cluster catalog page body did not parse");
    for (const auto& row : parsed.body.rows) {
      Require(row.kind == page::CatalogPageRowKind::cluster_catalog_record,
              "parsed row kind was not cluster catalog record");
      Require(catalog::DecodeClusterCatalogRecordPageRow(row).ok(),
              "parsed cluster catalog row did not decode");
      ++decoded_rows;
    }
  }
  Require(decoded_rows == records.size(),
          "not all cluster catalog page rows round-tripped");
}

void TestResolverAndCommentAuthority() {
  const auto set = ValidRecordSet();
  const auto validated = catalog::ValidateClusterCatalogRecordSet(set);
  Require(validated.ok(), "valid cluster catalog record set was refused");
  Require(validated.accepted_record_uuids.size() == set.records.size(),
          "record set did not accept every cluster catalog record");

  auto missing_name = set;
  missing_name.name_resolver_rows.clear();
  const auto missing_name_result =
      catalog::ValidateClusterCatalogRecordSet(missing_name);
  Require(!missing_name_result.ok() &&
              missing_name_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-RESOLVER-MISSING",
          "record set did not reject missing identity resolver rows");

  auto missing_comment = set;
  missing_comment.comment_resolver_rows.clear();
  const auto missing_comment_result =
      catalog::ValidateClusterCatalogRecordSet(missing_comment);
  Require(!missing_comment_result.ok() &&
              missing_comment_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-COMMENT-MISSING",
          "record set did not reject missing comment resolver rows");

  auto reused = set;
  const std::string primary_column =
      catalog::ClusterCatalogRecordPrimaryUuidColumn(reused.records[1]);
  for (auto& field : reused.records[1].fields) {
    if (field.column_name == primary_column) {
      field.value =
          catalog::ClusterCatalogRecordPrimaryUuidValue(reused.records.front());
    }
  }
  const auto reused_result =
      catalog::ValidateClusterCatalogRecordSet(reused);
  Require(!reused_result.ok() &&
              reused_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-UUID-REUSED",
          "record set did not reject reused primary UUIDs");
}

void TestRefusals() {
  auto record = BuiltinRecords().front();

  auto embedded_name = record;
  embedded_name.fields.push_back({"display_name", "cluster node"});
  const auto embedded_name_result =
      catalog::EncodeClusterCatalogRecord(embedded_name);
  Require(!embedded_name_result.ok() &&
              embedded_name_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-USER-TEXT-REFUSED",
          "codec did not reject embedded user-layer name text");

  auto embedded_comment = record;
  embedded_comment.fields.push_back({"comment_text", "do not embed"});
  const auto embedded_comment_result =
      catalog::EncodeClusterCatalogRecord(embedded_comment);
  Require(!embedded_comment_result.ok() &&
              embedded_comment_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-USER-TEXT-REFUSED",
          "codec did not reject embedded comment text");

  auto embedded_description = record;
  embedded_description.fields.push_back({"description_text", "do not embed"});
  const auto embedded_description_result =
      catalog::EncodeClusterCatalogRecord(embedded_description);
  Require(!embedded_description_result.ok() &&
              embedded_description_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-USER-TEXT-REFUSED",
          "codec did not reject embedded description text");

  auto property_bag = record;
  property_bag.fields.push_back({"role_properties", "k=v"});
  const auto property_bag_result =
      catalog::EncodeClusterCatalogRecord(property_bag);
  Require(!property_bag_result.ok() &&
              property_bag_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-PROPERTY-BAG-REFUSED",
          "codec did not reject generic property bag role behavior");

  auto missing_status = record;
  missing_status.fields.erase(
      std::remove_if(missing_status.fields.begin(),
                     missing_status.fields.end(),
                     [](const catalog::ClusterCatalogFieldValue& field) {
                       return field.column_name == "status";
                     }),
      missing_status.fields.end());
  const auto missing_status_result =
      catalog::EncodeClusterCatalogRecord(missing_status);
  Require(!missing_status_result.ok() &&
              missing_status_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-REQUIRED-FIELD-MISSING",
          "codec did not reject missing required status field");

  auto encoded = catalog::EncodeClusterCatalogRecord(record);
  Require(encoded.ok(), "valid record did not encode before tamper tests");

  auto unsupported = encoded.encoded;
  scratchbird::core::platform::StoreLittle32(unsupported.data() + 8, 99);
  const auto unsupported_result =
      catalog::DecodeClusterCatalogRecord(unsupported);
  Require(!unsupported_result.ok() &&
              unsupported_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-VERSION-UNSUPPORTED",
          "codec did not reject unsupported codec version");

  auto length_mismatch = encoded.encoded;
  scratchbird::core::platform::StoreLittle32(length_mismatch.data() + 20, 1);
  const auto length_result =
      catalog::DecodeClusterCatalogRecord(length_mismatch);
  Require(!length_result.ok() &&
              length_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-LENGTH-MISMATCH",
          "codec did not reject payload length mismatch");

  auto checksum_mismatch = encoded.encoded;
  checksum_mismatch.back() ^= static_cast<byte>(0x1u);
  const auto checksum_result =
      catalog::DecodeClusterCatalogRecord(checksum_mismatch);
  Require(!checksum_result.ok() &&
              checksum_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-RECORD-CHECKSUM-MISMATCH",
          "codec did not reject checksum mismatch");
}

}  // namespace

int main() {
  TestCodecRoundTripAndCatalogPageRows();
  TestResolverAndCommentAuthority();
  TestRefusals();
  return EXIT_SUCCESS;
}
