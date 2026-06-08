// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

class ScratchBirdException implements Exception {
  final String message;
  final String? sqlState;
  final int? code;

  const ScratchBirdException(
    this.message, {
    this.sqlState,
    this.code,
  });

  @override
  String toString() {
    final parts = <String>[message];
    if (sqlState != null && sqlState!.isNotEmpty) {
      parts.add('sqlState=$sqlState');
    }
    if (code != null) {
      parts.add('code=$code');
    }
    return 'ScratchBirdException(${parts.join(', ')})';
  }
}

class ScratchBirdConnectionException extends ScratchBirdException {
  const ScratchBirdConnectionException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdProtocolException extends ScratchBirdException {
  const ScratchBirdProtocolException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdAuthException extends ScratchBirdException {
  const ScratchBirdAuthException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdTransactionException extends ScratchBirdException {
  const ScratchBirdTransactionException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdExecutionException extends ScratchBirdException {
  const ScratchBirdExecutionException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdOperationalException extends ScratchBirdExecutionException {
  const ScratchBirdOperationalException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdDataException extends ScratchBirdExecutionException {
  const ScratchBirdDataException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdIntegrityException extends ScratchBirdExecutionException {
  const ScratchBirdIntegrityException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdProgrammingException extends ScratchBirdExecutionException {
  const ScratchBirdProgrammingException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdNotSupportedException extends ScratchBirdExecutionException {
  const ScratchBirdNotSupportedException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

class ScratchBirdInternalException extends ScratchBirdExecutionException {
  const ScratchBirdInternalException(
    super.message, {
    super.sqlState,
    super.code,
  });
}

enum ScratchBirdRetryScope {
  none,
  reconnect,
  statement,
  transaction,
}

ScratchBirdExecutionException mapSqlStateExecutionException(
  String message, {
  String? sqlState,
  int? code,
}) {
  final normalized = (sqlState ?? '').trim().toUpperCase();
  if (normalized.isEmpty) {
    return ScratchBirdExecutionException(message, code: code);
  }

  final sqlStateOut = normalized;
  if (normalized.length >= 2) {
    final cls = normalized.substring(0, 2);
    switch (cls) {
      case '08':
        return ScratchBirdOperationalException(
          message,
          sqlState: sqlStateOut,
          code: code,
        );
      case '22':
        return ScratchBirdDataException(
          message,
          sqlState: sqlStateOut,
          code: code,
        );
      case '23':
        return ScratchBirdIntegrityException(
          message,
          sqlState: sqlStateOut,
          code: code,
        );
      case '42':
        return ScratchBirdProgrammingException(
          message,
          sqlState: sqlStateOut,
          code: code,
        );
      case '0A':
        return ScratchBirdNotSupportedException(
          message,
          sqlState: sqlStateOut,
          code: code,
        );
      case 'XX':
        return ScratchBirdInternalException(
          message,
          sqlState: sqlStateOut,
          code: code,
        );
    }
  }

  return ScratchBirdExecutionException(
    message,
    sqlState: sqlStateOut,
    code: code,
  );
}

ScratchBirdException mapSqlStateAuthException(
  String message, {
  String? sqlState,
  int? code,
}) {
  final normalized = (sqlState ?? '').trim().toUpperCase();
  if (normalized.isEmpty) {
    return ScratchBirdAuthException(message, code: code);
  }

  if (normalized.length >= 2 && normalized.substring(0, 2) == '08') {
    return ScratchBirdConnectionException(
      message,
      sqlState: normalized,
      code: code,
    );
  }

  return ScratchBirdAuthException(
    message,
    sqlState: normalized,
    code: code,
  );
}

ScratchBirdRetryScope retryScopeForSqlState(String? sqlState) {
  // Drivers are fail-closed: fresh statement restart for 40xxx, reconnect
  // only for 08xxx, and no automatic whole-transaction replay.
  final normalized = (sqlState ?? '').trim().toUpperCase();
  if (normalized.length != 5) {
    return ScratchBirdRetryScope.none;
  }
  if (normalized == '40001' || normalized == '40P01') {
    return ScratchBirdRetryScope.statement;
  }
  if (normalized.startsWith('08')) {
    return ScratchBirdRetryScope.reconnect;
  }
  return ScratchBirdRetryScope.none;
}

bool isRetryableSqlState(String? sqlState) {
  return retryScopeForSqlState(sqlState) != ScratchBirdRetryScope.none;
}
