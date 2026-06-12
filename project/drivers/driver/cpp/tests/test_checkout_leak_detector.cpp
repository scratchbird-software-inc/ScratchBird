// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"
#include "scratchbird/client/leak_detector.h"

namespace {

scratchbird::sb_leak_detection_config checkoutDetectorConfig() {
    auto cfg = scratchbird::sb_leak_detection_config_default();
    cfg.threshold_ms = 250;
    cfg.check_interval_ms = 10;
    cfg.capture_stack_trace = true;
    return cfg;
}

}  // namespace

// MMCH_DRIVER_CHECKOUT_LEAK_DETECTOR
TEST(MMCH090CheckoutLeakDetectorGate, ConcurrentCheckoutCheckinChurnLeavesNoActiveState) {
    scratchbird::client::LeakDetector detector(checkoutDetectorConfig());

    constexpr int kThreadCount = 8;
    constexpr int kIterations = 200;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
        workers.emplace_back([&, thread_index]() {
            ready.fetch_add(1);
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const std::string conn_id = "conn-" + std::to_string(thread_index) +
                                            "-" + std::to_string(iteration);
                auto guard = detector.Checkout(conn_id, {
                    {"driver", "cpp"},
                    {"test", "concurrent_churn"}
                });
                ASSERT_NE(guard, nullptr);
                guard->Release();
            }
        });
    }

    while (ready.load() != kThreadCount) {
        std::this_thread::yield();
    }
    start.store(true);

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(detector.GetActiveCount(), 0u);
    EXPECT_EQ(detector.GetStats().potential_leaks, 0u);
}

TEST(MMCH090CheckoutLeakDetectorGate, DuplicateAndUnknownCheckinFailClosed) {
    scratchbird::client::LeakDetector detector(checkoutDetectorConfig());

    EXPECT_FALSE(detector.Checkin("missing-connection"));

    auto first_guard = detector.Checkout("conn-duplicate", {{"driver", "cpp"}});
    ASSERT_NE(first_guard, nullptr);
    EXPECT_EQ(detector.GetActiveCount(), 1u);

    auto replacement_guard = detector.Checkout("conn-duplicate", {{"driver", "cpp"}});
    ASSERT_NE(replacement_guard, nullptr);
    EXPECT_EQ(detector.GetActiveCount(), 1u);

    first_guard->Release();
    EXPECT_EQ(detector.GetActiveCount(), 1u)
        << "stale guard must not check in a newer checkout id";

    EXPECT_FALSE(detector.Checkin("unknown-connection"));
    EXPECT_EQ(detector.GetActiveCount(), 1u);

    EXPECT_TRUE(detector.Checkin("conn-duplicate"));
    EXPECT_EQ(detector.GetActiveCount(), 0u);
    EXPECT_FALSE(detector.Checkin("conn-duplicate"));

    auto legacy_guard_owner = detector.Checkout("conn-legacy", {{"driver", "cpp"}});
    ASSERT_NE(legacy_guard_owner, nullptr);
    scratchbird::client::LeakDetectionGuard legacy_guard(&detector, "conn-legacy");
    legacy_guard.Release();
    EXPECT_EQ(detector.GetActiveCount(), 0u);
    legacy_guard_owner->Release();
}

TEST(MMCH090CheckoutLeakDetectorGate, CApiDuplicateCheckoutPreservesNewestGuard) {
    auto cfg = checkoutDetectorConfig();
    scratchbird::sb_leak_detector* detector = scratchbird::sb_leak_detector_create(&cfg);
    ASSERT_NE(detector, nullptr);

    scratchbird::sb_leak_detector_checkout(detector, "conn-c-api");
    EXPECT_EQ(scratchbird::sb_leak_detector_get_active_count(detector), 1u);

    scratchbird::sb_leak_detector_checkout(detector, "conn-c-api");
    EXPECT_EQ(scratchbird::sb_leak_detector_get_active_count(detector), 1u)
        << "replacing the C API guard must not let the stale guard release the replacement";

    scratchbird::sb_leak_detector_checkin(detector, "unknown-c-api");
    EXPECT_EQ(scratchbird::sb_leak_detector_get_active_count(detector), 1u);

    scratchbird::sb_leak_detector_checkin(detector, "conn-c-api");
    EXPECT_EQ(scratchbird::sb_leak_detector_get_active_count(detector), 0u);
    scratchbird::sb_leak_detector_checkin(detector, "conn-c-api");
    EXPECT_EQ(scratchbird::sb_leak_detector_get_active_count(detector), 0u);

    scratchbird::sb_leak_detector_destroy(detector);
}

TEST(MMCH090CheckoutLeakDetectorGate, DetectorDestructionWithActiveGuardIsSafe) {
    std::unique_ptr<scratchbird::client::LeakDetectionGuard> guard;
    {
        scratchbird::client::LeakDetector detector(checkoutDetectorConfig());
        guard = detector.Checkout("conn-destroyed-detector", {{"driver", "cpp"}});
        ASSERT_NE(guard, nullptr);
        EXPECT_EQ(detector.GetActiveCount(), 1u);
    }

    ASSERT_NO_THROW(guard->Release());
}

TEST(MMCH090CheckoutLeakDetectorGate, StructuredDiagnosticFieldsAreCheckoutScoped) {
    auto cfg = checkoutDetectorConfig();
    cfg.threshold_ms = 0;
    scratchbird::client::LeakDetector detector(cfg);

    auto guard = detector.Checkout("conn-structured", {
        {"driver", "cpp"},
        {"owner", "mmch-090"}
    });
    ASSERT_NE(guard, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    const auto diagnostics = detector.GetActiveDiagnostics();
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_GT(diagnostics[0].checkout_id, 0u);
    EXPECT_EQ(diagnostics[0].connection_id, "conn-structured");
    EXPECT_GE(diagnostics[0].age_ms, 0u);
    EXPECT_FALSE(diagnostics[0].owner_thread.empty());
    EXPECT_EQ(diagnostics[0].metadata.at("owner"), "mmch-090");
    EXPECT_TRUE(diagnostics[0].stack_trace_status == "captured" ||
                diagnostics[0].stack_trace_status == "unsupported" ||
                diagnostics[0].stack_trace_status == "capture_failed");
    if (diagnostics[0].stack_trace_status == "captured") {
        EXPECT_FALSE(diagnostics[0].stack_trace.empty());
    }

    const auto stats = detector.GetStats();
    EXPECT_EQ(stats.active_checkouts, 1u);
    EXPECT_EQ(stats.potential_leaks, 1u);

    const auto payload = nlohmann::json::parse(detector.ExportDiagnosticsJson());
    EXPECT_EQ(payload["detector_kind"], "connection_checkout_leak_detector");
    EXPECT_EQ(payload["authority_scope"],
              "driver_checkout_leak_evidence_only_not_transaction_finality_visibility_authorization_recovery_parser_reference_or_benchmark_authority");
    EXPECT_EQ(payload["heap_leak_detection"], false);
    EXPECT_EQ(payload["active_checkouts"], 1);
    EXPECT_EQ(payload["potential_checkout_leaks"], 1);
    ASSERT_TRUE(payload["checkouts"].is_array());
    ASSERT_EQ(payload["checkouts"].size(), 1u);

    const auto& checkout = payload["checkouts"][0];
    EXPECT_GT(checkout["checkout_id"].get<uint64_t>(), 0u);
    EXPECT_EQ(checkout["connection_id"], "conn-structured");
    EXPECT_TRUE(checkout.contains("age_ms"));
    EXPECT_FALSE(checkout["owner_thread"].get<std::string>().empty());
    EXPECT_EQ(checkout["metadata"]["driver"], "cpp");
    EXPECT_EQ(checkout["metadata"]["owner"], "mmch-090");
    EXPECT_TRUE(checkout["stack_trace_status"] == "captured" ||
                checkout["stack_trace_status"] == "unsupported" ||
                checkout["stack_trace_status"] == "capture_failed");

    guard->Release();
    EXPECT_EQ(detector.GetActiveCount(), 0u);
}
