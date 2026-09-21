// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "server_engine_bridge/statement_context.hpp"

namespace scratchbird::engine::statement_management {
// Private acknowledgement encoders used by the public engine's binding owner.
// These preserve the existing private record layouts, not canonical SBLR or
// MessageVector admission. UUIDs are raw16 system identities. Invalid inputs or
// hash failure return no bytes; allocation failure may throw. Neither failure
// changes the acknowledgement. Successful encoding updates its evidence digest.
// Name-resolve, parse-text, epoch-check and database-attach encoding also retain
// their exact bytes, as required by their existing owners.
std::vector<std::uint8_t> statement_management_prepare_ack(
    server_engine_bridge::StatementPrepareBindAckV1* acknowledgement);

std::vector<std::uint8_t> statement_management_execute_direct_ack(
    server_engine_bridge::StatementExecuteDirectBindAckV1* acknowledgement);

std::vector<std::uint8_t> statement_management_name_resolve_ack(
    server_engine_bridge::StatementNameResolveBindAckV1* acknowledgement);

std::vector<std::uint8_t> statement_management_parse_text_ack(
    server_engine_bridge::StatementParseTextBindAckV1* acknowledgement);

std::vector<std::uint8_t> statement_management_catalog_epoch_check_ack(
    server_engine_bridge::StatementCatalogEpochCheckBindAckV1* acknowledgement);

std::vector<std::uint8_t> statement_management_database_attach_ack(
    server_engine_bridge::StatementDatabaseAttachBindAckV1* acknowledgement);

std::vector<std::uint8_t> statement_management_free_ack(
    server_engine_bridge::StatementFreeBindAckV1* acknowledgement);

std::vector<std::uint8_t> statement_management_cancel_ack(
    server_engine_bridge::StatementCancelBindAckV1* acknowledgement);

}  // namespace scratchbird::engine::statement_management
