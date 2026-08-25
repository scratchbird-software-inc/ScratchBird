// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parser_server_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace scratchbird::parser::ipc {
namespace {

constexpr std::uint32_t kFrameMagic = 0x53504253;  // SBPS
constexpr std::uint32_t kMessageVectorMagic = 0x564d4253;  // SBMV
constexpr std::uint16_t kHeaderBytes = 96;
constexpr std::uint16_t kProtocolMajor = 1;
constexpr std::uint16_t kProtocolMinor = 0;
constexpr std::uint8_t kCapabilityBaseline = 0x01u;
constexpr std::uint8_t kCapabilityTransactionRoutingV2 = 0x02u;
constexpr std::uint8_t kCapabilityPreparedMetadataTransferV1 = 0x04u;
constexpr std::uint8_t kCapabilityRelationDescriptorProjectionV3 = 0x08u;
constexpr std::size_t kHelloAcceptCapabilityOffset = 42;
constexpr std::uint32_t kFlagResponse = 1u << 0;
constexpr std::uint32_t kFlagError = 1u << 1;
constexpr std::uint32_t kFlagFinal = 1u << 2;
constexpr std::uint32_t kFlagPayloadChunk = 1u << 3;
constexpr std::uint32_t kSchemaHelloRequestV1 = 1001;
constexpr std::uint32_t kSchemaAuthHandoffV1 = 3001;
constexpr std::uint32_t kSchemaAttachRequestV1 = 3003;
constexpr std::uint32_t kSchemaPrepareSblrV1 = 4001;
constexpr std::uint32_t kSchemaExecuteSblrV1 = 4003;
constexpr std::uint32_t kSchemaFetchV1 = 4005;
constexpr std::uint32_t kSchemaCloseCursorV1 = 4007;
constexpr std::uint32_t kSchemaPrepareSblrV2 = 4009;
constexpr std::uint32_t kSchemaPrepareResultV2 = 4010;
constexpr std::uint32_t kSchemaExecuteSblrV2 = 4011;
constexpr std::uint32_t kSchemaExecuteResultV2 = 4012;
constexpr std::uint32_t kSchemaExecuteCanonicalSblrV1 = 4015;
constexpr std::uint32_t kSchemaExecuteCanonicalSblrLiteralV1 = 4016;
constexpr std::uint32_t kSchemaClosePreparedSblrV1 = 4013;
constexpr std::uint32_t kSchemaClosePreparedSblrResultV1 = 4014;
constexpr std::uint32_t kSchemaManagementRequestV1 = 6001;
constexpr std::uint32_t kSchemaManagementResponseV1 = 6002;
constexpr std::uint32_t kSchemaResolveNameRequestV1 = 7001;
constexpr std::uint32_t kSchemaResolveNameRequestV2 = 7005;
constexpr std::uint32_t kSchemaResolveNameResultV2 = 7006;
constexpr std::uint32_t kSchemaResolveNameRequestV3 = 7007;
constexpr std::uint32_t kSchemaResolveNameResultV3 = 7008;
constexpr std::uint32_t kSchemaRenderUuidRequestV1 = 7003;
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
constexpr std::uint32_t kSchemaNegotiateParameterDescriptorsRequestV1 = 7037;
constexpr std::uint32_t kSchemaNegotiateParameterDescriptorsResultV1 = 7038;
constexpr std::uint32_t kSchemaFinalizeParameterBindingRequestV1 = 7039;
constexpr std::uint32_t kSchemaFinalizeParameterBindingResultV1 = 7040;
constexpr std::uint32_t kSchemaBeginParameterCoordinationRequestV1 = 7041;
constexpr std::uint32_t kSchemaBeginParameterCoordinationResultV1 = 7042;
constexpr std::uint32_t kSchemaAcquireParameterStatementContextRequestV1 = 7043;
constexpr std::uint32_t kSchemaFinalizePreparedParameterRequestV1 = 7044;
constexpr std::uint32_t kSchemaFinalizePreparedParameterResultV1 = 7045;
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
constexpr std::uint32_t kSchemaCoordinateSavepointRequestV1 = 7065;
constexpr std::uint32_t kSchemaCoordinateSavepointResultV1 = 7066;
constexpr std::uint32_t kSchemaCoordinateAutonomousFrameRequestV1 = 7067;
constexpr std::uint32_t kSchemaCoordinateAutonomousFrameResultV1 = 7068;
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
constexpr std::uint32_t kSchemaCoordinateDdlCreateDomainRequestV1 = 7171;
constexpr std::uint32_t kSchemaCoordinateDdlCreateSchemaRequestV1 = 7173;
constexpr std::uint32_t kSchemaCoordinateSystemConfigSetRequestV1 = 7169;
constexpr std::uint32_t kSchemaCoordinateDdlCreateDomainResultV1 = 7172;
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
constexpr std::uint32_t kSchemaCoordinateDdlAlterTypeRequestV1 = 7293;
constexpr std::uint32_t kSchemaCoordinateDdlAlterTypeResultV1 = 7294;
constexpr std::uint32_t kSchemaCoordinateDdlDropTypeRequestV1 = 7295;
constexpr std::uint32_t kSchemaCoordinateDdlDropTypeResultV1 = 7296;
constexpr std::uint32_t kSchemaCoordinateDdlDropTableRequestV1 = 7335;
constexpr std::uint32_t kSchemaCoordinateDdlDropTableResultV1 = 7336;
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
constexpr std::uint32_t kSchemaCoordinateDdlRenameObjectVectorRequestV1 = 7213;
constexpr std::uint32_t kSchemaCoordinateDdlRenameObjectVectorResultV1 = 7214;
constexpr std::uint32_t kSchemaCoordinateDdlRenameObjectRequestV1 = 7225;
constexpr std::uint32_t kSchemaCoordinateDdlRenameObjectResultV1 = 7226;
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
constexpr std::uint32_t kSchemaCoordinateSecurityDropRoleRequestV1 = 7357;
constexpr std::uint32_t kSchemaCoordinateSecurityDropRoleResultV1 = 7358;
constexpr std::uint32_t kSchemaCoordinateSecurityCreatePolicyRequestV1 = 7359;
constexpr std::uint32_t kSchemaCoordinateSecurityCreatePolicyResultV1 = 7360;
constexpr std::uint32_t kSchemaCoordinateSecurityDropPolicyRequestV1 = 7361;
constexpr std::uint32_t kSchemaCoordinateSecurityDropPolicyResultV1 = 7362;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterRoleRequestV1 = 7363;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterRoleResultV1 = 7364;
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
constexpr std::uint32_t kSchemaCoordinateDdlCreateMacroRequestV1 = 7251;
constexpr std::uint32_t kSchemaCoordinateDdlCreateMacroResultV1 = 7252;
constexpr std::uint32_t kSchemaCoordinateDdlDropMacroRequestV1 = 7253;
constexpr std::uint32_t kSchemaCoordinateDdlDropMacroResultV1 = 7254;
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
constexpr std::uint16_t kMessageCoordinateDdlCreateMacroRequest = 256;
constexpr std::uint16_t kMessageCoordinateDdlCreateMacroResult = 257;
constexpr std::uint16_t kMessageCoordinateDdlDropMacroRequest = 258;
constexpr std::uint16_t kMessageCoordinateDdlDropMacroResult = 259;
constexpr std::uint16_t kMessageCoordinateDdlCreateDictionaryRequest = 264;
constexpr std::uint16_t kMessageCoordinateDdlCreateDictionaryResult = 265;
constexpr std::uint16_t kMessageCoordinateDdlDropDictionaryRequest = 266;
constexpr std::uint16_t kMessageCoordinateDdlDropDictionaryResult = 267;
constexpr std::uint16_t kMessageCoordinateDdlAlterDictionaryRequest = 270;
constexpr std::uint16_t kMessageCoordinateDdlAlterDictionaryResult = 271;
constexpr std::uint16_t kMessageCoordinateDdlCreateContinuousViewRequest = 272;
constexpr std::uint16_t kMessageCoordinateDdlCreateContinuousViewResult = 273;
constexpr std::uint16_t kMessageCoordinateDdlAlterContinuousViewRequest = 274;
constexpr std::uint16_t kMessageCoordinateDdlAlterContinuousViewResult = 275;
constexpr std::uint16_t kMessageCoordinateDdlDropContinuousViewRequest = 276;
constexpr std::uint16_t kMessageCoordinateDdlDropContinuousViewResult = 277;
constexpr std::uint16_t kMessageCoordinateDmlAsyncInsertSubmitRequest = 278;
constexpr std::uint16_t kMessageCoordinateDmlAsyncInsertSubmitResult = 279;
constexpr std::uint16_t kMessageCoordinateDmlAsyncInsertStatusRequest = 280;
constexpr std::uint16_t kMessageCoordinateDmlAsyncInsertStatusResult = 281;
constexpr std::uint16_t kMessageCoordinateDmlAsyncInsertCancelRequest = 282;
constexpr std::uint16_t kMessageCoordinateDmlAsyncInsertCancelResult = 283;
constexpr std::uint16_t kMessageCoordinateDmlCounterAddRequest = 286;
constexpr std::uint16_t kMessageCoordinateDmlCounterAddResult = 287;
constexpr std::uint16_t kMessageCoordinateDmlTimeseriesSchemaWriteRequest = 288;
constexpr std::uint16_t kMessageCoordinateDmlTimeseriesSchemaWriteResult = 289;
constexpr std::uint16_t kMessageCoordinateDdlTimeseriesSeriesCardinalityPolicyRequest = 290;
constexpr std::uint16_t kMessageCoordinateDdlTimeseriesSeriesCardinalityPolicyResult = 291;
constexpr std::uint16_t kMessageCoordinateDdlCreateTimeseriesValueCacheRequest = 292;
constexpr std::uint16_t kMessageCoordinateDdlCreateTimeseriesValueCacheResult = 293;
constexpr std::uint32_t kSchemaCoordinateAdminRegisterExternalRelationResolverRequestV1 = 7255;
constexpr std::uint32_t kSchemaCoordinateAdminRegisterExternalRelationResolverResultV1 = 7256;
constexpr std::uint16_t kMessageCoordinateAdminRegisterExternalRelationResolverRequest = 260;
constexpr std::uint16_t kMessageCoordinateAdminRegisterExternalRelationResolverResult = 261;
constexpr std::uint32_t kSchemaCoordinateAdminUnregisterExternalRelationResolverRequestV1 = 7257;
constexpr std::uint32_t kSchemaCoordinateAdminUnregisterExternalRelationResolverResultV1 = 7258;
constexpr std::uint16_t kMessageCoordinateAdminUnregisterExternalRelationResolverRequest = 262;
constexpr std::uint16_t kMessageCoordinateAdminUnregisterExternalRelationResolverResult = 263;
constexpr std::uint32_t kSchemaCoordinateDatabaseDeserializeLogicalSnapshotResultV1 = 7250;
constexpr std::uint32_t kSchemaCoordinateDdlCreateSchemaResultV1 = 7174;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableRequestV1 = 7175;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableResultV1 = 7176;
constexpr std::uint32_t kSchemaCoordinateDdlCreateIndexRequestV1 = 7177;
constexpr std::uint32_t kSchemaCoordinateDdlCreateIndexResultV1 = 7178;
constexpr std::uint32_t kSchemaCoordinateDdlDropIndexRequestV1 = 7179;
constexpr std::uint32_t kSchemaCoordinateDdlDropIndexResultV1 = 7180;
constexpr std::uint32_t kSchemaCoordinateSystemConfigSetResultV1 = 7170;
constexpr std::uint32_t kSchemaCoordinateAggregateRequestV1 = 7145;
constexpr std::uint32_t kSchemaCoordinateAggregateResultV1 = 7146;
constexpr std::uint32_t kSchemaCoordinateGroupRequestV1 = 7147;
constexpr std::uint32_t kSchemaCoordinateGroupResultV1 = 7148;
constexpr std::uint32_t kSchemaCoordinateSortRequestV1 = 7149;
constexpr std::uint32_t kSchemaCoordinateSortResultV1 = 7150;
constexpr std::uint32_t kSchemaCoordinateLimitRequestV1 = 7151;
constexpr std::uint32_t kSchemaCoordinateLimitResultV1 = 7152;
constexpr std::uint32_t kSchemaCoordinateWindowRequestV1 = 7153;
constexpr std::uint32_t kSchemaCoordinateWindowResultV1 = 7154;
constexpr std::uint32_t kSchemaCoordinateReturnResultSetRequestV1 = 7155;
constexpr std::uint32_t kSchemaCoordinateReturnResultSetResultV1 = 7156;
constexpr std::uint16_t kMessageHello = 1;
constexpr std::uint16_t kMessageHelloAccept = 2;
constexpr std::uint16_t kMessageAuthHandoff = 10;
constexpr std::uint16_t kMessageAuthResult = 11;
constexpr std::uint16_t kMessageAttachDatabase = 20;
constexpr std::uint16_t kMessageAttachResult = 21;
constexpr std::uint16_t kMessageManagementRequest = 30;
constexpr std::uint16_t kMessageNegotiateLiteralDescriptorsRequest = 38;
constexpr std::uint16_t kMessageNegotiateLiteralDescriptorsResult = 39;
constexpr std::uint16_t kMessageFinalizeLiteralBindingRequest = 40;
constexpr std::uint16_t kMessageFinalizeLiteralBindingResult = 41;
constexpr std::uint16_t kMessageNegotiateParameterDescriptorsRequest = 42;
constexpr std::uint16_t kMessageNegotiateParameterDescriptorsResult = 43;
constexpr std::uint16_t kMessageFinalizeParameterBindingRequest = 44;
constexpr std::uint16_t kMessageFinalizeParameterBindingResult = 45;
constexpr std::uint16_t kMessageBeginParameterCoordinationRequest = 50;
constexpr std::uint16_t kMessageBeginParameterCoordinationResult = 51;
constexpr std::uint16_t kMessageNegotiateVariableDescriptorsRequest = 52;
constexpr std::uint16_t kMessageNegotiateVariableDescriptorsResult = 53;
constexpr std::uint16_t kMessageFinalizeVariableBindingRequest = 54;
constexpr std::uint16_t kMessageFinalizeVariableBindingResult = 55;
constexpr std::uint16_t kMessageBeginVariableFrameRequest = 56;
constexpr std::uint16_t kMessageBeginVariableFrameResult = 57;
constexpr std::uint16_t kMessageCloseVariableFrameRequest = 58;
constexpr std::uint16_t kMessageCloseVariableFrameResult = 59;
constexpr std::uint16_t kMessageAssignVariableValuesRequest = 60;
constexpr std::uint16_t kMessageAssignVariableValuesResult = 61;
constexpr std::uint16_t kMessageIssueSourceMapRequest = 62;
constexpr std::uint16_t kMessageIssueSourceMapResult = 63;
constexpr std::uint16_t kMessageIssueErrorVectorRequest = 64;
constexpr std::uint16_t kMessageIssueErrorVectorResult = 65;
constexpr std::uint16_t kMessageCoordinateSavepointRequest = 66;
constexpr std::uint16_t kMessageCoordinateSavepointResult = 67;
constexpr std::uint16_t kMessageCoordinateAutonomousFrameRequest = 68;
constexpr std::uint16_t kMessageCoordinateAutonomousFrameResult = 69;
constexpr std::uint16_t kMessageCoordinateReservationReleaseRequest = 72;
constexpr std::uint16_t kMessageCoordinateReservationReleaseResult = 73;
constexpr std::uint16_t kMessageCoordinateTemporaryInstanceCleanupRequest = 76;
constexpr std::uint16_t kMessageCoordinateTemporaryInstanceCleanupResult = 77;
constexpr std::uint16_t kMessageCoordinateCursorOpenRequest = 78;
constexpr std::uint16_t kMessageCoordinateCursorOpenResult = 79;
constexpr std::uint16_t kMessageCoordinateReadByKeyRequest = 80;
constexpr std::uint16_t kMessageCoordinateReadByKeyResult = 81;
constexpr std::uint16_t kMessageCoordinateReadRangeRequest = 82;
constexpr std::uint16_t kMessageCoordinateReadRangeResult = 83;
constexpr std::uint16_t kMessageCoordinateReadStreamRequest = 84;
constexpr std::uint16_t kMessageCoordinateReadStreamResult = 85;
constexpr std::uint16_t kMessageCoordinateResultSetPassRequest = 86;
constexpr std::uint16_t kMessageCoordinateResultSetPassResult = 87;
constexpr std::uint16_t kMessageCoordinateAccessCursorOpenRequest = 88;
constexpr std::uint16_t kMessageCoordinateAccessCursorOpenResult = 89;
constexpr std::uint16_t kMessageCoordinateAccessCursorFetchRequest = 90;
constexpr std::uint16_t kMessageCoordinateAccessCursorFetchResult = 91;
constexpr std::uint16_t kMessageCoordinateAccessCursorCloseRequest = 92;
constexpr std::uint16_t kMessageCoordinateAccessCursorCloseResult = 93;
constexpr std::uint16_t kMessageCoordinateInsertRequest = 94;
constexpr std::uint16_t kMessageCoordinateInsertResult = 95;
constexpr std::uint16_t kMessageCoordinateUpdateRequest = 96;
constexpr std::uint16_t kMessageCoordinateUpdateResult = 97;
constexpr std::uint16_t kMessageCoordinateDeleteRequest = 98;
constexpr std::uint16_t kMessageCoordinateDeleteResult = 99;
constexpr std::uint16_t kMessageCoordinateMergeRequest = 100;
constexpr std::uint16_t kMessageCoordinateMergeResult = 101;
constexpr std::uint16_t kMessageCoordinateTableTruncateRequest = 102;
constexpr std::uint16_t kMessageCoordinateTableTruncateResult = 103;
constexpr std::uint16_t kMessageCoordinateTableAnalyzeRequest = 104;
constexpr std::uint16_t kMessageCoordinateTableAnalyzeResult = 105;
constexpr std::uint16_t kMessageCoordinateBulkImportStreamRequest = 106;
constexpr std::uint16_t kMessageCoordinateBulkImportStreamResult = 107;
constexpr std::uint16_t kMessageCoordinateBulkExportStreamRequest = 108;
constexpr std::uint16_t kMessageCoordinateBulkExportStreamResult = 109;
constexpr std::uint16_t kMessageCoordinateStatementBatchRequest = 110;
constexpr std::uint16_t kMessageCoordinateStatementBatchResult = 111;
constexpr std::uint16_t kMessageCoordinateAtomicCasRequest = 112;
constexpr std::uint16_t kMessageCoordinateAtomicCasResult = 113;
constexpr std::uint16_t kMessageCoordinateAtomicRmwRequest = 114;
constexpr std::uint16_t kMessageCoordinateAtomicRmwResult = 115;
constexpr std::uint16_t kMessageCoordinateAdvisoryLockRequest = 116;
constexpr std::uint16_t kMessageCoordinateAdvisoryLockResult = 117;
constexpr std::uint16_t kMessageCoordinateAdvisoryLockReleaseRequest = 118;
constexpr std::uint16_t kMessageCoordinateAdvisoryLockReleaseResult = 119;
constexpr std::uint16_t kMessageCoordinateFunctionCallRequest = 120;
constexpr std::uint16_t kMessageCoordinateFunctionCallResult = 121;
constexpr std::uint16_t kMessageCoordinateOperatorCallRequest = 122;
constexpr std::uint16_t kMessageCoordinateOperatorCallResult = 123;
constexpr std::uint16_t kMessageCoordinateCastRequest = 124;
constexpr std::uint16_t kMessageCoordinateCastResult = 125;
constexpr std::uint16_t kMessageCoordinateCompareRequest = 126;
constexpr std::uint16_t kMessageCoordinateCompareResult = 127;
constexpr std::uint16_t kMessageCoordinateDomainOperationRequest = 128;
constexpr std::uint16_t kMessageCoordinateDomainOperationResult = 129;
constexpr std::uint16_t kMessageCoordinateUdrInvokeRequest = 130;
constexpr std::uint16_t kMessageCoordinateUdrInvokeResult = 131;
constexpr std::uint16_t kMessageCoordinateProcedureInvokeRequest = 132;
constexpr std::uint16_t kMessageCoordinateProcedureInvokeResult = 133;
constexpr std::uint16_t kMessageCoordinateFunctionInvokeRequest = 134;
constexpr std::uint16_t kMessageCoordinateFunctionInvokeResult = 135;
constexpr std::uint16_t kMessageCoordinateAggregateInvokeRequest = 136;
constexpr std::uint16_t kMessageCoordinateAggregateInvokeResult = 137;
constexpr std::uint16_t kMessageCoordinateSequenceNextvalRequest = 138;
constexpr std::uint16_t kMessageCoordinateSequenceNextvalResult = 139;
constexpr std::uint16_t kMessageCoordinateSequenceCurrvalRequest = 140;
constexpr std::uint16_t kMessageCoordinateSequenceCurrvalResult = 141;
constexpr std::uint16_t kMessageCoordinateSequenceSetvalRequest = 142;
constexpr std::uint16_t kMessageCoordinateSequenceSetvalResult = 143;
constexpr std::uint16_t kMessageCoordinateQueryNumericRequest = 144;
constexpr std::uint16_t kMessageCoordinateQueryNumericResult = 145;
constexpr std::uint16_t kMessageCoordinateAdvancedDatatypeFamilyRequest = 146;
constexpr std::uint16_t kMessageCoordinateAdvancedDatatypeFamilyResult = 147;
constexpr std::uint16_t kMessageCoordinateProjectRequest = 148;
constexpr std::uint16_t kMessageCoordinateProjectResult = 149;
constexpr std::uint16_t kMessageCoordinateCatalogIntrospectRequest = 268;
constexpr std::uint16_t kMessageCoordinateCatalogIntrospectResult = 269;
constexpr std::uint16_t kMessageCoordinateKvStructuredReadRequest = 162;
constexpr std::uint16_t kMessageCoordinateKvStructuredReadResult = 163;
constexpr std::uint16_t kMessageCoordinateKvStructuredMutateRequest = 164;
constexpr std::uint16_t kMessageCoordinateKvStructuredMutateResult = 165;
constexpr std::uint16_t kMessageCoordinateKvStructuredScanRequest = 166;
constexpr std::uint16_t kMessageCoordinateKvStructuredScanResult = 167;
constexpr std::uint16_t kMessageCoordinateKvStructuredStreamReadRequest = 168;
constexpr std::uint16_t kMessageCoordinateKvStructuredStreamReadResult = 169;
constexpr std::uint16_t kMessageCoordinateKvStructuredStreamAppendRequest = 170;
constexpr std::uint16_t kMessageCoordinateKvStructuredStreamAppendResult = 171;
constexpr std::uint16_t kMessageCoordinateKvStructuredTimeseriesRequest = 172;
constexpr std::uint16_t kMessageCoordinateKvStructuredTimeseriesResult = 173;
constexpr std::uint16_t kMessageCoordinateDdlCreateDomainRequest = 176;
constexpr std::uint16_t kMessageCoordinateDdlCreateSchemaRequest = 178;
constexpr std::uint16_t kMessageCoordinateSystemConfigSetRequest = 174;
constexpr std::uint16_t kMessageCoordinateDdlCreateDomainResult = 177;
constexpr std::uint16_t kMessageCoordinateDdlAlterDomainRequest = 186;
constexpr std::uint16_t kMessageCoordinateDdlAlterDomainResult = 187;
constexpr std::uint16_t kMessageCoordinateDdlCreateViewRequest = 188;
constexpr std::uint16_t kMessageCoordinateDdlCreateViewResult = 189;
constexpr std::uint16_t kMessageCoordinateDdlCreateMaterializedViewRequest = 312;
constexpr std::uint16_t kMessageCoordinateDdlCreateMaterializedViewResult = 313;
constexpr std::uint16_t kMessageCoordinateDdlAlterViewRequest = 190;
constexpr std::uint16_t kMessageCoordinateDdlAlterViewResult = 191;
constexpr std::uint16_t kMessageCoordinateDdlDropViewRequest = 192;
constexpr std::uint16_t kMessageCoordinateDdlDropViewResult = 193;
constexpr std::uint16_t kMessageCoordinateDdlRefreshMaterializedViewRequest = 268;
constexpr std::uint16_t kMessageCoordinateDdlRefreshMaterializedViewResult = 269;
constexpr std::uint16_t kMessageCoordinateDdlDropMaterializedViewRequest = 306;
constexpr std::uint16_t kMessageCoordinateDdlDropMaterializedViewResult = 307;
constexpr std::uint16_t kMessageCoordinateDdlCreateTypeRequest = 300;
constexpr std::uint16_t kMessageCoordinateDdlCreateTypeResult = 301;
constexpr std::uint16_t kMessageCoordinateDdlAlterTypeRequest = 302;
constexpr std::uint16_t kMessageCoordinateDdlAlterTypeResult = 303;
constexpr std::uint16_t kMessageCoordinateDdlDropTypeRequest = 304;
constexpr std::uint16_t kMessageCoordinateDdlDropTypeResult = 305;
constexpr std::uint16_t kMessageCoordinateDdlDropTableRequest = 334;
constexpr std::uint16_t kMessageCoordinateDdlDropTableResult = 335;
constexpr std::uint16_t kMessageCoordinateDdlCreateTriggerRequest = 194;
constexpr std::uint16_t kMessageCoordinateDdlCreateTriggerResult = 195;
constexpr std::uint16_t kMessageCoordinateDdlAlterTriggerRequest = 196;
constexpr std::uint16_t kMessageCoordinateDdlAlterTriggerResult = 197;
constexpr std::uint16_t kMessageCoordinateDdlDropTriggerRequest = 198;
constexpr std::uint16_t kMessageCoordinateDdlDropTriggerResult = 199;
constexpr std::uint16_t kMessageCoordinateDdlCreateProcedureRequest = 200;
constexpr std::uint16_t kMessageCoordinateDdlCreateProcedureResult = 201;
constexpr std::uint16_t kMessageCoordinateDdlAlterProcedureRequest = 202;
constexpr std::uint16_t kMessageCoordinateDdlAlterProcedureResult = 203;
constexpr std::uint16_t kMessageCoordinateDdlDropProcedureRequest = 204;
constexpr std::uint16_t kMessageCoordinateDdlDropProcedureResult = 205;
constexpr std::uint16_t kMessageCoordinateDdlCreateFunctionRequest = 206;
constexpr std::uint16_t kMessageCoordinateDdlCreateFunctionResult = 207;
constexpr std::uint16_t kMessageCoordinateDdlAlterFunctionRequest = 208;
constexpr std::uint16_t kMessageCoordinateDdlAlterFunctionResult = 209;
constexpr std::uint16_t kMessageCoordinateDdlDropFunctionRequest = 210;
constexpr std::uint16_t kMessageCoordinateDdlDropFunctionResult = 211;
constexpr std::uint16_t kMessageCoordinateDdlCreatePackageRequest = 212;
constexpr std::uint16_t kMessageCoordinateDdlCreatePackageResult = 213;
constexpr std::uint16_t kMessageCoordinateDdlCreateSynonymRequest = 214;
constexpr std::uint16_t kMessageCoordinateDdlCreateSynonymResult = 215;
constexpr std::uint16_t kMessageCoordinateDdlCreateForeignTableRequest = 216;
constexpr std::uint16_t kMessageCoordinateDdlCreateForeignTableResult = 217;
constexpr std::uint16_t kMessageCoordinateDdlCreateFdwRequest = 326;
constexpr std::uint16_t kMessageCoordinateDdlCreateFdwResult = 327;
constexpr std::uint16_t kMessageCoordinateDdlDropFdwRequest = 328;
constexpr std::uint16_t kMessageCoordinateDdlDropFdwResult = 329;
constexpr std::uint16_t kMessageCoordinateDdlDropForeignTableRequest = 324;
constexpr std::uint16_t kMessageCoordinateDdlDropForeignTableResult = 325;
constexpr std::uint16_t kMessageCoordinateDdlDropSynonymRequest = 320;
constexpr std::uint16_t kMessageCoordinateDdlDropSynonymResult = 321;
constexpr std::uint16_t kMessageCoordinateDdlDropPackageRequest = 308;
constexpr std::uint16_t kMessageCoordinateDdlDropPackageResult = 309;
constexpr std::uint16_t kMessageCoordinateDdlAlterPackageRequest = 310;
constexpr std::uint16_t kMessageCoordinateDdlAlterPackageResult = 311;
constexpr std::uint16_t kMessageCoordinateDdlAlterSequenceRequest = 314;
constexpr std::uint16_t kMessageCoordinateDdlAlterSequenceResult = 315;
constexpr std::uint16_t kMessageCoordinateDdlDropSequenceRequest = 316;
constexpr std::uint16_t kMessageCoordinateDdlDropSequenceResult = 317;
enum class MessageType : std::uint16_t { kCoordinateDdlCreateTableAsQueryWithDataRequest = 330, kCoordinateDdlCreateTableAsQueryWithDataResult = 331, kCoordinateDdlCreateTableAsQueryWithNoDataRequest = 332, kCoordinateDdlCreateTableAsQueryWithNoDataResult = 333 };
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableAsQueryWithDataRequestV1 = 7331;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableAsQueryWithDataResultV1 = 7332;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableAsQueryWithNoDataRequestV1 = 7333;
constexpr std::uint32_t kSchemaCoordinateDdlCreateTableAsQueryWithNoDataResultV1 = 7334;
constexpr std::uint16_t kMessageCoordinateDdlCreateTemporaryTableRequest = 214;
constexpr std::uint16_t kMessageCoordinateDdlCreateTemporaryTableResult = 215;
constexpr std::uint16_t kMessageCoordinateDdlDropTemporaryTableRequest = 216;
constexpr std::uint16_t kMessageCoordinateDdlDropTemporaryTableResult = 217;
constexpr std::uint16_t kMessageCoordinateDdlRenameObjectVectorRequest = 218;
constexpr std::uint16_t kMessageCoordinateDdlRenameObjectVectorResult = 219;
constexpr std::uint16_t kMessageCoordinateDdlRenameObjectRequest = 230;
constexpr std::uint16_t kMessageCoordinateDdlRenameObjectResult = 231;
constexpr std::uint16_t kMessageCoordinateDdlCreateOrReplaceSrsRequest = 220;
constexpr std::uint16_t kMessageCoordinateDdlDropSrsRequest = 222;
constexpr std::uint16_t kMessageCoordinateDdlCreateOrReplaceSrsResult = 221;
constexpr std::uint16_t kMessageCoordinateDdlDropSrsResult = 223;
constexpr std::uint16_t kMessageCoordinateDdlCreateRewriteRuleRequest = 224;
constexpr std::uint16_t kMessageCoordinateDdlCreateRewriteRuleResult = 225;
constexpr std::uint16_t kMessageCoordinateDdlAlterRewriteRuleRequest = 226;
constexpr std::uint16_t kMessageCoordinateDdlAlterRewriteRuleResult = 227;
constexpr std::uint16_t kMessageCoordinateDdlDropRewriteRuleRequest = 228;
constexpr std::uint16_t kMessageCoordinateDdlDropRewriteRuleResult = 229;
constexpr std::uint16_t kMessageCoordinateDdlValidateConstraintRequest = 230;
constexpr std::uint16_t kMessageCoordinateDdlValidateConstraintResult = 231;
constexpr std::uint16_t kMessageCoordinateSecurityCreatePrivilegeTemplateRequest = 232;
constexpr std::uint16_t kMessageCoordinateSecurityCreatePrivilegeTemplateResult = 233;
constexpr std::uint16_t kMessageCoordinateSecurityCreateUserRequest = 330;
constexpr std::uint16_t kMessageCoordinateSecurityCreateUserResult = 331;
constexpr std::uint16_t kMessageCoordinateSecurityAlterUserRequest = 340;
constexpr std::uint16_t kMessageCoordinateSecurityAlterUserResult = 341;
constexpr std::uint16_t kMessageCoordinateSecurityCreateRoleRequest = 342;
constexpr std::uint16_t kMessageCoordinateSecurityCreateRoleResult = 343;
constexpr std::uint16_t kMessageCoordinateSecurityDropRoleRequest = 344;
constexpr std::uint16_t kMessageCoordinateSecurityDropRoleResult = 345;
constexpr std::uint16_t kMessageCoordinateSecurityCreatePolicyRequest = 346;
constexpr std::uint16_t kMessageCoordinateSecurityCreatePolicyResult = 347;
constexpr std::uint16_t kMessageCoordinateSecurityDropPolicyRequest = 348;
constexpr std::uint16_t kMessageCoordinateSecurityDropPolicyResult = 349;
constexpr std::uint16_t kMessageCoordinateSecurityAlterRoleRequest = 350;
constexpr std::uint16_t kMessageCoordinateSecurityAlterRoleResult = 351;
constexpr std::uint16_t kMessageCoordinateSecurityAlterPrivilegeTemplateRequest = 234;
constexpr std::uint16_t kMessageCoordinateSecurityAlterPrivilegeTemplateResult = 235;
constexpr std::uint16_t kMessageCoordinateSecurityDropPrivilegeTemplateRequest = 236;
constexpr std::uint16_t kMessageCoordinateSecurityDropPrivilegeTemplateResult = 237;
constexpr std::uint16_t kMessageCoordinateDatabaseCreateTemplateCloneRequest = 238;
constexpr std::uint16_t kMessageCoordinateDatabaseCreateTemplateCloneResult = 239;
constexpr std::uint16_t kMessageCoordinateDdlCreateAggregateRequest = 240;
constexpr std::uint16_t kMessageCoordinateDdlCreateAggregateResult = 241;
constexpr std::uint16_t kMessageCoordinateDdlAlterAggregateRequest = 242;
constexpr std::uint16_t kMessageCoordinateDdlAlterAggregateResult = 243;
constexpr std::uint16_t kMessageCoordinateDdlDropAggregateRequest = 244;
constexpr std::uint16_t kMessageCoordinateDdlDropAggregateResult = 245;
constexpr std::uint16_t kMessageCoordinateDdlPurgeSystemHistoryRequest = 246;
constexpr std::uint16_t kMessageCoordinateDdlPurgeSystemHistoryResult = 247;
constexpr std::uint16_t kMessageCoordinateDdlSetIndexOptimizerEligibilityRequest = 248;
constexpr std::uint16_t kMessageCoordinateDdlSetIndexOptimizerEligibilityResult = 249;
constexpr std::uint16_t kMessageCoordinateDdlSetTableTypeEnforcementRequest = 250;
constexpr std::uint16_t kMessageCoordinateDdlSetTableTypeEnforcementResult = 251;
constexpr std::uint16_t kMessageCoordinateDatabaseSerializeLogicalSnapshotRequest = 252;
constexpr std::uint16_t kMessageCoordinateDatabaseSerializeLogicalSnapshotResult = 253;
constexpr std::uint16_t kMessageCoordinateDatabaseDeserializeLogicalSnapshotRequest = 254;
constexpr std::uint16_t kMessageCoordinateDatabaseDeserializeLogicalSnapshotResult = 255;
constexpr std::uint16_t kMessageCoordinateDdlCreateSchemaResult = 179;
constexpr std::uint16_t kMessageCoordinateDdlCreateTableRequest = 180;
constexpr std::uint16_t kMessageCoordinateDdlCreateTableResult = 181;
constexpr std::uint16_t kMessageCoordinateDdlCreateIndexRequest = 182;
constexpr std::uint16_t kMessageCoordinateDdlCreateIndexResult = 183;
constexpr std::uint16_t kMessageCoordinateDdlDropIndexRequest = 184;
constexpr std::uint16_t kMessageCoordinateDdlDropIndexResult = 185;
constexpr std::uint16_t kMessageCoordinateSystemConfigSetResult = 175;
constexpr std::uint16_t kMessageCoordinateAggregateRequest = 150;
constexpr std::uint16_t kMessageCoordinateAggregateResult = 151;
constexpr std::uint16_t kMessageCoordinateGroupRequest = 152;
constexpr std::uint16_t kMessageCoordinateGroupResult = 153;
constexpr std::uint32_t kSchemaCoordinateSecurityCreateGroupMappingRequestV1 = 7365;
constexpr std::uint32_t kSchemaCoordinateSecurityCreateGroupMappingResultV1 = 7366;
constexpr std::uint16_t kMessageCoordinateSecurityCreateGroupMappingRequest = 352;
constexpr std::uint16_t kMessageCoordinateSecurityCreateGroupMappingResult = 353;
constexpr std::uint32_t kSchemaCoordinateSecurityDropGroupMappingRequestV1 = 7367;
constexpr std::uint32_t kSchemaCoordinateSecurityDropGroupMappingResultV1 = 7368;
constexpr std::uint16_t kMessageCoordinateSecurityDropGroupMappingRequest = 354;
constexpr std::uint16_t kMessageCoordinateSecurityDropGroupMappingResult = 355;
constexpr std::uint32_t kSchemaCoordinateSecurityGrantRequestV1 = 7369;
constexpr std::uint32_t kSchemaCoordinateSecurityGrantResultV1 = 7370;
constexpr std::uint16_t kMessageCoordinateSecurityGrantRequest = 356;
constexpr std::uint16_t kMessageCoordinateSecurityGrantResult = 357;
constexpr std::uint32_t kSchemaCoordinateSecurityRevokeRequestV1 = 7371;
constexpr std::uint32_t kSchemaCoordinateSecurityRevokeResultV1 = 7372;
constexpr std::uint16_t kMessageCoordinateSecurityRevokeRequest = 358;
constexpr std::uint16_t kMessageCoordinateSecurityRevokeResult = 359;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterPolicyRequestV1 = 7373;
constexpr std::uint32_t kSchemaCoordinateSecurityAlterPolicyResultV1 = 7374;
constexpr std::uint16_t kMessageCoordinateSecurityAlterPolicyRequest = 360;
constexpr std::uint16_t kMessageCoordinateSecurityAlterPolicyResult = 361;
constexpr std::uint32_t kSchemaCoordinateSecurityDropUserRequestV1 = 7375;
constexpr std::uint32_t kSchemaCoordinateSecurityDropUserResultV1 = 7376;
constexpr std::uint16_t kMessageCoordinateSecurityDropUserRequest = 362;
constexpr std::uint16_t kMessageCoordinateSecurityDropUserResult = 363;
constexpr std::uint32_t kSchemaCoordinateSecurityAuthenticateRequestV1 = 7377;
constexpr std::uint32_t kSchemaCoordinateSecurityAuthenticateResultV1 = 7378;
constexpr std::uint16_t kMessageCoordinateSecurityAuthenticateRequest = 364;
constexpr std::uint16_t kMessageCoordinateSecurityAuthenticateResult = 365;
constexpr std::uint32_t kSchemaCoordinateSecurityDeauthenticateRequestV1 = 7379;
constexpr std::uint32_t kSchemaCoordinateSecurityDeauthenticateResultV1 = 7380;
constexpr std::uint16_t kMessageCoordinateSecurityDeauthenticateRequest = 366;
constexpr std::uint16_t kMessageCoordinateSecurityDeauthenticateResult = 367;
constexpr std::uint32_t kSchemaSessionRoleSwitchRequestV1 = 7381;
constexpr std::uint32_t kSchemaSessionRoleSwitchResultV1 = 7382;
constexpr std::uint16_t kMessageSessionRoleSwitchRequest = 368;
constexpr std::uint16_t kMessageSessionRoleSwitchResult = 369;
constexpr std::uint32_t kSchemaSessionSettingSetRequestV1 = 7383;
constexpr std::uint32_t kSchemaSessionSettingSetResultV1 = 7384;
constexpr std::uint16_t kMessageSessionSettingSetRequest = 370;
constexpr std::uint16_t kMessageSessionSettingSetResult = 371;
constexpr std::uint32_t kSchemaSessionSettingResetRequestV1 = 7385;
constexpr std::uint32_t kSchemaSessionSettingResetResultV1 = 7386;
constexpr std::uint16_t kMessageSessionSettingResetRequest = 372;
constexpr std::uint16_t kMessageSessionSettingResetResult = 373;
constexpr std::uint16_t kMessageCoordinateSortRequest = 154;
constexpr std::uint16_t kMessageCoordinateSortResult = 155;
constexpr std::uint16_t kMessageCoordinateLimitRequest = 156;
constexpr std::uint16_t kMessageCoordinateLimitResult = 157;
constexpr std::uint16_t kMessageCoordinateWindowRequest = 158;
constexpr std::uint16_t kMessageCoordinateWindowResult = 159;
constexpr std::uint16_t kMessageCoordinateReturnResultSetRequest = 160;
constexpr std::uint16_t kMessageCoordinateReturnResultSetResult = 161;
constexpr std::uint16_t kMessageManagementResult = 31;
constexpr std::uint16_t kMessageResolveNameRequest = 32;
constexpr std::uint16_t kMessageResolveNameResult = 33;
constexpr std::uint16_t kMessageRenderUuidRequest = 34;
constexpr std::uint16_t kMessageRenderUuidResult = 35;
constexpr std::uint16_t kMessageAcquireStatementContextRequest = 36;
constexpr std::uint16_t kMessageAcquireStatementContextResult = 37;
constexpr std::uint16_t kMessagePrepareSblr = 40;
constexpr std::uint16_t kMessagePrepareResult = 41;
constexpr std::uint16_t kMessageExecuteSblr = 42;
constexpr std::uint16_t kMessageExecuteResult = 43;
constexpr std::uint16_t kMessageFetch = 44;
constexpr std::uint16_t kMessageFetchResult = 45;
constexpr std::uint16_t kMessageCloseCursor = 46;
constexpr std::uint16_t kMessageCloseCursorResult = 47;
constexpr std::uint16_t kMessageClosePreparedSblr = 48;
constexpr std::uint16_t kMessageClosePreparedSblrResult = 49;
constexpr std::uint16_t kMessageDiagnostic = 60;
constexpr std::uint16_t kMessageDisconnectNotice = 74;
constexpr std::uint32_t kMaxFramePayload = 1024 * 1024;
constexpr std::uint64_t kMaxChunkedPayload = static_cast<std::uint64_t>(kMaxFramePayload) * 16u;
constexpr std::uint32_t kCursorCloseFlagCancel = 1u << 0;
constexpr std::uint16_t kLongStringSentinel = 0xffff;
constexpr std::uint32_t kDefaultSbpsRequestTimeoutMs = 300000;
constexpr std::size_t kPortableAfUnixPathLimit = 108;
constexpr std::size_t kMaxSbpsClientPublicResolutionCacheEntries = 8192;
constexpr std::uint8_t kResolveNameProjectionRelationDescriptorV1 = 0x01u;
constexpr std::uint8_t kRelationDescriptorExtensionKind = 0x02u;
constexpr std::uint8_t kRelationDescriptorExtensionVersion = 0x01u;
constexpr std::uint8_t kRelationDescriptorExtensionVersionV2 = 0x02u;
constexpr std::size_t kMaxPublicRelationProjectionBytes = 512u * 1024u;
constexpr std::uint32_t kMaxPublicRelationProjectionColumns = 4096;
constexpr std::size_t kMaxPublicRelationMetadataTextBytes = 4096;
constexpr std::size_t kMaxPublicEncodedTypeDescriptorBytes = 65534;

bool EncodedDescriptorHasExactField(std::string_view descriptor,
                                    std::string_view key,
                                    std::string_view expected_value) {
  const std::string expected =
      std::string(key) + "=" + std::string(expected_value);
  const std::string prefix = std::string(key) + "=";
  bool matched = false;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto delimiter = descriptor.find(';', offset);
    const auto field = descriptor.substr(
        offset,
        delimiter == std::string_view::npos
            ? descriptor.size() - offset
            : delimiter - offset);
    if (field == expected) {
      if (matched) return false;
      matched = true;
    } else if (field.starts_with(prefix)) {
      return false;
    }
    if (delimiter == std::string_view::npos) break;
    offset = delimiter + 1;
  }
  return matched;
}

using SbpsClientTraceClock = std::chrono::steady_clock;

std::uint64_t SbpsClientElapsedMicros(
    SbpsClientTraceClock::time_point begin,
    SbpsClientTraceClock::time_point end = SbpsClientTraceClock::now()) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

void WriteSbpsClientPhaseTrace(std::string_view endpoint_path,
                               std::uint16_t message_type,
                               std::uint32_t schema_id,
                               int attempt_index,
                               std::size_t request_payload_bytes,
                               std::size_t encoded_frame_count,
                               std::size_t encoded_frame_bytes,
                               std::size_t response_payload_bytes,
                               bool success,
                               std::uint64_t endpoint_us,
                               std::uint64_t lock_wait_us,
                               std::uint64_t connect_us,
                               std::uint64_t encode_us,
                               std::uint64_t write_us,
                               std::uint64_t read_response_us,
                               std::uint64_t attempt_us,
                               std::uint64_t total_us) {
  const char* path = std::getenv("SCRATCHBIRD_SBPS_CLIENT_PHASE_TRACE_FILE");
  if (path == nullptr || *path == '\0') return;
  static std::mutex trace_mutex;
  std::lock_guard<std::mutex> guard(trace_mutex);
  std::ofstream out(path, std::ios::app);
  if (!out) return;
  out << "endpoint=" << endpoint_path
      << '\t' << "message_type=" << message_type
      << '\t' << "schema_id=" << schema_id
      << '\t' << "attempt=" << attempt_index
      << '\t' << "request_payload_bytes=" << request_payload_bytes
      << '\t' << "encoded_frame_count=" << encoded_frame_count
      << '\t' << "encoded_frame_bytes=" << encoded_frame_bytes
      << '\t' << "response_payload_bytes=" << response_payload_bytes
      << '\t' << "success=" << (success ? "true" : "false")
      << '\t' << "endpoint_us=" << endpoint_us
      << '\t' << "lock_wait_us=" << lock_wait_us
      << '\t' << "connect_us=" << connect_us
      << '\t' << "encode_us=" << encode_us
      << '\t' << "write_us=" << write_us
      << '\t' << "read_response_us=" << read_response_us
      << '\t' << "attempt_us=" << attempt_us
      << '\t' << "total_us=" << total_us
      << '\n';
}

std::string NormalizeLanguageTag(std::string_view value) {
  return value.empty() ? "en" : std::string(value);
}

std::string TrimAsciiLocal(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string InputFallbackTagForTag(std::string_view value) {
  const std::string tag = NormalizeLanguageTag(value);
  return tag == "en" ? std::string{} : "en";
}

void ApplySbpsLanguageContext(ParserSessionContext* session,
                              const ParserClientConfig& config,
                              std::string_view requested_language_tag,
                              std::uint64_t language_resource_epoch,
                              std::uint64_t localized_name_epoch) {
  if (session == nullptr) return;
  session->default_language = "en";
  session->language_tag = NormalizeLanguageTag(requested_language_tag);
  session->language_profile = config.default_language_profile;
  session->input_syntax_profile = config.input_syntax_profile;
  session->input_language_fallback_tag =
      InputFallbackTagForTag(session->language_tag);
  session->common_resource_hash = config.common_resource_hash;
  session->resource_compatibility_identity =
      config.resource_compatibility_identity;
  session->resource_version_identity = config.resource_version_identity;
  session->language_resource_epoch = language_resource_epoch;
  session->localized_name_epoch = localized_name_epoch;
  if (session->message_resource_epoch == 0) session->message_resource_epoch = 1;
}

struct FrameHeader {
  std::uint16_t message_type{0};
  std::uint32_t flags{0};
  std::uint32_t schema_id{0};
  std::uint32_t payload_len{0};
  std::uint64_t stream_id{0};
  std::uint64_t sequence_number{1};
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload;
};

void PutU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutAtU32(std::vector<std::uint8_t>* out, std::size_t offset, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    (*out)[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint32_t GetU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int byte = 3; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  if (offset + uuid.size() <= data.size()) {
    std::memcpy(uuid.data(), data.data() + offset, uuid.size());
  }
  return uuid;
}

void PutBytes32(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 32>& bytes) {
  out->insert(out->end(), bytes.begin(), bytes.end());
}

void PutBytes(std::vector<std::uint8_t>* out, const std::vector<std::uint8_t>& value) {
  PutU64(out, static_cast<std::uint64_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  if (value.size() >= kLongStringSentinel) {
    PutU16(out, kLongStringSentinel);
    PutU64(out, static_cast<std::uint64_t>(value.size()));
  } else {
    PutU16(out, static_cast<std::uint16_t>(value.size()));
  }
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadStringWithin(const std::vector<std::uint8_t>& data,
                      std::size_t* offset,
                      std::string* out,
                      std::size_t end,
                      std::size_t max_bytes) {
  if (offset == nullptr || out == nullptr || end > data.size() ||
      *offset > end || end - *offset < 2) {
    return false;
  }
  auto length = static_cast<std::uint64_t>(GetU16(data, *offset));
  *offset += 2;
  if (length == kLongStringSentinel) {
    if (*offset > end || end - *offset < 8) return false;
    length = GetU64(data, *offset);
    *offset += 8;
  }
  if (length > static_cast<std::uint64_t>(max_bytes) ||
      *offset > end ||
      length > static_cast<std::uint64_t>(end - *offset) ||
      length > static_cast<std::uint64_t>(
                   std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(data.data() + *offset),
              static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

bool ReadString(const std::vector<std::uint8_t>& data,
                std::size_t* offset,
                std::string* out) {
  return ReadStringWithin(data,
                          offset,
                          out,
                          data.size(),
                          std::numeric_limits<std::size_t>::max());
}

std::string UuidToText(const std::array<std::uint8_t, 16>& uuid);
bool UuidPresent(const std::array<std::uint8_t, 16>& uuid);
std::string OptionalUuidToText(const std::array<std::uint8_t, 16>& uuid);
void AddDiagnostic(
    MessageVectorSet* messages,
    std::string code,
    std::string message,
    std::string component = "parser_server_ipc.sbps_client",
    std::vector<Field> fields = {});
bool AppendTypedExecuteDiagnostics(
    const std::vector<std::uint8_t>& encoded_diagnostics,
    std::string_view expected_first_code,
    const std::array<std::uint8_t, 16>& expected_request_uuid,
    MessageVectorSet* messages);

std::string TextLineValue(std::string_view encoded, std::string_view key) {
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos && line.substr(0, equals) == key) {
      return std::string(line.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return {};
}

bool TextLineU64(std::string_view encoded, std::string_view key, std::uint64_t* out) {
  const auto value = TextLineValue(encoded, key);
  if (value.empty()) return false;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) return false;
    parsed = parsed * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  if (out != nullptr) *out = parsed;
  return true;
}

std::string EvidenceValue(std::string_view encoded, std::string_view evidence_kind) {
  const std::string prefix = "evidence=" + std::string(evidence_kind) + ":";
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    if (line.size() >= prefix.size() &&
        line.substr(0, prefix.size()) == prefix) {
      return std::string(line.substr(prefix.size()));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return {};
}

void PopulateTransactionStateFromPayload(std::string_view payload,
                                         ServerExecutionResult* result) {
  if (result == nullptr) return;
  std::uint64_t affected_rows = 0;
  if (TextLineU64(payload, "server_affected_rows", &affected_rows)) {
    result->affected_rows = affected_rows;
    result->affected_rows_present = true;
  }
  std::uint64_t local_transaction_id = 0;
  if (!TextLineU64(payload, "replacement_local_transaction_id", &local_transaction_id) &&
      !TextLineU64(payload, "local_transaction_id", &local_transaction_id)) {
    return;
  }
  result->transaction_state_present = true;
  result->local_transaction_id = local_transaction_id;
  std::uint64_t snapshot = 0;
  if (!TextLineU64(payload, "replacement_snapshot_visible_through_local_transaction_id", &snapshot)) {
    (void)TextLineU64(payload, "snapshot_visible_through_local_transaction_id", &snapshot);
  }
  result->snapshot_visible_through_local_transaction_id = snapshot;
  result->transaction_uuid = TextLineValue(payload, "replacement_transaction_uuid");
  if (result->transaction_uuid.empty()) {
    result->transaction_uuid = TextLineValue(payload, "transaction_uuid");
  }
  result->transaction_timestamp = TextLineValue(payload, "replacement_transaction_timestamp");
  if (result->transaction_timestamp.empty()) {
    result->transaction_timestamp = TextLineValue(payload, "transaction_timestamp");
  }
  if (result->transaction_timestamp.empty()) {
    result->transaction_timestamp = EvidenceValue(payload, "transaction_timestamp");
  }
}

bool ReadTransactionSelector(const std::vector<std::uint8_t>& payload,
                             std::size_t* offset,
                             ParserTransactionSelector* selector) {
  if (offset == nullptr || selector == nullptr || *offset + 8 > payload.size()) {
    return false;
  }
  selector->local_transaction_id = GetU64(payload, *offset);
  *offset += 8;
  return ReadString(payload, offset, &selector->transaction_uuid);
}

bool DecodeExecuteResultPayloadV2Base(const Frame& response,
                                      ServerExecutionResult* result,
                                      MessageVectorSet* messages) {
  if (result == nullptr) return false;
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) ||
      offset + 16 + 16 + 8 > response.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 execute result payload is malformed.");
    return false;
  }
  const auto result_request_uuid = GetUuid(response.payload, offset);
  if (!UuidPresent(result_request_uuid) ||
      (UuidPresent(response.header.request_uuid) &&
       response.header.request_uuid != result_request_uuid)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 execute result request identity is malformed or does not match its frame.");
    return false;
  }
  offset += 16;
  result->cursor_uuid = OptionalUuidToText(GetUuid(response.payload, offset));
  offset += 16;
  result->row_count = GetU64(response.payload, offset);
  offset += 8;
  std::string legacy_detail;
  if (!ReadString(response.payload, &offset, &result->operation_id) ||
      !ReadString(response.payload, &offset, &result->row_packet) ||
      !ReadString(response.payload, &offset, &legacy_detail) ||
      offset >= response.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 execute result payload is malformed.");
    return false;
  }
  const std::uint8_t transaction_flags = response.payload[offset++];
  if ((transaction_flags & 0xe0u) != 0) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction flags contain unknown bits.");
    return false;
  }
  result->selected_transaction_present = (transaction_flags & (1u << 0)) != 0;
  const bool encoded_finality_applied =
      (transaction_flags & (1u << 1)) != 0;
  result->finalized_transaction_present = (transaction_flags & (1u << 2)) != 0;
  result->replacement_transaction_present = (transaction_flags & (1u << 3)) != 0;
  result->catalog_invalidation_applied =
      (transaction_flags & (1u << 4)) != 0;
  if (offset >= response.payload.size() || response.payload[offset] > 3) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction finality state is malformed.");
    return false;
  }
  result->finality_state =
      static_cast<ParserTransactionFinality>(response.payload[offset++]);
  const bool finality_is_applied =
      result->finality_state == ParserTransactionFinality::kKnownApplied;
  if (encoded_finality_applied != finality_is_applied) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 applied-finality flag contradicts the typed finality state.");
    return false;
  }
  result->finality_applied = finality_is_applied;
  if (offset >= response.payload.size() || response.payload[offset] > 3) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction replacement reason is malformed.");
    return false;
  }
  result->replacement_reason =
      static_cast<ParserTransactionReplacementReason>(response.payload[offset++]);
  if (result->selected_transaction_present &&
      !ReadTransactionSelector(response.payload, &offset,
                               &result->selected_transaction)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 selected transaction selector is malformed.");
    return false;
  }
  if (result->finalized_transaction_present &&
      !ReadTransactionSelector(response.payload, &offset,
                               &result->finalized_transaction)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 finalized transaction selector is malformed.");
    return false;
  }
  if (result->replacement_transaction_present &&
      !ReadTransactionSelector(response.payload, &offset,
                               &result->replacement_transaction)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 replacement transaction selector is malformed.");
    return false;
  }
  if (!ReadString(response.payload, &offset,
                  &result->transaction_outcome_detail) ||
      !ReadString(response.payload, &offset,
                  &result->transaction_diagnostic_code)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction outcome is malformed.");
    return false;
  }
  // Schema 4012 keeps the transaction outcome typed even when statement
  // execution is rejected.  Its optional trailer is one length-delimited
  // ordinary SBPS message-vector payload; legacy V2 payloads without a
  // trailer remain decodable during the in-tree protocol transition.
  std::vector<std::uint8_t> encoded_diagnostics;
  if (offset != response.payload.size()) {
    constexpr std::uint64_t kMaximumTypedDiagnosticBytes = 1024u * 1024u;
    if (offset + 8 > response.payload.size()) {
      AddDiagnostic(messages,
                    "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                    "The server V2 typed diagnostic trailer is malformed at offset " +
                        std::to_string(offset) + " of " +
                        std::to_string(response.payload.size()) + ".");
      return false;
    }
    const std::uint64_t diagnostic_bytes = GetU64(response.payload, offset);
    offset += 8;
    if (diagnostic_bytes > kMaximumTypedDiagnosticBytes ||
        diagnostic_bytes >
            static_cast<std::uint64_t>(response.payload.size() - offset) ||
        offset + static_cast<std::size_t>(diagnostic_bytes) !=
            response.payload.size()) {
      AddDiagnostic(messages,
                    "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                    "The server V2 typed diagnostic trailer is malformed: offset=" +
                        std::to_string(offset) + ", bytes=" +
                        std::to_string(diagnostic_bytes) + ", total=" +
                        std::to_string(response.payload.size()) + ".");
      return false;
    }
    encoded_diagnostics.assign(
        response.payload.begin() + static_cast<std::ptrdiff_t>(offset),
        response.payload.end());
    offset = response.payload.size();
  }
  const auto selector_valid = [](bool present,
                                 const ParserTransactionSelector& selector) {
    return !present || selector.present();
  };
  const bool reason_present =
      result->replacement_reason !=
      ParserTransactionReplacementReason::kNone;
  const bool selected_matches_finalized =
      !result->selected_transaction_present ||
      !result->finalized_transaction_present ||
      (result->selected_transaction.local_transaction_id ==
           result->finalized_transaction.local_transaction_id &&
       result->selected_transaction.transaction_uuid ==
           result->finalized_transaction.transaction_uuid);
  const bool replacement_differs_from_finalized =
      !result->replacement_transaction_present ||
      !result->finalized_transaction_present ||
      result->replacement_transaction.local_transaction_id !=
          result->finalized_transaction.local_transaction_id ||
      result->replacement_transaction.transaction_uuid !=
          result->finalized_transaction.transaction_uuid;
  bool coherent =
      selector_valid(result->selected_transaction_present,
                     result->selected_transaction) &&
      selector_valid(result->finalized_transaction_present,
                     result->finalized_transaction) &&
      selector_valid(result->replacement_transaction_present,
                     result->replacement_transaction) &&
      (reason_present == result->replacement_transaction_present) &&
      selected_matches_finalized && replacement_differs_from_finalized &&
      result->replacement_reason !=
          ParserTransactionReplacementReason::kAutocommitReady &&
      (!result->catalog_invalidation_applied ||
       (result->finality_state == ParserTransactionFinality::kKnownApplied &&
        result->finalized_transaction_present &&
        result->operation_id == "transaction.commit"));
  switch (result->finality_state) {
    case ParserTransactionFinality::kNotApplicable:
      coherent = coherent && !result->finalized_transaction_present &&
                 !result->replacement_transaction_present;
      break;
    case ParserTransactionFinality::kKnownApplied:
      coherent = coherent && result->finalized_transaction_present &&
                 (!result->replacement_transaction_present || reason_present);
      break;
    case ParserTransactionFinality::kKnownNotApplied:
      coherent = coherent && !result->finalized_transaction_present &&
                 !result->replacement_transaction_present;
      break;
    case ParserTransactionFinality::kUnknown:
      coherent = coherent && !result->finalized_transaction_present &&
                 !result->replacement_transaction_present;
      break;
  }
  if (!coherent) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction outcome contains contradictory selector, finality, or replacement state.");
    return false;
  }
  if (outcome == "accepted" &&
      (result->finality_state ==
           ParserTransactionFinality::kKnownNotApplied ||
       result->finality_state == ParserTransactionFinality::kUnknown)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "An accepted V2 outcome cannot report unknown or known-not-applied finality.");
    return false;
  }
  PopulateTransactionStateFromPayload(result->row_packet, result);
  // Free-form row payload is never transaction authority in V2.  Preserve its
  // affected-row projection above, then rebuild active transaction state only
  // from the validated typed selector matrix.
  result->transaction_state_present = false;
  result->local_transaction_id = 0;
  result->snapshot_visible_through_local_transaction_id = 0;
  result->transaction_uuid.clear();
  result->transaction_timestamp.clear();
  // V2 selectors intentionally remain in their exact typed fields.  They do
  // not carry an MGA snapshot, so projecting one into the legacy session
  // state would publish a zero snapshot and silently corrupt caller state.
  if (outcome != "accepted" && outcome != "rejected") {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 execute outcome is not recognized.");
    return false;
  }
  result->accepted = outcome == "accepted";
  if (result->accepted && !encoded_diagnostics.empty()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "An accepted V2 outcome cannot carry rejection diagnostics.");
    return false;
  }
  if (!result->accepted) {
    if (!encoded_diagnostics.empty()) {
      if (!AppendTypedExecuteDiagnostics(encoded_diagnostics,
                                         result->transaction_diagnostic_code,
                                         result_request_uuid,
                                         messages)) {
        AddDiagnostic(messages,
                      "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                      "The server V2 typed diagnostic vector is malformed or contradicts its transaction diagnostic code.");
        return false;
      }
    } else {
      AddDiagnostic(messages,
                    !result->transaction_diagnostic_code.empty()
                        ? result->transaction_diagnostic_code
                        : (result->finality_applied
                               ? "PARSER_SERVER_IPC.FINALITY_APPLIED_REPLACEMENT_FAILED"
                               : "PARSER_SERVER_IPC.EXECUTE_REJECTED"),
                    result->transaction_outcome_detail.empty()
                        ? (legacy_detail.empty() ? "The server rejected V2 SBLR execution."
                                                 : legacy_detail)
                        : result->transaction_outcome_detail);
    }
  }
  return true;
}

bool DecodeExecuteResultPayloadV2(const Frame& response,
                                  ServerExecutionResult* result,
                                  MessageVectorSet* messages) {
  constexpr std::size_t kPresentDescriptorBytes =
      1 + 16 + 2 + 8 + 16 * 5 + 8 + 8;
  if (result == nullptr || response.payload.empty()) return false;
  Frame base = response;
  CursorStreamDescriptorV1 descriptor;
  if (response.payload.size() >= kPresentDescriptorBytes &&
      response.payload[response.payload.size() - kPresentDescriptorBytes] == 1) {
    std::size_t offset = response.payload.size() - kPresentDescriptorBytes + 1;
    const auto descriptor_uuid = GetUuid(response.payload, offset);
    offset += 16;
    descriptor.descriptor_version = GetU16(response.payload, offset);
    offset += 2;
    descriptor.descriptor_generation = GetU64(response.payload, offset);
    offset += 8;
    const auto cursor_uuid = GetUuid(response.payload, offset);
    offset += 16;
    const auto execution_uuid = GetUuid(response.payload, offset);
    offset += 16;
    const auto result_set_uuid = GetUuid(response.payload, offset);
    offset += 16;
    const auto row_descriptor_uuid = GetUuid(response.payload, offset);
    offset += 16;
    const auto snapshot_uuid = GetUuid(response.payload, offset);
    offset += 16;
    descriptor.max_chunk_rows = GetU64(response.payload, offset);
    offset += 8;
    descriptor.max_chunk_bytes = GetU64(response.payload, offset);
    descriptor.present = true;
    descriptor.stream_descriptor_uuid = OptionalUuidToText(descriptor_uuid);
    descriptor.cursor_uuid = OptionalUuidToText(cursor_uuid);
    descriptor.execution_uuid = OptionalUuidToText(execution_uuid);
    descriptor.result_set_uuid = OptionalUuidToText(result_set_uuid);
    descriptor.row_descriptor_uuid = OptionalUuidToText(row_descriptor_uuid);
    descriptor.snapshot_uuid = OptionalUuidToText(snapshot_uuid);
    if (descriptor.complete()) {
      base.payload.resize(response.payload.size() - kPresentDescriptorBytes);
    } else if (response.payload.back() == 0) {
      descriptor = CursorStreamDescriptorV1{};
      base.payload.pop_back();
    } else {
      AddDiagnostic(messages,
                    "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                    "The server V2 cursor stream descriptor trailer is malformed.");
      return false;
    }
  } else if (response.payload.back() == 0) {
    base.payload.pop_back();
  } else {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 cursor stream descriptor trailer is malformed.");
    return false;
  }
  if (!DecodeExecuteResultPayloadV2Base(base, result, messages)) return false;
  if (!result->cursor_uuid.empty()) {
    if (!descriptor.complete() || descriptor.cursor_uuid != result->cursor_uuid) {
      AddDiagnostic(messages,
                    "SERVER.STREAM.DESCRIPTOR_INVALID",
                    "The server cursor result lacks its exact versioned stream descriptor.");
      return false;
    }
    result->cursor_stream_descriptor = std::move(descriptor);
  } else if (descriptor.present) {
    AddDiagnostic(messages,
                  "SERVER.STREAM.DESCRIPTOR_INVALID",
                  "A cursor stream descriptor was returned without a cursor.");
    return false;
  }
  return true;
}

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(0u - (crc & 1u));
      crc = (crc >> 1u) ^ (0x82f63b78u & mask);
    }
  }
  return ~crc;
}

std::array<std::uint8_t, 16> MakeUuidV7Bytes() {
  static std::random_device rd;
  static std::mt19937_64 rng(rd());
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto timestamp = static_cast<std::uint64_t>(now);
  const auto r1 = rng();
  const auto r2 = rng();
  std::array<std::uint8_t, 16> uuid{};
  uuid[0] = static_cast<std::uint8_t>((timestamp >> 40u) & 0xffu);
  uuid[1] = static_cast<std::uint8_t>((timestamp >> 32u) & 0xffu);
  uuid[2] = static_cast<std::uint8_t>((timestamp >> 24u) & 0xffu);
  uuid[3] = static_cast<std::uint8_t>((timestamp >> 16u) & 0xffu);
  uuid[4] = static_cast<std::uint8_t>((timestamp >> 8u) & 0xffu);
  uuid[5] = static_cast<std::uint8_t>(timestamp & 0xffu);
  for (int i = 6; i < 14; ++i) {
    uuid[static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((r1 >> ((i - 6) * 8)) & 0xffu);
  }
  uuid[14] = static_cast<std::uint8_t>(r2 & 0xffu);
  uuid[15] = static_cast<std::uint8_t>((r2 >> 8u) & 0xffu);
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

std::string UuidToText(const std::array<std::uint8_t, 16>& uuid) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(kHex[(uuid[i] >> 4u) & 0x0fu]);
    out.push_back(kHex[uuid[i] & 0x0fu]);
  }
  return out;
}

bool UuidPresent(const std::array<std::uint8_t, 16>& uuid) {
  return std::any_of(uuid.begin(), uuid.end(), [](std::uint8_t value) {
    return value != 0;
  });
}

std::string OptionalUuidToText(const std::array<std::uint8_t, 16>& uuid) {
  return UuidPresent(uuid) ? UuidToText(uuid) : std::string{};
}

std::array<std::uint8_t, 16> TextToUuid(std::string_view text) {
  std::array<std::uint8_t, 16> out{};
  auto hex_value = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::size_t nibble = 0;
  for (const char ch : text) {
    if (ch == '-') continue;
    const int value = hex_value(ch);
    if (value < 0 || nibble >= 32) return {};
    if ((nibble % 2) == 0) {
      out[nibble / 2] = static_cast<std::uint8_t>(value << 4);
    } else {
      out[nibble / 2] = static_cast<std::uint8_t>(out[nibble / 2] | value);
    }
    ++nibble;
  }
  return nibble == 32 ? out : std::array<std::uint8_t, 16>{};
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV1(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  std::vector<std::uint8_t> out;
  out.reserve(2 + 16 + 8 + 16);
  PutU16(&out, 1);
  PutUuid(&out, TextToUuid(session.session_uuid));
  PutU64(&out, transaction.local_transaction_id);
  PutUuid(&out, TextToUuid(transaction.transaction_uuid));
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV2(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 2;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV3(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 3;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV4(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 4;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV5(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 5;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV6(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 6;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV7(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 7;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV8(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 8;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV9(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 9;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV10(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 10;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV11(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 11;
  return out;
}

bool IsCanonicalStatementTimestamp(std::string_view value) {
  if (value.size() != 20 &&
      (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigitIndexes[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigitIndexes) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](std::size_t offset, std::size_t digits) {
    unsigned result = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      result = result * 10 +
               static_cast<unsigned>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDaysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDaysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximum_day;
  return day != 0 && day <= maximum_day;
}

bool DecodeAcquireStatementContextPayloadV1(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  constexpr std::size_t kResultBytes = 2 + 1 + (6 * 16) + (2 * 8);
  if (context == nullptr || payload.size() != kResultBytes ||
      GetU16(payload, 0) != 1 || payload[2] != 1) {
    return false;
  }
  std::size_t offset = 3;
  const auto statement_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto local_transaction_id = GetU64(payload, offset);
  offset += 8;
  const auto transaction_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto statement_snapshot_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto statement_metadata_snapshot_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto catalog_epoch_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto security_context_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto high_watermark = GetU64(payload, offset);
  if (!UuidPresent(statement_uuid) || local_transaction_id == 0 ||
      !UuidPresent(transaction_uuid) ||
      !UuidPresent(statement_snapshot_uuid) ||
      !UuidPresent(statement_metadata_snapshot_uuid) ||
      !UuidPresent(catalog_epoch_uuid) ||
      !UuidPresent(security_context_uuid)) {
    return false;
  }
  ParserStatementContext decoded;
  decoded.acquired = true;
  decoded.statement_uuid = UuidToText(statement_uuid);
  decoded.transaction.local_transaction_id = local_transaction_id;
  decoded.transaction.transaction_uuid = UuidToText(transaction_uuid);
  decoded.statement_snapshot_uuid = UuidToText(statement_snapshot_uuid);
  decoded.statement_metadata_snapshot_uuid =
      UuidToText(statement_metadata_snapshot_uuid);
  decoded.catalog_epoch_uuid = UuidToText(catalog_epoch_uuid);
  decoded.security_context_uuid = UuidToText(security_context_uuid);
  decoded.snapshot_visible_through_local_transaction_id = high_watermark;
  if (!decoded.complete()) return false;
  *context = std::move(decoded);
  return true;
}

bool DecodeAcquireStatementContextPayloadNative(
    const std::vector<std::uint8_t>& payload,
    const std::uint16_t expected_version,
    const bool extended_aggregate_registry,
    const bool complete_aggregate_registry,
    const bool complete_window_registry,
    const std::uint8_t maximum_profile_kind,
    const bool has_statement_timestamp,
    const std::uint8_t exact_descriptor_cohort_version,
    ParserStatementContext* context,
    std::size_t* consumed_bytes = nullptr) {
  constexpr std::size_t kBaseBytes = 2 + 1 + (6 * 16) + (2 * 8);
  constexpr std::size_t kProfileBytes = 1 + 2 + (3 * 16) + 1 + (3 * 4);
  const bool exact_v8_descriptor_cohort =
      exact_descriptor_cohort_version == 8;
  const bool exact_v9_descriptor_cohort =
      exact_descriptor_cohort_version == 9;
  const bool exact_v10_descriptor_cohort =
      exact_descriptor_cohort_version == 10;
  const bool exact_descriptor_cohort =
      exact_v8_descriptor_cohort || exact_v9_descriptor_cohort ||
      exact_v10_descriptor_cohort;
  const std::size_t native_prefix_bytes =
      (extended_aggregate_registry ? 6U : 3U) * 16U + 2U +
      (has_statement_timestamp ? 22U : 0U);
  if (context == nullptr || payload.size() < kBaseBytes + native_prefix_bytes ||
      GetU16(payload, 0) != expected_version || payload[2] != 1) {
    return false;
  }

  std::vector<std::uint8_t> base(payload.begin(),
                                 payload.begin() + kBaseBytes);
  base[0] = 1;
  ParserStatementContext decoded;
  if (!DecodeAcquireStatementContextPayloadV1(base, &decoded)) return false;

  std::size_t offset = kBaseBytes;
  if (has_statement_timestamp &&
      (!ReadString(payload, &offset, &decoded.statement_timestamp) ||
      !IsCanonicalStatementTimestamp(decoded.statement_timestamp))) {
    return false;
  }
  const auto bound_ast_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto count_function_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto sum_function_uuid = GetUuid(payload, offset);
  offset += 16;
  std::array<std::uint8_t, 16> avg_function_uuid{};
  std::array<std::uint8_t, 16> min_function_uuid{};
  std::array<std::uint8_t, 16> max_function_uuid{};
  if (extended_aggregate_registry) {
    avg_function_uuid = GetUuid(payload, offset);
    offset += 16;
    min_function_uuid = GetUuid(payload, offset);
    offset += 16;
    max_function_uuid = GetUuid(payload, offset);
    offset += 16;
  }
  if (complete_aggregate_registry) {
    if (offset + 2 > payload.size()) return false;
    const auto aggregate_count = GetU16(payload, offset);
    offset += 2;
    if (aggregate_count != 43) return false;
    std::set<std::string> builtin_ids;
    std::set<std::string> function_uuids;
    decoded.aggregate_function_profiles.reserve(aggregate_count);
    for (std::uint16_t index = 0; index < aggregate_count; ++index) {
      if (offset + 2 > payload.size()) return false;
      ParserStatementContext::AggregateFunctionProfile profile;
      profile.abi_version = GetU16(payload, offset);
      offset += 2;
      if (!ReadString(payload, &offset, &profile.builtin_id) ||
          offset + 17 > payload.size()) {
        return false;
      }
      const auto function_uuid = GetUuid(payload, offset);
      offset += 16;
      const auto executable = payload[offset++];
      profile.function_uuid = UuidToText(function_uuid);
      profile.executable = executable == 1;
      if (profile.abi_version != 1 ||
          !profile.builtin_id.starts_with("sb.aggregate.") ||
          profile.builtin_id.size() <= std::string_view("sb.aggregate.").size() ||
          !UuidPresent(function_uuid) || executable != 1 ||
          !builtin_ids.insert(profile.builtin_id).second ||
          !function_uuids.insert(profile.function_uuid).second) {
        return false;
      }
      decoded.aggregate_function_profiles.push_back(std::move(profile));
    }
  }
  if (complete_window_registry) {
    if (offset + 2 > payload.size()) return false;
    const auto window_count = GetU16(payload, offset);
    offset += 2;
    if (window_count != 11) return false;
    std::set<std::string> builtin_ids;
    std::set<std::string> function_uuids;
    decoded.window_function_profiles.reserve(window_count);
    for (std::uint16_t index = 0; index < window_count; ++index) {
      if (offset + 2 > payload.size()) return false;
      ParserStatementContext::WindowFunctionProfile profile;
      profile.abi_version = GetU16(payload, offset);
      offset += 2;
      if (!ReadString(payload, &offset, &profile.builtin_id) ||
          offset + 17 > payload.size()) {
        return false;
      }
      const auto function_uuid = GetUuid(payload, offset);
      offset += 16;
      const auto executable = payload[offset++];
      profile.function_uuid = UuidToText(function_uuid);
      profile.executable = executable == 1;
      if (profile.abi_version != 1 ||
          !profile.builtin_id.starts_with("sb.window.") ||
          profile.builtin_id.size() <=
              std::string_view("sb.window.").size() ||
          !UuidPresent(function_uuid) || executable != 1 ||
          !builtin_ids.insert(profile.builtin_id).second ||
          !function_uuids.insert(profile.function_uuid).second) {
        return false;
      }
      decoded.window_function_profiles.push_back(std::move(profile));
    }
  }
  if (offset + 2 > payload.size()) return false;
  const auto profile_count = GetU16(payload, offset);
  offset += 2;
  if (!UuidPresent(bound_ast_uuid) || !UuidPresent(count_function_uuid) ||
      !UuidPresent(sum_function_uuid) ||
      (extended_aggregate_registry &&
       (!UuidPresent(avg_function_uuid) || !UuidPresent(min_function_uuid) ||
        !UuidPresent(max_function_uuid))) ||
      profile_count == 0 ||
      profile_count > static_cast<std::uint16_t>(maximum_profile_kind) * 32u ||
      (exact_v8_descriptor_cohort && profile_count != 322) ||
      (exact_v9_descriptor_cohort && profile_count != 326) ||
      (exact_v10_descriptor_cohort && profile_count != 646) ||
      (consumed_bytes == nullptr
           ? payload.size() != offset +
                                 static_cast<std::size_t>(profile_count) *
                                     kProfileBytes
           : payload.size() < offset +
                                 static_cast<std::size_t>(profile_count) *
                                     kProfileBytes)) {
    return false;
  }

  std::array<std::uint16_t, 24> expected_slots{};
  std::set<std::string> descriptor_uuids;
  std::array<std::string, 24> exact_type_uuids;
  decoded.descriptor_profiles.reserve(profile_count);
  for (std::uint16_t index = 0; index < profile_count; ++index) {
    ParserStatementContext::DescriptorProfile profile;
    profile.profile_kind = payload[offset++];
    profile.slot = GetU16(payload, offset);
    offset += 2;
    const auto descriptor_uuid = GetUuid(payload, offset);
    offset += 16;
    const auto type_uuid = GetUuid(payload, offset);
    offset += 16;
    const auto collation_uuid = GetUuid(payload, offset);
    offset += 16;
    const auto nullable = payload[offset++];
    profile.width = GetU32(payload, offset);
    offset += 4;
    profile.precision = GetU32(payload, offset);
    offset += 4;
    profile.scale = GetU32(payload, offset);
    offset += 4;
    std::uint8_t exact_expected_kind = 0;
    std::uint16_t exact_expected_slot = 0;
    if (exact_descriptor_cohort) {
      if (index < 320) {
        exact_expected_kind = static_cast<std::uint8_t>(index / 32 + 1);
        exact_expected_slot = static_cast<std::uint16_t>(index % 32);
      } else if (index < 322) {
        exact_expected_kind = 11;
        exact_expected_slot = static_cast<std::uint16_t>(index - 320);
      } else if (index < 324) {
        exact_expected_kind = 12;
        exact_expected_slot = static_cast<std::uint16_t>(index - 322);
      } else if (index < 326) {
        exact_expected_kind = 13;
        exact_expected_slot = static_cast<std::uint16_t>(index - 324);
      } else {
        exact_expected_kind =
            static_cast<std::uint8_t>(14 + (index - 326) / 32);
        exact_expected_slot = static_cast<std::uint16_t>((index - 326) % 32);
      }
    }
    if (profile.profile_kind < 1 ||
        profile.profile_kind > maximum_profile_kind ||
        profile.slot != expected_slots[profile.profile_kind]++ ||
        (exact_descriptor_cohort &&
         (profile.profile_kind != exact_expected_kind ||
          profile.slot != exact_expected_slot)) ||
        !UuidPresent(descriptor_uuid) || !UuidPresent(type_uuid) ||
        (exact_descriptor_cohort &&
         (((descriptor_uuid[6] & 0xf0u) != 0x70u) ||
          ((descriptor_uuid[8] & 0xc0u) != 0x80u) ||
          ((type_uuid[6] & 0xf0u) == 0) ||
          ((type_uuid[8] & 0xc0u) != 0x80u))) ||
        nullable > 1 ||
        (((profile.profile_kind <= 10 && profile.profile_kind % 2 == 0) ||
          (profile.profile_kind >= 14 && profile.profile_kind % 2 == 1)) !=
         (nullable == 1)) ||
        profile.scale > profile.precision) {
      return false;
    }
    profile.descriptor_uuid = UuidToText(descriptor_uuid);
    profile.type_uuid = UuidToText(type_uuid);
    profile.collation_uuid = OptionalUuidToText(collation_uuid);
    profile.nullable = nullable == 1;
    if (!descriptor_uuids.insert(profile.descriptor_uuid).second) return false;
    if (exact_descriptor_cohort && profile.profile_kind >= 11) {
      const bool exact_nullable = profile.profile_kind >= 14 &&
                                  profile.profile_kind % 2 == 1;
      if (profile.nullable != exact_nullable || UuidPresent(collation_uuid) ||
          profile.width != 0 || profile.precision != 0 ||
          profile.scale != 0) {
        return false;
      }
      auto& exact_type_uuid = exact_type_uuids[profile.profile_kind];
      if (exact_type_uuid.empty()) {
        exact_type_uuid = profile.type_uuid;
      } else if (profile.type_uuid != exact_type_uuid) {
        return false;
      }
    }
    decoded.descriptor_profiles.push_back(std::move(profile));
  }
  if (consumed_bytes == nullptr && offset != payload.size()) return false;
  for (std::size_t kind = 1; kind <= maximum_profile_kind; ++kind) {
    if (expected_slots[kind] == 0 ||
        (exact_descriptor_cohort &&
         expected_slots[kind] !=
             ((exact_v10_descriptor_cohort && kind >= 14)
                  ? 32
                  : (kind >= 11 ? 2 : 32)))) {
      return false;
    }
  }
  if (exact_v9_descriptor_cohort &&
      (exact_type_uuids[11].empty() || exact_type_uuids[12].empty() ||
       exact_type_uuids[13].empty() ||
       exact_type_uuids[11] == exact_type_uuids[12] ||
       exact_type_uuids[11] == exact_type_uuids[13] ||
       exact_type_uuids[12] == exact_type_uuids[13])) {
    return false;
  }
  if (exact_v10_descriptor_cohort) {
    if (exact_type_uuids[11].empty() || exact_type_uuids[12].empty() ||
        exact_type_uuids[13].empty() ||
        exact_type_uuids[14] != exact_type_uuids[15] ||
        exact_type_uuids[16] != exact_type_uuids[17] ||
        exact_type_uuids[18] != exact_type_uuids[19] ||
        exact_type_uuids[20] != exact_type_uuids[21] ||
        exact_type_uuids[22] != exact_type_uuids[23]) {
      return false;
    }
    const std::array<std::string, 5> multileg_type_uuids = {
        exact_type_uuids[14], exact_type_uuids[16], exact_type_uuids[18],
        exact_type_uuids[20], exact_type_uuids[22]};
    if (std::any_of(multileg_type_uuids.begin(), multileg_type_uuids.end(),
                    [](const auto& value) { return value.empty(); }) ||
        std::set<std::string>(multileg_type_uuids.begin(),
                              multileg_type_uuids.end()).size() != 5 ||
        exact_type_uuids[14] != exact_type_uuids[12] ||
        exact_type_uuids[16] != exact_type_uuids[13] ||
        exact_type_uuids[18] != exact_type_uuids[11]) {
      return false;
    }
  }
  decoded.bound_ast_uuid = UuidToText(bound_ast_uuid);
  decoded.count_function_uuid = UuidToText(count_function_uuid);
  decoded.sum_function_uuid = UuidToText(sum_function_uuid);
  if (extended_aggregate_registry) {
    decoded.avg_function_uuid = UuidToText(avg_function_uuid);
    decoded.min_function_uuid = UuidToText(min_function_uuid);
    decoded.max_function_uuid = UuidToText(max_function_uuid);
  }
  if (has_statement_timestamp && !exact_descriptor_cohort &&
      !decoded.native_v7_complete()) {
    return false;
  }
  if (exact_v8_descriptor_cohort && !decoded.native_v8_complete()) return false;
  if (exact_v9_descriptor_cohort && !decoded.native_v9_complete()) return false;
  if (exact_v10_descriptor_cohort && !decoded.native_v10_complete()) {
    return false;
  }
  if (consumed_bytes != nullptr) *consumed_bytes = offset;
  *context = std::move(decoded);
  return true;
}

bool DecodeAcquireStatementContextPayloadV2(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 2, false,
                                                     false, false, 6, false,
                                                     false, context);
}

bool DecodeAcquireStatementContextPayloadV3(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 3, true,
                                                     false, false, 6, false,
                                                     false, context);
}

bool DecodeAcquireStatementContextPayloadV4(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 4, true, true,
                                                     false, 6, false,
                                                     false, context);
}

bool DecodeAcquireStatementContextPayloadV5(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 5, true, true,
                                                     false, 10, false,
                                                     false, context);
}

bool DecodeAcquireStatementContextPayloadV6(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 6, true, true,
                                                     true, 10, false,
                                                     false, context);
}

bool DecodeAcquireStatementContextPayloadV7(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 7, true, true,
                                                     true, 10, true,
                                                     false, context);
}

bool DecodeAcquireStatementContextPayloadV8(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 8, true, true,
                                                     true, 11, true, 8,
                                                     context);
}

bool DecodeAcquireStatementContextPayloadV9(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 9, true, true,
                                                     true, 13, true, 9,
                                                     context);
}

bool DecodeAcquireStatementContextPayloadV10(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 10, true, true,
                                                     true, 23, true, 10,
                                                     context);
}

bool DecodeAcquireStatementContextPayloadV11(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  std::size_t offset = 0;
  if (!DecodeAcquireStatementContextPayloadNative(
          payload, 11, true, true, true, 23, true, 10, context, &offset) ||
      (payload.size() != offset + 76 && payload.size() != offset + 156 &&
       payload.size() != offset + 228 && payload.size() < offset + 260) ||
      GetU16(payload, offset + 2) != 0) {
    return false;
  }
  const auto wire_extension_version = GetU16(payload, offset);
  const auto extension_version = wire_extension_version >= 27 && wire_extension_version <= 58 ? 26 : wire_extension_version;
  if ((payload.size() == offset + 76 && wire_extension_version != 2) ||
      (payload.size() == offset + 156 && extension_version != 3) ||
      (payload.size() == offset + 228 && extension_version != 4) ||
      (payload.size() >= offset + 260 && extension_version != 5 &&
       extension_version != 6 && extension_version != 7 &&
       extension_version != 8 && extension_version != 9 && extension_version != 10 && extension_version != 11 && extension_version != 12 && extension_version != 13 && extension_version != 14 && extension_version != 15 && extension_version != 16 && extension_version != 17 && extension_version != 18 && extension_version != 19 && extension_version != 20 && extension_version != 21 && extension_version != 22 && extension_version != 23 && extension_version != 24 && extension_version != 25 && extension_version != 26 && wire_extension_version != 27 && wire_extension_version != 28 && wire_extension_version != 29 && wire_extension_version != 30 && wire_extension_version != 31 && wire_extension_version != 32 && wire_extension_version != 33 && wire_extension_version != 34 && wire_extension_version != 35 && wire_extension_version != 36 && wire_extension_version != 37 && wire_extension_version != 38 && wire_extension_version != 39 && wire_extension_version != 40 && wire_extension_version != 41 && wire_extension_version != 42 && wire_extension_version != 43 && wire_extension_version != 44 && wire_extension_version != 45 && wire_extension_version != 46 && wire_extension_version != 47 && wire_extension_version != 48 && wire_extension_version != 49 && wire_extension_version != 50 && wire_extension_version != 51 && wire_extension_version != 52 && wire_extension_version != 53 && wire_extension_version != 54 && wire_extension_version != 55 && wire_extension_version != 56 && wire_extension_version != 57 && wire_extension_version != 58 && wire_extension_version != 59 && wire_extension_version != 60 && wire_extension_version != 61)) {
    return false;
  }
  const auto preliminary_receipt_uuid = GetUuid(payload, offset + 4);
  const auto catalog_snapshot_uuid = GetUuid(payload, offset + 20);
  const auto catalog_generation = GetU64(payload, offset + 36);
  const auto security_epoch = GetU64(payload, offset + 44);
  const auto resource_epoch = GetU64(payload, offset + 52);
  const auto mga_snapshot_uuid = GetUuid(payload, offset + 60);
  if (!UuidPresent(preliminary_receipt_uuid) ||
      !UuidPresent(catalog_snapshot_uuid) || catalog_generation == 0 ||
      security_epoch == 0 || resource_epoch == 0 ||
      !UuidPresent(mga_snapshot_uuid)) {
    return false;
  }
  context->literal_statement_descriptor_profiles.clear();
  context->preliminary_receipt_uuid = UuidToText(preliminary_receipt_uuid);
  context->preliminary_catalog_snapshot_uuid = UuidToText(catalog_snapshot_uuid);
  context->preliminary_catalog_generation = catalog_generation;
  context->preliminary_security_epoch = security_epoch;
  context->preliminary_resource_epoch = resource_epoch;
  context->preliminary_mga_snapshot_uuid = UuidToText(mga_snapshot_uuid);
  context->preliminary_extension_version = extension_version;
  context->preliminary_prepared_statement_uuid.clear();
  context->preliminary_prepared_generation = 0;
  context->preliminary_batch_uuid.clear();
  context->preliminary_batch_generation = 0;
  context->preliminary_dynamic_package_uuid.clear();
  context->preliminary_dynamic_generation = 0;
  context->preliminary_parameter_executor_availability_generation = 0;
  context->preliminary_variable_scope_uuid.clear();
  context->preliminary_variable_scope_generation = 0;
  context->preliminary_variable_frame_uuid.clear();
  context->preliminary_variable_frame_generation = 0;
  context->preliminary_variable_registry_snapshot_uuid.clear();
  context->preliminary_variable_executor_availability_generation = 0;
  context->preliminary_diagnostic_registry_snapshot_uuid.clear();
  context->preliminary_diagnostic_registry_generation = 0;
  context->preliminary_diagnostic_identities.clear();
  context->preliminary_transaction_isolation_profile_uuid.clear();
  context->preliminary_transaction_isolation_profile_generation = 0;
  context->preliminary_transaction_policy_snapshot_uuid.clear();
  context->preliminary_transaction_policy_generation = 0;
  context->preliminary_transaction_executor_availability_generation = 0;
  context->preliminary_transaction_read_mode = 0;
  context->preliminary_transaction_authority_scope = 0;
  context->preliminary_transaction_wait_policy = 0;
  context->preliminary_transaction_deadline_monotonic_ns = 0;
  context->preliminary_transaction_commit_executor_availability_generation = 0;
  context->preliminary_transaction_commit_mode = 0;
  context->preliminary_transaction_commit_authority_scope = 0;
  context->preliminary_transaction_commit_wait_policy = 0;
  context->preliminary_transaction_commit_deadline_monotonic_ns = 0;
  context->preliminary_transaction_rollback_executor_availability_generation = 0;
  context->preliminary_transaction_rollback_mode = 0;
  context->preliminary_transaction_rollback_authority_scope = 0;
  context->preliminary_transaction_rollback_wait_policy = 0;
  context->preliminary_transaction_rollback_deadline_monotonic_ns = 0;
  context->preliminary_transaction_release_savepoint_executor_availability_generation = 0;
  context->preliminary_transaction_rollback_to_savepoint_executor_availability_generation = 0;
  context->preliminary_psql_autonomous_frame_executor_availability_generation = 0;
  context->preliminary_transaction_reservation_release_executor_availability_generation = 0;
  context->preliminary_temporary_instance_cleanup_executor_availability_generation = 0;
  context->preliminary_cursor_open_executor_availability_generation = 0;
  context->preliminary_cursor_fetch_executor_availability_generation = 0;
  context->preliminary_cursor_close_executor_availability_generation = 0;
  context->preliminary_read_by_key_executor_availability_generation = 0;
  context->preliminary_read_range_executor_availability_generation = 0;
  context->preliminary_read_stream_executor_availability_generation = 0;
  context->preliminary_result_set_pass_executor_availability_generation = 0;
  context->preliminary_access_cursor_open_executor_availability_generation = 0;
  context->preliminary_access_cursor_fetch_executor_availability_generation = 0;
  context->preliminary_access_cursor_close_executor_availability_generation = 0;
  context->preliminary_insert_executor_availability_generation = 0;
  context->preliminary_update_executor_availability_generation = 0;
  context->preliminary_delete_executor_availability_generation = 0;
  context->preliminary_merge_executor_availability_generation = 0;
  context->preliminary_table_truncate_executor_availability_generation = 0;
  context->preliminary_table_analyze_executor_availability_generation = 0;
  if (extension_version >= 3 && extension_version <= 26) {
    const auto prepared_uuid = GetUuid(payload, offset + 76);
    const auto prepared_generation = GetU64(payload, offset + 92);
    const auto batch_uuid = GetUuid(payload, offset + 100);
    const auto batch_generation = GetU64(payload, offset + 116);
    const auto dynamic_uuid = GetUuid(payload, offset + 124);
    const auto dynamic_generation = GetU64(payload, offset + 140);
    const auto parameter_executor_availability_generation =
        GetU64(payload, offset + 148);
    const auto exact_pair = [](const auto& uuid, std::uint64_t generation) {
      return UuidPresent(uuid) == (generation != 0);
    };
    if (!exact_pair(prepared_uuid, prepared_generation) ||
        !exact_pair(batch_uuid, batch_generation) ||
        !exact_pair(dynamic_uuid, dynamic_generation) ||
        parameter_executor_availability_generation == 0) {
      return false;
    }
    if (UuidPresent(prepared_uuid))
      context->preliminary_prepared_statement_uuid = UuidToText(prepared_uuid);
    context->preliminary_prepared_generation = prepared_generation;
    if (UuidPresent(batch_uuid))
      context->preliminary_batch_uuid = UuidToText(batch_uuid);
    context->preliminary_batch_generation = batch_generation;
    if (UuidPresent(dynamic_uuid))
      context->preliminary_dynamic_package_uuid = UuidToText(dynamic_uuid);
    context->preliminary_dynamic_generation = dynamic_generation;
    context->preliminary_parameter_executor_availability_generation =
        parameter_executor_availability_generation;
  }
  if (extension_version >= 4 && extension_version <= 26) {
    const auto scope_uuid = GetUuid(payload, offset + 156);
    const auto scope_generation = GetU64(payload, offset + 172);
    const auto frame_uuid = GetUuid(payload, offset + 180);
    const auto frame_generation = GetU64(payload, offset + 196);
    const auto registry_snapshot_uuid = GetUuid(payload, offset + 204);
    const auto executor_generation = GetU64(payload, offset + 220);
    const auto exact_pair = [](const auto& uuid, std::uint64_t generation) {
      return UuidPresent(uuid) == (generation != 0);
    };
    if ((extension_version == 4 &&
         (!UuidPresent(scope_uuid) || scope_generation == 0 ||
          !UuidPresent(frame_uuid) || frame_generation == 0)) ||
        ((extension_version >= 5 && extension_version <= 26) &&
         (!exact_pair(scope_uuid, scope_generation) ||
          !exact_pair(frame_uuid, frame_generation) ||
          (UuidPresent(registry_snapshot_uuid) != (executor_generation != 0)))) ||
        (extension_version == 4 &&
         (!UuidPresent(registry_snapshot_uuid) || executor_generation == 0))) {
      return false;
    }
    context->preliminary_variable_scope_uuid = UuidToText(scope_uuid);
    context->preliminary_variable_scope_generation = scope_generation;
    context->preliminary_variable_frame_uuid = UuidToText(frame_uuid);
    context->preliminary_variable_frame_generation = frame_generation;
    context->preliminary_variable_registry_snapshot_uuid =
        UuidToText(registry_snapshot_uuid);
    context->preliminary_variable_executor_availability_generation =
        executor_generation;
  }
  if (extension_version >= 5 && extension_version <= 26) {
    const auto diagnostic_snapshot_uuid = GetUuid(payload, offset + 228);
    const auto diagnostic_generation = GetU64(payload, offset + 244);
    const auto row_count = GetU32(payload, offset + 252);
    const auto row_bytes = GetU32(payload, offset + 256);
    if (wire_extension_version != 56 && (!UuidPresent(diagnostic_snapshot_uuid) || diagnostic_generation == 0 ||
        row_count == 0 || row_bytes != 72 || row_count > 4096 ||
        payload.size() != offset + 260 + static_cast<std::size_t>(row_count) * 72 +
                              (extension_version == 6 ? 80 :
                               extension_version == 7 ? 104 :
                               wire_extension_version == 61 ? 552 : wire_extension_version == 60 ? 544 : wire_extension_version == 59 ? 536 : wire_extension_version == 58 ? 528 :
                               wire_extension_version == 57 ? 520 : wire_extension_version == 56 ? 512 : wire_extension_version == 55 ? 504 : wire_extension_version == 54 ? 496 : wire_extension_version == 53 ? 488 : wire_extension_version == 52 ? 480 : wire_extension_version == 51 ? 472 : wire_extension_version == 50 ? 464 : wire_extension_version == 49 ? 456 : wire_extension_version == 48 ? 448 : wire_extension_version == 47 ? 440 : wire_extension_version == 46 ? 432 : wire_extension_version == 45 ? 424 : wire_extension_version == 44 ? 416 :
                               wire_extension_version == 43 ? 408 :
                               wire_extension_version == 42 ? 400 : wire_extension_version == 41 ? 392 : wire_extension_version == 40 ? 384 : wire_extension_version == 39 ? 376 : wire_extension_version == 38 ? 368 : wire_extension_version == 37 ? 360 : wire_extension_version == 36 ? 352 : wire_extension_version == 35 ? 344 : wire_extension_version == 34 ? 336 : wire_extension_version == 33 ? 328 : wire_extension_version == 32 ? 320 : wire_extension_version == 31 ? 312 : wire_extension_version == 30 ? 304 : wire_extension_version == 29 ? 296 : wire_extension_version == 28 ? 288 : wire_extension_version == 27 ? 280 : extension_version == 8 ? 128 : extension_version == 9 ? 136 : extension_version == 10 ? 144 : extension_version == 11 ? 152 : extension_version == 12 ? 160 : extension_version == 13 ? 168 : extension_version == 14 ? 176 : extension_version == 15 ? 184 : extension_version == 16 ? 192 : extension_version == 17 ? 200 : extension_version == 18 ? 208 : extension_version == 19 ? 216 : extension_version == 20 ? 224 : extension_version == 21 ? 232 : extension_version == 22 ? 240 : extension_version == 23 ? 248 : extension_version == 24 ? 256 : extension_version == 25 ? 264 : extension_version == 26 ? 272 : 0))) {
      return false;
    }
    context->preliminary_diagnostic_registry_snapshot_uuid =
        UuidToText(diagnostic_snapshot_uuid);
    context->preliminary_diagnostic_registry_generation = diagnostic_generation;
    context->preliminary_diagnostic_identities.reserve(row_count);
    for (std::uint32_t index = 0; index < row_count; ++index) {
      const auto at = offset + 260 + static_cast<std::size_t>(index) * 72;
      PreliminaryDiagnosticIdentityV1 row;
      const auto uuid = GetUuid(payload, at);
      row.generation = GetU64(payload, at + 16);
      row.precedence_ordinal = GetU32(payload, at + 24);
      row.severity_code = payload[at + 28];
      row.redaction_class = payload[at + 29];
      row.max_safe_fields = GetU32(payload, at + 32);
      if (!UuidPresent(uuid) || row.generation == 0 ||
          GetU16(payload, at + 30) != 0 || GetU32(payload, at + 36) != 0) {
        return false;
      }
      row.diagnostic_uuid = UuidToText(uuid);
      std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(at + 40), 32,
                  row.identity_sha256.begin());
      context->preliminary_diagnostic_identities.push_back(std::move(row));
    }
    if (extension_version >= 6 && extension_version <= 26) {
      const auto trailer = offset + 260 + static_cast<std::size_t>(row_count) * 72;
      const auto isolation_uuid = GetUuid(payload, trailer);
      const auto isolation_generation = GetU64(payload, trailer + 16);
      const auto policy_uuid = GetUuid(payload, trailer + 24);
      const auto policy_generation = GetU64(payload, trailer + 40);
      const auto executor_generation = GetU64(payload, trailer + 48);
      const auto read_mode = payload[trailer + 64];
      const auto authority_scope = payload[trailer + 65];
      const auto wait_policy = payload[trailer + 66];
      const auto deadline = GetU64(payload, trailer + 72);
      if (!UuidPresent(isolation_uuid) || isolation_generation == 0 ||
          !UuidPresent(policy_uuid) || policy_generation == 0 ||
          executor_generation == 0 || GetU64(payload, trailer + 56) != 0 ||
          read_mode < 1 || read_mode > 2 || authority_scope < 1 ||
          authority_scope > 2 || wait_policy < 1 || wait_policy > 2 ||
          std::any_of(payload.begin() + static_cast<std::ptrdiff_t>(trailer + 67),
                      payload.begin() + static_cast<std::ptrdiff_t>(trailer + 72),
                      [](std::uint8_t value) { return value != 0; })) {
        return false;
      }
      context->preliminary_transaction_isolation_profile_uuid = UuidToText(isolation_uuid);
      context->preliminary_transaction_isolation_profile_generation = isolation_generation;
      context->preliminary_transaction_policy_snapshot_uuid = UuidToText(policy_uuid);
      context->preliminary_transaction_policy_generation = policy_generation;
      context->preliminary_transaction_executor_availability_generation = executor_generation;
      context->preliminary_transaction_read_mode = read_mode;
      context->preliminary_transaction_authority_scope = authority_scope;
      context->preliminary_transaction_wait_policy = wait_policy;
      context->preliminary_transaction_deadline_monotonic_ns = deadline;
      if (extension_version >= 7 && extension_version <= 26) {
        const auto commit = trailer + 80;
        const auto commit_generation = GetU64(payload, commit);
        const auto commit_mode = payload[commit + 8];
        const auto commit_scope = payload[commit + 9];
        const auto commit_wait = payload[commit + 10];
        const auto commit_deadline = GetU64(payload, commit + 16);
        if (commit_generation == 0 || commit_mode != 1 ||
            commit_scope < 1 || commit_scope > 2 || commit_wait < 1 ||
            commit_wait > 2 ||
            std::any_of(payload.begin() + static_cast<std::ptrdiff_t>(commit + 11),
                        payload.begin() + static_cast<std::ptrdiff_t>(commit + 16),
                        [](std::uint8_t value) { return value != 0; })) return false;
        context->preliminary_transaction_commit_executor_availability_generation = commit_generation;
        context->preliminary_transaction_commit_mode = commit_mode;
        context->preliminary_transaction_commit_authority_scope = commit_scope;
        context->preliminary_transaction_commit_wait_policy = commit_wait;
        context->preliminary_transaction_commit_deadline_monotonic_ns = commit_deadline;
      }
      if (extension_version >= 8 && extension_version <= 26) {
        const auto rollback = trailer + 104;
        const auto rollback_generation = GetU64(payload, rollback);
        const auto rollback_mode = payload[rollback + 8];
        const auto rollback_scope = payload[rollback + 9];
        const auto rollback_wait = payload[rollback + 10];
        const auto rollback_deadline = GetU64(payload, rollback + 16);
        if (rollback_generation == 0 || rollback_mode != 1 ||
            rollback_scope < 1 || rollback_scope > 2 || rollback_wait < 1 ||
            rollback_wait > 2 ||
            std::any_of(payload.begin() + static_cast<std::ptrdiff_t>(rollback + 11),
                        payload.begin() + static_cast<std::ptrdiff_t>(rollback + 16),
                        [](std::uint8_t value) { return value != 0; })) return false;
        context->preliminary_transaction_rollback_executor_availability_generation =
            rollback_generation;
        context->preliminary_transaction_rollback_mode = rollback_mode;
        context->preliminary_transaction_rollback_authority_scope = rollback_scope;
        context->preliminary_transaction_rollback_wait_policy = rollback_wait;
        context->preliminary_transaction_rollback_deadline_monotonic_ns =
            rollback_deadline;
      }
      if (extension_version >= 9 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 128);
        if (generation == 0) return false;
        context->preliminary_transaction_release_savepoint_executor_availability_generation = generation;
      }
      if (extension_version >= 10 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 136);
        if (generation == 0) return false;
        context->preliminary_transaction_rollback_to_savepoint_executor_availability_generation = generation;
      }
      if (extension_version >= 11 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 144);
        if (generation == 0) return false;
        context->preliminary_psql_autonomous_frame_executor_availability_generation = generation;
      }
      if (extension_version >= 12 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 152);
        if (generation == 0) return false;
        context->preliminary_transaction_reservation_release_executor_availability_generation = generation;
      }
      if (extension_version >= 13 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 160);
        if (generation == 0) return false;
        context->preliminary_temporary_instance_cleanup_executor_availability_generation = generation;
      }
      if (extension_version >= 14 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 168);
        if (generation == 0) return false;
        context->preliminary_cursor_open_executor_availability_generation = generation;
      }
      if (extension_version >= 15 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 176);
        if (generation == 0) return false;
        context->preliminary_cursor_fetch_executor_availability_generation = generation;
      }
      if (extension_version >= 16 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 184);
        if (generation == 0) return false;
        context->preliminary_cursor_close_executor_availability_generation = generation;
      }
      if (extension_version >= 17 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 192);
        if (generation == 0) return false;
        context->preliminary_read_by_key_executor_availability_generation = generation;
      }
      if (extension_version >= 18 && extension_version <= 26) {
        const auto generation = GetU64(payload, trailer + 200);
        if (generation == 0) return false;
        context->preliminary_read_range_executor_availability_generation = generation;
      }
      if (extension_version >= 19 && extension_version <= 26) { const auto stream_generation = GetU64(payload, trailer + 208); if (stream_generation == 0) return false; context->preliminary_read_stream_executor_availability_generation = stream_generation; }
      if (extension_version >= 20 && extension_version <= 26) { const auto pass_generation = GetU64(payload, trailer + 216); if (pass_generation == 0) return false; context->preliminary_result_set_pass_executor_availability_generation = pass_generation; }
      if (extension_version >= 21 && extension_version <= 26) { const auto access_generation = GetU64(payload, trailer + 224); if (access_generation == 0) return false; context->preliminary_access_cursor_open_executor_availability_generation = access_generation; }
      if (extension_version >= 22 && extension_version <= 26) { const auto fetch_generation = GetU64(payload, trailer + 232); if (fetch_generation == 0) return false; context->preliminary_access_cursor_fetch_executor_availability_generation = fetch_generation; }
      if (extension_version >= 23 && extension_version <= 26) { const auto close_generation = GetU64(payload, trailer + 240); if (close_generation == 0) return false; context->preliminary_access_cursor_close_executor_availability_generation = close_generation; }
      if (extension_version >= 24 && extension_version <= 26) { const auto insert_generation = GetU64(payload, trailer + 248); if (insert_generation == 0) return false; context->preliminary_insert_executor_availability_generation = insert_generation; }
      if (extension_version >= 25 && extension_version <= 26) { const auto update_generation = GetU64(payload, trailer + 256); if (update_generation == 0) return false; context->preliminary_update_executor_availability_generation = update_generation; }
      if (extension_version == 26) { const auto delete_generation = GetU64(payload, trailer + 264); if (delete_generation == 0) return false; context->preliminary_delete_executor_availability_generation = delete_generation; }
      if (wire_extension_version == 27) { const auto merge_generation = GetU64(payload, trailer + 272); if (merge_generation == 0) return false; context->preliminary_merge_executor_availability_generation = merge_generation; }
      if (wire_extension_version == 28) { const auto merge_generation = GetU64(payload, trailer + 272); const auto truncate_generation = GetU64(payload, trailer + 280); if (merge_generation == 0 || truncate_generation == 0) return false; context->preliminary_merge_executor_availability_generation = merge_generation; context->preliminary_table_truncate_executor_availability_generation = truncate_generation; }
      if (wire_extension_version == 29) { const auto merge_generation = GetU64(payload, trailer + 272); const auto truncate_generation = GetU64(payload, trailer + 280); const auto analyze_generation = GetU64(payload, trailer + 288); if (merge_generation == 0 || truncate_generation == 0 || analyze_generation == 0) return false; context->preliminary_merge_executor_availability_generation = merge_generation; context->preliminary_table_truncate_executor_availability_generation = truncate_generation; context->preliminary_table_analyze_executor_availability_generation = analyze_generation; }
      if (wire_extension_version == 30) { const auto merge_generation = GetU64(payload, trailer + 272); const auto truncate_generation = GetU64(payload, trailer + 280); const auto analyze_generation = GetU64(payload, trailer + 288); const auto bulk_generation = GetU64(payload, trailer + 296); if (merge_generation == 0 || truncate_generation == 0 || analyze_generation == 0 || bulk_generation == 0) return false; context->preliminary_merge_executor_availability_generation = merge_generation; context->preliminary_table_truncate_executor_availability_generation = truncate_generation; context->preliminary_table_analyze_executor_availability_generation = analyze_generation; context->preliminary_bulk_import_stream_executor_availability_generation = bulk_generation; }
      if (wire_extension_version >= 31) { const auto merge_generation=GetU64(payload,trailer+272),truncate_generation=GetU64(payload,trailer+280),analyze_generation=GetU64(payload,trailer+288),import_generation=GetU64(payload,trailer+296),export_generation=GetU64(payload,trailer+304); if(!merge_generation||!truncate_generation||!analyze_generation||!import_generation||!export_generation)return false;context->preliminary_merge_executor_availability_generation=merge_generation;context->preliminary_table_truncate_executor_availability_generation=truncate_generation;context->preliminary_table_analyze_executor_availability_generation=analyze_generation;context->preliminary_bulk_import_stream_executor_availability_generation=import_generation;context->preliminary_bulk_export_stream_executor_availability_generation=export_generation; }
      if (wire_extension_version >= 32) { const auto batch_generation=GetU64(payload,trailer+312); if(!batch_generation)return false; context->preliminary_statement_batch_executor_availability_generation=batch_generation; }
      if (wire_extension_version >= 33) { const auto cas_generation=GetU64(payload,trailer+320); if(!cas_generation)return false; context->preliminary_atomic_cas_executor_availability_generation=cas_generation; }
      if (wire_extension_version >= 34) { const auto rmw_generation=GetU64(payload,trailer+328); if(!rmw_generation)return false; context->preliminary_atomic_rmw_executor_availability_generation=rmw_generation; }
      if (wire_extension_version >= 35) { const auto lock_generation=GetU64(payload,trailer+336); if(!lock_generation)return false; context->preliminary_advisory_lock_acquire_executor_availability_generation=lock_generation; }
      if (wire_extension_version >= 36) { const auto release_generation=GetU64(payload,trailer+344); if(!release_generation)return false; context->preliminary_advisory_lock_release_executor_availability_generation=release_generation; }
      if (wire_extension_version >= 37) { const auto function_generation=GetU64(payload,trailer+352); if(!function_generation)return false; context->preliminary_function_call_executor_availability_generation=function_generation; }
      if (wire_extension_version >= 38) { const auto operator_generation=GetU64(payload,trailer+360); if(!operator_generation)return false; context->preliminary_operator_call_executor_availability_generation=operator_generation; }
      if (wire_extension_version >= 39) { const auto cast_generation=GetU64(payload,trailer+368); if(!cast_generation)return false; context->preliminary_cast_executor_availability_generation=cast_generation; }
      if (wire_extension_version >= 40) { const auto compare_generation=GetU64(payload,trailer+376); if(!compare_generation)return false; context->preliminary_compare_executor_availability_generation=compare_generation; }
      if (wire_extension_version >= 41) { const auto domain_generation=GetU64(payload,trailer+384); if(!domain_generation)return false; context->preliminary_domain_operation_executor_availability_generation=domain_generation; }
      if (wire_extension_version >= 42) { const auto udr_generation=GetU64(payload,trailer+392); if(!udr_generation)return false; context->preliminary_udr_invoke_executor_availability_generation=udr_generation; }
      if (wire_extension_version >= 43) { const auto procedure_generation=GetU64(payload,trailer+400); if(!procedure_generation)return false; context->preliminary_procedure_invoke_executor_availability_generation=procedure_generation; }
      if (wire_extension_version >= 44) { const auto function_generation=GetU64(payload,trailer+408); if(!function_generation)return false; context->preliminary_function_invoke_executor_availability_generation=function_generation; }
      if (wire_extension_version >= 45) { const auto aggregate_generation=GetU64(payload,trailer+416); if(!aggregate_generation)return false; context->preliminary_aggregate_invoke_executor_availability_generation=aggregate_generation; }
      if (wire_extension_version >= 46) { const auto nextval_generation=GetU64(payload,trailer+424); if(!nextval_generation)return false; context->preliminary_sequence_nextval_executor_availability_generation=nextval_generation; }
      if (wire_extension_version >= 47) { const auto currval_generation=GetU64(payload,trailer+432); if(!currval_generation)return false; context->preliminary_sequence_currval_executor_availability_generation=currval_generation; }
      if (wire_extension_version >= 48) { const auto setval_generation=GetU64(payload,trailer+440); if(!setval_generation)return false; context->preliminary_sequence_setval_executor_availability_generation=setval_generation; }
      if (wire_extension_version >= 49) { const auto numeric_generation=GetU64(payload,trailer+448); if(!numeric_generation)return false; context->preliminary_query_numeric_executor_availability_generation=numeric_generation; }
      if (wire_extension_version >= 50) { const auto family_generation=GetU64(payload,trailer+456); if(!family_generation)return false; context->preliminary_advanced_datatype_family_executor_availability_generation=family_generation; }
      if (wire_extension_version >= 51) { const auto project_generation=GetU64(payload,trailer+464); if(!project_generation)return false; context->preliminary_project_executor_availability_generation=project_generation; }
      if (wire_extension_version >= 52) { const auto aggregate_generation=GetU64(payload,trailer+472); if(!aggregate_generation)return false; context->preliminary_aggregate_executor_availability_generation=aggregate_generation; }
      if (wire_extension_version >= 53) { const auto group_generation=GetU64(payload,trailer+480); if(!group_generation)return false; context->preliminary_group_executor_availability_generation=group_generation; }
      if (wire_extension_version >= 54) { const auto sort_generation=GetU64(payload,trailer+488); if(!sort_generation)return false; context->preliminary_sort_executor_availability_generation=sort_generation; }
      if (wire_extension_version >= 55) { const auto limit_generation=GetU64(payload,trailer+496); if(!limit_generation)return false; context->preliminary_limit_executor_availability_generation=limit_generation; }
      if (wire_extension_version >= 56) { const auto window_generation=GetU64(payload,trailer+504); if(!window_generation)return false; context->preliminary_window_executor_availability_generation=window_generation; }
      if (wire_extension_version >= 57) { const auto return_generation=GetU64(payload,trailer+512); if(!return_generation)return false; context->preliminary_return_result_set_executor_availability_generation=return_generation; }
      if (wire_extension_version >= 58) { const auto kv_generation=GetU64(payload,trailer+520); if(!kv_generation)return false; context->preliminary_kv_structured_read_executor_availability_generation=kv_generation; } if (wire_extension_version >= 59) { const auto kv_generation=GetU64(payload,trailer+528); if(!kv_generation)return false; context->preliminary_kv_structured_mutate_executor_availability_generation=kv_generation; } else if (wire_extension_version >= 58) context->preliminary_kv_structured_mutate_executor_availability_generation = context->preliminary_kv_structured_read_executor_availability_generation;
      context->preliminary_kv_structured_scan_executor_availability_generation = context->preliminary_kv_structured_mutate_executor_availability_generation ? context->preliminary_kv_structured_mutate_executor_availability_generation : context->preliminary_kv_structured_read_executor_availability_generation;
      context->preliminary_kv_structured_stream_read_executor_availability_generation = context->preliminary_kv_structured_scan_executor_availability_generation;
      context->preliminary_kv_structured_stream_append_executor_availability_generation = context->preliminary_kv_structured_stream_read_executor_availability_generation;
      context->preliminary_kv_structured_timeseries_executor_availability_generation = context->preliminary_kv_structured_stream_append_executor_availability_generation;
      context->preliminary_system_config_set_executor_availability_generation = context->preliminary_kv_structured_timeseries_executor_availability_generation;
      context->preliminary_ddl_create_domain_executor_availability_generation = context->preliminary_system_config_set_executor_availability_generation;
      context->preliminary_ddl_create_schema_executor_availability_generation = context->preliminary_ddl_create_domain_executor_availability_generation;
      context->preliminary_ddl_create_table_executor_availability_generation = context->preliminary_ddl_create_schema_executor_availability_generation;
      context->preliminary_ddl_create_index_executor_availability_generation = context->preliminary_ddl_create_table_executor_availability_generation;
      context->preliminary_ddl_drop_index_executor_availability_generation = context->preliminary_ddl_create_index_executor_availability_generation;
    }
  }
  context->literal_preliminary_receipt_uuid =
      UuidToText(preliminary_receipt_uuid);
  context->literal_catalog_snapshot_uuid = UuidToText(catalog_snapshot_uuid);
  context->literal_catalog_generation = catalog_generation;
  context->literal_security_epoch = security_epoch;
  context->literal_resource_epoch = resource_epoch;
  context->literal_mga_snapshot_uuid = UuidToText(mga_snapshot_uuid);
  return true;
}

bool IsCanonicalNonzeroUuidText(std::string_view text) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return false;
  }
  const auto parsed = TextToUuid(text);
  if (!UuidPresent(parsed)) return false;
  std::string normalized(text);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return UuidToText(parsed) == normalized;
}

bool IsErrorFrame(const Frame& frame) {
  return frame.header.message_type == kMessageDiagnostic || (frame.header.flags & kFlagError) != 0;
}

void AddDiagnostic(MessageVectorSet* messages,
                   std::string code,
                   std::string message,
                   std::string component,
                   std::vector<Field> fields) {
  if (messages == nullptr) return;
  messages->diagnostics.push_back(MakeDiagnostic(std::move(code),
                                                 "ERROR",
                                                 std::move(message),
                                                 std::move(component),
                                                 std::move(fields)));
}

struct DecodedMessageVector {
  std::string code;
  std::string message;
  std::vector<Field> fields;
};

std::optional<std::vector<DecodedMessageVector>> DecodeMessageVectors(
    const std::vector<std::uint8_t>& payload,
    const std::array<std::uint8_t, 16>& expected_request_uuid) {
  constexpr std::size_t kHeaderBytes = 64;
  constexpr std::size_t kRecordHeaderBytes = 112;
  constexpr std::size_t kMaximumSetBytes = 1024u * 1024u;
  constexpr std::size_t kMaximumRecordBytes = 256u * 1024u;
  std::vector<DecodedMessageVector> vectors;
  if (payload.size() < kHeaderBytes || payload.size() > kMaximumSetBytes ||
      GetU32(payload, 0) != kMessageVectorMagic ||
      GetU16(payload, 4) != kHeaderBytes || GetU16(payload, 6) != 1 ||
      (GetU32(payload, 8) & 0xfffffff0u) != 0 ||
      GetU32(payload, 16) != payload.size()) {
    return std::nullopt;
  }
  const auto vector_count = GetU32(payload, 12);
  if (vector_count > 1024u) return std::nullopt;
  for (std::size_t index = 56; index < kHeaderBytes; ++index) {
    if (payload[index] != 0) return std::nullopt;
  }
  auto header = std::vector<std::uint8_t>(
      payload.begin(),
      payload.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
  const auto expected_header_crc = GetU32(header, 52);
  for (std::size_t index = 52; index < 56; ++index) header[index] = 0;
  if (Crc32c(header.data(), header.size()) != expected_header_crc) {
    return std::nullopt;
  }
  const auto records_crc = GetU32(payload, 20);
  if (vector_count == 0) {
    if (records_crc != 0 || payload.size() != kHeaderBytes) {
      return std::nullopt;
    }
  } else {
    if (records_crc == 0 ||
        Crc32c(payload.data() + kHeaderBytes,
               payload.size() - kHeaderBytes) != records_crc) {
      return std::nullopt;
    }
  }

  std::size_t offset = kHeaderBytes;
  for (std::uint32_t index = 0; index < vector_count; ++index) {
    if (offset + kRecordHeaderBytes > payload.size()) return std::nullopt;
    const auto record_start = offset;
    const auto record_bytes = GetU32(payload, record_start);
    if (record_bytes < kRecordHeaderBytes ||
        record_bytes > kMaximumRecordBytes ||
        record_bytes > payload.size() - record_start) {
      return std::nullopt;
    }
    const auto record_end = record_start + record_bytes;
    auto record = std::vector<std::uint8_t>(
        payload.begin() + static_cast<std::ptrdiff_t>(record_start),
        payload.begin() + static_cast<std::ptrdiff_t>(record_end));
    const auto expected_record_crc = GetU32(record, 4);
    for (std::size_t byte = 4; byte < 8; ++byte) record[byte] = 0;
    if (Crc32c(record.data(), record.size()) != expected_record_crc ||
        GetU16(payload, record_start + 8) != 1 ||
        payload[record_start + 10] > 7 ||
        payload[record_start + 11] > 6 ||
        GetU32(payload, record_start + 12) != 0 ||
        !std::equal(expected_request_uuid.begin(),
                    expected_request_uuid.end(),
                    payload.begin() +
                        static_cast<std::ptrdiff_t>(record_start + 48)) ||
        payload[record_start + 108] > 2 ||
        payload[record_start + 109] > 3 ||
        GetU16(payload, record_start + 110) != 0) {
      return std::nullopt;
    }
    const auto language_len = GetU16(payload, offset + 92);
    const auto code_len = GetU16(payload, offset + 94);
    const auto message_key_len = GetU16(payload, offset + 96);
    const auto admin_detail_key_len = GetU16(payload, offset + 98);
    const auto safe_message_len = GetU16(payload, offset + 100);
    const auto field_count = GetU16(payload, offset + 102);
    const auto detail_count = GetU16(payload, offset + 104);
    const auto cause_count = GetU16(payload, offset + 106);
    if (field_count > 64 || detail_count > 64 || cause_count > 16) {
      return std::nullopt;
    }

    std::size_t cursor = record_start + kRecordHeaderBytes;
    auto read_padded_string = [&](std::uint16_t length,
                                  std::string* value) {
      if (value == nullptr || length > record_end - cursor) return false;
      value->assign(reinterpret_cast<const char*>(payload.data() + cursor),
                    length);
      cursor += length;
      while ((cursor % 4u) != 0u) {
        if (cursor >= record_end || payload[cursor++] != 0) return false;
      }
      return true;
    };
    std::string language;
    std::string message_key;
    std::string admin_detail_key;
    DecodedMessageVector vector;
    if (!read_padded_string(language_len, &language) ||
        !read_padded_string(code_len, &vector.code) ||
        !read_padded_string(message_key_len, &message_key) ||
        !read_padded_string(admin_detail_key_len, &admin_detail_key) ||
        !read_padded_string(safe_message_len, &vector.message)) {
      return std::nullopt;
    }

    auto read_tlv = [&](Field* field) {
      if (field == nullptr || cursor + 8 > record_end) return false;
      const auto key_len = GetU16(payload, cursor);
      const auto type_code = GetU16(payload, cursor + 2);
      const auto value_len = GetU32(payload, cursor + 4);
      cursor += 8;
      if (key_len == 0 || type_code != 1 ||
          key_len > record_end - cursor ||
          value_len > record_end - cursor - key_len) {
        return false;
      }
      field->name.assign(
          reinterpret_cast<const char*>(payload.data() + cursor), key_len);
      cursor += key_len;
      field->value.assign(
          reinterpret_cast<const char*>(payload.data() + cursor), value_len);
      cursor += value_len;
      while ((cursor % 4u) != 0u) {
        if (cursor >= record_end || payload[cursor++] != 0) return false;
      }
      return IsPublicDiagnosticFieldAllowed(field->name, field->value);
    };
    for (std::uint16_t field_index = 0; field_index < field_count;
         ++field_index) {
      Field field;
      if (!read_tlv(&field)) return std::nullopt;
      vector.fields.push_back(std::move(field));
    }
    for (std::uint32_t ignored = 0;
         ignored < static_cast<std::uint32_t>(detail_count) + cause_count;
         ++ignored) {
      Field field;
      if (!read_tlv(&field)) return std::nullopt;
    }
    if (cursor != record_end) return std::nullopt;
    vectors.push_back(std::move(vector));
    offset = record_end;
  }
  if (offset != payload.size() || vectors.size() != vector_count) {
    return std::nullopt;
  }
  return vectors;
}

bool AppendTypedExecuteDiagnostics(
    const std::vector<std::uint8_t>& encoded_diagnostics,
    std::string_view expected_first_code,
    const std::array<std::uint8_t, 16>& expected_request_uuid,
    MessageVectorSet* messages) {
  auto decoded = DecodeMessageVectors(encoded_diagnostics,
                                      expected_request_uuid);
  if (!decoded || decoded->empty() ||
      (!expected_first_code.empty() &&
       decoded->front().code != expected_first_code)) {
    return false;
  }
  for (auto& vector : *decoded) {
    AddDiagnostic(messages,
                  std::move(vector.code),
                  vector.message.empty()
                      ? "The server returned a typed execution diagnostic."
                      : std::move(vector.message),
                  "parser_server_ipc.sbps_client",
                  std::move(vector.fields));
  }
  return true;
}

void AddFrameDiagnostics(const Frame& frame, MessageVectorSet* messages) {
  auto decoded = DecodeMessageVectors(frame.payload,
                                      frame.header.request_uuid);
  if (!decoded || decoded->empty()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.REQUEST_FAILED", "The parser-server IPC request failed.");
    return;
  }
  for (auto& vector : *decoded) {
    AddDiagnostic(messages,
                  std::move(vector.code),
                  vector.message.empty() ? "The server returned a message vector for this request."
                                         : std::move(vector.message),
                  "parser_server_ipc.sbps_client",
                  std::move(vector.fields));
  }
}

std::vector<std::uint8_t> EncodeFrame(const FrameHeader& input,
                                      const std::vector<std::uint8_t>& payload) {
  FrameHeader header = input;
  header.payload_len = static_cast<std::uint32_t>(payload.size());
  const auto payload_crc = payload.empty() ? 0 : Crc32c(payload.data(), payload.size());
  std::vector<std::uint8_t> out;
  out.reserve(kHeaderBytes + payload.size());
  PutU32(&out, kFrameMagic);
  PutU16(&out, kHeaderBytes);
  PutU16(&out, kProtocolMajor);
  PutU16(&out, kProtocolMinor);
  PutU16(&out, header.message_type);
  PutU32(&out, header.flags);
  PutU32(&out, header.schema_id);
  PutU32(&out, header.payload_len);
  PutU32(&out, 0);
  PutU32(&out, payload_crc);
  PutU64(&out, header.stream_id);
  PutU64(&out, header.sequence_number);
  PutUuid(&out, header.request_uuid);
  PutUuid(&out, header.connection_uuid);
  PutUuid(&out, header.session_uuid);
  const auto header_crc = Crc32c(out.data(), out.size());
  PutAtU32(&out, 24, header_crc);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<std::vector<std::uint8_t>> EncodeFrameSequence(
    const FrameHeader& input,
    const std::vector<std::uint8_t>& payload) {
  if (payload.size() <= kMaxFramePayload) {
    return {EncodeFrame(input, payload)};
  }

  std::vector<std::vector<std::uint8_t>> frames;
  frames.reserve((payload.size() + kMaxFramePayload - 1) / kMaxFramePayload);
  const auto stream_id = input.stream_id == 0 ? 1 : input.stream_id;
  auto sequence_number = input.sequence_number == 0 ? 1 : input.sequence_number;
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const auto chunk_size = std::min<std::size_t>(kMaxFramePayload, payload.size() - offset);
    std::vector<std::uint8_t> chunk(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                    payload.begin() + static_cast<std::ptrdiff_t>(offset + chunk_size));
    FrameHeader header = input;
    header.stream_id = stream_id;
    header.sequence_number = sequence_number++;
    header.flags = input.flags | kFlagPayloadChunk;
    if (offset + chunk_size >= payload.size()) {
      header.flags |= kFlagFinal;
    } else {
      header.flags &= ~kFlagFinal;
    }
    frames.push_back(EncodeFrame(header, chunk));
    offset += chunk_size;
  }
  return frames;
}

bool DecodeFrame(const std::vector<std::uint8_t>& bytes, Frame* frame, MessageVectorSet* messages) {
  if (bytes.size() < kHeaderBytes) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID", "The SBPS response header is incomplete.");
    return false;
  }
  if (GetU32(bytes, 0) != kFrameMagic || GetU16(bytes, 4) != kHeaderBytes) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_HEADER_INVALID", "The SBPS response header is invalid.");
    return false;
  }
  if (GetU16(bytes, 6) != kProtocolMajor) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.PROTOCOL_VERSION_UNSUPPORTED", "The SBPS protocol version is unsupported.");
    return false;
  }
  const auto payload_len = GetU32(bytes, 20);
  if (payload_len > kMaxFramePayload || bytes.size() != kHeaderBytes + payload_len) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID", "The SBPS response frame length is invalid.");
    return false;
  }
  auto header_for_crc = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + kHeaderBytes);
  PutAtU32(&header_for_crc, 24, 0);
  if (Crc32c(header_for_crc.data(), header_for_crc.size()) != GetU32(bytes, 24)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_HEADER_CRC_INVALID", "The SBPS response header CRC is invalid.");
    return false;
  }
  if (payload_len != 0 &&
      Crc32c(bytes.data() + kHeaderBytes, payload_len) != GetU32(bytes, 28)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_PAYLOAD_CRC_INVALID", "The SBPS response payload CRC is invalid.");
    return false;
  }
  frame->header.message_type = GetU16(bytes, 10);
  frame->header.flags = GetU32(bytes, 12);
  frame->header.schema_id = GetU32(bytes, 16);
  frame->header.payload_len = payload_len;
  frame->header.stream_id = GetU64(bytes, 32);
  frame->header.sequence_number = GetU64(bytes, 40);
  frame->header.request_uuid = GetUuid(bytes, 48);
  frame->header.connection_uuid = GetUuid(bytes, 64);
  frame->header.session_uuid = GetUuid(bytes, 80);
  frame->payload.assign(bytes.begin() + kHeaderBytes, bytes.end());
  return true;
}

bool CompatibleChunk(const Frame& first, const Frame& next, std::uint64_t expected_sequence) {
  return (next.header.flags & kFlagPayloadChunk) != 0 &&
         next.header.message_type == first.header.message_type &&
         next.header.schema_id == first.header.schema_id &&
         next.header.stream_id == first.header.stream_id &&
         next.header.sequence_number == expected_sequence &&
         next.header.request_uuid == first.header.request_uuid &&
         next.header.connection_uuid == first.header.connection_uuid &&
         next.header.session_uuid == first.header.session_uuid;
}

std::string EndpointPath(std::string endpoint) {
  constexpr std::string_view unix_prefix = "unix:";
  if (endpoint.starts_with(unix_prefix)) endpoint.erase(0, unix_prefix.size());
  return endpoint;
}

bool ValidateEndpointPath(std::string_view path, MessageVectorSet* messages) {
  if (path.empty()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ENDPOINT_MISSING", "No parser-server IPC endpoint was assigned.");
    return false;
  }
  if (path.size() >= kPortableAfUnixPathLimit) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ENDPOINT_PATH_TOO_LONG", "The parser-server IPC endpoint path is too long.");
    return false;
  }
  return true;
}

#ifdef _WIN32
using SbpsSocketHandle = SOCKET;
constexpr SbpsSocketHandle kInvalidSbpsSocket = INVALID_SOCKET;

bool EnsureWinsockInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}

bool SbpsSocketInterrupted() {
  return ::WSAGetLastError() == WSAEINTR;
}

void CloseSbpsSocket(SbpsSocketHandle fd) {
  if (fd != kInvalidSbpsSocket) {
    ::closesocket(fd);
  }
}

int WriteSocketBytes(SbpsSocketHandle fd, const std::uint8_t* data, std::size_t size) {
  const auto chunk = static_cast<int>(
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return ::send(fd, reinterpret_cast<const char*>(data), chunk, 0);
}

int ReadSocketBytes(SbpsSocketHandle fd, std::uint8_t* data, std::size_t size) {
  const auto chunk = static_cast<int>(
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return ::recv(fd, reinterpret_cast<char*>(data), chunk, 0);
}

bool SetSocketTimeouts(SbpsSocketHandle fd, std::uint32_t timeout_ms) {
  const DWORD timeout = timeout_ms;
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
}
#else
using SbpsSocketHandle = int;
constexpr SbpsSocketHandle kInvalidSbpsSocket = -1;

bool SbpsSocketInterrupted() {
  return errno == EINTR;
}

void CloseSbpsSocket(SbpsSocketHandle fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

int WriteSocketBytes(SbpsSocketHandle fd, const std::uint8_t* data, std::size_t size) {
  const auto chunk =
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#ifdef MSG_NOSIGNAL
  return static_cast<int>(::send(fd, data, chunk, MSG_NOSIGNAL));
#else
  return static_cast<int>(::send(fd, data, chunk, 0));
#endif
}

int ReadSocketBytes(SbpsSocketHandle fd, std::uint8_t* data, std::size_t size) {
  const auto chunk =
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
  return static_cast<int>(::read(fd, data, chunk));
}

bool SetSocketTimeouts(SbpsSocketHandle fd, std::uint32_t timeout_ms) {
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}
#endif

class Fd {
 public:
  explicit Fd(SbpsSocketHandle fd = kInvalidSbpsSocket) : fd_(fd) {}
  ~Fd() { CloseSbpsSocket(fd_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  SbpsSocketHandle get() const { return fd_; }
  bool valid() const { return fd_ != kInvalidSbpsSocket; }
 private:
  SbpsSocketHandle fd_;
};

bool WriteAll(SbpsSocketHandle fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto rc = WriteSocketBytes(fd, bytes.data() + offset, bytes.size() - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && SbpsSocketInterrupted()) continue;
    return false;
  }
  return true;
}

bool ReadExact(SbpsSocketHandle fd, std::vector<std::uint8_t>* out, std::size_t bytes) {
  out->assign(bytes, 0);
  std::size_t offset = 0;
  while (offset < bytes) {
    const auto rc = ReadSocketBytes(fd, out->data() + offset, bytes - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && SbpsSocketInterrupted()) continue;
    return false;
  }
  return true;
}

bool ReadPhysicalFrame(SbpsSocketHandle fd, Frame* frame, MessageVectorSet* messages) {
  std::vector<std::uint8_t> header;
  if (!ReadExact(fd, &header, kHeaderBytes)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.READ_FAILED", "The parser could not read the SBPS response header.");
    return false;
  }
  const auto payload_len = GetU32(header, 20);
  if (payload_len > kMaxFramePayload) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID", "The SBPS response frame exceeds the negotiated physical frame limit.");
    return false;
  }
  std::vector<std::uint8_t> payload;
  if (payload_len > 0 && !ReadExact(fd, &payload, payload_len)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.READ_FAILED", "The parser could not read the SBPS response payload.");
    return false;
  }
  header.insert(header.end(), payload.begin(), payload.end());
  return DecodeFrame(header, frame, messages);
}

bool AssembleChunkedFrame(SbpsSocketHandle fd, Frame* frame, MessageVectorSet* messages) {
  if ((frame->header.flags & kFlagPayloadChunk) == 0) return true;
  if (frame->header.stream_id == 0 || frame->header.sequence_number == 0) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID", "The SBPS chunk sequence header is invalid.");
    return false;
  }

  Frame assembled = *frame;
  std::vector<std::uint8_t> payload = frame->payload;
  std::uint64_t expected_sequence = frame->header.sequence_number + 1;
  while ((assembled.header.flags & kFlagFinal) == 0) {
    Frame next;
    if (!ReadPhysicalFrame(fd, &next, messages)) return false;
    if (!CompatibleChunk(*frame, next, expected_sequence)) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID", "The SBPS chunk sequence is not contiguous.");
      return false;
    }
    if (payload.size() + next.payload.size() > kMaxChunkedPayload) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.PAYLOAD_TOO_LARGE", "The assembled SBPS payload exceeds the protocol limit.");
      return false;
    }
    payload.insert(payload.end(), next.payload.begin(), next.payload.end());
    assembled = next;
    ++expected_sequence;
  }
  frame->payload = std::move(payload);
  frame->header.payload_len = static_cast<std::uint32_t>(frame->payload.size());
  frame->header.flags = (assembled.header.flags & ~kFlagPayloadChunk) | kFlagFinal;
  return true;
}

std::mutex& CachedSbpsSocketMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, SbpsSocketHandle>& CachedSbpsSockets() {
  static std::map<std::string, SbpsSocketHandle> sockets;
  return sockets;
}

void AppendDiagnostics(MessageVectorSet* target, const MessageVectorSet& source) {
  if (target == nullptr || source.diagnostics.empty()) return;
  target->diagnostics.insert(target->diagnostics.end(),
                             source.diagnostics.begin(),
                             source.diagnostics.end());
}

std::string JoinStable(const std::vector<std::string>& values) {
  std::vector<std::string> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  std::string out;
  for (const auto& value : sorted) {
    if (!out.empty()) out.push_back(',');
    out += value;
  }
  return out;
}

bool ExecutionInvalidatesPublicResolutionCache(std::string_view operation_id) {
  return operation_id.rfind("ddl.", 0) == 0 ||
         operation_id.rfind("catalog.", 0) == 0 ||
         operation_id.rfind("security.", 0) == 0 ||
         operation_id.rfind("language.", 0) == 0 ||
         operation_id.rfind("policy.", 0) == 0 ||
         operation_id.rfind("auth.", 0) == 0;
}

struct SbpsClientPublicResolutionCacheRecord {
  std::string object_uuid;
  std::string canonical_name;
  std::string object_class;
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_epoch{0};
};

bool IsPublicResourceObjectClass(std::string_view object_class) {
  std::string normalized(object_class);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return normalized == "charset" || normalized == "collation";
}

std::mutex& SbpsClientPublicResolutionCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, SbpsClientPublicResolutionCacheRecord>&
SbpsClientPublicResolutionCache() {
  static std::map<std::string, SbpsClientPublicResolutionCacheRecord> cache;
  return cache;
}

std::deque<std::string>& SbpsClientPublicResolutionLru() {
  static std::deque<std::string> lru;
  return lru;
}

std::string SbpsClientPublicResolutionScopeKey(std::string_view endpoint,
                                               const ParserSessionContext& session) {
  std::ostringstream key;
  key << "endpoint=" << endpoint
      << "|session=" << session.session_uuid
      << "|connection=" << session.connection_uuid
      << "|database=" << session.database_uuid
      << "|user=" << session.authenticated_user_uuid
      << "|principal=" << session.principal_claim
      << "|auth_provider=" << session.auth_provider_family
      << "|catalog=" << session.catalog_epoch
      << "|security=" << session.security_policy_epoch
      << "|grant=" << session.grant_epoch
      << "|descriptor=" << session.descriptor_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|language_resource=" << session.language_resource_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|roles=" << JoinStable(session.effective_role_uuids)
      << "|groups=" << JoinStable(session.effective_group_uuids)
      << "|search_path=" << JoinStable(session.search_path)
      << "|default_language=" << session.default_language
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|dialect_profile=" << session.dialect_profile_uuid
      << "|policy_profile=" << session.policy_profile_uuid
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

std::string SbpsClientResolveNameCacheKey(std::string_view endpoint,
                                          const ParserSessionContext& session,
                                          std::string_view presented_name,
                                          bool quoted,
                                          std::string_view object_class,
                                          const ParserClientConfig& config) {
  std::ostringstream key;
  key << SbpsClientPublicResolutionScopeKey(endpoint, session)
      << "|kind=resolve_name"
      << "|name=" << presented_name
      << "|quoted=" << (quoted ? "1" : "0")
      << "|object_class=" << object_class
      << "|parser_profile=" << config.profile_id
      << "|parser_dialect=" << config.dialect
      << "|registry=" << config.registry_version;
  return key.str();
}

std::string SbpsClientRenderUuidCacheKey(std::string_view endpoint,
                                         const ParserSessionContext& session,
                                         std::string_view object_uuid) {
  std::ostringstream key;
  key << SbpsClientPublicResolutionScopeKey(endpoint, session)
      << "|kind=render_uuid"
      << "|object_uuid=" << object_uuid;
  return key.str();
}

PublicNameResolutionResult PublicResolutionResultFromCache(
    const SbpsClientPublicResolutionCacheRecord& cached) {
  PublicNameResolutionResult result;
  result.resolved = true;
  result.object_uuid = cached.object_uuid;
  result.canonical_name = cached.canonical_name;
  result.object_class = cached.object_class;
  result.catalog_epoch = cached.catalog_epoch;
  result.security_epoch = cached.security_epoch;
  return result;
}

std::optional<SbpsClientPublicResolutionCacheRecord>
LookupSbpsClientPublicResolutionCache(const std::string& cache_key) {
  std::lock_guard<std::mutex> guard(SbpsClientPublicResolutionCacheMutex());
  const auto found = SbpsClientPublicResolutionCache().find(cache_key);
  if (found == SbpsClientPublicResolutionCache().end()) return std::nullopt;
  return found->second;
}

void StoreSbpsClientPublicResolutionCacheEntry(
    const std::string& cache_key,
    const PublicNameResolutionResult& result) {
  if (cache_key.empty() || !result.resolved || result.object_uuid.empty() ||
      IsPublicResourceObjectClass(result.object_class)) {
    return;
  }
  std::lock_guard<std::mutex> guard(SbpsClientPublicResolutionCacheMutex());
  auto& cache = SbpsClientPublicResolutionCache();
  auto& lru = SbpsClientPublicResolutionLru();
  cache[cache_key] = SbpsClientPublicResolutionCacheRecord{
      result.object_uuid,
      result.canonical_name,
      result.object_class,
      result.catalog_epoch,
      result.security_epoch,
  };
  lru.erase(std::remove(lru.begin(), lru.end(), cache_key), lru.end());
  lru.push_back(cache_key);
  while (cache.size() > kMaxSbpsClientPublicResolutionCacheEntries && !lru.empty()) {
    cache.erase(lru.front());
    lru.pop_front();
  }
}

void ClearSbpsClientPublicResolutionCacheForSession(std::string_view endpoint,
                                                    const ParserSessionContext& session) {
  const std::string scope = SbpsClientPublicResolutionScopeKey(endpoint, session);
  std::lock_guard<std::mutex> guard(SbpsClientPublicResolutionCacheMutex());
  auto& cache = SbpsClientPublicResolutionCache();
  auto& lru = SbpsClientPublicResolutionLru();
  for (auto it = cache.begin(); it != cache.end();) {
    if (it->first.rfind(scope, 0) == 0) {
      it = cache.erase(it);
    } else {
      ++it;
    }
  }
  lru.erase(std::remove_if(lru.begin(), lru.end(), [&](const std::string& key) {
              return key.rfind(scope, 0) == 0;
            }),
            lru.end());
}

void CloseCachedSbpsSocket(std::string_view cache_key) {
  auto& sockets = CachedSbpsSockets();
  const auto found = sockets.find(std::string(cache_key));
  if (found == sockets.end()) return;
  CloseSbpsSocket(found->second);
  sockets.erase(found);
}

SbpsSocketHandle ConnectCachedSbpsSocket(std::string_view path,
                                         std::string_view cache_key,
                                         bool allow_fresh_connection,
                                         MessageVectorSet* messages,
                                         std::uint32_t timeout_ms) {
  auto& sockets = CachedSbpsSockets();
  const auto key = std::string(cache_key);
  if (const auto found = sockets.find(key);
      found != sockets.end() && found->second != kInvalidSbpsSocket) {
    return found->second;
  }
  // Session UUIDs are meaningful only on the physical route that negotiated
  // and authenticated them.  A caller with a session-bound frame must never
  // turn a missing cached route into a raw, unnegotiated replacement socket.
  if (!allow_fresh_connection) return kInvalidSbpsSocket;
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SOCKET_CREATE_FAILED", "Winsock initialization failed for the SBPS client.");
    return kInvalidSbpsSocket;
  }
#endif
  const SbpsSocketHandle fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == kInvalidSbpsSocket) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SOCKET_CREATE_FAILED", "The parser could not create an SBPS socket.");
    return kInvalidSbpsSocket;
  }
  (void)SetSocketTimeouts(fd, timeout_ms);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    CloseSbpsSocket(fd);
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ENDPOINT_PATH_TOO_LONG", "The parser-server IPC endpoint path is too long.");
    return kInvalidSbpsSocket;
  }
  std::memcpy(addr.sun_path, path.data(), path.size());
  addr.sun_path[path.size()] = '\0';
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSbpsSocket(fd);
    AddDiagnostic(messages, "PARSER_SERVER_IPC.CONNECT_FAILED", "The parser could not connect to sb_server.");
    return kInvalidSbpsSocket;
  }
  sockets[key] = fd;
  return fd;
}

bool ReadExpectedResponse(SbpsSocketHandle fd,
                          const std::array<std::uint8_t, 16>& request_uuid,
                          Frame* response,
                          MessageVectorSet* messages) {
  constexpr int kMaxStaleFrames = 64;
  for (int index = 0; index < kMaxStaleFrames; ++index) {
    Frame candidate;
    if (!ReadPhysicalFrame(fd, &candidate, messages)) return false;
    if (!AssembleChunkedFrame(fd, &candidate, messages)) return false;
    if (candidate.header.request_uuid == request_uuid) {
      *response = std::move(candidate);
      return true;
    }
  }
  AddDiagnostic(messages,
                "PARSER_SERVER_IPC.STALE_RESPONSE_LIMIT",
                "The parser-server IPC response stream did not produce the requested response before the stale-frame limit.");
  return false;
}

bool V2RequestIsNonReplayableAfterWrite(std::uint32_t schema_id) {
  return schema_id == kSchemaPrepareSblrV2 ||
         schema_id == kSchemaExecuteSblrV2 ||
         schema_id == kSchemaExecuteCanonicalSblrV1 ||
         schema_id == kSchemaResolveNameRequestV2 ||
         schema_id == kSchemaResolveNameRequestV3;
}

bool SessionBoundRequest(const FrameHeader& header) {
  return UuidPresent(header.session_uuid);
}

bool RequestMayRetryAfterTransportLoss(const FrameHeader& header) {
  return !SessionBoundRequest(header) &&
         !V2RequestIsNonReplayableAfterWrite(header.schema_id);
}

void AddTransportOutcomeUnknown(MessageVectorSet* messages,
                                const FrameHeader& header,
                                std::string phase) {
  AddDiagnostic(
      messages,
      "PARSER_SERVER_IPC.OUTCOME_UNKNOWN",
      "A request may have reached the server; its physical route is unusable and the request was not replayed.",
      "parser_server_ipc.sbps_client",
      {{"schema_id", std::to_string(header.schema_id)},
       {"transport_phase", std::move(phase)},
       {"session_bound", SessionBoundRequest(header) ? "true" : "false"},
       {"request_replayed", "false"},
       {"route_fatal", "true"},
       {"caller_cleanup_required", "true"}});
}

void AddSessionRouteUnavailable(MessageVectorSet* messages,
                                const FrameHeader& header) {
  AddDiagnostic(
      messages,
      "PARSER_SERVER_IPC.SESSION_ROUTE_UNAVAILABLE",
      "The session-bound request was not sent because its negotiated physical route is unavailable.",
      "parser_server_ipc.sbps_client",
      {{"schema_id", std::to_string(header.schema_id)},
       {"transport_phase", "before_write"},
       {"session_bound", "true"},
       {"request_written", "false"},
       {"request_replayed", "false"},
       {"route_fatal", "true"},
       {"caller_cleanup_required", "true"}});
}

bool HasDiagnosticCode(const MessageVectorSet& messages,
                       std::string_view code) {
  return std::any_of(messages.diagnostics.begin(),
                     messages.diagnostics.end(),
                     [&](const Diagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

template <typename CloseResult>
void ProjectCloseTransportFailure(const MessageVectorSet& messages,
                                  CloseResult* result) {
  if (result == nullptr) return;
  const bool outcome_unknown =
      HasDiagnosticCode(messages, "PARSER_SERVER_IPC.OUTCOME_UNKNOWN");
  const bool route_unavailable = HasDiagnosticCode(
      messages, "PARSER_SERVER_IPC.SESSION_ROUTE_UNAVAILABLE");
  if (!outcome_unknown && !route_unavailable) return;
  result->accepted = false;
  result->outcome_unknown = outcome_unknown;
  result->caller_cleanup_required = true;
  result->route_fatal = true;
  result->detail = outcome_unknown ? "transport_outcome_unknown"
                                   : "physical_session_route_unavailable";
}

void ProjectV2PrepareOutcomeUnknown(MessageVectorSet* messages,
                                    ServerPrepareSblrResult* result,
                                    std::string phase) {
  if (result == nullptr) return;
  if (messages != nullptr &&
      !HasDiagnosticCode(*messages, "PARSER_SERVER_IPC.OUTCOME_UNKNOWN")) {
    AddDiagnostic(
        messages,
        "PARSER_SERVER_IPC.OUTCOME_UNKNOWN",
        "A transaction-routed prepare may have created an engine-owned prepared object; the request was not replayed.",
        "parser_server_ipc.sbps_client",
        {{"transport_phase", phase},
         {"request_replayed", "false"},
         {"caller_cleanup_required", "true"}});
  }
  result->accepted = false;
  result->outcome_unknown = true;
  result->caller_cleanup_required = true;
  result->prepared_statement_uuid.clear();
  result->operation_id.clear();
  result->detail = std::move(phase);
}

void ProjectV2PrepareTransportOutcomeUnknown(
    const MessageVectorSet& messages,
    ServerPrepareSblrResult* result) {
  if (HasDiagnosticCode(messages, "PARSER_SERVER_IPC.OUTCOME_UNKNOWN")) {
    ProjectV2PrepareOutcomeUnknown(
        nullptr, result, "transport_outcome_unknown");
  }
}

bool DecodePrepareResultPayloadV2(
    const std::vector<std::uint8_t>& payload,
    ServerPrepareSblrResult* result,
    MessageVectorSet* messages) {
  if (result == nullptr) return false;
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(payload, &offset, &outcome)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server V2 prepare result payload is malformed.");
    ProjectV2PrepareOutcomeUnknown(
        messages, result, "malformed_outcome");
    return false;
  }
  if (outcome != "accepted" && outcome != "rejected") {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server V2 prepare outcome is not recognized.");
    ProjectV2PrepareOutcomeUnknown(
        messages, result, "malformed_outcome_value");
    return false;
  }
  if (offset + 16 > payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server V2 prepare result payload is malformed.");
    if (outcome == "accepted") {
      ProjectV2PrepareOutcomeUnknown(
          messages, result, "malformed_success_identity");
    }
    return false;
  }
  const auto prepared_uuid = GetUuid(payload, offset);
  offset += 16;
  if (!ReadString(payload, &offset, &result->operation_id) ||
      !ReadString(payload, &offset, &result->detail) ||
      offset != payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server V2 prepare result payload is malformed.");
    if (outcome == "accepted") {
      ProjectV2PrepareOutcomeUnknown(
          messages, result, "malformed_success_payload");
    }
    return false;
  }
  if (outcome == "rejected") {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_REJECTED",
                  "The server rejected transaction-routed SBLR prepare.");
    return false;
  }
  if (std::all_of(prepared_uuid.begin(), prepared_uuid.end(),
                  [](std::uint8_t byte) { return byte == 0; }) ||
      result->operation_id.empty()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server accepted V2 prepare without a usable prepared identity or operation identity.");
    ProjectV2PrepareOutcomeUnknown(
        messages, result, "malformed_success_identity");
    return false;
  }
  result->prepared_statement_uuid = UuidToText(prepared_uuid);
  result->accepted = true;
  return true;
}

void ProjectV2TransportOutcomeUnknown(const MessageVectorSet& messages,
                                      ServerExecutionResult* result) {
  if (result == nullptr ||
      !HasDiagnosticCode(messages, "PARSER_SERVER_IPC.OUTCOME_UNKNOWN")) {
    return;
  }
  result->finality_state = ParserTransactionFinality::kUnknown;
  result->finality_applied = false;
  result->transaction_state_present = false;
  result->local_transaction_id = 0;
  result->snapshot_visible_through_local_transaction_id = 0;
  result->transaction_uuid.clear();
  result->transaction_timestamp.clear();
  result->transaction_outcome_detail = "transport_outcome_unknown";
}

void ProjectV2ResponseOutcomeUnknown(MessageVectorSet* messages,
                                     ServerExecutionResult* result,
                                     std::string phase) {
  if (messages != nullptr &&
      !HasDiagnosticCode(*messages,
                         "PARSER_SERVER_IPC.OUTCOME_UNKNOWN")) {
    AddDiagnostic(
        messages,
        "PARSER_SERVER_IPC.OUTCOME_UNKNOWN",
        "A written transaction-routed request returned a response whose finality could not be authoritatively decoded.",
        "parser_server_ipc.sbps_client",
        {{"transport_phase", std::move(phase)},
         {"request_replayed", "false"},
         {"caller_cleanup_required", "true"}});
  }
  if (messages != nullptr) {
    ProjectV2TransportOutcomeUnknown(*messages, result);
  }
  if (result != nullptr) {
    result->transaction_outcome_detail =
        "response_finality_outcome_unknown";
  }
}

bool SendRequest(const std::string& endpoint,
                 const FrameHeader& header,
                 const std::vector<std::uint8_t>& payload,
                 Frame* response,
                 MessageVectorSet* messages,
                 std::string_view requested_socket_cache_key = {},
                 std::uint32_t timeout_ms = kDefaultSbpsRequestTimeoutMs) {
  const auto total_begin = SbpsClientTraceClock::now();
  const auto endpoint_begin = total_begin;
  const auto path = EndpointPath(endpoint);
  const std::string socket_cache_key = requested_socket_cache_key.empty()
                                           ? path
                                           : std::string(requested_socket_cache_key);
  const auto endpoint_us = SbpsClientElapsedMicros(endpoint_begin);
  if (!ValidateEndpointPath(path, messages)) return false;
  const auto lock_begin = SbpsClientTraceClock::now();
  std::unique_lock<std::mutex> lock(CachedSbpsSocketMutex());
  const auto lock_wait_us = SbpsClientElapsedMicros(lock_begin);
  const bool session_bound = SessionBoundRequest(header);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto attempt_begin = SbpsClientTraceClock::now();
    MessageVectorSet attempt_messages;
    const auto connect_begin = SbpsClientTraceClock::now();
    const SbpsSocketHandle fd = ConnectCachedSbpsSocket(
        path, socket_cache_key, !session_bound, &attempt_messages, timeout_ms);
    const auto connect_us = SbpsClientElapsedMicros(connect_begin);
    if (fd == kInvalidSbpsSocket) {
      if (session_bound) {
        AddSessionRouteUnavailable(&attempt_messages, header);
      }
      WriteSbpsClientPhaseTrace(path,
                                header.message_type,
                                header.schema_id,
                                attempt,
                                payload.size(),
                                0,
                                0,
                                0,
                                false,
                                endpoint_us,
                                lock_wait_us,
                                connect_us,
                                0,
                                0,
                                0,
                                SbpsClientElapsedMicros(attempt_begin),
                                SbpsClientElapsedMicros(total_begin));
      AppendDiagnostics(messages, attempt_messages);
      return false;
    }
    const auto encode_begin = SbpsClientTraceClock::now();
    const auto encoded_frames = EncodeFrameSequence(header, payload);
    const auto encode_us = SbpsClientElapsedMicros(encode_begin);
    std::size_t encoded_frame_bytes = 0;
    for (const auto& encoded : encoded_frames) encoded_frame_bytes += encoded.size();
    bool wrote_all = true;
    const auto write_begin = SbpsClientTraceClock::now();
    for (const auto& encoded : encoded_frames) {
      if (!WriteAll(fd, encoded)) {
        wrote_all = false;
        break;
      }
    }
    const auto write_us = SbpsClientElapsedMicros(write_begin);
    if (!wrote_all) {
      CloseCachedSbpsSocket(socket_cache_key);
      if (!RequestMayRetryAfterTransportLoss(header)) {
        WriteSbpsClientPhaseTrace(path,
                                  header.message_type,
                                  header.schema_id,
                                  attempt,
                                  payload.size(),
                                  encoded_frames.size(),
                                  encoded_frame_bytes,
                                  0,
                                  false,
                                  endpoint_us,
                                  lock_wait_us,
                                  connect_us,
                                  encode_us,
                                  write_us,
                                  0,
                                  SbpsClientElapsedMicros(attempt_begin),
                                  SbpsClientElapsedMicros(total_begin));
        AppendDiagnostics(messages, attempt_messages);
        AddTransportOutcomeUnknown(
            messages, header, "write_attempt_failed");
        return false;
      }
      if (attempt == 0) continue;
      WriteSbpsClientPhaseTrace(path,
                                header.message_type,
                                header.schema_id,
                                attempt,
                                payload.size(),
                                encoded_frames.size(),
                                encoded_frame_bytes,
                                0,
                                false,
                                endpoint_us,
                                lock_wait_us,
                                connect_us,
                                encode_us,
                                write_us,
                                0,
                                SbpsClientElapsedMicros(attempt_begin),
                                SbpsClientElapsedMicros(total_begin));
      AddDiagnostic(messages, "PARSER_SERVER_IPC.WRITE_FAILED", "The parser could not write to sb_server.");
      return false;
    }
    const auto read_begin = SbpsClientTraceClock::now();
    if (ReadExpectedResponse(fd, header.request_uuid, response, &attempt_messages)) {
      const auto read_us = SbpsClientElapsedMicros(read_begin);
      if (header.message_type == kMessageDisconnectNotice) {
        CloseCachedSbpsSocket(socket_cache_key);
      }
      WriteSbpsClientPhaseTrace(path,
                                header.message_type,
                                header.schema_id,
                                attempt,
                                payload.size(),
                                encoded_frames.size(),
                                encoded_frame_bytes,
                                response == nullptr ? 0 : response->payload.size(),
                                true,
                                endpoint_us,
                                lock_wait_us,
                                connect_us,
                                encode_us,
                                write_us,
                                read_us,
                                SbpsClientElapsedMicros(attempt_begin),
                                SbpsClientElapsedMicros(total_begin));
      return true;
    }
    const auto read_us = SbpsClientElapsedMicros(read_begin);
    CloseCachedSbpsSocket(socket_cache_key);
    if (!RequestMayRetryAfterTransportLoss(header)) {
      WriteSbpsClientPhaseTrace(path,
                                header.message_type,
                                header.schema_id,
                                attempt,
                                payload.size(),
                                encoded_frames.size(),
                                encoded_frame_bytes,
                                response == nullptr ? 0 : response->payload.size(),
                                false,
                                endpoint_us,
                                lock_wait_us,
                                connect_us,
                                encode_us,
                                write_us,
                                read_us,
                                SbpsClientElapsedMicros(attempt_begin),
                                SbpsClientElapsedMicros(total_begin));
      AppendDiagnostics(messages, attempt_messages);
      AddTransportOutcomeUnknown(
          messages, header, "response_unavailable_after_write");
      return false;
    }
    if (attempt == 0) continue;
    WriteSbpsClientPhaseTrace(path,
                              header.message_type,
                              header.schema_id,
                              attempt,
                              payload.size(),
                              encoded_frames.size(),
                              encoded_frame_bytes,
                              response == nullptr ? 0 : response->payload.size(),
                              false,
                              endpoint_us,
                              lock_wait_us,
                              connect_us,
                              encode_us,
                              write_us,
                              read_us,
                              SbpsClientElapsedMicros(attempt_begin),
                              SbpsClientElapsedMicros(total_begin));
    AppendDiagnostics(messages, attempt_messages);
    return false;
  }
  WriteSbpsClientPhaseTrace(path,
                            header.message_type,
                            header.schema_id,
                            2,
                            payload.size(),
                            0,
                            0,
                            0,
                            false,
                            endpoint_us,
                            lock_wait_us,
                            0,
                            0,
                            0,
                            0,
                            0,
                            SbpsClientElapsedMicros(total_begin));
  AddDiagnostic(messages, "PARSER_SERVER_IPC.REQUEST_FAILED", "The parser-server IPC request failed.");
  return false;
}

FrameHeader BaseHeader(std::uint16_t message_type,
                       std::uint32_t schema_id,
                       const std::array<std::uint8_t, 16>& session_uuid = {},
                       const std::array<std::uint8_t, 16>& connection_uuid = {}) {
  FrameHeader header;
  header.message_type = message_type;
  header.schema_id = schema_id;
  header.flags = 0;
  header.sequence_number = 1;
  header.request_uuid = MakeUuidV7Bytes();
  header.connection_uuid = connection_uuid;
  header.session_uuid = session_uuid;
  return header;
}

std::vector<std::uint8_t> EncodeBuiltInHelloPayload(
    bool require_transaction_routing_v2 = false,
    bool require_prepared_metadata_transfer_v1 = false,
    bool require_relation_descriptor_projection_v3 = false) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, MakeUuidV7Bytes());
  PutUuid(&out, MakeUuidV7Bytes());
  PutUuid(&out, MakeUuidV7Bytes());
  PutUuid(&out, MakeUuidV7Bytes());
  PutU32(&out, 3);
  PutU32(&out, 0);
  PutString(&out, "SBPS");
  PutString(&out, "sif.test");
  PutString(&out, "sif.test.bundle");
  PutBytes32(&out, {});
  PutUuid(&out, MakeUuidV7Bytes());
  PutUuid(&out, MakeUuidV7Bytes());
  PutU64(&out, 1);
  std::array<std::uint8_t, 32> capabilities{};
  capabilities[0] = kCapabilityBaseline;
  if (require_transaction_routing_v2 ||
      require_prepared_metadata_transfer_v1 ||
      require_relation_descriptor_projection_v3) {
    capabilities[0] |= kCapabilityTransactionRoutingV2;
  }
  if (require_prepared_metadata_transfer_v1) {
    capabilities[0] |= kCapabilityPreparedMetadataTransferV1;
  }
  if (require_relation_descriptor_projection_v3) {
    capabilities[0] |= kCapabilityRelationDescriptorProjectionV3;
  }
  PutBytes32(&out, capabilities);
  return out;
}

AuthCredentialEnvelope CredentialsFromTestWirePayload(std::string_view auth_payload) {
  const auto text = TrimAsciiLocal(auth_payload);
  AuthCredentialEnvelope credentials;
  const auto split = text.find_first_of(" \t\r\n");
  credentials.principal = std::string(split == std::string_view::npos ? text : text.substr(0, split));
  credentials.credential_evidence = split == std::string_view::npos
      ? std::string{}
      : TrimAsciiLocal(text.substr(split + 1));
  credentials.credential_evidence_present = !credentials.credential_evidence.empty();
  return credentials;
}

std::vector<std::uint8_t> EncodeAuthPayload(const AuthCredentialEnvelope& credentials,
                                            const std::array<std::uint8_t, 16>& connection_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutU8(&out, credentials.credential_evidence_present ? 1 : 0);
  PutU8(&out, credentials.credential_invalid ? 1 : 0);
  PutU8(&out, credentials.mfa_required ? 1 : 0);
  PutU8(&out, credentials.mfa_evidence_present ? 1 : 0);
  PutString(&out, credentials.provider_family.empty() ? "local_password" : credentials.provider_family);
  PutString(&out, credentials.principal);
  PutString(&out, credentials.requested_database.empty() ? "default" : credentials.requested_database);
  PutString(&out, credentials.requested_language.empty() ? "en" : credentials.requested_language);
  PutString(&out, credentials.credential_evidence);
  PutString(&out, credentials.application_name);
  PutString(&out, credentials.requested_role);
  return out;
}

std::vector<std::uint8_t> EncodeAttachPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                              const std::array<std::uint8_t, 16>& auth_context_uuid,
                                              std::string_view requested_database) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutUuid(&out, auth_context_uuid);
  PutString(&out, requested_database.empty() ? "default" : requested_database);
  PutString(&out, "read_write");
  return out;
}

std::vector<std::uint8_t> EncodeExecutePayload(const std::array<std::uint8_t, 16>& session_uuid,
                                               std::string_view encoded_sblr_envelope,
                                               bool cursor_requested,
                                               const std::vector<std::uint8_t>& data_packet = {}) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, {});
  PutU8(&out, cursor_requested ? 1 : 0);
  PutString(&out, encoded_sblr_envelope);
  if (!data_packet.empty()) {
    PutBytes(&out, data_packet);
  }
  return out;
}

void PutTransactionRouting(std::vector<std::uint8_t>* out,
                           const ParserTransactionRouting& transaction) {
  PutU8(out, static_cast<std::uint8_t>(transaction.route));
  PutU64(out, transaction.selector.local_transaction_id);
  PutString(out, transaction.selector.transaction_uuid);
}

void PutTransactionSelector(std::vector<std::uint8_t>* out,
                            const ParserTransactionSelector& transaction) {
  PutU64(out, transaction.local_transaction_id);
  PutString(out, transaction.transaction_uuid);
}

bool ValidateTransactionRouting(const ParserTransactionRouting& transaction,
                                MessageVectorSet* messages) {
  const bool selector_present = transaction.selector.present();
  const bool selector_partially_present =
      transaction.selector.local_transaction_id != 0 ||
      !transaction.selector.transaction_uuid.empty();
  switch (transaction.route) {
    case ParserTransactionRoute::kLegacyDefault:
      if (!selector_partially_present) return true;
      break;
    case ParserTransactionRoute::kSelected:
      if (selector_present) return true;
      break;
    case ParserTransactionRoute::kBeginAdditional:
      if (!selector_partially_present) return true;
      break;
  }
  AddDiagnostic(messages,
                "PARSER_SERVER_IPC.TRANSACTION_ROUTING_INVALID",
                "The transaction route and engine-issued selector are inconsistent.");
  return false;
}

std::vector<std::uint8_t> EncodeExecutePayloadV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    bool cursor_requested,
    const std::vector<std::uint8_t>& data_packet,
    const ParserTransactionRouting& transaction) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
  PutU8(&out, cursor_requested ? 1 : 0);
  PutTransactionRouting(&out, transaction);
  PutString(&out, encoded_sblr_envelope);
  PutBytes(&out, data_packet);
  return out;
}

std::vector<std::uint8_t> EncodeCanonicalExecutePayloadV1(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, TextToUuid(session.session_uuid));
  PutUuid(&out, {});  // Prepared reuse is outside the Packet 7 live route.
  PutU8(&out, cursor_requested ? 1 : 0);
  PutU8(&out, static_cast<std::uint8_t>(ParserTransactionRoute::kSelected));
  PutU64(&out, statement_context.transaction.local_transaction_id);
  PutUuid(&out, TextToUuid(statement_context.transaction.transaction_uuid));
  PutUuid(&out, TextToUuid(statement_context.statement_uuid));
  PutBytes(&out, submission.canonical_container_bytes);
  PutBytes(&out, submission.canonical_execution_envelope_bytes);
  PutBytes(&out, data_packet);
  return out;
}

std::vector<std::uint8_t> EncodeCanonicalExecuteLiteralPayloadV1(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  auto out = EncodeCanonicalExecutePayloadV1(
      session, statement_context, submission, data_packet, cursor_requested);
  PutU32(&out, 176);
  out.insert(out.end(), {'S', 'B', 'E', 'L'});
  PutU16(&out, 1);
  PutU16(&out, 176);
  PutU32(&out, 176);
  PutU32(&out, 0);
  PutUuid(&out, TextToUuid(submission.literal_final_receipt_uuid));
  PutUuid(&out, TextToUuid(submission.literal_admission_token_uuid));
  out.insert(out.end(), submission.literal_token_binding_sha256.begin(),
             submission.literal_token_binding_sha256.end());
  out.insert(out.end(), submission.literal_bound_ast_sha256.begin(),
             submission.literal_bound_ast_sha256.end());
  out.insert(out.end(), submission.literal_sbxn_sha256.begin(),
             submission.literal_sbxn_sha256.end());
  out.insert(out.end(), submission.literal_sbos_sha256.begin(),
             submission.literal_sbos_sha256.end());
  return out;
}

void AppendCanonicalExecuteLiteralEvidenceV1(
    std::vector<std::uint8_t>* out,
    const ParserCanonicalSblrSubmission& submission) {
  PutU32(out, 176);
  out->insert(out->end(), {'S', 'B', 'E', 'L'});
  PutU16(out, 1);
  PutU16(out, 176);
  PutU32(out, 176);
  PutU32(out, 0);
  PutUuid(out, TextToUuid(submission.literal_final_receipt_uuid));
  PutUuid(out, TextToUuid(submission.literal_admission_token_uuid));
  out->insert(out->end(), submission.literal_token_binding_sha256.begin(),
              submission.literal_token_binding_sha256.end());
  out->insert(out->end(), submission.literal_bound_ast_sha256.begin(),
              submission.literal_bound_ast_sha256.end());
  out->insert(out->end(), submission.literal_sbxn_sha256.begin(),
              submission.literal_sbxn_sha256.end());
  out->insert(out->end(), submission.literal_sbos_sha256.begin(),
              submission.literal_sbos_sha256.end());
}

std::vector<std::uint8_t> EncodeCanonicalExecuteParameterPayloadV1(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  auto out = EncodeCanonicalExecutePayloadV1(
      session, statement_context, submission, data_packet, cursor_requested);
  PutU32(&out, static_cast<std::uint32_t>(
                   submission.parameter_execution_extension_bytes.size()));
  out.insert(out.end(), submission.parameter_execution_extension_bytes.begin(),
             submission.parameter_execution_extension_bytes.end());
  PutU32(&out,
         static_cast<std::uint32_t>(submission.parameter_value_set_bytes.size()));
  out.insert(out.end(), submission.parameter_value_set_bytes.begin(),
             submission.parameter_value_set_bytes.end());
  if (submission.literal_finalized()) {
    AppendCanonicalExecuteLiteralEvidenceV1(&out, submission);
  }
  return out;
}

std::vector<std::uint8_t> EncodeCanonicalExecuteVariablePayloadV1(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  auto out = EncodeCanonicalExecutePayloadV1(
      session, statement_context, submission, data_packet, cursor_requested);
  PutU32(&out, static_cast<std::uint32_t>(
                   submission.variable_execution_extension_bytes.size()));
  out.insert(out.end(), submission.variable_execution_extension_bytes.begin(),
             submission.variable_execution_extension_bytes.end());
  return out;
}

std::vector<std::uint8_t> EncodePreparePayload(const ParserSessionContext& session,
                                               const std::array<std::uint8_t, 16>& session_uuid,
                                               std::string_view encoded_sblr_envelope) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, MakeUuidV7Bytes());
  PutU64(&out, session.catalog_epoch);
  PutU64(&out, session.security_policy_epoch);
  PutU64(&out, session.security_policy_epoch);
  PutString(&out, encoded_sblr_envelope);
  return out;
}

std::vector<std::uint8_t> EncodePreparePayloadV2(
    const ParserSessionContext& session,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string_view encoded_sblr_envelope,
    const ParserTransactionSelector& transaction) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, MakeUuidV7Bytes());
  PutU64(&out, session.catalog_epoch);
  PutU64(&out, session.security_policy_epoch);
  PutU64(&out, session.security_policy_epoch);
  PutTransactionSelector(&out, transaction);
  PutString(&out, encoded_sblr_envelope);
  return out;
}

std::vector<std::uint8_t> EncodeExecutePreparedPayload(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    bool cursor_requested,
    const std::vector<std::uint8_t>& data_packet = {}) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
  PutU8(&out, cursor_requested ? 1 : 0);
  PutString(&out, encoded_sblr_envelope);
  if (!data_packet.empty()) {
    PutBytes(&out, data_packet);
  }
  return out;
}

std::vector<std::uint8_t> EncodeClosePreparedSblrPayload(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
  return out;
}

std::vector<std::uint8_t> EncodeCursorPayload(const std::array<std::uint8_t, 16>& session_uuid,
                                              std::string_view cursor_uuid,
                                              const CursorStreamDescriptorV1* stream_descriptor,
                                              std::uint64_t max_rows = 1,
                                              std::uint64_t max_bytes = 0,
                                              std::uint32_t fetch_flags = 0) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, TextToUuid(cursor_uuid));
  PutU64(&out, max_rows);
  PutU64(&out, max_bytes);
  PutU32(&out, fetch_flags);
  if (stream_descriptor != nullptr) {
    PutUuid(&out, TextToUuid(stream_descriptor->stream_descriptor_uuid));
    PutU16(&out, stream_descriptor->descriptor_version);
    PutU64(&out, stream_descriptor->descriptor_generation);
  }
  return out;
}

std::string JoinSearchPath(const ParserSessionContext& session) {
  std::string out;
  for (const auto& item : session.search_path) {
    if (!out.empty()) out.push_back(',');
    out += item;
  }
  return out;
}

std::vector<std::uint8_t> EncodeResolveNamePayload(const ParserSessionContext& session,
                                                   std::string_view presented_name,
                                                   bool quoted,
                                                   std::string_view object_class,
                                                   const ParserClientConfig& config,
                                                   bool bypass_cache = false) {
  std::vector<std::uint8_t> out;
  PutString(&out, presented_name);
  PutU8(&out, quoted ? 1 : 0);
  const std::string identifier_profile =
      session.dialect_profile_uuid.empty() ? config.dialect_profile_uuid
                                           : session.dialect_profile_uuid;
  PutString(&out, identifier_profile);
  PutString(&out, session.default_language.empty() ? "en" : session.default_language);
  PutString(&out, JoinSearchPath(session));
  PutString(&out, object_class);
  PutU8(&out, bypass_cache ? 1 : 0);
  return out;
}

std::vector<std::uint8_t> EncodeResolveNamePayloadV2(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeResolveNamePayload(session,
                                      presented_name,
                                      quoted,
                                      object_class,
                                      config,
                                      true);
  PutUuid(&out, TextToUuid(session.session_uuid));
  PutTransactionSelector(&out, transaction);
  return out;
}

std::vector<std::uint8_t> EncodeResolveNamePayloadV3(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction,
    std::uint8_t projection_flags) {
  auto out = EncodeResolveNamePayloadV2(session,
                                        presented_name,
                                        quoted,
                                        object_class,
                                        config,
                                        transaction);
  PutU8(&out, projection_flags);
  return out;
}

std::vector<std::uint8_t> EncodeRenderUuidPayload(std::string_view object_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, TextToUuid(object_uuid));
  return out;
}

std::vector<std::uint8_t> EncodeManagementPayload(std::string_view operation_key,
                                                  std::string_view target_uuid,
                                                  std::string_view mode,
                                                  std::string_view audit_reason,
                                                  std::uint64_t timeout_ms,
                                                  bool include_history) {
  const std::vector<std::pair<std::string, std::string>> fields{
      {"operation_key", std::string(operation_key)},
      {"target_uuid", std::string(target_uuid)},
      {"mode", std::string(mode)},
      {"audit_reason", std::string(audit_reason)},
      {"timeout_ms", std::to_string(timeout_ms)},
      {"include_history", include_history ? "true" : "false"},
  };
  std::vector<std::uint8_t> out;
  PutU16(&out, static_cast<std::uint16_t>(fields.size()));
  for (const auto& [key, value] : fields) {
    PutString(&out, key);
    PutString(&out, value);
  }
  return out;
}

PublicNameResolutionResult DecodePublicNameResultPayload(const Frame& response,
                                                         std::string_view success_outcome) {
  PublicNameResolutionResult result;
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || offset + 16 > response.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The public name/UUID response payload is malformed.",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  const auto object_uuid = GetUuid(response.payload, offset);
  offset += 16;
  std::string canonical_name;
  std::string object_class;
  if (!ReadString(response.payload, &offset, &canonical_name) ||
      !ReadString(response.payload, &offset, &object_class) ||
      offset + 16 > response.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The public name/UUID response payload is malformed.",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  result.catalog_epoch = GetU64(response.payload, offset);
  offset += 8;
  result.security_epoch = GetU64(response.payload, offset);
  offset += 8;
  std::string detail;
  if (!ReadString(response.payload, &offset, &detail)) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The public name/UUID response payload is malformed.",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  if (outcome != success_outcome) {
    if (offset != response.payload.size()) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
          "ERROR",
          "The failed public name/UUID response has trailing bytes.",
          "parser_server_ipc.sbps_client"));
      return result;
    }
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE",
        "ERROR",
        "object name could not be resolved or is not visible",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  result.object_uuid = UuidToText(object_uuid);
  result.canonical_name = canonical_name;
  result.object_class = object_class;
  result.resolution_detail = detail;
  const bool resource_result = IsPublicResourceObjectClass(object_class);
  if (offset < response.payload.size()) {
    constexpr std::uint8_t kResourceDescriptorExtensionV1 = 1;
    const std::uint8_t extension_version = response.payload[offset++];
    auto& descriptor = result.resource_descriptor;
    if (extension_version != kResourceDescriptorExtensionV1 ||
        !ReadString(response.payload, &offset, &descriptor.resource_family) ||
        !ReadString(response.payload, &offset, &descriptor.canonical_name) ||
        !ReadString(response.payload, &offset, &descriptor.parent_resource_uuid) ||
        !ReadString(response.payload, &offset, &descriptor.parent_canonical_name) ||
        !ReadString(response.payload, &offset, &descriptor.default_collation_uuid) ||
        !ReadString(response.payload, &offset, &descriptor.default_collation_name) ||
        offset + 16 > response.payload.size()) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.RESOURCE_DESCRIPTOR_INVALID",
          "ERROR",
          "The engine resource descriptor extension is malformed.",
          "parser_server_ipc.sbps_client"));
      return result;
    }
    descriptor.resource_epoch = GetU64(response.payload, offset);
    offset += 8;
    descriptor.family_epoch = GetU64(response.payload, offset);
    offset += 8;
    if (!ReadString(response.payload, &offset, &descriptor.family_version) ||
        offset + 9 > response.payload.size()) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.RESOURCE_DESCRIPTOR_INVALID",
          "ERROR",
          "The engine resource descriptor extension is malformed.",
          "parser_server_ipc.sbps_client"));
      return result;
    }
    descriptor.min_bytes = GetU32(response.payload, offset);
    offset += 4;
    descriptor.max_bytes = GetU32(response.payload, offset);
    offset += 4;
    const std::uint8_t attributes = response.payload[offset++];
    descriptor.variable_width = (attributes & 0x01u) != 0;
    descriptor.default_for_parent = (attributes & 0x02u) != 0;
    descriptor.case_insensitive = (attributes & 0x04u) != 0;
    descriptor.accent_insensitive = (attributes & 0x08u) != 0;
    descriptor.present = true;
    const bool descriptor_valid =
        resource_result && offset == response.payload.size() &&
        IsPublicResourceObjectClass(descriptor.resource_family) &&
        descriptor.resource_family == object_class &&
        descriptor.canonical_name == canonical_name &&
        descriptor.resource_epoch != 0 && descriptor.family_epoch != 0 &&
        !descriptor.family_version.empty() &&
        (object_class != "charset" ||
         (descriptor.min_bytes != 0 &&
          descriptor.max_bytes >= descriptor.min_bytes)) &&
        (object_class != "collation" ||
         !descriptor.parent_resource_uuid.empty());
    if (!descriptor_valid) {
      descriptor.present = false;
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.RESOURCE_DESCRIPTOR_INVALID",
          "ERROR",
          "The engine resource descriptor extension failed validation.",
          "parser_server_ipc.sbps_client"));
      return result;
    }
  } else if (resource_result) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.RESOURCE_DESCRIPTOR_REQUIRED",
        "ERROR",
        "The server did not return required engine-owned resource metadata.",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  if (offset != response.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The public name/UUID response payload has trailing bytes.",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  result.resolved = true;
  return result;
}

PublicNameResolutionResult DecodePublicNameResultPayloadV3(
    const Frame& response,
    std::string_view success_outcome,
    bool require_relation_descriptor) {
  PublicNameResolutionResult result;
  auto invalid = [&](std::string code, std::string message) {
    result.resolved = false;
    result.object_uuid.clear();
    result.canonical_name.clear();
    result.object_class.clear();
    result.resolution_detail.clear();
    result.catalog_epoch = 0;
    result.security_epoch = 0;
    result.resource_descriptor = {};
    result.relation_descriptor = {};
    AddDiagnostic(&result.messages, std::move(code), std::move(message));
  };
  std::size_t offset = 0;
  auto read_base_string = [&](std::string* value,
                              std::size_t max_bytes) {
    return ReadStringWithin(response.payload,
                            &offset,
                            value,
                            response.payload.size(),
                            max_bytes);
  };
  std::string outcome;
  if (!read_base_string(&outcome, 64) ||
      offset + 16 > response.payload.size()) {
    invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
            "The V3 public name response payload is malformed.");
    return result;
  }
  const auto object_uuid = GetUuid(response.payload, offset);
  offset += 16;
  std::string canonical_name;
  std::string object_class;
  if (!read_base_string(&canonical_name, kMaxPublicRelationMetadataTextBytes) ||
      !read_base_string(&object_class, kMaxPublicRelationMetadataTextBytes) ||
      offset + 16 > response.payload.size()) {
    invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
            "The V3 public name response payload is malformed.");
    return result;
  }
  result.catalog_epoch = GetU64(response.payload, offset);
  offset += 8;
  result.security_epoch = GetU64(response.payload, offset);
  offset += 8;
  std::string detail;
  if (!read_base_string(&detail, kMaxPublicRelationMetadataTextBytes)) {
    invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
            "The V3 public name response payload is malformed.");
    return result;
  }
  if (outcome != success_outcome) {
    if (offset >= response.payload.size() ||
        response.payload.size() - offset != 1 ||
        response.payload[offset++] != 0) {
      invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
              "The failed V3 public name response has an invalid extension envelope.");
      return result;
    }
    invalid("PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE",
            "object name could not be resolved or is not visible");
    return result;
  }
  if (!UuidPresent(object_uuid) || canonical_name.empty() ||
      object_class.empty()) {
    invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
            "The V3 public name response has an incomplete object identity.");
    return result;
  }
  result.object_uuid = UuidToText(object_uuid);
  result.canonical_name = std::move(canonical_name);
  result.object_class = std::move(object_class);
  result.resolution_detail = std::move(detail);

  if (offset >= response.payload.size()) {
    invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUIRED",
            "The V3 public name response omitted its extension envelope.");
    return result;
  }
  const std::uint8_t extension_count = response.payload[offset++];
  if (extension_count > 1) {
    invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
            "The V3 relation descriptor extension count is invalid.");
    return result;
  }
  for (std::uint8_t extension_index = 0;
       extension_index < extension_count;
       ++extension_index) {
    if (offset + 6 > response.payload.size()) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor extension header is truncated.");
      return result;
    }
    const std::uint8_t extension_kind = response.payload[offset++];
    const std::uint8_t extension_version = response.payload[offset++];
    const std::uint32_t extension_bytes = GetU32(response.payload, offset);
    offset += 4;
    if (extension_kind != kRelationDescriptorExtensionKind ||
        (extension_version != kRelationDescriptorExtensionVersion &&
         extension_version != kRelationDescriptorExtensionVersionV2) ||
        extension_bytes > kMaxPublicRelationProjectionBytes ||
        extension_bytes > response.payload.size() - offset) {
      invalid(extension_bytes > kMaxPublicRelationProjectionBytes
                  ? "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_TOO_LARGE"
                  : "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor extension header is invalid.");
      return result;
    }
    const std::size_t extension_end = offset + extension_bytes;
    auto read_bounded_string = [&](std::string* value,
                                   std::size_t max_bytes) {
      return ReadStringWithin(response.payload,
                              &offset,
                              value,
                              extension_end,
                              max_bytes);
    };
    auto& descriptor = result.relation_descriptor;
    if (offset + (extension_version == kRelationDescriptorExtensionVersionV2
                      ? 64
                      : 48) > extension_end) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor identity is truncated.");
      return result;
    }
    const auto descriptor_uuid = GetUuid(response.payload, offset);
    offset += 16;
    const auto relation_uuid = GetUuid(response.payload, offset);
    offset += 16;
    std::array<std::uint8_t, 16> schema_uuid{};
    if (extension_version == kRelationDescriptorExtensionVersionV2) {
      schema_uuid = GetUuid(response.payload, offset);
      offset += 16;
    }
    descriptor.descriptor_generation = GetU64(response.payload, offset);
    offset += 8;
    descriptor.validated_resource_epoch = GetU64(response.payload, offset);
    offset += 8;
    if (offset + 4 > extension_end) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor column count is truncated.");
      return result;
    }
    const std::uint32_t column_count = GetU32(response.payload, offset);
    offset += 4;
    if (!UuidPresent(descriptor_uuid) || !UuidPresent(relation_uuid) ||
        (extension_version == kRelationDescriptorExtensionVersionV2 &&
         !UuidPresent(schema_uuid)) ||
        descriptor.descriptor_generation == 0 ||
        descriptor.validated_resource_epoch == 0 || column_count == 0 ||
        column_count > kMaxPublicRelationProjectionColumns) {
      invalid(column_count > kMaxPublicRelationProjectionColumns
                  ? "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_TOO_LARGE"
                  : "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor identity or column count is invalid.");
      return result;
    }
    descriptor.descriptor_uuid = UuidToText(descriptor_uuid);
    descriptor.relation_uuid = UuidToText(relation_uuid);
    descriptor.schema_uuid = OptionalUuidToText(schema_uuid);
    if (descriptor.relation_uuid != result.object_uuid) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RELATION_MISMATCH",
              "The projected descriptor does not identify the resolved relation.");
      return result;
    }
    std::set<std::string> column_uuids;
    std::set<std::uint32_t> ordinals;
    descriptor.columns.reserve(column_count);
    for (std::uint32_t column_index = 0; column_index < column_count;
         ++column_index) {
      if (offset + 20 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column identity is truncated.");
        return result;
      }
      PublicRelationColumnDescriptor column;
      const auto column_uuid = GetUuid(response.payload, offset);
      offset += 16;
      column.ordinal = GetU32(response.payload, offset);
      offset += 4;
      if (!read_bounded_string(&column.canonical_name_key,
                               kMaxPublicRelationMetadataTextBytes) ||
          offset + 16 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column name or type identity is malformed.");
        return result;
      }
      const auto type_descriptor_uuid = GetUuid(response.payload, offset);
      offset += 16;
      if (!read_bounded_string(&column.type_descriptor_kind,
                               kMaxPublicRelationMetadataTextBytes) ||
          !read_bounded_string(&column.canonical_type_name,
                               kMaxPublicRelationMetadataTextBytes) ||
          !read_bounded_string(&column.encoded_type_descriptor,
                               kMaxPublicEncodedTypeDescriptorBytes) ||
          offset >= extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column type descriptor is malformed.");
        return result;
      }
      const std::uint8_t attributes = response.payload[offset++];
      if ((attributes & 0xf0u) != 0 || offset + 16 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column attribute set is invalid.");
        return result;
      }
      column.nullable = (attributes & 0x01u) != 0;
      column.generated = (attributes & 0x02u) != 0;
      column.identity_column = (attributes & 0x04u) != 0;
      column.charset_variable_width = (attributes & 0x08u) != 0;
      const auto charset_uuid = GetUuid(response.payload, offset);
      offset += 16;
      if (!read_bounded_string(&column.charset_canonical_name,
                               kMaxPublicRelationMetadataTextBytes) ||
          offset + 16 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column charset descriptor is malformed.");
        return result;
      }
      const auto collation_uuid = GetUuid(response.payload, offset);
      offset += 16;
      if (!read_bounded_string(&column.collation_canonical_name,
                               kMaxPublicRelationMetadataTextBytes) ||
          offset + 12 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column collation descriptor is malformed.");
        return result;
      }
      column.character_length = GetU32(response.payload, offset);
      offset += 4;
      column.charset_min_bytes = GetU32(response.payload, offset);
      offset += 4;
      column.charset_max_bytes = GetU32(response.payload, offset);
      offset += 4;

      if (!UuidPresent(column_uuid) || !UuidPresent(type_descriptor_uuid) ||
          column.canonical_name_key.empty() ||
          column.type_descriptor_kind.empty() ||
          column.canonical_type_name.empty() ||
          column.encoded_type_descriptor.empty()) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column has incomplete canonical metadata.");
        return result;
      }
      column.column_uuid = UuidToText(column_uuid);
      column.type_descriptor_uuid = UuidToText(type_descriptor_uuid);
      if (!column_uuids.insert(column.column_uuid).second ||
          !ordinals.insert(column.ordinal).second) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "The V3 relation descriptor repeats a column identity or ordinal.");
        return result;
      }
      const bool has_charset = UuidPresent(charset_uuid);
      const bool has_collation = UuidPresent(collation_uuid);
      const bool text_large_object = EncodedDescriptorHasExactField(
          column.encoded_type_descriptor,
          "text_resource_storage",
          "large_object");
      if (has_charset) column.charset_uuid = UuidToText(charset_uuid);
      if (has_collation) column.collation_uuid = UuidToText(collation_uuid);
      const bool resource_shape_valid =
          (!has_collation || has_charset) &&
          (has_charset
               ? (!column.charset_canonical_name.empty() &&
                  (text_large_object ? column.character_length == 0
                                     : column.character_length != 0) &&
                  column.charset_min_bytes != 0 &&
                  column.charset_max_bytes >= column.charset_min_bytes)
               : (column.charset_canonical_name.empty() &&
                  column.collation_canonical_name.empty() &&
                  column.character_length == 0 &&
                  column.charset_min_bytes == 0 &&
                  column.charset_max_bytes == 0 &&
                  !column.charset_variable_width)) &&
          (!has_collation || !column.collation_canonical_name.empty());
      if (!resource_shape_valid) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
                "A V3 relation column has inconsistent canonical resource metadata.");
        return result;
      }
      descriptor.columns.push_back(std::move(column));
    }
    if (offset != extension_end) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor extension has trailing bytes.");
      return result;
    }
    descriptor.present = true;
  }
  if (offset != response.payload.size()) {
    invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
            "The V3 relation descriptor envelope has trailing bytes.");
    return result;
  }
  if (require_relation_descriptor && !result.relation_descriptor.present) {
    invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUIRED",
            "The server did not return the requested persisted relation descriptor.");
    return result;
  }
  result.resolved = true;
  return result;
}

bool RequireTransactionRoutingV2(const ParserSessionContext& session,
                                 MessageVectorSet* messages) {
  if (session.transaction_routing_v2_negotiated) return true;
  AddDiagnostic(messages,
                "PARSER_SERVER_IPC.TRANSACTION_ROUTING_V2_NOT_NEGOTIATED",
                "Independent transaction routing was not negotiated during hello.");
  return false;
}

bool RequireRelationDescriptorProjectionV3(
    const ParserSessionContext& session,
    MessageVectorSet* messages) {
  if (session.relation_descriptor_projection_v3_negotiated) return true;
  AddDiagnostic(
      messages,
      "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_V3_NOT_NEGOTIATED",
      "Persisted relation projection was not negotiated during hello.");
  return false;
}

} // namespace

bool DecodeDiagnosticFrameForTest(
    const std::vector<std::uint8_t>& encoded_frame,
    MessageVectorSet* messages) {
  if (messages == nullptr) return false;
  Frame frame;
  if (!DecodeFrame(encoded_frame, &frame, messages) || !IsErrorFrame(frame)) {
    return false;
  }
  const std::size_t diagnostic_count = messages->diagnostics.size();
  AddFrameDiagnostics(frame, messages);
  return messages->diagnostics.size() > diagnostic_count;
}

bool DecodeAcquireStatementContextResultPayloadV1ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV1(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV4ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV4(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV5ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV5(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV6ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV6(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV7ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV7(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV8ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV8(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV9ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV9(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV10ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV10(payload, context);
}

std::vector<std::uint8_t>
EncodeAcquireStatementContextRequestPayloadV1ForTest(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  return EncodeAcquireStatementContextPayloadV1(session, transaction);
}

std::vector<std::uint8_t> EncodeCanonicalExecutePayloadV1ForTest(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  return EncodeCanonicalExecutePayloadV1(session,
                                         statement_context,
                                         submission,
                                         data_packet,
                                         cursor_requested);
}

std::vector<std::uint8_t> EncodeResolveNameRequestPayloadV2ForTest(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) {
  return EncodeResolveNamePayloadV2(session,
                                    presented_name,
                                    quoted,
                                    object_class,
                                    config,
                                    transaction);
}

std::vector<std::uint8_t> EncodeResolveNameRequestPayloadV3ForTest(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction,
    std::uint8_t projection_flags) {
  return EncodeResolveNamePayloadV3(session,
                                    presented_name,
                                    quoted,
                                    object_class,
                                    config,
                                    transaction,
                                    projection_flags);
}

bool DecodeResolveNameResultPayloadV3ForTest(
    const std::vector<std::uint8_t>& payload,
    bool require_relation_descriptor,
    PublicNameResolutionResult* result) {
  if (result == nullptr) return false;
  Frame frame;
  frame.payload = payload;
  *result = DecodePublicNameResultPayloadV3(
      frame, "resolved", require_relation_descriptor);
  return result->resolved && result->messages.diagnostics.empty();
}

struct SbpsClientChannelState {
  bool dedicated_v2_channel_enabled{false};
  std::string dedicated_v2_socket_cache_key;
  std::vector<std::uint8_t> stable_baseline_hello_payload;
  std::vector<std::uint8_t> stable_v2_hello_payload;
  std::vector<std::uint8_t> stable_prepared_metadata_transfer_v1_hello_payload;
  std::vector<std::uint8_t> stable_relation_descriptor_v3_hello_payload;
  std::vector<std::uint8_t>
      stable_prepared_metadata_transfer_relation_descriptor_v3_hello_payload;
};

namespace {

void ReleaseDedicatedV2Channel(SbpsClientChannelState* state) {
  if (state == nullptr || !state->dedicated_v2_channel_enabled ||
      state->dedicated_v2_socket_cache_key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> guard(CachedSbpsSocketMutex());
  CloseCachedSbpsSocket(state->dedicated_v2_socket_cache_key);
  state->dedicated_v2_channel_enabled = false;
}

}  // namespace

std::vector<std::uint8_t> EncodePreparedParameterSchema4015Template(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission) {
  if (!submission.complete() || submission.literal_finalized() ||
      submission.parameter_finalized()) {
    return {};
  }
  return EncodeCanonicalExecutePayloadV1(session, statement_context,
                                         submission, {}, false);
}

bool DecodeExecuteResultPayloadV2ForTest(
    const std::vector<std::uint8_t>& payload,
    ServerExecutionResult* result,
    MessageVectorSet* messages) {
  Frame frame;
  frame.payload = payload;
  return DecodeExecuteResultPayloadV2(frame, result, messages);
}

bool DecodePrepareResultPayloadV2ForTest(
    const std::vector<std::uint8_t>& payload,
    ServerPrepareSblrResult* result,
    MessageVectorSet* messages) {
  return DecodePrepareResultPayloadV2(payload, result, messages);
}

bool V2RequestMayRetryAfterWriteForTest(std::uint32_t schema_id) {
  return !V2RequestIsNonReplayableAfterWrite(schema_id);
}

bool SessionBoundRequestMayRetryAfterWriteForTest(std::uint32_t schema_id) {
  FrameHeader header;
  header.schema_id = schema_id;
  header.session_uuid.front() = 1;
  return RequestMayRetryAfterTransportLoss(header);
}

SbpsClient::SbpsClient(std::string endpoint)
    : endpoint_(std::move(endpoint)),
      channel_state_(std::make_unique<SbpsClientChannelState>()) {
  channel_state_->dedicated_v2_socket_cache_key =
      endpoint_ + "|sbps-v2-client|" + UuidToText(MakeUuidV7Bytes());
  channel_state_->stable_baseline_hello_payload =
      EncodeBuiltInHelloPayload();
  channel_state_->stable_v2_hello_payload = EncodeBuiltInHelloPayload(true);
  channel_state_->stable_prepared_metadata_transfer_v1_hello_payload =
      EncodeBuiltInHelloPayload(true, true);
  channel_state_->stable_relation_descriptor_v3_hello_payload =
      EncodeBuiltInHelloPayload(true, false, true);
  channel_state_
      ->stable_prepared_metadata_transfer_relation_descriptor_v3_hello_payload =
      EncodeBuiltInHelloPayload(true, true, true);
}

SbpsClient::~SbpsClient() {
  ReleaseDedicatedV2Channel(channel_state_.get());
}

SbpsClient::SbpsClient(SbpsClient&& other) noexcept = default;

SbpsClient& SbpsClient::operator=(SbpsClient&& other) noexcept {
  if (this == &other) return *this;
  ReleaseDedicatedV2Channel(channel_state_.get());
  endpoint_ = std::move(other.endpoint_);
  channel_state_ = std::move(other.channel_state_);
  return *this;
}

void SbpsClient::EnableDedicatedV2Channel() const {
  if (channel_state_ != nullptr) {
    channel_state_->dedicated_v2_channel_enabled = true;
  }
}

const std::string& SbpsClient::ActiveSocketCacheKey() const {
  static const std::string empty;
  if (channel_state_ == nullptr ||
      !channel_state_->dedicated_v2_channel_enabled) {
    return empty;
  }
  return channel_state_->dedicated_v2_socket_cache_key;
}

const std::vector<std::uint8_t>& SbpsClient::StableV2HelloPayload() const {
  static const std::vector<std::uint8_t> empty;
  return channel_state_ == nullptr ? empty
                                   : channel_state_->stable_v2_hello_payload;
}

const std::vector<std::uint8_t>&
SbpsClient::StablePreparedMetadataTransferV1HelloPayload() const {
  static const std::vector<std::uint8_t> empty;
  return channel_state_ == nullptr
             ? empty
             : channel_state_
                   ->stable_prepared_metadata_transfer_v1_hello_payload;
}

const std::vector<std::uint8_t>&
SbpsClient::StableRelationDescriptorV3HelloPayload() const {
  static const std::vector<std::uint8_t> empty;
  return channel_state_ == nullptr
             ? empty
             : channel_state_->stable_relation_descriptor_v3_hello_payload;
}

const std::vector<std::uint8_t>&
SbpsClient::StablePreparedMetadataTransferRelationDescriptorV3HelloPayload()
    const {
  static const std::vector<std::uint8_t> empty;
  return channel_state_ == nullptr
             ? empty
             : channel_state_
                   ->stable_prepared_metadata_transfer_relation_descriptor_v3_hello_payload;
}

std::string SbpsClient::V2ChannelCacheKeyForTest() const {
  return channel_state_ == nullptr
             ? std::string{}
             : channel_state_->dedicated_v2_socket_cache_key;
}

std::vector<std::uint8_t> SbpsClient::V2HelloPayloadForTest() const {
  return StableV2HelloPayload();
}

std::vector<std::uint8_t>
SbpsClient::PreparedMetadataTransferV1HelloPayloadForTest() const {
  return StablePreparedMetadataTransferV1HelloPayload();
}

std::vector<std::uint8_t>
SbpsClient::RelationDescriptorV3HelloPayloadForTest() const {
  return StableRelationDescriptorV3HelloPayload();
}

bool SbpsClient::UsesDedicatedV2ChannelForTest() const {
  return channel_state_ != nullptr &&
         channel_state_->dedicated_v2_channel_enabled;
}

bool SbpsClient::SendHello(MessageVectorSet* messages) const {
  return SendHelloWithRequirements(false, nullptr, false, nullptr, false,
                                   nullptr, messages);
}

bool SbpsClient::SendHelloWithRequirements(
    bool require_transaction_routing_v2,
    bool* transaction_routing_v2_accepted,
    bool require_prepared_metadata_transfer_v1,
    bool* prepared_metadata_transfer_v1_accepted,
    bool require_relation_descriptor_projection_v3,
    bool* relation_descriptor_projection_v3_accepted,
    MessageVectorSet* messages) const {
  if (transaction_routing_v2_accepted != nullptr) {
    *transaction_routing_v2_accepted = false;
  }
  if (prepared_metadata_transfer_v1_accepted != nullptr) {
    *prepared_metadata_transfer_v1_accepted = false;
  }
  if (relation_descriptor_projection_v3_accepted != nullptr) {
    *relation_descriptor_projection_v3_accepted = false;
  }
  if (require_transaction_routing_v2 ||
      require_prepared_metadata_transfer_v1 ||
      require_relation_descriptor_projection_v3) {
    EnableDedicatedV2Channel();
  }
  const std::vector<std::uint8_t>* hello_payload = nullptr;
  if (require_prepared_metadata_transfer_v1 &&
      require_relation_descriptor_projection_v3) {
    hello_payload =
        &StablePreparedMetadataTransferRelationDescriptorV3HelloPayload();
  } else if (require_prepared_metadata_transfer_v1) {
    hello_payload = &StablePreparedMetadataTransferV1HelloPayload();
  } else if (require_relation_descriptor_projection_v3) {
    hello_payload = &StableRelationDescriptorV3HelloPayload();
  } else if (require_transaction_routing_v2) {
    hello_payload = &StableV2HelloPayload();
  } else {
    hello_payload = channel_state_ == nullptr
                        ? nullptr
                        : &channel_state_->stable_baseline_hello_payload;
  }
  if (hello_payload == nullptr || hello_payload->empty()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.HELLO_IDENTITY_INVALID",
                  "The stable parser HELLO identity is unavailable.");
    return false;
  }
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageHello, kSchemaHelloRequestV1),
                   *hello_payload,
                   &response,
                   messages,
                   ActiveSocketCacheKey())) {
    return false;
  }
  if (response.header.message_type != kMessageHelloAccept || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, messages);
    return false;
  }
  const bool transaction_routing_v2_accepted_by_server =
      response.payload.size() > kHelloAcceptCapabilityOffset &&
      (response.payload[kHelloAcceptCapabilityOffset] &
       kCapabilityTransactionRoutingV2) != 0;
  if (transaction_routing_v2_accepted != nullptr) {
    *transaction_routing_v2_accepted =
        transaction_routing_v2_accepted_by_server;
  }
  const bool prepared_metadata_transfer_v1_accepted_by_server =
      response.payload.size() > kHelloAcceptCapabilityOffset &&
      (response.payload[kHelloAcceptCapabilityOffset] &
       kCapabilityPreparedMetadataTransferV1) != 0;
  if (prepared_metadata_transfer_v1_accepted != nullptr) {
    *prepared_metadata_transfer_v1_accepted =
        prepared_metadata_transfer_v1_accepted_by_server;
  }
  const bool relation_descriptor_projection_v3_accepted_by_server =
      response.payload.size() > kHelloAcceptCapabilityOffset &&
      (response.payload[kHelloAcceptCapabilityOffset] &
       kCapabilityRelationDescriptorProjectionV3) != 0;
  if (relation_descriptor_projection_v3_accepted != nullptr) {
    *relation_descriptor_projection_v3_accepted =
        relation_descriptor_projection_v3_accepted_by_server;
  }
  if (require_transaction_routing_v2 &&
      !transaction_routing_v2_accepted_by_server) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.TRANSACTION_ROUTING_V2_REQUIRED",
                  "The server did not negotiate required independent transaction routing.");
    return false;
  }
  if (require_prepared_metadata_transfer_v1 &&
      !prepared_metadata_transfer_v1_accepted_by_server) {
    AddDiagnostic(
        messages,
        "PARSER_SERVER_IPC.PREPARED_METADATA_TRANSFER_V1_REQUIRED",
        "The server did not negotiate required prepared metadata transfer.");
    return false;
  }
  if (require_relation_descriptor_projection_v3 &&
      !relation_descriptor_projection_v3_accepted_by_server) {
    AddDiagnostic(
        messages,
        "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_V3_REQUIRED",
        "The server did not negotiate required persisted relation projection.");
    return false;
  }
  return true;
}

bool SbpsClient::AuthenticateAndAttach(std::string_view auth_payload,
                                       const ParserClientConfig& config,
                                       ParserSessionContext* session,
                                       MessageVectorSet* messages) const {
  auto credentials = CredentialsFromTestWirePayload(auth_payload);
  if (!config.database_token.empty() && credentials.requested_database == "default") {
    credentials.requested_database = config.database_token;
  }
  return AuthenticateAndAttach(credentials, config, session, messages);
}

bool SbpsClient::AuthenticateAndAttach(const AuthCredentialEnvelope& credentials,
                                       const ParserClientConfig& config,
                                       ParserSessionContext* session,
                                       MessageVectorSet* messages) const {
  if (session == nullptr) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SESSION_CONTEXT_MISSING", "The parser session context is unavailable.");
    return false;
  }
  bool transaction_routing_v2_accepted = false;
  bool prepared_metadata_transfer_v1_accepted = false;
  bool relation_descriptor_projection_v3_accepted = false;
  const bool require_transaction_routing_v2 =
      config.require_transaction_routing_v2 ||
      config.require_prepared_metadata_transfer_v1 ||
      config.require_relation_descriptor_projection_v3;
  const std::vector<std::uint8_t>* admitted_hello_payload = nullptr;
  if (config.require_prepared_metadata_transfer_v1 &&
      config.require_relation_descriptor_projection_v3) {
    admitted_hello_payload =
        &StablePreparedMetadataTransferRelationDescriptorV3HelloPayload();
  } else if (config.require_prepared_metadata_transfer_v1) {
    admitted_hello_payload = &StablePreparedMetadataTransferV1HelloPayload();
  } else if (config.require_relation_descriptor_projection_v3) {
    admitted_hello_payload = &StableRelationDescriptorV3HelloPayload();
  } else if (require_transaction_routing_v2) {
    admitted_hello_payload = &StableV2HelloPayload();
  } else if (channel_state_ != nullptr) {
    admitted_hello_payload =
        &channel_state_->stable_baseline_hello_payload;
  }
  if (admitted_hello_payload == nullptr || admitted_hello_payload->size() < 72) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.HELLO_IDENTITY_INVALID",
                  "The stable parser HELLO identity is unavailable.");
    return false;
  }
  const auto admitted_parser_package_uuid =
      GetUuid(*admitted_hello_payload, 16);
  const auto admitted_dialect_profile_uuid =
      GetUuid(*admitted_hello_payload, 48);
  const auto admitted_parser_api_major =
      GetU32(*admitted_hello_payload, 64);
  const auto admitted_parser_api_minor =
      GetU32(*admitted_hello_payload, 68);
  if (!UuidPresent(admitted_parser_package_uuid) ||
      !UuidPresent(admitted_dialect_profile_uuid) ||
      admitted_parser_api_major == 0) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.HELLO_IDENTITY_INVALID",
                  "The stable parser HELLO did not contain canonical package and dialect identities.");
    return false;
  }
  if (!SendHelloWithRequirements(require_transaction_routing_v2,
                                 &transaction_routing_v2_accepted,
                                 config.require_prepared_metadata_transfer_v1,
                                 &prepared_metadata_transfer_v1_accepted,
                                 config.require_relation_descriptor_projection_v3,
                                 &relation_descriptor_projection_v3_accepted,
                                 messages)) {
    return false;
  }
  session->transaction_routing_v2_negotiated =
      transaction_routing_v2_accepted;
  session->prepared_metadata_transfer_v1_negotiated =
      prepared_metadata_transfer_v1_accepted;
  session->relation_descriptor_projection_v3_negotiated =
      relation_descriptor_projection_v3_accepted;

  Frame auth_response;
  const auto connection_uuid = MakeUuidV7Bytes();
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageAuthHandoff, kSchemaAuthHandoffV1, {}, connection_uuid),
                   EncodeAuthPayload(credentials, connection_uuid),
                   &auth_response,
                   messages,
                   ActiveSocketCacheKey())) {
    return false;
  }
  if (auth_response.header.message_type != kMessageAuthResult) {
    AddFrameDiagnostics(auth_response, messages);
    return false;
  }
  std::size_t offset = 0;
  std::string auth_outcome;
  if (!ReadString(auth_response.payload, &offset, &auth_outcome) || auth_outcome != "accepted") {
    if (IsErrorFrame(auth_response)) AddFrameDiagnostics(auth_response, messages);
    else AddDiagnostic(messages, "SECURITY.AUTHENTICATION.FAILED", "Authentication failed.");
    return false;
  }
  if (offset + 16 * 4 + 8 > auth_response.payload.size()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.AUTH_RESULT_INVALID", "The server authentication result payload is malformed.");
    return false;
  }
  const auto auth_context_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  const auto auth_session_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  const auto principal_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  const auto effective_user_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  const auto security_epoch = GetU64(auth_response.payload, offset);
  (void)auth_session_uuid;
  (void)principal_uuid;
  (void)effective_user_uuid;
  (void)security_epoch;

  Frame attach_response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageAttachDatabase,
                              kSchemaAttachRequestV1,
                              {},
                              connection_uuid),
                   EncodeAttachPayload(connection_uuid, auth_context_uuid, credentials.requested_database),
                   &attach_response,
                   messages,
                   ActiveSocketCacheKey())) {
    return false;
  }
  if (attach_response.header.message_type != kMessageAttachResult) {
    AddFrameDiagnostics(attach_response, messages);
    return false;
  }
  offset = 0;
  std::string attach_outcome;
  if (!ReadString(attach_response.payload, &offset, &attach_outcome) || attach_outcome != "accepted") {
    if (IsErrorFrame(attach_response)) AddFrameDiagnostics(attach_response, messages);
    else AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_DATABASE_FAILED", "Database attach failed.");
    return false;
  }
  if (offset + 16 + 16 > attach_response.payload.size()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
    return false;
  }
  const auto session_uuid = GetUuid(attach_response.payload, offset);
  offset += 16;
  const auto user_uuid = GetUuid(attach_response.payload, offset);
  offset += 16;
  std::string database_path;
  std::string database_uuid;
  std::string attach_mode;
  if (!ReadString(attach_response.payload, &offset, &database_path) ||
      !ReadString(attach_response.payload, &offset, &database_uuid) ||
      !ReadString(attach_response.payload, &offset, &attach_mode) ||
      offset + 8 * 5 > attach_response.payload.size()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
    return false;
  }
  const auto catalog_generation = GetU64(attach_response.payload, offset);
  offset += 8;
  const auto attach_security_epoch = GetU64(attach_response.payload, offset);
  offset += 8;
  const auto policy_generation = GetU64(attach_response.payload, offset);
  offset += 8;
  const auto name_resolution_epoch = GetU64(attach_response.payload, offset);
  offset += 8;
  const auto descriptor_epoch = GetU64(attach_response.payload, offset);
  offset += 8;
  std::string attach_detail;
  std::string engine_health;
  if (offset < attach_response.payload.size() &&
      (!ReadString(attach_response.payload, &offset, &attach_detail) ||
       !ReadString(attach_response.payload, &offset, &engine_health))) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
    return false;
  }
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_uuid;
  std::string transaction_timestamp;
  std::vector<std::string> effective_role_uuids;
  std::vector<std::string> effective_group_uuids;
  if (offset < attach_response.payload.size()) {
    if (offset + 16 > attach_response.payload.size()) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
      return false;
    }
    local_transaction_id = GetU64(attach_response.payload, offset);
    offset += 8;
    snapshot_visible_through_local_transaction_id = GetU64(attach_response.payload, offset);
    offset += 8;
    if (!ReadString(attach_response.payload, &offset, &transaction_uuid) ||
        !ReadString(attach_response.payload, &offset, &transaction_timestamp)) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
      return false;
    }
  }
  if (offset < attach_response.payload.size()) {
    auto add_unique_uuid_text = [](std::vector<std::string>* values,
                                   const std::array<std::uint8_t, 16>& uuid) {
      if (values == nullptr || !UuidPresent(uuid)) return;
      const std::string text = UuidToText(uuid);
      if (std::find(values->begin(), values->end(), text) == values->end()) {
        values->push_back(text);
      }
    };
    if (offset + 4 > attach_response.payload.size()) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach role payload is malformed.");
      return false;
    }
    const auto role_count = GetU32(attach_response.payload, offset);
    offset += 4;
    for (std::uint32_t index = 0; index < role_count; ++index) {
      if (offset + 16 > attach_response.payload.size()) {
        AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach role payload is malformed.");
        return false;
      }
      add_unique_uuid_text(&effective_role_uuids, GetUuid(attach_response.payload, offset));
      offset += 16;
    }
    if (offset + 16 > attach_response.payload.size()) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach active-role payload is malformed.");
      return false;
    }
    add_unique_uuid_text(&effective_role_uuids, GetUuid(attach_response.payload, offset));
    offset += 16;
    if (offset + 4 > attach_response.payload.size()) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach group payload is malformed.");
      return false;
    }
    const auto group_count = GetU32(attach_response.payload, offset);
    offset += 4;
    for (std::uint32_t index = 0; index < group_count; ++index) {
      if (offset + 16 > attach_response.payload.size()) {
        AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach group payload is malformed.");
        return false;
      }
      add_unique_uuid_text(&effective_group_uuids, GetUuid(attach_response.payload, offset));
      offset += 16;
    }
  }
  (void)database_path;
  (void)attach_mode;
  (void)attach_detail;
  (void)engine_health;

  if (local_transaction_id == 0 ||
      !IsCanonicalNonzeroUuidText(transaction_uuid)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_TRANSACTION_IDENTITY_INVALID",
                  "Accepted database attach did not publish a complete engine-issued transaction ID and UUID.");
    return false;
  }

  session->authenticated = true;
  session->admitted_parser_package_uuid =
      UuidToText(admitted_parser_package_uuid);
  session->admitted_dialect_profile_uuid =
      UuidToText(admitted_dialect_profile_uuid);
  session->admitted_parser_package_version_major =
      admitted_parser_api_major;
  session->admitted_parser_package_version_minor =
      admitted_parser_api_minor;
  session->admitted_parser_package_version_patch = 0;
  session->session_uuid = UuidToText(session_uuid);
  session->connection_uuid = UuidToText(connection_uuid);
  session->database_uuid = database_uuid;
  session->authenticated_user_uuid = UuidToText(user_uuid);
  session->principal_claim = credentials.principal;
  session->auth_provider_family =
      credentials.provider_family.empty() ? "local_password" : credentials.provider_family;
  session->effective_role_uuids = std::move(effective_role_uuids);
  session->effective_group_uuids = std::move(effective_group_uuids);
  ApplySbpsLanguageContext(session,
                           config,
                           credentials.requested_language,
                           descriptor_epoch == 0 ? name_resolution_epoch
                                                 : descriptor_epoch,
                           name_resolution_epoch);
  // The admitted dialect UUID identifies the negotiated parser package and
  // is carried separately in canonical SBLR/SBEE binding. Public name
  // resolution uses the parser family's semantic identifier profile.
  session->dialect_profile_uuid = config.dialect_profile_uuid;
  session->search_path = config.default_search_path;
  session->transaction_context = "always_active";
  session->local_transaction_id = local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = transaction_uuid;
  session->transaction_timestamp = transaction_timestamp;
  session->catalog_epoch = catalog_generation;
  session->security_policy_epoch = attach_security_epoch == 0 ? policy_generation : attach_security_epoch;
  session->descriptor_epoch = descriptor_epoch == 0 ? name_resolution_epoch : descriptor_epoch;
  return true;
}

PublicNameResolutionResult ResolveNamePublicWithCachePolicy(std::string_view endpoint,
                                                            std::string_view socket_cache_key,
                                                            const ParserSessionContext& session,
                                                            std::string_view presented_name,
                                                            bool quoted,
                                                            std::string_view object_class,
                                                            const ParserClientConfig& config,
                                                            bool use_cache) {
  PublicNameResolutionResult result;
  use_cache = use_cache && !IsPublicResourceObjectClass(object_class);
  const std::string endpoint_string(endpoint);
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.AUTH.REQUIRED",
        "ERROR",
        "public name resolution requires an authenticated server session",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  const auto cache_key =
      SbpsClientResolveNameCacheKey(
          endpoint_string, session, presented_name, quoted, object_class, config);
  if (use_cache) {
    if (const auto cached = LookupSbpsClientPublicResolutionCache(cache_key)) {
      return PublicResolutionResultFromCache(*cached);
    }
  }
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!SendRequest(endpoint_string,
                   BaseHeader(kMessageResolveNameRequest,
                              kSchemaResolveNameRequestV1,
                              session_uuid,
                              connection_uuid),
                   EncodeResolveNamePayload(session,
                                            presented_name,
                                            quoted,
                                            object_class,
                                            config,
                                            !use_cache),
                   &response,
                   &messages,
                   socket_cache_key)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result = DecodePublicNameResultPayload(response, "resolved");
  if (use_cache) {
    StoreSbpsClientPublicResolutionCacheEntry(cache_key, result);
  }
  return result;
}

PublicNameResolutionResult SbpsClient::ResolveNamePublic(const ParserSessionContext& session,
                                                        std::string_view presented_name,
                                                        bool quoted,
                                                        std::string_view object_class,
                                                        const ParserClientConfig& config) const {
  return ResolveNamePublicWithCachePolicy(endpoint_,
                                          ActiveSocketCacheKey(),
                                          session,
                                          presented_name,
                                          quoted,
                                          object_class,
                                          config,
                                          true);
}

PublicNameResolutionResult SbpsClient::ResolveNamePublicUncached(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config) const {
  return ResolveNamePublicWithCachePolicy(endpoint_,
                                          ActiveSocketCacheKey(),
                                          session,
                                          presented_name,
                                          quoted,
                                          object_class,
                                          config,
                                          false);
}

PublicNameResolutionResult SbpsClient::ResolveNamePublicOnTransaction(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.AUTH.REQUIRED",
        "ERROR",
        "transaction-routed name resolution requires an authenticated server session",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  if (!RequireTransactionRoutingV2(session, &result.messages)) {
    return result;
  }
  if (!transaction.present()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
        "ERROR",
        "transaction-routed name resolution requires an engine-issued selector",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageResolveNameRequest,
                              kSchemaResolveNameRequestV2,
                              session_uuid,
                              connection_uuid),
                   EncodeResolveNamePayloadV2(session,
                                              presented_name,
                                              quoted,
                                              object_class,
                                              config,
                                              transaction),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult ||
      response.header.schema_id != kSchemaResolveNameResultV2 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(&messages,
                    "PARSER_SERVER_IPC.NAME_RESULT_SCHEMA_MISMATCH",
                    "The server did not return the transaction-routed name-result schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  return DecodePublicNameResultPayload(response, "resolved");
}

PublicNameResolutionResult
SbpsClient::ResolveNameSemanticPublicOnTransaction(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.AUTH.REQUIRED",
                  "semantic name resolution requires an authenticated server session");
    return result;
  }
  if (!RequireTransactionRoutingV2(session, &result.messages) ||
      !RequireRelationDescriptorProjectionV3(session, &result.messages)) {
    return result;
  }
  if (!transaction.present()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
                  "semantic name resolution requires an engine-issued selector");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageResolveNameRequest,
                     kSchemaResolveNameRequestV3,
                     session_uuid,
                     connection_uuid),
          EncodeResolveNamePayloadV3(session,
                                     presented_name,
                                     quoted,
                                     object_class,
                                     config,
                                     transaction,
                                     0),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult ||
      response.header.schema_id != kSchemaResolveNameResultV3 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(
          &messages,
          "PARSER_SERVER_IPC.NAME_RESULT_SCHEMA_MISMATCH",
          "The server did not return the semantic V3 name-result schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  return DecodePublicNameResultPayloadV3(response, "resolved", false);
}

PublicNameResolutionResult
SbpsClient::ResolveRelationDescriptorPublicOnTransaction(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.AUTH.REQUIRED",
                  "persisted relation projection requires an authenticated server session");
    return result;
  }
  if (!RequireTransactionRoutingV2(session, &result.messages)) {
    return result;
  }
  if (!RequireRelationDescriptorProjectionV3(session, &result.messages)) {
    return result;
  }
  if (!transaction.present()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
                  "persisted relation projection requires an engine-issued selector");
    return result;
  }
  if (object_class != "relation" && object_class != "table") {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID",
                  "persisted relation projection is valid only for a relation or table");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageResolveNameRequest,
                     kSchemaResolveNameRequestV3,
                     session_uuid,
                     connection_uuid),
          EncodeResolveNamePayloadV3(
              session,
              presented_name,
              quoted,
              object_class,
              config,
              transaction,
              kResolveNameProjectionRelationDescriptorV1),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult ||
      response.header.schema_id != kSchemaResolveNameResultV3 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(
          &messages,
          "PARSER_SERVER_IPC.NAME_RESULT_SCHEMA_MISMATCH",
          "The server did not return the persisted relation projection schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  return DecodePublicNameResultPayloadV3(response, "resolved", true);
}

PublicNameResolutionResult SbpsClient::RenderUuidPublic(const ParserSessionContext& session,
                                                       std::string_view object_uuid) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.AUTH.REQUIRED",
        "ERROR",
        "public UUID rendering requires an authenticated server session",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  const auto cache_key = SbpsClientRenderUuidCacheKey(endpoint_, session, object_uuid);
  if (const auto cached = LookupSbpsClientPublicResolutionCache(cache_key)) {
    return PublicResolutionResultFromCache(*cached);
  }
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageRenderUuidRequest,
                              kSchemaRenderUuidRequestV1,
                              session_uuid,
                              connection_uuid),
                   EncodeRenderUuidPayload(object_uuid),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageRenderUuidResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result = DecodePublicNameResultPayload(response, "rendered");
  StoreSbpsClientPublicResolutionCacheEntry(cache_key, result);
  return result;
}

ServerStatementContextResult SbpsClient::AcquireStatementContext(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) const {
  ServerStatementContextResult result;
  if (!session.authenticated) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.AUTH.REQUIRED",
                  "statement-context acquisition requires an authenticated server session");
    return result;
  }
  if (!transaction.present()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
                  "statement-context acquisition requires an engine-issued selector");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!UuidPresent(session_uuid) || !UuidPresent(connection_uuid) ||
      !UuidPresent(TextToUuid(transaction.transaction_uuid))) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_IDENTITY_INVALID",
                  "statement-context acquisition requires canonical nonzero UUID identities");
    return result;
  }
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageAcquireStatementContextRequest,
                     kSchemaAcquireStatementContextRequestV1,
                     session_uuid,
                     connection_uuid),
          EncodeAcquireStatementContextPayloadV1(session, transaction),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type !=
          kMessageAcquireStatementContextResult ||
      response.header.schema_id != kSchemaAcquireStatementContextResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(
          &messages,
          "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_SCHEMA_MISMATCH",
          "The server did not return the statement-context V1 result schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeAcquireStatementContextPayloadV1(response.payload,
                                               &result.context) ||
      result.context.transaction.local_transaction_id !=
          transaction.local_transaction_id ||
      result.context.transaction.transaction_uuid !=
          transaction.transaction_uuid) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_INVALID",
                  "The engine-issued statement context did not match the requested transaction.");
    result.context = {};
    return result;
  }
  result.accepted = true;
  return result;
}

ServerStatementContextResult SbpsClient::AcquireNativeStatementContext(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) const {
  ServerStatementContextResult result;
  if (!session.authenticated) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.AUTH.REQUIRED",
                  "native statement-context acquisition requires an authenticated server session");
    return result;
  }
  if (!session.transaction_routing_v2_negotiated ||
      !session.relation_descriptor_projection_v3_negotiated ||
      !transaction.present()) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.NATIVE_STATEMENT_CONTEXT_CAPABILITY_REQUIRED",
        "native statement-context acquisition requires the negotiated exact-transaction and relation-descriptor capabilities");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!UuidPresent(session_uuid) || !UuidPresent(connection_uuid) ||
      !UuidPresent(TextToUuid(transaction.transaction_uuid))) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.STATEMENT_CONTEXT_IDENTITY_INVALID",
        "native statement-context acquisition requires canonical nonzero UUID identities");
    return result;
  }

  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageAcquireStatementContextRequest,
                     kSchemaAcquireStatementContextRequestV11,
                     session_uuid,
                     connection_uuid),
          EncodeAcquireStatementContextPayloadV11(session, transaction),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type !=
          kMessageAcquireStatementContextResult ||
      response.header.schema_id != kSchemaAcquireStatementContextResultV11 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(
          &messages,
          "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_SCHEMA_MISMATCH",
          "The server did not return the native statement-context V11 result schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  const bool decoded_native_context =
      DecodeAcquireStatementContextPayloadV11(response.payload, &result.context);
  if (!decoded_native_context ||
      result.context.transaction.local_transaction_id !=
          transaction.local_transaction_id ||
      result.context.transaction.transaction_uuid !=
          transaction.transaction_uuid) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_INVALID",
        "The engine-issued native statement context was malformed or did not match the requested transaction (payload_bytes=" +
            std::to_string(response.payload.size()) + ", extension=" +
            std::to_string(result.context.preliminary_extension_version) +
            ", requested_local=" + std::to_string(transaction.local_transaction_id) +
            ", returned_local=" + std::to_string(result.context.transaction.local_transaction_id) +
            ", uuid_equal=" +
            (result.context.transaction.transaction_uuid == transaction.transaction_uuid ? "true" : "false") + ").");
    result.context = {};
    return result;
  }
  result.accepted = true;
  return result;
}

ServerParameterCoordinationResult
SbpsClient::BeginParameterExecutionCoordination(
    const ParserSessionContext& session,
    ParameterExecutionMode mode,
    std::string_view operation_uuid,
    std::string_view public_prepared_uuid,
    std::string_view public_dynamic_package_uuid) const {
  ServerParameterCoordinationResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  const auto operation = TextToUuid(operation_uuid);
  const auto prepared = TextToUuid(public_prepared_uuid);
  const auto dynamic = TextToUuid(public_dynamic_package_uuid);
  const auto mode_code = static_cast<std::uint8_t>(mode);
  const bool prepared_present = UuidPresent(prepared);
  const bool dynamic_present = UuidPresent(dynamic);
  const bool matrix_valid =
      (mode == ParameterExecutionMode::kDirect && !prepared_present &&
       !dynamic_present) ||
      (mode == ParameterExecutionMode::kPrepared && !dynamic_present) ||
      (mode == ParameterExecutionMode::kBatch && !dynamic_present) ||
      (mode == ParameterExecutionMode::kDynamic && !prepared_present &&
       dynamic_present);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || !UuidPresent(operation) ||
      mode_code > static_cast<std::uint8_t>(ParameterExecutionMode::kDynamic) ||
      !matrix_valid) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "The parameter execution coordination request is malformed.");
    return result;
  }
  std::vector<std::uint8_t> payload;
  PutU16(&payload, 1);
  PutU8(&payload, mode_code);
  PutU8(&payload, 0);
  PutUuid(&payload, session_uuid);
  PutUuid(&payload, operation);
  PutUuid(&payload, prepared);
  PutUuid(&payload, dynamic);
  PutU32(&payload, 0);
  Frame response;
  MessageVectorSet messages;
  if (payload.size() != 72 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageBeginParameterCoordinationRequest,
                              kSchemaBeginParameterCoordinationRequestV1,
                              session_uuid, connection_uuid),
                   payload, &response, &messages, ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type !=
          kMessageBeginParameterCoordinationResult ||
      response.header.schema_id !=
          kSchemaBeginParameterCoordinationResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (response.payload.size() != 48 || GetU16(response.payload, 0) != 1 ||
      response.payload[2] != mode_code || response.payload[3] != 0 ||
      GetUuid(response.payload, 20) != operation ||
      GetU64(response.payload, 36) == 0 ||
      GetU32(response.payload, 44) != 0 ||
      !UuidPresent(GetUuid(response.payload, 4))) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PARAMETER_COORDINATION_RESULT_INVALID",
                  "The parameter execution coordination result was malformed or mismatched.");
    return result;
  }
  result.coordination.mode = mode;
  result.coordination.public_coordination_uuid =
      UuidToText(GetUuid(response.payload, 4));
  result.coordination.operation_uuid = UuidToText(operation);
  result.coordination.coordinator_generation = GetU64(response.payload, 36);
  result.accepted = true;
  return result;
}

ServerStatementContextResult SbpsClient::AcquireParameterStatementContext(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction,
    const ParameterExecutionCoordination& coordination) const {
  ServerStatementContextResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  const auto coordination_uuid =
      TextToUuid(coordination.public_coordination_uuid);
  const auto operation_uuid = TextToUuid(coordination.operation_uuid);
  const auto mode_code = static_cast<std::uint8_t>(coordination.mode);
  if (!session.authenticated || !transaction.present() ||
      !coordination.present() || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || !UuidPresent(coordination_uuid) ||
      !UuidPresent(operation_uuid) ||
      mode_code > static_cast<std::uint8_t>(ParameterExecutionMode::kDynamic)) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "The parameter statement-context selection is malformed.");
    return result;
  }
  auto payload = EncodeAcquireStatementContextPayloadV11(session, transaction);
  PutU16(&payload, 1);
  PutU8(&payload, mode_code);
  PutU8(&payload, 0);
  PutUuid(&payload, coordination_uuid);
  PutUuid(&payload, operation_uuid);
  Frame response;
  MessageVectorSet messages;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageAcquireStatementContextRequest,
                              kSchemaAcquireParameterStatementContextRequestV1,
                              session_uuid, connection_uuid),
                   payload, &response, &messages, ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageAcquireStatementContextResult ||
      response.header.schema_id != kSchemaAcquireStatementContextResultV11 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeAcquireStatementContextPayloadV11(response.payload,
                                               &result.context) ||
      result.context.preliminary_extension_version != 3 ||
      result.context.transaction.local_transaction_id !=
          transaction.local_transaction_id ||
      result.context.transaction.transaction_uuid !=
          transaction.transaction_uuid) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_INVALID",
                  "The coordinated parameter statement context was malformed or mismatched.");
    result.context = {};
    return result;
  }
  result.accepted = true;
  return result;
}

ServerLiteralBindingResult SbpsClient::NegotiateLiteralDescriptors(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbln) const {
  ServerLiteralBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sbln.size() < 128 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageNegotiateLiteralDescriptorsRequest,
                              kSchemaNegotiateLiteralDescriptorsRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbln, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type !=
          kMessageNegotiateLiteralDescriptorsResult ||
      response.header.schema_id !=
          kSchemaNegotiateLiteralDescriptorsResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerLiteralBindingResult SbpsClient::FinalizeLiteralBinding(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sblf) const {
  ServerLiteralBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sblf.size() < 208 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageFinalizeLiteralBindingRequest,
                              kSchemaFinalizeLiteralBindingRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sblf, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageFinalizeLiteralBindingResult ||
      response.header.schema_id != kSchemaFinalizeLiteralBindingResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerParameterBindingResult SbpsClient::NegotiateParameterDescriptors(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbpr) const {
  ServerParameterBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sbpr.size() < 136 ||
      canonical_sbpr.size() > 98416 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageNegotiateParameterDescriptorsRequest,
                              kSchemaNegotiateParameterDescriptorsRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbpr, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type !=
          kMessageNegotiateParameterDescriptorsResult ||
      response.header.schema_id !=
          kSchemaNegotiateParameterDescriptorsResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerParameterBindingResult SbpsClient::FinalizeParameterBinding(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbpf) const {
  ServerParameterBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sbpf.size() < 280 ||
      canonical_sbpf.size() > 426192 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageFinalizeParameterBindingRequest,
                              kSchemaFinalizeParameterBindingRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbpf, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageFinalizeParameterBindingResult ||
      response.header.schema_id != kSchemaFinalizeParameterBindingResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::BeginVariableFrame(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbvb) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sbvb.size() < 144 ||
      canonical_sbvb.size() > 196704 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageBeginVariableFrameRequest,
                              kSchemaBeginVariableFrameRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbvb, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageBeginVariableFrameResult ||
      response.header.schema_id != kSchemaBeginVariableFrameResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerStatementContextResult SbpsClient::AcquireVariableStatementContext(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction,
    const VariableFrameCoordination& coordination) const {
  ServerStatementContextResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  const auto coordination_uuid = TextToUuid(coordination.public_coordination_uuid);
  const auto operation_uuid = TextToUuid(coordination.operation_uuid);
  if (!session.authenticated || !transaction.present() ||
      !coordination.present() || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || !UuidPresent(coordination_uuid) ||
      !UuidPresent(operation_uuid)) {
    AddDiagnostic(&result.messages, "SBLR.OPERAND_INVALID",
                  "The variable statement-context selection is malformed.");
    return result;
  }
  auto payload = EncodeAcquireStatementContextPayloadV11(session, transaction);
  PutU16(&payload, 1);
  PutU8(&payload, 1);
  PutU8(&payload, 0);
  PutUuid(&payload, coordination_uuid);
  PutUuid(&payload, operation_uuid);
  PutU64(&payload, coordination.coordinator_generation);
  Frame response;
  MessageVectorSet messages;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageAcquireStatementContextRequest,
                              kSchemaAcquireVariableStatementContextRequestV1,
                              session_uuid, connection_uuid),
                   payload, &response, &messages, ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageAcquireStatementContextResult ||
      response.header.schema_id != kSchemaAcquireStatementContextResultV11 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeAcquireStatementContextPayloadV11(response.payload,
                                               &result.context) ||
      result.context.preliminary_extension_version != 4 ||
      result.context.transaction.transaction_uuid != transaction.transaction_uuid) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_INVALID",
                  "The engine-issued variable statement context was malformed or mismatched.");
    result.context = {};
    return result;
  }
  result.accepted = true;
  return result;
}

ServerVariableBindingResult SbpsClient::NegotiateVariableDescriptors(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbvr) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sbvr.size() < 160 ||
      canonical_sbvr.size() > 131200 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageNegotiateVariableDescriptorsRequest,
                              kSchemaNegotiateVariableDescriptorsRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbvr, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageNegotiateVariableDescriptorsResult ||
      response.header.schema_id != kSchemaNegotiateVariableDescriptorsResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::FinalizeVariableBinding(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbvf) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sbvf.size() < 352 ||
      canonical_sbvf.size() > 655584 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageFinalizeVariableBindingRequest,
                              kSchemaFinalizeVariableBindingRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbvf, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageFinalizeVariableBindingResult ||
      response.header.schema_id != kSchemaFinalizeVariableBindingResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::AssignVariableValues(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbvy) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sbvy.size() < 296 ||
      canonical_sbvy.size() > 1048576 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageAssignVariableValuesRequest,
                              kSchemaAssignVariableValuesRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbvy, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageAssignVariableValuesResult ||
      response.header.schema_id != kSchemaAssignVariableValuesResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::CloseVariableFrame(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_sbvx) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_sbvx.size() != 64 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageCloseVariableFrameRequest,
                              kSchemaCloseVariableFrameRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbvx, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageCloseVariableFrameResult ||
      response.header.schema_id != kSchemaCloseVariableFrameResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::IssueSourceMapDescriptor(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_smrq) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_smrq.size() < 312 ||
      canonical_smrq.size() > 524496 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageIssueSourceMapRequest,
                              kSchemaIssueSourceMapRequestV1,
                              session_uuid, connection_uuid),
                   canonical_smrq, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageIssueSourceMapResult ||
      response.header.schema_id != kSchemaIssueSourceMapResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::IssueErrorVectorDescriptor(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_evrq) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_evrq.size() < 248 ||
      canonical_evrq.size() > 524408 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageIssueErrorVectorRequest,
                              kSchemaIssueErrorVectorRequestV1,
                              session_uuid, connection_uuid),
                   canonical_evrq, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageIssueErrorVectorResult ||
      response.header.schema_id != kSchemaIssueErrorVectorResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::CoordinateSavepoint(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_spcr) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_spcr.size() != 128 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageCoordinateSavepointRequest,
                              kSchemaCoordinateSavepointRequestV1,
                              session_uuid, connection_uuid),
                   canonical_spcr, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageCoordinateSavepointResult ||
      response.header.schema_id != kSchemaCoordinateSavepointResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::CoordinateAutonomousFrame(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& canonical_afcr) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || canonical_afcr.size() != 224 ||
      !SendRequest(endpoint_, BaseHeader(kMessageCoordinateAutonomousFrameRequest,
          kSchemaCoordinateAutonomousFrameRequestV1, session_uuid, connection_uuid),
          canonical_afcr, &response, &messages, ActiveSocketCacheKey())) {
    result.messages = std::move(messages); return result;
  }
  if (response.header.message_type != kMessageCoordinateAutonomousFrameResult ||
      response.header.schema_id != kSchemaCoordinateAutonomousFrameResultV1 ||
      response.payload.size() != 324 || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages); result.messages = std::move(messages); return result;
  }
  result.accepted = true; result.canonical_payload = std::move(response.payload); return result;
}
ServerVariableBindingResult SbpsClient::CoordinateReservationRelease(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult r;MessageVectorSet m;Frame response;auto su=TextToUuid(session.session_uuid),cu=TextToUuid(session.connection_uuid);if(!session.authenticated||!UuidPresent(su)||!UuidPresent(cu)||payload.size()!=80||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateReservationReleaseRequest,kSchemaCoordinateReservationReleaseRequestV1,su,cu),payload,&response,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;}if(response.header.message_type!=kMessageCoordinateReservationReleaseResult||response.header.schema_id!=kSchemaCoordinateReservationReleaseResultV1||response.payload.size()!=144||IsErrorFrame(response)){AddFrameDiagnostics(response,&m);r.messages=std::move(m);return r;}r.accepted=true;r.canonical_payload=std::move(response.payload);return r;}
ServerVariableBindingResult SbpsClient::CoordinateTemporaryInstanceCleanup(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult r;MessageVectorSet m;Frame response;auto su=TextToUuid(session.session_uuid),cu=TextToUuid(session.connection_uuid);if(!session.authenticated||!UuidPresent(su)||!UuidPresent(cu)||payload.size()!=88||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateTemporaryInstanceCleanupRequest,kSchemaCoordinateTemporaryInstanceCleanupRequestV1,su,cu),payload,&response,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;}if(response.header.message_type!=kMessageCoordinateTemporaryInstanceCleanupResult||response.header.schema_id!=kSchemaCoordinateTemporaryInstanceCleanupResultV1||response.payload.size()!=184||IsErrorFrame(response)){AddFrameDiagnostics(response,&m);r.messages=std::move(m);return r;}r.accepted=true;r.canonical_payload=std::move(response.payload);return r;}

ServerPreparedParameterFinalizeResult
SbpsClient::FinalizePreparedParameterSubmission(
    const ParserSessionContext& session,
    const ParameterExecutionCoordination& coordination,
    const std::vector<std::uint8_t>& canonical_sbpt,
    const ParserStatementContext& preliminary_context) const {
  ServerPreparedParameterFinalizeResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  const auto coordination_uuid =
      TextToUuid(coordination.public_coordination_uuid);
  const auto operation_uuid = TextToUuid(coordination.operation_uuid);
  const auto provisional_prepared =
      TextToUuid(preliminary_context.preliminary_prepared_statement_uuid);
  if (!session.authenticated ||
      coordination.mode != ParameterExecutionMode::kPrepared ||
      !coordination.present() || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || !UuidPresent(coordination_uuid) ||
      !UuidPresent(operation_uuid) ||
      preliminary_context.preliminary_extension_version != 3 ||
      !UuidPresent(provisional_prepared) ||
      preliminary_context.preliminary_prepared_generation == 0 ||
      canonical_sbpt.size() < 280 + 192 ||
      canonical_sbpt.size() > std::numeric_limits<std::uint32_t>::max() ||
      canonical_sbpt[0] != 'S' || canonical_sbpt[1] != 'B' ||
      canonical_sbpt[2] != 'P' || canonical_sbpt[3] != 'T' ||
      GetU16(canonical_sbpt, 4) != 1 || GetU16(canonical_sbpt, 6) != 280 ||
      GetU32(canonical_sbpt, 8) != canonical_sbpt.size() ||
      GetU32(canonical_sbpt, 12) != 0 ||
      GetUuid(canonical_sbpt, 16) != coordination_uuid ||
      GetUuid(canonical_sbpt, 32) != operation_uuid ||
      GetUuid(canonical_sbpt, 48) != provisional_prepared ||
      GetU64(canonical_sbpt, 64) !=
          preliminary_context.preliminary_prepared_generation ||
      GetU32(canonical_sbpt, 96) == 0 ||
      GetU32(canonical_sbpt, 100) != 192 ||
      static_cast<std::uint64_t>(280) + GetU32(canonical_sbpt, 96) +
              GetU32(canonical_sbpt, 100) !=
          canonical_sbpt.size()) {
    AddDiagnostic(&result.messages,
                  "SBLR.OPERAND_INVALID",
                  "The prepared parameter finalization request is malformed.");
    return result;
  }
  Frame response;
  MessageVectorSet messages;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessagePrepareSblr,
                              kSchemaFinalizePreparedParameterRequestV1,
                              session_uuid, connection_uuid),
                   canonical_sbpt, &response, &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessagePrepareResult ||
      response.header.schema_id != kSchemaFinalizePreparedParameterResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (response.payload.size() != 56 || GetU16(response.payload, 0) != 1 ||
      GetU16(response.payload, 2) != 0 ||
      !UuidPresent(GetUuid(response.payload, 4)) ||
      GetUuid(response.payload, 4) != provisional_prepared ||
      GetU64(response.payload, 20) !=
          preliminary_context.preliminary_prepared_generation ||
      GetUuid(response.payload, 28) != operation_uuid ||
      GetU64(response.payload, 44) <= coordination.coordinator_generation ||
      GetU32(response.payload, 52) != 0) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.PREPARED_PARAMETER_FINALIZE_RESULT_INVALID",
        "The prepared parameter reference did not exactly seal the provisional engine binding.");
    return result;
  }
  result.prepared.prepared_statement_uuid =
      UuidToText(GetUuid(response.payload, 4));
  result.prepared.prepared_generation = GetU64(response.payload, 20);
  result.prepared.operation_uuid = UuidToText(operation_uuid);
  result.prepared.coordination_generation = GetU64(response.payload, 44);
  result.accepted = true;
  return result;
}

ServerExecutionResult SbpsClient::ExecuteSblr(const ParserSessionContext& session,
                                             std::string_view encoded_sblr_envelope,
                                             bool cursor_requested) const {
  return ExecuteSblrWithDataPacket(session, encoded_sblr_envelope, {}, cursor_requested);
}

ServerExecutionResult SbpsClient::ExecuteSblrRouted(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope,
    const ParserTransactionRouting& transaction,
    bool cursor_requested) const {
  return ExecuteSblrWithDataPacketRouted(session,
                                         encoded_sblr_envelope,
                                         {},
                                         transaction,
                                         cursor_requested);
}

ServerExecutionResult SbpsClient::ExecuteSblrWithDataPacket(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) const {
  ServerExecutionResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageExecuteSblr,
                              kSchemaExecuteSblrV1,
                              session_uuid,
                              connection_uuid),
                   EncodeExecutePayload(session_uuid,
                                        encoded_sblr_envelope,
                                        cursor_requested,
                                        data_packet),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || outcome != "accepted") {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_REJECTED", "The server rejected SBLR execution.");
    result.messages = std::move(messages);
    return result;
  }
  if (offset + 16 + 16 + 8 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "The server execute result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  offset += 16; // server request UUID
  result.cursor_uuid = OptionalUuidToText(GetUuid(response.payload, offset));
  offset += 16;
  result.row_count = GetU64(response.payload, offset);
  offset += 8;
  if (!ReadString(response.payload, &offset, &result.operation_id) ||
      !ReadString(response.payload, &offset, &result.row_packet)) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "The server execute result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  PopulateTransactionStateFromPayload(result.row_packet, &result);
  result.accepted = true;
  if (ExecutionInvalidatesPublicResolutionCache(result.operation_id)) {
    ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  }
  return result;
}

ServerExecutionResult SbpsClient::ExecuteSblrWithDataPacketRouted(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    const ParserTransactionRouting& transaction,
    bool cursor_requested) const {
  ServerExecutionResult result;
  MessageVectorSet messages;
  if (!RequireTransactionRoutingV2(session, &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (!ValidateTransactionRouting(transaction, &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageExecuteSblr,
                              kSchemaExecuteSblrV2,
                              session_uuid,
                              connection_uuid),
                   EncodeExecutePayloadV2(session_uuid,
                                          {},
                                          encoded_sblr_envelope,
                                          cursor_requested,
                                          data_packet,
                                          transaction),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectV2TransportOutcomeUnknown(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult) {
    AddFrameDiagnostics(response, &messages);
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server returned the wrong message type for V2 execution.");
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "unexpected_response_type");
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.schema_id != kSchemaExecuteResultV2) {
    if (IsErrorFrame(response)) AddFrameDiagnostics(response, &messages);
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_SCHEMA_MISMATCH",
                  "The server did not return the required V2 execute-result schema.");
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "unexpected_response_schema");
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeExecuteResultPayloadV2(response, &result, &messages)) {
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "malformed_typed_response");
    result.messages = std::move(messages);
    return result;
  }
  result.messages = std::move(messages);
  if (result.catalog_invalidation_applied ||
      ((result.accepted ||
        result.finality_state == ParserTransactionFinality::kKnownApplied) &&
       ExecutionInvalidatesPublicResolutionCache(result.operation_id))) {
    ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  }
  return result;
}

ServerExecutionResult SbpsClient::ExecuteCanonicalSblrWithDataPacket(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) const {
  ServerExecutionResult result;
  MessageVectorSet messages;
  if (!RequireTransactionRoutingV2(session, &messages) ||
      !statement_context.complete() || !submission.complete() ||
      submission.statement_uuid != statement_context.statement_uuid ||
      (submission.variable_finalized() &&
       (submission.literal_finalized() || submission.parameter_finalized()))) {
    if (messages.diagnostics.empty()) {
      AddDiagnostic(&messages,
                    "PARSER_SERVER_IPC.CANONICAL_STATEMENT_CONTEXT_INVALID",
                    "Canonical execution requires the exact acquired statement and active transaction context.");
    }
    result.messages = std::move(messages);
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!UuidPresent(session_uuid) || !UuidPresent(connection_uuid) ||
      !UuidPresent(TextToUuid(statement_context.statement_uuid)) ||
      !UuidPresent(TextToUuid(statement_context.transaction.transaction_uuid))) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.CANONICAL_STATEMENT_CONTEXT_INVALID",
                  "Canonical execution identities must be canonical nonzero UUIDs.");
    result.messages = std::move(messages);
    return result;
  }
  const auto request_payload =
      submission.variable_finalized()
          ? EncodeCanonicalExecuteVariablePayloadV1(
                session, statement_context, submission, data_packet,
                cursor_requested)
          : submission.parameter_finalized()
          ? EncodeCanonicalExecuteParameterPayloadV1(
                session, statement_context, submission, data_packet,
                cursor_requested)
          : (submission.literal_finalized()
                 ? EncodeCanonicalExecuteLiteralPayloadV1(
                       session, statement_context, submission, data_packet,
                       cursor_requested)
                 : EncodeCanonicalExecutePayloadV1(
                       session, statement_context, submission, data_packet,
                       cursor_requested));
  if (submission.parameter_finalized() && request_payload.size() > 33554432) {
    AddDiagnostic(&messages, "RESOURCE.BUDGET_EXCEEDED",
                  "Canonical parameter execution exceeds the admitted combined transport maximum.");
    result.messages = std::move(messages);
    return result;
  }
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageExecuteSblr,
                     submission.variable_finalized()
                         ? kSchemaExecuteCanonicalSblrVariableV1
                         : submission.parameter_finalized()
                         ? kSchemaExecuteCanonicalSblrParameterV1
                         : (submission.literal_finalized()
                                ? kSchemaExecuteCanonicalSblrLiteralV1
                                : kSchemaExecuteCanonicalSblrV1),
                     session_uuid,
                     connection_uuid),
          request_payload,
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    ProjectV2TransportOutcomeUnknown(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult ||
      response.header.schema_id != kSchemaExecuteResultV2) {
    AddFrameDiagnostics(response, &messages);
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_SCHEMA_MISMATCH",
                  "The server did not return the canonical-route execute result schema.");
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "unexpected_canonical_response");
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeExecuteResultPayloadV2(response, &result, &messages)) {
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "malformed_canonical_response");
  }
  result.messages = std::move(messages);
  return result;
}

ServerPrepareSblrResult SbpsClient::PrepareSblr(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope) const {
  ServerPrepareSblrResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessagePrepareSblr,
                              kSchemaPrepareSblrV1,
                              session_uuid,
                              connection_uuid),
                   EncodePreparePayload(session, session_uuid, encoded_sblr_envelope),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessagePrepareResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || outcome != "accepted") {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.PREPARE_REJECTED", "The server rejected SBLR prepare.");
    result.messages = std::move(messages);
    return result;
  }
  if (offset + 16 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID", "The server prepare result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.prepared_statement_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  if (!ReadString(response.payload, &offset, &result.operation_id) ||
      !ReadString(response.payload, &offset, &result.detail)) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID", "The server prepare result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  return result;
}

ServerPrepareSblrResult SbpsClient::PrepareSblrRouted(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope,
    const ParserTransactionSelector& transaction) const {
  ServerPrepareSblrResult result;
  if (!RequireTransactionRoutingV2(session, &result.messages)) {
    return result;
  }
  if (!transaction.present()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
                  "V2 prepare requires an engine-issued transaction selector.");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessagePrepareSblr,
                              kSchemaPrepareSblrV2,
                              session_uuid,
                              connection_uuid),
                   EncodePreparePayloadV2(session,
                                          session_uuid,
                                          encoded_sblr_envelope,
                                          transaction),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectV2PrepareTransportOutcomeUnknown(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessagePrepareResult ||
      response.header.schema_id != kSchemaPrepareResultV2) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_SCHEMA_MISMATCH",
                  "The server did not return the required V2 prepare-result schema.");
    ProjectV2PrepareOutcomeUnknown(
        &messages, &result, "unexpected_response_type_or_schema");
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodePrepareResultPayloadV2(response.payload, &result, &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  result.messages = std::move(messages);
  return result;
}

ServerExecutionResult SbpsClient::ExecutePreparedSblr(
    const ParserSessionContext& session,
    std::string_view prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) const {
  ServerExecutionResult result;
  if (prepared_statement_uuid.empty()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PREPARED_HANDLE_REQUIRED",
                  "Prepared SBLR execution requires a prepared statement UUID.");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageExecuteSblr,
                              kSchemaExecuteSblrV1,
                              session_uuid,
                              connection_uuid),
                   EncodeExecutePreparedPayload(session_uuid,
                                                TextToUuid(prepared_statement_uuid),
                                                encoded_sblr_envelope,
                                                cursor_requested,
                                                data_packet),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || outcome != "accepted") {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_REJECTED", "The server rejected prepared SBLR execution.");
    result.messages = std::move(messages);
    return result;
  }
  if (offset + 16 + 16 + 8 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "The server execute result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  offset += 16; // server request UUID
  result.cursor_uuid = OptionalUuidToText(GetUuid(response.payload, offset));
  offset += 16;
  result.row_count = GetU64(response.payload, offset);
  offset += 8;
  if (!ReadString(response.payload, &offset, &result.operation_id) ||
      !ReadString(response.payload, &offset, &result.row_packet)) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "The server execute result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  PopulateTransactionStateFromPayload(result.row_packet, &result);
  result.accepted = true;
  if (ExecutionInvalidatesPublicResolutionCache(result.operation_id)) {
    ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  }
  return result;
}

ServerExecutionResult SbpsClient::ExecutePreparedSblrRouted(
    const ParserSessionContext& session,
    std::string_view prepared_statement_uuid,
    const ParserTransactionSelector& transaction,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) const {
  ServerExecutionResult result;
  if (!RequireTransactionRoutingV2(session, &result.messages)) {
    return result;
  }
  if (prepared_statement_uuid.empty()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PREPARED_HANDLE_REQUIRED",
                  "Prepared SBLR execution requires a prepared statement UUID.");
    return result;
  }
  ParserTransactionRouting routing;
  routing.route = ParserTransactionRoute::kSelected;
  routing.selector = transaction;
  MessageVectorSet messages;
  if (!ValidateTransactionRouting(routing, &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageExecuteSblr,
                              kSchemaExecuteSblrV2,
                              session_uuid,
                              connection_uuid),
                   EncodeExecutePayloadV2(session_uuid,
                                          TextToUuid(prepared_statement_uuid),
                                          encoded_sblr_envelope,
                                          cursor_requested,
                                          data_packet,
                                          routing),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectV2TransportOutcomeUnknown(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult ||
      response.header.schema_id != kSchemaExecuteResultV2) {
    if (IsErrorFrame(response)) AddFrameDiagnostics(response, &messages);
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_SCHEMA_MISMATCH",
                  "The server did not return the required V2 execute-result schema.");
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "unexpected_response_type_or_schema");
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeExecuteResultPayloadV2(response, &result, &messages)) {
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "malformed_typed_response");
    result.messages = std::move(messages);
    return result;
  }
  result.messages = std::move(messages);
  if (result.catalog_invalidation_applied ||
      ((result.accepted ||
        result.finality_state == ParserTransactionFinality::kKnownApplied) &&
       ExecutionInvalidatesPublicResolutionCache(result.operation_id))) {
    ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  }
  return result;
}

ServerClosePreparedSblrResult SbpsClient::ClosePreparedSblr(
    const ParserSessionContext& session,
    std::string_view prepared_statement_uuid) const {
  ServerClosePreparedSblrResult result;
  if (!IsCanonicalNonzeroUuidText(session.session_uuid)) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.SESSION_REQUIRED",
                  "Prepared SBLR close requires a canonical nonzero session UUID.");
    return result;
  }
  if (!IsCanonicalNonzeroUuidText(prepared_statement_uuid)) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PREPARED_HANDLE_REQUIRED",
                  "Prepared SBLR close requires a canonical nonzero prepared statement UUID.");
    return result;
  }

  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto prepared_uuid = TextToUuid(prepared_statement_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageClosePreparedSblr,
                              kSchemaClosePreparedSblrV1,
                              session_uuid,
                              connection_uuid),
                   EncodeClosePreparedSblrPayload(session_uuid,
                                                  prepared_uuid),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectCloseTransportFailure(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageClosePreparedSblrResult ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(&messages,
                    "PARSER_SERVER_IPC.CLOSE_PREPARED_RESULT_INVALID",
                    "The server returned the wrong message type for prepared SBLR close.");
    }
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.schema_id != kSchemaClosePreparedSblrResultV1) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.CLOSE_PREPARED_RESULT_SCHEMA_MISMATCH",
                  "The server returned the wrong schema for prepared SBLR close.");
    result.messages = std::move(messages);
    return result;
  }

  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) ||
      outcome != "accepted" || offset + 16 > response.payload.size()) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.CLOSE_PREPARED_RESULT_INVALID",
                  "The server prepared-close result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  const auto response_uuid = GetUuid(response.payload, offset);
  offset += 16;
  if (response_uuid != prepared_uuid ||
      !ReadString(response.payload, &offset, &result.detail) ||
      offset != response.payload.size()) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.CLOSE_PREPARED_RESULT_INVALID",
                  "The server prepared-close result did not echo the requested identity or contained trailing data.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.prepared_statement_uuid = UuidToText(response_uuid);
  result.messages = std::move(messages);
  return result;
}

ServerFetchResult SbpsClient::FetchCursor(const ParserSessionContext& session,
                                          std::string_view cursor_uuid,
                                          const CursorStreamDescriptorV1& stream_descriptor,
                                          std::uint64_t max_rows,
                                          std::uint64_t max_bytes,
                                          std::uint32_t fetch_flags) const {
  ServerFetchResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  if (!stream_descriptor.complete() ||
      stream_descriptor.cursor_uuid != cursor_uuid || max_rows == 0 ||
      max_bytes == 0 || max_rows > stream_descriptor.max_chunk_rows ||
      max_bytes > stream_descriptor.max_chunk_bytes) {
    AddDiagnostic(&messages,
                  "SERVER.STREAM.DESCRIPTOR_INVALID",
                  "Fetch requires the exact live cursor stream descriptor and bounded positive limits.");
    result.messages = std::move(messages);
    return result;
  }
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageFetch, kSchemaFetchV1, session_uuid, connection_uuid),
                   EncodeCursorPayload(session_uuid, cursor_uuid,
                                       &stream_descriptor, max_rows,
                                       max_bytes, fetch_flags),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageFetchResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (response.payload.size() < 16 + 8) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.FETCH_RESULT_INVALID", "The server fetch result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  result.cursor_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  result.row_count = GetU64(response.payload, offset);
  offset += 8;
  if (!ReadString(response.payload, &offset, &result.row_packet) || offset >= response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.FETCH_RESULT_INVALID", "The server fetch result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.end_of_cursor = response.payload[offset++] != 0;
  if (!ReadString(response.payload, &offset, &result.detail)) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.FETCH_RESULT_INVALID", "The server fetch result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  return result;
}

ServerCloseCursorResult SbpsClient::CloseCursor(const ParserSessionContext& session,
                                                std::string_view cursor_uuid) const {
  ServerCloseCursorResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageCloseCursor,
                              kSchemaCloseCursorV1,
                              session_uuid,
                              connection_uuid),
                   EncodeCursorPayload(session_uuid, cursor_uuid, nullptr, 1, 0, 0),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectCloseTransportFailure(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageCloseCursorResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || offset + 16 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.CLOSE_CURSOR_RESULT_INVALID", "The server close-cursor result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = outcome == "accepted";
  result.cursor_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  (void)ReadString(response.payload, &offset, &result.detail);
  result.messages = std::move(messages);
  return result;
}

ServerCloseCursorResult SbpsClient::CancelCursor(const ParserSessionContext& session,
                                                 std::string_view cursor_uuid) const {
  ServerCloseCursorResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageCloseCursor,
                              kSchemaCloseCursorV1,
                              session_uuid,
                              connection_uuid),
                   EncodeCursorPayload(session_uuid, cursor_uuid, nullptr, 1, 0, kCursorCloseFlagCancel),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectCloseTransportFailure(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageCloseCursorResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || offset + 16 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.CLOSE_CURSOR_RESULT_INVALID", "The server close-cursor result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = outcome == "accepted";
  result.cursor_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  (void)ReadString(response.payload, &offset, &result.detail);
  result.messages = std::move(messages);
  return result;
}

ServerManagementResult SbpsClient::Manage(const ParserSessionContext& session,
                                          std::string_view operation_key,
                                          std::string_view target_uuid,
                                          std::string_view mode,
                                          std::string_view audit_reason,
                                          std::uint64_t timeout_ms,
                                          bool include_history) const {
  ServerManagementResult result;
  result.operation_key = std::string(operation_key);
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.AUTH.REQUIRED",
        "ERROR",
        "server management requests require an authenticated server session",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageManagementRequest,
                              kSchemaManagementRequestV1,
                              session_uuid,
                              connection_uuid),
                   EncodeManagementPayload(operation_key,
                                           target_uuid,
                                           mode,
                                           audit_reason,
                                           timeout_ms,
                                           include_history),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageManagementResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.schema_id != kSchemaManagementResponseV1) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.MANAGEMENT_RESULT_INVALID",
                  "The server management response schema is not supported.");
    result.messages = std::move(messages);
    return result;
  }
  result.payload.assign(reinterpret_cast<const char*>(response.payload.data()),
                        response.payload.size());
  result.accepted = true;
  ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  return result;
}

bool SbpsClient::DisconnectSession(const ParserSessionContext& session, MessageVectorSet* messages) const {
  if (!session.authenticated || session.session_uuid.empty()) return true;
  ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  std::vector<std::uint8_t> disconnect_payload;
  PutUuid(&disconnect_payload, session_uuid);
  PutString(&disconnect_payload, "parser_disconnect_notice");
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageDisconnectNotice, 0, session_uuid, connection_uuid),
                   disconnect_payload,
                   &response,
                   messages,
                   ActiveSocketCacheKey())) {
    return false;
  }
  if (response.header.message_type != kMessageDisconnectNotice || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, messages);
    return false;
  }
  return true;
}

ServerVariableBindingResult SbpsClient::CoordinateCursorOpen(
    const ParserSessionContext& session,
    const std::vector<std::uint8_t>& payload) const {
  ServerVariableBindingResult result;
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!session.authenticated || !UuidPresent(session_uuid) ||
      !UuidPresent(connection_uuid) || payload.size() != 64 ||
      !SendRequest(endpoint_,
                   BaseHeader(kMessageCoordinateCursorOpenRequest,
                              kSchemaCoordinateCursorOpenRequestV1,
                              session_uuid, connection_uuid),
                   payload, &response, &messages, ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageCoordinateCursorOpenResult ||
      response.header.schema_id != kSchemaCoordinateCursorOpenResultV1 ||
      response.payload.size() != 232 || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.canonical_payload = std::move(response.payload);
  return result;
}

ServerVariableBindingResult SbpsClient::CoordinateReadByKey(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto session_uuid=TextToUuid(session.session_uuid);const auto connection_uuid=TextToUuid(session.connection_uuid);if(!session.authenticated||!UuidPresent(session_uuid)||!UuidPresent(connection_uuid)||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateReadByKeyRequest,kSchemaCoordinateReadByKeyRequestV1,session_uuid,connection_uuid),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateReadByKeyResult||response.header.schema_id!=kSchemaCoordinateReadByKeyResultV1||response.payload.size()!=308||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateReadRange(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto session_uuid=TextToUuid(session.session_uuid);const auto connection_uuid=TextToUuid(session.connection_uuid);if(!session.authenticated||!UuidPresent(session_uuid)||!UuidPresent(connection_uuid)||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateReadRangeRequest,kSchemaCoordinateReadRangeRequestV1,session_uuid,connection_uuid),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateReadRangeResult||response.header.schema_id!=kSchemaCoordinateReadRangeResultV1||response.payload.size()!=404||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateReadStream(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateReadStreamRequest,kSchemaCoordinateReadStreamRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateReadStreamResult||response.header.schema_id!=kSchemaCoordinateReadStreamResultV1||response.payload.size()!=240||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateResultSetPass(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateResultSetPassRequest,kSchemaCoordinateResultSetPassRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateResultSetPassResult||response.header.schema_id!=kSchemaCoordinateResultSetPassResultV1||response.payload.size()!=288||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAccessCursorOpen(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAccessCursorOpenRequest,kSchemaCoordinateAccessCursorOpenRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAccessCursorOpenResult||response.header.schema_id!=kSchemaCoordinateAccessCursorOpenResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAccessCursorFetch(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAccessCursorFetchRequest,kSchemaCoordinateAccessCursorFetchRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAccessCursorFetchResult||response.header.schema_id!=kSchemaCoordinateAccessCursorFetchResultV1||response.payload.size()!=280||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAccessCursorClose(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAccessCursorCloseRequest,kSchemaCoordinateAccessCursorCloseRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAccessCursorCloseResult||response.header.schema_id!=kSchemaCoordinateAccessCursorCloseResultV1||response.payload.size()!=232||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateInsert(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateInsertRequest,kSchemaCoordinateInsertRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateInsertResult||response.header.schema_id!=kSchemaCoordinateInsertResultV1||response.payload.size()!=440||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateUpdate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateUpdateRequest,kSchemaCoordinateUpdateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateUpdateResult||response.header.schema_id!=kSchemaCoordinateUpdateResultV1||response.payload.size()!=456||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDelete(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDeleteRequest,kSchemaCoordinateDeleteRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDeleteResult||response.header.schema_id!=kSchemaCoordinateDeleteResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateMerge(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateMergeRequest,kSchemaCoordinateMergeRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateMergeResult||response.header.schema_id!=kSchemaCoordinateMergeResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateTableTruncate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateTableTruncateRequest,kSchemaCoordinateTableTruncateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateTableTruncateResult||response.header.schema_id!=kSchemaCoordinateTableTruncateResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateTableAnalyze(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateTableAnalyzeRequest,kSchemaCoordinateTableAnalyzeRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateTableAnalyzeResult||response.header.schema_id!=kSchemaCoordinateTableAnalyzeResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateBulkImportStream(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateBulkImportStreamRequest,kSchemaCoordinateBulkImportStreamRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateBulkImportStreamResult||response.header.schema_id!=kSchemaCoordinateBulkImportStreamResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateBulkExportStream(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateBulkExportStreamRequest,kSchemaCoordinateBulkExportStreamRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateBulkExportStreamResult||response.header.schema_id!=kSchemaCoordinateBulkExportStreamResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateStatementBatch(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateStatementBatchRequest,kSchemaCoordinateStatementBatchRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateStatementBatchResult||response.header.schema_id!=kSchemaCoordinateStatementBatchResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAtomicCas(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAtomicCasRequest,kSchemaCoordinateAtomicCasRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAtomicCasResult||response.header.schema_id!=kSchemaCoordinateAtomicCasResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAtomicRmw(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAtomicRmwRequest,kSchemaCoordinateAtomicRmwRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAtomicRmwResult||response.header.schema_id!=kSchemaCoordinateAtomicRmwResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAdvisoryLock(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAdvisoryLockRequest,kSchemaCoordinateAdvisoryLockRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAdvisoryLockResult||response.header.schema_id!=kSchemaCoordinateAdvisoryLockResultV1||response.payload.size()!=352||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAdvisoryLockRelease(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAdvisoryLockReleaseRequest,kSchemaCoordinateAdvisoryLockReleaseRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAdvisoryLockReleaseResult||response.header.schema_id!=kSchemaCoordinateAdvisoryLockReleaseResultV1||response.payload.size()!=256||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateFunctionCall(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateFunctionCallRequest,kSchemaCoordinateFunctionCallRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateFunctionCallResult||response.header.schema_id!=kSchemaCoordinateFunctionCallResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateOperatorCall(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateOperatorCallRequest,kSchemaCoordinateOperatorCallRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateOperatorCallResult||response.header.schema_id!=kSchemaCoordinateOperatorCallResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateCast(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateCastRequest,kSchemaCoordinateCastRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateCastResult||response.header.schema_id!=kSchemaCoordinateCastResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateCompare(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateCompareRequest,kSchemaCoordinateCompareRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateCompareResult||response.header.schema_id!=kSchemaCoordinateCompareResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDomainOperation(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDomainOperationRequest,kSchemaCoordinateDomainOperationRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDomainOperationResult||response.header.schema_id!=kSchemaCoordinateDomainOperationResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateUdrInvoke(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateUdrInvokeRequest,kSchemaCoordinateUdrInvokeRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateUdrInvokeResult||response.header.schema_id!=kSchemaCoordinateUdrInvokeResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateProcedureInvoke(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateProcedureInvokeRequest,kSchemaCoordinateProcedureInvokeRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateProcedureInvokeResult||response.header.schema_id!=kSchemaCoordinateProcedureInvokeResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateFunctionInvoke(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateFunctionInvokeRequest,kSchemaCoordinateFunctionInvokeRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateFunctionInvokeResult||response.header.schema_id!=kSchemaCoordinateFunctionInvokeResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAggregateInvoke(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAggregateInvokeRequest,kSchemaCoordinateAggregateInvokeRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAggregateInvokeResult||response.header.schema_id!=kSchemaCoordinateAggregateInvokeResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSequenceNextval(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSequenceNextvalRequest,kSchemaCoordinateSequenceNextvalRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSequenceNextvalResult||response.header.schema_id!=kSchemaCoordinateSequenceNextvalResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSequenceCurrval(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSequenceCurrvalRequest,kSchemaCoordinateSequenceCurrvalRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSequenceCurrvalResult||response.header.schema_id!=kSchemaCoordinateSequenceCurrvalResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSequenceSetval(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSequenceSetvalRequest,kSchemaCoordinateSequenceSetvalRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSequenceSetvalResult||response.header.schema_id!=kSchemaCoordinateSequenceSetvalResultV1||response.payload.size()!=424||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateQueryNumeric(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateQueryNumericRequest,kSchemaCoordinateQueryNumericRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateQueryNumericResult||response.header.schema_id!=kSchemaCoordinateQueryNumericResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAdvancedDatatypeFamily(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAdvancedDatatypeFamilyRequest,kSchemaCoordinateAdvancedDatatypeFamilyRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAdvancedDatatypeFamilyResult||response.header.schema_id!=kSchemaCoordinateAdvancedDatatypeFamilyResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateProject(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateProjectRequest,kSchemaCoordinateProjectRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateProjectResult||response.header.schema_id!=kSchemaCoordinateProjectResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateCatalogIntrospect(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateCatalogIntrospectRequest,kSchemaCoordinateCatalogIntrospectRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateCatalogIntrospectResult||response.header.schema_id!=kSchemaCoordinateCatalogIntrospectResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateKvStructuredRead(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateKvStructuredReadRequest,kSchemaCoordinateKvStructuredReadRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateKvStructuredReadResult||response.header.schema_id!=kSchemaCoordinateKvStructuredReadResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAggregate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAggregateRequest,kSchemaCoordinateAggregateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAggregateResult||response.header.schema_id!=kSchemaCoordinateAggregateResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateGroup(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateGroupRequest,kSchemaCoordinateGroupRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateGroupResult||response.header.schema_id!=kSchemaCoordinateGroupResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSort(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSortRequest,kSchemaCoordinateSortRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSortResult||response.header.schema_id!=kSchemaCoordinateSortResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateLimit(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateLimitRequest,kSchemaCoordinateLimitRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateLimitResult||response.header.schema_id!=kSchemaCoordinateLimitResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateWindow(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateWindowRequest,kSchemaCoordinateWindowRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateWindowResult||response.header.schema_id!=kSchemaCoordinateWindowResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateReturnResultSet(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateReturnResultSetRequest,kSchemaCoordinateReturnResultSetRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateReturnResultSetResult||response.header.schema_id!=kSchemaCoordinateReturnResultSetResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}

ServerVariableBindingResult SbpsClient::CoordinateKvStructuredMutate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateKvStructuredMutateRequest,kSchemaCoordinateKvStructuredMutateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateKvStructuredMutateResult||response.header.schema_id!=kSchemaCoordinateKvStructuredMutateResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateKvStructuredScan(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateKvStructuredScanRequest,kSchemaCoordinateKvStructuredScanRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateKvStructuredScanResult||response.header.schema_id!=kSchemaCoordinateKvStructuredScanResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}

ServerVariableBindingResult SbpsClient::CoordinateKvStructuredStreamRead(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateKvStructuredStreamReadRequest,kSchemaCoordinateKvStructuredStreamReadRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateKvStructuredStreamReadResult||response.header.schema_id!=kSchemaCoordinateKvStructuredStreamReadResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}

ServerVariableBindingResult SbpsClient::CoordinateKvStructuredStreamAppend(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateKvStructuredStreamAppendRequest,kSchemaCoordinateKvStructuredStreamAppendRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateKvStructuredStreamAppendResult||response.header.schema_id!=kSchemaCoordinateKvStructuredStreamAppendResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateKvStructuredTimeseries(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateKvStructuredTimeseriesRequest,kSchemaCoordinateKvStructuredTimeseriesRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateKvStructuredTimeseriesResult||response.header.schema_id!=kSchemaCoordinateKvStructuredTimeseriesResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSystemConfigSet(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSystemConfigSetRequest,kSchemaCoordinateSystemConfigSetRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSystemConfigSetResult||response.header.schema_id!=kSchemaCoordinateSystemConfigSetResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateDomain(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateDomainRequest,kSchemaCoordinateDdlCreateDomainRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateDomainResult||response.header.schema_id!=kSchemaCoordinateDdlCreateDomainResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateSchema(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateSchemaRequest,kSchemaCoordinateDdlCreateSchemaRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateSchemaResult||response.header.schema_id!=kSchemaCoordinateDdlCreateSchemaResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateTable(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateTableRequest,kSchemaCoordinateDdlCreateTableRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateTableResult||response.header.schema_id!=kSchemaCoordinateDdlCreateTableResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateIndex(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateIndexRequest,kSchemaCoordinateDdlCreateIndexRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateIndexResult||response.header.schema_id!=kSchemaCoordinateDdlCreateIndexResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropIndex(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropIndexRequest,kSchemaCoordinateDdlDropIndexRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropIndexResult||response.header.schema_id!=kSchemaCoordinateDdlDropIndexResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterDomain(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterDomainRequest,kSchemaCoordinateDdlAlterDomainRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterDomainResult||response.header.schema_id!=kSchemaCoordinateDdlAlterDomainResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}ServerVariableBindingResult SbpsClient::CoordinateDdlCreateView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateViewRequest,kSchemaCoordinateDdlCreateViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateViewResult||response.header.schema_id!=kSchemaCoordinateDdlCreateViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterViewRequest,kSchemaCoordinateDdlAlterViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterViewResult||response.header.schema_id!=kSchemaCoordinateDdlAlterViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}ServerVariableBindingResult SbpsClient::CoordinateDdlDropView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropViewRequest,kSchemaCoordinateDdlDropViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropViewResult||response.header.schema_id!=kSchemaCoordinateDdlDropViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}ServerVariableBindingResult SbpsClient::CoordinateDdlCreateTrigger(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateTriggerRequest,kSchemaCoordinateDdlCreateTriggerRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateTriggerResult||response.header.schema_id!=kSchemaCoordinateDdlCreateTriggerResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterTrigger(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterTriggerRequest,kSchemaCoordinateDdlAlterTriggerRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterTriggerResult||response.header.schema_id!=kSchemaCoordinateDdlAlterTriggerResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropTrigger(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropTriggerRequest,kSchemaCoordinateDdlDropTriggerRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropTriggerResult||response.header.schema_id!=kSchemaCoordinateDdlDropTriggerResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateProcedure(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateProcedureRequest,kSchemaCoordinateDdlCreateProcedureRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateProcedureResult||response.header.schema_id!=kSchemaCoordinateDdlCreateProcedureResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterProcedure(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterProcedureRequest,kSchemaCoordinateDdlAlterProcedureRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterProcedureResult||response.header.schema_id!=kSchemaCoordinateDdlAlterProcedureResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropProcedure(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropProcedureRequest,kSchemaCoordinateDdlDropProcedureRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropProcedureResult||response.header.schema_id!=kSchemaCoordinateDdlDropProcedureResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateFunction(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateFunctionRequest,kSchemaCoordinateDdlCreateFunctionRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateFunctionResult||response.header.schema_id!=kSchemaCoordinateDdlCreateFunctionResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterFunction(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterFunctionRequest,kSchemaCoordinateDdlAlterFunctionRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterFunctionResult||response.header.schema_id!=kSchemaCoordinateDdlAlterFunctionResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropFunction(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropFunctionRequest,kSchemaCoordinateDdlDropFunctionRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropFunctionResult||response.header.schema_id!=kSchemaCoordinateDdlDropFunctionResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreatePackage(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreatePackageRequest,kSchemaCoordinateDdlCreatePackageRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreatePackageResult||response.header.schema_id!=kSchemaCoordinateDdlCreatePackageResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropPackage(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropPackageRequest,kSchemaCoordinateDdlDropPackageRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropPackageResult||response.header.schema_id!=kSchemaCoordinateDdlDropPackageResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateTemporaryTable(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateTemporaryTableRequest,kSchemaCoordinateDdlCreateTemporaryTableRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateTemporaryTableResult||response.header.schema_id!=kSchemaCoordinateDdlCreateTemporaryTableResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropTemporaryTable(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropTemporaryTableRequest,kSchemaCoordinateDdlDropTemporaryTableRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropTemporaryTableResult||response.header.schema_id!=kSchemaCoordinateDdlDropTemporaryTableResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlRenameObjectVector(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlRenameObjectVectorRequest,kSchemaCoordinateDdlRenameObjectVectorRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlRenameObjectVectorResult||response.header.schema_id!=kSchemaCoordinateDdlRenameObjectVectorResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateOrReplaceSrs(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateOrReplaceSrsRequest,kSchemaCoordinateDdlCreateOrReplaceSrsRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateOrReplaceSrsResult||response.header.schema_id!=kSchemaCoordinateDdlCreateOrReplaceSrsResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropSrs(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropSrsRequest,kSchemaCoordinateDdlDropSrsRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropSrsResult||response.header.schema_id!=kSchemaCoordinateDdlDropSrsResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateRewriteRule(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateRewriteRuleRequest,kSchemaCoordinateDdlCreateRewriteRuleRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateRewriteRuleResult||response.header.schema_id!=kSchemaCoordinateDdlCreateRewriteRuleResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterRewriteRule(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterRewriteRuleRequest,kSchemaCoordinateDdlAlterRewriteRuleRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterRewriteRuleResult||response.header.schema_id!=kSchemaCoordinateDdlAlterRewriteRuleResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropRewriteRule(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropRewriteRuleRequest,kSchemaCoordinateDdlDropRewriteRuleRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropRewriteRuleResult||response.header.schema_id!=kSchemaCoordinateDdlDropRewriteRuleResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlValidateConstraint(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlValidateConstraintRequest,kSchemaCoordinateDdlValidateConstraintRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlValidateConstraintResult||response.header.schema_id!=kSchemaCoordinateDdlValidateConstraintResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSecurityCreatePrivilegeTemplate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityCreatePrivilegeTemplateRequest,kSchemaCoordinateSecurityCreatePrivilegeTemplateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSecurityCreatePrivilegeTemplateResult||response.header.schema_id!=kSchemaCoordinateSecurityCreatePrivilegeTemplateResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSecurityCreateUser(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityCreateUserRequest,kSchemaCoordinateSecurityCreateUserRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSecurityCreateUserResult||response.header.schema_id!=kSchemaCoordinateSecurityCreateUserResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSecurityAlterUser(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityAlterUserRequest,kSchemaCoordinateSecurityAlterUserRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSecurityAlterUserResult||response.header.schema_id!=kSchemaCoordinateSecurityAlterUserResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);messages.diagnostics.push_back(MakeDiagnostic("SBLR.SECURITY_ALTER_USER.RESPONSE_SHAPE","ERROR","SEC_ALTER_USER SBPS response failed its message, schema, or payload contract.","parser_server_ipc.security_alter_user",{{"expected_message_type",std::to_string(kMessageCoordinateSecurityAlterUserResult)},{"actual_message_type",std::to_string(response.header.message_type)},{"expected_schema_id",std::to_string(kSchemaCoordinateSecurityAlterUserResultV1)},{"actual_schema_id",std::to_string(response.header.schema_id)},{"expected_payload_bytes","488"},{"actual_payload_bytes",std::to_string(response.payload.size())},{"error_frame",IsErrorFrame(response)?"true":"false"}}));result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSecurityCreateRole(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityCreateRoleRequest,kSchemaCoordinateSecurityCreateRoleRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSecurityCreateRoleResult||response.header.schema_id!=kSchemaCoordinateSecurityCreateRoleResultV1||response.payload.size()!=128||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);messages.diagnostics.push_back(MakeDiagnostic("SBLR.SECURITY_CREATE_ROLE.RESPONSE_SHAPE","ERROR","SEC_CREATE_ROLE response contract failed.","parser_server_ipc.security_create_role",{{"message_type",std::to_string(response.header.message_type)},{"schema_id",std::to_string(response.header.schema_id)},{"payload_bytes",std::to_string(response.payload.size())},{"error_frame",IsErrorFrame(response)?"true":"false"}}));result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSecurityDropRole(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityDropRoleRequest,kSchemaCoordinateSecurityDropRoleRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSecurityDropRoleResult||response.header.schema_id!=kSchemaCoordinateSecurityDropRoleResultV1||response.payload.size()!=128||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);messages.diagnostics.push_back(MakeDiagnostic("SBLR.SECURITY_DROP_ROLE.RESPONSE_SHAPE","ERROR","SEC_DROP_ROLE response contract failed.","parser_server_ipc.security_drop_role",{{"message_type",std::to_string(response.header.message_type)},{"schema_id",std::to_string(response.header.schema_id)},{"payload_bytes",std::to_string(response.payload.size())},{"error_frame",IsErrorFrame(response)?"true":"false"}}));result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSecurityAlterPrivilegeTemplate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityAlterPrivilegeTemplateRequest,kSchemaCoordinateSecurityAlterPrivilegeTemplateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSecurityAlterPrivilegeTemplateResult||response.header.schema_id!=kSchemaCoordinateSecurityAlterPrivilegeTemplateResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateSecurityDropPrivilegeTemplate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityDropPrivilegeTemplateRequest,kSchemaCoordinateSecurityDropPrivilegeTemplateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateSecurityDropPrivilegeTemplateResult||response.header.schema_id!=kSchemaCoordinateSecurityDropPrivilegeTemplateResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDatabaseCreateTemplateClone(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDatabaseCreateTemplateCloneRequest,kSchemaCoordinateDatabaseCreateTemplateCloneRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDatabaseCreateTemplateCloneResult||response.header.schema_id!=kSchemaCoordinateDatabaseCreateTemplateCloneResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateAggregate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateAggregateRequest,kSchemaCoordinateDdlCreateAggregateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateAggregateResult||response.header.schema_id!=kSchemaCoordinateDdlCreateAggregateResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterAggregate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterAggregateRequest,kSchemaCoordinateDdlAlterAggregateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterAggregateResult||response.header.schema_id!=kSchemaCoordinateDdlAlterAggregateResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropAggregate(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropAggregateRequest,kSchemaCoordinateDdlDropAggregateRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropAggregateResult||response.header.schema_id!=kSchemaCoordinateDdlDropAggregateResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlPurgeSystemHistory(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlPurgeSystemHistoryRequest,kSchemaCoordinateDdlPurgeSystemHistoryRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlPurgeSystemHistoryResult||response.header.schema_id!=kSchemaCoordinateDdlPurgeSystemHistoryResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlSetIndexOptimizerEligibility(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlSetIndexOptimizerEligibilityRequest,kSchemaCoordinateDdlSetIndexOptimizerEligibilityRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlSetIndexOptimizerEligibilityResult||response.header.schema_id!=kSchemaCoordinateDdlSetIndexOptimizerEligibilityResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlSetTableTypeEnforcement(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlSetTableTypeEnforcementRequest,kSchemaCoordinateDdlSetTableTypeEnforcementRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlSetTableTypeEnforcementResult||response.header.schema_id!=kSchemaCoordinateDdlSetTableTypeEnforcementResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDatabaseSerializeLogicalSnapshot(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDatabaseSerializeLogicalSnapshotRequest,kSchemaCoordinateDatabaseSerializeLogicalSnapshotRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDatabaseSerializeLogicalSnapshotResult||response.header.schema_id!=kSchemaCoordinateDatabaseSerializeLogicalSnapshotResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDatabaseDeserializeLogicalSnapshot(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDatabaseDeserializeLogicalSnapshotRequest,kSchemaCoordinateDatabaseDeserializeLogicalSnapshotRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDatabaseDeserializeLogicalSnapshotResult||response.header.schema_id!=kSchemaCoordinateDatabaseDeserializeLogicalSnapshotResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateMacro(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateMacroRequest,kSchemaCoordinateDdlCreateMacroRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateMacroResult||response.header.schema_id!=kSchemaCoordinateDdlCreateMacroResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropMacro(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropMacroRequest,kSchemaCoordinateDdlDropMacroRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropMacroResult||response.header.schema_id!=kSchemaCoordinateDdlDropMacroResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateDictionary(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateDictionaryRequest,kSchemaCoordinateDdlCreateDictionaryRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateDictionaryResult||response.header.schema_id!=kSchemaCoordinateDdlCreateDictionaryResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropDictionary(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropDictionaryRequest,kSchemaCoordinateDdlDropDictionaryRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropDictionaryResult||response.header.schema_id!=kSchemaCoordinateDdlDropDictionaryResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterDictionary(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterDictionaryRequest,kSchemaCoordinateDdlAlterDictionaryRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterDictionaryResult||response.header.schema_id!=kSchemaCoordinateDdlAlterDictionaryResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAdminRegisterExternalRelationResolver(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAdminRegisterExternalRelationResolverRequest,kSchemaCoordinateAdminRegisterExternalRelationResolverRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAdminRegisterExternalRelationResolverResult||response.header.schema_id!=kSchemaCoordinateAdminRegisterExternalRelationResolverResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateAdminUnregisterExternalRelationResolver(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateAdminUnregisterExternalRelationResolverRequest,kSchemaCoordinateAdminUnregisterExternalRelationResolverRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateAdminUnregisterExternalRelationResolverResult||response.header.schema_id!=kSchemaCoordinateAdminUnregisterExternalRelationResolverResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateContinuousView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateContinuousViewRequest,kSchemaCoordinateDdlCreateContinuousViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateContinuousViewResult||response.header.schema_id!=kSchemaCoordinateDdlCreateContinuousViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterContinuousView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterContinuousViewRequest,kSchemaCoordinateDdlAlterContinuousViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterContinuousViewResult||response.header.schema_id!=kSchemaCoordinateDdlAlterContinuousViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropContinuousView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropContinuousViewRequest,kSchemaCoordinateDdlDropContinuousViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropContinuousViewResult||response.header.schema_id!=kSchemaCoordinateDdlDropContinuousViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDmlAsyncInsertSubmit(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDmlAsyncInsertSubmitRequest,kSchemaCoordinateDmlAsyncInsertSubmitRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDmlAsyncInsertSubmitResult||response.header.schema_id!=kSchemaCoordinateDmlAsyncInsertSubmitResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDmlAsyncInsertStatus(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDmlAsyncInsertStatusRequest,kSchemaCoordinateDmlAsyncInsertStatusRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDmlAsyncInsertStatusResult||response.header.schema_id!=kSchemaCoordinateDmlAsyncInsertStatusResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDmlCounterAdd(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDmlCounterAddRequest,kSchemaCoordinateDmlCounterAddRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDmlCounterAddResult||response.header.schema_id!=kSchemaCoordinateDmlCounterAddResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDmlTimeseriesSchemaWrite(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDmlTimeseriesSchemaWriteRequest,kSchemaCoordinateDmlTimeseriesSchemaWriteRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDmlTimeseriesSchemaWriteResult||response.header.schema_id!=kSchemaCoordinateDmlTimeseriesSchemaWriteResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlTimeseriesSeriesCardinalityPolicy(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlTimeseriesSeriesCardinalityPolicyRequest,kSchemaCoordinateDdlTimeseriesSeriesCardinalityPolicyRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlTimeseriesSeriesCardinalityPolicyResult||response.header.schema_id!=kSchemaCoordinateDdlTimeseriesSeriesCardinalityPolicyResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDmlAsyncInsertCancel(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDmlAsyncInsertCancelRequest,kSchemaCoordinateDmlAsyncInsertCancelRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDmlAsyncInsertCancelResult||response.header.schema_id!=kSchemaCoordinateDmlAsyncInsertCancelResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateTimeseriesValueCache(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateTimeseriesValueCacheRequest,kSchemaCoordinateDdlCreateTimeseriesValueCacheRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateTimeseriesValueCacheResult||response.header.schema_id!=kSchemaCoordinateDdlCreateTimeseriesValueCacheResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlRefreshMaterializedView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlRefreshMaterializedViewRequest,kSchemaCoordinateDdlRefreshMaterializedViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlRefreshMaterializedViewResult||response.header.schema_id!=kSchemaCoordinateDdlRefreshMaterializedViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropMaterializedView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropMaterializedViewRequest,kSchemaCoordinateDdlDropMaterializedViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropMaterializedViewResult||response.header.schema_id!=kSchemaCoordinateDdlDropMaterializedViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult TypeCoord(const ParserSessionContext&s,const std::vector<std::uint8_t>&p,const std::string&ep,std::uint16_t mt,std::uint32_t ms,std::uint16_t mr,std::uint32_t rs,const std::string&socket_key){ServerVariableBindingResult r;MessageVectorSet m;Frame f;auto su=TextToUuid(s.session_uuid),cu=TextToUuid(s.connection_uuid);if(!s.authenticated||p.size()!=64||!SendRequest(ep,BaseHeader(mt,ms,su,cu),p,&f,&m,socket_key)){r.messages=std::move(m);return r;}if(f.header.message_type!=mr||f.header.schema_id!=rs||f.payload.size()!=488||IsErrorFrame(f)){if(f.payload.size()!=488&&!IsErrorFrame(f))m.diagnostics.push_back(MakeDiagnostic("SBLR.OPERAND.INVALID","ERROR","CREATE/ALTER/DROP TYPE response payload shape invalid: "+std::to_string(f.payload.size()),"parser_server_ipc.ddl_type_response_shape",{}));AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;}r.accepted=true;r.canonical_payload=std::move(f.payload);return r;}
ServerVariableBindingResult CtasCoord(const ParserSessionContext&s,const std::vector<std::uint8_t>&p,const std::string&ep,std::uint16_t mt,std::uint32_t ms,std::uint16_t mr,std::uint32_t rs,const std::string&key){ServerVariableBindingResult r;MessageVectorSet m;Frame f;auto su=TextToUuid(s.session_uuid),cu=TextToUuid(s.connection_uuid);if(!s.authenticated||p.size()!=64||!SendRequest(ep,BaseHeader(mt,ms,su,cu),p,&f,&m,key)){m.diagnostics.push_back(MakeDiagnostic("SBLR.CTAS.COORDINATION_FAILED","ERROR","CTAS coordination request did not produce a response.","parser_server_ipc.ctas_client",{{"request_message_type",std::to_string(mt)},{"request_schema_id",std::to_string(ms)},{"request_payload_bytes",std::to_string(p.size())}}));r.messages=std::move(m);return r;}if(f.header.message_type!=mr||f.header.schema_id!=rs||f.payload.size()!=512||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);m.diagnostics.push_back(MakeDiagnostic("SBLR.CTAS.RESPONSE_CONTRACT","ERROR","CTAS coordination response failed the message, schema, or descriptor-size contract.","parser_server_ipc.ctas_client",{{"expected_message_type",std::to_string(mr)},{"actual_message_type",std::to_string(f.header.message_type)},{"expected_schema_id",std::to_string(rs)},{"actual_schema_id",std::to_string(f.header.schema_id)},{"expected_payload_bytes","512"},{"actual_payload_bytes",std::to_string(f.payload.size())},{"error_frame",IsErrorFrame(f)?"true":"false"}}));r.messages=std::move(m);return r;}r.accepted=true;r.canonical_payload=std::move(f.payload);return r;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateType(const ParserSessionContext&s,const std::vector<std::uint8_t>&p)const{return TypeCoord(s,p,endpoint_,kMessageCoordinateDdlCreateTypeRequest,kSchemaCoordinateDdlCreateTypeRequestV1,kMessageCoordinateDdlCreateTypeResult,kSchemaCoordinateDdlCreateTypeResultV1,ActiveSocketCacheKey());}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateTableAsQueryWithData(const ParserSessionContext&s,const std::vector<std::uint8_t>&p)const{return CtasCoord(s,p,endpoint_,330,kSchemaCoordinateDdlCreateTableAsQueryWithDataRequestV1,331,kSchemaCoordinateDdlCreateTableAsQueryWithDataResultV1,ActiveSocketCacheKey());}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateTableAsQueryWithNoData(const ParserSessionContext&s,const std::vector<std::uint8_t>&p)const{return CtasCoord(s,p,endpoint_,332,kSchemaCoordinateDdlCreateTableAsQueryWithNoDataRequestV1,333,kSchemaCoordinateDdlCreateTableAsQueryWithNoDataResultV1,ActiveSocketCacheKey());}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterType(const ParserSessionContext&s,const std::vector<std::uint8_t>&p)const{return TypeCoord(s,p,endpoint_,kMessageCoordinateDdlAlterTypeRequest,kSchemaCoordinateDdlAlterTypeRequestV1,kMessageCoordinateDdlAlterTypeResult,kSchemaCoordinateDdlAlterTypeResultV1,ActiveSocketCacheKey());}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropType(const ParserSessionContext&s,const std::vector<std::uint8_t>&p)const{return TypeCoord(s,p,endpoint_,kMessageCoordinateDdlDropTypeRequest,kSchemaCoordinateDdlDropTypeRequestV1,kMessageCoordinateDdlDropTypeResult,kSchemaCoordinateDdlDropTypeResultV1,ActiveSocketCacheKey());}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropTable(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropTableRequest,kSchemaCoordinateDdlDropTableRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateDdlDropTableResult||f.header.schema_id!=kSchemaCoordinateDdlDropTableResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterPackage(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterPackageRequest,kSchemaCoordinateDdlAlterPackageRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterPackageResult||response.header.schema_id!=kSchemaCoordinateDdlAlterPackageResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlAlterSequence(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlAlterSequenceRequest,kSchemaCoordinateDdlAlterSequenceRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlAlterSequenceResult||response.header.schema_id!=kSchemaCoordinateDdlAlterSequenceResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlDropSequence(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropSequenceRequest,kSchemaCoordinateDdlDropSequenceRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlDropSequenceResult||response.header.schema_id!=kSchemaCoordinateDdlDropSequenceResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateMaterializedView(const ParserSessionContext&session,const std::vector<std::uint8_t>&payload)const{ServerVariableBindingResult result;MessageVectorSet messages;Frame response;const auto su=TextToUuid(session.session_uuid);const auto cu=TextToUuid(session.connection_uuid);if(!session.authenticated||payload.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateMaterializedViewRequest,kSchemaCoordinateDdlCreateMaterializedViewRequestV1,su,cu),payload,&response,&messages,ActiveSocketCacheKey())){result.messages=std::move(messages);return result;}if(response.header.message_type!=kMessageCoordinateDdlCreateMaterializedViewResult||response.header.schema_id!=kSchemaCoordinateDdlCreateMaterializedViewResultV1||response.payload.size()!=488||IsErrorFrame(response)){AddFrameDiagnostics(response,&messages);result.messages=std::move(messages);return result;}result.accepted=true;result.canonical_payload=std::move(response.payload);return result;}
ServerVariableBindingResult SbpsClient::CoordinateDdlRenameObject(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlRenameObjectRequest,kSchemaCoordinateDdlRenameObjectRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateDdlRenameObjectResult||f.header.schema_id!=kSchemaCoordinateDdlRenameObjectResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateSynonym(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateSynonymRequest,kSchemaCoordinateDdlCreateSynonymRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateDdlCreateSynonymResult||f.header.schema_id!=kSchemaCoordinateDdlCreateSynonymResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateForeignTable(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateForeignTableRequest,kSchemaCoordinateDdlCreateForeignTableRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateDdlCreateForeignTableResult||f.header.schema_id!=kSchemaCoordinateDdlCreateForeignTableResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateDdlCreateFdw(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlCreateFdwRequest,kSchemaCoordinateDdlCreateFdwRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateDdlCreateFdwResult||f.header.schema_id!=kSchemaCoordinateDdlCreateFdwResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateDdlDropFdw(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropFdwRequest,kSchemaCoordinateDdlDropFdwRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateDdlDropFdwResult||f.header.schema_id!=kSchemaCoordinateDdlDropFdwResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateDdlDropForeignTable(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropForeignTableRequest,kSchemaCoordinateDdlDropForeignTableRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateDdlDropForeignTableResult||f.header.schema_id!=kSchemaCoordinateDdlDropForeignTableResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateDdlDropSynonym(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateDdlDropSynonymRequest,kSchemaCoordinateDdlDropSynonymRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateDdlDropSynonymResult||f.header.schema_id!=kSchemaCoordinateDdlDropSynonymResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }

ServerVariableBindingResult SbpsClient::CoordinateSecurityCreatePolicy(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityCreatePolicyRequest,kSchemaCoordinateSecurityCreatePolicyRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityCreatePolicyResult||f.header.schema_id!=kSchemaCoordinateSecurityCreatePolicyResultV1||f.payload.size()!=488||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityDropPolicy(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityDropPolicyRequest,kSchemaCoordinateSecurityDropPolicyRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityDropPolicyResult||f.header.schema_id!=kSchemaCoordinateSecurityDropPolicyResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityAlterRole(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityAlterRoleRequest,kSchemaCoordinateSecurityAlterRoleRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityAlterRoleResult||f.header.schema_id!=kSchemaCoordinateSecurityAlterRoleResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }

ServerVariableBindingResult SbpsClient::CoordinateSecurityCreateGroupMapping(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityCreateGroupMappingRequest,kSchemaCoordinateSecurityCreateGroupMappingRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityCreateGroupMappingResult||f.header.schema_id!=kSchemaCoordinateSecurityCreateGroupMappingResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityDropGroupMapping(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityDropGroupMappingRequest,kSchemaCoordinateSecurityDropGroupMappingRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityDropGroupMappingResult||f.header.schema_id!=kSchemaCoordinateSecurityDropGroupMappingResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityGrant(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityGrantRequest,kSchemaCoordinateSecurityGrantRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityGrantResult||f.header.schema_id!=kSchemaCoordinateSecurityGrantResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityRevoke(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityRevokeRequest,kSchemaCoordinateSecurityRevokeRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityRevokeResult||f.header.schema_id!=kSchemaCoordinateSecurityRevokeResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityAlterPolicy(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityAlterPolicyRequest,kSchemaCoordinateSecurityAlterPolicyRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityAlterPolicyResult||f.header.schema_id!=kSchemaCoordinateSecurityAlterPolicyResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityDropUser(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityDropUserRequest,kSchemaCoordinateSecurityDropUserRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityDropUserResult||f.header.schema_id!=kSchemaCoordinateSecurityDropUserResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityAuthenticate(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityAuthenticateRequest,kSchemaCoordinateSecurityAuthenticateRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityAuthenticateResult||f.header.schema_id!=kSchemaCoordinateSecurityAuthenticateResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::CoordinateSecurityDeauthenticate(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageCoordinateSecurityDeauthenticateRequest,kSchemaCoordinateSecurityDeauthenticateRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageCoordinateSecurityDeauthenticateResult||f.header.schema_id!=kSchemaCoordinateSecurityDeauthenticateResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::SessionRoleSwitch(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=64||!SendRequest(endpoint_,BaseHeader(kMessageSessionRoleSwitchRequest,kSchemaSessionRoleSwitchRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageSessionRoleSwitchResult||f.header.schema_id!=kSchemaSessionRoleSwitchResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::SessionSettingSet(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=24||!SendRequest(endpoint_,BaseHeader(kMessageSessionSettingSetRequest,kSchemaSessionSettingSetRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageSessionSettingSetResult||f.header.schema_id!=kSchemaSessionSettingSetResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
ServerVariableBindingResult SbpsClient::SessionSettingReset(const ParserSessionContext& s,const std::vector<std::uint8_t>& p) const { ServerVariableBindingResult r; MessageVectorSet m; Frame f; const auto su=TextToUuid(s.session_uuid), cu=TextToUuid(s.connection_uuid); if(!s.authenticated||p.size()!=24||!SendRequest(endpoint_,BaseHeader(kMessageSessionSettingResetRequest,kSchemaSessionSettingResetRequestV1,su,cu),p,&f,&m,ActiveSocketCacheKey())){r.messages=std::move(m);return r;} if(f.header.message_type!=kMessageSessionSettingResetResult||f.header.schema_id!=kSchemaSessionSettingResetResultV1||f.payload.size()!=128||IsErrorFrame(f)){AddFrameDiagnostics(f,&m);r.messages=std::move(m);return r;} r.accepted=true;r.canonical_payload=std::move(f.payload);return r; }
} // namespace scratchbird::parser::ipc
