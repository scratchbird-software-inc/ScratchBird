// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "row_ordinal_locator.hpp"

namespace scratchbird::engine::executor {
namespace {

ExecutorRowOrdinalLookupResult Refuse(std::string diagnostic_code,
                                      std::string reason) {
  ExecutorRowOrdinalLookupResult result;
  result.evidence.diagnostic_code = std::move(diagnostic_code);
  result.evidence.evidence = {
      "executor_row_ordinal.accepted=false",
      "executor_row_ordinal.fail_closed_to_uuid_mga_lookup=true",
      "executor_row_ordinal.finality_authority=false",
      "durable_mga_inventory_remains_authority=true",
      "refusal_reason=" + std::move(reason),
  };
  return result;
}

}  // namespace

ExecutorRowOrdinalLookupResult ValidateExecutorRowOrdinalLocator(
    const ExecutorRowOrdinalLookupRequest& request) {
  if (!request.allow_internal_ordinal_acceleration) {
    return Refuse("SB_EXECUTOR_ROW_ORDINAL_ACCELERATION_DISABLED",
                  "ordinal_acceleration_disabled");
  }
  if (request.page_body == nullptr) {
    return Refuse("SB_EXECUTOR_ROW_ORDINAL_PAGE_BODY_REQUIRED",
                  "page_body_required");
  }

  const auto validation =
      page::ValidateDenseRowOrdinalLocator(*request.page_body, request.locator);
  ExecutorRowOrdinalLookupResult result;
  result.evidence.accepted = validation.accepted;
  result.evidence.fail_closed_to_uuid_mga_lookup =
      validation.fail_closed_to_uuid_mga_lookup;
  result.evidence.ordinal_visibility_or_finality_authority =
      validation.ordinal_is_visibility_or_finality_authority;
  result.evidence.durable_mga_inventory_remains_authority =
      validation.durable_mga_inventory_remains_authority;
  result.evidence.diagnostic_code =
      validation.accepted ? "SB_EXECUTOR_ROW_ORDINAL_LOCATOR_ACCEPTED"
                          : "SB_EXECUTOR_ROW_ORDINAL_LOCATOR_REFUSED";
  result.evidence.evidence = validation.evidence;
  if (!validation.refusal_reason.empty()) {
    result.evidence.evidence.push_back(
        "executor_refusal_reason=" + validation.refusal_reason);
  }
  result.row = validation.row;
  return result;
}

}  // namespace scratchbird::engine::executor
