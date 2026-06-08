// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "gin_physical_provider.hpp"
#include "ngram_physical_provider.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "gin_ngram_physical_provider_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

void RequireNoRuntimeLeak(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    Require(item.find("docs" "/execution-plans") == std::string::npos &&
                item.find("public_release_evidence") == std::string::npos &&
                item.find("docs/reference") == std::string::npos &&
                item.find("IRC-") == std::string::npos &&
                item.find("IRC_") == std::string::npos &&
                item.find("execution_plan") == std::string::npos,
            "runtime evidence leaked planning/spec/reference artifact");
  }
}

std::string UuidWithSuffix(std::string prefix, std::uint64_t suffix) {
  std::ostringstream out;
  out << prefix << std::setw(12) << std::setfill('0') << suffix;
  return out.str();
}

idx::TextInvertedRowLocator Locator(std::uint64_t row) {
  idx::TextInvertedRowLocator locator;
  locator.row_ordinal = row;
  locator.row_uuid = UuidWithSuffix("aaaaaaaa-aaaa-7aaa-8aaa-", row);
  locator.version_uuid = UuidWithSuffix("bbbbbbbb-bbbb-7bbb-8bbb-", row);
  return locator;
}

std::vector<std::uint64_t> GinRows(
    const std::vector<idx::GinCandidate>& candidates) {
  std::vector<std::uint64_t> rows;
  for (const auto& candidate : candidates) {
    rows.push_back(candidate.locator.row_ordinal);
  }
  return rows;
}

std::vector<std::uint64_t> NgramRows(
    const std::vector<idx::NgramCandidate>& candidates) {
  std::vector<std::uint64_t> rows;
  for (const auto& candidate : candidates) {
    rows.push_back(candidate.locator.row_ordinal);
  }
  return rows;
}

idx::GinExactRecheckProof GinProof() {
  idx::GinExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_recheck_required = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "gin_exact_source_mga_security_recheck_contract";
  return proof;
}

idx::NgramExactRecheckProof NgramProof() {
  idx::NgramExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_batch_available = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "ngram_exact_source_batch_mga_security_recheck_contract";
  return proof;
}

idx::GinOpclassDescriptor GinOpclass() {
  idx::GinOpclassDescriptor opclass;
  opclass.opclass_name = "text_token_array_ops";
  opclass.opclass_epoch = 13;
  opclass.resource_epoch = 17;
  opclass.deterministic = true;
  opclass.immutable = true;
  opclass.safe = true;
  opclass.tri_consistent_supported = true;
  return opclass;
}

idx::NgramTokenizerDescriptor NgramTokenizer() {
  idx::NgramTokenizerDescriptor tokenizer;
  tokenizer.tokenizer_epoch = 19;
  tokenizer.charset_epoch = 23;
  tokenizer.resource_epoch = 29;
  tokenizer.deterministic = true;
  tokenizer.tokenizer_safe = true;
  tokenizer.charset_safe = true;
  tokenizer.unicode_boundary_safe = true;
  return tokenizer;
}

std::vector<std::string> SplitTokens(std::string value) {
  std::vector<std::string> tokens;
  std::string current;
  for (char ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (ch == ' ') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  std::sort(tokens.begin(), tokens.end());
  tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
  return tokens;
}

idx::GinOpclassExtractor GinExtractor() {
  return [](const idx::GinOpclassExtractorInput& input) {
    idx::GinOpclassExtractorOutput output;
    output.keys = SplitTokens(input.exact_source_value);
    output.deterministic = true;
    output.exact_source_recheck_evidence_present = true;
    output.evidence_ref =
        "gin_opclass_extractor_source_recheck_" +
        std::to_string(input.locator.row_ordinal);
    return output;
  };
}

std::vector<idx::GinSourceRow> GinRowsFixture() {
  return {{Locator(10), "alpha beta"},
          {Locator(20), "alpha gamma"},
          {Locator(30), "alpha beta"},
          {Locator(40), "alpha beta"},
          {Locator(50), "omega"}};
}

idx::GinPhysicalBuildRequest GinBuildRequest() {
  idx::GinPhysicalBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.opclass = GinOpclass();
  request.pending_flush_threshold = 2;
  request.posting_list_limit = 2;
  request.recheck_proof = GinProof();
  request.rows = GinRowsFixture();
  request.extractor = GinExtractor();
  return request;
}

std::vector<idx::NgramSourceRow> NgramRowsFixture() {
  return {{Locator(101), "alpha beta"},
          {Locator(102), "alphabet"},
          {Locator(103), "beta alpha"},
          {Locator(104), "gamma alphabetic"},
          {Locator(105), "alpine beta"}};
}

idx::NgramPhysicalBuildRequest NgramBuildRequest() {
  idx::NgramPhysicalBuildRequest request;
  request.relation_uuid = "44444444-4444-7444-8444-444444444444";
  request.index_uuid = "55555555-5555-7555-8555-555555555555";
  request.provider_uuid = "66666666-6666-7666-8666-666666666666";
  request.base_generation = 7;
  request.provider_generation = 12;
  request.tokenizer = NgramTokenizer();
  request.gram_width = 3;
  request.recheck_proof = NgramProof();
  request.rows = NgramRowsFixture();
  return request;
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<platform::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.flush();
  Require(static_cast<bool>(out), "could not write persistence fixture");
}

std::vector<platform::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "could not read persistence fixture");
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

void VerifyGinBuildQueryMaintenanceAndReopen() {
  const auto built = idx::BuildGinPhysicalProvider(GinBuildRequest());
  Require(built.ok(), "GIN provider build failed");
  Require(built.provider.pending_list_present &&
              built.provider.posting_lists_present &&
              built.provider.posting_trees_present &&
              built.provider.tri_consistent_executor_present,
          "GIN physical surfaces missing");
  Require(HasEvidence(built.provider.evidence, idx::kGinPhysicalProviderSearchKey),
          "GIN neutral runtime evidence missing");
  RequireNoRuntimeLeak(built.provider.evidence);

  idx::GinTriConsistentRequest query;
  query.provider = built.provider;
  query.strategy = idx::GinQueryStrategy::contains_all;
  query.query_keys = {"alpha", "beta"};
  query.recheck_proof = GinProof();
  const auto result = idx::ExecuteGinTriConsistent(query);
  Require(result.ok(), "GIN tri-consistent query failed");
  Require(result.tri_consistent_executor_used &&
              result.candidate_rows_only &&
              !result.final_rows_authorized &&
              !result.descriptor_store_scan &&
              !result.behavior_store_scan,
          "GIN runtime violated candidate-only contract");
  Require(result.posting_tree_probe_count > 0 &&
              result.posting_list_probe_count > 0 &&
              result.pending_list_probe_count > 0,
          "GIN query did not consume pending/list/tree surfaces");
  Require(GinRows(result.candidates) ==
              std::vector<std::uint64_t>({10, 30, 40}),
          "GIN tri-consistent rows changed");
  for (const auto& candidate : result.candidates) {
    Require(candidate.exact_source_recheck_required &&
                candidate.mga_recheck_required &&
                candidate.security_recheck_required &&
                !candidate.final_row_admitted &&
                !candidate.source_recheck_evidence_ref.empty(),
            "GIN candidate omitted recheck proof");
  }
  RequireNoRuntimeLeak(result.evidence);

  idx::GinPhysicalMutation insert;
  insert.kind = idx::GinMutationKind::insert_row;
  insert.after_row_present = true;
  insert.after_row = {Locator(60), "alpha beta"};
  insert.recheck_proof = GinProof();
  insert.extractor = GinExtractor();
  const auto inserted =
      idx::ApplyGinPhysicalMutation(built.provider, insert);
  Require(inserted.ok() && inserted.pending_flushed,
          "GIN insert did not apply and flush pending list");

  query.provider = inserted.provider;
  const auto after_insert = idx::ExecuteGinTriConsistent(query);
  Require(after_insert.ok() &&
              GinRows(after_insert.candidates) ==
                  std::vector<std::uint64_t>({10, 30, 40, 60}),
          "GIN insert maintenance was not visible as candidate evidence");

  idx::GinPhysicalMutation update;
  update.kind = idx::GinMutationKind::update_row;
  update.before_row_present = true;
  update.before_row = {Locator(60), "alpha beta"};
  update.after_row_present = true;
  update.after_row = {Locator(60), "theta beta"};
  update.recheck_proof = GinProof();
  update.extractor = GinExtractor();
  const auto updated =
      idx::ApplyGinPhysicalMutation(inserted.provider, update);
  Require(updated.ok(), "GIN update maintenance failed");
  query.provider = updated.provider;
  const auto after_update = idx::ExecuteGinTriConsistent(query);
  Require(after_update.ok() &&
              GinRows(after_update.candidates) ==
                  std::vector<std::uint64_t>({10, 30, 40}),
          "GIN update maintenance left stale key candidate");

  const auto serialized = idx::SerializeGinPhysicalProvider(updated.provider);
  Require(serialized.ok(), "GIN serialization failed");
  const auto serialized_again = idx::SerializeGinPhysicalProvider(updated.provider);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "GIN serialization is not deterministic");
  const auto path =
      std::filesystem::temp_directory_path() /
      "scratchbird_gin_physical_provider_gate.sbgin";
  WriteFile(path, serialized.bytes);
  const auto persisted = ReadFile(path);
  std::filesystem::remove(path);
  Require(persisted == serialized.bytes, "GIN persisted bytes changed");

  idx::GinPhysicalOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = updated.provider.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = updated.provider.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = updated.provider.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = updated.provider.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = updated.provider.provider_generation;
  open.expected_opclass_epoch_present = true;
  open.expected_opclass_epoch = updated.provider.opclass.opclass_epoch;
  open.expected_resource_epoch_present = true;
  open.expected_resource_epoch = updated.provider.opclass.resource_epoch;
  open.recheck_proof = GinProof();
  const auto opened = idx::OpenGinPhysicalProvider(open);
  Require(opened.ok(), "GIN clean reopen failed");
  const auto reserialized = idx::SerializeGinPhysicalProvider(opened.provider);
  Require(reserialized.ok() && reserialized.bytes == serialized.bytes,
          "GIN reopen serialization equivalence failed");
}

void VerifyGinFailClosedDiagnostics() {
  auto missing_proof = GinBuildRequest();
  missing_proof.recheck_proof = {};
  const auto missing = idx::BuildGinPhysicalProvider(missing_proof);
  Require(!missing.ok() &&
              missing.diagnostic.diagnostic_code ==
                  "INDEX.GIN_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
          "GIN missing proof did not fail closed");

  auto unsafe = GinBuildRequest();
  unsafe.opclass.safe = false;
  const auto unsafe_result = idx::BuildGinPhysicalProvider(unsafe);
  Require(!unsafe_result.ok(),
          "GIN unsafe opclass was accepted");

  auto built = idx::BuildGinPhysicalProvider(GinBuildRequest());
  Require(built.ok(), "GIN fixture build failed for refusals");
  idx::GinTriConsistentRequest fallback;
  fallback.provider = built.provider;
  fallback.query_keys = {"alpha"};
  fallback.recheck_proof = GinProof();
  fallback.contract_only_fallback = true;
  const auto fallback_result = idx::ExecuteGinTriConsistent(fallback);
  Require(!fallback_result.ok() &&
              fallback_result.diagnostic.diagnostic_code ==
                  "INDEX.GIN_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
          "GIN contract-only fallback did not fail closed");
  fallback.contract_only_fallback = false;
  fallback.provider_only_fallback = true;
  Require(!idx::ExecuteGinTriConsistent(fallback).ok(),
          "GIN provider-only fallback did not fail closed");
  fallback.provider_only_fallback = false;
  fallback.descriptor_store_scan = true;
  Require(!idx::ExecuteGinTriConsistent(fallback).ok(),
          "GIN descriptor scan fallback did not fail closed");
  fallback.descriptor_store_scan = false;
  fallback.behavior_store_scan = true;
  Require(!idx::ExecuteGinTriConsistent(fallback).ok(),
          "GIN behavior scan fallback did not fail closed");

  auto serialized = idx::SerializeGinPhysicalProvider(built.provider);
  Require(serialized.ok(), "GIN serialization failed for stale epoch");
  idx::GinPhysicalOpenRequest stale;
  stale.bytes = serialized.bytes;
  stale.expected_opclass_epoch_present = true;
  stale.expected_opclass_epoch = 999;
  stale.recheck_proof = GinProof();
  const auto stale_result = idx::OpenGinPhysicalProvider(stale);
  Require(!stale_result.ok() &&
              stale_result.open_class == idx::GinPhysicalOpenClass::stale_epoch,
          "GIN stale opclass epoch did not fail closed");

  auto authority = GinBuildRequest();
  authority.opclass.index_finality_authority_claimed = true;
  Require(!idx::BuildGinPhysicalProvider(authority).ok(),
          "GIN authority claim was accepted");
}

void VerifyNgramBuildQueryMaintenanceAndReopen() {
  const auto built = idx::BuildNgramPhysicalProvider(NgramBuildRequest());
  Require(built.ok(), "ngram provider build failed");
  Require(built.provider.qgram_extractor_present &&
              built.provider.qgram_postings_present &&
              built.provider.prefix_acceleration_present &&
              built.provider.suffix_acceleration_present &&
              built.provider.contains_acceleration_present &&
              built.provider.false_positive_tracking_present &&
              built.provider.exact_source_recheck_batching_present,
          "ngram physical surfaces missing");
  Require(HasEvidence(built.provider.evidence,
                      idx::kNgramPhysicalProviderSearchKey),
          "ngram neutral runtime evidence missing");
  RequireNoRuntimeLeak(built.provider.evidence);

  idx::NgramQueryRequest prefix;
  prefix.provider = built.provider;
  prefix.kind = idx::NgramQueryKind::prefix;
  prefix.pattern = "alpha";
  prefix.recheck_proof = NgramProof();
  const auto prefix_result = idx::QueryNgramPhysicalProvider(prefix);
  Require(prefix_result.ok(), "ngram prefix query failed");
  Require(prefix_result.prefix_acceleration_used &&
              prefix_result.exact_recheck_batch_count >= 2 &&
              prefix_result.false_positive_count > 0 &&
              prefix_result.candidate_rows_only &&
              !prefix_result.final_rows_authorized &&
              !prefix_result.descriptor_store_scan &&
              !prefix_result.behavior_store_scan,
          "ngram prefix acceleration/recheck evidence missing");
  Require(NgramRows(prefix_result.candidates) ==
              std::vector<std::uint64_t>({101, 102}),
          "ngram prefix rows changed");
  for (const auto& candidate : prefix_result.candidates) {
    Require(candidate.exact_match &&
                candidate.exact_source_recheck_required &&
                candidate.mga_recheck_required &&
                candidate.security_recheck_required &&
                !candidate.final_row_admitted &&
                !candidate.source_recheck_evidence_ref.empty(),
            "ngram candidate omitted exact source/MGA/security recheck");
  }

  idx::NgramQueryRequest suffix = prefix;
  suffix.kind = idx::NgramQueryKind::suffix;
  suffix.pattern = "alpha";
  const auto suffix_result = idx::QueryNgramPhysicalProvider(suffix);
  Require(suffix_result.ok() && suffix_result.suffix_acceleration_used &&
              NgramRows(suffix_result.candidates) ==
                  std::vector<std::uint64_t>({103}),
          "ngram suffix acceleration rows changed");

  idx::NgramQueryRequest contains = prefix;
  contains.kind = idx::NgramQueryKind::contains;
  contains.pattern = "bet";
  const auto contains_result = idx::QueryNgramPhysicalProvider(contains);
  Require(contains_result.ok() &&
              contains_result.contains_acceleration_used &&
              NgramRows(contains_result.candidates) ==
                  std::vector<std::uint64_t>({101, 102, 103, 104, 105}),
          "ngram contains acceleration rows changed");
  RequireNoRuntimeLeak(contains_result.evidence);

  idx::NgramPhysicalMutation insert;
  insert.kind = idx::NgramMutationKind::insert_row;
  insert.after_row_present = true;
  insert.after_row = {Locator(106), "alpha centauri"};
  insert.recheck_proof = NgramProof();
  const auto inserted =
      idx::ApplyNgramPhysicalMutation(built.provider, insert);
  Require(inserted.ok(), "ngram insert maintenance failed");
  prefix.provider = inserted.provider;
  Require(NgramRows(idx::QueryNgramPhysicalProvider(prefix).candidates) ==
              std::vector<std::uint64_t>({101, 102, 106}),
          "ngram insert maintenance candidate rows changed");

  idx::NgramPhysicalMutation update;
  update.kind = idx::NgramMutationKind::update_row;
  update.before_row_present = true;
  update.before_row = {Locator(106), "alpha centauri"};
  update.after_row_present = true;
  update.after_row = {Locator(106), "omega centauri"};
  update.recheck_proof = NgramProof();
  const auto updated =
      idx::ApplyNgramPhysicalMutation(inserted.provider, update);
  Require(updated.ok(), "ngram update maintenance failed");
  prefix.provider = updated.provider;
  Require(NgramRows(idx::QueryNgramPhysicalProvider(prefix).candidates) ==
              std::vector<std::uint64_t>({101, 102}),
          "ngram update maintenance left stale prefix candidate");

  const auto serialized = idx::SerializeNgramPhysicalProvider(updated.provider);
  Require(serialized.ok(), "ngram serialization failed");
  auto mismatched_postings = updated.provider;
  Require(!mismatched_postings.postings.empty(),
          "ngram fixture did not build persisted postings");
  mismatched_postings.postings.pop_back();
  Require(!idx::SerializeNgramPhysicalProvider(mismatched_postings).ok(),
          "ngram serialization accepted source/posting mismatch");
  const auto serialized_again =
      idx::SerializeNgramPhysicalProvider(updated.provider);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "ngram serialization is not deterministic");
  const auto path =
      std::filesystem::temp_directory_path() /
      "scratchbird_ngram_physical_provider_gate.sbngr";
  WriteFile(path, serialized.bytes);
  const auto persisted = ReadFile(path);
  std::filesystem::remove(path);
  Require(persisted == serialized.bytes, "ngram persisted bytes changed");

  idx::NgramPhysicalOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = updated.provider.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = updated.provider.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = updated.provider.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = updated.provider.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = updated.provider.provider_generation;
  open.expected_tokenizer_epoch_present = true;
  open.expected_tokenizer_epoch = updated.provider.tokenizer.tokenizer_epoch;
  open.expected_charset_epoch_present = true;
  open.expected_charset_epoch = updated.provider.tokenizer.charset_epoch;
  open.expected_resource_epoch_present = true;
  open.expected_resource_epoch = updated.provider.tokenizer.resource_epoch;
  open.recheck_proof = NgramProof();
  const auto opened = idx::OpenNgramPhysicalProvider(open);
  Require(opened.ok(), "ngram clean reopen failed");
  const auto reserialized =
      idx::SerializeNgramPhysicalProvider(opened.provider);
  Require(reserialized.ok() && reserialized.bytes == serialized.bytes,
          "ngram reopen serialization equivalence failed");
}

void VerifyNgramFailClosedDiagnostics() {
  auto missing = NgramBuildRequest();
  missing.recheck_proof = {};
  Require(!idx::BuildNgramPhysicalProvider(missing).ok(),
          "ngram missing exact recheck was accepted");

  auto unsafe = NgramBuildRequest();
  unsafe.tokenizer.charset_safe = false;
  Require(!idx::BuildNgramPhysicalProvider(unsafe).ok(),
          "ngram unsafe charset was accepted");

  auto invalid_width = NgramBuildRequest();
  invalid_width.gram_width = 0;
  const auto invalid = idx::BuildNgramPhysicalProvider(invalid_width);
  Require(!invalid.ok() &&
              invalid.diagnostic.diagnostic_code ==
                  "INDEX.NGRAM_PHYSICAL_PROVIDER.INVALID_GRAM_WIDTH",
          "ngram invalid gram width did not fail closed");

  const auto built = idx::BuildNgramPhysicalProvider(NgramBuildRequest());
  Require(built.ok(), "ngram fixture build failed for refusals");
  idx::NgramQueryRequest query;
  query.provider = built.provider;
  query.kind = idx::NgramQueryKind::contains;
  query.pattern = "alpha";
  query.recheck_proof = NgramProof();
  query.descriptor_store_scan = true;
  Require(!idx::QueryNgramPhysicalProvider(query).ok(),
          "ngram descriptor scan fallback did not fail closed");
  query.descriptor_store_scan = false;
  query.behavior_store_scan = true;
  Require(!idx::QueryNgramPhysicalProvider(query).ok(),
          "ngram behavior scan fallback did not fail closed");
  query.behavior_store_scan = false;
  query.tokenizer_epoch_current = false;
  Require(!idx::QueryNgramPhysicalProvider(query).ok(),
          "ngram stale tokenizer epoch did not fail closed");
  query.tokenizer_epoch_current = true;
  query.recheck_proof = {};
  Require(!idx::QueryNgramPhysicalProvider(query).ok(),
          "ngram missing runtime exact recheck did not fail closed");

  auto authority = NgramBuildRequest();
  authority.tokenizer.index_finality_authority_claimed = true;
  Require(!idx::BuildNgramPhysicalProvider(authority).ok(),
          "ngram authority claim was accepted");

  const auto serialized = idx::SerializeNgramPhysicalProvider(built.provider);
  Require(serialized.ok(), "ngram serialization failed for stale epoch");
  idx::NgramPhysicalOpenRequest stale;
  stale.bytes = serialized.bytes;
  stale.expected_resource_epoch_present = true;
  stale.expected_resource_epoch = 999;
  stale.recheck_proof = NgramProof();
  const auto stale_result = idx::OpenNgramPhysicalProvider(stale);
  Require(!stale_result.ok() &&
              stale_result.open_class ==
                  idx::NgramPhysicalOpenClass::stale_epoch,
          "ngram stale resource epoch did not fail closed");
}

}  // namespace

int main() {
  VerifyGinBuildQueryMaintenanceAndReopen();
  VerifyGinFailClosedDiagnostics();
  VerifyNgramBuildQueryMaintenanceAndReopen();
  VerifyNgramFailClosedDiagnostics();
  std::cout << "gin_ngram_physical_provider_gate=passed\n";
  return EXIT_SUCCESS;
}
