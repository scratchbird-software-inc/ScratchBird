#!/usr/bin/env python3
"""Static authority/isolation gate for direct and persisted Firebird AVG."""

from __future__ import annotations

import argparse
from pathlib import Path


CANONICAL_AVG_UUID = "019de5fc-2400-78ac-b50c-45b832831004"
RETIRED_AVG_SURFACE_UUID = "019dffbb-f000-7fd3-b228-03bf40871b10"
LEGACY_AVG_SURFACE_UUID = "019dffbb-f000-710f-9410-919aad901ae2"
TRANSPORT_MARKER = "sblr.global_aggregate_projection.v1"
VIEW_TRANSPORT_MARKER = "engine.global_aggregate_view.v1"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def section(text: str, begin: str, end: str) -> str:
    start = text.find(begin)
    finish = text.find(end, start + len(begin))
    if start < 0 or finish < 0:
        raise AssertionError(f"missing guarded section {begin}..{end}")
    return text[start : finish + len(end)]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--parser-header", type=Path, required=True)
    parser.add_argument("--parser-source", type=Path, required=True)
    parser.add_argument("--execution-source", type=Path, required=True)
    parser.add_argument("--worker-source", type=Path, required=True)
    parser.add_argument("--engine-header", type=Path, required=True)
    parser.add_argument("--engine-source", type=Path, required=True)
    parser.add_argument("--view-engine-header", type=Path, required=True)
    parser.add_argument("--view-engine-source", type=Path, required=True)
    parser.add_argument("--dispatch-source", type=Path, required=True)
    parser.add_argument("--select-source", type=Path, required=True)
    parser.add_argument("--server-source", type=Path, required=True)
    parser.add_argument("--ipc-server-source", type=Path, required=True)
    args = parser.parse_args()

    parser_header = read(args.parser_header)
    parser_source = read(args.parser_source)
    execution_source = read(args.execution_source)
    worker_source = read(args.worker_source)
    engine_header = read(args.engine_header)
    engine_source = read(args.engine_source)
    view_engine_header = read(args.view_engine_header)
    view_engine_source = read(args.view_engine_source)
    dispatch_source = read(args.dispatch_source)
    select_source = read(args.select_source)
    server_source = read(args.server_source)
    ipc_server_source = read(args.ipc_server_source)

    scoped = "\n".join(
        [parser_header, parser_source, engine_header, engine_source]
    )
    require(CANONICAL_AVG_UUID in parser_header,
            "Firebird parser does not publish canonical AVG UUID")
    require(CANONICAL_AVG_UUID in engine_source,
            "neutral engine does not validate canonical AVG UUID")
    require(RETIRED_AVG_SURFACE_UUID not in scoped,
            "retired AVG surface UUID leaked into production ABI")
    require(LEGACY_AVG_SURFACE_UUID not in scoped,
            "legacy AVG surface UUID leaked into production ABI")

    parser_lower = parser_source.lower()
    require("sbsql" not in parser_lower,
            "standalone Firebird AVG parser depends on SBsql")
    for sibling in (
        "postgresql", "mysql", "mariadb", "oracle", "sqlite", "cockroach"
    ):
        require(sibling not in parser_lower,
                f"standalone Firebird AVG parser depends on {sibling}")

    for token in (
        "ParseFirebirdGlobalAvgProjectionRoute",
        "ResolveRelationDescriptorPublicOnTransaction",
        "BindFirebirdGlobalAvgProjection",
        "EncodeFirebirdGlobalAvgProjectionEnvelope",
        "FIREBIRD.AGGREGATE.SHAPE_UNSUPPORTED",
    ):
        require(token in execution_source,
                f"Firebird exact AVG binder/lowerer seam missing {token}")

    for token in (
        "kAvgField = 4",
        "kAvgDistinctField = 5",
        "FirebirdGlobalAvgResultKind",
        "kNullableInt64",
        "kNullableReal64",
    ):
        require(token in parser_header,
                f"Firebird AVG parser contract missing {token}")
    for token in (
        "gag1|",
        r'\"projection_count\":\"1\"',
        r'\"contains_sql_text\":false',
        "canonical=int64;precision=64;scale=0;nullable=true",
        "canonical=real64;precision=64;nullable=true",
    ):
        require(token in parser_source,
                f"Firebird AVG neutral lowering missing {token}")

    transport = section(
        dispatch_source,
        "SB_ENGINE_GLOBAL_AGGREGATE_PROJECTION_TRANSPORT_V1_BEGIN",
        "SB_ENGINE_GLOBAL_AGGREGATE_PROJECTION_TRANSPORT_V1_END",
    )
    for token in (
        TRANSPORT_MARKER,
        "SetInvalidGlobalAggregateProjectionTransport",
        "ReadExactSingleTransportOption",
        "EngineGlobalAggregateAvgFunctionUuid",
        "avg_field",
        "avg_distinct_field",
        "function_uuid != aggregate_function",
        "index >= output_count",
        "packed_outputs",
        "text.size() > 1 && text.front() == '0'",
    ):
        require(token in transport,
                f"neutral AVG transport guard missing {token}")
    for dialect in ("firebird", "sbsql", "postgresql", "mysql", "oracle"):
        require(dialect not in transport.lower(),
                f"neutral aggregate transport depends on {dialect}")

    worker = section(
        worker_source,
        "FIREBIRD_GLOBAL_AVG_PROJECTION_WORKER_BEGIN",
        "FIREBIRD_GLOBAL_AVG_PROJECTION_WORKER_END",
    )
    for token in (
        "? 580",
        ": 480",
        "descriptor.length = 8",
        "descriptor.nullable = true",
        "global_aggregate_projection_rowset",
        "one_mga_visible_scan",
        "std::isfinite",
        "rows->clear()",
    ):
        require(token in worker,
                f"AVG worker presentation guard missing {token}")
    for forbidden in (
        "VisibleCrudRows",
        "ExecuteGlobalAggregateProjection",
        "EngineSelectRows",
        "CommitTransaction",
        "RollbackTransaction",
        "parser_local",
    ):
        require(forbidden not in worker,
                f"AVG worker acquired forbidden authority: {forbidden}")

    for token in (
        "avg_field = 4",
        "avg_distinct_field = 5",
        "aggregate_function_uuid",
        "result_descriptor",
        "EngineGlobalAggregateAvgIntegerResultDescriptor",
        "EngineGlobalAggregateAvgRealResultDescriptor",
    ):
        require(token in engine_header,
                f"neutral AVG ABI missing {token}")
    for token in (
        "BindGlobalAggregateProjectionEnvelope",
        "AdmittedAvgInputKind",
        "CanonicalIntegerValue",
        "CanonicalReal64Value",
        "CheckedAddInteger",
        "__int128_t",
        "double real_sum = 0.0",
        "const double next = state.real_sum + real_value",
        "state.real_sum / static_cast<double>(state.avg_value_count)",
        "std::isfinite",
        "avg_value_count == 0",
        "canonical_distinct_keys",
        "global_aggregate_function_uuid_mixed",
        "global_aggregate_operation_family_mixed",
    ):
        require(token in engine_source,
                f"neutral engine AVG authority guard missing {token}")
    require("long double" not in engine_source,
            "neutral engine AVG uses a widened non-Firebird accumulator")

    for token in (
        "VisibleCrudRowsForContext",
        "ExecuteGlobalAggregateProjection",
        "one_mga_visible_scan",
        "global_aggregate_function_uuid",
        "global_aggregate_binding.outputs.front()",
    ):
        require(token in select_source,
                f"one-MGA-scan AVG route missing {token}")
    require(TRANSPORT_MARKER not in select_source,
            "EngineSelectRows depends on aggregate transport spelling")
    require(CANONICAL_AVG_UUID not in server_source,
            "server contains compatibility AVG identity/branching")
    require(TRANSPORT_MARKER not in server_source,
            "server contains aggregate transport decoder logic")

    # Persisted AVG view parsing is owned entirely by the standalone Firebird
    # parser.  The lowered gavc1/gavs1 packets carry only bound identities,
    # descriptors, and semantic evidence; never source SQL or a result value.
    for token in (
        "kFirebirdGlobalAggregateViewMarkerV1",
        "FirebirdGlobalAggregateViewCreateRoute",
        "FirebirdGlobalAggregateViewSelectRoute",
        "ParseFirebirdGlobalAggregateViewCreateRoute",
        "BindFirebirdGlobalAggregateViewCreate",
        "EncodeFirebirdGlobalAggregateViewCreateEnvelope",
        "ParseFirebirdGlobalAggregateViewSelectRoute",
        "BindFirebirdGlobalAggregateViewSelect",
        "EncodeFirebirdGlobalAggregateViewSelectEnvelope",
    ):
        require(token in parser_header,
                f"persisted AVG view parser contract missing {token}")
    for token in (
        'packed << "gavc1|"',
        'parts[0] != "gavs1"',
        r'\"contains_sql_text\":false',
        r'\"view_projection_count\":\"1\"',
        r'\"operation_family\":\"sblr.catalog.mutation.v3\"',
        r'\"projection_count\":\"1\"',
        "relation_descriptor_generation",
        "view_descriptor_generation",
        "result_alias",
        "FirebirdInt32Descriptor",
        'type == "INTEGER" || type == "INT" || type == "INT32"',
    ):
        require(token in parser_source,
                f"persisted AVG view SQL-free lowering missing {token}")
    require(r'\"sql_text\":' not in parser_source[
                parser_source.find("EncodeFirebirdGlobalAggregateViewCreateEnvelope"):
                parser_source.find("FirebirdGlobalCountProjectionOperationName")
            ], "persisted AVG view envelope leaked source SQL")
    require("sblr.ddl.schema.v3" not in parser_source,
            "persisted AVG view used an unregistered neutral operation family")

    view_transport = section(
        dispatch_source,
        "SB_ENGINE_GLOBAL_AGGREGATE_VIEW_TRANSPORT_V1_BEGIN",
        "SB_ENGINE_GLOBAL_AGGREGATE_VIEW_TRANSPORT_V1_END",
    )
    for token in (
        VIEW_TRANSPORT_MARKER,
        'kGlobalAggregateViewCreatePacketV1 = "gavc1"',
        'kGlobalAggregateViewSelectPacketV1 = "gavs1"',
        'marker_family =\n      "engine.global_aggregate_view"',
        "GlobalAggregateViewBaseDataEmpty",
        "GlobalAggregateViewOptionsAdmitted",
        "parts.size() != 23",
        "parts.size() != 11",
        "descriptor_generation == 0",
        "view_descriptor_generation=",
        ";result_alias=",
        "projection_encoded_descriptor != expected_semantic_descriptor",
        "base.bound_object_identity.object_uuid.canonical.empty()",
        "base.native_row_packet.packet_bytes.empty()",
        "base.rows.empty() && base.assignments.empty()",
    ):
        require(token in view_transport,
                f"neutral persisted-view transport guard missing {token}")
    for dialect in ("firebird", "sbsql", "postgresql", "mysql", "oracle"):
        require(dialect not in view_transport.lower(),
                f"neutral persisted-view transport depends on {dialect}")

    for token in (
        "kEngineGlobalAggregateViewMarkerV1",
        "EngineGlobalAggregateViewDescriptor",
        "PrepareEngineGlobalAggregateViewCreate",
        "EngineGlobalAggregateViewSemanticDescriptor",
        "ExpandEngineGlobalAggregateViewSelect",
    ):
        require(token in view_engine_header,
                f"neutral persisted-view engine ABI missing {token}")
    for token in (
        "PrepareEngineGlobalAggregateViewCreate",
        "DescribeEngineGlobalAggregateView",
        "EngineGlobalAggregateViewSemanticDescriptor",
        "view_descriptor_generation=",
        ";result_alias=",
        "global_aggregate_view_descriptor_stale",
        "global_aggregate_view_source_descriptor_stale",
        "LoadMgaRelationStorageDescriptor",
        "CanonicalTypeIdFromStableName",
        "CanonicalTypeId::int32",
    ):
        require(token in view_engine_source,
                f"neutral persisted-view engine authority missing {token}")
    require(
        engine_source.count("CanonicalTypeIdFromStableName") >= 4,
        "neutral AVG expression path bypasses canonical datatype authority",
    )

    for token in (
        'code != "PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE"',
        "ApplyFirebirdOrdinaryRelationSelectFallback",
        "result->global_aggregate_view_select_route = {}",
        "result->global_aggregate_view_result_alias.clear()",
        "&result, SelectEnvelope(*select, *source_uuid)",
    ):
        require(token in execution_source,
                f"ordinary table SELECT fallback guard missing {token}")

    for token in (
        "!global_aggregate_view_create_route.attempted",
        "engine_global_aggregate_view_create_route = {}",
        "engine_global_aggregate_view_select_route = {}",
        "ResolveNameSemanticPublicOnTransaction",
        "BindFirebirdGlobalAggregateViewSelect",
        "FIREBIRD.AGGREGATE_VIEW.ENGINE_OBJECT_UUID_REQUIRED",
        "FIREBIRD.AGGREGATE_VIEW.SELECTED_TRANSACTION_MISMATCH",
        "FirebirdServerResultObjectUuid",
        "parser_overlay=none",
        "ValidateFirebirdGlobalAvgProjectionCompletePacket",
        "nullable_sql_int64=true",
    ):
        require(token in worker_source,
                f"persisted AVG view worker guard missing {token}")
    require("RecordFirebirdCreateView(&state, sql_text)" in worker_source,
            "legacy metadata overlay seam unexpectedly disappeared")
    require(worker_source.find("!global_aggregate_view_create_route.attempted") <
            worker_source.find("RecordFirebirdCreateView(&state, sql_text)"),
            "persisted AVG view no longer preempts the legacy view overlay")

    semantic_bridge = section(
        ipc_server_source,
        "SB_SERVER_GLOBAL_AGGREGATE_VIEW_SEMANTIC_DETAIL_V1_BEGIN",
        "SB_SERVER_GLOBAL_AGGREGATE_VIEW_SEMANTIC_DETAIL_V1_END",
    )
    for token in (
        "EncodeGlobalAggregateViewSemanticDetail",
        'packed << "gavs1|"',
        "descriptor_generation == 0",
        "projection.result_alias.empty()",
        "EngineGlobalAggregateAvgIntegerResultDescriptor",
    ):
        require(token in semantic_bridge,
                f"generic semantic bridge missing persisted-view guard {token}")
    for dialect in ("firebird", "sbsql", "postgresql", "mysql", "oracle"):
        require(dialect not in semantic_bridge.lower(),
                f"generic semantic bridge depends on {dialect}")

    for token in (
        "bounded_projection_count",
        "parsed_count > (16u - (ch - '0')) / 10u",
        "parsed_count <= 16u",
        "projection < *bounded_projection_count",
        "noncanonical, or oversized counts deliberately omit projection operands",
    ):
        require(token in server_source,
                f"generic CREATE VIEW bridge count bound missing {token}")

    print("firebird_avg_aggregate_authority_gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
