// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_layout.hpp"

#include "database_format.hpp"

#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::storage::disk::IsSupportedDatabasePageSize;
using scratchbird::storage::disk::PageTypeName;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;

Status PageLayoutOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status PageLayoutErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

PageLayoutDescriptor Layout(PageType page_type,
                            PageFamily family,
                            std::string stable_name,
                            u32 body_header_bytes,
                            u32 slot_entry_bytes,
                            PageBodyGrowthDirection growth,
                            bool has_row_slots,
                            bool has_overflow_links,
                            bool supports_variable_payload,
                            bool supports_dense_internal_row_ordinals = false,
                            u32 minimum_page_size = 1024,
                            u32 minimum_free_bytes = 32) {
  PageLayoutDescriptor descriptor;
  descriptor.page_type = page_type;
  descriptor.family = family;
  descriptor.stable_name = std::move(stable_name);
  descriptor.body_header_bytes = body_header_bytes;
  descriptor.minimum_page_size = minimum_page_size;
  descriptor.slot_entry_bytes = slot_entry_bytes;
  descriptor.minimum_free_bytes = minimum_free_bytes;
  descriptor.growth = growth;
  descriptor.has_row_slots = has_row_slots;
  descriptor.has_overflow_links = has_overflow_links;
  descriptor.supports_variable_payload = supports_variable_payload;
  descriptor.supports_dense_internal_row_ordinals =
      supports_dense_internal_row_ordinals;
  return descriptor;
}

PageLayoutResult PageLayoutError(std::string diagnostic_code,
                                 std::string message_key,
                                 std::string detail = {}) {
  PageLayoutResult result;
  result.status = PageLayoutErrorStatus();
  result.diagnostic = MakePageLayoutDiagnostic(result.status,
                                               std::move(diagnostic_code),
                                               std::move(message_key),
                                               std::move(detail));
  return result;
}

}  // namespace

const char* PageBodyGrowthDirectionName(PageBodyGrowthDirection growth) {
  switch (growth) {
    case PageBodyGrowthDirection::forward: return "forward";
    case PageBodyGrowthDirection::split_fixed_variable: return "split_fixed_variable";
    case PageBodyGrowthDirection::append_segments: return "append_segments";
    case PageBodyGrowthDirection::opaque: return "opaque";
    case PageBodyGrowthDirection::unknown: return "unknown";
  }
  return "unknown";
}

const std::vector<PageLayoutDescriptor>& BuiltinPageLayoutRegistry() {
  static const std::vector<PageLayoutDescriptor> layouts = {
      Layout(PageType::database_header, PageFamily::startup, "database_header", 0, 0, PageBodyGrowthDirection::opaque, false, false, false),
      Layout(PageType::allocation_map, PageFamily::allocation, "allocation_map", 64, 8, PageBodyGrowthDirection::forward, true, false, false),
      Layout(PageType::catalog, PageFamily::catalog, "catalog", 64, 20, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::transaction_inventory, PageFamily::transaction, "transaction_inventory", 64, 32, PageBodyGrowthDirection::forward, true, true, false),
      Layout(PageType::row_data, PageFamily::data, "row_data", 96, 16, PageBodyGrowthDirection::split_fixed_variable, true, true, true, true),
      Layout(PageType::index_btree, PageFamily::index, "index_btree", 96, 16, PageBodyGrowthDirection::split_fixed_variable, true, true, true),
      Layout(PageType::index_btree_root, PageFamily::index, "index_btree_root", 128, 20, PageBodyGrowthDirection::split_fixed_variable, true, true, true),
      Layout(PageType::index_btree_branch, PageFamily::index, "index_btree_branch", 128, 20, PageBodyGrowthDirection::split_fixed_variable, true, true, true),
      Layout(PageType::index_btree_leaf, PageFamily::index, "index_btree_leaf", 128, 20, PageBodyGrowthDirection::split_fixed_variable, true, true, true),
      Layout(PageType::index_btree_posting, PageFamily::index, "index_btree_posting", 96, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_hash, PageFamily::index, "index_hash", 128, 24, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_bitmap, PageFamily::index, "index_bitmap", 128, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_summary, PageFamily::index, "index_summary", 128, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_inverted, PageFamily::index, "index_inverted", 128, 20, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_spatial, PageFamily::index, "index_spatial", 128, 24, PageBodyGrowthDirection::split_fixed_variable, true, true, true),
      Layout(PageType::index_vector, PageFamily::index, "index_vector", 128, 24, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_graph, PageFamily::index, "index_graph", 128, 24, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_temporary, PageFamily::index, "index_temporary", 96, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_statistics, PageFamily::index, "index_statistics", 96, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::index_special_root, PageFamily::index, "index_special_root", 128, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::blob, PageFamily::blob, "blob", 80, 12, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::metrics, PageFamily::metrics, "metrics", 64, 16, PageBodyGrowthDirection::forward, true, true, true),
      Layout(PageType::archive, PageFamily::archive, "archive", 80, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::columnar, PageFamily::columnar, "columnar", 128, 12, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::vector, PageFamily::vector, "vector", 128, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::graph, PageFamily::graph, "graph", 128, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::system_state, PageFamily::startup, "system_state", 64, 8, PageBodyGrowthDirection::forward, true, true, false),
      Layout(PageType::bootstrap_reserved, PageFamily::startup, "bootstrap_reserved", 0, 0, PageBodyGrowthDirection::opaque, false, false, false),
      Layout(PageType::filespace_directory, PageFamily::startup, "filespace_directory", 64, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::config_root, PageFamily::catalog, "config_root", 64, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::security_root, PageFamily::catalog, "security_root", 64, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::reserved_local, PageFamily::reserved, "reserved_local", 0, 0, PageBodyGrowthDirection::opaque, false, false, false),
      Layout(PageType::cluster_decision, PageFamily::cluster_private, "cluster_decision", 96, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::cluster_route, PageFamily::cluster_private, "cluster_route", 96, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::cluster_catalog, PageFamily::cluster_private, "cluster_catalog", 96, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::cluster_transaction, PageFamily::cluster_private, "cluster_transaction", 96, 16, PageBodyGrowthDirection::append_segments, true, true, true),
      Layout(PageType::encrypted_opaque, PageFamily::encrypted_or_opaque, "encrypted_opaque", 0, 0, PageBodyGrowthDirection::opaque, false, false, false),
  };
  return layouts;
}

PageLayoutResult LookupPageLayout(PageType page_type) {
  for (const PageLayoutDescriptor& descriptor : BuiltinPageLayoutRegistry()) {
    if (descriptor.page_type == page_type) {
      PageLayoutResult result;
      result.status = PageLayoutOkStatus();
      result.descriptor = descriptor;
      return result;
    }
  }
  return PageLayoutError("SB-PAGE-LAYOUT-UNKNOWN-PAGE-TYPE",
                         "storage.page_layout.unknown_page_type",
                         PageTypeName(page_type));
}

PageLayoutResult ComputePageLayoutCapacity(PageType page_type, u32 page_size) {
  if (!IsSupportedDatabasePageSize(page_size)) {
    return PageLayoutError("SB-PAGE-LAYOUT-PAGE-SIZE-INVALID",
                           "storage.page_layout.page_size_invalid",
                           std::to_string(page_size));
  }
  PageLayoutResult lookup = LookupPageLayout(page_type);
  if (!lookup.ok()) {
    return lookup;
  }
  if (page_size < lookup.descriptor.minimum_page_size) {
    return PageLayoutError("SB-PAGE-LAYOUT-PAGE-SIZE-BELOW-MINIMUM",
                           "storage.page_layout.page_size_below_minimum",
                           lookup.descriptor.stable_name);
  }

  PageLayoutCapacity capacity;
  capacity.descriptor = lookup.descriptor;
  capacity.page_size = page_size;
  capacity.body_bytes = page_size - kPageHeaderSerializedBytes;
  capacity.usable_payload_bytes = capacity.body_bytes > lookup.descriptor.body_header_bytes + lookup.descriptor.minimum_free_bytes
                                      ? capacity.body_bytes - lookup.descriptor.body_header_bytes - lookup.descriptor.minimum_free_bytes
                                      : 0;
  capacity.approximate_minimum_rows = lookup.descriptor.slot_entry_bytes == 0
                                          ? 0
                                          : capacity.usable_payload_bytes / lookup.descriptor.slot_entry_bytes;

  PageLayoutResult result;
  result.status = PageLayoutOkStatus();
  result.descriptor = lookup.descriptor;
  result.capacity = capacity;
  return result;
}

DiagnosticRecord MakePageLayoutDiagnostic(Status status,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.layout");
}

}  // namespace scratchbird::storage::page
