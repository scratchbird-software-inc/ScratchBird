#!/usr/bin/env python3
"""Validate the Core authority and isolated implementation for typed results."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

import yaml


SPEC_PATHS = (
    "chapters/wire-ipc/native-wire/appendix-native-type-parameter-result-metadata-layout.md",
    "chapters/wire-ipc/parser-server/appendix-parser-server-ipc-adapter.md",
    "chapters/wire-ipc/parser-server/appendix-sbps-execution-event-payload-binary-layout.md",
)
SUPPORT_PATHS = (
    "registries/result-shape-registry.yaml",
    "registries/datatype-type-codec-identity-registry.yaml",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace-root", type=Path, required=True)
    return parser.parse_args()


def require_contains(
    failures: list[str], path: Path, text: str, required: tuple[str, ...]
) -> None:
    for needle in required:
        if needle not in text:
            failures.append(f"{path}: missing {needle!r}")


def main() -> int:
    args = parse_args()
    workspace = args.workspace_root.resolve()
    specifications = workspace / "Specifications" / "Core"
    repository = workspace / "ScratchBird"
    failures: list[str] = []

    manifest_path = specifications / "MANIFEST.yaml"
    try:
        manifest = manifest_path.read_text(encoding="utf-8")
    except OSError as error:
        print(f"typed result authority validation failed: {error}", file=sys.stderr)
        return 1

    try:
        parsed_manifest = yaml.safe_load(manifest)
    except yaml.YAMLError as error:
        failures.append(f"{manifest_path}: YAML parse failed: {error}")
        parsed_manifest = {}
    authority_files = parsed_manifest.get("authority_files", [])
    if not isinstance(authority_files, list):
        failures.append(f"{manifest_path}: authority_files is not a list")
        authority_files = []
    for relative in SPEC_PATHS + SUPPORT_PATHS:
        occurrences = authority_files.count(relative)
        if occurrences != 1:
            failures.append(
                f"{manifest_path}: expected one direct member {relative}, "
                f"found {occurrences}"
            )
        resolved = (specifications / relative).resolve()
        if specifications not in resolved.parents or not resolved.is_file():
            failures.append(f"{manifest_path}: invalid authority path {relative}")

    native_path = specifications / SPEC_PATHS[0]
    adapter_path = specifications / SPEC_PATHS[1]
    sbps_path = specifications / SPEC_PATHS[2]
    native = native_path.read_text(encoding="utf-8")
    adapter = adapter_path.read_text(encoding="utf-8")
    sbps = sbps_path.read_text(encoding="utf-8")

    require_contains(
        failures,
        native_path,
        native,
        (
            "PS-RESULT-TRANSPORT-BINARY-V1",
            "ASCII `SBTRDS01`",
            "exactly 128 bytes",
            "descriptor_evidence_sha256",
            'ScratchBird.PsResultDescriptorVector.V1',
            "ASCII `SBTRBT01`",
            "exactly 224 bytes",
            "batch_evidence_sha256",
            'ScratchBird.PsRowDataPacket.V1',
            "cursor_bound=0",
            "cursor_bound=1",
            "SB_ENGINE_STATUS_INVALID_ARGUMENT",
            "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
            "PARSER_SERVER_IPC.SEQUENCE_INVALID",
            "DatatypeBinaryValueV1",
            "name_occurrence",
            "datatype_registry_generation",
            "query_execute_result_v1",
            "registries/result-shape-registry.yaml",
            "sb_engine_result_next_typed_batch_v2",
            "SBPS v1 defines no result-packet HMAC key",
            "QUERY-EXECUTE-PRODUCER-CURSOR-CARRIER-V1",
            "TypedResultProducerCursorCarrierV1",
            "TypedResultProducerPullRequestV1",
            "OpenTypedResultProducerCursorV1",
            "PullTypedResultProducerCursorV1",
            "CloseTypedResultProducerCursorV1",
            "snapshot pin, cancellation receipt, resource grant, producer state",
            "may replay durable cleanup evidence",
            "callback and its in-memory state never recover",
            "RESOURCE.BUDGET_EXCEEDED",
            "CURSOR.FETCH_FAILED",
        ),
    )
    require_contains(
        failures,
        sbps_path,
        sbps,
        (
            "SBPS-PS-EXECUTE-RESULT-TYPED-COMPONENT-BINDING-V1",
            "| 1043 | `ps_execute_result_v1` |",
            "| 1045 | `ps_fetch_result_v1` |",
            "`7 result_descriptor_vector:blob`",
            "`8 row_data_packet:blob`",
            "`3 row_data_packet:blob`",
            "`11 cursor_stream_descriptor_uuid:uuid`",
            "`16 execution_uuid:uuid`",
            "`17 result_set_uuid:uuid`",
            "`18 row_descriptor_uuid:uuid`",
            "`19 snapshot_uuid:uuid`",
            "`7 cursor_stream_descriptor_uuid:uuid`",
            "ResultDescriptorVectorV1",
            "RowDataPacketV1",
            "derive a session MAC key",
            "`row_batch` | zero",
            "`cursor_bound=0`",
            "row_count=0",
        ),
    )
    require_contains(
        failures,
        adapter_path,
        adapter,
        (
            "PARSER-SERVER-TYPED-RESULT-CURSOR-CLOSURE-V1",
            "PS-RESULT-TRANSPORT-BINARY-V1",
            "MUST NOT derive, transmit, cache, rotate, or",
            "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
            "DATATYPE.DESCRIPTOR_INVALID",
            "PARSER_SERVER_IPC.SEQUENCE_INVALID",
            "exact immutable descriptor and batch byte views",
            "QUERY-EXECUTE-CURSOR-STREAM-DESCRIPTOR-V1",
        ),
    )

    result_registry_path = specifications / SUPPORT_PATHS[0]
    datatype_registry_path = specifications / SUPPORT_PATHS[1]
    result_registry = result_registry_path.read_text(encoding="utf-8")
    datatype_registry = datatype_registry_path.read_text(encoding="utf-8")
    require_contains(
        failures,
        result_registry_path,
        result_registry,
        (
            "QUERY-EXECUTE-CURSOR-STREAM-DESCRIPTOR-V1",
            "QUERY-EXECUTE-RESULT-HANDLE-ROW-STREAM-SEPARATION-V1",
            "stream_descriptor_uuid",
            "descriptor_generation",
            "execution_uuid",
            "result_set_uuid",
            "row_descriptor_uuid",
            "snapshot_uuid",
            "QUERY-EXECUTE-PRODUCER-CURSOR-CARRIER-V1",
            "statement_receipt_uuid",
            "statement_snapshot_uuid",
            "cancellation_receipt_uuid",
            "resource_grant_receipt_uuid",
            "next_batch_ordinal",
            "aggregate_result_vector_as_cursor_source",
            "callback_recovery_or_plan_reexecution",
        ),
    )
    require_contains(
        failures,
        datatype_registry_path,
        datatype_registry,
        (
            "DATATYPE-TYPE-CODEC-IDENTITY-REGISTRY-V1",
            "catalog_generation",
            "registry_generation",
            "descriptor_generation",
            "type_generation",
            "codec_generation",
            "canonical_value_bytes",
            "DATATYPE.DESCRIPTOR_INVALID",
        ),
    )
    try:
        parsed_datatype_registry = yaml.safe_load(datatype_registry)[
            "datatype_type_codec_identity_registry"
        ]
    except (KeyError, TypeError, yaml.YAMLError) as error:
        failures.append(
            f"{datatype_registry_path}: datatype registry parse failed: "
            f"{error}"
        )
        parsed_datatype_registry = {}

    expected_text_row = {
        "row_id": "datatype.text.v1",
        "canonical_name": "text",
        "descriptor_uuid": "019d0000-0000-7000-8000-00000000d718",
        "descriptor_generation": 1,
        "type_uuid": "019d0000-0000-7000-8000-00000000d719",
        "type_generation": 1,
        "codec_uuid": "019d0000-0000-7000-8000-00000000d71a",
        "codec_id": "datatype.text.utf8.v1",
        "codec_version": 1,
        "codec_generation": 1,
        "canonical_value_bytes": {
            "minimum": 0,
            "maximum": 16777216,
            "exact": 0,
            "width": "variable",
            "exact_zero_semantics":
                "descriptor_width_marker_not_payload_length",
            "byte_order": "byte_sequence",
            "signed": False,
            "representation":
                "exact_well_formed_UTF8_scalar_sequence_without_implicit_"
                "normalization",
        },
        "text_semantics": {
            "canonical_family": "text",
            "charset": "UTF-8",
            "charset_rule":
                "exact_well_formed_shortest_form_UTF8_Unicode_scalar_values_"
                "and_no_charset_inference_from_name_or_payload",
            "length_units":
                "byte_length_and_Unicode_scalar_value_length_are_distinct",
            "length_bytes":
                "0_to_16777216_under_the_live_operation_and_resource_ceiling",
            "length_chars":
                "descriptor_bound_or_unbounded_when_the_descriptor_field_is_"
                "null",
            "malformed_sequence": "reject_with_CTB.TEXT.INVALID_ENCODING",
            "normalization":
                "exact_descriptor_bound_normalization_policy_and_resource_"
                "epoch_required_when_the_operation_requires_normalization_"
                "with_no_implicit_default",
            "collation":
                "exact_descriptor_bound_collation_UUID_generation_and_"
                "resource_epoch_required_for_equality_ordering_grouping_"
                "hashing_or_indexing_with_no_codec_name_byte_or_host_locale_"
                "fallback",
            "padding":
                "none_in_the_canonical_type_and_only_an_explicit_domain_or_"
                "compatibility_profile_may_add_padding",
            "empty_value":
                "admitted_value_present_with_zero_payload_bytes_and_distinct_"
                "from_SQL_NULL",
            "null_state":
                "SQL_NULL_only_in_the_containing_slot_null_state_with_zero_"
                "payload_bytes",
            "storage": "variable_width_inline_or_overflow_without_truncation",
            "operation_bound":
                "minimum_of_16777216_live_resource_grant_and_the_calling_"
                "operation_profile_ceiling",
        },
        "null_encoding": "containing_slot_value_or_null_state",
        "visibility": "authenticated_statement_descriptor_projection",
        "lifecycle": "receipt_catalog_snapshot_bound",
    }
    text_rows = [
        row for row in parsed_datatype_registry.get("rows", [])
        if row.get("row_id") == "datatype.text.v1"
    ]
    if text_rows != [expected_text_row]:
        failures.append(
            f"{datatype_registry_path}: canonical text row differs"
        )
    text_identity_values = (
        expected_text_row["descriptor_uuid"],
        expected_text_row["type_uuid"],
        expected_text_row["codec_uuid"],
    )
    all_identity_values = []
    for row in parsed_datatype_registry.get("rows", []):
        all_identity_values.extend(
            value for value in (
                row.get("descriptor_uuid"), row.get("type_uuid"),
                row.get("codec_uuid"),
            ) if value
        )
    allowed_boolean_identity = "01000000-626f-7f6c-a561-6e0000000000"
    collisions = {
        value for value in all_identity_values
        if all_identity_values.count(value) > 1 and
        value != allowed_boolean_identity
    }
    if len(set(text_identity_values)) != 3 or collisions:
        failures.append(
            f"{datatype_registry_path}: text identities are not distinct: "
            f"{sorted(collisions)!r}"
        )
    text_migration = parsed_datatype_registry.get(
        "text_canonical_identity_migration", {}
    )
    if (
        text_migration.get("search_key") !=
            "DATATYPE-TEXT-CANONICAL-IDENTITY-MIGRATION-V1"
        or text_migration.get("provisional_legacy_descriptor_uuid") !=
            "2c010000-6368-7172-a163-746572000000"
        or text_migration.get("provisional_legacy_type_uuid") !=
            "2c010000-6368-7172-a163-746572000000"
        or text_migration.get("canonical_descriptor_uuid") !=
            expected_text_row["descriptor_uuid"]
        or text_migration.get("canonical_type_uuid") !=
            expected_text_row["type_uuid"]
        or text_migration.get("canonical_codec_uuid") !=
            expected_text_row["codec_uuid"]
        or text_migration.get("canonical_codec_id") !=
            expected_text_row["codec_id"]
        or text_migration.get("upgrade", {}).get("owner") !=
            "MGA_catalog_migration"
        or text_migration.get("upgrade", {}).get("seal") !=
            "durable_migration_evidence_and_the_complete_replacement_catalog_"
            "snapshot_are_both_required_before_publication"
        or not {"runtime_alias", "encoded_descriptor_inference",
                "lazy_query_repair"}.issubset(
                    set(text_migration.get("prohibited", [])))
    ):
        failures.append(
            f"{datatype_registry_path}: sealed text migration differs"
        )
    provisional_text = "2c010000-6368-7172-a163-746572000000"
    if any(
        provisional_text in (row.get("descriptor_uuid"), row.get("type_uuid"))
        for row in parsed_datatype_registry.get("rows", [])
    ):
        failures.append(
            f"{datatype_registry_path}: provisional text identity is admitted"
        )

    try:
        parsed_result_registry = yaml.safe_load(result_registry)
        producer_contract = parsed_result_registry["result_shape_registry"][
            "semantic_contracts"
        ]["producer_cursor_carrier_v1"]
    except (KeyError, TypeError, yaml.YAMLError) as error:
        failures.append(
            f"{result_registry_path}: producer cursor contract parse failed: "
            f"{error}"
        )
        producer_contract = {}

    expected_producer_fields = (
        "carrier_uuid",
        "carrier_generation",
        "cursor_uuid",
        "session_uuid",
        "statement_receipt_uuid",
        "statement_snapshot_uuid",
        "cancellation_receipt_uuid",
        "cancellation_generation",
        "resource_grant_receipt_uuid",
        "resource_grant_generation",
        "resource_grant_bytes",
        "execution_uuid",
        "result_set_uuid",
        "row_descriptor_uuid",
        "snapshot_uuid",
        "cursor_stream_descriptor_uuid",
        "cursor_stream_descriptor_version",
        "cursor_stream_descriptor_generation",
        "row_descriptor_generation",
        "descriptor_evidence_sha256",
        "result_descriptor_vector",
        "max_chunk_rows",
        "max_chunk_bytes",
        "next_batch_ordinal",
        "lifecycle_state",
    )
    actual_producer_fields = tuple(
        field.get("name") for field in producer_contract.get("fields", [])
    )
    if actual_producer_fields != expected_producer_fields:
        failures.append(
            f"{result_registry_path}: producer cursor fields differ: "
            f"{actual_producer_fields!r}"
        )
    if producer_contract.get("search_key") != (
        "QUERY-EXECUTE-PRODUCER-CURSOR-CARRIER-V1"
    ):
        failures.append(f"{result_registry_path}: producer search key differs")
    if producer_contract.get("version") != 1:
        failures.append(f"{result_registry_path}: producer version is not 1")
    if producer_contract.get("visibility") != "server_private_nonserializable":
        failures.append(f"{result_registry_path}: producer visibility differs")

    expected_pull_fields = (
        "cursor_uuid",
        "carrier_generation",
        "cursor_stream_descriptor_uuid",
        "cursor_stream_descriptor_version",
        "cursor_stream_descriptor_generation",
        "expected_batch_ordinal",
        "maximum_rows",
        "maximum_bytes",
        "timeout_millis",
    )
    actual_pull_fields = tuple(
        producer_contract.get("pull_request", {}).get("fields_in_order", [])
    )
    if actual_pull_fields != expected_pull_fields:
        failures.append(
            f"{result_registry_path}: producer pull fields differ: "
            f"{actual_pull_fields!r}"
        )

    expected_retained_handles = (
        "statement_context_receipt",
        "MGA_snapshot_pin",
        "cancellation_receipt",
        "resource_grant_receipt",
        "producer_state",
    )
    actual_retained_handles = tuple(
        producer_contract.get("retained_authority", {}).get(
            "required_handles", []
        )
    )
    if actual_retained_handles != expected_retained_handles:
        failures.append(
            f"{result_registry_path}: retained producer authorities differ: "
            f"{actual_retained_handles!r}"
        )

    expected_transitions = {
        "open": ["pulling", "cancelled", "closed", "revoked"],
        "pulling": ["open", "eos", "cancelled", "revoked"],
        "eos": [],
        "cancelled": [],
        "closed": [],
        "revoked": [],
    }
    actual_transitions = producer_contract.get("state_machine", {}).get(
        "allowed_transitions", {}
    )
    if actual_transitions != expected_transitions:
        failures.append(
            f"{result_registry_path}: producer transitions differ: "
            f"{actual_transitions!r}"
        )
    expected_outcomes = {
        "batch",
        "empty_open",
        "empty_eos",
        "cancelled",
        "refused",
    }
    actual_outcomes = set(producer_contract.get("pull_outcomes", {}))
    if actual_outcomes != expected_outcomes:
        failures.append(
            f"{result_registry_path}: producer outcomes differ: "
            f"{sorted(actual_outcomes)!r}"
        )

    expected_refusals = (
        "SB_ENGINE_STATUS_INVALID_ARGUMENT",
        "SECURITY.ACCESS_DENIED",
        "MGA.TRANSACTION.STALE",
        "DATATYPE.DESCRIPTOR_INVALID",
        "CURSOR.STALE",
        "RESOURCE.BUDGET_EXCEEDED",
        "PROCESS.CANCELLED",
        "CURSOR.FETCH_FAILED",
    )
    actual_refusals = tuple(
        row.get("diagnostic")
        for row in producer_contract.get("refusal_precedence", [])
    )
    if actual_refusals != expected_refusals:
        failures.append(
            f"{result_registry_path}: producer refusal precedence differs: "
            f"{actual_refusals!r}"
        )

    header_path = repository / "project/src/wire/typed_result_transport_codec.hpp"
    source_path = repository / "project/src/wire/typed_result_transport_codec.cpp"
    carrier_header_path = (
        repository / "project/src/wire/typed_result_transport_carrier.hpp"
    )
    carrier_source_path = (
        repository / "project/src/wire/typed_result_transport_carrier.cpp"
    )
    test_path = repository / (
        "project/tests/sbsql_sblr_alignment/"
        "typed_result_transport_codec_test.cpp"
    )
    carrier_test_path = repository / (
        "project/tests/sbsql_sblr_alignment/"
        "typed_result_transport_carrier_test.cpp"
    )
    header = header_path.read_text(encoding="utf-8")
    source = source_path.read_text(encoding="utf-8")
    carrier_header = carrier_header_path.read_text(encoding="utf-8")
    carrier_source = carrier_source_path.read_text(encoding="utf-8")
    test = test_path.read_text(encoding="utf-8")
    carrier_test = carrier_test_path.read_text(encoding="utf-8")

    require_contains(
        failures,
        header_path,
        header,
        (
            "kTypedResultRowDescriptorHeaderBytes = 128",
            "kTypedResultBatchHeaderBytes = 224",
            "TypedResultEvidenceHash",
            "TypedResultCarrierBinding",
            "descriptor_evidence_sha256",
            "batch_evidence_sha256",
        ),
    )
    require_contains(
        failures,
        source_path,
        source,
        (
            "{'S', 'B', 'T', 'R', 'D', 'S', '0', '1'}",
            "{'S', 'B', 'T', 'R', 'B', 'T', '0', '1'}",
            '"ScratchBird.PsResultDescriptorVector.V1"',
            '"ScratchBird.PsRowDataPacket.V1"',
            "ComputeSha256Digest",
            "kMaxTransportFrameBytes = 16ull * 1024ull * 1024ull",
            "kMaxColumnCount = 16384",
            "kMaxRowCount = 1048576",
            "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
            "PARSER_SERVER_IPC.SEQUENCE_INVALID",
            "result_set_uuid",
            "cursor_stream_descriptor_version",
            "cursor_stream_descriptor_generation",
        ),
    )
    require_contains(
        failures,
        test_path,
        test,
        (
            "IndependentEvidence",
            "ScratchBird.WrongDomain.V1",
            "field;with=delimiters",
            "EmptyAndNullRemainDistinct",
            "Int128MinimumMaximumRoundTrip",
            "BatchEvidenceShapeAndCursorConsistency",
            "forbidden DatatypeBinaryValue toast flag",
            "outer row count mismatch",
        ),
    )
    require_contains(
        failures,
        carrier_header_path,
        carrier_header,
        (
            "SB-WIRE-TYPED-RESULT-CARRIER-VALIDATION-ANCHOR",
            "TypedResultExecuteOutcome",
            "TypedResultExecuteRequestAuthorityV1",
            "TypedResultDescriptorAuthorityValidator",
            "TypedResultCursorCarrierStateV1",
            "ValidateTypedResultExecuteCarrierV1",
            "ValidateTypedResultFetchCarrierV1",
            "cursor_state is copied",
        ),
    )
    require_contains(
        failures,
        carrier_source_path,
        carrier_source,
        (
            "execute_cursor_open_component_matrix_invalid",
            "execute_row_batch_component_matrix_invalid",
            "datatype_descriptor_authority_validator_required",
            "fetch_cursor_or_stream_descriptor_mismatch",
            "fetch_after_terminal_cursor",
            "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
            "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
            "DATATYPE.DESCRIPTOR_INVALID",
            "PARSER_SERVER_IPC.SEQUENCE_INVALID",
            "TypedResultCarrierKind::ps_execute_result_v1",
            "TypedResultCarrierKind::ps_fetch_result_v1",
        ),
    )
    require_contains(
        failures,
        carrier_test_path,
        carrier_test,
        (
            "ExecuteOutcomeMatrixAndAuthority",
            "CursorFetchSequenceAndAtomicity",
            "absent datatype registry validator",
            "packet matrix was not refused before descriptor decode",
            "skipped cursor batch mutated live sequence state",
            "post-terminal cursor packet was admitted",
            "stream-descriptor generation drift was admitted",
        ),
    )

    forbidden_codec_terms = (
        "ComputeHmacSha256Digest",
        "authentication_key",
        "TypedResultAuthenticationTag",
        "authentication_failed",
        "result_stream_generation",
    )
    for path, text in (
        (header_path, header),
        (source_path, source),
        (carrier_header_path, carrier_header),
        (carrier_source_path, carrier_source),
        (test_path, test),
        (carrier_test_path, carrier_test),
    ):
        for needle in forbidden_codec_terms:
            if needle in text:
                failures.append(f"{path}: forbidden invented-HMAC residue {needle!r}")

    untouched_routes = (
        repository / "project/src/engine/public_abi.cpp",
        repository
        / "project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp",
        repository
        / "project/src/wire/parser_server_ipc/parser_server_client.cpp",
    )
    for route in untouched_routes:
        route_text = route.read_text(encoding="utf-8")
        for isolated_component in (
            "typed_result_transport_codec",
            "typed_result_transport_carrier",
        ):
            if isolated_component in route_text:
                failures.append(
                    f"{route}: isolated {isolated_component} was integrated "
                    "before handoff"
                )

    if failures:
        print("typed result authority validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print(
        "typed result authority validation passed: "
        "manifest=5 descriptor_header=128 batch_header=224 "
        "evidence=sha256 outer_carrier=validated "
        "producer_cursor=validated routes=disjoint"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
