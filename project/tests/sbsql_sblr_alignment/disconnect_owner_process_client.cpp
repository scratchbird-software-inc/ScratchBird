// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Linux transport-owner regression; not complete public query E2E evidence.
#include "wire/sbsql_test_wire.hpp"
#include "metrics/parser_metrics.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <array>
#include <cerrno>
#include <iostream>
#include <string>

namespace sql = scratchbird::parser::sbsql;
int main(int argc, char** argv) {
  if (argc != 6) return 2;
  ::signal(SIGPIPE, SIG_IGN);
  const std::string mode = argv[4];
  const bool loss = std::string(argv[5]) == "loss";
  sql::ParserConfig config;
  config.server_endpoint = argv[1];
  config.database_token = argv[2];
  sql::ParserMetrics metrics;
  sql::SbsqlTestWireSession session(config, &metrics, nullptr);
  sql::AuthCredentialEnvelope credentials;
  credentials.provider_family = "local_password";
  credentials.principal = "alice";
  credentials.requested_database = argv[2];
  credentials.application_name = "disconnect-owner-regression";
  credentials.credential_evidence = argv[3];
  credentials.credential_evidence_present = true;
  sql::MessageVectorSet messages;
  if (!session.AuthenticateCredentials(credentials, &messages)) {
    for (const auto& d : messages.diagnostics) std::cerr << d.code << ':' << d.message << '\n';
    return 3;
  }
  const auto begun = session.RunPipeline("BEGIN;", false);
  if (!begun.accepted || begun.messages.has_errors()) {
    for (const auto& d : begun.messages.diagnostics) std::cerr << d.code << ':' << d.message << '\n';
    return 4;
  }
  // The parent stops the real server in the failure cases only after this
  // authenticated SQL transaction has actually executed, never by mocking IPC.
  std::cout << "AUTHENTICATED_TRANSACTION_READY\n" << std::flush;
  std::string go;
  if (!std::getline(std::cin, go) || go != "go") return 5;
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return 6;
  if (mode == "text") {
    if (::write(fds[0], "QUIT\n", 5) != 5) return 7;
  } else if (mode == "native") {
    // SBWP1.1 Terminate: 40-byte header, zero payload, stream1, no flags.
    std::array<unsigned char, 40> terminate{'S','B','W','P',1,1,0x0c,0};
    terminate[12] = 1;
    if (::write(fds[0], terminate.data(), terminate.size()) != 40) return 7;
  } else if (mode != "eof") return 8;
  ::shutdown(fds[0], SHUT_WR);
  const int rc = session.ServeFd(fds[1]);
  ::close(fds[1]);
  std::string response;
  std::array<char, 4096> buffer;
  for (;;) {
    const auto n = ::read(fds[0], buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    response.append(buffer.data(), static_cast<std::size_t>(n));
  }
  ::close(fds[0]);
  const bool bye = response.find("OK BYE") != std::string::npos;
  const bool expected_state = metrics.State() ==
      (loss ? sql::ParserState::kFailed : sql::ParserState::kDisconnected);
  const bool expected_response = mode != "text" ||
      (loss ? !bye && response.find("PARSER_SERVER_IPC.") != std::string::npos : bye);
  if ((loss ? rc == 0 : rc != 0) || !expected_state || !expected_response) {
    std::cerr << "disconnect owner mismatch mode=" << mode << " loss=" << loss
              << " rc=" << rc << " goodbye=" << bye
              << " expected_state=" << expected_state
              << " response=" << response << '\n';
    return 9;
  }
  std::cout << "OWNER_TERMINAL_VERIFIED mode=" << mode << " loss=" << loss
            << " rc=" << rc << '\n';
}
