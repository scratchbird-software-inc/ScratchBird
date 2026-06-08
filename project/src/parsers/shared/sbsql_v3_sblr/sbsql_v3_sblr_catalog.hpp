// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql_v3_sblr {

struct SblrOpcodeEntry {
  std::string sblr_operation;
  std::uint32_t opcode_value = 0;
  std::string payload_class;
  std::string api_operation_id;
  bool cluster_authority_required = false;
  bool fail_closed_without_cluster_authority = false;
  bool raw_sql_payload_allowed = false;
};

struct SblrEnvelope {
  std::string sblr_operation;
  std::uint32_t opcode_value = 0;
  std::uint16_t sblr_version = 1;
  std::string binding_epoch;
  std::string bound_root_uuid;
  std::string descriptor_digest;
  std::string payload_class;
  bool contains_raw_sql_text = false;
  bool cluster_authority_present = false;
};

bool IsUuidV7(std::string_view uuid_text);
std::optional<std::uint32_t> ParseOpcodeValue(std::string_view text);
bool ValidateOpcodeEntry(const SblrOpcodeEntry& entry, std::vector<std::string>* errors);
bool ValidateEnvelope(const SblrOpcodeEntry& entry, const SblrEnvelope& envelope, std::vector<std::string>* errors);
std::string EncodeEnvelopeForProbe(const SblrEnvelope& envelope);
std::optional<SblrEnvelope> DecodeEnvelopeForProbe(std::string_view encoded, std::vector<std::string>* errors);

}  // namespace scratchbird::parser::sbsql_v3_sblr
