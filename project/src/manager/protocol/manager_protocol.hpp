// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_MANAGER_PROTOCOL_LIBRARY

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::manager::protocol {

struct Field {
  std::string key;
  std::string value;
};

struct Diagnostic {
  std::string code;
  std::string message_key;
  std::string severity;
  std::string safe_message;
  std::vector<Field> fields;
};

struct MessageVectorSet {
  std::array<std::uint8_t, 16> request_uuid{};
  std::vector<Diagnostic> diagnostics;
};

struct DiagnosticResult {
  bool ok = true;
  std::vector<Diagnostic> diagnostics;
};

using Bytes = std::vector<std::uint8_t>;
using UuidBytes = std::array<std::uint8_t, 16>;
using Sha256Digest = std::array<std::uint8_t, 32>;

struct SbdbFrame {
  std::uint8_t type = 0;
  std::uint8_t flags = 0;
  Bytes payload;
};

struct ControlPlaneMessage {
  std::uint16_t message_type = 0;
  std::uint16_t flags = 0;
  std::uint64_t request_id = 0;
  Bytes payload;
};

struct DbbtToken {
  std::uint8_t version = 1;
  UuidBytes db_uuid{};
  std::uint32_t listener_id = 1;
  std::uint64_t issued_at_ms = 0;
  std::uint64_t expires_at_ms = 0;
  UuidBytes manager_session_id{};
  Bytes client_nonce;
  Bytes server_nonce;
  std::uint32_t flags = 0;
  Sha256Digest mac{};
};

struct DbbtValidationOptions {
  std::uint32_t expected_listener_id = 0;
  std::uint64_t now_ms = 0;
  std::uint64_t clock_skew_ms = 2000;
};

struct Lpreface {
  std::uint16_t reserved = 0;
  std::uint32_t listener_id = 0;
  Bytes dbbt;
  std::string db_selector;
  std::string requested_profile;
  std::string auth_provider_family;
  std::string auth_principal;
  std::string auth_token;
  std::uint32_t flags = 0;
};

struct LprefaceAck {
  bool accepted = false;
  std::uint16_t nack_code = 0;
  std::string message;
};

struct LprefaceHandoffClaim {
  Bytes client_nonce;
  Bytes server_nonce;
};

struct DbbtKeyringKey {
  std::string key_id;
  Bytes key;
  bool active = false;
};

struct DbbtKeyring {
  std::string active_key_id;
  Bytes active_key;
  std::vector<DbbtKeyringKey> verification_keys;
  std::uint64_t not_before_ms = 0;
  std::uint64_t not_after_ms = 0;
};

class DbbtReplayCache {
 public:
  explicit DbbtReplayCache(std::size_t max_entries = 4096);

  bool CheckAndInsert(const Sha256Digest& token_id,
                      std::uint64_t expires_at_ms,
                      std::uint64_t now_ms);
  void PruneExpired(std::uint64_t now_ms);
  std::size_t size() const;

 private:
  struct Entry {
    Sha256Digest token_id{};
    std::uint64_t expires_at_ms = 0;
  };

  std::size_t max_entries_ = 1;
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};

Diagnostic MakeDiagnostic(std::string code,
                          std::string message,
                          std::vector<Field> fields = {},
                          std::string severity = "error");
std::string JsonEscape(std::string_view value);
std::string ToMessageVectorJsonLine(const Diagnostic& diagnostic);
std::string ToMessageVectorSetJson(const MessageVectorSet& set);
DiagnosticResult EncodeMessageVectorSetV1(const MessageVectorSet& set,
                                          Bytes* encoded_out,
                                          std::uint64_t registry_generation = 0,
                                          std::uint32_t max_render_bytes = 65536);
std::optional<MessageVectorSet> DecodeMessageVectorSetV1(const Bytes& encoded,
                                                         std::vector<Diagnostic>* diagnostics);
UuidBytes MakePseudoUuidV7();
std::string Hex(const std::uint8_t* data, std::size_t size);
std::string Hex(const Bytes& data);
std::string Hex(const UuidBytes& data);
Bytes FromHex(std::string_view hex);
Bytes EncodeSbdbFrame(const SbdbFrame& frame);
std::optional<SbdbFrame> DecodeSbdbFrame(const Bytes& encoded, std::vector<Diagnostic>* diagnostics);
Bytes EncodeControlPlaneMessage(const ControlPlaneMessage& message);
std::optional<ControlPlaneMessage> DecodeControlPlaneMessage(const Bytes& encoded, std::vector<Diagnostic>* diagnostics);
bool ConstantTimeEqual(const std::uint8_t* lhs,
                       const std::uint8_t* rhs,
                       std::size_t size);
Sha256Digest Sha256(const Bytes& bytes);
Sha256Digest HmacSha256(const Bytes& key, const Bytes& body);
Bytes EncodeDbbtBody(const DbbtToken& token);
Bytes EncodeDbbt(const DbbtToken& token, const Bytes& key);
std::optional<DbbtToken> DecodeDbbt(const Bytes& encoded, std::vector<Diagnostic>* diagnostics);
DiagnosticResult LoadDbbtKeyring(const std::filesystem::path& path, DbbtKeyring* keyring_out);
Sha256Digest DbbtTokenId(const DbbtToken& token, const Bytes& key);
DiagnosticResult ValidateDbbtWithKeyring(const Bytes& encoded,
                                         const DbbtKeyring& keyring,
                                         const DbbtValidationOptions& options,
                                         DbbtReplayCache* replay_cache = nullptr,
                                         DbbtToken* token_out = nullptr,
                                         std::string* matched_key_id_out = nullptr);
DiagnosticResult EncodeLpreface(const Lpreface& preface, Bytes* encoded_out);
std::optional<Lpreface> DecodeLpreface(const Bytes& encoded, std::vector<Diagnostic>* diagnostics);
DiagnosticResult EncodeLprefaceAck(const LprefaceAck& ack, Bytes* encoded_out);
std::optional<LprefaceAck> DecodeLprefaceAck(const Bytes& encoded, std::vector<Diagnostic>* diagnostics);
std::string EncodeLprefaceHandoffClaim(const Bytes& client_nonce, const Bytes& server_nonce);
bool IsLprefaceHandoffClaimPrefix(std::string_view text);
std::optional<LprefaceHandoffClaim> DecodeLprefaceHandoffClaim(std::string_view line,
                                                               std::vector<Diagnostic>* diagnostics);
DiagnosticResult ValidateDbbt(const Bytes& encoded,
                              const Bytes& key,
                              const DbbtValidationOptions& options,
                              DbbtToken* token_out = nullptr);
std::uint64_t CurrentEpochMilliseconds();

}  // namespace scratchbird::manager::protocol
