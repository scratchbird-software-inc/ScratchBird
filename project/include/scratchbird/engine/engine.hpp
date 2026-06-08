// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/result.hpp"
#include "scratchbird/engine/session.hpp"
#include "scratchbird/engine/transaction.hpp"

#include <utility>

namespace scratchbird::engine {

class Engine {
 public:
  Engine() noexcept = default;
  explicit Engine(sb_engine_handle_t handle) noexcept : handle_(handle) {}
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Engine& operator=(Engine&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~Engine() { reset(); }

  sb_engine_handle_t get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }

  static Status open(const sb_engine_open_params_v1_t& params, Engine& out, Result* result = nullptr) noexcept {
    sb_engine_handle_t handle = nullptr;
    sb_engine_result_t raw_result = nullptr;
    auto status = sb_engine_open(&params, &handle, result == nullptr ? nullptr : &raw_result);
    if (status == SB_ENGINE_STATUS_OK) {
      out.reset(handle);
    }
    if (result != nullptr) {
      result->reset(raw_result);
    }
    return status;
  }

  void reset(sb_engine_handle_t next = nullptr) noexcept {
    if (handle_ != nullptr) {
      (void)sb_engine_close(handle_, nullptr);
    }
    handle_ = next;
  }

 private:
  sb_engine_handle_t handle_ = nullptr;
};

}  // namespace scratchbird::engine
