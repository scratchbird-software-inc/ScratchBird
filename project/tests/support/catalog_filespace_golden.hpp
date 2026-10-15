// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_filespace_record_codec.hpp"
#include <string>
namespace scratchbird::tests::catalog_filespace_golden {
namespace c=scratchbird::core::catalog;
namespace p=scratchbird::core::platform;
inline void Put(std::string& s,std::size_t offset,p::u64 value,unsigned count) {
  for(unsigned i=0;i<count;++i) s[offset+i]=static_cast<char>((value>>(8*i))&255);
}
inline std::string Golden(const c::CatalogFilespaceRecord& r) {
  // Independent Core field offsets, widths and tags; no encoder/schema calls.
  std::string s(464, '\0');
  s.replace(0,4,"SBCV");
  Put(s,4,1,2); Put(s,6,24,2); Put(s,8,464,4); Put(s,12,37,4);
  Put(s,16,65538,4); Put(s,20,1,2);
  Put(s,24,1,2); Put(s,26,5,1); Put(s,28,16,4);
  s.replace(32,16,reinterpret_cast<const char*>(r.filespace_uuid.value.bytes.data()),16);
  Put(s,48,2,2); Put(s,50,5,1); Put(s,52,16,4);
  s.replace(56,16,reinterpret_cast<const char*>(r.database_uuid.value.bytes.data()),16);
  Put(s,72,3,2); Put(s,74,1,1); Put(s,76,8,4);
  Put(s,80,r.filespace_role,8);
  Put(s,88,4,2); Put(s,90,2,1); Put(s,92,1,4);
  Put(s,96,r.first_filespace,1);
  Put(s,97,5,2); Put(s,99,2,1); Put(s,101,1,4);
  Put(s,105,r.startup_authority,1);
  Put(s,106,6,2); Put(s,108,2,1); Put(s,110,1,4);
  Put(s,114,r.catalog_persistence_owner,1);
  Put(s,115,7,2); Put(s,117,2,1); Put(s,119,1,4);
  Put(s,123,r.filespace_manifest_owner,1);
  Put(s,124,8,2); Put(s,126,2,1); Put(s,128,1,4);
  Put(s,132,r.recovery_evidence_owner,1);
  Put(s,133,9,2); Put(s,135,2,1); Put(s,137,1,4);
  Put(s,141,r.read_only,1);
  Put(s,142,10,2); Put(s,144,1,1); Put(s,146,8,4);
  Put(s,150,r.state,8);
  Put(s,158,11,2); Put(s,160,1,1); Put(s,162,8,4);
  Put(s,166,r.physical_filespace_id,8);
  Put(s,174,12,2); Put(s,176,1,1); Put(s,178,8,4);
  Put(s,182,r.lifecycle_generation,8);
  Put(s,190,13,2); Put(s,192,1,1); Put(s,194,8,4);
  Put(s,198,r.filespace_manifest_generation,8);
  Put(s,206,14,2); Put(s,208,1,1); Put(s,210,8,4);
  Put(s,214,r.catalog_manifest_format_version,8);
  Put(s,222,15,2); Put(s,224,1,1); Put(s,226,8,4);
  Put(s,230,r.resource_seed_manifest_format_version,8);
  Put(s,238,16,2); Put(s,240,1,1); Put(s,242,8,4);
  Put(s,246,r.registered_txn,8);
  Put(s,254,17,2); Put(s,256,1,1); Put(s,258,8,4);
  Put(s,262,r.last_lifecycle_transaction,8);
  Put(s,270,18,2); Put(s,272,1,1); Put(s,274,8,4);
  Put(s,278,r.uuid_source,8);
  Put(s,286,19,2); Put(s,288,2,1); Put(s,290,1,4);
  Put(s,294,r.header_database_uuid_match_required,1);
  Put(s,295,20,2); Put(s,297,2,1); Put(s,299,1,4);
  Put(s,303,r.header_filespace_uuid_match_required,1);
  Put(s,304,21,2); Put(s,306,2,1); Put(s,308,1,4);
  Put(s,312,r.startup_state_coupled,1);
  Put(s,313,22,2); Put(s,315,2,1); Put(s,317,1,4);
  Put(s,321,r.page_header_coupled,1);
  Put(s,322,23,2); Put(s,324,2,1); Put(s,326,1,4);
  Put(s,330,r.open_validate_header,1);
  Put(s,331,24,2); Put(s,333,2,1); Put(s,335,1,4);
  Put(s,339,r.attach_admission_validate_header,1);
  Put(s,340,25,2); Put(s,342,2,1); Put(s,344,1,4);
  Put(s,348,r.transaction_admission_validate_filespace,1);
  Put(s,349,26,2); Put(s,351,2,1); Put(s,353,1,4);
  Put(s,357,r.maintenance_validate_header,1);
  Put(s,358,27,2); Put(s,360,2,1); Put(s,362,1,4);
  Put(s,366,r.verify_repair_validate_header,1);
  Put(s,367,28,2); Put(s,369,2,1); Put(s,371,1,4);
  Put(s,375,r.shutdown_validate_header,1);
  Put(s,376,29,2); Put(s,378,2,1); Put(s,380,1,4);
  Put(s,384,r.recovery_validate_header,1);
  Put(s,385,30,2); Put(s,387,2,1); Put(s,389,1,4);
  Put(s,393,r.drop_requires_database_lifecycle,1);
  Put(s,394,31,2); Put(s,396,2,1); Put(s,398,1,4);
  Put(s,402,r.quarantine_on_ambiguous,1);
  Put(s,403,32,2); Put(s,405,2,1); Put(s,407,1,4);
  Put(s,411,r.state_change_evidence_before_success,1);
  Put(s,412,33,2); Put(s,414,2,1); Put(s,416,1,4);
  Put(s,420,r.mga_visibility_required,1);
  Put(s,421,34,2); Put(s,423,2,1); Put(s,425,1,4);
  Put(s,429,r.path_is_locator_not_identity,1);
  Put(s,430,35,2); Put(s,432,2,1); Put(s,434,1,4);
  Put(s,438,r.duplicate_identity_refusal,1);
  Put(s,439,36,2); Put(s,441,2,1); Put(s,443,1,4);
  Put(s,447,r.stale_identity_refusal,1);
  Put(s,448,37,2); Put(s,450,1,1); Put(s,452,8,4);
  Put(s,456,r.creator_transaction_number,8);
  return s;
}
} // namespace scratchbird::tests::catalog_filespace_golden
