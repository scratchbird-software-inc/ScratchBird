// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "transaction_state_page.hpp"
#include "uuid.hpp"

#include <array>
#include <optional>
#include <span>
#include <variant>

namespace scratchbird::storage::page {

using scratchbird::core::platform::Uuid;

// SEARCH_KEY: SB_SESSION_LEDGER_RECORD
// Plan 36 T2 (owner Q165 = 1, 2026-11-14; Q308 2026-11-19: the ledger is built
// before S4e). The durable SESSION LEDGER is a chain of fixed 256-byte records
// that is NOT catalog rows (a catalog row needs a transaction, which needs a
// session). It follows the SBTXHD01 transaction-header pattern: careful-write,
// one record written once into an empty slot, two records per 512-byte write
// unit. RULE (plan 36 §Target design 2): a record is durable before any
// transaction header cites it.
//
// Four record kinds. EVERY identifier is a binary(16) UUID; there is no text
// identifier and no name in a record. Peer addresses and the like are DATA
// bytes with an explicit type and length (see ConnectionRecord).
//   connection      one transport channel. Its record UUID IS the server-issued
//                   server_channel_uuid (never the client's own id).
//   session         principal, auth context, role-set digest, engine instance,
//                   policy snapshot, settings, opened and (in an END record)
//                   ended with a reason.
//   attachment      one binding of a session to a connection; predecessor link.
//   settings change a session's settings at a generation, effective from a
//                   transaction order.
//
// WRITE-ONCE. A record is never rewritten (a torn rewrite of a cited record
// would turn it into corruption). A session ENDS by appending a second session
// record, same record UUID, state 2 (ended), carrying ended_at and the end
// reason; the latest record of a UUID is its current state. Whether and when a
// session ends is S4e's business; this codec only admits both states.
//
// Layout (little-endian, 256 bytes):
//   +0   u16 version (1)             +2   u16 record bytes (256)
//   +4   u8  kind (1 connection, 2 session, 3 attachment, 4 settings change)
//   +5   u8  state (1 = complete; session only: 2 = complete, ended)
//   +6   u16 zero
//   +8   u64 ordinal (the slot's number in the chain: sequence * slots + slot)
//   +16  database UUID (v7)          +32  record UUID (v7)
//   +48..+247 the kind's fields, every unlisted byte zero
//   +248 u64 checksum: the first 8 bytes of SHA-256 over bytes 0..247. Any
//        single byte changed anywhere in the record fails it; an all-zero slot
//        is the EMPTY slot and has no checksum.
//
// Settings block (48 bytes, used by session and settings change):
//   +0 language UUID (zero = unset or v7)  +16 u8 isolation profile (as the
//   transaction header's byte)  +17 u8 read-only (0 or 1)  +18 u16 zero
//   +20 u32 lock wait timeout ms (0 = unset)  +24..+47 zero (reserved)
//
// Connection (kind 1), fields from +48:
//   +48 listener UUID (v7)           +64 parser instance UUID (v7)
//   +80 tunnel or route UUID (zero = direct, else v7)
//   +96 u64 opened_at, UTC ns (non-zero)
//   +104 u8 transport (1 tcp, 2 unix socket, 3 shared memory, 4 pipe, 5 in-process)
//   +105 u8 peer address type (0 none, 1 IPv4 = 4 bytes, 2 IPv6 = 16 bytes,
//        3 opaque transport peer data = 1..32 bytes)
//   +106 u8 peer address length    +107 u8 TLS evidence kind (0 none,
//        1 = a SHA-256 hash of the peer evidence at +144)
//   +108 u16 peer port (0 unless a type is stated)   +110 u16 zero
//   +112 [32] peer address bytes (the first length bytes, the rest zero)
//   +144 [32] TLS evidence hash (zero when the kind is 0)
//   +176..+247 zero (reserved)
// Session (kind 2), fields from +48:
//   +48 principal UUID (v7)          +64 auth context UUID (zero or v7)
//   +80 [32] role-set digest (SHA-256 of the role set; zero = none stated)
//   +112 engine instance UUID (v7)   +128 policy snapshot UUID (zero or v7)
//   +144 [48] settings block
//   +192 u64 opened_at (non-zero)    +200 u64 ended_at (0 in state 1; in state
//        2 at least opened_at)
//   +208 u16 end reason (0 in state 1; in state 2: 1 closed by client, 2 closed
//        by server, 3 ended by policy, 4 connection lost, 5 server shutdown)
//   +210..+247 zero
// Attachment (kind 3), fields from +48:
//   +48 session UUID (v7)            +64 connection UUID (v7)
//   +80 predecessor attachment UUID (zero or v7)
//   +96 engine instance UUID (v7)    +112 u64 at (non-zero)   +120..+247 zero
// Settings change (kind 4), fields from +48:
//   +48 session UUID (v7)            +64 u64 generation (>= 1)
//   +72 [48] settings block          +120 u64 effective-from transaction order (>= 1)
//   +128 u64 at (non-zero)           +136..+247 zero

inline constexpr std::size_t kSessionLedgerRecordBytes = 256;
inline constexpr u16 kSessionLedgerRecordVersion = 1;

enum class SessionLedgerKind : std::uint8_t { connection = 1, session = 2, attachment = 3, settings_change = 4 };
const char* SessionLedgerKindName(SessionLedgerKind kind) noexcept;

enum class LedgerTransport : std::uint8_t { tcp = 1, unix_socket = 2, shared_memory = 3, pipe = 4, in_process = 5 };
enum class LedgerPeerAddressType : std::uint8_t { none = 0, ipv4 = 1, ipv6 = 2, opaque = 3 };
enum class LedgerEndReason : std::uint16_t {
  none = 0,
  closed_by_client = 1,
  closed_by_server = 2,
  ended_by_policy = 3,
  connection_lost = 4,
  server_shutdown = 5,
};
inline constexpr std::size_t kLedgerPeerAddressBytes = 32;
inline constexpr std::size_t kLedgerDigestBytes = 32;

struct LedgerSettings {
  Uuid language_uuid;
  std::uint8_t isolation_profile = 0;
  bool read_only = false;
  u32 lock_wait_timeout_ms = 0;
  bool operator==(const LedgerSettings&) const = default;
};

struct LedgerConnectionRecord {
  Uuid listener_uuid;
  Uuid parser_instance_uuid;
  Uuid route_uuid;
  u64 opened_at_utc_ns = 0;
  LedgerTransport transport = LedgerTransport::tcp;
  LedgerPeerAddressType peer_address_type = LedgerPeerAddressType::none;
  std::uint8_t peer_address_length = 0;
  std::uint8_t tls_evidence_kind = 0;
  u16 peer_port = 0;
  std::array<byte, kLedgerPeerAddressBytes> peer_address{};
  std::array<byte, kLedgerDigestBytes> tls_evidence_hash{};
  bool operator==(const LedgerConnectionRecord&) const = default;
};

struct LedgerSessionRecord {
  Uuid principal_uuid;
  Uuid auth_context_uuid;
  std::array<byte, kLedgerDigestBytes> role_set_digest{};
  Uuid engine_instance_uuid;
  Uuid policy_snapshot_uuid;
  LedgerSettings settings;
  u64 opened_at_utc_ns = 0;
  u64 ended_at_utc_ns = 0;  // 0 = the open record
  LedgerEndReason end_reason = LedgerEndReason::none;
  bool ended() const noexcept { return ended_at_utc_ns != 0; }
  bool operator==(const LedgerSessionRecord&) const = default;
};

struct LedgerAttachmentRecord {
  Uuid session_uuid;
  Uuid connection_uuid;
  Uuid predecessor_uuid;
  Uuid engine_instance_uuid;
  u64 at_utc_ns = 0;
  bool operator==(const LedgerAttachmentRecord&) const = default;
};

struct LedgerSettingsChangeRecord {
  Uuid session_uuid;
  u64 generation = 0;
  LedgerSettings settings;
  u64 effective_from_transaction_order = 0;
  u64 at_utc_ns = 0;
  bool operator==(const LedgerSettingsChangeRecord&) const = default;
};

// The variant's index + 1 is the record's kind byte.
using LedgerBody = std::variant<LedgerConnectionRecord, LedgerSessionRecord, LedgerAttachmentRecord,
                                LedgerSettingsChangeRecord>;

struct SessionLedgerRecord {
  u64 ordinal = 0;  // set by the page / writer; the codec checks it against the slot
  Uuid database_uuid;
  Uuid record_uuid;
  LedgerBody body;
  SessionLedgerKind kind() const noexcept { return static_cast<SessionLedgerKind>(body.index() + 1); }
  bool operator==(const SessionLedgerRecord&) const = default;
};

// One distinct code per cause (owner Q193).
enum class SessionLedgerRecordError : std::uint8_t {
  none,
  invalid_framing,             // version or record size
  invalid_kind,                // kind byte not 1..4
  invalid_state,               // state byte not valid for the kind / session state mismatch
  invalid_reserved,            // a reserved or unlisted byte is not zero
  invalid_identity,            // the database or record UUID is not a v7 engine identity
  invalid_optional_identity,   // a field that may be zero is neither zero nor v7
  invalid_required_identity,   // a field that must be set is zero or not v7
  invalid_time,                // a required time is zero, or ended before opened
  invalid_enum,                // transport / address type / TLS kind / end reason / read-only byte
  invalid_peer_address,        // the length does not fit the type, or bytes beyond the length are set
  invalid_tls_evidence,        // the hash is set without a kind, or a kind without a hash
  invalid_generation,          // settings-change generation or effective order is zero
  invalid_checksum,            // the record's checksum does not match its bytes
  short_buffer,
};
const char* SessionLedgerRecordErrorName(SessionLedgerRecordError error) noexcept;

// Checks a record's values without encoding it (everything but the ordinal).
SessionLedgerRecordError ValidateSessionLedgerRecord(const SessionLedgerRecord& record) noexcept;
// Writes exactly kSessionLedgerRecordBytes; the output is untouched on a refusal.
SessionLedgerRecordError EncodeSessionLedgerRecord(const SessionLedgerRecord& record,
                                                   std::span<byte> output) noexcept;
struct SessionLedgerDecodeResult {
  SessionLedgerRecordError error = SessionLedgerRecordError::invalid_framing;
  std::optional<SessionLedgerRecord> record;
  bool ok() const noexcept { return error == SessionLedgerRecordError::none && record.has_value(); }
};
// Strict: size, version, checksum FIRST (so a torn or flipped record is
// invalid_checksum whatever else is wrong), then kind, state, reserved bytes
// and every field's rule. An all-zero image is not a record (the caller tests
// for the empty slot first).
SessionLedgerDecodeResult DecodeSessionLedgerRecord(std::span<const byte> bytes) noexcept;

}  // namespace scratchbird::storage::page
