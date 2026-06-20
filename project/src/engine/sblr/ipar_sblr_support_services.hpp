// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct IparDispatchEntry {
  std::string opcode;
  std::string type_signature;
  std::string handler_id;
  bool engine_sblr_only = true;
  bool parser_execution_authority = false;
};

class IparDispatchTable {
 public:
  bool Register(IparDispatchEntry entry);
  const IparDispatchEntry* Resolve(const std::string& opcode,
                                   const std::string& type_signature) const;
  std::size_t size() const { return entries_.size(); }

 private:
  std::map<std::string, IparDispatchEntry> entries_;
};

struct IparExpressionCacheKey {
  std::string expression_digest;
  std::string volatility_class;
  std::uint64_t statement_epoch = 0;
  std::uint64_t transaction_epoch = 0;
  bool security_sensitive = false;

  bool operator<(const IparExpressionCacheKey& other) const;
};

struct IparExpressionCacheEntry {
  IparExpressionCacheKey key;
  std::string encoded_value;
};

class IparDeterministicExpressionCache {
 public:
  bool Put(IparExpressionCacheEntry entry);
  bool Lookup(const IparExpressionCacheKey& key, std::string* encoded_value) const;

 private:
  std::map<IparExpressionCacheKey, std::string> entries_;
};

struct IparValidationColumn {
  std::string column_uuid;
  std::string validation_kind;
  bool fixed_width = true;
};

struct IparValidationVectorRequest {
  std::vector<IparValidationColumn> columns;
  std::uint64_t row_count = 0;
  std::uint64_t group_size = 0;
};

struct IparValidationVectorPlan {
  bool accepted = false;
  std::uint64_t vector_groups = 0;
  bool exact_row_diagnostics = true;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

IparValidationVectorPlan PlanIparValidationVectorization(
    const IparValidationVectorRequest& request);

}  // namespace scratchbird::engine::sblr
