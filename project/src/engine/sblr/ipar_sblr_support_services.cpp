// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_sblr_support_services.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

std::string DispatchKey(const std::string& opcode,
                        const std::string& type_signature) {
  return opcode + "|" + type_signature;
}

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

}  // namespace

bool IparDispatchTable::Register(IparDispatchEntry entry) {
  if (entry.opcode.empty() || entry.type_signature.empty() ||
      entry.handler_id.empty() || !entry.engine_sblr_only ||
      entry.parser_execution_authority) {
    return false;
  }
  entries_[DispatchKey(entry.opcode, entry.type_signature)] = std::move(entry);
  return true;
}

const IparDispatchEntry* IparDispatchTable::Resolve(
    const std::string& opcode,
    const std::string& type_signature) const {
  const auto found = entries_.find(DispatchKey(opcode, type_signature));
  return found == entries_.end() ? nullptr : &found->second;
}

bool IparExpressionCacheKey::operator<(
    const IparExpressionCacheKey& other) const {
  if (expression_digest != other.expression_digest) {
    return expression_digest < other.expression_digest;
  }
  if (volatility_class != other.volatility_class) {
    return volatility_class < other.volatility_class;
  }
  if (statement_epoch != other.statement_epoch) {
    return statement_epoch < other.statement_epoch;
  }
  if (transaction_epoch != other.transaction_epoch) {
    return transaction_epoch < other.transaction_epoch;
  }
  return security_sensitive < other.security_sensitive;
}

bool IparDeterministicExpressionCache::Put(IparExpressionCacheEntry entry) {
  if (entry.key.expression_digest.empty() || entry.encoded_value.empty() ||
      entry.key.statement_epoch == 0 || entry.key.transaction_epoch == 0 ||
      entry.key.volatility_class != "deterministic" ||
      entry.key.security_sensitive) {
    return false;
  }
  entries_[entry.key] = std::move(entry.encoded_value);
  return true;
}

bool IparDeterministicExpressionCache::Lookup(
    const IparExpressionCacheKey& key,
    std::string* encoded_value) const {
  if (key.volatility_class != "deterministic" || key.security_sensitive) {
    return false;
  }
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return false;
  }
  if (encoded_value != nullptr) {
    *encoded_value = found->second;
  }
  return true;
}

IparValidationVectorPlan PlanIparValidationVectorization(
    const IparValidationVectorRequest& request) {
  IparValidationVectorPlan plan;
  Add(&plan.evidence, "IPAR-P4-16");
  if (request.columns.empty() || request.row_count == 0 ||
      request.group_size == 0) {
    plan.diagnostic_code = "IPAR_VALIDATION_VECTOR_REQUEST_INCOMPLETE";
    return plan;
  }
  for (const auto& column : request.columns) {
    if (column.column_uuid.empty() || column.validation_kind.empty() ||
        !column.fixed_width) {
      plan.diagnostic_code = "IPAR_VALIDATION_VECTOR_UNSUPPORTED_COLUMN";
      return plan;
    }
  }
  plan.accepted = true;
  plan.vector_groups =
      (request.row_count + request.group_size - 1) / request.group_size;
  plan.diagnostic_code = "IPAR_VALIDATION_VECTOR_READY";
  Add(&plan.evidence, "validation_exact_row_diagnostics=true");
  Add(&plan.evidence, "validation_vectorized_groups=true");
  return plan;
}

}  // namespace scratchbird::engine::sblr
