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
        465,
        21_614,
        "SB_ENGINE_CANONICAL_QUERY_EXECUTE_COORDINATOR_AUTHORITY",
        2,
    ),
    "canonical_query_literal_values_composition.cpp": (
        368, 17_635,
        "SB_ENGINE_CANONICAL_QUERY_LITERAL_VALUES_COMPOSITION_AUTHORITY", 2,
    ),
    "canonical_query_model_family_planning.cpp": (
        122, 5_025,
        "SB_ENGINE_CANONICAL_QUERY_MODEL_FAMILY_PLANNING_AUTHORITY", 1,
    ),
    "canonical_query_runtime_services.cpp": (
        109, 3_516,
        "SB_ENGINE_CANONICAL_QUERY_RUNTIME_SERVICES_AUTHORITY", 2,
    ),
    "canonical_query_runtime_observation_support.cpp": (
        375, 18_079,
        "SB_ENGINE_CANONICAL_QUERY_RUNTIME_OBSERVATION_SUPPORT_AUTHORITY", 2,
    ),
    "canonical_query_filter_predicate_receipt.cpp": (
        333, 14_562,
        "SB_ENGINE_CANONICAL_QUERY_FILTER_PREDICATE_RECEIPT_AUTHORITY", 2,
    ),
    "canonical_query_persisted_descriptor_authority.cpp": (
        331, 14_733,
        "SB_ENGINE_CANONICAL_QUERY_PERSISTED_DESCRIPTOR_AUTHORITY_AUTHORITY", 2,
    ),
    "canonical_query_unary_preparation.cpp": (
        1_127, 45_993,
        "SB_ENGINE_CANONICAL_QUERY_UNARY_PREPARATION_AUTHORITY", 2,
    ),
    "canonical_query_join_set_preparation.cpp": (
        1_163, 49_130,
        "SB_ENGINE_CANONICAL_QUERY_JOIN_SET_PREPARATION_AUTHORITY", 2,
    ),
    "canonical_query_subquery_preparation.cpp": (
        303, 12_477,
        "SB_ENGINE_CANONICAL_QUERY_SUBQUERY_PREPARATION_AUTHORITY", 2,
    ),
    "canonical_query_window_preparation.cpp": (
        1_255,
        59_764,
        "SB_ENGINE_CANONICAL_QUERY_WINDOW_PREPARATION_AUTHORITY",
        2,
    ),
    "canonical_query_global_aggregate_preparation.cpp": (
        1_937,
        83_671,
        "SB_ENGINE_CANONICAL_QUERY_GLOBAL_AGGREGATE_PREPARATION_AUTHORITY",
        2,
    ),
    "canonical_query_grouped_aggregate_preparation.cpp": (
        1_599,
        75_454,
        "SB_ENGINE_CANONICAL_QUERY_GROUPED_AGGREGATE_PREPARATION_AUTHORITY",
        2,
    ),
    "canonical_query_aggregate_composition.cpp": (
        1_693,
        81_143,
        "SB_ENGINE_CANONICAL_QUERY_AGGREGATE_COMPOSITION_AUTHORITY",
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
    "canonical_query_filter_project_composition.cpp": (
        2_448,
        111_926,
        "SB_ENGINE_CANONICAL_QUERY_FILTER_PROJECT_COMPOSITION_AUTHORITY",
        2,
    ),
    "canonical_query_order_limit_composition.cpp": (
        969,
        43_206,
        "SB_ENGINE_CANONICAL_QUERY_ORDER_LIMIT_COMPOSITION_AUTHORITY",
        2,
    ),
    "canonical_query_join_registration.cpp": (
        857,
        42_577,
        "SB_ENGINE_CANONICAL_QUERY_JOIN_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_join_composition.cpp": (
        523,
        22_815,
        "SB_ENGINE_CANONICAL_QUERY_JOIN_COMPOSITION_AUTHORITY",
        2,
    ),
    "canonical_query_join_pipeline_composition.cpp": (
        831,
        38_130,
        "SB_ENGINE_CANONICAL_QUERY_JOIN_PIPELINE_COMPOSITION_AUTHORITY",
        2,
    ),
    "canonical_query_node_composition.cpp": (
        6_438,
        313_200,
        "SB_ENGINE_CANONICAL_QUERY_NODE_COMPOSITION_AUTHORITY",
        2,
    ),
    "canonical_query_object_free_profile.cpp": (
        250,
        11_934,
        "SB_ENGINE_CANONICAL_QUERY_OBJECT_FREE_PROFILE_AUTHORITY",
        2,
    ),
    "canonical_query_object_free_composition_support.cpp": (
        343,
        14_902,
        "SB_ENGINE_CANONICAL_QUERY_OBJECT_FREE_COMPOSITION_SUPPORT_AUTHORITY",
        2,
    ),
    "canonical_query_physical_registration.cpp": (
        972,
        42_303,
        "SB_ENGINE_CANONICAL_QUERY_PHYSICAL_REGISTRATION_AUTHORITY",
        2,
    ),
    "canonical_query_pivot_composition.cpp": (
        1_574,
        72_281,
        "SB_ENGINE_CANONICAL_QUERY_PIVOT_COMPOSITION_AUTHORITY",
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
    "canonical_query_set_composition.cpp": (
        820,
        36_819,
        "SB_ENGINE_CANONICAL_QUERY_SET_COMPOSITION_AUTHORITY",
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
    "canonical_query_time_series_composition.cpp": (
        4_783,
        242_149,
        "SB_ENGINE_CANONICAL_QUERY_TIME_SERIES_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_vector_composition.cpp": (
        1_104,
        56_866,
        "SB_ENGINE_CANONICAL_QUERY_VECTOR_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_search_composition.cpp": (
        2_528,
        127_548,
        "SB_ENGINE_CANONICAL_QUERY_SEARCH_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_key_value_composition.cpp": (
        2_396,
        119_306,
        "SB_ENGINE_CANONICAL_QUERY_KEY_VALUE_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_graph_composition.cpp": (
        2_756,
        139_015,
        "SB_ENGINE_CANONICAL_QUERY_GRAPH_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_document_composition.cpp": (
        3_738,
        188_993,
        "SB_ENGINE_CANONICAL_QUERY_DOCUMENT_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_spatial_columnar_composition.cpp": (
        6_864,
        333_991,
        "SB_ENGINE_CANONICAL_QUERY_SPATIAL_COLUMNAR_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_multileg_composition.cpp": (
        5_115,
        254_142,
        "SB_ENGINE_CANONICAL_QUERY_MULTILEG_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_table_function_composition.cpp": (
        725,
        33_629,
        "SB_ENGINE_CANONICAL_QUERY_TABLE_FUNCTION_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_current_heap_composition.cpp": (
        3_189,
        156_530,
        "SB_ENGINE_CANONICAL_QUERY_CURRENT_HEAP_COMPOSITION_AUTHORITY",
        1,
    ),
    "canonical_query_current_heap_join_composition.cpp": (
        2_982,
        141_041,
        "SB_ENGINE_CANONICAL_QUERY_CURRENT_HEAP_JOIN_COMPOSITION_AUTHORITY",
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

MODEL_FAMILY_ROUTE_FORBIDDEN = (
    "PersistLocalTransactionInventory",
    "FinalizePhysicalMgaCowTransaction",
    "EngineCommit",
    "EngineRollback",
    "ExecuteCanonicalObjectFreeValuesQuery",
    "ExecuteCanonicalCurrentHeapQuery",
    "parser/",
    "wal/",
)


def main() -> int:
    failures: list[str] = []
    cmake_text = CMAKE.read_text(encoding="utf-8")
    production_sources = cmake_text.partition("add_library(sb_engine_sblr\n")[2]
    production_sources = production_sources.partition("\n)")[0]
    contract_sources = cmake_text.partition("add_library(sb_qow_sblr_query_route_contract STATIC\n")[2]
    contract_sources = contract_sources.partition("\n  )")[0]
    for receipt_support in (
        "canonical_query_runtime_observation_support.cpp",
        "canonical_query_filter_predicate_receipt.cpp",
        "canonical_query_persisted_descriptor_authority.cpp",
    ):
        if (f"  {receipt_support}\n" not in production_sources or
                f"    {receipt_support}\n" not in contract_sources):
            failures.append(f"{receipt_support}: requires production and contract ownership")
        path = SBLR_ROOT / receipt_support
        if path.is_file():
            support_text = path.read_text(encoding="utf-8")
            for token in SHARED_SUPPORT_FORBIDDEN + (
                "EngineBegin", "EngineCommit", "EngineRollback",
                "AcquireTransactionInventoryGuard",
                "PlanAndPublishLivePhysicalDag(",
                "ExecuteSelectedCanonicalObjectFreeDag(",
                "ExecuteCanonicalOptimizerSelectedDag(",
                "AppendMgaRowVersion", "UpdateMgaRowVersion", "DeleteMgaRowVersion",
            ):
                if token in support_text:
                    failures.append(f"{receipt_support}: forbidden receipt support authority {token!r}")
    for service in (
        "canonical_query_literal_values_composition.cpp",
        "canonical_query_model_family_planning.cpp",
        "canonical_query_runtime_services.cpp",
    ):
        if f"  {service}\n" not in production_sources:
            failures.append(f"{service}: requires production ownership")
        contract_owned = f"    {service}\n" in contract_sources
        if contract_owned != (service != "canonical_query_model_family_planning.cpp"):
            failures.append(f"{service}: incorrect contract ownership")
        path = SBLR_ROOT / service
        if path.is_file():
            service_text = path.read_text(encoding="utf-8")
            forbidden = SHARED_SUPPORT_FORBIDDEN + (
                "EngineBegin", "EngineCommit", "EngineRollback",
                "AcquireTransactionInventoryGuard",
                "ExecuteCanonicalOptimizerSelectedDag(",
                "AppendMgaRowVersion", "UpdateMgaRowVersion", "DeleteMgaRowVersion",
            )
            if service != "canonical_query_literal_values_composition.cpp":
                forbidden += (
                    "ExecuteSelectedCanonicalObjectFreeDag(",
                    "PlanAndPublishLivePhysicalDag(",
                )
            if service == "canonical_query_runtime_services.cpp":
                forbidden += ("PlanOptimizerOwnedModelFamilySourceV1(",)
            for token in forbidden:
                if token in service_text:
                    failures.append(f"{service}: forbidden service authority {token!r}")
    for preparation in (
        "canonical_query_unary_preparation.cpp",
        "canonical_query_join_set_preparation.cpp",
        "canonical_query_subquery_preparation.cpp",
        "canonical_query_global_aggregate_preparation.cpp",
        "canonical_query_grouped_aggregate_preparation.cpp",
        "canonical_query_window_preparation.cpp",
    ):
        if (f"  {preparation}\n" not in production_sources or
                f"    {preparation}\n" not in contract_sources):
            failures.append(f"{preparation}: requires production and contract ownership")
        path = SBLR_ROOT / preparation
        if path.is_file():
            preparation_text = path.read_text(encoding="utf-8")
            for token in SHARED_SUPPORT_FORBIDDEN + (
                "EngineBegin", "EngineCommit", "EngineRollback",
                "PlanAndPublishLivePhysicalDag(",
                "ExecuteSelectedCanonicalObjectFreeDag(",
                "ExecuteCanonicalOptimizerSelectedDag(",
                "AppendMgaRowVersion", "UpdateMgaRowVersion", "DeleteMgaRowVersion",
            ):
                if token in preparation_text:
                    failures.append(f"{preparation}: forbidden preparation authority {token!r}")
    if "  canonical_query_table_function_composition.cpp\n" not in production_sources:
        failures.append("table-function composition requires production SBLR ownership")
    if "  canonical_query_current_heap_join_composition.cpp\n" not in production_sources:
        failures.append("current-heap join composition requires production SBLR ownership")
    if "  canonical_query_current_heap_composition.cpp\n" not in production_sources:
        failures.append("current-heap composition requires production SBLR ownership")
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
        if "function_seed_registry.hpp" in coordinator or "namespace fn =" in coordinator:
            failures.append("production function registry leaked into coordinator")
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
            "void PublishOrdinaryRuntimeObservations(",
            "bool HasOrdinaryRuntimeObservationWrapperTarget(",
            "struct FilterPredicateReceiptIssueResult {",
            "class CanonicalDescriptorFilterPredicateReceiptIssuer {",
            "bool ValidateCanonicalPersistedTextRowDescriptorAuthorityV1(",
            "BindCanonicalPersistedRowDescriptorAuthorityV1(",
            "LiveSetRegistrationProfiles MakeLiveSetRegistrationProfiles(",
            "bool BoundSetOperationEqualityComparisons(",
            "MaterializedSetOperationPlanningState MaterializeSetOperationPlanningState(",
            "LiveProjectRegistrationProfile MakeLiveProjectRegistrationProfile(",
            "LivePredicateSubqueryProfile MatchLivePredicateSubqueryProfile(",
            "LiveLateralSubqueryProfile MatchLiveLateralSubqueryProfile(",
            "LiveRecursiveCteProfile MatchLiveRecursiveCteProfile(",
            "bool PrepareCanonicalSortOrderTerm(",
            "PreparedSortRoot PrepareSortRoot(",
            "PreparedDistinctRoot PrepareQueryDistinctRoot(",
            "PreparedLimitRoot PrepareLimitRoot(",
            "PreparedProjectRoot PrepareDescriptorDirectProjectRoot(",
            "bool PrepareInputRowBinding(",
            "PreparedSortRoot PrepareExpressionSortRoot(",
            "PreparedProjectRoot PrepareExpressionProjectRoot(",
            "PreparedFilterRoot PrepareFilterRoot(",
            "bool ExactCanonicalBooleanJoinAliasDescriptorV1(",
            "std::string ExactCanonicalBooleanJoinAliasRuntimeCarrierV1(",
            "bool ProjectCanonicalBooleanJoinAliasRuntimeCarriersV1(",
            "PreparedJoinRoot PrepareJoinRoot(",
            "LiveSetOperationProfile MatchLiveSetOperationProfile(",
            "PreparedSetOperationRoot PrepareSetOperationRoot(",
            "bool EvaluateNonNegativeRowBound(",
            "bool BindPreparedRecursiveCteCardinalityImpl(",
            "MatchLiveLateralSubqueryProfile(",
            "bool DirectValueWindowUsesExactTypeV1(",
            "bool ExactCanonicalBooleanWindowSourceV1(",
            "bool CanonicalDescriptorFieldEqualsV1(",
            "unsigned ExactBoundedSignedIntegerTypeRankV1(",
            "bool ExactCanonicalScalarWindowOperandV1(",
            "bool ExactCanonicalBoundedSignedWindowSourceV1(",
            "bool ExactCanonicalBoundedSignedWindowOrderV1(",
            "GlobalRankingWindowProfile GlobalAggregateWindowProfileV1(",
            "PreparedGlobalRowNumberWindowBinding PrepareGlobalRankingWindowBinding(",
            "PreparedGlobalRowNumberWindowBinding PrepareGlobalRowNumberWindowBinding(",
            "bool CanonicalDescriptorFieldEqualsForComposition(",
            "unsigned ExactBoundedSignedIntegerTypeRankForComposition(",
            "LiveGroupedCountSumProfile MatchLiveGroupedCountSumProfile(",
            "bool IsLiveGroupedHavingProfile(",
            "LiveUnaryAggregateExpressionProfile MatchLiveUnaryAggregateExpressionProfile(",
            "LivePairStatisticalExpressionProfile MatchLivePairStatisticalExpressionProfile(",
            "LiveStringAggregateExpressionProfile MatchLiveStringAggregateExpressionProfile(",
            "MatchLiveOrderedSingleCollectionExpressionProfile(\n",
            "MatchLiveJsonObjectAggregateExpressionProfile(\n",
            "LiveListaggExpressionProfile MatchLiveListaggExpressionProfile(",
            "LiveOrderedSetExpressionProfile MatchLiveOrderedSetExpressionProfile(",
            "LiveApproximateExpressionProfile MatchLiveApproximateExpressionProfile(",
            "PreparedGlobalAggregateRoot PrepareGlobalAggregateRoot(",
            "PreparedGroupedCountSumRoot PrepareGroupedCountSumRoot(",
            "PreparedGroupedHavingRoot PrepareGroupedHavingRoot(",
            "bool InitializePlanningAggregateDistinctState(",
            "bool InitializePlanningAggregateModifierState(",
            "bool AdmitPlanningAggregateDistinctTuple(",
            "bool MeasureAggregateDistinctPeakMemoryForComposition(",
            "CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalCurrentHeapSingleSourceQuery(",
            "struct CurrentHeapStreamingScanBinding {",
            "struct CurrentHeapStreamingCompactRow {",
            "bool CurrentHeapMemoryAdd(",
            "bool CurrentHeapMemoryMultiply(",
            "bool CurrentHeapAccountString(",
            "bool CurrentHeapAccountDescriptor(",
            "bool CurrentHeapStreamingBindingMemory(",
            "bool CurrentHeapStreamingCompactRowMemory(",
            "bool PrepareCurrentHeapStreamingScanBinding(",
            "bool MaterializeCurrentHeapStreamingRow(",
            "CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalCurrentHeapJoin(",
            "bool MaterializeCanonicalGenerateSeriesBatch(",
            "constexpr std::string_view kGenerateSeriesFunctionId =",
            "constexpr std::string_view kGenerateSeriesFunctionUuid =",
            "constexpr std::size_t kGenerateSeriesMaximumRowCount =",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalGenerateSeriesTableFunctionQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalGenerateSeriesMatchRecognizeQuery(",
            "std::optional<std::string> Rcp079DescriptorField(",
            "WithMultilegResultDescriptorRebindingV1(",
            "void CaptureRcp079ModelLegV1(",
            "MakeRcp079CapturedModelLegRegistration(",
            "exec::CanonicalPhysicalExecutorRegistration MakeRcp079AsofRegistration(",
            "std::string Rcp079CanonicalReal64(",
            "plan::CanonicalMgaStatementContext Rcp079LogicalMga(",
            "api::TypedRelationalDag Rcp079OperatorLocalModelSourceDag(",
            "std::string Rcp079ModelFamilyForSource(",
            "Rcp079PreflightMultilegResultDescriptorsV1(",
            "bool CaptureRcp079ModelSourceLeg(",
            "std::string Rcp080LogicalOperatorV1(",
            "std::string Rcp080ImplementationV1(",
            "std::string Rcp080OperationV1(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalBoundedModelFamilyCompositionQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalCapturedModelFamilyJoinQuery(",
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
            "api::EngineApiResult Failure(",
            "struct MaterializedValues {",
            "MaterializedValues MaterializeValues(",
            "api::EngineApiResult SuccessfulApiResult(",
            "struct PreparedSetOperationRoot {",
            "struct LiveSetOperationProfile {",
            "struct PreparedLiveSetNode {",
            "struct PreparedJoinRoot {",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeSetOperationQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeNestedSetOperationQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeJoinQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeInnerJoinFilterProjectQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeFilterQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeProjectQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeFilterProjectQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeProjectSortQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeFilterProjectSortQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeFilterProjectSortLimitQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeFilterProjectDistinctSortLimitQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeLimitQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeSortQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeDistinctSortLimitQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreePivotQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeUnpivotQuery(",
            "bool CanonicalPivotCausalStructureExactlyMatches(",
            "bool CanonicalPivotExecutionReceiptMatches(",
            "bool CanonicalUnpivotOutputExactlyMatches(",
            "bool CanonicalUnpivotExecutionReceiptMatches(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeGroupedCountSumQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeGlobalAggregateQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalObjectFreeNodeDrivenCompositionQuery(",
            "CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalTimeSeriesFamilyQuery(",
            "CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalVectorFamilyQuery(",
            "CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalSearchFamilyQuery(",
            "CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalKeyValueFamilyQuery(",
            "CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalGraphFamilyQuery(",
            "CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalDocumentFamilyQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalColumnarFamilyJoinQuery(",
            "CanonicalObjectFreeValuesExecutionResult\n"
            "ExecuteCanonicalSpatialColumnarFamilyQuery(",
            "struct ContextualTextDirectRouteTargetV2 {",
            "bool Rcp079ExactContextualTextDirectRouteCandidateV2(",
            "struct Rcp079ColumnarJoinSourceV1 {",
            "bool CompareCanonicalQueryScalarsV1(",
            "bool CompareCanonicalRelationalScalarsV1(",
            "bool PollLiveCancellationProbeImpl(",
            "bool PollLiveCancellationProbe(",
            "opt::ModelFamilyCapabilitySnapshotV1 MakeModelFamilyCapabilitySnapshotV1(",
            "opt::ModelFamilyCoordinatorResultV1 PlanCanonicalModelFamilySourceV1(",
            "opt::ModelFamilyCapabilitySnapshotV1\nMakeModelFamilyCapabilitySnapshotForCompositionV1(",
            "opt::ModelFamilyCoordinatorResultV1\nPlanCanonicalModelFamilySourceForCompositionV1(",
            "CanonicalObjectFreeValuesExecutionResult\nExecuteCanonicalObjectFreeLiteralValuesQuery(",
        ):
            if extracted_definition in coordinator:
                failures.append(
                    "canonical_query_execute.cpp: extracted definition returned"
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
        "canonical_query_object_free_composition_support.cpp",
        "canonical_query_predicate_support.cpp",
        "canonical_query_table_function_composition.cpp",
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
        "canonical_query_aggregate_composition.cpp",
        "canonical_query_aggregate_registration.cpp",
        "canonical_query_filter_registration.cpp",
        "canonical_query_filter_project_composition.cpp",
        "canonical_query_order_limit_composition.cpp",
        "canonical_query_join_registration.cpp",
        "canonical_query_join_composition.cpp",
        "canonical_query_join_pipeline_composition.cpp",
        "canonical_query_node_composition.cpp",
        "canonical_query_physical_registration.cpp",
        "canonical_query_table_function_composition.cpp",
        "canonical_query_pivot_composition.cpp",
        "canonical_query_projection_registration.cpp",
        "canonical_query_recursive_registration.cpp",
        "canonical_query_relational_registration.cpp",
        "canonical_query_set_registration.cpp",
        "canonical_query_set_composition.cpp",
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

    for model_route_name in (
        "canonical_query_time_series_composition.cpp",
        "canonical_query_vector_composition.cpp",
        "canonical_query_search_composition.cpp",
        "canonical_query_key_value_composition.cpp",
        "canonical_query_graph_composition.cpp",
        "canonical_query_document_composition.cpp",
        "canonical_query_spatial_columnar_composition.cpp",
        "canonical_query_multileg_composition.cpp",
        "canonical_query_current_heap_join_composition.cpp",
        "canonical_query_current_heap_composition.cpp",
    ):
        model_route_path = SBLR_ROOT / model_route_name
        if not model_route_path.is_file():
            continue
        model_route = model_route_path.read_text(encoding="utf-8")
        for token in MODEL_FAMILY_ROUTE_FORBIDDEN:
            if token in model_route:
                failures.append(
                    f"{model_route_name}: forbidden authority token {token!r}"
                )
        if model_route_name in (
            "canonical_query_current_heap_join_composition.cpp",
            "canonical_query_current_heap_composition.cpp",
        ):
            for token in (
                "EngineBegin",
                "EngineResolveStatementSnapshot",
                "AppendMgaRowVersion",
                "UpdateMgaRowVersion",
                "DeleteMgaRowVersion",
            ):
                if token in model_route:
                    failures.append(
                        f"{model_route_name}: forbidden mutation/snapshot token {token!r}"
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
