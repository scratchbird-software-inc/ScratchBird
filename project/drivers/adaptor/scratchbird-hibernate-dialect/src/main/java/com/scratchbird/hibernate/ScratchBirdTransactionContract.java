// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.hibernate;

public final class ScratchBirdTransactionContract {
  private ScratchBirdTransactionContract() {}

  public static String begin() {
    return "BEGIN";
  }

  public static String commit() {
    return "COMMIT";
  }

  public static String rollback() {
    return "ROLLBACK";
  }

  public static String savepoint(String name) {
    return "SAVEPOINT " + normalizeSavepoint(name);
  }

  public static String rollbackToSavepoint(String name) {
    return "ROLLBACK TO SAVEPOINT " + normalizeSavepoint(name);
  }

  public static String releaseSavepoint(String name) {
    return "RELEASE SAVEPOINT " + normalizeSavepoint(name);
  }

  private static String normalizeSavepoint(String name) {
    if (name == null || name.isBlank()) {
      throw new IllegalArgumentException("savepoint name is required");
    }
    if (!name.matches("[A-Za-z_][A-Za-z0-9_]*")) {
      throw new IllegalArgumentException("savepoint name must be SQL identifier safe");
    }
    return name;
  }
}
