// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/sblr_ddl_create_schema_journal_io.hpp"
#include <iostream>
#include <string>
#if !defined(_WIN32)
#include <csignal>
#include <sys/resource.h>
#endif

namespace io = scratchbird::engine::internal_api::detail;

int main(int argc, char** argv) {
  if (argc != 4) return 64;
  const std::string mode = argv[1], path = argv[2];
  if (mode == "publish")
    return io::PublishCreateSchemaJournalReplacement(path, argv[3]) ? 0 : 3;
  if (mode == "hold-read") {
#if defined(_WIN32)
    HANDLE file = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;
    std::cout << "held" << std::endl;
    std::string line;
    std::getline(std::cin, line);
    return ::CloseHandle(file) ? 0 : 3;
#else
    return 64;  // This sharing-denial case is Windows-specific, never a pass.
#endif
  }
  if (mode != "create" && mode != "race" && mode != "create-hold" &&
      mode != "limited") return 64;
  if (mode == "race") {
    std::cout << "ready" << std::endl;
    std::string go;
    if (!std::getline(std::cin, go) || go != "go") return 64;
  }
  if (mode == "limited") {
#if !defined(_WIN32)
    std::signal(SIGXFSZ, SIG_IGN);
    const rlimit limit{0, 0};
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) return 64;
#else
    return 64;  // No simulated Windows I/O fault.
#endif
  }
  const int seed = std::stoi(argv[3]);
  if (seed < 0 || seed > 255) return 64;
  const std::vector<std::uint8_t> bytes(1032, static_cast<std::uint8_t>(seed));
  const auto status = io::CreateSchemaJournalFileExclusive(path, bytes);
  const int result = status == io::CreateSchemaJournalFileStatus::created ? 0
      : status == io::CreateSchemaJournalFileStatus::exists ? 2 : 3;
  if (mode == "create-hold" && result == 0) {
    std::cout << "created" << std::endl;
    std::string release;
    std::getline(std::cin, release);
  }
  return result;
}
