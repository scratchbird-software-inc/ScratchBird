#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kSblrLiteralExecutorId = "engine.op.literal";
inline constexpr std::uint16_t kSblrLiteralOpcodeCode = 3;
inline constexpr const char* kSblrLiteralOpcodeVersion = "1.0";
inline constexpr const char* kSblrLiteralOperandDescriptorId = "typed_literal";
inline constexpr const char* kSblrLiteralResultDescriptorId = "typed_value";
inline constexpr std::uint16_t kSblrLiteralResultDescriptorVersion = 1;
inline constexpr const char* kSblrParameterExecutorId = "engine.op.parameter";
inline constexpr std::uint16_t kSblrParameterOpcodeCode = 4;
inline constexpr const char* kSblrParameterOpcodeVersion = "1.0";
inline constexpr const char* kSblrParameterOperandDescriptorId =
    "parameter_descriptor_ref";
inline constexpr const char* kSblrParameterResultDescriptorId = "typed_value";
inline constexpr std::uint16_t kSblrParameterResultDescriptorVersion = 1;
inline constexpr const char* kSblrVariableExecutorId = "engine.op.variable";
inline constexpr std::uint16_t kSblrVariableOpcodeCode = 5;
inline constexpr const char* kSblrVariableOpcodeVersion = "1.0";
inline constexpr const char* kSblrVariableOperandDescriptorId =
    "variable_descriptor_ref";
inline constexpr const char* kSblrVariableResultDescriptorId = "typed_value";
inline constexpr std::uint16_t kSblrVariableResultDescriptorVersion = 1;
inline constexpr const char* kSblrSourceMapExecutorId = "engine.op.source_map";
inline constexpr std::uint16_t kSblrSourceMapOpcodeCode = 6;
inline constexpr const char* kSblrSourceMapOpcodeVersion = "1.0";
inline constexpr const char* kSblrSourceMapOperandDescriptorId =
    "source_map_entry_vector";
inline constexpr const char* kSblrSourceMapResultDescriptorId = "void";
inline constexpr std::uint16_t kSblrSourceMapResultDescriptorVersion = 1;
inline constexpr const char* kSblrErrorVectorExecutorId = "engine.op.error_vector";
inline constexpr std::uint16_t kSblrErrorVectorOpcodeCode = 7;
inline constexpr const char* kSblrErrorVectorOpcodeVersion = "1.0";
inline constexpr const char* kSblrErrorVectorOperandDescriptorId = "diagnostic_vector";
inline constexpr const char* kSblrErrorVectorResultDescriptorId = "void";
inline constexpr std::uint16_t kSblrErrorVectorResultDescriptorVersion = 1;
inline constexpr const char* kSblrTxnBeginExecutorId = "engine.op.txn_begin";
inline constexpr std::uint16_t kSblrTxnBeginOpcodeCode = 256;
inline constexpr const char* kSblrTxnBeginOpcodeVersion = "1.0";
inline constexpr const char* kSblrTxnBeginOperandDescriptorId =
    "transaction_begin_options";
inline constexpr const char* kSblrTxnBeginResultDescriptorId =
    "transaction_handle";
inline constexpr std::uint16_t kSblrTxnBeginResultDescriptorVersion = 1;
inline constexpr const char* kSblrTxnCommitExecutorId = "engine.op.txn_commit";
inline constexpr std::uint16_t kSblrTxnCommitOpcodeCode = 257;
inline constexpr const char* kSblrTxnCommitOpcodeVersion = "1.0";
inline constexpr const char* kSblrTxnCommitOperandDescriptorId =
    "transaction_handle_and_commit_options";
inline constexpr const char* kSblrTxnCommitResultDescriptorId = "commit_result";
inline constexpr std::uint16_t kSblrTxnCommitResultDescriptorVersion = 1;
inline constexpr const char* kSblrTxnRollbackExecutorId = "engine.op.txn_rollback";
inline constexpr std::uint16_t kSblrTxnRollbackOpcodeCode = 258;
inline constexpr const char* kSblrTxnRollbackOpcodeVersion = "1.0";
inline constexpr const char* kSblrTxnRollbackOperandDescriptorId = "transaction_handle_and_rollback_options";
inline constexpr const char* kSblrTxnRollbackResultDescriptorId = "rollback_result";
inline constexpr std::uint16_t kSblrTxnRollbackResultDescriptorVersion = 1;
inline constexpr const char* kSblrTxnSavepointExecutorId = "engine.op.txn_savepoint";
inline constexpr std::uint16_t kSblrTxnSavepointOpcodeCode = 259;
inline constexpr const char* kSblrTxnSavepointOpcodeVersion = "1.0";
inline constexpr const char* kSblrTxnSavepointOperandDescriptorId = "savepoint_descriptor";
inline constexpr const char* kSblrTxnSavepointResultDescriptorId = "savepoint_handle";
inline constexpr std::uint16_t kSblrTxnSavepointResultDescriptorVersion = 1;
inline constexpr const char* kSblrTxnReleaseSavepointExecutorId = "engine.op.txn_release_savepoint";
inline constexpr std::uint16_t kSblrTxnReleaseSavepointOpcodeCode = 260;
inline constexpr const char* kSblrTxnReleaseSavepointOpcodeVersion = "1.0";
inline constexpr const char* kSblrTxnReleaseSavepointOperandDescriptorId = "savepoint_release_handle";
inline constexpr const char* kSblrTxnReleaseSavepointResultDescriptorId = "savepoint_release_result";
inline constexpr std::uint16_t kSblrTxnReleaseSavepointResultDescriptorVersion = 1;
inline constexpr const char* kSblrTxnRollbackToSavepointExecutorId = "engine.op.txn_rollback_to_savepoint";
inline constexpr std::uint16_t kSblrTxnRollbackToSavepointOpcodeCode = 261;
inline constexpr const char* kSblrTxnRollbackToSavepointOpcodeVersion = "1.0";
inline constexpr const char* kSblrTxnRollbackToSavepointOperandDescriptorId = "savepoint_rollback_handle";
inline constexpr const char* kSblrTxnRollbackToSavepointResultDescriptorId = "savepoint_rollback_result";
inline constexpr std::uint16_t kSblrTxnRollbackToSavepointResultDescriptorVersion = 1;
inline constexpr const char* kSblrPsqlAutonomousFrameExecutorId = "engine.op.psql_autonomous_frame";
inline constexpr std::uint16_t kSblrPsqlAutonomousFrameOpcodeCode = 262;
inline constexpr const char* kSblrPsqlAutonomousFrameOpcodeVersion = "1.0";
inline constexpr const char* kSblrPsqlAutonomousFrameOperandDescriptorId = "autonomous_frame_descriptor";
inline constexpr const char* kSblrPsqlAutonomousFrameResultDescriptorId = "autonomous_frame_result";
inline constexpr std::uint16_t kSblrPsqlAutonomousFrameResultDescriptorVersion = 1;
inline constexpr const char* kSblrReservationReleaseExecutorId = "engine.op.transaction_reservation_release";
inline constexpr std::uint16_t kSblrReservationReleaseOpcodeCode = 263;
inline constexpr const char* kSblrReservationReleaseOpcodeVersion = "1.0";
inline constexpr const char* kSblrReservationReleaseOperandDescriptorId = "relation_reservation_release_descriptor";
inline constexpr const char* kSblrReservationReleaseResultDescriptorId = "transaction_reservation_result";
inline constexpr std::uint16_t kSblrReservationReleaseResultDescriptorVersion = 1;
inline constexpr const char* kSblrTemporaryInstanceCleanupExecutorId = "engine.op.temporary_instance_cleanup";
inline constexpr std::uint16_t kSblrTemporaryInstanceCleanupOpcodeCode = 264;
inline constexpr const char* kSblrTemporaryInstanceCleanupOpcodeVersion = "1.0";
inline constexpr const char* kSblrTemporaryInstanceCleanupOperandDescriptorId = "temporary_instance_cleanup_descriptor";
inline constexpr const char* kSblrTemporaryInstanceCleanupResultDescriptorId = "temporary_cleanup_result";
inline constexpr std::uint16_t kSblrTemporaryInstanceCleanupResultDescriptorVersion = 1;
inline constexpr const char* kSblrCursorOpenExecutorId = "engine.op.cursor_open";
inline constexpr std::uint16_t kSblrCursorOpenOpcodeCode = 512;
inline constexpr const char* kSblrCursorOpenOpcodeVersion = "1.0";
inline constexpr const char* kSblrCursorOpenOperandDescriptorId = "cursor_open_plan_ref";
inline constexpr const char* kSblrCursorOpenResultDescriptorId = "cursor_handle";
inline constexpr std::uint16_t kSblrCursorOpenResultDescriptorVersion = 1;
inline constexpr const char* kSblrCursorFetchExecutorId = "engine.op.cursor_fetch";
inline constexpr std::uint16_t kSblrCursorFetchOpcodeCode = 513;
inline constexpr const char* kSblrCursorFetchOpcodeVersion = "1.0";
inline constexpr const char* kSblrCursorFetchOperandDescriptorId = "cursor_fetch_handle";
inline constexpr const char* kSblrCursorFetchResultDescriptorId = "cursor_fetch_result";
inline constexpr std::uint16_t kSblrCursorFetchResultDescriptorVersion = 1;
inline constexpr const char* kSblrCursorCloseExecutorId = "engine.op.cursor_close";
inline constexpr std::uint16_t kSblrCursorCloseOpcodeCode = 514;
inline constexpr const char* kSblrCursorCloseOpcodeVersion = "1.0";
inline constexpr const char* kSblrCursorCloseOperandDescriptorId = "cursor_close_handle";
inline constexpr const char* kSblrCursorCloseResultDescriptorId = "cursor_close_result";
inline constexpr std::uint16_t kSblrCursorCloseResultDescriptorVersion = 1;
inline constexpr const char* kSblrReadByKeyExecutorId = "engine.op.read_by_key";
inline constexpr std::uint16_t kSblrReadByKeyOpcodeCode = 515;
inline constexpr const char* kSblrReadByKeyOpcodeVersion = "1.0";
inline constexpr const char* kSblrReadByKeyOperandDescriptorId = "uuid_object_key_descriptor";
inline constexpr const char* kSblrReadByKeyResultDescriptorId = "row_descriptor";
inline constexpr std::uint16_t kSblrReadByKeyResultDescriptorVersion = 1;
inline constexpr const char* kSblrReadRangeExecutorId = "engine.op.read_range";
inline constexpr std::uint16_t kSblrReadRangeOpcodeCode = 516;
inline constexpr const char* kSblrReadRangeOpcodeVersion = "1.0";
inline constexpr const char* kSblrReadRangeOperandDescriptorId = "range_scan_descriptor";
inline constexpr const char* kSblrReadRangeResultDescriptorId = "rowset_descriptor";
inline constexpr std::uint16_t kSblrReadRangeResultDescriptorVersion = 1;
inline constexpr const char* kSblrReadStreamExecutorId = "engine.op.read_stream";
inline constexpr std::uint16_t kSblrReadStreamOpcodeCode = 517;
inline constexpr const char* kSblrReadStreamOpcodeVersion = "1.0";
inline constexpr const char* kSblrReadStreamOperandDescriptorId = "stream_descriptor";
inline constexpr const char* kSblrReadStreamResultDescriptorId = "stream_handle";
inline constexpr std::uint16_t kSblrReadStreamResultDescriptorVersion = 1;
inline constexpr const char* kSblrResultSetPassExecutorId = "engine.op.result_set_pass";
inline constexpr std::uint16_t kSblrResultSetPassOpcodeCode = 518;
inline constexpr const char* kSblrResultSetPassOpcodeVersion = "1.0";
inline constexpr const char* kSblrResultSetPassOperandDescriptorId = "result_set_handle_and_lifetime";
inline constexpr const char* kSblrResultSetPassResultDescriptorId = "result_set_handle";
inline constexpr std::uint16_t kSblrResultSetPassResultDescriptorVersion = 1;
inline constexpr const char* kSblrAccessCursorOpenExecutorId = "engine.op.access_cursor_open";
inline constexpr std::uint16_t kSblrAccessCursorOpenOpcodeCode = 519;
inline constexpr const char* kSblrAccessCursorOpenOpcodeVersion = "1.0";
inline constexpr const char* kSblrAccessCursorOpenOperandDescriptorId = "access_cursor_open_descriptor";
inline constexpr const char* kSblrAccessCursorOpenResultDescriptorId = "access_cursor_handle";
inline constexpr std::uint16_t kSblrAccessCursorOpenResultDescriptorVersion = 1;
inline constexpr const char* kSblrAccessCursorFetchExecutorId = "engine.op.access_cursor_fetch";
inline constexpr std::uint16_t kSblrAccessCursorFetchOpcodeCode = 520;
inline constexpr const char* kSblrAccessCursorFetchOpcodeVersion = "1.0";
inline constexpr const char* kSblrAccessCursorFetchOperandDescriptorId = "access_cursor_fetch_descriptor";
inline constexpr const char* kSblrAccessCursorFetchResultDescriptorId = "access_cursor_rowset_or_eof";
inline constexpr std::uint16_t kSblrAccessCursorFetchResultDescriptorVersion = 1;
inline constexpr const char* kSblrAccessCursorCloseExecutorId = "engine.op.access_cursor_close";
inline constexpr std::uint16_t kSblrAccessCursorCloseOpcodeCode = 521;
inline constexpr const char* kSblrAccessCursorCloseOpcodeVersion = "1.0";
inline constexpr const char* kSblrAccessCursorCloseOperandDescriptorId = "access_cursor_close_descriptor";
inline constexpr const char* kSblrAccessCursorCloseResultDescriptorId = "void";
inline constexpr std::uint16_t kSblrAccessCursorCloseResultDescriptorVersion = 1;
inline constexpr const char* kSblrInsertExecutorId = "engine.op.insert";
inline constexpr std::uint16_t kSblrInsertOpcodeCode = 768;
inline constexpr const char* kSblrInsertOpcodeVersion = "1.0";
inline constexpr const char* kSblrInsertOperandDescriptorId = "insert_descriptor";
inline constexpr const char* kSblrInsertResultDescriptorId = "mutation_result";
inline constexpr std::uint16_t kSblrInsertResultDescriptorVersion = 1;
inline constexpr const char* kSblrUpdateExecutorId="engine.op.update";inline constexpr std::uint16_t kSblrUpdateOpcodeCode=769;inline constexpr const char* kSblrUpdateOpcodeVersion="1.0";inline constexpr const char* kSblrUpdateOperandDescriptorId="update_descriptor";inline constexpr const char* kSblrUpdateResultDescriptorId="mutation_result";inline constexpr std::uint16_t kSblrUpdateResultDescriptorVersion=1;
inline constexpr const char* kSblrDeleteExecutorId="engine.op.delete";inline constexpr std::uint16_t kSblrDeleteOpcodeCode=770;inline constexpr const char* kSblrDeleteOpcodeVersion="1.0";inline constexpr const char* kSblrDeleteOperandDescriptorId="delete_descriptor";inline constexpr const char* kSblrDeleteResultDescriptorId="mutation_result";inline constexpr std::uint16_t kSblrDeleteResultDescriptorVersion=1;
inline constexpr const char* kSblrMergeExecutorId="engine.op.merge";inline constexpr std::uint16_t kSblrMergeOpcodeCode=771;inline constexpr const char* kSblrMergeOpcodeVersion="1.0";inline constexpr const char* kSblrMergeOperandDescriptorId="merge_descriptor";inline constexpr const char* kSblrMergeResultDescriptorId="mutation_result";inline constexpr std::uint16_t kSblrMergeResultDescriptorVersion=1;
inline constexpr const char* kSblrTableTruncateExecutorId="engine.op.table_truncate";inline constexpr std::uint16_t kSblrTableTruncateOpcodeCode=773;inline constexpr const char* kSblrTableTruncateOpcodeVersion="1.0";inline constexpr const char* kSblrTableTruncateOperandDescriptorId="truncate_table_descriptor";inline constexpr const char* kSblrTableTruncateResultDescriptorId="mutation_result";inline constexpr std::uint16_t kSblrTableTruncateResultDescriptorVersion=1;
inline constexpr const char* kSblrTableAnalyzeExecutorId="engine.op.table_analyze";inline constexpr std::uint16_t kSblrTableAnalyzeOpcodeCode=774;inline constexpr const char* kSblrTableAnalyzeOpcodeVersion="1.0";inline constexpr const char* kSblrTableAnalyzeOperandDescriptorId="analyze_table_descriptor";inline constexpr const char* kSblrTableAnalyzeResultDescriptorId="mutation_result";inline constexpr std::uint16_t kSblrTableAnalyzeResultDescriptorVersion=1;
inline constexpr const char* kSblrBulkImportStreamExecutorId="engine.op.bulk_import_stream";inline constexpr std::uint16_t kSblrBulkImportStreamOpcodeCode=775;inline constexpr const char* kSblrBulkImportStreamOpcodeVersion="1.0";inline constexpr const char* kSblrBulkImportStreamOperandDescriptorId="bulk_import_stream_descriptor";inline constexpr const char* kSblrBulkImportStreamResultDescriptorId="bulk_mutation_result";inline constexpr std::uint16_t kSblrBulkImportStreamResultDescriptorVersion=1;
inline constexpr const char* kSblrBulkExportStreamExecutorId="engine.op.bulk_export_stream";inline constexpr std::uint16_t kSblrBulkExportStreamOpcodeCode=776;inline constexpr const char* kSblrBulkExportStreamOpcodeVersion="1.0";inline constexpr const char* kSblrBulkExportStreamOperandDescriptorId="bulk_export_stream_descriptor";inline constexpr const char* kSblrBulkExportStreamResultDescriptorId="bulk_read_result";inline constexpr std::uint16_t kSblrBulkExportStreamResultDescriptorVersion=1;
inline constexpr const char* kSblrStatementBatchExecutorId="engine.op.statement_batch";inline constexpr std::uint16_t kSblrStatementBatchOpcodeCode=777;inline constexpr const char* kSblrStatementBatchOpcodeVersion="1.0";inline constexpr const char* kSblrStatementBatchOperandDescriptorId="statement_batch_descriptor";inline constexpr const char* kSblrStatementBatchResultDescriptorId="batch_result_vector";inline constexpr std::uint16_t kSblrStatementBatchResultDescriptorVersion=1;
inline constexpr const char* kSblrAtomicCasExecutorId="engine.op.atomic_cas";inline constexpr std::uint16_t kSblrAtomicCasOpcodeCode=778;inline constexpr const char* kSblrAtomicCasOpcodeVersion="1.0";inline constexpr const char* kSblrAtomicCasOperandDescriptorId="atomic_cas_descriptor";inline constexpr const char* kSblrAtomicCasResultDescriptorId="atomic_cas_result";inline constexpr std::uint16_t kSblrAtomicCasResultDescriptorVersion=1;
inline constexpr const char* kSblrAtomicRmwExecutorId="engine.op.atomic_read_modify_write";inline constexpr std::uint16_t kSblrAtomicRmwOpcodeCode=779;inline constexpr const char* kSblrAtomicRmwOpcodeVersion="1.0";inline constexpr const char* kSblrAtomicRmwOperandDescriptorId="atomic_rmw_descriptor";inline constexpr const char* kSblrAtomicRmwResultDescriptorId="atomic_rmw_result";inline constexpr std::uint16_t kSblrAtomicRmwResultDescriptorVersion=1;
inline constexpr const char* kSblrAdvisoryLockAcquireExecutorId="engine.op.advisory_lock_acquire";inline constexpr std::uint16_t kSblrAdvisoryLockAcquireOpcodeCode=780;inline constexpr const char* kSblrAdvisoryLockAcquireOpcodeVersion="1.0";inline constexpr const char* kSblrAdvisoryLockAcquireOperandDescriptorId="advisory_lock_descriptor";inline constexpr const char* kSblrAdvisoryLockResultDescriptorId="advisory_lock_result";inline constexpr std::uint16_t kSblrAdvisoryLockResultDescriptorVersion=1;
inline constexpr const char* kSblrAdvisoryLockReleaseExecutorId="engine.op.advisory_lock_release";inline constexpr std::uint16_t kSblrAdvisoryLockReleaseOpcodeCode=781;inline constexpr const char* kSblrAdvisoryLockReleaseOpcodeVersion="1.0";inline constexpr const char* kSblrAdvisoryLockReleaseOperandDescriptorId="advisory_lock_release_descriptor";
inline constexpr const char* kSblrFunctionCallExecutorId="engine.op.function_call";inline constexpr std::uint16_t kSblrFunctionCallOpcodeCode=1024;inline constexpr const char* kSblrFunctionCallOpcodeVersion="1.0";inline constexpr const char* kSblrFunctionCallOperandDescriptorId="function_call_descriptor";inline constexpr const char* kSblrFunctionCallResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrFunctionCallResultDescriptorVersion=1;
inline constexpr const char* kSblrOperatorCallExecutorId="engine.op.operator_call";inline constexpr std::uint16_t kSblrOperatorCallOpcodeCode=1025;inline constexpr const char* kSblrOperatorCallOpcodeVersion="1.0";inline constexpr const char* kSblrOperatorCallOperandDescriptorId="operator_call_descriptor";inline constexpr const char* kSblrOperatorCallResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrOperatorCallResultDescriptorVersion=1;
inline constexpr const char* kSblrCastExecutorId="engine.op.cast";inline constexpr std::uint16_t kSblrCastOpcodeCode=1026;inline constexpr const char* kSblrCastOpcodeVersion="1.0";inline constexpr const char* kSblrCastOperandDescriptorId="cast_descriptor";inline constexpr const char* kSblrCastResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrCastResultDescriptorVersion=1;
inline constexpr const char* kSblrCompareExecutorId="engine.op.compare";inline constexpr std::uint16_t kSblrCompareOpcodeCode=1027;inline constexpr const char* kSblrCompareOpcodeVersion="1.0";inline constexpr const char* kSblrCompareOperandDescriptorId="comparison_descriptor";inline constexpr const char* kSblrCompareResultDescriptorId="boolean_value";inline constexpr std::uint16_t kSblrCompareResultDescriptorVersion=1;
inline constexpr const char* kSblrDomainOperationExecutorId="engine.op.domain_operation";inline constexpr std::uint16_t kSblrDomainOperationOpcodeCode=1028;inline constexpr const char* kSblrDomainOperationOpcodeVersion="1.0";inline constexpr const char* kSblrDomainOperationOperandDescriptorId="domain_operation_descriptor";inline constexpr const char* kSblrDomainOperationResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrDomainOperationResultDescriptorVersion=1;
inline constexpr const char* kSblrUdrInvokeExecutorId="engine.op.udr_invoke";inline constexpr std::uint16_t kSblrUdrInvokeOpcodeCode=1029;inline constexpr const char* kSblrUdrInvokeOpcodeVersion="1.0";inline constexpr const char* kSblrUdrInvokeOperandDescriptorId="registered_cpp_udr_invocation";inline constexpr const char* kSblrUdrInvokeResultDescriptorId="typed_value_or_result_set";inline constexpr std::uint16_t kSblrUdrInvokeResultDescriptorVersion=1;
inline constexpr const char* kSblrProcedureInvokeExecutorId="engine.op.procedure_invoke";inline constexpr std::uint16_t kSblrProcedureInvokeOpcodeCode=1030;inline constexpr const char* kSblrProcedureInvokeOpcodeVersion="1.0";inline constexpr const char* kSblrProcedureInvokeOperandDescriptorId="procedure_invoke_descriptor";inline constexpr const char* kSblrProcedureInvokeResultDescriptorId="procedure_result";inline constexpr std::uint16_t kSblrProcedureInvokeResultDescriptorVersion=1;
inline constexpr const char* kSblrFunctionInvokeExecutorId="engine.op.function_invoke";inline constexpr std::uint16_t kSblrFunctionInvokeOpcodeCode=1031;inline constexpr const char* kSblrFunctionInvokeOpcodeVersion="1.0";inline constexpr const char* kSblrFunctionInvokeOperandDescriptorId="function_invoke_descriptor";inline constexpr const char* kSblrFunctionInvokeResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrFunctionInvokeResultDescriptorVersion=1;
inline constexpr const char* kSblrAggregateInvokeExecutorId="engine.op.aggregate_invoke";inline constexpr std::uint16_t kSblrAggregateInvokeOpcodeCode=1032;inline constexpr const char* kSblrAggregateInvokeOpcodeVersion="1.0";inline constexpr const char* kSblrAggregateInvokeOperandDescriptorId="aggregate_invoke_descriptor";inline constexpr const char* kSblrAggregateInvokeResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrAggregateInvokeResultDescriptorVersion=1;
inline constexpr const char* kSblrSequenceNextvalExecutorId="engine.op.sequence_nextval";inline constexpr std::uint16_t kSblrSequenceNextvalOpcodeCode=1033;inline constexpr const char* kSblrSequenceNextvalOpcodeVersion="1.0";inline constexpr const char* kSblrSequenceNextvalOperandDescriptorId="sequence_nextval_descriptor";inline constexpr const char* kSblrSequenceNextvalResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrSequenceNextvalResultDescriptorVersion=1;
inline constexpr const char* kSblrSequenceCurrvalExecutorId="engine.op.sequence_currval";inline constexpr std::uint16_t kSblrSequenceCurrvalOpcodeCode=1034;inline constexpr const char* kSblrSequenceCurrvalOpcodeVersion="1.0";inline constexpr const char* kSblrSequenceCurrvalOperandDescriptorId="sequence_currval_descriptor";inline constexpr const char* kSblrSequenceCurrvalResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrSequenceCurrvalResultDescriptorVersion=1;
inline constexpr const char* kSblrSequenceSetvalExecutorId="engine.op.sequence_setval";inline constexpr std::uint16_t kSblrSequenceSetvalOpcodeCode=1035;inline constexpr const char* kSblrSequenceSetvalOpcodeVersion="1.0";inline constexpr const char* kSblrSequenceSetvalOperandDescriptorId="sequence_setval_descriptor";inline constexpr const char* kSblrSequenceSetvalResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrSequenceSetvalResultDescriptorVersion=1;
inline constexpr const char* kSblrQueryNumericExecutorId="engine.op.query_apply_numeric_operation";inline constexpr std::uint16_t kSblrQueryNumericOpcodeCode=1036;inline constexpr const char* kSblrQueryNumericOpcodeVersion="1.0";inline constexpr const char* kSblrQueryNumericOperandDescriptorId="numeric_descriptor_and_operand_values";inline constexpr const char* kSblrQueryNumericResultDescriptorId="typed_value";inline constexpr std::uint16_t kSblrQueryNumericResultDescriptorVersion=1;
inline constexpr const char* kSblrAdvancedDatatypeFamilyExecutorId="engine.op.query_evaluate_advanced_datatype_family";inline constexpr std::uint16_t kSblrAdvancedDatatypeFamilyOpcodeCode=1037;inline constexpr const char* kSblrAdvancedDatatypeFamilyOpcodeVersion="1.0";inline constexpr const char* kSblrAdvancedDatatypeFamilyOperandDescriptorId="advanced_family_descriptor_operation_index_profile";inline constexpr const char* kSblrAdvancedDatatypeFamilyResultDescriptorId="datatype_family_evaluation";inline constexpr std::uint16_t kSblrAdvancedDatatypeFamilyResultDescriptorVersion=1;
inline constexpr const char* kSblrProjectExecutorId="engine.op.project";inline constexpr std::uint16_t kSblrProjectOpcodeCode=1280;inline constexpr const char* kSblrProjectOpcodeVersion="1.0";inline constexpr const char* kSblrProjectOperandDescriptorId="projection_descriptor";inline constexpr const char* kSblrProjectResultDescriptorId="rowset_descriptor";inline constexpr std::uint16_t kSblrProjectResultDescriptorVersion=1;
inline constexpr const char* kSblrAggregateExecutorId="engine.op.aggregate";inline constexpr std::uint16_t kSblrAggregateOpcodeCode=1281;inline constexpr const char* kSblrAggregateOpcodeVersion="1.0";inline constexpr const char* kSblrAggregateOperandDescriptorId="aggregate_descriptor";inline constexpr const char* kSblrAggregateResultDescriptorId="rowset_descriptor";inline constexpr std::uint16_t kSblrAggregateResultDescriptorVersion=1;
inline constexpr const char* kSblrGroupExecutorId="engine.op.group";inline constexpr std::uint16_t kSblrGroupOpcodeCode=1282;inline constexpr const char* kSblrGroupOpcodeVersion="1.0";inline constexpr const char* kSblrGroupOperandDescriptorId="group_descriptor";inline constexpr const char* kSblrGroupResultDescriptorId="rowset_descriptor";inline constexpr std::uint16_t kSblrGroupResultDescriptorVersion=1;
inline constexpr const char* kSblrSortExecutorId="engine.op.sort";inline constexpr std::uint16_t kSblrSortOpcodeCode=1283;inline constexpr const char* kSblrSortOpcodeVersion="1.0";inline constexpr const char* kSblrSortOperandDescriptorId="sort_descriptor";inline constexpr const char* kSblrSortResultDescriptorId="rowset_descriptor";inline constexpr std::uint16_t kSblrSortResultDescriptorVersion=1;
inline constexpr const char* kSblrLimitExecutorId="engine.op.limit";inline constexpr std::uint16_t kSblrLimitOpcodeCode=1284;inline constexpr const char* kSblrLimitOpcodeVersion="1.0";inline constexpr const char* kSblrLimitOperandDescriptorId="limit_descriptor";inline constexpr const char* kSblrLimitResultDescriptorId="rowset_descriptor";inline constexpr std::uint16_t kSblrLimitResultDescriptorVersion=1;
inline constexpr const char* kSblrWindowExecutorId="engine.op.window";inline constexpr std::uint16_t kSblrWindowOpcodeCode=1285;inline constexpr const char* kSblrWindowOpcodeVersion="1.0";inline constexpr const char* kSblrWindowOperandDescriptorId="window_descriptor";inline constexpr const char* kSblrWindowResultDescriptorId="rowset_descriptor";inline constexpr std::uint16_t kSblrWindowResultDescriptorVersion=1;
inline constexpr const char* kSblrReturnResultSetExecutorId="engine.op.return_result_set";inline constexpr std::uint16_t kSblrReturnResultSetOpcodeCode=1286;inline constexpr const char* kSblrReturnResultSetOpcodeVersion="1.0";inline constexpr const char* kSblrReturnResultSetOperandDescriptorId="result_set_return_descriptor";inline constexpr const char* kSblrReturnResultSetResultDescriptorId="result_set_handle";inline constexpr std::uint16_t kSblrReturnResultSetResultDescriptorVersion=1;
inline constexpr const char* kSblrKvStructuredReadExecutorId="engine.op.kv_structured_read";inline constexpr std::uint16_t kSblrKvStructuredReadOpcodeCode=8192;inline constexpr const char* kSblrKvStructuredReadOpcodeVersion="1.0";inline constexpr const char* kSblrKvStructuredReadOperandDescriptorId="kv_structured_read_descriptor";inline constexpr const char* kSblrKvStructuredReadResultDescriptorId="kv_structured_result";inline constexpr std::uint16_t kSblrKvStructuredReadResultDescriptorVersion=1;
inline constexpr const char* kSblrKvStructuredMutateExecutorId="engine.op.kv_structured_mutate";inline constexpr std::uint16_t kSblrKvStructuredMutateOpcodeCode=8193;inline constexpr const char* kSblrKvStructuredMutateOpcodeVersion="1.0";inline constexpr const char* kSblrKvStructuredMutateOperandDescriptorId="kv_structured_mutate_descriptor";inline constexpr const char* kSblrKvStructuredMutateResultDescriptorId="kv_structured_result";inline constexpr std::uint16_t kSblrKvStructuredMutateResultDescriptorVersion=1;
inline constexpr const char* kSblrKvStructuredScanExecutorId="engine.op.kv_structured_scan";inline constexpr std::uint16_t kSblrKvStructuredScanOpcodeCode=8194;inline constexpr const char* kSblrKvStructuredScanOpcodeVersion="1.0";inline constexpr const char* kSblrKvStructuredScanOperandDescriptorId="kv_structured_scan_descriptor";inline constexpr const char* kSblrKvStructuredScanResultDescriptorId="kv_structured_result";inline constexpr std::uint16_t kSblrKvStructuredScanResultDescriptorVersion=1;
inline constexpr const char* kSblrKvStructuredStreamReadExecutorId="engine.op.kv_structured_stream_read";inline constexpr std::uint16_t kSblrKvStructuredStreamReadOpcodeCode=8195;inline constexpr const char* kSblrKvStructuredStreamReadOpcodeVersion="1.0";inline constexpr const char* kSblrKvStructuredStreamReadOperandDescriptorId="kv_structured_stream_read_descriptor";inline constexpr const char* kSblrKvStructuredStreamReadResultDescriptorId="kv_structured_result";inline constexpr std::uint16_t kSblrKvStructuredStreamReadResultDescriptorVersion=1;
inline constexpr const char* kSblrKvStructuredStreamAppendExecutorId="engine.op.kv_structured_stream_append";inline constexpr std::uint16_t kSblrKvStructuredStreamAppendOpcodeCode=8196;inline constexpr const char* kSblrKvStructuredStreamAppendOpcodeVersion="1.0";inline constexpr const char* kSblrKvStructuredStreamAppendOperandDescriptorId="kv_structured_stream_append_descriptor";inline constexpr const char* kSblrKvStructuredStreamAppendResultDescriptorId="kv_structured_mutation_result";inline constexpr std::uint16_t kSblrKvStructuredStreamAppendResultDescriptorVersion=1;
inline constexpr const char* kSblrKvStructuredTimeseriesExecutorId="engine.op.kv_structured_timeseries";inline constexpr std::uint16_t kSblrKvStructuredTimeseriesOpcodeCode=8197;inline constexpr const char* kSblrKvStructuredTimeseriesOpcodeVersion="1.0";inline constexpr const char* kSblrKvStructuredTimeseriesOperandDescriptorId="kv_timeseries_descriptor";inline constexpr const char* kSblrKvStructuredTimeseriesResultDescriptorId="kv_structured_result";inline constexpr std::uint16_t kSblrKvStructuredTimeseriesResultDescriptorVersion=1;
inline constexpr const char* kSblrSystemConfigSetExecutorId="engine.op.system_config_set";inline constexpr std::uint16_t kSblrSystemConfigSetOpcodeCode=5125;inline constexpr const char* kSblrSystemConfigSetOpcodeVersion="1.0";inline constexpr const char* kSblrSystemConfigSetOperandDescriptorId="system_config_set_descriptor";inline constexpr const char* kSblrSystemConfigSetResultDescriptorId="management_result";inline constexpr std::uint16_t kSblrSystemConfigSetResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateDomainExecutorId="engine.op.ddl_create_domain";inline constexpr std::uint16_t kSblrDdlCreateDomainOpcodeCode=1542;inline constexpr const char* kSblrDdlCreateDomainOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateDomainOperandDescriptorId="create_domain_descriptor";inline constexpr const char* kSblrDdlCreateDomainResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateDomainResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateSchemaExecutorId="engine.op.ddl_create_schema";inline constexpr std::uint16_t kSblrDdlCreateSchemaOpcodeCode=1536;inline constexpr const char* kSblrDdlCreateSchemaOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateSchemaOperandDescriptorId="create_schema_descriptor";inline constexpr const char* kSblrDdlCreateSchemaResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateSchemaResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateTableExecutorId="engine.op.ddl_create_table";inline constexpr std::uint16_t kSblrDdlCreateTableOpcodeCode=1537;inline constexpr const char* kSblrDdlCreateTableOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateTableOperandDescriptorId="create_table_descriptor";inline constexpr const char* kSblrDdlCreateTableResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateTableResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateIndexExecutorId="engine.op.ddl_create_index";inline constexpr std::uint16_t kSblrDdlCreateIndexOpcodeCode=1540;inline constexpr const char* kSblrDdlCreateIndexOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateIndexOperandDescriptorId="create_index_descriptor";inline constexpr const char* kSblrDdlCreateIndexResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateIndexResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropIndexExecutorId="engine.op.ddl_drop_index";inline constexpr std::uint16_t kSblrDdlDropIndexOpcodeCode=1541;inline constexpr const char* kSblrDdlDropIndexOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropIndexOperandDescriptorId="drop_index_descriptor";inline constexpr const char* kSblrDdlDropIndexResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropIndexResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlAlterDomainExecutorId="engine.op.ddl_alter_domain";inline constexpr std::uint16_t kSblrDdlAlterDomainOpcodeCode=1547;inline constexpr const char* kSblrDdlAlterDomainOpcodeVersion="1.0";inline constexpr const char* kSblrDdlAlterDomainOperandDescriptorId="alter_domain_descriptor";inline constexpr const char* kSblrDdlAlterDomainResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlAlterDomainResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateViewExecutorId="engine.op.ddl_create_view";inline constexpr std::uint16_t kSblrDdlCreateViewOpcodeCode=1548;inline constexpr const char* kSblrDdlCreateViewOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateViewOperandDescriptorId="create_view_descriptor";inline constexpr const char* kSblrDdlCreateViewResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateViewResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlAlterViewExecutorId="engine.op.ddl_alter_view";inline constexpr std::uint16_t kSblrDdlAlterViewOpcodeCode=1549;inline constexpr const char* kSblrDdlAlterViewOpcodeVersion="1.0";inline constexpr const char* kSblrDdlAlterViewOperandDescriptorId="alter_view_descriptor";inline constexpr const char* kSblrDdlAlterViewResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlAlterViewResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateTriggerExecutorId="engine.op.ddl_create_trigger";inline constexpr std::uint16_t kSblrDdlCreateTriggerOpcodeCode=1551;inline constexpr const char* kSblrDdlCreateTriggerOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateTriggerOperandDescriptorId="create_trigger_descriptor";inline constexpr const char* kSblrDdlCreateTriggerResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateTriggerResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlAlterTriggerExecutorId="engine.op.ddl_alter_trigger";inline constexpr std::uint16_t kSblrDdlAlterTriggerOpcodeCode=1552;inline constexpr const char* kSblrDdlAlterTriggerOpcodeVersion="1.0";inline constexpr const char* kSblrDdlAlterTriggerOperandDescriptorId="alter_trigger_descriptor";inline constexpr const char* kSblrDdlAlterTriggerResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlAlterTriggerResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropTriggerExecutorId="engine.op.ddl_drop_trigger";inline constexpr std::uint16_t kSblrDdlDropTriggerOpcodeCode=1553;inline constexpr const char* kSblrDdlDropTriggerOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropTriggerOperandDescriptorId="drop_trigger_descriptor";inline constexpr const char* kSblrDdlDropTriggerResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropTriggerResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateProcedureExecutorId="engine.op.ddl_create_procedure";inline constexpr std::uint16_t kSblrDdlCreateProcedureOpcodeCode=1554;inline constexpr const char* kSblrDdlCreateProcedureOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateProcedureOperandDescriptorId="create_procedure_descriptor";inline constexpr const char* kSblrDdlCreateProcedureResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateProcedureResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlAlterProcedureExecutorId="engine.op.ddl_alter_procedure";inline constexpr std::uint16_t kSblrDdlAlterProcedureOpcodeCode=1555;inline constexpr const char* kSblrDdlAlterProcedureOpcodeVersion="1.0";inline constexpr const char* kSblrDdlAlterProcedureOperandDescriptorId="alter_procedure_descriptor";inline constexpr const char* kSblrDdlAlterProcedureResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlAlterProcedureResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropProcedureExecutorId="engine.op.ddl_drop_procedure";inline constexpr std::uint16_t kSblrDdlDropProcedureOpcodeCode=1556;inline constexpr const char* kSblrDdlDropProcedureOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropProcedureOperandDescriptorId="drop_procedure_descriptor";inline constexpr const char* kSblrDdlDropProcedureResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropProcedureResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateFunctionExecutorId="engine.op.ddl_create_function";inline constexpr std::uint16_t kSblrDdlCreateFunctionOpcodeCode=1557;inline constexpr const char* kSblrDdlCreateFunctionOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateFunctionOperandDescriptorId="create_function_descriptor";inline constexpr const char* kSblrDdlCreateFunctionResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateFunctionResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlAlterFunctionExecutorId="engine.op.ddl_alter_function";inline constexpr std::uint16_t kSblrDdlAlterFunctionOpcodeCode=1558;inline constexpr const char* kSblrDdlAlterFunctionOpcodeVersion="1.0";inline constexpr const char* kSblrDdlAlterFunctionOperandDescriptorId="alter_function_descriptor";inline constexpr const char* kSblrDdlAlterFunctionResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlAlterFunctionResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropFunctionExecutorId="engine.op.ddl_drop_function";inline constexpr std::uint16_t kSblrDdlDropFunctionOpcodeCode=1559;inline constexpr const char* kSblrDdlDropFunctionOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropFunctionOperandDescriptorId="drop_function_descriptor";inline constexpr const char* kSblrDdlDropFunctionResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropFunctionResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreatePackageExecutorId="engine.op.ddl_create_package";inline constexpr std::uint16_t kSblrDdlCreatePackageOpcodeCode=1560;inline constexpr const char* kSblrDdlCreatePackageOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreatePackageOperandDescriptorId="create_package_descriptor";inline constexpr const char* kSblrDdlCreatePackageResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreatePackageResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropTemporaryTableExecutorId="engine.op.ddl_drop_temporary_table";inline constexpr std::uint16_t kSblrDdlDropTemporaryTableOpcodeCode=1562;inline constexpr const char* kSblrDdlDropTemporaryTableOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropTemporaryTableOperandDescriptorId="drop_temporary_table_descriptor";inline constexpr const char* kSblrDdlDropTemporaryTableResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropTemporaryTableResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateTemporaryTableExecutorId="engine.op.ddl_create_temporary_table";inline constexpr std::uint16_t kSblrDdlCreateTemporaryTableOpcodeCode=1561;inline constexpr const char* kSblrDdlCreateTemporaryTableOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateTemporaryTableOperandDescriptorId="create_temporary_table_descriptor";inline constexpr const char* kSblrDdlCreateTemporaryTableResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateTemporaryTableResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlRenameObjectVectorExecutorId="engine.op.ddl_rename_object_vector";inline constexpr std::uint16_t kSblrDdlRenameObjectVectorOpcodeCode=1563;inline constexpr const char* kSblrDdlRenameObjectVectorOpcodeVersion="1.0";inline constexpr const char* kSblrDdlRenameObjectVectorOperandDescriptorId="object_rename_vector_descriptor";inline constexpr const char* kSblrDdlRenameObjectVectorResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlRenameObjectVectorResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateOrReplaceSrsExecutorId="engine.op.ddl_create_or_replace_srs";inline constexpr std::uint16_t kSblrDdlCreateOrReplaceSrsOpcodeCode=1615;inline constexpr const char* kSblrDdlCreateOrReplaceSrsOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateOrReplaceSrsOperandDescriptorId="spatial_reference_system_descriptor";inline constexpr const char* kSblrDdlCreateOrReplaceSrsResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateOrReplaceSrsResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropSrsExecutorId="engine.op.ddl_drop_srs";inline constexpr std::uint16_t kSblrDdlDropSrsOpcodeCode=1616;inline constexpr const char* kSblrDdlDropSrsOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropSrsOperandDescriptorId="spatial_reference_system_drop_descriptor";inline constexpr const char* kSblrDdlDropSrsResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropSrsResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateRewriteRuleExecutorId="engine.op.ddl_create_rewrite_rule";inline constexpr std::uint16_t kSblrDdlCreateRewriteRuleOpcodeCode=1617;inline constexpr const char* kSblrDdlCreateRewriteRuleOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateRewriteRuleOperandDescriptorId="rewrite_rule_descriptor";inline constexpr const char* kSblrDdlCreateRewriteRuleResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateRewriteRuleResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlAlterRewriteRuleExecutorId="engine.op.ddl_alter_rewrite_rule";inline constexpr std::uint16_t kSblrDdlAlterRewriteRuleOpcodeCode=1618;inline constexpr const char* kSblrDdlAlterRewriteRuleOpcodeVersion="1.0";inline constexpr const char* kSblrDdlAlterRewriteRuleOperandDescriptorId="rewrite_rule_alter_descriptor";inline constexpr const char* kSblrDdlAlterRewriteRuleResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlAlterRewriteRuleResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropRewriteRuleExecutorId="engine.op.ddl_drop_rewrite_rule";inline constexpr std::uint16_t kSblrDdlDropRewriteRuleOpcodeCode=1619;inline constexpr const char* kSblrDdlDropRewriteRuleOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropRewriteRuleOperandDescriptorId="rewrite_rule_drop_descriptor";inline constexpr const char* kSblrDdlDropRewriteRuleResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropRewriteRuleResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlValidateConstraintExecutorId="engine.op.ddl_validate_constraint";inline constexpr std::uint16_t kSblrDdlValidateConstraintOpcodeCode=1620;inline constexpr const char* kSblrDdlValidateConstraintOpcodeVersion="1.0";inline constexpr const char* kSblrDdlValidateConstraintOperandDescriptorId="constraint_validation_descriptor";inline constexpr const char* kSblrDdlValidateConstraintResultDescriptorId="management_operation_result";inline constexpr std::uint16_t kSblrDdlValidateConstraintResultDescriptorVersion=1;
inline constexpr const char* kSblrSecurityCreatePrivilegeTemplateExecutorId="engine.op.security_create_privilege_template";inline constexpr std::uint16_t kSblrSecurityCreatePrivilegeTemplateOpcodeCode=1621;inline constexpr const char* kSblrSecurityCreatePrivilegeTemplateOpcodeVersion="1.0";inline constexpr const char* kSblrSecurityCreatePrivilegeTemplateOperandDescriptorId="privilege_template_descriptor";inline constexpr const char* kSblrSecurityCreatePrivilegeTemplateResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrSecurityCreatePrivilegeTemplateResultDescriptorVersion=1;
inline constexpr const char* kSblrSecurityAlterPrivilegeTemplateExecutorId="engine.op.security_alter_privilege_template";inline constexpr std::uint16_t kSblrSecurityAlterPrivilegeTemplateOpcodeCode=1622;inline constexpr const char* kSblrSecurityAlterPrivilegeTemplateOpcodeVersion="1.0";inline constexpr const char* kSblrSecurityAlterPrivilegeTemplateOperandDescriptorId="privilege_template_alter_descriptor";inline constexpr const char* kSblrSecurityAlterPrivilegeTemplateResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrSecurityAlterPrivilegeTemplateResultDescriptorVersion=1;
inline constexpr const char* kSblrSecurityDropPrivilegeTemplateExecutorId="engine.op.security_drop_privilege_template";inline constexpr std::uint16_t kSblrSecurityDropPrivilegeTemplateOpcodeCode=1623;inline constexpr const char* kSblrSecurityDropPrivilegeTemplateOpcodeVersion="1.0";inline constexpr const char* kSblrSecurityDropPrivilegeTemplateOperandDescriptorId="privilege_template_drop_descriptor";inline constexpr const char* kSblrSecurityDropPrivilegeTemplateResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrSecurityDropPrivilegeTemplateResultDescriptorVersion=1;
inline constexpr const char* kSblrDatabaseCreateTemplateCloneExecutorId="engine.op.database_create_template_clone";inline constexpr std::uint16_t kSblrDatabaseCreateTemplateCloneOpcodeCode=1624;inline constexpr const char* kSblrDatabaseCreateTemplateCloneOpcodeVersion="1.0";inline constexpr const char* kSblrDatabaseCreateTemplateCloneOperandDescriptorId="template_database_creation_descriptor";inline constexpr const char* kSblrDatabaseCreateTemplateCloneResultDescriptorId="management_operation_result";inline constexpr std::uint16_t kSblrDatabaseCreateTemplateCloneResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlCreateAggregateExecutorId="engine.op.ddl_create_aggregate";inline constexpr std::uint16_t kSblrDdlCreateAggregateOpcodeCode=1625;inline constexpr const char* kSblrDdlCreateAggregateOpcodeVersion="1.0";inline constexpr const char* kSblrDdlCreateAggregateOperandDescriptorId="aggregate_descriptor";inline constexpr const char* kSblrDdlCreateAggregateResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlCreateAggregateResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlAlterAggregateExecutorId="engine.op.ddl_alter_aggregate";inline constexpr std::uint16_t kSblrDdlAlterAggregateOpcodeCode=1626;inline constexpr const char* kSblrDdlAlterAggregateOpcodeVersion="1.0";inline constexpr const char* kSblrDdlAlterAggregateOperandDescriptorId="aggregate_alter_descriptor";inline constexpr const char* kSblrDdlAlterAggregateResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlAlterAggregateResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropAggregateExecutorId="engine.op.ddl_drop_aggregate";inline constexpr std::uint16_t kSblrDdlDropAggregateOpcodeCode=1627;inline constexpr const char* kSblrDdlDropAggregateOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropAggregateOperandDescriptorId="aggregate_drop_descriptor";inline constexpr const char* kSblrDdlDropAggregateResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropAggregateResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlPurgeSystemHistoryExecutorId="engine.op.ddl_purge_system_history";inline constexpr std::uint16_t kSblrDdlPurgeSystemHistoryOpcodeCode=1628;inline constexpr const char* kSblrDdlPurgeSystemHistoryOpcodeVersion="1.0";inline constexpr const char* kSblrDdlPurgeSystemHistoryOperandDescriptorId="system_history_purge_descriptor";inline constexpr const char* kSblrDdlPurgeSystemHistoryResultDescriptorId="management_operation_result";inline constexpr std::uint16_t kSblrDdlPurgeSystemHistoryResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlSetIndexOptimizerEligibilityExecutorId="engine.op.ddl_set_index_optimizer_eligibility";inline constexpr std::uint16_t kSblrDdlSetIndexOptimizerEligibilityOpcodeCode=1629;inline constexpr const char* kSblrDdlSetIndexOptimizerEligibilityOpcodeVersion="1.0";inline constexpr const char* kSblrDdlSetIndexOptimizerEligibilityOperandDescriptorId="index_optimizer_eligibility_descriptor";inline constexpr const char* kSblrDdlSetIndexOptimizerEligibilityResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlSetIndexOptimizerEligibilityResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlSetTableTypeEnforcementExecutorId="engine.op.ddl_set_table_type_enforcement";inline constexpr std::uint16_t kSblrDdlSetTableTypeEnforcementOpcodeCode=1630;inline constexpr const char* kSblrDdlSetTableTypeEnforcementOpcodeVersion="1.0";inline constexpr const char* kSblrDdlSetTableTypeEnforcementOperandDescriptorId="table_type_enforcement_descriptor";inline constexpr const char* kSblrDdlSetTableTypeEnforcementResultDescriptorId="management_operation_result";inline constexpr std::uint16_t kSblrDdlSetTableTypeEnforcementResultDescriptorVersion=1;
inline constexpr const char* kSblrDdlDropViewExecutorId="engine.op.ddl_drop_view";inline constexpr std::uint16_t kSblrDdlDropViewOpcodeCode=1550;inline constexpr const char* kSblrDdlDropViewOpcodeVersion="1.0";inline constexpr const char* kSblrDdlDropViewOperandDescriptorId="drop_view_descriptor";inline constexpr const char* kSblrDdlDropViewResultDescriptorId="ddl_result";inline constexpr std::uint16_t kSblrDdlDropViewResultDescriptorVersion=1;

enum class SblrExecutorAvailabilityState : std::uint8_t {
  installed = 1,
  revoked = 2,
  unavailable = 3,
};

struct SblrExecutorAvailabilityRowIdentity {
  std::string executor_id{kSblrLiteralExecutorId};
  std::uint16_t opcode_code{kSblrLiteralOpcodeCode};
  std::string opcode_version{kSblrLiteralOpcodeVersion};
  std::string operand_descriptor_id{kSblrLiteralOperandDescriptorId};
  std::string result_descriptor_id{kSblrLiteralResultDescriptorId};
  std::uint16_t result_descriptor_version{kSblrLiteralResultDescriptorVersion};
};

struct SblrExecutorAvailabilitySnapshot {
  std::string snapshot_uuid;
  std::uint64_t generation{0};
  std::string database_uuid;
  std::string row_identity_sha256;
  bool installed{false};
  SblrExecutorAvailabilityState availability_state{
      SblrExecutorAvailabilityState::unavailable};
  std::string decision_evidence_sha256;
};

struct SblrExecutorAvailabilityLoadResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrExecutorAvailabilitySnapshot snapshot;
};

struct SblrExecutorAvailabilitySetRequest {
  std::string database_uuid;
  std::string expected_snapshot_uuid;
  std::uint64_t expected_generation{0};
  SblrExecutorAvailabilityRowIdentity exact_row_identity;
  SblrExecutorAvailabilityState requested_state{
      SblrExecutorAvailabilityState::unavailable};
  std::string reason_code;
};

struct SblrExecutorAvailabilitySetResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrExecutorAvailabilitySnapshot snapshot;
  std::vector<EngineEvidenceReference> evidence;
};

// Loads one immutable owning-database snapshot. A genuinely absent store is
// initialized with the exact admitted literal row; any present but incomplete,
// torn, or contradictory store fails closed.
SblrExecutorAvailabilityLoadResult LoadSblrExecutorAvailabilitySnapshot(
    const EngineRequestContext& context);
SblrExecutorAvailabilityLoadResult LoadSblrExecutorAvailabilitySnapshot(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& exact_row_identity);

// engine.sblr_executor_availability_registry.set.v1
SblrExecutorAvailabilitySetResult SetSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilitySetRequest& request);

// Exact dispatch-time check. The supplied token snapshot is immutable
// admission evidence; this call reloads current durable authority.
EngineApiDiagnostic RevalidateSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilitySnapshot& admitted_snapshot,
    SblrExecutorAvailabilitySnapshot* current_snapshot);
EngineApiDiagnostic RevalidateSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& exact_row_identity,
    const SblrExecutorAvailabilitySnapshot& admitted_snapshot,
    SblrExecutorAvailabilitySnapshot* current_snapshot);

std::string ComputeSblrExecutorAvailabilityRowIdentitySha256(
    const SblrExecutorAvailabilityRowIdentity& identity);

}  // namespace scratchbird::engine::internal_api
