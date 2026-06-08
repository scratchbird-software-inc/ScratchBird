// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "row_data_page.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::executor {

namespace page = scratchbird::storage::page;

struct ExecutorRowOrdinalLookupRequest {
  const page::RowDataPageBody* page_body = nullptr;
  page::DenseRowOrdinalLocator locator;
  bool allow_internal_ordinal_acceleration = false;
};

struct ExecutorRowOrdinalLookupEvidence {
  bool accepted = false;
  bool fail_closed_to_uuid_mga_lookup = true;
  bool ordinal_visibility_or_finality_authority = false;
  bool durable_mga_inventory_remains_authority = true;
  std::string diagnostic_code = "SB_EXECUTOR_ROW_ORDINAL_LOOKUP_NOT_ATTEMPTED";
  std::vector<std::string> evidence;
};

struct ExecutorRowOrdinalLookupResult {
  ExecutorRowOrdinalLookupEvidence evidence;
  page::RowDataRecord row;
};

ExecutorRowOrdinalLookupResult ValidateExecutorRowOrdinalLocator(
    const ExecutorRowOrdinalLookupRequest& request);

}  // namespace scratchbird::engine::executor
