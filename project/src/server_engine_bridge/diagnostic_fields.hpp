// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/engine.h"
#include "core/diagnostics/canonical_diagnostic_snapshot.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server_engine_bridge {

// Private, family-neutral server-to-engine diagnostic transport. Structured
// This owned snapshot is private; it does not reinterpret public C ABI bytes.
struct EngineDiagnosticField {
  std::string key;
  std::string value;
};

struct EngineDiagnosticSnapshot {
  std::array<std::uint8_t, 16> occurrence_uuid{};
  std::uint32_t numeric_code = 0;
  sb_engine_diagnostic_severity_t severity = SB_ENGINE_DIAGNOSTIC_ERROR;
  std::string code;
  std::string message_key;
  // Private source detail, not permission to disclose it to a caller.
  std::string safe_detail;
  std::vector<EngineDiagnosticField> fields;
  // Exact trusted metadata, distinct from the legacy three-value C ABI view.
  // Neither this metadata nor the native cause authorizes parser disclosure.
  std::optional<scratchbird::core::diagnostics::CanonicalDiagnosticMetadata>
      canonical_metadata;
  std::optional<scratchbird::core::diagnostics::NativeDiagnosticSource>
      native_source;
};

// The result must remain alive during the copy. On any failure the output is
// unchanged; a successful snapshot remains valid after result release.
bool CopyEngineDiagnosticSnapshot(
    sb_engine_result_t result,
    std::size_t diagnostic_index,
    EngineDiagnosticSnapshot* snapshot);

bool CopyEngineDiagnosticFields(
    sb_engine_result_t result,
    std::size_t diagnostic_index,
    std::vector<EngineDiagnosticField>* fields);

}  // namespace scratchbird::server_engine_bridge
