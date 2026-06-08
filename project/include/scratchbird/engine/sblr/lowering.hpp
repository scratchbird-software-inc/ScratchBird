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

class EnvelopeBuilder {
 public:
  EnvelopeBuilder& operation(SblrOperationFamily family, std::uint16_t opcode) noexcept {
    envelope_.family = family;
    envelope_.opcode = opcode;
    return *this;
  }

  EnvelopeBuilder& payload_kind(SblrPayloadKind payload_kind) noexcept {
    envelope_.payload_kind = payload_kind;
    return *this;
  }

  EnvelopeBuilder& version(std::uint32_t major, std::uint32_t minor) noexcept {
    envelope_.version_major = major;
    envelope_.version_minor = minor;
    return *this;
  }

  EnvelopeBuilder& flags(std::uint32_t flags) noexcept {
    envelope_.flags = flags;
    return *this;
  }

  EnvelopeBuilder& descriptor(std::uint16_t kind, const std::uint8_t* data, std::uint64_t size, std::uint16_t flags = 0) {
    SblrDescriptor descriptor;
    descriptor.kind = kind;
    descriptor.flags = flags;
    if (data != nullptr && size != 0) {
      descriptor.payload.insert(descriptor.payload.end(), data, data + size);
    }
    envelope_.descriptors.push_back(std::move(descriptor));
    return *this;
  }

  EnvelopeBuilder& source_artifact(std::uint16_t kind, std::string value) {
    envelope_.source_artifacts.push_back(SblrSourceArtifact{kind, std::move(value)});
    return *this;
  }

  EnvelopeBuilder& append_bytes(const std::uint8_t* data, std::uint64_t size) {
    if (data != nullptr && size != 0) {
      envelope_.canonical_bytes.insert(envelope_.canonical_bytes.end(), data, data + size);
    }
    return *this;
  }

  SblrExecutionEnvelope finish() const { return envelope_; }

  std::vector<std::uint8_t> encode() const { return EncodeSblrEnvelope(envelope_); }

 private:
  SblrExecutionEnvelope envelope_;
};

}  // namespace scratchbird::engine::sblr
