// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.hpp"
#include "scratchbird/engine/sblr/lowering.hpp"
#include "scratchbird/engine/sblr/raising.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace {

sb_engine_uuid_t example_uuid(unsigned char tail) {
  sb_engine_uuid_t uuid{};
  uuid.bytes[0] = 0x01;
  uuid.bytes[6] = 0x70;
  uuid.bytes[15] = tail;
  return uuid;
}

std::array<std::uint8_t, 132> example_sblr_anchor() {
  std::array<std::uint8_t, 132> anchor{};
  const auto copy_uuid = [&anchor](std::size_t offset, unsigned char tail) {
    const auto uuid = example_uuid(tail);
    std::copy(std::begin(uuid.bytes), std::end(uuid.bytes),
              anchor.begin() + static_cast<std::ptrdiff_t>(offset));
  };
  copy_uuid(0, 0x21);
  copy_uuid(16, 0x22);
  copy_uuid(32, 0x23);
  anchor[48] = 1;
  anchor[52] = 1;
  anchor[60] = 1;
  anchor[68] = 1;
  copy_uuid(76, 0x24);
  anchor[92] = 1;
  anchor[100] = static_cast<std::uint8_t>(
      scratchbird::engine::SblrPayloadKind::operation_envelope);
  copy_uuid(116, 0x25);
  return anchor;
}

}  // namespace

int main() {
  scratchbird::engine::Engine engine;
  sb_engine_open_params_v1_t open_params{};
  open_params.struct_size = sizeof(open_params);
  open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  if (scratchbird::engine::Engine::open(open_params, engine, nullptr) != SB_ENGINE_STATUS_OK) {
    return 1;
  }

  sb_engine_session_params_v1_t session_params{};
  session_params.struct_size = sizeof(session_params);
  session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  session_params.effective_user_uuid = example_uuid(1);
  session_params.session_uuid = example_uuid(2);
  session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  session_params.default_language_utf8 = "en";
  session_params.default_language_size = 2;
  sb_engine_session_t session_handle = nullptr;
  if (sb_engine_session_begin(engine.get(), &session_params, &session_handle, nullptr) != SB_ENGINE_STATUS_OK) {
    return 2;
  }
  scratchbird::engine::Session session(session_handle);

  const std::uint8_t plan[] = {'d', 'o', 'c'};
  auto envelope = scratchbird::engine::sblr::EnvelopeBuilder()
                      .canonical_anchor(example_sblr_anchor())
                      .operation(scratchbird::engine::SblrOperationFamily::relational_query, 1)
                      .payload_kind(
                          scratchbird::engine::SblrPayloadKind::operation_envelope)
                      .append_bytes(plan, sizeof(plan))
                      .encode();
  auto decoded = scratchbird::engine::sblr::EnvelopeReader::decode(envelope.data(), envelope.size());
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok ||
      decoded.envelope.canonical_anchor != example_sblr_anchor() ||
      decoded.envelope.canonical_bytes.size() != sizeof(plan) ||
      !std::equal(decoded.envelope.canonical_bytes.begin(),
                  decoded.envelope.canonical_bytes.end(), plan)) {
    return 3;
  }

  sb_engine_result_t caps_handle = nullptr;
  if (sb_engine_describe_capabilities(engine.get(), nullptr, &caps_handle) != SB_ENGINE_STATUS_OK) {
    return 4;
  }
  scratchbird::engine::Result caps(caps_handle);
  if (caps.result_class() != SB_ENGINE_RESULT_CAPABILITY_REPORT ||
      caps.payload().find("sblr.query.relational.v3=admission_only") == std::string_view::npos) {
    return 5;
  }
  return 0;
}
