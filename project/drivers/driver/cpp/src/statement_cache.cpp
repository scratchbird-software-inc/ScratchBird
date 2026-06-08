// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * @file statement_cache.cpp
 * @brief Statement cache implementation for ScratchBird C/C++ driver
 */

#include "scratchbird/client/pool.h"

#include <chrono>
#include <cstdio>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

struct CacheEntry {
    sb_prepared* stmt{nullptr};
    std::chrono::steady_clock::time_point last_used;
};

struct sb_statement_cache {
    size_t max_size{0};
    std::unordered_map<std::string, CacheEntry> entries;
    std::list<std::string> lru;
    std::mutex mutex;
};

namespace {

static void touch_lru(sb_statement_cache* cache, const std::string& key) {
    cache->lru.remove(key);
    cache->lru.push_front(key);
}

static void evict_if_needed(sb_statement_cache* cache) {
    while (cache->entries.size() > cache->max_size && !cache->lru.empty()) {
        const std::string key = cache->lru.back();
        cache->lru.pop_back();
        auto it = cache->entries.find(key);
        if (it != cache->entries.end()) {
            if (it->second.stmt) {
                sb_prepared_free(it->second.stmt);
            }
            cache->entries.erase(it);
        }
    }
}

} // namespace

sb_statement_cache* sb_stmt_cache_create(size_t max_size) {
    auto* cache = new sb_statement_cache();
    cache->max_size = max_size;
    return cache;
}

void sb_stmt_cache_destroy(sb_statement_cache* cache) {
    if (!cache) {
        return;
    }
    sb_stmt_cache_clear(cache);
    delete cache;
}

sb_prepared* sb_stmt_cache_get(sb_statement_cache* cache,
                               sb_connection* conn,
                               const char* sql,
                               sb_error* err) {
    if (!conn || !sql) {
        if (err) {
            err->code = SB_ERR_INVALID_PARAM;
            std::snprintf(err->message, sizeof(err->message), "Invalid connection or SQL");
        }
        return nullptr;
    }

    if (!cache || cache->max_size == 0) {
        return sb_prepare(conn, sql, err);
    }

    const std::string key(sql);
    {
        std::lock_guard<std::mutex> lock(cache->mutex);
        auto it = cache->entries.find(key);
        if (it != cache->entries.end() && it->second.stmt) {
            it->second.last_used = std::chrono::steady_clock::now();
            touch_lru(cache, key);
            return it->second.stmt;
        }
    }

    sb_prepared* prepared = sb_prepare(conn, sql, err);
    if (!prepared) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(cache->mutex);
    auto it = cache->entries.find(key);
    if (it != cache->entries.end()) {
        if (it->second.stmt) {
            sb_prepared_free(prepared);
            it->second.last_used = std::chrono::steady_clock::now();
            touch_lru(cache, key);
            return it->second.stmt;
        }
        it->second.stmt = prepared;
        it->second.last_used = std::chrono::steady_clock::now();
        touch_lru(cache, key);
        return prepared;
    }

    CacheEntry entry;
    entry.stmt = prepared;
    entry.last_used = std::chrono::steady_clock::now();
    cache->entries.emplace(key, entry);
    touch_lru(cache, key);
    evict_if_needed(cache);
    return prepared;
}

void sb_stmt_cache_invalidate(sb_statement_cache* cache, const char* sql) {
    if (!cache || !sql) {
        return;
    }
    const std::string key(sql);
    std::lock_guard<std::mutex> lock(cache->mutex);
    auto it = cache->entries.find(key);
    if (it != cache->entries.end()) {
        if (it->second.stmt) {
            sb_prepared_free(it->second.stmt);
        }
        cache->lru.remove(key);
        cache->entries.erase(it);
    }
}

void sb_stmt_cache_clear(sb_statement_cache* cache) {
    if (!cache) {
        return;
    }
    std::lock_guard<std::mutex> lock(cache->mutex);
    for (auto& kv : cache->entries) {
        if (kv.second.stmt) {
            sb_prepared_free(kv.second.stmt);
        }
    }
    cache->entries.clear();
    cache->lru.clear();
}
