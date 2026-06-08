// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::donor {

struct Field {
  std::string name;
  std::string value;
};

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::string component;
  std::vector<Field> fields;
};

struct Token {
  std::string kind;
  std::string lexeme;
  std::size_t offset{0};
};

struct ProceduralFunctionalEncodingSpanMetadata {
  std::size_t header_source_span_count{0};
  std::size_t body_source_span_count{0};
  bool parser_bound_sblr_body_instruction_stream{false};
  bool uuid_dependency_bindings_bound{false};
};

struct ProceduralSourceRetentionMetadata {
  std::size_t source_byte_length{0};
  std::uint64_t source_hash{0};
  std::size_t header_start_byte{0};
  std::size_t header_end_byte{0};
  std::size_t body_start_byte{0};
  std::size_t body_end_byte{0};
  std::size_t header_source_span_count{0};
  std::size_t body_source_span_count{0};
  bool parser_bound_sblr_body_instruction_stream{false};
  bool uuid_dependency_bindings_bound{false};
};

enum class MappingDisposition {
  kAdmittedSblr,
  kScratchBirdLifecycleApi,
  kParserSupportUdr,
  kCatalogProjection,
  kPolicyRefusal,
  kSecurityRefusal,
  kUnsupportedRefusal,
};

enum class PatternMatch {
  kPrefix,
  kContains,
  kPrefixAndContains,
  kContainsFunctionCall,
  kLoadDataLocalInfile,
  kLoadDataServerInfile,
  kCreateTableEngineClause,
  kFromStringLiteralUriScheme,
  kRestPathSegment,
  kRestMethodRoute,
  kPplPipelineStage,
  kWord,
  kRelationReference,
};

struct OperationPattern {
  std::string_view match;
  PatternMatch match_kind{PatternMatch::kPrefix};
  std::string_view statement_family;
  std::string_view operation_family;
  MappingDisposition disposition{MappingDisposition::kAdmittedSblr};
  std::string_view mapping_key;
  std::string_view sblr_operation;
  std::string_view engine_api_function;
  std::string_view diagnostic_code;
  std::string_view diagnostic_message;
  bool requires_security_context{false};
  bool requires_transaction_context{false};
};

struct SurfaceDescriptor {
  std::string_view family;
  std::string_view surface;
  std::string_view owner;
};

struct DialectProfile {
  std::string_view dialect_id;
  std::string_view display_name;
  std::string_view parser_package_name;
  std::string_view parser_support_package_name;
  std::string_view release_profile;
  std::string_view diagnostic_prefix;
  std::string_view sblr_operation_family;
  std::span<const OperationPattern> patterns;
  std::span<const SurfaceDescriptor> datatype_surfaces;
  std::span<const SurfaceDescriptor> builtin_function_surfaces;
  std::span<const SurfaceDescriptor> catalog_overlay_surfaces;
  std::span<const SurfaceDescriptor> diagnostic_surfaces;
  int parser_surface_rows{0};
  int function_api_rows{0};
  int donor_compatible_alias_rows{0};
  int core_or_optional_alias_rows{0};
  int catalog_projection_only_rows{0};
  int connector_operation_rows{0};
  int policy_blocked_rows{0};
  int trusted_udr_registration_rows{0};
  int unsupported_rows{0};
};

struct ParseResult {
  bool ok{false};
  std::string normalized_sql;
  std::string statement_family;
  std::string operation_family;
  std::string lifecycle_operation_id;
  std::string sblr_operation;
  std::string sblr_operation_family;
  std::string engine_api_function;
  std::string lifecycle_mapping_key;
  std::string emulation_diagnostic_code;
  std::string authority_disposition;
  bool scratchbird_lifecycle_api{false};
  bool parser_support_udr_route{false};
  bool catalog_projection_only{false};
  bool exact_emulated_diagnostic{false};
  bool real_donor_file_effects{false};
  bool donor_engine_sql_executed{false};
  bool fail_closed_refusal{false};
  std::string sblr_envelope;
  std::string message_vector_json;
  std::string parser_evidence_json;
};

std::string TrimAscii(std::string_view text);
std::string NormalizeWhitespace(std::string_view text);
std::string ToUpperAscii(std::string_view text);
std::string EscapeJson(std::string_view text);
std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics);
std::vector<Token> LexTokens(std::string_view sql_text);
ParseResult ParseStatement(std::string_view sql_text, const DialectProfile& profile);
std::string DatatypeDescriptorEvidenceJson(std::size_t datatype_reference_count);
std::string DatatypeProfileEvidenceJson(std::string_view dialect_id,
                                        std::span<const Token> active_tokens);
bool IsIndexSemanticDefaultsStatement(std::string_view active_upper_sql);
std::string IndexSemanticDefaultsEvidenceJson(std::string_view dialect_id,
                                              std::string_view release_profile,
                                              std::string_view active_upper_sql);
bool IsConstraintSemanticDefaultsStatement(std::string_view active_upper_sql);
std::string ConstraintSemanticDefaultsEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasSequenceIdentitySemanticProfile(std::string_view dialect_id);
bool IsSequenceIdentitySemanticStatement(std::string_view dialect_id,
                                         std::string_view active_upper_sql);
std::string SequenceIdentitySemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasIdentifierNameResolutionProfile(std::string_view dialect_id);
std::string IdentifierNameResolutionEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasScalarExpressionSemanticProfile(std::string_view dialect_id);
bool IsScalarExpressionSemanticStatement(std::string_view dialect_id,
                                         std::string_view active_upper_sql);
std::string ScalarExpressionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasDmlMutationSemanticProfile(std::string_view dialect_id);
bool IsDmlMutationSemanticStatement(std::string_view dialect_id,
                                    std::string_view active_upper_sql);
std::string DmlMutationSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasTransactionSessionSemanticProfile(std::string_view dialect_id);
bool IsTransactionSessionSemanticStatement(std::string_view dialect_id,
                                           std::string_view active_upper_sql);
std::string TransactionSessionSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasTemporarySessionObjectSemanticProfile(std::string_view dialect_id);
bool IsTemporarySessionObjectSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql);
std::string TemporarySessionObjectSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasDependencyBearingDdlSemanticProfile(std::string_view dialect_id);
bool IsDependencyBearingDdlSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql);
std::string DependencyBearingDdlSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasDdlTransactionBehaviorSemanticProfile(std::string_view dialect_id);
bool IsDdlTransactionBehaviorSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql);
std::string DdlTransactionBehaviorSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasResourceTextSemanticProfile(std::string_view dialect_id);
bool IsResourceTextSemanticStatement(std::string_view dialect_id,
                                     std::string_view active_upper_sql);
std::string ResourceTextSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasStatisticsOptimizerSemanticProfile(std::string_view dialect_id);
bool IsStatisticsOptimizerSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql);
std::string StatisticsOptimizerSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasLocksIsolationSemanticProfile(std::string_view dialect_id);
bool IsLocksIsolationSemanticStatement(std::string_view dialect_id,
                                       std::string_view active_upper_sql);
std::string LocksIsolationSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
bool HasSystemCatalogDefaultsSemanticProfile(std::string_view dialect_id);
bool IsSystemCatalogDefaultsSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql);
std::string SystemCatalogDefaultsSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view operation_id,
    std::span<const SurfaceDescriptor> catalog_surfaces);
bool HasSessionSettingsDiagnosticsSemanticProfile(std::string_view dialect_id);
bool IsSessionSettingsDiagnosticsSemanticStatement(
    std::string_view dialect_id,
    std::string_view active_upper_sql);
std::string SessionSettingsDiagnosticsSemanticEvidenceJson(
    std::string_view dialect_id,
    std::string_view release_profile,
    std::string_view active_upper_sql);
std::string EnterpriseReadinessEvidenceJson();
bool IsProceduralBodySourceRetentionStatement(std::string_view statement_family,
                                              std::string_view operation_family,
                                              std::string_view active_upper_sql);
std::string ProceduralBodySourceRetentionEvidenceJson(
    const ProceduralSourceRetentionMetadata& metadata);
std::string ProceduralFunctionalEncodingEvidenceJson(
    std::size_t source_span_count,
    bool cst_materialized,
    bool ast_materialized,
    bool bound_ast_materialized,
    ProceduralFunctionalEncodingSpanMetadata span_metadata);
ProceduralFunctionalEncodingSpanMetadata
ProceduralFunctionalEncodingSpanMetadataFor(std::string_view dialect_id,
                                            std::string_view active_upper_sql,
                                            std::span<const Token> tokens);
ProceduralSourceRetentionMetadata ProceduralSourceRetentionMetadataFor(
    std::string_view dialect_id,
    std::string_view normalized_sql,
    std::string_view active_upper_sql,
    std::span<const Token> tokens);
std::string PackageIdentityJson(const DialectProfile& profile);
std::string SurfaceReportJson(const DialectProfile& profile);
std::string ConnectionSandboxReportJson(const DialectProfile& profile);
std::string DialectVariantReportJson(const DialectProfile& profile);
std::string MappingDispositionName(MappingDisposition disposition);

} // namespace scratchbird::parser::donor
