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
  // Prior1379 registrations plus five admitted PREPARED failures and one UUID
  // timestamp-bound refusal. Check exact Core import, not a minimum row count.
  Check(catalog.size==1385 && catalog.data!=nullptr,"complete Core code inventory missing");
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
  Sample("ACID.PARTIAL_OUTCOME_DETECTED",S::critical,true,"false","not_specified","ACID");
  Sample("MANAGER.NO_SPIN_REQUIRED",S::fatal,true,"false","not_specified","MANAGER");
  Sample("SBSQL.METADATA.CATALOG_SQL_FORBIDDEN",S::internal,true,
         "false","block_implementation_path","SBSQL");
  Sample("DIAG.CODE_UNKNOWN",S::error,true,"false","reject_operation","DIAG");
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
                                      "DATATYPE.DESCRIPTOR_INVALID"}) {
    allocation_forbidden=true;const auto* found=d::FindCanonicalDiagnosticCode(unknown);
    allocation_forbidden=false;
    Check(found==nullptr,"unknown code was invented, normalized or guessed");
  }
  constexpr std::array<std::uint8_t,32> expected_source{
    0x85,0x19,0x79,0x75,0x8d,0x5f,0xc1,0x21,0xf8,0x5f,0x81,0xac,0x83,0x5d,0xc2,0x2a,
    0x83,0xf3,0x8e,0x94,0x93,0x70,0xb0,0x3c,0xac,0x46,0x88,0x68,0x61,0xbe,0xc1,0xc6};
  Check(d::CanonicalDiagnosticCodeSourceSha256()==expected_source,"Core source provenance differs");
  std::cout<<"canonical_diagnostic_catalog rows="<<catalog.size<<" checks="<<checks
           <<" failures="<<failures<<'\n';
  return failures?1:0;
}
