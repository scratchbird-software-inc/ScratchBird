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

#include <cstring>

namespace {

sb_engine_uuid_t test_uuid(unsigned char tail) {
  sb_engine_uuid_t uuid{};
  uuid.bytes[0] = 0x01;
  uuid.bytes[6] = 0x70;
  uuid.bytes[15] = tail;
  return uuid;
}

}  // namespace

int main() {
  scratchbird::engine::Engine engine;
  scratchbird::engine::Result open_result;
  sb_engine_open_params_v1_t open_params{};
  open_params.struct_size = sizeof(open_params);
  open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  if (scratchbird::engine::Engine::open(open_params, engine, &open_result) != SB_ENGINE_STATUS_OK) {
    return 1;
  }

  sb_engine_session_params_v1_t session_params{};
  session_params.struct_size = sizeof(session_params);
  session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  session_params.effective_user_uuid = test_uuid(1);
  session_params.session_uuid = test_uuid(2);
  session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  session_params.default_language_utf8 = "en";
  session_params.default_language_size = 2;

  sb_engine_session_t raw_session = nullptr;
  if (sb_engine_session_begin(engine.get(), &session_params, &raw_session, nullptr) != SB_ENGINE_STATUS_OK) {
    return 2;
  }
  scratchbird::engine::Session session(raw_session);

  std::uint8_t payload[4] = {0x53, 0x42, 0x4c, 0x52};
  auto envelope = scratchbird::engine::sblr::EnvelopeBuilder().append_bytes(payload, sizeof(payload)).finish();
  scratchbird::engine::sblr::EnvelopeReader reader(envelope);
  if (reader.envelope().canonical_bytes.size() != sizeof(payload)) {
    return 3;
  }

  scratchbird::engine::Result caps;
  sb_engine_result_t raw_caps = nullptr;
  if (sb_engine_describe_capabilities(engine.get(), nullptr, &raw_caps) != SB_ENGINE_STATUS_OK) {
    return 4;
  }
  caps.reset(raw_caps);
  if (caps.result_class() != SB_ENGINE_RESULT_CAPABILITY_REPORT) {
    return 5;
  }

  return 0;
}
