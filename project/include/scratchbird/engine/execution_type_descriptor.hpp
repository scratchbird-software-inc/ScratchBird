// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine {

enum class ExecutionTypeFamily : std::uint16_t {
  null_type = 0,
  boolean = 1,
  signed_integer = 2,
  unsigned_integer = 3,
  real = 4,
  decimal = 5,
  uuid = 6,
  character = 7,
  binary = 8,
  bit_string = 9,
  temporal = 10,
  blob = 11,
  network = 12,
  document = 13,
  search = 14,
  structured = 15,
  range = 16,
  spatial = 17,
  vector = 18,
  graph = 19,
  time_series = 20,
  columnar = 21,
  aggregate_state = 22,
  sketch = 23,
  locator = 24,
  opaque = 25,
  result_set = 26,
  unknown = 0xffffu
};

enum class ExecutionTypeWidthClass : std::uint16_t {
  fixed = 0,
  variable = 1,
  descriptor_defined = 2,
  unknown = 0xffffu
};

enum class ExecutionTypeModifierFlag : std::uint64_t {
  precision = 1ull << 0,
  scale = 1ull << 1,
  length = 1ull << 2,
  charset_uuid = 1ull << 3,
  collation_uuid = 1ull << 4,
  timezone_uuid = 1ull << 5,
  domain_uuid = 1ull << 6,
  domain_stack = 1ull << 7,
  element_descriptor_uuid = 1ull << 8,
  vector_dimensions = 1ull << 9,
  container_rank = 1ull << 10,
  security_policy_uuid = 1ull << 11
};

constexpr std::uint64_t ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag flag) {
  return static_cast<std::uint64_t>(flag);
}

struct ExecutionTypeDescriptor {
  Uuid descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::uint32_t canonical_type_id = 0;
  ExecutionTypeFamily family = ExecutionTypeFamily::unknown;
  ExecutionTypeWidthClass width_class = ExecutionTypeWidthClass::unknown;
  std::string stable_name;
  std::uint32_t bit_width = 0;
  std::uint32_t precision = 0;
  std::uint32_t scale = 0;
  std::uint32_t length = 0;
  std::uint32_t vector_dimensions = 0;
  std::uint32_t container_rank = 0;
  std::uint64_t modifier_flags = 0;
  Uuid domain_uuid{};
  std::vector<Uuid> domain_stack;
  Uuid charset_uuid{};
  Uuid collation_uuid{};
  Uuid timezone_uuid{};
  Uuid element_descriptor_uuid{};
  Uuid security_policy_uuid{};
  bool nullable_allowed = true;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

}  // namespace scratchbird::engine
