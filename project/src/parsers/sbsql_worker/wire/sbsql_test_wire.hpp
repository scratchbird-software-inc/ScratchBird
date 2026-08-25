// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "auth/auth_relay.hpp"
#include "cache/sblr_template_cache.hpp"
#include "ipc/sbps_client.hpp"
#include "metrics/parser_metrics.hpp"
#include "engine/sblr/sblr_variable_runtime.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <memory>

namespace scratchbird::parser::sbsql {

struct CstDocument;
class EmbeddedEngineClient;
struct ServerManagementCommand {
  std::string operation_key;
  std::string operation_id;
  std::string mode;
  std::string audit_reason;
};

struct CachedPublicNameResolution {
  std::string object_uuid;
  std::string canonical_name;
  std::string object_class;
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_epoch{0};
};

struct StableCachedPublicNameResolution {
  std::string presented_name;
  bool quoted{false};
  std::string lookup_object_class;
  CachedPublicNameResolution resolved;
};

struct WireResponse {
  bool close{false};
  std::string text;
};

enum class PreparedParameterPayloadEncoding : std::uint8_t {
  utf8_text = 0,
  binary = 1,
};

struct PreparedParameterWireValue {
  bool is_null{false};
  PreparedParameterPayloadEncoding encoding{
      PreparedParameterPayloadEncoding::binary};
  std::vector<std::uint8_t> raw_bytes;
  std::uint32_t public_type_metadata{0};
};

struct PreparedParameterCanonicalValue {
  bool accepted{false};
  bool is_null{false};
  std::vector<std::uint8_t> canonical_bytes;
  std::string diagnostic_code;
};

PreparedParameterCanonicalValue CanonicalizePreparedParameterWireValue(
    const PreparedParameterWireValue& value,
    std::string_view authenticated_descriptor_uuid,
    std::string_view authenticated_type_uuid,
    bool nullable);

class SbsqlTestWireSession {
 public:
  SbsqlTestWireSession(ParserConfig config, ParserMetrics* metrics, SblrTemplateCache* cache);
  ~SbsqlTestWireSession();
  WireResponse HandleLine(std::string_view line);
  int ServeFd(std::intptr_t fd);
  PipelineResult RunPipeline(std::string_view sql,
                             bool submit,
                             bool cursor_requested = false,
                             std::uint64_t stream_row_count = 0,
                             bool autocommit_emulation = false,
                             const std::vector<PreparedParameterWireValue>&
                                 parameter_values = {},
                             const ipc::ParameterExecutionCoordination*
                                 parameter_coordination = nullptr,
                             bool parameter_prepare_only = false,
                             ipc::PreparedParameterReference*
                                 prepared_parameter_output = nullptr,
                             std::string_view expected_prepared_uuid = {},
                             std::uint64_t expected_prepared_generation = 0,
                             const ipc::VariableFrameCoordination*
                                 variable_coordination = nullptr,
                             const scratchbird::engine::sblr::SblrVariableFrameBeginResultV1*
                                 variable_frame = nullptr);
  PipelineResult RunVariableForWire(std::string_view sql,
                                    bool cursor_requested = false);
  PipelineResult RunSourceMapForWire();
  PipelineResult RunErrorVectorForWire();
  PipelineResult RunAutonomousFrameForWire();
  PipelineResult RunReservationReleaseForWire();
  PipelineResult RunTemporaryInstanceCleanupForWire();
  PipelineResult RunShowVersionForWire();
  PipelineResult RunShowWaitEventsForWire();
  PipelineResult RunCursorOpenForWire();
  PipelineResult RunCursorFetchForWire();
  PipelineResult RunCursorCloseForWire();
  PipelineResult RunReadByKeyForWire();
  PipelineResult RunReadRangeForWire();
  PipelineResult RunReadStreamForWire();
  PipelineResult RunResultSetPassForWire();
  PipelineResult RunAccessCursorOpenForWire();
  PipelineResult RunAccessCursorFetchForWire();
  PipelineResult RunAccessCursorCloseForWire();
  PipelineResult RunInsertForWire();
  PipelineResult RunUpdateForWire();
  PipelineResult RunDeleteForWire();
  PipelineResult RunMergeForWire();
  PipelineResult RunTableTruncateForWire();
  PipelineResult RunTableAnalyzeForWire();
  PipelineResult RunBulkImportStreamForWire();
  PipelineResult RunBulkExportStreamForWire();
  PipelineResult RunStatementBatchForWire();
  PipelineResult RunAtomicCasForWire();
  PipelineResult RunAtomicRmwForWire();
  PipelineResult RunAdvisoryLockForWire();
  PipelineResult RunAdvisoryLockReleaseForWire();
  PipelineResult RunFunctionCallForWire();
  PipelineResult RunOperatorCallForWire();
  PipelineResult RunCastForWire();
  PipelineResult RunCompareForWire();
  PipelineResult RunDomainOperationForWire();
  PipelineResult RunUdrInvokeForWire();
  PipelineResult RunProcedureInvokeForWire();
  PipelineResult RunFunctionInvokeForWire();
  PipelineResult RunAggregateInvokeForWire();
  PipelineResult RunSequenceNextvalForWire();
  PipelineResult RunSequenceCurrvalForWire();
  PipelineResult RunSequenceSetvalForWire();
  PipelineResult RunQueryNumericForWire();
  PipelineResult RunQueryEvaluateAdvancedDatatypeFamilyForWire();
  PipelineResult RunProjectForWire();
  PipelineResult RunShowObjectDetailForWire();
  PipelineResult RunNameResolveForWire();
  PipelineResult RunParseTextForWire();
  PipelineResult RunCatalogEpochCheckForWire();
  PipelineResult RunDatabaseAttachForWire();
  PipelineResult RunDatabaseDetachForWire();
  PipelineResult RunDatabaseCheckpointForWire();
  PipelineResult RunDatabaseVacuumForWire();
  PipelineResult RunDatabaseAlterForWire();
  PipelineResult RunLifecycleCreateDatabaseForWire();
  PipelineResult RunLifecycleOpenDatabaseForWire();
  PipelineResult RunLifecycleAttachDatabaseForWire();
  PipelineResult RunLifecycleDetachDatabaseForWire();
  PipelineResult RunLifecycleEnterMaintenanceForWire();
  PipelineResult RunLifecycleExitMaintenanceForWire();
  PipelineResult RunLifecycleEnterRestrictedOpenForWire();
  PipelineResult RunLifecycleExitRestrictedOpenForWire();
  PipelineResult RunLifecycleInspectDatabaseForWire();
  PipelineResult RunLifecycleVerifyDatabaseForWire();
  PipelineResult RunLifecycleRepairDatabaseForWire();
  PipelineResult RunLifecycleShutdownDatabaseForWire();
  PipelineResult RunLifecycleShutdownForceForWire();
  PipelineResult RunLifecycleShutdownAcknowledgeForWire();
  PipelineResult RunLifecycleDropDatabaseForWire();
  PipelineResult RunReplConsumerSubscribeForWire();
  PipelineResult RunReplConsumerResumeForWire();
  PipelineResult RunReplConsumerPauseForWire();
  PipelineResult RunReplConsumerCancelForWire();
  PipelineResult RunReplCdcReceiveForWire();
  PipelineResult RunReplCdcAckForWire();
  PipelineResult RunRepl2pcPrewriteForWire();
  PipelineResult RunRepl2pcCommitForWire();
  PipelineResult RunRepl2pcCleanupForWire();
  PipelineResult RunRepl2pcResolveLockForWire();
  PipelineResult RunRepl2pcPessimisticLockForWire();
  PipelineResult RunRepl2pcPessimisticRollbackForWire();
  PipelineResult RunRepl2pcHeartbeatForWire();
  PipelineResult RunAggregateForWire();
  PipelineResult RunGroupForWire();
  PipelineResult RunSortForWire();
  PipelineResult RunLimitForWire();
  PipelineResult RunWindowForWire();
  PipelineResult RunReturnResultSetForWire();
  PipelineResult RunKvStructuredReadForWire();
  PipelineResult RunKvStructuredMutateForWire();
  PipelineResult RunKvStructuredScanForWire();
  PipelineResult RunKvStructuredStreamReadForWire();
  PipelineResult RunKvStructuredStreamAppendForWire();
  PipelineResult RunKvStructuredTimeseriesForWire();
  PipelineResult RunSystemConfigSetForWire();
  PipelineResult RunDdlCreateDomainForWire();
  PipelineResult RunDdlAlterDomainForWire();
  PipelineResult RunDdlCreateViewForWire();
  PipelineResult RunDdlCreateMaterializedViewForWire();
  PipelineResult RunDdlAlterViewForWire();
  PipelineResult RunDdlDropViewForWire();
  PipelineResult RunDdlRefreshMaterializedViewForWire();
  PipelineResult RunDdlDropPackageForWire();
  PipelineResult RunDdlDropSynonymForWire();
  PipelineResult RunDdlDropForeignTableForWire();
  PipelineResult RunDdlAlterPackageForWire();
  PipelineResult RunDdlAlterSequenceForWire();
  PipelineResult RunDdlDropSequenceForWire();
  PipelineResult RunDdlDropMaterializedViewForWire();
  PipelineResult RunDdlCreateTypeForWire();
  PipelineResult RunDdlCreateTableAsQueryWithDataForWire();
  PipelineResult RunDdlCreateTableAsQueryWithNoDataForWire();
  PipelineResult RunDdlAlterTypeForWire();
  PipelineResult RunDdlDropTypeForWire();
  PipelineResult RunDdlDropTableForWire();
  PipelineResult RunGpuProfileDisableRefusalForWire();
  PipelineResult RunDdlCreateTriggerForWire();
  PipelineResult RunDdlAlterTriggerForWire();
  PipelineResult RunDdlDropTriggerForWire();
  PipelineResult RunDdlCreateProcedureForWire();
  PipelineResult RunDdlAlterProcedureForWire();
  PipelineResult RunDdlDropProcedureForWire();
  PipelineResult RunDdlCreateFunctionForWire();
  PipelineResult RunDdlAlterFunctionForWire();
  PipelineResult RunDdlDropFunctionForWire();
  PipelineResult RunDdlCreatePackageForWire();
  PipelineResult RunDdlCreateSequenceForWire();
  PipelineResult RunDdlCreateTemporaryTableForWire();
  PipelineResult RunDdlDropTemporaryTableForWire();
  PipelineResult RunDdlRenameObjectVectorForWire();
  PipelineResult RunDdlRenameObjectForWire();
  PipelineResult RunDdlCreateSynonymForWire();
  PipelineResult RunDdlCreateForeignTableForWire();
  PipelineResult RunDdlCreateFdwForWire();
  PipelineResult RunDdlDropFdwForWire();
  PipelineResult RunDdlCreateOrReplaceSrsForWire();
  PipelineResult RunDdlDropSrsForWire();
  PipelineResult RunDdlCreateRewriteRuleForWire();
  PipelineResult RunDdlAlterRewriteRuleForWire();
  PipelineResult RunDdlDropRewriteRuleForWire();
  PipelineResult RunDdlValidateConstraintForWire();
  PipelineResult RunSecurityCreatePrivilegeTemplateForWire();
  PipelineResult RunSecurityCreateUserForWire();
  PipelineResult RunSecurityCreateRoleForWire();
  PipelineResult RunSecurityDropRoleForWire();
  PipelineResult RunSecurityCreatePolicyForWire();
  PipelineResult RunSecurityDropPolicyForWire();
  PipelineResult RunSecurityAlterPolicyForWire();
  PipelineResult RunSecurityDropUserForWire();
  PipelineResult RunSecurityAuthenticateForWire();
  PipelineResult RunSecurityDeauthenticateForWire();
  PipelineResult RunSessionRoleSwitchForWire();
  PipelineResult RunSessionSettingSetForWire();
  PipelineResult RunSessionSettingResetForWire();
  PipelineResult RunSessionSettingGetForWire();
  PipelineResult RunSessionDefaultQualifierSetForWire();
  PipelineResult RunSessionDiscardForWire();
  PipelineResult RunSessionSnapshotHandleForWire();
  PipelineResult RunContextSetForWire();
  PipelineResult RunContextUnsetForWire();
  PipelineResult RunContextGetForWire();
  PipelineResult RunStmtPrepareForWire();
  PipelineResult RunStmtPrepareCanonicalForWire();
  PipelineResult RunStmtExecuteForWire();
  PipelineResult RunStmtExecuteDirectForWire();
  PipelineResult RunStmtFreeForWire();
  PipelineResult RunStmtCancelForWire();
  PipelineResult RunParameterBindForWire();
  PipelineResult RunResultPageForWire();
  PipelineResult RunQueryExecuteForWire();
  PipelineResult RunQueryExplainForWire();
  PipelineResult RunSecurityAlterRoleForWire();
  PipelineResult RunSecurityCreateGroupMappingForWire();
  PipelineResult RunSecurityDropGroupMappingForWire();
  PipelineResult RunSecurityGrantForWire();
  PipelineResult RunSecurityRevokeForWire();
  PipelineResult RunSecurityAlterUserForWire();
  PipelineResult RunSecurityAlterPrivilegeTemplateForWire();
  PipelineResult RunSecurityDropPrivilegeTemplateForWire();
  PipelineResult RunDatabaseCreateTemplateCloneForWire();
  PipelineResult RunDdlCreateAggregateForWire();
  PipelineResult RunDdlAlterAggregateForWire();
  PipelineResult RunDdlDropAggregateForWire();
  PipelineResult RunDdlPurgeSystemHistoryForWire();
  PipelineResult RunDdlSetIndexOptimizerEligibilityForWire();
  PipelineResult RunDdlSetTableTypeEnforcementForWire();
  PipelineResult RunDatabaseSerializeLogicalSnapshotForWire();
  PipelineResult RunDatabaseDeserializeLogicalSnapshotForWire();
  PipelineResult RunDdlCreateMacroForWire();
  PipelineResult RunDdlCreateDictionaryForWire();
  PipelineResult RunDdlDropDictionaryForWire();
  PipelineResult RunDdlAlterDictionaryForWire();
  PipelineResult RunDdlCreateContinuousViewForWire();
  PipelineResult RunDdlAlterContinuousViewForWire();
  PipelineResult RunDdlDropContinuousViewForWire();
  PipelineResult RunDmlAsyncInsertSubmitForWire();
  PipelineResult RunDmlAsyncInsertStatusForWire();
  PipelineResult RunDmlCounterAddForWire();
  PipelineResult RunDmlTimeseriesSchemaWriteForWire();
  PipelineResult RunDdlTimeseriesSeriesCardinalityPolicyForWire();
  PipelineResult RunDdlCreateTimeseriesValueCacheForWire();
  PipelineResult RunDmlAsyncInsertCancelForWire();
  PipelineResult RunDdlDropMacroForWire();
  PipelineResult RunAdminRegisterExternalRelationResolverForWire();
  PipelineResult RunAdminUnregisterExternalRelationResolverForWire();
  PipelineResult RunDdlCreateSchemaForWire();
  PipelineResult RunDdlCreateTableForWire();
  PipelineResult RunDdlCreateIndexForWire();
  PipelineResult RunDdlDropIndexForWire();
  PipelineResult RunRetiredTransactionCommitReplayForWire();
  PipelineResult RunRetiredTransactionRollbackReplayForWire();
  PipelineResult RunRetiredSavepointReplayForWire();
  PipelineResult RunReleasedSavepointReplayForWire();
  PipelineResult RunReleaseParentSavepointForWire();
  PipelineResult RunRollbackParentSavepointForWire();
  PipelineResult RunRolledBackSavepointReplayForWire();
  PipelineResult RunRolledBackDescendantForWire();
  ipc::ServerPreparedParameterFinalizeResult PrepareParameterizedForWire(
      std::string_view sql);
  PipelineResult RunDirectParameterizedForWire(
      std::string_view sql,
      const std::vector<PreparedParameterWireValue>& parameter_values,
      bool cursor_requested = false);
  PipelineResult RunPreparedParameterizedForWire(
      std::string_view sql,
      std::string_view prepared_statement_uuid,
      std::uint64_t prepared_generation,
      const std::vector<PreparedParameterWireValue>& parameter_values,
      bool cursor_requested = false);
  PipelineResult RunSblrEnvelope(std::string_view encoded_sblr_envelope,
                                 bool cursor_requested = false);
  PipelineResult RunSblrEnvelopeWithDataPacket(
      std::string_view encoded_sblr_envelope,
      const std::vector<std::uint8_t>& data_packet,
      bool cursor_requested = false);
  ServerPrepareSblrResult PrepareSblrForWire(std::string_view encoded_sblr_envelope);
  PipelineResult RunPreparedSblrEnvelopeForWire(std::string_view prepared_statement_uuid,
                                                std::string_view encoded_sblr_envelope,
                                                const std::vector<std::uint8_t>& data_packet = {},
                                                bool cursor_requested = false);
  ServerFetchResult FetchCursorOnRoute(std::string_view cursor_uuid,
                                       std::uint64_t max_rows = 1,
                                       std::uint64_t max_bytes = 0,
                                       std::uint32_t fetch_flags = 0);
  ServerCloseCursorResult CloseCursorOnRoute(std::string_view cursor_uuid);
  ServerCloseCursorResult CancelCursorOnRoute(std::string_view cursor_uuid);
  PublicNameResolutionResult ResolvePublicNameForWire(std::string_view presented_name,
                                                      bool quoted,
                                                      std::string_view object_class);
  bool AuthenticateCredentials(const AuthCredentialEnvelope& credentials,
                               MessageVectorSet* messages);
  [[nodiscard]] const SessionContext& session() const { return session_; }

 private:
  ParserConfig config_;
  ParserMetrics* metrics_;
  SblrTemplateCache* cache_;
  SessionContext session_;
  std::string last_cursor_uuid_;
  std::unique_ptr<EmbeddedEngineClient> embedded_client_;
  std::unique_ptr<SbpsClient> server_client_;
  std::map<std::string, CachedPublicNameResolution> name_resolution_cache_;
  std::vector<std::uint8_t> admitted_transaction_handle_;
  std::vector<std::uint8_t> retired_transaction_handle_;
  std::vector<std::uint8_t> admitted_savepoint_descriptor_;
  std::vector<std::uint8_t> retired_savepoint_descriptor_;
  std::vector<std::uint8_t> admitted_savepoint_handle_;
  std::vector<std::uint8_t> admitted_cursor_handle_;
  std::vector<std::uint8_t> parent_savepoint_handle_;
  std::vector<std::uint8_t> descendant_savepoint_handle_;
  std::vector<std::uint8_t> retired_savepoint_release_operand_;
  bool replaying_savepoint_release_{false};
  std::vector<std::uint8_t> retired_savepoint_rollback_operand_;
  bool replaying_savepoint_rollback_{false};
  bool replaying_savepoint_descriptor_{false};
  std::deque<std::string> name_resolution_lru_;
  std::map<std::string, StableCachedPublicNameResolution>
      stable_relation_name_resolution_cache_;
  std::map<std::string, ipc::CursorStreamDescriptorV1> cursor_stream_descriptors_;
  std::deque<std::string> stable_relation_name_resolution_lru_;

  bool HasExecutionRoute() const;
  ServerExecutionResult ExecuteSblrOnRoute(std::string_view encoded_sblr_envelope,
                                           bool cursor_requested = false);
  ServerExecutionResult ExecuteSblrOnRouteWithDataPacket(
      std::string_view encoded_sblr_envelope,
      const std::vector<std::uint8_t>& data_packet,
      bool cursor_requested = false);
  PublicNameResolutionResult ResolveNameOnRoute(std::string_view presented_name,
                                                bool quoted,
                                                std::string_view object_class);
  PublicNameResolutionResult ResolveNameOnRouteUncached(std::string_view presented_name,
                                                        bool quoted,
                                                        std::string_view object_class);
  void ClearNameResolutionCache(bool preserve_stable_relation_names = false);
  void RehydrateStableRelationNameResolutionCache();
  void StoreNameResolutionCacheEntry(std::string_view presented_name,
                                     bool quoted,
                                     std::string_view object_class,
                                     std::string_view object_uuid,
                                     std::string_view canonical_name,
                                     std::uint64_t catalog_epoch,
                                     std::uint64_t security_epoch,
                                     std::string_view resolved_object_class = {});
  void SeedCreatedDdlNameResolutionCache(const CstDocument& cst,
                                         const PipelineResult& result);
  bool DisconnectExecutionRoute(MessageVectorSet* messages);
  PipelineResult RunServerManagementCommand(const ServerManagementCommand& command);
  int ServeSbwp(std::intptr_t fd);
};

} // namespace scratchbird::parser::sbsql
