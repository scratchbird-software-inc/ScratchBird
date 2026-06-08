// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:async';

class KeepaliveConfig {
  int intervalMs;
  int maxIdleBeforeCheckMs;
  int validationTimeoutMs;

  KeepaliveConfig({
    this.intervalMs = 120000,
    this.maxIdleBeforeCheckMs = 600000,
    this.validationTimeoutMs = 5000,
  });
}

class KeepaliveTracker {
  final KeepaliveConfig config;
  int _lastActivity = DateTime.now().millisecondsSinceEpoch;

  KeepaliveTracker(this.config);

  void markActive() {
    _lastActivity = DateTime.now().millisecondsSinceEpoch;
  }

  bool needsValidation() {
    return DateTime.now().millisecondsSinceEpoch - _lastActivity > config.maxIdleBeforeCheckMs;
  }
}

typedef Pinger = Future<bool> Function();

class KeepaliveManager {
  final KeepaliveConfig config;
  final Map<String, KeepaliveTracker> _trackers = {};
  final Map<String, Pinger> _pingers = {};
  Timer? _timer;

  KeepaliveManager([KeepaliveConfig? config]) : config = config ?? KeepaliveConfig();

  void start() {
    if (_timer != null) return;
    _timer = Timer.periodic(Duration(milliseconds: config.intervalMs), (_) => _checkConnections());
  }

  void stop() {
    _timer?.cancel();
    _timer = null;
  }

  KeepaliveTracker register(String connId, Pinger pinger) {
    final tracker = KeepaliveTracker(config);
    _trackers[connId] = tracker;
    _pingers[connId] = pinger;
    return tracker;
  }

  void unregister(String connId) {
    _trackers.remove(connId);
    _pingers.remove(connId);
  }

  Future<void> _checkConnections() async {
    for (final entry in _trackers.entries) {
      if (!entry.value.needsValidation()) continue;
      final pinger = _pingers[entry.key];
      if (pinger == null) continue;
      try {
        final ok = await _pingWithTimeout(pinger);
        if (ok) {
          entry.value.markActive();
        }
      } catch (_) {}
    }
  }

  Future<bool> _pingWithTimeout(Pinger pinger) async {
    return pinger().timeout(Duration(milliseconds: config.validationTimeoutMs), onTimeout: () => false);
  }
}
