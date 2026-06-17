// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
//
// Canonical SET TERM- and comment-aware statement chunker shared by the C++
// driver tools (sb_isql_cpp, sb_regress_cpp). Keeping ONE implementation here
// (instead of a hand-maintained copy per tool) is what prevents the splitter
// divergence the chunker conformance fixture guards against.
//
// Behavior mirrors the Python reference
// (drivers/driver/python/src/scratchbird/sql.py::split_top_level_statements)
// and is verified against
// tests/conformance/drivers/chunker_conformance/cases.json.
#ifndef SCRATCHBIRD_SB_STATEMENT_CHUNKER_HPP
#define SCRATCHBIRD_SB_STATEMENT_CHUNKER_HPP

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace sbchunk {

inline std::string trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

inline std::string lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

inline bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

// Return the new terminator if `chunk` is a `SET TERM <terminator>` client
// directive, else "". Leading full-line `--` comments and blank lines are
// ignored when matching, so a directive may be preceded by comment lines.
inline std::string setTermDirective(const std::string& chunk) {
    std::string meaningful;
    std::istringstream lines(chunk);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string t = trim(line);
        if (t.empty() || startsWith(t, "--")) {
            continue;
        }
        if (!meaningful.empty()) {
            meaningful.push_back(' ');
        }
        meaningful += t;
    }
    if (!startsWith(lower(meaningful), "set term")) {
        return "";
    }
    return trim(meaningful.substr(8));  // text after "set term"; "" if none
}

// Split SQL into top-level statements on the active terminator.
//
// Quote-aware (single/double quotes) and `--` comment-aware. Honors the
// `SET TERM <terminator>` client directive (Firebird / sb_isql semantics): the
// directive changes the active terminator and is consumed (not emitted, not
// counted in statement indexing). With no SET TERM present, behavior is a plain
// quote-aware top-level `;` split, so existing scripts and indices are stable.
inline std::vector<std::string> splitStatements(const std::string& script) {
    std::vector<std::string> out;
    std::string current;
    std::string term = ";";
    bool single = false;
    bool dbl = false;

    const auto flush = [&]() {
        const std::string chunk = trim(current);
        if (chunk.empty()) {
            return;
        }
        const std::string newTerm = setTermDirective(chunk);
        if (!newTerm.empty()) {
            term = newTerm;
            return;
        }
        out.push_back(chunk);
    };

    std::size_t i = 0;
    const std::size_t n = script.size();
    while (i < n) {
        const char ch = script[i];
        if (!single && !dbl && ch == '-' && i + 1 < n && script[i + 1] == '-') {
            // `--` line comment: copy to end of line verbatim, without scanning
            // for the terminator or quotes inside it. ';'/terminator chars in a
            // comment never split.
            std::size_t eol = script.find('\n', i);
            if (eol == std::string::npos) {
                eol = n;
            }
            current.append(script, i, eol - i);
            i = eol;
            continue;
        }
        if (ch == '\'' && !dbl) {
            single = !single;
            current.push_back(ch);
            ++i;
            continue;
        }
        if (ch == '"' && !single) {
            dbl = !dbl;
            current.push_back(ch);
            ++i;
            continue;
        }
        if (!single && !dbl && !term.empty() &&
            script.compare(i, term.size(), term) == 0) {
            const std::size_t matchedLen = term.size();  // capture before flush() may change term
            flush();
            current.clear();
            i += matchedLen;
            continue;
        }
        current.push_back(ch);
        ++i;
    }
    flush();
    return out;
}

}  // namespace sbchunk

#endif  // SCRATCHBIRD_SB_STATEMENT_CHUNKER_HPP
