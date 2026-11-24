// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_STATE_DUMP (S4h part 1). READ-ONLY offline view of a native single-file database, through the same independent reader
// the native tests use: every non-unused TIP slot (order, state) and every session-ledger record (ordinal, kind, record uuid). Used to show what a
// real multi-process exchange left in the file (sessions and transactions begun, ended or left open) without opening the file as a server.
//   usage: native_state_dump <path> <page_size>
#include "../support/native_offline_file_reader.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: native_state_dump <path> <page_size>\n";
    return 2;
  }
  namespace off = scratchbird::tests::offline;
  namespace pgs = scratchbird::storage::page;
  const auto file = off::ReadFile(argv[1], static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)));
  static const char* const state_names[] = {"unused", "starting", "active", "read_only_active", "prepared", "committing", "committed",
                                            "rolling_back", "rolled_back", "dead", "limbo", "recovering", "archived"};
  for (const auto& [order, word] : file.words) {
    const auto code = static_cast<unsigned>(word.state);
    std::cout << "TIP order=" << order << " state=" << (code < 13 ? state_names[code] : "?") << '\n';
  }
  // S5: the durable HEADER of every transaction (what a boundary must write): final state, the identities at +64 (principal), +80
  // (attachment) and +96 (session), the isolation profile, read-only, and whether the completion times are set.
  const auto hex = [](const scratchbird::core::platform::Uuid& id) {
    std::string out;
    char buffer[3];
    for (const auto byte : id.bytes) {
      std::snprintf(buffer, sizeof buffer, "%02x", static_cast<unsigned>(byte));
      out += buffer;
    }
    return out;
  };
  for (const auto& [order, header] : file.headers) {
    const auto final_code = static_cast<unsigned>(header.final_state);
    std::cout << "HEADER order=" << order << " final=" << (final_code < 13 ? state_names[final_code] : "?")
              << " txn=" << hex(header.transaction_uuid) << " principal=" << hex(header.principal_or_agent_uuid)
              << " attachment=" << hex(header.attachment_uuid) << " session=" << hex(header.session_uuid)
              << " isolation=" << static_cast<unsigned>(header.isolation_profile) << " read_only=" << (header.read_only ? 1 : 0)
              << " commit_time=" << (header.commit_time_utc_ns != 0 ? 1 : 0)
              << " completed_at=" << (header.completed_at_utc_ns != 0 ? 1 : 0) << '\n';
  }
  static const char* const kind_names[] = {"?", "connection", "session", "attachment", "settings_change"};
  for (const auto& record : file.ledger) {
    const auto kind = static_cast<unsigned>(record.kind());
    std::cout << "LEDGER ordinal=" << record.ordinal << " kind=" << (kind < 5 ? kind_names[kind] : "?") << '\n';
  }
  std::cout << "COUNTS tip_slots=" << file.words.size() << " ledger_records=" << file.ledger.size() << '\n';
  return 0;
}
