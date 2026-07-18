#!/usr/bin/env python3
"""Static authority gate for the bounded Firebird CORE-0076 routine route."""

from __future__ import annotations

import argparse
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def section(text: str, begin: str, end: str) -> str:
    start = text.find(begin)
    require(start >= 0, f"missing section start: {begin}")
    finish = text.find(end, start + len(begin))
    require(finish >= 0, f"missing section end: {end}")
    return text[start:finish]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker-source", type=Path, required=True)
    parser.add_argument("--execution-header", type=Path, required=True)
    parser.add_argument("--execution-source", type=Path, required=True)
    parser.add_argument("--sbps-header", type=Path, required=True)
    parser.add_argument("--client-types", type=Path, required=True)
    parser.add_argument("--client-header", type=Path, required=True)
    parser.add_argument("--client-source", type=Path, required=True)
    args = parser.parse_args()

    worker = args.worker_source.read_text(encoding="utf-8")
    header = args.execution_header.read_text(encoding="utf-8")
    execution = args.execution_source.read_text(encoding="utf-8")
    sbps = args.sbps_header.read_text(encoding="utf-8")
    client_types = args.client_types.read_text(encoding="utf-8")
    client_header = args.client_header.read_text(encoding="utf-8")
    client_source = args.client_source.read_text(encoding="utf-8")

    require(
        "kCapabilityPreparedMetadataTransferV1 = 0x04u" in sbps,
        "SBPS capability 0x04 is not assigned to prepared metadata transfer",
    )
    known_capabilities = section(
        sbps,
        "constexpr std::uint8_t kKnownCapabilityByte0 =",
        "constexpr std::uint32_t kSchemaRenderUuidRequestV1",
    )
    require(
        "kCapabilityPreparedMetadataTransferV1" in known_capabilities,
        "SBPS known capability bitmap omits prepared metadata transfer",
    )
    for token in (
        "require_prepared_metadata_transfer_v1",
        "prepared_metadata_transfer_v1_negotiated",
    ):
        require(token in client_types, f"neutral client capability state missing: {token}")
    for token in (
        "kCapabilityPreparedMetadataTransferV1 = 0x04u",
        "PreparedMetadataTransferV1HelloPayloadForTest",
        "PARSER_SERVER_IPC.PREPARED_METADATA_TRANSFER_V1_REQUIRED",
        "session->prepared_metadata_transfer_v1_negotiated",
    ):
        require(token in client_header + client_source,
                f"neutral client negotiation guard missing: {token}")
    for token in (
        "config_.require_prepared_metadata_transfer_v1 = true",
        "session_.prepared_metadata_transfer_v1_negotiated",
    ):
        require(token in execution,
                f"Firebird attach does not require capability 0x04: {token}")
    require(
        "config.require_prepared_metadata_transfer_v1 = true" in worker,
        "Firebird worker client config does not require capability 0x04",
    )

    for token in (
        "kCreateOrAlterMetadataOnly",
        "kCreateOrAlterDeleteColumnRangeCount",
        "kInvokeLiteralIntegerPair",
        "ParseFirebirdBoundedProcedureRoute",
        "EncodeFirebirdBoundedProcedureEnvelope",
    ):
        require(token in header, f"bounded Firebird route declaration missing: {token}")

    encoder = section(
        execution,
        "std::string EncodeBoundedProcedureEnvelopeImpl(",
        "std::optional<std::string> EncodeDirectDefaultExpression",
    )
    for token in (
        '"ddl.create_procedure"',
        '"SBLR_DDL_CREATE_PROCEDURE"',
        "executable_descriptor_kind",
        "create_or_alter_procedure",
        "engine.routine.delete_column_range_count.v1",
        '"routine.procedure_invoke"',
        '"SBLR_PROCEDURE_INVOKE"',
        "|0|1|2|2",
    ):
        require(token in encoder, f"canonical routine envelope token missing: {token}")
    require('"contains_sql_text\\":false' in execution,
            "SBLR envelope does not prohibit SQL text")
    for forbidden in (
        "GenerateCrudEngineUuid",
        "GenerateEngineIdentity",
        "random_uuid",
        "DELETE FROM",
        "EXECUTE PROCEDURE",
        "ROW_COUNT",
        "statement_metadata_snapshot",
        "statement_metadata_snapshot_policy",
        "visible_through",
        "executable_generation",
    ):
        require(forbidden not in encoder, f"bounded envelope contains forbidden authority: {forbidden}")

    binder = section(
        execution,
        "FirebirdPipelineResult FirebirdExecutionSession::BindAndLowerForPrepare(",
        "FirebirdPipelineResult FirebirdExecutionSession::PrepareStatement(",
    )
    for token in (
        "ResolveRelationDescriptorPublicOnTransaction(",
        '"procedure"',
        "descriptor.relation_uuid != resolved_relation.object_uuid",
        "matched_column->column_uuid.empty()",
        "FIREBIRD.ROUTINE.RELATION_DESCRIPTOR_REQUIRED",
        "FIREBIRD.ROUTINE.INTEGER_COLUMN_BINDING_REQUIRED",
    ):
        require(token in binder, f"exact-selector routine binding guard missing: {token}")
    require("ExecuteFirebirdStatement" not in binder,
            "bounded binder calls the compatibility SQL executor")
    require("FirebirdToCanonical" not in binder,
            "bounded binder uses SQL translation/substitution")

    prepared_route = section(
        worker,
        "// FIREBIRD_PREPARED_DSQL_ROUTE_BEGIN",
        "// FIREBIRD_PREPARED_DSQL_ROUTE_END",
    )
    for token in (
        "ExecuteFirebirdPreparedStatementOnExactRoute(",
        "engine_routine_route_kind",
        'output.source_name = "routine_output_slot_2"',
        '"routine.procedure.result.v1"',
        '"routine_instruction:delete.uuid_bound.column_range"',
        '"routine_affected_rows_output_slot:2:"',
        "parser_overlay=none",
    ):
        require(token in worker, f"worker routine authority/presentation guard missing: {token}")
    require("ExecuteFirebirdStatement" not in prepared_route,
            "exact prepared route falls through to compatibility SQL execution")

    immediate_route = section(
        worker,
        "// FIREBIRD_EXEC_IMMEDIATE_BOUNDED_ROUTINE_ROUTE_BEGIN",
        "// FIREBIRD_EXEC_IMMEDIATE_BOUNDED_ROUTINE_ROUTE_END",
    )
    for token in (
        "ParseFirebirdBoundedProcedureRoute(sql_text)",
        "FirebirdExecutionTransactionSelector(",
        "state.execution_session->RunStatement(",
        "selected_transaction_matches",
        'FindServerRowFieldValue(fields, "object_uuid")',
        "parser_overlay=none",
    ):
        require(token in immediate_route,
                f"op_exec_immediate routine authority guard missing: {token}")
    for forbidden in (
        "ExecuteFirebirdStatement",
        "FirebirdToCanonical",
        "RecordFirebirdCreateProcedure",
    ):
        require(forbidden not in immediate_route,
                f"op_exec_immediate exact routine route uses compatibility authority: {forbidden}")
    require("RecordFirebirdCreateProcedure" not in prepared_route,
            "exact prepared create retains parser procedure metadata or SQL")

    transfer_guard = section(
        worker,
        "// FIREBIRD_CORE_0076_PREPARED_METADATA_TRANSFER_GUARD_BEGIN",
        "// FIREBIRD_CORE_0076_PREPARED_METADATA_TRANSFER_GUARD_END",
    )
    for token in (
        "kInvokeLiteralIntegerPair",
        "prepared_statement_uuid",
        "prepare_transaction_selector.present()",
        "prepared_metadata_transfer_v1_negotiated",
    ):
        require(token in transfer_guard,
                f"bounded transferable prepared guard missing: {token}")
    require("kCreateOrAlter" not in transfer_guard,
            "routine CREATE was incorrectly made transferable")

    transfer_execute = section(
        worker,
        "// FIREBIRD_CORE_0076_TRANSFERRED_EXECUTE_ROUTE_BEGIN",
        "// FIREBIRD_CORE_0076_TRANSFERRED_EXECUTE_ROUTE_END",
    )
    for token in (
        "prepared_selector_changed",
        "transferable_prepared_metadata",
        "retained_opaque_prepare_uuid_no_reprepare",
    ):
        require(token in transfer_execute,
                f"transferred execute retention guard missing: {token}")
    for forbidden in (
        "PrepareFirebirdStatementOnExactRoute",
        "CloseFirebirdStatementPreparedRoute",
        "prepared_statement_uuid.clear",
    ):
        require(forbidden not in transfer_execute,
                f"transferred execute route reparses/rebinds opaque metadata: {forbidden}")

    retire_route = section(
        worker,
        "void RetireFirebirdStatementRoutesForFinalizedSelector(",
        "void RetireAllFirebirdPreparedAndCursorRoutes(",
    )
    for token in (
        "FirebirdStatementUsesPreparedMetadataTransferV1(*state, handle)",
        "retained_opaque_prepared_uuid",
    ):
        require(token in retire_route,
                f"D__trans finality incorrectly stales transferable prepare: {token}")

    execute_route = section(
        worker,
        "FirebirdPipelineResult ExecuteFirebirdPreparedStatementOnExactRoute(",
        "std::optional<std::string> SelectFirebirdExecutionTransaction(",
    )
    require("!transferable_prepared_metadata" in execute_route,
            "prepared execute does not admit negotiated cross-selector transfer")
    require(
        "(prepared_selector_changed && !transferable_prepared_metadata)" in
        prepared_route,
        "non-transferable prepared statements lost strict execute-selector rebind",
    )

    require("(!engine_routine_prepare &&" in worker,
            "recognized routine create can still enter metadata-only fallback")
    require(
        "bounded_procedure_route.kind !=\n"
        "              FirebirdBoundedProcedureRouteKind::kInvokeLiteralIntegerPair" in worker,
        "literal procedure invocation can still enter parser-local execution",
    )
    require("text_resource_storage=large_object" in execution,
            "text BLOB large-object descriptor marker is missing")
    require("column.text_large_object ? std::string_view(\"BLOB\")" in execution,
            "text BLOB descriptor does not use the neutral BLOB base type")

    combined = header + execution + client_types + client_header + client_source
    for forbidden in (
        "statement_metadata_snapshot_policy",
        "prepared_metadata_snapshot_uuid",
        "prepared_metadata_version",
        "prepared_metadata_transfer_token",
    ):
        require(forbidden not in combined,
                f"parser/client received forbidden metadata authority: {forbidden}")
    for forbidden in (
        "sbsql_worker",
        "compatibility/postgresql",
        "compatibility/mysql",
        "ParseStatementWithOtherDialect",
    ):
        require(forbidden not in combined,
                f"standalone Firebird parser depends on another parser: {forbidden}")

    print("firebird CORE-0076 routine authority gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
