// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "session_ledger_record.hpp"

#include "hash_digest.hpp"

#include <algorithm>

namespace scratchbird::storage::page {
namespace {
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using E = SessionLedgerRecordError;

constexpr std::size_t kChecksumAt = 248;
constexpr std::size_t kFieldsAt = 48;

bool V7(const Uuid& id) { return scratchbird::core::uuid::IsEngineIdentityUuid(id); }
bool NoneOrV7(const Uuid& id) { return id.is_nil() || V7(id); }
void PutUuid(byte* p, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), p); }
Uuid GetUuid(const byte* p) {
  Uuid id;
  std::copy_n(p, 16, id.bytes.begin());
  return id;
}
bool ZeroRange(const byte* p, std::size_t n) {
  return std::all_of(p, p + n, [](byte b) { return b == 0; });
}

// The first 8 bytes of SHA-256 over bytes 0..247. False only when the hash
// provider fails (the record is then neither written nor accepted).
bool Checksum(const byte* record, std::array<byte, 8>& out) noexcept {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(record, kChecksumAt);
  if (!digest.ok()) return false;
  std::copy_n(digest.digest.begin(), 8, out.begin());
  return true;
}

E ValidateSettings(const LedgerSettings& s) {
  if (!NoneOrV7(s.language_uuid)) return E::invalid_optional_identity;
  return E::none;
}
void PutSettings(byte* p, const LedgerSettings& s) {
  PutUuid(p, s.language_uuid);
  p[16] = s.isolation_profile;
  p[17] = s.read_only ? 1 : 0;
  StoreLittle32(p + 20, s.lock_wait_timeout_ms);
}
// Reads a settings block; false when a reserved byte is set or read-only is not 0/1.
bool GetSettings(const byte* p, LedgerSettings& s, E& error) {
  if (p[17] > 1) { error = E::invalid_enum; return false; }
  if (p[18] || p[19] || !ZeroRange(p + 24, 24)) { error = E::invalid_reserved; return false; }
  s.language_uuid = GetUuid(p);
  s.isolation_profile = p[16];
  s.read_only = p[17] != 0;
  s.lock_wait_timeout_ms = LoadLittle32(p + 20);
  return true;
}

E ValidateBody(const LedgerConnectionRecord& c) {
  if (!V7(c.listener_uuid) || !V7(c.parser_instance_uuid)) return E::invalid_required_identity;
  if (!NoneOrV7(c.route_uuid)) return E::invalid_optional_identity;
  if (c.opened_at_utc_ns == 0) return E::invalid_time;
  const auto transport = static_cast<std::uint8_t>(c.transport);
  if (transport < 1 || transport > 5) return E::invalid_enum;
  std::size_t want_min = 0, want_max = 0;
  switch (c.peer_address_type) {
    case LedgerPeerAddressType::none: break;
    case LedgerPeerAddressType::ipv4: want_min = want_max = 4; break;
    case LedgerPeerAddressType::ipv6: want_min = want_max = 16; break;
    case LedgerPeerAddressType::opaque: want_min = 1; want_max = kLedgerPeerAddressBytes; break;
    default: return E::invalid_enum;
  }
  if (c.peer_address_length < want_min || c.peer_address_length > want_max) return E::invalid_peer_address;
  if (c.peer_address_type == LedgerPeerAddressType::none && c.peer_port != 0) return E::invalid_peer_address;
  if (!ZeroRange(c.peer_address.data() + c.peer_address_length, kLedgerPeerAddressBytes - c.peer_address_length))
    return E::invalid_peer_address;
  if (c.tls_evidence_kind > 1) return E::invalid_enum;
  const bool hash_set = !ZeroRange(c.tls_evidence_hash.data(), kLedgerDigestBytes);
  if ((c.tls_evidence_kind == 1) != hash_set) return E::invalid_tls_evidence;
  return E::none;
}
E ValidateBody(const LedgerSessionRecord& s) {
  if (!V7(s.principal_uuid) || !V7(s.engine_instance_uuid)) return E::invalid_required_identity;
  if (!NoneOrV7(s.auth_context_uuid) || !NoneOrV7(s.policy_snapshot_uuid)) return E::invalid_optional_identity;
  if (const auto e = ValidateSettings(s.settings); e != E::none) return e;
  if (s.opened_at_utc_ns == 0) return E::invalid_time;
  const auto reason = static_cast<std::uint16_t>(s.end_reason);
  if (s.ended_at_utc_ns == 0) return reason == 0 ? E::none : E::invalid_state;
  if (s.ended_at_utc_ns < s.opened_at_utc_ns) return E::invalid_time;
  if (reason < 1 || reason > 5) return E::invalid_enum;
  return E::none;
}
E ValidateBody(const LedgerAttachmentRecord& a) {
  if (!V7(a.session_uuid) || !V7(a.connection_uuid) || !V7(a.engine_instance_uuid)) return E::invalid_required_identity;
  if (!NoneOrV7(a.predecessor_uuid)) return E::invalid_optional_identity;
  if (a.predecessor_uuid == a.session_uuid || a.predecessor_uuid == a.connection_uuid) return E::invalid_optional_identity;
  if (a.at_utc_ns == 0) return E::invalid_time;
  return E::none;
}
E ValidateBody(const LedgerSettingsChangeRecord& c) {
  if (!V7(c.session_uuid)) return E::invalid_required_identity;
  if (const auto e = ValidateSettings(c.settings); e != E::none) return e;
  if (c.generation == 0 || c.effective_from_transaction_order == 0) return E::invalid_generation;
  if (c.at_utc_ns == 0) return E::invalid_time;
  return E::none;
}
}  // namespace

const char* SessionLedgerKindName(SessionLedgerKind kind) noexcept {
  switch (kind) {
    case SessionLedgerKind::connection: return "connection";
    case SessionLedgerKind::session: return "session";
    case SessionLedgerKind::attachment: return "attachment";
    case SessionLedgerKind::settings_change: return "settings_change";
  }
  return "invalid";
}

const char* SessionLedgerRecordErrorName(SessionLedgerRecordError error) noexcept {
  switch (error) {
    case E::none: return "none";
    case E::invalid_framing: return "invalid_framing";
    case E::invalid_kind: return "invalid_kind";
    case E::invalid_state: return "invalid_state";
    case E::invalid_reserved: return "invalid_reserved";
    case E::invalid_identity: return "invalid_identity";
    case E::invalid_optional_identity: return "invalid_optional_identity";
    case E::invalid_required_identity: return "invalid_required_identity";
    case E::invalid_time: return "invalid_time";
    case E::invalid_enum: return "invalid_enum";
    case E::invalid_peer_address: return "invalid_peer_address";
    case E::invalid_tls_evidence: return "invalid_tls_evidence";
    case E::invalid_generation: return "invalid_generation";
    case E::invalid_checksum: return "invalid_checksum";
    case E::short_buffer: return "short_buffer";
  }
  return "invalid";
}

SessionLedgerRecordError ValidateSessionLedgerRecord(const SessionLedgerRecord& r) noexcept {
  if (!V7(r.database_uuid) || !V7(r.record_uuid)) return E::invalid_identity;
  return std::visit([](const auto& body) { return ValidateBody(body); }, r.body);
}

SessionLedgerRecordError EncodeSessionLedgerRecord(const SessionLedgerRecord& r, std::span<byte> output) noexcept {
  if (const auto e = ValidateSessionLedgerRecord(r); e != E::none) return e;
  if (output.size() < kSessionLedgerRecordBytes) return E::short_buffer;
  std::array<byte, kSessionLedgerRecordBytes> p{};
  StoreLittle16(p.data() + 0, kSessionLedgerRecordVersion);
  StoreLittle16(p.data() + 2, static_cast<u16>(kSessionLedgerRecordBytes));
  p[4] = static_cast<byte>(r.body.index() + 1);
  StoreLittle64(p.data() + 8, r.ordinal);
  PutUuid(p.data() + 16, r.database_uuid);
  PutUuid(p.data() + 32, r.record_uuid);
  byte* f = p.data() + kFieldsAt;
  p[5] = 1;
  if (const auto* c = std::get_if<LedgerConnectionRecord>(&r.body)) {
    PutUuid(f + 0, c->listener_uuid);
    PutUuid(f + 16, c->parser_instance_uuid);
    PutUuid(f + 32, c->route_uuid);
    StoreLittle64(f + 48, c->opened_at_utc_ns);
    f[56] = static_cast<byte>(c->transport);
    f[57] = static_cast<byte>(c->peer_address_type);
    f[58] = c->peer_address_length;
    f[59] = c->tls_evidence_kind;
    StoreLittle16(f + 60, c->peer_port);
    std::copy(c->peer_address.begin(), c->peer_address.end(), f + 64);
    std::copy(c->tls_evidence_hash.begin(), c->tls_evidence_hash.end(), f + 96);
  } else if (const auto* s = std::get_if<LedgerSessionRecord>(&r.body)) {
    p[5] = s->ended() ? 2 : 1;
    PutUuid(f + 0, s->principal_uuid);
    PutUuid(f + 16, s->auth_context_uuid);
    std::copy(s->role_set_digest.begin(), s->role_set_digest.end(), f + 32);
    PutUuid(f + 64, s->engine_instance_uuid);
    PutUuid(f + 80, s->policy_snapshot_uuid);
    PutSettings(f + 96, s->settings);
    StoreLittle64(f + 144, s->opened_at_utc_ns);
    StoreLittle64(f + 152, s->ended_at_utc_ns);
    StoreLittle16(f + 160, static_cast<u16>(s->end_reason));
  } else if (const auto* a = std::get_if<LedgerAttachmentRecord>(&r.body)) {
    PutUuid(f + 0, a->session_uuid);
    PutUuid(f + 16, a->connection_uuid);
    PutUuid(f + 32, a->predecessor_uuid);
    PutUuid(f + 48, a->engine_instance_uuid);
    StoreLittle64(f + 64, a->at_utc_ns);
  } else if (const auto* c = std::get_if<LedgerSettingsChangeRecord>(&r.body)) {
    PutUuid(f + 0, c->session_uuid);
    StoreLittle64(f + 16, c->generation);
    PutSettings(f + 24, c->settings);
    StoreLittle64(f + 72, c->effective_from_transaction_order);
    StoreLittle64(f + 80, c->at_utc_ns);
  }
  std::array<byte, 8> sum{};
  if (!Checksum(p.data(), sum)) return E::invalid_checksum;
  std::copy(sum.begin(), sum.end(), p.data() + kChecksumAt);
  std::copy(p.begin(), p.end(), output.begin());
  return E::none;
}

SessionLedgerDecodeResult DecodeSessionLedgerRecord(std::span<const byte> bytes) noexcept {
  SessionLedgerDecodeResult result;
  if (bytes.size() != kSessionLedgerRecordBytes) return result;
  const byte* p = bytes.data();
  std::array<byte, 8> sum{};
  if (!Checksum(p, sum) || !std::equal(sum.begin(), sum.end(), p + kChecksumAt)) {
    result.error = E::invalid_checksum;
    return result;
  }
  if (LoadLittle16(p + 0) != kSessionLedgerRecordVersion || LoadLittle16(p + 2) != kSessionLedgerRecordBytes) return result;
  const std::uint8_t kind = p[4], state = p[5];
  if (kind < 1 || kind > 4) { result.error = E::invalid_kind; return result; }
  if (state != 1 && !(kind == 2 && state == 2)) { result.error = E::invalid_state; return result; }
  if (p[6] || p[7]) { result.error = E::invalid_reserved; return result; }
  SessionLedgerRecord r;
  r.ordinal = LoadLittle64(p + 8);
  r.database_uuid = GetUuid(p + 16);
  r.record_uuid = GetUuid(p + 32);
  const byte* f = p + kFieldsAt;
  const auto reserved = [&](std::size_t from_field_offset) { return ZeroRange(f + from_field_offset, kChecksumAt - kFieldsAt - from_field_offset); };
  E error = E::none;
  switch (kind) {
    case 1: {
      LedgerConnectionRecord c;
      c.listener_uuid = GetUuid(f + 0);
      c.parser_instance_uuid = GetUuid(f + 16);
      c.route_uuid = GetUuid(f + 32);
      c.opened_at_utc_ns = LoadLittle64(f + 48);
      c.transport = static_cast<LedgerTransport>(f[56]);
      c.peer_address_type = static_cast<LedgerPeerAddressType>(f[57]);
      c.peer_address_length = f[58];
      c.tls_evidence_kind = f[59];
      c.peer_port = LoadLittle16(f + 60);
      if (f[62] || f[63] || !reserved(128)) { result.error = E::invalid_reserved; return result; }
      std::copy_n(f + 64, kLedgerPeerAddressBytes, c.peer_address.begin());
      std::copy_n(f + 96, kLedgerDigestBytes, c.tls_evidence_hash.begin());
      r.body = c;
      break;
    }
    case 2: {
      LedgerSessionRecord s;
      s.principal_uuid = GetUuid(f + 0);
      s.auth_context_uuid = GetUuid(f + 16);
      std::copy_n(f + 32, kLedgerDigestBytes, s.role_set_digest.begin());
      s.engine_instance_uuid = GetUuid(f + 64);
      s.policy_snapshot_uuid = GetUuid(f + 80);
      if (!GetSettings(f + 96, s.settings, error)) { result.error = error; return result; }
      s.opened_at_utc_ns = LoadLittle64(f + 144);
      s.ended_at_utc_ns = LoadLittle64(f + 152);
      s.end_reason = static_cast<LedgerEndReason>(LoadLittle16(f + 160));
      if (!reserved(162)) { result.error = E::invalid_reserved; return result; }
      if ((state == 2) != s.ended()) { result.error = E::invalid_state; return result; }
      r.body = s;
      break;
    }
    case 3: {
      LedgerAttachmentRecord a;
      a.session_uuid = GetUuid(f + 0);
      a.connection_uuid = GetUuid(f + 16);
      a.predecessor_uuid = GetUuid(f + 32);
      a.engine_instance_uuid = GetUuid(f + 48);
      a.at_utc_ns = LoadLittle64(f + 64);
      if (!reserved(72)) { result.error = E::invalid_reserved; return result; }
      r.body = a;
      break;
    }
    default: {
      LedgerSettingsChangeRecord c;
      c.session_uuid = GetUuid(f + 0);
      c.generation = LoadLittle64(f + 16);
      if (!GetSettings(f + 24, c.settings, error)) { result.error = error; return result; }
      c.effective_from_transaction_order = LoadLittle64(f + 72);
      c.at_utc_ns = LoadLittle64(f + 80);
      if (!reserved(88)) { result.error = E::invalid_reserved; return result; }
      r.body = c;
      break;
    }
  }
  result.error = ValidateSessionLedgerRecord(r);
  if (result.error == E::none) result.record = r;
  return result;
}

}  // namespace scratchbird::storage::page
