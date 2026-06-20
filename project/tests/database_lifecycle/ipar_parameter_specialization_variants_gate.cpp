// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_protocol_support.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using scratchbird::server::IparCacheStatus;
using scratchbird::server::IparParameterBatchSizeClass;
using scratchbird::server::IparParameterKeyDistributionClass;
using scratchbird::server::IparParameterLargeValueClass;
using scratchbird::server::IparParameterNullabilityClass;
using scratchbird::server::IparParameterWidthClass;
using scratchbird::server::IparPreparedParameterSpecializationProfile;
using scratchbird::server::IparPreparedSpecializedVariant;
using scratchbird::server::IparServerProtocolSupport;
using scratchbird::server::IparSupportScopeForSession;
using scratchbird::server::IparSupportSessionScope;
using scratchbird::server::IparUuidDependency;
using scratchbird::server::ServerSessionRecord;

void Require(bool condition, const std::string& label) {
  if (!condition) {
    std::cerr << "FAILED: " << label << '\n';
    std::exit(1);
  }
}

void RequireAccepted(const IparCacheStatus& status, const std::string& label) {
  Require(status.accepted, label + " accepted detail=" + status.detail);
}

void RequireRejectedDetail(const IparCacheStatus& status,
                           const std::string& detail,
                           const std::string& label) {
  Require(!status.accepted, label + " rejected");
  Require(status.detail == detail,
          label + " detail expected=" + detail + " actual=" + status.detail);
}

std::array<std::uint8_t, 16> UuidBytes(std::uint8_t seed) {
  return {0x01, 0x9f, 0x41, seed, 0x00, 0x00, 0x70, 0x00,
          0x80, 0x00, 0x00, 0x00, 0x00, seed, seed, seed};
}

std::string UuidText(unsigned int suffix) {
  std::ostringstream out;
  out << "019f4100-0000-7000-8000-" << std::setw(12)
      << std::setfill('0') << suffix;
  return out.str();
}

ServerSessionRecord MakeSession(std::uint8_t seed) {
  ServerSessionRecord session;
  session.session_uuid = UuidBytes(seed);
  session.auth_context_uuid = UuidBytes(static_cast<std::uint8_t>(seed + 10));
  session.principal_uuid = UuidBytes(static_cast<std::uint8_t>(seed + 20));
  session.effective_user_uuid = UuidBytes(static_cast<std::uint8_t>(seed + 30));
  session.database_uuid = UuidText(900 + seed);
  session.catalog_generation = 110;
  session.security_epoch = 210;
  session.descriptor_epoch = 310;
  session.grant_epoch = 410;
  session.policy_generation = 510;
  session.capability_policy_generation = 610;
  session.cache_invalidation_epoch = 710;
  session.name_resolution_epoch = 810;
  session.resource_epoch = 910;
  session.role_set_hash = "sha256:roles-ipar-p1-19";
  session.group_set_hash = "sha256:groups-ipar-p1-19";
  session.search_path_hash = "sha256:search-ipar-p1-19";
  return session;
}

IparUuidDependency DependencyFor(const IparSupportSessionScope& scope) {
  IparUuidDependency dependency;
  dependency.dependency_kind = "relation";
  dependency.logical_name = "orders";
  dependency.object_uuid = UuidText(101);
  dependency.descriptor_uuid = UuidText(201);
  dependency.descriptor_hash = "sha256:orders-descriptor";
  dependency.catalog_generation = scope.epoch.catalog_generation;
  dependency.descriptor_epoch = scope.epoch.descriptor_epoch;
  dependency.name_resolution_epoch = scope.epoch.name_resolution_epoch;
  return dependency;
}

scratchbird::server::IparPreparedTemplatePut MakePreparedPut(
    const IparSupportSessionScope& scope) {
  scratchbird::server::IparPreparedTemplatePut put;
  put.scope = scope;
  put.operation_id = "dml.update_rows";
  put.operation_family = "sblr.dml.update.v3";
  put.result_descriptor_hash = "sha256:update-result-shape";
  put.dependencies.push_back(DependencyFor(scope));
  put.canonical_sblr_envelope =
      "envelope=SBLRExecutionEnvelope.v3\n"
      "operation_id=dml.update_rows\n"
      "operation_family=sblr.dml.update.v3\n"
      "target_object_uuid=" +
      put.dependencies.front().object_uuid +
      "\nparameter_slot=$1\nparameter_slot=$2\n"
      "parser_resolved_names_to_uuids=true\n";
  return put;
}

scratchbird::server::IparPreparedTemplateLookup MakeLookup(
    const IparSupportSessionScope& scope,
    const std::string& cache_key,
    const scratchbird::server::IparPreparedTemplatePut& put,
    IparPreparedParameterSpecializationProfile profile) {
  scratchbird::server::IparPreparedTemplateLookup lookup;
  lookup.scope = scope;
  lookup.cache_key = cache_key;
  lookup.operation_id = put.operation_id;
  lookup.operation_family = put.operation_family;
  lookup.canonical_sblr_envelope = put.canonical_sblr_envelope;
  lookup.result_descriptor_hash = put.result_descriptor_hash;
  lookup.dependencies = put.dependencies;
  lookup.observed_parameter_profile = profile;
  return lookup;
}

IparPreparedParameterSpecializationProfile Profile(
    IparParameterNullabilityClass nullability,
    IparParameterWidthClass width,
    IparParameterLargeValueClass large_value,
    IparParameterKeyDistributionClass key_distribution,
    IparParameterBatchSizeClass batch_size,
    std::uint64_t rows) {
  IparPreparedParameterSpecializationProfile profile;
  profile.nullability_class = nullability;
  profile.width_class = width;
  profile.large_value_class = large_value;
  profile.key_distribution_class = key_distribution;
  profile.batch_size_class = batch_size;
  profile.observed_batch_rows = rows;
  return profile;
}

IparPreparedSpecializedVariant Variant(
    std::string id,
    std::string path,
    IparPreparedParameterSpecializationProfile profile,
    const std::string& statement_semantics_hash,
    const std::string& authorization_context_hash) {
  IparPreparedSpecializedVariant variant;
  variant.variant_id = std::move(id);
  variant.fast_path_label = std::move(path);
  variant.profile = profile;
  variant.statement_semantics_hash = statement_semantics_hash;
  variant.authorization_context_hash = authorization_context_hash;
  return variant;
}

void RequireSelected(const IparCacheStatus& status,
                     const std::string& expected_id,
                     const std::string& expected_path) {
  RequireAccepted(status, "specialized lookup " + expected_id);
  Require(status.cache_hit, "specialized lookup cache hit " + expected_id);
  Require(status.selected_specialized_variant,
          "selected specialized variant " + expected_id);
  Require(status.selected_variant_id == expected_id,
          "selected variant id " + expected_id);
  Require(status.selected_variant_path == expected_path,
          "selected variant path " + expected_path);
}

void ProveParameterSpecializationVariants() {
  IparServerProtocolSupport support;
  const IparSupportSessionScope scope =
      IparSupportScopeForSession(MakeSession(1));

  auto put = MakePreparedPut(scope);
  const IparCacheStatus baseline = support.StorePreparedTemplate(put);
  RequireAccepted(baseline, "baseline template stored");
  const auto& baseline_record = support.prepared_templates().at(baseline.cache_key);
  Require(!baseline_record.statement_semantics_hash.empty(),
          "baseline statement semantics hash present");
  Require(!baseline_record.authorization_context_hash.empty(),
          "baseline authorization context hash present");

  const auto narrow_single =
      Profile(IparParameterNullabilityClass::kAllNonNull,
              IparParameterWidthClass::kNarrow,
              IparParameterLargeValueClass::kInlineOnly,
              IparParameterKeyDistributionClass::kPointStable,
              IparParameterBatchSizeClass::kSingleRow,
              1);
  const auto wide_bulk =
      Profile(IparParameterNullabilityClass::kNullableDense,
              IparParameterWidthClass::kWide,
              IparParameterLargeValueClass::kLargeValueRefs,
              IparParameterKeyDistributionClass::kHotKeySkew,
              IparParameterBatchSizeClass::kBulkBatch,
              4096);
  const auto mixed_large =
      Profile(IparParameterNullabilityClass::kNullableSparse,
              IparParameterWidthClass::kMixed,
              IparParameterLargeValueClass::kMixedInlineAndLarge,
              IparParameterKeyDistributionClass::kRangeClustered,
              IparParameterBatchSizeClass::kLargeBatch,
              512);
  const auto medium_hash =
      Profile(IparParameterNullabilityClass::kAllNonNull,
              IparParameterWidthClass::kMedium,
              IparParameterLargeValueClass::kInlineOnly,
              IparParameterKeyDistributionClass::kHashDistributed,
              IparParameterBatchSizeClass::kMediumBatch,
              128);

  put.specialized_variants.push_back(
      Variant("param.narrow.inline.point.single",
              "fast_variant.nullability=all_non_null.width=narrow.large=inline.key=point.batch=single",
              narrow_single,
              baseline_record.statement_semantics_hash,
              baseline_record.authorization_context_hash));
  put.specialized_variants.push_back(
      Variant("param.wide.large.hot.bulk",
              "fast_variant.nullability=nullable_dense.width=wide.large=large_value_refs.key=hot.batch=bulk",
              wide_bulk,
              baseline_record.statement_semantics_hash,
              baseline_record.authorization_context_hash));
  put.specialized_variants.push_back(
      Variant("param.mixed.large.range.large_batch",
              "fast_variant.nullability=nullable_sparse.width=mixed.large=mixed.key=range.batch=large",
              mixed_large,
              baseline_record.statement_semantics_hash,
              baseline_record.authorization_context_hash));
  put.specialized_variants.push_back(
      Variant("param.medium.inline.hash.medium_batch",
              "fast_variant.nullability=all_non_null.width=medium.large=inline.key=hash.batch=medium",
              medium_hash,
              baseline_record.statement_semantics_hash,
              baseline_record.authorization_context_hash));

  const IparCacheStatus stored = support.StorePreparedTemplate(put);
  RequireAccepted(stored, "specialized template stored");
  Require(stored.cache_key == baseline.cache_key,
          "specialized variants do not change statement cache key");

  const auto& record = support.prepared_templates().at(stored.cache_key);
  Require(record.specialized_variants.size() == 4,
          "stored four parameter specialization variants");
  for (const auto& variant : record.specialized_variants) {
    Require(variant.statement_semantics_hash == record.statement_semantics_hash,
            "variant keeps statement semantics hash");
    Require(variant.authorization_context_hash == record.authorization_context_hash,
            "variant keeps authorization context hash");
    Require(!variant.authority_flags.client_or_parser_authorization_authority,
            "variant does not grant authorization authority");
    Require(!variant.authority_flags.transaction_finality_authority,
            "variant does not grant finality authority");
    Require(!variant.authority_flags.visibility_authority,
            "variant does not grant visibility authority");
    Require(!variant.authority_flags.grants_authority,
            "variant does not grant authority");
  }

  RequireSelected(support.LookupPreparedTemplate(
                      MakeLookup(scope, stored.cache_key, put, narrow_single)),
                  "param.narrow.inline.point.single",
                  "fast_variant.nullability=all_non_null.width=narrow.large=inline.key=point.batch=single");
  RequireSelected(support.LookupPreparedTemplate(
                      MakeLookup(scope, stored.cache_key, put, wide_bulk)),
                  "param.wide.large.hot.bulk",
                  "fast_variant.nullability=nullable_dense.width=wide.large=large_value_refs.key=hot.batch=bulk");
  RequireSelected(support.LookupPreparedTemplate(
                      MakeLookup(scope, stored.cache_key, put, mixed_large)),
                  "param.mixed.large.range.large_batch",
                  "fast_variant.nullability=nullable_sparse.width=mixed.large=mixed.key=range.batch=large");
  RequireSelected(support.LookupPreparedTemplate(
                      MakeLookup(scope, stored.cache_key, put, medium_hash)),
                  "param.medium.inline.hash.medium_batch",
                  "fast_variant.nullability=all_non_null.width=medium.large=inline.key=hash.batch=medium");

  const auto unmatched =
      Profile(IparParameterNullabilityClass::kAllNull,
              IparParameterWidthClass::kWide,
              IparParameterLargeValueClass::kLargeValueRefs,
              IparParameterKeyDistributionClass::kHashDistributed,
              IparParameterBatchSizeClass::kSmallBatch,
              8);
  const IparCacheStatus generic =
      support.LookupPreparedTemplate(MakeLookup(scope, stored.cache_key, put, unmatched));
  RequireAccepted(generic, "unmatched profile remains semantic template hit");
  Require(generic.cache_hit, "unmatched profile cache hit");
  Require(!generic.selected_specialized_variant,
          "unmatched profile does not select a fast variant");
  Require(generic.detail == "ipar_template_cache_hit",
          "unmatched profile uses generic prepared path");

  auto stale_role_lookup = MakeLookup(scope, stored.cache_key, put, narrow_single);
  stale_role_lookup.scope.epoch.role_set_hash = "sha256:roles-changed";
  RequireRejectedDetail(support.LookupPreparedTemplate(stale_role_lookup),
                        "ipar_cache_authorization_hash_stale",
                        "specialized variant authorization invalidation");

  const auto metrics = support.metrics();
  Require(metrics.prepared_specialized_variant_hits == 4,
          "specialized variant hit metric");
  Require(metrics.prepared_specialized_variant_misses == 1,
          "specialized variant miss metric");
}

void ProveVariantAuthorityBoundaries() {
  IparServerProtocolSupport support;
  const IparSupportSessionScope scope =
      IparSupportScopeForSession(MakeSession(3));
  auto put = MakePreparedPut(scope);
  const IparCacheStatus baseline = support.StorePreparedTemplate(put);
  RequireAccepted(baseline, "boundary baseline template stored");
  const auto& baseline_record = support.prepared_templates().at(baseline.cache_key);
  const auto profile =
      Profile(IparParameterNullabilityClass::kAllNonNull,
              IparParameterWidthClass::kNarrow,
              IparParameterLargeValueClass::kInlineOnly,
              IparParameterKeyDistributionClass::kPointStable,
              IparParameterBatchSizeClass::kSingleRow,
              1);

  auto changed_semantics = put;
  changed_semantics.specialized_variants.push_back(
      Variant("param.changed-semantics",
              "fast_variant.invalid.changed_semantics",
              profile,
              "ipar.statement_semantics:changed",
              baseline_record.authorization_context_hash));
  RequireRejectedDetail(support.StorePreparedTemplate(changed_semantics),
                        "ipar_template_variant_semantics_change_forbidden",
                        "variant semantics change refused");

  auto changed_authorization = put;
  changed_authorization.specialized_variants.push_back(
      Variant("param.changed-authorization",
              "fast_variant.invalid.changed_authorization",
              profile,
              baseline_record.statement_semantics_hash,
              "ipar.authorization_context:changed"));
  RequireRejectedDetail(support.StorePreparedTemplate(changed_authorization),
                        "ipar_template_variant_authorization_change_forbidden",
                        "variant authorization change refused");

  auto finality_authority = put;
  auto unsafe = Variant("param.finality-authority",
                        "fast_variant.invalid.finality_authority",
                        profile,
                        baseline_record.statement_semantics_hash,
                        baseline_record.authorization_context_hash);
  unsafe.authority_flags.transaction_finality_authority = true;
  finality_authority.specialized_variants.push_back(unsafe);
  RequireRejectedDetail(support.StorePreparedTemplate(finality_authority),
                        "ipar_cache_finality_authority_forbidden",
                        "variant finality authority refused");

  auto parser_execution = put;
  unsafe = Variant("param.parser-executes-sql",
                   "fast_variant.invalid.parser_executes_sql",
                   profile,
                   baseline_record.statement_semantics_hash,
                   baseline_record.authorization_context_hash);
  unsafe.parser_or_driver_executes_sql = true;
  parser_execution.specialized_variants.push_back(unsafe);
  RequireRejectedDetail(support.StorePreparedTemplate(parser_execution),
                        "ipar_template_variant_sql_execution_forbidden",
                        "variant parser SQL execution refused");
}

}  // namespace

int main() {
  ProveParameterSpecializationVariants();
  ProveVariantAuthorityBoundaries();
  std::cout << "ipar_parameter_specialization_variants_gate=passed\n";
  return 0;
}
