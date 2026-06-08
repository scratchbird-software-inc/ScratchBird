// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_engine_envelope.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

enum class SblrOpcodeCategory {
  artifact,
  catalog,
  cluster,
  core,
  data_mutation,
  data_read,
  ddl,
  dml,
  expression,
  extensibility,
  management,
  nosql,
  observability,
  query,
  result_shape,
  security,
  transaction,
  unknown,
};

enum class SblrOpcodeSupport {
  implemented,
  local_profile_refusal,
  cluster_refusal,
  future_profile_refusal,
  deprecated_refusal,
  unsupported,
};

enum class SblrOpcodeTransactionEffect {
  none,
  read,
  local_write,
  local_or_cluster_write,
  catalog_write,
  cluster_write,
  management,
  security,
  external_audit,
  unknown,
};

enum class SblrOpcodeSecurityClass {
  public_metadata,
  authenticated,
  object_authorized,
  admin_authorized,
  sysarch_authorized,
  event_admin,
  cluster_authorized,
  unknown,
};

struct SblrOpcodeEntry {
  std::string operation_id;
  std::string opcode;
  std::string family;
  SblrOpcodeCategory category = SblrOpcodeCategory::unknown;
  SblrOpcodeSupport support = SblrOpcodeSupport::unsupported;
  SblrOpcodeTransactionEffect transaction_effect = SblrOpcodeTransactionEffect::unknown;
  SblrOpcodeSecurityClass security_class = SblrOpcodeSecurityClass::unknown;
  bool requires_security_context = true;
  bool requires_transaction_context = false;
  bool requires_cluster_authority = false;
  bool cluster_private = false;
  std::string refusal_diagnostic;
};

struct SblrOpcodeValidationResult {
  bool ok = false;
  const SblrOpcodeEntry* entry = nullptr;
  std::string diagnostic_id;
  std::string detail;
};

const std::vector<SblrOpcodeEntry>& StaticSblrOpcodeRegistry();
const SblrOpcodeEntry* LookupSblrOperation(std::string_view operation_id);
const SblrOpcodeEntry* LookupSblrOpcode(std::string_view opcode);
SblrOpcodeValidationResult ValidateSblrOpcodeForEnvelope(const SblrOperationEnvelope& envelope);
std::string ToString(SblrOpcodeCategory category);
std::string ToString(SblrOpcodeSupport support);
std::string ToString(SblrOpcodeTransactionEffect effect);
std::string ToString(SblrOpcodeSecurityClass security_class);

}  // namespace scratchbird::engine::sblr
