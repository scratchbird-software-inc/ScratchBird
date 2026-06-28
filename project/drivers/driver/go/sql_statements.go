// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import "strings"

// SplitTopLevelStatements splits SQL into top-level statements on the active
// terminator.
//
// The splitter is quote-aware (single/double quotes) and honors the
// `SET TERM <terminator>` client directive: the
// directive changes the active terminator and is consumed — it is not emitted
// as a statement and is not counted in statement indexing. This lets procedural
// bodies (functions, procedures, triggers) contain inner `;` between
// `SET TERM ^` and the restoring `SET TERM ;^`.
//
// `--` line comments are copied verbatim to end of line: a terminator or quote
// character inside a comment never splits and never toggles quote state.
//
// With no `SET TERM` directive present, the behavior is identical to a plain
// quote-aware top-level `;` split, so existing scripts and statement indices are
// unchanged. (The chosen terminator must not appear in the bodies it wraps.)
//
// This is the canonical cross-driver chunker; it must reproduce the conformance
// fixture at tests/conformance/drivers/chunker_conformance/cases.json exactly.
func SplitTopLevelStatements(sqlText string) []string {
	statements := []string{}
	term := ";"
	var buf strings.Builder
	inSingle := false
	inDouble := false

	runes := []rune(sqlText)
	length := len(runes)

	// flush trims the current buffer; if it is a SET TERM directive it updates
	// term and is consumed, otherwise the trimmed chunk is emitted (empty chunks
	// are skipped).
	flush := func() {
		chunk := strings.TrimSpace(buf.String())
		buf.Reset()
		if chunk == "" {
			return
		}
		if newTerm, ok := chunkSetTerm(chunk); ok {
			term = newTerm
			return
		}
		statements = append(statements, chunk)
	}

	termRunes := []rune(term)
	i := 0
	for i < length {
		ch := runes[i]

		// `--` line comment: consume to end of line verbatim, without scanning
		// for the terminator or quotes inside it.
		if !inSingle && !inDouble && ch == '-' && i+1 < length && runes[i+1] == '-' {
			eol := i
			for eol < length && runes[eol] != '\n' {
				eol++
			}
			buf.WriteString(string(runes[i:eol]))
			i = eol
			continue
		}

		if ch == '\'' && !inDouble {
			inSingle = !inSingle
			buf.WriteRune(ch)
			i++
			continue
		}
		if ch == '"' && !inSingle {
			inDouble = !inDouble
			buf.WriteRune(ch)
			i++
			continue
		}

		// Re-derive the active terminator runes (term may have changed via a
		// prior flush) and test for a match at the current position.
		termRunes = []rune(term)
		if !inSingle && !inDouble && len(termRunes) > 0 && matchAt(runes, i, termRunes) {
			matchedLen := len(termRunes) // capture before flush(), which may change term
			flush()
			i += matchedLen
			continue
		}

		buf.WriteRune(ch)
		i++
	}
	flush()
	return statements
}

// matchAt reports whether want matches runes starting at index i.
func matchAt(runes []rune, i int, want []rune) bool {
	if i+len(want) > len(runes) {
		return false
	}
	for j := 0; j < len(want); j++ {
		if runes[i+j] != want[j] {
			return false
		}
	}
	return true
}

// chunkSetTerm returns the new terminator and true if chunk is a
// `SET TERM <terminator>` client directive, else ("", false).
//
// Leading full-line `--` comments and blank lines are ignored when matching, so
// a directive may be preceded by comment lines in the same chunk.
func chunkSetTerm(chunk string) (string, bool) {
	meaningful := make([]string, 0)
	for _, line := range strings.Split(chunk, "\n") {
		stripped := strings.TrimSpace(line)
		if stripped == "" || strings.HasPrefix(stripped, "--") {
			continue
		}
		meaningful = append(meaningful, stripped)
	}
	if len(meaningful) == 0 {
		return "", false
	}
	// Match (case-insensitive) `set` <ws> `term` <ws> <rest>, where <rest> is the
	// non-empty trimmed remainder used as the new terminator. Mirrors the Python
	// reference regex `^set\s+term\s+(\S.*?)\s*$`.
	joined := strings.Join(meaningful, " ")
	rest := joined
	for _, keyword := range []string{"set", "term"} {
		rest = strings.TrimLeft(rest, " \t")
		if len(rest) < len(keyword) || !strings.EqualFold(rest[:len(keyword)], keyword) {
			return "", false
		}
		rest = rest[len(keyword):]
		// A keyword must be followed by whitespace (or end of string).
		if rest != "" && rest[0] != ' ' && rest[0] != '\t' {
			return "", false
		}
	}
	rest = strings.TrimSpace(rest)
	if rest == "" {
		return "", false
	}
	return rest, true
}
