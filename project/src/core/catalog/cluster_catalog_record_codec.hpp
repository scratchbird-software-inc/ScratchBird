// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CLUSTER_RECORD_CODEC
#include "catalog_page.hpp"
#include "cluster_catalog_manifest.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::catalog {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::storage::page::CatalogPageRow;

inline constexpr u32 kClusterCatalogRecordCodecVersionCurrent = 1;
inline constexpr u32 kClusterCatalogRecordCodecVersionMinSupported = 1;
inline constexpr u32 kClusterCatalogRecordCodecVersionMaxSupported = 1;
inline constexpr u32 kClusterCatalogRecordMaxPayloadBytes = 64u * 1024u;

struct ClusterCatalogFieldValue {
  std::string column_name;
  std::string value;
};

struct ClusterCatalogRecord {
  std::string table_path;
  u32 codec_version = kClusterCatalogRecordCodecVersionCurrent;
  u32 schema_version = kClusterCatalogManifestVersionCurrent;
  std::vector<ClusterCatalogFieldValue> fields;
};

struct ClusterCatalogNameResolverRow {
  std::string row_uuid;
  std::string target_record_uuid;
  std::string target_table_path;
  std::string language_tag;
  std::string identifier_profile_uuid;
  std::string name_class;
  std::string raw_name_text;
  std::string display_name;
  std::string normalized_lookup_key;
  std::string exact_lookup_key;
  std::string full_path_lookup_key;
  u32 catalog_generation = 0;
};

struct ClusterCatalogCommentResolverRow {
  std::string row_uuid;
  std::string comment_uuid;
  std::string target_record_uuid;
  std::string target_table_path;
  std::string language_tag;
  std::string comment_text;
  u32 catalog_generation = 0;
};

struct ClusterCatalogRecordSet {
  std::vector<ClusterCatalogRecord> records;
  std::vector<ClusterCatalogNameResolverRow> name_resolver_rows;
  std::vector<ClusterCatalogCommentResolverRow> comment_resolver_rows;
};

struct ClusterCatalogRecordCodecResult {
  Status status;
  ClusterCatalogRecord record;
  std::vector<byte> encoded;
  CatalogPageRow row;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct ClusterCatalogRecordSetValidationResult {
  Status status;
  std::vector<std::string> accepted_record_uuids;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const ClusterCatalogTableManifest* FindBuiltinClusterCatalogTableManifestByPath(
    std::string_view table_path);
std::string ClusterCatalogRecordPrimaryUuidColumn(
    const ClusterCatalogRecord& record);
std::string ClusterCatalogRecordPrimaryUuidValue(
    const ClusterCatalogRecord& record);
ClusterCatalogRecordCodecResult ValidateClusterCatalogRecord(
    const ClusterCatalogRecord& record);
ClusterCatalogRecordCodecResult EncodeClusterCatalogRecord(
    const ClusterCatalogRecord& record);
ClusterCatalogRecordCodecResult DecodeClusterCatalogRecord(
    const std::vector<byte>& encoded);
ClusterCatalogRecordCodecResult EncodeClusterCatalogRecordPageRow(
    const ClusterCatalogRecord& record,
    u32 ordinal);
ClusterCatalogRecordCodecResult DecodeClusterCatalogRecordPageRow(
    const CatalogPageRow& row);
ClusterCatalogRecordSetValidationResult ValidateClusterCatalogRecordSet(
    const ClusterCatalogRecordSet& record_set);
DiagnosticRecord MakeClusterCatalogRecordCodecDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::catalog
