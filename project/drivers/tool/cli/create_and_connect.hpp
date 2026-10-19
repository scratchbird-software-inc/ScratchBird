// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "first_principal_bootstrap.hpp"
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace scratchbird::cli {
struct CreateAndConnectRequest {
    FirstPrincipalBootstrapRequest creation;
    std::string server_program, server_config, ipc_path;
    std::uint32_t startup_timeout_ms = 30000;
    bool connect_password_stdin = false;
    std::vector<std::string> client_arguments;
};
struct CreateAndConnectParseResult {
    bool ok = false;
    bool help_requested = false;
    std::string diagnostic_code;
    CreateAndConnectRequest request;
};
struct CreateAndConnectResult {
    bool ok = false;
    std::string diagnostic_code;
    FirstPrincipalBootstrapResult creation;
    std::uint64_t server_pid = 0;
};
// The creator returns and releases every database handle before server launch.
// Client arguments are separately validated before any creation is performed.
CreateAndConnectParseResult ParseCreateAndConnectArgs(int, char* const[]);
CreateAndConnectResult RunCreateAndConnect(const CreateAndConnectRequest&,
                                         std::istream&, std::ostream&);
void PrintCreateAndConnectUsage(const char*, std::ostream&);
// Also used by an installer after a successful embedded constructor returns.
// The observation is not authority: the server reopens under its own locks,
// validates the database and supplies its actual identity only after readiness.
CreateAndConnectResult LaunchCreatedDatabaseServer(const CreateAndConnectRequest&,
                                                  FirstPrincipalBootstrapResult);
} // namespace scratchbird::cli
