// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime/parser_runtime.hpp"
#include "../../src/core/uuid/uuid.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace sql = scratchbird::parser::sbsql;
namespace uuid = scratchbird::core::uuid;
using Uuid = scratchbird::core::platform::Uuid;

int main() {
  const char* inherited = std::getenv("SB_PARSER_UUID");
  const std::optional<std::string> saved = inherited ? std::optional<std::string>(inherited) : std::nullopt;
  struct RestoreEnvironment {
    const std::optional<std::string>& saved;
    ~RestoreEnvironment() { if (saved) ::setenv("SB_PARSER_UUID", saved->c_str(), 1); else ::unsetenv("SB_PARSER_UUID"); }
  } restore{saved};
  ::unsetenv("SB_PARSER_UUID");
  std::array<char, 48> directory{};
  const std::string pattern = "/tmp/sb_parser_binary_identity.XXXXXX";
  std::copy(pattern.begin(), pattern.end(), directory.begin());
  if (!::mkdtemp(directory.data())) return 1;
  const auto root = std::filesystem::path(directory.data());
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
  const auto file = root / "identity.bin";
  unsigned failures = 0, checks = 0;
  const auto check = [&](bool ok, const char* label) { ++checks; if (!ok) { ++failures; std::cerr << label << '\n'; } };
  const auto config = [](std::string option = {}) {
    std::string program = "parser_config_binary_identity_test";
    std::array<char*, 2> arguments{program.data(), option.data()};
    return sql::ConfigFromArgs(option.empty() ? 1 : 2, arguments.data(), false);
  };
  const auto rejects = [&](std::string option) {
    try { (void)config(std::move(option)); return false; }
    catch (const std::invalid_argument&) { return true; }
  };
  const auto first = config(), second = config();
  check(uuid::IsEngineIdentityUuid(first.parser_uuid), "default startup identity is not engine UUIDv7");
  check(first.parser_uuid != second.parser_uuid, "distinct parser instances reused an identity");
  const Uuid expected{{0x01, 0x00, 0x0a, 0xff, 0x3b, 0x7c, 0x70, 0x00,
                       0x80, 0x0d, 0x00, 0xff, 0x22, 0x09, 0x3d, 0x01}};
  const auto write = [&](std::string_view bytes) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), bytes.size());
    if (!output) throw std::runtime_error("fixture identity write failed");
  };
  const std::string bytes(reinterpret_cast<const char*>(expected.bytes.data()), expected.bytes.size());
  const auto option = "--parser-identity-file=" + file.string();
  write(bytes);
  check(config(option).parser_uuid == expected, "startup changed binary identity bytes");
  for (std::size_t length = 0; length < 16; ++length) {
    write(std::string_view(bytes).substr(0, length));
    check(rejects(option), "startup accepted a truncated identity");
  }
  write(bytes + "x"); check(rejects(option), "startup accepted trailing identity bytes");
  write(std::string(16, '\0')); check(rejects(option), "startup accepted nil identity");
  auto invalid = bytes; invalid[6] = 0x40; write(invalid);
  check(rejects(option), "startup accepted non-v7 system identity");
  invalid = bytes; invalid[8] = 0; write(invalid);
  check(rejects(option), "startup accepted invalid UUID variant");
  write("01000aff-3b7c-7000-800d-00ff22093d01");
  check(rejects(option), "startup accepted UUID text in identity file");
  check(rejects("--parser-identity-file="), "startup silently ignored an empty identity file option");
  check(rejects("--parser-identity-file=" + (root / "missing").string()), "startup accepted a missing identity file");
  check(rejects("--parser-uuid=01000aff-3b7c-7000-800d-00ff22093d01"), "startup accepted legacy UUID text option");
  ::setenv("SB_PARSER_UUID", "01000aff-3b7c-7000-800d-00ff22093d01", 1);
  check(rejects({}), "startup accepted legacy UUID text environment");
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
