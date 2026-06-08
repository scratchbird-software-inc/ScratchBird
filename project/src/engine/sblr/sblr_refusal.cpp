// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_refusal.hpp"

#include <utility>

namespace scratchbird::engine::sblr {

SblrStatusCode StatusForRefusalDiagnostic(std::string_view diagnostic_id) {
  if (diagnostic_id.find("SECURITY") != std::string_view::npos ||
      diagnostic_id.find("PARSER_AUTHORITY") != std::string_view::npos) {
    return SblrStatusCode::security_refused;
  }
  if (diagnostic_id.find("POLICY") != std::string_view::npos) return SblrStatusCode::policy_refused;
  if (diagnostic_id.find("DEPENDENCY") != std::string_view::npos) return SblrStatusCode::dependency_unavailable;
  if (diagnostic_id.find("MALFORMED") != std::string_view::npos ||
      diagnostic_id.find("UNKNOWN_OPCODE") != std::string_view::npos) {
    return SblrStatusCode::invalid_envelope;
  }
  return SblrStatusCode::unsupported_feature;
}

SblrResult RefuseSblrOperation(const SblrExecutionContext& context,
                               std::string operation_id,
                               std::string diagnostic_id,
                               std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic(std::move(diagnostic_id), context, std::move(detail));
  diagnostic.fields.push_back({"operation_id", operation_id});
  return MakeSblrFailure(StatusForRefusalDiagnostic(diagnostic.diagnostic_id),
                         std::move(operation_id),
                         std::move(diagnostic));
}

SblrResult RefuseClusterTransactionHook(const SblrExecutionContext& context,
                                        std::string hook_name,
                                        std::string feature_gate) {
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_CLUSTER_TXN_UNAVAILABLE", context, "cluster transaction hook unavailable in non-cluster build");
  diagnostic.fields.push_back({"hook_name", hook_name});
  diagnostic.fields.push_back({"feature_gate", feature_gate});
  diagnostic.fields.push_back({"build_cluster_enabled", "false"});
  return MakeSblrFailure(SblrStatusCode::unsupported_feature, std::move(hook_name), std::move(diagnostic));
}

SblrResult RefuseParserAuthority(const SblrExecutionContext& context,
                                 std::string operation_id,
                                 std::string detail) {
  return RefuseSblrOperation(context,
                             std::move(operation_id),
                             "SB_DIAG_PARSER_AUTHORITY_FORBIDDEN",
                             std::move(detail));
}

}  // namespace scratchbird::engine::sblr
