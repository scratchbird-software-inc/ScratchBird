// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/sblr_envelope.hpp"

namespace scratchbird::engine::sblr {

class EnvelopeReader {
 public:
  explicit EnvelopeReader(const SblrExecutionEnvelope& envelope) noexcept : envelope_(envelope) {}
  const SblrExecutionEnvelope& envelope() const noexcept { return envelope_; }
  SblrOperationFamily family() const noexcept { return envelope_.family; }
  std::uint16_t opcode() const noexcept { return envelope_.opcode; }
  SblrPayloadKind payload_kind() const noexcept { return envelope_.payload_kind; }
  std::uint64_t descriptor_count() const noexcept { return static_cast<std::uint64_t>(envelope_.descriptors.size()); }
  SblrBehaviorStatus behavior_status() const noexcept {
    const auto* row = FindSblrPriorityDRegistryRow(envelope_.family, envelope_.opcode);
    return row == nullptr ? SblrBehaviorStatus::unsupported : row->behavior_status;
  }

  static SblrDecodedEnvelope decode(const std::uint8_t* data, std::uint64_t size) {
    return DecodeSblrEnvelopeBytes(data, size);
  }

 private:
  const SblrExecutionEnvelope& envelope_;
};

}  // namespace scratchbird::engine::sblr
