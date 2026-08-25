// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_IPC_FOUNDATION_SBPS

#pragma once

#include "diagnostics.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::server::sbps {

constexpr std::uint32_t kFrameMagic = 0x53504253;  // SBPS
constexpr std::uint32_t kMessageVectorMagic = 0x564d4253;  // SBMV
constexpr std::uint16_t kHeaderBytes = 96;
constexpr std::uint16_t kProtocolMajor = 1;
constexpr std::uint16_t kProtocolMinor = 0;
constexpr std::uint16_t kProtocolMajorMinSupported = 1;
constexpr std::uint16_t kProtocolMajorMaxSupported = 1;
constexpr std::uint16_t kProtocolMinorMinSupported = 0;
constexpr std::uint16_t kProtocolMinorMaxSupported = 0;
constexpr std::uint32_t kAllowedFrameFlags = 0x000003ff;
constexpr std::uint32_t kFlagResponse = 1u << 0;
constexpr std::uint32_t kFlagError = 1u << 1;
constexpr std::uint32_t kFlagFinal = 1u << 2;
constexpr std::uint32_t kFlagPayloadChunk = 1u << 3;
constexpr std::uint32_t kSchemaNone = 0;
constexpr std::uint32_t kSchemaHelloRequestV1 = 1001;
constexpr std::uint32_t kSchemaHelloAcceptV1 = 1002;
constexpr std::uint32_t kSchemaMessageVectorSetV1 = 2001;
constexpr std::uint32_t kSchemaManagementRequestV1 = 6001;
constexpr std::uint32_t kSchemaManagementResponseV1 = 6002;
constexpr std::uint32_t kSchemaResolveNameRequestV1 = 7001;
constexpr std::uint32_t kSchemaResolveNameResultV1 = 7002;
constexpr std::uint32_t kSchemaResolveNameRequestV2 = 7005;
constexpr std::uint32_t kSchemaResolveNameResultV2 = 7006;
constexpr std::uint32_t kSchemaResolveNameRequestV3 = 7007;
constexpr std::uint32_t kSchemaResolveNameResultV3 = 7008;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV1 = 7011;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV1 = 7012;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV2 = 7013;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV2 = 7014;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV3 = 7015;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV3 = 7016;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV4 = 7017;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV4 = 7018;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV5 = 7019;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV5 = 7020;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV6 = 7021;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV6 = 7022;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV7 = 7023;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV7 = 7024;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV8 = 7025;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV8 = 7026;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV9 = 7027;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV9 = 7028;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV10 = 7029;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV10 = 7030;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV11 = 7031;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV11 = 7032;
constexpr std::uint32_t kSchemaNegotiateLiteralDescriptorsRequestV1 = 7033;
constexpr std::uint32_t kSchemaNegotiateLiteralDescriptorsResultV1 = 7034;
constexpr std::uint32_t kSchemaFinalizeLiteralBindingRequestV1 = 7035;
constexpr std::uint32_t kSchemaFinalizeLiteralBindingResultV1 = 7036;
constexpr std::uint32_t kSchemaExecuteCanonicalSblrV1 = 4015;
constexpr std::uint32_t kSchemaExecuteCanonicalSblrLiteralV1 = 4016;
constexpr std::uint32_t kSchemaNegotiateParameterDescriptorsRequestV1 = 7037;
constexpr std::uint32_t kSchemaNegotiateParameterDescriptorsResultV1 = 7038;
constexpr std::uint32_t kSchemaFinalizeParameterBindingRequestV1 = 7039;
constexpr std::uint32_t kSchemaFinalizeParameterBindingResultV1 = 7040;
constexpr std::uint32_t kSchemaBeginParameterExecutionCoordinationRequestV1 = 7041;
constexpr std::uint32_t kSchemaBeginParameterExecutionCoordinationResultV1 = 7042;
constexpr std::uint32_t kSchemaAcquireParameterStatementContextRequestV1 = 7043;
constexpr std::uint32_t kSchemaFinalizePreparedSblrParameterRequestV1 = 7044;
constexpr std::uint32_t kSchemaFinalizePreparedSblrParameterResultV1 = 7045;
constexpr std::uint32_t kSchemaExecuteCanonicalSblrParameterV1 = 4017;
constexpr std::uint32_t kSchemaExecuteCanonicalSblrVariableV1 = 4018;
constexpr std::uint32_t kSchemaNegotiateVariableDescriptorsRequestV1 = 7050;
constexpr std::uint32_t kSchemaNegotiateVariableDescriptorsResultV1 = 7051;
constexpr std::uint32_t kSchemaFinalizeVariableBindingRequestV1 = 7052;
constexpr std::uint32_t kSchemaFinalizeVariableBindingResultV1 = 7053;
constexpr std::uint32_t kSchemaBeginVariableFrameRequestV1 = 7054;
constexpr std::uint32_t kSchemaBeginVariableFrameResultV1 = 7055;
constexpr std::uint32_t kSchemaAcquireVariableStatementContextRequestV1 = 7056;
constexpr std::uint32_t kSchemaCloseVariableFrameRequestV1 = 7057;
constexpr std::uint32_t kSchemaCloseVariableFrameResultV1 = 7058;
constexpr std::uint32_t kSchemaAssignVariableValuesRequestV1 = 7059;
constexpr std::uint32_t kSchemaAssignVariableValuesResultV1 = 7060;
constexpr std::uint32_t kSchemaIssueSourceMapRequestV1 = 7061;
constexpr std::uint32_t kSchemaIssueSourceMapResultV1 = 7062;
constexpr std::uint32_t kSchemaIssueErrorVectorRequestV1 = 7063;
constexpr std::uint32_t kSchemaIssueErrorVectorResultV1 = 7064;
constexpr std::uint32_t kSchemaReserveSavepointRequestV1 = 7065;
constexpr std::uint32_t kSchemaReserveSavepointResultV1 = 7066;
constexpr std::uint32_t kSchemaReserveAutonomousFrameRequestV1 = 7067;
constexpr std::uint32_t kSchemaReserveAutonomousFrameResultV1 = 7068;
constexpr std::uint32_t kSchemaCoordinateReservationReleaseRequestV1 = 7069;
constexpr std::uint32_t kSchemaCoordinateReservationReleaseResultV1 = 7070;
constexpr std::uint32_t kSchemaCoordinateTemporaryInstanceCleanupRequestV1 = 7071;
constexpr std::uint32_t kSchemaCoordinateTemporaryInstanceCleanupResultV1 = 7072;
constexpr std::uint32_t kSchemaCoordinateCursorOpenRequestV1 = 7073;
constexpr std::uint32_t kSchemaCoordinateCursorOpenResultV1 = 7074;
constexpr std::uint32_t kSchemaCoordinateReadByKeyRequestV1 = 7075;
constexpr std::uint32_t kSchemaCoordinateReadByKeyResultV1 = 7076;
constexpr std::uint32_t kSchemaCoordinateReadRangeRequestV1 = 7077;
constexpr std::uint32_t kSchemaCoordinateReadRangeResultV1 = 7078;
constexpr std::uint32_t kSchemaCoordinateReadStreamRequestV1 = 7079;
constexpr std::uint32_t kSchemaCoordinateReadStreamResultV1 = 7080;
constexpr std::uint32_t kSchemaCoordinateResultSetPassRequestV1 = 7081;
constexpr std::uint32_t kSchemaCoordinateResultSetPassResultV1 = 7082;
constexpr std::uint32_t kSchemaCoordinateAccessCursorOpenRequestV1 = 7083;
constexpr std::uint32_t kSchemaCoordinateAccessCursorOpenResultV1 = 7084;
constexpr std::uint32_t kSchemaCoordinateAccessCursorFetchRequestV1 = 7085;
constexpr std::uint32_t kSchemaCoordinateAccessCursorFetchResultV1 = 7086;
constexpr std::uint32_t kSchemaCoordinateAccessCursorCloseRequestV1 = 7087;
constexpr std::uint32_t kSchemaCoordinateAccessCursorCloseResultV1 = 7088;
constexpr std::uint32_t kSchemaCoordinateInsertRequestV1 = 7089;
constexpr std::uint32_t kSchemaCoordinateInsertResultV1 = 7090;
constexpr std::uint32_t kSchemaCoordinateUpdateRequestV1 = 7091;
constexpr std::uint32_t kSchemaCoordinateUpdateResultV1 = 7092;
constexpr std::uint32_t kSchemaCoordinateDeleteRequestV1 = 7093;
constexpr std::uint32_t kSchemaCoordinateDeleteResultV1 = 7094;
constexpr std::uint32_t kSchemaCoordinateMergeRequestV1 = 7095;
constexpr std::uint32_t kSchemaCoordinateMergeResultV1 = 7096;
constexpr std::uint32_t kSchemaCoordinateTableTruncateRequestV1 = 7097;
constexpr std::uint32_t kSchemaCoordinateTableTruncateResultV1 = 7098;
constexpr std::uint32_t kSchemaCoordinateTableAnalyzeRequestV1 = 7099;
constexpr std::uint32_t kSchemaCoordinateTableAnalyzeResultV1 = 7100;
constexpr std::uint32_t kSchemaCoordinateBulkImportStreamRequestV1 = 7101;
constexpr std::uint32_t kSchemaCoordinateBulkImportStreamResultV1 = 7102;
constexpr std::uint32_t kSchemaCoordinateBulkExportStreamRequestV1 = 7103;
constexpr std::uint32_t kSchemaCoordinateBulkExportStreamResultV1 = 7104;
constexpr std::uint32_t kSchemaCoordinateStatementBatchRequestV1 = 7105;
constexpr std::uint32_t kSchemaCoordinateStatementBatchResultV1 = 7106;
constexpr std::uint32_t kSchemaCoordinateAtomicCasRequestV1 = 7107;
constexpr std::uint32_t kSchemaCoordinateAtomicCasResultV1 = 7108;
constexpr std::uint32_t kSchemaCoordinateAtomicRmwRequestV1 = 7109;
constexpr std::uint32_t kSchemaCoordinateAtomicRmwResultV1 = 7110;
constexpr std::uint32_t kSchemaCoordinateAdvisoryLockRequestV1 = 7111;
constexpr std::uint32_t kSchemaCoordinateAdvisoryLockResultV1 = 7112;
constexpr std::uint32_t kSchemaCoordinateAdvisoryLockReleaseRequestV1 = 7113;
constexpr std::uint32_t kSchemaCoordinateAdvisoryLockReleaseResultV1 = 7114;
constexpr std::uint32_t kSchemaCoordinateFunctionCallRequestV1 = 7115;
constexpr std::uint32_t kSchemaCoordinateFunctionCallResultV1 = 7116;
constexpr std::uint32_t kSchemaCoordinateOperatorCallRequestV1 = 7117;
constexpr std::uint32_t kSchemaCoordinateOperatorCallResultV1 = 7118;
constexpr std::uint32_t kSchemaCoordinateCastRequestV1 = 7119;
constexpr std::uint32_t kSchemaCoordinateCastResultV1 = 7120;
constexpr std::uint32_t kSchemaCoordinateCompareRequestV1 = 7121;
constexpr std::uint32_t kSchemaCoordinateCompareResultV1 = 7122;
constexpr std::uint32_t kSchemaCoordinateDomainOperationRequestV1 = 7123;
constexpr std::uint32_t kSchemaCoordinateDomainOperationResultV1 = 7124;
constexpr std::uint32_t kSchemaCoordinateUdrInvokeRequestV1 = 7125;
constexpr std::uint32_t kSchemaCoordinateUdrInvokeResultV1 = 7126;
constexpr std::uint32_t kSchemaCoordinateProcedureInvokeRequestV1 = 7127;
constexpr std::uint32_t kSchemaCoordinateProcedureInvokeResultV1 = 7128;
constexpr std::uint32_t kSchemaCoordinateFunctionInvokeRequestV1 = 7129;
constexpr std::uint32_t kSchemaCoordinateFunctionInvokeResultV1 = 7130;
constexpr std::uint32_t kSchemaCoordinateAggregateInvokeRequestV1 = 7131;
constexpr std::uint32_t kSchemaCoordinateAggregateInvokeResultV1 = 7132;
constexpr std::uint32_t kSchemaCoordinateSequenceNextvalRequestV1 = 7133;
constexpr std::uint32_t kSchemaCoordinateSequenceNextvalResultV1 = 7134;
constexpr std::uint32_t kSchemaCoordinateSequenceCurrvalRequestV1 = 7135;
constexpr std::uint32_t kSchemaCoordinateSequenceCurrvalResultV1 = 7136;
constexpr std::uint32_t kSchemaCoordinateSequenceSetvalRequestV1 = 7137;
constexpr std::uint32_t kSchemaCoordinateSequenceSetvalResultV1 = 7138;
constexpr std::uint32_t kSchemaCoordinateQueryNumericRequestV1 = 7139;
constexpr std::uint32_t kSchemaCoordinateQueryNumericResultV1 = 7140;
constexpr std::uint32_t kSchemaCoordinateAdvancedDatatypeFamilyRequestV1 = 7141;
constexpr std::uint32_t kSchemaCoordinateAdvancedDatatypeFamilyResultV1 = 7142;
constexpr std::uint32_t kSchemaCoordinateProjectRequestV1 = 7143;
constexpr std::uint32_t kSchemaCoordinateProjectResultV1 = 7144;
constexpr std::uint32_t kSchemaCoordinateCatalogIntrospectRequestV1 = 7263;
constexpr std::uint32_t kSchemaCoordinateCatalogIntrospectResultV1 = 7264;
constexpr std::uint32_t kSchemaCoordinateKvStructuredReadRequestV1 = 7157;
constexpr std::uint32_t kSchemaCoordinateKvStructuredReadResultV1 = 7158;
constexpr std::uint32_t kSchemaCoordinateKvStructuredMutateRequestV1 = 7159;
constexpr std::uint32_t kSchemaCoordinateKvStructuredMutateResultV1 = 7160;
constexpr std::uint32_t kSchemaCoordinateKvStructuredScanRequestV1 = 7161;
constexpr std::uint32_t kSchemaCoordinateKvStructuredScanResultV1 = 7162;
constexpr std::uint32_t kSchemaCoordinateKvStructuredStreamReadRequestV1 = 7163;
constexpr std::uint32_t kSchemaCoordinateKvStructuredStreamReadResultV1 = 7164;
constexpr std::uint32_t kSchemaCoordinateKvStructuredStreamAppendRequestV1 = 7165;
constexpr std::uint32_t kSchemaCoordinateKvStructuredStreamAppendResultV1 = 7166;
constexpr std::uint32_t kSchemaCoordinateKvStructuredTimeseriesRequestV1 = 7167;
constexpr std::uint32_t kSchemaCoordinateKvStructuredTimeseriesResultV1 = 7168;
constexpr std::uint32_t kSchemaCoordinateSystemConfigSetRequestV1 = 7169;
constexpr std::uint32_t kSchemaCoordinateSystemConfigSetResultV1 = 7170;
constexpr std::uint32_t kSchemaCoordinateDdlCreateDomainRequestV1 = 7171;
constexpr std::uint32_t kSchemaCoordinateDdlCreateDomainResultV1 = 7172;
constexpr std::uint32_t kSchemaCoordinateDdlCreateSchemaRequestV1 = 7173;
constexpr std::uint32_t kSchemaCoordinateDdlCreateSchemaResultV1 = 7174;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableRequestV1 = 7175;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableResultV1 = 7176;
constexpr std::uint32_t kSchemaCoordinateDdlCreateIndexRequestV1 = 7177;
constexpr std::uint32_t kSchemaCoordinateDdlCreateIndexResultV1 = 7178;
constexpr std::uint32_t kSchemaCoordinateDdlDropIndexRequestV1 = 7179;
constexpr std::uint32_t kSchemaCoordinateDdlDropIndexResultV1 = 7180;
constexpr std::uint32_t kSchemaCoordinateDdlAlterDomainRequestV1 = 7181;
constexpr std::uint32_t kSchemaCoordinateDdlAlterDomainResultV1 = 7182;
constexpr std::uint32_t kSchemaCoordinateDdlCreateViewRequestV1 = 7183;
constexpr std::uint32_t kSchemaCoordinateDdlCreateViewResultV1 = 7184;
constexpr std::uint32_t kSchemaCoordinateDdlCreateMaterializedViewRequestV1 = 7303;
constexpr std::uint32_t kSchemaCoordinateDdlCreateMaterializedViewResultV1 = 7304;
constexpr std::uint32_t kSchemaCoordinateDdlAlterViewRequestV1 = 7185;
constexpr std::uint32_t kSchemaCoordinateDdlAlterViewResultV1 = 7186;
constexpr std::uint32_t kSchemaCoordinateDdlDropViewRequestV1 = 7187;
constexpr std::uint32_t kSchemaCoordinateDdlDropViewResultV1 = 7188;
constexpr std::uint32_t kSchemaCoordinateDdlRefreshMaterializedViewRequestV1 = 7289;
constexpr std::uint32_t kSchemaCoordinateDdlRefreshMaterializedViewResultV1 = 7290;
constexpr std::uint32_t kSchemaCoordinateDdlDropMaterializedViewRequestV1 = 7297;
constexpr std::uint32_t kSchemaCoordinateDdlDropMaterializedViewResultV1 = 7298;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTypeRequestV1 = 7291;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTypeResultV1 = 7292;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableAsQueryWithDataRequestV1 = 7331;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableAsQueryWithDataResultV1 = 7332;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableAsQueryWithNoDataRequestV1 = 7333;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableAsQueryWithNoDataResultV1 = 7334;
constexpr std::uint32_t kSchemaCoordinateDdlDropTableRequestV1 = 7335;
constexpr std::uint32_t kSchemaCoordinateDdlDropTableResultV1 = 7336;
constexpr std::uint32_t kSchemaCoordinateDdlAlterTypeRequestV1 = 7293;
constexpr std::uint32_t kSchemaCoordinateDdlAlterTypeResultV1 = 7294;
constexpr std::uint32_t kSchemaCoordinateDdlDropTypeRequestV1 = 7295;
constexpr std::uint32_t kSchemaCoordinateDdlDropTypeResultV1 = 7296;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTriggerRequestV1 = 7189;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTriggerResultV1 = 7190;
constexpr std::uint32_t kSchemaCoordinateDdlAlterTriggerRequestV1 = 7191;
constexpr std::uint32_t kSchemaCoordinateDdlAlterTriggerResultV1 = 7192;
constexpr std::uint32_t kSchemaCoordinateDdlDropTriggerRequestV1 = 7193;
constexpr std::uint32_t kSchemaCoordinateDdlDropTriggerResultV1 = 7194;
constexpr std::uint32_t kSchemaCoordinateDdlCreateProcedureRequestV1 = 7195;
constexpr std::uint32_t kSchemaCoordinateDdlCreateProcedureResultV1 = 7196;
constexpr std::uint32_t kSchemaCoordinateDdlAlterProcedureRequestV1 = 7197;
constexpr std::uint32_t kSchemaCoordinateDdlAlterProcedureResultV1 = 7198;
constexpr std::uint32_t kSchemaCoordinateDdlDropProcedureRequestV1 = 7199;
constexpr std::uint32_t kSchemaCoordinateDdlDropProcedureResultV1 = 7200;
constexpr std::uint32_t kSchemaCoordinateDdlCreateFunctionRequestV1 = 7201;
constexpr std::uint32_t kSchemaCoordinateDdlCreateFunctionResultV1 = 7202;
constexpr std::uint32_t kSchemaCoordinateDdlAlterFunctionRequestV1 = 7203;
constexpr std::uint32_t kSchemaCoordinateDdlAlterFunctionResultV1 = 7204;
constexpr std::uint32_t kSchemaCoordinateDdlDropFunctionRequestV1 = 7205;
constexpr std::uint32_t kSchemaCoordinateDdlDropFunctionResultV1 = 7206;
constexpr std::uint32_t kSchemaCoordinateDdlCreatePackageRequestV1 = 7207;
constexpr std::uint32_t kSchemaCoordinateDdlCreatePackageResultV1 = 7208;
constexpr std::uint32_t kSchemaCoordinateDdlCreateSynonymRequestV1 = 7187;
constexpr std::uint32_t kSchemaCoordinateDdlCreateSynonymResultV1 = 7188;
constexpr std::uint32_t kSchemaCoordinateDdlCreateForeignTableRequestV1 = 7315;
constexpr std::uint32_t kSchemaCoordinateDdlCreateForeignTableResultV1 = 7316;
constexpr std::uint32_t kSchemaCoordinateDdlCreateFdwRequestV1 = 7319;
constexpr std::uint32_t kSchemaCoordinateDdlCreateFdwResultV1 = 7320;
constexpr std::uint32_t kSchemaCoordinateDdlDropFdwRequestV1 = 7321;
constexpr std::uint32_t kSchemaCoordinateDdlDropFdwResultV1 = 7322;
constexpr std::uint32_t kSchemaCoordinateDdlDropForeignTableRequestV1 = 7317;
constexpr std::uint32_t kSchemaCoordinateDdlDropForeignTableResultV1 = 7318;
constexpr std::uint32_t kSchemaCoordinateDdlDropSynonymRequestV1 = 7313;
constexpr std::uint32_t kSchemaCoordinateDdlDropSynonymResultV1 = 7314;
constexpr std::uint32_t kSchemaCoordinateDdlDropPackageRequestV1 = 7299;
constexpr std::uint32_t kSchemaCoordinateDdlDropPackageResultV1 = 7300;
constexpr std::uint32_t kSchemaCoordinateDdlAlterPackageRequestV1 = 7301;
constexpr std::uint32_t kSchemaCoordinateDdlAlterPackageResultV1 = 7302;
constexpr std::uint32_t kSchemaCoordinateDdlAlterSequenceRequestV1 = 7305;
constexpr std::uint32_t kSchemaCoordinateDdlAlterSequenceResultV1 = 7306;
constexpr std::uint32_t kSchemaCoordinateDdlDropSequenceRequestV1 = 7307;
constexpr std::uint32_t kSchemaCoordinateDdlDropSequenceResultV1 = 7308;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTemporaryTableRequestV1 = 7209;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTemporaryTableResultV1 = 7210;
constexpr std::uint32_t kSchemaCoordinateDdlDropTemporaryTableRequestV1 = 7211;
constexpr std::uint32_t kSchemaCoordinateDdlDropTemporaryTableResultV1 = 7212;
constexpr std::uint32_t kSchemaCoordinateDdlCreateOrReplaceSrsRequestV1 = 7215;
constexpr std::uint32_t kSchemaCoordinateDdlDropSrsRequestV1 = 7217;
constexpr std::uint32_t kSchemaCoordinateDdlCreateOrReplaceSrsResultV1 = 7216;
constexpr std::uint32_t kSchemaCoordinateDdlDropSrsResultV1 = 7218;
constexpr std::uint32_t kSchemaCoordinateDdlCreateRewriteRuleRequestV1 = 7219;
constexpr std::uint32_t kSchemaCoordinateDdlCreateRewriteRuleResultV1 = 7220;
constexpr std::uint32_t kSchemaCoordinateDdlAlterRewriteRuleRequestV1 = 7221;
constexpr std::uint32_t kSchemaCoordinateDdlAlterRewriteRuleResultV1 = 7222;
constexpr std::uint32_t kSchemaCoordinateDdlDropRewriteRuleRequestV1 = 7223;
constexpr std::uint32_t kSchemaCoordinateDdlDropRewriteRuleResultV1 = 7224;
constexpr std::uint32_t kSchemaCoordinateDdlValidateConstraintRequestV1 = 7225;
constexpr std::uint32_t kSchemaCoordinateDdlValidateConstraintResultV1 = 7226;
constexpr std::uint32_t kSchemaCoordinateSecurityCreatePrivilegeTemplateRequestV1 = 7227;
constexpr std::uint32_t kSchemaCoordinateSecurityCreatePrivilegeTemplateResultV1 = 7228;
constexpr std::uint32_t kSchemaCoordinateSecurityCreateUserRequestV1 = 7323;
constexpr std::uint32_t kSchemaCoordinateSecurityCreateUserResultV1 = 7324;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterUserRequestV1 = 7353;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterUserResultV1 = 7354;
constexpr std::uint32_t kSchemaCoordinateSecurityCreateRoleRequestV1 = 7355;
constexpr std::uint32_t kSchemaCoordinateSecurityCreateRoleResultV1 = 7356;
constexpr std::uint32_t kSchemaCoordinateSecurityCreatePolicyRequestV1 = 7359;
constexpr std::uint32_t kSchemaCoordinateSecurityCreatePolicyResultV1 = 7360;
constexpr std::uint32_t kSchemaCoordinateSecurityDropPolicyRequestV1 = 7361;
constexpr std::uint32_t kSchemaCoordinateSecurityDropPolicyResultV1 = 7362;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterRoleRequestV1 = 7363;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterRoleResultV1 = 7364;
constexpr std::uint32_t kSchemaCoordinateSecurityDropRoleRequestV1 = 7357;
constexpr std::uint32_t kSchemaCoordinateSecurityDropRoleResultV1 = 7358;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterPrivilegeTemplateRequestV1 = 7229;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterPrivilegeTemplateResultV1 = 7230;
constexpr std::uint32_t kSchemaCoordinateSecurityDropPrivilegeTemplateRequestV1 = 7231;
constexpr std::uint32_t kSchemaCoordinateSecurityDropPrivilegeTemplateResultV1 = 7232;
constexpr std::uint32_t kSchemaCoordinateDatabaseCreateTemplateCloneRequestV1 = 7233;
constexpr std::uint32_t kSchemaCoordinateDatabaseCreateTemplateCloneResultV1 = 7234;
constexpr std::uint32_t kSchemaCoordinateDdlCreateAggregateRequestV1 = 7235;
constexpr std::uint32_t kSchemaCoordinateDdlCreateAggregateResultV1 = 7236;
constexpr std::uint32_t kSchemaCoordinateDdlAlterAggregateRequestV1 = 7237;
constexpr std::uint32_t kSchemaCoordinateDdlAlterAggregateResultV1 = 7238;
constexpr std::uint32_t kSchemaCoordinateDdlDropAggregateRequestV1 = 7239;
constexpr std::uint32_t kSchemaCoordinateDdlDropAggregateResultV1 = 7240;
constexpr std::uint32_t kSchemaCoordinateDdlPurgeSystemHistoryRequestV1 = 7241;
constexpr std::uint32_t kSchemaCoordinateDdlPurgeSystemHistoryResultV1 = 7242;
constexpr std::uint32_t kSchemaCoordinateDdlSetIndexOptimizerEligibilityRequestV1 = 7243;
constexpr std::uint32_t kSchemaCoordinateDdlSetIndexOptimizerEligibilityResultV1 = 7244;
constexpr std::uint32_t kSchemaCoordinateDdlSetTableTypeEnforcementRequestV1 = 7245;
constexpr std::uint32_t kSchemaCoordinateDdlSetTableTypeEnforcementResultV1 = 7246;
constexpr std::uint32_t kSchemaCoordinateDatabaseSerializeLogicalSnapshotRequestV1 = 7247;
constexpr std::uint32_t kSchemaCoordinateDatabaseSerializeLogicalSnapshotResultV1 = 7248;
constexpr std::uint32_t kSchemaCoordinateDatabaseDeserializeLogicalSnapshotRequestV1 = 7249;
constexpr std::uint32_t kSchemaCoordinateDatabaseDeserializeLogicalSnapshotResultV1 = 7250;
constexpr std::uint32_t kSchemaCoordinateDdlCreateMacroRequestV1 = 7251;
constexpr std::uint32_t kSchemaCoordinateDdlCreateMacroResultV1 = 7252;
constexpr std::uint32_t kSchemaCoordinateDdlCreateDictionaryRequestV1 = 7259;
constexpr std::uint32_t kSchemaCoordinateDdlCreateDictionaryResultV1 = 7260;
constexpr std::uint32_t kSchemaCoordinateDdlDropDictionaryRequestV1 = 7261;
constexpr std::uint32_t kSchemaCoordinateDdlDropDictionaryResultV1 = 7262;
constexpr std::uint32_t kSchemaCoordinateDdlAlterDictionaryRequestV1 = 7265;
constexpr std::uint32_t kSchemaCoordinateDdlAlterDictionaryResultV1 = 7266;
constexpr std::uint32_t kSchemaCoordinateDdlCreateContinuousViewRequestV1 = 7267;
constexpr std::uint32_t kSchemaCoordinateDdlCreateContinuousViewResultV1 = 7268;
constexpr std::uint32_t kSchemaCoordinateDdlAlterContinuousViewRequestV1 = 7269;
constexpr std::uint32_t kSchemaCoordinateDdlAlterContinuousViewResultV1 = 7270;
constexpr std::uint32_t kSchemaCoordinateDdlDropContinuousViewRequestV1 = 7271;
constexpr std::uint32_t kSchemaCoordinateDdlDropContinuousViewResultV1 = 7272;
constexpr std::uint32_t kSchemaCoordinateDmlAsyncInsertSubmitRequestV1 = 7273;
constexpr std::uint32_t kSchemaCoordinateDmlAsyncInsertSubmitResultV1 = 7274;
constexpr std::uint32_t kSchemaCoordinateDmlAsyncInsertStatusRequestV1 = 7275;
constexpr std::uint32_t kSchemaCoordinateDmlAsyncInsertStatusResultV1 = 7276;
constexpr std::uint32_t kSchemaCoordinateDmlAsyncInsertCancelRequestV1 = 7277;
constexpr std::uint32_t kSchemaCoordinateDmlAsyncInsertCancelResultV1 = 7278;
constexpr std::uint32_t kSchemaCoordinateDmlCounterAddRequestV1 = 7281;
constexpr std::uint32_t kSchemaCoordinateDmlCounterAddResultV1 = 7282;
constexpr std::uint32_t kSchemaCoordinateDmlTimeseriesSchemaWriteRequestV1 = 7283;
constexpr std::uint32_t kSchemaCoordinateDmlTimeseriesSchemaWriteResultV1 = 7284;
constexpr std::uint32_t kSchemaCoordinateDdlTimeseriesSeriesCardinalityPolicyRequestV1 = 7285;
constexpr std::uint32_t kSchemaCoordinateDdlTimeseriesSeriesCardinalityPolicyResultV1 = 7286;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTimeseriesValueCacheRequestV1 = 7287;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTimeseriesValueCacheResultV1 = 7288;
constexpr std::uint32_t kSchemaCoordinateDdlDropMacroRequestV1 = 7253;
constexpr std::uint32_t kSchemaCoordinateDdlDropMacroResultV1 = 7254;
constexpr std::uint32_t kSchemaCoordinateAdminRegisterExternalRelationResolverRequestV1 = 7255;
constexpr std::uint32_t kSchemaCoordinateAdminRegisterExternalRelationResolverResultV1 = 7256;
constexpr std::uint32_t kSchemaCoordinateAdminUnregisterExternalRelationResolverRequestV1 = 7257;
constexpr std::uint32_t kSchemaCoordinateAdminUnregisterExternalRelationResolverResultV1 = 7258;
constexpr std::uint32_t kSchemaCoordinateDdlRenameObjectVectorRequestV1 = 7213;
constexpr std::uint32_t kSchemaCoordinateDdlRenameObjectVectorResultV1 = 7214;
constexpr std::uint32_t kSchemaCoordinateDdlRenameObjectRequestV1 = 7225;
constexpr std::uint32_t kSchemaCoordinateDdlRenameObjectResultV1 = 7226;
constexpr std::uint32_t kSchemaCoordinateAggregateRequestV1 = 7145;
constexpr std::uint32_t kSchemaCoordinateAggregateResultV1 = 7146;
constexpr std::uint32_t kSchemaCoordinateGroupRequestV1 = 7147;
constexpr std::uint32_t kSchemaCoordinateGroupResultV1 = 7148;
constexpr std::uint32_t kSchemaCoordinateSecurityCreateGroupMappingRequestV1 = 7365;
constexpr std::uint32_t kSchemaCoordinateSecurityCreateGroupMappingResultV1 = 7366;
constexpr std::uint32_t kSchemaCoordinateSecurityDropGroupMappingRequestV1 = 7367;
constexpr std::uint32_t kSchemaCoordinateSecurityDropGroupMappingResultV1 = 7368;
constexpr std::uint32_t kSchemaCoordinateSecurityGrantRequestV1 = 7369;
constexpr std::uint32_t kSchemaCoordinateSecurityGrantResultV1 = 7370;
constexpr std::uint32_t kSchemaCoordinateSecurityRevokeRequestV1 = 7371;
constexpr std::uint32_t kSchemaCoordinateSecurityRevokeResultV1 = 7372;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterPolicyRequestV1 = 7373;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterPolicyResultV1 = 7374;
constexpr std::uint32_t kSchemaCoordinateSecurityDropUserRequestV1 = 7375;
constexpr std::uint32_t kSchemaCoordinateSecurityDropUserResultV1 = 7376;
constexpr std::uint32_t kSchemaCoordinateSecurityAuthenticateRequestV1 = 7377;
constexpr std::uint32_t kSchemaCoordinateSecurityAuthenticateResultV1 = 7378;
constexpr std::uint32_t kSchemaCoordinateSecurityDeauthenticateRequestV1 = 7379;
constexpr std::uint32_t kSchemaCoordinateSecurityDeauthenticateResultV1 = 7380;
constexpr std::uint32_t kSchemaSessionRoleSwitchRequestV1 = 7381;
constexpr std::uint32_t kSchemaSessionRoleSwitchResultV1 = 7382;
constexpr std::uint32_t kSchemaCoordinateSortRequestV1 = 7149;
constexpr std::uint32_t kSchemaCoordinateSortResultV1 = 7150;
constexpr std::uint32_t kSchemaCoordinateLimitRequestV1 = 7151;
constexpr std::uint32_t kSchemaCoordinateLimitResultV1 = 7152;
constexpr std::uint32_t kSchemaCoordinateWindowRequestV1 = 7153;
constexpr std::uint32_t kSchemaCoordinateWindowResultV1 = 7154;
constexpr std::uint32_t kSchemaCoordinateReturnResultSetRequestV1 = 7155;
constexpr std::uint32_t kSchemaCoordinateReturnResultSetResultV1 = 7156;

constexpr std::uint8_t kCapabilityBaseline = 0x01u;
constexpr std::uint8_t kCapabilityTransactionRoutingV2 = 0x02u;
constexpr std::uint8_t kCapabilityPreparedMetadataTransferV1 = 0x04u;
constexpr std::uint8_t kCapabilityRelationDescriptorProjectionV3 = 0x08u;
constexpr std::uint8_t kKnownCapabilityByte0 =
    kCapabilityBaseline | kCapabilityTransactionRoutingV2 |
    kCapabilityPreparedMetadataTransferV1 |
    kCapabilityRelationDescriptorProjectionV3;
constexpr std::uint32_t kSchemaRenderUuidRequestV1 = 7003;
constexpr std::uint32_t kSchemaRenderUuidResultV1 = 7004;
constexpr std::uint32_t kSchemaEventSubscribeRequestV1 = 5001;
constexpr std::uint32_t kSchemaEventSubscribeResultV1 = 5002;
constexpr std::uint32_t kSchemaEventUnsubscribeRequestV1 = 5003;
constexpr std::uint32_t kSchemaEventUnsubscribeResultV1 = 5004;
constexpr std::uint32_t kSchemaEventNotificationV1 = 5005;
constexpr std::uint32_t kSchemaEventAckV1 = 5006;
constexpr std::uint32_t kSchemaEventBackpressureV1 = 5007;

enum class MessageType : std::uint16_t {
  kReserved = 0,
  kHello = 1,
  kHelloAccept = 2,
  kHelloReject = 3,
  kAuthHandoff = 10,
  kAuthResult = 11,
  kAuthChallenge = 12,
  kAttachDatabase = 20,
  kAttachResult = 21,
  kManagementRequest = 30,
  kManagementResult = 31,
  kResolveNameRequest = 32,
  kResolveNameResult = 33,
  kRenderUuidRequest = 34,
  kRenderUuidResult = 35,
  kAcquireStatementContextRequest = 36,
  kAcquireStatementContextResult = 37,
  kNegotiateLiteralDescriptorsRequest = 38,
  kNegotiateLiteralDescriptorsResult = 39,
  kPrepareSblr = 40,
  kPrepareResult = 41,
  kExecuteSblr = 42,
  kExecuteResult = 43,
  kFetch = 44,
  kFetchResult = 45,
  kCloseCursor = 46,
  kCloseCursorResult = 47,
  kClosePreparedSblr = 48,
  kClosePreparedSblrResult = 49,
  kBeginParameterExecutionCoordinationRequest = 50,
  kBeginParameterExecutionCoordinationResult = 51,
  kNegotiateVariableDescriptorsRequest = 52,
  kNegotiateVariableDescriptorsResult = 53,
  kFinalizeVariableBindingRequest = 54,
  kFinalizeVariableBindingResult = 55,
  kBeginVariableFrameRequest = 56,
  kBeginVariableFrameResult = 57,
  kCloseVariableFrameRequest = 58,
  kCloseVariableFrameResult = 59,
  kAssignVariableValuesRequest = 60,
  kAssignVariableValuesResult = 61,
  kIssueSourceMapRequest = 62,
  kIssueSourceMapResult = 63,
  kIssueErrorVectorRequest = 64,
  kIssueErrorVectorResult = 65,
  kReserveSavepointRequest = 66,
  kReserveSavepointResult = 67,
  kReserveAutonomousFrameRequest = 68,
  kReserveAutonomousFrameResult = 69,
  kCoordinateReservationReleaseRequest = 72,
  kCoordinateReservationReleaseResult = 73,
  kCoordinateTemporaryInstanceCleanupRequest = 76,
  kCoordinateTemporaryInstanceCleanupResult = 77,
  kCoordinateCursorOpenRequest = 78,
  kCoordinateCursorOpenResult = 79,
  kCoordinateReadByKeyRequest = 80,
  kCoordinateReadByKeyResult = 81,
  kCoordinateReadRangeRequest = 82,
  kCoordinateReadRangeResult = 83,
  kCoordinateReadStreamRequest = 84,
  kCoordinateReadStreamResult = 85,
  kCoordinateResultSetPassRequest = 86,
  kCoordinateResultSetPassResult = 87,
  kCoordinateAccessCursorOpenRequest = 88,
  kCoordinateAccessCursorOpenResult = 89,
  kCoordinateAccessCursorFetchRequest = 90,
  kCoordinateAccessCursorFetchResult = 91,
  kCoordinateAccessCursorCloseRequest = 92,
  kCoordinateAccessCursorCloseResult = 93,
  kCoordinateInsertRequest = 94,
  kCoordinateInsertResult = 95,
  kCoordinateUpdateRequest = 96,
  kCoordinateUpdateResult = 97,
  kCoordinateDeleteRequest = 98,
  kCoordinateDeleteResult = 99,
  kCoordinateMergeRequest = 100,
  kCoordinateMergeResult = 101,
  kCoordinateTableTruncateRequest = 102,
  kCoordinateTableTruncateResult = 103,
  kCoordinateTableAnalyzeRequest = 104,
  kCoordinateTableAnalyzeResult = 105,
  kCoordinateBulkImportStreamRequest = 106,
  kCoordinateBulkImportStreamResult = 107,
  kCoordinateBulkExportStreamRequest = 108,
  kCoordinateBulkExportStreamResult = 109,
  kCoordinateStatementBatchRequest = 110,
  kCoordinateStatementBatchResult = 111,
  kCoordinateAtomicCasRequest = 112,
  kCoordinateAtomicCasResult = 113,
  kCoordinateAtomicRmwRequest = 114,
  kCoordinateAtomicRmwResult = 115,
  kCoordinateAdvisoryLockRequest = 116,
  kCoordinateAdvisoryLockResult = 117,
  kCoordinateAdvisoryLockReleaseRequest = 118,
  kCoordinateAdvisoryLockReleaseResult = 119,
  kCoordinateFunctionCallRequest = 120,
  kCoordinateFunctionCallResult = 121,
  kCoordinateOperatorCallRequest = 122,
  kCoordinateOperatorCallResult = 123,
  kCoordinateCastRequest = 124,
  kCoordinateCastResult = 125,
  kCoordinateCompareRequest = 126,
  kCoordinateCompareResult = 127,
  kCoordinateDomainOperationRequest = 128,
  kCoordinateDomainOperationResult = 129,
  kCoordinateUdrInvokeRequest = 130,
  kCoordinateUdrInvokeResult = 131,
  kCoordinateProcedureInvokeRequest = 132,
  kCoordinateProcedureInvokeResult = 133,
  kCoordinateFunctionInvokeRequest = 134,
  kCoordinateFunctionInvokeResult = 135,
  kCoordinateAggregateInvokeRequest = 136,
  kCoordinateAggregateInvokeResult = 137,
  kCoordinateSequenceNextvalRequest = 138,
  kCoordinateSequenceNextvalResult = 139,
  kCoordinateSequenceCurrvalRequest = 140,
  kCoordinateSequenceCurrvalResult = 141,
  kCoordinateSequenceSetvalRequest = 142,
  kCoordinateSequenceSetvalResult = 143,
  kCoordinateQueryNumericRequest = 144,
  kCoordinateQueryNumericResult = 145,
  kCoordinateAdvancedDatatypeFamilyRequest = 146,
  kCoordinateAdvancedDatatypeFamilyResult = 147,
  kCoordinateProjectRequest = 148,
  kCoordinateProjectResult = 149,
  kCoordinateCatalogIntrospectRequest = 268,
  kCoordinateCatalogIntrospectResult = 269,
  kCoordinateKvStructuredReadRequest = 162,
  kCoordinateKvStructuredReadResult = 163,
  kCoordinateKvStructuredMutateRequest = 164,
  kCoordinateKvStructuredMutateResult = 165,
  kCoordinateKvStructuredScanRequest = 166,
  kCoordinateKvStructuredScanResult = 167,
  kCoordinateKvStructuredStreamReadRequest = 168,
  kCoordinateKvStructuredStreamReadResult = 169,
  kCoordinateKvStructuredStreamAppendRequest = 170,
  kCoordinateKvStructuredStreamAppendResult = 171,
  kCoordinateKvStructuredTimeseriesRequest = 172,
  kCoordinateKvStructuredTimeseriesResult = 173,
  kCoordinateSystemConfigSetRequest = 174,
  kCoordinateSystemConfigSetResult = 175,
  kCoordinateDdlCreateDomainRequest = 176,
  kCoordinateDdlCreateDomainResult = 177,
  kCoordinateDdlCreateSchemaRequest = 178,
  kCoordinateDdlCreateSchemaResult = 179,
  kCoordinateDdlCreateTableRequest = 180,
  kCoordinateDdlCreateTableResult = 181,
  kCoordinateDdlCreateIndexRequest = 182,
  kCoordinateDdlCreateIndexResult = 183,
  kCoordinateDdlDropIndexRequest = 184,
  kCoordinateDdlDropIndexResult = 185,
  kCoordinateDdlAlterDomainRequest = 186,
  kCoordinateDdlAlterDomainResult = 187,
  kCoordinateDdlCreateViewRequest = 188,
  kCoordinateDdlCreateViewResult = 189,
  kCoordinateDdlCreateMaterializedViewRequest = 312,
  kCoordinateDdlCreateMaterializedViewResult = 313,
  kCoordinateDdlAlterViewRequest = 190,
  kCoordinateDdlAlterViewResult = 191,
  kCoordinateDdlDropViewRequest = 192,
  kCoordinateDdlDropViewResult = 193,
  kCoordinateDdlRefreshMaterializedViewRequest = 268,
  kCoordinateDdlRefreshMaterializedViewResult = 269,
  kCoordinateDdlDropMaterializedViewRequest = 306,
  kCoordinateDdlDropMaterializedViewResult = 307,
  kCoordinateDdlCreateTypeRequest = 300,
  kCoordinateDdlCreateTypeResult = 301,
  kCoordinateDdlAlterTypeRequest = 302,
  kCoordinateDdlAlterTypeResult = 303,
  kCoordinateDdlDropTypeRequest = 304,
  kCoordinateDdlDropTypeResult = 305,
  kCoordinateDdlCreateTableAsQueryWithDataRequest = 330,
  kCoordinateDdlCreateTableAsQueryWithDataResult = 331,
  kCoordinateDdlCreateTableAsQueryWithNoDataRequest = 332,
  kCoordinateDdlCreateTableAsQueryWithNoDataResult = 333,
  kCoordinateDdlDropTableRequest = 334,
  kCoordinateDdlDropTableResult = 335,
  kCoordinateDdlCreateTriggerRequest = 194,
  kCoordinateDdlCreateTriggerResult = 195,
  kCoordinateDdlAlterTriggerRequest = 196,
  kCoordinateDdlAlterTriggerResult = 197,
  kCoordinateDdlDropTriggerRequest = 198,
  kCoordinateDdlDropTriggerResult = 199,
  kCoordinateDdlCreateProcedureRequest = 200,
  kCoordinateDdlCreateProcedureResult = 201,
  kCoordinateDdlAlterProcedureRequest = 202,
  kCoordinateDdlAlterProcedureResult = 203,
  kCoordinateDdlDropProcedureRequest = 204,
  kCoordinateDdlDropProcedureResult = 205,
  kCoordinateDdlCreateFunctionRequest = 206,
  kCoordinateDdlCreateFunctionResult = 207,
  kCoordinateDdlAlterFunctionRequest = 208,
  kCoordinateDdlAlterFunctionResult = 209,
  kCoordinateDdlDropFunctionRequest = 210,
  kCoordinateDdlDropFunctionResult = 211,
  kCoordinateDdlCreatePackageRequest = 212,
  kCoordinateDdlCreatePackageResult = 213,
  kCoordinateDdlCreateSynonymRequest = 214,
  kCoordinateDdlCreateSynonymResult = 215,
  kCoordinateDdlCreateForeignTableRequest = 216,
  kCoordinateDdlCreateForeignTableResult = 217,
  kCoordinateDdlCreateFdwRequest = 326,
  kCoordinateDdlCreateFdwResult = 327,
  kCoordinateDdlDropFdwRequest = 328,
  kCoordinateDdlDropFdwResult = 329,
  kCoordinateDdlDropForeignTableRequest = 324,
  kCoordinateDdlDropForeignTableResult = 325,
  kCoordinateDdlDropSynonymRequest = 320,
  kCoordinateDdlDropSynonymResult = 321,
  kCoordinateDdlDropPackageRequest = 308,
  kCoordinateDdlDropPackageResult = 309,
  kCoordinateDdlAlterPackageRequest = 310,
  kCoordinateDdlAlterPackageResult = 311,
  kCoordinateDdlAlterSequenceRequest = 314,
  kCoordinateDdlAlterSequenceResult = 315,
  kCoordinateDdlDropSequenceRequest = 316,
  kCoordinateDdlDropSequenceResult = 317,
  kCoordinateDdlCreateTemporaryTableRequest = 214,
  kCoordinateDdlCreateTemporaryTableResult = 215,
  kCoordinateDdlDropTemporaryTableRequest = 216,
  kCoordinateDdlDropTemporaryTableResult = 217,
  kCoordinateDdlCreateOrReplaceSrsRequest = 220,
  kCoordinateDdlDropSrsRequest = 222,
  kCoordinateDdlCreateOrReplaceSrsResult = 221,
  kCoordinateDdlDropSrsResult = 223,
  kCoordinateDdlCreateRewriteRuleRequest = 224,
  kCoordinateDdlCreateRewriteRuleResult = 225,
  kCoordinateDdlAlterRewriteRuleRequest = 226,
  kCoordinateDdlAlterRewriteRuleResult = 227,
  kCoordinateDdlDropRewriteRuleRequest = 228,
  kCoordinateDdlDropRewriteRuleResult = 229,
  kCoordinateDdlValidateConstraintRequest = 230,
  kCoordinateDdlValidateConstraintResult = 231,
  kCoordinateSecurityCreatePrivilegeTemplateRequest = 232,
  kCoordinateSecurityCreatePrivilegeTemplateResult = 233,
  kCoordinateSecurityCreateUserRequest = 330,
  kCoordinateSecurityCreateUserResult = 331,
  kCoordinateSecurityAlterUserRequest = 340,
  kCoordinateSecurityAlterUserResult = 341,
  kCoordinateSecurityCreateRoleRequest = 342,
  kCoordinateSecurityCreateRoleResult = 343,
  kCoordinateSecurityCreatePolicyRequest = 346,
  kCoordinateSecurityCreatePolicyResult = 347,
  kCoordinateSecurityDropPolicyRequest = 348,
  kCoordinateSecurityDropPolicyResult = 349,
  kCoordinateSecurityAlterRoleRequest = 350,
  kCoordinateSecurityAlterRoleResult = 351,
  kCoordinateSecurityDropRoleRequest = 344,
  kCoordinateSecurityDropRoleResult = 345,
  kCoordinateSecurityAlterPrivilegeTemplateRequest = 234,
  kCoordinateSecurityAlterPrivilegeTemplateResult = 235,
  kCoordinateSecurityDropPrivilegeTemplateRequest = 236,
  kCoordinateSecurityDropPrivilegeTemplateResult = 237,
  kCoordinateDatabaseCreateTemplateCloneRequest = 238,
  kCoordinateDatabaseCreateTemplateCloneResult = 239,
  kCoordinateDdlCreateAggregateRequest = 240,
  kCoordinateDdlCreateAggregateResult = 241,
  kCoordinateDdlAlterAggregateRequest = 242,
  kCoordinateDdlAlterAggregateResult = 243,
  kCoordinateDdlDropAggregateRequest = 244,
  kCoordinateDdlDropAggregateResult = 245,
  kCoordinateDdlPurgeSystemHistoryRequest = 246,
  kCoordinateDdlPurgeSystemHistoryResult = 247,
  kCoordinateDdlSetIndexOptimizerEligibilityRequest = 248,
  kCoordinateDdlSetIndexOptimizerEligibilityResult = 249,
  kCoordinateDdlSetTableTypeEnforcementRequest = 250,
  kCoordinateDdlSetTableTypeEnforcementResult = 251,
  kCoordinateDatabaseSerializeLogicalSnapshotRequest = 252,
  kCoordinateDatabaseSerializeLogicalSnapshotResult = 253,
  kCoordinateDatabaseDeserializeLogicalSnapshotRequest = 254,
  kCoordinateDatabaseDeserializeLogicalSnapshotResult = 255,
  kCoordinateDdlCreateMacroRequest = 256,
  kCoordinateDdlCreateMacroResult = 257,
  kCoordinateDdlCreateDictionaryRequest = 264,
  kCoordinateDdlCreateDictionaryResult = 265,
  kCoordinateDdlDropDictionaryRequest = 266,
  kCoordinateDdlDropDictionaryResult = 267,
  kCoordinateDdlAlterDictionaryRequest = 270,
  kCoordinateDdlAlterDictionaryResult = 271,
  kCoordinateDdlCreateContinuousViewRequest = 272,
  kCoordinateDdlCreateContinuousViewResult = 273,
  kCoordinateDdlAlterContinuousViewRequest = 274,
  kCoordinateDdlAlterContinuousViewResult = 275,
  kCoordinateDdlDropContinuousViewRequest = 276,
  kCoordinateDdlDropContinuousViewResult = 277,
  kCoordinateDmlAsyncInsertSubmitRequest = 278,
  kCoordinateDmlAsyncInsertSubmitResult = 279,
  kCoordinateDmlAsyncInsertStatusRequest = 280,
  kCoordinateDmlAsyncInsertStatusResult = 281,
  kCoordinateDmlAsyncInsertCancelRequest = 282,
  kCoordinateDmlAsyncInsertCancelResult = 283,
  kCoordinateDmlCounterAddRequest = 286,
  kCoordinateDmlCounterAddResult = 287,
  kCoordinateDmlTimeseriesSchemaWriteRequest = 288,
  kCoordinateDmlTimeseriesSchemaWriteResult = 289,
  kCoordinateDdlTimeseriesSeriesCardinalityPolicyRequest = 290,
  kCoordinateDdlTimeseriesSeriesCardinalityPolicyResult = 291,
  kCoordinateDdlCreateTimeseriesValueCacheRequest = 292,
  kCoordinateDdlCreateTimeseriesValueCacheResult = 293,
  kCoordinateDdlDropMacroRequest = 258,
  kCoordinateDdlDropMacroResult = 259,
  kCoordinateAdminRegisterExternalRelationResolverRequest = 260,
  kCoordinateAdminRegisterExternalRelationResolverResult = 261,
  kCoordinateAdminUnregisterExternalRelationResolverRequest = 262,
  kCoordinateAdminUnregisterExternalRelationResolverResult = 263,
  kCoordinateDdlRenameObjectVectorRequest = 218,
  kCoordinateDdlRenameObjectVectorResult = 219,
  kCoordinateDdlRenameObjectRequest = 230,
  kCoordinateDdlRenameObjectResult = 231,
  kCoordinateAggregateRequest = 150,
  kCoordinateAggregateResult = 151,
  kCoordinateGroupRequest = 152,
  kCoordinateGroupResult = 153,
  kCoordinateSecurityCreateGroupMappingRequest = 352,
  kCoordinateSecurityCreateGroupMappingResult = 353,
  kCoordinateSecurityDropGroupMappingRequest = 354,
  kCoordinateSecurityDropGroupMappingResult = 355,
  kCoordinateSecurityGrantRequest = 356,
  kCoordinateSecurityGrantResult = 357,
  kCoordinateSecurityRevokeRequest = 358,
  kCoordinateSecurityRevokeResult = 359,
  kCoordinateSecurityAlterPolicyRequest = 360,
  kCoordinateSecurityAlterPolicyResult = 361,
  kCoordinateSecurityDropUserRequest = 362,
  kCoordinateSecurityDropUserResult = 363,
  kCoordinateSecurityAuthenticateRequest = 364,
  kCoordinateSecurityAuthenticateResult = 365,
  kCoordinateSecurityDeauthenticateRequest = 366,
  kCoordinateSecurityDeauthenticateResult = 367,
  kSessionRoleSwitchRequest = 368,
  kSessionRoleSwitchResult = 369,
  kCoordinateSortRequest = 154,
  kCoordinateSortResult = 155,
  kCoordinateLimitRequest = 156,
  kCoordinateLimitResult = 157,
  kCoordinateWindowRequest = 158,
  kCoordinateWindowResult = 159,
  kCoordinateReturnResultSetRequest = 160,
  kCoordinateReturnResultSetResult = 161,
  kDiagnostic = 60,
  kPing = 70,
  kPong = 71,
  kDisconnectNotice = 74,
  kEventSubscribeRequest = 80,
  kEventSubscribeResult = 81,
  kEventUnsubscribeRequest = 82,
  kEventUnsubscribeResult = 83,
  kEventNotification = 84,
  kEventAck = 85,
  kEventBackpressure = 86,
  kEventSubscriptionInvalidate = 87,
  kEventChannelClosed = 88,
};

struct FrameHeader {
  std::uint16_t protocol_major = kProtocolMajor;
  std::uint16_t protocol_minor = kProtocolMinor;
  std::uint16_t message_type = 0;
  std::uint32_t flags = 0;
  std::uint32_t payload_schema_id = 0;
  std::uint32_t payload_len = 0;
  std::uint32_t header_crc32c = 0;
  std::uint32_t payload_crc32c = 0;
  std::uint64_t stream_id = 0;
  std::uint64_t sequence_number = 1;
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload;
};

struct DecodeResult {
  std::optional<Frame> frame;
  std::vector<ServerDiagnostic> diagnostics;
  bool ok() const { return diagnostics.empty() && frame.has_value(); }
};

struct ChunkAssemblyResult {
  std::optional<Frame> frame;
  std::vector<ServerDiagnostic> diagnostics;
  bool ok() const { return diagnostics.empty() && frame.has_value(); }
};

struct HelloRequest {
  std::array<std::uint8_t, 16> parser_instance_uuid{};
  std::array<std::uint8_t, 16> parser_package_uuid{};
  std::array<std::uint8_t, 16> parser_family_uuid{};
  std::array<std::uint8_t, 16> dialect_profile_uuid{};
  std::uint32_t parser_api_major = 0;
  std::uint32_t parser_api_minor = 0;
  std::string protocol;
  std::string profile_id;
  std::string bundle_contract_id;
  std::array<std::uint8_t, 32> resource_bundle_hash{};
  std::array<std::uint8_t, 16> launch_uuid{};
  std::array<std::uint8_t, 16> listener_uuid{};
  std::uint64_t launch_generation = 0;
  std::array<std::uint8_t, 32> capability_bitmap{};
};

struct HelloAccept {
  std::array<std::uint8_t, 16> server_uuid{};
  std::array<std::uint8_t, 16> channel_uuid{};
  std::uint16_t protocol_minor = kProtocolMinor;
  std::uint32_t max_frame_bytes = 0;
  std::uint32_t max_streams = 0;
  std::array<std::uint8_t, 32> accepted_capability_bitmap{};
  std::uint64_t server_policy_generation = 1;
  std::array<std::uint8_t, 16> registry_snapshot_uuid{};
  std::uint32_t heartbeat_interval_ms = 30000;
};

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size);
std::array<std::uint8_t, 16> MakeUuidV7Bytes();
bool IsZeroUuid(const std::array<std::uint8_t, 16>& uuid);
bool IsKnownMessageType(std::uint16_t message_type);
bool HasUnknownCapabilityBits(const std::array<std::uint8_t, 32>& capability_bitmap);

std::vector<std::uint8_t> EncodeFrame(const FrameHeader& header,
                                      const std::vector<std::uint8_t>& payload);
std::vector<std::vector<std::uint8_t>> EncodeFrameSequence(
    const FrameHeader& header,
    const std::vector<std::uint8_t>& payload,
    std::uint64_t max_payload_bytes);
DecodeResult DecodeFrameBytes(const std::vector<std::uint8_t>& bytes,
                              std::uint32_t max_payload_bytes);
ChunkAssemblyResult AssembleDecodedChunkSequence(const std::vector<Frame>& chunks,
                                                 std::uint64_t max_total_payload_bytes);
std::optional<std::uint32_t> PayloadLengthFromHeader(const std::vector<std::uint8_t>& header_bytes);

std::vector<std::uint8_t> EncodeMessageVectorSet(
    const std::vector<ServerDiagnostic>& diagnostics,
    const std::array<std::uint8_t, 16>& request_uuid);
std::vector<std::string> DecodeMessageVectorDiagnosticCodes(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> EncodeHelloRequestForTest();
std::optional<HelloRequest> DecodeHelloRequest(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> EncodeHelloAccept(const HelloAccept& accept);
bool IsBuiltInTestHello(const HelloRequest& hello);

ServerDiagnostic IpcDiagnostic(std::string code,
                               std::string key,
                               std::string safe_message,
                               std::vector<ServerDiagnosticField> fields = {});

}  // namespace scratchbird::server::sbps
