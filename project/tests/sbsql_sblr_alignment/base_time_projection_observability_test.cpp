// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_time_projection.hpp"
#include "hash_digest_parts.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::atomic<long long> g_new_budget{-1};
std::atomic<unsigned long long> g_new_calls{0};
}

void* operator new(std::size_t bytes) {
  g_new_calls.fetch_add(1,std::memory_order_relaxed);
  auto budget=g_new_budget.load(std::memory_order_relaxed);
  while(budget>=0){
    if(budget==0)throw std::bad_alloc();
    if(g_new_budget.compare_exchange_weak(
           budget,budget-1,std::memory_order_relaxed))break;
  }
  if(void* result=std::malloc(bytes==0?1:bytes))return result;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* pointer) noexcept{std::free(pointer);}
void operator delete[](void* pointer) noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept{std::free(pointer);}
void operator delete[](void* pointer,std::size_t) noexcept{std::free(pointer);}

namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;
namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
platform::Uuid Instance(unsigned n, bool v7 = true) {
  platform::Uuid u{};
  u.bytes[0] = 1; u.bytes[5] = static_cast<platform::byte>(n);
  u.bytes[6] = v7 ? 0x70 : 0x40; u.bytes[8] = 0x80;
  u.bytes[15] = static_cast<platform::byte>(n);
  return u;
}
std::shared_ptr<const dt::TimeValidatedProfileHandleV3> Profile() {
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (!dt::IsExactCanonicalTimeTypeCodecIdentityV3(row)) continue;
    auto result = dt::BuildCurrentTimeValidatedProfileHandleV3(
        row.legacy_fields.catalog_snapshot_uuid);
    Require(result.ok(), "time profile construction");
    return std::make_shared<const dt::TimeValidatedProfileHandleV3>(
        std::move(result.profile));
  }
  throw std::runtime_error("time identity missing");
}
dt::TimeOwnedValueV3 Value(
    const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>& profile,
    platform::u64 value) {
  return {profile, dt::TimeValueStateV3::value, value};
}
dt::TimeOwnedValueV3 Null(
    const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>& profile) {
  return {profile, dt::TimeValueStateV3::sql_null, 0};
}
bool AlwaysCancel(void*) noexcept { return true; }
struct CancelAfterContext {
  unsigned calls = 0;
  unsigned admitted_calls = 0;
};
bool CancelAfter(void* context) noexcept {
  auto& state = *static_cast<CancelAfterContext*>(context);
  return state.calls++ >= state.admitted_calls;
}
struct ScrubProbe {
  std::array<platform::u64,
      static_cast<std::size_t>(dt::TimeScrubClassV3::count)> calls{};
  std::array<platform::u64,
      static_cast<std::size_t>(dt::TimeScrubClassV3::count)> bytes{};
  bool all_zero = true;
};
void ObserveScrub(void* context,dt::TimeScrubClassV3 scrub_class,
                  const platform::byte* bytes,platform::u64 extent) noexcept {
  auto& probe=*static_cast<ScrubProbe*>(context);
  const auto index=static_cast<std::size_t>(scrub_class);
  if(index>=probe.calls.size()){probe.all_zero=false;return;}
  ++probe.calls[index];probe.bytes[index]+=extent;
  for(platform::u64 i=0;i<extent;++i)if(bytes[i]!=0){probe.all_zero=false;break;}
}
platform::u32 LoadLittle32(const platform::byte* p) noexcept {
  return platform::u32{p[0]} | (platform::u32{p[1]} << 8) |
         (platform::u32{p[2]} << 16) | (platform::u32{p[3]} << 24);
}
platform::u64 LoadLittle64(const platform::byte* p) noexcept {
  platform::u64 value = 0;
  for (unsigned i = 0; i != 8; ++i) value |= platform::u64{p[i]} << (8 * i);
  return value;
}
bool ZeroBytes(std::span<const platform::byte> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](platform::byte value) { return value == 0; });
}
void RehashRecord(std::string_view domain,std::vector<platform::byte>* record,
                  std::size_t digest_offset) {
  Require(record!=nullptr&&digest_offset+32<=record->size(),
          "test record rehash bounds");
  constexpr std::array<platform::byte,32> zero{};
  const scratchbird::core::hash::HashDigestSegment segments[]{{
      reinterpret_cast<const platform::byte*>(domain.data()),domain.size()},
      {record->data(),digest_offset},{zero.data(),zero.size()},
      {record->data()+digest_offset+zero.size(),
       record->size()-digest_offset-zero.size()}};
  const auto digest=scratchbird::core::hash::ComputeSha256DigestPartsNative(
      segments,std::size(segments));
  Require(digest.ok(),"test record rehash");
  std::memcpy(record->data()+digest_offset,digest.digest.data(),
              digest.digest.size());
}
std::string HexUuid(const platform::Uuid& uuid) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i != uuid.bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    text.push_back(digits[uuid.bytes[i] >> 4]);
    text.push_back(digits[uuid.bytes[i] & 0x0f]);
  }
  return text;
}
platform::Uuid TimeAllocatedUuid(platform::byte final_octet) {
  return platform::Uuid({0x01, 0xa1, 0x03, 0x2b, 0x9f, 0x51, 0x72, 0x29,
                         0x97, 0x7b, 0x45, 0x73, 0x0f, 0x3d, 0xff,
                         final_octet});
}

inline constexpr std::string_view kExactDiagnosticRouteIdentities = R"ROUTES(
0|SBLR.OPERAND_INVALID|f7c73687-c78d-546a-bc93-028b52b435cf
0|CTI.TEMPORAL.INVALID_LITERAL|01a1008e-c200-7002-8000-000000000002
0|CTI.TEMPORAL.LEAP_SECOND_REFUSED|01a1032b-9f51-7229-977b-45730f3dff59
0|CTI.TEMPORAL.PRECISION_LOSS|01a1032b-9f51-7229-977b-45730f3dff58
1|SECURITY.ACCESS_DENIED|1a8d26b0-8755-56ad-a170-b24a3aaf1407
1|CTI.TEMPORAL.DESCRIPTOR_INVALID|01a1008e-c200-7001-8000-000000000001
2|STORAGE.PAGE_CHECKSUM_FAILED|0c7d9aaf-92e6-5905-9daf-0974b871f94a
2|CTI.TEMPORAL.CANONICAL_ENCODING_INVALID|01a1008e-c200-7003-8000-000000000003
3|DATATYPE.CAST_FORBIDDEN|0dc51a1c-ad21-57b5-ae65-e0fe5589021a
3|CTI.TEMPORAL.INVALID_LITERAL|01a1008e-c200-7002-8000-000000000002
3|CTI.TEMPORAL.LEAP_SECOND_REFUSED|01a1032b-9f51-7229-977b-45730f3dff59
4|CTI.TEMPORAL.PRECISION_LOSS|01a1032b-9f51-7229-977b-45730f3dff58
4|CTB.TEXT.LENGTH_EXCEEDED|b399b514-f771-5014-a5b4-9077f5dee369
5|CTI.TEMPORAL.OPERATION_REFUSED|01a1008e-c200-7005-8000-000000000005
5|CTI.INTERVAL.CALENDAR_OPERATION_REFUSED|01a1008e-c200-700c-8000-00000000000c
5|CTI.TEMPORAL.ORDERING_REFUSED|01a1008e-c200-7006-8000-000000000006
6|CTI.TEMPORAL.RANGE_EXCEEDED|01a1008e-c200-7004-8000-000000000004
6|RESOURCE.BUDGET_EXCEEDED|dedc1c9b-3879-5770-8f8e-c0c927a0c34d
7|RESOURCE.BUDGET_EXCEEDED|dedc1c9b-3879-5770-8f8e-c0c927a0c34d
7|PROCESS.CANCELLED|0f425de2-6bc7-51d4-8d34-72d107ad45e7
8|SBLR.ENVELOPE.CHECKSUM_INVALID|260f5eac-b286-5a00-816c-c4c2a8f1a25e
8|CTI.TEMPORAL.PROTECTION_UNSUPPORTED|01a1008e-c200-7009-8000-000000000009
9|SECURITY.ACCESS_DENIED|1a8d26b0-8755-56ad-a170-b24a3aaf1407
9|CTI.TEMPORAL.PROTECTION_UNSUPPORTED|01a1008e-c200-7009-8000-000000000009
10|SBLR.OPERAND_INVALID|f7c73687-c78d-546a-bc93-028b52b435cf
10|SBLR.ENVELOPE.CHECKSUM_INVALID|260f5eac-b286-5a00-816c-c4c2a8f1a25e
10|CTI.TEMPORAL.CANONICAL_ENCODING_INVALID|01a1008e-c200-7003-8000-000000000003
10|CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING|01a1008e-c200-7008-8000-000000000008
10|CTI.TRANSPORT.UNSUPPORTED|01a1008e-c200-700d-8000-00000000000d
11|CTI.TEMPORAL.ORDERING_REFUSED|01a1008e-c200-7006-8000-000000000006
11|OPTIMIZER.INDEX_COMPATIBILITY_MISSING|01a10c00-2000-700d-8000-00000000000d
11|CTI.TEMPORAL.INDEX_KEY_REFUSED|01a1008e-c200-7007-8000-000000000007
11|RESOURCE.BUDGET_EXCEEDED|dedc1c9b-3879-5770-8f8e-c0c927a0c34d
12|DATATYPE.NULL_STATE.INVALID|588fafd0-6386-55cf-844e-f5b0dc22c700
12|DATATYPE.NULL_NOT_ADMITTED|aba0828d-77c9-50de-9842-7f2047100214
12|DOMAIN.CONSTRAINT_FAILED|62be5052-adbc-59aa-b84b-ae60a8c40c0f
13|STORAGE.PAGE_CHECKSUM_FAILED|0c7d9aaf-92e6-5905-9daf-0974b871f94a
13|CTI.TEMPORAL.CANONICAL_ENCODING_INVALID|01a1008e-c200-7003-8000-000000000003
13|CTI.MERGE.MANUAL_REVIEW_REQUIRED|01a1008e-c200-700e-8000-00000000000e
14|SECURITY.ACCESS_DENIED|1a8d26b0-8755-56ad-a170-b24a3aaf1407
14|OPTIMIZER.STATISTICS_PRIVACY_DENIED|01a10c00-2000-700c-8000-00000000000c
14|OPTIMIZER.STATISTICS_EPOCH_MISMATCH|01a10c00-2000-700b-8000-00000000000b
)ROUTES";

void TestIndexRegistryAndPredicates() {
  using D = dt::TimeIndexDispositionV3;
  using P = dt::TimeProjectionKindV3;
  const std::array<D, 16> dispositions{{
      D::not_applicable, D::admitted_with_recheck, D::admitted_with_recheck,
      D::admitted_exact_if_budget, D::admitted_with_recheck,
      D::admitted_payload_only, D::not_applicable, D::conditional,
      D::not_applicable, D::not_applicable, D::admitted_with_recheck,
      D::conditional, D::admitted_exact_if_budget, D::not_applicable,
      D::admitted_exact_selected_mode, D::not_applicable}};
  const std::array<P, 16> projections{{
      P::none, P::equality_hash, P::zone_map, P::sort_key, P::zone_map,
      P::covering_value, P::none, P::conditional_result, P::none, P::none,
      P::equality_hash, P::conditional_underlying, P::range_pair, P::none,
      P::none, P::none}};
  const std::array<bool, 16> exact_recheck{{
      false, true, true, false, true, false, false, true,
      false, false, true, true, false, false, false, false}};
  const std::array<bool, 16> receiving_owner{{
      true, false, false, false, false, false, true, true,
      true, true, false, true, false, true, false, true}};
  std::set<std::array<platform::byte, 16>> ids;
  for (unsigned i = 0; i != 16; ++i) {
    const auto family = static_cast<dt::TimeIndexFamilyV3>(i);
    const auto row = dt::ResolveTimeIndexFamilyV3(family);
    Require(row.family == family, "index family identity");
    Require(row.compatibility_generation == 1, "index generation");
    Require(row.compatibility_uuid == TimeAllocatedUuid(
                static_cast<platform::byte>(0x48 + i)),
            "exact index UUID allocation");
    Require(row.disposition == dispositions[i], "index disposition");
    Require(row.projection == projections[i], "index projection");
    Require(row.exact_recheck_required == exact_recheck[i] &&
                row.receiving_owner_resolution_required == receiving_owner[i],
            "index exact recheck and owner disposition");
    const auto identity = dt::LookupTimeIndexCompatibilityIdentityV3(family);
    Require(identity.family == family &&
                identity.compatibility_uuid == row.compatibility_uuid &&
                identity.compatibility_generation == 1 &&
                dt::AdmitTimeIndexCompatibilityV3(identity).admitted,
            "index exact UUID-first admission");
    auto uuid_mutation = identity;
    uuid_mutation.compatibility_uuid.bytes[0] ^= 1;
    Require(!dt::AdmitTimeIndexCompatibilityV3(uuid_mutation).admitted,
            "index UUID mutation refusal");
    auto generation_mutation = identity;
    generation_mutation.compatibility_generation = 2;
    Require(!dt::AdmitTimeIndexCompatibilityV3(generation_mutation).admitted,
            "index generation mutation refusal");
    auto cross_family = identity;
    cross_family.compatibility_uuid =
        dt::LookupTimeIndexCompatibilityIdentityV3(
            static_cast<dt::TimeIndexFamilyV3>((i + 1) % 16))
            .compatibility_uuid;
    Require(!dt::AdmitTimeIndexCompatibilityV3(cross_family).admitted,
            "index cross-family UUID refusal");
    ids.insert(row.compatibility_uuid.bytes);
  }
  Require(ids.size() == 16, "index UUID uniqueness");
  dt::TimeIndexCompatibilityIdentityV3 missing_identity;
  Require(!dt::AdmitTimeIndexCompatibilityV3(missing_identity).admitted,
          "missing index compatibility identity refusal");
  const dt::TimeIndexCompatibilityIdentityV3 unknown_identity{
      static_cast<dt::TimeIndexFamilyV3>(255), TimeAllocatedUuid(0x48), 1};
  Require(!dt::AdmitTimeIndexCompatibilityV3(unknown_identity).admitted,
          "unknown index family refusal");
  using F = dt::TimeIndexFamilyV3;
  using O = dt::TimeIndexPredicateOperationV3;
  using X = dt::TimeIndexPredicateFactV3;
  for (unsigned i = 0; i != 16; ++i) {
    const auto family = static_cast<F>(i);
    auto uuid_mutation =
        dt::LookupTimeIndexCompatibilityIdentityV3(family);
    uuid_mutation.compatibility_uuid.bytes[15] ^= 1;
    const auto uuid_refusal = dt::ResolveTimeIndexPredicateV3(
        {uuid_mutation, O::unsupported});
    Require(!uuid_refusal.admitted &&
                uuid_refusal.diagnostic ==
                    "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
            "predicate UUID mutation refuses before family behavior");
    auto generation_mutation =
        dt::LookupTimeIndexCompatibilityIdentityV3(family);
    ++generation_mutation.compatibility_generation;
    Require(!dt::ResolveTimeIndexPredicateV3(
                 {generation_mutation, O::unsupported}).admitted,
            "predicate generation mutation refusal");
    auto cross_family = dt::LookupTimeIndexCompatibilityIdentityV3(family);
    cross_family.compatibility_uuid =
        dt::LookupTimeIndexCompatibilityIdentityV3(
            static_cast<F>((i + 1) % 16)).compatibility_uuid;
    Require(!dt::ResolveTimeIndexPredicateV3(
                 {cross_family, O::unsupported}).admitted,
            "predicate cross-family identity refusal");
  }
  auto expected_fact = [](F family, O operation) {
    if ((family == F::bitmap &&
         (operation == O::equality || operation == O::in ||
          operation == O::is_null)) ||
        (family == F::hash &&
         (operation == O::equality || operation == O::in)) ||
        (family == F::temporary_work && operation == O::equality))
      return X::requires_exact_state_component_recheck_never_final_match;
    if ((family == F::brin_like &&
         (operation == O::equality || operation == O::range ||
          operation == O::order)) ||
        (family == F::columnar_zone_map &&
         (operation == O::prune_range || operation == O::equality)))
      return X::prune_only_mandatory_source_row_predicate_recheck_never_final_match;
    if ((family == F::btree &&
         (operation == O::equality || operation == O::range ||
          operation == O::order)) ||
        (family == F::covering_included &&
         operation == O::include_payload) ||
        (family == F::range_exclusion &&
         (operation == O::range || operation == O::overlap ||
          operation == O::exclusion)) ||
        (family == F::temporary_work && operation == O::order))
      return X::exact_no_recheck_after_decode_reencode;
    return X::refused;
  };
  for (unsigned family_index = 0; family_index != 16; ++family_index) {
    const auto family = static_cast<F>(family_index);
    if (family == F::expression || family == F::partial_filtered) continue;
    for (unsigned operation_index = 0; operation_index != 10;
         ++operation_index) {
      const auto operation = static_cast<O>(operation_index);
      const auto result = dt::ResolveTimeIndexPredicateV3(
          {dt::LookupTimeIndexCompatibilityIdentityV3(family), operation});
      const auto expected = expected_fact(family, operation);
      Require(result.fact == expected && result.admitted ==
                                            (expected != X::refused),
              "complete direct predicate matrix");
      if (expected == X::refused) {
        const auto disposition = dt::ResolveTimeIndexFamilyV3(family).disposition;
        Require(result.diagnostic ==
                    (disposition == dt::TimeIndexDispositionV3::not_applicable
                         ? "OPTIMIZER.INDEX_COMPATIBILITY_MISSING"
                         : "CTI.TEMPORAL.INDEX_KEY_REFUSED"),
                "exact undeclared predicate refusal route");
      }
    }
  }
  const auto selected_btree =
      dt::LookupTimeIndexCompatibilityIdentityV3(F::btree);
  for (const auto conditional : {F::expression, F::partial_filtered}) {
    const auto requested = dt::LookupTimeIndexCompatibilityIdentityV3(conditional);
    for (unsigned operation_index = 0; operation_index != 10;
         ++operation_index) {
      const auto operation = static_cast<O>(operation_index);
      const auto delegated = dt::ResolveTimeIndexPredicateV3(
          {requested, operation, &selected_btree});
      const auto expected = expected_fact(F::btree, operation);
      Require(delegated.admitted == (expected != X::refused) &&
                  delegated.fact == expected &&
                  delegated.requested_resolution.family == conditional &&
                  delegated.effective_resolution.family == F::btree &&
                  delegated.predicate_owner_handoff_preserved ==
                      (conditional == F::partial_filtered),
              "conditional complete delegated operation matrix");
      if (expected == X::refused)
        Require(delegated.diagnostic == "CTI.TEMPORAL.INDEX_KEY_REFUSED",
                "conditional undeclared-operation refusal route");
    }
    const auto success = dt::ResolveTimeIndexPredicateV3(
        {requested, O::range, &selected_btree});
    Require(success.admitted &&
                success.fact == X::exact_no_recheck_after_decode_reencode &&
                success.requested_resolution.family == conditional &&
                success.effective_resolution.family == F::btree &&
                success.predicate_owner_handoff_preserved ==
                    (conditional == F::partial_filtered),
            "conditional exact selected predicate fact");
    Require(!dt::ResolveTimeIndexPredicateV3({requested, O::range}).admitted,
            "conditional missing selection refusal");
    auto mutated = selected_btree;
    mutated.compatibility_uuid.bytes[0] ^= 1;
    Require(!dt::ResolveTimeIndexPredicateV3(
                 {requested, O::range, &mutated}).admitted,
            "conditional mutated selection refusal");
    auto generation_mutated = selected_btree;
    ++generation_mutated.compatibility_generation;
    Require(!dt::ResolveTimeIndexPredicateV3(
                 {requested, O::range, &generation_mutated}).admitted,
            "conditional selected-generation mutation refusal");
    const auto selected_conditional =
        dt::LookupTimeIndexCompatibilityIdentityV3(F::expression);
    Require(!dt::ResolveTimeIndexPredicateV3(
                 {requested, O::range, &selected_conditional}).admitted,
            "conditional-to-conditional selection refusal");
    const auto selected_not_applicable =
        dt::LookupTimeIndexCompatibilityIdentityV3(F::vector_ann);
    Require(!dt::ResolveTimeIndexPredicateV3(
                 {requested, O::range, &selected_not_applicable}).admitted,
            "conditional not-applicable selection refusal");
  }

  for (const auto family : {F::bitmap, F::hash, F::temporary_work}) {
    const auto predicate = dt::ResolveTimeIndexPredicateV3(
        {dt::LookupTimeIndexCompatibilityIdentityV3(family), O::equality});
    const auto collision = dt::ClassifyTimeIndexCandidateV3(
        predicate.fact, true, false, false);
    Require(!collision.final_match &&
                collision.exact_state_component_recheck_required &&
                collision.disposition ==
                    dt::TimeIndexCandidateDispositionV3::rejected_after_required_recheck,
            "forced equal-digest different-value collision refusal");
  }
  const auto zone_predicate = dt::ResolveTimeIndexPredicateV3(
      {dt::LookupTimeIndexCompatibilityIdentityV3(F::columnar_zone_map),
       O::prune_range});
  const std::array<platform::u64, 2> source_population{{0, 10}};
  const platform::u64 query_value = 5;
  const bool projection_candidate =
      query_value >= source_population.front() &&
      query_value <= source_population.back();
  const bool source_row_match =
      std::ranges::find(source_population, query_value) !=
      source_population.end();
  const auto zone_candidate = dt::ClassifyTimeIndexCandidateV3(
      zone_predicate.fact, projection_candidate, false, source_row_match);
  Require(projection_candidate && !source_row_match &&
              !zone_candidate.final_match &&
              zone_candidate.source_row_predicate_recheck_required,
          "zone candidate five within zero-ten requires source-row recheck");
}

void TestProjectionCodecs(
    const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>& profile) {
  const auto null_value = Null(profile);
  const auto minimum = Value(profile, 0);
  const auto maximum = Value(profile, dt::kTimeMaximumNanosecondsV3);
  auto dirty_value = maximum;
  dirty_value.state = static_cast<dt::TimeValueStateV3>(255);
  auto mutated_compatibility =
      dt::LookupTimeIndexCompatibilityIdentityV3(
          dt::TimeIndexFamilyV3::btree);
  mutated_compatibility.compatibility_uuid.bytes[0] ^= 1;
  dt::TimeIndexProjectionRequestV3 identity_precedence;
  identity_precedence.compatibility = mutated_compatibility;
  identity_precedence.mode = dt::TimeProjectionModeV3::ascending_nulls_last;
  identity_precedence.value = &dirty_value;
  identity_precedence.provider_max_key_bytes = 0;
  identity_precedence.control = {0, AlwaysCancel, nullptr};
  const auto identity_refusal =
      dt::ProjectTimeIndexValueV3(identity_precedence);
  Require(!identity_refusal.ok() && identity_refusal.bytes.empty() &&
              identity_refusal.diagnostic.diagnostic_code ==
                  "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
          "UUID-first admission precedes state payload budget and cancellation");
  auto missing_profile_value = maximum;
  missing_profile_value.profile.reset();
  auto profile_precedence = identity_precedence;
  profile_precedence.compatibility = mutated_compatibility;
  profile_precedence.value = &missing_profile_value;
  const auto profile_refusal =
      dt::ProjectTimeIndexValueV3(profile_precedence);
  Require(!profile_refusal.ok() && profile_refusal.bytes.empty() &&
              profile_refusal.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "profile admission precedes state payload budget and cancellation");
  auto cover_null = dt::EncodeTimeCoveringValueV3(null_value);
  auto cover_min = dt::EncodeTimeCoveringValueV3(minimum);
  auto cover_max = dt::EncodeTimeCoveringValueV3(maximum);
  Require(cover_null.ok() && cover_null.bytes.size() == 1 &&
              cover_null.bytes[0] == 0,
          "covering NULL fixture");
  Require(cover_min.ok() && cover_min.bytes.size() == 9 &&
              cover_min.bytes[0] == 2,
          "covering minimum fixture");
  Require(cover_max.ok() && cover_max.bytes.size() == 9 &&
              std::memcmp(cover_max.bytes.data() + 1,
                          "\xff\xff\x4e\x91\x94\x4e\x00\x00", 8) == 0,
          "covering maximum LE8 fixture");
  auto decoded_cover = dt::DecodeTimeCoveringValueNoAllocV3(
      *profile, true, cover_max.bytes);
  Require(decoded_cover.ok() &&
              decoded_cover.value.nanoseconds_since_midnight ==
                  dt::kTimeMaximumNanosecondsV3,
          "covering roundtrip");
  Require(!dt::DecodeTimeCoveringValueNoAllocV3(*profile, false,
                                                 cover_null.bytes).ok(),
          "covering NULL admission");

  auto pair = dt::EncodeTimeRangePairV3(minimum, maximum);
  Require(pair.ok() && pair.bytes.size() == 216, "range pair extent");
  auto pair_view = dt::DecodeTimeRangePairNoAllocV3(*profile, pair.bytes);
  Require(pair_view.ok() && pair_view.value.lower_nanoseconds == 0 &&
              pair_view.value.upper_nanoseconds ==
                  dt::kTimeMaximumNanosecondsV3 &&
              pair_view.value.lower_key.size() == 108 &&
              pair_view.value.upper_key.size() == 108,
          "range pair exact roundtrip");
  Require(!dt::EncodeTimeRangePairV3(maximum, minimum).ok(),
          "range pair inverted refusal");

  dt::TimeZoneMapRequestV3 empty_zone;
  empty_zone.profile = profile;
  empty_zone.null_count = 3;
  auto empty = dt::EncodeTimeZoneMapV3(empty_zone);
  Require(empty.ok() && empty.bytes.size() == 96 &&
              std::memcmp(empty.bytes.data(), "SBTIMZ01", 8) == 0,
          "empty zone fixture");
  auto empty_view = dt::DecodeTimeZoneMapNoAllocV3(*profile, empty.bytes);
  Require(empty_view.ok() && empty_view.value.null_count == 3 &&
              empty_view.value.value_count == 0,
          "empty zone roundtrip");
  dt::TimeZoneMapRequestV3 full_zone;
  full_zone.profile = profile;
  full_zone.minimum = &minimum;
  full_zone.maximum = &maximum;
  full_zone.null_count = 2;
  full_zone.value_count = 3;
  auto full = dt::EncodeTimeZoneMapV3(full_zone);
  Require(full.ok() && full.bytes.size() == 312,
          "populated zone fixture");
  auto full_view = dt::DecodeTimeZoneMapNoAllocV3(*profile, full.bytes);
  Require(full_view.ok() && full_view.value.minimum_nanoseconds == 0 &&
              full_view.value.maximum_nanoseconds ==
                  dt::kTimeMaximumNanosecondsV3,
          "populated zone roundtrip");
  for (std::size_t i = 0; i != full.bytes.size(); ++i) {
    auto mutation = full.bytes;
    mutation[i] ^= 1;
    Require(!dt::DecodeTimeZoneMapNoAllocV3(*profile, mutation).ok(),
            "zone mutation matrix");
  }
  full_zone.provider_max_key_bytes = 311;
  auto provider_refusal = dt::EncodeTimeZoneMapV3(full_zone);
  Require(!provider_refusal.ok() &&
              provider_refusal.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.INDEX_KEY_REFUSED",
          "zone provider budget route");
  full_zone.provider_max_key_bytes = ~platform::u64{0};

  for (const auto mode : {dt::TimeProjectionModeV3::ascending_nulls_first,
                          dt::TimeProjectionModeV3::ascending_nulls_last,
                          dt::TimeProjectionModeV3::descending_nulls_first,
                          dt::TimeProjectionModeV3::descending_nulls_last}) {
    dt::TimeIndexProjectionRequestV3 request;
    request.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(dt::TimeIndexFamilyV3::btree);
    request.mode = mode; request.value = &maximum;
    auto projected = dt::ProjectTimeIndexValueV3(request);
    Require(projected.ok() && projected.bytes.size() == 108,
            "btree projection mode");
    auto key = dt::DecodeTimeSortKeyNoAllocV3(*profile, projected.bytes);
    Require(key.ok() && key.value.nanoseconds_since_midnight ==
                            dt::kTimeMaximumNanosecondsV3,
            "btree decode roundtrip");
    request.provider_max_key_bytes = 107;
    Require(!dt::ProjectTimeIndexValueV3(request).ok(),
            "btree exact provider budget refusal");
  }
  dt::TimeIndexProjectionRequestV3 hash_request;
  hash_request.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(dt::TimeIndexFamilyV3::hash);
  hash_request.mode = dt::TimeProjectionModeV3::equality_hash;
  hash_request.value = &maximum;
  auto projected_hash = dt::ProjectTimeIndexValueV3(hash_request);
  auto native_hash = dt::HashTimeValueV3(maximum);
  Require(projected_hash.ok() && native_hash.ok() &&
              projected_hash.bytes == native_hash.bytes,
          "SBTIMH01 projection identity");
  hash_request.provider_max_key_bytes = 31;
  Require(!dt::ProjectTimeIndexValueV3(hash_request).ok(),
          "index provider budget refusal");
  hash_request.provider_max_key_bytes = 32;
  hash_request.control = {32, AlwaysCancel, nullptr};
  Require(!dt::ProjectTimeIndexValueV3(hash_request).ok(),
          "index cancellation refusal");

  // Every direct, conditional, and not-applicable family follows its declared
  // projection or handoff path; this surface never chooses an optimizer plan.
  for (const auto family : {dt::TimeIndexFamilyV3::bitmap,
                            dt::TimeIndexFamilyV3::hash}) {
    dt::TimeIndexProjectionRequestV3 request;
    request.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(family);
    request.mode = dt::TimeProjectionModeV3::equality_hash;
    request.value = &minimum;
    Require(dt::ProjectTimeIndexValueV3(request).ok(),
            "hash-family projection");
    request.provider_max_key_bytes = 31;
    Require(!dt::ProjectTimeIndexValueV3(request).ok(),
            "hash-family exact provider budget refusal");
  }
  for (const auto family : {dt::TimeIndexFamilyV3::brin_like,
                            dt::TimeIndexFamilyV3::columnar_zone_map}) {
    dt::TimeIndexProjectionRequestV3 request;
    request.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(family); request.mode = dt::TimeProjectionModeV3::zone_map;
    request.value = &minimum; request.zone_map = &full_zone;
    Require(dt::ProjectTimeIndexValueV3(request).ok(),
            "zone-family projection");
    request.provider_max_key_bytes = 311;
    Require(!dt::ProjectTimeIndexValueV3(request).ok(),
            "zone-family exact provider budget refusal");
  }
  dt::TimeIndexProjectionRequestV3 covering;
  covering.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(dt::TimeIndexFamilyV3::covering_included);
  covering.mode = dt::TimeProjectionModeV3::payload;
  covering.value = &maximum;
  Require(dt::ProjectTimeIndexValueV3(covering).ok(),
          "covering-family projection");
  covering.provider_max_key_bytes = 8;
  Require(!dt::ProjectTimeIndexValueV3(covering).ok(),
          "covering-family exact provider budget refusal");
  dt::TimeIndexProjectionRequestV3 range;
  range.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(dt::TimeIndexFamilyV3::range_exclusion);
  range.mode = dt::TimeProjectionModeV3::ascending_nulls_last_pair;
  range.value = &minimum; range.upper_value = &maximum;
  Require(dt::ProjectTimeIndexValueV3(range).ok(),
          "range-family projection");
  range.provider_max_key_bytes = 215;
  Require(!dt::ProjectTimeIndexValueV3(range).ok(),
          "range-family exact provider budget refusal");
  for (const auto mode : {dt::TimeProjectionModeV3::equality_hash,
                          dt::TimeProjectionModeV3::ascending_nulls_first,
                          dt::TimeProjectionModeV3::ascending_nulls_last,
                          dt::TimeProjectionModeV3::descending_nulls_first,
                          dt::TimeProjectionModeV3::descending_nulls_last}) {
    dt::TimeIndexProjectionRequestV3 request;
    request.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(dt::TimeIndexFamilyV3::temporary_work);
    request.mode = mode; request.value = &maximum;
    Require(dt::ProjectTimeIndexValueV3(request).ok(),
            "temporary selected-mode projection");
    request.provider_max_key_bytes =
        mode == dt::TimeProjectionModeV3::equality_hash ? 31 : 107;
    Require(!dt::ProjectTimeIndexValueV3(request).ok(),
            "temporary exact provider budget refusal");
  }
  const auto selected_btree =
      dt::ResolveTimeIndexFamilyV3(dt::TimeIndexFamilyV3::btree);
  for (const auto family : {dt::TimeIndexFamilyV3::expression,
                            dt::TimeIndexFamilyV3::partial_filtered}) {
    dt::TimeIndexProjectionRequestV3 request;
    request.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(family);
    request.mode = dt::TimeProjectionModeV3::ascending_nulls_last;
    request.value = &maximum;
    request.selected_conditional_family = &selected_btree;
    const auto selected_projection = dt::ProjectTimeIndexValueV3(request);
    Require(selected_projection.ok() &&
                selected_projection.requested_resolution.family == family &&
                selected_projection.resolution.family ==
                    dt::TimeIndexFamilyV3::btree,
            "conditional selected-family projection preserves identities");
    request.provider_max_key_bytes = 107;
    Require(!dt::ProjectTimeIndexValueV3(request).ok(),
            "conditional selected-family provider budget refusal");
    request.provider_max_key_bytes = ~platform::u64{0};
    request.selected_conditional_family = nullptr;
    const auto handoff = dt::ProjectTimeIndexValueV3(request);
    Require(!handoff.ok() && handoff.owner_fact_only,
            "conditional receiving-owner handoff");
  }
  for (const auto family : {dt::TimeIndexFamilyV3::aggregate_sketch,
                            dt::TimeIndexFamilyV3::document_path,
                            dt::TimeIndexFamilyV3::full_text,
                            dt::TimeIndexFamilyV3::graph,
                            dt::TimeIndexFamilyV3::spatial,
                            dt::TimeIndexFamilyV3::vector_ann}) {
    dt::TimeIndexProjectionRequestV3 request;
    request.compatibility = dt::LookupTimeIndexCompatibilityIdentityV3(family); request.value = &maximum;
    const auto refusal = dt::ProjectTimeIndexValueV3(request);
    Require(!refusal.ok() && refusal.owner_fact_only &&
                refusal.diagnostic.diagnostic_code ==
                    "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
            "not-applicable family handoff");
  }
}

void TestStatisticsAndBackup(
    const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>& profile) {
  auto evidence = std::make_shared<dt::TimeStatisticsProviderEvidenceHandleV3>();
  evidence->provider_evidence_uuid = Instance(31);
  std::array<dt::TimeStatisticsHistogramRecordV3, 2> histogram{{
      {0, 1}, {dt::kTimeMaximumNanosecondsV3, 2}}};
  std::array<dt::TimeStatisticsMcvRecordV3, 2> mcv{};
  mcv[0].value_hash[31] = 1; mcv[0].frequency = 1;
  mcv[1].value_hash[31] = 2; mcv[1].frequency = 1;
  dt::TimeStatisticsProjectionV3 projection;
  projection.profile = profile; projection.provider_evidence = evidence;
  projection.statistics_uuid = Instance(32);
  projection.source_object_uuid = Instance(33, false);
  projection.schema_epoch = 1; projection.collection_epoch = 2;
  projection.security_epoch = 3; projection.row_count = 4;
  projection.null_count = 2; projection.value_count = 2;
  projection.minimum_maximum_present = true;
  projection.minimum_nanoseconds = 0;
  projection.maximum_nanoseconds = dt::kTimeMaximumNanosecondsV3;
  projection.histogram = histogram; projection.mcv = mcv;
  auto encoded = dt::EncodeTimeStatisticsProjectionV3(projection);
  Require(encoded.ok() && encoded.bytes.size() == 656 &&
              std::memcmp(encoded.bytes.data(), "SBTIMS01", 8) == 0,
          "SBTIMS01 fixture");
  Require(LoadLittle32(encoded.bytes.data() + 16) == 0x27 &&
              LoadLittle32(encoded.bytes.data() + 20) == 2 &&
              LoadLittle32(encoded.bytes.data() + 24) == 2 &&
              ZeroBytes(std::span(encoded.bytes).subspan(264, 16)) &&
              LoadLittle64(encoded.bytes.data() + 312) == 1 &&
              LoadLittle64(encoded.bytes.data() + 320) == 2 &&
              LoadLittle64(encoded.bytes.data() + 328) == 3 &&
              LoadLittle64(encoded.bytes.data() + 336) == 4 &&
              LoadLittle64(encoded.bytes.data() + 344) == 2 &&
              LoadLittle64(encoded.bytes.data() + 352) == 2 &&
              LoadLittle64(encoded.bytes.data() + 384) == 0 &&
              LoadLittle64(encoded.bytes.data() + 392) ==
                  dt::kTimeMaximumNanosecondsV3 &&
              LoadLittle64(encoded.bytes.data() + 400) ==
                  0x8000000000000000ULL &&
              ZeroBytes(std::span(encoded.bytes).subspan(512, 16)) &&
              LoadLittle32(encoded.bytes.data() + 600) == 1 &&
              LoadLittle32(encoded.bytes.data() + 604) == 1 &&
              LoadLittle32(encoded.bytes.data() + 648) == 2 &&
              LoadLittle32(encoded.bytes.data() + 652) == 1,
          "SBTIMS01 exact header offsets and reserved spans");
  auto decoded = dt::DecodeTimeStatisticsProjectionV3(profile, encoded.bytes);
  Require(decoded.ok() &&
              decoded.statistics.statistics_uuid == projection.statistics_uuid &&
              decoded.statistics.source_object_uuid ==
                  projection.source_object_uuid &&
              decoded.statistics.provider_evidence_uuid ==
                  evidence->provider_evidence_uuid &&
              decoded.statistics.schema_epoch == projection.schema_epoch &&
              decoded.statistics.collection_epoch ==
                  projection.collection_epoch &&
              decoded.statistics.security_epoch == projection.security_epoch &&
              decoded.statistics.row_count == projection.row_count &&
              decoded.statistics.null_count == projection.null_count &&
              decoded.statistics.value_count == projection.value_count &&
              decoded.statistics.minimum_maximum_present ==
                  projection.minimum_maximum_present &&
              decoded.statistics.minimum_nanoseconds ==
                  projection.minimum_nanoseconds &&
              decoded.statistics.maximum_nanoseconds ==
                  projection.maximum_nanoseconds &&
              decoded.statistics.histogram.size() == 2 &&
              decoded.statistics.histogram.back().cumulative_count == 2 &&
              decoded.statistics.mcv.size() == 2 &&
              decoded.statistics.mcv[0].value_hash == mcv[0].value_hash &&
              decoded.statistics.mcv[0].frequency == mcv[0].frequency &&
              decoded.statistics.mcv[1].value_hash == mcv[1].value_hash &&
              decoded.statistics.mcv[1].frequency == mcv[1].frequency,
          "statistics exact encode/decode field equality");
  auto reencode_projection = projection;
  reencode_projection.statistics_uuid = decoded.statistics.statistics_uuid;
  reencode_projection.source_object_uuid = decoded.statistics.source_object_uuid;
  reencode_projection.schema_epoch = decoded.statistics.schema_epoch;
  reencode_projection.collection_epoch = decoded.statistics.collection_epoch;
  reencode_projection.security_epoch = decoded.statistics.security_epoch;
  reencode_projection.row_count = decoded.statistics.row_count;
  reencode_projection.null_count = decoded.statistics.null_count;
  reencode_projection.value_count = decoded.statistics.value_count;
  reencode_projection.minimum_maximum_present =
      decoded.statistics.minimum_maximum_present;
  reencode_projection.minimum_nanoseconds =
      decoded.statistics.minimum_nanoseconds;
  reencode_projection.maximum_nanoseconds =
      decoded.statistics.maximum_nanoseconds;
  reencode_projection.histogram = decoded.statistics.histogram;
  reencode_projection.mcv = decoded.statistics.mcv;
  const auto reencoded =
      dt::EncodeTimeStatisticsProjectionV3(reencode_projection);
  Require(reencoded.ok() && reencoded.bytes == encoded.bytes,
          "statistics decode/re-encode byte identity");
  dt::TimeStatisticsReceivingFactsV3 receiving{
      true, true, 1, 2, 3, evidence->provider_evidence_uuid};
  Require(dt::ResolveTimeStatisticsReceivingFactsV3(decoded.statistics,
                                                     receiving) ==
              dt::TimeStatisticsReceivingDispositionV3::admitted,
          "statistics receiving exact facts");
  ++receiving.security_epoch;
  Require(dt::ResolveTimeStatisticsReceivingFactsV3(decoded.statistics,
                                                     receiving) ==
              dt::TimeStatisticsReceivingDispositionV3::security_epoch_mismatch,
          "statistics epoch staleness");
  receiving = {false, true, 1, 2, 3, evidence->provider_evidence_uuid};
  Require(dt::ResolveTimeStatisticsReceivingFactsV3(decoded.statistics,
                                                     receiving) ==
              dt::TimeStatisticsReceivingDispositionV3::security_denied,
          "statistics security denial");
  receiving = {true, false, 1, 2, 3, evidence->provider_evidence_uuid};
  Require(dt::ResolveTimeStatisticsReceivingFactsV3(decoded.statistics,
                                                     receiving) ==
              dt::TimeStatisticsReceivingDispositionV3::privacy_denied,
          "statistics privacy denial");
  receiving = {true, true, 2, 2, 3, evidence->provider_evidence_uuid};
  Require(dt::ResolveTimeStatisticsReceivingFactsV3(decoded.statistics,
                                                     receiving) ==
              dt::TimeStatisticsReceivingDispositionV3::schema_epoch_mismatch,
          "statistics schema staleness");
  receiving = {true, true, 1, 3, 3, evidence->provider_evidence_uuid};
  Require(dt::ResolveTimeStatisticsReceivingFactsV3(decoded.statistics,
                                                     receiving) ==
              dt::TimeStatisticsReceivingDispositionV3::collection_epoch_mismatch,
          "statistics collection staleness");
  receiving = {true, true, 1, 2, 3, Instance(34)};
  Require(dt::ResolveTimeStatisticsReceivingFactsV3(decoded.statistics,
                                                     receiving) ==
              dt::TimeStatisticsReceivingDispositionV3::provider_evidence_mismatch,
          "statistics provider-evidence staleness");
  using E = dt::TimeStatisticsEvidenceClassV3;
  using R = dt::TimeStatisticsReceivingDispositionV3;
  Require(dt::ResolveTimeStatisticsEvidenceV3(E::full_visible_population) ==
              R::admitted,
          "complete visible population admitted");
  Require(dt::ResolveTimeStatisticsEvidenceV3(E::partial_scan) ==
              R::full_recollection_required &&
              dt::ResolveTimeStatisticsEvidenceV3(E::weighted_input) ==
                  R::full_recollection_required &&
              dt::ResolveTimeStatisticsEvidenceV3(E::sampled_input) ==
                  R::full_recollection_required &&
              dt::ResolveTimeStatisticsEvidenceV3(E::snapshot_merge) ==
                  R::manual_review_required &&
              dt::ResolveTimeStatisticsEvidenceV3(
                  E::histogram_source_contradiction) ==
                  R::provider_evidence_mismatch,
          "incomplete population refusals");
  auto corrupt = encoded.bytes;
  corrupt[20] ^= 1;
  Require(!dt::DecodeTimeStatisticsProjectionV3(profile, corrupt).ok(),
          "statistics mutation refusal");
  using RA = dt::TimeStatisticsReadAdmissionDispositionV3;
  receiving = {false, false, 1, 2, 3, evidence->provider_evidence_uuid};
  auto read_admission =
      dt::AdmitTimeStatisticsReadV3(profile, corrupt, receiving);
  Require(read_admission.disposition == RA::security_denied &&
              read_admission.diagnostic.diagnostic_code ==
                  "SECURITY.ACCESS_DENIED" &&
              !read_admission.decode_attempted &&
              read_admission.receiving_owner_enforcement_required &&
              read_admission.datatype_outcome_unchanged &&
              read_admission.metric_outcome_unchanged,
          "statistics security precedes privacy and malformed format");
  receiving = {true, false, 1, 2, 3, evidence->provider_evidence_uuid};
  read_admission = dt::AdmitTimeStatisticsReadV3(profile, corrupt, receiving);
  Require(read_admission.disposition == RA::privacy_denied &&
              read_admission.diagnostic.diagnostic_code ==
                  "OPTIMIZER.STATISTICS_PRIVACY_DENIED" &&
              !read_admission.decode_attempted &&
              read_admission.datatype_outcome_unchanged &&
              read_admission.metric_outcome_unchanged,
          "statistics privacy precedes malformed format");
  receiving = {true, true, 1, 2, 3, evidence->provider_evidence_uuid};
  read_admission = dt::AdmitTimeStatisticsReadV3(profile, corrupt, receiving);
  Require(read_admission.disposition == RA::format_refused &&
              read_admission.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID" &&
              read_admission.decode_attempted &&
              read_admission.datatype_outcome_unchanged &&
              read_admission.metric_outcome_unchanged,
          "authorized malformed statistics select format diagnostic");
  read_admission = dt::AdmitTimeStatisticsReadV3(
      {}, encoded.bytes, receiving);
  Require(read_admission.disposition == RA::profile_refused &&
              read_admission.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              read_admission.decode_attempted,
          "authorized statistics invalid profile classification");
  dt::TimeExecutionControlV3 statistics_control;
  statistics_control.maximum_allocation_bytes = encoded.bytes.size() - 1;
  read_admission = dt::AdmitTimeStatisticsReadV3(
      profile, encoded.bytes, receiving, statistics_control);
  Require(read_admission.disposition == RA::resource_refused &&
              read_admission.diagnostic.diagnostic_code ==
                  "RESOURCE.BUDGET_EXCEEDED" &&
              read_admission.decode_attempted,
          "authorized statistics resource classification");
  statistics_control.maximum_allocation_bytes =
      std::numeric_limits<platform::u64>::max();
  statistics_control.cancelled = &AlwaysCancel;
  read_admission = dt::AdmitTimeStatisticsReadV3(
      profile, encoded.bytes, receiving, statistics_control);
  Require(read_admission.disposition == RA::cancelled &&
              read_admission.diagnostic.diagnostic_code ==
                  "PROCESS.CANCELLED" &&
              read_admission.decode_attempted,
          "authorized statistics cancellation classification");
  read_admission =
      dt::AdmitTimeStatisticsReadV3(profile, encoded.bytes, receiving);
  Require(read_admission.ok() && read_admission.decode_attempted &&
              read_admission.statistics.statistics_uuid ==
                  projection.statistics_uuid &&
              read_admission.receiving_owner_enforcement_required &&
              read_admission.datatype_outcome_unchanged &&
              read_admission.metric_outcome_unchanged,
          "authorized exact statistics read admission");
  for (const std::size_t reserved_offset : {std::size_t{264},
                                             std::size_t{279},
                                             std::size_t{512},
                                             std::size_t{527}}) {
    auto reserved_mutation = encoded.bytes;
    reserved_mutation[reserved_offset] = 1;
    Require(!dt::DecodeTimeStatisticsProjectionV3(profile,
                                                   reserved_mutation).ok(),
            "statistics reserved-span mutation refusal");
  }
  for (const std::size_t mcv_flags_offset : {std::size_t{604},
                                              std::size_t{652}}) {
    auto flags_mutation = encoded.bytes;
    flags_mutation[mcv_flags_offset] = 0;
    Require(!dt::DecodeTimeStatisticsProjectionV3(profile,
                                                   flags_mutation).ok(),
            "statistics MCV redaction flag mutation refusal");
  }
  for (std::size_t i = 0; i != encoded.bytes.size(); ++i) {
    auto mutation = encoded.bytes;
    mutation[i] ^= 1;
    Require(!dt::DecodeTimeStatisticsProjectionV3(profile, mutation).ok(),
            "statistics mutation matrix");
  }
  dt::TimeExecutionControlV3 small{encoded.bytes.size() - 1};
  Require(!dt::DecodeTimeStatisticsProjectionV3(profile, encoded.bytes,
                                                 small).ok(),
          "statistics decode allocation refusal");
  auto absent_with_inferred_minimum = projection;
  absent_with_inferred_minimum.row_count = projection.null_count;
  absent_with_inferred_minimum.value_count = 0;
  absent_with_inferred_minimum.minimum_maximum_present = false;
  absent_with_inferred_minimum.minimum_nanoseconds = 1;
  absent_with_inferred_minimum.maximum_nanoseconds = 0;
  absent_with_inferred_minimum.histogram = {};
  absent_with_inferred_minimum.mcv = {};
  const auto absent_refusal =
      dt::EncodeTimeStatisticsProjectionV3(absent_with_inferred_minimum);
  Require(!absent_refusal.ok() && absent_refusal.bytes.empty(),
          "statistics absent min/max rejects nonzero input without output");
  auto invalid_histogram = histogram;
  invalid_histogram[1].inclusive_upper_nanoseconds = 0;
  auto refused_projection = projection;
  refused_projection.histogram = invalid_histogram;
  Require(!dt::EncodeTimeStatisticsProjectionV3(refused_projection).ok(),
          "statistics non-increasing histogram refusal");
  invalid_histogram = histogram;
  invalid_histogram[1].cumulative_count = 1;
  refused_projection.histogram = invalid_histogram;
  Require(!dt::EncodeTimeStatisticsProjectionV3(refused_projection).ok(),
          "statistics incomplete histogram population refusal");
  auto invalid_mcv = mcv;
  invalid_mcv[0].frequency = 1;
  invalid_mcv[1].frequency = 2;
  refused_projection = projection;
  refused_projection.mcv = invalid_mcv;
  Require(!dt::EncodeTimeStatisticsProjectionV3(refused_projection).ok(),
          "statistics descending MCV frequency refusal");
  invalid_mcv = mcv;
  invalid_mcv[0].frequency = 2;
  invalid_mcv[1].frequency = 1;
  refused_projection.mcv = invalid_mcv;
  Require(!dt::EncodeTimeStatisticsProjectionV3(refused_projection).ok(),
          "statistics MCV population total refusal");

  auto null_only = projection;
  null_only.row_count = 5;
  null_only.null_count = 5;
  null_only.value_count = 0;
  null_only.minimum_maximum_present = false;
  null_only.minimum_nanoseconds = 0;
  null_only.maximum_nanoseconds = 0;
  null_only.histogram = {};
  null_only.mcv = {};
  const auto null_only_encoded = dt::EncodeTimeStatisticsProjectionV3(null_only);
  Require(null_only_encoded.ok() && null_only_encoded.bytes.size() == 528 &&
              LoadLittle32(null_only_encoded.bytes.data() + 16) == 0,
          "statistics null-only complete population fixture");

  std::array<dt::TimeStatisticsHistogramRecordV3, 64> maximum_histogram{};
  std::array<dt::TimeStatisticsMcvRecordV3, 64> maximum_mcv{};
  for (unsigned i = 0; i != 64; ++i) {
    maximum_histogram[i] = {i, i + 1};
    maximum_mcv[i].value_hash[31] = static_cast<platform::byte>(i);
    maximum_mcv[i].frequency = 1;
  }
  auto maximum_records = projection;
  maximum_records.row_count = 64;
  maximum_records.null_count = 0;
  maximum_records.value_count = 64;
  maximum_records.maximum_nanoseconds = 63;
  maximum_records.histogram = maximum_histogram;
  maximum_records.mcv = maximum_mcv;
  const auto maximum_encoded =
      dt::EncodeTimeStatisticsProjectionV3(maximum_records);
  Require(maximum_encoded.ok() && maximum_encoded.bytes.size() ==
                                      dt::kTimeStatisticsMaximumBytesV3,
          "statistics exact maximum record counts fixture");

  CancelAfterContext between_groups{0, 1};
  const auto between_groups_refusal = dt::EncodeTimeStatisticsProjectionV3(
      projection, {~platform::u64{0}, CancelAfter, &between_groups});
  Require(!between_groups_refusal.ok() && between_groups_refusal.bytes.empty() &&
              between_groups_refusal.diagnostic.diagnostic_code ==
                  "PROCESS.CANCELLED",
          "statistics between-group cancellation without publication");
  CancelAfterContext before_publication{0, 2};
  const auto before_publication_refusal = dt::EncodeTimeStatisticsProjectionV3(
      projection, {~platform::u64{0}, CancelAfter, &before_publication});
  Require(!before_publication_refusal.ok() &&
              before_publication_refusal.bytes.empty() &&
              before_publication_refusal.diagnostic.diagnostic_code ==
                  "PROCESS.CANCELLED",
          "statistics final cancellation without publication");

  const auto null_value = Null(profile);
  const auto maximum = Value(profile, dt::kTimeMaximumNanosecondsV3);
  auto backup_null = dt::EncodeTimeBackupTupleV3(null_value);
  auto backup_value = dt::EncodeTimeBackupTupleV3(maximum);
  Require(backup_null.ok() && backup_null.bytes.size() == 296 &&
              backup_value.ok() && backup_value.bytes.size() == 304 &&
              std::memcmp(backup_value.bytes.data(), "SBTIMB01", 8) == 0,
          "SBTIMB01 extents");
  auto restored = dt::DecodeTimeBackupTupleNoAllocV3(
      *profile, true, backup_value.bytes);
  Require(restored.ok() && restored.tuple.nanoseconds_since_midnight ==
                               dt::kTimeMaximumNanosecondsV3,
          "backup inner tuple roundtrip");
  Require(!dt::DecodeTimeBackupTupleNoAllocV3(*profile, false,
                                               backup_null.bytes).ok(),
          "backup NULL admission");
  auto bad_backup = backup_value.bytes; bad_backup[303] ^= 1;
  Require(!dt::DecodeTimeBackupTupleNoAllocV3(*profile, true,
                                               bad_backup).ok(),
          "backup mutation refusal");
  for (std::size_t i = 0; i != backup_value.bytes.size(); ++i) {
    auto mutation = backup_value.bytes;
    mutation[i] ^= 1;
    Require(!dt::DecodeTimeBackupTupleNoAllocV3(*profile, true, mutation).ok(),
            "backup mutation matrix");
  }
}

void TestProjectionHashMemoryAndScrubs(
    const std::shared_ptr<const dt::TimeValidatedProfileHandleV3>& profile) {
  const auto index=[](dt::TimeScrubClassV3 value){
    return static_cast<std::size_t>(value);
  };
  const auto require_record_scrubs=[&](
      const ScrubProbe& probe,platform::u64 record_hashes,
      platform::u64 record_blocks,platform::u64 profile_validations,
      dt::TimeScrubClassV3 staging,platform::u64 staging_bytes,
      platform::u64 prior_calls,platform::u64 owned_bytes,
      const char* message){
    const auto hashes=record_hashes+2*profile_validations;
    const auto blocks=record_blocks+16*profile_validations;
    Require(probe.all_zero,message);
    Require(probe.calls[index(dt::TimeScrubClassV3::sha_state)]==hashes&&
                probe.bytes[index(dt::TimeScrubClassV3::sha_state)]==hashes*32,
            message);
    Require(probe.calls[index(dt::TimeScrubClassV3::sha_schedule)]==blocks&&
                probe.bytes[index(dt::TimeScrubClassV3::sha_schedule)]==blocks*256,
            message);
    Require(probe.calls[index(dt::TimeScrubClassV3::sha_tail)]==hashes&&
                probe.bytes[index(dt::TimeScrubClassV3::sha_tail)]==hashes*128,
            message);
    Require(probe.calls[index(dt::TimeScrubClassV3::hash_digest)]==record_hashes&&
                probe.bytes[index(dt::TimeScrubClassV3::hash_digest)]==record_hashes*32,
            message);
    Require(probe.calls[index(dt::TimeScrubClassV3::profile_material)]==profile_validations&&
                probe.bytes[index(dt::TimeScrubClassV3::profile_material)]==profile_validations*592&&
                probe.calls[index(dt::TimeScrubClassV3::comparison_material)]==profile_validations&&
                probe.bytes[index(dt::TimeScrubClassV3::comparison_material)]==profile_validations*352&&
                probe.calls[index(dt::TimeScrubClassV3::profile_digest)]==profile_validations&&
                probe.calls[index(dt::TimeScrubClassV3::comparison_digest)]==profile_validations,
            message);
    Require(probe.calls[index(staging)]==1&&
                probe.bytes[index(staging)]==staging_bytes,message);
    Require(probe.calls[index(dt::TimeScrubClassV3::statistics_prior_hash)]==
                prior_calls&&
                probe.bytes[index(dt::TimeScrubClassV3::statistics_prior_hash)]==
                    prior_calls*32,
            message);
    Require(probe.calls[index(dt::TimeScrubClassV3::projection_owned_buffer)]==
                (owned_bytes?1:0)&&
                probe.bytes[index(dt::TimeScrubClassV3::projection_owned_buffer)]==
                    owned_bytes,
            message);
  };

  const auto value=Value(profile,45'296'100'000'000ull);
  dt::TimeIndexProjectionRequestV3 hash_request;
  hash_request.compatibility=dt::LookupTimeIndexCompatibilityIdentityV3(
      dt::TimeIndexFamilyV3::hash);
  hash_request.mode=dt::TimeProjectionModeV3::equality_hash;
  hash_request.value=&value;
  ScrubProbe hash_probe;
  hash_request.control.observe_scrubbed=&ObserveScrub;
  hash_request.control.scrub_observer_context=&hash_probe;
  const auto hash=dt::ProjectTimeIndexValueV3(hash_request);
  Require(hash.ok()&&hash.bytes.size()==32&&hash_probe.all_zero,
          "projection stack hash success");
  Require(hash_probe.calls[index(dt::TimeScrubClassV3::sha_state)]==3&&
              hash_probe.calls[index(dt::TimeScrubClassV3::sha_schedule)]==18&&
              hash_probe.calls[index(dt::TimeScrubClassV3::sha_tail)]==3&&
              hash_probe.calls[index(dt::TimeScrubClassV3::hash_preimage)]==1&&
              hash_probe.bytes[index(dt::TimeScrubClassV3::hash_preimage)]==109&&
              hash_probe.calls[index(dt::TimeScrubClassV3::hash_digest)]==1&&
              hash_probe.calls[index(dt::TimeScrubClassV3::profile_material)]==1&&
              hash_probe.calls[index(dt::TimeScrubClassV3::comparison_material)]==1&&
              hash_probe.calls[index(dt::TimeScrubClassV3::profile_digest)]==1&&
              hash_probe.calls[index(dt::TimeScrubClassV3::comparison_digest)]==1,
          "projection exact stack hash scrub contract");
  ScrubProbe hash_cancel_probe;
  CancelAfterContext hash_cancel{0,1};
  hash_request.control.cancelled=&CancelAfter;
  hash_request.control.cancellation_context=&hash_cancel;
  hash_request.control.scrub_observer_context=&hash_cancel_probe;
  const auto hash_cancelled=dt::ProjectTimeIndexValueV3(hash_request);
  Require(!hash_cancelled.ok()&&hash_cancelled.bytes.empty()&&
              hash_cancelled.diagnostic.diagnostic_code=="PROCESS.CANCELLED"&&
              hash_cancel_probe.all_zero&&
              hash_cancel_probe.calls[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==1&&
              hash_cancel_probe.bytes[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==32,
          "projection final cancellation scrubs exact result stage");
  hash_request.control={};
  g_new_calls.store(0,std::memory_order_relaxed);
  g_new_budget.store(1,std::memory_order_relaxed);
  const auto heap_limited_hash=dt::ProjectTimeIndexValueV3(hash_request);
  g_new_budget.store(-1,std::memory_order_relaxed);
  Require(heap_limited_hash.ok()&&heap_limited_hash.bytes==hash.bytes&&
              g_new_calls.load(std::memory_order_relaxed)==1,
          "projection hash uses only result allocation");

  ScrubProbe covering_probe;
  dt::TimeExecutionControlV3 covering_control;
  covering_control.observe_scrubbed=&ObserveScrub;
  covering_control.scrub_observer_context=&covering_probe;
  const auto covering=dt::EncodeTimeCoveringValueV3(value,covering_control);
  Require(covering.ok(),"covering exact decode reencode scrub");
  require_record_scrubs(covering_probe,0,0,2,
      dt::TimeScrubClassV3::covering_decode_reencode,9,0,0,
      "covering exact decode reencode scrub");
  ScrubProbe covering_decode_probe;
  covering_control.scrub_observer_context=&covering_decode_probe;
  Require(dt::DecodeTimeCoveringValueNoAllocV3(
              *profile,false,covering.bytes,covering_control).ok(),
          "covering decoder scrub observable");
  require_record_scrubs(covering_decode_probe,0,0,1,
      dt::TimeScrubClassV3::covering_decode_reencode,9,0,0,
      "covering decoder scrub observable");
  ScrubProbe covering_decode_cancel_probe;
  CancelAfterContext covering_decode_cancel{0,0};
  covering_control.cancelled=&CancelAfter;
  covering_control.cancellation_context=&covering_decode_cancel;
  covering_control.scrub_observer_context=&covering_decode_cancel_probe;
  const auto covering_decode_cancelled=dt::DecodeTimeCoveringValueNoAllocV3(
      *profile,false,covering.bytes,covering_control);
  Require(!covering_decode_cancelled.ok()&&
              covering_decode_cancelled.diagnostic.diagnostic_code==
                  "PROCESS.CANCELLED",
          "covering decoder final cancellation");
  require_record_scrubs(covering_decode_cancel_probe,0,0,1,
      dt::TimeScrubClassV3::covering_decode_reencode,9,0,0,
      "covering decoder cancellation exact scrubs");
  ScrubProbe covering_cancel_probe;
  CancelAfterContext covering_cancel{0,1};
  covering_control.cancelled=&CancelAfter;
  covering_control.cancellation_context=&covering_cancel;
  covering_control.scrub_observer_context=&covering_cancel_probe;
  const auto covering_cancelled=
      dt::EncodeTimeCoveringValueV3(value,covering_control);
  Require(!covering_cancelled.ok()&&covering_cancelled.bytes.empty()&&
              covering_cancel_probe.all_zero&&
              covering_cancel_probe.bytes[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==9,
          "covering final cancellation scrubs owned stage");

  ScrubProbe range_cancel_probe;
  CancelAfterContext range_cancel{0,1};
  dt::TimeExecutionControlV3 range_control;
  range_control.cancelled=&CancelAfter;
  range_control.cancellation_context=&range_cancel;
  range_control.observe_scrubbed=&ObserveScrub;
  range_control.scrub_observer_context=&range_cancel_probe;
  const auto range_cancelled=
      dt::EncodeTimeRangePairV3(value,value,range_control);
  Require(!range_cancelled.ok()&&range_cancelled.bytes.empty()&&
              range_cancel_probe.all_zero&&
              range_cancel_probe.bytes[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==216,
          "range final cancellation scrubs owned stage");
  Require(range_cancel_probe.calls[index(dt::TimeScrubClassV3::sha_state)]==8&&
              range_cancel_probe.calls[index(dt::TimeScrubClassV3::sha_schedule)]==64&&
              range_cancel_probe.calls[index(dt::TimeScrubClassV3::profile_material)]==4&&
              range_cancel_probe.calls[index(dt::TimeScrubClassV3::ordered_key_decode_reencode)]==2,
          "range encoder observes all profile and nested-key scrubs");
  const auto range_bytes=dt::EncodeTimeRangePairV3(value,value);
  Require(range_bytes.ok(),"range decoder cancellation fixture");
  ScrubProbe range_decode_cancel_probe;
  CancelAfterContext range_decode_cancel{0,0};
  dt::TimeExecutionControlV3 range_decode_control;
  range_decode_control.cancelled=&CancelAfter;
  range_decode_control.cancellation_context=&range_decode_cancel;
  range_decode_control.observe_scrubbed=&ObserveScrub;
  range_decode_control.scrub_observer_context=&range_decode_cancel_probe;
  const auto range_decode_cancelled=dt::DecodeTimeRangePairNoAllocV3(
      *profile,range_bytes.bytes,range_decode_control);
  Require(!range_decode_cancelled.ok()&&
              range_decode_cancelled.diagnostic.diagnostic_code==
                  "PROCESS.CANCELLED"&&range_decode_cancel_probe.all_zero&&
              range_decode_cancel_probe.calls[index(
                  dt::TimeScrubClassV3::ordered_key_decode_reencode)]==2&&
              range_decode_cancel_probe.calls[index(
                  dt::TimeScrubClassV3::profile_material)]==2&&
              range_decode_cancel_probe.calls[index(
                  dt::TimeScrubClassV3::sha_state)]==4,
          "range decoder validates both keys before final cancellation");
  auto range_bad_upper=range_bytes.bytes;
  range_bad_upper[108]^=1;
  dt::TimeExecutionControlV3 always_cancel_control;
  always_cancel_control.cancelled=&AlwaysCancel;
  const auto range_invalid_before_cancel=dt::DecodeTimeRangePairNoAllocV3(
      *profile,range_bad_upper,always_cancel_control);
  Require(!range_invalid_before_cancel.ok()&&
              range_invalid_before_cancel.diagnostic.diagnostic_code==
                  "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "range upper validation precedes final cancellation");

  dt::TimeZoneMapRequestV3 zone;
  zone.profile=profile;zone.null_count=1;
  ScrubProbe zone_probe;
  zone.control.observe_scrubbed=&ObserveScrub;
  zone.control.scrub_observer_context=&zone_probe;
  const auto zone_bytes=dt::EncodeTimeZoneMapV3(zone);
  Require(zone_bytes.ok(),"zone stack hash encode");
  require_record_scrubs(zone_probe,3,9,2,
      dt::TimeScrubClassV3::zone_map_decode_reencode,312,0,0,
      "zone encode exact scrubs");
  ScrubProbe zone_decode_probe;
  dt::TimeExecutionControlV3 zone_decode_control;
  zone_decode_control.observe_scrubbed=&ObserveScrub;
  zone_decode_control.scrub_observer_context=&zone_decode_probe;
  Require(dt::DecodeTimeZoneMapNoAllocV3(
              *profile,zone_bytes.bytes,zone_decode_control).ok(),
          "zone stack hash decode");
  require_record_scrubs(zone_decode_probe,2,6,1,
      dt::TimeScrubClassV3::zone_map_decode_reencode,312,0,0,
      "zone decode exact scrubs");
  ScrubProbe zone_decode_cancel_probe;
  CancelAfterContext zone_decode_cancel{0,0};
  zone_decode_control.cancelled=&CancelAfter;
  zone_decode_control.cancellation_context=&zone_decode_cancel;
  zone_decode_control.scrub_observer_context=&zone_decode_cancel_probe;
  const auto zone_decode_cancelled=dt::DecodeTimeZoneMapNoAllocV3(
      *profile,zone_bytes.bytes,zone_decode_control);
  Require(!zone_decode_cancelled.ok()&&
              zone_decode_cancelled.diagnostic.diagnostic_code==
                  "PROCESS.CANCELLED",
          "zone decoder final cancellation");
  require_record_scrubs(zone_decode_cancel_probe,2,6,1,
      dt::TimeScrubClassV3::zone_map_decode_reencode,312,0,0,
      "zone decoder cancellation exact scrubs");
  ScrubProbe zone_cancel_probe;
  CancelAfterContext zone_cancel{0,1};
  zone.control.cancelled=&CancelAfter;
  zone.control.cancellation_context=&zone_cancel;
  zone.control.scrub_observer_context=&zone_cancel_probe;
  const auto zone_cancelled=dt::EncodeTimeZoneMapV3(zone);
  Require(!zone_cancelled.ok()&&zone_cancelled.bytes.empty()&&
              zone_cancelled.diagnostic.diagnostic_code=="PROCESS.CANCELLED",
          "zone final cancellation");
  require_record_scrubs(zone_cancel_probe,3,9,2,
      dt::TimeScrubClassV3::zone_map_decode_reencode,312,0,96,
      "zone cancellation exact scrubs");
  zone.control={};
  auto populated_zone=zone;
  populated_zone.null_count=0;
  populated_zone.value_count=1;
  populated_zone.minimum=&value;
  populated_zone.maximum=&value;
  const auto populated_zone_bytes=dt::EncodeTimeZoneMapV3(populated_zone);
  Require(populated_zone_bytes.ok(),"populated zone precedence fixture");
  auto zone_bad_upper=populated_zone_bytes.bytes;
  zone_bad_upper[204]^=1;
  RehashRecord("ScratchBird.BaseTime.ZoneMap.V1",&zone_bad_upper,64);
  const auto zone_invalid_before_cancel=dt::DecodeTimeZoneMapNoAllocV3(
      *profile,zone_bad_upper,always_cancel_control);
  Require(!zone_invalid_before_cancel.ok()&&
              zone_invalid_before_cancel.diagnostic.diagnostic_code==
                  "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "zone upper-key validation precedes final cancellation");

  auto evidence=std::make_shared<dt::TimeStatisticsProviderEvidenceHandleV3>();
  evidence->provider_evidence_uuid=Instance(71);
  dt::TimeStatisticsProjectionV3 statistics;
  statistics.profile=profile;statistics.provider_evidence=evidence;
  statistics.statistics_uuid=Instance(72);
  statistics.source_object_uuid=Instance(73,false);
  statistics.schema_epoch=1;statistics.collection_epoch=1;
  statistics.security_epoch=1;
  ScrubProbe statistics_probe;
  dt::TimeExecutionControlV3 statistics_control;
  statistics_control.observe_scrubbed=&ObserveScrub;
  statistics_control.scrub_observer_context=&statistics_probe;
  const auto statistics_bytes=
      dt::EncodeTimeStatisticsProjectionV3(statistics,statistics_control);
  Require(statistics_bytes.ok(),"statistics stack hash encode");
  require_record_scrubs(statistics_probe,3,30,1,
      dt::TimeScrubClassV3::statistics_decode_reencode,4624,2,0,
      "statistics encode exact scrubs");
  ScrubProbe statistics_decode_probe;
  statistics_control.scrub_observer_context=&statistics_decode_probe;
  Require(dt::DecodeTimeStatisticsProjectionV3(
              profile,statistics_bytes.bytes,statistics_control).ok(),
          "statistics stack hash decode");
  require_record_scrubs(statistics_decode_probe,2,20,1,
      dt::TimeScrubClassV3::statistics_decode_reencode,4624,1,0,
      "statistics decode exact scrubs");
  ScrubProbe statistics_cancel_probe;
  CancelAfterContext statistics_cancel{0,2};
  statistics_control.cancelled=&CancelAfter;
  statistics_control.cancellation_context=&statistics_cancel;
  statistics_control.scrub_observer_context=&statistics_cancel_probe;
  const auto statistics_cancelled=
      dt::EncodeTimeStatisticsProjectionV3(statistics,statistics_control);
  Require(!statistics_cancelled.ok()&&statistics_cancelled.bytes.empty()&&
              statistics_cancelled.diagnostic.diagnostic_code==
                  "PROCESS.CANCELLED",
          "statistics final cancellation");
  require_record_scrubs(statistics_cancel_probe,3,30,1,
      dt::TimeScrubClassV3::statistics_decode_reencode,4624,2,528,
      "statistics cancellation exact scrubs");

  const auto histogram=std::array<dt::TimeStatisticsHistogramRecordV3,1>{
      dt::TimeStatisticsHistogramRecordV3{42,1}};
  auto populated=statistics;
  populated.row_count=1;populated.value_count=1;
  populated.minimum_maximum_present=true;
  populated.minimum_nanoseconds=42;populated.maximum_nanoseconds=42;
  populated.histogram=histogram;
  const auto populated_bytes=dt::EncodeTimeStatisticsProjectionV3(populated);
  Require(populated_bytes.ok(),"populated statistics scrub fixture");
  ScrubProbe decoded_owned_probe;
  CancelAfterContext decoded_cancel{0,1};
  dt::TimeExecutionControlV3 decoded_control;
  decoded_control.cancelled=&CancelAfter;
  decoded_control.cancellation_context=&decoded_cancel;
  decoded_control.observe_scrubbed=&ObserveScrub;
  decoded_control.scrub_observer_context=&decoded_owned_probe;
  const auto decoded_cancelled=dt::DecodeTimeStatisticsProjectionV3(
      profile,populated_bytes.bytes,decoded_control);
  Require(!decoded_cancelled.ok()&&
              decoded_cancelled.statistics.histogram.empty()&&
              decoded_owned_probe.all_zero&&
              decoded_owned_probe.bytes[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==
                  sizeof(dt::TimeStatisticsHistogramRecordV3),
          "statistics decoded owned stage scrub");
  dt::TimeStatisticsMcvRecordV3 mcv_record;
  mcv_record.value_hash[31]=1;mcv_record.frequency=1;
  auto populated_mcv=populated;
  populated_mcv.mcv=std::span<const dt::TimeStatisticsMcvRecordV3>(
      &mcv_record,1);
  const auto populated_mcv_bytes=
      dt::EncodeTimeStatisticsProjectionV3(populated_mcv);
  Require(populated_mcv_bytes.ok(),"populated MCV scrub fixture");
  ScrubProbe decoded_mcv_probe;
  CancelAfterContext decoded_mcv_cancel{0,2};
  decoded_control.cancellation_context=&decoded_mcv_cancel;
  decoded_control.scrub_observer_context=&decoded_mcv_probe;
  const auto decoded_mcv_cancelled=dt::DecodeTimeStatisticsProjectionV3(
      profile,populated_mcv_bytes.bytes,decoded_control);
  Require(!decoded_mcv_cancelled.ok()&&
              decoded_mcv_cancelled.diagnostic.diagnostic_code==
                  "PROCESS.CANCELLED"&&
              decoded_mcv_cancelled.statistics.histogram.empty()&&
              decoded_mcv_cancelled.statistics.mcv.empty()&&
              decoded_mcv_probe.all_zero&&
              decoded_mcv_probe.calls[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==2&&
              decoded_mcv_probe.bytes[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==
                  sizeof(dt::TimeStatisticsHistogramRecordV3)+
                  sizeof(dt::TimeStatisticsMcvRecordV3),
          "statistics decoded histogram and MCV owned stages scrub");
  dt::TimeStatisticsReceivingFactsV3 stale_receiving{
      true,true,2,statistics.collection_epoch,statistics.security_epoch,
      evidence->provider_evidence_uuid};
  ScrubProbe admission_refusal_probe;
  dt::TimeExecutionControlV3 admission_refusal_control;
  admission_refusal_control.observe_scrubbed=&ObserveScrub;
  admission_refusal_control.scrub_observer_context=&admission_refusal_probe;
  const auto admission_refusal=dt::AdmitTimeStatisticsReadV3(
      profile,populated_mcv_bytes.bytes,stale_receiving,
      admission_refusal_control);
  Require(!admission_refusal.ok()&&
              admission_refusal.disposition==
                  dt::TimeStatisticsReadAdmissionDispositionV3::
                      schema_epoch_mismatch&&
              admission_refusal.statistics.histogram.empty()&&
              admission_refusal.statistics.mcv.empty()&&
              admission_refusal_probe.all_zero&&
              admission_refusal_probe.calls[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==2&&
              admission_refusal_probe.bytes[index(
                  dt::TimeScrubClassV3::projection_owned_buffer)]==
                  sizeof(dt::TimeStatisticsHistogramRecordV3)+
                  sizeof(dt::TimeStatisticsMcvRecordV3),
          "statistics admission refusal scrubs decoded owned stages");

  ScrubProbe backup_probe;
  dt::TimeExecutionControlV3 backup_control;
  backup_control.observe_scrubbed=&ObserveScrub;
  backup_control.scrub_observer_context=&backup_probe;
  const auto backup=dt::EncodeTimeBackupTupleV3(value,backup_control);
  Require(backup.ok(),"backup stack hash encode");
  require_record_scrubs(backup_probe,3,18,3,
      dt::TimeScrubClassV3::backup_decode_reencode,304,0,0,
      "backup encode exact scrubs");
  ScrubProbe backup_decode_probe;
  backup_control.scrub_observer_context=&backup_decode_probe;
  Require(dt::DecodeTimeBackupTupleNoAllocV3(
              *profile,false,backup.bytes,backup_control).ok(),
          "backup stack hash decode");
  require_record_scrubs(backup_decode_probe,2,12,2,
      dt::TimeScrubClassV3::backup_decode_reencode,304,0,0,
      "backup decode exact scrubs");
  ScrubProbe backup_decode_cancel_probe;
  CancelAfterContext backup_decode_cancel{0,0};
  backup_control.cancelled=&CancelAfter;
  backup_control.cancellation_context=&backup_decode_cancel;
  backup_control.scrub_observer_context=&backup_decode_cancel_probe;
  const auto backup_decode_cancelled=dt::DecodeTimeBackupTupleNoAllocV3(
      *profile,false,backup.bytes,backup_control);
  Require(!backup_decode_cancelled.ok()&&
              backup_decode_cancelled.diagnostic.diagnostic_code==
                  "PROCESS.CANCELLED",
          "backup decoder final cancellation");
  require_record_scrubs(backup_decode_cancel_probe,2,12,2,
      dt::TimeScrubClassV3::backup_decode_reencode,304,0,0,
      "backup decoder cancellation exact scrubs");
  ScrubProbe backup_cancel_probe;
  CancelAfterContext backup_cancel{0,1};
  backup_control.cancelled=&CancelAfter;
  backup_control.cancellation_context=&backup_cancel;
  backup_control.scrub_observer_context=&backup_cancel_probe;
  const auto backup_cancelled=
      dt::EncodeTimeBackupTupleV3(value,backup_control);
  Require(!backup_cancelled.ok()&&backup_cancelled.bytes.empty(),
          "backup final cancellation");
  require_record_scrubs(backup_cancel_probe,3,18,3,
      dt::TimeScrubClassV3::backup_decode_reencode,304,0,304,
      "backup cancellation exact scrubs");
  auto backup_bad_component=backup.bytes;
  backup_bad_component.back()^=0x80;
  RehashRecord("ScratchBird.BaseTime.BackupTuple.V1",
               &backup_bad_component,264);
  const auto backup_invalid_before_cancel=dt::DecodeTimeBackupTupleNoAllocV3(
      *profile,false,backup_bad_component,always_cancel_control);
  Require(!backup_invalid_before_cancel.ok()&&
              backup_invalid_before_cancel.diagnostic.diagnostic_code==
                  "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "backup component validation precedes final cancellation");

  const auto require_one_result_allocation=[](const auto& result,
                                               const char* message){
    g_new_budget.store(-1,std::memory_order_relaxed);
    Require(result.ok()&&g_new_calls.load(std::memory_order_relaxed)==1,
            message);
  };
  g_new_calls.store(0,std::memory_order_relaxed);
  g_new_budget.store(1,std::memory_order_relaxed);
  const auto covering_one_allocation=dt::EncodeTimeCoveringValueV3(value);
  require_one_result_allocation(covering_one_allocation,
      "covering encoder uses only result allocation");
  g_new_calls.store(0,std::memory_order_relaxed);
  g_new_budget.store(1,std::memory_order_relaxed);
  const auto range_one_allocation=dt::EncodeTimeRangePairV3(value,value);
  require_one_result_allocation(range_one_allocation,
      "range encoder uses only result allocation");
  g_new_calls.store(0,std::memory_order_relaxed);
  g_new_budget.store(1,std::memory_order_relaxed);
  const auto zone_one_allocation=dt::EncodeTimeZoneMapV3(zone);
  require_one_result_allocation(zone_one_allocation,
      "zone encoder uses only result allocation");
  statistics_control={};
  g_new_calls.store(0,std::memory_order_relaxed);
  g_new_budget.store(1,std::memory_order_relaxed);
  const auto statistics_one_allocation=
      dt::EncodeTimeStatisticsProjectionV3(statistics,statistics_control);
  require_one_result_allocation(statistics_one_allocation,
      "statistics encoder uses only result allocation");
  backup_control={};
  g_new_calls.store(0,std::memory_order_relaxed);
  g_new_budget.store(1,std::memory_order_relaxed);
  const auto backup_one_allocation=
      dt::EncodeTimeBackupTupleV3(value,backup_control);
  require_one_result_allocation(backup_one_allocation,
      "backup encoder uses only result allocation");

  const auto sort_bytes=dt::MakeTimeSortKeyV3(
      value,dt::TimeSortDirectionV3::ascending,
      dt::TimeNullModeV3::nulls_last);
  Require(sort_bytes.ok(),"sort decoder heap-denial fixture");
  std::array<dt::TimeStatisticsHistogramRecordV3,64> maximum_histogram{};
  std::array<dt::TimeStatisticsMcvRecordV3,64> maximum_mcv{};
  for(unsigned i=0;i!=64;++i){
    maximum_histogram[i]={i,i+1};
    maximum_mcv[i].value_hash[31]=static_cast<platform::byte>(i);
    maximum_mcv[i].frequency=1;
  }
  auto maximum_statistics=statistics;
  maximum_statistics.row_count=64;
  maximum_statistics.value_count=64;
  maximum_statistics.minimum_maximum_present=true;
  maximum_statistics.maximum_nanoseconds=63;
  maximum_statistics.histogram=maximum_histogram;
  maximum_statistics.mcv=maximum_mcv;
  const auto maximum_statistics_bytes=
      dt::EncodeTimeStatisticsProjectionV3(maximum_statistics);
  Require(maximum_statistics_bytes.ok()&&
              maximum_statistics_bytes.bytes.size()==
                  dt::kTimeStatisticsMaximumBytesV3,
          "maximum statistics heap-denial fixture");
  const auto maximum_statistics_decoded=dt::DecodeTimeStatisticsProjectionV3(
      profile,maximum_statistics_bytes.bytes);
  Require(maximum_statistics_decoded.ok()&&
              maximum_statistics_decoded.statistics.histogram.size()==64&&
              maximum_statistics_decoded.statistics.mcv.size()==64,
          "maximum statistics direct decode");
  g_new_calls.store(0,std::memory_order_relaxed);
  g_new_budget.store(0,std::memory_order_relaxed);
  const auto maximum_statistics_no_heap=
      dt::DecodeTimeStatisticsProjectionV3(
          profile,maximum_statistics_bytes.bytes);
  g_new_budget.store(-1,std::memory_order_relaxed);
  Require(!maximum_statistics_no_heap.ok()&&
              maximum_statistics_no_heap.diagnostic.diagnostic_code==
                  "RESOURCE.BUDGET_EXCEEDED"&&
              g_new_calls.load(std::memory_order_relaxed)==1,
          "maximum statistics validation completes before owned allocation");
  g_new_calls.store(0,std::memory_order_relaxed);
  g_new_budget.store(0,std::memory_order_relaxed);
  const auto zone_no_heap=
      dt::DecodeTimeZoneMapNoAllocV3(*profile,zone_bytes.bytes);
  const auto covering_no_heap=dt::DecodeTimeCoveringValueNoAllocV3(
      *profile,false,covering.bytes);
  const auto range_no_heap=dt::DecodeTimeRangePairNoAllocV3(
      *profile,range_bytes.bytes);
  const auto sort_no_heap=dt::DecodeTimeSortKeyNoAllocV3(
      *profile,sort_bytes.bytes);
  const auto statistics_no_heap=
      dt::DecodeTimeStatisticsProjectionV3(profile,statistics_bytes.bytes);
  const auto backup_no_heap=
      dt::DecodeTimeBackupTupleNoAllocV3(*profile,false,backup.bytes);
  g_new_budget.store(-1,std::memory_order_relaxed);
  Require(zone_no_heap.ok()&&covering_no_heap.ok()&&range_no_heap.ok()&&
              sort_no_heap.ok()&&statistics_no_heap.ok()&&backup_no_heap.ok()&&
              g_new_calls.load(std::memory_order_relaxed)==0,
          "projection decoders survive complete heap denial");
}

void TestOwnerAndObservabilityRegistries() {
  const std::array<std::string_view, 10> protection_dispositions{{
      "admitted_datatype_component", "forbidden", "forbidden",
      "unsupported_current_core", "unsupported_current_core",
      "unsupported_current_core", "unsupported_current_core",
      "not_applicable_datatype_layer", "not_applicable_datatype_layer",
      "forbidden_current_core"}};
  const std::array<std::string_view, 10> protection_diagnostics{{
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED",
      "receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim",
      "receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim",
      "receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim",
      "receiving_outer_owner_typed_handoff_no_datatype_algorithm_or_order_claim",
      "owner_diagnostic_before_datatype", "owner_diagnostic_before_datatype",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED"}};
  for (unsigned i = 0; i != 10; ++i) {
    const auto row = dt::ResolveTimeProtectionV3(
        static_cast<dt::TimeProtectionCellV3>(i));
    Require(row.disposition == protection_dispositions[i],
            "protection disposition");
    Require(row.receiving_owner_handoff == (i >= 3 && i <= 8),
            "protection owner boundary");
    Require(row.datatype_admitted == (i == 0) &&
                row.diagnostic == protection_diagnostics[i],
            "protection exact admission and diagnostic");
  }
  for (unsigned i = 0; i != 27; ++i) {
    const auto row = dt::ResolveTimeWireLaneV3(
        static_cast<dt::TimeWireLaneV3>(i));
    Require(!row.admitted && row.receiving_owner_handoff,
            "wire refusal and owner handoff");
    Require(row.component_mapping_complete == (i < 3),
            "wire component mapping fact");
    Require(row.disposition ==
                (i < 3
                     ? "unsupported_current_outer_version_unallocated_datatype_component_mapping_complete"
                     : "unsupported_no_manifest_listed_compatibility_profile"),
            "wire disposition");
    Require(row.diagnostic == "CTI.TRANSPORT.UNSUPPORTED",
            "wire diagnostic");
  }
  const auto unknown_lane = dt::ResolveTimeWireLaneV3(
      static_cast<dt::TimeWireLaneV3>(27));
  Require(!unknown_lane.admitted && unknown_lane.receiving_owner_handoff &&
              !unknown_lane.component_mapping_complete &&
              unknown_lane.disposition == "unknown_lane_fail_closed" &&
              unknown_lane.diagnostic == "CTI.TRANSPORT.UNSUPPORTED",
          "unknown wire lane fails closed");

  const auto routes = dt::TimeDiagnosticRoutesV3();
  Require(routes.size() == 42, "diagnostic route count");
  unsigned leap = 0, precision = 0;
  std::string exact_route_identities{"\n"};
  std::set<std::string> route_keys;
  for (const auto& row : routes) {
    Require(!row.diagnostic_uuid.is_nil(), "diagnostic UUID");
    Require(row.ordered_parameter_schema ==
                "request_uuid;operation_uuid;statement_receipt_uuid;failure_offset_u32;reason_enum",
            "diagnostic exact parameter schema");
    Require(row.redaction ==
                "no_value_component_text_key_hash_preimage_secret_or_address",
            "diagnostic exact redaction");
    leap += row.code == "CTI.TEMPORAL.LEAP_SECOND_REFUSED";
    precision += row.code == "CTI.TEMPORAL.PRECISION_LOSS";
    Require(row.code != "CTI.TEMPORAL.ZERO_DATE_REFUSED",
            "no inherited date diagnostic");
    const auto route_key = std::to_string(static_cast<unsigned>(row.axis)) +
                           "|" + std::string(row.code);
    Require(route_keys.insert(route_key).second,
            "diagnostic axis/code route uniqueness");
    exact_route_identities += route_key + "|" + HexUuid(row.diagnostic_uuid) +
                              "\n";
  }
  Require(leap == 2 && precision == 2,
          "time-only diagnostic routes");
  Require(exact_route_identities == kExactDiagnosticRouteIdentities,
          "all exact diagnostic axis/code/UUID routes");

  using DS = dt::TimeDiagnosticRouteScalarTypeV3;
  using DX = dt::TimeDiagnosticRouteExecutionDispositionV3;
  const platform::Uuid request_uuid(
      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  const platform::Uuid operation_uuid(
      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2});
  const platform::Uuid statement_receipt_uuid(
      {0x01, 0x9d, 0, 0, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0xd7,
       0x10});
  std::array<dt::TimeDiagnosticRouteParameterV3, 5> route_payload{};
  route_payload[0].token = "request_uuid";
  route_payload[0].scalar_type = DS::uuid;
  route_payload[0].uuid_value = request_uuid;
  route_payload[1].token = "operation_uuid";
  route_payload[1].scalar_type = DS::uuid;
  route_payload[1].uuid_value = operation_uuid;
  route_payload[2].token = "statement_receipt_uuid";
  route_payload[2].scalar_type = DS::uuid;
  route_payload[2].uuid_value = statement_receipt_uuid;
  route_payload[3].token = "failure_offset_u32";
  route_payload[3].scalar_type = DS::u32;
  route_payload[3].unsigned_value = 0;
  route_payload[4].token = "reason_enum";
  route_payload[4].scalar_type = DS::closed_enum;
  route_payload[4].enum_value = "fixture";
  for (const auto& route : routes) {
    const auto fact = dt::ExecuteTimeDiagnosticRouteV3(
        {route.axis, route.code, route.diagnostic_uuid, route_payload,
         route.redaction});
    Require(fact.ok() && fact.axis == route.axis &&
                fact.diagnostic_code == route.code &&
                fact.diagnostic_uuid == route.diagnostic_uuid &&
                fact.parameter_count == 5 &&
                fact.ordered_payload == route_payload &&
                fact.redaction == route.redaction &&
                fact.metric_evidence_type_uuid == TimeAllocatedUuid(0x6d) &&
                fact.metric_name == "sb_time_diagnostic_family_total" &&
                fact.diagnostic_fact_emitted &&
                fact.receiving_owner_evidence_handoff_ready &&
                !fact.metric_mutation_admitted &&
                fact.datatype_outcome_unchanged &&
                fact.metric_outcome_unchanged,
            "all 42 typed diagnostic route vectors execute exactly");
  }
  for (const std::string_view reason :
       {"fixture", "grammar", "precision", "leap_second", "range",
        "resource", "corruption"}) {
    route_payload[4].enum_value = reason;
    Require(dt::ExecuteTimeDiagnosticRouteV3(
                {routes.front().axis, routes.front().code,
                 routes.front().diagnostic_uuid, route_payload,
                 routes.front().redaction})
                .ok(),
            "diagnostic closed reason enum member admitted");
  }
  route_payload[4].enum_value = "fixture";
  const auto& diagnostic_route = routes.front();
  const auto require_route_refusal =
      [&](std::span<const dt::TimeDiagnosticRouteParameterV3> payload,
          DX expected, const char* message) {
        const auto fact = dt::ExecuteTimeDiagnosticRouteV3(
            {diagnostic_route.axis, diagnostic_route.code,
             diagnostic_route.diagnostic_uuid, payload,
             diagnostic_route.redaction});
        Require(fact.disposition == expected &&
                    !fact.diagnostic_fact_emitted &&
                    !fact.receiving_owner_evidence_handoff_ready &&
                    !fact.metric_mutation_admitted &&
                    fact.datatype_outcome_unchanged &&
                    fact.metric_outcome_unchanged,
                message);
      };
  require_route_refusal(std::span(route_payload).first(4), DX::missing_token,
                        "diagnostic missing-token refusal without mutation");
  std::array<dt::TimeDiagnosticRouteParameterV3, 6> route_six{};
  std::copy(route_payload.begin(), route_payload.end(), route_six.begin());
  route_six[5].token = "input_extent_u32";
  route_six[5].scalar_type = DS::u32;
  require_route_refusal(route_six, DX::extra_token,
                        "diagnostic extra-known-token refusal without mutation");
  auto route_mutated_payload = route_payload;
  std::swap(route_mutated_payload[0], route_mutated_payload[1]);
  require_route_refusal(route_mutated_payload, DX::reordered_tokens,
                        "diagnostic reordered-token refusal without mutation");
  route_mutated_payload = route_payload;
  route_mutated_payload[3].scalar_type = DS::uuid;
  require_route_refusal(route_mutated_payload, DX::wrong_scalar_type,
                        "diagnostic wrong-scalar refusal without mutation");
  route_mutated_payload = route_payload;
  route_mutated_payload[3].token = "unregistered_token";
  require_route_refusal(route_mutated_payload, DX::unknown_token,
                        "diagnostic unknown-token refusal without mutation");
  route_mutated_payload = route_payload;
  route_mutated_payload[3].token = "input_extent_u32";
  require_route_refusal(route_mutated_payload, DX::mismatched_known_token,
                        "diagnostic mismatched-known-token refusal without mutation");
  route_mutated_payload = route_payload;
  route_mutated_payload[3].unsigned_value =
      static_cast<platform::u64>(std::numeric_limits<platform::u32>::max()) + 1;
  require_route_refusal(route_mutated_payload, DX::invalid_scalar_value,
                        "diagnostic invalid-u32 refusal without mutation");
  route_mutated_payload = route_payload;
  route_mutated_payload[2].uuid_value.bytes[15] ^= 1;
  require_route_refusal(
      route_mutated_payload, DX::invalid_scalar_value,
      "diagnostic non-D710 statement receipt refusal without mutation");
  route_mutated_payload = route_payload;
  route_mutated_payload[4].enum_value = "not_registered";
  require_route_refusal(route_mutated_payload, DX::invalid_scalar_value,
                        "diagnostic open enum refusal without mutation");
  const auto wrong_code = dt::ExecuteTimeDiagnosticRouteV3(
      {diagnostic_route.axis, "CTI.TEMPORAL.INVALID_LITERAL",
       diagnostic_route.diagnostic_uuid, route_payload,
       diagnostic_route.redaction});
  Require(wrong_code.disposition == DX::code_uuid_mismatch &&
              wrong_code.datatype_outcome_unchanged &&
              wrong_code.metric_outcome_unchanged,
          "diagnostic code UUID pair validated without mutation");
  const auto wrong_redaction = dt::ExecuteTimeDiagnosticRouteV3(
      {diagnostic_route.axis, diagnostic_route.code,
       diagnostic_route.diagnostic_uuid, route_payload, "unsafe"});
  Require(wrong_redaction.disposition == DX::redaction_mismatch &&
              wrong_redaction.datatype_outcome_unchanged &&
              wrong_redaction.metric_outcome_unchanged,
          "diagnostic redaction validated without mutation");
  const auto wrong_axis = dt::ExecuteTimeDiagnosticRouteV3(
      {dt::TimeDiagnosticAxisV3::descriptor_codec, diagnostic_route.code,
       diagnostic_route.diagnostic_uuid, route_payload,
       diagnostic_route.redaction});
  Require(wrong_axis.disposition == DX::unknown_route &&
              wrong_axis.datatype_outcome_unchanged &&
              wrong_axis.metric_outcome_unchanged,
          "diagnostic axis validated without mutation");

  const std::array<std::string_view, 28> global_precedence{{
      "SBLR.OPERAND_INVALID", "SBLR.ENVELOPE.CHECKSUM_INVALID",
      "STORAGE.PAGE_CHECKSUM_FAILED", "SECURITY.ACCESS_DENIED",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID", "DATATYPE.NULL_STATE.INVALID",
      "DATATYPE.NULL_NOT_ADMITTED", "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "DATATYPE.CAST_FORBIDDEN", "CTI.TEMPORAL.INVALID_LITERAL",
      "CTI.TEMPORAL.LEAP_SECOND_REFUSED", "CTI.TEMPORAL.PRECISION_LOSS",
      "CTI.TEMPORAL.RANGE_EXCEEDED", "CTB.TEXT.LENGTH_EXCEEDED",
      "CTI.TEMPORAL.OPERATION_REFUSED",
      "CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
      "CTI.TEMPORAL.ORDERING_REFUSED", "DOMAIN.CONSTRAINT_FAILED",
      "OPTIMIZER.STATISTICS_PRIVACY_DENIED",
      "OPTIMIZER.STATISTICS_EPOCH_MISMATCH",
      "OPTIMIZER.INDEX_COMPATIBILITY_MISSING",
      "CTI.TEMPORAL.INDEX_KEY_REFUSED",
      "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED", "CTI.TRANSPORT.UNSUPPORTED",
      "RESOURCE.BUDGET_EXCEEDED", "PROCESS.CANCELLED",
      "CTI.MERGE.MANUAL_REVIEW_REQUIRED"}};
  const std::array<std::string_view, 12> cast_precedence{{
      "SECURITY.ACCESS_DENIED", "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID", "DATATYPE.NULL_NOT_ADMITTED",
      "DATATYPE.CAST_FORBIDDEN", "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.INVALID_LITERAL", "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
      "CTI.TEMPORAL.PRECISION_LOSS", "CTB.TEXT.LENGTH_EXCEEDED",
      "RESOURCE.BUDGET_EXCEEDED", "PROCESS.CANCELLED"}};
  const std::array<std::string_view, 12> operation_precedence{{
      "SECURITY.ACCESS_DENIED", "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "DATATYPE.NULL_STATE.INVALID", "DATATYPE.NULL_NOT_ADMITTED",
      "CTI.TEMPORAL.OPERATION_REFUSED",
      "CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
      "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
      "CTI.TEMPORAL.INVALID_LITERAL", "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
      "CTI.TEMPORAL.RANGE_EXCEEDED", "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED"}};
  const std::array<std::string_view, 7> text_precedence{{
      "shape", "hour", "minute", "second_leap", "fraction_syntax",
      "precision", "trailing_zero"}};
  const std::array<std::string_view, 6> intrinsic_operation_gates{{
      "authority", "state", "classification", "admitted NULL propagation",
      "PRESENT payload", "resource cancellation publication"}};
  Require(std::ranges::equal(
              dt::TimeDiagnosticPrecedenceV3(
                  dt::TimeDiagnosticPrecedenceScopeV3::global),
              global_precedence) &&
              std::ranges::equal(
                  dt::TimeDiagnosticPrecedenceV3(
                      dt::TimeDiagnosticPrecedenceScopeV3::cast),
                  cast_precedence) &&
              std::ranges::equal(
                  dt::TimeDiagnosticPrecedenceV3(
                      dt::TimeDiagnosticPrecedenceScopeV3::operation),
                  operation_precedence) &&
              std::ranges::equal(
                  dt::TimeDiagnosticPrecedenceV3(
                      dt::TimeDiagnosticPrecedenceScopeV3::text_fields),
                  text_precedence) &&
              std::ranges::equal(
                  dt::TimeDiagnosticPrecedenceV3(
                      dt::TimeDiagnosticPrecedenceScopeV3::intrinsic_operation_gates),
                  intrinsic_operation_gates),
          "exact global and scoped diagnostic precedence");
  Require(dt::TimeDiagnosticSelectionRuleV3() ==
              "named binary format exact sequential gates win; scoped order applies within one gate; global order is fallback",
          "binary sequential-gate precedence override");
  const auto format_orders = dt::TimeDiagnosticFormatOrdersV3();
  Require(format_orders.size() == 5 &&
              format_orders[0].format_magic == "SBTIMP01" &&
              format_orders[0].authority_file ==
                  "base-time-profile-registry.yaml" &&
              format_orders[1].format_magic == "SBTIMC01" &&
              format_orders[2].format_magic == "SBTIMK01" &&
              format_orders[2].authority_file ==
                  "base-time-index-projection-layouts.yaml" &&
              format_orders[3].format_magic == "SBTIMS01" &&
              format_orders[3].authority_file ==
                  "base-time-statistics-layout.yaml" &&
              format_orders[4].format_magic == "SBTIMB01" &&
              format_orders[4].authority_file ==
                  "base-time-backup-transport-layout.yaml",
          "exact binary-format precedence references");
  const auto combined_faults = dt::TimeDiagnosticTextCombinedFaultsV3();
  Require(combined_faults.size() == 5 &&
              combined_faults[0].supplied_text == "12:34:60.1234567890" &&
              combined_faults[0].selected_reason == "leap" &&
              combined_faults[1].selected_reason == "invalid" &&
              combined_faults[2].selected_reason == "precision" &&
              combined_faults[3].selected_reason == "leap" &&
              combined_faults[4].selected_reason == "leap",
          "exact combined text fault selections");
  const auto classification_rules = dt::TimeDiagnosticClassificationRulesV3();
  Require(classification_rules.size() == 2 &&
              classification_rules[0].scope == "forbidden_cast" &&
              classification_rules[0].rule ==
                  "classification before PRESENT payload" &&
              classification_rules[1].scope == "operation" &&
              classification_rules[1].rule ==
                  "classification before PRESENT payload",
          "exact combined classification-before-payload rules");

  const auto diagnostic_metric_routes = dt::TimeDiagnosticMetricRoutesV3();
  Require(diagnostic_metric_routes.size() == 15,
          "diagnostic-family metric route count");
  for (unsigned i = 0; i != diagnostic_metric_routes.size(); ++i) {
    const auto& route = diagnostic_metric_routes[i];
    Require(static_cast<unsigned>(route.diagnostic_family) == i &&
                route.metric_evidence_type_uuid == TimeAllocatedUuid(0x6d) &&
                route.metric_name == "sb_time_diagnostic_family_total" &&
                route.rule ==
                    "after final primary diagnostic selection exactly once",
            "exact diagnostic-family metric route");
    const auto source_route = std::find_if(
        routes.begin(), routes.end(), [&](const auto& value) {
          return value.axis == route.diagnostic_family;
        });
    Require(source_route != routes.end(), "diagnostic axis has primary route");
    const auto resolved = dt::ResolveTimeDiagnosticMetricRouteV3(
        {source_route->axis, source_route->diagnostic_uuid});
    Require(resolved.disposition ==
                dt::TimeDiagnosticRouteFactDispositionV3::admitted &&
                !resolved.metric_mutation_admitted &&
                resolved.route.diagnostic_family == route.diagnostic_family,
            "typed diagnostic route key resolution without mutation");
  }
  using DR = dt::TimeDiagnosticRouteFactDispositionV3;
  Require(dt::ValidateTimeDiagnosticMetricRouteTableV3(
              diagnostic_metric_routes) == DR::admitted,
          "exact diagnostic metric route table");
  Require(dt::ValidateTimeDiagnosticMetricRouteTableV3(
              diagnostic_metric_routes.first(14)) == DR::missing,
          "missing diagnostic route refusal");
  std::vector<dt::TimeDiagnosticMetricRouteV3> route_mutation(
      diagnostic_metric_routes.begin(), diagnostic_metric_routes.end());
  std::swap(route_mutation[0], route_mutation[1]);
  Require(dt::ValidateTimeDiagnosticMetricRouteTableV3(route_mutation) ==
              DR::reordered,
          "reordered diagnostic route refusal");
  route_mutation.assign(diagnostic_metric_routes.begin(),
                        diagnostic_metric_routes.end());
  route_mutation[1] = route_mutation[0];
  Require(dt::ValidateTimeDiagnosticMetricRouteTableV3(route_mutation) ==
              DR::duplicate,
          "duplicate diagnostic route refusal");
  route_mutation.assign(diagnostic_metric_routes.begin(),
                        diagnostic_metric_routes.end());
  route_mutation[0].diagnostic_family =
      static_cast<dt::TimeDiagnosticAxisV3>(15);
  Require(dt::ValidateTimeDiagnosticMetricRouteTableV3(route_mutation) ==
              DR::unknown,
          "unknown diagnostic route refusal");
  Require(dt::ValidateTimeDiagnosticMetricRouteTableV3(
              diagnostic_metric_routes, false) == DR::wrong_type,
          "wrong-type diagnostic route refusal");

  const std::array<std::string_view, 21> names{{
      "sb_time_descriptor_admissions_total", "sb_time_invalid_literals_total",
      "sb_time_range_refusals_total", "sb_time_operation_attempts_total",
      "sb_time_operation_success_total", "sb_time_operation_refusals_total",
      "sb_time_cast_attempts_total", "sb_time_cast_success_total",
      "sb_time_index_admission_refusals_total", "sb_time_statistics_stale_total",
      "sb_time_reference_mapping_misses_total", "sb_time_transport_refusals_total",
      "sb_time_serialization_refusals_total", "sb_time_protection_refusals_total",
      "sb_time_merge_manual_review_total", "sb_time_resource_budget_refusals_total",
      "sb_time_cancellations_total", "sb_time_decode_corruption_total",
      "sb_time_diagnostic_family_total", "sb_time_precision_loss_refusals_total",
      "sb_time_leap_second_refusals_total"}};
  const std::array<std::string_view, 21> producers{{
      "time_descriptor", "time_text_and_constructor", "time_arithmetic",
      "time_runtime", "time_runtime", "time_runtime", "time_cast", "time_cast",
      "time_projection", "time_statistics", "time_boundary", "transport_owner",
      "time_boundary", "time_protection", "merge_owner", "resource_owner",
      "process_owner", "time_boundary", "metrics_diagnostic_router",
      "time_text_and_constructor", "time_text_and_constructor"}};
  const std::array<std::string_view, 21> records{{
      "time_descriptor_admissions_evidence", "time_invalid_literals_evidence",
      "time_range_refusals_evidence", "time_operation_attempts_evidence",
      "time_operation_success_evidence", "time_operation_refusals_evidence",
      "time_cast_attempts_evidence", "time_cast_success_evidence",
      "time_index_admission_refusals_evidence",
      "time_statistics_stale_evidence", "time_reference_mapping_misses_evidence",
      "time_transport_refusals_evidence", "time_serialization_refusals_evidence",
      "time_protection_refusals_evidence", "time_merge_manual_review_evidence",
      "time_resource_budget_refusals_evidence", "time_cancellations_evidence",
      "time_decode_corruption_evidence", "time_diagnostic_family_evidence",
      "time_precision_loss_refusals_evidence",
      "time_leap_second_refusals_evidence"}};
  const std::array<std::string_view, 21> labels{{
      "database_uuid,node_uuid,result", "database_uuid,node_uuid,reason",
      "database_uuid,node_uuid,operation", "database_uuid,node_uuid,operation",
      "database_uuid,node_uuid,operation",
      "database_uuid,node_uuid,operation,reason",
      "database_uuid,node_uuid,cast_context",
      "database_uuid,node_uuid,cast_context",
      "database_uuid,node_uuid,index_family,reason",
      "database_uuid,node_uuid,reason",
      "database_uuid,node_uuid,lane_group,reason",
      "database_uuid,node_uuid,lane_group,reason",
      "database_uuid,node_uuid,boundary,reason",
      "database_uuid,node_uuid,layer,reason", "database_uuid,node_uuid,reason",
      "database_uuid,node_uuid,operation,reason",
      "database_uuid,node_uuid,operation",
      "database_uuid,node_uuid,boundary,reason",
      "database_uuid,node_uuid,diagnostic_family",
      "database_uuid,node_uuid,operation,reason",
      "database_uuid,node_uuid,operation,reason"}};
  const std::array<unsigned, 21> tuple_counts{{
      1, 7, 4, 27, 24, 32, 3, 3, 16, 4, 1, 2, 47, 3, 1, 1, 24, 25, 15,
      2, 3}};
  const std::array<std::string_view, 21> phases{{
      "after_admission", "after_primary_diagnostic_commit",
      "after_primary_diagnostic_commit", "after_input_admission_before_work",
      "after_atomic_publication", "after_primary_diagnostic_commit",
      "after_input_admission_before_work", "after_atomic_publication",
      "after_primary_diagnostic_commit", "after_staleness_commit",
      "after_lookup_commit", "after_primary_diagnostic_commit",
      "after_primary_diagnostic_commit", "after_primary_diagnostic_commit",
      "after_primary_diagnostic_commit", "after_primary_diagnostic_commit",
      "after_primary_diagnostic_commit", "after_primary_diagnostic_commit",
      "after_primary_diagnostic_commit", "after_primary_diagnostic_commit",
      "after_primary_diagnostic_commit"}};
  const std::array<std::string_view, 21> conditions{{
      "exact_d710_profile_admitted", "CTI.TEMPORAL.INVALID_LITERAL",
      "CTI.TEMPORAL.RANGE_EXCEEDED", "classified_attempt_operation_id",
      "admitted_success_operation_id", "declared_operation_reason_tuple",
      "admitted_cast_pair_and_context", "admitted_cast_target",
      "CTI.TEMPORAL.INDEX_KEY_REFUSED", "exact_profile_or_epoch_mismatch",
      "manifest_listed_reference_profile_absent", "CTI.TRANSPORT.UNSUPPORTED",
      "reachable_product_boundary_serialization_refusal",
      "CTI.TEMPORAL.PROTECTION_UNSUPPORTED",
      "CTI.MERGE.MANUAL_REVIEW_REQUIRED", "RESOURCE.BUDGET_EXCEEDED",
      "PROCESS.CANCELLED", "reachable_product_boundary_decode_corruption",
      "exact_selected_diagnostic_family", "CTI.TEMPORAL.PRECISION_LOSS",
      "CTI.TEMPORAL.LEAP_SECOND_REFUSED"}};
  const auto metrics = dt::TimeMetricEvidenceTypesV3();
  Require(metrics.size() == 21, "metric evidence count");
  std::set<std::array<platform::byte, 16>> source_events;
  for (unsigned i = 0; i != metrics.size(); ++i) {
    const auto& row = metrics[i];
    Require(static_cast<unsigned>(row.type) == i, "metric enum order");
    Require(row.evidence_type_uuid == TimeAllocatedUuid(
                static_cast<platform::byte>(0x5b + i)),
            "exact metric evidence UUID allocation");
    Require(row.generation == 1 && row.metric_name_metadata == names[i] &&
                row.evidence_producer == producers[i],
            "metric typed identity");
    Require(row.evidence_record == records[i], "exact metric evidence record");
    Require(row.manager_consumer == "metrics_registry_manager",
            "metric consumer");
    Require(row.source_event_uuid == TimeAllocatedUuid(
                static_cast<platform::byte>(0x72 + i)) &&
                source_events.insert(row.source_event_uuid.bytes).second,
            "exact metric source-event identity");
    const auto exact_cardinality =
        "<=" + std::to_string(tuple_counts[i] * 256) +
        " series/local_node exact 256_database_times_" +
        std::to_string(tuple_counts[i]) + "_declared_tuple bound";
    Require(row.closed_label_schema == labels[i] &&
                row.cardinality_bound == exact_cardinality,
            "exact metric closed labels and cardinality");
    Require(row.trigger_phase == phases[i] &&
                row.trigger_condition == conditions[i] &&
                row.update_rule.find("tuple must belong to the exact declared tuple union") !=
                    std::string_view::npos,
            "exact metric typed trigger");
    const std::string_view common_failure =
        "missing_or_stale_unknown_never_changes_datatype_result; undeclared tuple rejected without metric mutation";
    const std::string_view diagnostic_family_failure =
        "absent or ambiguous family or undeclared tuple refuses the sample; undeclared tuple rejected without metric mutation; emit metrics evidence without aggregate mutation; datatype result unchanged";
    Require(row.failure_behavior ==
                (i == 18 ? diagnostic_family_failure : common_failure),
            "exact metric non-interference fact");
    Require(row.accepted_final_outcome ==
                "one_typed_evidence_emission_after_primary_outcome_commit",
            "metric exactly-once fact");
  }

  const auto envelope_identity = dt::TimeMetricEvidenceEnvelopeIdentityV3Value();
  Require(envelope_identity.schema_uuid == TimeAllocatedUuid(0x5a) &&
              envelope_identity.generation == 1,
          "exact metric evidence envelope identity");
  const auto closed_tuples = dt::TimeMetricClosedLabelTuplesV3();
  Require(closed_tuples.size() == 245, "exact closed metric tuple total");
  struct ExpectedMetricTuple {
    unsigned type;
    std::string_view first_name;
    std::string_view first_value;
    std::string_view second_name;
    std::string_view second_value;
    unsigned member_count;
  };
  static constexpr std::array<ExpectedMetricTuple, 245> expected_tuples{{
      {0, "result", "admitted", "", "", 1},
      {1, "reason", "grammar", "", "", 1},
      {1, "reason", "hour", "", "", 1},
      {1, "reason", "minute", "", "", 1},
      {1, "reason", "second", "", "", 1},
      {1, "reason", "nanosecond", "", "", 1},
      {1, "reason", "fraction_syntax", "", "", 1},
      {1, "reason", "trailing_zero", "", "", 1},
      {2, "operation", "add_nanoseconds", "", "", 1},
      {2, "operation", "subtract_nanoseconds", "", "", 1},
      {2, "operation", "successor", "", "", 1},
      {2, "operation", "predecessor", "", "", 1},
      {3, "operation", "validate_canonicalize", "", "", 1},
      {3, "operation", "civil_construct", "", "", 1},
      {3, "operation", "decompose_civil", "", "", 1},
      {3, "operation", "parse_canonical", "", "", 1},
      {3, "operation", "render_canonical", "", "", 1},
      {3, "operation", "add_nanoseconds", "", "", 1},
      {3, "operation", "subtract_nanoseconds", "", "", 1},
      {3, "operation", "successor", "", "", 1},
      {3, "operation", "predecessor", "", "", 1},
      {3, "operation", "difference_nanoseconds", "", "", 1},
      {3, "operation", "extract_hour", "", "", 1},
      {3, "operation", "extract_minute", "", "", 1},
      {3, "operation", "extract_second", "", "", 1},
      {3, "operation", "extract_nanosecond", "", "", 1},
      {3, "operation", "nanosecond_of_day", "", "", 1},
      {3, "operation", "truncate_nanosecond", "", "", 1},
      {3, "operation", "round_nanosecond", "", "", 1},
      {3, "operation", "larger_truncate_round_or_bucket", "", "", 1},
      {3, "operation", "duration_interval_or_modulo_day", "", "", 1},
      {3, "operation", "date_timestamp_or_timezone_cross_temporal", "", "", 1},
      {3, "operation", "compare", "", "", 1},
      {3, "operation", "hash", "", "", 1},
      {3, "operation", "sort_key", "", "", 1},
      {3, "operation", "cast", "", "", 1},
      {3, "operation", "serialize", "", "", 1},
      {3, "operation", "deserialize", "", "", 1},
      {3, "operation", "batch_materialize", "", "", 1},
      {4, "operation", "validate_canonicalize", "", "", 1},
      {4, "operation", "civil_construct", "", "", 1},
      {4, "operation", "decompose_civil", "", "", 1},
      {4, "operation", "parse_canonical", "", "", 1},
      {4, "operation", "render_canonical", "", "", 1},
      {4, "operation", "add_nanoseconds", "", "", 1},
      {4, "operation", "subtract_nanoseconds", "", "", 1},
      {4, "operation", "successor", "", "", 1},
      {4, "operation", "predecessor", "", "", 1},
      {4, "operation", "difference_nanoseconds", "", "", 1},
      {4, "operation", "extract_hour", "", "", 1},
      {4, "operation", "extract_minute", "", "", 1},
      {4, "operation", "extract_second", "", "", 1},
      {4, "operation", "extract_nanosecond", "", "", 1},
      {4, "operation", "nanosecond_of_day", "", "", 1},
      {4, "operation", "truncate_nanosecond", "", "", 1},
      {4, "operation", "round_nanosecond", "", "", 1},
      {4, "operation", "compare", "", "", 1},
      {4, "operation", "hash", "", "", 1},
      {4, "operation", "sort_key", "", "", 1},
      {4, "operation", "cast", "", "", 1},
      {4, "operation", "serialize", "", "", 1},
      {4, "operation", "deserialize", "", "", 1},
      {4, "operation", "batch_materialize", "", "", 1},
      {5, "operation", "add_nanoseconds", "reason", "range", 2},
      {5, "operation", "subtract_nanoseconds", "reason", "range", 2},
      {5, "operation", "successor", "reason", "range", 2},
      {5, "operation", "predecessor", "reason", "range", 2},
      {5, "operation", "batch_materialize", "reason", "allocation_budget", 2},
      {5, "operation", "validate_canonicalize", "reason", "cancelled", 2},
      {5, "operation", "civil_construct", "reason", "cancelled", 2},
      {5, "operation", "decompose_civil", "reason", "cancelled", 2},
      {5, "operation", "parse_canonical", "reason", "cancelled", 2},
      {5, "operation", "render_canonical", "reason", "cancelled", 2},
      {5, "operation", "add_nanoseconds", "reason", "cancelled", 2},
      {5, "operation", "subtract_nanoseconds", "reason", "cancelled", 2},
      {5, "operation", "successor", "reason", "cancelled", 2},
      {5, "operation", "predecessor", "reason", "cancelled", 2},
      {5, "operation", "difference_nanoseconds", "reason", "cancelled", 2},
      {5, "operation", "extract_hour", "reason", "cancelled", 2},
      {5, "operation", "extract_minute", "reason", "cancelled", 2},
      {5, "operation", "extract_second", "reason", "cancelled", 2},
      {5, "operation", "extract_nanosecond", "reason", "cancelled", 2},
      {5, "operation", "nanosecond_of_day", "reason", "cancelled", 2},
      {5, "operation", "truncate_nanosecond", "reason", "cancelled", 2},
      {5, "operation", "round_nanosecond", "reason", "cancelled", 2},
      {5, "operation", "compare", "reason", "cancelled", 2},
      {5, "operation", "hash", "reason", "cancelled", 2},
      {5, "operation", "sort_key", "reason", "cancelled", 2},
      {5, "operation", "cast", "reason", "cancelled", 2},
      {5, "operation", "serialize", "reason", "cancelled", 2},
      {5, "operation", "deserialize", "reason", "cancelled", 2},
      {5, "operation", "batch_materialize", "reason", "cancelled", 2},
      {5, "operation", "larger_truncate_round_or_bucket", "reason", "unsupported", 2},
      {5, "operation", "duration_interval_or_modulo_day", "reason", "unsupported", 2},
      {5, "operation", "date_timestamp_or_timezone_cross_temporal", "reason", "unsupported", 2},
      {6, "cast_context", "implicit", "", "", 1},
      {6, "cast_context", "assignment", "", "", 1},
      {6, "cast_context", "explicit", "", "", 1},
      {7, "cast_context", "implicit", "", "", 1},
      {7, "cast_context", "assignment", "", "", 1},
      {7, "cast_context", "explicit", "", "", 1},
      {8, "index_family", "bitmap", "reason", "provider_budget", 2},
      {8, "index_family", "bitmap", "reason", "unsupported", 2},
      {8, "index_family", "brin_like", "reason", "provider_budget", 2},
      {8, "index_family", "brin_like", "reason", "unsupported", 2},
      {8, "index_family", "btree", "reason", "provider_budget", 2},
      {8, "index_family", "btree", "reason", "unsupported", 2},
      {8, "index_family", "columnar_zone_map", "reason", "provider_budget", 2},
      {8, "index_family", "columnar_zone_map", "reason", "unsupported", 2},
      {8, "index_family", "covering_included", "reason", "provider_budget", 2},
      {8, "index_family", "covering_included", "reason", "unsupported", 2},
      {8, "index_family", "hash", "reason", "provider_budget", 2},
      {8, "index_family", "hash", "reason", "unsupported", 2},
      {8, "index_family", "range_exclusion", "reason", "provider_budget", 2},
      {8, "index_family", "range_exclusion", "reason", "unsupported", 2},
      {8, "index_family", "temporary_work", "reason", "provider_budget", 2},
      {8, "index_family", "temporary_work", "reason", "unsupported", 2},
      {9, "reason", "stale", "", "", 1},
      {9, "reason", "schema_epoch", "", "", 1},
      {9, "reason", "collection_epoch", "", "", 1},
      {9, "reason", "security_epoch", "", "", 1},
      {10, "lane_group", "reference_profile_absent", "reason", "absent", 2},
      {11, "lane_group", "native_outer_unallocated", "reason", "unsupported", 2},
      {11, "lane_group", "reference_profile_absent", "reason", "absent", 2},
      {12, "boundary", "component", "reason", "extent", 2},
      {12, "boundary", "component", "reason", "stale", 2},
      {12, "boundary", "component", "reason", "mismatch", 2},
      {12, "boundary", "component", "reason", "policy", 2},
      {12, "boundary", "SBDVAL01", "reason", "short", 2},
      {12, "boundary", "SBDVAL01", "reason", "trailing", 2},
      {12, "boundary", "SBDVAL01", "reason", "checksum", 2},
      {12, "boundary", "SBDVAL01", "reason", "reserved", 2},
      {12, "boundary", "SBDVAL01", "reason", "extent", 2},
      {12, "boundary", "SBDVAL01", "reason", "stale", 2},
      {12, "boundary", "SBDVAL01", "reason", "mismatch", 2},
      {12, "boundary", "SBDVAL01", "reason", "policy", 2},
      {12, "boundary", "SBDPV001", "reason", "short", 2},
      {12, "boundary", "SBDPV001", "reason", "trailing", 2},
      {12, "boundary", "SBDPV001", "reason", "checksum", 2},
      {12, "boundary", "SBDPV001", "reason", "reserved", 2},
      {12, "boundary", "SBDPV001", "reason", "extent", 2},
      {12, "boundary", "SBDPV001", "reason", "stale", 2},
      {12, "boundary", "SBDPV001", "reason", "mismatch", 2},
      {12, "boundary", "SBDPV001", "reason", "policy", 2},
      {12, "boundary", "backup", "reason", "short", 2},
      {12, "boundary", "backup", "reason", "trailing", 2},
      {12, "boundary", "backup", "reason", "checksum", 2},
      {12, "boundary", "backup", "reason", "reserved", 2},
      {12, "boundary", "backup", "reason", "extent", 2},
      {12, "boundary", "backup", "reason", "stale", 2},
      {12, "boundary", "backup", "reason", "mismatch", 2},
      {12, "boundary", "backup", "reason", "policy", 2},
      {12, "boundary", "statistics", "reason", "short", 2},
      {12, "boundary", "statistics", "reason", "trailing", 2},
      {12, "boundary", "statistics", "reason", "checksum", 2},
      {12, "boundary", "statistics", "reason", "reserved", 2},
      {12, "boundary", "statistics", "reason", "extent", 2},
      {12, "boundary", "statistics", "reason", "stale", 2},
      {12, "boundary", "statistics", "reason", "mismatch", 2},
      {12, "boundary", "statistics", "reason", "policy", 2},
      {12, "boundary", "ordered_key", "reason", "short", 2},
      {12, "boundary", "ordered_key", "reason", "trailing", 2},
      {12, "boundary", "ordered_key", "reason", "reserved", 2},
      {12, "boundary", "ordered_key", "reason", "extent", 2},
      {12, "boundary", "ordered_key", "reason", "stale", 2},
      {12, "boundary", "ordered_key", "reason", "mismatch", 2},
      {12, "boundary", "ordered_key", "reason", "policy", 2},
      {12, "boundary", "hash", "reason", "extent", 2},
      {12, "boundary", "hash", "reason", "stale", 2},
      {12, "boundary", "hash", "reason", "mismatch", 2},
      {12, "boundary", "hash", "reason", "policy", 2},
      {13, "layer", "inner_compression", "reason", "unsupported", 2},
      {13, "layer", "inner_encryption", "reason", "unsupported", 2},
      {13, "layer", "protected_index", "reason", "unsupported", 2},
      {14, "reason", "manual_review", "", "", 1},
      {15, "operation", "batch_materialize", "reason", "allocation_budget", 2},
      {16, "operation", "validate_canonicalize", "", "", 1},
      {16, "operation", "civil_construct", "", "", 1},
      {16, "operation", "decompose_civil", "", "", 1},
      {16, "operation", "parse_canonical", "", "", 1},
      {16, "operation", "render_canonical", "", "", 1},
      {16, "operation", "add_nanoseconds", "", "", 1},
      {16, "operation", "subtract_nanoseconds", "", "", 1},
      {16, "operation", "successor", "", "", 1},
      {16, "operation", "predecessor", "", "", 1},
      {16, "operation", "difference_nanoseconds", "", "", 1},
      {16, "operation", "extract_hour", "", "", 1},
      {16, "operation", "extract_minute", "", "", 1},
      {16, "operation", "extract_second", "", "", 1},
      {16, "operation", "extract_nanosecond", "", "", 1},
      {16, "operation", "nanosecond_of_day", "", "", 1},
      {16, "operation", "truncate_nanosecond", "", "", 1},
      {16, "operation", "round_nanosecond", "", "", 1},
      {16, "operation", "compare", "", "", 1},
      {16, "operation", "hash", "", "", 1},
      {16, "operation", "sort_key", "", "", 1},
      {16, "operation", "cast", "", "", 1},
      {16, "operation", "serialize", "", "", 1},
      {16, "operation", "deserialize", "", "", 1},
      {16, "operation", "batch_materialize", "", "", 1},
      {17, "boundary", "component", "reason", "extent", 2},
      {17, "boundary", "SBDVAL01", "reason", "short", 2},
      {17, "boundary", "SBDVAL01", "reason", "trailing", 2},
      {17, "boundary", "SBDVAL01", "reason", "checksum", 2},
      {17, "boundary", "SBDVAL01", "reason", "reserved", 2},
      {17, "boundary", "SBDVAL01", "reason", "extent", 2},
      {17, "boundary", "SBDPV001", "reason", "short", 2},
      {17, "boundary", "SBDPV001", "reason", "trailing", 2},
      {17, "boundary", "SBDPV001", "reason", "checksum", 2},
      {17, "boundary", "SBDPV001", "reason", "reserved", 2},
      {17, "boundary", "SBDPV001", "reason", "extent", 2},
      {17, "boundary", "backup", "reason", "short", 2},
      {17, "boundary", "backup", "reason", "trailing", 2},
      {17, "boundary", "backup", "reason", "checksum", 2},
      {17, "boundary", "backup", "reason", "reserved", 2},
      {17, "boundary", "backup", "reason", "extent", 2},
      {17, "boundary", "statistics", "reason", "short", 2},
      {17, "boundary", "statistics", "reason", "trailing", 2},
      {17, "boundary", "statistics", "reason", "checksum", 2},
      {17, "boundary", "statistics", "reason", "reserved", 2},
      {17, "boundary", "statistics", "reason", "extent", 2},
      {17, "boundary", "ordered_key", "reason", "short", 2},
      {17, "boundary", "ordered_key", "reason", "trailing", 2},
      {17, "boundary", "ordered_key", "reason", "reserved", 2},
      {17, "boundary", "ordered_key", "reason", "extent", 2},
      {18, "diagnostic_family", "input_parse", "", "", 1},
      {18, "diagnostic_family", "descriptor_codec", "", "", 1},
      {18, "diagnostic_family", "storage_read_write", "", "", 1},
      {18, "diagnostic_family", "cast_invalid", "", "", 1},
      {18, "diagnostic_family", "cast_range_loss", "", "", 1},
      {18, "diagnostic_family", "operation_invalid", "", "", 1},
      {18, "diagnostic_family", "bounds_overflow_underflow", "", "", 1},
      {18, "diagnostic_family", "resource_cancellation", "", "", 1},
      {18, "diagnostic_family", "compression_corruption", "", "", 1},
      {18, "diagnostic_family", "encryption_auth_key", "", "", 1},
      {18, "diagnostic_family", "wire_decode_encode", "", "", 1},
      {18, "diagnostic_family", "index_key", "", "", 1},
      {18, "diagnostic_family", "domain_validation", "", "", 1},
      {18, "diagnostic_family", "recovery_corruption", "", "", 1},
      {18, "diagnostic_family", "statistics_read", "", "", 1},
      {19, "operation", "parse_canonical", "reason", "precision", 2},
      {19, "operation", "cast", "reason", "precision", 2},
      {20, "operation", "civil_construct", "reason", "leap_second", 2},
      {20, "operation", "parse_canonical", "reason", "leap_second", 2},
      {20, "operation", "cast", "reason", "leap_second", 2},
  }};
  for (std::size_t i = 0; i != expected_tuples.size(); ++i) {
    const auto& actual = closed_tuples[i];
    const auto& expected = expected_tuples[i];
    Require(static_cast<unsigned>(actual.type) == expected.type &&
                actual.member_count == expected.member_count &&
                actual.members[0].name == expected.first_name &&
                actual.members[0].value == expected.first_value &&
                actual.members[1].name == expected.second_name &&
                actual.members[1].value == expected.second_value,
            "exact Core-owned closed metric tuple member sequence");
  }
  std::array<unsigned, 21> observed_tuple_counts{};
  std::set<std::string> unique_tuples;
  platform::u64 sequence = 1;
  for (const auto& tuple : closed_tuples) {
    const auto type_index = static_cast<unsigned>(tuple.type);
    Require(type_index < metrics.size(), "closed tuple metric type");
    ++observed_tuple_counts[type_index];
    std::string tuple_key = std::to_string(type_index);
    for (std::size_t i = 0; i != tuple.member_count; ++i) {
      tuple_key += "|" + std::string(tuple.members[i].name) + "=" +
                   std::string(tuple.members[i].value);
    }
    Require(unique_tuples.insert(tuple_key).second,
            "closed tuple uniqueness");
    dt::TimeMetricEvidenceEnvelopeV3 evidence;
    evidence.envelope_identity = envelope_identity;
    evidence.evidence_type_uuid = metrics[type_index].evidence_type_uuid;
    evidence.source_event_uuid = metrics[type_index].source_event_uuid;
    evidence.database_uuid = Instance(50);
    evidence.node_uuid = Instance(51);
    evidence.process_epoch = 7;
    evidence.event_sequence = sequence++;
    evidence.monotonic_timestamp_ns = sequence;
    evidence.counter_delta = 1;
    evidence.closed_label_tuple =
        std::span<const dt::TimeMetricLabelMemberV3>(
            tuple.members.data(), tuple.member_count);
    evidence.outcome_committed = true;
    const auto admitted = dt::ValidateTimeMetricEvidenceV3(evidence, {7, 0});
    Require(admitted.disposition ==
                dt::TimeMetricEvidenceDispositionV3::admitted &&
                admitted.type == tuple.type &&
                admitted.receiving_owner_update_admitted &&
                admitted.datatype_outcome_unchanged &&
                admitted.idempotency.event_sequence == evidence.event_sequence,
            "all 245 exact closed metric tuples admitted as pure facts");
  }
  Require(observed_tuple_counts == tuple_counts,
          "all 21 exact closed metric domain cardinalities");

  const std::array<dt::TimeMetricLabelMemberV3, 1> admitted_label{{
      {"result", "admitted"}}};
  const std::array<dt::TimeMetricLabelMemberV3, 1> unknown_label{{
      {"result", "unknown"}}};
  const std::array<dt::TimeMetricLabelMemberV3, 1> undeclared_name{{
      {"undeclared", "admitted"}}};
  dt::TimeMetricEvidenceEnvelopeV3 valid_evidence;
  valid_evidence.envelope_identity = envelope_identity;
  valid_evidence.evidence_type_uuid = metrics[0].evidence_type_uuid;
  valid_evidence.source_event_uuid = metrics[0].source_event_uuid;
  valid_evidence.database_uuid = Instance(50);
  valid_evidence.node_uuid = Instance(51);
  valid_evidence.process_epoch = 7;
  valid_evidence.event_sequence = 1;
  valid_evidence.monotonic_timestamp_ns = 1;
  valid_evidence.counter_delta = 1;
  valid_evidence.closed_label_tuple = admitted_label;
  valid_evidence.outcome_committed = true;
  using MD = dt::TimeMetricEvidenceDispositionV3;
  const auto require_metric_refusal =
      [&](const dt::TimeMetricEvidenceEnvelopeV3& evidence,
          const dt::TimeMetricEvidenceValidationContextV3& context,
          MD expected, const char* message) {
        const auto refusal = dt::ValidateTimeMetricEvidenceV3(evidence, context);
        Require(refusal.disposition == expected &&
                    !refusal.receiving_owner_update_admitted &&
                    refusal.datatype_outcome_unchanged,
                message);
      };
  auto negative = valid_evidence;
  negative.all_required_fields_present = false;
  require_metric_refusal(negative, {7, 0}, MD::missing_field,
                         "metric missing-field refusal without mutation");
  negative = valid_evidence;
  negative.evidence_type_uuid.bytes[0] ^= 1;
  require_metric_refusal(negative, {7, 0}, MD::unknown_evidence_type,
                         "metric unknown evidence-type refusal without mutation");
  negative = valid_evidence;
  negative.closed_label_tuple = unknown_label;
  require_metric_refusal(negative, {7, 0}, MD::undeclared_label_tuple,
                         "metric undeclared tuple refusal without mutation");
  negative = valid_evidence;
  negative.closed_label_tuple = undeclared_name;
  require_metric_refusal(negative, {7, 0}, MD::undeclared_label_tuple,
                         "metric undeclared label-name refusal without mutation");
  const std::array<dt::TimeMetricLabelMemberV3, 2> duplicate_labels{{
      {"result", "admitted"}, {"result", "admitted"}}};
  negative = valid_evidence;
  negative.closed_label_tuple = duplicate_labels;
  require_metric_refusal(negative, {7, 0}, MD::undeclared_label_tuple,
                         "metric duplicate label-name refusal without mutation");
  negative = valid_evidence;
  negative.scalar_types_valid = false;
  require_metric_refusal(negative, {7, 0}, MD::wrong_scalar,
                         "metric wrong-scalar refusal without mutation");
  negative = valid_evidence;
  negative.outcome_committed = false;
  require_metric_refusal(negative, {7, 0}, MD::outcome_not_committed,
                         "metric uncommitted-outcome refusal without mutation");
  negative = valid_evidence;
  negative.counter_delta = 2;
  require_metric_refusal(negative, {7, 0}, MD::invalid_counter_delta,
                         "metric non-unit-delta refusal without mutation");
  negative = valid_evidence;
  negative.process_epoch = 0;
  require_metric_refusal(negative, {7, 0}, MD::missing_field,
                         "metric zero process epoch refusal without mutation");
  negative = valid_evidence;
  negative.event_sequence = 0;
  require_metric_refusal(negative, {7, 0}, MD::missing_field,
                         "metric zero event sequence refusal without mutation");
  negative = valid_evidence;
  negative.monotonic_timestamp_ns = 0;
  require_metric_refusal(
      negative, {7, 0}, MD::missing_field,
      "metric zero monotonic timestamp refusal without mutation");
  require_metric_refusal(valid_evidence, {8, 0}, MD::stale_process_epoch,
                         "metric stale-process-epoch refusal without mutation");
  require_metric_refusal(
      valid_evidence, {7, std::numeric_limits<platform::u64>::max()},
      MD::counter_overflow,
      "metric counter-overflow refusal without mutation");

  for (unsigned i = 0; i != metrics.size(); ++i) {
    const dt::TimeMetricEvidenceIdempotencyFactV3 first{
        7, 1, TimeAllocatedUuid(static_cast<platform::byte>(0x72 + i)),
        metrics[i].evidence_type_uuid};
    const dt::TimeMetricEvidenceIdempotencyFactV3 duplicate{
        7, 2, first.source_event_uuid, first.evidence_type_uuid};
    const auto refusal =
        dt::ClassifyTimeMetricSameSourceEmissionV3(first, duplicate);
    Require(refusal.disposition ==
                dt::TimeMetricSameSourceDispositionV3::duplicate_type_refused &&
                !refusal.second_emission_admitted &&
                !refusal.receiving_owner_update_admitted &&
                refusal.datatype_outcome_unchanged,
            "all 21 exactly-once duplicate refusals");
  }
  const auto same_source_case = [&](unsigned first_type, unsigned second_type,
                                    platform::byte source_octet) {
    const dt::TimeMetricEvidenceIdempotencyFactV3 first{
        7, 1, TimeAllocatedUuid(source_octet),
        metrics[first_type].evidence_type_uuid};
    const dt::TimeMetricEvidenceIdempotencyFactV3 second{
        7, 2, first.source_event_uuid, metrics[second_type].evidence_type_uuid};
    const auto admitted =
        dt::ClassifyTimeMetricSameSourceEmissionV3(first, second);
    Require(admitted.disposition ==
                dt::TimeMetricSameSourceDispositionV3::admitted_ordered_pair &&
                admitted.second_emission_admitted &&
                admitted.receiving_owner_update_admitted,
            "registered same-source ordered pair");
    const auto reversed =
        dt::ClassifyTimeMetricSameSourceEmissionV3(second, first);
    Require(reversed.disposition ==
                dt::TimeMetricSameSourceDispositionV3::reversed_order_refused &&
                !reversed.receiving_owner_update_admitted,
            "registered same-source reversed order refusal");
  };
  same_source_case(10, 11, 0x87);
  same_source_case(20, 18, 0x88);
  const dt::TimeMetricEvidenceIdempotencyFactV3 unrelated_first{
      7, 1, Instance(60), metrics[0].evidence_type_uuid};
  const dt::TimeMetricEvidenceIdempotencyFactV3 unrelated_second{
      7, 2, unrelated_first.source_event_uuid, metrics[1].evidence_type_uuid};
  const auto unregistered = dt::ClassifyTimeMetricSameSourceEmissionV3(
      unrelated_first, unrelated_second);
  Require(unregistered.disposition ==
              dt::TimeMetricSameSourceDispositionV3::unregistered_pair_refused &&
              !unregistered.receiving_owner_update_admitted &&
              unregistered.datatype_outcome_unchanged,
          "unregistered same-source pair refusal");
  auto stale_second = unrelated_second;
  stale_second.process_epoch = 8;
  const auto stale = dt::ClassifyTimeMetricSameSourceEmissionV3(
      unrelated_first, stale_second);
  Require(stale.disposition ==
              dt::TimeMetricSameSourceDispositionV3::stale_process_epoch_refused &&
              !stale.receiving_owner_update_admitted &&
              stale.datatype_outcome_unchanged,
          "same-source stale epoch refusal");
}
}  // namespace

int main() {
  try {
    TestIndexRegistryAndPredicates();
    const auto profile = Profile();
    TestProjectionCodecs(profile);
    TestStatisticsAndBackup(profile);
    TestProjectionHashMemoryAndScrubs(profile);
    TestOwnerAndObservabilityRegistries();
    std::cout << "base_time_projection_observability_test: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "base_time_projection_observability_test: FAIL: "
              << error.what() << '\n';
    return 1;
  }
}
