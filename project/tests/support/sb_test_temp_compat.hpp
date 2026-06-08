// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>

inline char* scratchbird_test_mkdtemp(char* tmpl) {
  if (tmpl == nullptr) return nullptr;

  std::string value(tmpl);
  const std::string marker = "XXXXXX";
  const auto marker_pos = value.rfind(marker);
  if (marker_pos == std::string::npos) return nullptr;

  const auto parent = std::filesystem::path(value).parent_path();
  if (!parent.empty()) {
    std::error_code parent_ec;
    std::filesystem::create_directories(parent, parent_ec);
    if (parent_ec) return nullptr;
  }

  static std::atomic<unsigned long long> sequence{0};
  constexpr char alphabet[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

  const auto base =
      static_cast<unsigned long long>(
          std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
      sequence.fetch_add(1, std::memory_order_relaxed);

  for (unsigned attempt = 0; attempt < 256; ++attempt) {
    auto candidate = value;
    auto n = base + attempt * 1103515245ull;
    for (std::size_t i = 0; i < marker.size(); ++i) {
      candidate[marker_pos + i] = alphabet[n % (sizeof(alphabet) - 1)];
      n /= (sizeof(alphabet) - 1);
    }

    std::error_code ec;
    if (std::filesystem::create_directory(candidate, ec)) {
      std::memcpy(tmpl, candidate.c_str(), candidate.size() + 1);
      return tmpl;
    }
  }

  return nullptr;
}

#define mkdtemp scratchbird_test_mkdtemp

#endif
