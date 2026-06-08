// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_sblr_catalog.hpp"

#include <charconv>
#include <sstream>

namespace scratchbird::parser::sbsql_v3_sblr {
namespace {

std::vector<std::string_view> Split(std::string_view text, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto pos = text.find(delimiter, start);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

}  // namespace

bool IsUuidV7(std::string_view uuid_text) {
  return uuid_text.size() == 36 && uuid_text[8] == '-' && uuid_text[13] == '-' &&
         uuid_text[18] == '-' && uuid_text[23] == '-' && uuid_text[14] == '7';
}

std::optional<std::uint32_t> ParseOpcodeValue(std::string_view text) {
  std::uint32_t value = 0;
  if (StartsWith(text, "0x") || StartsWith(text, "0X")) {
    const auto digits = text.substr(2);
    const auto* begin = digits.data();
    const auto* end = digits.data() + digits.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec == std::errc() && result.ptr == end) {
      return value;
    }
    return std::nullopt;
  }
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value, 10);
  if (result.ec == std::errc() && result.ptr == end) {
    return value;
  }
  return std::nullopt;
}

bool ValidateOpcodeEntry(const SblrOpcodeEntry& entry, std::vector<std::string>* errors) {
  const auto before = errors ? errors->size() : 0;
  const auto add = [&](std::string message) {
    if (errors) {
      errors->push_back(std::move(message));
    }
  };
  if (!StartsWith(entry.sblr_operation, "SBLR_")) {
    add("sblr operation must use SBLR_ prefix");
  }
  if (entry.opcode_value == 0) {
    add("opcode value must be non-zero");
  }
  if (entry.payload_class.empty()) {
    add("payload class is required");
  }
  if (entry.raw_sql_payload_allowed) {
    add("raw SQL payloads are forbidden");
  }
  if (entry.cluster_authority_required && !entry.fail_closed_without_cluster_authority) {
    add("cluster authority rows must fail closed without authority");
  }
  return !errors || errors->size() == before;
}

bool ValidateEnvelope(const SblrOpcodeEntry& entry, const SblrEnvelope& envelope, std::vector<std::string>* errors) {
  const auto before = errors ? errors->size() : 0;
  const auto add = [&](std::string message) {
    if (errors) {
      errors->push_back(std::move(message));
    }
  };
  ValidateOpcodeEntry(entry, errors);
  if (envelope.sblr_operation != entry.sblr_operation) {
    add("envelope operation does not match opcode entry");
  }
  if (envelope.opcode_value != entry.opcode_value) {
    add("envelope opcode value does not match opcode entry");
  }
  if (envelope.sblr_version == 0) {
    add("SBLR version must be non-zero");
  }
  if (!IsUuidV7(envelope.bound_root_uuid)) {
    add("bound root UUID must be UUIDv7");
  }
  if (envelope.payload_class != entry.payload_class) {
    add("payload class does not match opcode entry");
  }
  if (envelope.contains_raw_sql_text || entry.raw_sql_payload_allowed) {
    add("engine envelope cannot contain raw SQL text");
  }
  if (entry.cluster_authority_required && !envelope.cluster_authority_present) {
    add("cluster authority token required for cluster envelope");
  }
  return !errors || errors->size() == before;
}

std::string EncodeEnvelopeForProbe(const SblrEnvelope& envelope) {
  std::ostringstream out;
  out << "SBLR3|" << envelope.sblr_version << '|' << envelope.sblr_operation << '|'
      << envelope.opcode_value << '|' << envelope.binding_epoch << '|'
      << envelope.bound_root_uuid << '|' << envelope.descriptor_digest << '|'
      << envelope.payload_class << '|' << (envelope.contains_raw_sql_text ? '1' : '0') << '|'
      << (envelope.cluster_authority_present ? '1' : '0');
  return out.str();
}

std::optional<SblrEnvelope> DecodeEnvelopeForProbe(std::string_view encoded, std::vector<std::string>* errors) {
  const auto parts = Split(encoded, '|');
  if (parts.size() != 10 || parts[0] != "SBLR3") {
    if (errors) {
      errors->push_back("invalid SBLR probe envelope");
    }
    return std::nullopt;
  }
  SblrEnvelope envelope;
  std::uint32_t version = 0;
  auto version_result = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), version, 10);
  if (version_result.ec != std::errc() || version_result.ptr != parts[1].data() + parts[1].size()) {
    if (errors) {
      errors->push_back("invalid SBLR version");
    }
    return std::nullopt;
  }
  const auto opcode = ParseOpcodeValue(parts[3]);
  if (!opcode) {
    if (errors) {
      errors->push_back("invalid opcode value");
    }
    return std::nullopt;
  }
  envelope.sblr_version = static_cast<std::uint16_t>(version);
  envelope.sblr_operation = std::string(parts[2]);
  envelope.opcode_value = *opcode;
  envelope.binding_epoch = std::string(parts[4]);
  envelope.bound_root_uuid = std::string(parts[5]);
  envelope.descriptor_digest = std::string(parts[6]);
  envelope.payload_class = std::string(parts[7]);
  envelope.contains_raw_sql_text = parts[8] == "1";
  envelope.cluster_authority_present = parts[9] == "1";
  return envelope;
}

}  // namespace scratchbird::parser::sbsql_v3_sblr
