// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/types.hpp"

#include <string_view>
#include <utility>

namespace scratchbird::engine {

class Result {
 public:
  Result() noexcept = default;
  explicit Result(sb_engine_result_t handle) noexcept : handle_(handle) {}
  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;
  Result(Result&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Result& operator=(Result&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~Result() { reset(); }

  sb_engine_result_t get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }

  void reset(sb_engine_result_t next = nullptr) noexcept {
    if (handle_ != nullptr) {
      (void)sb_engine_result_release(handle_);
    }
    handle_ = next;
  }

  sb_engine_result_class_t result_class() const noexcept {
    sb_engine_result_class_t cls = SB_ENGINE_RESULT_NONE;
    (void)sb_engine_result_class(handle_, &cls);
    return cls;
  }

  std::string_view payload() const noexcept {
    sb_engine_string_view_t view{};
    (void)sb_engine_result_payload(handle_, &view);
    return view.data == nullptr ? std::string_view{} : std::string_view(view.data, static_cast<std::size_t>(view.size_bytes));
  }

 private:
  sb_engine_result_t handle_ = nullptr;
};

}  // namespace scratchbird::engine
