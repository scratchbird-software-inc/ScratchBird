#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Guard Firebird wire handles against parser-owned MGA identity/finality."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


def extract_definition(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise ValueError(f"missing definition marker: {marker}")
    opening = source.find("{", start)
    if opening < 0:
        raise ValueError(f"missing definition body: {marker}")
    depth = 0
    index = opening
    state = "code"
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "/" and next_char == "/":
                state = "line_comment"
                index += 1
            elif char == "/" and next_char == "*":
                state = "block_comment"
                index += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
        elif state in {"string", "character"}:
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment" and char == "*" and next_char == "/":
            state = "code"
            index += 1
        index += 1
    raise ValueError(f"unterminated definition body: {marker}")


def require_tokens(
    findings: list[str], label: str, text: str, tokens: tuple[str, ...]
) -> None:
    for token in tokens:
        if token not in text:
            findings.append(f"{label}: missing {token}")


def forbid_tokens(
    findings: list[str], label: str, text: str, tokens: tuple[str, ...]
) -> None:
    for token in tokens:
        if token in text:
            findings.append(f"{label}: forbidden {token}")


def require_before(
    findings: list[str], label: str, text: str, first: str, second: str
) -> None:
    first_at = text.find(first)
    second_at = text.find(second)
    if first_at < 0 or second_at < 0 or first_at >= second_at:
        findings.append(f"{label}: {first} must precede {second}")


def section(source: str, begin: str, end: str) -> str:
    start = source.find(begin)
    if start < 0:
        raise ValueError(f"missing section marker: {begin}")
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise ValueError(f"missing section terminator: {end}")
    return source[start:finish]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker-source", required=True)
    parser.add_argument("--dialect-source", required=True)
    parser.add_argument("--execution-source", required=True)
    parser.add_argument("--server-source", required=True)
    parser.add_argument("--client-source", required=True)
    args = parser.parse_args()

    paths = tuple(
        pathlib.Path(value).resolve()
        for value in (
            args.worker_source,
            args.dialect_source,
            args.execution_source,
            args.server_source,
            args.client_source,
        )
    )
    findings: list[str] = []
    for path in paths:
        if not path.is_file():
            findings.append(f"missing source file: {path}")
    if findings:
        print("\n".join(findings), file=sys.stderr)
        return 1

    worker, dialect, execution, server, client = (
        path.read_text(encoding="utf-8") for path in paths
    )
    client_header_path = paths[-1].with_suffix(".hpp")
    if not client_header_path.is_file():
        print(f"missing source file: {client_header_path}", file=sys.stderr)
        return 1
    client_header = client_header_path.read_text(encoding="utf-8")
    try:
        select = extract_definition(
            worker, "std::optional<std::string> SelectFirebirdExecutionTransaction("
        )
        start = extract_definition(
            worker, "std::optional<std::string> StartFirebirdExecutionTransaction("
        )
        finish = extract_definition(
            worker, "std::optional<std::string> FinishFirebirdTransaction("
        )
        retire_statement_routes = extract_definition(
            worker, "void RetireFirebirdStatementRoutesForFinalizedSelector("
        )
        retire_all_statement_routes = extract_definition(
            worker, "void RetireAllFirebirdPreparedAndCursorRoutes("
        )
        close_statement_cursor = extract_definition(
            worker,
            "FirebirdStatementRouteCloseResult CloseFirebirdStatementCursor(",
        )
        close_statement_prepared = extract_definition(
            worker,
            "FirebirdStatementRouteCloseResult CloseFirebirdStatementPreparedRoute(",
        )
        close_quarantine_classifier = extract_definition(
            worker,
            "bool FirebirdStatementCloseRequiresRouteQuarantine(",
        )
        worker_prepare_route = extract_definition(
            worker, "FirebirdPipelineResult PrepareFirebirdStatementOnExactRoute("
        )
        worker_execute_prepared = extract_definition(
            worker,
            "FirebirdPipelineResult ExecuteFirebirdPreparedStatementOnExactRoute(",
        )
        prepare_quarantine_classifier = extract_definition(
            worker, "bool FirebirdPrepareRequiresRouteQuarantine("
        )
        execute_quarantine_classifier = extract_definition(
            worker, "bool FirebirdPreparedExecuteRequiresRouteQuarantine("
        )
        quarantine_route = extract_definition(
            worker, "void QuarantineFirebirdPreparedPhysicalRoute("
        )
        rollback_parent = extract_definition(
            worker,
            "std::optional<std::string> RollbackFirebirdExecutionTransactionsForParent(",
        )
        scoped_execute = extract_definition(
            worker,
            "FirebirdPipelineResult ExecuteFirebirdStatement(\n"
            "    FirebirdBinarySessionState* state,\n"
            "    std::string_view sql_text,\n"
            "    bool cursor_requested) {",
        )
        transaction_classifier = extract_definition(
            dialect, "FirebirdTransactionControl ClassifyFirebirdTransactionControl("
        )
        execution_run = extract_definition(
            execution, "FirebirdPipelineResult FirebirdExecutionSession::RunStatement("
        )
        execution_bind_prepare = extract_definition(
            execution,
            "FirebirdPipelineResult FirebirdExecutionSession::BindAndLowerForPrepare(",
        )
        execution_prepare = extract_definition(
            execution,
            "FirebirdPipelineResult FirebirdExecutionSession::PrepareStatement(",
        )
        execution_disconnect = extract_definition(
            execution, "bool FirebirdExecutionSession::DisconnectSession("
        )
        execution_close_prepared = extract_definition(
            execution, "FirebirdExecutionSession::ClosePreparedSblrOnRoute("
        )
        prepare_unknown_projection = extract_definition(
            client, "void ProjectV2PrepareOutcomeUnknown("
        )
        prepare_transport_projection = extract_definition(
            client, "void ProjectV2PrepareTransportOutcomeUnknown("
        )
        connect_cached_socket = extract_definition(
            client, "SbpsSocketHandle ConnectCachedSbpsSocket("
        )
        request_retry_policy = extract_definition(
            client, "bool RequestMayRetryAfterTransportLoss("
        )
        send_request = section(
            client, "bool SendRequest(", "FrameHeader BaseHeader("
        )
        close_transport_projection = extract_definition(
            client, "void ProjectCloseTransportFailure("
        )
        neutral_prepare_route = extract_definition(
            client, "ServerPrepareSblrResult SbpsClient::PrepareSblrRouted("
        )
        neutral_close_prepared = extract_definition(
            client,
            "ServerClosePreparedSblrResult SbpsClient::ClosePreparedSblr(",
        )
        neutral_close_cursor = extract_definition(
            client, "ServerCloseCursorResult SbpsClient::CloseCursor("
        )
        close_cursor_result_type = section(
            client_header,
            "struct ServerCloseCursorResult",
            "struct ServerClosePreparedSblrResult",
        )
        close_prepared_result_type = section(
            client_header,
            "struct ServerClosePreparedSblrResult",
            "// Deterministic protocol-conformance hook.",
        )
        server_close_prepared = extract_definition(
            server, "SessionOperationResult HandleClosePreparedSblr("
        )
        immediate = section(
            worker,
            "if (opcode == 64 || opcode == 75)",
            "if (opcode == 68)",
        )
        execute = section(
            worker,
            "if (opcode == 63 || opcode == 76)",
            "if (opcode == 65)",
        )
        prepare = section(
            worker,
            "if (opcode == 68)",
            "if (opcode == 70)",
        )
        prepared_route = section(
            worker,
            "// FIREBIRD_PREPARED_DSQL_ROUTE_BEGIN",
            "// FIREBIRD_PREPARED_DSQL_ROUTE_END",
        )
        fetch = section(worker, "if (opcode == 65)", "if (opcode == 67)")
        free_statement = section(
            worker,
            "if (opcode == 67)",
            "const auto response = FirebirdWishListResponsePacket(",
        )
        wire_finality = section(
            worker,
            "if (opcode == 30 || opcode == 31 || opcode == 50 || opcode == 86)",
            "if (opcode == 22)",
        )
    except ValueError as error:
        findings.append(str(error))
        select = start = finish = rollback_parent = scoped_execute = ""
        retire_statement_routes = retire_all_statement_routes = ""
        close_statement_cursor = close_statement_prepared = worker_prepare_route = ""
        worker_execute_prepared = execution_bind_prepare = execution_prepare = ""
        close_quarantine_classifier = ""
        prepare_quarantine_classifier = execute_quarantine_classifier = ""
        quarantine_route = ""
        prepare_unknown_projection = prepare_transport_projection = ""
        connect_cached_socket = request_retry_policy = send_request = ""
        close_transport_projection = ""
        neutral_prepare_route = neutral_close_prepared = neutral_close_cursor = ""
        close_cursor_result_type = close_prepared_result_type = ""
        server_close_prepared = ""
        execution_close_prepared = ""
        transaction_classifier = execution_run = execution_disconnect = ""
        immediate = execute = prepare = prepared_route = fetch = ""
        free_statement = wire_finality = ""

    require_tokens(
        findings,
        "V2 transaction binding",
        worker,
        (
            "ParserTransactionSelector selector",
            "FirebirdExecutionTransactionLifecycle lifecycle",
            "std::map<std::uint32_t, FirebirdExecutionTransactionBinding>",
            "execution_transactions",
            "execution_transaction_route_usable",
            "config.require_transaction_routing_v2 = true",
            "std::string prepared_statement_uuid",
            "prepare_transaction_selector",
            "cursor_owner_selector",
            "prepared_statement_stale",
            "cursor_route_stale",
        ),
    )
    forbid_tokens(
        findings,
        "V2 transaction binding",
        worker,
        (
            "selected_execution_transaction_id",
            "CONCURRENT_HANDLES_UNAVAILABLE",
            "execution_transaction_active",
            "emulated_transaction_handle_started",
            "execution_transaction_id",
        ),
    )

    for label, result_type in (
        ("neutral cursor close result", close_cursor_result_type),
        ("neutral prepared close result", close_prepared_result_type),
    ):
        require_tokens(
            findings,
            label,
            result_type,
            (
                "bool outcome_unknown{false}",
                "bool caller_cleanup_required{false}",
                "bool route_fatal{false}",
            ),
        )
    require_tokens(
        findings,
        "session-bound fresh socket refusal",
        connect_cached_socket,
        (
            "bool allow_fresh_connection",
            "if (!allow_fresh_connection) return kInvalidSbpsSocket",
        ),
    )
    require_tokens(
        findings,
        "session-bound replay refusal",
        request_retry_policy,
        (
            "!SessionBoundRequest(header)",
            "!V2RequestIsNonReplayableAfterWrite(header.schema_id)",
        ),
    )
    require_tokens(
        findings,
        "session-bound transport routing",
        send_request,
        (
            "const bool session_bound = SessionBoundRequest(header)",
            "!session_bound",
            "AddSessionRouteUnavailable",
            "AddTransportOutcomeUnknown",
        ),
    )
    if send_request.count("!RequestMayRetryAfterTransportLoss(header)") < 2:
        findings.append(
            "session-bound transport routing: write and read loss must both refuse replay"
        )
    require_tokens(
        findings,
        "typed close transport projection",
        close_transport_projection,
        (
            "PARSER_SERVER_IPC.OUTCOME_UNKNOWN",
            "PARSER_SERVER_IPC.SESSION_ROUTE_UNAVAILABLE",
            "outcome_unknown = outcome_unknown",
            "caller_cleanup_required = true",
            "route_fatal = true",
        ),
    )

    require_tokens(
        findings,
        "exact transaction selection",
        select,
        (
            "execution_transactions.find(transaction_id)",
            "FirebirdExecutionTransactionBindingIsActive",
            "execution_transaction_route_usable",
            "FIREBIRD.TRANSACTION.ENGINE_BINDING_REQUIRED",
            "FIREBIRD.TRANSACTION.ENGINE_IDENTITY_MISMATCH",
        ),
    )
    forbid_tokens(
        findings,
        "exact transaction selection",
        select,
        ("session().local_transaction_id", "session().transaction_uuid"),
    )

    require_tokens(
        findings,
        "multi-handle begin",
        start,
        (
            "BeginAdditional(policy)",
            "begun.selected_transaction_present",
            "begun.selected_transaction.present()",
            "InitialTransactionSelector",
            "FIREBIRD.TRANSACTION.ENGINE_IDENTITY_ALIAS",
            "RetireAllFirebirdWireTransactionHandles",
            "execution_transaction_route_usable = false",
            "execution_transactions.emplace(transaction_id, binding)",
        ),
    )
    forbid_tokens(
        findings,
        "multi-handle begin",
        start,
        ("RunStatement", '"BEGIN TRANSACTION"', "execution_transactions.empty()"),
    )
    require_tokens(
        findings,
        "Firebird policy admission",
        worker,
        (
            "ParseFirebirdTransactionTpb(request.parameter_buffer)",
            "ParseFirebirdSetTransactionSql(sql_text)",
            "FirebirdTransactionPolicyDiagnosticJson",
            "parsed_policy.policy",
            "FIREBIRD.TRANSACTION.START_UNSUPPORTED",
        ),
    )

    require_tokens(
        findings,
        "typed finality",
        finish,
        (
            "CommitTransaction",
            "RollbackTransaction",
            "CommitRetainingTransaction",
            "RollbackRetainingTransaction",
            "ParserTransactionFinality::kKnownApplied",
            "ParserTransactionFinality::kKnownNotApplied",
            "ParserTransactionFinality::kUnknown",
            "finalized_transaction_present",
            "ParserTransactionReplacementReason::kRetaining",
            "RETAINING_REPLACEMENT_UNAVAILABLE",
            "execution_transaction_route_usable = false",
            "RetireAllFirebirdWireTransactionHandles",
            "FINALITY_OUTCOME_UNKNOWN",
            "RetireFirebirdStatementRoutesForFinalizedSelector",
            "execution_transactions.erase(transaction_id)",
        ),
    )
    forbid_tokens(
        findings,
        "typed finality",
        finish,
        ("RunStatement", 'commit ? "COMMIT" : "ROLLBACK"'),
    )
    require_before(
        findings,
        "typed finality prepared retirement",
        finish,
        "RetireFirebirdStatementRoutesForFinalizedSelector",
        "execution_transactions.erase(transaction_id)",
    )
    require_tokens(
        findings,
        "prepared and cursor exact retirement",
        retire_statement_routes,
        (
            "prepare_transaction_selector",
            "cursor_owner_selector",
            "FirebirdTransactionSelectorsEqual",
            "prepared_statement_stale = true",
            "cursor_route_stale = true",
            "DiscardFirebirdStatementCursorData",
        ),
    )
    require_tokens(
        findings,
        "unknown route retirement",
        retire_all_statement_routes,
        (
            "prepared_statement_stale = true",
            "cursor_route_stale = true",
            "DiscardFirebirdStatementCursorData",
        ),
    )

    require_tokens(
        findings,
        "all-handle cleanup",
        rollback_parent,
        (
            "std::optional<std::string> first_diagnostic",
            "FinishFirebirdTransaction(",
            "if (!first_diagnostic)",
            "continue",
            "return first_diagnostic",
        ),
    )

    require_tokens(
        findings,
        "operation-scoped exact routing",
        worker,
        (
            "struct FirebirdExecutionCallContext",
            "class FirebirdExecutionCallScope",
            "execution_call_context = &context_",
            "execution_call_context = nullptr",
            "FirebirdTransactionSelectorsEqual",
        ),
    )
    require_tokens(
        findings,
        "nested exact routing",
        scoped_execute,
        (
            "execution_call_context",
            "FirebirdExecutionTransactionSelector",
            "FirebirdTransactionSelectorsEqual",
            "FIREBIRD.TRANSACTION.SELECTOR_REQUIRED",
        ),
    )
    direct_unrouted = re.findall(
        r"execution_session->RunStatement\s*\(\s*[^,]+,\s*(?:true|false)\b",
        worker,
    )
    if direct_unrouted:
        findings.append(
            f"exact routed execution: found {len(direct_unrouted)} RunStatement call(s) without a selector second argument"
        )

    require_tokens(
        findings,
        "immediate route gate",
        immediate,
        (
            "FirebirdExecutionCallScope execution_call_scope",
            "if (!execution_call_scope.active())",
            "fail_closed_exact_transaction_selector_required",
        ),
    )
    require_before(
        findings,
        "immediate route gate",
        immediate,
        "if (!execution_call_scope.active())",
        "ApplyFirebirdUserMutationOverlay",
    )
    require_tokens(
        findings,
        "prepared execute route gate",
        execute,
        (
            "FirebirdExecutionCallScope execution_call_scope",
            "if (!execution_call_scope.active())",
            "prepared_statement_uuid",
            "prepare_transaction_selector",
            "cursor_owner_selector",
            "ExecuteFirebirdPreparedStatementOnExactRoute",
            "FIREBIRD.DSQL.PARAMETER_PACKET_UNAVAILABLE",
        ),
    )
    require_tokens(
        findings,
        "Firebird attachment statement to exact SBPS prepare rebind",
        prepared_route,
        (
            "prepared_route_requires_rebind",
            "CloseFirebirdStatementPreparedRoute",
            "PrepareFirebirdStatementOnExactRoute",
            "op_execute_rebind",
            "firebird_execute_selector_changed",
            "accepted_exact_execute_selector",
        ),
    )
    require_before(
        findings,
        "Firebird attachment statement to exact SBPS prepare rebind",
        prepared_route,
        "CloseFirebirdStatementPreparedRoute",
        "PrepareFirebirdStatementOnExactRoute",
    )
    require_before(
        findings,
        "exact rebind before prepared execution",
        prepared_route,
        "PrepareFirebirdStatementOnExactRoute",
        "ExecuteFirebirdPreparedStatementOnExactRoute",
    )
    if "prepare_transaction_selector = *selected_transaction" in prepared_route:
        findings.append(
            "prepared execute selector rebind: old opaque prepared identity selector was rewritten instead of retired and freshly prepared"
        )
    require_before(
        findings,
        "prepared execute route gate",
        execute,
        "if (!execution_call_scope.active())",
        "ExecuteFirebirdPreparedStatementOnExactRoute",
    )
    require_tokens(
        findings,
        "prepared worker prepare route",
        worker_prepare_route,
        (
            "PrepareStatement(",
            "statement->sql_text, *selector,",
            "state->database_default_charset",
            "state->current_attachment_charset",
            "prepared.prepared_statement_uuid",
            "statement->prepared_statement_uuid",
            "statement->prepare_transaction_selector = *selector",
        ),
    )
    require_tokens(
        findings,
        "prepared worker execute route",
        worker_execute_prepared,
        (
            "ExecutePreparedSblrRouted",
            "statement->prepared_statement_uuid",
            "statement->prepare_transaction_selector",
            "FirebirdTransactionSelectorsEqual",
            "executed.selected_transaction_present",
            "executed.selected_transaction",
            "FIREBIRD.DSQL.PREPARED_TRANSACTION_SELECTOR_MISMATCH",
        ),
    )
    forbid_tokens(
        findings,
        "prepared worker execute route",
        worker_execute_prepared,
        ("ExecuteFirebirdStatement", "RunStatement", "PrepareStatement"),
    )
    require_tokens(
        findings,
        "prepared DSQL isolated route",
        prepared_route,
        (
            "prepared_statement_uuid",
            "prepare_transaction_selector",
            "FirebirdTransactionSelectorsEqual",
            "ExecuteFirebirdPreparedStatementOnExactRoute",
            "ExecuteRecreateDropPrelude",
            "CloseFirebirdStatementCursor",
            "FirebirdPreparedExecuteRequiresRouteQuarantine",
            "QuarantineFirebirdPreparedPhysicalRoute",
            "const auto failure_packet",
            "continue",
        ),
    )
    require_before(
        findings,
        "prepared execute quarantine pointer safety",
        prepared_route,
        "const auto failure_packet",
        "QuarantineFirebirdPreparedPhysicalRoute",
    )
    require_tokens(
        findings,
        "prepared cursor close quarantine",
        prepared_route,
        (
            "FirebirdWishListResponsePacket(*close_result.diagnostic)",
            "prepared_cursor_close_transport_outcome_unknown",
        ),
    )
    forbid_tokens(
        findings,
        "prepared DSQL isolated route",
        prepared_route,
        (
            "ExecuteFirebirdStatement",
            "RunStatement",
            "RecordFirebirdCreateTable",
            "EnsureFirebirdQaMonitoringOverlay",
            "RecordFirebirdInsertedBlobValues",
            "ApplyFirebirdAfterInsertTriggers",
            "AttachFirebirdBlobCells",
        ),
    )
    require_tokens(
        findings,
        "executable prepare installation",
        prepare,
        (
            "CloseFirebirdStatementCursor",
            "ClearFirebirdStatementPreparedRoute",
            "ClearFirebirdStatementPreparationDescriptor",
            "PrepareFirebirdStatementOnExactRoute",
            "FirebirdPrepareRequiresRouteQuarantine",
            "QuarantineFirebirdPreparedPhysicalRoute",
            "parser_local_metadata_only = !transaction_control_prepare",
            "parser_local_transaction_control =",
            "exact_mga_finality_route",
            "FirebirdWishListResponsePacket(*close_result.diagnostic)",
            "statement_close_transport_outcome_unknown",
        ),
    )
    require_before(
        findings,
        "reprepare close quarantine pointer safety",
        prepare,
        "FirebirdWishListResponsePacket(*close_result.diagnostic)",
        "statement_close_transport_outcome_unknown",
    )
    require_tokens(
        findings,
        "metadata cursor close quarantine",
        execute,
        (
            "FirebirdWishListResponsePacket(*close_result.diagnostic)",
            "metadata_cursor_close_transport_outcome_unknown",
        ),
    )
    require_tokens(
        findings,
        "prepare unknown classifier",
        prepare_quarantine_classifier,
        ("server_prepare.outcome_unknown", "caller_cleanup_required"),
    )
    require_tokens(
        findings,
        "resource close route-fatal classifier",
        close_quarantine_classifier,
        (
            "closed.route_fatal",
            "closed.outcome_unknown",
            "closed.caller_cleanup_required",
        ),
    )
    require_tokens(
        findings,
        "prepared execute unknown classifier",
        execute_quarantine_classifier,
        (
            "ParserTransactionFinality::kUnknown",
            "selected_transaction_present",
            "selected_transaction.present()",
            "FirebirdTransactionSelectorsEqual",
        ),
    )
    require_tokens(
        findings,
        "prepared route quarantine",
        quarantine_route,
        (
            "execution_transaction_route_usable = false",
            "RetireAllFirebirdWireTransactionHandles",
            "DisconnectFirebirdExecutionSession",
            "execution_session.reset()",
        ),
    )
    require_before(
        findings,
        "reprepare clears old authority",
        prepare,
        "ClearFirebirdStatementPreparedRoute",
        "PrepareFirebirdStatementOnExactRoute",
    )
    require_before(
        findings,
        "reprepare closes neutral prepared resource before local clear",
        prepare,
        "CloseFirebirdStatementPreparedRoute",
        "ClearFirebirdStatementPreparedRoute",
    )
    require_tokens(
        findings,
        "cursor owner fetch gate",
        fetch,
        (
            "cursor_owner_selector",
            "FirebirdActiveTransactionSelectorMatches",
            "FIREBIRD.DSQL.CURSOR_TRANSACTION_RETIRED",
            "DiscardFirebirdStatementCursorData",
            "FetchCursorOnRoute",
        ),
    )
    require_before(
        findings,
        "cursor owner fetch gate",
        fetch,
        "FirebirdActiveTransactionSelectorMatches",
        "FetchCursorOnRoute",
    )
    require_tokens(
        findings,
        "cursor close lifecycle",
        close_statement_cursor,
        (
            "cursor_owner_selector",
            "FirebirdActiveTransactionSelectorMatches",
            "CloseCursorOnRoute",
            "ClearFirebirdStatementCursorRoute",
            "FirebirdStatementCloseRequiresRouteQuarantine",
            "if (closed.accepted && !quarantine_route)",
            "fail_closed_neutral_cursor_close_outcome_unknown",
        ),
    )
    forbid_tokens(
        findings,
        "cursor close deterministic rejection retention",
        close_statement_cursor,
        (
            "CloseCursorOnRoute(cursor_uuid);\n  ClearFirebirdStatementCursorRoute",
            "ClearFirebirdStatementCursorRoute(statement);\n  if (closed.accepted)",
        ),
    )
    require_tokens(
        findings,
        "session-owned prepared close lifecycle",
        close_statement_prepared,
        (
            "prepared_statement_uuid",
            "ClosePreparedSblrOnRoute",
            "fail_closed_neutral_prepared_close_rejected",
            "FirebirdStatementCloseRequiresRouteQuarantine",
            "fail_closed_neutral_prepared_close_outcome_unknown",
        ),
    )
    forbid_tokens(
        findings,
        "session-owned prepared close lifecycle",
        close_statement_prepared,
        (
            "prepare_transaction_selector",
            "FirebirdActiveTransactionSelectorMatches",
            "SBSql",
            "sbsql",
        ),
    )
    require_tokens(
        findings,
        "DSQL free lifecycle",
        free_statement,
        (
            "kDsqlClose",
            "kDsqlDrop",
            "kDsqlUnprepare",
            "CloseFirebirdStatementCursor",
            "CloseFirebirdStatementPreparedRoute",
            "ClearFirebirdStatementPreparedRoute",
            "RemoveHandle",
            "FirebirdWishListResponsePacket(*close_result.diagnostic)",
            "statement_release_transport_outcome_unknown",
        ),
    )
    require_before(
        findings,
        "DSQL release quarantine pointer safety",
        free_statement,
        "FirebirdWishListResponsePacket(*close_result.diagnostic)",
        "statement_release_transport_outcome_unknown",
    )
    require_before(
        findings,
        "DSQL unprepare closes neutral prepared resource before local clear",
        free_statement,
        "CloseFirebirdStatementPreparedRoute",
        "ClearFirebirdStatementPreparedRoute",
    )
    require_before(
        findings,
        "DSQL drop retains handle until neutral close succeeds",
        free_statement,
        "CloseFirebirdStatementPreparedRoute",
        "RemoveHandle",
    )
    require_tokens(
        findings,
        "wire finality flush route gate",
        wire_finality,
        (
            "FirebirdExecutionCallScope execution_call_scope",
            "if (!execution_call_scope.active())",
            "FlushFirebirdPendingRestoreTables",
        ),
    )
    require_before(
        findings,
        "wire finality flush route gate",
        wire_finality,
        "if (!execution_call_scope.active())",
        "FlushFirebirdPendingRestoreTables",
    )

    require_tokens(
        findings,
        "execution adapter V2 surface",
        execution,
        (
            "config_.require_transaction_routing_v2 = true",
            "BeginAdditional(",
            "ExecuteSblrRouted(",
            "PrepareSblrRouted(",
            "CommitRetainingTransaction(",
            "RollbackRetainingTransaction(",
            "server_execution",
            "ProjectNonAuthoritativeRowResult",
        ),
    )
    require_tokens(
        findings,
        "mutation-free prepared bind/lower",
        execution_bind_prepare,
        (
            "LexTokens(firebird_sql)",
            'token.kind == "parameter"',
            "FIREBIRD.DSQL.PARAMETER_PACKET_UNAVAILABLE",
            "create_table->recreate = false",
            "ResolveNamePublicOnTransaction",
            "CreateTableEnvelope",
            "InsertEnvelope",
            "UpdateEnvelope",
            "DeleteEnvelope",
            "DropTableEnvelope",
            "SelectEnvelope",
        ),
    )
    forbid_tokens(
        findings,
        "mutation-free prepared bind/lower",
        execution_bind_prepare,
        (
            "ExecuteSblrRouted",
            "ExecutePreparedSblrRouted",
            "CreateDefaultSchemaEnvelope",
            "RunStatement",
        ),
    )
    require_tokens(
        findings,
        "neutral prepared admission",
        execution_prepare,
        (
            "BindAndLowerForPrepare",
            "PrepareSblrRouted",
            "prepared.prepared_statement_uuid",
            "FIREBIRD.DSQL.PREPARED_UUID_MISSING",
        ),
    )
    forbid_tokens(
        findings,
        "neutral prepared admission",
        execution_prepare,
        ("ExecuteSblrRouted", "ExecutePreparedSblrRouted", "RunStatement"),
    )
    require_tokens(
        findings,
        "execution exact selector",
        execution_run,
        (
            "const ipc::ParserTransactionSelector& transaction",
            "transaction.present()",
            "ExecuteSblrRouted(result.sblr_payload, transaction",
        ),
    )
    forbid_tokens(
        findings,
        "execution exact selector",
        execution_run,
        ("ApplyTransactionState", "session_.local_transaction_id ="),
    )
    require_tokens(
        findings,
        "execution disconnect",
        execution_disconnect,
        ("client_.DisconnectSession(session_, messages)", "session_ = {}"),
    )
    require_tokens(
        findings,
        "Firebird neutral prepared close adapter",
        execution_close_prepared,
        ("client_.ClosePreparedSblr", "prepared_statement_uuid"),
    )
    forbid_tokens(
        findings,
        "Firebird neutral prepared close adapter",
        execution_close_prepared,
        ("ParserTransactionSelector", "prepare_transaction_selector"),
    )
    require_tokens(
        findings,
        "neutral typed prepare unknown projection",
        prepare_unknown_projection,
        (
            "outcome_unknown = true",
            "caller_cleanup_required = true",
            "prepared_statement_uuid.clear()",
            "request_replayed",
        ),
    )
    require_tokens(
        findings,
        "neutral prepare transport projection",
        prepare_transport_projection,
        (
            "PARSER_SERVER_IPC.OUTCOME_UNKNOWN",
            "ProjectV2PrepareOutcomeUnknown",
            "transport_outcome_unknown",
        ),
    )
    require_tokens(
        findings,
        "neutral V2 prepare route",
        neutral_prepare_route,
        (
            "ProjectV2PrepareTransportOutcomeUnknown",
            "ProjectV2PrepareOutcomeUnknown",
            "DecodePrepareResultPayloadV2",
            "IsErrorFrame(response)",
        ),
    )
    require_tokens(
        findings,
        "neutral route-bound prepared close",
        neutral_close_prepared,
        (
            "kMessageClosePreparedSblr",
            "kSchemaClosePreparedSblrV1",
            "kMessageClosePreparedSblrResult",
            "kSchemaClosePreparedSblrResultV1",
            "EncodeClosePreparedSblrPayload",
            "ProjectCloseTransportFailure",
        ),
    )
    forbid_tokens(
        findings,
        "neutral route-bound prepared close",
        neutral_close_prepared,
        ("ParserTransactionSelector",),
    )
    require_tokens(
        findings,
        "neutral route-bound cursor close",
        neutral_close_cursor,
        (
            "kMessageCloseCursor",
            "kSchemaCloseCursorV1",
            "ProjectCloseTransportFailure",
        ),
    )
    require_tokens(
        findings,
        "server session-owned prepared close",
        server_close_prepared,
        (
            "CloseServerPreparedStatement",
            "prepared_statement_already_closed",
            "resource_session.local_transaction_id = 0",
            "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND",
        ),
    )
    forbid_tokens(
        findings,
        "server session-owned prepared close",
        server_close_prepared,
        (
            "transactions_by_local_id",
            "prepare_transaction_uuid",
            "PreparedStatementAuthorityMismatchReason",
        ),
    )

    require_tokens(
        findings,
        "Firebird-owned transaction classifier",
        transaction_classifier,
        (
            "LexTokens(sql_text)",
            'token.kind == "line_comment"',
            'token.kind == "block_comment"',
            "FirebirdTransactionControlKind::kRollbackToSavepoint",
        ),
    )
    require_tokens(
        findings,
        "neutral typed response",
        server,
        (
            "ServerTransactionResponseState::Finality::kKnownApplied",
            "ServerTransactionResponseState::Finality::kKnownNotApplied",
            "ServerTransactionResponseState::Finality::kUnknown",
            "ReplacementReason::kRetaining",
        ),
    )

    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        print(
            f"firebird_transaction_handle_authority_gate=failed findings={len(findings)}",
            file=sys.stderr,
        )
        return 1
    print("firebird_transaction_handle_authority_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
