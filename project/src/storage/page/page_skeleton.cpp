// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_skeleton.hpp"

#include "page_layout.hpp"

#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::storage::disk::PageTypeName;

Status PageSkeletonOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status PageSkeletonWarningStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::storage_page};
}

Status PageSkeletonErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

PageSkeletonDescriptor Descriptor(PageType page_type,
                                  PageSkeletonKind skeleton_kind,
                                  PageFamily family,
                                  PageSkeletonState state,
                                  std::string stable_name,
                                  u64 minimum_payload_bytes,
                                  bool engine_identity_uuid_v7_required = true,
                                  bool body_parser_available = false,
                                  bool body_mutation_available = false) {
  PageSkeletonDescriptor descriptor;
  descriptor.page_type = page_type;
  descriptor.skeleton_kind = skeleton_kind;
  descriptor.family = family;
  descriptor.state = state;
  descriptor.stable_name = std::move(stable_name);
  descriptor.minimum_payload_bytes = minimum_payload_bytes;
  descriptor.engine_identity_uuid_v7_required = engine_identity_uuid_v7_required;
  descriptor.body_parser_available = body_parser_available;
  descriptor.body_mutation_available = body_mutation_available;
  return descriptor;
}

}  // namespace

const char* PageSkeletonKindName(PageSkeletonKind kind) {
  switch (kind) {
    case PageSkeletonKind::database_header: return "database_header";
    case PageSkeletonKind::allocation_map: return "allocation_map";
    case PageSkeletonKind::catalog: return "catalog";
    case PageSkeletonKind::transaction_inventory: return "transaction_inventory";
    case PageSkeletonKind::row_data: return "row_data";
    case PageSkeletonKind::index_btree: return "index_btree";
    case PageSkeletonKind::index_hash: return "index_hash";
    case PageSkeletonKind::index_specialized: return "index_specialized";
    case PageSkeletonKind::blob: return "blob";
    case PageSkeletonKind::structured: return "structured";
    case PageSkeletonKind::unsupported: return "unsupported";
  }
  return "unsupported";
}

const char* PageSkeletonStateName(PageSkeletonState state) {
  switch (state) {
    case PageSkeletonState::skeleton_only: return "skeleton_only";
    case PageSkeletonState::body_layout_reserved: return "body_layout_reserved";
    case PageSkeletonState::body_implemented: return "body_implemented";
    case PageSkeletonState::body_parser_unavailable: return "body_parser_unavailable";
    case PageSkeletonState::unsupported: return "unsupported";
  }
  return "unsupported";
}

const char* PageBodyProductionAdmissionKindName(PageBodyProductionAdmissionKind kind) {
  switch (kind) {
    case PageBodyProductionAdmissionKind::local_engine_mutating: return "local_engine_mutating";
    case PageBodyProductionAdmissionKind::local_engine_read_only: return "local_engine_read_only";
    case PageBodyProductionAdmissionKind::skeleton_only_refused: return "skeleton_only_refused";
    case PageBodyProductionAdmissionKind::layout_only_refused: return "layout_only_refused";
    case PageBodyProductionAdmissionKind::reserved_nonmutating: return "reserved_nonmutating";
    case PageBodyProductionAdmissionKind::external_cluster_provider_required: return "external_cluster_provider_required";
    case PageBodyProductionAdmissionKind::decryption_required: return "decryption_required";
    case PageBodyProductionAdmissionKind::unregistered_refused: return "unregistered_refused";
    case PageBodyProductionAdmissionKind::body_refused: return "body_refused";
  }
  return "body_refused";
}

const std::vector<PageSkeletonDescriptor>& BuiltinPageSkeletonRegistry() {
  static const std::vector<PageSkeletonDescriptor> registry = {
      Descriptor(PageType::database_header,
                 PageSkeletonKind::database_header,
                 PageFamily::startup,
                 PageSkeletonState::body_implemented,
                 "database_header_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::system_state,
                 PageSkeletonKind::structured,
                 PageFamily::startup,
                 PageSkeletonState::body_implemented,
                 "system_state_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::bootstrap_reserved,
                 PageSkeletonKind::structured,
                 PageFamily::startup,
                 PageSkeletonState::body_implemented,
                 "bootstrap_reserved_page",
                 128,
                 true,
                 true,
                 false),
      Descriptor(PageType::filespace_directory,
                 PageSkeletonKind::structured,
                 PageFamily::startup,
                 PageSkeletonState::body_implemented,
                 "filespace_directory_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::config_root,
                 PageSkeletonKind::structured,
                 PageFamily::catalog,
                 PageSkeletonState::body_implemented,
                 "config_root_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::security_root,
                 PageSkeletonKind::structured,
                 PageFamily::catalog,
                 PageSkeletonState::body_implemented,
                 "security_root_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::allocation_map,
                 PageSkeletonKind::allocation_map,
                 PageFamily::allocation,
                 PageSkeletonState::body_implemented,
                 "allocation_map_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::catalog,
                 PageSkeletonKind::catalog,
                 PageFamily::catalog,
                 PageSkeletonState::body_implemented,
                 "catalog_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::transaction_inventory,
                 PageSkeletonKind::transaction_inventory,
                 PageFamily::transaction,
                 PageSkeletonState::body_implemented,
                 "transaction_inventory_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::row_data,
                 PageSkeletonKind::row_data,
                 PageFamily::data,
                 PageSkeletonState::body_implemented,
                 "row_data_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_btree,
                 PageSkeletonKind::index_btree,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_btree_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_btree_root,
                 PageSkeletonKind::index_btree,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_btree_root_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_btree_branch,
                 PageSkeletonKind::index_btree,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_btree_branch_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_btree_leaf,
                 PageSkeletonKind::index_btree,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_btree_leaf_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_btree_posting,
                 PageSkeletonKind::structured,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_btree_posting_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_hash,
                 PageSkeletonKind::index_hash,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_hash_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_bitmap,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_bitmap_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_summary,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_summary_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_inverted,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_inverted_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_spatial,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_spatial_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_vector,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_vector_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_graph,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_graph_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_temporary,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_temporary_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_statistics,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_statistics_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::index_special_root,
                 PageSkeletonKind::index_specialized,
                 PageFamily::index,
                 PageSkeletonState::body_implemented,
                 "index_special_root_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::blob,
                 PageSkeletonKind::blob,
                 PageFamily::blob,
                 PageSkeletonState::body_implemented,
                 "blob_overflow_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::metrics,
                 PageSkeletonKind::structured,
                 PageFamily::metrics,
                 PageSkeletonState::body_implemented,
                 "metrics_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::archive,
                 PageSkeletonKind::structured,
                 PageFamily::archive,
                 PageSkeletonState::body_implemented,
                 "archive_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::columnar,
                 PageSkeletonKind::structured,
                 PageFamily::columnar,
                 PageSkeletonState::body_implemented,
                 "columnar_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::vector,
                 PageSkeletonKind::structured,
                 PageFamily::vector,
                 PageSkeletonState::body_implemented,
                 "vector_page",
                 128,
                 true,
                 true,
                 true),
      Descriptor(PageType::graph,
                 PageSkeletonKind::structured,
                 PageFamily::graph,
                 PageSkeletonState::body_implemented,
                 "graph_page",
                 128,
                 true,
                 true,
                 true),
  };
  return registry;
}

PageSkeletonLookupResult LookupPageSkeleton(PageType page_type) {
  for (const PageSkeletonDescriptor& descriptor : BuiltinPageSkeletonRegistry()) {
    if (descriptor.page_type == page_type) {
      PageSkeletonLookupResult result;
      result.status = PageSkeletonOkStatus();
      result.descriptor = descriptor;
      return result;
    }
  }

  PageSkeletonLookupResult result;
  result.status = PageSkeletonErrorStatus();
  result.descriptor = Descriptor(page_type,
                                 PageSkeletonKind::unsupported,
                                 PageFamily::unknown,
                                 PageSkeletonState::unsupported,
                                 PageTypeName(page_type),
                                 0,
                                 false);
  result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                 "SB-PAGE-SKELETON-UNSUPPORTED-PAGE-TYPE",
                                                 "page.skeleton.unsupported_page_type",
                                                 PageTypeName(page_type));
  return result;
}

PageSkeletonClassification ClassifyPageSkeleton(const PageClassification& header_classification) {
  PageSkeletonClassification result;
  result.status = PageSkeletonOkStatus();
  result.manager_classification = ClassifyForPageManager(header_classification);

  if (!result.manager_classification.ok()) {
    result.status = result.manager_classification.status;
    result.diagnostic = result.manager_classification.diagnostic;
    return result;
  }

  PageSkeletonLookupResult lookup = LookupPageSkeleton(header_classification.page_type);
  result.descriptor = lookup.descriptor;

  if (!lookup.ok()) {
    result.status = lookup.status;
    result.diagnostic = lookup.diagnostic;
    result.may_interpret_body = false;
    result.may_mutate_body = false;
    return result;
  }

  result.may_interpret_body = result.manager_classification.may_read_body &&
                              lookup.descriptor.body_parser_available;
  result.may_mutate_body = result.manager_classification.may_write_body &&
                           lookup.descriptor.body_mutation_available;

  if (!lookup.descriptor.body_parser_available) {
    result.status = PageSkeletonWarningStatus();
    result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                   "SB-PAGE-SKELETON-BODY-PARSER-UNAVAILABLE",
                                                   "page.skeleton.body_parser_unavailable",
                                                   lookup.descriptor.stable_name);
    return result;
  }

  return result;
}

PageBodyProductionAdmissionResult ClassifyPageBodyProductionAdmission(
    const PageClassification& header_classification) {
  PageBodyProductionAdmissionResult result;
  result.classification = ClassifyPageSkeleton(header_classification);
  result.may_interpret_body = result.classification.may_interpret_body;
  result.may_mutate_body = result.classification.may_mutate_body;

  const auto family = LookupPageFamily(header_classification.page_type);
  if (!family.ok()) {
    result.status = PageSkeletonErrorStatus();
    result.kind = PageBodyProductionAdmissionKind::unregistered_refused;
    result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                   "SB-PAGE-BODY-PRODUCTION-UNREGISTERED-REFUSED",
                                                   "page.skeleton.production_unregistered_refused",
                                                   PageTypeName(header_classification.page_type));
    return result;
  }
  if (header_classification.cluster_authority_required || family.descriptor.cluster_only) {
    result.status = PageSkeletonWarningStatus();
    result.kind = PageBodyProductionAdmissionKind::external_cluster_provider_required;
    result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                   "SB-PAGE-BODY-PRODUCTION-CLUSTER-PROVIDER-REQUIRED",
                                                   "page.skeleton.production_cluster_provider_required",
                                                   family.descriptor.stable_name);
    return result;
  }
  if (header_classification.decryption_required || family.descriptor.encrypted_or_opaque) {
    result.status = PageSkeletonWarningStatus();
    result.kind = PageBodyProductionAdmissionKind::decryption_required;
    result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                   "SB-PAGE-BODY-PRODUCTION-DECRYPTION-REQUIRED",
                                                   "page.skeleton.production_decryption_required",
                                                   family.descriptor.stable_name);
    return result;
  }
  if (family.descriptor.reserved) {
    result.status = PageSkeletonWarningStatus();
    result.kind = PageBodyProductionAdmissionKind::reserved_nonmutating;
    result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                   "SB-PAGE-BODY-PRODUCTION-RESERVED-NONMUTATING",
                                                   "page.skeleton.production_reserved_nonmutating",
                                                   family.descriptor.stable_name);
    return result;
  }

  const auto layout = LookupPageLayout(header_classification.page_type);
  if (!layout.ok()) {
    result.status = PageSkeletonErrorStatus();
    result.kind = PageBodyProductionAdmissionKind::layout_only_refused;
    result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                   "SB-PAGE-BODY-PRODUCTION-LAYOUT-REFUSED",
                                                   "page.skeleton.production_layout_refused",
                                                   family.descriptor.stable_name);
    return result;
  }

  const auto skeleton = LookupPageSkeleton(header_classification.page_type);
  if (!skeleton.ok()) {
    result.status = PageSkeletonErrorStatus();
    result.kind = PageBodyProductionAdmissionKind::layout_only_refused;
    result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                   "SB-PAGE-BODY-PRODUCTION-SKELETON-REFUSED",
                                                   "page.skeleton.production_skeleton_refused",
                                                   family.descriptor.stable_name);
    return result;
  }

  if (!skeleton.descriptor.body_parser_available) {
    result.status = PageSkeletonWarningStatus();
    result.kind = PageBodyProductionAdmissionKind::skeleton_only_refused;
    result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                   "SB-PAGE-BODY-PRODUCTION-PARSER-REFUSED",
                                                   "page.skeleton.production_parser_refused",
                                                   skeleton.descriptor.stable_name);
    return result;
  }

  if (result.may_mutate_body) {
    result.status = PageSkeletonOkStatus();
    result.kind = PageBodyProductionAdmissionKind::local_engine_mutating;
    result.admitted = true;
    return result;
  }
  if (result.may_interpret_body) {
    result.status = PageSkeletonOkStatus();
    result.kind = PageBodyProductionAdmissionKind::local_engine_read_only;
    result.admitted = true;
    return result;
  }

  result.status = PageSkeletonWarningStatus();
  result.kind = PageBodyProductionAdmissionKind::body_refused;
  result.diagnostic = MakePageSkeletonDiagnostic(result.status,
                                                 "SB-PAGE-BODY-PRODUCTION-BODY-REFUSED",
                                                 "page.skeleton.production_body_refused",
                                                 skeleton.descriptor.stable_name);
  return result;
}

DiagnosticRecord MakePageSkeletonDiagnostic(Status status,
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
                        "storage.page.skeleton");
}

}  // namespace scratchbird::storage::page
