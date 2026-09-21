// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "canonical_diagnostic_catalog.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>

namespace { bool allocation_forbidden=false; }
void* operator new(std::size_t n) {
  if(allocation_forbidden) throw std::bad_alloc();
  if(void* p=std::malloc(n?n:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

namespace {
namespace d=scratchbird::core::diagnostics;
unsigned checks=0,failures=0;
void Check(bool ok,const char* why) {
  ++checks;
  if(!ok){++failures;std::cerr<<why<<'\n';}
}
void Sample(std::string_view code,d::CanonicalSeverity severity,bool failure,
            std::string_view retry,std::string_view outcome,std::string_view family) {
  const auto* row=d::FindCanonicalDiagnosticCode(code);
  Check(row && row->code==code && row->severity==severity && row->is_failure==failure &&
        row->retry_class==retry && row->required_outcome==outcome && row->diagnostic_class==family,
        "independent Core metadata sample differs");
}
}
int main() {
  using S=d::CanonicalSeverity;
  const auto catalog=d::CanonicalDiagnosticCodeCatalog();
  // Includes narrow-query, typed metric-update and clock-source registrations. Check the exact
  // admitted Core import, not a minimum row count.
  Check(catalog.size==1418 && catalog.data!=nullptr,"complete Core code inventory missing");
  std::size_t unspecified_retry=0,unspecified_outcome=0;
  std::string_view previous;
  for(const auto& row:catalog) {
    Check(!row.code.empty() && previous<row.code,"code inventory is not unique sorted data");
    Check(!row.retry_class.empty() && !row.required_outcome.empty() && !row.diagnostic_class.empty() &&
          !row.sqlstate.empty() && !row.numeric_binding.empty(),"metadata field disappeared");
    const auto severity=static_cast<unsigned>(row.severity);
    Check(severity>=1 && severity<=13,"severity outside controlling canonical vocabulary");
    allocation_forbidden=true;
    const auto* found=d::FindCanonicalDiagnosticCode(row.code);
    const auto again=d::CanonicalDiagnosticCodeCatalog();
    allocation_forbidden=false;
    Check(found==&row && again.data==catalog.data && again.size==catalog.size,
          "lookup allocated, copied, changed or failed to find a registered row");
    std::string invalid(row.code);invalid.push_back('\0');
    Check(d::FindCanonicalDiagnosticCode(invalid)==nullptr,"embedded-NUL code alias was admitted");
    unspecified_retry+=row.retry_class=="not_specified";
    unspecified_outcome+=row.required_outcome=="not_specified";
    previous=row.code;
  }
  Check(unspecified_retry==373 && unspecified_outcome==790,
        "missing behavior metadata was silently filled or removed");
  Sample("AUDIT_TRIGGER.EXTERNAL_TRANSACTION_COMMITTED",S::informational,false,
         "not_specified","return_to_parent_statement","AUDIT_TRIGGER");
  Sample("AUDIT_TRIGGER.QUEUE_RETRY",S::warning,false,
         "not_specified","continue_or_fail_by_policy","AUDIT_TRIGGER");
  Sample("DIAG.REDACTION_POLICY_INVALID",S::security,true,"false","deny_access","DIAG");
  Sample("STORAGE.PAGE_CHECKSUM_FAILED",S::corruption,true,"false","repair_required","STORAGE");
  Sample("NUMERIC.BACKEND.UNAVAILABLE",S::error,true,"retry_after_reference_backend_restored","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.ENCODING.NONCANONICAL",S::error,true,"retry_only_with_corrected_encoding","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.REAL128.DIVIDE_BY_ZERO",S::error,true,"retry_only_with_corrected_input","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.REAL128.INVALID",S::error,true,"retry_only_with_corrected_input_or_context","reject_invalid_operation_or_report_unordered_comparison","NUMERIC");
  Sample("NUMERIC.REAL128.OVERFLOW",S::error,true,"retry_only_with_corrected_input_or_context","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.REAL128.UNDERFLOW",S::warning,false,"not_applicable","preserve_rounded_value_and_underflow_inexact_facts","NUMERIC");
  Sample("STORAGE.CREATE_ARTIFACT_CONFLICT",S::error,true,
         "retry_only_after_corrected_artifact_ownership_and_fresh_admission",
         "preserve_existing_artifacts_without_creation_publication","STORAGE");
  Sample("STORAGE.CREATE_ARTIFACT_INSPECTION_FAILED",S::error,true,
         "retry_after_restored_inspection_and_fresh_admission",
         "preserve_existing_artifacts_without_creation_publication","STORAGE");
  for(const auto code:{"STORAGE.CREATE_ARTIFACT_CONFLICT","STORAGE.CREATE_ARTIFACT_INSPECTION_FAILED"}){
    const auto* row=d::FindCanonicalDiagnosticCode(code);
    Check(row&&row->sqlstate=="55000"&&row->numeric_binding=="not_applicable","creation artifact failure metadata differs");
  }
  Sample("STORAGE.READ_ONLY_DEVICE",S::error,true,
         "retry_only_with_authorized_writable_device",
         "reject_without_any_publication_or_page_mutation","STORAGE");
  const auto* read_only=d::FindCanonicalDiagnosticCode("STORAGE.READ_ONLY_DEVICE");
  Check(read_only && read_only->sqlstate=="25006" && read_only->numeric_binding=="not_applicable",
        "read-only device refusal lost canonical SQLSTATE or invented numeric binding");
  Sample("ACID.PARTIAL_OUTCOME_DETECTED",S::critical,true,"false","not_specified","ACID");
  Sample("MANAGER.NO_SPIN_REQUIRED",S::fatal,true,"false","not_specified","MANAGER");
  Sample("SBSQL.METADATA.CATALOG_SQL_FORBIDDEN",S::internal,true,
         "false","block_implementation_path","SBSQL");
  Sample("DIAG.CODE_UNKNOWN",S::error,true,"false","reject_operation","DIAG");
  Sample("TIME.SOURCE_FAILED",S::error,true,"only_after_clock_authority_revalidation",
         "reject_without_clock_or_identity_state_publication","TIME");
  const auto* clock_failure=d::FindCanonicalDiagnosticCode("TIME.SOURCE_FAILED");
  Check(clock_failure&&clock_failure->sqlstate=="55000"&&clock_failure->numeric_binding=="not_applicable",
        "clock-source failure metadata differs from admitted authority");
  Sample("SBLR.QUERY_BINDING.STALE",S::error,true,
         "only_after_corrected_input_and_fresh_authority_validation",
         "reject_without_binding_or_result_publication_preserve_transaction_state","SBLR");
  Sample("SBLR.PLAN_TREE.INVALID_HANDLE",S::error,true,
         "only_after_corrected_input_and_fresh_authority_validation",
         "reject_without_binding_or_result_publication_preserve_transaction_state","SBLR");
  Sample("PROJECTION.EXPRESSION_VECTOR.INVALID",S::error,true,
         "only_after_corrected_input_and_fresh_authority_validation",
         "reject_without_binding_or_result_publication_preserve_transaction_state","PROJECTION");
  Sample("PROJECTION.OUTPUT_ROWSET.INVALID",S::error,true,
         "only_after_corrected_input_and_fresh_authority_validation",
         "reject_without_binding_or_result_publication_preserve_transaction_state","PROJECTION");
  Sample("SORT.ORDERING_VECTOR.INVALID",S::error,true,
         "only_after_corrected_input_and_fresh_authority_validation",
         "reject_without_binding_or_result_publication_preserve_transaction_state","SORT");
  Sample("SORT.COLLATION_PROFILE.INVALID",S::error,true,
         "only_after_corrected_input_and_fresh_authority_validation",
         "reject_without_binding_or_result_publication_preserve_transaction_state","SORT");
  Sample("RESULT_SET.SHAPE_INVALID",S::error,true,
         "only_after_corrected_input_and_fresh_authority_validation",
         "reject_without_binding_or_result_publication_preserve_transaction_state","RESULT_SET");
  Sample("METRIC.RETENTION_POLICY_INVALID",S::error,true,
         "only_after_corrected_policy_and_fresh_catalog_validation",
         "reject_without_policy_or_retention_mutation","METRIC");
  for(const auto& pair:std::initializer_list<std::pair<std::string_view,std::string_view>>{
      {"METRIC.VALUE_INVALID","only_after_corrected_observation_and_descriptor"},
      {"METRIC.CURRENT_VALUE_INVALID","only_after_current_state_revalidation_or_repair"},
      {"METRIC.AGGREGATE_OVERFLOW","only_after_corrected_observation_or_authorized_reset"},
      {"METRIC.OBSERVATION_RESOURCE_EXHAUSTED","only_after_fresh_resource_admission"},
      {"METRIC.OBSERVATION_SOURCE_UNAVAILABLE","only_after_observation_source_revalidation"},
      {"METRIC.ARITHMETIC_FAILED","only_after_numeric_backend_revalidation"}})
    Sample(pair.first,S::error,true,pair.second,"reject_without_current_history_or_counter_mutation","METRIC");
  Sample("DATATYPE.DESCRIPTOR.INVALID",S::error,true,"false","refuse","DATATYPE");
  Sample("UUID.ENGINE_IDENTITY_NOT_V7",S::error,true,
         "never_retry_without_corrected_input_or_revalidated_authority",
         "reject_before_publication_preserve_authoritative_transaction_state","UUID");
  Sample("TIME.UUID_TIMESTAMP_OUT_OF_RANGE",S::error,true,
         "never_retry_without_corrected_time_authority","reject_uuid_generation_without_identity","TIME");
  Sample("PREPARED.IDENTITY_ISSUANCE_FAILED",S::error,true,
         "retry_after_resource_or_identity_authority_recovery","do_not_publish_template_or_use_receipt","PREPARED");
  Sample("PREPARED.ALLOCATION_FAILED",S::error,true,
         "retry_after_resource_recovery","do_not_publish_unowned_template_or_receipt","PREPARED");
  Sample("PREPARED.CONTENT_HASH_FAILED",S::error,true,
         "retry_after_hash_provider_recovery","do_not_publish_fallback_digest_or_template","PREPARED");
  Sample("PREPARED.METADATA_CONFLICT",S::error,true,
         "never_retry_without_corrected_metadata","refuse_cache_hit_preserve_retained_metadata","PREPARED");
  Sample("PREPARED.OWNER_MISMATCH",S::error,true,
         "never_retry_without_corrected_owner","refuse_binding_or_receipt_use","PREPARED");
  Sample("PREPARED.REGISTRY.INVALID",S::error,true,
         "only_after_verified_registry_recovery_and_fresh_request",
         "refuse_without_registry_mutation_or_transaction_outcome_change","PREPARED");
  Sample("PREPARED.REGISTRY.STALE",S::error,true,
         "only_after_verified_prepared_authority_revalidation_and_fresh_request",
         "refuse_without_registry_mutation_or_transaction_outcome_change","PREPARED");
  Sample("SBLR.PARAMETER.STALE",S::error,true,
         "only_after_verified_authority_revalidation_and_fresh_receipt",
         "refuse_without_changing_authoritative_transaction_outcome","SBLR");
  for(const auto code:{"PREPARED.REGISTRY.INVALID","PREPARED.REGISTRY.STALE"}) {
    const auto* row=d::FindCanonicalDiagnosticCode(code);
    Check(row&&row->sqlstate=="55000"&&row->numeric_binding=="not_applicable",
          "prepared registry SQLSTATE or numeric binding differs");
  }
  Sample("SBLR.ERROR_VECTOR.STALE",S::error,true,
         "only_after_verified_authority_revalidation_and_fresh_receipt",
         "refuse_without_changing_authoritative_transaction_outcome","SBLR");
  Sample("SBLR.EXECUTION_FAILED",S::error,true,"false",
         "fail_current_operation_preserve_authoritative_transaction_outcome","SBLR");
  Sample("SB_RESOURCE_ALIAS_AMBIGUOUS",S::error,true,
         "never_retry_without_catalog_or_authorization_change","reject_and_require_explicit_target","RESOURCE");
  const auto* alias=d::FindCanonicalDiagnosticCode("SB_RESOURCE_ALIAS_AMBIGUOUS");
  Check(alias && alias->sqlstate=="42702" && alias->numeric_binding=="not_applicable",
        "native/compatibility registration data changed");
  for(const std::string_view unknown:{"","diag.code_unknown"," DIAG.CODE_UNKNOWN",
                                      "DIAG.CODE_UNKNOWN ","SB_ENGINE_API_INVALID_REQUEST",
                                      "DATATYPE.DESCRIPTOR_INVALID","SBLR.OPERAND.INVALID",
                                      "MGA.TRANSACTION.STALE","CATALOG.SNAPSHOT_STALE"}) {
    allocation_forbidden=true;const auto* found=d::FindCanonicalDiagnosticCode(unknown);
    allocation_forbidden=false;
    Check(found==nullptr,"unknown code was invented, normalized or guessed");
  }
  constexpr std::array<std::uint8_t,32> expected_source{
    0x6d,0x59,0xa5,0x7b,0xad,0x95,0x15,0x94,0x2d,0xd2,0xbf,0x15,0x6c,0xab,0x3e,0x2b,0xf5,0x07,0x25,0xe5,0x44,0x67,0x89,0xc2,0xc9,0x48,0x2b,0xc8,0x04,0x4e,0xab,0xd4};
  Check(d::CanonicalDiagnosticCodeSourceSha256()==expected_source,"Core source provenance differs");
  std::cout<<"canonical_diagnostic_catalog rows="<<catalog.size<<" checks="<<checks
           <<" failures="<<failures<<'\n';
  return failures?1:0;
}
