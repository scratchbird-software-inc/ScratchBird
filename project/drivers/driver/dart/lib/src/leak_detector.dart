// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:async';

class LeakDetectionConfig {
  int thresholdMs;
  bool captureStackTrace;
  int checkIntervalMs;
  void Function(String connectionId, CheckoutInfo info, int heldDurationMs)?
      onLeakDetected;

  LeakDetectionConfig({
    this.thresholdMs = 30000,
    this.captureStackTrace = false,
    this.checkIntervalMs = 10000,
    this.onLeakDetected,
  });
}

class CheckoutInfo {
  final int checkoutTime = DateTime.now().millisecondsSinceEpoch;
  final String? stackTrace;
  final Map<String, String> metadata;

  CheckoutInfo(this.metadata, {required bool captureStackTrace})
      : stackTrace = captureStackTrace ? StackTrace.current.toString() : null;

  int heldDurationMs() => DateTime.now().millisecondsSinceEpoch - checkoutTime;
}

class LeakDetectionGuard {
  final LeakDetector _detector;
  final String _connectionId;
  bool _released = false;

  LeakDetectionGuard(this._detector, this._connectionId);

  void release() {
    if (_released) return;
    _released = true;
    _detector.checkin(_connectionId);
  }
}

class LeakDetector {
  final LeakDetectionConfig config;
  final Map<String, CheckoutInfo> _checkouts = {};
  Timer? _timer;

  LeakDetector([LeakDetectionConfig? config])
      : config = config ?? LeakDetectionConfig();

  void start() {
    if (_timer != null) return;
    _timer = Timer.periodic(
        Duration(milliseconds: config.checkIntervalMs), (_) => _checkLeaks());
  }

  void stop() {
    _timer?.cancel();
    _timer = null;
  }

  LeakDetectionGuard checkout(String connectionId,
      {Map<String, String> metadata = const {}}) {
    _checkouts[connectionId] = CheckoutInfo(Map<String, String>.from(metadata),
        captureStackTrace: config.captureStackTrace);
    return LeakDetectionGuard(this, connectionId);
  }

  void checkin(String connectionId) {
    _checkouts.remove(connectionId);
  }

  void _checkLeaks() {
    for (final entry in _checkouts.entries) {
      final heldDurationMs = entry.value.heldDurationMs();
      if (heldDurationMs > config.thresholdMs) {
        if (config.onLeakDetected != null) {
          config.onLeakDetected!(entry.key, entry.value, heldDurationMs);
          continue;
        }
        // ignore: avoid_print
        print(
            "POSSIBLE CONNECTION LEAK: conn=${entry.key} held=${heldDurationMs}ms");
      }
    }
  }
}
