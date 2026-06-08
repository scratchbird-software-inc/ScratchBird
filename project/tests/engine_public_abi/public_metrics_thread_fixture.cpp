// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"

#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

namespace {

bool payload_contains(sb_engine_result_t result, std::string_view needle) {
  sb_engine_string_view_t view{};
  if (sb_engine_result_payload(result, &view) != SB_ENGINE_STATUS_OK || view.data == nullptr) {
    return false;
  }
  return std::string_view(view.data, static_cast<std::size_t>(view.size_bytes)).find(needle) != std::string_view::npos;
}

bool has_diagnostic(sb_engine_result_t result, std::string_view code) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) != SB_ENGINE_STATUS_OK ||
      diagnostics.diagnostics == nullptr) {
    return false;
  }
  for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
    const auto& diagnostic = diagnostics.diagnostics[i];
    if (diagnostic.symbolic_code.data != nullptr &&
        std::string_view(diagnostic.symbolic_code.data,
                         static_cast<std::size_t>(diagnostic.symbolic_code.size_bytes)) == code) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  sb_engine_open_params_v1_t open_params{};
  open_params.struct_size = sizeof(open_params);
  open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  sb_engine_handle_t engine = nullptr;
  if (sb_engine_open(&open_params, &engine, nullptr) != SB_ENGINE_STATUS_OK || engine == nullptr) {
    return 1;
  }

  sb_engine_result_t result = nullptr;
  if (sb_engine_describe_capabilities(engine, nullptr, &result) != SB_ENGINE_STATUS_OK || result == nullptr ||
      !payload_contains(result, "sblr.query.relational.v3=admission_only") ||
      !payload_contains(result, "sblr.cluster.placement.v3=noncluster_fail_closed") ||
      !payload_contains(result, "sblr.acceleration.management.v3=capability_fail_closed")) {
    return 2;
  }
  (void)sb_engine_result_release(result);

  sb_engine_metric_request_v1_t metrics{};
  metrics.struct_size = sizeof(metrics);
  metrics.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  metrics.root_path_utf8 = "sys.metrics.engine";
  metrics.root_path_size = 18;
  result = nullptr;
  if (sb_engine_metric_root(engine, &metrics, &result) != SB_ENGINE_STATUS_OK || result == nullptr ||
      !payload_contains(result, "sys.metrics.engine.abi") ||
      !payload_contains(result, "sys.metrics.sblr.envelope")) {
    return 3;
  }
  (void)sb_engine_result_release(result);

  sb_engine_metric_request_v1_t invalid_metrics{};
  invalid_metrics.struct_size = sizeof(invalid_metrics);
  invalid_metrics.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  invalid_metrics.root_path_utf8 = "sys.metrics.invalid";
  invalid_metrics.root_path_size = 19;
  result = nullptr;
  if (sb_engine_metric_root(engine, &invalid_metrics, &result) != SB_ENGINE_STATUS_INVALID_ARGUMENT ||
      result == nullptr || !has_diagnostic(result, "ENGINE.METRIC.ROOT_INVALID")) {
    return 6;
  }
  (void)sb_engine_result_release(result);

  std::atomic_bool thread_ok{true};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < 16; ++j) {
        sb_engine_result_t local = nullptr;
        if (sb_engine_describe_capabilities(engine, nullptr, &local) != SB_ENGINE_STATUS_OK || local == nullptr ||
            !payload_contains(local, "abi=implemented")) {
          thread_ok.store(false, std::memory_order_relaxed);
        }
        (void)sb_engine_result_release(local);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  if (!thread_ok.load(std::memory_order_relaxed)) {
    return 4;
  }

  if (sb_engine_close(engine, nullptr) != SB_ENGINE_STATUS_OK) {
    return 5;
  }
  return 0;
}
