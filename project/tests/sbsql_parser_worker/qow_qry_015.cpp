// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/plan_api.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-015-V1: " << detail << '\n';
  }
  return condition;
}

api::CanonicalSeededSampleRequest BernoulliRequest() {
  api::CanonicalSeededSampleRequest request;
  request.input_row_count = 32;
  request.method = api::CanonicalSeededSampleMethod::kBernoulli;
  request.sample_basis_points = 5000;
  request.repeatable_seed = 42;
  request.repeatable_seed_is_bound = true;
  request.maximum_input_row_count = 64;
  return request;
}

// QOW-TEST-QRY-015-V1
bool ValidateSeededSampling() {
  bool passed = true;
  const auto bernoulli = api::ExecuteCanonicalSeededSample(BernoulliRequest());
  const auto repeated = api::ExecuteCanonicalSeededSample(BernoulliRequest());
  passed &= Require(
      bernoulli.accepted &&
          bernoulli.method_id == "bernoulli.seeded-row-hash.v1" &&
          bernoulli.examined_unit_count == 32 &&
          bernoulli.selected_row_indices == repeated.selected_row_indices &&
          !bernoulli.selected_row_indices.empty() &&
          bernoulli.selected_row_indices.size() < 32,
      "seeded BERNOULLI sample was not stable or bounded");

  bool observed_gap_then_row = false;
  for (std::size_t index = 1;
       index < bernoulli.selected_row_indices.size(); ++index) {
    if (bernoulli.selected_row_indices[index] >
        bernoulli.selected_row_indices[index - 1] + 1) {
      observed_gap_then_row = true;
      break;
    }
  }
  passed &= Require(observed_gap_then_row,
                    "BERNOULLI route collapsed to prefix sampling");

  auto request = BernoulliRequest();
  request.repeatable_seed = 43;
  const auto other_seed = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(other_seed.accepted &&
                        other_seed.selected_row_indices !=
                            bernoulli.selected_row_indices,
                    "sample seed did not influence row admission");

  request = BernoulliRequest();
  request.input_row_count = 1;
  request.sample_basis_points = 1;
  request.repeatable_seed = 0;
  auto result = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(result.accepted && result.selected_row_indices.empty(),
                    "nonzero sample rate forced one input row");

  request = BernoulliRequest();
  request.sample_basis_points = 0;
  result = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(result.accepted && result.selected_row_indices.empty(),
                    "zero-percent sample retained rows");

  request = BernoulliRequest();
  request.sample_basis_points = 10000;
  result = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(result.accepted && result.selected_row_indices.size() == 32 &&
                        result.selected_row_indices.front() == 0 &&
                        result.selected_row_indices.back() == 31,
                    "full sample did not retain all rows in order");

  request = BernoulliRequest();
  request.method = api::CanonicalSeededSampleMethod::kSystem;
  request.system_block_row_count = 4;
  const auto system = api::ExecuteCanonicalSeededSample(request);
  bool complete_blocks = system.accepted;
  for (std::size_t block = 0; block < 8; ++block) {
    std::size_t selected_in_block = 0;
    for (std::size_t row = block * 4; row < block * 4 + 4; ++row) {
      selected_in_block += static_cast<std::size_t>(
          std::find(system.selected_row_indices.begin(),
                    system.selected_row_indices.end(), row) !=
          system.selected_row_indices.end());
    }
    complete_blocks &= selected_in_block == 0 || selected_in_block == 4;
  }
  passed &= Require(
      complete_blocks && system.examined_unit_count == 8 &&
          system.method_id == "system.seeded-block-hash.v1" &&
          system.selected_row_indices != bernoulli.selected_row_indices,
      "SYSTEM sample did not retain complete method-specific blocks");

  request = BernoulliRequest();
  request.repeatable_seed_is_bound = false;
  result = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(!result.accepted && result.selected_row_indices.empty(),
                    "unseeded sample was accepted");

  request = BernoulliRequest();
  request.method = api::CanonicalSeededSampleMethod::kSystem;
  request.system_block_row_count = 0;
  result = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(!result.accepted,
                    "SYSTEM sample accepted an unbound block size");

  request = BernoulliRequest();
  request.sample_basis_points = 10001;
  result = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(!result.accepted,
                    "sample rate above one hundred percent was accepted");

  request = BernoulliRequest();
  request.maximum_input_row_count = 31;
  result = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(!result.accepted && result.selected_row_indices.empty(),
                    "sample input resource bound was exceeded");

  request = BernoulliRequest();
  request.input_row_count = 0;
  result = api::ExecuteCanonicalSeededSample(request);
  passed &= Require(result.accepted && result.selected_row_indices.empty() &&
                        result.examined_unit_count == 0,
                    "empty sample input invented a row or unit");
  return passed;
}

}  // namespace

int main() { return ValidateSeededSampling() ? EXIT_SUCCESS : EXIT_FAILURE; }
