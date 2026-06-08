// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-QUARANTINE-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::u64;

struct IndexPageAuthorityInput {
  TypedUuid expected_index_uuid;
  TypedUuid observed_index_uuid;
  IndexFamily expected_family = IndexFamily::unknown;
  IndexFamily observed_family = IndexFamily::unknown;
  u64 expected_resource_epoch = 0;
  u64 observed_resource_epoch = 0;
  bool checksum_valid = false;
  bool page_type_supported = false;
};

struct IndexQuarantineDecision {
  Status status;
  bool allow_use = false;
  bool quarantine_required = false;
  bool rebuild_required = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && allow_use && !quarantine_required; }
};

IndexQuarantineDecision ClassifyIndexPageAuthority(const IndexPageAuthorityInput& input);
DiagnosticRecord MakeIndexQuarantineDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

}  // namespace scratchbird::core::index
