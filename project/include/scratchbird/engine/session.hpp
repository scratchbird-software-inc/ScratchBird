// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/result.hpp"

#include <utility>

namespace scratchbird::engine {

class Session {
 public:
  Session() noexcept = default;
  explicit Session(sb_engine_session_t handle) noexcept : handle_(handle) {}
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Session& operator=(Session&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~Session() { reset(); }

  sb_engine_session_t get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }

  void reset(sb_engine_session_t next = nullptr) noexcept {
    if (handle_ != nullptr) {
      sb_engine_session_end_params_v1_t params{};
      params.struct_size = sizeof(params);
      params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      params.rollback_active_transactions = 1;
      params.cancel_open_results = 1;
      (void)sb_engine_session_end(handle_, &params, nullptr);
    }
    handle_ = next;
  }

 private:
  sb_engine_session_t handle_ = nullptr;
};

}  // namespace scratchbird::engine
