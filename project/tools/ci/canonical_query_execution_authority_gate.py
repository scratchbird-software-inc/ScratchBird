#!/usr/bin/env python3
"""Ratchet canonical-query decomposition without claiming runtime proof."""

from __future__ import annotations

import pathlib
import sys


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[2]
SBLR_ROOT = PROJECT_ROOT / "src" / "engine" / "sblr"
OPTIMIZER_ROOT = PROJECT_ROOT / "src" / "engine" / "optimizer"
OPTIMIZER_CMAKE = OPTIMIZER_ROOT / "CMakeLists.txt"
TEST_ROOT = PROJECT_ROOT / "tests" / "optimizer"
CMAKE = SBLR_ROOT / "CMakeLists.txt"
AUTHORITY = SBLR_ROOT / "CANONICAL_QUERY_EXECUTION_AUTHORITY.md"

MODULES = {
    "canonical_query_execute.cpp": (
        59_207,
        2_868_800,
        "SB_ENGINE_CANONICAL_QUERY_EXECUTE_COORDINATOR_AUTHORITY",
        2,
    ),
    "canonical_query_aggregate_registration.cpp": (
        1_409,
        62_559,
        "SB_ENGINE_CANONICAL_QUERY_AGGREGATE_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_correlated_registration.cpp": (
        893,
        40_698,
        "SB_ENGINE_CANONICAL_QUERY_CORRELATED_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_filter_registration.cpp": (
        383,
        18_148,
        "SB_ENGINE_CANONICAL_QUERY_FILTER_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_join_registration.cpp": (
        857,
        42_577,
        "SB_ENGINE_CANONICAL_QUERY_JOIN_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_object_free_profile.cpp": (
        250,
        11_934,
        "SB_ENGINE_CANONICAL_QUERY_OBJECT_FREE_PROFILE_AUTHORITY",
        2,
    ),
    "canonical_query_physical_registration.cpp": (
        972,
        42_303,
        "SB_ENGINE_CANONICAL_QUERY_PHYSICAL_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_predicate_support.cpp": (
        428,
        18_613,
        "SB_ENGINE_CANONICAL_QUERY_PREDICATE_SUPPORT_AUTHORITY",
        2,
    ),
    "canonical_query_projection_registration.cpp": (
        395,
        18_036,
        "SB_ENGINE_CANONICAL_QUERY_PROJECTION_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_recursive_registration.cpp": (
        971,
        43_851,
        "SB_ENGINE_CANONICAL_QUERY_RECURSIVE_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_relational_registration.cpp": (
        1_891,
        89_163,
        "SB_ENGINE_CANONICAL_QUERY_RELATIONAL_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_set_registration.cpp": (
        279,
        12_709,
        "SB_ENGINE_CANONICAL_QUERY_SET_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_sort_registration.cpp": (
        998,
        45_155,
        "SB_ENGINE_CANONICAL_QUERY_SORT_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_window_registration.cpp": (
        804,
        39_236,
        "SB_ENGINE_CANONICAL_QUERY_WINDOW_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_scalar_support.cpp": (
        182,
        6_757,
        "SB_ENGINE_CANONICAL_QUERY_SCALAR_SUPPORT_AUTHORITY",
        2,
    ),
    "canonical_query_descriptor_support.cpp": (
        150,
        6_049,
        "SB_ENGINE_CANONICAL_QUERY_DESCRIPTOR_SUPPORT_AUTHORITY",
        2,
    ),
    "canonical_query_json_support.cpp": (
        205,
        6_755,
        "SB_ENGINE_CANONICAL_QUERY_JSON_SUPPORT_AUTHORITY",
        2,
    ),
    "canonical_query_runtime_memory_support.cpp": (
        428,
        15_678,
        "SB_ENGINE_CANONICAL_QUERY_RUNTIME_MEMORY_SUPPORT_AUTHORITY",
        2,
    ),
    "canonical_query_time_series_endpoint.cpp": (
        151,
        5_668,
        "SB_ENGINE_CANONICAL_QUERY_TIME_SERIES_ENDPOINT_AUTHORITY",
        1,
    ),
}

PURE_ENDPOINT_FORBIDDEN = (
    "EngineResolveStatementSnapshot",
    "PersistLocalTransactionInventory",
    "FinalizePhysicalMgaCowTransaction",
    "ExecuteCanonicalObjectFreeValuesQuery",
    "ExecuteCanonicalCurrentHeapQuery",
    "mga_relation_store",
    "transaction/transaction_api",
)

SHARED_SUPPORT_FORBIDDEN = (
    "EngineResolveStatementSnapshot",
    "PersistLocalTransactionInventory",
    "FinalizePhysicalMgaCowTransaction",
    "ExecuteCanonicalObjectFreeValuesQuery",
    "ExecuteCanonicalCurrentHeapQuery",
    "mga_relation_store",
    "transaction/transaction_api",
    "parser/",
)

PHYSICAL_REGISTRATION_FORBIDDEN = (
    "PersistLocalTransactionInventory",
    "FinalizePhysicalMgaCowTransaction",
    "EngineCommit",
    "EngineRollback",
    "ExecuteCanonicalObjectFreeValuesQuery",
    "ExecuteCanonicalCurrentHeapQuery",
    "mga_relation_store",
    "parser/",
)


def main() -> int:
    failures: list[str] = []
    cmake_text = CMAKE.read_text(encoding="utf-8")
    if not AUTHORITY.is_file():
        failures.append("missing canonical query execution authority ledger")

    seen_keys: set[str] = set()
    for relative, (
        maximum_lines,
        maximum_bytes,
        search_key,
        cmake_count,
    ) in MODULES.items():
        path = SBLR_ROOT / relative
        if not path.is_file():
            failures.append(f"missing canonical query module: {relative}")
            continue
        raw = path.read_bytes()
        text = raw.decode("utf-8")
        line_count = len(text.splitlines())
        if line_count > maximum_lines:
            failures.append(
                f"{relative}: {line_count} lines exceeds ratchet {maximum_lines}"
            )
        if len(raw) > maximum_bytes:
            failures.append(
                f"{relative}: {len(raw)} bytes exceeds ratchet {maximum_bytes}"
            )
        if text.count(search_key) != 1:
            failures.append(f"{relative}: requires exactly one {search_key}")
        if search_key in seen_keys:
            failures.append(f"duplicate authority search key: {search_key}")
        seen_keys.add(search_key)
        if cmake_text.count(f"  {relative}\n") != cmake_count:
            failures.append(
                f"{relative}: expected {cmake_count} exact CMake enrollment(s)"
            )
        if any(
            line.lstrip().startswith("#include") and ".inc" in line
            for line in text.splitlines()
        ):
            failures.append(f"{relative}: implementation fragments are forbidden")

    coordinator_path = SBLR_ROOT / "canonical_query_execute.cpp"
    endpoint_path = SBLR_ROOT / "canonical_query_time_series_endpoint.cpp"
    if coordinator_path.is_file():
        coordinator = coordinator_path.read_text(encoding="utf-8")
        if (
            '#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)\n'
            '#include "engine/functions/registry/function_seed_registry.hpp"'
            not in coordinator
        ):
            failures.append("production function registry guard drifted")
        if (
            "#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)\n"
            "std::uint32_t CanonicalContextualTextRcp079RuntimeProofMaskForTest()"
            not in coordinator
        ):
            failures.append("production-only contextual proof guard drifted")
        entrypoints = (
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeValuesQuery(",
            "CanonicalObjectFreeValuesExecutionResult "
            "ExecuteCanonicalCurrentHeapQuery(",
        )
        for entrypoint in entrypoints:
            if coordinator.count(entrypoint) != 1:
                failures.append(
                    "canonical_query_execute.cpp: public entrypoint ownership changed"
                )
        for extracted_definition in (
            "TimeSeriesEndpointDaysFromCivil(",
            "bool ParseTimeSeriesEndpointUnsigned(",
            "bool ParseTimeSeriesEndpointNsV1(",
            "bool CanonicalUuidText(",
            "std::string DerivedCanonicalUuid(",
            "bool EncodeCanonicalScalarEqualityKey(",
            "bool DecodeCanonicalInt64Scalar(",
            "CanonicalDocumentWildcardExpansion ExpandCanonicalDocumentWildcard(",
            "bool RuntimeMaterializedBatchMemoryBytes(",
            "bool RuntimeTypedValueMemoryBytes(",
            "void PublishRuntimeMemoryObservation(",
            "bool AddBatchMemoryBytes(",
            "bool BoundDescriptorBatchLiveMemoryBytes(",
            "bool QueryDistinctAuxiliaryMemoryBytes(",
            "bool QueryDistinctOutputMemoryBytes(",
            "bool AddBatchProjectionMemoryBytes(",
            "bool AddBatchRowRangeMemoryBytes(",
            "bool CheckedAdd(",
            "bool CheckedMultiply(",
            "bool LogicalBitVectorPayloadBytes(",
            "std::uint64_t CanonicalUnsignedDecimalWidth(",
            "std::optional<std::size_t> SelectedNodeAggregateMemoryBound(",
            "CanonicalResultNullability ResultNullability(",
            "bool SameExactEngineDescriptorV1(",
            "bool SameExactRelationalTypeDescriptorV2(",
            "bool CanonicalQueryEngineDescriptorExactlyEqual(",
            "bool CanonicalQueryTypedValuePayloadExactlyEqual(",
            "bool CanonicalQueryDescriptorTuplePayloadExactlyEqual(",
            "bool CanonicalQueryDescriptorBatchesExactlyEqual(",
            "std::optional<std::string> ExactEncodedDescriptorField(",
        ):
            if extracted_definition in coordinator:
                failures.append(
                    "canonical_query_execute.cpp: extracted endpoint parser returned"
                )

    if endpoint_path.is_file():
        endpoint = endpoint_path.read_text(encoding="utf-8")
        for token in PURE_ENDPOINT_FORBIDDEN:
            if token in endpoint:
                failures.append(
                    "canonical_query_time_series_endpoint.cpp: forbidden authority "
                    f"token {token!r}"
                )

    for support_name in (
        "canonical_query_scalar_support.cpp",
        "canonical_query_descriptor_support.cpp",
        "canonical_query_json_support.cpp",
        "canonical_query_runtime_memory_support.cpp",
        "canonical_query_object_free_profile.cpp",
        "canonical_query_predicate_support.cpp",
    ):
        support_path = SBLR_ROOT / support_name
        if not support_path.is_file():
            continue
        support = support_path.read_text(encoding="utf-8")
        for token in SHARED_SUPPORT_FORBIDDEN:
            if token in support:
                failures.append(
                    f"{support_name}: forbidden authority token {token!r}"
                )

    for registration_name in (
        "canonical_query_correlated_registration.cpp",
        "canonical_query_aggregate_registration.cpp",
        "canonical_query_filter_registration.cpp",
        "canonical_query_join_registration.cpp",
        "canonical_query_physical_registration.cpp",
        "canonical_query_projection_registration.cpp",
        "canonical_query_recursive_registration.cpp",
        "canonical_query_relational_registration.cpp",
        "canonical_query_set_registration.cpp",
        "canonical_query_sort_registration.cpp",
        "canonical_query_window_registration.cpp",
    ):
        registration_path = SBLR_ROOT / registration_name
        if not registration_path.is_file():
            continue
        registration = registration_path.read_text(encoding="utf-8")
        for token in PHYSICAL_REGISTRATION_FORBIDDEN:
            if token in registration:
                failures.append(
                    f"{registration_name}: forbidden authority token {token!r}"
                )

    if coordinator_path.is_file():
        coordinator = coordinator_path.read_text(encoding="utf-8")
        for extracted_definition in (
            "struct OrdinaryRuntimeMemoryReceipt {",
            "struct LivePhysicalNodeProfile {",
            "struct LivePhysicalPlanningResult {",
            "bool CompleteLiveRuntimeMemoryReceipts(",
            "LivePhysicalPlanningResult PlanAndPublishLivePhysicalDag(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveMaterializedSourceRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveValuesRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveTableSubqueryRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveRowNumberRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveNtileRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveAggregateWindowRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveNavigationWindowRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLivePeerRankingRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveQueryDistinctRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveLimitRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveCountStarRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveSortRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveMatchRecognizeRegistration(",
            "struct PreparedCardinalitySubqueryRoot {",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveCardinalitySubqueryRegistration(",
            "struct PreparedPredicateSubqueryRoot {",
            "bool EvaluateCanonicalQuantifiedSubqueryTruth(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLivePredicateSubqueryRegistration(",
            "struct LiveProjectRuntimeNodeConfiguration {",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveHeapProjectRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveHeapSortRegistration(",
            "struct LiveFilterRuntimeNodeConfiguration {",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveFilterRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveHeapFilterRegistration(",
            "struct LiveJoinPredicateScratchBound {",
            "LiveJoinPredicateScratchBound BoundLiveJoinPredicateScratchBytes(",
            "CanonicalPredicateScratchBound BoundCanonicalPredicateScratchBytes(",
            "struct LiveNonrecursiveCteRuntimeNodeConfiguration {",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveNonrecursiveCteRegistration(",
            "exec::CanonicalExecutionMgaAuthority BuildCanonicalExecutionMgaAuthority(",
            "bool BuildOperatorLocalPhysicalDag(",
            "template <typename ExecutionReceipt>\n"
            "bool CanonicalOperatorExecutionReceiptMatches(",
            "std::optional<std::uint64_t> BoundOperatorLocalPhysicalDagCopyMemoryBytes(",
            "bool RebindOperatorLocalPhysicalMemoryGrant(",
            "bool BuildStrictUnaryOperatorLocalPhysicalDag(",
            "bool BuildStrictBinaryOperatorLocalPhysicalDag(",
            "bool InvokeLiveSortCancellationProbe(",
            "const exec::PhysicalAdmissionEvidence* FindLiveCancellationPolicy(",
            "void BindLiveCancellationFailure(",
            "bool MaterializeExpressionProjectBatch(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveProjectRegistration(",
            "BoundCanonicalCorrelatedComparisonAuthorityV1\n"
            "BindCanonicalCorrelatedComparisonAuthorityV1(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveCorrelatedSubqueryRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveLateralSubqueryRegistration(",
            "bool ValidateLiveSetMemoryReceipt(",
            "bool CanonicalSetOperationExecutionReceiptMatches(",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveSetOperationRegistration(",
            "struct LiveRecursiveCteProfile {",
            "enum class LiveRecursiveCteTermMode : std::uint8_t {",
            "PreparedRecursiveCteTerm PrepareLiveRecursiveCteTerm(",
            "bool LiveRecursiveCteTermNodeBound(",
            "LiveRecursiveCteTermExecution ExecutePreparedRecursiveCteTerm(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveRecursiveCteTermRegistration(",
            "struct PreparedRecursiveCteRoot {",
            "bool BoundPreparedRecursiveCtePeakPayload(",
            "bool BindPreparedRecursiveCtePeakMemory(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveRecursiveCteRegistration(",
            "struct LiveJoinRuntimeNodeConfiguration {",
            "exec::CanonicalPhysicalExecutorRegistration MakeLiveJoinRegistration(",
            "struct PreparedSortExpression {",
            "struct PreparedSortRoot {",
            "bool MaterializeExpressionSortBatch(",
            "class CanonicalDescriptorSortKeyReceiptIssuer {",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveExpressionSortRegistration(",
            "struct PreparedAggregateValueBindingReceipt {",
            "struct PreparedGlobalAggregateRoot {",
            "struct PreparedGroupedCountSumRoot {",
            "bool RevalidatePreparedAggregateValueBindings(",
            "bool BindPreparedGroupedComparisonCeilings(",
            "bool RevalidatePreparedGroupedKeyBindings(",
            "bool MaterializeAggregateFilterTruthValues(",
            "bool BindCanonicalAggregateEqualityTerms(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveAggregateRegistryRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration\n"
            "MakeLiveGroupedCountSumRegistration(",
        ):
            if extracted_definition in coordinator:
                failures.append(
                    "canonical_query_execute.cpp: Stage 3 extracted definition returned"
                )

    planner_path = OPTIMIZER_ROOT / "canonical_relational_dag_planner.cpp"
    relational_planner_path = OPTIMIZER_ROOT / "relational_planner.cpp"
    optimizer_cmake_text = OPTIMIZER_CMAKE.read_text(encoding="utf-8")
    if not planner_path.is_file():
        failures.append("missing focused canonical relational DAG planner")
    else:
        planner = planner_path.read_text(encoding="utf-8")
        if len(planner.splitlines()) > 107 or len(planner.encode()) > 4_395:
            failures.append("canonical relational DAG planner exceeds its ratchet")
        if planner.count(
            "SB_ENGINE_CANONICAL_RELATIONAL_DAG_PLANNER_AUTHORITY"
        ) != 1:
            failures.append("canonical relational DAG planner authority key drifted")
        if planner.count("PlanCanonicalRelationalDag(") != 1:
            failures.append("canonical relational DAG planner ownership drifted")
        if relational_planner_path.read_text(encoding="utf-8").count(
            "PlanCanonicalRelationalDag("
        ):
            failures.append("canonical DAG planner returned to relational_planner.cpp")
        if optimizer_cmake_text.count("canonical_relational_dag_planner.cpp") != 2:
            failures.append("canonical DAG planner CMake enrollment drifted")

    endpoint_test = TEST_ROOT / "canonical_query_time_series_endpoint.cpp"
    if not endpoint_test.is_file():
        failures.append("missing direct time-series endpoint boundary test")
    else:
        endpoint_test_text = endpoint_test.read_text(encoding="utf-8")
        if endpoint_test_text.count(
            "SB_TEST_CANONICAL_QUERY_TIME_SERIES_ENDPOINT_BOUNDARY_MATRIX"
        ) != 1:
            failures.append("time-series endpoint boundary matrix key drifted")
    if cmake_text.count("canonical_query_time_series_endpoint_boundary_v1") != 2:
        failures.append("time-series endpoint boundary test registration drifted")

    shared_support_test = TEST_ROOT / "canonical_query_shared_support.cpp"
    if not shared_support_test.is_file():
        failures.append("missing canonical query shared-support boundary test")
    else:
        shared_support_test_text = shared_support_test.read_text(encoding="utf-8")
        if shared_support_test_text.count(
            "SB_TEST_CANONICAL_QUERY_SHARED_SUPPORT_BOUNDARY_MATRIX"
        ) != 1:
            failures.append("canonical query shared-support matrix key drifted")
    if cmake_text.count("canonical_query_shared_support_boundary_v1") != 2:
        failures.append("canonical query shared-support test registration drifted")

    contract_start = cmake_text.find("add_library(sb_qow_sblr_query_route_contract")
    production_start = cmake_text.find("add_library(sb_engine_sblr")
    if contract_start < 0 or production_start < 0:
        failures.append("canonical query contract/production CMake targets are missing")
    elif "canonical_query_time_series_endpoint.cpp" in cmake_text[
        contract_start:production_start
    ]:
        failures.append(
            "time-series endpoint support leaked into the contract-only target"
        )
    elif "sb_qow_optimizer_admission" not in cmake_text[
        contract_start:production_start
    ]:
        failures.append("contract query route lost reduced optimizer admission")
    elif "sb_engine_optimizer_contract_only" in cmake_text[
        contract_start:production_start
    ]:
        failures.append("contract query route mixed reduced and full optimizer profiles")

    dispatch = (SBLR_ROOT / "sblr_dispatch.cpp").read_text(encoding="utf-8")
    if dispatch.find('#include "uuid.hpp"') > dispatch.find(
        "#ifndef SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY"
    ):
        failures.append("contract dispatch lost its shared UUID dependency")

    contextual_header = (
        PROJECT_ROOT
        / "src/engine/internal_api/query/contextual_text_literal_authority.hpp"
    ).read_text(encoding="utf-8")
    contextual_source = (
        PROJECT_ROOT
        / "src/engine/internal_api/query/contextual_text_literal_authority.cpp"
    ).read_text(encoding="utf-8")
    if "inline scratchbird::engine::sblr::SblrExpressionNodeTableCodecResultV1\nDecodeContextualTextComposedSbxnV2" not in contextual_header:
        failures.append("contextual composed SBXN wire adapter is not header-local")
    if "\nDecodeContextualTextComposedSbxnV2(" in contextual_source:
        failures.append("contextual composed SBXN adapter regained a link dependency")

    if failures:
        for failure in failures:
            print(
                f"canonical_query_execution_authority_gate: FAIL: {failure}",
                file=sys.stderr,
            )
        return 1
    print(
        "canonical_query_execution_authority_gate: PASS "
        f"modules={len(MODULES)} classification=source_contract_not_runtime_proof"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
