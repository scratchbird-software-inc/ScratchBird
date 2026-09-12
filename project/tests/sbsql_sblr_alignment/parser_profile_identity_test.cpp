// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "common/common.hpp"
#include "metrics/parser_metrics.hpp"

#include <iostream>
#include <type_traits>
#include <cstdlib>
#include <new>

namespace {
long fail_after = -1;
std::size_t allocation_count = 0;
bool injected = false;
}
void* operator new(std::size_t size) {
  ++allocation_count;
  if (fail_after >= 0 && fail_after-- == 0) {
    injected = true;
    throw std::bad_alloc{};
  }
  if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace ipc = scratchbird::parser::ipc;
namespace sql = scratchbird::parser::sbsql;
using Uuid = scratchbird::core::platform::Uuid;

int main() {
  static_assert(sizeof(decltype(ipc::ParserClientConfig::dialect_profile_uuid)) == 16);
  static_assert(std::is_same_v<decltype(ipc::ParserClientConfig::dialect_profile_uuid), Uuid>);
  std::size_t checks = 0, failures = 0;
  const auto check = [&](bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::cerr << label << '\n'; }
  };
  sql::ParserConfig config;
  check(config.dialect == "sbsql", "human dialect label changed");
  check(config.dialect_profile_uuid.is_nil(), "configuration fabricated an identity");
  check(!config.MatchesDialectProfile({}), "nil admitted identity matched");
  const Uuid admitted{{0x01,0xa0,0x91,0x12,0x7f,0xe3,0x7a,0x81,
                       0x9f,0x00,0x31,0xd8,0x45,0x6b,0x02,0xcc}};
  check(config.MatchesDialectProfile(admitted), "unset expectation rejected valid admission");
  config.dialect_profile_uuid = admitted;
  check(config.MatchesDialectProfile(admitted), "exact configured identity did not match");
  for (std::size_t position = 0; position < 16; ++position) {
    for (unsigned value = 0; value < 256; ++value) {
      auto candidate = admitted;
      candidate.bytes[position] = static_cast<unsigned char>(value);
      const bool exact = candidate.bytes == admitted.bytes;
      check(config.MatchesDialectProfile(candidate) == exact,
            "admission comparison did not bind all sixteen bytes");
      ipc::ParserClientConfig altered;
      altered.dialect_profile_uuid = candidate;
      check(altered.MatchesDialectProfile(admitted) == exact,
            "configured identity alias was accepted");
      check(config.dialect_profile_uuid == admitted, "comparison changed configuration");
    }
  }
  config.dialect_profile_uuid = {};
  for (unsigned version = 0; version < 16; ++version) {
    for (unsigned variant = 0; variant < 4; ++variant) {
      auto candidate = admitted;
      candidate.bytes[6] = static_cast<unsigned char>((version << 4) | 0xa);
      candidate.bytes[8] = static_cast<unsigned char>((variant << 6) | 0x1f);
      check(config.MatchesDialectProfile(candidate) == (version == 7 && variant == 2),
            "unset expectation admitted a non-system UUID");
    }
  }
  sql::SessionContext session;
  sql::SblrTemplateCache cache;
  sql::ParserMetrics metrics;
  session.session_uuid = admitted;
  session.connection_uuid = admitted;
  session.connection_uuid.bytes[15] = 0xce;
  const auto snapshot_before = session.session_uuid;
  const auto connection_before = session.connection_uuid;
  const auto preauth = metrics.SnapshotJson(config, session, cache);
  check(preauth.find("\"session_uuid\":\"\"") != std::string::npos,
        "preauth metrics disclosed the retained session identity");
  session.authenticated = true;
  const auto snapshot = metrics.SnapshotJson(config, session, cache);
  const auto heartbeat = metrics.HeartbeatJson(config, session, cache, "identity-check");
  const std::string expected_session = "\"session_uuid\":\"01a09112-7fe3-7a81-9f00-31d8456b02cc\"";
  const std::string expected_connection = "\"connection_uuid\":\"01a09112-7fe3-7a81-9f00-31d8456b02ce\"";
  check(snapshot.find(expected_session) != std::string::npos,
        "metrics did not render the exact binary session UUID");
  check(heartbeat.find(expected_session) != std::string::npos &&
        heartbeat.find(expected_connection) != std::string::npos,
        "heartbeat did not render exact distinct session and connection UUIDs");
  check(session.session_uuid == snapshot_before && session.connection_uuid == connection_before,
        "JSON rendering changed binary execution identities");
  session.connection_uuid = {};
  session.authenticated = false;
  const auto empty = metrics.HeartbeatJson(config, session, cache, "preauth");
  check(empty.find("\"connection_uuid\":\"\"") != std::string::npos &&
        empty.find("\"session_uuid\":\"\"") != std::string::npos,
        "absent/preauth identity was fabricated in heartbeat output");
  session.authenticated = true;
  session.connection_uuid = connection_before;
  std::size_t allocation_faults = 0;
  for (const bool heartbeat_output : {false, true}) {
    const auto render = [&] {
      return heartbeat_output
          ? metrics.HeartbeatJson(config, session, cache, "identity-check")
          : metrics.SnapshotJson(config, session, cache);
    };
    allocation_count = 0;
    (void)render();
    const auto sites = allocation_count;
    for (std::size_t site = 0; site < sites; ++site) {
      injected = false;
      fail_after = static_cast<long>(site);
      bool returned = false, rejected = false;
      try { (void)render(); returned = true; }
      catch (const std::bad_alloc&) { rejected = true; }
      catch (const std::ios_base::failure&) { rejected = true; }
      fail_after = -1;
      check(injected, "allocation sweep did not reach the selected site");
      check(rejected && !returned, "JSON rendering swallowed failure and returned partial output");
      check(session.session_uuid == snapshot_before && session.connection_uuid == connection_before,
            "failed JSON rendering changed binary execution identity");
      allocation_faults += injected;
    }
  }
  std::cout << "checks=" << checks << " failures=" << failures
            << " allocation_faults=" << allocation_faults << '\n';
  return failures == 0 ? 0 : 1;
}
