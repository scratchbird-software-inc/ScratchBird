// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "parser_server_client.hpp"

namespace scratchbird::parser::sbsql {
using ipc::AuthCredentialEnvelope;
using ipc::PublicNameResolutionResult;
using ipc::PublicRelationResolutionRequest;
using ipc::SbpsClient;
using ipc::ServerCloseCursorResult;
using ipc::ServerExecutionResult;
using ipc::ServerFetchResult;
using ipc::ServerManagementResult;
using ipc::ServerPrepareSblrResult;
} // namespace scratchbird::parser::sbsql
