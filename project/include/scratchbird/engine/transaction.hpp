// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/session.hpp"

#include <utility>

namespace scratchbird::engine {

class Transaction {
 public:
  Transaction() noexcept = default;
  explicit Transaction(sb_engine_transaction_t handle) noexcept : handle_(handle) {}
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Transaction& operator=(Transaction&& other) noexcept {
    if (this != &other) {
      rollback();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~Transaction() { rollback(); }

  sb_engine_transaction_t get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }

  Status commit() noexcept {
    if (handle_ == nullptr) {
      return SB_ENGINE_STATUS_INVALID_HANDLE;
    }
    sb_engine_transaction_finish_params_v1_t params{};
    params.struct_size = sizeof(params);
    params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    auto status = sb_engine_transaction_commit(handle_, &params, nullptr);
    if (status == SB_ENGINE_STATUS_OK) {
      handle_ = nullptr;
    }
    return status;
  }

  Status rollback() noexcept {
    if (handle_ == nullptr) {
      return SB_ENGINE_STATUS_OK;
    }
    sb_engine_transaction_finish_params_v1_t params{};
    params.struct_size = sizeof(params);
    params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    auto status = sb_engine_transaction_rollback(handle_, &params, nullptr);
    if (status == SB_ENGINE_STATUS_OK) {
      handle_ = nullptr;
    }
    return status;
  }

 private:
  sb_engine_transaction_t handle_ = nullptr;
};

}  // namespace scratchbird::engine
