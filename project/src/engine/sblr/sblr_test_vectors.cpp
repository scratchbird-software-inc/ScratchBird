// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_test_vectors.hpp"

namespace scratchbird::engine::sblr {

std::vector<SblrExecutorTestVector> BuiltInSblrExecutorTestVectors() {
  return {
      {"SEFI-053", "negative.cluster_hook_refusal", "negative_refusal", "sblr.cluster.hook",
       "SB_DIAG_CLUSTER_TXN_UNAVAILABLE", false},
      {"SEFI-054", "decoder.raw_sql_refusal", "decoder", "sblr.envelope.decode", "SB_SBLR_RAW_SQL_FORBIDDEN", false},
      {"SEFI-055", "operator.boolean_unknown", "expression", "operator.boolean.and", "", false},
      {"SEFI-056", "function.lower", "function_family", "data.scalar.lower", "", false},
      {"SEFI-057", "seed.function_uuid_stable", "seed_stability", "data.scalar.lower", "", false},
      {"SEFI-058", "no_orphan.function_registry", "no_orphan_reference", "sblr.function.dispatch", "", false},
      {"SEFI-059", "parser_authority.raw_sql_refusal", "parser_authority", "sblr.envelope.decode",
       "SB_DIAG_PARSER_AUTHORITY_REFUSED", false},
  };
}

}  // namespace scratchbird::engine::sblr
