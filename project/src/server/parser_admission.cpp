// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PARSER_ADMISSION_INTERFACE

#include "parser_admission.hpp"

#include <array>
#include <utility>

namespace scratchbird::server {
namespace {

ServerDiagnostic AdmissionDiagnostic(std::string code,
                                     std::string message,
                                     std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

std::string HexUuid(const std::array<std::uint8_t, 16>& uuid) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (auto byte : uuid) {
    out.push_back(hex[(byte >> 4u) & 0x0fu]);
    out.push_back(hex[byte & 0x0fu]);
  }
  return out;
}

}  // namespace

ParserAdmissionResult AdmitParserPreauthForListener(const ParserPackageRegistry& registry,
                                                    const ParserAdmissionRequest& request) {
  ParserAdmissionResult result;
  result.database_selector = request.database_selector;
  if (!request.preauth_channel) {
    result.diagnostics.push_back(AdmissionDiagnostic(
        "SERVER.PARSER.PREAUTH_REQUIRED",
        "Parser admission requires a pre-auth parser/server channel."));
    return result;
  }
  if (request.database_selector.empty()) {
    result.diagnostics.push_back(AdmissionDiagnostic(
        "SERVER.PARSER.DATABASE_SELECTOR_REQUIRED",
        "Parser admission requires a server-owned database selector."));
    return result;
  }
  const auto package = AdmitParserPackage(registry,
                                          request.hello,
                                          sbps::kProtocolMajor,
                                          sbps::kProtocolMinor);
  result.diagnostics = package.diagnostics;
  if (!package.admitted) {
    return result;
  }
  result.admitted = true;
  result.outcome = "accepted";
  result.parser_channel_uuid = HexUuid(sbps::MakeUuidV7Bytes());
  result.session_uuid = HexUuid(sbps::MakeUuidV7Bytes());
  return result;
}

bool ParserPreauthOperationAllowed(const std::string& operation_name) {
  return operation_name == "HELLO" ||
         operation_name == "AUTH_START" ||
         operation_name == "AUTH_CONTINUE" ||
         operation_name == "AUTH_CANCEL" ||
         operation_name == "ATTACH_PREPARE" ||
         operation_name == "PING" ||
         operation_name == "DISCONNECT";
}

}  // namespace scratchbird::server
