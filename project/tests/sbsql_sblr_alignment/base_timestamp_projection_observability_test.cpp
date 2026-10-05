// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../../src/core/datatypes/datatype_timestamp_projection.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;
namespace {
unsigned checks = 0;
[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "FAIL " << message << '\n';
  std::exit(1);
}
void Check(bool value, std::string_view message) {
  ++checks;
  if (!value) Fail(message);
}
p::Uuid D710() {
  return p::Uuid{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10}};
}
p::Uuid V7(p::byte discriminator) {
  return p::Uuid{{0x01,0xa1,0x04,0xf5,0,discriminator,0x70,0,0x80,0,0,0,0,0,0,discriminator}};
}
p::Uuid MetricUuid(unsigned index) {
  return p::Uuid{{0x01,0xa1,0x04,0xf2,0xee,0x69,0x75,0x5a,0xbb,0xe0,0x85,0x9c,0xc1,0xc1,0xc8,
                  static_cast<p::byte>(0x3f + index)}};
}
p::Uuid IndexUuid(p::byte tail) {
  return p::Uuid{{0x01,0xa1,0x04,0xfb,0x3b,0xb7,0x77,0x16,0x93,0x9b,0x99,0x85,0x45,0xbd,0xbd,tail}};
}
std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> Profile() {
  auto result = dt::BuildCurrentTimestampValidatedProfileHandleV3(D710());
  Check(result.ok(), "build d710 profile");
  return std::make_shared<const dt::TimestampValidatedProfileHandleV3>(std::move(result.profile));
}
dt::TimestampOwnedValueV3 Value(
    const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile,
    std::int32_t day, p::u64 nanos) {
  return {profile, dt::TimestampValueStateV3::value, day, nanos};
}
dt::TimestampOwnedValueV3 Null(
    const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  return {profile, dt::TimestampValueStateV3::sql_null, 0, 0};
}
bool AlwaysCancel(void*) noexcept { return true; }

void TestIndexFamiliesAndPredicates() {
  using D = dt::TimestampIndexDispositionV3;
  using K = dt::TimestampProjectionKindV3;
  using F = dt::TimestampIndexFamilyV3;
  using O = dt::TimestampIndexPredicateOperationV3;
  using X = dt::TimestampIndexPredicateFactV3;
  const std::array<D,16> dispositions{{
      D::not_applicable,D::admitted_with_recheck,D::admitted_with_recheck,
      D::admitted_exact_if_budget,D::admitted_with_recheck,D::admitted_payload_only,
      D::not_applicable,D::conditional,D::not_applicable,D::not_applicable,
      D::admitted_with_recheck,D::conditional,D::admitted_exact_if_budget,
      D::not_applicable,D::admitted_exact_selected_mode,D::not_applicable}};
  const std::array<K,16> projections{{
      K::none,K::equality_hash,K::zone_map,K::sort_key,K::zone_map,
      K::covering_value,K::none,K::conditional_result,K::none,K::none,
      K::equality_hash,K::conditional_underlying,K::range_pair,K::none,K::none,K::none}};
  const std::array<bool,16> rechecks{{false,true,true,false,true,false,false,true,
                                     false,false,true,true,false,false,false,false}};
  const std::array<bool,16> owner{{true,false,false,false,false,false,true,true,
                                  true,true,false,true,false,true,false,true}};
  const std::array<p::byte,16> uuid_tails{{0x7c,0x6f,0x70,0x6d,0x71,0x72,0x77,0x73,
                                           0x78,0x79,0x6e,0x74,0x75,0x7a,0x76,0x7b}};
  std::set<std::array<p::byte,16>> seen;
  for (unsigned i=0;i<16;++i) {
    const auto family=static_cast<F>(i);
    const auto row=dt::ResolveTimestampIndexFamilyV3(family);
    Check(row.family==family && row.compatibility_generation==1,
          "index family and generation");
    Check(row.compatibility_uuid==IndexUuid(uuid_tails[i]) &&
              row.disposition==dispositions[i] && row.projection==projections[i] &&
              row.exact_recheck_required==rechecks[i] &&
              row.receiving_owner_resolution_required==owner[i],
          "exact index family registry row");
    Check(seen.insert(row.compatibility_uuid.bytes).second,"index UUID uniqueness");
    auto identity=dt::LookupTimestampIndexCompatibilityIdentityV3(family);
    Check(dt::AdmitTimestampIndexCompatibilityV3(identity).admitted,
          "exact index identity admits");
    auto mutation=identity; mutation.compatibility_uuid.bytes[0]^=1;
    Check(!dt::AdmitTimestampIndexCompatibilityV3(mutation).admitted,
          "index UUID mutation refuses");
    mutation=identity; ++mutation.compatibility_generation;
    Check(!dt::AdmitTimestampIndexCompatibilityV3(mutation).admitted,
          "index generation mutation refuses");
    mutation=identity;
    mutation.compatibility_uuid=dt::LookupTimestampIndexCompatibilityIdentityV3(
        static_cast<F>((i+1)%16)).compatibility_uuid;
    Check(!dt::AdmitTimestampIndexCompatibilityV3(mutation).admitted,
          "cross-family identity refuses");
  }
  Check(!dt::AdmitTimestampIndexCompatibilityV3({}).admitted,
        "missing index identity refuses");
  Check(!dt::AdmitTimestampIndexCompatibilityV3(
            {static_cast<F>(255),V7(9),1}).admitted,
        "unknown index family refuses");

  const auto expected=[](F family,O operation) {
    if ((family==F::bitmap && (operation==O::equality||operation==O::in||operation==O::is_null)) ||
        (family==F::hash && (operation==O::equality||operation==O::in)) ||
        (family==F::temporary_work && operation==O::equality))
      return X::requires_exact_state_component_recheck_never_final_match;
    if ((family==F::brin_like && (operation==O::equality||operation==O::range||operation==O::order)) ||
        (family==F::columnar_zone_map && (operation==O::prune_range||operation==O::equality)))
      return X::prune_only_mandatory_source_row_predicate_recheck_never_final_match;
    if ((family==F::btree && (operation==O::equality||operation==O::range||operation==O::order)) ||
        (family==F::covering_included && operation==O::include_payload) ||
        (family==F::range_exclusion && (operation==O::range||operation==O::overlap||operation==O::exclusion)) ||
        (family==F::temporary_work && operation==O::order))
      return X::exact_no_recheck_after_decode_reencode;
    return X::refused;
  };
  for(unsigned fi=0;fi<16;++fi) {
    const auto family=static_cast<F>(fi);
    for(unsigned oi=0;oi<10;++oi) {
      const auto operation=static_cast<O>(oi);
      const auto result=dt::ResolveTimestampIndexPredicateV3(
          {dt::LookupTimestampIndexCompatibilityIdentityV3(family),operation});
      const auto fact=expected(family,operation);
      if(fact==X::refused) {
        Check(!result.admitted,"refused predicate matrix cell");
      } else {
        Check(result.admitted && result.fact==fact,
              "admitted predicate matrix cell");
      }
    }
  }
  const auto selected=dt::LookupTimestampIndexCompatibilityIdentityV3(F::btree);
  for(const auto conditional:{F::expression,F::partial_filtered}) {
    const auto requested=dt::LookupTimestampIndexCompatibilityIdentityV3(conditional);
    const auto delegated=dt::ResolveTimestampIndexPredicateV3(
        {requested,O::range,&selected});
    Check(delegated.admitted && delegated.fact==X::exact_no_recheck_after_decode_reencode &&
              delegated.effective_resolution.family==F::btree &&
              delegated.predicate_owner_handoff_preserved==(conditional==F::partial_filtered),
          "conditional predicate delegates exactly");
    Check(!dt::ResolveTimestampIndexPredicateV3({requested,O::range,nullptr}).admitted,
          "conditional predicate needs selected family");
  }
  for(unsigned fact=0;fact<4;++fact) {
    for(unsigned bits=0;bits<8;++bits) {
      const bool projection=bits&1, exact=bits&2, source=bits&4;
      const auto result=dt::ClassifyTimestampIndexCandidateV3(
          static_cast<X>(fact),projection,exact,source);
      if(!projection||fact==3) {
        Check(result.disposition==dt::TimestampIndexCandidateDispositionV3::not_a_candidate &&
                  !result.final_match,"candidate refusal truth table");
      } else if(fact==0) {
        Check(result.final_match &&
                  result.disposition==dt::TimestampIndexCandidateDispositionV3::exact_match &&
                  !result.exact_state_component_recheck_required &&
                  !result.source_row_predicate_recheck_required,
              "exact candidate truth table");
      } else if(fact==1) {
        Check(result.exact_state_component_recheck_required &&
                  result.final_match==exact,"hash collision exact recheck truth table");
      } else {
        Check(result.source_row_predicate_recheck_required &&
                  result.final_match==source,"zone prune source-row recheck truth table");
      }
    }
  }
  const auto unknown=dt::ClassifyTimestampIndexCandidateV3(static_cast<X>(255),true,true,true);
  Check(unknown.disposition==dt::TimestampIndexCandidateDispositionV3::not_a_candidate &&
            !unknown.final_match,"unknown candidate fact fails closed");
}

void TestProjectionLayouts(
    const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  using F=dt::TimestampIndexFamilyV3;
  using M=dt::TimestampProjectionModeV3;
  const auto lo=Value(profile,-1,86'399'999'999'999ull);
  const auto hi=Value(profile,0,0);
  const auto nullv=Null(profile);
  const auto cover_null=dt::EncodeTimestampCoveringValueV3(nullv);
  const auto cover_lo=dt::EncodeTimestampCoveringValueV3(lo);
  Check(cover_null.ok()&&cover_null.bytes==std::vector<p::byte>{0},
        "exact covering NULL vector");
  Check(cover_lo.ok()&&cover_lo.bytes.size()==17&&cover_lo.bytes[0]==2,
        "exact covering PRESENT extent and state");
  auto cover_view=dt::DecodeTimestampCoveringValueNoAllocV3(*profile,true,cover_lo.bytes);
  Check(cover_view.ok()&&cover_view.value.civil_day==-1&&
            cover_view.value.nanoseconds_since_midnight==86'399'999'999'999ull,
        "covering exact decode");
  dt::TimestampExecutionControlV3 decode_budget;
  decode_budget.maximum_allocation_bytes=16;
  auto cover_budget=dt::DecodeTimestampCoveringValueNoAllocV3(
      *profile,true,cover_lo.bytes,decode_budget);
  Check(!cover_budget.ok()&&cover_budget.diagnostic.diagnostic_code==
            "RESOURCE.BUDGET_EXCEEDED"&&
            cover_budget.diagnostic.detail=="covering_extent_budget",
        "covering decoder enforces full 17-byte resource grant");
  Check(!dt::DecodeTimestampCoveringValueNoAllocV3(*profile,false,cover_null.bytes).ok(),
        "covering NULL policy");
  auto bad_cover=cover_lo.bytes;bad_cover[0]=1;
  Check(!dt::DecodeTimestampCoveringValueNoAllocV3(*profile,true,bad_cover).ok(),
        "covering state mutation refuses");
  bad_cover=cover_lo.bytes;bad_cover[13]=1;
  Check(!dt::DecodeTimestampCoveringValueNoAllocV3(*profile,true,bad_cover).ok(),
        "covering reserved mutation refuses");
  bad_cover=cover_lo.bytes;bad_cover.pop_back();
  Check(!dt::DecodeTimestampCoveringValueNoAllocV3(*profile,true,bad_cover).ok(),
        "covering short extent refuses");
  bad_cover=cover_lo.bytes;bad_cover.push_back(0);
  Check(!dt::DecodeTimestampCoveringValueNoAllocV3(*profile,true,bad_cover).ok(),
        "covering trailing byte refuses");

  const auto range=dt::EncodeTimestampRangePairV3(lo,hi);
  Check(range.ok()&&range.bytes.size()==224,"exact range extent");
  const auto range_view=dt::DecodeTimestampRangePairNoAllocV3(*profile,range.bytes);
  Check(range_view.ok()&&range_view.value.lower_key.size()==112&&
            range_view.value.upper_key.size()==112&&
            range_view.value.lower_civil_day==-1&&range_view.value.upper_civil_day==0,
        "range exact decode");
  decode_budget.maximum_allocation_bytes=223;
  auto range_budget=dt::DecodeTimestampRangePairNoAllocV3(
      *profile,range.bytes,decode_budget);
  Check(!range_budget.ok()&&range_budget.diagnostic.diagnostic_code==
            "RESOURCE.BUDGET_EXCEEDED"&&
            range_budget.diagnostic.detail=="range_extent_budget",
        "range decoder enforces full 224-byte resource grant");
  auto malformed_upper=range.bytes;malformed_upper[210]^=1;
  decode_budget.maximum_allocation_bytes=0;
  auto range_precedence=dt::DecodeTimestampRangePairNoAllocV3(
      *profile,malformed_upper,decode_budget);
  Check(!range_precedence.ok()&&range_precedence.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "range validates malformed upper key before parent resource gate");
  Check(!dt::EncodeTimestampRangePairV3(hi,lo).ok(),"inverted range refuses");
  for(const std::size_t i:{0u,8u,16u,40u,80u,100u,112u,120u,128u,152u,192u,212u}){
    auto b=range.bytes;b[i]^=1;
    Check(!dt::DecodeTimestampRangePairNoAllocV3(*profile,b).ok(),
          "range structural or identity mutation refuses");}
  auto short_range=range.bytes;short_range.pop_back();
  Check(!dt::DecodeTimestampRangePairNoAllocV3(*profile,short_range).ok(),
        "range short extent refuses");
  auto trailing_range=range.bytes;trailing_range.push_back(0);
  Check(!dt::DecodeTimestampRangePairNoAllocV3(*profile,trailing_range).ok(),
        "range trailing extent refuses");

  dt::TimestampZoneMapRequestV3 empty;
  empty.profile=profile;empty.null_count=1;
  const auto zone_empty=dt::EncodeTimestampZoneMapV3(empty);
  Check(zone_empty.ok()&&zone_empty.bytes.size()==112&&
            std::memcmp(zone_empty.bytes.data(),"SBTSPZ01",8)==0,
        "exact empty zone vector");
  dt::TimestampZoneMapRequestV3 populated;
  populated.profile=profile;populated.minimum=&lo;populated.maximum=&hi;
  populated.value_count=2;
  const auto zone=dt::EncodeTimestampZoneMapV3(populated);
  Check(zone.ok()&&zone.bytes.size()==336&&
            std::memcmp(zone.bytes.data(),"SBTSPZ01",8)==0,
        "exact populated zone vector");
  const auto zone_view=dt::DecodeTimestampZoneMapNoAllocV3(*profile,zone.bytes);
  Check(zone_view.ok()&&zone_view.value.null_count==0&&zone_view.value.value_count==2&&
            zone_view.value.minimum_key.size()==112&&zone_view.value.maximum_key.size()==112,
        "zone exact decode");
  decode_budget.maximum_allocation_bytes=335;
  auto zone_budget=dt::DecodeTimestampZoneMapNoAllocV3(
      *profile,zone.bytes,decode_budget);
  Check(!zone_budget.ok()&&zone_budget.diagnostic.diagnostic_code==
            "RESOURCE.BUDGET_EXCEEDED"&&
            zone_budget.diagnostic.detail=="zone_extent_budget",
        "zone decoder enforces full 336-byte resource grant");
  for(const std::size_t legacy:{96u,312u,320u}) {
    std::vector<p::byte> b(legacy);
    Check(!dt::DecodeTimestampZoneMapNoAllocV3(*profile,b).ok(),
          "legacy zone extent refuses before interpretation");
  }
  for(std::size_t i=0;i<zone.bytes.size();++i){auto b=zone.bytes;b[i]^=1;
    Check(!dt::DecodeTimestampZoneMapNoAllocV3(*profile,b).ok(),
          "zone byte mutation refuses");}
  auto bad_empty=empty;bad_empty.null_count=2;
  Check(!dt::EncodeTimestampZoneMapV3(bad_empty).ok(),"generalized empty zone population refuses");
  auto bad_zone=populated;bad_zone.value_count=3;
  Check(!dt::EncodeTimestampZoneMapV3(bad_zone).ok(),"generalized populated zone population refuses");

  for(const auto mode:{M::ascending_nulls_first,M::ascending_nulls_last,
                       M::descending_nulls_first,M::descending_nulls_last}) {
    dt::TimestampIndexProjectionRequestV3 q;
    q.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::btree);
    q.mode=mode;q.value=&lo;
    auto result=dt::ProjectTimestampIndexValueV3(q);
    Check(result.ok()&&result.bytes.size()==112,"all btree projection modes");
    Check(dt::DecodeTimestampSortKeyNoAllocV3(*profile,result.bytes).ok(),
          "btree projection decodes");
    decode_budget.maximum_allocation_bytes=111;
    auto key_budget=dt::DecodeTimestampSortKeyNoAllocV3(
        *profile,result.bytes,decode_budget);
    Check(!key_budget.ok()&&key_budget.diagnostic.diagnostic_code==
              "RESOURCE.BUDGET_EXCEEDED",
          "sort-key decoder enforces full 112-byte resource grant");
    q.provider_max_key_bytes=111;
    Check(!dt::ProjectTimestampIndexValueV3(q).ok(),"btree one-short provider refusal");
  }
  for(const auto family:{F::bitmap,F::hash}) {
    dt::TimestampIndexProjectionRequestV3 q;
    q.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(family);
    q.mode=M::equality_hash;q.value=&lo;
    const auto result=dt::ProjectTimestampIndexValueV3(q);
    const auto hash=dt::HashTimestampValueV3(lo);
    Check(result.ok()&&hash.ok()&&result.bytes==hash.bytes,"hash family exact projection");
    q.provider_max_key_bytes=31;
    Check(!dt::ProjectTimestampIndexValueV3(q).ok(),"hash one-short provider refusal");
  }
  for(const auto family:{F::brin_like,F::columnar_zone_map}) {
    dt::TimestampIndexProjectionRequestV3 q;
    q.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(family);
    q.mode=M::zone_map;q.value=&lo;q.zone_map=&populated;
    Check(dt::ProjectTimestampIndexValueV3(q).bytes==zone.bytes,
          "zone family exact projection");
    q.provider_max_key_bytes=335;
    Check(!dt::ProjectTimestampIndexValueV3(q).ok(),"zone family one-short provider refusal");
  }
  dt::TimestampIndexProjectionRequestV3 covering;
  covering.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::covering_included);
  covering.mode=M::payload;covering.value=&lo;
  Check(dt::ProjectTimestampIndexValueV3(covering).bytes==cover_lo.bytes,
        "covering family exact projection");
  covering.provider_max_key_bytes=16;
  Check(!dt::ProjectTimestampIndexValueV3(covering).ok(),"covering one-short provider refusal");
  dt::TimestampIndexProjectionRequestV3 range_q;
  range_q.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::range_exclusion);
  range_q.mode=M::ascending_nulls_last_pair;range_q.value=&lo;range_q.upper_value=&hi;
  Check(dt::ProjectTimestampIndexValueV3(range_q).bytes==range.bytes,
        "range family exact projection");
  range_q.provider_max_key_bytes=223;
  Check(!dt::ProjectTimestampIndexValueV3(range_q).ok(),"range one-short provider refusal");
  for(const auto mode:{M::equality_hash,M::ascending_nulls_first,M::ascending_nulls_last,
                       M::descending_nulls_first,M::descending_nulls_last}) {
    dt::TimestampIndexProjectionRequestV3 q;
    q.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::temporary_work);
    q.mode=mode;q.value=&lo;
    const auto result=dt::ProjectTimestampIndexValueV3(q);
    Check(result.ok()&&result.bytes.size()==(mode==M::equality_hash?32:112),
          "temporary selected projection mode");
  }
  const auto selected=dt::ResolveTimestampIndexFamilyV3(F::btree);
  for(const auto family:{F::expression,F::partial_filtered}) {
    dt::TimestampIndexProjectionRequestV3 q;
    q.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(family);
    q.mode=M::ascending_nulls_last;q.value=&lo;q.selected_conditional_family=&selected;
    auto result=dt::ProjectTimestampIndexValueV3(q);
    Check(result.ok()&&result.requested_resolution.family==family&&
              result.resolution.family==F::btree,"conditional projection identity");
    q.selected_conditional_family=nullptr;
    result=dt::ProjectTimestampIndexValueV3(q);
    Check(!result.ok()&&result.owner_fact_only,"conditional projection owner handoff");
  }
  for(const auto family:{F::aggregate_sketch,F::document_path,F::full_text,F::graph,
                         F::spatial,F::vector_ann}) {
    dt::TimestampIndexProjectionRequestV3 q;
    q.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(family);q.value=&lo;
    const auto result=dt::ProjectTimestampIndexValueV3(q);
    Check(!result.ok()&&result.owner_fact_only&&
              result.diagnostic.diagnostic_code=="OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
          "not-applicable index owner handoff");
  }
  dt::TimestampIndexProjectionRequestV3 invalid;
  invalid.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::btree);
  invalid.mode=static_cast<M>(255);invalid.value=&lo;
  Check(!dt::ProjectTimestampIndexValueV3(invalid).ok(),"unknown projection mode refuses");

  auto invalid_present=lo;
  invalid_present.nanoseconds_since_midnight=86'400'000'000'000ull;
  dt::TimestampIndexProjectionRequestV3 precedence;
  precedence.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::btree);
  precedence.mode=M::ascending_nulls_last;precedence.value=&invalid_present;
  precedence.provider_max_key_bytes=111;
  auto refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code=="CTI.TEMPORAL.INDEX_KEY_REFUSED",
        "btree provider budget precedes invalid PRESENT payload");
  precedence.provider_max_key_bytes=112;
  precedence.control.maximum_allocation_bytes=111;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED",
        "btree resource budget precedes invalid PRESENT payload");
  precedence.control.maximum_allocation_bytes=112;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "btree invalid PRESENT payload follows complete budgets");

  auto invalid_upper=hi;
  invalid_upper.nanoseconds_since_midnight=86'400'000'000'000ull;
  precedence={};
  precedence.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::range_exclusion);
  precedence.mode=M::ascending_nulls_last_pair;precedence.value=&lo;
  precedence.upper_value=&invalid_upper;precedence.provider_max_key_bytes=223;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code=="CTI.TEMPORAL.INDEX_KEY_REFUSED",
        "range provider budget precedes invalid PRESENT upper bound");
  precedence.provider_max_key_bytes=224;precedence.control.maximum_allocation_bytes=223;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED",
        "range resource budget precedes invalid PRESENT upper bound");
  precedence.control.maximum_allocation_bytes=224;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "range invalid PRESENT upper bound follows complete budgets");

  auto bad_profile=std::make_shared<dt::TimestampValidatedProfileHandleV3>(*profile);
  bad_profile->profile_fingerprint[0]^=1;
  auto invalid_lower=lo;
  invalid_lower.nanoseconds_since_midnight=86'400'000'000'000ull;
  dt::TimestampOwnedValueV3 invalid_profile_upper{
      bad_profile,dt::TimestampValueStateV3::value,0,0};
  auto direct_precedence=dt::EncodeTimestampRangePairV3(
      invalid_lower,invalid_profile_upper);
  Check(!direct_precedence.ok()&&direct_precedence.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "direct range validates every profile before any PRESENT payload");
  precedence={};
  precedence.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::range_exclusion);
  precedence.mode=M::ascending_nulls_last_pair;
  precedence.value=&invalid_lower;precedence.upper_value=&invalid_profile_upper;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "range projection validates every profile before any PRESENT payload");

  auto invalid_minimum=lo;
  invalid_minimum.nanoseconds_since_midnight=86'400'000'000'000ull;
  auto invalid_zone=populated;invalid_zone.minimum=&invalid_minimum;
  precedence={};
  precedence.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::columnar_zone_map);
  precedence.mode=M::zone_map;precedence.value=&lo;precedence.zone_map=&invalid_zone;
  precedence.provider_max_key_bytes=335;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code=="CTI.TEMPORAL.INDEX_KEY_REFUSED",
        "zone provider budget precedes invalid PRESENT extremum");
  precedence.provider_max_key_bytes=336;precedence.control.maximum_allocation_bytes=335;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code=="RESOURCE.BUDGET_EXCEEDED",
        "zone resource budget precedes invalid PRESENT extremum");
  precedence.control.maximum_allocation_bytes=336;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "zone invalid PRESENT extremum follows complete budgets");

  dt::TimestampOwnedValueV3 invalid_profile_minimum{
      bad_profile,dt::TimestampValueStateV3::value,-1,0};
  auto invalid_profile_zone=populated;
  invalid_profile_zone.minimum=&invalid_profile_minimum;
  invalid_profile_zone.value_count=3;
  direct_precedence=dt::EncodeTimestampZoneMapV3(invalid_profile_zone);
  Check(!direct_precedence.ok()&&direct_precedence.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "direct zone validates every profile before population shape");
  auto dirty_query=Null(profile);dirty_query.civil_day=1;
  precedence={};
  precedence.compatibility=dt::LookupTimestampIndexCompatibilityIdentityV3(F::columnar_zone_map);
  precedence.mode=M::zone_map;precedence.value=&dirty_query;
  precedence.zone_map=&invalid_profile_zone;
  refusal=dt::ProjectTimestampIndexValueV3(precedence);
  Check(!refusal.ok()&&refusal.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "zone projection validates every profile before query state or population shape");
}

void SyncEvidence(dt::TimestampStatisticsProjectionV3& projection,
                  const std::shared_ptr<dt::TimestampStatisticsProviderEvidenceHandleV3>& evidence) {
  evidence->source_object_uuid=projection.source_object_uuid;
  evidence->authenticated_population=true;
  evidence->source_authorized=true;
  evidence->privacy_admitted=true;
  evidence->evidence_class=dt::TimestampStatisticsEvidenceClassV3::full_visible_population;
  evidence->schema_epoch=projection.schema_epoch;
  evidence->collection_epoch=projection.collection_epoch;
  evidence->security_epoch=projection.security_epoch;
  evidence->row_count=projection.row_count;
  evidence->null_count=projection.null_count;
  evidence->value_count=projection.value_count;
  evidence->minimum_maximum_present=projection.minimum_maximum_present;
  evidence->minimum_civil_day=projection.minimum_civil_day;
  evidence->minimum_nanoseconds_since_midnight=projection.minimum_nanoseconds_since_midnight;
  evidence->maximum_civil_day=projection.maximum_civil_day;
  evidence->maximum_nanoseconds_since_midnight=projection.maximum_nanoseconds_since_midnight;
  evidence->histogram.assign(projection.histogram.begin(),projection.histogram.end());
  evidence->mcv.assign(projection.mcv.begin(),projection.mcv.end());
  projection.provider_evidence=evidence;
}

void TestStatisticsAndBackup(
    const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  std::array<dt::TimestampStatisticsHistogramRecordV3,2> histogram{{
      {-1,86'399'999'999'999ull,1},{0,0,2}}};
  std::array<dt::TimestampStatisticsMcvRecordV3,2> mcv{};
  mcv[0].value_hash[31]=1;mcv[0].frequency=1;
  mcv[1].value_hash[31]=2;mcv[1].frequency=1;
  auto evidence=std::make_shared<dt::TimestampStatisticsProviderEvidenceHandleV3>();
  evidence->provider_evidence_uuid=V7(3);
  dt::TimestampStatisticsProjectionV3 projection;
  projection.profile=profile;projection.statistics_uuid=V7(1);
  projection.source_object_uuid=V7(2);projection.schema_epoch=11;
  projection.collection_epoch=12;projection.security_epoch=13;
  projection.row_count=2;projection.null_count=0;projection.value_count=2;
  projection.minimum_maximum_present=true;projection.minimum_civil_day=-1;
  projection.minimum_nanoseconds_since_midnight=86'399'999'999'999ull;
  projection.maximum_civil_day=0;projection.maximum_nanoseconds_since_midnight=0;
  projection.histogram=histogram;projection.mcv=mcv;SyncEvidence(projection,evidence);
  auto encoded=dt::EncodeTimestampStatisticsProjectionV3(projection);
  Check(encoded.ok()&&encoded.bytes.size()==688&&
            std::memcmp(encoded.bytes.data(),"SBTSTS01",8)==0,
        "statistics histogram/MCV exact vector");
  const auto evidence_refusal=[&](auto mutate,std::string_view diagnostic) {
    auto candidate_evidence=std::make_shared<dt::TimestampStatisticsProviderEvidenceHandleV3>(*evidence);
    auto candidate=projection;candidate.provider_evidence=candidate_evidence;
    mutate(*candidate_evidence);
    const auto result=dt::EncodeTimestampStatisticsProjectionV3(candidate);
    Check(!result.ok()&&result.diagnostic.diagnostic_code==diagnostic,
          "statistics provider evidence mutation refuses exactly");
  };
  evidence_refusal([](auto& e){e.source_authorized=false;},"SECURITY.ACCESS_DENIED");
  evidence_refusal([](auto& e){e.privacy_admitted=false;},"OPTIMIZER.STATISTICS_PRIVACY_DENIED");
  evidence_refusal([](auto& e){e.authenticated_population=false;},"OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  evidence_refusal([](auto& e){e.evidence_class=dt::TimestampStatisticsEvidenceClassV3::partial_scan;},
                   "OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  evidence_refusal([](auto& e){++e.schema_epoch;},"OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  evidence_refusal([](auto& e){++e.collection_epoch;},"OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  evidence_refusal([](auto& e){++e.security_epoch;},"OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  evidence_refusal([](auto& e){e.source_object_uuid=V7(10);},"OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  evidence_refusal([](auto& e){++e.row_count;},"OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  evidence_refusal([](auto& e){++e.histogram[0].cumulative_count;},"OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  evidence_refusal([](auto& e){++e.mcv[0].frequency;},"OPTIMIZER.STATISTICS_EPOCH_MISMATCH");
  auto decoded=dt::DecodeTimestampStatisticsProjectionV3(profile,encoded.bytes);
  Check(decoded.ok()&&decoded.statistics.row_count==2&&
            decoded.statistics.histogram==std::vector(histogram.begin(),histogram.end())&&
            decoded.statistics.mcv==std::vector(mcv.begin(),mcv.end()),
        "statistics exact decode fields");
  auto reencode=projection;
  reencode.histogram=decoded.statistics.histogram;reencode.mcv=decoded.statistics.mcv;
  SyncEvidence(reencode,evidence);
  Check(dt::EncodeTimestampStatisticsProjectionV3(reencode).bytes==encoded.bytes,
        "statistics byte-identical replay");
  for(std::size_t i=0;i<encoded.bytes.size();++i){auto b=encoded.bytes;b[i]^=1;
    Check(!dt::DecodeTimestampStatisticsProjectionV3(profile,b).ok(),
          "statistics byte mutation refuses");}

  dt::TimestampStatisticsReceivingFactsV3 facts{true,true,11,12,13,evidence->provider_evidence_uuid};
  facts.source_object_uuid=projection.source_object_uuid;
  using RD=dt::TimestampStatisticsReceivingDispositionV3;
  Check(dt::ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics,facts)==RD::admitted,
        "statistics receiving exact facts");
  auto changed=facts;changed.security_visible=false;
  Check(dt::ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics,changed)==RD::security_denied,
        "statistics receiving security");
  changed=facts;changed.privacy_admitted=false;
  Check(dt::ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics,changed)==RD::privacy_denied,
        "statistics receiving privacy");
  changed=facts;++changed.schema_epoch;
  Check(dt::ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics,changed)==RD::schema_epoch_mismatch,
        "statistics receiving schema epoch");
  changed=facts;++changed.collection_epoch;
  Check(dt::ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics,changed)==RD::collection_epoch_mismatch,
        "statistics receiving collection epoch");
  changed=facts;++changed.security_epoch;
  Check(dt::ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics,changed)==RD::security_epoch_mismatch,
        "statistics receiving security epoch");
  changed=facts;changed.provider_evidence_uuid=V7(9);
  Check(dt::ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics,changed)==RD::provider_evidence_mismatch,
        "statistics receiving evidence identity");
  changed=facts;changed.source_object_uuid=V7(10);
  Check(dt::ResolveTimestampStatisticsReceivingFactsV3(decoded.statistics,changed)==RD::provider_evidence_mismatch,
        "statistics receiving source object identity");
  using EC=dt::TimestampStatisticsEvidenceClassV3;
  Check(dt::ResolveTimestampStatisticsEvidenceV3(EC::full_visible_population)==RD::admitted,
        "full visible statistics evidence admits");
  Check(dt::ResolveTimestampStatisticsEvidenceV3(EC::partial_scan)==RD::full_recollection_required&&
            dt::ResolveTimestampStatisticsEvidenceV3(EC::weighted_input)==RD::full_recollection_required&&
            dt::ResolveTimestampStatisticsEvidenceV3(EC::sampled_input)==RD::full_recollection_required&&
            dt::ResolveTimestampStatisticsEvidenceV3(EC::snapshot_merge)==RD::manual_review_required&&
            dt::ResolveTimestampStatisticsEvidenceV3(EC::histogram_source_contradiction)==RD::provider_evidence_mismatch,
        "incomplete statistics evidence refuses");

  auto corrupt=encoded.bytes;corrupt[20]^=1;
  using RA=dt::TimestampStatisticsReadAdmissionDispositionV3;
  auto denied=facts;denied.security_visible=false;denied.privacy_admitted=false;
  auto read=dt::AdmitTimestampStatisticsReadV3(profile,corrupt,denied);
  Check(read.disposition==RA::security_denied&&!read.decode_attempted&&
            read.datatype_outcome_unchanged&&read.metric_outcome_unchanged,
        "statistics security precedes format");
  denied=facts;denied.privacy_admitted=false;
  read=dt::AdmitTimestampStatisticsReadV3(profile,corrupt,denied);
  Check(read.disposition==RA::privacy_denied&&!read.decode_attempted,
        "statistics privacy precedes format");
  dt::TimestampExecutionControlV3 statistics_budget;
  statistics_budget.maximum_allocation_bytes=16;
  auto statistics_resource=dt::DecodeTimestampStatisticsProjectionV3(
      profile,encoded.bytes,statistics_budget);
  Check(!statistics_resource.ok()&&statistics_resource.diagnostic.diagnostic_code==
            "RESOURCE.BUDGET_EXCEEDED"&&
            statistics_resource.diagnostic.detail=="statistics_allocation",
        "statistics canonical validation precedes full-record resource gate");
  auto integrity_corrupt=encoded.bytes;integrity_corrupt[440]^=1;
  read=dt::AdmitTimestampStatisticsReadV3(profile,integrity_corrupt,facts);
  Check(read.disposition==RA::format_refused&&
            read.diagnostic.diagnostic_code=="SBLR.ENVELOPE.CHECKSUM_INVALID"&&
            !read.decode_attempted&&read.payload_reads==0,
        "statistics receiving integrity refusal occurs before payload decode");
  auto raw_integrity=dt::DecodeTimestampStatisticsProjectionV3(profile,integrity_corrupt);
  Check(!raw_integrity.ok()&&raw_integrity.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "statistics raw decoder preserves canonical integrity diagnostic");
  read=dt::AdmitTimestampStatisticsReadV3(profile,encoded.bytes,facts);
  Check(read.ok()&&read.decode_attempted&&read.payload_reads==1&&
            read.statistics.statistics_uuid==projection.statistics_uuid,
        "statistics authorized read admission");
  auto other_source=facts;other_source.source_object_uuid=V7(10);
  read=dt::AdmitTimestampStatisticsReadV3(profile,encoded.bytes,other_source);
  Check(read.disposition==RA::provider_evidence_mismatch&&!read.decode_attempted,
        "statistics record cannot replay across source objects");
  auto invalid_profile=std::make_shared<dt::TimestampValidatedProfileHandleV3>(*profile);
  invalid_profile->profile_fingerprint[0]^=1;
  auto precedence=dt::DecodeTimestampStatisticsProjectionV3(invalid_profile,corrupt);
  Check(!precedence.ok()&&precedence.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
        "statistics structure and integrity precede caller profile");
  precedence=dt::DecodeTimestampStatisticsProjectionV3(invalid_profile,encoded.bytes);
  Check(!precedence.ok()&&precedence.diagnostic.diagnostic_code==
            "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "statistics valid format then validates caller profile");
  read=dt::AdmitTimestampStatisticsReadV3({},encoded.bytes,facts);
  Check(read.disposition==RA::profile_refused&&!read.decode_attempted&&
            read.diagnostic.diagnostic_code=="SBLR.OPERAND_INVALID",
        "statistics missing profile refuses before decode");
  read=dt::AdmitTimestampStatisticsReadV3(invalid_profile,encoded.bytes,facts);
  Check(read.disposition==RA::profile_refused&&!read.decode_attempted&&
            read.diagnostic.diagnostic_code=="CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "statistics invalid profile refuses before decode");
  for(unsigned field=0;field<4;++field){auto stale=facts;
    if(field==0)++stale.schema_epoch;else if(field==1)++stale.collection_epoch;
    else if(field==2)++stale.security_epoch;else stale.provider_evidence_uuid=V7(8);
    auto stale_read=dt::AdmitTimestampStatisticsReadV3(profile,encoded.bytes,stale);
    Check(!stale_read.ok()&&!stale_read.decode_attempted,
          "statistics read stale receiving fact refuses");}

  auto empty=projection;empty.row_count=empty.null_count=1;empty.value_count=0;
  empty.minimum_maximum_present=false;empty.minimum_civil_day=empty.maximum_civil_day=0;
  empty.minimum_nanoseconds_since_midnight=empty.maximum_nanoseconds_since_midnight=0;
  empty.histogram={};empty.mcv={};SyncEvidence(empty,evidence);
  Check(dt::EncodeTimestampStatisticsProjectionV3(empty).bytes.size()==544,
        "statistics exact empty extent");
  std::array<dt::TimestampStatisticsHistogramRecordV3,64> max_hist{};
  std::array<dt::TimestampStatisticsMcvRecordV3,64> max_mcv{};
  for(unsigned i=0;i<64;++i){max_hist[i]={0,i,i+1};max_mcv[i].value_hash[31]=static_cast<p::byte>(i);max_mcv[i].frequency=1;}
  auto maximum=projection;maximum.row_count=64;maximum.null_count=0;maximum.value_count=64;
  maximum.minimum_civil_day=maximum.maximum_civil_day=0;
  maximum.minimum_nanoseconds_since_midnight=0;maximum.maximum_nanoseconds_since_midnight=63;
  maximum.histogram=max_hist;maximum.mcv=max_mcv;SyncEvidence(maximum,evidence);
  Check(dt::EncodeTimestampStatisticsProjectionV3(maximum).bytes.size()==5152,
        "statistics exact maximum record extent");
  auto duplicate_hash=max_mcv;duplicate_hash[1].value_hash=duplicate_hash[0].value_hash;
  auto collision=maximum;collision.mcv=duplicate_hash;SyncEvidence(collision,evidence);
  Check(!dt::EncodeTimestampStatisticsProjectionV3(collision).ok(),
        "statistics duplicate hash collision omitted");
  auto raw_mutation=maximum;raw_mutation.minimum_nanoseconds_since_midnight=1;
  SyncEvidence(raw_mutation,evidence);
  Check(!dt::EncodeTimestampStatisticsProjectionV3(raw_mutation).ok(),
        "statistics histogram bounds enforce source population");

  const auto value=Value(profile,0,0);const auto nullv=Null(profile);
  const auto backup=dt::EncodeTimestampBackupTupleV3(value);
  const auto backup_null=dt::EncodeTimestampBackupTupleV3(nullv);
  Check(backup.ok()&&backup.bytes.size()==312&&
            std::memcmp(backup.bytes.data(),"SBTSPB01",8)==0&&
            backup_null.ok()&&backup_null.bytes.size()==296,
        "backup exact vectors");
  Check(dt::DecodeTimestampBackupTupleNoAllocV3(*profile,true,backup.bytes).ok()&&
            !dt::DecodeTimestampBackupTupleNoAllocV3(*profile,false,backup_null.bytes).ok(),
        "backup decode and NULL policy");
  dt::TimestampExecutionControlV3 backup_budget;
  backup_budget.maximum_allocation_bytes=311;
  auto backup_resource=dt::DecodeTimestampBackupTupleNoAllocV3(
      *profile,true,backup.bytes,backup_budget);
  Check(!backup_resource.ok()&&backup_resource.diagnostic.diagnostic_code==
            "RESOURCE.BUDGET_EXCEEDED"&&
            backup_resource.diagnostic.detail=="backup_extent_budget",
        "backup decoder enforces full 312-byte resource grant");
  for(std::size_t i=0;i<backup.bytes.size();++i){auto b=backup.bytes;b[i]^=1;
    Check(!dt::DecodeTimestampBackupTupleNoAllocV3(*profile,true,b).ok(),
          "backup byte mutation refuses");}
}

void TestProtectionAndWire(
    const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  const std::array<std::string_view,10> dispositions{{
      "admitted_datatype_component","forbidden","forbidden","unsupported_current_core",
      "unsupported_current_core","unsupported_current_core","unsupported_current_core",
      "not_applicable_datatype_layer","not_applicable_datatype_layer","forbidden_current_core"}};
  const std::array<std::string_view,10> diagnostics{{
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID","CTI.TEMPORAL.PROTECTION_UNSUPPORTED",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED",
      "receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim",
      "receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim",
      "receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim",
      "receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim",
      "owner_diagnostic_before_datatype","owner_diagnostic_before_datatype",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED"}};
  for(unsigned i=0;i<10;++i){const auto row=dt::ResolveTimestampProtectionV3(
      static_cast<dt::TimestampProtectionCellV3>(i));
    Check(row.cell==static_cast<dt::TimestampProtectionCellV3>(i)&&
              row.disposition==dispositions[i]&&row.diagnostic==diagnostics[i]&&
              row.datatype_admitted==(i==0)&&
              row.receiving_owner_handoff==(i>=3&&i<=8),"exact protection cell");}
  for(unsigned storage=0;storage<4;++storage)for(unsigned stream=0;stream<4;++stream)
    for(unsigned index=0;index<2;++index){const auto row=dt::AdmitTimestampProtectionCombinationV3(
        *profile,static_cast<dt::TimestampProtectionModeV3>(storage),
        static_cast<dt::TimestampProtectionModeV3>(stream),
        static_cast<dt::TimestampProtectionIndexModeV3>(index));
      const bool admitted=storage==0&&stream==0;
      Check(row.admitted==admitted&&
                (admitted?row.diagnostic.empty():row.diagnostic=="CTI.TEMPORAL.PROTECTION_UNSUPPORTED"),
            "closed 32 protection combinations");}
  Check(!dt::ResolveTimestampProtectionCombinationV3(
            static_cast<dt::TimestampProtectionModeV3>(4),dt::TimestampProtectionModeV3::clear,
            dt::TimestampProtectionIndexModeV3::not_indexed).admitted,
        "unknown protection mode refuses");
  const auto unknown_cell=dt::ResolveTimestampProtectionV3(
      static_cast<dt::TimestampProtectionCellV3>(255));
  Check(!unknown_cell.datatype_admitted&&!unknown_cell.receiving_owner_handoff&&
            unknown_cell.disposition=="unknown_cell_fail_closed"&&
            unknown_cell.diagnostic=="CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "unknown protection cell refuses without fabricated owner handoff");
  auto mutated_profile=*profile;mutated_profile.profile_fingerprint[0]^=1;
  const auto profile_refusal=dt::AdmitTimestampProtectionCombinationV3(
      mutated_profile,dt::TimestampProtectionModeV3::clear,
      dt::TimestampProtectionModeV3::clear,
      dt::TimestampProtectionIndexModeV3::not_indexed);
  Check(!profile_refusal.admitted&&profile_refusal.diagnostic=="CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "protection admission validates complete profile first");
  for(unsigned i=0;i<27;++i){const auto row=dt::ResolveTimestampWireLaneV3(
      static_cast<dt::TimestampWireLaneV3>(i));
    Check(!row.admitted&&row.receiving_owner_handoff&&
              row.component_mapping_complete==(i<3)&&row.diagnostic=="CTI.TRANSPORT.UNSUPPORTED"&&
              row.disposition==(i<3?"unsupported_current_outer_version_unallocated":
                                     "unsupported_no_manifest_listed_compatibility_profile"),
          "all 27 wire lanes refuse with typed handoff");}
  Check(!dt::ResolveTimestampWireLaneV3(static_cast<dt::TimestampWireLaneV3>(27)).admitted,
        "unknown wire lane refuses");
}

std::vector<dt::TimestampDiagnosticRouteParameterV3> PayloadFor(
    std::string_view schema) {
  using S=dt::TimestampDiagnosticRouteScalarTypeV3;
  std::vector<dt::TimestampDiagnosticRouteParameterV3> out;
  while(!schema.empty()){
    const auto separator=schema.find(';');const auto token=schema.substr(0,separator);
    dt::TimestampDiagnosticRouteParameterV3 p;p.token=token;
    if(token.ends_with("_uuid")||token.ends_with("_uuid_or_zero")){p.scalar_type=S::uuid;p.uuid_value=V7(6);}
    else if(token.ends_with("_u8")){p.scalar_type=S::u8;
      p.unsigned_value=token=="maximum_fraction_digits_u8"?9:3;}
    else if(token.ends_with("_u32")){p.scalar_type=S::u32;p.unsigned_value=29;}
    else if(token.ends_with("_u64")){p.scalar_type=S::u64;p.unsigned_value=29;}
    else {p.scalar_type=S::closed_enum;
      if(token=="bound_enum")p.enum_value="minimum";
      else if(token=="cast_context_enum")p.enum_value="explicit";
      else if(token=="checkpoint_enum")p.enum_value="before_publication";
      else if(token=="epoch_class_enum")p.enum_value="schema_epoch";
      else if(token=="lane_group_enum")p.enum_value="native_sbwp";
      else if(token=="layer_enum")p.enum_value="canonical";
      else if(token=="index_family_enum")p.enum_value="btree";
      else if(token=="boundary_enum")p.enum_value="component";
      else p.enum_value="absent";
    }
    out.push_back(p);if(separator==std::string_view::npos)break;schema.remove_prefix(separator+1);
  }
  return out;
}

void TestDiagnosticsAndMetrics() {
  using ED=dt::TimestampDiagnosticRouteExecutionDispositionV3;
  using TD=dt::TimestampDiagnosticRouteFactDispositionV3;
  const auto routes=dt::TimestampDiagnosticRoutesV3();
  const auto metric_routes=dt::TimestampDiagnosticMetricRoutesV3();
  Check(routes.size()==44&&metric_routes.size()==44,"exact diagnostic route counts");
  std::set<std::array<p::byte,17>> keys;
  std::set<unsigned> axes;
  for(std::size_t i=0;i<routes.size();++i){const auto& route=routes[i];
    axes.insert(static_cast<unsigned>(route.axis));
    Check(static_cast<unsigned>(route.axis)<15&&!route.code.empty()&&
              !route.diagnostic_uuid.is_nil()&&!route.ordered_parameter_schema.empty()&&
              (route.redaction=="no_component_rendered_timestamp_key_hash_preimage_secret_or_address"||
               route.redaction=="no_payload_no_rendered_timestamp_no_key_material_no_secret_no_memory_address"),
          "exact diagnostic route fields");
    std::array<p::byte,17> key{};key[0]=static_cast<p::byte>(route.axis);
    std::copy(route.diagnostic_uuid.bytes.begin(),route.diagnostic_uuid.bytes.end(),key.begin()+1);
    Check(keys.insert(key).second,"diagnostic route uniqueness");
    auto payload=PayloadFor(route.ordered_parameter_schema);
    dt::TimestampDiagnosticRouteExecutionRequestV3 request{
        route.axis,route.code,route.diagnostic_uuid,payload,route.redaction};
    const auto admitted=dt::ExecuteTimestampDiagnosticRouteV3(request);
    if(!(admitted.ok()&&admitted.diagnostic_fact_emitted&&
         admitted.receiving_owner_evidence_handoff_ready&&
         !admitted.metric_mutation_admitted&&admitted.datatype_outcome_unchanged&&
         admitted.metric_outcome_unchanged&&admitted.parameter_count==payload.size()&&
         admitted.metric_evidence_type_uuid==metric_routes[i].metric_evidence_type_uuid&&
         admitted.metric_name==metric_routes[i].metric_name)) {
      std::cerr << "diagnostic route failure index=" << i << " code=" << route.code
                << " disposition=" << static_cast<unsigned>(admitted.disposition) << '\n';
    }
    Check(admitted.ok()&&admitted.diagnostic_fact_emitted&&
              admitted.receiving_owner_evidence_handoff_ready&&
              !admitted.metric_mutation_admitted&&admitted.datatype_outcome_unchanged&&
              admitted.metric_outcome_unchanged&&admitted.parameter_count==payload.size()&&
              admitted.metric_evidence_type_uuid==metric_routes[i].metric_evidence_type_uuid&&
              admitted.metric_name==metric_routes[i].metric_name,
          "all diagnostic routes execute exactly");
    for(std::size_t parameter_index=0;parameter_index<payload.size();++parameter_index){
      const auto& source=payload[parameter_index];const auto& published=admitted.ordered_payload[parameter_index];
      Check(published.token==source.token&&published.scalar_type==source.scalar_type,
            "diagnostic route publishes exact active parameter identity");
      if(source.scalar_type==dt::TimestampDiagnosticRouteScalarTypeV3::uuid)
        Check(published.uuid_value==source.uuid_value&&published.unsigned_value==0&&
                  published.enum_value.empty(),
              "diagnostic UUID publication clears inactive scalar members");
      else if(source.scalar_type==dt::TimestampDiagnosticRouteScalarTypeV3::closed_enum)
        Check(published.enum_value==source.enum_value&&published.uuid_value.is_nil()&&
                  published.unsigned_value==0,
              "diagnostic enum publication clears inactive scalar members");
      else
        Check(published.unsigned_value==source.unsigned_value&&published.uuid_value.is_nil()&&
                  published.enum_value.empty(),
              "diagnostic numeric publication clears inactive scalar members");
    }
    auto missing=payload;missing.pop_back();request.ordered_payload=missing;
    Check(dt::ExecuteTimestampDiagnosticRouteV3(request).disposition==ED::missing_token,
          "route missing token refuses");
    auto extra=payload;extra.push_back(payload.front());request.ordered_payload=extra;
    Check(dt::ExecuteTimestampDiagnosticRouteV3(request).disposition==ED::extra_token,
          "route extra token refuses");
    auto reordered=payload;std::swap(reordered[0],reordered[1]);request.ordered_payload=reordered;
    Check(dt::ExecuteTimestampDiagnosticRouteV3(request).disposition==ED::reordered_tokens,
          "route reordered token refuses");
    auto wrong=payload;
    wrong[0].scalar_type=wrong[0].scalar_type==dt::TimestampDiagnosticRouteScalarTypeV3::uuid
        ?dt::TimestampDiagnosticRouteScalarTypeV3::closed_enum
        :dt::TimestampDiagnosticRouteScalarTypeV3::uuid;
    request.ordered_payload=wrong;
    Check(dt::ExecuteTimestampDiagnosticRouteV3(request).disposition==ED::wrong_scalar_type,
          "route wrong scalar refuses");
    auto unknown_token=payload;unknown_token[0].token="unknown";request.ordered_payload=unknown_token;
    Check(dt::ExecuteTimestampDiagnosticRouteV3(request).disposition==ED::unknown_token,
          "route unknown token refuses");
    auto mismatched_token=payload;mismatched_token[0].token="unknown_uuid";
    request.ordered_payload=mismatched_token;
    Check(dt::ExecuteTimestampDiagnosticRouteV3(request).disposition==ED::mismatched_known_token,
          "route mismatched known-shape token refuses");
    auto invalid_scalar=payload;
    switch(invalid_scalar[0].scalar_type){
      case dt::TimestampDiagnosticRouteScalarTypeV3::uuid:
        invalid_scalar[0].uuid_value={};break;
      case dt::TimestampDiagnosticRouteScalarTypeV3::u8:
        invalid_scalar[0].unsigned_value=256;break;
      case dt::TimestampDiagnosticRouteScalarTypeV3::u32:
        invalid_scalar[0].unsigned_value=std::numeric_limits<p::u64>::max();break;
      case dt::TimestampDiagnosticRouteScalarTypeV3::closed_enum:
        invalid_scalar[0].enum_value="unknown";break;
      case dt::TimestampDiagnosticRouteScalarTypeV3::u64:
        invalid_scalar[0].token="unknown";break;
    }
    request.ordered_payload=invalid_scalar;
    const auto invalid_result=dt::ExecuteTimestampDiagnosticRouteV3(request);
    Check(invalid_result.disposition==ED::invalid_scalar_value||
              invalid_result.disposition==ED::unknown_token,
          "route invalid scalar refuses");
    auto hidden_sensitive=payload;
    const auto uuid_parameter=std::find_if(hidden_sensitive.begin(),hidden_sensitive.end(),
        [](const auto& parameter){return parameter.scalar_type==
            dt::TimestampDiagnosticRouteScalarTypeV3::uuid;});
    if(uuid_parameter!=hidden_sensitive.end()){
      uuid_parameter->enum_value="secret-bearing-inactive-member";
      request.ordered_payload=hidden_sensitive;
      const auto hidden_result=dt::ExecuteTimestampDiagnosticRouteV3(request);
      Check(hidden_result.disposition==ED::invalid_scalar_value&&
                !hidden_result.diagnostic_fact_emitted&&
                !hidden_result.receiving_owner_evidence_handoff_ready&&
                !hidden_result.metric_mutation_admitted&&
                hidden_result.datatype_outcome_unchanged&&
                hidden_result.metric_outcome_unchanged,
            "inactive sensitive scalar refuses without diagnostic or metric emission");
    }
    request.ordered_payload=payload;request.redaction="none";
    Check(dt::ExecuteTimestampDiagnosticRouteV3(request).disposition==ED::redaction_mismatch,
          "route redaction mutation refuses");
    request.redaction=route.redaction;request.diagnostic_uuid.bytes[0]^=1;
    Check(dt::ExecuteTimestampDiagnosticRouteV3(request).disposition==ED::code_uuid_mismatch,
          "route code UUID mismatch refuses");
    const auto resolved=dt::ResolveTimestampDiagnosticMetricRouteV3(
        {route.axis,route.diagnostic_uuid});
    Check(resolved.disposition==TD::admitted&&
              resolved.route.diagnostic_family==route.axis&&
              resolved.route.diagnostic_uuid==route.diagnostic_uuid&&
              resolved.route.rule=="after final primary diagnostic selection exactly once",
          "diagnostic to metric route exact resolution");
  }
  Check(axes.size()==15&&*axes.begin()==0&&*axes.rbegin()==14,
        "all 15 diagnostic axes have exact routes");
  {
    const auto& route=routes.front();auto payload=PayloadFor(route.ordered_parameter_schema);
    const dt::TimestampDiagnosticRouteExecutionRequestV3 unknown{
        static_cast<dt::TimestampDiagnosticAxisV3>(255),"UNKNOWN",V7(99),payload,route.redaction};
    Check(dt::ExecuteTimestampDiagnosticRouteV3(unknown).disposition==ED::unknown_route,
          "unknown diagnostic route refuses");
  }
  Check(dt::ValidateTimestampDiagnosticMetricRouteTableV3(metric_routes)==TD::admitted,
        "exact diagnostic metric table admits");
  Check(dt::ValidateTimestampDiagnosticMetricRouteTableV3(metric_routes,false)==TD::wrong_type,
        "diagnostic metric scalar mutation refuses");
  std::vector<dt::TimestampDiagnosticMetricRouteV3> table(metric_routes.begin(),metric_routes.end());
  table.pop_back();Check(dt::ValidateTimestampDiagnosticMetricRouteTableV3(table)==TD::missing,
                         "missing diagnostic metric row refuses");
  table.assign(metric_routes.begin(),metric_routes.end());std::swap(table[0],table[1]);
  Check(dt::ValidateTimestampDiagnosticMetricRouteTableV3(table)==TD::reordered,
        "reordered diagnostic metric row refuses");
  table.assign(metric_routes.begin(),metric_routes.end());table[1]=table[0];
  Check(dt::ValidateTimestampDiagnosticMetricRouteTableV3(table)==TD::duplicate,
        "duplicate diagnostic metric row refuses");
  table.assign(metric_routes.begin(),metric_routes.end());table[0].metric_name="wrong";
  Check(dt::ValidateTimestampDiagnosticMetricRouteTableV3(table)==TD::wrong_type,
        "wrong diagnostic metric row refuses");

  const auto metrics=dt::TimestampMetricEvidenceTypesV3();
  const std::array<std::string_view,9> names{{
      "sys.metrics.datatype.base_timestamp.operation_attempt_total",
      "sys.metrics.datatype.base_timestamp.operation_success_total",
      "sys.metrics.datatype.base_timestamp.operation_refusal_total",
      "sys.metrics.datatype.base_timestamp.cancellation_total",
      "sys.metrics.datatype.base_timestamp.serialization_refusal_total",
      "sys.metrics.datatype.base_timestamp.decode_corruption_total",
      "sys.metrics.datatype.base_timestamp.profile_refusal_total",
      "sys.metrics.datatype.base_timestamp.cast_refusal_total",
      "sys.metrics.datatype.base_timestamp.projection_refusal_total"}};
  const std::array<unsigned,9> tuple_counts{{25,25,25,25,12,14,25,7,6}};
  Check(metrics.size()==9,"nine metric evidence types");
  std::set<std::array<p::byte,16>> metric_ids;
  for(unsigned i=0;i<9;++i){const auto& metric=metrics[i];
    Check(static_cast<unsigned>(metric.type)==i&&metric.evidence_type_uuid==MetricUuid(i)&&
              metric.metric_uuid==MetricUuid(i)&&metric.generation==1&&
              metric.metric_name_metadata==names[i]&&metric.closed_label_schema=="obligation_axis"&&
              metric.manager_consumer=="metrics_registry_manager"&&
              metric_ids.insert(metric.evidence_type_uuid.bytes).second,
          "exact metric evidence descriptor");}
  const auto tuples=dt::TimestampMetricClosedLabelTuplesV3();
  Check(tuples.size()==164,"exact closed metric tuple count");
  std::array<unsigned,9> observed{};std::set<std::string> tuple_keys;
  const auto envelope_identity=dt::TimestampMetricEvidenceEnvelopeIdentityV3Value();
  p::u64 sequence=1;
  for(const auto& tuple:tuples){const auto index=static_cast<unsigned>(tuple.type);
    Check(index<9&&tuple.member_count==1&&tuple.members[0].name=="obligation_axis"&&
              !tuple.members[0].value.empty(),"closed tuple exact shape");
    ++observed[index];
    Check(tuple_keys.insert(std::to_string(index)+":"+std::string(tuple.members[0].value)).second,
          "closed metric tuple uniqueness");
    dt::TimestampMetricEvidenceEnvelopeV3 evidence;
    evidence.envelope_identity=envelope_identity;evidence.evidence_type_uuid=metrics[index].evidence_type_uuid;
    evidence.metric_uuid=metrics[index].metric_uuid;evidence.source_event_uuid=V7(20+index);
    evidence.database_uuid=V7(40);evidence.node_uuid=V7(41);evidence.process_epoch=7;
    evidence.event_sequence=sequence++;evidence.monotonic_timestamp_ns=sequence;
    evidence.counter_delta=1;evidence.closed_label_tuple={tuple.members.data(),tuple.member_count};
    evidence.outcome_committed=index!=0;
    const auto result=dt::ValidateTimestampMetricEvidenceV3(evidence,{7,0});
    Check(result.disposition==dt::TimestampMetricEvidenceDispositionV3::admitted&&
              result.receiving_owner_update_admitted&&result.datatype_outcome_unchanged&&
              result.type==tuple.type,"all 164 metric tuples admit");
  }
  Check(observed==tuple_counts,"exact per-metric tuple cardinalities");

  dt::TimestampMetricEvidenceEnvelopeV3 valid;
  valid.envelope_identity=envelope_identity;valid.evidence_type_uuid=metrics[1].evidence_type_uuid;
  valid.metric_uuid=metrics[1].metric_uuid;valid.source_event_uuid=V7(60);
  valid.database_uuid=V7(61);valid.node_uuid=V7(62);valid.process_epoch=7;
  valid.event_sequence=1;valid.monotonic_timestamp_ns=1;valid.counter_delta=1;
  valid.closed_label_tuple={tuples[25].members.data(),1};valid.outcome_committed=true;
  using MD=dt::TimestampMetricEvidenceDispositionV3;
  const auto refusal=[&](const dt::TimestampMetricEvidenceEnvelopeV3& candidate,
                         dt::TimestampMetricEvidenceValidationContextV3 context,MD expected){
    const auto result=dt::ValidateTimestampMetricEvidenceV3(candidate,context);
    Check(result.disposition==expected&&!result.receiving_owner_update_admitted&&
              result.datatype_outcome_unchanged,"metric evidence refusal leaves datatype unchanged");};
  auto bad=valid;bad.all_required_fields_present=false;refusal(bad,{7,0},MD::missing_field);
  bad=valid;bad.evidence_type_uuid.bytes[0]^=1;refusal(bad,{7,0},MD::unknown_evidence_type);
  std::array<dt::TimestampMetricLabelMemberV3,1> unknown{{{"obligation_axis","unknown"}}};
  bad=valid;bad.closed_label_tuple=unknown;refusal(bad,{7,0},MD::undeclared_label_tuple);
  bad=valid;bad.scalar_types_valid=false;refusal(bad,{7,0},MD::wrong_scalar);
  bad=valid;bad.outcome_committed=false;refusal(bad,{7,0},MD::outcome_not_committed);
  bad=valid;bad.counter_delta=2;refusal(bad,{7,0},MD::invalid_counter_delta);
  refusal(valid,{8,0},MD::stale_process_epoch);
  refusal(valid,{7,std::numeric_limits<p::u64>::max()},MD::counter_overflow);

  const auto operation_refusal=metrics[2].evidence_type_uuid;
  unsigned route_bound_pairs=0;
  for(const auto& route:metric_routes){
    unsigned specialized=metrics.size();
    for(unsigned i=4;i<9;++i)if(metrics[i].evidence_type_uuid==route.metric_evidence_type_uuid)specialized=i;
    if(specialized==metrics.size())continue;
    ++route_bound_pairs;
    const dt::TimestampMetricEvidenceIdempotencyFactV3 first{7,1,V7(70),operation_refusal};
    const dt::TimestampMetricEvidenceIdempotencyFactV3 second{7,2,first.source_event_uuid,
                                                              metrics[specialized].evidence_type_uuid};
    const dt::TimestampDiagnosticRouteKeyV3 key{route.diagnostic_family,route.diagnostic_uuid};
    const auto admitted=dt::ClassifyTimestampMetricSameSourceEmissionV3(key,first,second);
    Check(admitted.disposition==dt::TimestampMetricSameSourceDispositionV3::admitted_ordered_pair&&
              admitted.second_emission_admitted&&admitted.receiving_owner_update_admitted&&
              admitted.datatype_outcome_unchanged,
          "route-bound same-source pair admits");
    const auto reversed=dt::ClassifyTimestampMetricSameSourceEmissionV3(key,second,first);
    Check(reversed.disposition==dt::TimestampMetricSameSourceDispositionV3::reversed_order_refused&&
              !reversed.receiving_owner_update_admitted,
          "route-bound reversed pair refuses");
    auto unknown_key=key;unknown_key.diagnostic_uuid.bytes[0]^=1;
    Check(dt::ClassifyTimestampMetricSameSourceEmissionV3(
              unknown_key,second,first).disposition==
              dt::TimestampMetricSameSourceDispositionV3::unregistered_pair_refused,
          "reverse pair requires an exact registered diagnostic route");
    auto mismatched_key=key;
    for(const auto& candidate:metric_routes){
      if(candidate.metric_evidence_type_uuid!=second.evidence_type_uuid){
        mismatched_key={candidate.diagnostic_family,candidate.diagnostic_uuid};break;}}
    Check(dt::ClassifyTimestampMetricSameSourceEmissionV3(
              mismatched_key,second,first).disposition==
              dt::TimestampMetricSameSourceDispositionV3::unregistered_pair_refused,
          "reverse pair refuses a registered route for another metric type");
  }
  Check(route_bound_pairs==26,"exact 26 route-bound same-source pairs");
  for(const auto& metric:metrics){const dt::TimestampMetricEvidenceIdempotencyFactV3 first{
      7,1,V7(80),metric.evidence_type_uuid};const auto duplicate=first;
    const auto result=dt::ClassifyTimestampMetricSameSourceEmissionV3(
        {routes[0].axis,routes[0].diagnostic_uuid},first,duplicate);
    Check(result.disposition==dt::TimestampMetricSameSourceDispositionV3::duplicate_type_refused,
          "each metric duplicate same-source emission refuses");}
  const dt::TimestampMetricEvidenceIdempotencyFactV3 first{7,1,V7(90),operation_refusal};
  dt::TimestampMetricEvidenceIdempotencyFactV3 second{7,2,first.source_event_uuid,metrics[4].evidence_type_uuid};
  auto unregistered=dt::ClassifyTimestampMetricSameSourceEmissionV3(
      {routes[0].axis,routes[0].diagnostic_uuid},first,second);
  Check(unregistered.disposition==dt::TimestampMetricSameSourceDispositionV3::unregistered_pair_refused,
        "unregistered route-bound pair refuses");
  second.process_epoch=8;
  Check(dt::ClassifyTimestampMetricSameSourceEmissionV3(
            {routes[0].axis,routes[0].diagnostic_uuid},first,second).disposition==
            dt::TimestampMetricSameSourceDispositionV3::stale_process_epoch_refused,
        "same-source stale process epoch refuses");
  second.process_epoch=7;second.source_event_uuid=V7(91);
  Check(dt::ClassifyTimestampMetricSameSourceEmissionV3(
            {routes[0].axis,routes[0].diagnostic_uuid},first,second).disposition==
            dt::TimestampMetricSameSourceDispositionV3::independent_sources,
        "independent metric sources remain independent");
}
}  // namespace

int main() {
  const auto profile=Profile();
  TestIndexFamiliesAndPredicates();
  TestProjectionLayouts(profile);
  TestStatisticsAndBackup(profile);
  TestProtectionAndWire(profile);
  TestDiagnosticsAndMetrics();
  std::cout << "PASS base.timestamp V3 projection/observability checks=" << checks << '\n';
}
