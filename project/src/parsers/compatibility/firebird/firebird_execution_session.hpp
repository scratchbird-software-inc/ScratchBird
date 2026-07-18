// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "firebird_catalog_projection.hpp"
#include "firebird_global_aggregate_projection.hpp"
#include "firebird_procedural_block.hpp"
#include "firebird_relation_projection_view.hpp"
#include "firebird_transaction_policy.hpp"
#include "parser_server_client.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::firebird {

struct FirebirdPipelineResult {
  bool accepted{false};
  bool frontdoor_cache_hit{false};
  bool parser_executes_sql{false};
  bool cached_storage_authority{false};
  bool cached_authorization_authority{false};
  bool cached_finality_authority{false};
  std::string statement_family;
  std::string operation_family;
  std::uint64_t statement_hash{0};
  std::string sblr_payload;
  // Preserve the complete neutral V2 outcome.  In particular, callers must
  // classify finality and retaining replacement from these typed fields; the
  // Firebird adapter never projects them into mutable session state.
  ipc::ServerExecutionResult server_execution;
  // Non-authoritative compatibility aliases for existing Firebird row/cursor
  // formatting code. They are copied only from server_execution and never
  // carry transaction identity, snapshot, replacement, or finality state.
  std::uint64_t server_row_count{0};
  std::uint64_t server_affected_rows{0};
  bool server_affected_rows_present{false};
  std::string server_cursor_uuid;
  std::string server_result_payload;
  // Opaque server-owned identity returned only by the neutral prepare route.
  // Firebird retains it as routing state; it never derives identity from SQL.
  std::string prepared_statement_uuid;
  ipc::ServerPrepareSblrResult server_prepare;
  // Non-authoritative presentation binding copied only from the exact V3
  // relation descriptor used to lower the bounded catalog projection.
  std::string catalog_relation_uuid;
  std::string catalog_relation_descriptor_uuid;
  std::uint64_t catalog_relation_descriptor_generation{0};
  // For SELECT, the effective variant is populated only after the exact
  // engine-owned persisted view descriptor refines the unbound parser shape.
  FirebirdCatalogProjectionRoute catalog_projection_route;
  FirebirdGlobalCountProjectionRoute global_count_projection_route;
  FirebirdGlobalAvgProjectionRoute global_avg_projection_route;
  FirebirdGlobalAvgResultKind global_avg_projection_result_kind{
      FirebirdGlobalAvgResultKind::kUnsupported};
  FirebirdGlobalAggregateViewCreateRoute
      global_aggregate_view_create_route;
  FirebirdGlobalAggregateViewSelectRoute
      global_aggregate_view_select_route;
  FirebirdGlobalAvgResultKind global_aggregate_view_result_kind{
      FirebirdGlobalAvgResultKind::kUnsupported};
  std::string global_aggregate_view_result_alias;
  FirebirdRelationProjectionViewCreateRoute
      relation_projection_view_create_route;
  FirebirdRelationProjectionViewCreateV2Route
      relation_projection_view_create_v2_route;
  FirebirdRelationProjectionViewDeleteV2Route
      relation_projection_view_delete_v2_route;
  FirebirdRelationProjectionViewSelectRoute
      relation_projection_view_select_route;
  std::vector<FirebirdRelationProjectionViewOutputDescriptor>
      relation_projection_view_outputs;
  std::string relation_projection_view_uuid;
  std::string relation_projection_view_descriptor_uuid;
  std::uint64_t relation_projection_view_descriptor_generation{0};
  FirebirdBoundedExecuteBlockRoute procedural_block_route;
  ipc::MessageVectorSet messages;
};

// Applies the exact state transition used only after semantic view resolution
// reports NAME_NOT_FOUND and the same SELECT * binds as an ordinary table.
// The ordinary table envelope/result state remains intact; all speculative
// aggregate-view presentation state is discarded.
void ApplyFirebirdOrdinaryRelationSelectFallback(
    FirebirdPipelineResult* result,
    std::string ordinary_select_sblr);

// The engine-owned routine route is intentionally bounded to the exact
// Firebird regression shapes described here.  Other procedure syntax remains
// available to the compatibility presentation layer, but it is not promoted
// to executable SBLR by this classifier.
enum class FirebirdBoundedProcedureRouteKind {
  kUnsupported,
  kCreateOrAlterMetadataOnly,
  kCreateOrAlterDeleteColumnRangeCount,
  kInvokeLiteralIntegerPair,
};

struct FirebirdBoundedProcedureRoute {
  FirebirdBoundedProcedureRouteKind kind{
      FirebirdBoundedProcedureRouteKind::kUnsupported};
  std::string procedure_name;
  bool procedure_quoted{false};
  std::vector<std::string> parameter_names;
  std::string return_name;
  std::string relation_name;
  bool relation_quoted{false};
  std::string column_name;
  bool column_quoted{false};
  std::vector<std::int64_t> literal_arguments;

  [[nodiscard]] bool recognized() const {
    return kind != FirebirdBoundedProcedureRouteKind::kUnsupported;
  }
};

FirebirdBoundedProcedureRoute ParseFirebirdBoundedProcedureRoute(
    std::string_view firebird_sql);

// Bounded Firebird D1 referential-constraint route.  This is syntax evidence
// only: object/column/support UUIDs are bound from the neutral engine on the
// exact selected MGA transaction before an executable envelope is emitted.
// Composite keys, actions, and deferred timing are deliberately refused.
struct FirebirdForeignKeyAlterRoute {
  bool attempted{false};
  bool supported{false};
  std::string child_table_name;
  bool child_table_quoted{false};
  std::string constraint_name;
  bool constraint_name_quoted{false};
  std::string child_column_name;
  bool child_column_quoted{false};
  std::string parent_table_name;
  bool parent_table_quoted{false};
  std::string parent_column_name;
  bool parent_column_quoted{false};
  std::string refusal_detail;

  [[nodiscard]] bool recognized() const { return attempted && supported; }
};

FirebirdForeignKeyAlterRoute ParseFirebirdForeignKeyAlterRoute(
    std::string_view firebird_sql);

// Deterministic projection used by the production binder and its focused
// conformance probe.  Every UUID is supplied by an engine resolution result;
// this function never allocates identity or embeds source SQL.
std::string EncodeFirebirdBoundedProcedureEnvelope(
    const FirebirdBoundedProcedureRoute& route,
    std::string_view schema_uuid,
    std::string_view relation_uuid,
    std::string_view column_uuid,
    std::string_view procedure_uuid);

std::string_view FirebirdBoundedProcedureRouteName(
    FirebirdBoundedProcedureRouteKind kind);

class FirebirdExecutionSession {
 public:
  explicit FirebirdExecutionSession(ipc::ParserClientConfig config);

  bool AuthenticateCredentials(const ipc::AuthCredentialEnvelope& credentials,
                               ipc::MessageVectorSet* messages);

  // The initial selector is published by the engine in the attach result.
  // It is an immutable hand-off only; finality responses never mutate it.
  [[nodiscard]] ipc::ParserTransactionSelector InitialTransactionSelector()
      const;

  // Open one additional, independent engine-owned MGA transaction.  The
  // policy is parser-owned request metadata; identity and admission are
  // allocated and decided by the engine.
  ipc::ServerExecutionResult BeginAdditional(
      const FirebirdTransactionPolicy& policy) const;

  ipc::ServerExecutionResult ExecuteSblrRouted(
      std::string_view encoded_sblr_envelope,
      const ipc::ParserTransactionSelector& transaction,
      bool cursor_requested = false) const;
  ipc::ServerPrepareSblrResult PrepareSblrRouted(
      std::string_view encoded_sblr_envelope,
      const ipc::ParserTransactionSelector& transaction) const;
  ipc::ServerExecutionResult ExecutePreparedSblrRouted(
      std::string_view prepared_statement_uuid,
      const ipc::ParserTransactionSelector& transaction,
      std::string_view encoded_sblr_envelope = {},
      const std::vector<std::uint8_t>& data_packet = {},
      bool cursor_requested = false) const;
  ipc::ServerClosePreparedSblrResult ClosePreparedSblrOnRoute(
      std::string_view prepared_statement_uuid) const;
  ipc::PublicNameResolutionResult ResolveNamePublicOnTransaction(
      std::string_view presented_name,
      bool quoted,
      std::string_view object_class,
      const ipc::ParserTransactionSelector& transaction) const;
  ipc::PublicNameResolutionResult ResolveNameSemanticPublicOnTransaction(
      std::string_view presented_name,
      bool quoted,
      std::string_view object_class,
      const ipc::ParserTransactionSelector& transaction) const;
  // Read the engine-owned persisted relation descriptor on one exact MGA
  // transaction.  The returned projection is neutral metadata only; this
  // parser may render it as Firebird SQLDA/catalog metadata but must not cache
  // it as storage or transaction authority.
  ipc::PublicNameResolutionResult ResolveRelationDescriptorPublicOnTransaction(
      std::string_view presented_name,
      bool quoted,
      const ipc::ParserTransactionSelector& transaction) const;

  // Exact-selector finality operations.  The full neutral typed result is
  // returned even when accepted=false (for example, known-applied finality
  // followed by a replacement-boundary failure).
  ipc::ServerExecutionResult CommitTransaction(
      const ipc::ParserTransactionSelector& transaction) const;
  ipc::ServerExecutionResult RollbackTransaction(
      const ipc::ParserTransactionSelector& transaction) const;
  ipc::ServerExecutionResult CommitRetainingTransaction(
      const ipc::ParserTransactionSelector& transaction) const;
  ipc::ServerExecutionResult RollbackRetainingTransaction(
      const ipc::ParserTransactionSelector& transaction) const;

  // Bind names and lower a directly supported Firebird statement against one
  // exact engine transaction without executing SBLR or running DDL preludes.
  // This is the only lowering path admitted by prepared DSQL.
  FirebirdPipelineResult BindAndLowerForPrepare(
      std::string_view firebird_sql,
      const ipc::ParserTransactionSelector& transaction,
      std::string_view database_default_charset = {},
      std::string_view attachment_charset = {});
  FirebirdPipelineResult PrepareStatement(
      std::string_view firebird_sql,
      const ipc::ParserTransactionSelector& transaction,
      std::string_view database_default_charset = {},
      std::string_view attachment_charset = {});
  FirebirdPipelineResult BindAndLowerCatalogProjection(
      const FirebirdCatalogProjectionRoute& route,
      const ipc::ParserTransactionSelector& transaction);
  FirebirdPipelineResult PrepareCatalogProjection(
      const FirebirdCatalogProjectionRoute& route,
      const ipc::ParserTransactionSelector& transaction);
  FirebirdPipelineResult RunCatalogProjection(
      const FirebirdCatalogProjectionRoute& route,
      const ipc::ParserTransactionSelector& transaction,
      bool submit);
  FirebirdPipelineResult ExecuteRecreateDropPrelude(
      std::string_view presented_name,
      bool quoted,
      const ipc::ParserTransactionSelector& transaction);

  FirebirdPipelineResult RunStatement(std::string_view firebird_sql,
                                      const ipc::ParserTransactionSelector& transaction,
                                      bool submit,
                                      bool cursor_requested = false,
                                      std::uint64_t stream_row_count = 0,
                                      bool autocommit_emulation = false,
                                      std::string_view database_default_charset = {},
                                      std::string_view attachment_charset = {});
  FirebirdPipelineResult RunSblrEnvelope(std::string_view encoded_sblr_envelope,
                                        const ipc::ParserTransactionSelector& transaction,
                                        bool cursor_requested = false);
  ipc::ServerFetchResult FetchCursorOnRoute(std::string_view cursor_uuid,
                                            std::uint64_t max_rows = 1,
                                            std::uint64_t max_bytes = 0,
                                            std::uint32_t fetch_flags = 0);
  ipc::ServerCloseCursorResult CloseCursorOnRoute(std::string_view cursor_uuid);
  ipc::ServerCloseCursorResult CancelCursorOnRoute(std::string_view cursor_uuid);
  bool DisconnectSession(ipc::MessageVectorSet* messages);

  [[nodiscard]] const ipc::ParserSessionContext& session() const {
    return session_;
  }

 private:
  ipc::ParserClientConfig config_;
  ipc::ParserSessionContext session_;
  // One physical parser/server channel belongs to one Firebird execution
  // session.  Reusing the client prevents path-global transport state from
  // aliasing separate Firebird attachments.
  mutable ipc::SbpsClient client_;

  [[nodiscard]] bool HasExecutionRoute() const;
};

} // namespace scratchbird::parser::firebird
