// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DATATYPE-DOCUMENT-CANONICALIZATION-ANCHOR
#include "datatype_descriptor.hpp"

#include <string>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;

struct DocumentCanonicalizationRequest {
  CanonicalTypeId type_id = CanonicalTypeId::document;
  std::string encoded_value;
  std::string reference_profile;
  bool allow_hstore_domain = false;
};

struct DocumentCanonicalizationResult {
  Status status;
  CanonicalTypeId canonical_type_id = CanonicalTypeId::unknown;
  std::string canonical_value;
  std::string canonical_format;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

DocumentCanonicalizationResult CanonicalizeDocumentValue(const DocumentCanonicalizationRequest& request);
DiagnosticRecord MakeDocumentCanonicalizationDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail = {});

}  // namespace scratchbird::core::datatypes
