// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "binary_status_packet.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>
namespace scratchbird::wire::binary_status {
// Derived observation evidence only; never transaction or recovery authority.
inline bool AppendObservation(const std::filesystem::path& path,
                              const std::string& payload) {
  std::vector<public_result::Field> fields;
  if (path.empty() || payload.size() > 64 * 1024 * 1024 ||
      !scratchbird::wire::binary_status::Decode(payload, &fields)) return false;
  static std::mutex writer_mutex;
  std::lock_guard<std::mutex> lock(writer_mutex);
  std::error_code error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
  }
  const bool exists = std::filesystem::exists(path, error);
  if (error) return false;
  if (exists) {
    const auto size = std::filesystem::file_size(path, error);
    if (error) return false;
    if (size) {
      std::ifstream existing(path, std::ios::binary);
      char magic[8]{};
      if (!existing.read(magic, 8) || std::string_view(magic, 8) != "SBOBS002")
        return false;
    }
  }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) return false;
  out.write("SBOBS002", 8);
  const auto size = static_cast<std::uint64_t>(payload.size());
  for (unsigned i = 0; i < 8; ++i) out.put(static_cast<char>(size >> (8 * i)));
  out.write(payload.data(), payload.size());
  out.close();
  return static_cast<bool>(out);
}
} // namespace scratchbird::wire::binary_status
