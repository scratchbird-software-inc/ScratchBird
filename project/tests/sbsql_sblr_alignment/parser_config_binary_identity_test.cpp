// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime/parser_runtime.hpp"
#include "../../src/core/uuid/uuid.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace sql = scratchbird::parser::sbsql;
namespace uuid = scratchbird::core::uuid;
using Uuid = scratchbird::core::platform::Uuid;

struct IdentityChannel {
  int descriptors[2]{-1, -1};
  IdentityChannel() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0)
      throw std::runtime_error("fixture socketpair failed");
  }
  IdentityChannel(const IdentityChannel&) = delete;
  ~IdentityChannel() { for (int fd : descriptors) if (fd >= 0) ::close(fd); }
  std::string option() const { return "--parser-identity-socket=" + std::to_string(descriptors[0]); }
  void send(std::string_view bytes, bool finish = true) {
    if (!bytes.empty() && ::write(descriptors[1], bytes.data(), bytes.size()) !=
                              static_cast<ssize_t>(bytes.size()))
      throw std::runtime_error("fixture identity send failed");
    if (finish && ::shutdown(descriptors[1], SHUT_WR) != 0)
      throw std::runtime_error("fixture identity EOF failed");
  }
};

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
  const auto config = [](std::string option = {}, std::string second_option = {}) {
    std::string program = "parser_config_binary_identity_test";
    std::array<char*, 3> arguments{program.data(), option.data(), second_option.data()};
    return sql::ConfigFromArgs(option.empty() ? 1 : second_option.empty() ? 2 : 3, arguments.data(), false);
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
  const std::string bytes(reinterpret_cast<const char*>(expected.bytes.data()), expected.bytes.size());
  IdentityChannel valid;
  valid.send(bytes);
  const auto original_flags = ::fcntl(valid.descriptors[0], F_GETFL);
  check(config(valid.option()).parser_uuid == expected, "startup changed binary identity bytes");
  check(::fcntl(valid.descriptors[0], F_GETFL) == original_flags,
        "startup closed or changed flags on the borrowed identity channel");
  const auto rejects_bytes = [&](std::string_view value) {
    IdentityChannel channel;
    channel.send(value);
    return rejects(channel.option());
  };
  for (std::size_t length = 0; length < 16; ++length) {
    check(rejects_bytes(std::string_view(bytes).substr(0, length)),
          "startup accepted a truncated identity");
  }
  check(rejects_bytes(bytes + "x"), "startup accepted trailing identity bytes");
  check(rejects_bytes(std::string(16, '\0')), "startup accepted nil identity");
  auto invalid = bytes; invalid[6] = 0x40;
  check(rejects_bytes(invalid), "startup accepted non-v7 system identity");
  invalid = bytes; invalid[8] = 0;
  check(rejects_bytes(invalid), "startup accepted invalid UUID variant");
  check(rejects_bytes("01000aff-3b7c-7000-800d-00ff22093d01"), "startup accepted UUID text");
  for (int split = 1; split < 16; ++split) {
    IdentityChannel fragmented;
    fragmented.send(std::string_view(bytes).substr(0, split), false);
    std::jthread sender([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      fragmented.send(std::string_view(bytes).substr(split));
    });
    check(config(fragmented.option()).parser_uuid == expected,
          "fragmented startup identity changed bytes or required one read");
  }
  IdentityChannel duplicate;
  duplicate.send(bytes);
  bool duplicate_refused = false;
  try { (void)config(duplicate.option(), duplicate.option()); }
  catch (const std::invalid_argument&) { duplicate_refused = true; }
  check(duplicate_refused, "startup accepted duplicate identity bindings");
  for (const auto value : {std::string_view{}, std::string_view(bytes)}) {
    IdentityChannel stalled;
    stalled.send(value, false);
    const auto start = std::chrono::steady_clock::now();
    check(rejects(stalled.option()), "startup accepted missing channel EOF");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(elapsed >= std::chrono::milliseconds(900) && elapsed < std::chrono::seconds(5),
          "startup identity channel deadline is missing or not event-driven");
  }
  for (const auto suffix : {"", "-1", "abc", "1junk", " 1", "99999999999999999999999999"})
    check(rejects(std::string("--parser-identity-socket=") + suffix), "invalid descriptor accepted");
  {
    std::ofstream output(file, std::ios::binary);
    output.write(bytes.data(), bytes.size());
    if (!output) throw std::runtime_error("fixture identity write failed");
  }
  const int ordinary_file = ::open(file.c_str(), O_RDONLY);
  if (ordinary_file < 0) throw std::runtime_error("fixture file open failed");
  check(rejects("--parser-identity-socket=" + std::to_string(ordinary_file)),
        "startup read an ordinary file through a descriptor");
  check(::lseek(ordinary_file, 0, SEEK_CUR) == 0, "startup consumed bytes from an ordinary file");
  ::close(ordinary_file);
  check(rejects("--parser-identity-file=" + file.string()), "startup still accepts direct identity file access");
  check(rejects("--parser-identity-file="), "startup silently ignored an empty identity file option");
  check(rejects("--parser-identity-file=" + (root / "missing").string()), "startup accepted a missing identity file");
  check(rejects("--parser-uuid=01000aff-3b7c-7000-800d-00ff22093d01"), "startup accepted legacy UUID text option");
  ::setenv("SB_PARSER_UUID", "01000aff-3b7c-7000-800d-00ff22093d01", 1);
  check(rejects({}), "startup accepted legacy UUID text environment");
  std::cout << "checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
