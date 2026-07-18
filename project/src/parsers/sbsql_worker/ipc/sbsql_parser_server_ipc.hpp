// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "ipc/sbsql_ipc_common.hpp"
#include "parser_server_ipc.hpp"

// SBsql source-compatibility aliases belong to the SBsql parser package, not
// to the family-neutral parser/server IPC schema.
namespace scratchbird::parser::sbsql {
using ipc::DecodePacket;
using ipc::EncodePacket;
using ipc::EncodeParserHello;
using ipc::NameResolutionRequest;
using ipc::NameResolutionResult;
using ipc::OpcodeName;
using ipc::ParserHello;
using ipc::ParserHelloResult;
using ipc::ParserServerOpcode;
using ipc::ParserServerPacket;
using ipc::RefusedHelloResult;
inline constexpr auto kParserServerIpcProtocolCurrent =
    ipc::kParserServerIpcProtocolCurrent;
inline constexpr auto kParserServerIpcProtocolMinSupported =
    ipc::kParserServerIpcProtocolMinSupported;
inline constexpr auto kParserServerIpcProtocolMaxSupported =
    ipc::kParserServerIpcProtocolMaxSupported;
inline constexpr auto kParserServerIpcMetricsSchemaCurrent =
    ipc::kParserServerIpcMetricsSchemaCurrent;
inline constexpr auto kParserServerIpcMetricsSchemaMinSupported =
    ipc::kParserServerIpcMetricsSchemaMinSupported;
inline constexpr auto kParserServerIpcMetricsSchemaMaxSupported =
    ipc::kParserServerIpcMetricsSchemaMaxSupported;
} // namespace scratchbird::parser::sbsql
