// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_NATIVE_LOGIN_THROTTLE (thin-slice S4c, plan 40; owner S4a Q1 = a: PBKDF2 stays on the loop thread, so
// the rate limit sits BEFORE the loop; the Firebird login review: 4 failures, 8 s, keyed by login and by address, in
// memory, a small bounded table).
//
// An in-memory failed-login throttle for the server layer (the handoff side). It is consulted BEFORE a native login
// request reaches the database loop: a bad-password storm must not occupy the loop (one PBKDF2 is about 0.2 s of loop
// time). While a key is throttled the handoff REJECTS EARLY with the same generic LOGIN_FAILED a wrong password gets
// (it does not sleep a thread, it does not reach the loop, and it does not tell the client it was throttled; the
// throttled rejection is its own INTERNAL code for the audit trail).
//
// KEYS. The database identity is part of every key. Per failed attempt up to two entries are touched:
//   (source, name digest)  and  (source)      when a SOURCE KEY is known;
//   (name digest)                              when it is not.
// The name is never stored: only a SHA-256 over (database, presented text). The source key is a SERVER-VERIFIED
// identity of where the attempt came from (a remote address once the listener hands one down); it is empty today -
// the server layer sees only its parser peer, which is untrusted - so, with no source, only the per-name entry
// applies. (It must never be a parser-claimed value: a parser could vary it to dodge the throttle.) A caller that has
// no source does NOT get an address-alone bucket: that bucket would be global and four bad logins from anyone would
// lock every user out.
//
// CONSTANTS (Firebird's; owner questions): kMaxFailures = 4 failures within kWindow = 8 s of the LAST failure throttle
// the key until the window passes since that last failure (a rejected attempt does not extend it); kCapacity = 16
// tracked entries in all. Expired entries are dropped when touched; when the table is full the entry with the oldest
// last failure is evicted (so a storm of many distinct names cannot grow the table past 16; it can evict an
// older entry, which is the cost of a bound). A success clears the entries of its keys. No persistent state.
//
// Thread safety: one mutex; every call is short. The clock is injectable (steady) so tests need no sleeps.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::server {

class LoginThrottle {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::uint32_t kMaxFailures = 4;
  static constexpr std::chrono::seconds kWindow{8};
  static constexpr std::size_t kCapacity = 16;

  using Digest = std::array<std::uint8_t, 32>;
  using Database = std::array<std::uint8_t, 16>;

  // `source` may be empty (unavailable). `name_digest` is the digest of the presented name (never the name).
  bool IsThrottled(const Database& database, const std::string& source, const Digest& name_digest, Clock::time_point now) {
    std::lock_guard<std::mutex> guard(mutex_);
    Purge(now);
    for (const auto& key : KeysFor(database, source, name_digest)) {
      const auto* entry = Find(key);
      if (entry != nullptr && entry->failures >= kMaxFailures && now - entry->last_failure < kWindow) return true;
    }
    return false;
  }
  void RecordFailure(const Database& database, const std::string& source, const Digest& name_digest, Clock::time_point now) {
    std::lock_guard<std::mutex> guard(mutex_);
    Purge(now);
    for (const auto& key : KeysFor(database, source, name_digest)) {
      auto* entry = Find(key);
      if (entry == nullptr) {
        if (entries_.size() >= kCapacity) EvictOldest();
        entries_.push_back(Entry{key, 0, now});
        entry = &entries_.back();
      }
      ++entry->failures;
      entry->last_failure = now;
    }
  }
  void RecordSuccess(const Database& database, const std::string& source, const Digest& name_digest) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto& key : KeysFor(database, source, name_digest)) {
      for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->key == key) it = entries_.erase(it);
        else ++it;
      }
    }
  }
  std::size_t size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return entries_.size();
  }
  void Clear() {
    std::lock_guard<std::mutex> guard(mutex_);
    entries_.clear();
  }

 private:
  struct Key {
    Database database{};
    std::string source;       // empty: not a source key
    bool has_name = false;
    Digest name{};
    bool operator==(const Key& other) const {
      return database == other.database && source == other.source && has_name == other.has_name && name == other.name;
    }
  };
  struct Entry {
    Key key;
    std::uint32_t failures = 0;
    Clock::time_point last_failure;
  };
  static std::vector<Key> KeysFor(const Database& database, const std::string& source, const Digest& name_digest) {
    std::vector<Key> keys;
    if (!source.empty()) {
      keys.push_back(Key{database, source, true, name_digest});
      keys.push_back(Key{database, source, false, {}});
    } else {
      keys.push_back(Key{database, {}, true, name_digest});
    }
    return keys;
  }
  Entry* Find(const Key& key) {
    for (auto& entry : entries_)
      if (entry.key == key) return &entry;
    return nullptr;
  }
  // Expired = the window has passed since the last failure (a throttled key is free again at that moment).
  void Purge(Clock::time_point now) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& entry) { return now - entry.last_failure >= kWindow; }),
                   entries_.end());
  }
  void EvictOldest() {
    auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                   [](const Entry& a, const Entry& b) { return a.last_failure < b.last_failure; });
    if (oldest != entries_.end()) entries_.erase(oldest);
  }
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};

// The process-wide throttle (the handoff runs per connection; a per-connection table would be defeated by
// reconnecting) and its clock. The clock hook exists for tests; production leaves it empty.
inline LoginThrottle& NativeLoginThrottle() {
  static LoginThrottle throttle;
  return throttle;
}
inline std::function<LoginThrottle::Clock::time_point()>& NativeLoginThrottleClockHook() {
  static std::function<LoginThrottle::Clock::time_point()> hook;
  return hook;
}
inline LoginThrottle::Clock::time_point NativeLoginThrottleNow() {
  const auto& hook = NativeLoginThrottleClockHook();
  return hook ? hook() : LoginThrottle::Clock::now();
}

}  // namespace scratchbird::server
