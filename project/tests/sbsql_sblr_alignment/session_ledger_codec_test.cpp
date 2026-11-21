// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_SESSION_LEDGER_RECORD, SB_SESSION_LEDGER_PAGE
// Plan 36 step T2 (owner Q165 = 1): the session ledger codec. Records: the four
// kinds, round trips (hand-built and seeded random), every refusal code, and
// the corruption rule (EVERY bit of EVERY byte of a record of each kind flipped:
// the record is refused; and every reserved, framing or enum byte set to a
// bad value under a RE-SEALED checksum is refused with its own code). Pages:
// geometry at the five page sizes (30/62/126/254/510 slots), append, the torn
// slot rule at every prefix length of a record, the full-page / link rules, and
// the one-unit edit property (an append changes exactly one 512-byte write unit).
// Growth across a page boundary is exercised at the codec level here (format
// the next page, link it, continue the ordinals); the real chain growth is the
// writer test (native_session_ledger_writer_test).
#include "hash_digest.hpp"
#include "session_ledger_page.hpp"
#include "session_ledger_record.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
using page::SessionLedgerKind;
using page::SessionLedgerRecord;
using page::SessionLedgerRecordError;
using page::TipPageError;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::Uuid;
using RE = SessionLedgerRecordError;

unsigned checks = 0;
void Check(bool ok, std::string_view why, std::source_location at = std::source_location::current()) {
  ++checks;
  if (!ok) throw std::runtime_error(std::string(why) + " (line " + std::to_string(at.line()) + ")");
}
void CheckRec(RE actual, RE expected, std::string_view why, std::source_location at = std::source_location::current()) {
  Check(actual == expected, std::string(why) + ": expected " + page::SessionLedgerRecordErrorName(expected) + ", got " +
                                page::SessionLedgerRecordErrorName(actual),
        at);
}
void CheckTip(TipPageError actual, TipPageError expected, std::string_view why,
              std::source_location at = std::source_location::current()) {
  Check(actual == expected, std::string(why) + ": expected " + page::TipPageErrorName(expected) + ", got " +
                                page::TipPageErrorName(actual),
        at);
}

Uuid V7(unsigned tag) {
  Uuid id;
  for (std::size_t i = 0; i < 16; ++i) id.bytes[i] = static_cast<byte>(0x10 + tag * 7 + i);
  id.bytes[6] = 0x70 | (id.bytes[6] & 0x0f);
  id.bytes[8] = 0x80 | (id.bytes[8] & 0x3f);
  return id;
}
Uuid V4(unsigned tag) {
  Uuid id = V7(tag);
  id.bytes[6] = 0x40 | (id.bytes[6] & 0x0f);
  return id;
}
const Uuid kDatabase = V7(1), kFilespace = V7(2);

struct Rng {
  u64 s;
  u64 Next() {
    u64 z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  u64 Below(u64 n) { return Next() % n; }
};

page::LedgerSettings Settings(unsigned tag, bool read_only = false) {
  page::LedgerSettings s;
  s.language_uuid = V7(tag);
  s.isolation_profile = static_cast<std::uint8_t>(tag % 7);
  s.read_only = read_only;
  s.lock_wait_timeout_ms = 1000 + tag;
  return s;
}
page::LedgerConnectionRecord Connection(unsigned tag) {
  page::LedgerConnectionRecord c;
  c.listener_uuid = V7(tag + 100);
  c.parser_instance_uuid = V7(tag + 200);
  c.route_uuid = V7(tag + 300);
  c.opened_at_utc_ns = 1'000'000'000ull * (tag + 1);
  c.transport = page::LedgerTransport::tcp;
  c.peer_address_type = page::LedgerPeerAddressType::ipv6;
  c.peer_address_length = 16;
  for (unsigned i = 0; i < 16; ++i) c.peer_address[i] = static_cast<byte>(0x20 + i + tag);
  c.peer_port = static_cast<u16>(5000 + tag);
  c.tls_evidence_kind = 1;
  for (unsigned i = 0; i < 32; ++i) c.tls_evidence_hash[i] = static_cast<byte>(0x40 + i + tag);
  return c;
}
page::LedgerSessionRecord Session(unsigned tag, bool ended = false) {
  page::LedgerSessionRecord s;
  s.principal_uuid = V7(tag + 400);
  s.auth_context_uuid = V7(tag + 500);
  for (unsigned i = 0; i < 32; ++i) s.role_set_digest[i] = static_cast<byte>(0x60 + i + tag);
  s.engine_instance_uuid = V7(tag + 600);
  s.policy_snapshot_uuid = V7(tag + 700);
  s.settings = Settings(tag, true);
  s.opened_at_utc_ns = 2'000'000'000ull * (tag + 1);
  // S4e Part 0 (owner 2026-11-22): the spec's session fields that fit the record (NEW).
  for (unsigned i = 0; i < 32; ++i) s.group_set_digest[i] = static_cast<byte>(0x90 + i + tag);
  if (ended) {
    s.ended_at_utc_ns = s.opened_at_utc_ns + 5;
    s.end_reason = page::LedgerEndReason::connection_lost;
  }
  return s;
}
page::LedgerAttachmentRecord Attachment(unsigned tag) {
  page::LedgerAttachmentRecord a;
  a.session_uuid = V7(tag + 800);
  a.connection_uuid = V7(tag + 900);
  a.predecessor_uuid = V7(tag + 1000);
  a.engine_instance_uuid = V7(tag + 1100);
  a.at_utc_ns = 3'000'000'000ull * (tag + 1);
  return a;
}
page::LedgerSettingsChangeRecord Change(unsigned tag) {
  page::LedgerSettingsChangeRecord c;
  c.session_uuid = V7(tag + 1200);
  c.generation = 3 + tag;
  c.settings = Settings(tag + 9);
  c.effective_from_transaction_order = 77 + tag;
  c.at_utc_ns = 4'000'000'000ull * (tag + 1);
  return c;
}
SessionLedgerRecord Rec(page::LedgerBody body, unsigned tag, u64 ordinal = 5) {
  SessionLedgerRecord r;
  r.ordinal = ordinal;
  r.database_uuid = kDatabase;
  r.record_uuid = V7(tag + 1300);
  r.body = std::move(body);
  return r;
}
std::vector<SessionLedgerRecord> OneOfEach(unsigned tag = 0) {
  return {Rec(Connection(tag), tag), Rec(Session(tag), tag + 1), Rec(Session(tag, true), tag + 1),
          Rec(Attachment(tag), tag + 2), Rec(Change(tag), tag + 3)};
}

std::array<byte, 256> Encode(const SessionLedgerRecord& r) {
  std::array<byte, 256> out{};
  CheckRec(page::EncodeSessionLedgerRecord(r, out), RE::none, "encode");
  return out;
}
// Re-seals a record image after a test changed bytes 0..247.
void Reseal(std::array<byte, 256>& image) {
  const auto d = scratchbird::core::hash::ComputeSha256Digest(image.data(), 248);
  Check(d.ok(), "sha256 for the test's re-seal");
  std::copy_n(d.digest.begin(), 8, image.begin() + 248);
}

// ---------------------------------------------------------------------------
void TestRoundTrips() {
  const auto records = OneOfEach();
  Check(records[0].kind() == SessionLedgerKind::connection && records[1].kind() == SessionLedgerKind::session &&
            records[3].kind() == SessionLedgerKind::attachment && records[4].kind() == SessionLedgerKind::settings_change,
        "kind is the variant index + 1");
  for (const auto& r : records) {
    const auto bytes = Encode(r);
    Check(bytes[0] == 1 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1, "version 1, 256 bytes, little endian");
    Check(bytes[4] == static_cast<byte>(r.kind()), "kind byte");
    const bool ended = r.kind() == SessionLedgerKind::session && std::get<page::LedgerSessionRecord>(r.body).ended();
    Check(bytes[5] == (ended ? 2 : 1), "state byte: 1 complete, 2 an ended session");
    const auto back = page::DecodeSessionLedgerRecord(bytes);
    Check(back.ok() && *back.record == r, "decode(encode(r)) == r");
    // Encoding twice gives the same bytes (no state, no clock).
    Check(Encode(*back.record) == bytes, "re-encoding is byte-identical");
  }
  // Property style: seeded random valid records of every kind round-trip.
  Rng rng{0x51ed1ed9e7ull};
  for (unsigned n = 0; n < 4000; ++n) {
    const unsigned tag = static_cast<unsigned>(rng.Below(5000));
    SessionLedgerRecord r;
    switch (rng.Below(4)) {
      case 0: {
        auto c = Connection(tag);
        c.transport = static_cast<page::LedgerTransport>(1 + rng.Below(5));
        const auto type = rng.Below(4);
        c.peer_address_type = static_cast<page::LedgerPeerAddressType>(type);
        c.peer_address = {};
        c.peer_address_length = type == 0 ? 0 : type == 1 ? 4 : type == 2 ? 16 : static_cast<std::uint8_t>(1 + rng.Below(32));
        for (unsigned i = 0; i < c.peer_address_length; ++i) c.peer_address[i] = static_cast<byte>(rng.Next());
        c.peer_port = type == 0 ? 0 : static_cast<u16>(rng.Next());
        c.route_uuid = rng.Below(2) ? Uuid{} : V7(tag + 5);
        c.tls_evidence_kind = static_cast<std::uint8_t>(rng.Below(2));
        if (!c.tls_evidence_kind) c.tls_evidence_hash = {};
        r = Rec(c, tag, rng.Below(100000));
        break;
      }
      case 1: {
        auto s = Session(tag, rng.Below(2));
        if (rng.Below(2)) s.auth_context_uuid = Uuid{};
        if (rng.Below(2)) s.policy_snapshot_uuid = Uuid{};
        if (rng.Below(2)) s.group_set_digest = {};                 // S4e Part 0: zero = none stated is valid
        if (s.ended()) s.end_reason = static_cast<page::LedgerEndReason>(1 + rng.Below(6));
        s.settings.read_only = rng.Below(2);
        r = Rec(s, tag, rng.Below(100000));
        break;
      }
      case 2: {
        auto a = Attachment(tag);
        if (rng.Below(2)) a.predecessor_uuid = Uuid{};
        r = Rec(a, tag, rng.Below(100000));
        break;
      }
      default: r = Rec(Change(tag), tag, rng.Below(100000));
    }
    std::array<byte, 256> bytes{};
    CheckRec(page::EncodeSessionLedgerRecord(r, bytes), RE::none, "random record encodes");
    const auto back = page::DecodeSessionLedgerRecord(bytes);
    Check(back.ok() && *back.record == r, "random record round-trips");
  }
  // A short output is refused and left untouched.
  std::array<byte, 255> small{};
  small.fill(0xAB);
  CheckRec(page::EncodeSessionLedgerRecord(records[0], small), RE::short_buffer, "short output");
  Check(std::all_of(small.begin(), small.end(), [](byte b) { return b == 0xAB; }), "output untouched on a refusal");
  // A bad record leaves the output untouched too.
  auto bad = records[0];
  bad.database_uuid = V4(1);
  std::array<byte, 256> out{};
  out.fill(0xCD);
  CheckRec(page::EncodeSessionLedgerRecord(bad, out), RE::invalid_identity, "v4 database uuid");
  Check(std::all_of(out.begin(), out.end(), [](byte b) { return b == 0xCD; }), "output untouched on a validation refusal");
  // Wrong sizes are framing errors; an all-zero slot is not a record.
  std::vector<byte> wrong(257);
  CheckRec(page::DecodeSessionLedgerRecord(wrong).error, RE::invalid_framing, "257 bytes");
  CheckRec(page::DecodeSessionLedgerRecord(std::span<const byte>(wrong).first(255)).error, RE::invalid_framing, "255 bytes");
  std::array<byte, 256> zero{};
  Check(!page::DecodeSessionLedgerRecord(zero).ok(), "an all-zero slot is not a record");
}

void TestValidationCodes() {
  const auto base = OneOfEach();
  const auto expect = [&](SessionLedgerRecord r, RE want, std::string_view why) {
    CheckRec(page::ValidateSessionLedgerRecord(r), want, why);
    std::array<byte, 256> out{};
    CheckRec(page::EncodeSessionLedgerRecord(r, out), want, why);
  };
  auto mod = [&](std::size_t i, auto&& fn) {
    auto r = base[i];
    fn(r);
    return r;
  };
  const auto conn = [](SessionLedgerRecord& r) -> page::LedgerConnectionRecord& { return std::get<page::LedgerConnectionRecord>(r.body); };
  const auto sess = [](SessionLedgerRecord& r) -> page::LedgerSessionRecord& { return std::get<page::LedgerSessionRecord>(r.body); };
  const auto att = [](SessionLedgerRecord& r) -> page::LedgerAttachmentRecord& { return std::get<page::LedgerAttachmentRecord>(r.body); };
  const auto chg = [](SessionLedgerRecord& r) -> page::LedgerSettingsChangeRecord& { return std::get<page::LedgerSettingsChangeRecord>(r.body); };
  // common
  expect(mod(0, [](auto& r) { r.database_uuid = Uuid{}; }), RE::invalid_identity, "nil database");
  expect(mod(0, [](auto& r) { r.record_uuid = V4(3); }), RE::invalid_identity, "v4 record uuid");
  // connection
  // S4e (owner 2026-11-22: zero when the server does not know them): OLD expectations were
  // invalid_required_identity for a nil listener and a v4 parser instance; NOW a nil listener and a nil parser instance
  // are valid, a non-zero value must still be v7 (invalid_optional_identity).
  expect(mod(0, [&](auto& r) { conn(r).listener_uuid = V4(3); }), RE::invalid_optional_identity, "v4 listener");
  expect(mod(0, [&](auto& r) { conn(r).parser_instance_uuid = V4(3); }), RE::invalid_optional_identity, "v4 parser instance");
  {
    auto unknown = base[0];
    conn(unknown).listener_uuid = Uuid{};
    conn(unknown).parser_instance_uuid = Uuid{};
    CheckRec(page::ValidateSessionLedgerRecord(unknown), RE::none, "nil listener and nil parser instance (unknown) are valid");
    std::array<byte, 256> out{};
    CheckRec(page::EncodeSessionLedgerRecord(unknown, out), RE::none, "and encode");
    const auto back = page::DecodeSessionLedgerRecord(out);
    Check(back.ok() && *back.record == unknown, "and round-trip");
  }
  expect(mod(0, [&](auto& r) { conn(r).route_uuid = V4(3); }), RE::invalid_optional_identity, "v4 route");
  expect(mod(0, [&](auto& r) { conn(r).opened_at_utc_ns = 0; }), RE::invalid_time, "no open time");
  expect(mod(0, [&](auto& r) { conn(r).transport = static_cast<page::LedgerTransport>(0); }), RE::invalid_enum, "transport 0");
  expect(mod(0, [&](auto& r) { conn(r).transport = static_cast<page::LedgerTransport>(6); }), RE::invalid_enum, "transport 6");
  expect(mod(0, [&](auto& r) { conn(r).peer_address_type = static_cast<page::LedgerPeerAddressType>(4); }), RE::invalid_enum, "type 4");
  expect(mod(0, [&](auto& r) { conn(r).peer_address_length = 15; }), RE::invalid_peer_address, "ipv6 length 15");
  expect(mod(0, [&](auto& r) { conn(r).peer_address_type = page::LedgerPeerAddressType::ipv4; }), RE::invalid_peer_address,
         "ipv4 with 16 bytes");
  expect(mod(0, [&](auto& r) { conn(r).peer_address_type = page::LedgerPeerAddressType::opaque; conn(r).peer_address_length = 0; }),
         RE::invalid_peer_address, "opaque needs 1..32");
  expect(mod(0, [&](auto& r) { conn(r).peer_address_length = 15; conn(r).peer_address_type = page::LedgerPeerAddressType::opaque; }),
         RE::invalid_peer_address, "opaque 15 with bytes beyond the length set");
  expect(mod(0, [&](auto& r) { conn(r).peer_address_type = page::LedgerPeerAddressType::none; conn(r).peer_address_length = 0; }),
         RE::invalid_peer_address, "type none with address bytes");
  expect(mod(0, [&](auto& r) {
           conn(r).peer_address_type = page::LedgerPeerAddressType::none;
           conn(r).peer_address_length = 0;
           conn(r).peer_address = {};
         }),
         RE::invalid_peer_address, "type none with a port");
  expect(mod(0, [&](auto& r) { conn(r).tls_evidence_kind = 2; }), RE::invalid_enum, "tls kind 2");
  expect(mod(0, [&](auto& r) { conn(r).tls_evidence_kind = 0; }), RE::invalid_tls_evidence, "hash without a kind");
  expect(mod(0, [&](auto& r) { conn(r).tls_evidence_hash = {}; }), RE::invalid_tls_evidence, "kind without a hash");
  // session
  expect(mod(1, [&](auto& r) { sess(r).principal_uuid = Uuid{}; }), RE::invalid_required_identity, "nil principal");
  expect(mod(1, [&](auto& r) { sess(r).engine_instance_uuid = V4(3); }), RE::invalid_required_identity, "v4 engine instance");
  expect(mod(1, [&](auto& r) { sess(r).auth_context_uuid = V4(3); }), RE::invalid_optional_identity, "v4 auth context");
  expect(mod(1, [&](auto& r) { sess(r).policy_snapshot_uuid = V4(3); }), RE::invalid_optional_identity, "v4 policy snapshot");
  expect(mod(1, [&](auto& r) { sess(r).settings.language_uuid = V4(3); }), RE::invalid_optional_identity, "v4 language");
  expect(mod(1, [&](auto& r) { sess(r).opened_at_utc_ns = 0; }), RE::invalid_time, "no open time");
  expect(mod(1, [&](auto& r) { sess(r).end_reason = page::LedgerEndReason::closed_by_client; }), RE::invalid_state,
         "a reason without an end time");
  expect(mod(2, [&](auto& r) { sess(r).ended_at_utc_ns = sess(r).opened_at_utc_ns - 1; }), RE::invalid_time, "ended before opened");
  expect(mod(2, [&](auto& r) { sess(r).end_reason = page::LedgerEndReason::none; }), RE::invalid_enum, "an end without a reason");
  // S4e: reason 6 (setup_failed) is now valid; 7 is the first invalid value (OLD: 6 was invalid).
  expect(mod(2, [&](auto& r) { sess(r).end_reason = static_cast<page::LedgerEndReason>(7); }), RE::invalid_enum, "reason 7");
  CheckRec(page::ValidateSessionLedgerRecord(mod(2, [&](auto& r) { sess(r).end_reason = page::LedgerEndReason::setup_failed; })),
           RE::none, "reason 6 (setup_failed) is valid");
  // attachment
  expect(mod(3, [&](auto& r) { att(r).session_uuid = Uuid{}; }), RE::invalid_required_identity, "nil session");
  expect(mod(3, [&](auto& r) { att(r).connection_uuid = V4(1); }), RE::invalid_required_identity, "v4 connection");
  expect(mod(3, [&](auto& r) { att(r).engine_instance_uuid = Uuid{}; }), RE::invalid_required_identity, "nil engine instance");
  expect(mod(3, [&](auto& r) { att(r).predecessor_uuid = V4(1); }), RE::invalid_optional_identity, "v4 predecessor");
  expect(mod(3, [&](auto& r) { att(r).predecessor_uuid = att(r).session_uuid; }), RE::invalid_optional_identity,
         "predecessor equal to the session");
  expect(mod(3, [&](auto& r) { att(r).at_utc_ns = 0; }), RE::invalid_time, "no time");
  // settings change
  expect(mod(4, [&](auto& r) { chg(r).session_uuid = Uuid{}; }), RE::invalid_required_identity, "nil session");
  expect(mod(4, [&](auto& r) { chg(r).generation = 0; }), RE::invalid_generation, "generation 0");
  expect(mod(4, [&](auto& r) { chg(r).effective_from_transaction_order = 0; }), RE::invalid_generation, "order 0");
  expect(mod(4, [&](auto& r) { chg(r).at_utc_ns = 0; }), RE::invalid_time, "no time");
  expect(mod(4, [&](auto& r) { chg(r).settings.language_uuid = V4(1); }), RE::invalid_optional_identity, "v4 language");
}

// The reserved / framing / enum bytes: each set to a bad value under a re-sealed
// checksum is refused, with the code the rule names.
void TestResealedRefusals() {
  for (const auto& r : OneOfEach(3)) {
    const auto good = Encode(r);
    const auto refused = [&](std::size_t at, byte value, RE want, std::string_view why) {
      auto image = good;
      image[at] = value;
      Reseal(image);
      const auto d = page::DecodeSessionLedgerRecord(image);
      Check(!d.ok(), std::string(why) + ": a re-sealed bad byte is refused");
      if (want != RE::none) CheckRec(d.error, want, why);
    };
    refused(0, 2, RE::invalid_framing, "version");
    refused(2, 1, RE::invalid_framing, "record size low");
    refused(3, 2, RE::invalid_framing, "record size high");
    refused(4, 0, RE::invalid_kind, "kind 0");
    refused(4, 5, RE::invalid_kind, "kind 5");
    refused(5, 0, RE::invalid_state, "state 0");
    refused(5, 3, RE::invalid_state, "state 3");
    refused(6, 1, RE::invalid_reserved, "byte +6");
    refused(7, 1, RE::invalid_reserved, "byte +7");
    // Everything unlisted in the kind's layout is reserved zero.
    const auto kind = good[4];
    std::vector<std::pair<std::size_t, std::size_t>> reserved;  // [from, to)
    if (kind == 1) reserved = {{110, 112}, {176, 248}};
    // S4e Part 0: session +210..+241 now hold the generations and the parser profile; only
    // +242..+247 stay reserved (OLD: {210, 248}).
    else if (kind == 2) reserved = {{242, 248}};
    else if (kind == 3) reserved = {{120, 248}};
    else reserved = {{136, 248}};
    for (const auto& [from, to] : reserved)
      for (std::size_t at = from; at < to; ++at) refused(at, 1, RE::invalid_reserved, "reserved byte");
    // A session record whose state byte disagrees with its end time.
    if (kind == 2) {
      auto other = good;
      other[5] = good[5] == 1 ? 2 : 1;
      Reseal(other);
      CheckRec(page::DecodeSessionLedgerRecord(other).error, RE::invalid_state, "state byte vs ended_at");
    }
  }
  // S4e Part 0 (NEW): the byte table of the added session field: the group-set digest at +210..+241, +242..+247 reserved.
  {
    auto s = Session(1);
    for (unsigned i = 0; i < 32; ++i) s.group_set_digest[i] = static_cast<byte>(0xA0 + i);
    const auto img = Encode(Rec(s, 1));
    for (unsigned i = 0; i < 32; ++i) Check(img[210 + i] == static_cast<byte>(0xA0 + i), "group-set digest at +210");
    for (unsigned at = 242; at < 248; ++at) Check(img[at] == 0, "+242..+247 stay zero");
    Check(std::equal(img.begin() + 80, img.begin() + 112, s.role_set_digest.begin()), "the role-set digest stays at +80");
    auto none = s;
    none.group_set_digest = {};
    const auto z = Encode(Rec(none, 1));
    Check(std::all_of(z.begin() + 210, z.begin() + 248, [](byte b) { return b == 0; }), "an unstated digest encodes as zero");
    const auto back = page::DecodeSessionLedgerRecord(z);
    Check(back.ok() && *back.record == Rec(none, 1), "a zero group digest round-trips");
  }
  // Settings-block reserved bytes (session +162..+163 and +168..+191; change +90..+91 and +96..+119).
  {
    const auto good = Encode(Rec(Session(1), 1));
    for (const std::size_t at : {std::size_t{162}, std::size_t{163}, std::size_t{168}, std::size_t{191}}) {
      auto image = good;
      image[at] = 1;
      Reseal(image);
      CheckRec(page::DecodeSessionLedgerRecord(image).error, RE::invalid_reserved, "session settings reserved byte");
    }
    auto image = good;
    image[161] = 2;  // read-only byte (+48+96+17 = 161)
    Reseal(image);
    CheckRec(page::DecodeSessionLedgerRecord(image).error, RE::invalid_enum, "read-only byte 2");
    const auto change = Encode(Rec(Change(1), 1));
    for (const std::size_t at : {std::size_t{90}, std::size_t{91}, std::size_t{96}, std::size_t{119}}) {
      auto changed = change;
      changed[at] = 1;
      Reseal(changed);
      CheckRec(page::DecodeSessionLedgerRecord(changed).error, RE::invalid_reserved, "change settings reserved byte");
    }
  }
}

// Every bit of every byte of a record of each kind, flipped: refused (the
// checksum), never accepted as a different record.
void TestEveryBitFlip() {
  unsigned flips = 0;
  for (const auto& r : OneOfEach(5)) {
    const auto good = Encode(r);
    for (std::size_t at = 0; at < 256; ++at)
      for (unsigned bit = 0; bit < 8; ++bit) {
        auto image = good;
        image[at] ^= static_cast<byte>(1u << bit);
        const auto d = page::DecodeSessionLedgerRecord(image);
        Check(!d.ok(), "a flipped bit is refused");
        CheckRec(d.error, RE::invalid_checksum, "a flipped bit fails the checksum");
        ++flips;
      }
    // A whole-byte change too (complement), at every offset.
    for (std::size_t at = 0; at < 256; ++at) {
      auto image = good;
      image[at] = static_cast<byte>(~image[at]);
      Check(!page::DecodeSessionLedgerRecord(image).ok(), "a complemented byte is refused");
    }
  }
  Check(flips == 5 * 256 * 8, "every bit of every record was flipped");
}

// ---------------------------------------------------------------------------
disk::NativeCommonPageHeader Header(u32 page_size, u64 page_number) {
  disk::NativeCommonPageHeader h;
  h.page_size_bytes = page_size;
  h.page_type = page::kTransactionStatePageType;
  h.database_uuid = kDatabase;
  h.filespace_uuid = kFilespace;
  h.page_uuid = V7(static_cast<unsigned>(60 + page_number));
  h.page_number = page_number;
  h.page_generation = 1;
  h.page_size_profile_uuid = disk::FindCanonicalFilespacePageProfileForSize(page_size)->uuid;
  return h;
}
std::vector<byte> Format(u32 page_size, u64 page_number, u64 sequence, u64 next = 0) {
  page::SessionLedgerPageFormat f;
  f.header = Header(page_size, page_number);
  f.sequence = sequence;
  f.next_page = next;
  std::vector<byte> image(page_size, 0xEE);
  CheckTip(page::FormatSessionLedgerPage(f, image), TipPageError::none, "format");
  return image;
}
SessionLedgerRecord PageRecord(u64 ordinal) {
  auto r = Rec(Attachment(static_cast<unsigned>(ordinal % 50)), static_cast<unsigned>(ordinal), ordinal);
  return r;
}
// The slot bytes a record occupies in a page image.
std::size_t SlotAt(u32 slot) { return 512 + 256 * slot; }

void TestGeometry() {
  const u32 sizes[] = {8192, 16384, 32768, 65536, 131072};
  const u32 slots[] = {30, 62, 126, 254, 510};
  for (int i = 0; i < 5; ++i) {
    Check(page::SessionLedgerSlotsPerPage(sizes[i]) == slots[i], "slots per page at each size");
    const auto at = page::LocateSessionLedgerSlot(sizes[i], u64{slots[i]} * 3 + 4);
    Check(at && at->sequence == 3 && at->slot == 4, "ordinal -> (sequence, slot)");
    Check(page::LocateSessionLedgerSlot(sizes[i], 0)->sequence == 0, "ordinal 0 is slot 0 of page 0");
    auto image = Format(sizes[i], 9, 0);
    Check(page::IsSessionLedgerPageImage(image), "the ledger magic is found");
    Check(std::equal(image.begin() + 128, image.begin() + 136, page::kSessionLedgerPageMagic.begin()), "magic at +128");
    const auto d = page::DecodeSessionLedgerPage(image);
    Check(d.ok() && d.view->used == 0 && !d.view->torn_slot && d.view->slots == slots[i] && d.view->first_ordinal == 0,
          "a fresh page decodes empty");
    // Rows 264..511 and 160..255 are zero.
    Check(std::all_of(image.begin() + 160, image.begin() + 256, [](byte b) { return b == 0; }), "family tail zero");
    Check(std::all_of(image.begin() + 264, image.begin() + 512, [](byte b) { return b == 0; }), "link tail zero");
  }
  Check(page::SessionLedgerSlotsPerPage(512) == 0 && page::SessionLedgerSlotsPerPage(8192 + 100) == 0, "bad sizes");
  // A SBTXHD01 page is not a ledger page and vice versa: told apart by the magic.
  auto image = Format(8192, 9, 0);
  image[128 + 6] = '0';  // "SBSESL01" -> "SBSESL..."? keep a magic change below
  std::fill(image.begin() + 128, image.begin() + 136, byte{'S'});
  Check(!page::IsSessionLedgerPageImage(image), "another magic is not a ledger page");
  Check(!page::DecodeSessionLedgerPage(image).ok(), "and does not decode");
}

void TestFormatRefusals() {
  page::SessionLedgerPageFormat f;
  f.header = Header(8192, 9);
  std::vector<byte> image(8192, 0xEE);
  f.header.page_type = 0x300;
  CheckTip(page::FormatSessionLedgerPage(f, image), TipPageError::invalid_header, "wrong page type");
  Check(std::all_of(image.begin(), image.end(), [](byte b) { return b == 0xEE; }), "output untouched on a refusal");
  f.header = Header(8192, 9);
  f.header.flags = 1;
  CheckTip(page::FormatSessionLedgerPage(f, image), TipPageError::invalid_header, "flags");
  f.header = Header(8192, 9);
  f.next_page = 9;
  CheckTip(page::FormatSessionLedgerPage(f, image), TipPageError::invalid_link, "a link to itself");
  f.next_page = 0;
  std::vector<byte> small(8191);
  CheckTip(page::FormatSessionLedgerPage(f, small), TipPageError::short_buffer, "short buffer");
  f.header.page_size_bytes = 8292;
  CheckTip(page::FormatSessionLedgerPage(f, image), TipPageError::invalid_page_size, "page size");
}

void TestAppendFillAndLink() {
  for (const u32 size : {8192u, 16384u, 32768u, 65536u, 131072u}) {
    const u32 slots = page::SessionLedgerSlotsPerPage(size);
    auto image = Format(size, 4, 0);
    for (u32 n = 0; n < slots; ++n) {
      const auto before = image;
      const auto edit = page::AppendSessionLedgerRecord(image, PageRecord(n));
      Check(edit.ok() && edit.changed && edit.slot == n, "append writes the next slot");
      Check(edit.unit_offset % 512 == 0 && edit.unit_offset <= SlotAt(n) && SlotAt(n) < edit.unit_offset + 512,
            "the edit's unit holds the slot");
      // Exactly one 512-byte write unit changed, and the preimage is its old bytes.
      std::size_t first = image.size(), last = 0;
      for (std::size_t i = 0; i < image.size(); ++i)
        if (image[i] != before[i]) { first = std::min(first, i); last = i; }
      Check(first >= edit.unit_offset && last < edit.unit_offset + 512, "only the edit's write unit changed");
      Check(std::equal(edit.preimage.begin(), edit.preimage.end(), before.begin() + edit.unit_offset), "preimage is the unit before");
      const auto d = page::DecodeSessionLedgerPage(image);
      Check(d.ok() && d.view->used == n + 1, "the page decodes with n+1 records");
      Check(page::SessionLedgerRecordAt(*d.view, n) && *page::SessionLedgerRecordAt(*d.view, n) == PageRecord(n),
            "the record reads back");
    }
    // Full: an append is refused, and an unfull page cannot be linked.
    CheckTip(page::AppendSessionLedgerRecord(image, PageRecord(slots)).error, TipPageError::invalid_slot, "full page");
    // Wrong ordinal and wrong database on a page with room.
    auto part = Format(size, 4, 0);
    CheckTip(page::AppendSessionLedgerRecord(part, PageRecord(1)).error, TipPageError::invalid_slot, "ordinal skips a slot");
    auto other_db = PageRecord(0);
    other_db.database_uuid = V7(9);
    CheckTip(page::AppendSessionLedgerRecord(part, other_db).error, TipPageError::invalid_slot, "another database");
    auto invalid = PageRecord(0);
    invalid.record_uuid = V4(1);
    CheckTip(page::AppendSessionLedgerRecord(part, invalid).error, TipPageError::invalid_word, "an invalid record");
    CheckTip(page::SetSessionLedgerNextPage(part, 77).error, TipPageError::invalid_link, "link only a full page");
    // The link: written once, to a page other than itself; then the page cannot take records.
    CheckTip(page::SetSessionLedgerNextPage(image, 4).error, TipPageError::invalid_link, "link to itself");
    CheckTip(page::SetSessionLedgerNextPage(image, 0).error, TipPageError::invalid_link, "link to 0");
    const auto link = page::SetSessionLedgerNextPage(image, 77);
    Check(link.ok() && link.changed && link.unit_offset == 256 - 256 % 512, "link edit in the unit holding +256");
    Check(page::SetSessionLedgerNextPage(image, 77).ok() && !page::SetSessionLedgerNextPage(image, 77).changed, "re-link is a no-op");
    CheckTip(page::SetSessionLedgerNextPage(image, 78).error, TipPageError::invalid_link, "a second, different link");
    CheckTip(page::AppendSessionLedgerRecord(image, PageRecord(slots)).error, TipPageError::invalid_slot, "no append after a link");
    const auto d = page::DecodeSessionLedgerPage(image);
    Check(d.ok() && d.view->next_page == 77 && d.view->used == slots, "a linked full page decodes");
    // The next page of the chain: sequence 1, first ordinal = slots.
    auto next = Format(size, 77, 1);
    const auto nd = page::DecodeSessionLedgerPage(next);
    Check(nd.ok() && nd.view->first_ordinal == slots && nd.view->sequence == 1, "page 2 starts at ordinal = slots");
    const auto cont = page::AppendSessionLedgerRecord(next, PageRecord(slots));
    Check(cont.ok() && cont.slot == 0, "the chain continues in slot 0 of the next page");
    CheckTip(page::AppendSessionLedgerRecord(next, PageRecord(0)).error, TipPageError::invalid_slot, "a repeated ordinal");
  }
}

// The torn-tail rule at the codec's atomic granularity: a record of 256 bytes
// is written as one write unit; a crash may leave any PREFIX of it over the
// slot's old bytes (zero, or an earlier torn attempt).
void TestTornTail() {
  const u32 size = 8192;
  auto base = Format(size, 4, 0);
  for (u32 n = 0; n < 5; ++n) CheckTip(page::AppendSessionLedgerRecord(base, PageRecord(n)).error, TipPageError::none, "seed");
  const auto next = PageRecord(5);
  std::array<byte, 256> encoded{};
  CheckRec(page::EncodeSessionLedgerRecord(next, encoded), RE::none, "encode the torn record");
  unsigned torn = 0;
  for (std::size_t prefix = 0; prefix <= 256; ++prefix) {
    auto image = base;
    std::copy_n(encoded.begin(), prefix, image.begin() + SlotAt(5));
    const auto d = page::DecodeSessionLedgerPage(image);
    Check(d.ok(), "a torn tail decodes");
    if (prefix == 256) {
      Check(d.view->used == 6 && !d.view->torn_slot, "the whole record is complete");
      continue;
    }
    if (prefix == 0 || std::all_of(encoded.begin(), encoded.begin() + prefix, [](byte b) { return b == 0; })) {
      Check(d.view->used == 5 && !d.view->torn_slot, "no bytes (or only zero bytes) is an empty slot");
      continue;
    }
    Check(d.view->used == 5 && d.view->torn_slot && *d.view->torn_slot == 5, "a torn slot is not a record and is noted");
    Check(!page::SessionLedgerRecordAt(*d.view, 5), "the torn record is never visible");
    ++torn;
    // The next append overwrites it, and the preimage the plan checks is the torn bytes.
    auto again = image;
    const auto edit = page::AppendSessionLedgerRecord(again, next);
    Check(edit.ok() && edit.slot == 5, "append over a torn slot");
    Check(std::equal(edit.preimage.begin(), edit.preimage.end(), image.begin() + edit.unit_offset), "preimage = the torn bytes");
    const auto after = page::DecodeSessionLedgerPage(again);
    Check(after.ok() && after.view->used == 6 && !after.view->torn_slot && *page::SessionLedgerRecordAt(*after.view, 5) == next,
          "the torn slot is replaced by the whole record");
    // A non-zero slot AFTER the torn one is corruption, not a tear.
    auto beyond = image;
    beyond[SlotAt(7)] = 1;
    CheckTip(page::DecodeSessionLedgerPage(beyond).error, TipPageError::invalid_slot, "non-zero after a torn slot");
  }
  Check(torn > 200, "most prefixes were non-trivially torn");
  // A complete record AFTER an empty slot is corruption.
  auto gap = base;
  std::copy(encoded.begin(), encoded.end(), gap.begin() + SlotAt(7));
  {
    auto r7 = PageRecord(7);
    std::array<byte, 256> e7{};
    CheckRec(page::EncodeSessionLedgerRecord(r7, e7), RE::none, "ordinal 7");
    std::copy(e7.begin(), e7.end(), gap.begin() + SlotAt(7));
  }
  const auto g = page::DecodeSessionLedgerPage(gap);
  CheckTip(g.error, TipPageError::invalid_slot, "a record after an empty slot");
  Check(g.slot && *g.slot == 7, "the slot at fault is named");
  // A damaged record in the middle (not the tail) is corruption: every byte of slot 2.
  for (std::size_t at = 0; at < 256; ++at) {
    auto image = base;
    image[SlotAt(2) + at] ^= 0x01;
    CheckTip(page::DecodeSessionLedgerPage(image).error, TipPageError::invalid_slot, "a damaged middle record refuses the page");
  }
  // A checksum-valid record that breaks a page rule: wrong ordinal, wrong database, bad content.
  {
    auto image = base;
    auto wrong = PageRecord(6);
    wrong.ordinal = 9;
    std::array<byte, 256> e{};
    CheckRec(page::EncodeSessionLedgerRecord(wrong, e), RE::none, "wrong ordinal encodes alone");
    std::copy(e.begin(), e.end(), image.begin() + SlotAt(5));
    const auto d = page::DecodeSessionLedgerPage(image);
    CheckTip(d.error, TipPageError::invalid_slot, "a record with the wrong ordinal");
    wrong = PageRecord(5);
    wrong.database_uuid = V7(9);
    CheckRec(page::EncodeSessionLedgerRecord(wrong, e), RE::none, "another database encodes alone");
    std::copy(e.begin(), e.end(), image.begin() + SlotAt(5));
    CheckTip(page::DecodeSessionLedgerPage(image).error, TipPageError::invalid_slot, "a record of another database");
    // sealed but invalid content (reserved byte set)
    CheckRec(page::EncodeSessionLedgerRecord(PageRecord(5), e), RE::none, "encode");
    e[130] = 1;
    Reseal(e);
    std::copy(e.begin(), e.end(), image.begin() + SlotAt(5));
    const auto content = page::DecodeSessionLedgerPage(image);
    CheckTip(content.error, TipPageError::invalid_word, "a sealed record with a bad field");
    CheckRec(content.record_error, RE::invalid_reserved, "its cause is carried");
  }
}

void TestPageStructureRefusals() {
  const u32 size = 8192;
  auto base = Format(size, 4, 0);
  CheckTip(page::DecodeSessionLedgerPage(std::span<const byte>(base).first(100)).error, TipPageError::invalid_header, "short image");
  // every byte of the family header and link area: a change is refused (nothing here is free).
  for (std::size_t at = 128; at < 512; ++at) {
    auto image = base;
    image[at] ^= 0x01;
    const auto d = page::DecodeSessionLedgerPage(image);
    // The link (+256..+263) may be any page number other than itself while the page has room?  No: a page with
    // a successor must be full, so a link on an empty page is refused too.
    Check(!d.ok(), "a changed byte in the family header or link area is refused");
  }
  // Common header: a changed byte is caught by the header's own check.
  for (std::size_t at = 0; at < 128; ++at) {
    auto image = base;
    image[at] ^= 0x01;
    Check(!page::DecodeSessionLedgerPage(image).ok(), "a changed common-header byte is refused");
  }
  // A page with a successor must be full (a link written early is refused).
  auto linked = base;
  std::copy_n(std::array<byte, 8>{5, 0, 0, 0, 0, 0, 0, 0}.begin(), 8, linked.begin() + 256);
  CheckTip(page::DecodeSessionLedgerPage(linked).error, TipPageError::invalid_link, "a link on a page with room");
  // Binding: the first page of a chain is bound to its root.
  disk::NativeCommonPageHeaderBinding binding;
  binding.filespace = {kDatabase, kFilespace, disk::FindCanonicalFilespacePageProfileForSize(size)->uuid};
  binding.page_number = 4;
  binding.page_generation = 1;
  binding.page_type = page::kTransactionStatePageType;
  Check(page::DecodeSessionLedgerPage(base, &binding).ok(), "bound to the right root");
  binding.page_number = 5;
  Check(!page::DecodeSessionLedgerPage(base, &binding).ok(), "bound to another page number");
}

// The unit-level growth logic: fill page 1, link page 2, fill it, link page 3:
// ordinals continue across pages and the chain decodes page by page.
void TestChainOfPages() {
  const u32 size = 8192, slots = 30;
  std::vector<std::vector<byte>> pages;
  u64 ordinal = 0;
  for (u64 p = 0; p < 3; ++p) {
    pages.push_back(Format(size, 10 + p, p));
    if (p) Check(page::SetSessionLedgerNextPage(pages[p - 1], 10 + p).ok(), "link the previous page");
    for (u32 s = 0; s < slots; ++s) Check(page::AppendSessionLedgerRecord(pages.back(), PageRecord(ordinal++)).ok(), "append");
  }
  u64 expect = 0;
  for (u64 p = 0; p < 3; ++p) {
    const auto d = page::DecodeSessionLedgerPage(pages[p]);
    Check(d.ok() && d.view->sequence == p && d.view->first_ordinal == p * slots && d.view->used == slots, "page decodes");
    Check(d.view->next_page == (p < 2 ? 11 + p : 0), "link as written");
    for (u32 s = 0; s < slots; ++s) Check(page::SessionLedgerRecordAt(*d.view, s)->ordinal == expect++, "ordinals run on");
  }
}

}  // namespace

int main() {
  try {
    TestRoundTrips();
    TestValidationCodes();
    TestResealedRefusals();
    TestEveryBitFlip();
    TestGeometry();
    TestFormatRefusals();
    TestAppendFillAndLink();
    TestTornTail();
    TestPageStructureRefusals();
    TestChainOfPages();
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << " after " << checks << " checks\n";
    return 1;
  }
  std::cout << "session_ledger_codec: " << checks << " checks passed\n";
  return 0;
}
