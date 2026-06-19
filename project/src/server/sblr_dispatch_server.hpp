// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_SBLR_DISPATCH_RESULTS

#pragma once

#include "engine_host.hpp"
#include "session_registry.hpp"
#include "catalog/sys_information_projection.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scratchbird::server {

struct ServerIparProjectionSources {
  std::vector<scratchbird::engine::internal_api::SysInformationIparAgentLifecycleSource>
      agent_lifecycle;
  std::vector<scratchbird::engine::internal_api::SysInformationIparMetricCounterSource>
      metric_counters;
  std::vector<scratchbird::engine::internal_api::SysInformationIparTelemetryControlSource>
      telemetry_controls;
  std::vector<scratchbird::engine::internal_api::SysInformationIparSlowPathReasonSource>
      slow_path_reasons;
};

struct ServerIparProjectionSourceFactory {
  void* context = nullptr;
  ServerIparProjectionSources (*build)(void* context) = nullptr;
};

std::vector<std::uint8_t> EncodePrepareSblrPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::string& encoded_sblr_envelope);
std::vector<std::uint8_t> EncodeExecuteSblrPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    const std::string& encoded_sblr_envelope,
    bool cursor_requested = false);
std::vector<std::uint8_t> EncodeFetchPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& cursor_uuid,
    std::uint64_t max_rows = 1,
    std::uint64_t max_bytes = 0,
    std::uint32_t fetch_flags = 0);
std::vector<std::uint8_t> EncodeCloseCursorPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& cursor_uuid);
std::vector<std::uint8_t> EncodeCancelCursorPayloadForTest(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& cursor_uuid);

std::optional<std::array<std::uint8_t, 16>> DecodePreparedStatementUuidForTest(
    const std::vector<std::uint8_t>& prepare_result_payload);
std::optional<std::array<std::uint8_t, 16>> DecodeCursorUuidForTest(
    const std::vector<std::uint8_t>& execute_result_payload);
struct FetchResultForTest {
  std::array<std::uint8_t, 16> cursor_uuid{};
  std::uint64_t row_count = 0;
  std::string row_packet;
  bool end_of_cursor = false;
  std::string detail;
};
std::optional<FetchResultForTest> DecodeFetchResultForTest(
    const std::vector<std::uint8_t>& fetch_result_payload);

std::string EncodeShowVersionSblrForTest();
std::string EncodeRawSqlSblrBypassForTest();
std::string EncodeClusterSblrForTest();
std::string EncodeCrudInsertSblrForTest();
std::string EncodeCrudSelectSblrForTest();
std::string EncodeCrudUpdateSblrForTest();
std::string EncodeCrudDeleteSblrForTest();
std::string EncodeCatalogCreateTableSblrForTest();
std::string EncodeCatalogGetDescriptorSblrForTest();
std::string EncodeIndexCreateSblrForTest();
std::string EncodeDatatypeCastSblrForTest();
std::string EncodeDatatypeExtractSblrForTest();
std::string EncodeDatatypeSetSblrForTest();
std::string EncodeOptimizerExplainSblrForTest();
std::string EncodeOptimizerPlanSblrForTest();
std::string EncodeLlvmCompileSblrForTest();
std::string EncodeBeginTransactionSblrForTest();
std::string EncodeEventChannelCreateSblrForTest(const std::string& channel_uuid);
std::string EncodeEventChannelNotifySblrForTest(const std::string& channel_uuid,
                                                const std::string& payload);

SessionOperationResult HandlePrepareSblr(ServerSessionRegistry* registry,
                                         const HostedEngineState& engine_state,
                                         const sbps::Frame& request);
SessionOperationResult HandleExecuteSblr(ServerSessionRegistry* registry,
                                         const HostedEngineState& engine_state,
                                         const sbps::Frame& request,
                                         const ServerIparProjectionSourceFactory* ipar_source_factory = nullptr);
SessionOperationResult HandleFetch(ServerSessionRegistry* registry,
                                   const sbps::Frame& request);
SessionOperationResult HandleCloseCursor(ServerSessionRegistry* registry,
                                         const sbps::Frame& request);

}  // namespace scratchbird::server
