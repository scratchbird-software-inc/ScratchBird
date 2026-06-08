// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/**
 * Network Subsystem Implementation
 *
 * ScratchBird Network Layer - Phase 3.1
 *
 * Initialization and cleanup for the network subsystem.
 */

#include "scratchbird/network/network.h"

#include <atomic>
#include <mutex>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#endif

namespace scratchbird {
namespace network {

namespace {
    std::atomic<bool> g_network_initialized{false};
    std::atomic<unsigned> g_network_init_count{0};
    std::mutex g_init_mutex;
}

bool initNetwork() {
    std::lock_guard<std::mutex> lock(g_init_mutex);

    if (g_network_init_count.load() > 0) {
        g_network_init_count.fetch_add(1);
        return true;  // Already initialized
    }

#ifdef _WIN32
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        return false;
    }
#endif

    g_network_initialized.store(true);
    g_network_init_count.store(1);
    return true;
}

void cleanupNetwork() {
    std::lock_guard<std::mutex> lock(g_init_mutex);

    const unsigned count = g_network_init_count.load();
    if (count == 0) {
        return;  // Not initialized
    }
    if (count > 1) {
        g_network_init_count.store(count - 1);
        return;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    g_network_init_count.store(0);
    g_network_initialized.store(false);
}

bool isNetworkInitialized() {
    return g_network_initialized.load();
}

} // namespace network
} // namespace scratchbird
