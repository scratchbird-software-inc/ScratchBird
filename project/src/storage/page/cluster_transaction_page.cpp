// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_transaction_page.hpp"

#include <utility>
#include <vector>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status ClusterPageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

ClusterTransactionPageBodyResult ClusterPageError(std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {}) {
  ClusterTransactionPageBodyResult result;
  result.status = ClusterPageErrorStatus();
  result.diagnostic = MakeClusterTransactionPageDiagnostic(result.status,
                                                           std::move(diagnostic_code),
                                                           std::move(message_key),
                                                           std::move(detail));
  return result;
}

}  // namespace

ClusterTransactionPageBodyResult BuildClusterTransactionPageBodyFailClosed(
    const ClusterTransactionPageBody& body,
    u32 page_size) {
  (void)body;
  (void)page_size;
  return ClusterPageError("SB-CLUSTER-MAPPING-UNAVAILABLE",
                          "cluster_transaction_page.mapping_unavailable");
}

ClusterTransactionPageBodyResult ParseClusterTransactionPageBodyFailClosed(
    const std::vector<byte>& serialized,
    u64 page_number) {
  (void)serialized;
  (void)page_number;
  return ClusterPageError("SB-CLUSTER-MAPPING-UNAVAILABLE",
                          "cluster_transaction_page.mapping_unavailable");
}

DiagnosticRecord MakeClusterTransactionPageDiagnostic(Status status,
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
                        "storage.page.cluster_transaction_mapping_unavailable");
}

}  // namespace scratchbird::storage::page
