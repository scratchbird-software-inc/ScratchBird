// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CATALOG-RECORD-CODEC-ANCHOR
#include "catalog_page.hpp"
#include "catalog_records.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::catalog {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::storage::page::CatalogPageRow;

struct CatalogTypedRecord {
  CatalogRecordHeader header;
  std::string payload;
};

struct CatalogRecordCodecResult {
  Status status;
  CatalogTypedRecord record;
  CatalogPageRow row;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

CatalogRecordCodecResult EncodeCatalogTypedRecord(const CatalogTypedRecord& record, u32 ordinal);
CatalogRecordCodecResult DecodeCatalogTypedRecord(const CatalogPageRow& row);
DiagnosticRecord MakeCatalogRecordCodecDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::core::catalog
