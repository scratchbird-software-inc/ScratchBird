// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_transaction_fail_closed.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status ClusterUnavailableStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::cluster_private};
}

ClusterTransactionResult ClusterFail(std::string diagnostic_code,
                                     std::string message_key,
                                     ClusterTransactionMetadata metadata,
                                     std::string detail = {}) {
  ClusterTransactionResult result;
  result.status = ClusterUnavailableStatus();
  result.metadata = metadata;
  result.diagnostic = MakeClusterTransactionDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

}  // namespace

ClusterTransactionResult RejectClusterTransactionUntilMappingAvailable(
    ClusterTransactionMetadata metadata,
    std::string operation_name) {
  return ClusterFail("SB-CLUSTER-MAPPING-UNAVAILABLE",
                     "cluster_transaction.mapping_unavailable",
                     metadata,
                     std::move(operation_name));
}

ClusterTransactionResult BeginClusterTransactionFailClosed(ClusterTransactionMetadata metadata) {
  return RejectClusterTransactionUntilMappingAvailable(metadata, "begin_cluster_transaction");
}

ClusterTransactionResult CommitClusterTransactionFailClosed(ClusterTransactionMetadata metadata) {
  return RejectClusterTransactionUntilMappingAvailable(metadata, "commit_cluster_transaction");
}

DiagnosticRecord MakeClusterTransactionDiagnostic(Status status,
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
                        "transaction.mga.cluster_fail_closed");
}

}  // namespace scratchbird::transaction::mga
