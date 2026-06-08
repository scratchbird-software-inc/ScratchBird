// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipc/parser_ipc.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  using namespace scratchbird::parser::sbsql;
  ParserServerPacket packet;
  packet.opcode = ParserServerOpcode::kParserHello;
  packet.request_id = 42;
  packet.payload = EncodeParserHello({"parser", "sbsql", "build", "registry", 1, "relay", 1});
  const auto encoded = EncodePacket(packet);
  MessageVectorSet messages;
  auto decoded = DecodePacket(encoded, &messages);
  if (!decoded || decoded->request_id != 42 || decoded->opcode != ParserServerOpcode::kParserHello) {
    std::cerr << "parser IPC schema probe failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
