#!/usr/bin/env python3
"""Static authority/isolation gate for the exact Firebird COUNT tranche."""

from __future__ import annotations

import argparse
from pathlib import Path


CANONICAL_COUNT_UUID = "019de5fc-2400-784a-9aec-371f8b95b7ea"
RETIRED_CONFLICTING_COUNT_UUID = "019dffbb-f000-7613-a71e-84b03ef18e1d"
LEGACY_AGGREGATE_COUNT_SURFACE_UUID = "019dffbb-f000-7293-b215-aa84d8693576"
TRANSPORT_MARKER = "sblr.global_aggregate_projection.v1"


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
    parser.add_argument("--dispatch-source", type=Path, required=True)
    parser.add_argument("--select-source", type=Path, required=True)
    parser.add_argument("--server-source", type=Path, required=True)
    args = parser.parse_args()

    parser_header = read(args.parser_header)
    parser_source = read(args.parser_source)
    execution_source = read(args.execution_source)
    worker_source = read(args.worker_source)
    engine_header = read(args.engine_header)
    engine_source = read(args.engine_source)
    dispatch_source = read(args.dispatch_source)
    select_source = read(args.select_source)
    server_source = read(args.server_source)

    scoped = "\n".join(
        [parser_header, parser_source, engine_header, engine_source]
    )
    require(CANONICAL_COUNT_UUID in parser_header,
            "Firebird parser does not publish canonical aggregate COUNT UUID")
    require(CANONICAL_COUNT_UUID in engine_source,
            "neutral engine does not validate canonical aggregate COUNT UUID")
    require(RETIRED_CONFLICTING_COUNT_UUID not in scoped,
            "retired conflicting count UUID leaked into the new ABI")
    require(LEGACY_AGGREGATE_COUNT_SURFACE_UUID not in scoped,
            "legacy sb.aggregate.count surface UUID leaked into the new ABI")
    require("sbsql" not in parser_source.lower(),
            "standalone Firebird COUNT parser depends on SBsql")
    for sibling in ("postgresql", "mysql", "mariadb", "oracle", "sqlite"):
        require(sibling not in parser_source.lower(),
                f"standalone Firebird COUNT parser depends on {sibling}")

    for token in (
        "ResolveRelationDescriptorPublicOnTransaction",
        "BindFirebirdGlobalCountProjection",
        "EncodeFirebirdGlobalCountProjectionEnvelope",
        "FIREBIRD.AGGREGATE.SHAPE_UNSUPPORTED",
    ):
        require(token in execution_source,
                f"Firebird exact binder/lowerer seam missing {token}")
    require("select count(*) from test" not in parser_source.lower(),
            "parser source contains regression SQL text")

    transport = section(
        dispatch_source,
        "SB_ENGINE_GLOBAL_AGGREGATE_PROJECTION_TRANSPORT_V1_BEGIN",
        "SB_ENGINE_GLOBAL_AGGREGATE_PROJECTION_TRANSPORT_V1_END",
    )
    require(TRANSPORT_MARKER in transport,
            "neutral dispatch transport marker is missing")
    require("SetInvalidGlobalAggregateProjectionTransport" in transport,
            "malformed neutral aggregate transport does not fail closed")
    for token in (
        "result_projection_count != 1",
        "exact_marker_count != 1",
        "ReadExactSingleTransportOption",
        'base, "aggregate_function:"',
        'base, "projection_count:"',
        "packed_outputs",
        "index >= output_count",
        "typed->option_envelopes.erase",
    ):
        require(token in transport,
                f"neutral aggregate transport exact-one guard missing {token}")
    for dialect in ("firebird", "sbsql", "postgresql", "mysql", "oracle"):
        require(dialect not in transport.lower(),
                f"neutral engine transport depends on {dialect}")
    require(TRANSPORT_MARKER not in select_source,
            "EngineSelectRows depends on a transport marker spelling")

    worker = section(
        worker_source,
        "FIREBIRD_GLOBAL_COUNT_PROJECTION_WORKER_BEGIN",
        "FIREBIRD_GLOBAL_COUNT_PROJECTION_WORKER_END",
    )
    for token in (
        "sql_type = 580",
        "descriptor.length = 8",
        "descriptor.scale = 0",
        "descriptor.subtype = 0",
        "descriptor.nullable = false",
        "global_aggregate_projection_rowset",
        "one_mga_visible_scan",
        "rows->clear()",
    ):
        require(token in worker, f"worker presentation guard missing {token}")
    for forbidden in (
        "VisibleCrudRows",
        "ExecuteGlobalAggregateProjection",
        "EngineSelectRows",
        "CommitTransaction",
        "RollbackTransaction",
        "parser_local",
    ):
        require(forbidden not in worker,
                f"worker presentation seam acquired forbidden authority: {forbidden}")

    for token in (
        "VisibleCrudRowsForContext",
        "ExecuteGlobalAggregateProjection",
        "one_mga_visible_scan",
        "global_aggregate_function_uuid",
    ):
        require(token in select_source,
                f"engine one-MGA-scan route missing {token}")
    require("BindGlobalAggregateProjectionEnvelope" in engine_source,
            "engine does not revalidate the bound relation/field descriptor")
    require("EngineGlobalAggregateFieldBinding source_field" in engine_header,
            "bound aggregate drops the exact field UUID/descriptor")
    for token in (
        "StoredFieldValueExact",
        "CanonicalIntegerDistinctKey",
        "AdmittedIntegerDistinctType",
        "dt::CanonicalTypeIdFromStableName",
        "dt::CanonicalTypeId::int32",
        "dt::CanonicalTypeId::int64",
        "dt::CanonicalTypeName",
        "dt::CastDatatypeValue",
        "bound_global_aggregate_source_field_descriptor_mismatch",
        "global_aggregate_source_field_duplicate_in_visible_row",
        "canonical_distinct_keys",
    ):
        require(token in engine_source,
                f"strict typed aggregate execution guard missing {token}")
    require("EqualsAsciiInsensitive" not in engine_source,
            "aggregate execution retains a case-folded field-name fallback")
    require(TRANSPORT_MARKER not in server_source,
            "server contains dialect/global aggregate decoder logic")

    print("firebird_count_aggregate_authority_gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
