// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_registry.hpp"

#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::storage::disk::PageClassificationKindName;
using scratchbird::storage::disk::PageTypeName;

Status PageRegistryOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status PageRegistryWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::storage_page};
}

Status PageRegistryErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

PageFamilyDescriptor Descriptor(PageType page_type,
                                PageFamily family,
                                std::string stable_name,
                                bool read,
                                bool write,
                                bool cluster_only = false,
                                bool encrypted_or_opaque = false,
                                bool reserved = false) {
  PageFamilyDescriptor descriptor;
  descriptor.page_type = page_type;
  descriptor.family = family;
  descriptor.stable_name = std::move(stable_name);
  descriptor.supported_local_read = read;
  descriptor.supported_local_write = write;
  descriptor.cluster_only = cluster_only;
  descriptor.encrypted_or_opaque = encrypted_or_opaque;
  descriptor.reserved = reserved;
  return descriptor;
}

}  // namespace

const char* PageFamilyName(PageFamily family) {
  switch (family) {
    case PageFamily::startup: return "startup";
    case PageFamily::allocation: return "allocation";
    case PageFamily::catalog: return "catalog";
    case PageFamily::transaction: return "transaction";
    case PageFamily::data: return "data";
    case PageFamily::index: return "index";
    case PageFamily::blob: return "blob";
    case PageFamily::metrics: return "metrics";
    case PageFamily::archive: return "archive";
    case PageFamily::columnar: return "columnar";
    case PageFamily::vector: return "vector";
    case PageFamily::graph: return "graph";
    case PageFamily::cluster_private: return "cluster_private";
    case PageFamily::encrypted_or_opaque: return "encrypted_or_opaque";
    case PageFamily::reserved: return "reserved";
    case PageFamily::unknown: return "unknown";
  }
  return "unknown";
}

bool IsKnownPageFamilyName(const std::string& stable_name) {
  if (stable_name.empty()) {
    return false;
  }
  for (const PageFamilyDescriptor& descriptor : BuiltinPageFamilyRegistry()) {
    if (descriptor.stable_name == stable_name || PageFamilyName(descriptor.family) == stable_name) {
      return descriptor.supported_local_read || descriptor.supported_local_write || descriptor.reserved;
    }
  }
  return false;
}

const std::vector<PageFamilyDescriptor>& BuiltinPageFamilyRegistry() {
  static const std::vector<PageFamilyDescriptor> registry = {
      Descriptor(PageType::database_header, PageFamily::startup, "database_header", true, true),
      Descriptor(PageType::allocation_map, PageFamily::allocation, "allocation_map", true, true),
      Descriptor(PageType::catalog, PageFamily::catalog, "catalog", true, true),
      Descriptor(PageType::transaction_inventory, PageFamily::transaction, "transaction_inventory", true, true),
      Descriptor(PageType::row_data, PageFamily::data, "row_data", true, true),
      Descriptor(PageType::index_btree, PageFamily::index, "index_btree", true, true),
      Descriptor(PageType::index_btree_root, PageFamily::index, "index_btree_root", true, true),
      Descriptor(PageType::index_btree_branch, PageFamily::index, "index_btree_branch", true, true),
      Descriptor(PageType::index_btree_leaf, PageFamily::index, "index_btree_leaf", true, true),
      Descriptor(PageType::index_btree_posting, PageFamily::index, "index_btree_posting", true, true),
      Descriptor(PageType::index_hash, PageFamily::index, "index_hash", true, true),
      Descriptor(PageType::index_bitmap, PageFamily::index, "index_bitmap", true, true),
      Descriptor(PageType::index_summary, PageFamily::index, "index_summary", true, true),
      Descriptor(PageType::index_inverted, PageFamily::index, "index_inverted", true, true),
      Descriptor(PageType::index_spatial, PageFamily::index, "index_spatial", true, true),
      Descriptor(PageType::index_vector, PageFamily::index, "index_vector", true, true),
      Descriptor(PageType::index_graph, PageFamily::index, "index_graph", true, true),
      Descriptor(PageType::index_temporary, PageFamily::index, "index_temporary", true, true),
      Descriptor(PageType::index_statistics, PageFamily::index, "index_statistics", true, true),
      Descriptor(PageType::index_special_root, PageFamily::index, "index_special_root", true, true),
      Descriptor(PageType::blob, PageFamily::blob, "blob", true, true),
      Descriptor(PageType::metrics, PageFamily::metrics, "metrics", true, true),
      Descriptor(PageType::archive, PageFamily::archive, "archive", true, true),
      Descriptor(PageType::columnar, PageFamily::columnar, "columnar", true, true),
      Descriptor(PageType::vector, PageFamily::vector, "vector", true, true),
      Descriptor(PageType::graph, PageFamily::graph, "graph", true, true),
      Descriptor(PageType::system_state, PageFamily::startup, "system_state", true, true),
      Descriptor(PageType::bootstrap_reserved, PageFamily::startup, "bootstrap_reserved", true, false),
      Descriptor(PageType::filespace_directory, PageFamily::startup, "filespace_directory", true, true),
      Descriptor(PageType::config_root, PageFamily::catalog, "config_root", true, true),
      Descriptor(PageType::security_root, PageFamily::catalog, "security_root", true, true),
      Descriptor(PageType::reserved_local, PageFamily::reserved, "reserved_local", true, false, false, false, true),
      Descriptor(PageType::cluster_decision, PageFamily::cluster_private, "cluster_decision", true, false, true),
      Descriptor(PageType::cluster_route, PageFamily::cluster_private, "cluster_route", true, false, true),
      Descriptor(PageType::cluster_catalog, PageFamily::cluster_private, "cluster_catalog", true, false, true),
      Descriptor(PageType::cluster_transaction, PageFamily::cluster_private, "cluster_transaction", true, false, true),
      Descriptor(PageType::encrypted_opaque, PageFamily::encrypted_or_opaque, "encrypted_opaque", true, false, false, true),
  };
  return registry;
}

PageRegistryLookupResult LookupPageFamily(PageType page_type) {
  for (const PageFamilyDescriptor& descriptor : BuiltinPageFamilyRegistry()) {
    if (descriptor.page_type == page_type) {
      PageRegistryLookupResult result;
      result.status = PageRegistryOkStatus();
      result.descriptor = descriptor;
      return result;
    }
  }

  PageRegistryLookupResult result;
  result.status = PageRegistryErrorStatus();
  result.descriptor = Descriptor(page_type, PageFamily::unknown, PageTypeName(page_type), false, false);
  result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                "SB-PAGE-REGISTRY-UNKNOWN-PAGE-TYPE",
                                                "page.registry.unknown_page_type",
                                                PageTypeName(page_type));
  return result;
}

PageManagerClassification ClassifyForPageManager(const PageClassification& header_classification) {
  PageManagerClassification result;
  result.status = PageRegistryOkStatus();
  result.header_classification = header_classification;

  PageRegistryLookupResult lookup = LookupPageFamily(header_classification.page_type);
  result.descriptor = lookup.descriptor;

  if (!header_classification.ok()) {
    result.status = PageRegistryErrorStatus();
    result.may_read_body = false;
    result.may_write_body = false;
    result.requires_cluster_authority = false;
    result.requires_decryption = false;
    result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                   "SB-PAGE-REGISTRY-HEADER-CLASSIFICATION-FAILED",
                                                   "page.registry.header_classification_failed",
                                                   PageClassificationKindName(header_classification.kind));
    return result;
  }

  result.requires_cluster_authority = header_classification.cluster_authority_required || result.descriptor.cluster_only;
  result.requires_decryption = header_classification.decryption_required || result.descriptor.encrypted_or_opaque;
  result.may_read_body = header_classification.readable && result.descriptor.supported_local_read;
  result.may_write_body = header_classification.writable && result.descriptor.supported_local_write &&
                          !result.requires_cluster_authority && !result.requires_decryption &&
                          !result.descriptor.reserved;

  if (result.requires_cluster_authority) {
    result.status = PageRegistryWarningStatus();
    result.may_write_body = false;
    result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                   "SB-PAGE-REGISTRY-CLUSTER-AUTHORITY-REQUIRED",
                                                   "page.registry.cluster_authority_required",
                                                   result.descriptor.stable_name);
    return result;
  }

  if (result.requires_decryption) {
    result.status = PageRegistryWarningStatus();
    result.may_write_body = false;
    result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                   "SB-PAGE-REGISTRY-DECRYPTION-REQUIRED",
                                                   "page.registry.decryption_required",
                                                   result.descriptor.stable_name);
    return result;
  }

  if (!lookup.ok()) {
    result.status = PageRegistryErrorStatus();
    result.may_read_body = false;
    result.may_write_body = false;
    result.diagnostic = lookup.diagnostic;
    return result;
  }

  if (result.descriptor.reserved) {
    result.status = PageRegistryWarningStatus();
    result.may_write_body = false;
    result.diagnostic = MakePageRegistryDiagnostic(result.status,
                                                   "SB-PAGE-REGISTRY-RESERVED-PAGE-TYPE",
                                                   "page.registry.reserved_page_type",
                                                   result.descriptor.stable_name);
  }

  return result;
}

DiagnosticRecord MakePageRegistryDiagnostic(Status status,
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
                        "storage.page.registry");
}

}  // namespace scratchbird::storage::page
