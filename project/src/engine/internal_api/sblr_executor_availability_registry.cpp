#include "sblr_executor_availability_registry.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::string_view kMagic = "SBEXAV1";
constexpr std::string_view kRegistryId =
    "engine.sblr_executor_availability_registry.v1";

std::recursive_mutex& RegistryMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

std::string StorePath(const EngineRequestContext& context,
                      const SblrExecutorAvailabilityRowIdentity& identity) {
  const auto suffix = identity.executor_id == kSblrParameterExecutorId ? ".parameter" :
      identity.executor_id == kSblrVariableExecutorId ? ".variable" :
      identity.executor_id == kSblrSourceMapExecutorId ? ".source_map" :
      identity.executor_id == kSblrErrorVectorExecutorId ? ".error_vector" :
      identity.executor_id == kSblrTxnBeginExecutorId ? ".txn_begin" :
      identity.executor_id == kSblrTxnCommitExecutorId ? ".txn_commit" : "";
  const auto final_suffix = identity.executor_id == kSblrTxnRollbackExecutorId
      ? ".txn_rollback" : identity.executor_id == kSblrTxnSavepointExecutorId
      ? ".txn_savepoint" : identity.executor_id == kSblrTxnReleaseSavepointExecutorId
      ? ".txn_release_savepoint" : suffix;
  const auto routed_suffix = identity.executor_id == kSblrTxnRollbackToSavepointExecutorId
      ? ".txn_rollback_to_savepoint" : final_suffix;
  const auto autonomous_suffix = identity.executor_id == kSblrPsqlAutonomousFrameExecutorId
      ? ".psql_autonomous_frame" : routed_suffix;
  const auto reservation_release_suffix = identity.executor_id == kSblrReservationReleaseExecutorId
      ? ".transaction_reservation_release" : autonomous_suffix;
  const auto temporary_cleanup_suffix = identity.executor_id == kSblrTemporaryInstanceCleanupExecutorId
      ? ".temporary_instance_cleanup" : reservation_release_suffix;
  const auto cursor_open_suffix = identity.executor_id == kSblrCursorOpenExecutorId
      ? ".cursor_open" : temporary_cleanup_suffix;
  const auto cursor_fetch_suffix = identity.executor_id == kSblrCursorFetchExecutorId
      ? ".cursor_fetch" : cursor_open_suffix;
  const auto cursor_close_suffix = identity.executor_id == kSblrCursorCloseExecutorId
      ? ".cursor_close" : cursor_fetch_suffix;
  const auto read_by_key_suffix = identity.executor_id == kSblrReadByKeyExecutorId
      ? ".read_by_key" : cursor_close_suffix;
  const auto read_range_suffix = identity.executor_id == kSblrReadRangeExecutorId
      ? ".read_range" : read_by_key_suffix;
  const auto read_stream_suffix = identity.executor_id == kSblrReadStreamExecutorId
      ? ".read_stream" : read_range_suffix;
  const auto result_set_pass_suffix = identity.executor_id == kSblrResultSetPassExecutorId
      ? ".result_set_pass" : read_stream_suffix;
  const auto access_cursor_open_suffix = identity.executor_id == kSblrAccessCursorOpenExecutorId
      ? ".access_cursor_open" : result_set_pass_suffix;
  const auto access_cursor_fetch_suffix = identity.executor_id == kSblrAccessCursorFetchExecutorId
      ? ".access_cursor_fetch" : access_cursor_open_suffix;
  const auto access_cursor_close_suffix = identity.executor_id == kSblrAccessCursorCloseExecutorId
      ? ".access_cursor_close" : access_cursor_fetch_suffix;
  const auto insert_suffix = identity.executor_id == kSblrInsertExecutorId
      ? ".insert" : access_cursor_close_suffix;
  const auto update_suffix = identity.executor_id == kSblrUpdateExecutorId ? ".update" : insert_suffix;
  const auto delete_suffix = identity.executor_id == kSblrDeleteExecutorId ? ".delete" : update_suffix;
  const auto merge_suffix = identity.executor_id == kSblrMergeExecutorId ? ".merge" : delete_suffix;
  const auto table_truncate_suffix = identity.executor_id == kSblrTableTruncateExecutorId ? ".table_truncate" : merge_suffix;
  const auto table_analyze_suffix = identity.executor_id == kSblrTableAnalyzeExecutorId ? ".table_analyze" : table_truncate_suffix;
  const auto bulk_import_stream_suffix = identity.executor_id == kSblrBulkImportStreamExecutorId ? ".bulk_import_stream" : table_analyze_suffix;
  const auto bulk_export_stream_suffix = identity.executor_id == kSblrBulkExportStreamExecutorId ? ".bulk_export_stream" : bulk_import_stream_suffix;
  const auto statement_batch_suffix = identity.executor_id == kSblrStatementBatchExecutorId ? ".statement_batch" : bulk_export_stream_suffix;
  const auto atomic_cas_suffix = identity.executor_id == kSblrAtomicCasExecutorId ? ".atomic_cas" : statement_batch_suffix;
  const auto atomic_rmw_suffix = identity.executor_id == kSblrAtomicRmwExecutorId ? ".atomic_rmw" : atomic_cas_suffix;
  const auto advisory_lock_suffix = identity.executor_id == kSblrAdvisoryLockAcquireExecutorId ? ".advisory_lock_acquire" : atomic_rmw_suffix;
  const auto advisory_lock_release_suffix = identity.executor_id == kSblrAdvisoryLockReleaseExecutorId ? ".advisory_lock_release" : advisory_lock_suffix;
  const auto function_call_suffix = identity.executor_id == kSblrFunctionCallExecutorId ? ".function_call" : advisory_lock_release_suffix;
  const auto operator_call_suffix = identity.executor_id == kSblrOperatorCallExecutorId ? ".operator_call" : function_call_suffix;
  const auto cast_suffix = identity.executor_id == kSblrCastExecutorId ? ".cast" : operator_call_suffix;
  const auto compare_suffix = identity.executor_id == kSblrCompareExecutorId ? ".compare" : cast_suffix;
  const auto domain_operation_suffix = identity.executor_id == kSblrDomainOperationExecutorId ? ".domain_operation" : compare_suffix;
  const auto udr_invoke_suffix = identity.executor_id == kSblrUdrInvokeExecutorId ? ".udr_invoke" : domain_operation_suffix;
  const auto procedure_invoke_suffix = identity.executor_id == kSblrProcedureInvokeExecutorId ? ".procedure_invoke" : udr_invoke_suffix;
  const auto function_invoke_suffix = identity.executor_id == kSblrFunctionInvokeExecutorId ? ".function_invoke" : procedure_invoke_suffix;
  const auto aggregate_invoke_suffix = identity.executor_id == kSblrAggregateInvokeExecutorId ? ".aggregate_invoke" : function_invoke_suffix;
  const auto sequence_nextval_suffix = identity.executor_id == kSblrSequenceNextvalExecutorId ? ".sequence_nextval" : aggregate_invoke_suffix;
  const auto sequence_currval_suffix = identity.executor_id == kSblrSequenceCurrvalExecutorId ? ".sequence_currval" : sequence_nextval_suffix;
  const auto sequence_setval_suffix = identity.executor_id == kSblrSequenceSetvalExecutorId ? ".sequence_setval" : sequence_currval_suffix;
  const auto query_numeric_suffix = identity.executor_id == kSblrQueryNumericExecutorId ? ".query_numeric" : sequence_setval_suffix;
  const auto advanced_datatype_family_suffix = identity.executor_id == kSblrAdvancedDatatypeFamilyExecutorId ? ".advanced_datatype_family" : query_numeric_suffix;
  const auto project_suffix = identity.executor_id == kSblrProjectExecutorId ? ".project" : advanced_datatype_family_suffix;
  const auto aggregate_suffix = identity.executor_id == kSblrAggregateExecutorId ? ".aggregate" : project_suffix;
  const auto group_suffix = identity.executor_id == kSblrGroupExecutorId ? ".group" : aggregate_suffix;
  const auto sort_suffix = identity.executor_id == kSblrSortExecutorId ? ".sort" : group_suffix;
  const auto limit_suffix = identity.executor_id == kSblrLimitExecutorId ? ".limit" : sort_suffix;
  const auto window_suffix = identity.executor_id == kSblrWindowExecutorId ? ".window" : limit_suffix;
  const auto return_result_set_suffix = identity.executor_id == kSblrReturnResultSetExecutorId ? ".return_result_set" : window_suffix;
  const auto kv_structured_read_suffix = identity.executor_id == kSblrKvStructuredReadExecutorId ? ".kv_structured_read" : return_result_set_suffix;
  const auto kv_structured_mutate_suffix = identity.executor_id == kSblrKvStructuredMutateExecutorId ? ".kv_structured_mutate" : kv_structured_read_suffix;
  const auto kv_structured_scan_suffix = identity.executor_id == kSblrKvStructuredScanExecutorId ? ".kv_structured_scan" : kv_structured_mutate_suffix;
  const auto kv_structured_stream_read_suffix = identity.executor_id == kSblrKvStructuredStreamReadExecutorId ? ".kv_structured_stream_read" : kv_structured_scan_suffix;
  const auto kv_structured_stream_append_suffix = identity.executor_id == kSblrKvStructuredStreamAppendExecutorId ? ".kv_structured_stream_append" : kv_structured_stream_read_suffix;
  const auto kv_structured_timeseries_suffix = identity.executor_id == kSblrKvStructuredTimeseriesExecutorId ? ".kv_structured_timeseries" : kv_structured_stream_append_suffix;
  const auto system_config_set_suffix = identity.executor_id == kSblrSystemConfigSetExecutorId ? ".system_config_set" : kv_structured_timeseries_suffix;
  const auto ddl_create_domain_suffix = identity.executor_id == kSblrDdlCreateDomainExecutorId ? ".ddl_create_domain" : system_config_set_suffix;
  const auto ddl_create_schema_suffix = identity.executor_id == kSblrDdlCreateSchemaExecutorId ? ".ddl_create_schema" : ddl_create_domain_suffix;
  const auto ddl_create_table_suffix = identity.executor_id == kSblrDdlCreateTableExecutorId ? ".ddl_create_table" : ddl_create_schema_suffix;
  const auto ddl_create_index_suffix = identity.executor_id == kSblrDdlCreateIndexExecutorId ? ".ddl_create_index" : ddl_create_table_suffix;
  const auto ddl_drop_index_suffix = identity.executor_id == kSblrDdlDropIndexExecutorId ? ".ddl_drop_index" : ddl_create_index_suffix;
  const auto ddl_drop_synonym_suffix = identity.executor_id == kSblrDdlDropSynonymExecutorId ? ".ddl_drop_synonym" : ddl_drop_index_suffix;
  const auto ddl_drop_foreign_table_suffix = identity.executor_id == kSblrDdlDropForeignTableExecutorId ? ".ddl_drop_foreign_table" : ddl_drop_synonym_suffix;
  return context.database_path + ".sb.sblr_executor_availability_registry.v1" +
         ddl_drop_foreign_table_suffix;
}

void AddField(std::string* out, std::string_view key, std::string_view value) {
  out->append(std::to_string(key.size()));
  out->push_back(':');
  out->append(key);
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::string Sha256(std::string_view value) {
  const auto* bytes = reinterpret_cast<const scratchbird::core::platform::byte*>(
      value.data());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes, value.size());
  if (!digest.ok() ||
      digest.digest_bytes != scratchbird::core::hash::kSha256DigestBytes) {
    return {};
  }
  return "sha256:" + scratchbird::core::hash::HexLower(digest.digest);
}

bool ExactLiteralIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrLiteralExecutorId &&
         row.opcode_code == kSblrLiteralOpcodeCode &&
         row.opcode_version == kSblrLiteralOpcodeVersion &&
         row.operand_descriptor_id == kSblrLiteralOperandDescriptorId &&
         row.result_descriptor_id == kSblrLiteralResultDescriptorId &&
         row.result_descriptor_version == kSblrLiteralResultDescriptorVersion;
}
bool ExactParameterIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrParameterExecutorId &&
         row.opcode_code == kSblrParameterOpcodeCode &&
         row.opcode_version == kSblrParameterOpcodeVersion &&
         row.operand_descriptor_id == kSblrParameterOperandDescriptorId &&
         row.result_descriptor_id == kSblrParameterResultDescriptorId &&
         row.result_descriptor_version == kSblrParameterResultDescriptorVersion;
}
bool ExactVariableIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrVariableExecutorId &&
         row.opcode_code == kSblrVariableOpcodeCode &&
         row.opcode_version == kSblrVariableOpcodeVersion &&
         row.operand_descriptor_id == kSblrVariableOperandDescriptorId &&
         row.result_descriptor_id == kSblrVariableResultDescriptorId &&
         row.result_descriptor_version == kSblrVariableResultDescriptorVersion;
}
bool ExactSourceMapIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrSourceMapExecutorId &&
         row.opcode_code == kSblrSourceMapOpcodeCode &&
         row.opcode_version == kSblrSourceMapOpcodeVersion &&
         row.operand_descriptor_id == kSblrSourceMapOperandDescriptorId &&
         row.result_descriptor_id == kSblrSourceMapResultDescriptorId &&
         row.result_descriptor_version == kSblrSourceMapResultDescriptorVersion;
}
bool ExactErrorVectorIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrErrorVectorExecutorId &&
         row.opcode_code == kSblrErrorVectorOpcodeCode &&
         row.opcode_version == kSblrErrorVectorOpcodeVersion &&
         row.operand_descriptor_id == kSblrErrorVectorOperandDescriptorId &&
         row.result_descriptor_id == kSblrErrorVectorResultDescriptorId &&
         row.result_descriptor_version == kSblrErrorVectorResultDescriptorVersion;
}
bool ExactTxnBeginIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrTxnBeginExecutorId &&
         row.opcode_code == kSblrTxnBeginOpcodeCode &&
         row.opcode_version == kSblrTxnBeginOpcodeVersion &&
         row.operand_descriptor_id == kSblrTxnBeginOperandDescriptorId &&
         row.result_descriptor_id == kSblrTxnBeginResultDescriptorId &&
         row.result_descriptor_version == kSblrTxnBeginResultDescriptorVersion;
}
bool ExactTxnCommitIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id == kSblrTxnCommitExecutorId &&
         row.opcode_code == kSblrTxnCommitOpcodeCode &&
         row.opcode_version == kSblrTxnCommitOpcodeVersion &&
         row.operand_descriptor_id == kSblrTxnCommitOperandDescriptorId &&
         row.result_descriptor_id == kSblrTxnCommitResultDescriptorId &&
         row.result_descriptor_version == kSblrTxnCommitResultDescriptorVersion;
}
bool ExactTxnRollbackIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id==kSblrTxnRollbackExecutorId&&row.opcode_code==kSblrTxnRollbackOpcodeCode&&row.opcode_version==kSblrTxnRollbackOpcodeVersion&&row.operand_descriptor_id==kSblrTxnRollbackOperandDescriptorId&&row.result_descriptor_id==kSblrTxnRollbackResultDescriptorId&&row.result_descriptor_version==kSblrTxnRollbackResultDescriptorVersion;
}
bool ExactTxnSavepointIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id==kSblrTxnSavepointExecutorId&&row.opcode_code==kSblrTxnSavepointOpcodeCode&&row.opcode_version==kSblrTxnSavepointOpcodeVersion&&row.operand_descriptor_id==kSblrTxnSavepointOperandDescriptorId&&row.result_descriptor_id==kSblrTxnSavepointResultDescriptorId&&row.result_descriptor_version==kSblrTxnSavepointResultDescriptorVersion;
}
bool ExactTxnReleaseSavepointIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id==kSblrTxnReleaseSavepointExecutorId&&row.opcode_code==kSblrTxnReleaseSavepointOpcodeCode&&row.opcode_version==kSblrTxnReleaseSavepointOpcodeVersion&&row.operand_descriptor_id==kSblrTxnReleaseSavepointOperandDescriptorId&&row.result_descriptor_id==kSblrTxnReleaseSavepointResultDescriptorId&&row.result_descriptor_version==kSblrTxnReleaseSavepointResultDescriptorVersion;
}
bool ExactTxnRollbackToSavepointIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id==kSblrTxnRollbackToSavepointExecutorId&&row.opcode_code==kSblrTxnRollbackToSavepointOpcodeCode&&row.opcode_version==kSblrTxnRollbackToSavepointOpcodeVersion&&row.operand_descriptor_id==kSblrTxnRollbackToSavepointOperandDescriptorId&&row.result_descriptor_id==kSblrTxnRollbackToSavepointResultDescriptorId&&row.result_descriptor_version==kSblrTxnRollbackToSavepointResultDescriptorVersion;
}
bool ExactPsqlAutonomousFrameIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  return row.executor_id==kSblrPsqlAutonomousFrameExecutorId&&row.opcode_code==kSblrPsqlAutonomousFrameOpcodeCode&&row.opcode_version==kSblrPsqlAutonomousFrameOpcodeVersion&&row.operand_descriptor_id==kSblrPsqlAutonomousFrameOperandDescriptorId&&row.result_descriptor_id==kSblrPsqlAutonomousFrameResultDescriptorId&&row.result_descriptor_version==kSblrPsqlAutonomousFrameResultDescriptorVersion;
}
bool ExactReservationReleaseIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrReservationReleaseExecutorId&&row.opcode_code==kSblrReservationReleaseOpcodeCode&&row.opcode_version==kSblrReservationReleaseOpcodeVersion&&row.operand_descriptor_id==kSblrReservationReleaseOperandDescriptorId&&row.result_descriptor_id==kSblrReservationReleaseResultDescriptorId&&row.result_descriptor_version==kSblrReservationReleaseResultDescriptorVersion;}
bool ExactTemporaryInstanceCleanupIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrTemporaryInstanceCleanupExecutorId&&row.opcode_code==kSblrTemporaryInstanceCleanupOpcodeCode&&row.opcode_version==kSblrTemporaryInstanceCleanupOpcodeVersion&&row.operand_descriptor_id==kSblrTemporaryInstanceCleanupOperandDescriptorId&&row.result_descriptor_id==kSblrTemporaryInstanceCleanupResultDescriptorId&&row.result_descriptor_version==kSblrTemporaryInstanceCleanupResultDescriptorVersion;}
bool ExactCursorOpenIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrCursorOpenExecutorId&&row.opcode_code==kSblrCursorOpenOpcodeCode&&row.opcode_version==kSblrCursorOpenOpcodeVersion&&row.operand_descriptor_id==kSblrCursorOpenOperandDescriptorId&&row.result_descriptor_id==kSblrCursorOpenResultDescriptorId&&row.result_descriptor_version==kSblrCursorOpenResultDescriptorVersion;}
bool ExactCursorFetchIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrCursorFetchExecutorId&&row.opcode_code==kSblrCursorFetchOpcodeCode&&row.opcode_version==kSblrCursorFetchOpcodeVersion&&row.operand_descriptor_id==kSblrCursorFetchOperandDescriptorId&&row.result_descriptor_id==kSblrCursorFetchResultDescriptorId&&row.result_descriptor_version==kSblrCursorFetchResultDescriptorVersion;}
bool ExactCursorCloseIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrCursorCloseExecutorId&&row.opcode_code==kSblrCursorCloseOpcodeCode&&row.opcode_version==kSblrCursorCloseOpcodeVersion&&row.operand_descriptor_id==kSblrCursorCloseOperandDescriptorId&&row.result_descriptor_id==kSblrCursorCloseResultDescriptorId&&row.result_descriptor_version==kSblrCursorCloseResultDescriptorVersion;}
bool ExactReadByKeyIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrReadByKeyExecutorId&&row.opcode_code==kSblrReadByKeyOpcodeCode&&row.opcode_version==kSblrReadByKeyOpcodeVersion&&row.operand_descriptor_id==kSblrReadByKeyOperandDescriptorId&&row.result_descriptor_id==kSblrReadByKeyResultDescriptorId&&row.result_descriptor_version==kSblrReadByKeyResultDescriptorVersion;}
bool ExactReadRangeIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrReadRangeExecutorId&&row.opcode_code==kSblrReadRangeOpcodeCode&&row.opcode_version==kSblrReadRangeOpcodeVersion&&row.operand_descriptor_id==kSblrReadRangeOperandDescriptorId&&row.result_descriptor_id==kSblrReadRangeResultDescriptorId&&row.result_descriptor_version==kSblrReadRangeResultDescriptorVersion;}
bool ExactReadStreamIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrReadStreamExecutorId&&row.opcode_code==kSblrReadStreamOpcodeCode&&row.opcode_version==kSblrReadStreamOpcodeVersion&&row.operand_descriptor_id==kSblrReadStreamOperandDescriptorId&&row.result_descriptor_id==kSblrReadStreamResultDescriptorId&&row.result_descriptor_version==kSblrReadStreamResultDescriptorVersion;}
bool ExactResultSetPassIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrResultSetPassExecutorId&&row.opcode_code==kSblrResultSetPassOpcodeCode&&row.opcode_version==kSblrResultSetPassOpcodeVersion&&row.operand_descriptor_id==kSblrResultSetPassOperandDescriptorId&&row.result_descriptor_id==kSblrResultSetPassResultDescriptorId&&row.result_descriptor_version==kSblrResultSetPassResultDescriptorVersion;}
bool ExactAccessCursorOpenIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrAccessCursorOpenExecutorId&&row.opcode_code==kSblrAccessCursorOpenOpcodeCode&&row.opcode_version==kSblrAccessCursorOpenOpcodeVersion&&row.operand_descriptor_id==kSblrAccessCursorOpenOperandDescriptorId&&row.result_descriptor_id==kSblrAccessCursorOpenResultDescriptorId&&row.result_descriptor_version==kSblrAccessCursorOpenResultDescriptorVersion;}
bool ExactAccessCursorFetchIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrAccessCursorFetchExecutorId&&row.opcode_code==kSblrAccessCursorFetchOpcodeCode&&row.opcode_version==kSblrAccessCursorFetchOpcodeVersion&&row.operand_descriptor_id==kSblrAccessCursorFetchOperandDescriptorId&&row.result_descriptor_id==kSblrAccessCursorFetchResultDescriptorId&&row.result_descriptor_version==kSblrAccessCursorFetchResultDescriptorVersion;}
bool ExactAccessCursorCloseIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrAccessCursorCloseExecutorId&&row.opcode_code==kSblrAccessCursorCloseOpcodeCode&&row.opcode_version==kSblrAccessCursorCloseOpcodeVersion&&row.operand_descriptor_id==kSblrAccessCursorCloseOperandDescriptorId&&row.result_descriptor_id==kSblrAccessCursorCloseResultDescriptorId&&row.result_descriptor_version==kSblrAccessCursorCloseResultDescriptorVersion;}
bool ExactInsertIdentity(const SblrExecutorAvailabilityRowIdentity& row) {return row.executor_id==kSblrInsertExecutorId&&row.opcode_code==kSblrInsertOpcodeCode&&row.opcode_version==kSblrInsertOpcodeVersion&&row.operand_descriptor_id==kSblrInsertOperandDescriptorId&&row.result_descriptor_id==kSblrInsertResultDescriptorId&&row.result_descriptor_version==kSblrInsertResultDescriptorVersion;}
bool ExactUpdateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrUpdateExecutorId&&r.opcode_code==769&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrUpdateOperandDescriptorId&&r.result_descriptor_id==kSblrUpdateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDeleteIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDeleteExecutorId&&r.opcode_code==770&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDeleteOperandDescriptorId&&r.result_descriptor_id==kSblrDeleteResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactMergeIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrMergeExecutorId&&r.opcode_code==771&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrMergeOperandDescriptorId&&r.result_descriptor_id==kSblrMergeResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactTableTruncateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrTableTruncateExecutorId&&r.opcode_code==773&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrTableTruncateOperandDescriptorId&&r.result_descriptor_id==kSblrTableTruncateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactTableAnalyzeIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrTableAnalyzeExecutorId&&r.opcode_code==774&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrTableAnalyzeOperandDescriptorId&&r.result_descriptor_id==kSblrTableAnalyzeResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactBulkImportStreamIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrBulkImportStreamExecutorId&&r.opcode_code==775&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrBulkImportStreamOperandDescriptorId&&r.result_descriptor_id==kSblrBulkImportStreamResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactBulkExportStreamIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrBulkExportStreamExecutorId&&r.opcode_code==776&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrBulkExportStreamOperandDescriptorId&&r.result_descriptor_id==kSblrBulkExportStreamResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactStatementBatchIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrStatementBatchExecutorId&&r.opcode_code==777&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrStatementBatchOperandDescriptorId&&r.result_descriptor_id==kSblrStatementBatchResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAtomicCasIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAtomicCasExecutorId&&r.opcode_code==778&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAtomicCasOperandDescriptorId&&r.result_descriptor_id==kSblrAtomicCasResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAtomicRmwIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAtomicRmwExecutorId&&r.opcode_code==779&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAtomicRmwOperandDescriptorId&&r.result_descriptor_id==kSblrAtomicRmwResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAdvisoryLockIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAdvisoryLockAcquireExecutorId&&r.opcode_code==780&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAdvisoryLockAcquireOperandDescriptorId&&r.result_descriptor_id==kSblrAdvisoryLockResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAdvisoryLockReleaseIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAdvisoryLockReleaseExecutorId&&r.opcode_code==781&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAdvisoryLockReleaseOperandDescriptorId&&r.result_descriptor_id==kSblrAdvisoryLockResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactFunctionCallIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrFunctionCallExecutorId&&r.opcode_code==1024&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrFunctionCallOperandDescriptorId&&r.result_descriptor_id==kSblrFunctionCallResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactOperatorCallIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrOperatorCallExecutorId&&r.opcode_code==1025&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrOperatorCallOperandDescriptorId&&r.result_descriptor_id==kSblrOperatorCallResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactCastIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrCastExecutorId&&r.opcode_code==1026&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrCastOperandDescriptorId&&r.result_descriptor_id==kSblrCastResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactCompareIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrCompareExecutorId&&r.opcode_code==1027&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrCompareOperandDescriptorId&&r.result_descriptor_id==kSblrCompareResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDomainOperationIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDomainOperationExecutorId&&r.opcode_code==1028&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDomainOperationOperandDescriptorId&&r.result_descriptor_id==kSblrDomainOperationResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactUdrInvokeIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrUdrInvokeExecutorId&&r.opcode_code==1029&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrUdrInvokeOperandDescriptorId&&r.result_descriptor_id==kSblrUdrInvokeResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactProcedureInvokeIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrProcedureInvokeExecutorId&&r.opcode_code==1030&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrProcedureInvokeOperandDescriptorId&&r.result_descriptor_id==kSblrProcedureInvokeResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactFunctionInvokeIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrFunctionInvokeExecutorId&&r.opcode_code==1031&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrFunctionInvokeOperandDescriptorId&&r.result_descriptor_id==kSblrFunctionInvokeResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAggregateInvokeIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAggregateInvokeExecutorId&&r.opcode_code==1032&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAggregateInvokeOperandDescriptorId&&r.result_descriptor_id==kSblrAggregateInvokeResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSequenceNextvalIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSequenceNextvalExecutorId&&r.opcode_code==1033&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSequenceNextvalOperandDescriptorId&&r.result_descriptor_id==kSblrSequenceNextvalResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSequenceCurrvalIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSequenceCurrvalExecutorId&&r.opcode_code==1034&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSequenceCurrvalOperandDescriptorId&&r.result_descriptor_id==kSblrSequenceCurrvalResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSequenceSetvalIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSequenceSetvalExecutorId&&r.opcode_code==1035&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSequenceSetvalOperandDescriptorId&&r.result_descriptor_id==kSblrSequenceSetvalResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactQueryNumericIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrQueryNumericExecutorId&&r.opcode_code==1036&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrQueryNumericOperandDescriptorId&&r.result_descriptor_id==kSblrQueryNumericResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAdvancedDatatypeFamilyIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAdvancedDatatypeFamilyExecutorId&&r.opcode_code==1037&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAdvancedDatatypeFamilyOperandDescriptorId&&r.result_descriptor_id==kSblrAdvancedDatatypeFamilyResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactProjectIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrProjectExecutorId&&r.opcode_code==1280&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrProjectOperandDescriptorId&&r.result_descriptor_id==kSblrProjectResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAggregateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAggregateExecutorId&&r.opcode_code==1281&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAggregateOperandDescriptorId&&r.result_descriptor_id==kSblrAggregateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactGroupIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrGroupExecutorId&&r.opcode_code==1282&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrGroupOperandDescriptorId&&r.result_descriptor_id==kSblrGroupResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSortIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSortExecutorId&&r.opcode_code==1283&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSortOperandDescriptorId&&r.result_descriptor_id==kSblrSortResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactLimitIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrLimitExecutorId&&r.opcode_code==1284&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrLimitOperandDescriptorId&&r.result_descriptor_id==kSblrLimitResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactWindowIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrWindowExecutorId&&r.opcode_code==1285&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrWindowOperandDescriptorId&&r.result_descriptor_id==kSblrWindowResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactReturnResultSetIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrReturnResultSetExecutorId&&r.opcode_code==1286&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrReturnResultSetOperandDescriptorId&&r.result_descriptor_id==kSblrReturnResultSetResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactKvStructuredReadIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrKvStructuredReadExecutorId&&r.opcode_code==8192&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrKvStructuredReadOperandDescriptorId&&r.result_descriptor_id==kSblrKvStructuredReadResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactKvStructuredMutateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrKvStructuredMutateExecutorId&&r.opcode_code==8193&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrKvStructuredMutateOperandDescriptorId&&r.result_descriptor_id==kSblrKvStructuredMutateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactKvStructuredScanIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrKvStructuredScanExecutorId&&r.opcode_code==8194&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrKvStructuredScanOperandDescriptorId&&r.result_descriptor_id==kSblrKvStructuredScanResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactKvStructuredStreamReadIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrKvStructuredStreamReadExecutorId&&r.opcode_code==8195&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrKvStructuredStreamReadOperandDescriptorId&&r.result_descriptor_id==kSblrKvStructuredStreamReadResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactKvStructuredStreamAppendIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrKvStructuredStreamAppendExecutorId&&r.opcode_code==8196&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrKvStructuredStreamAppendOperandDescriptorId&&r.result_descriptor_id==kSblrKvStructuredStreamAppendResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactKvStructuredTimeseriesIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrKvStructuredTimeseriesExecutorId&&r.opcode_code==8197&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrKvStructuredTimeseriesOperandDescriptorId&&r.result_descriptor_id==kSblrKvStructuredTimeseriesResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSystemConfigSetIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSystemConfigSetExecutorId&&r.opcode_code==5125&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSystemConfigSetOperandDescriptorId&&r.result_descriptor_id==kSblrSystemConfigSetResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSystemConfigGetIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSystemConfigGetExecutorId&&r.opcode_code==5126&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSystemConfigGetOperandDescriptorId&&r.result_descriptor_id==kSblrSystemConfigGetResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateDomainIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateDomainExecutorId&&r.opcode_code==1542&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateDomainOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateDomainResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateSchemaIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateSchemaExecutorId&&r.opcode_code==1536&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateSchemaOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateSchemaResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateTableIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateTableExecutorId&&r.opcode_code==1537&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateTableOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateTableResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateIndexIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateIndexExecutorId&&r.opcode_code==1540&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateIndexOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateIndexResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropIndexIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropIndexExecutorId&&r.opcode_code==1541&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropIndexOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropIndexResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropSynonymIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropSynonymExecutorId&&r.opcode_code==1575&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropSynonymOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropSynonymResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropForeignTableIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropForeignTableExecutorId&&r.opcode_code==1577&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropForeignTableOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropForeignTableResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateFdwIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateFdwExecutorId&&r.opcode_code==1578&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateFdwOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateFdwResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropFdwIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropFdwExecutorId&&r.opcode_code==1579&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropFdwOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropFdwResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSecurityCreateUserIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSecurityCreateUserExecutorId&&r.opcode_code==1792&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSecurityCreateUserOperandDescriptorId&&r.result_descriptor_id==kSblrSecurityCreateUserResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlAlterDomainIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterDomainExecutorId&&r.opcode_code==1547&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterDomainOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterDomainResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateViewIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateViewExecutorId&&r.opcode_code==1548&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateViewOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateViewResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlAlterViewIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterViewExecutorId&&r.opcode_code==1549&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterViewOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterViewResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateTriggerIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateTriggerExecutorId&&r.opcode_code==1551&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateTriggerOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateTriggerResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlAlterTriggerIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterTriggerExecutorId&&r.opcode_code==1552&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterTriggerOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterTriggerResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropTriggerIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropTriggerExecutorId&&r.opcode_code==1553&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropTriggerOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropTriggerResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateProcedureIdentity(const SblrExecutorAvailabilityRowIdentity&r){return (r.executor_id==kSblrDdlCreateProcedureExecutorId&&r.opcode_code==1554&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateProcedureOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateProcedureResultDescriptorId||r.executor_id==kSblrDdlAlterProcedureExecutorId&&r.opcode_code==1555&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterProcedureOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterProcedureResultDescriptorId)&&r.result_descriptor_version==1;}
bool ExactDdlAlterProcedureIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterProcedureExecutorId&&r.opcode_code==1555&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterProcedureOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterProcedureResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropProcedureIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropProcedureExecutorId&&r.opcode_code==1556&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropProcedureOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropProcedureResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateFunctionIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateFunctionExecutorId&&r.opcode_code==1557&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateFunctionOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateFunctionResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlAlterFunctionIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterFunctionExecutorId&&r.opcode_code==1558&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterFunctionOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterFunctionResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropFunctionIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropFunctionExecutorId&&r.opcode_code==1559&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropFunctionOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropFunctionResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreatePackageIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreatePackageExecutorId&&r.opcode_code==1560&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreatePackageOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreatePackageResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropTemporaryTableIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropTemporaryTableExecutorId&&r.opcode_code==1562&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropTemporaryTableOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropTemporaryTableResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateTemporaryTableIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateTemporaryTableExecutorId&&r.opcode_code==1561&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateTemporaryTableOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateTemporaryTableResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlRenameObjectVectorIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlRenameObjectVectorExecutorId&&r.opcode_code==1563&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlRenameObjectVectorOperandDescriptorId&&r.result_descriptor_id==kSblrDdlRenameObjectVectorResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlRenameObjectIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlRenameObjectExecutorId&&r.opcode_code==1572&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlRenameObjectOperandDescriptorId&&r.result_descriptor_id==kSblrDdlRenameObjectResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateOrReplaceSrsIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateOrReplaceSrsExecutorId&&r.opcode_code==1615&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateOrReplaceSrsOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateOrReplaceSrsResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropSrsIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropSrsExecutorId&&r.opcode_code==1616&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropSrsOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropSrsResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateRewriteRuleIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateRewriteRuleExecutorId&&r.opcode_code==1617&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateRewriteRuleOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateRewriteRuleResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlAlterRewriteRuleIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterRewriteRuleExecutorId&&r.opcode_code==1618&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterRewriteRuleOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterRewriteRuleResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropRewriteRuleIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropRewriteRuleExecutorId&&r.opcode_code==1619&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropRewriteRuleOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropRewriteRuleResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlValidateConstraintIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlValidateConstraintExecutorId&&r.opcode_code==1620&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlValidateConstraintOperandDescriptorId&&r.result_descriptor_id==kSblrDdlValidateConstraintResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSecurityCreatePrivilegeTemplateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSecurityCreatePrivilegeTemplateExecutorId&&r.opcode_code==1621&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSecurityCreatePrivilegeTemplateOperandDescriptorId&&r.result_descriptor_id==kSblrSecurityCreatePrivilegeTemplateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSecurityAlterPrivilegeTemplateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSecurityAlterPrivilegeTemplateExecutorId&&r.opcode_code==1622&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSecurityAlterPrivilegeTemplateOperandDescriptorId&&r.result_descriptor_id==kSblrSecurityAlterPrivilegeTemplateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactSecurityDropPrivilegeTemplateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrSecurityDropPrivilegeTemplateExecutorId&&r.opcode_code==1623&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrSecurityDropPrivilegeTemplateOperandDescriptorId&&r.result_descriptor_id==kSblrSecurityDropPrivilegeTemplateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDatabaseCreateTemplateCloneIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDatabaseCreateTemplateCloneExecutorId&&r.opcode_code==1624&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDatabaseCreateTemplateCloneOperandDescriptorId&&r.result_descriptor_id==kSblrDatabaseCreateTemplateCloneResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateAggregateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateAggregateExecutorId&&r.opcode_code==1625&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateAggregateOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateAggregateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlAlterAggregateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterAggregateExecutorId&&r.opcode_code==1626&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterAggregateOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterAggregateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropAggregateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropAggregateExecutorId&&r.opcode_code==1627&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropAggregateOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropAggregateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlPurgeSystemHistoryIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlPurgeSystemHistoryExecutorId&&r.opcode_code==1628&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlPurgeSystemHistoryOperandDescriptorId&&r.result_descriptor_id==kSblrDdlPurgeSystemHistoryResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlSetIndexOptimizerEligibilityIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlSetIndexOptimizerEligibilityExecutorId&&r.opcode_code==1629&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlSetIndexOptimizerEligibilityOperandDescriptorId&&r.result_descriptor_id==kSblrDdlSetIndexOptimizerEligibilityResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlSetTableTypeEnforcementIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlSetTableTypeEnforcementExecutorId&&r.opcode_code==1630&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlSetTableTypeEnforcementOperandDescriptorId&&r.result_descriptor_id==kSblrDdlSetTableTypeEnforcementResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDatabaseSerializeLogicalSnapshotIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDatabaseSerializeLogicalSnapshotExecutorId&&r.opcode_code==1631&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDatabaseSerializeLogicalSnapshotOperandDescriptorId&&r.result_descriptor_id==kSblrDatabaseSerializeLogicalSnapshotResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDatabaseDeserializeLogicalSnapshotIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDatabaseDeserializeLogicalSnapshotExecutorId&&r.opcode_code==1632&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDatabaseDeserializeLogicalSnapshotOperandDescriptorId&&r.result_descriptor_id==kSblrDatabaseDeserializeLogicalSnapshotResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateMacroIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateMacroExecutorId&&r.opcode_code==1633&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateMacroOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateMacroResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropMacroIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropMacroExecutorId&&r.opcode_code==1634&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropMacroOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropMacroResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAdminRegisterExternalRelationResolverIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAdminRegisterExternalRelationResolverExecutorId&&r.opcode_code==1635&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAdminRegisterExternalRelationResolverOperandDescriptorId&&r.result_descriptor_id==kSblrAdminRegisterExternalRelationResolverResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAdminUnregisterExternalRelationResolverIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrAdminUnregisterExternalRelationResolverExecutorId&&r.opcode_code==1636&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrAdminUnregisterExternalRelationResolverOperandDescriptorId&&r.result_descriptor_id==kSblrAdminUnregisterExternalRelationResolverResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateDictionaryIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateDictionaryExecutorId&&r.opcode_code==1637&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateDictionaryOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateDictionaryResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropDictionaryIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropDictionaryExecutorId&&r.opcode_code==1638&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropDictionaryOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropDictionaryResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlAlterDictionaryIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterDictionaryExecutorId&&r.opcode_code==1639&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterDictionaryOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterDictionaryResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateContinuousViewIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateContinuousViewExecutorId&&r.opcode_code==1640&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateContinuousViewOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateContinuousViewResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlAlterContinuousViewIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlAlterContinuousViewExecutorId&&r.opcode_code==1641&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlAlterContinuousViewOperandDescriptorId&&r.result_descriptor_id==kSblrDdlAlterContinuousViewResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropContinuousViewIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropContinuousViewExecutorId&&r.opcode_code==1642&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropContinuousViewOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropContinuousViewResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDmlAsyncInsertSubmitIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDmlAsyncInsertSubmitExecutorId&&r.opcode_code==1643&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDmlAsyncInsertSubmitOperandDescriptorId&&r.result_descriptor_id==kSblrDmlAsyncInsertSubmitResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDmlAsyncInsertStatusIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDmlAsyncInsertStatusExecutorId&&r.opcode_code==1644&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDmlAsyncInsertStatusOperandDescriptorId&&r.result_descriptor_id==kSblrDmlAsyncInsertStatusResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDmlAsyncInsertCancelIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDmlAsyncInsertCancelExecutorId&&r.opcode_code==1645&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDmlAsyncInsertCancelOperandDescriptorId&&r.result_descriptor_id==kSblrDmlAsyncInsertCancelResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDmlConditionalMutateIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDmlConditionalMutateExecutorId&&r.opcode_code==1646&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDmlConditionalMutateOperandDescriptorId&&r.result_descriptor_id==kSblrDmlConditionalMutateResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDmlCounterAddIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDmlCounterAddExecutorId&&r.opcode_code==1647&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDmlCounterAddOperandDescriptorId&&r.result_descriptor_id==kSblrDmlCounterAddResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlTimeseriesSeriesCardinalityPolicyIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlTimeseriesSeriesCardinalityPolicyExecutorId&&r.opcode_code==1649&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlTimeseriesSeriesCardinalityPolicyOperandDescriptorId&&r.result_descriptor_id==kSblrDdlTimeseriesSeriesCardinalityPolicyResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlCreateTimeseriesValueCacheIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlCreateTimeseriesValueCacheExecutorId&&r.opcode_code==1650&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlCreateTimeseriesValueCacheOperandDescriptorId&&r.result_descriptor_id==kSblrDdlCreateTimeseriesValueCacheResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactDdlDropViewIdentity(const SblrExecutorAvailabilityRowIdentity&r){return r.executor_id==kSblrDdlDropViewExecutorId&&r.opcode_code==1550&&r.opcode_version=="1.0"&&r.operand_descriptor_id==kSblrDdlDropViewOperandDescriptorId&&r.result_descriptor_id==kSblrDdlDropViewResultDescriptorId&&r.result_descriptor_version==1;}
bool ExactAdmittedIdentity(const SblrExecutorAvailabilityRowIdentity& row) {
  if (ExactDatabaseSerializeLogicalSnapshotIdentity(row)) return true;
  if (ExactDatabaseDeserializeLogicalSnapshotIdentity(row)) return true;
  if (ExactDdlCreateMacroIdentity(row)) return true;
  if (ExactDdlDropMacroIdentity(row)) return true;
  if (ExactAdminRegisterExternalRelationResolverIdentity(row)) return true;
  if (ExactAdminUnregisterExternalRelationResolverIdentity(row)) return true;
  if (ExactDdlCreateDictionaryIdentity(row)) return true;
  if (ExactDdlDropDictionaryIdentity(row)) return true;
  if (ExactDdlAlterDictionaryIdentity(row)) return true;
  if (ExactDdlCreateContinuousViewIdentity(row)) return true;
  if (ExactDdlAlterContinuousViewIdentity(row)) return true;
  if (ExactDdlDropContinuousViewIdentity(row)) return true;
  if (ExactDmlAsyncInsertSubmitIdentity(row)) return true;
  if (ExactDmlAsyncInsertStatusIdentity(row)) return true;
  if (ExactDmlAsyncInsertCancelIdentity(row)) return true;
  if (ExactDmlConditionalMutateIdentity(row)) return true;
  if (ExactDmlCounterAddIdentity(row)) return true;
  if (ExactDdlTimeseriesSeriesCardinalityPolicyIdentity(row)) return true;
  if (ExactDdlCreateTimeseriesValueCacheIdentity(row)) return true;
  if (ExactDdlDropForeignTableIdentity(row)) return true;
  if (ExactDdlCreateFdwIdentity(row)) return true;
  if (ExactDdlDropFdwIdentity(row)) return true;
  if (ExactSecurityCreateUserIdentity(row)) return true;
  if (row.executor_id == kSblrSecurityAlterUserExecutorId && row.opcode_code == kSblrSecurityAlterUserOpcodeCode && row.opcode_version == kSblrSecurityAlterUserOpcodeVersion && row.operand_descriptor_id == kSblrSecurityAlterUserOperandDescriptorId && row.result_descriptor_id == kSblrSecurityAlterUserResultDescriptorId && row.result_descriptor_version == kSblrSecurityAlterUserResultDescriptorVersion) return true;
  if (ExactSystemConfigGetIdentity(row)) return true;
  return ExactLiteralIdentity(row) || ExactParameterIdentity(row) ||
         ExactVariableIdentity(row) || ExactSourceMapIdentity(row) ||
         ExactErrorVectorIdentity(row) || ExactDdlCreateProcedureIdentity(row) || ExactDdlDropProcedureIdentity(row) || ExactSequenceSetvalIdentity(row) || ExactQueryNumericIdentity(row) || ExactAdvancedDatatypeFamilyIdentity(row) || ExactProjectIdentity(row) || ExactAggregateIdentity(row) || ExactGroupIdentity(row) || ExactSortIdentity(row) || ExactLimitIdentity(row) || ExactWindowIdentity(row) || ExactReturnResultSetIdentity(row) || ExactKvStructuredReadIdentity(row) || ExactKvStructuredMutateIdentity(row) || ExactKvStructuredScanIdentity(row) || ExactKvStructuredStreamReadIdentity(row) || ExactKvStructuredStreamAppendIdentity(row) || ExactKvStructuredTimeseriesIdentity(row) || ExactSystemConfigSetIdentity(row) || ExactDdlCreateDomainIdentity(row) || ExactDdlCreateSchemaIdentity(row) || ExactDdlCreateTableIdentity(row) || ExactDdlCreateIndexIdentity(row) || ExactDdlAlterDomainIdentity(row) || ExactDdlCreateViewIdentity(row) || ExactDdlAlterViewIdentity(row) || ExactDdlDropViewIdentity(row) || ExactDdlCreateTriggerIdentity(row) || ExactDdlAlterTriggerIdentity(row) || ExactDdlDropTriggerIdentity(row) || ExactDdlDropIndexIdentity(row) || ExactDdlDropSynonymIdentity(row) || ExactTxnBeginIdentity(row) ||
         ExactDdlCreateFunctionIdentity(row) || ExactDdlAlterFunctionIdentity(row) || ExactDdlDropFunctionIdentity(row) || ExactDdlCreatePackageIdentity(row) || ExactDdlCreateTemporaryTableIdentity(row) || ExactDdlDropTemporaryTableIdentity(row) || ExactDdlRenameObjectVectorIdentity(row) || ExactDdlRenameObjectIdentity(row) || ExactDdlCreateOrReplaceSrsIdentity(row) || ExactDdlDropSrsIdentity(row) || ExactDdlCreateRewriteRuleIdentity(row) || ExactDdlAlterRewriteRuleIdentity(row) || ExactDdlDropRewriteRuleIdentity(row) || ExactDdlValidateConstraintIdentity(row) || ExactSecurityCreatePrivilegeTemplateIdentity(row) || ExactSecurityAlterPrivilegeTemplateIdentity(row) || ExactSecurityDropPrivilegeTemplateIdentity(row) || ExactDatabaseCreateTemplateCloneIdentity(row) || ExactDdlCreateAggregateIdentity(row) || ExactDdlAlterAggregateIdentity(row) || ExactDdlDropAggregateIdentity(row) || ExactDdlPurgeSystemHistoryIdentity(row) || ExactDdlSetIndexOptimizerEligibilityIdentity(row) || ExactDdlSetTableTypeEnforcementIdentity(row) || ExactTxnCommitIdentity(row) || ExactTxnRollbackIdentity(row) ||
         ExactTxnSavepointIdentity(row) || ExactTxnReleaseSavepointIdentity(row) || ExactTxnRollbackToSavepointIdentity(row) || ExactPsqlAutonomousFrameIdentity(row) || ExactReservationReleaseIdentity(row) || ExactTemporaryInstanceCleanupIdentity(row) || ExactCursorOpenIdentity(row) || ExactCursorFetchIdentity(row) || ExactCursorCloseIdentity(row) || ExactReadByKeyIdentity(row) || ExactReadRangeIdentity(row) || ExactReadStreamIdentity(row) || ExactResultSetPassIdentity(row) || ExactAccessCursorOpenIdentity(row) || ExactAccessCursorFetchIdentity(row) || ExactAccessCursorCloseIdentity(row) || ExactInsertIdentity(row) || ExactUpdateIdentity(row) || ExactDeleteIdentity(row) || ExactMergeIdentity(row) || ExactTableTruncateIdentity(row) || ExactTableAnalyzeIdentity(row) || ExactBulkImportStreamIdentity(row) || ExactBulkExportStreamIdentity(row) || ExactStatementBatchIdentity(row) || ExactAtomicCasIdentity(row) || ExactAtomicRmwIdentity(row) || ExactAdvisoryLockIdentity(row) || ExactAdvisoryLockReleaseIdentity(row) || ExactFunctionCallIdentity(row) || ExactOperatorCallIdentity(row) || ExactCastIdentity(row) || ExactCompareIdentity(row) || ExactDomainOperationIdentity(row) || ExactUdrInvokeIdentity(row) || ExactProcedureInvokeIdentity(row) || ExactFunctionInvokeIdentity(row) || ExactAggregateInvokeIdentity(row) || ExactSequenceNextvalIdentity(row) || ExactSequenceCurrvalIdentity(row);
}

std::string StateName(SblrExecutorAvailabilityState state) {
  switch (state) {
    case SblrExecutorAvailabilityState::installed: return "installed";
    case SblrExecutorAvailabilityState::revoked: return "revoked";
    case SblrExecutorAvailabilityState::unavailable: return "unavailable";
  }
  return {};
}

bool ParseState(std::string_view value,
                SblrExecutorAvailabilityState* state) {
  if (state == nullptr) return false;
  if (value == "installed") {
    *state = SblrExecutorAvailabilityState::installed;
    return true;
  }
  if (value == "revoked") {
    *state = SblrExecutorAvailabilityState::revoked;
    return true;
  }
  if (value == "unavailable") {
    *state = SblrExecutorAvailabilityState::unavailable;
    return true;
  }
  return false;
}

bool SafeReason(std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  for (unsigned char c : value) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
          c == '-')) return false;
  }
  return true;
}

bool ValidIdentityUuid(std::string_view value,
                       scratchbird::core::platform::UuidKind kind) {
  return !value.empty() &&
      scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
          kind, std::string(value)).ok();
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto end = line.find('\t', start);
    fields.push_back(line.substr(start, end == std::string::npos
                                           ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return fields;
}

std::uint64_t ParseU64(std::string_view value) {
  std::uint64_t result = 0;
  if (value.empty()) return 0;
  for (char c : value) {
    if (c < '0' || c > '9') return 0;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (result > (UINT64_MAX - digit) / 10) return 0;
    result = result * 10 + digit;
  }
  return result;
}

std::string DecisionPayload(const std::string& database_uuid,
                            const std::string& prior_snapshot_uuid,
                            std::uint64_t prior_generation,
                            const SblrExecutorAvailabilitySnapshot& next,
                            std::string_view reason_code) {
  std::string payload;
  AddField(&payload, "registry_id", kRegistryId);
  AddField(&payload, "database_uuid", database_uuid);
  AddField(&payload, "prior_snapshot_uuid", prior_snapshot_uuid);
  AddField(&payload, "prior_generation", std::to_string(prior_generation));
  AddField(&payload, "snapshot_uuid", next.snapshot_uuid);
  AddField(&payload, "generation", std::to_string(next.generation));
  AddField(&payload, "row_identity_sha256", next.row_identity_sha256);
  AddField(&payload, "installed", next.installed ? "true" : "false");
  AddField(&payload, "availability_state", StateName(next.availability_state));
  AddField(&payload, "reason_code", reason_code);
  return payload;
}

std::string JoinRecord(std::string_view kind,
                       const SblrExecutorAvailabilitySnapshot& snapshot,
                       std::string_view prior_snapshot_uuid,
                       std::uint64_t prior_generation,
                       std::string_view reason_code) {
  std::ostringstream out;
  out << kMagic << '\t' << kind << '\t' << kRegistryId << '\t'
      << snapshot.database_uuid << '\t' << prior_snapshot_uuid << '\t'
      << prior_generation << '\t' << snapshot.snapshot_uuid << '\t'
      << snapshot.generation << '\t' << snapshot.row_identity_sha256 << '\t'
      << (snapshot.installed ? "1" : "0") << '\t'
      << StateName(snapshot.availability_state) << '\t'
      << snapshot.decision_evidence_sha256 << '\t' << reason_code;
  return out.str();
}

bool DurableAppend(const std::string& path, const std::string& line) {
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    out << line << '\n';
    out.flush();
    if (!out) return false;
  }
#if defined(_WIN32)
  HANDLE handle = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const bool ok = FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  return ok;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

EngineApiDiagnostic RegistryDiagnostic(std::string code,
                                       std::string key,
                                       std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}

std::string NewSnapshotUuid(std::uint64_t generation) {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, now + generation);
  return generated.ok()
      ? scratchbird::core::uuid::UuidToString(generated.value.value)
      : std::string{};
}

struct DecodedPair {
  SblrExecutorAvailabilitySnapshot snapshot;
  std::string prior_snapshot_uuid;
  std::uint64_t prior_generation{0};
  std::string reason_code;
};

bool DecodeRecord(const std::vector<std::string>& fields,
                  std::string_view expected_kind,
                  DecodedPair* decoded) {
  if (decoded == nullptr || fields.size() != 13 || fields[0] != kMagic ||
      fields[1] != expected_kind || fields[2] != kRegistryId ||
      !ValidIdentityUuid(fields[3],
                         scratchbird::core::platform::UuidKind::database) ||
      !ValidIdentityUuid(fields[6],
                         scratchbird::core::platform::UuidKind::object) ||
      ParseU64(fields[7]) == 0 ||
      fields[8].size() != 71 || !fields[8].starts_with("sha256:") ||
      (fields[9] != "0" && fields[9] != "1") ||
      fields[11].size() != 71 || !fields[11].starts_with("sha256:") ||
      !SafeReason(fields[12])) return false;
  decoded->snapshot.database_uuid = fields[3];
  decoded->prior_snapshot_uuid = fields[4];
  decoded->prior_generation = ParseU64(fields[5]);
  decoded->snapshot.snapshot_uuid = fields[6];
  decoded->snapshot.generation = ParseU64(fields[7]);
  decoded->snapshot.row_identity_sha256 = fields[8];
  decoded->snapshot.installed = fields[9] == "1";
  if (!ParseState(fields[10], &decoded->snapshot.availability_state)) return false;
  decoded->snapshot.decision_evidence_sha256 = fields[11];
  decoded->reason_code = fields[12];
  return decoded->snapshot.installed ==
      (decoded->snapshot.availability_state ==
       SblrExecutorAvailabilityState::installed);
}

SblrExecutorAvailabilityLoadResult LoadLocked(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& identity,
    bool allow_bootstrap);

bool PublishPair(const std::string& path,
                 const DecodedPair& pair) {
  if (!DurableAppend(path, JoinRecord("EVIDENCE", pair.snapshot,
                                      pair.prior_snapshot_uuid,
                                      pair.prior_generation,
                                      pair.reason_code))) return false;
  return DurableAppend(path, JoinRecord("SNAPSHOT", pair.snapshot,
                                        pair.prior_snapshot_uuid,
                                        pair.prior_generation,
                                        pair.reason_code));
}

SblrExecutorAvailabilityLoadResult BootstrapLocked(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& identity) {
  SblrExecutorAvailabilityLoadResult result;
  DecodedPair pair;
  pair.snapshot.snapshot_uuid = NewSnapshotUuid(1);
  pair.snapshot.generation = 1;
  pair.snapshot.database_uuid = context.database_uuid.canonical;
  pair.snapshot.row_identity_sha256 =
      ComputeSblrExecutorAvailabilityRowIdentitySha256(identity);
  pair.snapshot.installed = true;
  pair.snapshot.availability_state = SblrExecutorAvailabilityState::installed;
  pair.reason_code = ExactCastIdentity(identity)
                         ? "bootstrap.admitted_cast.v1"
                     : ExactOperatorCallIdentity(identity)
                         ? "bootstrap.admitted_operator_call.v1"
                     : ExactFunctionCallIdentity(identity)
                         ? "bootstrap.admitted_function_call.v1"
                     : ExactAdvisoryLockReleaseIdentity(identity)
                         ? "bootstrap.admitted_advisory_lock_release.v1"
                     : ExactAdvisoryLockIdentity(identity)
                         ? "bootstrap.admitted_advisory_lock_acquire.v1"
                     : ExactAtomicRmwIdentity(identity)
                         ? "bootstrap.admitted_atomic_rmw.v1"
                     : ExactAtomicCasIdentity(identity)
                         ? "bootstrap.admitted_atomic_cas.v1"
                     : ExactStatementBatchIdentity(identity)
                         ? "bootstrap.admitted_statement_batch.v1"
                     : ExactBulkImportStreamIdentity(identity)
                         ? "bootstrap.admitted_bulk_import_stream.v1"
                     : ExactTableAnalyzeIdentity(identity)
                         ? "bootstrap.admitted_table_analyze.v1"
                     : ExactDmlConditionalMutateIdentity(identity)
                         ? "bootstrap.admitted_dml_conditional_mutate.v1"
                     : ExactDmlCounterAddIdentity(identity)
                         ? "bootstrap.admitted_dml_counter_add.v1"
                     : ExactDdlTimeseriesSeriesCardinalityPolicyIdentity(identity)
                         ? "bootstrap.admitted_timeseries_series_cardinality_policy.v1"
                     : ExactDdlCreateTimeseriesValueCacheIdentity(identity)
                         ? "bootstrap.admitted_timeseries_value_cache.v1"
                         : ExactParameterIdentity(identity)
                         ? "bootstrap.admitted_parameter.v1"
                         : ExactVariableIdentity(identity)
                               ? "bootstrap.admitted_variable.v1"
                               : "bootstrap.admitted_literal.v1";
  pair.snapshot.decision_evidence_sha256 = Sha256(DecisionPayload(
      pair.snapshot.database_uuid, {}, 0, pair.snapshot, pair.reason_code));
  if (pair.snapshot.snapshot_uuid.empty() ||
      pair.snapshot.row_identity_sha256.empty() ||
      pair.snapshot.decision_evidence_sha256.empty() ||
      !PublishPair(StorePath(context, identity), pair)) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.executor_registry.bootstrap_failed", "durable bootstrap failed");
    return result;
  }
  result.ok = true;
  result.snapshot = pair.snapshot;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  return result;
}

SblrExecutorAvailabilityLoadResult LoadLocked(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& identity,
    bool allow_bootstrap) {
  SblrExecutorAvailabilityLoadResult result;
  const std::string path = StorePath(context, identity);
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error || context.database_path.empty() ||
      !ValidIdentityUuid(context.database_uuid.canonical,
                         scratchbird::core::platform::UuidKind::database)) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.executor_registry.database_identity_invalid", "fail closed");
    return result;
  }
  if (!exists) {
    return allow_bootstrap ? BootstrapLocked(context, identity) : result;
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  if (!input.eof() || lines.empty() || (lines.size() % 2) != 0) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.executor_registry.torn_or_missing_evidence", "fail closed");
    return result;
  }
  SblrExecutorAvailabilitySnapshot prior;
  std::set<std::uint64_t> generations;
  for (std::size_t i = 0; i < lines.size(); i += 2) {
    DecodedPair evidence;
    DecodedPair snapshot;
    if (!DecodeRecord(SplitTabs(lines[i]), "EVIDENCE", &evidence) ||
        !DecodeRecord(SplitTabs(lines[i + 1]), "SNAPSHOT", &snapshot) ||
        JoinRecord("EVIDENCE", evidence.snapshot, evidence.prior_snapshot_uuid,
                   evidence.prior_generation, evidence.reason_code) != lines[i] ||
        JoinRecord("SNAPSHOT", snapshot.snapshot, snapshot.prior_snapshot_uuid,
                   snapshot.prior_generation, snapshot.reason_code) != lines[i + 1] ||
        evidence.snapshot.snapshot_uuid != snapshot.snapshot.snapshot_uuid ||
        evidence.snapshot.generation != snapshot.snapshot.generation ||
        evidence.snapshot.database_uuid != snapshot.snapshot.database_uuid ||
        evidence.snapshot.row_identity_sha256 !=
            snapshot.snapshot.row_identity_sha256 ||
        evidence.snapshot.installed != snapshot.snapshot.installed ||
        evidence.snapshot.availability_state !=
            snapshot.snapshot.availability_state ||
        evidence.snapshot.decision_evidence_sha256 !=
            snapshot.snapshot.decision_evidence_sha256 ||
        evidence.prior_snapshot_uuid != snapshot.prior_snapshot_uuid ||
        evidence.prior_generation != snapshot.prior_generation ||
        evidence.reason_code != snapshot.reason_code ||
        evidence.snapshot.database_uuid != context.database_uuid.canonical ||
        evidence.snapshot.row_identity_sha256 !=
            ComputeSblrExecutorAvailabilityRowIdentitySha256(identity) ||
        evidence.snapshot.generation != prior.generation + 1 ||
        evidence.prior_generation != prior.generation ||
        evidence.prior_snapshot_uuid != prior.snapshot_uuid ||
        !generations.emplace(evidence.snapshot.generation).second ||
        Sha256(DecisionPayload(evidence.snapshot.database_uuid,
                               evidence.prior_snapshot_uuid,
                               evidence.prior_generation, evidence.snapshot,
                               evidence.reason_code)) !=
            evidence.snapshot.decision_evidence_sha256) {
      result.diagnostic = RegistryDiagnostic(
          "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "sblr.executor_registry.contradictory_evidence", "fail closed");
      return result;
    }
    prior = evidence.snapshot;
  }
  result.ok = true;
  result.snapshot = prior;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  return result;
}

bool HasAdminAuthority(const EngineRequestContext& context) {
  if (!context.security_context_present) return false;
  for (const auto& tag : context.trace_tags) {
    if (tag == "right:SBLR_EXECUTOR_AVAILABILITY_ADMIN") return true;
  }
  return false;
}

}  // namespace

std::string ComputeSblrExecutorAvailabilityRowIdentitySha256(
    const SblrExecutorAvailabilityRowIdentity& identity) {
  if (!ExactAdmittedIdentity(identity)) return {};
  std::string payload;
  AddField(&payload, "executor_id", identity.executor_id);
  AddField(&payload, "opcode_code", std::to_string(identity.opcode_code));
  AddField(&payload, "opcode_version", identity.opcode_version);
  AddField(&payload, "operand_descriptor_id", identity.operand_descriptor_id);
  AddField(&payload, "result_descriptor_id", identity.result_descriptor_id);
  AddField(&payload, "result_descriptor_version",
           std::to_string(identity.result_descriptor_version));
  return Sha256(payload);
}

SblrExecutorAvailabilityLoadResult LoadSblrExecutorAvailabilitySnapshot(
    const EngineRequestContext& context) {
  return LoadSblrExecutorAvailabilitySnapshot(
      context, SblrExecutorAvailabilityRowIdentity{});
}

SblrExecutorAvailabilityLoadResult LoadSblrExecutorAvailabilitySnapshot(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& exact_row_identity) {
  std::lock_guard<std::recursive_mutex> guard(RegistryMutex());
  if (!ExactAdmittedIdentity(exact_row_identity)) {
    SblrExecutorAvailabilityLoadResult result;
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPERAND_INVALID", "sblr.executor_registry.row_invalid",
        "exact admitted executor row required");
    return result;
  }
  return LoadLocked(context, exact_row_identity, true);
}

SblrExecutorAvailabilitySetResult SetSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilitySetRequest& request) {
  SblrExecutorAvailabilitySetResult result;
  if (!HasAdminAuthority(context)) {
    result.diagnostic = RegistryDiagnostic(
        "SECURITY.ACCESS_DENIED", "security.access_denied",
        "executor availability administration not admitted");
    return result;
  }
  if (request.database_uuid != context.database_uuid.canonical ||
      !ExactAdmittedIdentity(request.exact_row_identity) ||
      StateName(request.requested_state).empty() ||
      !SafeReason(request.reason_code)) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPERAND_INVALID", "sblr.executor_registry.set_invalid",
        "exact database row identity state and reason required");
    return result;
  }
  std::lock_guard<std::recursive_mutex> guard(RegistryMutex());
  const auto loaded = LoadLocked(context, request.exact_row_identity, true);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  if (loaded.snapshot.snapshot_uuid != request.expected_snapshot_uuid ||
      loaded.snapshot.generation != request.expected_generation) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
        "sblr.executor_registry.compare_failed", "snapshot changed");
    return result;
  }
  DecodedPair next;
  next.prior_snapshot_uuid = loaded.snapshot.snapshot_uuid;
  next.prior_generation = loaded.snapshot.generation;
  next.reason_code = request.reason_code;
  next.snapshot.snapshot_uuid = NewSnapshotUuid(next.prior_generation + 1);
  next.snapshot.generation = next.prior_generation + 1;
  next.snapshot.database_uuid = request.database_uuid;
  next.snapshot.row_identity_sha256 =
      ComputeSblrExecutorAvailabilityRowIdentitySha256(
          request.exact_row_identity);
  next.snapshot.availability_state = request.requested_state;
  next.snapshot.installed =
      request.requested_state == SblrExecutorAvailabilityState::installed;
  next.snapshot.decision_evidence_sha256 = Sha256(DecisionPayload(
      request.database_uuid, next.prior_snapshot_uuid, next.prior_generation,
      next.snapshot, next.reason_code));
  if (next.snapshot.snapshot_uuid.empty() ||
      next.snapshot.decision_evidence_sha256.empty() ||
      !PublishPair(StorePath(context, request.exact_row_identity), next)) {
    result.diagnostic = RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.executor_registry.publish_failed", "durable evidence or snapshot failed");
    return result;
  }
  result.ok = true;
  result.snapshot = next.snapshot;
  result.diagnostic = MakeEngineApiDiagnostic("OK", "ok", {}, false);
  result.evidence = {{"registry_id", std::string(kRegistryId)},
                     {"snapshot_uuid", next.snapshot.snapshot_uuid},
                     {"generation", std::to_string(next.snapshot.generation)},
                     {"row_identity_sha256", next.snapshot.row_identity_sha256},
                     {"availability_state", StateName(next.snapshot.availability_state)},
                     {"decision_evidence_sha256",
                      next.snapshot.decision_evidence_sha256}};
  return result;
}

EngineApiDiagnostic RevalidateSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilitySnapshot& admitted_snapshot,
    SblrExecutorAvailabilitySnapshot* current_snapshot) {
  return RevalidateSblrExecutorAvailability(
      context, SblrExecutorAvailabilityRowIdentity{}, admitted_snapshot,
      current_snapshot);
}

EngineApiDiagnostic RevalidateSblrExecutorAvailability(
    const EngineRequestContext& context,
    const SblrExecutorAvailabilityRowIdentity& exact_row_identity,
    const SblrExecutorAvailabilitySnapshot& admitted_snapshot,
    SblrExecutorAvailabilitySnapshot* current_snapshot) {
  const auto loaded = LoadSblrExecutorAvailabilitySnapshot(
      context, exact_row_identity);
  if (!loaded.ok) return loaded.diagnostic;
  if (current_snapshot != nullptr) *current_snapshot = loaded.snapshot;
  if (loaded.snapshot.availability_state ==
          SblrExecutorAvailabilityState::revoked) {
    return RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.opcode.executor_evidence_missing", "executor row revoked");
  }
  if (loaded.snapshot.availability_state ==
      SblrExecutorAvailabilityState::unavailable) {
    return RegistryDiagnostic("SBLR.OPCODE.EXECUTOR_UNAVAILABLE",
                              "sblr.opcode.executor_unavailable",
                              "executor row unavailable");
  }
  if (!loaded.snapshot.installed) {
    return RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "sblr.opcode.executor_evidence_missing", "executor row absent");
  }
  if (admitted_snapshot.database_uuid != loaded.snapshot.database_uuid ||
      admitted_snapshot.snapshot_uuid != loaded.snapshot.snapshot_uuid ||
      admitted_snapshot.generation != loaded.snapshot.generation ||
      admitted_snapshot.row_identity_sha256 !=
          loaded.snapshot.row_identity_sha256 ||
      !admitted_snapshot.installed ||
      admitted_snapshot.availability_state !=
          SblrExecutorAvailabilityState::installed) {
    return RegistryDiagnostic(
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
        "sblr.opcode.executor_evidence_stale", "installed snapshot changed");
  }
  return MakeEngineApiDiagnostic("OK", "ok", {}, false);
}

bool IsAdmittedExecutorAvailabilityIdentity(const SblrExecutorAvailabilityRowIdentity& identity) {
  return ExactAdmittedIdentity(identity);
}

}  // namespace scratchbird::engine::internal_api
