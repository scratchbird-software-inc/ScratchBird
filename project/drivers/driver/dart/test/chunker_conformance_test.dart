// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
//
// Cross-driver statement-chunker conformance test for the Dart driver. Loads
// the shared fixture and asserts splitStatements() reproduces `expected` for
// every `input`. Mirrors
// tests/conformance/drivers/chunker_conformance/verify_python_reference.py.

import 'dart:convert';
import 'dart:io';

import 'package:scratchbird/scratchbird.dart';
import 'package:test/test.dart';

void main() {
  // Locate the shared fixture by walking up from the current directory until
  // the conformance fixture is found, so the test is runnable from any working
  // directory (the package root, the repo root, an IDE, etc.).
  const fixtureSuffix =
      'project/tests/conformance/drivers/chunker_conformance/cases.json';
  File? fixture;
  for (var dir = Directory.current;; dir = dir.parent) {
    final candidate = File('${dir.path}/$fixtureSuffix');
    if (candidate.existsSync()) {
      fixture = candidate;
      break;
    }
    if (dir.path == dir.parent.path) {
      break; // reached filesystem root
    }
  }
  if (fixture == null) {
    fail('could not locate $fixtureSuffix from ${Directory.current.path}');
  }

  final raw = fixture.readAsStringSync();
  final cases =
      (jsonDecode(raw) as Map<String, Object?>)['cases'] as List<Object?>;

  group('chunker conformance', () {
    for (final entry in cases) {
      final case_ = entry as Map<String, Object?>;
      final name = case_['name'] as String;
      final input = case_['input'] as String;
      final expected = (case_['expected'] as List<Object?>).cast<String>();

      test(name, () {
        expect(splitStatements(input), equals(expected));
      });
    }
  });
}
