// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "parser_client_types.hpp"
#include "parser_ipc_common.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::parser::ipc {

struct SbpsClientChannelState;

struct CursorStreamDescriptorV1 {
  bool present{false};
  std::string stream_descriptor_uuid;
  std::uint16_t descriptor_version{0};
  std::uint64_t descriptor_generation{0};
  std::string cursor_uuid;
  std::string execution_uuid;
  std::string result_set_uuid;
  std::string row_descriptor_uuid;
  std::string snapshot_uuid;
  std::uint64_t max_chunk_rows{0};
  std::uint64_t max_chunk_bytes{0};

  [[nodiscard]] bool complete() const {
    return present && !stream_descriptor_uuid.empty() &&
           descriptor_version == 1 && descriptor_generation != 0 &&
           !cursor_uuid.empty() && !execution_uuid.empty() &&
           !result_set_uuid.empty() && !row_descriptor_uuid.empty() &&
           !snapshot_uuid.empty() && max_chunk_rows != 0 &&
           max_chunk_bytes != 0;
  }
};

struct ServerExecutionResult {
  bool accepted{false};
  std::string operation_id;
  std::string cursor_uuid;
  std::uint64_t row_count{0};
  std::uint64_t affected_rows{0};
  bool affected_rows_present{false};
  std::string row_packet;
  CursorStreamDescriptorV1 cursor_stream_descriptor;
  bool transaction_state_present{false};
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  std::string transaction_uuid;
  std::string transaction_timestamp;
  // SBPS V2 transaction outcome.  These fields are explicit so a compatibility
  // bridge never infers finality or a retaining replacement from free-form
  // result text and never retries an already-applied commit/rollback.
  bool selected_transaction_present{false};
  ParserTransactionSelector selected_transaction;
  ParserTransactionFinality finality_state{
      ParserTransactionFinality::kNotApplicable};
  bool finality_applied{false};
  bool catalog_invalidation_applied{false};
  bool finalized_transaction_present{false};
  ParserTransactionSelector finalized_transaction;
  bool replacement_transaction_present{false};
  ParserTransactionSelector replacement_transaction;
  ParserTransactionReplacementReason replacement_reason{
      ParserTransactionReplacementReason::kNone};
  std::string transaction_outcome_detail;
  std::string transaction_diagnostic_code;
  MessageVectorSet messages;
};

struct ServerPrepareSblrResult {
  bool accepted{false};
  // Prepare is non-replayable after its V2 request reaches the transport.  A
  // missing/malformed response can therefore leave an engine-owned prepared
  // object whose existence cannot be inferred by the parser.  These typed
  // fields require the caller to quarantine its physical route instead of
  // retrying or guessing from diagnostics.
  bool outcome_unknown{false};
  bool caller_cleanup_required{false};
  std::string prepared_statement_uuid;
  std::string operation_id;
  std::string detail;
  MessageVectorSet messages;
};

struct ServerFetchResult {
  bool accepted{false};
  std::string cursor_uuid;
  std::uint64_t row_count{0};
  std::string row_packet;
  std::string detail;
  bool end_of_cursor{false};
  MessageVectorSet messages;
};

struct ServerCloseCursorResult {
  bool accepted{false};
  // A close is bound to the negotiated physical session route.  Transport
  // loss after write makes the close outcome unknowable; loss before write
  // still makes the route unusable and requires caller-owned cleanup.
  bool outcome_unknown{false};
  bool caller_cleanup_required{false};
  bool route_fatal{false};
  std::string cursor_uuid;
  std::string detail;
  MessageVectorSet messages;
};

struct ServerClosePreparedSblrResult {
  bool accepted{false};
  bool outcome_unknown{false};
  bool caller_cleanup_required{false};
  bool route_fatal{false};
  std::string prepared_statement_uuid;
  std::string detail;
  MessageVectorSet messages;
};

struct ServerStatementContextResult {
  bool accepted{false};
  ParserStatementContext context;
  MessageVectorSet messages;
};

struct ServerLiteralBindingResult {
  bool accepted{false};
  std::vector<std::uint8_t> canonical_payload;
  MessageVectorSet messages;
};

struct ServerParameterBindingResult {
  bool accepted{false};
  std::vector<std::uint8_t> canonical_payload;
  MessageVectorSet messages;
};

struct ServerVariableBindingResult {
  bool accepted{false};
  std::vector<std::uint8_t> canonical_payload;
  MessageVectorSet messages;
};

struct VariableFrameCoordination {
  std::string public_coordination_uuid;
  std::string operation_uuid;
  std::uint64_t coordinator_generation{0};
  std::uint64_t frame_generation{0};

  [[nodiscard]] bool present() const {
    return !public_coordination_uuid.empty() && !operation_uuid.empty() &&
           coordinator_generation != 0 && frame_generation != 0;
  }
};

enum class ParameterExecutionMode : std::uint8_t {
  kDirect = 0,
  kPrepared = 1,
  kBatch = 2,
  kDynamic = 3,
};

struct ParameterExecutionCoordination {
  ParameterExecutionMode mode{ParameterExecutionMode::kDirect};
  std::string public_coordination_uuid;
  std::string operation_uuid;
  std::uint64_t coordinator_generation{0};

  [[nodiscard]] bool present() const {
    return !public_coordination_uuid.empty() && !operation_uuid.empty() &&
           coordinator_generation != 0;
  }
};

struct ServerParameterCoordinationResult {
  bool accepted{false};
  ParameterExecutionCoordination coordination;
  MessageVectorSet messages;
};

struct PreparedParameterReference {
  std::string prepared_statement_uuid;
  std::uint64_t prepared_generation{0};
  std::string operation_uuid;
  std::uint64_t coordination_generation{0};

  [[nodiscard]] bool present() const {
    return !prepared_statement_uuid.empty() && prepared_generation != 0 &&
           !operation_uuid.empty() && coordination_generation != 0;
  }
};

struct ServerPreparedParameterFinalizeResult {
  bool accepted{false};
  PreparedParameterReference prepared;
  MessageVectorSet messages;
};

// Deterministic protocol-conformance hook.  Production callers receive the
// same validation through the routed execute APIs.
bool DecodeExecuteResultPayloadV2ForTest(
    const std::vector<std::uint8_t>& payload,
    ServerExecutionResult* result,
    MessageVectorSet* messages);
bool DecodePrepareResultPayloadV2ForTest(
    const std::vector<std::uint8_t>& payload,
    ServerPrepareSblrResult* result,
    MessageVectorSet* messages);

// Canonical value-free schema-4015 request bytes used only as the SBPT
// template suffix. Runtime parameter values remain exclusively in a fresh
// schema-4017 EXECUTE request.
std::vector<std::uint8_t> EncodePreparedParameterSchema4015Template(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission);
bool DecodeDiagnosticFrameForTest(
    const std::vector<std::uint8_t>& encoded_frame,
    MessageVectorSet* messages);
bool DecodeAcquireStatementContextResultPayloadV1ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context);
bool DecodeAcquireStatementContextResultPayloadV4ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context);
bool DecodeAcquireStatementContextResultPayloadV5ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context);
bool DecodeAcquireStatementContextResultPayloadV6ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context);
bool DecodeAcquireStatementContextResultPayloadV7ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context);
bool DecodeAcquireStatementContextResultPayloadV8ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context);
bool DecodeAcquireStatementContextResultPayloadV9ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context);
bool DecodeAcquireStatementContextResultPayloadV10ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context);
std::vector<std::uint8_t>
EncodeAcquireStatementContextRequestPayloadV1ForTest(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction);
std::vector<std::uint8_t> EncodeCanonicalExecutePayloadV1ForTest(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested);
bool V2RequestMayRetryAfterWriteForTest(std::uint32_t schema_id);
bool SessionBoundRequestMayRetryAfterWriteForTest(std::uint32_t schema_id);

struct ServerManagementResult {
  bool accepted{false};
  std::string operation_key;
  std::string payload;
  MessageVectorSet messages;
};

struct AuthCredentialEnvelope {
  std::string provider_family{"local_password"};
  std::string principal;
  std::string requested_database{"default"};
  std::string requested_language{"en"};
  std::string requested_role;
  std::string application_name;
  std::string credential_evidence;
  bool credential_evidence_present{false};
  bool credential_invalid{false};
  bool mfa_required{false};
  bool mfa_evidence_present{false};
};

// Neutral, engine-owned projection of one persisted MGA relation column.
// Compatibility families may render this canonical metadata, but no upstream
// type ids, catalog ids, grammar policy, or presentation policy crosses SBPS.
struct PublicRelationColumnDescriptor {
  std::string column_uuid;
  std::uint32_t ordinal{0};
  std::string canonical_name_key;
  std::string type_descriptor_uuid;
  std::string type_descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_type_descriptor;
  bool nullable{true};
  bool generated{false};
  bool identity_column{false};
  std::string charset_uuid;
  std::string charset_canonical_name;
  std::string collation_uuid;
  std::string collation_canonical_name;
  std::uint32_t character_length{0};
  std::uint32_t charset_min_bytes{0};
  std::uint32_t charset_max_bytes{0};
  bool charset_variable_width{false};
};

struct PublicRelationDescriptor {
  bool present{false};
  std::string descriptor_uuid;
  std::string relation_uuid;
  std::string schema_uuid;
  std::uint64_t descriptor_generation{0};
  // Exact current resource catalog epoch under which every projected
  // resource UUID was revalidated.  This is not represented as a claim that
  // the MGA relation-storage descriptor persisted this scalar epoch.
  std::uint64_t validated_resource_epoch{0};
  std::vector<PublicRelationColumnDescriptor> columns;
};

struct PublicNameResolutionResult {
  struct ResourceDescriptor {
    bool present{false};
    std::string resource_family;
    std::string canonical_name;
    std::string parent_resource_uuid;
    std::string parent_canonical_name;
    std::string default_collation_uuid;
    std::string default_collation_name;
    std::uint64_t resource_epoch{0};
    std::uint64_t family_epoch{0};
    std::string family_version;
    std::uint32_t min_bytes{0};
    std::uint32_t max_bytes{0};
    bool variable_width{false};
    bool default_for_parent{false};
    bool case_insensitive{false};
    bool accent_insensitive{false};
  };

  bool resolved{false};
  std::string object_uuid;
  std::string canonical_name;
  std::string object_class;
  // Neutral engine-owned semantic detail returned by public name resolution.
  // Parser families may consume recognized descriptors but cannot persist or
  // reinterpret this text as transaction, storage, or authorization state.
  std::string resolution_detail;
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_epoch{0};
  ResourceDescriptor resource_descriptor;
  PublicRelationDescriptor relation_descriptor;
  MessageVectorSet messages;
};

// Deterministic neutral protocol-conformance hooks.  They exercise the same
// production request/result codecs without exposing any parser-family policy.
std::vector<std::uint8_t> EncodeResolveNameRequestPayloadV2ForTest(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction);
std::vector<std::uint8_t> EncodeResolveNameRequestPayloadV3ForTest(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction,
    std::uint8_t projection_flags);
bool DecodeResolveNameResultPayloadV3ForTest(
    const std::vector<std::uint8_t>& payload,
    bool require_relation_descriptor,
    PublicNameResolutionResult* result);

class SbpsClient {
 public:
  explicit SbpsClient(std::string endpoint);
  ~SbpsClient();

  SbpsClient(const SbpsClient&) = delete;
  SbpsClient& operator=(const SbpsClient&) = delete;
  SbpsClient(SbpsClient&& other) noexcept;
  SbpsClient& operator=(SbpsClient&& other) noexcept;

  [[nodiscard]] bool configured() const { return !endpoint_.empty(); }

  bool SendHello(MessageVectorSet* messages) const;
  bool AuthenticateAndAttach(const AuthCredentialEnvelope& credentials,
                             const ParserClientConfig& config,
                             ParserSessionContext* session,
                             MessageVectorSet* messages) const;
  bool AuthenticateAndAttach(std::string_view auth_payload,
                             const ParserClientConfig& config,
                             ParserSessionContext* session,
                             MessageVectorSet* messages) const;
  PublicNameResolutionResult ResolveNamePublic(const ParserSessionContext& session,
                                               std::string_view presented_name,
                                               bool quoted,
                                               std::string_view object_class,
                                               const ParserClientConfig& config) const;
  PublicNameResolutionResult ResolveNamePublicUncached(const ParserSessionContext& session,
                                                       std::string_view presented_name,
                                                       bool quoted,
                                                       std::string_view object_class,
                                                       const ParserClientConfig& config) const;
  PublicNameResolutionResult ResolveNamePublicOnTransaction(
      const ParserSessionContext& session,
      std::string_view presented_name,
      bool quoted,
      std::string_view object_class,
      const ParserClientConfig& config,
      const ParserTransactionSelector& transaction) const;
  // Negotiated V3 exact-transaction name resolution without requesting a
  // relation extension. The base result may carry bounded engine-owned
  // semantic detail for object classes such as persisted views.
  PublicNameResolutionResult ResolveNameSemanticPublicOnTransaction(
      const ParserSessionContext& session,
      std::string_view presented_name,
      bool quoted,
      std::string_view object_class,
      const ParserClientConfig& config,
      const ParserTransactionSelector& transaction) const;
  PublicNameResolutionResult ResolveRelationDescriptorPublicOnTransaction(
      const ParserSessionContext& session,
      std::string_view presented_name,
      bool quoted,
      std::string_view object_class,
      const ParserClientConfig& config,
      const ParserTransactionSelector& transaction) const;
  PublicNameResolutionResult RenderUuidPublic(const ParserSessionContext& session,
                                              std::string_view object_uuid) const;
  ServerStatementContextResult AcquireStatementContext(
      const ParserSessionContext& session,
      const ParserTransactionSelector& transaction) const;
  ServerStatementContextResult AcquireNativeStatementContext(
      const ParserSessionContext& session,
      const ParserTransactionSelector& transaction) const;
  ServerParameterCoordinationResult BeginParameterExecutionCoordination(
      const ParserSessionContext& session,
      ParameterExecutionMode mode,
      std::string_view operation_uuid,
      std::string_view public_prepared_uuid = {},
      std::string_view public_dynamic_package_uuid = {}) const;
  ServerStatementContextResult AcquireParameterStatementContext(
      const ParserSessionContext& session,
      const ParserTransactionSelector& transaction,
      const ParameterExecutionCoordination& coordination) const;
  ServerLiteralBindingResult NegotiateLiteralDescriptors(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbln) const;
  ServerLiteralBindingResult FinalizeLiteralBinding(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sblf) const;
  ServerParameterBindingResult NegotiateParameterDescriptors(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbpr) const;
  ServerParameterBindingResult FinalizeParameterBinding(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbpf) const;
  ServerVariableBindingResult BeginVariableFrame(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbvb) const;
  ServerStatementContextResult AcquireVariableStatementContext(
      const ParserSessionContext& session,
      const ParserTransactionSelector& transaction,
      const VariableFrameCoordination& coordination) const;
  ServerVariableBindingResult NegotiateVariableDescriptors(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbvr) const;
  ServerVariableBindingResult AssignVariableValues(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbvy) const;
  ServerVariableBindingResult FinalizeVariableBinding(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbvf) const;
  ServerVariableBindingResult CloseVariableFrame(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbvx) const;
  ServerVariableBindingResult IssueSourceMapDescriptor(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_smrq) const;
  ServerVariableBindingResult IssueErrorVectorDescriptor(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_evrq) const;
  ServerVariableBindingResult CoordinateSavepoint(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_spcr) const;
  ServerVariableBindingResult CoordinateAutonomousFrame(
      const ParserSessionContext& session,
      const std::vector<std::uint8_t>& canonical_afcr) const;
  ServerVariableBindingResult CoordinateReservationRelease(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateTemporaryInstanceCleanup(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateCursorOpen(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateReadByKey(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateReadRange(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateReadStream(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateResultSetPass(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAccessCursorOpen(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAccessCursorFetch(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAccessCursorClose(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateInsert(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateUpdate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDelete(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateMerge(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateTableTruncate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateTableAnalyze(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateBulkImportStream(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateBulkExportStream(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateStatementBatch(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAtomicCas(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAtomicRmw(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAdvisoryLock(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAdvisoryLockRelease(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateFunctionCall(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateOperatorCall(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateCast(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateCompare(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDomainOperation(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateUdrInvoke(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateProcedureInvoke(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateFunctionInvoke(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAggregateInvoke(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSequenceNextval(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSequenceCurrval(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSequenceSetval(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateQueryNumeric(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAdvancedDatatypeFamily(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateProject(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateCatalogIntrospect(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateKvStructuredRead(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateKvStructuredMutate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateKvStructuredScan(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateKvStructuredStreamRead(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateKvStructuredStreamAppend(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateKvStructuredTimeseries(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSystemConfigSet(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateDomain(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterDomain(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreatePublication(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterPublication(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropPublication(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateSubscription(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterSubscription(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropSubscription(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateOperator(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropOperator(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateOperatorClass(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropOperatorClass(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateOperatorFamily(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterOperatorFamily(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateExtension(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterExtension(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropExtension(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateClusterCreatePlacementPolicy(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateClusterAlterPlacementPolicy(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateClusterDropPlacementPolicy(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateVersionedBranchCreate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateVersionedBranchDelete(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateVersionedDiff(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateVersionedTag(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateVersionedRevert(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateVersionedReset(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAccelLlvmPolicySet(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropCast(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateMaterializedView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlRefreshMaterializedView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropMaterializedView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateType(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateTableAsQueryWithData(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateTableAsQueryWithNoData(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterType(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropType(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropTable(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateTrigger(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterTrigger(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropTrigger(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateProcedure(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterProcedure(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropProcedure(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateFunction(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterFunction(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropFunction(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreatePackage(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateSynonym(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateForeignTable(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateFdw(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropFdw(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropForeignTable(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropSynonym(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropPackage(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterPackage(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterSequence(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropSequence(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateTemporaryTable(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropTemporaryTable(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlRenameObjectVector(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlRenameObject(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateSchema(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateOrReplaceSrs(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropSrs(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateRewriteRule(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterRewriteRule(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropRewriteRule(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlValidateConstraint(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityCreatePrivilegeTemplate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityCreateUser(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityAlterUser(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityCreateRole(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityDropRole(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityAlterRole(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityCreatePolicy(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityDropPolicy(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityAlterPrivilegeTemplate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityDropPrivilegeTemplate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDatabaseCreateTemplateClone(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateAggregate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterAggregate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropAggregate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlPurgeSystemHistory(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlSetIndexOptimizerEligibility(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlSetTableTypeEnforcement(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDatabaseSerializeLogicalSnapshot(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDatabaseDeserializeLogicalSnapshot(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateMacro(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateDictionary(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropDictionary(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterDictionary(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateContinuousView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlAlterContinuousView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropContinuousView(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDmlAsyncInsertSubmit(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDmlAsyncInsertStatus(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDmlCounterAdd(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDmlTimeseriesSchemaWrite(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlTimeseriesSeriesCardinalityPolicy(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateTimeseriesValueCache(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDmlAsyncInsertCancel(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropMacro(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAdminRegisterExternalRelationResolver(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAdminUnregisterExternalRelationResolver(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateTable(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlCreateIndex(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateDdlDropIndex(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateAggregate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateGroup(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityCreateGroupMapping(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityDropGroupMapping(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityGrant(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityRevoke(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityAlterPolicy(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityDropUser(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityAuthenticate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSecurityDeauthenticate(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult SessionRoleSwitch(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult SessionSettingSet(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult ContextSet(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult ContextUnset(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult ContextGet(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult SessionSettingReset(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult SessionSettingGet(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult SessionDefaultQualifierSet(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult SessionDiscard(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult SessionSnapshotHandle(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateSort(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateLimit(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateWindow(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerVariableBindingResult CoordinateReturnResultSet(const ParserSessionContext&,const std::vector<std::uint8_t>&) const;
  ServerPreparedParameterFinalizeResult FinalizePreparedParameterSubmission(
      const ParserSessionContext& session,
      const ParameterExecutionCoordination& coordination,
      const std::vector<std::uint8_t>& canonical_sbpt,
      const ParserStatementContext& preliminary_context) const;
  ServerExecutionResult ExecuteSblr(const ParserSessionContext& session,
                                    std::string_view encoded_sblr_envelope,
                                    bool cursor_requested = false) const;
  ServerExecutionResult ExecuteSblrRouted(
      const ParserSessionContext& session,
      std::string_view encoded_sblr_envelope,
      const ParserTransactionRouting& transaction,
      bool cursor_requested = false) const;
  ServerExecutionResult ExecuteSblrWithDataPacket(
      const ParserSessionContext& session,
      std::string_view encoded_sblr_envelope,
      const std::vector<std::uint8_t>& data_packet,
      bool cursor_requested = false) const;
  ServerExecutionResult ExecuteSblrWithDataPacketRouted(
      const ParserSessionContext& session,
      std::string_view encoded_sblr_envelope,
      const std::vector<std::uint8_t>& data_packet,
      const ParserTransactionRouting& transaction,
      bool cursor_requested = false) const;
  ServerExecutionResult ExecuteCanonicalSblrWithDataPacket(
      const ParserSessionContext& session,
      const ParserStatementContext& statement_context,
      const ParserCanonicalSblrSubmission& submission,
      const std::vector<std::uint8_t>& data_packet,
      bool cursor_requested = false) const;
  ServerPrepareSblrResult PrepareSblr(const ParserSessionContext& session,
                                      std::string_view encoded_sblr_envelope) const;
  ServerPrepareSblrResult PrepareStmt(const ParserSessionContext& session,
                                      std::string_view encoded_sblr_envelope) const;
  ServerPrepareSblrResult PrepareStmtCanonical(const ParserSessionContext& session,
                                               const ParserCanonicalSblrSubmission& submission) const;
  ServerPrepareSblrResult PrepareSblrRouted(
      const ParserSessionContext& session,
      std::string_view encoded_sblr_envelope,
      const ParserTransactionSelector& transaction) const;
  ServerExecutionResult ExecutePreparedSblr(const ParserSessionContext& session,
                                            std::string_view prepared_statement_uuid,
                                            std::string_view encoded_sblr_envelope = {},
                                            const std::vector<std::uint8_t>& data_packet = {},
                                            bool cursor_requested = false) const;
  ServerExecutionResult ExecutePreparedSblrRouted(
      const ParserSessionContext& session,
      std::string_view prepared_statement_uuid,
      const ParserTransactionSelector& transaction,
      std::string_view encoded_sblr_envelope = {},
      const std::vector<std::uint8_t>& data_packet = {},
      bool cursor_requested = false) const;
  ServerClosePreparedSblrResult ClosePreparedSblr(
      const ParserSessionContext& session,
      std::string_view prepared_statement_uuid) const;
  ServerFetchResult FetchCursor(const ParserSessionContext& session,
                                std::string_view cursor_uuid,
                                const CursorStreamDescriptorV1& stream_descriptor,
                                std::uint64_t max_rows = 1,
                                std::uint64_t max_bytes = 0,
                                std::uint32_t fetch_flags = 0) const;
  ServerCloseCursorResult CloseCursor(const ParserSessionContext& session,
                                      std::string_view cursor_uuid) const;
  ServerCloseCursorResult CancelCursor(const ParserSessionContext& session,
                                       std::string_view cursor_uuid) const;
  ServerManagementResult Manage(const ParserSessionContext& session,
                                std::string_view operation_key,
                                std::string_view target_uuid = {},
                                std::string_view mode = {},
                                std::string_view audit_reason = {},
                                std::uint64_t timeout_ms = 30000,
                                bool include_history = false) const;
  bool DisconnectSession(const ParserSessionContext& session, MessageVectorSet* messages) const;

  // Deterministic neutral conformance hooks.  They expose only transport
  // identity stability; no parser-family or transaction semantics live here.
  [[nodiscard]] std::string V2ChannelCacheKeyForTest() const;
  [[nodiscard]] std::vector<std::uint8_t> V2HelloPayloadForTest() const;
  [[nodiscard]] std::vector<std::uint8_t>
  PreparedMetadataTransferV1HelloPayloadForTest() const;
  [[nodiscard]] std::vector<std::uint8_t>
  RelationDescriptorV3HelloPayloadForTest() const;
  [[nodiscard]] bool UsesDedicatedV2ChannelForTest() const;

 private:
  bool SendHelloWithRequirements(bool require_transaction_routing_v2,
                                 bool* transaction_routing_v2_accepted,
                                 bool require_prepared_metadata_transfer_v1,
                                 bool* prepared_metadata_transfer_v1_accepted,
                                 bool require_relation_descriptor_projection_v3,
                                 bool* relation_descriptor_projection_v3_accepted,
                                 MessageVectorSet* messages) const;
  void EnableDedicatedV2Channel() const;
  [[nodiscard]] const std::string& ActiveSocketCacheKey() const;
  [[nodiscard]] const std::vector<std::uint8_t>& StableV2HelloPayload() const;
  [[nodiscard]] const std::vector<std::uint8_t>&
  StablePreparedMetadataTransferV1HelloPayload() const;
  [[nodiscard]] const std::vector<std::uint8_t>&
  StableRelationDescriptorV3HelloPayload() const;
  [[nodiscard]] const std::vector<std::uint8_t>&
  StablePreparedMetadataTransferRelationDescriptorV3HelloPayload() const;
  std::string endpoint_;
  std::unique_ptr<SbpsClientChannelState> channel_state_;
};

} // namespace scratchbird::parser::ipc
