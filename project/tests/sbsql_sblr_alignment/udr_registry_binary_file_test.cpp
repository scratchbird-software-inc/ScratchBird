// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/parser_package_registry.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <unistd.h>
namespace server = scratchbird::server;
unsigned checks = 0;
void Check(bool value) {
  ++checks;
  if (!value) {
    std::cerr << "UDR binary registry check=" << checks << " failed\n";
    std::exit(EXIT_FAILURE);
  }
}
int main() {
  auto dir = std::filesystem::temp_directory_path() /
      ("sb_udr_binary_registry_" + std::to_string(::getpid()));
  Check(std::filesystem::create_directory(dir));
  auto uuid = scratchbird::tests::FixtureUuid(1346, 1);
  uuid.bytes[9] = 0; uuid.bytes[10] = '\n'; uuid.bytes[11] = ';'; uuid.bytes[12] = 255;
  const auto bytes = std::string(reinterpret_cast<const char*>(uuid.bytes.data()), 16);
  server::ServerBootstrapConfig config;
  config.parser_registry_path = dir / "registry.conf";
  const auto write = [&](const std::string& data) {
    std::ofstream identity(dir / "binding.bin", std::ios::binary);
    identity.write(data.data(), static_cast<std::streamsize>(data.size()));
  };
  {
    std::ofstream config_file(config.parser_registry_path);
    config_file << "format=SBPPR1\nparser_support_udr_identity_file=binding.bin\n";
  }
  write(bytes);
  const auto loaded = server::LoadParserPackageRegistry(config);
  Check(loaded.diagnostics.empty() && loaded.entries.size() == 1);
  Check(loaded.entries.front().parser_support_udr_uuid == uuid);
  for (std::size_t position = 0; position != 16; ++position) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      auto candidate = uuid;
      candidate.bytes[position] = static_cast<std::uint8_t>(octet);
      const bool valid = candidate.bytes[6] >> 4 == 7 && candidate.bytes[8] >> 6 == 2;
      write({reinterpret_cast<const char*>(candidate.bytes.data()), 16});
      const auto result = server::LoadParserPackageRegistry(config);
      if (valid) {
        Check(result.diagnostics.empty() && result.entries.size() == 1);
        Check(result.entries.front().parser_support_udr_uuid == candidate);
      } else {
        Check(result.entries.empty() && result.diagnostics.size() == 1);
        Check(result.snapshot_uuid.is_nil());
        Check(result.diagnostics.front().code == "SERVER.PARSER.SUPPORT_UDR_BINARY_IDENTITY_INVALID");
      }
    }
  }
  for (const auto& malformed : {bytes.substr(0, 15), bytes + "x", std::string(16, '\0')}) {
    write(malformed);
    const auto refused = server::LoadParserPackageRegistry(config);
    Check(refused.entries.empty() && refused.diagnostics.size() == 1);
    Check(refused.diagnostics.front().code == "SERVER.PARSER.SUPPORT_UDR_BINARY_IDENTITY_INVALID");
  }
  {
    std::ofstream config_file(config.parser_registry_path);
    config_file << "format=SBPPR1\nparser_support_udr_uuid=019d0000-0000-7000-8000-000000000001\n";
  }
  const auto legacy = server::LoadParserPackageRegistry(config);
  Check(legacy.entries.empty() && legacy.diagnostics.size() == 1);
  Check(legacy.diagnostics.front().code == "SERVER.PARSER.SUPPORT_UDR_BINARY_IDENTITY_REQUIRED");
  std::filesystem::remove_all(dir);
  std::cout << "UDR binary registry file PASS checks=" << checks << '\n';
}
