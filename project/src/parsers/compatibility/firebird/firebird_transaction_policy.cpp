// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_transaction_policy.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <optional>
#include <utility>

namespace scratchbird::parser::firebird {
namespace {

// Firebird 5.0.4 public TPB constants. They are kept local to this parser
// family so the standalone parser does not acquire an upstream SDK dependency.
constexpr std::uint8_t kTpbVersion1 = 1;
constexpr std::uint8_t kTpbVersion3 = 3;
constexpr std::uint8_t kTpbConsistency = 1;
constexpr std::uint8_t kTpbConcurrency = 2;
constexpr std::uint8_t kTpbShared = 3;
constexpr std::uint8_t kTpbProtected = 4;
constexpr std::uint8_t kTpbExclusive = 5;
constexpr std::uint8_t kTpbWait = 6;
constexpr std::uint8_t kTpbNoWait = 7;
constexpr std::uint8_t kTpbRead = 8;
constexpr std::uint8_t kTpbWrite = 9;
constexpr std::uint8_t kTpbLockRead = 10;
constexpr std::uint8_t kTpbLockWrite = 11;
constexpr std::uint8_t kTpbVerbTime = 12;
constexpr std::uint8_t kTpbCommitTime = 13;
constexpr std::uint8_t kTpbIgnoreLimbo = 14;
constexpr std::uint8_t kTpbReadCommitted = 15;
constexpr std::uint8_t kTpbAutocommit = 16;
constexpr std::uint8_t kTpbRecVersion = 17;
constexpr std::uint8_t kTpbNoRecVersion = 18;
constexpr std::uint8_t kTpbRestartRequests = 19;
constexpr std::uint8_t kTpbNoAutoUndo = 20;
constexpr std::uint8_t kTpbLockTimeout = 21;
constexpr std::uint8_t kTpbReadConsistency = 22;
constexpr std::uint8_t kTpbAtSnapshotNumber = 23;
constexpr std::uint8_t kTpbAutoReleaseTempBlobId = 24;
constexpr std::int32_t kMaxFirebirdLockTimeoutSeconds = 32767;

FirebirdTransactionPolicyResult MakeTpbFormFailure(
    std::size_t offset,
    std::vector<std::string> symbols,
    std::vector<std::string> arguments = {}) {
  FirebirdTransactionPolicyResult result;
  result.diagnostic.scratchbird_code = "SB_DIAG_FIREBIRD_BAD_TPB_FORM";
  result.diagnostic.firebird_status_symbols = std::move(symbols);
  result.diagnostic.arguments = std::move(arguments);
  result.diagnostic.input_offset = offset;
  return result;
}

FirebirdTransactionPolicyResult MakeTpbContentFailure(
    std::size_t offset,
    std::string nested_symbol,
    std::vector<std::string> arguments = {}) {
  FirebirdTransactionPolicyResult result;
  result.diagnostic.scratchbird_code = "SB_DIAG_FIREBIRD_BAD_TPB_CONTENT";
  result.diagnostic.firebird_status_symbols = {
      "isc_bad_tpb_content", std::move(nested_symbol)};
  result.diagnostic.arguments = std::move(arguments);
  result.diagnostic.input_offset = offset;
  return result;
}

FirebirdTransactionPolicyResult MakeUnsupportedTpbFailure(
    std::size_t offset,
    std::string option) {
  FirebirdTransactionPolicyResult result;
  result.diagnostic.scratchbird_code =
      "SB_DIAG_FIREBIRD_TRANSACTION_POLICY_UNSUPPORTED";
  result.diagnostic.arguments.push_back(std::move(option));
  result.diagnostic.input_offset = offset;
  return result;
}

FirebirdTransactionPolicyResult MakeSqlFailure(
    std::size_t offset,
    std::int32_t sql_code,
    std::vector<std::string> symbols,
    std::vector<std::string> arguments = {}) {
  FirebirdTransactionPolicyResult result;
  result.diagnostic.scratchbird_code = "SB_DIAG_FIREBIRD_DSQL_TRANSACTION";
  result.diagnostic.firebird_status_symbols = std::move(symbols);
  result.diagnostic.arguments = std::move(arguments);
  result.diagnostic.sql_code = sql_code;
  result.diagnostic.input_offset = offset;
  result.policy.source = FirebirdTransactionPolicySource::kSql;
  return result;
}

std::pair<std::size_t, std::size_t> SqlLineColumn(
    std::string_view sql,
    std::size_t offset) {
  std::size_t line = 1;
  std::size_t column = 1;
  const std::size_t bound = std::min(offset, sql.size());
  for (std::size_t index = 0; index < bound; ++index) {
    if (sql[index] == '\r') {
      if (index + 1 < bound && sql[index + 1] == '\n') ++index;
      ++line;
      column = 1;
    } else if (sql[index] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
  return {line, column};
}

FirebirdTransactionPolicyResult MakeSqlSyntaxFailure(
    std::string_view sql,
    std::size_t offset,
    std::string token) {
  const auto [line, column] = SqlLineColumn(sql, offset);
  return MakeSqlFailure(offset,
                        -104,
                        {"isc_sqlerr", "isc_dsql_token_unk_err", "isc_random"},
                        {std::to_string(line),
                         std::to_string(column),
                         std::move(token)});
}

FirebirdTransactionPolicyResult MakeSqlEndFailure(
    std::string_view sql,
    std::size_t offset) {
  const auto [line, column] = SqlLineColumn(sql, offset);
  return MakeSqlFailure(offset,
                        -104,
                        {"isc_sqlerr", "isc_command_end_err2"},
                        {std::to_string(line), std::to_string(column)});
}

FirebirdTransactionPolicyResult MakeSqlDuplicateFailure(
    std::size_t offset,
    std::string clause_label) {
  return MakeSqlFailure(offset,
                        -637,
                        {"isc_sqlerr", "isc_dsql_duplicate_spec"},
                        {std::move(clause_label)});
}

FirebirdTransactionPolicyResult MakeSqlShortIntegerFailure(
    std::string_view sql,
    std::size_t offset,
    std::string /*value*/) {
  const auto [line, column] = SqlLineColumn(sql, offset);
  return MakeSqlFailure(offset,
                        -842,
                        {"isc_sqlerr", "isc_expec_short", "isc_dsql_line_col_error"},
                        {std::to_string(line), std::to_string(column)});
}

FirebirdTransactionPolicyResult MakeUnsupportedSqlFailure(
    std::size_t offset,
    std::string clause) {
  FirebirdTransactionPolicyResult result;
  result.diagnostic.scratchbird_code =
      "SB_DIAG_FIREBIRD_TRANSACTION_POLICY_UNSUPPORTED";
  result.diagnostic.arguments.push_back(std::move(clause));
  result.diagnostic.input_offset = offset;
  result.policy.source = FirebirdTransactionPolicySource::kSql;
  return result;
}

std::string_view TpbOptionName(std::uint8_t tag) {
  switch (tag) {
    case kTpbConsistency: return "isc_tpb_consistency";
    case kTpbConcurrency: return "isc_tpb_concurrency";
    case kTpbShared: return "isc_tpb_shared";
    case kTpbProtected: return "isc_tpb_protected";
    case kTpbExclusive: return "isc_tpb_exclusive";
    case kTpbWait: return "isc_tpb_wait";
    case kTpbNoWait: return "isc_tpb_nowait";
    case kTpbRead: return "isc_tpb_read";
    case kTpbWrite: return "isc_tpb_write";
    case kTpbLockRead: return "isc_tpb_lock_read";
    case kTpbLockWrite: return "isc_tpb_lock_write";
    case kTpbVerbTime: return "isc_tpb_verb_time";
    case kTpbCommitTime: return "isc_tpb_commit_time";
    case kTpbIgnoreLimbo: return "isc_tpb_ignore_limbo";
    case kTpbReadCommitted: return "isc_tpb_read_committed";
    case kTpbAutocommit: return "isc_tpb_autocommit";
    case kTpbRecVersion: return "isc_tpb_rec_version";
    case kTpbNoRecVersion: return "isc_tpb_no_rec_version";
    case kTpbRestartRequests: return "isc_tpb_restart_requests";
    case kTpbNoAutoUndo: return "isc_tpb_no_auto_undo";
    case kTpbLockTimeout: return "isc_tpb_lock_timeout";
    case kTpbReadConsistency: return "isc_tpb_read_consistency";
    case kTpbAtSnapshotNumber: return "isc_tpb_at_snapshot_number";
    case kTpbAutoReleaseTempBlobId: return "isc_tpb_auto_release_temp_blobid";
    default: return {};
  }
}

std::int32_t DecodeFirebirdSignedLittleEndian(
    std::span<const std::uint8_t> value) {
  std::uint32_t decoded = 0;
  for (std::size_t index = 0; index < value.size(); ++index) {
    decoded |= static_cast<std::uint32_t>(value[index]) << (index * 8);
  }
  if (value.size() < sizeof(std::uint32_t) &&
      (value.back() & 0x80u) != 0) {
    decoded |= std::numeric_limits<std::uint32_t>::max()
               << (value.size() * 8);
  }
  return static_cast<std::int32_t>(decoded);
}

enum class SqlTokenKind {
  kWord,
  kNumber32Bit,
  kOtherNumber,
  kSymbol,
};

struct SqlToken {
  SqlTokenKind kind{SqlTokenKind::kSymbol};
  std::string text;
  std::string upper;
  std::size_t offset{0};
  std::int32_t int32_value{0};
};

struct SqlLexResult {
  bool ok{true};
  std::vector<SqlToken> tokens;
  std::size_t failure_offset{0};
};

std::string UpperAscii(std::string_view text) {
  std::string upper(text);
  std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return upper;
}

bool IsWordStart(unsigned char ch) {
  return std::isalpha(ch) != 0 || ch == '_';
}

bool IsWordContinue(unsigned char ch) {
  return std::isalnum(ch) != 0 || ch == '_' || ch == '$';
}

std::uint8_t HexDigitValue(unsigned char ch) {
  if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
  return static_cast<std::uint8_t>(std::toupper(ch) - 'A' + 10);
}

SqlLexResult LexTransactionSql(std::string_view sql) {
  SqlLexResult result;
  std::size_t offset = 0;
  while (offset < sql.size()) {
    const unsigned char ch = static_cast<unsigned char>(sql[offset]);
    if (std::isspace(ch) != 0) {
      ++offset;
      continue;
    }
    if (ch == '-' && offset + 1 < sql.size() && sql[offset + 1] == '-') {
      offset += 2;
      while (offset < sql.size() && sql[offset] != '\n' && sql[offset] != '\r') {
        ++offset;
      }
      continue;
    }
    if (ch == '/' && offset + 1 < sql.size() && sql[offset + 1] == '*') {
      const std::size_t comment_offset = offset;
      offset += 2;
      bool closed = false;
      while (offset + 1 < sql.size()) {
        if (sql[offset] == '*' && sql[offset + 1] == '/') {
          offset += 2;
          closed = true;
          break;
        }
        ++offset;
      }
      if (!closed) {
        result.ok = false;
        result.failure_offset = comment_offset;
        return result;
      }
      continue;
    }
    if (IsWordStart(ch)) {
      const std::size_t start = offset++;
      while (offset < sql.size() &&
             IsWordContinue(static_cast<unsigned char>(sql[offset]))) {
        ++offset;
      }
      SqlToken token;
      token.kind = SqlTokenKind::kWord;
      token.text = std::string(sql.substr(start, offset - start));
      token.upper = UpperAscii(token.text);
      token.offset = start;
      result.tokens.push_back(std::move(token));
      continue;
    }
    if (ch == '0' && offset + 2 < sql.size() &&
        (sql[offset + 1] == 'x' || sql[offset + 1] == 'X') &&
        std::isxdigit(static_cast<unsigned char>(sql[offset + 2])) != 0) {
      const std::size_t start = offset;
      offset += 2;
      std::uint32_t parsed = 0;
      std::size_t digit_count = 0;
      while (offset < sql.size() &&
             std::isxdigit(static_cast<unsigned char>(sql[offset])) != 0) {
        if (digit_count < 8) {
          parsed = (parsed << 4) |
                   HexDigitValue(static_cast<unsigned char>(sql[offset]));
        }
        ++digit_count;
        ++offset;
      }
      SqlToken token;
      token.kind = digit_count <= 8 ? SqlTokenKind::kNumber32Bit
                                    : SqlTokenKind::kOtherNumber;
      token.text = std::string(sql.substr(start, offset - start));
      token.upper = token.text;
      token.offset = start;
      if (token.kind == SqlTokenKind::kNumber32Bit) {
        token.int32_value = std::bit_cast<std::int32_t>(parsed);
      }
      result.tokens.push_back(std::move(token));
      continue;
    }
    if (std::isdigit(ch) != 0) {
      const std::size_t start = offset++;
      std::uint64_t parsed = static_cast<std::uint64_t>(ch - '0');
      bool overflow = false;
      while (offset < sql.size() &&
             std::isdigit(static_cast<unsigned char>(sql[offset])) != 0) {
        const std::uint64_t digit =
            static_cast<std::uint64_t>(sql[offset] - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
          overflow = true;
        } else if (!overflow) {
          parsed = parsed * 10 + digit;
        }
        ++offset;
      }
      SqlToken token;
      token.kind = !overflow &&
                           parsed <= static_cast<std::uint64_t>(
                                         std::numeric_limits<std::int32_t>::max())
                       ? SqlTokenKind::kNumber32Bit
                       : SqlTokenKind::kOtherNumber;
      token.text = std::string(sql.substr(start, offset - start));
      token.upper = token.text;
      token.offset = start;
      if (token.kind == SqlTokenKind::kNumber32Bit) {
        token.int32_value = static_cast<std::int32_t>(parsed);
      }
      result.tokens.push_back(std::move(token));
      continue;
    }
    SqlToken token;
    token.kind = SqlTokenKind::kSymbol;
    token.text.assign(1, static_cast<char>(ch));
    token.upper = token.text;
    token.offset = offset++;
    result.tokens.push_back(std::move(token));
  }
  return result;
}

struct SqlTransactionOptions {
  bool access_specified{false};
  bool read_only{false};
  std::size_t access_offset{0};
  bool wait_specified{false};
  bool wait{true};
  std::size_t wait_offset{0};
  bool isolation_specified{false};
  FirebirdTransactionIsolation isolation{FirebirdTransactionIsolation::kConcurrency};
  FirebirdRecordVersionMode record_version_mode{
      FirebirdRecordVersionMode::kDatabaseDefault};
  std::size_t isolation_offset{0};
  bool lock_timeout_specified{false};
  std::uint16_t lock_timeout_seconds{0};
  std::size_t lock_timeout_offset{0};
};

class SqlTransactionParser {
 public:
  SqlTransactionParser(std::string_view sql, std::vector<SqlToken> tokens)
      : sql_(sql), tokens_(std::move(tokens)) {}

  FirebirdTransactionPolicyResult Parse() {
    if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
    if (Peek().upper == "START") {
      return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
    }
    if (!Match("SET")) {
      return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
    }
    if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
    if (!Match("TRANSACTION")) {
      return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
    }

    while (!AtEnd()) {
      const SqlToken& token = Peek();
      if (token.text == ";") {
        ++position_;
        if (!AtEnd()) {
          return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
        }
        break;
      } else if (token.upper == "READ") {
        const auto failure = ParseReadClause();
        if (failure.has_value()) return *failure;
      } else if (token.upper == "WAIT") {
        const auto failure = SetWaitMode(true, token.offset);
        if (failure.has_value()) return *failure;
        ++position_;
      } else if (token.upper == "NO") {
        const auto failure = ParseNoClause();
        if (failure.has_value()) return *failure;
      } else if (token.upper == "ISOLATION") {
        const std::size_t offset = token.offset;
        ++position_;
        if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
        if (!Match("LEVEL")) {
          return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
        }
        const auto failure = ParseIsolationClause(offset);
        if (failure.has_value()) return *failure;
      } else if (token.upper == "SNAPSHOT") {
        const auto failure = ParseSnapshotClause(token.offset);
        if (failure.has_value()) return *failure;
      } else if (token.upper == "LOCK") {
        const auto failure = ParseLockTimeoutClause();
        if (failure.has_value()) return *failure;
      } else if (token.upper == "RESERVING" || token.upper == "IGNORE" ||
                 token.upper == "RESTART" || token.upper == "AUTO") {
        return MakeUnsupportedSqlFailure(token.offset, token.upper);
      } else {
        return MakeSqlSyntaxFailure(sql_, token.offset, token.text);
      }
    }

    std::vector<std::uint8_t> tpb = EmitCanonicalTpb();
    auto result = ParseFirebirdTransactionTpb(tpb);
    result.policy.source = FirebirdTransactionPolicySource::kSql;
    result.canonical_tpb = std::move(tpb);
    return result;
  }

 private:
  bool AtEnd() const { return position_ >= tokens_.size(); }

  const SqlToken& Peek() const { return tokens_[position_]; }

  const SqlToken* PeekAhead(std::size_t distance) const {
    const std::size_t index = position_ + distance;
    return index < tokens_.size() ? &tokens_[index] : nullptr;
  }

  bool Match(std::string_view keyword) {
    if (AtEnd() || Peek().upper != keyword) return false;
    ++position_;
    return true;
  }

  std::optional<FirebirdTransactionPolicyResult> SetAccessMode(
      bool read_only,
      std::size_t offset) {
    if (options_.access_specified) {
      return MakeSqlDuplicateFailure(offset, "READ {ONLY | WRITE}");
    }
    options_.access_specified = true;
    options_.read_only = read_only;
    options_.access_offset = offset;
    return std::nullopt;
  }

  std::optional<FirebirdTransactionPolicyResult> SetWaitMode(
      bool wait,
      std::size_t offset) {
    if (options_.wait_specified) {
      return MakeSqlDuplicateFailure(offset, "[NO] WAIT");
    }
    options_.wait_specified = true;
    options_.wait = wait;
    options_.wait_offset = offset;
    return std::nullopt;
  }

  std::optional<FirebirdTransactionPolicyResult> SetIsolation(
      FirebirdTransactionIsolation isolation,
      FirebirdRecordVersionMode record_version_mode,
      std::size_t offset) {
    if (options_.isolation_specified) {
      return MakeSqlDuplicateFailure(offset, "ISOLATION LEVEL");
    }
    options_.isolation_specified = true;
    options_.isolation = isolation;
    options_.record_version_mode = record_version_mode;
    options_.isolation_offset = offset;
    return std::nullopt;
  }

  std::optional<FirebirdTransactionPolicyResult> ParseReadClause() {
    const std::size_t offset = Peek().offset;
    ++position_;
    if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
    if (Match("ONLY")) return SetAccessMode(true, offset);
    if (Match("WRITE")) return SetAccessMode(false, offset);
    if (Match("COMMITTED") || Match("UNCOMMITTED")) {
      FirebirdRecordVersionMode version_mode =
          FirebirdRecordVersionMode::kNoRecordVersion;
      if (!AtEnd() && Peek().upper == "VERSION") {
        ++position_;
        version_mode = FirebirdRecordVersionMode::kRecordVersion;
      } else if (!AtEnd() && Peek().upper == "NO" &&
                 PeekAhead(1) != nullptr &&
                 PeekAhead(1)->upper == "VERSION") {
        const std::size_t no_offset = Peek().offset;
        ++position_;
        if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
        if (!Match("VERSION")) {
          return MakeSqlSyntaxFailure(sql_, no_offset, "NO");
        }
      } else if (!AtEnd() && Peek().upper == "READ" &&
                 PeekAhead(1) != nullptr &&
                 PeekAhead(1)->upper == "CONSISTENCY") {
        const std::size_t read_offset = Peek().offset;
        ++position_;
        if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
        if (!Match("CONSISTENCY")) {
          return MakeSqlSyntaxFailure(sql_, read_offset, "READ");
        }
        version_mode = FirebirdRecordVersionMode::kReadConsistency;
      }
      return SetIsolation(FirebirdTransactionIsolation::kReadCommitted,
                          version_mode,
                          offset);
    }
    return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
  }

  std::optional<FirebirdTransactionPolicyResult> ParseNoClause() {
    const std::size_t offset = Peek().offset;
    ++position_;
    if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
    if (Match("WAIT")) return SetWaitMode(false, offset);
    if (Match("AUTO")) {
      if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
      if (!Match("UNDO")) {
        return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
      }
      return MakeUnsupportedSqlFailure(offset, "NO AUTO UNDO");
    }
    return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
  }

  std::optional<FirebirdTransactionPolicyResult> ParseIsolationClause(
      std::size_t offset) {
    if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
    if (Peek().upper == "SNAPSHOT") return ParseSnapshotClause(offset);
    if (Peek().upper == "READ") return ParseReadClauseWithIsolationOffset(offset);
    return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
  }

  std::optional<FirebirdTransactionPolicyResult> ParseReadClauseWithIsolationOffset(
      std::size_t isolation_offset) {
    ++position_;
    if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
    if (!Match("COMMITTED") && !Match("UNCOMMITTED")) {
      return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
    }
    FirebirdRecordVersionMode version_mode =
        FirebirdRecordVersionMode::kNoRecordVersion;
    if (!AtEnd() && Match("VERSION")) {
      version_mode = FirebirdRecordVersionMode::kRecordVersion;
    } else if (!AtEnd() && Peek().upper == "NO" &&
               PeekAhead(1) != nullptr &&
               PeekAhead(1)->upper == "VERSION") {
      ++position_;
      if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
      if (!Match("VERSION")) {
        return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
      }
    } else if (!AtEnd() && Peek().upper == "READ" &&
               PeekAhead(1) != nullptr &&
               PeekAhead(1)->upper == "CONSISTENCY") {
      ++position_;
      if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
      if (!Match("CONSISTENCY")) {
        return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
      }
      version_mode = FirebirdRecordVersionMode::kReadConsistency;
    }
    return SetIsolation(FirebirdTransactionIsolation::kReadCommitted,
                        version_mode,
                        isolation_offset);
  }

  std::optional<FirebirdTransactionPolicyResult> ParseSnapshotClause(
      std::size_t offset) {
    if (!Match("SNAPSHOT")) {
      return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
    }
    if (!AtEnd() && Peek().upper == "AT") {
      return MakeUnsupportedSqlFailure(Peek().offset, "SNAPSHOT AT NUMBER");
    }
    if (!AtEnd() && Match("TABLE")) {
      if (!AtEnd() && Peek().upper == "STABILITY") ++position_;
      return SetIsolation(FirebirdTransactionIsolation::kConsistency,
                          FirebirdRecordVersionMode::kDatabaseDefault,
                          offset);
    }
    return SetIsolation(FirebirdTransactionIsolation::kConcurrency,
                        FirebirdRecordVersionMode::kDatabaseDefault,
                        offset);
  }

  std::optional<FirebirdTransactionPolicyResult> ParseLockTimeoutClause() {
    const std::size_t offset = Peek().offset;
    ++position_;
    if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
    if (!Match("TIMEOUT")) {
      return MakeSqlSyntaxFailure(sql_, Peek().offset, Peek().text);
    }
    if (options_.lock_timeout_specified) {
      return MakeSqlDuplicateFailure(offset, "LOCK TIMEOUT");
    }
    if (AtEnd()) return MakeSqlEndFailure(sql_, sql_.size());
    const SqlToken& value = Peek();
    if (value.kind != SqlTokenKind::kNumber32Bit) {
      return MakeSqlSyntaxFailure(sql_, value.offset, value.text);
    }
    const std::int32_t parsed = value.int32_value;
    if (parsed > kMaxFirebirdLockTimeoutSeconds) {
      return MakeSqlShortIntegerFailure(sql_, value.offset, value.text);
    }
    options_.lock_timeout_specified = true;
    options_.lock_timeout_seconds = static_cast<std::uint16_t>(parsed);
    options_.lock_timeout_offset = offset;
    ++position_;
    return std::nullopt;
  }

  std::vector<std::uint8_t> EmitCanonicalTpb() const {
    std::vector<std::uint8_t> tpb;
    if (!options_.access_specified && !options_.wait_specified &&
        !options_.isolation_specified && !options_.lock_timeout_specified) {
      return tpb;
    }
    tpb.push_back(kTpbVersion1);
    if (options_.access_specified) {
      tpb.push_back(options_.read_only ? kTpbRead : kTpbWrite);
    }
    if (options_.wait_specified) {
      tpb.push_back(options_.wait ? kTpbWait : kTpbNoWait);
    }
    if (options_.isolation_specified) {
      switch (options_.isolation) {
        case FirebirdTransactionIsolation::kConcurrency:
          tpb.push_back(kTpbConcurrency);
          break;
        case FirebirdTransactionIsolation::kConsistency:
          tpb.push_back(kTpbConsistency);
          break;
        case FirebirdTransactionIsolation::kReadCommitted:
          tpb.push_back(kTpbReadCommitted);
          switch (options_.record_version_mode) {
            case FirebirdRecordVersionMode::kRecordVersion:
              tpb.push_back(kTpbRecVersion);
              break;
            case FirebirdRecordVersionMode::kNoRecordVersion:
            case FirebirdRecordVersionMode::kDatabaseDefault:
              tpb.push_back(kTpbNoRecVersion);
              break;
            case FirebirdRecordVersionMode::kReadConsistency:
              tpb.push_back(kTpbReadConsistency);
              break;
          }
          break;
      }
    }
    if (options_.lock_timeout_specified) {
      tpb.push_back(kTpbLockTimeout);
      tpb.push_back(2);
      tpb.push_back(static_cast<std::uint8_t>(options_.lock_timeout_seconds & 0xffu));
      tpb.push_back(static_cast<std::uint8_t>(options_.lock_timeout_seconds >> 8));
    }
    return tpb;
  }

  std::string_view sql_;
  std::vector<SqlToken> tokens_;
  std::size_t position_{0};
  SqlTransactionOptions options_;
};

}  // namespace

FirebirdTransactionPolicyResult ParseFirebirdTransactionTpb(
    std::span<const std::uint8_t> tpb) {
  FirebirdTransactionPolicyResult result;
  result.policy.source = FirebirdTransactionPolicySource::kTpb;
  result.canonical_tpb.assign(tpb.begin(), tpb.end());
  if (tpb.empty()) {
    result.ok = true;
    return result;
  }
  if (tpb.front() != kTpbVersion1 && tpb.front() != kTpbVersion3) {
    return MakeTpbFormFailure(
        0, {"isc_bad_tpb_form", "isc_wrotpbver"});
  }
  result.policy.tpb_version = tpb.front();

  bool isolation_assigned = false;
  bool record_version_assigned = false;
  bool read_consistency_assigned = false;
  bool wait_assigned = false;
  bool access_assigned = false;
  bool lock_timeout_assigned = false;
  std::size_t offset = 1;
  while (offset < tpb.size()) {
    const std::size_t option_offset = offset;
    const std::uint8_t option = tpb[offset++];
    switch (option) {
      case kTpbConsistency:
      case kTpbConcurrency:
      case kTpbReadCommitted: {
        if (isolation_assigned) {
          return MakeTpbContentFailure(
              option_offset, "isc_tpb_multiple_txn_isolation");
        }
        if ((option == kTpbConsistency || option == kTpbConcurrency) &&
            read_consistency_assigned) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_conflicting_options",
              {std::string(TpbOptionName(option)), "isc_tpb_read_consistency"});
        }
        isolation_assigned = true;
        result.policy.isolation_explicit = true;
        if (option == kTpbConsistency) {
          result.policy.isolation = FirebirdTransactionIsolation::kConsistency;
          if (!record_version_assigned) {
            result.policy.record_version_mode =
                FirebirdRecordVersionMode::kDatabaseDefault;
          }
        } else if (option == kTpbConcurrency) {
          result.policy.isolation = FirebirdTransactionIsolation::kConcurrency;
          if (!record_version_assigned) {
            result.policy.record_version_mode =
                FirebirdRecordVersionMode::kDatabaseDefault;
          }
        } else {
          result.policy.isolation = FirebirdTransactionIsolation::kReadCommitted;
        }
        break;
      }

      case kTpbWait:
      case kTpbNoWait: {
        const bool incoming_wait = option == kTpbWait;
        if (wait_assigned) {
          const bool current_wait =
              result.policy.wait_mode == FirebirdTransactionWaitMode::kWait;
          if (current_wait != incoming_wait) {
            return MakeTpbContentFailure(
                option_offset,
                "isc_tpb_conflicting_options",
                {std::string(TpbOptionName(option)),
                 current_wait ? "isc_tpb_wait" : "isc_tpb_nowait"});
          }
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_multiple_spec",
              {std::string(TpbOptionName(option))});
        }
        if (!incoming_wait && lock_timeout_assigned) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_conflicting_options",
              {"isc_tpb_nowait", "isc_tpb_lock_timeout"});
        }
        wait_assigned = true;
        result.policy.wait_mode_explicit = true;
        result.policy.wait_mode = incoming_wait
                                      ? FirebirdTransactionWaitMode::kWait
                                      : FirebirdTransactionWaitMode::kNoWait;
        break;
      }

      case kTpbRead:
      case kTpbWrite: {
        const bool incoming_read_only = option == kTpbRead;
        if (access_assigned) {
          if (result.policy.read_only != incoming_read_only) {
            return MakeTpbContentFailure(
                option_offset,
                "isc_tpb_conflicting_options",
                {std::string(TpbOptionName(option)),
                 result.policy.read_only ? "isc_tpb_read" : "isc_tpb_write"});
          }
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_multiple_spec",
              {std::string(TpbOptionName(option))});
        }
        access_assigned = true;
        result.policy.access_mode_explicit = true;
        result.policy.read_only = incoming_read_only;
        break;
      }

      case kTpbRecVersion:
      case kTpbNoRecVersion: {
        if (isolation_assigned &&
            result.policy.isolation != FirebirdTransactionIsolation::kReadCommitted) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_option_without_rc",
              {std::string(TpbOptionName(option))});
        }
        if (record_version_assigned) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_multiple_spec",
              {std::string(TpbOptionName(option))});
        }
        if (read_consistency_assigned) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_conflicting_options",
              {std::string(TpbOptionName(option)), "isc_tpb_read_consistency"});
        }
        record_version_assigned = true;
        result.policy.record_version_explicit = true;
        result.policy.record_version_mode =
            option == kTpbRecVersion
                ? FirebirdRecordVersionMode::kRecordVersion
                : FirebirdRecordVersionMode::kNoRecordVersion;
        break;
      }

      case kTpbReadConsistency: {
        if (isolation_assigned &&
            result.policy.isolation != FirebirdTransactionIsolation::kReadCommitted) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_option_without_rc",
              {"isc_tpb_read_consistency"});
        }
        if (read_consistency_assigned) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_multiple_spec",
              {"isc_tpb_read_consistency"});
        }
        if (record_version_assigned) {
          const char* record_option =
              result.policy.record_version_mode ==
                      FirebirdRecordVersionMode::kRecordVersion
                  ? "isc_tpb_rec_version"
                  : "isc_tpb_no_rec_version";
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_conflicting_options",
              {"isc_tpb_read_consistency", record_option});
        }
        read_consistency_assigned = true;
        result.policy.record_version_explicit = true;
        result.policy.isolation = FirebirdTransactionIsolation::kReadCommitted;
        result.policy.record_version_mode =
            FirebirdRecordVersionMode::kReadConsistency;
        break;
      }

      case kTpbLockTimeout: {
        if (wait_assigned &&
            result.policy.wait_mode == FirebirdTransactionWaitMode::kNoWait) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_conflicting_options",
              {"isc_tpb_lock_timeout", "isc_tpb_nowait"});
        }
        if (lock_timeout_assigned) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_multiple_spec",
              {"isc_tpb_lock_timeout"});
        }
        lock_timeout_assigned = true;
        if (offset >= tpb.size()) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_missing_len",
              {"isc_tpb_lock_timeout"});
        }
        const std::uint8_t length = tpb[offset++];
        if (offset >= tpb.size()) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_missing_value",
              {std::to_string(length), "isc_tpb_lock_timeout"});
        }
        if (tpb.size() - offset < length) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_corrupt_len",
              {std::to_string(length), "isc_tpb_lock_timeout"});
        }
        if (length == 0) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_null_len",
              {"isc_tpb_lock_timeout"});
        }
        if (length > sizeof(std::uint32_t)) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_overflow_len",
              {std::to_string(length), "isc_tpb_lock_timeout"});
        }
        const std::int32_t value = DecodeFirebirdSignedLittleEndian(
            tpb.subspan(offset, length));
        if (value <= 0 || value > kMaxFirebirdLockTimeoutSeconds) {
          return MakeTpbContentFailure(
              option_offset,
              "isc_tpb_invalid_value",
              {std::to_string(value), "isc_tpb_lock_timeout"});
        }
        result.policy.lock_timeout_present = true;
        result.policy.lock_timeout_seconds = static_cast<std::uint16_t>(value);
        result.policy.wait_mode = FirebirdTransactionWaitMode::kWait;
        offset += length;
        break;
      }

      case kTpbShared:
      case kTpbProtected:
      case kTpbExclusive:
      case kTpbLockRead:
      case kTpbLockWrite:
      case kTpbVerbTime:
      case kTpbCommitTime:
      case kTpbIgnoreLimbo:
      case kTpbAutocommit:
      case kTpbRestartRequests:
      case kTpbNoAutoUndo:
      case kTpbAtSnapshotNumber:
      case kTpbAutoReleaseTempBlobId:
        return MakeUnsupportedTpbFailure(
            option_offset, std::string(TpbOptionName(option)));

      default:
        return MakeTpbFormFailure(option_offset, {"isc_bad_tpb_form"});
    }
  }

  if (record_version_assigned &&
      result.policy.isolation != FirebirdTransactionIsolation::kReadCommitted) {
    const char* option =
        result.policy.record_version_mode ==
                FirebirdRecordVersionMode::kRecordVersion
            ? "isc_tpb_rec_version"
            : "isc_tpb_no_rec_version";
    return MakeTpbContentFailure(
        tpb.size(), "isc_tpb_option_without_rc", {option});
  }
  result.ok = true;
  return result;
}

FirebirdTransactionPolicyResult ParseFirebirdSetTransactionSql(
    std::string_view sql) {
  auto lexed = LexTransactionSql(sql);
  if (!lexed.ok) return MakeSqlEndFailure(sql, lexed.failure_offset);
  SqlTransactionParser parser(sql, std::move(lexed.tokens));
  return parser.Parse();
}

std::vector<NeutralTransactionSblrField> RenderNeutralTransactionSblrFields(
    const FirebirdTransactionPolicy& policy) {
  std::string isolation;
  switch (policy.isolation) {
    case FirebirdTransactionIsolation::kConcurrency:
      isolation = "snapshot";
      break;
    case FirebirdTransactionIsolation::kConsistency:
      isolation = "serializable";
      break;
    case FirebirdTransactionIsolation::kReadCommitted:
      isolation = policy.record_version_mode ==
                          FirebirdRecordVersionMode::kReadConsistency
                      ? "read_consistency"
                      : "read_committed";
      break;
  }
  std::vector<NeutralTransactionSblrField> fields = {
      {"transaction_isolation_level", std::move(isolation)},
      {"transaction_read_only", policy.read_only ? "true" : "false"},
      {"transaction_wait_mode",
       policy.wait_mode == FirebirdTransactionWaitMode::kWait ? "wait"
                                                              : "no_wait"},
  };
  if (policy.lock_timeout_present) {
    fields.push_back(
        {"transaction_lock_timeout_ms",
         std::to_string(static_cast<std::uint64_t>(policy.lock_timeout_seconds) *
                        1000u)});
  }
  return fields;
}

std::string EncodeNeutralTransactionSblrFields(
    const FirebirdTransactionPolicy& policy) {
  std::string encoded;
  for (const auto& field : RenderNeutralTransactionSblrFields(policy)) {
    encoded += field.name;
    encoded.push_back('=');
    encoded += field.value;
    encoded.push_back('\n');
  }
  return encoded;
}

}  // namespace scratchbird::parser::firebird
