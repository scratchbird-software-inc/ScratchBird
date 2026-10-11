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
  Sample("AGENT.INVALID_STATE",S::error,true,
         "only_after_legal_state_and_owner_revalidation",
         "refuse_transition_preserve_current_state_and_ownership","AGENT");
  Sample("diag.mga.concurrency.invalid_primitive_descriptor",S::fatal,true,
         "only_after_corrected_descriptor_and_fresh_admission",
         "reject_before_registration_preserve_existing_ownership","MGA.CONCURRENCY");
  Sample("diag.mga.concurrency.latch_timeout",S::error,true,
         "only_after_owner_revalidates_wait_and_operation_state",
         "end_wait_preserve_authoritative_operation_outcome","MGA.CONCURRENCY");
  Sample("diag.mga.concurrency.fail_safe_release",S::error,true,
         "only_after_owner_proves_safe_cleanup_and_revalidates_admission",
         "retain_reachable_storage_and_original_ownership_until_safe_release","MGA.CONCURRENCY");
  Sample("diag.mga.concurrency.hazard_leak_detected",S::fatal,true,
         "only_after_owner_proves_reader_quiescence",
         "defer_reclamation_preserve_backing_and_charges","MGA.CONCURRENCY");
  Sample("diag.mga.concurrency.reclamation_blocked_by_hazard",S::warning,false,
         "only_after_reader_quiescence_or_later_collection",
         "defer_reclamation_preserve_backing_and_charges","MGA.CONCURRENCY");
  const auto catalog=d::CanonicalDiagnosticCodeCatalog();
  // Includes native bulk policy, shutdown identity and retained agent notices. Check the
  // exact admitted Core import, not a minimum row count.
  Check(catalog.size==1546 && catalog.data!=nullptr,"complete Core code inventory missing");
  for (const auto code : {"NUMERIC.INT128.OVERFLOW", "NUMERIC.UINT128.OVERFLOW"}) {
    Sample(code,S::error,true,"retry_only_with_corrected_input_or_context",
           "reject_without_numeric_value","NUMERIC");
    const auto* row=d::FindCanonicalDiagnosticCode(code);
    Check(row && row->sqlstate=="22003" && row->numeric_binding=="not_applicable",
          "128-bit overflow source registration changed");
  }
  Sample("MEMORY.LEAK_CLASSIFIED",S::informational,false,"not_applicable",
         "report_retention_preserve_ownership_and_operation_outcome","MEMORY");
  Sample("MEMORY.LEAK_SUSPECTED",S::warning,false,"not_applicable",
         "record_suspicion_require_investigation_preserve_ownership","MEMORY");
  Sample("MEMORY.LEAK_CONFIRMED",S::error,true,
         "only_after_owner_remediation_and_revalidation",
         "record_cleanup_failure_require_owner_remediation_preserve_ownership","MEMORY");
  for (const auto code : {"MEMORY.LEAK_CLASSIFIED", "MEMORY.LEAK_SUSPECTED", "MEMORY.LEAK_CONFIRMED"}) {
    const auto* row=d::FindCanonicalDiagnosticCode(code);
    Check(row && row->sqlstate=="not_applicable" && row->numeric_binding=="not_applicable",
          "retention classification invented a public SQLSTATE or numeric binding");
  }
  Sample("BLOB.IO_FAILED",S::error,true,
         "only after input authority environment or policy changes as applicable",
         "reject_abort_scrub_no_output","DATATYPE.BLOB");
  Sample("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE",S::error,true,
         "only after authority availability and full revalidation",
         "reject_no_content_no_output","DATATYPE.BLOB");
  Sample("MEMORY.CONTAINER_LIMIT_UNVERIFIED",S::error,true,
         "after_corrected_discovery_or_explicit_admitted_degraded_policy",
         "refuse_ordinary_startup","MEMORY");
  Sample("MEMORY.CONTAINER_LIMIT_DEGRADED",S::warning,false,
         "restore_discovery_then_readmit",
         "deliver_warning_before_ordinary_admission_and_retain_incomplete_state","MEMORY");
  Sample("MEMORY.EMERGENCY_RESERVE_INVALID",S::error,true,
         "after_corrected_policy_or_available_capacity",
         "reject_policy_or_startup_without_ordinary_admission","MEMORY");
  Sample("SB-MGA-SNAPSHOT-VECTOR-UNKNOWN",S::error,true,
         "only_with_actual_live_retained_pin",
         "refuse_without_descriptor_or_completed_prefix","TRANSACTION.SNAPSHOT");
  Sample("SB-MGA-SNAPSHOT-VECTOR-REVOKED",S::error,true,
         "never_reuse_revoked_pin_or_copied_descriptor",
         "refuse_without_descriptor_or_completed_prefix","TRANSACTION.SNAPSHOT");
  Sample("SB-MGA-SNAPSHOT-VECTOR-LOCK-FAILURE",S::error,true,
         "after_successful_fresh_pin_observation",
         "refuse_without_descriptor_or_completed_prefix","TRANSACTION.SNAPSHOT");
  const auto* blob_io=d::FindCanonicalDiagnosticCode("BLOB.IO_FAILED");
  const auto* blob_lifetime=d::FindCanonicalDiagnosticCode("BLOB.LIFETIME_AUTHORITY_UNAVAILABLE");
  Check(blob_io && blob_io->sqlstate=="58030" &&
        blob_io->numeric_binding=="not_applicable",
        "blob I/O diagnostic binding differs");
  Check(blob_lifetime && blob_lifetime->sqlstate=="58000" &&
        blob_lifetime->numeric_binding=="not_applicable",
        "blob lifetime authority diagnostic binding differs");
  for(const std::string_view code:{"AGENT.INVALID_STATE",
      "diag.mga.concurrency.invalid_primitive_descriptor",
      "diag.mga.concurrency.latch_timeout","diag.mga.concurrency.fail_safe_release",
      "diag.mga.concurrency.hazard_leak_detected",
      "diag.mga.concurrency.reclamation_blocked_by_hazard"}) {
    const auto* row=d::FindCanonicalDiagnosticCode(code);
    Check(row && row->sqlstate=="not_applicable" && row->numeric_binding=="not_applicable",
          "internal concurrency diagnostic acquired a public binding");
  }
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
  Check(unspecified_retry==373 && unspecified_outcome==788,
        "missing behavior metadata was silently filled or removed");
  Sample("AUDIT_TRIGGER.EXTERNAL_TRANSACTION_COMMITTED",S::informational,false,
         "not_specified","return_to_parent_statement","AUDIT_TRIGGER");
  Sample("AUDIT_TRIGGER.QUEUE_RETRY",S::warning,false,
         "not_specified","continue_or_fail_by_policy","AUDIT_TRIGGER");
  Sample("DIAG.REDACTION_POLICY_INVALID",S::security,true,"false","deny_access","DIAG");
  Sample("METRIC.ACCESS_DENIED",S::error,true,"only_after_authority_or_binding_revalidation",
         "deny_read_without_rows_or_existence_disclosure","METRIC");
  const auto* metric_denied=d::FindCanonicalDiagnosticCode("METRIC.ACCESS_DENIED");
  Check(metric_denied&&metric_denied->sqlstate=="42501"&&metric_denied->numeric_binding=="not_applicable",
        "metric visibility refusal must retain authorization SQLSTATE");
  Sample("FILESPACE_AGENT.ROLE_DENIED",S::error,true,
         "only_after_authorized_role_change_and_fresh_evidence",
         "refuse_capacity_action_without_physical_mutation","FILESPACE_AGENT");
  Sample("FILESPACE_AGENT.HEALTH_DENIED",S::error,true,
         "only_after_fresh_health_evidence_and_policy_revalidation",
         "refuse_capacity_action_without_physical_mutation","FILESPACE_AGENT");
  Sample("SB_ENGINE_API_EMBEDDED_TRUST_MODE",S::informational,false,
         "not_applicable","preserve_outcome_and_report_embedded_trust_context","ENGINE_API");
  Sample("AGENT.PAGE_PREALLOCATION.COMPLETED",S::informational,false,
         "not_applicable","retain_source_action_evidence_without_implying_transaction_commit","AGENT");
  for (const auto code : {"SB_ENGINE_API_EMBEDDED_TRUST_MODE", "AGENT.PAGE_PREALLOCATION.COMPLETED"}) {
    const auto* row = d::FindCanonicalDiagnosticCode(code);
    Check(row && row->sqlstate == "00000" && row->numeric_binding == "not_applicable",
          "retained informational source notice metadata drifted");
  }
  Sample("DML.NATIVE_BULK_INGEST.DISABLED",S::error,true,
         "only_after_authorized_enablement_and_fresh_admission",
         "reject_without_native_bulk_mutation","DML");
  const auto* native_disabled=d::FindCanonicalDiagnosticCode("DML.NATIVE_BULK_INGEST.DISABLED");
  Check(native_disabled && native_disabled->sqlstate=="0A000" &&
        native_disabled->numeric_binding=="not_applicable",
        "native bulk policy refusal SQLSTATE or numeric binding drifted");
  Sample("SB-DB-LIFECYCLE-SHUTDOWN-IDENTITY-INVALID",S::error,true,
         "only_after_context_authority_revalidation","reject_before_durable_mutation",
         "DATABASE_LIFECYCLE");
  Sample("SB-DB-LIFECYCLE-SHUTDOWN-IDENTITY-MISMATCH",S::error,true,
         "only_after_context_authority_revalidation","reject_before_durable_mutation",
         "DATABASE_LIFECYCLE");
  Sample("STORAGE.PAGE_CHECKSUM_FAILED",S::corruption,true,"false","repair_required","STORAGE");
  Sample("STORAGE.GROWTH.RETRY_CONFLICT",S::error,true,
         "only_original_exact_intent_or_fresh_separately_admitted_operation",
         "reject_without_mutation_preserve_original_growth_operation","STORAGE");
  Sample("filespace_growth_quarantine",S::error,true,
         "only_after_authoritative_physical_reconciliation_and_fresh_admission",
         "refuse_capacity_mutation_preserve_prior_physical_effects","STORAGE");
  for (const auto code : {"SB-FILESPACE-HEADER-DATABASE-UUID-MISMATCH",
                         "SB-FILESPACE-HEADER-FILESPACE-UUID-MISMATCH"})
    Sample(code,S::error,true,"only_after_corrected_owner_binding_and_fresh_admission",
           "reject_before_mutation_preserve_actual_filespace","STORAGE");
  Sample("SB-FILESPACE-HEADER-PAGE-SIZE-MISMATCH",S::error,true,
         "only_after_corrected_profile_binding_and_fresh_admission",
         "reject_before_mutation_preserve_actual_filespace","STORAGE");
  Sample("SB-FILESPACE-HEADER-FILE-SIZE-CAPACITY-MISMATCH",S::error,true,
         "only_after_authoritative_physical_reconciliation_and_fresh_admission",
         "refuse_capacity_publication_preserve_actual_file_contents","STORAGE");
  Sample("SB-STORAGE-DISK-WRITE-FAILED",S::error,true,
         "only_after_reconciling_possible_mutation_and_fresh_admission",
         "retain_transferred_bytes_and_prior_effects_without_completed_write_claim","STORAGE");
  Sample("SB-STORAGE-DISK-SYNC-FAILED",S::error,true,
         "only_after_reconciling_durability_and_fresh_admission",
         "retain_prior_writes_without_failed_barrier_durability_claim","STORAGE");
  Sample("SB-STORAGE-DISK-READ-SHORT",S::error,true,"only_after_retained_device_revalidation",
         "reject_observation_preserve_transfer_and_prior_effect_facts","STORAGE");
  for (const auto code : {"STORAGE.GROWTH.RETRY_CONFLICT", "filespace_growth_quarantine",
                         "SB-FILESPACE-HEADER-DATABASE-UUID-MISMATCH",
                         "SB-FILESPACE-HEADER-FILESPACE-UUID-MISMATCH",
                         "SB-FILESPACE-HEADER-PAGE-SIZE-MISMATCH",
                         "SB-FILESPACE-HEADER-FILE-SIZE-CAPACITY-MISMATCH",
                         "SB-STORAGE-DISK-WRITE-FAILED", "SB-STORAGE-DISK-SYNC-FAILED",
                         "SB-STORAGE-DISK-READ-SHORT"}) {
    const auto* row = d::FindCanonicalDiagnosticCode(code);
    const std::string_view name(code);
    const auto expected_state = name == "STORAGE.GROWTH.RETRY_CONFLICT" ? "22023" :
        name.starts_with("SB-STORAGE-DISK-") ? "58030" : "55000";
    Check(row && row->sqlstate == expected_state && row->numeric_binding == "not_applicable",
          "storage diagnostic SQLSTATE or native numeric binding drifted");
  }
  Sample("NUMERIC.BACKEND.UNAVAILABLE",S::error,true,"retry_after_reference_backend_restored","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.ENCODING.NONCANONICAL",S::error,true,"retry_only_with_corrected_encoding","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.REAL128.DIVIDE_BY_ZERO",S::error,true,"retry_only_with_corrected_input","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.REAL128.INVALID",S::error,true,"retry_only_with_corrected_input_or_context","reject_invalid_operation_or_report_unordered_comparison","NUMERIC");
  Sample("NUMERIC.REAL128.OVERFLOW",S::error,true,"retry_only_with_corrected_input_or_context","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.REAL128.UNDERFLOW",S::warning,false,"not_applicable","preserve_rounded_value_and_underflow_inexact_facts","NUMERIC");
  Sample("NUMERIC.REAL64.DIVIDE_BY_ZERO",S::error,true,"retry_only_with_corrected_input","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.REAL64.INVALID",S::error,true,"retry_only_with_corrected_input_or_context","reject_invalid_operation_or_report_unordered_comparison","NUMERIC");
  Sample("NUMERIC.REAL64.OVERFLOW",S::error,true,"retry_only_with_corrected_input_or_context","reject_without_numeric_value","NUMERIC");
  Sample("NUMERIC.REAL64.UNDERFLOW",S::warning,false,"not_applicable","preserve_rounded_value_and_underflow_inexact_facts","NUMERIC");
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
                                      "MGA.TRANSACTION.STALE","CATALOG.SNAPSHOT_STALE",
                                      "agent.invalid_state","DIAG.MGA.CONCURRENCY.LATCH_TIMEOUT"}) {
    allocation_forbidden=true;const auto* found=d::FindCanonicalDiagnosticCode(unknown);
    allocation_forbidden=false;
    Check(found==nullptr,"unknown code was invented, normalized or guessed");
  }
  constexpr std::array<std::uint8_t,32> expected_source{
    0x42,0x21,0x83,0x3c,0xee,0x3e,0x0e,0x02,0x43,0x2d,0x61,0x66,0x5c,0x65,0x4a,0x5c,0xa2,0x22,0x73,0xd3,0x14,0xce,0xf4,0x0d,0x70,0x68,0x92,0xb0,0xfd,0x82,0xa8,0x2b};
  Check(d::CanonicalDiagnosticCodeSourceSha256()==expected_source,"Core source provenance differs");
  std::cout<<"canonical_diagnostic_catalog rows="<<catalog.size<<" checks="<<checks
           <<" failures="<<failures<<'\n';
  return failures?1:0;
}
