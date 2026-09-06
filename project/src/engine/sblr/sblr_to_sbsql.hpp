// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_engine_envelope.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrToSbsqlDiagnostic {
  std::string code;
  std::string message;
  bool error = true;
};

struct SblrToSbsqlOptions {
  bool source_preserving = false;
};

struct SblrToSbsqlResult {
  bool ok = false;
  std::string sbsql_text;
  std::vector<SblrToSbsqlDiagnostic> diagnostics;
};

SblrToSbsqlResult RenderSblrEnvelopeToSbsql(const SblrOperationEnvelope& envelope,
                                            const SblrToSbsqlOptions& options);

// Decodes and validates the canonical outer SBLR container, its immutable
// operation payload, and the typed tag-0x30 source artifact before rendering.
// The source artifact is never admitted as operation or object authority.
SblrToSbsqlResult RenderSblrContainerToSbsql(
    const std::uint8_t* data,
    std::size_t size,
    const SblrToSbsqlOptions& options);

inline SblrToSbsqlResult RenderSblrContainerToSbsql(
    std::string_view bytes,
    const SblrToSbsqlOptions& options) {
  return RenderSblrContainerToSbsql(
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
      options);
}

// Renders one canonical container through an external SAM1 reference carried
// by the exact SBEE. Callers must supply the artifact bytes already resolved
// by engine authority; the function revalidates every reference, checksum,
// binding, redaction, node, and object field before rendering.
SblrToSbsqlResult RenderSblrExternalSourceArtifactToSbsql(
    const std::uint8_t* container_data,
    std::size_t container_size,
    const std::uint8_t* execution_envelope_data,
    std::size_t execution_envelope_size,
    const std::uint8_t* artifact_data,
    std::size_t artifact_size,
    const SblrToSbsqlOptions& options);

inline SblrToSbsqlResult RenderSblrExternalSourceArtifactToSbsql(
    std::string_view container_bytes,
    std::string_view execution_envelope_bytes,
    std::string_view artifact_bytes,
    const SblrToSbsqlOptions& options) {
  return RenderSblrExternalSourceArtifactToSbsql(
      reinterpret_cast<const std::uint8_t*>(container_bytes.data()),
      container_bytes.size(),
      reinterpret_cast<const std::uint8_t*>(
          execution_envelope_bytes.data()),
      execution_envelope_bytes.size(),
      reinterpret_cast<const std::uint8_t*>(artifact_bytes.data()),
      artifact_bytes.size(), options);
}

}  // namespace scratchbird::engine::sblr
